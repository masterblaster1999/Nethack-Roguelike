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

} // namespace

void Game::monsterTurn() {
    if (isFinished()) return;

    const Entity& p = player();
    Entity* dog = nullptr;
    for (auto& e : ents) {
        if (e.kind == EntityKind::Dog && e.hp > 0) { dog = &e; break; }
    }
    const int W = dung.width;
    const int H = dung.height;

    auto idx = [&](int x, int y) { return y * W + x; };

    const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

    // Some monsters can bash through locked doors while hunting.
    // We model this in pathfinding by treating locked doors as passable
    // with a steep movement cost (representing repeated smash attempts).
    enum PathMode : int {
        Normal = 0,
        SmashLockedDoors = 1,
    };

    auto monsterCanBashLockedDoor = [&](EntityKind k) -> bool {
        // Keep conservative for balance: only heavy bruisers.
        return (k == EntityKind::Ogre || k == EntityKind::Troll || k == EntityKind::Minotaur);
    };

    auto passableForMode = [&](int x, int y, int mode) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (dung.isPassable(x, y)) return true;
        if (mode == PathMode::SmashLockedDoors && dung.isDoorLocked(x, y)) return true;
        return false;
    };

    auto stepCostForMode = [&](int x, int y, int mode) -> int {
        if (!dung.inBounds(x, y)) return 0;
        const TileType t = dung.at(x, y).type;
        switch (t) {
            case TileType::DoorClosed:
                // Monsters open doors as an action, then step through next.
                return 2;
            case TileType::DoorLocked:
                // Smashing locks is much slower than opening an unlocked door.
                return (mode == PathMode::SmashLockedDoors) ? 4 : 0;
            default:
                return 1;
        }
    };

    auto diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return diagonalPassable(dung, {fromX, fromY}, dx, dy);
    };

    // Cache cost-to-target maps for this turn.
    // Keyed by (target tile index, path mode) to keep caching correct.
    // These maps are "minimum turns" approximations (doors/locks cost extra).
    std::unordered_map<int, std::vector<int>> costCache;
    costCache.reserve(32);

    auto getCostMap = [&](Vec2i target, int mode) -> const std::vector<int>& {
        const int key = (idx(target.x, target.y) << 1) | (mode & 1);
        auto it = costCache.find(key);
        if (it != costCache.end()) return it->second;

        auto passable = [&, mode](int x, int y) -> bool { return passableForMode(x, y, mode); };
        auto stepCost = [&, mode](int x, int y) -> int { return stepCostForMode(x, y, mode); };

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
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, m.pos, dx, dy)) continue;
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
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, m.pos, dx, dy)) continue;
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

    // Energy scheduling constants.
    constexpr int ENERGY_PER_ACTION = 100;
    constexpr int MAX_ACTIONS_PER_TURN = 3; // safety cap: avoids runaway loops if speed is ever mis-set

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

        // Starting companion AI (Dog): follows the player and fights nearby hostiles.
        if (m.kind == EntityKind::Dog) {
            // Look for the nearest hostile in line-of-sight.
            Entity* best = nullptr;
            int bestMan = 9999;
            for (auto& e : ents) {
                if (e.id == playerId_) continue;
                if (e.hp <= 0) continue;
                if (e.kind == EntityKind::Dog) continue;
                if (e.kind == EntityKind::Shopkeeper && !e.alerted) continue;

                const int man0 = manhattan(m.pos, e.pos);
                if (man0 > LOS_MANHATTAN) continue;
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

            // No visible hostiles: stick close to the player.
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

        bool seesPlayer = false;
        if (man <= LOS_MANHATTAN) {
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
            if (m.lastKnownPlayerAge < 9999) m.lastKnownPlayerAge += 1;
            agedThisTurn = true;
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

        // Path mode: heavy monsters can bash locked doors while hunting.
        int pathMode = PathMode::Normal;
        if (monsterCanBashLockedDoor(m.kind)) {
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

        // If adjacent, melee attack.
        if (isAdjacent8(m.pos, p.pos)) {
            Entity& pm = playerMut();
            attackMelee(m, pm);
            return;
        }
        // Monsters will also fight your companion if it blocks them.
        if (dog && dog->hp > 0 && dog->id != m.id && isAdjacent8(m.pos, dog->pos)) {
            attackMelee(m, *dog);
            return;
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
        if (m.kind == EntityKind::Minotaur && seesPlayer && man >= 3) {
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
        if (m.willFlee && m.hp <= std::max(1, m.hpMax / 3) && d0 >= 0) {
            Vec2i to = bestStepAway(m, costMap, pathMode);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
            }
            return;
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
