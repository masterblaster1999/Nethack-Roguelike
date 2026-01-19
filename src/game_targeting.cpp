#include "game_internal.hpp"

#include "combat_rules.hpp"
#include "projectile_utils.hpp"

namespace {
static bool isTargetCandidateHostile(const Game& g, const Entity& e) {
    if (e.id == g.player().id) return false;
    if (e.hp <= 0) return false;
    if (e.friendly) return false;

    // Peaceful shopkeepers are intentionally excluded.
    if (e.kind == EntityKind::Shopkeeper && !e.alerted) return false;

    return true;
}


static bool isProtectedNonHostile(const Game& g, const Entity& e) {
    if (e.id == g.player().id) return false;
    if (e.hp <= 0) return false;

    if (e.friendly) return true;

    // Peaceful shopkeepers are intentionally treated as "protected" for player safety prompts.
    if (e.kind == EntityKind::Shopkeeper && !e.alerted) return true;

    return false;
}

static std::string protectedNameForUI(const Entity& e) {
    // Keep this short: the targeting HUD has limited room.
    if (e.kind == EntityKind::Player) return "YOU";

    const std::string n = kindName(e.kind);
    if (e.friendly) {
        return std::string("YOUR ") + n;
    }

    if (e.kind == EntityKind::Shopkeeper && !e.alerted) {
        return "SHOPKEEPER";
    }

    return n;
}

static std::string summarizeNames(const std::vector<std::string>& names, size_t maxNames = 2) {
    if (names.empty()) return std::string();
    std::ostringstream ss;

    const size_t n = names.size();
    const size_t m = std::min(maxNames, n);
    for (size_t i = 0; i < m; ++i) {
        if (i > 0) ss << ", ";
        ss << names[i];
    }
    if (n > m) {
        ss << " +" << (n - m);
    }
    return ss.str();
}

static int hitChancePercent(int attackBonus, int targetAC) {
    int hits = 0;
    for (int natural = 1; natural <= 20; ++natural) {
        if (natural == 1) continue;       // always miss
        if (natural == 20) { ++hits; continue; } // always hit
        if (natural + attackBonus >= targetAC) ++hits;
    }
    // 20-sided die: each face is 5%.
    return hits * 5;
}

} // namespace


std::string Game::targetingInfoText() const {
    if (!targeting) return std::string();
    return describeAt(targetPos);
}

std::string Game::targetingStatusText() const {
    if (!targeting) return std::string();
    return targetStatusText_;
}

std::string Game::targetingWarningText() const {
    if (!targeting) return std::string();
    return targetWarningText_;
}


std::string Game::targetingCombatPreviewText() const {
    if (!targeting) return std::string();

    // Spell targeting preview (separate from ranged weapons/throw).
    if (targetingMode_ == TargetingMode::Spell) {
        const SpellKind sk = targetingSpell_;
        const SpellDef& sd = spellDef(sk);

        std::ostringstream ss;
        ss << sd.name;
        ss << " | MANA " << sd.manaCost;
        if (sd.range > 0) ss << " | RNG " << sd.range;

        if (sk == SpellKind::Blink) {
            // Teleport, no damage.
            return ss.str();
        }

        if (sk == SpellKind::PoisonCloud) {
            ss << " | GAS R2";
            ss << " | LINGERS";
            return ss.str();
        }

        // Damage preview for projectile spells.
        ProjectileKind projKind = ProjectileKind::Spark;
        if (sk == SpellKind::Fireball) projKind = ProjectileKind::Fireball;
        if (sk != SpellKind::MagicMissile && sk != SpellKind::Fireball) {
            return ss.str();
        }

        int atkBonus = player().baseAtk + playerFocus();
        if (sk == SpellKind::MagicMissile) atkBonus += 2;
        const int dmgBonus = std::max(0, playerFocus()) / 2;

        // Spells use the weaker baseline (wands are stronger).
        DiceExpr dice = rangedDiceForProjectile(projKind, /*wandPowered=*/false);
        dice.bonus += dmgBonus;
        dice.bonus += statDamageBonusFromAtk(player().baseAtk);
        const std::string dmgStr = diceToString(dice, true);

        // For an actual hit chance, only show it when the current target is valid and contains a creature.
        if (targetValid) {
            if (const Entity* e = entityAt(targetPos.x, targetPos.y)) {
                if (e->hp > 0 && e->id != player().id) {
                    const int ac = 10 + ((e->kind == EntityKind::Player) ? playerDefense() : e->baseDef);

                    const int dist = std::max(1, static_cast<int>(targetLine.size()) - 1);
                    const int penalty = dist / 3;

                    int adjAtk = atkBonus - penalty;
                    if (player().effects.confusionTurns > 0) adjAtk -= 3;

                    if (player().effects.hallucinationTurns > 0) {
                        ss << " | HIT ?%";
                    } else {
                        const int pct = hitChancePercent(adjAtk, ac);
                        ss << " | HIT " << pct << "%";
                    }
                }
            }
        }

        if (projKind == ProjectileKind::Fireball) ss << " | AOE";
        ss << " | DMG " << dmgStr;
        if (player().effects.confusionTurns > 0) ss << " | CONFUSED";

        return ss.str();
    }

    // Determine what will be used if the player fires right now (equipped ranged weapon vs throw).
    ProjectileKind projKind = ProjectileKind::Arrow;
    int range = 0;
    int atkBonus = 0;
    int dmgBonus = 0;
    bool isDigWand = false;
    bool wandPowered = false;
    std::string tag;

    if (const Item* w = equippedRanged()) {
        const ItemDef& d = itemDef(w->kind);
        const bool weaponReady =
            (d.range > 0) &&
            ((d.maxCharges <= 0) || (w->charges > 0)) &&
            ((d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0));

        if (weaponReady) {
            projKind = d.projectile;
            range = d.range;

            const int bucBonus = (w->buc < 0 ? -1 : (w->buc > 0 ? 1 : 0));
            const bool isWand = isRangedWeapon(w->kind) && d.maxCharges > 0 && d.ammo == AmmoKind::None;
            wandPowered = isWand;

            // Talents: Agility improves physical ranged weapons; Focus empowers wands.
            dmgBonus = w->enchant + bucBonus;
            if (isWand) dmgBonus += playerFocus();

            const int baseSkill = player().baseAtk + (isWand ? playerFocus() : playerAgility());
            atkBonus = baseSkill + d.rangedAtk + w->enchant + bucBonus;

            tag = itemDef(w->kind).name;
            isDigWand = (w->kind == ItemKind::WandDigging);
        }
    }

    if (range <= 0) {
        ThrowAmmoSpec spec;
        if (!choosePlayerThrowAmmo(inv, spec)) {
            return std::string();
        }

        projKind = spec.proj;
        range = throwRangeFor(player(), spec.ammo);
        atkBonus = player().baseAtk - 1 + playerAgility();
        dmgBonus = 0;
        tag = (spec.ammo == AmmoKind::Arrow) ? "THROW ARROW" : "THROW ROCK";
        isDigWand = false;
        wandPowered = false;
    }

    // Digging wands don't do direct damage; they carve tunnels.
    if (isDigWand) {
        return "DIG";
    }

    // Damage expression (before DR), approximated as base dice + static bonuses.
    DiceExpr dice = rangedDiceForProjectile(projKind, wandPowered);
    dice.bonus += dmgBonus;
    dice.bonus += statDamageBonusFromAtk(player().baseAtk);
    const std::string dmgStr = diceToString(dice, true);

    std::ostringstream ss;

    if (!tag.empty()) ss << tag << " ";

    // For an actual hit chance, only show it when the current target is valid and contains a creature.
    if (targetValid) {
        if (const Entity* e = entityAt(targetPos.x, targetPos.y)) {
            if (e->hp > 0 && e->id != player().id) {
                const int ac = 10 + ((e->kind == EntityKind::Player) ? playerDefense() : e->baseDef);

                const int dist = std::max(1, static_cast<int>(targetLine.size()) - 1);
                const int penalty = dist / 3;

                int adjAtk = atkBonus - penalty;
                const bool confused = (player().effects.confusionTurns > 0);
                if (confused) adjAtk -= 3;

                if (player().effects.hallucinationTurns > 0) {
                    ss << "HIT ?% ";
                } else {
                    const int pct = hitChancePercent(adjAtk, ac);
                    ss << "HIT " << pct << "% ";
                }
            }
        }
    }

    if (projKind == ProjectileKind::Fireball) ss << "AOE ";
    ss << "DMG " << dmgStr;

    if (player().effects.confusionTurns > 0) ss << " CONFUSED";

    return ss.str();
}


void Game::cycleTargetCursor(int dir) {
    if (!targeting) return;

    // Build a deterministic list of visible hostile targets.
    const Vec2i src = player().pos;
    int range = playerRangedRange();
    if (targetingMode_ == TargetingMode::Spell) {
        range = spellDef(targetingSpell_).range;
    }

    std::vector<Vec2i> cands;
    cands.reserve(16);

    for (const auto& e : ents) {
        if (!isTargetCandidateHostile(*this, e)) continue;
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;

        const Tile& t = dung.at(e.pos.x, e.pos.y);
        if (!t.visible) continue;

        const int dist = chebyshev(src, e.pos);
        if (range > 0 && dist > range) continue;

        if (!dung.hasLineOfSight(src.x, src.y, e.pos.x, e.pos.y)) continue;

        // Skip targets that are visible but not actually shootable (blocked by cover/corners).
        const std::vector<Vec2i> line = Game::bresenhamLine(src, e.pos);
        if (!hasClearProjectileLine(dung, line, e.pos, range)) continue;

        cands.push_back(e.pos);
    }

    if (cands.empty()) {
        pushMsg("NO VISIBLE TARGETS.", MessageKind::System, true);
        return;
    }

    // Stable deterministic ordering: closest first, then top-to-bottom, left-to-right.
    std::sort(cands.begin(), cands.end(), [&](const Vec2i& a, const Vec2i& b) {
        const int da = chebyshev(src, a);
        const int db = chebyshev(src, b);
        if (da != db) return da < db;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    cands.erase(std::unique(cands.begin(), cands.end()), cands.end());

    int idx = -1;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (cands[i] == targetPos) {
            idx = static_cast<int>(i);
            break;
        }
    }

    const int n = static_cast<int>(cands.size());
    int next = 0;
    if (idx < 0) {
        // If the cursor isn't on a hostile, jump to first/last depending on direction.
        next = (dir >= 0) ? 0 : (n - 1);
    } else {
        next = idx + dir;
        while (next < 0) next += n;
        while (next >= n) next -= n;
    }

    setTargetCursor(cands[static_cast<size_t>(next)]);
}

void Game::beginTargeting() {
    std::string reason;
    if (!playerHasRangedReady(&reason)) {
        pushMsg(reason);
        return;
    }

    // Provide a helpful hint about what will actually be used (weapon vs throw).
    std::string msg = "TARGETING...";

    if (const Item* w = equippedRanged()) {
        const ItemDef& d = itemDef(w->kind);
        const bool weaponReady =
            (d.range > 0) &&
            ((d.maxCharges <= 0) || (w->charges > 0)) &&
            ((d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0));

        if (weaponReady) {
            msg = "TARGETING (" + displayItemName(*w) + ")...";
        }
    }

    if (msg == "TARGETING...") {
        ThrowAmmoSpec spec;
        if (choosePlayerThrowAmmo(inv, spec)) {
            msg = (spec.ammo == AmmoKind::Arrow) ? "TARGETING (THROW ARROW)..." : "TARGETING (THROW ROCK)...";
        }
    }

    targeting = true;
    targetingMode_ = TargetingMode::Ranged;
    invOpen = false;
    spellsOpen = false;
    closeChestOverlay();
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;
    targetPos = player().pos;
    targetStatusText_.clear();
    recomputeTargetLine();
    pushMsg(msg);
}


void Game::beginSpellTargeting(SpellKind k) {
    const SpellDef& sd = spellDef(k);
    if (!sd.needsTarget) {
        pushMsg("THAT SPELL DOES NOT REQUIRE A TARGET.", MessageKind::System, true);
        return;
    }

    std::string reason;
    if (!canCastSpell(k, &reason)) {
        if (!reason.empty()) pushMsg(reason + ".", MessageKind::Warning, true);
        return;
    }

    targeting = true;
    targetingMode_ = TargetingMode::Spell;
    targetingSpell_ = k;

    invOpen = false;
    spellsOpen = false;
    closeChestOverlay();
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    targetPos = player().pos;
    targetStatusText_.clear();
    recomputeTargetLine();

    pushMsg(std::string("CAST ") + sd.name + "...", MessageKind::System, true);
}


bool Game::endTargeting(bool fire) {
    if (!targeting) return false;

    auto closeTargeting = [&]() {
        targeting = false;
        targetLine.clear();
        targetValid = false;
        targetStatusText_.clear();
        targetWarningText_.clear();
        targetUnsafe_ = false;
        targetUnsafeConfirmed_ = false;
        targetingMode_ = TargetingMode::Ranged;
    };

    // Spell targeting: cast the selected spell instead of firing ranged weapons.
    if (targetingMode_ == TargetingMode::Spell) {
        if (!fire) {
            closeTargeting();
            return false;
        }

        if (!targetValid) {
            if (!targetStatusText_.empty()) pushMsg(targetStatusText_ + ".");
            else pushMsg("NO CLEAR TARGET.");
            // Keep targeting open; do not consume the turn.
            return false;
        }


        // Safety: require a second press to confirm risky casts (friendly fire / self-damage).
        if (targetUnsafe_ && !targetUnsafeConfirmed_) {
            targetUnsafeConfirmed_ = true;
            if (!targetWarningText_.empty()) {
                targetWarningText_ += " (FIRE AGAIN)";
            } else {
                targetWarningText_ = "UNSAFE TARGET (FIRE AGAIN)";
            }
            pushMsg("UNSAFE TARGET - PRESS FIRE AGAIN TO CONFIRM.", MessageKind::Warning, true);
            return false;
        }

        const bool casted = castSpellAt(targetingSpell_, targetPos);
        if (casted) {
            closeTargeting();
            return true;
        }

        // If the cast failed (target changed, etc.), keep targeting open.
        return false;
    }

    // Ranged targeting.
    if (!fire) {
        closeTargeting();
        return false;
    }

    if (!targetValid) {
        if (!targetStatusText_.empty()) pushMsg(targetStatusText_ + ".");
        else pushMsg("NO CLEAR SHOT.");
        // Keep targeting open; do not consume the turn.
        return false;
    }


    // Safety: require a second press to confirm risky shots (friendly fire / self-damage).
    if (targetUnsafe_ && !targetUnsafeConfirmed_) {
        targetUnsafeConfirmed_ = true;
        if (!targetWarningText_.empty()) {
            targetWarningText_ += " (FIRE AGAIN)";
        } else {
            targetWarningText_ = "UNSAFE TARGET (FIRE AGAIN)";
        }
        pushMsg("UNSAFE TARGET - PRESS FIRE AGAIN TO CONFIRM.", MessageKind::Warning, true);
        return false;
    }

    bool didAttack = false;

    // First choice: fire the equipped ranged weapon if it is ready.
    int wIdx = equippedRangedIndex();
    if (wIdx >= 0) {
        // Copy weapon data up front so later inventory edits (ammo consumption) can't invalidate references.
        const Item wCopy = inv[static_cast<size_t>(wIdx)];
        const ItemDef& d = itemDef(wCopy.kind);

        const bool weaponReady =
            (d.range > 0) &&
            ((d.maxCharges <= 0) || (wCopy.charges > 0)) &&
            ((d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0));

        if (weaponReady) {
            Item projectile;
            const Item* projPtr = nullptr;

            // Consume charge (on the actual inventory item, not the copy).
            // Do this before consuming ammo, since ammo consumption can erase stacks and shift indices.
            bool sputtered = false;
            if (d.maxCharges > 0) {
                Item& wMut = inv[static_cast<size_t>(wIdx)];
                wMut.charges = std::max(0, wMut.charges - 1);
                sputtered = (wMut.charges <= 0);
            }

            // Consume ammo and capture a 1-count template so recovered projectiles preserve metadata
            // (shopPrice/shopDepth, etc.).
            if (d.ammo != AmmoKind::None) {
                if (consumeOneAmmo(inv, d.ammo, &projectile)) {
                    projPtr = &projectile;
                }
            }

            // d20 to-hit + dice damage handled in attackRanged().
            const int bucBonus = (wCopy.buc < 0 ? -1 : (wCopy.buc > 0 ? 1 : 0));

            const bool isWand = isRangedWeapon(wCopy.kind) && d.maxCharges > 0 && d.ammo == AmmoKind::None;

            // Talents: Agility improves physical ranged weapons; Focus empowers wands.
            int dmgBonus = wCopy.enchant + bucBonus;
            if (isWand) dmgBonus += playerFocus();

            const int baseSkill = player().baseAtk + (isWand ? playerFocus() : playerAgility());
            const int atkBonus = baseSkill + d.rangedAtk + wCopy.enchant + bucBonus;

            if (wCopy.kind == ItemKind::WandDigging) {
                zapDiggingWand(d.range);
            } else {
                attackRanged(playerMut(), targetPos, d.range, atkBonus, dmgBonus, d.projectile, true, projPtr, /*wandPowered=*/isWand);
            }

            if (isWand) {
                (void)markIdentified(wCopy.kind, false);
            }

            if (d.maxCharges > 0 && sputtered) {
                pushMsg("YOUR WAND SPUTTERS OUT.");
            }

            didAttack = true;
        }
    }

    // Fallback: if no ranged weapon is ready, allow throwing ammo by hand.
    if (!didAttack) {
        ThrowAmmoSpec spec;
        if (choosePlayerThrowAmmo(inv, spec)) {
            // Consume one projectile from the inventory and keep a 1-count template so recovered ammo
            // preserves metadata (shopPrice/shopDepth, etc.).
            Item projectile;
            const Item* projPtr = nullptr;
            if (consumeOneAmmo(inv, spec.ammo, &projectile)) {
                projPtr = &projectile;
            }

            const int range = throwRangeFor(player(), spec.ammo);
            const int atkBonus = player().baseAtk - 1 + playerAgility();
            const int dmgBonus = 0;
            attackRanged(playerMut(), targetPos, range, atkBonus, dmgBonus, spec.proj, true, projPtr);
            didAttack = true;
        }
    }

    if (!didAttack) {
        // Should be rare (inventory changed mid-targeting, etc).
        std::string reason;
        if (!playerHasRangedReady(&reason)) pushMsg(reason);
        else pushMsg("YOU CAN'T FIRE RIGHT NOW.");

        // Keep targeting open; do not consume the turn.
        return false;
    }

    closeTargeting();
    return true;
}



void Game::moveTargetCursor(int dx, int dy) {
    if (!targeting) return;
    Vec2i p = targetPos;
    p.x = clampi(p.x + dx, 0, dung.width - 1);
    p.y = clampi(p.y + dy, 0, dung.height - 1);
    setTargetCursor(p);
}

void Game::recomputeTargetLine() {
    targetStatusText_.clear();

    // Reset safety warnings whenever the cursor changes.
    // (If the player had confirmed a risky shot, moving the cursor should require a fresh confirm.)
    targetWarningText_.clear();
    targetUnsafe_ = false;
    targetUnsafeConfirmed_ = false;

    targetValid = false;

    targetLine = bresenhamLine(player().pos, targetPos);
    if (targetLine.size() <= 1) {
        targetStatusText_ = "NO TARGET";
        return;
    }

    // Clamp the line to the current targeting range (ranged weapons or spell range).
    // Note: the cursor can still be beyond range; in that case we render the truncated line but
    // mark the target as invalid.
    int range = playerRangedRange();
    if (targetingMode_ == TargetingMode::Spell) {
        range = spellDef(targetingSpell_).range;
    }
    if (range > 0 && static_cast<int>(targetLine.size()) > range + 1) {
        targetLine.resize(static_cast<size_t>(range + 1));
    }

    if (!dung.inBounds(targetPos.x, targetPos.y)) {
        targetStatusText_ = "OUT OF BOUNDS";
        return;
    }
    if (!dung.at(targetPos.x, targetPos.y).visible) {
        targetStatusText_ = "TARGET NOT VISIBLE";
        return;
    }

    if (targetingMode_ == TargetingMode::Spell) {
        // Validate spell prerequisites (mainly mana).
        std::string reason;
        if (!canCastSpell(targetingSpell_, &reason)) {
            targetStatusText_ = reason;
            return;
        }

        // Blink requires a walkable destination.
        if (targetingSpell_ == SpellKind::Blink) {
            if (!dung.isWalkable(targetPos.x, targetPos.y)) {
                targetStatusText_ = "CAN'T BLINK THERE";
                return;
            }
            if (const Entity* e = entityAt(targetPos.x, targetPos.y)) {
                if (e->hp > 0 && e->id != player().id) {
                    targetStatusText_ = "SPACE OCCUPIED";
                    return;
                }
            }
        }

        if (targetingSpell_ == SpellKind::PoisonCloud) {
            // Poison clouds only make sense on walkable tiles.
            if (!dung.isWalkable(targetPos.x, targetPos.y)) {
                targetStatusText_ = "CAN'T TARGET THERE";
                return;
            }
        }
    } else {
        // Weapon ready?
        std::string reason;
        if (!playerHasRangedReady(&reason)) {
            targetStatusText_ = reason;
            return;
        }
    }

    // If the truncated line doesn't reach the cursor, we're out of range.
    if (range > 0 && !targetLine.empty() && targetLine.back() != targetPos) {
        targetStatusText_ = "OUT OF RANGE";
        return;
    }

    // Verify a clear projectile line (no solid blockers; no diagonal corner threading).
    for (size_t i = 1; i < targetLine.size(); ++i) {
        const Vec2i p = targetLine[i];
        if (!dung.inBounds(p.x, p.y)) {
            targetStatusText_ = "OUT OF BOUNDS";
            return;
        }

        if (projectileCornerBlocked(dung, targetLine[i - 1], p)) {
            // Truncate the drawn line at the collision point for clarity.
            if (i + 1 < targetLine.size()) targetLine.resize(i + 1);
            targetStatusText_ = "NO CLEAR SHOT";
            return;
        }

        // Solid terrain blocks the shot unless it's the exact cursor tile.
        if (dung.blocksProjectiles(p.x, p.y) && p != targetPos) {
            if (i + 1 < targetLine.size()) targetLine.resize(i + 1);
            targetStatusText_ = "NO CLEAR SHOT";
            return;
        }
    }

    targetValid = true;

    // ------------------------------------------------------------
    // Safety warnings (UI-only)
    // ------------------------------------------------------------
    // Only warn when we can do so without leaking hidden information.
    // (We only consider entities on tiles currently visible to the player.)

    bool checkLine = false;
    bool checkAoe = false;
    int aoeRadius = 0;

    // Determine whether the current targeting mode produces a projectile and/or an AoE.
    if (targetingMode_ == TargetingMode::Spell) {
        switch (targetingSpell_) {
            case SpellKind::MagicMissile:
                checkLine = true;
                break;
            case SpellKind::Fireball:
                checkLine = true;
                checkAoe = true;
                aoeRadius = 1;
                break;
            case SpellKind::PoisonCloud:
                checkAoe = true;
                aoeRadius = 2;
                break;
            default:
                break;
        }
    } else {
        // Ranged weapons / throwing.
        ProjectileKind pk = ProjectileKind::Arrow;
        bool isDigWand = false;

        if (const Item* w = equippedRanged()) {
            const ItemDef& d = itemDef(w->kind);
            const bool chargesOk = (d.maxCharges <= 0) || (w->charges > 0);
            const bool ammoOk = (d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0);
            if (d.range > 0 && chargesOk && ammoOk) {
                pk = d.projectile;
                isDigWand = (w->kind == ItemKind::WandDigging);
            }
        }

        if (!isDigWand) {
            checkLine = true;
            if (pk == ProjectileKind::Fireball) {
                checkAoe = true;
                aoeRadius = 1;
            }
        }
    }

    const Entity* protectedOnLine = nullptr;
    int protectedDist = 0;

    if (checkLine) {
        for (size_t i = 1; i < targetLine.size(); ++i) {
            const Vec2i p = targetLine[i];
            if (!dung.inBounds(p.x, p.y)) continue;
            if (!dung.at(p.x, p.y).visible) continue;

            const Entity* e = entityAt(p.x, p.y);
            if (!e || e->hp <= 0) continue;
            if (isProtectedNonHostile(*this, *e)) {
                protectedOnLine = e;
                protectedDist = static_cast<int>(i);
                break;
            }
        }
    }

    std::vector<std::string> aoeHits;
    if (checkAoe && aoeRadius > 0) {
        std::vector<uint8_t> mask;
        dung.computeFovMask(targetPos.x, targetPos.y, aoeRadius, mask);

        auto inMask = [&](int x, int y) -> bool {
            if (!dung.inBounds(x, y)) return false;
            const int idx = y * dung.width + x;
            if (idx < 0 || idx >= static_cast<int>(mask.size())) return false;
            return mask[static_cast<size_t>(idx)] != 0u;
        };

        // Player self-hit.
        const Vec2i pp = player().pos;
        const int dx = std::abs(pp.x - targetPos.x);
        const int dy = std::abs(pp.y - targetPos.y);
        if (std::max(dx, dy) <= aoeRadius && inMask(pp.x, pp.y)) {
            aoeHits.push_back("YOU");
        }

        // Protected friendlies.
        for (const Entity& e : ents) {
            if (e.hp <= 0) continue;
            if (e.id == player().id) continue;
            if (!isProtectedNonHostile(*this, e)) continue;
            if (!dung.inBounds(e.pos.x, e.pos.y)) continue;

            const int ex = std::abs(e.pos.x - targetPos.x);
            const int ey = std::abs(e.pos.y - targetPos.y);
            if (std::max(ex, ey) > aoeRadius) continue;

            // Only warn about visible entities to avoid leaking hidden info.
            if (!dung.at(e.pos.x, e.pos.y).visible) continue;

            if (!inMask(e.pos.x, e.pos.y)) continue;

            aoeHits.push_back(protectedNameForUI(e));
        }
    }

    if (protectedOnLine || !aoeHits.empty()) {
        targetUnsafe_ = true;

        std::vector<std::string> parts;
        if (protectedOnLine) {
            std::ostringstream ss;
            ss << "LINE " << protectedDist << ": " << protectedNameForUI(*protectedOnLine);
            parts.push_back(ss.str());
        }
        if (!aoeHits.empty()) {
            parts.push_back(std::string("AOE: ") + summarizeNames(aoeHits));
        }

        std::ostringstream ws;
        ws << "WARNING: ";
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) ws << "; ";
            ws << parts[i];
        }
        targetWarningText_ = ws.str();
    }
}



void Game::zapDiggingWand(int range) {
    if (range <= 0) range = 1;

    const Vec2i src = player().pos;
    const int dx = (targetPos.x > src.x) ? 1 : (targetPos.x < src.x) ? -1 : 0;
    const int dy = (targetPos.y > src.y) ? 1 : (targetPos.y < src.y) ? -1 : 0;
    if (dx == 0 && dy == 0) return;

    // Directional (8-way) zap: always digs a full beam to max range.
    const Vec2i end{ src.x + dx * range, src.y + dy * range };
    std::vector<Vec2i> ray = bresenhamLine(src, end);

    // Visuals: reuse spark projectile FX for now.
    FXProjectile fxp;
    fxp.kind = ProjectileKind::Spark;
    fxp.path = ray;
    fxp.pathIndex = 0;
    fxp.stepTimer = 0.0f;
    fxp.stepTime = 0.02f;
    fx.push_back(fxp);
    inputLock = true;

    // Digging is loud.
    emitNoise(src, 16);

    int dug = 0;
    for (size_t i = 1; i < ray.size(); ++i) {
        const Vec2i p = ray[i];
        if (!dung.inBounds(p.x, p.y)) break;

        if (dung.dig(p.x, p.y)) {
            dug += 1;
            pushFxParticle(FXParticlePreset::Dig, p, 26, 0.12f);
        }
    }

    if (dug > 0) {
        if (dug == 1) pushMsg("THE WALL CRUMBLES.", MessageKind::Info, true);
        else pushMsg("THE WAND CARVES A TUNNEL (" + std::to_string(dug) + " TILES).", MessageKind::Info, true);

        recomputeFov();
    } else {
        pushMsg("NOTHING YIELDS.", MessageKind::Info, true);
    }
}
