#pragma once

#include "common.hpp"
#include "game.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// ------------------------------------------------------------
// Replay recording + playback
// ------------------------------------------------------------
//
// Design goals:
//  - No SDL dependency (logic/test friendly).
//  - Human-readable, line-based file format.
//  - Robust to keybind changes (records Actions, not raw keys).
//  - Stores gameplay-relevant settings to improve determinism.
//
// File format (v1):
//
//   @procrogue_replay 1
//   @game_version 0.21.0
//   @seed 123456
//   @class adventurer
//   @auto_pickup 3
//   @auto_step_delay_ms 45
//   @identify_items 1
//   @hunger_enabled 0
//   @encumbrance_enabled 0
//   @lighting_enabled 0
//   @yendor_doom_enabled 1
//   @bones_enabled 1
//   @end_header
//
//   <ms> A <action_id>
//   <ms> H <turn> <hash64hex>
//   <ms> TI <hex-encoded-utf8>
//   <ms> CB
//   <ms> CA
//   <ms> HB
//   <ms> HS
//   <ms> HC
//   <ms> TR <x> <y>
//   <ms> BL <x> <y>
//   <ms> TC <x> <y>
//   <ms> LC <x> <y>

enum class ReplayEventType : uint8_t {
    Action = 0,
    StateHash, // per-turn deterministic game state hash
    TextInput,
    CommandBackspace,
    CommandAutocomplete,
    MessageHistoryBackspace,
    MessageHistoryToggleSearch,
    MessageHistoryClearSearch,
    AutoTravel,
    BeginLook,
    TargetCursor,
    LookCursor,
};

struct ReplayMeta {
    int formatVersion = 1;
    std::string gameVersion;
    uint32_t seed = 0;
    std::string playerClassId; // "adventurer", "wizard", ... (same tokens as settings/CLI)

    // Gameplay-affecting settings snapshot.
    AutoPickupMode autoPickup = AutoPickupMode::Off;
    int autoStepDelayMs = 45;
    bool autoExploreSearch = false;
    bool identifyItems = true;
    bool hungerEnabled = false;
    bool encumbranceEnabled = false;
    bool lightingEnabled = false;
    bool yendorDoomEnabled = true;
    bool bonesEnabled = true;
};

struct ReplayEvent {
    uint32_t tMs = 0;
    ReplayEventType kind = ReplayEventType::Action;

    // Used by StateHash events (H): deterministic hash checkpoint at a specific turn.
    uint32_t turn = 0;
    uint64_t hash = 0;

    // Payload (only one is used depending on kind).
    Action action = Action::None;
    std::string text; // For TextInput (UTF-8)
    Vec2i pos{};      // For cursor/travel/look events
};

struct ReplayFile {
    ReplayMeta meta;
    std::vector<ReplayEvent> events;
};

// Hex encoding helpers (used for text input lines).
std::string replayHexEncode(const std::string& bytes);
bool replayHexDecode(const std::string& hex, std::string& outBytes);

// ------------------------------------------------------------
// Writer (streaming)
// ------------------------------------------------------------
class ReplayWriter {
public:
    bool open(const std::filesystem::path& path, const ReplayMeta& meta, std::string* err = nullptr);
    void close();
    bool isOpen() const { return static_cast<bool>(f_); }
    std::filesystem::path path() const { return path_; }

    void writeAction(uint32_t tMs, Action a);
    void writeStateHash(uint32_t tMs, uint32_t turn, uint64_t hash);
    void writeTextInput(uint32_t tMs, const std::string& utf8);
    void writeCommandBackspace(uint32_t tMs);
    void writeCommandAutocomplete(uint32_t tMs);
    void writeMessageHistoryBackspace(uint32_t tMs);
    void writeMessageHistoryToggleSearchMode(uint32_t tMs);
    void writeMessageHistoryClearSearch(uint32_t tMs);

    // Shorter aliases.
    void writeHistoryBackspace(uint32_t tMs) { writeMessageHistoryBackspace(tMs); }
    void writeHistoryToggleSearch(uint32_t tMs) { writeMessageHistoryToggleSearchMode(tMs); }
    void writeHistoryClearSearch(uint32_t tMs) { writeMessageHistoryClearSearch(tMs); }
    void writeAutoTravel(uint32_t tMs, Vec2i p);
    void writeBeginLook(uint32_t tMs, Vec2i p);
    void writeTargetCursor(uint32_t tMs, Vec2i p);
    void writeLookCursor(uint32_t tMs, Vec2i p);

private:
    void writeLine_(const std::string& line);

    std::filesystem::path path_;
    std::ofstream f_;
};

// ------------------------------------------------------------
// Reader (loads all events)
// ------------------------------------------------------------
bool loadReplayFile(const std::filesystem::path& path, ReplayFile& out, std::string* err = nullptr);
