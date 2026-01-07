#pragma once

// Shared helper utilities for the Hallucination status effect.
//
// IMPORTANT DESIGN NOTE
// ---------------------
// Hallucination is intended to be a *pure perception hazard*:
// it should never consume RNG state or alter the deterministic simulation.
//
// The functions below therefore derive all "fake" perceptions from stable
// hashes of (run seed, phase, entity/item identity).  This makes the effect
// deterministic, stable for short periods (reduced flicker), and safe for
// replay/state-hash verification.

#include "game.hpp"
#include "rng.hpp"

// A coarse-grained phase used to keep hallucinated mappings stable for a few
// turns to reduce visual flicker.
inline uint32_t hallucinationPhase(uint32_t turns) {
    return turns / 3u;
}

inline uint32_t hallucinationPhase(const Game& game) {
    return hallucinationPhase(game.turns());
}

inline bool isHallucinating(const Game& game) {
    return game.player().effects.hallucinationTurns > 0;
}

inline EntityKind hallucinatedEntityKind(const Game& game, const Entity& e) {
    if (!isHallucinating(game)) return e.kind;

    // Preserve player readability.
    if (e.id == game.playerId()) return e.kind;

    constexpr uint32_t kCount = static_cast<uint32_t>(ENTITY_KIND_COUNT);
    if (kCount <= 1u) return e.kind;

    const uint32_t base = hashCombine(game.seed() ^ 0x6A09E667u, hallucinationPhase(game));
    const uint32_t h = hashCombine(base, static_cast<uint32_t>(e.id) ^ hash32(e.spriteSeed));

    // Exclude Player (0); everything else is fair game.
    const uint32_t k = 1u + (h % (kCount - 1u));
    return static_cast<EntityKind>(k);
}

inline ItemKind hallucinatedItemKind(const Game& game, const Item& it) {
    if (!isHallucinating(game)) return it.kind;

    constexpr uint32_t kCount = static_cast<uint32_t>(ITEM_KIND_COUNT);
    if (kCount == 0u) return it.kind;

    const uint32_t base = hashCombine(game.seed() ^ 0xBB67AE85u, hallucinationPhase(game));
    const uint32_t h = hashCombine(base, static_cast<uint32_t>(it.id) ^ hash32(it.spriteSeed));
    const uint32_t k = (h % kCount);
    return static_cast<ItemKind>(k);
}
