#include "game_internal.hpp"

#include "combat_rules.hpp"

namespace {

static std::vector<Vec2i> bresenhamLineLocal(Vec2i a, Vec2i b) {
    std::vector<Vec2i> pts;
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;

    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        pts.push_back({x0, y0});
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
        if (pts.size() > 512) break;
    }
    return pts;
}


static bool isTargetCandidateHostile(const Game& g, const Entity& e) {
    if (e.id == g.player().id) return false;
    if (e.hp <= 0) return false;
    if (e.friendly) return false;

    // Peaceful shopkeepers are intentionally excluded.
    if (e.kind == EntityKind::Shopkeeper && !e.alerted) return false;

    return true;
}

static bool projectileCornerBlocked(const Dungeon& dung, const Vec2i& prev, const Vec2i& p) {
    const int dx = (p.x > prev.x) ? 1 : (p.x < prev.x) ? -1 : 0;
    const int dy = (p.y > prev.y) ? 1 : (p.y < prev.y) ? -1 : 0;
    if (dx == 0 || dy == 0) return false;

    const int ax = prev.x + dx;
    const int ay = prev.y;
    const int bx = prev.x;
    const int by = prev.y + dy;

    if (!dung.inBounds(ax, ay) || !dung.inBounds(bx, by)) return false;
    return dung.blocksProjectiles(ax, ay) && dung.blocksProjectiles(bx, by);
}

static bool hasClearProjectileLine(const Dungeon& dung, const Vec2i& src, const Vec2i& dst, int range) {
    std::vector<Vec2i> line = bresenhamLineLocal(src, dst);
    if (line.size() <= 1) return false;

    if (range > 0 && static_cast<int>(line.size()) > range + 1) {
        // Out of range.
        return false;
    }

    for (size_t i = 1; i < line.size(); ++i) {
        const Vec2i p = line[i];
        if (!dung.inBounds(p.x, p.y)) return false;

        if (projectileCornerBlocked(dung, line[i - 1], p)) return false;

        // Terrain blocks the shot unless it's the intended destination.
        if (dung.blocksProjectiles(p.x, p.y) && p != dst) return false;
    }

    return true;
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

std::string Game::targetingCombatPreviewText() const {
    if (!targeting) return std::string();

    // Determine what will be used if the player fires right now (equipped ranged weapon vs throw).
    ProjectileKind projKind = ProjectileKind::Arrow;
    int range = 0;
    int atkBonus = 0;
    int dmgBonus = 0;
    bool isDigWand = false;
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
    }

    // Digging wands don't do direct damage; they carve tunnels.
    if (isDigWand) {
        return "DIG";
    }

    // Damage expression (before DR), approximated as base dice + static bonuses.
    const bool wandPowered = (projKind == ProjectileKind::Spark || projKind == ProjectileKind::Fireball);
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

                const int pct = hitChancePercent(adjAtk, ac);
                ss << "HIT " << pct << "% ";
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
    const int range = playerRangedRange();

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
        if (!hasClearProjectileLine(dung, src, e.pos, range)) continue;

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
    invOpen = false;
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


void Game::endTargeting(bool fire) {
    if (!targeting) return;

    if (fire) {
        if (!targetValid) {
            if (!targetStatusText_.empty()) pushMsg(targetStatusText_ + ".");
            else pushMsg("NO CLEAR SHOT.");
        } else {
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
                        attackRanged(playerMut(), targetPos, d.range, atkBonus, dmgBonus, d.projectile, true, projPtr);
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
            }
        }
    }

    targeting = false;
    targetLine.clear();
    targetValid = false;
    targetStatusText_.clear();
}




void Game::moveTargetCursor(int dx, int dy) {
    if (!targeting) return;
    Vec2i p = targetPos;
    p.x = clampi(p.x + dx, 0, MAP_W - 1);
    p.y = clampi(p.y + dy, 0, MAP_H - 1);
    setTargetCursor(p);
}

void Game::recomputeTargetLine() {
    targetStatusText_.clear();
    targetValid = false;

    targetLine = bresenhamLine(player().pos, targetPos);
    if (targetLine.size() <= 1) {
        targetStatusText_ = "NO TARGET";
        return;
    }

    // Clamp the line to the *current* ranged range. Note: the cursor can still be beyond range;
    // in that case we render the truncated line but mark the target as invalid.
    const int range = playerRangedRange();
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

    // Weapon ready?
    std::string reason;
    if (!playerHasRangedReady(&reason)) {
        targetStatusText_ = reason;
        return;
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
