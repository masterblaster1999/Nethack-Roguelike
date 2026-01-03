#include "game_internal.hpp"

bool Game::tryMove(Entity& e, int dx, int dy) {
    if (e.hp <= 0) return false;
    if (dx == 0 && dy == 0) return false;

    const bool phasing = entityCanPhase(e.kind);
    const bool levitating = (e.effects.levitationTurns > 0);

    // Webbed: you can still act (use items, fire, etc.) but cannot move.
    // Attempting to move consumes a turn (so the web can wear off).
    if (!phasing && e.effects.webTurns > 0) {
        if (e.kind == EntityKind::Player) {
            pushMsg("YOU STRUGGLE AGAINST STICKY WEBBING!", MessageKind::Warning, true);
            // Struggling is loud enough to draw attention.
            emitNoise(e.pos, 7);
        }
        return true;
    }

    // Encumbrance: overloaded players cannot move. Attempting to move still costs a turn
    // (prevents "free" time-stalling by spamming movement inputs).
    if (e.id == playerId_ && encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
        pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true);
        // Shifting under too much weight makes noise.
        emitNoise(e.pos, 5);
        return true;
    }

    // Clamp to single-tile steps (safety: AI/pathing should only request these).
    dx = clampi(dx, -1, 1);
    dy = clampi(dy, -1, 1);

    // Confusion scrambles intended direction.
    if (e.effects.confusionTurns > 0) {
        static const int dirs[8][2] = {
            { 1, 0}, {-1, 0}, {0, 1}, {0,-1},
            { 1, 1}, { 1,-1}, {-1, 1}, {-1,-1},
        };
        const int i = rng.range(0, 7);
        dx = dirs[i][0];
        dy = dirs[i][1];
        if (e.kind == EntityKind::Player && rng.chance(0.25f)) {
            pushMsg("YOU STUMBLE IN CONFUSION.", MessageKind::Info, true);
        }
    }

    const int nx = e.pos.x + dx;
    const int ny = e.pos.y + dy;

    if (!dung.inBounds(nx, ny)) return false;

    // Prevent diagonal corner-cutting (no slipping between two blocking tiles).
    if (!phasing && dx != 0 && dy != 0 && !diagonalPassable(dung, e.pos, dx, dy)) {
        if (e.kind == EntityKind::Player) pushMsg("YOU CAN'T SQUEEZE THROUGH.");
        return false;
    }

    // Closed door: opening consumes a turn.
    if (!phasing && dung.isDoorClosed(nx, ny)) {
        dung.openDoor(nx, ny);
        if (e.kind == EntityKind::Player) {
            pushMsg("YOU OPEN THE DOOR.");
            // Opening doors is noisy; monsters may investigate.
            emitNoise({nx, ny}, 12);
        }
        return true;
    }

    // Locked door: keys open it instantly; lockpicks can work as a fallback.
    if (!phasing && dung.isDoorLocked(nx, ny)) {
        if (e.kind != EntityKind::Player) {
            // Monsters generally can't open locked doors.
            // However, a few heavy bruisers can bash them down while hunting.
            // This prevents "perfect safety" behind vault doors and makes
            // late-game chases more exciting.
            const bool canBash = (e.kind == EntityKind::Ogre || e.kind == EntityKind::Troll || e.kind == EntityKind::Minotaur);
            if (!canBash || !e.alerted) {
                return false;
            }

            float p = 0.0f;
            switch (e.kind) {
                case EntityKind::Ogre:     p = 0.30f; break;
                case EntityKind::Troll:    p = 0.25f; break;
                case EntityKind::Minotaur: p = 0.55f; break;
                default: p = 0.0f; break;
            }

            // Slight scaling with strength/depth so endgame bruisers feel scarier.
            p += 0.02f * static_cast<float>(std::max(0, e.baseAtk - 5));
            p = std::clamp(p, 0.05f, 0.85f);

            const bool vis = dung.inBounds(nx, ny) && dung.at(nx, ny).visible;
            if (rng.chance(p)) {
                // Smash -> door becomes open in one action.
                dung.unlockDoor(nx, ny);
                dung.openDoor(nx, ny);

                if (vis) {
                    std::ostringstream ss;
                    ss << kindName(e.kind) << " SMASHES OPEN THE LOCKED DOOR!";
                    pushMsg(ss.str(), MessageKind::Warning, false);
                }
            } else {
                if (vis) {
                    std::ostringstream ss;
                    ss << kindName(e.kind) << " RAMS THE LOCKED DOOR!";
                    pushMsg(ss.str(), MessageKind::Warning, false);
                }
            }

            // Bashing is loud, regardless of success.
            emitNoise({nx, ny}, 14);
            return true;
        }

        // Prefer keys (guaranteed).
        if (consumeKeys(1)) {
            dung.unlockDoor(nx, ny);
            dung.openDoor(nx, ny);
            pushMsg("YOU UNLOCK THE DOOR.", MessageKind::System, true);
            emitNoise({nx, ny}, 12);
            return true;
        }

        // No keys: attempt to pick the lock if you have lockpicks.
        if (lockpickCount() > 0) {
            // Success chance scales a bit with character level.
            float p = 0.55f + 0.03f * static_cast<float>(charLevel);
            // Talents: Agility helps with lockpicking.
            p += 0.02f * static_cast<float>(playerAgility());
            p = std::min(0.90f, p);

            if (rng.chance(p)) {
                dung.unlockDoor(nx, ny);
                dung.openDoor(nx, ny);
                pushMsg("YOU PICK THE LOCK.", MessageKind::Success, true);
            } else {
                pushMsg("YOU FAIL TO PICK THE LOCK.", MessageKind::Warning, true);

                // Chance the pick breaks on a failed attempt.
                const float breakChance = 0.25f;
                if (rng.chance(breakChance)) {
                    consumeLockpicks(1);
                    pushMsg("YOUR LOCKPICK BREAKS!", MessageKind::Warning, true);
                }
            }

            // Picking is noisy regardless of success.
            emitNoise({nx, ny}, 10);

            return true; // picking takes a turn either way
        }

        pushMsg("THE DOOR IS LOCKED.", MessageKind::Warning, true);
        return false;
    }

    // Pushable boulders (Sokoban-style): stepping into a boulder attempts to push it.
    // This is orthogonal-only (no diagonal pushes). Boulders can also be pushed into chasms
    // to create a rough bridge.
    if (!phasing && dung.at(nx, ny).type == TileType::Boulder) {
        if (dx != 0 && dy != 0) {
            if (e.kind == EntityKind::Player) pushMsg("YOU CAN'T PUSH THE BOULDER DIAGONALLY.");
            return false;
        }

        const int bx = nx + dx;
        const int by = ny + dy;

        if (!dung.inBounds(bx, by)) {
            if (e.kind == EntityKind::Player) pushMsg("THE BOULDER WON'T BUDGE.");
            return false;
        }
        if (entityAt(bx, by)) {
            if (e.kind == EntityKind::Player) pushMsg("SOMETHING BLOCKS THE BOULDER.");
            return false;
        }

        const TileType dest = dung.at(bx, by).type;
        if (dest == TileType::Floor) {
            // Slide boulder forward one tile.
            dung.at(bx, by).type = TileType::Boulder;
            dung.at(nx, ny).type = TileType::Floor;
            if (e.kind == EntityKind::Player) pushMsg("YOU PUSH THE BOULDER.", MessageKind::Info, true);
            emitNoise({nx, ny}, 13);
        } else if (dest == TileType::Chasm) {
            // Boulder falls in and fills a single chasm tile, forming a walkable bridge.
            dung.at(bx, by).type = TileType::Floor;
            dung.at(nx, ny).type = TileType::Floor;
            if (e.kind == EntityKind::Player) pushMsg("THE BOULDER CRASHES INTO THE CHASM, FORMING A ROUGH BRIDGE.", MessageKind::Info, true);
            emitNoise({nx, ny}, 16);
        } else {
            if (e.kind == EntityKind::Player) pushMsg("THE BOULDER WON'T BUDGE.");
            return false;
        }
    }

    const TileType tgtType = dung.at(nx, ny).type;
    const bool canStep = dung.isWalkable(nx, ny) || (tgtType == TileType::Chasm && levitating);

    if (!phasing && !canStep) {
        if (e.kind == EntityKind::Player) {
            if (tgtType == TileType::Chasm) {
                pushMsg("YOU CAN'T CROSS THE CHASM.", MessageKind::Warning, true);
                return false;
            }
            // Quality-of-life: if you are wielding a pickaxe, bumping into a diggable
            // tile will dig it out instead of just failing to move.
            if (const Item* mw = equippedMelee()) {
                if (mw->kind == ItemKind::Pickaxe && dung.isDiggable(nx, ny)) {
                    const TileType before = dung.at(nx, ny).type;
                    if (dung.dig(nx, ny)) {
                        emitNoise(e.pos, 14);

                        switch (before) {
                            case TileType::Wall:       pushMsg("YOU CHIP THROUGH THE WALL.", MessageKind::Info, true); break;
                            case TileType::Pillar:     pushMsg("YOU SHATTER THE PILLAR.", MessageKind::Info, true); break;
                            case TileType::DoorClosed:
                            case TileType::DoorLocked:
                            case TileType::DoorSecret: pushMsg("YOU SMASH THROUGH THE DOORFRAME.", MessageKind::Info, true); break;
                            default:                   pushMsg("YOU DIG.", MessageKind::Info, true); break;
                        }

                        recomputeFov();
                        return true; // consumes a turn via handleAction()
                    }
                }
            }

            pushMsg("YOU BUMP INTO A WALL.");
        }
        return false;
    }

    Vec2i prevPos = e.pos;
    bool moved = false;

    if (Entity* other = entityAtMut(nx, ny)) {
        if (other->id == e.id) return false;

        // Friendly swap: step into your dog (or let it step into you) to avoid getting stuck
        // in tight corridors. This also makes auto-travel much smoother with a companion.
        if (e.kind == EntityKind::Player && other->friendly) {
            if (other->effects.webTurns > 0) {
                pushMsg((other->kind == EntityKind::Dog) ? "YOUR DOG IS STUCK IN WEBBING!" : "YOUR COMPANION IS STUCK IN WEBBING!", MessageKind::Warning, true);
                return false;
            }
            other->pos = prevPos;
            e.pos = {nx, ny};
            moved = true;
        } else if (e.friendly && other->id == playerId_) {
            if (other->effects.webTurns > 0) {
                return false;
            }
            other->pos = prevPos;
            e.pos = {nx, ny};
            moved = true;
        }

        if (!moved) {
            if ((e.kind == EntityKind::Player || e.friendly) && other->kind == EntityKind::Shopkeeper && !other->alerted) {
                if (e.kind == EntityKind::Player) {
                    pushMsg("THE SHOPKEEPER SAYS: \"NO FIGHTING IN HERE!\"", MessageKind::Warning, true);
                }
                return false;
            }
            attackMelee(e, *other);
            return true;
        }
    } else {
        e.pos = {nx, ny};
        moved = true;
    }

    if (!moved) return false;

    if (e.kind == EntityKind::Player) {
        const bool wasInShop = (roomTypeAt(dung, prevPos) == RoomType::Shop);
        const bool nowInShop = (roomTypeAt(dung, e.pos) == RoomType::Shop);
        if (wasInShop && !nowInShop) {
            const int debt = shopDebtThisDepth();
            if (debt > 0 && anyPeacefulShopkeeper(ents, playerId_)) {
                setShopkeepersAlerted(ents, playerId_, e.pos, true);
                pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
            }
        }
        // Footstep noise: small, but enough for nearby monsters to investigate.
        // Scales a bit with encumbrance + armor clank.
        int vol = 4;
        if (encumbranceEnabled_) {
            switch (burdenState()) {
                case BurdenState::Unburdened: break;
                case BurdenState::Burdened:   vol += 1; break;
                case BurdenState::Stressed:   vol += 2; break;
                case BurdenState::Strained:   vol += 3; break;
                case BurdenState::Overloaded: vol += 4; break;
            }
        }
        if (const Item* a = equippedArmor()) {
            if (a->kind == ItemKind::ChainArmor) vol += 1;
            if (a->kind == ItemKind::PlateArmor) vol += 2;
        }

        if (isSneaking()) {
            // Sneaking can reduce footstep noise to near-silent levels, but
            // heavy armor / encumbrance still makes at least some noise.
            int reduce = 4 + std::min(2, playerAgility() / 4);
            vol -= reduce;

            int minVol = 0;
            if (encumbranceEnabled_) {
                switch (burdenState()) {
                    case BurdenState::Unburdened: break;
                    case BurdenState::Burdened:   minVol = std::max(minVol, 1); break;
                    case BurdenState::Stressed:   minVol = std::max(minVol, 1); break;
                    case BurdenState::Strained:   minVol = std::max(minVol, 1); break;
                    case BurdenState::Overloaded: minVol = std::max(minVol, 2); break;
                }
            }
            if (const Item* a = equippedArmor()) {
                if (a->kind == ItemKind::ChainArmor) minVol = std::max(minVol, 1);
                if (a->kind == ItemKind::PlateArmor) minVol = std::max(minVol, 2);
            }

            vol = clampi(vol, minVol, 14);
        } else {
            vol = clampi(vol, 2, 14);
        }

        if (vol > 0) {
            emitNoise(e.pos, vol);
        }

        // Convenience / QoL: auto-pickup when stepping on items.
        if (autoPickup != AutoPickupMode::Off) {
            (void)autoPickupAtPlayer();
        }
    }

    // Traps trigger on enter (monsters can trigger them too).
    triggerTrapAt(e.pos, e);

    return true;
}

Trap* Game::trapAtMut(int x, int y) {
    for (auto& t : trapsCur) {
        if (t.pos.x == x && t.pos.y == y) return &t;
    }
    return nullptr;
}

void Game::triggerTrapAt(Vec2i pos, Entity& victim, bool fromDisarm) {
    int tIndex = -1;
    for (size_t i = 0; i < trapsCur.size(); ++i) {
        if (trapsCur[i].pos == pos) {
            tIndex = static_cast<int>(i);
            break;
        }
    }
    if (tIndex < 0) return;
    Trap& t = trapsCur[static_cast<size_t>(tIndex)];

    const bool isPlayer = (victim.kind == EntityKind::Player);
    const bool tileVisible = dung.inBounds(pos.x, pos.y) && dung.at(pos.x, pos.y).visible;

    // You only "discover" a trap when you trigger it yourself, or when you can see it happen.
    if (isPlayer || tileVisible) {
        t.discovered = true;
    }

    auto msgIfSeen = [&](const std::string& s, MessageKind kind, bool fromPlayer = false) {
        if (isPlayer || tileVisible) {
            pushMsg(s, kind, fromPlayer);
        }
    };

    // Levitation lets you drift over some floor-based traps without triggering them.
    // (We only skip when you actually stepped onto the trap tile; disarm mishaps can still hurt.)
    if (!fromDisarm && victim.effects.levitationTurns > 0 && victim.pos == pos) {
        if (t.kind == TrapKind::Spike || t.kind == TrapKind::Web || t.kind == TrapKind::TrapDoor) {
            if (isPlayer) {
                pushMsg("YOU FLOAT OVER A TRAP.", MessageKind::Info, true);
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " FLOATS OVER A TRAP.";
                pushMsg(ss.str(), MessageKind::Info, false);
            }
            return;
        }
    }

    switch (t.kind) {
        case TrapKind::Spike: {
            int dmg = rng.range(2, 5) + std::min(3, depth_ / 2);
            victim.hp -= dmg;

            if (isPlayer) {
                std::ostringstream ss;
                if (fromDisarm) {
                    ss << "YOU SET OFF A SPIKE TRAP! YOU TAKE " << dmg << ".";
                } else {
                    ss << "YOU STEP ON A SPIKE TRAP! YOU TAKE " << dmg << ".";
                }
                pushMsg(ss.str(), MessageKind::Combat, false);
                if (victim.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "KILLED BY SPIKE TRAP";
                    gameOver = true;
                }
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " STEPS ON A SPIKE TRAP!";
                pushMsg(ss.str(), MessageKind::Combat, false);
                if (victim.hp <= 0) {
                    std::ostringstream ds;
                    ds << kindName(victim.kind) << " DIES.";
                    pushMsg(ds.str(), MessageKind::Combat, false);
                }
            }
            break;
        }
        case TrapKind::PoisonDart: {
            int dmg = rng.range(1, 2);
            victim.hp -= dmg;
            victim.effects.poisonTurns = std::max(victim.effects.poisonTurns, rng.range(6, 12));

            if (isPlayer) {
                std::ostringstream ss;
                ss << "A POISON DART HITS YOU! YOU TAKE " << dmg << ".";
                pushMsg(ss.str(), MessageKind::Combat, false);
                pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                if (victim.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "KILLED BY POISON DART TRAP";
                    gameOver = true;
                }
            } else if (tileVisible) {
                {
                    std::ostringstream ss;
                    ss << "A POISON DART HITS " << kindName(victim.kind) << "!";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                }
                if (victim.hp <= 0) {
                    std::ostringstream ds;
                    ds << kindName(victim.kind) << " DIES.";
                    pushMsg(ds.str(), MessageKind::Combat, false);
                } else {
                    std::ostringstream ps;
                    ps << kindName(victim.kind) << " IS POISONED!";
                    pushMsg(ps.str(), MessageKind::Warning, false);
                }
            }
            break;
        }
        case TrapKind::Teleport: {
            if (isPlayer) {
                pushMsg("A TELEPORT TRAP ACTIVATES!", MessageKind::Warning, false);
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " IS TELEPORTED!";
                pushMsg(ss.str(), MessageKind::Warning, false);
            }

            // Teleport to a random floor tile.
            Vec2i dst = dung.randomFloor(rng, true);
            for (int tries = 0; tries < 200; ++tries) {
                dst = dung.randomFloor(rng, true);
                if (!entityAt(dst.x, dst.y) && dst != dung.stairsUp && dst != dung.stairsDown) break;
            }

            Vec2i prevPos = victim.pos;
            victim.pos = dst;
            if (isPlayer) {
                recomputeFov();
                const bool wasInShop = (roomTypeAt(dung, prevPos) == RoomType::Shop);
                const bool nowInShop = (roomTypeAt(dung, victim.pos) == RoomType::Shop);
                if (wasInShop && !nowInShop) {
                    const int debt = shopDebtThisDepth();
                    if (debt > 0 && anyPeacefulShopkeeper(ents, playerId_)) {
                        setShopkeepersAlerted(ents, playerId_, victim.pos, true);
                        pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
                    }
                }
            }
            break;
        }
        case TrapKind::Alarm: {
            msgIfSeen("AN ALARM BLARES!", MessageKind::Warning, false);
            // Alert everything on the level to the alarm location.
            alertMonstersTo(pos, 0);
            break;
        }
        case TrapKind::Web: {
            const int turns = rng.range(4, 7) + std::min(6, depth_ / 2);
            victim.effects.webTurns = std::max(victim.effects.webTurns, turns);
            if (isPlayer) {
                pushMsg("YOU ARE CAUGHT IN STICKY WEBBING!", MessageKind::Warning, true);
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " IS CAUGHT IN STICKY WEBBING!";
                pushMsg(ss.str(), MessageKind::Warning, false);
            }
            break;
        }
        case TrapKind::ConfusionGas: {
            // Lingering confusion gas cloud. This trap creates a persistent, tile-based hazard
            // that slowly diffuses and dissipates over time.
            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (confusionGas_.size() != expect) confusionGas_.assign(expect, 0u);

            // Apply an immediate confusion hit to the victim (the cloud will keep it topped up).
            const int turns = rng.range(4, 7) + std::min(4, depth_ / 3);
            victim.effects.confusionTurns = std::max(victim.effects.confusionTurns, turns);

            // Seed the gas intensity in a small radius around the trap.
            const uint8_t baseStrength = static_cast<uint8_t>(clampi(8 + depth_ / 3, 8, 12));
            constexpr int radius = 2;

            std::vector<uint8_t> mask;
            dung.computeFovMask(pos.x, pos.y, radius, mask);

            const int minX = std::max(0, pos.x - radius);
            const int maxX = std::min(dung.width - 1, pos.x + radius);
            const int minY = std::max(0, pos.y - radius);
            const int maxY = std::min(dung.height - 1, pos.y + radius);

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const int dx = std::abs(x - pos.x);
                    const int dy = std::abs(y - pos.y);
                    const int dist = std::max(dx, dy);
                    if (dist > radius) continue;

                    const size_t i = static_cast<size_t>(y * dung.width + x);
                    if (i >= mask.size()) continue;
                    if (mask[i] == 0u) continue;
                    if (!dung.isWalkable(x, y)) continue;

                    const int s = static_cast<int>(baseStrength) - dist * 2;
                    if (s <= 0) continue;
                    const uint8_t ss = static_cast<uint8_t>(s);
                    if (confusionGas_[i] < ss) confusionGas_[i] = ss;
                }
            }

            if (isPlayer) {
                pushMsg("A NOXIOUS GAS SWIRLS AROUND YOU!", MessageKind::Warning, true);
                pushMsg("YOU FEEL CONFUSED!", MessageKind::Warning, true);
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " STAGGERS IN A NOXIOUS GAS CLOUD!";
                pushMsg(ss.str(), MessageKind::Warning, false);
            }

            // Gas traps are loud enough to draw attention.
            emitNoise(pos, 8);
            break;
        }
        case TrapKind::RollingBoulder: {
            // Rolling boulder trap: releases a heavy boulder that rolls in a straight line,
            // potentially crushing anything in its path. For simplicity this trap is single-use.

            if (isPlayer) {
                if (fromDisarm) pushMsg("CLICK! YOU SET OFF A ROLLING BOULDER TRAP!", MessageKind::Warning, true);
                else pushMsg("CLICK! YOU TRIGGER A ROLLING BOULDER TRAP!", MessageKind::Warning, true);
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << "CLICK! " << kindName(victim.kind) << " TRIGGERS A ROLLING BOULDER TRAP!";
                pushMsg(ss.str(), MessageKind::Warning, false);
            }

            // Loud enough to draw attention.
            emitNoise(pos, 16);

            auto sgn = [](int v) { return (v > 0) - (v < 0); };

            const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

            auto rollLenInDir = [&](Vec2i d) -> int {
                int len = 0;
                int x = pos.x;
                int y = pos.y;
                for (int step = 0; step < 24; ++step) {
                    x += d.x;
                    y += d.y;
                    if (!dung.inBounds(x, y)) break;

                    const TileType tt = dung.at(x, y).type;

                    // Avoid rolling onto stairs (blocking stairs is just annoying).
                    if (tt == TileType::StairsUp || tt == TileType::StairsDown) break;

                    // Hard blockers.
                    if (tt == TileType::Wall || tt == TileType::Pillar || tt == TileType::DoorSecret || tt == TileType::Boulder) break;

                    // Doors and chasms are valid next squares but stop the roll.
                    len += 1;
                    if (tt == TileType::DoorClosed || tt == TileType::DoorLocked || tt == TileType::Chasm) break;
                }
                return len;
            };

            // If the victim isn't on the trap tile (e.g. disarm mishap), bias the roll toward them.
            Vec2i preferred{0,0};
            if (victim.pos != pos) {
                if (victim.pos.x == pos.x) preferred = {0, sgn(victim.pos.y - pos.y)};
                else if (victim.pos.y == pos.y) preferred = {sgn(victim.pos.x - pos.x), 0};
                else if (std::abs(victim.pos.x - pos.x) >= std::abs(victim.pos.y - pos.y)) preferred = {sgn(victim.pos.x - pos.x), 0};
                else preferred = {0, sgn(victim.pos.y - pos.y)};
            }

            std::vector<Vec2i> bestDirs;
            int bestLen = -1;
            for (Vec2i d : dirs) {
                int len = rollLenInDir(d);
                if (len <= 0) continue;
                if (len > bestLen) {
                    bestLen = len;
                    bestDirs.clear();
                    bestDirs.push_back(d);
                } else if (len == bestLen) {
                    bestDirs.push_back(d);
                }
            }

            Vec2i rollDir{0,0};
            if (preferred.x != 0 || preferred.y != 0) {
                if (rollLenInDir(preferred) > 0) rollDir = preferred;
            }

            if (rollDir.x == 0 && rollDir.y == 0) {
                if (!bestDirs.empty()) {
                    rollDir = bestDirs[static_cast<size_t>(rng.range(0, static_cast<int>(bestDirs.size()) - 1))];
                } else {
                    // Nowhere to roll: drop in place.
                    rollDir = {1,0};
                }
            }

            auto canStand = [&](const Entity& e, Vec2i p) -> bool {
                if (!dung.inBounds(p.x, p.y)) return false;
                if (entityAt(p.x, p.y) != nullptr) return false;
                const TileType tt = dung.at(p.x, p.y).type;
                if (dung.isWalkable(p.x, p.y)) return true;
                if (tt == TileType::Chasm && e.effects.levitationTurns > 0) return true;
                return false;
            };

            auto scatterFrom = [&](Entity& e, Vec2i from, Vec2i dir) -> bool {
                // Prefer sideways relative to roll direction.
                Vec2i left{-dir.y, dir.x};
                Vec2i right{dir.y, -dir.x};
                Vec2i back{-dir.x, -dir.y};

                const Vec2i choices[8] = {
                    {from.x + left.x, from.y + left.y},
                    {from.x + right.x, from.y + right.y},
                    {from.x + back.x, from.y + back.y},
                    {from.x + left.x + back.x, from.y + left.y + back.y},
                    {from.x + right.x + back.x, from.y + right.y + back.y},
                    {from.x + left.x + dir.x, from.y + left.y + dir.y},
                    {from.x + right.x + dir.x, from.y + right.y + dir.y},
                    {from.x + dir.x, from.y + dir.y},
                };

                for (const Vec2i& p : choices) {
                    if (!canStand(e, p)) continue;
                    e.pos = p;
                    return true;
                }
                return false;
            };

            auto hitDmg = [&]() -> int {
                // Keep damage in a NetHack-like range; slightly scale with depth so it stays relevant.
                return rng.range(1, 20) + std::min(6, depth_);
            };

            auto applyBoulderHit = [&](Entity& e, bool isP) {
                const int dmg = hitDmg();
                e.hp -= dmg;

                if (isP) {
                    std::ostringstream ss;
                    ss << "A BOULDER CRUSHES YOU! YOU TAKE " << dmg << ".";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                    if (e.hp <= 0) {
                        pushMsg("YOU DIE.", MessageKind::Combat, false);
                        if (endCause_.empty()) endCause_ = "CRUSHED BY BOULDER TRAP";
                        gameOver = true;
                    }
                } else if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                    std::ostringstream ss;
                    ss << "A BOULDER CRUSHES " << kindName(e.kind) << "!";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                    if (e.hp <= 0) {
                        std::ostringstream ds;
                        ds << kindName(e.kind) << " DIES.";
                        pushMsg(ds.str(), MessageKind::Combat, false);
                    }
                }
            };

            bool playerMoved = false;

            // If the victim is on the trap square, they take the brunt of it and get shoved aside.
            if (victim.pos == pos) {
                applyBoulderHit(victim, isPlayer);
                if (victim.hp > 0) {
                    if (!scatterFrom(victim, pos, rollDir)) {
                        // Can't move them off the square: the trap jams after the hit.
                        trapsCur.erase(trapsCur.begin() + tIndex);
                        return;
                    }
                    if (isPlayer) playerMoved = true;
                }
            }

            // Spawn the boulder at the trap location (now empty), then roll it.
            if (dung.inBounds(pos.x, pos.y) && dung.at(pos.x, pos.y).type == TileType::Floor && entityAt(pos.x, pos.y) == nullptr) {
                dung.at(pos.x, pos.y).type = TileType::Boulder;
            }

            Vec2i bpos = pos;
            const int maxSteps = 24;
            for (int step = 0; step < maxSteps; ++step) {
                Vec2i nxt{ bpos.x + rollDir.x, bpos.y + rollDir.y };
                if (!dung.inBounds(nxt.x, nxt.y)) break;

                TileType tt = dung.at(nxt.x, nxt.y).type;

                if (tt == TileType::StairsUp || tt == TileType::StairsDown) break;

                // Hard blocks.
                if (tt == TileType::Wall || tt == TileType::Pillar || tt == TileType::DoorSecret || tt == TileType::Boulder) {
                    break;
                }

                // Doors: boulders can smash open some doors.
                if (tt == TileType::DoorClosed || tt == TileType::DoorLocked) {
                    float smashP = (tt == TileType::DoorClosed) ? 0.90f : 0.65f;
                    if (rng.chance(smashP)) {
                        if (tt == TileType::DoorLocked) dung.unlockDoor(nxt.x, nxt.y);
                        dung.openDoor(nxt.x, nxt.y);
                        if (dung.at(nxt.x, nxt.y).visible) {
                            pushMsg("A DOOR BURSTS OPEN!", MessageKind::System, false);
                        }
                        emitNoise(nxt, 14);
                        tt = dung.at(nxt.x, nxt.y).type;
                    } else {
                        // Can't break through.
                        break;
                    }
                }

                // Chasm: boulder fills it and disappears.
                if (tt == TileType::Chasm) {
                    dung.at(bpos.x, bpos.y).type = TileType::Floor;
                    dung.at(nxt.x, nxt.y).type = TileType::Floor;
                    if (dung.at(nxt.x, nxt.y).visible || tileVisible) {
                        pushMsg("THE BOULDER CRASHES INTO THE CHASM!", MessageKind::Info, false);
                    }
                    emitNoise(nxt, 18);
                    // Consumed.
                    bpos = nxt;
                    break;
                }

                // Check entity collision.
                Entity* hit = entityAtMut(nxt.x, nxt.y);
                if (hit) {
                    const bool hitIsPlayer = (hit->kind == EntityKind::Player);
                    applyBoulderHit(*hit, hitIsPlayer);
                    if (hit->hp > 0) {
                        if (!scatterFrom(*hit, hit->pos, rollDir)) {
                            // Can't move the victim: boulder stops.
                            break;
                        }
                        if (hitIsPlayer) playerMoved = true;
                    }
                }

                // Still blocked (couldn't scatter).
                if (entityAt(nxt.x, nxt.y) != nullptr) break;

                // Move boulder forward.
                dung.at(nxt.x, nxt.y).type = TileType::Boulder;
                dung.at(bpos.x, bpos.y).type = TileType::Floor;
                bpos = nxt;

                emitNoise(bpos, 12);
            }

            // Single-use: boulder traps are spent once triggered.
            trapsCur.erase(trapsCur.begin() + tIndex);

            if (playerMoved) recomputeFov();

            break;
        }

        case TrapKind::TrapDoor: {
            // Trap door: a hidden panel gives way, dropping the victim to the next dungeon level.
            // For simplicity this trap is single-use in this implementation.

            if (isPlayer) {
                if (fromDisarm) pushMsg("CLICK! A TRAP DOOR OPENS BENEATH YOU!", MessageKind::Warning, true);
                else pushMsg("A TRAP DOOR OPENS BENEATH YOU!", MessageKind::Warning, true);

                // Single-use: remove before changing levels so it persists correctly on the old floor.
                trapsCur.erase(trapsCur.begin() + tIndex);

                if (depth_ >= DUNGEON_MAX_DEPTH) {
                    pushMsg("THE TRAP DOOR SLAMS SHUT.", MessageKind::Info, true);
                    return;
                }

                // Falling is loud.
                emitNoise(pos, 18);

                // Drop the player to the next depth.
                const int dstDepth = depth_ + 1;
                changeLevel(dstDepth, true);

                // IMPORTANT: changeLevel may reallocate ents; reacquire the player reference.
                Entity& p = playerMut();

                // Land somewhere other than the stairs to avoid predictable pile-ups.
                Vec2i dst = dung.randomFloor(rng, true);
                for (int tries = 0; tries < 200; ++tries) {
                    dst = dung.randomFloor(rng, true);
                    if (dst == dung.stairsUp || dst == dung.stairsDown) continue;
                    if (entityAt(dst.x, dst.y) != nullptr) continue;
                    if (fireAt(dst.x, dst.y) > 0u) continue;

                    bool hasTrap = false;
                    for (const auto& tr : trapsCur) {
                        if (tr.pos == dst) { hasTrap = true; break; }
                    }
                    if (hasTrap) continue;

                    break;
                }

                p.pos = dst;
                recomputeFov();

                // Impact damage scales mildly with depth.
                const int dmg = rng.range(3, 7) + std::min(6, depth_ / 2);
                p.hp -= dmg;
                std::ostringstream ss;
                ss << "YOU LAND HARD! YOU TAKE " << dmg << ".";
                pushMsg(ss.str(), MessageKind::Combat, true);

                emitNoise(p.pos, 14);

                if (p.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "FELL THROUGH A TRAP DOOR";
                    gameOver = true;
                }

                return;
            } else {
                if (tileVisible) {
                    std::ostringstream ss;
                    ss << kindName(victim.kind) << " FALLS THROUGH A TRAP DOOR!";
                    pushMsg(ss.str(), MessageKind::Warning, false);
                }

                // Single-use.
                trapsCur.erase(trapsCur.begin() + tIndex);

                // Defensive: trap doors on the bottom floor should act as a dead-end.
                if (depth_ >= DUNGEON_MAX_DEPTH) {
                    pushMsg("YOU HEAR THE TRAP DOOR SLAM SHUT.", MessageKind::Info, false);
                    return;
                }

                const int dstDepth = depth_ + 1;

                // Falling hurts. (A lighter touch than the player's fall damage.)
                const int dmg = rng.range(2, 5) + (depth_ / 2);

                // Snapshot the creature before removing it from this level.
                Entity faller = victim;
                faller.hp = std::max(0, faller.hp - dmg);
                faller.pos = {-1, -1}; // resolved on arrival to the destination depth
                faller.energy = 0;

                const bool survived = (faller.hp > 0);

                if (survived) {
                    // Queue inter-level travel: the creature will appear on the level below
                    // the next time that depth is entered.
                    trapdoorFallers_[static_cast<size_t>(dstDepth)].push_back(faller);
                }

                // Audible feedback: even if you can't see it, you can hear something fall.
                if (survived) {
                    pushMsg("YOU HEAR A MUFFLED CRASH FROM BELOW.", MessageKind::Info, false);
                } else {
                    pushMsg("YOU HEAR A SICKENING THUD FROM BELOW.", MessageKind::Info, false);
                }

                // Remove the creature from this level without loot/corpse drops here.
                victim.hp = 0;
                victim.pos = {-1, -1};
                return;
            }
        }

        default:
            break;
    }
}

bool Game::searchForTraps(bool verbose, int* foundTrapsOut, int* foundSecretsOut) {
    Entity& p = playerMut();    // Searching is fairly quiet, but not silent.
    emitNoise(p.pos, 3);

    const int radius = 2;

    int foundTraps = 0;
    int foundSecrets = 0;
    float baseChance = 0.35f + 0.05f * static_cast<float>(charLevel);
    // Talents: Focus improves careful searching.
    baseChance += 0.02f * static_cast<float>(playerFocus());
    baseChance = std::min(0.90f, baseChance);

    for (auto& t : trapsCur) {
        if (t.discovered) continue;
        int dx = std::abs(t.pos.x - p.pos.x);
        int dy = std::abs(t.pos.y - p.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > radius) continue;

        float chance = baseChance;
        if (cheb <= 1) chance = std::min(0.95f, chance + 0.20f);
        if (rng.chance(chance)) {
            t.discovered = true;
            foundTraps += 1;
        }
    }

    // Trapped chests behave like traps for detection purposes.
    for (auto& gi : ground) {
        if (gi.item.kind != ItemKind::Chest) continue;
        if (!chestTrapped(gi.item)) continue;
        if (chestTrapKnown(gi.item)) continue;

        int dx = std::abs(gi.pos.x - p.pos.x);
        int dy = std::abs(gi.pos.y - p.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > radius) continue;

        float chance = baseChance;
        if (cheb <= 1) chance = std::min(0.95f, chance + 0.20f);
        if (rng.chance(chance)) {
            setChestTrapKnown(gi.item, true);
            foundTraps += 1;
        }
    }

    // Also search for secret doors in nearby walls.
    // Secret doors are encoded as TileType::DoorSecret and behave like walls until discovered.
    for (int y = p.pos.y - radius; y <= p.pos.y + radius; ++y) {
        for (int x = p.pos.x - radius; x <= p.pos.x + radius; ++x) {
            if (!dung.inBounds(x, y)) continue;
            Tile& t = dung.at(x, y);
            if (t.type != TileType::DoorSecret) continue;

            int dx = std::abs(x - p.pos.x);
            int dy = std::abs(y - p.pos.y);
            int cheb = std::max(dx, dy);
            if (cheb > radius) continue;

            float chance = std::max(0.10f, baseChance - 0.10f); // slightly harder than traps
            if (cheb <= 1) chance = std::min(0.95f, chance + 0.20f);

            if (rng.chance(chance)) {
                t.type = TileType::DoorClosed;
                t.explored = true;
                foundSecrets += 1;
            }
        }
    }

    if (foundTrapsOut) *foundTrapsOut = foundTraps;
    if (foundSecretsOut) *foundSecretsOut = foundSecrets;

    if (verbose) {
        if (foundTraps > 0 || foundSecrets > 0) {
            pushMsg(formatSearchDiscoveryMessage(foundTraps, foundSecrets), MessageKind::Info, true);
        } else {
            pushMsg("YOU SEARCH, BUT FIND NOTHING.", MessageKind::Info, true);
        }
    }


    return true; // Searching costs a turn.
}


bool Game::disarmTrap() {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();
    // Trapped chests can also be disarmed (when their trap is known).
    GroundItem* bestChest = nullptr;
    int bestChestDist = 999;
    for (auto& gi : ground) {
        if (gi.item.kind != ItemKind::Chest) continue;
        if (!chestTrapped(gi.item)) continue;
        if (!chestTrapKnown(gi.item)) continue;

        int dx = std::abs(gi.pos.x - p.pos.x);
        int dy = std::abs(gi.pos.y - p.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > 1) continue;

        if (cheb < bestChestDist) {
            bestChestDist = cheb;
            bestChest = &gi;
        }
    }

    // Choose the nearest discovered trap adjacent to the player (including underfoot).
    int bestIndex = -1;
    int bestDist = 999;
    for (size_t i = 0; i < trapsCur.size(); ++i) {
        const Trap& t = trapsCur[i];
        if (!t.discovered) continue;
        int dx = std::abs(t.pos.x - p.pos.x);
        int dy = std::abs(t.pos.y - p.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > 1) continue;
        if (cheb < bestDist) {
            bestDist = cheb;
            bestIndex = static_cast<int>(i);
        }
    }

    // Prefer the closest target. When distances tie, keep the original behavior
    // and disarm floor traps first.
    const bool targetIsChest = (bestChest != nullptr) &&
                               (bestIndex < 0 || bestChestDist < bestDist);

    if (bestIndex < 0 && !targetIsChest) {
        pushMsg("NO ADJACENT TRAP TO DISARM.", MessageKind::Info, true);
        return false;
    }

    auto trapName = [&](TrapKind k) -> const char* {
        switch (k) {
            case TrapKind::Spike: return "SPIKE";
            case TrapKind::PoisonDart: return "POISON DART";
            case TrapKind::Teleport: return "TELEPORT";
            case TrapKind::Alarm: return "ALARM";
            case TrapKind::Web: return "WEB";
            case TrapKind::ConfusionGas: return "CONFUSION GAS";
            case TrapKind::RollingBoulder: return "ROLLING BOULDER";
            case TrapKind::TrapDoor: return "TRAP DOOR";
        }
        return "TRAP";
    };

    // --- Chest trap disarm ---
    if (targetIsChest) {
        Item& chest = bestChest->item;
        emitNoise(bestChest->pos, 5);
        const TrapKind tk = chestTrapKind(chest);
        const int tier = chestTier(chest);

        const bool hasPicks = (lockpickCount() > 0);

        // Slightly harder than floor traps; higher-tier chests are also tougher.
        float chance = 0.25f + 0.04f * static_cast<float>(charLevel);
        // Talents: Agility improves delicate work.
        chance += 0.02f * static_cast<float>(playerAgility());
        chance = std::min(0.85f, chance);
        chance -= 0.05f * static_cast<float>(tier);
        if (hasPicks) chance = std::min(0.95f, chance + 0.20f);

        if (tk == TrapKind::Teleport) chance *= 0.85f;
        if (tk == TrapKind::Alarm) chance *= 0.90f;
        if (tk == TrapKind::Web) chance *= 0.95f;
        if (tk == TrapKind::ConfusionGas) chance *= 0.97f;


        if (rng.chance(chance)) {
            setChestTrapped(chest, false);
            setChestTrapKnown(chest, true);
            std::ostringstream ss;
            ss << "YOU DISARM THE CHEST'S " << trapName(tk) << " TRAP.";
            pushMsg(ss.str(), MessageKind::Success, true);
            return true;
        }

        {
            std::ostringstream ss;
            ss << "YOU FAIL TO DISARM THE CHEST'S " << trapName(tk) << " TRAP.";
            pushMsg(ss.str(), MessageKind::Warning, true);
        }

        // Mishaps: lockpicks can break, and you may set off the trap.
        if (hasPicks && rng.chance(0.20f)) {
            consumeLockpicks(1);
            pushMsg("YOUR LOCKPICK BREAKS!", MessageKind::Warning, true);
        }

        float setOffChance = 0.18f + 0.05f * static_cast<float>(tier);
        if (tk == TrapKind::Alarm) setOffChance += 0.10f;
        if (tk == TrapKind::Teleport) setOffChance += 0.06f;
        if (tk == TrapKind::Web) setOffChance += 0.04f;
        if (tk == TrapKind::ConfusionGas) setOffChance += 0.03f;

        if (rng.chance(setOffChance)) {
            pushMsg("YOU SET OFF THE CHEST TRAP!", MessageKind::Warning, true);

            // Chest traps are single-use.
            setChestTrapped(chest, false);
            setChestTrapKnown(chest, true);

            switch (tk) {
                case TrapKind::Spike: {
                    int dmg = rng.range(2, 5) + std::min(3, depth_ / 2);
                    p.hp -= dmg;
                    std::ostringstream ss;
                    ss << "NEEDLES JAB YOU! YOU TAKE " << dmg << ".";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                    if (p.hp <= 0) {
                        pushMsg("YOU DIE.", MessageKind::Combat, false);
                        if (endCause_.empty()) endCause_ = "KILLED BY CHEST TRAP";
                        gameOver = true;
                    }
                    break;
                }
                case TrapKind::PoisonDart: {
                    int dmg = rng.range(1, 2);
                    p.hp -= dmg;
                    p.effects.poisonTurns = std::max(p.effects.poisonTurns, rng.range(6, 12));
                    std::ostringstream ss;
                    ss << "POISON NEEDLES HIT YOU! YOU TAKE " << dmg << ".";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                    pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                    if (p.hp <= 0) {
                        pushMsg("YOU DIE.", MessageKind::Combat, false);
                        if (endCause_.empty()) endCause_ = "KILLED BY POISON CHEST TRAP";
                        gameOver = true;
                    }
                    break;
                }
                case TrapKind::Teleport: {
                    pushMsg("A TELEPORT GLYPH FLARES!", MessageKind::Warning, false);
                    Vec2i dst = dung.randomFloor(rng, true);
                    for (int tries = 0; tries < 200; ++tries) {
                        dst = dung.randomFloor(rng, true);
                        if (!entityAt(dst.x, dst.y) && dst != dung.stairsUp && dst != dung.stairsDown) break;
                    }
                    Vec2i prevPos = p.pos;
                    p.pos = dst;
                    recomputeFov();
                    const bool wasInShop = (roomTypeAt(dung, prevPos) == RoomType::Shop);
                    const bool nowInShop = (roomTypeAt(dung, p.pos) == RoomType::Shop);
                    if (wasInShop && !nowInShop) {
                        const int debt = shopDebtThisDepth();
                        if (debt > 0 && anyPeacefulShopkeeper(ents, playerId_)) {
                            setShopkeepersAlerted(ents, playerId_, p.pos, true);
                            pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
                        }
                    }
                    break;
                }
                case TrapKind::Alarm: {
                    pushMsg("AN ALARM BLARES!", MessageKind::Warning, false);
                    // The noise comes from the chest.
                    alertMonstersTo(bestChest->pos, 0);
                    break;
                }
                case TrapKind::Web: {
                    const int turns = rng.range(4, 7) + std::min(6, depth_ / 2);
                    p.effects.webTurns = std::max(p.effects.webTurns, turns);
                    pushMsg("STICKY WEBBING EXPLODES OUT!", MessageKind::Warning, true);
                    break;
                }
                default:
                    break;
            }
        }

        return true; // Disarming costs a turn.
    }

    // --- Floor trap disarm ---
    Trap& tr = trapsCur[static_cast<size_t>(bestIndex)];
    emitNoise(tr.pos, 5);

    const bool hasPicks = (lockpickCount() > 0);

    // Base chance scales with level. Tools help a lot, but magical traps are still tricky.
    float chance = 0.33f + 0.04f * static_cast<float>(charLevel);
    // Talents: Agility improves disarming.
    chance += 0.02f * static_cast<float>(playerAgility());
    chance = std::min(0.90f, chance);
    if (hasPicks) chance = std::min(0.95f, chance + 0.15f);

    if (tr.kind == TrapKind::Teleport) chance *= 0.85f;
    if (tr.kind == TrapKind::Alarm) chance *= 0.90f;
    if (tr.kind == TrapKind::RollingBoulder) chance *= 0.80f;
    if (tr.kind == TrapKind::TrapDoor) chance *= 0.82f;

    chance = std::max(0.05f, chance);

    if (rng.chance(chance)) {
        std::ostringstream ss;
        ss << "YOU DISARM THE " << trapName(tr.kind) << " TRAP.";
        pushMsg(ss.str(), MessageKind::Success, true);
        trapsCur.erase(trapsCur.begin() + bestIndex);
        return true;
    }

    {
        std::ostringstream ss;
        ss << "YOU FAIL TO DISARM THE " << trapName(tr.kind) << " TRAP.";
        pushMsg(ss.str(), MessageKind::Warning, true);
    }

    // Mishaps: lockpicks can break, and sometimes you set the trap off.
    if (hasPicks && rng.chance(0.15f)) {
        consumeLockpicks(1);
        pushMsg("YOUR LOCKPICK BREAKS!", MessageKind::Warning, true);
    }

    float setOffChance = 0.15f;
    if (tr.kind == TrapKind::Alarm) setOffChance = 0.25f;
    if (tr.kind == TrapKind::Web) setOffChance = 0.20f;
    if (tr.kind == TrapKind::ConfusionGas) setOffChance = 0.18f;
    if (tr.kind == TrapKind::RollingBoulder) setOffChance = 0.22f;
    if (tr.kind == TrapKind::TrapDoor) setOffChance = 0.24f;

    if (rng.chance(setOffChance)) {
        pushMsg("YOU SET OFF THE TRAP!", MessageKind::Warning, true);
        triggerTrapAt(tr.pos, p, true);
    }

    return true; // Disarming costs a turn.
}


bool Game::closeDoor() {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();
    struct Off { int dx, dy; };
    // Prefer cardinal directions (closing diagonals feels odd and can be ambiguous).
    const Off dirs[4] = { {0,-1}, {0,1}, {-1,0}, {1,0} };

    int doorX = -1;
    int doorY = -1;
    bool sawBlockedDoor = false;

    for (const auto& d : dirs) {
        int x = p.pos.x + d.dx;
        int y = p.pos.y + d.dy;
        if (!dung.inBounds(x, y)) continue;
        if (dung.at(x, y).type != TileType::DoorOpen) continue;

        // Can't close a door if something is standing in the doorway.
        if (entityAt(x, y) != nullptr) {
            sawBlockedDoor = true;
            continue;
        }

        doorX = x;
        doorY = y;
        break;
    }

    if (doorX < 0 || doorY < 0) {
        if (sawBlockedDoor) {
            pushMsg("THE DOORWAY IS BLOCKED.", MessageKind::Warning, true);
        } else {
            pushMsg("NO ADJACENT OPEN DOOR TO CLOSE.", MessageKind::Info, true);
        }
        return false;
    }

    dung.closeDoor(doorX, doorY);
    pushMsg("YOU CLOSE THE DOOR.", MessageKind::System, true);
    emitNoise({doorX, doorY}, 8);
    return true; // Closing a door costs a turn.
}

bool Game::lockDoor() {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();
    struct Off { int dx, dy; };
    // Prefer cardinal directions for door interactions.
    const Off dirs[4] = { {0,-1}, {0,1}, {-1,0}, {1,0} };

    int closedX = -1;
    int closedY = -1;
    int openX = -1;
    int openY = -1;

    bool sawBlockedDoor = false;
    bool sawLockedDoor = false;

    for (const auto& d : dirs) {
        int x = p.pos.x + d.dx;
        int y = p.pos.y + d.dy;
        if (!dung.inBounds(x, y)) continue;

        TileType tt = dung.at(x, y).type;

        if (tt == TileType::DoorLocked) {
            sawLockedDoor = true;
            continue;
        }

        if (tt == TileType::DoorClosed) {
            closedX = x;
            closedY = y;
            break; // prefer closed doors
        }

        if (tt == TileType::DoorOpen) {
            // Can't lock a door if something is standing in the doorway.
            if (entityAt(x, y) != nullptr) {
                sawBlockedDoor = true;
                continue;
            }
            // Save as fallback in case no closed door is adjacent.
            if (openX < 0) {
                openX = x;
                openY = y;
            }
        }
    }

    int doorX = closedX;
    int doorY = closedY;
    bool wasOpen = false;

    if (doorX < 0 || doorY < 0) {
        if (openX >= 0 && openY >= 0) {
            doorX = openX;
            doorY = openY;
            wasOpen = true;
        }
    }

    if (doorX < 0 || doorY < 0) {
        if (sawBlockedDoor) {
            pushMsg("THE DOORWAY IS BLOCKED.", MessageKind::Warning, true);
        } else if (sawLockedDoor) {
            pushMsg("THE DOOR IS ALREADY LOCKED.", MessageKind::Info, true);
        } else {
            pushMsg("NO ADJACENT DOOR TO LOCK.", MessageKind::Info, true);
        }
        return false;
    }

    if (!consumeKeys(1)) {
        pushMsg("YOU HAVE NO KEYS.", MessageKind::Warning, true);
        return false;
    }

    if (wasOpen) {
        dung.closeDoor(doorX, doorY);
    }

    dung.lockDoor(doorX, doorY);

    if (wasOpen) {
        pushMsg("YOU CLOSE AND LOCK THE DOOR.", MessageKind::System, true);
    } else {
        pushMsg("YOU LOCK THE DOOR.", MessageKind::System, true);
    }

    emitNoise({doorX, doorY}, 8);

    return true; // Locking costs a turn.
}


void Game::beginKick() {
    if (gameOver || gameWon) return;

    // Close other overlays/modes.
    invOpen = false;
    invIdentifyMode = false;
    targeting = false;
    looking = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    optionsOpen = false;

    if (commandOpen) {
        commandOpen = false;
        commandBuf.clear();
        commandDraft.clear();
        commandHistoryPos = -1;
    }

    msgScroll = 0;

    kicking = true;
    pushMsg("KICK IN WHICH DIRECTION?", MessageKind::System, true);
}

bool Game::kickInDirection(int dx, int dy) {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();

    dx = clampi(dx, -1, 1);
    dy = clampi(dy, -1, 1);
    if (dx == 0 && dy == 0) return false;

    // Confusion can scramble the kick direction.
    if (p.effects.confusionTurns > 0) {
        static const int dirs[8][2] = { {0,-1},{0,1},{-1,0},{1,0},{-1,-1},{1,-1},{-1,1},{1,1} };
        const int i = rng.range(0, 7);
        dx = dirs[i][0];
        dy = dirs[i][1];
        pushMsg("YOU FLAIL IN CONFUSION!", MessageKind::Warning, true);
    }

    // Prevent kicking diagonally "through" a blocked corner.
    if (dx != 0 && dy != 0 && !diagonalPassable(dung, p.pos, dx, dy)) {
        pushMsg("YOU CAN'T REACH AROUND THE CORNER.", MessageKind::Info, true);
        return false;
    }

    const Vec2i tgt{ p.pos.x + dx, p.pos.y + dy };
    if (!dung.inBounds(tgt.x, tgt.y)) {
        pushMsg("YOU KICK THE AIR.", MessageKind::Info, true);
        emitNoise(p.pos, 6);
        return true;
    }

    // Kicking is noisy even if it hits nothing useful.
    auto baseNoise = [&]() {
        emitNoise(tgt, 10);
    };

    // First, kicking a creature.
    if (Entity* e = entityAtMut(tgt.x, tgt.y)) {
        if (e->id == p.id) return false;
        if (e->friendly) {
            if (e->kind == EntityKind::Dog) pushMsg("YOU CAN'T BRING YOURSELF TO KICK YOUR DOG.", MessageKind::Info, true);
            else pushMsg("YOU CAN'T BRING YOURSELF TO KICK YOUR COMPANION.", MessageKind::Info, true);
            return false;
        }
        baseNoise();
        attackMelee(p, *e, true);
        return true;
    }

    // Next, kicking a chest on the ground.
    GroundItem* chestGi = nullptr;
    for (auto& gi : ground) {
        if (gi.pos == tgt && gi.item.kind == ItemKind::Chest) { chestGi = &gi; break; }
    }

    if (chestGi) {
        Item& chest = chestGi->item;

        // Mimic reveal.
        if (chestMimic(chest)) {
            // Remove the chest.
            const int chestId = chest.id;
            ground.erase(std::remove_if(ground.begin(), ground.end(), [&](const GroundItem& gi) {
                return gi.pos == tgt && gi.item.id == chestId;
            }), ground.end());

            pushMsg("THE CHEST WAS A MIMIC!", MessageKind::Warning, true);
            emitNoise(tgt, 14);

            Entity m;
            m.id = nextEntityId++;
            m.kind = EntityKind::Mimic;
            m.speed = baseSpeedFor(m.kind);
            m.energy = 0;
            m.pos = tgt;
            m.spriteSeed = rng.nextU32();
            m.groupId = 0;
            m.hpMax = 16;
            m.baseAtk = 4;
            m.baseDef = 2;
            m.willFlee = false;

            // Depth scaling.
            int dd = std::max(0, depth_ - 1);
            if (dd > 0) {
                m.hpMax += dd;
                m.baseAtk += dd / 3;
                m.baseDef += dd / 4;
            }
            m.hp = m.hpMax;
            m.alerted = true;
            m.lastKnownPlayerPos = p.pos;
            m.lastKnownPlayerAge = 0;

            ents.push_back(m);
            return true;
        }

        // Kick impact noise.
        baseNoise();

        // Trapped chest: kicking can set it off.
        if (chestTrapped(chest)) {
            // Reuse the chest trap logic used for opening.
            // This consumes the trap but does not open the chest.
            const TrapKind tk = chestTrapKind(chest);
            setChestTrapped(chest, false);
            setChestTrapKnown(chest, true);

            switch (tk) {
                case TrapKind::Spike: {
                    int dmg = rng.range(2, 5) + std::min(3, depth_ / 2);
                    p.hp -= dmg;
                    std::ostringstream ss;
                    ss << "A NEEDLE TRAP JABS YOU! YOU TAKE " << dmg << ".";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                    if (p.hp <= 0) {
                        pushMsg("YOU DIE.", MessageKind::Combat, false);
                        if (endCause_.empty()) endCause_ = "KILLED BY CHEST TRAP";
                        gameOver = true;
                        return true;
                    }
                    break;
                }
                case TrapKind::PoisonDart: {
                    int dmg = rng.range(1, 2);
                    p.hp -= dmg;
                    p.effects.poisonTurns = std::max(p.effects.poisonTurns, rng.range(6, 12));
                    std::ostringstream ss;
                    ss << "POISON NEEDLES HIT YOU! YOU TAKE " << dmg << ".";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                    pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                    if (p.hp <= 0) {
                        pushMsg("YOU DIE.", MessageKind::Combat, false);
                        if (endCause_.empty()) endCause_ = "KILLED BY POISON CHEST TRAP";
                        gameOver = true;
                        return true;
                    }
                    break;
                }
                case TrapKind::Teleport: {
                    pushMsg("A TELEPORT GLYPH FLARES FROM THE CHEST!", MessageKind::Warning, false);
                    Vec2i dst = dung.randomFloor(rng, true);
                    for (int tries = 0; tries < 200; ++tries) {
                        dst = dung.randomFloor(rng, true);
                        if (!entityAt(dst.x, dst.y) && dst != dung.stairsUp && dst != dung.stairsDown) break;
                    }
                    p.pos = dst;
                    recomputeFov();
                    break;
                }
                case TrapKind::Alarm: {
                    pushMsg("AN ALARM BLARES FROM THE CHEST!", MessageKind::Warning, false);
                    alertMonstersTo(tgt, 0);
                    break;
                }
                case TrapKind::Web: {
                    const int turns = rng.range(4, 7) + std::min(6, depth_ / 2);
                    p.effects.webTurns = std::max(p.effects.webTurns, turns);
                    pushMsg("STICKY WEBBING EXPLODES OUT OF THE CHEST!", MessageKind::Warning, true);
                    break;
                }
                case TrapKind::ConfusionGas: {
                    const int turns = rng.range(8, 14) + std::min(6, depth_ / 2);
                    p.effects.confusionTurns = std::max(p.effects.confusionTurns, turns);
                    pushMsg("A NOXIOUS GAS BURSTS FROM THE CHEST!", MessageKind::Warning, true);
                    pushMsg("YOU FEEL CONFUSED!", MessageKind::Warning, true);
                    emitNoise(tgt, 8);
                    break;
                }
                default:
                    break;
            }
        }

        if (gameOver) return true;

        // Bashing a lock: higher-tier chests are sturdier.
        if (chestLocked(chest)) {
            float chance = 0.18f + 0.04f * static_cast<float>(playerMight());
            chance += 0.02f * static_cast<float>(charLevel);
            chance -= 0.06f * static_cast<float>(chestTier(chest));
            chance = std::clamp(chance, 0.03f, 0.75f);

            if (rng.chance(chance)) {
                setChestLocked(chest, false);
                pushMsg("YOU BASH THE CHEST'S LOCK OPEN!", MessageKind::Success, true);
            } else {
                pushMsg("THE CHEST'S LOCK HOLDS.", MessageKind::Info, true);
            }
        }

        // Try to slide the chest one tile.
        const Vec2i dst{ tgt.x + dx, tgt.y + dy };
        if (dung.inBounds(dst.x, dst.y) && dung.isWalkable(dst.x, dst.y) && !entityAt(dst.x, dst.y)
            && dst != dung.stairsUp && dst != dung.stairsDown) {
            chestGi->pos = dst;
            pushMsg("YOU KICK THE CHEST. IT SLIDES!", MessageKind::Info, true);
        } else {
            pushMsg("THUD!", MessageKind::Info, true);
        }

        return true;
    }

    // Doors and secret doors.
    Tile& t = dung.at(tgt.x, tgt.y);
    if (t.type == TileType::DoorClosed) {
        dung.openDoor(tgt.x, tgt.y);
        pushMsg("YOU KICK OPEN THE DOOR.", MessageKind::Info, true);
        emitNoise(tgt, 14);
        return true;
    }
    if (t.type == TileType::DoorLocked) {
        float chance = 0.20f + 0.05f * static_cast<float>(playerMight());
        chance += 0.02f * static_cast<float>(charLevel);
        chance = std::clamp(chance, 0.05f, 0.85f);

        if (rng.chance(chance)) {
            dung.unlockDoor(tgt.x, tgt.y);
            dung.openDoor(tgt.x, tgt.y);
            pushMsg("YOU SMASH THE LOCKED DOOR OPEN!", MessageKind::Success, true);
        } else {
            pushMsg("THE LOCKED DOOR HOLDS.", MessageKind::Warning, true);
            // A hard kick can hurt.
            if (rng.chance(0.35f)) {
                p.hp -= 1;
                pushMsg("OUCH! YOU HURT YOUR FOOT.", MessageKind::Warning, true);
                if (p.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "KILLED BY A BROKEN TOE";
                    gameOver = true;
                }
            }
        }

        emitNoise(tgt, 16);
        return true;
    }
    if (t.type == TileType::DoorSecret) {
        float chance = 0.25f + 0.05f * static_cast<float>(playerMight());
        chance = std::clamp(chance, 0.05f, 0.80f);
        if (rng.chance(chance)) {
            t.type = TileType::DoorClosed;
            t.explored = true;
            pushMsg("YOU HEAR A HOLLOW SOUND.", MessageKind::Success, true);
        } else {
            pushMsg("THUD.", MessageKind::Info, true);
        }
        emitNoise(tgt, 10);
        return true;
    }
    if (t.type == TileType::DoorOpen) {
        pushMsg("IT'S ALREADY OPEN.", MessageKind::Info, true);
        return false;
    }

    // Otherwise, just kick whatever is there.
    if (!dung.isWalkable(tgt.x, tgt.y)) {
        pushMsg("THUD!", MessageKind::Info, true);
        emitNoise(tgt, 8);
        // Small chance to hurt yourself when kicking solid stone.
        if (rng.chance(0.20f)) {
            p.hp -= 1;
            pushMsg("OUCH!", MessageKind::Warning, true);
            if (p.hp <= 0) {
                pushMsg("YOU DIE.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = "KILLED BY A BROKEN TOE";
                gameOver = true;
            }
        }
        return true;
    }

    pushMsg("YOU KICK THE GROUND.", MessageKind::Info, true);
    emitNoise(tgt, 6);
    return true;
}


bool Game::prayAtShrine(const std::string& modeIn) {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();

    auto hasCursedEquipped = [&]() {
        for (const auto& it : inv) {
            if (it.id != equipMeleeId && it.id != equipRangedId && it.id != equipArmorId && it.id != equipRing1Id && it.id != equipRing2Id) continue;
            if (it.buc < 0) return true;
        }
        return false;
    };

    auto rechargeableWandIndices = [&]() {
        std::vector<int> idxs;
        idxs.reserve(8);
        for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
            const Item& it = inv[static_cast<size_t>(i)];
            if (!isWandKind(it.kind)) continue;
            const ItemDef& d = itemDef(it.kind);
            if (d.maxCharges <= 0) continue;
            if (it.charges < d.maxCharges) idxs.push_back(i);
        }
        return idxs;
    };

    auto hasRechargeableWand = [&]() {
        auto idxs = rechargeableWandIndices();
        return !idxs.empty();
    };

    auto unidentifiedKinds = [&]() {
        std::vector<ItemKind> out;
        out.reserve(16);
        auto seen = [&](ItemKind k) {
            for (ItemKind x : out) if (x == k) return true;
            return false;
        };
        for (const auto& it : inv) {
            if (!isIdentifiableKind(it.kind)) continue;
            if (isIdentified(it.kind)) continue;
            if (!seen(it.kind)) out.push_back(it.kind);
        }
        return out;
    };

    auto blessableIndices = [&]() {
        std::vector<int> idxs;
        idxs.reserve(16);
        for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
            const Item& it = inv[static_cast<size_t>(i)];
            if (it.kind == ItemKind::Gold) continue;
            if (it.kind == ItemKind::AmuletYendor) continue;
            const ItemDef& d = itemDef(it.kind);
            const bool gear = (d.slot != EquipSlot::None);
            const bool consumable = d.consumable;
            if (gear || consumable) idxs.push_back(i);
        }
        return idxs;
    };

    bool inShrine = false;
    for (const auto& r : dung.rooms) {
        if (r.type == RoomType::Shrine && r.contains(p.pos.x, p.pos.y)) { inShrine = true; break; }
    }
    if (!inShrine) {
        pushMsg("YOU ARE NOT IN A SHRINE.", MessageKind::Info, true);
        return false;
    }

    std::string mode = toLower(trim(modeIn));
    if (mode == "charge") mode = "recharge";

    if (!mode.empty()) {
        if (mode != "heal" && mode != "cure" && mode != "identify" && mode != "bless" && mode != "uncurse" && mode != "recharge") {
            pushMsg("UNKNOWN PRAYER: " + mode + ". TRY: heal / cure / identify / bless / uncurse / recharge", MessageKind::Info, true);
            return false;
        }
    } else {
        // Auto-pick a sensible prayer.
        if (p.effects.poisonTurns > 0 || p.effects.webTurns > 0 || p.effects.confusionTurns > 0 || p.effects.burnTurns > 0) mode = "cure";
        else if (p.hp < p.hpMax) mode = "heal";
        else if (hasCursedEquipped()) mode = "uncurse";
        else if (hasRechargeableWand()) mode = "recharge";
        else if (identifyItemsEnabled && !unidentifiedKinds().empty()) mode = "identify";
        else mode = "bless";
    }

    if (mode == "identify" && !identifyItemsEnabled) {
        pushMsg("DIVINE IDENTIFICATION IS DISABLED.", MessageKind::Info, true);
        return false;
    }

    // If the player explicitly requests a service that can't do anything, don't charge them.
    if (mode == "identify") {
        if (unidentifiedKinds().empty()) {
            pushMsg("YOU LEARN NOTHING NEW.", MessageKind::Info, true);
            return false;
        }
    }
    if (mode == "recharge") {
        if (rechargeableWandIndices().empty()) {
            pushMsg("YOU HAVE NOTHING TO RECHARGE.", MessageKind::Info, true);
            return false;
        }
    }

    const int base = 10 + depth_ * 2;
    int cost = 0;
    if (mode == "heal") cost = base + 6;
    else if (mode == "cure") cost = base + 8;
    else if (mode == "identify") cost = base + 10;
    else if (mode == "bless") cost = base + 12;
    else if (mode == "uncurse") cost = base + 14;
    else if (mode == "recharge") cost = base + 16;

    if (goldCount() < cost) {
        pushMsg("YOU LACK THE GOLD FOR THAT.", MessageKind::Info, true);
        return false;
    }

    // Spend gold now; selection prompts (if any) are UI-only and do not consume extra turns.
    (void)spendGoldFromInv(inv, cost);
    pushMsg("YOU OFFER " + std::to_string(cost) + " GOLD.", MessageKind::Info, true);

    if (mode == "heal") {
        int healed = std::max(8, p.hpMax / 2);
        p.hp = std::min(p.hp + healed, p.hpMax);
        pushMsg("DIVINE LIGHT MENDS YOUR WOUNDS.", MessageKind::Success, true);
    } else if (mode == "cure") {
        p.effects.poisonTurns = 0;
        p.effects.webTurns = 0;
        p.effects.confusionTurns = 0;
        p.effects.burnTurns = 0;
        pushMsg("YOU FEEL PURIFIED.", MessageKind::Success, true);
    } else if (mode == "identify") {
        std::vector<ItemKind> c = unidentifiedKinds();
        if (c.size() == 1) {
            (void)markIdentified(c[0], false);
            pushMsg("DIVINE INSIGHT REVEALS THE TRUTH.", MessageKind::Success, true);
        } else {
            openInventory();
            invPrompt_ = InvPromptKind::ShrineIdentify;
            // Prefer selecting the first unidentified item.
            for (size_t i = 0; i < inv.size(); ++i) {
                if (isIdentifiableKind(inv[i].kind) && !isIdentified(inv[i].kind)) { invSel = static_cast<int>(i); break; }
            }
            pushMsg("SELECT AN ITEM TO IDENTIFY (ENTER=CHOOSE, ESC=RANDOM).", MessageKind::System, true);
        }
    } else if (mode == "bless") {
        // Defensive buffs.
        p.effects.shieldTurns = std::max(p.effects.shieldTurns, 80);
        p.effects.regenTurns  = std::max(p.effects.regenTurns, 120);

        auto blessOne = [&](Item& it) {
            Item named = it;
            named.buc = 0;
            const std::string nm = displayItemName(named);

            if (it.buc < 0) {
                it.buc = 0;
                pushMsg("A WARMTH LIFTS THE CURSE FROM YOUR " + nm + ".", MessageKind::Success, true);
            } else if (it.buc == 0) {
                it.buc = 1;
                pushMsg("YOUR " + nm + " GLOWS WITH HOLY LIGHT.", MessageKind::Success, true);
            } else {
                pushMsg("YOUR " + nm + " SHINES BRIEFLY.", MessageKind::Info, true);
            }
        };

        std::vector<int> idxs = blessableIndices();
        if (idxs.size() == 1) {
            blessOne(inv[static_cast<size_t>(idxs[0])]);
        } else if (!idxs.empty()) {
            openInventory();
            invPrompt_ = InvPromptKind::ShrineBless;
            invSel = idxs[0];
            pushMsg("SELECT AN ITEM TO BLESS (ENTER=CHOOSE, ESC=EQUIPPED).", MessageKind::System, true);
        }

        pushMsg("A HOLY AURA SURROUNDS YOU.", MessageKind::Success, true);
    } else if (mode == "uncurse") {
        bool any = false;
        for (auto& it : inv) {
            if (it.id != equipMeleeId && it.id != equipRangedId && it.id != equipArmorId && it.id != equipRing1Id && it.id != equipRing2Id) continue;
            if (it.buc < 0) {
                it.buc = 0;
                any = true;
            }
        }
        pushMsg(any ? "A WEIGHT LIFTS FROM YOUR GEAR." : "YOU FEEL REASSURED.", MessageKind::Success, true);
    } else if (mode == "recharge") {
        auto rechargeOne = [&](Item& it) {
            const ItemDef& d = itemDef(it.kind);
            const int before = it.charges;
            if (d.maxCharges <= 0) return;

            if (it.buc < 0) it.buc = 0;
            it.charges = d.maxCharges;

            Item named = it;
            named.buc = 0;
            const std::string nm = displayItemName(named);

            if (before < d.maxCharges) {
                pushMsg("DIVINE ENERGY FLOWS INTO YOUR " + nm + ".", MessageKind::Success, true);
            } else {
                pushMsg("YOUR " + nm + " IS ALREADY FULLY CHARGED.", MessageKind::Info, true);
            }
        };

        std::vector<int> wands = rechargeableWandIndices();
        if (wands.size() == 1) {
            rechargeOne(inv[static_cast<size_t>(wands[0])]);
        } else {
            openInventory();
            invPrompt_ = InvPromptKind::ShrineRecharge;
            invSel = wands.empty() ? 0 : wands[0];
            pushMsg("SELECT A WAND TO RECHARGE (ENTER=CHOOSE, ESC=BEST).", MessageKind::System, true);
        }
    }

    advanceAfterPlayerAction();
    return true;
}
bool Game::payAtShop() {
    if (gameOver || gameWon) return false;

    if (!playerInShop()) {
        pushMsg("YOU ARE NOT IN A SHOP.", MessageKind::Warning, true);
        return false;
    }

    if (!anyLivingShopkeeper(ents, playerId_)) {
        pushMsg("NO ONE IS HERE TO TAKE YOUR MONEY.", MessageKind::Info, true);
        return false;
    }

    const int debt = shopDebtThisDepth();
    if (debt <= 0) {
        pushMsg("YOU OWE NOTHING.", MessageKind::Info, true);
        return false;
    }

    int availableGold = goldCount();
    if (availableGold <= 0) {
        pushMsg("YOU HAVE NO GOLD.", MessageKind::Warning, true);
        return false;
    }

    int spent = 0;
    int remainingGold = availableGold;

    // Apply payments across unpaid items on this depth.
    // - Stackables: pay whole units (split stack if only partially paid).
    // - Non-stackables: allow partial payments by reducing the remaining owed.
    for (size_t i = 0; i < inv.size(); ++i) {
        if (remainingGold <= 0) break;

        Item& it = inv[i];
        if (it.shopPrice <= 0 || it.shopDepth != depth_) continue;

        const int perUnit = it.shopPrice;
        if (perUnit <= 0) continue;

        if (isStackable(it.kind) && it.count > 1) {
            // Pay as many whole units as possible.
            const int canUnits = std::min(it.count, remainingGold / perUnit);
            if (canUnits <= 0) continue;

            const int pay = canUnits * perUnit;
            remainingGold -= pay;
            spent += pay;

            if (canUnits == it.count) {
                // Entire stack paid.
                it.shopPrice = 0;
                it.shopDepth = 0;
            } else {
                // Split: paid portion becomes a separate stack.
                it.count -= canUnits;
                Item paid = it;
                paid.count = canUnits;
                paid.shopPrice = 0;
                paid.shopDepth = 0;
                if (!tryStackItem(inv, paid)) {
                    inv.push_back(paid);
                }
            }
        } else {
            // Pay partially (or fully) for a single unit item.
            const int pay = std::min(perUnit, remainingGold);
            it.shopPrice -= pay;
            remainingGold -= pay;
            spent += pay;

            if (it.shopPrice <= 0) {
                it.shopPrice = 0;
                it.shopDepth = 0;
            }
        }
    }

    // Pay down any additional bill for goods already consumed/destroyed.
    if (remainingGold > 0 && depth_ >= 1 && depth_ <= DUNGEON_MAX_DEPTH) {
        int& bill = shopDebtLedger_[depth_];
        if (bill > 0) {
            const int pay = std::min(bill, remainingGold);
            bill -= pay;
            remainingGold -= pay;
            spent += pay;
        }
    }

    if (spent <= 0) {
        pushMsg("YOU CANNOT PAY FOR ANYTHING RIGHT NOW.", MessageKind::Info, true);
        return false;
    }

    (void)spendGoldFromInv(inv, spent);

    const int stillOwe = shopDebtThisDepth();
    if (stillOwe <= 0) {
        pushMsg("YOU PAY " + std::to_string(spent) + " GOLD. YOUR DEBT IS CLEARED.", MessageKind::Success, true);
        // Calm the shopkeeper if this payment settled the bill.
        setShopkeepersAlerted(ents, playerId_, player().pos, false);
    } else {
        pushMsg("YOU PAY " + std::to_string(spent) + " GOLD. YOU STILL OWE " + std::to_string(stillOwe) + " GOLD.", MessageKind::Info, true);
    }

    // Paying takes a turn.
    advanceAfterPlayerAction();
    return true;
}




bool Game::digInDirection(int dx, int dy) {
    if (gameOver || gameWon) return false;
    if (dx == 0 && dy == 0) {
        pushMsg("DIG WHERE?", MessageKind::Info, true);
        return false;
    }

    const Item* mw = equippedMelee();
    if (!mw || mw->kind != ItemKind::Pickaxe) {
        pushMsg("YOU NEED TO WIELD A PICKAXE.", MessageKind::Warning, true);
        return false;
    }

    const Vec2i src = player().pos;
    const Vec2i p{ src.x + dx, src.y + dy };
    if (!dung.inBounds(p.x, p.y)) {
        pushMsg("YOU CAN'T DIG THERE.", MessageKind::Info, true);
        return false;
    }

    // Attempting to dig always costs a turn (like lockpicking), even if nothing happens.
    emitNoise(src, 14);

    if (Entity* e = entityAtMut(p.x, p.y)) {
        (void)e;
        pushMsg("YOU CAN'T DIG THROUGH THAT!", MessageKind::Warning, true);
        advanceAfterPlayerAction();
        return true;
    }

    const TileType before = dung.at(p.x, p.y).type;
    if (!dung.isDiggable(p.x, p.y)) {
        pushMsg("YOU DIG, BUT NOTHING YIELDS.", MessageKind::Info, true);
        advanceAfterPlayerAction();
        return true;
    }

    (void)dung.dig(p.x, p.y);
    switch (before) {
        case TileType::Wall:       pushMsg("YOU DIG THROUGH THE WALL.", MessageKind::Info, true); break;
        case TileType::Pillar:     pushMsg("YOU SHATTER THE PILLAR.", MessageKind::Info, true); break;
        case TileType::DoorClosed:
        case TileType::DoorLocked:
        case TileType::DoorSecret: pushMsg("YOU SMASH THROUGH THE DOORFRAME.", MessageKind::Info, true); break;
        default:                   pushMsg("YOU DIG.", MessageKind::Info, true); break;
    }

    recomputeFov();
    advanceAfterPlayerAction();
    return true;
}
