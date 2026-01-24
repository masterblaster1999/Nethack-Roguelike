#pragma once

// SDL-free action metadata shared by the core game (extended commands, UI strings)
// and the SDL layer (keybind parsing / rendering).
//
// The canonical action token is the part after `bind_` in procrogue_settings.ini.
// Example: `bind_threat_preview = ctrl+t`  -> token is `threat_preview`.

#include "game.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <cctype>

namespace actioninfo {

struct ActionInfo {
    Action action;
    const char* token; // canonical token used in bind_<token>
    const char* desc;  // short, user-facing description (optional)
};

// NOTE: Order matters: it is used by UI listings (#binds and the keybinds overlay).
inline constexpr ActionInfo kActionInfoTable[] = {
    // Movement
    {Action::Up,        "up",         "Move north"},
    {Action::Down,      "down",       "Move south"},
    {Action::Left,      "left",       "Move west"},
    {Action::Right,     "right",      "Move east"},
    {Action::UpLeft,    "up_left",    "Move northwest"},
    {Action::UpRight,   "up_right",   "Move northeast"},
    {Action::DownLeft,  "down_left",  "Move southwest"},
    {Action::DownRight, "down_right", "Move southeast"},

    // Core actions
    {Action::Confirm, "confirm", "Confirm / interact / select"},
    {Action::Cancel,  "cancel",  "Cancel / close"},
    {Action::Wait,    "wait",    "Wait a turn"},
    {Action::Parry,   "parry",   "Parry (ready a riposte)"},
    {Action::Rest,    "rest",    "Rest until healed / interrupted"},
    {Action::ToggleSneak, "sneak", "Toggle sneak (quieter movement)"},
    {Action::Evade,   "evade",   "Smart step away from visible threats"},
    {Action::Pickup,  "pickup",  "Pick up items"},
    {Action::Inventory, "inventory", "Open inventory"},
    {Action::Fire,    "fire",    "Ranged attack / fire"},
    {Action::Search,  "search",  "Search nearby tiles"},
    {Action::Disarm,  "disarm",  "Disarm adjacent trap"},
    {Action::CloseDoor, "close_door", "Close adjacent door"},
    {Action::LockDoor,  "lock_door",  "Lock adjacent door"},
    {Action::Kick,    "kick",    "Kick"},
    {Action::Dig,     "dig",     "Dig / tunnel (requires pickaxe)"},
    {Action::Look,    "look",    "Enter LOOK mode"},
    {Action::StairsUp,   "stairs_up",   "Go up stairs"},
    {Action::StairsDown, "stairs_down", "Go down stairs"},
    {Action::AutoExplore, "auto_explore", "Auto-explore"},
    {Action::ToggleAutoPickup, "toggle_auto_pickup", "Cycle auto-pickup mode"},

    // Inventory-specific
    {Action::Equip,         "equip",          "Equip selected item"},
    {Action::Use,           "use",            "Use selected item"},
    {Action::Drop,          "drop",           "Drop selected stack"},
    {Action::DropAll,       "drop_all",       "Drop entire stack"},
    {Action::SortInventory, "sort_inventory", "Sort inventory"},

    // UI / meta
    {Action::Help,           "help",            "Open help"},
    {Action::MessageHistory, "message_history", "Open message history"},
    {Action::Codex,          "codex",           "Open monster codex"},
    {Action::Discoveries,    "discoveries",     "Open discoveries"},
    {Action::Spells,         "spells",          "Open spells"},
    {Action::Options,        "options",         "Open options menu"},
    {Action::Command,        "command",         "Open extended command prompt"},

    {Action::ToggleMinimap,   "toggle_minimap",   "Toggle minimap"},
    {Action::MinimapZoomIn,   "minimap_zoom_in",  "Minimap zoom in"},
    {Action::MinimapZoomOut,  "minimap_zoom_out", "Minimap zoom out"},
    {Action::ToggleStats,     "toggle_stats",     "Toggle stats panel"},
    {Action::TogglePerfOverlay, "toggle_perf_overlay", "Toggle performance overlay"},
    {Action::ToggleSoundPreview,  "sound_preview",  "Toggle sound preview"},
    {Action::ToggleThreatPreview, "threat_preview", "Toggle threat preview"},
    {Action::ToggleHearingPreview, "hearing_preview", "Toggle hearing (audibility) preview"},
    {Action::ToggleScentPreview,   "scent_preview",   "Toggle scent (trail) preview"},
    {Action::ToggleViewMode,      "toggle_view_mode", "Toggle top-down / isometric"},
    {Action::ToggleVoxelSprites,  "toggle_voxel_sprites", "Toggle 3D voxel sprites"},
    {Action::ToggleFullscreen,    "fullscreen", "Toggle fullscreen"},
    {Action::Screenshot,          "screenshot", "Take screenshot"},

    {Action::Save,     "save",      "Save game"},
    {Action::Load,     "load",      "Load save"},
    {Action::LoadAuto, "load_auto", "Load autosave"},
    {Action::Restart,  "restart",   "Restart run"},

    {Action::Scores, "scores", "Show high scores"},

    {Action::LogUp,   "log_up",   "Scroll message log up"},
    {Action::LogDown, "log_down", "Scroll message log down"},
};

inline const ActionInfo* find(Action a) {
    for (const auto& info : kActionInfoTable) {
        if (info.action == a) return &info;
    }
    return nullptr;
}

inline const ActionInfo* findByToken(std::string_view token) {
    for (const auto& info : kActionInfoTable) {
        if (token == info.token) return &info;
    }
    return nullptr;
}

inline const char* token(Action a) {
    if (const auto* info = find(a)) return info->token;
    return "";
}

inline const char* desc(Action a) {
    if (const auto* info = find(a)) return info->desc ? info->desc : "";
    return "";
}

inline std::string trim(std::string_view sv) {
    size_t b = 0;
    size_t e = sv.size();
    while (b < e && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

inline std::string toLower(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

inline std::string normalizeToken(std::string_view in) {
    std::string s = toLower(trim(in));
    if (s.empty()) return s;

    // Allow passing full INI keys (bind_<token>) as well as raw action tokens.
    if (s.rfind("bind_", 0) == 0) {
        s = s.substr(5);
    }

    // Normalize common separators.
    for (char& ch : s) {
        if (ch == '-') ch = '_';
    }

    struct Alias { const char* alias; const char* canonical; };
    static constexpr Alias kAliases[] = {
        // Movement shorthands
        {"upleft", "up_left"},
        {"upright", "up_right"},
        {"downleft", "down_left"},
        {"downright", "down_right"},

        // Core shorthands
        {"ok", "confirm"},
        {"escape", "cancel"},
        {"esc", "cancel"},

        {"dropall", "drop_all"},
        {"sortinventory", "sort_inventory"},
        {"togglesneak", "sneak"},
        {"toggle_sneak", "sneak"},

        {"pick_up", "pickup"},
        {"pick", "pickup"},
        {"inv", "inventory"},

        {"untrap", "disarm"},

        {"closedoor", "close_door"},
        {"close", "close_door"},
        {"lockdoor", "lock_door"},
        {"lock", "lock_door"},

        {"tunnel", "dig"},

        {"stairsup", "stairs_up"},
        {"stairsdown", "stairs_down"},

        {"autoexplore", "auto_explore"},

        {"toggleautopickup", "toggle_auto_pickup"},
        {"autopickup", "toggle_auto_pickup"},

        // UI / meta aliases
        {"minimap", "toggle_minimap"},
        {"toggleminimap", "toggle_minimap"},
        {"minimap_zoomin", "minimap_zoom_in"},
        {"minimapzoomin", "minimap_zoom_in"},
        {"zoom_minimap_in", "minimap_zoom_in"},
        {"minimap_zoomout", "minimap_zoom_out"},
        {"minimapzoomout", "minimap_zoom_out"},
        {"zoom_minimap_out", "minimap_zoom_out"},

        {"stats", "toggle_stats"},
        {"togglestats", "toggle_stats"},

        {"toggle_perf", "toggle_perf_overlay"},
        {"toggleperf", "toggle_perf_overlay"},
        {"perf", "toggle_perf_overlay"},
        {"perf_overlay", "toggle_perf_overlay"},

        {"toggle_sound_preview", "sound_preview"},
        {"togglesoundpreview", "sound_preview"},
        {"soundpreview", "sound_preview"},
        {"acoustic_preview", "sound_preview"},
        {"acoustic", "sound_preview"},

        {"toggle_hearing_preview", "hearing_preview"},
        {"togglehearingpreview", "hearing_preview"},
        {"hearingpreview", "hearing_preview"},
        {"audibility_preview", "hearing_preview"},
        {"audibility", "hearing_preview"},

        {"toggle_scent_preview", "scent_preview"},
        {"togglescentpreview", "scent_preview"},
        {"scentpreview", "scent_preview"},
        {"scent", "scent_preview"},
        {"smell_preview", "scent_preview"},
        {"smell", "scent_preview"},
        {"olfactory", "scent_preview"},

        {"toggle_threat_preview", "threat_preview"},
        {"togglethreatpreview", "threat_preview"},
        {"threatpreview", "threat_preview"},
        {"danger_preview", "threat_preview"},
        {"danger", "threat_preview"},

        {"view_mode", "toggle_view_mode"},
        {"toggle_iso", "toggle_view_mode"},
        {"toggleviewmode", "toggle_view_mode"},
        {"isometric", "toggle_view_mode"},

        {"voxel_sprites", "toggle_voxel_sprites"},
        {"toggle_3d_sprites", "toggle_voxel_sprites"},
        {"togglevoxelsprites", "toggle_voxel_sprites"},
        {"sprites3d", "toggle_voxel_sprites"},

        {"toggle_fullscreen", "fullscreen"},
        {"togglefullscreen", "fullscreen"},
        {"take_screenshot", "screenshot"},

        {"messagehistory", "message_history"},
        {"messages", "message_history"},
        {"msghistory", "message_history"},
        {"msglog", "message_history"},

        {"monster_codex", "codex"},
        {"bestiary", "codex"},
        {"monsters", "codex"},

        {"discovery", "discoveries"},
        {"identify_list", "discoveries"},

        {"spellbook", "spells"},
        {"cast", "spells"},

        {"extcmd", "command"},

        {"loadauto", "load_auto"},
        {"newgame", "restart"},

        {"hall", "scores"},
        {"halloffame", "scores"},

        {"logup", "log_up"},
        {"logdown", "log_down"},

        // Tactical synonyms
        {"flee", "evade"},
        {"panic", "evade"},
    };

    for (const auto& a : kAliases) {
        if (s == a.alias) return std::string(a.canonical);
    }

    return s;
}

inline std::optional<Action> parse(std::string_view in) {
    const std::string tok = normalizeToken(in);
    if (tok.empty()) return std::nullopt;

    if (const auto* info = findByToken(tok)) return info->action;
    return std::nullopt;
}

inline std::string bindKey(Action a) {
    const char* t = token(a);
    if (!t || t[0] == '\0') return std::string();
    return std::string("bind_") + t;
}

} // namespace actioninfo
