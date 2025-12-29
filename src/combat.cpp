#include "game.hpp"

#include "combat_rules.hpp"

#include <algorithm>
#include <sstream>

namespace {

const char* kindName(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return "YOU";
        case EntityKind::Goblin: return "GOBLIN";
        case EntityKind::Orc: return "ORC";
        case EntityKind::Bat: return "BAT";
        case EntityKind::Slime: return "SLIME";
        case EntityKind::SkeletonArcher: return "SKELETON";
        case EntityKind::KoboldSlinger: return "KOBOLD";
        case EntityKind::Wolf: return "WOLF";
        case EntityKind::Troll: return "TROLL";
        case EntityKind::Wizard: return "WIZARD";
        case EntityKind::Snake: return "SNAKE";
        case EntityKind::Spider: return "SPIDER";
        case EntityKind::Ogre: return "OGRE";
        case EntityKind::Mimic: return "MIMIC";
        case EntityKind::Shopkeeper: return "SHOPKEEPER";
        default: return "THING";
    }
}

struct HitCheck {
    bool hit = false;
    bool crit = false;
    int natural = 0; // 1..20
};

HitCheck rollToHit(RNG& rng, int attackBonus, int targetAC) {
    HitCheck r;
    r.natural = rng.range(1, 20);

    // Classic d20-style rules:
    //   - Natural 1  => always miss
    //   - Natural 20 => always hit + critical
    if (r.natural == 1) {
        r.hit = false;
        r.crit = false;
        return r;
    }
    if (r.natural == 20) {
        r.hit = true;
        r.crit = true;
        return r;
    }

    r.hit = (r.natural + attackBonus) >= targetAC;
    r.crit = false;
    return r;
}

int targetAC(const Game& game, const Entity& e) {
    const int def = (e.kind == EntityKind::Player) ? game.playerDefense() : e.baseDef;
    return 10 + def;
}

int damageReduction(const Game& game, const Entity& e) {
    // Monsters use their base DEF as "hide/armor" (small values, 0-2 typically).
    if (e.kind != EntityKind::Player) {
        return std::max(0, e.baseDef);
    }

    // Player DR is based on worn armor (and temporary shielding).
    // We don't want baseDef (dodge) to reduce damage, only to avoid getting hit.
    const int evasion = game.player().baseDef;
    return std::max(0, game.playerDefense() - evasion);
}

} // namespace


void Game::attackMelee(Entity& attacker, Entity& defender) {
    if (attacker.hp <= 0 || defender.hp <= 0) return;

    // Attacking breaks invisibility (balance + clarity).
    if (attacker.kind == EntityKind::Player) {
        Entity& p = playerMut();
        if (p.invisTurns > 0) {
            p.invisTurns = 0;
            pushMsg("YOU BECOME VISIBLE!", MessageKind::System, true);
        }
    }

    // Peaceful shopkeepers only become hostile if you aggress them (or steal).
    if (attacker.kind == EntityKind::Player && defender.kind == EntityKind::Shopkeeper) {
        if (!defender.alerted) {
            defender.alerted = true;
            defender.lastKnownPlayerPos = attacker.pos;
            defender.lastKnownPlayerAge = 0;
            pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
        }
    }

    const int atkBonus = (attacker.kind == EntityKind::Player) ? playerAttack() : attacker.baseAtk;
    const int ac = targetAC(*this, defender);
    const HitCheck hc = rollToHit(rng, atkBonus, ac);

    const bool msgFromPlayer = (attacker.kind == EntityKind::Player);

    if (!hc.hit) {
        std::ostringstream ss;
        if (attacker.kind == EntityKind::Player) {
            ss << "YOU MISS " << kindName(defender.kind) << ".";
        } else if (defender.kind == EntityKind::Player) {
            ss << kindName(attacker.kind) << " MISSES YOU.";
        } else {
            ss << kindName(attacker.kind) << " MISSES " << kindName(defender.kind) << ".";
        }
        pushMsg(ss.str(), MessageKind::Combat, msgFromPlayer);
        return;
    }

    // Roll damage.
    DiceExpr baseDice{1, 2, 0};
    int bonus = statDamageBonusFromAtk(attacker.baseAtk);

    if (attacker.kind == EntityKind::Player) {
        if (const Item* w = equippedMelee()) {
            baseDice = meleeDiceForWeapon(w->kind);
            bonus += w->enchant;
        }
    } else {
        baseDice = meleeDiceForMonster(attacker.kind);
    }
    const int dice1 = rollDice(rng, baseDice);
    const int dice2 = (hc.crit ? rollDice(rng, baseDice) : 0);

    int dmg = dice1 + dice2 + bonus;

    // Damage reduction (armor/hide). Criticals punch through a bit.
    int dr = damageReduction(*this, defender);
    if (hc.crit) dr = dr / 2;
    const int absorbed = (dr > 0) ? rng.range(0, dr) : 0;
    dmg = std::max(0, dmg - absorbed);

    defender.hp -= dmg;

    std::ostringstream ss;
    if (attacker.kind == EntityKind::Player) {
        ss << "YOU " << (hc.crit ? "CRIT " : "") << "HIT " << kindName(defender.kind);
        if (dmg > 0) ss << " FOR " << dmg;
        else ss << " BUT DO NO DAMAGE";
        ss << ".";
    } else if (defender.kind == EntityKind::Player) {
        ss << kindName(attacker.kind) << " " << (hc.crit ? "CRITS" : "HITS") << " YOU";
        if (dmg > 0) ss << " FOR " << dmg;
        else ss << " BUT DOES NO DAMAGE";
        ss << ".";
    } else {
        ss << kindName(attacker.kind) << " HITS " << kindName(defender.kind) << ".";
    }
    pushMsg(ss.str(), MessageKind::Combat, msgFromPlayer);

    if (attacker.kind == EntityKind::Player) {
        // Fighting is noisy; nearby monsters may investigate.
        emitNoise(attacker.pos, 11);
    }

    // Monster special effects.
    if (defender.hp > 0 && defender.kind == EntityKind::Player) {
        if (attacker.kind == EntityKind::Snake && rng.chance(0.35f)) {
            defender.poisonTurns = std::max(defender.poisonTurns, rng.range(4, 8));
            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
        }
        if (attacker.kind == EntityKind::Spider && rng.chance(0.45f)) {
            defender.webTurns = std::max(defender.webTurns, rng.range(2, 4));
            pushMsg("YOU ARE ENSNARED BY WEBBING!", MessageKind::Warning, false);
        }
    }

    if (defender.hp <= 0) {
        if (defender.kind == EntityKind::Player) {
            pushMsg("YOU DIE.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = std::string("KILLED BY ") + kindName(attacker.kind);
            gameOver = true;
        } else {
            std::ostringstream ds;
            ds << kindName(defender.kind) << " DIES.";
            pushMsg(ds.str(), MessageKind::Combat, msgFromPlayer);

            if (attacker.kind == EntityKind::Player) {
                ++killCount;
                grantXp(xpFor(defender.kind));
            }
        }
    }
}


void Game::attackRanged(Entity& attacker, Vec2i target, int range, int atkBonus, int dmgBonus, ProjectileKind projKind, bool fromPlayer) {
    std::vector<Vec2i> line = bresenhamLine(attacker.pos, target);
    if (line.size() <= 1) return;

    if (fromPlayer) {
        // Attacking breaks invisibility.
        Entity& p = playerMut();
        if (p.invisTurns > 0) {
            p.invisTurns = 0;
            pushMsg("YOU BECOME VISIBLE!", MessageKind::System, true);
        }

        // Ranged attacks are noisy; nearby monsters may investigate.
        emitNoise(attacker.pos, 13);
    }

    // Clamp to range (+ start tile)
    if (range > 0 && static_cast<int>(line.size()) > range + 1) {
        line.resize(static_cast<size_t>(range + 1));
    }

    bool hitWall = false;
    bool hitAny = false;
    Entity* hit = nullptr;
    size_t stopIdx = line.size() - 1;

    // Projectiles travel the full line. If they miss a creature, they keep going.
    for (size_t i = 1; i < line.size(); ++i) {
        Vec2i p = line[i];
        if (!dung.inBounds(p.x, p.y)) { stopIdx = i - 1; break; }

        // Walls/closed doors block projectiles.
        if (dung.isOpaque(p.x, p.y)) {
            hitWall = true;
            stopIdx = i;
            break;
        }

        Entity* e = entityAtMut(p.x, p.y);
        if (!e || e->id == attacker.id || e->hp <= 0) continue;

        // Distance penalty for ranged accuracy.
        const int dist = static_cast<int>(i);
        const int penalty = dist / 3;
        const int adjAtk = atkBonus - penalty;

        const int ac = targetAC(*this, *e);
        const HitCheck hc = rollToHit(rng, adjAtk, ac);

        if (!hc.hit) {
            // Miss: projectile continues.
            if (fromPlayer) {
                std::ostringstream ss;
                ss << "YOU MISS " << kindName(e->kind) << ".";
                pushMsg(ss.str(), MessageKind::Combat, true);
            } else if (e->kind == EntityKind::Player) {
                std::ostringstream ss;
                ss << kindName(attacker.kind) << " MISSES YOU.";
                pushMsg(ss.str(), MessageKind::Combat, false);
            }
            continue;
        }

        // Hit: apply damage and stop.
        hitAny = true;
        hit = e;
        stopIdx = i;

        if (fromPlayer && hit->kind == EntityKind::Shopkeeper && !hit->alerted) {
            hit->alerted = true;
            hit->lastKnownPlayerPos = attacker.pos;
            hit->lastKnownPlayerAge = 0;
            pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
        }

        const bool wandPowered = (projKind == ProjectileKind::Spark) && fromPlayer;
        const DiceExpr baseDice = rangedDiceForProjectile(projKind, wandPowered);
        const int dice1 = rollDice(rng, baseDice);
        const int dice2 = (hc.crit ? rollDice(rng, baseDice) : 0);

        int dmg = dice1 + dice2;
        dmg += dmgBonus;
        dmg += statDamageBonusFromAtk(attacker.baseAtk);

        int dr = damageReduction(*this, *hit);
        if (hc.crit) dr = dr / 2;
        const int absorbed = (dr > 0) ? rng.range(0, dr) : 0;
        dmg = std::max(0, dmg - absorbed);

        hit->hp -= dmg;

        std::ostringstream ss;
        if (fromPlayer) {
            ss << "YOU " << (hc.crit ? "CRIT " : "") << "HIT " << kindName(hit->kind);
            if (dmg > 0) ss << " FOR " << dmg;
            else ss << " BUT DO NO DAMAGE";
            ss << ".";
        } else if (hit->kind == EntityKind::Player) {
            ss << kindName(attacker.kind) << " " << (hc.crit ? "CRITS" : "HITS") << " YOU";
            if (dmg > 0) ss << " FOR " << dmg;
            else ss << " BUT DOES NO DAMAGE";
            ss << ".";
        } else {
            ss << kindName(attacker.kind) << " HITS " << kindName(hit->kind) << ".";
        }
        pushMsg(ss.str(), MessageKind::Combat, fromPlayer);

        if (hit->hp <= 0) {
            if (hit->kind == EntityKind::Player) {
                pushMsg("YOU DIE.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = std::string("KILLED BY ") + kindName(attacker.kind);
                gameOver = true;
            } else {
                std::ostringstream ds;
                ds << kindName(hit->kind) << " DIES.";
                pushMsg(ds.str(), MessageKind::Combat, fromPlayer);
                if (fromPlayer) {
                    ++killCount;
                    grantXp(xpFor(hit->kind));
                }
            }
        }
        break;
    }

    if (!hitAny) {
        if (hitWall) {
            if (fromPlayer) pushMsg("THE SHOT HITS A WALL.", MessageKind::Warning, true);
        } else {
            if (fromPlayer) pushMsg("YOU FIRE.", MessageKind::Combat, true);
        }
    }

    // Recoverable ammo: arrows/rocks may remain on the ground after firing.
    if (projKind == ProjectileKind::Arrow || projKind == ProjectileKind::Rock) {
        ItemKind dropK = (projKind == ProjectileKind::Arrow) ? ItemKind::Arrow : ItemKind::Rock;

        // Default landing tile is the last tile the projectile reached.
        Vec2i land = line[stopIdx];
        // If we hit a wall/closed door, the projectile can't occupy that tile; land on the last open tile instead.
        if (hitWall && stopIdx > 0) {
            land = line[stopIdx - 1];
        }

        if (dung.inBounds(land.x, land.y) && !dung.isOpaque(land.x, land.y)) {
            float dropChance = (projKind == ProjectileKind::Arrow) ? 0.60f : 0.75f;
            if (hitWall) dropChance -= 0.20f;
            if (!fromPlayer) dropChance -= 0.15f;
            dropChance = std::clamp(dropChance, 0.10f, 0.95f);
            if (rng.chance(dropChance)) {
                dropGroundItem(land, dropK, 1);
            }
        }
    }

    // FX projectile path (truncate)
    std::vector<Vec2i> fxPath;
    fxPath.reserve(stopIdx + 1);
    for (size_t i = 0; i <= stopIdx && i < line.size(); ++i) fxPath.push_back(line[i]);

    FXProjectile fxp;
    fxp.kind = projKind;
    fxp.path = std::move(fxPath);
    fxp.pathIndex = (fxp.path.size() > 1) ? 1 : 0;
    fxp.stepTimer = 0.0f;
    fxp.stepTime = (projKind == ProjectileKind::Spark) ? 0.02f : 0.03f;
    fx.push_back(std::move(fxp));

    inputLock = true;
}
