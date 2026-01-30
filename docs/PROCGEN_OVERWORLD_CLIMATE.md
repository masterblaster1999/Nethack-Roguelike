# Overworld Climate

This round upgrades overworld procedural generation with a lightweight **climate pass** that couples a deterministic **prevailing wind** with **orography** (terrain height).

The intent is not strict meteorological realism; it’s to create *readable* macro-structure:

- **Windward slopes** trend wetter (uplift / condensation).
- **Lee-side basins** trend drier (rain shadow).

These nudges cause forests/wetlands to cluster on one side of major ridges while deserts/badlands can form behind them.

## Deterministic prevailing wind

A single per-run “climate wind” direction is chosen deterministically from `runSeed`:

- Always **cardinal** (`{±1,0}` or `{0,±1}`)
- Never calm (`{0,0}`)
- Does **not** consume gameplay RNG (it uses hashed seeds only)

This is intentionally distinct from the *moment-to-moment* overworld **weather wind** (see `PROCGEN_OVERWORLD_WEATHER.md`). Climate is the stable background; weather is the moving front-layer.

## Chunk-scale biome selection

Chunk identity (biome/name/danger depth) remains stable and cheap.

Biome selection now:

1. Samples three low-frequency FBM fields at chunk-space:
   - elevation (`elev`)
   - wetness (`wet`)
   - temperature (`temp`)
2. Applies basic **latitude cooling** (north/south) and an **altitude lapse-rate** (higher elevation is colder).
3. Applies a wind-aligned moisture correction:
   - slightly wetter when the current chunk rises relative to the immediate upwind chunk
   - slightly drier when a nearby upwind ridge is significantly higher (rain shadow)
   - small moisture bonus if low-elevation + wet “ocean-like” chunks exist upwind
4. Classifies into one of:
   `PLAINS / FOREST / SWAMP / DESERT / TUNDRA / HIGHLANDS / BADLANDS / COAST`

## Tile-scale wetness advection

Inside `generateWildernessChunk`, after the base per-tile wetness FBM field is computed, we apply an additional pass to the **wetness field** before carving lakes, rivers, vegetation, and springs.

For each interior tile:

- Look up the **maximum elevation** within a short **upwind window**.
- Compare the tile’s elevation to:
  - its immediate upwind neighbor (rise/drop)
  - the upwind window’s ridge “barrier” height
- Apply small adjustments:
  - **uplift boost** when rising into the wind
  - **shadow penalty** when below a strong nearby ridge

Because any off-chunk lookups are sampled in **world coordinates**, the correction remains *seam-free* across chunk borders.

## Tuning knobs

The pass is intentionally gentle; base noise still dominates. Key knobs:

- `win`: upwind lookback window (auto-sized from chunk dimensions)
- `upliftGain`: how strongly windward rises increase wetness
- `shadowGain`: how strongly rain shadows reduce wetness
- `leeGain`: extra penalty for sharp lee-side drops
- `ridgeStart`: only ridges above this elevation cast strong shadows

## Determinism and seams

- No new save state.
- No gameplay RNG consumption.
- Uses the same world-space noise functions as the base terrain fields, so seams are avoided.

## Why this matters

The overworld now gains an additional “story” layer purely from terrain:

- You can often *read the map* and anticipate that the lee side of a ridge will be harsher.
- Rivers and springs become more likely on windward / valley-like regions.
- Biomes form in larger contiguous patches instead of being purely noise-speckled.
