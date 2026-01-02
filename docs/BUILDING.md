# Building ProcRogue

This project uses **CMake** and **SDL2**.

The CMake build is designed to work out-of-the-box on Windows **without** a preinstalled SDL2 by optionally using `FetchContent` (it downloads and builds SDL2 automatically).

If you prefer system packages (Linux/macOS) or vcpkg (Windows), those paths are supported too.

---

## Windows (Visual Studio)

### Option A: Visual Studio + FetchContent (fastest setup)

1. Install **Visual Studio 2022** with “Desktop development with C++”.
2. Make sure you have **internet access** during the first configure (SDL2 will be downloaded automatically).
3. Open the repo folder in Visual Studio (File → Open → Folder...).
4. Configure the CMake project.

If SDL2 is not found, enable the cache option:

- `PROCROGUE_FETCH_SDL2=ON`

Then build/run.

### Option B: Ninja (VS Code / CLion / CMake Tools)

If you use the Ninja presets (`ninja-debug` / `ninja-release`) on Windows, SDL2 will be fetched automatically when it is not found.

If you previously configured in a `build/` folder with `PROCROGUE_FETCH_SDL2=OFF`, delete that build folder (or clear your CMake cache) and reconfigure so the new setting takes effect.

### Option C: vcpkg (recommended for reproducible dependencies)

This repo includes a `vcpkg.json` manifest. If your Visual Studio is configured with vcpkg integration, it can restore dependencies automatically.

Manual vcpkg steps:

```powershell
git clone https://github.com/microsoft/vcpkg
cd vcpkg
bootstrap-vcpkg.bat

# from the game repo root
vcpkg install

cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

---

## Linux (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ProcRogue
```

If you want SDL2 fetched from source instead of system packages:

```bash
cmake -S . -B build -DPROCROGUE_FETCH_SDL2=ON -DCMAKE_BUILD_TYPE=Release
```

---

## macOS (Homebrew)

```bash
brew install cmake sdl2

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ProcRogue
```

---

## Common CMake Tips

- Clean configure:

  ```bash
  rm -rf build
  ```

- Turn warnings into errors:

  ```bash
  cmake -S . -B build -DPROCROGUE_WARNINGS_AS_ERRORS=ON
  ```

---

## About the common SDL2 "find_package" error

A frequent configuration failure looks like:

> Could not find a package configuration file provided by "SDL2" ...

This repo’s CMake is set up to:

- Prefer **SDL2 config packages** (vcpkg/MSYS/etc)
- On Linux/macOS, fall back to **pkg-config**
- On Windows, automatically **FetchContent** SDL2 when enabled/needed


## Unit tests

This project includes a small unit test executable (no SDL required).

If you don't have SDL2 installed, disable the game target while building tests:

```bash
cmake -S . -B build -DPROCROGUE_BUILD_GAME=OFF -DPROCROGUE_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

(If you *do* have SDL2 installed and also want to build the game, you can omit `-DPROCROGUE_BUILD_GAME=OFF`.)



## Tests only (no SDL required)

If you just want to build and run the unit tests (headless, no SDL2 dependency), configure with:

```bash
cmake -S . -B build_tests -DPROCROGUE_BUILD_GAME=OFF -DPROCROGUE_BUILD_TESTS=ON
cmake --build build_tests
ctest --test-dir build_tests --output-on-failure
```

This is also what the CI "tests-only" job uses.



## Headless replay verification (no SDL required)

ProcRogue can verify recorded replays without SDL2 or a renderer. This is useful for CI and for chasing determinism issues.

```bash
cmake -S . -B build -DPROCROGUE_BUILD_GAME=OFF -DPROCROGUE_BUILD_HEADLESS=ON
cmake --build build --target ProcRogueHeadless
./ProcRogueHeadless --replay your_run.prr
```

Useful flags:

- `--frame-ms <n>`: simulation step (1..100). Default is 16ms.
- `--no-verify-hashes`: run the input stream without validating StateHash checkpoints.
- `--max-ms <n>` / `--max-frames <n>`: safety limits for runaway auto-move.
