# Building ProcRogue

ProcRogue uses CMake (3.20+) and SDL2.

On Windows, SDL2 can be fetched automatically during configure (FetchContent).
On Linux and macOS, SDL2 is usually provided by the system package manager.
Tests and the headless verifier can be built without SDL2.

## SDL Dependency Flows

Use one of these SDL paths depending on your environment:

- FetchContent SDL (Windows default): `procrogue.bat play` or `cmake --preset msvc`
- vcpkg SDL (Windows): `procrogue.bat play vcpkg` or `cmake --preset msvc-vcpkg`
- System SDL2 package (Linux/macOS, or custom Windows install): `cmake -S . -B build` with SDL2 available in package search paths
- SDL-free (tests/headless): `cmake --preset tests` or manual `PROCROGUE_BUILD_GAME=OFF`

If you are offline, prefer vcpkg/system SDL over FetchContent.

## Quick Start

Windows (from cmd.exe):

```bat
procrogue.bat play
```

Linux/macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ProcRogue
```

## Prerequisites

- CMake 3.20 or newer
- C++17-capable compiler
- SDL2 development package, or internet access for automatic SDL2 fetch

Windows:

- Visual Studio 2022 with C++ workload
- Optional: Ninja

Linux (Debian/Ubuntu):

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev
```

macOS (Homebrew):

```bash
brew install cmake sdl2
```

## CMake Presets

Configure presets:

- `ninja-debug`
- `ninja-release`
- `msvc` (Visual Studio x64, `/MD` runtime)
- `msvc-static` (Visual Studio x64, `/MT` runtime)
- `msvc-vcpkg` (Visual Studio x64, `/MD` runtime, vcpkg manifest)
- `msvc-static-vcpkg` (Visual Studio x64, `/MT` runtime, vcpkg manifest)
- `ninja-release-tests`
- `tests` (SDL-free tests preset)

Build presets:

- `build-ninja-debug`
- `build-ninja-release`
- `build-msvc-release`
- `build-msvc-static-release`
- `build-msvc-vcpkg-release`
- `build-msvc-static-vcpkg-release`
- `tests`

Test preset:

- `tests`

List them locally:

```bash
cmake --list-presets
cmake --list-presets=build
```

## Windows Builds

### Preferred: procrogue.bat

`procrogue.bat` handles toolchain bootstrapping (`vcvars64.bat`) and wraps common flows.

```bat
procrogue.bat play
procrogue.bat play debug
procrogue.bat play static
procrogue.bat play vcpkg
procrogue.bat play vcpkg-static
procrogue.bat test
procrogue.bat doctor
```

Command summary:

- `play [release|debug|static|vcpkg|vcpkg-static]`: configure + build + run
- `configure [preset]`: run `cmake --preset`
- `build [build-preset]`: run `cmake --build --preset`
- `test [test-preset]`: configure/build/test
- `rebuild [release|debug|static|vcpkg|vcpkg-static]`: clean + configure + build
- `clean`: remove `build/`
- `doctor`: print environment/toolchain diagnostics

PowerShell sessions can bootstrap the same toolchain for Ninja presets with:

```powershell
.\procrogue-env.ps1
```

If a previous configure left a stale compiler cache, repair it first:

```powershell
.\procrogue-env.ps1 -RepairPreset tests
```

### Visual Studio presets

Dynamic CRT (`/MD`, `/MDd`):

```powershell
cmake --preset msvc
cmake --build --preset build-msvc-release
```

Static CRT (`/MT`, `/MTd`):

```powershell
cmake --preset msvc-static
cmake --build --preset build-msvc-static-release
```

Toolchain files:

- `cmake/toolchains/msvc-md.cmake`
- `cmake/toolchains/msvc-mt.cmake`

These keep CRT ABI settings consistent across the project and fetched dependencies.

SDL behavior for these presets:

- `msvc` / `msvc-static`: use SDL2 package discovery first, then FetchContent when needed
- `msvc-vcpkg` / `msvc-static-vcpkg`: use vcpkg manifest dependency resolution for SDL2

### vcpkg on Windows

This repo includes `vcpkg.json` with `sdl2`.

```powershell
git clone https://github.com/microsoft/vcpkg
cd vcpkg
bootstrap-vcpkg.bat

# from the ProcRogue repo root
vcpkg install
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

ABI note:

- For `/MD` (`msvc`), use a dynamic-CRT triplet such as `x64-windows`.
- For `/MT` (`msvc-static`), use a static-CRT triplet such as `x64-windows-static`.

Preset note:

- `msvc-vcpkg` uses `VCPKG_CHAINLOAD_TOOLCHAIN_FILE=cmake/toolchains/msvc-md.cmake`.
- `msvc-static-vcpkg` uses `VCPKG_CHAINLOAD_TOOLCHAIN_FILE=cmake/toolchains/msvc-mt.cmake`.
- Set `VCPKG_ROOT` before using these presets.

## Linux and macOS Builds

Release build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ProcRogue
```

Debug build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/ProcRogue
```

If SDL2 is not available via system packages, you can force source fetch:

```bash
cmake -S . -B build -DPROCROGUE_FETCH_SDL2=ON -DCMAKE_BUILD_TYPE=Release
```

If SDL2 is installed in a non-standard path, provide a hint:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/sdl2
```

## Tests and Headless Builds

### Tests only (no SDL required)

Preset flow:

```bash
cmake --preset tests
cmake --build --preset tests
ctest --preset tests --output-on-failure
```

PowerShell shortcut (ensures `cl` is on `PATH` for Ninja):

```powershell
.\procrogue-env.ps1 -RepairPreset tests
cmake --preset tests
cmake --build --preset tests
ctest --preset tests --output-on-failure
```

One-shot PowerShell runner:

```powershell
.\procrogue-test.ps1
```

If an interrupted run left locked `CMakeFiles`, stop stale `cmake`/`ninja` first:

```powershell
.\procrogue-test.ps1 -StopStaleBuildTools
```

Manual flow:

```bash
cmake -S . -B build_tests -DPROCROGUE_BUILD_GAME=OFF -DPROCROGUE_BUILD_TESTS=ON
cmake --build build_tests
ctest --test-dir build_tests --output-on-failure
```

### Headless replay verifier (no SDL required)

```bash
cmake -S . -B build_headless -DPROCROGUE_BUILD_GAME=OFF -DPROCROGUE_BUILD_HEADLESS=ON
cmake --build build_headless --target ProcRogueHeadless
./build_headless/ProcRogueHeadless --replay your_run.prr
```

Useful flags:

- `--frame-ms <n>`
- `--no-verify-hashes`
- `--max-ms <n>`
- `--max-frames <n>`

## Troubleshooting

### SDL2 not found

Typical error:

```
Could not find a package configuration file provided by "SDL2"
```

Resolution options:

- Enable fetch: `-DPROCROGUE_FETCH_SDL2=ON`
- Use vcpkg and pass `-DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake`
- Install SDL2 dev files and set `SDL2_DIR` or `CMAKE_PREFIX_PATH`

Windows shortcut flows:

- FetchContent path: `procrogue.bat play`
- vcpkg path: `procrogue.bat play vcpkg`

### Download failures while fetching SDL2

If FetchContent download fails, check outbound network and proxy variables (`https_proxy`, `http_proxy`, `no_proxy`), then reconfigure.

### Stale CMake cache or compiler-not-found

If presets fail after switching compilers/toolchains, clear build state and configure again.

Windows:

```bat
procrogue.bat clean
```

Linux/macOS:

```bash
rm -rf build build_tests build_headless
```

### Warnings as errors

```bash
cmake -S . -B build -DPROCROGUE_WARNINGS_AS_ERRORS=ON
```
