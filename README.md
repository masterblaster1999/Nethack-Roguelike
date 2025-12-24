# ProcRogue++

A small SDL2 roguelike with **procedurally generated dungeons** and **procedurally generated pixel sprites**.

This branch upgrades the original drop with bigger, more "roguelike" systems:

- **Multi-level dungeon persistence**
  - Stairs up/down, and previously visited depths remain in memory (exploration + monster/item state)
- **Win condition + quest item**
  - Find the **Amulet of Yendor** on **Depth 5**, then return to the exit (**<** on Depth 1)
- **Save / Load (quick keys)**
  - Save to a local file and resume later
- **Character progression**
  - XP from kills, character levels, and stat growth
- **New monsters**
  - **Troll** (regenerates)
  - **Wizard** (ranged magic)
- **New items**
  - **Sling** (rocks for ammo)
  - **Potion of Strength** (+ATK)
  - **Scroll of Mapping** (reveals the explored map)
  - **Amulet of Yendor** (quest item)
- **Help overlay**
  - In-game key reference (press **?**)

## Controls

Movement:
- **WASD** / **Arrow keys**: move
- **.** or **Space**: wait

Dungeon:
- **>**: stairs down (when standing on them)
- **<**: stairs up (when standing on them)
- **Enter**: context action (uses stairs up/down if you are standing on them)

Items:
- **G**: pick up items on your tile
- **I**: inventory (toggle)
  - **Up/Down**: select
  - **E**: equip / unequip
    - Melee weapons (dagger/sword) go to the **Melee** slot
    - Ranged weapons (bow/sling/wand) go to the **Ranged** slot
    - Armor goes to the **Armor** slot
  - **U**: use (potion/scroll)
  - **X**: drop
  - **Esc**: close inventory

Ranged targeting:
- **F**: start targeting (requires a ranged weapon in your **Ranged** slot)
- **Arrow keys**: move target cursor
- **Enter**: fire
- **Esc**: cancel targeting

UI / meta:
- **?**: help overlay
- **F5**: save
- **F9**: load
- **PageUp / PageDown**: scroll message history
- **R**: restart
- Close the window / **Esc** (when not in inventory/targeting/help): quit

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
