#include "items.hpp"
#include "content.hpp"
#include "vtuber_gen.hpp"
#include "rng.hpp"
#include <algorithm>
#include <sstream>

namespace {

std::string pluralizeStackableName(ItemKind kind, const char* name, int count) {
    std::string s(name);
    if (count <= 1) return s;

    if (isGold(kind)) {
        // "10 GOLD" reads better than "10 GOLDS".
        return s;
    }

    if (kind == ItemKind::Arrow) return "ARROWS";
    if (kind == ItemKind::Rock) return "ROCKS";
    if (kind == ItemKind::Torch) return "TORCHES";

    // Very small "good enough" pluralization for our short item list.
    if (s.rfind("POTION", 0) == 0) {
        // POTION -> POTIONS
        s.insert(6, "S");
        return s;
    }
    if (s.rfind("SCROLL", 0) == 0) {
        // SCROLL -> SCROLLS
        s.insert(6, "S");
        return s;
    }

    if (!s.empty() && s.back() != 'S') s.push_back('S');
    return s;
}



// -----------------------------------------------------------------------------
// Procedural artifacts (rare gear variants)
//
// This is intentionally lightweight: artifacts are stored as a bit flag on Item
// and use deterministic naming based on spriteSeed + kind. Effects can be layered
// on later without needing new save fields.
// -----------------------------------------------------------------------------

constexpr const char* ARTIFACT_PREFIXES[] = {
    "ANCIENT", "OBSIDIAN", "STARFORGED", "IVORY", "EMBER", "FROST", "BLOOD", "SILVER",
    "VOID", "ECHOING", "GILDED", "ASHEN", "SABLE", "RADIANT", "GRIM", "CELESTIAL",
};

constexpr const char* ARTIFACT_NOUNS[] = {
    "WHISPER", "FANG", "EDGE", "WARD", "GLORY", "BANE", "REQUIEM", "AURORA",
    "CROWN", "OATH", "FURY", "ECLIPSE", "VEIL", "BULWARK", "MIRROR", "SPIRAL",
};

constexpr const char* ARTIFACT_POWERS[] = {
    "FLAME", "VENOM", "DAZE", "WARD", "VITALITY",
};

inline uint32_t artifactSeed(const Item& it) {
    uint32_t s = it.spriteSeed;
    if (s == 0u) {
        // Fallback should be stable-ish even for legacy items.
        s = static_cast<uint32_t>(it.id) * 2654435761u;
    }
    s = hashCombine(s ^ 0xA11F00Du, static_cast<uint32_t>(it.kind));
    return hash32(s ^ 0xC0FFEEu);
}

inline const char* artifactPowerTag(const Item& it) {
    constexpr size_t n = sizeof(ARTIFACT_POWERS) / sizeof(ARTIFACT_POWERS[0]);
    const uint32_t h = artifactSeed(it);
    return ARTIFACT_POWERS[(n == 0) ? 0 : (h % static_cast<uint32_t>(n))];
}

inline std::string artifactTitle(const Item& it) {
    const uint32_t h = artifactSeed(it);
    constexpr size_t np = sizeof(ARTIFACT_PREFIXES) / sizeof(ARTIFACT_PREFIXES[0]);
    constexpr size_t nn = sizeof(ARTIFACT_NOUNS) / sizeof(ARTIFACT_NOUNS[0]);

    const char* pre = ARTIFACT_PREFIXES[(np == 0) ? 0 : ((h >> 8) % static_cast<uint32_t>(np))];
    const char* noun = ARTIFACT_NOUNS[(nn == 0) ? 0 : ((h >> 16) % static_cast<uint32_t>(nn))];

    std::string s;
    s.reserve(32);
    s += pre;
    s += " ";
    s += noun;
    return s;
}

} // namespace

const ItemDef& itemDef(ItemKind k) {
    // Keep in sync with enum ordering.
    static const ItemDef baseDefs[] = {
        // Weapons
        { ItemKind::Dagger,         "DAGGER",            false, false, false, EquipSlot::MeleeWeapon,  1, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 10, 8 },
        { ItemKind::Sword,          "SWORD",             false, false, false, EquipSlot::MeleeWeapon,  2, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 20, 20 },
        { ItemKind::Bow,            "BOW",               false, false, false, EquipSlot::RangedWeapon, 0, 1, 0, 8, AmmoKind::Arrow, ProjectileKind::Arrow, 0, 0, 0, 15, 25 },
        { ItemKind::Sling,          "SLING",             false, false, false, EquipSlot::RangedWeapon, 0, 1, 0, 7, AmmoKind::Rock,  ProjectileKind::Rock,  0, 0, 0, 10, 15 },
        { ItemKind::WandSparks,     "WAND OF SPARKS",    false, false, false, EquipSlot::RangedWeapon, 0, 2, 0, 7, AmmoKind::None,  ProjectileKind::Spark, 12, 0, 0, 5, 60 },

        // Armor
        { ItemKind::LeatherArmor,   "LEATHER ARMOR",     false, false, false, EquipSlot::Armor,       0, 0, 1, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 25, 25 },
        { ItemKind::ChainArmor,     "CHAIN ARMOR",       false, false, false, EquipSlot::Armor,       0, 0, 2, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 40, 45 },

        // Consumables
        { ItemKind::PotionHealing,  "POTION OF HEALING",  true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 6, 0, 2, 30 },
        { ItemKind::PotionStrength, "POTION OF STRENGTH", true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 2, 55 },
        { ItemKind::ScrollTeleport, "SCROLL OF TELEPORT", true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 80 },
        { ItemKind::ScrollMapping,  "SCROLL OF MAPPING",  true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 50 },

        // Quest / special
        { ItemKind::AmuletYendor,   "AMULET OF YENDOR",   false, false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 5, 0 },

        // Ammo / misc
        { ItemKind::Arrow,          "ARROW",             true,  false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Arrow, 0, 0, 0, 1, 2 },
        { ItemKind::Rock,           "ROCK",              true,  false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 2, 1 },
        { ItemKind::Gold,           "GOLD",              true,  false, true,  EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 0, 0 },

        // New consumables / progression
        { ItemKind::PotionAntidote,      "POTION OF ANTIDOTE",       true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 35 },
        { ItemKind::PotionRegeneration,  "POTION OF REGENERATION",   true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 70 },
        { ItemKind::PotionShielding,     "POTION OF STONESKIN",      true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 75 },
        { ItemKind::ScrollEnchantWeapon, "SCROLL OF ENCHANT WEAPON", true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 90 },
        { ItemKind::ScrollEnchantArmor,  "SCROLL OF ENCHANT ARMOR",  true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 90 },

        // Even newer consumables (append-only)
        { ItemKind::PotionHaste,         "POTION OF HASTE",          true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 70 },
        { ItemKind::PotionVision,        "POTION OF VISION",         true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 40 },

        // Identification / utility (append-only)
        { ItemKind::ScrollIdentify,      "SCROLL OF IDENTIFY",      true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 60 },

        // New items (append-only)
        { ItemKind::Axe,               "AXE",                false, false, false, EquipSlot::MeleeWeapon, 3, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 25, 30 },
        { ItemKind::PlateArmor,        "PLATE ARMOR",        false, false, false, EquipSlot::Armor,       0, 0, 3, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 60, 80 },
        { ItemKind::FoodRation,        "FOOD RATION",        true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 2, 250, 6, 12 },
        { ItemKind::ScrollDetectTraps, "SCROLL OF DETECT TRAPS", true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 40 },
        { ItemKind::ScrollDetectSecrets, "SCROLL OF DETECT SECRETS", true, true, false, EquipSlot::None,       0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 50 },

        // Misc (append-only)
        { ItemKind::Key,              "KEY",               true,  false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 20 },
        { ItemKind::Lockpick,         "LOCKPICK",          true,  false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 40 },
        { ItemKind::ScrollKnock,       "SCROLL OF KNOCK",   true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 50 },

        // Dungeon features (append-only)
        { ItemKind::Chest,            "CHEST",            false, false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 0, 0 },
        { ItemKind::ChestOpen,        "OPEN CHEST",       false, false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 0, 0 },
        { ItemKind::PotionInvisibility, "POTION OF INVISIBILITY",  true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 80 },

        // Lighting (append-only)
        { ItemKind::Torch,            "TORCH",             true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 10 },
        { ItemKind::TorchLit,         "LIT TORCH",         false, false, false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 0 },

        // Curses / blessings (append-only)
        { ItemKind::ScrollRemoveCurse, "SCROLL OF REMOVE CURSE", true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 120 },

        // Mind / control (append-only)
        { ItemKind::PotionClarity,   "POTION OF CLARITY",   true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 2, 45 },
        { ItemKind::ScrollConfusion, "SCROLL OF CONFUSION", true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 85 },

        // Terrain / digging (append-only)
        { ItemKind::Pickaxe,          "PICKAXE",           false, false, false, EquipSlot::MeleeWeapon,  1, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 9, 55 },
        { ItemKind::WandDigging,      "WAND OF DIGGING",   false, false, false, EquipSlot::RangedWeapon, 0, 0, 0, 7, AmmoKind::None,  ProjectileKind::Spark, 8, 0, 0, 5, 90 },

        // Explosives / magic (append-only)
        { ItemKind::WandFireball,     "WAND OF FIREBALL",  false, false, false, EquipSlot::RangedWeapon, 0, 2, 0, 6, AmmoKind::None,  ProjectileKind::Fireball, 6, 0, 0, 5, 140 },

        // Corpses (append-only)
        { ItemKind::CorpseGoblin,     "GOBLIN CORPSE",     true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 1, 120,  9, 0 },
        { ItemKind::CorpseOrc,        "ORC CORPSE",        true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 1, 140, 10, 0 },
        { ItemKind::CorpseBat,        "BAT CORPSE",        true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 0,  70,  6, 0 },
        { ItemKind::CorpseSlime,      "SLIME GLOB",        true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 0,  45,  8, 0 },
        { ItemKind::CorpseKobold,     "KOBOLD CORPSE",     true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 1, 110,  9, 0 },
        { ItemKind::CorpseWolf,       "WOLF CORPSE",       true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 2, 200, 14, 0 },
        { ItemKind::CorpseTroll,      "TROLL CORPSE",      true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 2, 230, 18, 0 },
        { ItemKind::CorpseWizard,     "WIZARD CORPSE",     true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 1, 130, 10, 0 },
        { ItemKind::CorpseSnake,      "SNAKE CORPSE",      true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 1,  90,  8, 0 },
        { ItemKind::CorpseSpider,     "SPIDER CORPSE",     true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 1,  90,  9, 0 },
        { ItemKind::CorpseOgre,       "OGRE CORPSE",       true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 3, 260, 22, 0 },
        { ItemKind::CorpseMimic,      "MIMIC REMAINS",     true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 1, 160, 14, 0 },
        { ItemKind::CorpseMinotaur,   "MINOTAUR CORPSE",   true, true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 4, 300, 26, 0 },

        // Rings (append-only)
        // Note: Rings use ItemDef::mod* stats and can also contribute to defense.
        { ItemKind::RingMight,        "RING OF MIGHT",      false, false, false, EquipSlot::Ring,       0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 0, 0, 1, 160, 2, 0, 0, 0 },
        { ItemKind::RingAgility,      "RING OF AGILITY",    false, false, false, EquipSlot::Ring,       0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 0, 0, 1, 160, 0, 2, 0, 0 },
        { ItemKind::RingFocus,        "RING OF FOCUS",      false, false, false, EquipSlot::Ring,       0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 0, 0, 1, 160, 0, 0, 0, 2 },
        { ItemKind::RingProtection,   "RING OF PROTECTION", false, false, false, EquipSlot::Ring,       0, 0, 1, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 0, 0, 1, 190, 0, 0, 0, 0 },

        // Traversal (append-only)
        { ItemKind::PotionLevitation, "POTION OF LEVITATION", true, true, false, EquipSlot::None,       0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,     0, 0, 0, 2, 95 },

        // Morale / control (append-only)
        { ItemKind::ScrollFear, "SCROLL OF FEAR", true,  true,  false, EquipSlot::None,        0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 90 },

        // Terrain / fortification (append-only)
        { ItemKind::ScrollEarth, "SCROLL OF EARTH", true,  true,  false, EquipSlot::None,      0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 110 },

        // Pets / companions (append-only)
        { ItemKind::ScrollTaming, "SCROLL OF TAMING", true,  true,  false, EquipSlot::None,   0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 200 },

        // Perception / weirdness (append-only)
        { ItemKind::PotionHallucination, "POTION OF HALLUCINATION", true, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 70 },

        // Mana / magic (append-only)
        { ItemKind::PotionEnergy, "POTION OF ENERGY", true, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 85 },

        // Spellbooks (append-only)
        { ItemKind::SpellbookMagicMissile, "SPELLBOOK OF MAGIC MISSILE", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 3, 120 },
        { ItemKind::SpellbookBlink, "SPELLBOOK OF BLINK", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 3, 160 },
        { ItemKind::SpellbookMinorHeal, "SPELLBOOK OF MINOR HEAL", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 3, 140 },
        { ItemKind::SpellbookDetectTraps, "SPELLBOOK OF DETECT TRAPS", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 3, 150 },
        { ItemKind::SpellbookFireball, "SPELLBOOK OF FIREBALL", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 4, 300 },
        { ItemKind::SpellbookStoneskin, "SPELLBOOK OF STONESKIN", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 3, 180 },
        { ItemKind::SpellbookHaste, "SPELLBOOK OF HASTE", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 3, 200 },
        { ItemKind::SpellbookInvisibility, "SPELLBOOK OF INVISIBILITY", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 3, 220 },
        { ItemKind::SpellbookPoisonCloud, "SPELLBOOK OF POISON CLOUD", false, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 4, 260 },
        { ItemKind::RingSearching, "RING OF SEARCHING", false, false, false, EquipSlot::Ring, 0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock, 0, 0, 0, 1, 200, 0, 0, 0, 0 },
        { ItemKind::RingSustenance, "RING OF SUSTENANCE", false, false, false, EquipSlot::Ring, 0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock, 0, 0, 0, 1, 190, 0, 0, 0, 0 },
        { ItemKind::ScrollEnchantRing, "SCROLL OF ENCHANT RING", true,  true,  false, EquipSlot::None,   0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 95 },
        { ItemKind::VtuberFigurine,  "VTUBER FIGURINE",       false, false, false, EquipSlot::None,   0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 2, 180 },
        { ItemKind::VtuberHoloCard,  "VTUBER HOLOCARD",       false, false, false, EquipSlot::None,   0, 0, 0, 0, AmmoKind::None,  ProjectileKind::Rock,  0, 0, 0, 1, 120 },
    };

    static std::vector<ItemDef> defs;
    static uint32_t appliedGen = 0;

    const uint32_t gen = contentOverridesGeneration();
    if (defs.empty() || appliedGen != gen) {
        defs.assign(baseDefs, baseDefs + (sizeof(baseDefs) / sizeof(baseDefs[0])));

        // Apply optional balance/content overrides (runtime).
        const auto& ovs = contentOverrides().items;
        for (const auto& kv : ovs) {
            const ItemKind kind = kv.first;
            const ItemDefOverride& o = kv.second;
            const size_t j = static_cast<size_t>(kind);
            if (j >= defs.size()) continue;
            ItemDef& d = defs[j];
            if (d.kind != kind) continue;

            if (o.meleeAtk) d.meleeAtk = *o.meleeAtk;
            if (o.rangedAtk) d.rangedAtk = *o.rangedAtk;
            if (o.defense) d.defense = *o.defense;
            if (o.range) d.range = *o.range;
            if (o.maxCharges) d.maxCharges = *o.maxCharges;
            if (o.healAmount) d.healAmount = *o.healAmount;
            if (o.hungerRestore) d.hungerRestore = *o.hungerRestore;
            if (o.weight) d.weight = *o.weight;
            if (o.value) d.value = *o.value;
            if (o.modMight) d.modMight = *o.modMight;
            if (o.modAgility) d.modAgility = *o.modAgility;
            if (o.modVigor) d.modVigor = *o.modVigor;
            if (o.modFocus) d.modFocus = *o.modFocus;

            // Basic safety clamps.
            d.range = std::max(0, d.range);
            d.maxCharges = std::max(0, d.maxCharges);
            d.healAmount = std::max(0, d.healAmount);
            d.hungerRestore = std::max(0, d.hungerRestore);
            d.weight = std::max(0, d.weight);
            d.value = std::max(0, d.value);
        }

        appliedGen = gen;
    }

    const size_t idx = static_cast<size_t>(k);
    if (idx >= defs.size()) {
        return defs[0];
    }
    return defs[idx];
}

std::string itemDisplayNameSingle(ItemKind k) {
    return itemDef(k).name;
}

std::string itemDisplayName(const Item& it) {
    const ItemDef& d = itemDef(it.kind);
    std::ostringstream ss;

    // Prefix BUC (blessed/uncursed/cursed) + enchantment for non-stackable equipment.
    // (We intentionally keep new ItemKind values appended to preserve old save compatibility.)
    if (isWearableGear(it.kind)) {
        if (itemIsArtifact(it)) ss << "ARTIFACT ";
        if (it.buc < 0) ss << "CURSED ";
        else if (it.buc > 0) ss << "BLESSED ";

        if (it.enchant != 0) {
            if (it.enchant > 0) ss << "+";
            ss << it.enchant << " ";
        }

        // Ego / brand prefix (rare).
        if (it.ego != ItemEgo::None) {
            const char* p = egoPrefix(it.ego);
            if (p && p[0]) ss << p << " ";
        }
    }


    if (it.kind == ItemKind::VtuberFigurine) {
        ss << d.name;
        if (it.spriteSeed != 0u) {
            ss << ": " << vtuberStageName(it.spriteSeed);
            ss << " (" << vtuberArchetype(it.spriteSeed) << ")";
        }
    } else if (it.kind == ItemKind::VtuberHoloCard) {
        ss << d.name;
        if (it.spriteSeed != 0u) {
            const uint32_t s = it.spriteSeed;
            const VtuberRarity rar = vtuberRarity(s);
            const VtuberCardEdition ed = vtuberCardEdition(s);

            ss << ": ";
            if (ed == VtuberCardEdition::Collab) {
                const uint32_t ps = vtuberCollabPartnerSeed(s);
                ss << vtuberStageName(s) << " x " << vtuberStageName(ps);
            } else {
                ss << vtuberStageName(s);
            }

            ss << " [" << vtuberRarityName(rar) << "]";

            const char* et = vtuberCardEditionTag(ed);
            if (et && et[0]) {
                ss << " {" << et << "}";
                if (vtuberCardHasSerial(ed)) {
                    ss << " #" << vtuberCardSerial(s);
                }
            }
        }
    } else if (d.stackable && it.count > 1) {
        ss << it.count << " " << pluralizeStackableName(it.kind, d.name, it.count);
    } else {
        ss << d.name;
    }

    if (itemIsArtifact(it) && isWearableGear(it.kind)) {
        ss << " '" << artifactTitle(it) << "'";
        const char* p = artifactPowerTag(it);
        if (p && p[0]) ss << " {" << p << "}";
    }

    if (it.kind == ItemKind::TorchLit) {
        ss << " (" << it.charges << "T)";
    } else if (isCorpseKind(it.kind)) {
        // Corpses decay (charges = remaining freshness in turns).
        // We display a coarse stage rather than the exact timer.
        const int ch = it.charges;
        if (ch <= 0) ss << " (ROTTEN)";
        else if (ch <= 60) ss << " (ROTTEN)";
        else if (ch <= 160) ss << " (STALE)";
        else ss << " (FRESH)";
    } else if (d.maxCharges > 0) {
        ss << " (" << it.charges << "/" << d.maxCharges << ")";
    }

    // Shop tag: show the total price for the stack (or 1 unit for non-stackables).
    if (it.shopPrice > 0 && it.shopDepth > 0) {
        const int n = d.stackable ? std::max(1, it.count) : 1;
        const int total = it.shopPrice * n;
        ss << " [PRICE " << total << "G]";
    }
    return ss.str();
}

int itemWeight(const Item& it) {
    const ItemDef& d = itemDef(it.kind);
    if (d.weight <= 0) return 0;

    const int n = d.stackable ? std::max(0, it.count) : 1;
    return d.weight * n;
}

int totalWeight(const std::vector<Item>& items) {
    int w = 0;
    for (const auto& it : items) {
        w += itemWeight(it);
    }
    return w;
}

int countGold(const std::vector<Item>& inv) {
    int g = 0;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::Gold) g += std::max(0, it.count);
    }
    return g;
}

int findItemIndexById(const std::vector<Item>& inv, int itemId) {
    for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
        if (inv[static_cast<size_t>(i)].id == itemId) return i;
    }
    return -1;
}

int findFirstAmmoIndex(const std::vector<Item>& inv, AmmoKind ammo) {
    if (ammo == AmmoKind::None) return -1;
    for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
        const auto& it = inv[static_cast<size_t>(i)];
        if (ammo == AmmoKind::Arrow && it.kind == ItemKind::Arrow && it.count > 0) return i;
        if (ammo == AmmoKind::Rock && it.kind == ItemKind::Rock && it.count > 0) return i;
    }
    return -1;
}

int ammoCount(const std::vector<Item>& inv, AmmoKind ammo) {
    int c = 0;
    if (ammo == AmmoKind::None) return 0;
    for (const auto& it : inv) {
        if (ammo == AmmoKind::Arrow && it.kind == ItemKind::Arrow) c += std::max(0, it.count);
        if (ammo == AmmoKind::Rock && it.kind == ItemKind::Rock) c += std::max(0, it.count);
    }
    return c;
}

bool consumeAmmo(std::vector<Item>& inv, AmmoKind ammo, int amount) {
    if (ammo == AmmoKind::None) return true;

    int need = amount;
    for (auto& it : inv) {
        if (need <= 0) break;
        if (ammo == AmmoKind::Arrow && it.kind == ItemKind::Arrow) {
            int take = std::min(it.count, need);
            it.count -= take;
            need -= take;
        } else if (ammo == AmmoKind::Rock && it.kind == ItemKind::Rock) {
            int take = std::min(it.count, need);
            it.count -= take;
            need -= take;
        }
    }

    // Remove emptied stackables (ammo, gold, potions, scrolls, ...)
    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& it) {
        return isStackable(it.kind) && it.count <= 0;
    }), inv.end());

    return need <= 0;
}


bool consumeOneAmmo(std::vector<Item>& inv, AmmoKind ammo, Item* outConsumed) {
    if (ammo == AmmoKind::None) return true;

    const ItemKind k = (ammo == AmmoKind::Arrow) ? ItemKind::Arrow : ItemKind::Rock;

    for (size_t i = 0; i < inv.size(); ++i) {
        Item& it = inv[i];
        if (it.kind != k) continue;
        if (it.count <= 0) continue;

        if (outConsumed) {
            *outConsumed = it;
            outConsumed->count = 1;
        }

        it.count -= 1;

        // Remove emptied stackables (ammo, gold, potions, scrolls, ...)
        inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& v) {
            return isStackable(v.kind) && v.count <= 0;
        }), inv.end());

        return true;
    }

    return false;
}

bool tryStackItem(std::vector<Item>& inv, const Item& incoming) {
    if (!isStackable(incoming.kind)) return false;

    // For stackables we require the important per-item metadata to match.
    // (This keeps future extensions like blessed/cursed consumables safe.)
    for (auto& it : inv) {
        if (it.kind != incoming.kind) continue;
        if (it.charges != incoming.charges) continue;
        if (it.enchant != incoming.enchant) continue;
        if (it.buc != incoming.buc) continue;
        if (it.ego != incoming.ego) continue;
        if (it.flags != incoming.flags) continue;
        if (it.shopPrice != incoming.shopPrice) continue;
        if (it.shopDepth != incoming.shopDepth) continue;

        it.count += incoming.count;
        return true;
    }
    return false;
}
