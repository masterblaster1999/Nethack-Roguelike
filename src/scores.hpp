#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Lightweight run-history / high-score tracking.
// Stored as a small CSV-like text file so it works everywhere.

struct ScoreEntry {
    // ISO-ish timestamp: YYYY-MM-DD HH:MM:SS (local time).
    std::string timestamp;

    bool won = false;
    uint32_t score = 0;

    int depth = 1;

    // Which dungeon branch this entry's "depth" refers to.
    // 0 = Camp, 1 = Main dungeon, other values reserved for future branches.
    uint8_t branch = 1;
    uint32_t turns = 0;
    uint32_t kills = 0;
    int level = 1;
    int gold = 0;
    uint32_t seed = 0;

    // Optional metadata (newer versions may record these).
    std::string name;        // player name
    std::string playerClass; // starting class/role (e.g. adventurer, wizard); optional
    std::string slot;        // save slot name ("default" or custom); optional
    std::string cause;       // end-of-run cause ("KILLED BY ...", "ESCAPED ...")
    std::string gameVersion; // e.g. "0.8.0"
};

uint32_t computeScore(const ScoreEntry& e);

class ScoreBoard {
public:
    // Loads entries from disk (if the file doesn't exist, this returns true and leaves entries empty).
    bool load(const std::string& path);

    // Adds an entry, keeps entries sorted by score (desc), and writes to disk.
    bool append(const std::string& path, const ScoreEntry& e);

    const std::vector<ScoreEntry>& entries() const { return entries_; }

    // Convenience: limit in-memory list.
    void trim(size_t maxEntries);

private:
    std::vector<ScoreEntry> entries_;
};
