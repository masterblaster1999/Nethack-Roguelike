#include "items.hpp"
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

} // namespace

const ItemDef& itemDef(ItemKind k) {
    // Keep in sync with enum ordering.
    static const ItemDef defs[] = {
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

    // Prefix BUC (blessed/uncursed/cursed) + enchantment for non-stackable equipment.
    // (We intentionally keep new ItemKind values appended to preserve old save compatibility.)
    if (isWeapon(it.kind) || isArmor(it.kind)) {
        if (it.buc < 0) ss << "CURSED ";
        else if (it.buc > 0) ss << "BLESSED ";

        if (it.enchant != 0) {
            if (it.enchant > 0) ss << "+";
            ss << it.enchant << " ";
        }
    }

    if (d.stackable && it.count > 1) {
        ss << it.count << " " << pluralizeStackableName(it.kind, d.name, it.count);
    } else {
        ss << d.name;
    }

    if (it.kind == ItemKind::TorchLit) {
        ss << " (" << it.charges << "T)";
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

bool tryStackItem(std::vector<Item>& inv, const Item& incoming) {
    if (!isStackable(incoming.kind)) return false;

    // For stackables we require the important per-item metadata to match.
    // (This keeps future extensions like blessed/cursed consumables safe.)
    for (auto& it : inv) {
        if (it.kind != incoming.kind) continue;
        if (it.charges != incoming.charges) continue;
        if (it.enchant != incoming.enchant) continue;
        if (it.buc != incoming.buc) continue;
        if (it.shopPrice != incoming.shopPrice) continue;
        if (it.shopDepth != incoming.shopDepth) continue;

        it.count += incoming.count;
        return true;
    }
    return false;
}