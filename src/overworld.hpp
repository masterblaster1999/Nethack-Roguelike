#pragma once

#include "dungeon.hpp"
#include "rng.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// -----------------------------------------------------------------------------
// Infinite overworld chunks (Camp branch, depth 0)
// -----------------------------------------------------------------------------
//
// The core idea: keep the surface camp as a "hub" at overworld (0,0), then let the
// player step through edge gates to travel to adjacent wilderness chunks.
//
// Chunks are generated deterministically from (runSeed, chunkX, chunkY) and are
// intentionally decoupled from the gameplay RNG stream.
//
namespace overworld {

struct ChunkGates {
    Vec2i north{-1, -1};
    Vec2i south{-1, -1};
    Vec2i west{-1, -1};
    Vec2i east{-1, -1};
};

inline uint32_t chunkSeed(uint32_t runSeed, int chunkX, int chunkY) {
    // Domain-separate from other seed uses.
    uint32_t s = runSeed;
    s = hashCombine(s, 0x0B1ECA5Eu);
    s = hashCombine(s, static_cast<uint32_t>(chunkX));
    s = hashCombine(s, static_cast<uint32_t>(chunkY));
    if (s == 0u) s = 1u;
    return s;
}

inline ChunkGates gatePositions(const Dungeon& d) {
    ChunkGates g;
    const int w = std::max(1, d.width);
    const int h = std::max(1, d.height);
    g.north = {w / 2, 0};
    g.south = {w / 2, h - 1};
    g.west  = {0, h / 2};
    g.east  = {w - 1, h / 2};
    return g;
}

inline void ensureBorderWalls(Dungeon& d) {
    if (d.width <= 0 || d.height <= 0) return;

    for (int x = 0; x < d.width; ++x) {
        d.at(x, 0).type = TileType::Wall;
        d.at(x, d.height - 1).type = TileType::Wall;
    }
    for (int y = 0; y < d.height; ++y) {
        d.at(0, y).type = TileType::Wall;
        d.at(d.width - 1, y).type = TileType::Wall;
    }
}

inline void carveFloor(Dungeon& d, Vec2i p) {
    if (!d.inBounds(p.x, p.y)) return;
    d.at(p.x, p.y).type = TileType::Floor;
}

inline void ensureBorderGates(Dungeon& d) {
    // Open 1-tile gates (plus a 1-tile "throat" inside) at edge midpoints.
    // Out-of-bounds movement through these gates is interpreted as chunk travel.
    const ChunkGates g = gatePositions(d);

    carveFloor(d, g.north);
    carveFloor(d, {g.north.x, g.north.y + 1});

    carveFloor(d, g.south);
    carveFloor(d, {g.south.x, g.south.y - 1});

    carveFloor(d, g.west);
    carveFloor(d, {g.west.x + 1, g.west.y});

    carveFloor(d, g.east);
    carveFloor(d, {g.east.x - 1, g.east.y});
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

inline float smoothstep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

inline float u32To01(uint32_t v) {
    // [0,1)
    return (v / (static_cast<float>(std::numeric_limits<uint32_t>::max()) + 1.0f));
}

inline uint32_t hashCoord(uint32_t seed, int x, int y) {
    uint32_t h = seed;
    h = hashCombine(h, static_cast<uint32_t>(x));
    h = hashCombine(h, static_cast<uint32_t>(y));
    return hash32(h);
}

inline float valueNoise01(uint32_t seed, float x, float y) {
    // 2D value noise in [0,1].
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;

    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const float sx = smoothstep(tx);
    const float sy = smoothstep(ty);

    const float v00 = u32To01(hashCoord(seed, x0, y0));
    const float v10 = u32To01(hashCoord(seed, x1, y0));
    const float v01 = u32To01(hashCoord(seed, x0, y1));
    const float v11 = u32To01(hashCoord(seed, x1, y1));

    const float a = lerp(v00, v10, sx);
    const float b = lerp(v01, v11, sx);
    return lerp(a, b, sy);
}

inline float fbm01(uint32_t seed, float x, float y, int octaves) {
    // Fractal brownian motion (fbm) over value noise, returns [0,1].
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    float norm = 0.0f;

    const int o = std::clamp(octaves, 1, 8);

    for (int i = 0; i < o; ++i) {
        const uint32_t s = hashCombine(seed, static_cast<uint32_t>(i) * 0x9E3779B9u);
        const float v = valueNoise01(s, x * freq, y * freq);
        sum += v * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }

    if (norm <= 0.0f) return 0.0f;
    return sum / norm;
}

inline void generateWildernessChunk(Dungeon& d, uint32_t runSeed, int chunkX, int chunkY) {
    // A simple outdoor generator: mountains (Wall), water (Chasm), trees (Pillar), rocks (Boulder).
    // A guaranteed cross-road is carved through the center so gates are always mutually reachable.

    d.rooms.clear();

    // Outdoors has no stairs by default.
    d.stairsUp = {-1, -1};
    d.stairsDown = {-1, -1};

    // Clear camp-only landmark hints.
    d.campStashSpot = {-1, -1};
    d.campGateIn = {-1, -1};
    d.campGateOut = {-1, -1};
    d.campSideGateIn = {-1, -1};
    d.campSideGateOut = {-1, -1};
    d.campWellSpot = {-1, -1};
    d.campFireSpot = {-1, -1};
    d.campAltarSpot = {-1, -1};

    if (d.width <= 2 || d.height <= 2) {
        // Degenerate case: just walls + gates.
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                d.at(x, y).type = TileType::Floor;
            }
        }
        ensureBorderWalls(d);
        ensureBorderGates(d);
        if (d.width >= 3 && d.height >= 3) {
            Room r;
            r.x = 1;
            r.y = 1;
            r.w = std::max(1, d.width - 2);
            r.h = std::max(1, d.height - 2);
            r.type = RoomType::Normal;
            d.rooms.push_back(r);
        }
        return;
    }

    const uint32_t s = chunkSeed(runSeed, chunkX, chunkY);
    const uint32_t sElev = hashCombine(s, 0xE1E7E1E7u);
    const uint32_t sWet = hashCombine(s, 0x77A7BEEFu);
    const uint32_t sVar = hashCombine(s, 0xC0FFEE11u);

    // Base fill.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
            d.at(x, y).visible = false;
            d.at(x, y).explored = false;
        }
    }

    // Terrain.
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            const int wx = chunkX * d.width + x;
            const int wy = chunkY * d.height + y;

            const float elev = fbm01(sElev, wx * 0.013f, wy * 0.013f, 5);
            const float wet = fbm01(sWet, wx * 0.021f + 17.0f, wy * 0.021f - 29.0f, 4);
            const float var = u32To01(hashCoord(sVar, wx, wy));

            // Water basins.
            if (elev < 0.22f && wet > 0.42f) {
                d.at(x, y).type = TileType::Chasm;
                continue;
            }

            // Mountains.
            if (elev > 0.83f) {
                d.at(x, y).type = TileType::Wall;
                continue;
            }

            // Forest.
            if (wet > 0.66f && elev < 0.78f) {
                if (var < 0.28f) {
                    d.at(x, y).type = TileType::Pillar;
                    continue;
                }
            }

            // Rocky outcrops.
            if (elev > 0.72f && var < 0.05f) {
                d.at(x, y).type = TileType::Boulder;
                continue;
            }

            // Sparse deadwood even in dry areas.
            if (wet < 0.24f && var < 0.015f) {
                d.at(x, y).type = TileType::Pillar;
                continue;
            }

            d.at(x, y).type = TileType::Floor;
        }
    }

    // Guaranteed cross-road to keep chunks readable and connected.
    const int cx = d.width / 2;
    const int cy = d.height / 2;
    for (int x = 0; x < d.width; ++x) {
        for (int dy = -1; dy <= 1; ++dy) {
            Vec2i p{x, cy + dy};
            if (!d.inBounds(p.x, p.y)) continue;
            d.at(p.x, p.y).type = TileType::Floor;
        }
    }
    for (int y = 0; y < d.height; ++y) {
        for (int dx = -1; dx <= 1; ++dx) {
            Vec2i p{cx + dx, y};
            if (!d.inBounds(p.x, p.y)) continue;
            d.at(p.x, p.y).type = TileType::Floor;
        }
    }

    // Borders and gates.
    ensureBorderWalls(d);
    ensureBorderGates(d);

    // Provide one big "interior" room for spawn systems.
    Room r;
    r.x = 1;
    r.y = 1;
    r.w = d.width - 2;
    r.h = d.height - 2;
    r.type = RoomType::Normal;
    d.rooms.push_back(r);
}

} // namespace overworld
