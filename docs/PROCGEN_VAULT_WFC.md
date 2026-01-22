# Procedural Vaults: WFC Ruin Pockets

The dungeon generator includes a small library of **hand-authored vault prefabs** plus a **fully procedural perfect-maze micro-vault**.

This project also ships a lightweight **Wave Function Collapse (WFC)** adjacency solver (`src/wfc.hpp`) that is already used elsewhere for furnishing.

This document describes an additional micro-vault generator added in **Round 114**:

- **WFC Ruin Pocket vaults**: small, single-entrance wall pockets whose interior is synthesized by WFC, then validated for solvability.

These vaults are designed to be rare *discoveries* off a corridor—they never gate the stairs path.


## Where the code lives

- `src/dungeon.cpp`
  - `tryPlaceProceduralWfcVaultPrefab(...)`
  - `makeWfcVaultBase(...)`

The generator plugs into `maybePlaceVaultPrefabs(...)` alongside the existing maze-vault generator.


## Prefab tile alphabet

The WFC vault outputs the same prefab characters used by the existing vault system:

- `#` — solid wall (uncarved)
- `.` — floor
- `P` — pillar obstacle
- `C` — chasm hazard
- `+` / `s` / `L` — entrance door (open / secret / locked)
- `T` — treasure marker (becomes a guaranteed bonus loot spot)
- `K` — key item spawn hint
- `R` — lockpicks item spawn hint

> Note: `T`, `K`, and `R` are *generator hints*. They are not items placed directly during dungeon carving; the spawn systems use them later.


## Generation pipeline

### 1) Choose vault size and entrance type

- Baseline size is **9x7**.
- Deeper floors can grow to **11x9**.

Entrance type is selected by depth:

- `+` early (ordinary door)
- `s` increasingly common as depth increases (secret door)
- `L` occasional on deeper floors (locked door)

A single door is placed on the prefab boundary (never on a corner).


### 2) WFC: synthesize the interior

WFC runs on the **full** `w x h` grid, but with **fixed boundary conditions**:

- All boundary cells are forced to `#` (wall)
- The entrance cell is forced to the `Door` tile (rendered as `+`/`s`/`L`)

Interior cells start with a domain that includes:

- Wall
- Floor
- Pillar
- (Optional) Chasm, on deeper floors

The generator forces the **interior-adjacent cell behind the door** to be `.` (floor), and applies a small safety buffer near the entrance that suppresses chasms/pillars to keep the vault readable.

The rules are intentionally simple (adjacency masks + weights): the solver produces small, aperiodic ruins quickly, and we rely on post-validation to reject bad layouts.


### 3) Validate solvability and annotate

After WFC solves:

- Run BFS from the interior-adjacent entrance tile.
- Reject layouts that are too empty or too trivial.
- Choose the **farthest reachable** floor tile `.` and mark it as `T` (treasure).
- Optionally pick a reachable dead-end and mark it as `R` (lockpicks) or `K` (key).


### 4) Place the vault into the dungeon

Placement reuses the existing vault prefab placement logic:

- Rotate/mirror the synthesized grid to build variants.
- Find a candidate wall tile adjacent to exactly one corridor floor tile.
- Apply the prefab into solid wall, creating a single-entrance pocket.

If the entrance is locked (`L`), the generator drops a **key outside the door** (as a bonus item spawn hint) so the vault is always solvable.


## Tuning knobs

Inside `maybePlaceVaultPrefabs(...)`:

- `pProc` controls how often the generator attempts *any* procedural vault.
- `pWfc` controls how often the procedural attempt prefers the WFC style over the maze style.

Both parameters scale with depth and are reduced in organic cavern layouts.


## Design notes

- WFC vaults are attached to corridors and carved into wall mass, so they are always optional.
- The generator is allowed to fail and retry without harming the main floor generation.
- The goal is **variety**: even with the same outer dungeon layout algorithm, these small finds add distinctive micro-structure and reward exploration.
