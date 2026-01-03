#pragma once

#include <cstdint>

// Timed status effects (buffs/debuffs).
//
// Design goals:
//  - Keep Entity clean by grouping all timed effects in one place.
//  - Preserve save compatibility by keeping each effect as a dedicated field (append-only).
//  - Provide a generic API (EffectKind + get()) so UI and game logic can iterate effects.

enum class EffectKind : uint8_t {
    Poison = 0,
    Regen,
    Shield,
    Haste,
    Vision,
    Invis,
    Web,

    // New debuffs (append-only)
    Confusion,

    // Fire damage over time (append-only)
    Burn,

    // Traversal / mobility (append-only)
    Levitation,

    // Morale / mind (append-only)
    Fear,
};

// Human-readable short tag for HUD/status lists.
inline const char* effectTag(EffectKind k) {
    switch (k) {
        case EffectKind::Poison:    return "POISON";
        case EffectKind::Regen:     return "REGEN";
        case EffectKind::Shield:    return "SHIELD";
        case EffectKind::Haste:     return "HASTE";
        case EffectKind::Vision:    return "VISION";
        case EffectKind::Invis:     return "INVIS";
        case EffectKind::Web:       return "WEB";
        case EffectKind::Confusion: return "CONF";
        case EffectKind::Burn:      return "BURN";
        case EffectKind::Levitation:return "LEV";
        case EffectKind::Fear:      return "FEAR";
        default:                    return "?";
    }
}

// End-of-effect message (player-facing). For non-player entities, the caller typically
// omits messaging.
inline const char* effectEndMessage(EffectKind k) {
    switch (k) {
        case EffectKind::Poison:    return "THE POISON WEARS OFF.";
        case EffectKind::Regen:     return "YOUR REGENERATION FADES.";
        case EffectKind::Shield:    return "YOUR STONESKIN CRUMBLES.";
        case EffectKind::Vision:    return "YOUR VISION RETURNS TO NORMAL.";
        case EffectKind::Invis:     return "YOU BECOME VISIBLE!";
        case EffectKind::Web:       return "YOU BREAK FREE OF THE WEB!";
        case EffectKind::Confusion: return "YOU FEEL LESS CONFUSED.";
        case EffectKind::Burn:      return "THE FLAMES SUBSIDE.";
        case EffectKind::Levitation:return "YOU SINK BACK TO THE GROUND.";
        case EffectKind::Fear:      return "YOU FEEL YOUR COURAGE RETURN.";
        default:                    return "";
    }
}

struct Effects {
    // NOTE: append-only (for save compatibility). Prefer adding new fields at the end.
    int poisonTurns = 0;    // lose 1 HP per full turn
    int regenTurns = 0;     // heal 1 HP per full turn
    int shieldTurns = 0;    // temporary defense boost

    int hasteTurns = 0;     // grants extra player actions (decrements on monster turns)
    int visionTurns = 0;    // increases FOV radius
    int invisTurns = 0;     // makes it harder for monsters to track/see you

    int webTurns = 0;       // prevents movement while >0

    int confusionTurns = 0; // makes movement/aim erratic while >0

    int burnTurns = 0;      // take 1 HP per turn while >0

    int levitationTurns = 0; // can traverse certain hazardous terrain while >0

    int fearTurns = 0;      // monsters prefer fleeing (and avoid attacking) while >0

    bool has(EffectKind k) const { return get(k) > 0; }

    int get(EffectKind k) const {
        switch (k) {
            case EffectKind::Poison:    return poisonTurns;
            case EffectKind::Regen:     return regenTurns;
            case EffectKind::Shield:    return shieldTurns;
            case EffectKind::Haste:     return hasteTurns;
            case EffectKind::Vision:    return visionTurns;
            case EffectKind::Invis:     return invisTurns;
            case EffectKind::Web:       return webTurns;
            case EffectKind::Confusion: return confusionTurns;
            case EffectKind::Burn:      return burnTurns;
            case EffectKind::Levitation:return levitationTurns;
            case EffectKind::Fear:      return fearTurns;
            default:                    return 0;
        }
    }

    int& getRef(EffectKind k) {
        switch (k) {
            case EffectKind::Poison:    return poisonTurns;
            case EffectKind::Regen:     return regenTurns;
            case EffectKind::Shield:    return shieldTurns;
            case EffectKind::Haste:     return hasteTurns;
            case EffectKind::Vision:    return visionTurns;
            case EffectKind::Invis:     return invisTurns;
            case EffectKind::Web:       return webTurns;
            case EffectKind::Confusion: return confusionTurns;
            case EffectKind::Burn:      return burnTurns;
            case EffectKind::Levitation:return levitationTurns;
            case EffectKind::Fear:      return fearTurns;
            default:                    return poisonTurns; // should be unreachable
        }
    }

    void clear(EffectKind k) { getRef(k) = 0; }
};

// Keep in sync with EffectKind (append-only).
inline constexpr int EFFECT_KIND_COUNT = static_cast<int>(EffectKind::Fear) + 1;
