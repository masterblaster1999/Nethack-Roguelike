# 3D Turntable UI Previews

This project already supports **3D voxel sprites** (and an optional isometric voxel raytracer) for in-game rendering.

This document describes an additional, *UI-only* 3D feature: **turntable previews**.

## What it is

When you open:

- **Monster Codex**
- **Discoveries**

…the right-hand details panel now includes a **large 3D preview** that **auto-rotates**.

The preview is purely visual:

- It does **not** change gameplay.
- It does **not** change any monster/item stats.

## How it works

### Turntable rendering

The voxel renderer already supported a tiny frame-based “yaw wobble” for living motion.

For UI previews we instead render with an explicit camera yaw (`yawRad`) so the model can be shown from multiple angles.

New exported helpers:

- `renderSprite3DExtrudedTurntable(...)`
- `renderSprite3DEntityTurntable(...)`
- `renderSprite3DItemTurntable(...)`

The preview uses a small number of discrete yaw steps (currently 24), so it appears smoothly rotating while still caching well.

### Caching

Large UI previews are **more expensive** than normal tile sprites.

To avoid re-rendering every frame, previews are stored in a dedicated LRU cache:

- `Renderer::uiPreviewTex` (`LRUTextureCache<1>`)

The cache is budgeted separately from the main sprite cache and is cleared when the user toggles voxel sprites / isometric raytracing.

### Identification safety

In the **Discoveries** overlay:

- If an item is **identified**, the preview uses the item’s procedural 3D model.
- If an item is **not identified**, the preview renders an **extruded turntable** based on its appearance sprite.
  - This preserves the NetHack-style “appearance → true item” mapping without leaking the true identity via distinct models.

### Respecting the user’s settings

If **voxel sprites are disabled**, UI previews fall back to a scaled 2D sprite.

## Where to look in code

- `src/spritegen3d.hpp / src/spritegen3d.cpp` – turntable render entry points
- `src/render.hpp / src/render.cpp` – UI preview texture cache + Codex/Discoveries integration
