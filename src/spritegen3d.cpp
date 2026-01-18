#include "spritegen3d.hpp"

#include "mesh2d.hpp"

#include "game.hpp"   // EntityKind
#include "items.hpp"  // ItemKind, ProjectileKind helpers

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <queue>
#include <vector>

namespace {

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3f operator+(const Vec3f& a, const Vec3f& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3f operator-(const Vec3f& a, const Vec3f& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3f operator*(const Vec3f& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline float dot(const Vec3f& a, const Vec3f& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

inline Vec3f cross(const Vec3f& a, const Vec3f& b) {
    return { a.y*b.z - a.z*b.y,
             a.z*b.x - a.x*b.z,
             a.x*b.y - a.y*b.x };
}

inline float len(const Vec3f& v) { return std::sqrt(dot(v,v)); }

inline Vec3f normalize(const Vec3f& v) {
    const float l = len(v);
    if (l <= 1e-6f) return {0,0,0};
    return { v.x / l, v.y / l, v.z / l };
}

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

inline Color mul(Color c, float f) {
    const int r = static_cast<int>(std::round(c.r * f));
    const int g = static_cast<int>(std::round(c.g * f));
    const int b = static_cast<int>(std::round(c.b * f));
    return { clamp8(r), clamp8(g), clamp8(b), c.a };
}

inline Color lerp(Color a, Color b, float t) {
    t = clampf(t, 0.0f, 1.0f);
    const float it = 1.0f - t;
    return {
        clamp8(static_cast<int>(std::round(a.r * it + b.r * t))),
        clamp8(static_cast<int>(std::round(a.g * it + b.g * t))),
        clamp8(static_cast<int>(std::round(a.b * it + b.b * t))),
        clamp8(static_cast<int>(std::round(a.a * it + b.a * t)))
    };
}

inline int signum(float v) {
    return (v > 0.0f) - (v < 0.0f);
}

struct VoxelModel {
    int w = 0;
    int h = 0;
    int d = 0;
    std::vector<Color> vox; // alpha==0 => empty

    Color at(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d) return {0,0,0,0};
        return vox[static_cast<size_t>((z * h + y) * w + x)];
    }
    void set(int x, int y, int z, Color c) {
        if (x < 0 || y < 0 || z < 0 || x >= w || y >= h || z >= d) return;
        vox[static_cast<size_t>((z * h + y) * w + x)] = c;
    }
};

// Nearest-neighbor voxel upscaling.
//
// This is intentionally "blocky": it preserves the original model's voxel-art
// silhouette while increasing geometric resolution so high-resolution sprite
// outputs (64x64, 128x128) don't look like gigantic cubes.
//
// We replicate filled voxels into an s×s×s block. Empty voxels are omitted (the
// destination grid is initialized as empty), keeping the operation reasonably
// fast for sparse models.
VoxelModel scaleVoxelModelNearest(const VoxelModel& src, int s) {
    if (s <= 1) return src;
    if (src.w <= 0 || src.h <= 0 || src.d <= 0) return src;

    VoxelModel dst;
    dst.w = src.w * s;
    dst.h = src.h * s;
    dst.d = src.d * s;
    dst.vox.assign(static_cast<size_t>(dst.w * dst.h * dst.d), {0, 0, 0, 0});

    for (int z = 0; z < src.d; ++z) {
        for (int y = 0; y < src.h; ++y) {
            const size_t srcRow = static_cast<size_t>((z * src.h + y) * src.w);
            for (int x = 0; x < src.w; ++x) {
                const Color c = src.vox[srcRow + static_cast<size_t>(x)];
                if (c.a == 0) continue;

                const int x0 = x * s;
                const int y0 = y * s;
                const int z0 = z * s;

                for (int zz = 0; zz < s; ++zz) {
                    for (int yy = 0; yy < s; ++yy) {
                        const size_t dstRow = static_cast<size_t>(((z0 + zz) * dst.h + (y0 + yy)) * dst.w + x0);
                        for (int xx = 0; xx < s; ++xx) {
                            dst.vox[dstRow + static_cast<size_t>(xx)] = c;
                        }
                    }
                }
            }
        }
    }
    return dst;
}

struct Palette {
    Color primary{180,180,180,255};
    Color secondary{120,120,120,255};
    Color accent{255,255,255,255};
};

Palette extractPalette(const SpritePixels& s) {
    // Quantized histogram to find the dominant non-outline colors.
    struct Bin {
        uint32_t count = 0;
        uint64_t sr = 0, sg = 0, sb = 0;
    };

    std::array<Bin, 8*8*8> bins{};

    for (int y = 0; y < s.h; ++y) {
        for (int x = 0; x < s.w; ++x) {
            const Color c = s.at(x,y);
            if (c.a == 0) continue;

            const int bright = static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b);
            if (bright < 60) continue; // ignore outlines/shadows

            const int rq = std::min(7, static_cast<int>(c.r) / 32);
            const int gq = std::min(7, static_cast<int>(c.g) / 32);
            const int bq = std::min(7, static_cast<int>(c.b) / 32);
            const size_t idx = static_cast<size_t>((rq << 6) | (gq << 3) | bq);

            bins[idx].count++;
            bins[idx].sr += c.r;
            bins[idx].sg += c.g;
            bins[idx].sb += c.b;
        }
    }

    auto binToColor = [&](size_t i) -> Color {
        const Bin& b = bins[i];
        if (b.count == 0) return {180,180,180,255};
        return {
            clamp8(static_cast<int>(b.sr / b.count)),
            clamp8(static_cast<int>(b.sg / b.count)),
            clamp8(static_cast<int>(b.sb / b.count)),
            255
        };
    };

    size_t top1 = 0;
    size_t top2 = 0;
    uint32_t c1 = 0, c2 = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
        const uint32_t c = bins[i].count;
        if (c > c1) {
            top2 = top1; c2 = c1;
            top1 = i;    c1 = c;
        } else if (c > c2) {
            top2 = i; c2 = c;
        }
    }

    Palette p;
    if (c1 == 0) {
        // Fallback: average all non-transparent pixels.
        uint64_t sr=0, sg=0, sb=0, n=0;
        for (const auto& c : s.px) {
            if (c.a == 0) continue;
            sr += c.r; sg += c.g; sb += c.b; n++;
        }
        if (n > 0) p.primary = { clamp8(static_cast<int>(sr/n)), clamp8(static_cast<int>(sg/n)), clamp8(static_cast<int>(sb/n)), 255 };
        p.secondary = mul(p.primary, 0.70f);
        p.accent = lerp(p.primary, {255,255,255,255}, 0.30f);
        return p;
    }

    p.primary = binToColor(top1);

    if (c2 > 0) {
        Color sec = binToColor(top2);
        // If the 2nd bin is too close, synthesize a darker variant.
        int dr = static_cast<int>(sec.r) - static_cast<int>(p.primary.r);
        int dg = static_cast<int>(sec.g) - static_cast<int>(p.primary.g);
        int db = static_cast<int>(sec.b) - static_cast<int>(p.primary.b);
        const int dist = std::abs(dr) + std::abs(dg) + std::abs(db);
        p.secondary = (dist < 60) ? mul(p.primary, 0.70f) : sec;
    } else {
        p.secondary = mul(p.primary, 0.70f);
    }

    p.accent = lerp(p.primary, {255,255,255,255}, 0.35f);
    return p;
}

VoxelModel voxelizeExtrude(const SpritePixels& base2d, uint32_t seed, int maxDepth) {
    VoxelModel m;
    m.w = base2d.w;
    m.h = base2d.h;
    m.d = maxDepth;
    m.vox.assign(static_cast<size_t>(m.w * m.h * m.d), {0,0,0,0});

    const int w = base2d.w;
    const int h = base2d.h;

    // Mask (alpha>0)
    std::vector<uint8_t> mask(static_cast<size_t>(w * h), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Color c = base2d.at(x,y);
            if (c.a > 0) mask[static_cast<size_t>(y*w + x)] = 1;
        }
    }

    // Average color (try to ignore near-black outlines).
    uint64_t sr=0, sg=0, sb=0, n=0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!mask[static_cast<size_t>(y*w + x)]) continue;
            const Color c = base2d.at(x,y);
            const int bright = static_cast<int>(c.r) + static_cast<int>(c.g) + static_cast<int>(c.b);
            if (c.a > 120 && bright > 140) { // skip the darkest pixels
                sr += c.r; sg += c.g; sb += c.b; n++;
            }
        }
    }
    if (n == 0) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!mask[static_cast<size_t>(y*w + x)]) continue;
                const Color c = base2d.at(x,y);
                sr += c.r; sg += c.g; sb += c.b; n++;
            }
        }
    }
    Color avg = {180,180,180,255};
    if (n > 0) {
        avg = { clamp8(static_cast<int>(sr / n)), clamp8(static_cast<int>(sg / n)), clamp8(static_cast<int>(sb / n)), 255 };
    }

    // Distance-to-edge inside the mask (4-neighborhood BFS).
    constexpr int INF = 9999;
    std::vector<int> dist(static_cast<size_t>(w * h), INF);
    std::queue<Vec2i> q;
    auto idx = [&](int x, int y){ return static_cast<size_t>(y*w + x); };
    auto isMask = [&](int x, int y)->bool {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        return mask[idx(x,y)] != 0;
    };
    auto isEdgePix = [&](int x, int y)->bool {
        if (!isMask(x,y)) return false;
        // If any neighbor is outside mask, it's an edge pixel.
        return !isMask(x-1,y) || !isMask(x+1,y) || !isMask(x,y-1) || !isMask(x,y+1);
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (isEdgePix(x,y)) {
                dist[idx(x,y)] = 0;
                q.push({x,y});
            }
        }
    }
    const int dx4[4] = {-1,1,0,0};
    const int dy4[4] = {0,0,-1,1};
    while (!q.empty()) {
        Vec2i p = q.front();
        q.pop();
        const int base = dist[idx(p.x,p.y)];
        for (int k = 0; k < 4; ++k) {
            const int nx = p.x + dx4[k];
            const int ny = p.y + dy4[k];
            if (!isMask(nx,ny)) continue;
            const size_t ni = idx(nx,ny);
            if (dist[ni] > base + 1) {
                dist[ni] = base + 1;
                q.push({nx,ny});
            }
        }
    }

    // Stable RNG noise (do NOT use frame; base2d already animates via frame).
    RNG rng(hashCombine(seed, 0xBADC0FFEu));

    // Fill voxels: cardboard extrusion + bevel (deeper layers erode the silhouette slightly).
    for (int yImg = 0; yImg < h; ++yImg) {
        for (int x = 0; x < w; ++x) {
            if (!isMask(x,yImg)) continue;

            const int dEdge = std::min(dist[idx(x,yImg)], maxDepth);
            int thickness = 2 + std::min(dEdge, maxDepth - 2);
            if (rng.chance(0.20f)) thickness += rng.range(-1, 1);
            thickness = std::clamp(thickness, 1, maxDepth);

            // Flatten the original shading a bit so we can re-light in 3D.
            Color c = base2d.at(x, yImg);
            c = lerp(c, avg, 0.55f);
            c.a = 255;

            const int yVox = (h - 1 - yImg); // sprite space (down) -> voxel space (up)

            for (int z = 0; z < thickness; ++z) {
                // Bevel: deeper layers shrink toward the silhouette interior.
                const int requiredDist = z / 2; // 0,0,1,1,2,2...
                if (dEdge < requiredDist) continue;

                // Slight color variation by layer for richness.
                float layerTint = 1.0f - 0.06f * static_cast<float>(z);
                Color cc = mul(c, layerTint);
                m.set(x, yVox, z, cc);
            }
        }
    }

    return m;
}

VoxelModel makeModel(int w, int h, int d) {
    VoxelModel m;
    m.w = w;
    m.h = h;
    m.d = d;
    m.vox.assign(static_cast<size_t>(w*h*d), {0,0,0,0});
    return m;
}

inline bool isFilled(const VoxelModel& m, int x, int y, int z) {
    return m.at(x,y,z).a > 0;
}

void addBox(VoxelModel& m, int x0, int y0, int z0, int x1, int y1, int z1, Color c, bool onlyIfEmpty = false) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (z0 > z1) std::swap(z0, z1);
    for (int z = z0; z <= z1; ++z) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (onlyIfEmpty && m.at(x,y,z).a > 0) continue;
                m.set(x,y,z,c);
            }
        }
    }
}

void addSphere(VoxelModel& m, float cx, float cy, float cz, float r, Color c, bool onlyIfEmpty = false) {
    const float r2 = r*r;
    const int x0 = static_cast<int>(std::floor(cx - r - 1));
    const int x1 = static_cast<int>(std::ceil (cx + r + 1));
    const int y0 = static_cast<int>(std::floor(cy - r - 1));
    const int y1 = static_cast<int>(std::ceil (cy + r + 1));
    const int z0 = static_cast<int>(std::floor(cz - r - 1));
    const int z1 = static_cast<int>(std::ceil (cz + r + 1));

    for (int z = z0; z <= z1; ++z) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) - cx;
                const float dy = (static_cast<float>(y) + 0.5f) - cy;
                const float dz = (static_cast<float>(z) + 0.5f) - cz;
                if (dx*dx + dy*dy + dz*dz > r2) continue;
                if (onlyIfEmpty && m.at(x,y,z).a > 0) continue;
                m.set(x,y,z,c);
            }
        }
    }
}

void addCylinderY(VoxelModel& m, float cx, float cz, float r, int y0, int y1, Color c,
                  int zMin, int zMax, bool onlyIfEmpty = false) {
    const float r2 = r*r;
    if (y0 > y1) std::swap(y0, y1);
    if (zMin > zMax) std::swap(zMin, zMax);

    const int x0 = static_cast<int>(std::floor(cx - r - 1));
    const int x1 = static_cast<int>(std::ceil (cx + r + 1));
    const int z0 = zMin;
    const int z1 = zMax;

    for (int y = y0; y <= y1; ++y) {
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) - cx;
                const float dz = (static_cast<float>(z) + 0.5f) - cz;
                if (dx*dx + dz*dz > r2) continue;
                if (onlyIfEmpty && m.at(x,y,z).a > 0) continue;
                m.set(x,y,z,c);
            }
        }
    }
}

void carveCylinderY(VoxelModel& m, float cx, float cz, float r, int y0, int y1, int zMin, int zMax) {
    const float r2 = r*r;
    if (y0 > y1) std::swap(y0, y1);
    if (zMin > zMax) std::swap(zMin, zMax);

    const int x0 = static_cast<int>(std::floor(cx - r - 1));
    const int x1 = static_cast<int>(std::ceil (cx + r + 1));

    for (int y = y0; y <= y1; ++y) {
        for (int z = zMin; z <= zMax; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) - cx;
                const float dz = (static_cast<float>(z) + 0.5f) - cz;
                if (dx*dx + dz*dz > r2) continue;
                m.set(x,y,z,{0,0,0,0});
            }
        }
    }
}

void addLine3D(VoxelModel& m, Vec3f a, Vec3f b, float radius, Color c) {
    const Vec3f d = b - a;
    const float L = len(d);
    if (L < 1e-6f) {
        addSphere(m, a.x, a.y, a.z, radius, c);
        return;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(L * 3.0f)));
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        Vec3f p = a + d * t;
        addSphere(m, p.x, p.y, p.z, radius, c);
    }
}

VoxelModel buildItemModel(ItemKind kind, uint32_t seed, int frame, const SpritePixels& base2d) {
    (void)frame;

    constexpr int W = 16;
    constexpr int H = 16;
    constexpr int D = 8;

    Palette pal = extractPalette(base2d);
    Color main = pal.primary;
    Color sec  = pal.secondary;
    Color acc  = pal.accent;

    RNG rng(hashCombine(seed, 0xA11C0DEu));

    const float cx = 7.5f;
    const float cz = (D - 1) / 2.0f;

    VoxelModel m;

    // Potions: translucent glass shell + opaque liquid core.
    if (isPotionKind(kind)) {
        m = makeModel(W,H,D);

        Color glass = {180, 220, 255, 110};
        Color liquid = main;
        liquid.a = 230;
        Color cork  = {140, 95, 55, 255};

        // Body
        addCylinderY(m, cx, cz, 4.15f, 2, 11, glass, 0, D-1, /*onlyIfEmpty=*/false);
        addCylinderY(m, cx, cz, 3.05f, 3, 10, liquid, 1, D-2, /*onlyIfEmpty=*/true);

        // Neck
        addCylinderY(m, cx, cz, 2.70f, 11, 13, glass, 0, D-1, false);
        addCylinderY(m, cx, cz, 1.70f, 11, 13, liquid, 1, D-2, true);

        // Cork
        addCylinderY(m, cx, cz, 2.10f, 13, 15, cork, 1, D-2, false);

        // A subtle highlight streak on the front.
        Color hi = {255,255,255,80};
        for (int y = 4; y <= 12; ++y) {
            m.set(10, y, 0, hi);
            if (y % 2 == 0) m.set(9, y, 0, {255,255,255,60});
        }

        // Tiny bubble variation
        if (rng.chance(0.55f)) {
            const int bx = rng.range(6, 9);
            const int by = rng.range(5, 10);
            const int bz = rng.range(2, 5);
            m.set(bx, by, bz, {255,255,255,70});
        }

        return m;
    }

    // Scrolls: a curled parchment slab with a wax seal.
    if (isScrollKind(kind)) {
        m = makeModel(W,H,D);

        Color paper = lerp(main, {235, 220, 185, 255}, 0.55f);
        paper.a = 255;
        Color paper2 = mul(paper, 0.85f);
        Color wax = {170, 40, 50, 255};

        // Main sheet (tilted slightly toward the camera)
        addBox(m, 4, 6, 1, 11, 9, 5, paper);
        // Rolled edges
        addBox(m, 3, 6, 2, 4, 9, 5, paper2);
        addBox(m, 11, 6, 1, 12, 9, 4, paper2);

        // Wax seal blob near center-bottom
        addSphere(m, 7.5f, 6.2f, 2.0f, 1.4f, wax);
        addSphere(m, 8.4f, 6.2f, 2.2f, 1.0f, mul(wax, 0.85f));

        // Ink specks (very subtle)
        for (int i = 0; i < 5; ++i) {
            int x = rng.range(5, 10);
            int y = rng.range(7, 9);
            int z = rng.range(1, 2);
            m.set(x,y,z, {60,60,70,200});
        }

        return m;
    }

    // Rings: a flat torus-like loop.
    if (kind == ItemKind::RingMight || kind == ItemKind::RingAgility || kind == ItemKind::RingFocus ||
        kind == ItemKind::RingProtection || kind == ItemKind::RingSearching || kind == ItemKind::RingSustenance) {
        m = makeModel(W,H,D);

        Color metal = lerp(main, {235, 210, 120, 255}, 0.40f);
        metal.a = 255;

        const float rOuter = 4.2f;
        const float rInner = 2.3f;
        for (int y = 6; y <= 8; ++y) {
            const float t = (y - 6) / 2.0f;
            const float ro = rOuter - 0.15f * std::abs(t - 0.5f);
            const float ri = rInner + 0.10f * std::abs(t - 0.5f);
            for (int z = 0; z < D; ++z) {
                for (int x = 0; x < W; ++x) {
                    const float dx = (static_cast<float>(x) + 0.5f) - cx;
                    const float dz = (static_cast<float>(z) + 0.5f) - cz;
                    const float d2 = dx*dx + dz*dz;
                    if (d2 <= ro*ro && d2 >= ri*ri) {
                        m.set(x,y,z, metal);
                    }
                }
            }
        }

        // A tiny gem hint for readability.
        addSphere(m, 10.2f, 8.5f, 1.5f, 1.0f, lerp(acc, {255, 240, 240, 255}, 0.20f));

        return m;
    }

    // Amulet of Yendor: gemstone + chain.
    if (kind == ItemKind::AmuletYendor) {
        m = makeModel(W,H,D);

        Color gem = lerp(main, {255, 210, 120, 255}, 0.35f);
        gem.a = 255;
        Color chain = {210, 210, 225, 255};

        addSphere(m, cx, 8.5f, cz + 0.1f, 3.2f, gem);
        addSphere(m, cx + 0.8f, 8.9f, cz - 0.6f, 2.2f, mul(gem, 0.90f), /*onlyIfEmpty=*/true);

        // Little chain loop above.
        addCylinderY(m, cx, cz, 2.5f, 12, 13, chain, 1, D-2, false);
        carveCylinderY(m, cx, cz, 1.3f, 12, 13, 1, D-2);

        return m;
    }

    // Gold: small coin pile.
    if (kind == ItemKind::Gold) {
        m = makeModel(W,H,D);

        Color gold = lerp(main, {245, 220, 120, 255}, 0.55f);
        gold.a = 255;
        Color gold2 = mul(gold, 0.85f);

        // A few overlapping short cylinders.
        for (int i = 0; i < 4; ++i) {
            float ox = cx + rng.range(-3, 3);
            float oz = cz + rng.range(-2, 2);
            float rr = 2.3f + 0.4f * rng.next01();
            int y0 = 2 + rng.range(0, 1);
            int y1 = y0 + 1;
            addCylinderY(m, ox, oz, rr, y0, y1, (i % 2 == 0) ? gold : gold2, 1, D-2, false);
        }

        // A bright glint
        addSphere(m, cx + 2.0f, 4.4f, 1.0f, 0.9f, {255,255,255,160});
        return m;
    }

    // Chests: chunky box props.
    if (kind == ItemKind::Chest || kind == ItemKind::ChestOpen) {
        m = makeModel(W,H,D);

        Color wood = lerp(main, {140, 95, 55, 255}, 0.50f);
        wood.a = 255;
        Color band = {210, 210, 225, 255};
        Color dark = mul(wood, 0.75f);

        // Base box
        addBox(m, 3, 2, 1, 12, 7, 6, wood);
        addBox(m, 3, 2, 1, 12, 2, 6, dark); // darker underside strip

        // Metal bands
        addBox(m, 3, 4, 1, 12, 4, 1, band);
        addBox(m, 3, 4, 6, 12, 4, 6, band);

        // Lid
        if (kind == ItemKind::ChestOpen) {
            // open lid tilted back
            addBox(m, 3, 8, 4, 12, 10, 6, wood);
            addBox(m, 3, 8, 6, 12, 8, 6, dark);
        } else {
            addBox(m, 3, 8, 1, 12, 10, 6, wood);
        }

        // Lock
        addBox(m, 7, 5, 1, 8, 6, 2, {245, 210, 120, 255});

        return m;
    }

    // Torches: handle + (optional) flame blob.
    if (kind == ItemKind::Torch || kind == ItemKind::TorchLit) {
        m = makeModel(W,H,D);

        Color wood = lerp(main, {140, 95, 55, 255}, 0.45f);
        wood.a = 255;

        // Slightly off-center for depth.
        const float tcx = cx - 1.2f;
        addCylinderY(m, tcx, cz, 1.15f, 2, 12, wood, 2, D-3, false);
        addCylinderY(m, tcx, cz, 1.05f, 2, 12, mul(wood, 0.85f), 1, 1, true);

        // Cloth wrap
        addCylinderY(m, tcx, cz, 1.65f, 10, 11, {180, 170, 140, 255}, 2, D-3, true);

        if (kind == ItemKind::TorchLit) {
            Color flameOuter = {220, 90, 35, 180};
            Color flameMid   = {255, 150, 70, 200};
            Color flameCore  = {255, 235, 170, 220};

            // Flicker: shift slightly based on frame.
            const float fx = tcx + ((frame % 2 == 0) ? 0.25f : -0.25f);
            const float fz = cz + ((frame % 2 == 0) ? -0.15f : 0.15f);

            addSphere(m, fx, 13.2f, fz, 2.6f, flameOuter);
            addSphere(m, fx, 13.4f, fz, 1.8f, flameMid, true);
            addSphere(m, fx, 13.7f, fz, 1.0f, flameCore, true);

            // A couple of sparks
            if (frame % 2 == 1) {
                m.set(static_cast<int>(tcx) + 1, 15, 1, {255,255,255,120});
                m.set(static_cast<int>(tcx), 14, 0, {255,240,200,100});
            }
        }

        return m;
    }

    // Wands: rod + orb tip.
    if (kind == ItemKind::WandSparks || kind == ItemKind::WandDigging || kind == ItemKind::WandFireball) {
        m = makeModel(W,H,D);

        Color rod = lerp(sec, {120, 80, 45, 255}, 0.35f);
        rod.a = 255;

        addCylinderY(m, cx, cz, 1.0f, 3, 12, rod, 2, D-3, false);
        addCylinderY(m, cx, cz, 0.85f, 3, 12, mul(rod, 0.85f), 1, 1, true);

        Color orb = acc;
        orb.a = 220;
        addSphere(m, cx, 13.2f, cz, 2.2f, orb);
        addSphere(m, cx + 0.7f, 13.8f, cz - 0.4f, 1.0f, {255,255,255,140}, true);

        return m;
    }

    // Keys / lockpicks: thin rods.
    if (kind == ItemKind::Key || kind == ItemKind::Lockpick) {
        m = makeModel(W,H,D);

        Color metal = lerp(main, {210, 210, 225, 255}, 0.55f);
        metal.a = 255;

        // Stem
        addLine3D(m, {7.0f, 4.0f, 2.0f}, {7.0f, 12.5f, 5.8f}, 0.7f, metal);

        // Handle ring (key only)
        if (kind == ItemKind::Key) {
            addCylinderY(m, 7.0f, 2.0f, 2.2f, 12, 13, metal, 1, D-2, false);
            carveCylinderY(m, 7.0f, 2.0f, 1.1f, 12, 13, 1, D-2);
        } else {
            // Lockpick bend
            addLine3D(m, {7.0f, 12.5f, 5.8f}, {9.5f, 13.8f, 6.5f}, 0.6f, metal);
        }

        return m;
    }

    // Armor: chunky torso.
    if (kind == ItemKind::LeatherArmor || kind == ItemKind::ChainArmor || kind == ItemKind::PlateArmor) {
        m = makeModel(W,H,D);

        Color armor = main;
        armor.a = 255;
        Color trim = sec;
        trim.a = 255;

        // Torso
        addBox(m, 4, 3, 2, 11, 10, 6, armor);

        // Shoulder bits
        addBox(m, 3, 9, 2, 5, 11, 5, trim);
        addBox(m, 10, 9, 2, 12, 11, 5, trim);

        // Collar highlight
        addBox(m, 6, 10, 1, 9, 10, 2, lerp(acc, {255,255,255,255}, 0.20f), true);

        return m;
    }

    // Food ration: simple parcel.
    if (kind == ItemKind::FoodRation) {
        m = makeModel(W,H,D);

        Color wrap = lerp(main, {190, 170, 130, 255}, 0.55f);
        wrap.a = 255;
        Color band = {140, 95, 55, 255};

        addBox(m, 4, 3, 2, 11, 7, 6, wrap);
        addBox(m, 7, 3, 2, 8, 7, 6, band);
        addSphere(m, 9.5f, 7.0f, 1.5f, 0.9f, {255,255,255,110});
        return m;
    }

    // Simple 3D weapons (vertical blades).
    if (kind == ItemKind::Dagger || kind == ItemKind::Sword) {
        m = makeModel(W,H,D);

        Color steel = lerp(main, {210, 210, 225, 255}, 0.55f);
        steel.a = 255;
        Color hilt = {130, 90, 45, 255};

        const int tipY = (kind == ItemKind::Sword) ? 14 : 12;
        const int bladeY0 = 5;
        const int bladeY1 = tipY;
        const int bladeW = (kind == ItemKind::Sword) ? 1 : 0;

        addBox(m, 7-bladeW, bladeY0, 2, 8+bladeW, bladeY1, 4, steel);
        addBox(m, 7-bladeW, bladeY0, 5, 8+bladeW, bladeY1, 6, mul(steel, 0.88f));

        // Hilt
        addBox(m, 5, 4, 2, 10, 5, 6, hilt);
        addBox(m, 6, 2, 3, 9, 3, 5, mul(hilt, 0.85f));

        // Highlight
        addLine3D(m, {7.0f, static_cast<float>(bladeY1), 2.0f}, {7.0f, static_cast<float>(bladeY0), 2.0f}, 0.45f, {255,255,255,90});

        return m;
    }

    if (kind == ItemKind::Axe || kind == ItemKind::Pickaxe) {
        m = makeModel(W,H,D);

        Color steel = {210,210,225,255};
        Color wood  = {135, 95, 55, 255};
        steel = lerp(steel, main, 0.25f);

        // Handle
        addCylinderY(m, cx, cz, 0.95f, 2, 13, wood, 2, D-3, false);

        if (kind == ItemKind::Axe) {
            // Axe head
            addBox(m, 5, 11, 1, 10, 13, 3, steel);
            addBox(m, 4, 11, 2, 6, 12, 4, mul(steel, 0.90f));
        } else {
            // Pickaxe head (cross)
            addBox(m, 4, 12, 2, 11, 13, 4, steel);
            addBox(m, 5, 11, 3, 10, 12, 3, mul(steel, 0.90f));
        }

        return m;
    }

    // Rocks as item: little sphere.
    if (kind == ItemKind::Rock) {
        m = makeModel(W,H,D);
        Color stone = lerp(main, {150,150,160,255}, 0.50f);
        stone.a = 255;
        addSphere(m, cx, 6.0f, cz, 3.2f, stone);
        addSphere(m, cx + 1.2f, 6.5f, cz - 0.8f, 2.0f, mul(stone, 0.88f), true);
        // tiny chip
        if (rng.chance(0.6f)) m.set(10, 7, 0, {255,255,255,70});
        return m;
    }

    // Default: no native model.
    return {};
}

VoxelModel buildProjectileModel(ProjectileKind kind, int frame, const SpritePixels& base2d) {
    constexpr int W = 16;
    constexpr int H = 16;
    constexpr int D = 8;

    Palette pal = extractPalette(base2d);
    Color main = pal.primary;

    const float cx = 7.5f;
    const float cz = (D - 1) / 2.0f;

    VoxelModel m;

    switch (kind) {
        case ProjectileKind::Rock: {
            m = makeModel(W,H,D);
            Color stone = lerp(main, {150,150,160,255}, 0.50f);
            stone.a = 255;
            addSphere(m, cx, 8.0f, cz, 3.0f, stone);
            if (frame % 2 == 1) {
                addSphere(m, cx + 1.0f, 8.6f, cz - 1.0f, 1.6f, mul(stone, 0.88f), true);
                m.set(10, 10, 0, {255,255,255,70});
            }
            return m;
        }
        case ProjectileKind::Spark: {
            m = makeModel(W,H,D);
            Color s1 = {120,220,255,200};
            Color s2 = {255,255,255,160};
            // 3D star: crossing rods
            addLine3D(m, {4.0f, 8.0f, 1.0f}, {11.5f, 8.0f, 6.5f}, 0.65f, s1);
            addLine3D(m, {11.5f, 8.0f, 1.0f}, {4.0f, 8.0f, 6.5f}, 0.65f, s1);
            addLine3D(m, {7.5f, 4.0f, 3.5f}, {7.5f, 12.0f, 3.5f}, 0.65f, s1);

            if (frame % 2 == 1) {
                addSphere(m, 7.5f, 8.0f, 3.5f, 1.2f, s2, true);
            }
            return m;
        }
        case ProjectileKind::Fireball: {
            m = makeModel(W,H,D);
            Color outer = {220, 80, 35, 170};
            Color mid   = {255, 150, 70, 200};
            Color core  = {255, 235, 170, 220};

            const float wob = (frame % 2 == 0) ? -0.2f : 0.2f;
            addSphere(m, cx + wob, 8.0f, cz - wob, 3.8f, outer);
            addSphere(m, cx + wob, 8.0f, cz - wob, 2.6f, mid, true);
            addSphere(m, cx + wob, 8.0f, cz - wob, 1.4f, core, true);

            // Small sparks
            if (frame % 2 == 1) {
                m.set(11, 10, 1, {255,255,255,120});
                m.set(4, 7, 0, {255,240,200,90});
            }
            return m;
        }
        case ProjectileKind::Torch: {
            m = makeModel(W,H,D);
            // Simple rod + flame. Torch projectiles are rendered as a small stick with a glowing head.
            Color wood = {120, 80, 45, 255};
            Color outer = {240, 120, 60, 200};
            Color core  = {255, 235, 170, 220};

            // Diagonal stick
            addLine3D(m, {4.0f, 9.0f, 1.0f}, {11.5f, 6.5f, 6.5f}, 0.75f, wood);

            // Flame at the leading end
            const float wob = (frame % 2 == 0) ? -0.15f : 0.15f;
            addSphere(m, 12.0f + wob, 6.0f - wob, 6.0f, 2.2f, outer, true);
            addSphere(m, 12.0f + wob, 6.0f - wob, 6.0f, 1.3f, core, true);
            return m;
        }
        case ProjectileKind::Arrow:
        default:
            return {};
    }
}

VoxelModel buildEntityModel(EntityKind kind, uint32_t seed, int frame, const SpritePixels& base2d) {
    constexpr int W = 16;
    constexpr int H = 16;
    constexpr int D = 8;

    Palette pal = extractPalette(base2d);
    Color main = pal.primary;

    RNG rng(hashCombine(seed, 0xE11A11Eu));

    const float cx = 7.5f;
    const float cz = (D - 1) / 2.0f;

    VoxelModel m;

    if (kind == EntityKind::Slime) {
        m = makeModel(W,H,D);
        Color goo = lerp(main, {90, 220, 120, 255}, 0.55f);
        goo.a = 200;

        // Blobby dome
        addSphere(m, cx, 6.0f, cz, 4.4f, goo);
        addSphere(m, cx + 1.0f, 5.0f, cz - 0.8f, 3.2f, mul(goo, 0.92f), true);

        // Tiny bubbles
        for (int i = 0; i < 2; ++i) {
            int bx = rng.range(5, 10);
            int by = rng.range(5, 8);
            int bz = rng.range(1, 6);
            m.set(bx, by, bz, {255,255,255,70});
        }

        if (frame % 2 == 1) {
            m.set(9, 8, 1, {255,255,255,90});
        }

        return m;
    }

    if (kind == EntityKind::Ghost) {
        m = makeModel(W,H,D);
        Color ecto = lerp(main, {160, 200, 255, 255}, 0.55f);
        ecto.a = 170;

        // Wispy stacked blobs
        addSphere(m, cx, 11.0f, cz, 3.2f, ecto);
        addSphere(m, cx, 8.5f, cz, 3.7f, ecto, true);
        addSphere(m, cx, 6.2f, cz, 3.0f, ecto, true);
        addSphere(m, cx, 4.4f, cz, 2.3f, ecto, true);

        // Fade out the tail (clear random holes)
        for (int i = 0; i < 16; ++i) {
            int x = rng.range(4, 11);
            int y = rng.range(2, 6);
            int z = rng.range(0, D-1);
            if (rng.chance(0.45f)) m.set(x,y,z,{0,0,0,0});
        }

        // Eyes (subtle)
        m.set(6, 11, 0, {40, 40, 55, 140});
        m.set(9, 11, 0, {40, 40, 55, 140});

        // Slight flicker
        if (frame % 2 == 1) {
            m.set(7, 12, 0, {255,255,255,60});
        }

        return m;
    }

    return {};
}

SpritePixels renderVoxel(const VoxelModel& m, int outW, int outH, int frame, float yawScale = 1.0f, float yawBase = 0.0f) {
    SpritePixels img;
    img.w = outW;
    img.h = outH;
    img.px.assign(static_cast<size_t>(outW * outH), {0,0,0,0});

    // Find bounds of filled voxels to auto-zoom.
    int minX = m.w, minY = m.h, minZ = m.d;
    int maxX = -1, maxY = -1, maxZ = -1;
    for (int z = 0; z < m.d; ++z) {
        for (int y = 0; y < m.h; ++y) {
            for (int x = 0; x < m.w; ++x) {
                if (m.at(x,y,z).a == 0) continue;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                minZ = std::min(minZ, z);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
                maxZ = std::max(maxZ, z);
            }
        }
    }
    if (maxX < 0) return img; // empty

    // Pad bounds a bit to make room for lighting/shadow.
    const int pad = 1;
    minX = std::max(minX - pad, 0);
    minY = std::max(minY - pad, 0);
    minZ = std::max(minZ - pad, 0);
    maxX = std::min(maxX + pad, m.w - 1);
    maxY = std::min(maxY + pad, m.h - 1);
    maxZ = std::min(maxZ + pad, m.d - 1);

    Vec3f boundMin = {static_cast<float>(minX), static_cast<float>(minY), static_cast<float>(minZ)};
    Vec3f boundMax = {static_cast<float>(maxX + 1), static_cast<float>(maxY + 1), static_cast<float>(maxZ + 1)};
    Vec3f center = (boundMin + boundMax) * 0.5f;

    // Camera direction with a tiny frame-based wobble.
    //
    // yawBase is used by UI "turntable" previews so we can rotate the camera
    // smoothly around the model without affecting the main in-game sprite frames.
    Vec3f dirBase = normalize({0.70f, -0.42f, 1.0f});
    const float yawWobble = ((frame % 2 == 0) ? -0.10f : +0.10f) * yawScale;
    const float yaw = yawBase + yawWobble;
    // Rotate around Y axis: (x,z) plane.
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    Vec3f dir = normalize({ dirBase.x * cy + dirBase.z * sy, dirBase.y, -dirBase.x * sy + dirBase.z * cy });

    Vec3f upWorld = {0.0f, 1.0f, 0.0f};
    Vec3f right = normalize(cross(dir, upWorld));
    Vec3f up = normalize(cross(right, dir));

    // Light from above-left-front.
    Vec3f lightDir = normalize({-0.55f, 0.85f, -0.45f});
    const float ambient = 0.32f;
    const float diffuse = 0.72f;
    const float specular = 0.38f;
    const float shininess = 18.0f;
    const float rimStrength = 0.16f;

    // Project bounds to screen plane to find extents along right/up.
    float minSx = 1e9f, maxSx = -1e9f;
    float minSy = 1e9f, maxSy = -1e9f;
    auto considerCorner = [&](Vec3f p){
        Vec3f v = p - center;
        float sx2 = dot(v, right);
        float sy2 = dot(v, up);
        minSx = std::min(minSx, sx2); maxSx = std::max(maxSx, sx2);
        minSy = std::min(minSy, sy2); maxSy = std::max(maxSy, sy2);
    };
    considerCorner({boundMin.x, boundMin.y, boundMin.z});
    considerCorner({boundMax.x, boundMin.y, boundMin.z});
    considerCorner({boundMin.x, boundMax.y, boundMin.z});
    considerCorner({boundMin.x, boundMin.y, boundMax.z});
    considerCorner({boundMax.x, boundMax.y, boundMin.z});
    considerCorner({boundMax.x, boundMin.y, boundMax.z});
    considerCorner({boundMin.x, boundMax.y, boundMax.z});
    considerCorner({boundMax.x, boundMax.y, boundMax.z});

    // Give a little breathing room so we don't clip.
    const float padScreen = 0.28f;
    minSx -= padScreen; maxSx += padScreen;
    minSy -= padScreen; maxSy += padScreen;

    const int margin = 2;
    const float dist = 64.0f; // camera backoff
    const Vec3f cameraPos = center - dir * dist;

    auto inBounds = [&](int x, int y, int z)->bool {
        return x >= minX && x <= maxX && y >= minY && y <= maxY && z >= minZ && z <= maxZ;
    };

    auto aabbHit = [&](const Vec3f& o, const Vec3f& d, float& tEnter, float& tExit, Vec3f& nEnter)->bool {
        tEnter = -1e9f; tExit = 1e9f;
        int axisEnter = -1;

        auto slab = [&](float oC, float dC, float mn, float mx, int axis)->bool {
            if (std::abs(dC) < 1e-6f) {
                if (oC < mn || oC > mx) return false;
                return true;
            }
            float t0 = (mn - oC) / dC;
            float t1 = (mx - oC) / dC;
            if (t0 > t1) std::swap(t0, t1);
            if (t0 > tEnter) { tEnter = t0; axisEnter = axis; }
            tExit = std::min(tExit, t1);
            return tExit >= tEnter;
        };

        if (!slab(o.x, d.x, boundMin.x, boundMax.x, 0)) return false;
        if (!slab(o.y, d.y, boundMin.y, boundMax.y, 1)) return false;
        if (!slab(o.z, d.z, boundMin.z, boundMax.z, 2)) return false;
        if (tExit < 0.0f) return false;

        // Entry normal for shading when the very first voxel is hit.
        nEnter = {0,0,0};
        if (axisEnter == 0) nEnter = { static_cast<float>(-signum(d.x)), 0, 0 };
        if (axisEnter == 1) nEnter = { 0, static_cast<float>(-signum(d.y)), 0 };
        if (axisEnter == 2) nEnter = { 0, 0, static_cast<float>(-signum(d.z)) };
        if (nEnter.x == 0 && nEnter.y == 0 && nEnter.z == 0) nEnter = normalize(d) * -1.0f;
        return true;
    };

    auto occ = [&](int x, int y, int z)->float {
        return static_cast<float>(m.at(x,y,z).a) / 255.0f;
    };

    auto smoothNormal = [&](int vx, int vy, int vz, const Vec3f& fallback)->Vec3f {
        Vec3f g {
            occ(vx-1,vy,vz) - occ(vx+1,vy,vz),
            occ(vx,vy-1,vz) - occ(vx,vy+1,vz),
            occ(vx,vy,vz-1) - occ(vx,vy,vz+1)
        };
        if (len(g) <= 1e-3f) return normalize(fallback);
        return normalize(g);
    };

    // Soft shadow by casting a secondary voxel DDA toward the light direction.
    auto traceShadow = [&](const Vec3f& start)->float {
        float tEnter = 0.0f, tExit = 0.0f;
        Vec3f dummyN{0,0,0};
        if (!aabbHit(start, lightDir, tEnter, tExit, dummyN)) return 1.0f;

        float t = std::max(tEnter, 0.0f) + 1e-4f;
        Vec3f p = start + lightDir * t;

        int ix = static_cast<int>(std::floor(p.x));
        int iy = static_cast<int>(std::floor(p.y));
        int iz = static_cast<int>(std::floor(p.z));

        if (!inBounds(ix,iy,iz)) {
            ix = std::clamp(ix, minX, maxX);
            iy = std::clamp(iy, minY, maxY);
            iz = std::clamp(iz, minZ, maxZ);
        }

        const int stepX = (lightDir.x > 0.0f) ? 1 : -1;
        const int stepY = (lightDir.y > 0.0f) ? 1 : -1;
        const int stepZ = (lightDir.z > 0.0f) ? 1 : -1;

        const float invDx = (std::abs(lightDir.x) < 1e-6f) ? 1e9f : (1.0f / std::abs(lightDir.x));
        const float invDy = (std::abs(lightDir.y) < 1e-6f) ? 1e9f : (1.0f / std::abs(lightDir.y));
        const float invDz = (std::abs(lightDir.z) < 1e-6f) ? 1e9f : (1.0f / std::abs(lightDir.z));

        float tMaxX, tMaxY, tMaxZ;
        {
            float nextX = (stepX > 0) ? (static_cast<float>(ix) + 1.0f) : static_cast<float>(ix);
            float nextY = (stepY > 0) ? (static_cast<float>(iy) + 1.0f) : static_cast<float>(iy);
            float nextZ = (stepZ > 0) ? (static_cast<float>(iz) + 1.0f) : static_cast<float>(iz);

            tMaxX = t + ((std::abs(lightDir.x) < 1e-6f) ? 1e9f : ((nextX - p.x) / lightDir.x));
            tMaxY = t + ((std::abs(lightDir.y) < 1e-6f) ? 1e9f : ((nextY - p.y) / lightDir.y));
            tMaxZ = t + ((std::abs(lightDir.z) < 1e-6f) ? 1e9f : ((nextZ - p.z) / lightDir.z));
        }

        float T = 1.0f; // transmittance
        for (int steps = 0; steps < 96; ++steps) {
            if (!inBounds(ix,iy,iz)) break;
            const Color c = m.at(ix,iy,iz);
            if (c.a > 0) {
                // Treat alpha as "density". Opaque voxels quickly kill light; translucent voxels attenuate.
                const float a = static_cast<float>(c.a) / 255.0f;
                T *= (1.0f - 0.85f * a);
                if (T <= 0.08f) return 0.0f;
            }

            // Advance to next voxel boundary.
            if (tMaxX < tMaxY) {
                if (tMaxX < tMaxZ) {
                    t = tMaxX;
                    tMaxX += invDx;
                    ix += stepX;
                } else {
                    t = tMaxZ;
                    tMaxZ += invDz;
                    iz += stepZ;
                }
            } else {
                if (tMaxY < tMaxZ) {
                    t = tMaxY;
                    tMaxY += invDy;
                    iy += stepY;
                } else {
                    t = tMaxZ;
                    tMaxZ += invDz;
                    iz += stepZ;
                }
            }

            if (t > tExit + 1e-3f) break;
        }
        return clampf(T, 0.0f, 1.0f);
    };

    // Hemisphere-based ambient occlusion sampling around the surface normal.
    // This avoids over-darkening broad flat faces (a common issue when AO is
    // derived from "internal" neighbors), while still deepening concave pockets.
    struct AOSample { int dx, dy, dz; float w; };
    static constexpr AOSample AO_SAMPLES[] = {
        // 1-step axis
        {  1,  0,  0, 1.00f }, { -1,  0,  0, 1.00f }, {  0,  1,  0, 1.00f }, {  0, -1,  0, 1.00f }, {  0,  0,  1, 1.00f }, {  0,  0, -1, 1.00f },
        // 1-step edges (sqrt2)
        {  1,  1,  0, 0.85f }, {  1, -1,  0, 0.85f }, { -1,  1,  0, 0.85f }, { -1, -1,  0, 0.85f },
        {  1,  0,  1, 0.85f }, {  1,  0, -1, 0.85f }, { -1,  0,  1, 0.85f }, { -1,  0, -1, 0.85f },
        {  0,  1,  1, 0.85f }, {  0,  1, -1, 0.85f }, {  0, -1,  1, 0.85f }, {  0, -1, -1, 0.85f },
        // 1-step corners (sqrt3)
        {  1,  1,  1, 0.70f }, {  1,  1, -1, 0.70f }, {  1, -1,  1, 0.70f }, {  1, -1, -1, 0.70f },
        { -1,  1,  1, 0.70f }, { -1,  1, -1, 0.70f }, { -1, -1,  1, 0.70f }, { -1, -1, -1, 0.70f },
        // 2-step axis (softens occlusion a little)
        {  2,  0,  0, 0.55f }, { -2,  0,  0, 0.55f }, {  0,  2,  0, 0.55f }, {  0, -2,  0, 0.55f }, {  0,  0,  2, 0.55f }, {  0,  0, -2, 0.55f },
    };

    auto ambientOcclusion = [&](int vx, int vy, int vz, const Vec3f& normal) -> float {
        const Vec3f nn = normalize(normal);

        float occSum = 0.0f;
        float wSum = 0.0f;

        for (const auto& s : AO_SAMPLES) {
            const Vec3f dir = normalize({ static_cast<float>(s.dx), static_cast<float>(s.dy), static_cast<float>(s.dz) });
            const float dp = dot(dir, nn);

            // Only sample the hemisphere in front of the surface; a small threshold
            // avoids noisy "side" contributions on flat faces.
            if (dp <= 0.10f) continue;

            const float o = occ(vx + s.dx, vy + s.dy, vz + s.dz);

            // Weight samples by both their importance and alignment with the normal.
            const float w = s.w * (0.35f + 0.65f * dp);

            occSum += o * w;
            wSum += w;
        }

        const float occAvg = (wSum > 1e-6f) ? (occSum / wSum) : 0.0f;

        // Map [0..1] occlusion to an AO multiplier.
        float ao = 1.0f - occAvg * 0.85f;
        ao = clampf(ao, 0.45f, 1.0f);
        ao = std::pow(ao, 1.25f);
        return ao;
    };

    auto shadeVoxel = [&](Color c, const Vec3f& n, const Vec3f& viewDir, float shadow, int vx, int vy, int vz)->Color {
        Vec3f nn = normalize(n);
        Vec3f vv = normalize(viewDir);

        float ndl = std::max(0.0f, dot(nn, lightDir));
        float shade = ambient + diffuse * ndl * shadow;

        // Hemisphere-based ambient occlusion sampling (surface-facing).
        // Keeps broad exposed faces from being over-darkened, while still
        // deepening concave pockets and creases.
        const float ao = ambientOcclusion(vx, vy, vz, nn);
        shade *= ao;
        shade = clampf(shade, 0.0f, 1.25f);

        // Specular (Blinn-Phong) + rim for readability.
        Vec3f h = normalize(lightDir + vv);
        float spec = std::pow(std::max(0.0f, dot(nn, h)), shininess) * specular * shadow;

        const float vdn = clampf(dot(nn, vv), 0.0f, 1.0f);
        float rim = std::pow(1.0f - vdn, 2.2f) * rimStrength;

        Color out = mul(c, shade);

        const float boost = clampf(spec + rim, 0.0f, 0.85f);
        if (boost > 0.0f) {
            const int addv = static_cast<int>(std::round(255.0f * boost));
            out.r = clamp8(static_cast<int>(out.r) + addv);
            out.g = clamp8(static_cast<int>(out.g) + addv);
            out.b = clamp8(static_cast<int>(out.b) + addv);
        }

        return out;
    };

    // Main render loop (perspective voxel DDA). Still supports translucent voxels via front-to-back compositing.
    for (int py = 0; py < outH; ++py) {
        for (int px = 0; px < outW; ++px) {
            if (px < margin || py < margin || px >= outW - margin || py >= outH - margin) {
                continue;
            }

            const float tx = (static_cast<float>(px) + 0.5f - margin) / static_cast<float>(outW - 2*margin);
            const float ty = (static_cast<float>(py) + 0.5f - margin) / static_cast<float>(outH - 2*margin);

            const float sx = minSx + tx * (maxSx - minSx);
            const float sy2 = maxSy - ty * (maxSy - minSy);

            // Perspective: constant camera origin; rays go through a screen plane passing through "center".
            Vec3f screenPoint = center + right * sx + up * sy2;
            Vec3f rayDir = normalize(screenPoint - cameraPos);
            Vec3f origin = cameraPos;

            float tEnter = 0.0f, tExit = 0.0f;
            Vec3f nEnter = {0,0,0};
            if (!aabbHit(origin, rayDir, tEnter, tExit, nEnter)) continue;

            float t = std::max(tEnter, 0.0f) + 1e-4f;
            Vec3f p = origin + rayDir * t;

            int ix = static_cast<int>(std::floor(p.x));
            int iy = static_cast<int>(std::floor(p.y));
            int iz = static_cast<int>(std::floor(p.z));

            if (!inBounds(ix,iy,iz)) {
                // Clamp just in case of precision issues.
                ix = std::clamp(ix, minX, maxX);
                iy = std::clamp(iy, minY, maxY);
                iz = std::clamp(iz, minZ, maxZ);
            }

            const int stepX = (rayDir.x > 0.0f) ? 1 : -1;
            const int stepY = (rayDir.y > 0.0f) ? 1 : -1;
            const int stepZ = (rayDir.z > 0.0f) ? 1 : -1;

            const float invDx = (std::abs(rayDir.x) < 1e-6f) ? 1e9f : (1.0f / std::abs(rayDir.x));
            const float invDy = (std::abs(rayDir.y) < 1e-6f) ? 1e9f : (1.0f / std::abs(rayDir.y));
            const float invDz = (std::abs(rayDir.z) < 1e-6f) ? 1e9f : (1.0f / std::abs(rayDir.z));

            float tMaxX, tMaxY, tMaxZ;
            {
                float nextX = (stepX > 0) ? (static_cast<float>(ix) + 1.0f) : static_cast<float>(ix);
                float nextY = (stepY > 0) ? (static_cast<float>(iy) + 1.0f) : static_cast<float>(iy);
                float nextZ = (stepZ > 0) ? (static_cast<float>(iz) + 1.0f) : static_cast<float>(iz);

                tMaxX = t + ((std::abs(rayDir.x) < 1e-6f) ? 1e9f : ((nextX - p.x) / rayDir.x));
                tMaxY = t + ((std::abs(rayDir.y) < 1e-6f) ? 1e9f : ((nextY - p.y) / rayDir.y));
                tMaxZ = t + ((std::abs(rayDir.z) < 1e-6f) ? 1e9f : ((nextZ - p.z) / rayDir.z));
            }

            Vec3f faceNormal = nEnter;

            // Accumulated front-to-back compositing (premultiplied).
            float outA = 0.0f;
            float outR = 0.0f, outG = 0.0f, outB = 0.0f;

            // Hard cap steps to avoid any infinite loops.
            for (int steps = 0; steps < 256; ++steps) {
                if (!inBounds(ix,iy,iz)) break;

                Color c = m.at(ix,iy,iz);
                if (c.a > 0) {
                    const Vec3f nn = smoothNormal(ix,iy,iz, faceNormal);
                    const Vec3f voxCenter = { static_cast<float>(ix) + 0.5f, static_cast<float>(iy) + 0.5f, static_cast<float>(iz) + 0.5f };

                    // Shadow: offset slightly toward the surface normal and toward the light to reduce acne.
                    const Vec3f shadowStart = voxCenter + nn * 0.56f + lightDir * 0.02f;
                    const float shadow = traceShadow(shadowStart);

                    Color shaded = shadeVoxel(c, nn, (cameraPos - voxCenter), shadow, ix,iy,iz);

                    const float a = shaded.a / 255.0f;
                    const float oneMinusA = (1.0f - outA);
                    outR += (shaded.r / 255.0f) * a * oneMinusA;
                    outG += (shaded.g / 255.0f) * a * oneMinusA;
                    outB += (shaded.b / 255.0f) * a * oneMinusA;
                    outA += a * oneMinusA;

                    // Early termination once we're effectively opaque.
                    if (outA >= 0.97f || shaded.a >= 245) {
                        break;
                    }
                }

                // Advance to next voxel boundary.
                if (tMaxX < tMaxY) {
                    if (tMaxX < tMaxZ) {
                        t = tMaxX;
                        tMaxX += invDx;
                        ix += stepX;
                        faceNormal = { static_cast<float>(-stepX), 0, 0 };
                    } else {
                        t = tMaxZ;
                        tMaxZ += invDz;
                        iz += stepZ;
                        faceNormal = { 0, 0, static_cast<float>(-stepZ) };
                    }
                } else {
                    if (tMaxY < tMaxZ) {
                        t = tMaxY;
                        tMaxY += invDy;
                        iy += stepY;
                        faceNormal = { 0, static_cast<float>(-stepY), 0 };
                    } else {
                        t = tMaxZ;
                        tMaxZ += invDz;
                        iz += stepZ;
                        faceNormal = { 0, 0, static_cast<float>(-stepZ) };
                    }
                }

                if (t > tExit + 1e-3f) break;
            }

            if (outA <= 0.0f) continue;

            // Convert back to straight-alpha.
            const float invA = (outA > 1e-6f) ? (1.0f / outA) : 0.0f;
            Color out;
            out.a = clamp8(static_cast<int>(std::round(outA * 255.0f)));
            out.r = clamp8(static_cast<int>(std::round(outR * invA * 255.0f)));
            out.g = clamp8(static_cast<int>(std::round(outG * invA * 255.0f)));
            out.b = clamp8(static_cast<int>(std::round(outB * invA * 255.0f)));

            img.px[static_cast<size_t>(py*outW + px)] = out;
        }
    }
    // A small "contact shadow" in screen-space for extra depth.
    // Shadow direction matches the light (down-right). We stamp a short falloff
    // chain so sprites feel grounded without looking blurry.
    SpritePixels withShadow = img;

    auto stampShadow = [&](int sx, int sy, uint8_t a) {
        if (a == 0) return;
        if (sx < 0 || sy < 0 || sx >= outW || sy >= outH) return;
        if (img.at(sx, sy).a != 0) return; // never paint over the sprite itself

        Color& d = withShadow.at(sx, sy);
        if (d.a == 0) {
            d = {0,0,0,a};
        } else if (d.r == 0 && d.g == 0 && d.b == 0) {
            if (a > d.a) d.a = a;
        }
    };

    for (int y = outH - 1; y >= 0; --y) {
        for (int x = outW - 1; x >= 0; --x) {
            const Color c = img.at(x,y);
            if (c.a == 0) continue;

            const float oa = static_cast<float>(c.a) / 255.0f;

            const uint8_t a1 = clamp8(static_cast<int>(std::round(74.0f * oa)));
            const uint8_t a2 = clamp8(static_cast<int>(std::round(44.0f * oa)));
            const uint8_t a3 = clamp8(static_cast<int>(std::round(26.0f * oa)));
            const uint8_t aSide = clamp8(static_cast<int>(std::round(28.0f * oa)));

            stampShadow(x + 1, y + 1, a1);
            stampShadow(x + 2, y + 2, a2);
            stampShadow(x + 3, y + 3, a3);

            // Small lateral spread so the shadow isn't a single-pixel staircase.
            stampShadow(x + 1, y + 2, aSide);
            stampShadow(x + 2, y + 1, aSide);
        }
    }

    return withShadow;
}




SpritePixels downscale2x(const SpritePixels& hi) {
    SpritePixels lo;
    lo.w = hi.w / 2;
    lo.h = hi.h / 2;
    lo.px.assign(static_cast<size_t>(lo.w * lo.h), {0,0,0,0});

    for (int y = 0; y < lo.h; ++y) {
        for (int x = 0; x < lo.w; ++x) {
            // 2x2 block
            uint32_t sumA = 0;
            uint32_t sumR = 0, sumG = 0, sumB = 0;
            for (int oy = 0; oy < 2; ++oy) {
                for (int ox = 0; ox < 2; ++ox) {
                    const Color c = hi.at(x*2 + ox, y*2 + oy);
                    sumA += c.a;
                    sumR += static_cast<uint32_t>(c.r) * c.a;
                    sumG += static_cast<uint32_t>(c.g) * c.a;
                    sumB += static_cast<uint32_t>(c.b) * c.a;
                }
            }
            Color out = {0,0,0,0};
            if (sumA > 0) {
                const float invA = 1.0f / static_cast<float>(sumA);
                out.a = clamp8(static_cast<int>(std::round(sumA / 4.0f)));
                out.r = clamp8(static_cast<int>(std::round(static_cast<float>(sumR) * invA)));
                out.g = clamp8(static_cast<int>(std::round(static_cast<float>(sumG) * invA)));
                out.b = clamp8(static_cast<int>(std::round(static_cast<float>(sumB) * invA)));
            }
            lo.px[static_cast<size_t>(y*lo.w + x)] = out;
        }
    }
    return lo;
}

void addOutline(SpritePixels& s) {
    SpritePixels src = s;
    auto isSolid = [&](int x, int y)->bool {
        if (x < 0 || y < 0 || x >= src.w || y >= src.h) return false;
        return src.at(x,y).a > 0;
    };
    for (int y = 0; y < s.h; ++y) {
        for (int x = 0; x < s.w; ++x) {
            if (src.at(x,y).a > 0) continue;
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; ++dy) {
                for (int dx = -1; dx <= 1 && !near; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    if (isSolid(x+dx, y+dy)) near = true;
                }
            }
            if (near) {
                s.at(x,y) = {0,0,0,200};
            }
        }
    }
}

inline int clampOutPx(int outPx) {
    return std::clamp(outPx, 16, 256);
}

// Pick a voxel-model upscaling factor based on the requested output size.
//
// The procedural voxel models are authored at a small canonical resolution
// (typically 16×16×8). Rendering those directly into large outputs (64×64,
// 128×128) makes each voxel span many screen pixels, producing a "chunky"
// look. We fix that by upscaling the *voxel model* (nearest-neighbor), keeping
// the same overall silhouette while increasing face density for smoother
// isometric rasterization.
//
// NOTE: Isometric raytracing is significantly more expensive than the mesh
// rasterizer, so we cap the detail factor there to keep runtime costs sane.
inline int voxelDetailScaleForOutPx(int outPx, bool isoRaytrace) {
    outPx = clampOutPx(outPx);
    int s = 1;
    if (outPx >= 128) s = 4;
    else if (outPx >= 64) s = 2;
    if (isoRaytrace && s > 2) s = 2;
    return s;
}

SpritePixels renderModelToSprite(const VoxelModel& model, int frame, float yawScale, int outPx) {
    outPx = clampOutPx(outPx);

    // Small sprites benefit a lot from 2x supersampling + downscale.
    const int hiW = (outPx <= 32) ? outPx * 2 : outPx;
    const int hiH = hiW;

    SpritePixels hi = renderVoxel(model, /*outW=*/hiW, /*outH=*/hiH, frame, yawScale);
    SpritePixels out = (hiW == outPx) ? hi : downscale2x(hi);

    // Keep edges crisp and readable in tiny tile sizes.
    if (outPx <= 32) addOutline(out);
    return out;
}

SpritePixels renderModelToSpriteTurntable(const VoxelModel& model, int frame, float yawRad, int outPx) {
    outPx = clampOutPx(outPx);

    // Same sampling rules as the normal sprite path.
    const int hiW = (outPx <= 32) ? outPx * 2 : outPx;
    const int hiH = hiW;

    // Turntable previews want deterministic yaw, not the in-game wobble.
    SpritePixels hi = renderVoxel(model, /*outW=*/hiW, /*outH=*/hiH, frame,
                                 /*yawScale=*/0.0f, /*yawBase=*/yawRad);
    SpritePixels out = (hiW == outPx) ? hi : downscale2x(hi);

    if (outPx <= 32) addOutline(out);
    return out;
}


// --- Isometric voxel renderer -------------------------------------------------
//
// Converts a voxel volume into a small, projected 2D mesh (quads/triangles) using
// a 2:1 dimetric/isometric projection, then rasterizes it back into a SpritePixels.
//
// This renderer is used for isometric view mode so 3D voxel sprites align with the
// isometric terrain projection.

enum class IsoFaceType : uint8_t { Left = 0, Right = 1, Top = 2 };

struct IsoQuad {
    std::array<Vec2f, 4> v;    // projected (unscaled) quad vertices
    std::array<float, 4> z{};  // depth at each vertex (larger = closer)
    Color c{0,0,0,0};
};

inline Vec2f isoProject(float x, float y, float z) {
    // 2:1 dimetric projection:
    //   +X goes down-right, +Z goes down-left, +Y goes up.
    // Units are voxel units; we scale/translate to output pixels later.
    return { (x - z), (x + z) * 0.5f - y };
}

Color shadeIsoFace(Color base, IsoFaceType t, const VoxelModel& m, int x, int y, int z) {
    // Stylized isometric lighting.
    //
    // The isometric camera in ProcRogue shows the Top (+Y), Right (+X) and Left (+Z) faces.
    // We keep shading "pixel-art friendly" (mostly flat per face), but add:
    //   - A consistent directional light (Lambertian diffuse + ambient)
    //   - A small, cheap ambient-occlusion term so concavities read deeper
    //   - A tiny specular + rim boost to help silhouettes pop in the 2.5D view
    //
    // IMPORTANT: We quantize the final multiplier so greedy face merging can still
    // collapse large uniform surfaces into big quads (performance + cleaner edges).

    const Vec3f lightDir = normalize({0.58f, 0.80f, 0.28f}); // above + slightly from the "front/right"
    const Vec3f viewDir  = normalize({0.62f, 0.78f, 0.62f}); // toward the isometric camera

    constexpr float ambient   = 0.40f;
    constexpr float diffuse   = 0.72f;
    constexpr float specular  = 0.22f;
    constexpr float shininess = 22.0f;
    constexpr float rimStrength = 0.10f;

    Vec3f n{0.0f, 1.0f, 0.0f};
    switch (t) {
        case IsoFaceType::Top:   n = {0.0f, 1.0f, 0.0f}; break;
        case IsoFaceType::Right: n = {1.0f, 0.0f, 0.0f}; break;
        case IsoFaceType::Left:  n = {0.0f, 0.0f, 1.0f}; break;
        default: break;
    }

    const float ndl = std::max(0.0f, dot(n, lightDir));
    float shade = ambient + diffuse * ndl;

    // Occupancy as "density" (alpha) so translucent voxels occlude less.
    auto occ = [&](int dx, int dy, int dz) -> float {
        return static_cast<float>(m.at(x + dx, y + dy, z + dz).a) / 255.0f;
    };

    float occSum = 0.0f;
    float wSum = 0.0f;
    auto sample = [&](int dx, int dy, int dz, float w) {
        occSum += occ(dx, dy, dz) * w;
        wSum += w;
    };

    // Cheap AO sampling tuned per face.
    // Side faces care a lot about "overhang" voxels above them.
    if (t == IsoFaceType::Top) {
        // Nearby voxels just above the top face plane.
        sample(-1, 1,  0, 1.00f);
        sample( 1, 1,  0, 1.00f);
        sample( 0, 1, -1, 1.00f);
        sample( 0, 1,  1, 1.00f);

        sample(-1, 1, -1, 0.70f);
        sample(-1, 1,  1, 0.70f);
        sample( 1, 1, -1, 0.70f);
        sample( 1, 1,  1, 0.70f);

        // A little from "two above" to deepen tall stacks.
        sample(0, 2, 0, 0.55f);
    } else if (t == IsoFaceType::Right) {
        // Direct overhang above the face is the most important.
        sample(0, 1, 0, 1.25f);
        sample(0, 2, 0, 0.45f);

        // Overhangs on the top edge and corners.
        sample(0, 1, -1, 0.75f);
        sample(0, 1,  1, 0.75f);
        sample(1, 1,  0, 0.85f);
        sample(1, 1, -1, 0.50f);
        sample(1, 1,  1, 0.50f);
    } else { // IsoFaceType::Left
        sample(0, 1, 0, 1.25f);
        sample(0, 2, 0, 0.45f);

        sample(-1, 1, 0, 0.55f);
        sample( 1, 1, 0, 0.55f);
        sample(0, 1, 1, 0.90f);
        sample(-1, 1, 1, 0.55f);
        sample( 1, 1, 1, 0.55f);
    }

    const float occAvg = (wSum > 1e-6f) ? (occSum / wSum) : 0.0f;
    const float aoStrength = (t == IsoFaceType::Top) ? 0.45f : 0.62f;

    float ao = 1.0f - occAvg * aoStrength;
    ao = clampf(ao, 0.55f, 1.0f);

    // Quantize to keep merges stable and reduce tiny-sprite shimmer.
    ao = std::round(ao * 16.0f) / 16.0f;

    shade *= ao;
    shade = clampf(shade, 0.35f, 1.25f);
    shade = std::round(shade * 32.0f) / 32.0f;

    Color out = mul(base, shade);

    // Tiny spec + rim to help 3D readability.
    const Vec3f h = normalize(lightDir + viewDir);
    float spec = std::pow(std::max(0.0f, dot(n, h)), shininess) * specular;

    const float vdn = clampf(dot(n, viewDir), 0.0f, 1.0f);
    float rim = std::pow(1.0f - vdn, 2.2f) * rimStrength;

    float boost = clampf(spec + rim, 0.0f, 0.65f);

    // Don't over-boost very translucent materials (ghost/slime/etc.).
    const float aF = static_cast<float>(base.a) / 255.0f;
    boost *= (0.35f + 0.65f * aF);

    if (boost > 0.0f) {
        const int addv = static_cast<int>(std::round(255.0f * boost));
        out.r = clamp8(static_cast<int>(out.r) + addv);
        out.g = clamp8(static_cast<int>(out.g) + addv);
        out.b = clamp8(static_cast<int>(out.b) + addv);
    }

    return out;
}

inline void blendOver(Color& dst, const Color& src) {
    const int sa = static_cast<int>(src.a);
    if (sa <= 0) return;
    if (sa >= 255) {
        dst = src;
        return;
    }

    const int da = static_cast<int>(dst.a);
    const int inv = 255 - sa;

    // Straight-alpha blend, computed via premultiplied intermediates.
    const int outA = sa + (da * inv + 127) / 255;

    const int srcRp = static_cast<int>(src.r) * sa;
    const int srcGp = static_cast<int>(src.g) * sa;
    const int srcBp = static_cast<int>(src.b) * sa;

    const int dstRp = static_cast<int>(dst.r) * da;
    const int dstGp = static_cast<int>(dst.g) * da;
    const int dstBp = static_cast<int>(dst.b) * da;

    const int outRp = srcRp + (dstRp * inv + 127) / 255;
    const int outGp = srcGp + (dstGp * inv + 127) / 255;
    const int outBp = srcBp + (dstBp * inv + 127) / 255;

    Color out{0, 0, 0, 0};
    out.a = clamp8(outA);
    if (outA > 0) {
        out.r = clamp8((outRp + outA / 2) / outA);
        out.g = clamp8((outGp + outA / 2) / outA);
        out.b = clamp8((outBp + outA / 2) / outA);
    }
    dst = out;
}

inline bool sameColor(const Color& a, const Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// Greedy rectangle merge on a 2D face mask.
//
// The grid is indexed row-major as (v * dimU + u).
// Cells with alpha==0 are treated as empty.
template <typename EmitFn>
void greedyMerge2D(int dimU, int dimV, const std::vector<Color>& cells, EmitFn emit) {
    if (dimU <= 0 || dimV <= 0) return;
    if (static_cast<int>(cells.size()) != dimU * dimV) return;

    std::vector<uint8_t> used(static_cast<size_t>(dimU * dimV), 0);

    auto idx = [&](int u, int v) -> size_t {
        return static_cast<size_t>(v * dimU + u);
    };

    for (int v = 0; v < dimV; ++v) {
        for (int u = 0; u < dimU; ++u) {
            const size_t i = idx(u, v);
            if (used[i]) continue;
            const Color c = cells[i];
            if (c.a == 0) continue;

            // Expand width (u).
            int w = 1;
            while (u + w < dimU) {
                const size_t j = idx(u + w, v);
                if (used[j]) break;
                if (!sameColor(cells[j], c)) break;
                ++w;
            }

            // Expand height (v).
            int h = 1;
            bool stop = false;
            while (!stop && v + h < dimV) {
                for (int du = 0; du < w; ++du) {
                    const size_t j = idx(u + du, v + h);
                    if (used[j] || !sameColor(cells[j], c)) {
                        stop = true;
                        break;
                    }
                }
                if (!stop) ++h;
            }

            for (int dv = 0; dv < h; ++dv) {
                for (int du = 0; du < w; ++du) {
                    used[idx(u + du, v + dv)] = 1;
                }
            }

            emit(u, v, w, h, c);
        }
    }
}

SpritePixels renderVoxelIsometric(const VoxelModel& m, int outW, int outH, int frame) {
    (void)frame;

    std::vector<IsoQuad> quads;
    quads.reserve(static_cast<size_t>(m.w * m.h * m.d));

    float minX = 1e9f, minY = 1e9f;
    float maxX = -1e9f, maxY = -1e9f;

    auto consider = [&](const Vec2f& p) {
        if (p.x < minX) minX = p.x;
        if (p.x > maxX) maxX = p.x;
        if (p.y < minY) minY = p.y;
        if (p.y > maxY) maxY = p.y;
    };

    auto pushQuad = [&](const std::array<Vec3f, 4>& p3, Color c) {
        IsoQuad q;
        q.c = c;
        for (int i = 0; i < 4; ++i) {
            q.v[i] = isoProject(p3[i].x, p3[i].y, p3[i].z);
            q.z[i] = p3[i].x + p3[i].y + p3[i].z;
            consider(q.v[i]);
        }
        quads.push_back(q);
    };

    // Build merged surface quads per face orientation.
    // We only generate the 3 faces visible in the game's isometric view: Top (+Y), Right (+X), Left (+Z).
    //
    // NOTE: Merge key is the *final shaded* face color so we don't merge across AO/shading boundaries.
    //       This keeps the look consistent while still reducing micro-face spam on uniform surfaces
    //       (e.g., potion glass, smooth slimes, etc.).
    //
    // Top faces: per y-slice, merge in (x,z).
    for (int y = 0; y < m.h; ++y) {
        std::vector<Color> cells(static_cast<size_t>(m.w * m.d), {0,0,0,0});
        for (int z = 0; z < m.d; ++z) {
            for (int x = 0; x < m.w; ++x) {
                const Color vox = m.at(x, y, z);
                if (vox.a == 0) continue;
                if (isFilled(m, x, y + 1, z)) continue;
                cells[static_cast<size_t>(z * m.w + x)] = shadeIsoFace(vox, IsoFaceType::Top, m, x, y, z);
            }
        }

        greedyMerge2D(m.w, m.d, cells, [&](int x0, int z0, int w, int h, const Color& c) {
            const float fx0 = static_cast<float>(x0);
            const float fz0 = static_cast<float>(z0);
            const float fx1 = static_cast<float>(x0 + w);
            const float fz1 = static_cast<float>(z0 + h);
            const float fy = static_cast<float>(y + 1);

            std::array<Vec3f, 4> p3{
                Vec3f{fx0, fy, fz0},
                Vec3f{fx1, fy, fz0},
                Vec3f{fx1, fy, fz1},
                Vec3f{fx0, fy, fz1},
            };
            pushQuad(p3, c);
        });
    }

    // Right faces: per x-slice, merge in (z,y).
    for (int x = 0; x < m.w; ++x) {
        const int dimU = m.d; // z
        const int dimV = m.h; // y
        std::vector<Color> cells(static_cast<size_t>(dimU * dimV), {0,0,0,0});
        for (int y = 0; y < m.h; ++y) {
            for (int z = 0; z < m.d; ++z) {
                const Color vox = m.at(x, y, z);
                if (vox.a == 0) continue;
                if (isFilled(m, x + 1, y, z)) continue;
                cells[static_cast<size_t>(y * dimU + z)] = shadeIsoFace(vox, IsoFaceType::Right, m, x, y, z);
            }
        }

        greedyMerge2D(dimU, dimV, cells, [&](int z0, int y0, int w, int h, const Color& c) {
            const float fx = static_cast<float>(x + 1);
            const float fz0 = static_cast<float>(z0);
            const float fy0 = static_cast<float>(y0);
            const float fz1 = static_cast<float>(z0 + w);
            const float fy1 = static_cast<float>(y0 + h);

            std::array<Vec3f, 4> p3{
                Vec3f{fx, fy0, fz0},
                Vec3f{fx, fy1, fz0},
                Vec3f{fx, fy1, fz1},
                Vec3f{fx, fy0, fz1},
            };
            pushQuad(p3, c);
        });
    }

    // Left faces: per z-slice, merge in (x,y).
    for (int z = 0; z < m.d; ++z) {
        const int dimU = m.w; // x
        const int dimV = m.h; // y
        std::vector<Color> cells(static_cast<size_t>(dimU * dimV), {0,0,0,0});
        for (int y = 0; y < m.h; ++y) {
            for (int x = 0; x < m.w; ++x) {
                const Color vox = m.at(x, y, z);
                if (vox.a == 0) continue;
                if (isFilled(m, x, y, z + 1)) continue;
                cells[static_cast<size_t>(y * dimU + x)] = shadeIsoFace(vox, IsoFaceType::Left, m, x, y, z);
            }
        }

        greedyMerge2D(dimU, dimV, cells, [&](int x0, int y0, int w, int h, const Color& c) {
            const float fz = static_cast<float>(z + 1);
            const float fx0 = static_cast<float>(x0);
            const float fy0 = static_cast<float>(y0);
            const float fx1 = static_cast<float>(x0 + w);
            const float fy1 = static_cast<float>(y0 + h);

            std::array<Vec3f, 4> p3{
                Vec3f{fx0, fy0, fz},
                Vec3f{fx0, fy1, fz},
                Vec3f{fx1, fy1, fz},
                Vec3f{fx1, fy0, fz},
            };
            pushQuad(p3, c);
        });
    }

    SpritePixels img;
    img.w = std::max(1, outW);
    img.h = std::max(1, outH);
    img.px.assign(static_cast<size_t>(img.w * img.h), {0, 0, 0, 0});

    if (quads.empty()) return img;

    const int margin = std::clamp(outW / 16, 1, 6);
    const float bbW = std::max(1e-3f, maxX - minX);
    const float bbH = std::max(1e-3f, maxY - minY);

    const float availW = static_cast<float>(outW - margin * 2);
    const float availH = static_cast<float>(outH - margin * 2);
    const float scale = std::max(1e-3f, std::min(availW / bbW, availH / bbH));

    // Center horizontally; align bottom to the sprite bottom margin.
    const float offX = static_cast<float>(margin) - minX * scale + (availW - bbW * scale) * 0.5f;
    const float offY = static_cast<float>(outH - margin) - maxY * scale;

    auto xf = [&](Vec2f p) -> Vec2f { return { p.x * scale + offX, p.y * scale + offY }; };

    // Convert quads to a 2D triangle mesh and rasterize.
    Mesh2D mesh;
    mesh.tris.reserve(quads.size() * 2);

    for (const IsoQuad& q : quads) {
        const Vec2f p0 = xf(q.v[0]);
        const Vec2f p1 = xf(q.v[1]);
        const Vec2f p2 = xf(q.v[2]);
        const Vec2f p3 = xf(q.v[3]);

        Mesh2DTriangle t0;
        t0.p0 = p0; t0.p1 = p1; t0.p2 = p2;
        t0.z0 = q.z[0]; t0.z1 = q.z[1]; t0.z2 = q.z[2];
        t0.c = q.c;
        mesh.tris.push_back(t0);

        Mesh2DTriangle t1;
        t1.p0 = p0; t1.p1 = p2; t1.p2 = p3;
        t1.z0 = q.z[0]; t1.z1 = q.z[2]; t1.z2 = q.z[3];
        t1.c = q.c;
        mesh.tris.push_back(t1);
    }

    img = rasterizeMesh2D(mesh, img.w, img.h);

    // Soft contact shadow: stamp onto transparent pixels below/around the sprite mass.
    SpritePixels withShadow = img;
    auto isSolid = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= img.w || y >= img.h) return false;
        return img.at(x, y).a > 0;
    };

    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            if (img.at(x, y).a > 0) continue;

            // Look for nearby solid pixels above/left (light from top-left).
            bool near = false;
            for (int dy = -2; dy <= 0 && !near; ++dy) {
                for (int dx = -2; dx <= 0 && !near; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    if (isSolid(x + dx, y + dy)) near = true;
                }
            }

            if (near) {
                // Slightly stronger at the very bottom of the sprite.
                const float t = static_cast<float>(y) / std::max(1, img.h - 1);
                const int a = static_cast<int>(60 + 80 * t);
                blendOver(withShadow.at(x, y), {0, 0, 0, clamp8(a)});
            }
        }
    }

    return withShadow;
}


SpritePixels renderVoxelIsometricRaytrace(const VoxelModel& m, int outW, int outH, int frame) {
    (void)frame;

    SpritePixels img;
    img.w = std::max(1, outW);
    img.h = std::max(1, outH);
    img.px.assign(static_cast<size_t>(img.w * img.h), {0, 0, 0, 0});

    if (m.w <= 0 || m.h <= 0 || m.d <= 0) return img;

    // Compute tight bounds of filled voxels.
    int minX = m.w, minY = m.h, minZ = m.d;
    int maxX = -1, maxY = -1, maxZ = -1;
    for (int y = 0; y < m.h; ++y) {
        for (int z = 0; z < m.d; ++z) {
            for (int x = 0; x < m.w; ++x) {
                if (m.at(x, y, z).a == 0) continue;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                minZ = std::min(minZ, z);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
                maxZ = std::max(maxZ, z);
            }
        }
    }
    if (maxX < minX || maxY < minY || maxZ < minZ) return img;

    // Expand bounds slightly so silhouettes don't get clipped.
    constexpr int pad = 2;
    const Vec3f boundMin{
        static_cast<float>(minX - pad),
        static_cast<float>(minY - pad),
        static_cast<float>(minZ - pad),
    };
    const Vec3f boundMax{
        static_cast<float>(maxX + 1 + pad),
        static_cast<float>(maxY + 1 + pad),
        static_cast<float>(maxZ + 1 + pad),
    };

    // Project the 8 AABB corners into iso space to compute a 2D fit transform.
    float minIsoX = 1e9f, maxIsoX = -1e9f;
    float minIsoY = 1e9f, maxIsoY = -1e9f;
    auto addCorner = [&](float x, float y, float z) {
        const Vec2f p = isoProject(x, y, z);
        minIsoX = std::min(minIsoX, p.x);
        maxIsoX = std::max(maxIsoX, p.x);
        minIsoY = std::min(minIsoY, p.y);
        maxIsoY = std::max(maxIsoY, p.y);
    };

    const float xs[2] = {boundMin.x, boundMax.x};
    const float ys[2] = {boundMin.y, boundMax.y};
    const float zs[2] = {boundMin.z, boundMax.z};
    for (float x : xs) for (float y : ys) for (float z : zs) addCorner(x, y, z);

    const int margin = std::clamp(outW / 16, 1, 6);
    const float bbW = std::max(1e-3f, maxIsoX - minIsoX);
    const float bbH = std::max(1e-3f, maxIsoY - minIsoY);

    const float availW = static_cast<float>(outW - margin * 2);
    const float availH = static_cast<float>(outH - margin * 2);
    const float scale = std::max(1e-3f, std::min(availW / bbW, availH / bbH));

    // Center horizontally; align the bottom-most projected point to the sprite bottom margin.
    const float offX = static_cast<float>(margin) - minIsoX * scale + (availW - bbW * scale) * 0.5f;
    const float offY = static_cast<float>(outH - margin) - maxIsoY * scale;

    // --- Custom orthographic isometric voxel raytracer ---
    // We shoot a constant-direction ray (toward -1,-1,-1) for each pixel.
    //
    // Our isometric projection is:
    //   isoX = x - z
    //   isoY = 0.5(x+z) - y
    //
    // A convenient inverse (up to the view direction) is:
    //   p = isoX*(0.5,0,-0.5) + isoY*(0,-1,0) + t*(1,1,1)
    //
    // where t moves along the view direction and does not change the projected iso coords.
    const Vec3f isoXVec{0.5f, 0.0f, -0.5f};
    const Vec3f isoYVec{0.0f, -1.0f, 0.0f};
    const Vec3f viewVec{1.0f, 1.0f, 1.0f};
    const Vec3f rayDir = normalize(Vec3f{-1.0f, -1.0f, -1.0f});
    const Vec3f viewDir = normalize(viewVec);

    // Lighting constants chosen to match shadeIsoFace().
    const Vec3f lightDir = normalize(Vec3f{0.58f, 0.80f, 0.28f});
    constexpr float ambient = 0.40f;
    constexpr float diffuse = 0.72f;
    constexpr float specular = 0.22f;
    constexpr float shininess = 22.0f;
    constexpr float rimStrength = 0.10f;

    // Distance along +viewVec used to place the orthographic camera plane "in front"
    // of the voxel bounds for *all* rays.
    float dist = 0.0f;
    auto consider = [&](float isoX, float isoY) {
        const Vec3f p = isoXVec * isoX + isoYVec * isoY;
        dist = std::max(dist, (boundMax.x + 1.0f) - p.x);
        dist = std::max(dist, (boundMax.y + 1.0f) - p.y);
        dist = std::max(dist, (boundMax.z + 1.0f) - p.z);
    };
    consider(minIsoX, minIsoY);
    consider(minIsoX, maxIsoY);
    consider(maxIsoX, minIsoY);
    consider(maxIsoX, maxIsoY);
    dist += 2.0f;

    auto occ = [&](int x, int y, int z) -> float {
        return static_cast<float>(m.at(x, y, z).a) / 255.0f;
    };

    auto smoothNormal = [&](int x, int y, int z, Vec3f fallback) -> Vec3f {
        const float dx = occ(x - 1, y, z) - occ(x + 1, y, z);
        const float dy = occ(x, y - 1, z) - occ(x, y + 1, z);
        const float dz = occ(x, y, z - 1) - occ(x, y, z + 1);
        Vec3f n{dx, dy, dz};
        if (dot(n, n) < 1e-6f) return normalize(fallback);
        return normalize(n);
    };

    auto aabbHit = [&](const Vec3f& ro, const Vec3f& rd, float& t0, float& t1, Vec3f& hitN) -> bool {
        float tmin = -1e9f;
        float tmax =  1e9f;
        Vec3f n{0, 0, 0};

        auto axis = [&](float roC, float rdC, float mnC, float mxC, int axisIdx) -> bool {
            if (std::abs(rdC) < 1e-6f) {
                if (roC < mnC || roC > mxC) return false;
                return true;
            }
            const float inv = 1.0f / rdC;
            float tA = (mnC - roC) * inv;
            float tB = (mxC - roC) * inv;
            float sign = -1.0f;
            if (tA > tB) { std::swap(tA, tB); sign = 1.0f; }

            if (tA > tmin) {
                tmin = tA;
                n = {0, 0, 0};
                if (axisIdx == 0) n.x = sign;
                else if (axisIdx == 1) n.y = sign;
                else n.z = sign;
            }

            tmax = std::min(tmax, tB);
            if (tmin > tmax) return false;
            return true;
        };

        if (!axis(ro.x, rd.x, boundMin.x, boundMax.x, 0)) return false;
        if (!axis(ro.y, rd.y, boundMin.y, boundMax.y, 1)) return false;
        if (!axis(ro.z, rd.z, boundMin.z, boundMax.z, 2)) return false;

        t0 = tmin;
        t1 = tmax;
        hitN = n;
        return t1 >= 0.0f;
    };

    auto traceShadow = [&](Vec3f ro, Vec3f n) -> float {
        // Offset to avoid self-shadow on the originating voxel face.
        ro = ro + n * 0.04f;

        float t0 = 0.0f, t1 = 0.0f;
        Vec3f hitN{0, 0, 0};
        if (!aabbHit(ro, lightDir, t0, t1, hitN)) return 1.0f;

        float t = std::max(0.0f, t0) + 1e-4f;
        Vec3f p = ro + lightDir * t;

        int x = static_cast<int>(std::floor(p.x));
        int y = static_cast<int>(std::floor(p.y));
        int z = static_cast<int>(std::floor(p.z));

        const int stepX = (lightDir.x > 0.0f) ? 1 : -1;
        const int stepY = (lightDir.y > 0.0f) ? 1 : -1;
        const int stepZ = (lightDir.z > 0.0f) ? 1 : -1;

        const float invDx = 1.0f / std::max(1e-6f, std::abs(lightDir.x));
        const float invDy = 1.0f / std::max(1e-6f, std::abs(lightDir.y));
        const float invDz = 1.0f / std::max(1e-6f, std::abs(lightDir.z));

        const float nextX = (stepX > 0) ? static_cast<float>(x + 1) : static_cast<float>(x);
        const float nextY = (stepY > 0) ? static_cast<float>(y + 1) : static_cast<float>(y);
        const float nextZ = (stepZ > 0) ? static_cast<float>(z + 1) : static_cast<float>(z);

        float tMaxX = t + (nextX - p.x) / lightDir.x;
        float tMaxY = t + (nextY - p.y) / lightDir.y;
        float tMaxZ = t + (nextZ - p.z) / lightDir.z;

        const float tDeltaX = invDx;
        const float tDeltaY = invDy;
        const float tDeltaZ = invDz;

        float trans = 1.0f;
        int steps = 0;
        const int maxSteps = (m.w + m.h + m.d + pad * 3) * 4;

        while (t < t1 && trans > 0.05f && steps++ < maxSteps) {
            const float a = occ(x, y, z);
            if (a > 0.0f) {
                // Translucent voxels absorb less light (nice for glass/potions).
                const float absorb = 0.80f + 0.20f * (1.0f - a);
                trans *= absorb;
            }

            if (tMaxX < tMaxY) {
                if (tMaxX < tMaxZ) {
                    x += stepX;
                    t = tMaxX;
                    tMaxX += tDeltaX;
                } else {
                    z += stepZ;
                    t = tMaxZ;
                    tMaxZ += tDeltaZ;
                }
            } else {
                if (tMaxY < tMaxZ) {
                    y += stepY;
                    t = tMaxY;
                    tMaxY += tDeltaY;
                } else {
                    z += stepZ;
                    t = tMaxZ;
                    tMaxZ += tDeltaZ;
                }
            }
        }

        return std::clamp(trans, 0.0f, 1.0f);
    };

    for (int py = 0; py < img.h; ++py) {
        for (int px = 0; px < img.w; ++px) {
            // Inverse transform to iso-space coordinates.
            const float isoX = (static_cast<float>(px) + 0.5f - offX) / scale;
            const float isoY = (static_cast<float>(py) + 0.5f - offY) / scale;

            // Build a per-pixel orthographic ray origin on the camera plane.
            const Vec3f camPos = isoXVec * isoX + isoYVec * isoY + viewVec * dist;

            float tEnter = 0.0f, tExit = 0.0f;
            Vec3f enterN{0, 0, 0};
            if (!aabbHit(camPos, rayDir, tEnter, tExit, enterN)) continue;

            float t = std::max(0.0f, tEnter) + 1e-4f;
            Vec3f p = camPos + rayDir * t;

            int x = static_cast<int>(std::floor(p.x));
            int y = static_cast<int>(std::floor(p.y));
            int z = static_cast<int>(std::floor(p.z));

            const int stepX = (rayDir.x > 0.0f) ? 1 : -1;
            const int stepY = (rayDir.y > 0.0f) ? 1 : -1;
            const int stepZ = (rayDir.z > 0.0f) ? 1 : -1;

            const float invDx = 1.0f / std::max(1e-6f, std::abs(rayDir.x));
            const float invDy = 1.0f / std::max(1e-6f, std::abs(rayDir.y));
            const float invDz = 1.0f / std::max(1e-6f, std::abs(rayDir.z));

            const float nextX = (stepX > 0) ? static_cast<float>(x + 1) : static_cast<float>(x);
            const float nextY = (stepY > 0) ? static_cast<float>(y + 1) : static_cast<float>(y);
            const float nextZ = (stepZ > 0) ? static_cast<float>(z + 1) : static_cast<float>(z);

            float tMaxX = t + (nextX - p.x) / rayDir.x;
            float tMaxY = t + (nextY - p.y) / rayDir.y;
            float tMaxZ = t + (nextZ - p.z) / rayDir.z;

            const float tDeltaX = invDx;
            const float tDeltaY = invDy;
            const float tDeltaZ = invDz;

            float accumA = 0.0f;
            Vec3f accumRGB{0.0f, 0.0f, 0.0f};

            Vec3f lastN = enterN;
            int steps = 0;
            const int maxSteps = (m.w + m.h + m.d + pad * 3) * 6;

            while (t < tExit && accumA < 0.995f && steps++ < maxSteps) {
                const Color vox = m.at(x, y, z);
                if (vox.a > 0) {
                    const float a = static_cast<float>(vox.a) / 255.0f;
                    Vec3f n = smoothNormal(x, y, z, lastN);

                    // Cheap AO: local occupancy around the voxel.
                    float occSum = 0.0f;
                    float wSum = 0.0f;
                    auto sample = [&](int dx, int dy, int dz, float w) {
                        occSum += occ(x + dx, y + dy, z + dz) * w;
                        wSum += w;
                    };

                    sample(-1, 0, 0, 1.0f);
                    sample( 1, 0, 0, 1.0f);
                    sample( 0,-1, 0, 1.0f);
                    sample( 0, 1, 0, 1.0f);
                    sample( 0, 0,-1, 1.0f);
                    sample( 0, 0, 1, 1.0f);

                    // Extra weight from "above" so overhangs read.
                    sample(0, 1, 0, 1.25f);
                    sample(0, 2, 0, 0.45f);

                    const float occl = (wSum > 0.0f) ? (occSum / wSum) : 0.0f;
                    const float ao = std::clamp(1.0f - occl * 0.55f, 0.45f, 1.0f);

                    const float ndl = std::max(0.0f, dot(n, lightDir));
                    const Vec3f h = normalize(lightDir + viewDir);
                    const float ndh = std::max(0.0f, dot(n, h));
                    const float spec = std::pow(ndh, shininess);
                    const float rim = std::pow(1.0f - std::max(0.0f, dot(n, viewDir)), 2.0f);

                    // Approximate hit position at the entry boundary for this cell.
                    const Vec3f hitPos = camPos + rayDir * (t - 1e-4f);
                    const float shadow = traceShadow(hitPos, n);

                    float shade = (ambient + diffuse * ndl * shadow) * ao;
                    shade += specular * spec * shadow;
                    shade += rimStrength * rim;
                    shade = std::clamp(shade, 0.0f, 1.35f);

                    Vec3f baseC{vox.r / 255.0f, vox.g / 255.0f, vox.b / 255.0f};
                    Vec3f lit = baseC * shade;

                    // Subtle gel lift for translucent voxels.
                    if (a < 0.98f) {
                        lit = lit * (0.92f + 0.20f * (1.0f - a)) + Vec3f{0.02f, 0.03f, 0.04f} * (1.0f - a);
                    }

                    const float oneMinusA = 1.0f - accumA;
                    accumRGB = accumRGB + lit * (a * oneMinusA);
                    accumA = accumA + a * oneMinusA;
                }

                // DDA step
                if (tMaxX < tMaxY) {
                    if (tMaxX < tMaxZ) {
                        x += stepX;
                        lastN = Vec3f{-static_cast<float>(stepX), 0.0f, 0.0f};
                        t = tMaxX;
                        tMaxX += tDeltaX;
                    } else {
                        z += stepZ;
                        lastN = Vec3f{0.0f, 0.0f, -static_cast<float>(stepZ)};
                        t = tMaxZ;
                        tMaxZ += tDeltaZ;
                    }
                } else {
                    if (tMaxY < tMaxZ) {
                        y += stepY;
                        lastN = Vec3f{0.0f, -static_cast<float>(stepY), 0.0f};
                        t = tMaxY;
                        tMaxY += tDeltaY;
                    } else {
                        z += stepZ;
                        lastN = Vec3f{0.0f, 0.0f, -static_cast<float>(stepZ)};
                        t = tMaxZ;
                        tMaxZ += tDeltaZ;
                    }
                }
            }

            Color out{0, 0, 0, 0};
            out.a = clamp8(static_cast<int>(std::round(accumA * 255.0f)));
            out.r = clamp8(static_cast<int>(std::round(accumRGB.x * 255.0f)));
            out.g = clamp8(static_cast<int>(std::round(accumRGB.y * 255.0f)));
            out.b = clamp8(static_cast<int>(std::round(accumRGB.z * 255.0f)));
            img.at(px, py) = out;
        }
    }

    return img;
}

SpritePixels renderModelToSpriteIsometric(const VoxelModel& model, int frame, int outPx, bool isoRaytrace) {
    outPx = clampOutPx(outPx);

    // Small sprites benefit a lot from 2x supersampling + downscale.
    const int hiW = (outPx <= 32) ? outPx * 2 : outPx;
    const int hiH = hiW;

    SpritePixels hi = isoRaytrace
        ? renderVoxelIsometricRaytrace(model, /*outW=*/hiW, /*outH=*/hiH, frame)
        : renderVoxelIsometric(model, /*outW=*/hiW, /*outH=*/hiH, frame);
    SpritePixels out = (hiW == outPx) ? hi : downscale2x(hi);

    if (outPx <= 32) addOutline(out);
    return out;
}

} // namespace

SpritePixels renderSprite3DExtruded(const SpritePixels& base2d, uint32_t seed, int frame, int outPx) {
    outPx = clampOutPx(outPx);
    if (base2d.w <= 0 || base2d.h <= 0) return base2d;

    const int detailScale = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);

    constexpr int maxDepth = 6;
    VoxelModel vox = voxelizeExtrude(base2d, seed, maxDepth);
    if (detailScale > 1) vox = scaleVoxelModelNearest(vox, detailScale);

    const int hiW = (outPx <= 32) ? outPx * 2 : outPx;
    const int hiH = hiW;

    SpritePixels hi = renderVoxel(vox, /*outW=*/hiW, /*outH=*/hiH, frame, /*yawScale=*/1.0f);
    SpritePixels out = (hiW == outPx) ? hi : downscale2x(hi);
    if (outPx <= 32) addOutline(out);
    return out;
}

SpritePixels renderSprite3DEntity(EntityKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx) {
    // Some entities look much better when generated as true 3D blobs (slimes/ghosts).
    // Everything else falls back to the faithful 2D->3D extrusion.
    const int detailScale = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);
    VoxelModel m = buildEntityModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSprite(m, frame, /*yawScale=*/0.65f, outPx);
    }
    return renderSprite3DExtruded(base2d, seed, frame, outPx);
}

SpritePixels renderSprite3DItem(ItemKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx) {
    const int detailScale = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);
    VoxelModel m = buildItemModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSprite(m, frame, /*yawScale=*/0.95f, outPx);
    }
    return renderSprite3DExtruded(base2d, seed, frame, outPx);
}

SpritePixels renderSprite3DProjectile(ProjectileKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx) {
    (void)seed;
    const int detailScale = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);
    VoxelModel m = buildProjectileModel(kind, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSprite(m, frame, /*yawScale=*/1.35f, outPx);
    }
    return renderSprite3DExtruded(base2d, seed, frame, outPx);
}

SpritePixels renderSprite3DExtrudedTurntable(const SpritePixels& base2d, uint32_t seed, int frame, float yawRad, int outPx) {
    outPx = clampOutPx(outPx);
    if (base2d.w <= 0 || base2d.h <= 0) return base2d;

    const int detailScale = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);

    constexpr int maxDepth = 6;
    VoxelModel vox = voxelizeExtrude(base2d, seed, maxDepth);
    if (detailScale > 1) vox = scaleVoxelModelNearest(vox, detailScale);

    return renderModelToSpriteTurntable(vox, frame, yawRad, outPx);
}

SpritePixels renderSprite3DEntityTurntable(EntityKind kind, const SpritePixels& base2d, uint32_t seed, int frame, float yawRad, int outPx) {
    // Identical to renderSprite3DEntity() but uses a stable yaw for UI preview rotation.
    const int detailScale = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);
    VoxelModel m = buildEntityModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSpriteTurntable(m, frame, yawRad, outPx);
    }

    // Fallback: 2D -> 3D extrusion.
    outPx = clampOutPx(outPx);
    const int d = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);
    constexpr int maxDepth = 6;
    VoxelModel vox = voxelizeExtrude(base2d, seed, maxDepth);
    if (d > 1) vox = scaleVoxelModelNearest(vox, d);
    return renderModelToSpriteTurntable(vox, frame, yawRad, outPx);
}

SpritePixels renderSprite3DItemTurntable(ItemKind kind, const SpritePixels& base2d, uint32_t seed, int frame, float yawRad, int outPx) {
    const int detailScale = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);
    VoxelModel m = buildItemModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSpriteTurntable(m, frame, yawRad, outPx);
    }

    // Fallback: 2D -> 3D extrusion.
    outPx = clampOutPx(outPx);
    const int d = voxelDetailScaleForOutPx(outPx, /*isoRaytrace=*/false);
    constexpr int maxDepth = 6;
    VoxelModel vox = voxelizeExtrude(base2d, seed, maxDepth);
    if (d > 1) vox = scaleVoxelModelNearest(vox, d);
    return renderModelToSpriteTurntable(vox, frame, yawRad, outPx);
}

SpritePixels renderSprite3DExtrudedIso(const SpritePixels& base2d, uint32_t seed, int frame, int outPx, bool isoRaytrace) {
    outPx = clampOutPx(outPx);
    if (base2d.w <= 0 || base2d.h <= 0) return base2d;

    const int detailScale = voxelDetailScaleForOutPx(outPx, isoRaytrace);

    constexpr int maxDepth = 6;
    VoxelModel vox = voxelizeExtrude(base2d, seed, maxDepth);
    if (detailScale > 1) vox = scaleVoxelModelNearest(vox, detailScale);

    return renderModelToSpriteIsometric(vox, frame, outPx, isoRaytrace);
}

SpritePixels renderSprite3DEntityIso(EntityKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx, bool isoRaytrace) {
    const int detailScale = voxelDetailScaleForOutPx(outPx, isoRaytrace);
    VoxelModel m = buildEntityModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSpriteIsometric(m, frame, outPx, isoRaytrace);
    }
    return renderSprite3DExtrudedIso(base2d, seed, frame, outPx, isoRaytrace);
}

SpritePixels renderSprite3DItemIso(ItemKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx, bool isoRaytrace) {
    const int detailScale = voxelDetailScaleForOutPx(outPx, isoRaytrace);
    VoxelModel m = buildItemModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSpriteIsometric(m, frame, outPx, isoRaytrace);
    }
    return renderSprite3DExtrudedIso(base2d, seed, frame, outPx, isoRaytrace);
}

SpritePixels renderSprite3DProjectileIso(ProjectileKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx, bool isoRaytrace) {
    (void)seed;
    const int detailScale = voxelDetailScaleForOutPx(outPx, isoRaytrace);
    VoxelModel m = buildProjectileModel(kind, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        if (detailScale > 1) m = scaleVoxelModelNearest(m, detailScale);
        return renderModelToSpriteIsometric(m, frame, outPx, isoRaytrace);
    }
    return renderSprite3DExtrudedIso(base2d, seed, frame, outPx, isoRaytrace);
}


// -----------------------------------------------------------------------------
// Isometric terrain voxel blocks
// -----------------------------------------------------------------------------

namespace {

Color sampleSpriteOr(const SpritePixels& s, int x, int y, Color fallback) {
    if (s.w <= 0 || s.h <= 0) return fallback;
    x = std::clamp(x, 0, s.w - 1);
    y = std::clamp(y, 0, s.h - 1);
    const Color c = s.at(x, y);
    return (c.a == 0) ? fallback : c;
}

void carveBoxAlpha0(VoxelModel& m, int x0, int y0, int z0, int x1, int y1, int z1) {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        if (z0 > z1) std::swap(z0, z1);
    for (int z = z0; z <= z1; ++z) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                m.set(x, y, z, {0, 0, 0, 0});
            }
        }
    }
}

VoxelModel buildIsoTerrainBlockModel(IsoTerrainBlockKind kind, uint32_t seed, int frame) {
    // Build a small base-resolution model; caller applies detail scaling.
    // Coords: X=right, Z=left (in iso projection), Y=up.
    constexpr int W = 16;
    constexpr int D = 16;
    constexpr int H = 20;

    VoxelModel m = makeModel(W, H, D);

    RNG rng(hashCombine(seed, 0x715E77A1u ^ static_cast<uint32_t>(frame * 131)));

    const int bodyH = 14;
    const int x0 = 1, x1 = 14;
    const int z0 = 1, z1 = 14;

    // Helper: fill a beveled stone-ish box using a source texture for per-voxel variation.
    auto fillBeveledBox = [&](const SpritePixels& tex2d, const Palette& pal, int height) {
        for (int y = 0; y < height; ++y) {
            // Bevel the top few layers inward for a chunkier silhouette.
            int inset = 0;
            if (y >= height - 3) inset = (y - (height - 3)) + 1; // 1..3

            const int xx0 = x0 + inset;
            const int xx1 = x1 - inset;
            const int zz0 = z0 + inset;
            const int zz1 = z1 - inset;

            const float t = (height > 1) ? (static_cast<float>(y) / static_cast<float>(height - 1)) : 0.0f;
            const float grad = 0.88f + 0.12f * t;

            for (int z = zz0; z <= zz1; ++z) {
                for (int x = xx0; x <= xx1; ++x) {
                    // Sample the 2D tile as a coarse material pattern.
                    Color c = sampleSpriteOr(tex2d, x, z, pal.primary);

                    // Mix toward the dominant palette to avoid overly noisy textures.
                    c = lerp(c, pal.primary, 0.35f);

                    // Slight vertical gradient.
                    c = mul(c, grad);

                    // Occasional speckle / mottling.
                    if ((hash32(hashCombine(seed, static_cast<uint32_t>((x * 73856093) ^ (y * 19349663) ^ (z * 83492791)))) & 15u) == 0u) {
                        c = lerp(c, pal.secondary, 0.20f);
                    }

                    m.set(x, y, z, c);
                }
            }
        }

        // Edge wear: brighten a few top-edge voxels.
        for (int k = 0; k < 18; ++k) {
            const int y = height - 1;
            const int x = (rng.nextU32() & 1u) ? x0 : x1;
            const int z = 2 + static_cast<int>(rng.nextU32() % 12u);
            Color c = m.at(x, y, z);
            if (c.a == 0) continue;
            m.set(x, y, z, lerp(c, pal.accent, 0.35f));
        }
    };

    auto stoneTex = generateWallTile(seed ^ 0xAA110u, frame, /*pxSize=*/16);
    Palette stonePal = extractPalette(stoneTex);

    if (kind == IsoTerrainBlockKind::Wall) {
        fillBeveledBox(stoneTex, stonePal, bodyH);
        return m;
    }

    // Door base: start from stone frame.
    if (kind == IsoTerrainBlockKind::DoorClosed || kind == IsoTerrainBlockKind::DoorLocked || kind == IsoTerrainBlockKind::DoorOpen) {
        fillBeveledBox(stoneTex, stonePal, bodyH);

        const bool faceX = (hash32(seed ^ 0xD00Du) & 1u) == 0u; // choose which visible iso face gets the door detail
        SpritePixels doorTex;
        if (kind == IsoTerrainBlockKind::DoorLocked) doorTex = generateLockedDoorTile(seed ^ 0x10CCEDu, frame, /*pxSize=*/16);
        else doorTex = generateDoorTile(seed ^ 0xC105EDu, /*open=*/false, frame, /*pxSize=*/16);
        Palette doorPal = extractPalette(doorTex);

        // Door rectangle on the chosen face.
        const int y0d = 1;
        const int y1d = bodyH - 3;
        const int a0 = 5;
        const int a1 = 10;

        if (kind == IsoTerrainBlockKind::DoorOpen) {
            // Carve an opening through the face (with a bit of depth), leaving a stone frame.
            if (faceX) {
                for (int y = y0d; y <= y1d; ++y) {
                    for (int z = a0 + 1; z <= a1 - 1; ++z) {
                        for (int x = x1 - 3; x <= x1; ++x) {
                            m.set(x, y, z, {0, 0, 0, 0});
                        }
                    }
                }
            } else {
                for (int y = y0d; y <= y1d; ++y) {
                    for (int x = a0 + 1; x <= a1 - 1; ++x) {
                        for (int z = z1 - 3; z <= z1; ++z) {
                            m.set(x, y, z, {0, 0, 0, 0});
                        }
                    }
                }
            }

            // Darken interior rim for readability.
            const Color rim = mul(stonePal.secondary, 0.75f);
            if (faceX) {
                const int x = x1 - 3;
                for (int y = y0d; y <= y1d; ++y) {
                    m.set(x, y, a0, rim);
                    m.set(x, y, a1, rim);
                }
            } else {
                const int z = z1 - 3;
                for (int y = y0d; y <= y1d; ++y) {
                    m.set(a0, y, z, rim);
                    m.set(a1, y, z, rim);
                }
            }

            return m;
        }

        // Closed/locked: stamp a wood door panel on the visible face.
        const int faceCoord = faceX ? x1 : z1;
        for (int y = y0d; y <= y1d; ++y) {
            for (int a = a0; a <= a1; ++a) {
                // Simple vertical plank pattern.
                Color c = doorPal.primary;
                if (((a - a0) % 2) == 0) c = lerp(c, doorPal.secondary, 0.35f);
                if (((y + frame) % 5) == 0) c = lerp(c, doorPal.accent, 0.10f);

                if (faceX) {
                    m.set(faceCoord, y, a, c);
                } else {
                    m.set(a, y, faceCoord, c);
                }
            }
        }

        // Locked: add a tiny brass lock/handle accent.
        if (kind == IsoTerrainBlockKind::DoorLocked) {
            const Color brass = {220, 200, 80, 255};
            const int ly = (y0d + y1d) / 2;
            const int la = a1 - 1;
            if (faceX) {
                m.set(faceCoord, ly, la, brass);
                m.set(faceCoord, ly + 1, la, brass);
            } else {
                m.set(la, ly, faceCoord, brass);
                m.set(la, ly + 1, faceCoord, brass);
            }
        }

        return m;
    }

    if (kind == IsoTerrainBlockKind::Pillar) {
        const SpritePixels pTex = generatePillarTile(seed ^ 0x9111A0u, frame, /*pxSize=*/16);
        const Palette pPal = extractPalette(pTex);

        // Base + shaft + cap.
        addCylinderY(m, 7.5f, 7.5f, 4.3f, 0, 1, mul(pPal.secondary, 0.95f), z0, z1);
        addCylinderY(m, 7.5f, 7.5f, 3.7f, 2, 12, pPal.primary, z0, z1);
        addCylinderY(m, 7.5f, 7.5f, 4.1f, 13, 13, lerp(pPal.primary, pPal.accent, 0.25f), z0, z1);
        addCylinderY(m, 7.5f, 7.5f, 4.4f, 14, 14, lerp(pPal.primary, pPal.accent, 0.45f), z0, z1);

        // Small chips.
        for (int i = 0; i < 10; ++i) {
            const int y = 2 + static_cast<int>(rng.nextU32() % 11u);
            const int a = 2 + static_cast<int>(rng.nextU32() % 12u);
            const int x = a;
            const int z = 2 + static_cast<int>(rng.nextU32() % 12u);
            if (m.at(x, y, z).a == 0) continue;
            if ((rng.nextU32() & 3u) == 0u) m.set(x, y, z, {0, 0, 0, 0});
        }

        return m;
    }

    if (kind == IsoTerrainBlockKind::Boulder) {
        const SpritePixels bTex = generateBoulderTile(seed ^ 0xB011D3u, frame, /*pxSize=*/16);
        const Palette bPal = extractPalette(bTex);

        // Start with a lumpy sphere.
        addSphere(m, 7.5f, 6.8f, 7.5f, 5.7f, bPal.primary);

        // Carve a few random chunks to break symmetry.
        for (int i = 0; i < 18; ++i) {
            const float cx = 4.0f + (rng.nextU32() % 800u) / 100.0f; // 4..12
            const float cy = 2.5f + (rng.nextU32() % 700u) / 100.0f; // 2.5..9.5
            const float cz = 4.0f + (rng.nextU32() % 800u) / 100.0f;
            const float rr = 1.0f + (rng.nextU32() % 200u) / 100.0f; // 1..3
            for (int z = 0; z < m.d; ++z) {
                for (int y = 0; y < m.h; ++y) {
                    for (int x = 0; x < m.w; ++x) {
                        const Color c = m.at(x, y, z);
                        if (c.a == 0) continue;
                        const float dx = (x + 0.5f) - cx;
                        const float dy = (y + 0.5f) - cy;
                        const float dz = (z + 0.5f) - cz;
                        if (dx*dx + dy*dy + dz*dz < rr*rr) {
                            m.set(x, y, z, {0, 0, 0, 0});
                        }
                    }
                }
            }
        }

        // Apply per-voxel color variation using the 2D sprite as a material swatch.
        for (int z = 0; z < m.d; ++z) {
            for (int y = 0; y < m.h; ++y) {
                for (int x = 0; x < m.w; ++x) {
                    Color c = m.at(x, y, z);
                    if (c.a == 0) continue;

                    Color s = sampleSpriteOr(bTex, x, z, bPal.primary);
                    c = lerp(c, s, 0.45f);

                    const uint32_t h = hash32(hashCombine(seed ^ 0xB01D3u, static_cast<uint32_t>((x * 33) ^ (y * 97) ^ (z * 131))));
                    const float n = ((h & 255u) / 255.0f) - 0.5f;
                    c = mul(c, 1.0f + 0.18f * n);

                    // Rare brighter fleck.
                    if ((h & 127u) == 0u) c = lerp(c, bPal.accent, 0.25f);

                    m.set(x, y, z, c);
                }
            }
        }

        return m;
    }

    // Fallback.
    fillBeveledBox(stoneTex, stonePal, bodyH);
    return m;
}

} // namespace

SpritePixels renderIsoTerrainBlockVoxel(IsoTerrainBlockKind kind, uint32_t seed, int frame, int outPx, bool isoRaytrace) {
    outPx = clampOutPx(outPx);
    frame = frame % FRAMES;

    const int detailScale = voxelDetailScaleForOutPx(outPx, isoRaytrace);

    VoxelModel vox = buildIsoTerrainBlockModel(kind, seed, frame);
    if (detailScale > 1) vox = scaleVoxelModelNearest(vox, detailScale);

    return renderModelToSpriteIsometric(vox, frame, outPx, isoRaytrace);
}
