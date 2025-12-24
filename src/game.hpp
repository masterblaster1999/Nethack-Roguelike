#pragma once
#include "common.hpp"
#include "dungeon.hpp"
#include "items.hpp"
#include "rng.hpp"
#include <cstdint>
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
};

enum class Action : uint8_t {
    None = 0,
    Up,
    Down,
    Left,
    Right,
    Wait,

    Confirm,     // Enter / > in some contexts
    Cancel,      // Escape (in-game cancel, main may still quit if not in a mode)

    StairsDown,
    Restart,

    Pickup,
    Inventory,
    Fire,

    Equip,
    Use,
    Drop,

    LogUp,
    LogDown,
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

    bool alerted = false;

    uint32_t spriteSeed = 0;
};

struct FXProjectile {
    ProjectileKind kind = ProjectileKind::Arrow;
    std::vector<Vec2i> path;
    size_t pathIndex = 0;
    float stepTimer = 0.0f;
    float stepTime = 0.03f; // seconds per tile
};

class Game {
public:
    static constexpr int MAP_W = 30;
    static constexpr int MAP_H = 20;

    Game();

    void newGame(uint32_t seed);
    void nextLevel();

    void handleAction(Action a);
    void update(float dt);

    const Dungeon& dungeon() const { return dung; }
    const std::vector<Entity>& entities() const { return ents; }
    const std::vector<GroundItem>& groundItems() const { return ground; }

    const Entity& player() const;
    Entity& playerMut();

    int playerId() const { return playerId_; }

    int level() const { return level_; }
    bool isGameOver() const { return gameOver; }

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

    // Targeting
    bool isTargeting() const { return targeting; }
    Vec2i targetingCursor() const { return targetPos; }
    const std::vector<Vec2i>& targetingLine() const { return targetLine; }
    bool targetingIsValid() const { return targetValid; }

    // Messages + scrollback
    const std::vector<std::string>& messages() const { return msgs; }
    int messageScroll() const { return msgScroll; }

    // FX
    const std::vector<FXProjectile>& fxProjectiles() const { return fx; }
    bool inputLocked() const { return inputLock; }

private:
    Dungeon dung;
    RNG rng;

    int level_ = 1;

    std::vector<Entity> ents;
    int nextEntityId = 1;
    int playerId_ = 0;

    // Items on ground
    std::vector<GroundItem> ground;
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

    // Messages
    std::vector<std::string> msgs;
    int msgScroll = 0;

    // Visual FX
    std::vector<FXProjectile> fx;
    bool inputLock = false;

    bool gameOver = false;

private:
    void pushMsg(const std::string& s);

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

    // Helpers
    int equippedMeleeIndex() const;
    int equippedRangedIndex() const;
    int equippedArmorIndex() const;
    const Item* equippedMelee() const;
    const Item* equippedRanged() const;
    const Item* equippedArmor() const;
    int playerRangedRange() const;
    bool playerHasRangedReady(std::string* reasonOut) const;

    // Generation
    void spawnMonsters();
    void spawnItems();
    Vec2i randomFreeTileInRoom(const Room& r, int tries = 200);

    // Line util
    static std::vector<Vec2i> bresenhamLine(Vec2i a, Vec2i b);
};
