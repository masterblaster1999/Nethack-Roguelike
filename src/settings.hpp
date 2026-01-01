#pragma once

#include <string>

#include "game.hpp"

// Simple user-editable settings file (INI-ish: key = value).
// The file is created next to the save file (SDL_GetPrefPath) on first run.
struct Settings {
    // Default tuned for the current (larger) map size so the initial window fits
    // comfortably on typical displays.
    int tileSize = 16;
    int hudHeight = 160;
    bool startFullscreen = false;

    // Player identity (used for the scoreboard and HUD)
    std::string playerName = "PLAYER";

    // Default starting class for new runs.
    // player_class: adventurer|knight|rogue|archer|wizard
    PlayerClass playerClass = PlayerClass::Adventurer;

    // HUD status tags: show remaining turns for timed effects (POISON(6), REGEN(3), ...)
    bool showEffectTimers = true;

    // UI skin (purely cosmetic)
    UITheme uiTheme = UITheme::DarkStone;   // darkstone|parchment|arcane
    bool uiPanelsTextured = true;           // textured|solid
    bool voxelSprites = true;               // true = 3D voxel-extruded sprites (entities/items/projectiles)

    // Rendering / performance
    // - vsync: enables SDL_Renderer vsync (lower CPU usage, smoother rendering).
    // - maxFps: optional software cap when vsync is disabled (0 = uncapped).
    // - textureCacheMB: approximate VRAM budget for cached entity/item/projectile textures.
    //   0 disables eviction (unlimited).
    int textureCacheMB = 256;
    bool vsync = true;
    int maxFps = 0; // 0 or 30..240

    // Input
    bool controllerEnabled = true;

    // Control preset (convenience: applies a cohesive bind_* scheme)
    // control_preset: modern|nethack
    ControlPreset controlPreset = ControlPreset::Modern;

    // Gameplay QoL
    AutoPickupMode autoPickup = AutoPickupMode::Gold; // off|gold|smart|all
    int autoStepDelayMs = 45; // auto-move speed (10..500)

    // Autosave (0 = off)
    int autosaveEveryTurns = 200;

    // Save backups
    // How many rotated backups to keep for manual saves and autosaves:
    // - 0 disables backups
    // - 1 keeps <file>.bak1
    // - N keeps <file>.bak1 ... <file>.bakN
    int saveBackups = 3;

    // Default save slot name.
    // - Empty means "default" (procrogue_save.dat)
    // - Non-empty means procrogue_save_<slot>.dat
    // This is overridden by the CLI flag: --slot <name>
    std::string defaultSlot;

    // NetHack-style item identification (potions/scrolls start unknown each run).
    // If false, items always show their true names (more "arcade" / beginner-friendly).
    bool identifyItems = true;
    bool hungerEnabled = false; // Optional hunger system (adds food).
    bool encumbranceEnabled = false; // Optional carrying capacity / burden system.
    bool lightingEnabled = false; // Optional darkness/lighting system (requires torches on deeper floors).
    bool yendorDoomEnabled = true; // Endgame escalation after acquiring the Amulet of Yendor.
    bool confirmQuit = true;   // Require ESC twice to quit (prevents accidental quits).
    bool autoMortem = true;   // Write a procrogue_mortem_*.txt dump automatically on win/death.
    bool bonesEnabled = true; // Allow "bones files" (persistent death remnants between runs).
};

// Loads settings from disk. If the file is missing or invalid, defaults are used.
Settings loadSettings(const std::string& path);

// Update (or append) a single key=value entry in the settings file.
// Returns false if the file could not be read/written.
bool updateIniKey(const std::string& path, const std::string& key, const std::string& value);

// Remove a single key entry from the settings file.
// Returns false only if the file could not be read/written.
bool removeIniKey(const std::string& path, const std::string& key);

// Writes a commented default settings file. Returns true on success.
bool writeDefaultSettings(const std::string& path);
