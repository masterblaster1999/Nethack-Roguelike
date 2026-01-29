#pragma once

#include "dungeon.hpp"
#include "items.hpp"

// Ecosystem-aware loot ecology helpers.
//
// Design goals:
//  - Keep ecosystem biases deterministic and *small* (nudges, not hard rules).
//  - Expose weight deltas as pure functions so they can be unit tested.
//  - Avoid requiring new serialized fields: biases are derived from existing
//    per-floor ecosystem caches + spawn positions.
//
// These helpers are intentionally conservative so the core item tables remain
// recognizable while biome regions still feel distinct.

inline int ecoWeaponEgoWeightDelta(EcosystemKind eco, ItemEgo ego) {
    // Positive values make an ego more likely in that ecosystem.
    // Negative values make it less likely.
    //
    // These values are applied on top of the baseline ego weights in game_spawn.cpp.
    // Keep magnitudes relatively small (roughly within +/-25) so the base ecology
    // still matters (room type, depth, substrate, shops, etc.).

    switch (eco) {
        case EcosystemKind::FungalBloom: {
            switch (ego) {
                case ItemEgo::Venom:     return +22;
                case ItemEgo::Webbing:   return +12;
                case ItemEgo::Flaming:   return -10;
                case ItemEgo::Corrosive: return -4;
                default:                 return 0;
            }
        }
        case EcosystemKind::CrystalGarden: {
            switch (ego) {
                case ItemEgo::Dazing:    return +18;
                case ItemEgo::Webbing:   return +6;
                case ItemEgo::Venom:     return -6;
                default:                 return 0;
            }
        }
        case EcosystemKind::BoneField: {
            switch (ego) {
                case ItemEgo::Vampiric:  return +16;
                case ItemEgo::Dazing:    return +6;
                case ItemEgo::Webbing:   return -4;
                default:                 return 0;
            }
        }
        case EcosystemKind::RustVeins: {
            switch (ego) {
                case ItemEgo::Corrosive: return +22;
                case ItemEgo::Dazing:    return +5;
                case ItemEgo::Flaming:   return -8;
                default:                 return 0;
            }
        }
        case EcosystemKind::AshenRidge: {
            switch (ego) {
                case ItemEgo::Flaming:   return +22;
                case ItemEgo::Corrosive: return +6;
                case ItemEgo::Webbing:   return -10;
                default:                 return 0;
            }
        }
        case EcosystemKind::FloodedGrotto: {
            switch (ego) {
                case ItemEgo::Webbing:   return +10;
                case ItemEgo::Flaming:   return -14;
                case ItemEgo::Corrosive: return -4;
                default:                 return 0;
            }
        }
        default:
            break;
    }

    return 0;
}

inline float ecoWeaponEgoChanceMul(EcosystemKind eco) {
    // Multiplier applied to the *chance of any ego* appearing.
    // Keep this close to 1.0 to avoid destabilizing loot balance.
    switch (eco) {
        case EcosystemKind::CrystalGarden: return 1.12f;
        case EcosystemKind::AshenRidge:    return 1.10f;
        case EcosystemKind::FloodedGrotto: return 0.86f;
        default:                           return 1.0f;
    }
}

