#pragma once

#include "items.hpp" // ProjectileKind

#include <cstdint>

// Basic spell system (NetHack-inspired): spells are learned via spellbooks
// and consume mana to cast.
//
// NOTE: SpellKind ids must remain stable across saves/replays.
// Always append new spells to the end.

enum class SpellKind : uint8_t {
    MagicMissile = 0,
    Blink,
    MinorHeal,
    DetectTraps,
    Fireball,
};

inline constexpr int SPELL_KIND_COUNT = static_cast<int>(SpellKind::Fireball) + 1;

struct SpellDef {
    SpellKind kind;
    const char* name;
    const char* desc;

    int manaCost = 0;

    // Max cast range in tiles (0 = self/ambient spell).
    int range = 0;

    // True if the spell expects a target tile.
    bool needsTarget = false;

    // Visual projectile (for targeted spells). Non-targeted spells may still
    // set this to a default value.
    ProjectileKind projectile = ProjectileKind::Spark;
};

const SpellDef& spellDef(SpellKind k);

inline const char* spellName(SpellKind k) { return spellDef(k).name; }
