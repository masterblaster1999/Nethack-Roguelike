#include "game_internal.hpp"

#include "threat_field.hpp"
#include "hearing_field.hpp"

#include <limits>

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
    autoTravelCautionAnnounced = false;

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

    // Destination validation: most of the time we require a passable tile.
    // However, for convenience we allow auto-travel to certain interactable blockers
    // that the player can resolve while walking (e.g. a locked door you can unlock).
    {
        const TileType tt = dung.at(goal.x, goal.y).type;

        bool okGoal = dung.isPassable(goal.x, goal.y);
        if (!okGoal) {
            const bool canUnlockDoors = (keyCount() > 0) || (lockpickCount() > 0);
            const bool levitating = (player().effects.levitationTurns > 0);

            if (tt == TileType::DoorLocked && canUnlockDoors) {
                okGoal = true;
            } else if (tt == TileType::Chasm && levitating) {
                // Rare case: allow targeting a chasm tile directly while levitating.
                okGoal = true;
            }
        }

        if (!okGoal) {
            pushMsg("NO PATH (BLOCKED).", MessageKind::Warning);
            return false;
        }
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
    //
    // Auto-explore is intentionally conservative: if any hostile is visible, stop immediately.
    // Auto-travel can keep going when hostiles are visible but still far away; a threat/ETA
    // check is performed later once we know the next step.
    if (autoMode == AutoMoveMode::Explore && anyVisibleHostiles()) {
        pushMsg("AUTO-EXPLORE INTERRUPTED!", MessageKind::Warning);
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

    if (confusionGasAt(player().pos.x, player().pos.y) > 0u) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE IN CONFUSION GAS).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (poisonGasAt(player().pos.x, player().pos.y) > 0u) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE IN POISON GAS).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    // Movement blockers: auto-move would just burn turns (and potentially make noise).
    // Stop immediately so the player can choose a deliberate action.
    if (player().effects.webTurns > 0) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE WEBBED).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }
    if (encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
        pushMsg("AUTO-MOVE STOPPED (YOU ARE OVERLOADED).", MessageKind::Warning);
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



    // Auto-travel: threat-aware safety stop.
    // If a visible hostile could reach the player (or the next step tile) within a short
    // ETA window, interrupt travel. Otherwise, keep going (and optionally warn once).
    if (autoMode == AutoMoveMode::Travel) {
        constexpr int kStopEta = 6;

        ThreatFieldResult tf = buildVisibleHostileThreatField(*this, kStopEta);
        if (!tf.sources.empty() && !tf.dist.empty()) {
            const int W = std::max(1, dung.width);
            auto idxOf = [W](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

            const size_t iHere = idxOf(p.pos.x, p.pos.y);
            const size_t iNext = idxOf(next.x, next.y);

            const int etaHere = (iHere < tf.dist.size()) ? tf.dist[iHere] : -1;
            const int etaNext = (iNext < tf.dist.size()) ? tf.dist[iNext] : -1;

            if ((etaHere >= 0 && etaHere <= kStopEta) || (etaNext >= 0 && etaNext <= kStopEta)) {
                pushMsg("AUTO-TRAVEL INTERRUPTED (DANGER NEARBY).", MessageKind::Warning);
                stopAutoMove(true);
                return false;
            }

            if (!autoTravelCautionAnnounced) {
                pushMsg("AUTO-TRAVEL: HOSTILES IN SIGHT (CAUTIOUS ROUTE).", MessageKind::Warning);
                autoTravelCautionAnnounced = true;
            }
        } else {
            autoTravelCautionAnnounced = false;
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

    // Special-case: if we're about to interact with a locked door, track state so we can
    // abort auto-move after a failed lockpick attempt (avoids noisy repeated attempts).
    const bool lockedDoorBefore = dung.isDoorLocked(next.x, next.y);
    const int keysBefore = keyCount();
    const int lockpicksBefore = lockpickCount();

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

    // If we attempted to pick a lock and failed, stop immediately so the player can decide
    // whether to try again, use a key, or take another route.
    if (lockedDoorBefore && keysBefore == 0 && lockpicksBefore > 0 && p.pos == posBefore && dung.isDoorLocked(next.x, next.y)) {
        pushMsg("AUTO-MOVE STOPPED (FAILED TO PICK LOCK).", MessageKind::Warning);
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
        if (confusionGasAt(x, y) > 0u) return false;
        if (poisonGasAt(x, y) > 0u) return false;

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
        if (confusionGasAt(x, y) > 0u) return false;
        if (poisonGasAt(x, y) > 0u) return false;

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
                const int dx = dirs[dir][0];
                const int dy = dirs[dir][1];
                const int nx = cur.x + dx;
                const int ny = cur.y + dy;
                if (!dung.inBounds(nx, ny)) continue;
                const int nIdx = idxOf(nx, ny);
                if (visited[nIdx]) continue;
                if (!passableForSearch(nx, ny)) continue;

                if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;

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
                const int dx = dirs[dir][0];
                const int dy = dirs[dir][1];
                const int nx = cur.x + dx;
                const int ny = cur.y + dy;
                if (!dung.inBounds(nx, ny)) continue;
                const int nIdx = idxOf(nx, ny);
                if (visited[nIdx]) continue;
                if (!passableForSearch(nx, ny)) continue;

                if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;

                const int curIdx = idxOf(cur.x, cur.y);
                int ft = firstTrapIdx[curIdx];
                if (ft == -1 && isKnownTrap(nx, ny)) ft = nIdx;
                firstTrapIdx[nIdx] = ft;

                visited[nIdx] = 1;
                q.push_back(Vec2i{nx, ny});
            }
        }
    }

    // Pass 3: if the floor appears "fully explored" but the last unexplored pocket is
    // behind a visible locked door, allow auto-explore to walk to that door and unlock it.
    //
    // We only do this AFTER exhausting normal frontiers so auto-explore doesn't burn keys
    // while there is still plenty of open terrain to reveal.
    if (canUnlockDoors) {
        auto isLockedDoorFrontier = [&](int x, int y) -> bool {
            if (!dung.inBounds(x, y)) return false;
            const Tile& t = dung.at(x, y);
            if (!t.explored) return false;
            if (t.type != TileType::DoorLocked) return false;
            if (fireAt(x, y) > 0u) return false;
            if (confusionGasAt(x, y) > 0u) return false;
            if (poisonGasAt(x, y) > 0u) return false;

            for (int dir = 0; dir < 8; ++dir) {
                const int nx = x + dirs[dir][0];
                const int ny = y + dirs[dir][1];
                if (!dung.inBounds(nx, ny)) continue;
                if (!dung.at(nx, ny).explored) return true;
            }
            return false;
        };

        // Pass 3a: BFS that does NOT traverse known traps.
        {
            std::deque<Vec2i> q;
            std::vector<uint8_t> visited(static_cast<size_t>(W * H), 0);
            visited[idxOf(start.x, start.y)] = 1;
            q.push_back(start);

            while (!q.empty()) {
                const Vec2i cur = q.front();
                q.pop_front();

                if (cur != start && isLockedDoorFrontier(cur.x, cur.y)) return cur;

                for (int dir = 0; dir < 8; ++dir) {
                    const int dx = dirs[dir][0];
                    const int dy = dirs[dir][1];
                    const int nx = cur.x + dx;
                    const int ny = cur.y + dy;
                    if (!dung.inBounds(nx, ny)) continue;
                    const int nIdx = idxOf(nx, ny);
                    if (visited[nIdx]) continue;
                    if (!passableForSearch(nx, ny)) continue;

                    if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;

                    if (isKnownTrap(nx, ny)) continue;

                    visited[nIdx] = 1;
                    q.push_back(Vec2i{nx, ny});
                }
            }
        }

        // Pass 3b: BFS that allows traversing known traps. If we find a locked-door frontier,
        // return the FIRST known trap tile along the shortest path to it.
        {
            std::deque<Vec2i> q;
            std::vector<uint8_t> visited(static_cast<size_t>(W * H), 0);
            std::vector<int> firstTrapIdx(static_cast<size_t>(W * H), -1);
            visited[idxOf(start.x, start.y)] = 1;
            q.push_back(start);

            while (!q.empty()) {
                const Vec2i cur = q.front();
                q.pop_front();

                if (cur != start && isLockedDoorFrontier(cur.x, cur.y)) {
                    const int ft = firstTrapIdx[idxOf(cur.x, cur.y)];
                    if (ft != -1) return Vec2i{ft % W, ft / W};
                    return cur;
                }

                for (int dir = 0; dir < 8; ++dir) {
                    const int dx = dirs[dir][0];
                    const int dy = dirs[dir][1];
                    const int nx = cur.x + dx;
                    const int ny = cur.y + dy;
                    if (!dung.inBounds(nx, ny)) continue;
                    const int nIdx = idxOf(nx, ny);
                    if (visited[nIdx]) continue;
                    if (!passableForSearch(nx, ny)) continue;

                    if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;

                    const int curIdx = idxOf(cur.x, cur.y);
                    int ft = firstTrapIdx[curIdx];
                    if (ft == -1 && isKnownTrap(nx, ny)) ft = nIdx;
                    firstTrapIdx[nIdx] = ft;

                    visited[nIdx] = 1;
                    q.push_back(Vec2i{nx, ny});
                }
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
        if (confusionGasAt(x, y) > 0u) return false;
        if (poisonGasAt(x, y) > 0u) return false;

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
        if (confusionGasAt(x, y) > 0u) return false;
        if (poisonGasAt(x, y) > 0u) return false;

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

    auto idxOf = [W](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    // Weighted pathing: doors and locks take extra turns to traverse.
    // This produces auto-travel paths that are closer to "minimum turns" rather
    // than "minimum tiles".

    // Build a per-tile discovered-trap penalty grid once (O(traps) instead of O(traps) per expanded node).
    std::vector<int> trapPenalty;
    if (!trapsCur.empty()) {
        trapPenalty.assign(static_cast<size_t>(W * H), 0);
        for (const auto& t : trapsCur) {
            if (!t.discovered) continue;
            if (!dung.inBounds(t.pos.x, t.pos.y)) continue;
            const size_t i = idxOf(t.pos.x, t.pos.y);
            const int p = autoMoveTrapPenalty(t.kind);
            if (i < trapPenalty.size() && p > trapPenalty[i]) trapPenalty[i] = p;
        }
    }

    auto isKnownTrap = [&](int x, int y) -> bool {
        if (trapPenalty.empty()) return false;
        const size_t i = idxOf(x, y);
        if (i >= trapPenalty.size()) return false;
        return trapPenalty[i] > 0;
    };

    // Threat-aware auto-travel: when hostiles are visible, add a soft repulsion term
    // based on the same monster pathing policy used by the AI and LOOK Threat Preview.
    const ThreatFieldResult threat = buildVisibleHostileThreatField(*this, 60);
    const std::vector<int>* threatDist = nullptr;
    if (!threat.sources.empty() && threat.dist.size() == static_cast<size_t>(W * H)) {
        threatDist = &threat.dist;
    }

    auto threatPenaltyFor = [&](int x, int y) -> int {
        if (!threatDist) return 0;
        const int eta = (*threatDist)[idxOf(x, y)];
        if (eta < 0) return 0;

        // Within this ETA window, increasingly discourage stepping closer.
        // Numbers tuned so it biases route choice without permanently dead-ending corridors.
        static constexpr int kAvoidEta = 12;
        if (eta >= kAvoidEta) return 0;

        int p = (kAvoidEta - eta) * 5;
        if (eta <= 2) p += 60; // very close -> strong repulsion
        return p;
    };

    // Noise-aware auto-travel (Sneak mode): when you are sneaking and hostiles are
    // currently visible, bias pathing away from tiles where your *actual* footstep
    // volume would be audible to any visible hostile.
    //
    // This is intentionally limited to currently visible hostiles so auto-move
    // can't "cheat" by avoiding unseen monsters.
    const std::vector<int>* minReqVol = nullptr;
    std::vector<int> footstepVol; // cached per-tile footstep volume (only when needed)
    HearingFieldResult hearing;
    if (isSneaking()) {
        // Max distance relevant for footsteps: maxFootstepVol(14) + maxHearingDelta(4) == 18.
        hearing = buildVisibleHostileHearingField(*this, 18);
        if (!hearing.listeners.empty() && hearing.minRequiredVolume.size() == static_cast<size_t>(W * H)) {
            minReqVol = &hearing.minRequiredVolume;

            // Cache the player's real footstep volume per tile so stepCost remains cheap.
            footstepVol.assign(static_cast<size_t>(W * H), 0);
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    footstepVol[idxOf(x, y)] = playerFootstepNoiseVolumeAt({x, y});
                }
            }
        }
    }

    auto noisePenaltyFor = [&](int x, int y) -> int {
        if (!minReqVol) return 0;
        const int req = (*minReqVol)[idxOf(x, y)];
        if (req < 0) return 0;

        const int vol = footstepVol[idxOf(x, y)];
        if (vol <= 0) return 0; // silent step

        const int margin = vol - req;
        if (margin < 0) return 0; // not audible

        // Penalize tiles that would be heard, scaling with how far above the
        // minimum-heard threshold the footstep is.
        int p = 6 + margin * 4;
        if (req <= 1) p += 10; // very close listeners -> stronger discouragement
        return p;
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
        // Prefer to avoid standing in hazardous gas clouds during auto-travel.
        const uint8_t cg = confusionGasAt(x, y);
        if (cg > 0u) cost += 12 + static_cast<int>(cg) / 32;
        const uint8_t pg = poisonGasAt(x, y);
        if (pg > 0u) cost += 16 + static_cast<int>(pg) / 32;

        if (allowKnownTraps && !trapPenalty.empty()) {
            cost += trapPenalty[idxOf(x, y)];
        }

        // Threat-aware bias (only active when hostiles are visible).
        cost += threatPenaltyFor(x, y);

        // Sneak-aware noise bias (only active when hostiles are visible).
        cost += noisePenaltyFor(x, y);

        return cost;
    };

    auto diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return diagonalPassable(dung, {fromX, fromY}, dx, dy);
    };

    return dijkstraPath(W, H, start, goal, passable, stepCost, diagOk);
}



bool Game::evadeStep() {
    // A single "smart step" away from visible hostiles.
    //
    // Design goals:
    // - Reuse the same ETA threat field as LOOK Threat Preview + auto-travel.
    // - Reuse the same hearing/audibility field used by sneak-aware auto-travel.
    // - Keep it best-effort and conservative: avoid stepping onto known hazards/traps,
    //   and prefer quieter moves when sneaking.

    // Only meaningful in the main game state.
    if (isFinished()) return false;

    Entity& p = playerMut();
    const Vec2i start = p.pos;
    if (!dung.inBounds(start.x, start.y)) return false;

    constexpr int kThreatMaxCost = 30;
    ThreatFieldResult tf = buildVisibleHostileThreatField(*this, kThreatMaxCost);
    if (tf.sources.empty() || tf.dist.empty()) {
        pushMsg("EVADE: NO VISIBLE THREATS.", MessageKind::System);
        return false;
    }

    // Compute the hearing field once so we can penalize steps that would be heard.
    constexpr int kHearMaxCost = 20;
    HearingFieldResult hf = buildVisibleHostileHearingField(*this, kHearMaxCost);
    const bool haveHearing = (!hf.listeners.empty() && !hf.minRequiredVolume.empty());

    const int W = std::max(1, dung.width);

    auto idxOf = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y * W + x);
    };

    auto etaRawAt = [&](Vec2i pos) -> int {
        if (!dung.inBounds(pos.x, pos.y)) return -1;
        const size_t i = idxOf(pos.x, pos.y);
        if (i >= tf.dist.size()) return -1;
        return tf.dist[i];
    };

    // Normalize "not reachable within field budget" to a large safe ETA.
    auto etaNorm = [&](int eta) -> int {
        return (eta < 0) ? (kThreatMaxCost + 10) : eta;
    };

    auto reqVolAt = [&](Vec2i pos) -> int {
        if (!haveHearing) return -1;
        if (!dung.inBounds(pos.x, pos.y)) return -1;
        const size_t i = idxOf(pos.x, pos.y);
        if (i >= hf.minRequiredVolume.size()) return -1;
        return hf.minRequiredVolume[i];
    };

    const int etaHere = etaNorm(etaRawAt(start));

    const bool phasing = entityCanPhase(p.kind);
    const bool levitating = (p.effects.levitationTurns > 0);

    struct Opt {
        int dx = 0;
        int dy = 0;
        bool isWait = false;
        bool moves = false;
        Vec2i resPos{0, 0};
        Vec2i noisePos{0, 0};
        int noiseVol = 0;
        int eta = 0;
        int score = std::numeric_limits<int>::min();
    };

    auto scoreOption = [&](Vec2i resPos, Vec2i noisePos, int noiseVol, bool moves, bool isWait) -> std::pair<int, int> {
        const int eta = etaNorm(etaRawAt(resPos));
        int score = 0;

        // Primary: maximize safety (ETA). The *gain* term helps break ties
        // when multiple options are "safe enough".
        score += eta * 100;
        score += (eta - etaHere) * 40;

        // Strongly discourage spending turns without repositioning.
        if (!moves) {
            score -= isWait ? 220 : 140;
        }

        // Environmental hazards at the final position.
        const uint8_t f = fireAt(resPos.x, resPos.y);
        if (f > 0u) score -= 900 + static_cast<int>(f) * 3;

        const uint8_t cg = confusionGasAt(resPos.x, resPos.y);
        if (cg > 0u) score -= 320 + static_cast<int>(cg) * 4;

        const uint8_t pg = poisonGasAt(resPos.x, resPos.y);
        if (pg > 0u) score -= 380 + static_cast<int>(pg) * 4;

        // Known traps: strongly avoided, but not hard-blocked.
        if (moves) {
            if (const Trap* t = discoveredTrapAt(trapsCur, resPos.x, resPos.y)) {
                const int tp = autoMoveTrapPenalty(t->kind);
                score -= 1800 + tp * 8;
            }
        }

        // Audibility penalty (only when hostiles are visible).
        if (haveHearing && noiseVol > 0) {
            const int req = reqVolAt(noisePos);
            if (req >= 0) {
                if (noiseVol > req) {
                    const int margin = noiseVol - req;
                    const int w = isSneaking() ? 250 : 140;
                    score -= 650 + margin * w;
                } else {
                    // Small bonus: quiet step that stays under the hearing threshold.
                    if (isSneaking()) score += 40;
                }
            }
        }

        return {score, eta};
    };

    auto better = [&](const Opt& a, const Opt& b) -> bool {
        if (a.score != b.score) return a.score > b.score;
        if (a.eta != b.eta) return a.eta > b.eta;
        if (a.moves != b.moves) return a.moves && !b.moves;
        if (a.noiseVol != b.noiseVol) return a.noiseVol < b.noiseVol;
        const int ad = std::abs(a.dx) + std::abs(a.dy);
        const int bd = std::abs(b.dx) + std::abs(b.dy);
        // Prefer cardinal over diagonal in ties (more controllable), but prefer moving over waiting.
        if (ad != bd) return ad < bd;
        return false;
    };

    Opt best;
    bool haveBest = false;

    auto consider = [&](Opt o) {
        auto sc = scoreOption(o.resPos, o.noisePos, o.noiseVol, o.moves, o.isWait);
        o.score = sc.first;
        o.eta = sc.second;

        if (!haveBest || better(o, best)) {
            best = o;
            haveBest = true;
        }
    };

    // Wait is a legal fallback (silent) when boxed in.
    {
        Opt w;
        w.isWait = true;
        w.moves = false;
        w.resPos = start;
        w.noisePos = start;
        w.noiseVol = 0;
        consider(w);
    }

    static const int dirs[8][2] = {
        { 0, -1}, { 0, 1}, {-1, 0}, { 1, 0},
        {-1, -1}, { 1, -1}, {-1, 1}, { 1, 1},
    };

    for (const auto& dxy : dirs) {
        const int dx = dxy[0];
        const int dy = dxy[1];
        const int nx = start.x + dx;
        const int ny = start.y + dy;

        if (!dung.inBounds(nx, ny)) continue;

        // Mirror tryMove() corner-cutting rules.
        if (!phasing && dx != 0 && dy != 0 && !diagonalPassable(dung, start, dx, dy)) {
            continue;
        }

        // Avoid intentionally attacking; this is an evasion helper.
        if (const Entity* occ = entityAt(nx, ny)) {
            if (occ->id == p.id) {
                continue;
            }
            // Allow friendly swaps (helps retreat in corridors).
            if (!occ->friendly) {
                continue;
            }
            // If they're webbed, tryMove() will refuse the swap.
            if (occ->effects.webTurns > 0) {
                continue;
            }
        }

        const TileType tt = dung.at(nx, ny).type;

        Opt o;
        o.dx = dx;
        o.dy = dy;
        o.isWait = false;

        // Door interactions consume a turn without changing position.
        if (!phasing && tt == TileType::DoorClosed) {
            o.moves = false;
            o.resPos = start;
            o.noisePos = {nx, ny};
            o.noiseVol = isSneaking() ? 8 : 12;
            consider(o);
            continue;
        }

        if (!phasing && tt == TileType::DoorLocked) {
            // Predict the best available unlock path (keys preferred over picks).
            if (keyCount() > 0) {
                o.moves = false;
                o.resPos = start;
                o.noisePos = {nx, ny};
                o.noiseVol = isSneaking() ? 9 : 12;
                consider(o);
                continue;
            }
            if (lockpickCount() > 0) {
                o.moves = false;
                o.resPos = start;
                o.noisePos = {nx, ny};
                o.noiseVol = isSneaking() ? 8 : 10;
                consider(o);
                continue;
            }
            // Can't unlock: skip.
            continue;
        }

        // Pushable boulder: allow if the push is legal (orthogonal, empty destination).
        if (!phasing && tt == TileType::Boulder) {
            if (dx != 0 && dy != 0) continue;

            const int bx = nx + dx;
            const int by = ny + dy;
            if (!dung.inBounds(bx, by)) continue;
            if (entityAt(bx, by)) continue;

            const TileType dest = dung.at(bx, by).type;
            if (dest == TileType::Floor) {
                o.moves = true;
                o.resPos = {nx, ny};
                o.noisePos = {nx, ny};
                o.noiseVol = 13;
                consider(o);
                continue;
            }
            if (dest == TileType::Chasm) {
                o.moves = true;
                o.resPos = {nx, ny};
                o.noisePos = {nx, ny};
                o.noiseVol = 16;
                consider(o);
                continue;
            }

            continue;
        }

        // Standard movement.
        bool canStep = true;
        if (!phasing) {
            canStep = dung.isWalkable(nx, ny) || (tt == TileType::Chasm && levitating);
        }
        if (!canStep) continue;

        o.moves = true;
        o.resPos = {nx, ny};
        o.noisePos = o.resPos;
        o.noiseVol = (p.kind == EntityKind::Player) ? playerFootstepNoiseVolumeAt(o.resPos) : 0;
        consider(o);
    }

    if (!haveBest) {
        pushMsg("EVADE: NO VALID MOVE.", MessageKind::Warning, true);
        return false;
    }

    if (best.isWait || (best.dx == 0 && best.dy == 0)) {
        pushMsg("YOU WAIT.", MessageKind::Info);
        return true;
    }

    // Execute the chosen direction through the real movement system so all
    // side effects (door open, lockpicking, boulder pushing, trap triggers, noise, ...)
    // remain authoritative.
    return tryMove(p, best.dx, best.dy);
}
