#pragma once

// Shared monster pathing rules used by both the monster AI and UI previews.
//
// Goal: keep "what monsters can traverse" + "how long it takes" consistent across
// systems, and correctly model *combined* capabilities (e.g. a bruiser that is
// currently levitating can also still bash locked doors).

#include "game.hpp"

#include "grid_utils.hpp"
#include "pathfinding.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

// Bitmask of movement/pathing capabilities.
//
// IMPORTANT: Keep this to <=3 bits so we can pack it into the AI cost-map cache key.
enum MonsterPathCaps : int {
    MPC_None      = 0,
    MPC_SmashLock = 1 << 0, // treat locked doors as passable (slowly)
    MPC_Levitate  = 1 << 1, // treat chasms as passable
    MPC_Phase     = 1 << 2, // ignore terrain (still cannot leave the map)
};

inline int monsterPathCapsForEntity(const Entity& e) {
    // Phasing dominates all other movement constraints.
    if (entityCanPhase(e.kind)) return MPC_Phase;

    int caps = MPC_None;
    if (entityCanBashLockedDoor(e.kind)) caps |= MPC_SmashLock;
    if (e.effects.levitationTurns > 0)   caps |= MPC_Levitate;
    return caps;
}

// A per-trap traversal penalty used by monster pathing (AI + UI ETA preview).
// Monsters can still step onto traps; they just prefer to route around *discovered* ones.
inline int trapPenaltyForMonsterPathing(TrapKind k) {
    switch (k) {
        case TrapKind::TrapDoor:       return 18;
        case TrapKind::RollingBoulder: return 17;
        case TrapKind::PoisonDart:     return 14;
        case TrapKind::Spike:          return 14;
        case TrapKind::PoisonGas:      return 13;
        case TrapKind::CorrosiveGas:   return 14;
        case TrapKind::ConfusionGas:   return 12;
        case TrapKind::Web:            return 10;
        case TrapKind::LetheMist:      return 9;
        case TrapKind::Alarm:          return 7;
        case TrapKind::Teleport:       return 6;
        default:                       return 12;
    }
}

inline std::vector<int> buildDiscoveredTrapPenaltyGrid(const Game& g) {
    const Dungeon& dung = g.dungeon();
    const int W = std::max(1, dung.width);
    const int H = std::max(1, dung.height);
    std::vector<int> out;
    out.assign(static_cast<size_t>(W * H), 0);

    auto idx = [&](int x, int y) { return y * W + x; };

    for (const auto& tr : g.traps()) {
        if (!tr.discovered) continue;
        if (!dung.inBounds(tr.pos.x, tr.pos.y)) continue;
        const size_t i = static_cast<size_t>(idx(tr.pos.x, tr.pos.y));
        if (i >= out.size()) continue;
        const int p = trapPenaltyForMonsterPathing(tr.kind);
        if (p > out[i]) out[i] = p;
    }

    return out;
}

inline bool monsterPassableForCaps(const Game& g, int x, int y, int caps) {
    const Dungeon& dung = g.dungeon();
    if (!dung.inBounds(x, y)) return false;

    // Phasing ignores terrain (but cannot leave the map).
    if (caps & MPC_Phase) return true;

    if (dung.isPassable(x, y)) return true;

    // Capability extensions.
    if ((caps & MPC_SmashLock) && dung.isDoorLocked(x, y)) return true;
    if ((caps & MPC_Levitate) && dung.at(x, y).type == TileType::Chasm) return true;

    return false;
}

inline int monsterStepCostForCaps(const Game& g, int x, int y, int caps, const std::vector<int>* discoveredTrapPenalty) {
    const Dungeon& dung = g.dungeon();
    if (!dung.inBounds(x, y)) return 0;

    int cost = 1;

    if (caps & MPC_Phase) {
        // Bias: prefer open corridors over "living" inside walls.
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
                cost = (caps & MPC_SmashLock) ? 4 : 0;
                break;
            default:
                cost = 1;
                break;
        }
    }

    if (cost <= 0) return cost;

    // Environmental hazards:
    // - Fire is an obvious hazard: monsters generally try to route around it.
    // - Confusion gas is undesirable.
    // - Poison gas is also undesirable.
    //
    // NOTE: Even phasing monsters still prefer to avoid hazards; this keeps the
    // ETA preview conservative and aligns with the AI.
    const uint8_t f = g.fireAt(x, y);
    if (f > 0u) {
        cost += 10 + static_cast<int>(f) / 16; // +10..+25
    }

    const uint8_t cg = g.confusionGasAt(x, y);
    if (cg > 0u) {
        cost += 6 + static_cast<int>(cg) / 32; // +6..+13
    }

    const uint8_t pg = g.poisonGasAt(x, y);
    if (pg > 0u) {
        cost += 7 + static_cast<int>(pg) / 32; // +7..+14
    }

    const uint8_t ag = g.corrosiveGasAt(x, y);
    if (ag > 0u) {
        cost += 8 + static_cast<int>(ag) / 32; // +8..+15
    }

    if (discoveredTrapPenalty && !discoveredTrapPenalty->empty()) {
        const int W = std::max(1, dung.width);
        const size_t ti = static_cast<size_t>(y * W + x);
        if (ti < discoveredTrapPenalty->size()) {
            const int tp = (*discoveredTrapPenalty)[ti];
            if (tp > 0) cost += tp;
        }
    }

    return cost;
}

inline bool monsterDiagonalOkForCaps(const Game& g, int fromX, int fromY, int dx, int dy, int caps) {
    // Cardinal moves never need special casing.
    if (dx == 0 || dy == 0) return true;

    if (caps & MPC_Phase) return true;

    // If levitating, ensure both adjacent cardinals are passable in this capability set.
    // This prevents "corner cutting" through a blocked corner while still allowing
    // diagonal movement across chasm edges.
    if (caps & MPC_Levitate) {
        return monsterPassableForCaps(g, fromX + dx, fromY, caps) &&
               monsterPassableForCaps(g, fromX, fromY + dy, caps);
    }

    const Dungeon& dung = g.dungeon();
    return diagonalPassable(dung, {fromX, fromY}, dx, dy);
}

inline PassableFn monsterPassableFn(const Game& g, int caps) {
    return [&g, caps](int x, int y) -> bool {
        return monsterPassableForCaps(g, x, y, caps);
    };
}

inline StepCostFn monsterStepCostFn(const Game& g, int caps, const std::vector<int>* discoveredTrapPenalty) {
    return [&g, caps, discoveredTrapPenalty](int x, int y) -> int {
        return monsterStepCostForCaps(g, x, y, caps, discoveredTrapPenalty);
    };
}

inline DiagonalOkFn monsterDiagonalOkFn(const Game& g, int caps) {
    return [&g, caps](int fromX, int fromY, int dx, int dy) -> bool {
        return monsterDiagonalOkForCaps(g, fromX, fromY, dx, dy, caps);
    };
}
