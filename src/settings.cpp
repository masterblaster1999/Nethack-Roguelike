#include "settings.hpp"
#include "slot_utils.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if __has_include(<filesystem>)
    #include <filesystem>
    namespace fs = std::filesystem;
#endif

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

void stripUtf8Bom(std::string& s) {
    // Some editors (notably Windows tools) may write a UTF-8 BOM at the start of text files.
    // If present, strip it so header/key parsing works as expected.
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
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

bool atomicWriteTextFile(const std::string& path, const std::string& contents) {
#if __has_include(<filesystem>)
    std::error_code ec;
    const fs::path p(path);
    const fs::path tmp = p.string() + ".tmp";

    // Ensure the parent directory exists (helps for custom/portable data dirs).
    if (!p.parent_path().empty()) {
        std::error_code ecDirs;
        fs::create_directories(p.parent_path(), ecDirs);
    }

    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out.good()) return false;
    }

    // Try rename; on Windows this fails if destination exists.
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(p, ec2);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
    if (ec) {
        // Fallback: copy then remove tmp
        std::error_code ec2;
        fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec2);
        fs::remove(tmp, ec2);
        return !ec2;
    }
    return true;
#else
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return out.good();
#endif
}

} // namespace

Settings loadSettings(const std::string& path) {
    Settings s;

    std::ifstream f(path);
    if (!f) return s;

    std::string line;
    while (std::getline(f, line)) {
        stripUtf8Bom(line);

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
            // Allow both smaller and larger tiles:
            // - smaller tiles help the (increasingly large) map fit on 1080p displays
            // - larger tiles are useful for high-DPI / 4K+ displays
            // Note: window size is MAP_W*tile_size by default.
            if (parseInt(val, v)) s.tileSize = std::clamp(v, 8, 256);
        } else if (key == "hud_height") {
            int v = 0;
            if (parseInt(val, v)) s.hudHeight = std::clamp(v, 120, 240);
        } else if (key == "minimap_zoom" || key == "minimap_scale") {
            int v = 0;
            if (parseInt(val, v)) s.minimapZoom = std::clamp(v, -3, 3);
        } else if (key == "view_w" || key == "view_width" || key == "viewport_w") {
            int v = 0;
            if (parseInt(val, v)) {
                // 0 = auto-fit (choose a viewport that fits the current display).
                // Otherwise interpret as a tile count.
                if (v <= 0) s.viewW = 0;
                else s.viewW = std::clamp(v, 1, Game::MAP_W);
            }
        } else if (key == "view_h" || key == "view_height" || key == "viewport_h") {
            int v = 0;
            if (parseInt(val, v)) {
                if (v <= 0) s.viewH = 0;
                else s.viewH = std::clamp(v, 1, Game::MAP_H);
            }
        } else if (key == "view_mode" || key == "camera_mode" || key == "camera") {
            ViewMode vm = ViewMode::TopDown;
            if (parseViewMode(val, vm)) s.viewMode = vm;
        } else if (key == "isometric_view") {
            bool b = false;
            if (parseBool(val, b)) s.viewMode = b ? ViewMode::Isometric : ViewMode::TopDown;
        } else if (key == "start_fullscreen") {
            bool b = false;
            if (parseBool(val, b)) s.startFullscreen = b;
        } else if (key == "player_name") {
            // Preserved as-is (except trimming). Used in the HUD + scoreboard.
            std::string n = trim(val);
            if (!n.empty()) {
                if (n.size() > 24) n.resize(24);
                s.playerName = n;
            }
        } else if (key == "player_class") {
            PlayerClass pc = PlayerClass::Adventurer;
            if (parsePlayerClass(val, pc)) s.playerClass = pc;
        } else if (key == "show_effect_timers") {
            bool b = true;
            if (parseBool(val, b)) s.showEffectTimers = b;
        } else if (key == "show_perf_overlay" || key == "perf_overlay" || key == "perf_ui") {
            bool b = false;
            if (parseBool(val, b)) s.showPerfOverlay = b;
        } else if (key == "ui_theme") {
            std::string v = toLower(val);
            if (v == "dark" || v == "darkstone" || v == "stone") s.uiTheme = UITheme::DarkStone;
            else if (v == "parchment" || v == "paper") s.uiTheme = UITheme::Parchment;
            else if (v == "arcane" || v == "purple") s.uiTheme = UITheme::Arcane;
        } else if (key == "ui_panels") {
            std::string v = toLower(val);
            bool b = true;
            if (parseBool(val, b)) s.uiPanelsTextured = b;
            else if (v == "textured" || v == "tiles" || v == "tile") s.uiPanelsTextured = true;
            else if (v == "solid" || v == "flat") s.uiPanelsTextured = false;
        } else if (key == "voxel_sprites") {
            bool b = true;
            if (parseBool(val, b)) s.voxelSprites = b;
        } else if (key == "iso_voxel_raytrace" || key == "voxel_iso_raytrace" || key == "isometric_voxel_raytrace") {
            bool b = false;
            if (parseBool(val, b)) s.isoVoxelRaytrace = b;
        } else if (key == "iso_terrain_voxels" || key == "iso_terrain_voxel_blocks" || key == "isometric_terrain_voxels") {
            bool b = s.isoTerrainVoxelBlocks;
            if (parseBool(val, b)) s.isoTerrainVoxelBlocks = b;
        } else if (key == "iso_cutaway" || key == "isometric_cutaway" || key == "iso_wall_cutaway") {
            bool b = true;
            if (parseBool(val, b)) s.isoCutaway = b;
        } else if (key == "proc_palette" || key == "procedural_palette" || key == "terrain_palette") {
            bool b = true;
            if (parseBool(val, b)) s.procPalette = b;
        } else if (key == "proc_palette_strength" || key == "palette_strength" || key == "terrain_palette_strength") {
            int v = 0;
            if (parseInt(val, v)) s.procPaletteStrength = std::clamp(v, 0, 100);
        } else if (key == "texture_cache_mb") {
            int v = 0;
            if (parseInt(val, v)) {
                if (v <= 0) s.textureCacheMB = 0;
                else s.textureCacheMB = std::clamp(v, 16, 2048);
            }
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
        } else if (key == "control_preset") {
            ControlPreset cp = ControlPreset::Modern;
            if (parseControlPreset(val, cp)) s.controlPreset = cp;
        } else if (key == "auto_step_delay_ms") {
            int v = 0;
            if (parseInt(val, v)) s.autoStepDelayMs = std::clamp(v, 10, 500);
        } else if (key == "auto_explore_search") {
            bool b = false;
            if (parseBool(val, b)) s.autoExploreSearch = b;
        } else if (key == "auto_pickup") {
            std::string v = toLower(val);
            if (v == "off") s.autoPickup = AutoPickupMode::Off;
            else if (v == "gold") s.autoPickup = AutoPickupMode::Gold;
            else if (v == "smart") s.autoPickup = AutoPickupMode::Smart;
            else if (v == "all") s.autoPickup = AutoPickupMode::All;
        } else if (key == "autosave_every_turns") {
            int v = 0;
            if (parseInt(val, v)) s.autosaveEveryTurns = std::clamp(v, 0, 5000);
        } else if (key == "save_backups") {
            int v = 0;
            if (parseInt(val, v)) s.saveBackups = std::clamp(v, 0, 10);
        } else if (key == "default_slot") {
            std::string v = trim(val);
            const std::string low = toLower(v);
            if (v.empty() || low == "default" || low == "none" || low == "off") {
                s.defaultSlot.clear();
            } else {
                s.defaultSlot = sanitizeSlotName(v);
            }
        } else if (key == "identify_items") {
            bool b = true;
            if (parseBool(val, b)) s.identifyItems = b;
        } else if (key == "hunger_enabled") {
            bool b = false;
            if (parseBool(val, b)) s.hungerEnabled = b;
        } else if (key == "encumbrance_enabled") {
            bool b = false;
            if (parseBool(val, b)) s.encumbranceEnabled = b;
        } else if (key == "lighting_enabled") {
            bool b = false;
            if (parseBool(val, b)) s.lightingEnabled = b;
        } else if (key == "yendor_doom_enabled") {
            bool b = true;
            if (parseBool(val, b)) s.yendorDoomEnabled = b;
        } else if (key == "confirm_quit") {
            bool b = true;
            if (parseBool(val, b)) s.confirmQuit = b;
        } else if (key == "auto_mortem") {
            bool b = true;
            if (parseBool(val, b)) s.autoMortem = b;
        }
        else if (key == "bones_enabled") {
            bool b = true;
            if (parseBool(val, b)) s.bonesEnabled = b;
        }
        else if (key == "infinite_world") {
            bool b = false;
            if (parseBool(val, b)) s.infiniteWorld = b;
        }
        else if (key == "infinite_keep_window") {
            int v = 0;
            if (parseInt(val, v)) s.infiniteKeepWindow = std::clamp(v, 0, 200);
        }
    }

    return s;
}

bool writeDefaultSettings(const std::string& path) {
    // Use a raw string literal to avoid fragile escaping / accidental newline-in-string bugs.
    const std::string contents = R"INI(# ProcRogue settings
#
# Lines are: key = value
# Comments start with # or ;
#
# This file is auto-created on first run. Edit it and restart the game.

# Rendering / UI
# tile_size: 8..256  (larger = bigger tiles + larger window)
# NOTE: the default map is fairly large; smaller tile sizes help it fit on 1080p displays.
tile_size = 13
hud_height = 160
start_fullscreen = false

# Minimap zoom level (UI-only)
# minimap_zoom: -3..3  (0 = default)
minimap_zoom = 0

# Viewport / camera
# view_w/view_h: 0 = auto-fit to your display; otherwise set a fixed viewport in tiles.
# Example (classic 16:9-ish): view_w = 80, view_h = 45
# If the viewport is smaller than the dungeon map, the renderer uses a scrolling camera that
# follows the player (and the look/target cursor when those modes are active).
view_w = 0
view_h = 0
# view_mode: topdown | isometric
# (isometric is an experimental 2.5D camera; toggle in-game with F7 by default)
view_mode = topdown

# Player identity (used in the HUD + scoreboard)
player_name = PLAYER

# Starting class/role for new runs
# player_class: adventurer | knight | rogue | archer | wizard
player_class = adventurer

# HUD
# show_effect_timers: true/false (shows remaining turns on POISON/REGEN/... in the HUD)
show_effect_timers = true

# show_perf_overlay: true/false (tiny debug HUD: FPS + cache stats)
show_perf_overlay = false

# UI skin (cosmetic)
# ui_theme: darkstone | parchment | arcane
ui_theme = darkstone
# ui_panels: textured | solid
ui_panels = textured

# voxel_sprites: true/false (true = render entities/items/projectiles as tiny 3D voxel sprites)
voxel_sprites = true

# iso_voxel_raytrace: true/false
# Isometric view only. When true, isometric voxel sprites are rendered using the
# custom voxel raytracer (orthographic DDA) instead of the face-meshed isometric rasterizer.
iso_voxel_raytrace = false


# iso_terrain_voxels: true/false
# Isometric view only. When true, wall/door/pillar/boulder "block" sprites are generated
# from small voxel models for cohesive 3D shading with voxel_sprites.
# NOTE: When iso_voxel_raytrace is enabled, terrain blocks use raytrace only for small tile sizes (<=64px).
iso_terrain_voxels = true

# iso_cutaway: true/false
# Isometric view only. When true, foreground walls/doors (in front of the player/cursor)
# are faded ("cutaway") so interiors remain readable.
iso_cutaway = true

# proc_palette: true/false
# Enables the procedural terrain color palette (run/floor-based tinting).
proc_palette = true

# proc_palette_strength: 0..100
# 0 disables the effect entirely; 100 is the most colorful.
proc_palette_strength = 70

# texture_cache_mb: 0 or 16..2048
# Approximate VRAM budget for cached entity/item/projectile textures.
# 0 disables eviction (unlimited). If you use huge tile sizes, consider lowering this.
texture_cache_mb = 256

# Rendering / performance
# vsync: true/false  (true = lower CPU usage, smoother rendering)
vsync = true
# max_fps: 0 disables; otherwise 30..240 (only used when vsync=false)
max_fps = 0

# Input
# controller_enabled: true/false  (enables SDL2 game controller support)
controller_enabled = true
# control_preset: modern | nethack  (convenience: applies a cohesive bind_* scheme)
control_preset = modern

# Gameplay QoL
# auto_pickup: off | gold | smart | all
auto_pickup = gold
# auto_step_delay_ms: 10..500 (lower = faster auto-move)
auto_step_delay_ms = 45
# auto_explore_search: true/false (auto-explore will search dead ends for secret doors)
auto_explore_search = false

# Safety
# confirm_quit: true/false (true = ESC twice to quit)
confirm_quit = true
# auto_mortem: true/false (true = write a mortem dump on win/death)
auto_mortem = true
# bones_enabled: true/false (true = previous runs can leave "bones" behind)
bones_enabled = true


# Optional survival mechanic
# hunger_enabled: true/false (adds food and starvation over time)
hunger_enabled = false

# Optional NetHack-style burden system
# encumbrance_enabled: true/false (carrying capacity + movement penalties)
encumbrance_enabled = false

# Optional darkness / lighting
# lighting_enabled: true/false (dark deeper floors; requires torches)
lighting_enabled = false

# Optional endgame escalation
# yendor_doom_enabled: true/false (after you take the Amulet, the dungeon fights back)
yendor_doom_enabled = true

# Infinite world (experimental)
# infinite_world: true/false (allow descending beyond the normal bottom)
infinite_world = false
# infinite_keep_window: 0 disables pruning; otherwise keeps N post-quest depths cached
infinite_keep_window = 12

# Item identification
# identify_items: true/false  (true = potions/scrolls start unidentified)
identify_items = true

# Autosave
# autosave_every_turns: 0 disables; otherwise saves an autosave file every N turns.
autosave_every_turns = 200

# Save slot
# default_slot: empty (or "default") uses procrogue_save.dat; otherwise procrogue_save_<slot>.dat
default_slot =

# Save backups
# save_backups: 0 disables; otherwise keeps N rotated backups (<file>.bak1..bakN).
save_backups = 3

# -----------------------------------------------------------------------------
# Keybindings
#
# Rebind keys by adding entries of the form:
#   bind_<action> = key[, key, ...]
#
# Modifiers: shift, ctrl, alt, cmd. Example: shift+comma
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
bind_sneak = n
bind_evade = ctrl+e
bind_pickup = g, comma, kp_0
bind_inventory = i, tab
bind_fire = f
bind_search = shift+c
bind_disarm = t
bind_close_door = k
bind_lock_door = shift+k
bind_kick = b
bind_dig = shift+d
bind_look = l, v
bind_sound_preview = ctrl+n
bind_threat_preview = ctrl+t
bind_hearing_preview = ctrl+h
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
bind_help = f1, shift+slash, h, cmd+?
bind_options = f2
bind_command = shift+3
bind_toggle_minimap = m
bind_minimap_zoom_out = [
bind_minimap_zoom_in = ]
bind_toggle_stats = shift+tab
bind_toggle_view_mode = f7
bind_toggle_voxel_sprites = f8
bind_message_history = f3, shift+m
bind_codex = f4
bind_discoveries = backslash
bind_fullscreen = f11
bind_screenshot = f12
bind_save = f5
bind_restart = f6
bind_load = f9
bind_load_auto = f10
bind_toggle_perf_overlay = shift+f10
bind_log_up = pageup
bind_log_down = pagedown
)INI";

    return atomicWriteTextFile(path, contents);
}


bool updateIniKey(const std::string& path, const std::string& key, const std::string& value) {
    std::ifstream in(path);
    if (!in) {
        // File doesn't exist yet; create a minimal one so in-game bind/options commands
        // can still persist changes even if the user deleted their settings.
        const std::string contents = key + " = " + value + "\n";
        return atomicWriteTextFile(path, contents);
    }

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

    const std::string target = keyLower(trimLocal(key));

    bool found = false;
    while (std::getline(in, line)) {
        stripUtf8Bom(line);
        std::string raw = line;

        // Strip comments for matching, but preserve the original line for output when not matching.
        auto commentPos = raw.find_first_of("#;");
        if (commentPos != std::string::npos) raw = raw.substr(0, commentPos);

        auto eq = raw.find('=');
        if (eq != std::string::npos) {
            std::string k = keyLower(trimLocal(raw.substr(0, eq)));
            if (!k.empty() && k == target) {
                if (!found) {
                    lines.push_back(key + " = " + value);
                    found = true;
                } else {
                    // Drop duplicate keys to keep the file unambiguous.
                }
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

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\n";
    }
    out << "\n"; // keep a trailing newline for nicer diffs/editing

    return atomicWriteTextFile(path, out.str());
}
bool removeIniKey(const std::string& path, const std::string& key) {
    std::ifstream in(path);
    if (!in) {
        // Missing file is not an error (nothing to remove).
        return true;
    }

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

    const std::string target = keyLower(trimLocal(key));

    bool removed = false;
    while (std::getline(in, line)) {
        stripUtf8Bom(line);
        std::string raw = line;

        // Strip comments for matching, but preserve the original line for output when not matching.
        auto commentPos = raw.find_first_of("#;");
        if (commentPos != std::string::npos) raw = raw.substr(0, commentPos);

        auto eq = raw.find('=');
        if (eq != std::string::npos) {
            std::string k = keyLower(trimLocal(raw.substr(0, eq)));
            if (!k.empty() && k == target) {
                removed = true;
                continue; // drop the line
            }
        }

        lines.push_back(line);
    }
    in.close();

    // Nothing to remove is not an error.
    if (!removed) return true;

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << "\n";
    }
    if (!lines.empty()) out << "\n";

    return atomicWriteTextFile(path, out.str());
}
