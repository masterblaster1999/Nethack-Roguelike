#pragma once

#include "common.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

// A tiny fixed-bucket spatial hash grid for fast minimum-distance checks.
//
// Intended for procedural generation passes where we place many features and
// need to enforce a minimum separation radius without repeatedly scanning all
// previously placed points.
//
// This implementation is deterministic, allocation-light (bucket arrays are
// pre-sized), and has no dependency on unordered_map.
class SpatialHashGrid2D {
public:
    SpatialHashGrid2D(int worldW, int worldH, int cellSize)
        : worldW_(std::max(0, worldW)),
          worldH_(std::max(0, worldH)),
          cellSize_(std::max(1, cellSize)) {
        gridW_ = (worldW_ + cellSize_ - 1) / cellSize_;
        gridH_ = (worldH_ + cellSize_ - 1) / cellSize_;
        buckets_.resize(static_cast<size_t>(std::max(1, gridW_ * gridH_)));
    }

    void clear() {
        for (auto& b : buckets_) b.clear();
    }

    void insert(const Vec2i& p) {
        buckets_[bucketIndex(p)].push_back(p);
    }

    // Returns true if any previously inserted point is strictly closer than
    // `radius` (Euclidean) to `p`. This mirrors common procgen checks that use
    // `distSq < r*r` (not <=).
    bool anyWithinRadius(const Vec2i& p, int radius) const {
        if (buckets_.empty()) return false;
        if (radius <= 0) return false;
        const int r2 = radius * radius;

        const int gx = p.x / cellSize_;
        const int gy = p.y / cellSize_;

        // How many grid cells we must scan to fully cover `radius`.
        const int rCells = std::max(1, (radius + cellSize_ - 1) / cellSize_);

        for (int oy = -rCells; oy <= rCells; ++oy) {
            for (int ox = -rCells; ox <= rCells; ++ox) {
                const int nx = gx + ox;
                const int ny = gy + oy;
                if (nx < 0 || ny < 0 || nx >= gridW_ || ny >= gridH_) continue;
                const auto& bucket = buckets_[static_cast<size_t>(ny * gridW_ + nx)];
                for (const Vec2i& q : bucket) {
                    const int dx = p.x - q.x;
                    const int dy = p.y - q.y;
                    if (dx * dx + dy * dy < r2) return true;
                }
            }
        }
        return false;
    }

private:
    size_t bucketIndex(const Vec2i& p) const {
        if (gridW_ <= 0 || gridH_ <= 0) return 0u;
        const int gx = std::clamp(p.x / cellSize_, 0, gridW_ - 1);
        const int gy = std::clamp(p.y / cellSize_, 0, gridH_ - 1);
        return static_cast<size_t>(gy * gridW_ + gx);
    }

    int worldW_ = 0;
    int worldH_ = 0;
    int cellSize_ = 1;
    int gridW_ = 1;
    int gridH_ = 1;
    std::vector<std::vector<Vec2i>> buckets_;
};
