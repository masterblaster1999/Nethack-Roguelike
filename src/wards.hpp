#pragma once

// Warding words / floor wards.
//
// Wards are engravings (see Engraving in game.hpp) with `isWard=true`.
// They act as NetHack-inspired "panic buttons": while you stand on a ward,
// some monsters may hesitate to attack and try to back off.
//
// This header centralizes ward parsing and balance rules so engraving (writing)
// and AI (reacting) stay in sync.

#include "game.hpp"

#include <algorithm>
#include <cctype>
#include <string>

enum class WardWord : uint8_t {
    None = 0,
    Elbereth,
    Salt,
    Iron,
    Fire,

    // Procedural rune-wards (typically etched by Rune Tablet "WARD" proc spells).
    RuneFire,
    RuneFrost,
    RuneShock,
    RuneVenom,
    RuneShadow,
    RuneRadiance,
    RuneArcane,
    RuneStone,
    RuneWind,
    RuneBlood,
};

inline const char* wardWordName(WardWord w) {
    switch (w) {
        case WardWord::Elbereth:     return "ELBERETH";
        case WardWord::Salt:         return "SALT";
        case WardWord::Iron:         return "IRON";
        case WardWord::Fire:         return "FIRE";

        case WardWord::RuneFire:     return "RUNE FIRE";
        case WardWord::RuneFrost:    return "RUNE FROST";
        case WardWord::RuneShock:    return "RUNE SHOCK";
        case WardWord::RuneVenom:    return "RUNE VENOM";
        case WardWord::RuneShadow:   return "RUNE SHADOW";
        case WardWord::RuneRadiance: return "RUNE RADIANCE";
        case WardWord::RuneArcane:   return "RUNE ARCANE";
        case WardWord::RuneStone:    return "RUNE STONE";
        case WardWord::RuneWind:     return "RUNE WIND";
        case WardWord::RuneBlood:    return "RUNE BLOOD";

        default:                     return "";
    }
}

inline std::string wardCanon(const std::string& s) {
    // Uppercase + trim ASCII whitespace. Keep internal spaces ("COLD IRON") intact.
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;

    std::string out;
    out.reserve(e - b);
    for (size_t i = b; i < e; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

inline WardWord wardWordFromText(const std::string& text) {
    const std::string canon = wardCanon(text);

    // Classic / simple wards.
    if (canon == "ELBERETH") return WardWord::Elbereth;
    if (canon == "SALT")     return WardWord::Salt;
    if (canon == "IRON" || canon == "COLD IRON") return WardWord::Iron;
    if (canon == "FIRE" || canon == "EMBER") return WardWord::Fire;

    // Rune wards:
    //   - "RUNE FIRE"
    //   - "RUNE:FIRE"
    //   - "RUNE OF FIRE"
    //   - "RUNE FIRE: KAR-THO-RAI"  (suffix allowed; only prefix is parsed)
    //
    // The goal is: be forgiving in parsing, while keeping the canonical name stable.
    if (canon.rfind("RUNE", 0) == 0) {
        size_t i = 4; // after "RUNE"
        while (i < canon.size() && (canon[i] == ' ' || canon[i] == ':' || canon[i] == '\t')) ++i;

        // Optional "OF".
        if (i + 2 < canon.size() && canon.compare(i, 3, "OF ") == 0) i += 3;
        while (i < canon.size() && (canon[i] == ' ' || canon[i] == ':' || canon[i] == '\t')) ++i;

        // Read the element token.
        const size_t start = i;
        while (i < canon.size() && std::isalpha(static_cast<unsigned char>(canon[i]))) ++i;

        if (i > start) {
            const std::string elem = canon.substr(start, i - start);

            if (elem == "FIRE" || elem == "EMBER") return WardWord::RuneFire;
            if (elem == "FROST" || elem == "ICE") return WardWord::RuneFrost;
            if (elem == "SHOCK" || elem == "STORM") return WardWord::RuneShock;
            if (elem == "VENOM" || elem == "POISON") return WardWord::RuneVenom;
            if (elem == "SHADOW" || elem == "DARK") return WardWord::RuneShadow;
            if (elem == "RADIANCE" || elem == "LIGHT") return WardWord::RuneRadiance;
            if (elem == "ARCANE" || elem == "AETHER") return WardWord::RuneArcane;
            if (elem == "STONE" || elem == "EARTH") return WardWord::RuneStone;
            if (elem == "WIND" || elem == "AIR") return WardWord::RuneWind;
            if (elem == "BLOOD") return WardWord::RuneBlood;
        }
    }

    return WardWord::None;
}

inline WardWord wardWordFromEngraving(const Engraving& eg) {
    if (!eg.isWard) return WardWord::None;
    return wardWordFromText(eg.text);
}

namespace {
inline bool wardIsUndead(EntityKind k) {
    return (k == EntityKind::SkeletonArcher || k == EntityKind::Ghost || k == EntityKind::Zombie);
}
} // namespace

// Returns true if this ward word should have *any* effect on this monster kind.
inline bool wardAffectsMonster(WardWord w, EntityKind k) {
    switch (w) {
        case WardWord::Elbereth:
            // Classic: scares "living" things; undead and bosses ignore.
            switch (k) {
                case EntityKind::SkeletonArcher:
                case EntityKind::Ghost:
                case EntityKind::Zombie:
                case EntityKind::Wizard:
                case EntityKind::Minotaur:
                case EntityKind::Shopkeeper:
                    return false;
                default:
                    return true;
            }

        case WardWord::Salt:
            // Folklore: a salt line wards off spirits/undead.
            switch (k) {
                case EntityKind::SkeletonArcher:
                case EntityKind::Ghost:
                case EntityKind::Zombie:
                    return true;
                default:
                    return false;
            }

        case WardWord::Iron:
            // Cold iron hurts "fae" tricksters.
            return (k == EntityKind::Leprechaun || k == EntityKind::Nymph);

        case WardWord::Fire:
            // Primal fear of flame: slimes and spiders hesitate.
            return (k == EntityKind::Slime || k == EntityKind::Spider);

        // ----------------------------
        // Rune wards (elemental wards)
        // ----------------------------

        case WardWord::RuneRadiance:
            // Bright runes scorch undead/ethereal minds.
            return wardIsUndead(k) || (k == EntityKind::Wizard);

        case WardWord::RuneShadow:
            // Shadow runes unsettle the living, but do little to the undead.
            if (wardIsUndead(k)) return false;
            // Civilized/unyielding minds tend to ignore this.
            if (k == EntityKind::Shopkeeper || k == EntityKind::Guard || k == EntityKind::Minotaur) return false;
            return true;

        case WardWord::RuneArcane:
            // "Weird" creatures hate explicit arcana.
            return (k == EntityKind::Wizard || k == EntityKind::Mimic || k == EntityKind::Leprechaun || k == EntityKind::Nymph);

        case WardWord::RuneShock:
            // Crackling lines startle small raiders.
            return (k == EntityKind::Goblin || k == EntityKind::Orc || k == EntityKind::KoboldSlinger);

        case WardWord::RuneFire:
            return (k == EntityKind::Slime || k == EntityKind::Spider);

        case WardWord::RuneFrost:
            return (k == EntityKind::Bat || k == EntityKind::Wolf || k == EntityKind::Snake);

        case WardWord::RuneStone:
            // Heavy brutes hesitate at "weight of stone".
            return (k == EntityKind::Ogre || k == EntityKind::Troll || k == EntityKind::Minotaur);

        case WardWord::RuneWind:
            // Air-sense wards disrupt fluttering/floating threats.
            return (k == EntityKind::Bat || k == EntityKind::Ghost);

        case WardWord::RuneVenom:
            // Toxic runes repulse predators that rely on smell.
            return (k == EntityKind::Wolf || k == EntityKind::Snake);

        case WardWord::RuneBlood:
            // Blood runes ward off beasts and venomous ambushers.
            return (k == EntityKind::Wolf || k == EntityKind::Snake || k == EntityKind::Spider);

        default:
            return false;
    }
}

// A small tuning knob: some monsters are less afraid even when the ward is applicable.
inline float wardResistanceFactor(WardWord w, EntityKind k) {
    // 1.0 = normal fear, 0.0 = fully immune.
    // Lower values reduce repel chance.
    switch (w) {
        case WardWord::Elbereth:
            // Big brutes are harder to scare.
            if (k == EntityKind::Ogre || k == EntityKind::Troll) return 0.70f;
            if (k == EntityKind::Mimic) return 0.80f;
            return 1.0f;
        case WardWord::Salt:
            // Skeletons are less "spooky" than ghosts.
            if (k == EntityKind::SkeletonArcher) return 0.70f;
            return 1.0f;
        case WardWord::Iron:
            // Leprechauns are bold; nymphs are skittish.
            if (k == EntityKind::Leprechaun) return 0.85f;
            return 1.0f;
        case WardWord::Fire:
            // Spiders are a bit bolder than slimes.
            if (k == EntityKind::Spider) return 0.80f;
            return 1.0f;

        // Rune wards: assume a generally higher "will check" across the board.
        case WardWord::RuneShadow:
            // Big brutes shrug off fear-magic.
            if (k == EntityKind::Ogre || k == EntityKind::Troll) return 0.75f;
            return 1.0f;

        case WardWord::RuneRadiance:
            // Wizards are stubborn even when it burns.
            if (k == EntityKind::Wizard) return 0.80f;
            return 1.0f;

        case WardWord::RuneArcane:
            if (k == EntityKind::Wizard) return 0.70f;
            return 1.0f;

        case WardWord::RuneStone:
            if (k == EntityKind::Minotaur) return 0.70f;
            if (k == EntityKind::Troll) return 0.80f;
            return 1.0f;

        case WardWord::RuneShock:
            if (k == EntityKind::Orc) return 0.85f;
            return 1.0f;

        case WardWord::RuneFrost:
        case WardWord::RuneFire:
        case WardWord::RuneWind:
        case WardWord::RuneVenom:
        case WardWord::RuneBlood:
            // Small dampening for "tough" monsters.
            if (k == EntityKind::Ogre || k == EntityKind::Minotaur) return 0.85f;
            return 1.0f;

        default:
            return 1.0f;
    }
}

// Compute the chance that a monster hesitates this turn.
// `strength` is the ward's remaining durability uses (1..254).
inline float wardRepelChance(WardWord w, EntityKind k, uint8_t strength) {
    if (w == WardWord::None) return 0.0f;
    if (!wardAffectsMonster(w, k)) return 0.0f;
    if (strength == 0u) return 0.0f;

    // Base chance per ward word.
    float base = 0.25f;
    float perUse = 0.08f;
    float cap = 0.85f;

    switch (w) {
        case WardWord::Elbereth:
            base = 0.28f;
            perUse = 0.08f;
            cap = 0.82f;
            break;
        case WardWord::Salt:
            base = 0.30f;
            perUse = 0.09f;
            cap = 0.88f;
            break;
        case WardWord::Iron:
            base = 0.38f;
            perUse = 0.10f;
            cap = 0.92f;
            break;
        case WardWord::Fire:
            base = 0.22f;
            perUse = 0.07f;
            cap = 0.78f;
            break;

        // Rune wards: tuned slightly lower than IRON, but competitive with classic wards.
        case WardWord::RuneShadow:
            base = 0.16f;
            perUse = 0.06f;
            cap = 0.72f;
            break;
        case WardWord::RuneRadiance:
            base = 0.24f;
            perUse = 0.08f;
            cap = 0.82f;
            break;
        case WardWord::RuneArcane:
            base = 0.22f;
            perUse = 0.07f;
            cap = 0.78f;
            break;
        case WardWord::RuneShock:
            base = 0.21f;
            perUse = 0.07f;
            cap = 0.78f;
            break;
        case WardWord::RuneFire:
            base = 0.19f;
            perUse = 0.06f;
            cap = 0.75f;
            break;
        case WardWord::RuneFrost:
            base = 0.19f;
            perUse = 0.06f;
            cap = 0.75f;
            break;
        case WardWord::RuneStone:
            base = 0.20f;
            perUse = 0.07f;
            cap = 0.80f;
            break;
        case WardWord::RuneWind:
            base = 0.18f;
            perUse = 0.06f;
            cap = 0.72f;
            break;
        case WardWord::RuneVenom:
            base = 0.18f;
            perUse = 0.06f;
            cap = 0.74f;
            break;
        case WardWord::RuneBlood:
            base = 0.20f;
            perUse = 0.07f;
            cap = 0.78f;
            break;

        default:
            break;
    }

    const float usesF = static_cast<float>(std::min<uint8_t>(strength, uint8_t{30}));
    float chance = base + perUse * usesF;

    // Apply per-monster resistance.
    chance *= wardResistanceFactor(w, k);

    return std::clamp(chance, 0.0f, cap);
}

// Additional durability wear when a monster is "stuck" and tries to smudge the ward.
// The returned amount is applied *in addition* to the normal per-contact wear.
inline int wardSmudgeWearBonus(WardWord w, EntityKind k) {
    (void)w;
    // Big creatures can smear wards faster.
    if (k == EntityKind::Ogre || k == EntityKind::Minotaur) return 2;
    if (k == EntityKind::Troll) return 1;
    return 0;
}
