# Nethack Roguelike

A tiny NetHack-inspired roguelike with:
- Turn-based combat and dungeon crawling
- Procedurally generated pixel sprites (player, monsters, items)
- Field-of-view + explored/seen tiles
- Traps, potions/scrolls, ranged attacks, stairs, saving/loading
- **Secret rooms + secret doors** (search to discover)
- **Locked doors, keys + lockpicks**: unlock optional **vault rooms** for bonus loot (or read a **Scroll of Knock**)

## New in this build (QoL + big usability upgrades)

- **High-resolution procedural sprites (up to 256x256)**: the procedural sprite generator can now output higher pixel resolutions (clamped to 256) so tiles/monsters/items remain crisp when you increase `tile_size` in `procrogue_settings.ini`.
  - Map tiles + overlays are generated at **tile resolution** to avoid blocky renderer upscaling.
  - 3D voxel sprites also render at the requested resolution.

- **New dungeon generation variety**:
  - **Depth 3** is a **cavern** (cellular automata)
  - **Depth 4** is a **maze** (perfect maze + a few loops + door chokepoints)
  - Deeper depths may randomly mix rooms / caverns / mazes

- **Inventory hotkeys**: while inventory is open:
  - **E** equip/unequip
  - **U** use (consumables)
  - **X** drop **one** (from a stack)
  - **Shift+X** drop the **whole stack**
  - **Shift+S** sort the inventory
  - **Enter** performs a **context action** (equip if gear, use if consumable)
- **Game controller support** (SDL2 GameController): D-pad move, **A** confirm, **B** cancel, **X** inventory, **Y** pickup, shoulders for look/fire.
- **VSync + FPS cap settings**: new `vsync` and `max_fps` keys in `procrogue_settings.ini`.

- **VRAM-friendly sprite caching**:
  - Procedural entity/item/projectile sprites are now cached with an **LRU texture cache** instead of growing unbounded.
  - New setting: `texture_cache_mb` (0 = unlimited) to cap approximate VRAM usage for cached sprites.
  - The **Stats** overlay (Shift+Tab) shows cache usage, hits/misses, and evictions.
  - When using very large tiles, the renderer also reduces decal/autotile-variant counts to keep texture memory reasonable.

- **Configurable keybindings**: add `bind_*` entries in `procrogue_settings.ini` (multiple keys per action).
- **In-game options menu**: press **F2** to tweak common settings (auto-pickup, autosave, etc.).
- **NetHack-like extended commands**: press **#** (Shift+3) to open a command prompt (`save`, `load`, `pray`, `quit`, ...).

- **Sound propagation + investigation:** noisy actions (footsteps, doors, combat) now generate dungeon-aware sound; monsters can investigate through corridors, while walls/secret doors block and closed/locked doors muffle.
- **Potion of Invisibility:** makes you much harder to see (most monsters only notice you when adjacent). Attacking breaks invisibility.
- **New extended command:** `#shout` (or `#yell`) spends a turn to make a loud noise to lure monsters.

- **Auto-travel**: enter look mode (`L` / `V` or **right-click**) and press **Enter** to auto-walk to the cursor tile.
- **Auto-explore**: press **O** to walk to the nearest unexplored frontier until interrupted.
  - If auto-explore spots a chest or loot you won’t auto-pick, it will **auto-walk to it and stop on arrival**.
- **Auto-pickup modes**: press **P** to cycle **OFF → GOLD → SMART → ALL → OFF**.
- **8-way movement**: move diagonally with **Q/E/Z/C** (or numpad 1/3/7/9). (Fully rebindable.)
- **Minimap + stats overlays**:
  - **M** toggles the minimap
  - **Shift+Tab** toggles the stats/high-scores panel
- **Screenshots**: press **F12** (bindable) to save a BMP screenshot (to the `screenshots/` folder next to your save).
- **Fullscreen toggle**: press **F11** (bindable).
- **Player name**: set `player_name` in `procrogue_settings.ini` or use `#name <text>` in-game (recorded in the scoreboard).
- **Richer high-scores**: run history now records your **name**, **cause of death/win**, and **game version** in `procrogue_scores.csv`.
- **HUD effect timers**: optional turn counters on status tags (example: `POISON(6)`). Toggle via the options menu or `show_effect_timers` in settings.
- **Auto-explore safety**: auto-move now stops when you get poisoned or webbed (not just when you take damage).
- **Secret rooms**: levels can now spawn optional **secret treasure rooms** behind hidden doors.
  - Use **C** to search and reveal secret doors.
  - New **Scroll of Detect Secrets** reveals secret doors on the current floor.
- **Vault rooms**: levels can now spawn optional **locked vault rooms** behind visible locked doors.
  - Find **Keys** as loot (or from enemies) to unlock vault doors.
  - If you’re out of keys, **Lockpicks** can pick locked doors (chance to fail/break).
  - New **Scroll of Knock** unlocks + opens nearby locked doors.
  - Some vault doors may be **trapped** (alarm/poison dart) — searching / Detect Traps can save you.
  - Your current counts are shown in the HUD ("KEYS" / "PICKS").
- **Treasure chests**: Treasure / Secret / Vault rooms can spawn chests.
  - Stand on a chest and press **Enter** to open it.
  - Chests may be **locked** (keys or lockpicks) and/or **trapped** (Search / Detect Traps can reveal).
  - **Scroll of Knock** can also unlock chests.
- **Run history / high scores** are stored in `procrogue_scores.csv`.
- **Mouse support**:
  - **Left click**: auto-travel to the clicked tile
  - **Right click**: enter look mode at the clicked tile
  - **Mouse move**: moves the look cursor / aiming cursor
  - **Mouse wheel**: scroll message log
- **Settings file** (auto-created on first run): tweak tile size, HUD height, fullscreen, auto-move speed, auto-pickup/autosave, and `bind_*` keybindings.
- **Optional hunger system**: enable `hunger_enabled` to add food + starvation (and a new **Food Ration** item).
- **Optional encumbrance system**: enable `encumbrance_enabled` for carrying capacity + burden states.
  - HUD shows `WT: current/max` and a burden tag (BURDENED/STRESSED/STRAINED/OVERLOADED).
  - Overloaded prevents movement until you drop items (and also blocks stairs).
  - Toggle via the options menu or `#encumbrance [on|off]`.
- **Safer quitting**: `confirm_quit` (default ON) requires **ESC twice** to exit (prevents accidental quits).
- **Context pickup**: when not on stairs, **Enter** will pick up items you’re standing on.

- **NetHack-style item identification**: potions + scrolls start unknown each run (randomized appearances). Using them identifies the item type; you can also find/read a **Scroll of Identify**. (Toggle via `identify_items` in settings.)
- **New scroll**: **Scroll of Detect Traps** reveals all traps on the current floor (and identifies itself when used).
- **Shrines**: use **#pray** (`pray heal|cure|identify|bless|uncurse`) to spend gold for a small blessing (consumes a turn).
- **Curses + blessings (BUC)**: weapons/armor can be **BLESSED**, **UNCURSED**, or **CURSED**.
  - **Cursed** gear can't be unequipped or dropped until uncursed.
  - New **Scroll of Remove Curse** can cleanse cursed gear.
  - Shrines support `pray uncurse` (and `pray bless` may bless a piece of equipped gear).

- **New trap**: **Web traps** can immobilize you for a few turns.
- **Scent trails**: certain monsters (wolves, snakes, spiders, etc.) can follow your lingering scent around corners when you break line-of-sight (e.g. invisibility/darkness). Scent decays over time and is blocked by walls and closed doors.
- **New combat spice**: spiders can web you (movement blocked for a few turns), and wizards can occasionally "blink" (teleport) to reposition — and sometimes curse your equipped gear.
- **New content**: **Axe** + **Plate Armor**, and a new monster: the **Ogre**.
- **CI build workflow** for Linux/macOS/Windows.

## Controls

### Movement / exploration
- **Move**: WASD / Arrow keys / Q-E-Z-C diagonals (also numpad)
- **Wait**: `.` or Space
- **Look**: `L` or `V` (or right-click)
- **Auto-travel**: Enter while looking (or left-click a tile)
- **Auto-explore**: `O`
- **Search (reveal traps)**: `C`
- **Control preset**: `F2` → **Control Preset** (or `#preset modern|nethack`)
  - **Modern**: WASD + QEZC diagonals (Search = `C`, Kick = `B`, Look = `L/V`)
  - **NetHack**: HJKL + YUBN diagonals (Search = `S`, Kick = `Ctrl+D`, Look = `:`/`V`)
- **Auto-pickup mode**: `P` (cycles OFF/GOLD/SMART/ALL)
- **Minimap**: `M`
- **Stats / high scores**: `Shift+Tab`

### Interaction
- **Pick up**: `G`
- **Inventory**: `I`
  - While inventory is open: **E** equip/unequip, **U** use, **X** drop one, **Shift+X** drop stack, **Shift+S** sort, **Enter** context action
- **Fire ranged**: `F` (aim with mouse or WASD/arrows/QEZC, **Enter** or **left-click** to fire, **right-click** or **Esc** cancels)
- **Use stairs**: `<` up, `>` down

### Controller (optional)
- **D-pad**: move
- **A**: confirm / context action in inventory
- **B**: cancel / close overlays
- **X**: inventory
- **Y**: pick up
- **LB**: look
- **RB**: fire
- **Start**: stats
- **Back**: help
- **R-stick**: minimap

### Meta
- **Help**: `F1`, `?`, or `H`
- **Options menu**: `F2`
- **Extended commands**: `#` (Shift+3)
- **Save / Load**: `F5` / `F9`
- **Load autosave**: `F10`
- **Screenshot**: `F12` (bindable via `bind_screenshot`)
- **Fullscreen**: `F11` (bindable via `bind_fullscreen`)
- **Message log scroll**: PageUp / PageDown (or mouse wheel)
- **Restart**: `F6`
- **Quit**: Esc (or Esc twice if `confirm_quit` is enabled)

![game nethack](https://github.com/user-attachments/assets/fc0e0902-b161-47e6-b2f3-cc6f88100548)

## Save files + settings location

By default, ProcRogue uses **SDL_GetPrefPath** so saves and settings live in a per-user writable folder.

You can override this with:
- `--data-dir <path>` (explicit directory)
- `--portable` (store next to the executable)
- `--slot <name>` (use a named save slot; affects save + autosave filenames)


- `procrogue_save.dat`
- `procrogue_autosave.dat`
- `procrogue_settings.ini`
- `procrogue_scores.csv`
- `screenshots/`

Slot files (when using `--slot <name>` or `#save <slot>`):

- `procrogue_save_<slot>.dat`
- `procrogue_autosave_<slot>.dat`

The settings file is created automatically on first run.

## Command-line

- Start with a specific seed:
  - `procrogue --seed 12345`

- Start a daily run (deterministic UTC-date seed):
  - `procrogue --daily`

- Auto-load the manual save on startup:
  - `procrogue --load` (alias: `--continue`)

- Auto-load the autosave on startup:
  - `procrogue --load-auto`

- Override the save/config directory:
  - `procrogue --data-dir ./my_saves`

- Use a named save slot (changes save + autosave filenames):
  - `procrogue --slot run1`

- Portable mode (store saves/config next to the executable):
  - `procrogue --portable`

- Reset settings to defaults (backs up the previous file to `procrogue_settings.ini.bak`):
  - `procrogue --reset-settings`

- Print the game version:
  - `procrogue --version`

- Show command-line help:
  - `procrogue --help`


## Extended commands

Press **`#`** (Shift+3) to open the in-game command prompt.

Useful commands:
- `#help` – list commands
- `#options` – open the options menu
- `#binds` – list active keybinds (defaults + overrides)
- `#bind <action> <keys>` – set a keybind (writes to settings + reloads)
- `#unbind <action>` – reset an action back to default bindings
- `#reload` – reload settings + keybinds from disk
- `#save <slot>` – save to a named slot (e.g. `#save run1`)
- `#load <slot>` – load a named slot
- `#loadauto <slot>` – load a named autosave slot
- `#saves` – list detected save slots
- `#slot <name>` – set the *active* default slot (affects F5/F9/F10 and persists to settings)
- `#paths` – show active save/autosave/scores/settings paths
- `#mortem [now|on|off]` – export a mortem dump now, or toggle auto-mortem-on-death/win
- `#restart` – restart with a random seed
- `#restart <seed>` – restart with a specific seed (e.g. `#restart 12345` or `#restart 0xBADC0DE`)
- `#daily` – restart using a deterministic **UTC-date** "daily" seed
- `#autopickup off|gold|smart|all` – set auto-pickup mode
- `#autosave <turns>` – set autosave interval (0 disables)
- `#stepdelay <ms>` – set auto-step delay (10–500)
- `#identify on|off` – toggle item identification system
- `#timers on|off` – toggle status effect timers
- `#seed` – show the current run seed
- `#scores [n]` – show top scores (default 10)
- `#history [n]` – show most recent runs (default 10)

Exports (written to your save/config directory):
- `#exportlog [filename]` – export a run log (`procrogue_log_*.txt`)
- `#exportmap [filename]` – export an ASCII map snapshot (`procrogue_map_*.txt`)
- `#export` – export both log + map
- `#exportall [prefix]` – export log + map + dump in one command
- `#dump [filename]` – export a single **full state dump** (stats + equipment/inventory + recent messages + map)

## Building

See `docs/BUILDING.md`.

To build the unit tests:
```bash
cmake -S . -B build -DPROCROGUE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
