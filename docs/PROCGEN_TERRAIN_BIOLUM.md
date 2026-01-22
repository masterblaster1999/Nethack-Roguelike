# Procedural Bioluminescent Terrain (Lichen / Crystal Glow)

This project already has a **darkness mode** (deeper floors) with real light propagation.
This feature adds a deterministic, purely-procedural "biolum" field that produces:

- **Patches of dim green lichen** (moss/dirt bias)
- **Cold blue/purple crystal glints** (crystal/metal bias)

The field is computed **once per floor** (cached), and then used to spawn a small set of
colored light sources so the dungeon gains subtle, organic navigation landmarks without
replacing torches.

---

## Overview

We compute a per-tile intensity cache:

- `Dungeon::biolumCache` (uint8 per tile, `0..255`)

It is generated inside `Dungeon::ensureMaterials()` alongside the substrate-material cache.
Because it is deterministic and derived from the same world/floor keys, it is:

- stable for a given run seed + floor
- not saved to disk (recomputed on demand)

---

## How it is generated

### 1) Active mask

Only *floor* tiles are considered "active" (eligible for biolum). Walls/chasm tiles are
pinned to a stable state so patterns do not smear through solid rock.

### 2) Seed droplets

The simulation is initialized with small "B" droplets, biased by substrate material:

- Moss / Dirt: more likely to seed in cavern-themed floors
- Crystal / Metal: more likely to seed in mines-themed floors
- Bone: rare eerie seeds in tomb-ish / labyrinth-ish themes

Depth increases seeding slightly.

### 3) Gray-Scott reaction-diffusion

We run a short Gray-Scott reaction-diffusion solve over the floor mask. Different floor
themes select different presets (feed/kill), producing different organic motifs.

### 4) Intensity mapping

We normalize the final `B` concentration and map it into `0..maxI` (depth-scaled), then
apply a material-specific scale so crystal peaks tend to be brighter than moss.

---

## How it is rendered / used

### Darkness lighting

When `darknessActive()` is true, the game collects the strongest biolum tiles and
selects a small number of them with a greedy "keep-apart" spacing rule.

These selected points are injected as normal light sources:

- small radius (2–5)
- modest intensity
- tinted color based on material (moss-green, crystal-cyan/violet, etc.)

### Ambient particles

In darkness, strong biolum tiles also emit occasional **motes/spores** (visual-only)
whose colors match the underlying material tint.

---

## Debugging

Use `#mapstats` and look for the line:

- `BIOLUM <tiles> | STRONG <tiles> | CRYSTAL <tiles> | MOSS <tiles>`

This is useful when tuning presets.

---

## Files

- `src/proc_rd.hpp` — shared Gray-Scott solver (also used by chemical spills)
- `src/dungeon.cpp/.hpp` — biolum cache generation + accessors
- `src/game_world.cpp` — injects biolum sources into the light map in darkness
- `src/render.cpp` — emits visual biolum motes for strong biolum tiles
- `src/game_internal.hpp` — `#mapstats` output
