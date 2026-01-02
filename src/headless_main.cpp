#include "replay.hpp"
#include "replay_runner.hpp"
#include "content.hpp"
#include "version.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

static void printUsage(const char* argv0) {
    std::cout
        << "Usage:\n"
        << "  " << argv0 << " --replay <file.prr> [options]\n\n"
        << "Options:\n"
        << "  --replay <path>         Replay file to verify/play headlessly.\n"
        << "  --content <path>        Optional content override INI to load.\n"
        << "  --frame-ms <n>          Fixed simulation step in milliseconds (1..100). Default: 16.\n"
        << "  --no-verify-hashes      Do not verify StateHash checkpoints, even if present.\n"
        << "  --max-ms <n>            Safety cap for simulated time in ms (0 = auto).\n"
        << "  --max-frames <n>        Safety cap for frames (0 = auto).\n"
        << "  --version               Print version.\n"
        << "  --help                  Show this help.\n";
}

static bool argValue(int& i, int argc, char** argv, std::string& out) {
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
}

static bool parseU32(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path replayPath;
    std::filesystem::path contentPath;
    bool verify = true;
    uint32_t frameMs = 16;
    uint32_t maxMs = 0;
    uint32_t maxFrames = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--version" || a == "-v") {
            std::cout << PROCROGUE_APPNAME << " " << PROCROGUE_VERSION << "\n";
            return 0;
        } else if (a == "--replay") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--replay requires a path\n";
                return 2;
            }
            replayPath = v;
        } else if (a == "--content") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--content requires a path\n";
                return 2;
            }
            contentPath = v;
        } else if (a == "--frame-ms") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--frame-ms requires a value\n";
                return 2;
            }
            uint32_t n = 0;
            if (!parseU32(v, n)) {
                std::cerr << "Invalid --frame-ms: " << v << "\n";
                return 2;
            }
            frameMs = n;
        } else if (a == "--max-ms") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--max-ms requires a value\n";
                return 2;
            }
            uint32_t n = 0;
            if (!parseU32(v, n)) {
                std::cerr << "Invalid --max-ms: " << v << "\n";
                return 2;
            }
            maxMs = n;
        } else if (a == "--max-frames") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--max-frames requires a value\n";
                return 2;
            }
            uint32_t n = 0;
            if (!parseU32(v, n)) {
                std::cerr << "Invalid --max-frames: " << v << "\n";
                return 2;
            }
            maxFrames = n;
        } else if (a == "--no-verify-hashes") {
            verify = false;
        } else {
            std::cerr << "Unknown arg: " << a << "\n";
            printUsage(argv[0]);
            return 2;
        }
    }

    if (replayPath.empty()) {
        std::cerr << "Missing --replay <file>\n";
        printUsage(argv[0]);
        return 2;
    }

    // Load replay file.
    ReplayFile rf;
    std::string err;
    if (!loadReplayFile(replayPath, rf, &err)) {
        std::cerr << err << "\n";
        return 1;
    }

    // Optional content overrides (same mechanism as the main game).
    if (!contentPath.empty()) {
        ContentOverrides co;
        std::string warns;
        if (!loadContentOverridesIni(contentPath.string(), co, &warns)) {
            std::cerr << "Failed to load content overrides: " << contentPath.string() << "\n";
            if (!warns.empty()) std::cerr << warns;
            return 1;
        }
        setContentOverrides(std::move(co));
        if (!warns.empty()) {
            std::cout << warns;
        }
    }

    Game game;
    if (!prepareGameForReplay(game, rf, &err)) {
        if (!err.empty()) std::cerr << err << "\n";
        return 1;
    }

    ReplayRunOptions opt;
    opt.frameMs = frameMs;
    opt.verifyHashes = verify;
    opt.maxSimMs = maxMs;
    opt.maxFrames = maxFrames;

    ReplayRunStats stats;
    if (!runReplayHeadless(game, rf, opt, &stats, &err)) {
        std::cerr << "Replay failed: " << err << "\n";
        return 1;
    }

    std::cout << "Replay OK: turns=" << stats.turns
              << " events=" << stats.eventsDispatched
              << " simMs=" << stats.simulatedMs
              << " frames=" << stats.frames
              << "\n";

    return 0;
}
