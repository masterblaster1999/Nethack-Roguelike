# Nethack Roguelike

A tiny NetHack-inspired roguelike with:
- Turn-based combat and dungeon crawling
- Procedurally generated pixel sprites (player, monsters, items)
- Field-of-view + explored/seen tiles
- Traps, potions/scrolls, ranged attacks, stairs, saving/loading

## New in this build (QoL + big usability upgrades)

- **Auto-travel**: enter look mode (`L` / `V` or **right-click**) and press **Enter** to auto-walk to the cursor tile.
- **Auto-explore**: press **O** to walk to the nearest unexplored frontier until interrupted.
- **Auto-pickup modes**: press **P** to cycle **OFF → GOLD → ALL → OFF**.
- **Mouse support**:
  - **Left click**: auto-travel to the clicked tile
  - **Right click**: enter look mode at the clicked tile
  - **Mouse move**: moves the look cursor / aiming cursor
  - **Mouse wheel**: scroll message log
- **Settings file** (auto-created on first run): lets you tweak tile size, HUD height, fullscreen, auto-move speed, and default auto-pickup mode.
- **CI build workflow** for Linux/macOS/Windows.

## Controls

### Movement / exploration
- **Move**: WASD / Arrow keys
- **Wait**: Space
- **Look**: `L` or `V` (or right-click)
- **Auto-travel**: Enter while looking (or left-click a tile)
- **Auto-explore**: `O`
- **Search (reveal traps)**: `C`
- **Auto-pickup mode**: `P` (cycles OFF/GOLD/ALL)

### Interaction
- **Pick up**: `G`
- **Inventory**: `I`
- **Fire ranged**: `F` (aim with mouse or WASD/arrows, **Enter** or **left-click** to fire, **right-click** or **Esc** cancels)
- **Use stairs**: `<` up, `>` down

### Meta
- **Help**: `?` or `H`
- **Save / Load**: `F5` / `F9`
- **Fullscreen**: `F11`
- **Message log scroll**: PageUp / PageDown (or mouse wheel)
- **Restart**: `R`
- **Quit**: Esc (when no UI mode is active)

## Save files + settings location

This build uses **SDL_GetPrefPath** so saves and settings live in a per-user writable folder:

- `procrogue_save.dat`
- `procrogue_settings.ini`

The settings file is created automatically on first run.

## Command-line

- Start with a specific seed:
  - `procrogue --seed 12345`
- Auto-load the save on startup:
  - `procrogue --load`

## Building

See `docs/BUILDING.md`.
