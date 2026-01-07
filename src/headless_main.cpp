#include "replay.hpp"
#include "replay_runner.hpp"
#include "content.hpp"
#include "version.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

static void printUsage(const char* argv0) {
    std::cout
        << "Usage:\n"
        << "  " << argv0 << " --replay <file.prr> [options]\n"
        << "  " << argv0 << " --replay-dir <dir> [options]\n\n"
        << "Options:\n"
        << "  --replay <path>         Replay file to verify/play headlessly.\n"
        << "  --replay-dir <path>     Verify all .prr files in a directory (non-recursive).\n"
        << "  --stop-after-first-fail Stop after the first failing replay in --replay-dir mode.\n"
        << "  --content <path>        Optional content override INI to load.\n"
        << "  --frame-ms <n>          Fixed simulation step in milliseconds (1..100). Default: 16.\n"
        << "  --no-verify-hashes      Do not verify StateHash checkpoints, even if present.\n"
        << "  --max-ms <n>            Safety cap for simulated time in ms (0 = auto).\n"
        << "  --max-frames <n>        Safety cap for frames (0 = auto).\n"
        << "  --trim-on-fail <path>   If a single replay fails due to hash mismatch, write a trimmed replay.\n"
        << "  --trim-dir <path>       In --replay-dir mode, write trimmed failing replays into this directory.\n"
        << "  --json-report <path>    Write a JSON summary report (useful for CI).\n"
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

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

static std::string hex64(uint64_t v) {
    std::ostringstream ss;
    ss << "0x" << std::hex << v;
    return ss.str();
}

static std::vector<std::filesystem::path> listReplayFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return out;
    }

    for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!ent.is_regular_file(ec)) continue;
        const std::filesystem::path p = ent.path();
        if (p.extension() == ".prr") {
            out.push_back(p);
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.generic_string() < b.generic_string();
    });
    return out;
}

static bool buildTrimmedReplay(const ReplayFile& src, uint32_t checkpointTurn, ReplayFile& out, std::string* err) {
    out.meta = src.meta;
    out.events.clear();

    // Find the timestamp of the last checkpoint at or before checkpointTurn.
    bool found = false;
    uint32_t cutMs = 0;
    for (const auto& ev : src.events) {
        if (ev.kind != ReplayEventType::StateHash) continue;
        if (ev.turn > checkpointTurn) continue;
        found = true;
        cutMs = ev.tMs;
    }

    if (!found) {
        if (err) {
            std::ostringstream ss;
            ss << "Replay contains no StateHash checkpoints <= turn " << checkpointTurn;
            *err = ss.str();
        }
        return false;
    }

    out.events.reserve(src.events.size());
    for (const auto& ev : src.events) {
        if (ev.tMs > cutMs) break;
        if (ev.kind == ReplayEventType::StateHash && ev.turn > checkpointTurn) continue;
        out.events.push_back(ev);
    }
    return true;
}

static bool writeReplayFile(const std::filesystem::path& outPath, const ReplayFile& rf, std::string* err) {
    ReplayWriter w;
    if (!w.open(outPath, rf.meta, err)) return false;

    for (const auto& ev : rf.events) {
        switch (ev.kind) {
            case ReplayEventType::Action:
                w.writeAction(ev.tMs, ev.action);
                break;
            case ReplayEventType::StateHash:
                w.writeStateHash(ev.tMs, ev.turn, ev.hash);
                break;
            case ReplayEventType::TextInput:
                w.writeTextInput(ev.tMs, ev.text);
                break;
            case ReplayEventType::CommandBackspace:
                w.writeCommandBackspace(ev.tMs);
                break;
            case ReplayEventType::CommandAutocomplete:
                w.writeCommandAutocomplete(ev.tMs);
                break;
            case ReplayEventType::MessageHistoryBackspace:
                w.writeMessageHistoryBackspace(ev.tMs);
                break;
            case ReplayEventType::MessageHistoryToggleSearch:
                w.writeMessageHistoryToggleSearchMode(ev.tMs);
                break;
            case ReplayEventType::MessageHistoryClearSearch:
                w.writeMessageHistoryClearSearch(ev.tMs);
                break;
            case ReplayEventType::AutoTravel:
                w.writeAutoTravel(ev.tMs, ev.pos);
                break;
            case ReplayEventType::BeginLook:
                w.writeBeginLook(ev.tMs, ev.pos);
                break;
            case ReplayEventType::TargetCursor:
                w.writeTargetCursor(ev.tMs, ev.pos);
                break;
            case ReplayEventType::LookCursor:
                w.writeLookCursor(ev.tMs, ev.pos);
                break;
        }
    }

    w.close();
    return true;
}

struct ReplayRunResult {
    std::filesystem::path file;
    bool ok = false;
    ReplayRunStats stats;
    std::string error;
    std::filesystem::path trimmedPath;
};

static bool writeJsonReport(const std::filesystem::path& path,
                            const std::vector<ReplayRunResult>& results,
                            const ReplayRunOptions& opt,
                            bool verifyHashes,
                            std::string* err) {
    std::ofstream f(path);
    if (!f) {
        if (err) *err = "Failed to open JSON report for writing: " + path.generic_string();
        return false;
    }

    size_t okCount = 0;
    for (const auto& r : results) if (r.ok) ++okCount;

    f << "{\n";
    f << "  \"tool\": \"ProcRogueHeadless\",\n";
    f << "  \"gameVersion\": \"" << jsonEscape(PROCROGUE_VERSION) << "\",\n";
    f << "  \"options\": {\n";
    f << "    \"frameMs\": " << opt.frameMs << ",\n";
    f << "    \"verifyHashes\": " << (verifyHashes ? "true" : "false") << ",\n";
    f << "    \"maxSimMs\": " << opt.maxSimMs << ",\n";
    f << "    \"maxFrames\": " << opt.maxFrames << "\n";
    f << "  },\n";
    f << "  \"summary\": {\n";
    f << "    \"total\": " << results.size() << ",\n";
    f << "    \"ok\": " << okCount << ",\n";
    f << "    \"failed\": " << (results.size() - okCount) << "\n";
    f << "  },\n";
    f << "  \"results\": [\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        f << "    {\n";
        f << "      \"file\": \"" << jsonEscape(r.file.generic_string()) << "\",\n";
        f << "      \"ok\": " << (r.ok ? "true" : "false") << ",\n";
        f << "      \"turns\": " << r.stats.turns << ",\n";
        f << "      \"eventsDispatched\": " << r.stats.eventsDispatched << ",\n";
        f << "      \"simulatedMs\": " << r.stats.simulatedMs << ",\n";
        f << "      \"frames\": " << r.stats.frames;

        if (!r.ok) {
            f << ",\n";
            f << "      \"failure\": \"" << jsonEscape(replayFailureKindName(r.stats.failure)) << "\",\n";
            f << "      \"error\": \"" << jsonEscape(r.error) << "\"";
            if (r.stats.failure == ReplayFailureKind::HashMismatch) {
                f << ",\n";
                f << "      \"failedTurn\": " << r.stats.failedTurn << ",\n";
                f << "      \"failedCheckpointTurn\": " << r.stats.failedCheckpointTurn << ",\n";
                f << "      \"expectedHash\": \"" << jsonEscape(hex64(r.stats.expectedHash)) << "\",\n";
                f << "      \"gotHash\": \"" << jsonEscape(hex64(r.stats.gotHash)) << "\"";
            }
            if (!r.trimmedPath.empty()) {
                f << ",\n";
                f << "      \"trimmedReplay\": \"" << jsonEscape(r.trimmedPath.generic_string()) << "\"";
            }
            f << "\n";
        } else {
            f << "\n";
        }

        f << "    }";
        if (i + 1 < results.size()) f << ",";
        f << "\n";
    }

    f << "  ]\n";
    f << "}\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path replayPath;
    std::filesystem::path replayDir;
    std::filesystem::path contentPath;
    std::filesystem::path trimOnFailPath;
    std::filesystem::path trimDir;
    std::filesystem::path jsonReport;
    bool stopAfterFirstFail = false;
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
        } else if (a == "--replay-dir") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--replay-dir requires a path\n";
                return 2;
            }
            replayDir = v;
        } else if (a == "--stop-after-first-fail") {
            stopAfterFirstFail = true;
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
        } else if (a == "--trim-on-fail") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--trim-on-fail requires a path\n";
                return 2;
            }
            trimOnFailPath = v;
        } else if (a == "--trim-dir") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--trim-dir requires a path\n";
                return 2;
            }
            trimDir = v;
        } else if (a == "--json-report") {
            std::string v;
            if (!argValue(i, argc, argv, v)) {
                std::cerr << "--json-report requires a path\n";
                return 2;
            }
            jsonReport = v;
        } else {
            std::cerr << "Unknown arg: " << a << "\n";
            printUsage(argv[0]);
            return 2;
        }
    }

    if (!replayPath.empty() && !replayDir.empty()) {
        std::cerr << "Specify only one of --replay or --replay-dir\n";
        return 2;
    }
    if (replayPath.empty() && replayDir.empty()) {
        std::cerr << "Missing --replay <file> or --replay-dir <dir>\n";
        printUsage(argv[0]);
        return 2;
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

    ReplayRunOptions opt;
    opt.frameMs = frameMs;
    opt.verifyHashes = verify;
    opt.maxSimMs = maxMs;
    opt.maxFrames = maxFrames;

    std::vector<ReplayRunResult> results;

    auto runOne = [&](const std::filesystem::path& p) -> ReplayRunResult {
        ReplayRunResult rr;
        rr.file = p;

        ReplayFile rf;
        std::string err;
        if (!loadReplayFile(p, rf, &err)) {
            rr.ok = false;
            rr.error = err;
            rr.stats.failure = ReplayFailureKind::Unknown;
            return rr;
        }

        Game game;
        if (!prepareGameForReplay(game, rf, &err)) {
            rr.ok = false;
            rr.error = err.empty() ? "prepareGameForReplay failed" : err;
            rr.stats.failure = ReplayFailureKind::Unknown;
            return rr;
        }

        ReplayRunStats stats;
        const bool ok = runReplayHeadless(game, rf, opt, &stats, &err);
        rr.ok = ok;
        rr.stats = stats;
        rr.error = err;
        rr.stats.failure = ok ? ReplayFailureKind::None : stats.failure;

        // Optional: trim failing replay to the first failing checkpoint.
        if (!ok && stats.failure == ReplayFailureKind::HashMismatch) {
            std::filesystem::path outPath;
            if (!trimOnFailPath.empty() && p == replayPath) {
                outPath = trimOnFailPath;
            } else if (!trimDir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(trimDir, ec);
                const std::string stem = p.stem().string();
                outPath = trimDir / (stem + ".trim" + p.extension().string());
            }

            if (!outPath.empty()) {
                ReplayFile trimmed;
                std::string terr;
                if (buildTrimmedReplay(rf, stats.failedCheckpointTurn, trimmed, &terr) &&
                    writeReplayFile(outPath, trimmed, &terr)) {
                    rr.trimmedPath = outPath;
                } else {
                    // Don't fail the run just because trimming failed.
                    std::cerr << "Trim failed for " << p.generic_string() << ": " << terr << "\n";
                }
            }
        }

        return rr;
    };

    if (!replayPath.empty()) {
        ReplayRunResult rr = runOne(replayPath);
        results.push_back(rr);

        if (rr.ok) {
            std::cout << "Replay OK: " << replayPath.generic_string()
                      << " turns=" << rr.stats.turns
                      << " events=" << rr.stats.eventsDispatched
                      << " simMs=" << rr.stats.simulatedMs
                      << " frames=" << rr.stats.frames
                      << "\n";
        } else {
            std::cout << "Replay FAILED: " << replayPath.generic_string() << "\n";
            std::cout << "  " << rr.error << "\n";
            if (!rr.trimmedPath.empty()) {
                std::cout << "  Trimmed replay written: " << rr.trimmedPath.generic_string() << "\n";
            }
        }

        if (!jsonReport.empty()) {
            std::string jerr;
            if (!writeJsonReport(jsonReport, results, opt, verify, &jerr)) {
                std::cerr << jerr << "\n";
            }
        }

        return rr.ok ? 0 : 1;
    }

    // Directory mode.
    const auto files = listReplayFiles(replayDir);
    if (files.empty()) {
        std::cerr << "No .prr replays found in: " << replayDir.generic_string() << "\n";
        return 2;
    }

    size_t okCount = 0;
    for (const auto& p : files) {
        ReplayRunResult rr = runOne(p);
        results.push_back(rr);

        if (rr.ok) {
            ++okCount;
            std::cout << "OK   " << p.filename().generic_string()
                      << " turns=" << rr.stats.turns
                      << " events=" << rr.stats.eventsDispatched
                      << "\n";
        } else {
            std::cout << "FAIL " << p.filename().generic_string()
                      << "  " << rr.error << "\n";
            if (!rr.trimmedPath.empty()) {
                std::cout << "     trimmed: " << rr.trimmedPath.filename().generic_string() << "\n";
            }
            if (stopAfterFirstFail) break;
        }
    }

    const size_t total = results.size();
    const size_t failed = total - okCount;
    std::cout << "Summary: total=" << total << " ok=" << okCount << " failed=" << failed << "\n";

    if (!jsonReport.empty()) {
        std::string jerr;
        if (!writeJsonReport(jsonReport, results, opt, verify, &jerr)) {
            std::cerr << jerr << "\n";
        }
    }

    return (failed == 0) ? 0 : 1;
}
