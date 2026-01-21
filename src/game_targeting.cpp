#include "game_internal.hpp"

#include "combat_rules.hpp"
#include "fishing_gen.hpp"
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


static bool isCaptureCandidate(const Game& g, const Entity& e) {
    if (e.id == g.player().id) return false;
    if (e.hp <= 0) return false;
    if (e.friendly) return false;

    // Never capture peaceful NPCs / bosses.
    if (e.kind == EntityKind::Shopkeeper) return false;
    if (e.kind == EntityKind::Minotaur) return false;

    // Keep parity with Scroll of Taming restrictions for now.
    if (entityIsUndead(e.kind)) return false;

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

static int captureChancePercent(const Game& g, ItemKind sphereKind, const Entity& target) {
    // Base chance + ...
    const int hpPct = (target.hpMax > 0) ? clampi((target.hp * 100 + target.hpMax / 2) / target.hpMax, 0, 100) : 100;

    int chance = 25;
    // Lower HP -> higher chance.
    chance += (100 - hpPct) / 2; // 0..50

    int statusBonus = 0;
    if (target.effects.webTurns > 0) statusBonus += 15;
    if (target.effects.fearTurns > 0) statusBonus += 12;
    if (target.effects.confusionTurns > 0) statusBonus += 10;
    if (target.effects.poisonTurns > 0) statusBonus += 8;
    if (target.effects.burnTurns > 0) statusBonus += 8;
    if (target.effects.corrosionTurns > 0) statusBonus += 8;
    chance += statusBonus;

    // Player talent influence: Focus (precision) and Agility (throwing).
    chance += g.playerFocus() * 3;
    chance += g.playerAgility() * 2;

    // Harder monsters are tougher to capture, especially deeper down.
    const int diff = g.xpFor(target.kind);
    chance -= (diff > 35 ? 35 : diff);
    chance -= g.depth();

    chance = clampi(chance, 3, 90);

    // Sphere tier multiplier (integer, to avoid float rounding differences).
    int multPct = 100;
    if (sphereKind == ItemKind::MegaSphere || sphereKind == ItemKind::MegaSphereFull) multPct = 125;
    chance = (chance * multPct) / 100;

    return clampi(chance, 1, 95);
}


// -----------------------------------------------------------------------------
// Fishing
// -----------------------------------------------------------------------------

static bool isFishableTile(const Game& g, Vec2i p) {
    if (!g.dungeon().inBounds(p.x, p.y)) return false;
    const TileType tt = g.dungeon().at(p.x, p.y).type;

    // Fountains are a small water source on any floor.
    if (tt == TileType::Fountain) return true;

    // In the overworld/surface camp, TileType::Chasm represents water basins.
    if (tt == TileType::Chasm && g.atCamp()) return true;

    return false;
}

static int fishableNeighborhoodCount(const Game& g, Vec2i p, int rad) {
    int n = 0;
    for (int dy = -rad; dy <= rad; ++dy) {
        for (int dx = -rad; dx <= rad; ++dx) {
            Vec2i q{p.x + dx, p.y + dy};
            if (!g.dungeon().inBounds(q.x, q.y)) continue;
            if (isFishableTile(g, q)) n += 1;
        }
    }
    return n;
}

static uint32_t fishWaterSeedAt(const Game& g, Vec2i p) {
    // Domain-separated stable per-tile seed.
    const uint32_t runSeed = static_cast<uint32_t>(g.seed());
    uint32_t salt = hashCombine(runSeed ^ 0xF1511234u,
                               hashCombine(static_cast<uint32_t>(g.branch()), static_cast<uint32_t>(g.depth())));

    // Overworld tiles should be stable across chunk boundaries.
    int wx = p.x;
    int wy = p.y;
    if (g.atCamp()) {
        wx = g.overworldX() * g.dungeon().width + p.x;
        wy = g.overworldY() * g.dungeon().height + p.y;
        salt = hashCombine(salt, hashCombine(static_cast<uint32_t>(g.overworldX()), static_cast<uint32_t>(g.overworldY())));
    }

    // hashCoord is defined in overworld.hpp; avoid needing it here by using a simple coordinate mix.
    const uint32_t hx = hash32(static_cast<uint32_t>(wx) ^ 0xA341316Cu);
    const uint32_t hy = hash32(static_cast<uint32_t>(wy) ^ 0xC8013EA4u);
    return hash32(hashCombine(salt ^ 0xB17ECAD1u, hashCombine(hx, hy)));
}

static int fishingChancePercent(const Game& g, const Item& rod, Vec2i waterPos) {
    (void)rod;

    const uint32_t ws = fishWaterSeedAt(g, waterPos);
    const int turn = static_cast<int>(g.turns());
    const bool inWindow = fishgen::isInBiteWindow(ws, turn);
    const float w01 = fishgen::biteWindow01(ws, turn);
    const int density = fishableNeighborhoodCount(g, waterPos, 2);

    float chance = 6.0f;

    // Bite cadence is the main driver: fishing is learnable and responsive.
    if (inWindow) chance += 10.0f;
    chance += 44.0f * w01;

    // Larger water bodies are a bit easier.
    chance += std::min(20, density) * 0.6f;

    // Player talent: focus (patience/feel) and agility (cast control).
    chance += g.playerFocus() * 2.0f;
    chance += g.playerAgility() * 1.0f;

    // Fountains are tiny and "concentrated".
    if (g.dungeon().at(waterPos.x, waterPos.y).type == TileType::Fountain) {
        chance += 10.0f;
    }

    // Starving hands shake.
    if (g.hungerEnabled() && g.hungerMaximum() > 0) {
        if (g.hungerCurrent() < g.hungerMaximum() / 4) chance -= 8.0f;
    }

    return clampi(static_cast<int>(std::lround(chance)), 1, 95);
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


std::string Game::fishingFightStatusText() const {
    if (!fishingFightActive_) return std::string();

    std::ostringstream ss;
    ss << "FISH ON";
    if (!fishingFightLabel_.empty()) ss << " | " << fishingFightLabel_;
    ss << " | TENSION " << fishingFightTension_ << " (SAFE " << fishingFightSafeMin_ << "-" << fishingFightSafeMax_ << ")";
    ss << " | PROG " << clampi(fishingFightProgress_, 0, 100) << "%";
    ss << " | TIME " << std::max(0, fishingFightTurnsLeft_);
    return ss.str();
}

std::string Game::fishingFightControlText() const {
    if (!fishingFightActive_) return std::string();
    return "ENTER REEL | . SLACK | ESC LET GO";
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

    // Capture sphere targeting preview.
    if (targetingMode_ == TargetingMode::Capture) {
        const int idx = findItemIndexById(inv, targetingCaptureItemId_);
        if (idx < 0) return "CAPTURE";
        const Item& sphere = inv[static_cast<size_t>(idx)];
        std::ostringstream ss;
        ss << itemDef(sphere.kind).name;
        ss << " | RNG " << captureSphereRange(sphere.kind);

        if (isCaptureSphereEmptyKind(sphere.kind)) {
            ss << " | THROW";
            if (targetValid) {
                if (const Entity* e = entityAt(targetPos.x, targetPos.y)) {
                    if (isCaptureCandidate(*this, *e)) {
                        if (player().effects.hallucinationTurns > 0) {
                            ss << " | CATCH ?%";
                        } else {
                            const int pct = captureChancePercent(*this, sphere.kind, *e);
                            ss << " | CATCH " << pct << "%";
                        }
                    }
                }
            }
        } else if (isCaptureSphereFullKind(sphere.kind)) {
            const int bond = clampi(captureSphereBondFromCharges(sphere.charges), 0, 99);
            const int hpPct = clampi(captureSphereHpPctFromCharges(sphere.charges), 0, 100);
            ss << " | RELEASE";
            ss << " | BOND " << bond;
            ss << " | HP " << hpPct << "%";
        } else {
            ss << " | ?";
        }

        return ss.str();
    }

    // Fishing rod targeting preview.
    if (targetingMode_ == TargetingMode::Fish) {
        const uint32_t ws = fishWaterSeedAt(*this, targetPos);
        const int turn = static_cast<int>(turns());
        const bool bite = fishgen::isInBiteWindow(ws, turn);
        const float w01 = fishgen::biteWindow01(ws, turn);
        const int density = fishableNeighborhoodCount(*this, targetPos, 2);
        const int pct = fishingChancePercent(*this, rod, targetPos);

        ss << (bite ? "BITE HOT" : "BITE COLD");
        if (bite && w01 > 0.60f) ss << "!";

        if (bite) {
            const int rem = fishgen::turnsRemainingInBiteWindow(ws, turn);
            if (rem > 0) ss << " (" << rem << "T)";
        } else {
            const int nxt = fishgen::turnsUntilNextBite(ws, turn);
            if (nxt > 0) ss << " (NEXT " << nxt << "T)";
        }

        ss << " | CATCH " << pct << "%";
        if (density >= 14) ss << " | DEEP";
        else if (density <= 8) ss << " | SHALLOW";
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
    } else if (targetingMode_ == TargetingMode::Capture) {
        const int idx = findItemIndexById(inv, targetingCaptureItemId_);
        if (idx >= 0) {
            const Item& sphere = inv[static_cast<size_t>(idx)];
            // When releasing a companion, target cycling doesn't make sense.
            if (isCaptureSphereFullKind(sphere.kind)) return;
            range = captureSphereRange(sphere.kind);
        } else {
            range = 6;
        }
    }

    std::vector<Vec2i> cands;
    cands.reserve(16);

    for (const auto& e : ents) {
        if (targetingMode_ == TargetingMode::Capture) {
            if (!isCaptureCandidate(*this, e)) continue;
        } else {
            if (!isTargetCandidateHostile(*this, e)) continue;
        }
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
    targetingCaptureItemId_ = 0;
    targetingFishingRodItemId_ = 0;
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
    targetingCaptureItemId_ = 0;
    targetingFishingRodItemId_ = 0;

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


void Game::beginCaptureTargeting(int sphereItemId) {
    const int idx = findItemIndexById(inv, sphereItemId);
    if (idx < 0) {
        pushMsg("YOU DON'T HAVE THAT ANYMORE.", MessageKind::Warning, true);
        return;
    }

    const Item& sphere = inv[static_cast<size_t>(idx)];
    if (!isCaptureSphereKind(sphere.kind)) {
        pushMsg("THAT IS NOT A CAPTURE SPHERE.", MessageKind::Warning, true);
        return;
    }

    targeting = true;
    targetingMode_ = TargetingMode::Capture;
    targetingCaptureItemId_ = sphereItemId;
    targetingFishingRodItemId_ = 0;

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

    if (isCaptureSphereEmptyKind(sphere.kind)) {
        pushMsg(std::string("THROW ") + itemDef(sphere.kind).name + "...", MessageKind::System, true);
    } else {
        pushMsg(std::string("RELEASE ") + itemDef(sphere.kind).name + "...", MessageKind::System, true);
    }
}


void Game::beginFishingTargeting(int rodItemId) {
    const int idx = findItemIndexById(inv, rodItemId);
    if (idx < 0) {
        pushMsg("YOU DON'T HAVE THAT ANYMORE.", MessageKind::Warning, true);
        return;
    }

    const Item& rod = inv[static_cast<size_t>(idx)];
    if (!isFishingRodKind(rod.kind)) {
        pushMsg("THAT IS NOT A FISHING ROD.", MessageKind::Warning, true);
        return;
    }

    const ItemDef& d = itemDef(rod.kind);
    const int maxDur = std::max(0, d.maxCharges);
    int curDur = rod.charges;
    if (maxDur > 0 && curDur <= 0) curDur = maxDur;
    if (maxDur > 0 && curDur <= 0) {
        pushMsg("YOUR ROD IS BROKEN.", MessageKind::Warning, true);
        return;
    }

    targeting = true;
    targetingMode_ = TargetingMode::Fish;
    targetingFishingRodItemId_ = rodItemId;
    targetingCaptureItemId_ = 0;

    invOpen = false;
    spellsOpen = false;
    closeChestOverlay();
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    // Start the cursor on the nearest visible fishable tile (if any), otherwise on the player.
    targetPos = player().pos;
    const int range = (d.range > 0) ? d.range : 6;
    int best = 1 << 30;
    for (int y = 0; y < dung.height; ++y) {
        for (int x = 0; x < dung.width; ++x) {
            if (!dung.at(x, y).visible) continue;
            const Vec2i p{x, y};
            if (!isFishableTile(*this, p)) continue;
            const int dist = chebyshev(player().pos, p);
            if (dist <= 0 || dist > range) continue;
            if (dist < best) {
                best = dist;
                targetPos = p;
            }
        }
    }

    targetStatusText_.clear();
    recomputeTargetLine();

    pushMsg("CAST YOUR LINE...", MessageKind::System, true);
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
        targetingCaptureItemId_ = 0;
        targetingFishingRodItemId_ = 0;
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

    // Capture sphere targeting.
    if (targetingMode_ == TargetingMode::Capture) {
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

        const int idx = findItemIndexById(inv, targetingCaptureItemId_);
        if (idx < 0) {
            pushMsg("YOU DON'T HAVE THAT ANYMORE.", MessageKind::Warning, true);
            closeTargeting();
            return false;
        }

        Item& sphere = inv[static_cast<size_t>(idx)];
        const ItemKind sphereKind = sphere.kind;

        // Helper: consume one sphere from a stack and record shop debt if it was unpaid.
        auto consumeOneSphere = [&](int invIndex) {
            if (invIndex < 0 || invIndex >= static_cast<int>(inv.size())) return;
            Item& it = inv[static_cast<size_t>(invIndex)];

            if (it.shopPrice > 0 && it.shopDepth > 0) {
                const int sd = it.shopDepth;
                if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH) {
                    shopDebtLedger_[sd] += it.shopPrice;
                }
            }

            if (it.count > 1) {
                it.count -= 1;
            } else {
                inv.erase(inv.begin() + invIndex);
            }
        };

        // ------------------------------------------------------------
        // Empty sphere: attempt capture.
        // ------------------------------------------------------------
        if (isCaptureSphereEmptyKind(sphereKind)) {
            Entity* t = entityAtMut(targetPos.x, targetPos.y);
            if (!t || t->hp <= 0) {
                pushMsg("NO TARGET.");
                return false;
            }
            if (!isCaptureCandidate(*this, *t)) {
                pushMsg("YOU CAN'T CAPTURE THAT.", MessageKind::Info, true);
                return false;
            }

            const int pct = captureChancePercent(*this, sphereKind, *t);
            const int roll = rng.range(1, 100);

            const EntityKind capturedKind = t->kind;
            uint32_t capturedSeed = t->spriteSeed;
            if (capturedSeed == 0u) capturedSeed = rng.nextU32();

            const int hpPct = (t->hpMax > 0)
                ? clampi((t->hp * 100 + t->hpMax / 2) / t->hpMax, 0, 100)
                : 100;

            // Consume the thrown sphere regardless of the outcome.
            consumeOneSphere(idx);

            if (roll <= pct) {
                // Remove target without killing (no corpse/loot).
                const int capturedId = t->id;
                for (size_t ei = 0; ei < ents.size(); ++ei) {
                    if (ents[ei].id == capturedId) {
                        ents.erase(ents.begin() + static_cast<long>(ei));
                        break;
                    }
                }

                // Create the full sphere.
                Item filled;
                filled.id = nextItemId++;
                filled.kind = captureSphereFilledKind(sphereKind);
                filled.count = 1;
                filled.enchant = static_cast<int>(capturedKind); // stores EntityKind
                filled.spriteSeed = capturedSeed;
                // Starting level scales gently with depth + creature difficulty.
                int startLv = 1 + depth_ / 3;
                startLv += xpFor(capturedKind) / 30;
                startLv = clampi(startLv, 1, captureSpherePetLevelCap());

                filled.charges = packCaptureSphereCharges(/*bond=*/0, hpPct, /*level=*/startLv, /*xp=*/0);
                filled.shopPrice = 0;
                filled.shopDepth = 0;

                std::ostringstream ss;
                ss << "CAPTURED " << petgen::petGivenName(capturedSeed) << " THE " << kindName(capturedKind) << "! (LV " << startLv << ")";

                if (inv.size() >= 26) {
                    dropGroundItemItem(player().pos, filled);
                    ss << " (PACK FULL - DROPPED)";
                } else {
                    inv.push_back(filled);
                }
                pushMsg(ss.str(), MessageKind::Success, true);

                closeTargeting();
                return true;
            }

            // Failed capture: alert the monster.
            t->alerted = true;
            t->lastKnownPlayerPos = player().pos;
            t->lastKnownPlayerAge = 0;

            if (player().effects.hallucinationTurns > 0) {
                pushMsg("THE SPHERE DEMANDS A LAWYER.", MessageKind::Info, true);
            } else {
                std::ostringstream ss;
                ss << "CAPTURE FAILED (" << pct << "%).";
                pushMsg(ss.str(), MessageKind::Info, true);
            }

            closeTargeting();
            return true;
        }

        // ------------------------------------------------------------
        // Full sphere: release the stored companion.
        // ------------------------------------------------------------
        if (isCaptureSphereFullKind(sphereKind)) {
            const int rawKind = sphere.enchant;
            if (rawKind < 0 || rawKind >= ENTITY_KIND_COUNT) {
                pushMsg("THE SPHERE BUZZES UNHAPPILY.", MessageKind::Warning, true);
                // Keep targeting open.
                return false;
            }
            const EntityKind k = static_cast<EntityKind>(rawKind);

            // Ensure the stored seed is non-zero (older saves / corrupted items).
            uint32_t seed = sphere.spriteSeed;
            if (seed == 0u) {
                seed = rng.nextU32();
                sphere.spriteSeed = seed;
            }

            // Prevent duplicates.
            for (const auto& e : ents) {
                if (e.hp <= 0) continue;
                if (!e.friendly) continue;
                if (e.kind == k && e.spriteSeed == seed) {
                    pushMsg("THAT COMPANION IS ALREADY OUT.", MessageKind::Info, true);
                    closeTargeting();
                    return false;
                }
            }

            const int bond = clampi(captureSphereBondFromCharges(sphere.charges), 0, 99);
            const int hpStoredPct = clampi(captureSphereHpPctFromCharges(sphere.charges), 0, 100);
            const int level = clampi(captureSpherePetLevelOrDefault(sphere.charges), 1, captureSpherePetLevelCap());

            Entity m = makeMonster(k, targetPos, /*groupId=*/0, /*allowProcVariant=*/false, /*forcedSpriteSeed=*/seed, /*isGuardian=*/false);
            m.friendly = true;
            m.allyOrder = AllyOrder::Follow;
            m.alerted = false;
            m.lastKnownPlayerPos = player().pos;
            m.lastKnownPlayerAge = 0;

            // Ensure deterministic pet traits (procedural bonuses keyed off spriteSeed).
            ensurePetTraits(m);

            // Pet progression bonuses.
            // 1) Level bonuses: small steady growth.
            m.baseAtk += captureSpherePetAtkBonus(level);
            m.baseDef += captureSpherePetDefBonus(level);
            m.hpMax  += captureSpherePetHpBonus(level);

            // 2) Bond tier bonuses: chunky trust breakpoints.
            const int tier = bond / 25; // 0..3
            if (tier >= 1) m.baseAtk += 1;
            if (tier >= 2) m.baseDef += 1;
            if (tier >= 3) m.hpMax += 3;

            // Restore stored HP% after all max-HP modifiers.
            if (m.hpMax > 0) {
                m.hp = clampi((m.hpMax * hpStoredPct + 50) / 100, 1, m.hpMax);
            }

            ents.push_back(m);

            std::ostringstream ss;
            ss << "YOU RELEASE " << petgen::petGivenName(seed) << " THE " << kindName(k) << " (LV " << level << ").";
            pushMsg(ss.str(), MessageKind::Success, true);

            closeTargeting();
            return true;
        }

        pushMsg("THAT'S NOT A CAPTURE SPHERE.", MessageKind::Warning, true);
        closeTargeting();
        return false;
    }


    // Fishing rod targeting.
    if (targetingMode_ == TargetingMode::Fish) {
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

        // Starting a cast always clears any prior in-progress fishing fight prompt.
        // (UI-only; not serialized)
        fishingFightActive_ = false;
        fishingFightRodItemId_ = 0;
        fishingFightFishSeed_ = 0u;
        fishingFightLabel_.clear();

        const int idx = findItemIndexById(inv, targetingFishingRodItemId_);
        if (idx < 0) {
            pushMsg("YOU DON'T HAVE THAT ANYMORE.", MessageKind::Warning, true);
            closeTargeting();
            return false;
        }

        // Copy the rod up front so we can safely erase it on break.
        const Item rodCopy = inv[static_cast<size_t>(idx)];
        if (!isFishingRodKind(rodCopy.kind)) {
            pushMsg("THAT IS NOT A FISHING ROD.", MessageKind::Warning, true);
            closeTargeting();
            return false;
        }

        const ItemDef& d = itemDef(rodCopy.kind);
        const int maxDur = std::max(0, d.maxCharges);
        int curDur = inv[static_cast<size_t>(idx)].charges;
        if (maxDur > 0 && curDur <= 0) curDur = maxDur;

        if (maxDur > 0 && curDur <= 0) {
            pushMsg("YOUR ROD IS BROKEN.", MessageKind::Warning, true);
            closeTargeting();
            return false;
        }

        // Spend durability for the cast.
        bool rodBroke = false;
        if (maxDur > 0) {
            curDur = std::max(0, curDur - 1);
            inv[static_cast<size_t>(idx)].charges = curDur;
            if (curDur <= 0) rodBroke = true;
        }

        // Casting makes noise (splash/line snap), which can matter for stealth.
        emitNoise(targetPos, 10, false);

        const uint32_t ws = fishWaterSeedAt(*this, targetPos);
        const int turn = static_cast<int>(turns());
        const bool bite = fishgen::isInBiteWindow(ws, turn);
        const float w01 = fishgen::biteWindow01(ws, turn);
        const int density = fishableNeighborhoodCount(*this, targetPos, 2);

        const int pct = fishingChancePercent(*this, rodCopy, targetPos);
        const int roll = rng.range(1, 100);

        if (roll <= pct) {
            const uint32_t casterSeed = (player().spriteSeed != 0u) ? player().spriteSeed : static_cast<uint32_t>(player().id);
            const uint32_t fishSeed = fishgen::fishSeedForCast(ws, turn, casterSeed);

            // Bias rarity a little toward "good" conditions without changing the deterministic seed.
            fishgen::FishRarity baseR = fishgen::rollRarity(fishSeed);
            int r = static_cast<int>(baseR);
            if (w01 > 0.60f) r += 1;
            if (density >= 14) r += 1;
            if (!atCamp()) r += depth_ / 10;
            r = clampi(r, 0, 4);

            int sizeHint = -1;
            if (density >= 16) sizeHint = clampi(8 + (density - 16), 0, 15);

            const fishgen::FishSpec fs = fishgen::makeFish(fishSeed, r, sizeHint, -1);
            const int fishEnchant = packFishEnchant(fs.sizeClass, static_cast<int>(fs.rarity), fs.shiny);

            // Large/rare fish trigger a short reeling interaction instead of an instant reward.
            const bool bigFish = (fs.shiny || fs.rarity >= fishgen::FishRarity::Rare || fs.weight10 >= 55);
            if (bigFish && !rodBroke) {
                // Initialize fight state.
                fishingFightActive_ = true;
                fishingFightRodItemId_ = rodCopy.id;
                fishingFightWaterPos_ = targetPos;
                fishingFightFishSeed_ = fishSeed;
                fishingFightFishEnchant_ = fishEnchant;
                fishingFightFishWeight10_ = fs.weight10;
                fishingFightFishRarity_ = static_cast<int>(fs.rarity);
                fishingFightFishShiny_ = fs.shiny;
                fishingFightProgress_ = 0;
                fishingFightStep_ = 0;

                // Difficulty tuning: safe-band width is widened by focus/agility and narrowed by fish size/rarity.
                int width = 46 + playerFocus() * 4 + playerAgility() * 2;
                width -= (fs.weight10 / 18);
                width -= static_cast<int>(fs.rarity) * 5;
                width = clampi(width, 18, 64);

                fishingFightSafeMin_ = clampi(50 - (width / 2), 6, 80);
                fishingFightSafeMax_ = clampi(50 + (width / 2), 20, 94);
                fishingFightTension_ = clampi((fishingFightSafeMin_ + fishingFightSafeMax_) / 2, 10, 90);

                // Fish pull strength + time pressure.
                fishingFightPull_ = clampi(4 + (fs.weight10 / 25) + static_cast<int>(fs.rarity) * 2, 4, 22);
                fishingFightTurnsLeft_ = clampi(5 + (playerFocus() / 2), 3, 10);

                // UI label (keep it short; reveal the full name on success).
                fishingFightLabel_.clear();
                if (fs.shiny) fishingFightLabel_ += "SHINY ";
                fishingFightLabel_ += fishgen::fishRarityName(fs.rarity);

                if (player().effects.hallucinationTurns > 0) {
                    pushMsg("SOMETHING TUGS AT YOUR LINE...", MessageKind::Info, true);
                } else {
                    pushMsg("FISH ON!", MessageKind::Success, true);
                }
                pushMsg("REEL: ENTER | SLACK: . | LET GO: ESC", MessageKind::System, true);
            } else {
                // Instant catch.
                Item fish;
                fish.id = nextItemId++;
                fish.kind = ItemKind::Fish;
                fish.count = 1;
                fish.spriteSeed = fishSeed;
                fish.charges = static_cast<int>(fishSeed);
                fish.enchant = fishEnchant;

                const std::string fishName = itemDisplayName(fish);

                if (inv.size() >= 26) {
                    dropGroundItemItem(player().pos, fish);
                    pushMsg("YOU REEL IN " + fishName + "! (PACK FULL - DROPPED)", MessageKind::Loot, true);
                } else {
                    inv.push_back(fish);
                    pushMsg("YOU REEL IN " + fishName + "!", MessageKind::Loot, true);
                }

                if (fs.shiny && player().effects.hallucinationTurns == 0) {
                    pushMsg("IT GLITTERS.", MessageKind::Success, true);
                }
            }
        } else {
            if (player().effects.hallucinationTurns > 0) {
                pushMsg("THE WATER LAUGHS BACK.", MessageKind::Info, true);
            } else if (bite) {
                pushMsg("A FISH NIBBLES... THEN SLIPS AWAY.", MessageKind::Info, true);
            } else {
                pushMsg("NO BITE.", MessageKind::Info, true);
            }
        }

        if (rodBroke) {
            // Remove the broken rod.
            inv.erase(inv.begin() + idx);
            invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
            pushMsg("YOUR FISHING ROD SNAPS!", MessageKind::Warning, true);
        }

        closeTargeting();
        return true;
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
    } else if (targetingMode_ == TargetingMode::Capture) {
        const int idx = findItemIndexById(inv, targetingCaptureItemId_);
        if (idx >= 0) {
            range = captureSphereRange(inv[static_cast<size_t>(idx)].kind);
        } else {
            range = 6;
        }
    } else if (targetingMode_ == TargetingMode::Fish) {
        const int idx = findItemIndexById(inv, targetingFishingRodItemId_);
        if (idx >= 0) {
            const Item& rod = inv[static_cast<size_t>(idx)];
            range = std::max(1, itemDef(rod.kind).range);
        } else {
            range = 6;
        }
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
    } else if (targetingMode_ == TargetingMode::Capture) {
        // Capture spheres require a valid sphere item in the inventory.
        const int idx = findItemIndexById(inv, targetingCaptureItemId_);
        if (idx < 0) {
            targetStatusText_ = "NO SPHERE";
            return;
        }
        const Item& sphere = inv[static_cast<size_t>(idx)];
        if (!isCaptureSphereKind(sphere.kind)) {
            targetStatusText_ = "NO SPHERE";
            return;
        }
    } else if (targetingMode_ == TargetingMode::Fish) {
        // Fishing requires a valid fishing rod.
        const int idx = findItemIndexById(inv, targetingFishingRodItemId_);
        if (idx < 0) {
            targetStatusText_ = "NO ROD";
            return;
        }
        const Item& rod = inv[static_cast<size_t>(idx)];
        if (!isFishingRodKind(rod.kind)) {
            targetStatusText_ = "NO ROD";
            return;
        }

        const ItemDef& d = itemDef(rod.kind);
        const int maxDur = std::max(0, d.maxCharges);
        int curDur = rod.charges;
        if (maxDur > 0 && curDur <= 0) curDur = maxDur;
        if (maxDur > 0 && curDur <= 0) {
            targetStatusText_ = "ROD BROKEN";
            return;
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

    // Capture spheres have additional targeting constraints.
    if (targetingMode_ == TargetingMode::Capture) {
        const int idx = findItemIndexById(inv, targetingCaptureItemId_);
        if (idx < 0) {
            targetStatusText_ = "NO SPHERE";
            return;
        }
        const Item& sphere = inv[static_cast<size_t>(idx)];
        const bool releasing = isCaptureSphereFullKind(sphere.kind);

        // Any living entity on the line blocks the thrown sphere.
        for (size_t i = 1; i < targetLine.size(); ++i) {
            const Vec2i p = targetLine[i];
            if (p == targetPos) break;
            if (const Entity* e = entityAt(p.x, p.y)) {
                if (e->hp > 0) {
                    targetStatusText_ = "PATH BLOCKED";
                    return;
                }
            }
        }

        if (releasing) {
            if (!dung.isWalkable(targetPos.x, targetPos.y)) {
                targetStatusText_ = "CAN'T RELEASE THERE";
                return;
            }
            if (const Entity* e = entityAt(targetPos.x, targetPos.y)) {
                if (e->hp > 0 && e->id != player().id) {
                    targetStatusText_ = "SPACE OCCUPIED";
                    return;
                }
            }
        } else {
            const Entity* e = entityAt(targetPos.x, targetPos.y);
            if (!e || e->hp <= 0) {
                targetStatusText_ = "NO TARGET";
                return;
            }
            if (!isCaptureCandidate(*this, *e)) {
                targetStatusText_ = "CAN'T CAPTURE THAT";
                return;
            }
        }
    }

    // Fishing has additional targeting constraints.
    if (targetingMode_ == TargetingMode::Fish) {
        // Must target a fishable tile (fountain water, or overworld basin).
        if (!isFishableTile(*this, targetPos)) {
            targetStatusText_ = "NOT WATER";
            return;
        }

        // Fountains are small: require the player to be nearby.
        if (dung.at(targetPos.x, targetPos.y).type == TileType::Fountain) {
            if (chebyshev(player().pos, targetPos) > 2) {
                targetStatusText_ = "GET CLOSER";
                return;
            }
        }

        // Treat living creatures as line blockers (prevents weird line-through-enemy casts).
        for (size_t i = 1; i < targetLine.size(); ++i) {
            const Vec2i p = targetLine[i];
            if (p == targetPos) break;
            if (const Entity* e = entityAt(p.x, p.y)) {
                if (e->hp > 0) {
                    targetStatusText_ = "PATH BLOCKED";
                    return;
                }
            }
        }

        // Don't cast onto an occupied fountain tile.
        if (dung.at(targetPos.x, targetPos.y).type == TileType::Fountain) {
            if (const Entity* e = entityAt(targetPos.x, targetPos.y)) {
                if (e->hp > 0 && e->id != player().id) {
                    targetStatusText_ = "SPACE OCCUPIED";
                    return;
                }
            }
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
    if (targetingMode_ == TargetingMode::Capture) {
        // Capture spheres are simple projectiles with no AoE; no special safety warnings.
    } else if (targetingMode_ == TargetingMode::Fish) {
        // Fishing does not harm entities.
    } else if (targetingMode_ == TargetingMode::Spell) {
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
