# PROCGEN: 3D Sprite Extrusion — Biconvex Bevel + Shape-Adaptive Depth

This note documents an upgrade to the **2D → 3D voxel extrusion fallback** used by the voxel sprite renderer (`src/spritegen3d.cpp`).

The renderer supports “native” procedural voxel models for some entities/items/projectiles, but many sprites still rely on the generic pipeline:

1. Start from the authored 2D sprite (typically 16×16).
2. Convert the sprite into a tiny voxel volume (extrusion + bevel + small relief cues).
3. Raytrace (or mesh-rasterize) the voxel volume back into a 2D sprite with lighting/shadow.

This round improves step **(2)** by producing a more “rounded token” depth profile while keeping the silhouette faithful to the original pixel art.

## Motivation

The earlier extrusion path behaved like a **one-sided bevel**: the front face was almost the full 2D silhouette, and the back face shrank with depth. It read “3D”, but often still felt like a flat sticker being lit.

The upgrade introduces two ideas:

- **Shape-adaptive depth budgeting**: how much depth the sprite can carry should depend on how “chunky” its silhouette is.
- **Biconvex beveling**: instead of shrinking only toward the back, shrink toward the **middle slice**, so both the front and the back taper inward.

The net effect is a smoother, more sculpted read—especially for round-ish items and characters—without requiring higher model resolution.

## Distance-to-edge map (2D distance transform)

We build a binary mask from the sprite’s alpha (after stripping out obvious baked drop-shadows/outlines from the 2D art), then compute a **distance-to-edge** value `dEdge(x,y)` for each filled pixel.

Intuitively:

- `dEdge = 0` means the pixel is on the silhouette boundary.
- Larger `dEdge` values are deeper inside the shape.

Implementation-wise this is a small multi-source BFS on the 16×16 grid (L1 / Manhattan distance), which is cheap and deterministic.

## Shape-adaptive depth budget

Let:

- `maxDistInMask = max(dEdge)` over the sprite mask.

We treat `maxDistInMask` as a rough “radius” of the silhouette and derive a per-sprite depth budget:

- `depthBudget = clamp(2 + 2*maxDistInMask, 1, maxDepth)`

So:

- Thin/line-like shapes (`maxDistInMask = 0`) stay shallow (≈2 voxels).
- Chunkier silhouettes get more depth, up to the caller’s `maxDepth` (typically 8).

This makes the overall 3D style more consistent: tiny icons don’t become chunky bricks, and big silhouettes aren’t stuck looking wafer-thin.

## Per-pixel thickness

Each pixel column gets a thickness `T(x,y)`:

- A base thickness mapped from `dEdge` → `[2..depthBudget]` using a slightly curved ramp:

  `baseThickness = 2 + round(pow(dEdge/maxDistInMask, 0.75) * (depthBudget - 2))`

- Then we add the existing **bas-relief** cue (small ±1 depth change driven by luminance relative to the sprite’s mean), scaled so it’s stronger away from silhouette edges.
- A tiny deterministic per-pixel micro-variation (suppressed near edges) breaks up perfectly flat slabs.

Finally, thickness is clamped to `[1..depthBudget]`.

## Biconvex bevel in depth

For a given `(x,y)` column with thickness `T` we fill depth slices `z = 0..T-1`, but we don’t necessarily fill every `z` at the silhouette boundary.

Instead we compute a depth-dependent required interior distance:

- `mid = (T-1)/2`
- `requiredDist(z) = round(abs(z - mid) * bevelSlope)` with `bevelSlope ≈ 0.50`

And we fill the voxel only if:

- `dEdge(x,y) >= requiredDist(z)`

This produces a **biconvex / lens-like** profile:

- The cross-section is widest at the middle slice (requiredDist≈0).
- It shrinks toward both ends (front and back), rounding the model.

Importantly, the *2D silhouette is still preserved*: boundary pixels still exist at some depth slice, so rays will hit them and the sprite outline remains stable. The improvement is primarily in where the “front surface” sits along depth, which changes lighting and perceived volume.

## Interaction with later passes

This biconvex profile works alongside the existing sprite-voxel pipeline:

- High-resolution detail scaling (voxel upscaling for 64px/128px outputs).
- Ambient occlusion + material-aware highlights in the raytracer.
- Post-upscale surface chamfer/alpha feathering (high-res silhouette smoothing).

Together, these features aim to keep the game’s 2D readability while making the 3D sprite mode feel more cohesive and less “cardboard”.
