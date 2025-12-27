#pragma once
#include "common.hpp"
#include "dungeon.hpp"
#include "items.hpp"
#include "rng.hpp"
#include "scores.hpp"

#include <cstdint>
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
};

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
    ToggleAutoPickup,   // Cycle auto-pickup mode (OFF/GOLD/ALL)
    AutoExplore,        // Auto-explore (walk to nearest unexplored frontier)

    // UI / QoL
    Look,               // Examine tiles without taking a turn
    Rest,               // Rest (auto-wait) until healed or interrupted

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

enum class MessageKind : uint8_t {
    Info = 0,
    Combat,
    Loot,
    System,
    Warning,
    Success,
};

// Deterministic "daily" seed derived from the current UTC date.
// If outDateIso is non-null, it receives an ISO date like "2025-12-27".
uint32_t dailySeedUtc(std::string* outDateIso = nullptr);

struct Message {
    std::string text;
    MessageKind kind = MessageKind::Info;
    bool fromPlayer = true;
};

enum class TrapKind : uint8_t {
    Spike = 0,
    PoisonDart,
    Teleport,
    Alarm,
    Web,
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

    // Monster behavior flags
    bool canRanged = false;
    int rangedRange = 0;
    int rangedAtk = 0;
    ProjectileKind rangedProjectile = ProjectileKind::Arrow;
    AmmoKind rangedAmmo = AmmoKind::None; // mostly for future use

    bool willFlee = false;
    bool packAI = false;
    int groupId = 0;

    // Optional regen (used by Troll)
    int regenChancePct = 0; // 0 = none
    int regenAmount = 0;

    bool alerted = false;

    // Timed status effects (mostly used by player, but can be applied to monsters too).
    int poisonTurns = 0;   // lose 1 HP per full turn
    int regenTurns = 0;    // heal 1 HP per full turn
    int shieldTurns = 0;   // temporary defense boost

    // New timed buffs
    int hasteTurns = 0;    // grants extra player actions (decrements on monster turns)
    int visionTurns = 0;   // increases FOV radius

    // New timed debuffs
    int webTurns = 0;      // prevents movement while >0

    uint32_t spriteSeed = 0;
};

struct FXProjectile {
    ProjectileKind kind = ProjectileKind::Arrow;
    std::vector<Vec2i> path;
    size_t pathIndex = 0;
    float stepTimer = 0.0f;
    float stepTime = 0.03f; // seconds per tile
};

struct LevelState {
    int depth = 1;
    Dungeon dung;
    std::vector<Entity> monsters;
    std::vector<GroundItem> ground;
    std::vector<Trap> traps;
};

class Game {
public:
    static constexpr int MAP_W = 30;
    static constexpr int MAP_H = 20;

    Game();

    void newGame(uint32_t seed);

    void handleAction(Action a);

    // Shrine interaction (use via #pray or future keybind).
    // mode: \"heal\" | \"cure\" | \"identify\" | \"bless\" | \"\" (auto)
    bool prayAtShrine(const std::string& mode = std::string());
    void update(float dt);

    const Dungeon& dungeon() const { return dung; }
    const std::vector<Entity>& entities() const { return ents; }
    const std::vector<GroundItem>& groundItems() const { return ground; }
    const std::vector<Trap>& traps() const { return trapsCur; }

    const Entity& player() const;
    Entity& playerMut();

    int playerId() const { return playerId_; }

    int depth() const { return depth_; }

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

    // End-of-run cause (e.g., "KILLED BY GOBLIN", "ESCAPED WITH THE AMULET")
    const std::string& endCause() const { return endCause_; }

    // HUD preferences
    bool showEffectTimers() const { return showEffectTimers_; }
    void setShowEffectTimers(bool enabled) { showEffectTimers_ = enabled; }

    // Inventory/UI accessors for renderer
    const std::vector<Item>& inventory() const { return inv; }
    bool isInventoryOpen() const { return invOpen; }
    int inventorySelection() const { return invSel; }
    bool isEquipped(int itemId) const;
    std::string equippedTag(int itemId) const; // e.g. "M", "R", "A", "MR"
    std::string equippedMeleeName() const;
    std::string equippedRangedName() const;
    std::string equippedArmorName() const;
    int playerAttack() const;
    int playerDefense() const;

    // Progression
    int playerCharLevel() const { return charLevel; }
    int playerXp() const { return xp; }
    int playerXpToNext() const { return xpNext; }

    // Convenience getters (kept for renderer/legacy naming)
    int goldCount() const { return countGold(inv); }
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
    int nextItemId = 1;

    // Player inventory & equipment
    std::vector<Item> inv;
    int equipMeleeId = 0;
    int equipRangedId = 0;
    int equipArmorId = 0;
    bool invOpen = false;
    int invSel = 0;

    // Targeting mode
    bool targeting = false;
    Vec2i targetPos{0,0};
    std::vector<Vec2i> targetLine;
    bool targetValid = false;

    // Help overlay
    bool helpOpen = false;

    // Look/examine mode
    bool looking = false;
    Vec2i lookPos{0,0};

    // Minimap / stats overlays
    bool minimapOpen = false;
    bool statsOpen = false;

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

    bool confirmQuitEnabled_ = true;
    bool autoMortemEnabled_ = true;
    bool mortemWritten_ = false;

    // Hunger system (optional; when disabled, hunger does not tick).
    bool hungerEnabled_ = false;
    int hunger = 0;
    int hungerMax = 0;
    int hungerStatePrev = 0; // for message throttling

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
    bool inputLock = false;

    bool gameOver = false;
    bool gameWon = false;

    // Player progression
    int charLevel = 1;
    int xp = 0;
    int xpNext = 20;

    // Run meta / stats
    uint32_t seed_ = 0;
    uint32_t killCount = 0;
    int maxDepth = 1;

    // Player identity (persisted via settings)
    std::string playerName_ = "PLAYER";

    // End-of-run cause string for scoreboard / UI
    std::string endCause_;

    // UI preferences (persisted via settings)
    bool showEffectTimers_ = true;

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
    void attackMelee(Entity& attacker, Entity& defender);
    void attackRanged(Entity& attacker, Vec2i target, int range, int atk, ProjectileKind projKind, bool fromPlayer);

    void monsterTurn();
    void cleanupDead();

    void recomputeFov();

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
    const Item* equippedMelee() const;
    const Item* equippedRanged() const;
    const Item* equippedArmor() const;
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
    bool searchForTraps();
    Trap* trapAtMut(int x, int y);
    void triggerTrapAt(Vec2i pos, Entity& victim);
    void applyEndOfTurnEffects();
    Vec2i randomFreeTileInRoom(const Room& r, int tries = 200);

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
