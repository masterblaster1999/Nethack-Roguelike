#pragma once

// Reusable procedural simulation helpers.
//
// Gray-Scott reaction-diffusion is a small, deterministic 2D simulation that can
// generate organic-looking spot/maze patterns. We use it for both gameplay fields
// (chemical hazards) and cosmetic worldgen fields (biolum/lichen).
//
// Header-only so gameplay + generator code can share the exact same numerics.

#include "common.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace proc {

struct GrayScottParams {
    float da = 1.0f;
    float db = 0.50f;
    float feed = 0.0367f;
    float kill = 0.0649f;
};

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Run a Gray-Scott reaction-diffusion simulation for `iters` steps.
//
// - `A` and `B` are resized to w*h if needed (initialized to A=1, B=0).
// - If `activeMask` is provided (w*h bytes), cells with 0 are treated as
//   inactive boundaries: they are forced to A=1, B=0 every iteration.
inline void runGrayScott(int w, int h,
                         const GrayScottParams& p,
                         int iters,
                         std::vector<float>& A,
                         std::vector<float>& B,
                         const std::vector<uint8_t>* activeMask = nullptr) {
    const int W = std::max(1, w);
    const int H = std::max(1, h);
    const size_t N = static_cast<size_t>(W * H);

    if (iters <= 0) return;

    if (A.size() != N) A.assign(N, 1.0f);
    if (B.size() != N) B.assign(N, 0.0f);

    std::vector<float> nA(N, 1.0f);
    std::vector<float> nB(N, 0.0f);

    auto at = [&](const std::vector<float>& v, int x, int y) -> float {
        x = (x < 0) ? 0 : ((x >= W) ? (W - 1) : x);
        y = (y < 0) ? 0 : ((y >= H) ? (H - 1) : y);
        return v[static_cast<size_t>(y * W + x)];
    };

    constexpr float dt = 1.0f;

    for (int it = 0; it < iters; ++it) {
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const size_t i = static_cast<size_t>(y * W + x);

                if (activeMask && i < activeMask->size() && (*activeMask)[i] == 0u) {
                    nA[i] = 1.0f;
                    nB[i] = 0.0f;
                    continue;
                }

                const float a = A[i];
                const float b = B[i];

                // 9-sample Laplacian (standard RD stencil).
                const float lapA =
                    -a
                    + 0.20f * (at(A, x - 1, y) + at(A, x + 1, y) + at(A, x, y - 1) + at(A, x, y + 1))
                    + 0.05f * (at(A, x - 1, y - 1) + at(A, x + 1, y - 1) + at(A, x - 1, y + 1) + at(A, x + 1, y + 1));

                const float lapB =
                    -b
                    + 0.20f * (at(B, x - 1, y) + at(B, x + 1, y) + at(B, x, y - 1) + at(B, x, y + 1))
                    + 0.05f * (at(B, x - 1, y - 1) + at(B, x + 1, y - 1) + at(B, x - 1, y + 1) + at(B, x + 1, y + 1));

                const float reaction = a * b * b;

                float na = a + (p.da * lapA - reaction + p.feed * (1.0f - a)) * dt;
                float nb = b + (p.db * lapB + reaction - (p.kill + p.feed) * b) * dt;

                nA[i] = clampf(na, 0.0f, 1.0f);
                nB[i] = clampf(nb, 0.0f, 1.0f);
            }
        }

        A.swap(nA);
        B.swap(nB);
    }

    // Enforce the mask one last time so callers can rely on stable boundaries.
    if (activeMask && activeMask->size() == N) {
        for (size_t i = 0; i < N; ++i) {
            if ((*activeMask)[i] == 0u) {
                A[i] = 1.0f;
                B[i] = 0.0f;
            }
        }
    }
}

} // namespace proc
