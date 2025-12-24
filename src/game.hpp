#pragma once
#include "common.hpp"
#include "dungeon.hpp"
#include "items.hpp"
#include "rng.hpp"
#include <cstdint>
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
};

enum class Action : uint8_t {
    None = 0,
    Up,
    Down,
    Left,
    Right,
    Wait,

    Confirm,     // Enter / context confirm
    Cancel,      // Escape (close UI modes)

    StairsUp,
    StairsDown,
    Restart,

    Pickup,
    Inventory,
    Fire,

    Equip,
    Use,
    Drop,

    Save,
    Load,
    Help,

    LogUp,
    LogDown,

    // New actions
    Search,            // Spend a turn searching for nearby traps
    ToggleAutoPickup,  // Toggle auto-pickup of gold

    // UI / QoL
    Look,              // Examine tiles without taking a turn
    Rest,              // Rest (auto-wait) until healed or interrupted
};

enum class MessageKind : uint8_t {
    Info = 0,
    Combat,
    Loot,
    System,
    Warning,
    Success,
};

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
    int characterLevel() const { return charLevel; }
    int experience() const { return xp; }
    int experienceToNext() const { return xpNext; }

    bool autoPickupEnabled() const { return autoPickupGold; }
    bool playerHasAmulet() const;

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

    // Messages
    std::vector<Message> msgs;
    int msgScroll = 0;

    // Options / quality-of-life
    bool autoPickupGold = true;

    // Time / pacing
    uint32_t turnCount = 0;
    int naturalRegenCounter = 0;

    // Haste is handled as "every other player action skips the monster turn".
    // This flag tracks whether we've already taken the "free" haste action.
    bool hastePhase = false;

    // Look/examine mode
    bool looking = false;
    Vec2i lookPos{0,0};

    // Visual FX
    std::vector<FXProjectile> fx;
    bool inputLock = false;

    bool gameOver = false;
    bool gameWon = false;

    // Player progression
    int charLevel = 1;
    int xp = 0;
    int xpNext = 20;

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
    bool pickupAtPlayer();
    bool dropSelected();
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
    bool saveToFile(const std::string& path);
    bool loadFromFile(const std::string& path);

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

    // QoL / traps / status
    bool autoPickupGoldAtPlayer();
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

    // Line util
    static std::vector<Vec2i> bresenhamLine(Vec2i a, Vec2i b);
};
