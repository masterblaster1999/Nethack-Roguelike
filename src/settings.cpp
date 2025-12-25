#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

static std::string ltrim(std::string s) {
    s.erase(s.begin(),
        std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    return s;
}

static std::string rtrim(std::string s) {
    s.erase(
        std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
        s.end());
    return s;
}

static std::string trim(std::string s) {
    return rtrim(ltrim(std::move(s)));
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static bool parseBool(const std::string& v, bool& out) {
    const std::string s = toLower(trim(v));
    if (s == "1" || s == "true" || s == "yes" || s == "on") { out = true; return true; }
    if (s == "0" || s == "false" || s == "no"  || s == "off") { out = false; return true; }
    return false;
}

static bool parseInt(const std::string& v, int& out) {
    try {
        out = std::stoi(trim(v));
        return true;
    } catch (...) {
        return false;
    }
}

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
        }
    }

    return s;
}

bool writeDefaultSettings(const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;

    f << "# ProcRogue settings\n";
    f << "# Edit this file and restart the game.\n\n";

    f << "# Rendering / UI\n";
    f << "tile_size = 32\n";
    f << "hud_height = 160\n";
    f << "start_fullscreen = false\n\n";

    f << "# Gameplay QoL\n";
    f << "# auto_pickup: off | gold | all\n";
    f << "auto_pickup = gold\n";
    f << "# auto_step_delay_ms: 10..500 (lower = faster auto-move)\n";
    f << "auto_step_delay_ms = 45\n\n";

    f << "# Autosave\n";
    f << "# autosave_every_turns: 0 disables; otherwise saves an autosave file every N turns.\n";
    f << "autosave_every_turns = 200\n";

    return true;
}
