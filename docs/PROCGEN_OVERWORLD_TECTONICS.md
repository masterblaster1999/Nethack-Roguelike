# Procedural Overworld: Tectonic Plates & Mountain Chains (Voronoi Ridges)

This patch upgrades overworld wilderness generation with a **macro-scale tectonics field** that produces **coherent mountain chains**, **basins**, and more readable **plate-like regions**.

The goals:

- Make mountains feel like **ranges** instead of noise blobs.
- Create large-scale **continents vs low basins** without introducing chunk seams.
- Improve terrain readability while keeping existing systems (rivers, trails, springs, brooks) intact.

## High-level idea

We evaluate a deterministic **Worley / Voronoi** field in **world tile coordinates**:

1. Scatter one “feature point” per coarse **plate cell** (stable per cell via hashing).
2. For any world coordinate `(wx, wy)`, compute:
   - `F1`: distance to the nearest feature point,
   - `F2`: distance to the second-nearest feature point.
3. The quantity `(F2 - F1)` is **small near Voronoi boundaries**, so it becomes a natural **ridge mask**:
   - boundary ≈ mountain chain
   - interior ≈ plate basin / continent interior

This is a standard trick for generating Voronoi cell boundaries (ridges) using **F1/F2 Worley noise**.

## Two scales: major plates + minor sub-ridges

To avoid a single uniform “mountain stripe” frequency, we combine two ridge fields:

- **Major plates** (large cell size): create the big, readable mountain ranges.
- **Minor plates** (smaller cell size): add secondary sub-ridges and foothills.

The final ridge mask is a weighted sum of both, with extra FBM modulation so ridge strength varies along its length.

## Deterministic passes (“gaps”)

Long unbroken walls are bad for navigation and boring to read, so the major ridge mask is multiplied by a low-frequency **pass noise**. When the pass noise is low, ridge intensity is reduced, creating occasional **saddles / passes** through mountain chains.

Trails still do their own connectivity planning afterward, but these natural gaps make the world feel less “generated”.

## How it influences terrain

After the existing **climate wetness advection** pass, the tectonics system modifies cached fields:

- **Plate elevation offset:** each major plate gets a small elevation bias (continents vs basins).
- **Ridge lift:** elevation is boosted near plate boundaries to form mountain chains.
- **Ridge drying:** wetness is reduced slightly on ridges to create more barren crests.

During tile placement:

- The **mountain threshold** is lowered locally in strong ridge areas (ridges become walls more reliably).
- **Vegetation** is reduced on ridges (requires higher wetness + lower chance).
- **Scree/boulders** become a bit more common on ridges.

## Talus aprons (foothills)

After carving macro rivers (and their riparian banks) but before placing landmarks/ruins, we add a lightweight **talus apron** pass:

- Floor tiles adjacent to tectonic mountain walls have a biome-tuned chance to become **Boulders**.
- This creates more natural “foothill clutter” instead of razor-thin wall borders.
- The pass avoids gate throats to reduce spawn/travel friction.

## Debugging and testing

A new test (`overworld_tectonics`) samples the world-space ridge field and asserts that it produces both low interior values and high boundary values (extrema exist), keeping the system deterministic and non-degenerate.

## Key implementation hooks

- `overworld::sampleTectonics(...)` — returns `{ ridge01, plateOffset }` for a world coord.
- `overworld::tectonicRidge01(...)` — convenience ridge sampler for tests/debug.
- `generateWildernessChunk(...)` — applies tectonics to cached elevation/wetness fields, then uses a local ridge-aware mountain threshold and a talus apron decorator.
