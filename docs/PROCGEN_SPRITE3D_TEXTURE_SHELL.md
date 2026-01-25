# PROCGEN: 3D Sprite Extrusion — Texture Shell + Material Fill

This note documents an upgrade to the **2D → 3D voxel extrusion fallback** used by the voxel sprite renderer (`src/spritegen3d.cpp`).

The renderer can choose between:

- **Native procedural voxel models** for some entities/items/projectiles (true 3D generation).
- A faithful **2D → 3D extrusion** path when a dedicated voxel model does not exist.

For the fallback, the pipeline is:

1. Start from the authored 2D sprite (typically 16×16).
2. Convert it into a tiny voxel volume (extrusion + bevel + subtle relief cues).
3. Render the voxel volume back into a 2D sprite using the voxel sprite renderer.

This round upgrades step **(2)** by decoupling **surface texture** from **volume fill**.

## Motivation

A naive extrusion approach assigns the *same per-pixel color* to **every voxel along that pixel’s depth column**.

That is faithful to the 2D sprite, but in 3D it creates a common artifact:

- Dark outlines, inked linework, and high-contrast 2D details turn into **thick “striped side walls”**.
- When the raytraced renderer sees a bit of the side/top surfaces, it shows those stripes very clearly.
- At large output sizes (64/128px) where the voxel model is upscaled for density, the stripe artifact becomes more noticeable.

In 2D art, linework is typically a **surface mark**. In 3D, we want it to mostly remain a surface mark — not become the object’s internal material.

## Key idea

Treat the 2D sprite as a **front-facing texture** and keep it on a thin **texture shell** near the front surface.

For deeper voxels (the “bulk”), fill with a **material color** derived from the sprite’s dominant palette color.

This keeps:

- The sprite’s authored facial features, runes, icon markings, and outline style on the surface.
- The 3D volume coherent (sides/top read like a single material rather than a stack of 2D scanlines).

## Implementation overview

Inside `voxelizeExtrude()` we already compute:

- `avg` — average opaque sprite color (dominant palette “base”).
- `accent01` — how far the pixel is from `avg` (higher means “detail/accent”).
- `interior01` — how far into the silhouette the pixel is (from the 2D distance-to-edge map).
- `thickness` — the per-pixel depth budget (shape-adaptive + bas-relief + micro noise).

We extend this with a per-pixel **ink detector** and two per-column colors:

### 1) Texture color (`tex`)

We keep the existing “accent-preserving albedo flattening” step:

- `tex = lerp(c0, avg, flatten)`

This keeps accents and removes some baked-in 2D lighting.

### 2) Ink likelihood (`ink01`)

We approximate whether a pixel is likely “linework/ink” by combining three cues:

- **Darkness** (low luminance)
- **Neutrality** (low saturation)
- **Edge proximity** (outline tends to sit near the silhouette)

This gives a continuous `ink01 ∈ [0,1]`.

### 3) Material fill color (`mat`)

We derive a fill color that is mostly the dominant palette color `avg`, but still carries some hue from `tex` when appropriate:

- `mat = lerp(avg, tex, carry01)`

Where:

- `carry01` increases with `accent01` (details carry more hue)
- `carry01` decreases with `ink01` (ink carries very little hue into the bulk)

Finally we slightly darken the fill (`×0.92`) so the texture shell stays readable.

### 4) Texture shell depth (`shellFront`)

We keep the fully textured region thin:

- Default: `shellFront = 1` (one layer).
- For strong non-ink accents, it can become `2..3` layers.

Ink-heavy pixels are biased toward `shellFront = 1` so outlines stay surface-bound.

### 5) Per-depth blending

For each filled voxel at depth `zLocal` in a pixel column:

- Compute `shellMix` from `zLocal` and `shellFront`.
- Blend the colors: `base = lerp(tex, mat, shellMix)`.
- Apply a depth tint that darkens deeper layers slightly more once they’re mostly material.
- Add tiny deterministic micro-variation in material layers to avoid perfectly flat side walls.

## Result

This upgrade preserves the silhouette and the authored 2D markings, but produces a stronger 3D read:

- **Less stripe noise** on sides/top surfaces.
- **Cleaner outlines**: outlines remain mostly on the front surface.
- Better coherence when voxel detail scaling is used for larger sprites.

The feature stacks with the earlier extrusion improvements:

- Shape-adaptive depth budget
- Biconvex bevel profile
- Surface chamfer/alpha feathering at higher voxel detail

Together, the extruded fallback reads less like a cardboard cutout and more like a small painted token.
