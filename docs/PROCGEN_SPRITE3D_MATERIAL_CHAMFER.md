# Procedural 3D Sprite Shading: Material Hints + Surface Chamfer

This project’s 3D sprites are generated from tiny procedural 2D sprites in two ways:

- **Native voxel primitives** for a few kinds (e.g., slimes, potions, rocks, sparks).
- **2D → 3D extrusion** for everything else.

Once we have a voxel model, we render it with a lightweight ray-marched voxel renderer (and an isometric mesh renderer / raytracer variant).

This doc describes two upgrades that make voxel sprites read better **at high output resolutions** (64×64 and up) without abandoning the “tiny voxel art” aesthetic:

1. **Material-aware highlight tuning** (specular, shininess, rim) derived from the voxel’s albedo + alpha.
2. **A surface chamfer / edge-feather pass** applied after voxel upscaling.


## 1) Material-aware highlight tuning (stylized Blinn-Phong)

Classic Blinn-Phong shading computes specular intensity from a “half-vector” between the view vector and the light vector:

- `H = normalize(L + V)`
- `spec = pow(max(0, dot(N, H)), shininess)`

In this codebase we keep the model stylized (not full PBR), but we avoid “one-size-fits-all” highlights by inferring **material hints** from:

- **Alpha**: low alpha tends to be **glass / gel** → sharper, stronger highlights.
- **Saturation**: highly saturated colors tend to be **paint / cloth / matte** → softer highlights.
- **Brightness + neutrality**: bright low-saturation colors read as **metal / stone** → moderate highlight boost.

The heuristic returns multipliers:

- `specMul` (specular strength)
- `shininessMul` (highlight tightness)
- `rimMul` (rim strength)

We also add a conservative **emissive lift** for extremely bright *and* saturated voxels (sparks, flame cores, magical runes). Emissive is additive and intentionally small so normal highlights don’t turn into neon glow.


## 2) Surface chamfer + alpha feathering (post-upscale)

When we upscale a tiny voxel model (e.g., 16×16×8 → 64×64×32), we get more geometric resolution, but silhouettes can still look a bit stair-steppy and edges can read flat.

We apply a post-process pass only when `detailScale > 1`:

- Detect **surface voxels** (any of 6 neighbors empty).
- Detect **edges/corners** by counting exposed neighbors.
- For near-opaque voxels, apply:
  - A *very subtle* edge/corner brighten (a “micro-chamfer highlight”).
  - A tiny **alpha feather** at convex edges/corners.

Why alpha feathering?

- The raytracer’s **occupancy-gradient normal smoothing** uses alpha to estimate a soft surface normal.
- Feathering edge voxels slightly helps the normal field become smoother, which improves lighting and reduces harsh “blocky” transitions.

Translucent materials (glass/ghost/slime) are excluded so their look stays authored.


## Where to find the code

- `src/spritegen3d.cpp`
  - `estimateMaterial(Color)`
  - `applyVoxelSurfaceChamfer(VoxelModel&, seed, strength)`
  - Integrated into both:
    - Perspective voxel renderer (`renderVoxel`)
    - Isometric renderers (`shadeIsoFace`, isometric raytrace)

