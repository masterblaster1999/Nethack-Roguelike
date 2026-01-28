# Raycast 3D View (Experimental)

This project includes an **experimental raycast 3D view mode** (classic “pseudo‑3D”
first-person rendering) that turns the dungeon into a textured 3D scene.

The renderer:
- Casts a ray per screen column using **grid DDA**.
- Draws **textured wall slices** and **textured floors** (floor casting).
- Optionally draws a **textured ceiling** (ceiling casting; can be disabled to fall back to a simple gradient).
- Uses the existing procedural tile generators to create **deterministic wall/floor/ceiling textures**,
  including **terrain material overlays**.
- Adds optional **relief shading** (cheap bump/normal mapping) derived from the procedural textures’
  own luminance gradients to make surfaces “read” as more 3D.
- Adds optional **parallax texture mapping** (tiny UV shifts from the same luminance “height”)
  and **material specular highlights** for extra depth/shininess in 3D.
- Adds subtle **contact shadows** near wall↔floor/ceiling seams to better anchor geometry.
- Modulates walls/floors/ceilings with the game’s **per-tile light map** (including **colored lighting**, darkness, and bioluminescence) so the 3D view matches the 2D renderer’s lighting rules.
- Renders **billboard sprites** for visible entities and ground items, with per-column depth testing
  against the wall z-buffer so sprites correctly disappear behind geometry.

## How to use

- Press **F7** (Toggle View Mode) to cycle:

  **TOPDOWN → ISOMETRIC → 3D → TOPDOWN**

- In `procrogue_settings.ini`, you can also set:

  `view_mode = 3d`

- While in **3D view**, you can rotate the camera (free-look):

  **Alt+Q** = turn left, **Alt+E** = turn right (defaults; configurable via keybinds).

### Raycast 3D tuning (procrogue_settings.ini)

These settings are **visual-only** and can be reloaded at runtime (Config Reload):

- `raycast3d_scale = 1..4`  
  Internal render resolution divisor (higher = faster, lower = sharper).

- `raycast3d_fov = 40..100`  
  Horizontal field-of-view in degrees. ~67 is “classic” Wolf3D-like.

- `raycast3d_ceiling = true/false`  
  Textured ceiling vs simple gradient.

- `raycast3d_bump = true/false`  
  Enables normal-mapped relief shading from procedural textures.

- `raycast3d_parallax = true/false`  
  Enables parallax texture mapping (small UV shifts for extra depth).

- `raycast3d_parallax_strength = 0..100`  
  Controls parallax depth (higher = deeper, can get "wavier").

- `raycast3d_specular = true/false`  
  Enables material specular highlights (metal/crystal shine).

- `raycast3d_specular_strength = 0..100`  
  Controls specular intensity.

- `raycast3d_follow_move = true/false`  
  When `true` (default), the camera direction snaps to your most recent movement tween.
  When `false`, the camera direction persists and is controlled only by manual turning.

- `raycast3d_turn_deg = 1..90`  
  Degrees rotated per view-turn keypress (`view_turn_left` / `view_turn_right`).

- `raycast3d_sprites = true/false`  
  Enables billboard rendering for visible entities (monsters/NPCs).

- `raycast3d_items = true/false`  
  Enables billboard rendering for visible ground items (pickups).

## What is procedurally generated

The 3D view uses the same proc-art pipeline as the 2D views:

- Floor textures are generated via `generateThemedFloorTile(...)` and then composited with
  `generateFloorMaterialOverlay(...)`.
- Ceiling textures are generated from the same themed base + material overlays, then tinted/darkened
  and decorated with additional deterministic “shadow/stalactite” noise so ceilings don’t look like
  upside-down floors.
- Wall textures are generated via `generateWallTile(...)` and then composited with
  `generateWallMaterialOverlay(...)`.
- Doors and chasms have their own proc textures.
- Relief shading is computed on-the-fly from each texture’s luminance gradient (a cheap pseudo-normal).

All textures are regenerated deterministically per run/floor (seeded by `styleSeed` and `lvlSeed`),
so the look stays consistent with the rest of the game.

## Implementation notes

- View mode enum: `ViewMode::Raycast3D` (`src/game.hpp`).
- Toggle behavior: `Action::ToggleViewMode` cycles all three modes (`src/game_loop.cpp`).
- Renderer path:
  - `Renderer::drawRaycast3DView(...)`
  - `Renderer::ensureRaycast3DAssets(...)`
  - `Renderer::ensureRaycast3DRenderTarget(...)`

These live in `src/render.cpp`.

### Fog-of-war safety

To avoid revealing unexplored map structure:
- Rays treat **unexplored tiles as solid** and render them as black.

### Performance

The 3D view renders into a **streaming texture at a reduced internal resolution**
(`raycast3d_scale`) and then scales it to the map region.

If you want the fastest possible 3D view:
- increase `raycast3d_scale`
- disable `raycast3d_bump`
- disable `raycast3d_parallax`
- disable `raycast3d_specular`
- disable `raycast3d_ceiling`
- disable `raycast3d_sprites`
- disable `raycast3d_items`
