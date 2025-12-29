#pragma once

#include "items.hpp"
#include "rng.hpp"

#include <cstdint>
#include <string>

// Forward declaration (defined in game.hpp).
enum class EntityKind : uint8_t;

// A tiny dice expression: `count` d `sides` + `bonus`.
// Examples:
//   {1,6,0}  => 1d6
//   {2,4,2}  => 2d4+2
struct DiceExpr {
    int count = 1;
    int sides = 4;
    int bonus = 0;
};

// Rolls the dice expression using the game's deterministic RNG.
int rollDice(RNG& rng, DiceExpr d);

// Base damage dice by equipment/monster/projectile.
DiceExpr meleeDiceForWeapon(ItemKind weapon);
DiceExpr meleeDiceForMonster(EntityKind kind);
DiceExpr rangedDiceForProjectile(ProjectileKind proj, bool wandPowered);

// A small strength-style bonus derived from ATK used to scale damage a bit with progression.
// (Used by both player and monsters.)
int statDamageBonusFromAtk(int atk);

// Pretty-prints a dice expression (e.g., "1d6+2").
std::string diceToString(DiceExpr d, bool includeBonus = true);
