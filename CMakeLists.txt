cmake_minimum_required(VERSION 3.16)

project(ProcRogue
    VERSION 0.1.0
    DESCRIPTION "A tiny NetHack-inspired roguelike with procedurally generated pixel sprites (SDL2)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PROCROGUE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

add_executable(proc_rogue
    src/main.cpp
    src/game.cpp
    src/dungeon.cpp
    src/render.cpp
    src/spritegen.cpp
    src/items.cpp
)

target_include_directories(proc_rogue PRIVATE src)

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
# Prefer SDL2's official CMake package config (SDL2Config.cmake), but fall back to pkg-config on Linux.
# SDL's official guidance for CMake packages:
# https://wiki.libsdl.org/SDL2/README-cmake

find_package(SDL2 CONFIG QUIET)

if (SDL2_FOUND)
    message(STATUS "Found SDL2 via CMake package config.")
    # SDL2::SDL2main may or may not be present. It's often needed for Windows GUI apps,
    # but this project defines SDL_MAIN_HANDLED so it isn't strictly required.
    if (TARGET SDL2::SDL2main)
        target_link_libraries(proc_rogue PRIVATE SDL2::SDL2main)
    endif()
    if (TARGET SDL2::SDL2)
        target_link_libraries(proc_rogue PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(proc_rogue PRIVATE SDL2::SDL2-static)
    else()
        message(FATAL_ERROR "SDL2 was found, but no SDL2::SDL2 or SDL2::SDL2-static target exists.")
    endif()
else()
    message(STATUS "SDL2 not found via CMake package config, trying pkg-config (Linux/BSD).")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)

    target_include_directories(proc_rogue PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(proc_rogue PRIVATE ${SDL2_LIBRARIES})
    target_compile_options(proc_rogue PRIVATE ${SDL2_CFLAGS_OTHER})
endif()

# On macOS, ensure we link required frameworks when using pkg-config (often handled automatically via SDL2Config).
if(APPLE AND NOT SDL2_FOUND)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    if(COCOA_FRAMEWORK)
        target_link_libraries(proc_rogue PRIVATE ${COCOA_FRAMEWORK})
    endif()
    if(IOKIT_FRAMEWORK)
        target_link_libraries(proc_rogue PRIVATE ${IOKIT_FRAMEWORK})
    endif()
    if(COREVIDEO_FRAMEWORK)
        target_link_libraries(proc_rogue PRIVATE ${COREVIDEO_FRAMEWORK})
    endif()
endif()

