# Annex Procgen: WFC Pipe-Network Style

This project already uses WFC (Wave Function Collapse) for **room furnishing** (mirrored pillar/boulder patterns).
This document describes a *new* use of WFC for **topology generation**: an optional annex micro-dungeon style that
synthesizes an entire walkable layout from local connectivity constraints.

## What it is

Some annex wall-pocket micro-dungeons can now generate as a **WFC connectivity lattice** ("pipe network"):

- The annex interior is treated as an **odd-coordinate lattice** (classic 1-tile corridor grid).
- A coarse grid cell corresponds to one walkable node (at `(1 + 2*cx, 1 + 2*cy)` in the annex).
- Each coarse cell is assigned one **connectivity tile**:
  - `Empty` (solid)
  - dead-ends (N/E/S/W)
  - straights (horizontal / vertical)
  - corners (NE/ES/SW/WN)
  - T-junctions (3-way)
  - cross (4-way)

Each tile is represented as a 4-bit mask over {N,E,S,W}. Neighbor compatibility is a **Wang-tile-style edge rule**:

> If a tile opens to the east, its east neighbor *must* open to the west, and vice-versa.

This is solved using the project's existing `wfc::solve()` (entropy + constraint propagation + restarts).

## Post-processing / shaping

After WFC produces a solution:

1. **Keep only the entry-connected component** so the annex is always navigable from its door.
2. Enforce minimum quality constraints (size, some branching on mid+ depths, occasional cycle preference on deep floors).
3. Stamp the coarse connectivity graph into the full annex tile grid as 1-tile corridors.
4. "Inflate" some junction nodes into **tiny 3x3 chambers** for readability and loot staging.
5. Carve **small alcoves** beyond some dead ends.
6. Optionally place **0–2 internal door markers** on straight corridor tiles (treated as bulkheads). The caller converts
   marker tiles into actual door tiles when stamping the annex into the dungeon.

## When it shows up

The WFC annex is weighted to appear more often on structured floor types (e.g., `RoomsGraph`, `Catacombs`) and
less often on already-chaotic generators (e.g., `Cavern`, `Warrens`). Depth also increases junction density.

## Debugging

Run `#mapstats`:

- `ANNEXES N | WFC M` indicates how many annexes were carved and how many used the WFC style.
- `ANNEXES N | KEYGATES K` indicates how many annexes received the internal key-gate micro-puzzle.

