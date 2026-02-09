#pragma once

// Noise localization helpers.
//
// This module provides a deterministic "sound source localization" model used by AI.
//
// Goal:
//   - Quiet or distant noises shouldn't pinpoint an exact tile.
//   - Monsters investigate an approximate area, with uncertainty increasing as the
//     sound approaches their hearing threshold.
//   - The result must be deterministic and must not consume the main RNG stream
//     (so replays and procedural generation remain stable aside from intended logic changes).
//
// The model is intentionally lightweight:
//   - We compute an uncertainty radius from (volume, effective volume, distance cost).
//   - We derive a stable per-monster, per-turn offset from a small integer hash.

#include "common.hpp"
#include "rng.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>

inline int noiseInvestigateRadius(int volume, int effVolume, int distCost) {
    // distCost: dungeon-aware propagation cost from the noise source to the listener.
    // effVolume: volume adjusted for monster hearing (volume + hearingDelta).
    if (volume <= 0) return 0;
    if (effVolume <= 0) return 0;
    if (distCost <= 0) return 0;

    // If the sound is very loud in absolute terms, treat it as precisely localizable.
    // (This keeps big events like explosions or alarms behaving "snappy".)
    if (volume >= 18) return 0;

    // Margin = how far above the hearing threshold we are.
    // margin == 0 => barely audible => high uncertainty.
    const int margin = std::max(0, effVolume - distCost);

    // Base radius: threshold => 4, +2 margin reduces radius by 1.
    int radius = 4 - (margin / 2);

    // Loud-ish sounds are easier to localize even at similar margins.
    if (volume >= 14) radius -= 1;

    // Clamp.
    radius = std::clamp(radius, 0, 6);

    // Very nearby sources should still be effectively exact.
    if (distCost <= 2) radius = 0;

    return radius;
}

// Derive a stable hash for a monster hearing a specific noise event.
inline uint32_t noiseInvestigateHash(uint32_t runSeed, uint32_t turn, int monsterId, Vec2i src, int volume, int effVolume, int distCost) {
    // Spatial mixing primes.
    const uint32_t sx = static_cast<uint32_t>(src.x) * 73856093u;
    const uint32_t sy = static_cast<uint32_t>(src.y) * 19349663u;
    const uint32_t sp = sx ^ sy;

    uint32_t h = hashCombine(runSeed, "NOISE"_tag);
    h = hashCombine(h, turn);
    h = hashCombine(h, static_cast<uint32_t>(monsterId));
    h = hashCombine(h, sp);

    // Pack several small ints into one 32-bit lane to keep mixing cheap.
    const uint32_t packed = (static_cast<uint32_t>(volume) & 0xFFu)
        | ((static_cast<uint32_t>(effVolume) & 0xFFu) << 8)
        | ((static_cast<uint32_t>(distCost) & 0xFFu) << 16);

    h = hashCombine(h, packed);
    return h;
}

// Convert a hash into a deterministic offset within [-radius, +radius] on each axis.
inline Vec2i noiseInvestigateOffset(uint32_t h, int radius) {
    if (radius <= 0) return Vec2i{0, 0};

    // Sample from a discrete Manhattan-diamond (not a square) to avoid
    // over-representing far diagonal offsets for a given uncertainty radius.
    //
    // Number of lattice points with |dx| + |dy| <= r:
    //   1 + 2*r*(r+1)
    const uint32_t count = static_cast<uint32_t>(1 + 2 * radius * (radius + 1));
    uint32_t pick = hash32(h) % count;

    for (int r = 0; r <= radius; ++r) {
        for (int dx = -r; dx <= r; ++dx) {
            const int dyAbs = r - std::abs(dx);
            if (dyAbs == 0) {
                if (pick == 0u) return Vec2i{dx, 0};
                --pick;
            } else {
                if (pick == 0u) return Vec2i{dx, dyAbs};
                --pick;
                if (pick == 0u) return Vec2i{dx, -dyAbs};
                --pick;
            }
        }
    }

    return Vec2i{0, 0};
}
