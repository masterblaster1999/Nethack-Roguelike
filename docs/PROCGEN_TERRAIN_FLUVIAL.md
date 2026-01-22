# Procedural Terrain: Fluvial Gullies (Drainage Network)

This procgen pass adds **narrow erosion trenches** that read like dried subterranean streams.

Unlike the global ravine / endless rift / run faultline systems (which create big, map-spanning seams), **fluvial gullies** are meant to be **local**, often **branchy**, and biased by the same deterministic **macro heightfield** used by the ridge/scree terrain pass.

## High-level idea

1. Sample the same macro heightfield used by the heightfield terrain pass.
2. Build a lightweight **D8 flow direction** field over *passable* tiles.
3. Compute a simple **flow accumulation** value per tile ("how many tiles drain through here").
4. Pick 1–3 high-elevation/high-slope "source" points.
5. Trace each source downhill and carve **Chasm** tiles where the accumulation becomes non-trivial (streams deepen downstream).
6. If the new chasms sever the stairs-to-stairs path, automatically place **causeways/bridges** (or rollback the gully).

The result is terrain that feels like "erosion" rather than a deliberate tectonic seam.

## Safety rules

The pass is intentionally conservative:

- **Never touches stairs**.
- **Never destroys doors**.
- Reserves a **halo around the shortest stairs path**, keeping at least one clean navigation "spine".
- Avoids **special rooms** (shops, treasure rooms, shrines, etc.).
- Always validates **stairs connectivity**; if it cannot be repaired, the entire gully is rolled back.

## Tuning knobs

The implementation uses a few deterministic controls:

- A per-depth probability gate (modulated by generator kind).
- A per-floor budget that limits total chasm carved by gullies.
- A minimum accumulation threshold before carving begins.
- Optional widening based on accumulation (thicker downstream).

## Debug

Use `#mapstats` to see:

- `TERRAIN FLUVIAL | GULLIES <n> | CHASM <tiles> | CAUSEWAYS <n>`

