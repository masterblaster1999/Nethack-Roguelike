#include "keybinds.hpp"
#include "action_info.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

Uint16 KeyBinds::normalizeMods(Uint16 mods) {
    return mods & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT | KMOD_GUI);
}

bool KeyBinds::chordMatches(const KeyChord& chord, SDL_Keycode key, Uint16 mods) {
    return chord.key == key && chord.mods == normalizeMods(mods);
}

std::string KeyBinds::trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string KeyBinds::toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::vector<std::string> KeyBinds::split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream iss(s);
    while (std::getline(iss, cur, delim)) out.push_back(cur);
    return out;
}

namespace {
// Convert a keycode that may represent a shifted symbol (e.g. '?', '<', '#')
// into the *base* (unmodified) keycode for the same physical key, using the
// current keyboard layout.
//
// This is critical because SDL will often set BOTH:
//   - mods: KMOD_SHIFT
//   - keycode: the shifted symbol
// for punctuation. Our binds are expressed as (base key + required modifiers),
// so we normalize tokens and events to that representation.
static SDL_Keycode baseKeycodeForLayout(SDL_Keycode key, Uint16* impliedMods) {
    // Preserve special keys.
    if (key == SDLK_UNKNOWN) return key;

    // Uppercase ASCII letters: treat as the base letter + implied Shift.
    if (key >= 'A' && key <= 'Z') {
        if (impliedMods) *impliedMods |= KMOD_SHIFT;
        return static_cast<SDL_Keycode>(static_cast<char>(key - 'A' + 'a'));
    }

    // Layout-aware fallback: map keycode -> scancode -> base keycode.
    const SDL_Scancode sc = SDL_GetScancodeFromKey(key);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        // SDL's keycode->scancode mapping is layout-dependent and may not
        // resolve *shifted* printable symbols (e.g. '<', '>', '?') on some
        // platforms/layouts.
        //
        // Our runtime input path normalizes events using the physical scancode
        // (see main.cpp) so binds are stored as: base key + required modifiers.
        //
        // If SDL cannot map a shifted symbol keycode back to its scancode, do a
        // small best-effort fallback for the most common NetHack / desktop UX
        // symbols, using the US keyboard shift pairs.
        //
        // IMPORTANT: This fallback only triggers when SDL reports UNKNOWN,
        // so layouts with dedicated '<'/'>' keys (where SDL *can* map them)
        // will keep working as-is.
        if (impliedMods) {
            switch (key) {
                case '?': *impliedMods |= KMOD_SHIFT; return SDLK_SLASH;
                case '<': *impliedMods |= KMOD_SHIFT; return SDLK_COMMA;
                case '>': {
                    // If SDL can't map '>' directly but *can* map '<', prefer
                    // interpreting '>' as SHIFT+'<' (common on keyboards with a
                    // dedicated '<'/'>' key).
                    const SDL_Scancode scLess = SDL_GetScancodeFromKey(SDLK_LESS);
                    if (scLess != SDL_SCANCODE_UNKNOWN) {
                        *impliedMods |= KMOD_SHIFT;
                        return SDL_GetKeyFromScancode(scLess);
                    }

                    *impliedMods |= KMOD_SHIFT;
                    return SDLK_PERIOD;
                }
                default: break;
            }
        }
        return key;
    }

    const SDL_Keycode base = SDL_GetKeyFromScancode(sc);
    if (base == SDLK_UNKNOWN || base == key) return key;

    // If the unmodified key differs from the symbol token, the symbol is
    // typically reached via Shift on this layout.
    if (impliedMods) *impliedMods |= KMOD_SHIFT;
    return base;
}
} // namespace

SDL_Keycode KeyBinds::parseKeycode(const std::string& keyNameIn, Uint16* impliedMods) {
    if (impliedMods) *impliedMods = KMOD_NONE;

    std::string raw = trim(keyNameIn);
    if (raw.empty()) return SDLK_UNKNOWN;

    // Single character tokens are accepted verbatim (e.g. w, ., ?, D).
    // We keep the original case so uppercase can imply Shift.
    if (raw.size() == 1) {
        SDL_Keycode kc = static_cast<SDL_Keycode>(static_cast<unsigned char>(raw[0]));
        return baseKeycodeForLayout(kc, impliedMods);
    }

    std::string keyName = trim(toLower(raw));
    if (keyName.empty()) return SDLK_UNKNOWN;

    // Directional / navigation
    if (keyName == "up") return SDLK_UP;
    if (keyName == "down") return SDLK_DOWN;
    if (keyName == "left") return SDLK_LEFT;
    if (keyName == "right") return SDLK_RIGHT;

    if (keyName == "pageup" || keyName == "pgup") return SDLK_PAGEUP;
    if (keyName == "pagedown" || keyName == "pgdn") return SDLK_PAGEDOWN;
    if (keyName == "home") return SDLK_HOME;
    if (keyName == "end") return SDLK_END;
    if (keyName == "insert" || keyName == "ins") return SDLK_INSERT;
    if (keyName == "delete" || keyName == "del") return SDLK_DELETE;

    // Control keys
    if (keyName == "enter" || keyName == "return") return SDLK_RETURN;
    if (keyName == "escape" || keyName == "esc") return SDLK_ESCAPE;
    if (keyName == "tab") return SDLK_TAB;
    if (keyName == "space") return SDLK_SPACE;
    if (keyName == "backspace") return SDLK_BACKSPACE;

    // Punctuation (named)
    if (keyName == "comma") return SDLK_COMMA;
    if (keyName == "period" || keyName == "dot") return SDLK_PERIOD;
    if (keyName == "slash") return SDLK_SLASH;
    if (keyName == "backslash") return SDLK_BACKSLASH;
    if (keyName == "minus" || keyName == "dash") return SDLK_MINUS;
    if (keyName == "equals" || keyName == "equal") return SDLK_EQUALS;
    if (keyName == "semicolon") return SDLK_SEMICOLON;
    if (keyName == "apostrophe" || keyName == "quote") return SDLK_QUOTE;
    if (keyName == "grave" || keyName == "backquote") return SDLK_BACKQUOTE;
    if (keyName == "less") return baseKeycodeForLayout(SDLK_LESS, impliedMods);
    if (keyName == "greater") return baseKeycodeForLayout(SDLK_GREATER, impliedMods);

    // Function keys
    if (keyName.size() >= 2 && keyName[0] == 'f' && std::isdigit(static_cast<unsigned char>(keyName[1]))) {
        int n = 0;
        try {
            n = std::stoi(keyName.substr(1));
        } catch (...) {
            n = 0;
        }
        if (n >= 1 && n <= 24) {
            return static_cast<SDL_Keycode>(SDLK_F1 + (n - 1));
        }
    }

    // Keypad
    if (keyName == "kp_enter") return SDLK_KP_ENTER;
    if (keyName == "kp_0") return SDLK_KP_0;
    if (keyName == "kp_1") return SDLK_KP_1;
    if (keyName == "kp_2") return SDLK_KP_2;
    if (keyName == "kp_3") return SDLK_KP_3;
    if (keyName == "kp_4") return SDLK_KP_4;
    if (keyName == "kp_5") return SDLK_KP_5;
    if (keyName == "kp_6") return SDLK_KP_6;
    if (keyName == "kp_7") return SDLK_KP_7;
    if (keyName == "kp_8") return SDLK_KP_8;
    if (keyName == "kp_9") return SDLK_KP_9;

    // Fallback: SDL's own key name parsing.
    // This lets users use names like "Left Shift", "Keypad 8", etc.
    SDL_Keycode kc = SDL_GetKeyFromName(keyNameIn.c_str());
    if (kc != SDLK_UNKNOWN) {
        return baseKeycodeForLayout(kc, impliedMods);
    }

    return SDLK_UNKNOWN;
}

std::optional<KeyChord> KeyBinds::parseChord(const std::string& tokenIn) {
    std::string token = trim(tokenIn);
    if (token.empty()) return std::nullopt;

    std::vector<std::string> parts = split(token, '+');
    if (parts.empty()) return std::nullopt;

    Uint16 mods = KMOD_NONE;

    // All parts except the last are modifiers.
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string m = trim(toLower(parts[i]));
        if (m == "shift") mods |= KMOD_SHIFT;
        else if (m == "ctrl" || m == "control") mods |= KMOD_CTRL;
        else if (m == "alt") mods |= KMOD_ALT;
        else if (m == "cmd" || m == "gui" || m == "meta" || m == "super") mods |= KMOD_GUI;
        else return std::nullopt;
    }

    Uint16 impliedMods = KMOD_NONE;
    SDL_Keycode key = parseKeycode(parts.back(), &impliedMods);
    if (key == SDLK_UNKNOWN) return std::nullopt;

    KeyChord chord;
    chord.key = key;
    chord.mods = normalizeMods(mods | impliedMods);
    return chord;
}

std::vector<KeyChord> KeyBinds::parseChordList(const std::string& valueIn) {
    std::string value = trim(valueIn);
    if (value.empty()) return {};
    std::string vLow = toLower(value);
    if (vLow == "none" || vLow == "unbound" || vLow == "disabled") return {};

    std::vector<KeyChord> out;

    auto already = [&](const KeyChord& c) -> bool {
        for (const auto& ex : out) {
            if (ex.key == c.key && ex.mods == c.mods) return true;
        }
        return false;
    };

    for (const auto& part : split(value, ',')) {
        auto chord = parseChord(part);
        if (chord.has_value() && !already(*chord)) out.push_back(*chord);
    }
    return out;
}


std::optional<Action> KeyBinds::parseActionName(const std::string& bindKeyIn) {
    // Only parse keybind declarations from INI keys (bind_<action>).
    // The shared registry still handles alias normalization *after* the prefix.
    std::string key = trim(toLower(bindKeyIn));
    if (key.rfind("bind_", 0) != 0) return std::nullopt;
    return actioninfo::parse(key);
}

KeyBinds KeyBinds::defaults() {
    KeyBinds kb;

    auto add = [&](Action a, SDL_Keycode key, Uint16 mods = KMOD_NONE) {
        kb.binds[a].push_back({key, normalizeMods(mods)});
    };

    // Movement
    add(Action::Up, SDLK_w);
    add(Action::Up, SDLK_UP);
    add(Action::Up, SDLK_KP_8);

    add(Action::Down, SDLK_s);
    add(Action::Down, SDLK_DOWN);
    add(Action::Down, SDLK_KP_2);

    add(Action::Left, SDLK_a);
    add(Action::Left, SDLK_LEFT);
    add(Action::Left, SDLK_KP_4);

    add(Action::Right, SDLK_d);
    add(Action::Right, SDLK_RIGHT);
    add(Action::Right, SDLK_KP_6);

    add(Action::UpLeft, SDLK_q);
    add(Action::UpLeft, SDLK_KP_7);

    add(Action::UpRight, SDLK_e);
    add(Action::UpRight, SDLK_KP_9);

    add(Action::DownLeft, SDLK_z);
    add(Action::DownLeft, SDLK_KP_1);

    add(Action::DownRight, SDLK_c);
    add(Action::DownRight, SDLK_KP_3);

    // Actions
    add(Action::Confirm, SDLK_RETURN);
    add(Action::Confirm, SDLK_KP_ENTER);

    add(Action::Cancel, SDLK_ESCAPE);
    add(Action::Cancel, SDLK_BACKSPACE);

    add(Action::Equip, SDLK_e);
    add(Action::Use, SDLK_u);

    add(Action::Drop, SDLK_x);
    add(Action::DropAll, SDLK_x, KMOD_SHIFT);

    add(Action::SortInventory, SDLK_s, KMOD_SHIFT);

    add(Action::Wait, SDLK_SPACE);
    add(Action::Wait, SDLK_PERIOD);
    add(Action::Parry, SDLK_p, KMOD_SHIFT);
    add(Action::Rest, SDLK_r);
    add(Action::ToggleSneak, SDLK_n);
    add(Action::Evade, SDLK_e, KMOD_CTRL);

    add(Action::Pickup, SDLK_g);
    add(Action::Pickup, SDLK_COMMA);
    add(Action::Pickup, SDLK_KP_0);

    add(Action::Inventory, SDLK_i);
    add(Action::Inventory, SDLK_TAB);

    // Spellbook / spellcasting overlay (WIP)
    add(Action::Spells, SDLK_z, KMOD_SHIFT);

    add(Action::Fire, SDLK_f);
    add(Action::Search, SDLK_c, KMOD_SHIFT);
    add(Action::Disarm, SDLK_t);
    add(Action::CloseDoor, SDLK_k);
    add(Action::LockDoor, SDLK_k, KMOD_SHIFT);
    add(Action::Kick, SDLK_b);
    add(Action::Butcher, {SDLK_b, KMOD_SHIFT});
    add(Action::Dig, SDLK_d, KMOD_SHIFT);
    add(Action::Look, SDLK_l);
    add(Action::Look, SDLK_v);

    add(Action::StairsUp, SDLK_COMMA, KMOD_SHIFT);
    add(Action::StairsUp, SDLK_LESS);

    add(Action::StairsDown, SDLK_PERIOD, KMOD_SHIFT);
    // Some keyboard layouts have a dedicated '<'/'>' key. Since we normalize
    // keybinds to the *base* key from the scancode, '>' becomes SHIFT+'<' on
    // those layouts.
    add(Action::StairsDown, SDLK_LESS, KMOD_SHIFT);
    add(Action::StairsDown, SDLK_GREATER);

    add(Action::AutoExplore, SDLK_o);
    add(Action::ToggleAutoPickup, SDLK_p);

    // UI / meta
    add(Action::Help, SDLK_F1);
    add(Action::Help, SDLK_SLASH, KMOD_SHIFT);
    add(Action::Help, SDLK_h);
    // Common desktop UX: Command+? (Cmd+Shift+/) for Help.
    add(Action::Help, SDLK_SLASH, KMOD_GUI | KMOD_SHIFT);

    add(Action::Options, SDLK_F2);
    add(Action::Command, SDLK_3, KMOD_SHIFT);
    // Convenience: open the extended command prompt on keyboards/layouts where '#' is awkward.
    add(Action::Command, SDLK_p, KMOD_CTRL);

    add(Action::ToggleMinimap, SDLK_m);
    add(Action::ToggleOverworldMap, SDLK_m, KMOD_SHIFT);
    add(Action::MinimapZoomOut, SDLK_LEFTBRACKET);
    add(Action::MinimapZoomIn, SDLK_RIGHTBRACKET);
    add(Action::MessageHistory, SDLK_F3);
    add(Action::MessageHistory, SDLK_m, KMOD_SHIFT);
    add(Action::Codex, SDLK_F4);
    add(Action::Discoveries, SDLK_BACKSLASH);
    add(Action::ToggleStats, SDLK_TAB, KMOD_SHIFT);
    add(Action::TogglePerfOverlay, SDLK_F10, KMOD_SHIFT);

    // LOOK helper: show an acoustic "heatmap" of sound propagation from the cursor
    add(Action::ToggleSoundPreview, SDLK_n, KMOD_CTRL);

    // LOOK helper: show a tactical "heatmap" of nearby monster threat/ETA
    add(Action::ToggleThreatPreview, SDLK_t, KMOD_CTRL);

    // LOOK helper: show an "audibility map" of where your footsteps would be heard
    add(Action::ToggleHearingPreview, SDLK_h, KMOD_CTRL);

    // LOOK helper: visualize your lingering scent trail (for smell-tracking monsters)
    add(Action::ToggleScentPreview, SDLK_s, KMOD_CTRL);

    add(Action::ToggleViewMode, SDLK_F7);
    add(Action::ToggleVoxelSprites, SDLK_F8);

    add(Action::ToggleFullscreen, SDLK_F11);
    add(Action::Screenshot, SDLK_F12);

    add(Action::Save, SDLK_F5);
    add(Action::Scores, SDLK_F6);
    add(Action::Restart, SDLK_F6, KMOD_SHIFT);
    add(Action::Load, SDLK_F9);
    add(Action::LoadAuto, SDLK_F10);

    add(Action::LogUp, SDLK_PAGEUP);
    add(Action::LogDown, SDLK_PAGEDOWN);

    return kb;
}

void KeyBinds::loadOverridesFromIni(const std::string& settingsPath) {
    std::ifstream in(settingsPath);
    if (!in) return;

    std::string line;
    while (std::getline(in, line)) {
        // Strip comments
        auto commentPos = line.find_first_of("#;");
        if (commentPos != std::string::npos) line = line.substr(0, commentPos);

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key.empty()) continue;

        auto act = parseActionName(key);
        if (!act.has_value()) continue;

        std::vector<KeyChord> chords = parseChordList(val);

        // Back-compat: older default settings files (and some old control preset writes) omitted cmd+?
        // from the Help binding. Now that cmd/gui is a first-class modifier, upgrade the common
        // default binds in-memory so macOS-style Command+? works without requiring a manual settings edit.
        if (*act == Action::Help) {
            bool hasCmd = false;
            bool hasF1 = false;
            bool hasShiftSlash = false;
            bool hasH = false;

            for (const auto& c : chords) {
                if ((c.mods & KMOD_GUI) != 0) hasCmd = true;
                if (c.key == SDLK_F1 && c.mods == KMOD_NONE) hasF1 = true;
                if (c.key == SDLK_SLASH && c.mods == KMOD_SHIFT) hasShiftSlash = true;
                if (c.key == SDLK_h && c.mods == KMOD_NONE) hasH = true;
            }

            const bool looksLikeOldModern = (chords.size() == 3 && hasF1 && hasShiftSlash && hasH);
            const bool looksLikeOldNethack = (chords.size() == 2 && hasF1 && hasShiftSlash && !hasH);

            if (!hasCmd && (looksLikeOldModern || looksLikeOldNethack)) {
                if (auto cmdChord = parseChord("cmd+?"); cmdChord.has_value()) {
                    chords.push_back(*cmdChord);
                }
            }
        }

        binds[*act] = std::move(chords);
    }

    // Back-compat: older default settings files wrote `bind_search = c`, which
    // conflicts with the modern diagonal move `bind_down_right = c`.
    // If we detect that specific conflict, automatically shift Search to `C`.
    {
        const KeyChord legacy{SDLK_c, 0};
        const KeyChord fixed{SDLK_c, KMOD_SHIFT};

        auto hasChord = [](const std::vector<KeyChord>& v, const KeyChord& c) {
            for (const auto& it : v) {
                if (it.key == c.key && it.mods == c.mods) return true;
            }
            return false;
        };

        auto itSearch = binds.find(Action::Search);
        auto itDR = binds.find(Action::DownRight);
        if (itSearch != binds.end() && itDR != binds.end()) {
            const bool searchHasLegacy = hasChord(itSearch->second, legacy);
            const bool downRightHasLegacy = hasChord(itDR->second, legacy);
            if (searchHasLegacy && downRightHasLegacy) {
                auto& chords = itSearch->second;
                chords.erase(
                    std::remove_if(chords.begin(), chords.end(), [&](const KeyChord& c) {
                        return c.key == legacy.key && c.mods == legacy.mods;
                    }),
                    chords.end());
                if (!hasChord(chords, fixed)) {
                    chords.push_back(fixed);
                }
            }
        }
    }
}

Action KeyBinds::mapKey(const Game& game, SDL_Keycode key, Uint16 mods) const {
    Uint16 nm = normalizeMods(mods);

    auto match = [&](Action a) -> bool {
        auto it = binds.find(a);
        if (it == binds.end()) return false;
        for (const auto& chord : it->second) {
            if (chordMatches(chord, key, nm)) return true;
        }
        return false;
    };

    auto matchIn = [&](std::initializer_list<Action> order) -> Action {
        for (auto a : order) {
            if (match(a)) return a;
        }
        return Action::None;
    };

    // Inventory gets priority for inventory-specific actions (users may rebind keys to overlap movement).
    //
    // IMPORTANT: global UI/meta hotkeys should still work while the inventory overlay is open
    // (help/options/message history/etc). This avoids "dead" bindings when a menu is up.
    if (game.isInventoryOpen()) {
        Action a = matchIn({
            Action::DropAll,
            Action::SortInventory,
            Action::Equip,
            Action::Use,
            Action::Drop,
            Action::Confirm,
            Action::Cancel,
            Action::Up,
            Action::Down,
            Action::Left,
            Action::Right,
            Action::LogUp,
            Action::LogDown,
            Action::Help,
            Action::Options,
            Action::Command,
            Action::MessageHistory,
            Action::Codex,
            Action::Discoveries,
            Action::Spells,
            Action::Scores,
            Action::Save,
            Action::Load,
            Action::LoadAuto,
            Action::Restart,
            Action::ToggleMinimap,
            Action::MinimapZoomIn,
            Action::MinimapZoomOut,
            Action::ToggleStats,
            Action::TogglePerfOverlay,
            Action::ToggleSoundPreview,
            Action::ToggleThreatPreview,
            Action::ToggleHearingPreview,
            Action::ToggleScentPreview,
            Action::ToggleViewMode,
            Action::ToggleVoxelSprites,
            Action::ToggleFullscreen,
            Action::Screenshot,
        });
        if (a != Action::None) return a;
    }

    // Default priority order.
    Action a = matchIn({
        Action::LogUp,
        Action::LogDown,

        Action::Help,
        Action::MessageHistory,
        Action::Codex,
        Action::Discoveries,
        Action::Options,
        Action::Command,

        Action::Save,
        Action::Load,
        Action::LoadAuto,
        Action::Restart,

        Action::Scores,

        Action::ToggleMinimap,
        Action::MinimapZoomIn,
        Action::MinimapZoomOut,
        Action::ToggleStats,
        Action::TogglePerfOverlay,
        Action::ToggleSoundPreview,
        Action::ToggleThreatPreview,
        Action::ToggleHearingPreview,
        Action::ToggleScentPreview,
        Action::ToggleViewMode,
        Action::ToggleVoxelSprites,
        Action::ToggleFullscreen,
        Action::Screenshot,

        Action::Inventory,
        Action::Spells,
        Action::Fire,
        Action::Look,
        Action::Search,
        Action::Disarm,
        Action::CloseDoor,
        Action::LockDoor,
        Action::Kick,
        Action::Dig,
        Action::AutoExplore,
        Action::ToggleAutoPickup,
        Action::Pickup,
        Action::Rest,
        Action::ToggleSneak,
        Action::Evade,
        Action::Parry,
        Action::Wait,

        Action::Confirm,
        Action::Cancel,

        Action::StairsUp,
        Action::StairsDown,

        Action::Up,
        Action::Down,
        Action::Left,
        Action::Right,
        Action::UpLeft,
        Action::UpRight,
        Action::DownLeft,
        Action::DownRight,
    });

    return a;
}



static std::string keycodeToToken(SDL_Keycode key) {
    // Printable ASCII range: keep letters/digits as-is for copy/paste convenience.
    if (key >= 32 && key <= 126) {
        const char c = static_cast<char>(key);
        // Prefer named tokens for common punctuation to match docs and reduce ambiguity.
        switch (c) {
            case ',': return "comma";
            case '.': return "period";
            case '/': return "slash";
            case '\\': return "backslash";
            case '-': return "minus";
            case '=': return "equals";
            case ';': return "semicolon";
            case '\'': return "apostrophe";
            case '`': return "grave";
            case '<': return "less";
            case '>': return "greater";
            case ' ': return "space";
            default:
                return std::string(1, c);
        }
    }

    switch (key) {
        // Arrows / navigation
        case SDLK_UP: return "up";
        case SDLK_DOWN: return "down";
        case SDLK_LEFT: return "left";
        case SDLK_RIGHT: return "right";
        case SDLK_PAGEUP: return "pageup";
        case SDLK_PAGEDOWN: return "pagedown";
        case SDLK_HOME: return "home";
        case SDLK_END: return "end";
        case SDLK_INSERT: return "insert";
        case SDLK_DELETE: return "delete";

        // Control keys
        case SDLK_RETURN: return "enter";
        case SDLK_ESCAPE: return "escape";
        case SDLK_TAB: return "tab";
        case SDLK_BACKSPACE: return "backspace";
        case SDLK_SPACE: return "space";

        // Function keys
        case SDLK_F1:  return "f1";
        case SDLK_F2:  return "f2";
        case SDLK_F3:  return "f3";
        case SDLK_F4:  return "f4";
        case SDLK_F5:  return "f5";
        case SDLK_F6:  return "f6";
        case SDLK_F7:  return "f7";
        case SDLK_F8:  return "f8";
        case SDLK_F9:  return "f9";
        case SDLK_F10: return "f10";
        case SDLK_F11: return "f11";
        case SDLK_F12: return "f12";

        // Keypad
        case SDLK_KP_ENTER: return "kp_enter";
        case SDLK_KP_0: return "kp_0";
        case SDLK_KP_1: return "kp_1";
        case SDLK_KP_2: return "kp_2";
        case SDLK_KP_3: return "kp_3";
        case SDLK_KP_4: return "kp_4";
        case SDLK_KP_5: return "kp_5";
        case SDLK_KP_6: return "kp_6";
        case SDLK_KP_7: return "kp_7";
        case SDLK_KP_8: return "kp_8";
        case SDLK_KP_9: return "kp_9";

        default: break;
    }

    // Fallback: SDL's canonical key name (parseable by SDL_GetKeyFromName()).
    const char* name = SDL_GetKeyName(key);
    if (name && name[0] != '\0') {
        return std::string(name);
    }
    return "unknown";
}

bool KeyBinds::isModifierKey(SDL_Keycode key) {
    switch (key) {
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:
        case SDLK_LCTRL:
        case SDLK_RCTRL:
        case SDLK_LALT:
        case SDLK_RALT:
        case SDLK_LGUI:
        case SDLK_RGUI:
            return true;
        default:
            return false;
    }
}

std::string KeyBinds::chordToString(SDL_Keycode key, Uint16 mods) {
    // NOTE: We intentionally ignore CAPS/NUM (and other non-bindable) modifiers.
    const Uint16 nm = normalizeMods(mods);

    std::string out;
    if (nm & KMOD_GUI) out += "cmd+";
    if (nm & KMOD_CTRL) out += "ctrl+";
    if (nm & KMOD_ALT) out += "alt+";
    if (nm & KMOD_SHIFT) out += "shift+";
    out += keycodeToToken(key);
    return out;
}


std::string KeyBinds::describeAction(Action a) const {
    auto it = binds.find(a);
    if (it == binds.end() || it->second.empty()) return "none";

    std::string out;
    const auto& chords = it->second;
    for (size_t i = 0; i < chords.size(); ++i) {
        out += KeyBinds::chordToString(chords[i].key, chords[i].mods);
        if (i + 1 < chords.size()) out += ", ";
    }
    return out.empty() ? "none" : out;
}

std::vector<std::pair<std::string, std::string>> KeyBinds::describeAll() const {
    std::vector<std::pair<std::string, std::string>> out;
    out.reserve(sizeof(actioninfo::kActionInfoTable) / sizeof(actioninfo::kActionInfoTable[0]));
    for (const auto& info : actioninfo::kActionInfoTable) {
        out.emplace_back(std::string(info.token), describeAction(info.action));
    }
    return out;
}