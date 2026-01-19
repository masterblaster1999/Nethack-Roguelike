#pragma once

#include "dungeon.hpp"

#include <vector>

// Shared helpers for projectile line-of-fire logic.
//
// IMPORTANT: These helpers must stay consistent with the projectile traversal rules
// implemented in combat (Game::attackRanged) and targeting.

// Returns true if moving diagonally from `prev -> p` is blocked by a tight corner.
//
// This matches the rule used in projectile combat: if a diagonal step would cut
// between two orthogonally-adjacent projectile-blocking tiles, the shot cannot
// pass through the corner.
inline bool projectileCornerBlocked(const Dungeon& dung, const Vec2i& prev, const Vec2i& p) {
    const int dx = (p.x > prev.x) ? 1 : (p.x < prev.x) ? -1 : 0;
    const int dy = (p.y > prev.y) ? 1 : (p.y < prev.y) ? -1 : 0;
    if (dx == 0 || dy == 0) return false;

    const int ax = prev.x + dx;
    const int ay = prev.y;
    const int bx = prev.x;
    const int by = prev.y + dy;

    if (!dung.inBounds(ax, ay) || !dung.inBounds(bx, by)) return false;
    return dung.blocksProjectiles(ax, ay) && dung.blocksProjectiles(bx, by);
}

// Returns true if the projectile line is clear of terrain blockers from src->dst.
//
// - `line` must be a Bresenham line including both endpoints (src at line[0]).
// - If `range > 0` and the destination lies beyond range, this returns false.
// - This ignores entities: it answers "can a projectile reach dst (if it kept going)?".
inline bool hasClearProjectileLine(const Dungeon& dung,
                                  const std::vector<Vec2i>& line,
                                  const Vec2i& dst,
                                  int range) {
    if (line.size() <= 1) return false;

    if (range > 0 && static_cast<int>(line.size()) > range + 1) {
        // Out of range.
        return false;
    }

    for (size_t i = 1; i < line.size(); ++i) {
        const Vec2i p = line[i];
        if (!dung.inBounds(p.x, p.y)) return false;

        if (projectileCornerBlocked(dung, line[i - 1], p)) return false;

        // Terrain blocks the shot unless it's the intended destination.
        if (dung.blocksProjectiles(p.x, p.y) && p != dst) return false;
    }

    return true;
}
