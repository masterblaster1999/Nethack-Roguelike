#pragma once

#include "craft_tags.hpp"
#include "rng.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Procedural win conditions (Victory Plans)
// -----------------------------------------------------------------------------
//
// This project started as a NetHack-inspired roguelike (the classic run arc is
// "retrieve the Amulet of Yendor and escape"). While that remains a supported
// victory path, PROCROGUE++ can also generate alternate run goals.
//
// IMPORTANT DESIGN GOALS:
// - Win conditions are *derived from the run seed*.
// - They do NOT consume Game::rng (so they do not perturb determinism).
// - They do not require changing save formats.
//
// The Game layer computes progress using live counters (kills, debt, inventory,
// etc.) and checks satisfaction at the camp exit.

namespace victorygen {

// Append-only in practice (not serialized), but keeping stable ids helps tests
// and avoids unnecessary churn.
enum class KeyKind : uint8_t {
    Amulet = 0,        // escape with the Amulet of Yendor
    Gold,              // escape with at least N gold
    EssenceShards,     // escape with N essence shards of a specific tag/tier
    ClearDebt,         // escape with zero shop debt

    // Conduct-style constraints (inspired by NetHack voluntary challenges).
    Pacifist,          // escape with zero DIRECT (player) kills
    Foodless,          // escape having eaten nothing at all
    Vegetarian,        // escape having eaten no corpses (food rations allowed)
    Atheist,           // escape having used no shrine services
    Illiterate,        // escape having read no scrolls or spellbooks

    // Trophy-style objectives (leveraging procedural butchering).
    HideTrophies,      // escape with N hides at or above a quality tier
    BoneTrophies,      // escape with N bones at or above a quality tier

    // Trophy-style objectives (leveraging procedural fishing).
    FishTrophies,      // escape with N trophy fish at or above a rarity tier (optionally tagged)
};

struct KeyReq {
    KeyKind kind = KeyKind::Amulet;

    // Parameter interpretation depends on kind.
    // - Gold:           amount = gold required
    // - EssenceShards:   amount = shard count required
    // - Hide/Bone:       amount = trophy count required
    // - FishTrophies:    amount = fish count required
    int amount = 0;

    // - EssenceShards:   required craft tag
    // - FishTrophies:    required fish bonus tag (subset; can be None for "any")
    crafttags::Tag tag = crafttags::Tag::None;

    // - EssenceShards: minTier = shard minimum tier (0..15)
    // - Hide/Bone:     minTier = minimum quality tier (0..3)
    // - FishTrophies:  minTier = minimum fish rarity (0..4)
    int minTier = 0;
};

struct VictoryPlan {
    uint32_t seed = 1u;      // never 0
    int targetDepth = 1;     // reach at least this deep

    // 1 or 2 key requirements (depth is always required).
    uint8_t reqCount = 1;
    std::array<KeyReq, 2> req{};
};

inline uint32_t victorySeed(uint32_t runSeed) {
    uint32_t s = hashCombine(runSeed, "VICTORY"_tag);
    if (s == 0u) s = 1u;
    return s;
}

inline bool isConductKey(KeyKind k) {
    switch (k) {
        case KeyKind::Pacifist:
        case KeyKind::Foodless:
        case KeyKind::Vegetarian:
        case KeyKind::Atheist:
        case KeyKind::Illiterate:
            return true;
        default:
            return false;
    }
}

inline const char* keyKindName(KeyKind k) {
    switch (k) {
        case KeyKind::Amulet:        return "AMULET";
        case KeyKind::Gold:          return "GOLD";
        case KeyKind::EssenceShards: return "ESSENCE";
        case KeyKind::ClearDebt:     return "DEBT";
        case KeyKind::Pacifist:      return "PACIFIST";
        case KeyKind::Foodless:      return "FOODLESS";
        case KeyKind::Vegetarian:    return "VEGETARIAN";
        case KeyKind::Atheist:       return "ATHEIST";
        case KeyKind::Illiterate:    return "ILLITERATE";
        case KeyKind::HideTrophies:  return "HIDES";
        case KeyKind::BoneTrophies:  return "BONES";
        case KeyKind::FishTrophies:  return "FISH";
        default:                     return "";
    }
}

inline bool requiresAmulet(const VictoryPlan& p) {
    for (int i = 0; i < static_cast<int>(p.reqCount); ++i) {
        if (p.req[static_cast<size_t>(i)].kind == KeyKind::Amulet) return true;
    }
    return false;
}

inline const char* trophyTierLabel(int tier) {
    const int t = std::clamp(tier, 0, 3);
    switch (t) {
        case 0: return "ANY";
        case 1: return "TOUGH+";
        case 2: return "FINE+";
        case 3: return "PRIME+";
        default: return "ANY";
    }
}

inline const char* fishRarityLabel(int minRarity) {
    const int r = std::clamp(minRarity, 0, 4);
    switch (r) {
        case 0: return "ANY";
        case 1: return "UNCOMMON+";
        case 2: return "RARE+";
        case 3: return "EPIC+";
        case 4: return "LEGENDARY";
        default: return "ANY";
    }
}

// Compact label for HUD strings.
inline const char* fishRarityCode(int minRarity) {
    const int r = std::clamp(minRarity, 0, 4);
    switch (r) {
        case 0: return "ANY";
        case 1: return "U+";
        case 2: return "R+";
        case 3: return "E+";
        case 4: return "L";
        default: return "ANY";
    }
}

inline std::string goalLineForReq(const KeyReq& r, bool secondaryLine) {
    // Goal lines are shown in the log as "GOAL: ...".
    // Keep lines short, all-caps, and UI-safe.
    const char* pre = secondaryLine ? "ALSO: " : "";

    switch (r.kind) {
        case KeyKind::Amulet:
            return std::string(pre) + "ESCAPE WITH THE AMULET OF YENDOR.";
        case KeyKind::Gold:
            return std::string(pre) + "ESCAPE WITH AT LEAST " + std::to_string(r.amount) + " GOLD.";
        case KeyKind::EssenceShards: {
            std::string ln = std::string(pre) + "ESCAPE WITH " + std::to_string(r.amount) + " ";
            ln += crafttags::tagToken(r.tag);
            ln += " ESSENCE SHARD";
            if (r.amount != 1) ln += "S";
            if (r.minTier > 0) {
                ln += " (TIER " + std::to_string(r.minTier) + "+)";
            }
            ln += ".";
            return ln;
        }
        case KeyKind::ClearDebt:
            return std::string(pre) + "ESCAPE OWING THE MERCHANT GUILD NOTHING.";
        case KeyKind::Pacifist:
            return std::string(pre) + "ESCAPE WITHOUT DELIVERING A KILLING BLOW.";
        case KeyKind::Foodless:
            return std::string(pre) + "ESCAPE WITHOUT EATING ANYTHING.";
        case KeyKind::Vegetarian:
            return std::string(pre) + "ESCAPE WITHOUT EATING CORPSES.";
        case KeyKind::Atheist:
            return std::string(pre) + "ESCAPE WITHOUT USING SHRINE SERVICES.";
        case KeyKind::Illiterate:
            return std::string(pre) + "ESCAPE WITHOUT READING (SCROLLS OR SPELLBOOKS).";
        case KeyKind::HideTrophies:
            return std::string(pre) + "ESCAPE WITH " + std::to_string(r.amount) + " HIDE TROPHIES (" + trophyTierLabel(r.minTier) + ").";
        case KeyKind::BoneTrophies:
            return std::string(pre) + "ESCAPE WITH " + std::to_string(r.amount) + " BONE TROPHIES (" + trophyTierLabel(r.minTier) + ").";
        case KeyKind::FishTrophies: {
            std::string ln = std::string(pre) + "ESCAPE WITH " + std::to_string(r.amount) + " ";
            if (r.tag != crafttags::Tag::None) {
                ln += crafttags::tagToken(r.tag);
                ln += " ";
            }
            ln += "TROPHY FISH (";
            ln += fishRarityLabel(r.minTier);
            ln += ").";
            return ln;
        }
        default:
            return std::string(pre) + "ESCAPE.";
    }
}

inline KeyReq makeGoldReq(int goldRequired) {
    KeyReq r;
    r.kind = KeyKind::Gold;
    r.amount = std::max(1, goldRequired);
    return r;
}

inline KeyReq makeDebtReq() {
    KeyReq r;
    r.kind = KeyKind::ClearDebt;
    return r;
}

inline KeyReq makeAmuletReq() {
    KeyReq r;
    r.kind = KeyKind::Amulet;
    return r;
}

inline KeyReq makePacifistReq() {
    KeyReq r;
    r.kind = KeyKind::Pacifist;
    return r;
}

inline KeyReq makeFoodlessReq() {
    KeyReq r;
    r.kind = KeyKind::Foodless;
    return r;
}

inline KeyReq makeVegetarianReq() {
    KeyReq r;
    r.kind = KeyKind::Vegetarian;
    return r;
}

inline KeyReq makeAtheistReq() {
    KeyReq r;
    r.kind = KeyKind::Atheist;
    return r;
}

inline KeyReq makeIlliterateReq() {
    KeyReq r;
    r.kind = KeyKind::Illiterate;
    return r;
}

inline KeyReq makeEssenceReq(crafttags::Tag tag, int count, int minTier) {
    KeyReq r;
    r.kind = KeyKind::EssenceShards;
    r.amount = std::max(1, count);
    r.tag = tag;
    r.minTier = std::clamp(minTier, 0, 15);
    return r;
}

inline KeyReq makeHideTrophyReq(int count, int minQualityTier) {
    KeyReq r;
    r.kind = KeyKind::HideTrophies;
    r.amount = std::max(1, count);
    r.minTier = std::clamp(minQualityTier, 0, 3);
    return r;
}

inline KeyReq makeBoneTrophyReq(int count, int minQualityTier) {
    KeyReq r;
    r.kind = KeyKind::BoneTrophies;
    r.amount = std::max(1, count);
    r.minTier = std::clamp(minQualityTier, 0, 3);
    return r;
}

inline KeyReq makeFishTrophyReq(crafttags::Tag tag, int count, int minRarity) {
    KeyReq r;
    r.kind = KeyKind::FishTrophies;
    r.amount = std::max(1, count);
    r.tag = tag;
    r.minTier = std::clamp(minRarity, 0, 4);
    return r;
}

inline KeyReq makeReqForKind(KeyKind k, RNG& rng, int targetDepth, bool secondary) {
    // secondary=true means this requirement is the 2nd clause in a dual-key plan.
    // Secondary clauses are slightly lighter.
    const int scalePct = secondary ? 75 : 100;

    switch (k) {
        case KeyKind::Amulet:
            return makeAmuletReq();
        case KeyKind::ClearDebt:
            return makeDebtReq();
        case KeyKind::Pacifist:
            return makePacifistReq();
        case KeyKind::Foodless:
            return makeFoodlessReq();
        case KeyKind::Vegetarian:
            return makeVegetarianReq();
        case KeyKind::Atheist:
            return makeAtheistReq();
        case KeyKind::Illiterate:
            return makeIlliterateReq();

        case KeyKind::Gold: {
            const int jitter = rng.range(0, 6) * 10; // 0..60
            int req = 80 + targetDepth * 8 + jitter;
            req = (req * scalePct) / 100;
            return makeGoldReq(req);
        }

        case KeyKind::EssenceShards: {
            static constexpr crafttags::Tag kCoreEssenceTags[] = {
                crafttags::Tag::Ember,
                crafttags::Tag::Venom,
                crafttags::Tag::Regen,
                crafttags::Tag::Aurora,
                crafttags::Tag::Stone,
                crafttags::Tag::Rune,
                crafttags::Tag::Clarity,
                crafttags::Tag::Shield,
                crafttags::Tag::Haste,
                crafttags::Tag::Arc,
            };
            const int idx = rng.range(0, static_cast<int>(sizeof(kCoreEssenceTags) / sizeof(kCoreEssenceTags[0])) - 1);
            const crafttags::Tag tag = kCoreEssenceTags[idx];

            int minTier = 0;
            if (targetDepth >= 18) minTier = 1;
            if (targetDepth >= 24) minTier = 2;

            int base = 3 + targetDepth / 8; // 3..6-ish
            int count = base + rng.range(0, 2);

            // Secondary objectives should be lighter.
            if (secondary) {
                count = std::max(1, count - 1);
                if (minTier > 0 && (rng.nextU32() % 3u) == 0u) minTier -= 1;
            }

            return makeEssenceReq(tag, count, minTier);
        }

        case KeyKind::HideTrophies:
        case KeyKind::BoneTrophies: {
            int minTier = 0;
            if (targetDepth >= 14) minTier = 1;
            if (targetDepth >= 20) minTier = 2;
            if (targetDepth >= 25) minTier = 3;

            int count = 2 + targetDepth / 10;
            if (secondary) count = std::max(1, count - 1);
            if (count < 1) count = 1;

            if (k == KeyKind::HideTrophies) return makeHideTrophyReq(count, minTier);
            return makeBoneTrophyReq(count, minTier);
        }

        case KeyKind::FishTrophies: {
            // Fish trophies are tuned to be achievable without requiring extreme RNG.
            // We avoid forcing Legendary and keep counts modest.

            int minR = 1; // Uncommon+
            if (targetDepth >= 14) minR = 2; // Rare+
            if (targetDepth >= 22) minR = 3; // Epic+

            int count = 1;
            if (minR <= 1) count = 3 + rng.range(0, 1);        // 3..4 uncommon+
            else if (minR == 2) count = 2 + rng.range(0, 1);   // 2..3 rare+
            else count = 1 + rng.range(0, 1);                  // 1..2 epic+

            if (secondary) count = std::max(1, count - 1);

            crafttags::Tag tag = crafttags::Tag::None;
            int tagChance = (minR <= 2) ? 35 : 15;
            if (secondary) tagChance = std::max(0, tagChance - 10);

            if (rng.range(0, 99) < tagChance) {
                static constexpr crafttags::Tag kFishTags[] = {
                    crafttags::Tag::Regen,
                    crafttags::Tag::Haste,
                    crafttags::Tag::Shield,
                    crafttags::Tag::Aurora,
                    crafttags::Tag::Clarity,
                    crafttags::Tag::Venom,
                    crafttags::Tag::Ember,
                };
                const int idx = rng.range(0, static_cast<int>(sizeof(kFishTags) / sizeof(kFishTags[0])) - 1);
                tag = kFishTags[idx];

                // Tagging makes the hunt more specific; keep it lighter.
                if (count > 1) count = std::max(1, count - 1);
                if (minR > 1 && (rng.nextU32() % 3u) == 0u) minR -= 1;
            }

            return makeFishTrophyReq(tag, count, minR);
        }

        default:
            return makeAmuletReq();
    }
}

inline bool isHardConduct(KeyKind k) {
    return (k == KeyKind::Pacifist || k == KeyKind::Foodless);
}

inline KeyKind pickPrimaryKey(RNG& rng) {
    // Conservative distribution; classic Amulet remains common.
    const int roll = rng.range(0, 99);

    if (roll < 30) return KeyKind::Amulet;
    if (roll < 48) return KeyKind::Gold;
    if (roll < 66) return KeyKind::EssenceShards;
    if (roll < 76) return KeyKind::ClearDebt;

    // Trophy runs are a little more common than strict conducts.
    if (roll < 83) return (rng.nextU32() & 1u) ? KeyKind::HideTrophies : KeyKind::BoneTrophies;
    if (roll < 88) return KeyKind::FishTrophies;

    if (roll < 92) return KeyKind::Pacifist;
    if (roll < 95) return KeyKind::Foodless;
    if (roll < 97) return KeyKind::Vegetarian;
    if (roll < 99) return KeyKind::Atheist;
    return KeyKind::Illiterate;
}

inline KeyKind pickSecondaryKey(RNG& rng, KeyKind primary) {
    // A curated bank of keys that work well as secondary clauses.
    // Avoid Amulet and avoid very hard conducts as secondaries.
    static constexpr KeyKind kBank[] = {
        KeyKind::Gold,
        KeyKind::EssenceShards,
        KeyKind::ClearDebt,
        KeyKind::Vegetarian,
        KeyKind::Atheist,
        KeyKind::Illiterate,
        KeyKind::HideTrophies,
        KeyKind::BoneTrophies,
        KeyKind::FishTrophies,
    };

    // Deterministically pick until we get a non-duplicate.
    for (int tries = 0; tries < 8; ++tries) {
        const int idx = rng.range(0, static_cast<int>(sizeof(kBank) / sizeof(kBank[0])) - 1);
        const KeyKind k = kBank[idx];
        if (k == primary) continue;

        // Foodless implies vegetarian; if primary is Foodless don't pick Vegetarian.
        if (primary == KeyKind::Foodless && k == KeyKind::Vegetarian) continue;

        return k;
    }

    // Fallback.
    return (primary == KeyKind::Gold) ? KeyKind::ClearDebt : KeyKind::Gold;
}

// Deterministically generate a victory plan for a given run.
//
// dungeonMaxDepth is the campaign max depth (typically 25).
// If infiniteWorldEnabled is true, the generator is allowed to target slightly
// deeper optional goals, but should still keep runs reasonable.
inline VictoryPlan planFor(uint32_t runSeed, int dungeonMaxDepth, bool infiniteWorldEnabled) {
    VictoryPlan p;
    p.seed = victorySeed(runSeed);

    RNG rng(p.seed);

    const KeyKind primary = pickPrimaryKey(rng);

    // Decide whether to add a 2nd clause.
    p.reqCount = 1;
    if (primary != KeyKind::Amulet) {
        int chance = 0;
        if (primary == KeyKind::Gold || primary == KeyKind::EssenceShards) chance = 45;
        else if (primary == KeyKind::ClearDebt || primary == KeyKind::HideTrophies || primary == KeyKind::BoneTrophies || primary == KeyKind::FishTrophies) chance = 35;
        else if (isConductKey(primary)) chance = 20;

        if (rng.range(0, 99) < chance) p.reqCount = 2;
    }

    // Determine a depth target.
    const int cap = infiniteWorldEnabled ? (dungeonMaxDepth + 10) : dungeonMaxDepth;
    int lo = std::max(6, dungeonMaxDepth / 3);
    int hi = std::max(lo, cap);

    // Dual-key plans should be shorter so they remain achievable.
    if (p.reqCount >= 2) {
        hi = std::min(hi, 20);
    }

    // Hard conduct plans are intentionally shorter.
    if (isHardConduct(primary)) {
        hi = std::min(hi, 18);
    }

    p.targetDepth = rng.range(lo, hi);

    // Amulet plans are the "full depth" run.
    if (primary == KeyKind::Amulet) {
        p.targetDepth = dungeonMaxDepth;
        p.req[0] = makeReqForKind(KeyKind::Amulet, rng, p.targetDepth, false);
        return p;
    }

    // Fill primary requirement.
    p.req[0] = makeReqForKind(primary, rng, p.targetDepth, false);

    // Fill secondary requirement (if any).
    if (p.reqCount >= 2) {
        const KeyKind secondary = pickSecondaryKey(rng, primary);
        p.req[1] = makeReqForKind(secondary, rng, p.targetDepth, true);

        // If the secondary clause is itself a hard conduct, nudge depth lower.
        if (isHardConduct(secondary)) {
            p.targetDepth = std::min(p.targetDepth, 18);
        }
    }

    return p;
}

// Human-readable, run-seeded goal lines (no progress numbers).
inline std::vector<std::string> goalLines(const VictoryPlan& p) {
    std::vector<std::string> out;
    out.reserve(4);

    out.push_back("REACH DEPTH " + std::to_string(p.targetDepth) + " (OR DEEPER)." );

    // Requirements.
    for (int i = 0; i < static_cast<int>(p.reqCount); ++i) {
        out.push_back(goalLineForReq(p.req[static_cast<size_t>(i)], i != 0));
    }

    // Remind the player where victory is claimed.
    out.push_back("RETURN TO CAMP AND TAKE THE EXIT.");
    return out;
}

// Short scoreboard-style end cause for a win under this plan.
inline std::string endCauseTag(const VictoryPlan& p) {
    if (requiresAmulet(p)) return "ESCAPED WITH THE AMULET";

    if (p.reqCount <= 1) {
        switch (p.req[0].kind) {
            case KeyKind::Gold:          return "ESCAPED (GOLD RUN)";
            case KeyKind::EssenceShards: return "ESCAPED (ESSENCE RITUAL)";
            case KeyKind::ClearDebt:     return "ESCAPED (DEBT-FREE)";
            case KeyKind::Pacifist:      return "ESCAPED (PACIFIST)";
            case KeyKind::Foodless:      return "ESCAPED (FOODLESS)";
            case KeyKind::Vegetarian:    return "ESCAPED (VEGETARIAN)";
            case KeyKind::Atheist:       return "ESCAPED (ATHEIST)";
            case KeyKind::Illiterate:    return "ESCAPED (ILLITERATE)";
            case KeyKind::HideTrophies:  return "ESCAPED (HIDE TROPHIES)";
            case KeyKind::BoneTrophies:  return "ESCAPED (BONE TROPHIES)";
            case KeyKind::FishTrophies:  return "ESCAPED (FISH TROPHY)";
            default:                     return "ESCAPED";
        }
    }

    // Dual-key summary.
    std::string out = "ESCAPED (";
    out += keyKindName(p.req[0].kind);
    out += "+";
    out += keyKindName(p.req[1].kind);
    out += ")";
    return out;
}

} // namespace victorygen
