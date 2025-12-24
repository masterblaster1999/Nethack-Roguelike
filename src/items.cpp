#include "items.hpp"
#include <algorithm>
#include <sstream>

const ItemDef& itemDef(ItemKind k) {
    // Keep in sync with enum ordering.
    static const ItemDef defs[] = {
        // Weapons
        { ItemKind::Dagger,        "DAGGER",        false, false, false, EquipSlot::MeleeWeapon, 1, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0 },
        { ItemKind::Sword,         "SWORD",         false, false, false, EquipSlot::MeleeWeapon, 2, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0 },
        { ItemKind::Bow,           "BOW",           false, false, false, EquipSlot::RangedWeapon, 0, 1, 0, 8, AmmoKind::Arrow, ProjectileKind::Arrow, 0, 0 },
        { ItemKind::WandSparks,    "WAND OF SPARKS",false, false, false, EquipSlot::RangedWeapon, 0, 2, 0, 7, AmmoKind::None, ProjectileKind::Spark, 12, 0 },

        // Armor
        { ItemKind::LeatherArmor,  "LEATHER ARMOR", false, false, false, EquipSlot::Armor, 0, 0, 1, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0 },
        { ItemKind::ChainArmor,    "CHAIN ARMOR",   false, false, false, EquipSlot::Armor, 0, 0, 2, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0 },

        // Consumables
        { ItemKind::PotionHealing, "POTION OF HEALING", true, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 6 },
        { ItemKind::ScrollTeleport,"SCROLL OF TELEPORT",true, true, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0 },

        // Ammo / misc
        { ItemKind::Arrow,         "ARROW",         true, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Arrow, 0, 0 },
        { ItemKind::Rock,          "ROCK",          true, false, false, EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0 },
        { ItemKind::Gold,          "GOLD",          true, false, true,  EquipSlot::None, 0, 0, 0, 0, AmmoKind::None, ProjectileKind::Rock, 0, 0 },
    };

    const size_t idx = static_cast<size_t>(k);
    if (idx >= (sizeof(defs) / sizeof(defs[0]))) {
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
    if (d.stackable && it.count > 1) {
        ss << it.count << " " << d.name;
        // Plural-ish
        if (it.kind == ItemKind::Arrow) ss << "S";
        if (it.kind == ItemKind::Rock) ss << "S";
    } else {
        ss << d.name;
    }
    if (it.kind == ItemKind::WandSparks) {
        ss << " (" << it.charges << "/" << d.maxCharges << ")";
    }
    return ss.str();
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
    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& it) {
        return (it.kind == ItemKind::Arrow || it.kind == ItemKind::Rock || it.kind == ItemKind::Gold || it.kind == ItemKind::PotionHealing || it.kind == ItemKind::ScrollTeleport)
            && it.count <= 0;
    }), inv.end());
    return need <= 0;
}

bool tryStackItem(std::vector<Item>& inv, const Item& incoming) {
    const ItemDef& d = itemDef(incoming.kind);
    if (!d.stackable) return false;
    for (auto& it : inv) {
        if (it.kind == incoming.kind) {
            it.count += std::max(1, incoming.count);
            return true;
        }
    }
    return false;
}
