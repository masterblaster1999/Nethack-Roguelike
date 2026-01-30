# Overworld Brooks (Spring-Fed Micro-Hydrology)

This pass makes **springs** (placed as `TileType::Fountain`) feel physically “real” by giving them a visible outlet:

- Each spring attempts to **spill downhill** into a narrow **brook** (carved as `TileType::Chasm`), following a cheap **local steepest-descent** rule.
- If the brook can’t find a meaningful downhill continuation (a **local sink / basin**), it terminates in a small **pond** (also `Chasm`), which reads as an oasis or seep lake depending on biome.
- A very sparse **bank** decoration ring (Pillars vs Boulders) is added around some brook/pond edges for readability.

The algorithm is deterministic per chunk (run seed + chunk coords) and uses only world-coordinate fields, so it remains **seam-free across chunk borders**.

---

## Where this runs

Order inside `overworld::generateWildernessChunk(...)` (high level):

1. Base terrain noise fields (`elevField`, `wetField`, `varField`)
2. Macro lakes/rivers + bank decoration
3. Biome landmarks
4. **Springs** (Poisson-disc candidates)
5. **Brooks + ponds** (this pass)
6. Trails (A* road planner), waystations, etc.

Putting brooks **before trails** ensures that even if water cuts awkwardly across a gate approach, the trail pass will always carve a causeway and keep all gates reachable.

---

## Core algorithm

For each spring center `S`:

1. Pick an adjacent start tile `P` (one of the 8 neighbors) that is:
   - inside the chunk interior,
   - not near any border gate throat,
   - not a Wall,
   - and *preferably* at lower elevation than `S` (but allowed to be slightly higher on flats).

2. Walk forward for up to `maxLen` steps:
   - Convert `P` to `Chasm` (unless it’s already water).
   - Choose the next step among the 8 neighbors by minimizing a score:
     - **elevation** (primary driver),
     - + small penalties for **boulders/trees**,
     - + a small **turn penalty** (inertia),
     - + tiny deterministic jitter (break ties),
     - − tiny wetness pull (prefers valley micro-wetness).
   - Allow tiny rises up to `flatSlack` so brooks don’t die on flat plains immediately.
   - If no neighbor qualifies (or we sit in an extended flat basin), carve a small **pond** at the current position and stop.
   - If we reach existing `Chasm` (a river/lake), stop (we joined the hydrology).

This is conceptually similar to common “grid flow direction” terrain drainage approaches (often called **D8** in GIS), but simplified to keep the runtime tiny and remain robust for roguelike chunk generation.

---

## Telemetry

The dungeon struct tracks overworld-only hydrology stats:

- `overworldSpringCount` — number of springs (Fountains).
- `overworldBrookCount` — number of brooks successfully carved.
- `overworldBrookTiles` — tiles converted to `Chasm` by brooks/ponds.
- `overworldPondCount` — number of terminal ponds.

`#mapstats` prints these when present under an `OVERWORLD` line.

---

## Tuning notes

The pass is biome-aware:

- **Swamp / Forest / Coast:** higher chance to form brooks, more bank vegetation (Pillars).
- **Highlands / Badlands / Tundra:** shorter, steeper brooks; rockier banks (Boulders).
- **Desert:** springs always try to produce at least an **oasis pond** so the feature is readable.

All decisions are deterministic: no global RNG state is consumed during gameplay.

