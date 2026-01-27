# Procedural Overworld Waystations (Caravan Shops)

This project’s overworld is an infinite grid of deterministic **wilderness chunks** (Camp branch, depth 0).  
Each chunk is generated from `(runSeed, chunkX, chunkY)`.

This document describes **Overworld Waystations**: rare, biome-aware **traveling merchant caravans** that appear as small shop structures embedded in wilderness chunks.

## Goals

- **Exploration reward:** occasionally stumbling on a merchant in the wilderness breaks up long treks.
- **Deterministic:** waystations are generated purely from the chunk seed (no gameplay RNG drift).
- **Uses existing systems:** shops use the same `RoomType::Shop` logic as dungeon shops:
  - shopkeeper spawning
  - procedural shop theme/profile
  - stocked items-for-sale
- **Safe space:** normal wilderness spawns should *not* bleed into shop interiors.

## How it works

### 1) Placement decision

In `overworld::generateWildernessChunk()` we derive an independent RNG:

- `stationRng = hashCombine(chunkSeed, "OW_STATION")`

A biome-aware chance is used (example values):

- Plains: ~10%
- Coast: ~8%
- Highlands: ~7%
- Forest: ~5.5%
- Swamp: ~4.5%
- Badlands: ~3.5%
- Tundra: ~3%
- Desert: ~2.5%

The chance is additionally scaled down at high `dangerDepth` so caravans become rarer far from camp.

### 2) Footprint search

If the roll succeeds, the generator searches for a valid rectangle footprint:

- biased to be near the **trail hub** (so it’s naturally reachable)
- far enough from edge **gates** to avoid blocking travel
- avoids carving through **water/chasms** (rivers/lakes)
- avoids cutting directly through mountain ridges (too much `TileType::Wall` in the footprint)

### 3) Carving the structure

Once a footprint is found:

- Border tiles become `TileType::Wall`
- Interior tiles become `TileType::Floor`
- A `TileType::DoorOpen` is placed on the side facing the hub

### 4) Ensuring connectivity

A short approach tile outside the door is carved into `Floor`, and the same meandering
trail walker used for gate-to-hub trails is called to connect the waystation to the hub.

This ensures the shop is connected into the chunk’s travel network.

### 5) Making it a shop

A `Room` is pushed with:

- `RoomType::Shop`
- rectangle bounds matching the carved structure

This is pushed **before** the chunk’s large catch-all `RoomType::Normal` so:

- `roomTypeAt()` resolves shop tiles correctly

## Spawn-safety: preventing hostile spawns inside shops

Overworld chunks intentionally use a large `RoomType::Normal` that covers almost the entire chunk
(for simple spawn logic).

Because of that, the generic spawn helpers must respect special sub-rooms.

`Game::randomFreeTileInRoom()` now validates that the candidate tile’s `roomTypeAt()` matches the room
it is spawning for. This prevents “normal room” monster/item spawns from appearing inside the shop’s
`RoomType::Shop` tiles.
