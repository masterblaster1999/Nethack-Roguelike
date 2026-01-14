#include "game_internal.hpp"

#include "combat_rules.hpp"
#include "hallucination.hpp"

#include <cmath>
#include <iomanip>

namespace {

struct ToHitOdds {
    float pHit = 0.0f;        // includes crit
    float pHitNonCrit = 0.0f; // excludes crit
    float pCrit = 0.0f;       // natural 20
};

// Mirrors the d20 rules used in combat.cpp:
//   - natural 1: always miss
//   - natural 20: always hit + crit
//   - otherwise hit if (natural + attackBonus) >= targetAC
static ToHitOdds d20ToHitOdds(int attackBonus, int targetAC) {
    ToHitOdds o;
    o.pCrit = 1.0f / 20.0f;

    // Count hit outcomes for natural 2..19.
    const int threshold = targetAC - attackBonus; // natural >= threshold
    const int minRoll = std::max(2, threshold);
    int hitCount = 0;
    if (minRoll <= 19) {
        // Inclusive range [minRoll, 19]
        hitCount = 19 - minRoll + 1;
    }

    o.pHitNonCrit = static_cast<float>(hitCount) / 20.0f;
    o.pHit = o.pHitNonCrit + o.pCrit;
    return o;
}

static float expectedDice(DiceExpr d) {
    d.count = std::max(0, d.count);
    d.sides = std::max(0, d.sides);
    // Expected value of a fair 1..sides roll is (sides+1)/2.
    return static_cast<float>(d.count) * (static_cast<float>(d.sides) + 1.0f) * 0.5f + static_cast<float>(d.bonus);
}

// Expected value of max(0, raw - U(0..dr)), approximated with a continuous uniform.
static float expectedAfterAbsorb(float raw, int dr) {
    if (raw <= 0.0f) return 0.0f;
    if (dr <= 0) return raw;
    const float fdr = static_cast<float>(dr);
    if (raw >= fdr) {
        return raw - 0.5f * fdr;
    }
    // raw in (0, dr): E = raw^2 / (2*dr)
    return (raw * raw) / (2.0f * fdr);
}

static int targetACForLook(const Game& g, const Entity& e) {
    // Matches combat.cpp targetAC(): monsters use baseDef; player uses playerDefense().
    const int def = (e.kind == EntityKind::Player) ? g.playerDefense() : e.baseDef;
    return 10 + def;
}

static int damageReductionForLook(const Game& g, const Entity& e) {
    // Matches combat.cpp damageReduction().
    if (e.kind != EntityKind::Player) {
        int dr = std::max(0, e.baseDef);

        if (monsterCanEquipArmor(e.kind) && e.gearArmor.id != 0 && isArmor(e.gearArmor.kind)) {
            const Item& a = e.gearArmor;
            const int b = (a.buc < 0) ? -1 : (a.buc > 0 ? 1 : 0);
            dr += itemDef(a.kind).defense + a.enchant + b;
        }

        return std::max(0, dr);
    }

    const int evasion = g.playerEvasion();
    return std::max(0, g.playerDefense() - evasion);
}

struct DuelForecast {
    float youTtk = INFINITY;   // expected turns for player to kill target
    float youHit = 0.0f;
    float youDmg = 0.0f;       // expected dmg per swing (includes misses)

    float foeTtd = INFINITY;   // expected turns for foe to kill player
    float foeHit = 0.0f;
    float foeDmg = 0.0f;

    bool ambush = false;
    bool backstab = false;
};

static DuelForecast computeDuelForecast(const Game& g, const Entity& foe) {
    DuelForecast out;

    const Entity& p = g.player();
    if (p.hp <= 0 || foe.hp <= 0) return out;

    // ------------------------------------------------------------
    // Player -> foe
    // ------------------------------------------------------------
    int atkBonus = g.playerAttack();

    // Ambush/backstab bonuses (mirrors combat.cpp).
    if (foe.kind != EntityKind::Player && !foe.alerted) {
        out.ambush = true;
        const int agi = g.playerAgility();
        atkBonus += 2 + std::min(3, agi / 4);

        const bool invis = (p.effects.invisTurns > 0);
        const bool sneak = g.isSneaking();
        out.backstab = (invis || sneak);
    }

    const int acFoe = targetACForLook(g, foe);
    const ToHitOdds oddsP = d20ToHitOdds(atkBonus, acFoe);
    out.youHit = oddsP.pHit;

    const Item* w = g.equippedMelee();
    DiceExpr baseDice{1, 2, 0};
    if (w) baseDice = meleeDiceForWeapon(w->kind);

    int atkStatForBonus = p.baseAtk + g.playerMight();
    int bonus = statDamageBonusFromAtk(atkStatForBonus);
    if (w) bonus += w->enchant;

    float raw = expectedDice(baseDice) + static_cast<float>(bonus);
    if (out.ambush) {
        raw += static_cast<float>(1 + std::min(3, g.playerAgility() / 4));
        if (out.backstab) {
            raw += expectedDice(baseDice);
        }
    }

    const int drFoe = damageReductionForLook(g, foe);
    const float nonCrit = expectedAfterAbsorb(raw, drFoe);
    const float crit = expectedAfterAbsorb(raw + expectedDice(baseDice), drFoe / 2);

    out.youDmg = oddsP.pHitNonCrit * nonCrit + oddsP.pCrit * crit;
    if (out.youDmg > 0.001f) {
        out.youTtk = static_cast<float>(foe.hp) / out.youDmg;
    }

    // ------------------------------------------------------------
    // Foe -> player
    // ------------------------------------------------------------
    int foeAtk = foe.baseAtk;
    DiceExpr foeDice = meleeDiceForMonster(foe.kind);

    int foeBonus = statDamageBonusFromAtk(foe.baseAtk);
    if (monsterCanEquipWeapons(foe.kind) && foe.gearMelee.id != 0 && isMeleeWeapon(foe.gearMelee.kind)) {
        foeDice = meleeDiceForWeapon(foe.gearMelee.kind);
        const int b = (foe.gearMelee.buc < 0) ? -1 : (foe.gearMelee.buc > 0 ? 1 : 0);
        foeAtk += foe.gearMelee.enchant + b;
        foeBonus += foe.gearMelee.enchant + b;
    }

    const int acP = targetACForLook(g, p);
    const ToHitOdds oddsF = d20ToHitOdds(foeAtk, acP);
    out.foeHit = oddsF.pHit;

    const float foeRaw = expectedDice(foeDice) + static_cast<float>(foeBonus);
    const int drP = damageReductionForLook(g, p);
    const float foeNonCrit = expectedAfterAbsorb(foeRaw, drP);
    const float foeCrit = expectedAfterAbsorb(foeRaw + expectedDice(foeDice), drP / 2);

    out.foeDmg = oddsF.pHitNonCrit * foeNonCrit + oddsF.pCrit * foeCrit;
    if (out.foeDmg > 0.001f) {
        out.foeTtd = static_cast<float>(p.hp) / out.foeDmg;
    }

    return out;
}

static std::string duelForecastLabel(const DuelForecast& f) {
    // Keep this short; it lives in the LOOK bottom-line.
    auto fmtT = [](float t) -> std::string {
        if (!std::isfinite(t) || t > 999.0f) return "INF";
        std::ostringstream ss;
        ss << std::fixed << std::setprecision((t < 10.0f) ? 1 : 0) << t;
        return ss.str();
    };

    std::ostringstream ss;
    if (f.ambush) {
        ss << (f.backstab ? "SNEAK " : "AMBUSH ");
    }

    ss << "DUEL: KILL~" << fmtT(f.youTtk) << " DIE~" << fmtT(f.foeTtd);
    return ss.str();
}

} // namespace

void Game::beginLook() {
    // Close other overlays
    invOpen = false;
    closeChestOverlay();
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    codexOpen = false;
    msgScroll = 0;

    // UI-only helper: acoustic preview is scoped to LOOK mode.
    soundPreviewOpen = false;
    soundPreviewDist.clear();

    looking = true;
    lookPos = player().pos;
}

void Game::endLook() {
    looking = false;
    soundPreviewOpen = false;
    soundPreviewDist.clear();
}

void Game::beginLookAt(Vec2i p) {
    beginLook();
    setLookCursor(p);
}

void Game::setLookCursor(Vec2i p) {
    if (!looking) return;
    p.x = clampi(p.x, 0, dung.width - 1);
    p.y = clampi(p.y, 0, dung.height - 1);
    lookPos = p;

    // Keep acoustic preview locked to the cursor while active.
    if (soundPreviewOpen) {
        soundPreviewSrc = lookPos;
        soundPreviewDist = dung.computeSoundMap(soundPreviewSrc.x, soundPreviewSrc.y, soundPreviewVol);
    }
}

void Game::setTargetCursor(Vec2i p) {
    if (!targeting) return;
    p.x = clampi(p.x, 0, dung.width - 1);
    p.y = clampi(p.y, 0, dung.height - 1);
    targetPos = p;
    recomputeTargetLine();
}

void Game::moveLookCursor(int dx, int dy) {
    if (!looking) return;
    Vec2i p = lookPos;
    p.x = clampi(p.x + dx, 0, dung.width - 1);
    p.y = clampi(p.y + dy, 0, dung.height - 1);
    lookPos = p;

    // Keep acoustic preview locked to the cursor while active.
    if (soundPreviewOpen) {
        soundPreviewSrc = lookPos;
        soundPreviewDist = dung.computeSoundMap(soundPreviewSrc.x, soundPreviewSrc.y, soundPreviewVol);
    }

}

void Game::toggleSoundPreview() {
    // This is a UI-only planning helper; it never consumes a turn.
    if (!looking) {
        beginLook();
    }

    soundPreviewOpen = !soundPreviewOpen;
    if (!soundPreviewOpen) {
        soundPreviewDist.clear();
        return;
    }

    // Default to the noise level of a single careful step (sneak vs normal).
    soundPreviewVol = isSneaking() ? 8 : 12;
    soundPreviewSrc = lookPos;
    soundPreviewDist = dung.computeSoundMap(soundPreviewSrc.x, soundPreviewSrc.y, soundPreviewVol);
}

void Game::adjustSoundPreviewVolume(int delta) {
    if (!soundPreviewOpen) return;
    // Common in-game noises fall roughly in the 1..18 range; keep a little headroom.
    soundPreviewVol = clampi(soundPreviewVol + delta, 1, 30);
    soundPreviewSrc = lookPos;
    soundPreviewDist = dung.computeSoundMap(soundPreviewSrc.x, soundPreviewSrc.y, soundPreviewVol);
}

std::string Game::describeAt(Vec2i p) const {
    if (!dung.inBounds(p.x, p.y)) return "OUT OF BOUNDS";

    const Tile& t = dung.at(p.x, p.y);
    if (!t.explored) {
        return "UNKNOWN";
    }

    std::ostringstream ss;

    const bool hallu = isHallucinating(*this);

    // Base tile description
    switch (t.type) {
        case TileType::Wall: ss << "WALL"; break;
        case TileType::DoorSecret: ss << "WALL"; break; // don't spoil undiscovered secrets
        case TileType::Pillar: ss << "PILLAR"; break;
        case TileType::Boulder: ss << "BOULDER"; break;
        case TileType::Chasm: ss << "CHASM"; break;
        case TileType::Floor: ss << "FLOOR"; break;
        case TileType::Fountain: ss << "FOUNTAIN"; break;
        case TileType::Altar: ss << "ALTAR"; break;
        case TileType::StairsUp: ss << "STAIRS UP"; break;
        case TileType::StairsDown: ss << "STAIRS DOWN"; break;
        case TileType::DoorClosed: ss << "DOOR (CLOSED)"; break;
        case TileType::DoorLocked: ss << "DOOR (LOCKED)"; break;
        case TileType::DoorOpen: ss << "DOOR (OPEN)"; break;
        default: ss << "TILE"; break;
    }

    // Branch-aware stair destination hints.
    // This keeps look/inspect readable now that multiple branches can share the same numeric depth.
    if (t.type == TileType::StairsUp) {
        if (atCamp()) {
            ss << " | EXIT";
        } else {
            // First-pass branching: stairs up from depth 1 returns to camp.
            if (depth_ <= 1) ss << " | TO CAMP";
            else ss << " | TO DEPTH " << (depth_ - 1);
        }
    } else if (t.type == TileType::StairsDown) {
        if (atCamp()) {
            ss << " | TO DUNGEON (DEPTH 1)";
        } else {
            if (depth_ >= DUNGEON_MAX_DEPTH) {
                ss << " | BOTTOM";
            } else {
                ss << " | TO DEPTH " << (depth_ + 1);
            }
        }
    }

    // Trap (can be remembered once discovered)
    for (const auto& tr : trapsCur) {
        if (!tr.discovered) continue;
        if (tr.pos.x != p.x || tr.pos.y != p.y) continue;
        ss << " | TRAP: ";
        switch (tr.kind) {
            case TrapKind::Spike: ss << "SPIKE"; break;
            case TrapKind::PoisonDart: ss << "POISON DART"; break;
            case TrapKind::Teleport: ss << "TELEPORT"; break;
            case TrapKind::Alarm: ss << "ALARM"; break;
            case TrapKind::Web: ss << "WEB"; break;
            case TrapKind::ConfusionGas: ss << "CONFUSION GAS"; break;
            case TrapKind::RollingBoulder: ss << "ROLLING BOULDER"; break;
            case TrapKind::TrapDoor: ss << "TRAP DOOR"; break;
            case TrapKind::LetheMist: ss << "LETHE MIST"; break;
            case TrapKind::PoisonGas: ss << "POISON GAS"; break;
        }
        break;
    }

    // Environmental fields (only if currently visible).
    if (t.visible) {
        if (confusionGasAt(p.x, p.y) > 0u) ss << " | CONFUSION GAS";
        if (poisonGasAt(p.x, p.y) > 0u) ss << " | POISON GAS";
        if (fireAt(p.x, p.y) > 0u) ss << " | FIRE";
    }

    // Player map marker / note (persistent on this floor).
    if (const MapMarker* mm = markerAt(p)) {
        ss << " | MARK: " << markerKindName(mm->kind) << " \"" << mm->label << "\"";
    }

    // Floor engraving / graffiti (persistent on this floor).
    if (const Engraving* eg = engravingAt(p)) {
        std::string sk;
        if (engravingIsSigil(*eg, &sk)) {
            ss << " | SIGIL: \"" << eg->text << "\"";
            if (eg->strength != 255) {
                const int uses = static_cast<int>(eg->strength);
                ss << " (" << uses << " USE" << (uses == 1 ? "" : "S") << " LEFT)";
            }
        } else {
            ss << " | ENGRAVING: \"" << eg->text << "\"";
            if (eg->isWard) ss << " (WARD)";
        }
    }

    // Entities/items: only if currently visible.
    if (t.visible) {
        if (const Entity* e = entityAt(p.x, p.y)) {
            if (e->id == playerId_) {
                ss << " | YOU";
            } else {
                const EntityKind showKind = hallucinatedEntityKind(*this, *e);
                std::string label = kindName(showKind);

    // Procedural monster variants: surface rank + affixes (unless hallucinating).
    if (!hallu && e && e->id != player().id) {
        if (e->procRank != ProcMonsterRank::Normal || e->procAffixMask != 0u) {
            label = kindName(e->kind);
            const int tier = procRankTier(e->procRank);
            if (tier > 0) {
                label = std::string(procMonsterRankName(e->procRank)) + " " + label;
            }
            const std::string aff = procMonsterAffixList(e->procAffixMask);
            if (!aff.empty()) {
                label += " (" + aff + ")";
            }
        }
    }
                if (e->friendly) {
                    label += " (ALLY";
                    switch (e->allyOrder) {
                        case AllyOrder::Stay:  label += ", STAY"; break;
                        case AllyOrder::Fetch: label += ", FETCH"; break;
                        case AllyOrder::Guard: label += ", GUARD"; break;
                        case AllyOrder::Follow:
                        default: break;
                    }
                    label += ")";
                }
                ss << " | " << label << " " << e->hp << "/" << e->hpMax;

                // Codex (per-run) stats: kills by kind + XP value.
                const uint16_t kindKills = codexKills(showKind);
                if (kindKills > 0) {
                    ss << " | KILLS: " << kindKills;
                }
                ss << " | XP: " << (hallu ? xpFor(showKind) : xpFor(*e));

                if (showKind == EntityKind::Ghost) {
                    ss << " | ETHEREAL";
                }

                if (e->stolenGold > 0) {
                    if (showKind == EntityKind::Leprechaun) {
                        ss << " | STOLEN: " << e->stolenGold << "G";
                    } else if (e->friendly) {
                        ss << " | CARRY: " << e->stolenGold << "G";
                    } else {
                        ss << " | LOOT: " << e->stolenGold << "G";
                    }
                }

                if (!hallu && e->friendly && e->pocketConsumable.id != 0 && e->pocketConsumable.count > 0) {
                    ss << " | PACK: " << displayItemName(e->pocketConsumable);
                }

                if (e->effects.fearTurns > 0) {
                    ss << " | FEARED";
                }

                // Don't leak extra information while hallucinating: the player is already
                // being shown a distorted creature type.
                if (!hallu) {
                    if (e->gearMelee.id != 0 && isMeleeWeapon(e->gearMelee.kind)) {
                        ss << " | WPN: " << itemDisplayNameSingle(e->gearMelee.kind);
                    }
                    if (e->gearArmor.id != 0 && isArmor(e->gearArmor.kind)) {
                        ss << " | ARM: " << itemDisplayNameSingle(e->gearArmor.kind);
                    }

                    // Quick melee duel forecast (expected turns to kill / die) for visible non-allies.
                    // Keep this intentionally compact; it renders in the LOOK bottom-line.
                    if (!e->friendly && e->hp > 0) {
                        const DuelForecast f = computeDuelForecast(*this, *e);
                        ss << " | " << duelForecastLabel(f);
                    }
                }

            }
        }

        // Items (show first one + count)
        int itemCount = 0;
        const GroundItem* first = nullptr;
        for (const auto& gi : ground) {
            if (gi.pos.x == p.x && gi.pos.y == p.y) {
                ++itemCount;
                if (!first) first = &gi;
            }
        }
        if (itemCount > 0 && first) {
            Item showItem = first->item;
            if (hallu) {
                showItem.kind = hallucinatedItemKind(*this, first->item);
            }

            std::string itemLabel = displayItemName(showItem);

            // Chest metadata is deliberately suppressed while hallucinating: it would otherwise
            // reveal the true underlying object even if the player "sees" something else.
            if (!hallu) {
                if (first->item.kind == ItemKind::Chest) {
                    if (chestLocked(first->item)) itemLabel += " (LOCKED)";
                    if (chestTrapped(first->item) && chestTrapKnown(first->item)) itemLabel += " (TRAPPED)";
                } else if (first->item.kind == ItemKind::ChestOpen) {
                    int stacks = 0;
                    for (const auto& c : chestContainers_) {
                        if (c.chestId == first->item.id) { stacks = static_cast<int>(c.items.size()); break; }
                    }
                    const int tier = chestTier(first->item);
                    const int limit = chestStackLimitForTier(tier);
                    std::stringstream cs;
                    cs << " (" << chestTierName(tier) << " " << stacks << "/" << limit << ")";
                    itemLabel += cs.str();
                }
            }
            ss << " | ITEM: " << itemLabel;
            if (itemCount > 1) ss << " (+" << (itemCount - 1) << ")";
        }
    }

    // Distance (Manhattan for clarity)
    Vec2i pp = player().pos;
    int dist = std::abs(p.x - pp.x) + std::abs(p.y - pp.y);
    ss << " | DIST " << dist;

    // Context hint for tile-interactables.
    if (p == player().pos) {
        const TileType tt = dung.at(p.x, p.y).type;
        if (tt == TileType::Fountain) {
            ss << " | ENTER: DRINK";
        } else if (tt == TileType::Altar) {
            ss << " | ENTER: PRAY";
        }
    }

    return ss.str();
}

std::string Game::lookInfoText() const {
    if (!looking) return std::string();
    std::string s = describeAt(lookPos);
    if (soundPreviewOpen) {
        s += " | SOUND PREVIEW VOL " + std::to_string(soundPreviewVol) + "  ([ ] ADJUST)";
    }
    return s;
}

void Game::restUntilSafe() {
    if (isFinished()) return;
    if (inputLock) return;

    // Cancel auto-move to avoid fighting the stepper.
    if (autoMode != AutoMoveMode::None) {
        stopAutoMove(true);
    }

    const int manaMax = playerManaMax();
    const bool needHp = (player().hp < player().hpMax);
    const bool needMana = (manaMax > 0 && mana_ < manaMax);

    // If nothing to do, don't burn time.
    if (!needHp && !needMana) {
        pushMsg("YOU ARE ALREADY FULLY RESTED.", MessageKind::System, true);
        return;
    }

    // Resting while standing in fire (or actively burning) is a great way to die.
    if (player().effects.burnTurns > 0 || fireAt(player().pos.x, player().pos.y) > 0u) {
        pushMsg("YOU CAN'T REST WHILE ON FIRE!", MessageKind::Warning, true);
        return;
    }

    // Don't auto-rest with danger in sight.
    if (anyVisibleHostiles()) {
        pushMsg("TOO DANGEROUS TO REST!", MessageKind::Warning, true);
        return;
    }

    // Hunger safety: if starvation is enabled and you're starving, don't auto-rest so you can eat.
    if (hungerEnabled_ && hungerStateFor(hunger, hungerMax) >= 2) {
        pushMsg("YOU ARE TOO HUNGRY TO REST!", MessageKind::Warning, true);
        return;
    }

    pushMsg("YOU REST...", MessageKind::Info, true);

    // Safety valve to prevent accidental infinite loops.
    const int maxSteps = 2000;
    int steps = 0;

    while (!isFinished() && steps < maxSteps) {
        // Abort if something hostile comes into view.
        if (anyVisibleHostiles()) {
            pushMsg("REST INTERRUPTED!", MessageKind::Warning, true);
            break;
        }

        const int manaMaxNow = playerManaMax();
        const bool needHpNow = (player().hp < player().hpMax);
        const bool needManaNow = (manaMaxNow > 0 && mana_ < manaMaxNow);

        if (!needHpNow && !needManaNow) {
            pushMsg("YOU FEEL RESTED.", MessageKind::Success, true);
            break;
        }

        // Resting while burning/standing in fire is never safe.
        if (player().effects.burnTurns > 0 || fireAt(player().pos.x, player().pos.y) > 0u) {
            pushMsg("REST INTERRUPTED!", MessageKind::Warning, true);
            break;
        }

        // Hunger safety: stop before starvation damage.
        if (hungerEnabled_ && hungerStateFor(hunger, hungerMax) >= 2) {
            pushMsg("REST STOPPED (YOU ARE STARVING).", MessageKind::Warning, true);
            break;
        }

        const int hpBefore = player().hp;

        // Consume a "wait" turn without spamming the log.
        advanceAfterPlayerAction();
        ++steps;

        if (isFinished()) break;

        // Stop if we took damage while resting (poison/burn/starvation/ambush/etc.).
        if (player().hp < hpBefore) {
            pushMsg("REST INTERRUPTED (YOU TOOK DAMAGE).", MessageKind::Warning, true);
            break;
        }

        // If hunger crossed into starvation, stop so the player can eat.
        if (hungerEnabled_ && hungerStateFor(hunger, hungerMax) >= 2) {
            pushMsg("REST STOPPED (YOU ARE STARVING).", MessageKind::Warning, true);
            break;
        }

        // If we became on fire during the wait, stop immediately.
        if (player().effects.burnTurns > 0 || fireAt(player().pos.x, player().pos.y) > 0u) {
            pushMsg("REST INTERRUPTED!", MessageKind::Warning, true);
            break;
        }
    }

    if (!isFinished() && steps >= maxSteps) {
        pushMsg("REST STOPPED (TOO LONG).", MessageKind::System, true);
    }
}


int Game::repeatSearch(int maxTurns, bool stopOnFind) {
    if (isFinished()) return 0;
    if (inputLock) return 0;

    if (maxTurns <= 0) return 0;
    maxTurns = clampi(maxTurns, 1, 2000);

    // Cancel auto-move to avoid fighting the stepper.
    if (autoMode != AutoMoveMode::None) {
        stopAutoMove(true);
    }

    // Single-turn: behave exactly like the normal Search action.
    if (maxTurns == 1) {
        (void)searchForTraps(true);
        advanceAfterPlayerAction();
        return 1;
    }

    // Repeated searching is usually only safe when no hostiles are visible.
    if (anyVisibleHostiles()) {
        pushMsg("TOO DANGEROUS TO SEARCH REPEATEDLY!", MessageKind::Warning, true);
        return 0;
    }

    pushMsg("YOU SEARCH...", MessageKind::Info, true);

    int steps = 0;
    int totalFoundTraps = 0;
    int totalFoundSecrets = 0;
    bool foundAny = false;
    bool interrupted = false;

    while (!isFinished() && steps < maxTurns) {
        // Abort if something hostile comes into view.
        if (anyVisibleHostiles()) {
            pushMsg("SEARCH INTERRUPTED!", MessageKind::Warning, true);
            interrupted = true;
            break;
        }

        int ft = 0;
        int fs = 0;
        (void)searchForTraps(false, &ft, &fs);

        totalFoundTraps += ft;
        totalFoundSecrets += fs;

        if (ft > 0 || fs > 0) {
            foundAny = true;
            if (stopOnFind) {
                // Report the discovery immediately (before monsters act), like normal search.
                pushMsg(formatSearchDiscoveryMessage(ft, fs), MessageKind::Info, true);
            }
        }

        advanceAfterPlayerAction();
        ++steps;

        if (foundAny && stopOnFind) break;
    }

    if (!isFinished()) {
        if (foundAny && !stopOnFind) {
            pushMsg(formatSearchDiscoveryMessage(totalFoundTraps, totalFoundSecrets), MessageKind::Info, true);
        } else if (!foundAny && !interrupted) {
            pushMsg("YOU FIND NOTHING.", MessageKind::Info, true);
        }
    }

    return steps;
}

