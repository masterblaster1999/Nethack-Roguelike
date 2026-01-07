# Changelog


## [0.22.0] - Unreleased

### Added
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

- **Secret shortcut doors**: some procedural floors now add a small number of **hidden** doors (secret doors) in corridor walls where adjacent passages run alongside each other.
  - These create optional **loops/shortcuts** that reward searching, without carving disconnected "hidden corridor" pockets.

- **Locked shortcut gates**: some procedural floors now add a small number of **visible locked doors** in corridor walls that connect two adjacent corridor regions already connected elsewhere.
  - These create optional **key/lockpick-powered shortcuts** without ever blocking stairs progression.

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
  - **FETCH** companions now pick up gold and actually **carry it back**, delivering it when adjacent to you (and dropping it on death).
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

- New monster: **Zombie** (slow, tough undead).
  - **Undead** (Ghost/Skeleton/Zombie) are **immune to poison damage**.

- **Trap setpieces**: some floors now generate small **corridor gauntlets** (short strips of traps in long straight corridors).
  - **Bonus loot caches** requested by the dungeon generator may also be guarded by one or more nearby floor traps.

- **Monsters can drink potions:** Wizards may now spawn with a pocket potion and will sometimes drink it mid-fight (heal, regen, shielding, invisibility, levitation). Levitation now also enables monster pathing across chasms.

### Save compatibility
- Save version bumped to **v38** (adds monster pocket consumables; retains v37 item egos/brands).

### Changed
- **#whistle** now calls **all friendly companions** with **Follow/Fetch** orders (not just the starting dog).
- Floor trap scatter now avoids **shops** and **shrines** (keeps safe spaces consistent; traps still appear elsewhere).
- Darkness lighting now treats **burning creatures** and **flaming ego weapons** as small moving light sources (in addition to torches, room ambient light, and fire fields).

### Fixed
- Test/headless builds no longer require SDL2: **keybinds** are compiled only for the game executable.
- Fixed build-breaking pointer dereferences in **Disarm Trap** and **Shrine prayer** interactions.
- Fixed MSVC build breaks/warnings: implemented missing chest overlay item icon helper, corrected keybind chord formatting, and centralized SDL includes to avoid SDL_MAIN_HANDLED macro redefinitions.









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
