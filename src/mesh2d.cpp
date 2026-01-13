#include "mesh2d.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return static_cast<uint8_t>(v);
}

// Straight-alpha source-over blend.
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

inline float edgeFn(const Vec2f& a, const Vec2f& b, float x, float y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

inline float avgDepth(const Mesh2DTriangle& t) {
    return (t.z0 + t.z1 + t.z2) / 3.0f;
}

void rasterTriOpaque(SpritePixels& img, std::vector<float>& zBuf, const Mesh2DTriangle& t) {
    const Vec2f a = t.p0;
    const Vec2f b = t.p1;
    const Vec2f c = t.p2;

    const float minXf = std::min({a.x, b.x, c.x});
    const float maxXf = std::max({a.x, b.x, c.x});
    const float minYf = std::min({a.y, b.y, c.y});
    const float maxYf = std::max({a.y, b.y, c.y});

    int minX = static_cast<int>(std::floor(minXf));
    int maxX = static_cast<int>(std::ceil(maxXf));
    int minY = static_cast<int>(std::floor(minYf));
    int maxY = static_cast<int>(std::ceil(maxYf));

    minX = std::clamp(minX, 0, img.w - 1);
    maxX = std::clamp(maxX, 0, img.w - 1);
    minY = std::clamp(minY, 0, img.h - 1);
    maxY = std::clamp(maxY, 0, img.h - 1);

    const float area = edgeFn(a, b, c.x, c.y);
    if (std::abs(area) < 1e-6f) return;
    const float invArea = 1.0f / area;

    // Support either winding.
    const bool ccw = (area > 0.0f);
    constexpr float eps = 1e-4f;

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;

            const float w0 = edgeFn(b, c, px, py);
            const float w1 = edgeFn(c, a, px, py);
            const float w2 = edgeFn(a, b, px, py);

            const bool inside = ccw
                ? (w0 >= -eps && w1 >= -eps && w2 >= -eps)
                : (w0 <= eps && w1 <= eps && w2 <= eps);
            if (!inside) continue;

            // Barycentric weights.
            const float l0 = w0 * invArea;
            const float l1 = w1 * invArea;
            const float l2 = w2 * invArea;

            const float z = l0 * t.z0 + l1 * t.z1 + l2 * t.z2;
            const size_t idx = static_cast<size_t>(y * img.w + x);
            if (z <= zBuf[idx]) continue;

            zBuf[idx] = z;
            img.at(x, y) = t.c;
        }
    }
}

void rasterTriTranslucent(SpritePixels& img, const std::vector<float>& zOpaque, const Mesh2DTriangle& t) {
    const Vec2f a = t.p0;
    const Vec2f b = t.p1;
    const Vec2f c = t.p2;

    const float minXf = std::min({a.x, b.x, c.x});
    const float maxXf = std::max({a.x, b.x, c.x});
    const float minYf = std::min({a.y, b.y, c.y});
    const float maxYf = std::max({a.y, b.y, c.y});

    int minX = static_cast<int>(std::floor(minXf));
    int maxX = static_cast<int>(std::ceil(maxXf));
    int minY = static_cast<int>(std::floor(minYf));
    int maxY = static_cast<int>(std::ceil(maxYf));

    minX = std::clamp(minX, 0, img.w - 1);
    maxX = std::clamp(maxX, 0, img.w - 1);
    minY = std::clamp(minY, 0, img.h - 1);
    maxY = std::clamp(maxY, 0, img.h - 1);

    const float area = edgeFn(a, b, c.x, c.y);
    if (std::abs(area) < 1e-6f) return;
    const float invArea = 1.0f / area;
    const bool ccw = (area > 0.0f);
    constexpr float eps = 1e-4f;

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;

            const float w0 = edgeFn(b, c, px, py);
            const float w1 = edgeFn(c, a, px, py);
            const float w2 = edgeFn(a, b, px, py);

            const bool inside = ccw
                ? (w0 >= -eps && w1 >= -eps && w2 >= -eps)
                : (w0 <= eps && w1 <= eps && w2 <= eps);
            if (!inside) continue;

            const float l0 = w0 * invArea;
            const float l1 = w1 * invArea;
            const float l2 = w2 * invArea;

            const float z = l0 * t.z0 + l1 * t.z1 + l2 * t.z2;
            const size_t idx = static_cast<size_t>(y * img.w + x);

            // Depth test against the opaque z-buffer only.
            if (z <= zOpaque[idx]) continue;
            blendOver(img.at(x, y), t.c);
        }
    }
}

} // namespace

SpritePixels rasterizeMesh2D(const Mesh2D& mesh, int outW, int outH) {
    SpritePixels img;
    img.w = std::max(1, outW);
    img.h = std::max(1, outH);
    img.px.assign(static_cast<size_t>(img.w * img.h), {0, 0, 0, 0});

    const float negInf = -std::numeric_limits<float>::infinity();
    std::vector<float> zBuf(static_cast<size_t>(img.w * img.h), negInf);

    std::vector<const Mesh2DTriangle*> opaque;
    std::vector<const Mesh2DTriangle*> translucent;
    opaque.reserve(mesh.tris.size());
    translucent.reserve(mesh.tris.size());

    for (const auto& t : mesh.tris) {
        // Alpha threshold: treat near-opaque triangles as opaque for stable silhouettes.
        if (t.c.a >= 250) opaque.push_back(&t);
        else translucent.push_back(&t);
    }

    // Opaque pass: z-write.
    for (const Mesh2DTriangle* t : opaque) {
        rasterTriOpaque(img, zBuf, *t);
    }

    // Translucent pass: sort far->near and blend, z-test against opaque only.
    std::stable_sort(translucent.begin(), translucent.end(), [](const Mesh2DTriangle* a, const Mesh2DTriangle* b) {
        return avgDepth(*a) < avgDepth(*b);
    });
    for (const Mesh2DTriangle* t : translucent) {
        rasterTriTranslucent(img, zBuf, *t);
    }

    return img;
}
