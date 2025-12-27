# Changelog



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
