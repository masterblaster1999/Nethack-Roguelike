#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string ltrim(std::string s) {
    s.erase(s.begin(),
        std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    return s;
}

std::string rtrim(std::string s) {
    s.erase(
        std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
        s.end());
    return s;
}

std::string trim(std::string s) {
    return rtrim(ltrim(std::move(s)));
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parseBool(const std::string& v, bool& out) {
    const std::string s = toLower(trim(v));
    if (s == "1" || s == "true" || s == "yes" || s == "on") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parseInt(const std::string& v, int& out) {
    try {
        out = std::stoi(trim(v));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

Settings loadSettings(const std::string& path) {
    Settings s;

    std::ifstream f(path);
    if (!f) return s;

    std::string line;
    while (std::getline(f, line)) {
        // Strip comments (# or ;)
        auto hash = line.find('#');
        auto semi = line.find(';');
        size_t cut = std::min(hash == std::string::npos ? line.size() : hash,
                              semi == std::string::npos ? line.size() : semi);
        line = line.substr(0, cut);

        line = trim(line);
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = toLower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        if (key == "tile_size") {
            int v = 0;
            if (parseInt(val, v)) s.tileSize = std::clamp(v, 16, 96);
        } else if (key == "hud_height") {
            int v = 0;
            if (parseInt(val, v)) s.hudHeight = std::clamp(v, 120, 240);
        } else if (key == "start_fullscreen") {
            bool b = false;
            if (parseBool(val, b)) s.startFullscreen = b;
        } else if (key == "vsync") {
            bool b = true;
            if (parseBool(val, b)) s.vsync = b;
        } else if (key == "max_fps") {
            int v = 0;
            if (parseInt(val, v)) {
                if (v <= 0) s.maxFps = 0;
                else s.maxFps = std::clamp(v, 30, 240);
            }
        } else if (key == "controller_enabled") {
            bool b = true;
            if (parseBool(val, b)) s.controllerEnabled = b;
        } else if (key == "auto_step_delay_ms") {
            int v = 0;
            if (parseInt(val, v)) s.autoStepDelayMs = std::clamp(v, 10, 500);
        } else if (key == "auto_pickup") {
            std::string v = toLower(val);
            if (v == "off") s.autoPickup = AutoPickupMode::Off;
            else if (v == "gold") s.autoPickup = AutoPickupMode::Gold;
            else if (v == "all") s.autoPickup = AutoPickupMode::All;
        } else if (key == "autosave_every_turns") {
            int v = 0;
            if (parseInt(val, v)) s.autosaveEveryTurns = std::clamp(v, 0, 5000);
        } else if (key == "identify_items") {
            bool b = true;
            if (parseBool(val, b)) s.identifyItems = b;
        }
    }

    return s;
}

bool writeDefaultSettings(const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;

    // Use a raw string literal to avoid fragile escaping / accidental newline-in-string bugs.
    f << R"INI(# ProcRogue settings
#
# Lines are: key = value
# Comments start with # or ;
#
# This file is auto-created on first run. Edit it and restart the game.

# Rendering / UI
tile_size = 32
hud_height = 160
start_fullscreen = false

# Rendering / performance
# vsync: true/false  (true = lower CPU usage, smoother rendering)
vsync = true
# max_fps: 0 disables; otherwise 30..240 (only used when vsync=false)
max_fps = 0

# Input
# controller_enabled: true/false  (enables SDL2 game controller support)
controller_enabled = true

# Gameplay QoL
# auto_pickup: off | gold | all
auto_pickup = gold
# auto_step_delay_ms: 10..500 (lower = faster auto-move)
auto_step_delay_ms = 45

# Item identification
# identify_items: true/false  (true = potions/scrolls start unidentified)
identify_items = true

# Autosave
# autosave_every_turns: 0 disables; otherwise saves an autosave file every N turns.
autosave_every_turns = 200

# -----------------------------------------------------------------------------
# Keybindings
#
# Rebind keys by adding entries of the form:
#   bind_<action> = key[, key, ...]
#
# Modifiers: shift, ctrl, alt. Example: shift+comma
# Tip: for '<' and '>' on most layouts, use shift+comma / shift+period.
#
# Set a binding to "none" to disable it.
# -----------------------------------------------------------------------------

# Movement
bind_up = w, up, kp_8
bind_down = s, down, kp_2
bind_left = a, left, kp_4
bind_right = d, right, kp_6
bind_up_left = q, kp_7
bind_up_right = e, kp_9
bind_down_left = z, kp_1
bind_down_right = c, kp_3

# Actions
bind_confirm = enter, kp_enter
bind_cancel = escape, backspace
bind_wait = space, period
bind_rest = r
bind_pickup = g, comma, kp_0
bind_inventory = i, tab
bind_fire = f
bind_search = c
bind_look = l, v
bind_stairs_up = shift+comma, less
bind_stairs_down = shift+period, greater
bind_auto_explore = o
bind_toggle_auto_pickup = p

# Inventory-specific
bind_equip = e
bind_use = u
bind_drop = x
bind_drop_all = shift+x
bind_sort_inventory = shift+s

# UI / meta
bind_help = f1, shift+slash, h
bind_options = f2
bind_command = shift+3
bind_toggle_minimap = m
bind_toggle_stats = shift+tab
bind_save = f5
bind_restart = f6
bind_load = f9
bind_load_auto = f10
bind_log_up = pageup
bind_log_down = pagedown
)INI";

    return true;
}

bool updateIniKey(const std::string& path, const std::string& key, const std::string& value) {
    std::ifstream in(path);
    if (!in) return false;

    std::vector<std::string> lines;
    std::string line;

    const auto keyLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    const auto trimLocal = [](std::string s) {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    };

    bool found = false;
    while (std::getline(in, line)) {
        std::string raw = line;

        // Strip comments for matching, but preserve the original line for output when not matching.
        auto commentPos = raw.find_first_of("#;");
        if (commentPos != std::string::npos) raw = raw.substr(0, commentPos);

        auto eq = raw.find('=');
        if (eq != std::string::npos) {
            std::string k = trimLocal(raw.substr(0, eq));
            if (!k.empty() && keyLower(k) == keyLower(key)) {
                lines.push_back(key + " = " + value);
                found = true;
                continue;
            }
        }

        lines.push_back(line);
    }
    in.close();

    if (!found) {
        // Append at end.
        lines.push_back(key + " = " + value);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\n";
    }
    return true;
}
