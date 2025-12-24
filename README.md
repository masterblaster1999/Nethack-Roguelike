# ProcRogue++

A small SDL2 roguelike with **procedurally generated dungeons** and **procedurally generated pixel sprites**.

This is a **massive gameplay upgrade** over the initial drop:

- **Items + inventory**
  - Pick up, drop, use, and equip items (weapon + armor)
  - Separate **melee** and **ranged** equipment slots (keep a sword equipped while firing a bow)
  - Stackable ammo + gold
- **Improved dungeon generator**
  - BSP-ish room layout
  - Door placement, multiple corridors, loops + dead ends
  - Special rooms (Treasure / Lair / Shrine)
- **More monsters + behaviors**
  - Ranged enemies (archer/slinger)
  - Fleeing behavior when hurt
  - Pack AI for wolves
- **Animated procedural sprites**
  - 2-frame flicker/bob animation for entities/items/FX
- **Targeting + projectiles**
  - Aim with a cursor and fire with visible projectiles
- **Proper message log scrollback**
  - PageUp/PageDown scrolls combat + loot history

## Controls

Movement:
- **WASD** / **Arrow keys**: move
- **.** or **Space**: wait

Dungeon:
- **Enter** or **>**: use stairs down (when standing on them)

Items:
- **G**: pick up items on your tile
- **I**: inventory (toggle)
  - **Up/Down**: select
  - **E**: equip / unequip (weapon or armor)
    - Melee weapons (dagger/sword) go to the **Melee** slot
    - Bows/wands go to the **Ranged** slot
    - Armor goes to the **Armor** slot
  - **U**: use (potion/scroll)
  - **X**: drop
  - **Esc**: close inventory

Ranged targeting:
- **F**: start targeting (requires a ranged weapon in your **Ranged** slot)
- **Arrow keys**: move target cursor
- **Enter**: fire
- **Esc**: cancel targeting

Message log:
- **PageUp / PageDown**: scroll message history

Other:
- **R**: restart
- Close the window / **Esc** (when not in inventory/targeting): quit

## Build

Requirements:
- CMake
- SDL2

Typical CMake build:

```bash
mkdir -p build
cd build
cmake ..
cmake --build . --config Release
```

If SDL2 isn't found automatically, make sure it’s installed for your platform and discoverable by CMake.
