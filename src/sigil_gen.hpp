#pragma once

// Procedural sigil generation.
//
// Sigils are rare magical floor inscriptions (Engravings beginning with "SIGIL").
// They trigger when stepped on and produce a small, local effect.
//
// This module makes sigils feel *procedural* without introducing any new save
// format requirements:
// - A sigil's parameters are derived deterministically from (run seed, depth,
//   position, archetype keyword).
// - The sigil text can include a generated epithet for flavor, but gameplay is
//   keyed off the first keyword token after "SIGIL" (e.g., "EMBER").

#include "dungeon.hpp" // Vec2i, RoomType
#include "rng.hpp"     // hash32/hashCombine, _tag

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace sigilgen {

// Keep this list modest; each new kind must be implemented in Game::triggerSigilAt.
enum class SigilKind : uint8_t {
    Unknown = 0,
    Seer,
    Nexus,
    Miasma,
    Ember,
    Venom,
    Rust,
    Aegis,
    Regen,
    Lethe,
};

inline const char* keywordForKind(SigilKind k) {
    switch (k) {
        case SigilKind::Seer:   return "SEER";
        case SigilKind::Nexus:  return "NEXUS";
        case SigilKind::Miasma: return "MIASMA";
        case SigilKind::Ember:  return "EMBER";
        case SigilKind::Venom:  return "VENOM";
        case SigilKind::Rust:   return "RUST";
        case SigilKind::Aegis:  return "AEGIS";
        case SigilKind::Regen:  return "REGEN";
        case SigilKind::Lethe:  return "LETHE";
        case SigilKind::Unknown:
        default:
            return "";
    }
}

inline SigilKind kindFromKeyword(const std::string& kwUpper) {
    if (kwUpper == "SEER") return SigilKind::Seer;
    if (kwUpper == "NEXUS") return SigilKind::Nexus;
    if (kwUpper == "MIASMA") return SigilKind::Miasma;
    if (kwUpper == "EMBER") return SigilKind::Ember;
    if (kwUpper == "VENOM") return SigilKind::Venom;
    if (kwUpper == "RUST") return SigilKind::Rust;
    if (kwUpper == "AEGIS") return SigilKind::Aegis;
    if (kwUpper == "REGEN") return SigilKind::Regen;
    if (kwUpper == "LETHE") return SigilKind::Lethe;
    return SigilKind::Unknown;
}

struct SigilSpec {
    SigilKind kind = SigilKind::Unknown;
    uint32_t seed = 0;
    uint8_t uses = 1;       // 1..254 (255 is reserved for permanent graffiti)

    // Tunables; interpretation depends on kind.
    int radius = 0;         // AoE radius (Chebyshev)
    int intensity = 0;      // center intensity for fields (gas/fire)
    int durationTurns = 0;  // status duration
    int param = 0;          // extra param (kind-specific; e.g., amnesia keep radius)

    // Flavor used only for inscription text.
    std::string epithet;
};

inline uint32_t sigilSeed(uint32_t runSeed, int depth, Vec2i pos, SigilKind kind) {
    uint32_t s = hashCombine(runSeed, "SIGIL"_tag);
    s = hashCombine(s, static_cast<uint32_t>(std::max(0, depth)));
    s = hashCombine(s, static_cast<uint32_t>(pos.x));
    s = hashCombine(s, static_cast<uint32_t>(pos.y));
    s = hashCombine(s, static_cast<uint32_t>(kind));
    // Extra salt to decorrelate from other seed domains.
    return hash32(s ^ 0x51C11CEu);
}

// Simple two-word epithet: ADJ NOUN.
inline std::string makeEpithet(uint32_t seed) {
    static constexpr std::array<const char*, 24> kAdj = {{
        "ASHEN", "SILENT", "BROKEN", "HOLLOW", "COLD", "BRIGHT",
        "PALE", "WICKED", "SERRATED", "GILDED", "SCOURGED", "WARPED",
        "BLACK", "WHITE", "RUSTED", "SANGUINE", "VERDANT", "AZURE",
        "VIOLET", "CINNABAR", "IVORY", "OBSIDIAN", "SALT", "IRON",
    }};
    static constexpr std::array<const char*, 24> kNoun = {{
        "LANTERN", "GATE", "EYE", "MOUTH", "KEY", "THREAD",
        "BLADE", "CROWN", "COIL", "MIRROR", "SPIRAL", "CHAIN",
        "ALTAR", "BONE", "VEIL", "RUNE", "SCALE", "HIVE",
        "EMBER", "MIST", "THORN", "FANG", "LOCK", "SHARD",
    }};

    const uint32_t a = hash32(seed ^ 0xA11CEu);
    const uint32_t b = hash32(seed ^ 0xB16B00Bu);
    const char* adj = kAdj[static_cast<size_t>(a % kAdj.size())];
    const char* noun = kNoun[static_cast<size_t>(b % kNoun.size())];

    std::string out;
    out.reserve(32);
    out.append(adj);
    out.push_back(' ');
    out.append(noun);
    return out;
}

inline uint8_t clampUses(int u) {
    return static_cast<uint8_t>(std::clamp(u, 1, 254));
}

inline SigilSpec makeSigil(uint32_t runSeed, int depth, Vec2i pos, const std::string& keywordUpper) {
    SigilSpec s;
    s.kind = kindFromKeyword(keywordUpper);
    if (s.kind == SigilKind::Unknown) return s;

    s.seed = sigilSeed(runSeed, depth, pos, s.kind);
    s.epithet = makeEpithet(s.seed);

    const int d = std::clamp(depth, 0, 30);
    auto h = [&](uint32_t salt) -> uint32_t { return hash32(s.seed ^ salt); };

    switch (s.kind) {
        case SigilKind::Seer: {
            s.radius = 4 + static_cast<int>(h(0x1111u) % 3u); // 4..6
            s.uses = 1;
            // Very occasionally (deeper floors) a sigil holds a second charge.
            if (d >= 10 && (h(0x1112u) % 100u) < 10u) s.uses = 2;
        } break;
        case SigilKind::Nexus: {
            s.uses = 1;
            // Use intensity as a "noise / visual" strength hint.
            s.intensity = 8 + static_cast<int>(h(0x2222u) % 7u); // 8..14
        } break;
        case SigilKind::Miasma: {
            s.radius = 1 + static_cast<int>(h(0x3333u) % 3u); // 1..3
            s.intensity = 10 + static_cast<int>(h(0x3334u) % 9u) + d / 6; // ~10..22
            s.durationTurns = 5 + static_cast<int>(h(0x3335u) % 6u) + d / 10; // ~5..11
            s.uses = clampUses(1 + static_cast<int>(h(0x3336u) % 2u));
        } break;
        case SigilKind::Ember: {
            s.radius = 1 + static_cast<int>(h(0x4444u) % 2u); // 1..2
            s.intensity = 12 + static_cast<int>(h(0x4445u) % 10u) + d / 7; // ~12..26
            s.durationTurns = 5 + static_cast<int>(h(0x4446u) % 6u) + d / 12; // ~5..10
            s.uses = clampUses(1 + static_cast<int>(h(0x4447u) % 2u));
        } break;
        case SigilKind::Venom: {
            s.radius = 1 + static_cast<int>(h(0x5555u) % 3u); // 1..3
            s.intensity = 10 + static_cast<int>(h(0x5556u) % 9u) + d / 7; // ~10..22
            s.durationTurns = 4 + static_cast<int>(h(0x5557u) % 7u) + d / 12; // ~4..11
            s.uses = clampUses(1 + static_cast<int>(h(0x5558u) % 2u));
        } break;
        case SigilKind::Rust: {
            s.radius = 1 + static_cast<int>(h(0x6666u) % 2u); // 1..2
            s.intensity = 10 + static_cast<int>(h(0x6667u) % 9u) + d / 6; // ~10..24
            s.durationTurns = 5 + static_cast<int>(h(0x6668u) % 7u) + d / 10; // ~5..15
            s.uses = clampUses(1 + static_cast<int>(h(0x6669u) % 2u));
        } break;
        case SigilKind::Aegis: {
            // Beneficial: give shield + a brief parry stance.
            s.durationTurns = 7 + static_cast<int>(h(0x7777u) % 10u) + d / 10; // shield
            s.param = 3 + static_cast<int>(h(0x7778u) % 6u); // parry turns
            s.uses = clampUses(1 + static_cast<int>(h(0x7779u) % 2u));
        } break;
        case SigilKind::Regen: {
            s.durationTurns = 9 + static_cast<int>(h(0x8888u) % 10u) + d / 10; // regen
            s.param = 1 + static_cast<int>(h(0x8889u) % 2u); // immediate heal 1..2
            s.uses = 1;
        } break;
        case SigilKind::Lethe: {
            // Harmful to the player: memory wipe (keepRadius param).
            s.param = 2 + static_cast<int>(h(0x9999u) % 7u); // keep radius 2..8
            s.uses = 1;
        } break;
        case SigilKind::Unknown:
        default:
            break;
    }

    // Ensure reserved strength is not used.
    if (s.uses == 255u) s.uses = 1u;
    return s;
}

inline SigilKind pickKindForRoom(uint32_t runSeed, int depth, Vec2i pos, RoomType roomType) {
    // Deterministic per tile, but room-biased.
    uint32_t s = hashCombine(runSeed, "SIGIL_PICK"_tag);
    s = hashCombine(s, static_cast<uint32_t>(std::max(0, depth)));
    s = hashCombine(s, static_cast<uint32_t>(pos.x));
    s = hashCombine(s, static_cast<uint32_t>(pos.y));
    s = hashCombine(s, static_cast<uint32_t>(roomType));
    const uint32_t r = hash32(s ^ 0xC0DEC0DEu) % 1000u;

    // Helper to pick from a simple weighted list.
    struct W { SigilKind k; uint32_t w; };
    auto pickFrom = [&](const std::array<W, 9>& ws) -> SigilKind {
        uint32_t sum = 0;
        for (auto& e : ws) sum += e.w;
        if (sum == 0) return SigilKind::Seer;
        uint32_t t = r % sum;
        for (auto& e : ws) {
            if (t < e.w) return e.k;
            t -= e.w;
        }
        return ws[0].k;
    };

    // Per-room weights (sum doesn't matter; only relative weights).
    switch (roomType) {
        case RoomType::Shrine:
            return pickFrom({{
                {SigilKind::Seer,  340},
                {SigilKind::Aegis, 220},
                {SigilKind::Regen, 140},
                {SigilKind::Nexus, 140},
                {SigilKind::Lethe,  60},
                {SigilKind::Miasma, 40},
                {SigilKind::Ember,  30},
                {SigilKind::Venom,  20},
                {SigilKind::Rust,   10},
            }});
        case RoomType::Library:
            return pickFrom({{
                {SigilKind::Seer,  300},
                {SigilKind::Lethe, 180},
                {SigilKind::Aegis, 160},
                {SigilKind::Regen, 120},
                {SigilKind::Nexus, 120},
                {SigilKind::Miasma, 60},
                {SigilKind::Ember,  30},
                {SigilKind::Venom,  20},
                {SigilKind::Rust,   10},
            }});
        case RoomType::Laboratory:
            return pickFrom({{
                {SigilKind::Miasma, 220},
                {SigilKind::Venom, 200},
                {SigilKind::Rust,  200},
                {SigilKind::Ember, 200},
                {SigilKind::Nexus, 120},
                {SigilKind::Lethe,  40},
                {SigilKind::Seer,   20},
                {SigilKind::Aegis,  0},
                {SigilKind::Regen,  0},
            }});
        case RoomType::Armory:
            return pickFrom({{
                {SigilKind::Ember, 220},
                {SigilKind::Rust,  220},
                {SigilKind::Aegis, 170},
                {SigilKind::Nexus, 150},
                {SigilKind::Seer,  90},
                {SigilKind::Miasma, 70},
                {SigilKind::Venom, 50},
                {SigilKind::Regen, 30},
                {SigilKind::Lethe, 0},
            }});
        case RoomType::Vault:
        case RoomType::Secret:
            return pickFrom({{
                {SigilKind::Nexus, 300},
                {SigilKind::Lethe, 220},
                {SigilKind::Seer,  100},
                {SigilKind::Miasma, 90},
                {SigilKind::Ember,  80},
                {SigilKind::Venom,  70},
                {SigilKind::Rust,   70},
                {SigilKind::Aegis,  50},
                {SigilKind::Regen,  20},
            }});
        default:
            // General case: varied but slightly biased toward utility.
            return pickFrom({{
                {SigilKind::Seer,  190},
                {SigilKind::Nexus, 160},
                {SigilKind::Miasma,140},
                {SigilKind::Ember, 140},
                {SigilKind::Venom, 110},
                {SigilKind::Rust,  90},
                {SigilKind::Aegis, 90},
                {SigilKind::Regen, 50},
                {SigilKind::Lethe, 30},
            }});
    }
}

inline SigilSpec makeSigilForSpawn(uint32_t runSeed, int depth, Vec2i pos, RoomType roomType) {
    const SigilKind k = pickKindForRoom(runSeed, depth, pos, roomType);
    const char* kw = keywordForKind(k);
    return makeSigil(runSeed, depth, pos, kw ? std::string(kw) : std::string());
}

// Convenience for spawn code: returns "<KEYWORD> <EPITHET>" (keyword is required).
inline std::string keywordPlusEpithet(const SigilSpec& s) {
    const char* kw = keywordForKind(s.kind);
    if (!kw || kw[0] == '\0') return std::string();
    std::string out = kw;
    if (!s.epithet.empty()) {
        out.push_back(' ');
        out.append(s.epithet);
    }
    return out;
}

} // namespace sigilgen
