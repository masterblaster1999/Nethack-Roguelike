# Procedural Leylines (Arcane Resonance Field)

The game already computes a few **deterministic, per-floor fields** (terrain materials, biome/ecosystem seeds, bioluminescence) to make a run feel *coherent* without saving extra simulation state.

**Leylines** extend that approach.

On every non-camp dungeon floor we compute a per-tile **arcane resonance intensity**:

- **Type:** `uint8_t` per tile
- **Range:** `0..255`
- **Storage:** `Dungeon::leylineCache` (not serialized)
- **Generation:** `Dungeon::ensureMaterials(...)` (computed alongside terrain materials + ecosystem seeds)

## Where it lives

- Cache: `Dungeon::leylineCache` (in-memory only, derived from the floor key)
- Query API:
  - `Dungeon::leylineAt(x, y, worldSeed, branch, depth, maxDepth)`
  - `Dungeon::leylineAtCached(x, y)` (requires `ensureMaterials(...)` has been called for the current key)

## How it is computed

Leylines are derived from three main deterministic drivers (plus a small jitter term):

1. **Heightfield ridges** (the same macro heightfield used for materials).
   - Produces long, continuous linework.
2. **Ecosystem ecotones and junctions**.
   - Biome boundaries strengthen lines.
   - 3+ ecosystem junctions become “nodes”.
3. **Conductive substrate nudges**.
   - Metal/crystal materials slightly boost resonance.

Implementation notes:

- Only computed for **walkable tiles** (plus chasms as “sinks” for levitation paths). Opaque/solid tiles are left at 0.
- Not saved: the field is always recomputable from the same floor key.

## Gameplay effects

Leylines are intentionally subtle: they're a tactical nudge, not a mandatory mechanic.

### Mana regeneration

In `Game::updatePlayerStatus()` (see `src/game_spawn.cpp`), the baseline deterministic mana regen interval is reduced when standing on stronger currents.

Very strong currents can also grant an occasional extra mana tick (still keyed purely off `turnCount` so it remains deterministic across save/load).

Tier thresholds:

- **< 120:** no boost
- **120–169:** FAINT (regen interval -1)
- **170–219:** BRIGHT (regen interval -2)
- **≥ 220:** STRONG (regen interval -3)

### Spell failure

In `Game::spellFailChancePct()` (see `src/game_spellcasting.cpp`), stronger currents slightly increase casting power (reducing failure chance). Very low intensity can add a tiny penalty (“static”) so dead-zones feel different.

## UI

- LOOK/inspect now prints `LEY: FAINT/BRIGHT/STRONG` when the tile's intensity is ≥ 120.
- Camp floors are all-zero by construction.

## Tuning

Primary tuning knobs live in `Dungeon::ensureMaterials()` under the `Leyline / arcane resonance field` section:

- ridge thresholds
- ridge/ecotone/junction/substrate weights
- sharpening curve
- low cutoff (dead-zone threshold)

These were chosen to keep hot lines thin while ensuring typical maps still get meaningful currents.

## Future hooks

Ideas that fit leylines well:

- Rituals/sigils that “snap” to strong currents
- Spellcasting monsters preferring ley tiles
- Rare artifacts that store charge faster on strong lines
- An optional overlay/scan spell to reveal ley patterns on the minimap
