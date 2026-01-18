# Hearing Preview (Audibility Lens)

This game has several **LOOK lenses**: UI-only tactical overlays that help you reason about the dungeon without advancing time.

The **Hearing Preview** ("audibility lens") answers a simple question:

> **If I step onto this tile, would any *currently visible* hostile hear the footstep?**

It uses the same acoustic model as the game's sound system (walls block, corridors carry, etc.), and it is **visibility-gated**: only enemies you can currently see are treated as listeners, so the overlay never leaks unseen monsters.

## Default keybind

- Toggle: **Ctrl+H** (rebindable via `bind_hearing_preview`)

## How to use

1. Enter LOOK mode.
2. Toggle **Hearing Preview**.
3. **Purple tiles** mean a footstep on that tile would be **audible** to at least one visible hostile.
4. **White outlines** mark the listener monsters the field was computed from.
5. Use **[ / ]** (minimap zoom keys) to adjust a *volume bias* and simulate louder/quieter actions.

## What the numbers mean

Internally, each tile gets:

- `REQ`: the minimum sound **volume** required at that tile to be heard by any visible hostile.
- `STEP`: your current **footstep volume** if you were standing on that tile.

A tile is marked audible if:

```
STEP + bias >= REQ
```

## Implementation notes

- Built as a multi-source **Dijkstra-style cost field** seeded from all visible hostile listeners.
- Per-monster hearing stats are converted into offsets so the final field directly encodes "minimum required volume".
- The overlay only draws on **explored tiles**.
