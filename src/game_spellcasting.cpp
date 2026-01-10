#include "game_internal.hpp"

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

    // Spend mana up front for immediate spells.
    mana_ = std::max(0, mana_ - sd.manaCost);

    Entity& p = playerMut();
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
            return true;
        }

        case SpellKind::Stoneskin: {
            const int dur = clampi(12 + focus * 2, 12, 42);
            p.effects.shieldTurns = std::max(p.effects.shieldTurns, dur);
            pushMsg("YOUR SKIN HARDENS LIKE STONE.", MessageKind::Success, true);
            return true;
        }

        case SpellKind::Haste: {
            const int add = clampi(6 + focus / 2, 6, 14);
            p.effects.hasteTurns = std::min(40, p.effects.hasteTurns + add);
            hastePhase = false; // ensure the next action is the "free" haste action
            pushMsg("YOU FEEL QUICK!", MessageKind::Success, true);
            return true;
        }

        case SpellKind::Invisibility: {
            const int add = clampi(14 + focus / 2, 14, 30);
            p.effects.invisTurns = std::min(60, p.effects.invisTurns + add);
            pushMsg("YOU FADE FROM SIGHT!", MessageKind::Success, true);
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
            mana_ = std::max(0, mana_ - sd.manaCost);
            const int atk = spellAtkBonus(*this, k);
            const int dmg = spellDmgBonus(*this, k);
            attackRanged(p, target, sd.range, atk, dmg, ProjectileKind::Spark, /*fromPlayer=*/true, nullptr, /*wandPowered=*/false);
            return true;
        }
        case SpellKind::Fireball: {
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

            mana_ = std::max(0, mana_ - sd.manaCost);
            p.pos = target;
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
