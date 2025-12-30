#include "game_internal.hpp"

void Game::cancelAutoMove(bool silent) {
    stopAutoMove(silent);
}

void Game::stopAutoMove(bool silent) {
    if (autoMode == AutoMoveMode::None) return;

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;

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

    if (!buildAutoTravelPath(goal, /*requireExplored*/true)) {
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
                if (!buildAutoTravelPath(bestPos, /*requireExplored*/true)) {
                    pushMsg("AUTO-EXPLORE STOPPED (NO PATH TO LOOT).", MessageKind::System);
                    stopAutoMove(true);
                    return false;
                }

                autoExploreGoalIsLoot = true;
                autoExploreGoalPos = bestPos;

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

    Entity& p = playerMut();    const Vec2i next = autoPathTiles[autoPathIndex];

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

    const int dx = next.x - p.pos.x;
    const int dy = next.y - p.pos.y;

    const int hpBefore = p.hp;
    const int poisonBefore = p.effects.poisonTurns;
    const int webBefore = p.effects.webTurns;
    const int confBefore = p.effects.confusionTurns;
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

bool Game::buildAutoTravelPath(Vec2i goal, bool requireExplored) {
    autoPathTiles = findPathBfs(player().pos, goal, requireExplored);
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

    Vec2i goal = findNearestExploreFrontier();
    if (goal.x < 0 || goal.y < 0) return false;
    return buildAutoTravelPath(goal, /*requireExplored*/true);
}

Vec2i Game::findNearestExploreFrontier() const {
    const Vec2i start = player().pos;

    std::vector<uint8_t> visited(MAP_W * MAP_H, 0);
    std::deque<Vec2i> q;

    auto idxOf = [](int x, int y) { return y * MAP_W + x; };

    visited[idxOf(start.x, start.y)] = 1;
    q.push_back(start);

    const bool canUnlockDoors = (keyCount() > 0) || (lockpickCount() > 0);

    auto isKnownTrap = [&](int x, int y) -> bool {
        for (const auto& t : trapsCur) {
            if (!t.discovered) continue;
            if (t.pos.x == x && t.pos.y == y) return true;
        }
        return false;
    };

    auto isFrontier = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        const Tile& t = dung.at(x, y);
        if (!t.explored) return false;
        // Treat locked doors as passable frontiers if we can unlock them.
        if (!dung.isPassable(x, y)) {
            const TileType tt = dung.at(x, y).type;
            if (!(canUnlockDoors && tt == TileType::DoorLocked)) return false;
        }
        if (isKnownTrap(x, y)) return false;

        // Any adjacent unexplored tile means stepping here can reveal something.
        const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
        for (const auto& d : dirs) {
            int nx = x + d[0], ny = y + d[1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.at(nx, ny).explored) return true;
        }
        return false;
    };

    const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };

    while (!q.empty()) {
        Vec2i cur = q.front();
        q.pop_front();

        if (!(cur == start) && isFrontier(cur.x, cur.y)) return cur;

        for (const auto& d : dirs) {
            int dx = d[0], dy = d[1];
            int nx = cur.x + dx, ny = cur.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;

            const int ii = idxOf(nx, ny);
            if (visited[ii]) continue;

            const Tile& t = dung.at(nx, ny);
            if (!t.explored) continue; // don't route through unknown
            if (!dung.isPassable(nx, ny)) {
                const TileType tt = dung.at(nx, ny).type;
                if (!(canUnlockDoors && tt == TileType::DoorLocked)) continue;
            }
            if (isKnownTrap(nx, ny)) continue;

            if (const Entity* occ = entityAt(nx, ny)) {
                if (occ->id != playerId_ && occ->kind != EntityKind::Dog) continue;
            }

            visited[ii] = 1;
            q.push_back({nx, ny});
        }
    }

    return {-1, -1};
}

std::vector<Vec2i> Game::findPathBfs(Vec2i start, Vec2i goal, bool requireExplored) const {
    if (!dung.inBounds(start.x, start.y) || !dung.inBounds(goal.x, goal.y)) return {};
    if (start == goal) return { start };

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

    auto passable = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;

        if (requireExplored && !dung.at(x, y).explored && !(x == goal.x && y == goal.y)) {
            return false;
        }

        // Allow auto-pathing through locked doors if the player has keys or lockpicks.
        // The actual unlock/open happens in tryMove.
        if (!dung.isPassable(x, y)) {
            const TileType tt = dung.at(x, y).type;
            if (!(canUnlockDoors && tt == TileType::DoorLocked)) {
                return false;
            }
        }

        // Avoid known traps.
        if (isKnownTrap(x, y) && !(x == goal.x && y == goal.y)) return false;

        // Don't path through monsters.
        if (const Entity* occ = entityAt(x, y)) {
            if (occ->id != playerId_ && occ->kind != EntityKind::Dog) return false;
        }

        return true;
    };

    auto stepCost = [&](int x, int y) -> int {
        if (!dung.inBounds(x, y)) return 0;
        const TileType tt = dung.at(x, y).type;

        // Default: moving into a tile costs one turn.
        switch (tt) {
            case TileType::DoorClosed:
                // 1 turn to open + 1 to step in.
                return 2;
            case TileType::DoorLocked:
                if (!canUnlockDoors) return 0;
                // Keys are guaranteed; lockpicks can fail and burn turns.
                return hasKey ? 2 : 4;
            default:
                return 1;
        }
    };

    auto diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return diagonalPassable(dung, {fromX, fromY}, dx, dy);
    };

    return dijkstraPath(MAP_W, MAP_H, start, goal, passable, stepCost, diagOk);
}

