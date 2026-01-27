# Procedurally Generated Shops

This patch adds **procedurally generated shop identities**. Every shop room now has a
stable, deterministic *profile* derived from:

- run seed
- dungeon depth
- the shop room's rectangle (x/y/w/h)

The profile is computed on-demand and **not stored in save files**, so it does not change
save format compatibility.

## What is generated?

Each shop now has:

- **Shop name** (e.g. `THE GILDED ANVIL`, `THE ARCANE LENS`, ...)
- **Shopkeeper name** (procedural syllable-based name)
- **Shop theme**: General / Armory / Magic / Supplies
- **Shop temperament**: Greedy / Shrewd / Fair / Generous / Eccentric

## Gameplay effects

### Prices vary per shop

Shops no longer share identical economics.

- Temperament affects the overall **markup** (buy prices) and **offer rate** (sell prices).
- Theme adds a bias:
  - Armories are better for weapons/armor, worse for off-theme goods.
  - Magic shops are better for wands/scrolls/potions/spellbooks/rings/rune tablets.
  - Supply shops are better for food/tools/materials.

The "Eccentric" temperament adds a small deterministic per-item jitter, creating
occasional bargains and occasional rip-offs.

### Shop entry announcements

When the player steps (or teleports) into a shop, the message log announces:

- the shop's generated name
- a shopkeeper greeting (if a peaceful shopkeeper is present in that room)

### HUD hint

While the player is standing inside a shop, the HUD line shows `SHOP: <name>` so you can
quickly tell which store you're in.

## Implementation notes

- Implementation lives in `src/shop_profile_gen.hpp`.
- Everything is derived from hashing and does **not** consume the global RNG stream.
- Shop stock prices are adjusted at the moment shop items are created.
- When selling an item to a shop, the offer and the resell price are adjusted using the
  current shop profile.
