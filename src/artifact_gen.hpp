#pragma once

// Procedural artifact helpers.
//
// Artifacts are stored as a bit-flag on Item (no new save fields). Their
// identity is derived deterministically from (spriteSeed, kind, id) and their
// potency scales with enchantment and blessing/curse.

#include "items.hpp"
#include "rng.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace artifactgen {

enum class Power : uint8_t {
    Flame = 0,
    Venom,
    Daze,
    Ward,
    Vitality,
    COUNT
};

inline int bucScalar(const Item& it) {
    if (it.buc < 0) return -1;
    if (it.buc > 0) return 1;
    return 0;
}

// Deterministic seed for an artifact item.
inline uint32_t artifactSeed(const Item& it) {
    uint32_t s = it.spriteSeed;
    if (s == 0u) {
        // Stable-ish fallback for legacy items.
        s = static_cast<uint32_t>(it.id) * 2654435761u;
    }
    s = hashCombine(s ^ 0xA11F00Du, static_cast<uint32_t>(it.kind));
    return hash32(s ^ 0xC0FFEEu);
}

inline bool isArtifactGear(const Item& it) {
    return itemIsArtifact(it) && isWearableGear(it.kind);
}

inline Power artifactPower(const Item& it) {
    constexpr uint32_t n = static_cast<uint32_t>(Power::COUNT);
    const uint32_t h = artifactSeed(it);
    return static_cast<Power>((n == 0u) ? 0u : (h % n));
}

inline const char* powerTag(Power p) {
    switch (p) {
        case Power::Flame:    return "FLAME";
        case Power::Venom:    return "VENOM";
        case Power::Daze:     return "DAZE";
        case Power::Ward:     return "WARD";
        case Power::Vitality: return "VITALITY";
        default:              return "";
    }
}

inline const char* powerShortDesc(Power p) {
    // Short and UI-friendly. (Tooltips can elaborate elsewhere.)
    switch (p) {
        case Power::Flame:    return "BURN ON HIT, +MIGHT";
        case Power::Venom:    return "POISON ON HIT, +AGI";
        case Power::Daze:     return "CONFUSE ON HIT, +FOCUS";
        case Power::Ward:     return "SHIELD PROC, +DEF";
        case Power::Vitality: return "LIFE SURGE, +VIG";
        default:              return "";
    }
}

inline const char* powerDesc(Power p) {
    // Slightly longer (still UI-friendly) description used by crafting / inspect panes.
    // Keep this stable: it appears in saved recipes and player-facing logs/screenshots.
    switch (p) {
        case Power::Flame:    return "IGNITES FOES ON HIT. PASSIVE: +MIGHT.";
        case Power::Venom:    return "POISONS FOES ON HIT. PASSIVE: +AGILITY.";
        case Power::Daze:     return "CONFUSES FOES ON HIT. PASSIVE: +FOCUS.";
        case Power::Ward:     return "OCCASIONALLY SHIELDS YOU. PASSIVE: +DEFENSE.";
        case Power::Vitality: return "LIFE SURGES ON STRIKES. PASSIVE: +VIGOR.";
        default:              return "";
    }
}

// Artifact power level (0..4). Level 0 means the artifact is currently inert
// (typically due to strong curses/negative enchant).
inline int powerLevel(const Item& it) {
    if (!isArtifactGear(it)) return 0;
    const int lvl = 1 + it.enchant + bucScalar(it);
    return std::clamp(lvl, 0, 4);
}

inline int tieredBonusFromLevel(int lvl) {
    // Maps [1..4] -> {1,1,2,2}. Keeps artifacts impactful but not runaway.
    if (lvl <= 0) return 0;
    return (lvl >= 3) ? 2 : 1;
}

inline int passiveBonusMight(const Item& it) {
    if (!isArtifactGear(it)) return 0;
    if (artifactPower(it) != Power::Flame) return 0;
    return tieredBonusFromLevel(powerLevel(it));
}

inline int passiveBonusAgility(const Item& it) {
    if (!isArtifactGear(it)) return 0;
    if (artifactPower(it) != Power::Venom) return 0;
    return tieredBonusFromLevel(powerLevel(it));
}

inline int passiveBonusFocus(const Item& it) {
    if (!isArtifactGear(it)) return 0;
    if (artifactPower(it) != Power::Daze) return 0;
    return tieredBonusFromLevel(powerLevel(it));
}

inline int passiveBonusDefense(const Item& it) {
    if (!isArtifactGear(it)) return 0;
    if (artifactPower(it) != Power::Ward) return 0;
    return tieredBonusFromLevel(powerLevel(it));
}

inline int passiveBonusVigor(const Item& it) {
    if (!isArtifactGear(it)) return 0;
    if (artifactPower(it) != Power::Vitality) return 0;
    // Used as a scaling input for regen-style procs.
    return tieredBonusFromLevel(powerLevel(it));
}

// Backward-compatible alias used by older callsites.
inline int passiveBonusRegen(const Item& it) {
    return passiveBonusVigor(it);
}

inline std::string artifactTitle(const Item& it) {
    static constexpr const char* kPrefixes[] = {
        "ANCIENT", "OBSIDIAN", "STARFORGED", "IVORY", "EMBER", "FROST", "BLOOD", "SILVER",
        "VOID", "ECHOING", "GILDED", "ASHEN", "SABLE", "RADIANT", "GRIM", "CELESTIAL",
    };
    static constexpr const char* kNouns[] = {
        "WHISPER", "FANG", "EDGE", "WARD", "GLORY", "BANE", "REQUIEM", "AURORA",
        "CROWN", "OATH", "FURY", "ECLIPSE", "VEIL", "BULWARK", "MIRROR", "SPIRAL",
    };

    const uint32_t h = artifactSeed(it);
    constexpr size_t np = sizeof(kPrefixes) / sizeof(kPrefixes[0]);
    constexpr size_t nn = sizeof(kNouns) / sizeof(kNouns[0]);

    const char* pre = kPrefixes[(np == 0) ? 0 : ((h >> 8) % static_cast<uint32_t>(np))];
    const char* noun = kNouns[(nn == 0) ? 0 : ((h >> 16) % static_cast<uint32_t>(nn))];

    std::string s;
    s.reserve(32);
    s += pre;
    s += " ";
    s += noun;
    return s;
}

inline const char* artifactPowerTag(const Item& it) {
    if (!isArtifactGear(it)) return "";
    return powerTag(artifactPower(it));
}

} // namespace artifactgen
