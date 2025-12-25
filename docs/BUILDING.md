# Building ProcRogue

This project uses **CMake** and **SDL2**.

The build is set up to work out-of-the-box on Windows by optionally using **CMake FetchContent**
(it can download and build SDL2 automatically).

If you'd rather use prebuilt dependencies, **vcpkg** also works (a `vcpkg.json` manifest is included).

---

## Windows (Visual Studio)

### Option A: FetchContent (no manual SDL install)

1. Install **Visual Studio 2022** with “Desktop development with C++”.
2. Make sure you have **internet access** during the first configure.
3. Configure/build with CMake.

On Windows, `PROCROGUE_FETCH_SDL2` defaults to **ON**, so SDL2 will be fetched automatically if not already installed.

If you want to force it explicitly:

```powershell
cmake -S . -B build -DPROCROGUE_FETCH_SDL2=ON
cmake --build build --config Release
```

### Option B: vcpkg (reproducible dependencies)

```powershell
# one-time vcpkg setup
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

---

## macOS (Homebrew)

```bash
brew install cmake sdl2

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ProcRogue
```
