# Building ProcRogue

This project uses **CMake** and **SDL2**.

The CMake build has been upgraded to work out-of-the-box on Windows **without** a preinstalled SDL2 by optionally using `FetchContent` (it downloads and builds SDL2 automatically).

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

### Option B: vcpkg (recommended for reproducible dependencies)

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

## About the Visual Studio CMake “FindPackageHandleStandardArgs” error

That error commonly appears when `find_package(SDL2 ...)` fails to locate SDL2.

This repo’s updated CMake now:

- Uses **SDL2 config packages** when available (vcpkg/MSYS/etc)
- On Linux/macOS, falls back to **pkg-config**
- On Windows, can automatically **FetchContent** SDL2 if enabled

So you should no longer be blocked by a missing SDL2 installation.
