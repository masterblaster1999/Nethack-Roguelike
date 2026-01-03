#pragma once

// Centralized SDL include.
// We define SDL_MAIN_HANDLED to prevent SDL from redefining main() as SDL_main.
// This avoids needing to link against SDLmain and keeps the entrypoint explicit.
//
// Note: When SDL_MAIN_HANDLED is used, SDL recommends calling SDL_SetMainReady()
// before SDL_Init() on some platforms.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#include <SDL.h>
