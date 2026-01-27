#include "game.hpp"
#include "artifact_gen.hpp"

#include "pet_gen.hpp"
#include "proc_names.hpp"

#include "combat_rules.hpp"
#include "physics.hpp"
#include "projectile_utils.hpp"

#include <algorithm>
#include <array>
#include <sstream>

namespace {

inline int corrosionPenalty(const Entity& e) {
    if (e.effects.corrosionTurns <= 0) return 0;
    // Matches player-side penalty (game.cpp): mild but meaningful.
    return clampi(1 + e.effects.corrosionTurns / 4, 1, 3);
}

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
        case EntityKind::Dog: return "DOG";
        case EntityKind::Ghost: return "GHOST";
        case EntityKind::Leprechaun: return "LEPRECHAUN";
        case EntityKind::Nymph: return "NYMPH";
        case EntityKind::Zombie: return "ZOMBIE";
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

// Hostile procedural variants: deterministic codename for message text.
static std::string kindNameForMsg(const Entity& e, bool hallu) {
    std::string base = kindName(e.kind);
    if (hallu) return base;
    if (procname::shouldShowCodename(e)) {
        const std::string code = procname::codename(e);
        if (!code.empty()) {
            return code + " " + base;
        }
    }
    return base;
}

// A player-facing description for what a projectile collided with.
// Keep this intentionally vague for secret doors to avoid spoiling hidden content.
const char* projectileBlockerNoun(TileType t) {
    switch (t) {
        case TileType::DoorClosed:
        case TileType::DoorLocked:
            return "A DOOR";
        case TileType::Pillar:
            return "A PILLAR";
        case TileType::Boulder:
            return "A BOULDER";
        // Secret doors should look/sound like walls until discovered.
        case TileType::DoorSecret:
            return "A WALL";
        case TileType::Wall:
        default:
            return "A WALL";
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
    const int def = (e.kind == EntityKind::Player) ? game.playerDefense() : (e.baseDef - corrosionPenalty(e));
    return 10 + def;
}


int damageReduction(const Game& game, const Entity& e) {
    // Monsters: base DEF represents hide/toughness. Equipped armor (if any) adds DR.
    if (e.kind != EntityKind::Player) {
        int dr = std::max(0, e.baseDef - corrosionPenalty(e));

        if (monsterCanEquipArmor(e.kind) && e.gearArmor.id != 0 && isArmor(e.gearArmor.kind)) {
            const Item& a = e.gearArmor;
            const int b = (a.buc < 0) ? -1 : (a.buc > 0 ? 1 : 0);
            dr += itemDef(a.kind).defense + a.enchant + b;
            // Artifact wards add extra "toughness" for monsters (DR).
            dr += artifactgen::passiveBonusDefense(a);
        }

        // Temporary shielding (from potions or procedural abilities) adds flat damage
        // reduction for monsters. This keeps the effect meaningful even though
        // monsters don't use the player's evasion/armor split.
        if (e.effects.shieldTurns > 0) {
            dr += 2;
        }

        return std::max(0, dr);
    }

    // Player DR is based on worn armor (and temporary shielding).
    // We don't want baseDef (dodge) to reduce damage, only to avoid getting hit.
    const int evasion = game.playerEvasion();
    return std::max(0, game.playerDefense() - evasion);
}


} // namespace


void Game::awardCapturedPetProgress(Entity& pet, int xpGain, int bondGain, bool showMsgs) {
    if (xpGain <= 0 && bondGain <= 0) return;
    if (pet.hp <= 0) return;
    if (!pet.friendly) return;
    if (pet.id == player().id) return;

    // Find a matching full capture sphere in the player's inventory.
    // (This is what persists the pet's bond/level/xp across recalls.)
    Item* sphere = nullptr;
    for (auto& it : inv) {
        if (!isCaptureSphereFullKind(it.kind)) continue;
        if (it.enchant != static_cast<int>(pet.kind)) continue;
        if (it.spriteSeed != pet.spriteSeed) continue;
        sphere = &it;
        break;
    }
    if (!sphere) return;

    const int oldBond = clampi(captureSphereBondFromCharges(sphere->charges), 0, 99);
    const int oldTier = oldBond / 25;
    const int oldLevel = clampi(captureSpherePetLevelOrDefault(sphere->charges), 1, captureSpherePetLevelCap());
    const int oldXp = clampi(captureSpherePetXpOrZero(sphere->charges), 0, 255);
    const int hpStoredPct = clampi(captureSphereHpPctFromCharges(sphere->charges), 0, 100);

    int bond = clampi(oldBond + bondGain, 0, 99);
    int tier = bond / 25;

    const int cap = captureSpherePetLevelCap();
    int level = oldLevel;
    int petXp = oldXp + xpGain;
    bool leveled = false;

    while (level < cap) {
        const int need = captureSpherePetXpToNext(level);
        if (petXp < need) break;
        petXp -= need;
        ++level;
        leveled = true;
    }
    if (level >= cap) {
        level = cap;
        petXp = 0;
    }
    petXp = clampi(petXp, 0, 255);

    // Persist updated progression into the sphere.
    sphere->charges = packCaptureSphereCharges(bond, hpStoredPct, level, petXp);

    // Apply incremental growth immediately to the in-world entity so the player feels it.
    const int atkDelta = captureSpherePetAtkBonus(level) - captureSpherePetAtkBonus(oldLevel);
    const int defDelta = captureSpherePetDefBonus(level) - captureSpherePetDefBonus(oldLevel);
    const int hpDelta  = captureSpherePetHpBonus(level) - captureSpherePetHpBonus(oldLevel);

    if (atkDelta != 0) pet.baseAtk += atkDelta;
    if (defDelta != 0) pet.baseDef += defDelta;
    if (hpDelta != 0) {
        pet.hpMax += hpDelta;
        pet.hp += hpDelta;
    }

    // Bond tier deltas (must mirror the release-time bonuses).
    if (oldTier < 1 && tier >= 1) pet.baseAtk += 1;
    if (oldTier < 2 && tier >= 2) pet.baseDef += 1;
    if (oldTier < 3 && tier >= 3) {
        pet.hpMax += 3;
        pet.hp += 3;
    }

    pet.hpMax = std::max(1, pet.hpMax);
    pet.hp = clampi(pet.hp, 1, pet.hpMax);

    if (!showMsgs) return;

    const std::string pname = (pet.spriteSeed != 0u)
        ? petgen::petGivenName(pet.spriteSeed)
        : (std::string("YOUR ") + kindName(pet.kind));

    if (leveled) {
        std::ostringstream ss;
        ss << pname << " LEVELS UP! (LV " << level << ")";
        pushMsg(ss.str(), MessageKind::Success, true);

        if (dung.inBounds(pet.pos.x, pet.pos.y) && dung.at(pet.pos.x, pet.pos.y).visible) {
            pushFxParticle(FXParticlePreset::Buff, pet.pos, 10, 0.30f);
        }
    }

    if (tier > oldTier) {
        std::ostringstream ss;
        ss << pname << " BOND RANK UP! (TIER " << tier << ")";
        pushMsg(ss.str(), MessageKind::Info, true);
    }
}


void Game::awardBountyProgress(EntityKind killedKind, bool showMsgs) {
    // Bounty contracts live in inventory; we advance any matching active contracts.
    // We keep messaging light to avoid spam: only notify on completion.
    bool completedAny = false;

    for (auto& it : inv) {
        if (it.kind != ItemKind::BountyContract) continue;

        const int rawTarget = bountyTargetKindFromCharges(it.charges);
        if (rawTarget < 0 || rawTarget >= ENTITY_KIND_COUNT) continue;
        if (static_cast<EntityKind>(rawTarget) != killedKind) continue;

        const int req = clampi(bountyRequiredKillsFromCharges(it.charges), 1, 255);
        const int prog = clampi(bountyProgressFromEnchant(it.enchant), 0, 255);
        if (prog >= req) continue;

        const int next = std::min(req, prog + 1);
        it.enchant = withBountyProgress(it.enchant, next);

        if (next >= req) {
            completedAny = true;
        }
    }

    if (completedAny && showMsgs) {
        std::ostringstream ss;
        ss << "BOUNTY COMPLETE! USE THE CONTRACT TO CLAIM YOUR REWARD.";
        pushSystemMessage(ss.str());
    }
}

void Game::applyKillMoraleShock(const Entity& killer, const Entity& victim, int overkill, bool assassinationStyle) {
    // Only apply morale shock for player-side kills of hostiles.
    // Fear in this codebase is specifically "fear of the player" (AI flees from player position),
    // so we keep this mechanic tightly scoped to avoid weird cross-faction behavior.
    if (victim.hp > 0) return;
    if (victim.kind == EntityKind::Player) return;
    if (victim.friendly) return;
    if (!(killer.kind == EntityKind::Player || killer.friendly)) return;

    // Base "panic DC" is driven by how formidable the fallen enemy was.
    int tier = procRankTier(victim.procRank); // 0..3
    int dc = 8 + tier * 2; // 8,10,12,14
    if (procHasAffix(victim.procAffixMask, ProcMonsterAffix::Commander)) dc += 2;
    if (victim.kind == EntityKind::Minotaur) dc += 2; // boss-like presence
    if (overkill >= std::max(1, victim.hpMax / 2)) dc += 1;

    // Assassinations are quieter and less likely to trigger a broad panic cascade.
    if (assassinationStyle) dc -= 2;

    dc = clampi(dc, 6, 18);

    // Witness radius: tighter for assassinations so stealth stays meaningful.
    int radius = assassinationStyle ? 5 : 7;
    radius = clampi(radius, 4, 9);

    int newlyFrightenedVisible = 0;
    int newlyFrightenedAny = 0;
    EntityKind sampleKind = EntityKind::Goblin;
    bool sampleSet = false;

    for (auto& e : ents) {
        if (e.hp <= 0) continue;
        if (e.kind == EntityKind::Player) continue;
        if (e.friendly) continue; // only hostiles can be intimidated into fleeing the player
        if (e.id == victim.id) continue;

        // Chebyshev distance.
        const int dx = std::abs(e.pos.x - victim.pos.x);
        const int dy = std::abs(e.pos.y - victim.pos.y);
        const int dist = (dx > dy) ? dx : dy;
        if (dist > radius) continue;

        // Must have a clear line of sight to the kill site.
        if (!dung.hasLineOfSight(e.pos.x, e.pos.y, victim.pos.x, victim.pos.y)) continue;

        int localDc = dc;
        // Closer witnesses are more likely to panic.
        if (dist <= 2) localDc += 1;
        // Losing a packmate is extra destabilizing.
        if (victim.groupId != 0 && e.groupId == victim.groupId) localDc += 1;
        localDc = clampi(localDc, 6, 20);

        // "Morale save" bonus: tougher monsters and those under commander aura resist panic.
        int bonus = procRankTier(e.procRank);
        bonus += commanderAuraTierFor(e);
        if (e.kind == EntityKind::Guard || e.kind == EntityKind::Shopkeeper) bonus += 2;

        // Mindless undead mostly ignore fear-of-player effects.
        if (e.kind == EntityKind::Zombie || e.kind == EntityKind::Ghost || e.kind == EntityKind::SkeletonArcher) {
            bonus += 6;
        }

        // Wounded creatures are easier to break.
        if (e.hpMax > 0 && e.hp <= std::max(1, e.hpMax / 3)) {
            bonus -= 1;
        }

        // Deterministic d20 roll (no RNG consumption).
        // Domain-separated so other deterministic hashes don't collide in patterns.
        uint32_t h = hashCombine(seed_, "MORALE_SHOCK"_tag);
        h = hashCombine(h, turnCount);
        h = hashCombine(h, static_cast<uint32_t>(killer.id));
        h = hashCombine(h, static_cast<uint32_t>(victim.id));
        h = hashCombine(h, static_cast<uint32_t>(e.id));
        h = hashCombine(h, static_cast<uint32_t>(victim.kind));
        h = hashCombine(h, static_cast<uint32_t>(e.kind));

        const int roll = 1 + static_cast<int>(h % 20u);

        bool fail = false;
        if (roll == 1) fail = true;
        else if (roll == 20) fail = false;
        else fail = (roll + bonus) < localDc;

        if (!fail) continue;

        const int before = e.effects.fearTurns;
        int diff = localDc - (roll + bonus);
        if (diff < 1) diff = 1;

        // Duration scales with how badly they failed the morale save.
        const int turns = clampi(2 + diff, 2, 16);
        e.effects.fearTurns = std::max(e.effects.fearTurns, turns);

        if (before == 0 && e.effects.fearTurns > 0) {
            ++newlyFrightenedAny;
            if (!sampleSet) {
                sampleKind = e.kind;
                sampleSet = true;
            }
            if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                ++newlyFrightenedVisible;
            }
        }
    }

    // Player-facing feedback only when at least one panicking enemy is visible.
    // (No "telepathic" off-screen panic messages.)
    if (newlyFrightenedVisible > 0) {
        if (newlyFrightenedVisible >= 3) {
            pushMsg("ENEMIES PANIC!", MessageKind::Info, true);
        } else if (newlyFrightenedVisible == 2) {
            pushMsg("THE ENEMIES RECOIL IN TERROR!", MessageKind::Info, true);
        } else {
            std::ostringstream ss;
            ss << "THE " << kindName(sampleKind) << " PANICS!";
            pushMsg(ss.str(), MessageKind::Info, true);
        }

        if (dung.inBounds(victim.pos.x, victim.pos.y) && dung.at(victim.pos.x, victim.pos.y).visible) {
            pushFxParticle(FXParticlePreset::Detect, victim.pos, 10, 0.20f);
        }
    }
}



void Game::attackMelee(Entity& attacker, Entity& defender, bool kick) {
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
            triggerShopTheftAlarm(defender.pos, attacker.pos);
        }
    }

    int atkBonus = 0;
    if (attacker.kind == EntityKind::Player) {
        // Kick is deliberately unarmed: it ignores wielded weapon accuracy.
        atkBonus = kick ? (playerMeleePower() - 1) : playerAttack();
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

    // Commander aura: nearby commander-type allies can boost monster accuracy.
    const int inspireTier = (attacker.kind != EntityKind::Player) ? commanderAuraTierFor(attacker) : 0;
    if (inspireTier > 0) {
        atkBonus += inspireTier;
    }

    const int ac = targetAC(*this, defender);
    const HitCheck hc = rollToHit(rng, atkBonus, ac);

    const bool msgFromPlayer = (attacker.kind == EntityKind::Player);

    if (!hc.hit) {
        std::ostringstream ss;
        if (attacker.kind == EntityKind::Player) {
            ss << "YOU MISS " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << ".";
        } else if (defender.kind == EntityKind::Player) {
            ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " MISSES YOU.";
        } else {
            ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " MISSES " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << ".";
        }
        pushMsg(ss.str(), MessageKind::Combat, msgFromPlayer);
        if (attacker.kind == EntityKind::Player) {
            // Even a miss makes noise.
            emitNoise(attacker.pos, 7);
        }

        // Parry stance: if the player is parrying and an enemy misses in melee,
        // they may instantly counterattack (riposte). The first turn after you
        // enter the stance is a guaranteed "perfect parry" window.
        if (defender.kind == EntityKind::Player && attacker.kind != EntityKind::Player && defender.effects.parryTurns > 0) {
            const bool perfect = (defender.effects.parryTurns >= 2);

            float chance = 1.0f;
            if (!perfect) {
                // Base riposte chance is modest; agility helps.
                const int agi = playerAgility();
                chance = 0.35f + 0.03f * static_cast<float>(agi);

                // Heavy armor makes clean ripostes harder.
                if (const Item* a = equippedArmor()) {
                    if (a->kind == ItemKind::ChainArmor) chance -= 0.10f;
                    if (a->kind == ItemKind::PlateArmor) chance -= 0.15f;
                }

                if (chance < 0.35f) chance = 0.35f;
                if (chance > 0.75f) chance = 0.75f;
            }

            if (rng.chance(chance)) {
                if (perfect) pushMsg("YOU PERFECTLY PARRY!", MessageKind::Combat, true);
                else pushMsg("YOU RIPOSTE!", MessageKind::Combat, true);

                // Perfect parry staggers the attacker briefly.
                if (perfect) {
                    attacker.effects.confusionTurns = std::max(attacker.effects.confusionTurns, 2);
                }

                // Parry stance is consumed on the first riposte attempt to prevent
                // multi-ripostes in a single monster phase.
                defender.effects.parryTurns = 0;

                // Riposte attack (does not consume an extra turn).
                attackMelee(defender, attacker, false);
            }
        }

        return;
    }

    // A successful melee hit can partially disrupt a parry stance.
    if (defender.kind == EntityKind::Player && attacker.kind != EntityKind::Player && defender.effects.parryTurns > 0) {
        defender.effects.parryTurns = std::max(0, defender.effects.parryTurns - 1);
    }

    // Roll damage.
    DiceExpr baseDice{1, 2, 0};
    int atkStatForBonus = attacker.baseAtk;
    if (attacker.kind == EntityKind::Player) {
        atkStatForBonus += playerMight();
    }
    int bonus = statDamageBonusFromAtk(atkStatForBonus);

    if (attacker.kind == EntityKind::Player) {
        if (kick) {
            // Kick is unarmed (ignores wielded weapons) and slightly stronger than a punch.
            baseDice = DiceExpr{1, 3, 0};
        } else if (const Item* w = equippedMelee()) {
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

    // Commander aura adds a small damage bump (scaled gently) to inspired monsters.
    if (attacker.kind != EntityKind::Player && inspireTier > 0) {
        if (inspireTier >= 2) dmg += 1;
        if (inspireTier >= 3) dmg += 1;
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
        ss << "YOU " << (hc.crit ? "CRIT " : "") << (kick ? "KICK " : "HIT ") << kindNameForMsg(defender, player().effects.hallucinationTurns > 0);
        if (dmg > 0) ss << " FOR " << dmg;
        else ss << " BUT DO NO DAMAGE";
        ss << ".";
    } else if (defender.kind == EntityKind::Player) {
        if (kick) ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " " << (hc.crit ? "CRIT KICKS" : "KICKS") << " YOU";
        else ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " " << (hc.crit ? "CRITS" : "HITS") << " YOU";
        if (dmg > 0) ss << " FOR " << dmg;
        else ss << " BUT DOES NO DAMAGE";
        ss << ".";
    } else {
        ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << (kick ? " KICKS " : " HITS ") << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << ".";
    }
    pushMsg(ss.str(), MessageKind::Combat, msgFromPlayer);

    if (attacker.kind == EntityKind::Player) {
        // Fighting is noisy; nearby monsters may investigate.
        emitNoise(attacker.pos, 11);
    }

    // Weapon ego procs (NetHack-style brands).
    // Kept intentionally lightweight: small extra effects that reward rare loot.
    if (!kick) {
        ItemEgo ego = ItemEgo::None;
        if (attacker.kind == EntityKind::Player) {
            if (const Item* w = equippedMelee()) ego = w->ego;
        } else {
            if (monsterCanEquipWeapons(attacker.kind) && attacker.gearMelee.id != 0 && isMeleeWeapon(attacker.gearMelee.kind)) {
                ego = attacker.gearMelee.ego;
            }
        }

        auto canSee = [&](const Entity& e) -> bool {
            if (e.kind == EntityKind::Player) return true;
            if (!dung.inBounds(e.pos.x, e.pos.y)) return false;
            return dung.at(e.pos.x, e.pos.y).visible;
        };

        switch (ego) {
            case ItemEgo::Flaming: {
                if (defender.hp > 0 && rng.chance(0.28f)) {
                    // Scale a bit with depth so it stays relevant.
                    const int minTurns = 2 + std::min(4, depth_ / 4);
                    const int turns = rng.range(minTurns, minTurns + 3);
                    const int before = defender.effects.burnTurns;
                    defender.effects.burnTurns = std::max(defender.effects.burnTurns, turns);

                    if (before == 0 && defender.effects.burnTurns > 0) {
                        if (defender.kind == EntityKind::Player) {
                            pushMsg("YOU ARE SET AFLAME!", MessageKind::Warning, false);
                        } else if (canSee(defender)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " CATCHES FIRE!";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
                break;
            }
            case ItemEgo::Venom: {
                if (defender.hp > 0 && rng.chance(0.32f)) {
                    const int turns = rng.range(4, 8);
                    const int before = defender.effects.poisonTurns;
                    defender.effects.poisonTurns = std::max(defender.effects.poisonTurns, turns);

                    if (before == 0 && defender.effects.poisonTurns > 0) {
                        if (defender.kind == EntityKind::Player) {
                            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                        } else if (canSee(defender)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS POISONED!";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
                break;
            }
            case ItemEgo::Webbing: {
                if (defender.hp > 0 && rng.chance(0.18f)) {
                    const int base = 2 + std::min(2, depth_ / 6);
                    const int turns = rng.range(base, base + 2);
                    const int before = defender.effects.webTurns;
                    defender.effects.webTurns = std::max(defender.effects.webTurns, turns);

                    if (before == 0 && defender.effects.webTurns > 0) {
                        if (defender.kind == EntityKind::Player) {
                            pushMsg("YOU ARE CAUGHT IN STICKY WEBBING!", MessageKind::Warning, false);
                        } else if (canSee(defender)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS CAUGHT IN STICKY WEBBING!";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
                break;
            }
            case ItemEgo::Corrosive: {
                if (defender.hp > 0 && rng.chance(0.24f)) {
                    const int base = 2 + std::min(3, depth_ / 5);
                    const int turns = rng.range(base, base + 3);
                    const int before = defender.effects.corrosionTurns;
                    defender.effects.corrosionTurns = std::max(defender.effects.corrosionTurns, turns);

                    if (before == 0 && defender.effects.corrosionTurns > 0) {
                        if (defender.kind == EntityKind::Player) {
                            pushMsg("ACID SIZZLES ON YOUR SKIN!", MessageKind::Warning, false);
                        } else if (canSee(defender)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS SPLASHED WITH ACID!";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
                break;
            }
            case ItemEgo::Dazing: {
                if (defender.hp > 0 && rng.chance(0.20f)) {
                    const int base = 2 + std::min(2, depth_ / 7);
                    const int turns = rng.range(base, base + 2);
                    const int before = defender.effects.confusionTurns;
                    defender.effects.confusionTurns = std::max(defender.effects.confusionTurns, turns);

                    if (before == 0 && defender.effects.confusionTurns > 0) {
                        if (defender.kind == EntityKind::Player) {
                            pushMsg("YOU ARE DAZED!", MessageKind::Warning, false);
                        } else if (canSee(defender)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " LOOKS DAZED!";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
                break;
            }
            case ItemEgo::Vampiric: {
                if (dmg > 0 && attacker.hp > 0) {
                    int heal = 1 + dmg / 4;
                    heal = std::min(3, heal);
                    const int beforeHp = attacker.hp;
                    attacker.hp = std::min(attacker.hpMax, attacker.hp + heal);
                    if (attacker.hp > beforeHp) {
                        if (attacker.kind == EntityKind::Player) {
                            pushMsg("YOU DRAIN LIFE!", MessageKind::Success, true);
                        } else if (defender.kind == EntityKind::Player) {
                            pushMsg("YOUR LIFE IS DRAINED!", MessageKind::Warning, false);
                        } else if (canSee(attacker)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " LOOKS REINVIGORATED.";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // -------------------------------------------------------------------------
    // Artifact power procs (procedural artifacts)
    //
    // Artifacts already have deterministic titles and power tags; this layer makes
    // those tags actually matter in combat. Kept lightweight and readable:
    // - at most one offensive status proc per melee hit (FLAME/VENOM/DAZE)
    // - WARD can grant temporary shielding
    // - VITALITY can heal/grant regeneration
    {
        using AP = artifactgen::Power;
        constexpr size_t AP_N = static_cast<size_t>(AP::COUNT);

        auto canSeeEnt = [&](const Entity& e) -> bool {
            if (e.kind == EntityKind::Player) return true;
            if (!dung.inBounds(e.pos.x, e.pos.y)) return false;
            return dung.at(e.pos.x, e.pos.y).visible;
        };

        auto consider = [&](const Item& it, std::array<int, AP_N>& best) {
            if (!artifactgen::isArtifactGear(it)) return;
            const int lvl = artifactgen::powerLevel(it);
            if (lvl <= 0) return;
            const AP p = artifactgen::artifactPower(it);
            const size_t idx = static_cast<size_t>(p);
            if (idx < best.size()) best[idx] = std::max(best[idx], lvl);
        };

        auto gatherForMelee = [&](const Entity& e, std::array<int, AP_N>& best) {
            // For melee procs we only consider "relevant" worn gear:
            // - attacker: melee weapon + worn armor + rings
            // - defender: held melee + worn armor + rings
            // Ranged weapons are excluded here to avoid e.g. a bow's artifact power
            // applying to a sword strike.
            if (e.kind == EntityKind::Player) {
                if (const Item* w = equippedMelee()) consider(*w, best);
                if (const Item* a = equippedArmor()) consider(*a, best);
                if (const Item* r = equippedRing1()) consider(*r, best);
                if (const Item* r = equippedRing2()) consider(*r, best);
            } else {
                if (e.gearMelee.id != 0) consider(e.gearMelee, best);
                if (e.gearArmor.id != 0) consider(e.gearArmor, best);
            }
        };

        std::array<int, AP_N> atkBest{};
        std::array<int, AP_N> defBest{};
        gatherForMelee(attacker, atkBest);
        gatherForMelee(defender, defBest);

        auto chanceFromLvl = [](int lvl, float base, float perLevel, float cap = 0.75f) -> float {
            float c = base + perLevel * static_cast<float>(lvl);
            return std::clamp(c, 0.0f, cap);
        };

        // Offensive status: pick at most one of {FLAME, VENOM, DAZE}.
        struct Cand { AP p; int lvl; int w; };
        Cand cands[3];
        int nCand = 0;
        auto addCand = [&](AP p) {
            const int lvl = atkBest[static_cast<size_t>(p)];
            if (lvl <= 0) return;
            const int w = 3 + lvl; // slightly favor higher potency
            cands[nCand++] = Cand{p, lvl, w};
        };
        addCand(AP::Flame);
        addCand(AP::Venom);
        addCand(AP::Daze);

        if (nCand > 0 && defender.hp > 0) {
            int totalW = 0;
            for (int i = 0; i < nCand; ++i) totalW += cands[i].w;
            int roll = rng.range(1, totalW);
            AP chosen = cands[0].p;
            int lvl = cands[0].lvl;
            for (int i = 0; i < nCand; ++i) {
                if (roll <= cands[i].w) {
                    chosen = cands[i].p;
                    lvl = cands[i].lvl;
                    break;
                }
                roll -= cands[i].w;
            }

            switch (chosen) {
                case AP::Flame: {
                    const float chance = chanceFromLvl(lvl, 0.12f, 0.07f, 0.65f);
                    if (rng.chance(chance)) {
                        const int before = defender.effects.burnTurns;
                        const int turns = clampi(rng.range(2 + lvl, 4 + lvl), 2, 16);
                        defender.effects.burnTurns = std::max(defender.effects.burnTurns, turns);
                        if (before == 0 && defender.effects.burnTurns > 0) {
                            if (defender.kind == EntityKind::Player) {
                                pushMsg("YOU ARE SET AFLAME!", MessageKind::Warning, false);
                            } else if (canSeeEnt(defender)) {
                                std::ostringstream ss2;
                                ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " CATCHES FIRE!";
                                pushMsg(ss2.str(), MessageKind::Info, true);
                            }
                        }
                    }
                    break;
                }
                case AP::Venom: {
                    const float chance = chanceFromLvl(lvl, 0.14f, 0.07f, 0.70f);
                    if (rng.chance(chance)) {
                        const int before = defender.effects.poisonTurns;
                        const int turns = clampi(rng.range(3 + lvl * 2, 6 + lvl * 2), 3, 20);
                        defender.effects.poisonTurns = std::max(defender.effects.poisonTurns, turns);
                        if (before == 0 && defender.effects.poisonTurns > 0) {
                            if (defender.kind == EntityKind::Player) {
                                pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                            } else if (canSeeEnt(defender)) {
                                std::ostringstream ss2;
                                ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS POISONED!";
                                pushMsg(ss2.str(), MessageKind::Info, true);
                            }
                            if (canSeeEnt(defender)) pushFxParticle(FXParticlePreset::Poison, defender.pos, 18, 0.35f);
                        }
                    }
                    break;
                }
                case AP::Daze: {
                    const float chance = chanceFromLvl(lvl, 0.10f, 0.06f, 0.55f);
                    if (rng.chance(chance)) {
                        const int before = defender.effects.confusionTurns;
                        const int turns = clampi(rng.range(2 + lvl, 3 + lvl), 2, 14);
                        defender.effects.confusionTurns = std::max(defender.effects.confusionTurns, turns);
                        if (before == 0 && defender.effects.confusionTurns > 0) {
                            if (defender.kind == EntityKind::Player) {
                                pushMsg("YOU ARE DAZED!", MessageKind::Warning, false);
                            } else if (canSeeEnt(defender)) {
                                std::ostringstream ss2;
                                ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " LOOKS DAZED!";
                                pushMsg(ss2.str(), MessageKind::Info, true);
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // VITALITY: heal the attacker from damage dealt.
        {
            const int lvl = atkBest[static_cast<size_t>(AP::Vitality)];
            if (lvl > 0 && dmg > 0 && attacker.hp > 0) {
                const float chance = chanceFromLvl(lvl, 0.18f, 0.07f, 0.70f);
                if (rng.chance(chance)) {
                    const int heal = clampi(1 + (lvl >= 3 ? 1 : 0), 1, 2);
                    const int before = attacker.hp;
                    attacker.hp = std::min(attacker.hpMax, attacker.hp + heal);
                    if (attacker.hp > before) {
                        if (attacker.kind == EntityKind::Player) {
                            pushMsg("LIFE SURGES THROUGH YOU!", MessageKind::Success, true);
                            pushFxParticle(FXParticlePreset::Heal, attacker.pos, 18, 0.35f);
                        } else if (defender.kind == EntityKind::Player) {
                            pushMsg("THE FOE SURGES WITH VITALITY!", MessageKind::Warning, false);
                        } else if (canSeeEnt(attacker)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " LOOKS REINVIGORATED.";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
            }
        }

        // WARD: chance to grant shielding to the bearer.
        {
            const int lvlAtk = atkBest[static_cast<size_t>(AP::Ward)];
            if (lvlAtk > 0 && attacker.hp > 0) {
                const float chance = chanceFromLvl(lvlAtk, 0.10f, 0.06f, 0.55f);
                if (rng.chance(chance)) {
                    const int before = attacker.effects.shieldTurns;
                    const int turns = clampi(rng.range(2 + lvlAtk, 3 + lvlAtk), 2, 16);
                    attacker.effects.shieldTurns = std::max(attacker.effects.shieldTurns, turns);
                    if (before == 0 && attacker.effects.shieldTurns > 0) {
                        if (attacker.kind == EntityKind::Player) {
                            pushMsg("A WARD SHIMMERS AROUND YOU!", MessageKind::Success, true);
                            pushFxParticle(FXParticlePreset::Buff, attacker.pos, 18, 0.25f);
                        } else if (defender.kind == EntityKind::Player) {
                            pushMsg("THE FOE RAISES A SHIMMERING WARD!", MessageKind::Warning, false);
                        } else if (canSeeEnt(attacker)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " IS SURROUNDED BY A WARD.";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
            }

            const int lvlDef = defBest[static_cast<size_t>(AP::Ward)];
            if (lvlDef > 0 && defender.hp > 0) {
                const float chance = chanceFromLvl(lvlDef, 0.14f, 0.06f, 0.65f);
                if (rng.chance(chance)) {
                    const int before = defender.effects.shieldTurns;
                    const int turns = clampi(rng.range(2 + lvlDef, 4 + lvlDef), 2, 18);
                    defender.effects.shieldTurns = std::max(defender.effects.shieldTurns, turns);
                    if (before == 0 && defender.effects.shieldTurns > 0) {
                        if (defender.kind == EntityKind::Player) {
                            pushMsg("YOUR WARD SHIELDS YOU!", MessageKind::Success, true);
                            pushFxParticle(FXParticlePreset::Buff, defender.pos, 18, 0.25f);
                        } else if (canSeeEnt(defender)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS PROTECTED BY A WARD.";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
            }
        }

        // VITALITY (defensive): chance to grant brief regeneration when struck.
        {
            const int lvl = defBest[static_cast<size_t>(AP::Vitality)];
            if (lvl > 0 && dmg > 0 && defender.hp > 0) {
                const float chance = chanceFromLvl(lvl, 0.16f, 0.06f, 0.60f);
                if (rng.chance(chance)) {
                    const int before = defender.effects.regenTurns;
                    const int turns = clampi(rng.range(2 + lvl, 4 + lvl), 2, 18);
                    defender.effects.regenTurns = std::max(defender.effects.regenTurns, turns);
                    if (before == 0 && defender.effects.regenTurns > 0) {
                        if (defender.kind == EntityKind::Player) {
                            pushMsg("YOU FEEL YOUR WOUNDS KNIT.", MessageKind::Success, true);
                            pushFxParticle(FXParticlePreset::Heal, defender.pos, 18, 0.25f);
                        } else if (canSeeEnt(defender)) {
                            std::ostringstream ss2;
                            ss2 << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " LOOKS HEALTHIER.";
                            pushMsg(ss2.str(), MessageKind::Info, true);
                        }
                    }
                }
            }
        }
    }

    // Monster special effects.
    if (defender.hp > 0 && defender.kind == EntityKind::Player) {
        // Ghosts: chilling touch can briefly disorient you.
        // Uses the existing confusion effect to avoid save-format churn.
        if (attacker.kind == EntityKind::Ghost && rng.chance(0.25f)) {
            defender.effects.confusionTurns = std::max(defender.effects.confusionTurns, rng.range(2, 4));
            pushMsg("AN ICY TOUCH LEAVES YOU DISORIENTED!", MessageKind::Warning, false);
        }
        if (attacker.kind == EntityKind::Snake && rng.chance(0.35f)) {
            defender.effects.poisonTurns = std::max(defender.effects.poisonTurns, rng.range(4, 8));
            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
        }
        if (attacker.kind == EntityKind::Spider && rng.chance(0.45f)) {
            defender.effects.webTurns = std::max(defender.effects.webTurns, rng.range(2, 4));
            pushMsg("YOU ARE ENSNARED BY WEBBING!", MessageKind::Warning, false);
        }
        // Mimics: adhesive bodies can stick you in place briefly.
        if (attacker.kind == EntityKind::Mimic && rng.chance(0.35f)) {
            defender.effects.webTurns = std::max(defender.effects.webTurns, rng.range(2, 4));
            pushMsg("THE MIMIC'S ADHESIVE HIDE STICKS TO YOU!", MessageKind::Warning, false);
        }
    }

    // Procedural monster affix on-hit procs.
    // These give procedural variants "signature" combat identity without creating new monster kinds.
    if (attacker.kind != EntityKind::Player && defender.hp > 0 && attacker.procAffixMask != 0u) {
        const int tier = procRankTier(attacker.procRank);

        auto canSeeProc = [&]() -> bool {
            if (defender.kind == EntityKind::Player) return true;
            if (!dung.inBounds(defender.pos.x, defender.pos.y)) return false;
            return dung.at(defender.pos.x, defender.pos.y).visible;
        };

        // Venomous: poison on hit.
        if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Venomous) && defender.hp > 0) {
            float chance = 0.22f + 0.06f * static_cast<float>(tier);
            chance = std::clamp(chance, 0.0f, 0.65f);
            if (rng.chance(chance)) {
                const int before = defender.effects.poisonTurns;
                const int turns = clampi(4 + tier * 2 + rng.range(0, 3), 3, 16);
                defender.effects.poisonTurns = std::max(defender.effects.poisonTurns, turns);
                if (before == 0 && defender.effects.poisonTurns > 0) {
                    if (defender.kind == EntityKind::Player) {
                        pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                    } else if (defender.friendly && canSeeProc()) {
                        std::ostringstream ps;
                        ps << "YOUR " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS POISONED!";
                        pushMsg(ps.str(), MessageKind::Info, true);
                    }
                    if (canSeeProc()) pushFxParticle(FXParticlePreset::Poison, defender.pos, 18, 0.35f);
                }
            }
        }

        // Flaming: burn on hit.
        if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Flaming) && defender.hp > 0) {
            float chance = 0.18f + 0.05f * static_cast<float>(tier);
            chance = std::clamp(chance, 0.0f, 0.60f);
            if (rng.chance(chance)) {
                const int before = defender.effects.burnTurns;
                const int turns = clampi(3 + tier + rng.range(0, 2), 2, 12);
                defender.effects.burnTurns = std::max(defender.effects.burnTurns, turns);
                if (before == 0 && defender.effects.burnTurns > 0) {
                    if (defender.kind == EntityKind::Player) {
                        pushMsg("YOU CATCH FIRE!", MessageKind::Warning, false);
                    } else if (defender.friendly && canSeeProc()) {
                        std::ostringstream bs;
                        bs << "YOUR " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " CATCHES FIRE!";
                        pushMsg(bs.str(), MessageKind::Info, true);
                    }
                }
            }
        }

        // Webbing: ensnare on hit.
        if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Webbing) && defender.hp > 0) {
            float chance = 0.16f + 0.04f * static_cast<float>(tier);
            chance = std::clamp(chance, 0.0f, 0.55f);
            if (rng.chance(chance)) {
                const int before = defender.effects.webTurns;
                const int turns = clampi(2 + tier + rng.range(0, 2), 2, 10);
                defender.effects.webTurns = std::max(defender.effects.webTurns, turns);
                if (before == 0 && defender.effects.webTurns > 0) {
                    if (defender.kind == EntityKind::Player) {
                        pushMsg("YOU ARE ENSNARED!", MessageKind::Warning, false);
                    } else if (defender.friendly && canSeeProc()) {
                        std::ostringstream ws;
                        ws << "YOUR " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS ENSNARED!";
                        pushMsg(ws.str(), MessageKind::Info, true);
                    }
                }
            }
        }

        // Vampiric: heal attacker from damage dealt.
        if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Vampiric) && dmg > 0 && attacker.hp > 0) {
            float chance = 0.35f + 0.05f * static_cast<float>(tier);
            chance = std::clamp(chance, 0.0f, 0.75f);
            if (rng.chance(chance)) {
                const int beforeHp = attacker.hp;
                const int heal = clampi(1 + dmg / 5 + tier, 1, 10);
                attacker.hp = std::min(attacker.hpMax, attacker.hp + heal);
                if (attacker.hp > beforeHp) {
                    if (defender.kind == EntityKind::Player) {
                        pushMsg("YOUR LIFE IS DRAINED!", MessageKind::Warning, false);
                    } else if (defender.friendly && canSeeProc()) {
                        std::ostringstream vs;
                        vs << "YOUR " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " IS DRAINED!";
                        pushMsg(vs.str(), MessageKind::Info, true);
                    }
                    if (dung.inBounds(attacker.pos.x, attacker.pos.y) && dung.at(attacker.pos.x, attacker.pos.y).visible) {
                        pushFxParticle(FXParticlePreset::Heal, attacker.pos, 12, 0.30f);
                    }
                }
            }
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
                // Kicks are unarmed: weapon weight doesn't matter.
                if (!kick) {
                    if (const Item* w = equippedMelee()) {
                        return (w->kind == ItemKind::Axe);
                    }
                }
            }
            return false;
        };

        if (attacker.kind == EntityKind::Player) {
            if (kick) {
                // Kicks are a positioning tool: modest damage, higher shove chance.
                chance = 0.40f;
                kcfg.power = 2;
            } else if (const Item* w = equippedMelee()) {
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
            } else if (kick && attacker.kind == EntityKind::Player && hc.crit && rng.chance(0.25f)) {
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
                    ks << "YOU KNOCK " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " BACK!";
                } else if (defender.kind == EntityKind::Player) {
                    ks << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " KNOCKS YOU BACK!";
                } else {
                    ks << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " KNOCKS " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " BACK.";
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
                else cs << kindNameForMsg(defender, player().effects.hallucinationTurns > 0);
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
                    else cs << kindNameForMsg(defender, player().effects.hallucinationTurns > 0);
                    cs << " CRASH INTO ";
                    if (other->kind == EntityKind::Player) cs << "YOU";
                    else cs << kindNameForMsg(*other, player().effects.hallucinationTurns > 0);
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
                    fs << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " FALLS INTO THE CHASM!";
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
                    fs << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " DODGES THE CHASM.";
                    pushMsg(fs.str(), MessageKind::Info, msgFromPlayer);
                }

            }

            // If the player's position changed (or a door got smashed open), refresh FOV immediately.
            // Forced movement can otherwise leave vision stale until the next player action.
            if (player().hp > 0 && ((defender.kind == EntityKind::Player && kb.stepsMoved > 0) || kb.doorChanged)) {
                recomputeFov();
            }
        }
    }

    if (defender.hp <= 0) {
        if (defender.kind == EntityKind::Player) {
            pushMsg("YOU DIE.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = std::string("KILLED BY ") + kindNameForMsg(attacker, player().effects.hallucinationTurns > 0);
            gameOver = true;
        } else {
            if (!skipDeathMsg) {
                std::ostringstream ds;
                if (defender.friendly) {
                    ds << "YOUR " << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " DIES.";
                } else {
                    ds << kindNameForMsg(defender, player().effects.hallucinationTurns > 0) << " DIES.";
                }
                pushMsg(ds.str(), MessageKind::Combat, msgFromPlayer);
            }

            if ((attacker.kind == EntityKind::Player || attacker.friendly) && !defender.friendly) {
                ++killCount;
                if (attacker.kind == EntityKind::Player && directKillCount_ < 0xFFFFFFFFu) {
                    ++directKillCount_;
                }

                const size_t kidx = static_cast<size_t>(defender.kind);
                if (kidx < codexKills_.size()) {
                    codexSeen_[kidx] = 1;
                    if (codexKills_[kidx] < 65535) ++codexKills_[kidx];
                }

                const int xpAward = xpFor(defender);
                grantXp(xpAward);
                awardBountyProgress(defender.kind, true);

                // Captured companion progression (Palworld/Pokemon-style).
                // - If a captured pet gets the kill: it gains XP and bond.
                // - If the player gets the kill: all currently-out captured pets gain a small XP share.
                if (attacker.friendly) {
                    const int petXp = clampi((xpAward + 1) / 2, 1, 120);
                    const int bondGain = clampi(1 + xpAward / 20, 1, 5);
                    const bool show = dung.inBounds(attacker.pos.x, attacker.pos.y) && dung.at(attacker.pos.x, attacker.pos.y).visible;
                    awardCapturedPetProgress(attacker, petXp, bondGain, show);
                } else if (attacker.kind == EntityKind::Player) {
                    const int share = std::max(1, xpAward / 3);
                    for (auto& e : ents) {
                        if (e.hp <= 0) continue;
                        if (!e.friendly) continue;
                        if (e.id == player().id) continue;
                        const bool show = dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible;
                        awardCapturedPetProgress(e, share, /*bondGain=*/0, show);
                    }
                }
                const int overkill = std::max(0, -defender.hp);
                const bool assassinationStyle = (attacker.kind == EntityKind::Player) && (attackerWasSneaking || attackerWasInvisible);
                applyKillMoraleShock(attacker, defender, overkill, assassinationStyle);
            }
        }
    }
}


void Game::attackRanged(Entity& attacker, Vec2i target, int range, int atkBonus, int dmgBonus, ProjectileKind projKind, bool fromPlayer, const Item* projectileTemplate, bool wandPowered) {
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

    // Commander aura: inspired monsters gain accuracy and a touch of extra damage.
    if (attacker.kind != EntityKind::Player) {
        const int inspireTier = commanderAuraTierFor(attacker);
        if (inspireTier > 0) {
            atkBonus += inspireTier;
            if (inspireTier >= 2) dmgBonus += 1;
            if (inspireTier >= 3) dmgBonus += 1;
        }
    }

    // Wind drift: physical projectiles are biased by the level's deterministic wind.
    // This is intentionally simple and readable: players can compensate by aiming into the wind.
    if (projKind == ProjectileKind::Arrow || projKind == ProjectileKind::Rock || projKind == ProjectileKind::Torch) {
        const WindShotAdjust wa = windAdjustShot(attacker.pos, target, range, projKind);
        target = wa.adjustedTarget;
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
    TileType hitWallTile = TileType::Wall;
    bool hitAny = false;
    Entity* hit = nullptr;
    size_t stopIdx = line.size() - 1;

    bool sparkCrit = false;

    // Whether Spark/Fireball use the stronger wand damage profile.
    // Provided by the caller so player spellcasting can use the weaker baseline.

    // Shared death + XP bookkeeping for this attack, so chained effects (like spark arcs)
    // can award kills consistently without duplicating long blocks of logic.
    auto handleRangedDeath = [&](Entity& victim) {
        if (victim.hp > 0) return;

        if (victim.kind == EntityKind::Player) {
            pushMsg("YOU DIE.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = std::string("KILLED BY ") + kindNameForMsg(attacker, player().effects.hallucinationTurns > 0);
            gameOver = true;
            return;
        }

        std::ostringstream ds;
        if (victim.friendly) ds << "YOUR " << kindNameForMsg(victim, player().effects.hallucinationTurns > 0) << " DIES.";
        else ds << kindNameForMsg(victim, player().effects.hallucinationTurns > 0) << " DIES.";
        pushMsg(ds.str(), MessageKind::Combat, fromPlayer);

        if ((attacker.kind == EntityKind::Player || attacker.friendly) && !victim.friendly) {
            ++killCount;
            if (attacker.kind == EntityKind::Player && directKillCount_ < 0xFFFFFFFFu) {
                ++directKillCount_;
            }

            const size_t kidx = static_cast<size_t>(victim.kind);
            if (kidx < codexKills_.size()) {
                codexSeen_[kidx] = 1;
                if (codexKills_[kidx] < 65535) ++codexKills_[kidx];
            }

            const int xpAward = xpFor(victim);
            grantXp(xpAward);
            awardBountyProgress(victim.kind, true);

            // Captured pet progression:
            // - If a captured pet gets the kill: it gains XP and bond.
            // - If the player gets the kill: all currently-out captured pets gain a small XP share.
            if (attacker.friendly) {
                const int petXp = clampi((xpAward + 1) / 2, 1, 120);
                const int bondGain = clampi(1 + xpAward / 20, 1, 5);
                const bool show = dung.inBounds(attacker.pos.x, attacker.pos.y) && dung.at(attacker.pos.x, attacker.pos.y).visible;
                awardCapturedPetProgress(attacker, petXp, bondGain, show);
            } else if (attacker.kind == EntityKind::Player) {
                const int share = std::max(1, xpAward / 3);
                for (auto& ally : ents) {
                    if (ally.hp <= 0) continue;
                    if (!ally.friendly) continue;
                    if (ally.id == player().id) continue;
                    const bool show = dung.inBounds(ally.pos.x, ally.pos.y) && dung.at(ally.pos.x, ally.pos.y).visible;
                    awardCapturedPetProgress(ally, share, /*bondGain=*/0, show);
                }
            }
            const int overkill = std::max(0, -victim.hp);
            applyKillMoraleShock(attacker, victim, overkill, /*assassinationStyle=*/false);
        }
    };

    // Projectiles travel the full line. If they miss a creature, they keep going.
    for (size_t i = 1; i < line.size(); ++i) {
        Vec2i p = line[i];
        if (!dung.inBounds(p.x, p.y)) { stopIdx = i - 1; break; }

        // Corner blocking: don't allow shots to "thread" a diagonal gap when both orthogonal
        // neighbors are solid projectile blockers.
        if (projectileCornerBlocked(dung, line[i - 1], p)) {
            hitWall = true;
            hitWallTile = TileType::Wall;
            stopIdx = i;
            break;
        }

        // Solid terrain blocks projectiles.
        if (dung.blocksProjectiles(p.x, p.y)) {
            hitWall = true;
            hitWallTile = dung.at(p.x, p.y).type;
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
                ss << "YOU MISS " << kindNameForMsg(*e, player().effects.hallucinationTurns > 0) << ".";
                pushMsg(ss.str(), MessageKind::Combat, true);
            } else if (e->kind == EntityKind::Player) {
                std::ostringstream ss;
                ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " MISSES YOU.";
                pushMsg(ss.str(), MessageKind::Combat, false);
            }
            continue;
        }

        // Hit: apply damage and stop.
        hitAny = true;
        hit = e;
        stopIdx = i;

        if (fromPlayer && hit->kind == EntityKind::Shopkeeper && !hit->alerted) {
            triggerShopTheftAlarm(hit->pos, attacker.pos);
        }

        if (projKind == ProjectileKind::Fireball) {
            // Fireball damage is applied by the AoE explosion at impact.
            break;
        }

        if (projKind == ProjectileKind::Spark) {
            sparkCrit = hc.crit;
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

        const int hpBefore = hit->hp;
        hit->hp -= dmg;

        std::ostringstream ss;
        if (fromPlayer) {
            ss << "YOU " << (hc.crit ? "CRIT " : "") << "HIT " << kindNameForMsg(*hit, player().effects.hallucinationTurns > 0);
            if (dmg > 0) ss << " FOR " << dmg;
            else ss << " BUT DO NO DAMAGE";
            ss << ".";
        } else if (hit->kind == EntityKind::Player) {
            ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " " << (hc.crit ? "CRITS" : "HITS") << " YOU";
            if (dmg > 0) ss << " FOR " << dmg;
            else ss << " BUT DOES NO DAMAGE";
            ss << ".";
        } else {
            ss << kindNameForMsg(attacker, player().effects.hallucinationTurns > 0) << " HITS " << kindNameForMsg(*hit, player().effects.hallucinationTurns > 0) << ".";
        }
        pushMsg(ss.str(), MessageKind::Combat, fromPlayer);

        // Special projectile: TORCH can ignite targets on a hit.
        if (projKind == ProjectileKind::Torch && hit) {
            const int beforeBurn = hit->effects.burnTurns;
            const int igniteTurns = clampi(3 + rng.range(0, 2), 2, 10); // 3..5
            if (beforeBurn < igniteTurns) hit->effects.burnTurns = igniteTurns;

            if (beforeBurn == 0 && hit->effects.burnTurns > 0) {
                if (hit->kind == EntityKind::Player) {
                    pushMsg("YOU CATCH FIRE!", MessageKind::Warning, false);
                } else if (dung.inBounds(hit->pos.x, hit->pos.y) && dung.at(hit->pos.x, hit->pos.y).visible) {
                    std::ostringstream bs;
                    bs << kindNameForMsg(*hit, player().effects.hallucinationTurns > 0) << " CATCHES FIRE!";
                    pushMsg(bs.str(), MessageKind::Info, fromPlayer);
                }
            }
        }

        // Procedural monster affix on-hit procs (ranged).
        if (attacker.kind != EntityKind::Player && hit->hp > 0 && attacker.procAffixMask != 0u) {
            const int tier = procRankTier(attacker.procRank);

            auto canSeeProc = [&]() -> bool {
                if (hit->kind == EntityKind::Player) return true;
                if (!dung.inBounds(hit->pos.x, hit->pos.y)) return false;
                return dung.at(hit->pos.x, hit->pos.y).visible;
            };

            // Venomous: poison on hit (reduced vs melee).
            if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Venomous) && hit->hp > 0) {
                float chance = (0.20f + 0.05f * static_cast<float>(tier)) * 0.75f;
                chance = std::clamp(chance, 0.0f, 0.50f);
                if (rng.chance(chance)) {
                    const int before = hit->effects.poisonTurns;
                    const int turns = clampi(3 + tier * 2 + rng.range(0, 3), 3, 14);
                    hit->effects.poisonTurns = std::max(hit->effects.poisonTurns, turns);
                    if (before == 0 && hit->effects.poisonTurns > 0) {
                        if (hit->kind == EntityKind::Player) {
                            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                        } else if (hit->friendly && canSeeProc()) {
                            std::ostringstream ps;
                            ps << "YOUR " << kindNameForMsg(*hit, player().effects.hallucinationTurns > 0) << " IS POISONED!";
                            pushMsg(ps.str(), MessageKind::Info, true);
                        }
                        if (canSeeProc()) pushFxParticle(FXParticlePreset::Poison, hit->pos, 16, 0.35f);
                    }
                }
            }

            // Flaming: burn on hit.
            if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Flaming) && hit->hp > 0) {
                float chance = (0.16f + 0.05f * static_cast<float>(tier)) * 0.85f;
                chance = std::clamp(chance, 0.0f, 0.55f);
                if (rng.chance(chance)) {
                    const int before = hit->effects.burnTurns;
                    const int turns = clampi(2 + tier + rng.range(0, 2), 2, 10);
                    hit->effects.burnTurns = std::max(hit->effects.burnTurns, turns);
                    if (before == 0 && hit->effects.burnTurns > 0) {
                        if (hit->kind == EntityKind::Player) {
                            pushMsg("YOU CATCH FIRE!", MessageKind::Warning, false);
                        } else if (hit->friendly && canSeeProc()) {
                            std::ostringstream bs;
                            bs << "YOUR " << kindNameForMsg(*hit, player().effects.hallucinationTurns > 0) << " CATCHES FIRE!";
                            pushMsg(bs.str(), MessageKind::Info, true);
                        }
                    }
                }
            }

            // Webbing: ensnare on hit (rare for ranged).
            if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Webbing) && hit->hp > 0) {
                float chance = (0.10f + 0.03f * static_cast<float>(tier)) * 0.55f;
                chance = std::clamp(chance, 0.0f, 0.35f);
                if (rng.chance(chance)) {
                    const int before = hit->effects.webTurns;
                    const int turns = clampi(2 + tier + rng.range(0, 2), 2, 8);
                    hit->effects.webTurns = std::max(hit->effects.webTurns, turns);
                    if (before == 0 && hit->effects.webTurns > 0) {
                        if (hit->kind == EntityKind::Player) {
                            pushMsg("YOU ARE ENSNARED!", MessageKind::Warning, false);
                        } else if (hit->friendly && canSeeProc()) {
                            std::ostringstream ws;
                            ws << "YOUR " << kindNameForMsg(*hit, player().effects.hallucinationTurns > 0) << " IS ENSNARED!";
                            pushMsg(ws.str(), MessageKind::Info, true);
                        }
                    }
                }
            }

            // Vampiric: life drain (reduced vs melee).
            if (procHasAffix(attacker.procAffixMask, ProcMonsterAffix::Vampiric) && dmg > 0 && attacker.hp > 0) {
                float chance = (0.30f + 0.05f * static_cast<float>(tier)) * 0.60f;
                chance = std::clamp(chance, 0.0f, 0.55f);
                if (rng.chance(chance)) {
                    const int beforeHp = attacker.hp;
                    const int heal = clampi(1 + dmg / 6 + tier, 1, 8);
                    attacker.hp = std::min(attacker.hpMax, attacker.hp + heal);
                    if (attacker.hp > beforeHp) {
                        if (hit->kind == EntityKind::Player) {
                            pushMsg("YOUR LIFE IS DRAINED!", MessageKind::Warning, false);
                        } else if (hit->friendly && canSeeProc()) {
                            std::ostringstream vs;
                            vs << "YOUR " << kindNameForMsg(*hit, player().effects.hallucinationTurns > 0) << " IS DRAINED!";
                            pushMsg(vs.str(), MessageKind::Info, true);
                        }
                        if (dung.inBounds(attacker.pos.x, attacker.pos.y) && dung.at(attacker.pos.x, attacker.pos.y).visible) {
                            pushFxParticle(FXParticlePreset::Heal, attacker.pos, 10, 0.30f);
                        }
                    }
                }
            }
        }

        if (hpBefore > 0 && hit->hp <= 0) {
            handleRangedDeath(*hit);
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
                if (fromPlayer) {
                    pushMsg(std::string("THE FIREBALL HITS ") + projectileBlockerNoun(hitWallTile) + ".", MessageKind::Warning, true);
                }
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

        // Lingering flames: leave a temporary fire field on walkable tiles in the blast.
        // This creates tactical area denial (and doubles as a light source in darkness mode).
        {
            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (fireField_.size() != expect) fireField_.assign(expect, 0u);

            const int baseStrength = clampi(6 + depth_ / 3, 6, 12);
            int ignitedVisible = 0;

            for (const Vec2i& bt : blastTiles) {
                if (!dung.inBounds(bt.x, bt.y)) continue;
                if (!dung.isWalkable(bt.x, bt.y)) continue;

                const int dist = std::max(std::abs(bt.x - center.x), std::abs(bt.y - center.y));
                const int s = baseStrength - dist * 2;
                if (s <= 0) continue;

                const size_t i = static_cast<size_t>(bt.y * dung.width + bt.x);
                if (i >= fireField_.size()) continue;

                const uint8_t prev = fireField_[i];
                const uint8_t next = static_cast<uint8_t>(clampi(s, 0, 255));
                if (next > prev) {
                    fireField_[i] = next;
                    if (prev == 0u && dung.at(bt.x, bt.y).visible) ++ignitedVisible;
                }
            }

            if (ignitedVisible > 0 && fromPlayer) {
                pushMsg("FLAMES LINGER ON THE GROUND.", MessageKind::System, true);
            }
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
                triggerShopTheftAlarm(e->pos, attacker.pos);
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
                ss << kindNameForMsg(*e, player().effects.hallucinationTurns > 0) << " IS HIT";
                if (dmg > 0) ss << " FOR " << dmg;
                else ss << " BUT TAKES NO DAMAGE";
                ss << ".";
                pushMsg(ss.str(), MessageKind::Combat, fromPlayer);
            }

            if (e->hp <= 0) {
                if (e->kind == EntityKind::Player) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) {
                        endCause_ = fromPlayer ? "KILLED BY YOUR OWN FIREBALL" : (std::string("KILLED BY ") + kindNameForMsg(attacker, player().effects.hallucinationTurns > 0));
                    }
                    gameOver = true;
                    break;
                } else {
                    const bool vis = dung.inBounds(e->pos.x, e->pos.y) && dung.at(e->pos.x, e->pos.y).visible;
                    if (fromPlayer || vis) {
                        std::ostringstream ds;
                        if (e->friendly) ds << "YOUR " << kindNameForMsg(*e, player().effects.hallucinationTurns > 0) << " DIES.";
                        else ds << kindNameForMsg(*e, player().effects.hallucinationTurns > 0) << " DIES.";
                        pushMsg(ds.str(), MessageKind::Combat, fromPlayer);
                    }
                    if ((attacker.kind == EntityKind::Player || attacker.friendly) && !e->friendly) {
                        ++killCount;
						if (attacker.kind == EntityKind::Player && directKillCount_ < 0xFFFFFFFFu) {
							++directKillCount_;
						}

                        const size_t kidx = static_cast<size_t>(e->kind);
                        if (kidx < codexKills_.size()) {
                            codexSeen_[kidx] = 1;
                            if (codexKills_[kidx] < 65535) ++codexKills_[kidx];
                        }

                        const int xpAward = xpFor(*e);
                        grantXp(xpAward);
                        awardBountyProgress(e->kind, true);

                        if (attacker.friendly) {
                            const int petXp = clampi((xpAward + 1) / 2, 1, 120);
                            const int bondGain = clampi(1 + xpAward / 20, 1, 5);
                            const bool show = dung.inBounds(attacker.pos.x, attacker.pos.y) && dung.at(attacker.pos.x, attacker.pos.y).visible;
                            awardCapturedPetProgress(attacker, petXp, bondGain, show);
                        } else if (attacker.kind == EntityKind::Player) {
                            const int share = std::max(1, xpAward / 3);
                            for (auto& ally : ents) {
                                if (ally.hp <= 0) continue;
                                if (!ally.friendly) continue;
                                if (ally.id == player().id) continue;
                                const bool show = dung.inBounds(ally.pos.x, ally.pos.y) && dung.at(ally.pos.x, ally.pos.y).visible;
                                awardCapturedPetProgress(ally, share, /*bondGain=*/0, show);
                            }
                        }
                    }
                }
            }
        }

        inputLock = true;
        return;
    }

    if (!hitAny) {
        if (hitWall) {
            if (fromPlayer) {
                if (projKind == ProjectileKind::Torch) {
                    pushMsg(std::string("THE TORCH HITS ") + projectileBlockerNoun(hitWallTile) + ".", MessageKind::Warning, true);
                } else {
                    pushMsg(std::string("THE SHOT HITS ") + projectileBlockerNoun(hitWallTile) + ".", MessageKind::Warning, true);
                }
            }
        } else {
            if (fromPlayer) {
                if (projKind == ProjectileKind::Torch) pushMsg("YOU THROW A TORCH.", MessageKind::Combat, true);
                else pushMsg("YOU FIRE.", MessageKind::Combat, true);
            }
        }
    }

    // Conductive spark arcs: electric bolts can jump to nearby targets when they strike
    // conductive terrain (metal/crystal substrates, water chasms) or land a critical hit.
    //
    // This is intentionally deterministic aside from the normal RNG rolls: the "best"
    // arc target is chosen by a simple score (conductive path length + proximity).
    if (projKind == ProjectileKind::Spark && !gameOver) {
        const bool impacted = hitAny || hitWall;
        if (impacted) {
            // Ensure deterministic terrain materials are cached for this floor.
            dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());

            Vec2i impactTile = line[stopIdx];
            Vec2i arcOrigin = impactTile;
            if (hitWall && stopIdx > 0) {
                // Use the last reachable tile as the arc origin; the wall tile remains
                // the conductivity reference.
                arcOrigin = line[stopIdx - 1];
            }
            if (hitAny && hit) {
                impactTile = hit->pos;
                arcOrigin = hit->pos;
            }

            auto isConductiveMaterial = [&](TerrainMaterial m) -> bool {
                return (m == TerrainMaterial::Metal || m == TerrainMaterial::Crystal);
            };
            auto conductiveAt = [&](const Vec2i& p) -> bool {
                if (!dung.inBounds(p.x, p.y)) return false;
                const TileType tt = dung.at(p.x, p.y).type;
                if (tt == TileType::Chasm) return true; // lakes/void act as strong conductors
                const TerrainMaterial m = dung.materialAtCached(p.x, p.y);
                return isConductiveMaterial(m);
            };
            auto conductionTierAt = [&](const Vec2i& p) -> int {
                if (conductiveAt(p)) return 2;
                // Adjacent conductive features can still induce a weaker arc.
                static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                for (const auto& d : dirs4) {
                    Vec2i q{p.x + d[0], p.y + d[1]};
                    if (conductiveAt(q)) return 1;
                }
                return 0;
            };

            const int tier = conductionTierAt(impactTile);

            float arcChance = 0.0f;
            if (wandPowered) arcChance += 0.18f;
            arcChance += 0.08f * static_cast<float>(tier);
            if (sparkCrit) arcChance += 0.12f;
            arcChance = std::clamp(arcChance, 0.0f, 0.55f);

            if (arcChance > 0.0f && rng.chance(arcChance)) {
                const int maxJumps = std::min(3, 1 + (tier >= 2 ? 1 : 0) + (wandPowered ? 1 : 0));
                int radius = 4 + (tier >= 2 ? 2 : 0) + (wandPowered ? 1 : 0);
                radius = std::min(radius, 8);

                if (fromPlayer && tier > 0 && dung.inBounds(impactTile.x, impactTile.y) && dung.at(impactTile.x, impactTile.y).visible) {
                    pushMsg("THE SPARKS DANCE ACROSS THE CONDUCTIVE SURFACE!", MessageKind::Info, true);
                }

                std::vector<int> alreadyHit;
                alreadyHit.reserve(static_cast<size_t>(maxJumps + 2));
                alreadyHit.push_back(attacker.id);
                if (hitAny && hit) alreadyHit.push_back(hit->id);

                auto already = [&](int id) -> bool {
                    for (int v : alreadyHit) if (v == id) return true;
                    return false;
                };

                auto isEligible = [&](const Entity& cand) -> bool {
                    if (cand.hp <= 0) return false;
                    if (already(cand.id)) return false;
                    if (fromPlayer) {
                        // Player-side arcs only seek hostiles (avoid zapping allies/pets).
                        if (cand.kind == EntityKind::Player) return false;
                        if (cand.friendly) return false;
                    } else {
                        // Monster-side arcs only seek the player side.
                        if (!(cand.kind == EntityKind::Player || cand.friendly)) return false;
                    }
                    return true;
                };

                auto cheb = [&](const Vec2i& a, const Vec2i& b) -> int {
                    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
                };

                auto hasClearArcLine = [&](const Vec2i& a, const Vec2i& b, std::vector<Vec2i>& outLine) -> bool {
                    outLine = bresenhamLine(a, b);
                    if (outLine.size() <= 1) return false;
                    for (size_t ii = 1; ii < outLine.size(); ++ii) {
                        const Vec2i p = outLine[ii];
                        if (!dung.inBounds(p.x, p.y)) return false;

                        if (projectileCornerBlocked(dung, outLine[ii - 1], p)) return false;

                        if (dung.blocksProjectiles(p.x, p.y) && p != b) return false;

                        // Don't allow arcs to 'skip' over a nearer body.
                        if (p != b) {
                            Entity* block = entityAtMut(p.x, p.y);
                            if (block && block->hp > 0) return false;
                        }
                    }
                    return true;
                };

                const DiceExpr baseDice = rangedDiceForProjectile(ProjectileKind::Spark, wandPowered);
                Vec2i src = arcOrigin;

                for (int jump = 0; jump < maxJumps; ++jump) {
                    Entity* best = nullptr;
                    int bestScore = -999999;
                    int bestDist = 999999;
                    std::vector<Vec2i> bestLine;

                    for (auto& cand : ents) {
                        if (!isEligible(cand)) continue;
                        const int dist = cheb(src, cand.pos);
                        if (dist <= 0 || dist > radius) continue;

                        std::vector<Vec2i> l;
                        if (!hasClearArcLine(src, cand.pos, l)) continue;

                        int conductiveCount = 0;
                        for (const Vec2i& pp : l) {
                            if (conductiveAt(pp)) ++conductiveCount;
                        }

                        int score = 0;
                        score += conductiveCount * 10;
                        score -= dist * 8;
                        if (conductiveAt(cand.pos)) score += 12;

                        if (score > bestScore || (score == bestScore && (dist < bestDist || (dist == bestDist && (!best || cand.id < best->id))))) {
                            bestScore = score;
                            bestDist = dist;
                            best = &cand;
                            bestLine = std::move(l);
                        }
                    }

                    if (!best) break;

                    if (fromPlayer && best->kind == EntityKind::Shopkeeper && !best->alerted) {
                        triggerShopTheftAlarm(best->pos, attacker.pos);
                    }

                    int dmg = rollDice(rng, baseDice);
                    const int arcBonus = (dmgBonus + statDamageBonusFromAtk(attacker.baseAtk)) / 2;
                    dmg += arcBonus;
                    const int scale = (jump == 0) ? 7 : (jump == 1 ? 5 : 4);
                    dmg = (dmg * scale) / 10;
                    const int dr = damageReduction(*this, *best);
                    const int absorbed = (dr > 0) ? rng.range(0, dr) : 0;
                    dmg = std::max(0, dmg - absorbed);

                    const int beforeHp = best->hp;
                    best->hp -= dmg;

                    const bool show = fromPlayer || best->kind == EntityKind::Player ||
                        (dung.inBounds(best->pos.x, best->pos.y) && dung.at(best->pos.x, best->pos.y).visible);
                    if (show) {
                        std::ostringstream ss;
                        if (fromPlayer) {
                            ss << "LIGHTNING ARCS TO " << kindNameForMsg(*best, player().effects.hallucinationTurns > 0);
                        } else if (best->kind == EntityKind::Player) {
                            ss << "LIGHTNING ARCS TO YOU";
                        } else if (best->friendly) {
                            ss << "LIGHTNING ARCS TO YOUR " << kindNameForMsg(*best, player().effects.hallucinationTurns > 0);
                        } else {
                            ss << "LIGHTNING ARCS TO " << kindNameForMsg(*best, player().effects.hallucinationTurns > 0);
                        }
                        if (dmg > 0) ss << " FOR " << dmg;
                        else ss << " BUT DOES NO DAMAGE";
                        ss << ".";
                        pushMsg(ss.str(), MessageKind::Combat, fromPlayer);
                    }

                    FXProjectile arcFx;
                    arcFx.kind = ProjectileKind::Spark;
                    arcFx.path = std::move(bestLine);
                    arcFx.pathIndex = (arcFx.path.size() > 1) ? 1 : 0;
                    arcFx.stepTimer = 0.0f;
                    arcFx.stepTime = 0.015f;
                    fx.push_back(std::move(arcFx));

                    alreadyHit.push_back(best->id);

                    // Chance to briefly confuse/stun the victim.
                    if (dmg > 0) {
                        float stunChance = 0.12f + 0.05f * static_cast<float>(tier);
                        if (wandPowered) stunChance += 0.05f;
                        stunChance = std::clamp(stunChance, 0.0f, 0.35f);
                        if (rng.chance(stunChance)) {
                            const int turns = 1 + rng.range(0, 1);
                            best->effects.confusionTurns = std::max(best->effects.confusionTurns, turns);
                            if (best->kind == EntityKind::Player) {
                                pushMsg("YOU ARE STUNNED BY THE SHOCK!", MessageKind::Warning, false);
                            } else if (dung.inBounds(best->pos.x, best->pos.y) && dung.at(best->pos.x, best->pos.y).visible) {
                                std::ostringstream cs;
                                cs << kindNameForMsg(*best, player().effects.hallucinationTurns > 0) << " REELS FROM THE SHOCK!";
                                pushMsg(cs.str(), MessageKind::Info, fromPlayer);
                            }
                        }
                    }

                    if (beforeHp > 0 && best->hp <= 0) {
                        handleRangedDeath(*best);
                        if (gameOver) break;
                    }

                    src = best->pos;
                }
            }
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

    // Thrown torches: always land on the ground (unless they fall into a chasm) and
    // leave a small patch of fire on the landing tile (no spread; just a brief hazard + light).
    if (projKind == ProjectileKind::Torch) {
        Vec2i land = line[stopIdx];
        if (hitWall && stopIdx > 0) {
            land = line[stopIdx - 1];
        }

        if (dung.inBounds(land.x, land.y) && !dung.isOpaque(land.x, land.y)) {
            const TileType tt = dung.at(land.x, land.y).type;

            if (tt == TileType::Chasm) {
                if (fromPlayer && dung.at(land.x, land.y).visible) {
                    pushMsg("YOUR TORCH PLUMMETS INTO THE CHASM.", MessageKind::Warning, true);
                }
            } else {
                // Preserve the thrown torch's remaining fuel (charges) when possible.
                Item drop;
                if (projectileTemplate && projectileTemplate->kind == ItemKind::TorchLit) {
                    drop = *projectileTemplate;
                } else {
                    drop.id = nextItemId++;
                    drop.kind = ItemKind::TorchLit;
                    drop.count = 1;
                    drop.enchant = 0;
                    drop.charges = 90 + rng.range(0, 60);
                    drop.spriteSeed = rng.nextU32();
                }
                drop.count = 1;

                // Only drop torches that are still burning.
                if (drop.charges > 0) {
                    dropGroundItemItem(land, drop);

                    // A small ember patch (won't spread unless boosted by other fire).
                    const size_t expect = static_cast<size_t>(dung.width * dung.height);
                    if (fireField_.size() != expect) fireField_.assign(expect, 0u);

                    if (dung.isWalkable(land.x, land.y)) {
                        const size_t i = static_cast<size_t>(land.y * dung.width + land.x);
                        if (i < fireField_.size()) {
                            const uint8_t prev = fireField_[i];
                            const uint8_t next = static_cast<uint8_t>(std::max<int>(prev, 5));
                            fireField_[i] = next;
                        }
                    }
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