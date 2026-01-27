#pragma once

// Procedural naming helpers.
//
// This module provides lightweight, deterministic, flavor-only codenames for
// procedural monster variants (Elite/Champion/Mythic, affix/ability monsters).
//
// Design goals:
// - Deterministic per-entity across save/load.
// - No save format changes (derived from existing persisted fields).
// - Cheap: fixed lookup tables + integer hashing; no RNG stream consumption.
// - Header-only (drop-in).

#include "game.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace procname {

// Only show codenames for rare procedural variants.
inline bool shouldShowCodename(const Entity& e) {
    if (e.kind == EntityKind::Player) return false;
    if (e.friendly) return false;

    // Procedural variants are rank-gated in the spawner; keep the predicate
    // conservative and future-proof.
    if (e.procRank != ProcMonsterRank::Normal) return true;
    if (e.procAffixMask != 0u) return true;
    if (e.procAbility1 != ProcMonsterAbility::None) return true;
    if (e.procAbility2 != ProcMonsterAbility::None) return true;
    return false;
}

inline uint32_t nameSeedFor(const Entity& e) {
    // Prefer the persisted sprite seed; it is stable across save/load.
    uint32_t s = e.spriteSeed;
    if (s == 0u) {
        // Defensive fallback for malformed/legacy entities.
        const uint32_t a = static_cast<uint32_t>(e.id) ^ 0xBADC0DEu;
        const uint32_t b = static_cast<uint32_t>(static_cast<uint8_t>(e.kind)) ^ 0xC0FFEEu;
        s = hashCombine(a, b);
        if (s == 0u) s = 1u;
    }

    // Domain-separate and fold in persisted proc fields so name stays stable even
    // if other code ever changes how spriteSeed is allocated.
    s = hashCombine(s, "MONNAME"_tag);
    s = hashCombine(s, static_cast<uint32_t>(static_cast<uint8_t>(e.kind)));
    s = hashCombine(s, static_cast<uint32_t>(static_cast<uint8_t>(e.procRank)));
    s = hashCombine(s, e.procAffixMask);
    s = hashCombine(s, static_cast<uint32_t>(static_cast<uint8_t>(e.procAbility1)));
    s = hashCombine(s, static_cast<uint32_t>(static_cast<uint8_t>(e.procAbility2)));

    if (s == 0u) s = 1u;
    return s;
}

namespace {

static constexpr std::array<const char*, 32> BASE_ADJ = {
    "SILENT", "CRIMSON", "IVORY", "ASHEN",
    "OBSIDIAN", "HOLLOW", "GILDED", "FROSTED",
    "RADIANT", "GRIM", "WICKED", "CELESTIAL",
    "EMBER", "SABLE", "STARFORGED", "ECHOING",
    "UMBRAL", "FERAL", "ARCANE", "BROKEN",
    "MOURNFUL", "LURKING", "RUSTED", "VERDANT",
    "SHATTERED", "HUNGRY", "HISSING", "BLOOD",
    "SHADOW", "THORNED", "BRASS", "SPECTRAL",
};

static constexpr std::array<const char*, 32> BASE_NOUN = {
    "FANG", "OATH", "VEIL", "REQUIEM",
    "SPIRAL", "CROWN", "BANE", "WARD",
    "AURORA", "MIRROR", "SIGIL", "LANTERN",
    "BULWARK", "WHISPER", "ECLIPSE", "GLORY",
    "CLAW", "HEX", "MAW", "PACK",
    "RIFT", "COIL", "THREAD", "HUNTER",
    "HOWL", "RUNE", "COIN", "ARROW",
    "MASK", "BLOOM", "EMBER", "TETHER",
};

inline const char* pick1(const char* const* arr, size_t n, uint32_t h) {
    if (!arr || n == 0) return "";
    return arr[static_cast<size_t>(h % static_cast<uint32_t>(n))];
}

inline const char* adjForAffix(ProcMonsterAffix a, uint32_t h) {
    switch (a) {
        case ProcMonsterAffix::Swift: {
            static constexpr const char* A[] = {"SWIFT", "FLEET", "RAPID", "GALE"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Stonehide: {
            static constexpr const char* A[] = {"STONE", "IRON", "GRANITE", "OBSIDIAN"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Savage: {
            static constexpr const char* A[] = {"SAVAGE", "FERAL", "RABID", "BRUTAL"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Blinking: {
            static constexpr const char* A[] = {"SHIFTING", "PHASED", "VANISHING", "WINKING"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Gilded: {
            static constexpr const char* A[] = {"GILDED", "GOLDEN", "COINED", "TREASURED"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Venomous: {
            static constexpr const char* A[] = {"TOXIC", "VENOM", "VIPER", "POISONED"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Flaming: {
            static constexpr const char* A[] = {"EMBER", "CINDER", "BLAZING", "FIERY"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Vampiric: {
            static constexpr const char* A[] = {"SANGUINE", "NOCTURNE", "BLOOD", "DREAD"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Webbing: {
            static constexpr const char* A[] = {"SILKEN", "WEAVING", "THREADBARE", "SPUN"};
            return pick1(A, 4, h);
        }
        case ProcMonsterAffix::Commander: {
            static constexpr const char* A[] = {"WAR", "BANNERED", "IMPERIOUS", "MARTIAL"};
            return pick1(A, 4, h);
        }
        default:
            return "";
    }
}

inline const char* nounForAffix(ProcMonsterAffix a, uint32_t h) {
    switch (a) {
        case ProcMonsterAffix::Swift: {
            static constexpr const char* N[] = {"GALE", "DASH", "WIND", "RIFT"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Stonehide: {
            static constexpr const char* N[] = {"BULWARK", "STONE", "WALL", "ANVIL"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Savage: {
            static constexpr const char* N[] = {"HUNTER", "MAW", "CLAW", "BANE"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Blinking: {
            static constexpr const char* N[] = {"RIFT", "MIRROR", "VEIL", "ECHO"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Gilded: {
            static constexpr const char* N[] = {"COIN", "CROWN", "GLORY", "LANTERN"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Venomous: {
            static constexpr const char* N[] = {"FANG", "MIASMA", "COIL", "VENOM"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Flaming: {
            static constexpr const char* N[] = {"EMBER", "CINDER", "NOVA", "ASH"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Vampiric: {
            static constexpr const char* N[] = {"BLOOD", "VEIL", "REQUIEM", "MAW"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Webbing: {
            static constexpr const char* N[] = {"THREAD", "WEB", "SILK", "SNARE"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAffix::Commander: {
            static constexpr const char* N[] = {"BANNER", "OATH", "CROWN", "BULWARK"};
            return pick1(N, 4, h);
        }
        default:
            return "";
    }
}

inline const char* nounForAbility(ProcMonsterAbility a, uint32_t h) {
    switch (a) {
        case ProcMonsterAbility::Pounce: {
            static constexpr const char* N[] = {"TALON", "LEAP", "CLAW", "PREDATOR"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAbility::ToxicMiasma: {
            static constexpr const char* N[] = {"MIASMA", "FUME", "CLOUD", "VENOM"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAbility::CinderNova: {
            static constexpr const char* N[] = {"NOVA", "CINDER", "ASH", "FLARE"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAbility::ArcaneWard: {
            static constexpr const char* N[] = {"WARD", "SIGIL", "RUNE", "AEGIS"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAbility::SummonMinions: {
            static constexpr const char* N[] = {"HERALD", "CALLER", "BANNER", "HORDE"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAbility::Screech: {
            static constexpr const char* N[] = {"SCREECH", "SHRIEK", "ECHO", "CRESCENDO"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAbility::VoidHook: {
            static constexpr const char* N[] = {"HOOK", "CHAIN", "TETHER", "RIFT"};
            return pick1(N, 4, h);
        }
        case ProcMonsterAbility::None:
        default:
            return "";
    }
}

inline const char* nounForKind(EntityKind k, uint32_t h) {
    switch (k) {
        case EntityKind::Wolf: {
            static constexpr const char* N[] = {"HOWL", "PACK", "FANG", "HUNTER"};
            return pick1(N, 4, h);
        }
        case EntityKind::Bat: {
            static constexpr const char* N[] = {"WING", "ECHO", "NIGHT", "FANG"};
            return pick1(N, 4, h);
        }
        case EntityKind::Snake: {
            static constexpr const char* N[] = {"COIL", "FANG", "VIPER", "VENOM"};
            return pick1(N, 4, h);
        }
        case EntityKind::Spider: {
            static constexpr const char* N[] = {"THREAD", "WEB", "SILK", "SNARE"};
            return pick1(N, 4, h);
        }
        case EntityKind::Wizard: {
            static constexpr const char* N[] = {"SIGIL", "HEX", "RUNE", "TOME"};
            return pick1(N, 4, h);
        }
        case EntityKind::Mimic: {
            static constexpr const char* N[] = {"MIRROR", "LURE", "MASK", "MAW"};
            return pick1(N, 4, h);
        }
        case EntityKind::SkeletonArcher: {
            static constexpr const char* N[] = {"ARROW", "BONE", "RATTLE", "REQUIEM"};
            return pick1(N, 4, h);
        }
        case EntityKind::Zombie: {
            static constexpr const char* N[] = {"FLESH", "GRAVE", "REQUIEM", "MAW"};
            return pick1(N, 4, h);
        }
        case EntityKind::Ogre:
        case EntityKind::Troll: {
            static constexpr const char* N[] = {"MAUL", "CLUB", "MAW", "BULWARK"};
            return pick1(N, 4, h);
        }
        case EntityKind::Orc:
        case EntityKind::Goblin:
        case EntityKind::KoboldSlinger: {
            static constexpr const char* N[] = {"KNIFE", "OATH", "CROWN", "BANE"};
            return pick1(N, 4, h);
        }
        default:
            return "";
    }
}

} // namespace

inline std::string codename(const Entity& e) {
    if (!shouldShowCodename(e)) return std::string();

    const uint32_t seed = nameSeedFor(e);
    const uint32_t hA = hash32(seed ^ 0xA11CEu);
    const uint32_t hN = hash32(seed ^ 0xC0DEF00Du);

    // Collect affixes for themed adjective selection.
    std::array<ProcMonsterAffix, 16> aff;
    int affN = 0;
    for (ProcMonsterAffix a : PROC_MONSTER_AFFIX_ALL) {
        if (!procHasAffix(e.procAffixMask, a)) continue;
        if (affN < static_cast<int>(aff.size())) aff[static_cast<size_t>(affN++)] = a;
    }

    // Adjective: mostly affix-driven for variants, with a small chance of a generic flair.
    const bool forceAffAdj = (e.procRank == ProcMonsterRank::Mythic);
    const bool useAffAdj = (affN > 0) && (forceAffAdj || ((hA & 7u) < 5u));

    const char* adj = "";
    if (useAffAdj) {
        const ProcMonsterAffix a = aff[static_cast<size_t>(hA % static_cast<uint32_t>(affN))];
        adj = adjForAffix(a, hA);
    }
    if (!adj || !adj[0]) {
        adj = BASE_ADJ[static_cast<size_t>(hA % static_cast<uint32_t>(BASE_ADJ.size()))];
    }

    // Noun pool: ability -> kind -> (rarely) affix.
    std::array<const char*, 8> nounPool;
    int nounN = 0;

    auto addN = [&](const char* n) {
        if (!n || !n[0]) return;
        if (nounN < static_cast<int>(nounPool.size())) nounPool[static_cast<size_t>(nounN++)] = n;
    };

    // Abilities bias nouns strongly (they read well as titles).
    addN(nounForAbility(e.procAbility1, hN));
    if (e.procAbility2 != e.procAbility1) addN(nounForAbility(e.procAbility2, hN >> 1));

    // Kind flavor fills gaps.
    addN(nounForKind(e.kind, hN));

    // Occasionally let an affix supply the noun too.
    if (affN > 0 && nounN < static_cast<int>(nounPool.size()) && ((hN & 3u) == 0u)) {
        const ProcMonsterAffix a = aff[static_cast<size_t>(hN % static_cast<uint32_t>(affN))];
        addN(nounForAffix(a, hN));
    }

    const bool useBaseNoun = (nounN == 0) || ((hN & 15u) == 15u); // rare base noun spice

    const char* noun = "";
    if (!useBaseNoun) {
        noun = nounPool[static_cast<size_t>(hN % static_cast<uint32_t>(nounN))];
    }
    if (!noun || !noun[0]) {
        noun = BASE_NOUN[static_cast<size_t>(hN % static_cast<uint32_t>(BASE_NOUN.size()))];
    }

    // Prevent occasional duplicated tokens like "EMBER EMBER".
    if (std::string(adj) == std::string(noun)) {
        noun = BASE_NOUN[static_cast<size_t>((hN ^ 0x1234567u) % static_cast<uint32_t>(BASE_NOUN.size()))];
    }

    std::string out;
    out.reserve(32);
    out += adj;
    out += ' ';
    out += noun;
    return out;
}

} // namespace procname
