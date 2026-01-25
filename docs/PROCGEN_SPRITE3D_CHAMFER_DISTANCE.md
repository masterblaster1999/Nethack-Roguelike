# PROCGEN: 3D Sprite Extrusion – Chamfer Distance + Mask Preconditioning

The 2D→3D fallback voxel pipeline starts from a *binary silhouette mask* (sprite pixels that should become solid volume), then uses a **distance-to-edge** field to drive:

- A *shape-adaptive depth budget* (thin sprites stay thin; chunky silhouettes can support more depth).
- The **biconvex bevel** profile (cross-sections shrink away from the mid-slice).
- Bas‑relief + micro-variation suppression near edges.

This document describes two small but high-impact upgrades to that “mask → distance field → bevel” stage:

1) **Tiny hole fill** (mask preconditioning)
2) **Quasi‑Euclidean chamfer distance** (8-neighborhood)

Both changes are cheap at sprite resolutions (16×16 / 32×32), but they significantly improve the 3D read.

---

## 1) Tiny hole fill

Pixel-art sprites sometimes contain *unintentional* interior transparent pixels (“pinholes”) due to:

- Dithering patterns
- Anti-aliased imports
- Hand-edited cleanup

When extruded, a 1–2 pixel pinhole becomes an unwanted **through-hole** in the 3D token, which can:

- Break silhouettes
- Create spurious dark cavities / self-shadow
- Make bevels look chipped

### Approach

We flood-fill background starting from the image border, then treat any remaining transparent connected components as **enclosed holes**.

We only fill holes up to a conservative size threshold:

- `maxHole = clamp((w*h)/96, 2, 24)`
  - 16×16 → 2 pixels
  - 32×32 → 10 pixels
  - capped at 24

This keeps intentional negative space (rings, keyholes, large cutouts) intact, while removing the tiny artifacts.

Filled hole pixels don’t have authored colors (they were transparent), so the extrusion step treats them like interior material and uses the sprite’s dominant palette color.

---

## 2) Quasi‑Euclidean chamfer distance

The bevel/thickness logic needs a per-pixel **distance from the nearest silhouette edge**.

A simple 4-neighborhood BFS produces a Manhattan distance field (diamond-shaped iso-contours). This is fast, but it can introduce:

- Slightly *boxy / diamond* bevel contours at high voxel detail scales
- Coarser bevel thresholds due to integer rounding

### Approach

We compute a small **8-neighborhood chamfer distance** using integer weights:

- ORTH step: `10`
- DIAG step: `14` (≈ 10·√2)

This is a quasi‑Euclidean metric that closely approximates Euclidean distance but stays simple and deterministic.

Implementation notes:

- Edge pixels are detected using 4-neighborhood “outside mask?” checks.
- Distances are propagated with a tiny Dijkstra (priority queue) across the mask.
  - At 16×16, this is trivial in cost.

### Why this helps

- Bevel contours become less “diamond-like” and read smoother.
- Because the field is in **chamfer units**, bevel thresholds can shrink the silhouette in *sub‑pixel* increments (e.g., 0.5px becomes cost 5), reducing quantization artifacts.

---

## How the field is used

The voxelizer treats the maximum interior distance as an approximate 2D “radius”:

- `maxDistCost = max(distCost[x,y])`
- `maxDistF = maxDistCost / ORTH`

This radius influences:

- Depth budget selection
- Thickness curve inside the sprite
- Biconvex bevel cutoffs per depth slice

The result is an extrusion that is still faithful to the source sprite, but produces *cleaner*, more stable 3D volume.

---

## Tuning knobs

These constants can be tuned if desired:

- `maxHole` scaling (`area/96`) – controls how aggressive hole filling is.
- `ORTH=10`, `DIAG=14` – chamfer weights.
- `bevelSlope` – how quickly the silhouette shrinks per depth step.

