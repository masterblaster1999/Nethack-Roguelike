# Procedural Terrain Color Palette (Hue / Saturation / Brightness)

The game can apply a **deterministic, seed-derived color grading** to procedural terrain (floors, walls, chasms, doors). This is *cosmetic only*:

* It does **not** touch gameplay RNG.
* It is stable within a run and changes in a controlled way by **branch/depth**.

## What the system does

1. **Run + depth palette tint**
   * The renderer derives a near-white **tint multiplier** from the run seed and depth, and multiplies it into the terrain draw color.
   * Room styles (Treasure/Lair/Shrine/...) receive small offsets so special rooms still read distinctly.

2. **Substrate material tint**
   * A deterministic terrain **material** map (STONE/BRICK/BASALT/...) further nudges hue and brightness so the same floor style can look like different stone.

3. **Spatial chroma field (new)**
   * A low-frequency noise field (seeded per floor) smoothly varies hue/saturation/brightness across the map.
   * This breaks up large uniform areas without looking like random per-tile flicker.
   * Doors are excluded for readability.

## In-game commands

The palette is controlled via the console:

* `#palette on|off|toggle`
* `#palette strength <0..100>`
* `#palette hue <deg -45..45>`
* `#palette sat <pct -80..80>`
* `#palette bright <pct -60..60>`
* `#palette spatial <0..100>`
* `#palette reset`

`#mapstats` prints the current palette configuration under the `PALETTE` line.

## settings.ini keys

All options can be set in `settings.ini`:

* `proc_palette = true|false`
* `proc_palette_strength = 0..100`
* `proc_palette_hue_deg = -45..45`
* `proc_palette_sat_pct = -80..80`
* `proc_palette_bright_pct = -60..60`
* `proc_palette_spatial = 0..100`

Notes:

* `proc_palette_strength` is the master knob; the other controls are scaled by it.
* The system applies adjustments in **HSV** for intuitive hue/saturation/brightness tuning.
