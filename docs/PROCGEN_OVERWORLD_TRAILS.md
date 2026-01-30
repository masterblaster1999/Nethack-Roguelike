# Procedural Overworld Trails

This document describes how wilderness chunks (Camp branch, depth 0) generate **trails** that connect all **border gates** back into a readable, navigable network.

## Goals

- **Connectivity:** every gate must be able to reach every other gate (and thus the wider overworld).
- **Readability:** trails should look like a coherent network, not random “carved bands”.
- **Terrain respect:** prefer going *around* rivers/mountains and climbing gently instead of tunneling straight through everything.
- **Determinism:** chunk generation must be stable for the same `(runSeed, chunkX, chunkY)`.

## High-level flow

1. Generate macro terrain:
   - heightfields (mountains/ridges)
   - rivers/chasm ribbons
   - local landmarks (ruins, groves, clearings, etc.)
2. Pick a **hub** near the center of the chunk and carve a small clearing.
3. Connect each gate’s “throat” tile (1 tile inside the boundary) to the hub using a **direction-aware A\*** planner.
4. Optionally connect a **waystation** back into the hub using the same trail planner.
5. Finally, enforce border walls and carve the final gate tiles.

## Direction-aware A* trail planner

Instead of a greedy random walk, trails use **A\*** pathfinding on a terrain-cost grid.

### Cost field

Each step has a base cost and adds penalties:

- **Tile type (current terrain):**
  - `Floor` (and door variants): cheap, preferred (also makes trails naturally “merge”)
  - `Pillar` / `Boulder`: moderate (rough ground)
  - `Chasm`: expensive (treated as river/ravine)
  - `Wall`: very expensive (mountains/ruins walls) — still allowed so paths always exist

- **Slope penalty (heightfield):**
  - local elevation gradient increases cost, discouraging steep climbs

- **Wetness penalty:**
  - the wettest tiles become slightly more expensive outside swamp biomes

- **Deterministic micro-jitter:**
  - a tiny, per-tile cost jitter breaks ties so the result isn’t perfectly mirrored

### Heading / turn cost

The A\* state includes **direction of travel** (4 headings). Turning adds extra cost:

- small penalty for a turn (encourages longer straight segments)
- larger penalty for a U-turn (prevents “wiggle” oscillations)

This produces smoother “road-like” trails without needing a separate smoothing pass.

## Terrain-preserving carving

Trails are carved with a radius (wider in plains/coasts/swamps), but widening is **terrain-aware**:

- the **centerline** tile is always carved to `Floor`
- widening tiles (the radius ring) skip `Wall` and `Chasm` unless explicitly forced (only used for the hub clearing)

This keeps rivers/mountains visually intact and prevents the trail system from flattening them into broad corridors.

## Debug & tests

The unit tests include a small regression that ensures the four gate throat tiles are mutually reachable inside a generated wilderness chunk.
