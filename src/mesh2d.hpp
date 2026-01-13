#pragma once

#include "common.hpp"
#include "spritegen.hpp"

#include <vector>

// A tiny CPU-side 2D triangle mesh + rasterizer intended for procedural sprite generation.
//
// This is *not* a general-purpose renderer. It intentionally supports a minimal feature set:
//  - Flat (per-triangle) color
//  - Per-vertex depth (z) for correct overlap with a simple z-buffer
//  - Opaque + translucent split (opaque: z-write; translucent: depth-sorted + z-test)

struct Mesh2DTriangle {
    Vec2f p0;
    Vec2f p1;
    Vec2f p2;

    // Depth at each vertex. Units are arbitrary as long as larger values mean "closer".
    float z0 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;

    Color c{0, 0, 0, 0};
};

struct Mesh2D {
    std::vector<Mesh2DTriangle> tris;
};

// Rasterize a mesh to a SpritePixels image.
//
// Output pixels are straight-alpha (to match the rest of the sprite pipeline).
SpritePixels rasterizeMesh2D(const Mesh2D& mesh, int outW, int outH);
