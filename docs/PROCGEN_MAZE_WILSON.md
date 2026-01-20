# Procgen: Wilson Mazes (Loop-Erased Random Walks)

This project’s maze floors use a **perfect maze** carved on an odd-cell lattice, then “braided” a bit by breaking some walls and carving a few chambers for landmarks.

In **Round 97**, the perfect-maze step gained an alternate algorithm: **Wilson’s algorithm** (loop-erased random walks), which generates a **uniform spanning tree** of the grid.

The goal is to make maze floors feel *meaningfully different* from the classic recursive backtracker while preserving all of the existing safety and gameplay constraints (stairs placement, chamber carving, LOS doors, etc.).

---

## Why a second maze algorithm

Recursive backtracker mazes tend to have:

- Longer straight-ish runs and a “depth-first” texture.
- Many short dead-ends branching off long corridors.

Wilson mazes tend to have:

- A more “evenly random” texture.
- Different corridor run-length statistics and a less DFS-looking structure.

Because the project already adds extra loops and chambers after the perfect maze step, the *local* gameplay beats remain familiar, but the *global* corridor feel changes.

---

## High-level algorithm

We work on a cell grid:

- `cellW = (mapW - 1) / 2`
- `cellH = (mapH - 1) / 2`

Each cell `(cx, cy)` maps to an in-map position `(x, y) = (1 + 2*cx, 1 + 2*cy)`.

Wilson’s algorithm builds a spanning tree like this:

1. Pick a root cell and add it to the tree.
2. For each cell not in the tree:
   - Start a random walk from that cell.
   - As you walk, **erase loops** (if you revisit a cell already in the current walk path, remove the cycle).
   - Stop when the walk hits the existing tree.
   - Add the loop-erased path to the tree.

Carving is trivial because edges are between adjacent cells:

- Carve the two cell centers as `Floor`.
- Carve the “wall” tile between them as `Floor`.

---

## Implementation details

The implementation uses:

- `inTree[cell]` – whether a cell is already part of the spanning tree.
- `path` – the current random walk path (vector of cell indices).
- `posInPath[cell]` – index of a cell in `path` (or `-1`) so loop erasure is `O(k)` on the erased segment.

After the perfect maze is carved:

- A number of random **wall breaks** add loops so the maze is not a strict tree.
- A central **start chamber** is carved on an existing corridor.
- Several **extra chambers** are carved as landmarks.
- Stairs are placed using BFS: `stairsDown` is the farthest reachable tile from `stairsUp`.
- Strategic corridor doors are placed to create LOS breakpoints.

---

## Debug and telemetry

Maze floors now expose non-serialized telemetry on `Dungeon`:

- `mazeAlgorithm` (`BACKTRACKER` or `WILSON`)
- `mazeChamberCount`
- `mazeBreakCount`

When `mazeAlgorithm == WILSON`, additional stats are tracked:

- `mazeWilsonWalkCount` – number of walks committed (how many times we started from an unvisited region).
- `mazeWilsonStepCount` – total random-walk steps.
- `mazeWilsonLoopEraseCount` – total cells removed due to loop erasure.
- `mazeWilsonMaxPathLen` – longest single loop-erased path length.

In-game, `#mapstats` prints a line like:

`MAZEGEN | WILSON | CHAMBERS 7 | BREAKS 14 | WALKS 53 | STEPS 4112 | ERASED 102 | MAXPATH 27`

---

## Player-facing hint

When a floor uses Wilson’s algorithm, the game gives a subtle arrival hint:

> “THE PASSAGES TWIST WITH AN UNCANNY, EVEN CHANCE.”
