#include "spritegen.hpp"
#include "game.hpp"      // EntityKind
#include "items.hpp"     // ItemKind, ProjectileKind
#include <algorithm>
#include <cmath>

namespace {

inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

Color add(Color c, int dr, int dg, int db) {
    return { clamp8(static_cast<int>(c.r) + dr),
             clamp8(static_cast<int>(c.g) + dg),
             clamp8(static_cast<int>(c.b) + db),
             c.a };
}

Color mul(Color c, float f) {
    return {
        clamp8(static_cast<int>(std::lround(c.r * f))),
        clamp8(static_cast<int>(std::lround(c.g * f))),
        clamp8(static_cast<int>(std::lround(c.b * f))),
        c.a
    };
}

SpritePixels makeSprite(int w, int h, Color fill) {
    SpritePixels s;
    s.w = w; s.h = h;
    s.px.assign(static_cast<size_t>(w * h), fill);
    return s;
}

void setPx(SpritePixels& s, int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
    s.at(x, y) = c;
}

[[maybe_unused]] void blendPx(SpritePixels& s, int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
    Color& dst = s.at(x, y);
    float a = c.a / 255.0f;
    dst.r = clamp8(static_cast<int>(std::lround(dst.r * (1.0f - a) + c.r * a)));
    dst.g = clamp8(static_cast<int>(std::lround(dst.g * (1.0f - a) + c.g * a)));
    dst.b = clamp8(static_cast<int>(std::lround(dst.b * (1.0f - a) + c.b * a)));
    dst.a = 255;
}

void rect(SpritePixels& s, int x, int y, int w, int h, Color c) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            setPx(s, xx, yy, c);
}

void outlineRect(SpritePixels& s, int x, int y, int w, int h, Color c) {
    for (int xx = x; xx < x + w; ++xx) {
        setPx(s, xx, y, c);
        setPx(s, xx, y + h - 1, c);
    }
    for (int yy = y; yy < y + h; ++yy) {
        setPx(s, x, yy, c);
        setPx(s, x + w - 1, yy, c);
    }
}

void line(SpritePixels& s, int x0, int y0, int x1, int y1, Color c) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        setPx(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void circle(SpritePixels& s, int cx, int cy, int r, Color c) {
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx*dx + dy*dy <= r*r) setPx(s, x, y, c);
        }
    }
}

float densityFor(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return 0.55f;
        case EntityKind::Goblin: return 0.58f;
        case EntityKind::Orc: return 0.62f;
        case EntityKind::Bat: return 0.40f;
        case EntityKind::Slime: return 0.70f;
        case EntityKind::SkeletonArcher: return 0.52f;
        case EntityKind::KoboldSlinger: return 0.50f;
        case EntityKind::Wolf: return 0.55f;
        case EntityKind::Troll: return 0.68f;
        case EntityKind::Wizard: return 0.50f;
        case EntityKind::Snake: return 0.48f;
        case EntityKind::Spider: return 0.46f;
        case EntityKind::Ogre: return 0.72f;
        case EntityKind::Mimic: return 0.74f;
        case EntityKind::Shopkeeper: return 0.54f;
        case EntityKind::Minotaur: return 0.76f;
        default: return 0.55f;
    }
}

Color baseColorFor(EntityKind k, RNG& rng) {
    switch (k) {
        case EntityKind::Player: return add({ 160, 200, 255, 255 }, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
        case EntityKind::Goblin: return add({ 80, 180, 90, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Orc: return add({ 70, 150, 60, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Bat: return add({ 120, 100, 140, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Slime: return add({ 70, 200, 160, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::SkeletonArcher: return add({ 200, 200, 190, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
        case EntityKind::KoboldSlinger: return add({ 180, 120, 70, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
        case EntityKind::Wolf: return add({ 150, 150, 160, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Troll: return add({ 90, 170, 90, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Wizard: return add({ 140, 100, 200, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Snake: return add({ 80, 190, 100, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Spider: return add({ 80, 80, 95, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
        case EntityKind::Ogre: return add({ 150, 120, 70, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Mimic: return add({ 150, 110, 60, 255 }, rng.range(-18,18), rng.range(-18,18), rng.range(-18,18));
        case EntityKind::Shopkeeper: return add({ 220, 200, 120, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
        case EntityKind::Minotaur: return add({ 160, 90, 60, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        default: return add({ 180, 180, 180, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
    }
}

} // namespace

SpritePixels generateEntitySprite(EntityKind kind, uint32_t seed, int frame) {
    // Base shape from seed (stable), subtle variation from frame.
    RNG rngBase(hash32(seed));
    RNG rngVar(hashCombine(seed, static_cast<uint32_t>(0xA5F00Du + frame * 1337u)));

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // 8x8 mask, mirrored horizontally.
    bool m[8][8] = {};
    float density = densityFor(kind);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) {
            bool on = rngBase.chance(density);
            m[y][x] = on;
            m[y][7 - x] = on;
        }
    }

    Color base = baseColorFor(kind, rngBase);

    // Expand mask into 16x16 with chunky pixels.
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            if (!m[y][x]) continue;
            int px = x * 2;
            int py = y * 2;
            Color c = base;

            // Light shading top-left
            float shade = 0.90f + 0.20f * ((7 - y) / 7.0f);
            // Frame shimmer
            if (frame % 2 == 1) {
                float n = (hashCombine(seed, static_cast<uint32_t>(x + y * 11 + frame * 97)) & 0xFFu) / 255.0f;
                shade *= (0.92f + 0.12f * n);
            }
            c = mul(c, shade);

            rect(s, px, py, 2, 2, c);
        }
    }

    // Add eyes-ish for living things
    if (kind != EntityKind::Slime) {
        int ey = 6 + (rngVar.range(-1, 1));
        int ex = 6;
        setPx(s, ex, ey, {255,255,255,255});
        setPx(s, ex+3, ey, {255,255,255,255});
        setPx(s, ex, ey+1, {0,0,0,255});
        setPx(s, ex+3, ey+1, {0,0,0,255});
    } else {
        // Slime: two bright blobs
        setPx(s, 6, 7, {230,255,255,200});
        setPx(s, 9, 7, {230,255,255,200});
    }

    // Kind-specific little accents
    if (kind == EntityKind::Bat) {
        // Wing flaps (frame toggles)
        int y = (frame % 2 == 0) ? 6 : 7;
        setPx(s, 1, y, mul(base, 0.6f));
        setPx(s, 14, y, mul(base, 0.6f));
    }
    if (kind == EntityKind::SkeletonArcher) {
        // A tiny bow line
        line(s, 12, 6, 12, 11, {120,80,40,255});
        line(s, 11, 6, 13, 11, {160,160,160,255});
    }
    if (kind == EntityKind::KoboldSlinger) {
        // Sling dot
        setPx(s, 12, 10, {60,40,30,255});
        setPx(s, 13, 9, {200,200,200,255});
    }
    if (kind == EntityKind::Wolf) {
        // Nose
        setPx(s, 8, 10, {30,30,30,255});
    }

    if (kind == EntityKind::Troll) {
        // Tusks + snout
        setPx(s, 7, 11, {240,240,240,255});
        setPx(s, 9, 11, {240,240,240,255});
        setPx(s, 8, 10, {30,30,30,255});
    }
    if (kind == EntityKind::Wizard) {
        // Simple hat + sparkle
        Color hat = mul(base, 0.55f);
        rect(s, 5, 4, 6, 1, hat);
        rect(s, 6, 1, 4, 4, mul(base, 0.65f));
        if (frame % 2 == 1) setPx(s, 9, 2, {255,255,255,140});
    }

    if (kind == EntityKind::Snake) {
        // Tiny tongue + a couple darker scale stripes
        setPx(s, 8, 11, {220,80,80,255});
        setPx(s, 9, 11, {220,80,80,255});
        Color stripe = mul(base, 0.55f);
        for (int x = 4; x <= 11; x += 2) {
            setPx(s, x, 9, stripe);
        }
    }
    if (kind == EntityKind::Spider) {
        // Legs
        Color leg = {20,20,20,255};
        for (int x = 3; x <= 12; x += 3) {
            setPx(s, x, 11, leg);
            setPx(s, x, 12, leg);
        }
        // Extra eyes
        setPx(s, 6, 6, {255,255,255,255});
        setPx(s, 9, 6, {255,255,255,255});
    }

    if (kind == EntityKind::Ogre) {
        // Horns + belt
        Color horn = {240,240,240,255};
        setPx(s, 6, 2, horn);
        setPx(s, 9, 2, horn);
        rect(s, 5, 11, 6, 1, {60,40,20,255});
    }

    if (kind == EntityKind::Minotaur) {
        // Big horns + nose ring
        Color horn = {245,245,245,255};
        setPx(s, 5, 2, horn);
        setPx(s, 10, 2, horn);
        setPx(s, 4, 3, horn);
        setPx(s, 11, 3, horn);

        // Snout / ring shimmer
        setPx(s, 8, 10, {30,30,30,255});
        if (frame % 2 == 1) setPx(s, 8, 11, {255,220,160,180});

        // Simple belt
        rect(s, 5, 12, 6, 1, {80,50,30,255});
    }

    if (kind == EntityKind::Mimic) {
        // Chest-like bands + a toothy maw.
        Color band = mul(base, 0.55f);
        rect(s, 4, 8, 8, 1, band);
        rect(s, 4, 9, 8, 1, mul(band, 0.90f));

        Color maw = {25, 18, 12, 255};
        rect(s, 5, 10, 6, 3, maw);

        // Teeth
        for (int x = 5; x <= 10; x += 2) {
            setPx(s, x, 10, {245, 245, 245, 255});
        }

        // Tongue highlight
        setPx(s, 7, 12, {200, 70, 70, 255});
        setPx(s, 8, 12, {200, 70, 70, 255});

        // Little latch / glint
        setPx(s, 8, 8, {230, 200, 80, 255});
    }

    // Soft outline (helps readability)
    for (int y = 1; y < 15; ++y) {
        for (int x = 1; x < 15; ++x) {
            Color c = s.at(x, y);
            if (c.a == 0) continue;
            // If neighbor transparent, darken edge
            bool edge = false;
            if (s.at(x-1, y).a == 0) edge = true;
            if (s.at(x+1, y).a == 0) edge = true;
            if (s.at(x, y-1).a == 0) edge = true;
            if (s.at(x, y+1).a == 0) edge = true;
            if (edge) {
                s.at(x, y) = mul(c, 0.85f);
            }
        }
    }

    return s;
}

SpritePixels generateItemSprite(ItemKind kind, uint32_t seed, int frame) {
    RNG rng(hash32(seed));
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    auto sparkle = [&]() {
        if (frame % 2 == 1) {
            int x = rng.range(2, 13);
            int y = rng.range(2, 13);
            setPx(s, x, y, {255,255,255,200});
        }
    };

    switch (kind) {
        case ItemKind::Dagger: {
            Color steel = add({200,200,210,255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
            Color hilt = {120,80,40,255};
            line(s, 8, 2, 8, 12, steel);
            line(s, 7, 3, 7, 11, mul(steel, 0.85f));
            rect(s, 6, 12, 5, 2, hilt);
            setPx(s, 8, 1, {255,255,255,255});
            sparkle();
            break;
        }
        case ItemKind::Sword: {
            Color steel = add({210,210,220,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color hilt = {130,90,45,255};
            line(s, 8, 1, 8, 12, steel);
            line(s, 7, 2, 7, 11, mul(steel, 0.85f));
            rect(s, 5, 12, 7, 2, hilt);
            rect(s, 7, 14, 3, 1, {90,60,30,255});
            sparkle();
            break;
        }
        case ItemKind::Axe: {
            Color steel = add({210,210,220,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color wood  = add({130,90,45,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            // Handle
            line(s, 8, 3, 8, 14, wood);
            line(s, 7, 4, 7, 13, mul(wood, 0.85f));
            // Head
            rect(s, 6, 3, 4, 3, steel);
            rect(s, 5, 4, 2, 2, mul(steel, 0.85f));
            // Highlight
            setPx(s, 9, 3, {255,255,255,200});
            sparkle();
            break;
        }

        case ItemKind::Pickaxe: {
            Color steel = add({210,210,220,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color wood  = add({125,85,40,255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
            // Handle
            line(s, 8, 3, 8, 14, wood);
            line(s, 7, 4, 7, 13, mul(wood, 0.85f));
            // Pick head (cross)
            rect(s, 5, 4, 7, 2, steel);
            rect(s, 6, 3, 5, 1, mul(steel, 0.85f));
            // Highlight
            setPx(s, 10, 4, {255,255,255,200});
            sparkle();
            break;
        }

        case ItemKind::Bow: {
            Color wood = add({150,100,50,255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
            // Simple arc
            for (int y = 3; y <= 13; ++y) {
                int dx = (y < 8) ? (8 - y) / 2 : (y - 8) / 2;
                setPx(s, 6 - dx, y, wood);
                setPx(s, 10 + dx, y, wood);
            }
            line(s, 6, 3, 6, 13, mul(wood, 0.8f));
            line(s, 10, 3, 10, 13, mul(wood, 0.8f));
            // String
            line(s, 6, 3, 10, 13, {220,220,220,255});
            sparkle();
            break;
        }
        case ItemKind::WandSparks: {
            Color stick = add({120,90,60,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color gem = {120,220,255,255};
            line(s, 4, 12, 12, 4, stick);
            rect(s, 11, 3, 3, 3, gem);
            if (frame % 2 == 1) {
                setPx(s, 14, 4, {255,255,255,200});
                setPx(s, 12, 2, {255,255,255,200});
            }
            break;
        }

        case ItemKind::WandDigging: {
            Color stick = add({120,80,45,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color gem   = add({170,120,60,255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
            rect(s, 7, 3, 3, 10, stick);
            rect(s, 6, 2, 5, 2, gem);
            // Small rune on the shaft
            setPx(s, 8, 8, {240,210,160,200});
            sparkle();
            break;
        }

        case ItemKind::WandFireball: {
            Color stick = add({110,75,45,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color gem   = add({255,120,60,255}, rng.range(-20,20), rng.range(-10,10), rng.range(-10,10));
            // Diagonal wand with a fiery head.
            line(s, 4, 12, 12, 4, stick);
            rect(s, 11, 3, 3, 3, gem);

            // Flicker highlight.
            if (frame % 2 == 1) {
                setPx(s, 13, 3, {255,230,170,220});
                setPx(s, 12, 2, {255,255,255,200});
            }
            sparkle();
            break;
        }

        case ItemKind::LeatherArmor: {
            Color leather = add({140,90,55,255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
            outlineRect(s, 4, 4, 8, 10, mul(leather, 0.8f));
            rect(s, 5, 5, 6, 8, leather);
            rect(s, 4, 6, 2, 6, leather);
            rect(s, 10, 6, 2, 6, leather);
            sparkle();
            break;
        }
        case ItemKind::ChainArmor: {
            Color steel = add({170,170,180,255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
            outlineRect(s, 4, 4, 8, 10, mul(steel, 0.75f));
            rect(s, 5, 5, 6, 8, steel);
            for (int y = 6; y < 12; y += 2) {
                for (int x = 6; x < 10; x += 2) {
                    setPx(s, x, y, mul(steel, 0.6f));
                }
            }
            sparkle();
            break;
        }
        case ItemKind::PlateArmor: {
            Color steel = add({175,175,190,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 4, 8, 10, mul(steel, 0.70f));
            rect(s, 5, 5, 6, 8, steel);
            // Shoulders
            rect(s, 4, 5, 2, 3, mul(steel, 0.9f));
            rect(s, 10, 5, 2, 3, mul(steel, 0.9f));
            // Rivets / highlights
            setPx(s, 6, 6, mul(steel, 0.6f));
            setPx(s, 9, 6, mul(steel, 0.6f));
            setPx(s, 7, 9, mul(steel, 0.55f));
            setPx(s, 8, 9, mul(steel, 0.55f));
            sparkle();
            break;
        }
        case ItemKind::PotionHealing: {
            Color glass = {200,200,220,180};
            Color fluid = {220,80,120,220};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            if (frame % 2 == 1) setPx(s, 9, 6, {255,255,255,200});
            break;
        }
        case ItemKind::PotionAntidote: {
            Color glass = {200,200,220,180};
            Color fluid = {90,160,240,220};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            // tiny cross highlight
            setPx(s, 8, 8, {255,255,255,180});
            if (frame % 2 == 1) setPx(s, 9, 6, {255,255,255,200});
            break;
        }
        case ItemKind::PotionRegeneration: {
            Color glass = {200,200,220,180};
            Color fluid = {190,90,230,220};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            if (frame % 2 == 1) {
                setPx(s, 9, 6, {255,255,255,200});
                setPx(s, 7, 9, {255,255,255,120});
            }
            break;
        }
        case ItemKind::PotionShielding: {
            Color glass = {200,200,220,180};
            Color fluid = {200,200,200,220};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            // small "stone" speckle
            setPx(s, 7, 10, {120,120,120,255});
            if (frame % 2 == 1) setPx(s, 9, 6, {255,255,255,200});
            break;
        }
        case ItemKind::PotionHaste: {
            Color glass = {200,200,220,180};
            Color fluid = {255,170,80,220};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            // a tiny "bolt" shimmer
            if (frame % 2 == 1) {
                setPx(s, 8, 8, {255,255,255,180});
                setPx(s, 9, 6, {255,255,255,200});
            }
            break;
        }
        case ItemKind::PotionVision: {
            Color glass = {200,200,220,180};
            Color fluid = {90,220,220,220};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            // eye highlight
            setPx(s, 8, 8, {255,255,255,160});
            setPx(s, 7, 8, {40,40,40,200});
            setPx(s, 9, 8, {40,40,40,200});
            if (frame % 2 == 1) setPx(s, 9, 6, {255,255,255,200});
            break;
        }
        case ItemKind::ScrollTeleport: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // rune squiggles
            for (int x = 6; x <= 9; ++x) setPx(s, x, 8, {80,50,30,255});
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        case ItemKind::ScrollEnchantWeapon: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // sword-ish glyph
            line(s, 8, 6, 8, 10, {80,50,30,255});
            line(s, 7, 10, 9, 10, {80,50,30,255});
            setPx(s, 8, 5, {255,255,255,140});
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        case ItemKind::ScrollEnchantArmor: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // shield-ish glyph
            outlineRect(s, 7, 7, 3, 4, {80,50,30,255});
            setPx(s, 8, 10, {80,50,30,255});
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        
        case ItemKind::ScrollIdentify: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // "?" / identify-ish glyph
            line(s, 8, 7, 8, 9, {80,50,30,255});
            setPx(s, 8, 6, {80,50,30,255});
            setPx(s, 8, 10, {80,50,30,255});
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        case ItemKind::ScrollDetectTraps: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // Trap-ish glyph (X)
            line(s, 7, 7, 9, 9, {80,50,30,255});
            line(s, 9, 7, 7, 9, {80,50,30,255});
            setPx(s, 8, 10, {80,50,30,255});
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        case ItemKind::ScrollDetectSecrets: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // Secret-door-ish glyph
            outlineRect(s, 7, 7, 3, 4, {80,50,30,255});
            setPx(s, 9, 9, {80,50,30,255}); // knob
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        case ItemKind::ScrollKnock: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);

            // Lock glyph (shackle + body)
            outlineRect(s, 7, 7, 3, 3, {80,50,30,255});
            rect(s, 7, 9, 3, 2, {80,50,30,255});
            // Keyhole
            setPx(s, 8, 10, paper);

            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
case ItemKind::Arrow: {
            Color wood = add({160,110,60,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            line(s, 4, 12, 12, 4, wood);
            line(s, 11, 3, 13, 5, {220,220,220,255});
            setPx(s, 3, 13, {220,220,220,255});
            if (frame % 2 == 1) setPx(s, 9, 7, {255,255,255,100});
            break;
        }
        case ItemKind::Rock: {
            Color stone = add({130,130,140,255}, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
            circle(s, 8, 9, 4, stone);
            circle(s, 7, 8, 2, mul(stone, 0.9f));
            if (frame % 2 == 1) setPx(s, 6, 7, {255,255,255,80});
            break;
        }
        case ItemKind::Gold: {
            Color coin = add({230,200,60,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            circle(s, 8, 8, 5, coin);
            circle(s, 7, 7, 2, mul(coin, 1.05f));
            if (frame % 2 == 1) {
                setPx(s, 10, 6, {255,255,255,200});
                setPx(s, 11, 7, {255,255,255,140});
            }
            break;
        }
        case ItemKind::Sling: {
            Color leather = add({140,90,55,255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
            // Strap
            line(s, 4, 12, 12, 4, leather);
            line(s, 5, 13, 13, 5, mul(leather, 0.8f));
            // Pouch + stone
            circle(s, 10, 8, 2, mul(leather, 0.9f));
            circle(s, 10, 8, 1, {140,140,150,255});
            sparkle();
            break;
        }
        case ItemKind::PotionStrength: {
            Color glass = {200,200,220,180};
            Color fluid = {120,220,100,220};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            if (frame % 2 == 1) setPx(s, 9, 6, {255,255,255,200});
            break;
        }
        case ItemKind::ScrollMapping: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // Simple map-ish marks
            line(s, 6, 7, 10, 7, {80,50,30,255});
            line(s, 6, 9, 10, 9, {80,50,30,255});
            line(s, 7, 7, 7, 10, {80,50,30,255});
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        case ItemKind::FoodRation: {
            // Simple "ration" icon: a wrapped package with crumbs.
            Color wrap = add({210, 190, 140, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color edge = mul(wrap, 0.8f);
            outlineRect(s, 4, 5, 8, 7, edge);
            rect(s, 5, 6, 6, 5, wrap);
            // A little tie
            setPx(s, 8, 5, {120, 80, 40, 255});
            setPx(s, 7, 5, {120, 80, 40, 255});
            // Crumbs
            if (frame % 2 == 1) {
                setPx(s, 6, 12, {230, 220, 190, 200});
                setPx(s, 11, 11, {230, 220, 190, 200});
            }
            break;
        }
        case ItemKind::AmuletYendor: {
            Color gold = add({230,200,60,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            // Chain
            line(s, 6, 4, 10, 4, mul(gold, 0.9f));
            line(s, 7, 5, 9, 5, mul(gold, 0.85f));
            // Pendant
            circle(s, 8, 10, 3, gold);
            circle(s, 8, 9, 1, mul(gold, 1.05f));
            if (frame % 2 == 1) setPx(s, 10, 8, {255,255,255,180});
            break;
        }
        case ItemKind::Key: {
            Color metal = add({210,190,80,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color dark = mul(metal, 0.75f);
            // Bow (ring)
            circle(s, 6, 7, 3, metal);
            circle(s, 6, 7, 1, {0,0,0,0});
            // Shaft
            line(s, 7, 7, 13, 7, metal);
            line(s, 7, 8, 13, 8, dark);
            // Teeth
            rect(s, 10, 9, 2, 2, metal);
            rect(s, 13, 9, 2, 2, dark);
            if (frame % 2 == 1) setPx(s, 12, 6, {255,255,255,160});
            break;
        }
        case ItemKind::Lockpick: {
            Color metal = add({185,185,205,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color dark = mul(metal, 0.65f);

            // Handle
            rect(s, 3, 10, 4, 2, dark);
            rect(s, 4, 9, 2, 1, dark);

            // Shaft
            line(s, 7, 10, 14, 10, metal);
            line(s, 7, 11, 14, 11, dark);

            // Hook tip
            setPx(s, 14, 9, metal);
            setPx(s, 14, 10, metal);
            setPx(s, 13, 9, metal);

            if (frame % 2 == 1) setPx(s, 10, 9, {255,255,255,140});
            break;
        }
        case ItemKind::Chest: {
            // A small wooden chest with a metal latch.
            Color wood = add({150, 105, 60, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
            Color woodDark = mul(wood, 0.70f);
            Color band = add({180, 180, 200, 255}, rng.range(-8,8), rng.range(-8,8), rng.range(-8,8));
            Color bandDark = mul(band, 0.75f);

            // Body
            outlineRect(s, 3, 7, 10, 7, woodDark);
            rect(s, 4, 8, 8, 5, wood);

            // Lid
            outlineRect(s, 3, 5, 10, 3, woodDark);
            rect(s, 4, 6, 8, 1, mul(wood, 0.90f));

            // Metal band
            line(s, 3, 10, 12, 10, bandDark);
            line(s, 3, 9, 12, 9, band);

            // Latch
            rect(s, 7, 9, 2, 3, bandDark);
            setPx(s, 8, 10, band);

            // A subtle glint.
            if (frame % 2 == 1) setPx(s, 10, 6, {255,255,255,120});
            break;
        }
        case ItemKind::ChestOpen: {
            // Open chest: lid up + visible gold.
            Color wood = add({150, 105, 60, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
            Color woodDark = mul(wood, 0.70f);
            Color gold = add({235, 200, 70, 255}, rng.range(-8,8), rng.range(-8,8), rng.range(-8,8));
            Color gold2 = mul(gold, 0.85f);

            // Body
            outlineRect(s, 3, 8, 10, 6, woodDark);
            rect(s, 4, 9, 8, 4, wood);

            // Open lid (angled)
            line(s, 4, 7, 10, 4, woodDark);
            line(s, 4, 6, 10, 3, mul(woodDark, 0.9f));

            // Gold inside
            rect(s, 5, 9, 6, 2, gold2);
            rect(s, 6, 10, 4, 2, gold);

            // Sparkle
            if (frame % 2 == 1) {
                setPx(s, 9, 8, {255,255,255,180});
                setPx(s, 7, 9, {255,255,255,120});
            }
            break;
        }
        case ItemKind::PotionInvisibility: {
            Color glass = {200,200,220,180};
            Color fluid = {180,180,255,120};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            if (frame % 2 == 1) {
                setPx(s, 9, 6, {255,255,255,120});
                setPx(s, 8, 9, {255,255,255,120});
            }
            break;
        }
        case ItemKind::ScrollRemoveCurse: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // Spiral glyph
            setPx(s, 7, 7, {80,50,30,255});
            setPx(s, 8, 7, {80,50,30,255});
            setPx(s, 9, 7, {80,50,30,255});
            setPx(s, 9, 8, {80,50,30,255});
            setPx(s, 9, 9, {80,50,30,255});
            setPx(s, 8, 9, {80,50,30,255});
            setPx(s, 7, 9, {80,50,30,255});
            setPx(s, 7, 8, {80,50,30,255});
            setPx(s, 8, 8, paper);
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }
        case ItemKind::PotionClarity: {
            // A mostly-clear potion with a subtle blue tint ("clarity").
            Color glass = {200,200,220,180};
            Color fluid = {200,230,255,140};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});
            // Tiny sparkles
            if (frame % 2 == 1) {
                setPx(s, 8, 7, {255,255,255,160});
                setPx(s, 9, 9, {255,255,255,120});
            }
            break;
        }
        case ItemKind::ScrollConfusion: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // Swirl glyph
            setPx(s, 7, 7, {80,50,30,255});
            setPx(s, 8, 7, {80,50,30,255});
            setPx(s, 9, 7, {80,50,30,255});
            setPx(s, 9, 8, {80,50,30,255});
            setPx(s, 8, 9, {80,50,30,255});
            setPx(s, 7, 9, {80,50,30,255});
            setPx(s, 7, 8, paper);
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }

        case ItemKind::Torch: {
            Color wood = add({130,90,45,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color tip = {60,40,25,255};
            line(s, 8, 4, 8, 14, wood);
            rect(s, 7, 11, 3, 3, mul(wood, 0.85f));
            rect(s, 6, 3, 5, 2, tip);
            if (frame % 2 == 1) setPx(s, 9, 5, {255,255,255,70});
            break;
        }
        case ItemKind::TorchLit: {
            Color wood = add({130,90,45,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color flame1 = {255,170,60,220};
            Color flame2 = {255,255,180,200};
            line(s, 8, 5, 8, 14, wood);
            rect(s, 7, 11, 3, 3, mul(wood, 0.85f));
            rect(s, 6, 4, 5, 2, mul(wood, 0.6f));
            circle(s, 8, 3, 2, flame1);
            circle(s, 8, 2, 1, flame2);
            if (frame % 2 == 1) {
                setPx(s, 9, 2, {255,255,255,180});
                setPx(s, 7, 3, {255,255,255,100});
            }
            break;
        }

        // --- Rings (append-only) ---
        case ItemKind::RingMight:
        case ItemKind::RingAgility:
        case ItemKind::RingFocus:
        case ItemKind::RingProtection: {
            // A small gold ring with a colored gem. Rings are tiny, so we use
            // chunky pixels and strong contrast.
            Color gold = add({235, 205, 85, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color goldDark = mul(gold, 0.70f);

            // Ring band (donut)
            circle(s, 8, 9, 4, gold);
            circle(s, 8, 9, 3, goldDark);
            circle(s, 8, 9, 2, {0,0,0,0});

            // Gem color per ring type
            Color gem{255, 255, 255, 235};
            switch (kind) {
                case ItemKind::RingMight:      gem = {220, 60, 50, 240}; break;
                case ItemKind::RingAgility:    gem = {60, 200, 90, 240}; break;
                case ItemKind::RingFocus:      gem = {90, 120, 255, 240}; break;
                case ItemKind::RingProtection: gem = {180, 240, 255, 235}; break;
                default: break;
            }

            // Gem on top of the ring
            circle(s, 8, 5, 2, gem);
            circle(s, 8, 5, 1, mul(gem, 0.85f));
            if (frame % 2 == 1) {
                setPx(s, 9, 4, {255,255,255,180});
                setPx(s, 7, 5, {255,255,255,120});
            }
            break;
        }

        // --- Corpses (append-only) ---
        case ItemKind::CorpseGoblin:
        case ItemKind::CorpseOrc:
        case ItemKind::CorpseBat:
        case ItemKind::CorpseSlime:
        case ItemKind::CorpseKobold:
        case ItemKind::CorpseWolf:
        case ItemKind::CorpseTroll:
        case ItemKind::CorpseWizard:
        case ItemKind::CorpseSnake:
        case ItemKind::CorpseSpider:
        case ItemKind::CorpseOgre:
        case ItemKind::CorpseMimic:
        case ItemKind::CorpseMinotaur: {
            // A small, simple corpse/remains icon. We vary the palette and silhouette
            // a bit by monster to help readability.
            Color blood = {140, 20, 20, 200};

            auto drawCorpseBlob = [&](Color body, Color shade, bool big) {
                // Blood pool
                rect(s, 4, 12, 8, 2, blood);
                setPx(s, 6, 11, blood);
                setPx(s, 10, 11, blood);

                const int r = big ? 4 : 3;
                circle(s, 8, 10, r, body);
                circle(s, 6, 8, r - 1, body);

                // Shading
                setPx(s, 9, 10, shade);
                setPx(s, 7, 9, shade);
                setPx(s, 6, 8, shade);

                // A tiny "eye" / detail
                setPx(s, 5, 8, {0, 0, 0, 180});
                if (frame % 2 == 1) setPx(s, 7, 8, {255, 255, 255, 90});
            };

            auto drawSnake = [&](Color body, Color shade) {
                // No blood pool for snakes: smaller splatter.
                rect(s, 5, 12, 6, 2, blood);
                // Body
                for (int i = 0; i < 9; ++i) {
                    const int x = 3 + i;
                    const int y = 9 + ((i % 3) == 0 ? 0 : ((i % 3) == 1 ? 1 : -1));
                    setPx(s, x, y, body);
                    if (i % 2 == 0) setPx(s, x, y + 1, mul(body, 0.85f));
                }
                // Head
                circle(s, 12, 9, 2, body);
                setPx(s, 13, 9, shade);
                setPx(s, 12, 8, {0, 0, 0, 180});
            };

            auto drawSpider = [&](Color body, Color mark) {
                rect(s, 5, 12, 6, 2, blood);
                circle(s, 8, 10, 3, body);
                circle(s, 7, 7, 2, body);
                // legs
                line(s, 5, 9, 2, 7, mul(body, 0.9f));
                line(s, 11, 9, 14, 7, mul(body, 0.9f));
                line(s, 5, 11, 2, 13, mul(body, 0.85f));
                line(s, 11, 11, 14, 13, mul(body, 0.85f));
                setPx(s, 8, 10, mark);
                setPx(s, 7, 7, {0, 0, 0, 180});
            };

            switch (kind) {
                case ItemKind::CorpseGoblin:
                    drawCorpseBlob({70, 155, 80, 255}, {35, 95, 45, 255}, false);
                    break;
                case ItemKind::CorpseOrc:
                    drawCorpseBlob({85, 135, 75, 255}, {45, 80, 40, 255}, false);
                    break;
                case ItemKind::CorpseBat:
                    drawCorpseBlob({90, 65, 110, 255}, {55, 35, 70, 255}, false);
                    break;
                case ItemKind::CorpseSlime: {
                    // Slime: no blood, just a goo puddle.
                    blood = {70, 170, 70, 180};
                    drawCorpseBlob({80, 190, 90, 210}, {50, 120, 55, 210}, false);
                    break;
                }
                case ItemKind::CorpseKobold:
                    drawCorpseBlob({160, 120, 90, 255}, {110, 80, 55, 255}, false);
                    break;
                case ItemKind::CorpseWolf:
                    drawCorpseBlob({165, 165, 175, 255}, {105, 105, 115, 255}, true);
                    break;
                case ItemKind::CorpseTroll:
                    drawCorpseBlob({95, 170, 85, 255}, {50, 105, 45, 255}, true);
                    break;
                case ItemKind::CorpseWizard: {
                    // Wizard: pale body + robe accent.
                    drawCorpseBlob({200, 175, 155, 255}, {130, 110, 95, 255}, false);
                    rect(s, 7, 9, 5, 3, {70, 95, 180, 220});
                    break;
                }
                case ItemKind::CorpseSnake:
                    drawSnake({95, 175, 70, 255}, {45, 110, 35, 255});
                    break;
                case ItemKind::CorpseSpider:
                    drawSpider({55, 55, 65, 255}, {140, 30, 30, 230});
                    break;
                case ItemKind::CorpseOgre:
                    drawCorpseBlob({175, 150, 125, 255}, {105, 90, 75, 255}, true);
                    break;
                case ItemKind::CorpseMimic:
                    drawCorpseBlob({150, 110, 70, 255}, {105, 75, 45, 255}, false);
                    break;
                case ItemKind::CorpseMinotaur: {
                    drawCorpseBlob({175, 125, 80, 255}, {105, 70, 45, 255}, true);
                    // small horns
                    setPx(s, 4, 6, {200, 200, 200, 200});
                    setPx(s, 5, 6, {200, 200, 200, 200});
                    setPx(s, 5, 5, {200, 200, 200, 200});
                    break;
                }
                default:
                    drawCorpseBlob({150, 150, 150, 255}, {90, 90, 90, 255}, false);
                    break;
            }

            break;
        }
        default:
            rect(s, 5, 5, 6, 6, {255,0,255,255});
            break;
    }

    return s;
}

SpritePixels generateProjectileSprite(ProjectileKind kind, uint32_t seed, int frame) {
    (void)seed;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    switch (kind) {
        case ProjectileKind::Arrow: {
            Color c = {220,220,220,255};
            line(s, 3, 13, 13, 3, c);
            line(s, 12, 2, 14, 4, c);
            line(s, 2, 14, 4, 12, c);
            break;
        }
        case ProjectileKind::Rock: {
            Color stone = {140,140,150,255};
            circle(s, 8, 8, 3, stone);
            if (frame % 2 == 1) setPx(s, 9, 7, {255,255,255,120});
            break;
        }
        case ProjectileKind::Spark: {
            Color s1 = {120,220,255,255};
            Color s2 = {255,255,255,200};
            line(s, 5, 11, 11, 5, s1);
            line(s, 6, 12, 12, 6, mul(s1, 0.75f));
            if (frame % 2 == 1) {
                setPx(s, 12, 4, s2);
                setPx(s, 4, 12, s2);
                setPx(s, 10, 6, s2);
            }
            break;
        }
        case ProjectileKind::Fireball: {
            // Small fiery blob with a bright core.
            Color outer = {200, 70, 30, 220};
            Color mid   = {255, 140, 60, 255};
            Color core  = {255, 230, 160, 255};

            circle(s, 8, 8, 3, outer);
            circle(s, 8, 8, 2, mid);
            circle(s, 8, 8, 1, core);

            // Flicker/sparks
            if (frame % 2 == 1) {
                setPx(s, 11, 6, {255,255,255,160});
                setPx(s, 6, 11, {255,220,180,140});
                setPx(s, 10, 10, {255,180,120,140});
            } else {
                setPx(s, 6, 6, {255,210,150,120});
                setPx(s, 10, 5, {255,200,120,110});
            }
            break;
        }
        default:
            break;
    }
    return s;
}

SpritePixels generateFloorTile(uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});
    RNG rng(hash32(seed));

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

            s.at(x, y) = mul(base, f);
        }
    }

    // Speckles
    for (int i = 0; i < 14; ++i) {
        int x = rng.range(0, 15);
        int y = rng.range(0, 15);
        s.at(x, y) = add(s.at(x, y), rng.range(-20, 20), rng.range(-20, 20), rng.range(-20, 20));
    }

    // Subtle animated "glint" pixels (torchlight shimmer).
    // Frame 1 brightens a few deterministic pixels; frame 0 is the baseline.
    if (frame % 2 == 1) {
        RNG g(hash32(seed ^ 0xF17A4u));
        for (int i = 0; i < 3; ++i) {
            int x = g.range(0, 15);
            int y = g.range(0, 15);
            s.at(x, y) = add(s.at(x, y), 35, 35, 35);
        }
        // A tiny 2px streak looks like a reflective grain line.
        int sx = g.range(1, 14);
        int sy = g.range(1, 14);
        setPx(s, sx, sy, add(s.at(sx, sy), 25, 25, 25));
        setPx(s, sx + 1, sy, add(s.at(sx + 1, sy), 18, 18, 18));
    }

    return s;
}

SpritePixels generateWallTile(uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});
    RNG rng(hash32(seed));

    Color base = { 70, 78, 92, 255 };
    base = add(base, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));

    // Brick pattern
    for (int y = 0; y < 16; ++y) {
        int rowOffset = (y / 4) % 2 ? 2 : 0;
        for (int x = 0; x < 16; ++x) {
            float f = 0.9f;
            // mortar lines
            if ((y % 4) == 0) f = 0.55f;
            if (((x + rowOffset) % 6) == 0) f = std::min(f, 0.6f);

            uint32_t n = hashCombine(seed, static_cast<uint32_t>(x + y * 19));
            float noise = ((n & 0xFFu) / 255.0f);
            float nf = 0.85f + noise * 0.25f;

            s.at(x, y) = mul(base, f * nf);
        }
    }

    // Subtle animated highlight on a few mortar pixels.
    if (frame % 2 == 1) {
        RNG g(hash32(seed ^ 0xBADD1u));
        for (int i = 0; i < 4; ++i) {
            int x = g.range(0, 15);
            int y = g.range(0, 15);
            // Prefer mortar lines for the highlight (looks like a light sweep).
            if ((y % 4) == 0 || (((x + ((y / 4) % 2 ? 2 : 0)) % 6) == 0)) {
                s.at(x, y) = add(s.at(x, y), 25, 25, 30);
            } else {
                // If we missed mortar, just add a tiny glint anyway.
                s.at(x, y) = add(s.at(x, y), 15, 15, 18);
            }
        }
    }

    return s;
}

SpritePixels generateChasmTile(uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});
    RNG rng(hash32(seed));

    // A dark "void" with subtle cool highlights so it reads differently than
    // unexplored black and the regular stone floor.
    Color base = { 10, 12, 18, 255 };
    base = add(base, rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2));

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint32_t n = hashCombine(seed, static_cast<uint32_t>(x + y * 31));
            float noise = ((n & 0xFFu) / 255.0f);

            // Stronger vignette than floor to suggest depth.
            float cx = (x - 7.5f) / 7.5f;
            float cy = (y - 7.5f) / 7.5f;
            float v = 1.0f - 0.22f * (cx*cx + cy*cy);

            // A faint ripple banding effect.
            float ripple = 0.90f + 0.10f * std::sin((x * 0.55f) + (y * 0.35f) + (seed % 97u) * 0.05f);

            float f = (0.78f + noise * 0.22f) * v * ripple;
            Color c = mul(base, f);

            // Cool tint. Keep subtle so global lighting mods still read.
            c = add(c, 0, 2, 10);
            s.at(x, y) = c;
        }
    }

    // Tiny "embers" of reflected light in the abyss.
    RNG sp(hash32(seed ^ 0xC4A5Au));
    int sparks = 6;
    if (frame % 2 == 1) sparks = 8;
    for (int i = 0; i < sparks; ++i) {
        int x = sp.range(1, 14);
        int y = sp.range(1, 14);
        Color c = s.at(x, y);
        c = add(c, 15, 18, 30);
        if (frame % 2 == 1 && (i % 2 == 0)) c = add(c, 18, 20, 35);
        s.at(x, y) = c;
    }

    return s;
}

SpritePixels generatePillarTile(uint32_t seed, int frame) {
    RNG rng(hash32(seed));

    // Base floor so the pillar feels embedded in the room.
    SpritePixels s = generateFloorTile(seed ^ 0x911A4u, frame);

    Color stone = { 128, 132, 145, 255 };
    stone = add(stone, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));
    Color dark = mul(stone, 0.65f);
    Color light = add(mul(stone, 1.08f), 8, 8, 10);

    // Soft shadow on the floor.
    for (int y = 11; y < 15; ++y) {
        for (int x = 4; x < 12; ++x) {
            s.at(x, y) = mul(s.at(x, y), 0.72f);
        }
    }

    // Pillar body (a simple column).
    outlineRect(s, 5, 2, 6, 13, dark);
    rect(s, 6, 3, 4, 11, stone);

    // Carve vertical grooves.
    for (int y = 3; y < 14; ++y) {
        if (y % 3 == 0) {
            setPx(s, 7, y, mul(stone, 0.85f));
            setPx(s, 8, y, mul(stone, 0.92f));
        }
    }

    // Cap and base rings.
    rect(s, 5, 2, 6, 1, light);
    rect(s, 5, 13, 6, 1, mul(stone, 0.92f));

    // A slight highlight shimmer on frame 1 to match other tiles.
    if (frame % 2 == 1) {
        setPx(s, 6, 4, add(s.at(6, 4), 25, 25, 28));
        setPx(s, 6, 9, add(s.at(6, 9), 18, 18, 20));
        setPx(s, 9, 6, add(s.at(9, 6), 12, 12, 14));
    }

    return s;
}

SpritePixels generateStairsTile(uint32_t seed, bool up, int frame) {
    RNG rng(hash32(seed));
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});

    // Base = floor-like
    SpritePixels floor = generateFloorTile(seed ^ 0xB00Bu, frame);
    s = floor;

    Color stair = { 180, 170, 150, 255 };
    stair = add(stair, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));

    // Simple diagonal steps
    for (int i = 0; i < 6; ++i) {
        int x0 = 4 + i;
        int y0 = 11 - i;
        line(s, x0, y0, x0 + 7, y0, mul(stair, 0.95f));
        line(s, x0, y0 + 1, x0 + 6, y0 + 1, mul(stair, 0.75f));
    }

    // Arrow hint
    Color arrow = up ? Color{120,255,120,200} : Color{255,120,120,200};
    if (frame % 2 == 1) arrow.a = 230;
    if (up) {
        line(s, 8, 4, 8, 9, arrow);
        line(s, 6, 6, 8, 4, arrow);
        line(s, 10, 6, 8, 4, arrow);
    } else {
        line(s, 8, 7, 8, 12, arrow);
        line(s, 6, 10, 8, 12, arrow);
        line(s, 10, 10, 8, 12, arrow);
    }

    return s;
}

SpritePixels generateDoorTile(uint32_t seed, bool open, int frame) {
    RNG rng(hash32(seed));
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});

    // Base floor-ish
    s = generateFloorTile(seed ^ 0xD00Du, frame);

    Color wood = add({140, 95, 55, 255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
    Color dark = mul(wood, 0.7f);

    if (open) {
        // Dark gap
        rect(s, 5, 3, 6, 11, {20,20,25,255});
        // Frame
        outlineRect(s, 4, 2, 8, 13, wood);
        // Hinges highlight
        if (frame % 2 == 1) {
            setPx(s, 4, 6, {255,255,255,80});
            setPx(s, 11, 8, {255,255,255,60});
        }
    } else {
        // Solid door
        outlineRect(s, 4, 2, 8, 13, dark);
        rect(s, 5, 3, 6, 11, wood);
        // Planks
        for (int y = 4; y <= 12; y += 3) line(s, 5, y, 10, y, mul(wood, 0.8f));
        // Knob
        circle(s, 10, 8, 1, {200, 190, 80, 255});
        if (frame % 2 == 1) setPx(s, 11, 7, {255,255,255,120});
    }



    return s;
}

SpritePixels generateLockedDoorTile(uint32_t seed, int frame) {
    // Base: closed door sprite, with a small lock overlay for readability.
    SpritePixels s = generateDoorTile(seed, /*open=*/false, frame);

    // Lock colors: warm metal with dark outline.
    const Color lockBody { 210, 185, 70, 255 };
    const Color lockOutline { 120, 90, 25, 255 };
    const Color keyhole { 30, 22, 10, 255 };

    // Center-ish placement (slight per-seed variation).
    int x0 = 6 + static_cast<int>((seed >> 12) & 1u);
    int y0 = 6;

    // Shackle
    outlineRect(s, x0, y0, 4, 4, lockOutline);

    // Body
    rect(s, x0, y0 + 4, 4, 3, lockBody);
    outlineRect(s, x0, y0 + 4, 4, 3, lockOutline);

    // Keyhole
    setPx(s, x0 + 1, y0 + 5, keyhole);
    setPx(s, x0 + 2, y0 + 5, keyhole);
    setPx(s, x0 + 2, y0 + 6, keyhole);

    // Tiny shimmer highlight every so often.
    if ((frame % 16) < 2) {
        setPx(s, x0 + 2, y0 + 4, Color{ 245, 235, 130, 255 });
    }

    return s;
}

SpritePixels generateUIPanelTile(UITheme theme, uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});

    // Theme palette (kept fairly dark so HUD/overlay text stays readable).
    Color base{22, 24, 32, 255};
    Color accent{120, 140, 180, 255};

    switch (theme) {
        case UITheme::DarkStone:
            base   = {22, 24, 32, 255};
            accent = {110, 140, 190, 255};
            break;
        case UITheme::Parchment:
            base   = {44, 38, 26, 255};
            accent = {170, 150, 95, 255};
            break;
        case UITheme::Arcane:
            base   = {32, 18, 40, 255};
            accent = {190, 120, 255, 255};
            break;
    }

    const uint32_t t = static_cast<uint32_t>(theme);
    RNG rng(hash32(seed ^ (0xC0FFEEu + t * 101u)));

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint32_t n = hashCombine(seed ^ (0xA11CEu + t * 177u),
                                     static_cast<uint32_t>(x + y * 17 + frame * 131));
            float noise = ((n & 0xFFu) / 255.0f);
            float f = 0.72f + noise * 0.35f;

            // Very subtle banding makes the panels feel less flat.
            float band = 0.92f + 0.08f * std::sin((x + frame * 2) * 0.9f + (seed & 0xFFu) * 0.10f);
            f *= band;

            // Darken edges a bit (helps framing).
            if (x == 0 || y == 0 || x == 15 || y == 15) f *= 0.85f;

            s.at(x, y) = mul(base, f);
        }
    }

    // Theme-specific micro-details.
    if (theme == UITheme::DarkStone) {
        // Hairline cracks.
        for (int i = 0; i < 2; ++i) {
            int x0 = rng.range(0, 15);
            int y0 = rng.range(0, 15);
            int x1 = std::clamp(x0 + rng.range(-6, 6), 0, 15);
            int y1 = std::clamp(y0 + rng.range(-6, 6), 0, 15);
            line(s, x0, y0, x1, y1, mul(accent, 0.25f));
        }
    } else if (theme == UITheme::Parchment) {
        // Fibers.
        for (int i = 0; i < 6; ++i) {
            int x = rng.range(0, 15);
            int y = rng.range(0, 15);
            int len = rng.range(3, 7);
            for (int j = 0; j < len; ++j) {
                int yy = std::clamp(y + j, 0, 15);
                setPx(s, x, yy, mul(accent, 0.18f));
            }
        }
    } else { // Arcane
        // Subtle rune-dots, with a mild "pulse" every other frame.
        Color rune = mul(accent, 0.28f);
        rune.a = 220;
        Color rune2 = mul(accent, 0.18f);
        rune2.a = 200;

        const int dots = 8;
        for (int i = 0; i < dots; ++i) {
            int x = rng.range(2, 13);
            int y = rng.range(2, 13);
            setPx(s, x, y, (i % 2 == 0) ? rune : rune2);
        }

        if (frame % 2 == 1) {
            // One extra bright spark on pulse frame.
            int x = rng.range(3, 12);
            int y = rng.range(3, 12);
            Color spark = accent;
            spark.a = 120;
            setPx(s, x, y, spark);
        }
    }

    return s;
}

SpritePixels generateUIOrnamentTile(UITheme theme, uint32_t seed, int frame) {
    (void)seed;

    // Transparent sprite; drawn on top of panel backgrounds.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    Color c{200, 210, 230, 190};
    switch (theme) {
        case UITheme::DarkStone: c = {200, 210, 230, 190}; break;
        case UITheme::Parchment: c = {230, 210, 150, 190}; break;
        case UITheme::Arcane:    c = {220, 160, 255, 190}; break;
    }

    Color c2 = mul(c, 0.65f);
    c2.a = 160;

    // Corner bracket
    line(s, 0, 0, 7, 0, c);
    line(s, 0, 0, 0, 7, c);
    line(s, 1, 1, 6, 1, c2);
    line(s, 1, 1, 1, 6, c2);

    // Tiny rune-ish mark
    setPx(s, 3, 3, c);
    setPx(s, 4, 3, c2);
    setPx(s, 3, 4, c2);
    setPx(s, 5, 4, c2);

    // Flicker highlight for a bit of life.
    if (frame % 2 == 1) {
        setPx(s, 2, 0, {255,255,255,110});
        setPx(s, 0, 2, {255,255,255,80});
        setPx(s, 3, 2, {255,255,255,60});
    }

    return s;
}

