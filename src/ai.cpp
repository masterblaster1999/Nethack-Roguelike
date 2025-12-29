#include "game.hpp"

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
    const int W = dung.width;
    const int H = dung.height;

    auto idx = [&](int x, int y) { return y * W + x; };

    const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

    // Monster movement costs: closed doors take an extra turn to open.
    auto passable = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        return dung.isPassable(x, y);
    };

    auto stepCost = [&](int x, int y) -> int {
        if (!dung.inBounds(x, y)) return 0;
        const TileType t = dung.at(x, y).type;
        switch (t) {
            case TileType::DoorClosed:
                // Monsters open doors as an action, then step through next.
                return 2;
            default:
                return 1;
        }
    };

    auto diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return diagonalPassable(dung, {fromX, fromY}, dx, dy);
    };

    // Cache cost-to-target maps for this turn (keyed by target tile index).
    // These maps are "minimum turns" approximations (doors cost extra).
    std::unordered_map<int, std::vector<int>> costCache;
    costCache.reserve(16);

    auto getCostMap = [&](Vec2i target) -> const std::vector<int>& {
        const int key = idx(target.x, target.y);
        auto it = costCache.find(key);
        if (it != costCache.end()) return it->second;
        auto [it2, inserted] = costCache.emplace(key, dijkstraCostToTarget(W, H, target, passable, stepCost, diagOk));
        (void)inserted;
        return it2->second;
    };

    auto bestStepToward = [&](const Entity& m, const std::vector<int>& costMap) -> Vec2i {
        Vec2i best = m.pos;
        int bestScore = std::numeric_limits<int>::max();
        for (auto& dv : dirs) {
            int dx = dv[0], dy = dv[1];
            const int nx = m.pos.x + dx;
            const int ny = m.pos.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, m.pos, dx, dy)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (entityAt(nx, ny)) continue;

            const int cToTarget = costMap[static_cast<size_t>(idx(nx, ny))];
            if (cToTarget < 0) continue;

            const int step = stepCost(nx, ny);
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

    auto bestStepAway = [&](const Entity& m, const std::vector<int>& costMap) -> Vec2i {
        Vec2i best = m.pos;
        int bestD = -1;
        for (auto& dv : dirs) {
            int dx = dv[0], dy = dv[1];
            const int nx = m.pos.x + dx;
            const int ny = m.pos.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, m.pos, dx, dy)) continue;
            if (!dung.isPassable(nx, ny)) continue;
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

        refreshPackAnchor();

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

        const std::vector<int>& costMap = getCostMap(target);
        const int d0 = costMap[static_cast<size_t>(idx(m.pos.x, m.pos.y))];

        // If adjacent, melee attack.
        if (isAdjacent8(m.pos, p.pos)) {
            Entity& pm = playerMut();
            attackMelee(m, pm);
            return;
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

        // Fleeing behavior (away from whatever the monster is currently "hunting").
        if (m.willFlee && m.hp <= std::max(1, m.hpMax / 3) && d0 >= 0) {
            Vec2i to = bestStepAway(m, costMap);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
            }
            return;
        }

        // Ranged behavior (only when the monster can actually see the player).
        if (m.canRanged && seesPlayer && man <= m.rangedRange) {
            // If too close, step back a bit.
            if (man <= 2 && d0 >= 0) {
                Vec2i to = bestStepAway(m, costMap);
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

            attackRanged(m, p.pos, m.rangedRange, m.rangedAtk, 0, m.rangedProjectile, false);
            return;
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
                if (!dung.isPassable(ax, ay)) continue;
                if (entityAt(ax, ay)) continue;

                const int k = idx(ax, ay);
                if (reservedAdj.find(k) != reservedAdj.end()) continue;

                const std::vector<int>& cm = getCostMap({ax, ay});
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
                const std::vector<int>& cm = getCostMap(bestAdj);
                Vec2i to = bestStepToward(m, cm);
                if (to != m.pos) {
                    tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                    return;
                }
            }
            // Fallback: chase directly below.
        }

        // Default: step toward the hunt target using a cost-to-target map.
        if (d0 >= 0) {
            Vec2i to = bestStepToward(m, costMap);
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
