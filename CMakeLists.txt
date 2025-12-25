cmake_minimum_required(VERSION 3.16)

project(ProcRogue LANGUAGES CXX)

option(PROCROGUE_FETCH_SDL2 "Download/build SDL2 via FetchContent if not found" OFF)
option(PROCROGUE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(PROCROGUE_SOURCES
    src/main.cpp
    src/game.cpp
    src/dungeon.cpp
    src/items.cpp
    src/ai.cpp
    src/render.cpp
    src/spritegen.cpp
    src/settings.cpp
)

add_executable(ProcRogue ${PROCROGUE_SOURCES})
target_compile_features(ProcRogue PRIVATE cxx_std_17)

# ------------------------------------------------------------
# SDL2 dependency
# ------------------------------------------------------------
if (PROCROGUE_FETCH_SDL2)
    include(FetchContent)
    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.28.5
    )
    FetchContent_MakeAvailable(SDL2)
endif()

# Prefer config packages (vcpkg / brew / MSYS2) when available.
find_package(SDL2 CONFIG QUIET)

if (TARGET SDL2::SDL2)
    target_link_libraries(ProcRogue PRIVATE SDL2::SDL2)
    if (TARGET SDL2::SDL2main)
        target_link_libraries(ProcRogue PRIVATE SDL2::SDL2main)
    endif()
elseif (TARGET SDL2::SDL2-static)
    target_link_libraries(ProcRogue PRIVATE SDL2::SDL2-static)
    if (TARGET SDL2::SDL2main)
        target_link_libraries(ProcRogue PRIVATE SDL2::SDL2main)
    endif()
elseif (TARGET SDL2)
    target_link_libraries(ProcRogue PRIVATE SDL2)
else()
    # Fall back to classic Find/variables if needed.
    find_package(SDL2 REQUIRED)
    target_include_directories(ProcRogue PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(ProcRogue PRIVATE ${SDL2_LIBRARIES})
endif()

# ------------------------------------------------------------
# Warnings
# ------------------------------------------------------------
if (MSVC)
    target_compile_options(ProcRogue PRIVATE /W4)
    if (PROCROGUE_WARNINGS_AS_ERRORS)
        target_compile_options(ProcRogue PRIVATE /WX)
    endif()
else()
    target_compile_options(ProcRogue PRIVATE -Wall -Wextra -Wpedantic)
    if (PROCROGUE_WARNINGS_AS_ERRORS)
        target_compile_options(ProcRogue PRIVATE -Werror)
    endif()
endif()
