#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#include "game.hpp"
#include "keybinds.hpp"
#include "render.hpp"
#include "settings.hpp"
#include "slot_utils.hpp"
#include "version.hpp"

static std::optional<uint32_t> parseSeedArg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) {
            try {
                unsigned long v = std::stoul(argv[i + 1], nullptr, 0);
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


static std::optional<std::string> parseStringArg(int argc, char** argv, const char* opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == opt && i + 1 < argc) {
            return std::string(argv[i + 1]);
        }
    }
    return std::nullopt;
}

static void printUsage(const char* exe) {
    std::cout
        << PROCROGUE_APPNAME << " " << PROCROGUE_VERSION << "\n"
        << "Usage: " << (exe ? exe : "procrogue") << " [options]\n\n"
        << "Options:\n"
        << "  --seed <n>           Start a new run with a specific seed\n"
        << "  --daily              Start a daily run (deterministic UTC-date seed)\n"
        << "  --load               Auto-load the manual save on start (alias: --continue)\n"
        << "  --load-auto          Auto-load the autosave on start\n"
        << "  --data-dir <path>    Override the save/config directory\n"
        << "  --slot <name>        Use a named save slot (affects save + autosave files)\n"
        << "  --portable           Store saves/config next to the executable\n"
        << "  --reset-settings     Overwrite settings with fresh defaults\n"
        << "\n"
        << "  --version, -v        Print version and exit\n"
        << "  --help, -h           Show this help and exit\n";
}

int main(int argc, char** argv) {
    if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
        printUsage(argc > 0 ? argv[0] : "procrogue");
        return 0;
    }
    if (hasFlag(argc, argv, "--version") || hasFlag(argc, argv, "-v")) {
        std::cout << PROCROGUE_APPNAME << " " << PROCROGUE_VERSION << "\n";
        return 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // Determine where to store settings/saves.
    // By default we use a per-user writable directory (SDL_GetPrefPath), but this can be overridden.
    const std::optional<std::string> dataDirArg = parseStringArg(argc, argv, "--data-dir");
    const std::optional<std::string> slotArg = parseStringArg(argc, argv, "--slot");
    const bool portable = hasFlag(argc, argv, "--portable");
    const bool resetSettings = hasFlag(argc, argv, "--reset-settings");

    std::filesystem::path baseDir;

    if (dataDirArg && !dataDirArg->empty()) {
        baseDir = std::filesystem::path(*dataDirArg);
    } else if (portable) {
        // Portable mode: store saves/config next to the executable (if possible).
        if (char* p = SDL_GetBasePath()) {
            baseDir = std::filesystem::path(p);
            SDL_free(p);
        } else {
            baseDir = std::filesystem::current_path();
        }
    } else {
        // Prefer a per-user writable directory for config/saves.
        if (char* p = SDL_GetPrefPath("masterblaster1999", PROCROGUE_APPNAME)) {
            baseDir = std::filesystem::path(p);
            SDL_free(p);
        } else {
            baseDir = std::filesystem::current_path();
        }
    }

    // Ensure the directory exists (best-effort).
    {
        std::error_code ec;
        std::filesystem::create_directories(baseDir, ec);
    }

    const std::filesystem::path settingsPathFs = baseDir / "procrogue_settings.ini";
    const std::filesystem::path saveBasePathFs = baseDir / "procrogue_save.dat";
    const std::filesystem::path autosaveBasePathFs = baseDir / "procrogue_autosave.dat";
    const std::filesystem::path scoresPathFs = baseDir / "procrogue_scores.csv";
    const std::filesystem::path screenshotDirFs = baseDir / "screenshots";

    const std::string settingsPath = settingsPathFs.string();
    const std::string scoresPath = scoresPathFs.string();
    const std::string screenshotDir = screenshotDirFs.string();

    // Load or create settings.
    if (resetSettings) {
        // Best-effort backup to <file>.bak (overwrite any existing .bak).
        std::error_code ec;
        const std::filesystem::path bak = settingsPathFs.string() + ".bak";
        std::filesystem::remove(bak, ec);
        if (std::filesystem::exists(settingsPathFs)) {
            std::filesystem::rename(settingsPathFs, bak, ec);
        }
        writeDefaultSettings(settingsPath);
    } else if (!std::filesystem::exists(settingsPathFs)) {
        writeDefaultSettings(settingsPath);
    }

    Settings settings = loadSettings(settingsPath);

    // Resolve the initial save slot.
    // Priority: CLI --slot > settings default_slot > (empty = default).
    std::string initialSlot;
    if (slotArg && !slotArg->empty()) {
        initialSlot = sanitizeSlotName(*slotArg);
    } else if (!settings.defaultSlot.empty()) {
        initialSlot = sanitizeSlotName(settings.defaultSlot);
    }
    if (initialSlot == "default" || initialSlot == "none" || initialSlot == "off") {
        initialSlot.clear();
    }

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
    // Configure base save/autosave paths first, then apply the active slot.
    game.setSavePath(saveBasePathFs.string());
    game.setAutosavePath(autosaveBasePathFs.string());
    game.setActiveSlot(initialSlot);

    const std::string savePath = game.defaultSavePath();
    const std::string autosavePath = game.defaultAutosavePath();
    game.setScoresPath(scoresPath);
    game.setAutoStepDelayMs(settings.autoStepDelayMs);
    game.setAutosaveEveryTurns(settings.autosaveEveryTurns);
    game.setAutoPickupMode(settings.autoPickup);
    game.setIdentificationEnabled(settings.identifyItems);
    game.setHungerEnabled(settings.hungerEnabled);
    game.setConfirmQuitEnabled(settings.confirmQuit);
    game.setAutoMortemEnabled(settings.autoMortem);
    game.setSaveBackups(settings.saveBackups);

    game.setPlayerName(settings.playerName);
    game.setShowEffectTimers(settings.showEffectTimers);
    game.setSettingsPath(settingsPath);

    KeyBinds keyBinds = KeyBinds::defaults();
    keyBinds.loadOverridesFromIni(settingsPath);

    auto reloadKeyBindsFromDisk = [&]() {
        keyBinds = KeyBinds::defaults();
        keyBinds.loadOverridesFromIni(settingsPath);
    };

    const bool wantLoadSave = hasFlag(argc, argv, "--load") || hasFlag(argc, argv, "--continue");
    const bool wantLoadAuto = hasFlag(argc, argv, "--load-auto");
    const bool wantDaily = hasFlag(argc, argv, "--daily");

    enum class LoadedFrom {
        None,
        Save,
        Autosave,
    };

    LoadedFrom loadedFrom = LoadedFrom::None;

    const bool saveExists = std::filesystem::exists(savePath);
    const bool autoExists = std::filesystem::exists(autosavePath);

    bool saveTried = false;
    bool autoTried = false;
    bool saveOk = false;
    bool autoOk = false;

    auto attemptLoad = [&](const std::string& p, bool exists, bool& tried, bool& ok) {
        tried = true;
        if (!exists) {
            ok = false;
            return;
        }
        ok = game.loadFromFile(p);
    };

    if (wantLoadAuto) {
        attemptLoad(autosavePath, autoExists, autoTried, autoOk);
        if (autoOk) {
            loadedFrom = LoadedFrom::Autosave;
        } else if (wantLoadSave) {
            attemptLoad(savePath, saveExists, saveTried, saveOk);
            if (saveOk) loadedFrom = LoadedFrom::Save;
        }
    } else if (wantLoadSave) {
        attemptLoad(savePath, saveExists, saveTried, saveOk);
        if (saveOk) {
            loadedFrom = LoadedFrom::Save;
        } else {
            attemptLoad(autosavePath, autoExists, autoTried, autoOk);
            if (autoOk) loadedFrom = LoadedFrom::Autosave;
        }
    }

    const bool loaded = (loadedFrom != LoadedFrom::None);
    std::string startMsg;

    if (loaded) {
        if (loadedFrom == LoadedFrom::Autosave && wantLoadSave && !wantLoadAuto) {
            startMsg = "LOADED AUTOSAVE (SAVE UNAVAILABLE).";
        } else if (loadedFrom == LoadedFrom::Save && wantLoadAuto) {
            // This can happen if autosave was missing/corrupt and the user also passed --load.
            startMsg = "LOADED MANUAL SAVE (AUTOSAVE UNAVAILABLE).";
        } else if (loadedFrom == LoadedFrom::Autosave && wantLoadAuto) {
            startMsg = "LOADED AUTOSAVE.";
        }
    } else if (wantLoadSave || wantLoadAuto) {
        // We were asked to load a save on startup, but couldn't. We'll start a new run and tell the player why.
        if (wantLoadAuto && !wantLoadSave) {
            startMsg = autoExists ? "FAILED TO LOAD AUTOSAVE (CORRUPT?); STARTED NEW RUN."
                                  : "NO AUTOSAVE FILE FOUND; STARTED NEW RUN.";
        } else if (wantLoadSave && !wantLoadAuto) {
            if (!saveExists && !autoExists) startMsg = "NO SAVE/AUTOSAVE FOUND; STARTED NEW RUN.";
            else if (saveExists && !saveOk && !autoExists) startMsg = "FAILED TO LOAD SAVE (CORRUPT?); STARTED NEW RUN.";
            else if (!saveExists && autoExists && autoTried && !autoOk) startMsg = "NO SAVE FOUND; AUTOSAVE FAILED TO LOAD; STARTED NEW RUN.";
            else startMsg = "FAILED TO LOAD SAVE AND AUTOSAVE; STARTED NEW RUN.";
        } else { // wantLoadAuto && wantLoadSave
            if (!saveExists && !autoExists) startMsg = "NO AUTOSAVE OR SAVE FOUND; STARTED NEW RUN.";
            else startMsg = "FAILED TO LOAD AUTOSAVE AND SAVE; STARTED NEW RUN.";
        }
    }

    if (!loaded) {
        std::string dailyMsg;

        const std::optional<uint32_t> seedArg = parseSeedArg(argc, argv);
        uint32_t seed = 0u;
        if (seedArg.has_value()) {
            seed = *seedArg;
        } else if (wantDaily) {
            std::string dateIso;
            seed = dailySeedUtc(&dateIso);
            dailyMsg = "DAILY RUN (UTC " + dateIso + ") SEED: " + std::to_string(seed);
        } else {
            seed = static_cast<uint32_t>(SDL_GetTicks());
        }

        game.newGame(seed);
        game.setAutoPickupMode(settings.autoPickup);

        if (!startMsg.empty()) game.pushSystemMessage(startMsg);
        if (!dailyMsg.empty()) game.pushSystemMessage(dailyMsg);
    } else if (!startMsg.empty()) {
        game.pushSystemMessage(startMsg);
    }

    const bool vsyncEnabled = settings.vsync;
    const int maxFps = settings.maxFps;
    const uint32_t targetFrameMs = (!vsyncEnabled && maxFps > 0) ? (1000u / static_cast<uint32_t>(maxFps)) : 0u;

    bool running = true;
    uint32_t lastTicks = SDL_GetTicks();
    bool wantScreenshot = false;
    bool textInputOn = false;
    uint32_t lastEscPressMs = 0;

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
                        if (textInputOn) SDL_StopTextInput();

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

                case SDL_TEXTINPUT:
                    if (game.isCommandOpen()) {
                        game.commandTextInput(ev.text.text);
                    }
                    break;

                case SDL_KEYDOWN:
                    if (ev.key.repeat != 0) break;
                    {
                        const SDL_Keycode key = ev.key.keysym.sym;
                        const Uint16 mod = ev.key.keysym.mod;

                        // Extended command prompt: treat the keyboard as text input.
                        if (game.isCommandOpen()) {
                            if (key == SDLK_ESCAPE) {
                                game.handleAction(Action::Cancel);
                                lastEscPressMs = 0;
                            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                                game.handleAction(Action::Confirm);
                            } else if (key == SDLK_BACKSPACE) {
                                game.commandBackspace();
                            } else if (key == SDLK_UP) {
                                game.handleAction(Action::Up);
                            } else if (key == SDLK_DOWN) {
                                game.handleAction(Action::Down);
                            } else if (key == SDLK_TAB) {
                                game.commandAutocomplete();
                            }
                            break;
                        }

                        if (key == SDLK_ESCAPE) {
                            // ESC cancels UI modes; cancels auto-move; otherwise quit (optionally double-press).
                            if (game.isInventoryOpen() || game.isTargeting() || game.isHelpOpen() || game.isLooking() ||
                                game.isMinimapOpen() || game.isStatsOpen() || game.isOptionsOpen() || game.isCommandOpen() ||
                                game.isAutoActive()) {
                                game.handleAction(Action::Cancel);
                                lastEscPressMs = 0;
                            } else {
                                if (!game.confirmQuitEnabled()) {
                                    running = false;
                                } else {
                                    const uint32_t now = SDL_GetTicks();
                                    if (lastEscPressMs != 0 && (now - lastEscPressMs) < 1500u) {
                                        running = false;
                                    } else {
                                        lastEscPressMs = now;
                                        game.pushSystemMessage("PRESS ESC AGAIN TO QUIT.");
                                    }
                                }
                            }
                            break;
                        }

                        const Action a = keyBinds.mapKey(game, key, mod);

                        // These are handled at the platform layer (so they work even during auto-move).
                        if (a == Action::ToggleFullscreen) {
                            renderer.toggleFullscreen();
                            break;
                        }
                        if (a == Action::Screenshot) {
                            wantScreenshot = true;
                            break;
                        }

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
                        if (game.isInventoryOpen() || game.isHelpOpen() || game.isMinimapOpen() || game.isStatsOpen() ||
                            game.isOptionsOpen() || game.isCommandOpen())
                            break;

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

        // Toggle SDL text input for the extended command prompt.
        if (game.isCommandOpen() && !textInputOn) {
            SDL_StartTextInput();
            textInputOn = true;
        } else if (!game.isCommandOpen() && textInputOn) {
            SDL_StopTextInput();
            textInputOn = false;
        }

        // Persist settings changes (options menu + some extended commands).
        if (game.settingsDirty() && !game.isOptionsOpen()) {
            auto autoPickupToString = [](AutoPickupMode m) -> std::string {
                switch (m) {
                    case AutoPickupMode::Off: return "off";
                    case AutoPickupMode::Gold: return "gold";
                    case AutoPickupMode::Smart: return "smart";
                    case AutoPickupMode::All: return "all";
                }
                return "gold";
            };

            bool ok = true;
            ok &= updateIniKey(settingsPath, "auto_pickup", autoPickupToString(game.autoPickupMode()));
            ok &= updateIniKey(settingsPath, "auto_step_delay_ms", std::to_string(game.autoStepDelayMs()));
            ok &= updateIniKey(settingsPath, "identify_items", game.identificationEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "hunger_enabled", game.hungerEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "confirm_quit", game.confirmQuitEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "auto_mortem", game.autoMortemEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "autosave_every_turns", std::to_string(game.autosaveEveryTurns()));
            ok &= updateIniKey(settingsPath, "save_backups", std::to_string(game.saveBackups()));

            // Only persist the default slot if the user changed it in-game (e.g., via #slot).
            if (game.slotDirty()) {
                ok &= updateIniKey(settingsPath, "default_slot", game.activeSlot());
            }

            ok &= updateIniKey(settingsPath, "player_name", game.playerName());
            ok &= updateIniKey(settingsPath, "show_effect_timers", game.showEffectTimers() ? "true" : "false");

            if (!ok) game.pushSystemMessage("FAILED TO SAVE SETTINGS.");

            game.clearSlotDirty();

            game.clearSettingsDirty();
        }

        // App-level requests (extended commands)
        if (game.configReloadRequested()) {
            const bool prevControllerEnabled = settings.controllerEnabled;

            Settings newSettings = loadSettings(settingsPath);

            // Apply a safe subset immediately. Renderer/window related settings still require restart.
            game.setAutoPickupMode(newSettings.autoPickup);
            game.setAutoStepDelayMs(newSettings.autoStepDelayMs);
            game.setAutosaveEveryTurns(newSettings.autosaveEveryTurns);
            game.setSaveBackups(newSettings.saveBackups);
            game.setIdentificationEnabled(newSettings.identifyItems);
            game.setHungerEnabled(newSettings.hungerEnabled);
            game.setConfirmQuitEnabled(newSettings.confirmQuit);
            game.setAutoMortemEnabled(newSettings.autoMortem);
            game.setPlayerName(newSettings.playerName);
            game.setShowEffectTimers(newSettings.showEffectTimers);

            // Keep the local copy up-to-date for any later use.
            settings = newSettings;

            // Controller can be toggled at runtime (best-effort).
            if (!settings.controllerEnabled && prevControllerEnabled) {
                closeController();
            } else if (settings.controllerEnabled && !prevControllerEnabled) {
                openFirstController();
            }

            reloadKeyBindsFromDisk();

            game.pushSystemMessage("RELOADED SETTINGS + KEYBINDS. (TILE SIZE/VSYNC/FULLSCREEN REQUIRE RESTART)");
            game.clearConfigReloadRequest();
        }

        if (game.keyBindsReloadRequested()) {
            reloadKeyBindsFromDisk();
            game.pushSystemMessage("RELOADED KEYBINDS.");
            game.clearKeyBindsReloadRequest();
        }

        if (game.keyBindsDumpRequested()) {
            game.pushSystemMessage("KEYBINDS:");
            const auto desc = keyBinds.describeAll();

            int shown = 0;
            for (const auto& kv : desc) {
                game.pushSystemMessage("  " + kv.first + " = " + kv.second);
                if (++shown >= 60) {
                    game.pushSystemMessage("  ...");
                    break;
                }
            }

            game.pushSystemMessage("TIP: #bind <action> <keys> | #unbind <action> | #reload");
            game.clearKeyBindsDumpRequest();
        }

        // Quit requests (e.g. from extended command "quit").
        if (game.quitRequested()) {
            running = false;
            game.clearQuitRequest();
        }

        if (!running) break;

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

    if (textInputOn) SDL_StopTextInput();

    closeController();
    renderer.shutdown();
    SDL_Quit();
    return 0;
}
