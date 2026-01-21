#pragma once

// Procedural crafting generation utilities.
//
// This is intentionally header-only so future gameplay/UI wiring can consume it
// without introducing new translation units.
//
// Design goals:
// - Deterministic: crafting outputs should be stable within a run.
// - Ingredient-driven: common items can still craft something, but rarer / shinier
//   ingredients bias toward stronger results.
// - Save-compatible: this module does not require any save format changes.
//
// NOTE: This round ships the *foundation generator* only. The gameplay loop
// (inventory prompt, turn cost, etc.) is wired in a later round.

#include "items.hpp"
#include "fishing_gen.hpp"
#include "farm_gen.hpp"

#include "common.hpp"
#include "rng.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <string>

namespace craftgen {

// -----------------------------------------------------------------------------
// Ingredient analysis
// -----------------------------------------------------------------------------

struct Essence {
    // Short uppercase tag used for UI and for deterministic recipe selection.
    // Empty means "no special essence".
    std::string tag;

    // Coarse tier score (0..10-ish). Higher tier increases odds of better output.
    int tier = 0;

    // Cosmetic + tuning hint.
    bool shiny = false;
};

inline const char* safeTag(const char* t) {
    return (t && t[0]) ? t : "";
}

inline int clampTier(int v) {
    return clampi(v, 0, 12);
}

inline Essence essenceFor(const Item& it) {
    Essence e;

    // Fish: uses the fish seed + packed meta.
    if (isFishKind(it.kind)) {
        const uint32_t seed = fishSeedFromCharges(it.charges);
        const int rarity = fishRarityFromEnchant(it.enchant);
        const int sizeClass = fishSizeClassFromEnchant(it.enchant);
        const bool shiny = fishIsShinyFromEnchant(it.enchant);

        const fishgen::FishSpec fs = fishgen::makeFish(seed, rarity, sizeClass, shiny ? 1 : 0);
        e.tag = safeTag(fs.bonusTag);
        e.shiny = shiny;

        // Tier: rarity dominates; sizeClass adds texture; shiny gives a bump.
        const int base = clampi(rarity, 0, 4) * 2;
        const int sizeB = clampi(sizeClass, 0, 15) / 5; // 0..3
        const int shinyB = shiny ? 2 : 0;
        e.tier = clampTier(base + sizeB + shinyB);
        return e;
    }

    // Farming: seeds and produce share crop metadata.
    if (isSeedKind(it.kind) || isCropProduceKind(it.kind)) {
        const uint32_t seed = cropSeedFromCharges(it.charges);
        const int rarity = cropRarityFromEnchant(it.enchant);
        const int variant = cropVariantFromEnchant(it.enchant);
        const bool shiny = cropIsShinyFromEnchant(it.enchant);
        const int quality = isCropProduceKind(it.kind) ? cropQualityFromEnchant(it.enchant) : 0;

        const farmgen::CropSpec cs = farmgen::makeCrop(seed, rarity, variant, shiny ? 1 : 0);
        e.tag = safeTag(cs.bonusTag);
        e.shiny = shiny;

        // Tier: rarity dominates; quality adds some weight for produce.
        const int base = clampi(rarity, 0, 4) * 2;
        const int qB = clampi(quality, 0, 15) / 4; // 0..3
        const int shinyB = shiny ? 2 : 0;
        e.tier = clampTier(base + qB + shinyB);
        return e;
    }

    // Corpses: treat as a weak essence source (future: corpse-specific recipes).
    if (isCorpseKind(it.kind)) {
        // Map corpse family to a rough tag.
        switch (it.kind) {
            case ItemKind::CorpseSnake:
            case ItemKind::CorpseSpider:
                e.tag = "VENOM";
                e.tier = 4;
                break;
            case ItemKind::CorpseTroll:
                e.tag = "REGEN";
                e.tier = 5;
                break;
            case ItemKind::CorpseWizard:
                e.tag = "AURORA";
                e.tier = 5;
                break;
            case ItemKind::CorpseMinotaur:
            case ItemKind::CorpseOgre:
                e.tag = "STONE";
                e.tier = 5;
                break;
            default:
                e.tag = "";
                e.tier = 2;
                break;
        }
        // Freshness is tracked in charges elsewhere (<=0 means rotten).
        if (it.charges <= 0) e.tier = std::max(0, e.tier - 2);
        return e;
    }

    // Fallback: give light "tool" essences based on item families.
    if (isPotionKind(it.kind)) {
        e.tag = "ALCH";
        e.tier = 2;
    } else if (isScrollKind(it.kind)) {
        e.tag = "RUNE";
        e.tier = 2;
    } else if (isRingKind(it.kind) || isWandKind(it.kind) || isSpellbookKind(it.kind)) {
        e.tag = "ARC";
        e.tier = 3;
    } else {
        e.tag = "";
        e.tier = 1;
    }

    return e;
}

// -----------------------------------------------------------------------------
// Craft outcome
// -----------------------------------------------------------------------------

struct Outcome {
    Item out;
    std::string tagA;
    std::string tagB;
    int tier = 0;
};

inline uint32_t recipeSeed(uint32_t runSeed, const Essence& a, const Essence& b, int tier) {
    // Order-independent hashing: sort tags to make A+B == B+A.
    const std::string ta = a.tag;
    const std::string tb = b.tag;
    const std::string lo = (ta <= tb) ? ta : tb;
    const std::string hi = (ta <= tb) ? tb : ta;

    // Domain-separated constant for craftgen.
    uint32_t h = hash32(runSeed ^ 0xC4A57105u);
    // Hash string content cheaply.
    for (char c : lo) h = hashCombine(h, static_cast<uint32_t>(static_cast<uint8_t>(c)));
    h = hashCombine(h, 0x9E3779B9u);
    for (char c : hi) h = hashCombine(h, static_cast<uint32_t>(static_cast<uint8_t>(c)));
    h = hashCombine(h, static_cast<uint32_t>(tier));
    return hash32(h ^ 0xC001D00Du);
}

inline ItemKind pickResultKind(const std::string& a, const std::string& b, int tier, RNG& rng) {
    // If both ingredients share the same strong tag, bias toward a matching item.
    if (a == b && !a.empty()) {
        if (a == "REGEN") return ItemKind::PotionRegeneration;
        if (a == "HASTE") return ItemKind::PotionHaste;
        if (a == "SHIELD" || a == "STONE") return ItemKind::PotionShielding;
        if (a == "CLARITY" || a == "AURORA") return ItemKind::PotionClarity;
        if (a == "VENOM") return ItemKind::PotionAntidote;
        if (a == "LUCK") return ItemKind::ScrollIdentify;
    }

    // Simple cross-tag synergies.
    auto has = [&](const char* t) {
        return a == t || b == t;
    };

    if (has("REGEN") && has("VENOM")) return ItemKind::PotionAntidote;
    if (has("REGEN") && has("SHIELD")) return ItemKind::PotionShielding;
    if (has("HASTE") && has("CLARITY")) return ItemKind::ScrollIdentify;
    if (has("EMBER") && has("STONE")) return ItemKind::WandFireball;
    if (has("LUCK") && has("ARC")) return ItemKind::ScrollEnchantWeapon;
    if (has("LUCK") && has("RUNE")) return ItemKind::ScrollEnchantArmor;

    // Fallback pool: scale with tier.
    const int roll = rng.range(0, 99);
    if (tier >= 8) {
        if (roll < 25) return ItemKind::ScrollEnchantWeapon;
        if (roll < 45) return ItemKind::ScrollEnchantArmor;
        if (roll < 60) return ItemKind::WandFireball;
        if (roll < 80) return ItemKind::ScrollRemoveCurse;
        return ItemKind::PotionRegeneration;
    }
    if (tier >= 5) {
        if (roll < 30) return ItemKind::PotionHaste;
        if (roll < 55) return ItemKind::PotionShielding;
        if (roll < 75) return ItemKind::ScrollIdentify;
        return ItemKind::PotionClarity;
    }
    // Low-tier results.
    if (roll < 40) return ItemKind::PotionHealing;
    if (roll < 60) return ItemKind::ScrollMapping;
    if (roll < 80) return ItemKind::ScrollTeleport;
    return ItemKind::FoodRation;
}


// -----------------------------------------------------------------------------
// Recipe sigils (UI flavor)
// -----------------------------------------------------------------------------
//
// A "sigil" is a deterministic, human-friendly name derived from a recipe seed.
// It provides a lore-friendly handle for recipe journaling without exposing the
// underlying seed values.
//
// Note: This is purely cosmetic; it does not affect crafting outcomes.

inline std::string sigilName(uint32_t recipeSeed) {
    static constexpr std::array<const char*, 32> kAdj = {{
        "EMBER","FROST","GILDED","HOLLOW","ARCANE","SERPENT","LUMINOUS","SABLE",
        "IVORY","RUSTED","CELESTIAL","CRYPTIC","FERAL","SILKEN","RADIANT","MURKY",
        "SACRED","VILE","BRIGHT","DREAD","WILD","STEADFAST","SWIFT","STONE",
        "IRON","GLASS","MOSS","VOID","THUNDER","ASHEN","MIRROR","WANDERING",
    }};

    static constexpr std::array<const char*, 32> kNoun = {{
        "ANVIL","LENS","WARD","SPIRE","SEAL","THREAD","BLADE","CROWN",
        "KEY","GRAIL","RUNE","SIGIL","FANG","ROOT","SPARK","VEIL",
        "ORB","FURNACE","ALTAR","HARP","HORIZON","LANTERN","TALON","STONE",
        "SCROLL","POTION","RING","WAND","GLYPH","CHAIN","BLOOM","TIDE",
    }};

    // Mix salts so closely related recipe seeds don't cluster.
    RNG rng(hashCombine(recipeSeed, 0x51C11A5u));

    const int ai = rng.range(0, static_cast<int>(kAdj.size()) - 1);
    const int ni = rng.range(0, static_cast<int>(kNoun.size()) - 1);

    std::string s;
    s.reserve(32);
    s += kAdj[static_cast<size_t>(ai)];
    s += " ";
    s += kNoun[static_cast<size_t>(ni)];
    return s;
}


inline Outcome craft(uint32_t runSeed, const Item& a, const Item& b) {
    const Essence ea = essenceFor(a);
    const Essence eb = essenceFor(b);

    Outcome o;
    o.tagA = ea.tag;
    o.tagB = eb.tag;
    o.tier = clampTier((ea.tier + eb.tier + 1) / 2);

    const uint32_t rs = recipeSeed(runSeed, ea, eb, o.tier);
    RNG rng(rs);

    const std::string ta = ea.tag;
    const std::string tb = eb.tag;
    const std::string lo = (ta <= tb) ? ta : tb;
    const std::string hi = (ta <= tb) ? tb : ta;

    Item out;
    out.kind = pickResultKind(lo, hi, o.tier, rng);
    out.count = 1;
    out.charges = 0;
    out.enchant = 0;
    out.buc = 0;
    out.spriteSeed = rs;

    // Bless/curse tuning: higher tier slightly increases odds of blessing.
    const int bucRoll = rng.range(0, 99);
    const int blessChance = clampi(3 + o.tier * 3 + (ea.shiny ? 4 : 0) + (eb.shiny ? 4 : 0), 0, 35);
    const int curseChance = clampi(6 - o.tier, 0, 6);
    if (bucRoll < blessChance) out.buc = 1;
    else if (bucRoll >= 100 - curseChance) out.buc = -1;

    // Stackable outputs can sometimes produce 2 when tier is high.
    if (isStackable(out.kind) && o.tier >= 7 && rng.range(0, 99) < 20) {
        out.count = 2;
    }

    // Wands: start with a small random charge count.
    if (isWandKind(out.kind)) {
        const ItemDef& d = itemDef(out.kind);
        const int maxC = std::max(1, d.maxCharges);
        const int base = clampi(1 + (o.tier / 3), 1, maxC);
        out.charges = clampi(rng.range(std::max(1, base - 1), std::min(maxC, base + 1)), 1, maxC);
    }

    // Enchant scrolls don't need extra metadata.

    o.out = out;
    return o;
}

} // namespace craftgen
