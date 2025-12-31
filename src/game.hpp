#pragma once
#include "common.hpp"
#include "dungeon.hpp"
#include "items.hpp"
#include "effects.hpp"
#include "rng.hpp"
#include "scores.hpp"

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <array>
#include <map>
#include <string>
#include <vector>

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
};


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
        default: return 100;
    }
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
};

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

// Optional NetHack-style burden/encumbrance states.
// Used by the carrying capacity system (when enabled in settings).
enum class BurdenState : uint8_t {
    Unburdened = 0,
    Burdened,
    Stressed,
    Strained,
    Overloaded,
};

enum class MessageKind : uint8_t {
    Info = 0,
    Combat,
    Loot,
    System,
    Warning,
    Success,
};

// UI skin theme (purely cosmetic; persisted via settings).
enum class UITheme : uint8_t {
    DarkStone = 0,
    Parchment,
    Arcane,
};

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

    // Turn scheduling (energy-based).
    // Each full player turn, monsters gain `speed` energy and can act once per 100 energy.
    // Example: speed=150 -> ~1.5 actions/turn (sometimes 2).
    int speed = 100;
    int energy = 0;
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

struct LevelState {
    int depth = 1;
    Dungeon dung;
    std::vector<Entity> monsters;
    std::vector<GroundItem> ground;
    std::vector<Trap> traps;

    // Environmental fields (per-tile intensities).
    // Currently used for persistent Confusion Gas clouds.
    std::vector<uint8_t> confusionGas;
};

class Game {
public:
    static constexpr int MAP_W = 30;
    static constexpr int MAP_H = 20;

    // Run structure / quest pacing.
    // The default run now spans 10 floors.
    static constexpr int DUNGEON_MAX_DEPTH = 10;
    static constexpr int QUEST_DEPTH = DUNGEON_MAX_DEPTH; // where the Amulet of Yendor is guaranteed
    static constexpr int MIDPOINT_DEPTH = DUNGEON_MAX_DEPTH / 2;

    Game();

    void newGame(uint32_t seed);

    void handleAction(Action a);

    // Spend a turn to emit a loud noise (useful to lure monsters).
    // Available via extended command: #shout
    void shout();
    void whistle();

    // Dig/tunnel using a pickaxe (adjacent tile). Used by extended command: #dig <dir>
    // Returns true if a turn was spent.
    bool digInDirection(int dx, int dy);

    // QoL: repeat the Search action multiple times without spamming the log.
    // Used by the extended command: #search N
    // Returns how many turns were actually spent searching.
    int repeatSearch(int maxTurns, bool stopOnFind = true);


    // Shrine interaction (use via #pray or future keybind).
    // mode: \"heal\" | \"cure\" | \"identify\" | \"bless\" | \"\" (auto)
    bool prayAtShrine(const std::string& mode = std::string());
    void update(float dt);

    const Dungeon& dungeon() const { return dung; }
    const std::vector<Entity>& entities() const { return ents; }
    const std::vector<GroundItem>& groundItems() const { return ground; }
    const std::vector<Trap>& traps() const { return trapsCur; }


    // Persistent environmental gas on the current level (0..255 intensity).
    // 0 means no gas. Only meaningful on in-bounds tiles.
    uint8_t confusionGasAt(int x, int y) const;

    const Entity& player() const;
    Entity& playerMut();

    int playerId() const { return playerId_; }

    int depth() const { return depth_; }

    int dungeonMaxDepth() const { return DUNGEON_MAX_DEPTH; }
    int questDepth() const { return QUEST_DEPTH; }

    bool isGameOver() const { return gameOver; }
    bool isGameWon() const { return gameWon; }
    bool isFinished() const { return gameOver || gameWon; }

    // Run meta
    uint32_t seed() const { return seed_; }
    uint32_t kills() const { return killCount; }
    int maxDepthReached() const { return maxDepth; }

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

// UI skin (purely cosmetic)
UITheme uiTheme() const { return uiTheme_; }
void setUITheme(UITheme theme) { uiTheme_ = theme; }

bool uiPanelsTextured() const { return uiPanelsTextured_; }
void setUIPanelsTextured(bool textured) { uiPanelsTextured_ = textured; }

    // Inventory/UI accessors for renderer
    const std::vector<Item>& inventory() const { return inv; }
    bool isInventoryOpen() const { return invOpen; }
    // True when the inventory overlay is being used for a special prompt (e.g. choosing an item for Scroll of Identify).
    bool isInventoryIdentifyMode() const { return invIdentifyMode; }
    int inventorySelection() const { return invSel; }
    bool isEquipped(int itemId) const;
    std::string equippedTag(int itemId) const; // e.g. "M", "R", "A", "1", "2"
    std::string equippedMeleeName() const;
    std::string equippedRangedName() const;
    std::string equippedArmorName() const;
    std::string equippedRing1Name() const;
    std::string equippedRing2Name() const;
    int playerAttack() const;
    int playerDefense() const;

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
    // Shops / economy
    // Total gold owed for UNPAID items currently in inventory.
    int shopDebtTotal() const;
    // Gold owed to the shop on the current depth.
    int shopDebtThisDepth() const;
    // True if the player is standing inside a shop room.
    bool playerInShop() const;
    // Pay for all (or as many as possible) unpaid goods on this depth. Use via #pay.
    bool payAtShop();

    int keyCount() const;
    int lockpickCount() const;
    int characterLevel() const { return charLevel; }
    int experience() const { return xp; }
    int experienceToNext() const { return xpNext; }
    AutoPickupMode autoPickupMode() const { return autoPickup; }
    bool autoPickupEnabled() const { return autoPickup != AutoPickupMode::Off; }
    void setAutoPickupMode(AutoPickupMode m);

    bool playerHasAmulet() const;

    // Item display helpers (respect identification settings + run knowledge).
    std::string displayItemName(const Item& it) const;
    std::string displayItemNameSingle(ItemKind k) const;

    void setIdentificationEnabled(bool enabled);
    bool identificationEnabled() const { return identifyItemsEnabled; }

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


    // Targeting
    bool isTargeting() const { return targeting; }
    Vec2i targetingCursor() const { return targetPos; }
    const std::vector<Vec2i>& targetingLine() const { return targetLine; }
    bool targetingIsValid() const { return targetValid; }

    // Kick prompt (directional)
    bool isKicking() const { return kicking; }

    // Help overlay
    bool isHelpOpen() const { return helpOpen; }

    // Look/examine overlay (cursor-based inspection)
    bool isLooking() const { return looking; }
    Vec2i lookCursor() const { return lookPos; }
    std::string lookInfoText() const;

    // Minimap / stats overlays
    bool isMinimapOpen() const { return minimapOpen; }
    bool isStatsOpen() const { return statsOpen; }

    // Options overlay
    bool isOptionsOpen() const { return optionsOpen; }
    int optionsSelection() const { return optionsSel; }

    // NetHack-like extended command prompt
    bool isCommandOpen() const { return commandOpen; }
    const std::string& commandBuffer() const { return commandBuf; }
    void commandTextInput(const char* utf8);
    void commandBackspace();
    void commandAutocomplete();

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

    // Messages + scrollback
    const std::vector<Message>& messages() const { return msgs; }
    int messageScroll() const { return msgScroll; }

    // FX
    const std::vector<FXProjectile>& fxProjectiles() const { return fx; }
    const std::vector<FXExplosion>& fxExplosions() const { return fxExpl; }
    bool inputLocked() const { return inputLock; }

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

    // Mouse helpers (do not consume turns directly)
    void beginLookAt(Vec2i p);
    void setLookCursor(Vec2i p);
    void setTargetCursor(Vec2i p);

private:
    // Drop an item on the ground, merging into an existing stack when possible.
    // This reduces clutter for stackable items (ammo, gold, potions, scrolls, etc.).
    void dropGroundItem(Vec2i pos, ItemKind k, int count = 1, int enchant = 0);
    void dropGroundItemItem(Vec2i pos, Item it);

    Dungeon dung;
    RNG rng;

    int depth_ = 1;

    // Persistent visited levels (monsters + items + explored tiles)
    std::map<int, LevelState> levels;

    std::vector<Entity> ents;
    int nextEntityId = 1;
    int playerId_ = 0;

    // Items on ground (current level)
    std::vector<GroundItem> ground;
    // Traps on current level
    std::vector<Trap> trapsCur;

    // Environmental fields (current level).
    // Stored as per-tile intensities (0..255).
    std::vector<uint8_t> confusionGas_;

    int nextItemId = 1;

    // Player inventory & equipment
    std::vector<Item> inv;
    int equipMeleeId = 0;
    int equipRangedId = 0;
    int equipArmorId = 0;
    int equipRing1Id = 0;
    int equipRing2Id = 0;
    bool invOpen = false;
    int invSel = 0;
    // Temporary inventory sub-mode (used for prompts like selecting an item to identify).
    bool invIdentifyMode = false;

    // Targeting mode
    bool targeting = false;
    Vec2i targetPos{0,0};
    std::vector<Vec2i> targetLine;
    bool targetValid = false;

    // Kick prompt mode (directional)
    bool kicking = false;

    // Help overlay
    bool helpOpen = false;

    // Look/examine mode
    bool looking = false;
    Vec2i lookPos{0,0};

    // Minimap / stats overlays
    bool minimapOpen = false;
    bool statsOpen = false;


    // Level-up talent allocation overlay (forced while points are pending)
    bool levelUpOpen = false;
    int levelUpSel = 0;


    // Options / command overlays
    bool optionsOpen = false;
    int optionsSel = 0;

    bool commandOpen = false;
    std::string commandBuf;
    std::string commandDraft;
    std::vector<std::string> commandHistory;
    int commandHistoryPos = -1;

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

    // Options / quality-of-life
    AutoPickupMode autoPickup = AutoPickupMode::Gold;

    // Sneak mode (quiet movement; affects footstep noise).
    bool sneakMode_ = false;

    bool confirmQuitEnabled_ = true;
    bool autoMortemEnabled_ = true;
    bool mortemWritten_ = false;

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

    // Item identification (NetHack-style). When enabled, potions/scrolls start unknown each run
    // and become identified through use/identify scrolls.
    bool identifyItemsEnabled = true;
    std::array<uint8_t, ITEM_KIND_COUNT> identKnown{};       // 0/1
    std::array<uint8_t, ITEM_KIND_COUNT> identAppearance{};  // appearance id (category-local)

    // Auto-move / auto-explore state (stepped in update() for UX)
    AutoMoveMode autoMode = AutoMoveMode::None;
    std::vector<Vec2i> autoPathTiles;
    size_t autoPathIndex = 0;
    float autoStepTimer = 0.0f;
    float autoStepDelay = 0.045f; // seconds between auto steps (configurable)

    // Auto-explore goal tracking (used to stop on arrival when targeting visible loot).
    bool autoExploreGoalIsLoot = false;
    Vec2i autoExploreGoalPos{-1, -1};

    // Save path overrides (set by main using SDL_GetPrefPath)
    std::string savePathOverride;
    std::string activeSlot_;
    // How many rotated backups to keep for save/autosave files.
    int saveBackups_ = 3;
    std::string autosavePathOverride;
    std::string scoresPathOverride;

    // Time / pacing
    uint32_t turnCount = 0;
    int naturalRegenCounter = 0;

    // Haste is handled as "every other player action skips the monster turn".
    // This flag tracks whether we've already taken the "free" haste action.
    bool hastePhase = false;

    // Visual FX
    std::vector<FXProjectile> fx;
    std::vector<FXExplosion> fxExpl;
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
    int maxDepth = 1;

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
    UITheme uiTheme_ = UITheme::DarkStone;
    bool uiPanelsTextured_ = true;

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
    // If kick=true, perform an unarmed kick attack (ignores equipped melee weapon)
    // with a stronger knockback profile.
    void attackMelee(Entity& attacker, Entity& defender, bool kick = false);
    void attackRanged(Entity& attacker, Vec2i target, int range, int atkBonus, int dmgBonus, ProjectileKind projKind, bool fromPlayer, const Item* projectileTemplate = nullptr);

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

    // Targeting actions
    void beginTargeting();
    void endTargeting(bool fire);
    void moveTargetCursor(int dx, int dy);
    void recomputeTargetLine();
    void zapDiggingWand(int range);

    // Level transitions
    void storeCurrentLevel();
    bool restoreLevel(int depth);
    void changeLevel(int newDepth, bool goingDown);

    // Save/load
public:
    bool saveToFile(const std::string& path, bool quiet = false);
    bool loadFromFile(const std::string& path);
private:

    // Helpers
    int equippedMeleeIndex() const;
    int equippedRangedIndex() const;
    int equippedArmorIndex() const;
    int equippedRing1Index() const;
    int equippedRing2Index() const;
    const Item* equippedMelee() const;
    const Item* equippedRanged() const;
    const Item* equippedArmor() const;
    const Item* equippedRing1() const;
    const Item* equippedRing2() const;

    // Passive equipment bonuses (currently only rings).
    int ringTalentBonusMight() const;
    int ringTalentBonusAgility() const;
    int ringTalentBonusVigor() const;
    int ringTalentBonusFocus() const;
    int ringDefenseBonus() const;
    int playerRangedRange() const;
    bool playerHasRangedReady(std::string* reasonOut) const;

    // Progression
    int xpFor(EntityKind k) const;
    void grantXp(int amount);
    void onPlayerLevelUp();

    // Generation
    void spawnMonsters();
    void spawnItems();
    void spawnTraps();

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
    bool consumeKeys(int n);
    bool consumeLockpicks(int n);

    // Kick action (directional prompt)
    void beginKick();
    bool kickInDirection(int dx, int dy);

    // Auto-move helpers
    bool hasRangedWeaponForAmmo(AmmoKind ammo) const;
    bool autoPickupWouldPick(ItemKind k) const;
    bool autoExploreWantsLoot(ItemKind k) const;
    bool tileHasAutoExploreLoot(Vec2i p) const;

    bool stepAutoMove();
    bool buildAutoTravelPath(Vec2i goal, bool requireExplored);
    bool buildAutoExplorePath();
    Vec2i findNearestExploreFrontier() const;
    std::vector<Vec2i> findPathBfs(Vec2i start, Vec2i goal, bool requireExplored) const;
    void stopAutoMove(bool silent);
    bool searchForTraps(bool verbose = true, int* foundTrapsOut = nullptr, int* foundSecretsOut = nullptr);
    bool disarmTrap();
    bool closeDoor();
    bool lockDoor();
    Trap* trapAtMut(int x, int y);
    void triggerTrapAt(Vec2i pos, Entity& victim, bool fromDisarm = false);
    // Alert monsters to a sound/event at `pos`. radius<=0 means "global" (all monsters).
    void alertMonstersTo(Vec2i pos, int radius);
    // Emit a positional noise and alert monsters that can hear it.
    //
    // Unlike alertMonstersTo(), this uses a dungeon-aware sound propagation
    // (walls/secret doors block; doors muffle) so noise doesn't "teleport"
    // through solid rock.
    void emitNoise(Vec2i pos, int volume);
    void applyEndOfTurnEffects();
    Vec2i randomFreeTileInRoom(const Room& r, int tries = 200);

    // Monster factory (shared by level generation + dynamic spawns).
    // Returns a fully-initialized Entity (id, stats, speed, ammo, gear, ...).
    Entity makeMonster(EntityKind k, Vec2i pos, int groupId, bool allowGear);
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
    std::string describeAt(Vec2i p) const;

    // Rest action
    void restUntilSafe();

    // Autosave / run history
    void maybeAutosave();
    void maybeRecordRun();

    // Line util
    static std::vector<Vec2i> bresenhamLine(Vec2i a, Vec2i b);
};
