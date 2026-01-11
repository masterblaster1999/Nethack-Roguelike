#include "game_internal.hpp"

namespace {
const Trap* discoveredTrapAt(const std::vector<Trap>& traps, int x, int y) {
    for (const Trap& t : traps) {
        if (!t.discovered) continue;
        if (t.pos.x == x && t.pos.y == y) return &t;
    }
    return nullptr;
}

int autoMoveTrapPenalty(TrapKind kind) {
    switch (kind) {
        case TrapKind::TrapDoor: return 120;
        case TrapKind::RollingBoulder: return 100;
        case TrapKind::PoisonDart: return 80;
        case TrapKind::Spike: return 80;
        case TrapKind::ConfusionGas: return 60;
        case TrapKind::PoisonGas: return 75;
        case TrapKind::LetheMist: return 70;
        case TrapKind::Alarm: return 50;
        case TrapKind::Teleport: return 40;
        default: return 75;
    }
}
} // namespace


void Game::cancelAutoMove(bool silent) {
    stopAutoMove(silent);
}

void Game::stopAutoMove(bool silent) {
    if (autoMode == AutoMoveMode::None) return;

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;

    // Clear auto-explore sub-goals/state.
    autoExploreGoalIsLoot = false;
    autoExploreGoalPos = Vec2i{-1, -1};
    autoExploreGoalIsSearch = false;
    autoExploreSearchGoalPos = Vec2i{-1, -1};
    autoExploreSearchTurnsLeft = 0;
    autoExploreSearchAnnounced = false;

    if (!silent) {
        pushMsg("AUTO-MOVE: OFF.", MessageKind::System);
    }
}


bool Game::hasRangedWeaponForAmmo(AmmoKind ammo) const {
    for (const auto& it : inv) {
        const ItemDef& d = itemDef(it.kind);
        if (d.slot == EquipSlot::RangedWeapon && d.ammo == ammo) return true;
    }
    return false;
}

bool Game::autoPickupWouldPick(ItemKind k) const {
    // Chests are world-interactables; never auto-pickup.
    if (isChestKind(k)) return false;

    switch (autoPickup) {
        case AutoPickupMode::Off:
            return false;
        case AutoPickupMode::Gold:
            return k == ItemKind::Gold;
        case AutoPickupMode::All:
            return true;
        case AutoPickupMode::Smart: {
            if (k == ItemKind::Gold) return true;
            if (k == ItemKind::Key || k == ItemKind::Lockpick) return true;
            if (k == ItemKind::AmuletYendor) return true;

            // Corpses are heavy and decay; don't auto-grab them in Smart mode.
            if (isCorpseKind(k)) return false;

            // Ammo only if we have a matching ranged weapon.
            if (k == ItemKind::Arrow) return hasRangedWeaponForAmmo(AmmoKind::Arrow);
            if (k == ItemKind::Rock)  return hasRangedWeaponForAmmo(AmmoKind::Rock);

            const ItemDef& def = itemDef(k);
            if (def.consumable) return true;
            if (def.slot != EquipSlot::None) return true; // equipment
            return false;
        }
    }

    return false;
}

bool Game::autoExploreWantsLoot(ItemKind k) const {
    // Gold never stops explore (it's either auto-picked or easy to pick later).
    if (k == ItemKind::Gold) return false;

    // Corpses are intentionally treated as "noise" for auto-explore.
    if (isCorpseKind(k)) return false;

    // Only unopened chests are "interesting".
    if (k == ItemKind::Chest) return true;
    if (k == ItemKind::ChestOpen) return false;

    // If this would be picked up automatically, don't stop/retarget for it.
    if (autoPickup != AutoPickupMode::Off && autoPickupWouldPick(k)) return false;

    // Ammo can be noisy; only treat it as interesting if you have the matching weapon.
    if (k == ItemKind::Arrow) return hasRangedWeaponForAmmo(AmmoKind::Arrow);
    if (k == ItemKind::Rock)  return hasRangedWeaponForAmmo(AmmoKind::Rock);

    return true;
}

bool Game::tileHasAutoExploreLoot(Vec2i p) const {
    for (const auto& gi : ground) {
        if (gi.pos == p && autoExploreWantsLoot(gi.item.kind)) {
            return true;
        }
    }
    return false;
}

bool Game::requestAutoTravel(Vec2i goal) {
    if (isFinished()) return false;
    if (!dung.inBounds(goal.x, goal.y)) return false;

    // Close overlays so you can see the walk.
    invOpen = false;
    closeChestOverlay();
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    // Don't auto-travel into the unknown: keep it deterministic and safe.
    if (!dung.at(goal.x, goal.y).explored) {
        pushMsg("CAN'T AUTO-TRAVEL TO AN UNEXPLORED TILE.", MessageKind::System);
        return false;
    }

    if (!dung.isPassable(goal.x, goal.y)) {
        pushMsg("NO PATH (BLOCKED).", MessageKind::Warning);
        return false;
    }

    if (goal == player().pos) {
        pushMsg("YOU ARE ALREADY THERE.", MessageKind::System);
        return false;
    }

    if (const Entity* occ = entityAt(goal.x, goal.y)) {
        if (occ->id != playerId_) {
            pushMsg("DESTINATION IS OCCUPIED.", MessageKind::Warning);
            return false;
        }
    }

    stopAutoMove(true);

    bool ok = buildAutoTravelPath(goal, /*requireExplored*/true, /*allowKnownTraps*/false);
    if (!ok) {
        // Fallback: allow routes that approach known traps (the stepper will still refuse to step onto one).
        ok = buildAutoTravelPath(goal, /*requireExplored*/true, /*allowKnownTraps*/true);
        if (ok) {
            pushMsg("AUTO-TRAVEL: NO SAFE PATH (KNOWN TRAPS).", MessageKind::Warning);
        }
    }

    if (!ok) {
        pushMsg("NO PATH FOUND.", MessageKind::Warning);
        return false;
    }

    autoMode = AutoMoveMode::Travel;
    pushMsg("AUTO-TRAVEL: ON (ESC TO CANCEL).", MessageKind::System);
    return true;
}

void Game::requestAutoExplore() {
    if (isFinished()) return;

    // Toggle off if already exploring.
    if (autoMode == AutoMoveMode::Explore) {
        stopAutoMove(false);
        return;
    }

    // Close overlays.
    invOpen = false;
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    looking = false;
    msgScroll = 0;

    if (anyVisibleHostiles()) {
        pushMsg("CANNOT AUTO-EXPLORE: DANGER NEARBY.", MessageKind::Warning);
        return;
    }

    stopAutoMove(true);

    autoMode = AutoMoveMode::Explore;
    if (!buildAutoExplorePath()) {
        autoMode = AutoMoveMode::None;
        pushMsg("NOTHING LEFT TO EXPLORE.", MessageKind::System);
        return;
    }

    pushMsg("AUTO-EXPLORE: ON (ESC TO CANCEL).", MessageKind::System);
}

bool Game::stepAutoMove() {
    if (autoMode == AutoMoveMode::None) return false;

    if (isFinished()) {
        stopAutoMove(true);
        return false;
    }

    // Safety stops.
    if (anyVisibleHostiles()) {
        pushMsg("AUTO-MOVE INTERRUPTED!", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }
    // Hunger safety: if starvation is enabled and you're starving, stop auto-move so you can eat.
    if (hungerEnabled_ && hungerStateFor(hunger, hungerMax) >= 2) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE STARVING).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (player().effects.confusionTurns > 0) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE CONFUSED).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (player().effects.burnTurns > 0 || fireAt(player().pos.x, player().pos.y) > 0u) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE ON FIRE).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    // Auto-explore: optional secret-hunting pass. If we're at a chosen search spot, spend turns searching
    // before declaring the floor fully explored.
    if (autoMode == AutoMoveMode::Explore && autoExploreGoalIsSearch && player().pos == autoExploreSearchGoalPos) {
        const int W = std::max(1, dung.width);
        const int H = std::max(1, dung.height);
        const size_t expect = static_cast<size_t>(W * H);

        // Lazily size the per-tile search budget grid (not serialized; purely transient).
        if (autoExploreSearchTriedTurns.size() != expect) {
            autoExploreSearchTriedTurns.assign(expect, 0u);
        }

        constexpr int kMaxSearchTurnsPerSpot = 4;

        const Vec2i here = player().pos;
        const int idx = here.y * W + here.x;
        const int tried = (idx >= 0 && static_cast<size_t>(idx) < autoExploreSearchTriedTurns.size())
                              ? static_cast<int>(autoExploreSearchTriedTurns[static_cast<size_t>(idx)])
                              : 0;

        if (autoExploreSearchTurnsLeft <= 0) {
            const int remaining = kMaxSearchTurnsPerSpot - tried;
            if (remaining <= 0) {
                // This spot is exhausted; clear the goal and continue selecting other targets.
                autoExploreGoalIsSearch = false;
                autoExploreSearchGoalPos = Vec2i{-1, -1};
                autoExploreSearchTurnsLeft = 0;

                // Clear any stale path and build the next explore target (frontier or another search spot).
                autoPathTiles.clear();
                autoPathIndex = 0;

                if (!buildAutoExplorePath()) {
                    pushMsg("FLOOR FULLY EXPLORED.", MessageKind::System);
                    stopAutoMove(true);
                    return false;
                }
                return true;
            }

            // Announce once per "secret hunting" stretch so the player understands why we're pausing.
            if (!autoExploreSearchAnnounced) {
                pushMsg("AUTO-EXPLORE: SEARCHING FOR SECRETS...", MessageKind::System);
                autoExploreSearchAnnounced = true;
            }

            autoExploreSearchTurnsLeft = remaining;
        }

        Entity& p = playerMut();
        const int hpBefore = p.hp;
        const int poisonBefore = p.effects.poisonTurns;
        const int webBefore = p.effects.webTurns;
        const int confBefore = p.effects.confusionTurns;
        const int burnBefore = p.effects.burnTurns;

        int foundTraps = 0;
        int foundSecrets = 0;
        searchForTraps(/*verbose*/false, &foundTraps, &foundSecrets);

        if (idx >= 0 && static_cast<size_t>(idx) < autoExploreSearchTriedTurns.size()) {
            uint8_t& counter = autoExploreSearchTriedTurns[static_cast<size_t>(idx)];
            if (counter < 255) counter++;
        }

        autoExploreSearchTurnsLeft--;

        advanceAfterPlayerAction();

        // Post-action safety stops (monsters can act during the turn we just spent searching).
        if (hungerEnabled_ && hungerStateFor(hunger, hungerMax) >= 2) {
            pushMsg("AUTO-MOVE STOPPED (YOU ARE STARVING).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }
        if (p.hp < hpBefore) {
            pushMsg("AUTO-MOVE STOPPED (YOU TOOK DAMAGE).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }
        if (p.effects.poisonTurns > poisonBefore) {
            pushMsg("AUTO-MOVE STOPPED (YOU WERE POISONED).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }
        if (p.effects.webTurns > webBefore) {
            pushMsg("AUTO-MOVE STOPPED (YOU WERE WEBBED).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }
        if (p.effects.confusionTurns > confBefore) {
            pushMsg("AUTO-MOVE STOPPED (YOU WERE CONFUSED).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }
        if (p.effects.burnTurns > burnBefore) {
            pushMsg("AUTO-MOVE STOPPED (YOU CAUGHT FIRE).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }

        if ((foundTraps + foundSecrets) > 0) {
            pushMsg(formatSearchDiscoveryMessage(foundTraps, foundSecrets), MessageKind::Info, true);
        }

        // If we found a secret, the world just changed (new door/frontier). Re-plan immediately.
        if (foundSecrets > 0 || autoExploreSearchTurnsLeft <= 0) {
            autoExploreGoalIsSearch = false;
            autoExploreSearchGoalPos = Vec2i{-1, -1};
            autoExploreSearchTurnsLeft = 0;

            // Clear stale path and build the next explore target (frontier or another search spot).
            autoPathTiles.clear();
            autoPathIndex = 0;

            if (!buildAutoExplorePath()) {
                pushMsg("FLOOR FULLY EXPLORED.", MessageKind::System);
                stopAutoMove(true);
                return false;
            }
        }

        return true;
    }

    // In auto-explore mode, if we see "interesting" loot that won't be auto-picked, retarget toward it and
    // stop when we arrive. This is less jarring than stopping immediately on sight.
    if (autoMode == AutoMoveMode::Explore) {
        const Vec2i here = player().pos;

        Vec2i bestPos{-1, -1};
        int bestPri = 999;
        int bestDist = 999999;

        for (const auto& gi : ground) {
            if (!dung.inBounds(gi.pos.x, gi.pos.y)) continue;
            if (!dung.at(gi.pos.x, gi.pos.y).visible) continue;

            const ItemKind k = gi.item.kind;
            if (!autoExploreWantsLoot(k)) continue;

            const int pri = (k == ItemKind::Chest) ? 0 : 1;
            const int dist = std::abs(gi.pos.x - here.x) + std::abs(gi.pos.y - here.y);

            if (pri < bestPri || (pri == bestPri && dist < bestDist)) {
                bestPri = pri;
                bestDist = dist;
                bestPos = gi.pos;
            }
        }

        if (bestPos.x >= 0) {
            // If we're already standing on it, stop immediately.
            if (bestPos == here) {
                pushMsg((bestPri == 0) ? "AUTO-EXPLORE STOPPED (CHEST HERE)." : "AUTO-EXPLORE STOPPED (LOOT HERE).",
                        MessageKind::System);
                stopAutoMove(true);
                return false;
            }

            // If we aren't already headed there, retarget.
            if (!autoExploreGoalIsLoot || autoExploreGoalPos != bestPos) {
                if (!buildAutoTravelPath(bestPos, /*requireExplored*/true, /*allowKnownTraps*/false)) {
                    pushMsg("AUTO-EXPLORE STOPPED (NO PATH TO LOOT).", MessageKind::System);
                    stopAutoMove(true);
                    return false;
                }

                autoExploreGoalIsLoot = true;
                autoExploreGoalPos = bestPos;

                // Cancel any secret-search sub-goal when we decide to go pick up loot.
                autoExploreGoalIsSearch = false;
                autoExploreSearchGoalPos = Vec2i{-1, -1};
                autoExploreSearchTurnsLeft = 0;

                pushMsg((bestPri == 0) ? "AUTO-EXPLORE: TARGETING CHEST." : "AUTO-EXPLORE: TARGETING LOOT.",
                        MessageKind::System);
            }
        }
    }


    // If we're out of path, rebuild (explore) or finish (travel).
    if (autoPathIndex >= autoPathTiles.size()) {
        if (autoMode == AutoMoveMode::Travel) {
            pushMsg("AUTO-TRAVEL COMPLETE.", MessageKind::System);
            stopAutoMove(true);
            return false;
        }

        // Explore: find the next frontier.
        if (!buildAutoExplorePath()) {
            pushMsg("FLOOR FULLY EXPLORED.", MessageKind::System);
            stopAutoMove(true);
            return false;
        }
    }

    if (autoPathIndex >= autoPathTiles.size()) return false;

    Entity& p = playerMut();
    const Vec2i next = autoPathTiles[autoPathIndex];

    // Sanity: we expect a 4-neighbor path.
    if (!isAdjacent8(p.pos, next)) {
        // The world changed (door opened, trap teleported you, etc). Rebuild if exploring, otherwise stop.
        if (autoMode == AutoMoveMode::Explore) {
            if (!buildAutoExplorePath()) {
                pushMsg("AUTO-EXPLORE STOPPED.", MessageKind::System);
                stopAutoMove(true);
                return false;
            }
            return true;
        }
        pushMsg("AUTO-TRAVEL STOPPED (PATH INVALID).", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    // If a monster blocks the next tile, stop and let the player decide.
    if (const Entity* occ = entityAt(next.x, next.y)) {
        if (occ->id != playerId_) {
            pushMsg("AUTO-MOVE STOPPED (MONSTER BLOCKING).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }
    }

    if (discoveredTrapAt(trapsCur, next.x, next.y)) {
        const char* msg = (autoMode == AutoMoveMode::Travel)
            ? "AUTO-TRAVEL STOPPED (KNOWN TRAP AHEAD)."
            : (autoMode == AutoMoveMode::Explore)
                ? "AUTO-EXPLORE STOPPED (KNOWN TRAP AHEAD)."
                : "AUTO-MOVE STOPPED (KNOWN TRAP AHEAD).";
        pushMsg(msg, MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    const int dx = next.x - p.pos.x;
    const int dy = next.y - p.pos.y;

    const int hpBefore = p.hp;
    const int poisonBefore = p.effects.poisonTurns;
    const int webBefore = p.effects.webTurns;
    const int confBefore = p.effects.confusionTurns;
    const int burnBefore = p.effects.burnTurns;
    const Vec2i posBefore = p.pos;

    const bool acted = tryMove(p, dx, dy);
    if (!acted) {
        pushMsg("AUTO-MOVE STOPPED (BLOCKED).", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    // If we moved onto the intended next tile, advance. If we opened a door, the position won't change,
    // so we'll try again on the next auto-step.
    if (p.pos == next) {
        autoPathIndex++;
    } else if (p.pos != posBefore) {
        // We moved, but not where we expected (shouldn't happen in 4-neighbor movement).
        pushMsg("AUTO-MOVE STOPPED (DESYNC).", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    advanceAfterPlayerAction();

    if (hungerEnabled_ && hungerStateFor(hunger, hungerMax) >= 2) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE STARVING).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (p.hp < hpBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU TOOK DAMAGE).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (p.effects.poisonTurns > poisonBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU WERE POISONED).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (p.effects.webTurns > webBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU WERE WEBBED).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (p.effects.confusionTurns > confBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU WERE CONFUSED).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (p.effects.burnTurns > burnBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU CAUGHT FIRE).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    // If we were auto-exploring toward loot, stop once we arrive (so the player can decide what to do).
    if (autoMode == AutoMoveMode::Explore && autoExploreGoalIsLoot && p.pos == autoExploreGoalPos) {
        if (tileHasAutoExploreLoot(p.pos)) {
            const bool chestHere = std::any_of(ground.begin(), ground.end(), [&](const GroundItem& gi) {
                return gi.pos == p.pos && gi.item.kind == ItemKind::Chest;
            });
            pushMsg(chestHere ? "AUTO-EXPLORE STOPPED (CHEST REACHED)." : "AUTO-EXPLORE STOPPED (LOOT REACHED).",
                    MessageKind::System);
            stopAutoMove(true);
            return false;
        }
        autoExploreGoalIsLoot = false;
        autoExploreGoalPos = Vec2i{-1, -1};
    }

    // If travel completed after this step, finish.
    if (autoMode == AutoMoveMode::Travel && autoPathIndex >= autoPathTiles.size()) {
        pushMsg("AUTO-TRAVEL COMPLETE.", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    return true;
}

bool Game::buildAutoTravelPath(Vec2i goal, bool requireExplored, bool allowKnownTraps) {
    autoPathTiles = findPathBfs(player().pos, goal, requireExplored, allowKnownTraps);
    if (autoPathTiles.empty()) return false;

    // Remove start tile so the vector becomes a list of "next tiles to step into".
    if (!autoPathTiles.empty() && autoPathTiles.front() == player().pos) {
        autoPathTiles.erase(autoPathTiles.begin());
    }

    autoPathIndex = 0;
    autoStepTimer = 0.0f;

    return !autoPathTiles.empty();
}

bool Game::buildAutoExplorePath() {
    // Auto-explore normally aims for the nearest frontier (unexplored adjacency).
    // Loot handling is done opportunistically in stepAutoMove() when it becomes visible.
    autoExploreGoalIsLoot = false;
    autoExploreGoalPos = Vec2i{-1, -1};

    // Clear any stale search goal when replanning.
    autoExploreGoalIsSearch = false;
    autoExploreSearchGoalPos = Vec2i{-1, -1};
    autoExploreSearchTurnsLeft = 0;

    Vec2i goal = findNearestExploreFrontier();
    if (goal.x >= 0 && goal.y >= 0) {
        // We have something "normal" to do again; reset the secret-hunt announcement.
        autoExploreSearchAnnounced = false;
        return buildAutoTravelPath(goal, /*requireExplored*/true, /*allowKnownTraps*/false);
    }

    // Optional: when the floor appears fully explored, walk to dead-ends/corridor corners and spend a few
    // turns searching for secret doors before giving up.
    if (!autoExploreSearchEnabled_) return false;

    Vec2i searchGoal = findNearestExploreSearchSpot();
    if (searchGoal.x < 0 || searchGoal.y < 0) return false;

    autoExploreGoalIsSearch = true;
    autoExploreSearchGoalPos = searchGoal;
    autoExploreSearchTurnsLeft = 0; // initialized when we arrive

    if (searchGoal == player().pos) {
        // We are already standing on a candidate search tile; no travel path required.
        autoPathTiles.clear();
        autoPathIndex = 0;
        autoStepTimer = 0.0f;
        return true;
    }

    return buildAutoTravelPath(searchGoal, /*requireExplored*/true, /*allowKnownTraps*/false);
}

Vec2i Game::findNearestExploreFrontier() const {
    // A "frontier" is any explored, passable tile that borders at least one unexplored tile.
    //
    // We try to find a frontier reachable without stepping on known traps first. If none exists,
    // we do a second search that *allows* traversing known traps, and return the first known trap
    // on the shortest path to the nearest frontier. This lets auto-explore guide the player to the
    // blocking trap instead of incorrectly claiming the floor is fully explored.

    const Vec2i start = player().pos;
    const bool canUnlockDoors = (keyCount() > 0) || (lockpickCount() > 0);
    const int W = std::max(1, dung.width);
    const int H = std::max(1, dung.height);

    auto idxOf = [W](int x, int y) -> int { return x + y * W; };
    auto isKnownTrap = [&](int x, int y) -> bool { return discoveredTrapAt(trapsCur, x, y) != nullptr; };

    const int dirs[8][2] = {
        { 1, 0 },
        { -1, 0 },
        { 0, 1 },
        { 0, -1 },
        { 1, 1 },
        { 1, -1 },
        { -1, 1 },
        { -1, -1 },
    };

    auto isFrontier = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (!dung.at(x, y).explored) return false;
        if (!dung.isPassable(x, y)) return false;
        if (fireAt(x, y) > 0u) return false;

        for (int dir = 0; dir < 8; ++dir) {
            const int nx = x + dirs[dir][0];
            const int ny = y + dirs[dir][1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.at(nx, ny).explored) return true;
        }
        return false;
    };

    auto passableForSearch = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (!dung.at(x, y).explored) return false;
        if (fireAt(x, y) > 0u) return false;

        const TileType tt = dung.at(x, y).type;
        const bool passable = dung.isPassable(x, y) || (canUnlockDoors && tt == TileType::DoorLocked);
        if (!passable) return false;

        if (const Entity* occ = entityAt(x, y)) {
            if (occ->id != playerId() && !occ->friendly) return false;
        }
        return true;
    };

    // Pass 1: BFS that does NOT traverse known traps (but can still return a trap tile if it's a frontier).
    {
        std::deque<Vec2i> q;
        std::vector<uint8_t> visited(static_cast<size_t>(W * H), 0);
        visited[idxOf(start.x, start.y)] = 1;
        q.push_back(start);

        while (!q.empty()) {
            const Vec2i cur = q.front();
            q.pop_front();

            if (cur != start && isFrontier(cur.x, cur.y)) return cur;

            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cur.x + dirs[dir][0];
                const int ny = cur.y + dirs[dir][1];
                if (!dung.inBounds(nx, ny)) continue;
                const int nIdx = idxOf(nx, ny);
                if (visited[nIdx]) continue;
                if (!passableForSearch(nx, ny)) continue;

                // We don't traverse known traps in pass 1, but if a known trap tile is itself a frontier,
                // we return it as the goal (auto-explore will stop before stepping onto it).
                if (isKnownTrap(nx, ny)) {
                    if (Vec2i{nx, ny} != start && isFrontier(nx, ny)) return Vec2i{nx, ny};
                    continue;
                }

                visited[nIdx] = 1;
                q.push_back(Vec2i{nx, ny});
            }
        }
    }

    // Pass 2: BFS that allows traversing known traps. If we find any frontier, we return the FIRST
    // known trap tile along the shortest path to it (so the player can deal with the blocker).
    {
        std::deque<Vec2i> q;
        std::vector<uint8_t> visited(static_cast<size_t>(W * H), 0);
        std::vector<int> firstTrapIdx(static_cast<size_t>(W * H), -1);
        visited[idxOf(start.x, start.y)] = 1;
        q.push_back(start);

        while (!q.empty()) {
            const Vec2i cur = q.front();
            q.pop_front();

            if (cur != start && isFrontier(cur.x, cur.y)) {
                const int ft = firstTrapIdx[idxOf(cur.x, cur.y)];
                if (ft != -1) return Vec2i{ft % W, ft / W};
                return cur;
            }

            for (int dir = 0; dir < 8; ++dir) {
                const int nx = cur.x + dirs[dir][0];
                const int ny = cur.y + dirs[dir][1];
                if (!dung.inBounds(nx, ny)) continue;
                const int nIdx = idxOf(nx, ny);
                if (visited[nIdx]) continue;
                if (!passableForSearch(nx, ny)) continue;

                const int curIdx = idxOf(cur.x, cur.y);
                int ft = firstTrapIdx[curIdx];
                if (ft == -1 && isKnownTrap(nx, ny)) ft = nIdx;
                firstTrapIdx[nIdx] = ft;

                visited[nIdx] = 1;
                q.push_back(Vec2i{nx, ny});
            }
        }
    }

    return Vec2i{-1, -1};
}

Vec2i Game::findNearestExploreSearchSpot() const {
    constexpr int kMaxSearchTurnsPerSpot = 4;

    const Vec2i start = player().pos;
    const bool canUnlockDoors = (keyCount() > 0) || (lockpickCount() > 0);
    const int W = std::max(1, dung.width);
    const int H = std::max(1, dung.height);

    auto idxOf = [W](int x, int y) { return y * W + x; };

    auto isKnownTrap = [&](int x, int y) {
        for (const Trap& t : trapsCur) {
            if (t.pos.x == x && t.pos.y == y && t.discovered) return true;
        }
        return false;
    };

    auto passable = [&](int x, int y) {
        if (!dung.inBounds(x, y)) return false;
        if (!dung.at(x, y).explored) return false;

        // Treat locked doors as passable if we can actually unlock them.
        if (!dung.isPassable(x, y)) {
            if (!(canUnlockDoors && dung.at(x, y).type == TileType::DoorLocked)) return false;
        }

        if (isKnownTrap(x, y)) return false;
        if (fireAt(x, y) > 0u) return false;

        if (const Entity* occ = entityAt(x, y)) {
            if (occ->id != playerId_ && !occ->friendly) return false;
        }

        return true;
    };

    auto isValidSearchSpot = [&](int x, int y) {
        if (!dung.inBounds(x, y)) return false;
        const Tile& t = dung.at(x, y);
        if (!t.explored) return false;
        if (!passable(x, y)) return false;
        if (isKnownTrap(x, y)) return false;
        if (fireAt(x, y) > 0u) return false;

        const int ii = idxOf(x, y);
        const int tried = (ii >= 0 && static_cast<size_t>(ii) < autoExploreSearchTriedTurns.size())
                              ? static_cast<int>(autoExploreSearchTriedTurns[static_cast<size_t>(ii)])
                              : 0;
        if (tried >= kMaxSearchTurnsPerSpot) return false;

        if (const Entity* occ = entityAt(x, y)) {
            if (occ->id != playerId_ && !occ->friendly) return false;
        }

        // Corridor geometry heuristic:
        // - Dead ends are strong candidates.
        // - Tight corners (L-bends in corridors) are moderate candidates.
        // We intentionally avoid using hidden knowledge (e.g., the presence of a DoorSecret tile).
        const bool n = passable(x, y - 1);
        const bool s = passable(x, y + 1);
        const bool w = passable(x - 1, y);
        const bool e = passable(x + 1, y);
        const int pass4 = (int)n + (int)s + (int)w + (int)e;

        if (pass4 <= 1) return true;

        if (pass4 == 2) {
            // Exclude straight corridors; only corners.
            if ((n && s) || (e && w)) return false;

            // Exclude roomy corners (e.g., room interiors) by requiring a very tight 8-neighborhood.
            int pass8 = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    if (passable(x + dx, y + dy)) pass8++;
                }
            }
            if (pass8 <= 2) return true;
        }

        return false;
    };

    std::vector<uint8_t> visited(static_cast<size_t>(W * H), 0);
    std::deque<Vec2i> q;
    visited[idxOf(start.x, start.y)] = 1;
    q.push_back(start);

    static constexpr int kDirs8[8][2] = {
        {1, 0},
        {-1, 0},
        {0, 1},
        {0, -1},
        {1, 1},
        {1, -1},
        {-1, 1},
        {-1, -1},
    };

    while (!q.empty()) {
        const Vec2i cur = q.front();
        q.pop_front();

        if (isValidSearchSpot(cur.x, cur.y)) {
            return cur;
        }

        for (int i = 0; i < 8; ++i) {
            const int dx = kDirs8[i][0];
            const int dy = kDirs8[i][1];
            const int nx = cur.x + dx;
            const int ny = cur.y + dy;

            if (!dung.inBounds(nx, ny)) continue;
            if (!passable(nx, ny)) continue;
            if (!diagonalPassable(dung, cur, dx, dy)) continue;

            const int ii = idxOf(nx, ny);
            if (visited[ii]) continue;
            visited[ii] = 1;
            q.push_back({nx, ny});
        }
    }

    return {-1, -1};
}

std::vector<Vec2i> Game::findPathBfs(Vec2i start, Vec2i goal, bool requireExplored, bool allowKnownTraps) const {
    if (!dung.inBounds(start.x, start.y) || !dung.inBounds(goal.x, goal.y)) return {};
    if (start == goal) return { start };

    const int W = std::max(1, dung.width);
    const int H = std::max(1, dung.height);

    // Weighted pathing: doors and locks take extra turns to traverse.
    // This produces auto-travel paths that are closer to "minimum turns" rather
    // than "minimum tiles".

    auto isKnownTrap = [&](int x, int y) -> bool {
        for (const auto& t : trapsCur) {
            if (!t.discovered) continue;
            if (t.pos.x == x && t.pos.y == y) return true;
        }
        return false;
    };

    const bool hasKey = (keyCount() > 0);
    const bool hasLockpick = (lockpickCount() > 0);
    const bool canUnlockDoors = hasKey || hasLockpick;

    const bool levitating = (player().effects.levitationTurns > 0);

    auto passable = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;

        if (requireExplored && !dung.at(x, y).explored && !(x == goal.x && y == goal.y)) {
            return false;
        }

        // Allow auto-pathing through locked doors if the player has keys or lockpicks.
        // The actual unlock/open happens in tryMove.
        if (!dung.isPassable(x, y)) {
            const TileType tt = dung.at(x, y).type;
            if (tt == TileType::Chasm && levitating) {
                // Levitation lets the player auto-path across chasms.
            } else if (!(canUnlockDoors && tt == TileType::DoorLocked)) {
                return false;
            }
        }

        // Avoid known traps.
        if (!allowKnownTraps && isKnownTrap(x, y) && !(x == goal.x && y == goal.y)) return false;

        // Don't path through monsters.
        if (const Entity* occ = entityAt(x, y)) {
            if (occ->id != playerId_ && !occ->friendly) return false;
        }

        return true;
    };

    auto stepCost = [&](int x, int y) -> int {
        if (!dung.inBounds(x, y)) return 0;
        const TileType tt = dung.at(x, y).type;

        // Default: moving into a tile costs one turn.
        int cost = 1;
        switch (tt) {
            case TileType::DoorClosed:
                // 1 turn to open + 1 to step in.
                cost = 2;
                break;
            case TileType::DoorLocked:
                if (!canUnlockDoors) return 0;
                // Keys are guaranteed; lockpicks can fail and burn turns.
                cost = hasKey ? 2 : 4;
                break;
            default:
                cost = 1;
                break;
        }

        // Strongly prefer routes that avoid lingering fire, but don't hard-block.
        if (fireAt(x, y) > 0u) cost += 25;
        if (allowKnownTraps) {
            if (const Trap* t = discoveredTrapAt(trapsCur, x, y)) {
                cost += autoMoveTrapPenalty(t->kind);
            }
        }

        return cost;
    };

    auto diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return diagonalPassable(dung, {fromX, fromY}, dx, dy);
    };

    return dijkstraPath(W, H, start, goal, passable, stepCost, diagOk);
}
