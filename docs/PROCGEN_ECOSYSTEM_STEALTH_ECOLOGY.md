# PROCGEN_ECOSYSTEM_STEALTH_ECOLOGY

This document describes a small but far-reaching “stealth ecology” layer built on top of the
procedural ecosystem system.

The goal is to make biome regions feel meaningfully different for *stealth* and *tracking* without
introducing new opaque rules. The changes are deliberately lightweight (think: **TerrainMaterialFx**
scale), but they touch core perception systems:

- **Footstep noise** (how loud you are when moving)
- **Hearing** (how easily a listener can pick up a sound)
- **Listening** (player “#listen” action effectiveness)
- **Scent trails** (how long scent persists + how far it spreads)

## Design principles

- **Deterministic**: derived from the existing per-floor ecosystem cache (no new RNG stream usage).
- **Composable**: ecosystem deltas layer on top of substrate/material deltas.
- **Small numbers**: meant to bias decisions, not hard-lock strategies.
- **Player-facing**: biome entry flavor text hints at the stealth character of a region.

## The data model

Implemented in `src/dungeon.hpp`:

- `struct EcosystemFx`
- `inline EcosystemFx ecosystemFx(EcosystemKind e)`

`EcosystemFx` currently exposes:

- `footstepNoiseDelta`
- `hearingMaskDelta` (positive = masking / noisier; negative = echo / clearer)
- `listenRangeDelta`
- `scentDecayDelta`
- `scentSpreadDropDelta`

These deltas are applied in a few key places (see “Code pointers”).

## Acoustic ecology

### Footstep noise

Footstep noise is still primarily influenced by:

- player state (sneak, footwear, burden)
- substrate materials (moss/metal/etc.)

Ecosystems now provide an additional delta via:

- `Game::playerFootstepNoiseVolumeAt()` (`src/game_interact.cpp`)
- the *hearing preview* overlay (`src/game_look.cpp`)

This makes (for example) Flooded Grotto steps splashier and Fungal Bloom steps slightly softer.

### Hearing mask / echo

When a sound is emitted (`Game::emitNoise()`), each potential listener now gets a small biome-based
hearing adjustment:

- loud, masking regions (water/wind) reduce effective hearing
- echoing regions (crystal/metal) increase effective hearing

This is modeled as:

- `eff = volume + entityHearingDelta(listener) - ecosystemFx(listenerBiome).hearingMaskDelta`

The *hearing preview field* used by sneak auto-move uses the same rule so UI matches behavior.

### Player #listen action

The player’s listening action is also adjusted by the biome they are standing in:

- Flooded Grotto: harder to listen (drip/splash masking)
- Crystal Garden: easier to listen (echo)

This uses `ecosystemFx(ecoHere).listenRangeDelta` in `Game::listen()`.

## Olfactory ecology

The scent field uses the shared `updateScentField()` helper.

Each tile already provides a material-based `ScentCellFx` (decay + spread drop). Ecosystems now add
another small delta on top of the material values during `Game::updateScentMap()`.

Practical result:

- **Flooded Grotto** washes scent away quickly (higher decay and spread drop)
- **Fungal Bloom** preserves + carries scent (lower decay and spread drop)

This directly affects:

- monster tracking logic (AI follows higher scent gradients)
- the scent preview overlay

## Current tuning table

These are the current ecosystem deltas (small by design):

| Ecosystem | Footsteps | Hearing (mask/echo) | Listen | Scent trails |
|---|---:|---:|---:|---|
| Fungal Bloom | -1 | +1 (mask) | -1 | linger + spread (decay -1, drop -3) |
| Crystal Garden | +1 | -1 (echo) | +1 | thin (decay +1, drop +2) |
| Bone Field | +1 | 0 | 0 | neutral |
| Rust Veins | +1 | -1 (echo) | +1 | thin (decay +1, drop +2) |
| Ashen Ridge | +1 | +1 (mask) | -1 | thin (decay +2, drop +4) |
| Flooded Grotto | +2 | +2 (mask) | -2 | washes away (decay +4, drop +8) |

## Code pointers

- `src/dungeon.hpp`
  - `EcosystemFx` / `ecosystemFx()`
- `src/game_interact.cpp`
  - `Game::playerFootstepNoiseVolumeAt()` (ecosystem footstep delta)
- `src/game_save.cpp`
  - `Game::emitNoise()` (listener ecosystem hearing mask)
- `src/hearing_field.hpp`
  - `buildVisibleHostileHearingField()` (UI + automove uses same mask)
- `src/game_loop.cpp`
  - `Game::listen()` (ecosystem listen range delta)
- `src/game_world.cpp`
  - `Game::updateScentMap()` (ecosystem scent deltas)
