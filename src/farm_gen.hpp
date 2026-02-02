#pragma once

// Procedural farming generation utilities.
//
// Design goals:
// - Deterministic from a stable per-crop / per-plot seed.
// - Header-only and lightweight.
// - Save-compatible: callers can pack farm metadata into existing Item fields
//   (charges/enchant/spriteSeed) without expanding the save format.
//
// This module is intentionally "foundation only": generation helpers, naming,
// and tuning formulas. The gameplay loop (tilling/planting/growth/harvest)
// is wired elsewhere.

#include "rng.hpp"
#include "common.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace farmgen {

// -----------------------------------------------------------------------------
// Tags shared across farming items.
//
// These tags are deliberately short, uppercase, and compatible with the existing
// message/UI style.
// -----------------------------------------------------------------------------

inline constexpr std::array<const char*, 10> FARM_TAGS = {
    "REGEN", "HASTE", "SHIELD", "CLARITY", "VENOM",
    "EMBER", "AURORA", "THORN", "STONE", "LUCK",
};

inline const char* farmTagByIndex(int idx) {
    const int i = clampi(idx, 0, static_cast<int>(FARM_TAGS.size()) - 1);
    return FARM_TAGS[static_cast<size_t>(i)];
}

inline int farmTagIndex(const char* tag) {
    if (!tag || !tag[0]) return -1;
    for (size_t i = 0; i < FARM_TAGS.size(); ++i) {
        const char* t = FARM_TAGS[i];
        if (!t) continue;
        // Very small compare (tags are short).
        bool ok = true;
        for (int j = 0; tag[j] || t[j]; ++j) {
            if (tag[j] != t[j]) { ok = false; break; }
            if (tag[j] == 0 && t[j] == 0) break;
        }
        if (ok) return static_cast<int>(i);
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Soil
// -----------------------------------------------------------------------------

struct SoilSpec {
    // 0..100; higher = faster growth / better yield.
    int fertility = 50;

    // Optional affinity tag that can reward matching crops (future hook).
    const char* affinityTag = "";
};

inline SoilSpec makeSoil(uint32_t soilSeed) {
    // Domain-separated seed.
    const uint32_t h = hash32(soilSeed ^ 0x50D1F00Du);
    RNG rng(h);

    SoilSpec s;
    // Fertility leans toward "okay" with occasional extremes.
    // (Clamp to 0..100 for compact UI and packing.)
    const float r = rng.next01();
    int fert = 0;
    if (r < 0.08f) fert = rng.range(5, 25);
    else if (r < 0.30f) fert = rng.range(25, 45);
    else if (r < 0.78f) fert = rng.range(45, 70);
    else if (r < 0.96f) fert = rng.range(70, 90);
    else fert = rng.range(90, 100);
    s.fertility = clampi(fert, 0, 100);

    // 60% chance to have no special affinity.
    const int roll = rng.range(0, 99);
    if (roll < 40) {
        s.affinityTag = "";
    } else {
        const uint32_t hh = hash32(h ^ 0xAFF1A17Du);
        s.affinityTag = farmTagByIndex(static_cast<int>((hh >> 8) % static_cast<uint32_t>(FARM_TAGS.size())));
    }
    return s;
}

// Stable per-tile soil seed helper.
inline uint32_t soilSeedAt(uint32_t levelSeed, Vec2i pos) {
    uint32_t h = hash32(levelSeed ^ 0xFA245011u);
    h = hashCombine(h, static_cast<uint32_t>(pos.x));
    h = hashCombine(h, static_cast<uint32_t>(pos.y));
    return hash32(h ^ 0xC0FFEEu);
}

// -----------------------------------------------------------------------------
// Crops
// -----------------------------------------------------------------------------

enum class CropRarity : uint8_t {
    Common = 0,
    Uncommon,
    Rare,
    Epic,
    Legendary,
};

inline CropRarity clampRarityInt(int v) {
    v = clampi(v, 0, 4);
    return static_cast<CropRarity>(static_cast<uint8_t>(v));
}

inline const char* cropRarityName(CropRarity r) {
    switch (r) {
        case CropRarity::Common:    return "COMMON";
        case CropRarity::Uncommon:  return "UNCOMMON";
        case CropRarity::Rare:      return "RARE";
        case CropRarity::Epic:      return "EPIC";
        case CropRarity::Legendary: return "LEGENDARY";
        default:                    return "COMMON";
    }
}

inline CropRarity rollRarity(uint32_t seed) {
    // Slightly more generous than fish: farming wants more frequent "interesting" drops.
    const float r = rand01(hash32(seed ^ 0xC20BFA23u));
    if (r < 0.62f) return CropRarity::Common;
    if (r < 0.86f) return CropRarity::Uncommon;
    if (r < 0.95f) return CropRarity::Rare;
    if (r < 0.99f) return CropRarity::Epic;
    return CropRarity::Legendary;
}

inline bool rollShiny(uint32_t seed, CropRarity rarity) {
    // Shiny odds: very rare, slightly better with rarity.
    uint32_t h = hash32(seed ^ 0x5A1B7001u);
    const int x = static_cast<int>(h % 10000u); // 0..9999

    int denom = 2048;
    switch (rarity) {
        case CropRarity::Common:    denom = 2048; break;
        case CropRarity::Uncommon:  denom = 1536; break;
        case CropRarity::Rare:      denom = 1024; break;
        case CropRarity::Epic:      denom = 768;  break;
        case CropRarity::Legendary: denom = 512;  break;
        default: break;
    }

    const int thr = std::max(1, 10000 / denom);
    return x < thr;
}

inline int defaultVariant(uint32_t seed, CropRarity rarity) {
    RNG rng(hash32(seed ^ 0xBADC0DEu));
    // Common crops cluster toward low variants; legendary skew high.
    const int lo = (rarity == CropRarity::Legendary) ? 10 : (rarity >= CropRarity::Epic ? 7 : 0);
    const int hi = (rarity == CropRarity::Common) ? 9 : 15;
    return clampi(rng.range(lo, hi), 0, 15);
}

inline const char* rollBonusTag(uint32_t seed, CropRarity rarity) {
    // Like fish: most crops have no bonus tag; rarer crops more likely.
    const uint32_t h = hash32(seed ^ 0xB0B05C0u);
    const int r = static_cast<int>(h % 100);

    int chance = 4;
    switch (rarity) {
        case CropRarity::Common:    chance = 3; break;
        case CropRarity::Uncommon:  chance = 6; break;
        case CropRarity::Rare:      chance = 10; break;
        case CropRarity::Epic:      chance = 18; break;
        case CropRarity::Legendary: chance = 28; break;
        default: break;
    }

    if (r >= chance) return "";
    return farmTagByIndex(static_cast<int>((h >> 8) % static_cast<uint32_t>(FARM_TAGS.size())));
}

struct CropSpec {
    CropRarity rarity = CropRarity::Common;
    bool shiny = false;

    // 0..15; a small "strain" / "variety" selector.
    int variant = 0;

    // Tuning fields used by future planting/harvesting mechanics.
    int growMinTurns = 60;
    int growMaxTurns = 120;

    int yieldMin = 1;
    int yieldMax = 2;

    int value = 0;
    int hungerRestore = 0;
    int healAmount = 0;

    // Optional short tag.
    const char* bonusTag = "";

    // Uppercase name.
    std::string name;
};

inline std::string cropName(uint32_t seed, CropRarity rarity, bool shiny, int variant) {
    RNG rng(hash32(seed ^ 0xC20F0A01u) ^ static_cast<uint32_t>(variant));

    static constexpr std::array<const char*, 28> ADJ_COMMON = {
        "WILD", "EARTH", "RIVER", "DUSK", "DAWN", "SUN", "MOON", "MOSS",
        "BROWN", "PALE", "BRIGHT", "SWEET", "BITTER", "SOUR", "WARM", "COLD",
        "HARD", "SOFT", "THIN", "FAT", "PRICKLY", "SLICK", "FERAL", "CALM",
        "SMOKE", "SAND", "WET", "DRY",
    };

    static constexpr std::array<const char*, 20> ADJ_RARE = {
        "GILDED", "LUMINOUS", "PHANTOM", "ABYSSAL", "STARLIT", "EMBER", "FROST",
        "VOID", "ARCANE", "RADIANT", "ECHOING", "GLASS", "IVORY", "OBSIDIAN",
        "CELESTIAL", "SABLE", "AURORA", "CRYSTAL", "BLOOD", "THUNDER",
    };

    static constexpr std::array<const char*, 30> NOUNS = {
        "ROOT", "BERRY", "BEAN", "LEAF", "BLOOM", "CAP", "MUSHROOM", "GOURD",
        "TUBER", "BULB", "STALK", "CLOVE", "MELON", "SPROUT", "SEEDPOD", "RIND",
        "CORN", "WHEAT", "HERB", "MINT", "THISTLE", "ONION", "GARLIC", "RADISH",
        "TURNIP", "CARROT", "PEPPER", "TOMATO", "SQUASH", "FLOX",
    };

    const bool rareAdj = (rarity >= CropRarity::Rare);
    const char* adj = rareAdj ? ADJ_RARE[rng.range(0, static_cast<int>(ADJ_RARE.size()) - 1)]
                              : ADJ_COMMON[rng.range(0, static_cast<int>(ADJ_COMMON.size()) - 1)];
    const char* noun = NOUNS[rng.range(0, static_cast<int>(NOUNS.size()) - 1)];

    std::string name;
    name.reserve(32);

    if (shiny) {
        static constexpr std::array<const char*, 6> SHINY = {
            "SHIMMERING", "PRISM", "OPAL", "RAINBOW", "STAR", "LUSTROUS",
        };
        name += SHINY[rng.range(0, static_cast<int>(SHINY.size()) - 1)];
        name += " ";
    }

    name += adj;
    name += " ";
    name += noun;
    return name;
}

inline CropSpec makeCrop(uint32_t cropSeed, int rarityHint = -1, int variantHint = -1, int shinyHint = -1) {
    CropSpec c;

    c.rarity = (rarityHint >= 0) ? clampRarityInt(rarityHint) : rollRarity(cropSeed);

    if (shinyHint >= 0) c.shiny = (shinyHint != 0);
    else c.shiny = rollShiny(cropSeed, c.rarity);

    c.variant = (variantHint >= 0) ? clampi(variantHint, 0, 15) : defaultVariant(cropSeed, c.rarity);

    // Base growth window by rarity.
    int gLo = 70, gHi = 120;
    switch (c.rarity) {
        case CropRarity::Common:    gLo = 55;  gHi = 95;  break;
        case CropRarity::Uncommon:  gLo = 70;  gHi = 120; break;
        case CropRarity::Rare:      gLo = 90;  gHi = 150; break;
        case CropRarity::Epic:      gLo = 120; gHi = 190; break;
        case CropRarity::Legendary: gLo = 160; gHi = 250; break;
        default: break;
    }

    c.growMinTurns = gLo;
    c.growMaxTurns = gHi;

    // Yield by rarity.
    int yLo = 1, yHi = 2;
    switch (c.rarity) {
        case CropRarity::Common:    yLo = 1; yHi = 2; break;
        case CropRarity::Uncommon:  yLo = 1; yHi = 3; break;
        case CropRarity::Rare:      yLo = 2; yHi = 4; break;
        case CropRarity::Epic:      yLo = 2; yHi = 5; break;
        case CropRarity::Legendary: yLo = 3; yHi = 6; break;
        default: break;
    }
    c.yieldMin = yLo;
    c.yieldMax = yHi;

    // Consumable tuning.
    int hunger = 45;
    int heal = 0;
    int value = 0;

    switch (c.rarity) {
        case CropRarity::Common:    hunger = 45; heal = 0; value = 8; break;
        case CropRarity::Uncommon:  hunger = 65; heal = 1; value = 14; break;
        case CropRarity::Rare:      hunger = 90; heal = 2; value = 25; break;
        case CropRarity::Epic:      hunger = 120; heal = 3; value = 45; break;
        case CropRarity::Legendary: hunger = 160; heal = 5; value = 90; break;
        default: break;
    }

    if (c.shiny) {
        hunger = static_cast<int>(hunger * 1.15f);
        value = static_cast<int>(value * 1.50f);
    }

    c.hungerRestore = clampi(hunger, 0, 9999);
    c.healAmount = clampi(heal, 0, 9999);
    c.value = clampi(value, 0, 9999);

    c.bonusTag = rollBonusTag(cropSeed, c.rarity);
    c.name = cropName(cropSeed, c.rarity, c.shiny, c.variant);
    return c;
}

// -----------------------------------------------------------------------------
// Gameplay helper formulas (future hooks).
// -----------------------------------------------------------------------------

inline int growDurationTurns(const CropSpec& crop, int fertility, int waterTier = 0) {
    // Lower fertility => slower. Water tier can speed up a little.
    const int fert = clampi(fertility, 0, 100);
    const int wt = clampi(waterTier, 0, 10);

    // Map fertility to a multiplier ~[1.35 .. 0.75].
    const float fertMul = 1.35f - 0.60f * (static_cast<float>(fert) / 100.0f);
    const float waterMul = 1.00f - 0.03f * static_cast<float>(wt);

    const int base = clampi((crop.growMinTurns + crop.growMaxTurns) / 2, 10, 9999);
    const int dur = static_cast<int>(std::lround(static_cast<float>(base) * fertMul * waterMul));
    return clampi(dur, 10, 9999);
}

inline int harvestYieldCount(const CropSpec& crop, int fertility, uint32_t harvestSeed) {
    const int fert = clampi(fertility, 0, 100);
    const int lo = std::min(crop.yieldMin, crop.yieldMax);
    const int hi = std::max(crop.yieldMin, crop.yieldMax);

    RNG rng(hash32(harvestSeed ^ 0x7131D00Du));

    // Fertility gives a mild upward bias.
    const float t = static_cast<float>(fert) / 100.0f;
    const int extra = (rng.next01() < t * 0.35f) ? 1 : 0;

    return clampi(rng.range(lo, hi) + extra, 0, 99);
}

inline int qualityGradeIndex(int fertility, CropRarity rarity, bool shiny) {
    // 0=C,1=B,2=A,3=S,4=SS
    int score = clampi(fertility, 0, 100);

    // Rarity has a small effect.
    score += 4 * static_cast<int>(rarity);

    if (shiny) score += 12;

    if (score >= 110) return 4;
    if (score >= 92) return 3;
    if (score >= 72) return 2;
    if (score >= 52) return 1;
    return 0;
}


// A compact grade label for a 0..15 produce-quality value.
// Quality is stored with 4 bits (0..15) on CropProduce items, where higher is better.
// This maps the fine-grained scale onto the familiar C/B/A/S/SS labels used by the UI.
inline const char* qualityGradeLetter(int quality) {
    const int q = clampi(quality, 0, 15);
    if (q >= 12) return "SS";
    if (q >= 9) return "S";
    if (q >= 6) return "A";
    if (q >= 3) return "B";
    return "C";
}

inline const char* qualityGradeName(int idx) {
    switch (clampi(idx, 0, 4)) {
        case 0: return "C";
        case 1: return "B";
        case 2: return "A";
        case 3: return "S";
        case 4: return "SS";
        default: return "C";
    }
}

} // namespace farmgen
