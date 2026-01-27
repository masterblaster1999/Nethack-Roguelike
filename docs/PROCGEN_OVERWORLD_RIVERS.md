# Procedural Overworld Rivers

This project treats the overworld as an *infinite* stream of deterministic “wilderness chunks”.
Each chunk is generated from a stable chunk seed (run seed + chunk coordinate), but we also
layer in *world-space* noise fields so that terrain features can “flow” continuously across
chunk borders.

This document describes the **macro river** pass: thin, continuous “water” ribbons carved
into the surface map.

## Representation

There is no dedicated `Water` tile type. Instead:

- `TileType::Chasm` represents **water basins / rivers** when `Game::atCamp()` is true.
- `TileType::Fountain` represents a small fishable water source on any floor.

Fishing specifically treats `Chasm` as fishable water **only** on the overworld/surface camp.

## Generation

Macro rivers are generated in `overworld::generateWildernessChunk()` as an additional pass
after base biome terrain (mountains, wetlands, deadwood, etc.) and before landmarks/trails.

### Key properties

- **World-space sampling:** river noise is evaluated at `(wx, wy)` where  
  `wx = chunkX * chunkWidth + localX` and `wy = chunkY * chunkHeight + localY`.  
  This means rivers are continuous across chunk borders.

- **Band carving:** we carve a river where a low-frequency noise value is close to 0.5:  
  `abs(noise - 0.5) < bandWidth`.  
  This produces thin, curving “ribbons” without needing to store a global river graph.

- **Biome parameters:** band width (and how strongly wet areas widen rivers) is tuned by biome:
  swamps/coasts are river-heavy, deserts have rare wadis, highlands avoid carving through
  mountain walls by default.

- **Connectivity safety:** the surface generator always carves organic trails between all
  edge gates *after* the river pass, guaranteeing the chunk remains traversable even when
  a river slices across it.

### Inspiration

The “terrain from layered noise” approach (elevation + moisture fields) is a proven baseline
for overworld biomes, and band-carving on a noise field is a simple way to create coherent
linear features (roads/rivers) on chunked infinite maps.

References:
- https://www.redblobgames.com/maps/terrain-from-noise/
- https://www.redblobgames.com/x/1723-procedural-river-growing/
- https://gamedev.stackexchange.com/questions/59314/procedural-river-or-road-generation-for-infinite-terrain
