#pragma once

// Poisson-disc sampling (blue-noise point sets)
//
// This header provides a small, self-contained implementation of Bridson's
// Poisson-disc sampling algorithm for integer grid domains.
//
// We use this in procgen to place "feature seeds" (rooms, springs, outcrops, ...)
// with a minimum distance constraint so they don't clump.

#include "common.hpp"
#include "rng.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace procgen {

// Bridson Poisson-disc sampling over an axis-aligned integer rectangle.
//
// Domain is inclusive: [minX..maxX] x [minY..maxY].
//
// minDist is the minimum Euclidean distance between returned points.
// k is the number of random candidates tested per active point.
//
// Notes:
//  - Points are returned on integer coordinates (rounded from continuous candidates).
//  - The distribution is "blue-noise": well-spaced without obvious grid patterns.
//  - Deterministic for a given RNG stream.
inline std::vector<Vec2i> poissonDiscSample2D(RNG& rng,
                                              int minX, int minY,
                                              int maxX, int maxY,
                                              float minDist,
                                              int k = 30) {
    std::vector<Vec2i> out;
    if (!(minDist > 0.0f)) return out;
    if (minX > maxX || minY > maxY) return out;

    // Acceleration grid cell size (Bridson): r / sqrt(2).
    const float cellSize = minDist / std::sqrt(2.0f);
    if (!(cellSize > 0.0f)) return out;

    const int domW = (maxX - minX + 1);
    const int domH = (maxY - minY + 1);
    const int gridW = std::max(1, static_cast<int>(std::ceil(static_cast<float>(domW) / cellSize)));
    const int gridH = std::max(1, static_cast<int>(std::ceil(static_cast<float>(domH) / cellSize)));

    std::vector<int> grid(static_cast<size_t>(gridW * gridH), -1);
    std::vector<int> active;
    active.reserve(64);

    auto inGrid = [&](int gx, int gy) {
        return gx >= 0 && gy >= 0 && gx < gridW && gy < gridH;
    };

    auto gridIndex = [&](int gx, int gy) -> size_t {
        return static_cast<size_t>(gy * gridW + gx);
    };

    auto gridPos = [&](int x, int y) -> Vec2i {
        const float fx = static_cast<float>(x - minX) / cellSize;
        const float fy = static_cast<float>(y - minY) / cellSize;
        return { static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy)) };
    };

    auto fits = [&](int x, int y) -> bool {
        if (x < minX || x > maxX || y < minY || y > maxY) return false;

        const Vec2i gp = gridPos(x, y);
        const int gx = gp.x;
        const int gy = gp.y;
        if (!inGrid(gx, gy)) return false;

        const float r2 = minDist * minDist;

        // With cellSize=r/sqrt(2), checking +/-2 cells is sufficient.
        for (int yy = gy - 2; yy <= gy + 2; ++yy) {
            for (int xx = gx - 2; xx <= gx + 2; ++xx) {
                if (!inGrid(xx, yy)) continue;
                const int pi = grid[gridIndex(xx, yy)];
                if (pi < 0) continue;
                const Vec2i& p = out[static_cast<size_t>(pi)];
                const float dx = static_cast<float>(x - p.x);
                const float dy = static_cast<float>(y - p.y);
                if (dx * dx + dy * dy < r2) return false;
            }
        }

        return true;
    };

    // Seed a first sample.
    const int sx = rng.range(minX, maxX);
    const int sy = rng.range(minY, maxY);
    out.push_back({sx, sy});
    active.push_back(0);
    {
        const Vec2i gp = gridPos(sx, sy);
        if (inGrid(gp.x, gp.y)) grid[gridIndex(gp.x, gp.y)] = 0;
    }

    const float twoPi = 6.283185307179586f;

    while (!active.empty()) {
        const int ai = rng.range(0, static_cast<int>(active.size()) - 1);
        const int baseIdx = active[static_cast<size_t>(ai)];
        const Vec2i base = out[static_cast<size_t>(baseIdx)];

        bool found = false;
        for (int attempt = 0; attempt < k; ++attempt) {
            const float ang = rng.next01() * twoPi;
            const float rad = minDist * (1.0f + rng.next01()); // [r, 2r)

            const float fx = static_cast<float>(base.x) + std::cos(ang) * rad;
            const float fy = static_cast<float>(base.y) + std::sin(ang) * rad;
            const int x = static_cast<int>(std::lround(fx));
            const int y = static_cast<int>(std::lround(fy));

            if (!fits(x, y)) continue;

            const int newIdx = static_cast<int>(out.size());
            out.push_back({x, y});
            active.push_back(newIdx);

            const Vec2i gp = gridPos(x, y);
            if (inGrid(gp.x, gp.y)) grid[gridIndex(gp.x, gp.y)] = newIdx;

            found = true;
            break;
        }

        if (!found) {
            // Retire this active sample.
            active[static_cast<size_t>(ai)] = active.back();
            active.pop_back();
        }
    }

    // De-duplicate after rounding (rare).
    std::sort(out.begin(), out.end(), [](const Vec2i& a, const Vec2i& b) {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const Vec2i& a, const Vec2i& b) {
        return a.x == b.x && a.y == b.y;
    }), out.end());

    return out;
}

} // namespace procgen
