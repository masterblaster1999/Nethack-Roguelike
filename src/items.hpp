#pragma once
#include "common.hpp"
#include <cstdint>
#include <string>
#include <vector>

enum class ProjectileKind : uint8_t {
    Arrow = 0,
    Rock,
    Spark,
    // New projectile kinds (append-only)
    Fireball,
    Torch,
};

enum class AmmoKind : uint8_t {
    None = 0,
    Arrow,
    Rock,
};

enum class EquipSlot : uint8_t {
    None = 0,
    MeleeWeapon,
    RangedWeapon,
    Armor,
    // New equipment types (append-only; NOT serialized)
    Ring,
};

enum class ItemKind : uint8_t {
    // Weapons
    Dagger = 0,
    Sword,
    Bow,
    Sling,
    WandSparks,

    // Armor
    LeatherArmor,
    ChainArmor,

    // Consumables
    PotionHealing,
    PotionStrength,
    ScrollTeleport,
    ScrollMapping,

    // Quest / special
    AmuletYendor,

    // Ammo / misc
    Arrow,
    Rock,
    Gold,

    // --- New consumables / progression (added after existing kinds to keep save compatibility) ---
    PotionAntidote,
    PotionRegeneration,
    PotionShielding,
    ScrollEnchantWeapon,
    ScrollEnchantArmor,

    // --- Even newer consumables (append-only to keep save compatibility) ---
    PotionHaste,
    PotionVision,

    // --- Identification / utility (append-only) ---
    ScrollIdentify,

    // --- New items (append-only to keep save compatibility) ---
    Axe,
    PlateArmor,
    FoodRation,
    ScrollDetectTraps,
    ScrollDetectSecrets,

    // --- Misc (append-only) ---
    Key,

    // --- Locks / doors (append-only) ---
    Lockpick,
    ScrollKnock,

    // --- Dungeon features (append-only) ---
    // A ground-interactable chest. It cannot be picked up.
    Chest,
    // Decorative open chest left behind after looting.
    ChestOpen,

    // --- Stealth / perception (append-only) ---
    PotionInvisibility,

    // --- Lighting (append-only) ---
    Torch,
    TorchLit,

    // --- Curses / blessings (append-only) ---
    ScrollRemoveCurse,

    // --- Mind / control (append-only) ---
    PotionClarity,
    ScrollConfusion,

    // --- Terrain / digging (append-only) ---
    Pickaxe,
    WandDigging,

    // --- Explosives / magic (append-only) ---
    WandFireball,

    // --- Corpses (append-only) ---
    // Dropped by slain monsters. Corpses rot away over time, and can be eaten
    // (at some risk) for hunger and sometimes temporary buffs.
    CorpseGoblin,
    CorpseOrc,
    CorpseBat,
    CorpseSlime,
    CorpseKobold,
    CorpseWolf,
    CorpseTroll,
    CorpseWizard,
    CorpseSnake,
    CorpseSpider,
    CorpseOgre,
    CorpseMimic,
    CorpseMinotaur,

    // --- Rings (append-only) ---
    RingMight,
    RingAgility,
    RingFocus,
    RingProtection,

    // --- Traversal (append-only) ---
    PotionLevitation,

    // --- Morale / control (append-only) ---
    ScrollFear,

    // --- Terrain / fortification (append-only) ---
    // NetHack-inspired utility scroll: raises boulders around the reader.
    ScrollEarth,

    // --- Pets / companions (append-only) ---
    // Charms nearby creatures into friendly companions.
    ScrollTaming,

    // --- Perception / weirdness (append-only) ---
    PotionHallucination,

    // --- Mana / magic (append-only) ---
    PotionEnergy,

    // --- Spellbooks (append-only) ---
    SpellbookMagicMissile,
    SpellbookBlink,
    SpellbookMinorHeal,
    SpellbookDetectTraps,
    SpellbookFireball,
    SpellbookStoneskin,
    SpellbookHaste,
    SpellbookInvisibility,
    SpellbookPoisonCloud,

    // --- New rings (append-only; keep ids stable for save compatibility) ---
    RingSearching,
};

// Item "egos" (NetHack-style brands / special properties) applied to some gear.
//
// Append-only: egos are serialized with items, so keep ids stable.
enum class ItemEgo : uint8_t {
    None = 0,

    // Weapon egos
    Flaming,
    Venom,
    Vampiric,
};

// Keep in sync with ItemEgo (append-only).
inline constexpr int ITEM_EGO_COUNT = static_cast<int>(ItemEgo::Vampiric) + 1;

inline const char* egoPrefix(ItemEgo e) {
    switch (e) {
        case ItemEgo::Flaming:  return "FLAMING";
        case ItemEgo::Venom:    return "VENOM";
        case ItemEgo::Vampiric: return "VAMPIRIC";
        default:                return "";
    }
}

// A rough shop/value multiplier for ego gear.
// Returned as a percentage (100 = no change).
inline int egoValueMultiplierPct(ItemEgo e) {
    switch (e) {
        case ItemEgo::Flaming:  return 160;
        case ItemEgo::Venom:    return 170;
        case ItemEgo::Vampiric: return 220;
        default:                return 100;
    }
}

// Keep in sync with the last enum value (append-only).
inline constexpr int ITEM_KIND_COUNT = static_cast<int>(ItemKind::RingSearching) + 1;

inline bool isChestKind(ItemKind k) {
    return k == ItemKind::Chest || k == ItemKind::ChestOpen;
}

inline bool isCorpseKind(ItemKind k) {
    switch (k) {
        case ItemKind::CorpseGoblin:
        case ItemKind::CorpseOrc:
        case ItemKind::CorpseBat:
        case ItemKind::CorpseSlime:
        case ItemKind::CorpseKobold:
        case ItemKind::CorpseWolf:
        case ItemKind::CorpseTroll:
        case ItemKind::CorpseWizard:
        case ItemKind::CorpseSnake:
        case ItemKind::CorpseSpider:
        case ItemKind::CorpseOgre:
        case ItemKind::CorpseMimic:
        case ItemKind::CorpseMinotaur:
            return true;
        default:
            return false;
    }
}

inline bool isPotionKind(ItemKind k) {
    switch (k) {
        case ItemKind::PotionHealing:
        case ItemKind::PotionStrength:
        case ItemKind::PotionAntidote:
        case ItemKind::PotionRegeneration:
        case ItemKind::PotionShielding:
        case ItemKind::PotionHaste:
        case ItemKind::PotionVision:
        case ItemKind::PotionInvisibility:
        case ItemKind::PotionClarity:
        case ItemKind::PotionLevitation:
        case ItemKind::PotionHallucination:
        case ItemKind::PotionEnergy:
            return true;
        default:
            return false;
    }
}

inline bool isScrollKind(ItemKind k) {
    switch (k) {
        case ItemKind::ScrollTeleport:
        case ItemKind::ScrollMapping:
        case ItemKind::ScrollEnchantWeapon:
        case ItemKind::ScrollEnchantArmor:
        case ItemKind::ScrollIdentify:
        case ItemKind::ScrollDetectTraps:
        case ItemKind::ScrollDetectSecrets:
        case ItemKind::ScrollRemoveCurse:
        case ItemKind::ScrollConfusion:
        case ItemKind::ScrollFear:
        case ItemKind::ScrollEarth:
        case ItemKind::ScrollTaming:
            return true;
        case ItemKind::ScrollKnock:
            return true;
        default:
            return false;
    }
}


struct ItemDef {
    ItemKind kind;
    const char* name;

    bool stackable = false;
    bool consumable = false;
    bool isGold = false;

    EquipSlot slot = EquipSlot::None;

    // Stat modifiers
    int meleeAtk = 0;
    int rangedAtk = 0;
    int defense = 0;

    // Ranged properties
    int range = 0;              // 0 means not ranged
    AmmoKind ammo = AmmoKind::None;
    ProjectileKind projectile = ProjectileKind::Arrow;

    // Wand-like charges
    int maxCharges = 0;

    // Consumable effects
    int healAmount = 0;
    int hungerRestore = 0; // 0 = no hunger effect

    // Encumbrance / carrying
    // Simple integer "weight" units used by the optional encumbrance system.
    // 0 means weightless (e.g., gold by default).
    int weight = 0;

    // Economy / shops: base value in gold for one unit of this item.
    // 0 means "not normally sold" (e.g., gold itself, quest items, decorative props).
    int value = 0;

    // Talent/stat modifiers granted while equipped.
    // These are primarily used by rings (and are append-only for future gear types).
    int modMight = 0;
    int modAgility = 0;
    int modVigor = 0;
    int modFocus = 0;
};

struct Item {
    int id = 0;
    ItemKind kind = ItemKind::Dagger;
    int count = 1;          // for stackables
    int charges = 0;        // for wands / torches (fuel)
    int enchant = 0;        // for weapons/armor (+/-), 0 = normal
    int buc = 0;            // -1 = cursed, 0 = uncursed, +1 = blessed (primarily for gear)
    uint32_t spriteSeed = 0;

    // Shops: if >0, this item is tagged with a shop price (per-unit) and ownership.
    // shopDepth tracks which dungeon depth the shop belongs to.
    // In inventory, nonzero shopPrice means the item is UNPAID (debt).
    int shopPrice = 0;
    int shopDepth = 0;

    // Item ego / brand (rare). Used primarily for melee weapons.
    ItemEgo ego = ItemEgo::None;

    // Misc item flags (append-only).
    // Used to tag special ground items (e.g. item mimics).
    uint8_t flags = 0;
};

struct GroundItem {
    Item item;
    Vec2i pos;
};

// Item flags (append-only).
// NOTE: flags are serialized; only add new bits at the end.
inline constexpr uint8_t ITEM_FLAG_MIMIC_BAIT = 1u << 0;

inline bool itemIsMimicBait(const Item& it) { return (it.flags & ITEM_FLAG_MIMIC_BAIT) != 0; }
inline void setItemMimicBait(Item& it, bool v) {
    if (v) it.flags = static_cast<uint8_t>(it.flags | ITEM_FLAG_MIMIC_BAIT);
    else   it.flags = static_cast<uint8_t>(it.flags & static_cast<uint8_t>(~ITEM_FLAG_MIMIC_BAIT));
}

const ItemDef& itemDef(ItemKind k);

inline bool isStackable(ItemKind k) { return itemDef(k).stackable; }
inline bool isConsumable(ItemKind k) { return itemDef(k).consumable; }
inline bool isGold(ItemKind k) { return itemDef(k).isGold; }
inline EquipSlot equipSlot(ItemKind k) { return itemDef(k).slot; }
inline bool isMeleeWeapon(ItemKind k) { return equipSlot(k) == EquipSlot::MeleeWeapon; }
inline bool isRangedWeapon(ItemKind k) { return equipSlot(k) == EquipSlot::RangedWeapon; }
inline bool isWeapon(ItemKind k) { return isMeleeWeapon(k) || isRangedWeapon(k); }
inline bool isArmor(ItemKind k) { return equipSlot(k) == EquipSlot::Armor; }
inline bool isRingKind(ItemKind k) { return equipSlot(k) == EquipSlot::Ring; }

// Wands are ranged weapons that use charges (maxCharges>0) and do not require ammo.
inline bool isWandKind(ItemKind k) {
    const ItemDef& d = itemDef(k);
    return isRangedWeapon(k) && d.maxCharges > 0 && d.ammo == AmmoKind::None;
}

// Identifiable items start unknown each run and use randomized appearances.
inline bool isIdentifiableKind(ItemKind k) {
    return isPotionKind(k) || isScrollKind(k) || isRingKind(k) || isWandKind(k);
}


// Convenience: "gear" in ProcRogue means an equipable item subject to BUC / enchant rules.
inline bool isWearableGear(ItemKind k) { return isWeapon(k) || isArmor(k) || isRingKind(k); }

std::string itemDisplayName(const Item& it);
std::string itemDisplayNameSingle(ItemKind k);

// Encumbrance helpers
int itemWeight(const Item& it);
int totalWeight(const std::vector<Item>& items);

// Inventory helpers
int countGold(const std::vector<Item>& inv);
int findItemIndexById(const std::vector<Item>& inv, int itemId);
int findFirstAmmoIndex(const std::vector<Item>& inv, AmmoKind ammo);
int ammoCount(const std::vector<Item>& inv, AmmoKind ammo);

// Consumes up to `amount` ammo from matching stacks. Returns true if fully consumed.
bool consumeAmmo(std::vector<Item>& inv, AmmoKind ammo, int amount);

// Consumes exactly 1 ammo and optionally returns a template Item (count=1) preserving metadata
// like shopPrice/shopDepth so projectiles can be recovered without laundering shop debt.
bool consumeOneAmmo(std::vector<Item>& inv, AmmoKind ammo, Item* outConsumed = nullptr);

// Stacking: tries to merge `incoming` into existing stack in `inv` if possible.
// Returns true if merged; false if caller should push as new entry.
bool tryStackItem(std::vector<Item>& inv, const Item& incoming);
