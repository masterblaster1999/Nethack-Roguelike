# Procedural Overworld Atlas (World Map)

This patch adds an **Overworld Atlas** overlay that visualizes the run-seeded, procedurally generated wilderness “chunk” grid.

The atlas is intentionally **lightweight and deterministic**:
- Chunks are labeled by **biome** and a generated **region name** (`overworld::profileFor`).
- Weather shown is the chunk’s deterministic **weather profile** (`overworld::weatherFor`).
- The atlas only reveals chunks you have **actually visited** (tracked in-memory per run).

> Note: overworld chunk snapshots are still treated as **ephemeral** (they can be evicted from memory and are not serialized yet). The atlas visitation record is also **UI-only / not saved**.

---

## Controls

Default keybind:
- **Shift+M** — Toggle the Overworld Atlas

While the atlas is open:
- **Move** (arrow keys / roguelike movement) — pan the cursor around the chunk grid
- **Enter** — recenter cursor on your current chunk
- **+ / -** (minimap zoom bindings) — zoom the atlas view (changes how many chunks are visible)
- **Esc** — close the atlas

---

## Map Legend

Each chunk is rendered as **two characters**:

- First character: **Biome letter**
  - `P` Plains
  - `F` Forest
  - `S` Swamp
  - `D` Desert
  - `T` Tundra
	  - `H` Highlands
  - `B` Badlands
	  - `C` Coast

- Second character: **marker**
  - `@` your current chunk
  - `*` home camp chunk (0,0)
  - `$` a discovered chunk with a **Shop** room snapshot loaded (used by overworld waystations)
  - `.` normal discovered chunk
  - `?` unknown / not yet visited

---

## Implementation Notes

### Discovery tracking (UI-only)
The game tracks visited overworld chunks in a small in-memory set:
- `Game::markOverworldDiscovered(x, y)` is called when you step between overworld chunks.
- The atlas uses `Game::overworldChunkDiscovered(x, y)` to decide whether a chunk should display its biome/name/weather.

A safety cap is applied to avoid unbounded memory growth during extreme long-distance travel.

### “Loaded” vs “not in memory”
The atlas can show visited chunks even if their full simulation snapshot was evicted from memory. For some details (like whether the chunk contains a shop room), the atlas needs an in-memory snapshot:

- `Game::overworldChunkDungeon(x, y)` returns a `Dungeon*` snapshot when available.
- If no snapshot is available, the details panel shows `STATE: NOT IN MEMORY`.

---

## Future Extensions (not implemented here)
Some natural next steps (intentionally deferred):
- Persistent serialization of overworld chunks + atlas discovery.
- Fast-travel / multi-chunk auto-route plotting.
- Explicit landmark iconography (ruins, shrines, caves, etc.) beyond waystations.
