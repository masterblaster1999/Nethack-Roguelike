# Procedural Ecosystem Microclimates

This project already uses a **procedural ecosystem seed** system to label dungeon regions (Fungal Bloom, Crystal Garden, etc.) and to bias stealth/scent and hazard ambience.

This document describes the next layer: **microclimates** — small, deterministic, *local* environment modifiers that make each ecosystem feel tactically distinct without turning it into a hard ruleset.

## What microclimates do

Microclimate modifiers are applied **per tile** based on its ecosystem label:

- **Visibility (FOV)**: some ecosystems slightly change the player's sight radius while standing in that region.
- **Hazard persistence/drift**: gas and fire fields can dissipate a bit faster, or (for dense/strong fields) linger a bit longer.

The effect sizes are intentionally small and are meant to *stack with* existing systems:

- terrain materials (absorption/slickness)
- weather (overworld)
- ecosystem hazard pulses (events)

## Where it lives in code

- `src/dungeon.hpp`
  - `struct EcosystemFx` now includes microclimate fields (`fovDelta`, gas quench/spread deltas, fire quench + spread multiplier).
  - `ecosystemFx(EcosystemKind)` is the single lookup table.

- `src/game_world.cpp`
  - `Game::recomputeFov()` applies `ecosystemFx(ecoHere).fovDelta` to the base radius.

- `src/game_spawn.cpp`
  - `Game::applyEndOfTurnEffects()` applies microclimate deltas during gas diffusion/decay and fire decay/spread.
  - Negative gas quench values only boost **dense** clouds (so wisps still fade normally).
  - Negative fire quench values only boost **strong** fires (so embers still die out).

- `src/game_look.cpp`
  - LOOK mode shows `VIS:+N` / `VIS:-N` alongside the `ECO:` tag.

## Ecosystem microclimate cheat sheet

These are the current intended vibes:

- **Fungal Bloom**
  - `VIS:-1` (spore haze)
  - poison/confusion gas tends to linger and drift slightly farther
  - fire is slightly more likely to sputter out

- **Crystal Garden**
  - `VIS:+1` (light scatter)
  - fire spread chance reduced (hard, non-flammable substrate)

- **Rust Veins**
  - `VIS:-1` (dusty, metallic haze)
  - corrosive gas tends to cling/linger

- **Ashen Ridge**
  - `VIS:-1` (ash/smoke)
  - gases disperse a bit faster, but strong fires can linger/spread

- **Flooded Grotto**
  - `VIS:-1` (mist)
  - gases are scrubbed quickly; fire is strongly quenched

- **Bone Field**
  - neutral visibility; mostly an acoustic/stealth identity

## Design notes

- All effects are deterministic per tile and derived from the level's ecosystem seed layout.
- Deltas are small and are clamped/thresholded to avoid runaway/permanent hazards.
