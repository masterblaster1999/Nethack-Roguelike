# Changelog

## Round 235
- Dungeon generation: added a large shared vault prefab catalog (260+ micro-vault pockets) to diversify corridor side rooms.
- Added catalog validation helpers + unit test to ensure prefab dimensions and entrance rules stay correct.
- Build: registered the new catalog .cpp in CMake sources.


## Round 234
- Levitation: diagonal corner-cutting treats adjacent chasm tiles as clear (movement + auto-pathing).
- Added a regression test for levitation-enabled diagonal frontier reachability across chasm corners.

## Round 233 - Auto-Explore Levitation & Cursor Label

- Auto-explore now treats **chasms as traversable while levitating**, so it can select frontiers/search spots across sinkholes/void when the player is safely hovering.
- Overworld Atlas cursor travel now sets the destination label to **CURSOR** (so overlays/HUD show a concrete goal label rather than the generic default).
- Docs: refreshed the Overworld Atlas doc to reflect persistence, waypoint/travel controls, and route overlays.
- Tests: added a regression test ensuring levitation enables frontier selection across a chasm barrier.


## Round 232 - Atlas Auto-Travel Route Overlay

- Overworld Atlas route preview (Ctrl+R) now also draws the active overworld auto-travel route in **green** (to your current travel goal), alongside the existing cursor route in **blue**.
- Overworld auto-travel now stores a lightweight **destination label** (UI-only) so overlays can display what you are traveling to (WAYPOINT / CURSOR / NEAREST WAYSTATION / etc.).
- Atlas details panel now shows an **AUTO:** status line (paused/direction + remaining legs + label + goal chunk).
- Tests: registered the missing manual pause test and added a label regression test.

## Round 231 - Overworld Auto-Travel Manual Pause

- Added **manual pause/resume** for overworld auto-travel (**Ctrl+O** by default). This keeps your planned route armed without canceling travel.
- HUD now shows **AUTO:PAUSED** while overworld auto-travel is paused.
- Overworld Atlas footer + docs updated to include the pause hotkey.
- New unit test for pause/resume toggle behavior.


## Round 230 - Overworld Auto-Travel Pause/Resume

- **Pause/resume:** overworld auto-travel no longer cancels just because you opened a UI overlay (inventory/help/atlas/etc.). It now **pauses** while menus are open and **resumes** when you close them.
- **Robust legs:** if an in-chunk leg is interrupted before reaching its exit gate (e.g., an overlay appears mid-walk), the system now **replans** instead of hard-canceling the whole trip.
- **Cancel semantics:** **Esc** cancels overworld auto-travel only when you are *not* already inside another UI mode. (If you are, Esc closes that overlay and travel remains armed.)
- Added a regression test covering the new pause/resume behavior.


## Round 229 - Overworld Travel: Nearest Landmarks

- **Overworld travel shortcuts (surface-only):**
  - **Ctrl+Y** — auto-travel to the **nearest discovered Waystation** (`$`).
  - **Ctrl+U** — auto-travel to the **nearest discovered Stronghold** (`!`).
  - Nearest is computed using a **BFS over the discovered chunk graph**, so it respects actual reachable paths (not just manhattan distance).
- **UX:** overworld auto-travel start messages now include the planned **leg count** (waypoint, cursor, and other destinations).
- Atlas overlay: control hints updated to show the new nearest-landmark travel keys.

## Round 228 - Overworld Auto-Travel To Cursor

- **Overworld atlas: auto-travel to cursor destination:** added **Ctrl+Shift+G** while the atlas is open to auto-travel directly to the selected chunk.
  - This does **not** change your persistent waypoint.
- **Implementation:** fully implemented `requestOverworldAutoTravelToChunk` and wired it into the atlas cursor travel action.
- **HUD:** when on the surface, the main HUD now shows auto-travel progress (next direction + remaining legs + destination), even if no waypoint is set.



## Round 227 - Overworld Auto-Travel To Waypoint

- **Overworld auto-travel to waypoint:** added chunk-to-chunk surface auto-travel that walks to the next edge gate and steps into adjacent discovered chunks until reaching your waypoint.
  - **Ctrl+G** toggles auto-travel on/off (works from both gameplay and the atlas).
  - Routing is restricted to **discovered (visited)** chunks, and each in-chunk leg requires an **explored** path to the exit gate (no free scouting).
  - Uses existing auto-travel safety rules (known-trap avoidance, hazard checks, etc.). If a leg is interrupted (blocked), the overall travel cancels cleanly.
- **Auto-travel UX:** internal multi-leg travel can suppress the per-leg "AUTO-TRAVEL COMPLETE" spam so overworld travel stays quiet and readable.



## Round 226 - Overworld Atlas Waypoint

- **Overworld Atlas waypoint:** add a player-set waypoint marker (`X`) in overworld chunk-space.
  - **Ctrl+W** sets/moves the waypoint to the current atlas cursor position.
  - **Ctrl+Shift+W** clears the waypoint.
  - Waypoint is shown in the atlas grid (subtle highlight + `X` marker), in the atlas details panel, and as a compact `WP:` tag on the main HUD while on the surface.
- **Save format bump (v58):** waypoint state is serialized so it persists across sessions.
- **Fix:** `Ctrl+R` route preview toggle is now handled while the **atlas is open** (it previously only worked from the minimap overlay).
- **Fix:** removed a stray `ss` / brace fragment in the atlas renderer that could break compilation.




## Round 225 - Overworld Atlas Route Preview

- **Overworld Atlas route preview:** added an optional route overlay that highlights the **shortest discovered-chunk path** from your current chunk to the atlas cursor.
  - **Ctrl+R** toggles the route preview **ON/OFF** while the atlas is open.
  - The route only uses **discovered (visited)** chunks; if discovery has gaps, the atlas will report the route as **UNREACHABLE**.
- Atlas details panel: shows a `ROUTE:` line with status + step count (discovered-route length).


## Round 224 - Overworld Atlas Landmark Finder

- **Overworld Atlas landmark navigation:** added a UI-only *FIND* control to jump between discovered overworld landmarks.
  - **Ctrl+F**: next landmark
  - **Ctrl+Shift+F**: previous landmark
  - **Ctrl+L**: cycle landmark filter (**ALL → WAYSTATIONS → STRONGHOLDS → ALL**)
- Atlas details: shows cursor **delta/distance** from your current chunk plus the active landmark filter + count.
- Keybinds/settings: new bind actions `overworld_next_landmark`, `overworld_prev_landmark`, and `overworld_landmark_filter` (with defaults and documentation).


## Round 223 - Overworld Atlas Persistent Terrain Summary

- **Overworld Atlas terrain stats now persist out-of-memory:** the details panel shows a cached per-chunk terrain summary (**CHASM/BOULDERS/PILLARS**) even when the chunk snapshot is not currently loaded.
- **Save format bump (v57):** overworld terrain summaries are serialized; v55/v56 saves migrate by inferring summaries from any cached overworld chunk snapshots present in the save.
- **Bugfix:** loading a save made at the **home camp** now correctly restores the `Camp/0` level (instead of attempting to restore it from the overworld chunk cache).
- Docs: updated `docs/PROCGEN_OVERWORLD_ATLAS.md` to reflect persistent terrain summaries.
- Tests: added `overworld_atlas_terrain_save_load` to validate terrain summary persistence across save/load (and after overworld cache eviction).


## Round 222 - Overworld Atlas Persistent Landmarks

- **Overworld Atlas landmarks now persist out-of-memory:** discovered waystations (`$`) and strongholds (`!`) are now displayed even when the chunk snapshot is not currently loaded, by caching lightweight per-chunk feature flags on discovery.
- **Save format bump (v56):** overworld atlas feature flags are serialized; v55 saves migrate by inferring flags from any cached overworld chunks in the save.
- Atlas details: the right-hand info panel shows **FEATURE: WAYSTATION/STRONGHOLD** regardless of loaded state; terrain stats (water/boulders/pillars) still require an in-memory snapshot.
- Docs: updated `docs/PROCGEN_OVERWORLD_ATLAS.md` and `docs/PROCGEN_OVERWORLD_STRONGHOLDS.md`.
- Tests: added `overworld_atlas_features_save_load` to validate feature-flag persistence across save/load (and after cache eviction).
- Tests build: GCC/Clang now compile `procrogue_tests` with `-fno-access-control` so the test suite can intentionally inspect internal game state.

## Round 221 - Overworld Atlas Stronghold Marker

- **Overworld Atlas landmark marker for strongholds:** the atlas grid now renders `!` for discovered chunks that contain a stronghold (when the chunk snapshot is loaded / in memory).
  - Stronghold presence is detected via the keep’s `RoomType::Vault` footprint, so the marker survives save/load even though procgen telemetry counters are generation-only.
- Docs: updated `docs/PROCGEN_OVERWORLD_ATLAS.md` and `docs/PROCGEN_OVERWORLD_STRONGHOLDS.md` to reflect the new marker and v55+ serialization notes.
- Tests: strengthened `overworld_strongholds` sanity check by asserting stronghold chunks contain a vault room.
- Code hygiene: updated outdated comments about overworld chunk/atlas serialization.

## Round 220 - Ruin Strongholds (Overworld setpieces)

- **Added rare overworld "stronghold" setpieces**: ruined, walled settlements with a central keep, scattered outbuildings, and rubble.
  - Perimeter walls are drawn as a broken ellipse ring (walls + boulder rubble), with an explicit breach marked by an open door.
  - Outbuildings are distributed via Poisson-disc sampling for organic-but-spaced layouts.
  - The keep can spawn as a mini-vault with a locked door.
- **Guaranteed loot hooks for strongholds**
  - Strongholds request at least one chest cache via `bonusLootSpots` and may request a small extra cache.
  - Optional fixed ambient items (gold/food) can be requested via `bonusItemSpawns`.
- **Reachability guarantees**: the stronghold entrance is connected back to the chunk trail hub via a carved spur trail.
- **New overworld procgen telemetry**: `overworldStronghold*` counters are tracked and surfaced in `#mapstats`.
- **Safety/robustness**: wilderness generation now clears `bonusLootSpots`/`bonusItemSpawns` up-front so generation-only requests never persist between chunk instances.

## Round 219

- **Overworld tectonics (Voronoi plates + mountain chains):** wilderness chunks now apply a seam-free **Worley/Voronoi (F1/F2)** ridge field in *world space* to create coherent **plate basins** and long **mountain ranges**.
  - Two scales (major plates + minor sub-ridges) plus deterministic **passes/gaps** to avoid infinite unbroken ridge walls.
  - Plate interiors get a small elevation bias; ridge crests are slightly **dried** to naturally reduce vegetation.
- **Talus aprons (foothills):** added a biome-tuned boulder apron along tectonic mountain edges, avoiding gate throats and running *before* landmarks so ruins aren't accidentally “talus-decorated”.
- Docs: added `docs/PROCGEN_OVERWORLD_TECTONICS.md`.
- Tests: added `overworld_tectonics` ridge-field sanity check (ensures the ridge mask has extrema and remains deterministic).

## Round 218

- **Overworld brooks + ponds (spring-fed micro-hydrology):** springs (`TileType::Fountain`) now “spill” downhill into narrow **brooks** carved as `TileType::Chasm`, following a deterministic local **steepest-descent (D8-ish)** walk over the cached elevation field.
  - Brooks join existing rivers/lakes when they hit `Chasm`; otherwise they terminate in a small **pond** when trapped in a basin.
  - A sparse **riparian bank** decorator (Pillars vs Boulders, biome-tuned) is applied around some brook/pond edges so the new water reads coherently.
  - New telemetry: `overworldBrookCount`, `overworldBrookTiles`, `overworldPondCount` (shown by `#mapstats` under `OVERWORLD`).
- Docs: added `docs/PROCGEN_OVERWORLD_BROOKS.md`.
- Tests: added `overworld_brooks` sanity check (find at least one spring adjacent to water in a neighborhood).

## Round 217

- **Overworld climate upgrade (wind + orography):** overworld terrain now runs a lightweight **climate wetness advection** pass that couples a per-run **prevailing wind** with local **ridge shadows**.
  - **Windward uplift:** rising slopes into the wind get slightly wetter.
  - **Rain shadow:** lee-side basins behind strong ridges become drier.
  - The wetness correction is applied *before* lakes/rivers/vegetation/springs are carved, so it meaningfully shapes the whole overworld.
- **Biome picking improved:** chunk biomes now incorporate a cheap upwind lookback (rain shadow + ocean proximity) and apply an altitude lapse-rate to temperature for more coherent macro-regions.
- Docs: added `docs/PROCGEN_OVERWORLD_CLIMATE.md`.
- Tests: added `overworld_biome_diversity` sanity check (ensures biome variety + cardinal prevailing wind).

## Round 216

- **Overworld springs (blue-noise POIs):** wilderness chunks can now place 0..2 small **springs** as `TileType::Fountain`, using **Poisson-disc (blue-noise)** candidates + a wetness/flatness scoring pass to bias toward valley basins.
  - Springs carve a small clearing and add a biome-tuned riparian ring (Pillars in wet biomes, Boulders in harsh biomes).
  - New telemetry: `overworldSpringCount` (shown by `#mapstats` as `OVERWORLD | SPRINGS <N>` when present).
- **Procgen refactor:** extracted Bridson Poisson-disc sampler into reusable header `src/poisson_disc.hpp`, used by the RoomsGraph generator and overworld spring placement.
- Docs: added `docs/PROCGEN_OVERWORLD_SPRINGS.md`.
- Tests: added `overworld_springs` sanity check (find at least one spring in a neighborhood).

## Round 215

- **Overworld river upgrade (domain-warped, multi-band fluvial network):** macro rivers are no longer a single noise ribbon—rivers are now generated from **two oriented trunk bands** (for long runs + occasional confluences) plus **wetness-gated tributary/capillary channels**.
  - **Domain warping** adds natural meanders and breaks up straight contour artifacts.
  - **Width modulation** now incorporates wetness and *local flatness* (estimated from the elevation gradient) so wet lowlands can appear more braided.
  - New **riparian bank** decoration pass: sparse Pillars/Boulders appear along water edges (biome-tuned), while keeping gate throats clear.
- **Trail crossings telemetry:** overworld trail carving now increments `fluvialCausewayCount` when a trail converts `Chasm` to `Floor` (useful for debugging / mapstats).
- Docs: updated `docs/PROCGEN_OVERWORLD_RIVERS.md`.
- Tests: added `overworld_rivers` sanity check (finds at least one chunk with water + bank adjacency in a neighborhood).

## Round 214

- **Overworld trail network upgrade (direction-aware A* road planner):** wilderness chunks now connect border gates to the central hub using a terrain-cost **A*** planner that prefers existing trails, avoids rivers (**Chasm**) and mountains (**Wall**), and penalizes hard turns for smoother, more readable paths.
  - Trails now respect the heightfield: steep local elevation gradients are expensive, producing more natural “go around / gentle climb” routes instead of straight tunnels through ridges.
  - **Terrain-preserving carving:** trail widening no longer bulldozes rivers/mountains—only the centerline is guaranteed floor; the radius expansion skips Wall/Chasm unless explicitly forced (hub clearing), preserving readable river silhouettes.
- Docs: added `docs/PROCGEN_OVERWORLD_TRAILS.md`.
- Tests: added `overworld_trails` connectivity regression; fixed test references to `DUNGEON_MAX_DEPTH` to use `Game::DUNGEON_MAX_DEPTH`.

## Round 213

- **Ancient Rune Wards (leyline setpieces):** dungeon generation can now carve a tiny number of functional **Rune Wards** at **high-intensity leyline nodes** (depth 3+), biased toward junctions/open spaces.
  - Placement avoids obvious safe zones (kept away from stairs landings; not inside Shops/Shrines/Vaults/Secret rooms; never overwrites existing engravings).
  - Ancient wards have **finite durability** like normal wards.
- **Leyline rune caches:** occasionally a **matching-element Rune Tablet** spawns near an Ancient Rune Ward (bounded proc-id search to align element), creating a small "follow the leyline" exploration breadcrumb.
- Docs: updated `docs/PROCGEN_RUNE_WARDS.md`.

## Round 212

- **Rune wards (proc spell synergy with the ward system):** Ward-form procedural rune spells (usually from **Rune Tablets**) now **etch an elemental ward inscription underfoot** when cast (without overwriting non-ward / non-graffiti engravings).
  - New ward keywords: `RUNE FIRE`, `RUNE SHADOW`, `RUNE RADIANCE`, etc. Parsing is forgiving (`RUNE:FIRE`, `RUNE OF FIRE`, and optional sigil suffix like `RUNE FIRE: KAR-THO-RAI`).
  - Elemental affinities: different rune wards repel different monster sets (undead vs raiders vs brutes, etc).
- Docs: `docs/PROCGEN_RUNE_WARDS.md`
- Tests: `rune_wards` parsing/affinity sanity checks.

## Round 211

- **Procedural leylines (arcane resonance field):** a deterministic per-tile (0..255) "mana current" cache computed alongside terrain materials and ecosystem seeds.
  - Leylines tend to form thin linework along macro ridges and ecosystem boundaries, with extra strength on conductive substrates (metal/crystal).
  - **Gameplay:** standing on strong currents nudges mana regeneration; spell failure chance is slightly reduced on strong currents (and slightly increased in dead-zones).
  - **UI:** LOOK now shows `LEY: FAINT/BRIGHT/STRONG` when relevant.
- Tests: add `proc_leylines` sanity/determinism test.
- Docs: `docs/PROCGEN_LEYLINES.md`.

## Round 210
- Fix: remove duplicate inline definition of `ecosystemKindFlavor()` (header compile error).
- **Ecosystem Loot Ecology:** biome-biased weapon egos (brands) + small themed loot caches near ecosystem seeds (derived RNG; doesn’t perturb main loot stream).
- Refactor: `spawnItems()` item dropping now supports **derived RNG streams** for deterministic “side systems” (used by loot caches).
- Tests: add `ecosystem_weapon_ego_loot_bias` guardrail test.
- Docs: `docs/PROCGEN_ECOSYSTEM_LOOT_ECOLOGY.md`.

## Round 209
- **Ecosystem microclimates**: biome regions now have small, deterministic *local* environment modifiers that change how areas feel tactically.
  - **Visibility (FOV)**: ecosystems can nudge sight radius (`VIS:+1` / `VIS:-1`) while you stand in them.
  - **Hazard persistence**: gas/fire diffusion uses ecosystem-specific quench/spread deltas, so some regions scrub vapors quickly while others let dense clouds cling.
  - LOOK mode now shows `VIS` next to `ECO` for quick at-a-glance readouts.
- Added `docs/PROCGEN_ECOSYSTEM_MICROCLIMATES.md`.

## Round 208
- **Ecosystem resource nodes**: stationary harvestable nodes spawn near biome seeds, themed by ecosystem.
  - Harvesting yields shards and can trigger a biome-specific backlash hazard.
- Added `docs/PROCGEN_ECOSYSTEM_RESOURCE_NODES.md` and `docs/ROUND208_NOTES.md`.

## Round 207
- **Ecosystem stealth ecology (acoustics + scent)**: procedural biome regions now influence stealth and tracking in a deterministic, material-composable way.
  - Footstep noise is nudged by ecosystem (e.g., FloodedGrotto splashes louder; FungalBloom hushes steps).
  - Monster hearing is adjusted by local biome acoustics (masking water/wind vs echoing crystal/metal) so the same noise may or may not alert foes depending on where they are.
  - The player’s **#listen** range is similarly affected by local acoustics.
  - Scent trail simulation now layers ecosystem deltas on top of substrate materials: FungalBloom scents linger/spread; FloodedGrotto washes scent quickly.
- Added docs: `docs/PROCGEN_ECOSYSTEM_STEALTH_ECOLOGY.md`.

## Round 206
- **Ecosystem crafting catalysts**: the local procedural biome now acts as a deterministic **catalyst** for Crafting Kit recipes (in addition to room-type workstations).
  - Crafting seed is salted by `EcosystemKind`, so the **same ingredients** can yield **different sigils/outcomes** in different biomes (stable across save/load).
  - The crafting preview now shows both **WORKSTATION** and **CATALYST** lines.
  - The Craft Recipe journal now records `@WORKSTATION/CATALYST` for each learned sigil.
  - Biome flavor nudges (deterministic):
    - FungalBloom: can yield **extra Essence Shard byproduct**.
    - CrystalGarden: can **boost wand charges**.
    - RustVeins: can **temper gear enchantment**.
    - BoneField: can **forge enchantment** with a small **cursed risk**.
    - AshenRidge: can **boost fire-wand charges** and lightly **temper gear**.
    - FloodedGrotto: can **brew fuller potions** and sometimes **wash away curses**.
- Added docs: `docs/PROCGEN_ECOSYSTEM_CRAFTING_CATALYSTS.md`.

## Round 205
- **Ecosystem pulses (biome events)**: main-dungeon floors with ecosystem seeds now emit periodic, deterministic **pulses** near ecosystem cores, seeding short-lived hazards/boons that then drift via the existing field simulation.
  - FungalBloom: spore burst → **confusion gas**.
  - RustVeins: acrid vent → **corrosive gas**.
  - AshenRidge: ember fissure → **fire** (plus a tiny smoky/confusion fringe at depth).
  - FloodedGrotto: cool mist **quenches nearby fire**, can reduce your **burn duration**, and emits dripping **echo noise**.
  - CrystalGarden: resonance chime emits noise and can restore **1–2 mana** if you’re nearby.
  - BoneField: ossuary miasma → **poison gas**.
- Pulses are **RNG-isolated** (hash-derived) and keyed off `turnCount` + a per-floor phase, so they are stable across save/load and do not perturb other procedural rolls.
- Added docs: `docs/PROCGEN_ECOSYSTEM_PULSES.md`.

## Round 204
- **Ecosystem-aware spawn ecology**: room and guardian monster spawns now consult the local procedural **ecosystem field**, biasing spawn-table weights toward biome-appropriate monsters.
  - Ecotone (biome boundary) tiles blend in a weaker secondary bias from the neighboring ecosystem to create natural transitional mixes.
  - Ecotones also slightly increase the odds of liminal tricksters (**Mimics/Leprechauns/Nymphs**) and (at depth) **Wizards** so borders feel unstable and weird.
- **Biome-tuned nest clusters**: existing small nest/cluster spawns (spider/snake/slime/bat) now receive ecosystem-based chance adjustments, and BoneField regions can occasionally raise a small **zombie pack**.
- Added docs: `docs/PROCGEN_ECOSYSTEM_SPAWN_ECOLOGY.md`.

## Round 203
- **Ecosystem guardians (apex packs)**: main-dungeon floors with procedural biome seeds may now spawn **0–2 deterministic guardian packs** near ecosystem cores.
  - Leader is a proc-ranked **Elite/Champion/Mythic** with **Gilded** + a biome-themed signature affix and proc abilities.
  - Spawns avoid stairs, shops, and treasure/vault/secret rooms to keep difficulty spikes fair.
  - Placement uses an **RNG-isolated** stream (derived from run seed + depth) so it doesn’t perturb other spawn randomness.
- **Proc-ranked Essence Shard drops**: Champion/Mythic enemies (proc tier ≥ 2) can now shed a small stack of **ecosystem-aligned Essence Shards** on death.
  - Drop/tag/tier/shiny are **hash-derived** (deterministic) so they don’t perturb the global RNG stream that drives other loot/events.
- Added docs: `docs/PROCGEN_ECOSYSTEM_GUARDIANS.md`.

## Round 202
- **Ecosystem sigils**: floors with procedural biome seeds now occasionally place ecosystem-aware **"Heartstone" sigils** near a biome core and rarer **"Ecotone" sigils** at biome boundaries.
  - Heartstones map each ecosystem to a **good/bad sigil pair** (e.g., Fungal → REGEN/VENOM; Ashen → AEGIS/EMBER) with depth-biased hazard odds.
  - Ecotones detect boundary tiles (cardinal neighbor ecosystem diversity) and choose a sigil kind from a mixed pool derived from both adjacent biomes.
  - Both placements are **hash-derived** (RNG-isolated) so they don’t perturb the main RNG stream.
- **Sigil parameter consistency in overworld**: sigil triggering now uses the same `(materialWorldSeed, materialDepth)` domain as sigil spawning so wilderness chunks get consistent sigil effects.

## Round 201
- **Ecosystem trap clusters**: biome regions now sometimes spawn small, ecosystem-themed trap patches (webby spore pockets, rune-y crystal wards, bone spikes, corrosive rust vents, ashen haze, grotto drips).
  - Uses an isolated RNG stream so these hazards don’t perturb other trap setpieces.
  - Counts toward the normal trap budget (flavor, not a global difficulty spike).
- **Ecosystem ambient particles (visual-only)**: visible tiles in ecosystem regions now emit subtle biome-specific particles (spores, crystal motes, dust, ash, droplets) using the renderer’s deterministic phase-crossing emitter logic.
- Fixed a latent build issue in `game_spawn.cpp`: include `craft_tags.hpp` explicitly and use `RNG::next01()` instead of a non-existent `unit()` helper.

## Round 200
- **Stratum-biased ecosystems**: each infinite-world stratum (and signature floors) now selects a deterministic **primary/secondary ecosystem palette**; biome seeds are biased toward these picks for stronger macro-coherence across depths.
- **Ecosystem essence resources**: floors now spawn small, deterministic clusters of **Essence Shards** aligned to local ecosystems (tag/tier/shiny based on ecosystem + substrate + depth) for crafting synergy.
- **Biome UI feedback**:
  - LOOK/inspect now shows `ECO: ...` for terrain tiles.
  - Stepping into a new ecosystem kind triggers a one-time per-floor callout (and pauses auto-move/auto-explore on first entry).
- **Mapstats upgrade**: `#mapstats` now reports ecosystem seed count and the top 3 ecosystem coverages.

## Round 199
- **Procedural ecosystem seeds**: added a deterministic per-floor **ecosystem field** (soft Voronoi biome patches) derived from the same level key as terrain materials.
  - Ecosystem types: **FungalBloom**, **CrystalGarden**, **BoneField**, **RustVeins**, **AshenRidge**, **FloodedGrotto**.
  - Not serialized; recomputed on demand alongside `Dungeon::ensureMaterials()`.
- **Biome substrate overlays**: ecosystem patches softly bias floor materials (moss/dirt, crystal growth, bone dust, metal seams, volcanic stone), which then feeds into the existing substrate FX + biolum seeding.
- **Ecosystem-aware proc monster variants**: ranked procedural monsters now incorporate the local ecosystem into their deterministic variant seed and apply small ecosystem-themed weight nudges to proc **affix** and **ability** rolls.

## Round 198
- **Directional auto-run**: added a classic roguelike “run” mode that keeps stepping in a chosen direction until you reach an **intersection**, **obstacle**, **loot**, **stairs**, or a **safety stop** triggers.
  - Default bindings: **Shift + Arrow Keys** (cardinals) and **Shift + Numpad** (8-way)
  - Rebindable actions: `run_up`, `run_down`, `run_left`, `run_right`, `run_up_left`, `run_up_right`, `run_down_left`, `run_down_right`
- **Smart corridor following**: auto-run will automatically follow forced corridor corners (when there’s exactly one forward option), but stops at branch points so you can decide.
- **Fair exploration**: auto-run will not path into **unexplored** tiles (so it can’t “peek” at the true map); just hit run again when you’re ready to push into new territory.

## Round 197
- **Interactive replay playback controls**: `--replay` sessions are no longer “hands off”. While a replay is playing you can now:
  - **Pause / resume** (`replay_pause`, default **Space**)
  - **Step** to the next recorded input event (`replay_step`, default **.** / **Right Arrow**)
  - Adjust **playback speed** (`replay_speed_down` / `replay_speed_up`, defaults **-** and **+**)
  - **Stop playback** (Esc) to immediately unlock input and continue playing from the current state
- **Replay HUD indicator**: the HUD stat line now shows `REPLAY` with **speed** and a **mm:ss/mm:ss** timeline, plus `PAUSED` when paused.
- **Replay safety**: during playback, only replay controls + a small set of visual-only toggles are accepted to avoid desync; stopping playback (or a hash mismatch) automatically disarms verification so you can safely take over.
- **Playback speed is simulation-aware**: speeding up also scales the simulation timestep (auto-travel and other time-based systems keep up), using small fixed update steps for stability.

## Round 196
- **In-game replay recording controls**: added extended commands `record` / `stoprecord` (plus `rec` / `stoprec`) so you can start/stop `.prr` replay capture **without restarting the app**.
  - `record [path]` starts recording to an optional filename (defaults to the same timestamped name used by `--record`).
  - `stoprecord` closes the replay file and prints the saved path.
- **HUD indicator**: while recording, the main HUD stat line shows a compact `REC` tag so you can tell at a glance when capture is active.
- Added a unit test that asserts the new command prompt routes `record` / `stoprecord` into platform-layer recording requests.

## Round 195
- **Raycast 3D free-look turning**: added view-only camera turning actions `view_turn_left` / `view_turn_right`.
  - Default keybinds: **Alt+Q** = turn left, **Alt+E** = turn right (plus Alt+Left/Right as an alternative).
  - New settings: `raycast3d_follow_move` (camera follows last movement tween) and `raycast3d_turn_deg` (degrees per keypress).
  - Turning triggers a short "manual look" hold to prevent follow-move snapping from overwriting the turn instantly.
- Added a unit test that asserts the new action tokens exist in the shared action registry.

## Round 194
- **Action palette (command prompt)**: the extended command prompt now acts like a lightweight **command palette**.
  - Prefix with **`@`** to run any **action token** (the same tokens used for `#bind`).
  - **TAB completion** lists actions and shows their **keybind hint + description**.
  - Examples: `@inventory`, `@toggle_minimap`, `@stairs_down`, `@look`.
- **TAB cycling fix**: completion cycling now works even when the base prefix is empty (e.g. `#bind` / `#unbind` argument completion from a blank value, and `@` action palette from empty input).
- Added a unit test that exercises `@inv` autocomplete and `@inventory` execution.

## Round 193
- **Raycast 3D**: render depth-correct **billboard sprites** for visible entities and ground items.
  - Per-column z-buffer occlusion (sprites properly hide behind walls).
  - Per-tile lighting/tint modulation + distance shading to match the raycast world.
  - New settings: `raycast3d_sprites`, `raycast3d_items`.

## Round 192

- **Raycast3D lighting parity**: the pseudo‑3D raycaster now uses the **same per-tile lightmap + depth tint** as the 2D/isometric renderer, so walls/floors/ceilings react to **colored lighting, darkness, and bioluminescence** instead of only “visible vs explored”.
- **Torch flicker in 3D**: nearby torches now add a subtle, deterministic flame flicker to the first-person view.
- **Specular highlights respect lighting**: shiny materials now tint specular highlights using the current light color (and only on visible tiles), so reflections feel anchored to in-world light sources.

## Round 191

- **Keybind auto-migration (persisted)**: common legacy `procrogue_settings.ini` defaults are now detected, repaired, and written back to disk:
  - `bind_search = c` is migrated to `shift+c` when it conflicts with modern diagonal `c` movement.
  - `bind_message_history` no longer steals `shift+m` from the **Overworld Map**.
  - `bind_restart` no longer steals `f6` from **High Scores** (Restart stays on `shift+f6`).
  - Legacy single-key binds are upgraded with conventional shortcuts when they don't introduce obvious conflicts: **Ctrl/Cmd+Comma** for Options, and **Ctrl+P / Shift+Ctrl/Cmd+P** for the extended command prompt.
- **Default settings keybinds updated**: the generated settings file now ships the up-to-date action set and keybind defaults (Overworld Map, Spells, Parry, Butcher, High Scores, etc.), preventing “fresh install” drift and conflicts.

## Round 190

- **Keybind syntax fix**: chord parsing now supports the common `++` spelling for the literal **plus** key (so `ctrl++` / `cmd++` works in `procrogue_settings.ini` and in `#bind`), while keeping `+` as the modifier delimiter.
- **Keypad shorthand support**: added forgiving parsing for keypad operator keys and common shorthands (`kp+`, `kp-`, `kp*`, `kp/`, `kp.`, `kp,`, `kp=`; plus `kp_add`/`kp_sub`) so bindings are easier to author by hand.
- **AltGr reliability**: when SDL reports **AltGr** via `KMOD_MODE` (and some platforms also set `CTRL`), keybind matching now treats MODE as ALT and drops CTRL while MODE is held; config also accepts `altgr+` / `mode+` modifier names.

## Round 189

- **Keypad keybind fix**: added explicit, config-safe tokens for keypad operator keys (`kp_plus`, `kp_minus`, `kp_multiply`, `kp_divide`, `kp_period`, `kp_comma`, `kp_equals`) so they can be captured, displayed, and parsed reliably (and no longer collide with the `+` chord delimiter).
- **CMD prompt UX**: when the TAB completion dropdown is active, **Up/Down now navigates the suggestion list** (instead of jumping straight into history). Cursor navigation (Left/Right/Home/End) now clears the completion dropdown so it never “sticks” after you start editing.
- **Keybinds QoL**: in REPLACE mode, binding a chord now **auto-reassigns** it by removing that chord from any other action it was bound to (ADD mode still allows intentional overlaps, with a warning).

## Round 188

- **CMD prompt fix**: Home/End navigation in the extended command prompt now works again (Cmd+Left/Right and Ctrl+A/E no longer get eaten by message-log scrolling).
- **Key mapping robustness**: keybind action evaluation order now automatically appends the canonical action registry (`action_info.hpp`) after the hand-tuned priority list, preventing future “bindable but unreachable” actions when new Actions are added.
- **International keyboard fix**: treat SDL's **KMOD_MODE** (often AltGr) as **ALT** for keybind capture/matching and for slash-driven search toggles, avoiding false “no-modifier” detection on layouts that require AltGr.
- **UI polish**: command prompt footer now says **COMMAND/ARGS** (not "CMD/ARGS") and shows platform-appropriate modifier labels (OPT on macOS; avoids advertising WIN/SUPER combos on non-mac).

## Round 187

- **Options menu fix**: "Control Preset", "Keybinds", and "Close" now trigger the correct actions (they were index-misaligned).
- **Key mapping fix**: `Butcher` is now reachable from both gameplay and the inventory overlay (it was bindable but never mapped in the keybind priority lists, so `SHIFT+B` did nothing).
- **CMD layout/QoL**: added **Ctrl/Cmd+F** to open/toggle search in **Message History** and the **Keybinds** filter; updated UI hint strings.

## Round 186

- **Keybind reliability fix**: canonicalize SDL modifier groups (Shift/Ctrl/Alt/Cmd) so left/right variants match bindings consistently (fixes Shift+... actions and Cmd+... binds).
- **Key mapping fix**: `ToggleOverworldMap` is now included in the keybind action priority list so its binding actually works; default `SHIFT+M` is reserved for the Overworld Map and Message History defaults to `F3` to avoid conflicts.
- **Command prompt UX (CMD layout)**: added mac/desktop-friendly text-editing shortcuts (Cmd+Left/Right = Home/End, Alt+Left/Right = word move, Alt+Backspace = word delete, Cmd+Backspace = kill-to-start, Cmd+L = clear line) and **Ctrl/Cmd+V paste** support (command prompt + search filters).
- Updated in-game HUD/help strings to reflect the new shortcuts (CTRL/CMD variants) and show platform-appropriate GUI modifier labels (CMD/WIN/SUPER; OPT on macOS).

## Round 185

- **Raycast 3D surfacing upgrade**: added **parallax texture mapping** and **material specular highlights**
  to the pseudo‑3D renderer, plus subtle **contact shadows** along wall/floor/ceiling seams for improved
  depth perception.
- Added new `settings.txt` knobs for the 3D view: `raycast3d_parallax`, `raycast3d_parallax_strength`,
  `raycast3d_specular`, and `raycast3d_specular_strength` (hot-reloadable via Config Reload).

## Round 184

- **Raycast 3D view upgrade**: added **procedurally generated textured ceilings** (ceiling casting) and
  optional **relief shading** (cheap bump/normal mapping derived from texture luminance gradients) for
  walls/floors/ceilings to make the pseudo‑3D view read as more “3D”.
- Added new `settings.txt` knobs for the 3D view: `raycast3d_scale`, `raycast3d_fov`,
  `raycast3d_ceiling`, and `raycast3d_bump` (hot-reloadable via Config Reload).

## Round 183
- Add **raycast 3D view mode**: experimental first-person/pseudo-3D renderer that draws textured walls + floors using the same deterministic proc-generation pipeline (including terrain material overlays).
- View modes now **cycle** with the Toggle View Mode action (F7 by default): **TOPDOWN → ISOMETRIC → 3D → TOPDOWN**.
- New doc: `docs/PROCGEN_RAYCAST3D_VIEW.md`.


## Round 182
- Add **procedural terrain material overlays**: per-`TerrainMaterial` transparent detail patterns (wood grain, metal seams, marble veins, moss patches, etc.) layered onto floor + wall tiles, including isometric floor support.
- New doc: `docs/PROCGEN_TERRAIN_MATERIAL_OVERLAYS.md`.


## [0.22.0] - Unreleased

### Added
- **Spell miscasts**: Learned spells now have a failure chance (0..95%) influenced by Focus, character level, armor/encumbrance, confusion, and hallucination. Miscasts still consume a turn and some mana, create noise, and can cause backlash; **Blink** miscasts can drift. Spells UI + targeting preview now show **FAIL%**.
- **Procedural rune tablets**: Rune Tablets now spawn and are fully usable — each tablet carries a deterministic proc-spell id, displays its rune-spell name, and can be invoked from the inventory to cast procedural rune magic (with targeting UI + mana costs).
- **Procedural weapon ego ecology**: expanded ego weapon brands (WEBBING/CORROSIVE/DAZING) and made ego rolls bias toward a per-floor theme plus room type + terrain material, so loot feels more coherent without becoming predictable.
- **Procedural victory plans (win conditions)**: Victory plans now support 1–2 run-seeded key clauses (shown as "ALSO:"), including templates such as foodless/vegetarian/atheist/illiterate conducts, hide/bone trophy hunts, and **fish trophy hunts** (rarity-tiered, optionally tagged) while still supporting classic Amulet wins. Goals are shown at run start, in the HUD, and in the stats overlay; the camp exit checks the plan and lists missing requirements.
- Rune Tablets are now flagged as **consumables** (read/use) in item definitions, in preparation for the upcoming procedural casting vertical slice.
- Rune Tablet **procedural spell targeting/casting**: `TargetingMode::RuneTablet`, plus `canCastProcSpell` / `castProcSpell` / `castProcSpellAt` for deterministic rune spells, now fully wired to inventory item use.
- Rune Tablets now appear as rare loot (Treasure/Vault/Secret rooms), can be found on Shrines, and may be stocked in Magic shops (with depth-based rune tier).
- Procedural terrain palette: per-run/per-floor HSL tints with room-style accents; configurable via settings and the new `#palette` command.
- Procedural palette upgrades: added **HSV hue/saturation/brightness** controls plus a smooth per-floor **spatial chroma field** (low-frequency noise) to reduce large-area flatness while keeping doors readable.
- **Procedural biolum terrain**: a deterministic Gray-Scott reaction-diffusion lichen/crystal glow field that injects **colored ambient light sources** in darkness mode (plus subtle biolum motes); see `docs/PROCGEN_TERRAIN_BIOLUM.md`.
- Procedural terrain materials: deterministic, depth-aware cellular-noise substrate (STONE/BRICK/BASALT/...) used for subtle tinting (scaled by proc_palette strength) and LOOK descriptions.
- **Overworld wilderness chunk profiles**: wilderness chunks now have deterministic biome/name/danger identities, continuous terrain noise across chunk borders, meandering trail connections between edge gates, lightweight biome landmarks, per-chunk material palettes, and HUD/travel messaging that surfaces region identity.
- **Time-varying overworld weather fronts**: wilderness chunk weather now accepts `turnCount` and drifts wind/cloud noise fields to create slowly changing fronts (still deterministic; no gameplay RNG use; no save-file state); see `docs/PROCGEN_OVERWORLD_WEATHER.md`.
- **Overworld macro rivers**: surface wilderness chunks now include world-space continuous river ribbons carved as camp-water chasms; river width is biome-tuned and can widen in wet microclimates; trails are carved after rivers so edge-gate traversal stays reliable.
- **Overworld weather**: wilderness chunks now have deterministic weather profiles (wind/fog/rain/snow/dust) derived from run seed + chunk coords; fog reduces FOV on the surface, precipitation quenches fire/burning, and the HUD/travel log show WX/WIND.
- **Overworld waystations**: wilderness chunks now have rare, biome-aware **traveling merchant caravans** (small `RoomType::Shop` setpieces) connected to the chunk trail hub; waystations reuse the normal shopkeeper + stocking system for surface trading.
- **Overworld atlas (world map)**: new **Shift+M** overlay that shows your discovered overworld chunks as a biome grid, with cursor inspection for region name, danger depth, and deterministic weather (plus lightweight terrain stats when the chunk is loaded).
- **Overworld boundary-shared gates**: wilderness chunk edge gates are now offset per shared chunk boundary (deterministic and aligned with neighbors), forming continuous cross-chunk roads; home-camp-adjacent gates remain centered to preserve the camp layout.
- **Overworld danger-depth scaling for surface loot/traps/fountains**: overworld chunks now use their computed danger depth for item, trap, and fountain generation (matching monsters) so the wilderness difficulty curve stays coherent as you travel farther from camp.
- Added `docs/PROCGEN_OVERWORLD_RIVERS.md` and a deterministic overworld chunk unit test.
- **Procedural companion behaviors**: new companion traits (**Scenthound**, **Shiny**, **Pack Mule**) with gameplay hooks (trap sniff pings, opportunistic gold fetching while following, and a modest carry-capacity bonus when nearby). LOOK and `#pet` status now surface companion names + traits.
- **Capture spheres (Palworld/Pokemon-inspired)**: you can now capture eligible monsters into **Capture Spheres** / **Mega Spheres**, then **recall** or **release** them later; captured pals persist as items in your inventory.
  - **Pet progression**: captured pals store **Level / XP / Bond / HP%** inside their sphere, gain XP from kills (plus a small party share when *you* kill), and receive modest stat growth as they level and bond.
  - **Sphere economy**: empty spheres can now appear as rare treasure finds and are stocked in magic shops (with deeper floors occasionally offering Mega Spheres).
- **Procedural fishing**: cast a **Fishing Rod** into **camp water basins** (overworld chasms) and **fountains** to reel in deterministic, procedurally generated fish.
  - Fishing uses a per-tile **bite cadence** (learnable hot/cold windows) with a targeting preview that shows current bite status + catch chance.
  - Caught fish are edible and can grant small buffs (or rare weird effects) based on their procedural bonus tag.
  - **Big fish now fight back**: large/rare (and shiny) fish trigger a short, turn-based **tension / progress** reeling prompt (ENTER to REEL, . to give SLACK).
  - Targeting preview now includes a **countdown** to the next bite window (and remaining window time when hot).
- **Procedural farming foundations**: introduces deterministic crop/soil generation (names, rarity, yield/quality hooks) and adds farming item kinds (hoe, seeds, tilled soil, crop stages, produce) with save-compatible metadata packing.
- **Procedural crafting foundations**: adds a **Crafting Kit** item kind and a deterministic, ingredient-driven crafting generator (`craftgen`) that maps two ingredients into a stable, procedurally chosen output.
- **Procedural crafting mechanics**: the Crafting Kit is now usable to craft by selecting two ingredients in a dedicated inventory prompt; crafting in themed rooms acts like a location-based 'workstation' that can nudge item quality.
- **Procedural forging (crafting)**: crafting with gear and/or butchery materials now yields **forged wearable items** (weapons/armor/rings/ranged) with deterministic enchant/BUC/ego rolls, plus a rare **artifact infusion** that is tuned to the recipe's essence tags (e.g., VENOM/EMBER/WARD/VITALITY). High-tier recipes can also output spellbooks and rune tablets.
- **Procedural bounties**: introduces **Bounty Contracts** (guild kill contracts) that track progress inside the item; complete the objective to redeem a deterministic reward (use `#bounty` to list active contracts).
- **Procedural shrine patrons**: each Shrine room now has a deterministic **patron** (name + domain) shown in the HUD/LOOK; shrine prayer costs are biased toward the patron, and prayers grant a small domain "resonance" boon (with **Insight** shrines also offering cheaper augury).
- **Crafting Insight**: inventory crafting now shows a live **workstation + recipe preview** and assigns each recipe a deterministic **sigil name**; use `#recipes` to list recipes you've learned this run.
- Material acoustics + scent absorption: substrate materials now modulate **footstep/dig noise** and **scent trail decay/spread** (moss/dirt dampen; metal/crystal ring out).
- **Material acoustics now also affect sound propagation**: `Dungeon::soundTileCost()` incorporates substrate materials (moss/dirt/wood dampen; metal/crystal carry), so `computeSoundMap`-based systems (noise alerting, LOOK sound preview, `#listen`, and sneak-aware auto-travel) reflect the floor you're moving on.
- **Wind-biased scent trails**: the existing per-level deterministic wind now also nudges scent propagation, stretching trails downwind and sharpening upwind drop-off.
- Smell tracking AI now escapes local scent plateaus via a small bounded BFS, improving animal tracking behavior in tight corridors when diffusion is conservative.
- **Noise uncertainty investigation**: monsters now remember how imprecise a sound-based alert was (from noise localization) and perform a deterministic perimeter sweep within that uncertainty radius once they reach the anchor, instead of taking a single random “search” step. This makes stealth + **Throw Voice** lures feel more coherent.
- **Monster torches**: on dark floors, some humanoid monsters can spawn with torches and will sometimes **light them** when alerted but unable to see you, creating **moving light sources** that interact with darkness stealth. Lit torches flicker visually, burn down over time, and drop as loot.
- **Procedural artifact powers**: artifacts now have real, deterministic power effects (FLAME/VENOM/DAZE/WARD/VITALITY). Power tags grant small passive bonuses while equipped and can trigger lightweight combat procs (burn, poison, confusion, shimmering wards, life surge regen).
  - Natural healing now respects **total Vigor** (base + ring + artifact bonuses), so Vitality-themed gear has an immediate impact beyond level-up math.
- **Inventory trait tooltips**: inventory preview now surfaces **EGO** and **ARTIFACT** proc/passive lines for gear, and adds a compact **TRAITS** summary line for your equipped loadout.
- **Parry + Riposte stance**: New Parry action (default **Shift+P**) that grants a brief defensive stance (+AC). If an enemy misses you in melee while parrying, you can instantly **riposte**; the first turn is a **perfect parry** window that also staggers attackers (confusion).
- **Butchering**: New Butcher action (default **Shift+B**) to carve corpses into meat/hide/bones. Butchered meat inherits freshness and can carry a small essence tag when eaten.
- **Procedural butchering (expanded)**: Corpses now carve into multiple distinct meat cuts (including prime cuts), and hides/bones gain material variants (pelts, scales, chitin, horns/fangs/teeth, robe scraps) plus per-yield quality. Quality/variant is reflected in names and sprites, and hide/bone products now contribute meaningful essences to crafting.
- **Confused melee swings**: Confusion now also scrambles **melee attacks** for monsters and companions (biased toward the intended target), enabling wild whiffs and occasional accidental **friendly fire / infighting** when multiple creatures are adjacent.
- **Kick-rolled boulders**: You can now **kick boulders to set them rolling**, crushing anything in their path. Rolling-boulder traps and kicked boulders share a unified boulder-roll simulator with momentum-based damage falloff.
- **Morale shock (intimidation)**: when you (or a friendly companion) kill a hostile, nearby hostiles with line-of-sight may **panic and flee** for a few turns. Tougher enemies and commander-led groups resist; assassinations have a tighter panic radius. (Deterministic d20 save; no RNG consumption.)
- **Conductive sparks**: Spark projectiles can now **arc** (chain lightning) to nearby hostile targets when they strike **conductive terrain** (metal/crystal substrates, and water chasms) or score a critical hit. Wands are more likely to arc and can chain farther; arcs can briefly **stun** (confusion).

- Fixed save v54 Parry persistence: entity deserialization now correctly reads and assigns `parryTurns` (previously a missing temp could break builds / loads).
- **Wolf pack encirclement**: wolves now coordinate as a pack when they have line-of-sight, using a tiny min-cost matching solver to claim distinct adjacent tiles and reduce pileups while applying surround pressure.
- **Expanded floor wards**: `#engrave` can now create multiple warding words (**ELBERETH**, **SALT**, **IRON**, **FIRE**) with different monster affinities.
  - LOOK now displays the **ward type** and **uses remaining**.
  - Cornered brutes can now attempt to **smudge** a ward away instead of freezing forever.
- **Procedural graffiti rumors**: generated dungeon graffiti is now produced by a deterministic grammar that can emit floor-specific **hints** (secret doors, vaults, chasms, boulder-bridges, shops) with 8-way direction + distance buckets, while preserving classic one-liners; see `docs/PROCGEN_GRAFFITI_RUMORS.md`.
- **Procedural sigils**: rare `SIGIL: ...` floor inscriptions are now procedurally parameterized (radius/intensity/duration) per (run seed, depth, position) and expanded with new archetypes (**VENOM**, **RUST**, **AEGIS**, **REGEN**, **LETHE**) plus generated epithets; see `docs/PROCGEN_SIGILS.md`.
- **Procedural monster ecology (spawn theming)**: dungeon **terrain materials** and **room types** now apply mild, deterministic biases to the monster spawn tables — and certain affinity spawns can roll 1–2 nearby **nestmates** (shared groupId) — creating emergent ecology (spiders/snakes in dirt & moss, undead in marble/brick, kobolds in metal seams) without overriding content-mod spawn weights.
- **Procedural monster variants**: rare monsters now roll a persistent **rank** (Elite/Champion/Mythic) plus 1-3 **affixes** at spawn.
- **Procedural elite monster codenames**: Elite+ procedural variants now get deterministic two-word **codenames** (derived from their saved proc seed + rank/affixes/abilities) that surface in LOOK and in many combat/AI messages for clearer disambiguation and extra flavor.
- **Procedural monster abilities**: Elite+ monsters can roll 1–2 active abilities (pounce, toxic miasma, cinder nova, arcane ward, summoning, screech, void hook) with cooldowns, saved/loaded per-entity.
  - Affixes implemented this round: **Swift**, **Stonehide**, **Savage**, **Blinking** (panic blink reposition), and **Gilded** (bonus gold + higher key/drop odds).
  - New combat-proc affixes: **Venomous** (poison), **Flaming** (burn), **Vampiric** (life drain), and **Webbing** (ensnare).
- **New procedural monster affix: Commander**: Commander-marked elites project a small, line-of-sight **aura** that **inspires** nearby allies (+accuracy, a small damage bump at higher tiers, and faster fear recovery). LOOK shows **INSPIRED** on affected monsters.
  - LOOK/targeting now surfaces rank + affixes, and XP rewards scale with procedural rank/affixes.
  - HP pip color hints rank (hidden while hallucinating).
  - Rolls are deterministic per monster (seeded from its sprite seed + depth + room type) and are **saved/loaded** so variants survive reloads.
- Game-driven procedural particle FX events (spells + digging) via `FXParticleEvent` queue.
- **Procedural particle flipbook animation**: renderer-owned particle textures (spark/smoke/ember/mote) are now generated as **4-frame animated flipbooks** using looped noise + domain warp, with per-particle phase offsets so bursts don’t animate in perfect sync.
- **Procedural particle motion advection**: smoke/motes/embers now drift through a lightweight **curl-noise** flow field and are biased by the game’s deterministic per-level **wind**, yielding more fluid, coherent motion (visual-only).
- **Wind-drifted physical projectiles**: arrows/rocks/torches are now biased by the level’s deterministic wind (aim into the wind to compensate). The ranged targeting preview shows **WIND** and **DRIFT**, and will display **HIT 0%** when your aimed tile is no longer on the projectile line due to drift.
- **Procedural WFC vault prefabs**: new wall-pocket "ruin pocket" micro-vaults synthesized with the built-in Wave Function Collapse solver, validated for solvability, and mixed into vault prefab placement (see `docs/PROCGEN_VAULT_WFC.md`).
- **Procedural item micro-animations (4-frame)**: potions now slosh with looped-noise swirl (plus metallic/milky/murky/smoky variants), scroll labels shimmer, rings get orbiting specular glints, wands pulse with an energy crawl, and lit torches flicker with coherent embers.
- **Procedural sprite animation**: entities, items, and projectiles now use smooth, renderer-side procedural motion (move tween + hop/squash, idle bob, and hit recoil) to reduce "teleporty" feel in both top-down and isometric views.
- **High-resolution voxel sprite detail (64x64/128x128)**: voxel sprites now automatically increase their internal voxel-model resolution (nearest-neighbor voxel upscaling) before rendering, so large tile sizes produce smoother 3D voxel meshes in both perspective and isometric modes (isometric raytracing caps detail to stay performant).
- **3D turntable UI previews**: the **Monster Codex** and **Discoveries** overlays now include a large **auto-rotating 3D preview** (budget-cached). Unidentified items render an **appearance-based extruded turntable** so the NetHack-style identification game stays intact.
- **Voxel extrusion bevel smoothing**: 2D→3D fallback extrusion now uses a quasi‑Euclidean **8-neighborhood chamfer distance field** (plus tiny enclosed-hole filling) to drive thickness + biconvex bevels, reducing boxy “diamond” bevel artifacts and preventing accidental pinhole cavities from 2D dithering.
- **Voxel extrusion soft bevel alpha**: distance-field-driven fractional voxel coverage (alpha fade) near bevel cutoffs for smoother silhouettes and reduced 1-voxel popping at large tile sizes; see `docs/PROCGEN_SPRITE3D_SOFT_BEVEL_ALPHA.md`.
- **Isometric voxel terrain blocks**: optional voxel-generated wall/door/pillar/boulder **block** sprites in isometric view for better 3D cohesion with `voxel_sprites` (`iso_terrain_voxels`; toggle with `#isoterrainvox` / `#isoblocks`). Uses raytrace only for small tile sizes (<=64px) to avoid long stalls.
- **Expanded procedural surface camp (depth 0)**: the camp hub now generates a variable palisade yard with 2–4 packed tents/huts, an optional well + fire ring, clustered wilderness dressing, and a guaranteed walkable spine between EXIT and DUNGEON (with adaptive brush-clearing if trees/rocks block the route).
  - **Camp map size now varies per run** (compact/standard/sprawling) within the same safe bounds used by the dungeon generators, so the hub itself scales naturally with its palisade and wilderness dressing.
  - Camp gates now face the EXIT side, and the EXIT position varies per run for more distinct surface layouts.
  - Optional side sallyport gate + shallow ditch/moat (chasm ring) around the palisade (bridges preserved at gates).
  - Larger yards can generate 1–2 corner watchtowers, plus an internal reserved footpath network so later clutter doesn't block common routes.
  - Optional sacred grove POI outside the walls: standing stones around an altar, connected by a protected trail.
  - Camp landmarks now add extra map markers (WELL, FIRE, ALTAR, SIDEGATE).
- **Procedural rift cache pockets**: some floors now generate tiny optional wall pockets gated by a pushable **boulder** and a 1-tile **chasm** gap; pushing the boulder into the gap creates a rough bridge into a guaranteed **bonus chest** cache (via `bonusLootSpots`).
- **Procgen setpiece item hooks**: dungeon generation can now request specific guaranteed **ground items** (via `bonusItemSpawns`) so setpieces can reliably drop tools/rewards outside of rooms. Vault prefabs gained new glyphs: `K` (spawn a key) and `R` (spawn a lockpick), enabling a new "keyed inner vault" micro-puzzle and a secret tool cache. Dead-end stash closets can also drop small utility finds when no chest rolls.
- **Annex key-gate micro-puzzles (procgen)**: some annex wall-pocket micro-dungeons now generate an *internal* locked gate placed at an articulation point in the annex graph, with a guaranteed key spawn on the entry side and the chest reward deep behind the gate.
- **Annex WFC micro-dungeons (procgen)**: a new annex style uses **Wave Function Collapse** over a coarse connectivity lattice to synthesize compact "pipe network" micro-dungeons, then inflates some junctions into tiny chambers and occasionally adds 0–2 internal bulkhead doors.
- **Annex fractal micro-dungeons (procgen)**: a new annex style generates choice-dense pocket dungeons by interpreting a stochastic **L-system** (turtle + stack grammar) into a branching corridor "coral", then inflating junction chambers and budding small dead-end alcoves. (#mapstats: ANNEXES | FRACTAL)
- **RoomsGraph Poisson–Delaunay ruins (procgen)**: the RoomsGraph floor generator now places room centers using Poisson-disc sampling (blue-noise) and connects rooms via Delaunay triangulation → MST (+ extra loops) for more natural, planar-ish corridor graphs. (#mapstats: RUINSGEN | POISSON / PLACED / DT / LOOPS)
- **Metaball caverns (procgen variant)**: cavern floors can now optionally use a **metaballs/implicit-surface** field (quantile-thresholded) to create smoother, more organic cave silhouettes than cellular automata alone, while still keeping the largest connected component + fallbacks. (#mapstats: CAVERNGEN | METABALLS / KEPT)
- **Wilson mazes (procgen variant)**: maze floors can now optionally use **Wilson's algorithm** (loop-erased random walks) to generate a **uniform spanning tree** over the cell grid before the existing braiding + chamber carving pass, yielding a distinct “evenly random” maze texture. Exposes telemetry via `Dungeon` stats and `#mapstats` (`MAZEGEN | WILSON / ...`).
- **Symmetric room furnishing (new procgen pass)**: on some floors, 1–2 non-shop/non-shrine rooms get mirrored pillar/boulder (rarely chasm) "furniture" patterns with a reserved doorway-to-center navigation spine, so rooms stay traversable while looking more intentional. (#mapstats: FURNISH | SYMROOMS / SYMOBS)
- **Perimeter service tunnels (new procgen pass)**: on some room-based floors, the generator carves partial *inner-border* maintenance tunnels (offset=1 from the solid boundary) and punches 2–4 short door "hatches" back into the interior to create macro alternate routes (flanking / reduced backtracking), with an optional bonus cache chest. (#mapstats: PERIM TUNNELS | HATCHES)
- **Burrow crosscuts (new procgen pass)**: on some room-based floors, the generator uses an A* search biased toward **digging through wall mass** to carve 1–2 long crosscut tunnels between two far-apart corridor points, gated by doors (often locked/secret on one end). This creates dramatic optional shortcuts without tunneling through rooms or touching the stairs core route. (#mapstats: CROSSCUTS | CARVED | LOCKED | SECRET)
- **Secret crawlspace networks (new procgen pass)**: on some room-based floors, the generator carves a small hidden 1-tile-wide tunnel network entirely within wall mass, then connects it back to the corridor graph through 2–3 **secret doors**. Often contains an optional bonus cache chest deep inside. (#mapstats: CRAWLSPACES | CARVED | DOORS)
- **Fluvial erosion gullies (new procgen terrain pass)**: on some floors, the generator carves a few narrow **chasm trenches** that follow a deterministic **drainage network** over the macro heightfield (deeper downstream, sometimes widened), then preserves stairs connectivity by placing small **causeways/bridges** or rolling back. Avoids special rooms and reserves a short stairs-path halo. (#mapstats: TERRAIN FLUVIAL | GULLIES / CHASM / CAUSEWAYS; see `docs/PROCGEN_TERRAIN_FLUVIAL.md`).
- **Procedural VFX custom animation (4-frame)**: the renderer now samples **4 animation frames** for procedural hazard tiles (gas + fire), and cross-fades between frames with per-tile phase offsets so large fields don’t flicker in sync.
- **Field chemistry: reactive hazards**: **fire** now burns away lingering **gas clouds**, and dense **poison vapors** can occasionally **ignite** into a brief flash-fire explosion that consumes the cloud and leaves lingering flames.
- **Hearing preview LOOK lens**: a new audibility heatmap that marks explored tiles where your **footsteps** would be heard by any *currently visible* hostile listener (default **Ctrl+H**; **[ / ]** adjusts volume bias).
- **Scent trail preview LOOK lens**: a new smell-field heatmap + flow arrows that show your lingering scent gradient (default **Ctrl+S**; **[ / ]** adjusts the minimum scent cutoff).
- Fountain ripples now animate across all 4 frames using coherent traveling waves + looped fBm (no per-frame hash flicker), and item sprites (ground + UI icons) also get a small per-item phase offset to avoid synchronized blinking.
- **Procedural 2D sprite polish**: hue-shifted ramp shading for saturated colors, humanoid cloth panel patterns (with optional tiny capes) for more variety, and proto-silhouette connectivity cleanup to remove stray islands/holes.
- **Reaction-diffusion rune fields**: Arcane UI panels now get an animated “living circuitry” rune layer, and altars gain a subtle etched rune-glow pass — generated from a tiny Gray–Scott solver and animated via seamless domain drift.
- **Procedural cursor reticles**: LOOK and targeting cursors now use procedurally generated animated “marching ants” reticle overlays (square + isometric diamond), improving focus/readability at high zoom.
- **Procedural room ambiance decals (4-frame)**: lair floors now get an animated **biofilm shimmer** decal, and shrine rooms now get **rotating rune** decals (top-down + isometric).
  - The renderer de-syncs these animated decal styles per-tile so special rooms feel alive instead of flashing in perfect unison.
- **Procedural room-trim floor borders (autotiled)**: themed rooms now render a subtle style-colored "trim" border overlay keyed by a 4-bit N/E/S/W neighbor mask, in both top-down and isometric views (see `docs/PROCGEN_TILE_BORDERS.md`).
- **Ambient environmental particles (procedural)**: fountains now exhale cool mist puffs (plus rare water sparkles) and altars shed slow arcane motes tinted to your UI theme. These are visual-only and phase-driven so they stay stable across frame rates.
- **Ring of Searching**: a new ring that grants **automatic searching** each turn, helping you uncover nearby **traps, trapped chests, and secret doors** without spending an extra action.
- **Ring of Sustenance**: a new ring that grants **passive sustenance**, slowing **hunger loss** when the hunger system is enabled.
  - **Enchant/bless** increases potency; **curses** remove the benefit.
- **Scroll of Enchant Ring**: a new scroll that lets you enchant one of your rings (+ENCHANT).
  - If you have multiple rings, you will be prompted to choose; cancelling picks a random ring.
- **Shrine altars**: shrine rooms now spawn a visible **altar** tile (overlay on themed floor).
  - Press **Enter** while standing on the altar to invoke the existing **shrine prayer** interaction (including auto-identify when pious).
- **Obscure debug/info commands**: added a few extended commands for power-users:
  - `#pos [x y]` (`#where` alias): prints coordinates + depth + level size (uses the LOOK cursor when active).
  - `#what [x y]` (`#tile`/`#describe`/`#whatis` aliases): prints the same rich tile description used by LOOK mode.
  - `#mapstats`: prints quick floor stats (exploration %, rooms, monsters, items, traps, marks, engravings).
- **Stairs-path weaving (procgen)**: a new late-generation pass analyzes bridge (cut-edge) chokepoints on the shortest path between stairs and conservatively carves tiny 2x2 bypass loops to encourage alternate routes.
  - Surfaced in `#mapstats` as **BRIDGES** (remaining cut-edges on the shortest stairs path) and **BYPASSES** (loops carved this floor).
- **Global anti-chokepoint weaving (procgen)**: after stairs-path weaving, a second late pass now scans *all* bridge (cut-edge) chokepoints in the passable tile graph, ranks them by approximate cut-size (2-edge-connected component contraction), and carves a small number of conservative 2x2 bypass loops to reduce major chokepoints anywhere on the floor.
  - Surfaced in `#mapstats` as **GRAPH BRIDGES** (bridge count after weaving, with a WAS baseline) and **WEAVES** (loops carved this floor).
- **Generate-and-test floor picking (procgen)**: standard floors now generate **2–3 candidate layouts** (same generator kind) and pick the best by a deterministic quality score (stairs redundancy, path length pacing, density, and dead-end ratio).
  - Surfaced in `#mapstats` as **GEN PICK** (chosen attempt/score/seed).
- **Procedural VTuber figurines**: a rare treasure collectible with a deterministic stage name + archetype (from its sprite seed) and a new chibi portrait item sprite that blinks/animates subtly.
- **Procedural VTuber holo cards**: an additional VTuber collectible whose display name includes a deterministic rarity tier, with a new framed portrait card sprite.
- **VTuber holo card editions**: VTuber HOLOCARD drops now deterministically roll an **edition** (STANDARD/FOIL/ALT/SIGNED/COLLAB) from the card's sprite seed.
  - **FOIL** and **ALT** editions render with distinct holo/alt-art flair; **SIGNED** adds a tiny autograph scribble + serial ticks.
  - **COLLAB** editions feature **two** generated VTuber personas on one card and display both names in item labels and `#vtubers`.
- Shop economy: VTuber merch base value now scales procedurally by **rarity**, **edition**, and a coarse **follower band** (still with small deterministic shop variance).
- `#vtubers` (alias `#vt`): prints a compact roster of VTuber personas currently present (inventory + ground), including rarity, agency, tag, followers, and an optional catchphrase.
  - Added `#vtubers` (and `#vt`) to list VTuber collectibles currently present on the floor/in your inventory.

- **Infinite world (experimental)**: new settings `infinite_world` / `infinite_keep_window` allow descent beyond the normal bottom depth.
  - Depth == maxDepth remains a special "sanctum" floor, but it gains a downstairs in infinite mode so depth 26+ is reachable.
  - Deterministic per-depth generation (seeded from the run seed + depth) enables safe pruning/regeneration of deep floors.
  - Save version bumped to v51 to persist endless mode + keep window across reloads.
  - Trapdoors can now spawn on depth > maxDepth (except the sanctum), matching endless descent.
  - HUD shows "ENDLESS" for depths beyond the normal bottom.
  - Endless depths gradually bias generator selection toward more irregular layouts (maze/cavern/catacombs) while keeping occasional room floors for pacing.
  - **Endless strata (macro theming)**: depths > maxDepth are grouped into run-seeded bands (5–9 floors each) with a stable theme (Ruins/Caverns/Labyrinth/Warrens/Mines/Catacombs) that nudges generator choice + biome zone styles, announced on entry and surfaced in `#mapstats` as STRATUM.
  - **Endless map-size drift**: depths > maxDepth now vary map width/height smoothly (aligned to the same Endless Strata bands) so infinite descent isn’t stuck at one fixed layout scale forever.
    - Augury now previews the correct next-floor dimensions and uses the run seed for infinite-world generation, making the omen vision much closer to what you’ll actually face.

  - **Endless rift continuity**: endless depths can now spawn a run-seeded, stratum-aligned **faultline rift** (a curved chasm seam) that **drifts smoothly** across floors to create large-scale geological continuity.
    - Always preserves the guaranteed stairs path by adding natural **stone bridges** (rolls back if it can’t keep connectivity).
    - Surfaced in `#mapstats` as **RIFT** (ON/OFF, intensity, chasm tiles, bridges, and seed).

- **Finite campaign fault band continuity**: the fixed-length Main branch can now roll a run-seeded, multi-floor **FAULT** band (typically 3–6 floors) that carves a drifting chasm seam across those floors for cross-floor geological continuity.
  - Preserves the guaranteed stairs path by adding natural **stone bridges** (repairs with explicit bridges or rolls back if connectivity can’t be kept).
  - Surfaced in `#mapstats` as **FAULT** (ON/SKIP, band range/pos, intensity, chasm tiles, bridges, boulders, seed).

- **Biome zones (procgen)**: a new macro-structure post-pass partitions the *passable* map into **2–4 contiguous regions** (a Voronoi diagram over the passable graph) and applies coherent region styles:
  - **Pillar fields** (colonnaded ruins), **rubble gardens** (boulder clusters), and rare **cracked zones** (short chasm seams).
  - Never overwrites doors/stairs/special rooms, protects the main stairs path, and rolls back any region that would break stairs connectivity.
  - Surfaced in `#mapstats` as **BIOMES** (zone/style counts + edits).
- **Fire lane dampening (procgen)**: a new tactical post-pass measures the floor’s longest straight **projectile lane** and (when needed) inserts micro-cover to break extreme “sniper alleys” without harming stairs connectivity.
  - Prefers **barricade chicanes**: a **boulder** is placed in the lane *and* a tiny 1-tile side-step bypass is carved so movement remains smooth.
  - Falls back to dropping a single **cover tile** in open areas if chicane carving isn’t possible (always rolls back if stairs would disconnect).
  - Surfaced in `#mapstats` as **LANES** (max lane length + cover/chicane counts).
  - The generate-and-test floor score now softly prefers a **moderate** maximum lane length.
- **Open-space breakup (procgen)**: a new late-generation pass computes a simple distance-to-obstacle **clearance field** and (when the floor is too "wide open") drops a few **pillars/boulders** at clearance maxima to break up giant empty kill-box areas.
  - Always protects the shortest stairs path, avoids doors/special rooms, and rolls back any placement that would disconnect stairs.
  - Surfaced in `#mapstats` as **OPEN** (max clearance + pillar/boulder counts).

- **Moated room setpieces (procgen)**: treasure/shrine rooms can now occasionally generate a central **"island"** surrounded by a 1-tile **chasm moat**, connected by 1–2 narrow **bridges** for tactical variety.
  - Applied as a late post-pass and always rolled back if it would ever break the guaranteed stairs path.
  - Surfaced in `#mapstats` as **MOATS** (rooms, bridges, and chasm tiles).
  - The generate-and-test floor score now softly prefers a **moderate** max clearance.

- **Path-aware special-room placement (procgen)**: special room selection now understands the stairs **critical path**.
  - Rooms that intersect the shortest stairs path are labeled the **"spine"** and are preferred for **shops/shrines**, while **treasure/lairs** are biased **off-spine** to reward exploration.
  - Adds a soft minimum separation between selected special rooms to reduce clumping.
  - Surfaced in `#mapstats` as **SPECIALS** (counts + SPINE + MINSEP).

- **Perf overlay**: added a tiny optional debug HUD that shows **FPS/frame time**, sprite-cache stats, and a low-rate **determinism hash**.
  - Toggle via **Shift+F10** (default), `show_perf_overlay` in settings, or `#perf on/off`.

- **Safe-fire targeting**: ranged and offensive spell targeting now surfaces **friendly-fire / self-damage warnings** (line-of-fire and AoE), and requires a **second confirmation press** before committing a risky shot.

- **LOOK duel forecast**: LOOK mode now appends a compact **DUEL** estimate for visible non-allies (expected turns to **KILL** / **DIE** based on your current melee stats).
  - Accounts for the same d20 hit rules, armor damage reduction, weapon enchants, and sneak/ambush bonuses used by real combat.
  - The LOOK bottom-line now clips to the window width so long descriptions never run off-screen.
- **Item calling (NetHack-style)**: you can attach a short **CALL** label to unidentified potion/scroll/ring/wand appearances via `#call <label>`.
  - Uses the LOOK cursor (if active), otherwise your inventory selection, otherwise the first identifiable item underfoot.
  - `#call clear|none|off|-` clears the label; `#label` is an alias.
  - Labels persist in saves and render as a `{LABEL}` suffix on unidentified items (inventory/LOOK) and in Discoveries.

- **Procedural identification appearance phrases**: unidentified potion/scroll/ring/wand appearance labels are now procedurally generated per run (deterministic from run seed + appearance id).
  - Potions/rings/wands keep their base material word (e.g. **RUBY**, **OAK**) but gain a short descriptor (e.g. **BUBBLING RUBY**, **CARVED OAK**).
  - Scroll labels now form a 2–3 word incantation (e.g. **ZELGO VORPAL**), while staying safe for quoting in HUD/inventory.

- **Acoustic preview (Sound lens)**: LOOK mode can now toggle a **sound propagation heatmap** that shows where noise would travel (respecting door muffling) **without revealing unexplored tiles**.
  - Defaults to your current **real** footstep loudness (sneak/armor/encumbrance/substrate); adjust volume bias with `[` / `]` while in LOOK.
  - Outlines visible hostiles that would hear the sound (per-kind hearing).
  - Default bind: **Ctrl+N** (`bind_sound_preview` in settings; rebindable via `#bind`).
- **Per-level wind drafts**: each floor now has a deterministic **cardinal wind** (CALM/N/E/S/W) that gently biases **gas drift** and **fire spread** through corridors.
  - Announced once when a floor is first generated; check anytime with `#wind`.
- **Annex micro-dungeons**: some floors now carve a larger optional **side area** (a mini-maze, mini-cavern, or mini-ruins) into solid rock behind a door.
  - New annex style: **MiniRuins** packs several small rooms connected by corridors (MST + a couple extra loops) and places internal doors (some broken open) for a classic roguelike side-complex feel.
  - Doors can be **secret** or **locked**, and the annex always contains one or more **deep chest caches** to reward exploration.
- **Procedural per-run palette & patina**: terrain rendering now mixes the run seed into a cosmetic "style seed" to generate a gentle per-run palette and subtle per-tile value-noise shading (visual-only, deterministic).
- **Per-run isometric tileset**: isometric terrain assets (diamond floors, chasms, 2.5D blocks, decals, stairs overlays, and hazard VFX) now re-seed from the run’s cosmetic style seed, giving each run a distinct (but deterministic) isometric look.
- **Isometric cutaway**: optional 2.5D rendering aid that fades foreground walls/doors/pillars in front of the player (or LOOK/target cursor) so interiors stay readable.
  - Toggle in Options, via `#isocutaway`, or `iso_cutaway` in settings.
- Renderer build fix: removed an accidental duplicate depth-tint block introduced during palette work.
- Build fixes: corrected a missing room-type lookup in item spawning, and fixed an AI visibility helper call used by the Blink affix FX.


- **Isometric depth shading**: isometric view now adds **procedurally generated diamond edge shading** (contact shadows against walls/objects + subtle chasm rim) so the 2.5D map reads with more depth.
- **Isometric bevel lighting**: isometric diamond terrain tiles now apply a subtle **edge-only bevel ramp** (top-left highlight / bottom-right shadow) to reinforce the 2.5D ground plane.
- **Isometric themed floor diamonds**: themed floor tiles in isometric view are now generated directly in **diamond space** (no projection), keeping seams/cracks/planks aligned to the 2:1 grid and improving crispness at larger tile sizes.
- **Isometric chasm depth tiles**: chasms in isometric view now use a purpose-built **procedural diamond tile** (stone rim + shaded inner walls + swirling void core) so pits read with stronger depth, replacing the projected top-down chasm tile.
- **Isometric chasm gloom shading**: floor tiles adjacent to chasms now receive a subtle inward **gloom** darkening overlay in isometric view (beyond the rim band), making pits read deeper and edges feel more dangerous.
- **Isometric sprite grounding shadows**: entities (and hallucination phantoms) now draw a small **diamond ground shadow** under their feet in isometric view, improving depth cues and reducing the “floating sprite” effect.
- **Isometric voxel sprites**: when `voxel_sprites` is enabled in isometric view, entities/items/projectiles now render via a **mesh-based isometric voxel rasterizer** backed by a tiny **procedural 2D mesh** engine (merged visible faces -> 2D triangles -> z-buffered sprite), so they visually align with the 2.5D terrain and translucent voxels (glass/liquid) sort more robustly.
- **Isometric voxel raytracer (optional)**: add `iso_voxel_raytrace` to render isometric voxel sprites using a custom **orthographic DDA raytracer** (instead of the face-mesh rasterizer).
  - Toggle at runtime via `#isoraytrace on/off/toggle`.
- **Isometric cast shadows**: floor tiles in isometric view now receive subtle **directional cast shadows** from nearby tall blockers (walls/closed doors/pillars/etc), improving 2.5D depth cues.
- **Procedural isometric sun direction**: isometric view now chooses a deterministic per-run diagonal light direction (NW/NE/SE/SW) and rotates **cast shadows** + **entity ground-shadow offsets** accordingly, with a short multi-tile soft shadow reach that stops at unknown tiles (no map leaks).
- **Top-down wall contact shadows**: floor tiles now receive subtle **ambient occlusion** shading along edges adjacent to wall-mass terrain (walls/closed doors/pillars) and tall props (boulders/fountains/altars), improving depth cues and readability.
- **Isometric floor decals**: themed floor decal overlays (cracks/runes/stains) are now generated directly in isometric diamond space, keeping thin lines crisp and patterns aligned to the 2.5D grid.
- **Isometric environmental overlays**: gas and fire floor hazards now generate directly in isometric diamond space (instead of projecting square sprites), so animated VFX align cleanly to the 2.5D grid. Poison gas now uses the isometric diamond variants as well.
- **Isometric door blocks**: closed and locked doors now render as **procedurally generated 2.5D door blocks** in isometric view, matching the wall block style (wood panels inset into stone faces) instead of using flat top-down overlay sprites.
- **Isometric block surface texture**: isometric wall/door/doorway blocks now add subtle procedural **brick seams** on stone faces and **wood grain** on door panels for richer, less-flat 2.5D visuals.
- **Isometric open door frames**: open doors now render as a **procedurally generated 2.5D doorway frame** in isometric view (a passable archway), so doorways keep their vertical depth instead of relying on a flat floor overlay.
- **Isometric stairs depth overlays**: stairs up/down now use a purpose-built **procedural diamond overlay** (rim + interior shading + step lines) so stairwells read with more depth in 2.5D view.
- **Isometric prop blocks**: pillars and boulders now render as **procedurally generated 2.5D blocks** in isometric view, matching the wall/door block style instead of relying on flat top-down overlay sprites.
- **Procedural mines floors**: a new dungeon generator that builds **winding tunnel networks** connecting many small chambers.
  - Appears at **depth 2** and again at **depth 7**, adding mid-run navigational variety.
  - Tunnels are carved with a **biased random-walk** for more organic shapes (with safe fallback pathing).
- **Global ravines / fissures**: some procedural floors can now spawn a long, winding **chasm fissure** that slices across the map.
  - The generator always preserves stairs connectivity (by placing natural stone bridges, or repairing if needed).
  - **Deep Mines** are guaranteed to have at least one fissure, and may also spawn extra **boulders** near the edge for optional bridge-making.

- **Sinkholes**: a late procedural pass that carves small, irregular **chasm clusters** into corridors/tunnels on deeper floors.
  - Designed as local navigation puzzles (levitation, boulder-bridging, or detours) while always preserving the core stairs path.
  - **Deep Mines** now feel extra unstable: they are guaranteed to generate at least one sinkhole cluster, often with nearby boulders.


- **Catacombs floor**: depth **8** now uses a new generator that creates a dense grid of small tomb rooms connected by a **cell-maze** (spanning tree + extra loops).
  - Emphasis on **doors**, short sight-lines, and frequent junctions for more tactical room-to-room play.

- **Grotto lakes**: the cavern-like floor at **depth 4** now carves a blobby **subterranean lake** (chasm terrain) with automatic **stone causeways** placed via BFS to preserve stairs connectivity.
  - The generator also sometimes places a few **boulders** near the shoreline as optional bridge-making tools.

- **Bonus room prefabs**: vaults and secret rooms now gain internal layouts (moats/trenches/pillar lattices) to make side objectives feel more distinct.
  - Some layouts generate **boulder-bridge** puzzles over chasms and request extra **bonus loot** caches in hard-to-reach pockets.
  - Vault room sizes scale gently with depth to support more interesting shapes.
- **Vault suites**: a new advanced vault prefab that partitions some vault rooms into multiple locked-in chambers using interior **walls + doors**.
  - Creates short "mini-dungeon" side objectives with staged sight-lines and better tactical pacing.
  - The generator tracks how many suites were placed via `Dungeon::vaultSuiteCount` (useful for tests/callouts).


- **Themed rooms**: **Armory**, **Library**, and **Laboratory**. These appear as an extra "moderate" special room on many floors, biasing spawns toward weapons/armor, scrolls/wands, and potions respectively.
  - Laboratories can also generate **extra volatile traps** inside them (confusion gas / poison darts / etc.) for flavor.
  - New: the dungeon generator now adds **themed interior prefabs** (armory weapon racks + crates, library shelves + aisles, laboratory spill hazards + lab benches).
    - Some layouts can request a rare **bonus loot cache** tucked deep behind the furniture.

- **Room shape variety**: some **normal rooms** now get internal wall partitions (L-shaped alcoves, donut blocks, and partition walls with occasional inner doors).
  - Designed to increase tactical line-of-sight variation while keeping the global stairs path intact.

- **Procedural map sizes**: newly generated floors now vary in **width/height** by depth (with mild jitter), adding navigational variety.
  - Bespoke/puzzle floors (Sokoban/Rogue/labyrinth/sanctum) keep the canonical size.
  - The game now prints `LEVEL SIZE: WxH` when a floor is generated for the first time.

- **Merchant guild pursuit**: stealing from shops can now trigger a guard response that may follow you across floors.
  - Shop debts can be paid across depths, and clearing all debt causes guards to stand down globally.
  - Save format bumped to **v42**.

- **Secret shortcut doors**: some procedural floors now add a small number of **hidden** doors (secret doors) in corridor walls where adjacent passages run alongside each other.
  - These create optional **loops/shortcuts** that reward searching, without carving disconnected "hidden corridor" pockets.

- **Locked shortcut gates**: some procedural floors now add a small number of **visible locked doors** in corridor walls that connect two adjacent corridor regions already connected elsewhere.
  - These create optional **key/lockpick-powered shortcuts** without ever blocking stairs progression.

- **Inter-room doorways (procgen)**: some room-based floors now carve 1–3 direct doors through single-wall separations between *adjacent rooms*, but only when the existing route between the two rooms is long enough to make the new doorway a meaningful shortcut.
  - Doors can be **closed**, **open**, or (deeper) rarely **secret/locked**. (#mapstats: INTERROOM DOORS)

- **Corridor hubs & great halls**: a late procedural pass that widens some hallway junctions and long corridor runs into broader spaces (junction hubs + 2-wide halls, with rare 3-wide flares).
  - Adds tactical variety outside formal room rectangles, without creating door-less openings into rooms.

- **Ethereal bones ghosts**: ghosts spawned from **bones files** can now **phase through walls/doors**.
  - Their chilling touch can also briefly **disorient** the player.

- **Potion of Levitation**: grants temporary **LEV** status.
  - While levitating, you can **traverse chasms** and **float over** certain floor traps (spikes/webs).
  - If levitation ends while you’re above a chasm, you’ll **fall**, take damage, and scramble back onto solid ground.

- **Rolling boulder traps**: rare floor traps that release a boulder which **rolls in a straight line**, potentially **crushing** anything in its path.
  - The boulder can sometimes **smash open doors** and can **fill chasms** (turning them into floor).
  - After triggering, the trap is **spent**, leaving the boulder as a new obstacle (or consuming it if it falls into a chasm).

- **Shop debt ledger**: consuming/destroying unpaid shop goods now still leaves you owing the shopkeeper (and **#pay** can pay it down).

- **Shrine service improvements**: shrine prayers that target a specific item now open a **modal inventory prompt** so you can choose the target.
  - **#pray identify**: choose which unidentified item kind to identify (ESC still picks a random unknown).
  - **#pray bless**: choose which item to bless/uncurse (ESC blesses your currently equipped gear, as before).
  - New: **#pray recharge**: restores charges on a depleted wand (ESC picks the most depleted).

- **Scroll of Fear**: reading it scares **visible hostile monsters**, applying a temporary **FEAR** status that makes them prioritize fleeing.
  - Mindless/undead monsters are immune.

- **Scroll of Earth**: reading it makes the dungeon shake, raising **boulders** in the 8 surrounding tiles.
  - It can also **fill adjacent chasms** and may pelt adjacent hostile creatures with falling rock.

- **Scroll of Taming**: reading it can **charm** nearby creatures into becoming **friendly companions**.
  - NetHack-inspired quirk: while confused, the effect expands to an 11x11 area (chebyshev radius 5).
  - Undead, shopkeepers, and the Minotaur are immune.

- **Companion order improvements**:
  - **FETCH** companions now pick up **gold and useful loot** (never shop stock), carrying it back and delivering/dropping it when adjacent to you (still drops carried loot on death).
  - **STAY/GUARD** now remember an anchor tile so companions can **return** after chasing enemies; **GUARD** will also lightly **patrol** near its anchor.

- **Targeting / ranged QoL**:
  - While aiming, **TAB / SHIFT+TAB** cycle **shootable** visible hostile targets.
  - The targeting HUD now shows a compact **hit chance** + **damage dice** preview for your current shot.
  - Invalid targeting states are now explained more clearly (e.g., **OUT OF RANGE**, **TARGET NOT VISIBLE**, **NO CLEAR SHOT**).
  - **Boulders** now block projectiles (even though they remain non-opaque for line-of-sight), and projectiles can no longer "cut" perfectly blocked diagonal corners.

- **Hallucination improvements**:
  - While hallucinating, **look/target** text now reflects the same scrambled monster/object types you see on the map.
  - New: hallucination can also manifest as fleeting **phantom monsters** on empty visible tiles.

- **Rogue homage floor**: depth **6** is now a classic 3x3-room "Rogue level" with **doorless corridors** for a distinctly different tactical feel.
  - Entering it prints: `YOU ENTER WHAT SEEMS TO BE AN OLDER, MORE PRIMITIVE WORLD.`

- **Trap doors**: rare floor traps that drop you to the **next depth**, dealing **impact damage** on landing.
  - While levitating, you can **float over** trap doors.
  - Creatures (including **friendly companions**) that fall through a trap door can now also **tumble to the level below** and may show up later (taking some fall damage).

- **Corpse revival**: **stale corpses** left on the ground may **rise once** as hostile undead.
  - Special cases: **troll/slime/mimic/wizard corpses** can sometimes reanimate into their original creature.
  - Rising is **noisy** and can wake nearby monsters.

- **Item mimics**: rare high-value loot (outside of shops) may actually be a **Mimic**. Attempting to pick it up reveals an ambushing Mimic that drops the original item on death.
  - Weighted toward **Treasure/Vault/Secret** and themed rooms (Armory/Library/Laboratory).
- **Mimic stickiness**: Mimic attacks can briefly **stick** you in place (applies the existing **WEB** status).

- New monster: **Zombie** (slow, tough undead).
  - **Undead** (Ghost/Skeleton/Zombie) are **immune to poison damage**.

- **Trap setpieces**: some floors now generate small **corridor gauntlets** (short strips of traps in long straight corridors).
  - **Bonus loot caches** requested by the dungeon generator may also be guarded by one or more nearby floor traps.
  - **Traffic traps**: some floors now place 1-2 traps on *high-traffic* corridor junctions (estimated by sampling shortest paths between stairs and special rooms).

- **Monsters can drink potions:** Wizards may now spawn with a pocket potion and will sometimes drink it mid-fight (heal, regen, shielding, invisibility, levitation). Levitation now also enables monster pathing across chasms.

- **Shrine piety + offerings:** shrine services now cost **PIETY** and have a cooldown.
  - Earn PIETY by donating gold (**#donate**) or offering corpses (**#sacrifice**) at a shrine (or at the surface camp).
  - The HUD now shows your current PIETY and remaining prayer cooldown.

- **Procedural particle VFX engine**: renderer-owned, procedurally generated spark/smoke/ember textures with lightweight simulation; used for hits, explosions, fire-field embers, and projectile trails.

- **Procedural rune spells (foundation)**: added a deterministic rune-spell generator (`src/proc_spells.hpp`) that derives spell names + gameplay parameters from a packed 32-bit id (tier + seed), plus unit test coverage.

- **Procedural trap salvage**: successful disarms can yield deterministic **Essence Shards** based on the trap's type (STONE/VENOM/RUNE/ARC/SHIELD/DAZE/CLARITY/ALCH), rewarding careful trap work without consuming extra RNG; see `docs/PROCGEN_TRAP_SALVAGE.md`.


- **Procedural shop identities**: each shop room now gets a deterministic **name**, **shopkeeper name**, and **personality** derived from the run seed + room geometry.
  - Shop personalities apply small, theme-aware buy/sell price biases (armory/magic/supplies/general).
  - Entering a shop announces the shop name + greeting, and the HUD shows the current shop name while inside.
  - See `docs/PROCGEN_SHOPS.md` for details.

### Save compatibility
- Save version bumped to **v43** (v41 adds `Item.flags`; v42 adds merchant guild pursuit; v43 adds shrine piety + prayer cooldown).

- Added `tag32("...")` compile-time seed salt helper (FNV-1a) in `src/rng.hpp` for readable domain separation when mixing deterministic procedural seeds.
### Changed
- Crafting recipe sigils/seeds now incorporate ingredient fingerprints (kind + packed metadata) for more variety and more meaningful material use.
- Sound alerts now use deterministic **localization uncertainty**: quiet or distant noises no longer pinpoint the exact source tile, so monsters may investigate an approximate area (loud/near noises remain effectively exact).
- WFC solver: upgraded `src/wfc.hpp` with bounded DFS backtracking (fewer full restarts) and a per-attempt RNG stream for more stable procgen; added WFC unit tests.
- Procedural entity/projectile flipbooks: added sprite-space idle warps (slime squash & stretch, bat wing flap, snake slither, spider scuttle, wolf/dog tail wag) and replaced per-frame sprite shading jitter with a seamless looping shimmer; projectile arrow/rock now have 4-frame glint/tumble cues.
- **3D voxel sprite extrusion**: improved the 2D→3D fallback with subtle **bas-relief depth modulation** (luminance-driven thickness), **accent-preserving albedo flattening**, and a cleaner 3D source pipeline that avoids voxelizing 2D drop shadows/outlines; see `docs/PROCGEN_SPRITE3D_RELIEF_EXTRUSION.md`.
- **3D voxel sprite shading**: material-aware specular/rim + emissive lift, plus a high-res surface chamfer/alpha-feather post-pass for smoother silhouettes when detail scaling is enabled; see `docs/PROCGEN_SPRITE3D_MATERIAL_CHAMFER.md`.
- **3D voxel sprite volume**: upgraded the 2D→3D extrusion fallback with **shape-adaptive depth budgeting** (silhouette distance-to-edge radius) and a **biconvex bevel profile** (front+back taper) so extruded sprites read more like small sculpted tokens under lighting; see `docs/PROCGEN_SPRITE3D_BICONVEX_BEVEL.md`.
- **3D voxel sprite texturing**: extruded sprites now use a thin front **texture shell** + palette-derived **material fill** so ink/outline pixels stay surface-bound and side walls stop reading as "striped" columns; see `docs/PROCGEN_SPRITE3D_TEXTURE_SHELL.md`.
- Procedural VFX flipbooks: **confusion/poison gas** and **fire** overlays now apply a lightweight **curl-noise flow warp** (divergence-free) before the existing domain-warp masks, producing more "advected" motion and richer turbulence in both top-down and isometric views.
- **Dungeon length**: increased the default run from **20** to **25** floors.
- **Dungeon generation**: the main branch is now **fully procedural** by default for depths **1–25** (no fixed handcrafted set-piece floors).
- Overworld wilderness generation: cached per-tile elevation/moisture/variance noise fields so multi-pass chunk procgen (terrain + rivers) no longer recomputes multi-octave FBM, improving surface chunk generation performance.
- Extended command prompt: **TAB completion** now extends to the **longest common prefix** and lets you **cycle** through ambiguous matches by pressing **TAB** repeatedly (also works for pasted NetHack-style `#cmd` inputs).
- Extended command prompt: upgraded to a proper **caret editor** (LEFT/RIGHT, HOME/END, DEL forward-delete) with familiar shell/Readline shortcuts: **Ctrl+A/E** home/end, **Ctrl+W** delete previous word, **Ctrl+U** clear to start, **Ctrl+K** clear to end; plus **Ctrl+L** clears the whole line (consistent with filter UIs).
- Extended command prompt: TAB completion now shows an in-overlay **dropdown match list** with the active selection highlighted; added a default **Ctrl+P** binding to open the prompt when `#` is awkward on some layouts.
- Extended command prompt: completion dropdown now shows **keybinding hints** for common commands (save/load/options/help/etc) and scrolls to keep the active selection visible; TAB completion falls back to a **fuzzy subsequence match** when there are no prefix matches; added additional Readline-style shortcuts inside the prompt (**Ctrl+B/F** move caret, **Ctrl+P/N** history, **Ctrl+D** forward delete).
- Extended command prompt: unknown commands now show a short **DID YOU MEAN** suggestion list to make typos less punishing.
- Extended command prompt: TAB completion is now **context-aware** for common argument slots: `bind`/`unbind` completes **action names** (from the shared Action registry), and `preset`/`autopickup`/`identify`/`mortem` complete their common option values.
- Inventory overlay: the left-hand list now shows compact **quick-compare** badges (ATK/DEF/RA/RN deltas + ring mods) so upgrades are visible at a glance; fixed gear stat comparisons to include **bless/curse** modifiers.
- Keybinds editor overlay: added an in-overlay **filter/search** (press `/`), **DEL** to unbind (writes `none`), and highlighted **conflicting** duplicate chords.
- Keyboard input: SDL key repeat is now honored for movement and menu navigation (hold a direction to keep moving; repeats are still suppressed for toggles/confirm/ESC to avoid accidental spam).
- Keyboard input: added a NetHack-style **COUNT prefix** — type digits then a repeatable action (move/wait/search/evade/stairs) to auto-repeat it; **Backspace** edits the count and **Esc** clears/cancels.
- **#whistle** now calls **all friendly companions** with **Follow/Fetch** orders (not just the starting dog).
- Auto-travel: you can now **target locked doors** directly (when you have keys/lockpicks), making it easier to walk to vault doors intentionally.
- Auto-explore: when no normal frontiers remain, it will now **walk to reachable locked doors** that border unexplored tiles (if you have keys/lockpicks) so it doesn't incorrectly report the floor as fully explored.
- Auto-move hazard awareness: **auto-travel** now strongly prefers routes that avoid **confusion/poison gas** tiles, and **auto-explore** will not path through gas clouds (similar to fire) until they dissipate.
  - Auto-move also stops immediately if you end up standing in a gas cloud (even before the status effect lands), preventing accidental turn-burning in hazards.
- Corrosion hazard upgrades: **Corrosion** can now *pit equipped gear* on damage ticks (rare **-1 enchant**, blocked by Shielding; blessed resists; cursed vulnerable; artifacts immune), and very strong **Corrosive Gas** can slowly **erode locks/doors** (locked→closed, closed→open); see `docs/CORROSION_GEAR_AND_DOORS.md`.
- Auto-travel (Sneak): when **Sneak** is enabled and hostiles are visible, auto-travel/auto-explore path planning now incorporates the dungeon's **sound propagation** model + monster hearing deltas to prefer routes where your **footsteps won't be heard**.
- Sound/hearing integration: visible-hostile audibility is now computed as a single **multi-source seeded Dijkstra field** (tile→listener), improving performance and making muffling doors/locks model **directionally** (no more listener→tile approximation).
- Floor trap scatter now avoids **shops** and **shrines** (keeps safe spaces consistent; traps still appear elsewhere).
- Darkness lighting now treats **burning creatures** and **flaming ego weapons** as small moving light sources (in addition to torches, room ambient light, and fire fields).
- Monster pathfinding now treats **poison gas** and **discovered traps** as higher-cost tiles, so monsters (and pets) will route around known hazards when practical.
- Ranged monster tactics: ranged attackers now avoid shooting through **allied** monsters (and peaceful shopkeepers), and will try to **sidestep** for a clear shot instead of friendly-firing.
- Ranged monsters now require a clear projectile line-of-fire before shooting (cover like boulders/corners no longer causes wasted ranged attacks), and will strafe for a firing lane when possible.
- Procedural terrain visuals: floor/wall/chasm variant selection now uses low-frequency coherent noise (less "TV static", more natural patches).
- Floor decals now use jittered-grid placement to reduce clumping; wall stains avoid adjacent clusters for a cleaner, more deliberate look (top-down + isometric).
- Isometric floor decals are now generated directly in diamond space (instead of projecting top-down squares), keeping thin cracks/runes crisper and better aligned to the 2.5D grid.
- Isometric VFX overlays: confusion/poison gas and fire field overlays now generate directly in diamond space in isometric view (no square projection), keeping the animation aligned to the 2:1 grid; poison gas now also uses the isometric textures.
- Isometric UI polish: auto-travel path overlay now renders as a faint polyline with diamond pips; trap/marker indicators snap to the diamond grid; explosions render in diamond space in isometric view (prevents square bleed), and a lightweight hover-inspect tooltip appears when no modal UI is open.
- Procedural VFX overlays: **confusion/poison gas** and **fire fields** now use **domain-warped fBm** masks for more wispy motion (less blobby), with subtle **spark** highlights in fire; still uses ordered dithering to stay crisp in pixel-art.
- Procedural animation generation: upgraded all major 4-frame flipbooks to use **seamless looping domain drift** (reduces the frame-3 → frame-0 “pop”), improved chasm void motion, added ghost row-warp drift, and refreshed UI tiles + HUD status icons with coherent 4-phase micro-animations. (See `docs/PROCEDURAL_ANIMATION.md`.)
- Voxel sprites: improved 3D voxel sprite shading with hemisphere-based ambient occlusion (reduces over-darkening on flat surfaces) plus a softer drop shadow for better grounding.
- Isometric voxel sprites: improved per-face lighting (directional diffuse + cheap AO + subtle spec/rim) so voxel_sprites read with more depth/volume in 2.5D view; shading is quantized to keep greedy face merges stable.
- Isometric wall blocks now pick variants coherently along wall segments, improving brickwork continuity in 2.5D view.
- Isometric ground plane lighting: isometric diamond terrain tiles now apply a subtle whole-tile directional ramp (top-left brighter / bottom-right darker) for better 2.5D depth, and cast shadows now feature stronger corner occlusion with caster-type strength scaling.
- Isometric block sprites: wall/door/doorway/pillar block sprites now get subtle vertical-face ambient occlusion (under-cap overhang, ridge seam, and base grounding) plus a light-facing cap rim highlight, improving volume/readability in 2.5D view.
- Renderer: special overlay tiles (stairs, altars, fountains) now get a subtle deterministic **glint** modulation (scaled by proc_palette strength) so interactables pop without breaking pixel-art.

### Fixed
- Fixed a proc-spell Shadow invisibility duration bug (typo: `invisibilityTurns` -> `invisTurns`).
- Direct kill tracking is now incremented when the player lands the killing blow, making pacifist conducts/victory plans accurate.
- Fixed MSVC build break in the inventory renderer: artifact effect tag now calls `artifactgen::powerTag(...)` (correct signature).
- Forced movement now recomputes FOV immediately (knockback, door-smash, and forced pulls), preventing stale vision after being shoved around.
- Rune Tablet targeting now matches display-name generation for legacy tablets (spriteSeed=0), ensuring the spell you see is the spell you cast.
- Magic shop item table thresholds are now fully reachable within the 0..99 roll (previously some high-end outcomes were unreachable).
- Monster timed-effect timers now tick correctly for enemies as well (regen/shield/invisibility no longer last forever if a monster uses a potion or ability).
- Shielding now contributes to **monster damage reduction**, making shield potions and Arcane Ward meaningful.
- Fixed MSVC build break: LOOK/targeting HUD no longer references an out-of-scope `fitToChars` lambda (shared helper lives in render.cpp).
- Fixed MSVC build break: LOOK info overlays (sound/hearing) no longer reference an undefined `idx` helper; grid indexing is now local and bounds-checked.
- Fixed MSVC build errors in heightfield terrain decoration: ridge pillar spacing now checks against actual placed pillars (no stray out-of-scope identifier).
- Targeting + LOOK bottom hint text now uses a compact **two-line** layout and a middle-ellipsis fitter, keeping both context and keybind hints readable on narrow windows.
- Removed MSVC C4244 warnings by passing properly typed `Uint8` color channels to `SDL_SetRenderDrawColor` and by using `uint8_t{0}` for byte-grid fills/initialization.
- Fixed MSVC build errors in monster AI procedural abilities (undefined dungeon member, incorrect FOV mask type, and undefined dimensions) and reduced per-cast allocations by reusing a scratch FOV mask for hazard seeding.
- Cleaned up additional MSVC warnings: shadowed `rng`/`fx`/debug locals, unused dungeon-generator parameters, and corrected `pushFxParticle` argument ordering in AI ability FX calls.
- Fixed MSVC build breaks in the renderer: procedural palette depth normalization now uses `Game::depth()`/`Game::dungeonMaxDepth()` locals, and overlay tile tinting no longer references a missing helper.
- Fixed MSVC build breaks in sprite generation: VTuber holo card border drawing now uses the existing `outlineRect` helper.
- Fixed a build break in LOOK/renderer UI helpers by exposing the `equippedMelee/Ranged/Armor/Ring1/Ring2` accessors publicly.
- Shops now correctly spawn a **Shopkeeper** (enabling **#pay** and selling), and shop rooms no longer spawn random monsters.
- Shrine item-targeting prompts (**Identify/Bless/Recharge/Sacrifice**) now resolve properly from the inventory UI.
- Fixed build-breaking errors in the merchant guild theft-alarm helper.
- Test/headless builds no longer require SDL2: **keybinds** are compiled only for the game executable.
- Fixed build-breaking pointer dereferences in **Disarm Trap** and **Shrine prayer** interactions.
- Fixed MSVC build breaks/warnings: implemented missing chest overlay item icon helper, corrected keybind chord formatting, and centralized SDL includes to avoid SDL_MAIN_HANDLED macro redefinitions.
- Fixed MSVC build errors/warnings in renderer + procedural spritegen: declared `resampleSpriteToSize()` in the public spritegen header, added a `rand01(uint32_t)` hash-to-float helper, corrected an isometric `pointInIsoDiamond()` callsite, avoided shadowed locals in render UI preview blocks, exposed `Renderer::FRAMES` for flipbook consumers, and added `ScoreEntry.conducts` (recorded + persisted in the scores CSV).
- Fixed MSVC build break in minimap cursor tooltip: expose Game::describeAt and remove shadowing warnings in save migration + isometric phantom rendering.
- Fixed MSVC build errors in fishing targeting preview (rod lookup + local HUD stream) and corrected an `emitNoise` call to match its signature.
- Fixed MSVC build error for `#bounty`: `Game::showBountyContracts()` is now public.
- Cleaned up MSVC shadowing warnings (C4456/C4458) in combat XP progression, dungeon door placement, and `#pet` status output.
- Keybind parsing now **deduplicates equivalent chords** (e.g. `?` and `shift+slash`) so bindings don't “double up” in the UI.
- Keybinds now treat the **CMD/GUI** modifier as a first-class bindable modifier (INI + editor), and the default help bind includes **cmd+?**.
- Keybind parsing now robustly accepts literal `<`, `>`, and `?` tokens even when SDL can't map those shifted symbols back to scancodes (helps config-file editing across layouts).
- StairsDown defaults now also bind to **SHIFT+<** for keyboards where `<`/`>` share a dedicated key (so `>` works on that same physical key + Shift).
- Keybinds + Help overlays now render friendlier chord strings (e.g., **CMD+?**, **<**, **>**, **ENTER**) instead of raw token names.
- UI panels now pick up subtle deterministic per-run variation (cached) to enhance the procedural GUI feel without impacting readability.
- HUD text now **wraps** (stats, message log, and bottom hint lines) instead of clipping off the right edge on narrow windows.
- Help overlay now **wraps** long lines and supports **scrolling** (Up/Down, PageUp/PageDown, mouse wheel) so content never clips off-screen.
- Message history overlay now supports **PageUp/PageDown paging** (10 lines) and highlights **search matches**.
- Message history overlay now **wraps** long messages (no more right-edge truncation) and supports **CTRL+C** to copy the filtered history to the clipboard.
- Mouse wheel scrolling is now **context-sensitive**: it scrolls selection in list-based overlays (inventory/chests/spells/options/codex/discoveries/scores) and enables the minimap’s intended **quick-scroll** behavior (LOG UP/DOWN moves the cursor by 10 tiles instead of scrolling the message log).
- Auto-explore: frontier selection now respects diagonal "corner-cut" movement rules, avoiding unreachable diagonal targets that could prematurely end exploration.
- LOOK-mode Threat Preview + sound propagation now share the same Dijkstra grid helper (no duplicated priority-queue implementations).
- Threat Preview ETA is now computed as a true *enemy-to-tile* travel cost field (forward multi-source Dijkstra), improving correctness around door/lock movement costs.
- Threat Preview + monster AI now share a single monster pathing policy (doors, chasms, phasing, hazards, discovered-trap avoidance), fixing hybrid cases like levitating door-smashers.
- Centralized the "heavy monsters can bash locked doors" rule via `entityCanBashLockedDoor()` so monster AI and the UI preview cannot drift.
- Auto-travel no longer cancels immediately just because a hostile is visible: it now stops only when a visible hostile could reach you (or the next step tile) within a short ETA window, and otherwise continues on a cautious route.
- Auto-travel path planning is now **threat-aware**: it reuses the same visible-hostile ETA field as LOOK Threat Preview to bias routes away from enemy influence (while still respecting locks/doors, levitation chasms, gas/fire, and known-trap penalties).
- Auto-travel pathfinding now precomputes a discovered-trap penalty grid once per path build, avoiding O(traps) scanning in the Dijkstra inner loop.
- New tactical helper: **Evade** (default **CTRL+E**, `#evade`): chooses a best-effort single-step retreat using the same visible-hostile Threat ETA field + hearing/audibility field, avoiding known traps/hazards when possible.
- Save loading now automatically attempts rotated backups (`.bak1..bakN`) when a primary save/autosave is corrupt (truncated/CRC mismatch), improving recovery after crashes.
- Extended-command integration: centralized alias normalization so legacy spellings/synonyms (#go/#goto, #ledger, #perf_overlay, #iso_raytrace, etc.) resolve to their canonical commands *before* prefix matching/completion.
- Fixed several previously unreachable extended commands by bringing the canonicals into the completion/matching list: **#perf**, **#debt** (with #ledger alias), and **#isocutaway** (with #cutaway alias).
- Command prompt TAB-completion now carries per-command metadata (keybind token + short description) so the dropdown can show a one-line **INFO** description for the current selection while keeping keybind hints accurate and de-duplicated.
- Consolidated canonical **Action token** metadata into a shared, SDL-free registry (`action_info.hpp`), so keybind parsing and extended command `#bind/#unbind` stay perfectly in sync.
- Keybinds overlay now shows a contextual **INFO** description for the currently selected action (pulled from the same registry).
- Fixed MSVC build errors/warnings in procedural biolum lighting + fluvial terrain: replaced an invalid hex literal suffix used for a seed salt, and removed C4456 shadowing warnings in the gully connectivity repair logic.
- Fixed MSVC build break: replaced invalid mnemonic hex seed salts in biolum ambient particles and room-trim floor border overlays with compile-time hashed tags (`"..."_tag`).


- Fixed MSVC build errors in inventory/butchering: use `Effects.confusionTurns`/`hallucinationTurns`, remove stale `Item.ownerId`, and respect `equippedMelee()` constness.
- Added `FXParticlePreset::EmberBurst` (renderer + preset name) and wired it to EMBER-tagged meat burn FX for a punchier fire burst.
- Fixed MSVC build break in artifact crafting preview by adding `artifactgen::powerDesc(...)` (long-form tooltip text).
- Added `Room::contains(Vec2i)` overload so interaction code can pass positions directly (and fixed a minor overworld lambda shadowing warning).
- Crafting outcomes now store essence tag pairs in a canonical (order-independent) order, matching recipe hashing and restoring determinism tests.


## [0.20.6] - 2025-12-27

### Added
- **Throw-by-hand fallback** for ranged attacks: if you don’t have a *ready* ranged weapon, you can still use **Fire** to **throw a Rock/Arrow** (if you have ammo).
  - Rocks are preferred when both are available.
  - Throw range scales slightly with your attack stat (as a simple stand-in for strength).
- Targeting UI now indicates whether you’re **firing** an equipped weapon or **throwing** a projectile.

## [0.20.5] - 2025-12-27

### Added
- **Monster tracking (last known position)**: monsters now hunt toward the **last seen/heard** player location instead of having perfect information when line-of-sight is broken.
  - If they reach the spot and still can't see you, they will **search** nearby briefly and can eventually **give up**.
- **Noise alerting**: **opening doors** and **attacking** (melee/ranged) will alert nearby monsters to investigate.
