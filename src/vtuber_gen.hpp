#pragma once

#include "common.hpp"
#include "rng.hpp"

#include <string>
#include <sstream>
#include <algorithm>

// -----------------------------------------------------------------------------
// Procedural VTuber generator (original, deterministic)
// -----------------------------------------------------------------------------
//
// This provides small, anime-adjacent "VTuber" persona bits (stage name,
// archetype, stream tag, follower count) derived deterministically from a
// 32-bit seed. The intent is to create *original* outputs without relying on
// any real VTuber names, brands, or copyrighted assets.
//
// Typical usage:
//   - Seed with Item::spriteSeed (unique per drop)
//   - Use vtuberStageName(seed) for display strings
//   - Use the same seed for sprite generation (see spritegen.cpp)
// -----------------------------------------------------------------------------

inline uint32_t vtuberMixSeed(uint32_t seed) {
    // Extra mixing layer so adjacent seeds don't look too similar.
    return hash32(seed ^ 0xC0B17E99u);
}

inline std::string vtuberMakeWord(RNG& rng, int sylMin, int sylMax) {
    // Invented CV-ish syllables (not a dictionary).
    // Keeping these short reduces the chance of accidentally matching any
    // specific real-world name.
    static constexpr const char* SYL[] = {
        "ka","ki","ku","ke","ko",
        "sa","shi","su","se","so",
        "ta","chi","tsu","te","to",
        "na","ni","nu","ne","no",
        "ha","hi","fu","he","ho",
        "ma","mi","mu","me","mo",
        "ra","ri","ru","re","ro",
        "ya","yu","yo",
        "ga","gi","gu","ge","go",
        "pa","pi","pu","pe","po",
        "la","li","lu","le","lo",
        "za","zi","zu","ze","zo",
        "nya","mya","ryo","kyo","pyo",
        "sha","sho","chu","kha","fyo"
    };

    const int SYL_COUNT = static_cast<int>(sizeof(SYL) / sizeof(SYL[0]));
    sylMin = std::max(1, sylMin);
    sylMax = std::max(sylMin, sylMax);

    const int n = rng.range(sylMin, sylMax);
    std::string w;
    w.reserve(12);

    for (int i = 0; i < n; ++i) {
        const char* s = SYL[rng.range(0, SYL_COUNT - 1)];
        // Keep words compact.
        if (w.size() + std::char_traits<char>::length(s) > 10 && i >= sylMin) break;
        w += s;
    }

    // Title-ish casing (then callers often toUpper()).
    if (!w.empty()) {
        if (w[0] >= 'a' && w[0] <= 'z') w[0] = static_cast<char>(w[0] - 'a' + 'A');
        for (size_t i = 1; i < w.size(); ++i) {
            if (w[i] >= 'A' && w[i] <= 'Z') w[i] = static_cast<char>(w[i] - 'A' + 'a');
        }
    }

    if (w.empty()) w = "Aoi"; // extremely defensive
    return w;
}

inline std::string vtuberStageName(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed));

    // Two-part stage name.
    std::string first = vtuberMakeWord(rng, 2, 3);
    std::string last  = vtuberMakeWord(rng, 2, 3);

    // Occasionally add a short epithet-like suffix.
    if (rng.chance(0.18f)) {
        static constexpr const char* EP[] = { "NOVA", "NEON", "LUNA", "AURORA", "PIXEL", "COMET" };
        const int epN = static_cast<int>(sizeof(EP) / sizeof(EP[0]));
        last = last + " " + EP[rng.range(0, epN - 1)];
    }

    return toUpper(first + " " + last);
}

inline std::string vtuberArchetype(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed ^ 0x9E3779B9u));

    static constexpr const char* A[] = {
        "MOON WITCH",
        "NEON ANDROID",
        "CLOCKWORK ANGEL",
        "FOREST ORACLE",
        "STAR PIRATE",
        "DEEPSEA SIREN",
        "GLITCH FAIRY",
        "SUN KNIGHT",
        "ICE ALCHEMIST",
        "THUNDER FOX",
        "CRYSTAL SWORDSMAGE",
        "DREAM WEAVER",
        "VOID JESTER",
        "RUNE LIBRARIAN",
        "MOSS MAGE",
        "LAVA DJ",
        "SKY GARDENER",
        "ROSE NECROMANCER",
        "COSMIC BARD",
        "FROST DRIFTER"
    };

    const int N = static_cast<int>(sizeof(A) / sizeof(A[0]));
    return std::string(A[rng.range(0, N - 1)]);
}

inline std::string vtuberStreamTag(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed ^ 0xA57D1E55u));

    static constexpr const char* T[] = {
        "KARAOKE",
        "GAMING",
        "ASMR",
        "ART",
        "COOKING",
        "LORE",
        "CHAOS",
        "SPEEDRUN",
        "PUZZLES",
        "RHYTHM",
        "CHALLENGE RUNS",
        "DUNGEON TALK"
    };

    const int N = static_cast<int>(sizeof(T) / sizeof(T[0]));
    return std::string(T[rng.range(0, N - 1)]);
}

inline int vtuberFollowerCount(uint32_t seed) {
    // Purely flavor: produce a plausible follower count.
    RNG rng(vtuberMixSeed(seed ^ 0x7F4A7C15u));

    // Skewed distribution: lots of small-mid, few huge.
    const float r = rng.next01();
    int base;
    if (r < 0.65f) base = rng.range(1'000, 99'999);
    else if (r < 0.92f) base = rng.range(100'000, 799'999);
    else base = rng.range(800'000, 2'500'000);

    return std::max(0, base);
}

inline std::string vtuberFormatFollowers(int n) {
    n = std::max(0, n);
    std::ostringstream ss;
    if (n < 1'000) {
        ss << n;
    } else if (n < 1'000'000) {
        const int k = n / 1'000;
        ss << k << "K";
    } else {
        const int m = n / 1'000'000;
        const int rem = (n % 1'000'000) / 100'000;
        ss << m;
        if (rem > 0) ss << "." << rem;
        ss << "M";
    }
    return ss.str();
}

inline std::string vtuberFollowerText(uint32_t seed) {
    return vtuberFormatFollowers(vtuberFollowerCount(seed));
}

// -----------------------------------------------------------------------------
// Extra flavor for VTuber collectibles (cards/figurines)
// -----------------------------------------------------------------------------

enum class VtuberRarity : uint8_t {
    Common = 0,
    Rare,
    Epic,
    Mythic,
};

inline VtuberRarity vtuberRarity(uint32_t seed) {
    // Deterministic and *not* tied to followerCount directly so you can
    // occasionally find a "mythic" indie or a "common" breakout.
    RNG rng(vtuberMixSeed(seed ^ 0x51A6F3C9u));
    const float r = rng.next01();
    if (r < 0.70f) return VtuberRarity::Common;
    if (r < 0.92f) return VtuberRarity::Rare;
    if (r < 0.985f) return VtuberRarity::Epic;
    return VtuberRarity::Mythic;
}

inline const char* vtuberRarityName(VtuberRarity r) {
    switch (r) {
        case VtuberRarity::Common: return "COMMON";
        case VtuberRarity::Rare:   return "RARE";
        case VtuberRarity::Epic:   return "EPIC";
        case VtuberRarity::Mythic: return "MYTHIC";
        default:                   return "COMMON";
    }
}



// -----------------------------------------------------------------------------
// Holo card editions / variants (all derived deterministically from the seed).
// -----------------------------------------------------------------------------

enum class VtuberCardEdition : uint8_t {
    Standard = 0,
    Foil,
    AltArt,
    Signed,
    Collab,
};

inline VtuberCardEdition vtuberCardEdition(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed ^ 0x2F7D4C2Bu));
    const VtuberRarity rar = vtuberRarity(seed);
    const float r = rng.next01();

    // Distributions biased by rarity: common cards are mostly standard,
    // mythic cards more often have special editions (foil/signed/collab).
    if (rar == VtuberRarity::Common) {
        if (r < 0.86f) return VtuberCardEdition::Standard;
        if (r < 0.98f) return VtuberCardEdition::Foil;
        return VtuberCardEdition::AltArt;
    }
    if (rar == VtuberRarity::Rare) {
        if (r < 0.70f) return VtuberCardEdition::Standard;
        if (r < 0.90f) return VtuberCardEdition::Foil;
        if (r < 0.97f) return VtuberCardEdition::AltArt;
        if (r < 0.995f) return VtuberCardEdition::Signed;
        return VtuberCardEdition::Collab;
    }
    if (rar == VtuberRarity::Epic) {
        if (r < 0.45f) return VtuberCardEdition::Standard;
        if (r < 0.70f) return VtuberCardEdition::Foil;
        if (r < 0.82f) return VtuberCardEdition::AltArt;
        if (r < 0.94f) return VtuberCardEdition::Signed;
        return VtuberCardEdition::Collab;
    }

    // Mythic
    if (r < 0.18f) return VtuberCardEdition::Standard;
    if (r < 0.48f) return VtuberCardEdition::Foil;
    if (r < 0.63f) return VtuberCardEdition::AltArt;
    if (r < 0.78f) return VtuberCardEdition::Signed;
    return VtuberCardEdition::Collab;
}

inline const char* vtuberCardEditionName(VtuberCardEdition e) {
    switch (e) {
        case VtuberCardEdition::Standard: return "STANDARD";
        case VtuberCardEdition::Foil:     return "FOIL";
        case VtuberCardEdition::AltArt:   return "ALT";
        case VtuberCardEdition::Signed:   return "SIGNED";
        case VtuberCardEdition::Collab:   return "COLLAB";
        default:                          return "STANDARD";
    }
}

inline const char* vtuberCardEditionTag(VtuberCardEdition e) {
    // Short tag for compact UI. Standard intentionally empty.
    switch (e) {
        case VtuberCardEdition::Foil:   return "FOIL";
        case VtuberCardEdition::AltArt: return "ALT";
        case VtuberCardEdition::Signed: return "SIGNED";
        case VtuberCardEdition::Collab: return "COLLAB";
        default:                        return "";
    }
}

inline bool vtuberCardHasSerial(VtuberCardEdition e) {
    return e == VtuberCardEdition::Signed || e == VtuberCardEdition::Collab;
}

inline uint32_t vtuberCollabPartnerSeed(uint32_t seed) {
    // Deterministically derive a partner persona seed for COLLAB cards.
    // Keep it independent from vtuberMixSeed() so small seed changes don't
    // trivially map to the same partner.
    uint32_t p = hash32(seed ^ 0xC011AB1Eu);
    if (p == 0u) p = 1u;
    if (p == seed) p ^= 0x9E3779B9u;
    return p;
}

inline int vtuberCardSerial(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed ^ 0x7A4D2E1Bu));
    // 4-digit serial. (Purely cosmetic.)
    return rng.range(1, 9999);
}

// Economy helpers (used by shop.cpp).
inline int vtuberRarityValueMultiplierPct(VtuberRarity r) {
    switch (r) {
        case VtuberRarity::Common: return 100;
        case VtuberRarity::Rare:   return 180;
        case VtuberRarity::Epic:   return 300;
        case VtuberRarity::Mythic: return 520;
        default:                   return 100;
    }
}

inline int vtuberCardEditionValueMultiplierPct(VtuberCardEdition e) {
    switch (e) {
        case VtuberCardEdition::Foil:   return 135;
        case VtuberCardEdition::AltArt: return 125;
        case VtuberCardEdition::Signed: return 170;
        case VtuberCardEdition::Collab: return 190;
        default:                        return 100;
    }
}

inline std::string vtuberAgency(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed ^ 0xD7C3A5B1u));

    static constexpr const char* SUF[] = {
        "STUDIO", "WORKS", "ARCADE", "LAB", "BUREAU", "HOUSE", "ATELIER", "NETWORK"
    };
    const int sufN = static_cast<int>(sizeof(SUF) / sizeof(SUF[0]));

    // Invented short agency brand.
    const std::string a = vtuberMakeWord(rng, 2, 3);
    const std::string b = vtuberMakeWord(rng, 1, 2);
    std::string name = toUpper(a + b);

    if (rng.chance(0.45f)) {
        name += " " + std::string(SUF[rng.range(0, sufN - 1)]);
    }
    return name;
}

inline std::string vtuberEmote(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed ^ 0x1B873593u));
    static constexpr const char* E[] = {
        "owo", ":3", "^^", "!!", "<3", "?!", "nya~", "~", ":D", ":)"
    };
    const int N = static_cast<int>(sizeof(E) / sizeof(E[0]));
    return std::string(E[rng.range(0, N - 1)]);
}

inline std::string vtuberCatchphrase(uint32_t seed) {
    RNG rng(vtuberMixSeed(seed ^ 0x85EBCA6Bu));

    static constexpr const char* VERB[] = {
        "DIVE", "SPARK", "HACK", "JAM", "CRAFT", "WANDER", "VIBE", "SING"
    };
    static constexpr const char* NOUN[] = {
        "DUNGEON", "PIXELS", "STARS", "LORE", "POTIONS", "RIDDLES", "BOSSES", "CHAOS"
    };
    static constexpr const char* OPEN[] = {
        "WELCOME, CHAT!", "OKAY OKAY!", "ALRIGHT, LISTEN!", "WE'RE LIVE!", "HELLO HELLO!"
    };

    const int vN = static_cast<int>(sizeof(VERB) / sizeof(VERB[0]));
    const int nN = static_cast<int>(sizeof(NOUN) / sizeof(NOUN[0]));
    const int oN = static_cast<int>(sizeof(OPEN) / sizeof(OPEN[0]));

    std::ostringstream ss;
    ss << OPEN[rng.range(0, oN - 1)] << " ";
    ss << "LET'S " << VERB[rng.range(0, vN - 1)] << " THE " << NOUN[rng.range(0, nN - 1)] << "!";
    return ss.str();
}

inline Color vtuberAccentColor(uint32_t seed) {
    // Tiny HSL -> RGB helper for pleasing accent colors.
    auto hue2rgb = [](float p, float q, float t) -> float {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };
    auto hslToRgb = [&](float h, float s, float l) -> Color {
        float r, g, b;
        if (s <= 0.0f) {
            r = g = b = l;
        } else {
            const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
            const float p = 2.0f * l - q;
            r = hue2rgb(p, q, h + 1.0f / 3.0f);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1.0f / 3.0f);
        }
        Color c;
        c.r = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
        c.g = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
        c.b = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
        c.a = 255;
        return c;
    };

    RNG rng(vtuberMixSeed(seed ^ 0x3C6EF372u));
    const float h = (rng.range(0, 359)) / 360.0f;
    const float s = (rng.range(58, 86)) / 100.0f;
    const float l = (rng.range(42, 62)) / 100.0f;
    return hslToRgb(h, s, l);
}
