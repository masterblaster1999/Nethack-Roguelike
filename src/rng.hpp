#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>

// Compile-time tag hashing (FNV-1a) for readable domain separation.
// Useful for salting procedural generators without magic hex constants.
//
// Example:
//   uint32_t s = hashCombine(levelSeed, tag32("BIOLUM"));
constexpr uint32_t fnv1a32(const char* data, std::size_t len) {
    uint32_t h = 2166136261u; // FNV offset basis
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(static_cast<unsigned char>(data[i]));
        h *= 16777619u; // FNV prime
    }
    return h;
}

template <std::size_t N>
constexpr uint32_t tag32(const char (&str)[N]) {
    // N includes the null terminator for string literals.
    return fnv1a32(str, (N > 0) ? (N - 1) : 0);
}




// User-defined literal for compile-time tag hashing.
// Example:  uint32_t s = hashCombine(seed, "BIOLUM"_tag);
constexpr uint32_t operator"" _tag(const char* str, std::size_t len) {
    return fnv1a32(str, len);
}

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

    // Back-compat shorthand used by some older procedural generators.
    uint32_t u32() { return nextU32(); }

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

// Variadic hash combine convenience helpers.
//
// Several procedural systems want to combine more than two values. Keep the
// implementation header-only and deterministic.
inline uint32_t hashCombine(uint32_t a, uint32_t b, uint32_t c) {
    return hashCombine(hashCombine(a, b), c);
}

template <typename... Rest>
inline uint32_t hashCombine(uint32_t a, uint32_t b, uint32_t c, Rest... rest) {
    uint32_t h = hashCombine(a, b, c);
    ((h = hashCombine(h, static_cast<uint32_t>(rest))), ...);
    return h;
}

// Convert a 32-bit integer hash into a stable float in [0, 1).
//
// Useful for cheap deterministic noise without having to allocate a full RNG
// instance (e.g. per-pixel variation in procedural sprites).
inline float rand01(uint32_t h) {
    return (h / (static_cast<float>(std::numeric_limits<uint32_t>::max()) + 1.0f));
}
