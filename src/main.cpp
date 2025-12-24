#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "game.hpp"
#include "render.hpp"
#include "rng.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

static Action actionFromKey(const SDL_Keysym& k) {
    const SDL_Keycode key = k.sym;
    const Uint16 mod = k.mod;

    switch (key) {
        case SDLK_ESCAPE: return Action::None; // handled separately
        case SDLK_UP:    return Action::MoveUp;
        case SDLK_DOWN:  return Action::MoveDown;
        case SDLK_LEFT:  return Action::MoveLeft;
        case SDLK_RIGHT: return Action::MoveRight;

        case SDLK_w: return Action::MoveUp;
        case SDLK_s: return Action::MoveDown;
        case SDLK_a: return Action::MoveLeft;
        case SDLK_d: return Action::MoveRight;

        case SDLK_SPACE:
        case SDLK_PERIOD:
            // Shift+period is '>' on many keyboards; treat that as stairs.
            if ((mod & KMOD_SHIFT) != 0) return Action::StairsDown;
            return Action::Wait;

        case SDLK_GREATER:
            return Action::StairsDown;

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return Action::StairsDown;

        case SDLK_r:
            return Action::Restart;

        default:
            break;
    }

    return Action::None;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // For pixel art scaling
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    const int tileSize = 32;
    const int hudHeight = 160;
    const int winW = Game::MAP_W * tileSize;
    const int winH = Game::MAP_H * tileSize + hudHeight;

    Renderer renderer(winW, winH, tileSize, hudHeight);
    if (!renderer.init()) {
        SDL_Quit();
        return 1;
    }

    // Seed from time
    uint32_t seed = static_cast<uint32_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    seed = hash32(seed);

    Game game;
    game.newGame(seed);

    bool running = true;
    while (running) {
        SDL_Event e;
        Action action = Action::None;

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else {
                    action = actionFromKey(e.key.keysym);
                }
            }
        }

        if (action != Action::None) {
            game.handleAction(action);
        }

        renderer.render(game);

        // Small delay to avoid maxing a CPU core.
        SDL_Delay(8);
    }

    renderer.shutdown();
    SDL_Quit();
    return 0;
}
