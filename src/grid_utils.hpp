#pragma once

#include "common.hpp"
#include "dungeon.hpp"

// Small grid helpers shared across the codebase.

inline bool isAdjacent8(const Vec2i& a, const Vec2i& b) {
    const int dx = std::abs(a.x - b.x);
    const int dy = std::abs(a.y - b.y);
    return (dx <= 1 && dy <= 1 && (dx + dy) != 0);
}

inline bool diagonalPassable(const Dungeon& dung, const Vec2i& from, int dx, int dy) {
    // Prevent corner-cutting through two blocked orthogonal tiles.
    if (dx == 0 || dy == 0) return true;
    const int ox1 = from.x + dx;
    const int oy1 = from.y;
    const int ox2 = from.x;
    const int oy2 = from.y + dy;
    // Closed doors are treated as blocking here so you can't slip around them.
    const bool o1 = dung.isWalkable(ox1, oy1);
    const bool o2 = dung.isWalkable(ox2, oy2);
    return o1 || o2;
}
inline bool diagonalPassable(const Dungeon& dung, const Vec2i& from, int dx, int dy, bool canTraverseChasm) {
    // Prevent corner-cutting through two blocked orthogonal tiles, with optional support for
    // treating chasms as "clear" when an entity can traverse them (e.g. levitation).
    if (dx == 0 || dy == 0) return true;
    if (!canTraverseChasm) return diagonalPassable(dung, from, dx, dy);

    const int ox1 = from.x + dx;
    const int oy1 = from.y;
    const int ox2 = from.x;
    const int oy2 = from.y + dy;

    auto orthClear = [&](int x, int y) -> bool {
        if (dung.isWalkable(x, y)) return true;
        if (!dung.inBounds(x, y)) return false;
        return dung.at(x, y).type == TileType::Chasm;
    };

    const bool o1 = orthClear(ox1, oy1);
    const bool o2 = orthClear(ox2, oy2);
    return o1 || o2;
}
