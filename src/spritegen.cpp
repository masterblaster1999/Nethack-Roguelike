#include "spritegen.hpp"
#include "spritegen3d.hpp"
#include "game.hpp"      // EntityKind
#include "items.hpp"     // ItemKind, ProjectileKind
#include <algorithm>
#include <cmath>
#include <utility>

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

Color getPx(const SpritePixels& s, int x, int y) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return {0,0,0,0};
    return s.at(x, y);
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

// --- Resampling helpers -----------------------------------------------------

inline bool sameColor(const Color& a, const Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

inline int clampSpriteSize(int pxSize) {
    // Keep sprites in a sane range; large sizes are supported for high-DPI displays.
    return std::clamp(pxSize, 16, 256);
}

SpritePixels resizeNearest(const SpritePixels& src, int outW, int outH) {
    if (src.w <= 0 || src.h <= 0 || outW <= 0 || outH <= 0) {
        return makeSprite(std::max(0, outW), std::max(0, outH), {0,0,0,0});
    }

    SpritePixels dst = makeSprite(outW, outH, {0,0,0,0});
    for (int y = 0; y < outH; ++y) {
        const int sy = (y * src.h) / outH;
        for (int x = 0; x < outW; ++x) {
            const int sx = (x * src.w) / outW;
            dst.at(x, y) = src.at(sx, sy);
        }
    }
    return dst;
}

// Scale2x pixel-art upscaling algorithm (edge-aware). This preserves crisp
// silhouettes much better than nearest-neighbor when scaling to 32/64/128/256.
SpritePixels scale2x(const SpritePixels& src) {
    if (src.w <= 0 || src.h <= 0) return src;
    SpritePixels dst = makeSprite(src.w * 2, src.h * 2, {0,0,0,0});

    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const Color A = getPx(src, x - 1, y - 1);
            const Color B = getPx(src, x,     y - 1);
            const Color C = getPx(src, x + 1, y - 1);
            const Color D = getPx(src, x - 1, y);
            const Color E = getPx(src, x,     y);
            const Color F = getPx(src, x + 1, y);
            const Color G = getPx(src, x - 1, y + 1);
            const Color H = getPx(src, x,     y + 1);
            const Color I = getPx(src, x + 1, y + 1);
            (void)A; (void)C; (void)G; (void)I;

            Color E0 = E, E1 = E, E2 = E, E3 = E;
            if (!sameColor(B, H) && !sameColor(D, F)) {
                E0 = sameColor(D, B) ? D : E;
                E1 = sameColor(B, F) ? F : E;
                E2 = sameColor(D, H) ? D : E;
                E3 = sameColor(H, F) ? F : E;
            }

            dst.at(2 * x,     2 * y)     = E0;
            dst.at(2 * x + 1, 2 * y)     = E1;
            dst.at(2 * x,     2 * y + 1) = E2;
            dst.at(2 * x + 1, 2 * y + 1) = E3;
        }
    }
    return dst;
}

inline bool isPow2(int v) {
    return v > 0 && (v & (v - 1)) == 0;
}

inline bool isPow2Multiple(int base, int target) {
    if (base <= 0 || target <= 0) return false;
    if (target < base) return false;
    if (target % base != 0) return false;
    return isPow2(target / base);
}

SpritePixels resampleSpriteToSize(const SpritePixels& src, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    if (src.w == pxSize && src.h == pxSize) return src;

    // Fast path: edge-aware Scale2x chain for powers-of-two scaling.
    if (src.w == src.h && isPow2Multiple(src.w, pxSize)) {
        SpritePixels cur = src;
        while (cur.w < pxSize) cur = scale2x(cur);
        return cur;
    }

    // Fallback: nearest-neighbor resize.
    return resizeNearest(src, pxSize, pxSize);
}

// --- Pixel-art helpers (ordered dithering, outlines, shadows) ---

inline float bayer4Threshold(int x, int y) {
    // 4x4 Bayer matrix threshold map (ordered dithering)
    static constexpr int BAYER4[4][4] = {
        { 0,  8,  2, 10 },
        {12,  4, 14,  6 },
        { 3, 11,  1,  9 },
        {15,  7, 13,  5 },
    };
    const int v = BAYER4[y & 3][x & 3];
    return (static_cast<float>(v) + 0.5f) / 16.0f; // [0,1)
}

Color lerp(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto lerp8 = [&](uint8_t A, uint8_t B) -> uint8_t {
        return clamp8(static_cast<int>(std::lround(A + (B - A) * t)));
    };
    return { lerp8(a.r, b.r), lerp8(a.g, b.g), lerp8(a.b, b.b), lerp8(a.a, b.a) };
}

// Quantized shading ramp for crisp pixel-art lighting (4 tones), with ordered dithering.
Color rampShade(Color base, float shade01, int x, int y) {
    shade01 = std::clamp(shade01, 0.0f, 1.0f);

    Color ramp[4];
    ramp[0] = mul(base, 0.45f);
    ramp[1] = mul(base, 0.70f);
    ramp[2] = base;
    ramp[3] = add(mul(base, 1.12f), 12, 12, 14);

    // Map shade into 0..3 range.
    const float t = shade01 * 3.0f;
    int idx = static_cast<int>(std::floor(t));
    float frac = t - static_cast<float>(idx);

    if (idx < 0) { idx = 0; frac = 0.0f; }
    if (idx > 3) { idx = 3; frac = 0.0f; }

    // Ordered dithering between idx and idx+1.
    if (idx < 3) {
        const float thr = bayer4Threshold(x, y);
        if (frac > thr) idx++;
    }

    return ramp[idx];
}

// Softer, hue-shifted ramp for environment tiles (stone, panels). Keeps the world looking
// like crisp pixel-art instead of smooth gradients.
Color rampShadeTile(Color base, float shade01, int x, int y) {
    shade01 = std::clamp(shade01, 0.0f, 1.0f);

    // Slight hue shift: cooler shadows, warmer highlights.
    Color ramp[4];
    ramp[0] = add(mul(base, 0.52f), -12, -12, +6);
    ramp[1] = add(mul(base, 0.78f), -4, -4, +3);
    ramp[2] = base;
    ramp[3] = add(mul(base, 1.08f), +12, +10, +4);

    float t = shade01 * 3.0f;
    int idx = static_cast<int>(std::floor(t));
    float frac = t - static_cast<float>(idx);
    idx = std::clamp(idx, 0, 2);

    const float thr = bayer4Threshold(x, y);
    if (frac > thr) ++idx;

    return ramp[idx];
}


Color averageOpaqueColor(const SpritePixels& s) {
    uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
    for (const Color& c : s.px) {
        if (c.a == 0) continue;
        sr += static_cast<uint64_t>(c.r) * c.a;
        sg += static_cast<uint64_t>(c.g) * c.a;
        sb += static_cast<uint64_t>(c.b) * c.a;
        sa += c.a;
    }
    if (sa == 0) return {40, 40, 45, 255};
    return {
        static_cast<uint8_t>(sr / sa),
        static_cast<uint8_t>(sg / sa),
        static_cast<uint8_t>(sb / sa),
        255
    };
}

void lineBlend(SpritePixels& s, int x0, int y0, int x1, int y1, Color c) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        blendPx(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void applyDropShadow(SpritePixels& s, int dx, int dy, uint8_t alpha) {
    if (alpha == 0) return;
    const SpritePixels orig = s;
    for (int y = 0; y < orig.h; ++y) {
        for (int x = 0; x < orig.w; ++x) {
            if (orig.at(x, y).a == 0) continue;
            const int xx = x + dx;
            const int yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= orig.w || yy >= orig.h) continue;
            if (orig.at(xx, yy).a != 0) continue; // don't shadow inside

            Color& dst = s.at(xx, yy);
            if (dst.a < alpha) {
                dst = { 0, 0, 0, alpha };
            }
        }
    }
}

void applyExteriorOutline(SpritePixels& s, Color outline) {
    if (outline.a == 0) return;
    const SpritePixels orig = s;
    for (int y = 0; y < orig.h; ++y) {
        for (int x = 0; x < orig.w; ++x) {
            if (orig.at(x, y).a == 0) continue;

            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    const int xx = x + ox;
                    const int yy = y + oy;
                    if (xx < 0 || yy < 0 || xx >= orig.w || yy >= orig.h) continue;
                    if (orig.at(xx, yy).a != 0) continue;

                    Color& dst = s.at(xx, yy);
                    if (dst.a < outline.a) dst = outline;
                }
            }
        }
    }
}

void applyContourShade(SpritePixels& s, int edgeDx, int edgeDy, float factor) {
    const SpritePixels orig = s;
    for (int y = 0; y < orig.h; ++y) {
        for (int x = 0; x < orig.w; ++x) {
            if (orig.at(x, y).a == 0) continue;
            const int xx = x + edgeDx;
            const int yy = y + edgeDy;
            if (xx < 0 || yy < 0 || xx >= orig.w || yy >= orig.h) continue;
            if (orig.at(xx, yy).a != 0) continue;

            s.at(x, y) = mul(orig.at(x, y), factor);
        }
    }
}

void applyRimLight(SpritePixels& s, int edgeDx, int edgeDy, Color highlight) {
    if (highlight.a == 0) return;
    const SpritePixels orig = s;
    for (int y = 0; y < orig.h; ++y) {
        for (int x = 0; x < orig.w; ++x) {
            if (orig.at(x, y).a == 0) continue;
            const int xx = x + edgeDx;
            const int yy = y + edgeDy;
            if (xx < 0 || yy < 0 || xx >= orig.w || yy >= orig.h) continue;
            if (orig.at(xx, yy).a != 0) continue;

            blendPx(s, x, y, highlight);
        }
    }
}

void finalizeSprite(SpritePixels& s, uint32_t seed, int frame, uint8_t outlineAlpha, uint8_t shadowAlpha) {
    (void)seed;

    // Derive a dark outline color from the sprite itself (tinted outline reads well).
    // Compute this *before* adding a shadow so the shadow doesn't skew the average.
    const Color avg = averageOpaqueColor(s);
    Color outline = add(mul(avg, 0.18f), -18, -18, -18);
    outline.a = outlineAlpha;

    // 1) Drop shadow first so the outline overwrites it on edge pixels.
    applyDropShadow(s, 1, 1, shadowAlpha);

    // 2) Outline.
    applyExteriorOutline(s, outline);

    // 3) Slight contour lighting: darker bottom-right, lighter top-left.
    applyContourShade(s, 1, 1, 0.92f);

    Color rim{255, 255, 255, static_cast<uint8_t>(35 + ((frame % 2) ? 15 : 0))};
    applyRimLight(s, -1, -1, rim);
}


// --- Identification appearance art -----------------------------------------
//
// When SPRITE_SEED_IDENT_APPEARANCE_FLAG is set on an *item* sprite seed,
// generateItemSprite() will draw NetHack-style "randomized appearance" art for
// potions/scrolls/rings/wands (using seed&0xFF as the appearance id).

struct PotionStyle {
    Color fluid{120, 180, 255, 220};
    Color fluidHi{180, 220, 255, 220};
    bool metallic = false;
    bool smoky = false;
    bool murky = false;
    bool milky = false;
};

PotionStyle potionStyleFor(uint8_t a) {
    // Mapping matches Game's POTION_APPEARANCES (16 entries).
    PotionStyle st;
    switch (static_cast<int>(a % 16u)) {
        case 0:  st.fluid = {220, 60, 80, 230};  st.fluidHi = {255, 150, 170, 220}; break; // Ruby
        case 1:  st.fluid = {60, 200, 90, 230};  st.fluidHi = {150, 255, 190, 220}; break; // Emerald
        case 2:  st.fluid = {80, 120, 255, 230}; st.fluidHi = {170, 210, 255, 220}; break; // Sapphire
        case 3:  st.fluid = {255, 170, 70, 230}; st.fluidHi = {255, 230, 160, 220}; break; // Amber
        case 4:  st.fluid = {240, 220, 80, 230}; st.fluidHi = {255, 250, 185, 220}; break; // Topaz
        case 5:  st.fluid = {70, 55, 95, 230};   st.fluidHi = {140, 120, 170, 220}; break; // Onyx
        case 6:  st.fluid = {225, 230, 240, 215}; st.fluidHi = {255, 255, 255, 210}; st.milky = true; break; // Pearl
        case 7:  st.fluid = {235, 225, 205, 215}; st.fluidHi = {255, 250, 235, 210}; st.milky = true; break; // Ivory
        case 8:  st.fluid = {80, 220, 220, 230}; st.fluidHi = {175, 255, 255, 220}; break; // Azure
        case 9:  st.fluid = {190, 90, 230, 230}; st.fluidHi = {235, 190, 255, 220}; break; // Violet
        case 10: st.fluid = {200, 40, 55, 230};  st.fluidHi = {255, 140, 150, 220}; break; // Crimson
        case 11: st.fluid = {90, 220, 120, 230}; st.fluidHi = {170, 255, 200, 220}; break; // Verdant
        case 12: st.fluid = {205, 210, 220, 220}; st.fluidHi = {255, 255, 255, 210}; st.metallic = true; break; // Silver
        case 13: st.fluid = {235, 200, 70, 230}; st.fluidHi = {255, 245, 170, 220}; st.metallic = true; break; // Golden
        case 14: st.fluid = {175, 175, 185, 170}; st.fluidHi = {225, 225, 235, 165}; st.smoky = true; break; // Smoke
        case 15: st.fluid = {120, 110, 85, 230}; st.fluidHi = {165, 150, 120, 220}; st.murky = true; break; // Murky
        default: break;
    }
    return st;
}

void drawPotionAppearance(SpritePixels& s, uint32_t seed, RNG& rng, uint8_t a, int frame) {
    const PotionStyle st = potionStyleFor(a);

    // Bottle
    Color glass = {200, 220, 255, 170};
    Color glassEdge = {170, 200, 235, 200};
    Color cork = {140, 95, 55, 255};

    // Body + neck
    outlineRect(s, 6, 4, 4, 9, glassEdge);
    rect(s, 7, 5, 2, 7, glass);
    rect(s, 6, 3, 4, 2, cork);

    // Liquid (slight gradient)
    rect(s, 7, 7, 2, 5, st.fluid);
    setPx(s, 7, 7, st.fluidHi);
    setPx(s, 8, 7, st.fluidHi);

    // Extra vibe: metallic shimmer / smoke / murk.
    const uint32_t h = hash32(seed ^ (0xC0FFEEu + static_cast<uint32_t>(frame) * 1337u));

    if (st.metallic) {
        // Dithered sparkle inside the liquid.
        for (int yy = 7; yy <= 11; ++yy) {
            for (int xx = 7; xx <= 8; ++xx) {
                const uint32_t v = hash32(h ^ static_cast<uint32_t>(xx * 31 + yy * 97));
                if ((v & 7u) == 0u) setPx(s, xx, yy, {255, 255, 255, 210});
                else if ((v & 7u) == 1u) setPx(s, xx, yy, mul(st.fluid, 0.85f));
            }
        }
    }

    if (st.murky) {
        // Dark specks.
        for (int i = 0; i < 3; ++i) {
            const int xx = 7 + static_cast<int>((h >> (i * 3)) & 1u);
            const int yy = 8 + static_cast<int>((h >> (i * 4)) & 3u);
            setPx(s, xx, yy, mul(st.fluid, 0.60f));
        }
    }

    if (st.smoky) {
        // A little smoke curl above the bottle.
        Color smoke = {190, 190, 205, static_cast<uint8_t>(120 + (frame % 2) * 30)};
        const int off = (frame % 2);
        setPx(s, 9 + off, 2, smoke);
        setPx(s, 10 + off, 3, smoke);
        setPx(s, 11 + off, 4, {190, 190, 205, 90});
    }

    if (st.milky) {
        // Soft swirl highlight.
        setPx(s, 7, 9, {255,255,255,120});
        setPx(s, 8, 10, {255,255,255,90});
    }

    // Glass highlight
    if (frame % 2 == 1) {
        setPx(s, 9, 5, {255,255,255,130});
        setPx(s, 9, 7, {255,255,255,90});
    }

    // Tiny bubble (stable-ish, but flickers with frame)
    if ((h & 3u) == 0u) {
        setPx(s, 8, 9 + (frame % 2), {255,255,255,90});
    }

    (void)rng; // reserved for future shape variation
}

// A tiny 3x5 rune alphabet (15-bit masks).
// Bit i corresponds to x + 3*y (x in [0,2], y in [0,4]).
constexpr uint16_t RUNE_GLYPHS[] = {
    0b010'111'010'010'010, // "T"
    0b111'101'111'101'111, // "A"-ish
    0b110'101'110'101'110, // "B"-ish
    0b111'100'100'100'111, // "C"
    0b110'101'101'101'110, // "O"-ish
    0b111'100'111'100'111, // "E"-ish
    0b111'100'110'100'100, // "P"-ish
    0b101'101'111'001'001, // "Y"-ish
    0b010'111'101'111'010, // "*" sigil
    0b100'010'001'010'100, // "X"
    0b001'010'100'010'001, // mirrored X
    0b010'101'010'101'010, // "#"-ish
};

void drawRuneGlyph(SpritePixels& s, int x, int y, uint16_t mask, Color ink) {
    for (int yy = 0; yy < 5; ++yy) {
        for (int xx = 0; xx < 3; ++xx) {
            const int bit = xx + yy * 3;
            if ((mask >> bit) & 1u) setPx(s, x + xx, y + yy, ink);
        }
    }
}

void drawScrollAppearance(SpritePixels& s, uint32_t seed, RNG& rng, uint8_t a, int frame) {
    // Paper palette (slight variation per appearance)
    RNG palRng(hashCombine(seed, 0x5C2011u));
    Color paper = add({225, 215, 190, 255}, palRng.range(-10,10), palRng.range(-10,10), palRng.range(-10,10));
    Color paperEdge = mul(paper, 0.80f);
    Color ink = {70, 55, 45, 220};

    // Scroll body
    outlineRect(s, 4, 5, 8, 7, paperEdge);
    rect(s, 5, 6, 6, 5, paper);
    // curled edges
    rect(s, 4, 6, 1, 5, mul(paper, 0.75f));
    rect(s, 11, 6, 1, 5, mul(paper, 0.75f));

    // Wax seal color varies with appearance id.
    static const Color waxColors[] = {
        {170, 40, 50, 255}, {70, 90, 190, 255}, {60, 160, 100, 255}, {150, 90, 170, 255},
        {150, 120, 60, 255}, {70, 70, 70, 255}
    };
    Color wax = waxColors[static_cast<size_t>(a % (sizeof(waxColors)/sizeof(waxColors[0])))] ;
    circle(s, 8, 11, 1, wax);
    setPx(s, 8, 10, mul(wax, 0.85f));

    // Rune "label" generated from appearance id.
    RNG r(hashCombine(seed, static_cast<uint32_t>(a) ^ 0xC0DEC0DEu));
    const int gx0 = 5;
    const int gy0 = 6;
    const int cols = 2;
    const int rows = 2;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const uint16_t g = RUNE_GLYPHS[r.nextU32() % (sizeof(RUNE_GLYPHS)/sizeof(RUNE_GLYPHS[0]))];
            const int x = gx0 + col * 4;
            const int y = gy0 + row * 3;
            // draw slightly squashed (top 3 rows of 5) to fit.
            drawRuneGlyph(s, x, y, g, ink);
        }
    }

    // A little magical glint
    if (frame % 2 == 1) {
        const int fx = 5 + static_cast<int>((hash32(seed ^ 0x51A11u) % 6u));
        setPx(s, fx, 7, {255,255,255,120});
    }

    (void)rng;
}

Color ringMaterial(uint8_t a) {
    // Mapping matches Game's RING_APPEARANCES (16 entries).
    switch (static_cast<int>(a % 16u)) {
        case 0:  return {190, 120, 70, 255};  // Copper
        case 1:  return {205, 175, 85, 255};  // Brass
        case 2:  return {175, 175, 190, 255}; // Steel
        case 3:  return {220, 220, 235, 255}; // Silver
        case 4:  return {235, 205, 85, 255};  // Gold
        case 5:  return {205, 225, 225, 255}; // Platinum
        case 6:  return {140, 140, 150, 255}; // Iron
        case 7:  return {170, 170, 175, 255}; // Tin
        case 8:  return {200, 230, 255, 235}; // Opal
        case 9:  return {60, 60, 70, 255};    // Onyx
        case 10: return {60, 180, 100, 255};  // Jade
        case 11: return {220, 60, 80, 255};   // Ruby
        case 12: return {80, 120, 255, 255};  // Sapphire
        case 13: return {60, 200, 90, 255};   // Emerald
        case 14: return {240, 220, 80, 255};  // Topaz
        case 15: return {200, 220, 255, 170}; // Glass
        default: return {235, 205, 85, 255};
    }
}

void drawRingAppearance(SpritePixels& s, uint32_t seed, RNG& rng, uint8_t a, int frame) {
    Color base = ringMaterial(a);
    base = add(base, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
    Color dark = mul(base, 0.70f);

    // Band
    circle(s, 8, 9, 4, base);
    circle(s, 8, 9, 3, dark);
    circle(s, 8, 9, 2, {0,0,0,0});

    // Some appearances are gem-like; add a stone.
    const bool gemLike = (a % 16u) >= 8u;
    if (gemLike) {
        Color gem = base;
        // Opal: iridescent shimmer.
        if ((a % 16u) == 8u) {
            gem = (frame % 2 == 0) ? Color{200, 255, 240, 235} : Color{255, 210, 255, 235};
        }
        circle(s, 8, 5, 2, gem);
        circle(s, 8, 5, 1, mul(gem, 0.85f));
    }

    // Highlights
    if (frame % 2 == 1) {
        setPx(s, 9, 7, {255,255,255,110});
        setPx(s, 7, 8, {255,255,255,90});
    }

    (void)seed;
}

Color wandMaterial(uint8_t a) {
    // Mapping matches Game's WAND_APPEARANCES (16 entries).
    switch (static_cast<int>(a % 16u)) {
        case 0:  return {145, 105, 65, 255};  // Oak
        case 1:  return {220, 220, 210, 255}; // Bone
        case 2:  return {235, 225, 200, 255}; // Ivory
        case 3:  return {150, 140, 130, 255}; // Ash
        case 4:  return {55, 45, 40, 255};    // Ebony
        case 5:  return {185, 155, 95, 255};  // Pine
        case 6:  return {140, 190, 120, 255}; // Bamboo
        case 7:  return {160, 90, 60, 255};   // Yew
        case 8:  return {175, 125, 85, 255};  // Maple
        case 9:  return {130, 100, 70, 255};  // Elm
        case 10: return {225, 210, 190, 255}; // Birch
        case 11: return {130, 150, 120, 255}; // Willow
        case 12: return {175, 220, 255, 220}; // Crystal
        case 13: return {50, 40, 60, 255};    // Obsidian
        case 14: return {130, 130, 140, 255}; // Stone
        case 15: return {190, 120, 70, 255};  // Copper
        default: return {145, 105, 65, 255};
    }
}

void drawWandAppearance(SpritePixels& s, uint32_t seed, RNG& rng, uint8_t a, int frame) {
    Color mat = wandMaterial(a);
    Color mat2 = mul(mat, 0.80f);
    Color tip = {255, 255, 255, 200};
    if ((a % 16u) == 12u) tip = {180, 240, 255, 210};      // crystal
    else if ((a % 16u) == 13u) tip = {200, 120, 255, 200}; // obsidian
    else if ((a % 16u) == 15u) tip = {255, 200, 120, 210}; // copper

    // Shaft (diagonal) + thickness.
    line(s, 4, 12, 12, 4, mat);
    line(s, 4, 13, 13, 4, mat2);

    // Grip / wrap
    rect(s, 5, 11, 2, 2, mul(mat, 0.70f));

    // Tip ornament
    circle(s, 12, 4, 1, tip);
    if (frame % 2 == 1) {
        setPx(s, 13, 4, {255,255,255,150});
    }

    // Tiny rune notches along the shaft (deterministic).
    const uint32_t h = hash32(hashCombine(seed, static_cast<uint32_t>(a) * 0x9E37u));
    for (int i = 0; i < 3; ++i) {
        const int t = 2 + i * 3;
        const int x = 4 + t;
        const int y = 12 - t;
        if ((h >> i) & 1u) setPx(s, x, y, {30, 25, 20, 200});
    }

    // Subtle sparkle for magical materials.
    if ((a % 16u) >= 12u && (frame % 2 == 1)) {
        setPx(s, 10, 6, {255,255,255,110});
    }

    (void)rng;
}


float densityFor(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return 0.55f;
        case EntityKind::Goblin: return 0.58f;
        case EntityKind::Leprechaun: return 0.50f;
        case EntityKind::Zombie: return 0.60f;
        case EntityKind::Orc: return 0.62f;
        case EntityKind::Bat: return 0.40f;
        case EntityKind::Slime: return 0.70f;
        case EntityKind::SkeletonArcher: return 0.52f;
        case EntityKind::KoboldSlinger: return 0.50f;
        case EntityKind::Wolf: return 0.55f;
        case EntityKind::Dog: return 0.52f;
        case EntityKind::Troll: return 0.68f;
        case EntityKind::Wizard: return 0.50f;
        case EntityKind::Ghost: return 0.42f;
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
        case EntityKind::Leprechaun: return add({ 60, 210, 90, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Zombie: return add({ 120, 180, 120, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Orc: return add({ 70, 150, 60, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Bat: return add({ 120, 100, 140, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Slime: return add({ 70, 200, 160, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::SkeletonArcher: return add({ 200, 200, 190, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
        case EntityKind::KoboldSlinger: return add({ 180, 120, 70, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
        case EntityKind::Wolf: return add({ 150, 150, 160, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Dog: return add({ 180, 140, 90, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Troll: return add({ 90, 170, 90, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Wizard: return add({ 140, 100, 200, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Ghost: return add({ 210, 230, 255, 190 }, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
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


SpritePixels generateEntitySprite(EntityKind kind, uint32_t seed, int frame, bool use3d, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    // Base shape from seed (stable), subtle variation from frame.
    RNG rngBase(hash32(seed));
    RNG rngVar(hashCombine(seed, static_cast<uint32_t>(0xA5F00Du + frame * 1337u)));

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // 8x8 mask, mirrored horizontally.
    bool m[8][8] = {};
    bool lock[8][8] = {}; // template pixels we always keep

    auto mark = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= 8 || y >= 8) return;
        m[y][x] = true;
        m[y][7 - x] = true;
        lock[y][x] = true;
        lock[y][7 - x] = true;
    };

    // A tiny silhouette template per monster family for readability.
    auto addHumanoid = [&]() {
        // Head
        mark(3, 1); mark(4, 1);
        mark(3, 2); mark(4, 2);
        mark(2, 2); mark(5, 2);
        // Torso
        for (int y = 3; y <= 5; ++y) { mark(3, y); mark(4, y); }
        mark(2, 4); mark(5, 4); // arms
        // Legs
        mark(3, 6); mark(4, 6);
        mark(3, 7); mark(4, 7);
    };

    auto addBigHumanoid = [&]() {
        addHumanoid();
        // Wider shoulders/torso
        mark(2, 3); mark(5, 3);
        mark(2, 5); mark(5, 5);
        // Wider legs
        mark(2, 7); mark(5, 7);
    };

    auto addQuadruped = [&]() {
        // Body
        for (int x = 2; x <= 5; ++x) { mark(x, 5); mark(x, 6); }
        // Neck/head (front)
        mark(1, 4); mark(2, 4);
        mark(1, 5);
        // Legs
        mark(2, 7); mark(4, 7); mark(5, 7);
    };

    auto addBat = [&]() {
        // Body
        mark(3, 4); mark(4, 4);
        mark(3, 5); mark(4, 5);
        // Wings
        for (int x = 0; x <= 2; ++x) { mark(x, 3); mark(x, 4); }
        mark(1, 2); mark(2, 2);
        mark(0, 5); mark(1, 6); mark(2, 6);
    };

    auto addBlob = [&]() {
        for (int y = 3; y <= 7; ++y) {
            for (int x = 2; x <= 5; ++x) mark(x, y);
        }
        // Round the top
        mark(3, 2); mark(4, 2);
    };

    auto addSnake = [&]() {
        // Curvy body
        mark(2, 5); mark(3, 5); mark(4, 5); mark(5, 5);
        mark(2, 6); mark(3, 6); mark(4, 6);
        mark(3, 4); mark(4, 4);
        // Head
        mark(5, 4);
    };

    auto addSpider = [&]() {
        // Body + head
        mark(3, 5); mark(4, 5);
        mark(3, 4); mark(4, 4);
        mark(3, 6); mark(4, 6);
        // Legs
        mark(1, 4); mark(2, 3);
        mark(1, 6); mark(2, 7);
    };

    auto addChest = [&]() {
        // Mimic: chunky chest silhouette.
        for (int x = 2; x <= 5; ++x) { mark(x, 6); mark(x, 7); }
        for (int x = 2; x <= 5; ++x) { mark(x, 5); }
        // Lid
        for (int x = 2; x <= 5; ++x) { mark(x, 4); }
    };

    switch (kind) {
        case EntityKind::Bat: addBat(); break;
        case EntityKind::Slime: addBlob(); break;
        case EntityKind::Wolf:
        case EntityKind::Dog: addQuadruped(); break;
        case EntityKind::Snake: addSnake(); break;
        case EntityKind::Spider: addSpider(); break;
        case EntityKind::Mimic: addChest(); break;
        case EntityKind::Troll:
        case EntityKind::Ogre:
        case EntityKind::Minotaur: addBigHumanoid(); break;
        default: addHumanoid(); break;
    }

    // Random fill to add texture/variation.
    float density = densityFor(kind);
    // Keep templates readable: let random fill be slightly less aggressive.
    density = std::clamp(density, 0.35f, 0.80f);

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) {
            bool on = rngBase.chance(density);
            if (lock[y][x]) on = true;
            m[y][x] = m[y][x] || on;
            m[y][7 - x] = m[y][x];
        }
    }

    // A couple cellular-automata smoothing passes remove singletons and fill holes.
    auto countN = [&](int x, int y) {
        int c = 0;
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                if (ox == 0 && oy == 0) continue;
                int xx = x + ox;
                int yy = y + oy;
                if (xx < 0 || yy < 0 || xx >= 8 || yy >= 8) continue;
                if (m[yy][xx]) ++c;
            }
        }
        return c;
    };

    for (int iter = 0; iter < 2; ++iter) {
        bool tmp[8][8] = {};
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                if (lock[y][x]) { tmp[y][x] = true; continue; }
                const int n = countN(x, y);
                if (m[y][x]) tmp[y][x] = (n >= 2);
                else tmp[y][x] = (n >= 5);
            }
        }
        // Keep symmetry exact.
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 4; ++x) {
                tmp[y][7 - x] = tmp[y][x];
            }
        }
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                m[y][x] = tmp[y][x];
            }
        }
    }

    Color base = baseColorFor(kind, rngBase);

    // Expand mask into 16x16 with chunky pixels, but shade using a quantized ramp + dithering.
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            if (!m[y][x]) continue;
            int px = x * 2;
            int py = y * 2;

            for (int oy = 0; oy < 2; ++oy) {
                for (int ox = 0; ox < 2; ++ox) {
                    const int xx = px + ox;
                    const int yy = py + oy;

                    // Lighting: top-left biased + subtle spherical highlight.
                    const float lx = (15.0f - xx) / 15.0f;
                    const float ly = (15.0f - yy) / 15.0f;
                    float shade = 0.58f + 0.22f * ly + 0.10f * lx;

                    const float cx = (xx - 7.5f) / 7.5f;
                    const float cy = (yy - 8.0f) / 8.0f;
                    const float d2 = cx * cx + cy * cy;
                    const float sphere = (d2 < 1.0f) ? std::sqrt(1.0f - d2) : 0.0f;
                    shade *= (0.78f + 0.30f * sphere);

                    // Seeded micro-noise so large flat areas don't band.
                    const uint32_t n = hashCombine(seed, static_cast<uint32_t>(xx + yy * 17 + frame * 131));
                    const float noise = (n & 0xFFu) / 255.0f;
                    shade *= (0.90f + 0.18f * noise);

                    setPx(s, xx, yy, rampShade(base, shade, xx, yy));
                }
            }
        }
    }


    // Extra depth: inner ambient-occlusion along the silhouette makes sprites readable
    // even on high-detail dungeon tiles.
    {
        SpritePixels orig = s;
        for (int y = 0; y < s.h; ++y) {
            for (int x = 0; x < s.w; ++x) {
                const Color c0 = orig.at(x, y);
                if (c0.a == 0) continue;

                int open = 0;
                const int nx[4] = { x + 1, x - 1, x, x };
                const int ny[4] = { y, y, y + 1, y - 1 };
                for (int i = 0; i < 4; ++i) {
                    const int xx = nx[i];
                    const int yy = ny[i];
                    if (xx < 0 || yy < 0 || xx >= s.w || yy >= s.h) { open++; continue; }
                    if (orig.at(xx, yy).a == 0) open++;
                }

                if (open > 0) {
                    float f = 1.0f - 0.04f * static_cast<float>(open);
                    if (f < 0.82f) f = 0.82f;
                    s.at(x, y) = mul(c0, f);
                }
            }
        }
    }

    // Add eyes-ish for living things (only if inside the body).
    if (kind != EntityKind::Slime && kind != EntityKind::Mimic) {
        int ey = 6 + (rngVar.range(-1, 1));
        int ex = 6;
        auto safeEye = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= 16 || y >= 16) return false;
            return s.at(x, y).a != 0;
        };

        // If the default spot isn't inside the sprite, nudge downward a bit.
        if (!safeEye(ex, ey) || !safeEye(ex + 3, ey)) {
            ey = 7;
        }
        if (safeEye(ex, ey) && safeEye(ex + 3, ey)) {
            setPx(s, ex, ey, {255,255,255,255});
            setPx(s, ex+3, ey, {255,255,255,255});
            setPx(s, ex, ey+1, {0,0,0,255});
            setPx(s, ex+3, ey+1, {0,0,0,255});
        }
    } else if (kind == EntityKind::Slime) {
        // Slime: two bright blobs.
        setPx(s, 6, 7, {230,255,255,200});
        setPx(s, 9, 7, {230,255,255,200});
    }

    // Kind-specific accents
    if (kind == EntityKind::Bat) {
        // Wing flaps (frame toggles)
        int y = (frame % 2 == 0) ? 6 : 7;
        setPx(s, 1, y, mul(base, 0.55f));
        setPx(s, 14, y, mul(base, 0.55f));
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
    if (kind == EntityKind::Dog) {
        // Nose + a tiny collar.
        setPx(s, 8, 10, {30,30,30,255});
        rect(s, 7, 12, 3, 1, {220, 40, 40, 255});
        setPx(s, 8, 13, {240, 200, 80, 255});
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
        if (frame % 2 == 1) {
            setPx(s, 8, 11, {220,80,80,255});
            setPx(s, 9, 11, {220,80,80,255});
        }
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

    // Final pass: readable outlines + shadow.

    // Humanoid gear overlays: breaks symmetry and gives the procedural silhouettes a bit more
    // "character" (weapon/staff/shield). This is purely cosmetic.
    {
        const bool rightHand = ((seed >> 5) & 1u) != 0u;
        // Small bob so gear isn't perfectly static across frames.
        const int wobble = (frame % 2 == 1) ? 1 : 0;

        auto drawBlade = [&](int x0, int y0, int dx, int dy, int len, Color metal, Color grip) {
            // Handle
            setPx(s, x0, y0, grip);
            setPx(s, x0 - dx, y0 - dy, grip);

            // Blade
            for (int i = 1; i <= len; ++i) {
                setPx(s, x0 + dx * i, y0 + dy * i, metal);
            }

            // Specular tick.
            setPx(s, x0 + dx * (len - 1), y0 + dy * (len - 1), add(metal, 30, 30, 30));
        };

        auto drawAxe = [&](int x0, int y0, int dir, Color metal, Color grip) {
            // Shaft
            line(s, x0, y0, x0, y0 - 5, grip);
            // Head
            setPx(s, x0 + dir, y0 - 4, metal);
            setPx(s, x0 + dir, y0 - 3, metal);
            setPx(s, x0 + dir * 2, y0 - 4, mul(metal, 0.85f));
            setPx(s, x0 + dir * 2, y0 - 3, mul(metal, 0.75f));
            setPx(s, x0, y0 - 5, add(metal, 20, 20, 25));
        };

        auto drawClub = [&](int x0, int y0, int dir, Color wood) {
            line(s, x0, y0, x0 + dir * 2, y0 - 5, wood);
            setPx(s, x0 + dir * 2, y0 - 5, add(wood, 18, 12, 6));
            setPx(s, x0 + dir * 2, y0 - 4, mul(wood, 0.75f));
            setPx(s, x0 + dir, y0 - 4, mul(wood, 0.85f));
        };

        auto drawStaff = [&](int x0, int y0, int dir, Color wood, Color orb) {
            line(s, x0, y0, x0 + dir, y0 - 7, wood);
            circle(s, x0 + dir, y0 - 7, 1, orb);
            setPx(s, x0 + dir + (dir > 0 ? 1 : -1), y0 - 7, {255,255,255,120});
        };

        auto drawShield = [&](int x0, int y0, Color body) {
            Color dark = mul(body, 0.70f);
            outlineRect(s, x0, y0, 3, 5, dark);
            rect(s, x0 + 1, y0 + 1, 1, 3, body);
            setPx(s, x0 + 1, y0 + 2, add(body, 18, 18, 18));
        };

        // Seeded colors for gear.
        Color metal = add(Color{210, 215, 225, 255}, rngVar.range(-12, 12), rngVar.range(-12, 12), rngVar.range(-12, 12));
        Color grip  = add(Color{110, 75, 40, 255}, rngVar.range(-10, 10), rngVar.range(-10, 10), rngVar.range(-10, 10));
        Color wood  = add(Color{120, 80, 45, 255}, rngVar.range(-12, 12), rngVar.range(-12, 12), rngVar.range(-12, 12));

        const int dir = rightHand ? 1 : -1;
        const int handX = rightHand ? 11 : 4;
        const int handY = 12 - wobble;

        switch (kind) {
            case EntityKind::Player: {
                drawBlade(handX, handY, dir, -1, 4, metal, grip);
                drawShield(rightHand ? 2 : 11, 8, add({90, 120, 160, 255}, rngVar.range(-10, 10), rngVar.range(-10, 10), rngVar.range(-10, 10)));
                break;
            }
            case EntityKind::Goblin: {
                drawBlade(handX, handY, dir, -1, 3, mul(metal, 0.90f), grip);
                break;
            }
            case EntityKind::Orc: {
                drawAxe(handX, handY, dir, metal, grip);
                drawShield(rightHand ? 2 : 11, 8, add({100, 110, 120, 255}, rngVar.range(-10, 10), rngVar.range(-10, 10), rngVar.range(-10, 10)));
                break;
            }
            case EntityKind::Troll:
            case EntityKind::Ogre: {
                drawClub(handX, handY, dir, wood);
                break;
            }
            case EntityKind::Minotaur: {
                drawAxe(handX, handY, dir, add(metal, 10, 10, 0), grip);
                // Bigger shield-ish chunk for silhouette.
                drawShield(rightHand ? 1 : 12, 7, add({120, 90, 70, 255}, rngVar.range(-12, 12), rngVar.range(-12, 12), rngVar.range(-12, 12)));
                break;
            }
            case EntityKind::Wizard: {
                Color orb = add({180, 120, 255, 230}, rngVar.range(-10, 10), rngVar.range(-10, 10), rngVar.range(-10, 10));
                drawStaff(handX, handY, dir, wood, orb);
                break;
            }
            case EntityKind::Shopkeeper: {
                // Coin-pouch / jingling keys.
                Color gold = {235, 205, 95, 240};
                circle(s, rightHand ? 11 : 4, 12, 1, gold);
                setPx(s, rightHand ? 10 : 5, 12, {255,255,255,110});
                break;
            }
            default:
                break;
        }
    }

    if (kind == EntityKind::Ghost) {
        // Make ghosts more ethereal: fade out toward the bottom.
        for (int y = 0; y < 16; ++y) {
            const float t = y / 15.0f;
            const float fade = 1.0f - 0.55f * t;
            for (int x = 0; x < 16; ++x) {
                Color c = getPx(s, x, y);
                if (c.a == 0) continue;
                c.a = static_cast<uint8_t>(static_cast<float>(c.a) * fade);
                setPx(s, x, y, c);
            }
        }
        finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/55);
    } else {
        finalizeSprite(s, seed, frame, /*outlineAlpha=*/255, /*shadowAlpha=*/90);
    }
    return use3d ? renderSprite3DEntity(kind, s, seed, frame, pxSize)
                 : resampleSpriteToSize(s, pxSize);
}


SpritePixels generateItemSprite(ItemKind kind, uint32_t seed, int frame, bool use3d, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    RNG rng(hash32(seed));
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // NetHack-style identification visuals:
    // if the renderer sets SPRITE_SEED_IDENT_APPEARANCE_FLAG, we generate
    // appearance-based art for identifiable items (potion/scroll/ring/wand)
    // so the sprite itself doesn't leak the true item kind.
    if ((seed & SPRITE_SEED_IDENT_APPEARANCE_FLAG) != 0u) {
        const uint8_t app = static_cast<uint8_t>(seed & 0xFFu);
        if (isPotionKind(kind)) {
            drawPotionAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? renderSprite3DItem(kind, s, seed, frame, pxSize)
                         : resampleSpriteToSize(s, pxSize);
        }
        if (isScrollKind(kind)) {
            drawScrollAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? renderSprite3DItem(kind, s, seed, frame, pxSize)
                         : resampleSpriteToSize(s, pxSize);
        }
        if (kind == ItemKind::RingMight || kind == ItemKind::RingAgility || kind == ItemKind::RingFocus ||
            kind == ItemKind::RingProtection) {
            drawRingAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? renderSprite3DItem(kind, s, seed, frame, pxSize)
                         : resampleSpriteToSize(s, pxSize);
        }
        if (kind == ItemKind::WandSparks || kind == ItemKind::WandDigging || kind == ItemKind::WandFireball) {
            drawWandAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? renderSprite3DItem(kind, s, seed, frame, pxSize)
                         : resampleSpriteToSize(s, pxSize);
        }
    }

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
        case ItemKind::PotionLevitation: {
            // Light, airy potion: pale sky fluid + tiny upward arrow shimmer.
            Color glass = {200,200,220,180};
            Color fluid = {175,205,255,200};
            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});

            if (frame % 2 == 1) {
                // Up-arrow sparkle
                setPx(s, 8, 7, {255,255,255,170});
                setPx(s, 8, 6, {255,255,255,120});
                setPx(s, 7, 7, {255,255,255,120});
                setPx(s, 9, 7, {255,255,255,120});
                // Glass highlight
                setPx(s, 9, 6, {255,255,255,170});
            }
            break;
        }
        case ItemKind::PotionHallucination: {
            // Kaleidoscopic potion: prismatic fluid + drifting sparkles.
            Color glass = {200,200,220,180};

            // Create a shifting rainbow-ish fluid by mixing two colors based on frame + seed.
            uint32_t h = hash32(seed ^ 0xA11u);
            Color c1 = { static_cast<uint8_t>(80 + (h & 0x7F)),
                         static_cast<uint8_t>(80 + ((h >> 7) & 0x7F)),
                         static_cast<uint8_t>(80 + ((h >> 14) & 0x7F)),
                         200 };
            Color c2 = { static_cast<uint8_t>(80 + ((h >> 21) & 0x7F)),
                         static_cast<uint8_t>(80 + ((h >> 5) & 0x7F)),
                         static_cast<uint8_t>(80 + ((h >> 12) & 0x7F)),
                         200 };

            float t = (frame % 4) * 0.25f;
            Color fluid = {
                static_cast<uint8_t>(c1.r * (1.0f - t) + c2.r * t),
                static_cast<uint8_t>(c1.g * (1.0f - t) + c2.g * t),
                static_cast<uint8_t>(c1.b * (1.0f - t) + c2.b * t),
                200
            };

            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});

            // Sparkles that drift as the animation frames tick.
            uint32_t sh = hash32(seed ^ (0xBEEFu + static_cast<uint32_t>(frame)));
            for (int i = 0; i < 3; ++i) {
                int sx = 7 + static_cast<int>((sh >> (i * 5)) & 1u);
                int sy = 6 + static_cast<int>((sh >> (i * 7)) % 6u);
                setPx(s, sx, sy, {255,255,255,140});
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

        case ItemKind::ScrollFear: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);

            // A simple "scared face" glyph.
            const Color ink = {80,50,30,255};
            setPx(s, 7, 7, ink);
            setPx(s, 9, 7, ink);
            setPx(s, 8, 8, ink);
            line(s, 7, 9, 9, 9, ink);
            setPx(s, 8, 10, ink);

            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }

        case ItemKind::ScrollEarth: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);

            // A small "mountain" / boulder glyph.
            const Color ink = {80,50,30,255};
            line(s, 6, 10, 8, 7, ink);
            line(s, 8, 7, 10, 10, ink);
            line(s, 6, 10, 10, 10, ink);
            setPx(s, 8, 9, ink);

            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,120});
            break;
        }

        case ItemKind::ScrollTaming: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);

            // A tiny heart / charm glyph.
            const Color ink = {80,50,30,255};
            // two bumps
            setPx(s, 7, 7, ink);
            setPx(s, 9, 7, ink);
            setPx(s, 6, 8, ink);
            setPx(s, 8, 8, ink);
            setPx(s, 10, 8, ink);
            // point
            setPx(s, 7, 9, ink);
            setPx(s, 9, 9, ink);
            setPx(s, 8, 10, ink);

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

    // Post-process: subtle outline + shadow for readability on noisy floors.
    finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);

    return use3d ? renderSprite3DItem(kind, s, seed, frame, pxSize)
                 : resampleSpriteToSize(s, pxSize);
}

SpritePixels generateProjectileSprite(ProjectileKind kind, uint32_t seed, int frame, bool use3d, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
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
        case ProjectileKind::Torch: {
            // A small stick with a flickering flame.
            Color wood = {120, 80, 45, 255};
            // handle
            line(s, 6, 12, 10, 6, wood);
            line(s, 6, 13, 9, 7, mul(wood, 0.85f));
            // flame
            Color outer = {220, 90, 40, 220};
            Color core  = {255, 220, 160, 255};
            circle(s, 10, 5, 2, outer);
            setPx(s, 10, 4, core);
            if (frame % 2 == 1) {
                setPx(s, 11, 4, {255,255,255,140});
                setPx(s, 9, 5, {255,200,140,140});
            } else {
                setPx(s, 9, 4, {255,210,150,120});
            }
            break;
        }
        default:
            break;
    }

    // Post-process: a crisp outline keeps fast projectiles readable.
    finalizeSprite(s, seed, frame, /*outlineAlpha=*/200, /*shadowAlpha=*/55);

    return use3d ? renderSprite3DProjectile(kind, s, seed, frame, pxSize)
                 : resampleSpriteToSize(s, pxSize);
}


SpritePixels generateFloorTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});
    RNG rng(hash32(seed));

    Color base = { 92, 82, 64, 255 };
    base = add(base, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));

    // Coarse 4x4 "stone patches" + fine noise. This reads as cobble/grain instead of flat static.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int cx = x / 4;
            const int cy = y / 4;

            uint32_t cN = hashCombine(seed ^ 0x51F00u, static_cast<uint32_t>(cx + cy * 7));
            float cell = (cN & 0xFFu) / 255.0f;
            float cellF = 0.85f + cell * 0.25f;

            uint32_t n = hashCombine(seed, static_cast<uint32_t>(x + y * 17 + frame * 131));
            float noise = (n & 0xFFu) / 255.0f;
            float f = cellF * (0.80f + noise * 0.30f);

            // Directional light bias (top-left brighter) so the dungeon doesn't feel flat.
            const float lx = (15.0f - x) / 15.0f;
            const float ly = (15.0f - y) / 15.0f;
            f *= (0.92f + 0.08f * (0.60f * lx + 0.40f * ly));

            // Subtle vignette keeps tiles centered.
            float vx = (x - 7.5f) / 7.5f;
            float vy = (y - 7.5f) / 7.5f;
            f *= 1.0f - 0.08f * (vx * vx + vy * vy);

            // Tiny edge darkening reduces the "flat" look and helps tile seams read
            // without adding hard grid-lines (kept subtle so it doesn't look like a
            // checkerboard when tiled).
            if (x == 0 || y == 0 || x == 15 || y == 15) {
                f *= 0.95f;
            }

            s.at(x, y) = rampShadeTile(base, f * 0.90f, x, y);
        }
    }

    // Pebbles / chips
    for (int i = 0; i < 18; ++i) {
        int x = rng.range(0, 15);
        int y = rng.range(0, 15);
        s.at(x, y) = add(s.at(x, y), rng.range(-22, 22), rng.range(-22, 22), rng.range(-22, 22));
    }

    // Hairline cracks (blended so they don't look like hard grid-lines).
    Color crack = mul(base, 0.55f);
    crack.a = 170;
    for (int i = 0; i < 2; ++i) {
        int x0 = rng.range(0, 15);
        int y0 = rng.range(0, 15);
        int x1 = std::clamp(x0 + rng.range(-10, 10), 0, 15);
        int y1 = std::clamp(y0 + rng.range(-10, 10), 0, 15);
        lineBlend(s, x0, y0, x1, y1, crack);
    }

    // Subtle animated "glint" pixels (torchlight shimmer).
    if (frame % 2 == 1) {
        RNG g(hash32(seed ^ 0xF17A4u));
        for (int i = 0; i < 3; ++i) {
            int x = g.range(0, 15);
            int y = g.range(0, 15);
            s.at(x, y) = add(s.at(x, y), 35, 35, 35);
        }
        int sx = g.range(1, 14);
        int sy = g.range(1, 14);
        setPx(s, sx, sy, add(s.at(sx, sy), 20, 20, 20));
        setPx(s, sx + 1, sy, add(s.at(sx + 1, sy), 14, 14, 14));
    }

    return resampleSpriteToSize(s, pxSize);
}


// Themed floor tile. This intentionally keeps each theme fairly dark so that
// entities/items remain readable, but changes material + micro-detail so that
// special rooms stand out instantly.
// style mapping:
//  0 = Normal, 1 = Treasure, 2 = Lair, 3 = Shrine, 4 = Secret, 5 = Vault, 6 = Shop
SpritePixels generateThemedFloorTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    if (style == 0u) {
        return generateFloorTile(seed, frame, pxSize);
    }

    SpritePixels s = makeSprite(16, 16, {0,0,0,255});

    const uint32_t sMix = static_cast<uint32_t>(style) * 0x9E3779B9u;
    RNG rng(hash32(seed ^ sMix));

    // Defaults (overridden per style)
    Color base{ 82, 74, 60, 255 };
    Color accent{ 130, 120, 85, 255 };
    float noiseGain = 0.30f;
    float patchGain = 0.25f;
    float edgeDark = 0.12f;

    switch (style) {
        case 1: // Treasure
            base   = { 86, 74, 50, 255 };
            accent = { 235, 205, 120, 255 };
            noiseGain = 0.26f;
            patchGain = 0.22f;
            break;
        case 2: // Lair
            base   = { 64, 58, 46, 255 };
            accent = { 90, 120, 75, 255 };
            noiseGain = 0.36f;
            patchGain = 0.30f;
            edgeDark = 0.16f;
            break;
        case 3: // Shrine
            base   = { 72, 78, 92, 255 };
            accent = { 150, 210, 255, 255 };
            noiseGain = 0.22f;
            patchGain = 0.18f;
            break;
        case 4: // Secret
            base   = { 58, 62, 52, 255 };
            accent = { 90, 140, 90, 255 };
            noiseGain = 0.34f;
            patchGain = 0.26f;
            edgeDark = 0.18f;
            break;
        case 5: // Vault
            base   = { 78, 84, 96, 255 };
            accent = { 200, 220, 245, 255 };
            noiseGain = 0.18f;
            patchGain = 0.12f;
            edgeDark = 0.10f;
            break;
        case 6: // Shop
            base   = { 78, 58, 36, 255 };
            accent = { 125, 90, 55, 255 };
            noiseGain = 0.22f;
            patchGain = 0.10f;
            edgeDark = 0.10f;
            break;
        default:
            break;
    }

    // Light base variation per-variant seed.
    base = add(base, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));

    if (style == 6u) {
        // Shop: wood planks (horizontal).
        // The dithering ramp keeps it looking like pixel-art rather than a smooth gradient.
        for (int y = 0; y < 16; ++y) {
            const int plank = y / 4; // 4px planks
            const bool seam = (y % 4) == 0;
            for (int x = 0; x < 16; ++x) {
                uint32_t n = hashCombine(seed ^ (0xB00Du + sMix), static_cast<uint32_t>(x + y * 23 + frame * 101));
                float noise = (n & 0xFFu) / 255.0f;

                // Gentle grain running along x.
                const float gx = std::sin((static_cast<float>(x) * 0.55f) + (static_cast<float>(plank) * 1.2f) + (seed & 0xFFu) * 0.04f);
                float f = 0.76f + gx * 0.06f + (noise - 0.5f) * noiseGain;

                // Plank-to-plank contrast.
                const float pVar = 0.96f + 0.04f * std::sin(static_cast<float>(plank) * 2.1f + (seed & 0x3Fu) * 0.2f);
                f *= pVar;

                // Seams between planks.
                if (seam) f *= 0.70f;

                // Slight edge darkening.
                if (x == 0 || y == 0 || x == 15 || y == 15) f *= (1.0f - edgeDark);

                s.at(x, y) = rampShadeTile(base, f, x, y);
            }
        }

        // Occasional nails / knots.
        for (int i = 0; i < 5; ++i) {
            const int x = rng.range(1, 14);
            const int y = (rng.range(0, 3) * 4) + rng.range(1, 2);
            Color nail = mul(accent, 0.45f);
            nail = add(nail, 25, 25, 25);
            setPx(s, x, y, nail);
        }

        // Small rug hint (soft red stripe) sometimes.
        if ((hash32(seed ^ 0x5A0F5u) & 1u) == 1u) {
            const int cx = 8;
            const int cy = 8;
            Color rug{ 90, 35, 35, 120 };
            if (frame % 2 == 1) rug.a = 135;
            for (int y = 4; y <= 11; ++y) {
                for (int x = 4; x <= 11; ++x) {
                    const int dx = x - cx;
                    const int dy = y - cy;
                    if (dx * dx + dy * dy > 18) continue;
                    blendPx(s, x, y, rug);
                }
            }
        }

        return resampleSpriteToSize(s, pxSize);
    }

    // Stone-like base fill (used by all other themed floors).
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int cx = x / 4;
            const int cy = y / 4;

            const uint32_t cN = hashCombine(seed ^ (0x51F00u + sMix), static_cast<uint32_t>(cx + cy * 7));
            const float cell = (cN & 0xFFu) / 255.0f;
            const float cellF = 0.86f + cell * patchGain;

            const uint32_t n = hashCombine(seed ^ (0xF1000u + sMix), static_cast<uint32_t>(x + y * 17 + frame * 131));
            const float noise = (n & 0xFFu) / 255.0f;
            float f = cellF * (0.80f + (noise - 0.5f) * noiseGain);

            // Directional light bias (top-left brighter)
            const float lx = (15.0f - x) / 15.0f;
            const float ly = (15.0f - y) / 15.0f;
            f *= (0.92f + 0.08f * (0.60f * lx + 0.40f * ly));

            // Subtle vignette
            const float vx = (x - 7.5f) / 7.5f;
            const float vy = (y - 7.5f) / 7.5f;
            f *= 1.0f - 0.08f * (vx * vx + vy * vy);

            // Edge darkening (helps "tile" separation)
            if (x == 0 || y == 0 || x == 15 || y == 15) f *= (1.0f - edgeDark);

            // Shrine: add a marble vein field.
            if (style == 3u) {
                const float v = std::sin((x * 0.7f + y * 1.1f) + (seed & 0xFFu) * 0.08f);
                f *= (0.98f + 0.04f * v);
            }

            s.at(x, y) = rampShadeTile(base, f * 0.95f, x, y);
        }
    }

    // Style-specific overlays.
    if (style == 1u) {
        // Treasure: gold inlays + sparkles.
        Color inlay = mul(accent, 0.55f);
        inlay.a = 140;
        Color inlay2 = mul(accent, 0.35f);
        inlay2.a = 110;

        // A few thin inlay lines.
        for (int i = 0; i < 3; ++i) {
            const int x0 = rng.range(1, 14);
            const int y0 = rng.range(1, 14);
            const int x1 = std::clamp(x0 + rng.range(-8, 8), 1, 14);
            const int y1 = std::clamp(y0 + rng.range(-8, 8), 1, 14);
            lineBlend(s, x0, y0, x1, y1, (i % 2 == 0) ? inlay : inlay2);
        }

        // Sparkle pips.
        if (frame % 2 == 1) {
            for (int i = 0; i < 4; ++i) {
                const int x = rng.range(2, 13);
                const int y = rng.range(2, 13);
                s.at(x, y) = add(s.at(x, y), 28, 28, 18);
                setPx(s, x + 1, y, add(s.at(std::min(15, x + 1), y), 16, 16, 10));
            }
        }
    } else if (style == 2u) {
        // Lair: grime + mossy stains.
        Color stain{ 35, 60, 35, 120 };
        for (int i = 0; i < 4; ++i) {
            const int cx = rng.range(2, 13);
            const int cy = rng.range(2, 13);
            const int rr = rng.range(2, 4);
            for (int y = cy - rr; y <= cy + rr; ++y) {
                for (int x = cx - rr; x <= cx + rr; ++x) {
                    const int dx = x - cx;
                    const int dy = y - cy;
                    if (dx * dx + dy * dy > rr * rr) continue;
                    blendPx(s, x, y, stain);
                }
            }
        }
        // Bone chips / pale grit.
        for (int i = 0; i < 10; ++i) {
            const int x = rng.range(0, 15);
            const int y = rng.range(0, 15);
            s.at(x, y) = add(s.at(x, y), 14, 12, 8);
        }
    } else if (style == 3u) {
        // Shrine: rune ring + soft glows.
        Color rune = mul(accent, 0.35f);
        rune.a = 160;
        Color rune2 = mul(accent, 0.22f);
        rune2.a = 135;

        // Simple ring around the center.
        const int cx = 8;
        const int cy = 8;
        const int r0 = 5;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const int dx = x - cx;
                const int dy = y - cy;
                const int d2 = dx * dx + dy * dy;
                if (d2 >= r0 * r0 - 3 && d2 <= r0 * r0 + 3) {
                    blendPx(s, x, y, ((x + y) & 1) ? rune : rune2);
                }
            }
        }

        // Pulse spark.
        if (frame % 2 == 1) {
            const int x = rng.range(4, 11);
            const int y = rng.range(4, 11);
            blendPx(s, x, y, Color{ 255, 255, 255, 85 });
        }
    } else if (style == 4u) {
        // Secret: moss patches (thresholded noise).
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const uint32_t n = hashCombine(seed ^ (0x6055u + sMix), static_cast<uint32_t>(x + y * 31));
                const uint8_t v = static_cast<uint8_t>(n & 0xFFu);
                if (v < 52u) {
                    Color moss{ 40, 80, 45, 120 };
                    if ((v & 3u) == 0u) moss.a = 150;
                    blendPx(s, x, y, moss);
                }
            }
        }
        // Extra cracks.
        Color crack = mul(base, 0.50f);
        crack.a = 160;
        for (int i = 0; i < 2; ++i) {
            const int x0 = rng.range(0, 15);
            const int y0 = rng.range(0, 15);
            const int x1 = std::clamp(x0 + rng.range(-10, 10), 0, 15);
            const int y1 = std::clamp(y0 + rng.range(-10, 10), 0, 15);
            lineBlend(s, x0, y0, x1, y1, crack);
        }
    } else if (style == 5u) {
        // Vault: polished stone / metal seams.
        Color seam = mul(base, 0.55f);
        seam.a = 200;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                if (x == 0 || y == 0) continue;
                const bool vSeam = (x % 4) == 0;
                const bool hSeam = (y % 4) == 0;
                if (vSeam || hSeam) {
                    blendPx(s, x, y, seam);
                }
            }
        }
        // A few sharp glints on pulse frame.
        if (frame % 2 == 1) {
            for (int i = 0; i < 3; ++i) {
                const int x = (rng.range(1, 3) * 4) - 1;
                const int y = (rng.range(1, 3) * 4) - 1;
                s.at(x, y) = add(s.at(x, y), 30, 30, 38);
            }
        }
    }

    return resampleSpriteToSize(s, pxSize);
}



SpritePixels generateWallTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});
    RNG rng(hash32(seed));

    Color base = { 70, 78, 92, 255 };
    base = add(base, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));

    // Brick pattern with a tiny bevel (top edges lighter, bottom edges darker).
    for (int y = 0; y < 16; ++y) {
        const int rowOffset = (y / 4) % 2 ? 2 : 0;
        const int yIn = y % 4;
        for (int x = 0; x < 16; ++x) {
            bool mortar = false;
            if ((yIn) == 0) mortar = true;
            if (((x + rowOffset) % 6) == 0) mortar = true;

            uint32_t n = hashCombine(seed, static_cast<uint32_t>(x + y * 19));
            float noise = (n & 0xFFu) / 255.0f;
            float nf = 0.86f + noise * 0.22f;

            float f = mortar ? 0.55f : 0.95f;

            if (!mortar) {
                // Bevel: top row of the brick is brighter, bottom row darker.
                if (yIn == 1) f *= 1.10f;
                if (yIn == 3) f *= 0.78f;

                // Slight edge shading around vertical mortar.
                const bool leftMortar  = (((x - 1 + rowOffset) % 6) == 0);
                const bool rightMortar = (((x + 1 + rowOffset) % 6) == 0);
                if (leftMortar)  f *= 1.06f;
                if (rightMortar) f *= 0.88f;
            }

            // Directional light bias (top-left brighter).
            const float lx = (15.0f - x) / 15.0f;
            const float ly = (15.0f - y) / 15.0f;
            f *= (0.93f + 0.07f * (0.55f * lx + 0.45f * ly));

            s.at(x, y) = rampShadeTile(base, (f * nf) * 0.90f, x, y);
        }
    }

    // Random chips / grime on a handful of brick pixels.
    for (int i = 0; i < 10; ++i) {
        int x = rng.range(1, 14);
        int y = rng.range(1, 14);
        // Avoid mortar-heavy rows so chips don't look like noise.
        if ((y % 4) == 0) continue;
        s.at(x, y) = mul(s.at(x, y), 0.78f);
    }

    // Subtle animated highlight on a few mortar pixels.
    if (frame % 2 == 1) {
        RNG g(hash32(seed ^ 0xBADD1u));
        for (int i = 0; i < 4; ++i) {
            int x = g.range(0, 15);
            int y = g.range(0, 15);
            const int rowOffset = (y / 4) % 2 ? 2 : 0;
            if ((y % 4) == 0 || (((x + rowOffset) % 6) == 0)) {
                s.at(x, y) = add(s.at(x, y), 25, 25, 30);
            } else {
                s.at(x, y) = add(s.at(x, y), 12, 12, 14);
            }
        }
    }

    return resampleSpriteToSize(s, pxSize);
}


SpritePixels generateChasmTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,255});
    RNG rng(hash32(seed));

    // A dark "void" with subtle cool highlights so it reads differently than
    // unexplored black and the regular stone floor.
    Color base = { 10, 14, 28, 255 };
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
            s.at(x, y) = rampShadeTile(base, f * 0.95f, x, y);
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

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels projectToIsometricDiamond(const SpritePixels& src, uint32_t seed, int frame, bool outline) {
    // NOTE: This is a pure pixel-space transform used by the renderer.
    // We keep it deterministic (seed + frame) so capture/replay stays stable.
    if (src.w <= 0 || src.h <= 0) return SpritePixels{};

    const int w = src.w;
    const int h = std::max(1, src.h / 2);

    // First, vertically squash to a 2:1 tile aspect (nearest-neighbor keeps pixel art crisp).
    const SpritePixels squashed = resizeNearest(src, w, h);

    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // Diamond mask + subtle boundary shading (helps the diamond read against adjacent tiles).
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float dx = std::abs(static_cast<float>(x) - cx) / hw;
            const float dy = std::abs(static_cast<float>(y) - cy) / hh;
            const float d = dx + dy;
            if (d > 1.0f) continue;

            Color c = squashed.at(x, y);

            // Fade a touch darker toward the boundary so the silhouette stays crisp.
            if (d > 0.90f) {
                const float t = std::clamp((d - 0.90f) / 0.10f, 0.0f, 1.0f);
                c = mul(c, 1.0f - 0.12f * t);
            }

            // Tiny animated glint along the top ridge (torch shimmer).
            if ((frame % 2 == 1) && (y <= (h / 3)) && (d > 0.86f) && (d < 0.94f)) {
                const uint32_t n = hashCombine(seed ^ 0x15C0u, static_cast<uint32_t>(x + y * 131));
                if ((n & 7u) == 0u) c = add(c, 10, 10, 12);
            }

            out.at(x, y) = c;
        }
    }

    if (outline) {
        // Outline pass: darken pixels that sit on the diamond edge.
        SpritePixels edged = out;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const Color c = out.at(x, y);
                if (c.a == 0) continue;

                // If any 4-neighbor falls outside the diamond, treat as an edge pixel.
                auto inside = [&](int xx, int yy) -> bool {
                    const float ddx = std::abs(static_cast<float>(xx) - cx) / hw;
                    const float ddy = std::abs(static_cast<float>(yy) - cy) / hh;
                    return (ddx + ddy) <= 1.0f;
                };

                bool edge = false;
                if (!inside(x - 1, y)) edge = true;
                else if (!inside(x + 1, y)) edge = true;
                else if (!inside(x, y - 1)) edge = true;
                else if (!inside(x, y + 1)) edge = true;

                if (!edge) continue;

                Color d = mul(c, 0.70f);
                d.a = c.a;

                // Small highlight bias on the top-left edges for depth.
                if (x < static_cast<int>(cx) && y < static_cast<int>(cy) && ((x + y) & 1) == 0) {
                    d = add(d, 6, 6, 8);
                }

                edged.at(x, y) = d;
            }
        }
        out = std::move(edged);
    }

    return out;
}

SpritePixels generateIsometricWallBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    // Build in the 16x16 design grid, then upscale.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed));

    // Base stone palette (close to wall tiles but with stronger face shading).
    Color base = { 70, 78, 92, 255 };
    base = add(base, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));
    Color top = add(mul(base, 1.05f), 10, 10, 14);
    Color left = mul(base, 0.78f);
    Color right = mul(base, 0.88f);
    Color outlineC = mul(base, 0.45f);
    outlineC.a = 255;

    const int W = 16;
    const int H = 16;
    const int topH = 8;
    const int wallH = 8;

    // Helper: scanline-fill a convex quad.
    auto fillQuad = [&](Vec2i p0, Vec2i p1, Vec2i p2, Vec2i p3, const Color& c0, float shadeMul) {
        const Vec2i pts[4] = { p0, p1, p2, p3 };
        int minY = pts[0].y, maxY = pts[0].y;
        for (int i = 1; i < 4; ++i) {
            minY = std::min(minY, pts[i].y);
            maxY = std::max(maxY, pts[i].y);
        }
        minY = std::clamp(minY, 0, H - 1);
        maxY = std::clamp(maxY, 0, H - 1);

        for (int y = minY; y <= maxY; ++y) {
            float xInts[8];
            int nInts = 0;
            for (int e = 0; e < 4; ++e) {
                const Vec2i a = pts[e];
                const Vec2i b = pts[(e + 1) & 3];
                if (a.y == b.y) continue;
                const int y0 = a.y;
                const int y1 = b.y;
                const bool inRange = (y >= std::min(y0, y1)) && (y < std::max(y0, y1));
                if (!inRange) continue;
                const float t = (static_cast<float>(y) - static_cast<float>(y0)) / (static_cast<float>(y1 - y0));
                xInts[nInts++] = static_cast<float>(a.x) + t * static_cast<float>(b.x - a.x);
            }
            if (nInts < 2) continue;
            float xMin = xInts[0], xMax = xInts[0];
            for (int i = 1; i < nInts; ++i) {
                xMin = std::min(xMin, xInts[i]);
                xMax = std::max(xMax, xInts[i]);
            }
            int xi0 = std::clamp(static_cast<int>(std::floor(xMin)), 0, W - 1);
            int xi1 = std::clamp(static_cast<int>(std::ceil(xMax)), 0, W - 1);
            for (int x = xi0; x <= xi1; ++x) {
                // Micro noise so faces don't look like flat fills.
                const uint32_t n = hashCombine(seed ^ 0xB10Cu, static_cast<uint32_t>(x + y * 37 + frame * 101));
                const float noise = (n & 0xFFu) / 255.0f;
                float f = (0.92f + noise * 0.16f) * shadeMul;
                // Tiny directional bias: upper pixels slightly brighter.
                f *= (0.94f + 0.06f * ((15.0f - static_cast<float>(y)) / 15.0f));
                Color cc = rampShadeTile(c0, f, x, y);
                cc.a = 255;
                s.at(x, y) = cc;
            }
        }
    };

    // Side faces first.
    // Left face quad: L(0,4) -> B(8,7) -> BD(8,15) -> LD(0,12)
    fillQuad({0, 4}, {8, 7}, {8, 15}, {0, 12}, left, 0.95f);
    // Right face quad: R(15,4) -> RD(15,12) -> BD(8,15) -> B(8,7)
    fillQuad({15, 4}, {15, 12}, {8, 15}, {8, 7}, right, 1.00f);

    // Add a few darker brick seams on the side faces.
    for (int y = topH + 1; y < H; ++y) {
        if ((y % 3) != 0) continue;
        for (int x = 0; x < W; ++x) {
            const Color c = s.at(x, y);
            if (c.a == 0) continue;
            s.at(x, y) = mul(c, 0.80f);
        }
    }

    // Top face (diamond) drawn last.
    const float cx = (static_cast<float>(W) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(topH) - 1.0f) * 0.5f;
    const float hw = static_cast<float>(W) * 0.5f;
    const float hh = static_cast<float>(topH) * 0.5f;
    for (int y = 0; y < topH; ++y) {
        for (int x = 0; x < W; ++x) {
            const float dx = std::abs(static_cast<float>(x) - cx) / hw;
            const float dy = std::abs(static_cast<float>(y) - cy) / hh;
            if ((dx + dy) > 1.0f) continue;

            const uint32_t n = hashCombine(seed ^ 0x70F1u, static_cast<uint32_t>(x + y * 53 + frame * 97));
            const float noise = (n & 0xFFu) / 255.0f;

            // Subtle top-left highlight so the block reads as 3D.
            const float lx = (15.0f - static_cast<float>(x)) / 15.0f;
            const float ly = (15.0f - static_cast<float>(y)) / 15.0f;

            float f = 0.88f + noise * 0.18f;
            f *= (0.92f + 0.08f * (0.60f * lx + 0.40f * ly));

            Color cc = rampShadeTile(top, f, x, y);
            cc.a = 255;
            s.at(x, y) = cc;
        }
    }

    // Outline cube edges.
    line(s, 8, 0, 0, 4, outlineC);   // top-left
    line(s, 8, 0, 15, 4, outlineC);  // top-right
    line(s, 0, 4, 8, 7, outlineC);   // left->bottom (top)
    line(s, 15, 4, 8, 7, outlineC);  // right->bottom (top)
    line(s, 0, 4, 0, 12, outlineC);  // left vertical
    line(s, 15, 4, 15, 12, outlineC);// right vertical
    line(s, 8, 7, 8, 15, outlineC);  // middle vertical
    line(s, 0, 12, 8, 15, outlineC); // bottom-left
    line(s, 15, 12, 8, 15, outlineC);// bottom-right

    // Tiny flicker glint on the top ridge.
    if (frame % 2 == 1) {
        setPx(s, 8, 1, add(s.at(8, 1), 18, 18, 22));
        setPx(s, 9, 2, add(s.at(9, 2), 10, 10, 12));
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generatePillarTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    RNG rng(hash32(seed));

    // Pillars are rendered as a transparent overlay layered on top of the
    // underlying themed floor (handled by the renderer). This keeps pillars
    // consistent across room floor styles.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    Color stone = { 128, 132, 145, 255 };
    stone = add(stone, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));
    Color dark = mul(stone, 0.62f);
    Color light = add(mul(stone, 1.10f), 10, 10, 12);

    // Soft shadow on the floor (semi-transparent so the floor shows through).
    for (int y = 10; y < 15; ++y) {
        for (int x = 3; x < 13; ++x) {
            float cx = (x - 7.5f) / 5.5f;
            float cy = (y - 12.5f) / 3.0f;
            float d2 = cx*cx + cy*cy;
            if (d2 > 1.0f) continue;
            int a = static_cast<int>(std::lround((1.0f - d2) * 110.0f));
            a = std::clamp(a, 0, 110);
            setPx(s, x, y, Color{0, 0, 0, static_cast<uint8_t>(a)});
        }
    }

    // Pillar body (a simple column).
    outlineRect(s, 5, 2, 6, 13, dark);
    rect(s, 6, 3, 4, 11, stone);

    // Carve vertical grooves.
    for (int y = 3; y < 14; ++y) {
        if ((y % 3) == 0) {
            setPx(s, 7, y, mul(stone, 0.82f));
            setPx(s, 8, y, mul(stone, 0.92f));
        }
    }

    // Cap and base rings.
    rect(s, 5, 2, 6, 1, light);
    rect(s, 5, 13, 6, 1, mul(stone, 0.92f));

    // Subtle animated sparkle so pillars don't look perfectly static.
    if (frame % 2 == 1) {
        setPx(s, 6, 4, add(s.at(6, 4), 22, 22, 24));
        setPx(s, 9, 7, add(s.at(9, 7), 14, 14, 16));
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateBoulderTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    // Boulder is a transparent overlay layered on top of the themed floor.
    RNG rng(hash32(seed ^ 0xB00B135u));

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    Color stone = { 118, 122, 130, 255 };
    stone = add(stone, rng.range(-14, 14), rng.range(-14, 14), rng.range(-14, 14));
    Color dark = mul(stone, 0.58f);
    Color light = add(mul(stone, 1.12f), 14, 14, 16);

    auto rand01 = [&](uint32_t v) -> float {
        return static_cast<float>(v) / 4294967295.0f;
    };

    // Soft shadow under the boulder.
    for (int y = 9; y < 15; ++y) {
        for (int x = 2; x < 14; ++x) {
            float cx = (x - 7.5f) / 6.0f;
            float cy = (y - 12.5f) / 3.2f;
            float d2 = cx*cx + cy*cy;
            if (d2 > 1.0f) continue;
            int a = static_cast<int>(std::lround((1.0f - d2) * 120.0f));
            a = std::clamp(a, 0, 120);
            setPx(s, x, y, Color{0, 0, 0, static_cast<uint8_t>(a)});
        }
    }

    // Boulder body: slightly irregular ellipse with top-left lighting.
    const float cx = 7.5f;
    const float cy = 7.0f;
    const float rx = 6.2f;
    const float ry = 5.2f;

    for (int y = 1; y < 15; ++y) {
        for (int x = 1; x < 15; ++x) {
            float nx = (static_cast<float>(x) - cx) / rx;
            float ny = (static_cast<float>(y) - cy) / ry;
            float d2 = nx*nx + ny*ny;

            // Small shape jitter via hash-based noise.
            uint32_t hv = hash32(seed ^ static_cast<uint32_t>(x * 73856093) ^ static_cast<uint32_t>(y * 19349663) ^ static_cast<uint32_t>(frame * 83492791));
            float n = rand01(hv) - 0.5f; // [-0.5, +0.5]
            float edge = 1.0f + n * 0.08f;

            if (d2 > edge) continue;

            // Lighting: highlight toward (-1,-1) direction.
            float shade = 0.80f;
            shade += (-nx * 0.10f) + (-ny * 0.14f);
            shade = std::clamp(shade, 0.52f, 1.18f);

            Color c = rampShadeTile(stone, shade, x, y);

            // Darker rim for definition.
            if (d2 > edge * 0.88f) c = mul(c, 0.78f);

            setPx(s, x, y, c);
        }
    }

    // A couple of cracks / speckles.
    for (int i = 0; i < 8; ++i) {
        int x = rng.range(3, 12);
        int y = rng.range(3, 11);
        if (s.at(x, y).a == 0) continue;
        if (rng.chance(0.55f)) setPx(s, x, y, mul(s.at(x, y), 0.72f));
        if (rng.chance(0.35f) && x + 1 < 16 && s.at(x + 1, y).a != 0) setPx(s, x + 1, y, mul(s.at(x + 1, y), 0.80f));
        if (rng.chance(0.30f) && y + 1 < 16 && s.at(x, y + 1).a != 0) setPx(s, x, y + 1, mul(s.at(x, y + 1), 0.86f));
    }

    // Subtle animated glint so boulders don't read as a flat blob.
    if (frame % 2 == 1) {
        if (s.at(5, 4).a != 0) setPx(s, 5, 4, add(s.at(5, 4), 18, 18, 20));
        if (s.at(6, 3).a != 0) setPx(s, 6, 3, add(s.at(6, 3), 10, 10, 12));
    }

    // Outline pass for crispness.
    for (int y = 1; y < 15; ++y) {
        for (int x = 1; x < 15; ++x) {
            if (s.at(x, y).a == 0) continue;
            bool edgePx = false;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    int nx = x + ox;
                    int ny = y + oy;
                    if (nx < 0 || nx >= 16 || ny < 0 || ny >= 16) { edgePx = true; continue; }
                    if (s.at(nx, ny).a == 0) edgePx = true;
                }
            }
            if (edgePx) setPx(s, x, y, mul(s.at(x, y), 0.88f));
        }
    }

    // Add a small highlight stroke.
    line(s, 4, 6, 7, 4, light);
    line(s, 5, 7, 8, 5, add(light, -10, -10, -10));

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateStairsTile(uint32_t seed, bool up, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    RNG rng(hash32(seed));
    // Stairs are rendered as a transparent overlay layered on top of the
    // underlying themed floor (handled by the renderer).
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    Color stair = { 185, 175, 155, 255 };
    stair = add(stair, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));

    // Soft base shadow so the stair shape reads against noisy floors.
    for (int y = 5; y < 14; ++y) {
        for (int x = 3; x < 14; ++x) {
            // Slight diagonal falloff.
            float d = (static_cast<float>(x) + static_cast<float>(y) * 0.9f) / 28.0f;
            d = std::clamp(d, 0.0f, 1.0f);
            uint8_t a = static_cast<uint8_t>(std::lround(55.0f + 45.0f * d));
            setPx(s, x, y, Color{0, 0, 0, a});
        }
    }

    // Simple diagonal steps (opaque strokes, with a darker underside line).
    for (int i = 0; i < 6; ++i) {
        int x0 = 4 + i;
        int y0 = 11 - i;
        line(s, x0, y0, x0 + 7, y0, mul(stair, 0.95f));
        // Underside (draw slightly translucent so it blends).
        Color under = mul(stair, 0.72f);
        under.a = 210;
        line(s, x0, y0 + 1, x0 + 6, y0 + 1, under);
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

    // Tiny rim highlight to separate stairs from the floor near the top.
    if (frame % 2 == 1) {
        setPx(s, 4, 5, Color{240, 235, 225, 180});
        setPx(s, 5, 5, Color{240, 235, 225, 160});
        setPx(s, 6, 4, Color{255, 255, 255, 120});
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateDoorTile(uint32_t seed, bool open, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    RNG rng(hash32(seed));
    // Doors are rendered as transparent overlays layered on top of the
    // underlying themed floor (handled by the renderer).
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    Color wood = add({140, 95, 55, 255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
    Color dark = mul(wood, 0.68f);

    // A subtle threshold shadow so the doorway reads against busy floors.
    for (int y = 12; y < 15; ++y) {
        for (int x = 4; x < 12; ++x) {
            uint8_t a = static_cast<uint8_t>(60 + (y - 12) * 22);
            setPx(s, x, y, Color{0,0,0,a});
        }
    }

    if (open) {
        // Dark gap (semi-transparent so floor shows through).
        for (int y = 3; y < 14; ++y) {
            for (int x = 5; x < 11; ++x) {
                uint8_t a = static_cast<uint8_t>(150 + (y - 3) * 4);
                setPx(s, x, y, Color{10, 10, 14, a});
            }
        }

        // Frame
        outlineRect(s, 4, 2, 8, 13, dark);
        // Inner highlight
        Color hi = add(mul(wood, 1.05f), 10, 10, 12);
        hi.a = 220;
        line(s, 5, 3, 10, 3, hi);

        // Hinges highlight
        if (frame % 2 == 1) {
            setPx(s, 4, 6, {255,255,255,70});
            setPx(s, 11, 8, {255,255,255,55});
        }
    } else {
        // Solid door
        outlineRect(s, 4, 2, 8, 13, dark);
        rect(s, 5, 3, 6, 11, wood);

        // Planks
        for (int y = 4; y <= 12; y += 3) {
            Color plank = mul(wood, 0.82f);
            plank.a = 245;
            line(s, 5, y, 10, y, plank);
        }

        // Knob + tiny specular highlight
        circle(s, 10, 8, 1, {200, 190, 80, 255});
        if (frame % 2 == 1) setPx(s, 11, 7, {255,255,255,110});
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateLockedDoorTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    // Base: closed door sprite, with a small lock overlay for readability.
    SpritePixels s = generateDoorTile(seed, /*open=*/false, frame, /*pxSize=*/16);

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

    // Tiny shimmer highlight (alternating frame).
    if (frame % 2 == 1) {
        setPx(s, x0 + 2, y0 + 4, Color{ 245, 235, 130, 255 });
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateUIPanelTile(UITheme theme, uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
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

            s.at(x, y) = rampShadeTile(base, f * 0.90f, x, y);
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

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateUIOrnamentTile(UITheme theme, uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
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

    return resampleSpriteToSize(s, pxSize);
}



// -----------------------------------------------------------------------------
// Tile overlay decals (transparent 16x16 sprites)
// style mapping (kept in renderer):
//  0 = Generic, 1 = Treasure, 2 = Lair, 3 = Shrine, 4 = Secret, 5 = Vault, 6 = Shop
// -----------------------------------------------------------------------------

SpritePixels generateFloorDecalTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed ^ (static_cast<uint32_t>(style) * 0x9E3779B9u)));

    auto sparkle = [&](int cx, int cy, Color c) {
        setPx(s, cx, cy, c);
        setPx(s, cx-1, cy, mul(c, 0.75f));
        setPx(s, cx+1, cy, mul(c, 0.75f));
        setPx(s, cx, cy-1, mul(c, 0.75f));
        setPx(s, cx, cy+1, mul(c, 0.75f));
    };

    switch (style) {
        default:
        case 0: { // Generic: cracks + pebbles
            Color crack{ 10, 10, 14, 0 };
            crack.a = static_cast<uint8_t>(120 + rng.range(0, 60));
            for (int i = 0; i < 2; ++i) {
                int x0 = rng.range(0, 15);
                int y0 = rng.range(0, 15);
                int x1 = std::clamp(x0 + rng.range(-9, 9), 0, 15);
                int y1 = std::clamp(y0 + rng.range(-9, 9), 0, 15);
                line(s, x0, y0, x1, y1, crack);
                // small offshoot
                if (rng.chance(0.50f)) {
                    int x2 = std::clamp(x0 + rng.range(-4, 4), 0, 15);
                    int y2 = std::clamp(y0 + rng.range(-4, 4), 0, 15);
                    Color c2 = crack; c2.a = static_cast<uint8_t>(crack.a * 0.75f);
                    line(s, x0, y0, x2, y2, c2);
                }
            }

            // pebble specks
            for (int i = 0; i < 10; ++i) {
                int x = rng.range(1, 14);
                int y = rng.range(1, 14);
                Color p{ static_cast<uint8_t>(110 + rng.range(-10, 10)),
                         static_cast<uint8_t>(105 + rng.range(-10, 10)),
                         static_cast<uint8_t>(95 + rng.range(-10, 10)),
                         static_cast<uint8_t>(60 + rng.range(0, 80)) };
                setPx(s, x, y, p);
            }

            // occasional wet spot shimmer
            if (frame % 2 == 1 && rng.chance(0.35f)) {
                int cx = rng.range(3, 12);
                int cy = rng.range(3, 12);
                Color w{ 90, 140, 190, 70 };
                setPx(s, cx, cy, w);
                setPx(s, cx+1, cy, mul(w, 0.80f));
                setPx(s, cx, cy+1, mul(w, 0.80f));
                setPx(s, cx-1, cy, mul(w, 0.70f));
            }
            break;
        }

        case 1: { // Treasure: gold inlay + sparkles
            Color gold{ 235, 200, 120, 160 };
            Color gold2 = mul(gold, 0.70f); gold2.a = 140;

            // thin filigree lines
            int y = rng.range(3, 12);
            for (int x = 2; x < 14; ++x) {
                if ((x + y) % 3 == 0) setPx(s, x, y, gold);
                if ((x + y) % 5 == 0) setPx(s, x, y+1, gold2);
            }

            // coin-ish dot
            int cx = rng.range(4, 11);
            int cy = rng.range(4, 11);
            circle(s, cx, cy, 2, gold2);
            circle(s, cx, cy, 1, gold);

            // sparkle pulse
            if (frame % 2 == 1) {
                sparkle(rng.range(3, 12), rng.range(3, 12), Color{255, 245, 200, 180});
            }
            break;
        }

        case 2: { // Lair: grime + claw marks
            Color moss{ 70, 140, 70, 120 };
            Color grime{ 30, 35, 28, 120 };

            // moss clumps around edges
            for (int i = 0; i < 22; ++i) {
                int x = (rng.chance(0.5f) ? rng.range(0, 5) : rng.range(10, 15));
                int y = rng.range(0, 15);
                if (rng.chance(0.5f)) std::swap(x, y);
                setPx(s, x, y, rng.chance(0.6f) ? moss : grime);
            }

            // claw marks
            Color claw{ 20, 15, 15, 150 };
            int x0 = rng.range(2, 6);
            int y0 = rng.range(9, 13);
            for (int i = 0; i < 3; ++i) {
                int dx = 4 + i;
                line(s, x0 + dx, y0 - i, x0 + dx + 4, y0 - i - 5, claw);
            }

            // faint slime shimmer
            if (frame % 2 == 1 && rng.chance(0.55f)) {
                int cx = rng.range(2, 13);
                int cy = rng.range(2, 13);
                setPx(s, cx, cy, Color{120, 220, 160, 70});
            }
            break;
        }

        case 3: { // Shrine: runes (cool glow)
            Color rune{ 160, 210, 255, 150 };
            Color rune2{ 120, 170, 255, 120 };
            if (frame % 2 == 1) {
                rune = add(rune, 25, 25, 25);
                rune.a = 180;
            }

            // central sigil
            int cx = 8 + rng.range(-1, 1);
            int cy = 8 + rng.range(-1, 1);
            circle(s, cx, cy, 3, rune2);
            circle(s, cx, cy, 2, rune);

            // spokes
            line(s, cx, cy - 4, cx, cy + 4, rune);
            line(s, cx - 4, cy, cx + 4, cy, rune);

            // dots
            for (int i = 0; i < 4; ++i) {
                int x = cx + (i < 2 ? -5 : 5);
                int y = cy + (i % 2 ? -5 : 5);
                setPx(s, std::clamp(x, 0, 15), std::clamp(y, 0, 15), rune2);
            }
            break;
        }

        case 4: { // Secret: dust + cobwebs (subtle)
            Color dust{ 220, 210, 200, 60 };
            Color dust2{ 200, 190, 175, 55 };

            // corner webs
            line(s, 0, 0, 6, 6, dust);
            line(s, 15, 0, 9, 6, dust);
            line(s, 0, 15, 6, 9, dust);
            line(s, 15, 15, 9, 9, dust);

            // drifting dust mote
            if (frame % 2 == 1) {
                int x = rng.range(3, 12);
                int y = rng.range(3, 12);
                setPx(s, x, y, dust2);
                setPx(s, x+1, y, Color{255,255,255,35});
            }
            break;
        }

        case 5: { // Vault: steel plate seams + rivets
            Color steel{ 200, 220, 255, 110 };
            Color rivet{ 235, 245, 255, 150 };
            Color scratch{ 40, 50, 65, 120 };

            // seam rectangle
            int x0 = rng.range(2, 5);
            int y0 = rng.range(2, 5);
            int w = rng.range(7, 11);
            int h = rng.range(6, 9);
            outlineRect(s, x0, y0, w, h, steel);

            // rivets
            setPx(s, x0, y0, rivet);
            setPx(s, x0 + w - 1, y0, rivet);
            setPx(s, x0, y0 + h - 1, rivet);
            setPx(s, x0 + w - 1, y0 + h - 1, rivet);

            // scratches
            int sx0 = rng.range(2, 13);
            int sy0 = rng.range(2, 13);
            line(s, sx0, sy0, std::clamp(sx0 + rng.range(-6, 6), 0, 15), std::clamp(sy0 + rng.range(-6, 6), 0, 15), scratch);

            if (frame % 2 == 1 && rng.chance(0.45f)) {
                // tiny glint
                sparkle(x0 + w/2, y0 + 1, Color{255,255,255,120});
            }
            break;
        }

        case 6: { // Shop: rug / plank hint
            Color rug{ 170, 80, 70, 120 };
            Color border{ 235, 210, 150, 130 };

            // small rug patch
            int x0 = rng.range(3, 6);
            int y0 = rng.range(5, 8);
            rect(s, x0, y0, 10 - x0, 7, rug);
            outlineRect(s, x0, y0, 10 - x0, 7, border);

            // weave pattern
            for (int y = y0 + 1; y < y0 + 6; ++y) {
                for (int x = x0 + 1; x < x0 + (10 - x0) - 1; ++x) {
                    if (((x + y + frame) % 3) == 0) setPx(s, x, y, mul(rug, 0.85f));
                }
            }

            if (frame % 2 == 1) {
                setPx(s, x0 + 2, y0 + 2, Color{255, 240, 220, 70});
            }
            break;
        }
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateWallDecalTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed ^ (static_cast<uint32_t>(style) * 0xA341316Cu)));

    Color stain{ 0, 0, 0, 120 };
    switch (style) {
        default:
        case 0: stain = { 0, 0, 0, 110 }; break;
        case 1: stain = { 240, 200, 120, 110 }; break; // treasure glint
        case 2: stain = { 70, 140, 70, 120 }; break;   // moss
        case 3: stain = { 150, 200, 255, 120 }; break; // rune glow
        case 4: stain = { 220, 210, 200, 70 }; break;  // dust
        case 5: stain = { 200, 220, 255, 110 }; break; // steel
        case 6: stain = { 200, 150, 100, 110 }; break; // wood-ish
    }

    // Drips / streaks
    const int drips = 2 + rng.range(0, 2);
    for (int i = 0; i < drips; ++i) {
        int x = rng.range(2, 13);
        int y0 = rng.range(1, 8);
        int len = rng.range(3, 8);
        for (int j = 0; j < len; ++j) {
            int y = std::clamp(y0 + j, 0, 15);
            Color c = stain;
            c.a = static_cast<uint8_t>(std::max<int>(20, stain.a - j * 10));
            setPx(s, x, y, c);
            if (rng.chance(0.25f)) setPx(s, x+1, y, mul(c, 0.70f));
        }
    }

    // One crack
    Color crack = stain;
    crack.r = static_cast<uint8_t>(std::min<int>(crack.r, 40));
    crack.g = static_cast<uint8_t>(std::min<int>(crack.g, 40));
    crack.b = static_cast<uint8_t>(std::min<int>(crack.b, 55));
    crack.a = static_cast<uint8_t>(100 + rng.range(0, 70));

    int x0 = rng.range(1, 14);
    int y0 = rng.range(1, 14);
    int x1 = std::clamp(x0 + rng.range(-8, 8), 0, 15);
    int y1 = std::clamp(y0 + rng.range(-8, 8), 0, 15);
    line(s, x0, y0, x1, y1, crack);

    // Gentle pulse on rune/treasure styles
    if (frame % 2 == 1 && (style == 1 || style == 3)) {
        int cx = rng.range(3, 12);
        int cy = rng.range(3, 12);
        setPx(s, cx, cy, Color{255,255,255,70});
    }

    return resampleSpriteToSize(s, pxSize);
}


// -----------------------------------------------------------------------------
// Autotile overlays (transparent 16x16 sprites)
//
// These are layered on top of the base wall/chasm tiles in the renderer to create
// crisp edges, corners, and a stronger sense of depth without requiring a full
// 47-tile tileset.
// -----------------------------------------------------------------------------

static inline void setPxAlpha(SpritePixels& s, int x, int y, Color c, uint8_t a) {
    c.a = a;
    setPx(s, x, y, c);
}

SpritePixels generateWallEdgeOverlay(uint32_t seed, uint8_t openMask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    (void)frame;

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    if (openMask == 0u) return resampleSpriteToSize(s, pxSize);

    RNG rng(hash32(seed ^ (static_cast<uint32_t>(openMask) * 0x9E3779B9u) ^ (static_cast<uint32_t>(variant) * 0x85EBCA6Bu)));

    // Grayscale pixels; the renderer applies lighting/tint via texture color modulation.
    const Color outline = { 10, 10, 12, 255 };
    const Color shadow  = { 0, 0, 0, 255 };
    const Color hilite  = { 255, 255, 255, 255 };
    const Color hilite2 = { 215, 220, 230, 255 };

    const auto chip = [&](int x, int y) -> bool {
        // Tiny deterministic wear so the outline doesn't look perfectly computer-drawn.
        const uint32_t h = hash32(seed ^ 0xC0FFEEu ^ static_cast<uint32_t>(x + y * 17) ^ static_cast<uint32_t>(variant * 131));
        const uint8_t r = static_cast<uint8_t>(h & 0xFFu);
        return r < 18u;
    };

    auto drawHLine = [&](int y, bool top, uint8_t a0, uint8_t a1) {
        for (int x = 0; x < 16; ++x) {
            if (chip(x, y)) continue;
            setPxAlpha(s, x, y, outline, a0);
            // bevel highlight/shadow just inside
            if (top) {
                if (y + 1 < 16) setPxAlpha(s, x, y + 1, (x < 7 ? hilite : hilite2), a1);
            } else {
                if (y - 1 >= 0) setPxAlpha(s, x, y - 1, shadow, a1);
            }
        }
    };

    auto drawVLine = [&](int x, bool left, uint8_t a0, uint8_t a1) {
        for (int y = 0; y < 16; ++y) {
            if (chip(x, y)) continue;
            setPxAlpha(s, x, y, outline, a0);
            if (left) {
                if (x + 1 < 16) setPxAlpha(s, x + 1, y, (y < 7 ? hilite : hilite2), a1);
            } else {
                if (x - 1 >= 0) setPxAlpha(s, x - 1, y, shadow, a1);
            }
        }
    };

    // Exposed edges: 1=N, 2=E, 4=S, 8=W
    if (openMask & 0x01u) drawHLine(0, /*top=*/true, 170, 90);
    if (openMask & 0x04u) drawHLine(15, /*top=*/false, 190, 100);
    if (openMask & 0x08u) drawVLine(0, /*left=*/true, 170, 90);
    if (openMask & 0x02u) drawVLine(15, /*left=*/false, 190, 100);

    // Corner emphasis (helps walls read as blocks).
    auto corner = [&](int x, int y, bool bright) {
        const uint8_t a = bright ? 210 : 170;
        setPxAlpha(s, x, y, bright ? hilite : outline, a);
        setPxAlpha(s, x + (x == 0 ? 1 : -1), y, hilite2, 80);
        setPxAlpha(s, x, y + (y == 0 ? 1 : -1), hilite2, 80);
    };

    if ((openMask & 0x01u) && (openMask & 0x08u)) corner(0, 0, true);
    if ((openMask & 0x01u) && (openMask & 0x02u)) corner(15, 0, false);
    if ((openMask & 0x04u) && (openMask & 0x08u)) corner(0, 15, false);
    if ((openMask & 0x04u) && (openMask & 0x02u)) corner(15, 15, false);

    // A couple of tiny pits/chips near exposed edges (adds variety without noise).
    for (int i = 0; i < 4; ++i) {
        int x = rng.range(1, 14);
        int y = rng.range(1, 14);
        // bias toward edges for readability
        if (rng.chance(0.7f)) {
            if (openMask & 0x01u) y = rng.range(1, 3);
            if (openMask & 0x04u) y = rng.range(12, 14);
            if (openMask & 0x08u) x = rng.range(1, 3);
            if (openMask & 0x02u) x = rng.range(12, 14);
        }
        setPxAlpha(s, x, y, shadow, 110);
        if (rng.chance(0.45f)) setPxAlpha(s, x + 1, y, shadow, 70);
        if (rng.chance(0.45f)) setPxAlpha(s, x, y + 1, shadow, 70);
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateChasmRimOverlay(uint32_t seed, uint8_t openMask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    if (openMask == 0u) return resampleSpriteToSize(s, pxSize);

    RNG rng(hash32(seed ^ 0xA11CEu ^ (static_cast<uint32_t>(openMask) * 131u) ^ (static_cast<uint32_t>(variant) * 977u)));

    // Slightly cool grayscale; renderer tint + lighting will do most of the work.
    const Color lipHi = { 255, 255, 255, 255 };
    const Color lipMd = { 210, 220, 240, 255 };
    const Color lipSh = { 0, 0, 0, 255 };

    auto rimH = [&](int y0, bool top) {
        for (int x = 0; x < 16; ++x) {
            const uint32_t h = hash32(seed ^ static_cast<uint32_t>(x + y0 * 31) ^ static_cast<uint32_t>(variant * 17));
            const uint8_t r = static_cast<uint8_t>(h & 0xFFu);
            const bool breakPix = (r < 10u); // tiny gaps
            if (breakPix) continue;

            if (top) {
                setPxAlpha(s, x, y0, lipHi, 200);
                setPxAlpha(s, x, y0 + 1, lipMd, 150);
                setPxAlpha(s, x, y0 + 2, lipSh, 90);
            } else {
                setPxAlpha(s, x, y0, lipMd, 160);
                setPxAlpha(s, x, y0 - 1, lipSh, 120);
            }
        }
    };

    auto rimV = [&](int x0, bool left) {
        for (int y = 0; y < 16; ++y) {
            const uint32_t h = hash32(seed ^ static_cast<uint32_t>(x0 + y * 29) ^ static_cast<uint32_t>(variant * 13));
            const uint8_t r = static_cast<uint8_t>(h & 0xFFu);
            const bool breakPix = (r < 10u);
            if (breakPix) continue;

            if (left) {
                setPxAlpha(s, x0, y, lipHi, 200);
                setPxAlpha(s, x0 + 1, y, lipMd, 150);
                setPxAlpha(s, x0 + 2, y, lipSh, 90);
            } else {
                setPxAlpha(s, x0, y, lipMd, 160);
                setPxAlpha(s, x0 - 1, y, lipSh, 120);
            }
        }
    };

    if (openMask & 0x01u) rimH(0, true);
    if (openMask & 0x04u) rimH(15, false);
    if (openMask & 0x08u) rimV(0, true);
    if (openMask & 0x02u) rimV(15, false);

    // A few shimmering rim pixels on the animated frame.
    if (frame % 2 == 1) {
        for (int i = 0; i < 5; ++i) {
            int x = rng.range(0, 15);
            int y = rng.range(0, 15);
            // bias toward rim
            if (rng.chance(0.7f)) {
                if (openMask & 0x01u) y = 0;
                if (openMask & 0x04u) y = 15;
                if (openMask & 0x08u) x = 0;
                if (openMask & 0x02u) x = 15;
            }
            setPxAlpha(s, x, y, lipHi, 160);
        }
    }

    return resampleSpriteToSize(s, pxSize);
}

// Procedural confusion-gas tile: grayscale translucent cloud.
// Color/tint is applied in the renderer (so lighting affects it naturally).
SpritePixels generateConfusionGasTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed ^ 0xC0FF1151u));

    struct Blob {
        float cx, cy;
        float r;
        float w;
        float vx, vy;
    };

    const int nBlobs = 3 + (rng.range(0, 100) < 55 ? 1 : 0);
    std::vector<Blob> blobs;
    blobs.reserve(static_cast<size_t>(nBlobs));

    const float t = static_cast<float>(frame) * 0.9f;

    for (int i = 0; i < nBlobs; ++i) {
        Blob b;
        b.cx = static_cast<float>(rng.range(0, 15));
        b.cy = static_cast<float>(rng.range(0, 15));
        b.r  = static_cast<float>(rng.range(4, 8));
        b.w  = 0.9f + (rng.range(0, 100) / 100.0f) * 0.6f;
        b.vx = (rng.range(-10, 10) / 10.0f) * 0.6f;
        b.vy = (rng.range(-10, 10) / 10.0f) * 0.6f;
        // Slight bob per frame for motion.
        b.cx += b.vx * t;
        b.cy += b.vy * t;
        blobs.push_back(b);
    }

    // Carve-out holes for a swirly feel.
    const int nHoles = 1 + (rng.range(0, 100) < 50 ? 1 : 0);
    std::vector<Blob> holes;
    holes.reserve(static_cast<size_t>(nHoles));
    for (int i = 0; i < nHoles; ++i) {
        Blob b;
        b.cx = static_cast<float>(rng.range(0, 15));
        b.cy = static_cast<float>(rng.range(0, 15));
        b.r  = static_cast<float>(rng.range(3, 6));
        b.w  = 0.8f + (rng.range(0, 100) / 100.0f) * 0.5f;
        b.vx = (rng.range(-10, 10) / 10.0f) * 0.4f;
        b.vy = (rng.range(-10, 10) / 10.0f) * 0.4f;
        b.cx += b.vx * t;
        b.cy += b.vy * t;
        holes.push_back(b);
    }

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            float v = 0.0f;

            for (const Blob& b : blobs) {
                const float dx = static_cast<float>(x) - b.cx;
                const float dy = static_cast<float>(y) - b.cy;
                const float d  = std::sqrt(dx*dx + dy*dy);
                const float k  = std::clamp(1.0f - d / b.r, 0.0f, 1.0f);
                v += (k * k) * b.w;
            }

            for (const Blob& b : holes) {
                const float dx = static_cast<float>(x) - b.cx;
                const float dy = static_cast<float>(y) - b.cy;
                const float d  = std::sqrt(dx*dx + dy*dy);
                const float k  = std::clamp(1.0f - d / b.r, 0.0f, 1.0f);
                v -= (k * k) * b.w * 0.8f;
            }

            // Add a little fine noise so it reads as gas, not a soft blob.
            const uint32_t hn = hash32(seed ^ static_cast<uint32_t>(x + y * 17) ^ static_cast<uint32_t>(frame * 131));
            const float n = static_cast<float>(hn & 0xFFu) / 255.0f; // 0..1
            v += (n - 0.5f) * 0.12f;

            // Normalize-ish
            v = std::clamp(v * 0.55f, 0.0f, 1.0f);

            // Sharper edge with dithering.
            const float edge = std::clamp((v - 0.18f) / 0.82f, 0.0f, 1.0f);
            const float thr = bayer4Threshold(x + frame * 2, y + frame * 3);
            if (edge < thr * 0.65f) continue;

            const int a = static_cast<int>(std::round(edge * 190.0f));
            const uint8_t aa = static_cast<uint8_t>(std::clamp(a, 0, 190));

            // Slight center brightening.
            const float vx = (x - 7.5f) / 7.5f;
            const float vy = (y - 7.5f) / 7.5f;
            const float center = 1.0f - 0.25f * (vx*vx + vy*vy);

            const uint8_t g = clamp8(static_cast<int>(std::round(220.0f * center)));
            setPx(s, x, y, Color{ g, g, g, aa });
        }
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateFireTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    // A small, animated flame overlay. The renderer tints this sprite, so we keep
    // it mostly grayscale with alpha.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const uint32_t base = hash32(seed ^ 0xF17ECAFEu);

    auto rand01 = [&](uint32_t v) -> float {
        return static_cast<float>(v & 0xFFFFu) / 65535.0f;
    };

    // Three flame "tongues" with slightly different phases.
    const float cx1 = 4.0f  + rand01(hash32(base ^ 0xA1u)) * 8.0f;
    const float cx2 = 4.0f  + rand01(hash32(base ^ 0xB2u)) * 8.0f;
    const float cx3 = 4.0f  + rand01(hash32(base ^ 0xC3u)) * 8.0f;

    const float w1  = 1.6f + rand01(hash32(base ^ 0x11u)) * 2.2f;
    const float w2  = 1.4f + rand01(hash32(base ^ 0x22u)) * 2.4f;
    const float w3  = 1.8f + rand01(hash32(base ^ 0x33u)) * 2.0f;

    const float ph1 = rand01(hash32(base ^ 0x91u)) * 6.2831853f;
    const float ph2 = rand01(hash32(base ^ 0x92u)) * 6.2831853f;
    const float ph3 = rand01(hash32(base ^ 0x93u)) * 6.2831853f;

    for (int y = 0; y < 16; ++y) {
        // y=0 top, y=15 bottom
        const float yy = static_cast<float>(y) / 15.0f;

        // Flames are stronger toward the bottom, but still flicker above.
        float baseV = std::pow(std::max(yy, 0.02f), 0.38f);
        baseV *= (0.45f + 0.55f * yy);

        for (int x = 0; x < 16; ++x) {
            const float xx = static_cast<float>(x);

            auto tongue = [&](float cx, float w, float ph) -> float {
                // More lateral wobble near the top.
                const float wobAmp = (1.0f - yy) * 1.6f;
                const float wob = std::sin(ph + yy * 3.4f + static_cast<float>(frame) * 0.7f) * wobAmp;
                const float c = cx + wob;

                // Wider at the bottom.
                const float ww = w * (0.55f + 0.95f * yy);
                const float dx = (xx - c) / std::max(0.35f, ww);
                const float g = std::exp(-dx * dx * 2.3f);
                return g;
            };

            float v = 0.0f;
            v = std::max(v, tongue(cx1, w1, ph1));
            v = std::max(v, tongue(cx2, w2, ph2));
            v = std::max(v, tongue(cx3, w3, ph3));

            // Vertical envelope.
            v *= baseV;

            // Noisy flicker.
            const uint32_t h = hash32(base ^ static_cast<uint32_t>(x * 374761393) ^ static_cast<uint32_t>(y * 668265263) ^ static_cast<uint32_t>(frame * 1447));
            const float n = (static_cast<float>(h & 0xFFu) / 255.0f - 0.5f) * 0.35f;
            v += n;

            v = std::clamp(v, 0.0f, 1.0f);

            // Ordered dithering keeps the overlay from looking like a solid blob.
            const float thr = bayer4Threshold(x + frame * 2, y + frame * 3);
            if (v < thr * 0.95f) continue;

            if (v < 0.10f) continue;

            const float t = (v - 0.10f) / 0.90f;
            const int a = static_cast<int>(90 + t * 165);
            const int g = static_cast<int>(180 + t * 75);
            setPx(s, x, y, Color{ clamp8(g), clamp8(g), clamp8(g), clamp8(a) });
        }
    }

    // A little dark outline helps flames read in bright rooms.
    finalizeSprite(s, hash32(base ^ 0xF17Eu), frame, /*outlineAlpha=*/90, /*shadowAlpha=*/0);
    return resampleSpriteToSize(s, pxSize);
}


// -----------------------------------------------------------------------------
// HUD/status icons (transparent 16x16 sprites)
// -----------------------------------------------------------------------------

SpritePixels generateEffectIcon(EffectKind kind, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    auto pulse = [&](Color c, int addv) {
        if (frame % 2 == 1) return add(c, addv, addv, addv);
        return c;
    };

    switch (kind) {
        case EffectKind::Poison: {
            Color g = pulse(Color{90, 235, 110, 255}, 15);
            Color dk{20, 35, 20, 255};

            // Droplet
            circle(s, 8, 6, 3, mul(g, 0.85f));
            circle(s, 8, 7, 3, g);
            line(s, 8, 9, 8, 12, g);
            setPx(s, 7, 11, mul(g, 0.80f));
            setPx(s, 9, 11, mul(g, 0.80f));

            // Tiny skull eyes
            setPx(s, 7, 7, dk);
            setPx(s, 9, 7, dk);
            break;
        }
        case EffectKind::Regen: {
            Color c = pulse(Color{120, 255, 140, 255}, 10);
            Color c2 = mul(c, 0.70f);

            // Plus
            rect(s, 7, 4, 2, 8, c);
            rect(s, 4, 7, 8, 2, c);

            // heartbeat tick
            line(s, 3, 12, 6, 12, c2);
            line(s, 6, 12, 7, 10, c2);
            line(s, 7, 10, 8, 13, c2);
            line(s, 8, 13, 10, 11, c2);
            line(s, 10, 11, 13, 11, c2);
            break;
        }
        case EffectKind::Shield: {
            Color c = pulse(Color{210, 220, 235, 255}, 8);
            Color c2 = mul(c, 0.75f);

            // Shield silhouette
            rect(s, 5, 3, 6, 8, c2);
            rect(s, 6, 2, 4, 10, c);
            line(s, 6, 12, 8, 14, c2);
            line(s, 8, 14, 10, 12, c2);

            // Shine
            if (frame % 2 == 1) {
                line(s, 7, 4, 7, 10, Color{255,255,255,140});
            }
            break;
        }
        case EffectKind::Haste: {
            Color c = pulse(Color{255, 225, 120, 255}, 12);
            Color c2 = mul(c, 0.70f);

            // Lightning bolt
            line(s, 9, 2, 6, 8, c);
            line(s, 6, 8, 10, 8, c);
            line(s, 10, 8, 7, 14, c);
            // motion ticks
            line(s, 3, 5, 5, 5, c2);
            line(s, 2, 8, 5, 8, c2);
            break;
        }
        case EffectKind::Vision: {
            Color c = pulse(Color{140, 220, 255, 255}, 8);
            Color c2 = mul(c, 0.70f);

            // Eye outline
            line(s, 3, 8, 6, 5, c2);
            line(s, 6, 5, 10, 5, c2);
            line(s, 10, 5, 13, 8, c2);
            line(s, 13, 8, 10, 11, c2);
            line(s, 10, 11, 6, 11, c2);
            line(s, 6, 11, 3, 8, c2);

            circle(s, 8, 8, 2, c);
            setPx(s, 8, 8, Color{20, 30, 40, 255});
            break;
        }
        case EffectKind::Invis: {
            Color c{190, 160, 255, static_cast<uint8_t>((frame % 2 == 1) ? 170 : 210) };
            Color c2 = mul(c, 0.75f);

            // Faint ghost-ish silhouette
            circle(s, 6, 7, 2, c2);
            circle(s, 10, 7, 2, c2);
            rect(s, 5, 8, 6, 5, c);
            // cutout holes
            setPx(s, 7, 9, Color{0,0,0,0});
            setPx(s, 9, 9, Color{0,0,0,0});
            break;
        }
        case EffectKind::Web: {
            Color c = pulse(Color{230, 230, 240, 255}, 6);
            Color c2 = mul(c, 0.65f);

            // Web spokes
            line(s, 8, 2, 8, 14, c2);
            line(s, 2, 8, 14, 8, c2);
            line(s, 3, 3, 13, 13, c2);
            line(s, 13, 3, 3, 13, c2);

            // Rings
            circle(s, 8, 8, 5, c);
            circle(s, 8, 8, 3, c);
            break;
        }
        case EffectKind::Confusion: {
            Color c = pulse(Color{255, 140, 255, 255}, 10);
            Color c2 = mul(c, 0.70f);

            // Spiral-ish squiggle
            line(s, 4, 8, 12, 4, c2);
            line(s, 12, 4, 10, 10, c2);
            line(s, 10, 10, 6, 12, c2);
            line(s, 6, 12, 8, 6, c2);
            setPx(s, 8, 6, c);
            if (frame % 2 == 1) {
                setPx(s, 5, 6, Color{255,255,255,100});
                setPx(s, 11, 11, Color{255,255,255,80});
            }
            break;
        }
        case EffectKind::Burn: {
            Color hot  = pulse(Color{255, 170, 90, 255}, 12);
            Color core = pulse(Color{255, 235, 160, 255}, 8);
            Color dk{70, 25, 10, 255};

            // Flame base
            circle(s, 8, 11, 3, mul(hot, 0.90f));
            circle(s, 8, 10, 2, hot);

            // Rising tongue
            line(s, 8, 4, 8, 10, hot);
            circle(s, 8, 6, 2, mul(core, 0.95f));
            setPx(s, 8, 5, core);
            setPx(s, 7, 6, core);
            setPx(s, 9, 6, core);

            // Ember/spark
            if (frame % 2 == 1) {
                setPx(s, 5, 6, Color{255, 255, 255, 120});
                setPx(s, 11, 4, Color{255, 255, 255, 90});
            }

            // A couple dark pixels to add contrast
            setPx(s, 7, 12, dk);
            setPx(s, 9, 12, dk);
            break;
        }
        case EffectKind::Levitation: {
            Color c = pulse(Color{175, 205, 255, 255}, 8);
            Color c2 = mul(c, 0.70f);

            // Upward arrow + little wind ticks
            line(s, 8, 3, 8, 12, c);
            line(s, 8, 3, 5, 6, c);
            line(s, 8, 3, 11, 6, c);

            line(s, 3, 11, 5, 11, c2);
            line(s, 11, 9, 13, 9, c2);
            break;
        }
        case EffectKind::Fear: {
            Color c = pulse(Color{255, 205, 120, 255}, 10);
            Color dk{50, 25, 10, 255};

            // An exclamation mark inside a little "shiver" halo.
            rect(s, 7, 3, 2, 7, c);
            setPx(s, 8, 12, c);
            circle(s, 8, 8, 5, mul(c, 0.45f));

            setPx(s, 8, 6, dk);
            setPx(s, 8, 9, dk);
            break;
        }
        case EffectKind::Hallucination: {
            Color c = pulse(Color{255, 140, 255, 255}, 14);
            Color c2 = pulse(Color{140, 220, 255, 255}, 10);

            // A tiny "kaleidoscope" star.
            line(s, 8, 2, 8, 14, mul(c, 0.75f));
            line(s, 2, 8, 14, 8, mul(c2, 0.75f));
            line(s, 3, 3, 13, 13, mul(c2, 0.55f));
            line(s, 13, 3, 3, 13, mul(c, 0.55f));
            circle(s, 8, 8, 2, add(c, 10, 10, 10));
            setPx(s, 8, 8, Color{20, 20, 30, 255});
            break;
        }
        default:
            break;
    }

    // A crisp outline helps tiny HUD icons read against textured panels.
    finalizeSprite(s, hash32(static_cast<uint32_t>(kind) ^ 0x51A11u), frame, /*outlineAlpha=*/220, /*shadowAlpha=*/0);
    return resampleSpriteToSize(s, pxSize);
}

