#pragma once

// Procedural rune-spell generator.
//
// This module defines a deterministic (replay-safe) procedural spell spec derived
// from a packed 32-bit id.
//
// Design goals:
//   - Deterministic: the same id always produces the same spell.
//   - Compact ids: tier + seed packed into a u32 for easy storage in items/saves.
//   - No dependency on the global RNG stream.
//   - Self-contained: integration into spellcasting/items can be layered on later.

#include "common.hpp"
#include "rng.hpp"

#include <array>
#include <cstdint>
#include <string>

enum class ProcSpellElement : uint8_t {
    Fire = 0,
    Frost,
    Shock,
    Venom,
    Shadow,
    Radiance,
    Arcane,
    Stone,
    Wind,
    Blood,
};

enum class ProcSpellForm : uint8_t {
    Bolt = 0,
    Beam,
    Burst,
    Cloud,
    Hex,
    Ward,
    Echo,
};

enum ProcSpellMod : uint8_t {
    ProcSpellMod_Focused   = 1u << 0,
    ProcSpellMod_Lingering = 1u << 1,
    ProcSpellMod_Volatile  = 1u << 2,
    ProcSpellMod_Wild      = 1u << 3,
    ProcSpellMod_Echoing   = 1u << 4,
};

struct ProcSpell {
    uint32_t id = 0;
    uint8_t tier = 1;              // 1..15
    ProcSpellElement element{};
    ProcSpellForm form{};
    uint8_t mods = 0;

    // Core gameplay knobs (integration will decide how these map to game mechanics).
    int manaCost = 0;
    int range = 0;                 // 0 = self/ambient
    bool needsTarget = false;

    // Common proc spell parameters.
    int aoeRadius = 0;             // 0 = none
    int durationTurns = 0;         // 0 = instant
    int damageDiceCount = 0;       // 0 = non-damaging
    int damageDiceSides = 0;
    int damageFlat = 0;
    int noise = 0;

    // UI/text.
    std::string name;              // uppercase
    std::string runeSigil;         // e.g. "KAR-THO-RAI"
    std::string description;       // uppercase (single paragraph)
    std::string tags;              // uppercase, comma-separated
};

// -----------------------------------------------------------------------------
// Id packing helpers
// -----------------------------------------------------------------------------

inline constexpr uint32_t PROC_SPELL_SEED_MASK = 0x0FFFFFFFu;

inline uint32_t makeProcSpellId(uint8_t tier, uint32_t seed28) {
    const uint32_t t = (static_cast<uint32_t>(tier) & 0xFu) << 28;
    const uint32_t s = (seed28 & PROC_SPELL_SEED_MASK);
    return t | s;
}

inline uint8_t procSpellTier(uint32_t id) {
    return static_cast<uint8_t>((id >> 28) & 0xFu);
}

inline uint32_t procSpellSeed(uint32_t id) {
    return id & PROC_SPELL_SEED_MASK;
}

inline uint8_t procSpellTierClamped(uint32_t id) {
    uint8_t t = procSpellTier(id);
    if (t == 0) t = 1;
    if (t > 15) t = 15;
    return t;
}

// -----------------------------------------------------------------------------
// Text helpers
// -----------------------------------------------------------------------------

inline const char* procSpellElementName(ProcSpellElement e) {
    switch (e) {
    case ProcSpellElement::Fire: return "FIRE";
    case ProcSpellElement::Frost: return "FROST";
    case ProcSpellElement::Shock: return "SHOCK";
    case ProcSpellElement::Venom: return "VENOM";
    case ProcSpellElement::Shadow: return "SHADOW";
    case ProcSpellElement::Radiance: return "RADIANCE";
    case ProcSpellElement::Arcane: return "ARCANE";
    case ProcSpellElement::Stone: return "STONE";
    case ProcSpellElement::Wind: return "WIND";
    case ProcSpellElement::Blood: return "BLOOD";
    default: return "ARCANE";
    }
}

inline const char* procSpellFormName(ProcSpellForm f) {
    switch (f) {
    case ProcSpellForm::Bolt: return "BOLT";
    case ProcSpellForm::Beam: return "BEAM";
    case ProcSpellForm::Burst: return "BURST";
    case ProcSpellForm::Cloud: return "CLOUD";
    case ProcSpellForm::Hex: return "HEX";
    case ProcSpellForm::Ward: return "WARD";
    case ProcSpellForm::Echo: return "ECHO";
    default: return "BOLT";
    }
}

inline std::string procSpellModsToTags(uint8_t mods) {
    std::string out;
    auto add = [&](const char* t) {
        if (!out.empty()) out += ", ";
        out += t;
    };
    if (mods & ProcSpellMod_Focused) add("FOCUSED");
    if (mods & ProcSpellMod_Lingering) add("LINGERING");
    if (mods & ProcSpellMod_Volatile) add("VOLATILE");
    if (mods & ProcSpellMod_Wild) add("WILD");
    if (mods & ProcSpellMod_Echoing) add("ECHOING");
    return out;
}

// -----------------------------------------------------------------------------
// Deterministic generation
// -----------------------------------------------------------------------------

namespace detail_procspells {

struct WordBank {
    std::array<const char*, 8> adjs{};
    std::array<const char*, 8> nouns{};
    uint8_t nAdjs = 0;
    uint8_t nNouns = 0;
};

inline constexpr WordBank elementWords(ProcSpellElement e) {
    switch (e) {
    case ProcSpellElement::Fire:
        return {{"EMBER", "CINDER", "PYRIC", "INFERNAL", "SCORCHED", "SMOLDERING", "", ""},
                {"PYRE", "ASH", "FLAME", "BRAND", "COAL", "SUNSPARK", "", ""}, 6, 6};
    case ProcSpellElement::Frost:
        return {{"RIME", "GLACIAL", "FRIGID", "ICEBOUND", "WINTER", "PALE", "", ""},
                {"FROST", "ICE", "HOAR", "SNOW", "SHARD", "SLEET", "", ""}, 6, 6};
    case ProcSpellElement::Shock:
        return {{"STATIC", "THUNDER", "STORM", "SPARKING", "VOLT", "SKYFORGED", "", ""},
                {"BOLT", "STORM", "ARC", "SURGE", "STRIKE", "GROUNDFIRE", "", ""}, 6, 6};
    case ProcSpellElement::Venom:
        return {{"TOXIC", "VENOMOUS", "FETID", "NOXIOUS", "GREEN", "BLIGHTED", "", ""},
                {"MIASMA", "POISON", "SPITTLE", "BLIGHT", "ICHOR", "FUME", "", ""}, 6, 6};
    case ProcSpellElement::Shadow:
        return {{"UMBRAL", "GLOOM", "ECLIPSED", "DUSK", "MIDNIGHT", "HOLLOW", "", ""},
                {"SHADE", "ECLIPSE", "NIGHT", "VEIL", "VOID", "SILENCE", "", ""}, 6, 6};
    case ProcSpellElement::Radiance:
        return {{"HALOED", "DAWN", "SOLAR", "LUMINOUS", "GILDED", "BRIGHT", "", ""},
                {"HALO", "SUN", "AURORA", "GLORY", "RAY", "SIGN", "", ""}, 6, 6};
    case ProcSpellElement::Arcane:
        return {{"ARCANE", "AETHERIC", "RUNED", "SIGILED", "ELDRITCH", "MYSTIC", "", ""},
                {"SIGIL", "RUNE", "MANA", "THREAD", "GLYPH", "PATTERN", "", ""}, 6, 6};
    case ProcSpellElement::Stone:
        return {{"BASALT", "STONE", "IRON", "OBSIDIAN", "GRANITE", "DENSE", "", ""},
                {"SPIKE", "SLAB", "SHARD", "WALL", "SHELL", "PILLAR", "", ""}, 6, 6};
    case ProcSpellElement::Wind:
        return {{"GALE", "WHIRLING", "CUTTING", "SKIRLING", "SWIFT", "CYCLONIC", "", ""},
                {"GUST", "TEMPEST", "ZEphyr", "EDGE", "SCREAM", "CURRENT", "", ""}, 6, 6};
    case ProcSpellElement::Blood:
        return {{"SANGUINE", "CRIMSON", "RUSTED", "HEARTBOUND", "FERVID", "SCARLET", "", ""},
                {"BLOOD", "OATH", "VEIN", "PULSE", "WOUND", "THIRST", "", ""}, 6, 6};
    default:
        return {{"ARCANE", "", "", "", "", "", "", ""},
                {"RUNE", "", "", "", "", "", "", ""}, 1, 1};
    }
}

inline constexpr std::array<const char*, 4> formSynonyms(ProcSpellForm f, int which) {
    // Return a small set of synonyms (4 fixed slots). The caller chooses one.
    (void)which;
    switch (f) {
    case ProcSpellForm::Bolt:  return {"BOLT", "LANCE", "DART", "SPIKE"};
    case ProcSpellForm::Beam:  return {"BEAM", "RAY", "LINE", "LASH"};
    case ProcSpellForm::Burst: return {"BURST", "NOVA", "BLAST", "SHOCKWAVE"};
    case ProcSpellForm::Cloud: return {"CLOUD", "FOG", "MIASMA", "HAAZE"};
    case ProcSpellForm::Hex:   return {"HEX", "CURSE", "MARK", "BRAND"};
    case ProcSpellForm::Ward:  return {"WARD", "AEGIS", "BARRIER", "SIGIL"};
    case ProcSpellForm::Echo:  return {"ECHO", "CALL", "REVERB", "CHIME"};
    default:                   return {"BOLT", "LANCE", "DART", "SPIKE"};
    }
}

inline const char* pickAdj(const WordBank& wb, RNG& rng) {
    const int n = wb.nAdjs ? wb.nAdjs : 1;
    return wb.adjs[static_cast<size_t>(rng.range(0, n - 1))];
}

inline const char* pickNoun(const WordBank& wb, RNG& rng) {
    const int n = wb.nNouns ? wb.nNouns : 1;
    return wb.nouns[static_cast<size_t>(rng.range(0, n - 1))];
}

inline const char* pickFormWord(ProcSpellForm f, RNG& rng) {
    const auto syn = formSynonyms(f, 0);
    return syn[static_cast<size_t>(rng.range(0, static_cast<int>(syn.size()) - 1))];
}

inline std::string makeRuneSigil(RNG& rng, int minParts = 3, int maxParts = 5) {
    static constexpr std::array<const char*, 18> syll = {
        "KA", "RA", "THO", "MI", "ZU", "VEL", "SHA", "NIR", "GOR",
        "EL", "BAR", "TIN", "LO", "FA", "OR", "KY", "SA", "UM"
    };

    if (maxParts < minParts) maxParts = minParts;
    const int parts = clampi(rng.range(minParts, maxParts), 1, 8);

    std::string out;
    for (int i = 0; i < parts; ++i) {
        if (i) out += "-";
        out += syll[static_cast<size_t>(rng.range(0, static_cast<int>(syll.size()) - 1))];
    }
    return out;
}

inline ProcSpellForm pickFormForTier(uint8_t tier, RNG& rng) {
    // Lower tiers avoid the more complex shapes.
    if (tier <= 2) {
        static constexpr std::array<ProcSpellForm, 3> forms = {ProcSpellForm::Bolt, ProcSpellForm::Hex, ProcSpellForm::Ward};
        return forms[static_cast<size_t>(rng.range(0, static_cast<int>(forms.size()) - 1))];
    }
    if (tier <= 4) {
        static constexpr std::array<ProcSpellForm, 5> forms = {
            ProcSpellForm::Bolt, ProcSpellForm::Beam, ProcSpellForm::Burst, ProcSpellForm::Hex, ProcSpellForm::Ward
        };
        return forms[static_cast<size_t>(rng.range(0, static_cast<int>(forms.size()) - 1))];
    }
    static constexpr std::array<ProcSpellForm, 7> forms = {
        ProcSpellForm::Bolt, ProcSpellForm::Beam, ProcSpellForm::Burst, ProcSpellForm::Cloud, ProcSpellForm::Hex, ProcSpellForm::Ward, ProcSpellForm::Echo
    };
    return forms[static_cast<size_t>(rng.range(0, static_cast<int>(forms.size()) - 1))];
}

inline uint8_t pickMods(uint8_t tier, ProcSpellForm form, RNG& rng) {
    // Mods add spice; keep counts low so later gameplay integration has room.
    static constexpr std::array<uint8_t, 5> pool = {
        ProcSpellMod_Focused,
        ProcSpellMod_Lingering,
        ProcSpellMod_Volatile,
        ProcSpellMod_Wild,
        ProcSpellMod_Echoing,
    };

    int want = 0;
    if (tier >= 3 && rng.chance(0.50f)) ++want;
    if (tier >= 6 && rng.chance(0.40f)) ++want;
    if (tier >= 10 && rng.chance(0.35f)) ++want;
    if (want > 2) want = 2;

    uint8_t mods = 0;
    for (int i = 0; i < want; ++i) {
        const uint8_t m = pool[static_cast<size_t>(rng.range(0, static_cast<int>(pool.size()) - 1))];
        mods |= m;
    }

    // Form-specific bias.
    if (form == ProcSpellForm::Echo) mods |= ProcSpellMod_Echoing;
    if (form == ProcSpellForm::Cloud) mods |= ProcSpellMod_Lingering;
    return mods;
}

inline int formManaDelta(ProcSpellForm form) {
    switch (form) {
    case ProcSpellForm::Bolt:  return 0;
    case ProcSpellForm::Beam:  return 2;
    case ProcSpellForm::Burst: return 2;
    case ProcSpellForm::Cloud: return 3;
    case ProcSpellForm::Hex:   return 1;
    case ProcSpellForm::Ward:  return 1;
    case ProcSpellForm::Echo:  return 2;
    default: return 0;
    }
}

inline void computeCoreNumbers(ProcSpell& s, RNG& rng) {
    // Baseline: scale mostly with tier, then nudge with form/modifiers.
    s.manaCost = 2 + static_cast<int>(s.tier) + formManaDelta(s.form);

    // Wild magic is cheaper but noisier.
    if (s.mods & ProcSpellMod_Wild) {
        s.manaCost = std::max(1, s.manaCost - 2);
    }

    // Targeting & range.
    s.needsTarget = !(s.form == ProcSpellForm::Ward);
    s.range = s.needsTarget ? (6 + static_cast<int>(s.tier) / 2 + 2) : 0;

    // AoE radius.
    if (s.form == ProcSpellForm::Burst || s.form == ProcSpellForm::Cloud) {
        s.aoeRadius = 1 + static_cast<int>(s.tier) / 4;
        if (s.mods & ProcSpellMod_Volatile) s.aoeRadius += 1;
        if (s.mods & ProcSpellMod_Focused) s.aoeRadius = std::max(1, s.aoeRadius - 1);
    }

    // Duration.
    if (s.form == ProcSpellForm::Cloud) {
        s.durationTurns = 3 + static_cast<int>(s.tier);
    } else if (s.form == ProcSpellForm::Hex || s.form == ProcSpellForm::Ward) {
        s.durationTurns = 4 + static_cast<int>(s.tier);
    } else {
        s.durationTurns = 0;
    }
    if (s.mods & ProcSpellMod_Lingering) s.durationTurns += 2;

    // Damage model: only some forms are directly damaging.
    const bool damaging = (s.form == ProcSpellForm::Bolt || s.form == ProcSpellForm::Beam || s.form == ProcSpellForm::Burst || s.form == ProcSpellForm::Cloud || s.form == ProcSpellForm::Echo);
    if (damaging) {
        const int base = 3 + static_cast<int>(s.tier) * 2 + rng.range(0, 2);

        // Convert base into dice (keeps numbers "roguelike-y").
        s.damageDiceSides = 4 + (static_cast<int>(s.tier) / 3) * 2; // 4,6,8,10...
        if (s.damageDiceSides > 12) s.damageDiceSides = 12;
        s.damageDiceCount = std::max(1, base / std::max(2, s.damageDiceSides / 2));
        if (s.damageDiceCount > 6) s.damageDiceCount = 6;

        // Small flat to differentiate.
        s.damageFlat = (base % 3);
        if (s.mods & ProcSpellMod_Focused) s.damageFlat += 1;
        if (s.mods & ProcSpellMod_Volatile) s.damageFlat += 1;
    } else {
        s.damageDiceCount = 0;
        s.damageDiceSides = 0;
        s.damageFlat = 0;
    }

    // Noise is a gameplay-relevant hook; use manaCost as a proxy for magnitude.
    s.noise = 4 + s.manaCost * 2;
    if (s.form == ProcSpellForm::Echo) s.noise += 6;
    if (s.mods & ProcSpellMod_Volatile) s.noise += 4;
    if (s.mods & ProcSpellMod_Wild) s.noise += 2;
    if (s.noise > 30) s.noise = 30;
}

inline std::string makeName(const ProcSpell& s, RNG& rng) {
    const WordBank wb = elementWords(s.element);
    const char* adj = pickAdj(wb, rng);
    const char* noun = pickNoun(wb, rng);
    const char* formw = pickFormWord(s.form, rng);

    const int pattern = rng.range(0, 2);
    std::string name;
    if (pattern == 0) {
        name = std::string(adj) + " " + formw;
    } else if (pattern == 1) {
        name = std::string(formw) + " OF " + noun;
    } else {
        name = std::string(noun) + " " + formw;
    }

    // Add a small chance of a rune epithet at higher tiers.
    if (s.tier >= 7 && rng.chance(0.25f)) {
        name += " (";
        name += (rng.chance(0.5f) ? "ANCIENT" : "RUNED");
        name += ")";
    }
    return toUpper(name);
}

inline std::string makeDescription(const ProcSpell& s) {
    // Keep descriptions short and in the same tone/style as built-in spells.
    std::string d;

    switch (s.form) {
    case ProcSpellForm::Bolt:
        d = "A FAST " + std::string(procSpellElementName(s.element)) + " PROJECTILE.";
        break;
    case ProcSpellForm::Beam:
        d = "A CUTTING " + std::string(procSpellElementName(s.element)) + " RAY THAT RAKES A LINE.";
        break;
    case ProcSpellForm::Burst:
        d = "A " + std::string(procSpellElementName(s.element)) + " NOVA THAT ERUPTS AT THE TARGET.";
        break;
    case ProcSpellForm::Cloud:
        d = "CONJURE A LINGERING " + std::string(procSpellElementName(s.element)) + " CLOUD.";
        break;
    case ProcSpellForm::Hex:
        d = "BRAND A TARGET WITH A " + std::string(procSpellElementName(s.element)) + " HEX.";
        break;
    case ProcSpellForm::Ward:
        d = "ETCH A " + std::string(procSpellElementName(s.element)) + " WARD UPON YOURSELF.";
        break;
    case ProcSpellForm::Echo:
        d = "SEND A " + std::string(procSpellElementName(s.element)) + " ECHO THAT RINGS THROUGH STONE.";
        break;
    default:
        d = "A RUNE-SPELL.";
        break;
    }

    if (s.mods & ProcSpellMod_Focused)   d += " FOCUSED.";
    if (s.mods & ProcSpellMod_Lingering) d += " LINGERS.";
    if (s.mods & ProcSpellMod_Volatile)  d += " VOLATILE.";
    if (s.mods & ProcSpellMod_Wild)      d += " WILD.";
    if (s.mods & ProcSpellMod_Echoing)   d += " ECHOING.";

    // Append a compact stat line for debugging/UI prototypes.
    // (The main UI can hide this later; for now it's useful and deterministic.)
    d += " ";
    d += "T" + std::to_string(static_cast<int>(s.tier));
    d += " | MANA " + std::to_string(s.manaCost);
    if (s.needsTarget) d += " | RNG " + std::to_string(s.range);
    if (s.damageDiceCount > 0) {
        d += " | DMG " + std::to_string(s.damageDiceCount) + "D" + std::to_string(s.damageDiceSides);
        if (s.damageFlat) d += "+" + std::to_string(s.damageFlat);
    }
    if (s.aoeRadius > 0) d += " | RAD " + std::to_string(s.aoeRadius);
    if (s.durationTurns > 0) d += " | DUR " + std::to_string(s.durationTurns);
    d += " | NOISE " + std::to_string(s.noise);

    return toUpper(d);
}

} // namespace detail_procspells

inline ProcSpell generateProcSpell(uint32_t id) {
    ProcSpell s;
    s.id = id;
    s.tier = procSpellTierClamped(id);

    // Seed an isolated RNG from the packed id; do NOT consume the game's RNG.
    const uint32_t seed28 = procSpellSeed(id);
    const uint32_t domain = "PROC_SPELL"_tag;
    const uint32_t seed = hashCombine(seed28, hashCombine(domain, s.tier));
    RNG rng(seed);

    // Element + form.
    static constexpr std::array<ProcSpellElement, 10> elements = {
        ProcSpellElement::Fire,
        ProcSpellElement::Frost,
        ProcSpellElement::Shock,
        ProcSpellElement::Venom,
        ProcSpellElement::Shadow,
        ProcSpellElement::Radiance,
        ProcSpellElement::Arcane,
        ProcSpellElement::Stone,
        ProcSpellElement::Wind,
        ProcSpellElement::Blood,
    };
    s.element = elements[static_cast<size_t>(rng.range(0, static_cast<int>(elements.size()) - 1))];

    s.form = detail_procspells::pickFormForTier(s.tier, rng);
    s.mods = detail_procspells::pickMods(s.tier, s.form, rng);

    detail_procspells::computeCoreNumbers(s, rng);

    // Text generation.
    s.runeSigil = detail_procspells::makeRuneSigil(rng);
    s.name = detail_procspells::makeName(s, rng);
    s.description = detail_procspells::makeDescription(s);
    s.tags = procSpellModsToTags(s.mods);
    if (!s.tags.empty()) {
        s.tags = std::string(procSpellElementName(s.element)) + ", " + s.tags;
    } else {
        s.tags = procSpellElementName(s.element);
    }
    s.tags = toUpper(s.tags);

    return s;
}
