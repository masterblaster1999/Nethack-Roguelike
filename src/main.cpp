#include "sdl.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

#include "game.hpp"
#include "content.hpp"
#include "keybinds.hpp"
#include "replay.hpp"
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

static std::string timestampForReplayFilename() {
    // Local time, suitable for filenames.
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

static void printUsage(const char* exe) {
    std::cout
        << PROCROGUE_APPNAME << " " << PROCROGUE_VERSION << "\n"
        << "Usage: " << (exe ? exe : "procrogue") << " [options]\n\n"
        << "Options:\n"
        << "  --seed <n>           Start a new run with a specific seed\n"
        << "  --class <name>      Start a new run as a class (adventurer|knight|rogue|archer|wizard)\n"
        << "  --daily              Start a daily run (deterministic UTC-date seed)\n"
        << "  --load               Auto-load the manual save on start (alias: --continue)\n"
        << "  --load-auto          Auto-load the autosave on start\n"
        << "  --record [path]      Record inputs to a replay file (default: procrogue_replay_<timestamp>.prr)\n"
        << "  --replay <path>      Play back a recorded replay file (disables live input until finished)\n"
        << "  --record-hashes      Record per-turn deterministic state hashes in replays (default)\n"
        << "  --no-record-hashes   Disable per-turn hash recording (smaller replays)\n"
        << "  --record-hash-interval <n> Record one state hash every N turns (default: 1)\n"
        << "  --verify-hashes      Verify per-turn state hashes during replay (default)\n"
        << "  --no-verify-hashes   Disable hash verification during replay playback\n"
        << "  --content <path>     Load content/balance overrides from an INI file (optional)\n"
        << "  --data-dir <path>    Override the save/config directory\n"
        << "  --slot <name>        Use a named save slot (affects save + autosave files)\n"
        << "  --portable           Store saves/config next to the executable\n"
        << "  --reset-settings     Overwrite settings with fresh defaults\n"
        << "\n"
        << "  --version, -v        Print version and exit\n"
        << "  --help, -h           Show this help and exit\n";
}

namespace {

struct TurnHashCheckpoint {
    uint32_t turn = 0;
    uint64_t hash = 0;
};

struct TurnHashRecordCtx {
    ReplayWriter* writer = nullptr;
    uint32_t recordStartTicks = 0;
    uint32_t interval = 1;
};

static std::string hex64(uint64_t v) {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << v;
    return ss.str();
}

static void onTurnHashRecord(void* user, uint32_t turn, uint64_t hash) {
    auto* ctx = static_cast<TurnHashRecordCtx*>(user);
    if (!ctx || !ctx->writer) return;
    if (!ctx->writer->isOpen()) return;
    if (ctx->interval > 1 && (turn % ctx->interval) != 0) return;
    const uint32_t now = SDL_GetTicks();
    const uint32_t tMs = (now >= ctx->recordStartTicks) ? (now - ctx->recordStartTicks) : 0u;
    ctx->writer->writeStateHash(tMs, turn, hash);
}

struct TurnHashVerifyCtx {
    const std::vector<TurnHashCheckpoint>* expected = nullptr;
    size_t idx = 0;
    bool failed = false;
    uint32_t failedTurn = 0;
    uint64_t expectedHash = 0;
    uint64_t gotHash = 0;
};

static void onTurnHashVerify(void* user, uint32_t turn, uint64_t hash) {
    auto* ctx = static_cast<TurnHashVerifyCtx*>(user);
    if (!ctx || ctx->failed || !ctx->expected) return;
    while (ctx->idx < ctx->expected->size() && (*ctx->expected)[ctx->idx].turn < turn) {
        // We missed one or more expected checkpoints; treat as failure.
        ctx->failed = true;
        ctx->failedTurn = turn;
        ctx->expectedHash = (*ctx->expected)[ctx->idx].hash;
        ctx->gotHash = hash;
        return;
    }
    if (ctx->idx < ctx->expected->size() && (*ctx->expected)[ctx->idx].turn == turn) {
        const uint64_t exp = (*ctx->expected)[ctx->idx].hash;
        ctx->idx++;
        if (exp != hash) {
            ctx->failed = true;
            ctx->failedTurn = turn;
            ctx->expectedHash = exp;
            ctx->gotHash = hash;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (hasFlag(argc, argv, "--help") || hasFlag(argc, argv, "-h")) {
        printUsage(argc > 0 ? argv[0] : "procrogue");
        return 0;
    }
    if (hasFlag(argc, argv, "--version") || hasFlag(argc, argv, "-v")) {
        std::cout << PROCROGUE_APPNAME << " " << PROCROGUE_VERSION << "\n";
        return 0;
    }

    const std::optional<std::string> recordArg = parseStringArg(argc, argv, "--record");
    const bool wantRecord = hasFlag(argc, argv, "--record");

    // Replay hash recording/verification options (useful for determinism debugging).
    bool recordHashes = true;
    if (hasFlag(argc, argv, "--no-record-hashes")) recordHashes = false;
    if (hasFlag(argc, argv, "--record-hashes")) recordHashes = true;
    uint32_t recordHashInterval = 1;
    if (auto s = parseStringArg(argc, argv, "--record-hash-interval")) {
        try {
            const int v = std::stoi(*s);
            if (v > 0) recordHashInterval = static_cast<uint32_t>(v);
        } catch (...) {
            std::cerr << "Invalid --record-hash-interval value; using 1\n";
        }
    }

    bool verifyReplayHashes = true;
    if (hasFlag(argc, argv, "--no-verify-hashes")) verifyReplayHashes = false;
    if (hasFlag(argc, argv, "--verify-hashes")) verifyReplayHashes = true;

    const std::optional<std::string> replayArg = parseStringArg(argc, argv, "--replay");
    const bool wantReplay = hasFlag(argc, argv, "--replay");
    if (wantReplay && (!replayArg || replayArg->empty())) {
        std::cerr << "--replay requires <path>\n";
        printUsage(argc > 0 ? argv[0] : "procrogue");
        return 1;
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // Determine where to store settings/saves.
    // By default we use a per-user writable directory (SDL_GetPrefPath), but this can be overridden.
    const std::optional<std::string> dataDirArg = parseStringArg(argc, argv, "--data-dir");
    const std::optional<std::string> contentArg = parseStringArg(argc, argv, "--content");
    const std::optional<std::string> slotArg = parseStringArg(argc, argv, "--slot");
    const std::optional<std::string> classArg = parseStringArg(argc, argv, "--class");
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


    // ------------------------------------------------------------
    // Content overrides (optional): a simple INI-ish file that can tweak
    // monster/item stats and spawn weights without recompiling.
    // ------------------------------------------------------------
    {
        bool haveContent = false;
        std::filesystem::path contentPathFs;

        if (contentArg && !contentArg->empty()) {
            contentPathFs = std::filesystem::path(*contentArg);
            if (contentPathFs.is_relative()) contentPathFs = baseDir / contentPathFs;
            haveContent = true;
        } else {
            const std::filesystem::path defaultContent = baseDir / "procrogue_content.ini";
            if (std::filesystem::exists(defaultContent)) {
                contentPathFs = defaultContent;
                haveContent = true;
            }
        }

        if (haveContent) {
            ContentOverrides co;
            std::string warns;
            if (!loadContentOverridesIni(contentPathFs.string(), co, &warns)) {
                std::cerr << "Failed to load content overrides: " << contentPathFs.string() << "\n";
                if (!warns.empty()) std::cerr << warns;
                if (contentArg && !contentArg->empty()) {
                    SDL_Quit();
                    return 1;
                }
            } else {
                setContentOverrides(std::move(co));
                std::cout << "Loaded content overrides: " << contentPathFs.string() << "\n";
                if (!warns.empty()) {
                    std::cout << warns;
                }
            }
        }
    }

    // ------------------------------------------------------------
    // Replay (optional): load early so we can apply deterministic settings+seed
    // before starting a new game.
    // ------------------------------------------------------------
    bool replayMode = false;
    ReplayFile replayFile;
    std::filesystem::path replayPathFs;

    if (wantReplay && replayArg && !replayArg->empty()) {
        replayPathFs = std::filesystem::path(*replayArg);
        if (replayPathFs.is_relative()) replayPathFs = baseDir / replayPathFs;

        std::string err;
        if (!loadReplayFile(replayPathFs, replayFile, &err)) {
            std::cerr << err << "\n";
            SDL_Quit();
            return 1;
        }
        replayMode = true;
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

    // Replays run in an isolated slot to avoid touching the user's real save/autosave files.
    if (replayMode) {
        initialSlot = "__replay__";
    }

    const int tileSize = settings.tileSize;
    const int hudHeight = settings.hudHeight;

    // Viewport / camera:
    // - If view_w/view_h are set in settings, they define the visible tile area.
    // - Otherwise, auto-fit a viewport that comfortably fits the current display.
    //
    // This keeps the game playable even with large tile sizes, and enables a scrolling camera
    // when the viewport is smaller than the dungeon map.
    const int mapW = Game::MAP_W;
    const int mapH = Game::MAP_H;

    auto clampViewW = [&](int v) {
        if (v <= 0) return 0;
        return std::clamp(v, 1, mapW);
    };
    auto clampViewH = [&](int v) {
        if (v <= 0) return 0;
        return std::clamp(v, 1, mapH);
    };

    int viewW = clampViewW(settings.viewW);
    int viewH = clampViewH(settings.viewH);

    if (viewW == 0 || viewH == 0) {
        // Auto-fit to display size (conservative margins for window decorations/taskbar).
        SDL_DisplayMode dm;
        int dispW = 1920;
        int dispH = 1080;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
            dispW = dm.w;
            dispH = dm.h;
        }

        const int maxWinW = std::max(640, dispW - 120);
        const int maxWinH = std::max(480, dispH - 120);

        const int fullW = mapW * tileSize;
        const int fullH = mapH * tileSize + hudHeight;

        if (fullW <= maxWinW && fullH <= maxWinH) {
            viewW = mapW;
            viewH = mapH;
        } else {
            viewW = std::clamp(maxWinW / std::max(1, tileSize), 1, mapW);
            const int availH = std::max(0, maxWinH - hudHeight);
            viewH = std::clamp(availH / std::max(1, tileSize), 1, mapH);
        }
    }

    const int winW = viewW * tileSize;
    const int winH = viewH * tileSize + hudHeight;

    Renderer renderer(winW, winH, tileSize, hudHeight, settings.vsync, settings.textureCacheMB);
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
    game.setAutoExploreSearchEnabled(settings.autoExploreSearch);
    game.setIdentificationEnabled(settings.identifyItems);
    game.setHungerEnabled(settings.hungerEnabled);
    game.setEncumbranceEnabled(settings.encumbranceEnabled);
    game.setLightingEnabled(settings.lightingEnabled);
    game.setYendorDoomEnabled(settings.yendorDoomEnabled);
    game.setConfirmQuitEnabled(settings.confirmQuit);
    game.setAutoMortemEnabled(settings.autoMortem);
    game.setBonesEnabled(settings.bonesEnabled);
    game.setVoxelSpritesEnabled(settings.voxelSprites);
    game.setSaveBackups(settings.saveBackups);

    if (replayMode) {
        // Keep replays self-contained and non-destructive.
        // (Avoid writing autosaves/mortems/backups into the user's normal slot.)
        game.setAutosaveEveryTurns(0);
        game.setSaveBackups(0);
        game.setAutoMortemEnabled(false);

        // Apply recorded gameplay-affecting settings for determinism.
        game.setAutoStepDelayMs(replayFile.meta.autoStepDelayMs);
        game.setAutoPickupMode(replayFile.meta.autoPickup);
        game.setAutoExploreSearchEnabled(replayFile.meta.autoExploreSearch);
        game.setIdentificationEnabled(replayFile.meta.identifyItems);
        game.setHungerEnabled(replayFile.meta.hungerEnabled);
        game.setEncumbranceEnabled(replayFile.meta.encumbranceEnabled);
        game.setLightingEnabled(replayFile.meta.lightingEnabled);
        game.setYendorDoomEnabled(replayFile.meta.yendorDoomEnabled);
        game.setBonesEnabled(replayFile.meta.bonesEnabled);
    }

    game.setPlayerName(settings.playerName);

    // Default starting class for new runs.
    PlayerClass startClass = settings.playerClass;
    if (classArg && !classArg->empty()) {
        PlayerClass pc = PlayerClass::Adventurer;
        if (parsePlayerClass(*classArg, pc)) {
            startClass = pc;
        } else {
            std::cerr << "Unknown class '" << *classArg << "' (use adventurer|knight|rogue|archer|wizard)."
                      << " Falling back to settings player_class.\n";
        }
    }

    // Replays override the chosen starting class (for determinism).
    if (replayMode && !replayFile.meta.playerClassId.empty()) {
        PlayerClass pc = startClass;
        if (parsePlayerClass(replayFile.meta.playerClassId, pc)) {
            startClass = pc;
        }
    }
    game.setPlayerClass(startClass);
    game.setShowEffectTimers(settings.showEffectTimers);
    game.setUITheme(settings.uiTheme);
    game.setUIPanelsTextured(settings.uiPanelsTextured);
    game.setViewMode(settings.viewMode);
    game.setControlPreset(settings.controlPreset);
    game.setSettingsPath(settingsPath);

    KeyBinds keyBinds = KeyBinds::defaults();
    keyBinds.loadOverridesFromIni(settingsPath);
    game.setKeybindsDescription(keyBinds.describeAll());

    auto reloadKeyBindsFromDisk = [&]() {
        keyBinds = KeyBinds::defaults();
        keyBinds.loadOverridesFromIni(settingsPath);
        game.setKeybindsDescription(keyBinds.describeAll());
    };

    bool wantLoadSave = hasFlag(argc, argv, "--load") || hasFlag(argc, argv, "--continue");
    bool wantLoadAuto = hasFlag(argc, argv, "--load-auto");
    bool wantDaily = hasFlag(argc, argv, "--daily");

    // Replays always start a fresh run from the replay header's seed/class.
    if (replayMode) {
        wantLoadSave = false;
        wantLoadAuto = false;
        wantDaily = false;
    }

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

        if (replayMode) {
            seed = replayFile.meta.seed;
        } else if (seedArg.has_value()) {
            seed = *seedArg;
        } else if (wantDaily) {
            std::string dateIso;
            seed = dailySeedUtc(&dateIso);
            dailyMsg = "DAILY RUN (UTC " + dateIso + ") SEED: " + std::to_string(seed);
        } else {
            seed = static_cast<uint32_t>(SDL_GetTicks());
        }

        game.newGame(seed);

        // newGame() resets some settings; re-apply the intended auto-pickup mode.
        if (replayMode) {
            game.setAutoPickupMode(replayFile.meta.autoPickup);
        game.setAutoExploreSearchEnabled(replayFile.meta.autoExploreSearch);
        } else {
            game.setAutoPickupMode(settings.autoPickup);
    game.setAutoExploreSearchEnabled(settings.autoExploreSearch);
        }

        if (replayMode) {
            game.pushSystemMessage("REPLAY MODE: " + replayPathFs.string());
            if (!replayFile.meta.gameVersion.empty() && replayFile.meta.gameVersion != PROCROGUE_VERSION) {
                game.pushSystemMessage("WARNING: REPLAY VERSION MISMATCH (" + replayFile.meta.gameVersion + " != " + std::string(PROCROGUE_VERSION) + ")");
            }
        }

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


    // ------------------------------------------------------------
    // Replay playback state
    // ------------------------------------------------------------
    bool replayActive = replayMode;
    size_t replayIndex = 0;
    uint32_t replayStartTicks = SDL_GetTicks();


    // ------------------------------------------------------------
    // Replay recording state
    // ------------------------------------------------------------
    ReplayWriter recorder;
    TurnHashRecordCtx recordHashCtx;
    bool recording = false;
    uint32_t recordStartTicks = 0;
    std::optional<Vec2i> lastTargetCursorRecorded;
    std::optional<Vec2i> lastLookCursorRecorded;

    auto recordTimeMs = [&]() -> uint32_t {
        const uint32_t nowTicks = SDL_GetTicks();
        return (nowTicks >= recordStartTicks) ? (nowTicks - recordStartTicks) : 0u;
    };

    auto startRecording = [&](std::filesystem::path outPath) -> bool {
        if (recording) return true;

        if (outPath.empty()) {
            outPath = baseDir / ("procrogue_replay_" + timestampForReplayFilename() + ".prr");
        }
        if (outPath.is_relative()) {
            outPath = baseDir / outPath;
        }
        {
            std::error_code ec;
            std::filesystem::create_directories(outPath.parent_path(), ec);
        }

        ReplayMeta meta;
        meta.gameVersion = PROCROGUE_VERSION;
        meta.seed = game.seed();
        meta.playerClassId = game.playerClassIdString();
        meta.autoStepDelayMs = game.autoStepDelayMs();
        meta.autoPickup = game.autoPickupMode();
        meta.autoExploreSearch = game.autoExploreSearchEnabled();
        meta.identifyItems = game.identificationEnabled();
        meta.hungerEnabled = game.hungerEnabled();
        meta.encumbranceEnabled = game.encumbranceEnabled();
        meta.lightingEnabled = game.lightingEnabled();
        meta.yendorDoomEnabled = game.yendorDoomEnabled();
        meta.bonesEnabled = game.bonesEnabled();

        std::string err;
        if (!recorder.open(outPath, meta, &err)) {
            std::cerr << "Failed to start recording: " << err << "\n";
            game.pushSystemMessage("REPLAY RECORD FAILED.");
            return false;
        }

        recordStartTicks = SDL_GetTicks();
        recording = true;
        if (recordHashes) {
            recordHashCtx.writer = &recorder;
            recordHashCtx.recordStartTicks = recordStartTicks;
            recordHashCtx.interval = recordHashInterval;
            game.setTurnHook(&onTurnHashRecord, &recordHashCtx);
            // Initial checkpoint at turn 0 (before any time-consuming action).
            recorder.writeStateHash(0, game.turns(), game.determinismHash());
        } else {
            game.setTurnHook(nullptr, nullptr);
        }
        game.pushSystemMessage("RECORDING REPLAY: " + outPath.string());
        return true;
    };

    auto stopRecording = [&]() {
        if (!recording) return;
        game.setTurnHook(nullptr, nullptr);
        recorder.close();
        recording = false;
        game.pushSystemMessage("STOPPED RECORDING REPLAY.");
    };

    // Replay hash verification (optional)
    std::vector<TurnHashCheckpoint> replayHashCheckpoints;
    TurnHashVerifyCtx replayVerifyCtx;
    bool replayVerifyArmed = false;
    auto handleReplayHashFailure = [&]() {
        if (!replayVerifyCtx.failed) return;
        replayActive = false;
        game.setTurnHook(nullptr, nullptr);
        const std::string expected = hex64(replayVerifyCtx.expectedHash);
        const std::string got = hex64(replayVerifyCtx.gotHash);
        std::ostringstream ss;
        ss << "REPLAY DIVERGED at turn " << replayVerifyCtx.failedTurn
           << ": expected 0x" << expected << ", got 0x" << got;
        const std::string msg = ss.str();
        std::cerr << msg << "\n";
        game.pushSystemMessage(msg);
    };

    // Start recording immediately if requested.
    if (!replayActive && wantRecord) {
        std::filesystem::path outPath;
        if (recordArg && !recordArg->empty() && recordArg->front() != '-') {
            outPath = *recordArg;
        }

        startRecording(outPath);
    }

    // Arm replay hash verification if the replay contains hash checkpoints.
    if (replayMode && verifyReplayHashes) {
        for (const auto& ev : replayFile.events) {
            if (ev.kind == ReplayEventType::StateHash) {
                replayHashCheckpoints.push_back(TurnHashCheckpoint{ev.turn, ev.hash});
            }
        }
        if (!replayHashCheckpoints.empty()) {
            replayVerifyCtx.expected = &replayHashCheckpoints;
            game.setTurnHook(&onTurnHashVerify, &replayVerifyCtx);
            replayVerifyArmed = true;
            // Validate initial state (turn 0) immediately, if present in the replay.
            onTurnHashVerify(&replayVerifyCtx, game.turns(), game.determinismHash());
            handleReplayHashFailure();
        }
    }

    auto dispatchAction = [&](Action a, bool fromReplay = false) {
        if (a == Action::None) return;
        if (recording && !fromReplay) {
            recorder.writeAction(recordTimeMs(), a);
        }

        // These are handled at the platform layer (so they work even during auto-move).
        if (a == Action::ToggleFullscreen) {
            renderer.toggleFullscreen();
            return;
        }
        if (a == Action::Screenshot) {
            wantScreenshot = true;
            return;
        }

        game.handleAction(a);
    };

    auto dispatchReplayEvent = [&](const ReplayEvent& rev) {
        switch (rev.kind) {
            case ReplayEventType::StateHash:
                // Hash checkpoints are validated via the per-turn hook.
                break;
            case ReplayEventType::Action:
                dispatchAction(rev.action, /*fromReplay=*/true);
                break;
            case ReplayEventType::TextInput:
                if (game.isCommandOpen()) {
                    game.commandTextInput(rev.text.c_str());
                } else if (game.isMessageHistoryOpen() && game.isMessageHistorySearchMode()) {
                    game.messageHistoryTextInput(rev.text.c_str());
                }
                break;
            case ReplayEventType::CommandBackspace:
                if (game.isCommandOpen()) game.commandBackspace();
                break;
            case ReplayEventType::CommandAutocomplete:
                if (game.isCommandOpen()) game.commandAutocomplete();
                break;
            case ReplayEventType::MessageHistoryBackspace:
                if (game.isMessageHistoryOpen()) game.messageHistoryBackspace();
                break;
            case ReplayEventType::MessageHistoryToggleSearch:
                if (game.isMessageHistoryOpen()) game.messageHistoryToggleSearchMode();
                break;
            case ReplayEventType::MessageHistoryClearSearch:
                if (game.isMessageHistoryOpen()) game.messageHistoryClearSearch();
                break;
            case ReplayEventType::AutoTravel:
                game.requestAutoTravel(rev.pos);
                break;
            case ReplayEventType::BeginLook:
                game.beginLookAt(rev.pos);
                break;
            case ReplayEventType::TargetCursor:
                if (game.isTargeting()) game.setTargetCursor(rev.pos);
                break;
            case ReplayEventType::LookCursor:
                if (game.isLooking()) game.setLookCursor(rev.pos);
                break;
        }
    };

    while (running) {
        const uint32_t frameStart = SDL_GetTicks();
        const uint32_t now = frameStart;

        float dt = (now - lastTicks) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        lastTicks = now;

        // Keep renderer in sync with the current camera/view mode (affects mouse->tile mapping).
        renderer.setViewMode(game.viewMode());

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
                    if (replayActive) break;
                    if (!settings.controllerEnabled) break;
                    switch (ev.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:    dispatchAction(Action::Up); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  dispatchAction(Action::Down); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  dispatchAction(Action::Left); break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: dispatchAction(Action::Right); break;

                        case SDL_CONTROLLER_BUTTON_A:          dispatchAction(Action::Confirm); break;
                        case SDL_CONTROLLER_BUTTON_B:          dispatchAction(Action::Cancel); break;
                        case SDL_CONTROLLER_BUTTON_X:          dispatchAction(Action::Inventory); break;
                        case SDL_CONTROLLER_BUTTON_Y:          dispatchAction(Action::Pickup); break;

                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  dispatchAction(Action::Look); break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: dispatchAction(Action::Fire); break;

                        case SDL_CONTROLLER_BUTTON_BACK:  dispatchAction(Action::Help); break;
                        case SDL_CONTROLLER_BUTTON_START: dispatchAction(Action::ToggleStats); break;

                        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: dispatchAction(Action::ToggleMinimap); break;
                        default:
                            break;
                    }
                    break;

                case SDL_TEXTINPUT:
                    if (replayActive) break;
                    if (game.isCommandOpen()) {
                        if (recording) recorder.writeTextInput(recordTimeMs(), ev.text.text);
                        game.commandTextInput(ev.text.text);
                    } else if (game.isMessageHistoryOpen() && game.isMessageHistorySearchMode()) {
                        if (recording) recorder.writeTextInput(recordTimeMs(), ev.text.text);
                        game.messageHistoryTextInput(ev.text.text);
                    }
                    break;

                case SDL_KEYDOWN:
                    if (ev.key.repeat != 0) break;
                    {
                        // For configurable keybinds we want the *unmodified* key symbol
                        // (layout-aware) plus the explicit modifier bits.
                        //
                        // SDL will often report SHIFT-modified punctuation as a different
                        // keycode (e.g. '?' instead of '/' + Shift). Using the scancode to
                        // recover the base key makes bindings like `shift+slash` and
                        // `shift+3` work reliably across layouts.
                        const SDL_Keycode rawKey = ev.key.keysym.sym;
                        const SDL_Scancode sc = ev.key.keysym.scancode;
                        SDL_Keycode key = SDL_GetKeyFromScancode(sc);
                        if (key == SDLK_UNKNOWN) key = rawKey;

                        const Uint16 mod = ev.key.keysym.mod;

                        // During replay playback, ignore live input (optional ESC to exit).
                        if (replayActive) {
                            if (key == SDLK_ESCAPE) running = false;
                            break;
                        }

                        // Keybind editor capture: interpret the next key press as a raw chord token
                        // (do NOT map it to an in-game Action).
                        if (game.isKeybindsCapturing()) {
                            if (key == SDLK_ESCAPE) {
                                game.keybindsCancelCapture();
                                break;
                            }
                            if (KeyBinds::isModifierKey(key)) {
                                break; // wait for a non-modifier key
                            }
                            const std::string chord = KeyBinds::chordToString(key, mod);
                            game.keybindsCaptureToken(chord);
                            break;
                        }

                        // Extended command prompt: treat the keyboard as text input.
                        if (game.isCommandOpen()) {
                            if (key == SDLK_ESCAPE) {
                                dispatchAction(Action::Cancel);
                                lastEscPressMs = 0;
                            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
                                dispatchAction(Action::Confirm);
                            } else if (key == SDLK_BACKSPACE) {
                                if (recording) recorder.writeCommandBackspace(recordTimeMs());
                                game.commandBackspace();
                            } else if (key == SDLK_UP) {
                                dispatchAction(Action::Up);
                            } else if (key == SDLK_DOWN) {
                                dispatchAction(Action::Down);
                            } else if (key == SDLK_TAB) {
                                if (recording) recorder.writeCommandAutocomplete(recordTimeMs());
                                game.commandAutocomplete();
                            }
                            break;
                        }

                        // Message history overlay: intercept a few text/navigation keys so they
                        // don't get interpreted as gameplay actions (especially Backspace).
                        if (game.isMessageHistoryOpen()) {
                            if (!game.isMessageHistorySearchMode() && key == SDLK_SLASH && (mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) == 0 && (mod & KMOD_SHIFT) == 0) {
                                if (recording) recorder.writeMessageHistoryToggleSearchMode(recordTimeMs());
                                game.messageHistoryToggleSearchMode();
                                break;
                            }
                            if (key == SDLK_BACKSPACE) {
                                if (recording) recorder.writeMessageHistoryBackspace(recordTimeMs());
                                game.messageHistoryBackspace();
                                break;
                            }
                            if ((mod & KMOD_CTRL) != 0 && (key == SDLK_l)) {
                                if (recording) recorder.writeMessageHistoryClearSearch(recordTimeMs());
                                game.messageHistoryClearSearch();
                                break;
                            }
                        }

                        if (key == SDLK_ESCAPE) {
                            // ESC cancels UI modes; cancels auto-move; otherwise quit (optionally double-press).
                            if (game.isInventoryOpen() || game.isTargeting() || game.isHelpOpen() || game.isLooking() ||
                                game.isMinimapOpen() || game.isStatsOpen() || game.isMessageHistoryOpen() || game.isOptionsOpen() || game.isKeybindsOpen() || game.isCommandOpen() ||
                                game.isAutoActive()) {
                                dispatchAction(Action::Cancel);
                                lastEscPressMs = 0;
                            } else {
                                if (!game.confirmQuitEnabled()) {
                                    running = false;
                                } else {
                                    const uint32_t nowTicks = SDL_GetTicks();
                                    if (lastEscPressMs != 0 && (nowTicks - lastEscPressMs) < 1500u) {
                                        running = false;
                                    } else {
                                        lastEscPressMs = nowTicks;
                                        game.pushSystemMessage("PRESS ESC AGAIN TO QUIT.");
                                    }
                                }
                            }
                            break;
                        }

                        const Action a = keyBinds.mapKey(game, key, mod);
                        dispatchAction(a);
                    }
                    break;

                case SDL_MOUSEWHEEL:
                    if (replayActive) break;
                    if (ev.wheel.y > 0) dispatchAction(Action::LogUp);
                    else if (ev.wheel.y < 0) dispatchAction(Action::LogDown);
                    break;

                case SDL_MOUSEMOTION:
                    if (replayActive) break;
                    {
                        int tx = 0, ty = 0;
                        // Minimap hover updates the minimap cursor (UI-only).
                        if (game.isMinimapOpen()) {
                            if (!renderer.windowToMinimapTile(game, ev.motion.x, ev.motion.y, tx, ty)) break;
                            game.setMinimapCursor(Vec2i{tx, ty});
                            break;
                        }

                        if (!renderer.windowToMapTile(ev.motion.x, ev.motion.y, tx, ty)) break;
                        const Vec2i p{tx, ty};

                        if (game.isTargeting()) {
                            game.setTargetCursor(p);
                            if (recording && (!lastTargetCursorRecorded.has_value() || *lastTargetCursorRecorded != p)) {
                                recorder.writeTargetCursor(recordTimeMs(), p);
                                lastTargetCursorRecorded = p;
                            }
                            lastLookCursorRecorded.reset();
                        } else if (game.isLooking()) {
                            game.setLookCursor(p);
                            if (recording && (!lastLookCursorRecorded.has_value() || *lastLookCursorRecorded != p)) {
                                recorder.writeLookCursor(recordTimeMs(), p);
                                lastLookCursorRecorded = p;
                            }
                            lastTargetCursorRecorded.reset();
                        } else {
                            lastTargetCursorRecorded.reset();
                            lastLookCursorRecorded.reset();
                        }
                    }
                    break;

                case SDL_MOUSEBUTTONDOWN:
                    if (replayActive) break;
                    {
                        // Minimap: click to travel/look.
                        if (game.isMinimapOpen()) {
                            int tx = 0, ty = 0;
                            if (!renderer.windowToMinimapTile(game, ev.button.x, ev.button.y, tx, ty)) break;
                            const Vec2i p{tx, ty};

                            game.setMinimapCursor(p);

                            if (ev.button.button == SDL_BUTTON_LEFT) {
                                if (recording) recorder.writeAutoTravel(recordTimeMs(), p);
                                game.requestAutoTravel(p);
                            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                                if (recording) recorder.writeBeginLook(recordTimeMs(), p);
                                game.beginLookAt(p);
                            }
                            break;
                        }

                        // Ignore mouse when menus are open.
                        if (game.isInventoryOpen() || game.isHelpOpen() || game.isStatsOpen() ||
                            game.isMessageHistoryOpen() || game.isOptionsOpen() || game.isCommandOpen())
                            break;

                        int tx = 0, ty = 0;
                        if (!renderer.windowToMapTile(ev.button.x, ev.button.y, tx, ty)) break;
                        const Vec2i p{tx, ty};

                        if (game.isTargeting()) {
                            if (ev.button.button == SDL_BUTTON_LEFT) {
                                game.setTargetCursor(p);
                                if (recording && (!lastTargetCursorRecorded.has_value() || *lastTargetCursorRecorded != p)) {
                                    recorder.writeTargetCursor(recordTimeMs(), p);
                                    lastTargetCursorRecorded = p;
                                }
                                dispatchAction(Action::Confirm);
                            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                                dispatchAction(Action::Cancel);
                            }
                            break;
                        }

                        if (game.isLooking()) {
                            if (ev.button.button == SDL_BUTTON_LEFT) {
                                game.setLookCursor(p);
                                if (recording && (!lastLookCursorRecorded.has_value() || *lastLookCursorRecorded != p)) {
                                    recorder.writeLookCursor(recordTimeMs(), p);
                                    lastLookCursorRecorded = p;
                                }
                            } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                                dispatchAction(Action::Cancel);
                            }
                            break;
                        }

                        // Normal mode: left-click auto-travels; right-click enters look mode.
                        if (ev.button.button == SDL_BUTTON_LEFT) {
                            if (recording) recorder.writeAutoTravel(recordTimeMs(), p);
                            game.requestAutoTravel(p);
                        } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                            if (recording) recorder.writeBeginLook(recordTimeMs(), p);
                            game.beginLookAt(p);
                        }
                    }
                    break;
            }

            if (!running) break;
        }

        // Replay playback: feed recorded input events based on elapsed wall-clock time.
        if (replayActive) {
            const uint32_t elapsedMs = SDL_GetTicks() - replayStartTicks;
            while (replayIndex < replayFile.events.size() && replayFile.events[replayIndex].tMs <= elapsedMs) {
                dispatchReplayEvent(replayFile.events[replayIndex]);
                if (replayVerifyCtx.failed) break;
                ++replayIndex;
            }

            handleReplayHashFailure();
            if (replayIndex >= replayFile.events.size()) {
                replayActive = false;
                game.pushSystemMessage("REPLAY FINISHED (input unlocked).");
            }
        }

        // Toggle SDL text input for command prompt / message-history search.
        const bool wantTextInput = game.isCommandOpen() || (game.isMessageHistoryOpen() && game.isMessageHistorySearchMode());
        if (wantTextInput && !textInputOn) {
            SDL_StartTextInput();
            textInputOn = true;
        } else if (!wantTextInput && textInputOn) {
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
            ok &= updateIniKey(settingsPath, "auto_explore_search", game.autoExploreSearchEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "identify_items", game.identificationEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "hunger_enabled", game.hungerEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "encumbrance_enabled", game.encumbranceEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "lighting_enabled", game.lightingEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "yendor_doom_enabled", game.yendorDoomEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "confirm_quit", game.confirmQuitEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "auto_mortem", game.autoMortemEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "bones_enabled", game.bonesEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "voxel_sprites", game.voxelSpritesEnabled() ? "true" : "false");
            ok &= updateIniKey(settingsPath, "autosave_every_turns", std::to_string(game.autosaveEveryTurns()));
            ok &= updateIniKey(settingsPath, "save_backups", std::to_string(game.saveBackups()));

            // Only persist the default slot if the user changed it in-game (e.g., via #slot).
            if (game.slotDirty()) {
                ok &= updateIniKey(settingsPath, "default_slot", game.activeSlot());
            }

            ok &= updateIniKey(settingsPath, "player_name", game.playerName());
            ok &= updateIniKey(settingsPath, "player_class", game.playerClassIdString());
            ok &= updateIniKey(settingsPath, "show_effect_timers", game.showEffectTimers() ? "true" : "false");

            auto uiThemeToString = [](UITheme t) -> const char* {
                switch (t) {
                    case UITheme::DarkStone: return "darkstone";
                    case UITheme::Parchment: return "parchment";
                    case UITheme::Arcane:    return "arcane";
                }
                return "darkstone";
            };
            ok &= updateIniKey(settingsPath, "ui_theme", uiThemeToString(game.uiTheme()));
            ok &= updateIniKey(settingsPath, "ui_panels", game.uiPanelsTextured() ? "textured" : "solid");
            ok &= updateIniKey(settingsPath, "view_mode", viewModeId(game.viewMode()));
            ok &= updateIniKey(settingsPath, "control_preset", controlPresetId(game.controlPreset()));

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
            game.setAutoExploreSearchEnabled(newSettings.autoExploreSearch);
            game.setAutosaveEveryTurns(newSettings.autosaveEveryTurns);
            game.setSaveBackups(newSettings.saveBackups);
            game.setIdentificationEnabled(newSettings.identifyItems);
            game.setHungerEnabled(newSettings.hungerEnabled);
            game.setEncumbranceEnabled(newSettings.encumbranceEnabled);
            game.setLightingEnabled(newSettings.lightingEnabled);
            game.setYendorDoomEnabled(newSettings.yendorDoomEnabled);
            game.setConfirmQuitEnabled(newSettings.confirmQuit);
            game.setAutoMortemEnabled(newSettings.autoMortem);
            game.setBonesEnabled(newSettings.bonesEnabled);
            game.setPlayerName(newSettings.playerName);
            game.setShowEffectTimers(newSettings.showEffectTimers);
            game.setUITheme(newSettings.uiTheme);
            game.setUIPanelsTextured(newSettings.uiPanelsTextured);
            game.setViewMode(newSettings.viewMode);
            game.setControlPreset(newSettings.controlPreset);

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
        handleReplayHashFailure();
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

    // Ensure replay recordings are flushed/closed on exit.
    stopRecording();

    if (textInputOn) SDL_StopTextInput();

    closeController();
    renderer.shutdown();
    SDL_Quit();
    return 0;
}
