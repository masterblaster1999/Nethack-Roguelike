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
};

inline const char* wardWordName(WardWord w) {
    switch (w) {
        case WardWord::Elbereth: return "ELBERETH";
        case WardWord::Salt:     return "SALT";
        case WardWord::Iron:     return "IRON";
        case WardWord::Fire:     return "FIRE";
        default:                 return "";
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
    if (canon == "ELBERETH") return WardWord::Elbereth;
    if (canon == "SALT")     return WardWord::Salt;
    if (canon == "IRON" || canon == "COLD IRON") return WardWord::Iron;
    if (canon == "FIRE" || canon == "EMBER") return WardWord::Fire;
    return WardWord::None;
}

inline WardWord wardWordFromEngraving(const Engraving& eg) {
    if (!eg.isWard) return WardWord::None;
    return wardWordFromText(eg.text);
}

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
        default:
            break;
    }

    const float usesF = static_cast<float>(std::min<uint8_t>(strength, 30u));
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
