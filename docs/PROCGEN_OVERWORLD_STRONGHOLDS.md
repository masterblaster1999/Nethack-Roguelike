# Overworld Strongholds (Ruined Settlements)

This document describes the **overworld stronghold** setpiece introduced in **Round 220**.

A *stronghold* is a rare, high-signal surface point-of-interest: a ruined walled settlement with a central keep, scattered outbuildings, rubble, and a guaranteed loot cache.

## Goals

- Make wilderness chunks feel inhabited without turning them into towns everywhere.
- Provide memorable landmarks worth detouring for.
- Guarantee reachability by connecting the entrance to the chunk trail hub.
- Reuse existing tile types and spawn systems (doors, rooms, bonus spawn requests).
- Stay deterministic per chunk.

## Placement

Strongholds roll once per chunk from a seed derived from **(runSeed + chunk coordinates)**. The chance scales with **danger depth** and is modulated by **biome** (rarer in desert/tundra, more common in plains/highlands/badlands).

A footprint is accepted only if:

- It is far from border gates (so gates remain readable).
- It avoids heavy chasm coverage (prevents rivers splitting the setpiece).
- It avoids extreme mountain mass (so it doesn’t stamp onto solid wall fields).

## Geometry

1. The footprint rectangle is cleared to `Floor` to represent ancient graded ground.
2. A broken perimeter wall is drawn as an ellipse ring using `Wall` tiles, with deterministic gaps and occasional `Boulder` rubble.
3. A main breach is carved and marked with `DoorOpen` so the entrance reads at a glance.
4. A central keep is carved (rectangular room), slightly biased away from the breach.
5. Additional outbuildings are placed with Poisson-disc sampling inside the footprint; each receives a door or breach and extra collapsed rubble.

## Loot and spawns

- The keep requests a guaranteed chest cache via `Dungeon::bonusLootSpots`.
- There is a small chance for a second minor cache in an outbuilding.
- A few fixed ambient items (gold, food) can be requested via `Dungeon::bonusItemSpawns`.
- The keep entrance may be `DoorLocked`, which automatically triggers the existing key/lockpick guarantees in the item-spawn logic.

## Trail integration

After the chunk’s gate-to-hub trails are carved, the generator also carves a spur trail from the stronghold entrance back to the hub. This ensures the stronghold is discoverable and not accidentally isolated by walls/chasms.

## Telemetry

`Dungeon` tracks:

- `overworldStrongholdCount`
- `overworldStrongholdBuildingCount`
- `overworldStrongholdCacheCount`
- `overworldStrongholdWallTiles`

These are surfaced by `#mapstats` for quick sanity checks.
