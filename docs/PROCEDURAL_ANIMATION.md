# Procedural Animation Generation

This project leans heavily on **procedural sprite generation** (build a tiny 16x16 or 16x8 “design-grid” sprite deterministically from a seed) and then **procedural animation** (generate a *looping* flipbook from the same seed).

The goal is to get “alive” visuals (fire flicker, gas swirl, void motion, UI shimmer, etc.) **without shipping hand-authored sprite sheets**.

## The 4-frame flipbook contract

Most animated textures are generated as a small flipbook (currently **4 frames**). The renderer advances frames over time and may blend between consecutive frames for smoothness.

Because flipbooks loop, we want **frame 3 → frame 0** to wrap cleanly.

## Seamless loops via circular domain drift

A simple trick to make procedural noise loop is to **move the sampling point around a circle** in the noise domain.

Instead of using a linear time `t` (which causes a discontinuity when the flipbook wraps), we compute an angle:

- `ang = 2π * (frame / 4)`
- `offset = (cos(ang), sin(ang)) * radius`

Sampling noise at `(x + offset.x, y + offset.y)` guarantees the pattern repeats exactly every full revolution.

In code this lives in `src/spritegen.cpp` as small helpers (e.g. `phaseAngle_4`, `loopValueNoise2D01`, `loopFbm2D01`).

## Domain warp + fBm

Wispy effects like **confusion gas** are generated with a classic “domain-warped fBm” approach:

1. Sample two independent fBm fields `w1`, `w2`
2. Use them as a displacement `(wx, wy)`
3. Sample a main density field at the displaced coordinates

This yields swirling, smoke-like motion without simulating fluids.

## Curl-noise flow warping

Domain-warped fBm looks “smoky,” but it can still read like the texture is
sliding around.

To push effects like **gas** and **fire** closer to “advected” motion (as if the
pattern is being carried by a fluid), we add a lightweight **curl-noise** flow
warp:

1. Build a looped scalar field `n(x,y,frame)` using the seamless 4-frame noise helpers.
2. Approximate its derivatives and take the **2D curl**:
   - `v = (dn/dy, -dn/dx)` (a divergence-free velocity field)
3. Move the sampling point `(x,y)` along this field with a few short Euler steps.
4. Sample the usual domain-warped density at the warped point.

Because the underlying scalar field is looped, the resulting flow warp is also
looped and still respects the 4-frame flipbook contract.

Implementation notes:

- `src/spritegen.cpp`: `flowVelocity(...)` + `flowWarp2D(...)`
- Used by: `generateConfusionGasTile`, `generateIsometricGasTile`,
  `generateFireTile`, `generateIsometricFireTile`

## Sprite-space warping for entity “life”

Some entities get an extra procedural post-process that warps sprite pixels by ±1–2 px.
Because this is done in **sprite space** (after the base 16x16 art is generated), it works for *all* seeded variants without needing authored animations.

Key techniques used in `src/spritegen.cpp`:

- **Row-wave warp** (`warpSpriteRowWave`)
  - Shifts each sprite *row* by a tiny, phase-driven amount.
  - Used for ethereal drift (e.g., **ghosts**) and subtle shimmer.

- **Column-wave warp** (`warpSpriteColumnWave`)
  - Shifts each sprite *column* up/down, producing a slither / ripple motion.
  - Used for creatures like **snakes**.

- **Side-wave warp** (`warpSpriteSideWave`)
  - Keeps the center mass stable while pushing the left/right edges in/out.
  - Great for **bat wing flaps** and **spider leg scuttles**.

- **Squash & stretch** (`warpSpriteScaleNearest`)
  - Performs nearest-neighbor scaling around an anchor point (typically bottom-center) using inverse mapping.
  - Used for soft-bodied creatures (e.g., **slimes**) so they "breathe" / pulse without blurring the pixel art.

All of these are driven by the same 4-frame looping phase (cos/sin), so the animation wraps cleanly from frame 3 back to frame 0.

## UI animation

UI panels / ornaments use the same principles:

- Coherent looping grain (instead of per-frame random flicker)
- Small traveling “glints” and pulsing accents that loop cleanly


## Cursor / targeting reticles: marching ants

LOOK and targeting overlays now draw a **procedurally generated animated reticle texture** instead of raw SDL line primitives.

Why generate it procedurally?

- It keeps the cursor *readable* even when users zoom to very large tile sizes.
- We can animate it with the same 4-frame contract as the rest of the renderer.
- We get both a top-down **square** reticle and an isometric **diamond** reticle from the same generator.

Technique:

- Parameterize the reticle perimeter into a 1D list of pixels.
  - Top-down: walk the rectangle border.
  - Isometric: rasterize the four diamond edges (Bresenham) and concatenate them into a loop.
- Produce “marching ants” by applying a dashed pattern along that 1D index and **shifting the phase each frame**:
  - `on = ((i + phase) % period) < duty`
- Choose `period` divisible by 4 so the dash phase completes a full cycle over the 4-frame flipbook (seamless wrap).

Implementation notes:

- `src/spritegen.cpp`: `generateCursorReticleTile` (outputs a transparent RGBA sprite at `pxSize x pxSize`).
- `src/render.cpp`: caches the 4 reticle frames (`cursorReticleTex` / `cursorReticleIsoTex`) and uses them in
  `drawLookOverlay` and `drawTargetingOverlay`.

## Reaction-diffusion rune fields (Arcane UI + altars)

To get rune patterns that read less like random dots and more like *living circuitry*, we added a tiny **Gray–Scott reaction–diffusion** solver on the 16×16 design grid (see `makeRDSigilField` in `src/spritegen.cpp`).

Instead of advancing the simulation over time (which would not loop cleanly), we:

- Generate a deterministic base field from the seed (a handful of seeded “ink drops” + several iterations).
- Animate by **drifting the sampling point around a circle** (same seamless-loop trick used for noise).
- Optionally apply a small **curl-noise flow warp** to the sampling coordinates so the pattern feels advected.

We then derive “rune lines” from the field using a simple gradient magnitude (`rdGradMag`) + `smoothstep` thresholding, and blend the result as a faint glow.

Used by:

- `generateUIPanelTile` for `UITheme::Arcane` (animated rune circuitry behind panels)
- `generateAltarTile` (subtle etched rune glow on the stone base)

## Fountain ripples: coherent loop + sparkle

Fountain water was updated to avoid per-frame hash flicker and instead uses:

- Superposed traveling waves driven by the 4-frame phase
- Looped fBm for fine detail
- A looped, high-threshold noise sparkle pass
- A gentle flow warp so ripples feel slightly “advected”


## Procedural particle flipbooks

The renderer also owns a small **procedural particle engine** (hit sparks, projectile trails, explosion embers, ambient motes/smoke).

These particle textures are now generated as **4-frame flipbooks** too (same contract as terrain/UI), which gives much richer motion without shipping any authored sprites:

- **Spark**: slight rotation wobble + twinkle noise (additive)
- **Smoke**: looped domain-warped fBm “puff” (alpha blended)
- **Ember**: flickering hot dot with internal noise (additive)
- **Mote**: animated ring + twinkle noise (additive)

Particles sample frames using **particle-relative time** (so they animate across their lifetime) plus a stable **per-particle phase offset** to prevent large bursts from cycling in perfect lockstep.

Implementation notes:

- `src/render.cpp`: `ParticleEngine::{valueNoise2D01, fbm2D01, createTex, animFrameFor}`

## Procedural particle motion: curl-noise advection + deterministic wind

Once particle textures were animated as flipbooks, the remaining giveaway was motion: if smoke only drifts by a simple sinusoid, it can look like the whole puff is "sliding".

To make particles move more like a fluid, the renderer now advects **smoke, motes, and embers** through a lightweight **curl-noise** flow field:

- Build a time-varying scalar potential `n(x,y,t)` from fBm.
- Estimate its derivatives via finite differences.
- Rotate the gradient to obtain a divergence-free velocity field:
  - `v = (dn/dy, -dn/dx)`
- Apply that field as a small velocity nudge each simulation step.

On top of that, particles are biased by the game's **deterministic per-level wind** (`Game::windDir()` / `Game::windStrength()`), so effects like poison mist and fire embers feel like they're in the same "air" as the underlying gas/fire simulation.

Implementation notes:

- `src/render.cpp`: `ParticleEngine::curlNoise2D` + the `ParticleEngine::update(..., windAccel)` drift block.

## Procedural item micro-animations

Procedural animation isn't just for terrain and VFX — we also generate **4-frame item flipbooks** so loot feels alive even when the dungeon is static.

Key patterns:

- **Potions (appearance sprites)**: the liquid has a *sloshing surface* plus an *internal swirl* driven by **looped fBm** so the bottle contents move without harsh blinking. Special styles add extra character:
  - *metallic* potions get coherent shimmer flakes
  - *milky* potions get soft creamy highlight swirls
  - *murky* potions get drifting dark specks
  - *smoky* potions emit a small, looped smoke curl above the neck

- **Scrolls (appearance sprites)**: the rune label ink uses **looped value-noise shimmer**, and the curled edges subtly alternate shading to suggest flutter while keeping the outline stable.

- **Rings (appearance sprites + known ring sprites)**: band specular highlights and gem glints **orbit over the 4-frame loop**, reading like rotation rather than a simple on/off blink. Opals use a small **4-step iridescent palette cycle**.

- **Wands (appearance sprites)**: the tip ornament **pulses**, magical materials add an orbiting sparkle, and a small **energy crawl** highlights rune-notches in a (0,1,2,1) sequence that loops smoothly.

- **Torches (known sprites)**: lit torches use a tiny circular flicker offset and coherent ember/smoke specks so flames feel organic on the same 4-frame contract.

Implementation notes:

- `src/spritegen.cpp`: `drawPotionAppearance`, `drawScrollAppearance`, `drawRingAppearance`, `drawWandAppearance`, plus several item cases (e.g., `TorchLit`).

## Where to look

- `src/spritegen.cpp`
  - Looped noise helpers and row-warp utility
  - Gas / fire / chasm flipbook generators
  - HUD/status icons (procedural 4-frame micro-animations)
- `src/render.cpp`
  - Frame advancement and (for some effects) frame blending
