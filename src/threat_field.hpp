#pragma once

// Shared threat/ETA field builder.
//
// This is used by:
//   * LOOK-mode Threat Preview overlay (UI-only)
//   * Auto-travel safety checks + threat-aware path planning
//
// It computes a conservative "soonest arrival" cost-to-tile field for the
// nearest *currently visible hostile*, using the same monster pathing policy
// as the actual AI.

#include "monster_pathing.hpp"

#include <array>
#include <algorithm>
#include <vector>

struct ThreatFieldResult {
    std::vector<Vec2i> sources; // positions of included visible hostiles
    std::vector<int> dist;      // per-tile minimum cost-to-reach from nearest hostile (or -1 if > maxCost/unreachable)
};

inline ThreatFieldResult buildVisibleHostileThreatField(const Game& g, int maxCost) {
    ThreatFieldResult out;

    const Dungeon& dung = g.dungeon();
    if (dung.width <= 0 || dung.height <= 0) return out;

    // IMPORTANT: Group by *combined* capability masks so hybrid monsters are modeled
    // correctly (e.g. levitating door-smashers can traverse both chasms and locked doors).
    std::array<std::vector<Vec2i>, 8> srcByCaps;
    for (auto& v : srcByCaps) v.clear();

    for (const auto& e : g.entities()) {
        if (e.id == g.playerId()) continue;
        if (e.friendly) continue;
        if (e.kind == EntityKind::Shopkeeper && !e.alerted) continue;
        if (e.hp <= 0) continue;
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
        if (!dung.at(e.pos.x, e.pos.y).visible) continue;

        out.sources.push_back(e.pos);
        const int caps = monsterPathCapsForEntity(e) & 7;
        srcByCaps[static_cast<size_t>(caps)].push_back(e.pos);
    }

    if (out.sources.empty()) return out;

    const int W = std::max(1, dung.width);
    const int H = std::max(1, dung.height);

    out.dist.assign(static_cast<size_t>(W * H), -1);

    auto combineMin = [&](const std::vector<int>& m) {
        if (m.empty()) return;
        if (m.size() != out.dist.size()) return;
        for (size_t i = 0; i < m.size(); ++i) {
            const int v = m[i];
            if (v < 0) continue;
            int& dst = out.dist[i];
            if (dst < 0 || v < dst) dst = v;
        }
    };

    // Discovered traps are visible information; include them as a soft penalty so ETA
    // matches what monsters will actually prefer.
    std::vector<int> trapPenalty = buildDiscoveredTrapPenaltyGrid(g);
    if (trapPenalty.size() != static_cast<size_t>(W * H)) {
        trapPenalty.assign(static_cast<size_t>(W * H), 0);
    }

    for (int caps = 0; caps < 8; ++caps) {
        const auto& srcs = srcByCaps[static_cast<size_t>(caps)];
        if (srcs.empty()) continue;

        PassableFn passable = monsterPassableFn(g, caps);
        StepCostFn stepCost = monsterStepCostFn(g, caps, &trapPenalty);
        DiagonalOkFn diagOk = monsterDiagonalOkFn(g, caps);

        const auto m = dijkstraCostFromSources(W, H, srcs, passable, stepCost, diagOk, maxCost);
        combineMin(m);
    }

    return out;
}
