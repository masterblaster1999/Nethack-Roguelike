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
