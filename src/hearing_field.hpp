#pragma once

// Shared hearing/audibility field builder.
//
// This is used to integrate the game's sound propagation model with
// decision-making systems that want to reason about "will a noise here be heard?"
//
// Current primary use:
//   * Auto-travel / auto-explore path planning while Sneak is enabled.
//     (Prefer routes whose footstep noise is less likely to alert visible hostiles.)

#include "game.hpp"
#include "pathfinding.hpp"

#include <algorithm>
#include <vector>

struct HearingFieldResult {
    // Positions of included listeners (currently: visible hostile monsters).
    std::vector<Vec2i> listeners;

    // Per-tile minimum *noise volume* required for a sound at that tile to be heard by
    // at least one listener (or -1 if no listener can hear the tile within maxCost).
    //
    // Volumes are in the same "tile-cost" units as Dungeon::computeSoundMap.
    std::vector<int> minRequiredVolume;

    // For each tile, the index into `listeners` that achieved minRequiredVolume.
    //
    // -1 means unreachable / no audible listener within maxCost.
    //
    // NOTE: This is a UI/analysis affordance. It must never be used to reason about
    // unseen monsters (buildVisibleHostileHearingField() only includes visible hostiles).
    std::vector<int> dominantListenerIndex;
};

inline HearingFieldResult buildVisibleHostileHearingField(const Game& g, int maxCost) {
    HearingFieldResult out;

    const Dungeon& dung = g.dungeon();
    if (dung.width <= 0 || dung.height <= 0) return out;

    // Ensure deterministic per-floor material cache so sound propagation costs
    // can incorporate substrate acoustics (moss/dirt dampen; metal/crystal carry).
    dung.ensureMaterials(g.materialWorldSeed(), g.branch(), g.materialDepth(), g.dungeonMaxDepth());

    // Collect visible hostile listeners.
    struct Listener {
        Vec2i pos;
        int hearingDelta = 0;
    };

    std::vector<Listener> ls;
    ls.reserve(16);

    for (const auto& e : g.entities()) {
        if (e.id == g.playerId()) continue;
        if (e.friendly) continue;
        if (e.kind == EntityKind::Shopkeeper && !e.alerted) continue;
        if (e.hp <= 0) continue;
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
        if (!dung.at(e.pos.x, e.pos.y).visible) continue;

        Listener l;
        l.pos = e.pos;
        l.hearingDelta = entityHearingDelta(e.kind);
        ls.push_back(l);
        out.listeners.push_back(e.pos);
    }

    if (ls.empty()) return out;

    const int W = std::max(1, dung.width);
    const int H = std::max(1, dung.height);
    const size_t n = static_cast<size_t>(W * H);

    out.minRequiredVolume.assign(n, -1);
    out.dominantListenerIndex.assign(n, -1);

    // We want, for each tile t:
    //   minRequiredVolume[t] = min_listener max(0, dist(t -> listener) - hearingDelta(listener))
    //
    // Where dist(...) uses the SAME sound propagation graph as Dungeon::computeSoundMap.
    //
    // Instead of running one Dijkstra per listener, we can compute this in ONE multi-source
    // reverse Dijkstra by seeding each listener with an initial cost bias:
    //   seedCost = offset - hearingDelta(listener)
    //
    // Then the resulting field is:
    //   best[t] = min_listener dist(t -> listener) + offset - hearingDelta(listener)
    // => best[t] - offset = min_listener (dist - hearingDelta)
    //
    // Finally clamp to >= 0, because volume cannot be negative.
    int maxHearingDelta = 0;
    for (const auto& l : ls) maxHearingDelta = std::max(maxHearingDelta, l.hearingDelta);
    const int offset = std::max(0, maxHearingDelta);

    std::vector<DijkstraSeed> seeds;
    seeds.reserve(ls.size());
    for (const auto& l : ls) {
        const int init = offset - l.hearingDelta;
        seeds.push_back(DijkstraSeed{l.pos, init < 0 ? 0 : init});
    }

    PassableFn passable = [&](int x, int y) -> bool { return dung.soundPassable(x, y); };
    StepCostFn stepCost = [&](int x, int y) -> int { return dung.soundTileCost(x, y); };
    DiagonalOkFn diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return dung.soundDiagonalOk(fromX, fromY, dx, dy);
    };

    const int seededMaxCost = (maxCost < 0) ? -1 : (maxCost + offset);
    const DijkstraNearestSeededResult best = dijkstraCostToNearestSeededWithProvenance(W, H, seeds, passable, stepCost, diagOk, seededMaxCost);
    if (best.cost.size() != n || best.nearestSeedIndex.size() != n) return out;

    for (size_t i = 0; i < n; ++i) {
        const int v = best.cost[i];
        if (v < 0) continue;
	    const int src = best.nearestSeedIndex[i];
	    if (src < 0) continue;

        int req = v - offset;
        if (req < 0) req = 0;
        if (maxCost >= 0 && req > maxCost) continue;
        out.minRequiredVolume[i] = req;
        out.dominantListenerIndex[i] = src;
    }

    return out;
}
