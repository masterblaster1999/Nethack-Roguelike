#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "game.hpp"
#include "render.hpp"
#include "settings.hpp"
#include "version.hpp"

static Action keyToAction(const Game& game, SDL_Keycode key, Uint16 mod) {
    // Message log scrolling
    switch (key) {
        case SDLK_PAGEUP:   return Action::LogUp;
        case SDLK_PAGEDOWN: return Action::LogDown;
        default: break;
    }

    // F1 help
    if (key == SDLK_F1) return Action::Help;

    // Inventory-specific commands (avoid conflicts with movement keys like 'u').
    if (game.isInventoryOpen()) {
        switch (key) {
            case SDLK_e: return Action::Equip;
            case SDLK_u: return Action::Use;
            case SDLK_x: return Action::Drop;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                return Action::Confirm; // context action on selected item
            default:
                break;
        }
    }

    switch (key) {
        // 8-way movement (WASD / arrows / numpad / YUBN)
        case SDLK_w:
        case SDLK_UP:
        case SDLK_KP_8:
            return Action::Up;

        case SDLK_s:
        case SDLK_DOWN:
        case SDLK_KP_2:
            return Action::Down;

        case SDLK_a:
        case SDLK_LEFT:
        case SDLK_KP_4:
            return Action::Left;

        case SDLK_d:
        case SDLK_RIGHT:
        case SDLK_KP_6:
            return Action::Right;

        case SDLK_y:
        case SDLK_KP_7:
            return Action::UpLeft;

        case SDLK_u:
        case SDLK_KP_9:
            return Action::UpRight;

        case SDLK_b:
        case SDLK_KP_1:
            return Action::DownLeft;

        case SDLK_n:
        case SDLK_KP_3:
            return Action::DownRight;

        case SDLK_PERIOD:
        case SDLK_SPACE:
        case SDLK_KP_5:
            return Action::Wait;

        case SDLK_z:
            return Action::Rest;

        case SDLK_g:
            return Action::Pickup;

        case SDLK_i:
            return Action::Inventory;

        case SDLK_f:
            return Action::Fire;

        case SDLK_c:
            return Action::Search;

        case SDLK_l:
        case SDLK_v:
            return Action::Look;

        case SDLK_COMMA:
            // '<' on most keyboards (Shift + ',')
            if (mod & KMOD_SHIFT) return Action::StairsUp;
            return Action::None;

        case SDLK_GREATER:
            // '>'
            return Action::StairsDown;

        case SDLK_o:
            return Action::AutoExplore;

        case SDLK_p:
            return Action::ToggleAutoPickup;

        case SDLK_m:
            return Action::ToggleMinimap;

        case SDLK_TAB:
            return Action::ToggleStats;

        case SDLK_F5:
            return Action::Save;

        case SDLK_F9:
            return Action::Load;

        case SDLK_F10:
            return Action::LoadAuto;

        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return Action::Confirm;

        case SDLK_BACKSPACE:
            return Action::Cancel;

        case SDLK_SLASH:
            if (mod & KMOD_SHIFT) return Action::Help;
            return Action::None;

        case SDLK_QUESTION:
            return Action::Help;

        case SDLK_h:
            return Action::Help;

        case SDLK_r:
            return Action::Restart;

        case SDLK_ESCAPE:
            return Action::Cancel;

        default:
            return Action::None;
    }
}

static std::optional<uint32_t> parseSeedArg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) {
            try {
                unsigned long v = std::stoul(argv[i + 1]);
                return static_cast<uint32_t>(v);
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

static bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // Prefer a per-user writable directory for config/saves.
    std::string prefPath;
    if (char* p = SDL_GetPrefPath("masterblaster1999", PROCROGUE_APPNAME)) {
        prefPath = p;
        SDL_free(p);
    }

    const std::string basePath = prefPath.empty() ? std::string("./") : prefPath;
    const std::string settingsPath = basePath + "procrogue_settings.ini";
    const std::string savePath = basePath + "procrogue_save.dat";
    const std::string autosavePath = basePath + "procrogue_autosave.dat";
    const std::string scoresPath = basePath + "procrogue_scores.csv";
    const std::string screenshotDir = basePath + "screenshots";

    // Load or create settings.
    if (!std::filesystem::exists(settingsPath)) {
        writeDefaultSettings(settingsPath);
    }
    Settings settings = loadSettings(settingsPath);

    const int tileSize = settings.tileSize;
    const int hudHeight = settings.hudHeight;
    const int winW = Game::MAP_W * tileSize;
    const int winH = Game::MAP_H * tileSize + hudHeight;

    Renderer renderer(winW, winH, tileSize, hudHeight, settings.vsync);
    if (!renderer.init()) {
        SDL_Quit();
        return 1;
    }

    if (settings.startFullscreen) {
        renderer.toggleFullscreen();
    }

    // Optional controller support (SDL_GameController)
    SDL_GameController* controller = nullptr;
    SDL_JoystickID controllerId = -1;

    auto closeController = [&]() {
        if (controller) {
            SDL_GameControllerClose(controller);
            controller = nullptr;
            controllerId = -1;
        }
    };

    auto openFirstController = [&]() {
        if (!settings.controllerEnabled) return;
        if (controller) return;

        const int n = SDL_NumJoysticks();
        for (int i = 0; i < n; ++i) {
            if (!SDL_IsGameController(i)) continue;

            controller = SDL_GameControllerOpen(i);
            if (controller) {
                SDL_Joystick* joy = SDL_GameControllerGetJoystick(controller);
                controllerId = joy ? SDL_JoystickInstanceID(joy) : -1;
                const char* name = SDL_GameControllerName(controller);
                std::cout << "Controller connected: " << (name ? name : "(unknown)") << "\n";
            }
            break;
        }
    };

    openFirstController();

    Game game;
    game.setSavePath(savePath);
    game.setAutosavePath(autosavePath);
    game.setScoresPath(scoresPath);
    game.setAutoStepDelayMs(settings.autoStepDelayMs);
    game.setAutosaveEveryTurns(settings.autosaveEveryTurns);
    game.setAutoPickupMode(settings.autoPickup);
    game.setIdentificationEnabled(settings.identifyItems);

    const bool loadOnStart = hasFlag(argc, argv, "--load") || hasFlag(argc, argv, "--continue");

    bool loaded = false;
    if (loadOnStart && std::filesystem::exists(savePath)) {
        loaded = game.loadFromFile(savePath);
    }

    if (!loaded) {
        const uint32_t seed = parseSeedArg(argc, argv).value_or(static_cast<uint32_t>(SDL_GetTicks()));
        game.newGame(seed);
        game.setAutoPickupMode(settings.autoPickup);
    }

    const bool vsyncEnabled = settings.vsync;
    const int maxFps = settings.maxFps;
    const uint32_t targetFrameMs = (!vsyncEnabled && maxFps > 0) ? (1000u / static_cast<uint32_t>(maxFps)) : 0u;

    bool running = true;
    uint32_t lastTicks = SDL_GetTicks();
    bool wantScreenshot = false;

    while (running) {
        const uint32_t frameStart = SDL_GetTicks();
        const uint32_t now = frameStart;

        float dt = (now - lastTicks) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        lastTicks = now;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_CONTROLLERDEVICEADDED:
                    openFirstController();
                    break;

                case SDL_CONTROLLERDEVICEREMOVED:
                    if (controller && controllerId == static_cast<SDL_JoystickID>(ev.cdevice.which)) {
                        std::cout << "Controller disconnected\n";
                        closeController();
                    }
                    break;

                case SDL_CONTROLLERBUTTONDOWN:
                    if (!settings.controllerEnabled) break;
                    switch (ev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:    game.handleAction(Action::Up); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  game.handleAction(Action::Down); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  game.handleAction(Action::Left); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: game.handleAction(Action::Right); break;

                        case SDL_CONTROLLER_BUTTON_A:          game.handleAction(Action::Confirm); break;
                        case SDL_CONTROLLER_BUTTON_B:          game.handleAction(Action::Cancel); break;
                        case SDL_CONTROLLER_BUTTON_X:          game.handleAction(Action::Inventory); break;
                        case SDL_CONTROLLER_BUTTON_Y:          game.handleAction(Action::Pickup); break;

                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  game.handleAction(Action::Look); break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: game.handleAction(Action::Fire); break;

                        case SDL_CONTROLLER_BUTTON_BACK:  game.handleAction(Action::Help); break;
                        case SDL_CONTROLLER_BUTTON_START: game.handleAction(Action::ToggleStats); break;

                        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: game.handleAction(Action::ToggleMinimap); break;
                        default:
                            break;
                    }
                    break;

                case SDL_KEYDOWN:
                    if (ev.key.repeat != 0) break;
                    {
                        const SDL_Keycode key = ev.key.keysym.sym;
                        const Uint16 mod = ev.key.keysym.mod;

                        if (key == SDLK_F12) {
                            wantScreenshot = true;
                            break;
                        }

                        if (key == SDLK_F11) {
                            renderer.toggleFullscreen();
                            break;
                        }

                        if (key == SDLK_ESCAPE) {
                            // ESC cancels UI modes; cancels auto-move; otherwise quit.
                            if (game.isInventoryOpen() || game.isTargeting() || game.isHelpOpen() || game.isLooking() ||
                                game.isMinimapOpen() || game.isStatsOpen() || game.isAutoActive()) {
                                game.handleAction(Action::Cancel);
                            } else {
                                running = false;
                            }
                            break;
                        }

                        const Action a = keyToAction(game, key, mod);
                        if (a != Action::None) {
                            game.handleAction(a);
                        }
                    }
                    break;

                case SDL_MOUSEWHEEL:
                    if (ev.wheel.y > 0) game.handleAction(Action::LogUp);
                    else if (ev.wheel.y < 0) game.handleAction(Action::LogDown);
                    break;

                case SDL_MOUSEMOTION:
                    {
                        int tx = 0, ty = 0;
                        if (!renderer.windowToMapTile(ev.motion.x, ev.motion.y, tx, ty)) break;
                        const Vec2i p{tx, ty};

                        if (game.isTargeting()) {
                            game.setTargetCursor(p);
                        } else if (game.isLooking()) {
                            game.setLookCursor(p);
                        }
                    }
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    {
                        // Ignore mouse when menus are open.
                        if (game.isInventoryOpen() || game.isHelpOpen() || game.isMinimapOpen() || game.isStatsOpen()) break;

                        int tx = 0, ty = 0;
                        if (!renderer.windowToMapTile(ev.button.x, ev.button.y, tx, ty)) break;
                        const Vec2i p{tx, ty};

                        if (game.isTargeting()) {
                            if (ev.button.button == SDL_BUTTON_LEFT) {
                                game.setTargetCursor(p);
                                game.handleAction(Action::Confirm);
                            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                                game.handleAction(Action::Cancel);
                            }
                            break;
                        }

                        if (game.isLooking()) {
                            if (ev.button.button == SDL_BUTTON_LEFT) {
                                game.setLookCursor(p);
                            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                                game.handleAction(Action::Cancel);
                            }
                            break;
                        }

                        // Normal mode: left-click auto-travels; right-click enters look mode.
                        if (ev.button.button == SDL_BUTTON_LEFT) {
                            game.requestAutoTravel(p);
                        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                            game.beginLookAt(p);
                        }
                    }
                    break;
            }

            if (!running) break;
        }

        game.update(dt);
        renderer.render(game);

        if (wantScreenshot) {
            std::string outPath = renderer.saveScreenshotBMP(screenshotDir);
            if (!outPath.empty()) {
                game.pushSystemMessage("SCREENSHOT SAVED: " + outPath);
            } else {
                game.pushSystemMessage("SCREENSHOT FAILED.");
            }
            wantScreenshot = false;
        }

        // If vsync is enabled, SDL_RenderPresent will throttle naturally.
        // Otherwise, optionally cap frame rate (and always yield a little to keep CPU usage sane).
        if (!vsyncEnabled) {
            if (targetFrameMs > 0) {
                const uint32_t frameTime = SDL_GetTicks() - frameStart;
                if (frameTime < targetFrameMs) {
                    SDL_Delay(targetFrameMs - frameTime);
                } else {
                    SDL_Delay(1);
                }
            } else {
                SDL_Delay(1);
            }
        }
    }

    closeController();
    renderer.shutdown();
    SDL_Quit();
    return 0;
}
