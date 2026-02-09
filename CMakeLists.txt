cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(ProcRogue
    VERSION 0.22.0
    DESCRIPTION "A tiny NetHack-inspired roguelike with procedurally generated pixel sprites (SDL2)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PROCROGUE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(_PROCROGUE_WINDOWS_UNLOCK_LINK_OUTPUTS_DEFAULT OFF)
if (WIN32)
    set(_PROCROGUE_WINDOWS_UNLOCK_LINK_OUTPUTS_DEFAULT ON)
endif()
option(
    PROCROGUE_WINDOWS_UNLOCK_LINK_OUTPUTS
    "Stop stale running ProcRogue binaries before link on Windows to avoid LNK1168 file-lock failures"
    ${_PROCROGUE_WINDOWS_UNLOCK_LINK_OUTPUTS_DEFAULT}
)
unset(_PROCROGUE_WINDOWS_UNLOCK_LINK_OUTPUTS_DEFAULT)

set(PROCROGUE_WINDOWS_UNLOCK_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/scripts/unlock_binary.cmake")

# When PROCROGUE_BUILD_TESTS=ON we want the project to configure/build cleanly
# without requiring SDL2. Developers can still set PROCROGUE_BUILD_GAME=ON to
# build the full game alongside the tests.
option(PROCROGUE_BUILD_TESTS "Build unit tests (no SDL required)" OFF)

set(_PROCROGUE_GAME_DEFAULT ON)
if (PROCROGUE_BUILD_TESTS)
    set(_PROCROGUE_GAME_DEFAULT OFF)
endif()
option(PROCROGUE_BUILD_GAME "Build the SDL2 game executable" ${_PROCROGUE_GAME_DEFAULT})
unset(_PROCROGUE_GAME_DEFAULT)

# Headless tools are most useful for CI/regression (and are also SDL-free).
set(_PROCROGUE_HEADLESS_DEFAULT OFF)
if (PROCROGUE_BUILD_TESTS)
    set(_PROCROGUE_HEADLESS_DEFAULT ON)
endif()
option(PROCROGUE_BUILD_HEADLESS "Build the headless replay verifier tool (no SDL required)" ${_PROCROGUE_HEADLESS_DEFAULT})
unset(_PROCROGUE_HEADLESS_DEFAULT)

# Fetching SDL2 is most useful on Windows/MSVC. On Linux/macOS, SDL2 is typically installed
# via the system package manager and found via pkg-config. Default accordingly.
set(_PROCROGUE_FETCH_SDL2_DEFAULT OFF)
if (WIN32)
    set(_PROCROGUE_FETCH_SDL2_DEFAULT ON)
endif()
option(PROCROGUE_FETCH_SDL2 "Fetch SDL2 automatically when it is not found" ${_PROCROGUE_FETCH_SDL2_DEFAULT})
unset(_PROCROGUE_FETCH_SDL2_DEFAULT)

function(procrogue_infer_triplet_crt_linkage out_var triplet)
    set(_PROCROGUE_LINKAGE "unknown")
    if (triplet)
        string(TOLOWER "${triplet}" _PROCROGUE_TRIPLET_LOWER)
        if (_PROCROGUE_TRIPLET_LOWER MATCHES "-static-md$" OR _PROCROGUE_TRIPLET_LOWER MATCHES "-windows$")
            set(_PROCROGUE_LINKAGE "dynamic")
        elseif (_PROCROGUE_TRIPLET_LOWER MATCHES "-static$")
            set(_PROCROGUE_LINKAGE "static")
        endif()
        unset(_PROCROGUE_TRIPLET_LOWER)
    endif()
    set(${out_var} "${_PROCROGUE_LINKAGE}" PARENT_SCOPE)
endfunction()

if (MSVC)
    if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    set(_PROCROGUE_MSVC_CRT_LINKAGE "unknown")
    if (CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
        set(_PROCROGUE_MSVC_CRT_LINKAGE "dynamic")
    elseif (CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded")
        set(_PROCROGUE_MSVC_CRT_LINKAGE "static")
    endif()

    set(_PROCROGUE_VCPKG_CRT_LINKAGE "unknown")
    if (DEFINED VCPKG_CRT_LINKAGE AND NOT VCPKG_CRT_LINKAGE STREQUAL "")
        string(TOLOWER "${VCPKG_CRT_LINKAGE}" _PROCROGUE_VCPKG_CRT_LINKAGE)
        if (NOT _PROCROGUE_VCPKG_CRT_LINKAGE STREQUAL "dynamic" AND
            NOT _PROCROGUE_VCPKG_CRT_LINKAGE STREQUAL "static")
            set(_PROCROGUE_VCPKG_CRT_LINKAGE "unknown")
        endif()
    elseif (DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
        procrogue_infer_triplet_crt_linkage(_PROCROGUE_VCPKG_CRT_LINKAGE "${VCPKG_TARGET_TRIPLET}")
    endif()

    if (NOT _PROCROGUE_MSVC_CRT_LINKAGE STREQUAL "unknown" AND
        NOT _PROCROGUE_VCPKG_CRT_LINKAGE STREQUAL "unknown" AND
        NOT _PROCROGUE_MSVC_CRT_LINKAGE STREQUAL _PROCROGUE_VCPKG_CRT_LINKAGE)
        message(FATAL_ERROR
            "MSVC runtime ABI mismatch:\n"
            "  CMAKE_MSVC_RUNTIME_LIBRARY='${CMAKE_MSVC_RUNTIME_LIBRARY}' (${_PROCROGUE_MSVC_CRT_LINKAGE} CRT)\n"
            "  vcpkg expects '${_PROCROGUE_VCPKG_CRT_LINKAGE}' CRT\n"
            "\n"
            "Select matching settings:\n"
            "  - /MD preset/toolchain with dynamic CRT triplet (e.g. x64-windows)\n"
            "  - /MT preset/toolchain with static CRT triplet (e.g. x64-windows-static)\n"
        )
    endif()

    message(STATUS "MSVC runtime ABI: ${CMAKE_MSVC_RUNTIME_LIBRARY}")
    if (DEFINED VCPKG_CRT_LINKAGE AND NOT VCPKG_CRT_LINKAGE STREQUAL "")
        message(STATUS "vcpkg CRT linkage: ${VCPKG_CRT_LINKAGE}")
    endif()
    if (DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
        message(STATUS "vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")
    endif()

    unset(_PROCROGUE_MSVC_CRT_LINKAGE)
    unset(_PROCROGUE_VCPKG_CRT_LINKAGE)
endif()

# ------------------------------------------------------------
# Helpers
# ------------------------------------------------------------
function(procrogue_apply_warnings tgt)
    if (MSVC)
        # /FS + /Zf improve PDB access behavior; /MP speeds large TU builds.
        target_compile_options(${tgt} PRIVATE /W4 /FS /Zf /MP)
        if (PROCROGUE_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
        if (PROCROGUE_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(procrogue_enable_windows_link_unlock tgt output_name)
    if (NOT PROCROGUE_WINDOWS_UNLOCK_LINK_OUTPUTS OR NOT WIN32 OR NOT MSVC)
        return()
    endif()

    if (NOT EXISTS "${PROCROGUE_WINDOWS_UNLOCK_SCRIPT}")
        message(FATAL_ERROR
            "Expected unlock helper script at '${PROCROGUE_WINDOWS_UNLOCK_SCRIPT}', but it was not found."
        )
    endif()

    add_custom_command(
        TARGET ${tgt}
        PRE_LINK
        COMMAND
            ${CMAKE_COMMAND}
            "-DPROCROGUE_UNLOCK_IMAGE=${output_name}.exe"
            -P "${PROCROGUE_WINDOWS_UNLOCK_SCRIPT}"
        VERBATIM
    )
endfunction()

# ------------------------------------------------------------
# Core (SDL-free) library
# ------------------------------------------------------------
set(PROCROGUE_CORE_SOURCES
    src/replay.cpp
    src/replay_runner.cpp
    src/content.cpp
    src/game.cpp
    src/game_save.cpp
    src/game_loop.cpp
    src/game_automove.cpp
    src/game_look.cpp
    src/game_interact.cpp
    src/game_farming.cpp
    src/game_world.cpp
    src/game_inventory.cpp
    src/game_spellcasting.cpp
    src/game_targeting.cpp
    src/game_spawn.cpp
    src/game_markers.cpp
    src/pathfinding.cpp
    src/combat.cpp
    src/physics.cpp
    src/combat_rules.cpp
    src/dungeon.cpp
    src/vault_prefab_catalog.cpp
    src/corridor_braid.cpp
    src/terrain_sculpt.cpp
    src/items.cpp
    src/spells.cpp
    src/shop.cpp
    src/scores.cpp
)

# Optional sources (present in some forks/patches)
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/settings.cpp")
    list(APPEND PROCROGUE_CORE_SOURCES src/settings.cpp)
endif()
if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/ai.cpp")
    list(APPEND PROCROGUE_CORE_SOURCES src/ai.cpp)
endif()

add_library(procrogue_core STATIC ${PROCROGUE_CORE_SOURCES})
target_include_directories(procrogue_core PUBLIC src)
target_compile_features(procrogue_core PUBLIC cxx_std_17)

target_compile_definitions(procrogue_core PUBLIC
    PROCROGUE_VERSION="${PROJECT_VERSION}"
    PROCROGUE_APPNAME="ProcRogue"
)

if (WIN32)
    target_compile_definitions(procrogue_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

procrogue_apply_warnings(procrogue_core)

# ------------------------------------------------------------
# SDL-free sprite generation / mesh utilities
# ------------------------------------------------------------
#
# These sources are used both by the game (renderer) and the test suite.
# Keeping them in a separate library allows CI/test builds to exercise the
# sprite resampling logic without pulling in SDL.

set(PROCROGUE_SPRITEGEN_SOURCES
    src/spritegen.cpp
    src/spritegen3d.cpp
    src/mesh2d.cpp
)

add_library(procrogue_spritegen STATIC ${PROCROGUE_SPRITEGEN_SOURCES})
target_compile_features(procrogue_spritegen PUBLIC cxx_std_17)
target_link_libraries(procrogue_spritegen PUBLIC procrogue_core)
procrogue_apply_warnings(procrogue_spritegen)

# ------------------------------------------------------------
# Headless tool (SDL-free)
# ------------------------------------------------------------
if (PROCROGUE_BUILD_HEADLESS)
    add_executable(procrogue_headless
        src/headless_main.cpp
    )
    set_target_properties(procrogue_headless PROPERTIES OUTPUT_NAME ProcRogueHeadless)

    target_link_libraries(procrogue_headless PRIVATE procrogue_core)
    procrogue_apply_warnings(procrogue_headless)
    procrogue_enable_windows_link_unlock(procrogue_headless ProcRogueHeadless)
endif()

# ------------------------------------------------------------
# SDL2 game executable
# ------------------------------------------------------------
if (PROCROGUE_BUILD_GAME)

set(PROCROGUE_TARGET proc_rogue)

add_executable(${PROCROGUE_TARGET}
    src/main.cpp
    src/keybinds.cpp
    src/render.cpp
)
set_target_properties(${PROCROGUE_TARGET} PROPERTIES OUTPUT_NAME ProcRogue)

target_include_directories(${PROCROGUE_TARGET} PRIVATE src)
target_compile_features(${PROCROGUE_TARGET} PRIVATE cxx_std_17)

target_link_libraries(${PROCROGUE_TARGET} PRIVATE procrogue_spritegen)

# SDL redefines main() on some platforms unless SDL_MAIN_HANDLED is set.
target_compile_definitions(${PROCROGUE_TARGET} PRIVATE SDL_MAIN_HANDLED)

# Ensure a console subsystem when building with MSVC (helps for logs/stdout).
if (MSVC)
    target_link_options(${PROCROGUE_TARGET} PRIVATE "/SUBSYSTEM:CONSOLE")
endif()


procrogue_apply_warnings(${PROCROGUE_TARGET})
procrogue_enable_windows_link_unlock(${PROCROGUE_TARGET} ProcRogue)

# ------------------------------------------------------------
# SDL2 dependency (only required for the game executable)
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

        if (MSVC)
            # Keep SDL's CRT linkage aligned with the active project runtime ABI.
            set(_PROCROGUE_SDL_STATIC_CRT OFF)
            if (CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded($|Debug$)")
                set(_PROCROGUE_SDL_STATIC_CRT ON)
            endif()
            set(SDL_FORCE_STATIC_VCRT ${_PROCROGUE_SDL_STATIC_CRT} CACHE BOOL "" FORCE)
            unset(_PROCROGUE_SDL_STATIC_CRT)
        endif()

        # Keep the fetched SDL build lean.
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        set(FETCHCONTENT_QUIET OFF)

        set(_PROCROGUE_SDL_URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-2.32.10.tar.gz")
        set(_PROCROGUE_SDL_ARCHIVE_CANDIDATES
            "${CMAKE_CURRENT_SOURCE_DIR}/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/ninja-release/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/ninja-debug/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/ninja-release-game/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/ninja-debug-game/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/msvc/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/msvc-static/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/msvc-vcpkg/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/msvc-static-vcpkg/_deps/sdl2-subbuild/sdl2-populate-prefix/src/release-2.32.10.tar.gz"
        )
        set(_PROCROGUE_SDL_ARCHIVE_LOCAL "")
        foreach(_PROCROGUE_SDL_ARCHIVE_PATH IN LISTS _PROCROGUE_SDL_ARCHIVE_CANDIDATES)
            if (EXISTS "${_PROCROGUE_SDL_ARCHIVE_PATH}")
                file(SIZE "${_PROCROGUE_SDL_ARCHIVE_PATH}" _PROCROGUE_SDL_ARCHIVE_SIZE)
                if (_PROCROGUE_SDL_ARCHIVE_SIZE GREATER 0)
                    set(_PROCROGUE_SDL_ARCHIVE_LOCAL "${_PROCROGUE_SDL_ARCHIVE_PATH}")
                    break()
                endif()

                message(STATUS "Ignoring empty SDL2 archive cache file: ${_PROCROGUE_SDL_ARCHIVE_PATH}")
                file(REMOVE "${_PROCROGUE_SDL_ARCHIVE_PATH}")
            endif()
        endforeach()

        if (_PROCROGUE_SDL_ARCHIVE_LOCAL)
            set(_PROCROGUE_SDL_URL "${_PROCROGUE_SDL_ARCHIVE_LOCAL}")
            message(STATUS "Using cached SDL2 archive: ${_PROCROGUE_SDL_ARCHIVE_LOCAL}")
        else()
            message(STATUS "No local SDL2 archive cache found; downloading from upstream.")
        endif()

        # Download SDL2 source via a release archive URL, so a stock Visual Studio install
        # can configure without needing Git installed.
        FetchContent_Declare(
            SDL2
            URL "${_PROCROGUE_SDL_URL}"
        )
        FetchContent_MakeAvailable(SDL2)
        unset(_PROCROGUE_SDL_URL)
        unset(_PROCROGUE_SDL_ARCHIVE_CANDIDATES)
        unset(_PROCROGUE_SDL_ARCHIVE_LOCAL)
        unset(_PROCROGUE_SDL_ARCHIVE_PATH)
        unset(_PROCROGUE_SDL_ARCHIVE_SIZE)

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
        "     - or set VCPKG_ROOT and use preset: cmake --preset msvc-vcpkg\n"
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
