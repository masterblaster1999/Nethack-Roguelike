# Procedural Shrines (Patrons + Domains)

This project already has *Shrines* as a gameplay room type (altars that accept **donations**, **sacrifices**, and **prayer services**).

This module adds a lightweight procgen layer on top:

- Every **Shrine room** gets a deterministic **Patron** (a deity name + epithet).
- Every patron has a **Domain** that biases shrine services toward a particular style (heal / cure / identify / bless / uncurse / recharge).
- The profile is derived from **(runSeed, depth, shrine room rect)** and is therefore:
  - stable within a run
  - stable across saves
  - requires **no new save fields**

## What this changes in-game

### 1) HUD + LOOK

- When standing inside a shrine room, the HUD shows:
  - `SHRINE: <NAME> (<DOMAIN>)`
- Looking at an altar also shows the same tag (and still shows `CAMP ALTAR` at the camp).

### 2) Domain-tuned service costs

Prayer services now have a small cost multiplier depending on the shrine’s domain:

- The **favored** service is cheaper.
- A couple of them get a mild “adjacent” discount (for example, Mercy also discounts Cure).
- Other services are slightly more expensive.

This is a simple way to create shrine variety without turning the system into a full skill tree.

### 3) Patron resonance (small bonus per prayer)

Any successful shrine prayer applies a small, domain-flavored extra effect. Examples:

- **Mercy**: extra regen + a small “top-off” heal.
- **Cleansing**: clears corrosion + hallucinations.
- **Insight**: grants vision and reveals nearby traps / secret doors.
- **Benediction**: grants parry + extra shielding.
- **Purging**: may uncurse a single item in your pack.
- **Artifice**: may add a charge to a wand.

The goal is to make *where* you pray matter, while keeping shrines recognizable and readable.

## Implementation notes

- Implementation is in `src/shrine_profile_gen.hpp`.
- Profile generation is deterministic and does not use global RNG state.
- UI hooks are in:
  - `src/render.cpp`
  - `src/game_look.cpp`
  - `src/game_interact.cpp`

