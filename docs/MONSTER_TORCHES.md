# Monster Torches

Darkness mode is most interesting when information is *negotiated* instead of binary. This feature adds **mobile light** via torch-bearing monsters.

## Summary

On dark floors (where `Game::darknessActive()` is true):

- Some humanoid-ish monsters may spawn with a **Torch** (or occasionally a **Lit Torch**) in their `pocketConsumable` slot.
- When **alerted** but unable to see the player due to darkness, they can spend a turn to **light the torch**.
- Lit torches contribute to the gameplay **lightmap** and the renderer’s subtle **flicker**, so darkness can “move” with enemies.
- Torches burn down over time like player and ground torches; when a torchbearer dies, the torch drops like other pocket items.

## Gameplay implications

- Stealth in dark corridors remains strong, but “torchbearers” can reintroduce vision at a cost (spending a turn and making noise).
- Enemies carrying torches are easier to track and spot (they reveal themselves and their surroundings), creating interesting tradeoffs.
- A lit torch dropped by a dead torchbearer becomes usable loot (and will continue to burn down).

## Implementation notes

- **Spawn:** `Game::makeMonster()` (`src/game_spawn.cpp`) assigns pocket torches for **Goblins, Orcs, Guards, Kobold Slingers, and Shopkeepers** on dark floors.
- **AI:** `Game::monsterTurn()` (`src/ai.cpp`) lights an unlit torch when the monster is alerted, cannot see the player, the player isn’t currently lit, and the distance is plausible.
  - Lighting emits a small noise burst and triggers `recomputeFov()` so visibility updates immediately.
- **Lighting:** `Game::recomputeLightMap()` (`src/game_world.cpp`) now includes carried torches as light sources (capped for performance).
- **Rendering:** `src/render.cpp` includes carried torches in the flicker source list.
- **Burn-down:** carried `TorchLit` items in monster pocket slots decrement `charges` once per turn in the same tick that handles player + ground torches.

## Tuning knobs

Look in the spawn/AI blocks if you want to tune:

- Spawn chance per monster kind
- Starting fuel range
- AI “light torch” probability and distance window
- Carried torch radius/intensity (currently slightly dimmer than the player’s torch)
