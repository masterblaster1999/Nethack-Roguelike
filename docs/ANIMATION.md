# Procedural Animation

This project is turn-based, but the renderer can still add **visual-only** motion so actions feel less “teleporty” while keeping gameplay fully discrete.

## What’s animated

### Entities
When an entity changes tile position, the renderer plays a short procedural move animation:

- **Smooth tween** between the previous and current tile.
- **Hop arc** (a sine bump) so the sprite lifts slightly mid-step.
- **Squash & stretch** (subtle scale warp) to make the step feel more alive.

When an entity takes damage (HP decreases), the renderer triggers a quick:

- **Recoil / kickback** (screen-space offset), biased away from the player when possible.

When standing still, entities get a subtle:

- **Idle bob** (small vertical sine wave) with a per-entity phase.

### Ground items
Items bob gently so they read as “pickups,” and in isometric view they get a small diamond ground shadow that shrinks/fades slightly as the item rises.

### Projectiles
Projectiles lerp between path tiles using their step timer, with a small arc lift mid-segment.

## Implementation notes

All of this lives in the **Renderer** and does not affect simulation:

- `Renderer::updateProceduralAnimations(...)` tracks per-entity state (last position, last HP, and active move/hurt timers) in a map keyed by entity id.
- `Renderer::render(...)` uses small helper lambdas (`sampleEntityAnim`, `itemBob`) to convert game state into animated `SDL_Rect` destinations.

The goal is to keep it simple, fast, and purely cosmetic. You can tune constants (durations, hop height, bob amplitude) directly in `render.cpp`.
