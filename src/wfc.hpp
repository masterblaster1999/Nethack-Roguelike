#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "rng.hpp"

// ------------------------------------------------------------
// Minimal Wave Function Collapse (WFC) solver for small grids.
//
// This is a lightweight, deterministic constraint solver intended for
// roguelike procgen tasks (room furnishing, micro-patterns, etc.).
//
// Notes:
// - Domains are stored as 32-bit bitmasks (nTiles must be <= 32).
// - Rules are provided as per-tile, per-direction allowed-neighbor masks.
// - Solver uses greedy "lowest entropy" collapse + constraint propagation.
// - On contradictions, the solver restarts from the initial domains.
// ------------------------------------------------------------

namespace wfc {

struct SolveStats {
    int restarts = 0;
    int contradictions = 0;
};

inline int popcount32(uint32_t v) {
    // Portable enough for our build environment.
#if defined(__GNUG__) || defined(__clang__)
    return __builtin_popcount(v);
#else
    // Fallback: Brian Kernighan.
    int c = 0;
    while (v) { v &= (v - 1u); ++c; }
    return c;
#endif
}

inline int ctz32(uint32_t v) {
#if defined(__GNUG__) || defined(__clang__)
    return (v == 0u) ? 32 : __builtin_ctz(v);
#else
    if (v == 0u) return 32;
    int n = 0;
    while ((v & 1u) == 0u) { v >>= 1u; ++n; }
    return n;
#endif
}

inline uint32_t allMask(int nTiles) {
    if (nTiles <= 0) return 0u;
    if (nTiles >= 32) return 0xFFFFFFFFu;
    return (1u << static_cast<uint32_t>(nTiles)) - 1u;
}

inline int pickWeightedFromMask(uint32_t mask, const std::vector<float>& weights, RNG& rng) {
    if (mask == 0u) return -1;

    // Sum weights across available tiles.
    float total = 0.0f;
    for (uint32_t m = mask; m != 0u; m &= (m - 1u)) {
        const int t = ctz32(m);
        if (t >= 0 && static_cast<size_t>(t) < weights.size()) {
            total += std::max(0.0f, weights[static_cast<size_t>(t)]);
        }
    }

    // If weights are degenerate, fall back to uniform.
    if (!(total > 0.0f)) {
        const int n = popcount32(mask);
        const int pick = rng.range(0, std::max(0, n - 1));
        int k = 0;
        for (uint32_t m = mask; m != 0u; m &= (m - 1u)) {
            const int t = ctz32(m);
            if (k == pick) return t;
            ++k;
        }
        return ctz32(mask);
    }

    float r = rng.next01() * total;
    int last = -1;
    for (uint32_t m = mask; m != 0u; m &= (m - 1u)) {
        const int t = ctz32(m);
        last = t;
        float w = 1.0f;
        if (t >= 0 && static_cast<size_t>(t) < weights.size()) {
            w = std::max(0.0f, weights[static_cast<size_t>(t)]);
        }
        r -= w;
        if (r <= 0.0f) return t;
    }
    return last;
}

inline uint32_t unionAllowed(uint32_t domain, const std::vector<uint32_t>& allowForDir) {
    uint32_t out = 0u;
    for (uint32_t m = domain; m != 0u; m &= (m - 1u)) {
        const int t = ctz32(m);
        if (t >= 0 && static_cast<size_t>(t) < allowForDir.size()) {
            out |= allowForDir[static_cast<size_t>(t)];
        }
    }
    return out;
}

// Solve a WFC problem on a w*h grid.
//
// - allow[dir][tile] is a bitmask of tiles allowed in the neighbor cell
//   in direction dir from the current cell.
//   dir ordering: 0=+X, 1=-X, 2=+Y, 3=-Y.
//
// initialDomains may be empty (meaning "all tiles allowed" everywhere)
// or size exactly w*h.
//
// Returns true if solved; outTiles receives per-cell tile ids (0..nTiles-1).
inline bool solve(int w, int h,
                  int nTiles,
                  const std::vector<uint32_t> allow[4],
                  const std::vector<float>& weights,
                  RNG& rng,
                  const std::vector<uint32_t>& initialDomains,
                  std::vector<uint8_t>& outTiles,
                  int maxRestarts = 10,
                  SolveStats* outStats = nullptr) {
    if (w <= 0 || h <= 0) return false;
    if (nTiles <= 0 || nTiles > 32) return false;
    const size_t N = static_cast<size_t>(w * h);

    for (int dir = 0; dir < 4; ++dir) {
        if (allow[dir].size() != static_cast<size_t>(nTiles)) return false;
    }

    const uint32_t fullMask = allMask(nTiles);
    if (fullMask == 0u) return false;

    if (!initialDomains.empty() && initialDomains.size() != N) return false;

    std::vector<uint32_t> dom;
    dom.resize(N, fullMask);

    // Work queue for propagation.
    std::vector<int> q;
    q.reserve(N);

    auto idx = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y * w + x);
    };

    auto push = [&](int cell) {
        q.push_back(cell);
    };

    auto inBounds = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h;
    };

    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    const int restartCap = std::max(0, maxRestarts);
    int contradictions = 0;

    for (int attempt = 0; attempt <= restartCap; ++attempt) {
        bool failed = false;

        // Reset domains.
        if (initialDomains.empty()) {
            std::fill(dom.begin(), dom.end(), fullMask);
        } else {
            dom = initialDomains;
        }

        // Seed propagation from all pre-restricted cells.
        q.clear();
        for (size_t i = 0; i < N; ++i) {
            if (dom[i] == 0u) {
                contradictions++;
                failed = true;
                break;
            }
            if (dom[i] != fullMask) {
                push(static_cast<int>(i));
            }
        }
        if (failed) continue;

        // Propagation lambda (returns false on contradiction).
        auto propagate = [&]() -> bool {
            size_t head = 0;
            while (head < q.size()) {
                const int cur = q[head++];
                if (cur < 0 || static_cast<size_t>(cur) >= N) continue;

                const int cx = cur % w;
                const int cy = cur / w;
                const uint32_t curDom = dom[static_cast<size_t>(cur)];
                if (curDom == 0u) return false;

                for (int dir = 0; dir < 4; ++dir) {
                    const int nx = cx + dx[dir];
                    const int ny = cy + dy[dir];
                    if (!inBounds(nx, ny)) continue;

                    const size_t ni = idx(nx, ny);
                    const uint32_t allowed = unionAllowed(curDom, allow[dir]);
                    const uint32_t oldDom = dom[ni];
                    const uint32_t newDom = oldDom & allowed;
                    if (newDom == 0u) return false;
                    if (newDom != oldDom) {
                        dom[ni] = newDom;
                        push(static_cast<int>(ni));
                    }
                }
            }
            return true;
        };

        if (!propagate()) {
            contradictions++;
            continue;
        }

        // Collapse loop.
        for (int safety = 0; safety < w * h * 8; ++safety) {
            int bestEntropy = std::numeric_limits<int>::max();
            std::vector<int> best;
            best.reserve(32);

            for (size_t i = 0; i < N; ++i) {
                const uint32_t m = dom[i];
                const int e = popcount32(m);
                if (e <= 1) continue;
                if (e < bestEntropy) {
                    bestEntropy = e;
                    best.clear();
                    best.push_back(static_cast<int>(i));
                } else if (e == bestEntropy) {
                    best.push_back(static_cast<int>(i));
                }
            }

            // Done (all collapsed).
            if (best.empty()) {
                outTiles.assign(N, 0);
                for (size_t i = 0; i < N; ++i) {
                    const uint32_t m = dom[i];
                    const int t = ctz32(m);
                    outTiles[i] = static_cast<uint8_t>(std::max(0, t));
                }

                if (outStats) {
                    outStats->restarts = attempt;
                    outStats->contradictions = contradictions;
                }
                return true;
            }

            // Pick a minimal-entropy cell at random.
            const int pickCell = best[static_cast<size_t>(rng.range(0, static_cast<int>(best.size()) - 1))];
            if (pickCell < 0 || static_cast<size_t>(pickCell) >= N) {
                contradictions++;
                failed = true;
                break;
            }

            const uint32_t cellDom = dom[static_cast<size_t>(pickCell)];
            const int chosen = pickWeightedFromMask(cellDom, weights, rng);
            if (chosen < 0 || chosen >= nTiles) {
                contradictions++;
                failed = true;
                break;
            }

            dom[static_cast<size_t>(pickCell)] = (1u << static_cast<uint32_t>(chosen));
            q.clear();
            push(pickCell);
            if (!propagate()) {
                contradictions++;
                failed = true;
                break;
            }
        }

        if (failed) continue;

        // If we hit safety cap, treat as a restart.
        contradictions++;
    }

    if (outStats) {
        outStats->restarts = restartCap;
        outStats->contradictions = contradictions;
    }
    return false;
}

} // namespace wfc
