# Procedural Overworld Rivers

This project treats the overworld as an *infinite* stream of deterministic “wilderness chunks”.
Each chunk is generated from a stable chunk seed (run seed + chunk coordinates), but all large-scale
terrain features (including rivers) are sampled in **world coordinates** so they remain seamless across
chunk borders.

## Representation

- **Water** is represented as `TileType::Chasm` in overworld chunks.
- **Banks** are sparse `TileType::Pillar` (vegetation) and `TileType::Boulder` (rocks) placed along water edges.
- **Crossings** are created by the overworld trail pass: trail carving always overwrites the *centerline* tile to
  `Floor`, so paths can cross rivers. Each time a trail converts a `Chasm` tile to `Floor`, we increment
  `Dungeon::fluvialCausewayCount` (useful as a debug/telemetry readout).

## Generation

The overworld terrain pass first builds cached, per-tile fields:

- `elevation` (FBM noise)
- `wetness` (FBM noise)
- `variation` (hash jitter)

Those fields are reused for lakes, vegetation, rivers, landmarks, and trails.

### Lakes

Lakes are carved early as basins where elevation is low and wetness is high.

### River channels

Rivers are carved as a **domain-warped, multi-band noise field**:

1. **Trunks (macro rivers)**  
   Two low-frequency trunk fields are sampled as isovalue bands:
   - `RIV` sampled in standard coordinates
   - `RIV2` sampled in a rotated coordinate basis (adds variety + occasional confluences)

   The distance-to-band (`abs(n - 0.5)`) is compared against a biome-tuned width threshold.

2. **Domain warping**  
   A lower-frequency warp field offsets the sampling coordinates of the trunk and tributary noises.
   This produces more natural meanders and reduces “straight” noise contours.

3. **Width modulation**  
   The trunk width is modulated by:
   - **wetness** (wetter regions widen)
   - **local flatness** estimated from the elevation gradient (flat + wet areas widen further, creating a braided feel)

4. **Tributaries**  
   A higher-frequency tributary band (`RIVT`) activates only:
   - in sufficiently wet tiles
   - and *near* trunk valleys (within a multiple of the trunk width)

   This keeps tributaries visually tied to a main channel instead of turning into “random stripes”.

5. **Micro-channels (wetland texture)**  
   In very wet biomes (swamp/coast), an even higher-frequency band adds small capillary channels.

All sampling uses **world coordinates**, keeping the river field consistent across chunk boundaries.

### Riparian banks

After carving water, we decorate the edges:

- For each water tile, examine its 4-neighbors.
- Where a neighbor is `Floor`, place a `Pillar` or `Boulder` with a biome-tuned probability and bias
  (more vegetation in swamps/forests; more rocks in highlands/badlands).
- Gate “throats” (the walkable tile just inside each border gate) are kept clear to avoid ugly or annoying chokepoints.
- Trails and landmarks run *after* banks, so they can still carve through when needed.

### Key properties

- **Seamlessness:** all core fields are evaluated in world space, not chunk space.
- **Biome variation:** each biome tweaks river width, warp strength, tributary activation, and bank density.
- **Guaranteed travel:** trails are carved after rivers, ensuring gates remain mutually reachable even when rivers cut across the chunk.

### Inspiration / reading

- Red Blob Games — “Making maps with noise functions”: https://www.redblobgames.com/maps/terrain-from-noise/
- Red Blob Games — “Procedural river drainage basins”: https://www.redblobgames.com/x/1723-procedural-river-growing/
- Domain warping overview (paper): https://arxiv.org/html/2405.07124v1
