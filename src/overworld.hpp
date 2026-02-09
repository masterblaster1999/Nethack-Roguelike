#pragma once

#include "dungeon.hpp"
#include "rng.hpp"
#include "poisson_disc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <queue>
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
// Tectonics: Voronoi plates + ridge lines (seam-free world-space field)
// -----------------------------------------------------------------------------
//
// We use a lightweight Worley/Voronoi (F1/F2) field to create *macro-scale*
// tectonic "plates":
//   - A per-plate base elevation offset (continental vs basin-like plates).
//   - A ridge mask near plate boundaries (mountain chains / fault ridges).
//
// This is evaluated in *world tile coordinates* so the result is seamless across
// chunk borders and can be reused by multiple overworld passes (terrain, climate,
// rivers, etc.).

struct WorleyF1F2 {
    float f1 = 0.0f; // distance to nearest feature point (cell units)
    float f2 = 0.0f; // distance to second nearest feature point (cell units)
    int cellX = 0;   // cell coord of the nearest feature point
    int cellY = 0;
};

inline WorleyF1F2 worleyF1F2(uint32_t seed, int wx, int wy, int cellSize) {
    // Work in "cell units" so the distribution is scale-invariant.
    const double inv = 1.0 / static_cast<double>(std::max(1, cellSize));
    const double fx = static_cast<double>(wx) * inv;
    const double fy = static_cast<double>(wy) * inv;

    const int ix = static_cast<int>(std::floor(fx));
    const int iy = static_cast<int>(std::floor(fy));

    const float lx = static_cast<float>(fx - static_cast<double>(ix));
    const float ly = static_cast<float>(fy - static_cast<double>(iy));

    float best1 = std::numeric_limits<float>::infinity();
    float best2 = std::numeric_limits<float>::infinity();
    int bestCellX = ix;
    int bestCellY = iy;

    // Search neighboring cells (nearest point may lie across a cell border).
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = ix + dx;
            const int cy = iy + dy;

            // Feature point inside the cell (stable per cell).
            const float px = u32To01(hashCoord(seed, cx, cy));
            const float py = u32To01(hashCoord(seed ^ 0xB5297A4Du, cx, cy));

            const float vx = static_cast<float>(dx) + px - lx;
            const float vy = static_cast<float>(dy) + py - ly;
            const float dist2 = vx * vx + vy * vy;

            if (dist2 < best1) {
                best2 = best1;
                best1 = dist2;
                bestCellX = cx;
                bestCellY = cy;
            } else if (dist2 < best2) {
                best2 = dist2;
            }
        }
    }

    WorleyF1F2 r;
    r.f1 = std::sqrt(best1);
    r.f2 = std::sqrt(best2);
    r.cellX = bestCellX;
    r.cellY = bestCellY;
    return r;
}

inline float worleyBoundary01(float f1, float f2, float width0, float width1) {
    // Smaller (F2-F1) => closer to the Voronoi boundary.
    const float diff = std::max(0.0f, f2 - f1);
    const float denom = std::max(1e-6f, width1 - width0);
    const float t = std::clamp((diff - width0) / denom, 0.0f, 1.0f);
    // Ridge mask: 1 at boundary, 0 far away.
    return 1.0f - smoothstep(t);
}

struct TectonicSample {
    float ridge = 0.0f;       // 0..1 ridge intensity (plate boundary proximity)
    float plateOffset = 0.0f; // additive elevation offset (tiles use this to bias plate interiors)
};

inline TectonicSample sampleTectonics(uint32_t terrainSeed, int wx, int wy) {
    const uint32_t sMajor = hashCombine(terrainSeed, "TECT_MAJ"_tag);
    const uint32_t sMinor = hashCombine(terrainSeed, "TECT_MIN"_tag);
    const uint32_t sPlate = hashCombine(terrainSeed, "TECT_PLATE"_tag);
    const uint32_t sVar   = hashCombine(terrainSeed, "TECT_VAR"_tag);
    const uint32_t sPass  = hashCombine(terrainSeed, "TECT_PASS"_tag);

    // Two scales: major plates (big, readable mountain chains) + minor sub-ridges.
    const int majorCell = 360; // world tiles
    const int minorCell = 170;

    WorleyF1F2 maj = worleyF1F2(sMajor, wx, wy, majorCell);
    WorleyF1F2 min = worleyF1F2(sMinor, wx, wy, minorCell);

    float rMaj = worleyBoundary01(maj.f1, maj.f2, 0.045f, 0.120f);
    float rMin = worleyBoundary01(min.f1, min.f2, 0.050f, 0.135f);

    // Deterministic gaps/passes along major boundaries (prevents infinite unbroken walls).
    const float pass = fbm01(sPass, wx * 0.0028f, wy * 0.0028f, 3);
    if (pass < 0.16f) rMaj *= 0.22f;

    // Ridge amplitude variation (avoids uniform stripes).
    const float v1 = fbm01(sVar, wx * 0.0035f, wy * 0.0035f, 3);
    const float v2 = fbm01(sVar ^ 0x9E3779B9u, wx * 0.0075f, wy * 0.0075f, 2);

    float ridge = rMaj * (0.75f + 0.45f * v1) + rMin * (0.35f + 0.35f * v2);
    ridge = std::clamp(ridge, 0.0f, 1.0f);

    // Plate base elevation offset from the major nearest-cell id.
    // Map to roughly [-0.055, +0.055].
    const float plateU = u32To01(hashCoord(sPlate, maj.cellX, maj.cellY));
    const float plateOffset = (plateU - 0.5f) * 0.11f;

    TectonicSample out;
    out.ridge = ridge;
    out.plateOffset = plateOffset;
    return out;
}

inline float tectonicRidge01(uint32_t runSeed, int wx, int wy) {
    return sampleTectonics(terrainBaseSeed(runSeed), wx, wy).ridge;
}

inline float tectonicPlateElevOffset(uint32_t runSeed, int wx, int wy) {
    return sampleTectonics(terrainBaseSeed(runSeed), wx, wy).plateOffset;
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
// Overworld weather (deterministic per chunk, time-varying fronts)
// -----------------------------------------------------------------------------
//
// Wilderness chunks expose a lightweight weather profile derived deterministically
// from (runSeed, chunkX, chunkY) and (optionally) turnCount. This is intentionally *not* a full time simulation;
// it is a per-region "climate" snapshot with slowly drifting wind/cloud fronts that:
//   - provides coherent wind for scent/gas/fire drift on the overworld,
//   - occasionally reduces visibility (fog/snow/dust), and
//   - can quench fire during rain/storms.
//
// The profile is cheap to compute, does not consume gameplay RNG, and needs no save-file state.

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

// Time-varying weather fronts
//
// Rather than simulating per-tile weather (which would require save-file state and
// additional per-turn processing), we "animate" the large-scale cloud/front and wind
// noise fields by drifting their sampling coordinates as a pure function of
// (runSeed, turnCount).
//
// This keeps the system:
//   - deterministic (replay-safe),
//   - coherent across neighboring chunks,
//   - cheap to evaluate on demand, and
//   - fully backwards compatible: turnCount==0 reproduces the old static snapshot.

inline Vec2f driftDir8(uint32_t h) {
    constexpr float d = 0.70710678f; // 1/sqrt(2)
    switch (h % 8u) {
        case 0: return { 1.0f, 0.0f };
        case 1: return { -1.0f, 0.0f };
        case 2: return { 0.0f, 1.0f };
        case 3: return { 0.0f, -1.0f };
        case 4: return { d, d };
        case 5: return { -d, d };
        case 6: return { d, -d };
        default: return { -d, -d };
    }
}

inline Vec2f weatherDrift(uint32_t runSeed, uint32_t domainTag, uint32_t turnCount, float turnsPerUnit, float driftPerUnit) {
    if (turnCount == 0u) return {0.0f, 0.0f};
    if (turnsPerUnit <= 0.0f) return {0.0f, 0.0f};

    const uint32_t base = hashCombine(runSeed, "OW_WEATHER_DRIFT"_tag);
    const uint32_t h = hashCombine(base, domainTag);

    const Vec2f dir = driftDir8(h);

    // Small per-run/per-domain speed variation to keep fronts from feeling too clockwork.
    const float jitter = 0.75f + 0.50f * rand01(hashCombine(h, "SPEED"_tag));
    const float t = static_cast<float>(turnCount) / turnsPerUnit;

    return { dir.x * t * driftPerUnit * jitter, dir.y * t * driftPerUnit * jitter };
}


inline WeatherProfile weatherFor(uint32_t runSeed, int chunkX, int chunkY, Biome biome, uint32_t turnCount = 0u) {
    WeatherProfile w;

    const uint32_t base = hashCombine(runSeed, "OW_WEATHER"_tag);

    // Time-varying drift (fronts): animate the wind + cloud fields with turnCount.
    // Using different drift domains keeps wind bands and cloud fronts loosely coupled.
    const Vec2f windDrift  = weatherDrift(runSeed, "WX_WIND"_tag,  turnCount, 650.0f, 0.12f);
    const Vec2f cloudDrift = weatherDrift(runSeed, "WX_FRONT"_tag, turnCount, 900.0f, 0.16f);


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
    const float wfx = static_cast<float>(chunkX) * 0.17f + windDrift.x;
    const float wfy = static_cast<float>(chunkY) * 0.17f + windDrift.y;

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

    const float cfx = fx + cloudDrift.x;
    const float cfy = fy + cloudDrift.y;
    const float cloud = fbm01(sCloud, cfx + 91.0f, cfy - 37.0f, 3);

    const uint32_t sFront = hashCombine(base, "FRONT"_tag);
    const float front = fbm01(sFront, cfx - 13.0f, cfy + 77.0f, 3);

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

// -----------------------------------------------------------------------------
// Chunk climate: a cheap rain-shadow / orographic uplift model for biome picking
// -----------------------------------------------------------------------------
//
// Biomes are per-chunk identity, so we want a stable *large scale* climate signal.
// We start from 3 low-frequency FBM fields (elevation / wetness / temperature),
// then apply a small deterministic wind-aligned correction:
//   * windward rises get slightly wetter (uplift/condensation),
//   * leeward basins behind nearby ridges get drier (rain shadow),
//   * proximity to low-elevation wet "ocean" tiles upwind boosts moisture.
//
// The goal is not realism; it's to generate coherent, readable biome regions:
// forests on windward slopes, badlands/deserts in mountain rain shadows, etc.
inline Vec2i climateWindDir(uint32_t runSeed) {
    const uint32_t s = hashCombine(runSeed, "OW_CLIMATE_WIND"_tag);
    const uint32_t r = (s ^ (s >> 16)) % 4u;
    switch (r) {
        case 0:  return { 1,  0}; // westerlies -> blow east
        case 1:  return {-1,  0}; // easterlies -> blow west
        case 2:  return { 0,  1}; // blow south
        default: return { 0, -1}; // blow north
    }
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

    // Altitude lapse-rate bias: higher regions are colder.
    temp = std::clamp(temp - elev * 0.22f, 0.0f, 1.0f);

    // Wind-aligned moisture correction (rain shadow / uplift).
    const Vec2i wdir = climateWindDir(runSeed);

    const int steps = 5; // chunk-scale upwind lookback
    float maxUpElev = elev;
    float prevUpElev = elev;
    float oceanProx = 0.0f;

    for (int k = 1; k <= steps; ++k) {
        const int ux = chunkX - wdir.x * k;
        const int uy = chunkY - wdir.y * k;

        const float fux = static_cast<float>(ux) * 0.23f;
        const float fuy = static_cast<float>(uy) * 0.23f;

        const float eUp = fbm01(sElev, fux, fuy, 4);
        const float wUp = fbm01(sWet,  fux + 17.0f, fuy - 29.0f, 4);

        if (k == 1) prevUpElev = eUp;
        maxUpElev = std::max(maxUpElev, eUp);

        // Treat very low elevation + above-average wetness upwind as an "ocean"
        // moisture source and decay with distance.
        float oceanHere = 0.0f;
        if (eUp < 0.30f && wUp > 0.45f) {
            oceanHere = std::clamp((0.30f - eUp) / 0.30f, 0.0f, 1.0f);
        }
        const float falloff = 1.0f - (static_cast<float>(k - 1) / static_cast<float>(steps));
        oceanProx = std::max(oceanProx, oceanHere * falloff);
    }

    const float rise = std::max(0.0f, elev - prevUpElev);
    const float drop = std::max(0.0f, prevUpElev - elev);
    float shadow = std::max(0.0f, maxUpElev - elev - 0.03f);

    // Only strong ridges cast strong rain shadows.
    const float ridge = std::clamp((maxUpElev - 0.60f) * 2.2f, 0.0f, 1.0f);
    shadow *= ridge;

    wet += oceanProx * 0.12f;
    wet += rise * 0.55f * (0.35f + 0.65f * ridge);
    wet -= shadow * 0.65f * (0.35f + 0.65f * ridge);
    wet -= drop * 0.20f * ridge;
    wet = std::clamp(wet, 0.0f, 1.0f);

    // Ocean proximity slightly moderates temperature (coasts are less extreme).
    temp = std::clamp(temp + oceanProx * 0.05f, 0.0f, 1.0f);

    // Lowlands + enough moisture + ocean influence => coastal.
    if (elev < 0.30f && wet > 0.42f && oceanProx > 0.10f) return Biome::Coast;

    // High elevation dominates.
    if (elev > 0.78f) return Biome::Highlands;

    // Cold dominates after elevation.
    if (temp < 0.22f) return Biome::Tundra;

    // Very dry.
    if (wet < 0.20f) {
        if (elev > 0.55f || shadow > 0.06f) return Biome::Badlands;
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
    // Overworld generation may request bonus spawns (chest caches, fixed items).
    // These are generation-only and should never carry across chunk instances.
    d.bonusLootSpots.clear();
    d.bonusItemSpawns.clear();
    d.heightfieldRidgePillarCount = 0;
    d.heightfieldScreeBoulderCount = 0;
    d.fluvialGullyCount = 0;
    d.fluvialChasmCount = 0;
    d.fluvialCausewayCount = 0;
    d.dungeonSeepSpringCount = 0;
    d.dungeonSeepFountainTiles = 0;
    d.dungeonConfluenceCount = 0;
    d.dungeonConfluenceFountainTiles = 0;
    d.overworldSpringCount = 0;
    d.overworldBrookCount = 0;
    d.overworldBrookTiles = 0;
    d.overworldPondCount = 0;
    d.overworldStrongholdCount = 0;
    d.overworldStrongholdBuildingCount = 0;
    d.overworldStrongholdCacheCount = 0;
    d.overworldStrongholdWallTiles = 0;
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
    std::vector<float> ridgeField(fieldCount, 0.0f);

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

    
    // ---------------------------------------------------------------------
    // Climate wetness advection (orographic uplift + rain shadow)
    // ---------------------------------------------------------------------
    // The base wetness field is pure world-coordinate FBM noise. To make
    // large-scale biomes/terrain feel more coherent (windward forests, lee-side
    // dry basins), we apply a cheap deterministic correction aligned with a
    // per-run prevailing wind:
    //   - rising terrain *into* the wind boosts wetness (uplift/condensation),
    //   - terrain in the lee of a nearby ridge becomes drier (rain shadow).
    //
    // The correction depends only on world-space elevation samples, so it stays
    // seam-free across chunk borders.
    {
        const Vec2i wdir = climateWindDir(runSeed);

        // Lookback window in tiles along the upwind direction.
        const int win = std::clamp(std::min(d.width, d.height) / 4, 8, 16);

        // Tuning knobs (small nudges; base noise still dominates).
        const float shadowSlack = 0.018f;
        const float ridgeStart  = 0.58f;

        const float upliftGain  = 0.75f;
        const float shadowGain  = 0.60f;
        const float leeGain     = 0.18f;

        auto elevWorld = [&](int lx, int ly) -> float {
            const int wx = wx0 + lx;
            const int wy = wy0 + ly;
            return fbm01(sElev, wx * 0.013f, wy * 0.013f, 5);
        };

        auto elevAt = [&](int lx, int ly) -> float {
            if (lx >= 1 && ly >= 1 && lx <= d.width - 2 && ly <= d.height - 2) {
                const size_t ii = idx(lx, ly);
                if (ii < elevField.size()) return elevField[ii];
            }
            return elevWorld(lx, ly);
        };

        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                const size_t i = idx(x, y);
                if (i >= wetField.size()) continue;

                const float e = elevField[i];

                // Upwind neighbor for rise/drop.
                const float ePrev = elevAt(x - wdir.x, y - wdir.y);

                float barrier = e;
                for (int k = 1; k <= win; ++k) {
                    barrier = std::max(barrier, elevAt(x - wdir.x * k, y - wdir.y * k));
                }

                // Only strong ridges cast strong shadows.
                const float ridge = std::clamp((barrier - ridgeStart) * 2.2f, 0.0f, 1.0f);

                const float rise = std::max(0.0f, e - ePrev);
                const float drop = std::max(0.0f, ePrev - e);

                float shadow = std::max(0.0f, barrier - e - shadowSlack);
                shadow *= ridge;

                float w = wetField[i];
                w += rise * upliftGain * (0.35f + 0.65f * ridge);
                w -= shadow * shadowGain * (0.35f + 0.65f * ridge);
                w -= drop * leeGain * ridge;

                wetField[i] = std::clamp(w, 0.0f, 1.0f);
            }
        }
    }


    // ---------------------------------------------------------------------
    // Tectonic plates: plate offsets + ridge-line amplification (seam-free)
    // ---------------------------------------------------------------------
    // After climate wetness advection we apply a deterministic *tectonics* field
    // in world space:
    //   - plate interiors get a small elevation offset (continents vs basins),
    //   - plate boundaries lift into coherent ridge lines (mountain chains),
    //   - ridges are slightly dried (rocky/exposed crests).
    //
    // This is evaluated in world tile coordinates, so it stays seamless across
    // chunk borders.
    {
        const uint32_t terrainSeed = base;
        for (int y = 1; y < d.height - 1; ++y) {
            const int wy = wy0 + y;
            (void)wy;
            for (int x = 1; x < d.width - 1; ++x) {
                const int wx = wx0 + x;
                const size_t i = idx(x, y);
                if (i >= elevField.size() || i >= wetField.size() || i >= ridgeField.size()) continue;

                const TectonicSample ts = sampleTectonics(terrainSeed, wx, wy0 + y);
                const float ridge = ts.ridge;

                ridgeField[i] = ridge;

                // Elevation: plate bias + ridge lift.
                float e = elevField[i];
                const float ridgeLift = std::pow(ridge, 1.35f) * 0.22f;
                e = std::clamp(e + ts.plateOffset + ridgeLift, 0.0f, 1.0f);
                elevField[i] = e;

                // Wetness: reduce on ridges (rocky/exposed), but keep it subtle.
                float w = wetField[i];
                w = std::clamp(w - ridge * 0.12f, 0.0f, 1.0f);
                wetField[i] = w;
            }
        }
    }

    // Generate terrain inside the chunk bounds (leave border for walls/gates).
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            const size_t i = idx(x, y);
            const float elev = elevField[i];
            const float wet  = wetField[i];
            const float var  = varField[i];
            const float ridge = (i < ridgeField.size()) ? ridgeField[i] : 0.0f;

            TileType tt = TileType::Floor;

            // Mountains on high elevation (tectonic ridges lower the local threshold).
            float mountainMinHere = mountainElevMin;
            mountainMinHere = std::clamp(mountainMinHere - ridge * 0.12f, 0.55f, 0.95f);
            if (elev > mountainMinHere) {
                tt = TileType::Wall;
            }

            // Water basins in low elevation + wet.
            if (tt == TileType::Floor && elev < lakeElevMax && wet > lakeWetMin) {
                tt = TileType::Chasm;
                d.fluvialChasmCount++;
            }

            // Vegetation / trees.
            if (tt == TileType::Floor && wet > (treeWetMin + ridge * 0.05f) && elev < treeElevMax && var < (treeChance * (1.0f - ridge * 0.35f))) {
                tt = TileType::Pillar;
                d.heightfieldRidgePillarCount++;
            }

            // Scree / boulders at moderate-high elevations.
            if (tt == TileType::Floor && elev > (screeElevMin - ridge * 0.06f) && var < (screeVarMax + ridge * 0.06f)) {
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
    // Upgrade: a *domain-warped*, multi-band river field.
    //
    //   - Two low-frequency trunk bands (different orientations) create long,
    //     readable rivers and occasional confluences.
    //   - A higher-frequency tributary band activates near trunks and in wetter
    //     areas, adding dendritic side-channels.
    //   - Width is modulated by wetness and *local flatness* (braiding) to make
    //     plains/swamps feel more "fluvial" without turning everything into water.
    //
    // All sampling is in world space, so rivers remain seamless across chunk borders.
    // Trails are carved *after* this pass, guaranteeing that gates remain mutually
    // reachable even when water slices across the wilderness.
    {
        const uint32_t sRivA   = hashCombine(base, "RIV"_tag);
        const uint32_t sRivB   = hashCombine(base, "RIV2"_tag);
        const uint32_t sRivW   = hashCombine(base, "RIVW"_tag);
        const uint32_t sWarpX  = hashCombine(base, "RIVWX"_tag);
        const uint32_t sWarpY  = hashCombine(base, "RIVWY"_tag);
        const uint32_t sTrib   = hashCombine(base, "RIVT"_tag);
        const uint32_t sTribW  = hashCombine(base, "RIVTW"_tag);
        const uint32_t sBank   = hashCombine(base, "RIVBANK"_tag);

        // Frequencies are in world-tile coordinates.
        const float mainFreq = 0.0062f; // trunks
        const float tribFreq = 0.0133f; // tributaries
        const float capFreq  = 0.0210f; // micro-channels (swamps/coasts)

        // Biome-tuned knobs.
        float bandBase = 0.012f;  // base half-width of the trunk band
        float wetBoost = 0.004f;  // additional widening in very wet areas
        float elevMin  = 0.20f;   // avoid deep basins (handled as lakes)
        float elevMax  = std::max(0.0f, mountainElevMin - 0.05f); // don't cut mountains by default

        float warpAmp    = 0.18f; // domain-warp amplitude (in noise-space units)
        float braidAmp   = 0.55f; // width boost in flat+wet areas
        float valleyMul  = 3.20f; // tributary activation radius (multiples of trunk band)
        float tribMul    = 0.55f; // tributary band width relative to trunk
        float tribWetMin = 0.62f; // require some wetness to grow tributaries
        float capWetMin  = 0.78f; // micro-channels only in very wet areas

        float bankBaseChance = 0.14f; // base chance to add a bank obstacle near water
        float bankTreeBias   = 0.55f; // probability to choose Pillar over Boulder

        switch (biome) {
            case Biome::Swamp:
                bandBase = 0.020f;
                wetBoost = 0.012f;
                elevMin  = 0.16f;
                warpAmp  = 0.20f;
                braidAmp = 0.95f;
                tribWetMin = 0.56f;
                capWetMin  = 0.70f;
                bankBaseChance = 0.22f;
                bankTreeBias   = 0.86f;
                break;
            case Biome::Coast:
                bandBase = 0.018f;
                wetBoost = 0.010f;
                elevMin  = 0.16f;
                warpAmp  = 0.20f;
                braidAmp = 0.80f;
                tribWetMin = 0.58f;
                capWetMin  = 0.72f;
                bankBaseChance = 0.18f;
                bankTreeBias   = 0.65f;
                break;
            case Biome::Forest:
                bandBase = 0.014f;
                wetBoost = 0.007f;
                elevMin  = 0.18f;
                warpAmp  = 0.18f;
                braidAmp = 0.60f;
                tribWetMin = 0.60f;
                capWetMin  = 0.80f;
                bankBaseChance = 0.18f;
                bankTreeBias   = 0.80f;
                break;
            case Biome::Plains:
                bandBase = 0.012f;
                wetBoost = 0.005f;
                elevMin  = 0.20f;
                warpAmp  = 0.18f;
                braidAmp = 0.65f;
                tribWetMin = 0.62f;
                capWetMin  = 0.82f;
                bankBaseChance = 0.14f;
                bankTreeBias   = 0.62f;
                break;
            case Biome::Tundra:
                bandBase = 0.011f;
                wetBoost = 0.004f;
                elevMin  = 0.20f;
                warpAmp  = 0.17f;
                braidAmp = 0.50f;
                tribWetMin = 0.66f;
                capWetMin  = 0.84f;
                bankBaseChance = 0.12f;
                bankTreeBias   = 0.18f;
                break;
            case Biome::Highlands:
                bandBase = 0.009f;
                wetBoost = 0.002f;
                elevMin  = 0.24f;
                warpAmp  = 0.16f;
                braidAmp = 0.40f;
                tribWetMin = 0.68f;
                capWetMin  = 0.86f;
                bankBaseChance = 0.12f;
                bankTreeBias   = 0.12f;
                break;
            case Biome::Badlands:
                bandBase = 0.008f;
                wetBoost = 0.001f;
                elevMin  = 0.24f;
                warpAmp  = 0.16f;
                braidAmp = 0.40f;
                tribWetMin = 0.70f;
                capWetMin  = 0.88f;
                bankBaseChance = 0.10f;
                bankTreeBias   = 0.08f;
                break;
            case Biome::Desert:
                bandBase = 0.006f;  // rare wadis
                wetBoost = 0.000f;
                elevMin  = 0.26f;
                warpAmp  = 0.15f;
                braidAmp = 0.25f;
                tribWetMin = 0.78f;
                capWetMin  = 0.92f;
                bankBaseChance = 0.06f;
                bankTreeBias   = 0.03f;
                break;
            default:
                break;
        }

        // Gate throat positions (one step inside the border) are kept relatively clear.
        const ChunkGates gBank = gatePositions(d, runSeed, chunkX, chunkY);
        const Vec2i throatN{gBank.north.x, gBank.north.y + 1};
        const Vec2i throatS{gBank.south.x, gBank.south.y - 1};
        const Vec2i throatW{gBank.west.x + 1, gBank.west.y};
        const Vec2i throatE{gBank.east.x - 1, gBank.east.y};

        auto nearThroat = [&](int x, int y) -> bool {
            auto md = [&](Vec2i p) -> int { return std::abs(x - p.x) + std::abs(y - p.y); };
            const int d0 = std::min(std::min(md(throatN), md(throatS)), std::min(md(throatW), md(throatE)));
            return d0 <= 4;
        };

        auto elevSamp = [&](int x, int y) -> float {
            x = std::clamp(x, 1, d.width - 2);
            y = std::clamp(y, 1, d.height - 2);
            const size_t i = idx(x, y);
            if (i >= elevField.size()) return 0.0f;
            return elevField[i];
        };

        auto localSlope = [&](int x, int y) -> float {
            const float ex = elevSamp(x + 1, y) - elevSamp(x - 1, y);
            const float ey = elevSamp(x, y + 1) - elevSamp(x, y - 1);
            return std::sqrt(ex * ex + ey * ey);
        };

        // First: carve the river channels (Chasm) deterministically.
        for (int y = 1; y < d.height - 1; ++y) {
            const int wy = wy0 + y;
            for (int x = 1; x < d.width - 1; ++x) {
                if (d.at(x, y).type == TileType::Wall) continue;

                const size_t i = idx(x, y);
                const float elev = elevField[i];
                if (elev < elevMin || elev > elevMax) continue;

                const int wx = wx0 + x;
                const float wet = wetField[i];

                // Domain warp (sampled at lower frequency than the trunks).
                const float fx = static_cast<float>(wx) * mainFreq;
                const float fy = static_cast<float>(wy) * mainFreq;

                const float wfx = fx * 0.55f;
                const float wfy = fy * 0.55f;
                const float wxw = fbm01(sWarpX, wfx + 11.0f, wfy - 27.0f, 3) - 0.5f;
                const float wyw = fbm01(sWarpY, wfx - 19.0f, wfy + 37.0f, 3) - 0.5f;
                const float dx = wxw * warpAmp;
                const float dy = wyw * warpAmp;

                // Two trunk bands: second band uses a rotated coordinate basis.
                constexpr float COS30 = 0.8660254f;
                constexpr float SIN30 = 0.5f;
                const float rfx = COS30 * fx + SIN30 * fy;
                const float rfy = -SIN30 * fx + COS30 * fy;

                const float nA = fbm01(sRivA, fx + dx,  fy + dy,  3);
                const float nB = fbm01(sRivB, rfx + dx * 0.85f, rfy + dy * 0.85f, 3);
                const float dMain = std::min(std::abs(nA - 0.5f), std::abs(nB - 0.5f));

                // Width modulation + braiding in flat, wet areas.
                const float w = fbm01(sRivW, (fx + dx) * 3.05f, (fy + dy) * 3.05f, 2);

                float band = bandBase * (0.62f + 0.92f * w);
                band += std::max(0.0f, wet - 0.55f) * wetBoost;

                const float slope = localSlope(x, y);
                const float flat  = 1.0f - std::clamp(slope * 18.0f, 0.0f, 1.0f);
                const float braid = flat * std::max(0.0f, wet - 0.58f);
                band *= (1.0f + braidAmp * braid);

                bool carve = (dMain < band);

                // Tributaries: a finer band that only "activates" near trunk valleys.
                if (!carve && wet > tribWetMin) {
                    const float tfx = static_cast<float>(wx) * tribFreq;
                    const float tfy = static_cast<float>(wy) * tribFreq;
                    const float tdx = dx * 2.2f;
                    const float tdy = dy * 2.2f;

                    const float tn = fbm01(sTrib,  tfx + tdx, tfy + tdy, 3);
                    const float tw = fbm01(sTribW, (tfx + tdx) * 2.10f, (tfy + tdy) * 2.10f, 2);

                    float tBand = (bandBase * tribMul) * (0.60f + 0.85f * tw);
                    const float valley = band * valleyMul;

                    if (dMain < valley && std::abs(tn - 0.5f) < tBand) {
                        carve = true;
                    }
                }

                // Micro-channels: swampy/coastal capillaries that add texture to wetlands.
                if (!carve && wet > capWetMin) {
                    const float cn = fbm01(sTrib, static_cast<float>(wx) * capFreq + 101.0f,
                                            static_cast<float>(wy) * capFreq - 59.0f, 2);
                    const float cBand = (bandBase * 0.28f) * (0.85f + 0.35f * flat);
                    if (std::abs(cn - 0.5f) < cBand) {
                        carve = true;
                    }
                }

                if (carve) {
                    if (d.at(x, y).type != TileType::Chasm) d.fluvialChasmCount++;
                    d.at(x, y).type = TileType::Chasm;
                }
            }
        }

        // Second: add sparse riparian banks (Pillars/Boulders) along water edges.
        // Trails/landmarks can still carve through these later.
        auto bankRand01 = [&](int x, int y) -> float {
            const int wx = wx0 + x;
            const int wy = wy0 + y;
            return u32To01(hashCoord(sBank, wx, wy));
        };

        for (int y = 2; y < d.height - 2; ++y) {
            for (int x = 2; x < d.width - 2; ++x) {
                if (d.at(x, y).type != TileType::Chasm) continue;

                static const int DX[4] = { 1, -1, 0, 0 };
                static const int DY[4] = { 0, 0, 1, -1 };

                for (int k = 0; k < 4; ++k) {
                    const int nx = x + DX[k];
                    const int ny = y + DY[k];
                    if (nx <= 1 || ny <= 1 || nx >= d.width - 2 || ny >= d.height - 2) continue;
                    if (nearThroat(nx, ny)) continue;

                    if (d.at(nx, ny).type != TileType::Floor) continue;

                    const size_t ni = idx(nx, ny);
                    const float wet = (ni < wetField.size()) ? wetField[ni] : 0.0f;
                    const float elev = (ni < elevField.size()) ? elevField[ni] : 0.0f;

                    float p = bankBaseChance;
                    // Wetter areas are more likely to get banks (vegetation/rocks).
                    p *= (0.35f + 0.90f * std::clamp(wet, 0.0f, 1.0f));
                    // Very high elevations get fewer banks (rocky/sheer edges already read "hard").
                    p *= (1.0f - std::clamp((elev - 0.72f) * 1.5f, 0.0f, 0.85f));

                    const float r0 = bankRand01(nx, ny);
                    if (r0 > p) continue;

                    float treeBias = bankTreeBias;
                    treeBias *= std::clamp((wet - 0.45f) * 2.2f, 0.0f, 1.0f);

                    // Use a second hashed sample to avoid correlation with the place/no-place decision.
                    const float r1 = u32To01(hashCoord(sBank, (wx0 + nx) + 97, (wy0 + ny) - 23));
                    const bool placeTree = (r1 < treeBias);

                    const TileType tt = placeTree ? TileType::Pillar : TileType::Boulder;
                    d.at(nx, ny).type = tt;
                    if (tt == TileType::Pillar) d.heightfieldRidgePillarCount++;
                    else d.heightfieldScreeBoulderCount++;
                }
            }
        }
    }

    // ---------------------------------------------------------------------

    // ---------------------------------------------------------------------
    // Talus aprons (mountain foothills)
    // ---------------------------------------------------------------------
    // Tectonic ridges create more coherent mountain chains; to make them read
    // like *terrain* (not just noise-threshold walls), we sprinkle a light
    // boulder "talus" apron along mountain edges.
    //
    // This runs *after* macro rivers (so banks stay readable) but *before*
    // landmarks/ruins (so we don't decorate man-made walls).
    {
        float baseChance = 0.08f;
        float maxChance  = 0.20f;
        float ridgeMin   = 0.18f; // ignore non-tectonic walls (ruins) by default

        switch (biome) {
            case Biome::Highlands:
                baseChance = 0.12f;
                maxChance  = 0.26f;
                ridgeMin   = 0.12f;
                break;
            case Biome::Badlands:
                baseChance = 0.11f;
                maxChance  = 0.24f;
                ridgeMin   = 0.14f;
                break;
            case Biome::Desert:
                baseChance = 0.09f;
                maxChance  = 0.18f;
                ridgeMin   = 0.16f;
                break;
            case Biome::Tundra:
                baseChance = 0.10f;
                maxChance  = 0.20f;
                ridgeMin   = 0.15f;
                break;
            case Biome::Forest:
            case Biome::Swamp:
            case Biome::Coast:
                baseChance = 0.05f;
                maxChance  = 0.14f;
                ridgeMin   = 0.18f;
                break;
            default:
                break;
        }

        const ChunkGates gTal = gatePositions(d, runSeed, chunkX, chunkY);
        const Vec2i throatN{gTal.north.x, gTal.north.y + 1};
        const Vec2i throatS{gTal.south.x, gTal.south.y - 1};
        const Vec2i throatW{gTal.west.x + 1, gTal.west.y};
        const Vec2i throatE{gTal.east.x - 1, gTal.east.y};

        auto nearThroat = [&](int x, int y) -> bool {
            auto md = [&](Vec2i p) -> int { return std::abs(x - p.x) + std::abs(y - p.y); };
            const int d0 = std::min(std::min(md(throatN), md(throatS)), std::min(md(throatW), md(throatE)));
            return d0 <= 4;
        };

        const uint32_t sTalus = hashCombine(base, "OW_TALUS"_tag);

        for (int y = 2; y < d.height - 2; ++y) {
            const int wy = wy0 + y;
            for (int x = 2; x < d.width - 2; ++x) {
                if (nearThroat(x, y)) continue;
                if (d.at(x, y).type != TileType::Floor) continue;

                const size_t i = idx(x, y);
                if (i >= wetField.size()) continue;

                // Any adjacent wall? And is it likely a tectonic mountain wall?
                float ridgeAdj = 0.0f;
                int wallAdj = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (!d.inBounds(nx, ny)) continue;
                        if (d.at(nx, ny).type != TileType::Wall) continue;
                        wallAdj++;

                        const size_t wi = idx(nx, ny);
                        if (wi < ridgeField.size()) ridgeAdj = std::max(ridgeAdj, ridgeField[wi]);
                    }
                }
                if (wallAdj == 0) continue;
                if (ridgeAdj < ridgeMin) continue;

                float p = baseChance + (maxChance - baseChance) * ridgeAdj;

                // Drier => more talus; very wet => fewer loose rocks.
                const float wet = wetField[i];
                p *= (1.20f - std::clamp(wet * 0.95f, 0.0f, 0.95f));

                // More wall adjacency => more talus.
                p *= (0.55f + 0.12f * static_cast<float>(wallAdj));
                p = std::clamp(p, 0.0f, 0.55f);

                const float r = u32To01(hashCoord(sTalus, wx0 + x, wy));
                if (r < p) {
                    d.at(x, y).type = TileType::Boulder;
                    d.heightfieldScreeBoulderCount++;
                }
            }
        }
    }

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
    // Ruin strongholds (rare overworld setpieces)
    // ---------------------------------------------------------------------
    // A "stronghold" is a ruined settlement/fortification: a broken perimeter
    // wall, a small keep (as a Vault room), and at least one guaranteed cache
    // chest. We generate these *before* trails so the trail pass can always
    // repair global connectivity.
    bool hasStronghold = false;
    Vec2i strongholdApproach{-1, -1};
    Room strongholdVault; // keep footprint; pushed as RoomType::Vault at the end

    {
        RNG strongRng(hashCombine(prof.seed, "OW_STRONGHOLD"_tag));

        // Base chance tuned so a neighborhood scan usually finds a few, but
        // they remain "high-signal" discoveries.
        float chance = 0.030f + 0.0023f * static_cast<float>(std::clamp(prof.dangerDepth, 0, 12));
        if (prof.dangerDepth <= 0) chance *= 0.40f; // keep the immediate camp vicinity quieter

        switch (biome) {
            case Biome::Badlands:   chance *= 1.25f; break;
            case Biome::Highlands:  chance *= 1.18f; break;
            case Biome::Plains:     chance *= 1.08f; break;
            case Biome::Forest:     chance *= 0.95f; break;
            case Biome::Coast:      chance *= 0.92f; break;
            case Biome::Swamp:      chance *= 0.90f; break;
            case Biome::Tundra:     chance *= 0.85f; break;
            case Biome::Desert:     chance *= 0.62f; break;
            default: break;
        }

        if (chance > 0.0f && strongRng.chance(chance)) {
            // Split into independent streams so tuning one sub-step doesn't
            // cascade changes through the rest of the setpiece.
            RNG pickRng(hashCombine(strongRng.nextU32(), "STR_PICK"_tag));
            RNG ringRng(hashCombine(strongRng.nextU32(), "STR_RING"_tag));
            RNG buildRng(hashCombine(strongRng.nextU32(), "STR_BUILD"_tag));
            RNG decoRng(hashCombine(strongRng.nextU32(), "STR_DECO"_tag));

            int rx = 0;
            int ry = 0;
            Vec2i c{-1, -1};

            for (int attempt = 0; attempt < 90; ++attempt) {
                rx = pickRng.range(11, 16);
                ry = pickRng.range(8, 13);

                const int x = pickRng.range(rx + 3, d.width - rx - 4);
                const int y = pickRng.range(ry + 3, d.height - ry - 4);
                if (!farFromGates(x, y)) continue;

                // Keep the chunk center readable; strongholds want to be a bit off-axis.
                if (std::abs(x - d.width / 2) + std::abs(y - d.height / 2) < 10) {
                    if (pickRng.chance(0.85f)) continue;
                }

                int chasm = 0;
                int wall = 0;
                for (int yy = y - ry; yy <= y + ry; ++yy) {
                    for (int xx = x - rx; xx <= x + rx; ++xx) {
                        if (!d.inBounds(xx, yy)) continue;
                        const TileType tt = d.at(xx, yy).type;
                        if (tt == TileType::Chasm) ++chasm;
                        else if (tt == TileType::Wall) ++wall;
                    }
                }
                const int area = (2 * rx + 1) * (2 * ry + 1);

                // Strongholds are easiest to read on dry-ish land.
                if (chasm > std::max(2, area / 20)) continue;       // ~5% water tolerance
                if (wall > static_cast<int>(area * 0.55f)) continue; // avoid stamping on full mountain mass

                c = {x, y};
                break;
            }

            if (c.x >= 0) {
                hasStronghold = true;
                d.overworldStrongholdCount++;

                const int x1 = c.x - rx;
                const int x2 = c.x + rx;
                const int y1 = c.y - ry;
                const int y2 = c.y + ry;

                auto inInterior = [&](int x, int y) -> bool {
                    return x > 0 && y > 0 && x < d.width - 1 && y < d.height - 1;
                };

                auto setFloorSafe = [&](int x, int y) {
                    if (!inInterior(x, y)) return;
                    const TileType tt = d.at(x, y).type;
                    if (tt == TileType::Fountain || tt == TileType::Altar) return;
                    d.at(x, y).type = TileType::Floor;
                };

                // Clear the footprint (ruins were built here long ago).
                for (int yy = y1; yy <= y2; ++yy) {
                    for (int xx = x1; xx <= x2; ++xx) {
                        setFloorSafe(xx, yy);
                    }
                }

                // Courtyard / plaza clearing.
                carveDisk(c, std::max(2, rx / 4), std::max(2, ry / 4), TileType::Floor);

                // Perimeter wall: ellipse ring with deterministic breaks.
                const float frx = static_cast<float>(std::max(1, rx));
                const float fry = static_cast<float>(std::max(1, ry));
                const float ringInner = 0.90f;
                const float ringOuter = 1.08f;

                for (int yy = y1; yy <= y2; ++yy) {
                    for (int xx = x1; xx <= x2; ++xx) {
                        if (!inInterior(xx, yy)) continue;
                        const float nx = static_cast<float>(xx - c.x) / frx;
                        const float ny = static_cast<float>(yy - c.y) / fry;
                        const float d2 = nx * nx + ny * ny;
                        if (d2 < ringInner || d2 > ringOuter) continue;

                        if (ringRng.chance(0.82f)) {
                            if (d.at(xx, yy).type != TileType::Wall) d.overworldStrongholdWallTiles++;
                            d.at(xx, yy).type = TileType::Wall;
                        } else if (ringRng.chance(0.35f)) {
                            d.at(xx, yy).type = TileType::Boulder;
                            d.heightfieldScreeBoulderCount++;
                        } else {
                            d.at(xx, yy).type = TileType::Floor;
                        }
                    }
                }

                // Choose and carve a main breach (3-wide).
                const int dir = pickRng.range(0, 3);
                Vec2i ent = c;
                Vec2i insideStep{0, 0};
                Vec2i lateral{0, 0};
                switch (dir) {
                    case 0: // north
                        ent.y -= ry;
                        ent.x += pickRng.range(-rx / 4, rx / 4);
                        insideStep = {0, 1};
                        lateral = {1, 0};
                        break;
                    case 1: // south
                        ent.y += ry;
                        ent.x += pickRng.range(-rx / 4, rx / 4);
                        insideStep = {0, -1};
                        lateral = {1, 0};
                        break;
                    case 2: // west
                        ent.x -= rx;
                        ent.y += pickRng.range(-ry / 4, ry / 4);
                        insideStep = {1, 0};
                        lateral = {0, 1};
                        break;
                    default: // east
                        ent.x += rx;
                        ent.y += pickRng.range(-ry / 4, ry / 4);
                        insideStep = {-1, 0};
                        lateral = {0, 1};
                        break;
                }
                ent.x = std::clamp(ent.x, x1 + 2, x2 - 2);
                ent.y = std::clamp(ent.y, y1 + 2, y2 - 2);

                auto setDoorOpenSafe = [&](int x, int y) {
                    if (!inInterior(x, y)) return;
                    const TileType tt = d.at(x, y).type;
                    if (tt == TileType::Fountain || tt == TileType::Altar) return;
                    d.at(x, y).type = TileType::DoorOpen;
                };

                for (int s = -1; s <= 1; ++s) {
                    const int bx = ent.x + lateral.x * s;
                    const int by = ent.y + lateral.y * s;
                    setFloorSafe(bx, by);
                    setFloorSafe(bx + insideStep.x, by + insideStep.y);
                    setFloorSafe(bx + insideStep.x * 2, by + insideStep.y * 2);
                }
                setDoorOpenSafe(ent.x, ent.y);
                strongholdApproach = ent;

                // Helper: rectangle overlap.
                auto rectsOverlap = [&](int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) -> bool {
                    return !(ax + aw <= bx || bx + bw <= ax || ay + ah <= by || by + bh <= ay);
                };

                auto carveRectRuin = [&](int x0, int y0, int w, int h, bool heavierRubble) {
                    for (int y = y0; y < y0 + h; ++y) {
                        for (int x = x0; x < x0 + w; ++x) {
                            if (!inInterior(x, y)) continue;
                            const bool border = (x == x0 || y == y0 || x == x0 + w - 1 || y == y0 + h - 1);
                            if (border) {
                                // Broken walls: mostly wall, some gaps / rubble.
                                float breakChance = heavierRubble ? 0.22f : 0.12f;
                                float rubbleChance = heavierRubble ? 0.28f : 0.18f;

                                if (decoRng.chance(breakChance)) {
                                    d.at(x, y).type = TileType::Floor;
                                } else if (decoRng.chance(rubbleChance)) {
                                    d.at(x, y).type = TileType::Boulder;
                                    d.heightfieldScreeBoulderCount++;
                                } else {
                                    if (d.at(x, y).type != TileType::Wall) d.overworldStrongholdWallTiles++;
                                    d.at(x, y).type = TileType::Wall;
                                }
                            } else {
                                d.at(x, y).type = TileType::Floor;
                            }
                        }
                    }

                    const int rubble = heavierRubble ? decoRng.range(2, 5) : decoRng.range(1, 3);
                    for (int i = 0; i < rubble; ++i) {
                        const int rx = decoRng.range(x0 + 1, x0 + w - 2);
                        const int ry = decoRng.range(y0 + 1, y0 + h - 2);
                        if (!inInterior(rx, ry)) continue;
                        const TileType tt = d.at(rx, ry).type;
                        if (tt == TileType::Fountain || tt == TileType::Altar) continue;
                        d.at(rx, ry).type = TileType::Boulder;
                        d.heightfieldScreeBoulderCount++;
                    }
                };

                // Keep placement: biased "behind" the breach.
                Vec2i keepC = c;
                keepC.x += insideStep.x * std::max(2, rx / 5);
                keepC.y += insideStep.y * std::max(2, ry / 5);
                keepC.x = std::clamp(keepC.x, x1 + 5, x2 - 5);
                keepC.y = std::clamp(keepC.y, y1 + 5, y2 - 5);

                int kw = buildRng.range(9, 13);
                int kh = buildRng.range(7, 11);
                if ((kw & 1) == 0) ++kw;
                if ((kh & 1) == 0) ++kh;

                int kx0 = std::clamp(keepC.x - kw / 2, x1 + 2, x2 - kw - 1);
                int ky0 = std::clamp(keepC.y - kh / 2, y1 + 2, y2 - kh - 1);

                carveRectRuin(kx0, ky0, kw, kh, /*heavierRubble=*/false);
                d.overworldStrongholdBuildingCount++;

                // Keep door (faces the breach).
                Vec2i door{keepC.x, keepC.y};
                if (insideStep.x != 0) {
                    door.x = (insideStep.x > 0) ? kx0 : (kx0 + kw - 1);
                    door.y = std::clamp(keepC.y, ky0 + 1, ky0 + kh - 2);
                } else {
                    door.y = (insideStep.y > 0) ? ky0 : (ky0 + kh - 1);
                    door.x = std::clamp(keepC.x, kx0 + 1, kx0 + kw - 2);
                }

                const float lockBase = 0.22f + 0.03f * static_cast<float>(std::clamp(prof.dangerDepth, 0, 10));
                const bool locked = decoRng.chance(std::clamp(lockBase, 0.10f, 0.65f));
                d.at(door.x, door.y).type = locked ? TileType::DoorLocked : TileType::DoorClosed;
                setFloorSafe(door.x - insideStep.x, door.y - insideStep.y);

                // Guaranteed cache chest deep inside the keep (bonusLootSpots).
                Vec2i cache{keepC.x, keepC.y};
                if (insideStep.x > 0) cache.x = kx0 + kw - 2;
                else if (insideStep.x < 0) cache.x = kx0 + 1;
                if (insideStep.y > 0) cache.y = ky0 + kh - 2;
                else if (insideStep.y < 0) cache.y = ky0 + 1;
                cache.x = std::clamp(cache.x, kx0 + 1, kx0 + kw - 2);
                cache.y = std::clamp(cache.y, ky0 + 1, ky0 + kh - 2);
                setFloorSafe(cache.x, cache.y);
                d.bonusLootSpots.push_back(cache);
                d.overworldStrongholdCacheCount++;

                // A little salvage in/around the courtyard.
                if (decoRng.chance(0.70f)) {
                    const Vec2i goldPos{std::clamp(c.x + pickRng.range(-2, 2), x1 + 2, x2 - 2), std::clamp(c.y + pickRng.range(-2, 2), y1 + 2, y2 - 2)};
                    d.bonusItemSpawns.push_back({goldPos, ItemKind::Gold, decoRng.range(8, 22)});
                }
                if (decoRng.chance(0.42f)) {
                    const Vec2i rationPos{std::clamp(c.x + pickRng.range(-3, 3), x1 + 2, x2 - 2), std::clamp(c.y + pickRng.range(-3, 3), y1 + 2, y2 - 2)};
                    d.bonusItemSpawns.push_back({rationPos, ItemKind::FoodRation, 1});
                }

                // Optional courtyard altar (rare; avoids harsh biomes).
                if (biome != Biome::Desert && biome != Biome::Badlands && decoRng.chance(0.18f)) {
                    d.at(c.x, c.y).type = TileType::Altar;
                }

                // Outbuildings: blue-noise seeds inside the ellipse.
                const int domMinX = x1 + 3;
                const int domMinY = y1 + 3;
                const int domMaxX = x2 - 3;
                const int domMaxY = y2 - 3;
                const float minDist = std::clamp(static_cast<float>(std::min(rx, ry)) * 0.80f, 7.0f, 12.0f);

                std::vector<Vec2i> seeds = procgen::poissonDiscSample2D(buildRng, domMinX, domMinY, domMaxX, domMaxY, minDist, 24);
                int buildingsPlaced = 0;
                const int targetBuildings = std::clamp(buildRng.range(3, 6), 2, 7);

                for (const Vec2i& p : seeds) {
                    if (buildingsPlaced >= targetBuildings) break;

                    // Stay well inside the ellipse interior.
                    const float nx = static_cast<float>(p.x - c.x) / frx;
                    const float ny = static_cast<float>(p.y - c.y) / fry;
                    if (nx * nx + ny * ny > 0.78f) continue;

                    // Keep the courtyard clear.
                    if (std::abs(p.x - c.x) + std::abs(p.y - c.y) < (rx / 3)) continue;

                    int bw = buildRng.range(5, 9);
                    int bh = buildRng.range(5, 9);
                    if (buildRng.chance(0.35f)) std::swap(bw, bh);
                    if ((bw & 1) == 0) ++bw;
                    if ((bh & 1) == 0) ++bh;

                    int bx0 = std::clamp(p.x - bw / 2, domMinX, domMaxX - bw);
                    int by0 = std::clamp(p.y - bh / 2, domMinY, domMaxY - bh);

                    // Avoid overlapping the keep (with a small buffer).
                    if (rectsOverlap(bx0, by0, bw, bh, kx0 - 2, ky0 - 2, kw + 4, kh + 4)) continue;

                    // Avoid overlapping the breach apron (keeps the approach readable).
                    if (std::abs(p.x - ent.x) + std::abs(p.y - ent.y) < 6) continue;

                    carveRectRuin(bx0, by0, bw, bh, /*heavierRubble=*/true);
                    d.overworldStrongholdBuildingCount++;
                    buildingsPlaced++;

                    // Add a simple "door" opening facing the courtyard.
                    const int dx = c.x - p.x;
                    const int dy = c.y - p.y;
                    Vec2i dpos{p.x, p.y};
                    if (std::abs(dx) >= std::abs(dy)) {
                        dpos.x = (dx > 0) ? (bx0 + bw - 1) : bx0;
                        dpos.y = std::clamp(p.y, by0 + 1, by0 + bh - 2);
                    } else {
                        dpos.y = (dy > 0) ? (by0 + bh - 1) : by0;
                        dpos.x = std::clamp(p.x, bx0 + 1, bx0 + bw - 2);
                    }
                    if (decoRng.chance(0.55f)) {
                        d.at(dpos.x, dpos.y).type = TileType::DoorOpen;
                    } else {
                        setFloorSafe(dpos.x, dpos.y);
                    }

                    // Small chance of a second, minor stash.
                    if (d.overworldStrongholdCacheCount < 2 && decoRng.chance(0.15f)) {
                        Vec2i stash{bx0 + bw / 2, by0 + bh / 2};
                        stash.x = std::clamp(stash.x, bx0 + 1, bx0 + bw - 2);
                        stash.y = std::clamp(stash.y, by0 + 1, by0 + bh - 2);
                        setFloorSafe(stash.x, stash.y);
                        d.bonusLootSpots.push_back(stash);
                        d.overworldStrongholdCacheCount++;
                    }
                }

                // Vault room annotation for spawn/BuC logic.
                strongholdVault.x = kx0;
                strongholdVault.y = ky0;
                strongholdVault.w = kw;
                strongholdVault.h = kh;
                strongholdVault.type = RoomType::Vault;
            }
        }
    }

    std::vector<Vec2i> placedSprings;
    placedSprings.reserve(4);

    // ---------------------------------------------------------------------
    // Springs (minor overworld POIs)
    // ---------------------------------------------------------------------
    // Place 0..2 small, interactable fountains as "springs". We use a blue-noise
    // seed distribution (Poisson-disc) so springs don't clump, then bias the
    // final selection toward wet + flat terrain (valleys / low-gradient basins).
    {
        RNG springRng(hashCombine(prof.seed, "OW_SPRING"_tag));

        float springChance = 0.32f;
        float springWetMin = 0.52f;
        float springElevMax = 0.70f;

        switch (biome) {
            case Biome::Swamp:
                springChance = 0.62f;
                springWetMin = 0.46f;
                springElevMax = 0.78f;
                break;
            case Biome::Coast:
                springChance = 0.52f;
                springWetMin = 0.48f;
                springElevMax = 0.74f;
                break;
            case Biome::Forest:
                springChance = 0.46f;
                springWetMin = 0.50f;
                springElevMax = 0.76f;
                break;
            case Biome::Plains:
                springChance = 0.34f;
                springWetMin = 0.54f;
                springElevMax = 0.72f;
                break;
            case Biome::Tundra:
                springChance = 0.28f;
                springWetMin = 0.56f;
                springElevMax = 0.70f;
                break;
            case Biome::Highlands:
                springChance = 0.22f;
                springWetMin = 0.58f;
                springElevMax = 0.68f;
                break;
            case Biome::Badlands:
                springChance = 0.20f;
                springWetMin = 0.60f;
                springElevMax = 0.66f;
                break;
            case Biome::Desert:
                springChance = 0.16f;
                springWetMin = 0.62f;
                springElevMax = 0.64f;
                break;
            default:
                break;
        }

        int targetSprings = 0;
        if (springRng.chance(springChance)) targetSprings = 1;
        if (springRng.chance(springChance * 0.22f)) targetSprings += 1;
        targetSprings = std::clamp(targetSprings, 0, 2);

        auto idx2 = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * d.width + x);
        };

        auto localSlope2 = [&](int x, int y) -> float {
            const float c = elevField[idx2(x, y)];
            float m = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= d.width || ny >= d.height) continue;
                    const float e = elevField[idx2(nx, ny)];
                    m = std::max(m, std::abs(e - c));
                }
            }
            return m;
        };

        auto nearWater = [&](int x, int y) -> int {
            int adj = 0;
            static const int DX[8] = { 1,-1,0,0,  1,1,-1,-1 };
            static const int DY[8] = { 0,0,1,-1, 1,-1,1,-1 };
            for (int k = 0; k < 8; ++k) {
                const int nx = x + DX[k];
                const int ny = y + DY[k];
                if (!d.inBounds(nx, ny)) continue;
                if (d.at(nx, ny).type == TileType::Chasm) ++adj;
            }
            return adj;
        };

        auto placeSpring = [&](Vec2i c) {
            if (!d.inBounds(c.x, c.y)) return;
            if (c.x <= 1 || c.y <= 1 || c.x >= d.width - 2 || c.y >= d.height - 2) return;

            // Ensure the center is walkable and not already a special tile.
            const TileType here = d.at(c.x, c.y).type;
            if (here == TileType::Wall || here == TileType::Chasm) return;

            d.at(c.x, c.y).type = TileType::Fountain;
            d.overworldSpringCount++;
            placedSprings.push_back(c);

            // Small clearing around the spring.
            const int r = springRng.range(2, 3);
            for (int yy = c.y - r; yy <= c.y + r; ++yy) {
                for (int xx = c.x - r; xx <= c.x + r; ++xx) {
                    if (!d.inBounds(xx, yy)) continue;
                    if (xx <= 0 || yy <= 0 || xx >= d.width - 1 || yy >= d.height - 1) continue;
                    const int md = std::abs(xx - c.x) + std::abs(yy - c.y);
                    if (md > r) continue;
                    if (d.at(xx, yy).type == TileType::Wall) continue;
                    if (d.at(xx, yy).type == TileType::Chasm) continue;
                    if (d.at(xx, yy).type != TileType::Fountain) d.at(xx, yy).type = TileType::Floor;
                }
            }

            // Riparian ring: reeds/trees in wetter biomes, stones in harsher ones.
            float vegBias = 0.55f;
            if (biome == Biome::Desert || biome == Biome::Badlands || biome == Biome::Highlands) vegBias = 0.18f;
            if (biome == Biome::Tundra) vegBias = 0.25f;

            for (int yy = c.y - (r + 1); yy <= c.y + (r + 1); ++yy) {
                for (int xx = c.x - (r + 1); xx <= c.x + (r + 1); ++xx) {
                    if (!d.inBounds(xx, yy)) continue;
                    if (xx <= 0 || yy <= 0 || xx >= d.width - 1 || yy >= d.height - 1) continue;
                    if (xx == c.x && yy == c.y) continue;

                    const int md = std::abs(xx - c.x) + std::abs(yy - c.y);
                    if (md != r + 1) continue;
                    if (!d.isWalkable(xx, yy)) continue;

                    const size_t ii = idx2(xx, yy);
                    const float wet = (ii < wetField.size()) ? wetField[ii] : 0.0f;
                    const float elev = (ii < elevField.size()) ? elevField[ii] : 0.0f;

                    float p = 0.20f;
                    p *= (0.45f + 0.80f * std::clamp(wet, 0.0f, 1.0f));
                    p *= (1.0f - std::clamp((elev - 0.70f) * 2.0f, 0.0f, 0.85f));
                    if (!springRng.chance(p)) continue;

                    const bool placeVeg = springRng.chance(vegBias);
                    const TileType tt = placeVeg ? TileType::Pillar : TileType::Boulder;
                    d.at(xx, yy).type = tt;
                    if (tt == TileType::Pillar) d.heightfieldRidgePillarCount++;
                    else d.heightfieldScreeBoulderCount++;
                }
            }
        };

        if (targetSprings > 0) {
            const int domMinX = 3;
            const int domMinY = 3;
            const int domMaxX = d.width - 4;
            const int domMaxY = d.height - 4;

            const float baseMinDist = std::clamp(static_cast<float>(std::min(d.width, d.height)) * 0.26f, 10.0f, 16.0f);
            std::vector<Vec2i> seeds = procgen::poissonDiscSample2D(springRng, domMinX, domMinY, domMaxX, domMaxY, baseMinDist, 24);

            struct Cand { Vec2i p; float score; };
            std::vector<Cand> cands;
            cands.reserve(seeds.size());

            for (const Vec2i& p : seeds) {
                if (!d.inBounds(p.x, p.y)) continue;
                if (!farFromGates(p.x, p.y)) continue;
                if (d.at(p.x, p.y).type != TileType::Floor) continue;

                const size_t ii = idx2(p.x, p.y);
                const float wet  = (ii < wetField.size()) ? wetField[ii] : 0.0f;
                const float elev = (ii < elevField.size()) ? elevField[ii] : 0.0f;
                if (wet < springWetMin) continue;
                if (elev > springElevMax) continue;

                const float slope = localSlope2(p.x, p.y);
                const float flat  = 1.0f - std::clamp(slope * 18.0f, 0.0f, 1.0f);

                // Prefer wet + flat tiles, lightly penalize proximity to existing rivers.
                float score = wet * 0.90f + flat * 0.60f;
                score -= std::max(0.0f, elev - 0.55f) * 0.65f;
                score -= static_cast<float>(nearWater(p.x, p.y)) * 0.06f;

                cands.push_back({p, score});
            }

            // Highest-score first; deterministic tie-breaker.
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                if (a.score != b.score) return a.score > b.score;
                if (a.p.y != b.p.y) return a.p.y < b.p.y;
                return a.p.x < b.p.x;
            });

            int placed = 0;
            for (const Cand& c : cands) {
                if (placed >= targetSprings) break;
                placeSpring(c.p);
                if (d.at(c.p.x, c.p.y).type == TileType::Fountain) {
                    placed++;
                }
            }

            // Fallback: if filtering was too strict, try a few random points.
            // (Keeps the system visible even in edge-case chunks.)
            if (placed == 0 && targetSprings > 0) {
                for (int tries = 0; tries < 64 && placed < targetSprings; ++tries) {
                    Vec2i p{ springRng.range(domMinX, domMaxX), springRng.range(domMinY, domMaxY) };
                    if (!farFromGates(p.x, p.y)) continue;
                    if (d.at(p.x, p.y).type != TileType::Floor) continue;
                    const size_t ii = idx2(p.x, p.y);
                    const float wet  = (ii < wetField.size()) ? wetField[ii] : 0.0f;
                    const float elev = (ii < elevField.size()) ? elevField[ii] : 0.0f;
                    if (wet < springWetMin) continue;
                    if (elev > springElevMax) continue;
                    const float slope = localSlope2(p.x, p.y);
                    if (slope > 0.10f) continue;
                    placeSpring(p);
                    if (d.at(p.x, p.y).type == TileType::Fountain) placed++;
                }
            }
        }
    }


    // ---------------------------------------------------------------------
    // Spring-fed brooks (micro-hydrology)
    // ---------------------------------------------------------------------
    // Springs (TileType::Fountain) now "spill" downhill into narrow brooks that
    // follow the local elevation gradient (a cheap D8-ish descent) until they
    // meet existing water (rivers/lakes) or terminate in a tiny pond at a sink.
    //
    // This is intentionally *lightweight*: it runs after macro rivers and
    // landmarks, but before trails, so connectivity is always repaired by the
    // trail-carving pass if water happens to cut across the critical gate paths.
    if (!placedSprings.empty()) {
        RNG brookRng(hashCombine(prof.seed, "OW_BROOK"_tag));
        const uint32_t sBrookBank = hashCombine(base, "BRKBANK"_tag);

        // Biome tuning.
        float perSpringChance = 0.92f;
        float bankChanceBase  = 0.08f; // bank obstacles around brooks/ponds (sparser than main rivers)
        float bankTreeBias    = 0.62f; // Pillar vs Boulder bias for banks
        float flatSlack       = 0.0035f; // allow tiny rises on flats (avoids premature termination)
        float minDrop         = 0.0008f; // descent threshold that qualifies as "real downhill"
        float turnPenalty     = 0.0020f; // mild inertia so brooks don't jitter

        switch (biome) {
            case Biome::Swamp:
                perSpringChance = 0.98f;
                bankChanceBase  = 0.14f;
                bankTreeBias    = 0.86f;
                flatSlack       = 0.0045f;
                minDrop         = 0.0006f;
                turnPenalty     = 0.0016f;
                break;
            case Biome::Coast:
                perSpringChance = 0.96f;
                bankChanceBase  = 0.11f;
                bankTreeBias    = 0.70f;
                flatSlack       = 0.0040f;
                minDrop         = 0.0007f;
                turnPenalty     = 0.0018f;
                break;
            case Biome::Forest:
                perSpringChance = 0.95f;
                bankChanceBase  = 0.12f;
                bankTreeBias    = 0.80f;
                flatSlack       = 0.0038f;
                minDrop         = 0.0007f;
                turnPenalty     = 0.0019f;
                break;
            case Biome::Plains:
                perSpringChance = 0.92f;
                bankChanceBase  = 0.09f;
                bankTreeBias    = 0.65f;
                break;
            case Biome::Tundra:
                perSpringChance = 0.88f;
                bankChanceBase  = 0.06f;
                bankTreeBias    = 0.22f;
                flatSlack       = 0.0030f;
                minDrop         = 0.0009f;
                break;
            case Biome::Highlands:
            case Biome::Badlands:
                perSpringChance = 0.86f;
                bankChanceBase  = 0.06f;
                bankTreeBias    = 0.14f;
                flatSlack       = 0.0030f;
                minDrop         = 0.0010f;
                break;
            case Biome::Desert:
                // Desert "springs" are rare; when they exist, bias toward an oasis pool.
                perSpringChance = 1.00f;
                bankChanceBase  = 0.05f;
                bankTreeBias    = 0.06f;
                flatSlack       = 0.0045f;
                minDrop         = 0.0005f;
                turnPenalty     = 0.0014f;
                break;
            default:
                break;
        }

        const int maxLen = std::clamp((d.width + d.height) * 2, 90, 180);

        auto elevAt = [&](int x, int y) -> float {
            x = std::clamp(x, 1, d.width - 2);
            y = std::clamp(y, 1, d.height - 2);
            const size_t i = idx(x, y);
            if (i < elevField.size()) return elevField[i];
            const int wx = wx0 + x;
            const int wy = wy0 + y;
            return fbm01(sElev, wx * 0.013f, wy * 0.013f, 5);
        };

        auto wetAt = [&](int x, int y) -> float {
            x = std::clamp(x, 1, d.width - 2);
            y = std::clamp(y, 1, d.height - 2);
            const size_t i = idx(x, y);
            if (i < wetField.size()) return wetField[i];
            const int wx = wx0 + x;
            const int wy = wy0 + y;
            return fbm01(sWet, wx * 0.011f, wy * 0.011f, 4);
        };

        auto varAt = [&](int x, int y) -> float {
            x = std::clamp(x, 1, d.width - 2);
            y = std::clamp(y, 1, d.height - 2);
            const size_t i = idx(x, y);
            if (i < varField.size()) return varField[i];
            return 0.5f;
        };

        auto bankRand01 = [&](int x, int y) -> float {
            // Deterministic hashed RNG in world space.
            return u32To01(hashCoord(sBrookBank, wx0 + x, wy0 + y));
        };

        auto inInterior = [&](int x, int y) -> bool {
            return (x > 0 && y > 0 && x < d.width - 1 && y < d.height - 1);
        };

        auto carveChasm = [&](int x, int y) -> bool {
            if (!inInterior(x, y)) return false;
            if (!farFromGates(x, y)) return false;

            Tile& t = d.at(x, y);
            if (t.type == TileType::Wall) return false;
            if (t.type == TileType::Fountain) return false;

            if (t.type != TileType::Chasm) {
                t.type = TileType::Chasm;
                d.fluvialChasmCount++;
                d.overworldBrookTiles++;
                return true;
            }
            return false;
        };

        auto maybePlaceBank = [&](int x, int y, float localWet) {
            if (!inInterior(x, y)) return;
            if (!farFromGates(x, y)) return;

            Tile& t = d.at(x, y);
            if (t.type != TileType::Floor) return;

            float p = bankChanceBase;
            p *= (0.30f + 0.95f * std::clamp(localWet, 0.0f, 1.0f));
            // Avoid super high elevation banks in mountainous areas (already reads "hard").
            const float e = elevAt(x, y);
            p *= (1.0f - std::clamp((e - 0.76f) * 1.6f, 0.0f, 0.85f));

            const float r0 = bankRand01(x, y);
            if (r0 > p) return;

            float treeBias = bankTreeBias;
            treeBias *= std::clamp((localWet - 0.42f) * 2.2f, 0.0f, 1.0f);

            // Second sample to choose tile kind.
            const float r1 = u32To01(hashCoord(sBrookBank ^ 0xBADC0DEu, (wx0 + x) + 41, (wy0 + y) - 17));
            const bool placeTree = (r1 < treeBias);

            const TileType tt = placeTree ? TileType::Pillar : TileType::Boulder;
            t.type = tt;
            if (tt == TileType::Pillar) d.heightfieldRidgePillarCount++;
            else d.heightfieldScreeBoulderCount++;
        };

        auto carvePond = [&](Vec2i c, int r) {
            r = std::clamp(r, 1, 4);
            int changed = 0;

            for (int yy = c.y - r; yy <= c.y + r; ++yy) {
                for (int xx = c.x - r; xx <= c.x + r; ++xx) {
                    if (!inInterior(xx, yy)) continue;
                    if (!farFromGates(xx, yy)) continue;

                    const int dx = xx - c.x;
                    const int dy = yy - c.y;
                    if (dx * dx + dy * dy > r * r) continue;

                    Tile& t = d.at(xx, yy);
                    if (t.type == TileType::Wall || t.type == TileType::Fountain) continue;

                    if (t.type != TileType::Chasm) {
                        t.type = TileType::Chasm;
                        d.fluvialChasmCount++;
                        d.overworldBrookTiles++;
                        changed++;
                    }
                }
            }

            if (changed > 0) {
                d.overworldPondCount++;

                // Sparse bank ring around the pond.
                for (int k = 0; k < 16; ++k) {
                    const int xx = c.x + brookRng.range(-(r + 1), r + 1);
                    const int yy = c.y + brookRng.range(-(r + 1), r + 1);
                    if (!inInterior(xx, yy)) continue;
                    if (d.at(xx, yy).type == TileType::Chasm) continue;
                    const float w = wetAt(xx, yy);
                    maybePlaceBank(xx, yy, w);
                }
            }
        };

        static const int DX8[8] = { 1, -1, 0, 0,  1, 1, -1, -1 };
        static const int DY8[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

        for (const Vec2i& spring : placedSprings) {
            if (!brookRng.chance(perSpringChance)) continue;

            // Pick a downhill-adjacent start tile (one step away from the Fountain).
            Vec2i start{-1, -1};
            float bestE = 999.0f;

            const float eSpring = elevAt(spring.x, spring.y);

            for (int k = 0; k < 8; ++k) {
                const int nx = spring.x + DX8[k];
                const int ny = spring.y + DY8[k];
                if (!inInterior(nx, ny)) continue;
                if (!farFromGates(nx, ny)) continue;

                const TileType tt = d.at(nx, ny).type;
                if (tt == TileType::Wall) continue;
                if (tt == TileType::Fountain) continue;

                const float e = elevAt(nx, ny);
                // Prefer lower elevation, but allow a slightly higher start on perfectly flat terrain.
                if (e <= eSpring + flatSlack && e < bestE) {
                    bestE = e;
                    start = {nx, ny};
                }
            }

            if (start.x < 0) continue;

            Vec2i prev = spring;
            Vec2i cur = start;
            Vec2i dir{cur.x - prev.x, cur.y - prev.y};
            dir.x = (dir.x > 0) ? 1 : (dir.x < 0 ? -1 : 0);
            dir.y = (dir.y > 0) ? 1 : (dir.y < 0 ? -1 : 0);

            std::vector<uint8_t> visited(static_cast<size_t>(d.width * d.height), uint8_t{0});

            int carvedTiles = 0;
            bool reachedWater = false;

            for (int step = 0; step < maxLen; ++step) {
                if (!inInterior(cur.x, cur.y)) break;
                if (!farFromGates(cur.x, cur.y)) break;

                const size_t vi = idx(cur.x, cur.y);
                if (vi < visited.size() && visited[vi]) break;
                if (vi < visited.size()) visited[vi] = 1u;

                const TileType here = d.at(cur.x, cur.y).type;
                if (here == TileType::Chasm) {
                    reachedWater = true;
                    break;
                }
                if (here == TileType::Wall || here == TileType::Fountain) break;

                const bool carved = carveChasm(cur.x, cur.y);
                if (carved) {
                    carvedTiles++;

                    // Riparian banks: very sparse around narrow brooks.
                    const float w0 = wetAt(cur.x, cur.y);
                    for (int k = 0; k < 4; ++k) {
                        const int nx = cur.x + DX8[k];
                        const int ny = cur.y + DY8[k];
                        if (!inInterior(nx, ny)) continue;
                        if (d.at(nx, ny).type == TileType::Chasm) continue;
                        if (!brookRng.chance(0.40f)) continue;
                        maybePlaceBank(nx, ny, w0);
                    }
                }

                const float e0 = elevAt(cur.x, cur.y);

                // Choose next step: minimal (elevation + penalties) neighbor within slack.
                Vec2i best{-1, -1};
                Vec2i bestDir{0, 0};
                float bestScore = 999.0f;
                bool foundRealDownhill = false;

                for (int k = 0; k < 8; ++k) {
                    const int nx = cur.x + DX8[k];
                    const int ny = cur.y + DY8[k];

                    if (!inInterior(nx, ny)) continue;
                    if (!farFromGates(nx, ny)) continue;
                    if (nx == prev.x && ny == prev.y) continue;

                    const TileType tt = d.at(nx, ny).type;
                    if (tt == TileType::Wall) continue;
                    if (tt == TileType::Fountain) continue;

                    const float e = elevAt(nx, ny);
                    const float rise = e - e0;

                    if (rise > flatSlack) continue;

                    if (e < e0 - minDrop) foundRealDownhill = true;

                    float score = e;

                    // Obstacle preference: streams tend to go around boulders/trees if possible.
                    if (tt == TileType::Pillar) score += 0.015f;
                    if (tt == TileType::Boulder) score += 0.020f;

                    // Mild inertia: penalize changing direction.
                    Vec2i nd{nx - cur.x, ny - cur.y};
                    nd.x = (nd.x > 0) ? 1 : (nd.x < 0 ? -1 : 0);
                    nd.y = (nd.y > 0) ? 1 : (nd.y < 0 ? -1 : 0);
                    if (nd.x != dir.x || nd.y != dir.y) score += turnPenalty;

                    // Prefer wetter micro-cells (a tiny pull toward valley wetness).
                    const float w = wetAt(nx, ny);
                    score -= w * 0.0030f;

                    // Deterministic jitter breaks ties.
                    score += (varAt(nx, ny) - 0.5f) * 0.0040f;

                    if (score < bestScore) {
                        bestScore = score;
                        best = {nx, ny};
                        bestDir = nd;
                    }
                }

                if (best.x < 0) {
                    // Nowhere to go within slack: create a tiny pond at this sink.
                    const float w = wetAt(cur.x, cur.y);
                    int r = 1;
                    if (w > 0.78f && brookRng.chance(0.70f)) r = 2;
                    if (w > 0.88f && brookRng.chance(0.40f)) r = 3;
                    if (biome == Biome::Desert && brookRng.chance(0.60f)) r = std::max(r, 2);
                    carvePond(cur, r);
                    break;
                }

                // If we haven't found any meaningful downhill for a while, treat this as a basin and pond out.
                if (!foundRealDownhill && step >= 10 && brookRng.chance(0.65f)) {
                    const float w = wetAt(cur.x, cur.y);
                    int r = (w > 0.80f) ? 2 : 1;
                    if (biome == Biome::Desert) r = std::max(r, 2);
                    carvePond(cur, r);
                    break;
                }

                prev = cur;
                cur = best;
                dir = bestDir;
            }

            if (carvedTiles > 0) {
                d.overworldBrookCount++;
                // If we never reached water, ensure at least a tiny terminal pool so the feature reads.
                if (!reachedWater && d.at(cur.x, cur.y).type != TileType::Chasm) {
                    const float w = wetAt(cur.x, cur.y);
                    const int r = (w > 0.80f) ? 2 : 1;
                    carvePond(cur, r);
                }
            }
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

    // Biome-aware trail width. Plains/coasts/swamps get slightly wider, more "road-like"
    // trails, while harsher biomes keep narrower paths.
    const int trailRadius = (biome == Biome::Plains || biome == Biome::Coast || biome == Biome::Swamp) ? 1 : 0;

    // Terrain-aware carving:
    //  * Always carve the *centerline* tile to Floor (so trails can cross rivers/mountains).
    //  * Only widen (radius) into non-Wall / non-Chasm tiles unless forceWide is requested.
    //    This preserves river/mountain silhouettes instead of bulldozing them into flat bands.
    auto carveTrailAt = [&](int x, int y, bool forceWide = false) {
        x = std::clamp(x, 1, d.width - 2);
        y = std::clamp(y, 1, d.height - 2);

        for (int dy = -trailRadius; dy <= trailRadius; ++dy) {
            for (int dx = -trailRadius; dx <= trailRadius; ++dx) {
                const int xx = x + dx;
                const int yy = y + dy;
                if (!d.inBounds(xx, yy)) continue;
                if (xx <= 0 || yy <= 0 || xx >= d.width - 1 || yy >= d.height - 1) continue;

                // Preserve water/river and mountain walls around the trail edges for readability.
                if (!forceWide && (dx != 0 || dy != 0)) {
                    const TileType tt = d.at(xx, yy).type;
                    if (tt == TileType::Wall || tt == TileType::Chasm) continue;
                }

                if (d.at(xx, yy).type == TileType::Chasm) d.fluvialCausewayCount++;
                // Preserve springs / fountains when a trail crosses them.
                if (d.at(xx, yy).type != TileType::Fountain) d.at(xx, yy).type = TileType::Floor;
            }
        }
    };

    // Small clearing at the hub (force wide to make it feel like a crossroads).
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            carveTrailAt(hub.x + dx, hub.y + dy, true);

    // Direction-aware A* ("road planner") for trails:
    // - Cost field prefers existing trails (Floor), avoids rivers (Chasm) and mountains (Wall),
    //   and discourages steep slopes (elevation gradient).
    // - State includes heading direction; turning costs extra, producing smoother paths.
    struct TrailAStarEntry {
        int f = 0;
        int g = 0;
        int state = 0;
    };
    struct TrailAStarCmp {
        bool operator()(const TrailAStarEntry& a, const TrailAStarEntry& b) const {
            return a.f > b.f; // min-heap by f via inverted comparator
        }
    };

    struct TrailPlanner {
        Dungeon& d;
        int W = 0;
        int H = 0;

        const std::vector<float>& elev;
        const std::vector<float>& wet;
        const std::vector<float>& var;
        Biome biome = Biome::Plains;

        int turnPenalty = 7;   // extra cost when changing direction
        int uTurnPenalty = 16; // extra cost for 180-degree reversals

        std::vector<int> gScore;
        std::vector<int> parent;
        std::vector<uint8_t> closed;

        TrailPlanner(Dungeon& dd,
                     const std::vector<float>& elevField,
                     const std::vector<float>& wetField,
                     const std::vector<float>& varField,
                     Biome b)
            : d(dd), W(dd.width), H(dd.height), elev(elevField), wet(wetField), var(varField), biome(b) {
            const size_t N = static_cast<size_t>(W * H * 4);
            gScore.assign(N, std::numeric_limits<int>::max());
            parent.assign(N, -1);
            closed.assign(N, uint8_t{0});

            // Biome-tuned smoothness: dense biomes bias smoother paths (less zig-zag),
            // while open biomes allow slightly more meander.
            switch (biome) {
                case Biome::Forest:    turnPenalty = 9;  uTurnPenalty = 20; break;
                case Biome::Swamp:     turnPenalty = 8;  uTurnPenalty = 20; break;
                case Biome::Highlands: turnPenalty = 8;  uTurnPenalty = 18; break;
                case Biome::Badlands:  turnPenalty = 8;  uTurnPenalty = 18; break;
                case Biome::Tundra:    turnPenalty = 7;  uTurnPenalty = 18; break;
                case Biome::Desert:    turnPenalty = 7;  uTurnPenalty = 18; break;
                case Biome::Coast:     turnPenalty = 6;  uTurnPenalty = 16; break;
                case Biome::Plains:
                default:               turnPenalty = 6;  uTurnPenalty = 16; break;
            }
        }

        inline bool inInterior(int x, int y) const {
            return x > 0 && y > 0 && x < W - 1 && y < H - 1;
        }

        inline size_t tidx(int x, int y) const {
            return static_cast<size_t>(y * W + x);
        }

        inline int stateIdx(int x, int y, int dir) const {
            return static_cast<int>(((y * W + x) * 4) + dir);
        }

        int tileCost(int x, int y) const {
            if (!inInterior(x, y)) return 1000000;
            const size_t i = tidx(x, y);

            // Base cost by current terrain.
            const TileType tt = d.at(x, y).type;
            int c = 10; // minimum step cost (keeps heuristic admissible)
            switch (tt) {
                case TileType::Floor:
                case TileType::DoorOpen:
                case TileType::DoorClosed:
                case TileType::DoorLocked:
                case TileType::DoorSecret:
                    c = 10;
                    break;
                case TileType::Pillar:
                    c = 24;
                    break;
                case TileType::Boulder:
                    c = 28;
                    break;
                case TileType::Chasm:
                    c = 130; // "river" / ravine
                    break;
                case TileType::Wall:
                default:
                    c = 240; // mountains/ruins walls are expensive, but still passable via carving
                    break;
            }

            // Slope penalty: discourage steep climbs by looking at local elevation gradient.
            if (!elev.empty() && i < elev.size()) {
                const float e0 = elev[i];
                float grad = 0.0f;

                auto samp = [&](int sx, int sy) -> float {
                    const size_t si = tidx(sx, sy);
                    if (si >= elev.size()) return e0;
                    return elev[si];
                };

                if (inInterior(x - 1, y)) grad = std::max(grad, std::abs(samp(x - 1, y) - e0));
                if (inInterior(x + 1, y)) grad = std::max(grad, std::abs(samp(x + 1, y) - e0));
                if (inInterior(x, y - 1)) grad = std::max(grad, std::abs(samp(x, y - 1) - e0));
                if (inInterior(x, y + 1)) grad = std::max(grad, std::abs(samp(x, y + 1) - e0));

                // grad is typically small (noise is smooth); amplify to make it matter.
                c += static_cast<int>(std::round(grad * 140.0f));
            }

            // Wetness penalty: avoid the wettest areas unless we're already in a swamp biome.
            if (!wet.empty() && i < wet.size()) {
                const float w = wet[i];
                const float threshold = (biome == Biome::Swamp) ? 0.62f : 0.72f;
                const float k = (biome == Biome::Swamp) ? 18.0f : 26.0f;
                if (w > threshold) c += static_cast<int>(std::round((w - threshold) * k));
            }

            // Deterministic micro-jitter breaks ties and prevents perfectly mirrored paths.
            // Keep it small and never let it drop below the minimum base step cost.
            if (!var.empty() && i < var.size()) {
                const float r = var[i];
                const int jitter = static_cast<int>(std::round((r - 0.5f) * 6.0f)); // ~[-3, +3]
                c += jitter;
            }

            c = std::max(10, c);
            return c;
        }

        bool findPath(Vec2i start, Vec2i goal, std::vector<Vec2i>& out) {
            if (!inInterior(start.x, start.y)) return false;
            if (!inInterior(goal.x, goal.y)) return false;

            const size_t N = static_cast<size_t>(W * H * 4);
            std::fill(gScore.begin(), gScore.end(), std::numeric_limits<int>::max());
            std::fill(parent.begin(), parent.end(), -1);
            std::fill(closed.begin(), closed.end(), uint8_t{0});

            auto heuristic = [&](int x, int y) -> int {
                const int md = std::abs(goal.x - x) + std::abs(goal.y - y);
                return md * 10; // admissible lower bound (min step cost)
            };

            std::priority_queue<TrailAStarEntry, std::vector<TrailAStarEntry>, TrailAStarCmp> open;

            // Seed all 4 heading directions at the start; the optimal one emerges naturally.
            for (int dir = 0; dir < 4; ++dir) {
                const int s = stateIdx(start.x, start.y, dir);
                gScore[static_cast<size_t>(s)] = 0;
                open.push({heuristic(start.x, start.y), 0, s});
            }

            static const int DX[4] = { 1, -1, 0, 0 };
            static const int DY[4] = { 0, 0, 1, -1 };
            static const int OPP[4] = { 1, 0, 3, 2 };

            int goalState = -1;

            while (!open.empty()) {
                const TrailAStarEntry cur = open.top();
                open.pop();

                if (cur.state < 0 || static_cast<size_t>(cur.state) >= N) continue;
                if (cur.g != gScore[static_cast<size_t>(cur.state)]) continue;
                if (closed[static_cast<size_t>(cur.state)]) continue;
                closed[static_cast<size_t>(cur.state)] = 1;

                const int pos = cur.state / 4;
                const int dir = cur.state % 4;
                const int x = pos % W;
                const int y = pos / W;

                if (x == goal.x && y == goal.y) {
                    goalState = cur.state;
                    break;
                }

                for (int nd = 0; nd < 4; ++nd) {
                    const int nx = x + DX[nd];
                    const int ny = y + DY[nd];
                    if (!inInterior(nx, ny)) continue;

                    int step = tileCost(nx, ny);
                    if (nd != dir) step += turnPenalty;
                    if (nd == OPP[dir]) step += uTurnPenalty;

                    const int ns = stateIdx(nx, ny, nd);
                    const int ng = cur.g + step;

                    if (ng < gScore[static_cast<size_t>(ns)]) {
                        gScore[static_cast<size_t>(ns)] = ng;
                        parent[static_cast<size_t>(ns)] = cur.state;
                        open.push({ng + heuristic(nx, ny), ng, ns});
                    }
                }
            }

            if (goalState < 0) return false;

            out.clear();
            int s = goalState;
            while (s >= 0) {
                const int pos = s / 4;
                out.push_back({pos % W, pos / W});
                s = parent[static_cast<size_t>(s)];
            }
            std::reverse(out.begin(), out.end());

            return !out.empty();
        }
    };

    TrailPlanner planner(d, elevField, wetField, varField, biome);

    std::vector<Vec2i> trailPath;
    trailPath.reserve(static_cast<size_t>(d.width + d.height));

    auto walkTrail = [&](Vec2i start) {
        Vec2i s = start;
        s.x = std::clamp(s.x, 1, d.width - 2);
        s.y = std::clamp(s.y, 1, d.height - 2);

        trailPath.clear();
        if (planner.findPath(s, hub, trailPath)) {
            for (const Vec2i& p : trailPath) carveTrailAt(p.x, p.y);
            return;
        }

        // Fallback (extremely rare): the old greedy meander.
        Vec2i p = s;
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

    walkTrail(startN);
    walkTrail(startS);
    walkTrail(startW);
    walkTrail(startE);

    // If a rare stronghold setpiece exists in this chunk, cut a spur that
    // reliably connects its entrance back to the hub/trail network.
    // This keeps the POI reachable even when mountains/water surround it.
    if (hasStronghold && d.inBounds(strongholdApproach.x, strongholdApproach.y)) {
        carveTrailAt(strongholdApproach.x, strongholdApproach.y, true);
        walkTrail(strongholdApproach);
    }

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
                        // Preserve minor POIs (springs) by refusing to build over them.
                        if (t == TileType::Fountain) bad += 2;
                        if (t == TileType::Wall) mountain += 1;
                    }
                }

                // Don't pave over rivers/lakes/springs.
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
                    walkTrail(approach);
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

    if (hasStronghold && strongholdVault.w > 0 && strongholdVault.h > 0) {
        d.rooms.push_back(strongholdVault);
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
