#pragma once
#include "common.hpp"
#include "dungeon.hpp"
#include "items.hpp"
#include "spells.hpp"
#include "effects.hpp"
#include "rng.hpp"
#include "scores.hpp"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Dungeon topology: a run can span multiple branches (e.g., Main dungeon, Camp hub).
//
// First pass: we only expose Camp vs Main, but this is intentionally modeled as a
// real branch dimension so future side-dungeons can slot in without overloading
// the meaning of numeric depth.
enum class DungeonBranch : uint8_t {
    Camp = 0,
    Main = 1,
};

// A unique identifier for a level within the run.
//
// NOTE: We keep this comparable so it can be used as a std::map key.
struct LevelId {
    DungeonBranch branch = DungeonBranch::Main;
    int depth = 1;

    bool operator<(const LevelId& o) const {
        const uint8_t a = static_cast<uint8_t>(branch);
        const uint8_t b = static_cast<uint8_t>(o.branch);
        if (a != b) return a < b;
        return depth < o.depth;
    }
    bool operator==(const LevelId& o) const {
        return branch == o.branch && depth == o.depth;
    }
};

enum class EntityKind : uint8_t {
    Player = 0,
    Goblin,
    Orc,
    Bat,
    Slime,
    SkeletonArcher,
    KoboldSlinger,
    Wolf,
    Troll,
    Wizard,

    // --- New monsters (appended to keep save compatibility) ---
    Snake,
    Spider,
    Ogre,
    Mimic,
    // --- Shops (appended to keep save compatibility) ---
    Shopkeeper,
    Minotaur,

    // Companions / allies (append-only to keep save compatibility)
    Dog,

    // Undead / special (append-only to keep save compatibility)
    Ghost,

    // Tricksters / thieves (append-only to keep save compatibility)
    Leprechaun,

    // Undead (append-only to keep save compatibility)
    Zombie,

    // Merchant guild enforcement (append-only to keep save compatibility)
    Guard,

    // Beautiful but dangerous thieves (append-only to keep save compatibility)
    Nymph,
};

inline constexpr int ENTITY_KIND_COUNT = static_cast<int>(EntityKind::Nymph) + 1;

inline const char* entityKindName(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return "PLAYER";
        case EntityKind::Goblin: return "GOBLIN";
        case EntityKind::Orc: return "ORC";
        case EntityKind::Bat: return "BAT";
        case EntityKind::Slime: return "SLIME";
        case EntityKind::SkeletonArcher: return "SKELETON ARCHER";
        case EntityKind::KoboldSlinger: return "KOBOLD SLINGER";
        case EntityKind::Wolf: return "WOLF";
        case EntityKind::Troll: return "TROLL";
        case EntityKind::Wizard: return "WIZARD";
        case EntityKind::Snake: return "SNAKE";
        case EntityKind::Spider: return "SPIDER";
        case EntityKind::Ogre: return "OGRE";
        case EntityKind::Mimic: return "MIMIC";
        case EntityKind::Shopkeeper: return "SHOPKEEPER";
        case EntityKind::Minotaur: return "MINOTAUR";
        case EntityKind::Dog: return "DOG";
        case EntityKind::Ghost: return "GHOST";
        case EntityKind::Leprechaun: return "LEPRECHAUN";
        case EntityKind::Zombie: return "ZOMBIE";
        case EntityKind::Guard: return "GUARD";
        case EntityKind::Nymph: return "NYMPH";
        default: return "UNKNOWN";
    }
}


// -----------------------------------------------------------------------------
// Hearing (noise sensitivity)
//
// Monsters have small per-kind hearing differences.
// Hearing is expressed as a modifier against positional noise 'volume'
// (both are in tile-cost units).
// -----------------------------------------------------------------------------
inline constexpr int BASE_HEARING = 8;

inline int entityHearing(EntityKind k) {
    switch (k) {
        case EntityKind::Bat:            return 12;
        case EntityKind::Wolf:           return 10;
        case EntityKind::Snake:          return 9;
        case EntityKind::Wizard:         return 9;
        case EntityKind::Spider:         return 8;
        case EntityKind::Goblin:         return 8;
        case EntityKind::Leprechaun:     return 11;
        case EntityKind::Nymph:          return 10;
        case EntityKind::Orc:            return 8;
        case EntityKind::KoboldSlinger:  return 8;
        case EntityKind::SkeletonArcher: return 7;
        case EntityKind::Troll:          return 7;
        case EntityKind::Ogre:           return 7;
        case EntityKind::Shopkeeper:     return 10;
        case EntityKind::Slime:          return 6;
        case EntityKind::Mimic:          return 5;
        default:                         return BASE_HEARING;
    }
}

inline int entityHearingDelta(EntityKind k) {
    return entityHearing(k) - BASE_HEARING;
}


// -----------------------------------------------------------------------------
// Procedural monster variants (rank + affixes)
//
// These are lightweight "roguelike affixes" applied at spawn time to create
// rare standout monsters without introducing new EntityKind ids.
// Persisted in saves so a monster keeps its rolled variant across load.
// -----------------------------------------------------------------------------

enum class ProcMonsterRank : uint8_t {
    Normal = 0,
    Elite = 1,
    Champion = 2,
    Mythic = 3,
};

inline const char* procMonsterRankName(ProcMonsterRank r) {
    switch (r) {
        case ProcMonsterRank::Normal:   return "NORMAL";
        case ProcMonsterRank::Elite:    return "ELITE";
        case ProcMonsterRank::Champion: return "CHAMPION";
        case ProcMonsterRank::Mythic:   return "MYTHIC";
        default:                        return "NORMAL";
    }
}

// Bitmask affixes for procedural monsters. Keep these sparse and append-only
// so the save format remains forward-compatible.
enum class ProcMonsterAffix : uint32_t {
    None      = 0u,
    Swift     = 1u << 0, // faster energy gain
    Stonehide = 1u << 1, // more defense + a bit more HP
    Savage    = 1u << 2, // more attack (melee and ranged)
    Blinking  = 1u << 3, // AI: panic-blink reposition when wounded
    Gilded    = 1u << 4, // loot: bonus gold / key chance

    // On-hit procedural affixes (combat procs).
    // These are intentionally lightweight "status hooks" rather than brand-new mechanics.
    // Append-only: do not reuse bits (save format stores the raw mask).
    Venomous  = 1u << 5, // attacks can poison
    Flaming   = 1u << 6, // attacks can ignite (burn)
    Vampiric  = 1u << 7, // attacks can drain life (heal attacker)
    Webbing   = 1u << 8, // attacks can ensnare (web)
};

inline constexpr uint32_t procAffixBit(ProcMonsterAffix a) {
    return static_cast<uint32_t>(a);
}

inline constexpr bool procHasAffix(uint32_t mask, ProcMonsterAffix a) {
    return (mask & procAffixBit(a)) != 0u;
}

// Canonical ordered list of all procedural monster affix bits.
// Useful for UI (listing) and deterministic iteration (save-forward compatible).
inline constexpr ProcMonsterAffix PROC_MONSTER_AFFIX_ALL[] = {
    ProcMonsterAffix::Swift,
    ProcMonsterAffix::Stonehide,
    ProcMonsterAffix::Savage,
    ProcMonsterAffix::Blinking,
    ProcMonsterAffix::Gilded,
    ProcMonsterAffix::Venomous,
    ProcMonsterAffix::Flaming,
    ProcMonsterAffix::Vampiric,
    ProcMonsterAffix::Webbing,
};

inline const char* procMonsterAffixName(ProcMonsterAffix a) {
    switch (a) {
        case ProcMonsterAffix::Swift:     return "SWIFT";
        case ProcMonsterAffix::Stonehide: return "STONEHIDE";
        case ProcMonsterAffix::Savage:    return "SAVAGE";
        case ProcMonsterAffix::Blinking:  return "BLINKING";
        case ProcMonsterAffix::Gilded:    return "GILDED";
        case ProcMonsterAffix::Venomous:  return "VENOMOUS";
        case ProcMonsterAffix::Flaming:   return "FLAMING";
        case ProcMonsterAffix::Vampiric:  return "VAMPIRIC";
        case ProcMonsterAffix::Webbing:   return "WEBBING";
        default:                          return "UNKNOWN";
    }
}

inline int procMonsterAffixCount(uint32_t mask) {
    int n = 0;
    for (ProcMonsterAffix a : PROC_MONSTER_AFFIX_ALL) {
        if (procHasAffix(mask, a)) ++n;
    }
    return n;
}

inline std::string procMonsterAffixList(uint32_t mask, const char* sep = ", ") {
    std::string out;
    bool first = true;
    for (ProcMonsterAffix a : PROC_MONSTER_AFFIX_ALL) {
        if (!procHasAffix(mask, a)) continue;
        if (!first) out += sep;
        first = false;
        out += procMonsterAffixName(a);
    }
    return out;
}

inline constexpr int procRankTier(ProcMonsterRank r) {
    return static_cast<int>(r);
}


// -----------------------------------------------------------------------------
// Procedural monster abilities (active kits)
//
// These are *active* behaviors that can be rolled onto procedural variants
// (ranked monsters). Unlike affixes, abilities usually involve a cooldown and
// a tactical decision (area denial, reposition, summoning).
//
// Save format notes:
// - Append-only. Old saves may not have these fields; they default to None.
// -----------------------------------------------------------------------------

enum class ProcMonsterAbility : uint8_t {
    None = 0,

    // Mobility / pressure
    Pounce = 1,        // leap adjacent to the target and strike

    // Environment control
    ToxicMiasma = 2,   // seed a lingering poison gas field
    CinderNova = 3,    // seed a short-lived fire field burst

    // Defense
    ArcaneWard = 4,    // temporary shielding (damage reduction)

    // Reinforcements
    SummonMinions = 5, // call weak allies nearby

    // Debuff
    Screech = 6,       // sonic burst that confuses the player
};

inline const char* procMonsterAbilityName(ProcMonsterAbility a) {
    switch (a) {
        case ProcMonsterAbility::Pounce:        return "POUNCE";
        case ProcMonsterAbility::ToxicMiasma:   return "TOXIC MIASMA";
        case ProcMonsterAbility::CinderNova:    return "CINDER NOVA";
        case ProcMonsterAbility::ArcaneWard:    return "ARCANE WARD";
        case ProcMonsterAbility::SummonMinions: return "SUMMON";
        case ProcMonsterAbility::Screech:       return "SCREECH";
        case ProcMonsterAbility::None:
        default:                                return "NONE";
    }
}

inline constexpr ProcMonsterAbility PROC_MONSTER_ABILITY_ALL[] = {
    ProcMonsterAbility::Pounce,
    ProcMonsterAbility::ToxicMiasma,
    ProcMonsterAbility::CinderNova,
    ProcMonsterAbility::ArcaneWard,
    ProcMonsterAbility::SummonMinions,
    ProcMonsterAbility::Screech,
};

inline int procMonsterAbilityCount(ProcMonsterAbility a, ProcMonsterAbility b) {
    int n = 0;
    if (a != ProcMonsterAbility::None) ++n;
    if (b != ProcMonsterAbility::None && b != a) ++n;
    return n;
}

inline std::string procMonsterAbilityList(ProcMonsterAbility a, ProcMonsterAbility b, const char* sep = ", ") {
    std::string out;
    bool first = true;
    auto add = [&](ProcMonsterAbility x) {
        if (x == ProcMonsterAbility::None) return;
        if (!first) out += sep;
        first = false;
        out += procMonsterAbilityName(x);
    };
    add(a);
    if (b != a) add(b);
    return out;
}

// Baseline movement speed for monsters (used by the energy-based turn scheduler).
// 100 = normal speed (roughly 1 action per player turn).
// Values above 100 act more often; values below 100 act less often.
inline int baseSpeedFor(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return 100;
        case EntityKind::Goblin: return 100;
        case EntityKind::Orc: return 95;
        case EntityKind::Bat: return 150;
        case EntityKind::Slime: return 70;
        case EntityKind::SkeletonArcher: return 90;
        case EntityKind::KoboldSlinger: return 110;
        case EntityKind::Wolf: return 125;
        case EntityKind::Troll: return 85;
        case EntityKind::Wizard: return 105;
        case EntityKind::Snake: return 135;
        case EntityKind::Spider: return 110;
        case EntityKind::Ogre: return 80;
        case EntityKind::Mimic: return 60;
        case EntityKind::Shopkeeper: return 100;
        case EntityKind::Minotaur: return 105;
        case EntityKind::Dog: return 120;
        // Ghosts are dangerous because they are ethereal (can phase through terrain).
        // Keep their base speed slightly below "normal" so the fight stays fair.
        case EntityKind::Ghost: return 95;
        case EntityKind::Leprechaun: return 140;
        case EntityKind::Zombie: return 80;
        case EntityKind::Guard: return 110;
        case EntityKind::Nymph: return 130;
        default: return 100;
    }
}

// Ethereal entities can ignore terrain restrictions in pathing/movement.
// Keep this in a header so both AI and movement logic stay consistent.
inline bool entityCanPhase(EntityKind k) {
    return (k == EntityKind::Ghost);
}


// Some heavy monsters can bash through locked doors while hunting.
// Keep this in a header so AI + UI helpers stay consistent.
inline bool entityCanBashLockedDoor(EntityKind k) {
    // Keep conservative for balance: only heavy bruisers.
    return (k == EntityKind::Ogre || k == EntityKind::Troll || k == EntityKind::Minotaur);
}

inline bool entityIsUndead(EntityKind k) {
    return (k == EntityKind::Ghost || k == EntityKind::SkeletonArcher || k == EntityKind::Zombie);
}

struct MonsterBaseStats {
    int hpMax = 6;
    int baseAtk = 1;
    int baseDef = 0;

    bool willFlee = true;
    bool packAI = false;

    bool canRanged = false;
    int rangedRange = 0;
    int rangedAtk = 0;
    ProjectileKind rangedProjectile = ProjectileKind::Arrow;
    AmmoKind rangedAmmo = AmmoKind::None;

    int regenChancePct = 0;
    int regenAmount = 0;
};

// Baseline (unscaled) monster stats/flags per kind.
//
// NOTE: Implemented in content.cpp so it can be overridden by optional
// content/balance files at runtime.
MonsterBaseStats baseMonsterStatsFor(EntityKind k);

inline int monsterDepthScale(EntityKind k, int depth) {
    int d = std::max(0, depth - 1);
    if (k == EntityKind::Goblin || k == EntityKind::Bat || k == EntityKind::Slime || k == EntityKind::Snake || k == EntityKind::Leprechaun || k == EntityKind::Nymph) {
        d = d / 2;
    }
    if (k == EntityKind::Minotaur) {
        d = std::max(0, depth - 6);
    }
    return d;
}

// Stats after depth scaling. Flags (ranged, pack AI, etc.) are preserved.
inline MonsterBaseStats monsterStatsForDepth(EntityKind k, int depth) {
    MonsterBaseStats s = baseMonsterStatsFor(k);
    int d = monsterDepthScale(k, depth);
    s.hpMax += d;
    s.baseAtk += (d / 3);
    s.baseDef += (d / 4);
    return s;
}


// Monsters that can intelligently wield weapons / wear armor.
// Keep this conservative to avoid weirdness with beasts (wolves, bats, etc.).
inline bool monsterCanEquipWeapons(EntityKind k) {
    switch (k) {
        case EntityKind::Goblin:
        case EntityKind::Orc:
        case EntityKind::SkeletonArcher:
        case EntityKind::KoboldSlinger:
        case EntityKind::Wizard:
        case EntityKind::Ghost:
        case EntityKind::Guard:
            return true;
        default:
            return false;
    }
}

inline bool monsterCanEquipArmor(EntityKind k) {
    switch (k) {
        case EntityKind::Goblin:
        case EntityKind::Orc:
        case EntityKind::SkeletonArcher:
        case EntityKind::KoboldSlinger:
        case EntityKind::Wizard:
        case EntityKind::Ghost:
        case EntityKind::Guard:
            return true;
        default:
            return false;
    }
}

enum class Action : uint8_t {
    None = 0,

    // Movement
    Up,
    Down,
    Left,
    Right,
    UpLeft,
    UpRight,
    DownLeft,
    DownRight,

    Wait,

    Confirm,     // Enter / context confirm
    Cancel,      // Escape (close UI modes / cancel targeting / cancel auto-move)

    StairsUp,
    StairsDown,
    Restart,

    Pickup,
    Inventory,
    Fire,

    Equip,
    Use,
    Drop,
    DropAll,     // Drop entire stack (inventory)
    SortInventory, // Sort inventory (inventory)

    Save,
    Load,
    LoadAuto,
    Help,
    MessageHistory, // Full message history overlay
    Codex,          // Monster codex / bestiary overlay

    LogUp,
    LogDown,

    // New actions
    Search,             // Spend a turn searching for nearby traps
    Disarm,             // Attempt to disarm a discovered adjacent trap
    CloseDoor,          // Close an adjacent open door
    LockDoor,           // Lock an adjacent closed/open door (consumes a Key)
    Kick,               // Kick in a chosen direction
    ToggleAutoPickup,   // Cycle auto-pickup mode (OFF/GOLD/ALL)
    AutoExplore,        // Auto-explore (walk to nearest unexplored frontier)

    // UI / QoL
    Look,               // Examine tiles without taking a turn
    Rest,               // Rest (auto-wait) until healed or interrupted

    ToggleSneak,         // Toggle sneak mode (quiet movement / ambushes)

    ToggleMinimap,
    ToggleStats,

    // Menus / extended commands
    Options,
    Command,

    // Window / capture
    ToggleFullscreen,
    Screenshot,

    // Item discoveries / identification reference overlay
    Discoveries,

    // Append-only: keep Action ids stable for replays.
    Dig,                // Dig/tunnel (directional prompt) if wielding a pickaxe
    ToggleViewMode,      // Toggle TopDown / Isometric 2.5D view (visual-only)
    ToggleVoxelSprites,  // Toggle 2D/3D (voxel) procedural sprites (visual-only)
    Scores,             // Scores / run history overlay (Hall of Fame)

    // Minimap (append-only)
    MinimapZoomIn,
    MinimapZoomOut,

    // Spells (append-only)
    Spells,

    // Debug (append-only)
    TogglePerfOverlay,

    // UI-only (append-only)
    ToggleSoundPreview, // Toggle the acoustic/sound propagation preview overlay (Look-mode helper)
    ToggleThreatPreview, // Toggle the tactical threat/ETA heatmap overlay (Look-mode helper)

    // Tactical helpers (append-only)
    Evade, // Smart evasive step away from visible threats (uses threat+hearing fields)

    // LOOK helper (append-only)
    ToggleHearingPreview, // Toggle the hearing/audibility heatmap overlay (Look-mode helper)

    // LOOK helper (append-only)
    ToggleScentPreview, // Toggle the scent trail preview overlay (Look-mode helper)
};

// Item discoveries overlay filter/sort modes (NetHack-style "discoveries").
enum class DiscoveryFilter : uint8_t {
    All = 0,
    Potions,
    Scrolls,
    Rings,
    Wands,
};

enum class DiscoverySort : uint8_t {
    Appearance = 0,      // sort by appearance label (what the player sees while unidentified)
    IdentifiedFirst,     // group identified entries first, then sort by appearance
};

inline const char* discoveryFilterDisplayName(DiscoveryFilter f) {
    switch (f) {
        case DiscoveryFilter::All:     return "ALL";
        case DiscoveryFilter::Potions: return "POTIONS";
        case DiscoveryFilter::Scrolls: return "SCROLLS";
        case DiscoveryFilter::Rings:   return "RINGS";
        case DiscoveryFilter::Wands:   return "WANDS";
        default:                       return "ALL";
    }
}

inline const char* discoverySortDisplayName(DiscoverySort s) {
    switch (s) {
        case DiscoverySort::Appearance:      return "APPEAR";
        case DiscoverySort::IdentifiedFirst: return "KNOWN";
        default:                             return "APPEAR";
    }
}

enum class AutoPickupMode : uint8_t {
    Off  = 0,
    Gold = 1,
    All  = 2,
    Smart = 3,
};

enum class AutoMoveMode : uint8_t {
    None = 0,
    Travel,
    Explore,
};

enum class ScoresView : uint8_t {
    Top = 0,
    Recent,
};

inline const char* scoresViewDisplayName(ScoresView v) {
    switch (v) {
        case ScoresView::Top:    return "TOP";
        case ScoresView::Recent: return "RECENT";
        default:                 return "TOP";
    }
}

// Optional NetHack-style burden/encumbrance states.
// Used by the carrying capacity system (when enabled in settings).
enum class BurdenState : uint8_t {
    Unburdened = 0,
    Burdened,
    Stressed,
    Strained,
    Overloaded,
};

// Companion / ally orders (NetHack-ish pet control).
// Only meaningful when Entity::friendly is true.
enum class AllyOrder : uint8_t {
    Follow = 0,   // stay near the player (default)
    Stay,         // hold position (still fights nearby hostiles)
    Fetch,        // prioritize picking up visible gold and bringing it to you
    Guard,        // guard your current area more aggressively
};


enum class MessageKind : uint8_t {
    Info = 0,
    Combat,
    Loot,
    System,
    ImportantMsg,
    Warning,
    Success,
};

// Message log filters (used by the message history overlay).
enum class MessageFilter : uint8_t {
    All = 0,
    Important,
    Combat,
    Loot,
    System,
    Warning,
    Success,
    Info,
};

inline const char* messageFilterDisplayName(MessageFilter f) {
    switch (f) {
        case MessageFilter::All:       return "ALL";
        case MessageFilter::Important: return "IMPORTANT";
        case MessageFilter::Combat:    return "COMBAT";
        case MessageFilter::Loot:      return "LOOT";
        case MessageFilter::System:    return "SYSTEM";
        case MessageFilter::Warning:   return "WARNING";
        case MessageFilter::Success:   return "SUCCESS";
        case MessageFilter::Info:      return "INFO";
        default:                       return "ALL";
    }
}

inline bool messageFilterMatches(MessageFilter f, MessageKind k) {
    switch (f) {
        case MessageFilter::All:       return true;
        case MessageFilter::Important: return k == MessageKind::ImportantMsg;
        case MessageFilter::Combat:    return k == MessageKind::Combat;
        case MessageFilter::Loot:      return k == MessageKind::Loot;
        case MessageFilter::System:    return k == MessageKind::System;
        case MessageFilter::Warning:   return k == MessageKind::Warning;
        case MessageFilter::Success:   return k == MessageKind::Success;
        case MessageFilter::Info:      return k == MessageKind::Info;
        default:                       return true;
    }
}

// Monster codex overlay filter/sort modes.
enum class CodexFilter : uint8_t {
    All = 0,
    Seen,
    Killed,
};

enum class CodexSort : uint8_t {
    Kind = 0,      // enum order
    KillsDesc,     // most kills first
};

inline const char* codexFilterDisplayName(CodexFilter f) {
    switch (f) {
        case CodexFilter::All:    return "ALL";
        case CodexFilter::Seen:   return "SEEN";
        case CodexFilter::Killed: return "KILLED";
        default:                  return "ALL";
    }
}

inline const char* codexSortDisplayName(CodexSort s) {
    switch (s) {
        case CodexSort::Kind:      return "KIND";
        case CodexSort::KillsDesc: return "KILLS";
        default:                   return "KIND";
    }
}

// UI skin theme (purely cosmetic; persisted via settings).
enum class UITheme : uint8_t {
    DarkStone = 0,
    Parchment,
    Arcane,
};

// Input control presets.
//
// NOTE: Keybinds are still fully configurable via procrogue_settings.ini (bind_*).
// The preset is a convenience for quickly applying a cohesive scheme.
enum class ControlPreset : uint8_t {
    Modern = 0,   // WASD + Q/E/Z/C diagonals (default)
    Nethack,      // vi-keys HJKL + YUBN (NetHack-style)
};

// View / camera presentation mode.
// This is purely a renderer feature (no gameplay changes), useful for experimenting with
// an isometric 2.5D look.
enum class ViewMode : uint8_t {
    TopDown = 0,
    Isometric,
};

inline const char* viewModeId(ViewMode m) {
    switch (m) {
        case ViewMode::TopDown:   return "topdown";
        case ViewMode::Isometric: return "isometric";
        default:                  return "topdown";
    }
}

inline const char* viewModeDisplayName(ViewMode m) {
    switch (m) {
        case ViewMode::TopDown:   return "TOPDOWN";
        case ViewMode::Isometric: return "ISOMETRIC";
        default:                  return "TOPDOWN";
    }
}

inline bool parseViewMode(const std::string& raw, ViewMode& out) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isspace(uc)) s.push_back(static_cast<char>(std::tolower(uc)));
    }

    if (s == "topdown" || s == "top_down" || s == "2d" || s == "ortho" || s == "orthographic") {
        out = ViewMode::TopDown;
        return true;
    }
    if (s == "isometric" || s == "iso" || s == "2.5d" || s == "2_5d" || s == "dimetric") {
        out = ViewMode::Isometric;
        return true;
    }
    return false;
}

inline const char* controlPresetId(ControlPreset p) {
    switch (p) {
        case ControlPreset::Modern:  return "modern";
        case ControlPreset::Nethack: return "nethack";
        default:                     return "modern";
    }
}

inline const char* controlPresetDisplayName(ControlPreset p) {
    switch (p) {
        case ControlPreset::Modern:  return "MODERN";
        case ControlPreset::Nethack: return "NETHACK";
        default:                     return "MODERN";
    }
}

inline bool parseControlPreset(const std::string& raw, ControlPreset& out) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isspace(uc)) s.push_back(static_cast<char>(std::tolower(uc)));
    }

    if (s == "modern" || s == "wasd" || s == "default") {
        out = ControlPreset::Modern;
        return true;
    }
    if (s == "nethack" || s == "vi" || s == "vim" || s == "roguelike" || s == "classic") {
        out = ControlPreset::Nethack;
        return true;
    }

    return false;
}

// Player starting class/role.
// Stored in save files (per-run). The default for new runs comes from settings/CLI.
enum class PlayerClass : uint8_t {
    Adventurer = 0,
    Knight,
    Rogue,
    Archer,
    Wizard,
};

inline const char* playerClassId(PlayerClass c) {
    switch (c) {
        case PlayerClass::Adventurer: return "adventurer";
        case PlayerClass::Knight:     return "knight";
        case PlayerClass::Rogue:      return "rogue";
        case PlayerClass::Archer:     return "archer";
        case PlayerClass::Wizard:     return "wizard";
        default:                      return "adventurer";
    }
}

inline const char* playerClassDisplayName(PlayerClass c) {
    switch (c) {
        case PlayerClass::Adventurer: return "ADVENTURER";
        case PlayerClass::Knight:     return "KNIGHT";
        case PlayerClass::Rogue:      return "ROGUE";
        case PlayerClass::Archer:     return "ARCHER";
        case PlayerClass::Wizard:     return "WIZARD";
        default:                      return "ADVENTURER";
    }
}

inline PlayerClass playerClassFromU8(uint8_t v) {
    const uint8_t maxV = static_cast<uint8_t>(PlayerClass::Wizard);
    if (v <= maxV) return static_cast<PlayerClass>(v);
    return PlayerClass::Adventurer;
}

inline bool parsePlayerClass(const std::string& raw, PlayerClass& out) {
    std::string s;
    s.reserve(raw.size());
    for (unsigned char ch : raw) {
        if (std::isspace(ch) || ch == '_' || ch == '-') continue;
        s.push_back(static_cast<char>(std::tolower(ch)));
    }

    if (s.empty()) return false;

    if (s == "adventurer" || s == "advent" || s == "adv") { out = PlayerClass::Adventurer; return true; }
    if (s == "knight" || s == "warrior" || s == "fighter") { out = PlayerClass::Knight; return true; }
    if (s == "rogue" || s == "thief") { out = PlayerClass::Rogue; return true; }
    if (s == "archer" || s == "ranger" || s == "bow") { out = PlayerClass::Archer; return true; }
    if (s == "wizard" || s == "mage" || s == "wiz") { out = PlayerClass::Wizard; return true; }

    return false;
}


// Deterministic "daily" seed derived from the current UTC date.
// If outDateIso is non-null, it receives an ISO date like "2025-12-27".
uint32_t dailySeedUtc(std::string* outDateIso = nullptr);

struct Message {
    std::string text;
    MessageKind kind = MessageKind::Info;
    bool fromPlayer = true;

    // Metadata for log/history tools.
    uint32_t turn = 0;        // game turn when the message was generated
    DungeonBranch branch = DungeonBranch::Main; // dungeon branch when generated
    int depth = 0;            // dungeon depth when generated

    // Consecutive duplicate messages are compacted by incrementing this counter.
    // Example: "YOU HIT THE ORC." repeated 3 times becomes one log line with repeat=3.
    int repeat = 1;
};

enum class TrapKind : uint8_t {
    Spike = 0,
    PoisonDart,
    Teleport,
    Alarm,
    Web,
    // New traps (append-only)
    ConfusionGas,
    RollingBoulder,
    TrapDoor,
    LetheMist,

    // Persistent hazard cloud (append-only)
    PoisonGas,
};

struct Trap {
    TrapKind kind = TrapKind::Spike;
    Vec2i pos{0,0};
    bool discovered = false;
};

struct Entity {
    int id = 0;
    EntityKind kind = EntityKind::Goblin;
    Vec2i pos{0,0};

    int hp = 1;
    int hpMax = 1;

    int baseAtk = 1;
    int baseDef = 0;

    // Monster equipment (optional). The player uses inventory equip IDs instead.
    // For monsters, id==0 means the slot is empty.
    // Gear affects combat stats and is dropped on death.
    Item gearMelee; // melee weapon (optional)
    Item gearArmor; // armor (optional)

    // Monster behavior flags
    bool canRanged = false;
    int rangedRange = 0;
    int rangedAtk = 0;
    ProjectileKind rangedProjectile = ProjectileKind::Arrow;
    AmmoKind rangedAmmo = AmmoKind::None; // mostly for future use
    int rangedAmmoCount = 0; // used by ammo-based ranged monsters (0 = empty)

    bool willFlee = false;
    bool packAI = false;
    int groupId = 0;

    // Optional regen (used by Troll)
    int regenChancePct = 0; // 0 = none
    int regenAmount = 0;

    bool alerted = false;

    // Monster perception: last known player (or noise) location and how long ago it was confirmed.
    // This prevents monsters from having perfect information when they lose line of sight.
    Vec2i lastKnownPlayerPos{-1, -1};
    int lastKnownPlayerAge = 9999; // turns since last sight/hearing; large means "unknown"

    // Timed status effects (buffs/debuffs). Kept as a separate struct so
    // adding new effects is append-only for save compatibility.
    Effects effects;

    uint32_t spriteSeed = 0;

    // Procedural monster variants (rank + affixes).
    ProcMonsterRank procRank = ProcMonsterRank::Normal;
    uint32_t procAffixMask = 0u;

    // Procedural monster abilities (active kit).
    // Stored as two slots for compactness and straightforward UI display.
    ProcMonsterAbility procAbility1 = ProcMonsterAbility::None;
    ProcMonsterAbility procAbility2 = ProcMonsterAbility::None;

    // Cooldowns are in monster actions (each time the monster spends 100 energy).
    int procAbility1Cd = 0;
    int procAbility2Cd = 0;

    // Turn scheduling (energy-based).
    // Each full player turn, monsters gain `speed` energy and can act once per 100 energy.
    // Example: speed=150 -> ~1.5 actions/turn (sometimes 2).
    int speed = 100;
    int energy = 0;

    // Companion / ally state.
    // Friendly entities are allied with the player (dogs, tamed beasts, etc.).
    bool friendly = false;
    AllyOrder allyOrder = AllyOrder::Follow;

    // When an ally is ordered to STAY or GUARD, remember an anchor tile so it can
    // return after chasing enemies or being displaced.
    // NOTE: not serialized; on load, anchors are lazily initialized to the ally's
    // current position the first time the order is processed.
    Vec2i allyHomePos{-1, -1};

    // Loot carried by monsters (append-only for save compatibility).
    // Used by thieves (e.g., Leprechauns) so stolen gold can be recovered on death.
    int stolenGold = 0;

    // Pocket consumable carried by monsters (append-only for save compatibility).
    // Used by a few intelligent monsters so they can drink a potion mid-fight.
    // id==0 means the slot is empty.
    Item pocketConsumable;
};

struct FXProjectile {
    ProjectileKind kind = ProjectileKind::Arrow;
    std::vector<Vec2i> path;
    size_t pathIndex = 0;
    float stepTimer = 0.0f;
    float stepTime = 0.03f; // seconds per tile
};

// Simple AoE "flash" effect (used by Fireball explosions, etc.).
// This is purely visual; gameplay is applied instantly.
struct FXExplosion {
    std::vector<Vec2i> tiles;
    float delay = 0.0f;     // seconds before the flash starts
    float timer = 0.0f;     // seconds elapsed since start
    float duration = 0.18f; // total flash duration
};

// -----------------------------------------------------------------------------
// Lightweight visual-only particle events.
// These are consumed by the renderer's procedural particle system.
// They do NOT affect gameplay and are not serialized.
// -----------------------------------------------------------------------------

enum class FXParticlePreset : uint8_t {
    Heal = 0,
    Buff,
    Invisibility,
    Blink,
    Poison,
    Dig,
    Detect,
};

inline const char* fxParticlePresetName(FXParticlePreset p) {
    switch (p) {
        case FXParticlePreset::Heal:         return "heal";
        case FXParticlePreset::Buff:         return "buff";
        case FXParticlePreset::Invisibility: return "invisibility";
        case FXParticlePreset::Blink:        return "blink";
        case FXParticlePreset::Poison:       return "poison";
        case FXParticlePreset::Dig:          return "dig";
        case FXParticlePreset::Detect:       return "detect";
        default:                             return "unknown";
    }
}

struct FXParticleEvent {
    FXParticlePreset preset = FXParticlePreset::Heal;
    Vec2i pos{0, 0};
    int intensity = 10;     // scaling hint; interpretation depends on preset
    float delay = 0.0f;     // seconds before the event becomes active
    float timer = 0.0f;     // seconds elapsed since activation
    float duration = 0.20f; // lifetime of the event (renderer emits while active)
    uint32_t seed = 0;      // deterministic seed for procedural variation
};



// -----------------------------------------------------------------------------
// Map markers / notes
// -----------------------------------------------------------------------------

enum class MarkerKind : uint8_t {
    Note = 0,
    Danger,
    Loot,
};

inline const char* markerKindName(MarkerKind k) {
    switch (k) {
        case MarkerKind::Danger: return "DANGER";
        case MarkerKind::Loot:   return "LOOT";
        case MarkerKind::Note:
        default:
            return "NOTE";
    }
}

struct MapMarker {
    Vec2i pos{0, 0};
    MarkerKind kind = MarkerKind::Note;
    std::string label;
};

// Floor engraving / graffiti. These are small bits of text bound to a tile.
//
// Some engravings can act as "wards" (NetHack-inspired): writing the right word
// can make certain monsters hesitate to attack while you stand on it.
struct Engraving {
    Vec2i pos{0, 0};
    std::string text;

    // 255 = permanent (does not decay). Otherwise, this is a small "durability" counter
    // consumed when a ward repels monsters.
    uint8_t strength = 255;

    // True if this engraving is a recognized ward.
    bool isWard = false;

    // True if this engraving was generated by the dungeon (flavor graffiti).
    bool isGraffiti = false;
};

struct ChestContainer {
    int chestId = 0;
    std::vector<Item> items;
};

struct LevelState {
    DungeonBranch branch = DungeonBranch::Main;
    int depth = 1;
    Dungeon dung;
    std::vector<Entity> monsters;
    std::vector<GroundItem> ground;
    std::vector<Trap> traps;

    // Player annotations / map markers.
    // These are sparse and can be used for navigation (see: #mark / #travel).
    std::vector<MapMarker> markers;

    // Floor engravings / graffiti (sparse, per-level, persisted in saves).
    std::vector<Engraving> engravings;

    // Container contents keyed by chest item id.
    // This enables NetHack-like persistent storage (stash) in opened chests.
    std::vector<ChestContainer> chestContainers;


    // Environmental fields (per-tile intensities).
    // Currently used for persistent Confusion Gas clouds.
    std::vector<uint8_t> confusionGas;

    // Persistent toxic vapor / poison gas field.
    std::vector<uint8_t> poisonGas;


    // Persistent flames / embers left behind by explosions and other fire sources.
    std::vector<uint8_t> fireField;

    // Persistent scent trail used by smell-capable monsters to track the player.
    // Stored as a per-tile intensity map (0..255).
    std::vector<uint8_t> scentField;
};

class Game {
public:
    // The game renders the whole dungeon at once (no camera/scrolling).
    // Keep these in sync with Dungeon's default generation dimensions.
    //
    // Round 19: bumped the map up again by +50% area (84x55 -> 105x66).
    // NOTE: The window size is MAP_W * tile_size by default.
    // If the window is too large on your display, lower tile_size (settings.ini)
    // or enable fullscreen.
    static constexpr int MAP_W = Dungeon::DEFAULT_W;
    static constexpr int MAP_H = Dungeon::DEFAULT_H;

    // Run structure / quest pacing.
    // The default run now spans 25 floors.
    static constexpr int DUNGEON_MAX_DEPTH = 25;
    static constexpr int QUEST_DEPTH = DUNGEON_MAX_DEPTH; // where the Amulet of Yendor is guaranteed
    static constexpr int MIDPOINT_DEPTH = DUNGEON_MAX_DEPTH / 2;

    Game();

    void newGame(uint32_t seed);

    // Procedural map size selection used during level generation.
    // Returns {w,h}. Consumes RNG in the same way as real generation,
    // so callers should pass a copy if they want a "peek" without perturbing fate.
    Vec2i proceduralMapSizeFor(RNG& rngRef, DungeonBranch branch, int depth) const;

    void handleAction(Action a);

    // Spend a turn to emit a loud noise (useful to lure monsters).
    // Available via extended command: #shout
    void shout();
    void whistle();

    // Spend a turn to listen carefully for unseen movement.
    // Extended command: #listen
    void listen();

    // Throw your voice to a distant tile (emits loud noise at the target).
    // Extended command: #throwvoice [X Y]
    // TIP: open LOOK (:) and move the cursor, then use #throwvoice with no coordinates.
    // Returns true if a turn was spent.
    bool throwVoiceAt(Vec2i target);


    // Attempt to tame an adjacent beast using a food ration (costs a turn).
    // Extended command: #tame
    void tame();

    // Set orders for all friendly companions.
    // Extended command: #pet <follow|stay|fetch|guard>
    void setAlliesOrder(AllyOrder order, bool verbose = true);


    // Dig/tunnel using a pickaxe (adjacent tile). Used by extended command: #dig <dir>
    // Returns true if a turn was spent.
    bool digInDirection(int dx, int dy);

    // Throw a lit torch in a direction. Useful for lighting up distant areas or igniting foes.
    // Extended command: #throwtorch <dir>
    // Returns true if a turn was spent.
    bool throwTorchInDirection(int dx, int dy);

    // QoL: repeat the Search action multiple times without spamming the log.
    // Used by the extended command: #search N
    // Returns how many turns were actually spent searching.
    int repeatSearch(int maxTurns, bool stopOnFind = true);

    // Floor engravings / graffiti.
    // Extended command: #engrave <text>
    // Returns true if a turn was spent.
    bool engraveHere(const std::string& text);


    // Shrine interaction (use via #pray or future keybind).
    // mode: \"heal\" | \"cure\" | \"identify\" | \"bless\" | \"\" (auto)
    bool prayAtShrine(const std::string& mode = std::string());

    // Donate gold to earn piety at a shrine (or at the surface camp).
    // Extended command: #donate [amount]
    // Returns true if a turn was spent.
    bool donateAtShrine(int goldAmount);

    // Sacrifice a corpse to earn piety at a shrine (or at the surface camp).
    // Extended command: #sacrifice
    // Returns true if a turn was spent.
    bool sacrificeAtShrine();

    // Augury/divination: pay gold at a shrine or the surface camp to receive
    // cryptic hints about the *next* floor layout.
    // Extended command: #augury
    // Returns true if a turn was spent.
    bool augury();
    void update(float dt);

    const Dungeon& dungeon() const { return dung; }
    const std::vector<Entity>& entities() const { return ents; }
    const std::vector<GroundItem>& groundItems() const { return ground; }
    const std::vector<Trap>& traps() const { return trapsCur; }

    // Map markers / notes (sparse, per-level, persisted in saves).
    const std::vector<MapMarker>& mapMarkers() const { return mapMarkers_; }
    const MapMarker* markerAt(Vec2i p) const;
    bool setMarker(Vec2i p, MarkerKind kind, const std::string& label, bool verbose = true);
    bool clearMarker(Vec2i p, bool verbose = true);
    void clearAllMarkers(bool verbose = true);

    // Floor engravings / graffiti (sparse, per-level, persisted in saves).
    // Note: visibility rules are simple: engravings exist on the tile whether or not you can
    // currently see them; UI layers decide what to reveal.
    const std::vector<Engraving>& engravings() const { return engravings_; }
    const Engraving* engravingAt(Vec2i p) const;


    // Persistent environmental gas on the current level (0..255 intensity).
    // 0 means no gas. Only meaningful on in-bounds tiles.
    uint8_t confusionGasAt(int x, int y) const;

    // Persistent poison gas on the current level (0..255 intensity).
    // 0 means no gas. Only meaningful on in-bounds tiles.
    uint8_t poisonGasAt(int x, int y) const;


    // Persistent fire field on the current level (0..255 intensity).
    // 0 means no fire. Only meaningful on in-bounds tiles.
    uint8_t fireAt(int x, int y) const;

    // Persistent scent trail intensity on the current level (0..255).
    // 0 means no scent. Only meaningful on in-bounds tiles.
    uint8_t scentAt(int x, int y) const;

    // Procedural per-level wind (deterministic from run seed + level id).
    // Used to bias gas and fire drift/spread. Returns a cardinal direction vector
    // (dx,dy) or {0,0} for calm.
    Vec2i windDir() const;
    // 0 = calm, 1..3 = increasing strength.
    int windStrength() const;

    const Entity& player() const;
    Entity& playerMut();

    int playerId() const { return playerId_; }

    int depth() const { return depth_; }

    DungeonBranch branch() const { return branch_; }

    bool atCamp() const { return branch_ == DungeonBranch::Camp && depth_ == 0; }

    // Infinite overworld (Camp depth 0): the surface camp hub lives at chunk (0,0).
    // Traveling across map edges moves between adjacent chunks without changing branch/depth.
    bool atHomeCamp() const { return atCamp() && overworldX_ == 0 && overworldY_ == 0; }
    int overworldX() const { return overworldX_; }
    int overworldY() const { return overworldY_; }

    int dungeonMaxDepth() const { return DUNGEON_MAX_DEPTH; }
    int questDepth() const { return QUEST_DEPTH; }

    bool isGameOver() const { return gameOver; }
    bool isGameWon() const { return gameWon; }
    bool isFinished() const { return gameOver || gameWon; }

    // Run meta
    uint32_t seed() const { return seed_; }
    uint32_t kills() const { return killCount; }
    int maxDepthReached() const { return maxDepth; }

    // Conducts (NetHack-style voluntary challenges).
    // These are tracked per-run and persisted in saves for scoreboard/history.
    uint32_t directKills() const { return directKillCount_; }
    uint32_t foodsEaten() const { return conductFoodEaten_; }
    uint32_t corpsesEaten() const { return conductCorpseEaten_; }
    uint32_t scrollsRead() const { return conductScrollsRead_; }
    uint32_t spellbooksRead() const { return conductSpellbooksRead_; }
    uint32_t prayersUsed() const { return conductPrayers_; }

    // Returns a '|' separated list of currently-kept conducts (e.g., "FOODLESS | PACIFIST"),
    // or an empty string if none are currently kept.
    std::string runConductsTag() const;

    // Player identity (used for HUD + scoreboard)
    const std::string& playerName() const { return playerName_; }
    void setPlayerName(std::string name);

    // Player starting class/role (saved per-run).
    //
    // The default for new runs can be set via settings.ini (player_class) or the CLI (--class).
    PlayerClass playerClass() const { return playerClass_; }
    const char* playerClassIdString() const { return ::playerClassId(playerClass_); }
    const char* playerClassDisplayName() const { return ::playerClassDisplayName(playerClass_); }
    void setPlayerClass(PlayerClass pc) { playerClass_ = pc; }

    // End-of-run cause (e.g., "KILLED BY GOBLIN", "ESCAPED WITH THE AMULET")
    const std::string& endCause() const { return endCause_; }

    // HUD preferences
    bool showEffectTimers() const { return showEffectTimers_; }
    void setShowEffectTimers(bool enabled) { showEffectTimers_ = enabled; }

    bool perfOverlayEnabled() const { return perfOverlayEnabled_; }
    void setPerfOverlayEnabled(bool enabled) { perfOverlayEnabled_ = enabled; }

// UI skin (purely cosmetic)
UITheme uiTheme() const { return uiTheme_; }
void setUITheme(UITheme theme) { uiTheme_ = theme; }

bool uiPanelsTextured() const { return uiPanelsTextured_; }
void setUIPanelsTextured(bool textured) { uiPanelsTextured_ = textured; }

// View mode (renderer-only camera presentation).
ViewMode viewMode() const { return viewMode_; }
const char* viewModeIdString() const { return ::viewModeId(viewMode_); }
const char* viewModeDisplayName() const { return ::viewModeDisplayName(viewMode_); }
void setViewMode(ViewMode mode) { viewMode_ = mode; }
bool isIsometricView() const { return viewMode_ == ViewMode::Isometric; }

// Control preset (for help text + optional keybind presets).
ControlPreset controlPreset() const { return controlPreset_; }
const char* controlPresetIdString() const { return ::controlPresetId(controlPreset_); }
const char* controlPresetDisplayName() const { return ::controlPresetDisplayName(controlPreset_); }
void setControlPreset(ControlPreset preset) { controlPreset_ = preset; }


    // Sprite style
    // When enabled, entity/item/projectile sprites are rendered by voxelizing the 2D procedural sprite
    // and re-rendering it with simple 3D lighting (still within a 2D tile).
    bool voxelSpritesEnabled() const { return voxelSpritesEnabled_; }
    void setVoxelSpritesEnabled(bool enabled) { voxelSpritesEnabled_ = enabled; }

    // Isometric-only: when enabled, isometric voxel sprites are rendered using the voxel raytracer
    // (orthographic DDA traversal) instead of the mesh-projected isometric rasterizer.
    // This is UI-only and only affects sprite generation/caching.
    bool isoVoxelRaytraceEnabled() const { return isoVoxelRaytraceEnabled_; }
    void setIsoVoxelRaytraceEnabled(bool enabled) { isoVoxelRaytraceEnabled_ = enabled; }
    bool isoTerrainVoxelBlocksEnabled() const { return isoTerrainVoxelBlocksEnabled_; }
    void setIsoTerrainVoxelBlocksEnabled(bool enabled) { isoTerrainVoxelBlocksEnabled_ = enabled; }

    // Isometric-only: fade foreground occluders near the player/cursor for readability.
    bool isoCutawayEnabled() const { return isoCutawayEnabled_; }
    void setIsoCutawayEnabled(bool enabled) { isoCutawayEnabled_ = enabled; }

    // Procedural terrain palette (cosmetic): seed-derived per-floor tints that
    // nudge terrain towards a coherent palette for this run/floor.
    bool procPaletteEnabled() const { return procPaletteEnabled_; }
    int procPaletteStrength() const { return procPaletteStrength_; }
    void setProcPaletteEnabled(bool enabled) { procPaletteEnabled_ = enabled; }
    void setProcPaletteStrength(int pct) { procPaletteStrength_ = std::clamp(pct, 0, 100); }



    // Inventory/UI accessors for renderer
    const std::vector<Item>& inventory() const { return inv; }
    bool isInventoryOpen() const { return invOpen; }
    // True when the inventory overlay is being used for a special prompt (e.g. choosing an item for Scroll of Identify).
    bool isInventoryIdentifyMode() const { return invIdentifyMode; }
    bool isInventoryEnchantRingMode() const { return invEnchantRingMode; }
    int inventorySelection() const { return invSel; }
    bool isEquipped(int itemId) const;
    std::string equippedTag(int itemId) const; // e.g. "M", "R", "A", "1", "2"

    // Equipped item accessors (UI/debug). These return pointers into the inventory.
    // Null means the slot is empty.
    const Item* equippedMelee() const;
    const Item* equippedRanged() const;
    const Item* equippedArmor() const;
    const Item* equippedRing1() const;
    const Item* equippedRing2() const;

    std::string equippedMeleeName() const;
    std::string equippedRangedName() const;
    std::string equippedArmorName() const;
    std::string equippedRing1Name() const;
    std::string equippedRing2Name() const;
    int playerAttack() const;
    int playerDefense() const;


    // Chest container (loot / stash) overlay accessors for renderer
    bool isChestOpen() const { return chestOpen; }
    int chestSelection() const { return chestSel; }
    bool chestPaneIsChest() const { return chestPaneChest; }
    int chestOpenChestId() const { return chestOpenId; }
    int chestOpenTier() const { return chestOpenTier_; }
    int chestOpenStackLimit() const { return chestOpenMaxStacks_; }
    const std::vector<Item>& chestOpenItems() const;

    // Spells overlay (renderer)
    bool isSpellsOpen() const { return spellsOpen; }
    int spellsSelection() const { return spellsSel; }

    // Talents (earned on level-up). These provide build variety while keeping the
    // classic ATK/DEF progression intact.
    int playerMight() const { return talentMight_ + ringTalentBonusMight(); }      // melee power / carry capacity
    int playerAgility() const { return talentAgility_ + ringTalentBonusAgility(); }  // ranged accuracy / evasion / locks & traps
    int playerVigor() const { return talentVigor_ + ringTalentBonusVigor(); }      // max HP growth
    int playerFocus() const { return talentFocus_ + ringTalentBonusFocus(); }      // wand power / searching
    int pendingTalentPoints() const { return talentPointsPending_; }

    // Derived core stats (used by combat rules / UI).
    int playerMeleePower() const { return player().baseAtk + playerMight(); }
    int playerRangedSkill() const { return player().baseAtk + playerAgility(); }
    int playerWandPower() const { return playerFocus(); }
    int playerEvasion() const { return player().baseDef + playerAgility(); }

    // Level-up allocation overlay
    bool isLevelUpOpen() const { return levelUpOpen; }
    int levelUpSelection() const { return levelUpSel; }


    // Progression
    int playerCharLevel() const { return charLevel; }
    int playerXp() const { return xp; }
    int playerXpToNext() const { return xpNext; }

    // Convenience getters (kept for renderer/legacy naming)
    int goldCount() const { return countGold(inv); }
    // Shrines / religion
    int piety() const { return piety_; }
    // Remaining turns until the next shrine prayer service is allowed (0 = ready).
    int prayerCooldownTurns() const {
        if (turnCount >= prayerCooldownUntilTurn_) return 0;
        return static_cast<int>(prayerCooldownUntilTurn_ - turnCount);
    }

    // Spells / mana (work-in-progress)
    int playerMana() const { return mana_; }
    int playerManaMax() const;
    bool knowsSpell(SpellKind k) const;
    std::vector<SpellKind> knownSpellsList() const;
    SpellKind selectedSpell() const;
    // Shops / economy
    // Total gold owed for UNPAID items currently in inventory.
    int shopDebtTotal() const;
    // Gold owed to the shop on the current depth.
    int shopDebtThisDepth() const;
    // True if the player is standing inside a shop room.
    bool playerInShop() const;
    // Pay for all (or as many as possible) unpaid goods on this depth. Use via #pay.
    bool payAtShop();
    // Pay debts to the Merchant Guild while at the surface camp (depth 0).
    // This is a QoL escape hatch so a run isn't soft-locked by debt on a dead shop floor.
    bool payAtCamp();

    // Print a per-depth breakdown of outstanding shop debts (unpaid items + consumed/destroyed ledger).
    // Extended command: #debt
    void showDebtLedger();

    int keyCount() const;
    int lockpickCount() const;
    int characterLevel() const { return charLevel; }
    int experience() const { return xp; }
    int experienceToNext() const { return xpNext; }

    // XP reward for killing a given monster kind (used by UI such as the Codex).
    int xpFor(EntityKind k) const;

    // XP reward for killing a specific entity instance (procedural variants scale this up).
    int xpFor(const Entity& e) const;
    AutoPickupMode autoPickupMode() const { return autoPickup; }
    bool autoPickupEnabled() const { return autoPickup != AutoPickupMode::Off; }
    void setAutoPickupMode(AutoPickupMode m);

    bool playerHasAmulet() const;

    // Item display helpers (respect identification settings + run knowledge).
    std::string displayItemName(const Item& it) const;
    std::string displayItemNameSingle(ItemKind k) const;

    void setIdentificationEnabled(bool enabled);
    bool identificationEnabled() const { return identifyItemsEnabled; }

    // Per-run randomized appearance id for an item kind (used by renderer for NetHack-style identification visuals).
    uint8_t itemAppearanceFor(ItemKind k) const { return appearanceFor(k); }

// NetHack-style "call" labels for unidentified appearances (per-run, saved).
// This lets players attach a short note to a randomized appearance (e.g. "RUBY POTION" -> "HEAL?").
// Notes are shown as a {LABEL} suffix for unidentified items (inventory / LOOK) and in the Discoveries overlay.
bool hasItemCallLabel(ItemKind k) const;
std::string itemCallLabel(ItemKind k) const;
// Sets (or replaces) the call label. Returns true if the label changed.
bool setItemCallLabel(ItemKind k, const std::string& label);
// Clears any existing call label. Returns true if something was cleared.
bool clearItemCallLabel(ItemKind k);

    // Optional hunger system (survival / NetHack-like pacing).
    void setHungerEnabled(bool enabled);
    bool hungerEnabled() const { return hungerEnabled_; }
    int hungerCurrent() const { return hunger; }
    int hungerMaximum() const { return hungerMax; }
    // Returns an empty string when not hungry; otherwise "HUNGRY"/"STARVING".
    std::string hungerTag() const;

    // Optional encumbrance system (carrying capacity + movement penalties).
    void setEncumbranceEnabled(bool enabled);
    bool encumbranceEnabled() const { return encumbranceEnabled_; }
    // Total weight of inventory (stack-aware).
    int inventoryWeight() const;
    // Carry capacity derived from progression stats.
    int carryCapacity() const;
    BurdenState burdenState() const;
    // A short HUD-friendly label for current burden state ("BURDENED", "STRESSED", ...).
    std::string burdenTag() const;

    // Sneak / stealth mode (reduces movement noise).
    void setSneakMode(bool enabled, bool quiet = false);
    void toggleSneakMode(bool quiet = false);
    bool isSneaking() const { return sneakMode_; }
    std::string sneakTag() const;

    // Lighting / darkness system (optional). When enabled, deeper levels can contain dark tiles
    // that require light sources (torches, lit rooms) to see beyond a short range.
    void setLightingEnabled(bool enabled);
    bool lightingEnabled() const { return lightingEnabled_; }
    // True when darkness rules are active on the current depth.
    bool darknessActive() const;
    // Per-tile light level (0..255) for the current level.
    uint8_t tileLightLevel(int x, int y) const;
    // Per-tile light color (RGB 0..255) for the current level.
    Color tileLightColor(int x, int y) const;
    // A short HUD-friendly label for lighting state ("DARK", "TORCH(123)", ...).
    std::string lightTag() const;

    // ------------------------------------------------------------
    // Yendor Doom (endgame escalation)
    //
    // Optional pressure system: once the player acquires the Amulet of Yendor,
    // the dungeon starts fighting back with periodic "doom" noises and hunter
    // spawns. This can be disabled for a more relaxed endgame.
    // ------------------------------------------------------------
    void setYendorDoomEnabled(bool enabled);
    bool yendorDoomEnabled() const { return yendorDoomEnabled_; }
    bool yendorDoomActive() const { return yendorDoomActive_; }
    int yendorDoomLevel() const { return yendorDoomLevel_; }

    // Quit confirmation (ESC requires a second press within a short window).
    void setConfirmQuitEnabled(bool enabled) { confirmQuitEnabled_ = enabled; }
    bool confirmQuitEnabled() const { return confirmQuitEnabled_; }

    // Automatic mortem dump (full-state dump written on win/death).
    void setAutoMortemEnabled(bool enabled) { autoMortemEnabled_ = enabled; }
    bool autoMortemEnabled() const { return autoMortemEnabled_; }

    // Bones files (persistent death remnants between runs).
    void setBonesEnabled(bool enabled) { bonesEnabled_ = enabled; }
    bool bonesEnabled() const { return bonesEnabled_; }

    // Endless / infinite world (experimental): allow descending beyond the normal bottom.
    void setInfiniteWorldEnabled(bool enabled) { infiniteWorldEnabled_ = enabled; }
    bool infiniteWorldEnabled() const { return infiniteWorldEnabled_; }

    // Infinite world memory cap: keep only a sliding window of post-quest levels cached.
    // 0 disables pruning.
    void setInfiniteKeepWindow(int n) { infiniteKeepWindow_ = std::clamp(n, 0, 200); }
    int infiniteKeepWindow() const { return infiniteKeepWindow_; }


    // Targeting
    bool isTargeting() const { return targeting; }
    Vec2i targetingCursor() const { return targetPos; }
    const std::vector<Vec2i>& targetingLine() const { return targetLine; }
    bool targetingIsValid() const { return targetValid; }

    // True when the current target is considered risky (friendly fire / self-damage) and
    // the game will require an extra confirmation press before executing the action.
    bool targetingNeedsConfirm() const { return targeting && targetUnsafe_ && !targetUnsafeConfirmed_; }
    // HUD-friendly info string describing the current targeting cursor.
    // Used by the targeting overlay.
    std::string targetingInfoText() const;

    // Optional extra info for the targeting HUD (hit chance / damage preview).
    // Returns an empty string when no preview is available.
    std::string targetingCombatPreviewText() const;

    // When targetingIsValid() is false, this provides a short explanation suitable for the HUD
    // (e.g. "OUT OF RANGE", "NO CLEAR SHOT"). Empty when valid.
    std::string targetingStatusText() const;

    // Optional warning/hint shown during targeting even when the shot is otherwise valid
    // (e.g., friendly-fire risk for ranged attacks / AoE spells).
    // Empty when there is no warning.
    std::string targetingWarningText() const;

    // Kick prompt (directional)
    bool isKicking() const { return kicking; }

    // Dig prompt (directional)
    bool isDigging() const { return digging; }

    // Help overlay
    bool isHelpOpen() const { return helpOpen; }
    int helpScrollLines() const { return helpScroll_; }


    // Look/examine overlay (cursor-based inspection)
    bool isLooking() const { return looking; }
    Vec2i lookCursor() const { return lookPos; }
    std::string lookInfoText() const;

    // Acoustic preview overlay (LOOK helper; UI-only)
    bool isSoundPreviewOpen() const { return soundPreviewOpen; }
    int soundPreviewVolume() const { return soundPreviewVol; }
    Vec2i soundPreviewSource() const { return soundPreviewSrc; }
    const std::vector<int>& soundPreviewMap() const { return soundPreviewDist; }
    void toggleSoundPreview();
    void adjustSoundPreviewVolume(int delta);

    // Threat preview overlay (LOOK helper; UI-only)
    bool isThreatPreviewOpen() const { return threatPreviewOpen; }
    int threatPreviewHorizon() const { return threatPreviewMaxCost; }
    const std::vector<Vec2i>& threatPreviewSources() const { return threatPreviewSrcs; }
    const std::vector<int>& threatPreviewMap() const { return threatPreviewDist; }
    void toggleThreatPreview();
    void adjustThreatPreviewHorizon(int delta);

    // Hearing preview overlay (LOOK helper; UI-only): visualize where your *footsteps*
    // would be audible to any currently visible hostile (uses the same hearing stats
    // as real noise emission; never leaks hidden monsters).
    bool isHearingPreviewOpen() const { return hearingPreviewOpen; }
    int hearingPreviewVolumeBias() const { return hearingPreviewVolBias; }
	    const std::vector<Vec2i>& hearingPreviewListeners() const { return hearingPreviewListeners_; }
	    int hearingPreviewDominantListenerIndexAt(Vec2i p) const {
	        if (!dung.inBounds(p.x, p.y)) return -1;
	        if (dung.width <= 0 || dung.height <= 0) return -1;
	        const size_t i = static_cast<size_t>(p.y * dung.width + p.x);
	        if (i >= hearingPreviewDominantListenerIndex_.size()) return -1;
	        return hearingPreviewDominantListenerIndex_[i];
	    }
    const std::vector<int>& hearingPreviewMinRequiredVolume() const { return hearingPreviewMinReq; }
    const std::vector<int>& hearingPreviewFootstepVolume() const { return hearingPreviewFootstepVol; }
    void toggleHearingPreview();
    void adjustHearingPreviewVolume(int delta);

    // Scent preview overlay (LOOK helper; UI-only): visualize your lingering scent trail
    // (useful for planning around smell-tracking monsters and sneak mode).
    bool isScentPreviewOpen() const { return scentPreviewOpen; }
    int scentPreviewCutoff() const { return scentPreviewCutoff_; }
    void toggleScentPreview();
    void adjustScentPreviewCutoff(int delta);

    // Minimap / stats overlays
    bool isMinimapOpen() const { return minimapOpen; }
    bool isStatsOpen() const { return statsOpen; }

    // Minimap cursor (UI-only): used for minimap keyboard/mouse navigation.
    bool minimapCursorActive() const { return minimapCursorActive_; }
    Vec2i minimapCursor() const { return minimapCursorPos_; }
    void setMinimapCursor(Vec2i p) {
        p.x = std::clamp(p.x, 0, std::max(0, dung.width - 1));
        p.y = std::clamp(p.y, 0, std::max(0, dung.height - 1));
        minimapCursorPos_ = p;
        minimapCursorActive_ = true;
    }
    void clearMinimapCursor() { minimapCursorActive_ = false; }


    // Minimap zoom (UI-only; persisted in settings).
    int minimapZoom() const { return minimapZoom_; }
    void setMinimapZoom(int z) { minimapZoom_ = std::clamp(z, -3, 3); }

    // Options overlay
    bool isOptionsOpen() const { return optionsOpen; }
    int optionsSelection() const { return optionsSel; }

    // Keybinds overlay (interactive editor; does not consume turns)
    bool isKeybindsOpen() const { return keybindsOpen; }
    int keybindsSelection() const { return keybindsSel; }
    int keybindsScroll() const { return keybindsScroll_; }
    bool isKeybindsCapturing() const { return keybindsCapture; }
    bool keybindsCaptureAddMode() const { return keybindsCaptureAdd; }
    int keybindsCaptureActionIndex() const { return keybindsCaptureIndex; }

    // Keybinds overlay search/filter (UI-only; does not consume turns)
    bool isKeybindsSearchMode() const { return keybindsSearchMode_; }
    const std::string& keybindsSearchQuery() const { return keybindsSearch_; }
    void keybindsToggleSearchMode();
    void keybindsTextInput(const char* utf8);
    void keybindsBackspace();
    void keybindsClearSearch();

    // Unbind (disable) the currently selected action in the keybinds overlay.
    // Writes `bind_<action> = none` to settings.
    void keybindsUnbindSelected();

    // Build the currently visible keybind row indices (after applying any filter).
    // Indices refer to keybindsDesc_.
    void keybindsBuildVisibleIndices(std::vector<int>& out) const;

    // Cached bindings for the keybinds UI (action name -> chord list).
    // Filled/updated by the platform layer (main.cpp) after loading keybinds from disk.
    const std::vector<std::pair<std::string, std::string>>& keybindsDescription() const { return keybindsDesc_; }
    void setKeybindsDescription(std::vector<std::pair<std::string, std::string>> desc) { keybindsDesc_ = std::move(desc); }

    // Raw keybind editor capture (called by main.cpp while capturing).
    void keybindsCaptureToken(const std::string& chordToken);
    void keybindsCancelCapture();


    // NetHack-like extended command prompt
    bool isCommandOpen() const { return commandOpen; }
    const std::string& commandBuffer() const { return commandBuf; }
    // Byte index into commandBuffer() (UTF-8 safe; always kept on a codepoint boundary).
    int commandCursorByte() const { return commandCursor_; }
    void commandTextInput(const char* utf8);
    void commandBackspace();
    void commandAutocomplete();

    // Cursor navigation for the extended command prompt (UI-only; does not consume turns).
    void commandCursorLeft();
    void commandCursorRight();
    void commandCursorHome();
    void commandCursorEnd();

    // Tab completion UI state (UI-only; does not consume turns).
    const std::vector<std::string>& commandAutocompleteMatches() const { return commandAutoMatches; }
    const std::vector<std::string>& commandAutocompleteHints() const { return commandAutoHints; }
    const std::vector<std::string>& commandAutocompleteDescs() const { return commandAutoDescs; }
    int commandAutocompleteIndex() const { return commandAutoIndex; }
    const std::string& commandAutocompleteBase() const { return commandAutoBase; }
    bool commandAutocompleteFuzzy() const { return commandAutoFuzzy; }

    // Settings path (for UI hints) + persistence flag for options
    void setSettingsPath(const std::string& path);
    const std::string& settingsPath() const { return settingsPath_; }
    bool settingsDirty() const { return settingsDirtyFlag; }
    void markSettingsDirty() { settingsDirtyFlag = true; }
    void clearSettingsDirty() { settingsDirtyFlag = false; }

    // Separate flag so we don't accidentally persist --slot when the user only changed other options.
    bool slotDirty() const { return slotDirtyFlag; }
    void markSlotDirty() { slotDirtyFlag = true; settingsDirtyFlag = true; }
    void clearSlotDirty() { slotDirtyFlag = false; }

    // Auto-step delay getter (milliseconds)
    int autoStepDelayMs() const;

    // Quit requests (from extended commands)
    bool quitRequested() const { return quitReq; }
    void requestQuit() { quitReq = true; }
    void clearQuitRequest() { quitReq = false; }

    // Requests to the platform layer (handled in main.cpp)
    bool keyBindsReloadRequested() const { return keyBindsReloadReq; }
    void requestKeyBindsReload() { keyBindsReloadReq = true; }
    void clearKeyBindsReloadRequest() { keyBindsReloadReq = false; }

    bool keyBindsDumpRequested() const { return keyBindsDumpReq; }
    void requestKeyBindsDump() { keyBindsDumpReq = true; }
    void clearKeyBindsDumpRequest() { keyBindsDumpReq = false; }

    bool configReloadRequested() const { return configReloadReq; }
    void requestConfigReload() { configReloadReq = true; }
    void clearConfigReloadRequest() { configReloadReq = false; }

    // Turn counter (increments once per player action that consumes time)
    uint32_t turns() const { return turnCount; }

    // Determinism / replay helpers.
    // Returns a stable 64-bit hash of simulation-relevant state.
    //
    // Intentionally excludes UI-only state, message logs, transient FX animation state,
    // and real-time timers so it can be used to validate deterministic simulation
    // across machines/frame rates.
    uint64_t determinismHash() const;

    // Optional callback invoked once per completed player turn (after the turn counter
    // increments and end-of-turn logic like monster turns/FOV/effects have been applied).
    //
    // Used by the replay recorder/verifier to attach per-turn state hashes without
    // coupling the core game logic to replay I/O.
    using TurnHookFn = void(*)(void* user, uint32_t turn, uint64_t stateHash);
    void setTurnHook(TurnHookFn fn, void* user) { turnHookFn_ = fn; turnHookUser_ = user; }
    void clearTurnHook() { turnHookFn_ = nullptr; turnHookUser_ = nullptr; }

    // Messages + scrollback
    const std::vector<Message>& messages() const { return msgs; }
    int messageScroll() const { return msgScroll; }

    // Message history overlay (full log viewer; does not consume turns)
    bool isMessageHistoryOpen() const { return msgHistoryOpen; }
    bool isMessageHistorySearchMode() const { return msgHistorySearchMode; }
    MessageFilter messageHistoryFilter() const { return msgHistoryFilter; }
    const std::string& messageHistorySearch() const { return msgHistorySearch; }
    int messageHistoryScroll() const { return msgHistoryScroll; }

    // Build a plaintext dump of the currently filtered message history (respects
    // filter + search). Intended for clipboard / bug reports.
    std::string messageHistoryClipboardText() const;

    // Text input helpers for the message history overlay (search mode).
    void messageHistoryTextInput(const char* utf8);
    void messageHistoryBackspace();
    void messageHistoryToggleSearchMode();
    void messageHistoryClearSearch();
    void messageHistoryCycleFilter(int dir);

    // Monster codex (bestiary / encounter log)
    bool isCodexOpen() const { return codexOpen; }
    CodexFilter codexFilter() const { return codexFilter_; }
    CodexSort codexSort() const { return codexSort_; }
    int codexSelection() const { return codexSel; }

    // Item discoveries overlay (identification reference)
    bool isDiscoveriesOpen() const { return discoveriesOpen; }
    DiscoveryFilter discoveriesFilter() const { return discoveriesFilter_; }
    DiscoverySort discoveriesSort() const { return discoveriesSort_; }
    int discoveriesSelection() const { return discoveriesSel; }

    // Scores / run history overlay (Hall of Fame)
    bool isScoresOpen() const { return scoresOpen; }
    ScoresView scoresView() const { return scoresView_; }
    int scoresSelection() const { return scoresSel; }
    void buildScoresList(std::vector<size_t>& out) const;

    // UI helper: whether a given identifiable item kind is currently identified.
    // Non-identifiable kinds are treated as identified.
    bool discoveriesIsIdentified(ItemKind k) const { return isIdentified(k); }

    // Returns the "appearance label" used when the item is not identified.
    // Examples: "RUBY POTION", "SCROLL 'ZELGO'", "OPAL RING", "OAK WAND".
    std::string discoveryAppearanceLabel(ItemKind k) const;

    void buildDiscoveryList(std::vector<ItemKind>& out) const;
    void buildDiscoveryList(std::vector<ItemKind>& out, DiscoveryFilter filter, DiscoverySort sort) const;

    bool codexHasSeen(EntityKind k) const {
        const size_t idx = static_cast<size_t>(k);
        return idx < codexSeen_.size() && codexSeen_[idx] != 0;
    }
    uint16_t codexKills(EntityKind k) const {
        const size_t idx = static_cast<size_t>(k);
        return idx < codexKills_.size() ? codexKills_[idx] : 0;
    }

    void buildCodexList(std::vector<EntityKind>& out) const;

    // FX
    const std::vector<FXProjectile>& fxProjectiles() const { return fx; }
    const std::vector<FXExplosion>& fxExplosions() const { return fxExpl; }
    const std::vector<FXParticleEvent>& fxParticles() const { return fxParticles_; }
    bool inputLocked() const { return inputLock; }

    // Queue a visual-only particle event for the renderer.
    void pushFxParticle(FXParticlePreset preset, Vec2i pos, int intensity = 10, float duration = 0.20f, float delay = 0.0f, uint32_t seed = 0);
    // Save/load helpers
    std::string defaultSavePath() const;
    void setSavePath(const std::string& path);

    // Active save slot name (empty = "default").
    // This changes what defaultSavePath/defaultAutosavePath point to.
    const std::string& activeSlot() const { return activeSlot_; }
    void setActiveSlot(std::string slot);

    // Save backup rotation (applies to manual saves and autosaves)
    // 0 disables backups; N keeps <file>.bak1..bakN.
    void setSaveBackups(int count);
    int saveBackups() const { return saveBackups_; }

    // Autosave
    void setAutosavePath(const std::string& path);
    std::string defaultAutosavePath() const;
    void setAutosaveEveryTurns(int turns);
    int autosaveEveryTurns() const { return autosaveInterval; }
    uint32_t lastAutosaveAtTurn() const { return lastAutosaveTurn; }

    // Scores
    void setScoresPath(const std::string& path);
    std::string defaultScoresPath() const;
    const ScoreBoard& scoreBoard() const { return scores; }

    // Convenience for UI layer
    void pushSystemMessage(const std::string& msg);

    // Auto-move / auto-explore
    bool isAutoActive() const { return autoMode != AutoMoveMode::None; }
    bool isAutoTraveling() const { return autoMode == AutoMoveMode::Travel; }
    bool isAutoExploring() const { return autoMode == AutoMoveMode::Explore; }
    const std::vector<Vec2i>& autoPath() const { return autoPathTiles; }
    bool requestAutoTravel(Vec2i goal);
    void requestAutoExplore();
    void cancelAutoMove(bool silent = false);
    void setAutoStepDelayMs(int ms);
    void setAutoExploreSearchEnabled(bool enabled) { autoExploreSearchEnabled_ = enabled; }
    bool autoExploreSearchEnabled() const { return autoExploreSearchEnabled_; }

    // Mouse helpers (do not consume turns directly)
    void beginLookAt(Vec2i p);
    void setLookCursor(Vec2i p);
    void setTargetCursor(Vec2i p);

    // Describe the tile/entity at a map coordinate (used by UI tooltips such as the minimap cursor info line).
    // Returns "UNKNOWN" for unexplored tiles and avoids spoiling secret doors.
    std::string describeAt(Vec2i p) const;

    // Unit-test access hook.
    friend struct GameTestAccess;

private:
    // Drop an item on the ground, merging into an existing stack when possible.
    // This reduces clutter for stackable items (ammo, gold, potions, scrolls, etc.).
    void dropGroundItem(Vec2i pos, ItemKind k, int count = 1, int enchant = 0);
    void dropGroundItemItem(Vec2i pos, Item it);

    // Bones files (persistent death remnants between runs).
    bool tryApplyBones();
    bool writeBonesFile();

    // Flavor graffiti generation for a freshly created level.
    // (Runs only for newly generated levels, not restored ones.)
    void spawnGraffiti();

    Dungeon dung;
    RNG rng;

    DungeonBranch branch_ = DungeonBranch::Main;
    int depth_ = 1;

    // Persistent visited levels (monsters + items + explored tiles)
    std::map<LevelId, LevelState> levels;

    // Infinite overworld chunk cache (Camp depth 0).
    //
    // The hub camp is always at (0,0) and is stored in `levels[{Camp,0}]` like before.
    // Wilderness chunks are stored here and are currently treated as ephemeral (not serialized).
    struct OverworldKey {
        int x = 0;
        int y = 0;
        bool operator<(const OverworldKey& o) const {
            if (x != o.x) return x < o.x;
            return y < o.y;
        }
    };

    int overworldX_ = 0;
    int overworldY_ = 0;
    std::map<OverworldKey, LevelState> overworldChunks_;

    // Monsters/companions that fell through trap doors into a deeper level.
    // These are queued by destination depth and spawned when that level is entered.
    // Index 0 is unused.
    // v33+: creatures that fell through trap doors to deeper levels but haven't been placed yet.
    // Keyed by (branch, depth) so multiple branches can safely coexist.
    std::map<LevelId, std::vector<Entity>> trapdoorFallers_{};

    std::vector<Entity> ents;
    int nextEntityId = 1;
    int playerId_ = 0;

    // Items on ground (current level)
    std::vector<GroundItem> ground;

    // Opened chest contents / containers on current level.
    std::vector<ChestContainer> chestContainers_;
    // Traps on current level
    std::vector<Trap> trapsCur;

    // Player map markers / notes on current level.
    std::vector<MapMarker> mapMarkers_;

    // Floor engravings / graffiti on current level.
    std::vector<Engraving> engravings_;

    // Environmental fields (current level).
    // Stored as per-tile intensities (0..255).
    std::vector<uint8_t> confusionGas_;
    std::vector<uint8_t> poisonGas_;
    std::vector<uint8_t> fireField_;
    std::vector<uint8_t> scentField_;

    int nextItemId = 1;

    // Player inventory & equipment
    std::vector<Item> inv;
    // Shop bills: debt recorded for consumed/destroyed unpaid goods (per depth).
    // This is additive to per-item shopPrice tagging.
    std::array<int, DUNGEON_MAX_DEPTH + 1> shopDebtLedger_{};

    // Merchant guild pursuit state: set when you steal from a shop and persists across floors.
    bool merchantGuildAlerted_ = false;

    // Shrine economy: piety earned via donations/sacrifices and spent on shrine services.
    int piety_ = 0;
    // Simple prayer timeout: once you receive a shrine service, you cannot receive another
    // one until this turn count is reached.
    uint32_t prayerCooldownUntilTurn_ = 0u;

    // Spell system (append-only): mana + known spells bitmask.
    // NOTE: These are persisted (v44+) but the load migration is not yet wired.
    int mana_ = 0;
    uint32_t knownSpellsMask_ = 0u;

    int equipMeleeId = 0;
    int equipRangedId = 0;
    int equipArmorId = 0;
    int equipRing1Id = 0;
    int equipRing2Id = 0;
    bool invOpen = false;
    int invSel = 0;
    // Temporary inventory sub-mode (used for prompts like selecting an item to identify).
    bool invIdentifyMode = false;
    bool invEnchantRingMode = false;

    // Additional modal inventory prompts (e.g., shrine services that need a target item).
    enum class InvPromptKind : uint8_t {
        None = 0,
        ShrineIdentify,
        ShrineBless,
        ShrineRecharge,
        ShrineSacrifice,
    };
    InvPromptKind invPrompt_ = InvPromptKind::None;

    // Chest container overlay (loot/stash).
    bool chestOpen = false;
    int chestOpenId = 0;
    int chestSel = 0;
    bool chestPaneChest = true;
    int chestOpenTier_ = 0;
    int chestOpenMaxStacks_ = 0;

    // Targeting mode
    bool targeting = false;
    Vec2i targetPos{0,0};
    std::vector<Vec2i> targetLine;
    bool targetValid = false;
    std::string targetStatusText_; // reason for invalid targeting (UI-only; not serialized)

    // Targeting safety (UI-only; not serialized): used for friendly-fire/self-damage warnings
    // and a two-step confirmation when the shot is risky.
    bool targetUnsafe_ = false;
    bool targetUnsafeConfirmed_ = false;
    std::string targetWarningText_;

    enum class TargetingMode : uint8_t {
        Ranged = 0,
        Spell,
    };
    TargetingMode targetingMode_ = TargetingMode::Ranged;
    SpellKind targetingSpell_ = SpellKind::MagicMissile;

    // Spells overlay (UI-only; not serialized)
    bool spellsOpen = false;
    int spellsSel = 0;

    // Kick prompt mode (directional)
    bool kicking = false;

    // Dig prompt mode (directional)
    bool digging = false;

    // Help overlay
    bool helpOpen = false;
    int helpScroll_ = 0; // UI-only; not serialized


    // Look/examine mode
    bool looking = false;
    Vec2i lookPos{0,0};

    // Acoustic preview (UI-only; not serialized). When enabled, we cache a sound-cost map
    // from the LOOK cursor so the renderer can show an in-world sound propagation heatmap.
    bool soundPreviewOpen = false;
    int soundPreviewVol = 12;      // final volume (base + bias), 0..30
    int soundPreviewVolBase = 12;  // derived from your current footstep model at the LOOK cursor
    int soundPreviewVolBias = 0;   // user adjustment via [ ] while preview is open
    Vec2i soundPreviewSrc{0,0};
    std::vector<int> soundPreviewDist;

    bool threatPreviewOpen = false;
    int threatPreviewMaxCost = 12;
    std::vector<Vec2i> threatPreviewSrcs;
    std::vector<int> threatPreviewDist;

    // Hearing preview (UI-only; not serialized): caches a per-tile "minimum volume required"
    // field for currently visible hostiles, plus the player's per-tile footstep volume.
    bool hearingPreviewOpen = false;
    int hearingPreviewVolBias = 0; // user bias via [ ] while preview is open
	    std::vector<Vec2i> hearingPreviewListeners_;
	    std::vector<int> hearingPreviewDominantListenerIndex_;
    std::vector<int> hearingPreviewMinReq;
    std::vector<int> hearingPreviewFootstepVol;

    // Scent preview (UI-only; not serialized): visualize the player's lingering scent trail
    // as a heatmap + flow arrows in LOOK mode.
    bool scentPreviewOpen = false;
    int scentPreviewCutoff_ = 24; // 0..255 minimum scent intensity to render (adjustable via [ ])

    // Minimap / stats overlays
    bool minimapOpen = false;
    bool statsOpen = false;

    // Minimap cursor (UI-only). Allows clicking/keyboard navigation in the minimap.
    Vec2i minimapCursorPos_{0,0};
    bool minimapCursorActive_ = false;
    int minimapZoom_ = 0;


    // Level-up talent allocation overlay (forced while points are pending)
    bool levelUpOpen = false;
    int levelUpSel = 0;


    // Options / command overlays
    bool optionsOpen = false;
    int optionsSel = 0;

    // Keybinds overlay (interactive editor; does not consume turns)
    bool keybindsOpen = false;
    int keybindsSel = 0;
    int keybindsScroll_ = 0;
    bool keybindsCapture = false;
    bool keybindsCaptureAdd = false;
    int keybindsCaptureIndex = -1;

    // Keybinds list filter (UI-only; not serialized).
    bool keybindsSearchMode_ = false;
    std::string keybindsSearch_;

    std::vector<std::pair<std::string, std::string>> keybindsDesc_;


    bool commandOpen = false;
    std::string commandBuf;
    int commandCursor_ = 0; // byte index into commandBuf
    std::string commandDraft;
    std::vector<std::string> commandHistory;
    int commandHistoryPos = -1;

    // Command prompt tab-completion cycle (UI-only; not serialized).
    std::string commandAutoBase;
    std::string commandAutoPrefix; // text before the token being completed (e.g. "bind ")
    std::vector<std::string> commandAutoMatches;
    std::vector<std::string> commandAutoHints;
    std::vector<std::string> commandAutoDescs;
    int commandAutoIndex = -1;
    bool commandAutoFuzzy = false;

    // Settings persistence + UI helpers
    std::string settingsPath_;
    bool settingsDirtyFlag = false;
    bool slotDirtyFlag = false;

    // Application-level quit request (e.g., from extended command "quit")
    bool quitReq = false;

    // Application-level requests (extended commands may set these; main.cpp handles them).
    bool keyBindsReloadReq = false;
    bool keyBindsDumpReq = false;
    bool configReloadReq = false;

    // Messages
    std::vector<Message> msgs;
    int msgScroll = 0;

    // Message history overlay (full log viewer)
    bool msgHistoryOpen = false;
    bool msgHistorySearchMode = false;
    MessageFilter msgHistoryFilter = MessageFilter::All;
    std::string msgHistorySearch;
    int msgHistoryScroll = 0;

    // Monster codex overlay (bestiary / encounter log)
    bool codexOpen = false;
    CodexFilter codexFilter_ = CodexFilter::All;
    CodexSort codexSort_ = CodexSort::Kind;
    int codexSel = 0;

    // Item discoveries overlay (identification reference)
    bool discoveriesOpen = false;
    DiscoveryFilter discoveriesFilter_ = DiscoveryFilter::All;
    DiscoverySort discoveriesSort_ = DiscoverySort::Appearance;
    int discoveriesSel = 0;

    // Scores / run history overlay (Hall of Fame)
    bool scoresOpen = false;
    ScoresView scoresView_ = ScoresView::Top;
    int scoresSel = 0;

    // Options / quality-of-life
    AutoPickupMode autoPickup = AutoPickupMode::Gold;

    // Sneak mode (quiet movement; affects footstep noise).
    bool sneakMode_ = false;

    bool confirmQuitEnabled_ = true;
    bool autoMortemEnabled_ = true;
    bool mortemWritten_ = false;

    bool bonesEnabled_ = true;
    bool bonesWritten_ = false;

    // Endless / infinite world (experimental): allow depths beyond DUNGEON_MAX_DEPTH.
    bool infiniteWorldEnabled_ = false;
    // Sliding window size (in floors) for keeping deep (post-quest) levels cached.
    int infiniteKeepWindow_ = 12;

    // Hunger system (optional; when disabled, hunger does not tick).
    bool hungerEnabled_ = false;
    int hunger = 0;
    int hungerMax = 0;
    int hungerStatePrev = 0; // for message throttling

    // Encumbrance system (optional; when enabled, inventory weight slows/restricts the player).
    bool encumbranceEnabled_ = false;
    BurdenState burdenPrev_ = BurdenState::Unburdened; // for message throttling

    // Lighting / darkness system (optional).
    bool lightingEnabled_ = false;
    // Light map for the current level (0..255 brightness per tile). Recomputed when FOV updates.
    std::vector<uint8_t> lightMap_;
    // Per-tile light color for the current level (RGB 0..255). Recomputed with lightMap_.
    std::vector<Color> lightColorMap_;

    // Item identification (NetHack-style). When enabled, potions/scrolls start unknown each run
    // and become identified through use/identify scrolls.
    bool identifyItemsEnabled = true;
    std::array<uint8_t, ITEM_KIND_COUNT> identKnown{};       // 0/1
    std::array<uint8_t, ITEM_KIND_COUNT> identAppearance{};  // appearance id (category-local)
    std::array<std::string, ITEM_KIND_COUNT> identCall{};      // user "call" labels (per-run; saved)

    // Auto-move / auto-explore state (stepped in update() for UX)
    AutoMoveMode autoMode = AutoMoveMode::None;
    std::vector<Vec2i> autoPathTiles;
    size_t autoPathIndex = 0;
    float autoStepTimer = 0.0f;
    float autoStepDelay = 0.045f; // seconds between auto steps (configurable)

    // Auto-explore goal tracking (used to stop on arrival when targeting visible loot).
    bool autoExploreGoalIsLoot = false;
    Vec2i autoExploreGoalPos{-1, -1};

    // Auto-explore: optional secret-hunting pass when the floor appears fully explored.
    bool autoExploreSearchEnabled_ = false;
    bool autoExploreGoalIsSearch = false;
    Vec2i autoExploreSearchGoalPos{-1, -1};
    int autoExploreSearchTurnsLeft = 0;
    bool autoExploreSearchAnnounced = false; // message shown for the current secret-hunt stretch
    std::vector<uint8_t> autoExploreSearchTriedTurns; // per-tile number of auto-search turns already spent

    // Auto-travel: one-time warning throttle when hostiles are visible but still far away.
    bool autoTravelCautionAnnounced = false;

    // Save path overrides (set by main using SDL_GetPrefPath)
    std::string savePathOverride;
    std::string activeSlot_;
    // How many rotated backups to keep for save/autosave files.
    int saveBackups_ = 3;
    std::string autosavePathOverride;
    std::string scoresPathOverride;

    // Time / pacing
    uint32_t turnCount = 0;

    // Optional per-turn hook (used by the replay recorder/verifier).
    TurnHookFn turnHookFn_ = nullptr;
    void* turnHookUser_ = nullptr;
    int naturalRegenCounter = 0;

    // Haste is handled as "every other player action skips the monster turn".
    // This flag tracks whether we've already taken the "free" haste action.
    bool hastePhase = false;

    // Visual FX
    std::vector<FXProjectile> fx;
    std::vector<FXExplosion> fxExpl;
    std::vector<FXParticleEvent> fxParticles_;
    bool inputLock = false;

    bool gameOver = false;
    bool gameWon = false;

    // Player progression
    int charLevel = 1;
    int xp = 0;
    int xpNext = 20;

    // Talent allocations (earned on level-up). Saved per-run.
    int talentMight_ = 0;
    int talentAgility_ = 0;
    int talentVigor_ = 0;
    int talentFocus_ = 0;
    int talentPointsPending_ = 0;


    // Run meta / stats
    uint32_t seed_ = 0;
    uint32_t killCount = 0;

    // Conduct tracking (NetHack-style voluntary challenges).
    // These do not affect gameplay, but are saved so they survive save/load.
    uint32_t directKillCount_ = 0;       // kills delivered by the player (not allies)
    uint32_t conductFoodEaten_ = 0;      // times the player ate any food (rations/corpses)
    uint32_t conductCorpseEaten_ = 0;    // corpses eaten
    uint32_t conductScrollsRead_ = 0;    // scrolls read
    uint32_t conductSpellbooksRead_ = 0; // spellbooks studied
    uint32_t conductPrayers_ = 0;        // shrine services used

    int maxDepth = 1;

    // Monster codex knowledge (per-run).
    std::array<uint8_t, ENTITY_KIND_COUNT> codexSeen_{};
    std::array<uint16_t, ENTITY_KIND_COUNT> codexKills_{};

    // Yendor Doom (endgame escalation).
    //
    // When enabled, picking up the Amulet of Yendor activates a pressure system
    // that grows over time and as the player ascends.
    bool yendorDoomEnabled_ = true;   // user toggle (persisted via settings)
    bool yendorDoomActive_ = false;   // per-run state (serialized in save v21+)
    int yendorDoomLevel_ = 0;         // cached for HUD; recomputed during tick
    uint32_t yendorDoomStartTurn_ = 0;
    uint32_t yendorDoomLastPulseTurn_ = 0;
    uint32_t yendorDoomLastSpawnTurn_ = 0;
    int yendorDoomMsgStage_ = 0;

    // Player identity (persisted via settings)
    std::string playerName_ = "PLAYER";

    // Starting class/role (saved per-run; default is set from settings/CLI for new games).
    PlayerClass playerClass_ = PlayerClass::Adventurer;


    // End-of-run cause string for scoreboard / UI
    std::string endCause_;

    // UI preferences (persisted via settings)
    bool showEffectTimers_ = true;
    bool perfOverlayEnabled_ = false; // UI-only: show perf HUD overlay
    UITheme uiTheme_ = UITheme::DarkStone;
    bool uiPanelsTextured_ = true;
    ViewMode viewMode_ = ViewMode::TopDown;
    ControlPreset controlPreset_ = ControlPreset::Modern;
    bool voxelSpritesEnabled_ = true;
    bool isoVoxelRaytraceEnabled_ = false;
    bool isoTerrainVoxelBlocksEnabled_ = true;
    bool isoCutawayEnabled_ = true;
    bool procPaletteEnabled_ = true;
    int procPaletteStrength_ = 70; // 0..100

    // Autosave
    int autosaveInterval = 0; // 0 = off
    uint32_t lastAutosaveTurn = 0;

    // Scoreboard
    ScoreBoard scores;
    bool runRecorded = false;

private:
    void pushMsg(const std::string& s, MessageKind kind = MessageKind::Info, bool fromPlayer = true);

    Entity* entityById(int id);
    const Entity* entityById(int id) const;

    Entity* entityAtMut(int x, int y);
    const Entity* entityAt(int x, int y) const;

    bool tryMove(Entity& e, int dx, int dy);

    // Compute the positional noise volume produced by a single player step onto `pos`.
    // Used by movement, and by LOOK-mode Sound Preview so the UI matches real stealth rules.
    int playerFootstepNoiseVolumeAt(Vec2i pos) const;

    // Recompute the cached LOOK-mode sound preview map (volume + dist field).
    // Safe to call frequently while the cursor moves.
    void refreshSoundPreview();
    // Recompute the cached LOOK-mode hearing preview fields.
    // Safe to call on toggle or when hostiles visibility changes.
    void refreshHearingPreview();
    // If kick=true, perform an unarmed kick attack (ignores equipped melee weapon)
    // with a stronger knockback profile.
    void attackMelee(Entity& attacker, Entity& defender, bool kick = false);
    void attackRanged(Entity& attacker, Vec2i target, int range, int atkBonus, int dmgBonus, ProjectileKind projKind, bool fromPlayer, const Item* projectileTemplate = nullptr, bool wandPowered = false);

    void monsterTurn();
    void cleanupDead();

    void recomputeFov();
    void recomputeLightMap();

    // Inventory actions
    void openInventory();
    void closeInventory();
    void moveInventorySelection(int dy);
    void sortInventory();
    bool pickupAtPlayer();
    bool dropSelected();
    bool dropSelectedAll();
    bool equipSelected();
    bool useSelected();

    // Spells
    void openSpells();
    void closeSpells();
    void moveSpellsSelection(int dy);
    bool canCastSpell(SpellKind k, std::string* reasonOut = nullptr) const;
    // Cast immediately (no target selection).
    bool castSpell(SpellKind k);
    // Cast at an already-selected target (used by spell targeting).
    bool castSpellAt(SpellKind k, Vec2i target);

    // Targeting actions
    void beginTargeting();
    void beginSpellTargeting(SpellKind k);
    bool endTargeting(bool fire);
    void moveTargetCursor(int dx, int dy);
    void recomputeTargetLine();
    // Cycle between visible hostile targets while in targeting mode.
    // dir: +1 = next, -1 = previous.
    void cycleTargetCursor(int dir);
    void zapDiggingWand(int range);

    // Level transitions
    void storeCurrentLevel();
    bool restoreLevel(LevelId id);

    // Overworld chunk travel (Camp depth 0).
    // Triggered when the player attempts to step out-of-bounds through an edge gate.
    bool tryOverworldStep(int dx, int dy);
    bool restoreOverworldChunk(int x, int y);
    void pruneOverworldChunks();
    void changeLevel(int newDepth, bool goingDown);
    void changeLevel(LevelId newLevel, bool goingDown);

    // Deterministic per-level worldgen seed (run seed + level identity).
    // Used to decouple procedural generation from the gameplay RNG stream.
    uint32_t levelGenSeed(LevelId id) const;

    // Backwards compatibility: older code called this "infiniteLevelSeed" when the
    // deterministic mode was only used for Infinite World.
    uint32_t infiniteLevelSeed(LevelId id) const { return levelGenSeed(id); }

    // Endless / infinite world helpers.
    void ensureEndlessSanctumDownstairs(LevelId id, Dungeon& d, RNG& rngForPlacement) const;
    void pruneEndlessLevels();

    // Shops / economy
    // Triggered when the player escapes a shop with unpaid goods or attacks the shopkeeper.
    void triggerShopTheftAlarm(Vec2i shopInsidePos, Vec2i playerPos);

    // Save/load
public:
    bool saveToFile(const std::string& path, bool quiet = false);

    // Loads a save file. When reportErrors=false, this is silent (returns false on any failure)
    // so callers can attempt fallback strategies (e.g. rotated backups).
    bool loadFromFile(const std::string& path, bool reportErrors = true);

    // Convenience: attempt to load the requested save file, and if it fails,
    // automatically try rotated backups (<path>.bak1..bak10) in order.
    bool loadFromFileWithBackups(const std::string& path);
private:

    // Helpers
    int equippedMeleeIndex() const;
    int equippedRangedIndex() const;
    int equippedArmorIndex() const;
    int equippedRing1Index() const;
    int equippedRing2Index() const;


    // Passive equipment bonuses (currently only rings).
    int ringTalentBonusMight() const;
    int ringTalentBonusAgility() const;
    int ringTalentBonusVigor() const;
    int ringTalentBonusFocus() const;
    int ringDefenseBonus() const;
    int playerRangedRange() const;
    bool playerHasRangedReady(std::string* reasonOut) const;

    // Progression
    void grantXp(int amount);
    void onPlayerLevelUp();

    // Generation
    void spawnMonsters();
    void spawnItems();
    void spawnTraps();
    void spawnFountains();
    void spawnAltars();

    // Surface camp (depth 0) fixtures.
    void setupSurfaceCampInstallations();

    // Identification
    void initIdentificationTables();
    bool isIdentified(ItemKind k) const;
    bool markIdentified(ItemKind k, bool quiet = false);
    uint8_t appearanceFor(ItemKind k) const;
    std::string appearanceName(ItemKind k) const;
    std::string unknownDisplayName(const Item& it) const;

    // QoL / traps / status
    bool autoPickupAtPlayer();
    bool openChestAtPlayer();
    bool drinkFromFountain();
    // Ambush mimics: used by chest mimics and item mimics.
    // `lootToDrop` (if non-null) will be carried by the mimic and dropped on death.
    void revealMimicFromBait(Vec2i baitPos, const std::string& revealMsg, const Item* lootToDrop);

    // Chest container overlay
    bool openChestOverlayAtPlayer();
    void closeChestOverlay();
    void moveChestSelection(int dy);
    bool chestMoveSelected(bool moveAll);
    bool chestMoveAll();
    void sortChestContents(int chestId, int* selInOut);
    bool consumeKeys(int n);
    bool consumeLockpicks(int n);

    // Dig action (directional prompt)
    void beginDig();

    // Kick action (directional prompt)
    void beginKick();
    bool kickInDirection(int dx, int dy);

    // Auto-move helpers
    bool hasRangedWeaponForAmmo(AmmoKind ammo) const;
    bool autoPickupWouldPick(ItemKind k) const;
    bool autoExploreWantsLoot(ItemKind k) const;
    bool tileHasAutoExploreLoot(Vec2i p) const;

    bool stepAutoMove();
    // Tactical helper: choose a best-effort single-step evasion move away from visible hostiles.
    bool evadeStep();
    bool buildAutoTravelPath(Vec2i goal, bool requireExplored, bool allowKnownTraps);
    bool buildAutoExplorePath();
    Vec2i findNearestExploreFrontier() const;
    Vec2i findNearestExploreSearchSpot() const;
    std::vector<Vec2i> findPathBfs(Vec2i start, Vec2i goal, bool requireExplored, bool allowKnownTraps) const;
    void stopAutoMove(bool silent);
    bool searchForTraps(bool verbose = true, int* foundTrapsOut = nullptr, int* foundSecretsOut = nullptr);
    void autoSearchTick();
    bool disarmTrap();
    bool closeDoor();
    bool lockDoor();
    Trap* trapAtMut(int x, int y);
    void triggerTrapAt(Vec2i pos, Entity& victim, bool fromDisarm = false);
    // Engraving-based floor effects (sigils). Triggered on entering a tile, like traps.
    void triggerSigilAt(Vec2i pos, Entity& victim);

    // Magical mind-wipe effect used by rare traps and confused scroll interactions.
    //
    // - Clears explored map memory (and auto-explore search bookkeeping) beyond a small
    //   local radius.
    // - Forgets discovered traps beyond that radius.
    // - Forgets far-away map markers.
    // - Unseen hostile monsters may lose track of the player.
    //
    // keepRadiusCheb <= 0 means "forget everything except what you can currently see".
    void applyAmnesiaShock(int keepRadiusCheb);
    // Alert monsters to a sound/event at `pos`. radius<=0 means "global" (all monsters).
    void alertMonstersTo(Vec2i pos, int radius);
    // Emit a positional noise and alert monsters that can hear it.
    //
    // Unlike alertMonstersTo(), this uses a dungeon-aware sound propagation
    // (walls/secret doors block; doors muffle) so noise doesn't "teleport"
    // through solid rock.
    void emitNoise(Vec2i pos, int volume);
    void applyEndOfTurnEffects();
    void updateScentMap();
    Vec2i randomFreeTileInRoom(const Room& r, int tries = 200);

    // Monster factory (shared by level generation + dynamic spawns).
    // Returns a fully-initialized Entity (id, stats, speed, ammo, gear, ...).
    Entity makeMonster(EntityKind k, Vec2i pos, int groupId, bool allowGear, uint32_t forcedSpriteSeed = 0, bool allowProcVariant = true);
    // Convenience wrapper that appends the monster to the entity list and returns a reference.
    Entity& spawnMonster(EntityKind k, Vec2i pos, int groupId, bool allowGear = true);

    // Yendor Doom: endgame escalation tick (runs once per player action that consumes time).
    void onAmuletAcquired();
    void tickYendorDoom();
    void spawnYendorHunterPack(int doomLevel);
    int computeYendorDoomLevel() const;

    // Unified "time passes" handler (runs monsters + end-of-turn effects, handles haste)
    void advanceAfterPlayerAction();
    bool anyVisibleHostiles() const;

    // Look/examine mode helpers
    void beginLook();
    void endLook();
    void moveLookCursor(int dx, int dy);

    // Rest action
    void restUntilSafe();

    // Autosave / run history
    void maybeAutosave();
    void maybeRecordRun();

    // Line util
    static std::vector<Vec2i> bresenhamLine(Vec2i a, Vec2i b);
};
