#include "spells.hpp"

#include <array>

const SpellDef& spellDef(SpellKind k) {
    // Keep in sync with SpellKind ordering (append-only).
    static constexpr std::array<SpellDef, SPELL_KIND_COUNT> defs = {{
        { SpellKind::MagicMissile, "MAGIC MISSILE", "A SIMPLE ARCANE BOLT.", 2, 8, true, ProjectileKind::Spark },
        { SpellKind::Blink,        "BLINK",         "SHORT-RANGE TELEPORT.", 3, 6, true, ProjectileKind::Spark },
        { SpellKind::MinorHeal,    "MINOR HEAL",    "RESTORE A BIT OF HEALTH.", 3, 0, false, ProjectileKind::Spark },
        { SpellKind::DetectTraps,  "DETECT TRAPS",  "REVEAL TRAPS NEARBY.", 4, 0, false, ProjectileKind::Spark },
        { SpellKind::Fireball,     "FIREBALL",      "HURL A BURST OF FLAME.", 6, 7, true, ProjectileKind::Fireball },

        // New spells (append-only)
        { SpellKind::Stoneskin,    "STONESKIN",     "HARDEN YOUR SKIN TO TURN ASIDE BLOWS.", 4, 0, false, ProjectileKind::Spark },
        { SpellKind::Haste,        "HASTE",         "MOVE WITH SUPERNATURAL SPEED.", 5, 0, false, ProjectileKind::Spark },
        { SpellKind::Invisibility, "INVISIBILITY",  "FADE FROM SIGHT FOR A TIME.", 5, 0, false, ProjectileKind::Spark },
        { SpellKind::PoisonCloud,  "POISON CLOUD",  "CONJURE A LINGERING CLOUD OF TOXIC VAPOR.", 6, 7, true, ProjectileKind::Spark },
    }};

    const size_t idx = static_cast<size_t>(k);
    if (idx >= defs.size()) {
        return defs[0];
    }
    return defs[idx];
}
