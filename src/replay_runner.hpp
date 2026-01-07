#pragma once

#include "replay.hpp"

#include <cstdint>
#include <string>

// Headless replay runner: drives the simulation using the recorded input stream
// and (optionally) validates deterministic state-hash checkpoints.
//
// This is useful for CI/regression testing and for diagnosing desyncs without
// needing SDL2 or a renderer.

struct ReplayRunOptions {
    // Fixed "frame" step used for update() when simulating wall-clock time.
    // Must be in [1, 100] to match the game's dt clamp behavior (0.1s).
    uint32_t frameMs = 16;

    // If true and the replay contains StateHash events, validate them.
    bool verifyHashes = true;

    // Optional safety limits (0 = unlimited).
    uint32_t maxSimMs = 0;
    uint32_t maxFrames = 0;
};

// If a replay run fails, we categorize the failure for tooling/CI purposes.
//
// This enum is intentionally small and stable; new categories should be appended.
enum class ReplayFailureKind : uint8_t {
    None = 0,
    HashMismatch,
    SafetyLimit,
    Unknown,
};

inline const char* replayFailureKindName(ReplayFailureKind k) {
    switch (k) {
        case ReplayFailureKind::None:         return "None";
        case ReplayFailureKind::HashMismatch: return "HashMismatch";
        case ReplayFailureKind::SafetyLimit:  return "SafetyLimit";
        case ReplayFailureKind::Unknown:      return "Unknown";
    }
    return "Unknown";
}

struct ReplayRunStats {
    uint32_t simulatedMs = 0;
    uint32_t frames = 0;
    uint32_t eventsDispatched = 0;
    uint32_t turns = 0;

    // Filled if the run fails (best-effort). Tools should not rely on the
    // presence of these fields unless the run returned false.
    ReplayFailureKind failure = ReplayFailureKind::None;

    // HashMismatch details.
    //  - failedTurn: the current game turn when the verifier noticed the problem.
    //  - failedCheckpointTurn: the checkpoint turn that was expected (may be < failedTurn
    //    in "missed checkpoint" cases).
    uint32_t failedTurn = 0;
    uint32_t failedCheckpointTurn = 0;
    uint64_t expectedHash = 0;
    uint64_t gotHash = 0;
};

// Configure a fresh Game instance from the replay metadata and start a new run
// with the recorded seed.
//
// This mirrors the main executable's replay mode setup: it disables autosaves,
// mortems, and backups to keep verification non-destructive.
bool prepareGameForReplay(Game& game, const ReplayFile& replay, std::string* err = nullptr);

// Run a replay against an already-initialized game (typically prepared via
// prepareGameForReplay). Returns true on success.
bool runReplayHeadless(Game& game,
                       const ReplayFile& replay,
                       const ReplayRunOptions& opt = {},
                       ReplayRunStats* outStats = nullptr,
                       std::string* err = nullptr);
