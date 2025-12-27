# Nethack Roguelike

A tiny NetHack-inspired roguelike with:
- Turn-based combat and dungeon crawling
- Procedurally generated pixel sprites (player, monsters, items)
- Field-of-view + explored/seen tiles
- Traps, potions/scrolls, ranged attacks, stairs, saving/loading

## New in this build (QoL + big usability upgrades)

- **Inventory hotkeys**: while inventory is open:
  - **E** equip/unequip
  - **U** use (consumables)
  - **X** drop **one** (from a stack)
  - **Shift+X** drop the **whole stack**
  - **Shift+S** sort the inventory
  - **Enter** performs a **context action** (equip if gear, use if consumable)
- **Game controller support** (SDL2 GameController): D-pad move, **A** confirm, **B** cancel, **X** inventory, **Y** pickup, shoulders for look/fire.
- **VSync + FPS cap settings**: new `vsync` and `max_fps` keys in `procrogue_settings.ini`.

- **Configurable keybindings**: add `bind_*` entries in `procrogue_settings.ini` (multiple keys per action).
- **In-game options menu**: press **F2** to tweak common settings (auto-pickup, autosave, etc.).
- **NetHack-like extended commands**: press **#** (Shift+3) to open a command prompt (`save`, `load`, `pray`, `quit`, ...).

- **Auto-travel**: enter look mode (`L` / `V` or **right-click**) and press **Enter** to auto-walk to the cursor tile.
- **Auto-explore**: press **O** to walk to the nearest unexplored frontier until interrupted.
- **Auto-pickup modes**: press **P** to cycle **OFF → GOLD → ALL → OFF**.
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
- **Run history / high scores** are stored in `procrogue_scores.csv`.
- **Mouse support**:
  - **Left click**: auto-travel to the clicked tile
  - **Right click**: enter look mode at the clicked tile
  - **Mouse move**: moves the look cursor / aiming cursor
  - **Mouse wheel**: scroll message log
- **Settings file** (auto-created on first run): tweak tile size, HUD height, fullscreen, auto-move speed, auto-pickup/autosave, and `bind_*` keybindings.
- **Optional hunger system**: enable `hunger_enabled` to add food + starvation (and a new **Food Ration** item).
- **Safer quitting**: `confirm_quit` (default ON) requires **ESC twice** to exit (prevents accidental quits).
- **Context pickup**: when not on stairs, **Enter** will pick up items you’re standing on.

- **NetHack-style item identification**: potions + scrolls start unknown each run (randomized appearances). Using them identifies the item type; you can also find/read a **Scroll of Identify**. (Toggle via `identify_items` in settings.)
- **New scroll**: **Scroll of Detect Traps** reveals all traps on the current floor (and identifies itself when used).
- **Shrines**: use **#pray** (`pray heal|cure|identify|bless`) to spend gold for a small blessing (consumes a turn).
- **New trap**: **Web traps** can immobilize you for a few turns.
- **New combat spice**: spiders can web you (movement blocked for a few turns), and wizards can occasionally "blink" (teleport) to reposition.
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
- **Auto-pickup mode**: `P` (cycles OFF/GOLD/ALL)
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

This build uses **SDL_GetPrefPath** so saves and settings live in a per-user writable folder:

- `procrogue_save.dat`
- `procrogue_autosave.dat`
- `procrogue_settings.ini`
- `procrogue_scores.csv`
- `screenshots/`

The settings file is created automatically on first run.

## Command-line

- Start with a specific seed:
  - `procrogue --seed 12345`
- Auto-load the save on startup:
  - `procrogue --load`

- Print the game version:
  - `procrogue --version`

- Show command-line help:
  - `procrogue --help`

## Building

See `docs/BUILDING.md`.

To build the unit tests:
```bash
cmake -S . -B build -DPROCROGUE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
