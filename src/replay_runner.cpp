#include "replay_runner.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <vector>

namespace {

struct TurnHashCheckpoint {
    uint32_t turn = 0;
    uint64_t hash = 0;
};

struct TurnHashVerifyCtx {
    const std::vector<TurnHashCheckpoint>* expected = nullptr;
    size_t idx = 0;
    bool failed = false;
    // Current turn when the mismatch is detected.
    uint32_t failedTurn = 0;
    // Checkpoint turn that was expected (may be < failedTurn if we skipped over it).
    uint32_t expectedTurn = 0;
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
        ctx->expectedTurn = (*ctx->expected)[ctx->idx].turn;
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
            ctx->expectedTurn = turn;
            ctx->expectedHash = exp;
            ctx->gotHash = hash;
        }
    }
}

static void formatHashMismatch(const TurnHashVerifyCtx& ctx, std::string& out) {
    std::ostringstream ss;
    if (ctx.expectedTurn != 0 && ctx.expectedTurn != ctx.failedTurn) {
        ss << "REPLAY DESYNC: missed checkpoint turn " << ctx.expectedTurn
           << " while at turn " << ctx.failedTurn
           << " (expected 0x" << std::hex << ctx.expectedHash
           << ", got 0x" << std::hex << ctx.gotHash << ")";
    } else {
        ss << "REPLAY DESYNC at turn " << ctx.failedTurn
           << " (expected 0x" << std::hex << ctx.expectedHash
           << ", got 0x" << std::hex << ctx.gotHash << ")";
    }
    out = ss.str();
}

static uint32_t clampFrameMs(uint32_t v) {
    if (v < 1) return 1;
    if (v > 100) return 100; // match dt clamp in main loop
    return v;
}

static void dispatchReplayEvent(Game& game, const ReplayEvent& rev) {
    switch (rev.kind) {
        case ReplayEventType::StateHash:
            // Hash checkpoints are validated via the per-turn hook.
            break;
        case ReplayEventType::Action:
            game.handleAction(rev.action);
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
}

} // namespace

bool prepareGameForReplay(Game& game, const ReplayFile& replay, std::string* err) {
    (void)err;

    // Keep replays self-contained and non-destructive.
    game.setActiveSlot("__replay__");
    game.setAutosaveEveryTurns(0);
    game.setSaveBackups(0);
    game.setAutoMortemEnabled(false);

    // Apply recorded gameplay-affecting settings for determinism.
    game.setAutoStepDelayMs(replay.meta.autoStepDelayMs);
    game.setAutoPickupMode(replay.meta.autoPickup);
    game.setAutoExploreSearchEnabled(replay.meta.autoExploreSearch);
    game.setIdentificationEnabled(replay.meta.identifyItems);
    game.setHungerEnabled(replay.meta.hungerEnabled);
    game.setEncumbranceEnabled(replay.meta.encumbranceEnabled);
    game.setLightingEnabled(replay.meta.lightingEnabled);
    game.setYendorDoomEnabled(replay.meta.yendorDoomEnabled);
    game.setBonesEnabled(replay.meta.bonesEnabled);

    // Starting class is recorded for determinism.
    PlayerClass pc = PlayerClass::Adventurer;
    if (!replay.meta.playerClassId.empty()) {
        PlayerClass parsed = pc;
        if (parsePlayerClass(replay.meta.playerClassId, parsed)) {
            pc = parsed;
        }
    }
    game.setPlayerClass(pc);

    // Start the run with the recorded seed.
    game.newGame(replay.meta.seed);

    // newGame() may reset some settings; re-apply the intended auto modes.
    game.setAutoPickupMode(replay.meta.autoPickup);
    game.setAutoExploreSearchEnabled(replay.meta.autoExploreSearch);

    return true;
}

bool runReplayHeadless(Game& game,
                       const ReplayFile& replay,
                       const ReplayRunOptions& opt,
                       ReplayRunStats* outStats,
                       std::string* err) {
    if (outStats) *outStats = ReplayRunStats{};

    const uint32_t frameMs = clampFrameMs(opt.frameMs);

    // Collect hash checkpoints (if any).
    std::vector<TurnHashCheckpoint> checkpoints;
    checkpoints.reserve(256);
    for (const auto& ev : replay.events) {
        if (ev.kind == ReplayEventType::StateHash) {
            checkpoints.push_back(TurnHashCheckpoint{ev.turn, ev.hash});
        }
    }
    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const TurnHashCheckpoint& a, const TurnHashCheckpoint& b) { return a.turn < b.turn; });

    TurnHashVerifyCtx verify{};
    if (opt.verifyHashes && !checkpoints.empty()) {
        verify.expected = &checkpoints;
        game.setTurnHook(&onTurnHashVerify, &verify);

        // Validate initial state (turn 0) immediately, if present in the replay.
        onTurnHashVerify(&verify, game.turns(), game.determinismHash());
        if (verify.failed) {
            if (outStats) {
                outStats->failure = ReplayFailureKind::HashMismatch;
                outStats->failedTurn = verify.failedTurn;
                outStats->failedCheckpointTurn = verify.expectedTurn;
                outStats->expectedHash = verify.expectedHash;
                outStats->gotHash = verify.gotHash;
            }
            if (err) formatHashMismatch(verify, *err);
            return false;
        }
    } else {
        game.clearTurnHook();
    }

    // Replay playback state: feed recorded input events based on elapsed simulated time.
    size_t idx = 0;
    uint32_t elapsedMs = 0;
    uint32_t frames = 0;
    uint32_t dispatched = 0;

    uint32_t lastEventMs = 0;
    if (!replay.events.empty()) {
        lastEventMs = replay.events.back().tMs;
    }

    // Safety: unless caller explicitly overrides, cap runtime to avoid infinite loops
    // if something goes wrong (e.g., auto-move stuck). Still large enough for typical
    // replays; callers can set maxSimMs=0 to disable or set a larger value.
    uint32_t maxSimMs = opt.maxSimMs;
    if (maxSimMs == 0) {
        // Default: last event time + 5 seconds (and at least 5 seconds).
        const uint32_t slack = 5000;
        maxSimMs = (lastEventMs > (std::numeric_limits<uint32_t>::max() - slack))
                 ? std::numeric_limits<uint32_t>::max()
                 : (lastEventMs + slack);
        if (maxSimMs < 5000) maxSimMs = 5000;
    }

    const uint32_t maxFrames = (opt.maxFrames == 0) ? (maxSimMs / frameMs + 10) : opt.maxFrames;

    while (frames < maxFrames && elapsedMs <= maxSimMs) {
        // Dispatch all events that are due at this time (mirrors main.cpp behavior).
        while (idx < replay.events.size() && replay.events[idx].tMs <= elapsedMs) {
            dispatchReplayEvent(game, replay.events[idx]);
            ++idx;
            ++dispatched;

            if (verify.failed) {
                if (outStats) {
                    outStats->failure = ReplayFailureKind::HashMismatch;
                    outStats->failedTurn = verify.failedTurn;
                    outStats->failedCheckpointTurn = verify.expectedTurn;
                    outStats->expectedHash = verify.expectedHash;
                    outStats->gotHash = verify.gotHash;
                    outStats->simulatedMs = elapsedMs;
                    outStats->frames = frames;
                    outStats->eventsDispatched = dispatched;
                    outStats->turns = game.turns();
                }
                if (err) formatHashMismatch(verify, *err);
                return false;
            }
        }

        // Done once all replay events were processed AND (if verifying) all checkpoints were consumed.
        const bool allEventsDone = (idx >= replay.events.size());
        const bool hashesDone = (!verify.expected) || (verify.idx >= verify.expected->size());
        if (allEventsDone && hashesDone) {
            break;
        }

        // Advance simulated time by one fixed step.
        uint32_t stepMs = frameMs;
        if (elapsedMs + stepMs > maxSimMs) stepMs = maxSimMs - elapsedMs;
        if (stepMs == 0) break;

        float dt = stepMs / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;

        game.update(dt);

        elapsedMs += stepMs;
        ++frames;

        if (verify.failed) {
            if (outStats) {
                outStats->failure = ReplayFailureKind::HashMismatch;
                outStats->failedTurn = verify.failedTurn;
                outStats->failedCheckpointTurn = verify.expectedTurn;
                outStats->expectedHash = verify.expectedHash;
                outStats->gotHash = verify.gotHash;
                outStats->simulatedMs = elapsedMs;
                outStats->frames = frames;
                outStats->eventsDispatched = dispatched;
                outStats->turns = game.turns();
            }
            if (err) formatHashMismatch(verify, *err);
            return false;
        }
    }

    if (frames >= maxFrames || elapsedMs > maxSimMs) {
        if (outStats) {
            outStats->failure = ReplayFailureKind::SafetyLimit;
            outStats->simulatedMs = elapsedMs;
            outStats->frames = frames;
            outStats->eventsDispatched = dispatched;
            outStats->turns = game.turns();
        }
        if (err) {
            std::ostringstream ss;
            ss << "Replay runner exceeded safety limit (elapsedMs=" << elapsedMs
               << ", frames=" << frames << ", maxSimMs=" << maxSimMs << ").";
            *err = ss.str();
        }
        return false;
    }

    if (outStats) {
        outStats->simulatedMs = elapsedMs;
        outStats->frames = frames;
        outStats->eventsDispatched = dispatched;
        outStats->turns = game.turns();
    }

    return true;
}
