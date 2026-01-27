# Procedural identification appearance phrases

This project uses a classic NetHack-style identification model: certain item kinds
(potions, scrolls, rings, wands) do **not** show their true names until identified.
Instead, each run assigns every such item kind an **appearance id** (e.g. a gem
color/material).

This round upgrades the *appearance labels* themselves so they are also
procedurally generated.

## Goals

- **Deterministic / replay-safe**: appearance phrases are derived from
  `(run seed, appearance id)` and do not consume the global RNG stream.
- **Save-format neutral**: the strings are not serialized; they regenerate on load.
- **Readable**: potions/rings/wands always preserve the original base material
  word (e.g. `RUBY`, `OAK`, `OPAL`) so player note-taking still works.
- **Quote-safe**: scroll labels are sanitized so they can be wrapped in quotes in
  the HUD/inventory without breaking formatting.

## What changes in-game

### Potions / Rings / Wands

These now gain a small descriptor prefix:

- `RUBY POTION` → `BUBBLING RUBY POTION`
- `OPAL RING` → `ENGRAVED OPAL RING`
- `OAK WAND` → `CARVED OAK WAND`

The descriptor is generated deterministically from `(seed, appearance id)`.

### Scrolls

Scroll labels become a 2–3 word incantation:

- `SCROLL 'ZELGO'` → `SCROLL 'ZELGO VORPAL'`

The first word remains anchored to the base appearance id so the mapping between
appearance and label stays intuitive.

## Implementation

See `src/ident_gen.hpp` and `Game::appearanceName()` in `src/game.cpp`.
