# Procedural 2D Sprite Upgrades: Hue-Shifted Ramps + Cloth Panels

This project uses a **procedural sprite generator** (`src/spritegen.cpp`) to create
many monsters and items at runtime in a consistent pixel-art style.

This document describes two quality-focused upgrades to the **2D sprite pipeline**:

1. **Hue-shifted 4-tone shading ramp** (`rampShade`) for saturated colors.
2. **Humanoid cloth panel pass** that adds deterministic costume variation while
   preserving the underlying shading.

These changes intentionally keep the generator **deterministic** (seed + kind + frame)
so sprites remain stable across saves and replays.


## 1) Hue-shifted 4-tone ramp (`rampShade`)

Many procedural sprites are shaded using a compact, **4-tone ramp** and ordered
dithering. A pure-RGB ramp often makes saturated colors look muddy when darkened
or highlighted.

`rampShade` now:

* Converts the base color to **HSV**.
* Creates a 4-tone ramp by scaling **V** (value/brightness) while doing small,
  saturation-scaled hue shifts:
  * shadows drift gently toward **blue**
  * highlights drift gently toward **yellow**
* Uses ordered dithering (`bayer4Threshold`) to stay crisp at small resolutions.

Because hue shifting is scaled by **saturation**, near-neutral greys remain very
stable and do not unexpectedly "tint".


## 2) Humanoid clothing & pattern pass

Humanoid silhouettes can look overly uniform if they are entirely body-colored.
To add character (without authored sprite sheets), we apply a subtle,
deterministic **cloth panel** pass:

* Applies only to humanoid-like entity kinds (player, goblinoids, guard,
  shopkeeper, etc.).
* Picks a cloth hue from a kind-biased palette, with small per-seed variation.
* Recolors a torso rectangle by **replacing hue/saturation** while preserving the
  pixel's original **value** (so lighting stays intact).
* Adds a tiny 1-bit pattern (stripes/checker/diagonal) by modulating V/S.
* Occasionally adds a small **cape column** on one side for asymmetry.


## 3) Mask connectivity cleanup (8x8 proto-silhouette)

After cellular-automata smoothing, the 8x8 silhouette mask may still contain
isolated "islands" or tiny holes.

The generator now:

* Keeps the **connected component** that contains the most template-locked pixels
  (then the most area).
* Removes stray islands.
* Fills tiny 1-cell cavities.
* Re-applies exact mirror symmetry.

This improves readability in 2D and also reduces stray voxels when the sprite is
fed into the 3D extruder.
