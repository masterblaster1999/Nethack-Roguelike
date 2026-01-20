# Procgen: Metaball Caverns (Implicit Surfaces)

This project’s default cavern generator is a classic **cellular automata** smoother (random fill → a few smoothing iterations → keep the largest connected component).

This document describes an alternate cavern mode added in **Round 96**: **metaball caverns**.

The goal is to produce caverns that feel **more organic and “melted”**, with fewer grid/CA artifacts, while keeping the same safety guarantees (connectivity + fallbacks).

---

## High-level idea

We generate a cave silhouette by evaluating an **implicit field** over the map and thresholding it.

1. Sample `N` blob centers (`cx, cy`) and radii `r`.
2. Evaluate a field at each tile:

   `f(x, y) = Σ (r_i^2 / (d(x,y,c_i)^2 + 1))`

3. Convert to terrain by thresholding:

   - `Floor` if `f(x,y) >= threshold`
   - `Wall` otherwise

This style is commonly called **metaballs**: multiple soft “influence” blobs merge smoothly when close.

---

## Why quantile thresholding

A constant threshold can be brittle: different random blob placements may yield a cave that is too open or too closed.

Instead, the implementation picks the threshold by **quantile**, targeting a stable floor coverage fraction (roughly ~44–60% depending on depth). This makes the output’s “openness” predictable across seeds.

Implementation detail:

- Collect all field values on interior tiles.
- Choose an index `cut = round((1 - floorFrac) * (N - 1))`.
- Use `std::nth_element` to find the value at that cut efficiently.

---

## Post-processing and robustness

To keep the generator safe and gameplay-friendly:

- A small smoothing step (2 iterations) removes pinholes and tiny artifacts.
- The generator keeps only the **largest connected floor component** (4-neighborhood).
- If the resulting cavern is too small (`kept < area/6`), it **falls back** to the default CA cavern, and if that fails, falls back to BSP rooms.

After the cave is carved, the generator uses the same “landmarks” strategy as the CA cavern:

- Carves a **start chamber** on the floor tile nearest the map center.
- Carves several **extra chambers** on random floor tiles.
- Places stairs using a BFS distance map (stairsDown = farthest reachable tile).

---

## Debug/telemetry

Metaball caves are tracked via non-serialized telemetry fields on `Dungeon`:

- `cavernMetaballsUsed`
- `cavernMetaballBlobCount`
- `cavernMetaballKeptTiles`

In-game, `#mapstats` prints a line like:

`CAVERNGEN | METABALLS <N> | KEPT <tiles>`

---

## Tuning knobs

If you want to tune the “feel”:

- **Blob count**
  - Higher → more fragmented / more interesting pockets
  - Lower → fewer but larger chambers
- **Radius range**
  - Larger radii → smoother and more open
  - Smaller radii → tighter winding pockets
- **Target floor fraction**
  - Higher `floorFrac` → more open caves
  - Lower `floorFrac` → tighter caves
- **Smoothing iterations**
  - More smoothing → rounder blobs but less detail

---

## Player-facing hint

When the floor uses metaballs, the game gives a subtle arrival hint:

> “THE CAVERN WALLS CURVE AS IF MOLDED FROM LIQUID STONE.”
