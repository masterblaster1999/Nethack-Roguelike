# Procgen Setpiece Item Hooks

This project already supported generator-driven **bonus chests** via `Dungeon::bonusLootSpots` (consumed by `Game::spawnItems()` to place chests, and by `spawnTraps()` to sometimes guard them).

Round 89 adds a second, more general hook: **procgen-requested ground items**.

## `bonusItemSpawns`

Dungeon generation can now request specific, guaranteed item drops as part of setpieces (prefabs, dead-end stashes, special passes).

- Type: `std::vector<BonusItemSpawn> Dungeon::bonusItemSpawns`
- Each request contains:
  - `pos` — the tile to spawn the item on
  - `kind` — `ItemKind` to spawn
  - `count` — stack count (minimum 1)

This is a **generation-time hint** only; it is not serialized. Once the items are spawned onto the ground, the requests no longer matter.

## How spawning works

`Game::spawnItems()` now applies `bonusItemSpawns` **after** generic room loot rolls, so:

- We can avoid collisions with already-spawned items.
- Keys/lockpicks requested by setpieces count toward the existing “guarantee a key/lockpick when locked doors exist” safety net.

Spawns are skipped if:

- The requested position is out of bounds.
- The tile isn’t a floor tile.
- There is an entity on the tile.
- There is already a ground item on the tile.

## Vault prefab glyphs

The vault-prefab system supports a small “glyph language”. Round 89 extends it with:

- `T` — request a **bonus chest** (`bonusLootSpots`)
- `K` — request a **key** (`bonusItemSpawns`)
- `R` — request a **lockpick** (`bonusItemSpawns`)

These glyphs let prefabs express small, deterministic micro-puzzles (e.g., a reachable key that unlocks an inner door).

## Extending it

- New prefabs can add additional glyphs if desired.
- Non-prefab procgen passes can directly push `BonusItemSpawn` requests for deterministic rewards in procedurally carved spaces.
