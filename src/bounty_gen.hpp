#pragma once

// Procedural bounty contract generator.
//
// Goals:
// - Provide run direction ("kill X of Y") with a tangible reward.
// - Keep save compatibility: store bounty state in Item::charges/enchant.
// - Deterministic per-contract (seeded by Item::spriteSeed).
// - Header-only to keep build integration simple.

#include "game.hpp"   // EntityKind + entityKindName
#include "items.hpp"  // ItemKind + pack helpers
#include "rng.hpp"    // hash32/hashCombine

#include <array>
#include <cstdint>
#include <string>

namespace bountygen {

// A compact human-ish codename for the contract.
// This is purely flavor (used in the item name).
inline std::string codename(uint32_t seed) {
    static constexpr std::array<const char*, 16> ADJ = {
        "SILENT", "CRIMSON", "IVORY", "ASHEN",
        "OBSIDIAN", "HOLLOW", "GILDED", "FROSTED",
        "RADIANT", "GRIM", "WICKED", "CELESTIAL",
        "EMBER", "SABLE", "STARFORGED", "ECHOING",
    };
    static constexpr std::array<const char*, 16> NOUN = {
        "FANG", "OATH", "VEIL", "REQUIEM",
        "SPIRAL", "CROWN", "BANE", "WARD",
        "AURORA", "MIRROR", "SIGIL", "LANTERN",
        "BULWARK", "WHISPER", "ECLIPSE", "GLORY",
    };

    uint32_t s = hash32(seed ^ 0xB01D'B01Du);
    const uint32_t a = s & 15u;
    s = hash32(s ^ 0x1234ABCDu);
    const uint32_t b = s & 15u;

    std::string out;
    out.reserve(32);
    out += ADJ[a];
    out += " ";
    out += NOUN[b];
    return out;
}

// Turn a "depth hint" (e.g., max depth reached) into a small bounty tier.
inline int tierFromDepthHint(int depthHint) {
    // depthHint is 0 for camp; 1.. for main dungeon.
    // We want early tiers to stick around for a while.
    const int d = std::max(0, depthHint);
    if (d <= 2) return 1;
    if (d <= 5) return 2;
    if (d <= 9) return 3;
    if (d <= 13) return 4;
    return 5;
}

inline EntityKind pickTarget(uint32_t seed, int tier) {
    // Exclude Player/Shopkeeper/Guard/Dog: bounties should point outward.
    // We keep targets relatively common per tier.
    static constexpr std::array<EntityKind, 6> T1 = {
        EntityKind::Goblin, EntityKind::Bat, EntityKind::Slime,
        EntityKind::KoboldSlinger, EntityKind::Snake, EntityKind::Spider
    };
    static constexpr std::array<EntityKind, 6> T2 = {
        EntityKind::Orc, EntityKind::Wolf, EntityKind::Snake,
        EntityKind::Spider, EntityKind::SkeletonArcher, EntityKind::Leprechaun
    };
    static constexpr std::array<EntityKind, 6> T3 = {
        EntityKind::Troll, EntityKind::Ogre, EntityKind::Wizard,
        EntityKind::Mimic, EntityKind::Zombie, EntityKind::Nymph
    };
    static constexpr std::array<EntityKind, 4> T4 = {
        EntityKind::Wizard, EntityKind::Ogre, EntityKind::Mimic, EntityKind::Minotaur
    };
    static constexpr std::array<EntityKind, 4> T5 = {
        EntityKind::Minotaur, EntityKind::Wizard, EntityKind::Ogre, EntityKind::Mimic
    };

    uint32_t s = hash32(seed ^ 0xCAFE1234u);
    const int t = std::clamp(tier, 1, 5);
    if (t == 1) return T1[s % T1.size()];
    if (t == 2) return T2[s % T2.size()];
    if (t == 3) return T3[s % T3.size()];
    if (t == 4) return T4[s % T4.size()];
    return T5[s % T5.size()];
}

inline int pickRequiredKills(uint32_t seed, int tier, EntityKind target) {
    // Slightly higher requirements for sturdier targets.
    int base = 3 + tier * 2;
    if (target == EntityKind::Bat || target == EntityKind::Slime) base -= 1;
    if (target == EntityKind::Troll || target == EntityKind::Ogre) base += 2;
    if (target == EntityKind::Wizard || target == EntityKind::Minotaur) base += 3;

    uint32_t s = hash32(seed ^ 0xB0B0B0B0u);
    base += static_cast<int>(s % 3u); // +0..2

    // Clamp to something that fits in the UI and our 8-bit progress storage.
    return std::clamp(base, 2, 18);
}

inline ItemKind pickRewardKind(uint32_t seed, int tier) {
    // Keep rewards meaningful but not run-breaking; tier scales up.
    static constexpr std::array<ItemKind, 8> R1 = {
        ItemKind::Gold, ItemKind::PotionHealing, ItemKind::ScrollMapping, ItemKind::FoodRation,
        ItemKind::ScrollIdentify, ItemKind::PotionEnergy, ItemKind::ScrollDetectTraps, ItemKind::PotionShielding
    };
    static constexpr std::array<ItemKind, 8> R2 = {
        ItemKind::Gold, ItemKind::ScrollEnchantWeapon, ItemKind::ScrollEnchantArmor, ItemKind::PotionHaste,
        ItemKind::RingProtection, ItemKind::CaptureSphere, ItemKind::PotionRegeneration, ItemKind::ScrollRemoveCurse
    };
    static constexpr std::array<ItemKind, 8> R3 = {
        ItemKind::Gold, ItemKind::MegaSphere, ItemKind::ScrollEnchantRing, ItemKind::WandFireball,
        ItemKind::RingFocus, ItemKind::RingMight, ItemKind::PotionLevitation, ItemKind::ScrollEarth
    };
    static constexpr std::array<ItemKind, 6> R4 = {
        ItemKind::Gold, ItemKind::WandFireball, ItemKind::MegaSphereFull, ItemKind::RingSearching,
        ItemKind::RingSustenance, ItemKind::PotionInvisibility
    };
    static constexpr std::array<ItemKind, 6> R5 = {
        ItemKind::Gold, ItemKind::WandFireball, ItemKind::MegaSphereFull, ItemKind::RingSearching,
        ItemKind::ScrollEnchantRing, ItemKind::PotionInvisibility
    };

    uint32_t s = hash32(seed ^ 0xDEC0DEu);
    const int t = std::clamp(tier, 1, 5);
    if (t == 1) return R1[s % R1.size()];
    if (t == 2) return R2[s % R2.size()];
    if (t == 3) return R3[s % R3.size()];
    if (t == 4) return R4[s % R4.size()];
    return R5[s % R5.size()];
}

inline int pickRewardCount(uint32_t seed, int tier, ItemKind rewardKind) {
    uint32_t s = hash32(seed ^ 0x00DDBB11u);
    const int t = std::clamp(tier, 1, 5);

    if (rewardKind == ItemKind::Gold) {
        // Gold payout scales with tier.
        const int base = 45 + t * 35;
        const int jitter = static_cast<int>(s % 31u); // 0..30
        return std::clamp(base + jitter, 20, 240);
    }

    // Stackables: small bundles.
    if (isStackable(rewardKind)) {
        int c = 1;
        if (t <= 1) c = 1 + static_cast<int>(s % 2u); // 1..2
        else if (t == 2) c = 1 + static_cast<int>(s % 2u); // 1..2
        else if (t == 3) c = 1 + static_cast<int>(s % 3u); // 1..3
        else c = 1 + static_cast<int>(s % 2u); // 1..2
        return std::clamp(c, 1, 6);
    }

    // Non-stackables: always 1 (counts are handled elsewhere via enchant/charges).
    return 1;
}

// Produces a ready-to-store Item::charges payload for a new bounty contract.
inline int makeCharges(uint32_t spriteSeed, int depthHint) {
    const int tier = tierFromDepthHint(depthHint);
    const EntityKind target = pickTarget(spriteSeed, tier);
    const int req = pickRequiredKills(spriteSeed, tier, target);

    const ItemKind rewardK = pickRewardKind(spriteSeed, tier);
    const int rewardCount = pickRewardCount(spriteSeed, tier, rewardK);

    return packBountyCharges(
        static_cast<int>(target),
        req,
        static_cast<int>(rewardK),
        rewardCount
    );
}

// Display helper: pluralize entity name for bounty objective.
inline std::string pluralizeEntityName(EntityKind k, int count) {
    std::string n = entityKindName(k);
    if (count <= 1) return n;

    // Special cases for a nicer read.
    if (k == EntityKind::Wolf) return "WOLVES";

    // Pluralize the last word for multi-word names.
    const size_t sp = n.find_last_of(' ');
    if (sp != std::string::npos && sp + 1 < n.size()) {
        std::string head = n.substr(0, sp + 1);
        std::string tail = n.substr(sp + 1);
        if (!tail.empty() && tail.back() != 'S') tail.push_back('S');
        return head + tail;
    }

    if (!n.empty() && n.back() != 'S') n.push_back('S');
    return n;
}

} // namespace bountygen
