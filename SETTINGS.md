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

- `minimap_zoom` (int, default `0`)
  - Clamped to `-3..3`
  - Minimap overlay zoom (0 = default auto-fit)

- `start_fullscreen` (`true/false`, default `false`)

- `view_mode` (string, default `topdown`)
  - `topdown`: classic 2D grid view
  - `isometric`: experimental 2.5D isometric camera
  - You can also toggle this in-game (default keybind: **F7**).
  - In isometric mode, terrain is rendered using diamond-projected tiles and taller wall blocks.

- `voxel_sprites` (`true/false`, default `true`)
  - When `true`, entities/items/projectiles are rendered using the voxel (3D) procedural sprite pipeline.
  - In `isometric` view mode, voxel sprites are re-rendered using a matching **isometric/dimetric** projection so their shading and silhouette line up with the 2.5D terrain.
  - When `false`, those sprites use the 2D procedural pipeline.
  - You can toggle this at runtime (default keybind: **F8**) or via `#sprites3d on|off`.

- `iso_voxel_raytrace` (`true/false`, default `false`)
  - **Isometric view only** (ignored in `topdown`).
  - When `true`, isometric voxel sprites are rendered using a **custom orthographic voxel raytracer** (DDA traversal) instead of the face-meshed isometric rasterizer.
  - This is more expensive to generate, but sprites are cached, so it mainly affects *generation time* (not per-frame rendering).
  - Toggle at runtime via `#isoraytrace on|off|toggle` (this also clears the sprite cache so textures regenerate cleanly).

- `iso_terrain_voxels` (`true/false`, default `true`)
  - **Isometric view only** (ignored in `topdown`).
  - When `true`, isometric terrain "block" sprites (walls/doors/pillars/boulders) are generated from small voxel models, so they match the voxel-sprite shading pipeline.
  - When `iso_voxel_raytrace` is enabled, terrain blocks use raytrace **only** for small tile sizes (<=64px); larger sizes fall back to the faster mesh renderer to avoid long stalls.
  - Toggle at runtime via `#isoterrainvox on|off|toggle` (alias: `#isoblocks`).

- `player_name` (string, default `PLAYER`)
  - Used in the HUD + scoreboard.

- `player_class` (string, default `adventurer`)
  - Starting class/role for **new runs**.
  - Valid values: `adventurer`, `knight`, `rogue`, `archer`, `wizard`.
  - You can also change this in-game with `#class <name>` (restarts the run).

- `show_effect_timers` (`true/false`, default `true`)
  - When `true`, status tags show remaining turns (example: `POISON(6)`).

- `show_perf_overlay` (`true/false`, default `false`)
  - Shows a tiny **performance HUD** (FPS + sprite cache stats) in the top-left.
  - Toggle in-game via **Shift+F10** (default) or `#perf on/off`.

- `vsync` (`true/false`, default `true`)
  - When `true`, the renderer uses vsync (smoother animation, lower CPU usage).

- `max_fps` (int, default `0`)
  - `0` disables the FPS cap (only a small yield delay is used).
  - When `vsync = false`, values are clamped to `30..240`.

### Input

- `controller_enabled` (`true/false`, default `true`)
  - Enables SDL2 GameController support (D-pad movement, A confirm, etc.).

### Gameplay QoL

- `auto_pickup` (`off | gold | smart | all`, default `gold`)
  - `off`: never auto-pickup
  - `gold`: auto-pickup gold only
  - `smart`: auto-pickup “core” items (gold, keys/lockpicks, consumables, and equipment; ammo only if you have the matching ranged weapon)
  - `all`: auto-pickup any item you step on

- `auto_step_delay_ms` (int, default `45`)
  - Clamped to `10..500`
  - Lower = faster auto-travel / auto-explore
- `confirm_quit` (`true/false`, default `true`)
  - When `true`, quitting via **ESC** requires pressing ESC twice (prevents accidental quits).

- `auto_mortem` (`true/false`, default `true`)
  - When `true`, ProcRogue writes a `procrogue_mortem_*.txt` full-state dump automatically when you **win** or **die**.
  - You can toggle this in-game with `#mortem on|off`.


- `hunger_enabled` (`true/false`, default `false`)
  - Enables an optional hunger system.
  - Adds *Food Rations* to loot tables.
  - When hunger reaches zero, you take starvation damage over time until you eat.


- `encumbrance_enabled` (`true/false`, default `false`)
  - Enables an optional carrying capacity / burden system.
  - When enabled, your total inventory weight affects movement pacing (extra monster turns when heavily burdened).
  - Overloaded characters cannot use stairs.


- `lighting_enabled` (`true/false`, default `false`)
  - Enables an optional darkness / lighting system.
  - Deeper floors can have darkness that limits your vision unless you carry a light source (torches).


- `yendor_doom_enabled` (`true/false`, default `true`)
  - Enables an optional endgame escalation after you acquire the *Amulet of Yendor*.
  - The dungeon will periodically emit loud "doom" noises and spawn hunter packs as you try to ascend.


- `autosave_every_turns` (int, default `200`)
  - Clamped to `0..5000`
  - `0` disables autosave
  - Autosave writes to `procrogue_autosave.dat`

- `default_slot` (string, default empty)
  - Empty (or `default`) uses the standard filenames:
    - `procrogue_save.dat`
    - `procrogue_autosave.dat`
  - Non-empty uses slot filenames:
    - `procrogue_save_<slot>.dat`
    - `procrogue_autosave_<slot>.dat`
  - You can set this in-game with `#slot <name>`.

- `save_backups` (int, default `3`)
  - Clamped to `0..10`
  - `0` disables backups
  - Keeps rotated backups for both manual saves and autosaves:
  - On load, if the primary save/autosave is corrupt (or fails CRC), the game will automatically
    try the newest backup first (`.bak1`, then `.bak2`, etc.) before giving up.
    - `procrogue_save.dat.bak1..bakN`
    - `procrogue_autosave.dat.bak1..bakN`

- `identify_items` (`true/false`, default `true`)
  - `true`: potions/scrolls start unidentified each run (NetHack-style)
  - `false`: items always show their true names (more "arcade" / beginner-friendly)


### Keybindings

You can rebind most keyboard controls by adding `bind_<action>` entries to `procrogue_settings.ini`.

Tip: you can also manage these in-game using `#binds`, `#bind <action> <keys>`, `#unbind <action>`, and `#reload`.

You can also use the **Options → Keybinds** editor:

- **Up/Down**: select action
- **Enter**: rebind (replace)
- **Right**: add another key to the action
- **Left**: reset to default (removes the `bind_` override)
- **Delete**: unbind/disable (writes `none`)
- **/**: filter/search (type to narrow the list)
- **Ctrl+L**: clear the filter


Format:

```
bind_<action> = key[, key, ...]
```

- Multiple keys can be bound to the same action by separating them with commas.
- Modifiers are written as `shift+`, `ctrl+`, `alt+`, or `cmd+` prefixes (example: `shift+comma`).

  - On macOS, `cmd+` corresponds to the Command key. On Windows/Linux it maps to the GUI/Windows/Super key.
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
- Punctuation: `comma`, `period`, `slash`, `backslash`
- Other keys: `tab`, `space`, `enter`, `escape`, `pageup`, `pagedown`
- Numpad: `kp_0` .. `kp_9`, `kp_enter`
- Function keys: `f1` .. `f24`

Available actions:

**Movement**
- `up`, `down`, `left`, `right`
- `up_left`, `up_right`, `down_left`, `down_right`

**Gameplay**
- `confirm`, `cancel`
- `wait`, `rest`, `sneak` (toggle sneak mode: quieter actions, weaker scent trail, shorter enemy sight range, but slower)
- `pickup`, `inventory`
- `fire`, `search`, `disarm`
- `close_door`, `lock_door`
- `kick`, `dig`
- `look`
- `stairs_up`, `stairs_down`
- `auto_explore`, `toggle_auto_pickup`
**Inventory**
- `equip`, `use`, `drop`, `drop_all`, `sort_inventory`

**UI / Meta**
- `help`, `options`, `command`
- `sound_preview`, `hearing_preview`, `threat_preview` (LOOK lenses: sound propagation, audibility, and threat ETA heatmaps)
- `message_history`, `codex`, `discoveries`
- `toggle_minimap`, `minimap_zoom_in`, `minimap_zoom_out`, `toggle_stats`, `toggle_perf_overlay`
- `fullscreen`, `screenshot`
- `save`, `load`, `load_auto`, `restart`
- `log_up`, `log_down`

Tip: For `<` and `>` on most keyboard layouts, prefer `shift+comma` and `shift+period`.
