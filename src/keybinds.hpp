#pragma once

#include "sdl.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

#include "game.hpp"

// Configurable keybindings loaded from procrogue_settings.ini.
//
// The binding format is:
//   bind_<action> = key[, key, ...]
//
// Each key can be:
//   - a single character: w, ., ?
//   - a named key: up, down, left, right, tab, enter, escape, pageup, f1, kp_8, ...
// Modifiers can be prefixed with: shift+, ctrl+, alt+  (example: shift+comma)
//
// Notes:
//   * We treat bindings as (keycode + required modifiers). Extra modifiers do NOT match.
//   * When the inventory is open, inventory actions are prioritized over movement.

struct KeyChord {
    SDL_Keycode key = SDLK_UNKNOWN;
    Uint16 mods = KMOD_NONE; // only SHIFT/CTRL/ALT bits are used
};

struct ActionHash {
    size_t operator()(Action a) const noexcept { return static_cast<size_t>(a); }
};

class KeyBinds {
public:
    static KeyBinds defaults();
    void loadOverridesFromIni(const std::string& settingsPath);

    Action mapKey(const Game& game, SDL_Keycode key, Uint16 mods) const;

    // Returns human-readable bindings for UI/logging (action name -> key list).
    std::vector<std::pair<std::string, std::string>> describeAll() const;
    std::string describeAction(Action a) const;

    // Utilities for UI/editor: produce a parseable chord token string (e.g. "ctrl+f", "shift+comma").
    // Note: This uses the same canonical tokens as describeAction()/the INI parser.
    static bool isModifierKey(SDL_Keycode key);
    static std::string chordToString(SDL_Keycode key, Uint16 mods);


private:
    std::unordered_map<Action, std::vector<KeyChord>, ActionHash> binds;

    static Uint16 normalizeMods(Uint16 mods);
    static bool chordMatches(const KeyChord& chord, SDL_Keycode key, Uint16 mods);

    static std::optional<Action> parseActionName(const std::string& bindKey);
    static std::vector<KeyChord> parseChordList(const std::string& value);

    static std::string trim(std::string s);
    static std::string toLower(std::string s);
    static std::vector<std::string> split(const std::string& s, char delim);

    static std::optional<KeyChord> parseChord(const std::string& token);
    static SDL_Keycode parseKeycode(const std::string& keyName, Uint16* impliedMods);
};
