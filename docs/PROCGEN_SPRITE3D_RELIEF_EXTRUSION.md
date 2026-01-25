# 3D Spritegen: Relief Extrusion

This project supports a hybrid 2D/3D look:

- **2D mode**: classic procedural **16×16** pixel-art sprites are generated and upscaled.
- **3D sprite mode**: those same 16×16 sprites are converted into tiny **voxel mini-models** and re-rendered back to 2D with lighting.

The 3D pipeline lives in `src/spritegen3d.cpp`.

## Goals

The 2D→3D fallback is meant to:

- Keep 3D sprites **deterministic** and **cheap** to generate.
- Avoid **bloated silhouettes** caused by voxelizing 2D drop shadows/outlines.
- Avoid losing important **accent details** (eyes, runes, trim) when flattening 2D shading.
- Add subtle **surface relief** so the lighting doesn't read like a flat cardboard cutout.

## Source sprite: no baked outline/shadow

When generating **3D sprites**, the sprite generator now feeds the voxelizer an **un-finalized** base sprite:

- No drop shadow
- No exterior outline
- No automatic contour shading
- No rim light

The 3D renderer already adds its own contact shadow and (for small output sizes) a clean 1px outline **after** voxel rendering.

Relevant code:

- `src/spritegen.cpp` (skips `finalizeSprite()` when `use3d == true`)
- `src/spritegen3d.cpp`

## Masking: drop shadow stripping

For robustness (in case a finalized sprite is passed into the voxelizer by some future call site), the voxelizer contains a small heuristic to ignore `finalizeSprite()` drop shadows:

- The shadow pixels are **near-black** and **semi-transparent**.
- `finalizeSprite()`'s drop shadow is offset by **(+1, +1)**, so a shadow pixel at `(x,y)` tends to have a corresponding source pixel at `(x-1,y-1)`.

Those pixels are excluded from the extrusion mask so they do not inflate the 3D silhouette.

## Bas-relief thickness modulation

The extrusion thickness is not purely "distance-to-edge" anymore.

Each masked pixel's thickness is modulated by its **luminance** relative to the sprite's **mean luminance**:

- **Brighter pixels** become (slightly) thicker.
- **Darker pixels** become (slightly) thinner.

The modulation is intentionally subtle (usually **±1 voxel layer**) and is suppressed near silhouette edges to preserve the original 2D outline.

The result is a tiny "height field" (bas-relief) that gives the 3D lighting something to bite on, especially for sprites that are otherwise flat color blocks.

## Accent-preserving albedo flattening

To prevent "double lighting" (baked 2D shading + dynamic 3D shading), the voxelizer flattens colors toward the sprite's **dominant palette** color.

However, flattening is now reduced for pixels far from that dominant color. This preserves high-contrast details such as:

- Eyes and mouths
- Runes and glyphs
- Metal trim and highlights

Without this, those details tend to wash out under re-lighting.

## Determinism

Any micro-variation uses **per-pixel hash noise** (seed + x + y) rather than consuming a linear RNG stream.

This keeps generation deterministic and avoids coupling the appearance of one pixel to the presence/absence of other pixels.
