# Annex Procgen: Fractal L-System Style

This document describes a new annex micro-dungeon style that generates a **highly branching pocket layout** by
interpreting a small **stochastic L-system** (turtle + stack grammar) directly onto the annex grid.

It aims for a different feel than:

- **Perfect mazes** (tree-like, evenly spaced corridors)
- **CA caverns** (blobby, organic open space)
- **WFC pipe networks** (constraint-synthesized connectivity tiles)

The fractal annex reads like **branching roots / coral tunnels**: compact, choice-dense, and locally structured.

## Core idea

We build a short L-system string and interpret it as a turtle walk:

- `F` = step forward + carve
- `+` = turn right 90°
- `-` = turn left 90°
- `[` = push current turtle state (position + direction)
- `]` = pop turtle state

A simple stochastic rewrite rule expands `F` into one of a few branching patterns (biased toward the classic
`F[+F]F[-F]F`). The resulting command string is then executed inside the annex interior ring.

## Shaping passes

After the raw turtle carve:

1. **Entry breathing room**: open a small 3×3 area around the annex entrance.
2. **Junction chambers**: some degree-3+ nodes are inflated into tiny 3×3 chambers.
3. **Dead-end buds**: some dead ends grow a 1–2 tile alcove (more satisfying exploration tips).
4. **Sanity checks**: require a minimum amount of carved floor so the annex doesn’t collapse into a tiny scribble.

## When it shows up

The fractal annex is weighted toward **organic** main-floor generators (`Cavern`, `Warrens`) and appears less often
on strict mazes. Depth slightly increases its density and shaping intensity.

## Debugging

Run `#mapstats`:

- `ANNEXES N | FRACTAL F` shows how many annexes were carved and how many used the fractal style.
- `ANNEXES N | KEYGATES K` still indicates internal lock-and-key puzzles (may also appear in fractal annexes).
