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

struct ReplayRunStats {
    uint32_t simulatedMs = 0;
    uint32_t frames = 0;
    uint32_t eventsDispatched = 0;
    uint32_t turns = 0;
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
