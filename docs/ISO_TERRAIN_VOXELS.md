# Isometric terrain voxel blocks

This project has two separate "3D-ish" systems:

1. **Voxel sprites** for entities/items/projectiles (`voxel_sprites`).
2. **Isometric terrain blocks** for walls/doors/pillars/boulders in isometric view.

Historically, (2) was rendered using a 2.5D sprite pipeline: square textures procedurally drawn in 2D to look like taller blocks.

## What changed

There is now an optional **voxel-generated** path for isometric terrain "block" sprites:

- Wall blocks
- Door blocks (closed / locked / open doorway)
- Pillar blocks
- Boulder blocks

When enabled, these blocks are generated from **small voxel models** and then rendered in the same isometric projection as voxel sprites.

This improves:

- **Visual cohesion**: voxel entities and terrain share lighting/shading cues.
- **Depth readability**: contact shadows + consistent highlights help separate wall edges from floor diamonds.
- **Deterministic variety**: each run/floor style seed produces stable variations.

## Controls

You can toggle at runtime with an extended command:

- `#isoterrainvox on|off|toggle`
- Alias: `#isoblocks on|off|toggle`

(Like other visual settings, toggling marks settings as dirty so it gets persisted.)

## Settings

In `procrogue_settings.ini`:

```ini
# iso_terrain_voxels: true/false
iso_terrain_voxels = true
```

Notes:

- This setting is **isometric-view only**.
- The blocks are cached textures, so the main cost is **generation time** when entering isometric view, changing tile size, or changing the style seed.

### Raytrace interaction

If you enable `iso_voxel_raytrace = true`, voxel sprites can use the higher-quality orthographic voxel raytracer.

Terrain blocks are a different trade-off: they are generated in batches (many variants), so raytracing them at very large tile sizes can stall.

For that reason the terrain-block voxel renderer:

- Uses raytrace only when tile size is small (<= 64px).
- Uses the fast isometric mesh renderer for larger tile sizes.

