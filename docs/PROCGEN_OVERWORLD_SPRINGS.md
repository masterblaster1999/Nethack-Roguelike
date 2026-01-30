# Overworld Springs (Blue-noise POIs)

Round 216 adds a **minor overworld POI system** that places small, interactable **springs** in wilderness chunks.

A spring is represented by a **`TileType::Fountain`** tile (so it uses the existing drink/fish interactions), surrounded by a small clearing and a thin decorative ring (reeds/trees or stones depending on biome).

## Why

The wilderness already has macro features (mountains, rivers, landmarks, trails), but it benefited from an additional *small-scale* reason to step off the main road.

Springs:

- provide **discoverable micro-objectives** (a safe-ish, recognizable point on the map),
- add **repeatable utility** (drink / fish),
- and increase **local biome texture** without blocking gate connectivity.

## Placement algorithm

Springs are generated **deterministically per chunk** using a two-stage process:

1) **Blue-noise seed distribution**

We first sample candidate points using **Poisson-disc sampling** (Bridson-style), enforcing a minimum spacing so springs don't clump.

2) **Terrain-biased selection**

We score each candidate using the already-computed world-space fields:

- **wetness** (prefer wetter tiles),
- **slope/flatness** (prefer low-gradient basins / valley floors),
- **elevation** (avoid high ridges),
- and a small penalty for proximity to existing **river water** (to reduce redundant springs right next to chasms).

Candidates must also be **far from gates** and start on **walkable Floor**.

Finally, we select the top-scoring candidates and stamp 0..2 springs, with biome-specific rarity tuning.

## Decoration

After stamping the `Fountain` center tile, we:

- carve a small manhattan-radius **clearing** around it (keeps the POI readable),
- and place a thin ring of **Pillar** (reeds/trees) vs **Boulder** (stones) tiles depending on biome.

## Debug / telemetry

A new non-serialized counter is tracked on `Dungeon`:

- `overworldSpringCount`

It is shown in `#mapstats` as:

- `OVERWORLD | SPRINGS <N>` (only when `N > 0`).
