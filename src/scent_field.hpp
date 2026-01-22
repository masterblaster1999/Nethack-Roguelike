#pragma once

// Scent field helper.
//
// This implements a lightweight, deterministic "scent trail" simulation that can be
// used by AI to track the player around corners.
//
// Core model:
//   1) Per-turn global decay (material dependent).
//   2) Deposit scent at the source tile.
//   3) One relaxation/spread pass along walkable tiles.
//
// Wind model (optional):
//   When a per-level wind is present, scent spreading is biased so that traveling
//   *with* the wind has a smaller loss (tailwind), while traveling *against* the
//   wind has a larger loss (headwind). This creates elongated downwind scent
//   gradients without requiring expensive multi-pass diffusion.

#include "common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

struct ScentCellFx {
    // Additive modifiers applied to base parameters.
    int decayDelta = 0;
    int spreadDropDelta = 0;
};

struct ScentFieldParams {
    // Base global decay per turn (before material deltas).
    int baseDecay = 2;
    // Base scent loss per-tile when spreading (before material deltas).
    int baseSpreadDrop = 14;

    // Clamps for derived parameters.
    int maxDecay = 20;
    int minSpreadDrop = 6;
    int maxSpreadDrop = 40;

    // Wind parameters.
    Vec2i windDir{0, 0};
    int windStrength = 0; // 0..3

    // How much wind changes spread loss.
    // Tailwind reduces drop; headwind increases it.
    int tailwindDropBiasPerStrength = 2;
    int headwindDropBiasPerStrength = 3;
};

// Update an in-place scent field.
//
// Parameters:
//   - width/height: grid dimensions.
//   - field: per-tile intensity (0..255), updated in-place.
//   - depositPos/depositStrength: scent source deposit for this turn.
//   - isWalkable(x,y): returns true if scent can exist/spread on the tile.
//   - fxAt(x,y): returns material modifiers for the tile.
//   - params: global tunables (including optional wind bias).
//
// Notes:
//   - Deterministic (no RNG).
//   - Uses only cardinal spreading (4-neighborhood) for stable gradients.
//   - Non-walkable tiles are forced to 0 each update so scent can't "leak" through walls.
template <typename WalkableFn, typename FxFn>
inline void updateScentField(int width,
                             int height,
                             std::vector<uint8_t>& field,
                             Vec2i depositPos,
                             uint8_t depositStrength,
                             const WalkableFn& isWalkable,
                             const FxFn& fxAt,
                             const ScentFieldParams& params = ScentFieldParams{}) {
    if (width <= 0 || height <= 0) return;

    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (n == 0) return;

    if (field.size() != n) {
        field.assign(n, 0u);
    }

    auto inBounds = [&](int x, int y) -> bool {
        return (x >= 0 && y >= 0 && x < width && y < height);
    };
    auto idx = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
    };

    // --- Phase 1: global decay ---
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = idx(x, y);

            if (!isWalkable(x, y)) {
                field[i] = 0u;
                continue;
            }

            const uint8_t v = field[i];
            if (v == 0u) continue;

            const ScentCellFx fx = fxAt(x, y);
            const int decay = clampi(params.baseDecay + fx.decayDelta, 0, params.maxDecay);
            field[i] = (v > static_cast<uint8_t>(decay)) ? static_cast<uint8_t>(v - static_cast<uint8_t>(decay)) : 0u;
        }
    }

    // --- Phase 2: deposit at source ---
    if (depositStrength > 0u && inBounds(depositPos.x, depositPos.y) && isWalkable(depositPos.x, depositPos.y)) {
        const size_t pi = idx(depositPos.x, depositPos.y);
        field[pi] = std::max(field[pi], depositStrength);
    }

    // --- Phase 3: one spread/relaxation pass ---
    std::vector<uint8_t> next = field;

    const bool windy = (params.windStrength > 0) && !(params.windDir.x == 0 && params.windDir.y == 0);

    auto windDropAdjust = [&](int travelDx, int travelDy) -> int {
        if (!windy) return 0;

        // travelDx/travelDy is the direction scent travels (from neighbor -> current).
        if (travelDx == params.windDir.x && travelDy == params.windDir.y) {
            // Tailwind: scent travels further downwind.
            return -params.tailwindDropBiasPerStrength * params.windStrength;
        }
        if (travelDx == -params.windDir.x && travelDy == -params.windDir.y) {
            // Headwind: scent struggles to travel upwind.
            return params.headwindDropBiasPerStrength * params.windStrength;
        }
        return 0;
    };

    constexpr int dirs4[4][2] = {
        {1, 0},
        {-1, 0},
        {0, 1},
        {0, -1},
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = idx(x, y);

            if (!isWalkable(x, y)) {
                next[i] = 0u;
                continue;
            }

            const ScentCellFx fx = fxAt(x, y);
            const int baseDrop = params.baseSpreadDrop + fx.spreadDropDelta;

            uint8_t best = 0u;

            for (const auto& d : dirs4) {
                const int nx = x + d[0];
                const int ny = y + d[1];
                if (!inBounds(nx, ny)) continue;
                if (!isWalkable(nx, ny)) continue;

                const uint8_t nv = field[idx(nx, ny)];
                if (nv == 0u) continue;

                // Direction scent is travelling from neighbor -> current.
                const int tdx = x - nx;
                const int tdy = y - ny;
                const int drop = clampi(baseDrop + windDropAdjust(tdx, tdy), params.minSpreadDrop, params.maxSpreadDrop);

                const uint8_t cand = (nv > static_cast<uint8_t>(drop)) ? static_cast<uint8_t>(nv - static_cast<uint8_t>(drop)) : 0u;
                if (cand > best) best = cand;
            }

            if (best > next[i]) next[i] = best;
        }
    }

    field.swap(next);
}
