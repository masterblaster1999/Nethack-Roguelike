#include "game_internal.hpp"
#include "combat_rules.hpp"
#include "proc_spells.hpp"

#include <sstream>

void Game::openSpells() {
    // Close other overlays
    targeting = false;
    targetingMode_ = TargetingMode::Ranged;
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    // Close other modal overlays.
    invOpen = false;
    invIdentifyMode = false;
    invEnchantRingMode = false;
    invPrompt_ = InvPromptKind::None;
    closeChestOverlay();

    spellsOpen = true;
    const auto ks = knownSpellsList();
    if (ks.empty()) {
        spellsSel = 0;
    } else {
        spellsSel = clampi(spellsSel, 0, static_cast<int>(ks.size()) - 1);
    }
}

void Game::closeSpells() {
    spellsOpen = false;
}

void Game::moveSpellsSelection(int dy) {
    const auto ks = knownSpellsList();
    if (ks.empty()) {
        spellsSel = 0;
        return;
    }
    spellsSel = clampi(spellsSel + dy, 0, static_cast<int>(ks.size()) - 1);
}

bool Game::canCastSpell(SpellKind k, std::string* reasonOut) const {
    if (!knowsSpell(k)) {
        if (reasonOut) *reasonOut = "YOU DON'T KNOW THAT SPELL";
        return false;
    }
    const SpellDef& sd = spellDef(k);
    if (mana_ < sd.manaCost) {
        if (reasonOut) *reasonOut = "NOT ENOUGH MANA";
        return false;
    }
    return true;
}

int Game::spellFailChancePct(SpellKind k) const {
    if (!knowsSpell(k)) return 95;

    const SpellDef& sd = spellDef(k);

    // A lightweight, NetHack-inspired failure model:
    // - Difficulty scales primarily with mana cost and targeting complexity.
    // - Casting power scales with Focus and character level.
    // - Heavy armor, encumbrance, and mental distortion add penalties.
    int diff = 12 + sd.manaCost * 7;
    if (sd.needsTarget) diff += 6;

    int power = playerCharLevel() * 3 + playerFocus() * 10;
    if (playerClass() == PlayerClass::Wizard) power += 6; // mild class affinity

    int penalty = 0;
    if (const Item* armor = equippedArmor()) {
        switch (armor->kind) {
            case ItemKind::ChainArmor: penalty += 12; break;
            case ItemKind::PlateArmor: penalty += 22; break;
            default: break;
        }
    }

    switch (burdenState()) {
        case BurdenState::Unburdened: break;
        case BurdenState::Burdened:   penalty += 4;  break;
        case BurdenState::Strained:   penalty += 10; break;
        case BurdenState::Overloaded: penalty += 18; break;
        default: break;
    }

    const Entity& p = player();
    if (p.effects.confusionTurns > 0) penalty += 18;
    if (p.effects.hallucinationTurns > 0) penalty += 10;

    int chance = diff - power + penalty;
    return clampi(chance, 0, 95);
}

bool Game::checkSpellFailure(SpellKind k, Vec2i intendedTarget, bool hasTarget) {
    const int chance = spellFailChancePct(k);
    if (chance <= 0) return false;

    const int roll = rng.range(1, 100);
    if (roll > chance) return false;

    const SpellDef& sd = spellDef(k);
    Entity& p = playerMut();

    const int margin = chance - roll;

    // Mana burn: most failures waste about half the cost; severe backfires can waste it all.
    int manaSpent = std::max(1, (sd.manaCost + 1) / 2);
    const bool severe = (margin >= 18) || (chance >= 60 && roll <= chance / 2);
    if (severe) manaSpent = sd.manaCost;
    mana_ = std::max(0, mana_ - manaSpent);

    {
        std::ostringstream ss;
        ss << "YOU MISCAST " << sd.name << "!";
        pushMsg(ss.str(), MessageKind::Warning, true);
    }

    const int focus = std::max(0, playerFocus());
    const int intensity = clampi(22 + sd.manaCost * 10 + focus * 2, 22, 150);

    FXParticlePreset preset = FXParticlePreset::Detect;
    if (k == SpellKind::Blink) preset = FXParticlePreset::Blink;
    if (k == SpellKind::PoisonCloud) preset = FXParticlePreset::Poison;
    pushFxParticle(preset, p.pos, intensity, 0.28f);

    // Miscasts are noisy.
    emitNoise(p.pos, 10 + (severe ? 6 : 0));

    // Special-case: Blink doesn't just fizzle; it becomes imprecise.
    if (k == SpellKind::Blink && hasTarget) {
        const Vec2i src = p.pos;
        Vec2i dst = intendedTarget;
        const int drift = severe ? 3 : 1;
        bool placed = false;
        for (int tries = 0; tries < 24; ++tries) {
            Vec2i cand = dst;
            cand.x += rng.range(-drift, drift);
            cand.y += rng.range(-drift, drift);
            if (!dung.inBounds(cand.x, cand.y)) continue;
            if (!dung.at(cand.x, cand.y).visible) continue;
            if (!dung.isWalkable(cand.x, cand.y)) continue;
            if (Entity* o = entityAtMut(cand.x, cand.y)) {
                if (o->id != p.id && o->hp > 0) continue;
            }
            dst = cand;
            placed = true;
            break;
        }
        if (placed && (dst.x != src.x || dst.y != src.y)) {
            pushFxParticle(FXParticlePreset::Blink, src, intensity, 0.18f);
            p.pos = dst;
            pushFxParticle(FXParticlePreset::Blink, dst, intensity, 0.18f, 0.03f);
            pushMsg("YOUR BLINK GOES AWRY!", MessageKind::Warning, true);
            emitNoise(p.pos, 10);
        }
    }

    // Backlash tiers.
    if (margin >= 12) {
        const int conf = clampi(2 + sd.manaCost / 2 + rng.range(0, 2), 2, 16);
        p.effects.confusionTurns = std::max(p.effects.confusionTurns, conf);
        pushMsg("YOUR MIND REELS.", MessageKind::Warning, true);
    }

    if (margin >= 24) {
        // Catastrophic backfire: damage + element-flavored DOT + occasionally hallucination.
        const int dmg = clampi(1 + sd.manaCost / 3 + rng.range(0, 1), 1, 6);
        p.hp = std::max(0, p.hp - dmg);

        if (k == SpellKind::Fireball) {
            const int burn = clampi(2 + sd.manaCost / 2 + rng.range(0, 2), 2, 14);
            p.effects.burnTurns = std::max(p.effects.burnTurns, burn);
            pushMsg("ARCANE FLAMES SCORCH YOU!", MessageKind::Warning, true);

            // Also seed a tiny fire field around the caster.
            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (fireField_.size() != expect) fireField_.assign(expect, 0u);
            constexpr int r = 1;
            std::vector<uint8_t> mask;
            dung.computeFovMask(p.pos.x, p.pos.y, r, mask);
            const uint8_t base = static_cast<uint8_t>(clampi(14 + sd.manaCost * 2, 14, 32));
            for (int y = std::max(0, p.pos.y - r); y <= std::min(dung.height - 1, p.pos.y + r); ++y) {
                for (int x = std::max(0, p.pos.x - r); x <= std::min(dung.width - 1, p.pos.x + r); ++x) {
                    const int dx = std::abs(x - p.pos.x);
                    const int dy = std::abs(y - p.pos.y);
                    const int dist = std::max(dx, dy);
                    if (dist > r) continue;
                    const size_t i = static_cast<size_t>(y * dung.width + x);
                    if (i >= mask.size() || mask[i] == 0u) continue;
                    if (!dung.isWalkable(x, y)) continue;
                    const int s = static_cast<int>(base) - dist * 6;
                    if (s <= 0) continue;
                    const uint8_t ss = static_cast<uint8_t>(s);
                    if (fireField_[i] < ss) fireField_[i] = ss;
                }
            }
        }

        if (k == SpellKind::PoisonCloud) {
            const int pt = clampi(2 + sd.manaCost / 2 + rng.range(0, 2), 2, 14);
            p.effects.poisonTurns = std::max(p.effects.poisonTurns, pt);
            pushMsg("YOU CHOKE ON TOXIC FUMES!", MessageKind::Warning, true);

            // Also seed a small poison puff at the caster.
            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (poisonGas_.size() != expect) poisonGas_.assign(expect, 0u);
            constexpr int r = 1;
            std::vector<uint8_t> mask;
            dung.computeFovMask(p.pos.x, p.pos.y, r, mask);
            const uint8_t base = static_cast<uint8_t>(clampi(10 + focus / 2, 8, 16));
            for (int y = std::max(0, p.pos.y - r); y <= std::min(dung.height - 1, p.pos.y + r); ++y) {
                for (int x = std::max(0, p.pos.x - r); x <= std::min(dung.width - 1, p.pos.x + r); ++x) {
                    const int dx = std::abs(x - p.pos.x);
                    const int dy = std::abs(y - p.pos.y);
                    const int dist = std::max(dx, dy);
                    if (dist > r) continue;
                    const size_t i = static_cast<size_t>(y * dung.width + x);
                    if (i >= mask.size() || mask[i] == 0u) continue;
                    if (!dung.isWalkable(x, y)) continue;
                    const int s = static_cast<int>(base) - dist * 3;
                    if (s <= 0) continue;
                    const uint8_t ss = static_cast<uint8_t>(s);
                    if (poisonGas_[i] < ss) poisonGas_[i] = ss;
                }
            }
        }

        if (rng.chance(0.25f)) {
            const int hall = clampi(4 + rng.range(0, 8), 4, 24);
            p.effects.hallucinationTurns = std::max(p.effects.hallucinationTurns, hall);
            pushMsg("REALITY WARPES!", MessageKind::Warning, true);
        }

        if (p.hp <= 0) {
            pushMsg("YOU DIE.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = "KILLED BY SPELL MISCAST";
            gameOver = true;
        }
    }

    return true;
}

static int spellAtkBonus(const Game& g, SpellKind k) {
    // Spells scale primarily with Focus; baseAtk provides a small baseline.
    // Keep these conservative: wands remain the "high power" magic.
    int atk = g.player().baseAtk + g.playerFocus();
    switch (k) {
        case SpellKind::MagicMissile: atk += 2; break;
        case SpellKind::Fireball:     atk += 0; break;
        default: break;
    }
    return atk;
}

static int spellDmgBonus(const Game& g, SpellKind k) {
    int bonus = std::max(0, g.playerFocus()) / 2;
    switch (k) {
        case SpellKind::MagicMissile: bonus += 0; break;
        case SpellKind::Fireball:     bonus += 0; break;
        default: break;
    }
    return bonus;
}

bool Game::castSpell(SpellKind k) {
    std::string reason;
    if (!canCastSpell(k, &reason)) {
        if (!reason.empty()) pushMsg(reason + ".", MessageKind::Warning, true);
        return false;
    }

    const SpellDef& sd = spellDef(k);
    if (sd.needsTarget) {
        // Should be handled via beginSpellTargeting().
        pushMsg("THAT SPELL REQUIRES A TARGET.", MessageKind::System, true);
        return false;
    }

    Entity& p = playerMut();

    // Learned spells can miscast; failures still consume the turn.
    if (checkSpellFailure(k, p.pos, /*hasTarget=*/false)) {
        return true;
    }

    // Spend mana for the successful cast.
    mana_ = std::max(0, mana_ - sd.manaCost);

    const int focus = std::max(0, playerFocus());

    switch (k) {
        case SpellKind::MinorHeal: {
            const int before = p.hp;
            const int heal = clampi(4 + focus / 2, 2, 18);
            p.hp = std::min(p.hpMax, p.hp + heal);
            const int gained = p.hp - before;
            if (gained > 0) {
                std::ostringstream ss;
                ss << "YOU FEEL BETTER (" << gained << ").";
                pushMsg(ss.str(), MessageKind::Success, true);
                const int intensity = clampi(14 + gained * 4 + focus * 2, 14, 90);
                pushFxParticle(FXParticlePreset::Heal, p.pos, intensity, 0.22f);
            } else {
                pushMsg("YOU FEEL NO DIFFERENT.", MessageKind::System, true);
            }
            return true;
        }

        case SpellKind::DetectTraps: {
            // Reveal traps in a modest radius around the player.
            const int radius = clampi(6 + focus / 2, 6, 12);
            int found = 0;
            for (auto& tr : trapsCur) {
                const int dist = chebyshev(p.pos, tr.pos);
                if (dist > radius) continue;
                if (!tr.discovered) {
                    tr.discovered = true;
                    ++found;
                }
                if (dung.inBounds(tr.pos.x, tr.pos.y)) {
                    dung.at(tr.pos.x, tr.pos.y).explored = true;
                }
            }
            if (found > 0) {
                pushMsg("YOU SENSE NEARBY TRAPS!", MessageKind::ImportantMsg, true);
            } else {
                pushMsg("YOU SENSE NO TRAPS.", MessageKind::System, true);
            }
            const int intensity = clampi(18 + found * 6 + focus * 2, 18, 120);
            pushFxParticle(FXParticlePreset::Detect, p.pos, intensity, 0.30f);
            return true;
        }

        case SpellKind::Stoneskin: {
            const int dur = clampi(12 + focus * 2, 12, 42);
            p.effects.shieldTurns = std::max(p.effects.shieldTurns, dur);
            pushMsg("YOUR SKIN HARDENS LIKE STONE.", MessageKind::Success, true);
            const int intensity = clampi(18 + focus * 3, 18, 120);
            pushFxParticle(FXParticlePreset::Buff, p.pos, intensity, 0.35f);
            return true;
        }

        case SpellKind::Haste: {
            const int add = clampi(6 + focus / 2, 6, 14);
            p.effects.hasteTurns = std::min(40, p.effects.hasteTurns + add);
            hastePhase = false; // ensure the next action is the "free" haste action
            pushMsg("YOU FEEL QUICK!", MessageKind::Success, true);
            const int intensity = clampi(16 + focus * 2, 16, 90);
            pushFxParticle(FXParticlePreset::Buff, p.pos, intensity, 0.25f);
            return true;
        }

        case SpellKind::Invisibility: {
            const int add = clampi(14 + focus / 2, 14, 30);
            p.effects.invisTurns = std::min(60, p.effects.invisTurns + add);
            pushMsg("YOU FADE FROM SIGHT!", MessageKind::Success, true);
            const int intensity = clampi(20 + focus * 3, 20, 140);
            pushFxParticle(FXParticlePreset::Invisibility, p.pos, intensity, 0.40f);
            return true;
        }

        case SpellKind::MagicMissile:
        case SpellKind::Blink:
        case SpellKind::Fireball:
        case SpellKind::PoisonCloud:
        default:
            // Safety: if we somehow get here, do nothing (mana already spent).
            pushMsg("NOTHING HAPPENS.", MessageKind::System, true);
            return true;
    }
}

bool Game::castSpellAt(SpellKind k, Vec2i target) {
    std::string reason;
    if (!canCastSpell(k, &reason)) {
        if (!reason.empty()) pushMsg(reason + ".", MessageKind::Warning, true);
        return false;
    }

    const SpellDef& sd = spellDef(k);
    if (!sd.needsTarget) {
        // Route to the no-target path.
        return castSpell(k);
    }

    Entity& p = playerMut();

    // Targeted spells only spend mana if the cast is actually executed.
    switch (k) {
        case SpellKind::MagicMissile: {
            if (checkSpellFailure(k, target, /*hasTarget=*/true)) return true;
            mana_ = std::max(0, mana_ - sd.manaCost);
            const int atk = spellAtkBonus(*this, k);
            const int dmg = spellDmgBonus(*this, k);
            attackRanged(p, target, sd.range, atk, dmg, ProjectileKind::Spark, /*fromPlayer=*/true, nullptr, /*wandPowered=*/false);
            return true;
        }
        case SpellKind::Fireball: {
            if (checkSpellFailure(k, target, /*hasTarget=*/true)) return true;
            mana_ = std::max(0, mana_ - sd.manaCost);
            const int atk = spellAtkBonus(*this, k);
            const int dmg = spellDmgBonus(*this, k);
            attackRanged(p, target, sd.range, atk, dmg, ProjectileKind::Fireball, /*fromPlayer=*/true, nullptr, /*wandPowered=*/false);
            return true;
        }
        case SpellKind::Blink: {
            // Blink is a targeted teleport to a visible, walkable tile.
            if (!dung.inBounds(target.x, target.y)) {
                pushMsg("OUT OF BOUNDS.", MessageKind::System, true);
                return false;
            }
            if (!dung.at(target.x, target.y).visible) {
                pushMsg("TARGET NOT VISIBLE.", MessageKind::System, true);
                return false;
            }
            if (!dung.isWalkable(target.x, target.y)) {
                pushMsg("YOU CAN'T BLINK THERE.", MessageKind::System, true);
                return false;
            }
            if (Entity* o = entityAtMut(target.x, target.y)) {
                if (o->id != p.id && o->hp > 0) {
                    pushMsg("THAT SPACE IS OCCUPIED.", MessageKind::System, true);
                    return false;
                }
            }

            if (checkSpellFailure(k, target, /*hasTarget=*/true)) return true;

            mana_ = std::max(0, mana_ - sd.manaCost);
            const int focus = std::max(0, playerFocus());
            const int intensity = clampi(30 + focus * 2, 30, 90);
            const Vec2i src = p.pos;
            pushFxParticle(FXParticlePreset::Blink, src, intensity, 0.18f);
            p.pos = target;
            pushFxParticle(FXParticlePreset::Blink, target, intensity, 0.18f, 0.03f);
            pushMsg("YOU BLINK.", MessageKind::System, true);
            emitNoise(p.pos, 10);
            return true;
        }

        case SpellKind::PoisonCloud: {
            // Conjure a lingering poison gas field.
            // The environmental tick will apply poison to anything standing in it.
            if (!dung.inBounds(target.x, target.y)) {
                pushMsg("OUT OF BOUNDS.", MessageKind::System, true);
                return false;
            }
            if (!dung.at(target.x, target.y).visible) {
                pushMsg("TARGET NOT VISIBLE.", MessageKind::System, true);
                return false;
            }
            if (!dung.isWalkable(target.x, target.y)) {
                pushMsg("THAT TILE CAN'T HOLD A CLOUD.", MessageKind::System, true);
                return false;
            }

            if (checkSpellFailure(k, target, /*hasTarget=*/true)) return true;

            mana_ = std::max(0, mana_ - sd.manaCost);

            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (poisonGas_.size() != expect) poisonGas_.assign(expect, 0u);

            const int focus = std::max(0, playerFocus());
            const uint8_t baseStrength = static_cast<uint8_t>(clampi(10 + focus / 2, 8, 18));
            constexpr int radius = 2;

            std::vector<uint8_t> mask;
            dung.computeFovMask(target.x, target.y, radius, mask);

            const int minX = std::max(0, target.x - radius);
            const int maxX = std::min(dung.width - 1, target.x + radius);
            const int minY = std::max(0, target.y - radius);
            const int maxY = std::min(dung.height - 1, target.y + radius);

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const int dx = std::abs(x - target.x);
                    const int dy = std::abs(y - target.y);
                    const int dist = std::max(dx, dy);
                    if (dist > radius) continue;

                    const size_t i = static_cast<size_t>(y * dung.width + x);
                    if (i >= mask.size()) continue;
                    if (mask[i] == 0u) continue;
                    if (!dung.isWalkable(x, y)) continue;

                    const int s = static_cast<int>(baseStrength) - dist * 2;
                    if (s <= 0) continue;
                    const uint8_t ss = static_cast<uint8_t>(s);
                    if (poisonGas_[i] < ss) poisonGas_[i] = ss;
                }
            }

            pushMsg("A CLOUD OF TOXIC VAPOR BLOOMS.", MessageKind::Warning, true);
            emitNoise(target, 8);
            const int intensity = clampi(static_cast<int>(baseStrength) * 6, 24, 140);
            pushFxParticle(FXParticlePreset::Poison, target, intensity, 0.50f);
            return true;
        }

        case SpellKind::MinorHeal:
        case SpellKind::DetectTraps:
        case SpellKind::Stoneskin:
        case SpellKind::Haste:
        case SpellKind::Invisibility:
        default:
            return castSpell(k);
    }
}


// ------------------------------------------------------------
// Procedural Rune Tablet spellcasting (procedural spells)
// ------------------------------------------------------------

namespace {
static bool hasMod(uint8_t mask, ProcSpellMod m) {
    return (mask & static_cast<uint8_t>(m)) != 0u;
}

static int procExpectedDamageMeanTimes2(const ProcSpell& ps) {
    // Returns 2x expected mean damage (integer) to avoid floats:
    // E[NdS + B] = N*(S+1)/2 + B
    const int n = std::max(0, ps.damageDiceCount);
    const int s = std::max(0, ps.damageDiceSides);
    const int b = ps.damageFlat;
    // 2 * mean = n*(s+1) + 2*b
    return n * (s + 1) + 2 * b;
}
} // namespace

bool Game::canCastProcSpell(uint32_t procId, std::string* reasonOut) const {
    const ProcSpell ps = generateProcSpell(procId);

    // NOTE: proc spell ids are deterministic; treat missing/zero ids as a valid (tier-clamped) spell.
    const int cost = std::max(0, ps.manaCost);
    if (mana_ < cost) {
        if (reasonOut) *reasonOut = "NOT ENOUGH MANA";
        return false;
    }
    return true;
}

bool Game::castProcSpell(uint32_t procId) {
    std::string reason;
    if (!canCastProcSpell(procId, &reason)) {
        if (!reason.empty()) pushMsg(reason + ".", MessageKind::Warning, true);
        return false;
    }

    const ProcSpell ps = generateProcSpell(procId);
    if (ps.needsTarget) {
        pushMsg("THAT RUNE SPELL NEEDS A TARGET.", MessageKind::System, true);
        return false;
    }

    Entity& p = playerMut();

    // Spend mana on a successful cast.
    mana_ = std::max(0, mana_ - std::max(0, ps.manaCost));

    const int focus = std::max(0, playerFocus());
    int turns = std::max(1, ps.durationTurns);
    if (hasMod(ps.mods, ProcSpellMod_Focused)) turns += 2;
    if (hasMod(ps.mods, ProcSpellMod_Lingering)) turns += std::max(1, turns / 2);
    turns = clampi(turns, 1, 120);

    // Only Wards are currently no-target in the proc generator.
    // Map elements to existing effect buckets.
    switch (ps.element) {
        case ProcSpellElement::Shock: {
            p.effects.hasteTurns = std::max(p.effects.hasteTurns, turns);
            pushMsg("RUNES SURGE THROUGH YOU. YOU FEEL FASTER.", MessageKind::Info, true);
            pushFxParticle(FXParticlePreset::Buff, p.pos, 18 + focus * 2, 0.35f);
            break;
        }
        case ProcSpellElement::Wind: {
            p.effects.levitationTurns = std::max(p.effects.levitationTurns, turns);
            pushMsg("THE RUNES LIFT YOU. YOU BEGIN TO FLOAT.", MessageKind::Info, true);
            pushFxParticle(FXParticlePreset::Buff, p.pos, 18 + focus * 2, 0.35f);
            break;
        }
        case ProcSpellElement::Shadow: {
            p.effects.invisTurns = std::max(p.effects.invisTurns, turns);
            pushMsg("SHADOW RUNES VEIL YOU FROM SIGHT.", MessageKind::Info, true);
            pushFxParticle(FXParticlePreset::Invisibility, p.pos, 20 + focus * 2, 0.40f);
            break;
        }
        case ProcSpellElement::Radiance: {
            p.effects.visionTurns = std::max(p.effects.visionTurns, turns);
            pushMsg("RADIANT RUNES SHARPEN YOUR SENSES.", MessageKind::Info, true);
            pushFxParticle(FXParticlePreset::Detect, p.pos, 18 + focus * 2, 0.35f);
            break;
        }
        case ProcSpellElement::Blood: {
            p.effects.regenTurns = std::max(p.effects.regenTurns, turns);
            pushMsg("BLOOD RUNES THRUM. YOU FEEL YOUR WOUNDS KNIT.", MessageKind::Info, true);
            pushFxParticle(FXParticlePreset::Heal, p.pos, 20 + focus * 2, 0.35f);
            break;
        }
        case ProcSpellElement::Stone:
        case ProcSpellElement::Frost:
        case ProcSpellElement::Arcane:
        case ProcSpellElement::Venom:
        case ProcSpellElement::Fire:
        default: {
            // Default ward: a stoneskin-style shield.
            p.effects.shieldTurns = std::max(p.effects.shieldTurns, turns);
            pushMsg("RUNES HARDEN AROUND YOU LIKE STONE.", MessageKind::Info, true);
            pushFxParticle(FXParticlePreset::Buff, p.pos, 18 + focus * 2, 0.35f);
            break;
        }
    }

    if (ps.noise > 0) emitNoise(p.pos, ps.noise);

    // Volatile wards can backfire, briefly burning the caster.
    if (hasMod(ps.mods, ProcSpellMod_Volatile)) {
        float chance = 0.12f + 0.01f * static_cast<float>(ps.tier);
        chance = std::clamp(chance, 0.0f, 0.35f);
        if (rng.chance(chance)) {
            const int burn = clampi(2 + ps.tier / 3 + rng.range(0, 2), 2, 10);
            const int before = p.effects.burnTurns;
            p.effects.burnTurns = std::max(p.effects.burnTurns, burn);
            if (before == 0 && p.effects.burnTurns > 0) {
                pushMsg("THE RUNES BACKFIRE! YOU CATCH FIRE.", MessageKind::Warning, true);
            }
            if (p.hp > 0) p.hp = std::max(0, p.hp - 1);
            if (p.hp <= 0) {
                pushMsg("YOU DIE.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = "KILLED BY RUNE BACKFIRE";
                gameOver = true;
            }
        }
    }

    return true;
}

bool Game::castProcSpellAt(uint32_t procId, Vec2i target) {
    std::string reason;
    if (!canCastProcSpell(procId, &reason)) {
        if (!reason.empty()) pushMsg(reason + ".", MessageKind::Warning, true);
        return false;
    }

    const ProcSpell ps = generateProcSpell(procId);
    if (!ps.needsTarget) {
        // Route to the no-target path.
        return castProcSpell(procId);
    }

    Entity& p = playerMut();

    if (!dung.inBounds(target.x, target.y)) {
        pushMsg("OUT OF BOUNDS.", MessageKind::System, true);
        return false;
    }
    if (!dung.at(target.x, target.y).visible) {
        pushMsg("TARGET NOT VISIBLE.", MessageKind::System, true);
        return false;
    }

    const int dist = std::max(std::abs(target.x - p.pos.x), std::abs(target.y - p.pos.y));
    if (ps.range > 0 && dist > ps.range) {
        pushMsg("OUT OF RANGE.", MessageKind::System, true);
        return false;
    }

    // Wild spells can drift off-target slightly.
    Vec2i finalTarget = target;
    if (hasMod(ps.mods, ProcSpellMod_Wild)) {
        Vec2i drifted = finalTarget;
        drifted.x += rng.range(-1, 1);
        drifted.y += rng.range(-1, 1);
        if (dung.inBounds(drifted.x, drifted.y) && dung.at(drifted.x, drifted.y).visible) {
            finalTarget = drifted;
        }
    }

    // Some forms have special target constraints.
    if (ps.form == ProcSpellForm::Cloud) {
        if (!dung.isWalkable(finalTarget.x, finalTarget.y)) {
            pushMsg("THAT TILE CAN'T HOLD A CLOUD.", MessageKind::System, true);
            return false;
        }
    }
    if (ps.form == ProcSpellForm::Hex) {
        const Entity* e = entityAt(finalTarget.x, finalTarget.y);
        if (!e || e->hp <= 0 || e->id == p.id) {
            pushMsg("NO TARGET.", MessageKind::System, true);
            return false;
        }
    }

    // Spend mana on a successful cast.
    mana_ = std::max(0, mana_ - std::max(0, ps.manaCost));
    if (ps.noise > 0) emitNoise(finalTarget, ps.noise);

    const int focus = std::max(0, playerFocus());
    const int statBonus = statDamageBonusFromAtk(p.baseAtk);

    // Approximate the proc spell's damage expression by mapping it to a bonus over an existing projectile.
    auto computeDmgBonus = [&](ProjectileKind proj) -> int {
        const DiceExpr base = rangedDiceForProjectile(proj, /*wandPowered=*/false);
        // Convert expected means to 2x integer space to avoid float drift.
        const int baseMean2 = base.count * (base.sides + 1) + 2 * base.bonus;
        const int procMean2 = procExpectedDamageMeanTimes2(ps);

        // Desired bonus in 2x space:
        // (baseMean2 + 2*dmgBonus + 2*statBonus) ~= procMean2
        int want2 = procMean2 - baseMean2 - 2 * statBonus;

        // Bias slightly with focus so proc spells scale with progression.
        want2 += focus;

        int bonus = want2 / 2;
        bonus = clampi(bonus, -10, 60);
        return bonus;
    };

    auto computeAtkBonus = [&]() -> int {
        int atk = p.baseAtk + focus + std::max(0, static_cast<int>(ps.tier) / 2);
        if (hasMod(ps.mods, ProcSpellMod_Focused)) atk += 2;
        if (hasMod(ps.mods, ProcSpellMod_Wild)) atk -= 1;
        if (p.effects.confusionTurns > 0) atk -= 3;
        return atk;
    };

    switch (ps.form) {
        case ProcSpellForm::Bolt:
        case ProcSpellForm::Beam: {
            const int atk = computeAtkBonus();
            const int dmg = computeDmgBonus(ProjectileKind::Spark);
            attackRanged(p, finalTarget, ps.range, atk, dmg, ProjectileKind::Spark, true, nullptr, false);
            return true;
        }

        case ProcSpellForm::Echo: {
            const int atk = computeAtkBonus();
            const int dmg = computeDmgBonus(ProjectileKind::Spark);
            attackRanged(p, finalTarget, ps.range, atk, dmg, ProjectileKind::Spark, true, nullptr, false);

            // Echo: a second, weaker follow-up bolt (no extra mana).
            const int atk2 = atk - 1;
            const int dmg2 = dmg - 2;
            if (atk2 > 0) {
                attackRanged(p, finalTarget, ps.range, atk2, dmg2, ProjectileKind::Spark, true, nullptr, false);
            }
            return true;
        }

        case ProcSpellForm::Burst: {
            // Use the existing Fireball projectile + explosion logic (radius 1).
            const int atk = computeAtkBonus();
            const int dmg = computeDmgBonus(ProjectileKind::Fireball);
            attackRanged(p, finalTarget, ps.range, atk, dmg, ProjectileKind::Fireball, true, nullptr, false);
            return true;
        }

        case ProcSpellForm::Cloud: {
            // Seed one of the existing environmental fields.
            std::vector<uint8_t>* field = &poisonGas_;
            FXParticlePreset preset = FXParticlePreset::Poison;
            const char* msg = "A CLOUD OF TOXIC VAPOR BLOOMS.";

            if (ps.element == ProcSpellElement::Shadow || ps.element == ProcSpellElement::Arcane) {
                field = &confusionGas_;
                preset = FXParticlePreset::Detect;
                msg = "A CLOUD OF DIZZYING MIASMA BLOOMS.";
            } else if (ps.element == ProcSpellElement::Stone) {
                field = &corrosiveGas_;
                preset = FXParticlePreset::Dig;
                msg = "A CLOUD OF CORROSIVE FUMES BLOOMS.";
            } else if (ps.element == ProcSpellElement::Fire) {
                field = &fireField_;
                preset = FXParticlePreset::Buff;
                msg = "THE AIR IGNITES WITH RUNIC FIRE.";
            } else if (ps.element == ProcSpellElement::Venom) {
                field = &poisonGas_;
                preset = FXParticlePreset::Poison;
                msg = "A CLOUD OF TOXIC VAPOR BLOOMS.";
            }

            const size_t expect = static_cast<size_t>(dung.width * dung.height);
            if (field->size() != expect) field->assign(expect, 0u);

            int radius = std::max(1, ps.aoeRadius);
            radius = clampi(radius, 1, 4);

            int baseStrength = clampi(8 + static_cast<int>(ps.tier) + focus / 4, 8, 22);
            if (hasMod(ps.mods, ProcSpellMod_Lingering)) baseStrength += 2;
            baseStrength = clampi(baseStrength, 6, 30);

            std::vector<uint8_t> mask;
            dung.computeFovMask(finalTarget.x, finalTarget.y, radius, mask);

            const int minX = std::max(0, finalTarget.x - radius);
            const int maxX = std::min(dung.width - 1, finalTarget.x + radius);
            const int minY = std::max(0, finalTarget.y - radius);
            const int maxY = std::min(dung.height - 1, finalTarget.y + radius);

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const int dx = std::abs(x - finalTarget.x);
                    const int dy = std::abs(y - finalTarget.y);
                    const int cd = std::max(dx, dy);
                    if (cd > radius) continue;

                    const size_t i = static_cast<size_t>(y * dung.width + x);
                    if (i >= mask.size()) continue;
                    if (mask[i] == 0u) continue;
                    if (!dung.isWalkable(x, y)) continue;

                    const int s = baseStrength - cd * 2;
                    if (s <= 0) continue;
                    const uint8_t ss = static_cast<uint8_t>(std::min(255, s));
                    if ((*field)[i] < ss) (*field)[i] = ss;
                }
            }

            pushMsg(msg, MessageKind::Warning, true);
            const int intensity = clampi(baseStrength * 6, 24, 140);
            pushFxParticle(preset, finalTarget, intensity, 0.50f);
            return true;
        }

        case ProcSpellForm::Hex: {
            // Apply a single-target debuff based on element.
            Entity* e = entityAtMut(finalTarget.x, finalTarget.y);
            if (!e || e->hp <= 0) {
                pushMsg("NO TARGET.", MessageKind::System, true);
                return false;
            }

            int turns = std::max(1, ps.durationTurns);
            if (hasMod(ps.mods, ProcSpellMod_Focused)) turns += 2;
            if (hasMod(ps.mods, ProcSpellMod_Lingering)) turns += std::max(1, turns / 2);
            turns = clampi(turns, 1, 120);

            const bool vis = dung.at(finalTarget.x, finalTarget.y).visible;
            auto msgTarget = [&](const char* verb) {
                if (e->kind == EntityKind::Player) return;
                if (!vis) return;
                std::ostringstream ss;
                ss << kindName(e->kind) << " " << verb << ".";
                pushMsg(ss.str(), MessageKind::Info, true);
            };

            switch (ps.element) {
                case ProcSpellElement::Venom:
                case ProcSpellElement::Blood:
                    e->effects.poisonTurns = std::max(e->effects.poisonTurns, turns);
                    msgTarget("IS POISONED");
                    pushFxParticle(FXParticlePreset::Poison, finalTarget, 16, 0.40f);
                    break;
                case ProcSpellElement::Fire:
                    e->effects.burnTurns = std::max(e->effects.burnTurns, turns);
                    msgTarget("CATCHES FIRE");
                    pushFxParticle(FXParticlePreset::Buff, finalTarget, 14, 0.35f);
                    break;
                case ProcSpellElement::Stone:
                    e->effects.corrosionTurns = std::max(e->effects.corrosionTurns, turns);
                    msgTarget("IS CORRODED");
                    pushFxParticle(FXParticlePreset::Dig, finalTarget, 12, 0.35f);
                    break;
                case ProcSpellElement::Frost:
                    e->effects.webTurns = std::max(e->effects.webTurns, turns);
                    msgTarget("IS ENTANGLED");
                    pushFxParticle(FXParticlePreset::Buff, finalTarget, 12, 0.35f);
                    break;
                case ProcSpellElement::Wind:
                case ProcSpellElement::Shock:
                    e->effects.confusionTurns = std::max(e->effects.confusionTurns, turns);
                    msgTarget("LOOKS DISORIENTED");
                    pushFxParticle(FXParticlePreset::Detect, finalTarget, 12, 0.35f);
                    break;
                case ProcSpellElement::Shadow:
                case ProcSpellElement::Arcane:
                case ProcSpellElement::Radiance:
                default:
                    e->effects.fearTurns = std::max(e->effects.fearTurns, turns);
                    msgTarget("TREMBLES");
                    pushFxParticle(FXParticlePreset::Detect, finalTarget, 12, 0.35f);
                    break;
            }

            return true;
        }

        case ProcSpellForm::Ward:
        default:
            // Should not happen (ward spells are no-target), but keep the system robust.
            return castProcSpell(procId);
    }
}
