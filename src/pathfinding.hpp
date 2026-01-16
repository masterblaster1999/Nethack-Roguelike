#pragma once

#include "common.hpp"

#include <functional>
#include <vector>

// Lightweight Dijkstra helpers for 8-way grid pathing.
//
// The callbacks are intentionally minimal so the logic can be reused by both
// player auto-move (which needs to consider doors/locks/traps) and monster AI.
//
// Conventions:
//   - stepCost(x,y) is the cost to ENTER tile (x,y). Return <=0 to treat the
//     tile as blocked.
//   - passable(x,y) should return true if the tile can be entered (ignoring
//     entities).
//   - diagonalOk(fromX,fromY,dx,dy) is called only for diagonal moves, where
//     (dx,dy) is one of (+/-1,+/-1). Return false to prevent corner-cutting.

using PassableFn  = std::function<bool(int x, int y)>;
using StepCostFn  = std::function<int(int x, int y)>;
using DiagonalOkFn = std::function<bool(int fromX, int fromY, int dx, int dy)>;

// Multi-source Dijkstra "seed".
//
// initialCost is added to the resulting cost field as if the seed were reached
// via an extra edge of weight initialCost.
//
// IMPORTANT:
//   initialCost must be >= 0.
//
// Why?
//   The pathfinding helpers use -1 to represent unreachable tiles. Allowing
//   negative seed costs would make -1 a valid reachable cost, which would be
//   ambiguous at the API boundary.
struct DijkstraSeed {
    Vec2i pos;
    int initialCost = 0;
};

// Returns a path including {start, ..., goal}. Empty on failure.
std::vector<Vec2i> dijkstraPath(
    int width,
    int height,
    Vec2i start,
    Vec2i goal,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk);

// Builds a "cost-to-target" map, where cost[i] is the minimum cost to reach
// `target` from tile i (excluding the cost of the starting tile itself).
//
// Unreachable tiles are -1.
//
// If maxCost >= 0, the search is truncated (tiles with best cost > maxCost
// remain -1).
std::vector<int> dijkstraCostToTarget(
    int width,
    int height,
    Vec2i target,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost = -1);

// Multi-source variant: returns a "cost-to-nearest-source" map, where cost[i]
// is the minimum cost to reach ANY of the tiles in `sources` from tile i
// (excluding the cost of the starting tile itself).
//
// Unreachable tiles are -1.
//
// If maxCost >= 0, the search is truncated (tiles with best cost > maxCost
// remain -1).
std::vector<int> dijkstraCostToNearestSource(
    int width,
    int height,
    const std::vector<Vec2i>& sources,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost = -1);

// Forward multi-source variant: returns a "cost-from-nearest-source" map, where
// cost[i] is the minimum cost to reach tile i from ANY of the tiles in
// `sources` (excluding the cost of the starting tile itself).
//
// Unreachable tiles are -1.
//
// If maxCost >= 0, the search is truncated (tiles with best cost > maxCost
// remain -1).
std::vector<int> dijkstraCostFromSources(
    int width,
    int height,
    const std::vector<Vec2i>& sources,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost = -1);

// Seeded multi-source variants.
//
// These are useful for influence fields where each source has a different
// baseline strength/bias, while still keeping all edge weights non-negative.
//
// dijkstraCostToNearestSeeded(): returns a "cost-to-nearest-seed" map
// (reverse expansion), matching dijkstraCostToNearestSource semantics.
std::vector<int> dijkstraCostToNearestSeeded(
    int width,
    int height,
    const std::vector<DijkstraSeed>& seeds,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost = -1);

// dijkstraCostFromSeeded(): returns a "cost-from-nearest-seed" map
// (forward expansion), matching dijkstraCostFromSources semantics.
std::vector<int> dijkstraCostFromSeeded(
    int width,
    int height,
    const std::vector<DijkstraSeed>& seeds,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost = -1);


