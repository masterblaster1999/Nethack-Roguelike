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

        case ItemKind::PotionEnergy: {
            // Bright cyan "mana" potion: glowing fluid + a couple sparkles.
            Color glass = {200,200,220,180};
            Color fluid = {90,240,230,220};

            outlineRect(s, 6, 4, 4, 9, mul(glass, 0.9f));
            rect(s, 7, 6, 2, 6, fluid);
            rect(s, 6, 3, 4, 2, {140,140,150,220});

            if (frame % 2 == 1) {
                setPx(s, 8, 7, {255,255,255,170});
                setPx(s, 9, 9, {255,255,255,120});
                setPx(s, 9, 6, {255,255,255,170});
            }
            break;
        }

        case ItemKind::SpellbookMagicMissile: {
            drawSpellbook({90,140,240,255}, {220,240,255,220});
            break;
        }
        case ItemKind::SpellbookBlink: {
            drawSpellbook({170,90,220,255}, {255,255,255,200});
            break;
        }
        case ItemKind::SpellbookMinorHeal: {
            drawSpellbook({90,200,120,255}, {240,255,240,210});
            break;
        }
        case ItemKind::SpellbookDetectTraps: {
            drawSpellbook({200,160,90,255}, {255,245,210,210});
            break;
        }
        case ItemKind::SpellbookFireball: {
            drawSpellbook({220,90,60,255}, {255,230,200,220});
            if (frame % 2 == 1) {
                setPx(s, 9, 6, {255,240,200,140});
            }
            break;
        }
        case ItemKind::SpellbookStoneskin: {
            drawSpellbook({160,160,170,255}, {235,235,245,220});
            break;
        }
        case ItemKind::SpellbookHaste: {
            drawSpellbook({220,200,80,255}, {255,255,210,220});
            if (frame % 2 == 1) {
                setPx(s, 11, 7, {255,255,255,120});
            }
            break;
        }
        case ItemKind::SpellbookInvisibility: {
            drawSpellbook({80,80,120,255}, {220,220,255,180});
            if (frame % 2 == 1) {
                setPx(s, 6, 7, {255,255,255,70});
                setPx(s, 10, 11, {255,255,255,60});
            }
            break;
        }
        case ItemKind::SpellbookPoisonCloud: {
            drawSpellbook({80,160,90,255}, {220,255,220,200});
            if (frame % 2 == 1) {
                setPx(s, 7, 11, {200,255,200,110});
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

            // Flame flicker: drive a tiny circular offset and brightness pulse from the
            // same 4-frame phase used by the rest of the procedural animation system.
            const float ang = phaseAngle_4(frame);
            const int ox = static_cast<int>(std::lround(std::cos(ang) * 0.85f)); // 1,0,-1,0
            const int oy = static_cast<int>(std::lround(std::sin(ang) * 0.65f)); // 0,1,0,-1

            const float pulse01 = 0.5f + 0.5f * std::cos(ang * 2.0f + hash01_16(hash32(seed ^ 0xF1A9u)) * TAU);
            const float hot = 0.78f + 0.22f * pulse01;

            Color flameOuter = {255,170,60,220};
            Color flameMid   = {255,220,120,210};
            Color flameCore  = {255,255,200,190};

            line(s, 8, 5, 8, 14, wood);
            rect(s, 7, 11, 3, 3, mul(wood, 0.85f));
            rect(s, 6, 4, 5, 2, mul(wood, 0.6f));

            const int fx = 8 + ox;
            const int fy = 3 + oy;

            circle(s, fx, fy, 2, mul(flameOuter, hot));
            circle(s, fx, fy - 1, 2, mul(flameMid, hot));
            circle(s, fx, fy - 1, 1, mul(flameCore, 0.92f + 0.08f * hot));

            // Tiny embers / smoke specks (coherent, looped noise — no harsh blink).
            const float n = loopValueNoise2D01(3.7f, 1.2f, seed ^ 0xE11B3A5u, 3.0f, frame, 2.1f);
            if (n > 0.62f) setPx(s, fx + 1, fy - 2, {255,255,255,120});
            if (n < 0.28f) setPx(s, fx - 1, fy - 3, {190,190,205,80});

            break;
        }

        // --- Rings (append-only) ---
        case ItemKind::RingMight:
        case ItemKind::RingAgility:
        case ItemKind::RingFocus:
        case ItemKind::RingProtection:
        case ItemKind::RingSearching:
        case ItemKind::RingSustenance: {
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
                case ItemKind::RingSearching:  gem = {210, 180, 255, 240}; break;
                case ItemKind::RingSustenance: gem = {255, 190, 60, 240}; break;
                default: break;
            }

            // Gem on top of the ring
            circle(s, 8, 5, 2, gem);
            circle(s, 8, 5, 1, mul(gem, 0.85f));

            // Orbiting glints (4-frame loop) make rings feel "alive" without flicker.
            static const int hx[4] = { 9, 10, 7, 6 };
            static const int hy[4] = { 7,  9, 11, 9 };
            const int i = frame & 3;
            setPx(s, hx[i], hy[i], {255,255,255,110});
            setPx(s, hx[(i + 1) & 3], hy[(i + 1) & 3], {255,255,255,70});

            static const int gx[4] = { 9, 8, 7, 8 };
            static const int gy[4] = { 5, 4, 5, 6 };
            setPx(s, gx[i], gy[i], {255,255,255,150});

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

    return use3d ? render3d(s)
                         : resampleSpriteToSize(s, pxSize);
}

SpritePixels generateProjectileSprite(ProjectileKind kind, uint32_t seed, int frame, bool use3d, int pxSize, bool isometric, bool isoRaytrace) {
    pxSize = clampSpriteSize(pxSize);
    (void)seed;
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    switch (kind) {
        case ProjectileKind::Arrow: {
            Color c = {220,220,220,255};
            line(s, 3, 13, 13, 3, c);
            line(s, 12, 2, 14, 4, c);
            line(s, 2, 14, 4, 12, c);

            // Specular glint that travels along the shaft over 4 frames.
            const int fi = (frame & 3);
            const int gx = 4 + fi * 3;           // 4,7,10,13
            const int gy = 12 - fi * 3;          // 12,9,6,3
            setPx(s, gx, gy, {255,255,255,180});
            if (fi == 1 || fi == 3) setPx(s, gx - 1, gy + 1, {255,255,255,90});
            break;
        }
        case ProjectileKind::Rock: {
            Color stone = {140,140,150,255};
            circle(s, 8, 8, 3, stone);

            // Tumble highlight rotates around the rock across 4 frames.
            static const int hx[4] = {9, 8, 7, 8};
            static const int hy[4] = {7, 6, 7, 8};
            static const int sx[4] = {7, 6, 7, 8};
            static const int sy[4] = {9, 8, 9, 10};

            const int fi = (frame & 3);
            setPx(s, hx[fi], hy[fi], {255,255,255,120});
            setPx(s, sx[fi], sy[fi], {60,60,70,85});
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

    if (use3d) {
        return isometric ? renderSprite3DProjectileIso(kind, s, seed, frame, pxSize, isoRaytrace)
                         : renderSprite3DProjectile(kind, s, seed, frame, pxSize);
    }
    return resampleSpriteToSize(s, pxSize);
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

    // Seamless 4-frame animation: drift the sampling point in a circle.
    const float ang = phaseAngle_4(frame);
    const float driftX = std::cos(ang) * 2.6f;
    const float driftY = std::sin(ang) * 2.6f;

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);

            // Stronger vignette than floor to suggest depth.
            float cx = (fx - 7.5f) / 7.5f;
            float cy = (fy - 7.5f) / 7.5f;
            float v = 1.0f - 0.24f * (cx*cx + cy*cy);

            // Coherent void texture (no harsh per-frame flicker).
            const float n = fbm2D01(fx * 1.05f + driftX + 4.7f,
                                    fy * 1.05f + driftY - 2.9f,
                                    seed ^ 0xC4A5Au);
            float f = (0.74f + (n - 0.5f) * 0.38f) * v;

            // Faint animated ripple banding.
            const float ripple = 0.90f + 0.10f * std::sin((fx * 0.55f) + (fy * 0.35f) + (seed % 97u) * 0.05f + ang * 1.15f);
            f *= ripple;

            // Tiny drifting micro-grain to keep large chasms from feeling static.
            const float g = loopValueNoise2D01(fx * 0.90f, fy * 0.90f, seed ^ 0xBADD1u, 4.0f, frame, 1.9f);
            f += (g - 0.5f) * 0.05f;

            s.at(x, y) = rampShadeTile(base, f * 0.95f, x, y);
        }
    }

    // Tiny "embers" of reflected light in the abyss.
    // Instead of toggling random points each frame (which can flicker), we place a
    // deterministic set of candidates and animate their intensity smoothly.
    RNG sp(hash32(seed ^ 0xC4A5Au));
    const int candidates = 10;
    for (int i = 0; i < candidates; ++i) {
        int x = sp.range(1, 14);
        int y = sp.range(1, 14);

        const float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(ang * 1.7f + static_cast<float>(i) * 1.1f + static_cast<float>(seed & 0xFFu) * 0.03f));
        if (tw < 0.55f) continue;

        Color c = s.at(x, y);
        c = add(c,
                static_cast<int>(std::lround(18.0f * tw)),
                static_cast<int>(std::lround(22.0f * tw)),
                static_cast<int>(std::lround(35.0f * tw)));
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
    // In addition to the silhouette darkening, we add a very gentle, *edge-only*
    // bevel lighting ramp in isometric mode. This nudges the diamond to read as
    // a 3D plane under a consistent light direction (top-left), without turning
    // the interior into a distracting gradient.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float sx = (static_cast<float>(x) - cx) / hw; // [-1,1]
            const float sy = (static_cast<float>(y) - cy) / hh; // [-1,1]
            const float d = std::abs(sx) + std::abs(sy);
            if (d > 1.0f) continue;

            Color c = squashed.at(x, y);

            // Fade a touch darker toward the boundary so the silhouette stays crisp.
            if (d > 0.90f) {
                const float t = std::clamp((d - 0.90f) / 0.10f, 0.0f, 1.0f);
                c = mul(c, 1.0f - 0.12f * t);
            }

            // Isometric bevel shading: highlight the top-left edges, darken the bottom-right edges.
            // Only applied to terrain tiles (outline=true), so translucent overlays (gas/fire/etc)
            // keep their intended colors.
            if (outline && c.a != 0) {
                // Light comes from the top-left (screen space).
                float dir = (-sx - sy) * 0.5f; // [-1,1] approx
                dir = std::clamp(dir, -1.0f, 1.0f);

                // Gentle ground-plane lighting: a subtle gradient across the whole diamond
                // helps the isometric ground read as a single tilted plane, without
                // overpowering the underlying floor texture.
                const uint32_t pn = hashCombine(seed ^ 0x19050u,
                                                static_cast<uint32_t>(x + y * 131 + frame * 17));
                const float pNoise = (((pn & 0xFFu) / 255.0f) - 0.5f) * 0.04f; // +/-2% approx
                const float kPlane = 0.055f; // +/-5.5% across the tile
                float plane = 1.0f + kPlane * dir + pNoise;
                plane = std::clamp(plane, 0.88f, 1.12f);
                c = mul(c, plane);

                const float edgeT = std::clamp((d - 0.55f) / 0.45f, 0.0f, 1.0f);
                if (edgeT > 0.0f) {
                    // Stronger near corners, weaker along flat edges.
                    const float ax = std::abs(sx);
                    const float ay = std::abs(sy);
                    const float edgeAniso = std::abs(ax - ay);
                    const float cornerW = 1.0f - std::clamp(edgeAniso * 1.6f, 0.0f, 1.0f);

                    const float kBevel = 0.11f; // subtle (~±11% at strongest edge pixels)
                    float shade = 1.0f + kBevel * edgeT * dir;

                    // Tiny corner AO so seam junctions feel grounded.
                    shade *= (1.0f - 0.06f * edgeT * edgeT * cornerW);

                    shade = std::clamp(shade, 0.70f, 1.30f);
                    c = mul(c, shade);
                }
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



SpritePixels generateIsometricThemedFloorTile(uint32_t seed, uint8_t style, int frame, int pxSize) {
    // Purpose-built isometric themed floor tile drawn directly in diamond space.
    //
    // Historically, isometric floors were made by projecting the top-down square tiles.
    // That keeps things simple, but it also means patterns (cracks, seams, planks) are
    // "screen-axis aligned" and can look a bit off in 2.5D view. Generating directly
    // in diamond space keeps motifs aligned to the 2:1 isometric grid and avoids any
    // projection/resample artifacts.
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);
    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});

    // Style mixing keeps per-theme patterns deterministic but distinct.
    const uint32_t sMix = static_cast<uint32_t>(style) * 0x9E3779B9u;
    RNG rng(hash32(seed ^ sMix ^ 0x150F100u));

    // Defaults (roughly match generateThemedFloorTile, tuned for diamond space).
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

    // Small per-variant base jitter (keeps different variants from looking too similar).
    base = add(base, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // Helper: convert pixel coordinate to a stable 0..16-ish "diamond space" coordinate
    // aligned with the isometric axes (u/v).
    auto uv16 = [&](float sx, float sy) -> std::pair<float, float> {
        // Rotate 45 degrees in normalized diamond space so u/v run along the two isometric axes.
        const float a = (sx + sy) * 0.5f;
        const float b = (sx - sy) * 0.5f;
        return { (a + 1.0f) * 8.0f, (b + 1.0f) * 8.0f }; // ~[0,16]
    };

    // Helper: pick a random pixel inside the diamond interior (slightly inset so lines don't
    // immediately clip on the boundary).
    auto pickDiamondPixel = [&]() -> std::pair<int, int> {
        for (int tries = 0; tries < 96; ++tries) {
            const int x = rng.range(1, std::max(1, w - 2));
            const int y = rng.range(1, std::max(1, h - 2));
            const float sx = (static_cast<float>(x) - cx) / hw;
            const float sy = (static_cast<float>(y) - cy) / hh;
            if (std::abs(sx) + std::abs(sy) <= 0.92f) return {x, y};
        }
        return { w / 2, h / 2 };
    };

    auto applyBaseStone = [&]() {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float sx = (static_cast<float>(x) - cx) / hw; // [-1,1]
                const float sy = (static_cast<float>(y) - cy) / hh; // [-1,1]
                const float d = std::abs(sx) + std::abs(sy);
                if (d > 1.0f) continue;

                const auto [u, v] = uv16(sx, sy);

                // Coarse patching (4x4-ish in 16-space).
                int cellX = static_cast<int>(std::floor(u / 4.0f));
                int cellY = static_cast<int>(std::floor(v / 4.0f));
                cellX = std::clamp(cellX, 0, 3);
                cellY = std::clamp(cellY, 0, 3);

                const uint32_t cN = hashCombine(seed ^ (0x51F00u + sMix),
                                                static_cast<uint32_t>(cellX + cellY * 7));
                const float cell = (cN & 0xFFu) / 255.0f;
                const float cellF = 0.86f + cell * patchGain;

                // Low-ish frequency material noise in diamond space.
                const float n = valueNoise2D01(u + static_cast<float>(frame) * 0.9f,
                                               v - static_cast<float>(frame) * 0.4f,
                                               seed ^ (0xF1000u + sMix),
                                               2.2f);
                float f = cellF * (0.80f + (n - 0.5f) * noiseGain);

                // Directional light bias (top-left brighter).
                float dir = (-sx - sy) * 0.5f;
                dir = std::clamp(dir, -1.0f, 1.0f);

                // Gentle ground-plane lighting ramp + tiny per-pixel jitter so large tiles don't
                // look like a smooth gradient.
                const uint32_t pn = hashCombine(seed ^ 0x19050u,
                                                static_cast<uint32_t>((x + y * 131) ^ (frame * 17)));
                const float pNoise = (((pn & 0xFFu) / 255.0f) - 0.5f) * 0.04f; // +/-2%
                float plane = 1.0f + 0.055f * dir + pNoise;
                plane = std::clamp(plane, 0.88f, 1.12f);
                f *= plane;

                // Subtle vignette.
                f *= 1.0f - 0.08f * (sx * sx + sy * sy);

                // Edge darkening (helps tile separation without hard grid lines).
                const float edgeT = std::clamp((d - 0.84f) / 0.16f, 0.0f, 1.0f);
                f *= (1.0f - edgeDark * edgeT);

                // Shrine: faint marble vein field.
                if (style == 3u) {
                    const float vein = std::sin((u * 0.70f + v * 1.10f) + (seed & 0xFFu) * 0.08f);
                    f *= (0.98f + 0.04f * vein);
                }

                f = std::clamp(f * 0.95f, 0.0f, 1.0f);
                Color c = rampShadeTile(base, f, x, y);
                c.a = 255;
                out.at(x, y) = c;

                // Tiny animated glint along the top ridge (torch shimmer).
                if ((frame % 2 == 1) && (y <= (h / 3)) && (d > 0.86f) && (d < 0.94f)) {
                    const uint32_t hn = hashCombine(seed ^ 0x15C0u, static_cast<uint32_t>(x + y * 131));
                    if ((hn & 7u) == 0u) out.at(x, y) = add(out.at(x, y), 10, 10, 12);
                }
            }
        }
    };

    auto applyShopPlanks = [&]() {
        // Shop floors: wood planks, but aligned to isometric axes (diamond space).
        const float plankW = 2.6f;  // width of a plank in u-space (~6 planks across)
        const float seamW  = 0.12f; // seam thickness in u-space

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float sx = (static_cast<float>(x) - cx) / hw;
                const float sy = (static_cast<float>(y) - cy) / hh;
                const float d = std::abs(sx) + std::abs(sy);
                if (d > 1.0f) continue;

                const auto [u, v] = uv16(sx, sy);

                float pu = u / plankW;
                const int plank = static_cast<int>(std::floor(pu));
                const float frac = pu - static_cast<float>(plank);
                const bool seam = (frac < seamW);

                // Gentle grain running along v (plank direction).
                const uint32_t n = hashCombine(seed ^ (0xB00Du + sMix),
                                               static_cast<uint32_t>(static_cast<int>(u * 4.0f) +
                                                                     static_cast<int>(v * 4.0f) * 23 +
                                                                     frame * 101));
                const float noise = (n & 0xFFu) / 255.0f;

                const float gx = std::sin((v * 0.55f) + (static_cast<float>(plank) * 1.2f) + (seed & 0xFFu) * 0.04f);
                float f = 0.76f + gx * 0.06f + (noise - 0.5f) * noiseGain;

                // Plank-to-plank contrast.
                const float pVar = 0.96f + 0.04f * std::sin(static_cast<float>(plank) * 2.1f + (seed & 0x3Fu) * 0.2f);
                f *= pVar;

                // Seams between planks.
                if (seam) f *= 0.70f;

                // Directional light + subtle vignette.
                float dir = (-sx - sy) * 0.5f;
                dir = std::clamp(dir, -1.0f, 1.0f);
                f *= std::clamp(1.0f + 0.05f * dir, 0.85f, 1.12f);
                f *= 1.0f - 0.07f * (sx * sx + sy * sy);

                // Edge darkening.
                const float edgeT = std::clamp((d - 0.84f) / 0.16f, 0.0f, 1.0f);
                f *= (1.0f - edgeDark * edgeT);

                f = std::clamp(f, 0.0f, 1.0f);
                Color c = rampShadeTile(base, f, x, y);
                c.a = 255;
                out.at(x, y) = c;
            }
        }

        // Occasional nails / knots (subtle, deterministic).
        for (int i = 0; i < 5; ++i) {
            auto [x, y] = pickDiamondPixel();
            Color nail = mul(accent, 0.45f);
            nail = add(nail, 25, 25, 25);
            setPx(out, x, y, nail);
        }

        // Small rug hint (soft red blob) sometimes.
        if ((hash32(seed ^ 0x5A0F5u) & 1u) == 1u) {
            Color rug{ 90, 35, 35, 120 };
            if (frame % 2 == 1) rug.a = 135;

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float sx = (static_cast<float>(x) - cx) / hw;
                    const float sy = (static_cast<float>(y) - cy) / hh;
                    const float d = std::abs(sx) + std::abs(sy);
                    if (d > 1.0f) continue;

                    const auto [u, v] = uv16(sx, sy);
                    const float du = u - 8.0f;
                    const float dv = v - 8.0f;
                    if (du * du + dv * dv > 18.0f) continue;
                    blendPx(out, x, y, rug);
                }
            }
        }
    };

    if (style == 6u) {
        applyShopPlanks();
    } else {
        applyBaseStone();
    }

    // --- Style-specific overlays (diamond-space) ---
    if (style == 1u) {
        // Treasure: gold inlays + sparkles.
        Color inlay = mul(accent, 0.55f);
        inlay.a = 140;
        Color inlay2 = mul(accent, 0.35f);
        inlay2.a = 110;

        for (int i = 0; i < 3; ++i) {
            auto [x0, y0] = pickDiamondPixel();
            auto [x1, y1] = pickDiamondPixel();
            lineBlend(out, x0, y0, x1, y1, (i % 2 == 0) ? inlay : inlay2);
        }

        if (frame % 2 == 1) {
            for (int i = 0; i < 4; ++i) {
                auto [x, y] = pickDiamondPixel();
                out.at(x, y) = add(out.at(x, y), 28, 28, 18);
                setPx(out, x + 1, y, add(getPx(out, x + 1, y), 16, 16, 10));
            }
        }
    } else if (style == 2u) {
        // Lair: grime + mossy stains.
        Color stain{ 35, 60, 35, 120 };
        for (int i = 0; i < 4; ++i) {
            const float cu = static_cast<float>(rng.range(4, 12));
            const float cv = static_cast<float>(rng.range(4, 12));
            const float rr = static_cast<float>(rng.range(2, 4));

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float sx = (static_cast<float>(x) - cx) / hw;
                    const float sy = (static_cast<float>(y) - cy) / hh;
                    const float d = std::abs(sx) + std::abs(sy);
                    if (d > 1.0f) continue;

                    const auto [u, v] = uv16(sx, sy);
                    const float du = u - cu;
                    const float dv = v - cv;
                    if (du * du + dv * dv > rr * rr) continue;
                    blendPx(out, x, y, stain);
                }
            }
        }

        // Bone chips / pale grit.
        for (int i = 0; i < 10; ++i) {
            auto [x, y] = pickDiamondPixel();
            out.at(x, y) = add(out.at(x, y), 14, 12, 8);
        }
    } else if (style == 3u) {
        // Shrine: rune ring + soft glows.
        Color rune = mul(accent, 0.35f);
        rune.a = 160;
        Color rune2 = mul(accent, 0.22f);
        rune2.a = 135;

        const float r0 = 5.0f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float sx = (static_cast<float>(x) - cx) / hw;
                const float sy = (static_cast<float>(y) - cy) / hh;
                const float d = std::abs(sx) + std::abs(sy);
                if (d > 1.0f) continue;

                const auto [u, v] = uv16(sx, sy);
                const float du = u - 8.0f;
                const float dv = v - 8.0f;
                const float dist2 = du * du + dv * dv;
                if (dist2 >= (r0 * r0 - 3.0f) && dist2 <= (r0 * r0 + 3.0f)) {
                    blendPx(out, x, y, (((x + y) & 1) != 0) ? rune : rune2);
                }
            }
        }

        if (frame % 2 == 1) {
            auto [x, y] = pickDiamondPixel();
            blendPx(out, x, y, Color{ 255, 255, 255, 85 });
        }
    } else if (style == 4u) {
        // Secret: moss patches (thresholded noise) + extra cracks.
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float sx = (static_cast<float>(x) - cx) / hw;
                const float sy = (static_cast<float>(y) - cy) / hh;
                const float d = std::abs(sx) + std::abs(sy);
                if (d > 1.0f) continue;

                const auto [u, v] = uv16(sx, sy);
                const uint32_t n = hashCombine(seed ^ (0x6055u + sMix),
                                               static_cast<uint32_t>(static_cast<int>(u * 3.0f) + static_cast<int>(v * 3.0f) * 31));
                const uint8_t vv = static_cast<uint8_t>(n & 0xFFu);
                if (vv < 52u) {
                    Color moss{ 40, 80, 45, 120 };
                    if ((vv & 3u) == 0u) moss.a = 150;
                    blendPx(out, x, y, moss);
                }
            }
        }

        Color crack = mul(base, 0.50f);
        crack.a = 160;
        for (int i = 0; i < 2; ++i) {
            auto [x0, y0] = pickDiamondPixel();
            auto [x1, y1] = pickDiamondPixel();
            lineBlend(out, x0, y0, x1, y1, crack);
        }
    } else if (style == 5u) {
        // Vault: polished stone / metal seams aligned to iso axes.
        Color seam = mul(base, 0.55f);
        seam.a = 200;

        auto nearMod = [](float x, float step, float w) -> bool {
            const float m = std::fmod(x, step);
            return (m < w) || (m > step - w);
        };

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float sx = (static_cast<float>(x) - cx) / hw;
                const float sy = (static_cast<float>(y) - cy) / hh;
                const float d = std::abs(sx) + std::abs(sy);
                if (d > 1.0f) continue;

                const auto [u, v] = uv16(sx, sy);
                if (nearMod(u, 4.0f, 0.18f) || nearMod(v, 4.0f, 0.18f)) {
                    blendPx(out, x, y, seam);
                }
            }
        }

        if (frame % 2 == 1) {
            for (int i = 0; i < 3; ++i) {
                auto [x, y] = pickDiamondPixel();
                out.at(x, y) = add(out.at(x, y), 30, 30, 38);
            }
        }
    }

    // Final safety: ensure pixels outside the diamond are transparent.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float sx = (static_cast<float>(x) - cx) / hw;
            const float sy = (static_cast<float>(y) - cy) / hh;
            if ((std::abs(sx) + std::abs(sy)) > 1.0f) {
                out.at(x, y) = {0, 0, 0, 0};
            } else {
                out.at(x, y).a = 255;
            }
        }
    }

    return out;
}



SpritePixels generateIsometricChasmTile(uint32_t seed, int frame, int pxSize) {
    // Purpose-built isometric chasm tile drawn directly in diamond space.
    //
    // The top-down chasm tile looks good when projected, but it reads fairly flat in 2.5D.
    // This generator adds a thin stone rim + a shaded "inner wall" band and a deeper void
    // core, giving the eye a stronger depth cue while staying pixel-art friendly via
    // quantized ramps + ordered dithering.
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);
    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});

    RNG rng(hash32(seed));

    // Rim/wall palette (cool stone) + deep void palette (cool black).
    Color rimStone = { 52, 60, 78, 255 };
    rimStone = add(rimStone, rng.range(-8, 8), rng.range(-8, 8), rng.range(-8, 8));

    Color wallStone = add(mul(rimStone, 0.86f), -6, -6, -2);

    Color voidBase = { 10, 14, 28, 255 };
    voidBase = add(voidBase, rng.range(-2, 2), rng.range(-2, 2), rng.range(-2, 2));

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // Geometry bands in diamond-distance space (d = |nx| + |ny|).
    const float rimBand = 0.11f;            // outer stone lip thickness
    const float wallOuterD = 1.0f - rimBand; // start of the rim band
    const float innerD = 0.56f;             // start of the deep void core

    // Map pixel coords into a stable 0..16-ish design space so noise scale stays consistent
    // regardless of the requested sprite size.
    const float du = 16.0f / std::max(1.0f, static_cast<float>(w - 1));
    const float dv = 16.0f / std::max(1.0f, static_cast<float>(h - 1));

    // Seamless 4-frame drift for the void core animation.
    const float ang = phaseAngle_4(frame);
    const float driftX = std::cos(ang) * 2.4f;
    const float driftY = std::sin(ang) * 2.4f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = (static_cast<float>(x) - cx) / hw; // [-1,1]
            const float ny = (static_cast<float>(y) - cy) / hh; // [-1,1]
            const float d = std::abs(nx) + std::abs(ny);
            if (d > 1.0f) continue;

            // Light direction (top-left).
            float dir = (-nx - ny) * 0.5f;
            dir = std::clamp(dir, -1.0f, 1.0f);

            const float ux = static_cast<float>(x) * du;
            const float uy = static_cast<float>(y) * dv;

            // Small, stable per-pixel grain for "rock" breakup.
            const uint32_t hn = hashCombine(seed ^ 0xC1A500u, static_cast<uint32_t>(x + y * 131));
            const float grain = ((hn & 0xFFu) / 255.0f - 0.5f) * 0.10f;

            Color c{0, 0, 0, 0};

            if (d > wallOuterD) {
                // --- Rim (stone lip) ---
                const float t = std::clamp((d - wallOuterD) / std::max(0.001f, rimBand), 0.0f, 1.0f);

                // Slightly darker at the very boundary for a crisp silhouette.
                float shade = 0.58f + 0.16f * dir + grain * 0.8f;
                shade *= (0.92f - 0.10f * t);

                // Occasional chips.
                if ((hash32(hn ^ 0x51E1u) & 31u) == 0u) shade *= 0.80f;

                shade = std::clamp(shade, 0.0f, 1.0f);
                c = rampShadeTile(rimStone, shade, x, y);
                c.a = 255;
            } else if (d > innerD) {
                // --- Inner walls (shaded band) ---
                const float t = std::clamp((d - innerD) / std::max(0.001f, (wallOuterD - innerD)), 0.0f, 1.0f);

                // Rock breakup at a lower frequency than per-pixel grain.
                const float rock = fbm2D01(ux * 1.15f + 7.3f, uy * 1.15f - 3.1f, seed ^ 0x9111A0u);
                const float rockJ = (rock - 0.5f) * 0.14f;

                // Brighter near rim, darker toward the void core.
                float shade = 0.24f + 0.52f * t + 0.18f * dir + rockJ + grain * 0.4f;

                // Corner occlusion: deepen near diamond corners to suggest a deeper pocket.
                const float ax = std::abs(nx);
                const float ay = std::abs(ny);
                const float cornerW = 1.0f - std::clamp(std::abs(ax - ay) * 1.8f, 0.0f, 1.0f);
                shade *= (1.0f - 0.12f * (1.0f - t) * cornerW);

                // Striation bands (subtle) so the wall doesn't read like a smooth gradient.
                const float bands = std::sin((ux * 0.65f) + (uy * 0.90f) + (seed & 0xFFu) * 0.03f);
                if (bands > 0.92f) shade *= 0.84f;

                // Darken a couple of pixels right at the inner lip for separation.
                if (t < 0.10f && bayer4Threshold(x, y) > 0.35f) shade *= 0.78f;

                shade = std::clamp(shade, 0.0f, 1.0f);
                c = rampShadeTile(wallStone, shade, x, y);
                c.a = 255;
            } else {
                // --- Deep void core ---
                // Domain-warped fBm for a slow, "swirling" abyss texture.
                const float w1 = fbm2D01(ux * 0.95f + driftX + 4.3f, uy * 0.95f + driftY - 3.7f, seed ^ 0xA11CEu);
                const float w2 = fbm2D01(ux * 0.95f - driftX - 3.9f, uy * 0.95f - driftY + 4.1f, seed ^ 0xBEEFu);
                const float uu = ux + (w1 - 0.5f) * 3.2f;
                const float vv = uy + (w2 - 0.5f) * 3.2f;

                const float n = fbm2D01(uu * 1.35f, vv * 1.35f, seed ^ 0xC4A5Au);

                // Depth vignette: center is darker.
                const float t = std::clamp(d / std::max(0.001f, innerD), 0.0f, 1.0f); // 0 center .. 1 boundary
                const float center = 1.0f - t;
                float v = 0.70f - 0.18f * center * center;

                // Gentle ripple banding so the void doesn't look like static.
                const float ripple = 0.90f + 0.10f * std::sin((uu * 0.55f) + (vv * 0.35f) + (seed % 97u) * 0.05f + ang * 1.15f);

                float shade = (0.52f + n * 0.26f + grain * 0.5f) * v * ripple;

                // Tiny top-left lift so it still reads under directional lighting.
                shade += 0.05f * std::max(0.0f, dir) * (1.0f - t);

                shade = std::clamp(shade, 0.0f, 1.0f);
                c = rampShadeTile(voidBase, shade, x, y);
                c.a = 255;
            }

            out.at(x, y) = c;
        }
    }

    // Tiny "embers" / glints in the abyss (kept inside the void core so they don't
    // fight the rim shading). Instead of toggling random points, we keep deterministic candidates
    // and animate their intensity smoothly across the 4-frame cycle (reduces flicker, adds life).
    RNG sp(hash32(seed ^ 0xC4A5Au));
    const int candidates = 8;
    for (int i = 0; i < candidates; ++i) {
        const int x = sp.range(1, w - 2);
        const int y = sp.range(1, h - 2);

        const float nx = (static_cast<float>(x) - cx) / hw;
        const float ny = (static_cast<float>(y) - cy) / hh;
        const float d = std::abs(nx) + std::abs(ny);
        if (d > innerD * 0.92f) continue;

        const float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(ang * 1.9f + static_cast<float>(i) * 1.3f + static_cast<float>(seed & 0xFFu) * 0.03f));
        if (tw < 0.55f) continue;

        Color c = out.at(x, y);
        if (c.a == 0) continue;

        c = add(c,
                static_cast<int>(std::lround(15.0f * tw)),
                static_cast<int>(std::lround(18.0f * tw)),
                static_cast<int>(std::lround(30.0f * tw)));
        out.at(x, y) = c;
    }

    // Outline pass: darken pixels that sit on the diamond edge so the silhouette stays crisp.
    {
        SpritePixels edged = out;

        auto inside = [&](int xx, int yy) -> bool {
            const float sx = (static_cast<float>(xx) - cx) / hw;
            const float sy = (static_cast<float>(yy) - cy) / hh;
            return (std::abs(sx) + std::abs(sy)) <= 1.0f;
        };

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const Color c = out.at(x, y);
                if (c.a == 0) continue;

                bool edge = false;
                if (!inside(x - 1, y)) edge = true;
                else if (!inside(x + 1, y)) edge = true;
                else if (!inside(x, y - 1)) edge = true;
                else if (!inside(x, y + 1)) edge = true;

                if (!edge) continue;

                Color d = mul(c, 0.72f);
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


SpritePixels generateIsometricEdgeShadeOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize) {
    // A diamond-shaped, transparent overlay used for isometric contact shadows / chasm rims.
    // The output is a true 2:1 diamond in pixel space (w=pxSize, h=pxSize/2).
    (void)frame; // currently static (no animation)
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);

    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});

    if (mask == 0u) return out;

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // Thickness of the shaded band near the diamond edge (in normalized diamond space).
    constexpr float kEdgeBand = 0.22f;

    auto gate = [&](float v) -> float {
        // Gentle curve so the effect hugs the edge but still reaches corners.
        v = std::clamp(v, 0.0f, 1.0f);
        return std::sqrt(v);
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = (static_cast<float>(x) - cx) / hw; // [-1,1]
            const float ny = (static_cast<float>(y) - cy) / hh; // [-1,1]
            const float d = std::abs(nx) + std::abs(ny);
            if (d > 1.0f) continue;

            // Edge factor: 0 inside, 1 at the boundary.
            const float t = std::clamp((d - (1.0f - kEdgeBand)) / kEdgeBand, 0.0f, 1.0f);
            if (t <= 0.0f) continue;

            // Directional gates (light from top-left, so SE edges read slightly darker).
            float shade = 0.0f;
            if (mask & 0x01u) shade += (t * t) * gate(-ny) * 0.85f; // N
            if (mask & 0x02u) shade += (t * t) * gate(nx)  * 1.00f; // E
            if (mask & 0x04u) shade += (t * t) * gate(ny)  * 1.05f; // S
            if (mask & 0x08u) shade += (t * t) * gate(-nx) * 0.85f; // W

            // Tiny ordered-dither modulation so the gradient stays pixel-art friendly.
            const uint32_t n = hashCombine(seed ^ 0x150A0u, static_cast<uint32_t>(x + y * 131));
            const float noise = ((n & 0xFFu) / 255.0f - 0.5f) * 0.06f;

            shade = std::clamp(shade * (1.0f + noise), 0.0f, 1.0f);

            // Quantize to 4 alpha levels with ordered dithering.
            float levels = shade * 3.0f; // 0..3
            int li = static_cast<int>(std::floor(levels));
            float frac = levels - static_cast<float>(li);
            if (li < 3 && frac > bayer4Threshold(x, y)) li++;

            const uint8_t a = static_cast<uint8_t>(std::clamp((li * 255) / 3, 0, 255));

            // White RGB so renderer can tint (black shadow, blue rim, etc.).
            out.at(x, y) = {255, 255, 255, a};
        }
    }

    return out;
}


SpritePixels generateIsometricChasmGloomOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize) {
    // A diamond-shaped, transparent overlay that subtly darkens floor tiles adjacent
    // to chasms in isometric view. This extends farther inward than the thin rim/edge
    // shade band, helping pits read as deeper voids without needing hand-authored
    // transitional tiles.
    (void)frame; // static
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);

    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});
    if (mask == 0u) return out;

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // How far inward (in normalized diamond units) the gloom reaches.
    constexpr float kReach = 0.86f;
    constexpr float kMaxAlpha = 250.0f;

    auto falloff = [&](float dist) -> float {
        // 1 at the boundary (dist=0), fades to 0 by kReach.
        float t = 1.0f - (dist / kReach);
        t = std::clamp(t, 0.0f, 1.0f);
        // Softer penumbra: strong near edge, gentle fade inward.
        return t * t;
    };

    auto cornerBoost = [&](float a, float b) -> float {
        // Extra occlusion where two chasm edges meet (makes corners feel deeper).
        float c = std::min(a, b);
        c = std::clamp(c, 0.0f, 1.0f);
        return c * c;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = (static_cast<float>(x) - cx) / hw; // [-1,1]
            const float ny = (static_cast<float>(y) - cy) / hh; // [-1,1]
            const float d = std::abs(nx) + std::abs(ny);
            if (d > 1.0f) continue;

            float shade = 0.0f;

            float gN = 0.0f, gE = 0.0f, gS = 0.0f, gW = 0.0f;

            // Distances from each diamond edge, in normalized units.
            //  - N edge: ny_edge = -(1 - |nx|)  => dist = ny - ny_edge = ny + 1 - |nx|
            //  - S edge: ny_edge = +(1 - |nx|)  => dist = ny_edge - ny = 1 - |nx| - ny
            //  - E edge: nx_edge = +(1 - |ny|)  => dist = nx_edge - nx = 1 - |ny| - nx
            //  - W edge: nx_edge = -(1 - |ny|)  => dist = nx - nx_edge = nx + 1 - |ny|
            if (mask & 0x01u) { gN = falloff(ny + 1.0f - std::abs(nx)); shade += gN * 0.92f; }
            if (mask & 0x02u) { gE = falloff((1.0f - std::abs(ny)) - nx); shade += gE * 1.02f; }
            if (mask & 0x04u) { gS = falloff((1.0f - std::abs(nx)) - ny); shade += gS * 1.08f; }
            if (mask & 0x08u) { gW = falloff(nx + 1.0f - std::abs(ny)); shade += gW * 0.92f; }

            // Corner deepening where two chasm edges meet.
            if ((mask & 0x01u) && (mask & 0x02u)) shade += cornerBoost(gN, gE) * 0.55f; // NE
            if ((mask & 0x02u) && (mask & 0x04u)) shade += cornerBoost(gE, gS) * 0.55f; // SE
            if ((mask & 0x04u) && (mask & 0x08u)) shade += cornerBoost(gS, gW) * 0.55f; // SW
            if ((mask & 0x08u) && (mask & 0x01u)) shade += cornerBoost(gW, gN) * 0.55f; // NW

            // Subtle directional bias (light from top-left): bottom-right feels slightly deeper.
            float dir = (-nx - ny) * 0.5f; // [-1,1]
            dir = std::clamp(dir, -1.0f, 1.0f);
            const float unlit = std::clamp((-dir + 1.0f) * 0.5f, 0.0f, 1.0f); // 0 bright .. 1 dark
            shade *= (0.92f + 0.22f * unlit);

            // Keep the diamond silhouette crisp: reduce a hair right at the boundary.
            shade *= (0.90f + 0.10f * (1.0f - std::clamp((d - 0.86f) / 0.14f, 0.0f, 1.0f)));

            // Tiny ordered-dither modulation so the gradient stays pixel-art friendly.
            const uint32_t n = hashCombine(seed ^ 0xC1A5F00Du, static_cast<uint32_t>(x + y * 131));
            const float noise = ((hash32(n) & 0xFFu) / 255.0f - 0.5f) * 0.10f;
            shade = std::clamp(shade * (1.0f + noise), 0.0f, 1.0f);

            // Quantize to 6 alpha levels with ordered dithering.
            float levels = shade * 5.0f; // 0..5
            int li = static_cast<int>(std::floor(levels));
            float frac = levels - static_cast<float>(li);
            if (li < 5 && frac > bayer4Threshold(x, y)) li++;

            if (li <= 0) continue;
            const uint8_t a = static_cast<uint8_t>(std::clamp((li * static_cast<int>(kMaxAlpha)) / 5, 0, 255));
            out.at(x, y) = {255, 255, 255, a};
        }
    }

    return out;
}


SpritePixels generateIsometricCastShadowOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize) {
    // A soft, directional cast shadow used on the *ground plane* in isometric view.
    // This is drawn on floor-like tiles adjacent to tall occluders (walls/closed doors/pillars/etc)
    // to reinforce verticality without requiring any new hand-authored art.
    //
    // Mask bits: 1=N, 2=E, 4=S, 8=W (bit set means "neighbor is a tall shadow caster").
    // The renderer selects which bits to set based on the global isometric light direction.
    (void)frame; // currently static (no animation)
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);

    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});
    if (mask == 0u) return out;

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // Shadow reach in normalized diamond units:
    //  - "core" is the darkest region right by the occluder
    //  - "tail" is a softer penumbra that reaches farther into the tile
    constexpr float kReachCore = 1.05f;
    constexpr float kReachTail = 1.65f;
    constexpr float kMaxAlpha = 230.0f;

    auto shadowFalloff = [&](float dist) -> float {
        // Core: sharper and stronger.
        float core = 1.0f - (dist / kReachCore);
        core = std::clamp(core, 0.0f, 1.0f);
        core = core * core;

        // Tail: broader and softer.
        float tail = 1.0f - (dist / kReachTail);
        tail = std::clamp(tail, 0.0f, 1.0f);
        tail = std::sqrt(tail);

        return core * 0.72f + tail * 0.28f;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = (static_cast<float>(x) - cx) / hw; // [-1,1]
            const float ny = (static_cast<float>(y) - cy) / hh; // [-1,1]
            const float d = std::abs(nx) + std::abs(ny);
            if (d > 1.0f) continue;

            float shade = 0.0f;
            float tN = 0.0f;
            float tE = 0.0f;
            float tS = 0.0f;
            float tW = 0.0f;

            // Shadow from a tall occluder immediately north of this tile.
            // Distance from the top diamond edge for this x: ny_edge = -(1 - |nx|).
            if (mask & 0x01u) {
                const float dist = ny + 1.0f - std::abs(nx); // 0 at boundary, ~1 at center
                shade += shadowFalloff(dist) * 0.85f;
                tN = std::clamp(1.0f - (dist / kReachTail), 0.0f, 1.0f);
            }

            // Shadow from a tall occluder immediately east of this tile.
            // Distance from the right diamond edge for this y: nx_edge = +(1 - |ny|).
            if (mask & 0x02u) {
                const float dist = 1.0f - std::abs(ny) - nx; // 0 at boundary, ~1 at center
                shade += shadowFalloff(dist) * 0.85f;
                tE = std::clamp(1.0f - (dist / kReachTail), 0.0f, 1.0f);
            }

            // Shadow from a tall occluder immediately south of this tile.
            // Distance from the bottom diamond edge for this x: ny_edge = +(1 - |nx|).
            if (mask & 0x04u) {
                const float dist = 1.0f - std::abs(nx) - ny; // 0 at boundary, ~1 at center
                shade += shadowFalloff(dist) * 0.85f;
                tS = std::clamp(1.0f - (dist / kReachTail), 0.0f, 1.0f);
            }

            // Shadow from a tall occluder immediately west of this tile.
            // Distance from the left diamond edge for this y: nx_edge = -(1 - |ny|).
            if (mask & 0x08u) {
                const float dist = nx + 1.0f - std::abs(ny); // 0 at boundary, ~1 at center
                shade += shadowFalloff(dist) * 0.85f;
                tW = std::clamp(1.0f - (dist / kReachTail), 0.0f, 1.0f);
            }

            // Extra occlusion in tight inner corners. Makes corridors feel grounded.
            auto cornerBoost = [&](float a, float b) {
                float corner = std::min(a, b);
                corner = corner * corner;
                return corner;
            };

            if ((mask & 0x01u) && (mask & 0x08u)) shade += cornerBoost(tN, tW) * 0.55f; // NW
            if ((mask & 0x01u) && (mask & 0x02u)) shade += cornerBoost(tN, tE) * 0.55f; // NE
            if ((mask & 0x04u) && (mask & 0x02u)) shade += cornerBoost(tS, tE) * 0.55f; // SE
            if ((mask & 0x04u) && (mask & 0x08u)) shade += cornerBoost(tS, tW) * 0.55f; // SW

            shade = std::clamp(shade, 0.0f, 1.0f);

            // Soft falloff so the shadow reads like lighting rather than a hard band.
            shade *= shade;

            // Preserve a crisp tile silhouette: slightly reduce shadow right at the diamond boundary.
            shade *= (0.85f + 0.15f * (1.0f - std::clamp((d - 0.65f) / 0.35f, 0.0f, 1.0f)));

            // Tiny ordered-dither modulation so the gradient stays pixel-art friendly.
            const uint32_t n = hashCombine(seed ^ 0xCA57u, static_cast<uint32_t>(x + y * 131));
            const float noise = ((n & 0xFFu) / 255.0f - 0.5f) * 0.10f;
            shade = std::clamp(shade * (1.0f + noise), 0.0f, 1.0f);

            // Quantize to 6 alpha levels with ordered dithering.
            float levels = shade * 5.0f; // 0..5
            int li = static_cast<int>(std::floor(levels));
            float frac = levels - static_cast<float>(li);
            if (li < 5 && frac > bayer4Threshold(x, y)) li++;

            if (li <= 0) continue;

            const uint8_t a = static_cast<uint8_t>(std::clamp((li * static_cast<int>(kMaxAlpha)) / 5, 0, 255));

            // White RGB so the renderer can tint it (typically black).
            out.at(x, y) = {255, 255, 255, a};
        }
    }

    return out;
}



SpritePixels generateIsometricEntityShadowOverlay(uint32_t seed, uint8_t lightDir, int frame, int pxSize) {
    // A small, soft diamond shadow used to anchor sprites to the ground plane in
    // isometric view. This improves depth/readability without requiring per-entity
    // authored shadows or expensive lighting.
    (void)frame; // static
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);

    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});

    // Shadow shape parameters.
    // We keep it smaller than the full tile diamond so it reads like a footprint
    // shadow rather than 'darkening the tile'.
    constexpr float kInner = 0.76f;
    constexpr float kMaxAlpha = 230.0f;

    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // Bias the shadow slightly away from the light direction.
    // lightDir encoding (from the renderer):
    //   0 = light from NW, 1 = light from NE, 2 = light from SE, 3 = light from SW
    // Shadows fall in the opposite direction.
    const float ox = static_cast<float>(w) * 0.06f;
    const float oy = static_cast<float>(h) * 0.14f;

    float dx = 0.0f;
    float dy = 0.0f;
    switch (lightDir & 0x03u) {
        default:
        case 0: dx = +ox; dy = +oy; break; // NW light -> SE shadow
        case 1: dx = -ox; dy = +oy; break; // NE light -> SW shadow
        case 2: dx = -ox; dy = -oy; break; // SE light -> NW shadow
        case 3: dx = +ox; dy = -oy; break; // SW light -> NE shadow
    }

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f + dx;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f + dy;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = (static_cast<float>(x) - cx) / hw;
            const float ny = (static_cast<float>(y) - cy) / hh;

            float d = (std::abs(nx) + std::abs(ny)) / kInner;
            if (d > 1.0f) continue;

            // t=1 at the center, 0 at the boundary.
            float t = std::clamp(1.0f - d, 0.0f, 1.0f);

            // Sharpen the center but keep a soft falloff.
            float a = t * t;

            // Tiny pixel-noise modulation so the falloff doesn't look like a smooth ramp
            // when upscaled.
            const uint32_t n = hashCombine(seed ^ 0x5AD0F00u, static_cast<uint32_t>(x + y * 131));
            const float noise = ((hash32(n) & 0xFFu) / 255.0f - 0.5f) * 0.10f;
            a = std::clamp(a * (1.0f + noise), 0.0f, 1.0f);

            // Quantize to a few alpha levels (ordered dithering) to stay pixel-art friendly.
            float levels = a * 4.0f; // 0..4
            int li = static_cast<int>(std::floor(levels));
            float frac = levels - static_cast<float>(li);
            if (li < 4 && frac > bayer4Threshold(x, y)) li++;

            if (li <= 0) continue;
            const uint8_t alpha = static_cast<uint8_t>(std::clamp((li * static_cast<int>(kMaxAlpha)) / 4, 0, 255));

            // White RGB so the renderer can tint it (typically black).
            out.at(x, y) = {255, 255, 255, alpha};
        }
    }

    return out;
}



SpritePixels generateIsometricStairsOverlay(uint32_t seed, bool up, int frame, int pxSize) {
    // A purpose-built isometric (diamond) stairwell overlay.
    //
    // In earlier versions we simply projected the top-down stair overlay into a
    // diamond. That works, but it tends to read a bit "flat" in 2.5D view.
    // This generator draws directly in diamond space and uses rim + interior
    // shading (with ordered dithering) so stairs feel more like a feature in the
    // ground plane.
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);
    SpritePixels out = makeSprite(w, h, {0, 0, 0, 0});

    // Per-type seed salt so up/down stairs differ even if called with the same seed.
    const uint32_t salt = up ? 0x515A1u : 0x515A2u;
    RNG rng(hash32(seed ^ salt));

    // Stone palette for the rim/steps.
    Color stone = {185, 175, 155, 255};
    stone = add(stone, rng.range(-12, 12), rng.range(-12, 12), rng.range(-12, 12));

    // Dark interior for "stairs down".
    Color holeBase = {28, 28, 36, 255};
    holeBase = add(holeBase, rng.range(-4, 4), rng.range(-4, 4), rng.range(-4, 4));

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    // Geometry in normalized diamond distance (d=|nx|+|ny|).
    // We intentionally leave a small outer margin so the underlying themed floor
    // still frames the stairwell.
    const float outerD = up ? 0.90f : 0.92f;
    const float innerD = up ? 0.82f : 0.70f; // inner area (steps / hole)
    const float shadowBand = up ? 0.06f : 0.00f;

    // Helper: write a pixel only if it's inside the tile diamond.
    auto inDiamond = [&](int px, int py) -> bool {
        const float nx = (static_cast<float>(px) - cx) / hw;
        const float ny = (static_cast<float>(py) - cy) / hh;
        return (std::abs(nx) + std::abs(ny)) <= 1.0f;
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float nx = (static_cast<float>(x) - cx) / hw; // [-1,1]
            const float ny = (static_cast<float>(y) - cy) / hh; // [-1,1]
            const float d = std::abs(nx) + std::abs(ny);
            if (d > 1.0f) continue;

            // Global light direction (top-left => brighter where (-nx - ny) is positive).
            float dir = (-nx - ny) * 0.5f;
            dir = std::clamp(dir, -1.0f, 1.0f);

            // Tiny stable noise so surfaces don't look like a flat fill when upscaled.
            const uint32_t n = hashCombine(seed ^ 0x57A1F5u ^ salt, static_cast<uint32_t>(x + y * 131 + frame * 17));
            const float noise = ((n & 0xFFu) / 255.0f - 0.5f) * 0.08f;

            if (up) {
                if (d > (outerD + shadowBand)) continue;

                // Soft shadow ring behind the raised steps (adds contact / depth).
                if (d > outerD) {
                    const float t = std::clamp((d - outerD) / std::max(0.001f, shadowBand), 0.0f, 1.0f);
                    const uint8_t a = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(90.0f * (1.0f - t))), 0, 120));
                    out.at(x, y) = {0, 0, 0, a};
                    continue;
                }

                // Step surface.
                if (d <= outerD) {
                    float shade = 0.64f + 0.18f * dir + noise;

                    // Step stripes (descending toward bottom-right).
                    const float sv = (nx + ny + 1.0f) * 0.5f; // 0..1
                    const float steps = sv * 6.0f;
                    const float frac = steps - std::floor(steps);
                    if (frac < 0.09f) shade *= 0.78f;          // edge line
                    else if (frac > 0.92f) shade *= 0.90f;     // soft second line

                    // Slight extra highlight near the top-left rim.
                    if (d > (outerD - 0.10f) && dir > 0.15f) {
                        const float t = std::clamp((d - (outerD - 0.10f)) / 0.10f, 0.0f, 1.0f);
                        shade += 0.06f * t;
                    }

                    shade = std::clamp(shade, 0.0f, 1.0f);
                    Color c = rampShadeTile(stone, shade, x, y);
                    c.a = 255;
                    out.at(x, y) = c;
                }
            } else {
                // Stairs down: a rim + dark interior hole.
                if (d > outerD) continue;

                if (d > innerD) {
                    // Rim band.
                    const float t = std::clamp((d - innerD) / std::max(0.001f, (outerD - innerD)), 0.0f, 1.0f);
                    float shade = 0.58f + 0.22f * dir + noise * 0.6f;

                    // Make the inner edge a touch darker so the lip reads as a drop.
                    shade *= (0.92f - 0.10f * (1.0f - t));

                    shade = std::clamp(shade, 0.0f, 1.0f);
                    Color c = rampShadeTile(stone, shade, x, y);
                    c.a = 255;

                    // Darken a few pixels right at the inner edge (dithered) to increase separation.
                    if (t < 0.18f && (bayer4Threshold(x, y) > 0.25f)) {
                        c = mul(c, 0.78f);
                        c.a = 255;
                    }
                    out.at(x, y) = c;
                } else {
                    // Interior hole.
                    const float t = std::clamp(d / std::max(0.001f, innerD), 0.0f, 1.0f);
                    float shade = 0.30f + 0.10f * dir + 0.22f * t + noise * 0.5f;
                    shade = std::clamp(shade, 0.0f, 1.0f);
                    Color c = rampShadeTile(holeBase, shade, x, y);
                    c.a = 255;

                    // Subtle "step" highlights inside the hole (fades with depth).
                    const float sv = (nx + ny + 1.0f) * 0.5f; // 0..1
                    const float steps = sv * 6.0f;
                    const float frac = steps - std::floor(steps);
                    if (frac < 0.07f) {
                        const float lift = (1.0f - sv) * 0.9f;
                        c = add(c, static_cast<int>(std::lround(12.0f * lift)), static_cast<int>(std::lround(12.0f * lift)), static_cast<int>(std::lround(14.0f * lift)));
                        c.a = 255;
                    }

                    out.at(x, y) = c;
                }
            }
        }
    }

    // Arrow hint (blink) so stairs are easy to spot even on noisy floors.
    // Keep it small and centered so it doesn't fight the room decals.
    const uint8_t arrowA = static_cast<uint8_t>((frame % 2 == 0) ? 200 : 230);
    const Color arrow = up ? Color{120, 255, 120, arrowA} : Color{255, 120, 120, arrowA};

    const int ax = w / 2;
    const int ay = h / 2;
    const int ah = std::max(3, h / 3);
    const int aw = std::max(2, w / 12);

    auto put = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= w || py >= h) return;
        if (!inDiamond(px, py)) return;

        // Only stamp the arrow where the overlay already has pixels.
        if (out.at(px, py).a == 0) return;

        out.at(px, py) = arrow;
    };

    if (up) {
        // Up arrow: stem + head.
        for (int i = 0; i < ah; ++i) put(ax, ay + (ah / 2) - i);
        for (int i = 0; i < aw + 1; ++i) {
            put(ax - i, ay - (ah / 2) + i);
            put(ax + i, ay - (ah / 2) + i);
        }
    } else {
        // Down arrow.
        for (int i = 0; i < ah; ++i) put(ax, ay - (ah / 2) + i);
        for (int i = 0; i < aw + 1; ++i) {
            put(ax - i, ay + (ah / 2) - i);
            put(ax + i, ay + (ah / 2) - i);
        }
    }

    return out;
}



// --- Isometric block sprite polish -------------------------------------------------
//
// The 2.5D wall/door/pillar "block" sprites are rendered as taller sprites above the
// diamond ground plane. Because they're procedurally generated, a small amount of
// extra AO + rim-lighting goes a long way toward making them read as solid volume.
//
// These helpers operate on the 16x16 design grid sprites (before upscale) and are
// intentionally subtle + ordered-dithered so they stay crisp when upscaled.
static void applyIsoBlockVerticalFaceAO(SpritePixels& s,
                                        uint32_t /*seed*/,
                                        int /*frame*/,
                                        int startY,
                                        float ridgeX,
                                        float ridgeWidth,
                                        float overhangDark,
                                        float baseDark,
                                        float ridgeDark) {
    const int W = s.w;
    const int H = s.h;
    if (W <= 0 || H <= 0) return;

    startY = std::clamp(startY, 0, H);

    for (int y = startY; y < H; ++y) {
        // 0 at the top of the vertical face region, 1 a few pixels below it.
        const float topT = std::clamp((static_cast<float>(y) - static_cast<float>(startY)) / 3.0f, 0.0f, 1.0f);
        // 0 above the last few rows, 1 at the bottom.
        const float botT = std::clamp((static_cast<float>(y) - static_cast<float>(H - 4)) / 3.0f, 0.0f, 1.0f);

        for (int x = 0; x < W; ++x) {
            Color c = s.at(x, y);
            if (c.a != 255) continue; // only affect solid pixels (keep semi-transparent cutouts as-is)

            float ao = 1.0f;

            // Under-cap overhang shadow (strongest right under the top face).
            ao *= (1.0f - overhangDark * (1.0f - topT));

            // Grounding near the base (slightly darker at the bottom).
            ao *= (1.0f - baseDark * botT);

            // Inner corner between faces (ridge). Darken pixels near the seam so it reads as depth.
            const float dc = std::abs(static_cast<float>(x) - ridgeX);
            if (dc < ridgeWidth) {
                const float t = 1.0f - (dc / std::max(0.001f, ridgeWidth));
                ao *= (1.0f - ridgeDark * t * t);
            }

            // Tiny ordered-dither jitter so the AO doesn't read like a smooth gradient when upscaled.
            const float thr = bayer4Threshold(x, y);
            const float jitter = (thr - 0.5f) * 0.04f; // +/- 2%
            ao = std::clamp(ao * (1.0f + jitter), 0.0f, 1.0f);

            s.at(x, y) = mul(c, ao);
        }
    }
}

static void applyIsoTopRimHighlight(SpritePixels& s,
                                   int topYMax,
                                   float cx, float cy,
                                   float hw, float hh,
                                   float rimStart,
                                   float rimWidth,
                                   int dr, int dg, int db) {
    const int W = s.w;
    const int H = s.h;
    if (W <= 0 || H <= 0) return;

    topYMax = std::clamp(topYMax, 0, H);
    hw = std::max(0.001f, hw);
    hh = std::max(0.001f, hh);
    rimWidth = std::max(0.001f, rimWidth);

    for (int y = 0; y < topYMax; ++y) {
        for (int x = 0; x < W; ++x) {
            Color c = s.at(x, y);
            if (c.a != 255) continue;

            const float sx = (static_cast<float>(x) - cx) / hw;
            const float sy = (static_cast<float>(y) - cy) / hh;
            const float d = std::abs(sx) + std::abs(sy);
            if (d > 1.0f) continue;

            const float edgeT = std::clamp((d - rimStart) / rimWidth, 0.0f, 1.0f);
            if (edgeT <= 0.0f) continue;

            // Light from top-left, so favor the NW-ish rim.
            float dir = (-sx - sy) * 0.5f;
            dir = std::clamp(dir, 0.0f, 1.0f);

            const float w = edgeT * dir;
            if (w <= 0.0f) continue;

            // Ordered-dither the highlight so it stays crisp and pixel-art friendly.
            if (w > bayer4Threshold(x, y)) {
                s.at(x, y) = add(c, dr, dg, db);
            }
        }
    }
}


static void applyIsoStoneBrickPattern(SpritePixels& s,
                                      uint32_t seed,
                                      int startY,
                                      int ridgeX,
                                      float seamMul = 0.82f) {
    const int W = s.w;
    const int H = s.h;
    if (W <= 0 || H <= 0) return;

    startY = std::clamp(startY, 0, H);
    if (startY >= H) return;
    ridgeX = std::clamp(ridgeX, 0, W);

    // The iso block sprites are tiny (16x16 design grid). We fake brick/mortar seams
    // by darkening a few pixels in a face-aligned coordinate system, then ordered-dither
    // the result so it stays crisp when upscaled to 32/64/128/256.
    for (int y = startY; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            Color c = s.at(x, y);
            if (c.a != 255) continue;

            const bool leftFace = (x < ridgeX);

            int u = 0;
            int v = 0;
            int stepU = 0;
            int stepV = 0;
            uint32_t salt = 0;

            if (leftFace) {
                // Left face top edge is roughly along (8,3).
                u = 8 * x + 3 * y;
                v = -3 * x + 8 * y;
                stepU = 32; // ~4px along the edge direction
                stepV = 24; // ~3px perpendicular to the edge direction
                salt = 0x51E7u;
            } else {
                // Right face top edge is roughly along (7,-3).
                u = 7 * x - 3 * y;
                v = 3 * x + 7 * y;
                stepU = 28; // ~4px along the edge direction
                stepV = 21; // ~3px perpendicular to the edge direction
                salt = 0x51E8u;
            }

            const int row = v / std::max(1, stepV);
            int rowOffset = (row & 1) ? (stepU / 2) : 0;

            // Small deterministic jitter so the seam layout doesn't look perfectly grid-like.
            const int jitter = static_cast<int>((hashCombine(seed ^ salt, static_cast<uint32_t>(row)) & 3u)) - 1;
            rowOffset += jitter * 2;

            int ru = (u + rowOffset) % stepU;
            int rv = v % stepV;
            if (ru < 0) ru += stepU;
            if (rv < 0) rv += stepV;

            const int seamU = leftFace ? 4 : 3;
            const int seamV = leftFace ? 4 : 3;

            const bool horiz = (rv < seamV) || (rv > (stepV - seamV));
            const bool vert  = (ru < seamU) || (ru > (stepU - seamU));

            if (!(horiz || vert)) {
                // Very subtle chips/speckles so large faces don't read as flat fills.
                const uint32_t n = hashCombine(seed ^ 0xC4C4u, static_cast<uint32_t>(x + y * 37));
                if ((n & 0xFFu) < 5u) {
                    const bool dark = ((n >> 8) & 1u) == 0;
                    s.at(x, y) = mul(c, dark ? 0.92f : 1.06f);
                }
                continue;
            }

            float w = 0.0f;
            if (horiz) w = std::max(w, 0.98f);
            if (vert)  w = std::max(w, 0.72f);
            if (horiz && vert) w = 1.0f;

            if (w > bayer4Threshold(x, y)) {
                s.at(x, y) = mul(c, seamMul);
            }
        }
    }
}

static void applyIsoWoodGrain(SpritePixels& s, uint32_t seed) {
    const int W = s.w;
    const int H = s.h;
    if (W <= 0 || H <= 0) return;

    auto isWoodish = [](const Color& c) -> bool {
        if (c.a != 255) return false;
        // Wood palette tends to be warm: R > G > B with some margin.
        return (c.r > c.g + 14) && (c.g > c.b + 10) && (c.r > 70);
    };

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            Color c = s.at(x, y);
            if (!isWoodish(c)) continue;

            const uint32_t colN = hashCombine(seed ^ 0x600Du, static_cast<uint32_t>(x * 97));
            const uint8_t v = static_cast<uint8_t>(colN & 0xFFu);

            float w = 0.0f;
            float f = 1.0f;
            if (v < 28u)      { w = 0.92f; f = 0.80f; } // deep grain streak
            else if (v < 56u) { w = 0.80f; f = 0.88f; } // light grain streak
            else if (v > 236u){ w = 0.55f; f = 1.0f; }  // highlight streak (uses add)

            const float thr = bayer4Threshold(x, y);
            if (w > 0.0f && w > thr) {
                if (v > 236u) {
                    s.at(x, y) = add(c, 12, 8, 4);
                } else {
                    s.at(x, y) = mul(c, f);
                }
            }

            // Rare dents (dark pinpricks) to break up long streaks.
            const uint32_t dn = hashCombine(seed ^ 0xD3A7u, static_cast<uint32_t>(x + y * 37));
            if ((dn & 0xFFu) == 0u) {
                s.at(x, y) = mul(s.at(x, y), 0.78f);
            }
        }
    }
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

    // Stonework seams: subtle brick/mortar lines on the vertical faces for texture/readability.
    applyIsoStoneBrickPattern(s, seed, /*startY=*/topH + 1, /*ridgeX=*/8, /*seamMul=*/0.82f);

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


    // Extra depth cues on 2.5D blocks: subtle AO under the cap + inner-corner occlusion,
    // plus a light-facing rim highlight on the top face.
    applyIsoBlockVerticalFaceAO(s, seed, frame, topH, /*ridgeX=*/8.0f, /*ridgeWidth=*/2.2f,
                               /*overhangDark=*/0.18f, /*baseDark=*/0.12f, /*ridgeDark=*/0.10f);
    applyIsoTopRimHighlight(s, topH, cx, cy, hw, hh, /*rimStart=*/0.78f, /*rimWidth=*/0.22f, 10, 10, 12);

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

SpritePixels generateIsometricDoorBlockTile(uint32_t seed, bool locked, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    // Build in the 16x16 design grid, then upscale.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed));

    // Match the wall block palette so doors read as "wall geometry" in isometric mode.
    Color stoneBase = { 70, 78, 92, 255 };
    stoneBase = add(stoneBase, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));
    Color top = add(mul(stoneBase, 1.05f), 10, 10, 14);
    Color leftStone = mul(stoneBase, 0.78f);
    Color rightStone = mul(stoneBase, 0.88f);
    Color outlineC = mul(stoneBase, 0.45f);
    outlineC.a = 255;

    // Door wood palette.
    Color wood = add({140, 95, 55, 255}, rng.range(-12, 12), rng.range(-12, 12), rng.range(-12, 12));
    Color woodDark = mul(wood, 0.66f);
    Color woodHi = add(mul(wood, 1.06f), 10, 10, 12);

    const int W = 16;
    const int H = 16;
    const int topH = 8;

    auto lerpP = [&](Vec2i a, Vec2i b, float t) -> Vec2i {
        t = std::clamp(t, 0.0f, 1.0f);
        const float xf = static_cast<float>(a.x) + (static_cast<float>(b.x) - static_cast<float>(a.x)) * t;
        const float yf = static_cast<float>(a.y) + (static_cast<float>(b.y) - static_cast<float>(a.y)) * t;
        return { static_cast<int>(std::lround(xf)), static_cast<int>(std::lround(yf)) };
    };

    // Helper: scanline-fill a convex quad (same approach as the wall block generator).
    auto fillQuad = [&](Vec2i p0, Vec2i p1, Vec2i p2, Vec2i p3, const Color& c0, float shadeMul, uint32_t salt) {
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
                const float tt = (static_cast<float>(y) - static_cast<float>(y0)) / (static_cast<float>(y1 - y0));
                xInts[nInts++] = static_cast<float>(a.x) + tt * static_cast<float>(b.x - a.x);
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
                const uint32_t n = hashCombine(seed ^ salt, static_cast<uint32_t>(x + y * 37 + frame * 101));
                const float noise = (n & 0xFFu) / 255.0f;
                float f = (0.92f + noise * 0.16f) * shadeMul;
                f *= (0.94f + 0.06f * ((15.0f - static_cast<float>(y)) / 15.0f));
                Color cc = rampShadeTile(c0, f, x, y);
                cc.a = 255;
                s.at(x, y) = cc;
            }
        }
    };

    // Stone side faces.
    fillQuad({0, 4}, {8, 7}, {8, 15}, {0, 12}, leftStone, 0.95f, 0xB10Cu);
    fillQuad({15, 4}, {15, 12}, {8, 15}, {8, 7}, rightStone, 1.00f, 0xB10Du);

    // Stonework seams on the vertical faces so door frames match the wall block texture.
    applyIsoStoneBrickPattern(s, seed, /*startY=*/topH + 1, /*ridgeX=*/8, /*seamMul=*/0.82f);

    // Door panels inset into the side faces.
    // (We draw on both faces so orientation doesn't matter.)
    const Vec2i lp0{1, 5}, lp1{7, 8}, lp2{7, 14}, lp3{1, 11};
    const Vec2i rp0{9, 8}, rp1{14, 5}, rp2{14, 11}, rp3{9, 14};

    fillQuad(lp0, lp1, lp2, lp3, wood, 1.02f, 0xD00D0u);
    fillQuad(rp0, rp1, rp2, rp3, wood, 1.00f, 0xD00D1u);

    // Subtle wood grain so panels read as planks (non-animated, pixel-art friendly).
    applyIsoWoodGrain(s, seed ^ 0xD00Du);

    // Panel borders.
    line(s, lp0.x, lp0.y, lp1.x, lp1.y, woodDark);
    line(s, lp1.x, lp1.y, lp2.x, lp2.y, woodDark);
    line(s, lp2.x, lp2.y, lp3.x, lp3.y, woodDark);
    line(s, lp3.x, lp3.y, lp0.x, lp0.y, woodDark);

    line(s, rp0.x, rp0.y, rp1.x, rp1.y, woodDark);
    line(s, rp1.x, rp1.y, rp2.x, rp2.y, woodDark);
    line(s, rp2.x, rp2.y, rp3.x, rp3.y, woodDark);
    line(s, rp3.x, rp3.y, rp0.x, rp0.y, woodDark);

    // Plank seams (a couple of slanted dividers) so the door doesn't read as a flat blob.
    for (int k = 1; k <= 2; ++k) {
        const float t = static_cast<float>(k) / 3.0f;
        Vec2i a0 = lerpP(lp0, lp3, t);
        Vec2i a1 = lerpP(lp1, lp2, t);
        line(s, a0.x, a0.y, a1.x, a1.y, mul(wood, 0.82f));

        Vec2i b0 = lerpP(rp0, rp3, t);
        Vec2i b1 = lerpP(rp1, rp2, t);
        line(s, b0.x, b0.y, b1.x, b1.y, mul(wood, 0.82f));
    }

    // Knobs (gold-ish) and a tiny animated glint.
    const Color knob{200, 190, 80, 255};
    circle(s, 6, 10, 1, knob);
    circle(s, 11, 10, 1, knob);
    if (frame % 2 == 1) {
        setPx(s, 7, 9, {255,255,255,110});
        setPx(s, 12, 9, {255,255,255,95});
    }

    // Locked variant: add a tiny padlock on each face for readability.
    if (locked) {
        const Color lockBody { 210, 185, 70, 255 };
        const Color lockOutline { 120, 90, 25, 255 };
        const Color keyhole { 30, 22, 10, 255 };

        auto tinyLock = [&](int cx, int cy) {
            // Shackle
            setPx(s, cx - 1, cy - 2, lockOutline);
            setPx(s, cx,     cy - 2, lockOutline);
            setPx(s, cx + 1, cy - 2, lockOutline);
            setPx(s, cx - 1, cy - 1, lockOutline);
            setPx(s, cx + 1, cy - 1, lockOutline);

            // Body
            rect(s, cx - 1, cy, 3, 2, lockBody);
            outlineRect(s, cx - 1, cy, 3, 2, lockOutline);
            setPx(s, cx, cy + 1, keyhole);
        };

        tinyLock(5, 11);
        tinyLock(12, 11);
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


    // Same polish for doors: keep the stone cap crisp and add gentle AO grounding.
    applyIsoBlockVerticalFaceAO(s, seed, frame, topH, /*ridgeX=*/8.0f, /*ridgeWidth=*/2.2f,
                               /*overhangDark=*/0.16f, /*baseDark=*/0.10f, /*ridgeDark=*/0.08f);
    applyIsoTopRimHighlight(s, topH, cx, cy, hw, hh, /*rimStart=*/0.79f, /*rimWidth=*/0.21f, 10, 10, 12);

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

    // Small highlight on the top ridge to keep doors from looking too flat.
    if (frame % 2 == 1) {
        setPx(s, 8, 1, add(s.at(8, 1), 18, 18, 22));
        setPx(s, 9, 2, add(s.at(9, 2), 10, 10, 12));
    }

    return resampleSpriteToSize(s, pxSize);
}


SpritePixels generateIsometricDoorwayBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    // Build in the 16x16 design grid, then upscale.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed));

    // Reuse the wall/door stone palette so doorway frames feel like part of the wall geometry.
    Color stoneBase = { 70, 78, 92, 255 };
    stoneBase = add(stoneBase, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));
    Color top = add(mul(stoneBase, 1.05f), 10, 10, 14);
    Color leftStone = mul(stoneBase, 0.78f);
    Color rightStone = mul(stoneBase, 0.88f);
    Color outlineC = mul(stoneBase, 0.45f);
    outlineC.a = 255;

    const int W = 16;
    const int H = 16;
    const int topH = 8;

    // Helper: scanline-fill a convex quad (same approach as the wall/door block generator).
    auto fillQuad = [&](Vec2i p0, Vec2i p1, Vec2i p2, Vec2i p3, const Color& c0, float shadeMul, uint32_t salt) {
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
                const float tt = (static_cast<float>(y) - static_cast<float>(y0)) / (static_cast<float>(y1 - y0));
                xInts[nInts++] = static_cast<float>(a.x) + tt * static_cast<float>(b.x - a.x);
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
                const uint32_t n = hashCombine(seed ^ salt, static_cast<uint32_t>(x + y * 37 + frame * 101));
                const float noise = (n & 0xFFu) / 255.0f;
                float f = (0.92f + noise * 0.16f) * shadeMul;
                f *= (0.94f + 0.06f * ((15.0f - static_cast<float>(y)) / 15.0f));
                Color cc = rampShadeTile(c0, f, x, y);
                cc.a = 255;
                s.at(x, y) = cc;
            }
        }
    };

    // Helper: fill a quad with a semi-transparent interior shade (no noise) so the floor
    // beneath the doorway reads slightly darker (suggesting thickness/depth).
    auto fillQuadInterior = [&](Vec2i p0, Vec2i p1, Vec2i p2, Vec2i p3, uint8_t aTop, uint8_t aBot) {
        const Vec2i pts[4] = { p0, p1, p2, p3 };
        int minY = pts[0].y, maxY = pts[0].y;
        for (int i = 1; i < 4; ++i) {
            minY = std::min(minY, pts[i].y);
            maxY = std::max(maxY, pts[i].y);
        }
        minY = std::clamp(minY, 0, H - 1);
        maxY = std::clamp(maxY, 0, H - 1);

        const int denom = std::max(1, maxY - minY);

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
                const float tt = (static_cast<float>(y) - static_cast<float>(y0)) / (static_cast<float>(y1 - y0));
                xInts[nInts++] = static_cast<float>(a.x) + tt * static_cast<float>(b.x - a.x);
            }
            if (nInts < 2) continue;
            float xMin = xInts[0], xMax = xInts[0];
            for (int i = 1; i < nInts; ++i) {
                xMin = std::min(xMin, xInts[i]);
                xMax = std::max(xMax, xInts[i]);
            }
            int xi0 = std::clamp(static_cast<int>(std::floor(xMin)), 0, W - 1);
            int xi1 = std::clamp(static_cast<int>(std::ceil(xMax)), 0, W - 1);

            const int a = static_cast<int>(aTop) + (static_cast<int>(aBot) - static_cast<int>(aTop)) * (y - minY) / denom;
            Color cc{0, 0, 0, static_cast<uint8_t>(std::clamp(a, 0, 255))};

            for (int x = xi0; x <= xi1; ++x) {
                s.at(x, y) = cc;
            }
        }
    };

    // Stone side faces.
    fillQuad({0, 4}, {8, 7}, {8, 15}, {0, 12}, leftStone, 0.95f, 0xB10Cu);
    fillQuad({15, 4}, {15, 12}, {8, 15}, {8, 7}, rightStone, 1.00f, 0xB10Du);

    // Stonework seams on the vertical faces so doorway frames match wall blocks.
    applyIsoStoneBrickPattern(s, seed, /*startY=*/topH + 1, /*ridgeX=*/8, /*seamMul=*/0.82f);

    // Carve a passable doorway by shading the interior lightly (so underlying floor shows through).
    // We intentionally keep this symmetric because the roguelike door tile does not encode orientation.
    const Vec2i lp0{2, 6}, lp1{7, 9}, lp2{7, 14}, lp3{2, 11};
    const Vec2i rp0{9, 9}, rp1{13, 6}, rp2{13, 11}, rp3{9, 14};

    fillQuadInterior(lp0, lp1, lp2, lp3, /*aTop=*/28, /*aBot=*/85);
    fillQuadInterior(rp0, rp1, rp2, rp3, /*aTop=*/28, /*aBot=*/85);

    // Inner opening outlines (darker) + tiny highlight to sell the thickness.
    const Color innerEdge{0, 0, 0, 190};
    const Color innerHi{255, 255, 255, 55};

    line(s, lp0.x, lp0.y, lp1.x, lp1.y, innerEdge);
    line(s, lp1.x, lp1.y, lp2.x, lp2.y, innerEdge);
    line(s, lp2.x, lp2.y, lp3.x, lp3.y, innerEdge);
    line(s, lp3.x, lp3.y, lp0.x, lp0.y, innerEdge);

    line(s, rp0.x, rp0.y, rp1.x, rp1.y, innerEdge);
    line(s, rp1.x, rp1.y, rp2.x, rp2.y, innerEdge);
    line(s, rp2.x, rp2.y, rp3.x, rp3.y, innerEdge);
    line(s, rp3.x, rp3.y, rp0.x, rp0.y, innerEdge);

    // A couple of highlight pixels near the top of the opening.
    if (frame % 2 == 1) {
        setPx(s, 7, 9, innerHi);
        setPx(s, 9, 9, innerHi);
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

            // Subtle top-left highlight so the frame reads as 3D.
            const float lx = (15.0f - static_cast<float>(x)) / 15.0f;
            const float ly = (15.0f - static_cast<float>(y)) / 15.0f;

            float f = 0.88f + noise * 0.18f;
            f *= (0.92f + 0.08f * (0.60f * lx + 0.40f * ly));

            Color cc = rampShadeTile(top, f, x, y);
            cc.a = 255;
            s.at(x, y) = cc;
        }
    }


    // Doorway frames: a touch of AO on the vertical stone + a small rim-light on the cap.
    applyIsoBlockVerticalFaceAO(s, seed, frame, topH, /*ridgeX=*/8.0f, /*ridgeWidth=*/2.2f,
                               /*overhangDark=*/0.15f, /*baseDark=*/0.10f, /*ridgeDark=*/0.08f);
    applyIsoTopRimHighlight(s, topH, cx, cy, hw, hh, /*rimStart=*/0.79f, /*rimWidth=*/0.21f, 10, 10, 12);

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


SpritePixels generateIsometricPillarBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    // Build in the 16x16 design grid, then upscale.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed));

    // Slightly lighter stone than wall blocks so pillars pop as "props".
    Color base = { 92, 98, 112, 255 };
    base = add(base, rng.range(-12, 12), rng.range(-12, 12), rng.range(-12, 12));
    Color top = add(mul(base, 1.06f), 10, 10, 14);
    Color left = mul(base, 0.75f);
    Color right = mul(base, 0.86f);
    Color outlineC = mul(base, 0.42f);
    outlineC.a = 255;

    const int W = 16;
    const int H = 16;

    // Pillar footprint points (narrower than a full wall block).
    const Vec2i pTop{8, 1};
    const Vec2i pLeft{3, 4};
    const Vec2i pRight{13, 4};
    const Vec2i pBot{8, 7};

    const Vec2i pLeftD{3, 12};
    const Vec2i pRightD{13, 12};
    const Vec2i pBotD{8, 15};

    // Helper: scanline-fill a convex quad (same technique as wall/door blocks).
    auto fillQuad = [&](Vec2i p0, Vec2i p1, Vec2i p2, Vec2i p3, const Color& c0, float shadeMul, uint32_t salt) {
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
                const uint32_t n = hashCombine(seed ^ salt, static_cast<uint32_t>(x + y * 37 + frame * 101));
                const float noise = (n & 0xFFu) / 255.0f;
                float f = (0.92f + noise * 0.16f) * shadeMul;
                f *= (0.94f + 0.06f * ((15.0f - static_cast<float>(y)) / 15.0f));
                Color cc = rampShadeTile(c0, f, x, y);
                cc.a = 255;
                s.at(x, y) = cc;
            }
        }
    };

    // Side faces first.
    fillQuad(pLeft, pBot, pBotD, pLeftD, left, 0.98f, 0x9111A0u);
    fillQuad(pRight, pRightD, pBotD, pBot, right, 1.03f, 0x9111A1u);

    // Light vertical fluting on the faces (masked so it only affects pillar pixels).
    auto lineMasked = [&](int x0, int y0, int x1, int y1, Color c) {
        int dx = std::abs(x1 - x0);
        int sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0);
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        int x = x0;
        int y = y0;
        while (true) {
            if (x >= 0 && y >= 0 && x < W && y < H) {
                if (s.at(x, y).a != 0) s.at(x, y) = c;
            }
            if (x == x1 && y == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; } 
            if (e2 <= dx) { err += dx; y += sy; }
        }
    };

    // We'll implement flutes by slightly darkening a few interior columns.
    const Color groove = mul(base, 0.62f);
    lineMasked(6, 6, 6, 15, groove);
    lineMasked(10, 6, 10, 15, groove);
    // Tiny highlight stripe between grooves.
    const Color hiStripe = add(mul(base, 1.02f), 12, 12, 14);
    lineMasked(8, 6, 8, 15, hiStripe);

    // Top face (small diamond) drawn last.
    const float cx = 8.0f;
    const float cy = 4.0f;
    const float hw = 5.0f;
    const float hh = 3.0f;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < W; ++x) {
            const float dx = std::abs(static_cast<float>(x) - cx) / hw;
            const float dy = std::abs(static_cast<float>(y) - cy) / hh;
            if ((dx + dy) > 1.0f) continue;

            const uint32_t n = hashCombine(seed ^ 0x9111u, static_cast<uint32_t>(x + y * 53 + frame * 97));
            const float noise = (n & 0xFFu) / 255.0f;

            // Top-left highlight so the cap reads.
            const float lx = (15.0f - static_cast<float>(x)) / 15.0f;
            const float ly = (7.0f - static_cast<float>(y)) / 7.0f;

            float f = 0.90f + noise * 0.16f;
            f *= (0.92f + 0.08f * (0.65f * lx + 0.35f * ly));

            Color cc = rampShadeTile(top, f, x, y);
            cc.a = 255;
            s.at(x, y) = cc;
        }
    }


    // Pillar polish: subtle grounding + cap rim-light so it reads as a solid column.
    applyIsoBlockVerticalFaceAO(s, seed, frame, /*startY=*/8, /*ridgeX=*/8.0f, /*ridgeWidth=*/1.8f,
                               /*overhangDark=*/0.14f, /*baseDark=*/0.14f, /*ridgeDark=*/0.07f);
    applyIsoTopRimHighlight(s, /*topYMax=*/8, cx, cy, hw, hh, /*rimStart=*/0.80f, /*rimWidth=*/0.20f, 10, 10, 12);

    // Outline edges.
    line(s, pTop.x, pTop.y, pLeft.x, pLeft.y, outlineC);
    line(s, pTop.x, pTop.y, pRight.x, pRight.y, outlineC);
    line(s, pLeft.x, pLeft.y, pBot.x, pBot.y, outlineC);
    line(s, pRight.x, pRight.y, pBot.x, pBot.y, outlineC);

    line(s, pLeft.x, pLeft.y, pLeftD.x, pLeftD.y, outlineC);
    line(s, pRight.x, pRight.y, pRightD.x, pRightD.y, outlineC);
    line(s, pBot.x, pBot.y, pBotD.x, pBotD.y, outlineC);

    line(s, pLeftD.x, pLeftD.y, pBotD.x, pBotD.y, outlineC);
    line(s, pRightD.x, pRightD.y, pBotD.x, pBotD.y, outlineC);

    // Small animated glint on the cap ridge.
    if (frame % 2 == 1) {
        setPx(s, 8, 2, add(s.at(8, 2), 18, 18, 22));
        setPx(s, 9, 3, add(s.at(9, 3), 10, 10, 12));
    }

    return resampleSpriteToSize(s, pxSize);
}

SpritePixels generateIsometricBoulderBlockTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    // Build in the 16x16 design grid, then upscale.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    RNG rng(hash32(seed));

    // Boulder palette: slightly warmer rock so it reads distinct from walls/pillars.
    Color base = { 100, 92, 82, 255 };
    base = add(base, rng.range(-16, 16), rng.range(-16, 16), rng.range(-16, 16));
    Color outlineC = mul(base, 0.44f);
    outlineC.a = 255;

    const int W = 16;
    const int H = 16;

    // Boulder is shorter than wall blocks: keep some transparent headroom so it doesn't
    // compete with walls/doors.
    const float cx = 8.0f;
    const float cy = 11.0f;
    const float rx = 5.6f;
    const float ry = 4.2f;

    // Fill a slightly irregular ellipsoid with a simple directional lighting model.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float nx = (static_cast<float>(x) - cx) / rx;
            const float ny = (static_cast<float>(y) - cy) / ry;
            const float d2 = nx * nx + ny * ny;

            // Boundary jitter for a more organic silhouette.
            const uint32_t hn = hashCombine(seed ^ 0xB011D3u, static_cast<uint32_t>(x + y * 37 + frame * 11));
            const float noise = (hn & 0xFFu) / 255.0f;
            const float jitter = (noise - 0.5f) * 0.18f;
            const float boundary = 1.0f + jitter;

            if (d2 > boundary * boundary) continue;

            // Fake "sphere" depth for shading.
            const float z = std::sqrt(std::max(0.0f, 1.0f - (d2 / std::max(0.001f, boundary * boundary))));

            // Light from top-left: brighter where (x,y) are smaller.
            const float dir = 0.5f * ((-nx) + (-ny)); // positive on top-left
            float shade = 0.55f + 0.35f * z + 0.10f * dir + (noise - 0.5f) * 0.10f;

            // Ground contact: a touch darker near the bottom.
            const float down = std::clamp((static_cast<float>(y) - (cy + 1.0f)) / 5.0f, 0.0f, 1.0f);
            shade *= (0.92f - 0.15f * down);

            shade = std::clamp(shade, 0.0f, 1.0f);
            Color c = rampShadeTile(base, shade, x, y);
            c.a = 255;
            s.at(x, y) = c;
        }
    }

    // Outline: darken boundary pixels for readability on noisy floors.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (s.at(x, y).a == 0) continue;
            bool edge = false;
            const int dx[4] = {1, -1, 0, 0};
            const int dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) {
                const int xx = x + dx[k];
                const int yy = y + dy[k];
                if (xx < 0 || yy < 0 || xx >= W || yy >= H) { edge = true; break; }
                if (s.at(xx, yy).a == 0) { edge = true; break; }
            }
            if (edge) s.at(x, y) = outlineC;
        }
    }

    // A couple of subtle cracks (masked so they only draw on boulder pixels).
    auto lineMasked = [&](int x0, int y0, int x1, int y1, Color c) {
        int dx = std::abs(x1 - x0);
        int sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0);
        int sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        int x = x0;
        int y = y0;
        while (true) {
            if (x >= 0 && y >= 0 && x < W && y < H) {
                if (s.at(x, y).a != 0) s.at(x, y) = c;
            }
            if (x == x1 && y == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x += sx; }
            if (e2 <= dx) { err += dx; y += sy; }
        }
    };

    const Color crack = mul(base, 0.55f);
    lineMasked(6, 10, 12, 13, crack);
    lineMasked(5, 12, 11, 14, mul(crack, 0.9f));

    // Tiny animated highlight on the top-left shoulder.
    if (frame % 2 == 1) {
        for (int yy = 7; yy <= 8; ++yy) {
            for (int xx = 6; xx <= 7; ++xx) {
                if (s.at(xx, yy).a != 0) s.at(xx, yy) = add(s.at(xx, yy), 18, 18, 20);
            }
        }
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


SpritePixels generateFountainTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    RNG rng(hash32(seed ^ 0xF00F7A1u));

    // Fountain is a transparent overlay layered on top of the themed floor.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // Stone basin palette (slightly varied per seed so fountains don't look identical).
    Color stone = { 138, 142, 152, 255 };
    stone = add(stone, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));
    Color light = add(mul(stone, 1.10f), 12, 12, 14);

    // Water palette.
    Color water = { 64, 132, 210, 210 };
    Color waterDark = { 42, 92, 160, 210 };
    Color waterLight = { 96, 170, 240, 220 };


    // Soft shadow on the floor under the basin.
    for (int y = 10; y < 16; ++y) {
        for (int x = 2; x < 14; ++x) {
            float cx = (x - 7.5f) / 6.0f;
            float cy = (y - 13.0f) / 2.8f;
            float d2 = cx * cx + cy * cy;
            if (d2 > 1.0f) continue;
            int a = static_cast<int>(std::lround((1.0f - d2) * 95.0f));
            a = std::clamp(a, 0, 95);
            setPx(s, x, y, Color{0, 0, 0, static_cast<uint8_t>(a)});
        }
    }

    // Basin geometry (ellipse ring + inner water pool).
    const float cx = 7.5f;
    const float cy = 8.0f;
    const float rx = 6.3f;
    const float ry = 4.6f;

    // Seamless 4-frame loop phase for the water ripple field.
    const float ang = phaseAngle_4(frame);
    const float driftX = std::cos(ang + hash01_16(seed ^ 0xF00F7A1u) * TAU) * 1.25f;
    const float driftY = std::sin(ang + hash01_16(seed ^ 0xBEEF1234u) * TAU) * 1.25f;

    for (int y = 1; y < 15; ++y) {
        for (int x = 1; x < 15; ++x) {
            float nx = (static_cast<float>(x) - cx) / rx;
            float ny = (static_cast<float>(y) - cy) / ry;
            float d2 = nx * nx + ny * ny;

            if (d2 > 1.02f) continue;

            const bool ring = (d2 > 0.72f); // outer basin wall thickness

            if (ring) {
                // Lighting: brighter toward top-left.
                float shade = 0.82f + (-nx * 0.10f) + (-ny * 0.14f);
                shade = std::clamp(shade, 0.55f, 1.12f);
                Color c = rampShadeTile(stone, shade, x, y);
                c.a = 255;

                // Darken extreme rim for definition.
                if (d2 > 0.95f) c = mul(c, 0.82f);

                setPx(s, x, y, c);
            } else {
                // Water pool: animated ripples (seamless + coherent 4-frame loop).
                // Compose a couple traveling waves with looped fBm, then gently flow-warp.
                float fx = static_cast<float>(x) + driftX;
                float fy = static_cast<float>(y) + driftY;
                flowWarp2D(fx, fy, seed ^ 0xF00F7A1u, frame, /*strength=*/1.05f, /*steps=*/2);

                const float w0 = std::sin((nx * 6.2f + ny * 5.1f) * 2.2f + ang * 2.0f);
                const float w1 = std::sin((nx * -4.8f + ny * 6.9f) * 1.9f - ang * 1.6f + hash01_16(seed) * TAU);
                float ripple = (w0 * 0.50f + w1 * 0.50f) * 0.16f;

                const float n = loopFbm2D01(fx * 0.95f + 7.1f,
                                           fy * 0.95f - 3.3f,
                                           seed ^ 0xBADC0DEu,
                                           frame,
                                           /*radius=*/2.2f);
                ripple += (n - 0.5f) * 0.22f;

                // Tiny sparkles that loop instead of popping randomly.
                const float gl = loopValueNoise2D01(fx + 11.2f,
                                                   fy - 9.7f,
                                                   seed ^ 0x51A11u,
                                                   /*period=*/5.0f,
                                                   frame,
                                                   /*radius=*/1.7f);
                if (gl > 0.92f) ripple += (gl - 0.92f) * 0.60f;

                // Slightly stronger movement nearer the center.
                const float r = std::sqrt(std::max(0.0f, d2));
                ripple *= (0.70f + 0.30f * (1.0f - r));

                float t = 0.55f + ripple;
                t = std::clamp(t, 0.0f, 1.0f);

                Color c = lerp(waterDark, water, t);
                const float hl = std::clamp(0.18f + ripple, 0.0f, 0.60f);
                c = lerp(c, waterLight, hl);
                c.a = water.a;

                // Slight highlight near top-left.
                if (x < 7 && y < 7) c = add(c, 8, 10, 12);

                setPx(s, x, y, c);
            }
        }
    }

    // Small central spout / sparkle (4-frame pulse).
    {
        const int ph = (frame & 3);
        int addR = 10, addG = 12, addB = 14;
        int addR2 = 0, addG2 = 0, addB2 = 0;

        switch (ph) {
            default:
            case 0: addR = 10; addG = 12; addB = 14; addR2 = 0;  addG2 = 0;  addB2 = 0;  break; // idle
            case 1: addR = 18; addG = 22; addB = 26; addR2 = 10; addG2 = 12; addB2 = 14; break; // bright
            case 2: addR = 14; addG = 18; addB = 22; addR2 = 6;  addG2 = 8;  addB2 = 10; break; // mid
            case 3: addR = 8;  addG = 10; addB = 12; addR2 = 0;  addG2 = 0;  addB2 = 0;  break; // dim
        }

        setPx(s, 8, 6, add(s.at(8, 6), addR, addG, addB));
        if (addR2 > 0 || addG2 > 0 || addB2 > 0) {
            setPx(s, 7, 7, add(s.at(7, 7), addR2, addG2, addB2));
        }
    }

    // Crisp outline on the basin rim.
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
            if (edgePx && s.at(x, y).a == 255) {
                setPx(s, x, y, mul(s.at(x, y), 0.88f));
            }
        }
    }

    // Highlight stroke on the top-left rim.
    line(s, 3, 7, 5, 5, light);
    line(s, 4, 8, 6, 6, add(light, -12, -12, -12));

    return resampleSpriteToSize(s, pxSize);
}


SpritePixels generateAltarTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    RNG rng(hash32(seed ^ 0xA17A12u));

    // Altar is a transparent overlay layered on top of the themed floor.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // Stone palette (slight per-seed variation).
    Color stone = { 150, 152, 162, 255 };
    stone = add(stone, rng.range(-10, 10), rng.range(-10, 10), rng.range(-10, 10));
    Color dark = mul(stone, 0.72f);
    Color light = add(mul(stone, 1.10f), 10, 10, 12);

    // Soft shadow under the altar (helps it read on noisy floors).
    for (int y = 9; y < 16; ++y) {
        for (int x = 2; x < 14; ++x) {
            float cx = (x - 7.5f) / 6.0f;
            float cy = (y - 12.8f) / 3.2f;
            float d2 = cx * cx + cy * cy;
            if (d2 > 1.0f) continue;
            int a = static_cast<int>(std::lround((1.0f - d2) * 80.0f));
            a = std::clamp(a, 0, 80);
            setPx(s, x, y, Color{0, 0, 0, static_cast<uint8_t>(a)});
        }
    }

    // Top slab (slightly wider than the base).
    for (int y = 6; y <= 8; ++y) {
        for (int x = 3; x <= 12; ++x) {
            float shade = 0.95f + (8 - y) * 0.03f + (7 - x) * 0.01f;
            shade = std::clamp(shade, 0.75f, 1.15f);
            Color c = rampShadeTile(stone, shade, x, y);
            c.a = 255;
            setPx(s, x, y, c);
        }
    }

    // Base block.
    for (int y = 9; y <= 12; ++y) {
        for (int x = 4; x <= 11; ++x) {
            float shade = 0.82f + (9 - y) * 0.02f + (7 - x) * 0.01f;
            shade = std::clamp(shade, 0.60f, 1.05f);
            Color c = rampShadeTile(stone, shade, x, y);
            c.a = 255;
            setPx(s, x, y, c);
        }
    }

    // Crisp outlines.
    outlineRect(s, 3, 6, 10, 3, mul(dark, 0.95f));
    outlineRect(s, 4, 9, 8, 4, mul(dark, 0.92f));

    // Subtle etched rune glow (seamless 4-frame loop).
    // Uses a tiny reaction-diffusion field as a base, then drifts/warps the
    // sampling coordinates so the runes feel alive without popping.
    {
        const uint32_t rseed = hash32(seed ^ 0xA17A12u ^ 0x5EEDBEEFu);
        const RDField rd = makeRDSigilField(rseed, /*iters=*/112);

        const float ang = phaseAngle_4(frame);
        const float driftX = std::cos(ang + hash01_16(rseed) * TAU) * 1.10f;
        const float driftY = std::sin(ang + hash01_16(rseed >> 11) * TAU) * 1.10f;

        // Apply only to the carved stone (top slab + front face).
        for (int y = 6; y <= 12; ++y) {
            for (int x = 3; x <= 12; ++x) {
                if (s.at(x, y).a == 0) continue;

                float fx = static_cast<float>(x) + driftX;
                float fy = static_cast<float>(y) + driftY;

                // Slight swirl to avoid looking like a rigid scrolling texture.
                flowWarp2D(fx, fy, rseed ^ 0xF105EEDu, frame, /*strength=*/0.70f, /*steps=*/2);

                const float g = rdGradMag(rd, fx * 0.90f, fy * 0.90f);
                float line = smoothstepEdge(0.040f, 0.125f, g);

                // Fade near the altar's edges so it doesn't look "printed".
                const float ex = std::min(static_cast<float>(x), 15.0f - static_cast<float>(x));
                const float ey = std::min(static_cast<float>(y), 15.0f - static_cast<float>(y));
                float edgeFade = std::min(ex, ey) / 3.0f;
                edgeFade = std::clamp(edgeFade, 0.0f, 1.0f);

                const float pulse = 0.60f + 0.40f * std::cos(ang * 1.6f + static_cast<float>(x + y) * 0.35f);
                line *= edgeFade * pulse;

                if (line > 0.001f) {
                    Color cur = s.at(x, y);

                    // Warm holy glow etched into the stone.
                    const int dr = static_cast<int>(std::lround(line * 24.0f));
                    const int dg = static_cast<int>(std::lround(line * 20.0f));
                    const int db = static_cast<int>(std::lround(line * 10.0f));

                    setPx(s, x, y, add(cur, dr, dg, db));
                }
            }
        }
    }

    // Cloth runner on top.
    Color cloth = { 150, 45, 55, 235 };
    if (frame % 2 == 1) cloth = add(cloth, 6, 2, 2);
    rect(s, 5, 7, 6, 1, cloth);
    rect(s, 6, 6, 4, 1, mul(cloth, 0.92f));

    // Simple holy symbol (gold cross) on the cloth.
    Color gold = { 220, 190, 70, 245 };
    if (frame % 2 == 1) gold = add(gold, 10, 8, 0);
    setPx(s, 8, 6, gold);
    setPx(s, 8, 7, gold);
    setPx(s, 8, 8, gold);
    setPx(s, 7, 7, gold);
    setPx(s, 9, 7, gold);

    // Candles (two small ones) with flickering flame.
    Color wax = { 235, 230, 220, 255 };
    Color flame = { 255, 170, 60, 240 };
    if (frame % 2 == 1) flame = add(flame, 0, 25, 20);

    auto candle = [&](int x, int y) {
        setPx(s, x, y, wax);
        setPx(s, x, y - 1, flame);
        if (frame % 2 == 1) {
            setPx(s, x, y - 2, Color{255, 240, 140, 180});
        }
    };

    candle(5, 5);
    candle(11, 5);

    // Highlight stroke on top-left rim.
    line(s, 4, 7, 6, 6, light);
    line(s, 5, 8, 7, 7, add(light, -12, -12, -12));

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

    // Smooth, *seamless* 4-frame animation: we drift the noise sampling point around a
    // circle in noise-space, which avoids harsh per-frame flicker.
    const float ang = phaseAngle_4(frame);
    const float driftX = std::cos(ang) * 2.2f;
    const float driftY = std::sin(ang) * 2.2f;

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);

            // Coherent grain (fBm) + a gentle moving band, both looped.
            const float n = fbm2D01(fx * 0.95f + driftX + 7.1f,
                                    fy * 0.95f + driftY - 3.3f,
                                    seed ^ (0xA11CEu + t * 177u));
            float f = 0.70f + (n - 0.5f) * 0.42f; // ~0.49..0.91

            const float band = 0.92f + 0.08f * std::sin((fx * 0.85f + fy * 0.33f) + ang * 1.35f + (seed & 0xFFu) * 0.10f);
            f *= band;

            // Add a second, tiny drifting component so large panels don't read as a static loop.
            const float n2 = loopValueNoise2D01(fx + 1.7f, fy - 2.3f,
                                                seed ^ (0xBEEFu + t * 13u),
                                                /*period=*/5.0f,
                                                frame,
                                                /*radius=*/1.6f);
            f += (n2 - 0.5f) * 0.06f;

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

            // Slight pulse so cracks feel like they're catching shifting torchlight.
            const float p = 0.88f + 0.12f * std::cos(ang + static_cast<float>(i) * 1.7f);
            line(s, x0, y0, x1, y1, mul(accent, 0.25f * p));
        }
    } else if (theme == UITheme::Parchment) {
        // Fibers.
        const float p = 0.85f + 0.15f * std::cos(ang);
        for (int i = 0; i < 6; ++i) {
            int x = rng.range(0, 15);
            int y = rng.range(0, 15);
            int len = rng.range(3, 7);
            for (int j = 0; j < len; ++j) {
                int yy = std::clamp(y + j, 0, 15);
                setPx(s, x, yy, mul(accent, 0.18f * p));
            }
        }
    } else { // Arcane
        // Organic rune field using a tiny reaction-diffusion simulation (Gray-Scott).
        // We animate it by drifting/wrapping sampling coordinates around a circle
        // (seamless 4-frame loop) and adding a gentle curl-noise flow warp.
        const uint32_t rseed = hash32(seed ^ (0xA11CE5u + t * 991u));
        const RDField rd = makeRDSigilField(rseed, /*iters=*/96);

        const float dX = std::cos(ang + hash01_16(rseed) * TAU) * 1.15f;
        const float dY = std::sin(ang + hash01_16(rseed >> 9) * TAU) * 1.15f;

        for (int y = 1; y < 15; ++y) {
            for (int x = 1; x < 15; ++x) {
                float fx = static_cast<float>(x) + dX;
                float fy = static_cast<float>(y) + dY;

                // Swirl the rune field a bit so it feels "alive" (still loops because
                // flowWarp2D is looped, and dX/dY form a loop over 4 frames).
                flowWarp2D(fx, fy, rseed ^ 0xF105EEDu, frame, /*strength=*/0.85f, /*steps=*/2);

                const float g = rdGradMag(rd, fx * 0.85f, fy * 0.85f);
                float line = smoothstepEdge(0.035f, 0.115f, g);

                // Keep it low-contrast so UI text remains readable.
                const float p = 0.70f + 0.30f * std::cos(ang * 1.35f + (static_cast<float>(x) - static_cast<float>(y)) * 0.22f);
                line *= p;

                if (line > 0.001f) {
                    Color cur = s.at(x, y);
                    const int dr = static_cast<int>(std::lround(line * 18.0f));
                    const int dg = static_cast<int>(std::lround(line * 10.0f));
                    const int db = static_cast<int>(std::lround(line * 26.0f));
                    s.at(x, y) = add(cur, dr, dg, db);
                }
            }
        }

        // A few rune "nodes" with a traveling spark (reads as magic circuitry).
        Color rune = mul(accent, 0.28f);
        rune.a = 220;
        Color rune2 = mul(accent, 0.18f);
        rune2.a = 200;

        const float pulse = 0.70f + 0.30f * std::cos(ang);
        Color runeP = mul(rune, pulse);
        runeP.a = rune.a;
        Color rune2P = mul(rune2, 0.85f + 0.15f * std::sin(ang + 1.3f));
        rune2P.a = rune2.a;

        const int dots = 8;
        std::vector<Vec2i> pos;
        pos.reserve(static_cast<size_t>(dots));
        for (int i = 0; i < dots; ++i) {
            int x = rng.range(2, 13);
            int y = rng.range(2, 13);
            pos.push_back({x, y});

            // Modulate node brightness by local line strength so nodes tend to land
            // on the more interesting parts of the field.
            const float gg = rdGradMag(rd, static_cast<float>(x), static_cast<float>(y));
            const float w = 0.65f + 0.35f * smoothstepEdge(0.030f, 0.115f, gg);

            Color c = (i % 2 == 0) ? runeP : rune2P;
            c = mul(c, w);
            c.a = (i % 2 == 0) ? runeP.a : rune2P.a;
            setPx(s, x, y, c);
        }

        // Hop the spark between every-other node so it "travels" instead of flashing randomly.
        if (!pos.empty()) {
            const int hi = ((frame & 3) * 2) % dots;
            const Vec2i p = pos[static_cast<size_t>(hi)];
            Color cur = getPx(s, p.x, p.y);
            if (cur.a != 0) {
                setPx(s, p.x, p.y, add(cur, 22, 18, 30));
            } else {
                Color spark = add(accent, 18, 12, 22);
                spark.a = static_cast<uint8_t>(110 + std::lround(70.0f * pulse));
                setPx(s, p.x, p.y, spark);
            }
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

    const float ang = phaseAngle_4(frame);
    const float pulse = 0.70f + 0.30f * std::cos(ang);

    // Tiny rune-ish mark (pulses subtly).
    Color r0 = mul(c, 0.92f * pulse);
    r0.a = c.a;
    Color r1 = mul(c2, 0.95f * pulse);
    r1.a = c2.a;

    setPx(s, 3, 3, r0);
    setPx(s, 4, 3, r1);
    setPx(s, 3, 4, r1);
    setPx(s, 5, 4, r1);

    // Traveling glint along the bracket so the corners feel "alive".
    // 4-frame loop: glint marches out from the corner, then wraps.
    const int step = (frame & 3);
    const int gx = std::min(7, 1 + step * 2);
    const int gy = std::min(7, 1 + step * 2);

    setPx(s, gx, 0, {255,255,255,110});
    setPx(s, 0, gy, {255,255,255,85});

    // A softer inner glint.
    if (gx >= 1 && gx <= 6) setPx(s, gx, 1, {255,255,255,70});
    if (gy >= 1 && gy <= 6) setPx(s, 1, gy, {255,255,255,55});

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

        case 2: { // Lair: animated biofilm shimmer + claw marks
            // This decal is intentionally *animated* across all 4 frames.
            // Rather than a binary on/off shimmer, we use a small looping drift field
            // so lair floors feel alive (spores/biofilm) without adding new tile types.

            Color moss{ 70, 140, 70, 120 };
            Color grime{ 30, 35, 28, 120 };

            // Moss clumps around edges (static per-seed so the room layout stays consistent).
            for (int i = 0; i < 26; ++i) {
                int x = (rng.chance(0.5f) ? rng.range(0, 5) : rng.range(10, 15));
                int y = rng.range(0, 15);
                if (rng.chance(0.5f)) std::swap(x, y);
                setPx(s, x, y, rng.chance(0.62f) ? moss : grime);
            }

            // A few interior specks (keeps larger tiles from looking too edge-heavy).
            for (int i = 0; i < 7; ++i) {
                int x = rng.range(2, 13);
                int y = rng.range(2, 13);
                if (rng.chance(0.55f)) setPx(s, x, y, mul(moss, 0.90f));
            }

            // Claw marks (static, but read well when highlighted by the biofilm shimmer).
            Color claw{ 20, 15, 15, 150 };
            int x0 = rng.range(2, 6);
            int y0 = rng.range(9, 13);
            for (int i = 0; i < 3; ++i) {
                int dx = 4 + i;
                line(s, x0 + dx, y0 - i, x0 + dx + 4, y0 - i - 5, claw);
            }

            // 4-frame looping drift offsets (0, +, 0, -). This guarantees a clean loop
            // while still creating motion.
            const int ph = (frame & 3);
            int ox = 0, oy = 0;
            if (ph == 1) { ox = 3; oy = 1; }
            else if (ph == 3) { ox = -3; oy = -1; }

            // Animated shimmer mask: only affects pixels already painted by this decal,
            // so it reads as wet/slimy sheen rather than random green noise.
            const uint32_t baseH = hash32(seed ^ 0xB10F11Au);
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    Color c = s.at(x, y);
                    if (c.a == 0) continue;

                    const int sx = (x + ox) & 15;
                    const int sy = (y + oy) & 15;
                    const uint32_t hv = hash32(baseH ^ static_cast<uint32_t>(sx * 73856093) ^ static_cast<uint32_t>(sy * 19349663));
                    const uint8_t r = static_cast<uint8_t>(hv & 0xFFu);

                    // Rare bright glints + more common soft sheen.
                    if (r > 246u) {
                        setPx(s, x, y, add(c, 10, 34, 16));
                    } else if (r > 232u && ((x + y + ph) & 1) == 0) {
                        setPx(s, x, y, add(c, 5, 18, 9));
                    }
                }
            }
            break;
        }

        case 3: { // Shrine: rotating runes (cool glow)
            Color rune{ 160, 210, 255, 150 };
            Color rune2{ 120, 170, 255, 120 };

            // 4-frame pulse (brighter at frame 1, dimmer at frame 3).
            const int ph = (frame & 3);
            float pulse = 1.0f;
            if (ph == 1) pulse = 1.18f;
            else if (ph == 3) pulse = 0.92f;
            rune = mul(rune, pulse);
            rune2 = mul(rune2, pulse);
            rune.a = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(rune.a) + ((ph == 1) ? 35 : (ph == 3 ? -18 : 0)), 90, 220));
            rune2.a = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(rune2.a) + ((ph == 1) ? 25 : (ph == 3 ? -12 : 0)), 70, 200));

            // Central sigil.
            int cx = 8 + rng.range(-1, 1);
            int cy = 8 + rng.range(-1, 1);
            circle(s, cx, cy, 4, rune2);
            circle(s, cx, cy, 3, rune);

            // Rotating rune marks: pick base indices deterministically, then rotate by a
            // quarter-turn each frame (12 points, shift by 3 => full loop in 4 frames).
            static constexpr int RING_N = 12;
            static const Vec2i ring[RING_N] = {
                { 0, -5 }, { 3, -4 }, { 5, -2 }, { 5,  0 }, { 5,  2 }, { 3,  4 },
                { 0,  5 }, { -3, 4 }, { -5, 2 }, { -5, 0 }, { -5,-2 }, { -3,-4 }
            };

            bool used[RING_N]{};
            const int shift = (ph * 3) % RING_N;

            for (int i = 0; i < 6; ++i) {
                int baseIdx = rng.range(0, RING_N - 1);
                for (int tries = 0; tries < 16 && used[baseIdx]; ++tries) baseIdx = rng.range(0, RING_N - 1);
                used[baseIdx] = true;

                const int idx = (baseIdx + shift) % RING_N;
                const int x = cx + ring[idx].x;
                const int y = cy + ring[idx].y;

                const uint32_t g = hash32(seed ^ static_cast<uint32_t>(baseIdx * 1337) ^ 0x51A11u);
                const int kind = static_cast<int>(g & 3u);

                // Small glyph strokes.
                switch (kind) {
                    default:
                    case 0: line(s, x, y - 1, x, y + 1, rune); break;
                    case 1: line(s, x - 1, y, x + 1, y, rune); break;
                    case 2: line(s, x - 1, y - 1, x + 1, y + 1, rune); break;
                    case 3: line(s, x - 1, y + 1, x + 1, y - 1, rune); break;
                }
                setPx(s, x, y, rune);
                if (rng.chance(0.35f)) setPx(s, x + (ring[idx].x > 0 ? 1 : -1), y, rune2);
            }

            // Center sparkle pulse (ties the animation together).
            if (ph == 1) {
                sparkle(cx, cy, Color{255, 250, 235, 185});
            } else if (ph == 2) {
                sparkle(cx, cy, Color{210, 235, 255, 150});
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


SpritePixels generateIsometricFloorDecalOverlay(uint32_t seed, uint8_t style, int frame, int pxSize) {
    // Diamond-shaped (2:1) isometric decal overlay.
    //
    // The top-down decal sprites project reasonably, but projection can introduce
    // small distortions (especially for thin lines) and tends to "fight" the 2.5D
    // grid. Generating directly in diamond space keeps decals crisp and better
    // aligned to the isometric ground plane.
    pxSize = clampSpriteSize(pxSize);

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);

    SpritePixels s = makeSprite(w, h, {0,0,0,0});
    RNG rng(hash32(seed ^ (static_cast<uint32_t>(style) * 0x9E3779B9u) ^ 0x150DEu));

    const float cx = (static_cast<float>(w) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(h) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(w) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(h) * 0.5f);

    auto diamondD = [&](int x, int y) -> float {
        const float nx = (static_cast<float>(x) - cx) / hw;
        const float ny = (static_cast<float>(y) - cy) / hh;
        return std::abs(nx) + std::abs(ny);
    };

    auto inside = [&](int x, int y, float margin) -> bool {
        return diamondD(x, y) <= (1.0f - margin);
    };

    auto pickInside = [&](float margin) -> Vec2i {
        for (int tries = 0; tries < 200; ++tries) {
            const int x = rng.range(0, w - 1);
            const int y = rng.range(0, h - 1);
            if (inside(x, y, margin)) return {x, y};
        }
        return {w / 2, h / 2};
    };

    // Scale "stroke thickness" a bit for very large tile sizes.
    const int thick = (w >= 192) ? 3 : (w >= 96) ? 2 : 1;

    auto drawThickLine = [&](int x0, int y0, int x1, int y1, Color c) {
        line(s, x0, y0, x1, y1, c);
        if (thick >= 2) line(s, x0, y0 + 1, x1, y1 + 1, c);
        if (thick >= 3) line(s, x0 + 1, y0, x1 + 1, y1, c);
    };

    auto sparkle = [&](int x, int y, Color c) {
        setPx(s, x, y, c);
        setPx(s, x-1, y, mul(c, 0.75f));
        setPx(s, x+1, y, mul(c, 0.75f));
        setPx(s, x, y-1, mul(c, 0.75f));
        setPx(s, x, y+1, mul(c, 0.75f));
    };

    // style mapping (kept in renderer):
    //  0 = Generic, 1 = Treasure, 2 = Lair, 3 = Shrine, 4 = Secret, 5 = Vault, 6 = Shop
    switch (style) {
        default:
        case 0: { // Generic: cracks + pebbles + occasional wet shimmer
            Color crack{ 10, 10, 14, 0 };
            crack.a = static_cast<uint8_t>(110 + rng.range(0, 70));

            for (int i = 0; i < 2; ++i) {
                Vec2i p0 = pickInside(0.18f);

                const bool slopePos = rng.chance(0.5f); // +/- 0.5 slope (tile edges)
                int len = rng.range(std::max(6, w / 4), std::max(10, w / 2));
                len = (len / 2) * 2; // even so dy=dx/2 stays integral

                int dx = rng.chance(0.5f) ? len : -len;
                int dy = slopePos ? (dx / 2) : (-dx / 2);

                drawThickLine(p0.x, p0.y, p0.x + dx, p0.y + dy, crack);

                // small offshoot (cross direction)
                if (rng.chance(0.55f)) {
                    int len2 = std::max(4, len / 3);
                    len2 = (len2 / 2) * 2;
                    int dx2 = rng.chance(0.5f) ? len2 : -len2;
                    int dy2 = slopePos ? (-dx2 / 2) : (dx2 / 2);

                    Color c2 = crack;
                    c2.a = static_cast<uint8_t>(std::max<int>(20, (crack.a * 70) / 100));
                    drawThickLine(p0.x, p0.y, p0.x + dx2, p0.y + dy2, c2);
                }
            }

            // pebble specks (biased toward center so we don't clutter the rim).
            const int pebbles = (w >= 96) ? 16 : 12;
            for (int i = 0; i < pebbles; ++i) {
                Vec2i p = pickInside(0.12f);
                Color pcol{
                    static_cast<uint8_t>(110 + rng.range(-12, 12)),
                    static_cast<uint8_t>(105 + rng.range(-12, 12)),
                    static_cast<uint8_t>(95 + rng.range(-12, 12)),
                    static_cast<uint8_t>(55 + rng.range(0, 80))
                };
                setPx(s, p.x, p.y, pcol);
                if (thick >= 2 && rng.chance(0.35f)) setPx(s, p.x + 1, p.y, mul(pcol, 0.85f));
            }

            // occasional wet spot shimmer
            if (frame % 2 == 1 && rng.chance(0.35f)) {
                Vec2i p = pickInside(0.25f);
                Color wcol{ 90, 140, 190, 70 };
                setPx(s, p.x, p.y, wcol);
                setPx(s, p.x + 1, p.y, mul(wcol, 0.80f));
                setPx(s, p.x, p.y + 1, mul(wcol, 0.80f));
                setPx(s, p.x - 1, p.y, mul(wcol, 0.70f));
            }
            break;
        }

        case 1: { // Treasure: gold inlay + sparkles
            Color gold{ 235, 200, 120, 160 };
            Color gold2 = mul(gold, 0.70f); gold2.a = 140;

            // thin filigree strokes aligned to the diamond edges
            for (int k = 0; k < 2; ++k) {
                Vec2i p0 = pickInside(0.22f);
                const bool slopePos = (k == 0);
                int len = rng.range(std::max(10, w / 3), std::max(14, w / 2));
                len = (len / 2) * 2;
                int dx = rng.chance(0.5f) ? len : -len;
                int dy = slopePos ? (dx / 2) : (-dx / 2);

                drawThickLine(p0.x, p0.y, p0.x + dx, p0.y + dy, gold);
                if (rng.chance(0.55f)) drawThickLine(p0.x, p0.y + 1, p0.x + dx, p0.y + dy + 1, gold2);
            }

            // coin-ish dot
            Vec2i c0 = pickInside(0.30f);
            int r = std::max(1, w / 18);
            circle(s, c0.x, c0.y, r + 1, gold2);
            circle(s, c0.x, c0.y, r, gold);

            // sparkle pulse
            if (frame % 2 == 1) {
                Vec2i sp = pickInside(0.25f);
                sparkle(sp.x, sp.y, Color{255, 245, 200, 180});
            }
            break;
        }

        case 2: { // Lair: animated biofilm shimmer + claw marks
            Color moss{ 70, 140, 70, 120 };
            Color grime{ 30, 35, 28, 120 };

            const int specks = (w >= 96) ? 34 : 26;
            for (int i = 0; i < specks; ++i) {
                Vec2i p = pickInside(0.0f);
                if (diamondD(p.x, p.y) < 0.68f && rng.chance(0.75f)) continue; // edge bias
                setPx(s, p.x, p.y, rng.chance(0.6f) ? moss : grime);
                if (rng.chance(0.35f)) setPx(s, p.x + 1, p.y, mul(grime, 0.75f));
            }

            // claw marks: three parallel slashes
            Color claw{ 20, 15, 15, 150 };
            Vec2i p0 = pickInside(0.28f);
            p0.y = std::max(p0.y, h / 2); // keep them in the lower half
            for (int i = 0; i < 3; ++i) {
                const int ox = (i * 2) + 1;
                const int oy = i;
                drawThickLine(p0.x + ox, p0.y - oy, p0.x + ox + std::max(6, w / 6), p0.y - oy - std::max(3, h / 4), claw);
            }

            // Animated shimmer: 4-frame looping drift offsets (0, +, 0, -).
            const int ph = (frame & 3);
            const int scX = std::max(1, w / 16);
            const int scY = std::max(1, h / 16);
            int ox = 0, oy = 0;
            if (ph == 1) { ox = 3 * scX; oy = 2 * scY; }
            else if (ph == 3) { ox = -3 * scX; oy = -2 * scY; }

            const uint32_t baseH = hash32(seed ^ 0xB10F11Au ^ 0x150DEu);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    Color c = s.at(x, y);
                    if (c.a == 0) continue;
                    const uint32_t hv = hash32(baseH ^ static_cast<uint32_t>((x + ox) * 73856093) ^ static_cast<uint32_t>((y + oy) * 19349663));
                    const uint8_t r = static_cast<uint8_t>(hv & 0xFFu);
                    if (r > 248u) {
                        setPx(s, x, y, add(c, 10, 34, 16));
                    } else if (r > 236u && ((x + y + ph) & 1) == 0) {
                        setPx(s, x, y, add(c, 5, 18, 9));
                    }
                }
            }
            break;
        }

        case 3: { // Shrine: rotating runes (cool glow)
            Color rune{ 160, 210, 255, 150 };
            Color rune2{ 120, 170, 255, 120 };

            const int ph = (frame & 3);
            float pulse = 1.0f;
            if (ph == 1) pulse = 1.18f;
            else if (ph == 3) pulse = 0.92f;
            rune = mul(rune, pulse);
            rune2 = mul(rune2, pulse);
            rune.a = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(rune.a) + ((ph == 1) ? 35 : (ph == 3 ? -18 : 0)), 90, 220));
            rune2.a = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(rune2.a) + ((ph == 1) ? 25 : (ph == 3 ? -12 : 0)), 70, 200));

            // Central sigil ring (slightly elliptical in tile pixel aspect).
            const float r0 = 0.18f;
            const float r1 = 0.27f;
            const float r2 = 0.14f;

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (diamondD(x, y) > 0.92f) continue;
                    const float nx = (static_cast<float>(x) - cx) / hw;
                    const float ny = (static_cast<float>(y) - cy) / hh;
                    const float rr = nx * nx + ny * ny;

                    if (rr > r0 * r0 && rr < r1 * r1) {
                        setPx(s, x, y, rune2);
                    } else if (rr < r2 * r2) {
                        setPx(s, x, y, rune);
                    }
                }
            }

            // Rotating rune marks: choose a few glyph anchors on an ellipse and rotate them.
            static constexpr int K = 12;
            const int shift = (ph * 3) % K;
            bool used[K]{};

            const float ringR = 0.45f;
            const int gsz = std::max(1, w / 64);

            for (int i = 0; i < 6; ++i) {
                int baseIdx = rng.range(0, K - 1);
                for (int tries = 0; tries < 16 && used[baseIdx]; ++tries) baseIdx = rng.range(0, K - 1);
                used[baseIdx] = true;

                const int idx = (baseIdx + shift) % K;
                const float ang = (static_cast<float>(idx) * 6.2831853f) / static_cast<float>(K);

                const int x = static_cast<int>(std::lround(cx + std::cos(ang) * hw * ringR));
                const int y = static_cast<int>(std::lround(cy + std::sin(ang) * hh * ringR));
                if (!inside(x, y, 0.12f)) continue;

                const uint32_t g = hash32(seed ^ static_cast<uint32_t>(baseIdx * 1337) ^ 0x51A11u);
                const int kind = static_cast<int>(g & 3u);

                // Small glyph strokes (scaled by gsz so larger tiles don't look too sparse).
                switch (kind) {
                    default:
                    case 0: line(s, x, y - gsz, x, y + gsz, rune); break;
                    case 1: line(s, x - gsz, y, x + gsz, y, rune); break;
                    case 2: line(s, x - gsz, y - gsz, x + gsz, y + gsz, rune); break;
                    case 3: line(s, x - gsz, y + gsz, x + gsz, y - gsz, rune); break;
                }
                setPx(s, x, y, rune);
                if (rng.chance(0.30f)) setPx(s, x + (gsz + 1), y, rune2);
            }

            if (ph == 1) {
                Vec2i sp = pickInside(0.22f);
                sparkle(sp.x, sp.y, Color{255, 250, 235, 185});
            }
            break;
        }

        case 4: { // Secret: dust + cobwebs (subtle)
            Color dust{ 220, 210, 200, 60 };
            Color dust2{ 200, 190, 175, 55 };

            const int topX = w / 2;
            const int topY = 0;
            const int rightX = w - 1;
            const int rightY = h / 2;
            const int botX = w / 2;
            const int botY = h - 1;
            const int leftX = 0;
            const int leftY = h / 2;

            // corner web strands (from diamond corners toward the interior)
            drawThickLine(topX, topY, topX - w / 6, topY + h / 4, dust);
            drawThickLine(topX, topY, topX + w / 6, topY + h / 4, dust);
            drawThickLine(rightX, rightY, rightX - w / 5, rightY - h / 6, dust);
            drawThickLine(leftX, leftY, leftX + w / 5, leftY - h / 6, dust);
            drawThickLine(botX, botY, botX - w / 6, botY - h / 4, dust);
            drawThickLine(botX, botY, botX + w / 6, botY - h / 4, dust);

            // drifting dust mote
            if (frame % 2 == 1) {
                Vec2i p = pickInside(0.30f);
                setPx(s, p.x, p.y, dust2);
                setPx(s, p.x + 1, p.y, Color{255,255,255,35});
            }
            break;
        }

        case 5: { // Vault: steel plate seams + rivets
            Color steel{ 200, 220, 255, 110 };
            Color rivet{ 235, 245, 255, 150 };
            Color scratch{ 40, 50, 65, 120 };

            // seam diamond ring
            const float d0 = 0.58f;
            const float d1 = 0.62f;
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float d = diamondD(x, y);
                    if (d > 1.0f) continue;
                    if (d >= d0 && d <= d1) {
                        setPx(s, x, y, steel);
                    }
                }
            }

            // rivets at the four ring corners
            const float dR = 0.60f;
            const int ryT = static_cast<int>(std::lround(cy - dR * hh));
            const int ryB = static_cast<int>(std::lround(cy + dR * hh));
            const int rxL = static_cast<int>(std::lround(cx - dR * hw));
            const int rxR = static_cast<int>(std::lround(cx + dR * hw));
            setPx(s, static_cast<int>(cx), ryT, rivet);
            setPx(s, static_cast<int>(cx), ryB, rivet);
            setPx(s, rxL, static_cast<int>(cy), rivet);
            setPx(s, rxR, static_cast<int>(cy), rivet);

            // scratches
            Vec2i a = pickInside(0.25f);
            Vec2i b = pickInside(0.25f);
            drawThickLine(a.x, a.y, b.x, b.y, scratch);

            if (frame % 2 == 1 && rng.chance(0.45f)) {
                // tiny glint
                sparkle(static_cast<int>(cx), std::max(0, ryT + 1), Color{255,255,255,120});
            }
            break;
        }

        case 6: { // Shop: rug / plank hint (small inner diamond)
            Color rug{ 170, 80, 70, 120 };
            Color border{ 235, 210, 150, 130 };

            const float inner = 0.55f;
            const float outline = 0.60f;

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float d = diamondD(x, y);
                    if (d > 1.0f) continue;

                    if (d <= inner) {
                        Color c = rug;
                        // weave pattern
                        if (((x + y + frame) % 3) == 0) c = mul(rug, 0.85f);
                        setPx(s, x, y, c);
                    } else if (d <= outline && d > (outline - 0.03f)) {
                        setPx(s, x, y, border);
                    }
                }
            }

            if (frame % 2 == 1) {
                Vec2i p = pickInside(0.35f);
                setPx(s, p.x, p.y, Color{255, 240, 220, 70});
            }
            break;
        }
    }

    // Final diamond mask: guarantee we never draw outside the silhouette.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (diamondD(x, y) > 1.0f) s.at(x, y) = {0,0,0,0};
        }
    }

    return s;
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

// Top-down wall contact shadow / ambient-occlusion overlay.
// This is a subtle black alpha gradient along edges where a floor tile touches
// a wall-mass neighbor, adding depth and improving readability in top-down view.
// Mask bits: 1=N, 2=E, 4=S, 8=W (bit set means "neighbor is a wall-mass occluder")
SpritePixels generateTopDownWallShadeOverlay(uint32_t seed, uint8_t mask, int variant, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    (void)frame;

    SpritePixels s = makeSprite(16, 16, {0,0,0,0});
    if (mask == 0u) return resampleSpriteToSize(s, pxSize);

    RNG rng(hash32(seed ^ (static_cast<uint32_t>(mask) * 0x9E3779B9u) ^ (static_cast<uint32_t>(variant) * 0x85EBCA6Bu)));

    const Color shadow = { 0, 0, 0, 255 };

    // Variants tweak thickness/roughness a bit to avoid obvious repetition.
    const int baseT = 3 + (variant & 1);                    // 3..4 pixels
    const float roughAmp = (variant & 2) ? 0.70f : 0.45f;   // boundary wobble

    auto smooth01 = [&](float t) -> float {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    auto edgeJitter = [&](uint32_t salt) -> float {
        const uint32_t h = hash32(seed ^ salt);
        return (static_cast<float>(h & 0xFFu) / 255.0f - 0.5f) * roughAmp;
    };

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            float a = 0.0f;

            // Combine edge contributions as a soft "union" (avoids double-darkening in corners).
            auto blendEdge = [&](float edgeA) {
                edgeA = std::clamp(edgeA, 0.0f, 1.0f);
                a = 1.0f - (1.0f - a) * (1.0f - edgeA);
            };

            if (mask & 0x01u) { // N (shadow along top edge)
                const float j = edgeJitter(static_cast<uint32_t>(x * 131 + variant * 17 + 0xA11CEu));
                const float d = static_cast<float>(y) - j;
                if (d < static_cast<float>(baseT)) {
                    const float t = 1.0f - (d / static_cast<float>(baseT));
                    blendEdge(smooth01(t));
                }
            }
            if (mask & 0x04u) { // S
                const float j = edgeJitter(static_cast<uint32_t>(x * 137 + variant * 19 + 0x511ADu));
                const float d = static_cast<float>(15 - y) - j;
                if (d < static_cast<float>(baseT)) {
                    const float t = 1.0f - (d / static_cast<float>(baseT));
                    blendEdge(smooth01(t));
                }
            }
            if (mask & 0x08u) { // W
                const float j = edgeJitter(static_cast<uint32_t>(y * 139 + variant * 23 + 0xB011Du));
                const float d = static_cast<float>(x) - j;
                if (d < static_cast<float>(baseT)) {
                    const float t = 1.0f - (d / static_cast<float>(baseT));
                    blendEdge(smooth01(t));
                }
            }
            if (mask & 0x02u) { // E
                const float j = edgeJitter(static_cast<uint32_t>(y * 149 + variant * 29 + 0xEAD5u));
                const float d = static_cast<float>(15 - x) - j;
                if (d < static_cast<float>(baseT)) {
                    const float t = 1.0f - (d / static_cast<float>(baseT));
                    blendEdge(smooth01(t));
                }
            }

            // Corner emphasis (contact shadow) when two walls meet.
            if ((mask & 0x09u) == 0x09u && x < 3 && y < 3) a = std::min(1.0f, a + 0.18f);      // NW
            if ((mask & 0x03u) == 0x03u && x > 12 && y < 3) a = std::min(1.0f, a + 0.16f);     // NE
            if ((mask & 0x0Cu) == 0x0Cu && x < 3 && y > 12) a = std::min(1.0f, a + 0.16f);     // SW
            if ((mask & 0x06u) == 0x06u && x > 12 && y > 12) a = std::min(1.0f, a + 0.14f);    // SE

            // Micro noise so the gradient isn't perfectly clean (still very subtle).
            if (a > 0.0f) {
                const uint32_t n = hashCombine(seed ^ 0xC0FFEEu, static_cast<uint32_t>(x + y * 17 + variant * 131));
                const float noise = (static_cast<float>(n & 0xFFu) / 255.0f);
                a *= (0.92f + noise * 0.18f);
            }

            const uint8_t aa = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(a * 255.0f)), 0, 255));
            if (aa != 0) setPxAlpha(s, x, y, shadow, aa);
        }
    }

    // A couple of tiny "nicks" near the edge so it doesn't look like a pure filter.
    for (int i = 0; i < 4; ++i) {
        int x = rng.range(0, 15);
        int y = rng.range(0, 15);
        if ((mask & 0x01u) && y < 3) setPxAlpha(s, x, y, shadow, 255);
        if ((mask & 0x04u) && y > 12) setPxAlpha(s, x, y, shadow, 255);
        if ((mask & 0x08u) && x < 3) setPxAlpha(s, x, y, shadow, 255);
        if ((mask & 0x02u) && x > 12) setPxAlpha(s, x, y, shadow, 255);
    }

    return resampleSpriteToSize(s, pxSize);
}


// Procedural confusion-gas tile: grayscale translucent cloud.
// Color/tint is applied in the renderer (so lighting affects it naturally).
SpritePixels generateConfusionGasTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // A domain-warped fBm field produces wispy, swirly shapes without needing
    // expensive fluid simulation.
    const uint32_t base = hash32(seed ^ 0xC0FF1151u);

    // Seamless 4-frame loop: drive motion from an angle step instead of a linear
    // time value so frame 3 -> frame 0 wraps without a discontinuity.
    const float ang = phaseAngle_4(frame);
    const float ca = std::cos(ang);
    const float sa = std::sin(ang);

    // Slow drift so the 4-frame animation doesn't feel static.
    const float driftX = std::sin(ang + hash01_16(base) * TAU) * 0.65f;
    const float driftY = std::cos(ang + hash01_16(base >> 7) * TAU) * 0.65f;

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float px = static_cast<float>(x) + driftX;
            const float py = static_cast<float>(y) + driftY;

            // Flow-warp the sample point along a divergence-free curl field.
            // This gives the cloud a more "advected" look than pure domain-warp.
            float fx = px;
            float fy = py;
            flowWarp2D(fx, fy, base ^ 0xF105EEDu, frame, /*strength=*/1.85f, /*steps=*/3);

            // Domain warp (two independent fields -> "swirl" impression).
            const float w1 = fbm2D01(fx * 1.10f + ca * 6.0f, fy * 1.10f - sa * 5.0f, base ^ 0xA11CEu);
            const float w2 = fbm2D01(fx * 1.10f - ca * 5.5f, fy * 1.10f + sa * 6.3f, base ^ 0xBEEFu);

            const float wx = (w1 - 0.5f) * 3.2f;
            const float wy = (w2 - 0.5f) * 3.2f;

            const float sx = fx + wx;
            const float sy = fy + wy;

            // Main density + a moving "hole" field (cuts gaps into the cloud).
            const float n = fbm2D01(sx * 1.55f + ca * 2.8f, sy * 1.55f - sa * 2.2f, base ^ 0x6A5u);
            const float holes = fbm2D01(sx * 2.15f - ca * 1.6f + 13.7f,
                                        sy * 2.15f + sa * 1.3f - 9.2f,
                                        base ^ 0xC0DEC0DEu);

            // Extra fine grain so it reads as gas at 16x16.
            const float fine = valueNoise2D01(sx * 3.0f + ca * 4.0f, sy * 3.0f - sa * 3.7f,
                                              base ^ 0x12345u, 1.75f);

            float v = (n * 0.70f + fine * 0.30f) - holes * 0.55f;

            // Gentle radial envelope (keeps tile edges from looking like hard cutouts).
            const float vx = (static_cast<float>(x) - 7.5f) / 7.5f;
            const float vy = (static_cast<float>(y) - 7.5f) / 7.5f;
            float rad = 1.0f - 0.23f * (vx*vx + vy*vy);
            rad = std::clamp(rad, 0.0f, 1.0f);

            // Shift into a nicer [0,1] range and apply the envelope.
            v = (v + 0.28f) * rad;
            v = std::clamp(v, 0.0f, 1.0f);

            // Sharper edge with ordered dithering for crisp pixel-art.
            const float edge = std::clamp((v - 0.14f) / 0.86f, 0.0f, 1.0f);
            const float thr = bayer4Threshold(x + frame * 2, y + frame * 3);
            if (edge < thr * 0.72f) continue;

            const uint8_t aa = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(edge * 195.0f)), 0, 195));

            // Slight center brightening helps it feel volumetric (tint comes from renderer).
            float center = 1.0f - 0.30f * (vx*vx + vy*vy);
            center = std::clamp(center, 0.55f, 1.0f);

            // Tiny flicker so different frames don't just "slide" the same pattern.
            const float flick = 0.94f + 0.10f * std::sin((sx + sy) * 0.35f + ang * 3.1f + (base & 0xFFu) * 0.02f);

            const uint8_t g = clamp8(static_cast<int>(std::round(225.0f * center * flick)));
            setPx(s, x, y, Color{ g, g, g, aa });
        }
    }

    return resampleSpriteToSize(s, pxSize);
}


// Isometric gas overlay (diamond-shaped, translucent).
// Generated directly in diamond space (16x8 design grid) so the animated
// cloud aligns cleanly to the 2:1 isometric grid without projection artifacts.
//
// Color/tint is applied in the renderer (lighting-aware).
SpritePixels generateIsometricGasTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    // Work in a small deterministic 16x8 design grid (a 2:1 diamond tile),
    // then upscale to (pxSize x pxSize/2) via nearest-neighbor.
    static constexpr int BASE_W = 16;
    static constexpr int BASE_H = 8;

    SpritePixels s = makeSprite(BASE_W, BASE_H, {0,0,0,0});

    const uint32_t base = hash32(seed ^ 0xC0FF1151u);
    // Seamless 4-frame loop: angle step.
    const float ang = phaseAngle_4(frame);
    const float ca = std::cos(ang);
    const float sa = std::sin(ang);

    // Slow drift so the 4-frame animation doesn't feel static.
    const float driftX = std::sin(ang + hash01_16(base) * TAU) * 0.65f;
    const float driftY = std::cos(ang + hash01_16(base >> 7) * TAU) * 0.65f;

    const float cx = (static_cast<float>(BASE_W) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(BASE_H) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(BASE_W) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(BASE_H) * 0.5f);

    for (int y = 0; y < BASE_H; ++y) {
        for (int x = 0; x < BASE_W; ++x) {
            // Diamond mask in normalized isometric-tile space.
            const float sx = (static_cast<float>(x) - cx) / hw;
            const float sy = (static_cast<float>(y) - cy) / hh;
            const float d = std::abs(sx) + std::abs(sy);
            if (d > 1.0f) continue;

            // Invert the isometric projection to get tile-plane coordinates (u,v) in [-1,1].
            // This keeps noise patterns stable relative to the floor plane rather than
            // screen-space.
            const float u = (sx + sy) * 0.5f;
            const float v = (sy - sx) * 0.5f;

            // Convert to a ~16x16 coordinate space so the noise periods match the
            // square gas generator's scale.
            float px = (u + 1.0f) * 8.0f + driftX;
            float py = (v + 1.0f) * 8.0f + driftY;

            // Flow-warp the sample point along a divergence-free curl field.
            // This makes the diamond gas overlay match the square version's
            // more advected, smoky motion.
            float fx = px;
            float fy = py;
            flowWarp2D(fx, fy, base ^ 0xF105EEDu, frame, /*strength=*/1.85f, /*steps=*/3);

            // Domain warp (two independent fields -> "swirl" impression).
            const float w1 = fbm2D01(fx * 1.10f + ca * 6.0f, fy * 1.10f - sa * 5.0f, base ^ 0xA11CEu);
            const float w2 = fbm2D01(fx * 1.10f - ca * 5.5f, fy * 1.10f + sa * 6.3f, base ^ 0xBEEFu);

            const float wx = (w1 - 0.5f) * 3.2f;
            const float wy = (w2 - 0.5f) * 3.2f;

            const float sxp = fx + wx;
            const float syp = fy + wy;

            // Main density + moving hole field (cuts gaps into the cloud).
            const float n = fbm2D01(sxp * 1.55f + ca * 2.8f, syp * 1.55f - sa * 2.2f, base ^ 0x6A5u);
            const float holes = fbm2D01(sxp * 2.15f - ca * 1.6f + 13.7f,
                                        syp * 2.15f + sa * 1.3f - 9.2f,
                                        base ^ 0xC0DEC0DEu);

            const float fine = valueNoise2D01(sxp * 3.0f + ca * 4.0f, syp * 3.0f - sa * 3.7f,
                                              base ^ 0x12345u, 1.75f);

            float den = (n * 0.70f + fine * 0.30f) - holes * 0.55f;

            // Gentle radial envelope.
            float rad = 1.0f - 0.23f * (u*u + v*v);
            rad = std::clamp(rad, 0.0f, 1.0f);

            den = (den + 0.28f) * rad;
            den = std::clamp(den, 0.0f, 1.0f);

            // Sharper edge with ordered dithering for crisp pixel-art.
            float edge = std::clamp((den - 0.14f) / 0.86f, 0.0f, 1.0f);

            // Fade a touch near the diamond boundary so it doesn't look like a hard cutout.
            if (d > 0.90f) {
                const float t = std::clamp((d - 0.90f) / 0.10f, 0.0f, 1.0f);
                edge *= (1.0f - 0.18f * t);
            }

            const float thr = bayer4Threshold(x + frame * 2, y + frame * 3);
            if (edge < thr * 0.72f) continue;

            const uint8_t aa = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(edge * 195.0f)), 0, 195));

            // Slight center brightening helps it feel volumetric (tint comes from renderer).
            float center = 1.0f - 0.30f * (u*u + v*v);
            center = std::clamp(center, 0.55f, 1.0f);

            // Tiny flicker so different frames don't just "slide" the same pattern.
            const float flick = 0.94f + 0.10f * std::sin((sxp + syp) * 0.35f + ang * 3.1f + (base & 0xFFu) * 0.02f);

            const uint8_t g = clamp8(static_cast<int>(std::round(225.0f * center * flick)));
            setPx(s, x, y, Color{ g, g, g, aa });
        }
    }

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);
    return resizeNearest(s, w, h);
}


SpritePixels generateFireTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    // A small, animated flame overlay. The renderer tints this sprite, so we keep
    // it mostly grayscale with alpha.
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    const uint32_t base = hash32(seed ^ 0xF17ECAFEu);

    auto rand01 = [&](uint32_t v) -> float {
        return hash01_16(hash32(v));
    };

    // Three flame "tongues" with slightly different phases.
    const float cx1 = 4.0f + rand01(base ^ 0xA1u) * 8.0f;
    const float cx2 = 4.0f + rand01(base ^ 0xB2u) * 8.0f;
    const float cx3 = 4.0f + rand01(base ^ 0xC3u) * 8.0f;

    const float w1  = 1.6f + rand01(base ^ 0x11u) * 2.2f;
    const float w2  = 1.4f + rand01(base ^ 0x22u) * 2.4f;
    const float w3  = 1.8f + rand01(base ^ 0x33u) * 2.0f;

    const float ph1 = rand01(base ^ 0x91u) * 6.2831853f;
    const float ph2 = rand01(base ^ 0x92u) * 6.2831853f;
    const float ph3 = rand01(base ^ 0x93u) * 6.2831853f;

    // Seamless 4-frame loop: angle step.
    const float ang = phaseAngle_4(frame);
    const float ca = std::cos(ang);
    const float sa = std::sin(ang);
    const float ca2 = std::cos(ang * 2.0f);
    const float sa2 = std::sin(ang * 2.0f);

    for (int y = 0; y < 16; ++y) {
        // y=0 top, y=15 bottom
        const float yy  = static_cast<float>(y) / 15.0f;
        const float inv = 1.0f - yy;

        // Flames are stronger toward the bottom, but still flicker above.
        float baseV = std::pow(std::max(yy, 0.02f), 0.36f);
        baseV *= (0.48f + 0.52f * yy);

        for (int x = 0; x < 16; ++x) {
            const float xx = static_cast<float>(x);

            // Flow-warp a pixel-space coordinate for the turbulence fields.
            // (Keep the geometric tongue shapes based on the unwarped `xx`.)
            float nx = xx;
            float ny = static_cast<float>(y);
            flowWarp2D(nx, ny, base ^ 0xF10F1E11u, frame, /*strength=*/1.25f, /*steps=*/2);

            // Turbulence-driven lateral drift that increases toward the top.
            const float drift = (fbm2D01(nx * 0.90f + ca * 3.2f + sa2 * 1.2f,
                                         (ny / 15.0f) * 12.0f - sa * 6.5f + ca2 * 1.1f,
                                         base ^ 0xA511u) - 0.5f) * inv * 1.25f;

            auto tongue = [&](float cx, float w, float ph) -> float {
                // More lateral wobble near the top.
                const float wobAmp = inv * 1.9f;

                const float wob = std::sin(ph + yy * 3.6f + ang) * wobAmp;
                const float c = cx + wob + drift;

                // Wider at the bottom.
                const float ww = w * (0.55f + 0.95f * yy);
                const float dx = (xx - c) / std::max(0.35f, ww);
                return std::exp(-dx * dx * 2.3f);
            };

            float v = 0.0f;
            v = std::max(v, tongue(cx1, w1, ph1));
            v = std::max(v, tongue(cx2, w2, ph2));
            v = std::max(v, tongue(cx3, w3, ph3));

            // Vertical envelope.
            v *= baseV;

            // Rising turbulence: add upward-moving noise so flames feel alive.
            const float turb = (fbm2D01(nx * 1.20f + ca * 4.2f,
                                        ny * 1.35f - sa * 10.0f,
                                        base ^ 0xB00B1Eu) - 0.5f) * (0.62f * inv);
            v += turb;

            // Carve small gaps near the top so it doesn't read as a solid blob.
            const float cut = fbm2D01(nx * 1.60f - ca * 3.1f + 19.0f,
                                      ny * 1.55f - sa * 12.0f,
                                      base ^ 0xC011AB1Eu);
            v -= (cut * 0.55f) * inv;

            // Hot core near the bottom center.
            if (yy > 0.72f) {
                const float dx = (xx - 7.5f);
                const float core = std::exp(-(dx * dx) / 6.0f) * ((yy - 0.72f) / 0.28f);
                v = std::max(v, core);
            }

            v = std::clamp(v, 0.0f, 1.0f);

            // Ordered dithering keeps the overlay from looking like a solid blob.
            const float thr = bayer4Threshold(x + frame * 2, y + frame * 3);
            if (v < thr * 0.93f) continue;

            if (v < 0.08f) continue;

            const float t = (v - 0.08f) / 0.92f;
            const int a = static_cast<int>(80 + t * 175);
            const int g = static_cast<int>(170 + t * 85);
            setPx(s, x, y, Color{ clamp8(g), clamp8(g), clamp8(g), clamp8(a) });
        }
    }

    // Tiny bright sparks near the top add motion/readability (very subtle).
    auto nearFire = [&](int sx, int sy) -> bool {
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int xx = sx + ox;
                const int yy = sy + oy;
                if (xx < 0 || yy < 0 || xx >= 16 || yy >= 16) continue;
                if (s.at(xx, yy).a > 0) return true;
            }
        }
        return false;
    };

    // Candidate sparks that animate intensity smoothly across the 4-frame loop.
    for (int i = 0; i < 4; ++i) {
        const uint32_t h = hash32(base ^ 0x51A11u ^ static_cast<uint32_t>(i * 131));
        const int sx = static_cast<int>(h % 16u);
        const int sy = static_cast<int>((h >> 8) % 6u); // top region
        if (!nearFire(sx, sy)) continue;

        const float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(ang * 2.0f + static_cast<float>(i) * 1.7f));
        if (tw < 0.55f) continue;

        const uint8_t aa = static_cast<uint8_t>(120 + static_cast<int>(std::lround(120.0f * tw)) + (h & 0x1Fu));
        setPx(s, sx, sy, Color{ 255, 255, 255, aa });
    }

    // A little dark outline helps flames read in bright rooms.
    finalizeSprite(s, hash32(base ^ 0xF17Eu), frame, /*outlineAlpha=*/90, /*shadowAlpha=*/0);
    return resampleSpriteToSize(s, pxSize);
}


// Isometric fire overlay (diamond-shaped, translucent).
// Generated directly in diamond space (16x8 design grid) so the animated flame
// aligns to the 2:1 isometric grid without a projection step.
//
// Color/tint is applied in the renderer (lighting-aware).
SpritePixels generateIsometricFireTile(uint32_t seed, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);

    static constexpr int BASE_W = 16;
    static constexpr int BASE_H = 8;

    SpritePixels s = makeSprite(BASE_W, BASE_H, {0,0,0,0});

    const uint32_t base = hash32(seed ^ 0xF17ECAFEu);

    auto rand01 = [&](uint32_t v) -> float {
        return hash01_16(hash32(v));
    };

    // Three flame "tongues" with slightly different phases.
    const float cx1 = 4.0f + rand01(base ^ 0xA1u) * 8.0f;
    const float cx2 = 4.0f + rand01(base ^ 0xB2u) * 8.0f;
    const float cx3 = 4.0f + rand01(base ^ 0xC3u) * 8.0f;

    const float w1  = 1.6f + rand01(base ^ 0x11u) * 2.2f;
    const float w2  = 1.4f + rand01(base ^ 0x22u) * 2.4f;
    const float w3  = 1.8f + rand01(base ^ 0x33u) * 2.0f;

    const float ph1 = rand01(base ^ 0x91u) * 6.2831853f;
    const float ph2 = rand01(base ^ 0x92u) * 6.2831853f;
    const float ph3 = rand01(base ^ 0x93u) * 6.2831853f;

    // Seamless 4-frame loop: angle step.
    const float ang = phaseAngle_4(frame);
    const float ca = std::cos(ang);
    const float sa = std::sin(ang);
    const float ca2 = std::cos(ang * 2.0f);
    const float sa2 = std::sin(ang * 2.0f);

    const float cx = (static_cast<float>(BASE_W) - 1.0f) * 0.5f;
    const float cy = (static_cast<float>(BASE_H) - 1.0f) * 0.5f;
    const float hw = std::max(1.0f, static_cast<float>(BASE_W) * 0.5f);
    const float hh = std::max(1.0f, static_cast<float>(BASE_H) * 0.5f);

    for (int y = 0; y < BASE_H; ++y) {
        // y=0 top, y=BASE_H-1 bottom
        const float yy  = (BASE_H <= 1) ? 1.0f : (static_cast<float>(y) / static_cast<float>(BASE_H - 1));
        const float inv = 1.0f - yy;

        // Flames are stronger toward the bottom, but still flicker above.
        float baseV = std::pow(std::max(yy, 0.02f), 0.36f);
        baseV *= (0.48f + 0.52f * yy);

        for (int x = 0; x < BASE_W; ++x) {
            // Diamond silhouette.
            const float sx = (static_cast<float>(x) - cx) / hw;
            const float sy = (static_cast<float>(y) - cy) / hh;
            const float d = std::abs(sx) + std::abs(sy);
            if (d > 1.0f) continue;

            const float xx = static_cast<float>(x);

            // Flow-warp a pixel-space coordinate for the turbulence fields.
            // We map the 16x8 design grid into the same ~0..15 range used by the
            // square fire generator so the motion feels consistent.
            float nx = xx;
            float ny = yy * 15.0f;
            flowWarp2D(nx, ny, base ^ 0xF10F1E11u, frame, /*strength=*/1.25f, /*steps=*/2);

            // Turbulence-driven lateral drift that increases toward the top.
            const float drift = (fbm2D01(nx * 0.90f + ca * 3.2f + sa2 * 1.2f,
                                         (ny / 15.0f) * 12.0f - sa * 6.5f + ca2 * 1.1f,
                                         base ^ 0xA511u) - 0.5f) * inv * 1.25f;

            auto tongue = [&](float ccx, float w, float ph) -> float {
                // More lateral wobble near the top.
                const float wobAmp = inv * 1.9f;

                const float wob = std::sin(ph + yy * 3.6f + ang) * wobAmp;
                const float c = ccx + wob + drift;

                // Wider at the bottom.
                const float ww = w * (0.55f + 0.95f * yy);
                const float dx = (xx - c) / std::max(0.35f, ww);
                return std::exp(-dx * dx * 2.3f);
            };

            float v = 0.0f;
            v = std::max(v, tongue(cx1, w1, ph1));
            v = std::max(v, tongue(cx2, w2, ph2));
            v = std::max(v, tongue(cx3, w3, ph3));

            // Vertical envelope.
            v *= baseV;

            // Rising turbulence: add upward-moving noise so flames feel alive.
            const float turb = (fbm2D01(nx * 1.20f + ca * 4.2f,
                                        ny * 1.35f - sa * 10.0f,
                                        base ^ 0xB00B1Eu) - 0.5f) * (0.62f * inv);
            v += turb;

            // Carve small gaps near the top so it doesn't read as a solid blob.
            const float cut = fbm2D01(nx * 1.60f - ca * 3.1f + 19.0f,
                                      ny * 1.55f - sa * 12.0f,
                                      base ^ 0xC011AB1Eu);
            v -= (cut * 0.55f) * inv;

            // Hot core near the bottom center.
            if (yy > 0.72f) {
                const float dx = (xx - 7.5f);
                const float core = std::exp(-(dx * dx) / 6.0f) * ((yy - 0.72f) / 0.28f);
                v = std::max(v, core);
            }

            v = std::clamp(v, 0.0f, 1.0f);

            // Fade a touch near the diamond boundary so the flame doesn't outline the tile.
            const float edgeFade = std::clamp((1.0f - d) / 0.16f, 0.0f, 1.0f);
            v *= (0.70f + 0.30f * edgeFade);

            // Ordered dithering keeps the overlay from looking like a solid blob.
            const float thr = bayer4Threshold(x + frame * 2, y + frame * 3);
            if (v < thr * 0.93f) continue;
            if (v < 0.08f) continue;

            const float t = (v - 0.08f) / 0.92f;
            const int a = static_cast<int>(80 + t * 175);
            const int g = static_cast<int>(170 + t * 85);
            setPx(s, x, y, Color{ clamp8(g), clamp8(g), clamp8(g), clamp8(a) });
        }
    }

    // Tiny bright sparks near the top add motion/readability (very subtle).
    auto nearFire = [&](int sx, int sy) -> bool {
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int xx = sx + ox;
                const int yy = sy + oy;
                if (xx < 0 || yy < 0 || xx >= BASE_W || yy >= BASE_H) continue;
                if (s.at(xx, yy).a > 0) return true;
            }
        }
        return false;
    };

    const int topRows = std::max(1, (BASE_H * 3) / 8); // ~top 3/8ths
    for (int i = 0; i < 3; ++i) {
        const uint32_t h = hash32(base ^ 0x51A11u ^ static_cast<uint32_t>(i * 131));
        const int sx = static_cast<int>(h % static_cast<uint32_t>(BASE_W));
        const int sy = static_cast<int>((h >> 8) % static_cast<uint32_t>(topRows));

        // Keep sparks inside the diamond silhouette.
        const float nx = (static_cast<float>(sx) - cx) / hw;
        const float ny = (static_cast<float>(sy) - cy) / hh;
        if (std::abs(nx) + std::abs(ny) > 1.0f) continue;

        if (!nearFire(sx, sy)) continue;

        const float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(ang * 2.0f + static_cast<float>(i) * 1.7f));
        if (tw < 0.55f) continue;

        const uint8_t aa = static_cast<uint8_t>(120 + static_cast<int>(std::lround(120.0f * tw)) + (h & 0x1Fu));
        setPx(s, sx, sy, Color{ 255, 255, 255, aa });
    }

    // A little dark outline helps flames read in bright rooms.
    finalizeSprite(s, hash32(base ^ 0xF17Eu), frame, /*outlineAlpha=*/90, /*shadowAlpha=*/0);

    // Re-mask any outline bleed so the output stays a clean diamond.
    for (int y = 0; y < BASE_H; ++y) {
        for (int x = 0; x < BASE_W; ++x) {
            const float sx = (static_cast<float>(x) - cx) / hw;
            const float sy = (static_cast<float>(y) - cy) / hh;
            if (std::abs(sx) + std::abs(sy) > 1.0f) {
                s.at(x, y) = {0,0,0,0};
            }
        }
    }

    const int w = pxSize;
    const int h = std::max(1, pxSize / 2);
    return resizeNearest(s, w, h);
}


// -----------------------------------------------------------------------------
// HUD/status icons (transparent 16x16 sprites)
// -----------------------------------------------------------------------------

SpritePixels generateEffectIcon(EffectKind kind, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    SpritePixels s = makeSprite(16, 16, {0,0,0,0});

    // 4-frame procedural HUD animation helpers.
    // Using a cosine pulse gives a smooth-in/smooth-out cycle across FRAMES=4.
    const float ang = phaseAngle_4(frame);
    const float pulse01 = 0.5f + 0.5f * std::cos(ang);   // 1.0, 0.5, 0.0, 0.5
    const float pulse02 = 0.5f + 0.5f * std::sin(ang);   // 0.5, 1.0, 0.5, 0.0

    const int wobX = (frame == 1) ? 1 : (frame == 3) ? -1 : 0; // 0, +1, 0, -1
    const int wobY = (frame == 2) ? 1 : 0;

    auto pulse = [&](Color c, int addv) {
        const int dv = static_cast<int>(std::lround(static_cast<float>(addv) * pulse01));
        return add(c, dv, dv, dv);
    };

    switch (kind) {
        case EffectKind::Poison: {
            Color g = pulse(Color{90, 235, 110, 255}, 18);
            Color dk{20, 35, 20, 255};

            const int cx = 8 + wobX;

            // Droplet (slight sway).
            circle(s, cx, 6, 3, mul(g, 0.85f));
            circle(s, cx, 7, 3, g);
            line(s, cx, 9, cx, 12, g);
            setPx(s, cx - 1, 11, mul(g, 0.80f));
            setPx(s, cx + 1, 11, mul(g, 0.80f));

            // Tiny skull eyes.
            setPx(s, cx - 1, 7, dk);
            setPx(s, cx + 1, 7, dk);

            // A drifting bubble (procedural 4-frame loop).
            const int by = 11 - (frame & 3) * 2; // 11,9,7,5
            if (by >= 3) {
                Color b = mul(g, 0.70f);
                b.a = static_cast<uint8_t>(120 + std::lround(80.0f * pulse02));
                circle(s, cx + 3, by, 1, b);
                setPx(s, cx + 3, by, add(b, 35, 35, 35));
            }
            break;
        }
        case EffectKind::Regen: {
            Color c = pulse(Color{120, 255, 140, 255}, 12);
            Color c2 = mul(c, 0.70f);

            // Plus (subtle pulse).
            rect(s, 7, 4, 2, 8, c);
            rect(s, 4, 7, 8, 2, c);

            // Heartbeat tick (tiny wobble so it doesn't look like a static stamp).
            const int dx = wobX;
            line(s, 3 + dx, 12, 6 + dx, 12, c2);
            line(s, 6 + dx, 12, 7 + dx, 10, c2);
            line(s, 7 + dx, 10, 8 + dx, 13, c2);
            line(s, 8 + dx, 13, 10 + dx, 11, c2);
            line(s, 10 + dx, 11, 13 + dx, 11, c2);
            break;
        }
        case EffectKind::Shield: {
            Color c = pulse(Color{210, 220, 235, 255}, 10);
            Color c2 = mul(c, 0.75f);

            // Shield silhouette.
            rect(s, 5, 3, 6, 8, c2);
            rect(s, 6, 2, 4, 10, c);
            line(s, 6, 12, 8, 14, c2);
            line(s, 8, 14, 10, 12, c2);

            // Shine stripe sweeps across the shield over 4 frames.
            const int sx = 6 + (frame & 3); // 6..9
            line(s, sx, 4, sx, 10, Color{255,255,255,static_cast<uint8_t>(90 + std::lround(80.0f * pulse02))});
            if ((frame & 3) == 1) setPx(s, sx + 1, 5, {255,255,255,70});
            break;
        }
        case EffectKind::Haste: {
            Color c = pulse(Color{255, 225, 120, 255}, 16);
            Color c2 = mul(c, 0.70f);

            // Lightning bolt (flickers + nudges).
            const int dx = wobX;
            line(s, 9 + dx, 2, 6 + dx, 8, c);
            line(s, 6 + dx, 8, 10 + dx, 8, c);
            line(s, 10 + dx, 8, 7 + dx, 14, c);

            // Motion ticks.
            line(s, 3, 5 + wobY, 5, 5 + wobY, c2);
            line(s, 2, 8, 5, 8, c2);
            line(s, 4, 11 - wobY, 6, 11 - wobY, c2);
            break;
        }
        case EffectKind::Vision: {
            Color c = pulse(Color{140, 220, 255, 255}, 10);
            Color c2 = mul(c, 0.70f);

            // Eye outline.
            line(s, 3, 8, 6, 5, c2);
            line(s, 6, 5, 10, 5, c2);
            line(s, 10, 5, 13, 8, c2);
            line(s, 13, 8, 10, 11, c2);
            line(s, 10, 11, 6, 11, c2);
            line(s, 6, 11, 3, 8, c2);

            // Iris dilation.
            const int r = (frame == 2) ? 1 : 2;
            circle(s, 8, 8, r, c);
            setPx(s, 8, 8, Color{20, 30, 40, 255});
            if (frame == 1) setPx(s, 9, 7, Color{255,255,255,80});
            break;
        }
        case EffectKind::Invis: {
            // Alpha pulse feels more "alive" than a hard 2-frame blink.
            const uint8_t a = static_cast<uint8_t>(150 + std::lround(70.0f * pulse01));
            Color c{190, 160, 255, a};
            Color c2 = mul(c, 0.75f);

            // Faint ghost-ish silhouette.
            circle(s, 6, 7, 2, c2);
            circle(s, 10, 7, 2, c2);
            rect(s, 5, 8, 6, 5, c);
            // cutout holes
            setPx(s, 7, 9, Color{0,0,0,0});
            setPx(s, 9, 9, Color{0,0,0,0});
            break;
        }
        case EffectKind::Web: {
            Color c = pulse(Color{230, 230, 240, 255}, 8);
            Color c2 = mul(c, 0.65f);

            // Web spokes.
            line(s, 8, 2, 8, 14, c2);
            line(s, 2, 8, 14, 8, c2);
            line(s, 3, 3, 13, 13, c2);
            line(s, 13, 3, 3, 13, c2);

            // Rings.
            circle(s, 8, 8, 5, c);
            circle(s, 8, 8, 3, c);

            // Specular crawl (a tiny highlight that moves along a ring segment).
            const int hx = 8 + ((frame & 3) == 1 ? 3 : (frame & 3) == 3 ? -3 : 0);
            const int hy = 8 + ((frame & 3) == 0 ? -3 : (frame & 3) == 2 ? 3 : 0);
            setPx(s, hx, hy, {255,255,255,static_cast<uint8_t>(70 + std::lround(70.0f * pulse02))});
            break;
        }
        case EffectKind::Confusion: {
            Color c = pulse(Color{255, 140, 255, 255}, 14);
            Color c2 = mul(c, 0.70f);

            // Spiral-ish squiggle that "orbits" around the center.
            const int dx = wobX;
            const int dy = (frame == 2) ? 1 : 0;

            line(s, 4 + dx, 8 + dy, 12 + dx, 4 + dy, c2);
            line(s, 12 + dx, 4 + dy, 10 + dx, 10 + dy, c2);
            line(s, 10 + dx, 10 + dy, 6 + dx, 12 + dy, c2);
            line(s, 6 + dx, 12 + dy, 8 + dx, 6 + dy, c2);
            setPx(s, 8 + dx, 6 + dy, c);

            // A couple sparkles that walk around the icon.
            setPx(s, 5 + dx, 6 + dy, Color{255,255,255,static_cast<uint8_t>(60 + std::lround(80.0f * pulse02))});
            setPx(s, 11 + dx, 11 + dy, Color{255,255,255,static_cast<uint8_t>(45 + std::lround(60.0f * pulse01))});
            break;
        }
        case EffectKind::Burn: {
            Color hot  = pulse(Color{255, 170, 90, 255}, 18);
            Color core = pulse(Color{255, 235, 160, 255}, 12);
            Color dk{70, 25, 10, 255};

            const int dx = wobX;

            // Flame base.
            circle(s, 8 + dx, 11, 3, mul(hot, 0.90f));
            circle(s, 8 + dx, 10, 2, hot);

            // Rising tongue.
            line(s, 8 + dx, 4, 8 + dx, 10, hot);
            circle(s, 8 + dx, 6, 2, mul(core, 0.95f));
            setPx(s, 8 + dx, 5, core);
            setPx(s, 7 + dx, 6, core);
            setPx(s, 9 + dx, 6, core);

            // Ember/spark rises and drifts.
            const int ey = 12 - (frame & 3) * 2; // 12,10,8,6
            if (ey >= 4) {
                setPx(s, 11 - dx, ey, Color{255, 255, 255, static_cast<uint8_t>(80 + std::lround(90.0f * pulse02))});
            }

            // A couple dark pixels to add contrast.
            setPx(s, 7 + dx, 12, dk);
            setPx(s, 9 + dx, 12, dk);
            break;
        }
        case EffectKind::Levitation: {
            Color c = pulse(Color{175, 205, 255, 255}, 10);
            Color c2 = mul(c, 0.70f);

            // Bob the arrow up/down over the 4-frame cycle.
            const int by = (frame == 1) ? -1 : (frame == 3) ? 1 : 0;

            line(s, 8, 3 + by, 8, 12 + by, c);
            line(s, 8, 3 + by, 5, 6 + by, c);
            line(s, 8, 3 + by, 11, 6 + by, c);

            // Wind ticks.
            line(s, 3, 11, 5, 11, c2);
            line(s, 11, 9 + wobY, 13, 9 + wobY, c2);
            break;
        }
        case EffectKind::Fear: {
            Color c = pulse(Color{255, 205, 120, 255}, 14);
            Color dk{50, 25, 10, 255};

            const int dx = wobX;

            // Exclamation mark trembles slightly.
            rect(s, 7 + dx, 3, 2, 7, c);
            setPx(s, 8 + dx, 12, c);

            // Shiver halo pulses.
            circle(s, 8, 8, 5, mul(c, 0.45f + 0.10f * pulse01));

            setPx(s, 8 + dx, 6, dk);
            setPx(s, 8 + dx, 9, dk);
            break;
        }
        case EffectKind::Hallucination: {
            // Cycle two palettes and "rotate" the star by swapping diagonal emphasis.
            Color c = pulse(Color{255, 140, 255, 255}, 18);
            Color c2 = pulse(Color{140, 220, 255, 255}, 14);

            const bool diagA = ((frame & 3) == 0) || ((frame & 3) == 2);

            line(s, 8, 2, 8, 14, mul(c, 0.75f));
            line(s, 2, 8, 14, 8, mul(c2, 0.75f));

            if (diagA) {
                line(s, 3, 3, 13, 13, mul(c2, 0.55f));
                line(s, 13, 3, 3, 13, mul(c, 0.55f));
            } else {
                line(s, 3, 3, 13, 13, mul(c, 0.55f));
                line(s, 13, 3, 3, 13, mul(c2, 0.55f));
            }

            circle(s, 8, 8, 2, add(c, 10, 10, 10));
            setPx(s, 8, 8, Color{20, 20, 30, 255});
            break;
        }
        case EffectKind::Corrosion: {
            // Acid droplet + pitted metal motif.
            Color c = pulse(Color{255, 235, 120, 255}, 18);
            Color c2 = pulse(Color{200, 255, 140, 255}, 12);
            Color dk{50, 35, 10, 255};

            const int cx = 8 + wobX;
            const int cy = 6 + wobY;

            // Droplet (shimmering).
            circle(s, cx, cy, 3, mul(c, 0.85f));
            circle(s, cx, cy + 1, 3, c);
            line(s, cx, cy + 3, cx, 13, c2);

            // Pitted "holes" that animate by shifting a pixel.
            const int ox = (frame == 1) ? 1 : 0;
            setPx(s, 5 + ox, 11, dk);
            setPx(s, 11 - ox, 12, dk);
            setPx(s, 9, 10, dk);
            // A small highlight on the droplet.
            setPx(s, cx - 1, cy, add(c, 25, 25, 25));
            setPx(s, cx, cy - 1, add(c2, 20, 20, 20));
            break;
        }
        default:
            break;
    }

    // A crisp outline helps tiny HUD icons read against textured panels.
    finalizeSprite(s, hash32(static_cast<uint32_t>(kind) ^ 0x51A11u), frame, /*outlineAlpha=*/220, /*shadowAlpha=*/0);
    return resampleSpriteToSize(s, pxSize);
}



// -----------------------------------------------------------------------------
// Cursor / targeting reticle overlay (transparent, animated)
//
// This is a *UI* overlay generated at pixel resolution (pxSize x pxSize) so the
// stroke thickness remains readable when users zoom to very large tile sizes.
//
// Animation style: a classic "marching ants" dashed outline. We implement it by
// parameterizing the reticle perimeter into a 1D index and then shifting the dash
// phase each frame. Choosing a period that is divisible by FRAMES ensures the
// 4-frame loop is seamless.
// -----------------------------------------------------------------------------

SpritePixels generateCursorReticleTile(uint32_t seed, bool isometric, int frame, int pxSize) {
    pxSize = clampSpriteSize(pxSize);
    frame &= 3;

    const int W = pxSize;
    const int H = pxSize;
    SpritePixels s = makeSprite(W, H, {0, 0, 0, 0});

    const uint32_t h = hash32(seed ^ 0xC0A51EEDu);

    // Scale dash size gently with resolution (avoid huge chunky dashes at 256px).
    const int scale = std::clamp(pxSize / 96, 1, 4); // 16..95=>1, 96..191=>1, 192..287=>2, etc

    // Pick one of a few base periods (all divisible by 4) and scale it.
    const int baseSel = static_cast<int>(h & 3u);
    int basePeriod = 8;
    if (baseSel == 1) basePeriod = 12;
    else if (baseSel == 2) basePeriod = 16;
    else if (baseSel == 3) basePeriod = 8;

    const int period = std::max(4, basePeriod * scale);
    const int duty = std::clamp((period * 5) / 8, 2, period - 1); // ~62% on
    const int step = period / 4; // ensures 4-frame loop closes
    const int offset = (frame * step) % period;

    // Glow band thickness (inner ring) and crosshair thickness.
    const int glowT = std::clamp(pxSize / 64, 1, 5);
    const int crossT = std::clamp(pxSize / 96, 1, 3);

    const float ang = phaseAngle_4(frame);
    const float pulse = 0.80f + 0.20f * std::cos(ang);

    const uint8_t brightA = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(210 + std::lround(30.0f * pulse)), 0, 255));
    const uint8_t dimA    = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(70 + (h & 15u) + std::lround(12.0f * pulse)), 0, 255));
    const uint8_t glowA0  = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(26 + (h & 7u) * 2 + std::lround(10.0f * pulse)), 0, 255));

    auto put = [&](int x, int y, uint8_t a) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        Color& c = s.at(x, y);
        if (a <= c.a) return;
        c = {255, 255, 255, a};
    };

    // Build an ordered list of perimeter pixels (so we can march along it).
    std::vector<Vec2i> per;

    if (!isometric) {
        per.reserve(static_cast<size_t>(W * 4));

        // Clockwise perimeter order.
        for (int x = 0; x < W; ++x) per.push_back({x, 0});
        for (int y = 1; y < H - 1; ++y) per.push_back({W - 1, y});
        for (int x = W - 1; x >= 0; --x) per.push_back({x, H - 1});
        for (int y = H - 2; y >= 1; --y) per.push_back({0, y});

        // Inner glow band (inside the rectangle border).
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int d = std::min(std::min(x, y), std::min(W - 1 - x, H - 1 - y));
                if (d <= 0 || d > glowT) continue;
                const float t = 1.0f - static_cast<float>(d - 1) / static_cast<float>(std::max(1, glowT));
                const uint8_t a = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(static_cast<float>(glowA0) * t)), 0, 255));
                put(x, y, a);
            }
        }

        // Center crosshair: a small plus that subtly breathes.
        const int cx = W / 2;
        const int cy = H / 2;
        const int len = std::max(3, W / 6);
        const int len2 = (frame == 1 || frame == 3) ? len + 1 : len;
        const uint8_t ca = static_cast<uint8_t>(std::clamp<int>(90 + std::lround(35.0f * pulse), 0, 255));
        Color cc{255, 255, 255, ca};

        for (int t = -crossT; t <= crossT; ++t) {
            line(s, cx - len2, cy + t, cx + len2, cy + t, cc);
            line(s, cx + t, cy - len2, cx + t, cy + len2, cc);
        }

    } else {
        // Isometric: diamond perimeter inscribed in the square.
        const int cx = W / 2;
        const int cy = H / 2;
        const Vec2i top{cx, 0};
        const Vec2i right{W - 1, cy};
        const Vec2i bot{cx, H - 1};
        const Vec2i left{0, cy};

        auto rasterLine = [&](Vec2i a, Vec2i b) {
            std::vector<Vec2i> pts;
            int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
            const int dx = std::abs(x1 - x0);
            const int sx = (x0 < x1) ? 1 : -1;
            const int dy = -std::abs(y1 - y0);
            const int sy = (y0 < y1) ? 1 : -1;
            int err = dx + dy;
            while (true) {
                pts.push_back({x0, y0});
                if (x0 == x1 && y0 == y1) break;
                const int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
            return pts;
        };

        auto addEdge = [&](Vec2i a, Vec2i b, bool includeFirst) {
            auto pts = rasterLine(a, b);
            const size_t start = includeFirst ? 0u : 1u;
            for (size_t i = start; i < pts.size(); ++i) per.push_back(pts[i]);
        };

        per.reserve(static_cast<size_t>(W * 4));
        addEdge(top, right, true);
        addEdge(right, bot, false);
        addEdge(bot, left, false);
        addEdge(left, top, false);

        // Inner diamond glow band (computed via normalized L1 distance to stay symmetric).
        const float hw = static_cast<float>(W) * 0.5f;
        const float hh = static_cast<float>(H) * 0.5f;
        const float band = std::clamp(static_cast<float>(glowT) / std::max(1.0f, hw), 0.004f, 0.12f);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float nx = std::abs((static_cast<float>(x) + 0.5f) - hw) / hw;
                const float ny = std::abs((static_cast<float>(y) + 0.5f) - hh) / hh;
                const float d = nx + ny;
                if (d > 1.0f) continue;
                const float edge = 1.0f - d;
                if (edge < 0.0f || edge > band) continue;

                const float t = 1.0f - (edge / band);
                const uint8_t a = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::lround(static_cast<float>(glowA0) * t)), 0, 255));
                put(x, y, a);
            }
        }

        // Center crosshair: short axis lines that breathe.
        const int len = std::max(3, W / 6);
        const int len2 = (frame == 1 || frame == 3) ? len + 1 : len;
        const uint8_t ca = static_cast<uint8_t>(std::clamp<int>(90 + std::lround(35.0f * pulse), 0, 255));
        Color cc{255, 255, 255, ca};

        for (int t = -crossT; t <= crossT; ++t) {
            line(s, cx - len2, cy + t, cx + len2, cy + t, cc);
            line(s, cx + t, cy - len2, cx + t, cy + len2, cc);
        }
    }

    // Marching-ants perimeter stroke.
    if (!per.empty()) {
        const size_t L = per.size();

        // Add a single traveling "spark" to help motion read even when dashes are tiny.
        const size_t sparkStep = std::max<size_t>(1u, L / 4u);
        const size_t sparkIdx = (static_cast<size_t>(h) + static_cast<size_t>(frame) * sparkStep) % L;

        for (size_t i = 0; i < L; ++i) {
            const Vec2i p = per[i];
            const bool on = (static_cast<int>((i + static_cast<size_t>(offset)) % static_cast<size_t>(period)) < duty);
            uint8_t a = on ? brightA : dimA;

            if (i == sparkIdx) {
                a = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(a) + 55, 0, 255));
            }

            put(p.x, p.y, a);
        }
    }

    return s;
}
