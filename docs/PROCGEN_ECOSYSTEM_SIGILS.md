# Ecosystem Sigils (Heartstones & Ecotones)

This patch makes the **procedural ecosystem system** more *gameplay-visible* by
spawning a tiny number of **ecosystem-aware sigils** on floors that have biome
seeds.

Sigils already exist as rare magical floor inscriptions (`SIGIL: ...`) that
trigger when stepped on (see `docs/PROCGEN_SIGILS.md`). This add-on layer makes
them context-sensitive: *biomes sometimes leave a "heartstone" behind, and the
seam between biomes can occasionally "tear" into an ecotone glyph.*

## Two new procedural placement archetypes

### 1) Heartstone sigils (biome cores)

On floors with ecosystems (Dungeon branches; not the surface Camp hub), the
generator may place **one sigil near the core of a randomly picked biome seed**.

* Placement: searches a small radius around the biome seed center and picks the
  first valid `Floor` tile that:
  * is walkable
  * is not a stair tile
  * is not inside a Shop room
  * is not already engraved
  * actually belongs to that biome (via the cached ecosystem field)

* Kind selection: each ecosystem maps to a **good/bad** sigil pair. A hash-based
  "bad" roll (slightly more likely on deeper floors) chooses between them.

  Example mapping:
  * Fungal Bloom → `REGEN` (good) / `VENOM` (bad)
  * Crystal Garden → `SEER` / `NEXUS`
  * Bone Field → `AEGIS` / `LETHE`
  * Rust Veins → `AEGIS` / `RUST`
  * Ashen Ridge → `AEGIS` / `EMBER`
  * Flooded Grotto → `SEER` / `LETHE`

The inscription adds a short biome token (e.g. `SIGIL: REGEN FUNGAL ...`) to make
the intent readable in LOOK without changing the routing keyword.

### 2) Ecotone sigils (biome boundaries)

If the floor is deep enough, the generator may also place a rarer **boundary
sigil** on a tile where the ecosystem field changes across cardinal neighbors.

* Candidate scoring:
  * must be a valid `Floor` tile (same constraints as Heartstones)
  * must have at least **two distinct ecosystem kinds** among itself and its
    neighbors
  * higher "diversity" (2+ distinct ecos) wins
  * deterministic hash breaks ties

* Kind selection: starts with a small baseline pool (`NEXUS`, `SEER`, `AEGIS`)
  and then mixes in the good/bad options from each of the two ecosystems.

The inscription adds `ECOTONE` plus biome tokens so it's obvious why it's there:
`SIGIL: NEXUS ECOTONE ASH RUST ...`.

## Determinism and RNG isolation

These placements are **hash-derived** from `(materialWorldSeed, materialDepth)`.
They do not consume the game's main RNG stream, which helps keep other procedural
systems (loot placement, monster rolls, etc.) stable.

Implementation lives in `Game::spawnGraffiti()`.
