#pragma once

#include "dungeon.hpp"
#include "game.hpp" // Entity, EntityKind
#include "rng.hpp"

#include <cstdint>
#include <vector>

// Logic-only helpers for forced movement (knockback, collisions, door-smash).
//
// This module mutates:
//  - `ents` (positions and/or hp)
//  - `dung` (doors can smash open)
//
// It does NOT:
//  - push messages
//  - award XP / kill credit
//  - set Game::endCause()

enum class KnockbackStop : uint8_t {
    None = 0,        // moved full distance
    Blocked,         // couldn't move at all (no collision damage)
    SlammedWall,     // blocked by wall/pillar/secret door/bounds; collision damage applied
    SlammedDoor,     // blocked by door that didn't break; collision damage applied
    HitEntity,       // blocked by another entity; collision damage applied
    FellIntoChasm,   // attempted to move into chasm; defender hp set to 0
    CaughtEdge,      // player avoided chasm; damage applied but position unchanged
    ImmuneToChasm,   // defender ignored chasm knockback and position unchanged
    DoorSmashed,     // a door was smashed open
};

struct KnockbackConfig {
    int distance = 1;               // number of tiles to attempt
    int power = 1;                  // affects door smash chance and collision damage
    float playerCatchChance = 0.0f; // used only when defender is the player and would fall into a chasm
    bool allowDoorSmash = true;

    // Collision damage range (inclusive).
    int collisionMin = 1;
    int collisionMax = 3;
};

struct KnockbackResult {
    Vec2i start{0, 0};
    Vec2i end{0, 0};
    int stepsMoved = 0;

    KnockbackStop stop = KnockbackStop::None;

    // Collision metadata
    TileType blockedTile = TileType::Wall;
    int collisionDamageDefender = 0;
    int collisionDamageOther = 0;
    int otherEntityId = 0;

    // Door smash metadata
    bool doorChanged = false;
    Vec2i doorPos{0, 0};
    TileType doorFrom = TileType::Wall;
    TileType doorTo = TileType::Wall;
};

// Applies a knockback to `defenderId` away from `attackerId` by (dx,dy) up to cfg.distance.
// The function mutates the `ents` vector (positions/hp) and can mutate `dung` (doors can smash open).
// Returns a struct describing what happened for the caller to emit messages, award XP, etc.
KnockbackResult applyKnockback(Dungeon& dung,
                               std::vector<Entity>& ents,
                               RNG& rng,
                               int attackerId,
                               int defenderId,
                               int dx,
                               int dy,
                               const KnockbackConfig& cfg);
