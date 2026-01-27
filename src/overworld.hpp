#pragma once

#include "dungeon.hpp"
#include "rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Infinite overworld chunks (Camp branch, depth 0)
// -----------------------------------------------------------------------------
//
// The surface camp is the hub at overworld (0,0). Stepping through edge gates
// moves between adjacent wilderness chunks without changing branch/depth.
//
// Each chunk is generated deterministically from (runSeed, chunkX, chunkY) and is
// intentionally decoupled from the gameplay RNG stream.
//
// Design goals for the wilderness generator:
//  * Solid border walls with deterministic edge gates (shared per chunk boundary).
//  * Continuous terrain across chunk borders (no seams) via world-coordinate noise.
//  * Each chunk has a lightweight deterministic identity: biome, name, danger depth.
//  * Edge gates are always mutually connected via meandering trails.
// -----------------------------------------------------------------------------

namespace overworld {

// Deterministic per-chunk seed (for local placement decisions).
inline uint32_t chunkSeed(uint32_t runSeed, int chunkX, int chunkY) {
    uint32_t s = runSeed;
    s = hashCombine(s, "OW_CHUNK"_tag);
    s = hashCombine(s, static_cast<uint32_t>(chunkX));
    s = hashCombine(s, static_cast<uint32_t>(chunkY));
    if (s == 0u) s = 1u;
    return s;
}

// Domain-separated seed for overworld terrain fields (continuous across chunks).
inline uint32_t terrainBaseSeed(uint32_t runSeed) {
    uint32_t s = hashCombine(runSeed, "OW_TERRAIN"_tag);
    if (s == 0u) s = 1u;
    return s;
}

// Domain-separated seed for per-chunk material palettes.
inline uint32_t materialSeed(uint32_t runSeed, int chunkX, int chunkY) {
    uint32_t s = hashCombine(runSeed, "OW_MAT"_tag);
    s = hashCombine(s, static_cast<uint32_t>(chunkX));
    s = hashCombine(s, static_cast<uint32_t>(chunkY));
    if (s == 0u) s = 1u;
    return s;
}

// Domain-separated seed for per-chunk naming.
inline uint32_t nameSeed(uint32_t runSeed, int chunkX, int chunkY) {
    uint32_t s = hashCombine(runSeed, "OW_NAME"_tag);
    s = hashCombine(s, static_cast<uint32_t>(chunkX));
    s = hashCombine(s, static_cast<uint32_t>(chunkY));
    if (s == 0u) s = 1u;
    return s;
}

inline int manhattanDist(int x, int y) {
    return std::abs(x) + std::abs(y);
}

// Overworld danger is a depth-like scalar that grows with distance from (0,0).
// This biases spawns and other depth-aware procgen without changing the actual
// branch/depth (which remain Camp/0 for the overworld).
inline int dangerDepthFor(int chunkX, int chunkY, int maxDepth) {
    if (chunkX == 0 && chunkY == 0) return 0; // home camp
    const int d = manhattanDist(chunkX, chunkY);
    const int raw = 1 + d * 2;
    const int hi = std::max(1, maxDepth);
    return std::clamp(raw, 1, hi);
}

struct ChunkGates {
    Vec2i north{-1, -1}; // x, y=0
    Vec2i south{-1, -1}; // x, y=h-1
    Vec2i west{-1, -1};  // x=0, y
    Vec2i east{-1, -1};  // x=w-1, y
};

// Gates are *shared per chunk boundary* so the trail network can form continuous
// cross-chunk roads without seams.
//
// Vertical boundary key (V): between (bx, y) and (bx+1, y) => shared Y coordinate.
// Horizontal boundary key (H): between (x, by) and (x, by+1) => shared X coordinate.
//
// Home-camp-adjacent boundaries are pinned to mid-edge to preserve the camp layout.
inline bool boundaryTouchesHomeCampVertical(int boundaryX, int chunkY) {
    // Boundary between (boundaryX, chunkY) and (boundaryX+1, chunkY).
    return (chunkY == 0 && (boundaryX == -1 || boundaryX == 0));
}

inline bool boundaryTouchesHomeCampHorizontal(int chunkX, int boundaryY) {
    // Boundary between (chunkX, boundaryY) and (chunkX, boundaryY+1).
    return (chunkX == 0 && (boundaryY == -1 || boundaryY == 0));
}

inline int sharedGateOffsetVertical(uint32_t runSeed, int boundaryX, int chunkY, int height) {
    const int mid = height / 2;

    // Avoid corners so the "throat" tile is always in-bounds and readable.
    const int lo0 = 2;
    const int hi0 = height - 3;

    const int lo = (hi0 >= lo0) ? lo0 : std::max(1, mid);
    const int hi = (hi0 >= lo0) ? hi0 : lo;

    const int midSafe = std::clamp(mid, lo, hi);

    if (boundaryTouchesHomeCampVertical(boundaryX, chunkY)) return midSafe;

    uint32_t s = hashCombine(runSeed, "OW_GATE_V"_tag);
    s = hashCombine(s, static_cast<uint32_t>(boundaryX));
    s = hashCombine(s, static_cast<uint32_t>(chunkY));
    s = hash32(s);
    RNG r(s == 0u ? 1u : s);
    return r.range(lo, hi);
}

inline int sharedGateOffsetHorizontal(uint32_t runSeed, int chunkX, int boundaryY, int width) {
    const int mid = width / 2;

    const int lo0 = 2;
    const int hi0 = width - 3;

    const int lo = (hi0 >= lo0) ? lo0 : std::max(1, mid);
    const int hi = (hi0 >= lo0) ? hi0 : lo;

    const int midSafe = std::clamp(mid, lo, hi);

    if (boundaryTouchesHomeCampHorizontal(chunkX, boundaryY)) return midSafe;

    uint32_t s = hashCombine(runSeed, "OW_GATE_H"_tag);
    s = hashCombine(s, static_cast<uint32_t>(chunkX));
    s = hashCombine(s, static_cast<uint32_t>(boundaryY));
    s = hash32(s);
    RNG r(s == 0u ? 1u : s);
    return r.range(lo, hi);
}

inline ChunkGates gatePositions(const Dungeon& d, uint32_t runSeed, int chunkX, int chunkY) {
    ChunkGates g;

    // North boundary is between (chunkX, chunkY-1) and (chunkX, chunkY).
    const int nx = sharedGateOffsetHorizontal(runSeed, chunkX, chunkY - 1, d.width);
    // South boundary is between (chunkX, chunkY) and (chunkX, chunkY+1).
    const int sx = sharedGateOffsetHorizontal(runSeed, chunkX, chunkY, d.width);

    // West boundary is between (chunkX-1, chunkY) and (chunkX, chunkY).
    const int wy = sharedGateOffsetVertical(runSeed, chunkX - 1, chunkY, d.height);
    // East boundary is between (chunkX, chunkY) and (chunkX+1, chunkY).
    const int ey = sharedGateOffsetVertical(runSeed, chunkX, chunkY, d.height);

    g.north = { nx, 0 };
    g.south = { sx, d.height - 1 };
    g.west  = { 0, wy };
    g.east  = { d.width - 1, ey };
    return g;
}

inline void ensureBorderWalls(Dungeon& d) {
    for (int x = 0; x < d.width; ++x) {
        d.at(x, 0).type = TileType::Wall;
        d.at(x, d.height - 1).type = TileType::Wall;
    }
    for (int y = 0; y < d.height; ++y) {
        d.at(0, y).type = TileType::Wall;
        d.at(d.width - 1, y).type = TileType::Wall;
    }
}

inline void ensureBorderGates(Dungeon& d, uint32_t runSeed, int chunkX, int chunkY) {
    const ChunkGates g = gatePositions(d, runSeed, chunkX, chunkY);

    // Gate mask: bit 0 = north, 1 = south, 2 = west, 3 = east.
    d.gateMask = 0u;
    d.gatePositions.clear();

    auto carveGate = [&](Vec2i p, uint8_t bit) {
        if (!d.inBounds(p.x, p.y)) return;

        d.at(p.x, p.y).type = TileType::Floor;

        // Carve a 1-tile throat inward so you can step through without hugging the border.
        if (p.y == 0 && d.inBounds(p.x, p.y + 1)) d.at(p.x, p.y + 1).type = TileType::Floor;
        if (p.y == d.height - 1 && d.inBounds(p.x, p.y - 1)) d.at(p.x, p.y - 1).type = TileType::Floor;
        if (p.x == 0 && d.inBounds(p.x + 1, p.y)) d.at(p.x + 1, p.y).type = TileType::Floor;
        if (p.x == d.width - 1 && d.inBounds(p.x - 1, p.y)) d.at(p.x - 1, p.y).type = TileType::Floor;

        d.gateMask |= (1u << bit);
        d.gatePositions.push_back(p);
    };

    carveGate(g.north, 0);
    carveGate(g.south, 1);
    carveGate(g.west,  2);
    carveGate(g.east,  3);
}

// --- Simple deterministic noise helpers (float, 0..1) ------------------------

inline float u32To01(uint32_t x) {
    return (x / (static_cast<float>(std::numeric_limits<uint32_t>::max()) + 1.0f));
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

inline float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float smoothstep01(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return 0.0f;
    float t = (x - edge0) / (edge1 - edge0);
    return smoothstep(t);
}

inline uint32_t hashCoord(uint32_t seed, int x, int y) {
    uint32_t h = seed;
    h = hashCombine(h, static_cast<uint32_t>(x));
    h = hashCombine(h, static_cast<uint32_t>(y));
    return hash32(h);
}

// 2D value noise, smoothed, in [0,1]
inline float valueNoise01(uint32_t seed, float x, float y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;

    const float tx = smoothstep01(0.0f, 1.0f, x - static_cast<float>(x0));
    const float ty = smoothstep01(0.0f, 1.0f, y - static_cast<float>(y0));

    const float v00 = u32To01(hashCoord(seed, x0, y0));
    const float v10 = u32To01(hashCoord(seed, x1, y0));
    const float v01 = u32To01(hashCoord(seed, x0, y1));
    const float v11 = u32To01(hashCoord(seed, x1, y1));

    const float vx0 = lerp(v00, v10, tx);
    const float vx1 = lerp(v01, v11, tx);
    return lerp(vx0, vx1, ty);
}

// Fractal Brownian motion: sum of octaves of value noise in [0,1].
inline float fbm01(uint32_t seed, float x, float y, int octaves) {
    float sum = 0.0f;
    float amp = 1.0f;
    float freq = 1.0f;
    float norm = 0.0f;

    const int o = std::max(1, octaves);
    for (int i = 0; i < o; ++i) {
        sum += valueNoise01(hashCombine(seed, static_cast<uint32_t>(i) * 0x9E3779B9u), x * freq, y * freq) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }

    if (norm > 0.0f) sum /= norm;
    return std::clamp(sum, 0.0f, 1.0f);
}

// -----------------------------------------------------------------------------
// Chunk identity: biome + name + danger depth
// -----------------------------------------------------------------------------

enum class Biome : uint8_t {
    Plains = 0,
    Forest,
    Swamp,
    Desert,
    Tundra,
    Highlands,
    Badlands,
    Coast,
};

inline const char* biomeName(Biome b) {
    switch (b) {
        case Biome::Plains:    return "PLAINS";
        case Biome::Forest:    return "FOREST";
        case Biome::Swamp:     return "SWAMP";
        case Biome::Desert:    return "DESERT";
        case Biome::Tundra:    return "TUNDRA";
        case Biome::Highlands: return "HIGHLANDS";
        case Biome::Badlands:  return "BADLANDS";
        case Biome::Coast:     return "COAST";
        default:               return "PLAINS";
    }
}


// -----------------------------------------------------------------------------
// Overworld weather (deterministic per chunk)
// -----------------------------------------------------------------------------
//
// Wilderness chunks expose a lightweight weather profile derived deterministically
// from (runSeed, chunkX, chunkY). This is intentionally *not* a full time simulation;
// it is a per-region "climate" snapshot that:
//   - provides coherent wind for scent/gas/fire drift on the overworld,
//   - occasionally reduces visibility (fog/snow/dust), and
//   - can quench fire during rain/storms.
//
// The profile is cheap to compute and does not consume gameplay RNG.

enum class WeatherKind : uint8_t {
    Clear = 0,
    Breezy,
    Windy,
    Fog,
    Rain,
    Storm,
    Snow,
    Dust,
};

inline const char* weatherName(WeatherKind w) {
    switch (w) {
        case WeatherKind::Clear:  return "CLEAR";
        case WeatherKind::Breezy: return "BREEZY";
        case WeatherKind::Windy:  return "WINDY";
        case WeatherKind::Fog:    return "FOG";
        case WeatherKind::Rain:   return "RAIN";
        case WeatherKind::Storm:  return "STORM";
        case WeatherKind::Snow:   return "SNOW";
        case WeatherKind::Dust:   return "DUST";
        default:                  return "CLEAR";
    }
}

struct WeatherProfile {
    WeatherKind kind = WeatherKind::Clear;

    // Wind: cardinal direction or {0,0} for calm.
    Vec2i windDir{0, 0};
    int windStrength = 0; // 0..3

    // Gameplay modifiers.
    int fovPenalty = 0;   // subtract from player FOV radius (0..5)
    int fireQuench = 0;   // extra per-turn decay for fireField (0..3)
    int burnQuench = 0;   // extra per-turn decay for burnTurns (0..2)
};

inline WeatherProfile weatherFor(uint32_t runSeed, int chunkX, int chunkY, Biome biome) {
    WeatherProfile w;

    const uint32_t base = hashCombine(runSeed, "OW_WEATHER"_tag);

    // Use the same broad climate fields as biome selection for coherence.
    const uint32_t biomeBase = hashCombine(runSeed, "OW_BIOME"_tag);
    const uint32_t sWet  = hashCombine(biomeBase, "WET"_tag);
    const uint32_t sTemp = hashCombine(biomeBase, "TEMP"_tag);

    const float fx = static_cast<float>(chunkX) * 0.23f;
    const float fy = static_cast<float>(chunkY) * 0.23f;

    float wet  = fbm01(sWet,  fx + 17.0f, fy - 29.0f, 4);
    float temp = fbm01(sTemp, fx - 53.0f, fy + 11.0f, 3);

    // Latitude bias: north/south is colder (match biome bias).
    const float lat = std::min(1.0f, std::abs(static_cast<float>(chunkY)) * 0.08f);
    temp = std::clamp(temp - lat * 0.45f, 0.0f, 1.0f);

    // Wind field: sample a large-scale noise potential and take a finite-difference gradient.
    const uint32_t sWind = hashCombine(base, "WIND"_tag);
    const float wfx = static_cast<float>(chunkX) * 0.17f;
    const float wfy = static_cast<float>(chunkY) * 0.17f;

    constexpr float eps = 0.65f;
    const float fxp = fbm01(sWind, wfx + eps, wfy, 3);
    const float fxm = fbm01(sWind, wfx - eps, wfy, 3);
    const float fyp = fbm01(sWind, wfx, wfy + eps, 3);
    const float fym = fbm01(sWind, wfx, wfy - eps, 3);

    const float gx = fxp - fxm;
    const float gy = fyp - fym;
    const float gmag = std::abs(gx) + std::abs(gy);

    Vec2i dir{0, 0};
    if (gmag > 0.025f) {
        if (std::abs(gx) > std::abs(gy)) dir = { (gx > 0.0f) ? 1 : -1, 0 };
        else                             dir = { 0, (gy > 0.0f) ? 1 : -1 };
    }

    int strength = 0;
    if (dir.x != 0 || dir.y != 0) {
        if (gmag < 0.055f) strength = 1;
        else if (gmag < 0.11f) strength = 2;
        else strength = 3;
    }

    // Biome wind bias.
    switch (biome) {
        case Biome::Highlands:
        case Biome::Coast:
        case Biome::Badlands:
            strength = std::min(3, strength + 1);
            break;
        case Biome::Forest:
        case Biome::Swamp:
            strength = std::max(0, strength - 1);
            break;
        default:
            break;
    }

    if (strength <= 0) {
        dir = {0, 0};
        strength = 0;
    }

    w.windDir = dir;
    w.windStrength = strength;

    // Micro-variation fields for fog/storm selection.
    const uint32_t sCloud = hashCombine(base, "CLOUD"_tag);
    const float cloud = fbm01(sCloud, fx + 91.0f, fy - 37.0f, 3);

    const uint32_t sFront = hashCombine(base, "FRONT"_tag);
    const float front = fbm01(sFront, fx - 13.0f, fy + 77.0f, 3);

    // Start with clear/windy, then overlay precipitation/visibility effects.
    w.kind = WeatherKind::Clear;
    if (strength >= 2) w.kind = WeatherKind::Windy;
    else if (strength == 1) w.kind = WeatherKind::Breezy;

    const bool arid = (biome == Biome::Desert || biome == Biome::Badlands);

    // Dust storms: arid + strong wind + some cloudiness.
    if (arid && strength >= 2 && wet < 0.30f && cloud > 0.55f) {
        w.kind = WeatherKind::Dust;
    }

    // Snow: cold + wet.
    if (temp < 0.22f && wet > 0.38f) {
        w.kind = WeatherKind::Snow;
    }

    // Fog: humid biomes, calm-ish, and humid cloud peak.
    const bool humidBiome = (biome == Biome::Swamp || biome == Biome::Coast || biome == Biome::Forest);
    if (humidBiome && wet > 0.55f && strength <= 1 && cloud > 0.52f) {
        w.kind = WeatherKind::Fog;
    }

    // Rain: wet climates (avoid if snow already).
    if (w.kind != WeatherKind::Snow && wet > 0.62f && temp > 0.18f && cloud > 0.46f) {
        w.kind = WeatherKind::Rain;
    }

    // Storm: wet + windy + front peak (avoid deserts; avoid snow).
    if (w.kind != WeatherKind::Snow && !arid && wet > 0.60f && strength >= 2 && front > 0.62f) {
        w.kind = WeatherKind::Storm;
    }

    // Gameplay tuning.
    switch (w.kind) {
        case WeatherKind::Fog:
            w.fovPenalty = 3;
            break;
        case WeatherKind::Dust:
            w.fovPenalty = 2;
            break;
        case WeatherKind::Snow:
            w.fovPenalty = 2;
            w.fireQuench = 1;
            w.burnQuench = 1;
            break;
        case WeatherKind::Rain:
            w.fovPenalty = 1;
            w.fireQuench = 2;
            w.burnQuench = 1;
            break;
        case WeatherKind::Storm:
            w.fovPenalty = 2;
            w.fireQuench = 3;
            w.burnQuench = 2;
            break;
        default:
            break;
    }

    return w;
}

inline Biome biomeFor(uint32_t runSeed, int chunkX, int chunkY) {
    const uint32_t base = hashCombine(runSeed, "OW_BIOME"_tag);
    const uint32_t sElev = hashCombine(base, "ELEV"_tag);
    const uint32_t sWet  = hashCombine(base, "WET"_tag);
    const uint32_t sTemp = hashCombine(base, "TEMP"_tag);

    // Chunk-space sampling (stable large-scale regions).
    const float fx = static_cast<float>(chunkX) * 0.23f;
    const float fy = static_cast<float>(chunkY) * 0.23f;

    float elev = fbm01(sElev, fx, fy, 4);
    float wet  = fbm01(sWet,  fx + 17.0f, fy - 29.0f, 4);
    float temp = fbm01(sTemp, fx - 53.0f, fy + 11.0f, 3);

    // Latitude bias: north/south is colder.
    const float lat = std::min(1.0f, std::abs(static_cast<float>(chunkY)) * 0.08f);
    temp = std::clamp(temp - lat * 0.45f, 0.0f, 1.0f);

    // Lowlands + above-average moisture => coastal.
    if (elev < 0.28f && wet > 0.45f) return Biome::Coast;

    // High elevation dominates.
    if (elev > 0.78f) return Biome::Highlands;

    // Cold dominates after elevation.
    if (temp < 0.22f) return Biome::Tundra;

    // Very dry.
    if (wet < 0.20f) {
        if (elev > 0.55f) return Biome::Badlands;
        return Biome::Desert;
    }

    // Very wet lowlands.
    if (wet > 0.74f && elev < 0.62f) return Biome::Swamp;

    // Moderately wet.
    if (wet > 0.55f) return Biome::Forest;

    return Biome::Plains;
}

struct ChunkProfile {
    int x = 0;
    int y = 0;
    uint32_t seed = 1u;         // chunk-local placement seed
    uint32_t nameSeed = 1u;     // chunk name seed
    uint32_t materialSeed = 1u; // per-chunk material palette seed
    Biome biome = Biome::Plains;
    int dangerDepth = 0;
};

inline ChunkProfile profileFor(uint32_t runSeed, int chunkX, int chunkY, int maxDepth) {
    ChunkProfile p;
    p.x = chunkX;
    p.y = chunkY;
    p.seed = chunkSeed(runSeed, chunkX, chunkY);
    p.nameSeed = nameSeed(runSeed, chunkX, chunkY);
    p.materialSeed = materialSeed(runSeed, chunkX, chunkY);
    p.biome = biomeFor(runSeed, chunkX, chunkY);
    p.dangerDepth = dangerDepthFor(chunkX, chunkY, maxDepth);
    return p;
}

inline std::string chunkNameFor(const ChunkProfile& p) {
    struct Bank {
        const char* const* adj = nullptr;
        int adjN = 0;
        const char* const* noun = nullptr;
        int nounN = 0;
    };

    auto bankFor = [](Biome b) -> Bank {
        // PLAINS
        static const char* const plainsAdj[] = {
            "WIDE", "OPEN", "GOLD", "WIND", "GREEN", "BRIGHT", "LONG", "SUN", "MEADOW", "HOLLOW"
        };
        static const char* const plainsNoun[] = {
            "FIELD", "MEADOW", "STEPPE", "PRAIRIE", "VALE", "HEATH", "DOWNS", "RIDGE", "BARROW", "PLAIN"
        };

        // FOREST
        static const char* const forestAdj[] = {
            "ASH", "BRIAR", "DARK", "FERN", "MOSS", "PINE", "RAVEN", "SILVER", "OLD", "THORN"
        };
        static const char* const forestNoun[] = {
            "WOOD", "GROVE", "THICKET", "GLADE", "COPSE", "CANOPY", "HOLLOW", "BOWER", "DELL", "WILDWOOD"
        };

        // SWAMP
        static const char* const swampAdj[] = {
            "BLACK", "MIRE", "FEN", "SUNKEN", "MURK", "REED", "SILT", "BRACKISH", "SOUR", "CROAK"
        };
        static const char* const swampNoun[] = {
            "MARSH", "FEN", "MIRE", "BAYOU", "DELTA", "SINK", "POOL", "SLOUGH", "QUAG", "WETLAND"
        };

        // DESERT
        static const char* const desertAdj[] = {
            "SALT", "DUST", "DRY", "EMBER", "PALE", "RED", "BARREN", "SCOUR", "SUN", "SAND"
        };
        static const char* const desertNoun[] = {
            "DUNES", "WASTES", "SANDS", "FLATS", "BASIN", "RIM", "HOLLOW", "SCAR", "PLATEAU", "SALTFLAT"
        };

        // TUNDRA
        static const char* const tundraAdj[] = {
            "FROST", "ICE", "WHITE", "COLD", "WINTER", "GRAY", "BLEAK", "RIME", "SNOW", "PALE"
        };
        static const char* const tundraNoun[] = {
            "TUNDRA", "MOOR", "DRIFTS", "WASTE", "RIDGE", "FIELDS", "STEPPE", "ICEFIELD", "BARRENS", "FJELL"
        };

        // HIGHLANDS
        static const char* const highAdj[] = {
            "HIGH", "IRON", "STONE", "CLOUD", "EAGLE", "STEEP", "RUGGED", "GRANITE", "SHEER", "CRAG"
        };
        static const char* const highNoun[] = {
            "RIDGE", "PEAK", "HEIGHTS", "CRAGS", "SLOPES", "SPINE", "RANGE", "SCARP", "SUMMIT", "HIGHLAND"
        };

        // BADLANDS
        static const char* const badAdj[] = {
            "BROKEN", "RUST", "JAGGED", "BONE", "SCAR", "HARSH", "SHATTER", "DRY", "IRON", "RED"
        };
        static const char* const badNoun[] = {
            "BADLANDS", "GULCH", "ARROYO", "CANYON", "RAVINES", "WASH", "CUTS", "SCREE", "MAZE", "SCRUB"
        };

        // COAST
        static const char* const coastAdj[] = {
            "SALT", "WAVE", "SEA", "FOAM", "MIST", "SHELL", "WIND", "GRAY", "TIDE", "HARBOR"
        };
        static const char* const coastNoun[] = {
            "SHORE", "COAST", "BAY", "COVE", "SANDS", "REEF", "HEADLAND", "TIDEFLAT", "STRAIT", "BEACH"
        };

        switch (b) {
            case Biome::Forest:    return {forestAdj, 10, forestNoun, 10};
            case Biome::Swamp:     return {swampAdj, 10, swampNoun, 10};
            case Biome::Desert:    return {desertAdj, 10, desertNoun, 10};
            case Biome::Tundra:    return {tundraAdj, 10, tundraNoun, 10};
            case Biome::Highlands: return {highAdj, 10, highNoun, 10};
            case Biome::Badlands:  return {badAdj, 10, badNoun, 10};
            case Biome::Coast:     return {coastAdj, 10, coastNoun, 10};
            case Biome::Plains:
            default:               return {plainsAdj, 10, plainsNoun, 10};
        }
    };

    RNG rng(p.nameSeed);
    const Bank b = bankFor(p.biome);

    const char* a = (b.adjN > 0) ? b.adj[rng.range(0, b.adjN - 1)] : "WILD";
    const char* n = (b.nounN > 0) ? b.noun[rng.range(0, b.nounN - 1)] : "LAND";

    // 45% chance of a fused name (ASHWOOD, SALTCOAST, etc), otherwise two words.
    std::string out;
    if (rng.chance(0.45f)) out = std::string(a) + std::string(n);
    else out = std::string(a) + " " + std::string(n);

    // Keep HUD-safe and avoid excessively long strings.
    if (out.size() > 32) out.resize(32);
    return out;
}

// -----------------------------------------------------------------------------
// Wilderness chunk generation
// -----------------------------------------------------------------------------

inline void generateWildernessChunk(Dungeon& d, uint32_t runSeed, int chunkX, int chunkY) {
    // Reset dungeon state.
    d.rooms.clear();
    d.heightfieldRidgePillarCount = 0;
    d.heightfieldScreeBoulderCount = 0;
    d.fluvialGullyCount = 0;
    d.fluvialChasmCount = 0;
    d.fluvialCausewayCount = 0;
    d.symmetryRoomCount = 0;
    d.symmetryObstacleCount = 0;

    if (d.width <= 2 || d.height <= 2) {
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                d.at(x, y).type = TileType::Floor;
        ensureBorderWalls(d);
        ensureBorderGates(d, runSeed, chunkX, chunkY);
        return;
    }

    // Chunk identity (biome+seed) is deterministic.
    const ChunkProfile prof = profileFor(runSeed, chunkX, chunkY, /*maxDepth=*/25);
    const Biome biome = prof.biome;

    // Base fill: floor.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            Tile& t = d.at(x, y);
            t.type = TileType::Floor;
            t.visible = false;
            t.explored = false;
        }
    }

    // Continuous terrain seeds (world-coordinate fields).
    const uint32_t base = terrainBaseSeed(runSeed);
    const uint32_t sElev = hashCombine(base, "ELEV"_tag);
    const uint32_t sWet  = hashCombine(base, "WET"_tag);
    const uint32_t sVar  = hashCombine(base, "VAR"_tag);

    // Biome-specific knobs.
    float mountainElevMin = 0.82f;
    float lakeElevMax     = 0.25f;
    float lakeWetMin      = 0.42f;

    float treeWetMin      = 0.66f;
    float treeElevMax     = 0.78f;
    float treeChance      = 0.28f;

    float screeElevMin    = 0.72f;
    float screeVarMax     = 0.050f;

    float deadwoodWetMax  = 0.24f;
    float deadwoodVarMax  = 0.015f;

    switch (biome) {
        case Biome::Forest:
            treeWetMin = 0.56f;
            treeChance = 0.46f;
            lakeElevMax = 0.23f;
            lakeWetMin  = 0.40f;
            mountainElevMin = 0.86f;
            break;
        case Biome::Swamp:
            lakeElevMax = 0.40f;
            lakeWetMin  = 0.35f;
            treeWetMin  = 0.58f;
            treeChance  = 0.35f;
            mountainElevMin = 0.88f;
            break;
        case Biome::Desert:
            lakeElevMax = 0.18f;
            lakeWetMin  = 0.70f;
            treeWetMin  = 0.90f;
            treeChance  = 0.10f;
            deadwoodWetMax = 0.55f;
            deadwoodVarMax = 0.025f;
            screeElevMin   = 0.66f;
            screeVarMax    = 0.090f;
            mountainElevMin = 0.80f;
            break;
        case Biome::Tundra:
            lakeElevMax = 0.22f;
            lakeWetMin  = 0.55f;
            treeWetMin  = 0.82f;
            treeChance  = 0.14f;
            deadwoodWetMax = 0.30f;
            deadwoodVarMax = 0.010f;
            screeElevMin   = 0.65f;
            screeVarMax    = 0.080f;
            mountainElevMin = 0.78f;
            break;
        case Biome::Highlands:
            lakeElevMax = 0.20f;
            lakeWetMin  = 0.55f;
            treeWetMin  = 0.78f;
            treeChance  = 0.18f;
            screeElevMin   = 0.60f;
            screeVarMax    = 0.100f;
            mountainElevMin = 0.74f;
            break;
        case Biome::Badlands:
            lakeElevMax = 0.16f;
            lakeWetMin  = 0.65f;
            treeWetMin  = 0.92f;
            treeChance  = 0.08f;
            deadwoodWetMax = 0.38f;
            deadwoodVarMax = 0.020f;
            screeElevMin   = 0.58f;
            screeVarMax    = 0.120f;
            mountainElevMin = 0.76f;
            break;
        case Biome::Coast:
            lakeElevMax = 0.30f;
            lakeWetMin  = 0.38f;
            treeWetMin  = 0.62f;
            treeChance  = 0.24f;
            mountainElevMin = 0.84f;
            break;
        case Biome::Plains:
        default:
            break;
    }

    // Cache per-tile noise fields so subsequent passes (rivers, landmarks, etc.)
    // can reuse them without recomputing multi-octave FBM.
    //
    // This improves chunk generation time and ensures the same elev/wet values are
    // referenced consistently across all wilderness procgen phases.
    const size_t fieldCount = static_cast<size_t>(d.width * d.height);
    std::vector<float> elevField(fieldCount, 0.0f);
    std::vector<float> wetField(fieldCount, 0.0f);
    std::vector<float> varField(fieldCount, 0.0f);

    auto idx = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y * d.width + x);
    };

    const int wx0 = chunkX * d.width;
    const int wy0 = chunkY * d.height;

    for (int y = 1; y < d.height - 1; ++y) {
        const int wy = wy0 + y;
        for (int x = 1; x < d.width - 1; ++x) {
            const int wx = wx0 + x;
            const size_t i = idx(x, y);
            elevField[i] = fbm01(sElev, wx * 0.013f, wy * 0.013f, 5);
            wetField[i]  = fbm01(sWet,  wx * 0.011f, wy * 0.011f, 4);
            varField[i]  = u32To01(hashCoord(sVar, wx, wy));
        }
    }

    // Generate terrain inside the chunk bounds (leave border for walls/gates).
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            const size_t i = idx(x, y);
            const float elev = elevField[i];
            const float wet  = wetField[i];
            const float var  = varField[i];

            TileType tt = TileType::Floor;

            // Mountains on high elevation.
            if (elev > mountainElevMin) {
                tt = TileType::Wall;
            }

            // Water basins in low elevation + wet.
            if (tt == TileType::Floor && elev < lakeElevMax && wet > lakeWetMin) {
                tt = TileType::Chasm;
                d.fluvialChasmCount++;
            }

            // Vegetation / trees.
            if (tt == TileType::Floor && wet > treeWetMin && elev < treeElevMax && var < treeChance) {
                tt = TileType::Pillar;
                d.heightfieldRidgePillarCount++;
            }

            // Scree / boulders at moderate-high elevations.
            if (tt == TileType::Floor && elev > screeElevMin && var < screeVarMax) {
                tt = TileType::Boulder;
                d.heightfieldScreeBoulderCount++;
            }

            // Deadwood: a few pillars in drier biomes.
            if (tt == TileType::Floor && wet < deadwoodWetMax && var < deadwoodVarMax) {
                tt = TileType::Pillar;
                d.heightfieldRidgePillarCount++;
            }

            d.at(x, y).type = tt;
        }
    }


    // ---------------------------------------------------------------------
    // Macro rivers: continuous chasm ribbons across world coordinates.
    // ---------------------------------------------------------------------
    // We carve thin "water" lines using a low-frequency noise band. Because the
    // noise is sampled in world-space, rivers remain continuous across chunk
    // borders. Trails are carved *after* this pass, guaranteeing that gates
    // remain mutually reachable even when a river cuts across the wilderness.
    {
        const uint32_t sRiv  = hashCombine(base, "RIV"_tag);
        const uint32_t sRivW = hashCombine(base, "RIVW"_tag);

        float bandBase = 0.012f;  // base half-width of the river band
        float wetBoost = 0.004f;  // extra widening in very wet areas
        float elevMin  = 0.20f;   // avoid deep basins (handled as lakes) and extreme flats
        float elevMax  = std::max(0.0f, mountainElevMin - 0.05f); // don't cut mountains by default

        switch (biome) {
            case Biome::Swamp:
                bandBase = 0.020f;
                wetBoost = 0.010f;
                elevMin  = 0.18f;
                break;
            case Biome::Coast:
                bandBase = 0.018f;
                wetBoost = 0.008f;
                elevMin  = 0.16f;
                break;
            case Biome::Forest:
                bandBase = 0.014f;
                wetBoost = 0.006f;
                elevMin  = 0.18f;
                break;
            case Biome::Plains:
                bandBase = 0.012f;
                wetBoost = 0.004f;
                elevMin  = 0.20f;
                break;
            case Biome::Tundra:
                bandBase = 0.011f;
                wetBoost = 0.004f;
                elevMin  = 0.20f;
                break;
            case Biome::Highlands:
                bandBase = 0.009f;
                wetBoost = 0.002f;
                elevMin  = 0.24f;
                break;
            case Biome::Badlands:
                bandBase = 0.008f;
                wetBoost = 0.001f;
                elevMin  = 0.24f;
                break;
            case Biome::Desert:
                bandBase = 0.006f;  // rare wadis
                wetBoost = 0.000f;
                elevMin  = 0.26f;
                break;
            default:
                break;
        }

        for (int y = 1; y < d.height - 1; ++y) {
            const int wy = wy0 + y;
            for (int x = 1; x < d.width - 1; ++x) {
                if (d.at(x, y).type == TileType::Wall) continue;

                const size_t i = idx(x, y);
                const float elev = elevField[i];
                if (elev < elevMin || elev > elevMax) continue;

                const int wx = wx0 + x;
                const float wet  = wetField[i];

                // Low-frequency line noise: thin ribbons near the 0.5 isovalue.
                const float n = fbm01(sRiv,  wx * 0.0062f, wy * 0.0062f, 3);
                const float w = fbm01(sRivW, wx * 0.0190f, wy * 0.0190f, 2);

                float band = bandBase * (0.70f + 0.80f * w);
                band += std::max(0.0f, wet - 0.55f) * wetBoost;

                if (std::abs(n - 0.5f) < band) {
                    if (d.at(x, y).type != TileType::Chasm) d.fluvialChasmCount++;
                    d.at(x, y).type = TileType::Chasm;
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // Biome landmarks (lightweight, deterministic)
    // ---------------------------------------------------------------------
    // Place 0..2 landmarks before carving trails so connectivity is guaranteed.
    const ChunkGates gates = gatePositions(d, runSeed, chunkX, chunkY);
    RNG landRng(hashCombine(prof.seed, "OW_LAND"_tag));

    auto farFromGates = [&](int x, int y) -> bool {
        const int dN = std::abs(x - gates.north.x) + std::abs(y - (gates.north.y + 1));
        const int dS = std::abs(x - gates.south.x) + std::abs(y - (gates.south.y - 1));
        const int dW = std::abs(x - (gates.west.x + 1)) + std::abs(y - gates.west.y);
        const int dE = std::abs(x - (gates.east.x - 1)) + std::abs(y - gates.east.y);
        const int m = std::min(std::min(dN, dS), std::min(dW, dE));
        return m >= 7;
    };

    auto pickLandmarkCenter = [&]() -> Vec2i {
        for (int i = 0; i < 64; ++i) {
            const int x = landRng.range(3, d.width - 4);
            const int y = landRng.range(3, d.height - 4);
            if (!farFromGates(x, y)) continue;
            if (!d.isWalkable(x, y)) continue;
            return {x, y};
        }
        return {d.width / 2, d.height / 2};
    };

    auto carveDisk = [&](Vec2i c, int rx, int ry, TileType tt) {
        rx = std::max(1, rx);
        ry = std::max(1, ry);
        for (int yy = c.y - ry; yy <= c.y + ry; ++yy) {
            for (int xx = c.x - rx; xx <= c.x + rx; ++xx) {
                if (!d.inBounds(xx, yy)) continue;
                if (xx <= 0 || yy <= 0 || xx >= d.width - 1 || yy >= d.height - 1) continue;
                const float dx = (static_cast<float>(xx - c.x) / static_cast<float>(rx));
                const float dy = (static_cast<float>(yy - c.y) / static_cast<float>(ry));
                if (dx * dx + dy * dy <= 1.0f) {
                    if (d.at(xx, yy).type != tt) {
                        if (tt == TileType::Chasm) d.fluvialChasmCount++;
                    }
                    d.at(xx, yy).type = tt;
                }
            }
        }
    };

    auto placeRuins = [&](Vec2i c) {
        const int w = landRng.range(5, 9);
        const int h = landRng.range(5, 9);
        const int x0 = std::clamp(c.x - w / 2, 2, d.width - w - 2);
        const int y0 = std::clamp(c.y - h / 2, 2, d.height - h - 2);
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                const bool border = (x == x0 || y == y0 || x == x0 + w - 1 || y == y0 + h - 1);
                d.at(x, y).type = border ? TileType::Wall : TileType::Floor;
            }
        }
        // A few collapsed boulders.
        const int rubble = landRng.range(1, 3);
        for (int i = 0; i < rubble; ++i) {
            const int rx = landRng.range(x0 + 1, x0 + w - 2);
            const int ry = landRng.range(y0 + 1, y0 + h - 2);
            d.at(rx, ry).type = TileType::Boulder;
            d.heightfieldScreeBoulderCount++;
        }
    };

    auto placeStoneCircle = [&](Vec2i c) {
        const int r = landRng.range(2, 3);
        for (int i = 0; i < 24; ++i) {
            const float a = (static_cast<float>(i) / 24.0f) * 6.2831853f;
            const int x = c.x + static_cast<int>(std::round(std::cos(a) * r));
            const int y = c.y + static_cast<int>(std::round(std::sin(a) * r));
            if (!d.inBounds(x, y)) continue;
            if (x <= 0 || y <= 0 || x >= d.width - 1 || y >= d.height - 1) continue;
            d.at(x, y).type = TileType::Boulder;
            d.heightfieldScreeBoulderCount++;
        }
    };

    auto placeGrove = [&](Vec2i c) {
        const int r = landRng.range(2, 4);
        for (int y = c.y - r; y <= c.y + r; ++y) {
            for (int x = c.x - r; x <= c.x + r; ++x) {
                if (!d.inBounds(x, y)) continue;
                if (x <= 0 || y <= 0 || x >= d.width - 1 || y >= d.height - 1) continue;
                const int md = std::abs(x - c.x) + std::abs(y - c.y);
                if (md > r) continue;
                if (landRng.chance(0.35f)) {
                    d.at(x, y).type = TileType::Pillar;
                    d.heightfieldRidgePillarCount++;
                }
            }
        }
    };

    int landmarkCount = 0;
    if (landRng.chance(0.55f)) landmarkCount = 1;
    if (landRng.chance(0.18f)) landmarkCount += 1;

    for (int i = 0; i < landmarkCount; ++i) {
        const Vec2i c = pickLandmarkCenter();

        // Choose a landmark type biased by biome.
        const uint32_t roll = landRng.nextU32() % 100u;

        if (biome == Biome::Desert) {
            if (roll < 65u) carveDisk(c, landRng.range(2, 4), landRng.range(2, 4), TileType::Chasm); // oasis
            else placeRuins(c);
        } else if (biome == Biome::Swamp || biome == Biome::Coast) {
            if (roll < 70u) carveDisk(c, landRng.range(2, 5), landRng.range(2, 5), TileType::Chasm);
            else placeRuins(c);
        } else if (biome == Biome::Forest) {
            if (roll < 55u) placeGrove(c);
            else placeRuins(c);
        } else if (biome == Biome::Highlands || biome == Biome::Badlands) {
            if (roll < 55u) placeRuins(c);
            else placeStoneCircle(c);
        } else if (biome == Biome::Tundra) {
            if (roll < 45u) placeStoneCircle(c);
            else placeRuins(c);
        } else {
            // Plains fallback.
            if (roll < 40u) carveDisk(c, landRng.range(2, 4), landRng.range(2, 4), TileType::Chasm);
            else if (roll < 75u) placeRuins(c);
            else placeGrove(c);
        }
    }

    // ---------------------------------------------------------------------
    // Organic trail network: connect all gates to a jittered hub.
    // ---------------------------------------------------------------------
    RNG trailRng(hashCombine(prof.seed, "OW_TRAIL"_tag));
    Vec2i hub{d.width / 2, d.height / 2};
    hub.x += trailRng.range(-d.width / 6, d.width / 6);
    hub.y += trailRng.range(-d.height / 6, d.height / 6);
    hub.x = std::clamp(hub.x, 2, d.width - 3);
    hub.y = std::clamp(hub.y, 2, d.height - 3);

    const int trailRadius = (biome == Biome::Plains || biome == Biome::Coast || biome == Biome::Swamp) ? 1 : 0;

    auto carveTrailAt = [&](int x, int y) {
        for (int dy = -trailRadius; dy <= trailRadius; ++dy) {
            for (int dx = -trailRadius; dx <= trailRadius; ++dx) {
                const int xx = x + dx;
                const int yy = y + dy;
                if (!d.inBounds(xx, yy)) continue;
                if (xx <= 0 || yy <= 0 || xx >= d.width - 1 || yy >= d.height - 1) continue;
                d.at(xx, yy).type = TileType::Floor;
            }
        }
    };

    // Small clearing at the hub.
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            carveTrailAt(hub.x + dx, hub.y + dy);

    auto walkMeander = [&](Vec2i start) {
        Vec2i p = start;
        p.x = std::clamp(p.x, 1, d.width - 2);
        p.y = std::clamp(p.y, 1, d.height - 2);

        const int maxSteps = d.width * d.height * 2;
        for (int i = 0; i < maxSteps; ++i) {
            carveTrailAt(p.x, p.y);
            if (p.x == hub.x && p.y == hub.y) break;

            const int dx = hub.x - p.x;
            const int dy = hub.y - p.y;

            bool stepX = false;
            if (dx == 0) stepX = false;
            else if (dy == 0) stepX = true;
            else {
                const bool preferX = (std::abs(dx) >= std::abs(dy));
                stepX = preferX;
                if (trailRng.chance(0.18f)) stepX = !stepX;
                if (trailRng.chance(0.10f)) stepX = trailRng.chance(0.5f);
            }

            Vec2i n = p;
            if (stepX) n.x += (dx > 0) ? 1 : -1;
            else       n.y += (dy > 0) ? 1 : -1;

            if (n.x <= 0 || n.y <= 0 || n.x >= d.width - 1 || n.y >= d.height - 1) break;
            p = n;
        }
    };

    const Vec2i startN{gates.north.x, gates.north.y + 1};
    const Vec2i startS{gates.south.x, gates.south.y - 1};
    const Vec2i startW{gates.west.x + 1, gates.west.y};
    const Vec2i startE{gates.east.x - 1, gates.east.y};

    walkMeander(startN);
    walkMeander(startS);
    walkMeander(startW);
    walkMeander(startE);

    // ---------------------------------------------------------------------
    // Procedural overworld waystations (traveling merchant caravans)
    // ---------------------------------------------------------------------
    // These are small shop rooms embedded in the wilderness, using the existing
    // dungeon shop system (RoomType::Shop) for stocking and shopkeeper behavior.
    //
    // Design:
    //  * Deterministic per chunk (runSeed + coords)
    //  * Biome-aware frequency (more common on plains/coasts; rarer in tundra/desert)
    //  * Physically connected: carved approach trail meanders back to the hub
    //  * Spawn-safe: normal overworld spawns avoid Shop room tiles (see Game::randomFreeTileInRoom)

    bool hasWaystation = false;
    Room waystation;

    auto waystationChance = [&](Biome b, int dangerDepth) -> float {
        float c = 0.05f; // baseline
        switch (b) {
            case Biome::Plains:    c = 0.10f; break;
            case Biome::Coast:     c = 0.08f; break;
            case Biome::Highlands: c = 0.07f; break;
            case Biome::Forest:    c = 0.055f; break;
            case Biome::Swamp:     c = 0.045f; break;
            case Biome::Badlands:  c = 0.035f; break;
            case Biome::Tundra:    c = 0.030f; break;
            case Biome::Desert:    c = 0.025f; break;
        }

        // The farthest chunks are more dangerous; caravans thin out.
        if (dangerDepth >= 10) c *= 0.70f;
        if (dangerDepth >= 16) c *= 0.55f;
        // A tiny boost to make early exploration feel alive.
        if (dangerDepth <= 3) c *= 1.10f;

        return std::clamp(c, 0.0f, 0.14f);
    };

    {
        RNG stationRng(hashCombine(prof.seed, "OW_STATION"_tag));
        const float chance = waystationChance(biome, prof.dangerDepth);

        // Keep waystations relatively rare so each one feels like a discovery.
        if (chance > 0.0f && stationRng.chance(chance)) {
            const int rw = stationRng.range(7, 11);
            const int rh = stationRng.range(6, 9);

            auto farFromChunkGates = [&](Vec2i c) {
                const int minD = 10;
                auto md = [&](Vec2i a, Vec2i b) { return std::abs(a.x - b.x) + std::abs(a.y - b.y); };
                if (md(c, gates.north) < minD) return false;
                if (md(c, gates.south) < minD) return false;
                if (md(c, gates.west) < minD) return false;
                if (md(c, gates.east) < minD) return false;
                return true;
            };

            auto rectContains = [&](int x0, int y0, int w0, int h0, Vec2i p) {
                return p.x >= x0 && p.y >= y0 && p.x < x0 + w0 && p.y < y0 + h0;
            };

            for (int attempt = 0; attempt < 48 && !hasWaystation; ++attempt) {
                // Bias toward being "near a trail" by placing relative to the hub.
                Vec2i c = hub;
                const int dist = stationRng.range(10, 22);
                const int dir = stationRng.range(0, 3);
                if (dir == 0) c.x += dist;
                if (dir == 1) c.x -= dist;
                if (dir == 2) c.y += dist;
                if (dir == 3) c.y -= dist;

                c.x += stationRng.range(-4, 4);
                c.y += stationRng.range(-4, 4);
                c.x = std::clamp(c.x, 3 + rw / 2, d.width - 4 - rw / 2);
                c.y = std::clamp(c.y, 3 + rh / 2, d.height - 4 - rh / 2);

                if (!farFromChunkGates(c)) continue;

                const int x0 = c.x - rw / 2;
                const int y0 = c.y - rh / 2;
                if (x0 <= 2 || y0 <= 2 || x0 + rw >= d.width - 2 || y0 + rh >= d.height - 2) continue;

                // Avoid building directly on top of the hub clearing.
                if (rectContains(x0, y0, rw, rh, hub)) continue;

                // Validate that the footprint is mostly reasonable terrain.
                int bad = 0;
                int mountain = 0;
                for (int y = y0; y < y0 + rh; ++y) {
                    for (int x = x0; x < x0 + rw; ++x) {
                        const TileType t = d.at(x, y).type;
                        if (t == TileType::Chasm) bad += 3;
                        if (t == TileType::Wall) mountain += 1;
                    }
                }

                // Don't pave over rivers/lakes.
                if (bad > 0) continue;
                // Don't carve a shop *through* a mountain ridge.
                if (mountain > (rw * rh) / 4) continue;

                // Carve the waystation structure.
                for (int y = y0; y < y0 + rh; ++y) {
                    for (int x = x0; x < x0 + rw; ++x) {
                        const bool border = (x == x0 || y == y0 || x == x0 + rw - 1 || y == y0 + rh - 1);
                        d.at(x, y).type = border ? TileType::Wall : TileType::Floor;
                    }
                }

                // Door on the side facing the hub.
                const Vec2i center{c.x, c.y};
                const int dx = hub.x - center.x;
                const int dy = hub.y - center.y;
                Vec2i door{center.x, center.y};
                Vec2i out{0, 0};

                if (std::abs(dx) >= std::abs(dy)) {
                    if (dx < 0) { // hub is left
                        door.x = x0;
                        door.y = y0 + rh / 2;
                        out = {-1, 0};
                    } else {
                        door.x = x0 + rw - 1;
                        door.y = y0 + rh / 2;
                        out = {1, 0};
                    }
                } else {
                    if (dy < 0) { // hub is up
                        door.x = x0 + rw / 2;
                        door.y = y0;
                        out = {0, -1};
                    } else {
                        door.x = x0 + rw / 2;
                        door.y = y0 + rh - 1;
                        out = {0, 1};
                    }
                }

                // Ensure the door tile is not on the chunk border.
                if (door.x <= 1 || door.y <= 1 || door.x >= d.width - 2 || door.y >= d.height - 2) continue;

                d.at(door.x, door.y).type = TileType::DoorOpen;
                Vec2i approach{door.x + out.x, door.y + out.y};
                if (d.inBounds(approach.x, approach.y)) {
                    carveTrailAt(approach.x, approach.y);
                    // Connect to the hub via the same meander style as gate trails.
                    walkMeander(approach);
                }

                waystation.x = x0;
                waystation.y = y0;
                waystation.w = rw;
                waystation.h = rh;
                waystation.type = RoomType::Shop;
                hasWaystation = true;
            }
        }
    }

    // Finalize border walls + gates.
    ensureBorderWalls(d);
    ensureBorderGates(d, runSeed, chunkX, chunkY);

    // Single large "room" covering most of the chunk for spawn logic.
    // NOTE: If we spawned special sub-rooms (shops, shrines, etc.), they must be
    // pushed *before* this catch-all room so roomTypeAt() returns the special
    // room type for their tiles.
    if (hasWaystation) {
        d.rooms.push_back(waystation);
    }

    Room r;
    r.x = 1;
    r.y = 1;
    r.w = d.width - 2;
    r.h = d.height - 2;
    r.type = RoomType::Normal;
    d.rooms.push_back(r);
}

} // namespace overworld
