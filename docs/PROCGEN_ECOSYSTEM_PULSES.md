# Procedural Ecosystem Pulses

Ecosystems already shape **terrain materials**, **ambient particles**, **traps**, **sigils**, **guardians**, and (as of Round 204) **spawn ecology**.

**Round 205** adds another layer of "the dungeon is alive": periodic, deterministic **ecosystem pulses**.

A pulse is a small, localized biome event that fires occasionally **near an ecosystem core**, seeding a short-lived hazard/boon that then naturally diffuses via the existing field simulation.

## Scheduling

- Pulses run only in the **main dungeon** (`DungeonBranch::Main`).
- Each floor selects an interval of roughly **22–34 turns** (mildly depth-scaled).
- A per-floor **phase offset** is derived from deterministic hashes of:
  - `materialWorldSeed()`
  - `materialDepth()`
  - `branch`
- When the schedule hits, a single pulse is emitted.

## Choosing a pulse center

When a pulse triggers, the game:

1. Chooses one `EcosystemSeed` (kind != `None`) deterministically.
2. Samples a point near the seed center (within a clamped fraction of the seed radius) until it finds a good tile:
   - in-bounds
   - **walkable**
   - in the **same ecosystem** as the chosen seed
   - not on **stairs**
   - not directly under the **player**
   - not directly under another **entity**

This keeps pulses flavorful and fair (no unavoidable "spawn on your head" hazards).

## Pulse themes

Each ecosystem kind maps to a small, themed event:

- **FungalBloom**
  - *Spore burst* → injects a small bloom of **confusion gas**.

- **RustVeins**
  - *Acrid vent* → injects a small bloom of **corrosive gas**.

- **AshenRidge**
  - *Ember fissure* → injects a small bloom of **fire** (and at deeper depths, a tiny smoky/confusion fringe).

- **FloodedGrotto**
  - *Cool mist* → gently **quenches nearby fire tiles**, can reduce the player’s **burn duration** if they’re close, and emits a dripping **echo noise** that can attract monsters.

- **CrystalGarden**
  - *Resonance chime* → emits noise; if the player is nearby and mana is not full, restores **1–2 mana**.

- **BoneField**
  - *Ossuary miasma* → injects a small bloom of **poison gas**.

All pulses reuse existing hazard fields, so they automatically:

- drift with wind and corridor flow
- interact with fire/gas chemistry
- apply status effects through the normal per-turn hazard logic

## Determinism and balance

- **RNG-isolated:** pulse selection/placement uses a local `RNG` seeded from `hashCombine(...)` salts; it does not consume from the game’s primary RNG stream.
- **Stable across save/load:** pulses are keyed off `turnCount` and deterministic floor parameters.
- **Not a global difficulty spike:** intensities are mild with gentle depth scaling, and pulses avoid spawning directly under the player.

## Implementation notes

Implemented in `Game::applyEndOfTurnEffects()` (`src/game_spawn.cpp`) immediately after hazard-field arrays are sized and before field reactions are processed.

Key APIs:

- `Dungeon::ecosystemSeedsCached()`
- `Dungeon::ecosystemAtCached()`
- hazard fields: `confusionGas_`, `poisonGas_`, `corrosiveGas_`, `fireField_`
- `emitNoise(...)` and `pushFxParticle(...)` for telegraphing
