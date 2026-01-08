#include "game.hpp"

#include "combat_rules.hpp"

#include "grid_utils.hpp"
#include "pathfinding.hpp"

#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace {

const char* kindName(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return "YOU";
        case EntityKind::Goblin: return "GOBLIN";
        case EntityKind::Orc: return "ORC";
        case EntityKind::Bat: return "BAT";
        case EntityKind::Slime: return "SLIME";
        case EntityKind::SkeletonArcher: return "SKELETON";
        case EntityKind::KoboldSlinger: return "KOBOLD";
        case EntityKind::Wolf: return "WOLF";
        case EntityKind::Dog: return "DOG";
        case EntityKind::Ghost: return "GHOST";
        case EntityKind::Leprechaun: return "LEPRECHAUN";
        case EntityKind::Zombie: return "ZOMBIE";
        case EntityKind::Troll: return "TROLL";
        case EntityKind::Wizard: return "WIZARD";
        case EntityKind::Snake: return "SNAKE";
        case EntityKind::Spider: return "SPIDER";
        case EntityKind::Ogre: return "OGRE";
        case EntityKind::Mimic: return "MIMIC";
        case EntityKind::Shopkeeper: return "SHOPKEEPER";
        case EntityKind::Minotaur: return "MINOTAUR";
        default: return "THING";
    }
}



int smellFor(EntityKind k) {
    switch (k) {
        // Animals and bestial monsters track well by scent.
        case EntityKind::Wolf: return 12;
        case EntityKind::Dog: return 11;
        case EntityKind::Snake: return 10;
        case EntityKind::Spider: return 9;
        // Some brutes have a decent nose.
        case EntityKind::Troll: return 7;
        case EntityKind::Ogre: return 6;
        default: return 0;
    }
}

} // namespace

void Game::monsterTurn() {
    if (isFinished()) return;

    const Entity& p = player();

    // Friendly companions (dog, tamed beasts, etc.)
    std::vector<Entity*> allies;
    allies.reserve(8);
    for (auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (e.friendly) allies.push_back(&e);
    }
    const int W = dung.width;
    const int H = dung.height;

    auto idx = [&](int x, int y) { return y * W + x; };

    const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

    // Some monsters can bash through locked doors while hunting.
    // We model this in pathfinding by treating locked doors as passable
    // with a steep movement cost (representing repeated smash attempts).
    //
    // In addition, a few special entities are ethereal and can phase through
    // terrain entirely (e.g. bones ghosts).
    enum PathMode : int {
        Normal = 0,
        SmashLockedDoors = 1,
        Phasing = 2,
        Levitate = 3,
    };

    auto monsterCanBashLockedDoor = [&](EntityKind k) -> bool {
        // Keep conservative for balance: only heavy bruisers.
        return (k == EntityKind::Ogre || k == EntityKind::Troll || k == EntityKind::Minotaur);
    };

    auto passableForMode = [&](int x, int y, int mode) -> bool {
        if (!dung.inBounds(x, y)) return false;

        // Ethereal entities ignore terrain restrictions (but still can't leave the map).
        if (mode == PathMode::Phasing) return true;

        if (dung.isPassable(x, y)) return true;
        if (mode == PathMode::SmashLockedDoors && dung.isDoorLocked(x, y)) return true;
        if (mode == PathMode::Levitate && dung.at(x, y).type == TileType::Chasm) return true;
        return false;
    };

    auto stepCostForMode = [&](int x, int y, int mode) -> int {
        if (!dung.inBounds(x, y)) return 0;

        int cost = 1;

        // Phasing movement still consumes time, but we bias the pathfinder
        // to prefer open corridors over "living" inside solid walls.
        if (mode == PathMode::Phasing) {
            cost = dung.isWalkable(x, y) ? 1 : 2;
        } else {
            const TileType t = dung.at(x, y).type;
            switch (t) {
                case TileType::DoorClosed:
                    // Monsters open doors as an action, then step through next.
                    cost = 2;
                    break;
                case TileType::DoorLocked:
                    // Smashing locks is much slower than opening an unlocked door.
                    cost = (mode == PathMode::SmashLockedDoors) ? 4 : 0;
                    break;
                default:
                    cost = 1;
                    break;
            }
        }

        if (cost <= 0) return cost;

        // Environmental hazards:
        // - Fire is an obvious hazard: monsters generally try to route around it.
        // - Confusion gas is also undesirable (unless it is the only way through).
        // This mirrors player auto-travel's strong preference to avoid fire.
        const uint8_t f = this->fireAt(x, y);
        if (f > 0u) {
            // Strongly discourage stepping onto burning tiles, but don't hard-block.
            cost += 10 + static_cast<int>(f) / 16; // +10..+25
        }

        const uint8_t g = this->confusionGasAt(x, y);
        if (g > 0u) {
            // Moderate penalty so monsters avoid lingering gas clouds when possible.
            cost += 6 + static_cast<int>(g) / 32; // +6..+13
        }

        return cost;
    };

    auto diagOkForMode = [&](int fromX, int fromY, int dx, int dy, int mode) -> bool {
        // Cardinal moves never need special casing.
        if (dx == 0 || dy == 0) return true;

        if (mode == PathMode::Phasing) return true;

        if (mode == PathMode::Levitate) {
            // Allow diagonal movement as long as the two adjacent cardinal tiles
            // are passable in this mode (including chasms).
            return passableForMode(fromX + dx, fromY, mode) && passableForMode(fromX, fromY + dy, mode);
        }

        return diagonalPassable(dung, {fromX, fromY}, dx, dy);
    };

    // Cache cost-to-target maps for this turn.
    // Keyed by (target tile index, path mode) to keep caching correct.
    // These maps are "minimum turns" approximations (doors/locks cost extra).
    std::unordered_map<int, std::vector<int>> costCache;
    costCache.reserve(32);

    auto getCostMap = [&](Vec2i target, int mode) -> const std::vector<int>& {
        // Key by (target tile index, path mode). We pack the mode in the low bits.
        // 2 bits are enough for our current modes.
        const int key = (idx(target.x, target.y) << 2) | (mode & 3);
        auto it = costCache.find(key);
        if (it != costCache.end()) return it->second;

        auto passable = [&, mode](int x, int y) -> bool { return passableForMode(x, y, mode); };
        auto stepCost = [&, mode](int x, int y) -> int { return stepCostForMode(x, y, mode); };
        auto diagOk = [&, mode](int fromX, int fromY, int dx, int dy) -> bool { return diagOkForMode(fromX, fromY, dx, dy, mode); };

        auto [it2, inserted] = costCache.emplace(key, dijkstraCostToTarget(W, H, target, passable, stepCost, diagOk));
        (void)inserted;
        return it2->second;
    };

    auto bestStepToward = [&](const Entity& m, const std::vector<int>& costMap, int mode) -> Vec2i {
        Vec2i best = m.pos;
        int bestScore = std::numeric_limits<int>::max();
        for (auto& dv : dirs) {
            int dx = dv[0], dy = dv[1];
            const int nx = m.pos.x + dx;
            const int ny = m.pos.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagOkForMode(m.pos.x, m.pos.y, dx, dy, mode)) continue;
            if (!passableForMode(nx, ny, mode)) continue;
            if (entityAt(nx, ny)) continue;

            const int cToTarget = costMap[static_cast<size_t>(idx(nx, ny))];
            if (cToTarget < 0) continue;

            const int step = stepCostForMode(nx, ny, mode);
            if (step <= 0) continue;

            // Choose the move that minimizes "step + remaining" cost.
            const int score = step + cToTarget;
            if (score < bestScore) {
                bestScore = score;
                best = {nx, ny};
            }
        }
        return best;
    };

    auto bestStepAway = [&](const Entity& m, const std::vector<int>& costMap, int mode) -> Vec2i {
        Vec2i best = m.pos;
        int bestD = -1;
        for (auto& dv : dirs) {
            int dx = dv[0], dy = dv[1];
            const int nx = m.pos.x + dx;
            const int ny = m.pos.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagOkForMode(m.pos.x, m.pos.y, dx, dy, mode)) continue;
            if (!passableForMode(nx, ny, mode)) continue;
            if (entityAt(nx, ny)) continue;

            const int d0 = costMap[static_cast<size_t>(idx(nx, ny))];
            if (d0 >= 0 && d0 > bestD) {
                bestD = d0;
                best = {nx, ny};
            }
        }
        return best;
    };

    constexpr int LOS_MANHATTAN = 12;
    constexpr int TRACK_TURNS = 16;

    // Sneak mode: while sneaking, reduce the range at which most monsters can visually
    // notice the player. This stacks with the existing noise + scent systems.
    // Heavy armor and encumbrance reduce the benefit; keen-sensed monsters partially ignore it.
    int sneakSightStealth = 0;
    if (isSneaking() && p.effects.invisTurns <= 0) {
        const int agi = std::max(0, playerAgility());
        // Base stealth from agility: 2..6 tiles of LOS reduction.
        sneakSightStealth = 2 + std::min(4, agi / 4);

        // Heavy armor makes sneaking less effective.
        if (const Item* a = equippedArmor()) {
            if (a->kind == ItemKind::ChainArmor) sneakSightStealth -= 1;
            if (a->kind == ItemKind::PlateArmor) sneakSightStealth -= 2;
        }

        // Encumbrance makes it harder to sneak effectively.
        if (encumbranceEnabled_) {
            switch (burdenState()) {
                case BurdenState::Unburdened: break;
                case BurdenState::Burdened:   sneakSightStealth -= 1; break;
                case BurdenState::Stressed:   sneakSightStealth -= 2; break;
                case BurdenState::Strained:   sneakSightStealth -= 3; break;
                case BurdenState::Overloaded: sneakSightStealth -= 3; break;
            }
        }

        if (sneakSightStealth < 0) sneakSightStealth = 0;
    }

    // Energy scheduling constants.
    constexpr int ENERGY_PER_ACTION = 100;
    constexpr int MAX_ACTIONS_PER_TURN = 3; // safety cap: avoids runaway loops if speed is ever mis-set

    // Gold pickup helper used by FETCH-mode allies.
    auto gainGold = [&](int amount) {
        if (amount <= 0) return;
        Item g;
        g.id = nextItemId++;
        g.kind = ItemKind::Gold;
        g.count = amount;
        g.charges = 0;
        g.enchant = 0;
        g.buc = 0;
        g.spriteSeed = rng.nextU32();
        g.shopPrice = 0;
        g.shopDepth = 0;

        if (!tryStackItem(inv, g)) {
            inv.push_back(g);
        }
    };

    auto pickupGoldAt = [&](Entity& ally) -> bool {
        for (size_t gi = 0; gi < ground.size(); ++gi) {
            GroundItem& g = ground[gi];
            if (g.pos != ally.pos) continue;
            if (g.item.kind != ItemKind::Gold) continue;
            if (g.item.shopPrice > 0) continue; // don't steal shop stock
            const int amt = std::max(0, g.item.count);
            if (amt <= 0) continue;

            // Remove the pile and have the ally carry it.
            ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(gi));
            ally.stolenGold += amt;

            const bool vis = dung.inBounds(ally.pos.x, ally.pos.y) && dung.at(ally.pos.x, ally.pos.y).visible;
            if (vis) {
                std::ostringstream ss;
                ss << "YOUR " << kindName(ally.kind) << " PICKS UP " << amt << " GOLD.";
                pushMsg(ss.str(), MessageKind::Loot, true);
            }
            return true;
        }
        return false;
    };

    auto depositAllyGold = [&](Entity& ally) -> bool {
        if (ally.stolenGold <= 0) return false;
        if (!isAdjacent8(ally.pos, p.pos)) return false;

        const int amt = ally.stolenGold;
        ally.stolenGold = 0;
        gainGold(amt);

        const bool vis = dung.inBounds(ally.pos.x, ally.pos.y) && dung.at(ally.pos.x, ally.pos.y).visible;
        if (vis) {
            std::ostringstream ss;
            ss << "YOUR " << kindName(ally.kind) << " BRINGS YOU " << amt << " GOLD.";
            pushMsg(ss.str(), MessageKind::Loot, true);
        }
        return true;
    };

    auto findVisibleGoldTarget = [&](const Entity& ally, Vec2i& out) -> bool {
        int bestMan = 999999;
        Vec2i best{-1, -1};

        for (const auto& g : ground) {
            if (g.item.kind != ItemKind::Gold) continue;
            if (g.item.shopPrice > 0) continue;
            if (g.item.count <= 0) continue;

            if (!dung.inBounds(g.pos.x, g.pos.y)) continue;
            const Tile& t0 = dung.at(g.pos.x, g.pos.y);
            if (!t0.visible) continue;

            const int man = manhattan(ally.pos, g.pos);
            if (man < bestMan) {
                bestMan = man;
                best = g.pos;
            }
        }

        if (best.x < 0) return false;
        out = best;
        return true;
    };

    // Pack monsters will "reserve" adjacent-to-player tiles so they spread out
    // a bit more (reduced bumping / pileups).
    std::unordered_set<int> reservedAdj;
    reservedAdj.reserve(16);

    Vec2i reserveAnchor = p.pos;

    auto refreshPackAnchor = [&]() {
        // The player can now be forced-moved during monster turns (e.g. knockback).
        // If that happens, refresh any pack reservations anchored to the old position.
        if (p.pos != reserveAnchor) {
            reservedAdj.clear();
            reserveAnchor = p.pos;
        }
    };

    auto actOnce = [&](Entity& m, bool& agedThisTurn) {
        if (isFinished()) return;

        // QoL/robustness: older saves may have wolves without packAI set.
        if (m.kind == EntityKind::Wolf) {
            m.packAI = true;
        }

        refreshPackAnchor();

        // Ally AI: friendly companions (dog, tamed beasts, etc.).
        if (m.friendly) {
            // Lazily initialize / clear home anchors based on current order.
            if (m.allyOrder == AllyOrder::Stay || m.allyOrder == AllyOrder::Guard) {
                if (m.allyHomePos.x < 0) m.allyHomePos = m.pos;
            } else {
                if (m.allyHomePos.x >= 0) m.allyHomePos = {-1, -1};
            }

            // If an ally is adjacent and carrying gold, deliver it immediately.
            // (This consumes the ally's action for the turn.)
            if (depositAllyGold(m)) return;

            // FETCH: grab any gold you're standing on.
            if (m.allyOrder == AllyOrder::Fetch) {
                if (pickupGoldAt(m)) return;
            }

            // Look for the nearest hostile in line-of-sight.
            Entity* best = nullptr;
            int bestMan = 9999;

            int maxChase = LOS_MANHATTAN;
            if (m.allyOrder == AllyOrder::Stay) {
                maxChase = 8;
            } else if (m.allyOrder == AllyOrder::Fetch) {
                // When carrying loot, prefer returning instead of chasing fights.
                maxChase = (m.stolenGold > 0) ? 6 : 10;
            } else if (m.allyOrder == AllyOrder::Guard) {
                maxChase = LOS_MANHATTAN;
            }

            for (auto& e : ents) {
                if (e.id == playerId_) continue;
                if (e.hp <= 0) continue;
                if (e.friendly) continue;
                if (e.kind == EntityKind::Shopkeeper && !e.alerted) continue;

                // GUARD: ignore threats far from our anchor.
                if (m.allyOrder == AllyOrder::Guard && m.allyHomePos.x >= 0) {
                    if (chebyshev(e.pos, m.allyHomePos) > 8) continue;
                }

                const int man0 = manhattan(m.pos, e.pos);
                if (man0 > maxChase) continue;
                if (!dung.hasLineOfSight(m.pos.x, m.pos.y, e.pos.x, e.pos.y)) continue;

                if (man0 < bestMan) {
                    bestMan = man0;
                    best = &e;
                }
            }

            if (best) {
                if (isAdjacent8(m.pos, best->pos)) {
                    attackMelee(m, *best);
                    return;
                }

                const auto& costMap = getCostMap(best->pos, PathMode::Normal);
                const Vec2i step = bestStepToward(m, costMap, PathMode::Normal);
                if (step != m.pos) {
                    tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                }
                return;
            }

            // No visible hostiles: obey orders.
            if (m.allyOrder == AllyOrder::Stay) {
                // Stay: return to the anchor tile if displaced.
                if (m.allyHomePos.x >= 0 && m.pos != m.allyHomePos) {
                    const auto& costMap = getCostMap(m.allyHomePos, PathMode::Normal);
                    const Vec2i step = bestStepToward(m, costMap, PathMode::Normal);
                    if (step != m.pos) {
                        tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                    }
                }
                return;
            }

            if (m.allyOrder == AllyOrder::Guard) {
                // Guard: patrol near the anchor, and return if pulled too far away.
                const Vec2i home = (m.allyHomePos.x >= 0) ? m.allyHomePos : m.pos;
                const int guardRadius = 3;
                const int distHome = chebyshev(m.pos, home);

                if (distHome > guardRadius) {
                    const auto& costMap = getCostMap(home, PathMode::Normal);
                    const Vec2i step = bestStepToward(m, costMap, PathMode::Normal);
                    if (step != m.pos) {
                        tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                    }
                    return;
                }

                // Small random patrol step within the guard radius.
                if (rng.chance(0.22f)) {
                    Vec2i bestStep = m.pos;
                    // Try a handful of random directions to avoid deterministic jitter.
                    for (int tries = 0; tries < 12; ++tries) {
                        const int di = rng.range(0, 7);
                        const int nx = m.pos.x + dirs[di][0];
                        const int ny = m.pos.y + dirs[di][1];
                        if (!dung.inBounds(nx, ny)) continue;
                        if (!dung.isWalkable(nx, ny)) continue;
                        if (entityAt(nx, ny)) continue;
                        if (chebyshev(Vec2i{nx, ny}, home) > guardRadius) continue;
                        bestStep = {nx, ny};
                        break;
                    }
                    if (bestStep != m.pos) {
                        tryMove(m, bestStep.x - m.pos.x, bestStep.y - m.pos.y);
                    }
                }
                return;
            }

            if (m.allyOrder == AllyOrder::Fetch) {
                // If carrying gold, head back to the player to deliver.
                if (m.stolenGold > 0) {
                    const auto& costMap = getCostMap(p.pos, PathMode::Normal);
                    const Vec2i step = bestStepToward(m, costMap, PathMode::Normal);
                    if (step != m.pos) {
                        tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                    }
                    return;
                }

                Vec2i goldPos;
                if (findVisibleGoldTarget(m, goldPos)) {
                    if (goldPos == m.pos) {
                        (void)pickupGoldAt(m);
                        return;
                    }
                    const auto& costMap = getCostMap(goldPos, PathMode::Normal);
                    const Vec2i step = bestStepToward(m, costMap, PathMode::Normal);
                    if (step != m.pos) {
                        tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                        return;
                    }
                }
            }

            // Default: stick close to the player.
            const int dist = chebyshev(m.pos, p.pos);
            if (dist > 2) {
                const auto& costMap = getCostMap(p.pos, PathMode::Normal);
                const Vec2i step = bestStepToward(m, costMap, PathMode::Normal);
                if (step != m.pos) {
                    tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                }
            }
            return;
        }

// Peaceful shopkeepers don't hunt or wander.
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) {
            // Don't allow "banked" energy while peaceful.
            m.energy = 0;
            return;
        }

        const int man = manhattan(m.pos, p.pos);

        // Leprechaun: snatch loose gold on the floor (but never shop bookkeeping).
        if (m.kind == EntityKind::Leprechaun) {
            for (size_t gi = 0; gi < ground.size(); ++gi) {
                GroundItem& g = ground[gi];
                if (g.pos != m.pos) continue;
                if (g.item.kind != ItemKind::Gold) continue;
                if (g.item.count <= 0) continue;
                if (g.item.shopPrice > 0) continue;

                const int amt = g.item.count;
                m.stolenGold += amt;
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << "THE LEPRECHAUN SNATCHES " << amt << " GOLD!";
                    pushMsg(ss.str(), MessageKind::Warning, false);
                }
                emitNoise(m.pos, 9);

                ground.erase(ground.begin() + static_cast<std::ptrdiff_t>(gi));
                return;
            }
        }

        const int smellR = smellFor(m.kind);
        const uint8_t scentHere = (smellR > 0) ? scentAt(m.pos.x, m.pos.y) : 0u;

        auto bestScentStep = [&]() -> Vec2i {
            if (smellR <= 0) return m.pos;

            // Require a meaningful gradient to avoid oscillations on very faint scent.
            constexpr uint8_t TRACK_THRESHOLD = 32u;

            Vec2i best = m.pos;
            uint8_t bestV = scentHere;

            for (int di = 0; di < 8; ++di) {
                const int dx = dirs[di][0];
                const int dy = dirs[di][1];
                const int nx = m.pos.x + dx;
                const int ny = m.pos.y + dy;

                if (!dung.inBounds(nx, ny)) continue;
                if (!dung.isWalkable(nx, ny)) continue;

                if (dx != 0 && dy != 0 && !diagonalPassable(dung, m.pos, dx, dy)) continue;

                const uint8_t sv = scentAt(nx, ny);
                if (sv > bestV) {
                    bestV = sv;
                    best = {nx, ny};
                }
            }

            if (best != m.pos && bestV >= TRACK_THRESHOLD) return best;
            return m.pos;
        };

        int losLimit = LOS_MANHATTAN;
        if (sneakSightStealth > 0) {
            // Some monsters have especially keen senses and partially ignore stealth.
            int keen = 0;
            switch (m.kind) {
                case EntityKind::Bat:      keen = 2; break;
                case EntityKind::Wizard:   keen = 3; break;
                case EntityKind::Minotaur: keen = 1; break;
                default: break;
            }

            losLimit = LOS_MANHATTAN - sneakSightStealth + keen;
            if (losLimit < 4) losLimit = 4; // never fully "blind" at close range
            if (losLimit > LOS_MANHATTAN) losLimit = LOS_MANHATTAN;
        }

        bool seesPlayer = false;
        if (man <= losLimit) {
            seesPlayer = dung.hasLineOfSight(m.pos.x, m.pos.y, p.pos.x, p.pos.y);
        }

        const bool adj8 = isAdjacent8(m.pos, p.pos);
        // Invisibility: most monsters only notice you when adjacent.
        // Wizards are special-cased to still see invisible (but not through walls).
        if (p.effects.invisTurns > 0 && m.kind != EntityKind::Wizard) {
            seesPlayer = adj8;
        }

        // Darkness: if the player isn't lit, most monsters only notice you at very short range.
        if (seesPlayer && darknessActive()) {
            const bool playerLit = (tileLightLevel(p.pos.x, p.pos.y) > 0);
            const bool hasDarkVision = (m.kind == EntityKind::Bat || m.kind == EntityKind::Wizard || m.kind == EntityKind::Spider || m.kind == EntityKind::Minotaur);
            if (!playerLit && !hasDarkVision && man > 2) {
                seesPlayer = false;
            }
        }

        const bool wasAlerted = m.alerted;
        if (seesPlayer) {
            m.alerted = true;
            m.lastKnownPlayerPos = p.pos;
            m.lastKnownPlayerAge = 0;
            agedThisTurn = true;
        } else if (m.alerted && !agedThisTurn) {
            // If this monster has a nose and is currently standing in a reasonably fresh scent
            // trail, keep it "alerted" without aging out. This lets smell-capable monsters keep
            // tracking around corners even after visual contact is lost.
            if (smellR > 0 && scentHere >= 24u) {
                agedThisTurn = true;
            } else {
                if (m.lastKnownPlayerAge < 9999) m.lastKnownPlayerAge += 1;
                agedThisTurn = true;
            }
        }

        // Compatibility fallback: if something flagged the monster alerted but didn't provide a
        // last-known position (older saves or older code paths), assume the alert was to the
        // player's current location.
        if (m.alerted && m.lastKnownPlayerPos.x < 0) {
            m.lastKnownPlayerPos = p.pos;
            m.lastKnownPlayerAge = 0;
            agedThisTurn = true;
        }

        // Determine hunt target.
        Vec2i target{-1, -1};
        bool hunting = false;

        if (seesPlayer) {
            target = p.pos;
            hunting = true;
        } else if (m.alerted && m.lastKnownPlayerPos.x >= 0 && m.lastKnownPlayerPos.y >= 0 && (m.kind == EntityKind::Shopkeeper || m.lastKnownPlayerAge <= TRACK_TURNS)) {
            target = m.lastKnownPlayerPos;
            hunting = true;
        }
        else if (m.alerted && smellR > 0) {
            // Smell tracking fallback: if the monster has lost the player's exact trail but can
            // still pick up scent nearby, keep hunting.
            const Vec2i step = bestScentStep();
            if (step != m.pos) {
                target = step;
                hunting = true;
            }
        }

        // Fear makes monsters *want* to run from the player. Ensure they have a meaningful
        // target to run away from even if they weren't already hunting (e.g. player is
        // invisible but triggered a fear effect).
        if (m.effects.fearTurns > 0) {
            target = p.pos;
            hunting = true;
            m.alerted = true;
            m.lastKnownPlayerPos = p.pos;
            m.lastKnownPlayerAge = 0;
        }

        if (!hunting) {
            // Idle wander.
            m.alerted = false;
            m.lastKnownPlayerPos = {-1, -1};
            m.lastKnownPlayerAge = 9999;

            float wanderChance = (m.kind == EntityKind::Bat) ? 0.65f : 0.25f;
            if (rng.chance(wanderChance)) {
                int di = rng.range(0, 7);
                tryMove(m, dirs[di][0], dirs[di][1]);
            }
            return;
        }

        // Path mode selection:
        //  - Ethereal monsters (e.g. bones ghosts) can phase through terrain.
        //  - Heavy bruisers can bash locked doors while hunting.
        int pathMode = PathMode::Normal;
        if (entityCanPhase(m.kind)) {
            pathMode = PathMode::Phasing;
        } else if (m.effects.levitationTurns > 0) {
            pathMode = PathMode::Levitate;
        } else if (monsterCanBashLockedDoor(m.kind)) {
            pathMode = PathMode::SmashLockedDoors;
        }

        // Pack / group coordination: if a grouped monster just spotted you,
        // it alerts nearby groupmates so packs behave like packs.
        if (seesPlayer && !wasAlerted && m.groupId != 0) {
            for (auto& o : ents) {
                if (o.id == playerId_) continue;
                if (o.hp <= 0) continue;
                if (o.groupId != m.groupId) continue;

                o.alerted = true;
                o.lastKnownPlayerPos = p.pos;
                o.lastKnownPlayerAge = 0;
            }

            // Flavor: wolves occasionally howl when they first spot the player.
            if (m.kind == EntityKind::Wolf) {
                const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                if (vis) {
                    pushMsg("THE WOLF HOWLS FOR HELP!", MessageKind::Warning, false);
                }
            }
        }

        const std::vector<int>& costMap = getCostMap(target, pathMode);
        const int d0 = costMap[static_cast<size_t>(idx(m.pos.x, m.pos.y))];

        // Pocket consumables: a few intelligent monsters can carry a potion and
        // will sometimes drink it mid-fight. This makes encounters with Wizards
        // more dynamic and also ensures levitation is meaningful for monsters.
        if (!m.friendly && m.pocketConsumable.id != 0 && m.pocketConsumable.count > 0 && isPotionKind(m.pocketConsumable.kind)) {
            auto drinkPocketPotion = [&](ItemKind pk) {
                const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                if (vis) {
                    Item tmp = m.pocketConsumable;
                    tmp.count = 1;
                    pushMsg(std::string("THE ") + kindName(m.kind) + " DRINKS A " + displayItemName(tmp) + "!", MessageKind::Warning, false);
                }

                // Apply a subset of potion effects that make sense for monsters.
                switch (pk) {
                    case ItemKind::PotionHealing: {
                        const int heal = itemDef(ItemKind::PotionHealing).healAmount;
                        m.hp = std::min(m.hpMax, m.hp + std::max(1, heal));
                        break;
                    }
                    case ItemKind::PotionRegeneration:
                        m.effects.regenTurns = std::max(m.effects.regenTurns, 18);
                        break;
                    case ItemKind::PotionShielding:
                        m.effects.shieldTurns = std::max(m.effects.shieldTurns, 14);
                        break;
                    case ItemKind::PotionInvisibility:
                        m.effects.invisTurns = std::min(60, m.effects.invisTurns + 18);
                        break;
                    case ItemKind::PotionLevitation: {
                        const int dur = 14 + rng.range(0, 6);
                        m.effects.levitationTurns = std::max(m.effects.levitationTurns, dur);
                        break;
                    }
                    default:
                        break;
                }

                // If the effect is obvious and the player saw it happen, auto-identify.
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    const bool obvious = (pk == ItemKind::PotionInvisibility || pk == ItemKind::PotionShielding || pk == ItemKind::PotionRegeneration || pk == ItemKind::PotionLevitation);
                    if (obvious) {
                        markIdentified(pk, false);
                    }
                }

                // Consume one potion from the stack.
                m.pocketConsumable.count -= 1;
                if (m.pocketConsumable.count <= 0) {
                    m.pocketConsumable.id = 0;
                }
            };

            const ItemKind pk = m.pocketConsumable.kind;

            // Emergency heal.
            if (pk == ItemKind::PotionHealing && m.hp <= std::max(1, m.hpMax / 3)) {
                drinkPocketPotion(pk);
                return;
            }

            // Combat buffs once engaged.
            if (seesPlayer && pk == ItemKind::PotionShielding && m.effects.shieldTurns <= 0 && rng.chance(0.65f)) {
                drinkPocketPotion(pk);
                return;
            }
            if (seesPlayer && pk == ItemKind::PotionInvisibility && m.effects.invisTurns <= 0 && rng.chance(0.75f)) {
                drinkPocketPotion(pk);
                return;
            }
            if (pk == ItemKind::PotionRegeneration && m.hp < m.hpMax && m.effects.regenTurns <= 0 && (m.hp <= m.hpMax / 2 || seesPlayer)) {
                drinkPocketPotion(pk);
                return;
            }

            // Tactical levitation: if the player is unreachable with normal pathing
            // (typically due to a chasm split), drink levitation to open a route.
            if (pk == ItemKind::PotionLevitation && m.effects.levitationTurns <= 0 && d0 < 0) {
                const std::vector<int>& costLev = getCostMap(target, PathMode::Levitate);
                const int dLev = costLev[static_cast<size_t>(idx(m.pos.x, m.pos.y))];
                if (dLev >= 0) {
                    drinkPocketPotion(pk);
                    return;
                }
            }
        }

        // If adjacent, melee attack (with some monster-specific tricks).
        if (isAdjacent8(m.pos, p.pos)) {
            Entity& pm = playerMut();

            // Fear: try to break contact instead of trading blows.
            // If no escape route exists, the monster will fall back to attacking.
            if (m.effects.fearTurns > 0 && d0 >= 0) {
                Vec2i to = bestStepAway(m, costMap, pathMode);
                if (to != m.pos) {
                    tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                    return;
                }
            }

            // Floor wards (NetHack-style): if the player is standing on a warding
            // engraving (e.g. "ELBERETH"), some monsters may hesitate and try to
            // break contact instead of attacking.
            if (seesPlayer) {
                // Find a ward on the player's tile (sparse list; linear scan is fine).
                size_t wardIdx = static_cast<size_t>(-1);
                for (size_t ii = 0; ii < engravings_.size(); ++ii) {
                    const Engraving& eg = engravings_[ii];
                    if (eg.isWard && eg.strength > 0 && eg.pos.x == pm.pos.x && eg.pos.y == pm.pos.y) {
                        wardIdx = ii;
                        break;
                    }
                }

                if (wardIdx != static_cast<size_t>(-1)) {
                    auto wardImmune = [&](EntityKind k) {
                        switch (k) {
                            // Undead and "boss" monsters ignore wards.
                            case EntityKind::SkeletonArcher:
                            case EntityKind::Ghost:
                            case EntityKind::Zombie:
                            case EntityKind::Wizard:
                            case EntityKind::Minotaur:
                            case EntityKind::Shopkeeper:
                                return true;
                            default:
                                return false;
                        }
                    };

                    if (!wardImmune(m.kind)) {
                        Engraving& eg = engravings_[wardIdx];
                        const float repelChance = std::clamp(0.35f + 0.10f * static_cast<float>(eg.strength), 0.35f, 0.85f);
                        const bool repelled = rng.chance(repelChance);

                        // Wards degrade with contact (finite uses). Permanent graffiti wards
                        // (strength=255) are treated as non-degrading.
                        if (eg.strength != 255) {
                            if (eg.strength > 0) --eg.strength;
                            if (eg.strength == 0) {
                                // Erase and optionally message.
                                const bool visWard = dung.inBounds(pm.pos.x, pm.pos.y) && dung.at(pm.pos.x, pm.pos.y).visible;
                                engravings_.erase(engravings_.begin() + static_cast<std::ptrdiff_t>(wardIdx));
                                if (visWard) {
                                    pushMsg("THE WARDING WORDS FADE!", MessageKind::Info, false);
                                }
                            }
                        }

                        if (repelled) {
                            const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                            if (vis) {
                                std::ostringstream ss;
                                ss << "THE " << kindName(m.kind) << " SHRINKS FROM THE WARD!";
                                pushMsg(ss.str(), MessageKind::Info, false);
                            }

                            if (d0 >= 0) {
                                Vec2i to = bestStepAway(m, costMap, pathMode);
                                if (to != m.pos) {
                                    tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                                    return;
                                }
                            }

                            // No escape route: the monster loses its action.
                            return;
                        }
                    }
                }
            }

            // Leprechaun: try to steal gold and teleport away instead of trading blows.
            if (m.kind == EntityKind::Leprechaun && seesPlayer) {
                const int playerGold = countGold(inv);
                if (playerGold > 0) {
                    // Steal a chunk (bounded so early-game isn't instantly ruined).
                    int want = rng.range(6, 16) + std::max(0, depth_ - 1) * 2;
                    if (want > playerGold) want = playerGold;

                    int need = want;
                    for (size_t ii = 0; ii < inv.size() && need > 0; ) {
                        Item& it = inv[ii];
                        if (it.kind == ItemKind::Gold) {
                            const int take = std::min(it.count, need);
                            it.count -= take;
                            need -= take;
                            if (it.count <= 0) {
                                inv.erase(inv.begin() + static_cast<std::ptrdiff_t>(ii));
                                continue;
                            }
                        }
                        ++ii;
                    }
                    const int took = want - need;

                    if (took > 0) {
                        m.stolenGold += took;
                        emitNoise(m.pos, 10);

                        {
                            std::ostringstream ss;
                            ss << "THE LEPRECHAUN STEALS " << took << " GOLD!";
                            pushMsg(ss.str(), MessageKind::Warning, true);
                        }

                        // Teleport away to a random safe floor tile.
                        Vec2i dst = m.pos;
                        for (int tries = 0; tries < 400; ++tries) {
                            Vec2i cand = dung.randomFloor(rng, true);
                            if (entityAt(cand.x, cand.y)) continue;
                            if (cand == dung.stairsUp || cand == dung.stairsDown) continue;
                            if (manhattan(cand, p.pos) < 8) continue;
                            dst = cand;
                            break;
                        }

                        if (dst != m.pos) {
                            const bool wasVisible = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                            m.pos = dst;
                            if (wasVisible) pushMsg("IT VANISHES!", MessageKind::Info, false);
                            return;
                        }

                        // Fallback: step away if teleport couldn't find a good spot.
                        if (d0 >= 0) {
                            Vec2i to = bestStepAway(m, costMap, pathMode);
                            if (to != m.pos) {
                                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                                return;
                            }
                        }
                    }
                }
            }

            attackMelee(m, pm);
            return;
        }
        // Monsters will also fight your companions if they block them.
        for (Entity* a : allies) {
            if (!a || a->hp <= 0) continue;
            if (a->id == m.id) continue;
            if (isAdjacent8(m.pos, a->pos)) {
                attackMelee(m, *a);
                return;
            }
        }

        // Ammo-based ranged monsters can run out. If they're standing on free ammo, reload it.
        if (m.canRanged && m.rangedAmmo != AmmoKind::None && m.rangedAmmoCount <= 0) {
            const ItemKind ammoK = (m.rangedAmmo == AmmoKind::Arrow) ? ItemKind::Arrow : ItemKind::Rock;
            const int ammoMax = (m.kind == EntityKind::KoboldSlinger) ? 18 : 12;

            for (size_t gi = 0; gi < ground.size(); ++gi) {
                GroundItem& g = ground[gi];
                if (g.pos.x != m.pos.x || g.pos.y != m.pos.y) continue;
                if (g.item.kind != ammoK) continue;
                if (g.item.shopPrice > 0) continue; // don't steal shop stock
                if (g.item.count <= 0) continue;

                const int take = std::min(g.item.count, ammoMax);
                m.rangedAmmoCount += take;
                g.item.count -= take;

                const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                if (vis) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " PICKS UP " << (ammoK == ItemKind::Arrow ? "ARROWS." : "ROCKS.");
                    pushMsg(ss.str(), MessageKind::Info, false);
                }

                if (g.item.count <= 0) {
                    ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(gi));
                }
                return;
            }
        }

        // Humanoid-ish monsters: if they're standing on better gear, equip it (costs their action).
        // This creates emergent difficulty (monsters can arm themselves) and makes loot more coherent.
        if ((monsterCanEquipWeapons(m.kind) || monsterCanEquipArmor(m.kind))) {
            auto bucBonus = [](int buc) -> int { return (buc < 0) ? -1 : (buc > 0 ? 1 : 0); };

            auto diceAvgTimes2 = [](DiceExpr d) -> int {
                // 2 * average(d) = count*(sides+1) + 2*bonus
                return d.count * (d.sides + 1) + 2 * d.bonus;
            };

            auto weaponScore = [&](const Item& it) -> int {
                if (!isMeleeWeapon(it.kind)) return std::numeric_limits<int>::min() / 2;
                const int avg2 = diceAvgTimes2(meleeDiceForWeapon(it.kind));
                // Weight dice heavily, then small nudges for accuracy/enchants/B.U.C.
                int score = avg2 * 10;
                score += itemDef(it.kind).meleeAtk * 8;
                score += it.enchant * 12;
                score += bucBonus(it.buc) * 10;
                return score;
            };

            auto naturalWeaponScore = [&]() -> int {
                const int avg2 = diceAvgTimes2(meleeDiceForMonster(m.kind));
                // Natural attacks usually don't get "meleeAtk" bonuses; keep this slightly lower so
                // equal-dice weapons are still attractive upgrades.
                return avg2 * 10;
            };

            auto armorScore = [&](const Item& it) -> int {
                if (!isArmor(it.kind)) return std::numeric_limits<int>::min() / 2;
                int score = itemDef(it.kind).defense * 15;
                score += it.enchant * 12;
                score += bucBonus(it.buc) * 10;
                return score;
            };

            const bool weaponLocked = (m.gearMelee.id != 0 && m.gearMelee.buc < 0);
            const bool armorLocked = (m.gearArmor.id != 0 && m.gearArmor.buc < 0);

            const int curWeapon = (m.gearMelee.id != 0 && isMeleeWeapon(m.gearMelee.kind)) ? weaponScore(m.gearMelee) : naturalWeaponScore();
            const int curArmor = (m.gearArmor.id != 0 && isArmor(m.gearArmor.kind)) ? armorScore(m.gearArmor) : 0;

            int bestDelta = 0;
            size_t bestGi = 0;
            enum class Slot { None, Weapon, Armor };
            Slot bestSlot = Slot::None;

            for (size_t gi = 0; gi < ground.size(); ++gi) {
                const GroundItem& g = ground[gi];
                if (g.pos.x != m.pos.x || g.pos.y != m.pos.y) continue;

                const Item& it = g.item;
                if (it.shopPrice > 0) continue; // don't steal shop stock
                if (it.count <= 0) continue;

                if (monsterCanEquipWeapons(m.kind) && !weaponLocked && isMeleeWeapon(it.kind)) {
                    const int sc = weaponScore(it);
                    const int delta = sc - curWeapon;
                    if (delta > bestDelta) {
                        bestDelta = delta;
                        bestGi = gi;
                        bestSlot = Slot::Weapon;
                    }
                }

                if (monsterCanEquipArmor(m.kind) && !armorLocked && isArmor(it.kind)) {
                    const int sc = armorScore(it);
                    const int delta = sc - curArmor;
                    if (delta > bestDelta) {
                        bestDelta = delta;
                        bestGi = gi;
                        bestSlot = Slot::Armor;
                    }
                }
            }

            if (bestSlot != Slot::None && bestDelta > 0 && bestGi < ground.size()) {
                Item picked = ground[bestGi].item;

                // Remove from ground.
                ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(bestGi));

                const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;

                if (bestSlot == Slot::Weapon) {
                    if (m.gearMelee.id != 0 && m.gearMelee.buc >= 0) {
                        dropGroundItemItem(m.pos, m.gearMelee);
                    }
                    m.gearMelee = picked;

                    if (vis) {
                        std::ostringstream ss;
                        ss << kindName(m.kind) << " PICKS UP " << itemDisplayNameSingle(picked.kind) << ".";
                        pushMsg(ss.str(), MessageKind::Info, false);
                    }
                    return;
                }

                if (bestSlot == Slot::Armor) {
                    if (m.gearArmor.id != 0 && m.gearArmor.buc >= 0) {
                        dropGroundItemItem(m.pos, m.gearArmor);
                    }
                    m.gearArmor = picked;

                    if (vis) {
                        std::ostringstream ss;
                        ss << kindName(m.kind) << " PUTS ON " << itemDisplayNameSingle(picked.kind) << ".";
                        pushMsg(ss.str(), MessageKind::Info, false);
                    }
                    return;
                }
            }
        }

// Wizard: occasionally "blinks" (teleports) to reposition, especially when wounded.
        if (m.kind == EntityKind::Wizard && seesPlayer) {
            const bool lowHp = (m.hp <= std::max(2, m.hpMax / 3));
            const bool close = (man <= 3);
            if (lowHp || (close && rng.chance(0.25f)) || rng.chance(0.08f)) {
                Vec2i dst = m.pos;
                for (int tries = 0; tries < 300; ++tries) {
                    Vec2i cand = dung.randomFloor(rng, true);
                    if (entityAt(cand.x, cand.y)) continue;
                    if (cand == dung.stairsUp || cand == dung.stairsDown) continue;
                    if (manhattan(cand, p.pos) < 6) continue;
                    dst = cand;
                    break;
                }
                if (dst != m.pos) {
                    const bool wasVisible = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                    m.pos = dst;
                    if (wasVisible) pushMsg("THE WIZARD BLINKS AWAY!", MessageKind::Warning, false);
                    return;
                }
            }
        }


        // Leprechaun: blinks away aggressively once it has stolen gold.
        if (m.kind == EntityKind::Leprechaun && seesPlayer) {
            const bool hasLoot = (m.stolenGold > 0);
            const bool close = (man <= 4);
            if ((hasLoot && (close || rng.chance(0.35f))) || rng.chance(0.04f)) {
                Vec2i dst = m.pos;
                for (int tries = 0; tries < 250; ++tries) {
                    Vec2i cand = dung.randomFloor(rng, true);
                    if (entityAt(cand.x, cand.y)) continue;
                    if (cand == dung.stairsUp || cand == dung.stairsDown) continue;
                    if (manhattan(cand, p.pos) < 7) continue;
                    dst = cand;
                    break;
                }

                if (dst != m.pos) {
                    const bool wasVisible = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                    m.pos = dst;
                    if (wasVisible) pushMsg("THE LEPRECHAUN VANISHES!", MessageKind::Warning, false);
                    return;
                }
            }
        }

        // If the monster reached the last-known spot but can't see the player, it will "search"
        // around for a little while and then eventually give up.
        if (!seesPlayer && m.pos == target) {
            float searchChance = (m.kind == EntityKind::Bat) ? 0.75f : 0.55f;
            if (rng.chance(searchChance)) {
                int di = rng.range(0, 7);
                tryMove(m, dirs[di][0], dirs[di][1]);
            }

            // Searching without finding the player makes the monster forget faster.
            m.lastKnownPlayerAge = std::min(9999, m.lastKnownPlayerAge + 1);
            return;
        }

        // Minotaur: brutal straight-line charge to close distance quickly.
        // This is intentionally simple (cardinal-only) but creates memorable boss turns.
        if (m.kind == EntityKind::Minotaur && m.effects.fearTurns <= 0 && seesPlayer && man >= 3) {
            int cdx = 0;
            int cdy = 0;
            if (m.pos.x == p.pos.x) {
                cdy = sign(p.pos.y - m.pos.y);
            } else if (m.pos.y == p.pos.y) {
                cdx = sign(p.pos.x - m.pos.x);
            }

            if ((cdx != 0 || cdy != 0) && rng.chance(0.28f)) {
                const int dist = (cdx != 0) ? std::abs(p.pos.x - m.pos.x) : std::abs(p.pos.y - m.pos.y);
                const int maxCharge = 6;
                int steps = std::min(maxCharge, std::max(0, dist - 1));

                if (steps >= 2) {
                    const bool wasVisible = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                    if (wasVisible) {
                        pushMsg("THE MINOTAUR CHARGES!", MessageKind::Warning, false);
                    }

                    emitNoise(m.pos, 16);

                    Vec2i cur = m.pos;
                    for (int i = 0; i < steps; ++i) {
                        Vec2i nxt{cur.x + cdx, cur.y + cdy};
                        if (nxt == p.pos) break;
                        if (!dung.inBounds(nxt.x, nxt.y)) break;

                        // Don't trample other entities during the charge (simple + avoids weirdness).
                        if (entityAt(nxt.x, nxt.y)) break;

                        TileType t = dung.at(nxt.x, nxt.y).type;
                        if (t == TileType::DoorClosed || t == TileType::DoorLocked) {
                            // Smash doors open as part of the charge.
                            dung.at(nxt.x, nxt.y).type = TileType::DoorOpen;
                            emitNoise(nxt, 14);
                            if (wasVisible) {
                                pushMsg("A DOOR BURSTS OPEN!", MessageKind::System, false);
                            }
                            t = TileType::DoorOpen;
                        }

                        // Stop if we hit solid terrain.
                        if (!dung.isWalkable(nxt.x, nxt.y)) break;

                        // Move.
                        m.pos = nxt;
                        cur = nxt;

                        // Charging can still trigger traps.
                        triggerTrapAt(m.pos, m);
                        if (m.hp <= 0) break;
                    }

                    if (m.hp > 0 && isAdjacent8(m.pos, p.pos)) {
                        Entity& pm = playerMut();
                        attackMelee(m, pm);
                    }
                    return;
                }
            }
        }

        // Fleeing behavior (away from whatever the monster is currently "hunting").
        const bool fleeLoot = (m.kind == EntityKind::Leprechaun && m.stolenGold > 0 && seesPlayer);
        const bool feared = (m.effects.fearTurns > 0);
        const bool lowHpFlee = (m.willFlee && m.hp <= std::max(1, m.hpMax / 3));
        if ((feared || fleeLoot || lowHpFlee) && d0 >= 0) {
            Vec2i to = bestStepAway(m, costMap, pathMode);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                return;
            }
        }

        // Ranged behavior (only when the monster can actually see the player).
        if (m.canRanged && seesPlayer && man <= m.rangedRange) {
            // Ammo-based ranged monsters can run out.
            if (m.rangedAmmo != AmmoKind::None && m.rangedAmmoCount <= 0) {
                // Out of ammo: close in instead of trying to kite.
            } else {
                // If too close, step back a bit.
                if (man <= 2 && d0 >= 0) {
                    Vec2i to = bestStepAway(m, costMap, pathMode);
                    if (to != m.pos) {
                        tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                        return;
                    }
                }

                // Wizards sometimes cast a curse instead of throwing a projectile.
                if (m.kind == EntityKind::Wizard && rng.chance(0.25f)) {
                    std::vector<int> candIdx;
                    int i = equippedMeleeIndex();
                    if (i >= 0 && inv[static_cast<size_t>(i)].buc >= 0) candIdx.push_back(i);
                    i = equippedArmorIndex();
                    if (i >= 0 && inv[static_cast<size_t>(i)].buc >= 0) candIdx.push_back(i);
                    i = equippedRangedIndex();
                    if (i >= 0 && inv[static_cast<size_t>(i)].buc >= 0) candIdx.push_back(i);
                    i = equippedRing1Index();
                    if (i >= 0 && inv[static_cast<size_t>(i)].buc >= 0) candIdx.push_back(i);
                    i = equippedRing2Index();
                    if (i >= 0 && inv[static_cast<size_t>(i)].buc >= 0) candIdx.push_back(i);

                    if (!candIdx.empty()) {
                        // Saving throw: defense + shielding helps resist.
                        int save = rng.range(1, 20) + playerDefense();
                        if (p.effects.shieldTurns > 0) save += 4;
                        int dc = 13 + std::max(0, depth_ - 1) / 2;

                        emitNoise(m.pos, 8);

                        if (save >= dc) {
                            pushMsg("YOU RESIST A MALEVOLENT CURSE.", MessageKind::System, false);
                            return;
                        }

                        Item& tgt = inv[static_cast<size_t>(candIdx[static_cast<size_t>(rng.range(0, static_cast<int>(candIdx.size()) - 1))])];
                        if (tgt.buc > 0) {
                            tgt.buc = 0;
                            pushMsg("A DARK AURA SNUFFS OUT A BLESSING.", MessageKind::System, false);
                        } else {
                            tgt.buc = -1;
                            pushMsg("YOUR EQUIPMENT FEELS... CURSED.", MessageKind::Warning, true);
                        }
                        return;
                    }
                }

                if (m.rangedAmmo != AmmoKind::None) {
                    m.rangedAmmoCount = std::max(0, m.rangedAmmoCount - 1);
                }

                attackRanged(m, p.pos, m.rangedRange, m.rangedAtk, 0, m.rangedProjectile, false);
                return;
            }
        }

        // Pack behavior: try to occupy adjacent tiles around player (only when seeing the player).
        if (m.packAI && seesPlayer) {
            Vec2i bestAdj = m.pos;
            int bestCost = std::numeric_limits<int>::max();
            bool found = false;

            for (auto& dv : dirs) {
                const int ax = p.pos.x + dv[0];
                const int ay = p.pos.y + dv[1];
                if (!dung.inBounds(ax, ay)) continue;
                if (!passableForMode(ax, ay, pathMode)) continue;
                if (entityAt(ax, ay)) continue;

                const int k = idx(ax, ay);
                if (reservedAdj.find(k) != reservedAdj.end()) continue;

                const std::vector<int>& cm = getCostMap({ax, ay}, pathMode);
                const int c = cm[static_cast<size_t>(idx(m.pos.x, m.pos.y))];
                if (c < 0) continue;

                if (!found || c < bestCost) {
                    found = true;
                    bestCost = c;
                    bestAdj = {ax, ay};
                }
            }

            if (found) {
                reservedAdj.insert(idx(bestAdj.x, bestAdj.y));
                const std::vector<int>& cm = getCostMap(bestAdj, pathMode);
                Vec2i to = bestStepToward(m, cm, pathMode);
                if (to != m.pos) {
                    tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                    return;
                }
            }
            // Fallback: chase directly below.
        }

        // Smell tracking: if a fresh scent gradient exists, follow it before falling back to
        // the generic chase step (this helps around corners and after invis/darkness break LOS).
        if (!seesPlayer && smellR > 0) {
            const Vec2i to = bestScentStep();
            if (to != m.pos) {
                if (tryMove(m, to.x - m.pos.x, to.y - m.pos.y)) return;
            }
        }

        // Default: step toward the hunt target using a cost-to-target map.
        if (d0 >= 0) {
            Vec2i to = bestStepToward(m, costMap, pathMode);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
            }
        } else {
            // No path found: wander a bit so the monster doesn't freeze.
            float wanderChance = (m.kind == EntityKind::Bat) ? 0.65f : 0.25f;
            if (rng.chance(wanderChance)) {
                int di = rng.range(0, 7);
                tryMove(m, dirs[di][0], dirs[di][1]);
            }
        }
    };

    for (auto& m : ents) {
        if (isFinished()) return;
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;

        // Ensure speed is initialized (covers older in-memory entities and keeps future changes robust).
        if (m.speed <= 0) m.speed = baseSpeedFor(m.kind);

        // Peaceful shopkeepers should not "bank" energy while idle.
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) {
            m.energy = 0;
            continue;
        }

        // Accumulate energy for this turn.
        const int gain = clampi(m.speed, 10, 200);
        m.energy += gain;

        // Cap stored energy so monsters don't unleash huge bursts if they were stalled.
        const int maxEnergy = ENERGY_PER_ACTION * MAX_ACTIONS_PER_TURN;
        if (m.energy > maxEnergy) m.energy = maxEnergy;

        bool agedThisTurn = false;
        int actions = 0;
        while (!isFinished() && m.hp > 0 && m.energy >= ENERGY_PER_ACTION && actions < MAX_ACTIONS_PER_TURN) {
            m.energy -= ENERGY_PER_ACTION;
            ++actions;
            actOnce(m, agedThisTurn);
        }
    }

    // Post-turn passive effects (regen, etc.).
    for (auto& m : ents) {
        if (isFinished()) return;
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;
        if (m.regenAmount <= 0 || m.regenChancePct <= 0) continue;
        if (m.hp >= m.hpMax) continue;
        if (rng.range(1, 100) <= m.regenChancePct) {
            m.hp = std::min(m.hpMax, m.hp + m.regenAmount);

            // Only message if the monster is currently visible to the player.
            if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                std::ostringstream ss;
                ss << kindName(m.kind) << " REGENERATES.";
                pushMsg(ss.str());
            }
        }
    }
}
