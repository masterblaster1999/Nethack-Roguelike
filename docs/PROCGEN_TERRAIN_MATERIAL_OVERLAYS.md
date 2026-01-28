# Procedural Terrain Material Overlays

This project already uses procedural **room-style** terrain tiles (floor themes, decals, borders, etc.) and a separate procedural **terrain material** system (`TerrainMaterial`) that influences things like *tinting*, ambience, and bioluminescence.

This patch adds a new missing piece: **material-specific texture cues**.

## What it is

We now generate a small set of **transparent, mostly-grayscale overlay textures** per `TerrainMaterial` and layer them on top of the normal floor/wall tile art:

- **Wood** gets grain + plank seams
- **Metal** gets plate seams + rivets
- **Marble** gets vein lines
- **Brick** gets mortar/brick grid hints
- **Bone** gets pits/pores + cracks
- **Crystal** gets facet lines + subtle sparkles
- **Stone / Basalt / Obsidian / Dirt / Moss** get appropriate speckle, banding, cracks, or patchiness

Because these overlays are grayscale, the existing renderer tinting (lighting + palette + `terrainMaterialTint`) can re-tint them naturally without needing a full per-material tileset.

## Rendering order

For both top-down and isometric floors, the material overlay is drawn:

1. Base themed floor/wall tile
2. **Material overlay** (new)
3. Room-style decals
4. Room-style borders / wall edges / shading overlays
5. Entity / item sprites

This keeps the material pattern as a *substrate detail* while still allowing decals and edges to sit on top.

## Coherent variation + VRAM control

To avoid obvious repetition, the renderer selects overlay variants using the existing `pickCoherentVariantIndex(...)` function (coherent noise across tile coordinates).

To keep VRAM stable when users crank the tile size up, the renderer scales the number of overlay variants with `spritePx` (similar to other terrain variant pools).

## Files

- `src/spritegen.[hpp|cpp]`: procedural overlay generators for floor/walls + isometric floor overlays
- `src/render.[hpp|cpp]`: cache + generation + render-time layering

