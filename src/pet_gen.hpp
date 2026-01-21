#pragma once

// Procedural companion (pet) generation utilities.
//
// Goals:
// - Deterministic from a stable per-entity seed (Entity::spriteSeed).
// - Save-compatible without expanding the save format.
//   We store a compact pet trait bitmask inside the high bits of
//   Entity::procAffixMask (which is already serialized).
//
// NOTE: This is intentionally header-only so pet logic can live alongside the
// split Game translation units (game.cpp, game_loop.cpp, etc.) without needing
// a new compilation unit.

#include "rng.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace petgen {

// We reserve the high byte (bits 24..31) of Entity::procAffixMask for pet traits.
// The existing procedural monster affix system currently occupies low bits.
static constexpr uint32_t PET_TRAIT_SHIFT = 24u;
static constexpr uint32_t PET_TRAIT_MASK  = 0xFFu << PET_TRAIT_SHIFT;

enum class PetTrait : uint8_t {
    None      = 0,
    Sprinter  = 1u << 0, // +speed
    Stout     = 1u << 1, // +HP / +DEF
    Ferocious = 1u << 2, // +ATK

    // Utility / behavior traits (handled by AI + systems).
    Scenthound = 1u << 3, // passive trap detection pings
    Shiny      = 1u << 4, // opportunistic gold pickup
    PackMule   = 1u << 5, // increases carrying capacity
};

inline constexpr uint8_t petTraitBit(PetTrait t) {
    return static_cast<uint8_t>(t);
}

inline constexpr uint8_t petTraitMask(uint32_t procAffixMask) {
    return static_cast<uint8_t>((procAffixMask & PET_TRAIT_MASK) >> PET_TRAIT_SHIFT);
}

inline constexpr void setPetTraitMask(uint32_t& procAffixMask, uint8_t traits) {
    procAffixMask = (procAffixMask & ~PET_TRAIT_MASK) | (static_cast<uint32_t>(traits) << PET_TRAIT_SHIFT);
}

inline constexpr bool petHasTrait(uint32_t procAffixMask, PetTrait t) {
    return (petTraitMask(procAffixMask) & petTraitBit(t)) != 0;
}

inline const char* petTraitName(PetTrait t) {
    switch (t) {
        case PetTrait::Sprinter:  return "SPRINTER";
        case PetTrait::Stout:     return "STOUT";
        case PetTrait::Ferocious: return "FEROCIOUS";
        case PetTrait::Scenthound:return "SCENTHOUND";
        case PetTrait::Shiny:     return "SHINY";
        case PetTrait::PackMule:  return "PACK MULE";
        default:                  return "";
    }
}

inline std::string petTraitList(uint32_t procAffixMask, const char* sep = ", ") {
    const uint8_t m = petTraitMask(procAffixMask);
    if (m == 0) return std::string();

    std::string out;
    bool first = true;
    auto add = [&](PetTrait t) {
        if ((m & petTraitBit(t)) == 0) return;
        if (!first) out += sep;
        first = false;
        out += petTraitName(t);
    };

    add(PetTrait::Sprinter);
    add(PetTrait::Stout);
    add(PetTrait::Ferocious);
    add(PetTrait::Scenthound);
    add(PetTrait::Shiny);
    add(PetTrait::PackMule);
    return out;
}

// Deterministic pet name from a 32-bit seed.
// The syllable lists are intentionally short and pronounceable.
inline std::string petGivenName(uint32_t seed) {
    // Domain-separate from other spriteSeed uses.
    uint32_t h = hash32(seed ^ 0xA17F3D29u);

    static constexpr std::array<const char*, 24> A = {
        "KI","RA","ZU","MO","LA","BE","TH","SH",
        "KA","NA","RO","VE","SI","DA","FI","GU",
        "PA","NO","MI","LU","SA","TA","VA","ZE",
    };
    static constexpr std::array<const char*, 24> B = {
        "RO","LA","MI","NA","ZU","VI","TA","RE",
        "KO","SA","NE","LI","MA","DO","FA","NO",
        "RI","TE","GA","SI","PA","MO","KE","YU",
    };
    static constexpr std::array<const char*, 24> C = {
        "N","R","S","T","K","L","M","Z",
        "TH","SH","ND","RK","NN","SS","TT","KK",
        "RA","NA","LO","MI","ZU","TA","RE","VA",
    };

    const char* a = A[h % A.size()];
    h = hash32(h + 0x9E3779B9u);
    const char* b = B[h % B.size()];
    h = hash32(h + 0x9E3779B9u);
    const char* c = C[h % C.size()];

    const bool threeSyllable = ((h >> 7) & 1u) != 0u;

    std::string out;
    out.reserve(12);
    out += a;
    out += b;
    if (threeSyllable) out += c;
    if (out.empty()) out = "PET";
    return out;
}

// Roll a compact bitmask of pet traits deterministically from seed.
// We keep this conservative (1..2 traits) so pets feel distinct without
// power-spiking too hard.
inline uint8_t petRollTraitMask(uint32_t seed) {
    uint32_t h = hash32(seed ^ 0xC0FFEEu);

    // 1..2 traits.
    const int want = 1 + static_cast<int>((h >> 8) & 1u);

    uint8_t mask = 0;

    auto pick = [&](uint32_t r) -> PetTrait {
        // Weighted mix: mostly combat traits, with a few utility personalities.
        // Keep the total <= 255 so modulo is safe.
        constexpr uint32_t W_SPRINTER  = 3u;
        constexpr uint32_t W_STOUT     = 3u;
        constexpr uint32_t W_FEROCIOUS = 3u;
        constexpr uint32_t W_SCENT     = 2u;
        constexpr uint32_t W_SHINY     = 1u;
        constexpr uint32_t W_MULE      = 2u;
        constexpr uint32_t TOTAL = W_SPRINTER + W_STOUT + W_FEROCIOUS + W_SCENT + W_SHINY + W_MULE;

        uint32_t x = r % TOTAL;
        if (x < W_SPRINTER) return PetTrait::Sprinter;
        x -= W_SPRINTER;
        if (x < W_STOUT) return PetTrait::Stout;
        x -= W_STOUT;
        if (x < W_FEROCIOUS) return PetTrait::Ferocious;
        x -= W_FEROCIOUS;
        if (x < W_SCENT) return PetTrait::Scenthound;
        x -= W_SCENT;
        if (x < W_SHINY) return PetTrait::Shiny;
        return PetTrait::PackMule;
    };

    for (int i = 0; i < want; ++i) {
        // Try a few times to avoid duplicates when we want 2 traits.
        for (int tries = 0; tries < 8; ++tries) {
            h = hash32(h + 0x9E3779B9u + static_cast<uint32_t>(i) * 13u + static_cast<uint32_t>(tries) * 97u);
            const PetTrait t = pick(h);
            const uint8_t bit = petTraitBit(t);
            if ((mask & bit) != 0) continue;
            mask |= bit;
            break;
        }
    }

    // Defensive: ensure at least one trait bit is set.
    if (mask == 0) mask = petTraitBit(PetTrait::Sprinter);
    return mask;
}

} // namespace petgen
