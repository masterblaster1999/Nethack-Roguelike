# Procedural Ecosystem Resource Nodes

Round 208 adds **procedural, harvestable “resource nodes”** that spawn inside ecosystem regions (Fungal Bloom, Crystal Garden, etc.). These are **stationary ground props** (not pick‑up items) that you interact with by standing on them and pressing **CONFIRM**.

The design goal is to make biome regions feel like *resource-rich but risky* pockets that create interesting micro-decisions:
- Do you harvest now and risk a backlash (gas, embers, corrosion)?
- Or do you come back later, lure monsters into the hazard, or return with better gear?

---

## What counts as a node?

Nodes are implemented as new `ItemKind` values (append-only, save-safe):

- **Spore Pod** (Fungal Bloom)
- **Crystal Node** (Crystal Garden)
- **Bone Pile** (Bone Field)
- **Rust Vent** (Rust Veins)
- **Ash Vent** (Ashen Ridge)
- **Grotto Spring** (Flooded Grotto)

Each node has a small number of remaining harvests stored in `Item::charges` and is marked `ITEM_FLAG_STATIONARY`.

---

## Spawn rules

Nodes are spawned in `Game::spawnItems()` after ecosystem seeds are computed:

- Only on dungeon floors (not overworld; not camp).
- Only inside a seed’s ecosystem region (`dung.ecosystemAtCached(...) == seed.kind`).
- Only on **Floor** tiles, not shops, and not within 2 tiles of stairs.
- Prefers “clean” tiles (no stacked loot).

A **derived RNG** is used to place nodes so the node system does not perturb the existing loot RNG stream.

---

## Harvest interaction

Standing on a node and pressing **CONFIRM** triggers:

1. A **biome-appropriate backlash**:
   - *Spore Pod*: confusion gas + mild confusion
   - *Crystal Node*: loud “sing” + short shield buff
   - *Bone Pile*: mild amnesia shock
   - *Rust Vent*: corrosive gas + mild corrosion
   - *Ash Vent*: fire field + mild burn
   - *Grotto Spring*: heals/cleanses + clears nearby gas

2. A guaranteed **Essence Shard** reward:
   - Tag is chosen from a small tag-pair themed per node type (with light substrate bias).
   - Tier scales gently with depth; Crystal Nodes bias slightly higher.
   - Crystal Nodes have a higher “shiny” chance.

3. A small, rare **bonus item** themed per node (e.g. Antidote/Clarity from spores, Scroll Identify from crystals, etc.).

Nodes lose one charge per harvest and vanish when empty.

---

## Notes / future hooks

This system intentionally uses **items-as-props** so it can:
- reuse existing ground-item rendering and “look” UI,
- avoid creating new tile types,
- remain save-safe by append-only enums + per-item stationary flags.

Potential future extensions:
- Node-specific monster behaviors (guarding, feeding, nesting).
- Tool-based “safe harvesting” (masks, tongs, water skins, etc.).
- Rare multi-tile “mega nodes” that act like mini-vault objectives.
