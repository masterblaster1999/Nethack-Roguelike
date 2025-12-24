cmake_minimum_required(VERSION 3.20)

project(ProcRogue
    VERSION 0.2.0
    DESCRIPTION "A tiny NetHack-inspired roguelike with procedurally generated pixel sprites (SDL2)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PROCROGUE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# Fetching SDL2 is most useful on Windows/MSVC. On Linux/macOS, SDL2 is typically installed
# via the system package manager and found via pkg-config. Default accordingly.
set(_PROCROGUE_FETCH_SDL2_DEFAULT OFF)
if (WIN32)
    set(_PROCROGUE_FETCH_SDL2_DEFAULT ON)
endif()
option(PROCROGUE_FETCH_SDL2 "Fetch SDL2 automatically when it is not found" ${_PROCROGUE_FETCH_SDL2_DEFAULT})
unset(_PROCROGUE_FETCH_SDL2_DEFAULT)

add_executable(proc_rogue
    src/main.cpp
    src/game.cpp
    src/dungeon.cpp
    src/render.cpp
    src/spritegen.cpp
    src/items.cpp
)

target_include_directories(proc_rogue PRIVATE src)
target_compile_features(proc_rogue PRIVATE cxx_std_17)



if (WIN32)
    target_compile_definitions(proc_rogue PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if (MSVC)
    target_compile_options(proc_rogue PRIVATE /W4)
    if (PROCROGUE_WARNINGS_AS_ERRORS)
        target_compile_options(proc_rogue PRIVATE /WX)
    endif()
else()
    target_compile_options(proc_rogue PRIVATE -Wall -Wextra -Wpedantic)
    if (PROCROGUE_WARNINGS_AS_ERRORS)
        target_compile_options(proc_rogue PRIVATE -Werror)
    endif()
endif()

# ---- SDL2 ----
#
# Your build error on Windows typically happens because SDL2 isn't found and the old
# fallback path tried to use pkg-config (which is uncommon on MSVC/Visual Studio). This
# CMake file:
#   1) tries SDL2's official CMake config package first,
#   2) optionally fetches SDL2 as a subproject if missing,
#   3) only falls back to pkg-config on non-Windows platforms.

find_package(SDL2 CONFIG QUIET)

if (NOT SDL2_FOUND AND PROCROGUE_FETCH_SDL2)
    message(STATUS "SDL2 not found via find_package(); fetching SDL2 (FetchContent)...")

    include(FetchContent)

    # Keep the fetched SDL build lean.
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

    # Download SDL2 source via a release archive URL, so a stock Visual Studio install
    # can configure without needing Git installed.
    FetchContent_Declare(
        SDL2
        URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.30.0.tar.gz
    )
    FetchContent_MakeAvailable(SDL2)
endif()

# Figure out which targets exist (package vs FetchContent build can differ).
set(_SDL2_LIB "")
set(_SDL2_MAIN "")

if (TARGET SDL2::SDL2)
    set(_SDL2_LIB SDL2::SDL2)
elseif (TARGET SDL2::SDL2-static)
    set(_SDL2_LIB SDL2::SDL2-static)
elseif (TARGET SDL2)
    set(_SDL2_LIB SDL2)
elseif (TARGET SDL2-static)
    set(_SDL2_LIB SDL2-static)
endif()

if (TARGET SDL2::SDL2main)
    set(_SDL2_MAIN SDL2::SDL2main)
elseif (TARGET SDL2main)
    set(_SDL2_MAIN SDL2main)
endif()

if (NOT _SDL2_LIB)
    if (WIN32)
        message(FATAL_ERROR
            "SDL2 was not found.\n"
            "\n"
            "Fix options:\n"
            "  1) Enable PROCROGUE_FETCH_SDL2 (default ON on Windows) and ensure you have internet access, OR\n"
            "  2) Install SDL2 via vcpkg and configure CMake with the vcpkg toolchain:\n"
            "     - vcpkg install sdl2\n"
            "     - cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
            "  3) Install SDL2 development packages and set CMAKE_PREFIX_PATH to SDL2's cmake config folder.\n"
        )
    else()
        message(STATUS "SDL2 not found via CMake config; trying pkg-config (non-Windows).")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)

        add_library(SDL2_fallback INTERFACE)
        target_include_directories(SDL2_fallback INTERFACE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(SDL2_fallback INTERFACE ${SDL2_LIBRARIES})
        target_compile_options(SDL2_fallback INTERFACE ${SDL2_CFLAGS_OTHER})
        set(_SDL2_LIB SDL2_fallback)
    endif()
endif()

target_link_libraries(proc_rogue PRIVATE ${_SDL2_LIB})
if (_SDL2_MAIN)
    target_link_libraries(proc_rogue PRIVATE ${_SDL2_MAIN})
endif()

unset(_SDL2_LIB)
unset(_SDL2_MAIN)
