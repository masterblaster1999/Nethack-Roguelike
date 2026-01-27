# Procedural Overworld Gates (Boundary-Shared Roads)

The overworld is built out of **infinite wilderness chunks**. Each chunk is a normal dungeon grid with
solid border walls, and the only way to travel to a neighboring chunk is through an **edge gate**.

Early versions used **fixed mid-edge gates** (always centered on each edge). That makes travel easy,
but it also makes every chunk feel structurally identical.

This system upgrades the overworld to use **boundary-shared gate offsets**:

- Each *chunk boundary* (the line between two adjacent chunks) deterministically chooses a gate offset.
- Both chunks on either side of that boundary use the *same* gate offset.
- Result: the trail network can form **continuous cross-chunk roads** without seams.

Home camp adjacency is special-cased so the camp stays readable and stable.


## Design goals

1. **Seamless roads**
   - If chunk `(x, y)` has an east gate at `y = k`, then chunk `(x+1, y)` must have a west gate at `y = k`.
   - If chunk `(x, y)` has a south gate at `x = k`, then chunk `(x, y+1)` must have a north gate at `x = k`.

2. **Determinism**
   - A run seed and chunk coordinates uniquely determine all gate positions.

3. **Readable traversal**
   - Gates are kept away from corners so the “throat” tile carved inward is always in-bounds and visually clear.

4. **Preserve home camp**
   - Boundaries touching `(0,0)` are pinned to mid-edge so the home camp’s layout doesn’t get cut open by shifting gates.


## Boundary keys

Instead of “gate belongs to chunk”, think “gate belongs to boundary”:

- **Vertical boundary (V)**: between `(bx, y)` and `(bx+1, y)`
  - Stores a shared **Y** coordinate for the gate.
- **Horizontal boundary (H)**: between `(x, by)` and `(x, by+1)`
  - Stores a shared **X** coordinate for the gate.

For a chunk `(cx, cy)`:

- West edge uses the vertical boundary `(cx-1, cy)`.
- East edge uses the vertical boundary `(cx, cy)`.
- North edge uses the horizontal boundary `(cx, cy-1)`.
- South edge uses the horizontal boundary `(cx, cy)`.


## Deterministic offset generation

Offsets are generated from a hash of:

- `runSeed`
- A direction tag (`OW_GATE_V` or `OW_GATE_H`)
- The boundary coordinate (`bx, y` or `x, by`)

Then the result is mapped into a safe range:

- Horizontal gates: `x ∈ [2, width-3]`
- Vertical gates: `y ∈ [2, height-3]`

(If a map is ever extremely tiny, the range collapses safely to the nearest possible value.)


## Integration points

### 1) Gate carving
`ensureBorderGates(d, runSeed, chunkX, chunkY)`:

- Carves the 4 border gate tiles.
- Carves a 1-tile “throat” inward for readability.
- Stores a simple gate mask + positions on the dungeon instance.

### 2) Trail network
`generateWildernessChunk(...)` queries the gate locations *before* carving trails so the trail hub connects
to the correct gate throats.

### 3) Travel arrival placement
When stepping into a destination chunk, arrival is computed using the destination chunk’s gate positions:

- Move east → arrive at **destination west gate**
- Move west → arrive at **destination east gate**
- Move south → arrive at **destination north gate**
- Move north → arrive at **destination south gate**

Because gates are boundary-shared, this keeps the macro road network aligned across chunk borders.


## Why this matters

This is one of those “small change, big feel” upgrades:

- Roads stop being perfectly centered every time.
- Chunk structure becomes more varied.
- Cross-chunk travel feels like you’re following a real network of trails rather than a tiled set of identical rooms.
