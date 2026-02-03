#pragma once

// Procedural crafting generation utilities.
//
// This module is intentionally header-only so gameplay/UI code can consume it
// without introducing new translation units.
//
// Design goals:
// - Deterministic: outputs are stable within a run.
// - Ingredient-driven: each ingredient contributes an "essence" (tag + tier).
// - Procedural: recipes incorporate ingredient fingerprints so different items
//   can yield different outcomes even when their essences match.
// - Save-compatible: uses only existing Item fields (no save format changes).
//
// Crafting can yield either:
// - Consumables (potions/scrolls/wands/spellbooks/food/rune tablets),
// - Forged gear (weapons/armor/rings) with deterministic ego/artifact infusion, or
// - Refined essences (Essence Shards) when combining shards of the same tag.

#include "items.hpp"
#include "artifact_gen.hpp"
#include "butcher_gen.hpp"
#include "fishing_gen.hpp"
#include "farm_gen.hpp"
#include "proc_spells.hpp"
#include "craft_tags.hpp"

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

    // Coarse tier score (0..12). Higher tier biases toward stronger results.
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

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline bool hasTagPair(const std::string& a, const std::string& b, const char* t) {
    return a == t || b == t;
}

inline bool hasAnyTagPair2(const std::string& a, const std::string& b, const char* t0, const char* t1) {
    return hasTagPair(a, b, t0) || hasTagPair(a, b, t1);
}

inline bool hasAnyTagPair3(const std::string& a, const std::string& b, const char* t0, const char* t1, const char* t2) {
    return hasTagPair(a, b, t0) || hasTagPair(a, b, t1) || hasTagPair(a, b, t2);
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

    // Corpses: treat as a weak essence source.
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

    // Rune tablets: spriteSeed stores a packed ProcSpell id.
    if (it.kind == ItemKind::RuneTablet) {
        const uint8_t t = procSpellTierClamped(it.spriteSeed);
        e.tag = "RUNE";
        // Higher tier tablets are a meaningful ingredient.
        e.tier = clampTier(4 + static_cast<int>(t) / 3); // 4..9
        e.shiny = (t >= 11);
        return e;
    }

    // Butchery outputs: meat/hide/bones carry deterministic provenance and quality.
    if (it.kind == ItemKind::ButcheredMeat) {
        const int tagId = butcherMeatTagFromEnchant(it.enchant);
        const auto tg = butchergen::tagFromIndex(tagId);
        const char* tok = butchergen::tagToken(tg);
        e.tag = safeTag(tok);

        const int hunger = butcherMeatHungerFromEnchant(it.enchant);
        const int heal = butcherMeatHealFromEnchant(it.enchant);

        int tier = (tok && tok[0]) ? 5 : 2;
        tier += clampi(hunger, 0, 255) / 120; // 0..2
        tier += clampi(heal, 0, 255) / 40;   // 0..6 (usually small)

        // Freshness is tracked in charges elsewhere (<=0 means rotten).
        if (it.charges <= 0) tier -= 2;
        else if (it.charges <= 160) tier -= 1;

        e.tier = clampTier(tier);
        e.shiny = (tok && tok[0]) && (it.charges > 160);
        return e;
    }

    if (it.kind == ItemKind::ButcheredHide || it.kind == ItemKind::ButcheredBones) {
        const int q = butcherMaterialQualityFromEnchant(it.enchant);
        const int v = butcherMaterialVariantFromEnchant(it.enchant);

        const int srcRaw = butcherSourceKindFromEnchant(it.enchant);
        ItemKind srcKind = ItemKind::CorpseGoblin;
        if (srcRaw >= 0 && srcRaw < ITEM_KIND_COUNT) srcKind = static_cast<ItemKind>(srcRaw);

        const char* tag = "";
        if (it.kind == ItemKind::ButcheredHide) {
            const auto ht = butchergen::hideTypeFromIndex(v);
            switch (ht) {
                case butchergen::HideType::Pelt:       tag = "REGEN"; break;
                case butchergen::HideType::Scales:     tag = "STONE"; break;
                case butchergen::HideType::Chitin:     tag = (srcKind == ItemKind::CorpseSpider) ? "VENOM" : "SHIELD"; break;
                case butchergen::HideType::MimicSkin:  tag = "CLARITY"; break;
                case butchergen::HideType::RobeScraps: tag = "AURORA"; break;
                default:                               tag = "SHIELD"; break;
            }
        } else {
            const auto bt = butchergen::boneTypeFromIndex(v);
            switch (bt) {
                case butchergen::BoneType::Horn:        tag = "STONE"; break;
                case butchergen::BoneType::Fang:        tag = "VENOM"; break;
                case butchergen::BoneType::ChitinShard: tag = "VENOM"; break;
                case butchergen::BoneType::Tooth:       tag = "CLARITY"; break;
                default:                                tag = "SHIELD"; break;
            }
        }

        e.tag = safeTag(tag);
        e.shiny = (q >= 240);
        e.tier = clampTier(2 + butcherQualityTierFromQuality(q) + (e.shiny ? 1 : 0));
        return e;
    }

    // Procedural crafting byproduct: Essence Shards encode (tag,tier,shiny) in enchant.
    if (it.kind == ItemKind::EssenceShard) {
        const int tagId = essenceShardTagFromEnchant(it.enchant);
        const int t = essenceShardTierFromEnchant(it.enchant);
        const bool shiny = essenceShardIsShinyFromEnchant(it.enchant);

        const crafttags::Tag tg = crafttags::tagFromIndex(tagId);
        const char* tok = crafttags::tagToken(tg);
        e.tag = safeTag(tok);
        e.tier = clampTier(std::max(1, t));
        e.shiny = shiny;
        return e;
    }

    // Wearable gear: treat as high-value essence sources.
    if (isWearableGear(it.kind)) {
        const ItemDef& d = itemDef(it.kind);

        // Prefer concrete tags that capture the theme of the item.
        if (itemIsArtifact(it)) {
            const auto p = artifactgen::artifactPower(it);
            e.tag = safeTag(artifactgen::powerTag(p));
            e.shiny = true;
        } else if (it.ego != ItemEgo::None) {
            // Ego tags intentionally share tokens with butchering/fishing/farming.
            switch (it.ego) {
                case ItemEgo::Flaming:   e.tag = "EMBER"; break;
                case ItemEgo::Venom:     e.tag = "VENOM"; break;
                case ItemEgo::Vampiric:  e.tag = "REGEN"; break;
                case ItemEgo::Webbing:   e.tag = "SHIELD"; break;
                case ItemEgo::Corrosive: e.tag = "STONE"; break;
                case ItemEgo::Dazing:    e.tag = "CLARITY"; break;
                default:                 e.tag = ""; break;
            }
            e.shiny = true;
        } else if (isRingKind(it.kind)) {
            // Rings express their primary stat through tags.
            if (it.kind == ItemKind::RingProtection) e.tag = "SHIELD";
            else if (it.kind == ItemKind::RingAgility) e.tag = "HASTE";
            else if (it.kind == ItemKind::RingFocus) e.tag = "CLARITY";
            else if (it.kind == ItemKind::RingMight) e.tag = "EMBER";
            else if (it.kind == ItemKind::RingSearching) e.tag = "RUNE";
            else if (it.kind == ItemKind::RingSustenance) e.tag = "REGEN";
            else e.tag = "ARC";
        } else if (isWandKind(it.kind)) {
            // Wands: map a few iconic wands to classic tags.
            if (it.kind == ItemKind::WandFireball) e.tag = "EMBER";
            else if (it.kind == ItemKind::WandDigging) e.tag = "STONE";
            else e.tag = "ARC";
        } else if (isArmor(it.kind)) {
            e.tag = "SHIELD";
        } else if (isWeapon(it.kind)) {
            // Most mundane weapons are tagless; their tier still matters.
            e.tag = "";
        }

        // Tier heuristic from base stats + item state.
        int score = 0;
        score += std::abs(d.meleeAtk) * 3;
        score += std::abs(d.rangedAtk) * 2;
        score += std::abs(d.defense) * 4;
        score += std::abs(d.modMight) + std::abs(d.modAgility) + std::abs(d.modVigor) + std::abs(d.modFocus);

        // Wands: treat charge capacity as part of power.
        if (isWandKind(it.kind)) {
            score += std::max(0, d.maxCharges) / 2;
            score += clampi(it.charges, 0, std::max(1, d.maxCharges)) / 2;
        }

        int t = 2 + clampi(score, 0, 18) / 3;
        t += it.enchant;
        t += (it.buc < 0 ? -1 : (it.buc > 0 ? 1 : 0));
        if (itemIsArtifact(it)) t += 3;
        else if (it.ego != ItemEgo::None) t += 2;

        e.tier = clampTier(t);
        return e;
    }

    // Identifiable consumables: map to effect-ish tags when possible.
    if (isPotionKind(it.kind)) {
        switch (it.kind) {
            case ItemKind::PotionRegeneration: e.tag = "REGEN"; e.tier = 6; break;
            case ItemKind::PotionHaste:        e.tag = "HASTE"; e.tier = 6; break;
            case ItemKind::PotionShielding:    e.tag = "SHIELD"; e.tier = 6; break;
            case ItemKind::PotionClarity:      e.tag = "CLARITY"; e.tier = 6; break;
            case ItemKind::PotionInvisibility: e.tag = "AURORA"; e.tier = 6; break;
            case ItemKind::PotionAntidote:     e.tag = "VENOM"; e.tier = 5; break;
            case ItemKind::PotionEnergy:       e.tag = "ARC"; e.tier = 5; break;
            case ItemKind::PotionVision:       e.tag = "CLARITY"; e.tier = 4; break;
            case ItemKind::PotionStrength:     e.tag = "EMBER"; e.tier = 4; break;
            case ItemKind::PotionHealing:      e.tag = "ALCH"; e.tier = 2; break;
            default:
                e.tag = "ALCH";
                e.tier = 2;
                break;
        }
        return e;
    }

    if (isScrollKind(it.kind)) {
        switch (it.kind) {
            case ItemKind::ScrollEnchantWeapon:
            case ItemKind::ScrollEnchantArmor:
            case ItemKind::ScrollEnchantRing:
                e.tag = "LUCK";
                e.tier = 6;
                break;
            case ItemKind::ScrollIdentify:
            case ItemKind::ScrollDetectTraps:
            case ItemKind::ScrollDetectSecrets:
                e.tag = "CLARITY";
                e.tier = 5;
                break;
            case ItemKind::ScrollRemoveCurse:
                e.tag = "AURORA";
                e.tier = 6;
                break;
            case ItemKind::ScrollEarth:
                e.tag = "STONE";
                e.tier = 5;
                break;
            case ItemKind::ScrollConfusion:
            case ItemKind::ScrollFear:
                e.tag = "DAZE";
                e.tier = 4;
                break;
            default:
                e.tag = "RUNE";
                e.tier = 2;
                break;
        }
        return e;
    }

    if (isSpellbookKind(it.kind)) {
        switch (it.kind) {
            case ItemKind::SpellbookMinorHeal:    e.tag = "REGEN"; e.tier = 6; break;
            case ItemKind::SpellbookHaste:        e.tag = "HASTE"; e.tier = 6; break;
            case ItemKind::SpellbookStoneskin:    e.tag = "STONE"; e.tier = 6; break;
            case ItemKind::SpellbookInvisibility: e.tag = "AURORA"; e.tier = 6; break;
            case ItemKind::SpellbookPoisonCloud:  e.tag = "VENOM"; e.tier = 7; break;
            case ItemKind::SpellbookFireball:     e.tag = "EMBER"; e.tier = 7; break;
            default:
                e.tag = "ARC";
                e.tier = 4;
                break;
        }
        return e;
    }

    // Fallback.
    e.tag = "";
    e.tier = 1;
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

    // Optional deterministic byproduct.
    bool hasByproduct = false;
    Item byproduct;
};

inline uint32_t ingredientFingerprint(const Item& it) {
    // IMPORTANT: ignore id/count so stacks and save/load don't perturb recipes.
    uint32_t h = hash32(static_cast<uint32_t>(it.kind) ^ 0xD00DFEEDu);
    h = hashCombine(h, static_cast<uint32_t>(static_cast<int32_t>(it.enchant)));
    h = hashCombine(h, static_cast<uint32_t>(static_cast<int32_t>(it.charges)));
    h = hashCombine(h, static_cast<uint32_t>(static_cast<int32_t>(it.buc)));
    h = hashCombine(h, static_cast<uint32_t>(it.flags));
    h = hashCombine(h, static_cast<uint32_t>(it.ego));
    h = hashCombine(h, it.spriteSeed);
    return hash32(h ^ 0xA53EEDu);
}

inline uint32_t recipeSeed(uint32_t runSeed, const Item& ia, const Item& ib, const Essence& ea, const Essence& eb, int tier) {
    // Order-independent hashing: sort (tag,fingerprint) so A+B == B+A.
    const uint32_t fa = ingredientFingerprint(ia);
    const uint32_t fb = ingredientFingerprint(ib);

    const bool swap = (ea.tag > eb.tag) || (ea.tag == eb.tag && fa > fb);
    const std::string loTag = swap ? eb.tag : ea.tag;
    const std::string hiTag = swap ? ea.tag : eb.tag;
    const uint32_t loF = swap ? fb : fa;
    const uint32_t hiF = swap ? fa : fb;

    uint32_t h = hash32(runSeed ^ 0xC4A57105u);
    for (char c : loTag) h = hashCombine(h, static_cast<uint32_t>(static_cast<uint8_t>(c)));
    h = hashCombine(h, loF);
    h = hashCombine(h, 0x9E3779B9u);
    for (char c : hiTag) h = hashCombine(h, static_cast<uint32_t>(static_cast<uint8_t>(c)));
    h = hashCombine(h, hiF);
    h = hashCombine(h, static_cast<uint32_t>(tier));
    return hash32(h ^ 0xC001D00Du);
}

inline bool isGearMaterialKind(ItemKind k) {
    return (k == ItemKind::ButcheredHide || k == ItemKind::ButcheredBones);
}

inline bool canBeArtifactCraft(ItemKind k) {
    // Mirror the loot rules: artifacts are wearable gear, excluding wands.
    if (!isWearableGear(k)) return false;
    if (isWandKind(k)) return false;
    if (k == ItemKind::AmuletYendor) return false;
    if (k == ItemKind::Chest || k == ItemKind::ChestOpen) return false;
    return true;
}

inline bool canHaveMeleeEgoCraft(ItemKind k) {
    // Ego procs currently apply to equipped melee weapons.
    return (k == ItemKind::Dagger || k == ItemKind::Sword || k == ItemKind::Axe);
}

inline artifactgen::Power desiredArtifactPowerForTags(const std::string& a, const std::string& b, RNG& rng) {
    using P = artifactgen::Power;

    // Dominant signals.
    if (hasTagPair(a, b, "VENOM")) return P::Venom;
    if (hasAnyTagPair2(a, b, "EMBER", "FLAME")) return P::Flame;
    if (hasAnyTagPair2(a, b, "DAZE", "CLARITY")) return P::Daze;
    if (hasAnyTagPair3(a, b, "WARD", "SHIELD", "STONE")) return P::Ward;
    if (hasAnyTagPair2(a, b, "REGEN", "VITALITY")) return P::Vitality;

    // Otherwise, deterministic variety.
    const int r = rng.range(0, static_cast<int>(P::COUNT) - 1);
    return static_cast<P>(r);
}

inline ItemEgo desiredMeleeEgoForTags(const std::string& a, const std::string& b, RNG& rng) {
    // Map tags into the existing ego palette.
    if (hasTagPair(a, b, "VENOM")) return ItemEgo::Venom;
    if (hasAnyTagPair2(a, b, "EMBER", "FLAME")) return ItemEgo::Flaming;
    if (hasAnyTagPair2(a, b, "REGEN", "VITALITY")) return ItemEgo::Vampiric;
    if (hasAnyTagPair2(a, b, "DAZE", "CLARITY")) return ItemEgo::Dazing;
    if (hasAnyTagPair2(a, b, "SHIELD", "WARD")) return ItemEgo::Webbing;
    if (hasTagPair(a, b, "STONE")) return ItemEgo::Corrosive;

    // No strong theme: mostly none, rarely a random brand at high tiers.
    if (rng.range(0, 99) < 85) return ItemEgo::None;
    const int r = rng.range(1, ITEM_EGO_COUNT - 1);
    return static_cast<ItemEgo>(r);
}

inline uint32_t tuneArtifactSeedForPower(uint32_t baseSeed, ItemKind kind, artifactgen::Power desired) {
    // Try a small deterministic search to align the artifact power to the recipe theme.
    // Probability of failure after 32 tries is (4/5)^32 ~= 0.0003.
    Item tmp;
    tmp.kind = kind;
    tmp.id = 1;
    tmp.spriteSeed = baseSeed;

    if (artifactgen::artifactPower(tmp) == desired) return baseSeed;

    uint32_t s = baseSeed;
    for (uint32_t i = 1; i <= 32; ++i) {
        const uint32_t cand = hash32(hashCombine(s, 0xA11F00Du ^ i));
        tmp.spriteSeed = cand;
        if (artifactgen::artifactPower(tmp) == desired) return cand;
        s = cand;
    }
    return baseSeed;
}

inline ItemKind pickConsumableResultKind(const std::string& a, const std::string& b, int tier, RNG& rng) {
    // If both ingredients share the same strong tag, bias toward a matching item.
    if (a == b && !a.empty()) {
        if (a == "REGEN") return ItemKind::PotionRegeneration;
        if (a == "HASTE") return ItemKind::PotionHaste;
        if (a == "SHIELD" || a == "WARD" || a == "STONE") return ItemKind::PotionShielding;
        if (a == "CLARITY") return ItemKind::PotionClarity;
        if (a == "AURORA") return ItemKind::PotionInvisibility;
        if (a == "VENOM") return ItemKind::PotionAntidote;
        if (a == "EMBER" || a == "FLAME") return (tier >= 7 ? ItemKind::SpellbookFireball : ItemKind::PotionStrength);
        if (a == "DAZE") return ItemKind::ScrollConfusion;
        if (a == "LUCK") return ItemKind::ScrollIdentify;
        if (a == "ARC") return ItemKind::WandSparks;
        if (a == "RUNE") return ItemKind::ScrollRemoveCurse;
    }

    auto has = [&](const char* t) { return a == t || b == t; };

    // Spellbook synergies: rune + theme.
    if (has("RUNE") && has("VENOM")) return ItemKind::SpellbookPoisonCloud;
    if (has("RUNE") && (has("EMBER") || has("FLAME"))) return ItemKind::SpellbookFireball;
    if (has("RUNE") && has("STONE")) return ItemKind::SpellbookStoneskin;
    if (has("RUNE") && has("HASTE")) return ItemKind::SpellbookHaste;
    if (has("RUNE") && has("REGEN")) return ItemKind::SpellbookMinorHeal;
    if (has("RUNE") && has("CLARITY")) return ItemKind::SpellbookDetectTraps;

    // Classic potion/scroll synergies.
    if (has("REGEN") && has("VENOM")) return ItemKind::PotionAntidote;
    if (has("REGEN") && (has("SHIELD") || has("STONE"))) return ItemKind::PotionShielding;
    if (has("HASTE") && has("CLARITY")) return ItemKind::ScrollIdentify;

    // Arcana synergies.
    if ((has("EMBER") || has("FLAME")) && has("STONE")) return ItemKind::WandFireball;
    if (has("LUCK") && has("ARC")) return ItemKind::ScrollEnchantWeapon;
    if (has("LUCK") && has("RUNE")) return ItemKind::ScrollEnchantArmor;

    // High-tier rune work: occasionally mint a rune tablet.
    if ((has("RUNE") || has("ARC")) && tier >= 9 && rng.range(0, 99) < 12) {
        return ItemKind::RuneTablet;
    }

    // Fallback pool: scale with tier.
    const int roll = rng.range(0, 99);
    if (tier >= 10) {
        if (roll < 18) return ItemKind::ScrollEnchantWeapon;
        if (roll < 34) return ItemKind::ScrollEnchantArmor;
        if (roll < 46) return ItemKind::ScrollEnchantRing;
        if (roll < 58) return ItemKind::ScrollRemoveCurse;
        if (roll < 72) return ItemKind::SpellbookFireball;
        if (roll < 82) return ItemKind::WandFireball;
        if (roll < 92) return ItemKind::PotionRegeneration;
        return ItemKind::PotionClarity;
    }
    if (tier >= 7) {
        if (roll < 20) return ItemKind::PotionHaste;
        if (roll < 40) return ItemKind::PotionShielding;
        if (roll < 56) return ItemKind::ScrollIdentify;
        if (roll < 68) return ItemKind::SpellbookHaste;
        if (roll < 80) return ItemKind::WandSparks;
        return ItemKind::PotionClarity;
    }
    if (tier >= 4) {
        if (roll < 30) return ItemKind::PotionHealing;
        if (roll < 50) return ItemKind::ScrollMapping;
        if (roll < 65) return ItemKind::ScrollTeleport;
        if (roll < 80) return ItemKind::WandSparks;
        return ItemKind::SpellbookMagicMissile;
    }

    // Low-tier results.
    if (roll < 40) return ItemKind::PotionHealing;
    if (roll < 60) return ItemKind::ScrollMapping;
    if (roll < 80) return ItemKind::ScrollTeleport;
    return ItemKind::FoodRation;
}

inline ItemKind pickRingKindForTags(const std::string& a, const std::string& b, int tier, RNG& rng) {
    // Strongly themed rings.
    if (hasTagPair(a, b, "SHIELD") || hasTagPair(a, b, "WARD") || hasTagPair(a, b, "STONE")) return ItemKind::RingProtection;
    if (hasTagPair(a, b, "HASTE")) return ItemKind::RingAgility;
    if (hasTagPair(a, b, "CLARITY") || hasTagPair(a, b, "DAZE")) return ItemKind::RingFocus;
    if (hasAnyTagPair2(a, b, "EMBER", "FLAME")) return ItemKind::RingMight;
    if (hasTagPair(a, b, "RUNE")) return ItemKind::RingSearching;
    if (hasAnyTagPair2(a, b, "REGEN", "VITALITY")) return ItemKind::RingSustenance;

    // Slight bias: higher tiers are more likely to produce utility rings.
    const int roll = rng.range(0, 99);
    if (tier >= 8) {
        if (roll < 22) return ItemKind::RingProtection;
        if (roll < 42) return ItemKind::RingFocus;
        if (roll < 60) return ItemKind::RingAgility;
        if (roll < 78) return ItemKind::RingMight;
        if (roll < 90) return ItemKind::RingSearching;
        return ItemKind::RingSustenance;
    }
    if (roll < 30) return ItemKind::RingProtection;
    if (roll < 55) return ItemKind::RingAgility;
    if (roll < 80) return ItemKind::RingMight;
    return ItemKind::RingFocus;
}

inline ItemKind pickRangedKindForTags(const std::string& a, const std::string& b, int tier, RNG& rng) {
    // Wands are gated by tier so early crafting doesn't flood charges.
    if ((hasAnyTagPair2(a, b, "EMBER", "FLAME")) && tier >= 6) return ItemKind::WandFireball;
    if ((hasTagPair(a, b, "STONE") || hasTagPair(a, b, "SHIELD")) && tier >= 6) return ItemKind::WandDigging;
    if ((hasTagPair(a, b, "ARC") || hasTagPair(a, b, "RUNE") || hasTagPair(a, b, "CLARITY")) && tier >= 5) return ItemKind::WandSparks;

    // Mundane ranged.
    return (rng.range(0, 99) < (tier >= 6 ? 65 : 40)) ? ItemKind::Bow : ItemKind::Sling;
}

inline ItemKind pickGearResultKind(const Item& ia, const Item& ib, const std::string& a, const std::string& b, int tier, RNG& rng) {
    const bool aGear = isWearableGear(ia.kind);
    const bool bGear = isWearableGear(ib.kind);
    const bool aMat = isGearMaterialKind(ia.kind);
    const bool bMat = isGearMaterialKind(ib.kind);

    const EquipSlot sa = aGear ? equipSlot(ia.kind) : EquipSlot::None;
    const EquipSlot sb = bGear ? equipSlot(ib.kind) : EquipSlot::None;

    EquipSlot slot = EquipSlot::None;

    if (aGear && !bGear) slot = sa;
    else if (bGear && !aGear) slot = sb;
    else if (aGear && bGear) {
        if (sa == sb) slot = sa;
        else {
            // Resolve mixed-slot reforges by theme.
            if (hasAnyTagPair3(a, b, "SHIELD", "WARD", "STONE")) slot = EquipSlot::Armor;
            else if (hasAnyTagPair3(a, b, "ARC", "RUNE", "CLARITY")) slot = EquipSlot::Ring;
            else if (hasAnyTagPair3(a, b, "EMBER", "FLAME", "VENOM") || hasTagPair(a, b, "DAZE")) slot = EquipSlot::MeleeWeapon;
            else slot = (rng.range(0, 99) < 50) ? sa : sb;
        }
    } else {
        // Material-only forging.
        if ((ia.kind == ItemKind::ButcheredHide || ib.kind == ItemKind::ButcheredHide) &&
            (ia.kind != ItemKind::ButcheredBones && ib.kind != ItemKind::ButcheredBones)) {
            slot = EquipSlot::Armor;
        } else if ((ia.kind == ItemKind::ButcheredBones || ib.kind == ItemKind::ButcheredBones) &&
                   (ia.kind != ItemKind::ButcheredHide && ib.kind != ItemKind::ButcheredHide)) {
            slot = EquipSlot::MeleeWeapon;
        } else if (aMat || bMat) {
            // Hide + bone mix: allow rings sometimes if the essence leans arcane.
            if (tier >= 6 && hasAnyTagPair3(a, b, "ARC", "RUNE", "CLARITY") && rng.range(0, 99) < 35) slot = EquipSlot::Ring;
            else slot = (rng.range(0, 99) < 55) ? EquipSlot::Armor : EquipSlot::MeleeWeapon;
        }
    }

    if (slot == EquipSlot::None) {
        // Shouldn't happen often; fallback to consumables.
        return pickConsumableResultKind(a, b, tier, rng);
    }

    // Choose a base kind in the chosen slot.
    switch (slot) {
        case EquipSlot::MeleeWeapon: {
            // Prefer an existing melee weapon if provided.
            ItemKind prefer = ItemKind::Dagger;
            bool hasPrefer = false;
            if (isMeleeWeapon(ia.kind)) { prefer = ia.kind; hasPrefer = true; }
            else if (isMeleeWeapon(ib.kind)) { prefer = ib.kind; hasPrefer = true; }

            ItemKind base = ItemKind::Dagger;
            if (tier >= 10) base = ItemKind::Axe;
            else if (tier >= 6) base = ItemKind::Sword;
            else base = ItemKind::Dagger;

            if (hasPrefer) {
                // Reforge usually preserves the weapon type, with occasional upgrades.
                base = prefer;
                if (prefer == ItemKind::Dagger && tier >= 6 && rng.range(0, 99) < 55) base = ItemKind::Sword;
                if (prefer != ItemKind::Axe && tier >= 10 && rng.range(0, 99) < 45) base = ItemKind::Axe;
            } else {
                // Theme nudges.
                if (hasAnyTagPair2(a, b, "STONE", "SHIELD") && tier >= 8 && rng.range(0, 99) < 35) base = ItemKind::Axe;
            }
            return base;
        }
        case EquipSlot::Armor: {
            ItemKind prefer = ItemKind::LeatherArmor;
            bool hasPrefer = false;
            if (isArmor(ia.kind)) { prefer = ia.kind; hasPrefer = true; }
            else if (isArmor(ib.kind)) { prefer = ib.kind; hasPrefer = true; }

            ItemKind base = ItemKind::LeatherArmor;
            if (tier >= 10) base = ItemKind::PlateArmor;
            else if (tier >= 6) base = ItemKind::ChainArmor;
            else base = ItemKind::LeatherArmor;

            if (hasPrefer) {
                base = prefer;
                if (prefer == ItemKind::LeatherArmor && tier >= 6 && rng.range(0, 99) < 55) base = ItemKind::ChainArmor;
                if (prefer != ItemKind::PlateArmor && tier >= 10 && rng.range(0, 99) < 45) base = ItemKind::PlateArmor;
            }
            return base;
        }
        case EquipSlot::Ring: {
            // Prefer an existing ring if provided.
            if (isRingKind(ia.kind) && rng.range(0, 99) < 70) return ia.kind;
            if (isRingKind(ib.kind) && rng.range(0, 99) < 70) return ib.kind;
            return pickRingKindForTags(a, b, tier, rng);
        }
        case EquipSlot::RangedWeapon: {
            // Prefer existing ranged if provided.
            if (isRangedWeapon(ia.kind) && rng.range(0, 99) < 70) return ia.kind;
            if (isRangedWeapon(ib.kind) && rng.range(0, 99) < 70) return ib.kind;
            return pickRangedKindForTags(a, b, tier, rng);
        }
        default:
            break;
    }

    return pickConsumableResultKind(a, b, tier, rng);
}

inline int targetEnchantForTier(int tier) {
    if (tier >= 10) return 3;
    if (tier >= 7) return 2;
    if (tier >= 4) return 1;
    if (tier <= 0) return -2;
    if (tier <= 1) return -1;
    return 0;
}

inline int rollCraftedGearEnchant(const Item& ia, const Item& ib, ItemKind outKind, int tier, RNG& rng) {
    const EquipSlot outSlot = equipSlot(outKind);
    int sum = 0;
    int n = 0;

    if (isWearableGear(ia.kind) && equipSlot(ia.kind) == outSlot) { sum += ia.enchant; ++n; }
    if (isWearableGear(ib.kind) && equipSlot(ib.kind) == outSlot) { sum += ib.enchant; ++n; }

    const int inherited = (n > 0) ? (sum / n) : 0;
    const int target = targetEnchantForTier(tier);

    int e = (n > 0) ? ((inherited + target) / 2) : target;

    // Small deterministic jitter.
    if (tier >= 8) e += rng.range(0, 1);
    else if (tier <= 1) e += rng.range(-1, 0);
    else e += rng.range(-1, 1);

    return clampi(e, -2, 3);
}

// -----------------------------------------------------------------------------
// Recipe sigils (UI flavor)
// -----------------------------------------------------------------------------

inline std::string sigilName(uint32_t recipeSeed) {
    // Keep sigils short, readable, and deterministic.
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

inline Outcome craft(uint32_t runSeed, const Item& a0, const Item& b0) {
    const Essence ea = essenceFor(a0);
    const Essence eb = essenceFor(b0);

    Outcome o;

    // Special-case: Essence Shard refinement.
    // Combining two shards of the same tag produces a higher-tier shard.
    //
    // This creates a small, deterministic progression loop for crafting byproducts
    // (and trap salvage) without changing the save format.
    if (a0.kind == ItemKind::EssenceShard && b0.kind == ItemKind::EssenceShard) {
        // Only refine when both shards share a non-empty tag. Mixed-tag shards
        // still fall through to normal crafting for interesting outcomes.
        if (!ea.tag.empty() && ea.tag == eb.tag) {
            const int outTier = clampi(std::max(ea.tier, eb.tier) + 1, 1, 12);

            // Build a deterministic recipe seed for this refinement so it can
            // still be journaled as a sigil like other crafts.
            const uint32_t rs = recipeSeed(runSeed, a0, b0, ea, eb, outTier);

            bool outShiny = false;
            if (ea.shiny && eb.shiny) {
                outShiny = true;
            } else {
                int shinyChance = 6 + outTier * 2; // 8..30-ish
                if (ea.shiny || eb.shiny) shinyChance += 18;
                if (outTier >= 10) shinyChance += 8;
                shinyChance = clampi(shinyChance, 0, 100);

                const uint32_t hShiny = hash32(rs ^ 0x51A7D00Du);
                outShiny = ((hShiny % 100u) < static_cast<uint32_t>(shinyChance));
            }

            const crafttags::Tag tg = crafttags::tagFromToken(ea.tag);
            const int tagId = crafttags::tagIndex(tg);

            Item shard;
            shard.kind = ItemKind::EssenceShard;
            shard.count = 1;
            shard.charges = 0;
            shard.enchant = packEssenceShardEnchant(tagId, outTier, outShiny);
            shard.buc = 0;
            shard.ego = ItemEgo::None;
            shard.spriteSeed = hash32(rs ^ 0xE55E1234u) ^ (static_cast<uint32_t>(tagId) * 0x9E3779B9u);

            o.out = shard;
            o.tagA = ea.tag;
            o.tagB = eb.tag;
            o.tier = outTier;
            o.hasByproduct = false;
            return o;
        }
    }

    // Special-case: Essence Shard infusion.
    // Combining an Essence Shard with wearable gear upgrades that gear in a
    // deterministic way, preserving the base item kind.
    //
    // This gives Essence Shards a reliable use-case beyond rolling entirely new
    // items, while keeping outcomes deterministic and order-independent.
    const bool shardInfuse = (a0.kind == ItemKind::EssenceShard && isWearableGear(b0.kind)) ||
                             (b0.kind == ItemKind::EssenceShard && isWearableGear(a0.kind));

    if (shardInfuse) {
        const Item& gearIn  = (a0.kind == ItemKind::EssenceShard) ? b0 : a0;
        const Essence es = (a0.kind == ItemKind::EssenceShard) ? ea : eb;
        const Essence eg = (a0.kind == ItemKind::EssenceShard) ? eb : ea;

        // Infusion tier biases toward the shard's tier, but respects existing gear.
        int itier = (es.tier + eg.tier + 1) / 2;
        if (es.shiny) itier += 1;
        o.tier = clampTier(itier);

        const uint32_t rs = recipeSeed(runSeed, a0, b0, ea, eb, o.tier);

        auto rollPct = [&](uint32_t salt, int pct) -> bool {
            const int p = clampi(pct, 0, 100);
            const uint32_t h = hash32(rs ^ salt);
            return (h % 100u) < static_cast<uint32_t>(p);
        };

        // Base output: keep the gear kind and core identity.
        Item out = gearIn;
        out.count = 1;

        // Deterministic upgrade magnitude from shard tier and "shiny" status.
        int boost = 0;
        if (es.tier >= 10) boost = 2;
        else if (es.tier >= 6) boost = 1;
        if (es.shiny) boost += 1;
        if (!es.tag.empty() && es.tag == eg.tag) boost += 1;
        boost = clampi(boost, 0, 3);

        // Purify/cleanse: certain essences are better at stripping curses.
        if (out.buc < 0) {
            const bool canCleanse = es.shiny || hasAnyTagPair3(es.tag, eg.tag, "AURORA", "CLARITY", "LUCK");
            if (canCleanse) {
                int pct = 20 + es.tier * 6;
                if (es.shiny) pct += 20;
                if (hasTagPair(es.tag, eg.tag, "AURORA")) pct += 10;
                pct = clampi(pct, 0, 100);
                if (rollPct(0x1E55E001u, pct)) out.buc = 0;
            }
        } else if (out.buc == 0) {
            const bool canBless = es.shiny || hasAnyTagPair2(es.tag, eg.tag, "LUCK", "AURORA");
            if (canBless) {
                int pct = 6 + es.tier * 3 + boost * 4;
                if (es.shiny) pct += 10;
                pct = clampi(pct, 0, 60);
                if (rollPct(0x1E55EB1Eu, pct)) out.buc = 1;
            }
        }

        // Apply the actual "upgrade".
        if (isWandKind(out.kind)) {
            const ItemDef& d = itemDef(out.kind);
            const int maxC = std::max(1, d.maxCharges);
            int cur = out.charges;
            if (cur <= 0) cur = std::max(1, maxC / 2);
            cur = clampi(cur, 1, maxC);

            int delta = boost;
            // Arc/Rune/Ember/Stone essences resonate with charged implements.
            if (hasAnyTagPair3(es.tag, eg.tag, "ARC", "RUNE", "EMBER")) delta += 1;
            if (hasTagPair(es.tag, eg.tag, "STONE")) delta += 1;
            delta = clampi(delta, 0, 4);

            out.charges = clampi(cur + delta, 1, maxC);
        } else {
            // Weapons/armor/rings: improve enchantment (never decreases here).
            int delta = boost;
            if (es.tier >= 9 && rollPct(0x1E55E99Eu, 35)) delta += 1;
            delta = clampi(delta, 0, 4);
            out.enchant = clampi(out.enchant + delta, -3, 6);

            // Melee weapons: allow deterministic ego infusion at higher shard tiers.
            if (canHaveMeleeEgoCraft(out.kind) && !itemIsArtifact(out) && out.ego == ItemEgo::None) {
                if (!es.tag.empty() && es.tier >= 5) {
                    RNG rng(rs ^ 0xA11CE5E1u);
                    const ItemEgo want = desiredMeleeEgoForTags(es.tag, eg.tag, rng);
                    int pct = 30 + es.tier * 5;
                    if (es.shiny) pct += 10;
                    if (want != ItemEgo::None && rollPct(0x1E55E600u, pct)) {
                        out.ego = want;
                    }
                }
            }
        }

        // Keep artifact identities stable; otherwise give the infused item a new procedural seed.
        if (!itemIsArtifact(out)) {
            out.spriteSeed = rs;
        }

        // Order-independent tag pair for journaling/selection.
        const bool swapTags = (ea.tag > eb.tag) || (ea.tag == eb.tag && ingredientFingerprint(a0) > ingredientFingerprint(b0));
        o.tagA = swapTags ? eb.tag : ea.tag;
        o.tagB = swapTags ? ea.tag : eb.tag;

        o.out = out;
        o.hasByproduct = false;
        return o;
    }

    // Combine tiers; shiny ingredients slightly bias up.
    int t = (ea.tier + eb.tier + 1) / 2;
    if (ea.shiny) t += 1;
    if (eb.shiny) t += 1;
    o.tier = clampTier(t);

    const uint32_t rs = recipeSeed(runSeed, a0, b0, ea, eb, o.tier);
    RNG rng(rs);

    // Order-independent tag pair for selection.
    const bool swap = (ea.tag > eb.tag) || (ea.tag == eb.tag && ingredientFingerprint(a0) > ingredientFingerprint(b0));
    const std::string lo = swap ? eb.tag : ea.tag;
    const std::string hi = swap ? ea.tag : eb.tag;

    o.tagA = lo;
    o.tagB = hi;

    // Decide whether this is a consumable craft or a forge.
    const bool forgeMode = isWearableGear(a0.kind) || isWearableGear(b0.kind) || isGearMaterialKind(a0.kind) || isGearMaterialKind(b0.kind);

    Item out;
    out.kind = forgeMode ? pickGearResultKind(a0, b0, lo, hi, o.tier, rng)
                         : pickConsumableResultKind(lo, hi, o.tier, rng);
    out.count = 1;
    out.charges = 0;
    out.enchant = 0;
    out.buc = 0;
    out.spriteSeed = rs;
    out.ego = ItemEgo::None;
    // flags and id are assigned by the caller.

    // Bless/curse tuning: higher tier slightly increases odds of blessing.
    const int shinyBonus = (ea.shiny ? 4 : 0) + (eb.shiny ? 4 : 0);
    const int ingBias = (a0.buc < 0 ? -1 : (a0.buc > 0 ? 1 : 0)) + (b0.buc < 0 ? -1 : (b0.buc > 0 ? 1 : 0));

    int blessChance = clampi(3 + o.tier * 3 + shinyBonus + (ingBias > 0 ? 8 : 0), 0, 45);
    int curseChance = clampi(6 - o.tier + (ingBias < 0 ? 8 : 0), 0, 22);

    const int bucRoll = rng.range(0, 99);
    if (bucRoll < blessChance) out.buc = 1;
    else if (bucRoll >= 100 - curseChance) out.buc = -1;

    // Forge outputs: add enchant/ego/artifact logic.
    if (forgeMode && isWearableGear(out.kind)) {
        // Base enchant from tier + inherited enchant from same-slot ingredients.
        out.enchant = rollCraftedGearEnchant(a0, b0, out.kind, o.tier, rng);

        // Ego infusion (melee only).
        if (canHaveMeleeEgoCraft(out.kind) && o.tier >= 4) {
            const ItemEgo desired = desiredMeleeEgoForTags(lo, hi, rng);
            if (desired != ItemEgo::None) {
                float egoChance = 0.10f + 0.03f * std::max(0, o.tier - 4);
                if (!lo.empty() && lo == hi) egoChance += 0.22f;
                if (ea.shiny) egoChance += 0.05f;
                if (eb.shiny) egoChance += 0.05f;
                if (a0.ego != ItemEgo::None || b0.ego != ItemEgo::None) egoChance += 0.10f;
                egoChance = clampf(egoChance, 0.0f, 0.75f);

                if (rng.chance(egoChance)) {
                    out.ego = desired;
                    if (out.enchant < 1) out.enchant = 1;
                }
            }
        }

        // Artifact forging (any wearable gear except wands).
        if (canBeArtifactCraft(out.kind) && o.tier >= 8) {
            float artChance = 0.04f + 0.02f * std::max(0, o.tier - 8); // 4% @8 -> 12% @12
            if (ea.shiny) artChance += 0.03f;
            if (eb.shiny) artChance += 0.03f;
            if (itemIsArtifact(a0) || itemIsArtifact(b0)) artChance += 0.25f; // salvage/reforge risk
            if (a0.ego != ItemEgo::None || b0.ego != ItemEgo::None) artChance += 0.06f;
            artChance = clampf(artChance, 0.0f, 0.60f);

            if (rng.chance(artChance)) {
                setItemArtifact(out, true);
                out.ego = ItemEgo::None;
                if (out.enchant < 1) out.enchant = 1;

                const auto desiredP = desiredArtifactPowerForTags(lo, hi, rng);
                out.spriteSeed = tuneArtifactSeedForPower(out.spriteSeed, out.kind, desiredP);
            }
        }
    }

    // Stackable outputs can sometimes produce 2 when tier is high.
    if (isStackable(out.kind) && o.tier >= 7 && rng.range(0, 99) < 20) {
        out.count = 2;
    }

    // Wands: roll charges.
    if (isWandKind(out.kind)) {
        const ItemDef& d = itemDef(out.kind);
        const int maxC = std::max(1, d.maxCharges);
        const int base = clampi(1 + (o.tier / 3), 1, maxC);
        out.charges = clampi(rng.range(std::max(1, base - 1), std::min(maxC, base + 1)), 1, maxC);
    }

    // Rune tablets: assign a deterministic proc-spell id.
    if (out.kind == ItemKind::RuneTablet) {
        const uint8_t pt = static_cast<uint8_t>(clampi(1 + (o.tier / 1), 1, 15));
        const uint32_t seed28 = rng.nextU32() & PROC_SPELL_SEED_MASK;
        out.spriteSeed = makeProcSpellId(pt, seed28);
        out.enchant = 0;
        out.charges = 0;
        out.buc = 0;
    }

    // Optional deterministic byproduct: an Essence Shard capturing the craft's dominant tag.
    //
    // IMPORTANT: this uses hash32(rs ^ salt) and does NOT consume from the main RNG stream,
    // which keeps legacy craft results (kinds/enchants/egos) stable across patches.
    if (o.tier >= 4) {
        std::string shardTag;
        if (!ea.tag.empty() || !eb.tag.empty()) {
            if (!ea.tag.empty() && eb.tag.empty()) shardTag = ea.tag;
            else if (ea.tag.empty() && !eb.tag.empty()) shardTag = eb.tag;
            else if (ea.tag == eb.tag) shardTag = ea.tag;
            else {
                if (ea.tier > eb.tier) shardTag = ea.tag;
                else if (eb.tier > ea.tier) shardTag = eb.tag;
                else shardTag = ((hash32(rs ^ 0xA11C0C0Au) & 1u) ? ea.tag : eb.tag);
            }

            const crafttags::Tag tg = crafttags::tagFromToken(shardTag);
            const int tagId = crafttags::tagIndex(tg);
            if (tg != crafttags::Tag::None) {
                int chance = 8 + 2 * o.tier;
                if (!ea.tag.empty() && ea.tag == eb.tag) chance += 12;
                chance += (ea.shiny ? 4 : 0) + (eb.shiny ? 4 : 0);
                if (forgeMode) chance = (chance * 65) / 100;
                chance = clampi(chance, 0, 55);

                const uint32_t hDrop = hash32(rs ^ 0xE55E5A9Du);
                if ((hDrop % 100u) < static_cast<uint32_t>(chance)) {
                    int shardTier = clampi((o.tier + std::max(ea.tier, eb.tier) + 1) / 2, 1, 12);
                    if (!ea.tag.empty() && ea.tag == eb.tag) shardTier = clampi(shardTier + 1, 1, 12);

                    int shinyChance = 4;
                    shinyChance += (ea.shiny ? 16 : 0);
                    shinyChance += (eb.shiny ? 16 : 0);
                    if (!ea.tag.empty() && ea.tag == eb.tag) shinyChance += 8;
                    if (o.tier >= 10) shinyChance += 8;
                    if (ea.shiny && eb.shiny) shinyChance = 100;
                    shinyChance = clampi(shinyChance, 0, 100);

                    const uint32_t hShiny = hash32(rs ^ 0x51A7D00Du);
                    const bool shardShiny = ((hShiny % 100u) < static_cast<uint32_t>(shinyChance));

                    int shardCount = 1;
                    if (o.tier >= 10 && !ea.tag.empty() && ea.tag == eb.tag) {
                        const uint32_t hCount = hash32(rs ^ 0xC0FFEE21u);
                        if ((hCount % 100u) < 35u) shardCount = 2;
                    }

                    Item shard;
                    shard.kind = ItemKind::EssenceShard;
                    shard.count = shardCount;
                    shard.charges = 0;
                    shard.enchant = packEssenceShardEnchant(tagId, shardTier, shardShiny);
                    shard.buc = 0;
                    shard.spriteSeed = hash32(rs ^ 0x5EED1234u) ^ (static_cast<uint32_t>(tagId) * 0x9E3779B9u);
                    shard.ego = ItemEgo::None;

                    o.hasByproduct = true;
                    o.byproduct = shard;
                }
            }
        }
    }

    o.out = out;
    return o;
}

} // namespace craftgen
