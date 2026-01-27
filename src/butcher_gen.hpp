#pragma once
#include "items.hpp"
#include "rng.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace butchergen {

// Procedural corpse butchering
//
// Deterministic per-corpse (seeded by the caller). Converts a corpse ItemKind
// into meat/hide/bone yields plus metadata to pack into Item::enchant.
//
// Key design goals:
// - Fully deterministic from (corpse seed + tool kind) so results are stable.
// - Save-compatible: all data is packed into Item::enchant (and spriteSeed encodes visuals).
// - Gameplay-forward: encourages interesting choices (eat vs craft vs drop).

// Tag token strings intentionally match existing fish/produce tag handling
// (REGEN/HASTE/SHIELD/AURORA/CLARITY/VENOM/EMBER).
enum class Tag : uint8_t {
    None = 0,
    Regen,
    Haste,
    Shield,
    Aurora,
    Clarity,
    Venom,
    Ember,
};

inline const char* tagToken(Tag t) {
    switch (t) {
        case Tag::Regen:   return "REGEN";
        case Tag::Haste:   return "HASTE";
        case Tag::Shield:  return "SHIELD";
        case Tag::Aurora:  return "AURORA";
        case Tag::Clarity: return "CLARITY";
        case Tag::Venom:   return "VENOM";
        case Tag::Ember:   return "EMBER";
        default:           return "";
    }
}

inline Tag tagFromIndex(int idx) { return static_cast<Tag>(clampi(idx, 0, 7)); }
inline int tagIndex(Tag t) { return static_cast<int>(t); }

enum class Cut : uint8_t {
    Steak = 0,
    Rib,
    Haunch,
    Strip,
    Chunk,
    Slab,
    Cutlet,
    Tenderloin,
    Organ,
    Filet,
    Shank,
    Roast,
    Belly,
    Tongue,
    Heart,
    Loin,
};

inline const char* cutToken(Cut c) {
    switch (c) {
        case Cut::Steak:      return "STEAK";
        case Cut::Rib:        return "RIB";
        case Cut::Haunch:     return "HAUNCH";
        case Cut::Strip:      return "STRIP";
        case Cut::Chunk:      return "CHUNK";
        case Cut::Slab:       return "SLAB";
        case Cut::Cutlet:     return "CUTLET";
        case Cut::Tenderloin: return "TENDERLOIN";
        case Cut::Organ:      return "ORGAN";
        case Cut::Filet:      return "FILET";
        case Cut::Shank:      return "SHANK";
        case Cut::Roast:      return "ROAST";
        case Cut::Belly:      return "BELLY";
        case Cut::Tongue:     return "TONGUE";
        case Cut::Heart:      return "HEART";
        case Cut::Loin:       return "LOIN";
        default:              return "CUT";
    }
}

inline const char* cutTokenPlural(Cut c) {
    // A small handful of irregular plurals; most take "S".
    switch (c) {
        case Cut::Rib:    return "RIBS";
        case Cut::Haunch: return "HAUNCHES";
        case Cut::Belly:  return "BELLIES";
        default:          break;
    }
    // Fallback: token + "S" is fine for our set.
    // (Callers can special-case if they care.)
    return nullptr;
}

inline Cut cutFromIndex(int idx) { return static_cast<Cut>(clampi(idx, 0, 15)); }
inline int cutIndex(Cut c) { return static_cast<int>(c); }

// Material variants for butchered hide/bone outputs.
// These values are stored in Item::enchant bits 8..15 via packButcherMaterialEnchant.
// IMPORTANT: Keep values stable once shipped (save compatibility).
enum class HideType : uint8_t {
    Hide = 0,       // default legacy "HIDE" (leather-like)
    Pelt = 1,       // furry
    Scales = 2,     // reptilian scales
    Chitin = 3,     // insectoid plates
    MimicSkin = 4,  // weird adhesive skin
    RobeScraps = 5, // cloth scraps (wizard)
};

inline HideType hideTypeFromIndex(int idx) { return static_cast<HideType>(clampi(idx, 0, 5)); }
inline int hideTypeIndex(HideType t) { return static_cast<int>(t); }

inline const char* hideTokenSingular(HideType t) {
    switch (t) {
        case HideType::Pelt:      return "PELT";
        case HideType::Scales:    return "SCALE";
        case HideType::Chitin:    return "CHITIN";
        case HideType::MimicSkin: return "SKIN";
        case HideType::RobeScraps:return "ROBE SCRAP";
        default:                 return "HIDE";
    }
}

inline const char* hideTokenPlural(HideType t) {
    switch (t) {
        case HideType::Pelt:      return "PELTS";
        case HideType::Scales:    return "SCALES";
        case HideType::Chitin:    return "CHITIN";
        case HideType::MimicSkin: return "SKINS";
        case HideType::RobeScraps:return "ROBE SCRAPS";
        default:                 return "HIDES";
    }
}

enum class BoneType : uint8_t {
    Bones = 0,        // default legacy bones
    Horn = 1,
    Fang = 2,
    ChitinShard = 3,
    Tooth = 4,
};

inline BoneType boneTypeFromIndex(int idx) { return static_cast<BoneType>(clampi(idx, 0, 4)); }
inline int boneTypeIndex(BoneType t) { return static_cast<int>(t); }

inline const char* boneTokenSingular(BoneType t) {
    switch (t) {
        case BoneType::Horn:        return "HORN";
        case BoneType::Fang:        return "FANG";
        case BoneType::ChitinShard: return "CHITIN SHARD";
        case BoneType::Tooth:       return "TOOTH";
        default:                    return "BONE";
    }
}

inline const char* boneTokenPlural(BoneType t) {
    switch (t) {
        case BoneType::Horn:        return "HORNS";
        case BoneType::Fang:        return "FANGS";
        case BoneType::ChitinShard: return "CHITIN SHARDS";
        case BoneType::Tooth:       return "TEETH";
        default:                    return "BONES";
    }
}

inline std::string corpseLabel(ItemKind corpseKind) {
    std::string n = itemDef(corpseKind).name ? itemDef(corpseKind).name : "CORPSE";
    constexpr const char* suf = " CORPSE";
    const std::string s = suf;
    if (n.size() > s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0) {
        n.erase(n.size() - s.size());
    }
    return n;
}

inline Tag defaultTagForCorpse(ItemKind corpseKind) {
    switch (corpseKind) {
        case ItemKind::CorpseBat:      return Tag::Haste;
        case ItemKind::CorpseWolf:     return Tag::Regen;
        case ItemKind::CorpseTroll:    return Tag::Regen;
        case ItemKind::CorpseWizard:   return Tag::Aurora;
        case ItemKind::CorpseSnake:    return Tag::Venom;
        case ItemKind::CorpseSpider:   return Tag::Venom;
        case ItemKind::CorpseSlime:    return Tag::Venom;
        case ItemKind::CorpseOgre:     return Tag::Shield;
        case ItemKind::CorpseMinotaur: return Tag::Shield;
        case ItemKind::CorpseMimic:    return Tag::Clarity;
        default:                       return Tag::None;
    }
}

inline HideType hideTypeForCorpse(ItemKind corpseKind) {
    switch (corpseKind) {
        case ItemKind::CorpseWolf:   return HideType::Pelt;
        case ItemKind::CorpseSnake:  return HideType::Scales;
        case ItemKind::CorpseSpider: return HideType::Chitin;
        case ItemKind::CorpseMimic:  return HideType::MimicSkin;
        case ItemKind::CorpseWizard: return HideType::RobeScraps;
        default:                     return HideType::Hide;
    }
}

inline BoneType boneTypeForCorpse(ItemKind corpseKind) {
    switch (corpseKind) {
        case ItemKind::CorpseMinotaur: return BoneType::Horn;
        case ItemKind::CorpseSnake:    return BoneType::Fang;
        case ItemKind::CorpseSpider:   return BoneType::ChitinShard;
        case ItemKind::CorpseMimic:    return BoneType::Tooth;
        default:                       return BoneType::Bones;
    }
}

inline bool corpseHasHide(ItemKind corpseKind) {
    // We treat "hide" broadly: fur pelts, scales, chitin plates, etc.
    // Slimes have no hide; everything else has *some* recoverable material.
    return corpseKind != ItemKind::CorpseSlime;
}

inline bool corpseHasBones(ItemKind corpseKind) {
    // Slimes have no bones. Spiders yield "chitin shards" instead of true bones.
    return corpseKind != ItemKind::CorpseSlime;
}

inline int baseMeatPiecesForCorpse(ItemKind corpseKind) {
    const int w = itemDef(corpseKind).weight;
    if (w >= 70) return 5;
    if (w >= 55) return 4;
    if (w >= 40) return 3;
    if (w >= 22) return 2;
    return 1;
}

inline int baseHidePiecesForCorpse(ItemKind corpseKind) {
    if (!corpseHasHide(corpseKind)) return 0;

    switch (corpseKind) {
        case ItemKind::CorpseMinotaur: return 4;
        case ItemKind::CorpseMimic:    return 3;
        case ItemKind::CorpseOgre:     return 3;
        case ItemKind::CorpseTroll:    return 2;
        case ItemKind::CorpseWolf:     return 2;
        case ItemKind::CorpseWizard:   return 1;
        case ItemKind::CorpseSnake:    return 1;
        case ItemKind::CorpseSpider:   return 1;
        case ItemKind::CorpseOrc:      return 1;
        case ItemKind::CorpseGoblin:   return 1;
        case ItemKind::CorpseKobold:   return 1;
        default:                       return 1;
    }
}

inline int baseBonePiecesForCorpse(ItemKind corpseKind) {
    if (!corpseHasBones(corpseKind)) return 0;

    // Large bodies yield more salvage.
    const int meat = baseMeatPiecesForCorpse(corpseKind);
    if (meat >= 5) return 3;
    if (meat >= 3) return 2;
    return 1;
}

inline Cut chooseCutForCorpse(ItemKind corpseKind, RNG& rng) {
    const int w = itemDef(corpseKind).weight;
    if (w < 20) {
        constexpr std::array<Cut, 5> kSmall = { Cut::Cutlet, Cut::Strip, Cut::Chunk, Cut::Organ, Cut::Filet };
        return kSmall[rng.range(0, static_cast<int>(kSmall.size()) - 1)];
    }
    if (w < 45) {
        constexpr std::array<Cut, 7> kMed = { Cut::Steak, Cut::Rib, Cut::Haunch, Cut::Strip, Cut::Chunk, Cut::Cutlet, Cut::Organ };
        return kMed[rng.range(0, static_cast<int>(kMed.size()) - 1)];
    }
    constexpr std::array<Cut, 8> kLarge = { Cut::Slab, Cut::Roast, Cut::Haunch, Cut::Steak, Cut::Tenderloin, Cut::Belly, Cut::Shank, Cut::Loin };
    return kLarge[rng.range(0, static_cast<int>(kLarge.size()) - 1)];
}

inline Cut choosePrimeCutForCorpse(ItemKind corpseKind, RNG& rng) {
    // Prime cuts are slightly biased toward "special" tokens (heart/tenderloin/etc).
    (void)corpseKind;
    constexpr std::array<Cut, 7> kPrime = { Cut::Tenderloin, Cut::Filet, Cut::Heart, Cut::Loin, Cut::Belly, Cut::Roast, Cut::Organ };
    return kPrime[rng.range(0, static_cast<int>(kPrime.size()) - 1)];
}

inline int toolPrecision(ItemKind toolKind) {
    // Higher = cleaner cuts and less waste.
    switch (toolKind) {
        case ItemKind::Dagger:  return 2;
        case ItemKind::Sword:   return 1;
        case ItemKind::Axe:     return 0;
        case ItemKind::Pickaxe: return -1;
        default:                return 0;
    }
}

inline int materialQuality(ItemKind corpseKind, int freshnessTurns, int precision, RNG& rng) {
    // Map remaining freshness into a coarse 0..255 quality baseline.
    // (Freshness itself is already decay-time-scaled by corpse weight in spawn logic.)
    const int w = std::max(1, itemDef(corpseKind).weight);
    const int fresh = clampi(freshnessTurns, 0, 380);
    int q = (fresh * 255) / 380;

    // Heavier hides/bones tend to be tougher, but also harder to recover cleanly.
    q += clampi(w * 2, 0, 60);

    // Tool precision helps.
    q += precision * 18;

    // Small deterministic variance.
    q += rng.range(-18, 18);

    // Spoilage punishes quality hard.
    if (freshnessTurns <= 60) q -= 80;
    else if (freshnessTurns <= 160) q -= 25;

    return clampi(q, 0, 255);
}

struct MeatStack {
    int pieces = 0;

    Tag tag = Tag::None;
    Cut cut = Cut::Steak;

    int hungerPerPiece = 0;
    int healPerPiece = 0;
};

struct Yield {
    std::vector<MeatStack> meat;

    int hidePieces = 0;
    HideType hideType = HideType::Hide;
    int hideQuality = 0;

    int bonePieces = 0;
    BoneType boneType = BoneType::Bones;
    int boneQuality = 0;
};

// freshnessTurns mirrors corpse freshness:
//   <= 60: rotten, <= 160: stale, else fresh.
inline Yield generate(ItemKind corpseKind, uint32_t seed, int freshnessTurns, ItemKind toolKind) {
    RNG rng(hash32(seed));

    Yield y;
    const int precision = toolPrecision(toolKind);

    const bool rotten = (freshnessTurns <= 60);
    const bool stale  = (freshnessTurns <= 160);

    // --- Piece counts ---------------------------------------------------------
    int meat = baseMeatPiecesForCorpse(corpseKind) + rng.range(-1, 1);

    // Tool impact: precise tools waste less; crude tools waste more.
    if (!rotten) {
        if (precision >= 2 && (rng.range(1, 2) == 1)) meat += 1;
        if (precision <= -1 && (rng.range(1, 2) == 1)) meat -= 1;
    }
    meat = clampi(meat, 0, 7);

    int hide = corpseHasHide(corpseKind) ? baseHidePiecesForCorpse(corpseKind) + rng.range(-1, 0) : 0;
    int bone = corpseHasBones(corpseKind) ? baseBonePiecesForCorpse(corpseKind) + rng.range(0, 1) : 0;

    // Axes/pickaxes tend to splinter bones; daggers keep hides cleaner.
    if (!rotten) {
        if (toolKind == ItemKind::Axe && bone > 0 && (rng.range(1, 2) == 1)) bone += 1;
        if (toolKind == ItemKind::Pickaxe && bone > 0 && (rng.range(1, 2) == 1)) bone += 1;
        if (toolKind == ItemKind::Pickaxe && hide > 0 && (rng.range(1, 3) == 1)) hide -= 1;
    }

    hide = std::max(0, hide);
    bone = std::max(0, bone);

    if (stale) {
        meat = std::max(0, meat - 1);
        if (hide > 0 && (rng.range(1, 3) == 1)) hide -= 1;
    }

    if (rotten) {
        meat = 0;
        if (hide > 0 && (rng.range(1, 2) == 1)) hide -= 1;
    }

    if (corpseKind == ItemKind::CorpseSlime) {
        hide = 0;
        bone = 0;
        if (!rotten) meat = std::max(1, meat / 2);
    }

    // --- Material variants + quality -----------------------------------------
    y.hidePieces = hide;
    y.bonePieces = bone;

    y.hideType = hideTypeForCorpse(corpseKind);
    y.boneType = boneTypeForCorpse(corpseKind);

    y.hideQuality = (hide > 0) ? materialQuality(corpseKind, freshnessTurns, precision, rng) : 0;
    y.boneQuality = (bone > 0) ? materialQuality(corpseKind, freshnessTurns, precision - 1, rng) : 0; // bones are harder to preserve

    // --- Meat stacks ----------------------------------------------------------
    if (meat <= 0) return y;

    const ItemDef& d = itemDef(corpseKind);
    const int totalH = std::max(1, d.hungerRestore);
    const int totalHeal = std::max(0, d.healAmount);

    // Number of distinct stacks is capped so it doesn't explode the inventory.
    int desiredStacks = 1;
    if (meat >= 5) desiredStacks = 3;
    else if (meat >= 3) desiredStacks = 2;

    // Prime cut logic: tagged corpses yield a small "prime" portion that holds the tag.
    const Tag corpseTag = defaultTagForCorpse(corpseKind);
    int primePieces = 0;
    if (corpseTag != Tag::None && meat > 0) {
        primePieces = 1;
        if (meat >= 5 && (rng.range(1, 2) == 1)) primePieces = 2;
        primePieces = std::min(primePieces, meat);
    }

    int remaining = meat - primePieces;

    // Clamp stacks so each stack has at least 1 piece.
    int stacks = desiredStacks;
    if (primePieces > 0) {
        // One slot reserved for prime.
        stacks = std::max(1, std::min(desiredStacks, 1 + remaining));
    } else {
        stacks = std::max(1, std::min(desiredStacks, remaining));
    }

    const int normalStacks = (primePieces > 0) ? (stacks - 1) : stacks;

    // Decide piece counts for normal stacks.
    std::vector<int> normalCounts;
    if (normalStacks > 0) {
        normalCounts.assign(static_cast<size_t>(normalStacks), 1);
        int left = std::max(0, remaining - normalStacks);
        for (int i = 0; i < left; ++i) {
            const int idx = rng.range(0, normalStacks - 1);
            normalCounts[static_cast<size_t>(idx)] += 1;
        }
    }

    // Allocate total hunger/heal: prime cut gets a larger share (if it exists).
    int primeH = 0;
    int primeHeal = 0;
    if (primePieces > 0) {
        primeH = (totalH * 45) / 100;
        primeHeal = (totalHeal * 70) / 100;

        // Ensure prime isn't completely empty if totals are tiny.
        if (primeH <= 0 && totalH > 0) primeH = std::min(totalH, primePieces);
        if (primeHeal <= 0 && totalHeal > 0 && primePieces > 0) primeHeal = std::min(totalHeal, primePieces);
    }
    const int normalH = std::max(0, totalH - primeH);
    const int normalHeal = std::max(0, totalHeal - primeHeal);

    if (primePieces > 0) {
        MeatStack ms;
        ms.pieces = primePieces;
        ms.tag = corpseTag;
        ms.cut = choosePrimeCutForCorpse(corpseKind, rng);
        ms.hungerPerPiece = clampi((primeH + primePieces - 1) / primePieces, 1, 255);
        ms.healPerPiece = clampi((primeHeal + primePieces - 1) / primePieces, 0, 255);
        y.meat.push_back(ms);
    }

    const int normalPiecesTotal = std::max(1, remaining);

    // Pre-allocate per-stack totals using proportional division.
    int hRem = normalH;
    int healRem = normalHeal;
    for (int i = 0; i < normalStacks; ++i) {
        const int pcs = normalCounts[static_cast<size_t>(i)];
        int allocH = (normalH * pcs) / normalPiecesTotal;
        int allocHeal = (normalHeal * pcs) / normalPiecesTotal;

        // Distribute any rounding leftovers later via hRem/healRem.
        hRem -= allocH;
        healRem -= allocHeal;

        MeatStack ms;
        ms.pieces = pcs;
        ms.tag = Tag::None;
        ms.cut = chooseCutForCorpse(corpseKind, rng);
        ms.hungerPerPiece = clampi((allocH + pcs - 1) / pcs, 1, 255);
        ms.healPerPiece = clampi((allocHeal + pcs - 1) / pcs, 0, 255);
        y.meat.push_back(ms);
    }

    // Rounding leftovers: buff the first stacks a bit (deterministically) so totals don't go missing.
    if (!y.meat.empty() && (hRem > 0 || healRem > 0)) {
        for (size_t i = 0; i < y.meat.size() && (hRem > 0 || healRem > 0); ++i) {
            MeatStack& ms = y.meat[i];
            if (hRem > 0) {
                ms.hungerPerPiece = clampi(ms.hungerPerPiece + 1, 1, 255);
                --hRem;
            }
            if (healRem > 0) {
                ms.healPerPiece = clampi(ms.healPerPiece + 1, 0, 255);
                --healRem;
            }
        }
    }

    // If the corpse is stale/rotten, drop potency.
    if (stale) {
        for (auto& ms : y.meat) {
            ms.hungerPerPiece = std::max(1, (ms.hungerPerPiece * 3) / 4);
            ms.healPerPiece = std::max(0, ms.healPerPiece - 1);
        }
    }
    if (rotten) {
        y.meat.clear();
    }

    return y;
}

} // namespace butchergen
