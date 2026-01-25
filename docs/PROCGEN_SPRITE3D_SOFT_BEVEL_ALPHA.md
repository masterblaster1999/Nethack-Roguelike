# 3D Spritegen: Soft Bevel Alpha (Distance-Field Coverage)

The 2D→3D fallback in ProcRogue converts a classic pixel-art sprite into a small voxel volume and then renders that volume under lighting.

Earlier rounds improved the *shape* by:

- Building a **silhouette mask** from the sprite.
- Computing a **distance-to-edge field** (now a quasi‑Euclidean 8-neighborhood chamfer distance).
- Using that distance field to allocate **shape-adaptive thickness** and a **biconvex bevel profile**.

This doc covers a follow-up improvement: **soft bevel coverage**.

## Problem

When a bevel is implemented as a hard threshold ("if distance < required → no voxel"), near-edge slices can show:

- **Hard popping** between adjacent depth slices (especially when yaw animates).
- **Jagged silhouette steps** at higher output resolutions (even after voxel upscaling).
- Overly binary transitions where a one-voxel change removes an entire column.

In pixel art, hard steps can read fine at 16×16, but they start to look harsh once the same model is rendered at 64×64 or 128×128.

## Core Idea

Instead of treating the distance field as a binary inside/outside test at the bevel boundary, treat it as a **coverage estimate**.

If a voxel is just barely outside the required bevel radius (within roughly one sprite pixel), we keep it as a **semi-transparent voxel**.

That makes the bevel boundary behave like a tiny, distance-field-driven anti-alias band.

## Implementation Overview

Inside `voxelizeExtrude()` (`src/spritegen3d.cpp`):

1. For each sprite pixel, compute its distance-to-edge `dEdge` (in chamfer cost units).
2. For each depth slice `zLocal`:
   - Compute a smooth biconvex erosion radius `requiredDist`.
   - Convert `(requiredDist - dEdge)` into a **coverage** value in `[0..1]`.
   - Multiply voxel alpha by that coverage.

### Smooth biconvex profile

We still use a biconvex profile, but the "ramp" from middle → cap is now smoother than the original linear mapping.

We use a *sagitta* style curve:

- Let `u = |zLocal - zMid| / zMid` in `[0..1]`
- `profile(u) = 1 - sqrt(1 - u^2)`

This keeps the interior slices closer to the full silhouette while still rounding the ends.

### Coverage (soft bevel)

We define a small band near the threshold (≈ one pixel in distance-field units) where voxels fade out instead of disappearing instantly:

- `margin = requiredDist - dEdge`
- If `margin <= 0` → voxel is fully inside (coverage = 1)
- If `margin` is within ~1 pixel of distance (`ORTH`) → coverage fades from 1→0

We also:

- Keep the **very front slice** fully opaque for crisp silhouette + texture readability.
- Drop extremely low alpha voxels to avoid noisy translucent dust.

## Why it helps

- **Smoother silhouettes** at large tile sizes.
- Less "one voxel" popping as the sprite yaws.
- Bevels read more like rounded material instead of stacked slices.

## Tuning notes

Parameters are intentionally conservative:

- The fade band uses `ORTH` (one pixel step) so the effect stays subtle.
- Coverage is gamma-shaped (`pow`) to keep most of the band close to opaque.
- Very low alpha voxels are culled to reduce shimmer.

If you want a stronger soft bevel:

- Increase the band width (e.g., treat `2*ORTH` as the fade region).
- Lower the alpha-cull threshold.
- Reduce the `pow()` exponent to make the fade more linear.
