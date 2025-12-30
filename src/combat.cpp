#include "game.hpp"

#include "combat_rules.hpp"
#include "physics.hpp"

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
        case EntityKind::Minotaur: return "MINOTAUR";
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
    // Note: for monsters, "armor" currently increases damage reduction but does not
    // make them harder to hit (AC still comes from baseDef). This keeps fights readable
    // while still letting gear matter.
    const int def = (e.kind == EntityKind::Player) ? game.playerDefense() : e.baseDef;
    return 10 + def;
}


int damageReduction(const Game& game, const Entity& e) {
    // Monsters: base DEF represents hide/toughness. Equipped armor (if any) adds DR.
    if (e.kind != EntityKind::Player) {
        int dr = std::max(0, e.baseDef);

        if (monsterCanEquipArmor(e.kind) && e.gearArmor.id != 0 && isArmor(e.gearArmor.kind)) {
            const Item& a = e.gearArmor;
            const int b = (a.buc < 0) ? -1 : (a.buc > 0 ? 1 : 0);
            dr += itemDef(a.kind).defense + a.enchant + b;
        }

        return std::max(0, dr);
    }

    // Player DR is based on worn armor (and temporary shielding).
    // We don't want baseDef (dodge) to reduce damage, only to avoid getting hit.
    const int evasion = game.playerEvasion();
    return std::max(0, game.playerDefense() - evasion);
}


} // namespace


void Game::attackMelee(Entity& attacker, Entity& defender) {
    if (attacker.hp <= 0 || defender.hp <= 0) return;

    const bool attackerWasInvisible = (attacker.kind == EntityKind::Player && attacker.effects.invisTurns > 0);
    const bool attackerWasSneaking = (attacker.kind == EntityKind::Player && isSneaking());

    // Attacking breaks invisibility (balance + clarity).
    if (attacker.kind == EntityKind::Player) {
        Entity& p = playerMut();
        if (p.effects.invisTurns > 0) {
            p.effects.invisTurns = 0;
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

    int atkBonus = 0;
if (attacker.kind == EntityKind::Player) {
    atkBonus = playerAttack();
} else {
    atkBonus = attacker.baseAtk;

    // Enchants and blessings/curse on a wielded weapon affect accuracy a bit.
    if (monsterCanEquipWeapons(attacker.kind) && attacker.gearMelee.id != 0 && isMeleeWeapon(attacker.gearMelee.kind)) {
        const int b = (attacker.gearMelee.buc < 0) ? -1 : (attacker.gearMelee.buc > 0 ? 1 : 0);
        atkBonus += attacker.gearMelee.enchant + b;
    }
}
    bool ambush = false;
    bool backstab = false;
    if (attacker.kind == EntityKind::Player && defender.kind != EntityKind::Player && !defender.alerted) {
        ambush = true;
        const int agi = playerAgility();
        atkBonus += 2 + std::min(3, agi / 4);
        backstab = (attackerWasSneaking || attackerWasInvisible);
    }

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
        if (attacker.kind == EntityKind::Player) {
            // Even a miss makes noise.
            emitNoise(attacker.pos, 7);
        }
        return;
    }

    // Roll damage.
    DiceExpr baseDice{1, 2, 0};
    int atkStatForBonus = attacker.baseAtk;
    if (attacker.kind == EntityKind::Player) {
        atkStatForBonus += playerMight();
    }
    int bonus = statDamageBonusFromAtk(atkStatForBonus);

    if (attacker.kind == EntityKind::Player) {
    if (const Item* w = equippedMelee()) {
        baseDice = meleeDiceForWeapon(w->kind);
        bonus += w->enchant;
    }
} else {
    baseDice = meleeDiceForMonster(attacker.kind);

    if (monsterCanEquipWeapons(attacker.kind) && attacker.gearMelee.id != 0 && isMeleeWeapon(attacker.gearMelee.kind)) {
        baseDice = meleeDiceForWeapon(attacker.gearMelee.kind);
        const int b = (attacker.gearMelee.buc < 0) ? -1 : (attacker.gearMelee.buc > 0 ? 1 : 0);
        bonus += attacker.gearMelee.enchant + b;
    }
}
    const int dice1 = rollDice(rng, baseDice);
    const int dice2 = (hc.crit ? rollDice(rng, baseDice) : 0);

    int dmg = dice1 + dice2 + bonus;

    // Ambush/backstab: reward catching monsters unaware (typically via sneak mode).
    if (ambush) {
        dmg += 1 + std::min(3, playerAgility() / 4);
        if (backstab) {
            // Add an extra weapon dice roll (roughly doubles dice damage).
            dmg += rollDice(rng, baseDice);
        }
    }

    // Damage reduction (armor/hide). Criticals punch through a bit.
    int dr = damageReduction(*this, defender);
    if (hc.crit) dr = dr / 2;
    const int absorbed = (dr > 0) ? rng.range(0, dr) : 0;
    dmg = std::max(0, dmg - absorbed);

    defender.hp -= dmg;

    std::ostringstream ss;
    if (attacker.kind == EntityKind::Player) {
        if (ambush) ss << (backstab ? "SNEAK ATTACK! " : "AMBUSH! ");
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
            defender.effects.poisonTurns = std::max(defender.effects.poisonTurns, rng.range(4, 8));
            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
        }
        if (attacker.kind == EntityKind::Spider && rng.chance(0.45f)) {
            defender.effects.webTurns = std::max(defender.effects.webTurns, rng.range(2, 4));
            pushMsg("YOU ARE ENSNARED BY WEBBING!", MessageKind::Warning, false);
        }
    }

    // Knockback / forced movement.
    // Adds tactical positioning and makes chasms/pillars matter more.
    bool skipDeathMsg = false;
    if (defender.hp > 0) {
        KnockbackConfig kcfg;
        kcfg.distance = 1;
        kcfg.power = 1;
        float chance = 0.0f;
        const bool critKnockback = hc.crit;

        auto isHeavyAttacker = [&]() -> bool {
            if (attacker.kind == EntityKind::Ogre) return true;
            if (attacker.kind == EntityKind::Troll) return true;
            if (attacker.kind == EntityKind::Minotaur) return true;
            if (attacker.kind == EntityKind::Player) {
                if (const Item* w = equippedMelee()) {
                    return (w->kind == ItemKind::Axe);
                }
            }
            return false;
        };

        if (attacker.kind == EntityKind::Player) {
            if (const Item* w = equippedMelee()) {
                switch (w->kind) {
                    case ItemKind::Axe:    chance = 0.26f; kcfg.power = 3; break;
                    case ItemKind::Sword:  chance = 0.18f; kcfg.power = 2; break;
                    case ItemKind::Dagger: chance = 0.12f; kcfg.power = 1; break;
                    default:               chance = 0.10f; kcfg.power = 1; break;
                }
            } else {
                chance = 0.10f;
                kcfg.power = 1;
            }
        } else {
            switch (attacker.kind) {
                case EntityKind::Ogre:  chance = 0.30f; kcfg.power = 3; break;
                case EntityKind::Troll: chance = 0.22f; kcfg.power = 2; break;
                case EntityKind::Minotaur: chance = 0.35f; kcfg.power = 4; break;
                case EntityKind::Wolf:  chance = 0.12f; kcfg.power = 1; break;
                default:                chance = 0.0f;  kcfg.power = 1; break;
            }
        }

        // Critical hits always attempt knockback when the attacker has any knockback profile.
        if (hc.crit) {
            kcfg.power = std::max(kcfg.power, 2);
        }

        // Defender resistance: agile/armored targets are harder to shove around.
        int defEvasion = defender.baseDef;
        if (defender.kind == EntityKind::Player) {
            defEvasion = playerEvasion();
        }
        chance -= 0.04f * static_cast<float>(std::max(0, defEvasion));
        if (defender.kind == EntityKind::Player) {
            const int drNow = damageReduction(*this, defender);
            chance -= 0.03f * static_cast<float>(std::max(0, drNow));
            if (player().effects.shieldTurns > 0) chance -= 0.10f;
        }
        chance = std::clamp(chance, 0.0f, 1.0f);

        if (chance > 0.0f && (critKnockback || rng.chance(chance))) {
            // Distance: heavy crits can push 2 tiles.
            kcfg.distance = 1;
            if (hc.crit && isHeavyAttacker()) {
                kcfg.distance = 2;
            } else if (!hc.crit && kcfg.power >= 3 && rng.chance(0.25f)) {
                kcfg.distance = 2;
            }

            // If the defender is the player, compute chance to catch the edge of a chasm.
            if (defender.kind == EntityKind::Player) {
                float catchP = 0.65f;
                catchP += 0.02f * static_cast<float>(std::max(0, playerEvasion()));
                if (encumbranceEnabled_) {
                    switch (burdenState()) {
                        case BurdenState::Unburdened: break;
                        case BurdenState::Burdened:   catchP -= 0.05f; break;
                        case BurdenState::Stressed:   catchP -= 0.10f; break;
                        case BurdenState::Strained:   catchP -= 0.18f; break;
                        case BurdenState::Overloaded: catchP -= 0.30f; break;
                    }
                }
                if (defender.effects.shieldTurns > 0) catchP += 0.08f;
                catchP = std::clamp(catchP, 0.10f, 0.90f);
                kcfg.playerCatchChance = catchP;
            }

            // Direction away from attacker.
            const int kdx = clampi(defender.pos.x - attacker.pos.x, -1, 1);
            const int kdy = clampi(defender.pos.y - attacker.pos.y, -1, 1);

            const Vec2i before = defender.pos;
            KnockbackResult kb = applyKnockback(dung, ents, rng, attacker.id, defender.id, kdx, kdy, kcfg);

            if (kb.stepsMoved > 0) {
                std::ostringstream ks;
                if (attacker.kind == EntityKind::Player) {
                    ks << "YOU KNOCK " << kindName(defender.kind) << " BACK!";
                } else if (defender.kind == EntityKind::Player) {
                    ks << kindName(attacker.kind) << " KNOCKS YOU BACK!";
                } else {
                    ks << kindName(attacker.kind) << " KNOCKS " << kindName(defender.kind) << " BACK.";
                }
                pushMsg(ks.str(), MessageKind::Combat, msgFromPlayer);

                // Forced movement can trigger traps.
                if (defender.hp > 0) {
                    triggerTrapAt(defender.pos, defender);
                }
            }

            // Door smash feedback.
            if (kb.doorChanged) {
                pushMsg("A DOOR BURSTS OPEN!", MessageKind::System, msgFromPlayer);
                emitNoise(kb.doorPos, 14);
            }

            // Stop reasons.
            if (kb.stop == KnockbackStop::SlammedWall || kb.stop == KnockbackStop::SlammedDoor) {
                std::ostringstream cs;
                const bool door = (kb.stop == KnockbackStop::SlammedDoor);
                if (defender.kind == EntityKind::Player) cs << "YOU";
                else cs << kindName(defender.kind);
                cs << (door ? " SLAM INTO THE DOOR" : " SLAM INTO THE WALL");
                if (kb.collisionDamageDefender > 0) cs << " FOR " << kb.collisionDamageDefender;
                cs << "!";
                pushMsg(cs.str(), MessageKind::Combat, msgFromPlayer);
                emitNoise(defender.pos, door ? 12 : 10);
            } else if (kb.stop == KnockbackStop::HitEntity) {
                const Entity* other = nullptr;
                for (const auto& e : ents) {
                    if (e.id == kb.otherEntityId) { other = &e; break; }
                }
                std::ostringstream cs;
                if (other) {
                    if (defender.kind == EntityKind::Player) cs << "YOU";
                    else cs << kindName(defender.kind);
                    cs << " CRASH INTO ";
                    if (other->kind == EntityKind::Player) cs << "YOU";
                    else cs << kindName(other->kind);
                    cs << "!";
                } else {
                    cs << "SOMETHING GETS RAMMED!";
                }
                pushMsg(cs.str(), MessageKind::Combat, msgFromPlayer);
                emitNoise(defender.pos, 12);
            } else if (kb.stop == KnockbackStop::FellIntoChasm) {
                std::ostringstream fs;
                if (defender.kind == EntityKind::Player) {
                    fs << "YOU FALL INTO THE CHASM!";
                    pushMsg(fs.str(), MessageKind::Warning, false);
                    if (endCause_.empty()) endCause_ = "FELL INTO A CHASM";
                } else {
                    fs << kindName(defender.kind) << " FALLS INTO THE CHASM!";
                    pushMsg(fs.str(), MessageKind::Combat, msgFromPlayer);
                    skipDeathMsg = true;
                }
                emitNoise(before, 16);
            } else if (kb.stop == KnockbackStop::CaughtEdge) {
                pushMsg("YOU CATCH THE EDGE OF THE CHASM!", MessageKind::Warning, false);
                emitNoise(defender.pos, 10);
            } else if (kb.stop == KnockbackStop::ImmuneToChasm) {
                if (defender.kind != EntityKind::Player) {
                    std::ostringstream fs;
                    fs << kindName(defender.kind) << " DODGES THE CHASM.";
                    pushMsg(fs.str(), MessageKind::Info, msgFromPlayer);
                }
            }
        }
    }

    if (defender.hp <= 0) {
        if (defender.kind == EntityKind::Player) {
            pushMsg("YOU DIE.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = std::string("KILLED BY ") + kindName(attacker.kind);
            gameOver = true;
        } else {
            if (!skipDeathMsg) {
                std::ostringstream ds;
                ds << kindName(defender.kind) << " DIES.";
                pushMsg(ds.str(), MessageKind::Combat, msgFromPlayer);
            }

            if (attacker.kind == EntityKind::Player) {
                ++killCount;
                grantXp(xpFor(defender.kind));
            }
        }
    }
}


void Game::attackRanged(Entity& attacker, Vec2i target, int range, int atkBonus, int dmgBonus, ProjectileKind projKind, bool fromPlayer, const Item* projectileTemplate) {
    // Confusion: shots drift and accuracy suffers.
    if (attacker.effects.confusionTurns > 0) {
        atkBonus -= 3;

        int ox = 0;
        int oy = 0;
        for (int tries = 0; tries < 4 && ox == 0 && oy == 0; ++tries) {
            ox = rng.range(-2, 2);
            oy = rng.range(-2, 2);
        }

        const int maxX = std::max(0, dung.width - 1);
        const int maxY = std::max(0, dung.height - 1);
        target.x = clampi(target.x + ox, 0, maxX);
        target.y = clampi(target.y + oy, 0, maxY);

        if (fromPlayer) {
            pushMsg("YOU FIRE WILDLY IN YOUR CONFUSION!", MessageKind::Warning, true);
        }
    }

    std::vector<Vec2i> line = bresenhamLine(attacker.pos, target);
    if (line.size() <= 1) return;

    if (fromPlayer) {
        // Attacking breaks invisibility.
        Entity& p = playerMut();
        if (p.effects.invisTurns > 0) {
            p.effects.invisTurns = 0;
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

    // "Powered" ranged magic (wands) get slightly beefier dice.
    const bool wandPowered = fromPlayer && (projKind == ProjectileKind::Spark || projKind == ProjectileKind::Fireball);

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

        if (projKind == ProjectileKind::Fireball) {
            // Fireball damage is applied by the AoE explosion at impact.
            break;
        }

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

    // --- Fireball special-case ---
    // Fireballs always explode at their final impact point, dealing AoE damage.
    if (projKind == ProjectileKind::Fireball) {
        // If we hit a wall/closed door, explode on the last reachable tile to avoid "blasting through".
        size_t impactIdx = stopIdx;
        if (hitWall && stopIdx > 1) {
            impactIdx = stopIdx - 1;
        }
        if (impactIdx >= line.size()) impactIdx = line.size() - 1;

        const Vec2i center = line[impactIdx];

        // Minimal feedback when the bolt doesn't connect with a creature.
        if (!hitAny) {
            if (hitWall) {
                if (fromPlayer) pushMsg("THE FIREBALL HITS A WALL.", MessageKind::Warning, true);
            } else {
                if (fromPlayer) pushMsg("YOU LAUNCH A FIREBALL.", MessageKind::Combat, true);
            }
        }

        // Blast radius (chebyshev): 1 tile => 3x3.
        const int radius = 1;

        // Compute a small FOV mask from the impact point so walls/closed doors block the blast.
        std::vector<uint8_t> blastMask;
        dung.computeFovMask(center.x, center.y, radius, blastMask);
        auto maskIdx = [&](int x, int y) -> int {
            return y * dung.width + x;
        };

        std::vector<Vec2i> blastTiles;
        blastTiles.reserve(static_cast<size_t>((2 * radius + 1) * (2 * radius + 1)));
        for (int y = center.y - radius; y <= center.y + radius; ++y) {
            for (int x = center.x - radius; x <= center.x + radius; ++x) {
                if (!dung.inBounds(x, y)) continue;
                const int ii = maskIdx(x, y);
                if (ii < 0 || ii >= static_cast<int>(blastMask.size())) continue;
                if (blastMask[ii] == 0) continue;
                blastTiles.push_back({x, y});
            }
        }
        if (blastTiles.empty() && dung.inBounds(center.x, center.y)) {
            blastTiles.push_back(center);
        }

        // FX projectile path (truncate to impact)
        std::vector<Vec2i> fxPath;
        fxPath.reserve(impactIdx + 1);
        for (size_t i = 0; i <= impactIdx && i < line.size(); ++i) fxPath.push_back(line[i]);

        FXProjectile fxp;
        fxp.kind = projKind;
        fxp.path = std::move(fxPath);
        fxp.pathIndex = (fxp.path.size() > 1) ? 1 : 0;
        fxp.stepTimer = 0.0f;
        fxp.stepTime = 0.03f;
        const float travelDelay = fxp.stepTime * static_cast<float>((fxp.path.size() > 0) ? (fxp.path.size() - 1) : 0);
        fx.push_back(std::move(fxp));

        FXExplosion ex;
        ex.tiles = blastTiles;
        ex.delay = travelDelay;
        ex.timer = 0.0f;
        ex.duration = 0.18f;
        fxExpl.push_back(std::move(ex));

        // Explosion noise (louder than a normal shot).
        emitNoise(center, 18);

        // System-level blast interactions: burn webs and sometimes blow doors open.
        int websBurnedSeen = 0;
        for (size_t ti = 0; ti < trapsCur.size(); ) {
            Trap& tr = trapsCur[ti];
            if (tr.kind == TrapKind::Web) {
                bool inBlast = false;
                for (const Vec2i& bt : blastTiles) {
                    if (bt.x == tr.pos.x && bt.y == tr.pos.y) { inBlast = true; break; }
                }
                if (inBlast) {
                    if (dung.inBounds(tr.pos.x, tr.pos.y) && dung.at(tr.pos.x, tr.pos.y).visible) {
                        ++websBurnedSeen;
                    }
                    trapsCur.erase(trapsCur.begin() + static_cast<std::vector<Trap>::difference_type>(ti));
                    continue;
                }
            }
            ++ti;
        }
        if (websBurnedSeen > 0 && fromPlayer) {
            pushMsg(websBurnedSeen == 1 ? "A WEB BURNS AWAY." : "WEBS BURN AWAY.", MessageKind::System, true);
        }

        int doorsBlownSeen = 0;
        for (const Vec2i& bt : blastTiles) {
            if (!dung.inBounds(bt.x, bt.y)) continue;
            Tile& tt = dung.at(bt.x, bt.y);
            if (tt.type == TileType::DoorClosed) {
                if (rng.chance(0.35f)) {
                    tt.type = TileType::DoorOpen;
                    if (tt.visible) ++doorsBlownSeen;
                }
            } else if (tt.type == TileType::DoorLocked) {
                if (rng.chance(0.15f)) {
                    tt.type = TileType::DoorOpen;
                    if (tt.visible) ++doorsBlownSeen;
                }
            }
        }
        if (doorsBlownSeen > 0 && fromPlayer) {
            pushMsg(doorsBlownSeen == 1 ? "A DOOR IS BLOWN OPEN." : "SOME DOORS ARE BLOWN OPEN.", MessageKind::System, true);
        }

        // Explosion message (shown immediately; visual flash plays after the projectile).
        if (fromPlayer) {
            pushMsg("THE FIREBALL EXPLODES!", MessageKind::Combat, true);
        } else if (dung.inBounds(center.x, center.y) && dung.at(center.x, center.y).visible) {
            pushMsg("A FIREBALL EXPLODES!", MessageKind::Combat, false);
        }

        // Damage entities in the blast.
        const DiceExpr baseDice = rangedDiceForProjectile(projKind, wandPowered);
        for (const Vec2i& bt : blastTiles) {
            Entity* e = entityAtMut(bt.x, bt.y);
            if (!e || e->hp <= 0) continue;

            // Distance falloff is very mild (radius 1).
            const int dist = std::max(std::abs(bt.x - center.x), std::abs(bt.y - center.y));

            int dmg = rollDice(rng, baseDice);
            dmg += dmgBonus;
            dmg += statDamageBonusFromAtk(attacker.baseAtk);

            const int dr = damageReduction(*this, *e);
            const int absorbed = (dr > 0) ? rng.range(0, dr) : 0;
            dmg = std::max(0, dmg - absorbed);
            dmg = std::max(0, dmg - dist);

            if (fromPlayer && e->kind == EntityKind::Shopkeeper && !e->alerted) {
                e->alerted = true;
                e->lastKnownPlayerPos = attacker.pos;
                e->lastKnownPlayerAge = 0;
                pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
            }

            if (dmg > 0) e->hp -= dmg;

            const bool tileVisible = dung.inBounds(bt.x, bt.y) && dung.at(bt.x, bt.y).visible;
            if (e->kind == EntityKind::Player) {
                std::ostringstream ss;
                ss << "YOU ARE CAUGHT IN THE BLAST";
                if (dmg > 0) ss << " FOR " << dmg;
                else ss << " BUT TAKE NO DAMAGE";
                ss << ".";
                pushMsg(ss.str(), MessageKind::Combat, false);
            } else if (fromPlayer || tileVisible) {
                std::ostringstream ss;
                ss << kindName(e->kind) << " IS HIT";
                if (dmg > 0) ss << " FOR " << dmg;
                else ss << " BUT TAKES NO DAMAGE";
                ss << ".";
                pushMsg(ss.str(), MessageKind::Combat, fromPlayer);
            }

            if (e->hp <= 0) {
                if (e->kind == EntityKind::Player) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) {
                        endCause_ = fromPlayer ? "KILLED BY YOUR OWN FIREBALL" : (std::string("KILLED BY ") + kindName(attacker.kind));
                    }
                    gameOver = true;
                    break;
                } else {
                    const bool vis = dung.inBounds(e->pos.x, e->pos.y) && dung.at(e->pos.x, e->pos.y).visible;
                    if (fromPlayer || vis) {
                        std::ostringstream ds;
                        ds << kindName(e->kind) << " DIES.";
                        pushMsg(ds.str(), MessageKind::Combat, fromPlayer);
                    }
                    if (fromPlayer) {
                        ++killCount;
                        grantXp(xpFor(e->kind));
                    }
                }
            }
        }

        inputLock = true;
        return;
    }

    if (!hitAny) {
        if (hitWall) {
            if (fromPlayer) pushMsg("THE SHOT HITS A WALL.", MessageKind::Warning, true);
        } else {
            if (fromPlayer) pushMsg("YOU FIRE.", MessageKind::Combat, true);
        }
    }
    // Recoverable ammo: arrows/rocks may remain on the ground after firing.
    // We treat this as "breakage/loss" rather than a raw drop chance, and we preserve the projectile's
    // metadata (shopPrice/shopDepth, etc.) when a template is provided (player ammo).
    if (projKind == ProjectileKind::Arrow || projKind == ProjectileKind::Rock) {
        const ItemKind ammoKind = (projKind == ProjectileKind::Arrow) ? ItemKind::Arrow : ItemKind::Rock;

        // Default landing tile is the last tile the projectile reached.
        Vec2i land = line[stopIdx];

        // If we hit a wall/closed door, the projectile can't occupy that tile; land on the last reachable tile instead.
        if (hitWall && stopIdx > 0) {
            land = line[stopIdx - 1];
        }

        if (dung.inBounds(land.x, land.y) && !dung.isOpaque(land.x, land.y)) {
            const TileType tt = dung.at(land.x, land.y).type;

            // Chasms eat physical projectiles.
            if (tt == TileType::Chasm) {
                if (fromPlayer && dung.at(land.x, land.y).visible) {
                    if (projKind == ProjectileKind::Arrow) pushMsg("YOUR ARROW PLUMMETS INTO THE CHASM.", MessageKind::Warning, true);
                    else pushMsg("YOUR ROCK PLUMMETS INTO THE CHASM.", MessageKind::Warning, true);
                }
            } else {
                // Break chance depends on what we struck.
                float breakChance = 0.0f;
                if (projKind == ProjectileKind::Arrow) {
                    breakChance = hitAny ? 0.35f : (hitWall ? 0.25f : 0.15f);
                } else { // Rock
                    breakChance = hitAny ? 0.10f : (hitWall ? 0.08f : 0.05f);
                }

                // Slightly higher loss rate for monster-fired ammo to reduce clutter.
                if (!fromPlayer) breakChance = std::min(0.95f, breakChance + 0.10f);

                if (rng.chance(breakChance)) {
                    if (fromPlayer && dung.at(land.x, land.y).visible) {
                        if (projKind == ProjectileKind::Arrow) pushMsg("YOUR ARROW SHATTERS.", MessageKind::System, true);
                        else pushMsg("YOUR ROCK SHATTERS.", MessageKind::System, true);
                    }
                } else {
                    Item drop;
                    if (projectileTemplate && projectileTemplate->kind == ammoKind) {
                        drop = *projectileTemplate;
                    } else {
                        drop.kind = ammoKind;
                        drop.count = 1;
                        drop.enchant = 0;
                        drop.charges = 0;
                        drop.buc = 0;
                        drop.shopPrice = 0;
                        drop.shopDepth = 0;
                    }
                    drop.count = 1;

                    dropGroundItemItem(land, drop);
                }
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
