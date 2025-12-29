#include "combat_rules.hpp"
#include "game.hpp" // for EntityKind values

#include <algorithm>
#include <sstream>

int rollDice(RNG& rng, DiceExpr d) {
    d.count = std::max(0, d.count);
    d.sides = std::max(0, d.sides);

    int sum = d.bonus;
    if (d.count <= 0 || d.sides <= 0) return sum;
    for (int i = 0; i < d.count; ++i) {
        sum += rng.range(1, d.sides);
    }
    return sum;
}

DiceExpr meleeDiceForWeapon(ItemKind weapon) {
    // These are intentionally simple (NetHack-ish vibes, not exact values).
    switch (weapon) {
        case ItemKind::Dagger: return {1, 4, 0};
        case ItemKind::Sword:  return {1, 6, 0};
        case ItemKind::Axe:    return {1, 8, 0};

        // Ranged weapons as improvised melee (rare): keep weak.
        case ItemKind::Bow:    return {1, 3, 0};
        case ItemKind::Sling:  return {1, 2, 0};
        case ItemKind::WandSparks: return {1, 2, 0};
        default:               return {1, 2, 0}; // unarmed/unknown
    }
}

DiceExpr meleeDiceForMonster(EntityKind kind) {
    switch (kind) {
        case EntityKind::Goblin:         return {1, 4, 0};
        case EntityKind::Orc:            return {1, 6, 0};
        case EntityKind::Bat:            return {1, 3, 0};
        case EntityKind::Slime:          return {1, 5, 0};
        case EntityKind::SkeletonArcher: return {1, 4, 0};
        case EntityKind::KoboldSlinger:  return {1, 4, 0};
        case EntityKind::Wolf:           return {1, 6, 0};
        case EntityKind::Troll:          return {2, 4, 0};
        case EntityKind::Wizard:         return {1, 4, 0};
        case EntityKind::Snake:          return {1, 3, 0};
        case EntityKind::Spider:         return {1, 3, 0};
        case EntityKind::Ogre:           return {1, 10, 0};
        case EntityKind::Mimic:          return {1, 8, 0};
        case EntityKind::Shopkeeper:     return {2, 4, 0};
        case EntityKind::Player:         return {1, 2, 0};
        default:                         return {1, 4, 0};
    }
}

DiceExpr rangedDiceForProjectile(ProjectileKind proj, bool wandPowered) {
    // Ammunition/projectiles: base dice. The caller can add bonuses.
    switch (proj) {
        case ProjectileKind::Arrow: return {1, 6, 0};
        case ProjectileKind::Rock:  return {1, 4, 0};
        case ProjectileKind::Spark:
            // Wands are a bit spicier than wizard zaps.
            return wandPowered ? DiceExpr{1, 6, 2} : DiceExpr{1, 6, 0};
        default: return {1, 4, 0};
    }
}

int statDamageBonusFromAtk(int atk) {
    // A very small, smooth bonus. Starting ATK=3 gives +1.
    // Keeps damage scaling without making level-ups explode.
    atk = std::max(0, atk);
    return std::max(0, (atk - 1) / 2);
}

std::string diceToString(DiceExpr d, bool includeBonus) {
    std::ostringstream ss;
    ss << std::max(0, d.count) << "d" << std::max(0, d.sides);
    if (includeBonus && d.bonus != 0) {
        if (d.bonus > 0) ss << "+";
        ss << d.bonus;
    }
    return ss.str();
}
