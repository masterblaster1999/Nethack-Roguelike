#include "spritegen.hpp"
#include "game.hpp"
#include <algorithm>
#include <cmath>

namespace {

inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

Color mul(Color c, float f) {
    return {
        clamp8(static_cast<int>(std::lround(c.r * f))),
        clamp8(static_cast<int>(std::lround(c.g * f))),
        clamp8(static_cast<int>(std::lround(c.b * f))),
        c.a
    };
}

Color add(Color c, int dr, int dg, int db) {
    return {
        clamp8(static_cast<int>(c.r) + dr),
        clamp8(static_cast<int>(c.g) + dg),
        clamp8(static_cast<int>(c.b) + db),
        c.a
    };
}

SpritePixels makeSprite(int w, int h, Color bg = {0,0,0,0}) {
    SpritePixels s;
    s.w = w;
    s.h = h;
    s.px.assign(static_cast<size_t>(w * h), bg);
    return s;
}

void put(SpritePixels& s, int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
    s.at(x, y) = c;
}

Color baseColorFor(EntityKind kind) {
    switch (kind) {
        case EntityKind::Player: return { 70, 140, 255, 255 };
        case EntityKind::Goblin: return { 90, 200,  90, 255 };
        case EntityKind::Bat:    return { 190,  90, 230, 255 };
        case EntityKind::Slime:  return {  70, 210, 180, 255 };
        default: return { 200, 200, 200, 255 };
    }
}

float densityFor(EntityKind kind) {
    switch (kind) {
        case EntityKind::Player: return 0.55f;
        case EntityKind::Goblin: return 0.50f;
        case EntityKind::Bat:    return 0.42f;
        case EntityKind::Slime:  return 0.60f;
        default: return 0.50f;
    }
}

} // namespace

SpritePixels generateEntitySprite(EntityKind kind, uint32_t seed) {
    RNG rng(hash32(seed));

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // 8x8 mask, mirrored horizontally.
    bool m[8][8] = {};
    int filled = 0;

    float density = densityFor(kind);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) {
            bool on = rng.chance(density);
            m[y][x] = on;
            m[y][7 - x] = on;
            if (on) filled += 2;
        }
    }

    // Ensure we don't end up with a nearly-empty sprite.
    if (filled < 14) {
        for (int i = 0; i < 10; ++i) {
            int x = rng.range(1, 6);
            int y = rng.range(1, 6);
            m[y][x] = true;
        }
    }

    Color base = baseColorFor(kind);
    // Small per-sprite tint variation:
    base = add(base, rng.range(-18, 18), rng.range(-18, 18), rng.range(-18, 18));

    // Fill body
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            if (!m[y][x]) continue;

            // Vertical shading: lighter on top, darker on bottom.
            float shade = 1.18f - (y / 8.0f) * 0.35f;
            // Small noise variation.
            shade += (rng.next01() - 0.5f) * 0.10f;

            Color c = mul(base, shade);
            c.a = 255;

            // Scale to 16x16 by painting a 2x2 block.
            int ox = x * 2;
            int oy = y * 2;
            put(s, ox + 0, oy + 0, c);
            put(s, ox + 1, oy + 0, c);
            put(s, ox + 0, oy + 1, c);
            put(s, ox + 1, oy + 1, c);
        }
    }

    // Outline: paint around non-transparent pixels.
    Color outline = mul(base, 0.35f);
    outline.a = 255;

    SpritePixels out = makeSprite(16, 16, {0,0,0,0});
    out.px = s.px;

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (s.at(x, y).a == 0) continue;

            static const int N[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
            for (auto& d : N) {
                int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= 16 || ny >= 16) continue;
                if (s.at(nx, ny).a == 0) {
                    out.at(nx, ny) = outline;
                }
            }
        }
    }

    s = std::move(out);

    // Add simple eyes for monsters (not player).
    if (kind != EntityKind::Player) {
        Color eye = {240, 240, 240, 255};
        Color pupil = {20, 20, 20, 255};
        int ey = 6;
        int ex1 = 6;
        int ex2 = 9;
        put(s, ex1, ey, eye);
        put(s, ex2, ey, eye);
        put(s, ex1, ey+1, pupil);
        put(s, ex2, ey+1, pupil);
    } else {
        // Player "visor"
        Color visor = {230, 230, 230, 255};
        put(s, 7, 5, visor);
        put(s, 8, 5, visor);
    }

    return s;
}

SpritePixels generateFloorTile(uint32_t seed) {
    RNG rng(hash32(seed));
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});

    Color base = { 92, 82, 64, 255 };
    base = add(base, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint32_t n = hashCombine(seed, static_cast<uint32_t>(x + y * 16));
            float noise = ((n & 0xFFu) / 255.0f);
            float f = 0.85f + noise * 0.35f;

            // Slight vignette
            float cx = (x - 7.5f) / 7.5f;
            float cy = (y - 7.5f) / 7.5f;
            float v = 1.0f - 0.10f * (cx*cx + cy*cy);
            f *= v;

            Color c = mul(base, f);

            // Occasional pebble
            if ((n % 97u) == 0u) c = mul(c, 1.35f);
            if ((n % 193u) == 0u) c = mul(c, 0.65f);

            s.at(x, y) = c;
        }
    }

    return s;
}

SpritePixels generateWallTile(uint32_t seed) {
    RNG rng(hash32(seed));
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});

    Color brick = { 84, 84, 96, 255 };
    brick = add(brick, rng.range(-8, 8), rng.range(-8, 8), rng.range(-8, 8));
    Color mortar = mul(brick, 0.55f);
    mortar.a = 255;

    for (int y = 0; y < 16; ++y) {
        int row = y / 4;
        int offset = (row % 2) ? 4 : 0;

        for (int x = 0; x < 16; ++x) {
            bool isMortar = false;
            if (y % 4 == 0) isMortar = true;
            int bx = (x + offset) % 8;
            if (bx == 0) isMortar = true;

            uint32_t n = hashCombine(seed ^ 0xBEEFu, static_cast<uint32_t>(x + y * 16));
            float noise = ((n & 0xFFu) / 255.0f);
            float f = 0.80f + noise * 0.30f;

            Color c = isMortar ? mortar : mul(brick, f);
            // Darker at bottom
            c = mul(c, 1.05f - (y / 16.0f) * 0.25f);
            s.at(x, y) = c;
        }
    }

    // Add a couple subtle chips
    for (int i = 0; i < 8; ++i) {
        int x = rng.range(1, 14);
        int y = rng.range(1, 14);
        s.at(x, y) = mul(s.at(x, y), 0.6f);
    }

    return s;
}

SpritePixels generateStairsTile(uint32_t seed) {
    SpritePixels s = generateFloorTile(seed ^ 0x1234abcd);

    // Draw some "steps" going down-right.
    Color step = { 210, 210, 210, 255 };
    Color shadow = { 40, 40, 40, 200 };

    int x0 = 4;
    int y0 = 4;

    for (int i = 0; i < 4; ++i) {
        int y = y0 + i * 3;
        for (int x = x0 + i * 2; x < 13; ++x) {
            put(s, x, y, step);
            put(s, x, y + 1, mul(step, 0.85f));
        }
        // shadow under each step
        for (int x = x0 + i * 2; x < 13; ++x) {
            put(s, x, y + 2, shadow);
        }
    }

    // Down arrow
    for (int y = 10; y <= 12; ++y) {
        put(s, 12, y, step);
    }
    put(s, 11, 12, step);
    put(s, 13, 12, step);

    return s;
}
