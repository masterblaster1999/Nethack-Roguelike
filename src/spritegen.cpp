#include "spritegen.hpp"
#include "spritegen3d.hpp"
#include "game.hpp"      // EntityKind
#include "items.hpp"     // ItemKind, ProjectileKind
#include "vtuber_gen.hpp"
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

SpritePixels resampleSpriteToSizeInternal(const SpritePixels& src, int pxSize) {
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


// --- Tiny 2D noise helpers (used by procedural VFX tiles) -------------------

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

inline float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float hash01_16(uint32_t v) {
    return static_cast<float>(v & 0xFFFFu) / 65535.0f;
}

// Lightweight value noise (bilinear interpolation on a hashed lattice).
// `period` is expressed in the same units as x/y (pixels in our 16x16 design grid).
inline float valueNoise2D01(float x, float y, uint32_t seed, float period) {
    period = std::max(0.001f, period);

    const float gx = x / period;
    const float gy = y / period;

    const int ix = static_cast<int>(std::floor(gx));
    const int iy = static_cast<int>(std::floor(gy));

    const float fx = gx - static_cast<float>(ix);
    const float fy = gy - static_cast<float>(iy);

    auto lattice = [&](int lx, int ly) -> float {
        const uint32_t h = hash32(hashCombine(seed, hashCombine(static_cast<uint32_t>(lx), static_cast<uint32_t>(ly))));
        return hash01_16(h);
    };

    const float n00 = lattice(ix,     iy);
    const float n10 = lattice(ix + 1, iy);
    const float n01 = lattice(ix,     iy + 1);
    const float n11 = lattice(ix + 1, iy + 1);

    const float u = smoothstep01(fx);
    const float v = smoothstep01(fy);

    const float a = lerpf(n00, n10, u);
    const float b = lerpf(n01, n11, u);
    return lerpf(a, b, v);
}

// Tiny 3-octave fBm (fixed weights) in [0,1]. Useful for smoky / turbulent masks.
inline float fbm2D01(float x, float y, uint32_t seed) {
    const float n0 = valueNoise2D01(x,          y,          seed ^ 0xA531F00Du, 8.0f);
    const float n1 = valueNoise2D01(x + 19.1f,  y - 7.7f,  seed ^ 0xC0FFEE11u, 4.0f);
    const float n2 = valueNoise2D01(x - 13.3f,  y + 27.9f, seed ^ 0x1234BEEFu, 2.0f);
    return (n0 * 0.55f) + (n1 * 0.30f) + (n2 * 0.15f);
}


// --- Looped noise helpers (seamless 4-frame cycle) ---------------------
// Many of our procedural tiles/sprites are authored as a tiny flipbook (FRAMES=4).
// To avoid harsh per-frame flicker, we animate by *moving the sampling point* around a
// circle in noise-space. Because cos/sin return to the same point every 2π, the
// animation loops seamlessly.
static constexpr float TAU = 6.28318530718f;

inline float phase01_4(int frame) {
    return static_cast<float>(frame & 3) * 0.25f; // 0, 0.25, 0.5, 0.75
}

inline float phaseAngle_4(int frame) {
    return phase01_4(frame) * TAU;
}

inline float loopValueNoise2D01(float x, float y, uint32_t seed, float period, int frame, float radius) {
    const float ang = phaseAngle_4(frame);
    const float ox = std::cos(ang) * radius;
    const float oy = std::sin(ang) * radius;
    return valueNoise2D01(x + ox, y + oy, seed, period);
}

inline float loopFbm2D01(float x, float y, uint32_t seed, int frame, float radius) {
    const float ang = phaseAngle_4(frame);
    const float ox = std::cos(ang) * radius;
    const float oy = std::sin(ang) * radius;
    return fbm2D01(x + ox, y + oy, seed);
}

// --- Curl-noise / flow-warp helpers ----------------------------------------
// For smoke/fire-like visuals, simple domain-warped fBm already looks good.
// But we can push it further by warping sample points along a divergence-free
// velocity field derived from noise ("curl noise"). This creates a more
// convincing "advected" look without running a full fluid solver.
//
// These helpers are intentionally lightweight (tiny grids, few steps) because
// spritegen runs for many variants at startup.

struct V2 {
    float x = 0.0f;
    float y = 0.0f;
};

inline V2 v2Add(V2 a, V2 b) { return { a.x + b.x, a.y + b.y }; }
inline V2 v2Mul(V2 a, float s) { return { a.x * s, a.y * s }; }

inline V2 v2Norm(V2 v) {
    const float l2 = v.x * v.x + v.y * v.y;
    if (l2 < 1e-8f) return {0.0f, 0.0f};
    const float inv = 1.0f / std::sqrt(l2);
    return { v.x * inv, v.y * inv };
}

// Curl of a scalar field n(x,y): v = (dn/dy, -dn/dx)
inline V2 curlLoopFbm2D(float x, float y, uint32_t seed, int frame, float loopRadius, float eps) {
    eps = std::max(0.05f, eps);
    const float nL = loopFbm2D01(x - eps, y,       seed, frame, loopRadius);
    const float nR = loopFbm2D01(x + eps, y,       seed, frame, loopRadius);
    const float nD = loopFbm2D01(x,       y - eps, seed, frame, loopRadius);
    const float nU = loopFbm2D01(x,       y + eps, seed, frame, loopRadius);

    const float dndx = (nR - nL) / (2.0f * eps);
    const float dndy = (nU - nD) / (2.0f * eps);
    return { dndy, -dndx };
}

// Multi-scale curl field: combine two curls at different frequencies for richer motion.
inline V2 flowVelocity(V2 p, uint32_t seed, int frame) {
    const V2 c1 = curlLoopFbm2D(p.x * 0.85f,           p.y * 0.85f,
                               seed ^ 0xA11CE5u, frame, /*loopRadius=*/2.6f, /*eps=*/0.40f);
    const V2 c2 = curlLoopFbm2D(p.x * 1.65f + 11.7f,   p.y * 1.65f - 9.2f,
                               seed ^ 0xC0FFEE11u, frame, /*loopRadius=*/1.9f, /*eps=*/0.28f);

    V2 v = v2Add(v2Mul(c1, 0.72f), v2Mul(c2, 0.28f));

    // Make speed stable across the tiny 16x16 domain: normalize, then allow
    // a small, looped pulse so different seeds don't look identical.
    v = v2Norm(v);
    const float ang = phaseAngle_4(frame);
    const float pulse = 0.85f + 0.15f * std::sin(ang * 2.0f + hash01_16(seed) * TAU);
    return v2Mul(v, pulse);
}

// In-place flow-warp using a few short Euler steps.
inline void flowWarp2D(float& x, float& y, uint32_t seed, int frame, float strength, int steps) {
    steps = std::clamp(steps, 1, 6);
    const float step = strength / static_cast<float>(steps);

    V2 p{ x, y };
    for (int i = 0; i < steps; ++i) {
        const uint32_t salt = static_cast<uint32_t>(i * 0x9E3779B9u);
        const V2 v = flowVelocity(p, seed ^ salt, frame);
        p.x += v.x * step;
        p.y += v.y * step;
    }

    x = p.x;
    y = p.y;
}

// --- Reaction-diffusion helpers (Gray-Scott) ------------------------------
// A tiny Gray-Scott reaction-diffusion simulation gives us organic, rune-like
// "worm" patterns from a deterministic seed. We use this as a *base* field for
// arcane UI / shrine visuals, and animate it by smoothly drifting the sampling
// coordinates around a circle (seamless 4-frame loop) — similar to our looped
// noise trick, but with a very different underlying texture.
//
// NOTE: This is intentionally lightweight (16x16 grid, modest iteration count).

struct RDField {
    static constexpr int W = 16;
    static constexpr int H = 16;
    std::vector<float> U;
    std::vector<float> V;
    RDField() : U(static_cast<size_t>(W * H), 1.0f),
                V(static_cast<size_t>(W * H), 0.0f) {}
};

inline int rdWrap(int v, int m) {
    v %= m;
    if (v < 0) v += m;
    return v;
}

inline size_t rdIndex(int x, int y) {
    x = rdWrap(x, RDField::W);
    y = rdWrap(y, RDField::H);
    return static_cast<size_t>(y * RDField::W + x);
}

inline float rdClamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

inline float smoothstepEdge(float a, float b, float x) {
    if (a == b) return (x < a) ? 0.0f : 1.0f;
    float t = (x - a) / (b - a);
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline void rdLaplacian(const RDField& f, int x, int y, float& lapU, float& lapV) {
    const size_t c = rdIndex(x, y);

    auto U = [&](int ix, int iy) -> float { return f.U[rdIndex(ix, iy)]; };
    auto V = [&](int ix, int iy) -> float { return f.V[rdIndex(ix, iy)]; };

    const float uC = f.U[c];
    const float vC = f.V[c];

    // Classic 3x3 kernel used in many Gray-Scott examples:
    // center -1, cardinals 0.2, diagonals 0.05
    lapU = -uC;
    lapV = -vC;

    lapU += 0.20f * (U(x - 1, y) + U(x + 1, y) + U(x, y - 1) + U(x, y + 1));
    lapV += 0.20f * (V(x - 1, y) + V(x + 1, y) + V(x, y - 1) + V(x, y + 1));

    lapU += 0.05f * (U(x - 1, y - 1) + U(x + 1, y - 1) + U(x - 1, y + 1) + U(x + 1, y + 1));
    lapV += 0.05f * (V(x - 1, y - 1) + V(x + 1, y - 1) + V(x - 1, y + 1) + V(x + 1, y + 1));
}

// Deterministic Gray-Scott field seeded with a handful of "ink drops".
inline RDField makeRDSigilField(uint32_t seed, int iters) {
    iters = std::clamp(iters, 8, 260);

    RDField f;
    RDField tmp;

    RNG rng(hash32(seed ^ 0xA7C4A11u));

    // Seed a few V "droplets".
    const int drops = 5 + rng.range(0, 4);
    for (int i = 0; i < drops; ++i) {
        const int cx = rng.range(2, RDField::W - 3);
        const int cy = rng.range(2, RDField::H - 3);
        const int r  = rng.range(1, 2);
        for (int oy = -r; oy <= r; ++oy) {
            for (int ox = -r; ox <= r; ++ox) {
                if (ox * ox + oy * oy > r * r) continue;
                const size_t id = rdIndex(cx + ox, cy + oy);
                f.U[id] = 0.0f;
                f.V[id] = 1.0f;
            }
        }
    }

    // Slight parameter variation per seed (keeps different seeds from looking identical).
    const float Du = 0.16f;
    const float Dv = 0.08f;

    float feed = 0.034f + (hash01_16(hash32(seed ^ 0xF33D1234u)) - 0.5f) * 0.010f;
    float kill = 0.062f + (hash01_16(hash32(seed ^ 0xBEEFC0DEu)) - 0.5f) * 0.010f;
    feed = std::clamp(feed, 0.020f, 0.060f);
    kill = std::clamp(kill, 0.045f, 0.075f);

    for (int iter = 0; iter < iters; ++iter) {
        for (int y = 0; y < RDField::H; ++y) {
            for (int x = 0; x < RDField::W; ++x) {
                const size_t id = static_cast<size_t>(y * RDField::W + x);

                const float u = f.U[id];
                const float v = f.V[id];

                float lapU = 0.0f, lapV = 0.0f;
                rdLaplacian(f, x, y, lapU, lapV);

                const float uvv = u * v * v;
                const float du = Du * lapU - uvv + feed * (1.0f - u);
                const float dv = Dv * lapV + uvv - (kill + feed) * v;

                // dt ~ 1 is fine for this tiny grid; clamp for stability.
                tmp.U[id] = rdClamp01(u + du);
                tmp.V[id] = rdClamp01(v + dv);
            }
        }
        f.U.swap(tmp.U);
        f.V.swap(tmp.V);
    }

    return f;
}

inline float rdSampleV(const RDField& f, float x, float y) {
    // Wrap coordinates into [0,W/H).
    const float fx = x - std::floor(x / static_cast<float>(RDField::W)) * static_cast<float>(RDField::W);
    const float fy = y - std::floor(y / static_cast<float>(RDField::H)) * static_cast<float>(RDField::H);

    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;

    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const float v00 = f.V[rdIndex(x0, y0)];
    const float v10 = f.V[rdIndex(x1, y0)];
    const float v01 = f.V[rdIndex(x0, y1)];
    const float v11 = f.V[rdIndex(x1, y1)];

    const float a = lerpf(v00, v10, tx);
    const float b = lerpf(v01, v11, tx);
    return lerpf(a, b, ty);
}

inline float rdGradMag(const RDField& f, float x, float y) {
    const float vL = rdSampleV(f, x - 1.0f, y);
    const float vR = rdSampleV(f, x + 1.0f, y);
    const float vD = rdSampleV(f, x, y - 1.0f);
    const float vU = rdSampleV(f, x, y + 1.0f);
    return std::abs(vR - vL) + std::abs(vU - vD);
}

// Simple row-wise domain warp (keeps pixel art crisp). Intended for ethereal sprites
// like ghosts and subtle HUD shimmer.
SpritePixels warpSpriteRowWave(const SpritePixels& src, uint32_t seed, int frame, float ampPx, float freq) {
    SpritePixels out = makeSprite(src.w, src.h, {0, 0, 0, 0});

    const float ang = phaseAngle_4(frame);
    const float base = static_cast<float>(seed & 0xFFu) * 0.017f;

    for (int y = 0; y < src.h; ++y) {
        const float yy = static_cast<float>(y);

        // Mix a sinusoid and a tiny looped noise for a less "robotic" wobble.
        float s = std::sin(ang + base + yy * freq);
        float n = loopValueNoise2D01(yy * 0.85f + 3.7f, 9.1f, seed ^ 0x51A11u, 6.0f, frame, 2.2f) - 0.5f;

        int shift = static_cast<int>(std::lround(s * ampPx + n * 0.75f));
        shift = std::clamp(shift, -2, 2);

        for (int x = 0; x < src.w; ++x) {
            const int sx = x - shift;
            if (sx < 0 || sx >= src.w) continue;
            out.at(x, y) = src.at(sx, y);
        }
    }

    return out;
}

// Nearest-neighbor sprite scaling around an anchor point (inverse mapping).
// This is used for pixel-art-friendly squash & stretch.
SpritePixels warpSpriteScaleNearest(const SpritePixels& src, float sx, float sy, float anchorX, float anchorY) {
    sx = std::clamp(sx, 0.40f, 2.50f);
    sy = std::clamp(sy, 0.40f, 2.50f);

    // Early-out for identity.
    if (std::abs(sx - 1.0f) < 0.0005f && std::abs(sy - 1.0f) < 0.0005f) return src;

    SpritePixels out = makeSprite(src.w, src.h, {0, 0, 0, 0});

    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            const float fx = (static_cast<float>(x) - anchorX) / sx + anchorX;
            const float fy = (static_cast<float>(y) - anchorY) / sy + anchorY;

            const int sx0 = static_cast<int>(std::lround(fx));
            const int sy0 = static_cast<int>(std::lround(fy));
            if (sx0 < 0 || sy0 < 0 || sx0 >= src.w || sy0 >= src.h) continue;

            out.at(x, y) = src.at(sx0, sy0);
        }
    }

    return out;
}

// Column-wise domain warp (keeps pixel art crisp). Useful for slithering / "waving" motion.
SpritePixels warpSpriteColumnWave(const SpritePixels& src, uint32_t seed, int frame, float ampPx, float freq) {
    SpritePixels out = makeSprite(src.w, src.h, {0, 0, 0, 0});

    const float ang = phaseAngle_4(frame);
    const float base = static_cast<float>((seed >> 8) & 0xFFu) * 0.017f;

    for (int x = 0; x < src.w; ++x) {
        const float xx = static_cast<float>(x);

        // Mix a sinusoid and a tiny looped noise for a more organic slither.
        float s = std::sin(ang + base + xx * freq);
        float n = loopValueNoise2D01(xx * 0.85f + 2.7f, 7.9f, seed ^ 0xA11CE5u, 6.0f, frame, 2.2f) - 0.5f;

        int shift = static_cast<int>(std::lround(s * ampPx + n * 0.75f));
        shift = std::clamp(shift, -2, 2);

        for (int y = 0; y < src.h; ++y) {
            const int sy = y - shift;
            if (sy < 0 || sy >= src.h) continue;
            out.at(x, y) = src.at(x, sy);
        }
    }

    return out;
}

// Side-only (wing/leg) horizontal wave: keeps the center mass stable and pushes
// pixels near the left/right edges in/out. Great for bat wing flaps and spider leg scuttles.
SpritePixels warpSpriteSideWave(const SpritePixels& src, uint32_t seed, int frame, float ampPx, float freq, int margin) {
    SpritePixels out = makeSprite(src.w, src.h, {0, 0, 0, 0});

    const float ang = phaseAngle_4(frame);
    const float base = static_cast<float>((seed >> 16) & 0xFFu) * 0.019f;

    const int cx = src.w / 2;
    margin = std::clamp(margin, 0, std::max(0, cx - 1));

    for (int y = 0; y < src.h; ++y) {
        const float yy = static_cast<float>(y);

        // Cosine gives us an "open/mid/closed/mid" cycle across 4 frames.
        float c = std::cos(ang + base + yy * freq);
        float n = loopValueNoise2D01(yy * 0.70f + 3.1f, 5.3f, seed ^ 0xF1A9u, 6.0f, frame, 2.0f) - 0.5f;

        int sh = static_cast<int>(std::lround(c * ampPx + n * 0.55f));
        sh = std::clamp(sh, -2, 2);

        for (int x = 0; x < src.w; ++x) {
            int sx = x;
            if (x < cx - margin) sx = x + sh;
            else if (x > cx + margin) sx = x - sh;

            if (sx < 0 || sx >= src.w) continue;
            out.at(x, y) = src.at(sx, y);
        }
    }

    return out;
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

    // -----------------------------------------------------------------
    // Animated liquid (4-frame loop): sloshy surface + internal swirl.
    //
    // We keep everything deterministic from seed, and drive motion with the
    // same 4-frame looping phase helpers used elsewhere in spritegen.
    // -----------------------------------------------------------------
    const float ang = phaseAngle_4(frame);

    auto quant3 = [](float v, float t) -> int {
        if (v > t) return 1;
        if (v < -t) return -1;
        return 0;
    };

    // Per-potion slosh parameters (stable across frames).
    const float p0 = hash01_16(seed ^ 0xA11CE5u) * TAU;
    const float p1 = hash01_16(seed ^ 0xC0FFEEu) * TAU;

    // Small, quantized slosh so it reads as liquid movement even at 16x16.
    const int slosh = quant3(std::sin(ang + p0), 0.28f);
    const int tilt  = quant3(std::cos(ang + p1), 0.35f);

    const int fx0 = 7;
    const int fx1 = 8;
    const int baseTop = 7;
    const int fyBot = 11;

    int topY[2] = { baseTop, baseTop };

    for (int xx = fx0; xx <= fx1; ++xx) {
        const int side = (xx == fx0) ? -1 : 1;
        int top = baseTop + slosh + side * tilt;
        top = std::clamp(top, 6, 9);
        topY[xx - fx0] = top;

        for (int yy = top; yy <= fyBot; ++yy) {
            // Brighter near the surface.
            const float t01 = (fyBot > top) ? (static_cast<float>(yy - top) / static_cast<float>(fyBot - top)) : 0.0f;
            const float surf = 1.0f - t01;

            // Internal swirl using looped fBm (so the flipbook wraps cleanly).
            float n = loopFbm2D01(xx * 0.90f + 2.1f,
                                 yy * 0.90f - 3.7f,
                                 seed ^ 0xB00B1Eu,
                                 frame,
                                 2.0f);
            n = (n - 0.5f); // [-0.5, 0.5]

            float shade = 0.70f + 0.26f * surf + 0.18f * n;
            Color c = rampShade(st.fluid, shade, xx, yy);

            // Style-specific accents.
            if (st.metallic) {
                // Flakes / shimmer that move coherently (not per-frame random).
                const float m = loopValueNoise2D01(xx * 2.20f + 7.3f,
                                                  yy * 2.20f - 1.9f,
                                                  seed ^ 0x51A11u,
                                                  2.5f,
                                                  frame,
                                                  1.7f);
                if (m > 0.84f) c = {255, 255, 255, 210};
                else if (m < 0.18f) c = mul(c, 0.85f);
            }

            if (st.murky) {
                // Dark specks that drift subtly.
                const float m = loopValueNoise2D01(xx * 1.80f - 4.1f,
                                                  yy * 1.80f + 3.9f,
                                                  seed ^ 0xD17F00Du,
                                                  3.0f,
                                                  frame,
                                                  1.9f);
                if (m < 0.20f) c = mul(c, 0.62f);
            }

            if (st.milky) {
                // Soft, creamy swirl highlights.
                const float m = loopValueNoise2D01(xx * 1.60f + 1.1f,
                                                  yy * 1.60f - 9.3f,
                                                  seed ^ 0x0111C0DEu,
                                                  3.5f,
                                                  frame,
                                                  2.0f);
                if (m > 0.72f) c = add(c, 20, 18, 12);
            }

            setPx(s, xx, yy, c);
        }
    }

    // Surface highlight line (helps sell the "sloshing" motion).
    for (int xx = fx0; xx <= fx1; ++xx) {
        const int top = topY[xx - fx0];
        setPx(s, xx, top, st.fluidHi);
    }

    // Tiny bubble: coherent motion driven by looped noise.
    {
        const float b = loopValueNoise2D01(9.1f, 2.3f, seed ^ 0xB0BB1Eu, 3.0f, frame, 2.1f);
        if (b > 0.55f) {
            const int bx = (b > 0.80f) ? 7 : 8;
            int by = 8 + static_cast<int>(std::lround((1.0f - b) * 3.0f));
            by = std::clamp(by, topY[bx - fx0] + 1, fyBot);
            setPx(s, bx, by, {255, 255, 255, 90});
        }
    }

    if (st.smoky) {
        // A small smoke curl above the bottle. Use looped noise so it's not
        // a harsh 2-frame blink.
        for (int yy = 1; yy <= 4; ++yy) {
            for (int xx = 8; xx <= 13; ++xx) {
                const float n = loopFbm2D01(xx * 1.15f, yy * 1.15f,
                                           seed ^ 0x5E10E12u,
                                           frame,
                                           2.2f);
                if (n > 0.74f) {
                    const int a0 = 70 + static_cast<int>(std::lround((n - 0.74f) * 420.0f));
                    int a1 = std::clamp(a0 - (yy * 10), 45, 150);
                    setPx(s, xx, yy, {190, 190, 205, static_cast<uint8_t>(a1)});
                }
            }
        }
    }

    // Glass highlight (subtle).
    if (frame % 2 == 1) {
        setPx(s, 9, 5, {255, 255, 255, 130});
        setPx(s, 9, 7, {255, 255, 255, 90});
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

    // A tiny "flutter" cue: alternate shading of the curls (keeps outline stable).
    if ((frame & 3) == 1) {
        rect(s, 4, 6, 1, 5, mul(paper, 0.68f));
        rect(s, 11, 6, 1, 5, mul(paper, 0.80f));
    } else if ((frame & 3) == 3) {
        rect(s, 4, 6, 1, 5, mul(paper, 0.80f));
        rect(s, 11, 6, 1, 5, mul(paper, 0.68f));
    }

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
            drawRuneGlyph(s, x, y, g, ink);
        }
    }

    // -----------------------------------------------------------------
    // Animated ink shimmer (4-frame loop).
    //
    // Instead of per-frame random sparkles, we modulate the ink with looped
    // noise so the label reads as "magically alive" without harsh blinking.
    // -----------------------------------------------------------------
    const uint32_t shSeed = seed ^ (0x1A55B11Eu + static_cast<uint32_t>(a) * 0x9E3779B9u);

    for (int yy = 6; yy <= 12; ++yy) {
        for (int xx = 5; xx <= 11; ++xx) {
            Color c = getPx(s, xx, yy);
            if (c.a == 0) continue;

            // Only touch the rune ink pixels.
            if (c.r == ink.r && c.g == ink.g && c.b == ink.b) {
                const float n = loopValueNoise2D01(xx * 1.35f, yy * 1.35f,
                                                  shSeed,
                                                  3.5f,
                                                  frame,
                                                  1.8f);
                if (n > 0.80f) {
                    c = add(c, 55, 45, 35);
                } else if (n < 0.22f) {
                    c = mul(c, 0.78f);
                }
                setPx(s, xx, yy, c);
            }
        }
    }

    // A small traveling paper glint.
    {
        const float g = loopValueNoise2D01(0.9f, 3.7f, seed ^ 0x51A11u, 5.0f, frame, 2.0f);
        const int fx = 5 + std::clamp(static_cast<int>(std::lround(g * 5.0f)), 0, 5);
        setPx(s, fx, 7, {255,255,255,110});
        if ((frame & 3) == 1) setPx(s, fx + 1, 8, {255,255,255,70});
    }

    // A couple tiny magic dust pixels around the scroll (subtle).
    if ((frame & 3) == 1) {
        setPx(s, 12, 6, {255,255,255,70});
    } else if ((frame & 3) == 3) {
        setPx(s, 3, 10, {255,255,255,60});
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

        // Opal: 4-step iridescent cycle.
        if ((a % 16u) == 8u) {
            static const Color OPAL[4] = {
                {200, 255, 240, 235},
                {255, 210, 255, 235},
                {255, 245, 200, 235},
                {210, 220, 255, 235},
            };
            gem = OPAL[static_cast<size_t>(frame & 3)];
        }

        circle(s, 8, 5, 2, gem);
        circle(s, 8, 5, 1, mul(gem, 0.85f));

        // Gem glint orbits around the stone (reads as rotation).
        static const int gx[4] = { 9, 8, 7, 8 };
        static const int gy[4] = { 5, 4, 5, 6 };
        const int gi = frame & 3;
        setPx(s, gx[gi], gy[gi], {255,255,255,140});
    }

    // Specular glint orbit around the band.
    static const int hx[4] = { 9, 10, 7, 6 };
    static const int hy[4] = { 7,  9, 11, 9 };
    const int i = frame & 3;
    setPx(s, hx[i], hy[i], {255,255,255,110});
    setPx(s, hx[(i + 1) & 3], hy[(i + 1) & 3], {255,255,255,70});

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

    // Tip ornament (pulses on the 4-frame loop).
    const float ang = phaseAngle_4(frame);
    const float pulse01 = 0.5f + 0.5f * std::cos(ang); // 1,0.5,0,0.5
    const float f = 0.78f + 0.22f * pulse01;
    circle(s, 12, 4, 1, mul(tip, f));

    // Orbiting sparkle around the tip for magical materials.
    if ((a % 16u) >= 12u) {
        static const int ox[4] = { 1, 0, -1, 0 };
        static const int oy[4] = { 0, -1, 0, 1 };
        const int i = frame & 3;
        setPx(s, 12 + ox[i], 4 + oy[i], {255,255,255,110});
    } else if ((frame & 3) == 1) {
        // Non-magical wands still get a tiny highlight.
        setPx(s, 13, 4, {255,255,255,120});
    }

    // Tiny rune notches along the shaft (deterministic).
    const uint32_t h = hash32(hashCombine(seed, static_cast<uint32_t>(a) * 0x9E37u));
    for (int i = 0; i < 3; ++i) {
        const int t = 2 + i * 3;
        const int x = 4 + t;
        const int y = 12 - t;
        if ((h >> i) & 1u) setPx(s, x, y, {30, 25, 20, 200});
    }

    // Energy crawl: highlight one notch per frame (0,1,2,1) so it loops smoothly.
    {
        const int seq[4] = { 0, 1, 2, 1 };
        const int i = seq[frame & 3];
        const int t = 2 + i * 3;
        const int x = 4 + t;
        const int y = 12 - t;
        Color g = add(tip, -40, -40, -40);
        g.a = 160;
        setPx(s, x, y, g);
        // Small trailing sparkle.
        if ((frame & 3) == 1 || (frame & 3) == 3) {
            setPx(s, x - 1, y + 1, {255,255,255,70});
        }
    }

    // Subtle sparkle for magical materials.
    if ((a % 16u) >= 12u) {
        const float n = loopValueNoise2D01(10.0f, 6.0f, seed ^ 0x51A11u, 4.0f, frame, 2.0f);
        if (n > 0.72f) {
            setPx(s, 10, 6, {255,255,255,90});
        }
    }

    (void)rng;
}



float densityFor(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return 0.55f;
        case EntityKind::Goblin: return 0.58f;
        case EntityKind::Leprechaun: return 0.50f;
        case EntityKind::Nymph: return 0.52f;
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
        case EntityKind::Guard: return 0.60f;
        case EntityKind::Minotaur: return 0.76f;
        default: return 0.55f;
    }
}

Color baseColorFor(EntityKind k, RNG& rng) {
    switch (k) {
        case EntityKind::Player: return add({ 160, 200, 255, 255 }, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
        case EntityKind::Goblin: return add({ 80, 180, 90, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Leprechaun: return add({ 60, 210, 90, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        case EntityKind::Nymph: return add({ 220, 160, 210, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
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
        case EntityKind::Guard: return add({ 170, 185, 210, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
        case EntityKind::Minotaur: return add({ 160, 90, 60, 255 }, rng.range(-20,20), rng.range(-20,20), rng.range(-20,20));
        default: return add({ 180, 180, 180, 255 }, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
    }
}

} // namespace

// Public wrapper: keep the heavy lifting in the anonymous namespace helpers but
// expose a stable entry point for other translation units (renderer UI previews,
// etc.).
SpritePixels resampleSpriteToSize(const SpritePixels& src, int pxSize) {
    return resampleSpriteToSizeInternal(src, pxSize);
}


SpritePixels generateEntitySprite(EntityKind kind, uint32_t seed, int frame, bool use3d, int pxSize, bool isometric, bool isoRaytrace) {
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
                    // Use a *looped* noise so the shimmer animates without harsh per-frame flicker
                    // (wraps cleanly across the 4-frame flipbook).
                    const float noise = loopValueNoise2D01(static_cast<float>(xx) + 0.37f,
                                                        static_cast<float>(yy) - 1.91f,
                                                        seed ^ 0xC0DEC0DEu,
                                                        5.0f,
                                                        frame,
                                                        2.2f);
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
            case EntityKind::Guard: {
                // Sword + simple shield for silhouette.
                drawBlade(handX, handY, dir, -1, 4, mul(metal, 0.95f), grip);
                drawShield(rightHand ? 2 : 11, 8, add({120, 140, 170, 255}, rngVar.range(-10, 10), rngVar.range(-10, 10), rngVar.range(-10, 10)));
                break;
            }
            default:
                break;
        }
    }

    // ---------------------------------------------------------------------
    // Procedural sprite-space idle animation (4-frame loop).
    //
    // Renderer-side motion already provides hop/squash on movement; these warps
    // add *per-sprite* life even while standing still (bat wing flap, slime pulse,
    // snake slither, spider leg scuttle, etc.).
    //
    // NOTE: These are designed to preserve crisp pixel art (nearest-neighbor warps)
    // and to loop seamlessly across FRAMES=4.
    // ---------------------------------------------------------------------
    if (kind == EntityKind::Slime) {
        // Classic squash & stretch pulse.
        const float ang = phaseAngle_4(frame);
        const float osc = std::cos(ang); // 1,0,-1,0
        const float sx = 1.0f + 0.10f * osc;
        const float sy = 1.0f - 0.10f * osc;
        s = warpSpriteScaleNearest(s, sx, sy, /*anchorX=*/7.5f, /*anchorY=*/15.0f);
    }

    if (kind == EntityKind::Bat) {
        // Wing flap: push side membranes in/out while keeping the torso stable.
        s = warpSpriteSideWave(s, seed ^ 0xBA7F00Du, frame, /*ampPx=*/1.25f, /*freq=*/0.22f, /*margin=*/2);
    }

    if (kind == EntityKind::Snake) {
        // Slither: vertical wave travels along the body.
        s = warpSpriteColumnWave(s, seed ^ 0x51E7E1E7u, frame, /*ampPx=*/1.05f, /*freq=*/0.55f);
    }

    if (kind == EntityKind::Spider) {
        // Leg scuttle: subtle outward/inward splay.
        s = warpSpriteSideWave(s, seed ^ 0x5A1D3E11u, frame, /*ampPx=*/1.05f, /*freq=*/0.30f, /*margin=*/3);
    }

    if (kind == EntityKind::Wolf || kind == EntityKind::Dog) {
        // Tail wag (slight asymmetry reads as life).
        const bool tailRight = ((seed >> 6) & 1u) != 0u;
        const int wag = (frame == 1) ? 1 : (frame == 3) ? -1 : 0;
        const int up  = (frame == 2) ? -1 : 0;

        Color tail = mul(base, 0.82f);
        const int dir = tailRight ? 1 : -1;

        const int bx = tailRight ? 11 : 4;
        const int by = 10;

        auto safeSet = [&](int x, int y, Color c) {
            if (x < 0 || y < 0 || x >= 16 || y >= 16) return;
            if (c.a == 0) return;
            // Prefer empty pixels so we don't erase the body.
            if (s.at(x, y).a == 0 || s.at(x, y).a < 80) setPx(s, x, y, c);
        };

        // Base + mid segment.
        safeSet(bx, by, tail);
        safeSet(bx + dir, by, tail);

        // Tip wags + lifts slightly.
        safeSet(bx + dir * (2 + wag), by + up, add(tail, 12, 12, 12));
    }


    if (kind == EntityKind::Ghost) {
        // Ethereal wobble: procedurally warp rows so the sprite "breathes" / drifts
        // without needing authored hand-drawn frames.
        s = warpSpriteRowWave(s, seed ^ 0xB00FCA11u, frame, /*ampPx=*/1.05f, /*freq=*/0.38f);

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
    if (use3d) {
        return isometric ? renderSprite3DEntityIso(kind, s, seed, frame, pxSize, isoRaytrace)
                         : renderSprite3DEntity(kind, s, seed, frame, pxSize);
    }
    return resampleSpriteToSize(s, pxSize);
}


SpritePixels generateItemSprite(ItemKind kind, uint32_t seed, int frame, bool use3d, int pxSize, bool isometric, bool isoRaytrace) {
    pxSize = clampSpriteSize(pxSize);
    RNG rng(hash32(seed));
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    auto render3d = [&](const SpritePixels& base) -> SpritePixels {
        return isometric ? renderSprite3DItemIso(kind, base, seed, frame, pxSize, isoRaytrace)
                         : renderSprite3DItem(kind, base, seed, frame, pxSize);
    };

    // NetHack-style identification visuals:
    // if the renderer sets SPRITE_SEED_IDENT_APPEARANCE_FLAG, we generate
    // appearance-based art for identifiable items (potion/scroll/ring/wand)
    // so the sprite itself doesn't leak the true item kind.
    if ((seed & SPRITE_SEED_IDENT_APPEARANCE_FLAG) != 0u) {
        const uint8_t app = static_cast<uint8_t>(seed & 0xFFu);
        if (isPotionKind(kind)) {
            drawPotionAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? render3d(s)
                         : resampleSpriteToSize(s, pxSize);
        }
        if (isScrollKind(kind)) {
            drawScrollAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? render3d(s)
                         : resampleSpriteToSize(s, pxSize);
        }
        if (kind == ItemKind::RingMight || kind == ItemKind::RingAgility || kind == ItemKind::RingFocus ||
            kind == ItemKind::RingProtection || kind == ItemKind::RingSearching || kind == ItemKind::RingSustenance) {
            drawRingAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? render3d(s)
                         : resampleSpriteToSize(s, pxSize);
        }
        if (kind == ItemKind::WandSparks || kind == ItemKind::WandDigging || kind == ItemKind::WandFireball) {
            drawWandAppearance(s, seed, rng, app, frame);
            finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
            return use3d ? render3d(s)
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

    auto drawSpellbook = [&](Color cover, Color rune) {
        // Simple hardbound book with a rune on the cover.
        outlineRect(s, 4, 4, 8, 10, mul(cover, 0.75f));
        rect(s, 5, 5, 6, 8, cover);

        // Spine
        line(s, 4, 4, 4, 13, mul(cover, 0.6f));

        // Clasp
        rect(s, 10, 8, 1, 2, mul({220,220,220,255}, 0.85f));

        // Rune (tiny cross-ish glyph)
        setPx(s, 8, 8, rune);
        setPx(s, 8, 9, rune);
        setPx(s, 7, 8, rune);
        setPx(s, 9, 8, rune);

        if (frame % 2 == 1) {
            // Cover highlight
            setPx(s, 6, 6, {255,255,255,110});
            setPx(s, 7, 6, {255,255,255,90});
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

        // --- Collectibles (append-only) ---
        case ItemKind::VtuberFigurine: {
            // A tiny chibi \"VTuber\" figurine: big head, big eyes, lots of hair color.
            // The persona text uses vtuberMixSeed(seed) too (vtuber_gen.hpp), so the name
            // and visual tend to \"match\" consistently across runs.
            RNG vrng(vtuberMixSeed(seed));

            static constexpr Color SKIN[] = {
                {255, 224, 200, 255},
                {245, 210, 180, 255},
                {235, 195, 165, 255},
                {255, 236, 220, 255},
                {225, 185, 155, 255},
            };
            static constexpr Color HAIR[] = {
                {245, 120, 200, 255}, // pink
                {120, 190, 255, 255}, // sky
                {165, 120, 255, 255}, // purple
                {255, 220, 120, 255}, // blonde
                { 90, 240, 190, 255}, // mint
                {235,  95,  95, 255}, // red
                {210, 210, 225, 255}, // silver
                { 40,  40,  55, 255}, // black
            };
            static constexpr Color EYES[] = {
                { 90, 210, 255, 255},
                {255, 120, 200, 255},
                {120, 255, 160, 255},
                {255, 190,  80, 255},
                {180, 130, 255, 255},
                {255, 255, 140, 255},
            };

            const int skinN = static_cast<int>(sizeof(SKIN) / sizeof(SKIN[0]));
            const int hairN = static_cast<int>(sizeof(HAIR) / sizeof(HAIR[0]));
            const int eyeN  = static_cast<int>(sizeof(EYES)  / sizeof(EYES[0]));

            Color skin = SKIN[vrng.range(0, skinN - 1)];
            Color skinShade = mul(skin, 0.88f);

            Color hair = HAIR[vrng.range(0, hairN - 1)];
            Color hairDark = mul(hair, 0.70f);
            Color hairLight = add(hair, 20, 20, 20);

            Color eye = EYES[vrng.range(0, eyeN - 1)];
            Color eyeDark = mul(eye, 0.70f);

            // Accent: a small hue-ish shift from eye color.
            Color accent = add(eye, vrng.range(-25, 25), vrng.range(-25, 25), vrng.range(-25, 25));
            Color outfit = mul(accent, 0.85f);

            const int hairStyle = vrng.range(0, 3);
            const int accessory = vrng.range(0, 4);

            const bool blink = ((frame + static_cast<int>(seed & 31u)) % 34) <= 1;
            const bool mouthOpen = ((frame + static_cast<int>((seed >> 5) & 31u)) % 16) < 6;

            // Hair base behind the head
            circle(s, 8, 6, 7, hairDark);
            circle(s, 8, 5, 6, hair);

            // Face / head (big)
            circle(s, 8, 9, 5, skin);
            circle(s, 8, 11, 4, skinShade);

            // Side locks (vary slightly per style)
            if (hairStyle == 0) {
                rect(s, 3, 7, 2, 6, hairDark);
                rect(s, 11, 7, 2, 6, hairDark);
            } else if (hairStyle == 1) {
                rect(s, 2, 8, 3, 6, hairDark);
                rect(s, 11, 8, 3, 6, hairDark);
                // tiny \"twin tail\" bobbles
                circle(s, 2, 12, 2, hair);
                circle(s, 14, 12, 2, hair);
            } else if (hairStyle == 2) {
                rect(s, 3, 8, 2, 5, hair);
                rect(s, 11, 8, 2, 5, hair);
            } else {
                rect(s, 3, 8, 2, 6, hairDark);
                rect(s, 11, 8, 2, 6, hair);
            }

            // Bangs / fringe
            switch (hairStyle) {
                case 0: // straight bangs
                    rect(s, 4, 6, 8, 2, hair);
                    rect(s, 4, 8, 8, 1, mul(hair, 0.85f));
                    break;
                case 1: // zig-zag bangs
                    for (int x = 4; x <= 11; ++x) {
                        int y = 6 + ((x + (seed & 3u)) % 2);
                        setPx(s, x, y, hair);
                        setPx(s, x, y + 1, mul(hair, 0.85f));
                    }
                    break;
                case 2: // side-swept
                    line(s, 4, 6, 11, 8, hair);
                    line(s, 4, 7, 11, 9, mul(hair, 0.85f));
                    break;
                case 3: // choppy
                default:
                    for (int x = 4; x <= 11; ++x) {
                        int y = 6 + (vrng.range(0, 1));
                        setPx(s, x, y, hair);
                    }
                    rect(s, 5, 8, 6, 1, mul(hair, 0.85f));
                    break;
            }

            // Accessory
            switch (accessory) {
                case 0: { // cat ears
                    // left ear
                    setPx(s, 5, 2, hair);
                    setPx(s, 4, 3, hair);
                    setPx(s, 5, 3, hair);
                    setPx(s, 6, 3, hair);
                    setPx(s, 5, 4, accent);
                    // right ear
                    setPx(s, 11, 2, hair);
                    setPx(s, 10, 3, hair);
                    setPx(s, 11, 3, hair);
                    setPx(s, 12, 3, hair);
                    setPx(s, 11, 4, accent);
                    break;
                }
                case 1: { // halo
                    Color gold = {255, 230, 140, 190};
                    circle(s, 8, 2, 3, gold);
                    circle(s, 8, 2, 2, {0,0,0,0});
                    break;
                }
                case 2: { // headset + mic
                    circle(s, 4, 9, 1, accent);
                    circle(s, 12, 9, 1, accent);
                    line(s, 12, 10, 14, 12, accent);
                    setPx(s, 14, 12, {255,255,255,110});
                    break;
                }
                case 3: { // ribbon
                    setPx(s, 8, 4, accent);
                    setPx(s, 7, 4, accent);
                    setPx(s, 9, 4, accent);
                    setPx(s, 6, 4, mul(accent, 0.85f));
                    setPx(s, 10, 4, mul(accent, 0.85f));
                    setPx(s, 8, 5, mul(accent, 0.85f));
                    break;
                }
                case 4: // tiny horns
                default: {
                    setPx(s, 5, 3, accent);
                    setPx(s, 6, 2, accent);
                    setPx(s, 11, 3, accent);
                    setPx(s, 10, 2, accent);
                    break;
                }
            }

            // Eyes (big)
            Color white = {245, 245, 245, 255};
            if (blink) {
                line(s, 5, 10, 7, 10, eyeDark);
                line(s, 9, 10, 11, 10, eyeDark);
            } else {
                rect(s, 5, 9, 3, 3, white);
                rect(s, 9, 9, 3, 3, white);

                // iris
                rect(s, 6, 10, 1, 2, eye);
                rect(s, 10, 10, 1, 2, eye);

                // darker top
                rect(s, 6, 9, 1, 1, eyeDark);
                rect(s, 10, 9, 1, 1, eyeDark);

                // highlight
                setPx(s, 6, 10, {255,255,255,170});
                setPx(s, 10, 10, {255,255,255,170});
            }

            // Mouth
            Color mouth = {120, 60, 70, 255};
            if (mouthOpen) {
                rect(s, 7, 13, 3, 1, mouth);
                setPx(s, 8, 12, mouth);
            } else {
                line(s, 7, 12, 9, 12, mouth);
            }

            // Blush (sometimes)
            if (vrng.chance(0.45f)) {
                Color blush = {255, 140, 170, 90};
                setPx(s, 4, 11, blush);
                setPx(s, 12, 11, blush);
                setPx(s, 5, 11, {255, 140, 170, 60});
                setPx(s, 11, 11, {255, 140, 170, 60});
            }

            // Outfit / base
            rect(s, 5, 14, 7, 2, outfit);
            rect(s, 5, 15, 7, 1, mul(outfit, 0.85f));
            // collar highlight
            setPx(s, 8, 14, {255,255,255,120});
            setPx(s, 7, 14, {255,255,255,90});
            setPx(s, 9, 14, {255,255,255,90});

            // Hair highlight flicker
            if (frame % 2 == 1) {
                setPx(s, 6, 5, hairLight);
                setPx(s, 10, 5, hairLight);
                setPx(s, 8, 4, {255,255,255,60});
            }

            break;
        }

        case ItemKind::VtuberHoloCard: {
            // A "holo card" for a procedural VTuber persona: a tiny framed portrait
            // with rarity-dependent border flair + deterministic "edition" variants
            // (foil / alt-art / signed / collab).
            //
            // NOTE: 16x16 sprites are tight; we keep this deliberately iconic.
            RNG vrng(vtuberMixSeed(seed ^ 0xA9B4C2D1u));

            const VtuberRarity rar = vtuberRarity(seed);
            const VtuberCardEdition ed = vtuberCardEdition(seed);
            const uint32_t partnerSeed = (ed == VtuberCardEdition::Collab) ? vtuberCollabPartnerSeed(seed) : 0u;

            const Color accent = vtuberAccentColor(seed);
            const Color accent2 = (partnerSeed != 0u) ? vtuberAccentColor(partnerSeed) : accent;
            const Color bg = {18, 18, 22, 255};

            // Paper tint by edition (subtle, but readable in 16x16).
            Color paper = mul({220, 220, 230, 255}, 0.78f);
            if (ed == VtuberCardEdition::Foil)   paper = mul({235, 235, 245, 255}, 0.82f);
            if (ed == VtuberCardEdition::AltArt) paper = mul(add(accent, 150, 150, 150), 0.55f);
            if (ed == VtuberCardEdition::Signed) paper = mul({230, 230, 240, 255}, 0.78f);
            if (ed == VtuberCardEdition::Collab) paper = mul({225, 225, 235, 255}, 0.78f);

            // Card body
            rect(s, 2, 1, 12, 14, mul(bg, 0.95f));
            rect(s, 3, 2, 10, 12, paper);

            // Inner "holo" sheen band (foil has extra sheen).
            const int sheenMod = (ed == VtuberCardEdition::Foil) ? 4 : 6;
            const int sheenWin = (ed == VtuberCardEdition::Foil) ? 3 : 2;
            if ((frame + static_cast<int>(seed & 7u)) % sheenMod <= sheenWin) {
                const uint8_t a = (ed == VtuberCardEdition::Foil) ? 75 : 55;
                for (int y = 2; y <= 13; ++y) {
                    int x = 3 + ((y + static_cast<int>((seed >> 3) & 3u)) % 8);
                    setPx(s, x, y, {255, 255, 255, a});
                    if (ed == VtuberCardEdition::Foil) {
                        int x2 = 3 + ((x + 3) % 10);
                        if (x2 >= 3 && x2 <= 12) setPx(s, x2, y, {255,255,255,45});
                    }
                }
            }

            // Alt-art: add a tiny starfield pattern.
            if (ed == VtuberCardEdition::AltArt) {
                const bool tw = ((frame + static_cast<int>((seed >> 9) & 31u)) % 8) < 3;
                if (tw) {
                    for (int i = 0; i < 6; ++i) {
                        int x = 3 + ((i * 3 + static_cast<int>(seed & 7u)) % 10);
                        int y = 2 + ((i * 5 + static_cast<int>((seed >> 4) & 7u)) % 10);
                        setPx(s, x, y, {255,255,255,70});
                    }
                }
            }

            // Border (rarity)
            Color border = accent;
            if (rar == VtuberRarity::Common) border = mul(accent, 0.80f);
            if (rar == VtuberRarity::Rare)   border = add(accent, 10, 10, 10);
            if (rar == VtuberRarity::Epic)   border = add(accent, 25, 25, 25);
            if (rar == VtuberRarity::Mythic) border = add(accent, 40, 40, 40);

            // Edition tints
            if (ed == VtuberCardEdition::Foil)   border = add(border, 15, 15, 25);
            if (ed == VtuberCardEdition::Signed) border = add(border, 10, 10, 10);
            if (ed == VtuberCardEdition::Collab) border = add(border, 20, 20, 20);

            // Outer border
            outlineRect(s, 2, 1, 12, 14, border);

            // Collab: split accent along bottom/right edges.
            if (partnerSeed != 0u) {
                Color b2 = accent2;
                if (rar == VtuberRarity::Common) b2 = mul(accent2, 0.80f);
                if (rar == VtuberRarity::Rare)   b2 = add(accent2, 10, 10, 10);
                if (rar == VtuberRarity::Epic)   b2 = add(accent2, 25, 25, 25);
                if (rar == VtuberRarity::Mythic) b2 = add(accent2, 40, 40, 40);
                b2 = add(b2, 20, 20, 20);
                for (int x = 2; x <= 13; ++x) setPx(s, x, 14, b2);
                for (int y = 1; y <= 14; ++y) setPx(s, 13, y, b2);
            }

            // Rare+: double border
            if (rar >= VtuberRarity::Rare) {
                outlineRect(s, 3, 2, 10, 12, mul(border, 0.75f));
            }

            // Epic/Mythic OR Foil: corner sparkles.
            if (rar >= VtuberRarity::Epic || ed == VtuberCardEdition::Foil) {
                const bool twinkle = ((frame + static_cast<int>((seed >> 8) & 31u)) % 8) < 3;
                Color sp = twinkle ? Color{255,255,255,180} : mul(border, 0.85f);
                setPx(s, 2, 1, sp);  setPx(s, 13, 1, sp);
                setPx(s, 2, 14, sp); setPx(s, 13, 14, sp);
            }

            if (rar == VtuberRarity::Mythic || ed == VtuberCardEdition::Foil) {
                // Animated "glint" traveling along the top edge.
                int gx = 3 + ((frame + static_cast<int>((seed >> 16) & 15u)) % 10);
                setPx(s, gx, 1, {255,255,255,220});
                setPx(s, gx + 1, 1, {255,255,255,120});
            }

            // Mini portrait region (top half of inner panel).
            static constexpr Color SKIN[] = {
                {255, 224, 200, 255},
                {245, 210, 180, 255},
                {235, 195, 165, 255},
                {255, 236, 220, 255},
                {225, 185, 155, 255},
            };

            auto drawHead = [&](int cx, int cy, uint32_t sseed, const Color& acc, bool small) {
                RNG rr(vtuberMixSeed(sseed ^ 0xA9B4C2D1u));

                const int skinN = static_cast<int>(sizeof(SKIN) / sizeof(SKIN[0]));
                Color skin = SKIN[rr.range(0, skinN - 1)];
                Color skinShade = mul(skin, 0.88f);

                Color hair = mul(acc, 0.9f);
                // Nudge hair away from accent to avoid monochrome cards.
                hair = add(hair, rr.range(-45, 45), rr.range(-35, 35), rr.range(-45, 45));
                Color hairDark = mul(hair, 0.70f);

                Color eye = add(acc, rr.range(-25, 25), rr.range(-25, 25), rr.range(-25, 25));
                Color eyeDark = mul(eye, 0.70f);

                const int rHair  = small ? 2 : 4;
                const int rHair2 = small ? 1 : 3;
                const int rSkin  = small ? 1 : 3;
                const int rSkin2 = small ? 0 : 2;

                // Hair + head
                circle(s, cx, cy, rHair, hairDark);
                circle(s, cx, cy, rHair2, hair);
                circle(s, cx, cy + 1, rSkin, skin);
                if (!small) {
                    circle(s, cx, cy + 2, rSkin2, skinShade);
                } else {
                    setPx(s, cx, cy + 1, skinShade);
                }

                // Eyes (blink sometimes)
                const bool blink = ((frame + static_cast<int>(sseed & 31u)) % 28) <= 1;
                if (blink) {
                    line(s, cx - 1, cy + 1, cx + 1, cy + 1, mul({40,40,40,255}, 0.8f));
                } else {
                    // For the tiny collab heads, just do 1px eyes.
                    setPx(s, cx - 1, cy + 1, eyeDark);
                    setPx(s, cx + 1, cy + 1, eyeDark);
                    if (!small) {
                        circle(s, cx - 1, cy + 1, 1, eye);
                        circle(s, cx + 1, cy + 1, 1, eye);
                        setPx(s, cx - 1, cy, {255,255,255,150});
                        setPx(s, cx + 1, cy, {255,255,255,150});
                    }
                }

                // Alt-art: small accent star above the portrait.
                if (ed == VtuberCardEdition::AltArt && !small && rr.chance(0.55f)) {
                    setPx(s, cx, cy - 2, add(acc, 40, 40, 40));
                }
            };

            if (partnerSeed != 0u) {
                // Two tiny portraits.
                drawHead(6, 5, seed, accent, true);
                drawHead(10, 5, partnerSeed, accent2, true);
            } else {
                // Single portrait.
                drawHead(8, 5, seed, accent, false);
            }

            // Nameplate / "logo" strip (bottom)
            rect(s, 4, 11, 8, 2, mul(border, 0.45f));

            // Tiny diagonal "sigil" pattern (brighter for foil).
            const float sigMul = (ed == VtuberCardEdition::Foil) ? 0.36f : 0.28f;
            for (int i = 0; i < 6; ++i) {
                int x = 4 + i;
                int y = 11 + (i % 2);
                setPx(s, x, y, mul({255,255,255,255}, sigMul));
            }

            // Signed: scribble autograph in the bottom panel.
            if (ed == VtuberCardEdition::Signed) {
                RNG sr(vtuberMixSeed(seed ^ 0x13579BDFu));
                Color ink = mul(add(border, 30, 30, 30), 0.85f);
                int x = 4 + sr.range(0, 2);
                int y = 12;
                for (int i = 0; i < 7; ++i) {
                    int nx = 4 + sr.range(0, 7);
                    int ny = 11 + sr.range(0, 2);
                    line(s, x, y, nx, ny, ink);
                    x = nx; y = ny;
                }
                // Tiny serial "ticks".
                setPx(s, 11, 13, {255,255,255,90});
                setPx(s, 12, 13, {255,255,255,70});
            }

            // Collab: small 'X' mark on the nameplate.
            if (ed == VtuberCardEdition::Collab) {
                setPx(s, 8, 12, {255,255,255,120});
                setPx(s, 7, 11, {255,255,255,90});
                setPx(s, 9, 11, {255,255,255,90});
                setPx(s, 7, 13, {255,255,255,90});
                setPx(s, 9, 13, {255,255,255,90});
            }

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
        
        case ItemKind::ScrollEnchantRing: {
            Color paper = add({220,210,180,255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            outlineRect(s, 4, 5, 8, 7, mul(paper, 0.85f));
            rect(s, 5, 6, 6, 5, paper);
            // ring-ish glyph
            outlineRect(s, 7, 7, 3, 3, {80,50,30,255});
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

        case ItemKind::CraftingKit: {
            // A compact leather pouch with a little "tool" motif.
            Color leather = add({150, 95, 55, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
            Color leatherDark = mul(leather, 0.72f);
            Color leatherLight = mul(leather, 1.08f);
            Color metal = add({200, 200, 215, 235}, rng.range(-8,8), rng.range(-8,8), rng.range(-8,8));
            Color metalDark = mul(metal, 0.75f);

            // Pouch body
            outlineRect(s, 4, 7, 8, 7, leatherDark);
            rect(s, 5, 8, 6, 5, leather);

            // Flap
            line(s, 4, 7, 11, 7, leatherDark);
            rect(s, 5, 6, 6, 1, leatherLight);

            // Strap + buckle
            line(s, 8, 7, 8, 12, mul(leather, 0.62f));
            rect(s, 7, 9, 2, 2, metalDark);
            rect(s, 7, 10, 2, 1, metal);

            // Tiny hammer glyph (upper-left)
            line(s, 5, 5, 7, 5, metal);
            setPx(s, 6, 6, metalDark);
            setPx(s, 6, 7, metalDark);

            // Subtle glint
            if (frame % 2 == 1) setPx(s, 9, 8, {255,255,255,110});
            break;
        }
        
        case ItemKind::BountyContract: {
            // A parchment contract with a wax seal.
            Color paper = add({230, 220, 185, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color paperDark = mul(paper, 0.75f);
            Color ink = add({40, 35, 30, 255}, rng.range(-6,6), rng.range(-6,6), rng.range(-6,6));
            Color seal = add({170, 45, 55, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
            Color sealDark = mul(seal, 0.70f);
            
            // Scroll body
            outlineRect(s, 4, 5, 8, 10, paperDark);
            rect(s, 5, 6, 6, 8, paper);
            
            // Paper curl highlights
            line(s, 4, 5, 11, 5, paperDark);
            line(s, 4, 14, 11, 14, paperDark);
            
            // Ink lines
            for (int y = 7; y <= 11; y += 2) {
                line(s, 6, y, 10, y, ink);
            }
            
            // Wax seal
            circle(s, 8, 13, 2, sealDark);
            circle(s, 8, 13, 1, seal);
            
            // Tiny glint
            if (frame % 2 == 1) setPx(s, 9, 12, {255,255,255,110});
            break;
        }

        case ItemKind::Chest: {
            Color wood = add({150, 95, 55, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
            Color woodDark = mul(wood, 0.72f);
            Color woodLight = mul(wood, 1.08f);
            Color metal = add({205, 205, 220, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color metalDark = mul(metal, 0.75f);

            // Lid
            outlineRect(s, 3, 4, 10, 4, woodDark);
            rect(s, 4, 5, 8, 2, wood);
            line(s, 4, 5, 11, 5, woodLight);

            // Body
            outlineRect(s, 3, 7, 10, 7, woodDark);
            rect(s, 4, 8, 8, 5, wood);
            // Grain
            for (int x = 4; x <= 11; x += 2) {
                line(s, x, 8, x, 12, mul(wood, 0.90f));
            }

            // Metal bands + latch
            line(s, 5, 7, 5, 13, metalDark);
            line(s, 10, 7, 10, 13, metalDark);
            line(s, 3, 10, 12, 10, metalDark);
            rect(s, 7, 10, 2, 3, metal);
            setPx(s, 8, 12, metalDark);

            // Subtle glint
            if (frame % 2 == 1) setPx(s, 11, 6, {255,255,255,110});
            break;
        }

        case ItemKind::ChestOpen: {
            // An open chest (decorative). Interior is dark with a little treasure sparkle.
            Color wood = add({150, 95, 55, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
            Color woodDark = mul(wood, 0.70f);
            Color interior = {35, 28, 22, 255};
            Color metal = add({205, 205, 220, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color gold = add({235, 200, 70, 255}, rng.range(-8,8), rng.range(-8,8), rng.range(-8,8));

            // Base box
            outlineRect(s, 3, 8, 10, 6, woodDark);
            rect(s, 4, 9, 8, 4, wood);
            rect(s, 4, 9, 8, 2, interior);

            // Lid swung open (simple trapezoid)
            line(s, 3, 8, 12, 4, woodDark);
            line(s, 4, 7, 12, 3, wood);
            line(s, 4, 6, 11, 3, mul(wood, 0.95f));
            // Hinges
            setPx(s, 4, 8, metal);
            setPx(s, 11, 5, metal);

            // A few coins
            setPx(s, 6, 11, gold);
            setPx(s, 8, 11, mul(gold, 0.85f));
            setPx(s, 10, 12, gold);
            if (frame % 2 == 1) setPx(s, 9, 10, {255,255,255,140});
            break;
        }

        default: {
            // Category-based fallback so new items always render something distinct.
            const uint8_t kind8 = static_cast<uint8_t>(kind);
            const uint8_t app = static_cast<uint8_t>(kind8 * 37u + 11u);

            if (isPotionKind(kind)) {
                drawPotionAppearance(s, seed, rng, app, frame);
                break;
            }
            if (isScrollKind(kind)) {
                drawScrollAppearance(s, seed, rng, app, frame);
                break;
            }
            if (isRingKind(kind)) {
                drawRingAppearance(s, seed, rng, app, frame);
                break;
            }
            if (isWandKind(kind)) {
                drawWandAppearance(s, seed, rng, app, frame);
                break;
            }

            if (isSpellbookKind(kind)) {
                Color cover{140, 80, 60, 255};
                Color rune{235, 235, 245, 255};
                switch (kind) {
                    case ItemKind::SpellbookMagicMissile: cover = {70, 120, 230, 255}; rune = {235, 245, 255, 255}; break;
                    case ItemKind::SpellbookBlink:        cover = {155, 95, 235, 255}; rune = {190, 255, 255, 255}; break;
                    case ItemKind::SpellbookMinorHeal:    cover = {200, 70, 80, 255};  rune = {255, 245, 245, 255}; break;
                    case ItemKind::SpellbookDetectTraps:  cover = {165, 120, 80, 255}; rune = {255, 230, 170, 255}; break;
                    case ItemKind::SpellbookFireball:     cover = {185, 55, 55, 255};  rune = {255, 200, 120, 255}; break;
                    case ItemKind::SpellbookStoneskin:    cover = {125, 125, 135, 255}; rune = {235, 225, 205, 255}; break;
                    case ItemKind::SpellbookHaste:        cover = {70, 170, 95, 255};  rune = {255, 255, 190, 255}; break;
                    case ItemKind::SpellbookInvisibility: cover = {90, 90, 130, 255};  rune = {210, 235, 255, 255}; break;
                    case ItemKind::SpellbookPoisonCloud:  cover = {70, 110, 65, 255};  rune = {210, 160, 255, 255}; break;
                    default: break;
                }
                drawSpellbook(cover, rune);
                break;
            }

            if (isCorpseKind(kind)) {
                // Simple lumpy corpse sprite (varies subtly by kind).
                const uint32_t h = hashCombine(seed, 0xC0F1E5u ^ static_cast<uint32_t>(kind8));
                RNG crng(hash32(h));
                Color flesh = add({120, 75, 60, 255}, crng.range(-20,20), crng.range(-20,20), crng.range(-20,20));
                Color dark = mul(flesh, 0.70f);
                circle(s, 8, 11, 4, flesh);
                circle(s, 6, 10, 2, flesh);
                circle(s, 10, 10, 2, flesh);
                // Dark underside
                line(s, 5, 13, 11, 13, dark);
                // Bone hint
                setPx(s, 7, 11, {230,230,230,255});
                setPx(s, 8, 11, {245,245,245,255});
                setPx(s, 9, 11, {230,230,230,255});
                break;
            }

            if (isCaptureSphereKind(kind)) {
                const bool full = isCaptureSphereFullKind(kind);
                const bool mega = (kind == ItemKind::MegaSphere || kind == ItemKind::MegaSphereFull);
                Color shell = mega ? Color{95, 150, 255, 255} : Color{95, 210, 190, 255};
                Color shellDark = mul(shell, 0.70f);
                Color core = mega ? Color{255, 235, 120, 220} : Color{235, 120, 200, 220};
                circle(s, 8, 9, 5, shellDark);
                circle(s, 8, 9, 4, shell);
                // Highlight
                circle(s, 7, 8, 2, mul(shell, 1.08f));
                if (full) {
                    circle(s, 9, 10, 2, core);
                    setPx(s, 9, 10, {255,255,255,120});
                } else {
                    // Hollow center
                    circle(s, 8, 10, 2, {0,0,0,0});
                }
                if (frame % 2 == 1) setPx(s, 11, 7, {255,255,255,110});
                break;
            }

            if (kind == ItemKind::Torch || kind == ItemKind::TorchLit) {
                Color wood = add({140, 95, 55, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
                Color cloth = add({190, 190, 175, 255}, rng.range(-8,8), rng.range(-8,8), rng.range(-8,8));
                // Handle
                line(s, 8, 5, 8, 14, wood);
                line(s, 7, 6, 7, 13, mul(wood, 0.85f));
                // Wrap
                rect(s, 7, 5, 3, 2, cloth);
                if (kind == ItemKind::TorchLit) {
                    Color flame = add({255, 170, 60, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
                    Color flameHi = {255, 245, 190, 220};
                    circle(s, 8, 3, 2, flame);
                    setPx(s, 8, 2, flameHi);
                    if (frame % 2 == 1) setPx(s, 9, 3, {255,255,255,130});
                } else {
                    // Unlit head
                    setPx(s, 8, 3, mul(cloth, 0.75f));
                }
                break;
            }

            if (kind == ItemKind::FishingRod) {
                Color wood = add({150, 100, 55, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
                Color lineC = {220, 220, 235, 180};
                line(s, 4, 13, 11, 4, wood);
                line(s, 5, 13, 11, 5, mul(wood, 0.82f));
                // Fishing line + hook
                line(s, 11, 4, 12, 8, lineC);
                setPx(s, 12, 9, {200,200,210,220});
                break;
            }
            if (kind == ItemKind::Fish) {
                Color body = add({120, 170, 205, 255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
                Color dark = mul(body, 0.75f);
                // Body oval
                circle(s, 8, 10, 3, body);
                rect(s, 6, 9, 6, 3, body);
                // Tail
                setPx(s, 5, 10, dark);
                setPx(s, 4, 9, dark);
                setPx(s, 4, 11, dark);
                // Eye
                setPx(s, 10, 9, {30,30,35,255});
                if (frame % 2 == 1) setPx(s, 9, 8, {255,255,255,110});
                break;
            }

            if (kind == ItemKind::GardenHoe) {
                Color wood = add({140, 95, 55, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
                Color metal = add({205, 205, 220, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
                // Handle
                line(s, 7, 4, 9, 14, wood);
                // Blade
                rect(s, 10, 8, 3, 2, metal);
                rect(s, 9, 9, 4, 1, mul(metal, 0.85f));
                break;
            }
            if (kind == ItemKind::Seed) {
                Color seedC = add({200, 175, 120, 255}, rng.range(-15,15), rng.range(-15,15), rng.range(-15,15));
                // A few kernels
                setPx(s, 7, 10, seedC);
                setPx(s, 9, 11, mul(seedC, 0.90f));
                setPx(s, 8, 12, mul(seedC, 0.85f));
                setPx(s, 10, 10, seedC);
                break;
            }
            if (kind == ItemKind::TilledSoil) {
                Color soil = add({110, 80, 55, 255}, rng.range(-12,12), rng.range(-12,12), rng.range(-12,12));
                Color soilDark = mul(soil, 0.75f);
                outlineRect(s, 4, 8, 8, 6, soilDark);
                rect(s, 5, 9, 6, 4, soil);
                // Furrows
                for (int x = 6; x <= 10; x += 2) {
                    line(s, x, 9, x, 12, soilDark);
                }
                break;
            }
            if (kind == ItemKind::CropSprout || kind == ItemKind::CropGrowing || kind == ItemKind::CropMature || kind == ItemKind::CropProduce) {
                Color leaf = add({85, 200, 110, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
                Color stem = mul(leaf, 0.75f);
                Color fruit = add({235, 120, 80, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
                // Stem
                line(s, 8, 12, 8, 8, stem);
                // Leaves / growth stage
                setPx(s, 7, 9, leaf);
                setPx(s, 9, 9, leaf);
                if (kind == ItemKind::CropGrowing || kind == ItemKind::CropMature || kind == ItemKind::CropProduce) {
                    setPx(s, 6, 10, leaf);
                    setPx(s, 10, 10, leaf);
                }
                if (kind == ItemKind::CropMature || kind == ItemKind::CropProduce) {
                    setPx(s, 7, 8, leaf);
                    setPx(s, 9, 8, leaf);
                }
                if (kind == ItemKind::CropProduce) {
                    // Fruit/produce
                    circle(s, 8, 12, 1, fruit);
                    setPx(s, 9, 11, mul(fruit, 0.85f));
                }
                break;
            }

            // Generic mystery token with a hashed rune glyph.
            Color plate = add({120, 125, 135, 255}, rng.range(-10,10), rng.range(-10,10), rng.range(-10,10));
            Color plateEdge = mul(plate, 0.70f);
            outlineRect(s, 4, 5, 8, 8, plateEdge);
            rect(s, 5, 6, 6, 6, plate);
            Color ink = {35, 35, 40, 235};
            const uint32_t hh = hashCombine(seed, 0x51A11Fu ^ static_cast<uint32_t>(kind8));
            uint16_t mask = static_cast<uint16_t>((hash32(hh) >> 1) & 0x7FFFu);
            if (mask == 0) mask = 0x1555u; // avoid blank glyph
            drawRuneGlyph(s, 6, 7, mask, ink);
            if (frame % 2 == 1) setPx(s, 10, 6, {255,255,255,110});
            break;
        }
    }

    finalizeSprite(s, seed, frame, /*outlineAlpha=*/190, /*shadowAlpha=*/70);
    return use3d ? render3d(s)
                 : resampleSpriteToSize(s, pxSize);
}

// -----------------------------------------------------------------------------
// Terrain / UI / overlay sprite generation
//
// These functions are declared in spritegen.hpp and are used by the renderer to
// generate procedural terrain tiles, overlays, UI skins, and projectile sprites.
// The original upstream prototype set expanded over time; some builds referenced
// these symbols without a concrete implementation.
//
// Implementation notes:
//  - We keep a deterministic 16px "design grid" (or 16x8 for isometric diamonds)
//    and resample to the requested pxSize using the generator's Scale2x chain.
//  - Overlays are transparent where appropriate and are designed to layer cleanly
//    on top of the themed floor/wall/chasm base tiles.
// -----------------------------------------------------------------------------

namespace {

inline Color withAlpha(Color c, uint8_t a) {
    c.a = a;
    return c;
}

// Proper alpha-over blend that preserves destination alpha.
void blendOver(SpritePixels& s, int x, int y, Color src) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
    Color dst = s.at(x, y);

    const float sa = src.a / 255.0f;
    const float da = dst.a / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f) {
        s.at(x, y) = {0,0,0,0};
        return;
    }

    auto comp = [&](uint8_t sc, uint8_t dc) -> uint8_t {
        const float out = (sc * sa + dc * da * (1.0f - sa)) / oa;
        return clamp8(static_cast<int>(std::lround(out)));
    };

    Color out;
    out.r = comp(src.r, dst.r);
    out.g = comp(src.g, dst.g);
    out.b = comp(src.b, dst.b);
    out.a = clamp8(static_cast<int>(std::lround(oa * 255.0f)));
    s.at(x, y) = out;
}

inline float noise01(uint32_t seed, int x, int y) {
    const uint32_t h = hashCombine(seed, hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
    return rand01(h);
}

inline int clampiLocal(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Rect resampling (used for isometric diamond tiles where h != w).
SpritePixels resampleSpriteToRectInternal(const SpritePixels& src, int outW, int outH) {
    outW = std::clamp(outW, 1, 512);
    outH = std::clamp(outH, 1, 512);

    if (src.w == outW && src.h == outH) return src;

    // Fast path: Scale2x chain when both dimensions share the same power-of-two factor.
    if (src.w > 0 && src.h > 0) {
        const int wFactor = (outW % src.w == 0) ? (outW / src.w) : 0;
        const int hFactor = (outH % src.h == 0) ? (outH / src.h) : 0;
        if (wFactor > 0 && wFactor == hFactor && isPow2(wFactor)) {
            SpritePixels cur = src;
            while (cur.w < outW && cur.h < outH) {
                cur = scale2x(cur);
            }
            if (cur.w == outW && cur.h == outH) return cur;
        }
    }

    return resizeNearest(src, outW, outH);
}

// Convert an (x,y) in a 2:1 isometric diamond (width=N, height=N/2) into
// (u,v) coordinates in the corresponding top-down square space (size=N).
// Returns false if (x,y) lies outside the diamond.
inline bool isoDiamondToSquareUV(int N, int x, int y, int& u, int& v) {
    if (N <= 0) return false;
    const int H = std::max(1, N / 2);
    if (x < 0 || y < 0 || x >= N || y >= H) return false;

    const int half = N / 2;
    const int uu = x + 2 * y - half;
    const int vv = 2 * y - x + half;
    if (uu < 0 || vv < 0 || uu >= N || vv >= N) return false;

    u = uu;
    v = vv;
    return true;
}

struct TilePalette {
    Color base;
    Color dark;
    Color light;
    Color accent;
    Color accent2;
    Color grout;
};

TilePalette floorPalette(uint8_t style, uint32_t seed) {
    RNG rng(hashCombine(seed, static_cast<uint32_t>(style) * 131u + 17u));

    TilePalette p{};
    switch (style) {
        default:
        case 0: // Normal
            p.base   = {120, 122, 135, 255};
            p.dark   = { 78,  80,  92, 255};
            p.light  = {160, 164, 176, 255};
            p.accent = {110, 140, 155, 255};
            p.accent2= {175, 175, 185, 255};
            p.grout  = { 64,  66,  74, 255};
            break;
        case 1: // Treasure
            p.base   = {140, 128,  84, 255};
            p.dark   = { 88,  76,  48, 255};
            p.light  = {196, 172, 102, 255};
            p.accent = {210, 180,  64, 255};
            p.accent2= {255, 245, 200, 255};
            p.grout  = { 74,  60,  40, 255};
            break;
        case 2: // Lair
            p.base   = {110, 118,  86, 255};
            p.dark   = { 68,  78,  54, 255};
            p.light  = {150, 160, 112, 255};
            p.accent = { 88, 150,  88, 255};
            p.accent2= {180, 210, 160, 255};
            p.grout  = { 52,  56,  40, 255};
            break;
        case 3: // Shrine
            p.base   = {118, 122, 148, 255};
            p.dark   = { 70,  72, 100, 255};
            p.light  = {170, 178, 220, 255};
            p.accent = {190, 150, 255, 255};
            p.accent2= {240, 230, 255, 255};
            p.grout  = { 58,  60,  78, 255};
            break;
        case 4: // Secret
            p.base   = { 96,  98, 112, 255};
            p.dark   = { 50,  52,  64, 255};
            p.light  = {132, 136, 150, 255};
            p.accent = {120, 110,  90, 255};
            p.accent2= {180, 170, 140, 255};
            p.grout  = { 42,  44,  52, 255};
            break;
        case 5: // Vault
            p.base   = {110, 114, 122, 255};
            p.dark   = { 56,  58,  66, 255};
            p.light  = {180, 186, 198, 255};
            p.accent = {150, 170, 210, 255};
            p.accent2= {230, 240, 255, 255};
            p.grout  = { 40,  42,  48, 255};
            break;
        case 6: // Shop
            p.base   = {140, 110,  78, 255};
            p.dark   = { 88,  64,  42, 255};
            p.light  = {190, 150, 110, 255};
            p.accent = {170, 120,  80, 255};
            p.accent2= {220, 190, 150, 255};
            p.grout  = { 70,  52,  34, 255};
            break;
    }

    // Tiny per-run hue/value wiggle so different seeds feel like different "paint jobs".
    const int wig = rng.range(-6, 6);
    p.base   = add(p.base,   wig, wig, wig);
    p.dark   = add(p.dark,   wig, wig, wig);
    p.light  = add(p.light,  wig, wig, wig);
    p.grout  = add(p.grout,  wig, wig, wig);

    return p;
}

TilePalette wallPalette(uint32_t seed) {
    RNG rng(hashCombine(seed, 0xA11A11u));
    TilePalette p{};
    p.base  = {118, 120, 134, 255};
    p.dark  = { 62,  64,  76, 255};
    p.light = {168, 172, 188, 255};
    p.accent= {126, 150, 168, 255};
    p.accent2={210, 210, 220, 255};
    p.grout = { 46,  48,  58, 255};

    const int wig = rng.range(-6, 6);
    p.base  = add(p.base, wig, wig, wig);
    p.dark  = add(p.dark, wig, wig, wig);
    p.light = add(p.light, wig, wig, wig);
    p.grout = add(p.grout, wig, wig, wig);
    return p;
}

// --- Tiny raster helpers for blocky iso geometry at 16px --------------------

inline int edgeFn(int ax, int ay, int bx, int by, int px, int py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

void fillTri(SpritePixels& s, int x0, int y0, int x1, int y1, int x2, int y2, Color c) {
    int minX = std::min({x0, x1, x2});
    int maxX = std::max({x0, x1, x2});
    int minY = std::min({y0, y1, y2});
    int maxY = std::max({y0, y1, y2});

    minX = clampiLocal(minX, 0, s.w - 1);
    maxX = clampiLocal(maxX, 0, s.w - 1);
    minY = clampiLocal(minY, 0, s.h - 1);
    maxY = clampiLocal(maxY, 0, s.h - 1);

    const int area = edgeFn(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const int w0 = edgeFn(x1, y1, x2, y2, x, y);
            const int w1 = edgeFn(x2, y2, x0, y0, x, y);
            const int w2 = edgeFn(x0, y0, x1, y1, x, y);

            if ((area > 0 && w0 >= 0 && w1 >= 0 && w2 >= 0) ||
                (area < 0 && w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                setPx(s, x, y, c);
            }
        }
    }
}

void fillQuad(SpritePixels& s,
              int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3,
              Color c) {
    // Split quad into two triangles.
    fillTri(s, x0, y0, x1, y1, x2, y2, c);
    fillTri(s, x0, y0, x2, y2, x3, y3, c);
}

// --- Base (16px) tile generators -------------------------------------------

SpritePixels genFloor16(uint32_t seed, uint8_t style, int frame) {
    (void)frame;
    const TilePalette p = floorPalette(style, seed);
    SpritePixels s = makeSprite(16, 16, p.base);

    RNG rng(hashCombine(seed, 0xF100Au));
    const int ox = rng.range(0, 3);
    const int oy = rng.range(0, 3);

    // Base noise + subtle slab lines.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float n = noise01(seed ^ 0xA11CEu, x, y);
            const int v = static_cast<int>(std::lround((n - 0.5f) * 12.0f));
            Color c = add(p.base, v, v, v);

            // Grout lines every ~4px, offset per seed.
            const bool grout = (((x + ox) % 4) == 0) || (((y + oy) % 4) == 0);
            if (grout && n > 0.10f) {
                c = add(p.grout, v / 2, v / 2, v / 2);
            }

            // Style accents.
            if (style == 2) { // Lair: moss patches
                if (noise01(seed ^ 0xB055u, x / 2, y / 2) > 0.72f) {
                    c = lerp(c, p.accent, 0.45f);
                }
            } else if (style == 6) { // Shop: plank-ish streaks
                if (((y + oy) % 3) == 0) c = lerp(c, p.dark, 0.25f);
            } else if (style == 5) { // Vault: cold inlay lines
                if (((x + ox) % 5) == 0 && ((y + oy) % 5) != 0) c = lerp(c, p.accent, 0.35f);
            } else if (style == 1) { // Treasure: occasional sparkles
                const bool sparkle = ((hash32(hashCombine(seed, hashCombine(x, y))) & 31u) == 0u);
                if (sparkle) {
                    const int tw = ((frame + ((x + y) & 3)) & 3);
                    c = (tw == 0) ? p.accent2 : p.light;
                }
            } else if (style == 4) { // Secret: hairline cracks
                if ((hash32(hashCombine(seed ^ 0xC1ACu, hashCombine(x, y))) & 63u) == 0u) {
                    c = lerp(c, p.dark, 0.55f);
                }
            }

            setPx(s, x, y, c);
        }
    }

    if (style == 3) {
        // Shrine: central rune mark (subtle, so it still tiles).
        const uint16_t rune = static_cast<uint16_t>(0b01010'11111'01010);
        drawRuneGlyph(s, 5, 5, rune, withAlpha(p.accent, 220));
    }

    return s;
}

SpritePixels genWall16(uint32_t seed, int frame) {
    (void)frame;
    const TilePalette p = wallPalette(seed);
    SpritePixels s = makeSprite(16, 16, p.base);

    const int brickH = 3;
    const int brickW = 5;

    for (int y = 0; y < 16; ++y) {
        const int row = y / (brickH + 1);
        const bool mortarY = (y % (brickH + 1)) == 0;
        const int xOff = (row & 1) ? ((brickW + 1) / 2) : 0;

        for (int x = 0; x < 16; ++x) {
            const bool mortarX = ((x + xOff) % (brickW + 1)) == 0;
            if (mortarY || mortarX) {
                setPx(s, x, y, p.grout);
                continue;
            }

            const int col = (x + xOff) / (brickW + 1);
            const uint32_t bh = hashCombine(seed, hashCombine(static_cast<uint32_t>(row), static_cast<uint32_t>(col)));
            const float n = rand01(bh);
            Color bc = p.base;
            if (n < 0.33f) bc = lerp(bc, p.dark, 0.25f);
            else if (n > 0.77f) bc = lerp(bc, p.light, 0.20f);

            // Per-pixel grit.
            const float g = noise01(seed ^ 0x5170AEu, x, y);
            if (g > 0.92f) bc = lerp(bc, p.accent2, 0.35f);

            setPx(s, x, y, bc);
        }
    }

    return s;
}

SpritePixels genChasm16(uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {8, 10, 14, 255});

    const int cx = 8;
    const int cy = 8;

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            const float d = std::sqrt(static_cast<float>(dx*dx + dy*dy));

            // Darker center, slightly lighter near rim.
            float t = std::clamp(d / 9.0f, 0.0f, 1.0f);
            Color c = lerp(Color{4, 6, 10, 255}, Color{18, 20, 26, 255}, t);

            // Animated void specks.
            const float n = noise01(seed ^ (0xB00Bu + static_cast<uint32_t>(frame) * 911u), x, y);
            if (n > 0.93f) {
                c = add(c, 18, 18, 26);
            } else if (n < 0.05f) {
                c = add(c, -8, -8, -8);
            }

            setPx(s, x, y, c);
        }
    }

    return s;
}

// Generate a 16x8 isometric diamond tile directly in diamond space by reusing
// the same (u,v) square-coordinate material function.
SpritePixels genIsoFloor16(uint32_t seed, uint8_t style, int frame) {
    SpritePixels out = makeSprite(16, 8, {0,0,0,0});
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            int u=0, v=0;
            if (!isoDiamondToSquareUV(16, x, y, u, v)) continue;

            // Reuse square floor generation by sampling its deterministic pixel.
            // Generate on demand rather than allocating a temp 16x16.
            const TilePalette p = floorPalette(style, seed);
            const float n = noise01(seed ^ 0xA11CEu, u, v);
            const int dv = static_cast<int>(std::lround((n - 0.5f) * 12.0f));
            Color c = add(p.base, dv, dv, dv);

            // Grout lines: keep aligned to square grid for a coherent iso read.
            const int ox = static_cast<int>(hash32(seed) & 3u);
            const int oy = static_cast<int>((hash32(seed ^ 0x1234u) >> 8) & 3u);
            const bool grout = (((u + ox) % 4) == 0) || (((v + oy) % 4) == 0);
            if (grout && n > 0.10f) c = add(p.grout, dv / 2, dv / 2, dv / 2);

            // Style accents mirrored from genFloor16().
            if (style == 2) {
                if (noise01(seed ^ 0xB055u, u / 2, v / 2) > 0.72f) c = lerp(c, p.accent, 0.45f);
            } else if (style == 6) {
                if (((v + oy) % 3) == 0) c = lerp(c, p.dark, 0.25f);
            } else if (style == 5) {
                if (((u + ox) % 5) == 0 && ((v + oy) % 5) != 0) c = lerp(c, p.accent, 0.35f);
            } else if (style == 1) {
                const bool sparkle = ((hash32(hashCombine(seed, hashCombine(u, v))) & 31u) == 0u);
                if (sparkle) {
                    const int tw = ((frame + ((u + v) & 3)) & 3);
                    c = (tw == 0) ? p.accent2 : p.light;
                }
            } else if (style == 4) {
                if ((hash32(hashCombine(seed ^ 0xC1ACu, hashCombine(u, v))) & 63u) == 0u) {
                    c = lerp(c, p.dark, 0.55f);
                }
            }

            // Slight edge darken to read as a diamond without looking like a hard grid.
            const int edgeD = std::min({u, v, 15 - u, 15 - v});
            if (edgeD <= 1) c = lerp(c, p.dark, 0.18f);

            setPx(out, x, y, c);
        }
    }

    if (style == 3) {
        // Shrine rune: project location so it lands near tile center.
        const uint16_t rune = static_cast<uint16_t>(0b01010'11111'01010);
        // Draw a tiny rune in the center area of the diamond.
        for (int ry = 0; ry < 5; ++ry) {
            for (int rx = 0; rx < 5; ++rx) {
                const bool on = (rune >> (ry * 5 + rx)) & 1u;
                if (!on) continue;
                const int px = 6 + rx;
                const int py = 2 + ry;
                if (px >= 0 && px < 16 && py >= 0 && py < 8) {
                    // Only draw if this pixel is inside the diamond.
                    int u=0, v=0;
                    if (isoDiamondToSquareUV(16, px, py, u, v)) {
                        setPx(out, px, py, withAlpha(floorPalette(style, seed).accent, 220));
                    }
                }
            }
        }
    }

    return out;
}

SpritePixels genIsoChasm16(uint32_t seed, int frame) {
    SpritePixels out = makeSprite(16, 8, {0,0,0,0});

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            int u=0, v=0;
            if (!isoDiamondToSquareUV(16, x, y, u, v)) continue;

            const int edgeD = std::min({u, v, 15 - u, 15 - v});
            const float rimT = std::clamp(edgeD / 5.0f, 0.0f, 1.0f);

            // Rim slightly lighter, interior darker.
            Color c = lerp(Color{10, 12, 18, 255}, Color{2, 3, 6, 255}, 1.0f - rimT);

            const float n = noise01(seed ^ (0xB00Bu + static_cast<uint32_t>(frame) * 911u), u, v);
            if (n > 0.94f) c = add(c, 18, 18, 28);

            setPx(out, x, y, c);
        }
    }

    return out;
}

SpritePixels genIsoMaskOverlay16(uint32_t seed, uint8_t mask, int frame, int thickness, uint8_t alphaMax) {
    (void)seed;
    SpritePixels out = makeSprite(16, 8, {0,0,0,0});

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            int u=0, v=0;
            if (!isoDiamondToSquareUV(16, x, y, u, v)) continue;

            float a = 0.0f;

            if (mask & 1u) { // N: NE edge => v=0
                const int d = v;
                a = std::max(a, std::clamp(1.0f - (static_cast<float>(d) / static_cast<float>(thickness)), 0.0f, 1.0f));
            }
            if (mask & 2u) { // E: SE edge => u=15
                const int d = 15 - u;
                a = std::max(a, std::clamp(1.0f - (static_cast<float>(d) / static_cast<float>(thickness)), 0.0f, 1.0f));
            }
            if (mask & 4u) { // S: SW edge => v=15
                const int d = 15 - v;
                a = std::max(a, std::clamp(1.0f - (static_cast<float>(d) / static_cast<float>(thickness)), 0.0f, 1.0f));
            }
            if (mask & 8u) { // W: NW edge => u=0
                const int d = u;
                a = std::max(a, std::clamp(1.0f - (static_cast<float>(d) / static_cast<float>(thickness)), 0.0f, 1.0f));
            }

            // Subtle animated dither so large tiles don't look like flat alpha planes.
            const float dither = bayer4Threshold(x + frame, y + frame);
            a = std::clamp(a - (dither - 0.5f) * 0.08f, 0.0f, 1.0f);

            if (a <= 0.001f) continue;
            setPx(out, x, y, {0, 0, 0, clamp8(static_cast<int>(std::lround(alphaMax * a)))});
        }
    }

    return out;
}

// Simple isometric entity ground shadow on 16x8 diamond.
SpritePixels genIsoEntityShadow16(uint32_t seed, uint8_t lightDir, int frame) {
    (void)seed;
    (void)frame;
    SpritePixels out = makeSprite(16, 8, {0,0,0,0});

    // Shadow offset is opposite light direction.
    int dx = 0, dy = 0;
    switch (lightDir & 3u) {
        default:
        case 0u: dx = +1; dy = +1; break; // light NW => shadow SE
        case 1u: dx = -1; dy = +1; break; // light NE => shadow SW
        case 2u: dx = -1; dy = -1; break; // light SE => shadow NW
        case 3u: dx = +1; dy = -1; break; // light SW => shadow NE
    }

    const int cx = 8 + dx;
    const int cy = 4 + dy;

    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            int u=0, v=0;
            if (!isoDiamondToSquareUV(16, x, y, u, v)) continue;

            // Diamond-ish distance.
            const int dd = std::abs(x - cx) + 2 * std::abs(y - cy);
            const int r0 = 6;
            if (dd > r0) continue;

            float a = (static_cast<float>(r0 - dd) / static_cast<float>(r0));
            a = a * a;
            const uint8_t A = clamp8(static_cast<int>(std::lround(140.0f * a)));
            if (A == 0) continue;

            // Slight warm tint to avoid pure black.
            setPx(out, x, y, {6, 4, 2, A});
        }
    }

    return out;
}

// Isometric floor decals: reuse the same square-space decal logic as top-down.
SpritePixels genFloorDecal16(uint32_t seed, uint8_t style, int frame) {
    (void)style;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    RNG rng(hashCombine(seed, 0xDECALu + static_cast<uint32_t>(style) * 17u));

    // Choose a simple motif per style.
    switch (style) {
        default:
        case 0: { // cracks
            for (int i = 0; i < 2; ++i) {
                int x0 = rng.range(2, 13);
                int y0 = rng.range(2, 13);
                int x1 = clampiLocal(x0 + rng.range(-6, 6), 1, 14);
                int y1 = clampiLocal(y0 + rng.range(-6, 6), 1, 14);
                line(s, x0, y0, x1, y1, withAlpha(Color{40, 42, 50, 255}, 160));
            }
            break;
        }
        case 1: { // treasure: scattered coins
            const Color coin = {210, 180, 64, 220};
            for (int i = 0; i < 4; ++i) {
                int x = rng.range(3, 12);
                int y = rng.range(3, 12);
                circle(s, x, y, 1, coin);
                if (((frame + i) & 3) == 0) setPx(s, x, y, {255, 245, 200, 255});
            }
            break;
        }
        case 2: { // lair: slime blot
            const Color slime = {90, 160, 90, 150};
            for (int y = 3; y < 13; ++y) {
                for (int x = 3; x < 13; ++x) {
                    const float n = noise01(seed ^ 0x511AEu, x, y);
                    if (n > 0.78f) blendOver(s, x, y, slime);
                }
            }
            break;
        }
        case 3: { // shrine: rune decal
            const TilePalette p = floorPalette(style, seed);
            const uint16_t rune = static_cast<uint16_t>(0b00100'01110'00100);
            drawRuneGlyph(s, 6, 6, rune, withAlpha(p.accent, 210));
            break;
        }
        case 4: { // secret: scratch marks
            const Color ink = {80, 70, 60, 160};
            for (int i = 0; i < 3; ++i) {
                int y = rng.range(4, 12);
                line(s, 4, y, 12, y + rng.range(-1, 1), ink);
            }
            break;
        }
        case 5: { // vault: geometric inlay
            const Color ink = {140, 170, 210, 160};
            outlineRect(s, 4, 4, 8, 8, ink);
            break;
        }
        case 6: { // shop: footprints
            const Color fp = {60, 40, 30, 120};
            for (int i = 0; i < 3; ++i) {
                const int x = 6 + i * 2;
                const int y = 10 - i * 2;
                setPx(s, x, y, fp);
                setPx(s, x + 1, y + 1, fp);
            }
            break;
        }
    }

    return s;
}

SpritePixels genWallDecal16(uint32_t seed, uint8_t style, int frame) {
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hashCombine(seed, 0xDECA1u + static_cast<uint32_t>(style) * 19u));

    switch (style) {
        default:
        case 0: { // grime streak
            const Color grime = {30, 30, 34, 120};
            for (int y = 2; y < 15; ++y) {
                if (noise01(seed ^ 0x6B1EEFu, 1, y) > 0.6f) {
                    int x = rng.range(2, 13);
                    blendOver(s, x, y, grime);
                    blendOver(s, x, y + 1, grime);
                }
            }
            break;
        }
        case 1: { // gild flecks
            const Color g = {210, 180, 64, 120};
            for (int i = 0; i < 8; ++i) {
                int x = rng.range(2, 13);
                int y = rng.range(2, 13);
                setPx(s, x, y, g);
            }
            break;
        }
        case 2: { // claw marks
            const Color ink = {50, 50, 50, 180};
            for (int i = 0; i < 3; ++i) {
                int x0 = 4 + i * 3;
                line(s, x0, 3, x0 - 2, 12, ink);
            }
            break;
        }
        case 3: { // shrine sigil
            const Color ink = {190, 150, 255, 190};
            circle(s, 8, 8, 4, withAlpha(ink, 90));
            outlineRect(s, 6, 6, 5, 5, ink);
            break;
        }
        case 4: { // secret: hairline crack
            const Color ink = {30, 30, 34, 140};
            line(s, 2, 10, 13, 5, ink);
            break;
        }
        case 5: { // vault: riveted plate
            const Color plate = {140, 150, 170, 90};
            rect(s, 4, 4, 8, 8, plate);
            for (int i = 0; i < 4; ++i) {
                setPx(s, 5 + (i % 2) * 6, 5 + (i / 2) * 6, {220, 230, 245, 150});
            }
            break;
        }
        case 6: { // shop: poster
            rect(s, 5, 4, 6, 8, {220, 210, 190, 120});
            outlineRect(s, 5, 4, 6, 8, {120, 90, 60, 160});
            break;
        }
    }

    return s;
}


// Themed floor border overlay: subtle accent lines drawn *inside* a room's floor tiles
// to visually frame special rooms and mark style transitions.
//
// mask bits: 1=N, 2=E, 4=S, 8=W (bit set means "draw border on that edge")
SpritePixels genFloorBorderOverlay16(uint32_t seed, uint8_t style, uint8_t mask, int variant, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    if (style == 0u || mask == 0u) return s;

    const TilePalette p = floorPalette(style, seed ^ "BORDER"_tag);

    int thick = 2;
    int inset = 1; // draw inside the tile so borders feel like a room "trim", not a seam crack

    Color lineCol = withAlpha(p.accent, 175);
    Color glowCol = withAlpha(p.accent2, 90);
    Color shadeCol = withAlpha(p.dark, 110);
    Color studCol = withAlpha(p.accent2, 180);

    switch (style) {
        default:
        case 1: // Treasure: brighter, gilded
            lineCol = withAlpha(p.accent, 200);
            glowCol = withAlpha(p.accent2, 120);
            studCol = withAlpha(p.accent2, 220);
            break;
        case 2: // Lair: slimy, softer edge
            lineCol = withAlpha(p.accent, 150);
            glowCol = withAlpha(p.accent2, 70);
            shadeCol = withAlpha(p.dark, 90);
            thick = 2;
            break;
        case 3: // Shrine: runic glow
            lineCol = withAlpha(p.accent, 165 + (frame & 1) * 18);
            glowCol = withAlpha(p.accent2, 95 + (frame & 1) * 18);
            studCol = withAlpha(p.accent2, 210);
            break;
        case 4: // Secret: subtle, dusty
            lineCol = withAlpha(p.accent, 115);
            glowCol = withAlpha(p.accent2, 55);
            shadeCol = withAlpha(p.dark, 120);
            thick = 1;
            break;
        case 5: // Vault: crisp metal inlay
            lineCol = withAlpha(p.accent, 180);
            glowCol = withAlpha(p.accent2, 90);
            studCol = withAlpha(p.accent2, 200);
            break;
        case 6: // Shop: warm trim
            lineCol = withAlpha(p.accent, 170);
            glowCol = withAlpha(p.accent2, 80);
            studCol = withAlpha(p.accent2, 190);
            break;
    }

    auto ditherKeep = [&](int x, int y, float strength) -> bool {
        // strength in [0..1]
        const float b = bayer4Threshold(x + static_cast<int>(seed), y + static_cast<int>(seed));
        return b < strength;
    };

    auto drawH = [&](int y) {
        for (int x = 0; x < 16; ++x) {
            // Slightly broken line so it doesn't look like a perfectly printed UI stroke.
            float keep = 0.92f;
            if (style == 4) keep = 0.75f;
            if (style == 2) keep = 0.86f;
            if (!ditherKeep(x, y, keep)) continue;

            setPx(s, x, y, lineCol);
            if (thick >= 2 && y + 1 < 16) setPx(s, x, y + 1, shadeCol);
            if (thick >= 3 && y + 2 < 16) setPx(s, x, y + 2, withAlpha(shadeCol, 70));
        }
    };

    auto drawHBottom = [&](int y) {
        for (int x = 0; x < 16; ++x) {
            float keep = 0.92f;
            if (style == 4) keep = 0.75f;
            if (style == 2) keep = 0.86f;
            if (!ditherKeep(x, y, keep)) continue;

            setPx(s, x, y, lineCol);
            if (thick >= 2 && y - 1 >= 0) setPx(s, x, y - 1, shadeCol);
            if (thick >= 3 && y - 2 >= 0) setPx(s, x, y - 2, withAlpha(shadeCol, 70));
        }
    };

    auto drawV = [&](int x) {
        for (int y = 0; y < 16; ++y) {
            float keep = 0.92f;
            if (style == 4) keep = 0.75f;
            if (style == 2) keep = 0.86f;
            if (!ditherKeep(x, y, keep)) continue;

            setPx(s, x, y, lineCol);
            if (thick >= 2 && x + 1 < 16) setPx(s, x + 1, y, shadeCol);
            if (thick >= 3 && x + 2 < 16) setPx(s, x + 2, y, withAlpha(shadeCol, 70));
        }
    };

    auto drawVRight = [&](int x) {
        for (int y = 0; y < 16; ++y) {
            float keep = 0.92f;
            if (style == 4) keep = 0.75f;
            if (style == 2) keep = 0.86f;
            if (!ditherKeep(x, y, keep)) continue;

            setPx(s, x, y, lineCol);
            if (thick >= 2 && x - 1 >= 0) setPx(s, x - 1, y, shadeCol);
            if (thick >= 3 && x - 2 >= 0) setPx(s, x - 2, y, withAlpha(shadeCol, 70));
        }
    };

    // Draw edges (inside the tile).
    if (mask & 1u) drawH(inset);               // N
    if (mask & 4u) drawHBottom(15 - inset);    // S
    if (mask & 8u) drawV(inset);               // W
    if (mask & 2u) drawVRight(15 - inset);     // E

    // Corner accents help readability on small tile sizes.
    auto cornerDot = [&](int cx, int cy) {
        // Tiny 2x2 glow
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const int x = clampiLocal(cx + dx, 0, 15);
                const int y = clampiLocal(cy + dy, 0, 15);
                blendOver(s, x, y, glowCol);
            }
        }
    };

    if ((mask & 1u) && (mask & 8u)) cornerDot(inset, inset);
    if ((mask & 1u) && (mask & 2u)) cornerDot(15 - inset - 1, inset);
    if ((mask & 4u) && (mask & 8u)) cornerDot(inset, 15 - inset - 1);
    if ((mask & 4u) && (mask & 2u)) cornerDot(15 - inset - 1, 15 - inset - 1);

    // Style-specific embellishments.
    switch (style) {
        case 1: { // Treasure: animated sparkles along the border
            const int phase = (frame + variant) & 3;
            auto sparkleH = [&](int y) {
                for (int x = 2; x < 14; ++x) {
                    if (((x + y) & 3) != phase) continue;
                    if (noise01(seed ^ 0x51A1u, x, y) > 0.74f) setPx(s, x, y, studCol);
                }
            };
            auto sparkleV = [&](int x) {
                for (int y = 2; y < 14; ++y) {
                    if (((x + y) & 3) != phase) continue;
                    if (noise01(seed ^ 0x51A2u, x, y) > 0.74f) setPx(s, x, y, studCol);
                }
            };
            if (mask & 1u) sparkleH(inset);
            if (mask & 4u) sparkleH(15 - inset);
            if (mask & 8u) sparkleV(inset);
            if (mask & 2u) sparkleV(15 - inset);
            break;
        }
        case 2: { // Lair: occasional slime drips inward
            auto dripH = [&](int y, int dir) {
                for (int x = 2; x < 14; ++x) {
                    const float n = noise01(seed ^ 0x511AEu, x + variant * 7, y);
                    if (n > 0.83f) {
                        blendOver(s, x, y + dir, withAlpha(lineCol, 120));
                        if (n > 0.90f) blendOver(s, x, y + 2 * dir, withAlpha(lineCol, 90));
                    }
                }
            };
            auto dripV = [&](int x, int dir) {
                for (int y = 2; y < 14; ++y) {
                    const float n = noise01(seed ^ 0x511AFu, x, y + variant * 7);
                    if (n > 0.83f) {
                        blendOver(s, x + dir, y, withAlpha(lineCol, 120));
                        if (n > 0.90f) blendOver(s, x + 2 * dir, y, withAlpha(lineCol, 90));
                    }
                }
            };
            if (mask & 1u) dripH(inset, +1);
            if (mask & 4u) dripH(15 - inset, -1);
            if (mask & 8u) dripV(inset, +1);
            if (mask & 2u) dripV(15 - inset, -1);
            break;
        }
        case 3: { // Shrine: rune ticks (pulsing)
            const uint8_t a = static_cast<uint8_t>(150 + ((frame + variant) & 1) * 40);
            const Color rune = withAlpha(p.accent2, a);
            auto ticksH = [&](int y, int dir) {
                for (int x = 2; x < 14; x += 3) {
                    if (noise01(seed ^ 0x5EEDu, x, y) > 0.55f) {
                        setPx(s, x, y + dir, rune);
                        if (noise01(seed ^ 0x5EEDu, x + 1, y) > 0.80f) setPx(s, x + 1, y + dir, rune);
                    }
                }
            };
            auto ticksV = [&](int x, int dir) {
                for (int y = 2; y < 14; y += 3) {
                    if (noise01(seed ^ 0x5EEE0u, x, y) > 0.55f) {
                        setPx(s, x + dir, y, rune);
                        if (noise01(seed ^ 0x5EEE0u, x, y + 1) > 0.80f) setPx(s, x + dir, y + 1, rune);
                    }
                }
            };
            if (mask & 1u) ticksH(inset, +1);
            if (mask & 4u) ticksH(15 - inset, -1);
            if (mask & 8u) ticksV(inset, +1);
            if (mask & 2u) ticksV(15 - inset, -1);
            break;
        }
        case 5: { // Vault: rivet studs
            auto studsH = [&](int y) {
                for (int x = 3; x < 13; x += 5) {
                    if (noise01(seed ^ 0xA11Cu, x, y) > 0.45f) setPx(s, x, y, studCol);
                }
            };
            auto studsV = [&](int x) {
                for (int y = 3; y < 13; y += 5) {
                    if (noise01(seed ^ 0xA11Du, x, y) > 0.45f) setPx(s, x, y, studCol);
                }
            };
            if (mask & 1u) studsH(inset);
            if (mask & 4u) studsH(15 - inset);
            if (mask & 8u) studsV(inset);
            if (mask & 2u) studsV(15 - inset);
            break;
        }
        case 6: { // Shop: stitched trim pattern
            auto stitchH = [&](int y) {
                for (int x = 2; x < 14; ++x) {
                    if (((x + variant + frame) & 1) == 0) continue;
                    blendOver(s, x, y, glowCol);
                }
            };
            auto stitchV = [&](int x) {
                for (int y = 2; y < 14; ++y) {
                    if (((y + variant + frame) & 1) == 0) continue;
                    blendOver(s, x, y, glowCol);
                }
            };
            if (mask & 1u) stitchH(inset);
            if (mask & 4u) stitchH(15 - inset);
            if (mask & 8u) stitchV(inset);
            if (mask & 2u) stitchV(15 - inset);
            break;
        }
        default:
            break;
    }

    return s;
}


// Autotile overlay: simple edge highlight. Generated at 16x16.
SpritePixels genWallEdgeOverlay16(uint32_t seed, uint8_t openMask, int variant, int frame) {
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    RNG rng(hashCombine(seed, static_cast<uint32_t>(variant) * 73u + 7u));

    const Color hi = {200, 210, 230, 110};
    const Color lo = { 20,  22,  28,  80};

    auto jitter = [&](int v) {
        return clampiLocal(v + rng.range(-1, 1), 0, 15);
    };

    if (openMask & 1u) { // N
        for (int x = 0; x < 16; ++x) {
            setPx(s, x, 0, hi);
            if ((x & 1) == 0) setPx(s, x, 1, lo);
        }
    }
    if (openMask & 4u) { // S
        for (int x = 0; x < 16; ++x) {
            setPx(s, x, 15, hi);
            if ((x & 1) == 0) setPx(s, x, 14, lo);
        }
    }
    if (openMask & 8u) { // W
        for (int y = 0; y < 16; ++y) {
            setPx(s, 0, y, hi);
            if ((y & 1) == 0) setPx(s, 1, y, lo);
        }
    }
    if (openMask & 2u) { // E
        for (int y = 0; y < 16; ++y) {
            setPx(s, 15, y, hi);
            if ((y & 1) == 0) setPx(s, 14, y, lo);
        }
    }

    // Tiny chips
    for (int i = 0; i < 3; ++i) {
        const int x = jitter(rng.range(0, 15));
        const int y = jitter(rng.range(0, 15));
        if ((x == 0 && (openMask & 8u)) || (x == 15 && (openMask & 2u)) ||
            (y == 0 && (openMask & 1u)) || (y == 15 && (openMask & 4u))) {
            setPx(s, x, y, {255, 255, 255, 90});
        }
    }

    return s;
}

// Chasm rim overlay: light earthen rim + inner dark band.
SpritePixels genChasmRimOverlay16(uint32_t seed, uint8_t openMask, int variant, int frame) {
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hashCombine(seed, static_cast<uint32_t>(variant) * 97u + 11u));

    const Color rim = {110, 92, 60, 200};
    const Color inner = {10, 10, 12, 140};

    auto speck = [&](int x, int y) {
        if (noise01(seed ^ 0xA11C0Au, x, y) > 0.82f) {
            setPx(s, x, y, add(rim, 10, 8, 6));
        }
    };

    if (openMask & 1u) {
        for (int x = 0; x < 16; ++x) { setPx(s, x, 0, rim); setPx(s, x, 1, inner); speck(x, 0); }
    }
    if (openMask & 4u) {
        for (int x = 0; x < 16; ++x) { setPx(s, x, 15, rim); setPx(s, x, 14, inner); speck(x, 15); }
    }
    if (openMask & 8u) {
        for (int y = 0; y < 16; ++y) { setPx(s, 0, y, rim); setPx(s, 1, y, inner); speck(0, y); }
    }
    if (openMask & 2u) {
        for (int y = 0; y < 16; ++y) { setPx(s, 15, y, rim); setPx(s, 14, y, inner); speck(15, y); }
    }

    // Random notch.
    if (rng.chance(0.35f)) {
        int x = rng.range(2, 13);
        int y = rng.range(2, 13);
        setPx(s, x, y, rim);
    }

    return s;
}

SpritePixels genTopDownShade16(uint32_t seed, uint8_t mask, int variant, int frame) {
    (void)variant;
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const int thickness = 3;

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            float a = 0.0f;
            if (mask & 1u) a = std::max(a, 1.0f - (static_cast<float>(y) / thickness));
            if (mask & 4u) a = std::max(a, 1.0f - (static_cast<float>(15 - y) / thickness));
            if (mask & 8u) a = std::max(a, 1.0f - (static_cast<float>(x) / thickness));
            if (mask & 2u) a = std::max(a, 1.0f - (static_cast<float>(15 - x) / thickness));

            if (a <= 0.0f) continue;
            const float d = bayer4Threshold(x + static_cast<int>(seed), y + static_cast<int>(seed));
            a = std::clamp(a - (d - 0.5f) * 0.12f, 0.0f, 1.0f);
            const uint8_t A = clamp8(static_cast<int>(std::lround(120.0f * a)));
            if (A == 0) continue;

            setPx(s, x, y, {0,0,0,A});
        }
    }

    return s;
}

SpritePixels genPillar16(uint32_t seed, int frame) {
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const TilePalette p = wallPalette(seed);

    // Base + shaft.
    circle(s, 8, 10, 4, p.base);
    circle(s, 8, 10, 2, p.light);

    // Highlight.
    circle(s, 7, 9, 1, p.accent2);

    // Outline.
    applyExteriorOutline(s, withAlpha(p.dark, 220));

    return s;
}

SpritePixels genBoulder16(uint32_t seed, int frame) {
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const TilePalette p = wallPalette(seed);

    // Irregular blob.
    for (int y = 4; y < 15; ++y) {
        for (int x = 2; x < 14; ++x) {
            const int dx = x - 8;
            const int dy = y - 10;
            const float d = std::sqrt(static_cast<float>(dx*dx + dy*dy));
            const float n = noise01(seed ^ 0xB0A1D3u, x, y);
            if (d < 4.5f + (n - 0.5f) * 1.3f) {
                Color c = p.base;
                if (dx + dy < -2) c = lerp(c, p.light, 0.25f);
                if (dx + dy > 4) c = lerp(c, p.dark, 0.25f);
                setPx(s, x, y, c);
            }
        }
    }

    // Spec.
    setPx(s, 6, 8, p.accent2);

    applyExteriorOutline(s, withAlpha(p.dark, 220));

    return s;
}

SpritePixels genFountain16(uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    const TilePalette p = wallPalette(seed);

    // Stone rim.
    circle(s, 8, 10, 5, p.dark);
    circle(s, 8, 10, 4, p.base);

    // Water.
    Color water = {60, 110, 170, 220};
    circle(s, 8, 10, 3, water);

    // Animated shimmer.
    if ((frame & 1) == 0) {
        setPx(s, 7, 9, {200, 230, 255, 240});
        setPx(s, 9, 11, {180, 210, 255, 200});
    } else {
        setPx(s, 8, 9, {200, 230, 255, 240});
        setPx(s, 10, 11, {180, 210, 255, 200});
    }

    applyExteriorOutline(s, withAlpha(p.dark, 220));

    return s;
}

SpritePixels genAltar16(uint32_t seed, int frame) {
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    const TilePalette p = floorPalette(3, seed);

    rect(s, 4, 8, 8, 5, p.base);
    outlineRect(s, 4, 8, 8, 5, p.dark);

    // Sigil.
    setPx(s, 8, 10, p.accent2);
    setPx(s, 7, 10, p.accent);
    setPx(s, 9, 10, p.accent);
    setPx(s, 8, 9, p.accent);

    applyExteriorOutline(s, withAlpha(p.dark, 220));

    return s;
}

SpritePixels genStairs16(uint32_t seed, bool up, int frame) {
    (void)seed;
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const Color a = {190, 190, 200, 230};
    const Color b = {120, 120, 135, 230};
    const Color o = { 30,  30,  40, 220};

    // Steps.
    for (int i = 0; i < 5; ++i) {
        const int y = 6 + i;
        rect(s, 5, y, 6, 1, (i & 1) ? a : b);
    }
    outlineRect(s, 5, 6, 6, 5, o);

    // Arrow.
    if (up) {
        line(s, 8, 5, 8, 3, {230, 230, 240, 230});
        line(s, 8, 3, 7, 4, {230, 230, 240, 230});
        line(s, 8, 3, 9, 4, {230, 230, 240, 230});
    } else {
        line(s, 8, 12, 8, 14, {230, 230, 240, 230});
        line(s, 8, 14, 7, 13, {230, 230, 240, 230});
        line(s, 8, 14, 9, 13, {230, 230, 240, 230});
    }

    return s;
}

SpritePixels genDoor16(uint32_t seed, bool open, bool locked, int frame) {
    (void)frame;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const TilePalette wp = wallPalette(seed);
    const Color wood = {140, 100, 60, 255};
    const Color wood2= {110,  78, 46, 255};
    const Color metal= {220, 220, 230, 255};

    if (open) {
        // Doorway frame.
        rect(s, 4, 4, 2, 8, wp.base);
        rect(s, 10, 4, 2, 8, wp.base);
        rect(s, 4, 4, 8, 2, wp.light);
        outlineRect(s, 4, 4, 8, 8, wp.dark);
    } else {
        // Closed door slab.
        rect(s, 5, 4, 6, 9, wood);
        outlineRect(s, 5, 4, 6, 9, wp.dark);
        for (int y = 5; y < 12; ++y) {
            if ((y & 1) == 0) {
                setPx(s, 6, y, wood2);
                setPx(s, 9, y, wood2);
            }
        }
        // Handle / lock.
        if (locked) {
            rect(s, 8, 8, 2, 2, metal);
            setPx(s, 9, 10, metal);
        } else {
            setPx(s, 10, 9, metal);
        }
    }

    applyExteriorOutline(s, withAlpha(wp.dark, 220));

    return s;
}

// Environmental overlays.
SpritePixels genGas16(uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const Color gas1 = {160, 90, 200, 90};
    const Color gas2 = {90, 200, 160, 70};

    const int ox = (frame & 3) - 1;
    const int oy = ((frame >> 1) & 3) - 1;

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float n = noise01(seed ^ 0x6A5u, x + ox, y + oy);
            if (n > 0.66f) {
                blendOver(s, x, y, (n > 0.82f) ? gas1 : gas2);
            }
        }
    }

    // Slight denser core.
    circle(s, 8, 8, 3, {170, 120, 220, 65});

    return s;
}

SpritePixels genFire16(uint32_t seed, int frame) {
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const Color hot = {255, 220, 140, 180};
    const Color mid = {255, 140,  60, 170};
    const Color low = {120,  20,  10, 140};

    // Flicker offset.
    const int wob = (hash32(seed ^ static_cast<uint32_t>(frame) * 911u) & 3u) - 1;

    for (int y = 4; y < 15; ++y) {
        for (int x = 3; x < 13; ++x) {
            const int dx = x - (8 + wob);
            const int dy = y - 11;
            const float d = std::sqrt(static_cast<float>(dx*dx + dy*dy));
            const float n = noise01(seed ^ 0xF1AEu, x + frame, y - frame);

            float r = 5.0f - (static_cast<float>(y) - 6.0f) * 0.15f;
            if (d < r + (n - 0.5f) * 1.2f) {
                Color c = low;
                if (y < 10) c = mid;
                if (y < 8) c = hot;

                // Hot specks.
                if (n > 0.92f) c = hot;

                blendOver(s, x, y, c);
            }
        }
    }

    return s;
}

// Isometric diamond overlays for gas/fire based on square-space sampling.
SpritePixels genIsoOverlayFromSquare16(const SpritePixels& square16, uint32_t seed, int frame) {
    (void)seed;
    (void)frame;
    SpritePixels out = makeSprite(16, 8, {0,0,0,0});
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            int u=0, v=0;
            if (!isoDiamondToSquareUV(16, x, y, u, v)) continue;
            const Color c = getPx(square16, u, v);
            setPx(out, x, y, c);
        }
    }
    return out;
}

// Isometric 2.5D block sprite helpers (square 16x16).
SpritePixels genIsoWallBlock16(uint32_t seed, int frame) {
    (void)frame;
    const TilePalette p = wallPalette(seed);

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // Define a simple cube-ish wall block.
    const int cx = 8;
    const int topY = 2;
    const int halfW = 5;
    const int halfH = 3;
    const int height = 8;

    const int xTop = cx;
    const int yTop = topY;
    const int xRight = cx + halfW;
    const int yRight = topY + halfH;
    const int xBottom = cx;
    const int yBottom = topY + halfH * 2;
    const int xLeft = cx - halfW;
    const int yLeft = topY + halfH;

    const int yDrop = height;

    const int xRight2 = xRight;
    const int yRight2 = yRight + yDrop;
    const int xBottom2 = xBottom;
    const int yBottom2 = yBottom + yDrop;
    const int xLeft2 = xLeft;
    const int yLeft2 = yLeft + yDrop;

    // Faces.
    fillQuad(s, xTop, yTop, xRight, yRight, xBottom, yBottom, xLeft, yLeft, p.light);
    fillQuad(s, xLeft, yLeft, xBottom, yBottom, xBottom2, yBottom2, xLeft2, yLeft2, p.base);
    fillQuad(s, xBottom, yBottom, xRight, yRight, xRight2, yRight2, xBottom2, yBottom2, p.dark);

    applyExteriorOutline(s, withAlpha(Color{20, 20, 26, 255}, 220));

    return s;
}

SpritePixels genIsoDoorBlock16(uint32_t seed, bool locked, int frame) {
    (void)frame;
    SpritePixels s = genIsoWallBlock16(seed ^ 0xD00Du, frame);

    // Paint a door on the brighter face (left face), roughly.
    const Color wood = {150, 110, 70, 255};
    const Color wood2= {110,  80, 48, 255};
    const Color metal= {220, 220, 230, 255};

    // Door panel rectangle in approximate left face region.
    for (int y = 7; y < 13; ++y) {
        for (int x = 4; x < 8; ++x) {
            if (getPx(s, x, y).a == 0) continue;
            setPx(s, x, y, ( (x + y) & 1) ? wood : wood2);
        }
    }

    if (locked) {
        setPx(s, 6, 10, metal);
        setPx(s, 6, 11, metal);
    } else {
        setPx(s, 7, 10, metal);
    }

    return s;
}

SpritePixels genIsoDoorwayBlock16(uint32_t seed, int frame) {
    (void)seed;
    (void)frame;
    SpritePixels s = genIsoWallBlock16(seed ^ 0x0A11u, frame);

    // Carve out a doorway hole (make pixels transparent in a central region).
    for (int y = 8; y < 14; ++y) {
        for (int x = 5; x < 9; ++x) {
            Color c = getPx(s, x, y);
            if (c.a == 0) continue;
            // Keep a thin frame.
            if (x == 5 || x == 8 || y == 8) continue;
            setPx(s, x, y, {0,0,0,0});
        }
    }

    applyExteriorOutline(s, {20, 20, 26, 200});

    return s;
}

SpritePixels genIsoPillarBlock16(uint32_t seed, int frame) {
    (void)frame;
    const TilePalette p = wallPalette(seed);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // Draw a tall column prism.
    const int x0 = 6;
    const int y0 = 4;
    const int w = 4;
    const int h = 10;

    rect(s, x0, y0, w, h, p.base);
    rect(s, x0 + 2, y0, 2, h, p.dark);
    rect(s, x0, y0, 2, h, p.light);

    // Cap.
    rect(s, x0 - 1, y0 - 1, w + 2, 2, p.light);
    outlineRect(s, x0 - 1, y0 - 1, w + 2, h + 1, withAlpha(p.dark, 220));

    applyExteriorOutline(s, withAlpha(Color{20, 20, 26, 255}, 220));

    return s;
}

SpritePixels genIsoBoulderBlock16(uint32_t seed, int frame) {
    // In isometric mode we still just show a chunky shaded rock.
    return genBoulder16(seed ^ 0x150B0u, frame);
}

} // namespace

// --- Public terrain/procedural functions ------------------------------------

SpritePixels generateFloorTile(uint32_t seed, int frame, int pxSize) {
    return generateThemedFloorTile(seed, 0, frame, pxSize);
}

SpritePixels generateThemedFloorTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genFloor16(seed, style, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateWallTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genWall16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateChasmTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genChasm16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels projectToIsometricDiamond(const SpritePixels& src, uint32_t seed, int frame, bool outline) {
    (void)seed;
    (void)frame;

    if (src.w <= 0 || src.h <= 0) return makeSprite(0, 0, {0,0,0,0});

    const int N = src.w;
    const int H = std::max(1, src.h / 2);

    SpritePixels out = makeSprite(N, H, {0,0,0,0});

    // Inverse isometric mapping from destination diamond pixel to source square pixel.
    const int half = N / 2;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < N; ++x) {
            const int u = x + 2 * y - half;
            const int v = 2 * y - x + half;
            if (u < 0 || v < 0 || u >= src.w || v >= src.h) continue;
            out.at(x, y) = src.at(u, v);
        }
    }

    if (outline) {
        Color avg = averageOpaqueColor(out);
        Color ol = withAlpha(mul(avg, 0.25f), 220);
        applyExteriorOutline(out, ol);
    }

    return out;
}

SpritePixels generateIsometricThemedFloorTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoFloor16(seed, style, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateIsometricChasmTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoChasm16(seed, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateIsometricWallBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoWallBlock16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricDoorBlockTile(uint32_t seed, bool locked, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoDoorBlock16(seed, locked, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricDoorwayBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoDoorwayBlock16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricPillarBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoPillarBlock16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricBoulderBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoBoulderBlock16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricEdgeShadeOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoMaskOverlay16(seed, mask, frame, /*thickness=*/2, /*alphaMax=*/120);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateIsometricChasmGloomOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoMaskOverlay16(seed ^ 0xC115Au, mask, frame, /*thickness=*/4, /*alphaMax=*/160);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateIsometricCastShadowOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize) {
    // Similar to edge shade but thicker/softer.
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoMaskOverlay16(seed ^ 0x5A4D0Fu, mask, frame, /*thickness=*/5, /*alphaMax=*/140);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateIsometricEntityShadowOverlay(uint32_t seed, uint8_t lightDir, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genIsoEntityShadow16(seed, lightDir, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateIsometricStairsOverlay(uint32_t seed, bool up, int frame, int pxSize) {
    // Generate top-down stairs overlay and project to iso diamond.
    pxSize = clampSpriteSize(pxSize);

    SpritePixels stairsSq = genStairs16(seed, up, frame);
    SpritePixels base = genIsoOverlayFromSquare16(stairsSq, seed, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generatePillarTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genPillar16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateBoulderTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genBoulder16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateFountainTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genFountain16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateAltarTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genAltar16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateStairsTile(uint32_t seed, bool up, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genStairs16(seed, up, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateDoorTile(uint32_t seed, bool open, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genDoor16(seed, open, /*locked=*/false, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateLockedDoorTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genDoor16(seed, /*open=*/false, /*locked=*/true, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateUIPanelTile(UITheme theme, uint32_t seed, int frame, int pxSize) {
    (void)frame;
    pxSize = clampSpriteSize(pxSize);

    SpritePixels s = makeSprite(16, 16, {0,0,0,255});
    RNG rng(hashCombine(seed, static_cast<uint32_t>(theme) * 991u + 17u));

    Color base{0,0,0,255};
    Color speck{0,0,0,255};
    Color vein{0,0,0,255};

    switch (theme) {
        case UITheme::DarkStone:
            base  = {26, 28, 34, 255};
            speck = {55, 58, 70, 255};
            vein  = {18, 18, 22, 255};
            break;
        case UITheme::Parchment:
            base  = {92, 76, 46, 255};
            speck = {120, 98, 60, 255};
            vein  = {70, 58, 38, 255};
            break;
        case UITheme::Arcane:
            base  = {34, 18, 44, 255};
            speck = {64, 36, 78, 255};
            vein  = {20, 10, 28, 255};
            break;
    }

    // Subtle deterministic texture.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float n = noise01(seed ^ 0xA11E1u, x, y);
            Color c = base;
            if (n > 0.86f) c = speck;
            if (n < 0.06f) c = vein;
            setPx(s, x, y, c);
        }
    }

    // Tiny theme motif (kept subtle so tiling doesn't become obvious).
    if (theme == UITheme::Arcane) {
        // A faint rune dot pattern.
        for (int i = 0; i < 6; ++i) {
            const int x = rng.range(1, 14);
            const int y = rng.range(1, 14);
            setPx(s, x, y, {90, 60, 120, 255});
        }
    }

    return (pxSize == 16) ? s : resampleSpriteToSizeInternal(s, pxSize);
}

SpritePixels generateUIOrnamentTile(UITheme theme, uint32_t seed, int frame, int pxSize) {
    (void)seed;
    (void)frame;
    pxSize = clampSpriteSize(pxSize);

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    Color ink{200,200,200,220};
    switch (theme) {
        case UITheme::DarkStone: ink = {180, 200, 235, 220}; break;
        case UITheme::Parchment: ink = {235, 215, 160, 220}; break;
        case UITheme::Arcane: ink = {230, 170, 255, 220}; break;
    }

    // Corner filigree: a small arc + ticks.
    for (int i = 0; i < 8; ++i) {
        setPx(s, i, 0, withAlpha(ink, 160));
        setPx(s, 0, i, withAlpha(ink, 160));
    }

    circle(s, 2, 2, 2, withAlpha(ink, 120));
    line(s, 1, 6, 6, 1, withAlpha(ink, 180));

    // Tiny accent dot.
    setPx(s, 4, 4, ink);

    return (pxSize == 16) ? s : resampleSpriteToSizeInternal(s, pxSize);
}

SpritePixels generateFloorDecalTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genFloorDecal16(seed, style, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricFloorDecalOverlay(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels square = genFloorDecal16(seed, style, frame);
    SpritePixels base = genIsoOverlayFromSquare16(square, seed, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateWallDecalTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genWallDecal16(seed, style, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateFloorBorderOverlay(uint32_t seed, uint8_t style, uint8_t mask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genFloorBorderOverlay16(seed, style, mask, variant, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricFloorBorderOverlay(uint32_t seed, uint8_t style, uint8_t mask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels square = genFloorBorderOverlay16(seed, style, mask, variant, frame);
    SpritePixels base = genIsoOverlayFromSquare16(square, seed, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}


SpritePixels generateWallEdgeOverlay(uint32_t seed, uint8_t openMask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genWallEdgeOverlay16(seed, openMask, variant, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateChasmRimOverlay(uint32_t seed, uint8_t openMask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genChasmRimOverlay16(seed, openMask, variant, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateTopDownWallShadeOverlay(uint32_t seed, uint8_t mask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genTopDownShade16(seed, mask, variant, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateConfusionGasTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genGas16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateFireTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels base = genFire16(seed, frame);
    return resampleSpriteToSizeInternal(base, pxSize);
}

SpritePixels generateIsometricGasTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels square = genGas16(seed, frame);
    SpritePixels base = genIsoOverlayFromSquare16(square, seed, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateIsometricFireTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels square = genFire16(seed, frame);
    SpritePixels base = genIsoOverlayFromSquare16(square, seed, frame);
    return resampleSpriteToRectInternal(base, pxSize, std::max(1, pxSize / 2));
}

SpritePixels generateEffectIcon(EffectKind kind, int frame, int pxSize) {
    (void)frame;
    pxSize = clampSpriteSize(pxSize);

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // Simple, high-contrast glyph-ish icons.
    const Color ink = {235, 235, 245, 255};
    const Color warn = {255, 180, 120, 255};
    const Color good = {160, 255, 190, 255};
    const Color bad  = {255, 140, 140, 255};

    switch (kind) {
        default:
        case EffectKind::Poison:
            circle(s, 8, 8, 5, withAlpha(bad, 220));
            line(s, 6, 6, 10, 10, ink);
            line(s, 10, 6, 6, 10, ink);
            break;
        case EffectKind::Regen:
            circle(s, 8, 8, 5, withAlpha(good, 200));
            line(s, 8, 5, 8, 11, ink);
            line(s, 5, 8, 11, 8, ink);
            break;
        case EffectKind::Shield:
            rect(s, 6, 5, 5, 8, withAlpha(ink, 200));
            setPx(s, 8, 13, withAlpha(ink, 200));
            break;
        case EffectKind::Haste:
            line(s, 4, 6, 12, 6, warn);
            line(s, 6, 10, 14, 10, warn);
            line(s, 4, 8, 12, 8, ink);
            break;
        case EffectKind::Vision:
            circle(s, 8, 8, 5, withAlpha(ink, 120));
            circle(s, 8, 8, 2, withAlpha(ink, 220));
            setPx(s, 8, 8, {0,0,0,255});
            break;
        case EffectKind::Invis:
            outlineRect(s, 4, 4, 8, 8, withAlpha(ink, 140));
            line(s, 4, 12, 12, 4, bad);
            break;
        case EffectKind::Web:
            for (int i = 0; i < 6; ++i) {
                line(s, 8, 8, 8 + (i - 3) * 2, 3, withAlpha(ink, 180));
                line(s, 8, 8, 3, 8 + (i - 3) * 2, withAlpha(ink, 180));
            }
            break;
        case EffectKind::Confusion:
            circle(s, 8, 8, 5, withAlpha(warn, 160));
            circle(s, 8, 8, 2, withAlpha(ink, 200));
            line(s, 8, 6, 10, 8, ink);
            line(s, 10, 8, 8, 10, ink);
            line(s, 8, 10, 6, 8, ink);
            break;
        case EffectKind::Burn:
            rect(s, 7, 6, 2, 7, withAlpha(bad, 200));
            rect(s, 6, 9, 4, 4, withAlpha(warn, 200));
            break;
        case EffectKind::Levitation:
            line(s, 8, 3, 8, 12, withAlpha(ink, 220));
            line(s, 8, 3, 6, 5, withAlpha(ink, 220));
            line(s, 8, 3, 10, 5, withAlpha(ink, 220));
            circle(s, 8, 12, 2, withAlpha(good, 170));
            break;
        case EffectKind::Fear:
            circle(s, 8, 8, 5, withAlpha(bad, 160));
            setPx(s, 8, 6, ink);
            setPx(s, 8, 8, ink);
            setPx(s, 8, 11, ink);
            break;
        case EffectKind::Hallucination:
            for (int i = 0; i < 6; ++i) {
                int x = 3 + (i * 2);
                int y = 4 + ((i & 1) * 6);
                setPx(s, x, y, {200, 170, 255, 220});
                setPx(s, x + 1, y + 1, {255, 230, 120, 200});
            }
            break;
        case EffectKind::Corrosion:
            rect(s, 6, 4, 4, 6, withAlpha(good, 120));
            rect(s, 7, 10, 2, 3, withAlpha(good, 180));
            setPx(s, 8, 13, withAlpha(good, 180));
            break;
    }

    return (pxSize == 16) ? s : resampleSpriteToSizeInternal(s, pxSize);
}

SpritePixels generateCursorReticleTile(uint32_t seed, bool isometric, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    // Generate directly at pxSize so stroke thickness remains readable at high zoom.
    SpritePixels s = makeSprite(pxSize, pxSize, {0,0,0,0});

    const int t = std::max(1, pxSize / 64); // stroke thickness
    const int pad = std::max(2, pxSize / 16);

    const uint8_t a0 = static_cast<uint8_t>(180 + ((frame & 1) ? 40 : 0));
    const Color ink = {235, 235, 245, a0};

    auto drawThickLine = [&](int x0, int y0, int x1, int y1) {
        for (int i = -t; i <= t; ++i) {
            line(s, x0 + i, y0, x1 + i, y1, ink);
            line(s, x0, y0 + i, x1, y1 + i, ink);
        }
    };

    if (!isometric) {
        // Corner brackets.
        drawThickLine(pad, pad, pad + pxSize / 6, pad);
        drawThickLine(pad, pad, pad, pad + pxSize / 6);

        drawThickLine(pxSize - pad - pxSize / 6, pad, pxSize - pad, pad);
        drawThickLine(pxSize - pad, pad, pxSize - pad, pad + pxSize / 6);

        drawThickLine(pad, pxSize - pad, pad + pxSize / 6, pxSize - pad);
        drawThickLine(pad, pxSize - pad - pxSize / 6, pad, pxSize - pad);

        drawThickLine(pxSize - pad - pxSize / 6, pxSize - pad, pxSize - pad, pxSize - pad);
        drawThickLine(pxSize - pad, pxSize - pad - pxSize / 6, pxSize - pad, pxSize - pad);

        // Crosshair.
        const int cx = pxSize / 2;
        const int cy = pxSize / 2;
        drawThickLine(cx - pxSize / 10, cy, cx + pxSize / 10, cy);
        drawThickLine(cx, cy - pxSize / 10, cx, cy + pxSize / 10);

    } else {
        // Diamond outline + cross.
        const int tileW = pxSize;
        const int tileH = std::max(1, pxSize / 2);
        const int cx = tileW / 2;
        const int cy = tileH / 2;

        auto inDiamond = [&](int x, int y) {
            const int hw = std::max(1, tileW / 2);
            const int hh = std::max(1, tileH / 2);
            const float nx = std::abs(static_cast<float>(x - cx)) / static_cast<float>(hw);
            const float ny = std::abs(static_cast<float>(y - cy)) / static_cast<float>(hh);
            return (nx + ny) <= 1.0f;
        };

        // Draw outline by marking pixels that are inside diamond but have a neighbor outside.
        for (int y = 0; y < tileH; ++y) {
            for (int x = 0; x < tileW; ++x) {
                if (!inDiamond(x, y)) continue;
                bool edge = false;
                if (!inDiamond(x - 1, y) || !inDiamond(x + 1, y) || !inDiamond(x, y - 1) || !inDiamond(x, y + 1)) edge = true;
                if (edge) {
                    for (int yy = -t; yy <= t; ++yy) {
                        for (int xx = -t; xx <= t; ++xx) {
                            setPx(s, x + xx, y + yy, ink);
                        }
                    }
                }
            }
        }

        // Cross.
        drawThickLine(cx - tileW / 10, cy, cx + tileW / 10, cy);
        drawThickLine(cx, cy - tileH / 3, cx, cy + tileH / 3);

        // Add a tiny center pip.
        setPx(s, cx, cy, {0,0,0,0});
        setPx(s, cx, cy, {255,255,255, a0});
    }

    // Seed-based subtle phase shift: shift alpha pattern for different seeds.
    if (seed != 0u) {
        const uint32_t h = hash32(seed ^ 0xC0A51Eu);
        const int off = static_cast<int>(h & 3u);
        for (int y = 0; y < s.h; ++y) {
            for (int x = 0; x < s.w; ++x) {
                Color c = s.at(x, y);
                if (c.a == 0) continue;
                if (((x + y + off) & 1) == 0) c.a = static_cast<uint8_t>(std::max<int>(0, c.a - 20));
                s.at(x, y) = c;
            }
        }
    }

    // Note: in isometric mode, only the top tileH rows are used (mapTileDst height).
    // The renderer still stores the texture as a square, but draws it scaled to tileW x tileH.
    // Keeping the rest transparent is harmless.

    return s;
}

SpritePixels generateProjectileSprite(ProjectileKind kind, uint32_t seed, int frame, bool use3d, int pxSize, bool isometric, bool isoRaytrace) {
    pxSize = clampSpriteSize(pxSize);

    // Base 2D projectile sprite on the 16px design grid.
    SpritePixels base = makeSprite(16, 16, {0,0,0,0});

    auto drawArrow = [&]() {
        const Color shaft = {220, 220, 230, 255};
        const Color fletch= {160, 120, 80, 255};
        line(base, 3, 12, 12, 3, shaft);
        // Head.
        setPx(base, 12, 3, {255, 255, 255, 255});
        setPx(base, 11, 3, {255, 255, 255, 255});
        setPx(base, 12, 4, {255, 255, 255, 255});
        // Fletching.
        setPx(base, 3, 12, fletch);
        setPx(base, 2, 12, fletch);
        setPx(base, 3, 13, fletch);
    };

    auto drawRock = [&]() {
        const TilePalette p = wallPalette(seed);
        for (int y = 6; y < 14; ++y) {
            for (int x = 4; x < 12; ++x) {
                const int dx = x - 8;
                const int dy = y - 10;
                const float d = std::sqrt(static_cast<float>(dx*dx + dy*dy));
                const float n = noise01(seed ^ 0xB0C4u, x, y);
                if (d < 3.5f + (n - 0.5f) * 1.0f) {
                    Color c = p.base;
                    if (dx + dy < -1) c = lerp(c, p.light, 0.25f);
                    if (dx + dy > 3) c = lerp(c, p.dark, 0.25f);
                    setPx(base, x, y, c);
                }
            }
        }
        applyExteriorOutline(base, withAlpha(Color{20,20,26,255}, 220));
    };

    auto drawSpark = [&]() {
        const Color c0 = {255, 240, 180, 255};
        const Color c1 = {255, 200, 80, 255};
        const int t = (frame & 3);
        setPx(base, 8, 8, c0);
        line(base, 8, 8, 8 + (t - 1), 3, c1);
        line(base, 8, 8, 13, 8 + (1 - t), c1);
        line(base, 8, 8, 3 + (t), 8, c1);
        line(base, 8, 8, 8, 13 - (t), c1);
    };

    auto drawFireball = [&]() {
        const Color hot = {255, 240, 160, 255};
        const Color mid = {255, 140, 60, 255};
        const Color low = {120, 20, 10, 220};
        circle(base, 8, 8, 4, mid);
        circle(base, 8, 8, 2, hot);
        // Flicker tongues.
        if ((frame & 1) == 0) {
            setPx(base, 8, 3, hot);
            setPx(base, 11, 6, mid);
        } else {
            setPx(base, 7, 4, hot);
            setPx(base, 12, 8, mid);
        }
        applyExteriorOutline(base, withAlpha(low, 220));
    };

    auto drawTorch = [&]() {
        const Color wood = {140, 100, 60, 255};
        const Color wrap = {110, 80, 48, 255};
        rect(base, 7, 7, 2, 6, wood);
        rect(base, 6, 11, 4, 2, wrap);
        // Flame.
        const int wob = (frame & 1) ? 1 : 0;
        setPx(base, 8 + wob, 5, {255, 230, 140, 255});
        setPx(base, 8, 6, {255, 140, 60, 255});
        setPx(base, 7, 6, {255, 140, 60, 255});
        applyExteriorOutline(base, withAlpha(Color{20,20,26,255}, 220));
    };

    switch (kind) {
        case ProjectileKind::Arrow:    drawArrow(); break;
        case ProjectileKind::Rock:     drawRock(); break;
        case ProjectileKind::Spark:    drawSpark(); break;
        case ProjectileKind::Fireball: drawFireball(); break;
        case ProjectileKind::Torch:    drawTorch(); break;
    }

    // 3D voxel mode: render a dedicated projectile mini-model.
    if (use3d) {
        if (isometric) {
            return renderSprite3DProjectileIso(kind, base, seed, frame, pxSize, isoRaytrace);
        }
        return renderSprite3DProjectile(kind, base, seed, frame, pxSize);
    }

    // 2D mode: upscale the base sprite.
    SpritePixels out = resampleSpriteToSizeInternal(base, pxSize);
    return out;
}

// -----------------------------------------------------------------------------
