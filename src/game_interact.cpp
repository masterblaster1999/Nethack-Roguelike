#include "game_internal.hpp"

int Game::playerFootstepNoiseVolumeAt(Vec2i pos) const {
    if (!dung.inBounds(pos.x, pos.y)) return 0;

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

    // Substrate materials subtly affect how much sound you make while moving.
    // (Moss/dirt dampen; metal/crystal ring out.)
    int matDelta = 0;
    {
        dung.ensureMaterials(seed_, branch_, depth_, dungeonMaxDepth());
        const TerrainMaterial m = dung.materialAtCached(pos.x, pos.y);
        matDelta = terrainMaterialFx(m).footstepNoiseDelta;
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
        vol = clampi(vol + matDelta, minVol, 14);
    } else {
        vol = clampi(vol, 2, 14);
        vol = clampi(vol + matDelta, 1, 14);
    }

    return std::max(0, vol);
}


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

    if (!dung.inBounds(nx, ny)) {
        if (e.id == playerId_ && atCamp()) {
            return tryOverworldStep(dx, dy);
        }
        return false;
    }

    // Prevent diagonal corner-cutting (no slipping between two blocking tiles).
    if (!phasing && dx != 0 && dy != 0 && !diagonalPassable(dung, e.pos, dx, dy)) {
        if (e.kind == EntityKind::Player) pushMsg("YOU CAN'T SQUEEZE THROUGH.");
        return false;
    }

    // Closed door: opening consumes a turn.
    if (!phasing && dung.isDoorClosed(nx, ny)) {
        dung.openDoor(nx, ny);
        onDoorOpened({nx, ny}, e.id == playerId_);
        if (e.kind == EntityKind::Player) {
            pushMsg("YOU OPEN THE DOOR.");
            // Opening doors is noisy; monsters may investigate.
            emitNoise({nx, ny}, isSneaking() ? 8 : 12);
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
            // Keep door-bashing policy consistent with AI/pathing helpers.
            const bool canBash = entityCanBashLockedDoor(e.kind);
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
                onDoorOpened({nx, ny}, false);

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
            onDoorOpened({nx, ny}, true);
            pushMsg("YOU UNLOCK THE DOOR.", MessageKind::System, true);
            emitNoise({nx, ny}, isSneaking() ? 9 : 12);
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
                onDoorOpened({nx, ny}, true);
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
            emitNoise({nx, ny}, isSneaking() ? 8 : 10);

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
                        // Digging noise depends on the local substrate material (metal rings, moss muffles, ...).
                        dung.ensureMaterials(seed_, branch_, depth_, dungeonMaxDepth());
                        const TerrainMaterial digMat = dung.materialAtCached(nx, ny);
                        int digNoise = 14 + terrainMaterialFx(digMat).digNoiseDelta;
                        digNoise = clampi(digNoise, 6, 20);
                        emitNoise(e.pos, digNoise);
                        pushFxParticle(FXParticlePreset::Dig, Vec2i{nx, ny}, 24, 0.14f);

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
                triggerShopTheftAlarm(prevPos, e.pos);
            }
        }
        // Footstep noise: small, but enough for nearby monsters to investigate.
        // Scales with encumbrance + armor clank + substrate material, and respects sneak.
        const int vol = playerFootstepNoiseVolumeAt(e.pos);
        if (vol > 0) {
            emitNoise(e.pos, vol);
        }
        // Convenience / QoL: auto-pickup when stepping on items.
        if (autoPickup != AutoPickupMode::Off) {
            (void)autoPickupAtPlayer();
        }
    }

    // Traps trigger on enter (monsters can trigger them too).
    const Vec2i enteredPos = e.pos;
    const int depthBefore = depth_;
    triggerTrapAt(enteredPos, e);

    // Some traps (trap doors) change dungeon depth. Only trigger sigils if we stayed
    // on the same depth and the victim survived.
    if (!gameOver && e.hp > 0 && depth_ == depthBefore) {
        triggerSigilAt(enteredPos, e);
    }

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
                        triggerShopTheftAlarm(prevPos, victim.pos);
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

        case TrapKind::PoisonGas: {
            // Lingering poison gas cloud. This trap creates a persistent, tile-based hazard
            // that slowly diffuses and dissipates over time.
            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (poisonGas_.size() != expect) poisonGas_.assign(expect, 0u);

            // Apply an immediate poison hit to the victim (the cloud will keep it topped up).
            const int turns = rng.range(3, 6) + std::min(4, depth_ / 3);
            victim.effects.poisonTurns = std::max(victim.effects.poisonTurns, turns);

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
                    if (poisonGas_[i] < ss) poisonGas_[i] = ss;
                }
            }

            if (isPlayer) {
                pushMsg("A CLOUD OF TOXIC VAPOR ERUPTS!", MessageKind::Warning, true);
                pushMsg("YOU ARE POISONED!", MessageKind::Warning, true);
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " CHOKES IN A CLOUD OF TOXIC VAPOR!";
                pushMsg(ss.str(), MessageKind::Warning, false);
            }

            // Gas traps are loud enough to draw attention.
            emitNoise(pos, 8);
            break;
        }

        case TrapKind::CorrosiveGas: {
            // Lingering corrosive gas cloud. This trap creates a persistent, tile-based hazard
            // that slowly diffuses and dissipates over time.
            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (corrosiveGas_.size() != expect) corrosiveGas_.assign(expect, 0u);

            // Apply an immediate corrosion hit to the victim (the cloud will keep it topped up).
            const int turns = rng.range(3, 6) + std::min(3, depth_ / 4);
            victim.effects.corrosionTurns = std::max(victim.effects.corrosionTurns, turns);

            // Seed the gas intensity in a small radius around the trap.
            const uint8_t baseStrength = static_cast<uint8_t>(clampi(9 + depth_ / 4, 9, 13));
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
                    if (corrosiveGas_[i] < ss) corrosiveGas_[i] = ss;
                }
            }

            if (isPlayer) {
                pushMsg("A HISSING CLOUD OF ACRID VAPOR ERUPTS!", MessageKind::Warning, true);
            } else if (tileVisible) {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " IS CAUGHT IN A HISSING CLOUD OF ACRID VAPOR!";
                pushMsg(ss.str(), MessageKind::Warning, false);
            }

            emitNoise(pos, 8);
            break;
        }


        case TrapKind::LetheMist: {
            // Lethe mist: a single-use burst of forgetfulness.
            //
            // If the player is affected, they forget most of the current level's map memory
            // (explored tiles, discovered traps, and far-off markers). Additionally, unseen
            // monsters may lose the thread of where the player is.
            //
            // This is intentionally quieter than confusion gas.

            if (isPlayer) {
                if (fromDisarm) pushMsg("A GREY MIST ERUPTS! YOUR MEMORY SLIPS AWAY...", MessageKind::Warning, true);
                else pushMsg("A GREY MIST ENVELOPS YOU! YOUR MEMORY SLIPS AWAY...", MessageKind::Warning, true);

                // Keep a small local patch of remembered map.
                applyAmnesiaShock(6);
            } else {
                // Monsters lose the thread.
                victim.alerted = false;
                victim.lastKnownPlayerPos = {-1, -1};
                victim.lastKnownPlayerAge = 9999;

                if (tileVisible) {
                    pushMsg("A GREY MIST SWIRLS BRIEFLY.", MessageKind::Info, false);
                }
            }

            // Single-use: the mist is spent once released.
            trapsCur.erase(trapsCur.begin() + tIndex);
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
                        onDoorOpened(nxt, false);
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

                if (depth_ >= DUNGEON_MAX_DEPTH && !(infiniteWorldEnabled_ && branch_ == DungeonBranch::Main)) {
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
                if (depth_ >= DUNGEON_MAX_DEPTH && !(infiniteWorldEnabled_ && branch_ == DungeonBranch::Main)) {
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
                    const LevelId dst{branch_, dstDepth};
                    trapdoorFallers_[dst].push_back(faller);
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

void Game::triggerSigilAt(Vec2i pos, Entity& victim) {
    if (gameOver) return;
    if (!dung.inBounds(pos.x, pos.y)) return;

    // Sigils are special graffiti: an engraving whose text begins with "SIGIL".
    // They are intentionally sparse, limited-use, and a little unpredictable.
    for (size_t i = 0; i < engravings_.size(); ++i) {
        Engraving& eg = engravings_[i];
        if (eg.pos.x != pos.x || eg.pos.y != pos.y) continue;

        std::string key;
        if (!engravingIsSigil(eg, &key)) return;

        // If a sigil somehow persisted with 0 strength, clean it up.
        if (eg.strength == 0u) {
            engravings_.erase(engravings_.begin() + static_cast<std::vector<Engraving>::difference_type>(i));
            return;
        }

        const bool isPlayer = (victim.id == playerId_);
        const bool vis = dung.at(pos.x, pos.y).visible;

        auto say = [&](const std::string& s, MessageKind kind, bool importantWhenUnseen) {
            if (isPlayer) {
                pushMsg(s, kind, true);
            } else if (vis) {
                pushMsg(s, kind, false);
            } else if (importantWhenUnseen) {
                // Even if you can't see it, some sigils have audible/tactile feedback.
                pushMsg(s, kind, false);
            }
        };

        auto consumeUse = [&](bool fadeMessage) {
            // 255 is reserved for "permanent" graffiti; sigils should never be 255, but
            // if they are (e.g., via manual save editing), treat it as single-use.
            if (eg.strength == 255u) {
                eg.strength = 0u;
            } else if (eg.strength > 0u) {
                eg.strength = static_cast<uint8_t>(eg.strength - 1u);
            }

            if (eg.strength == 0u) {
                if (fadeMessage) {
                    say("THE SIGIL FADES.", MessageKind::System, false);
                }
                engravings_.erase(engravings_.begin() + static_cast<std::vector<Engraving>::difference_type>(i));
            }
        };

        // ------------------------------------------------------------------
        // SIGIL EFFECTS
        // ------------------------------------------------------------------
        // NOTE: Keep effects deterministic and local. We use existing systems
        // (traps/doors, gas, fire) rather than inventing new persistent formats.

        if (key == "SEER") {
            // A mild "detect secrets" burst around the sigil's location.
            // Player-only: monsters don't meaningfully use this information.
            if (!isPlayer) return;

            constexpr int radius = 5;
            int revealedTraps = 0;
            int revealedDoors = 0;
            int revealedChests = 0;

            for (auto& t : trapsCur) {
                if (t.discovered) continue;
                if (chebyshev(t.pos, pos) <= radius) {
                    t.discovered = true;
                    revealedTraps += 1;
                }
            }

            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) > radius) continue;
                    const int x = pos.x + dx;
                    const int y = pos.y + dy;
                    if (!dung.inBounds(x, y)) continue;
                    Tile& tt = dung.at(x, y);
                    if (tt.type == TileType::DoorSecret) {
                        tt.type = TileType::DoorClosed;
                        tt.explored = true;
                        revealedDoors += 1;
                    }
                }
            }

            for (auto& gi : ground) {
                if (!isChestKind(gi.item.kind)) continue;
                if (!chestTrapped(gi.item)) continue;
                if (chestTrapKnown(gi.item)) continue;
                if (chebyshev(gi.pos, pos) > radius) continue;
                setChestTrapKnown(gi.item, true);
                revealedChests += 1;
            }

            say("THE SIGIL'S LINES REARRANGE IN YOUR MIND.", MessageKind::System, false);

            if (revealedTraps + revealedDoors + revealedChests > 0) {
                std::ostringstream ss;
                ss << "YOU GLIMPSE ";
                bool first = true;
                if (revealedDoors > 0) {
                    ss << revealedDoors << " HIDDEN PASSAGE" << (revealedDoors == 1 ? "" : "S");
                    first = false;
                }
                if (revealedTraps > 0) {
                    if (!first) ss << ", ";
                    ss << revealedTraps << " TRAP" << (revealedTraps == 1 ? "" : "S");
                    first = false;
                }
                if (revealedChests > 0) {
                    if (!first) ss << ", ";
                    ss << revealedChests << " TRAPPED CHEST" << (revealedChests == 1 ? "" : "S");
                }
                ss << ".";
                say(ss.str(), MessageKind::Info, false);
            } else {
                say("...BUT NOTHING STIRS.", MessageKind::Info, false);
            }

            consumeUse(true);
            return;
        }

        if (key == "NEXUS") {
            // Teleport (neutral-chaotic). Works on monsters too.
            if (isPlayer) {
                say("SPACE TWISTS AROUND YOU!", MessageKind::Warning, true);
            } else {
                std::ostringstream ss;
                ss << kindName(victim.kind) << " VANISHES!";
                say(ss.str(), MessageKind::Warning, false);
            }

            Vec2i dst = dung.randomFloor(rng, true);
            // Avoid teleporting into stairs or onto another entity.
            for (int tries = 0; tries < 200; ++tries) {
                Vec2i cand = dung.randomFloor(rng, true);
                if (!dung.inBounds(cand.x, cand.y)) continue;
                TileType tt = dung.at(cand.x, cand.y).type;
                if (tt == TileType::StairsUp || tt == TileType::StairsDown) continue;
                if (entityAt(cand.x, cand.y) != nullptr) continue;
                dst = cand;
                break;
            }

            const Vec2i from = victim.pos;
            victim.pos = dst;

            // Mirrors trap teleport's shop-debt safety (so you can't escape a shop by luck).
            if (isPlayer) {
                const bool wasInShop = (roomTypeAt(dung, from) == RoomType::Shop);
                const bool nowInShop = (roomTypeAt(dung, dst) == RoomType::Shop);
                if (wasInShop && !nowInShop) {
                    const int debt = shopDebtThisDepth();
                    if (debt > 0) {
                        victim.pos = from;
                        say("A FORCE YANKS YOU BACK!", MessageKind::Warning, true);
                    }
                }
            }

            // A teleport should wake up the floor a bit.
            emitNoise(pos, 10);

            consumeUse(true);
            return;
        }

        if (key == "MIASMA") {
            // Spawn a small confusion gas bloom.
            say("THE SIGIL EXHALES A NOXIOUS MIASMA!", MessageKind::Warning, true);

            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (confusionGas_.size() != expect) confusionGas_.assign(expect, 0u);
            auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * dung.width + x); };

            constexpr int radius = 2;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int x = pos.x + dx;
                    const int y = pos.y + dy;
                    if (!dung.inBounds(x, y)) continue;
                    if (!dung.isWalkable(x, y)) continue;
                    const int dist = std::max(std::abs(dx), std::abs(dy));
                    const int inten = 10 - 3 * dist;
                    if (inten <= 0) continue;
                    const size_t ii = idx(x, y);
                    if (ii >= confusionGas_.size()) continue;
                    confusionGas_[ii] = std::max(confusionGas_[ii], static_cast<uint8_t>(inten));
                }
            }

            victim.effects.confusionTurns = std::max(victim.effects.confusionTurns, 6);
            emitNoise(pos, 8);

            consumeUse(true);
            return;
        }

        if (key == "EMBER") {
            // Spawn a small fire flare.
            say("THE SIGIL FLARES WITH EMBERS!", MessageKind::Warning, true);

            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (fireField_.size() != expect) fireField_.assign(expect, 0u);
            auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * dung.width + x); };

            constexpr int radius = 1;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int x = pos.x + dx;
                    const int y = pos.y + dy;
                    if (!dung.inBounds(x, y)) continue;
                    if (!dung.isWalkable(x, y)) continue;
                    const int dist = std::max(std::abs(dx), std::abs(dy));
                    const int inten = (dist == 0) ? 12 : 6;
                    const size_t ii = idx(x, y);
                    if (ii >= fireField_.size()) continue;
                    fireField_[ii] = std::max(fireField_[ii], static_cast<uint8_t>(inten));
                }
            }

            victim.effects.burnTurns = std::max(victim.effects.burnTurns, 6);
            emitNoise(pos, 10);

            consumeUse(true);
            return;
        }

        // Unknown sigil keyword: treat as inert graffiti.
        return;
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




void Game::autoSearchTick() {
    if (gameOver || gameWon) return;

    // Requires at least one Ring of Searching equipped.
    int bestPower = -9999;
    bool hasRing = false;

    auto consider = [&](const Item* r) {
        if (!r) return;
        if (r->kind != ItemKind::RingSearching) return;
        hasRing = true;

        int p = r->enchant;
        if (r->buc < 0) p -= 1;
        else if (r->buc > 0) p += 1;

        bestPower = std::max(bestPower, p);
    };

    consider(equippedRing1());
    consider(equippedRing2());

    if (!hasRing) return;

    Entity& pl = playerMut();

    // A subtle, automatic search around the player each turn.
    // This is intentionally weaker than the explicit SEARCH action.
    int radius = 1;
    if (bestPower >= 2) radius = 2;

    float baseChance = 0.08f + 0.015f * static_cast<float>(charLevel);
    baseChance += 0.0075f * static_cast<float>(playerFocus());
    baseChance += 0.04f * static_cast<float>(bestPower);
    baseChance = std::min(0.65f, std::max(0.05f, baseChance));

    int foundTraps = 0;
    int foundSecrets = 0;

    // Traps
    for (auto& t : trapsCur) {
        if (t.discovered) continue;
        int dx = std::abs(t.pos.x - pl.pos.x);
        int dy = std::abs(t.pos.y - pl.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > radius) continue;

        float chance = baseChance;
        if (cheb <= 1) chance = std::min(0.75f, chance + 0.12f);
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

        int dx = std::abs(gi.pos.x - pl.pos.x);
        int dy = std::abs(gi.pos.y - pl.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > radius) continue;

        float chance = baseChance;
        if (cheb <= 1) chance = std::min(0.75f, chance + 0.12f);
        if (rng.chance(chance)) {
            setChestTrapKnown(gi.item, true);
            foundTraps += 1;
        }
    }

    // Secret doors (TileType::DoorSecret -> DoorClosed)
    for (int y = pl.pos.y - radius; y <= pl.pos.y + radius; ++y) {
        for (int x = pl.pos.x - radius; x <= pl.pos.x + radius; ++x) {
            if (!dung.inBounds(x, y)) continue;
            Tile& t = dung.at(x, y);
            if (t.type != TileType::DoorSecret) continue;

            int dx = std::abs(x - pl.pos.x);
            int dy = std::abs(y - pl.pos.y);
            int cheb = std::max(dx, dy);
            if (cheb > radius) continue;

            float chance = std::max(0.05f, baseChance - 0.12f);
            if (cheb <= 1) chance = std::min(0.75f, chance + 0.12f);

            if (rng.chance(chance)) {
                t.type = TileType::DoorClosed;
                t.explored = true;
                foundSecrets += 1;
            }
        }
    }

    if (foundTraps > 0 || foundSecrets > 0) {
        // Keep the messaging terse; this can trigger often.
        pushMsg(formatSearchDiscoveryMessage(foundTraps, foundSecrets), MessageKind::Info, /*fromPlayer=*/false);
    }
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
            case TrapKind::LetheMist: return "LETHE MIST";
            case TrapKind::PoisonGas: return "POISON GAS";
            case TrapKind::CorrosiveGas: return "CORROSIVE GAS";
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
        if (tk == TrapKind::PoisonGas) chance *= 0.92f;
        if (tk == TrapKind::CorrosiveGas) chance *= 0.90f;


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
        if (tk == TrapKind::PoisonGas) setOffChance += 0.03f;
        if (tk == TrapKind::CorrosiveGas) setOffChance += 0.04f;

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
                            triggerShopTheftAlarm(prevPos, p.pos);
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
            case TrapKind::ConfusionGas: {
                // A burst of noxious gas.
                p.effects.confusionTurns = std::max(p.effects.confusionTurns, rng.range(4, 7));
                pushMsg("A NOXIOUS GAS ERUPTS! YOU FEEL CONFUSED!", MessageKind::Warning, true);
                break;
            }
            case TrapKind::PoisonGas: {
                // A burst of toxic vapor.
                p.effects.poisonTurns = std::max(p.effects.poisonTurns, rng.range(3, 6));
                pushMsg("A CLOUD OF TOXIC VAPOR ERUPTS!", MessageKind::Warning, true);
                pushMsg("YOU ARE POISONED!", MessageKind::Warning, true);
                break;
            }
            case TrapKind::CorrosiveGas: {
                // A burst of acrid vapor.
                p.effects.corrosionTurns = std::max(p.effects.corrosionTurns, rng.range(3, 6));
                pushMsg("A HISSING CLOUD OF ACRID VAPOR ERUPTS!", MessageKind::Warning, true);
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
    if (tr.kind == TrapKind::PoisonGas) chance *= 0.85f;
    if (tr.kind == TrapKind::CorrosiveGas) chance *= 0.82f;

    if (tr.kind == TrapKind::LetheMist) chance *= 0.83f;

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
    if (tr.kind == TrapKind::PoisonGas) setOffChance = 0.18f;
    if (tr.kind == TrapKind::CorrosiveGas) setOffChance = 0.20f;
    if (tr.kind == TrapKind::RollingBoulder) setOffChance = 0.22f;
    if (tr.kind == TrapKind::TrapDoor) setOffChance = 0.24f;
    if (tr.kind == TrapKind::LetheMist) setOffChance = 0.23f;

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
    emitNoise({doorX, doorY}, isSneaking() ? 6 : 8);
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

    emitNoise({doorX, doorY}, isSneaking() ? 6 : 8);

    return true; // Locking costs a turn.
}



void Game::beginDig() {
    if (gameOver || gameWon) return;

    const Item* mw = equippedMelee();
    if (!mw || mw->kind != ItemKind::Pickaxe) {
        pushMsg("YOU NEED TO WIELD A PICKAXE.", MessageKind::Warning, true);
        return;
    }

    // Close other overlays/modes.
    invOpen = false;
    invIdentifyMode = false;
    invEnchantRingMode = false;
    targeting = false;
    looking = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    optionsOpen = false;

    if (commandOpen) {
        commandOpen = false;
        commandBuf.clear();
        commandCursor_ = 0;
        commandDraft.clear();
        commandHistoryPos = -1;
    }

    msgScroll = 0;

    digging = true;
    pushMsg("DIG IN WHICH DIRECTION?", MessageKind::System, true);
}

void Game::beginKick() {
    if (gameOver || gameWon) return;

    // Close other overlays/modes.
    invOpen = false;
    invIdentifyMode = false;
    invEnchantRingMode = false;
    targeting = false;
    looking = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    optionsOpen = false;

    if (commandOpen) {
        commandOpen = false;
        commandBuf.clear();
        commandCursor_ = 0;
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
                case TrapKind::PoisonGas: {
                    const int turns = rng.range(6, 10) + std::min(6, depth_ / 2);
                    p.effects.poisonTurns = std::max(p.effects.poisonTurns, turns);
                    pushMsg("A CLOUD OF TOXIC VAPOR BURSTS FROM THE CHEST!", MessageKind::Warning, true);
                    pushMsg("YOU ARE POISONED!", MessageKind::Warning, true);
                    emitNoise(tgt, 8);
                    break;
                }
                case TrapKind::CorrosiveGas: {
                    const int turns = rng.range(6, 10) + std::min(6, depth_ / 2);
                    p.effects.corrosionTurns = std::max(p.effects.corrosionTurns, turns);
                    pushMsg("A HISSING CLOUD OF ACRID VAPOR BURSTS FROM THE CHEST!", MessageKind::Warning, true);
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
        onDoorOpened(tgt, true);
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
            onDoorOpened(tgt, true);
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

    // Prayer timeout: shrine services cannot be spammed back-to-back.
    if (turnCount < prayerCooldownUntilTurn_) {
        const int cd = static_cast<int>(prayerCooldownUntilTurn_ - turnCount);
        pushMsg("THE GODS ARE SILENT. (COOLDOWN: " + std::to_string(cd) + ")", MessageKind::Info, true);
        return false;
    }

    // Services are priced in PIETY. If you don't have enough, the shrine will accept a
    // gold donation to make up the difference.
    constexpr int GOLD_PER_PIETY = 5;

    const int baseGold = 10 + depth_ * 2;
    int costGold = 0;
    if (mode == "heal") costGold = baseGold + 6;
    else if (mode == "cure") costGold = baseGold + 8;
    else if (mode == "identify") costGold = baseGold + 10;
    else if (mode == "bless") costGold = baseGold + 12;
    else if (mode == "uncurse") costGold = baseGold + 14;
    else if (mode == "recharge") costGold = baseGold + 16;

    const int costPiety = std::max(1, (costGold + GOLD_PER_PIETY - 1) / GOLD_PER_PIETY);

    if (piety_ < costPiety) {
        const int missing = costPiety - piety_;
        const int goldNeeded = missing * GOLD_PER_PIETY;
        if (goldCount() < goldNeeded) {
            pushMsg("YOU LACK THE PIETY FOR THAT.", MessageKind::Info, true);
            pushMsg("YOU ALSO LACK THE GOLD TO DONATE (" + std::to_string(goldNeeded) + ").", MessageKind::Info, true);
            return false;
        }

        // Convert just enough gold into piety.
        (void)spendGoldFromInv(inv, goldNeeded);
        piety_ = std::min(999, piety_ + missing);
        pushMsg("YOU DONATE " + std::to_string(goldNeeded) + " GOLD. (+" + std::to_string(missing) + " PIETY)", MessageKind::Info, true);
    }

    // Spend piety now; selection prompts (if any) are UI-only and do not consume extra turns.
    piety_ -= costPiety;

    // Conduct tracking: using shrine services breaks ATHEIST.
    ++conductPrayers_;
    pushMsg("YOU OFFER " + std::to_string(costPiety) + " PIETY.", MessageKind::Info, true);

    // Set a simple prayer timeout scaled by how "expensive" the service is.
    const uint32_t cooldown = 40u + static_cast<uint32_t>(costPiety) * 10u;
    prayerCooldownUntilTurn_ = std::max(prayerCooldownUntilTurn_, turnCount + cooldown);

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

bool Game::donateAtShrine(int goldAmount) {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();

    bool inShrine = false;
    for (const auto& r : dung.rooms) {
        if (r.type == RoomType::Shrine && r.contains(p.pos.x, p.pos.y)) { inShrine = true; break; }
    }
    const bool atCamp = (branch_ == DungeonBranch::Camp);

    if (!inShrine && !atCamp) {
        pushMsg("YOU NEED A SHRINE OR YOUR CAMP TO DONATE.", MessageKind::Info, true);
        return false;
    }

    constexpr int GOLD_PER_PIETY = 5;

    const int gold = goldCount();
    const int maxConvertible = (gold / GOLD_PER_PIETY) * GOLD_PER_PIETY;
    if (maxConvertible < GOLD_PER_PIETY) {
        pushMsg("YOU HAVE TOO LITTLE GOLD TO DONATE.", MessageKind::Info, true);
        return false;
    }

    int target = goldAmount;
    if (target <= 0) {
        // Default: donate enough for ~10 piety (or as much as you can).
        target = std::min(maxConvertible, GOLD_PER_PIETY * 10);
    }

    int donateGold = (target / GOLD_PER_PIETY) * GOLD_PER_PIETY;
    donateGold = std::min(donateGold, maxConvertible);

    if (donateGold < GOLD_PER_PIETY) {
        pushMsg("DONATION MUST BE AT LEAST " + std::to_string(GOLD_PER_PIETY) + " GOLD.", MessageKind::Info, true);
        return false;
    }

    const int gain = donateGold / GOLD_PER_PIETY;

    (void)spendGoldFromInv(inv, donateGold);
    piety_ = std::min(999, piety_ + gain);

    pushMsg("YOU DONATE " + std::to_string(donateGold) + " GOLD. (+" + std::to_string(gain) + " PIETY)", MessageKind::Success, true);

    advanceAfterPlayerAction();
    return true;
}

bool Game::sacrificeAtShrine() {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();

    bool inShrine = false;
    for (const auto& r : dung.rooms) {
        if (r.type == RoomType::Shrine && r.contains(p.pos.x, p.pos.y)) { inShrine = true; break; }
    }
    const bool atCamp = (branch_ == DungeonBranch::Camp);

    if (!inShrine && !atCamp) {
        pushMsg("YOU NEED A SHRINE OR YOUR CAMP TO SACRIFICE.", MessageKind::Info, true);
        return false;
    }

    std::vector<int> corpses;
    corpses.reserve(8);
    for (size_t i = 0; i < inv.size(); ++i) {
        if (isCorpseKind(inv[i].kind)) corpses.push_back(static_cast<int>(i));
    }

    if (corpses.empty()) {
        pushMsg("YOU HAVE NOTHING TO SACRIFICE.", MessageKind::Info, true);
        return false;
    }

    auto sacrificeOne = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(inv.size())) return;

        Item& it = inv[static_cast<size_t>(idx)];
        if (!isCorpseKind(it.kind)) return;

        const bool rotten = (it.charges <= 0);

        EntityKind ek = EntityKind::Goblin;
        switch (it.kind) {
            case ItemKind::CorpseGoblin:   ek = EntityKind::Goblin; break;
            case ItemKind::CorpseOrc:      ek = EntityKind::Orc; break;
            case ItemKind::CorpseBat:      ek = EntityKind::Bat; break;
            case ItemKind::CorpseSlime:    ek = EntityKind::Slime; break;
            case ItemKind::CorpseKobold:   ek = EntityKind::KoboldSlinger; break;
            case ItemKind::CorpseWolf:     ek = EntityKind::Wolf; break;
            case ItemKind::CorpseTroll:    ek = EntityKind::Troll; break;
            case ItemKind::CorpseWizard:   ek = EntityKind::Wizard; break;
            case ItemKind::CorpseSnake:    ek = EntityKind::Snake; break;
            case ItemKind::CorpseSpider:   ek = EntityKind::Spider; break;
            case ItemKind::CorpseOgre:     ek = EntityKind::Ogre; break;
            case ItemKind::CorpseMimic:    ek = EntityKind::Mimic; break;
            case ItemKind::CorpseMinotaur: ek = EntityKind::Minotaur; break;
            default: break;
        }

        int gain = std::max(1, xpFor(ek) / 8);
        if (rotten) gain = std::max(1, gain / 2);

        // Consume corpse (corpses are usually count=1, but keep it generic).
        if (it.count > 1) {
            it.count -= 1;
        } else {
            inv.erase(inv.begin() + idx);
            invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
        }

        piety_ = std::min(999, piety_ + gain);
        pushMsg("YOU OFFER A SACRIFICE. (+" + std::to_string(gain) + " PIETY)", MessageKind::Success, true);
    };

    if (corpses.size() == 1) {
        sacrificeOne(corpses[0]);
    } else {
        openInventory();
        invPrompt_ = InvPromptKind::ShrineSacrifice;
        invSel = corpses[0];
        pushMsg("SELECT A CORPSE TO SACRIFICE (ENTER=OFFER, ESC=BEST).", MessageKind::System, true);
    }

    advanceAfterPlayerAction();
    return true;
}


bool Game::augury() {
    if (gameOver || gameWon) return false;

    if (depth_ >= DUNGEON_MAX_DEPTH && !(infiniteWorldEnabled_ && branch_ == DungeonBranch::Main)) {
        pushMsg("NO DEEPER FUTURE CALLS.", MessageKind::Info, true);
        return false;
    }

    Entity& p = playerMut();

    bool inShrine = false;
    for (const auto& r : dung.rooms) {
        if (r.type == RoomType::Shrine && r.contains(p.pos.x, p.pos.y)) { inShrine = true; break; }
    }

    const bool atCamp = (branch_ == DungeonBranch::Camp);
    if (!inShrine && !atCamp) {
        pushMsg("YOU NEED A SHRINE OR YOUR CAMP TO ATTEMPT AUGURY.", MessageKind::Info, true);
        return false;
    }

    // Slightly cheaper in shrines (where you're still in danger) than in the safe-ish camp.
    const int base = 8 + std::max(0, depth_) * 2;
    const int cost = inShrine ? base : (base + 4);

    if (goldCount() < cost) {
        pushMsg("YOU LACK THE GOLD FOR AUGURY.", MessageKind::Info, true);
        return false;
    }

    (void)spendGoldFromInv(inv, cost);
    pushMsg("YOU PAY " + std::to_string(cost) + " GOLD AND CAST THE BONES...", MessageKind::Info, true);

    // Preview the next floor using its deterministic per-level seed.
    // (Worldgen is decoupled from the gameplay RNG stream, so this vision stays accurate.)

    DungeonBranch nextBranch = branch_;
    int nextDepth = depth_ + 1;

    // From Camp, the "next floor" is the first floor of the Main dungeon.
    if (atCamp) {
        nextBranch = DungeonBranch::Main;
        nextDepth = 1;
    }
    RNG previewRng(levelGenSeed(LevelId{nextBranch, nextDepth}));
    const Vec2i msz = proceduralMapSizeFor(previewRng, nextBranch, nextDepth);
    Dungeon preview(msz.x, msz.y);

    preview.generate(previewRng, nextBranch, nextDepth, DUNGEON_MAX_DEPTH, seed_);
    ensureEndlessSanctumDownstairs(LevelId{nextBranch, nextDepth}, preview, previewRng);

    auto dirFromDelta = [&](int dx, int dy) -> std::string {
        if (dx == 0 && dy == 0) return "HERE";
        const int adx = std::abs(dx);
        const int ady = std::abs(dy);

        const bool east = (dx > 0);
        const bool south = (dy > 0);

        // Strong axis bias if one component dominates.
        if (adx > ady * 2) return east ? "EAST" : "WEST";
        if (ady > adx * 2) return south ? "SOUTH" : "NORTH";

        if (!south && east) return "NORTHEAST";
        if (!south && !east) return "NORTHWEST";
        if (south && east) return "SOUTHEAST";
        return "SOUTHWEST";
    };

    // Collect candidate omen lines based on the previewed floor.
    std::vector<std::string> pool;
    pool.reserve(16);

	// Floor signature (themed depths get a thematic line).
    if (nextDepth == Dungeon::MINES_DEPTH || nextDepth == Dungeon::DEEP_MINES_DEPTH) {
        pool.push_back("YOU DREAM OF PICKAXES AND TWISTING TUNNELS.");
    }
    if (nextDepth == Dungeon::GROTTO_DEPTH) {
        pool.push_back("YOU HEAR WATER DRIPPING IN YOUR DREAMS.");
    }
    if (nextDepth == Dungeon::CATACOMBS_DEPTH) {
        pool.push_back("MANY DOORS. MANY NAMES. MANY BONES.");
    }
    if (nextDepth == DUNGEON_MAX_DEPTH) {
		pool.push_back("THE AIR BELOW HUMS WITH OLD POWER.");
    }

    int shops = 0;
    int shrines = 0;
    int vaults = 0;
    int secrets = 0;
    int armories = 0;
    int libraries = 0;
    int labs = 0;
    for (const auto& r : preview.rooms) {
        switch (r.type) {
            case RoomType::Shop: ++shops; break;
            case RoomType::Shrine: ++shrines; break;
            case RoomType::Vault: ++vaults; break;
            case RoomType::Secret: ++secrets; break;
            case RoomType::Armory: ++armories; break;
            case RoomType::Library: ++libraries; break;
            case RoomType::Laboratory: ++labs; break;
            default: break;
        }
    }

    if (shops > 0) {
        pool.push_back("COINS CLINK BEHIND A COUNTER.");
    }
    if (vaults > 0) {
        pool.push_back("IRON AND GOLD WAIT BEHIND A LOCK.");
    }
    if (secrets > 0 || preview.secretShortcutCount > 0) {
        pool.push_back("A DOOR THAT IS NOT A DOOR HIDES IN STONE.");
    }
    if (shrines > 0) {
        pool.push_back("CANDLELIGHT FLICKERS BELOW.");
    }
    if (armories > 0) {
        pool.push_back("YOU SMELL OIL AND STEEL.");
    }
    if (libraries > 0) {
        pool.push_back("PAGES RUSTLE WITHOUT WIND.");
    }
    if (labs > 0) {
        pool.push_back("ACRID FUMES CURL THROUGH DARK HALLS.");
    }

    if (preview.hasCavernLake) {
        pool.push_back("A BLACK LAKE REFLECTS NO SKY.");
    }
    if (preview.hasWarrens) {
        pool.push_back("THE EARTH BELOW IS HONEYCOMBED WITH BURROWS.");
    }
    if (preview.lockedShortcutCount > 0) {
        pool.push_back("YOU HEAR KEYS RATTLING SOMEWHERE BELOW.");
    }
    if (preview.sinkholeCount > 0) {
        pool.push_back("THE GROUND FEELS HOLLOW UNDERFOOT.");
    }
    if (preview.deadEndClosetCount > 0) {
        pool.push_back("A BLIND HALL HIDES A SECRET CACHE.");
    }

    // Direction hint: from the up-stairs spawn to the down-stairs.
    std::string dirHint;
    if (preview.stairsDown.x >= 0 && preview.stairsDown.y >= 0 &&
        preview.inBounds(preview.stairsUp.x, preview.stairsUp.y) &&
        preview.inBounds(preview.stairsDown.x, preview.stairsDown.y)) {

        const int dx = preview.stairsDown.x - preview.stairsUp.x;
        const int dy = preview.stairsDown.y - preview.stairsUp.y;
        const std::string dir = dirFromDelta(dx, dy);
        if (dir == "HERE") {
            dirHint = "THE WAY DOWN IS CLOSE... TOO CLOSE.";
        } else {
            dirHint = "THE WAY DOWN LEANS " + dir + ".";
        }
    } else {
        // Some special floors (final sanctum) may not have a downward stair.
        dirHint = "THE VISION SHOWS NO WAY DOWN.";
    }

    // Pick up to 3 lines: direction + (optionally) 2 more from the pool.
    std::vector<std::string> chosen;
    chosen.reserve(3);
    if (!dirHint.empty()) chosen.push_back(dirHint);

    // Shuffle pool with the preview RNG so we don't consume game RNG.
    for (int i = static_cast<int>(pool.size()) - 1; i > 0; --i) {
        int j = previewRng.range(0, i);
        std::swap(pool[static_cast<size_t>(i)], pool[static_cast<size_t>(j)]);
    }

    for (const std::string& s : pool) {
        if (chosen.size() >= 3) break;
        bool dup = false;
        for (const auto& c : chosen) {
            if (c == s) { dup = true; break; }
        }
        if (!dup) chosen.push_back(s);
    }

    pushMsg("...THE SIGNS SWIM INTO PLACE.", MessageKind::Info, true);
    for (const auto& s : chosen) {
        pushMsg(s, MessageKind::System, true);
    }
    pushMsg("THE VISION FLICKERS. FATE IS NOT FIXED.", MessageKind::Info, true);

    advanceAfterPlayerAction();
    return true;
}

bool Game::payAtShop() {
    if (gameOver || gameWon) return false;

    if (!playerInShop()) {
        pushMsg("YOU MUST BE IN A SHOP TO PAY.", MessageKind::Info, true);
        return false;
    }

    if (!anyLivingShopkeeper(ents, playerId_)) {
        pushMsg("THERE IS NO SHOPKEEPER HERE.", MessageKind::Info, true);
        return false;
    }

    auto standDownMerchantGuild = [&]() {
        // Calm shopkeepers on the current level.
        setShopkeepersAlerted(ents, playerId_, player().pos, false);

        // Remove merchant guild guards from this level.
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != playerId_ && e.hp > 0 && e.kind == EntityKind::Guard;
        }), ents.end());

        // Calm shopkeepers + remove guards from stored levels (so the world "cools down" everywhere).
        for (auto& [d, st] : levels) {
            for (auto& e : st.monsters) {
                if (e.hp <= 0) continue;
                if (e.kind == EntityKind::Shopkeeper) {
                    e.alerted = false;
                }
            }

            st.monsters.erase(std::remove_if(st.monsters.begin(), st.monsters.end(), [&](const Entity& e) {
                return e.hp > 0 && e.kind == EntityKind::Guard;
            }), st.monsters.end());
        }

        // Also cancel any queued trapdoor fallers that are guards.
        // Trapdoor fallers are keyed by (branch, depth), so remove guards across all entries.
        for (auto it = trapdoorFallers_.begin(); it != trapdoorFallers_.end(); ) {
            auto& q = it->second;
            q.erase(std::remove_if(q.begin(), q.end(), [&](const Entity& e) {
                return e.hp > 0 && e.kind == EntityKind::Guard;
            }), q.end());
            if (q.empty()) {
                it = trapdoorFallers_.erase(it);
            } else {
                ++it;
            }
        }

        merchantGuildAlerted_ = false;
    };

    const int availableGold = countGold(inv);
    if (availableGold <= 0) {
        pushMsg("YOU HAVE NO GOLD TO PAY.", MessageKind::Info, true);
        return false;
    }

    const int owedTotal = shopDebtTotal();
    if (owedTotal <= 0) {
        pushMsg("YOU OWE NOTHING.", MessageKind::Info, true);

        // Safety: if a save somehow preserved an alerted guild state with no debt, stand down.
        if (merchantGuildAlerted_) {
            standDownMerchantGuild();
        }
        return false;
    }

    int spent = 0;
    int remainingGold = availableGold;

    auto payForItem = [&](Item& it) {
        if (remainingGold <= 0) return;
        if (it.shopPrice <= 0 || it.shopDepth <= 0) return;

        const int perUnit = it.shopPrice;
        if (perUnit <= 0) return;

        if (isStackable(it.kind) && it.count > 1) {
            // Pay as many whole units as possible.
            const int canUnits = std::min(it.count, remainingGold / perUnit);
            if (canUnits <= 0) return;

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
                    // Splitting a stack into an additional entry must create a new unique id
                    // (otherwise selection-by-id and other systems can break).
                    paid.id = nextItemId++;
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
    };

    // Apply payments across unpaid items.
    // Pay current depth first (so the shop you're standing in is satisfied ASAP),
    // then pay any remaining debts from other depths.
    for (size_t i = 0; i < inv.size() && remainingGold > 0; ++i) {
        Item& it = inv[i];
        if (it.shopPrice <= 0) continue;
        if (it.shopDepth != depth_) continue;
        payForItem(it);
    }
    for (size_t i = 0; i < inv.size() && remainingGold > 0; ++i) {
        Item& it = inv[i];
        if (it.shopPrice <= 0) continue;
        if (it.shopDepth <= 0 || it.shopDepth == depth_) continue;
        payForItem(it);
    }

    // Pay down any additional bill for goods already consumed/destroyed.
    auto payBill = [&](int d) {
        if (remainingGold <= 0) return;
        if (d < 1 || d > DUNGEON_MAX_DEPTH) return;
        int& bill = shopDebtLedger_[d];
        if (bill <= 0) return;

        const int pay = std::min(bill, remainingGold);
        bill -= pay;
        remainingGold -= pay;
        spent += pay;
    };

    payBill(depth_);
    for (int d = 1; d <= DUNGEON_MAX_DEPTH && remainingGold > 0; ++d) {
        if (d == depth_) continue;
        payBill(d);
    }

    if (spent <= 0) {
        pushMsg("YOU CANNOT PAY FOR ANYTHING RIGHT NOW.", MessageKind::Info, true);
        return false;
    }

    (void)spendGoldFromInv(inv, spent);

    const int stillOwe = shopDebtTotal();
    if (stillOwe <= 0) {
        pushMsg("YOU PAY " + std::to_string(spent) + " GOLD. ALL DEBTS ARE CLEARED.", MessageKind::Success, true);
        standDownMerchantGuild();
    } else {
        pushMsg("YOU PAY " + std::to_string(spent) + " GOLD. YOU STILL OWE " + std::to_string(stillOwe) + " GOLD.", MessageKind::Info, true);
    }

    // Paying takes a turn.
    advanceAfterPlayerAction();
    return true;
}

bool Game::payAtCamp() {
    if (gameOver || gameWon) return false;

    // Camp hub is a separate branch; don't rely on depth==0 to identify it.
    if (branch_ != DungeonBranch::Camp) {
        pushMsg("YOU MUST BE AT CAMP TO SETTLE YOUR DEBTS.", MessageKind::Info, true);
        return false;
    }

    auto standDownMerchantGuild = [&]() {
        // Current level (camp) shouldn't have shopkeepers/guards, but keep it symmetric.
        setShopkeepersAlerted(ents, playerId_, player().pos, false);
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != playerId_ && e.hp > 0 && e.kind == EntityKind::Guard;
        }), ents.end());

        // Stored levels: calm shopkeepers + remove guards.
        for (auto& [d, st] : levels) {
            for (auto& e : st.monsters) {
                if (e.hp <= 0) continue;
                if (e.kind == EntityKind::Shopkeeper) {
                    e.alerted = false;
                }
            }

            st.monsters.erase(std::remove_if(st.monsters.begin(), st.monsters.end(), [&](const Entity& e) {
                return e.hp > 0 && e.kind == EntityKind::Guard;
            }), st.monsters.end());
        }

        // Trapdoor fallers are keyed by (branch, depth), so remove guards across all entries.
        for (auto it = trapdoorFallers_.begin(); it != trapdoorFallers_.end(); ) {
            auto& q = it->second;
            q.erase(std::remove_if(q.begin(), q.end(), [&](const Entity& e) {
                return e.hp > 0 && e.kind == EntityKind::Guard;
            }), q.end());
            if (q.empty()) {
                it = trapdoorFallers_.erase(it);
            } else {
                ++it;
            }
        }

        merchantGuildAlerted_ = false;
    };

    const int owedTotal = shopDebtTotal();
    if (owedTotal <= 0) {
        pushMsg("YOU OWE NOTHING.", MessageKind::Info, true);
        if (merchantGuildAlerted_) {
            // Safety: if the guild is flagged as alerted but there is no debt, stand down.
            standDownMerchantGuild();
        }
        return false;
    }

    const int availableGold = countGold(inv);
    if (availableGold <= 0) {
        pushMsg("YOU HAVE NO GOLD TO PAY.", MessageKind::Info, true);
        return false;
    }

    int spent = 0;
    int remainingGold = availableGold;

    auto payForItem = [&](Item& it) {
        if (remainingGold <= 0) return;
        if (it.shopPrice <= 0 || it.shopDepth <= 0) return;

        const int perUnit = it.shopPrice;
        if (perUnit <= 0) return;

        if (isStackable(it.kind) && it.count > 1) {
            const int canUnits = std::min(it.count, remainingGold / perUnit);
            if (canUnits <= 0) return;

            const int pay = canUnits * perUnit;
            remainingGold -= pay;
            spent += pay;

            if (canUnits == it.count) {
                it.shopPrice = 0;
                it.shopDepth = 0;
            } else {
                it.count -= canUnits;
                Item paid = it;
                paid.count = canUnits;
                paid.shopPrice = 0;
                paid.shopDepth = 0;
                if (!tryStackItem(inv, paid)) {
                    paid.id = nextItemId++;
                    inv.push_back(paid);
                }
            }
        } else {
            const int pay = std::min(perUnit, remainingGold);
            it.shopPrice -= pay;
            remainingGold -= pay;
            spent += pay;

            if (it.shopPrice <= 0) {
                it.shopPrice = 0;
                it.shopDepth = 0;
            }
        }
    };

    auto payBill = [&](int d) {
        if (remainingGold <= 0) return;
        if (d < 1 || d > DUNGEON_MAX_DEPTH) return;
        int& bill = shopDebtLedger_[d];
        if (bill <= 0) return;

        const int pay = std::min(bill, remainingGold);
        bill -= pay;
        remainingGold -= pay;
        spent += pay;
    };

    // Camp has no "current shop depth", so pay debts from shallow->deep for predictability.
    for (int d = 1; d <= DUNGEON_MAX_DEPTH && remainingGold > 0; ++d) {
        for (size_t i = 0; i < inv.size() && remainingGold > 0; ++i) {
            Item& it = inv[i];
            if (it.shopPrice <= 0) continue;
            if (it.shopDepth != d) continue;
            payForItem(it);
        }
        payBill(d);
    }

    if (spent <= 0) {
        pushMsg("YOU CANNOT PAY FOR ANYTHING RIGHT NOW.", MessageKind::Info, true);
        return false;
    }

    (void)spendGoldFromInv(inv, spent);

    const int stillOwe = shopDebtTotal();
    if (stillOwe <= 0) {
        pushMsg("YOU PAY " + std::to_string(spent) + " GOLD TO THE MERCHANT GUILD. ALL DEBTS ARE CLEARED.", MessageKind::Success, true);
        standDownMerchantGuild();
    } else {
        pushMsg("YOU PAY " + std::to_string(spent) + " GOLD TO THE MERCHANT GUILD. YOU STILL OWE " + std::to_string(stillOwe) + " GOLD.", MessageKind::Info, true);
    }

    // Paying takes a turn.
    advanceAfterPlayerAction();
    return true;
}

void Game::showDebtLedger() {
    const int owedTotal = shopDebtTotal();
    if (owedTotal <= 0) {
        pushSystemMessage("YOU OWE NOTHING.");
        return;
    }

    std::vector<int> perDepth(static_cast<size_t>(DUNGEON_MAX_DEPTH + 1), 0);

    // Unpaid items currently in inventory.
    for (const auto& it : inv) {
        if (it.shopPrice <= 0) continue;
        if (it.shopDepth <= 0 || it.shopDepth > DUNGEON_MAX_DEPTH) continue;
        const int c = std::max(1, it.count);
        const int add = it.shopPrice * c;
        if (add > 0) perDepth[static_cast<size_t>(it.shopDepth)] += add;
    }

    // Extra bill for consumed/destroyed goods.
    for (int d = 1; d <= DUNGEON_MAX_DEPTH; ++d) {
        const int bill = shopDebtLedger_[d];
        if (bill > 0) perDepth[static_cast<size_t>(d)] += bill;
    }

    std::ostringstream ss;
    ss << "DEBT:";
    bool any = false;
    for (int d = 1; d <= DUNGEON_MAX_DEPTH; ++d) {
        const int v = perDepth[static_cast<size_t>(d)];
        if (v <= 0) continue;
        ss << "  D" << d << ":" << v << "G";
        any = true;
    }
    if (!any) {
        ss << " (UNKNOWN)";
    }
    ss << "  TOTAL:" << owedTotal << "G";
    pushSystemMessage(ss.str());

    if (merchantGuildAlerted_) {
        pushSystemMessage("MERCHANT GUILD: ALERTED.");
    }
}




bool Game::digInDirection(int dx, int dy) {
    if (gameOver || gameWon) return false;

    dx = clampi(dx, -1, 1);
    dy = clampi(dy, -1, 1);

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

    // Confusion can scramble the dig direction.
    if (player().effects.confusionTurns > 0) {
        static const int dirs[8][2] = { {0,-1},{0,1},{-1,0},{1,0},{-1,-1},{1,-1},{-1,1},{1,1} };
        const int i = rng.range(0, 7);
        dx = dirs[i][0];
        dy = dirs[i][1];
        pushMsg("YOU SWING THE PICKAXE WILDLY!", MessageKind::Warning, true);
    }

    // Prevent digging diagonally "through" a blocked corner.
    if (dx != 0 && dy != 0 && !diagonalPassable(dung, src, dx, dy)) {
        pushMsg("YOU CAN'T REACH AROUND THE CORNER.", MessageKind::Info, true);
        return false;
    }

    const Vec2i p{ src.x + dx, src.y + dy };
    if (!dung.inBounds(p.x, p.y)) {
        pushMsg("YOU CAN'T DIG THERE.", MessageKind::Info, true);
        return false;
    }

    // Attempting to dig always costs a turn (like lockpicking), even if nothing happens.
    // Substrate materials modulate how loud the digging is.
    dung.ensureMaterials(seed_, branch_, depth_, dungeonMaxDepth());
    const TerrainMaterial digMat = dung.materialAtCached(p.x, p.y);
    int digNoise = 14 + terrainMaterialFx(digMat).digNoiseDelta;
    digNoise = clampi(digNoise, 6, 20);
    emitNoise(src, digNoise);

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

bool Game::throwTorchInDirection(int dx, int dy) {
    if (gameOver || gameWon) return false;

    if (dx == 0 && dy == 0) {
        pushMsg("THROW WHERE?", MessageKind::Info, true);
        return false;
    }

    // Find a lit torch in inventory.
    int torchIdx = -1;
    for (size_t i = 0; i < inv.size(); ++i) {
        const Item& it = inv[i];
        if (it.kind == ItemKind::TorchLit && it.charges > 0) {
            torchIdx = static_cast<int>(i);
            break;
        }
    }

    if (torchIdx < 0) {
        pushMsg("YOU HAVE NO LIT TORCH.", MessageKind::Warning, true);
        return false;
    }

    // Remove the torch from inventory (it becomes the projectile template / will land on the ground).
    Item thrown = inv[static_cast<size_t>(torchIdx)];
    inv.erase(inv.begin() + torchIdx);
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    // Range is similar to throwing a rock, but slightly shorter (torches are awkward).
    const int range = std::max(2, throwRangeFor(player(), AmmoKind::Rock) - 1);

    const Vec2i src = player().pos;
    const Vec2i dst{ src.x + dx * range, src.y + dy * range };

    // Attack/aim bonuses mimic unarmed throwing (used for rocks/arrows when no ranged weapon is ready).
    const int atkBonus = player().baseAtk - 1 + playerAgility();
    const int dmgBonus = 0;

    attackRanged(playerMut(), dst, range, atkBonus, dmgBonus, ProjectileKind::Torch, /*fromPlayer=*/true, &thrown);
    advanceAfterPlayerAction();
    return true;
}

bool Game::engraveHere(const std::string& rawText) {
    if (gameOver || gameWon) return false;

    const Vec2i pos = player().pos;

    if (!dung.inBounds(pos.x, pos.y) || !dung.isWalkable(pos.x, pos.y)) {
        pushMsg("YOU CAN'T ENGRAVE HERE.", MessageKind::Warning, true);
        return false;
    }

    std::string text = trim(rawText);
    if (text.empty()) {
        pushMsg("WHAT DO YOU WANT TO ENGRAVE?", MessageKind::Info, true);
        return false;
    }

    // Keep message log and look UI readable.
    if (text.size() > 72) text.resize(72);

    // Warding word: classic NetHack nod.
    const std::string canon = toUpper(text);
    const bool isWard = (canon == "ELBERETH");

    // For wards, durability depends on what you're holding.
    uint8_t strength = 255; // permanent for non-wards
    if (isWard) {
        int uses = 3;
        if (const Item* w = equippedMelee()) {
            if (w->kind == ItemKind::Pickaxe) {
                uses = 7;
            } else if (isMeleeWeapon(w->kind)) {
                uses = 5;
            }
        }
        strength = static_cast<uint8_t>(std::clamp(uses, 1, 254));
    }

    // Replace an existing engraving on this tile, otherwise add a new one.
    for (auto& e : engravings_) {
        if (e.pos == pos) {
            e.text = text;
            e.isWard = isWard;
            e.isGraffiti = false;
            e.strength = strength;
            pushMsg(isWard ? "YOU ENGRAVE THE WARDING WORD." : "YOU ENGRAVE A MESSAGE INTO THE FLOOR.", MessageKind::Info, true);
            advanceAfterPlayerAction();
            return true;
        }
    }

    // Keep the list bounded.
    constexpr size_t kMaxEngravingsPerFloor = 128;
    if (engravings_.size() >= kMaxEngravingsPerFloor) {
        // Prefer to drop an old graffiti entry first.
        auto it = std::find_if(engravings_.begin(), engravings_.end(), [](const Engraving& e) { return e.isGraffiti; });
        if (it != engravings_.end()) {
            engravings_.erase(it);
        } else {
            engravings_.erase(engravings_.begin());
        }
    }

    Engraving e;
    e.pos = pos;
    e.text = text;
    e.isWard = isWard;
    e.isGraffiti = false;
    e.strength = strength;
    engravings_.push_back(std::move(e));

    pushMsg(isWard ? "YOU ENGRAVE THE WARDING WORD." : "YOU ENGRAVE A MESSAGE INTO THE FLOOR.", MessageKind::Info, true);
    advanceAfterPlayerAction();
    return true;
}



bool Game::drinkFromFountain() {
    Entity& p = playerMut();
    if (!dung.inBounds(p.pos.x, p.pos.y)) return false;

    Tile& tile = dung.at(p.pos.x, p.pos.y);
    if (tile.type != TileType::Fountain) {
        pushMsg("THERE IS NO FOUNTAIN HERE.", MessageKind::Info, true);
        return false;
    }

    // Drinking is fairly loud (splashing / slurping), but not as loud as combat.
    emitNoise(p.pos, 6);

    pushMsg("YOU DRINK FROM THE FOUNTAIN.", MessageKind::Info, true);

    auto maybeDryUp = [&]() {
        // NetHack-inspired: fountains often dry up after use.
        if (tile.type == TileType::Fountain && rng.chance(0.33f)) {
            tile.type = TileType::Floor;
            pushMsg("THE FOUNTAIN DRIES UP.", MessageKind::System, true);
        }
    };

    auto applyHungerDelta = [&](int delta) {
        if (!hungerEnabled_) return;
        if (hungerMax <= 0) hungerMax = 800;

        const int beforeState = hungerStateFor(hunger, hungerMax);
        hunger = clampi(hunger + delta, 0, hungerMax);
        const int afterState = hungerStateFor(hunger, hungerMax);

        if (afterState < beforeState) {
            if (beforeState >= 2 && afterState < 2) {
                pushMsg("YOU FEEL LESS STARVED.", MessageKind::System, true);
            } else if (beforeState >= 1 && afterState == 0) {
                pushMsg("YOU FEEL SATIATED.", MessageKind::System, true);
            }
        } else if (afterState > beforeState) {
            if (afterState == 1) {
                pushMsg("YOU FEEL HUNGRY.", MessageKind::System, true);
            } else if (afterState >= 2) {
                pushMsg("YOU ARE STARVING!", MessageKind::Warning, true);
            }
        }

        // Sync throttling so the next hunger tick doesn't immediately re-announce.
        hungerStatePrev = hungerStateFor(hunger, hungerMax);
    };

    auto findSpawnAdj = [&]() -> Vec2i {
        std::vector<Vec2i> opts;
        opts.reserve(8);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                Vec2i q{p.pos.x + dx, p.pos.y + dy};
                if (!dung.inBounds(q.x, q.y)) continue;
                if (q == dung.stairsUp || q == dung.stairsDown) continue;
                if (!dung.isWalkable(q.x, q.y)) continue;
                if (entityAt(q.x, q.y) != nullptr) continue;
                opts.push_back(q);
            }
        }
        if (opts.empty()) return {-1, -1};
        const int i = rng.range(0, static_cast<int>(opts.size()) - 1);
        return opts[static_cast<size_t>(i)];
    };

    auto spawnHostile = [&](EntityKind k) -> bool {
        const Vec2i sp = findSpawnAdj();
        if (!dung.inBounds(sp.x, sp.y)) return false;

        Entity& m = spawnMonster(k, sp, /*groupId=*/0, /*allowGear=*/false);
        m.alerted = true;
        m.lastKnownPlayerPos = p.pos;
        m.lastKnownPlayerAge = 0;
        return true;
    };

    // Roll a NetHack-ish 1d30 table (simplified / adapted to this game's mechanics).
    const int r = rng.range(0, 29);

    // Common outcomes.
    if (r < 9) {
        // 9/30: refresh
        pushMsg("THE COOL DRAUGHT REFRESHES YOU.", MessageKind::Success, true);

        // Small heal, tiny mana refill, and a bit of nourishment.
        if (p.hp < p.hpMax) {
            const int heal = 2 + rng.range(0, 4);
            p.hp = std::min(p.hpMax, p.hp + heal);
        }

        // Clear some common ailments.
        if (p.effects.poisonTurns > 0) p.effects.poisonTurns = 0;
        if (p.effects.burnTurns > 0) p.effects.burnTurns = 0;
        if (p.effects.confusionTurns > 0) p.effects.confusionTurns = 0;

        // A little mana back.
        const int manaMax = playerManaMax();
        if (manaMax > 0) {
            mana_ = std::min(manaMax, mana_ + 2 + rng.range(0, 2));
        }

        applyHungerDelta(30);
        maybeDryUp();
        return true;
    }

    if (r < 18) {
        // 9/30: no effect
        pushMsg("THIS TEPID WATER IS TASTELESS.", MessageKind::Info, true);
        maybeDryUp();
        return true;
    }

    // Rare outcomes.
    switch (r) {
        case 18: {
            // "Self-knowledge" / detect monsters (lite): report nearby hostiles.
            int hostile = 0;
            for (const auto& e : ents) {
                if (e.id == playerId_) continue;
                if (e.hp <= 0) continue;
                if (e.friendly) continue;
                if (e.kind == EntityKind::Shopkeeper) continue;
                if (manhattan(e.pos, p.pos) <= 14) hostile += 1;
            }

            if (hostile <= 0) {
                pushMsg("YOU FEEL SELF-KNOWLEDGEABLE... AND ALONE.", MessageKind::Info, true);
            } else if (hostile == 1) {
                pushMsg("YOU SENSE A CREATURE NEARBY.", MessageKind::Info, true);
            } else {
                pushMsg("YOU SENSE " + std::to_string(hostile) + " CREATURES NEARBY.", MessageKind::Info, true);
            }

            // A tiny wisdom/perception bump (mechanically: brief vision).
            p.effects.visionTurns = std::max(p.effects.visionTurns, 10);
            recomputeFov();
            maybeDryUp();
            return true;
        }

        case 19: {
            // "Stalking image" -> brief heightened senses.
            pushMsg("YOU SEE AN IMAGE OF SOMEONE STALKING YOU... BUT IT FADES.", MessageKind::Warning, true);
            p.effects.visionTurns = std::max(p.effects.visionTurns, 18);
            recomputeFov();
            maybeDryUp();
            return true;
        }

        case 20: {
            // Find some coins.
            const int amt = 3 + rng.range(0, 4) + std::min(12, depth_);

            Item g;
            g.id = nextItemId++;
            g.kind = ItemKind::Gold;
            g.count = amt;
            g.spriteSeed = rng.nextU32();

            if (!tryStackItem(inv, g)) {
                inv.push_back(g);
            }

            pushMsg("YOU FIND SOME COINS IN THE WATER!", MessageKind::Loot, true);
            maybeDryUp();
            return true;
        }

        case 21: {
            // Bad breath: briefly frighten nearby hostiles.
            pushMsg("THIS WATER GIVES YOU BAD BREATH!", MessageKind::Warning, true);

            int affected = 0;
            for (auto& e : ents) {
                if (e.id == playerId_) continue;
                if (e.hp <= 0) continue;
                if (e.friendly) continue;
                if (e.kind == EntityKind::Shopkeeper) continue;
                if (manhattan(e.pos, p.pos) > 10) continue;
                e.effects.fearTurns = std::max(e.effects.fearTurns, 4);
                affected += 1;
            }

            if (affected > 0) {
                pushMsg("MONSTERS RECOIL FROM YOU!", MessageKind::Info, true);
            }

            maybeDryUp();
            return true;
        }

        case 22: {
            // Bad water.
            pushMsg("THIS WATER'S NO GOOD!", MessageKind::Warning, true);

            // Make you hungrier and a bit confused.
            applyHungerDelta(-40);
            p.effects.confusionTurns = std::max(p.effects.confusionTurns, 6 + rng.range(0, 5));

            // Small chance to also poison.
            if (rng.chance(0.40f)) {
                p.effects.poisonTurns = std::max(p.effects.poisonTurns, 6 + rng.range(0, 5));
            }

            maybeDryUp();
            return true;
        }

        case 23: {
            // Water gushes forth: extinguish nearby flames.
            pushMsg("WATER GUSHES FORTH!", MessageKind::Warning, true);
            emitNoise(p.pos, 10);

            if (p.effects.burnTurns > 0) {
                p.effects.burnTurns = 0;
                pushMsg("THE WATER EXTINGUISHES THE FLAMES.", MessageKind::Success, true);
            }

            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (fireField_.size() == expect) {
                auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * dung.width + x); };

                constexpr int radius = 2;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int x = p.pos.x + dx;
                        const int y = p.pos.y + dy;
                        if (!dung.inBounds(x, y)) continue;
                        const size_t ii = idx(x, y);
                        if (ii >= fireField_.size()) continue;
                        fireField_[ii] = 0;
                    }
                }
            }

            maybeDryUp();
            return true;
        }

        case 24: {
            // Snakes!
            pushMsg("SOMETHING WRIGGLES OUT OF THE FOUNTAIN!", MessageKind::Warning, true);
            const int n = 1 + rng.range(0, 1) + (depth_ >= 6 ? 1 : 0);
            int spawned = 0;
            for (int i = 0; i < n; ++i) {
                if (spawnHostile(EntityKind::Snake)) spawned += 1;
            }
            if (spawned <= 0) {
                pushMsg("...BUT IT SLIPS AWAY.", MessageKind::Info, true);
            }
            maybeDryUp();
            return true;
        }

        case 25: {
            // Water nymph analogue.
            pushMsg("A SLY CREATURE EMERGES FROM THE WATER!", MessageKind::Warning, true);
            (void)spawnHostile(EntityKind::Leprechaun);
            maybeDryUp();
            return true;
        }

        case 26: {
            // Water demon analogue.
            pushMsg("A MALEVOLENT PRESENCE RISES FROM THE FOUNTAIN!", MessageKind::Warning, true);
            if (depth_ >= 7) (void)spawnHostile(EntityKind::Ghost);
            else (void)spawnHostile(EntityKind::Slime);
            maybeDryUp();
            return true;
        }

        case 27: {
            // Polluted water -> poison.
            pushMsg("THE WATER BURNS YOUR THROAT!", MessageKind::Warning, true);
            p.effects.poisonTurns = std::max(p.effects.poisonTurns, 8 + rng.range(0, 6));
            maybeDryUp();
            return true;
        }

        case 28: {
            // Big boon.
            pushMsg("WOW! THIS MAKES YOU FEEL GREAT!", MessageKind::Success, true);

            p.hp = p.hpMax;
            p.effects.poisonTurns = 0;
            p.effects.burnTurns = 0;
            p.effects.confusionTurns = 0;
            p.effects.hallucinationTurns = 0;

            p.effects.regenTurns = std::max(p.effects.regenTurns, 12);

            const int manaMax = playerManaMax();
            if (manaMax > 0) mana_ = manaMax;

            applyHungerDelta(80);

            // This kind of magic tends to exhaust the fountain.
            if (tile.type == TileType::Fountain) {
                tile.type = TileType::Floor;
                pushMsg("THE FOUNTAIN RUNS DRY.", MessageKind::System, true);
            }
            return true;
        }

        case 29:
        default: {
            pushMsg("A STRANGE TINGLING RUNS UP YOUR ARM.", MessageKind::Info, true);
            maybeDryUp();
            return true;
        }
    }
}
