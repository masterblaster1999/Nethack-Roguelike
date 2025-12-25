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

### Gameplay QoL

- `auto_pickup` (`off | gold | all`, default `gold`)
  - `off`: never auto-pickup
  - `gold`: auto-pickup gold only
  - `all`: auto-pickup any item you step on

- `auto_step_delay_ms` (int, default `45`)
  - Clamped to `10..500`
  - Lower = faster auto-travel / auto-explore

- `autosave_every_turns` (int, default `200`)
  - Clamped to `0..5000`
  - `0` disables autosave
  - Autosave writes to `procrogue_autosave.dat`

- `identify_items` (`true/false`, default `true`)
  - `true`: potions/scrolls start unidentified each run (NetHack-style)
  - `false`: items always show their true names (more "arcade" / beginner-friendly)
