#pragma once

#include <string>

#include "game.hpp"

// Simple user-editable settings file (INI-ish: key = value).
// The file is created next to the save file (SDL_GetPrefPath) on first run.
struct Settings {
    int tileSize = 32;
    int hudHeight = 160;
    bool startFullscreen = false;

    // Gameplay QoL
    AutoPickupMode autoPickup = AutoPickupMode::Gold; // off|gold|all
    int autoStepDelayMs = 45; // auto-move speed (10..500)
};

// Loads settings from disk. If the file is missing or invalid, defaults are used.
Settings loadSettings(const std::string& path);

// Writes a commented default settings file. Returns true on success.
bool writeDefaultSettings(const std::string& path);
