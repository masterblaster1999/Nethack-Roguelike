cmake_minimum_required(VERSION 3.20)

project(ProcRogue
    VERSION 0.21.0
    DESCRIPTION "A tiny NetHack-inspired roguelike with procedurally generated pixel sprites (SDL2)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PROCROGUE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(PROCROGUE_BUILD_TESTS "Build unit tests (no SDL required)" OFF)
option(PROCROGUE_BUILD_GAME "Build the SDL2 game executable" ON)

# Fetching SDL2 is most useful on Windows/MSVC. On Linux/macOS, SDL2 is typically installed
# via the system package manager and found via pkg-config. Default accordingly.
set(_PROCROGUE_FETCH_SDL2_DEFAULT OFF)
if (WIN32)
    set(_PROCROGUE_FETCH_SDL2_DEFAULT ON)
endif()
option(PROCROGUE_FETCH_SDL2 "Fetch SDL2 automatically when it is not found" ${_PROCROGUE_FETCH_SDL2_DEFAULT})
unset(_PROCROGUE_FETCH_SDL2_DEFAULT)

if (PROCROGUE_BUILD_GAME)

set(PROCROGUE_TARGET proc_rogue)

set(PROCROGUE_SOURCES
    src/main.cpp
    src/keybinds.cpp
    src/game.cpp
    src/game_save.cpp
    src/game_loop.cpp
    src/game_automove.cpp
    src/game_look.cpp
    src/game_interact.cpp
    src/game_world.cpp
    src/game_inventory.cpp
    src/game_targeting.cpp
    src/game_spawn.cpp
    src/pathfinding.cpp
    src/combat.cpp
    src/physics.cpp
    src/combat_rules.cpp
    src/dungeon.cpp
    src/render.cpp
    src/spritegen.cpp
    src/spritegen3d.cpp
    src/items.cpp
    src/shop.cpp
    src/scores.cpp
)

# Optional sources (present in some forks/patches)
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/settings.cpp")
    list(APPEND PROCROGUE_SOURCES src/settings.cpp)
endif()
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/ai.cpp")
    list(APPEND PROCROGUE_SOURCES src/ai.cpp)
endif()

add_executable(${PROCROGUE_TARGET} ${PROCROGUE_SOURCES})
set_target_properties(${PROCROGUE_TARGET} PROPERTIES OUTPUT_NAME ProcRogue)

target_include_directories(${PROCROGUE_TARGET} PRIVATE src)
target_compile_features(${PROCROGUE_TARGET} PRIVATE cxx_std_17)

target_compile_definitions(${PROCROGUE_TARGET} PRIVATE
    PROCROGUE_VERSION="${PROJECT_VERSION}"
    PROCROGUE_APPNAME="ProcRogue"
)

if (WIN32)
    target_compile_definitions(${PROCROGUE_TARGET} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ------------------------------------------------------------
# Warnings
# ------------------------------------------------------------
if (MSVC)
    target_compile_options(${PROCROGUE_TARGET} PRIVATE /W4)
    if (PROCROGUE_WARNINGS_AS_ERRORS)
        target_compile_options(${PROCROGUE_TARGET} PRIVATE /WX)
    endif()
else()
    target_compile_options(${PROCROGUE_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
    if (PROCROGUE_WARNINGS_AS_ERRORS)
        target_compile_options(${PROCROGUE_TARGET} PRIVATE -Werror)
    endif()
endif()

# ------------------------------------------------------------
# SDL2 dependency
# ------------------------------------------------------------
#
# We try, in order:
#   1) SDL2's official CMake *config* package (common with vcpkg, MSYS2, Homebrew)
#   2) FetchContent (downloads/builds SDL2 automatically; default ON on Windows)
#   3) pkg-config (Linux/macOS)
#
# NOTE: Some config packages only expose targets after requesting COMPONENTS.
#
function(procrogue_detect_sdl2 out_lib out_main)
    set(_lib "")
    set(_main "")

    if (TARGET SDL2::SDL2)
        set(_lib SDL2::SDL2)
    elseif (TARGET SDL2::SDL2-static)
        set(_lib SDL2::SDL2-static)
    elseif (TARGET SDL2)
        set(_lib SDL2)
    elseif (TARGET SDL2-static)
        set(_lib SDL2-static)
    endif()

    if (TARGET SDL2::SDL2main)
        set(_main SDL2::SDL2main)
    elseif (TARGET SDL2main)
        set(_main SDL2main)
    endif()

    set(${out_lib} "${_lib}" PARENT_SCOPE)
    set(${out_main} "${_main}" PARENT_SCOPE)
endfunction()

set(_SDL2_LIB "")
set(_SDL2_MAIN "")

# 1) Config packages (preferred)
find_package(SDL2 CONFIG QUIET COMPONENTS SDL2)
if (SDL2_FOUND)
    # SDL2main is optional
    find_package(SDL2 CONFIG QUIET COMPONENTS SDL2main)
endif()
procrogue_detect_sdl2(_SDL2_LIB _SDL2_MAIN)

# 2) FetchContent when needed
if (NOT _SDL2_LIB)
    # If you are on Windows and a preset/cache forced PROCROGUE_FETCH_SDL2=OFF,
    # auto-enable it so the project "just works" out of the box.
    if (WIN32 AND NOT PROCROGUE_FETCH_SDL2)
        message(STATUS "SDL2 was not found; enabling PROCROGUE_FETCH_SDL2 automatically on Windows.")
        set(PROCROGUE_FETCH_SDL2 ON CACHE BOOL "Fetch SDL2 automatically when it is not found" FORCE)
    endif()

    if (PROCROGUE_FETCH_SDL2)
        message(STATUS "SDL2 not found via find_package(); fetching SDL2 (FetchContent)...")

        include(FetchContent)

        # Keep the fetched SDL build lean.
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

        # Download SDL2 source via a release archive URL, so a stock Visual Studio install
        # can configure without needing Git installed.
        FetchContent_Declare(
            SDL2
            URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.32.10.tar.gz
        )
        FetchContent_MakeAvailable(SDL2)

        procrogue_detect_sdl2(_SDL2_LIB _SDL2_MAIN)
    endif()
endif()

# 3) pkg-config fallback (non-Windows)
if (NOT _SDL2_LIB AND NOT WIN32)
    message(STATUS "SDL2 not found via CMake config; trying pkg-config (non-Windows).")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)

    add_library(SDL2_fallback INTERFACE)
    target_include_directories(SDL2_fallback INTERFACE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(SDL2_fallback INTERFACE ${SDL2_LIBRARIES})
    target_compile_options(SDL2_fallback INTERFACE ${SDL2_CFLAGS_OTHER})

    set(_SDL2_LIB SDL2_fallback)
endif()

if (NOT _SDL2_LIB)
    message(FATAL_ERROR
        "SDL2 was not found.\n"
        "\n"
        "Fix options:\n"
        "  1) Ensure you have internet access and configure with PROCROGUE_FETCH_SDL2=ON, OR\n"
        "  2) Install SDL2 via vcpkg and configure CMake with the vcpkg toolchain:\n"
        "     - vcpkg install sdl2\n"
        "     - cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
        "  3) Install SDL2 (development files) and set SDL2_DIR or CMAKE_PREFIX_PATH to SDL2's CMake config folder.\n"
    )
endif()

# SDL2main (when present) must be linked BEFORE SDL2 itself.
if (_SDL2_MAIN)
    target_link_libraries(${PROCROGUE_TARGET} PRIVATE ${_SDL2_MAIN})
endif()
target_link_libraries(${PROCROGUE_TARGET} PRIVATE ${_SDL2_LIB})

unset(_SDL2_LIB)
unset(_SDL2_MAIN)

endif() # PROCROGUE_BUILD_GAME

# ------------------------------------------------------------
# Tests
# ------------------------------------------------------------
if (PROCROGUE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
