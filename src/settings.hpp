#pragma once

#include <string>

#include "game.hpp"

// Simple user-editable settings file (INI-ish: key = value).
// The file is created next to the save file (SDL_GetPrefPath) on first run.
struct Settings {
    int tileSize = 32;
    int hudHeight = 160;
    bool startFullscreen = false;

    // Rendering / performance
    // - vsync: enables SDL_Renderer vsync (lower CPU usage, smoother rendering).
    // - maxFps: optional software cap when vsync is disabled (0 = uncapped).
    bool vsync = true;
    int maxFps = 0; // 0 or 30..240

    // Input
    bool controllerEnabled = true;

    // Gameplay QoL
    AutoPickupMode autoPickup = AutoPickupMode::Gold; // off|gold|all
    int autoStepDelayMs = 45; // auto-move speed (10..500)

    // Autosave (0 = off)
    int autosaveEveryTurns = 200;

    // NetHack-style item identification (potions/scrolls start unknown each run).
    // If false, items always show their true names (more "arcade" / beginner-friendly).
    bool identifyItems = true;
};

// Loads settings from disk. If the file is missing or invalid, defaults are used.
Settings loadSettings(const std::string& path);

// Writes a commented default settings file. Returns true on success.
bool writeDefaultSettings(const std::string& path);
