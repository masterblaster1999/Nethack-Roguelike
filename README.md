# Nethack-Roguelike (ProcRogue)

A small NetHack-inspired roguelike written in **C++17** using **SDL2**.

Sprites are procedurally generated at runtime (no external art assets), and the game features multiple dungeon floors, traps, ranged combat, equipment, and save/load.

---

## What's new in this upgraded build

* **Modernized CMake**: clearer errors, better Windows support, optional auto-download of SDL2.
* **Improved Field-of-View**: replaced per-tile raycasts with **shadowcasting** for smoother visibility.
* **Look/Examine mode**: press **L** (or **V**) to move a cursor and inspect tiles (**no turn consumed**).
* **Rest command**: press **Z** to rest until healed (interrupts if an enemy becomes visible).
* **New consumables**:
  * **Potion of Haste** – grants extra actions (monsters act every other player action).
  * **Potion of Vision** – increases sight radius temporarily.
* **Resizable window + fullscreen toggle**: resize freely, toggle fullscreen with **F11**.
* **HUD improvements**: status effects now display HASTE/VISION and a turn counter.

---

## Controls

| Action | Keys |
|---|---|
| Move | WASD / Arrow Keys |
| Wait | `.` |
| Pick up | `G` |
| Inventory | `I` |
| Target ranged | `F` |
| Look / examine | `L` / `V` |
| Rest | `Z` |
| Search for traps | `C` |
| Toggle auto-pickup gold | `P` |
| Use stairs | `<` / `>` |
| Help overlay | `?` |
| Save / Load | `F5` / `F9` |
| Fullscreen | `F11` |
| Restart | `R` |
| Cancel / Exit overlays | `Esc` |

---

## Building

This project uses CMake. SDL2 can be provided three ways:

1) **Windows**: CMake can **FetchContent** SDL2 automatically (recommended).
2) **Windows**: use **vcpkg** (`vcpkg.json` is included).
3) **Linux/macOS**: install SDL2 from your package manager.

See **[docs/BUILDING.md](docs/BUILDING.md)** for detailed, copy/pasteable steps.

---

## Save files

The game writes a save file to the working directory (same folder you run the executable from).

---

## License

See [LICENSE](LICENSE).
