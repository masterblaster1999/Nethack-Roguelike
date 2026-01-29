# Procedural Ecosystem Spawn Ecology

The game already uses a depth-based **spawn table** (optionally overridden by `procrogue_content.ini`) and then applies small, deterministic nudges based on **room type** and **terrain material**.

**Round 204** extends that system so monster spawns also consult the per-floor procedural **ecosystem field** (`Dungeon::ecosystemAtCached()`), making biome patches feel like they have a more coherent, local ecology.

## High-level algorithm

When the game chooses a monster for a normal room spawn (and also for "guardian" spawns that protect valuable rooms), it:

1. Starts from `effectiveSpawnTable(category, depth)` (defaults + overrides).
2. Applies **room bias** (`applyRoomBias`): libraries skew toward wizards, lairs skew toward wolves, vaults skew toward mimics/thieves, etc.
3. Applies **material bias** (`applyMaterialBias`): mossy floors skew toward slimes/bats, marble skews toward undead, metal skews toward kobolds/guards, etc.
4. Computes an **`EcoCtx`** for the exact spawn tile:
   - `here`: ecosystem at the tile
   - `diversity`: distinct non-`None` ecosystems in `{here + 4-neighbors}`
   - `other`: the most frequent neighboring ecosystem different from `here`
   - `ecotone`: true when `diversity >= 2` and `other != None`
5. Applies **ecosystem bias**:
   - a strong pass for `eco.here`
   - if `eco.ecotone`: a weaker pass for `eco.other` (boundary blending)
6. If `eco.ecotone`: applies a small "liminal" bias toward tricksters/oddities.

Finally, it rolls a monster from the adjusted weight table.

**Important:** this is weight-only. It does not introduce additional RNG draws; it only changes the weights used in the existing roll.

## Ecosystem themes

Exact multipliers are intentionally mild and can be tuned in `src/game_spawn.cpp` (`applyEcosystemBias` / `applyEcotoneBias`). Conceptually:

- **FungalBloom:** more **Spiders/Slimes/Snakes**, fewer undead.
- **CrystalGarden:** more **Wizards/Mimics/Nymphs**, fewer snakes/spiders.
- **BoneField:** more **Skeleton Archers/Zombies/Ghosts**, fewer slime/spider/snake.
- **RustVeins:** more **Kobold Slingers/Orcs/Mimics**, fewer bats/slimes.
- **AshenRidge:** more **Orcs/Trolls/Ogres** (and sometimes wizards), fewer bats/slimes.
- **FloodedGrotto:** more **Bats/Slimes/Snakes**, fewer orcs/ogres.

The **Guardian** spawn category receives the same ecosystem bias, plus a couple tiny extra nudges (e.g., some deep "constructed" biomes slightly prefer **Guards**).

## Ecotones (biome boundaries)

An **ecotone** is detected when the tile and its cardinal neighbors contain **two or more** distinct non-`None` ecosystems.

At ecotones we do two things:

- **Blend:** apply a weaker bias from the dominant neighboring ecosystem so boundaries feel like transitional mixes.
- **Weirdness:** slightly bump "liminal" creatures (Mimic/Leprechaun/Nymph) and (past early depths) Wizards. If the border touches **BoneField**, undead get an extra nudge.

This makes borders feel unstable and strange rather than being hard walls.

## Biome-tuned nest clusters

The existing small "nest" logic (spiders/snakes/slimes/bats sometimes spawning 1–2 extra nearby) now adjusts its chance based on local ecosystem:

- **FungalBloom** increases spider/snake/slime clustering.
- **FloodedGrotto** increases bat clustering.
- **CrystalGarden** increases slime clustering.
- **BoneField** reduces bat/spider clusters.

Additionally, **BoneField** (and dressed stone) can rarely raise a small **zombie pack** (1–2 extra zombies).

See: `Game::spawnMonsters()` → `maybeSpawnEcologyCluster` in `src/game_spawn.cpp`.

## Modding notes

- Content overrides still work: we take `effectiveSpawnTable(...)` (defaults + overrides) first, then apply multiplicative ecosystem nudges.
- If you remove a monster from a depth table entirely, ecosystem bias cannot reintroduce it (`mulWeight` only affects existing entries).
