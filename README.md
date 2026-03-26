# Nethack Roguelike

<img width="3508" height="1837" alt="Screenshot 2026-01-19 223849" src="https://github.com/user-attachments/assets/3c3e0797-77e2-405e-b7db-72694e585c66" />

ProcRogue is a small NetHack-inspired roguelike with turn-based dungeon crawling, procedural content, optional isometric and raycast views, replay recording, and a large amount of runtime-tunable behavior.

## Fastest Path

If you only need the shortest route to a working build:

- `procrogue.bat play` - play immediately on Windows
- `procrogue.bat test` - build and run the SDL-free test suite on Windows
- `procrogue.bat smoke` - run the headless replay/perf smoke lane on Windows
- [docs/BUILDING.md](docs/BUILDING.md) - Linux/macOS and advanced build flows

Standalone tools:

- `ProcRogueHeadless` verifies replays, generates curated golden replays, and runs fixed-seed headless perf suites.
- `ProcRogueRenderPerf` runs the render perf suite.
- Both tools use the same CLI style: `--help` / `-h`, `--version` / `-v`, and value-taking options accept either `--name value` or `--name=value`.

## First Successful Run

- [ ] Launch the game with `procrogue.bat play` on Windows, or build/run `./build/ProcRogue` after following [docs/BUILDING.md](docs/BUILDING.md) on Linux/macOS.
- [ ] Verify the basic keybinds: move with `WASD`, arrow keys, or `Q/E/Z/C`; wait with `.` or `Space`; open inventory with `I`; pick up with `G`.
- [ ] Confirm where saves and settings live: by default they go into the per-user writable directory from `SDL_GetPrefPath`; use `--portable` or `--data-dir <path>` to override that.
- [ ] Use `F1` for help, `F2` for options, and `#help` for the extended command list.

## What Is In The Game

- A 20-floor run with multiple layout families, secrets, traps, vaults, shops, shrines, and a surface camp hub.
- Procedural sprites and multiple presentation modes, including classic 2D, isometric, and raycast-style first-person rendering.
- Save/load, autosaves, replay recording, deterministic verification tools, and runtime content overrides.

## Repository Layout

- `src/` is the canonical source tree for the game, headless tools, and renderer.
- `tests/` contains the SDL-free automated test suite and replay/perf harnesses.
- `docs/` contains build notes and feature/design documentation.
- Generated local output such as `build/` and IDE metadata such as `.vs/` should stay out of distributed project copies.

## Documentation

Start here:

- [Building and test flows](docs/BUILDING.md)
- [Settings, keybinds, and data file locations](docs/SETTINGS.md)
- [Extended command reference](docs/COMMANDS.md)
- [Replay recording and playback](docs/REPLAY_RECORDING.md)
- [Challenge ladder, daily/weekly cadence, and curated seed events](docs/CHALLENGE_LADDER.md)
- [Factions and reputation](docs/FACTIONS_AND_REPUTATION.md)
- [Runtime mod packs and content overrides](docs/MODPACK_TOML.md)
- [Item identification system](docs/IDENTIFICATION.md)
- [Changelog](CHANGELOG.md)
- [Contributing](CONTRIBUTING.md)
- [Full docs index](docs/README.md)

To refresh the generated settings and command-reference pages, build the `docs_dump` target.

Common topic guides:

- [Overworld atlas and generation](docs/PROCGEN_OVERWORLD_ATLAS.md)
- [Shops and market systems](docs/PROCGEN_SHOPS.md)
- [Shrines, piety, and prayer](docs/PROCGEN_SHRINES.md)
- [Win conditions](docs/PROCGEN_WIN_CONDITIONS.md)
- [Raycast 3D view](docs/PROCGEN_RAYCAST3D_VIEW.md)
- [Isometric terrain notes](docs/ISO_TERRAIN_VOXELS.md)

## Saves and Settings

By default, ProcRogue stores saves and settings in the per-user writable directory returned by `SDL_GetPrefPath`.

Useful runtime options:

- `--portable` to store data next to the executable
- `--data-dir <path>` to choose an explicit directory
- `--slot <name>` to use a named save slot, or `default` / `none` / `off` / `auto` for the normal slot
- `--validate-content` or `--dry-run` to validate the active modpack/override file and exit before starting a run

Common files:

- `procrogue_settings.ini`
- `procrogue_save.dat`
- `procrogue_autosave.dat`
- `procrogue_scores.csv`
- `screenshots/`
- `procrogue_modpack.toml`, `procrogue_modpack.json`, or `procrogue_content.ini`
