#pragma once

// Procedural identification appearance labels.
//
// The game already randomizes the mapping of identifiable item kinds
// (potions/scrolls/rings/wands) -> appearance ids per run (NetHack-style).
//
// This module upgrades the *string labels* of those appearances so they are
// also procedurally generated in a deterministic, replay-safe way:
//   - No global RNG stream consumption.
//   - No save format changes (derived from run seed + appearance id).
//   - Stable across platforms.
//
// The generated strings are *purely flavor/UI* and intentionally conservative:
// they keep the base material/gem word so players can still reason about
// identification, while adding per-run variety.

#include "common.hpp"
#include "rng.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace identgen {

inline std::string trimSpaces(std::string s) {
    // Minimal whitespace trim (spaces only) to keep this header self-contained.
    size_t a = 0;
    while (a < s.size() && s[a] == ' ') ++a;
    size_t b = s.size();
    while (b > a && s[b - 1] == ' ') --b;
    if (a == 0 && b == s.size()) return s;
    return s.substr(a, b - a);
}

// Domain-separated deterministic seed.
inline uint32_t appearanceSeed(uint32_t runSeed, uint32_t domainTag, uint8_t appearanceId) {
    uint32_t s = hashCombine(runSeed, domainTag);
    s = hashCombine(s, static_cast<uint32_t>(appearanceId));
    if (s == 0u) s = 1u;
    return s;
}

template <size_t N>
inline const char* pickFrom(const std::array<const char*, N>& arr, RNG& rng) {
    static_assert(N > 0, "pickFrom array must not be empty");
    return arr[static_cast<size_t>(rng.range(0, static_cast<int>(N) - 1))];
}

template <size_t N>
inline const char* pickFromCArray(const char* const (&arr)[N], RNG& rng) {
    static_assert(N > 0, "pickFromCArray array must not be empty");
    return arr[static_cast<size_t>(rng.range(0, static_cast<int>(N) - 1))];
}

inline bool isUpperWordChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

inline std::string join2(const char* a, const char* b) {
    std::string out;
    if (a && a[0]) {
        out += a;
        if (b && b[0]) out += ' ';
    }
    if (b && b[0]) out += b;
    return out;
}

// Clamp appearance labels to a reasonable max so inventory rows remain readable.
inline void clampLabelLength(std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return;
    s.resize(maxLen);
    // Avoid trailing space.
    while (!s.empty() && s.back() == ' ') s.pop_back();
}

// -----------------------------------------------------------------------------
// Potion appearances
// -----------------------------------------------------------------------------

inline std::string potionLabel(uint32_t runSeed, uint8_t appearanceId, const char* base) {
    // Keep the base gem/color so the player's identification notes still make sense.
    static constexpr std::array<const char*, 16> PREFIX = {
        "BUBBLING", "FIZZY", "SMOKY", "SHIMMERING",
        "GLOWING", "OILY", "THICK", "CLEAR",
        "SWEET", "BITTER", "SPARKLING", "MURKY",
        "CHILLED", "WARM", "DUSTY", "" // allow no prefix
    };

    RNG rng(appearanceSeed(runSeed, "POTION_APP"_tag, appearanceId));
    const char* p = pickFrom(PREFIX, rng);
    // Avoid duplicate like "MURKY MURKY".
    if (p && base && p[0] && base[0] && std::string(p) == std::string(base)) {
        // Pick a different prefix deterministically.
        rng.nextU32();
        p = pickFrom(PREFIX, rng);
        if (p && base && p[0] && base[0] && std::string(p) == std::string(base)) p = "";
    }

    std::string out = join2(p, base);
    clampLabelLength(out, 22);
    return out;
}

// -----------------------------------------------------------------------------
// Ring appearances
// -----------------------------------------------------------------------------

inline std::string ringLabel(uint32_t runSeed, uint8_t appearanceId, const char* base) {
    static constexpr std::array<const char*, 16> PREFIX = {
        "PLAIN", "ENGRAVED", "ETCHED", "FILIGREED",
        "INLAID", "TWISTED", "RUNED", "SPIKED",
        "SMOOTH", "DULL", "POLISHED", "GILDED",
        "TARNISHED", "BENT", "ANCIENT", "" // allow no prefix
    };

    RNG rng(appearanceSeed(runSeed, "RING_APP"_tag, appearanceId));
    const char* p = pickFrom(PREFIX, rng);
    if (p && base && p[0] && base[0] && std::string(p) == std::string(base)) p = "";

    std::string out = join2(p, base);
    clampLabelLength(out, 22);
    return out;
}

// -----------------------------------------------------------------------------
// Wand appearances
// -----------------------------------------------------------------------------

inline std::string wandLabel(uint32_t runSeed, uint8_t appearanceId, const char* base) {
    static constexpr std::array<const char*, 16> PREFIX = {
        "CARVED", "KNOTTED", "POLISHED", "CRACKED",
        "BENT", "RUNIC", "BURNT", "SMOOTH",
        "SPIRAL", "WARPED", "LACQUERED", "SPLINTERED",
        "WEATHERED", "SLEEK", "CHARRED", "" // allow no prefix
    };

    RNG rng(appearanceSeed(runSeed, "WAND_APP"_tag, appearanceId));
    const char* p = pickFrom(PREFIX, rng);
    if (p && base && p[0] && base[0] && std::string(p) == std::string(base)) p = "";

    std::string out = join2(p, base);
    clampLabelLength(out, 22);
    return out;
}

// -----------------------------------------------------------------------------
// Scroll appearances
// -----------------------------------------------------------------------------

template <size_t N>
inline std::string scrollLabel(uint32_t runSeed, uint8_t appearanceId, const char* const (&wordBank)[N]) {
    static_assert(N > 0, "scrollLabel word bank must not be empty");

    RNG rng(appearanceSeed(runSeed, "SCROLL_APP"_tag, appearanceId));

    // The first word stays anchored to the base appearance id.
    const char* w1 = wordBank[static_cast<size_t>(appearanceId % static_cast<uint8_t>(N))];

    // Usually two words, rarely three.
    const int nWords = (rng.range(0, 99) < 18) ? 3 : 2;
    const char* w2 = pickFromCArray(wordBank, rng);
    // Avoid duplicates.
    for (int tries = 0; tries < 4 && w2 && w1 && std::string(w2) == std::string(w1); ++tries) {
        rng.nextU32();
        w2 = pickFromCArray(wordBank, rng);
    }
    if (w2 && w1 && std::string(w2) == std::string(w1)) {
        // Deterministic fallback.
        w2 = wordBank[(static_cast<size_t>(appearanceId) + 5u) % N];
    }

    std::string out;
    out.reserve(32);
    out += (w1 ? w1 : "");
    out += ' ';
    out += (w2 ? w2 : "");

    if (nWords == 3) {
        const char* w3 = pickFromCArray(wordBank, rng);
        for (int tries = 0; tries < 6; ++tries) {
            if (!w3) break;
            const std::string s3 = w3;
            if (w1 && s3 == w1) { rng.nextU32(); w3 = pickFromCArray(wordBank, rng); continue; }
            if (w2 && s3 == w2) { rng.nextU32(); w3 = pickFromCArray(wordBank, rng); continue; }
            break;
        }
        if (w3) {
            const std::string s3 = w3;
            if ((!w1 || s3 != w1) && (!w2 || s3 != w2)) {
                out += ' ';
                out += w3;
            }
        }
    }

    // Sanity: ensure label contains only safe characters for quoting.
    // (unknownDisplayName wraps scroll labels in single quotes.)
    for (char& c : out) {
        if (c == '\'' || c == '"') c = ' ';
        if (c == '-') c = ' ';
        if (c == '_') c = ' ';
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        // Keep ASCII uppercase + spaces.
        if (c != ' ' && !isUpperWordChar(c)) c = ' ';
    }

    // Collapse double spaces.
    for (;;) {
        const size_t pos = out.find("  ");
        if (pos == std::string::npos) break;
        out.erase(pos, 1);
    }
    out = trimSpaces(out);
    clampLabelLength(out, 26);
    return out;
}

} // namespace identgen
