#include "spritegen3d.hpp"

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
        kind == ItemKind::RingProtection) {
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

SpritePixels renderVoxel(const VoxelModel& m, int outW, int outH, int frame, float yawScale = 1.0f) {
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
    Vec3f dirBase = normalize({0.70f, -0.42f, 1.0f});
    float yaw = ((frame % 2 == 0) ? -0.10f : +0.10f) * yawScale;
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

    auto shadeVoxel = [&](Color c, const Vec3f& n, const Vec3f& viewDir, float shadow, int vx, int vy, int vz)->Color {
        Vec3f nn = normalize(n);
        Vec3f vv = normalize(viewDir);

        float ndl = std::max(0.0f, dot(nn, lightDir));
        float shade = ambient + diffuse * ndl * shadow;

        // Tiny ambient occlusion based on filled neighbors.
        int neighbors = 0;
        neighbors += (occ(vx-1,vy,vz) > 0.01f) ? 1 : 0;
        neighbors += (occ(vx+1,vy,vz) > 0.01f) ? 1 : 0;
        neighbors += (occ(vx,vy-1,vz) > 0.01f) ? 1 : 0;
        neighbors += (occ(vx,vy+1,vz) > 0.01f) ? 1 : 0;
        neighbors += (occ(vx,vy,vz-1) > 0.01f) ? 1 : 0;
        neighbors += (occ(vx,vy,vz+1) > 0.01f) ? 1 : 0;

        float ao = 1.0f - 0.055f * static_cast<float>(neighbors);
        ao = clampf(ao, 0.60f, 1.0f);

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

    // A tiny "contact shadow" in screen-space for extra depth.
    SpritePixels withShadow = img;
    for (int y = outH - 2; y >= 0; --y) {
        for (int x = outW - 2; x >= 0; --x) {
            const Color c = img.at(x,y);
            if (c.a == 0) continue;
            const int sx = x + 1;
            const int shY = y + 1;
            if (sx < 0 || shY < 0 || sx >= outW || shY >= outH) continue;
            Color& d = withShadow.at(sx,shY);
            if (d.a == 0) {
                d = {0,0,0,80};
            }
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

} // namespace

SpritePixels renderSprite3DExtruded(const SpritePixels& base2d, uint32_t seed, int frame, int outPx) {
    outPx = clampOutPx(outPx);
    if (base2d.w <= 0 || base2d.h <= 0) return base2d;

    constexpr int maxDepth = 6;
    const VoxelModel vox = voxelizeExtrude(base2d, seed, maxDepth);

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
    const VoxelModel m = buildEntityModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        return renderModelToSprite(m, frame, /*yawScale=*/0.65f, outPx);
    }
    return renderSprite3DExtruded(base2d, seed, frame, outPx);
}

SpritePixels renderSprite3DItem(ItemKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx) {
    const VoxelModel m = buildItemModel(kind, seed, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        return renderModelToSprite(m, frame, /*yawScale=*/0.95f, outPx);
    }
    return renderSprite3DExtruded(base2d, seed, frame, outPx);
}

SpritePixels renderSprite3DProjectile(ProjectileKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx) {
    (void)seed;
    const VoxelModel m = buildProjectileModel(kind, frame, base2d);
    if (m.w > 0 && m.h > 0 && m.d > 0) {
        return renderModelToSprite(m, frame, /*yawScale=*/1.35f, outPx);
    }
    return renderSprite3DExtruded(base2d, seed, frame, outPx);
}
