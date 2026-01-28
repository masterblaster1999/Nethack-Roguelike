#pragma once

#include <string>

#include "game.hpp"

// Simple user-editable settings file (INI-ish: key = value).
// The file is created next to the save file (SDL_GetPrefPath) on first run.
struct Settings {
    // Default tuned for the current (larger) map size so the initial window fits
    // comfortably on typical displays.
    int tileSize = 13;
    int hudHeight = 160;
    bool startFullscreen = false;

    // Minimap zoom level (UI-only): -3..3 (0 = default auto-fit)
    int minimapZoom = 0;

    // Viewport / camera
    // view_w/view_h are in tiles:
    // - 0 means auto-fit to your current display (and uses full-map view when it fits).
    // - if smaller than the map, the renderer enables a scrolling camera that follows the player
    //   (and the look/target cursor when those modes are active).
    int viewW = 0;
    int viewH = 0;

    // Camera presentation / view mode
    // view_mode: topdown|isometric|3d
    ViewMode viewMode = ViewMode::TopDown;

    // Raycast 3D view options (visual-only)
    // These only affect the experimental first-person/pseudo-3D view mode.
    int raycast3DScale = 2;     // raycast3d_scale: 1..4 (higher = faster, lower = sharper)
    int raycast3DFovDeg = 67;   // raycast3d_fov: 40..100 (horizontal field of view in degrees)
    bool raycast3DCeiling = true; // raycast3d_ceiling: true|false (textured ceiling vs simple gradient)
    bool raycast3DBump = true;    // raycast3d_bump: true|false (normal-mapped relief shading)

    // Extra 3D surfacing controls (visual-only)
    bool raycast3DParallax = true;       // raycast3d_parallax: true|false (parallax texture mapping for extra depth)
    int raycast3DParallaxStrength = 60;  // raycast3d_parallax_strength: 0..100 (parallax depth)
    bool raycast3DSpecular = true;       // raycast3d_specular: true|false (material specular highlights)
    int raycast3DSpecularStrength = 70;  // raycast3d_specular_strength: 0..100 (specular intensity)

    // Raycast3D camera controls (visual-only)
    bool raycast3DFollowMove = true;     // raycast3d_follow_move: true|false (camera dir follows player movement)
    int raycast3DTurnDegrees = 15;       // raycast3d_turn_deg: 1..90 (degrees per turn action)

    // Raycast 3D billboards (visual-only)
    // These only affect view_mode = 3d.
    bool raycast3DSprites = true; // raycast3d_sprites: true|false (render entity billboards in the 3D view)
    bool raycast3DItems = true;   // raycast3d_items: true|false (render ground-item billboards in the 3D view)


    // Player identity (used for the scoreboard and HUD)
    std::string playerName = "PLAYER";

    // Default starting class for new runs.
    // player_class: adventurer|knight|rogue|archer|wizard
    PlayerClass playerClass = PlayerClass::Adventurer;

    // HUD status tags: show remaining turns for timed effects (POISON(6), REGEN(3), ...)
    bool showEffectTimers = true;

    // Debug HUD overlay (FPS + texture cache stats). UI-only.
    bool showPerfOverlay = false;

    // UI skin (purely cosmetic)
    UITheme uiTheme = UITheme::DarkStone;   // darkstone|parchment|arcane
    bool uiPanelsTextured = true;           // textured|solid
    bool voxelSprites = true;               // true = 3D voxel-extruded sprites (entities/items/projectiles)
    bool isoVoxelRaytrace = false;        // isometric-only: raytraced voxel sprites instead of mesh rasterization
    bool isoTerrainVoxelBlocks = true; // isometric-only: render terrain blocks as voxel sprites for 3D cohesion
    bool isoCutaway = true;             // isometric-only: fade foreground walls/doors near the player/cursor (cutaway) for readability

    // Procedural terrain palette (purely cosmetic).
    // Applies a seed-derived color palette to floor/wall/chasm shading so each run/floor can
    // have its own mood beyond the base sprite colors.
    bool procPalette = true;              // proc_palette: true|false
    int procPaletteStrength = 70;         // proc_palette_strength: 0..100 (0 disables the effect)

    // Optional user controls for the procedural palette:
    // - Hue shift (degrees): rotates the palette in HSV space.
    // - Saturation/brightness (%): scales HSV S/V.
    // - Spatial strength (%): adds a smooth, low-frequency chroma field across the map.
    int procPaletteHueDeg = 0;            // proc_palette_hue_deg: -45..45
    int procPaletteSaturationPct = 0;     // proc_palette_sat_pct: -80..80
    int procPaletteBrightnessPct = 0;     // proc_palette_bright_pct: -60..60
    int procPaletteSpatialStrength = 35;  // proc_palette_spatial: 0..100


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

    // Auto-explore can optionally spend turns searching dead-ends/corridor corners for secret doors
    // once the floor appears fully explored.
    bool autoExploreSearch = false;

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

    // Endless / infinite world (experimental): allow descending beyond the normal bottom.
    bool infiniteWorld = false;

    // Infinite world memory cap: keep a sliding window of post-quest levels cached.
    // 0 disables pruning.
    int infiniteKeepWindow = 12;
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
