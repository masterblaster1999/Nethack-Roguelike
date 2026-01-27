# Procedural Graffiti Rumors

ProcRogue already supports **Engravings** (player-written), **Wards** (special words that repel/affect monsters), and a few rare **Sigils** ("SIGIL: ..." magical floor inscriptions).

This patch upgrades the dungeon's *generated* graffiti so it behaves more like a lightweight "rumor" system:

- Graffiti remains **sparse** (max ~8 per floor).
- Lines are **short** (clipped to 72 chars) for readability in LOOK mode.
- Many lines are **contextual hints** based on the actual generated floor layout.
- Everything is **deterministic** (stable for a given run seed + level identity).

## Where it lives

- `Game::spawnGraffiti()`
  - Clears and (re)places generated graffiti each time a floor is generated.
  - Still places rare **Sigils** (now procedurally parameterized; see `PROCGEN_SIGILS.md`).
  - Now delegates text generation to `graffitigen::generateLine(...)`.

- `src/graffiti_gen.hpp`
  - Header-only procedural graffiti generator.
  - Scans the current `Dungeon` to build a **hint pool** (`collectHints`).
  - Builds either:
    - a **hint line** (directional, feature-aware)
    - or an **ambient line** (small grammar + classic one-liners)

## Hint pool (examples)

`collectHints()` currently looks for:

- `TileType::DoorSecret` (secret doors)
- `TileType::DoorLocked` (locked doors)
- `TileType::Chasm` (a centroid hint to avoid flooding)
- `TileType::Boulder` next to a `Chasm` ("boulder bridge" opportunities)
- Special rooms (weighted):
  - `RoomType::Vault`
  - `RoomType::Shrine`
  - `RoomType::Shop`

Hints are *weighted* so rare rooms (vault/shrine/shop) appear often enough even if many secret doors exist.

## Locality + direction

When a hint line is chosen:

- The generator prefers hint targets within a small manhattan radius of the graffiti tile.
- Direction uses an 8-way label (`NORTH`, `SOUTHWEST`, ...).
- Distance is bucketed (`NEAR`, `CLOSE`, `FAR`, `VERY FAR`).

This keeps graffiti *useful* without giving exact coordinates.

## Determinism

The generator is designed to avoid consuming extra RNG state:

- `spawnGraffiti()` draws a single `rng.nextU32()` per graffiti line.
- That seed is expanded using `hash32(...)` in `graffiti_gen.hpp`.

As a result, increasing the complexity of the grammar does not require extra RNG draws.

## Extending

Easy upgrades:

- Add new `HintKind` values (doors, setpieces, trap clusters once trap spawning runs).
- Add more room-specific "voices" or branch-specific vocab.
- Add optional UI affordances (e.g. a glossary tab for collected graffiti).
