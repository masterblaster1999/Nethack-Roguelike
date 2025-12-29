#pragma once
#include "common.hpp"
#include <cstdint>
#include <string>
#include <vector>

enum class ProjectileKind : uint8_t {
    Arrow = 0,
    Rock,
    Spark,
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
};

// Keep in sync with the last enum value (append-only).
inline constexpr int ITEM_KIND_COUNT = static_cast<int>(ItemKind::WandDigging) + 1;

inline bool isChestKind(ItemKind k) {
    return k == ItemKind::Chest || k == ItemKind::ChestOpen;
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
            return true;
        case ItemKind::ScrollKnock:
            return true;
        default:
            return false;
    }
}

inline bool isIdentifiableKind(ItemKind k) {
    return isPotionKind(k) || isScrollKind(k);
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
};

struct GroundItem {
    Item item;
    Vec2i pos;
};

const ItemDef& itemDef(ItemKind k);

inline bool isStackable(ItemKind k) { return itemDef(k).stackable; }
inline bool isConsumable(ItemKind k) { return itemDef(k).consumable; }
inline bool isGold(ItemKind k) { return itemDef(k).isGold; }
inline EquipSlot equipSlot(ItemKind k) { return itemDef(k).slot; }
inline bool isMeleeWeapon(ItemKind k) { return equipSlot(k) == EquipSlot::MeleeWeapon; }
inline bool isRangedWeapon(ItemKind k) { return equipSlot(k) == EquipSlot::RangedWeapon; }
inline bool isWeapon(ItemKind k) { return isMeleeWeapon(k) || isRangedWeapon(k); }
inline bool isArmor(ItemKind k) { return equipSlot(k) == EquipSlot::Armor; }

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

// Stacking: tries to merge `incoming` into existing stack in `inv` if possible.
// Returns true if merged; false if caller should push as new entry.
bool tryStackItem(std::vector<Item>& inv, const Item& incoming);
