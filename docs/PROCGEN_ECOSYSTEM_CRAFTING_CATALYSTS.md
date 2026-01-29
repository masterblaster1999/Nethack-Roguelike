# Ecosystem Crafting Catalysts

This project already treats **room types** (Armory, Library, Laboratory, Shrine, Camp) as deterministic *workstations* that shift Crafting Kit outcomes inside a run.

Round 206 adds a second environmental axis: the local **procedural ecosystem biome** now acts as a deterministic **catalyst** for crafting.

## What this means

When you combine two crafting ingredients (via **Crafting Kit** / `#craft`):

- The game computes a **run-stable craft seed**.
- That seed is now salted by:
  1. The current room’s **workstation** (if any).
  2. The current tile’s **EcosystemKind** catalyst (if any).

So the *same ingredients* can produce **different sigils / outputs** when crafted in different biomes — while remaining deterministic across save/load and replays.

The crafting preview panel now shows both:

- `WORKSTATION: ...`
- `CATALYST: ...`

And the Craft Recipe journal lines include:

- `@WORKSTATION/CATALYST`

## Biome catalyst effects

In addition to seed-salting (new outcomes), each ecosystem applies a small, deterministic “flavor nudge”:

- **Fungal Bloom**: can yield **extra Essence Shard byproduct**.
- **Crystal Garden**: can **increase wand charges** and sometimes **purify** a cursed wand.
- **Rust Veins**: can **temper** crafted gear (small chance to **increase enchant** / bless).
- **Bone Field**: can **forge enchantment**, but has a small **cursed risk**.
- **Ashen Ridge**: can **boost fire-wand charges** and lightly **temper gear**.
- **Flooded Grotto**: can **brew fuller potions** and sometimes **wash away curses**.

All nudges are deterministic (hash-based), so they do **not** perturb the global RNG stream.

## Implementation notes (for contributors)

Key changes live in:

- `src/game_inventory.cpp`
  - `Game::computeCraftComputed(...)` now reads the ecosystem at the player’s tile and salts crafting.
  - The preview UI and recipe journal also surface the catalyst.
- `src/game.hpp`
  - `CraftComputed` and `CraftRecipeEntry` now carry an `EcosystemKind ecosystem` field.

