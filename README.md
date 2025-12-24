# ProcRogue (first playable drop)

A small, NetHack-inspired, turn-based dungeon crawler written in **C++17 + SDL2**.

**All graphics are generated at runtime** (procedural 16×16 pixel sprites and tile textures). No external art assets.

This is intended to be a clean starting point that we can iterate on heavily (items, inventory, more monsters, more levels, better FOV, UI polish, etc).

---

## Features (so far)

- Procedural dungeon (rooms + corridors)
- Player + monsters with simple combat (bump to attack)
- Simple monster AI (wander, chase when close)
- Field-of-view + exploration memory (fog of war)
- Procedural pixel sprites for:
  - Floors, walls, stairs
  - Player + monsters (each has a unique seed → unique sprite)
- Minimal HUD with bitmap font (no SDL_ttf dependency)

---

## Controls

- **Move:** Arrow keys or WASD
- **Wait (do nothing):** `.` (period) or `Space`
- **Go down stairs:** `>` or `Enter` (only works while standing on stairs)
- **Restart:** `R`
- **Quit:** `Esc`

---

## Build (CMake)

### Linux (Ubuntu / Debian)

Install SDL2 dev:

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ libsdl2-dev
```

Build & run:

```bash
cmake -S . -B build
cmake --build build -j
./build/proc_rogue
```

### macOS (Homebrew)

```bash
brew install sdl2 cmake
cmake -S . -B build
cmake --build build -j
./build/proc_rogue
```

### Windows

Recommended easiest path:

- Install **Visual Studio 2022** with C++ Desktop workload
- Install **vcpkg**
- `vcpkg install sdl2`

Then configure CMake with the vcpkg toolchain:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
.uild\Release\proc_rogue.exe
```

---

## Notes

- This project defines `SDL_MAIN_HANDLED`, so it should not strictly require `SDL2main`.
- All sprite generation is contained in `src/spritegen.*` and is meant to be extended:
  - animations
  - equipment overlays
  - per-biome tile sets
  - etc.

---

## License

MIT (see LICENSE).
