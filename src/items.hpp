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
    Weapon,
    Armor,
};

enum class ItemKind : uint8_t {
    // Weapons
    Dagger = 0,
    Sword,
    Bow,
    WandSparks,

    // Armor
    LeatherArmor,
    ChainArmor,

    // Consumables
    PotionHealing,
    ScrollTeleport,

    // Ammo / misc
    Arrow,
    Rock,
    Gold,
};

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
};

struct Item {
    int id = 0;
    ItemKind kind = ItemKind::Dagger;
    int count = 1;          // for stackables
    int charges = 0;        // for wands
    uint32_t spriteSeed = 0;
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
inline bool isWeapon(ItemKind k) { return equipSlot(k) == EquipSlot::Weapon; }
inline bool isArmor(ItemKind k) { return equipSlot(k) == EquipSlot::Armor; }

std::string itemDisplayName(const Item& it);
std::string itemDisplayNameSingle(ItemKind k);

// Inventory helpers
int countGold(const std::vector<Item>& inv);
int findItemIndexById(const std::vector<Item>& inv, int itemId);
int findFirstAmmoIndex(const std::vector<Item>& inv, AmmoKind ammo);
int ammoCount(const std::vector<Item>& inv, AmmoKind ammo);

// Consumes up to `amount` ammo from the first matching stack. Returns true if fully consumed.
bool consumeAmmo(std::vector<Item>& inv, AmmoKind ammo, int amount);

// Stacking: tries to merge `incoming` into existing stack in `inv` if possible.
// Returns true if merged; false if caller should push as new entry.
bool tryStackItem(std::vector<Item>& inv, const Item& incoming);
