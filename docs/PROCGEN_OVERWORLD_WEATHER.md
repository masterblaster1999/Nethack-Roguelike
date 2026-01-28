# Procedural Overworld Weather

This project’s overworld “wilderness chunks” now have **deterministic, procedurally-generated weather**.

The goal is to add surface-world texture (wind, fog, rain, snow, dust) without:

- adding new save-file fields,
- consuming the main gameplay RNG stream,
- or requiring costly per-turn simulations.

Weather is computed on-demand from:

- **run seed**
- **overworld chunk coordinates (x,y)**
- **biome** (so deserts tend to stay dry, tundra stays cold, etc.)
- **turn count** (for slow-moving time-varying fronts; no extra save state)

Implementation lives in `src/overworld.hpp`.

---

## High-level design

### 1) “Climate” coherence

The weather generator reuses the **same broad wetness/temperature fields** used by biome selection. This keeps results intuitive:

- cold + wet → snow
- warm + wet → rain/storm
- humid wetlands → fog
- arid deserts + wind → dust

### 2) Wind as a coherent vector field

Instead of choosing wind direction randomly per chunk, wind is derived from a **noise potential field**:

1. Sample an fBm noise value at `(x,y)`.
2. Compute a finite-difference gradient `(∂f/∂x, ∂f/∂y)`.
3. Pick the **dominant axis** to map the gradient to a **cardinal direction**.

This produces **regional wind bands** that feel continuous across neighboring chunks.

### 3) Lightweight gameplay hooks

Weather is exposed to the game through `Game::overworldWeatherFx()`.

Effects are intentionally small and targeted:

- **Wind** influences drifting hazards (gas, fire) and scent.
- **Fog / Dust / Snow / Rain / Storm** can reduce player FOV radius.
- **Rain / Snow / Storm** quench fire fields faster.
- **Rain / Snow / Storm** also reduce burning duration.

There’s no added per-tile simulation beyond small tweaks to existing hazard updates.

---



### 4) Time-varying fronts (Round 180)

Weather is still **deterministic** and still avoids a per-tile simulation, but it can now *change over time*.

`overworld::weatherFor(...)` accepts an optional `turnCount` and uses it to create slow-moving **wind bands and cloud fronts** by drifting the *sampling coordinates* of the underlying noise fields:

- pick a deterministic drift direction/speed from `(runSeed, domainTag)`,
- convert `turnCount` into a coarse “weather time” scale (hundreds of turns),
- offset the wind/cloud noise coordinates by that drift.

This preserves:

- replay safety (no gameplay RNG consumption),
- regional coherence across neighboring chunks,
- and zero save-file overhead.

Passing `turnCount == 0` reproduces the original “static snapshot” behavior.

## Weather kinds

`overworld::WeatherKind` currently includes:

- `CLEAR`
- `BREEZY`
- `WINDY`
- `FOG`
- `RAIN`
- `STORM`
- `SNOW`
- `DUST`

Each kind maps to a simple `WeatherProfile`:

- `windDir` (cardinal) and `windStrength` (0..3)
- `fovPenalty` (visibility reduction)
- `fireQuench` and `burnQuench` (precipitation effects)

---

## UI / player feedback

When traveling between wilderness chunks, the travel message includes:

- biome + danger depth
- `WX:<weather>`
- optional `WIND:<dir><strength>`

The HUD surface line also displays the current wilderness weather.

---

## Notes / future extensions

This system is intentionally conservative. Some natural extensions (not implemented yet):

- temperature affecting stamina/food spoilage
- precipitation affecting scent decay and tracks
- rare lightning events in storms

