# Procedural win conditions

This project traditionally followed a classic NetHack-style victory arc: **retrieve the Amulet of Yendor** from the deepest quest floor, then **return to camp and take the exit**.

To make runs feel more distinct without requiring new UI/menus or save-format changes, the game generates a **run-seeded Victory Plan** at the start of every run.

## How it works

The Victory Plan is generated deterministically from your run seed (and whether *Infinite World* is enabled). It does not consume the main RNG stream, so it is stable across reloads and remains deterministic.

The plan always includes:

1. A **depth requirement**: reach at least a target depth.
2. One or two **key requirements**: bring back an object/value or maintain a conduct.

You win by returning to **camp** (depth 0) and using the **exit stairs up** (`<`) while the depth requirement and all key requirements are satisfied.

## Key templates

Every run chooses a primary key requirement. Some runs also roll a second clause, shown as `ALSO:` in the GOAL lines. When a second clause exists, **both clauses must be satisfied**.

Possible key requirements:

### Classic

- **Amulet**: escape with the **Amulet of Yendor** (classic win).

### Wealth and trade

- **Gold haul**: escape with at least **N gold**.
- **Debt-free**: escape while owing **0** shop debt.

### Essence

- **Essence ritual**: escape with **N Essence Shards** of a specific tag (optionally requiring a minimum tier).

### Trophy hunting

- **Hide trophies**: escape with **N** butchered hides at or above a quality tier.
- **Bone trophies**: escape with **N** butchered bones at or above a quality tier.
- **Fish trophies**: escape with **N** trophy fish at or above a rarity tier. Some runs also require a specific fish bonus tag (e.g. **EMBER**, **VENOM**).
  - Fish are obtained via the **Fishing Rod** (camp stash) by fishing at **fountains** and the **camp basins**.

Quality tiers are summarized as:

- **ANY**, **TOUGH+**, **FINE+**, **PRIME+**

### Conduct challenges

- **Pacifist**: escape with **0 direct (player) killing blows**.
- **Foodless**: escape without eating anything.
- **Vegetarian**: escape without eating corpses (food rations allowed).
- **Atheist**: escape without using shrine services (prayers).
- **Illiterate**: escape without reading scrolls or spellbooks.

## UI

- At the start of a run, the message log prints the generated plan as a set of **GOAL:** lines (including **ALSO:** lines if present).
- The HUD shows a compact **WIN:** tag with your current progress for each clause.
- The **Stats** overlay also lists the win conditions and progress.

## Notes

- The generator is intentionally conservative: essence goals only pick from a small bank of "core" tags that are widely obtainable.
- Dual-clause plans and hard conducts clamp the target depth to keep runs fair.
- All checks are computed from live counters (kills, prayers, food eaten) and inventory (gold, shards, trophies), so no new save fields are needed.
