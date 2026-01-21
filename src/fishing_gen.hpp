#pragma once

// Procedural fishing generation utilities.
//
// This is intentionally header-only so it can be used by both gameplay code
// and UI/sprite plumbing without adding a new translation unit.
//
// Design goals:
// - Deterministic from a stable per-fish seed.
// - Save-compatible without expanding the save format (fish meta can be packed
//   into existing Item fields by callers).
// - Lightweight "bite cadence" helpers for a future fishing loop.

#include "rng.hpp"
#include "common.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>

namespace fishgen {

// -----------------------------------------------------------------------------
// Bite cadence (future-facing): per-water-tile schedule helpers.
//
// The idea: each water tile has a repeating bite window (like a "pulse") so
// fishing feels responsive and learnable instead of pure RNG spam.
// -----------------------------------------------------------------------------

struct BiteCadence {
    int periodTurns = 40;   // full cycle length
    int windowTurns = 8;    // bite window duration within the cycle
    int phaseOffset = 0;    // phase offset (0..period-1)
};

inline BiteCadence biteCadence(uint32_t waterSeed) {
    // Domain-separated hash so water cadence doesn't correlate with other uses.
    const uint32_t h = hash32(waterSeed ^ 0xB17ECAD1u);
    RNG rng(h);

    BiteCadence bc;
    bc.periodTurns = clampi(rng.range(28, 60), 12, 180);
    bc.windowTurns = clampi(rng.range(5, 12), 2, bc.periodTurns - 1);
    bc.phaseOffset = rng.range(0, bc.periodTurns - 1);
    return bc;
}

inline bool isInBiteWindow(uint32_t waterSeed, int turn) {
    if (turn < 0) return false;
    const BiteCadence bc = biteCadence(waterSeed);
    const int t = (turn + bc.phaseOffset) % bc.periodTurns;
    return t < bc.windowTurns;
}

inline float biteWindow01(uint32_t waterSeed, int turn) {
    // 0..1 "how far into the bite window" (0 outside).
    if (!isInBiteWindow(waterSeed, turn)) return 0.0f;
    const BiteCadence bc = biteCadence(waterSeed);
    const int t = (turn + bc.phaseOffset) % bc.periodTurns;
    return (bc.windowTurns <= 1) ? 1.0f : (static_cast<float>(t) / static_cast<float>(bc.windowTurns - 1));
}

// Generate a deterministic per-cast fish seed (future hook point).
inline uint32_t fishSeedForCast(uint32_t waterSeed, int turn, uint32_t casterSeed) {
    uint32_t h = hash32(waterSeed ^ 0xF15A1234u); // domain sep
    h = hashCombine(h, static_cast<uint32_t>(turn));
    h = hashCombine(h, casterSeed);
    return hash32(h ^ 0xC0FFEEu);
}



// Turns until the next bite window begins (0 if currently in the window).
inline int turnsUntilNextBite(uint32_t waterSeed, int turn) {
    const BiteCadence bc = biteCadence(waterSeed);
    const int period = std::max(1, bc.periodTurns);
    const int t = (turn + bc.phaseOffset) % period;
    if (t < bc.windowTurns) return 0;
    return period - t;
}

// Turns remaining in the current bite window (0 if not currently in the window).
inline int turnsRemainingInBiteWindow(uint32_t waterSeed, int turn) {
    const BiteCadence bc = biteCadence(waterSeed);
    const int period = std::max(1, bc.periodTurns);
    const int t = (turn + bc.phaseOffset) % period;
    if (t >= bc.windowTurns) return 0;
    return std::max(0, bc.windowTurns - t);
}
// -----------------------------------------------------------------------------
// Fish identity
// -----------------------------------------------------------------------------

enum class FishRarity : uint8_t {
    Common = 0,
    Uncommon,
    Rare,
    Epic,
    Legendary,
};

inline FishRarity clampRarityInt(int v) {
    v = clampi(v, 0, 4);
    return static_cast<FishRarity>(static_cast<uint8_t>(v));
}

inline const char* fishRarityName(FishRarity r) {
    switch (r) {
        case FishRarity::Common:    return "COMMON";
        case FishRarity::Uncommon:  return "UNCOMMON";
        case FishRarity::Rare:      return "RARE";
        case FishRarity::Epic:      return "EPIC";
        case FishRarity::Legendary: return "LEGENDARY";
        default:                    return "COMMON";
    }
}

inline FishRarity rollRarity(uint32_t seed) {
    // A conservative rarity curve (tunable later).
    const float r = rand01(hash32(seed ^ 0xA17C0DE1u));
    if (r < 0.60f) return FishRarity::Common;
    if (r < 0.85f) return FishRarity::Uncommon;
    if (r < 0.95f) return FishRarity::Rare;
    if (r < 0.99f) return FishRarity::Epic;
    return FishRarity::Legendary;
}

inline bool rollShiny(uint32_t seed, FishRarity rarity) {
    // Shiny odds increase a bit with rarity.
    uint32_t h = hash32(seed ^ 0x5A1B7001u);
    const int x = static_cast<int>(h % 10000u); // 0..9999

    int denom = 2048; // ~0.05%
    switch (rarity) {
        case FishRarity::Common:    denom = 2048; break;
        case FishRarity::Uncommon:  denom = 1536; break;
        case FishRarity::Rare:      denom = 1024; break;
        case FishRarity::Epic:      denom = 768;  break;
        case FishRarity::Legendary: denom = 512;  break;
        default: break;
    }
    // Convert denom to a threshold in 0..9999.
    const int thr = std::max(1, 10000 / denom);
    return x < thr;
}

struct FishSpec {
    FishRarity rarity = FishRarity::Common;
    bool shiny = false;

    // 0..15 (caller can treat this as size tier; optional override)
    int sizeClass = 0;

    // Tenths of a pound (for short UI strings).
    int weight10 = 10; // 1.0 lb

    int value = 0;
    int hungerRestore = 0;
    int healAmount = 0;

    // Optional flavor tag (future hook for bonuses).
    const char* bonusTag = "";

    // Uppercase display name (procedurally generated).
    std::string name;
};

inline int defaultSizeClassFor(FishRarity r, uint32_t seed) {
    RNG rng(hash32(seed ^ 0x515ECA5Eu));
    switch (r) {
        case FishRarity::Common:    return rng.range(0, 5);
        case FishRarity::Uncommon:  return rng.range(3, 9);
        case FishRarity::Rare:      return rng.range(6, 12);
        case FishRarity::Epic:      return rng.range(9, 14);
        case FishRarity::Legendary: return rng.range(12, 15);
        default:                    return rng.range(0, 7);
    }
}

inline int rollWeight10(FishRarity r, int sizeClass, uint32_t seed) {
    RNG rng(hash32(seed ^ 0x7E16A7B5u));

    // Base weight by rarity, scaled by sizeClass.
    int baseLo = 8, baseHi = 28;
    switch (r) {
        case FishRarity::Common:    baseLo = 6;  baseHi = 24; break;
        case FishRarity::Uncommon:  baseLo = 10; baseHi = 40; break;
        case FishRarity::Rare:      baseLo = 18; baseHi = 70; break;
        case FishRarity::Epic:      baseLo = 30; baseHi = 120; break;
        case FishRarity::Legendary: baseLo = 60; baseHi = 220; break;
        default: break;
    }

    const float t = clampi(sizeClass, 0, 15) / 15.0f;
    const int lo = static_cast<int>(std::lround(baseLo * (0.75f + 0.75f * t)));
    const int hi = static_cast<int>(std::lround(baseHi * (0.75f + 0.75f * t)));
    return clampi(rng.range(std::min(lo, hi), std::max(lo, hi)), 1, 999);
}

inline const char* rollBonusTag(uint32_t seed, FishRarity rarity) {
    // Mostly empty; rarer fish more likely to have a tag.
    const uint32_t h = hash32(seed ^ 0xB0C0512u);
    const int r = static_cast<int>(h % 100);

    int chance = 3;
    switch (rarity) {
        case FishRarity::Common:    chance = 2; break;
        case FishRarity::Uncommon:  chance = 4; break;
        case FishRarity::Rare:      chance = 7; break;
        case FishRarity::Epic:      chance = 12; break;
        case FishRarity::Legendary: chance = 18; break;
        default: break;
    }
    if (r >= chance) return "";

    static constexpr std::array<const char*, 7> TAGS = {
        "REGEN", "HASTE", "SHIELD", "CLARITY", "VENOM", "EMBER", "AURORA",
    };
    return TAGS[(h >> 8) % TAGS.size()];
}

inline std::string fishName(uint32_t seed, FishRarity rarity, bool shiny) {
    // Uppercase, NetHack-ish naming.
    RNG rng(hash32(seed ^ 0xF15A0A8Eu));

    static constexpr std::array<const char*, 28> ADJ_COMMON = {
        "SILVER", "MOTTLED", "SPECKLED", "DUSK", "RIVER", "TIDE", "PALE", "DULL",
        "BRIGHT", "BLUE", "GREEN", "RUST", "SMOKE", "SAND", "COLD", "WARM",
        "SWIFT", "STILL", "SHALLOW", "DEEP", "SLIM", "FAT", "SLICK", "PRICKLY",
        "GENTLE", "FERAL", "ODD", "WARY",
    };
    static constexpr std::array<const char*, 20> ADJ_RARE = {
        "GILDED", "LUMINOUS", "PHANTOM", "ABYSSAL", "STARLIT", "EMBER", "FROST",
        "VOID", "SUNBURN", "MOON", "ARCANE", "RADIANT", "ECHOING", "GLASS",
        "IVORY", "OBSIDIAN", "CELESTIAL", "SABLE", "AURORA", "CRYSTAL",
    };
    static constexpr std::array<const char*, 26> SPECIES = {
        "CARP", "TROUT", "PERCH", "CATFISH", "EEL", "PIKE", "BASS", "MINNOW",
        "SALMON", "HERRING", "SARDINE", "ANCHOVY", "TILAPIA", "STURGEON",
        "SUNFISH", "GUPPY", "LOACH", "KOI", "MUDSKIPPER", "LANTERNFISH",
        "BLOWFISH", "SQUID", "OCTOPUS", "FLOUNDER", "RAY", "GHOSTFISH",
    };

    const bool isRareAdj = (rarity >= FishRarity::Rare);
    const char* adj = isRareAdj ? ADJ_RARE[rng.range(0, static_cast<int>(ADJ_RARE.size()) - 1)]
                                : ADJ_COMMON[rng.range(0, static_cast<int>(ADJ_COMMON.size()) - 1)];
    const char* sp = SPECIES[rng.range(0, static_cast<int>(SPECIES.size()) - 1)];

    // Occasionally prefix a "THE" style title for legendary.
    const bool titled = (rarity == FishRarity::Legendary) && (((rng.nextU32() >> 3) & 1u) != 0u);

    std::string name;
    name.reserve(32);
    if (titled) name += "THE ";

    if (shiny) {
        static constexpr std::array<const char*, 6> SHINY = {
            "SHINY", "PRISMATIC", "IRIDESCENT", "GLITTERING", "PEARLESCENT", "OPALESCENT",
        };
        name += SHINY[rng.range(0, static_cast<int>(SHINY.size()) - 1)];
        name += " ";
    }

    name += adj;
    name += " ";
    name += sp;
    return name;
}

// Make a fish from a seed, optionally overriding its meta.
//
// Parameters:
// - rarityHint: 0..4 to force a tier; -1 to roll from seed
// - sizeHint:   0..15 to force sizeClass; -1 to derive
// - shinyHint:  0/1 to force; -1 to roll
inline FishSpec makeFish(uint32_t seed, int rarityHint = -1, int sizeHint = -1, int shinyHint = -1) {
    FishSpec fs;

    fs.rarity = (rarityHint >= 0) ? clampRarityInt(rarityHint) : rollRarity(seed);
    fs.sizeClass = (sizeHint >= 0) ? clampi(sizeHint, 0, 15) : defaultSizeClassFor(fs.rarity, seed);
    fs.shiny = (shinyHint >= 0) ? (shinyHint != 0) : rollShiny(seed, fs.rarity);

    fs.weight10 = rollWeight10(fs.rarity, fs.sizeClass, seed);

    // Coarse value/hunger formulas (future: shops/food hooks can read these).
    const int rarityMul = 10 + static_cast<int>(fs.rarity) * 12;
    fs.value = clampi((fs.weight10 * rarityMul) / 10, 0, 9999);

    fs.hungerRestore = clampi((fs.weight10 * 6) / 10, 0, 600);

    // Rare fish can have a small heal bonus.
    fs.healAmount = 0;
    if (fs.rarity >= FishRarity::Rare) {
        fs.healAmount = clampi(1 + (fs.weight10 / 40), 0, 12);
    }

    fs.bonusTag = rollBonusTag(seed, fs.rarity);
    fs.name = fishName(seed, fs.rarity, fs.shiny);

    return fs;
}

} // namespace fishgen
