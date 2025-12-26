#include "keybinds.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

Uint16 KeyBinds::normalizeMods(Uint16 mods) {
    return mods & (KMOD_SHIFT | KMOD_CTRL | KMOD_ALT);
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

SDL_Keycode KeyBinds::parseKeycode(const std::string& keyNameIn, Uint16* impliedMods) {
    if (impliedMods) *impliedMods = KMOD_NONE;
    std::string keyName = trim(toLower(keyNameIn));
    if (keyName.empty()) return SDLK_UNKNOWN;

    // Single character (letters are treated case-insensitively).
    if (keyName.size() == 1) {
        unsigned char c = static_cast<unsigned char>(keyName[0]);
        if (std::isalpha(c)) c = static_cast<unsigned char>(std::tolower(c));
        return static_cast<SDL_Keycode>(c);
    }

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
    if (keyName == "less") return SDLK_LESS;
    if (keyName == "greater") return SDLK_GREATER;

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
    if (kc != SDLK_UNKNOWN) return kc;

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
    for (const auto& part : split(value, ',')) {
        auto chord = parseChord(part);
        if (chord.has_value()) out.push_back(*chord);
    }
    return out;
}

std::optional<Action> KeyBinds::parseActionName(const std::string& bindKeyIn) {
    std::string key = trim(toLower(bindKeyIn));
    if (key.rfind("bind_", 0) != 0) return std::nullopt;
    std::string name = key.substr(5);

    // Movement
    if (name == "up") return Action::Up;
    if (name == "down") return Action::Down;
    if (name == "left") return Action::Left;
    if (name == "right") return Action::Right;
    if (name == "up_left" || name == "upleft") return Action::UpLeft;
    if (name == "up_right" || name == "upright") return Action::UpRight;
    if (name == "down_left" || name == "downleft") return Action::DownLeft;
    if (name == "down_right" || name == "downright") return Action::DownRight;

    // Core actions
    if (name == "confirm" || name == "ok") return Action::Confirm;
    if (name == "cancel" || name == "escape") return Action::Cancel;
    if (name == "equip") return Action::Equip;
    if (name == "use") return Action::Use;
    if (name == "drop") return Action::Drop;
    if (name == "drop_all" || name == "dropall") return Action::DropAll;
    if (name == "sort_inventory" || name == "sortinventory") return Action::SortInventory;
    if (name == "wait") return Action::Wait;
    if (name == "rest") return Action::Rest;
    if (name == "pickup" || name == "pick_up") return Action::Pickup;
    if (name == "inventory" || name == "inv") return Action::Inventory;
    if (name == "fire") return Action::Fire;
    if (name == "search") return Action::Search;
    if (name == "look") return Action::Look;
    if (name == "stairs_up" || name == "stairsup") return Action::StairsUp;
    if (name == "stairs_down" || name == "stairsdown") return Action::StairsDown;
    if (name == "auto_explore" || name == "autoexplore") return Action::AutoExplore;
    if (name == "toggle_auto_pickup" || name == "toggleautopickup") return Action::ToggleAutoPickup;

    // UI / meta
    if (name == "toggle_minimap" || name == "minimap") return Action::ToggleMinimap;
    if (name == "toggle_stats" || name == "stats") return Action::ToggleStats;
    if (name == "help") return Action::Help;
    if (name == "options") return Action::Options;
    if (name == "command" || name == "extcmd") return Action::Command;

    if (name == "save") return Action::Save;
    if (name == "load") return Action::Load;
    if (name == "load_auto" || name == "loadauto") return Action::LoadAuto;
    if (name == "restart" || name == "newgame") return Action::Restart;

    if (name == "log_up" || name == "logup") return Action::LogUp;
    if (name == "log_down" || name == "logdown") return Action::LogDown;

    return std::nullopt;
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
    add(Action::Rest, SDLK_r);

    add(Action::Pickup, SDLK_g);
    add(Action::Pickup, SDLK_COMMA);
    add(Action::Pickup, SDLK_KP_0);

    add(Action::Inventory, SDLK_i);
    add(Action::Inventory, SDLK_TAB);

    add(Action::Fire, SDLK_f);
    add(Action::Search, SDLK_c);
    add(Action::Look, SDLK_l);
    add(Action::Look, SDLK_v);

    add(Action::StairsUp, SDLK_COMMA, KMOD_SHIFT);
    add(Action::StairsUp, SDLK_LESS);

    add(Action::StairsDown, SDLK_PERIOD, KMOD_SHIFT);
    add(Action::StairsDown, SDLK_GREATER);

    add(Action::AutoExplore, SDLK_o);
    add(Action::ToggleAutoPickup, SDLK_p);

    // UI / meta
    add(Action::Help, SDLK_F1);
    add(Action::Help, SDLK_SLASH, KMOD_SHIFT);
    add(Action::Help, SDLK_h);

    add(Action::Options, SDLK_F2);
    add(Action::Command, SDLK_3, KMOD_SHIFT);

    add(Action::ToggleMinimap, SDLK_m);
    add(Action::ToggleStats, SDLK_TAB, KMOD_SHIFT);

    add(Action::Save, SDLK_F5);
    add(Action::Restart, SDLK_F6);
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

        binds[*act] = parseChordList(val);
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
        });
        if (a != Action::None) return a;
    }

    // Default priority order.
    Action a = matchIn({
        Action::LogUp,
        Action::LogDown,

        Action::Help,
        Action::Options,
        Action::Command,

        Action::Save,
        Action::Load,
        Action::LoadAuto,
        Action::Restart,

        Action::ToggleMinimap,
        Action::ToggleStats,

        Action::Inventory,
        Action::Fire,
        Action::Look,
        Action::Search,
        Action::AutoExplore,
        Action::ToggleAutoPickup,
        Action::Pickup,
        Action::Rest,
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
