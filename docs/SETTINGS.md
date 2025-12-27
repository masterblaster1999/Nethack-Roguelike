# Settings

On first run, ProcRogue creates a simple settings file next to the save file:

- `procrogue_settings.ini`
- `procrogue_save.dat`

The exact folder is provided by **SDL_GetPrefPath** (a per-user writable directory).

Related files created in the same folder:

- `procrogue_autosave.dat` (automatic periodic save)
- `procrogue_scores.csv` (run history / high scores)
- `screenshots/` (F12 BMP screenshots)

## Keys

All keys use the form:

```
key = value
```

Comments start with `#` or `;`.

### Rendering / UI

- `tile_size` (int, default `32`)
  - Clamped to `16..96`
  - Larger tiles = bigger UI

- `hud_height` (int, default `160`)
  - Clamped to `120..240`

- `start_fullscreen` (`true/false`, default `false`)

- `player_name` (string, default `PLAYER`)
  - Used in the HUD + scoreboard.

- `show_effect_timers` (`true/false`, default `true`)
  - When `true`, status tags show remaining turns (example: `POISON(6)`).

- `vsync` (`true/false`, default `true`)
  - When `true`, the renderer uses vsync (smoother animation, lower CPU usage).

- `max_fps` (int, default `0`)
  - `0` disables the FPS cap (only a small yield delay is used).
  - When `vsync = false`, values are clamped to `30..240`.

### Input

- `controller_enabled` (`true/false`, default `true`)
  - Enables SDL2 GameController support (D-pad movement, A confirm, etc.).

### Gameplay QoL

- `auto_pickup` (`off | gold | all`, default `gold`)
  - `off`: never auto-pickup
  - `gold`: auto-pickup gold only
  - `all`: auto-pickup any item you step on

- `auto_step_delay_ms` (int, default `45`)
  - Clamped to `10..500`
  - Lower = faster auto-travel / auto-explore
- `confirm_quit` (`true/false`, default `true`)
  - When `true`, quitting via **ESC** requires pressing ESC twice (prevents accidental quits).

- `hunger_enabled` (`true/false`, default `false`)
  - Enables an optional hunger system.
  - Adds *Food Rations* to loot tables.
  - When hunger reaches zero, you take starvation damage over time until you eat.


- `autosave_every_turns` (int, default `200`)
  - Clamped to `0..5000`
  - `0` disables autosave
  - Autosave writes to `procrogue_autosave.dat`

- `identify_items` (`true/false`, default `true`)
  - `true`: potions/scrolls start unidentified each run (NetHack-style)
  - `false`: items always show their true names (more "arcade" / beginner-friendly)


### Keybindings

You can rebind most keyboard controls by adding `bind_<action>` entries to `procrogue_settings.ini`.

Format:

```
bind_<action> = key[, key, ...]
```

- Multiple keys can be bound to the same action by separating them with commas.
- Modifiers are written as `shift+`, `ctrl+`, or `alt+` prefixes (example: `shift+comma`).
- Key names are case-insensitive.

Examples:

```
# NetHack-ish extended command key:
bind_command = shift+3

# Classic roguelike diagonals:
bind_up_left = y
bind_up_right = u
bind_down_left = b
bind_down_right = n

# Alternative stairs keys:
bind_stairs_up = shift+comma, less
bind_stairs_down = shift+period, greater
```

Common key names:

- Letters and digits: `w`, `a`, `1`, `9`
- Directions: `up`, `down`, `left`, `right`
- Punctuation: `comma`, `period`, `slash`
- Other keys: `tab`, `space`, `enter`, `escape`, `pageup`, `pagedown`
- Numpad: `kp_0` .. `kp_9`, `kp_enter`
- Function keys: `f1` .. `f24`

Available actions:

**Movement**
- `up`, `down`, `left`, `right`
- `up_left`, `up_right`, `down_left`, `down_right`

**Gameplay**
- `confirm`, `cancel`
- `wait`, `rest`
- `pickup`, `inventory`
- `fire`, `search`, `look`
- `stairs_up`, `stairs_down`
- `auto_explore`, `toggle_auto_pickup`

**Inventory**
- `equip`, `use`, `drop`, `drop_all`, `sort_inventory`

**UI / Meta**
- `help`, `options`, `command`
- `toggle_minimap`, `toggle_stats`
- `fullscreen`, `screenshot`
- `save`, `load`, `load_auto`, `restart`
- `log_up`, `log_down`

Tip: For `<` and `>` on most keyboard layouts, prefer `shift+comma` and `shift+period`.
