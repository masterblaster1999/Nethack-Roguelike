#pragma once

#include <algorithm>
#include <cctype>
#include <string>

// Small shared helpers for save-slot naming.
//
// We intentionally keep this header SDL-free so it can be used by:
//   - main.cpp (CLI parsing)
//   - settings.cpp (default_slot parsing)
//   - game.cpp (extended commands)
//   - unit tests (PROCROGUE_BUILD_TESTS)
//
// Notes:
// - Slot names are used as a suffix in filenames (procrogue_save_<slot>.dat).
// - We sanitize aggressively to keep saves portable across platforms.

namespace procrogue_slot_detail {

inline std::string trim(std::string s) {
    auto isNotSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), isNotSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), isNotSpace).base(), s.end());
    return s;
}

inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

inline bool isWindowsReservedBasename(const std::string& lower) {
    // Windows device names are invalid as file basenames (even with extensions).
    // Guard against common ones to avoid surprising save-slot failures.
    static const char* reserved[] = {
        "con", "prn", "aux", "nul",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
    };
    for (const char* r : reserved) {
        if (lower == r) return true;
    }
    return false;
}

} // namespace procrogue_slot_detail

inline std::string sanitizeSlotName(std::string raw) {
    using namespace procrogue_slot_detail;

    // Keep only filename-safe characters for a slot name (portable + predictable).
    raw = toLower(trim(std::move(raw)));

    std::string out;
    out.reserve(raw.size());

    for (unsigned char c : raw) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(c));
        } else if (c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else if (std::isspace(c)) {
            out.push_back('_');
        } else {
            // Avoid path separators / dots / other punctuation.
            out.push_back('_');
        }
    }

    // Collapse repeated underscores.
    out.erase(std::unique(out.begin(), out.end(), [](char a, char b) {
        return a == '_' && b == '_';
    }), out.end());

    // Trim underscores/hyphens from ends.
    while (!out.empty() && (out.front() == '_' || out.front() == '-')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '_' || out.back() == '-')) out.pop_back();

    if (out.empty()) out = "slot";
    if (out.size() > 32) out.resize(32);

    if (isWindowsReservedBasename(out)) {
        out = "_" + out;
    }

    return out;
}
