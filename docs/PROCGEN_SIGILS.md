# Procedural Sigils

ProcRogue has three closely related "floor text" systems:

* **Engravings** – player-written text via `#engrave`.
* **Wards** – special engravings (e.g. `ELBERETH`) that repel certain monsters.
* **Sigils** – rare magical engravings whose text begins with `SIGIL` and which
  trigger when stepped on.

This patch upgrades sigils from a handful of fixed, identical effects to a
**procedurally parameterized** system.

## Design goals

* **Feels procedural but stays deterministic**: sigil parameters are derived
  from `(run seed, depth, position, archetype keyword)` so the same run always
  generates the same sigil at the same tile.
* **No new save format requirements**: everything is derived from existing
  run state + tile position.
* **Readable in LOOK**: the *first* keyword token after `SIGIL` is the archetype
  (e.g. `EMBER`). Additional words are purely flavor.

## Sigil inscription format

Sigils are detected with a simple rule:

*Any engraving whose text begins with `SIGIL` (case-insensitive) is a sigil.*

The effect archetype is the first alphabetical token after `SIGIL`:

* `SIGIL: EMBER ASHEN LANTERN` → keyword `EMBER`
* `SIGIL OF MIASMA` → keyword `MIASMA`

Only the **keyword** is used for effect routing; the rest is flavor.

## Archetypes

The current procedural sigil archetypes are:

* `SEER` – reveals nearby hidden doors, undiscovered traps, and trapped chest status.
* `NEXUS` – teleports the victim.
* `MIASMA` – spawns confusion gas and applies confusion.
* `EMBER` – spawns fire and applies burning.
* `VENOM` – spawns poison gas and applies poison.
* `RUST` – spawns corrosive gas and applies corrosion.
* `AEGIS` – grants shield + parry (player/allies only).
* `REGEN` – grants regeneration + a small heal (player/allies only).
* `LETHE` – amnesia shock for the player; causes monsters to forget the player's last known position.

## Implementation notes

The generator lives in `src/sigil_gen.hpp`:

* `sigilgen::makeSigil(runSeed, depth, pos, keyword)` returns a fully described
  `SigilSpec` (radius/intensity/duration/etc.).
* `sigilgen::makeSigilForSpawn(...)` chooses a room-biased archetype so labs
  skew toward hazards while shrines skew toward utility.

Sigils are placed as part of `Game::spawnGraffiti()` and triggered in
`Game::triggerSigilAt()`.

### Seed/depth domain note (Overworld)

In the overworld wilderness (Camp depth 0, non-home chunks), the game uses
`Game::materialWorldSeed()` and `Game::materialDepth()` to key procedural content
so each chunk can have its own "dialect" and difficulty scaling.

Sigil parameters/effects use the **same** material-seed + material-depth domain so
the inscription you see and the effect you get stay consistent even in wilderness
chunks.
