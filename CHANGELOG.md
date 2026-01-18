# Changelog


## [0.22.0] - Unreleased

### Added
- Procedural terrain palette: per-run/per-floor HSL tints with room-style accents; configurable via settings and the new `#palette` command.
- Procedural terrain materials: deterministic, depth-aware cellular-noise substrate (STONE/BRICK/BASALT/...) used for subtle tinting (scaled by proc_palette strength) and LOOK descriptions.
- Material acoustics + scent absorption: substrate materials now modulate **footstep/dig noise** and **scent trail decay/spread** (moss/dirt dampen; metal/crystal ring out).
- **Procedural monster ecology (spawn theming)**: dungeon **terrain materials** and **room types** now apply mild, deterministic biases to the monster spawn tables — and certain affinity spawns can roll 1–2 nearby **nestmates** (shared groupId) — creating emergent ecology (spiders/snakes in dirt & moss, undead in marble/brick, kobolds in metal seams) without overriding content-mod spawn weights.
- **Procedural monster variants**: rare monsters now roll a persistent **rank** (Elite/Champion/Mythic) plus 1-3 **affixes** at spawn.
- **Procedural monster abilities**: Elite+ monsters can roll 1–2 active abilities (pounce, toxic miasma, cinder nova, arcane ward, summoning, screech) with cooldowns, saved/loaded per-entity.
  - Affixes implemented this round: **Swift**, **Stonehide**, **Savage**, **Blinking** (panic blink reposition), and **Gilded** (bonus gold + higher key/drop odds).
  - New combat-proc affixes: **Venomous** (poison), **Flaming** (burn), **Vampiric** (life drain), and **Webbing** (ensnare).
  - LOOK/targeting now surfaces rank + affixes, and XP rewards scale with procedural rank/affixes.
  - HP pip color hints rank (hidden while hallucinating).
  - Rolls are deterministic per monster (seeded from its sprite seed + depth + room type) and are **saved/loaded** so variants survive reloads.
- Game-driven procedural particle FX events (spells + digging) via `FXParticleEvent` queue.
- **Procedural particle flipbook animation**: renderer-owned particle textures (spark/smoke/ember/mote) are now generated as **4-frame animated flipbooks** using looped noise + domain warp, with per-particle phase offsets so bursts don’t animate in perfect sync.
- **Procedural particle motion advection**: smoke/motes/embers now drift through a lightweight **curl-noise** flow field and are biased by the game’s deterministic per-level **wind**, yielding more fluid, coherent motion (visual-only).
- **Procedural item micro-animations (4-frame)**: potions now slosh with looped-noise swirl (plus metallic/milky/murky/smoky variants), scroll labels shimmer, rings get orbiting specular glints, wands pulse with an energy crawl, and lit torches flicker with coherent embers.
- **Procedural sprite animation**: entities, items, and projectiles now use smooth, renderer-side procedural motion (move tween + hop/squash, idle bob, and hit recoil) to reduce "teleporty" feel in both top-down and isometric views.
- **High-resolution voxel sprite detail (64x64/128x128)**: voxel sprites now automatically increase their internal voxel-model resolution (nearest-neighbor voxel upscaling) before rendering, so large tile sizes produce smoother 3D voxel meshes in both perspective and isometric modes (isometric raytracing caps detail to stay performant).
- **3D turntable UI previews**: the **Monster Codex** and **Discoveries** overlays now include a large **auto-rotating 3D preview** (budget-cached). Unidentified items render an **appearance-based extruded turntable** so the NetHack-style identification game stays intact.
- **Isometric voxel terrain blocks**: optional voxel-generated wall/door/pillar/boulder **block** sprites in isometric view for better 3D cohesion with `voxel_sprites` (`iso_terrain_voxels`; toggle with `#isoterrainvox` / `#isoblocks`). Uses raytrace only for small tile sizes (<=64px) to avoid long stalls.
- **Expanded procedural surface camp (depth 0)**: the camp hub now generates a variable palisade yard with 2–4 packed tents/huts, an optional well + fire ring, clustered wilderness dressing, and a guaranteed walkable spine between EXIT and DUNGEON (with adaptive brush-clearing if trees/rocks block the route).
  - **Camp map size now varies per run** (compact/standard/sprawling) within the same safe bounds used by the dungeon generators, so the hub itself scales naturally with its palisade and wilderness dressing.
  - Camp gates now face the EXIT side, and the EXIT position varies per run for more distinct surface layouts.
  - Optional side sallyport gate + shallow ditch/moat (chasm ring) around the palisade (bridges preserved at gates).
  - Larger yards can generate 1–2 corner watchtowers, plus an internal reserved footpath network so later clutter doesn't block common routes.
  - Optional sacred grove POI outside the walls: standing stones around an altar, connected by a protected trail.
  - Camp landmarks now add extra map markers (WELL, FIRE, ALTAR, SIDEGATE).
- **Procedural rift cache pockets**: some floors now generate tiny optional wall pockets gated by a pushable **boulder** and a 1-tile **chasm** gap; pushing the boulder into the gap creates a rough bridge into a guaranteed **bonus chest** cache (via `bonusLootSpots`).
- **Symmetric room furnishing (new procgen pass)**: on some floors, 1–2 non-shop/non-shrine rooms get mirrored pillar/boulder (rarely chasm) "furniture" patterns with a reserved doorway-to-center navigation spine, so rooms stay traversable while looking more intentional. (#mapstats: FURNISH | SYMROOMS / SYMOBS)
- **Perimeter service tunnels (new procgen pass)**: on some room-based floors, the generator carves partial *inner-border* maintenance tunnels (offset=1 from the solid boundary) and punches 2–4 short door "hatches" back into the interior to create macro alternate routes (flanking / reduced backtracking), with an optional bonus cache chest. (#mapstats: PERIM TUNNELS | HATCHES)
- **Burrow crosscuts (new procgen pass)**: on some room-based floors, the generator uses an A* search biased toward **digging through wall mass** to carve 1–2 long crosscut tunnels between two far-apart corridor points, gated by doors (often locked/secret on one end). This creates dramatic optional shortcuts without tunneling through rooms or touching the stairs core route. (#mapstats: CROSSCUTS | CARVED | LOCKED | SECRET)
- **Secret crawlspace networks (new procgen pass)**: on some room-based floors, the generator carves a small hidden 1-tile-wide tunnel network entirely within wall mass, then connects it back to the corridor graph through 2–3 **secret doors**. Often contains an optional bonus cache chest deep inside. (#mapstats: CRAWLSPACES | CARVED | DOORS)
- **Procedural VFX custom animation (4-frame)**: the renderer now samples **4 animation frames** for procedural hazard tiles (gas + fire), and cross-fades between frames with per-tile phase offsets so large fields don’t flicker in sync.
- **Field chemistry: reactive hazards**: **fire** now burns away lingering **gas clouds**, and dense **poison vapors** can occasionally **ignite** into a brief flash-fire explosion that consumes the cloud and leaves lingering flames.
- **Hearing preview LOOK lens**: a new audibility heatmap that marks explored tiles where your **footsteps** would be heard by any *currently visible* hostile listener (default **Ctrl+H**; **[ / ]** adjusts volume bias).
- Fountain ripples now animate across all 4 frames using coherent traveling waves + looped fBm (no per-frame hash flicker), and item sprites (ground + UI icons) also get a small per-item phase offset to avoid synchronized blinking.
- **Reaction-diffusion rune fields**: Arcane UI panels now get an animated “living circuitry” rune layer, and altars gain a subtle etched rune-glow pass — generated from a tiny Gray–Scott solver and animated via seamless domain drift.
- **Procedural cursor reticles**: LOOK and targeting cursors now use procedurally generated animated “marching ants” reticle overlays (square + isometric diamond), improving focus/readability at high zoom.
- **Procedural room ambiance decals (4-frame)**: lair floors now get an animated **biofilm shimmer** decal, and shrine rooms now get **rotating rune** decals (top-down + isometric).
  - The renderer de-syncs these animated decal styles per-tile so special rooms feel alive instead of flashing in perfect unison.
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
### Save compatibility
- Save version bumped to **v43** (v41 adds `Item.flags`; v42 adds merchant guild pursuit; v43 adds shrine piety + prayer cooldown).

### Changed
- Procedural entity/projectile flipbooks: added sprite-space idle warps (slime squash & stretch, bat wing flap, snake slither, spider scuttle, wolf/dog tail wag) and replaced per-frame sprite shading jitter with a seamless looping shimmer; projectile arrow/rock now have 4-frame glint/tumble cues.
- Procedural VFX flipbooks: **confusion/poison gas** and **fire** overlays now apply a lightweight **curl-noise flow warp** (divergence-free) before the existing domain-warp masks, producing more "advected" motion and richer turbulence in both top-down and isometric views.
- **Dungeon length**: increased the default run from **20** to **25** floors (5 additional depths before the labyrinth and final sanctum).
- Extended command prompt: **TAB completion** now extends to the **longest common prefix** and lets you **cycle** through ambiguous matches by pressing **TAB** repeatedly (also works for pasted NetHack-style `#cmd` inputs).
- Extended command prompt: upgraded to a proper **caret editor** (LEFT/RIGHT, HOME/END, DEL forward-delete) with familiar shell/Readline shortcuts: **Ctrl+A/E** home/end, **Ctrl+W** delete previous word, **Ctrl+U** clear to start, **Ctrl+K** clear to end; plus **Ctrl+L** clears the whole line (consistent with filter UIs).
- Extended command prompt: TAB completion now shows an in-overlay **dropdown match list** with the active selection highlighted; added a default **Ctrl+P** binding to open the prompt when `#` is awkward on some layouts.
- Extended command prompt: completion dropdown now shows **keybinding hints** for common commands (save/load/options/help/etc) and scrolls to keep the active selection visible; TAB completion falls back to a **fuzzy subsequence match** when there are no prefix matches; added additional Readline-style shortcuts inside the prompt (**Ctrl+B/F** move caret, **Ctrl+P/N** history, **Ctrl+D** forward delete).
- Extended command prompt: unknown commands now show a short **DID YOU MEAN** suggestion list to make typos less punishing.
- Extended command prompt: TAB completion is now **context-aware** for common argument slots: `bind`/`unbind` completes **action names** (from the shared Action registry), and `preset`/`autopickup`/`identify`/`mortem` complete their common option values.
- Inventory overlay: the left-hand list now shows compact **quick-compare** badges (ATK/DEF/RA/RN deltas + ring mods) so upgrades are visible at a glance; fixed gear stat comparisons to include **bless/curse** modifiers.
- Keybinds editor overlay: added an in-overlay **filter/search** (press `/`), **DEL** to unbind (writes `none`), and highlighted **conflicting** duplicate chords.
- Keyboard input: SDL key repeat is now honored for movement and menu navigation (hold a direction to keep moving; repeats are still suppressed for toggles/confirm/ESC to avoid accidental spam).
- **#whistle** now calls **all friendly companions** with **Follow/Fetch** orders (not just the starting dog).
- Auto-travel: you can now **target locked doors** directly (when you have keys/lockpicks), making it easier to walk to vault doors intentionally.
- Auto-explore: when no normal frontiers remain, it will now **walk to reachable locked doors** that border unexplored tiles (if you have keys/lockpicks) so it doesn't incorrectly report the floor as fully explored.
- Auto-move hazard awareness: **auto-travel** now strongly prefers routes that avoid **confusion/poison gas** tiles, and **auto-explore** will not path through gas clouds (similar to fire) until they dissipate.
  - Auto-move also stops immediately if you end up standing in a gas cloud (even before the status effect lands), preventing accidental turn-burning in hazards.
- Auto-travel (Sneak): when **Sneak** is enabled and hostiles are visible, auto-travel/auto-explore path planning now incorporates the dungeon's **sound propagation** model + monster hearing deltas to prefer routes where your **footsteps won't be heard**.
- Sound/hearing integration: visible-hostile audibility is now computed as a single **multi-source seeded Dijkstra field** (tile→listener), improving performance and making muffling doors/locks model **directionally** (no more listener→tile approximation).
- Floor trap scatter now avoids **shops** and **shrines** (keeps safe spaces consistent; traps still appear elsewhere).
- Darkness lighting now treats **burning creatures** and **flaming ego weapons** as small moving light sources (in addition to torches, room ambient light, and fire fields).
- Monster pathfinding now treats **poison gas** and **discovered traps** as higher-cost tiles, so monsters (and pets) will route around known hazards when practical.
- Ranged monster tactics: ranged attackers now avoid shooting through **allied** monsters (and peaceful shopkeepers), and will try to **sidestep** for a clear shot instead of friendly-firing.
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
- Monster timed-effect timers now tick correctly for enemies as well (regen/shield/invisibility no longer last forever if a monster uses a potion or ability).
- Shielding now contributes to **monster damage reduction**, making shield potions and Arcane Ward meaningful.
- Fixed MSVC build break: LOOK/targeting HUD no longer references an out-of-scope `fitToChars` lambda (shared helper lives in render.cpp).
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
- Fixed MSVC build break in minimap cursor tooltip: expose Game::describeAt and remove shadowing warnings in save migration + isometric phantom rendering.
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

### Changed
- **Alarm traps** now alert monsters to the alarm's **location** (so they path toward the source).

## [0.20.4] - 2025-12-27

### Added
- **Recoverable ammo**: **Arrows** and **Rocks** can now sometimes remain on the ground after a ranged attack.
  - Works for both your shots and enemy volleys.
  - Recovery chance is lower when the projectile hits a wall/closed door, and also reduced for enemy shots (so archers don’t become infinite ammo printers).

## [0.20.3] - 2025-12-27

### Added
- **Monsters trigger traps**: spike, poison dart, teleport, alarm, and web traps now affect monsters too (and can even kill them).
- **Monster timed effects**: **poison** and **webbing** now tick down on monsters over time (webbing prevents movement while active).

### Changed
- Traps are only marked as **discovered** when **you** trigger them, or when you can **see** another creature trigger them.

## [0.20.2] - 2025-12-27

### Added
- **Mimic chests**: some **Chests** are actually **Mimics** in disguise.
  - They look like normal chests until you try to **open** them.
  - Mimics prefer to spawn **adjacent** when revealed to avoid overlapping the player (since chests are opened underfoot).

## [0.20.1] - 2025-12-27

### Added
- **Disarm chest trap**: the **Disarm** action (`T` by default) can now disarm a *known* trapped **Chest** adjacent to you (or underfoot).
  - On failure you may **set the trap off**.
  - **Lockpicks** improve your chances, but can break.

## [0.20.0] - 2025-12-27

### Added
- **Lock door** action (`Shift+K` by default): lock an adjacent door by consuming a **Key**.
  - Locks a **closed** door, or **closes + locks** an adjacent open door (if the doorway is not blocked).
  - Locked doors are **impassable** until unlocked (use a key/lockpick or a Scroll of Knock).

### Dev
- Added `Dungeon::lockDoor()` and a unit test covering door locking/unlocking rules.

## [0.19.0] - 2025-12-27

### Changed
- **Scroll of Identify** now lets you *choose* which unidentified potion/scroll to learn when multiple candidates exist.
  - Press **Enter** on the highlighted inventory item to identify it.
  - Press **Esc** to identify a random unknown item (preserves the previous random behavior).


## [0.18.8] - 2025-12-27

### Added
- **Close door** action (`K` by default): close an adjacent open door.
  - Only closes cardinally-adjacent doors; cannot close a blocked doorway.

## [0.18.7] - 2025-12-27

### Added
- **Disarm trap** action (`T` by default): attempt to disarm a discovered adjacent trap.
  - Chance scales with level; lockpicks improve success but can break on failure.
  - On failure you may set the trap off.

## [0.18.6] - 2025-12-27

### Fixed
- Default settings file creation (`writeDefaultSettings`) is now atomic and creates parent directories automatically (more robust for nested `--data-dir` paths and portable installs).

### Dev
- Added a unit test to ensure `writeDefaultSettings()` can create missing parent directories.
## [0.18.5] - 2025-12-27

### Fixed
- Settings and scoreboard parsing now tolerate a UTF-8 BOM at the start of the file (common when editing on Windows).

### Dev
- Added unit tests covering BOM-prefixed `procrogue_scores.csv` and `procrogue_settings.ini` parsing.

## [0.18.4] - 2025-12-27

### Fixed
- Scoreboard CSV numeric parsing now rejects negative/overflow values for unsigned fields (prevents wrapped huge numbers if the file is edited by hand).

### Changed
- Atomic text writes for the scoreboard and settings INI now create parent directories automatically (more robust for `--data-dir` / portable installs).
- Scoreboard "top runs" ordering is now deterministic for tied scores (ties break by timestamp and metadata).

### Dev
- Added unit tests for hardened numeric parsing, tie-breaking sort order, and parent-directory creation for atomic writes.

## [0.18.3] - 2025-12-27

### Fixed
- Scoreboard trimming now scales the **top+recent** mix correctly even when trimming to a small cap (e.g. a top-10 view), so recent history doesn't silently disappear.

### Changed
- INI helper writes (`updateIniKey()` / `removeIniKey()`) are now **atomic** and `updateIniKey()` deduplicates multiple existing entries for the same key.

### Dev
- Added unit tests for small-cap scoreboard trimming and INI key deduplication.

## [0.18.2] - 2025-12-27

### Fixed
- Scoreboard trimming now keeps a mix of **top runs** and **recent runs**, so `#history` continues to show your latest games even if they are low-scoring.

### Changed
- `procrogue_scores.csv` retention increased to **120 entries** (top **60** by score + most recent **60** by timestamp).

### Dev
- Added a unit test covering scoreboard trimming behavior (top+recent mix).

## [0.18.1] - 2025-12-27

### Fixed
- Scoreboard CSV parsing now preserves leading/trailing whitespace *inside quoted fields* (CSV semantics), while still trimming unquoted fields.
- `removeIniKey()` now treats missing settings files as a no-op (instead of failing).

### Changed
- `updateIniKey()` now creates a minimal settings file if it doesn't exist (more robust for in-game `#bind` / options commands).

### Dev
- Added unit tests for quoted-whitespace CSV fields and INI helper behavior.
## [0.18.0] - 2025-12-27

### Added
- **Smart auto-pickup mode**: new `smart` mode sits between `gold` and `all`.
  - Picks up “core” items (gold, keys/lockpicks, consumables, and equipment; ammo only if you have the matching ranged weapon).
- **Recent run history**: new extended command `#history [n]` shows your most recent runs (newest first).

### Changed
- Auto-explore now **auto-walks to visible loot/chests** (that won't be auto-picked) and **stops on arrival**, instead of stopping immediately on sight.
- Auto-explore no longer treats **open chests** as an interruption target.

### Fixed
- Save loading now preserves `AutoPickupMode::Smart` (no longer clamped back to `gold`).

### Dev
- Added a unit test for parsing `auto_pickup = smart` from settings.


## [0.17.0] - 2025-12-27

### Added
- **In-game keybind management**:
  - `#binds` prints the current active keybinds (defaults + overrides).
  - `#bind <action> <keys>` updates `bind_<action>` in `procrogue_settings.ini` and reloads keybinds immediately.
  - `#unbind <action>` removes the override for an action (resets it back to defaults) and reloads keybinds.
- **Hot config reload**: `#reload` reloads settings + keybinds from disk while the game is running (safe subset applies immediately; renderer/window-related settings still require restart).
- **Scoreboard slot tracking**: `procrogue_scores.csv` now records the active save slot for each run (new `slot` column; older files remain readable).

### Changed
- `#scores` output now includes the run slot when it is not `default`.

### Dev
- Added `removeIniKey()` helper for settings file maintenance.


## [0.16.0] - 2025-12-27

### Added
- **Persistent default save slot**: new `default_slot` setting (and new extended command `#slot <name>`) lets you set the active slot used by **F5/F9/F10** and `#save/#load`.
- **Daily runs from CLI**: `--daily` starts a deterministic UTC-date seeded run (matches `#daily`).

### Changed
- Options menu now includes `AUTO MORTEM` and `SAVE BACKUPS`.
- Exported logs/dumps now include the current **slot name**.

### Fixed
- `--seed` now accepts hex input (e.g., `--seed 0xBADC0DE`).
- Minor formatting cleanup in `main.cpp`.

## [0.15.0] - 2025-12-27

### Added
- **Save slots**:
  - `#save <slot>` / `#load <slot>` / `#loadauto <slot>` to save/load named slots (files are stored next to your normal save).
  - `#saves` to list detected save slots (manual + autosave).
  - `--slot <name>` command-line flag to run the game using a specific slot by default.
- **Automatic mortem dumps**: new `auto_mortem` setting writes a `procrogue_mortem_*.txt` full-state dump automatically when you **win** or **die**.
  - `#mortem` exports a mortem dump immediately.
  - `#mortem on|off` toggles the feature.
- **New extended commands**:
  - `#paths` shows the active save/autosave/scores/settings file paths.
  - `#exportall` exports **log + map + dump** in one command.

### Changed
- Autosave now also triggers on **floor changes** (stairs) when autosave is enabled (`autosave_every_turns > 0`), improving crash-safety between levels.

### Dev
- Unit tests now also compile `spritegen.cpp` (still no SDL required).

## [0.14.0] - 2025-12-27

### Added
- **Daily challenge run**: `#daily` restarts a new game using a deterministic **UTC-date** seed.
- **Full state dump export**: `#dump` writes a single timestamped text file with your **run state**, **equipment/inventory**, **recent messages**, and an **ASCII map snapshot**.
- **Seeded restarts**: `#restart <seed>` now accepts an optional seed argument.

### Changed
- Auto-explore no longer stops for normal loot when **auto-pickup is ALL** (it will still stop for **chests**, since they can't be auto-picked).
- Restart no longer resets your auto-pickup preference.
- Auto-move now stops automatically if you are **starving** (when hunger is enabled).

### Fixed
- `#exportlog` / `#exportmap` / `#export` now compile again and correctly export using the current `MessageKind` / `EntityKind` types.
- Fixed a compile error in auto-explore frontier selection (`canUnlockDoors` capture order).
- Fixed a compile error in chest-lockpick chance clamping (uses `std::clamp`).

### Dev
- Unit tests now compile the full core game logic (`game.cpp`) even when building tests without SDL.

## [0.13.0] - 2025-12-27

### Added
- **Save backups**: new `save_backups` setting to keep rotated backups of manual saves and autosaves (`.bak1..bakN`).
- **Startup flags**:
  - `--load-auto` to auto-load the autosave on launch.
  - `--data-dir <path>` to override the save/config directory.
  - `--portable` to store saves/config next to the executable.
  - `--reset-settings` to overwrite settings with fresh defaults.
- **Exports**: new extended commands:
  - `#exportlog` writes a timestamped run log (`procrogue_log_YYYYMMDD_HHMMSS.txt`).
  - `#exportmap` writes a timestamped ASCII map snapshot (`procrogue_map_YYYYMMDD_HHMMSS.txt`).
  - `#export` writes both.

### Changed
- Fullscreen and screenshot are now **fully rebindable/disableable** (no hard-coded F11/F12 override).
- Startup load flow now tries **save ↔ autosave fallbacks** and shows clear status messages.

### Fixed
- Locked-door sprite compile issue: `Renderer` now declares `doorLockedTex` (matched to `render.cpp`).
- Save loading now reports **corrupt/truncated** saves instead of failing silently.

## [0.12.0] - 2025-12-27

### Added
- **Treasure chests**: locked/trapped chests can spawn in **Treasure / Secret / Vault** rooms.
  Stand on a chest and press **Enter** to open it.
- Chest loot spills onto the floor (gold + items) and the chest becomes an **open chest** on the map.

### Changed
- **Scroll of Knock** now unlocks nearby **chests** as well as locked doors.
- **Search** and **Scroll of Detect Traps** now reveal **trapped chests**.
- Pickup/auto-pickup ignores chests (they are world interactables, not inventory items).

### Fixed
- Auto-travel / auto-explore pathfinding now properly routes through **locked doors** when you have **Keys** or **Lockpicks**.

## [0.11.0] - 2025-12-27

### Added
- **Lockpicks**: a new item that can pick locked doors when you don’t have keys (chance to fail/break).
- **Scroll of Knock**: unlocks and opens nearby locked doors.
- **Trapped vault doors**: some locked doors now hide **alarm/poison dart traps** on the doorway tile.
- HUD/Stats overlay now shows your current **PICKS** count.

### Changed
- Locked doors now render with a distinct **lock overlay** sprite.

### Fixed
- FOV now correctly treats **locked doors as opaque**, matching line-of-sight rules.

## [0.10.0] - 2025-12-27

### Added
- **Locked doors + keys**: vault doors are visible but locked; find **Keys** to unlock them.
- **Vault rooms**: optional locked side-rooms with higher-risk/higher-reward loot.
- HUD/Stats overlay now shows your current **KEYS** count.
- New Key item sprite.
- Unit tests: locked door tile rules + unlock/open transitions.

### Changed
- Minimap now renders locked doors with a distinct tint.

## [0.9.0] - 2025-12-27

### Added
- **Secret rooms + secret doors**: each floor may now generate 1–2 optional secret treasure rooms hidden behind secret doors.
- Searching (**C**) can now reveal **secret doors** in addition to traps.
- New **Scroll of Detect Secrets** (identifiable scroll) that reveals secret doors on the current floor.
- Unit tests: improved dungeon connectivity BFS to match in-game diagonal movement rules.
- CI: replaced the placeholder GitHub Actions workflow with a real build+test matrix (Ubuntu/macOS/Windows) and a Linux game build.
- Repo hygiene: restored `.gitignore`, `.editorconfig`, and `.clang-format` files in the root.

## [0.8.0] - 2025-12-26

### Added
- Player name (`player_name` / `#name`) shown in HUD overlays and recorded in the scoreboard.
- Richer scoreboard metadata: name, cause of death/win, and game version are stored in `procrogue_scores.csv`.
- Rebindable fullscreen + screenshot actions (`bind_fullscreen`, `bind_screenshot`).
- HUD effect timers option (`show_effect_timers`) + new in-game toggle in the Options menu.
- `#scores [N]` extended command to print top runs in the message log.

### Changed
- Auto-explore now interrupts on newly applied debuffs (poison/web) in addition to enemies/damage.
- Scoreboard loading/writing is more robust and backwards-compatible with older CSV layouts.

### Fixed
- Documentation now reflects the current `.bin` save file extensions.

## [0.7.0] - 2025-12-26

### Added
- New **Web trap** that can immobilize you for several turns.
- New **Scroll of Detect Traps** (identifiable scroll) that reveals all traps on the current floor.
- New shrine interaction command: **#pray** (auto, or `pray heal|cure|identify|bless`). Costs gold and consumes a turn.
- Proper scroll sprites for **Scroll of Identify** and **Scroll of Detect Traps** (no more fallback icon).

### Changed
- Trap generation now includes Web traps on deeper floors.
- Help/overlay text updated to include the new shrine command.

## [0.6.1] - 2025-12-26

### Added
- Real **GitHub Actions CI** (build + unit tests) instead of the placeholder workflow.
- Repo hygiene files: `.gitignore`, `.editorconfig`, `.clang-format`.

### Changed
- FetchContent SDL2 archive bumped to **SDL2 2.32.10** for consistency with docs.
- Default stairs bindings no longer use `kp_9` / `kp_3` (avoids conflicts with diagonal movement).

### Fixed
- **CMakePresets**: fixed invalid parent preset reference (Visual Studio could fail to load presets).
- **Build breakages** caused by accidental escaping / broken string literals in `game.cpp`, `render.cpp`, and `settings.cpp`.
- Options/command overlays now use a consistent local UI palette (`white` / `gray`) and the correct `Color` type.

## [0.6.0] - 2025-12-26

### Added
- **Configurable keybindings** via `bind_*` entries in `procrogue_settings.ini`
- In-game **Options** menu (**F2**) for common toggles (auto-pickup, autosave, identify, auto-step delay)
- NetHack-like **extended command prompt** (**#**, Shift+3) with history + tab completion (`save`, `load`, `quit`, etc.)

### Changed
- HUD + Help text updated to reflect the new menus / command system
- Settings are auto-saved when changed via the Options menu / extended commands

## [0.5.0] - 2025-12-26

### Added
- Inventory quality-of-life:
  - **Shift+S** sorts the inventory (no turn cost)
  - **Shift+X** drops the entire selected stack
- New content: **Axe**, **Plate Armor**, and a new monster: **Ogre**
- Build system: `PROCROGUE_BUILD_GAME` (ON by default) to allow **tests-only builds without SDL**
- GitHub Actions CI (Windows/macOS/Linux) + a dedicated **headless tests-only** job
- Repository hygiene: `.clang-format`, `.editorconfig`, `.gitignore`

### Changed
- Updated the FetchContent SDL2 tarball to **SDL2 2.32.10** (official libsdl release tarball)

### Fixed
- Fixed multiple compile-time issues in the renderer minimap overlay
- Fixed compile errors in run recording (`goldCount()` call) and score parsing (`trim()` name clash)

## 0.4.0

### Gameplay / controls
- **Inventory is fully usable via keyboard now**:
  - `E` equip, `U` use, `X` drop (while inventory is open).
  - `Enter`/`A` (controller) performs a **context action** on the selected item (equip if gear, use if consumable).
- **PageUp / PageDown** now scroll the message log (mouse wheel still works).
- **Look mode** can be toggled with `L` or `V`.

### Input
- **Game controller support (SDL2 GameController API)**:
  - D-pad movement, `A` confirm, `B` cancel, `X` inventory, `Y` pickup, shoulders for look/fire.
  - Can be disabled via settings.

### Rendering / performance
- Updated **SDL2 FetchContent** (Windows option) to **SDL2 2.32.10**.
- New settings:
  - `vsync = true/false`
  - `max_fps = 0|30..240` (only used when `vsync=false`)
- HUD text rendering improved by expanding the built-in **5x7 font** to cover the punctuation used in UI (|, (), [], +, =, etc.).

### Dev / project upgrades
- Added a real **GitHub Actions CI** workflow (Windows/macOS/Linux) that builds and runs tests.
- Added a small **unit test suite** (RNG determinism, dungeon connectivity, item definition sanity).
- Added `.clang-format` and `.editorconfig` for consistent formatting.

## 0.3.0

### New gameplay
- **NetHack-style item identification**: potions + scrolls start unidentified each run, with randomized appearances.
  - Using an item identifies that item type for the rest of the run.
  - New item: **Scroll of Identify** (reveals one random unknown potion/scroll in your inventory).
- **New monster pressure**:
  - **Spiders** can **web** the player (prevents movement for a few turns).
  - **Wizards** can occasionally **blink** (teleport) to reposition.

### UI / QoL
- Inventory, look mode, pickup/drop messages now respect identification (unknown items show their appearance label).
- HUD now shows compact **status effect tags** (POISON / WEB / REGEN / SHIELD / HASTE / VISION).

### Settings
- New key: `identify_items = true/false` (see `docs/SETTINGS.md`).

### Save compatibility
- Save version bumped to **v6**.
  - v5 and older saves still load (items will appear fully known, matching older behavior).
