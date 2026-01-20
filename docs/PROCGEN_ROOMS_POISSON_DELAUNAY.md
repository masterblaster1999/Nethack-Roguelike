# Procgen: Poisson–Delaunay “Ruins” RoomsGraph

This project has multiple floor generators. The **RoomsGraph** generator (informally “ruins”) is intended to feel *scattered / non-hierarchical* compared to BSP: rooms are distributed across the footprint and connected by a lightweight corridor graph rather than an implied tree.

Round 95 upgrades RoomsGraph to a classic **Poisson → Delaunay → MST (+ loops)** pipeline:

## 1) Poisson-disc room center placement (blue-noise)

We first generate candidate room centers using **Poisson-disc sampling** (Bridson-style):

- Keeps a **minimum separation radius** between centers.
- Produces a “blue-noise” distribution: *evenly spaced, not grid-like, not clumpy*.
- Uses an acceleration grid with cell size `r / sqrt(2)` so neighbor checks stay fast.

In practice:

- We sample inside the safe interior (`x/y in [3..w-4]`) and then stamp rectangles around those centers.
- If the sample produces too few sites for the current target room count, we **relax** the minimum distance slightly and resample.
- Each site gets multiple tries to fit a room rectangle without overlapping existing rooms (with a small jitter so things don’t look too “stamped”).

## 2) Delaunay triangulation adjacency graph

After rooms are placed, we build a **Delaunay triangulation** over the final room centers.

Why Delaunay?

- It yields a **sparse, planar-ish** neighbor graph.
- Edges tend to connect “natural” neighbors rather than creating lots of long-cross-floor edges.
- It’s a common foundation for room-graph dungeon generators.

Implementation notes:

- We use a small Bowyer–Watson triangulation (robust enough for the small room counts here).
- We extract unique undirected edges from the final triangle set.
- If triangulation yields a disconnected graph for any reason, we fall back to the previous **complete-graph** edge set.

## 3) Minimum Spanning Tree + extra loops

We then:

- Compute an **MST** over the edge set (Delaunay edges when possible). This guarantees global connectivity.
- Add a few extra edges as **loops**, so the floor isn’t a pure tree (supports flanking / escape routes).

## Debug / tuning

`#mapstats` now prints a **RUINSGEN** line on floors that used RoomsGraph:

- `POISSON`: number of sampled sites
- `PLACED`: rooms successfully stamped from those sites
- `DT`: number of Delaunay edges used as the candidate connection graph
- `LOOPS`: extra loops added beyond the MST

These stats are not saved/loaded; they’re for generation diagnostics.
