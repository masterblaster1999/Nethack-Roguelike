# Procedural Ecosystem Guardians (Apex Packs)

Round 203 adds **Ecosystem Guardians**: rare, biome-themed **apex packs** that spawn near the **core** of a procedural ecosystem seed.

The intent is to make ecosystem regions feel *owned* by something dangerous (and worth seeking out), without turning every floor into a boss rush.

## What it does

On most main-dungeon floors (depth ≥ 2, excluding the boss-end floors), the generator may select **0–2 ecosystem seeds** and place a **guardian pack**:

- **Leader**: a proc-ranked monster (**Elite / Champion / Mythic**)
  - Always has the **Gilded** affix (bonus gold)
  - Also gets a **signature affix** themed to the ecosystem (e.g. Webbing/Venomous for fungal cores)
  - Rolls **proc abilities** using the existing ecosystem-biased proc system
- **Escort**: 2–4 nearby minions, biased toward the leader kind but with small ecosystem variety

Guardians prefer seed **cores** (near the seed center), and the placement logic avoids:

- stairs (and the immediate stair "bubble")
- shops / camp tiles
- treasure/vault/secret rooms (to avoid turning reward rooms into unavoidable death traps)

## Rewards: proc-ranked Essence Shards

To make these encounters (and proc-ranked enemies in general) pay out in a **biome-relevant** way, Round 203 also adds a deterministic drop rule:

- **Champion/Mythic** monsters (proc tier ≥ 2) can drop an **Essence Shard** stack aligned to the **local ecosystem + substrate material**.
- This is **hash-derived** (not driven by the main RNG stream), so it doesn’t perturb other loot or event randomness.

This ties neatly into the existing ecosystem resource loop introduced in Round 200 (floor shard clusters) and makes biome cores a consistent "crafting fuel" hotspot.

## Determinism & RNG isolation

Guardian selection + placement uses a dedicated RNG stream derived from:

- run seed
- branch + depth
- ecosystem seed properties (kind + position)

This keeps the feature deterministic while also minimizing unexpected butterfly effects on other procedural systems.

## Implementation notes

- **Spawn:** `Game::spawnMonsters()` in `src/game_spawn.cpp`
  - Adds a late-pass that selects candidate ecosystem seeds and spawns the guardian pack.
- **Drops:** `Game::cleanupDead()` in `src/game_spawn.cpp`
  - Adds deterministic essence shard drops for proc-ranked enemies.

