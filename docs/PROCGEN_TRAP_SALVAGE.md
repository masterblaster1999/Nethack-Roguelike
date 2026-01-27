# Procedural Trap Salvage

This patch makes **trap disarming** feed directly into **procedural crafting**.

When you successfully disarm a **floor trap** (or a known **chest trap**), the game may award a deterministic **Essence Shard** that represents the trap's "essence".

The core goals are:

- **Deterministic**: salvage is derived from stable inputs and does **not** consume the main RNG stream.
- **Save-safe**: reuse the existing `ItemKind::EssenceShard` encoding (no new save fields).
- **Meaningful**: disarming traps now produces tangible crafting fuel without making it mandatory.

## Where it lives

- `src/trap_salvage_gen.hpp`
  - Header-only generator.
  - Produces a `trapsalvage::SalvageSpec` (tag/tier/count/shiny/spriteSeed).

- `Game::disarmTrap()` (`src/game_interact.cpp`)
  - On a **successful** disarm, calls the generator and (if any salvage is produced) adds the shard to inventory.
  - The disarm success message is upgraded to:
    - `YOU DISARM THE ... TRAP.`
    - `YOU DISARM THE ... TRAP AND SALVAGE <SHARD>.`

## Determinism

Salvage uses `hashCombine`/`hash32` with compile-time domain tags (see `rng.hpp`):

- **Floor traps** seed:
  - `(runSeed, depth, pos, trapKind)`

- **Chest traps** seed:
  - `(runSeed, depth, chestSeed, trapKind, chestTier)`

Because this is hash-driven, adding more salvage logic later will not perturb unrelated RNG-driven game systems.

## Tag mapping

Traps map onto existing crafting tags:

- `Spike`, `RollingBoulder`, `TrapDoor`  → **STONE**
- `PoisonDart`, `PoisonGas`              → **VENOM**
- `Teleport`                             → **RUNE**
- `Alarm`                                → **ARC**
- `Web`                                  → **SHIELD**
- `ConfusionGas`                         → **DAZE**
- `LetheMist`                            → **CLARITY**
- `CorrosiveGas`                         → **ALCH**

## Scaling

- **Chance** to salvage and shard **tier** scale modestly with `depth`.
- **Chest traps** are biased upward (slightly higher chance and tier) to reward higher-risk interactions.
- A small deterministic **jitter** prevents every trap of the same kind from always yielding identical tiers.

## Tests

`tests/test_main.cpp` includes `trap_salvage_procgen`:

- Verifies determinism for a range of trap kinds.
- Validates tier/count bounds.
- Validates the stable tag mapping.

## Future extensions

Natural upgrades that fit the current architecture:

- Salvage additional byproducts (mechanical parts vs. magical residue) without new RNG draws.
- Add rare salvage outcomes (e.g. runic fragments) for magical traps.
- Let certain monsters/skills improve salvage (still deterministically) by salting the seed with skill state.
