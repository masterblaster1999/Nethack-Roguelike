#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdint>
#include <iostream>

#include "game.hpp"
#include "render.hpp"

static Action keyToAction(SDL_Keycode key, Uint16 mod) {
    switch (key) {
        case SDLK_UP:    return Action::Up;
        case SDLK_DOWN:  return Action::Down;
        case SDLK_LEFT:  return Action::Left;
        case SDLK_RIGHT: return Action::Right;

        case SDLK_w: return Action::Up;
        case SDLK_s: return Action::Down;
        case SDLK_a: return Action::Left;
        case SDLK_d: return Action::Right;

        case SDLK_SPACE:
            return Action::Wait;

        case SDLK_c:
            return Action::Search;

        case SDLK_p:
            return Action::ToggleAutoPickup;

        case SDLK_PERIOD:
            if ((mod & KMOD_SHIFT) != 0) {
                // '>' on many keyboards
                return Action::StairsDown;
            }
            return Action::Wait;

        case SDLK_COMMA:
            if ((mod & KMOD_SHIFT) != 0) {
                // '<' on many keyboards
                return Action::StairsUp;
            }
            return Action::None;

        case SDLK_GREATER:
            return Action::StairsDown;
        case SDLK_LESS:
            return Action::StairsUp;

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return Action::Confirm;

        case SDLK_g:
            return Action::Pickup;

        case SDLK_i:
            return Action::Inventory;

        case SDLK_f:
            return Action::Fire;

        case SDLK_QUESTION:
        case SDLK_h:
            return Action::Help;

        case SDLK_F5:
            return Action::Save;
        case SDLK_F9:
            return Action::Load;

        // Inventory actions (only do something while inventory is open)
        case SDLK_e:
            return Action::Equip;
        case SDLK_u:
            return Action::Use;
        case SDLK_x:
            return Action::Drop;

        case SDLK_PAGEUP:
            return Action::LogUp;
        case SDLK_PAGEDOWN:
            return Action::LogDown;

        case SDLK_r:
            return Action::Restart;

        default:
            return Action::None;
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    Game game;
    uint32_t seed = static_cast<uint32_t>(SDL_GetTicks());
    game.newGame(seed);

    const int tileSize = 32;
    const int hudHeight = 160;
    const int winW = Game::MAP_W * tileSize;
    const int winH = Game::MAP_H * tileSize + hudHeight;

    Renderer renderer(winW, winH, tileSize, hudHeight);
    if (!renderer.init()) {
        SDL_Quit();
        return 1;
    }

    bool running = true;
    uint32_t lastTicks = SDL_GetTicks();

    while (running) {
        uint32_t now = SDL_GetTicks();
        float dt = (now - lastTicks) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        lastTicks = now;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
                break;
            }
            if (ev.type == SDL_KEYDOWN && ev.key.repeat == 0) {
                SDL_Keycode key = ev.key.keysym.sym;
                Uint16 mod = ev.key.keysym.mod;

                if (key == SDLK_ESCAPE) {
                    // ESC cancels UI modes; otherwise quit.
                    if (game.isInventoryOpen() || game.isTargeting() || game.isHelpOpen()) {
                        game.handleAction(Action::Cancel);
                    } else {
                        running = false;
                    }
                    continue;
                }

                Action a = keyToAction(key, mod);
                if (a != Action::None) {
                    game.handleAction(a);
                }
            }
        }

        game.update(dt);
        renderer.render(game);

        SDL_Delay(8);
    }

    renderer.shutdown();
    SDL_Quit();
    return 0;
}
