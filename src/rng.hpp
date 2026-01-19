#pragma once
#include <cstdint>
#include <limits>

// Simple, fast RNG with deterministic cross-platform behavior.
// Not cryptographically secure.
struct RNG {
    uint32_t state;

    explicit RNG(uint32_t seed = 0x12345678u) : state(seed ? seed : 0x12345678u) {}

    uint32_t nextU32() {
        // xorshift32
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    int range(int lo, int hiInclusive) {
        if (hiInclusive <= lo) return lo;
        uint32_t span = static_cast<uint32_t>(hiInclusive - lo + 1);
        return lo + static_cast<int>(nextU32() % span);
    }

    float next01() {
        // [0,1)
        return (nextU32() / (static_cast<float>(std::numeric_limits<uint32_t>::max()) + 1.0f));
    }

    bool chance(float p) {
        return next01() < p;
    }
};

// A tiny integer hash for stable variation (tile variants, etc).
inline uint32_t hash32(uint32_t x) {
    // Thomas Wang-ish mix
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

inline uint32_t hashCombine(uint32_t a, uint32_t b) {
    return hash32(a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2)));
}

// Convert a 32-bit integer hash into a stable float in [0, 1).
//
// Useful for cheap deterministic noise without having to allocate a full RNG
// instance (e.g. per-pixel variation in procedural sprites).
inline float rand01(uint32_t h) {
    return (h / (static_cast<float>(std::numeric_limits<uint32_t>::max()) + 1.0f));
}
