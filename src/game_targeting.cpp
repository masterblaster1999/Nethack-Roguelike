#include "game_internal.hpp"

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
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;
    targetPos = player().pos;
    recomputeTargetLine();
    pushMsg(msg);
}


void Game::endTargeting(bool fire) {
    if (!targeting) return;

    if (fire) {
        if (!targetValid) {
            pushMsg("NO CLEAR SHOT.");
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
}




void Game::moveTargetCursor(int dx, int dy) {
    if (!targeting) return;
    Vec2i p = targetPos;
    p.x = clampi(p.x + dx, 0, MAP_W - 1);
    p.y = clampi(p.y + dy, 0, MAP_H - 1);
    setTargetCursor(p);
}

void Game::recomputeTargetLine() {
    targetLine = bresenhamLine(player().pos, targetPos);

    // Clamp to range
    int range = playerRangedRange();
    if (range > 0 && static_cast<int>(targetLine.size()) > range + 1) {
        targetLine.resize(static_cast<size_t>(range + 1));
    }

    // Determine validity: must have LOS and be within visible tiles (you can't target what you can't see).
    targetValid = false;

    if (!dung.inBounds(targetPos.x, targetPos.y)) return;
    if (!dung.at(targetPos.x, targetPos.y).visible) return;

    // Verify LOS along clamped line (stop at opaque).
    for (size_t i = 1; i < targetLine.size(); ++i) {
        Vec2i p = targetLine[i];
        if (dung.isOpaque(p.x, p.y)) {
            // If the target is behind an opaque tile, invalid.
            if (p != targetPos) return;
        }
    }

    // Must be within range (by path length)
    if (range > 0) {
        int dist = static_cast<int>(targetLine.size()) - 1;
        if (dist > range) return;
    }

    // Weapon ready?
    std::string reason;
    if (!playerHasRangedReady(&reason)) return;

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
