# Contributing

Thanks for helping!

## Quick dev setup

- Build instructions live in `docs/BUILDING.md`.
- Settings docs live in `docs/SETTINGS.md`.

## Style

- C++: 4 spaces, no tabs.
- Keep save-file changes **append-only** and bump the save version.
- Prefer small, readable helpers over giant functions (especially in `game.cpp`).

## Gameplay changes

- Try to keep new mechanics optional where possible (via settings).
- If you add new item kinds or monster kinds, **append** to the enum to avoid breaking old saves.