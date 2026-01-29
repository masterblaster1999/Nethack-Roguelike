#include "items.hpp"
#include "content.hpp"
#include "vtuber_gen.hpp"
#include "pet_gen.hpp"
#include "fishing_gen.hpp"
#include "farm_gen.hpp"
#include "butcher_gen.hpp"
#include "bounty_gen.hpp"
#include "craft_tags.hpp"
#include "proc_spells.hpp"
#include "game.hpp"
#include "rng.hpp"
#include "artifact_gen.hpp"
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
    if (kind == ItemKind::Fish) return "FISH";
    if (kind == ItemKind::Seed) return "SEEDS";

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
        { ItemKind::CaptureSphere,     "CAPTURE SPHERE",        true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 120 },
        { ItemKind::MegaSphere,        "MEGA SPHERE",           true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 220 },
        { ItemKind::CaptureSphereFull, "CAPTURE SPHERE",        false, true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 0 },
        { ItemKind::MegaSphereFull,    "MEGA SPHERE",           false, true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 0 },

        // Fishing (append-only)
        { ItemKind::FishingRod,   "FISHING ROD",        false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 40, 0, 0, 6, 45 },
        { ItemKind::Fish,         "FISH",               true,  false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 2, 0 },

        // Farming (append-only)
        { ItemKind::GardenHoe,    "GARDEN HOE",         false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 60, 0, 0, 6, 55 },
        { ItemKind::Seed,         "SEED",               true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 0 },
        { ItemKind::TilledSoil,   "TILLED SOIL",        false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 0, 0 },
        { ItemKind::CropSprout,   "CROP SPROUT",        false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 0, 0 },
        { ItemKind::CropGrowing,  "GROWING CROP",       false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 0, 0 },
        { ItemKind::CropMature,   "MATURE CROP",        false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 0, 0 },
        { ItemKind::CropProduce,  "CROP",               true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 0 },

        // Crafting (append-only)
        { ItemKind::CraftingKit,  "CRAFTING KIT",       false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 4, 75 },
        { ItemKind::BountyContract, "BOUNTY CONTRACT", false, true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 0 },
    
        // Procedural rune magic (append-only)
        // Rune Tablets are consumables (read/use) even before the full procedural spell casting
        // vertical slice lands.
        { ItemKind::RuneTablet, "RUNE TABLET", false, true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 6, 200 },

        // Butchering outputs (append-only)
        { ItemKind::ButcheredMeat,  "MEAT",  true,  true,  false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 300, 0, 25, 4, 4 },
        { ItemKind::ButcheredHide,  "HIDE",  true,  false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0,   0, 0,  6, 10 },
        { ItemKind::ButcheredBones, "BONES", true,  false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0,   0, 0,  3, 6 },

        // Procedural crafting byproducts (append-only)
        { ItemKind::EssenceShard, "ESSENCE SHARD", true, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0, 0, 1, 45 },

        // Ecosystem resource nodes (append-only)
        // Stationary ground props spawned near biome seeds; harvest with CONFIRM.
        { ItemKind::SporePod,      "SPORE POD",      false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock },
        { ItemKind::CrystalNode,   "CRYSTAL NODE",   false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock },
        { ItemKind::BonePile,      "BONE PILE",      false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock },
        { ItemKind::RustVent,      "RUST VENT",      false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock },
        { ItemKind::AshVent,       "ASH VENT",       false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock },
        { ItemKind::GrottoSpring,  "GROTTO SPRING",  false, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock },
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


    if (isCaptureSphereFullKind(it.kind)) {
        ss << d.name;
        const int rawKind = it.enchant;
        EntityKind ek = EntityKind::Goblin;
        bool kindOk = (rawKind >= 0 && rawKind < ENTITY_KIND_COUNT);
        if (kindOk) ek = static_cast<EntityKind>(rawKind);

        const std::string petName = (it.spriteSeed != 0u) ? petgen::petGivenName(it.spriteSeed) : std::string("UNKNOWN");
        const int bond = clampi(captureSphereBondFromCharges(it.charges), 0, 99);
        const int hpPct = clampi(captureSphereHpPctFromCharges(it.charges), 0, 100);
        const int level = clampi(captureSpherePetLevelOrDefault(it.charges), 1, captureSpherePetLevelCap());

        ss << ": " << petName;
        if (kindOk) ss << " THE " << entityKindName(ek);
        else ss << " THE CREATURE";
        ss << " {LV " << level << "}";
        ss << " {BOND " << bond << "}";
        ss << " {" << hpPct << "% HP}";
    } else if (it.kind == ItemKind::Seed) {

        // Procedural seeds: crop seed typically stored in charges.
        uint32_t cropSeed = 0u;
        if (it.charges != 0) {
            cropSeed = cropSeedFromCharges(it.charges);
        } else if (it.spriteSeed != 0u) {
            cropSeed = it.spriteSeed;
        } else {
            cropSeed = hash32(static_cast<uint32_t>(it.id) ^ 0x53EED123u);
        }

        const bool hasMeta = (it.enchant != 0);
        const int rarityHint  = hasMeta ? cropRarityFromEnchant(it.enchant) : -1;
        const int variantHint = hasMeta ? cropVariantFromEnchant(it.enchant) : -1;
        const int shinyHint   = hasMeta ? (cropIsShinyFromEnchant(it.enchant) ? 1 : 0) : -1;

        const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);

        if (it.count > 1) ss << it.count << " ";
        ss << "SEEDS: " << cs.name;
        ss << " [" << farmgen::cropRarityName(cs.rarity) << "]";
        if (cs.shiny) ss << " {SHINY}";
        if (cs.bonusTag && cs.bonusTag[0]) ss << " {" << cs.bonusTag << "}";

    } else if (it.kind == ItemKind::CropProduce) {

        // Procedural crop produce: uses the same crop seed/meta packing as seeds.
        uint32_t cropSeed = 0u;
        if (it.charges != 0) {
            cropSeed = cropSeedFromCharges(it.charges);
        } else if (it.spriteSeed != 0u) {
            cropSeed = it.spriteSeed;
        } else {
            cropSeed = hash32(static_cast<uint32_t>(it.id) ^ 0xC20BB33Fu);
        }

        const bool hasMeta = (it.enchant != 0);
        const int rarityHint  = hasMeta ? cropRarityFromEnchant(it.enchant) : -1;
        const int variantHint = hasMeta ? cropVariantFromEnchant(it.enchant) : -1;
        const int shinyHint   = hasMeta ? (cropIsShinyFromEnchant(it.enchant) ? 1 : 0) : -1;

        const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);

        const int qRaw = cropQualityFromEnchant(it.enchant);
        const int qIdx = clampi(qRaw, 0, 4);

        if (it.count > 1) ss << it.count << " ";
        ss << cs.name;
        ss << " [" << farmgen::cropRarityName(cs.rarity) << "]";
        ss << " {" << farmgen::qualityGradeName(qIdx) << "}";
        if (cs.shiny) ss << " {SHINY}";
        if (cs.bonusTag && cs.bonusTag[0]) ss << " {" << cs.bonusTag << "}";

    } else if (it.kind == ItemKind::TilledSoil) {

        ss << d.name;
        const int fert = clampi(tilledSoilFertilityFromEnchant(it.enchant), 0, 100);
        ss << " {FERT " << fert << "}";

        const int affinity = tilledSoilAffinityFromEnchant(it.enchant);
        if (affinity >= 0) {
            ss << " {AFF " << farmgen::farmTagByIndex(affinity) << "}";
        }

    } else if (isEcosystemNodeKind(it.kind)) {

        ss << d.name;

        // Remaining harvest uses stored in charges (defaults to 1 if unset).
        int taps = it.charges;
        if (taps <= 0) taps = 1;
        if (taps != 1) ss << " {" << taps << " TAPS}";

    } else if (isFarmPlantKind(it.kind)) {

        uint32_t cropSeed = 0u;
        if (it.spriteSeed != 0u) cropSeed = it.spriteSeed;
        else if (it.charges != 0) cropSeed = cropSeedFromCharges(it.charges);
        else cropSeed = hash32(static_cast<uint32_t>(it.id) ^ 0xC0C0A11Eu);

        const bool hasMeta = (it.enchant != 0);
        const int rarityHint  = hasMeta ? cropRarityFromEnchant(it.enchant) : -1;
        const int variantHint = hasMeta ? cropVariantFromEnchant(it.enchant) : -1;
        const int shinyHint   = hasMeta ? (cropIsShinyFromEnchant(it.enchant) ? 1 : 0) : -1;

        const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);

        // Stage label derived from ItemKind.
        const char* stage = (it.kind == ItemKind::CropMature) ? "MATURE" : (it.kind == ItemKind::CropGrowing ? "GROWING" : "SPROUT");
        ss << cs.name << " " << stage;
        if (cs.shiny) ss << " {SHINY}";

        const int fert = farmPlantFertilityFromEnchant(it.enchant);
        if (fert > 0) ss << " {FERT " << clampi(fert, 0, 100) << "}";

        const int affinity = farmPlantAffinityFromEnchant(it.enchant);
        if (affinity >= 0) ss << " {AFF " << farmgen::farmTagByIndex(affinity) << "}";

    } else if (it.kind == ItemKind::Fish) {

        // Procedural fish: encoded meta (rarity/size/shiny) can live in enchant,
        // while the fish seed can be stored in charges (or spriteSeed as a fallback).
        uint32_t fishSeed = 0u;
        if (it.charges != 0) {
            fishSeed = fishSeedFromCharges(it.charges);
        } else if (it.spriteSeed != 0u) {
            fishSeed = it.spriteSeed;
        } else {
            fishSeed = hash32(static_cast<uint32_t>(it.id) ^ 0xF15B00Fu);
        }

        const bool hasMeta = (it.enchant != 0);
        const int rarityHint = hasMeta ? fishRarityFromEnchant(it.enchant) : -1;
        const int sizeHint   = hasMeta ? fishSizeClassFromEnchant(it.enchant) : -1;
        const int shinyHint  = hasMeta ? (fishIsShinyFromEnchant(it.enchant) ? 1 : 0) : -1;

        const fishgen::FishSpec fs = fishgen::makeFish(fishSeed, rarityHint, sizeHint, shinyHint);

        ss << fs.name;
        ss << " [" << fishgen::fishRarityName(fs.rarity) << "]";
        if (fs.shiny) ss << " {SHINY}";

        const int lb = fs.weight10 / 10;
        const int tenth = fs.weight10 % 10;
        ss << " {" << lb << "." << tenth << "LB}";

        if (fs.bonusTag && fs.bonusTag[0]) {
            ss << " {" << fs.bonusTag << "}";
        }

    } else if (it.kind == ItemKind::FishingRod) {
        ss << d.name;

        // Rod durability stored in charges (defaults to max if unset).
        const int maxDur = std::max(0, d.maxCharges);
        if (maxDur > 0) {
            int cur = it.charges;
            if (cur <= 0) cur = maxDur;
            cur = clampi(cur, 0, maxDur);
            ss << " {" << cur << "/" << maxDur << " DUR}";
        }

    } else if (it.kind == ItemKind::GardenHoe) {
        ss << d.name;

        // Hoe durability stored in charges (defaults to max if unset).
        const int maxDur = std::max(0, d.maxCharges);
        if (maxDur > 0) {
            int cur = it.charges;
            if (cur <= 0) cur = maxDur;
            cur = clampi(cur, 0, maxDur);
            ss << " {" << cur << "/" << maxDur << " DUR}";
        }

    

    } else if (it.kind == ItemKind::EssenceShard) {
        if (it.count > 1) ss << it.count << " ";

        const int tagId = essenceShardTagFromEnchant(it.enchant);
        const int tier = essenceShardTierFromEnchant(it.enchant);
        const bool shiny = essenceShardIsShinyFromEnchant(it.enchant);

        const crafttags::Tag tg = crafttags::tagFromIndex(tagId);
        const char* tok = crafttags::tagToken(tg);
        if (tok && tok[0]) ss << tok << " ";
        else ss << "MUNDANE ";

        ss << "ESSENCE SHARD";
        if (it.count > 1) ss << "S";
        ss << " {T" << clampi(tier, 0, 15) << "}";
        if (shiny) ss << " {SHINY}";

} else if (it.kind == ItemKind::RuneTablet) {
        // Procedural rune magic tablet: spell id is encoded in spriteSeed.
        // This is UI/name/sprite support only for now; learning/casting wiring will come later.
        uint32_t pid = it.spriteSeed;
        if (pid == 0u) pid = hash32(static_cast<uint32_t>(it.id) ^ 0x52C39A7Bu);

        const ProcSpell ps = generateProcSpell(pid);

        ss << "RUNE TABLET OF " << ps.name;
        ss << " {T" << static_cast<int>(ps.tier) << "}";
        ss << " {" << procSpellElementName(ps.element) << " " << procSpellFormName(ps.form) << "}";

        const std::string modTags = procSpellModsToTags(ps.mods);
        if (!modTags.empty()) ss << " {" << toUpper(modTags) << "}";

        ss << " {M" << ps.manaCost << "}";
        if (ps.needsTarget) ss << " {R" << ps.range << "}";
        if (ps.aoeRadius > 0) ss << " {A" << ps.aoeRadius << "}";
        if (ps.durationTurns > 0) ss << " {D" << ps.durationTurns << "}";
        ss << " {" << ps.runeSigil << "}";
} else if (it.kind == ItemKind::BountyContract) {

    // Procedural bounty contracts: show target/progress/reward inline.
    uint32_t seed = it.spriteSeed;
    if (seed == 0u) seed = hash32(static_cast<uint32_t>(it.id) ^ 0xB01DCAFEu);

    const std::string code = bountygen::codename(seed);

    const int rawTarget = bountyTargetKindFromCharges(it.charges);
    EntityKind target = EntityKind::Goblin;
    if (rawTarget >= 0 && rawTarget < ENTITY_KIND_COUNT) target = static_cast<EntityKind>(rawTarget);

    int req = bountyRequiredKillsFromCharges(it.charges);
    if (req <= 0) {
        // Fallback for legacy/placeholder contracts.
        req = bountygen::pickRequiredKills(seed, bountygen::tierFromDepthHint(1), target);
    }
    req = clampi(req, 1, 255);

    const int prog = clampi(bountyProgressFromEnchant(it.enchant), 0, 255);
    const int shownProg = clampi(prog, 0, req);

    const int rawReward = bountyRewardKindFromCharges(it.charges);
    ItemKind rewardK = ItemKind::Gold;
    if (rawReward >= 0 && rawReward < ITEM_KIND_COUNT) rewardK = static_cast<ItemKind>(rawReward);

    int rewardC = bountyRewardCountFromCharges(it.charges);
    rewardC = clampi(rewardC, 0, 255);

    ss << "CONTRACT: " << code;
    ss << " {KILL " << req << " " << bountygen::pluralizeEntityName(target, req) << "}";
    ss << " [" << shownProg << "/" << req << "]";

    if (shownProg >= req) {
        ss << " {COMPLETE}";
    }

    if (rewardK == ItemKind::Gold) {
        if (rewardC > 0) ss << " {REWARD " << rewardC << "G}";
    } else {
        const ItemDef& rd = itemDef(rewardK);
        const int rc = std::max(1, rewardC);
        if (isStackable(rewardK) && rc > 1) {
            ss << " {REWARD " << rc << " " << pluralizeStackableName(rewardK, rd.name, rc) << "}";
        } else {
            ss << " {REWARD " << rd.name << "}";
        }
    }

} else if (it.kind == ItemKind::VtuberFigurine) {

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
        ss << " '" << artifactgen::artifactTitle(it) << "'";
        const char* p = artifactgen::artifactPowerTag(it);
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
    } else if (it.kind == ItemKind::ButcheredMeat) {
        // Butchered product display is fully custom; clear the default name output.
        ss.str("");
        ss.clear();

        const bool plural = (it.count > 1);
        if (plural) ss << it.count << " ";

        const int srcRaw = butcherSourceKindFromEnchant(it.enchant);
        ItemKind srcKind = ItemKind::CorpseGoblin;
        if (srcRaw >= 0 && srcRaw < ITEM_KIND_COUNT) srcKind = static_cast<ItemKind>(srcRaw);

        const int tagId = butcherMeatTagFromEnchant(it.enchant);
        const char* tag = butchergen::tagToken(butchergen::tagFromIndex(tagId));
        if (tag && tag[0]) ss << "PRIME ";

        const auto cut = butchergen::cutFromIndex(butcherMeatCutFromEnchant(it.enchant));
        ss << butchergen::corpseLabel(srcKind) << " ";
        if (plural) {
            const char* cp = butchergen::cutTokenPlural(cut);
            if (cp) ss << cp;
            else ss << butchergen::cutToken(cut) << "S";
        } else {
            ss << butchergen::cutToken(cut);
        }

        if (it.charges <= 60) ss << " (ROTTEN)";
        else if (it.charges <= 160) ss << " (STALE)";
        else ss << " (FRESH)";

        if (tag && tag[0]) ss << " {" << tag << "}";

    } else if (it.kind == ItemKind::ButcheredHide) {
        ss.str("");
        ss.clear();

        const bool plural = (it.count > 1);
        if (plural) ss << it.count << " ";

        const int srcRaw = butcherSourceKindFromEnchant(it.enchant);
        ItemKind srcKind = ItemKind::CorpseGoblin;
        if (srcRaw >= 0 && srcRaw < ITEM_KIND_COUNT) srcKind = static_cast<ItemKind>(srcRaw);

        const int q = butcherMaterialQualityFromEnchant(it.enchant);
        const int v = butcherMaterialVariantFromEnchant(it.enchant);
        const auto ht = butchergen::hideTypeFromIndex(v);

        ss << butcherQualityAdj(q) << " " << butchergen::corpseLabel(srcKind) << " ";
        ss << (plural ? butchergen::hideTokenPlural(ht) : butchergen::hideTokenSingular(ht));

    } else if (it.kind == ItemKind::ButcheredBones) {
        ss.str("");
        ss.clear();

        const bool plural = (it.count > 1);
        if (plural) ss << it.count << " ";

        const int srcRaw = butcherSourceKindFromEnchant(it.enchant);
        ItemKind srcKind = ItemKind::CorpseGoblin;
        if (srcRaw >= 0 && srcRaw < ITEM_KIND_COUNT) srcKind = static_cast<ItemKind>(srcRaw);

        const int q = butcherMaterialQualityFromEnchant(it.enchant);
        const int v = butcherMaterialVariantFromEnchant(it.enchant);
        const auto bt = butchergen::boneTypeFromIndex(v);

        ss << butcherQualityAdj(q) << " " << butchergen::corpseLabel(srcKind) << " ";
        ss << (plural ? butchergen::boneTokenPlural(bt) : butchergen::boneTokenSingular(bt));
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
