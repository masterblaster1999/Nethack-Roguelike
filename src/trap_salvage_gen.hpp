#pragma once

// Procedural trap salvage generation.
//
// When the player successfully disarms a trap (floor or chest), we can award a
// deterministic crafting byproduct that represents the trap's "essence".
//
// Design goals:
// - Deterministic: derived from (run seed, depth, position, trap kind), so it
//   is stable across saves and does not consume the main RNG stream.
// - Save-compatible: reuses existing ItemKind::EssenceShard encoding.
// - Gameplay-forward: makes trap disarming meaningfully feed into crafting.

#include "game.hpp"          // TrapKind
#include "craft_tags.hpp"    // crafttags::Tag
#include "rng.hpp"           // hash32/hashCombine, _tag

#include <cstdint>

namespace trapsalvage {

struct SalvageSpec {
    crafttags::Tag tag = crafttags::Tag::None;
    int tier = 0;      // 1..12
    int count = 0;     // 0 = none
    bool shiny = false;
    uint32_t spriteSeed = 0;
};

inline crafttags::Tag tagForTrap(TrapKind k) {
    // Map traps onto existing crafting tags.
    switch (k) {
        case TrapKind::Spike:          return crafttags::Tag::Stone;
        case TrapKind::PoisonDart:     return crafttags::Tag::Venom;
        case TrapKind::Teleport:       return crafttags::Tag::Rune;
        case TrapKind::Alarm:          return crafttags::Tag::Arc;
        case TrapKind::Web:            return crafttags::Tag::Shield;
        case TrapKind::ConfusionGas:   return crafttags::Tag::Daze;
        case TrapKind::RollingBoulder: return crafttags::Tag::Stone;
        case TrapKind::TrapDoor:       return crafttags::Tag::Stone;
        case TrapKind::LetheMist:      return crafttags::Tag::Clarity;
        case TrapKind::PoisonGas:      return crafttags::Tag::Venom;
        case TrapKind::CorrosiveGas:   return crafttags::Tag::Alch;
        default:                       return crafttags::Tag::None;
    }
}

inline bool trapIsMagical(TrapKind k) {
    return (k == TrapKind::Teleport) || (k == TrapKind::LetheMist);
}

inline int baseTierForTrap(TrapKind k) {
    // Coarse baseline that sets the "feel" of the salvage.
    switch (k) {
        case TrapKind::Spike:          return 3;
        case TrapKind::PoisonDart:     return 4;
        case TrapKind::Alarm:          return 4;
        case TrapKind::Web:            return 3;
        case TrapKind::ConfusionGas:   return 4;
        case TrapKind::TrapDoor:       return 5;
        case TrapKind::PoisonGas:      return 5;
        case TrapKind::Teleport:       return 6;
        case TrapKind::RollingBoulder: return 6;
        case TrapKind::LetheMist:      return 6;
        case TrapKind::CorrosiveGas:   return 6;
        default:                       return 3;
    }
}

inline uint32_t seedForFloorTrap(uint32_t runSeed, int depth, Vec2i pos, TrapKind k) {
    uint32_t s = hashCombine(runSeed, "TRAP_SALVAGE"_tag);
    s = hashCombine(s, static_cast<uint32_t>(depth));
    s = hashCombine(s, static_cast<uint32_t>(pos.x));
    s = hashCombine(s, static_cast<uint32_t>(pos.y));
    s = hashCombine(s, static_cast<uint32_t>(k));
    return hash32(s ^ 0xA11CE55Eu);
}

inline uint32_t seedForChestTrap(uint32_t runSeed, int depth, uint32_t chestSeed, TrapKind k, int chestTier) {
    uint32_t s = hashCombine(runSeed, "CHEST_SALVAGE"_tag);
    s = hashCombine(s, static_cast<uint32_t>(depth));
    s = hashCombine(s, chestSeed);
    s = hashCombine(s, static_cast<uint32_t>(k));
    s = hashCombine(s, static_cast<uint32_t>(clampi(chestTier, 0, 7)));
    return hash32(s ^ 0xC0FFEE21u);
}

inline SalvageSpec rollSalvage(uint32_t baseSeed, TrapKind k, int depth, bool chest) {
    SalvageSpec out;
    out.tag = tagForTrap(k);
    if (out.tag == crafttags::Tag::None) return out;

    // Chance to salvage anything at all.
    // Chests pay out more often (they're riskier and often more deliberate).
    int chance = chest ? 55 : 33;

    // Depth increases the chance slightly, but cap so it doesn't become mandatory.
    chance += clampi(depth, 0, 30) / 3; // +0..10

    // Very "complex" traps are a bit more likely to yield something useful.
    if (trapIsMagical(k)) chance += 8;
    if (k == TrapKind::RollingBoulder) chance += 6;
    if (k == TrapKind::CorrosiveGas) chance += 6;

    chance = clampi(chance, 10, 85);

    const uint32_t h0 = hash32(baseSeed ^ 0x13579BDFu);
    if ((h0 % 100u) >= static_cast<uint32_t>(chance)) return out;

    // Count: usually 1, sometimes 2 for higher tiers / deeper floors.
    out.count = 1;
    const uint32_t hCount = hash32(baseSeed ^ 0xF00DFACEu);
    const int countChance = (chest ? 14 : 8) + clampi(depth, 0, 20) / 4; // +0..5
    if ((hCount % 100u) < static_cast<uint32_t>(clampi(countChance, 0, 28))) out.count = 2;

    // Tier: trap baseline + depth ramp + small deterministic jitter.
    const int base = baseTierForTrap(k);
    const int dB = clampi(depth, 0, 30) / 4; // 0..7

    const int jitter = static_cast<int>(hash32(baseSeed ^ 0xBADC0DEu) % 3u) - 1; // -1..+1
    int tier = base + dB + jitter;

    if (chest) tier += 1;

    // Cap to the EssenceShard schema range.
    out.tier = clampi(tier, 1, 12);

    // Shiny: rare, but more likely on deeper floors and for magical traps.
    int shinyChance = 4 + clampi(depth, 0, 30) / 3; // 4..14
    if (trapIsMagical(k)) shinyChance += 8;
    if (chest) shinyChance += 5;

    shinyChance = clampi(shinyChance, 1, 45);

    const uint32_t hShiny = hash32(baseSeed ^ 0x51A7D00Du);
    out.shiny = ((hShiny % 100u) < static_cast<uint32_t>(shinyChance));

    // Stable sprite seed: encode tag for visible variation.
    const int tagId = crafttags::tagIndex(out.tag);
    out.spriteSeed = hash32(baseSeed ^ 0x5EED1234u) ^ (static_cast<uint32_t>(tagId) * 0x9E3779B9u);

    return out;
}

} // namespace trapsalvage
