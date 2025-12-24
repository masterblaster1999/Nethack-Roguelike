#pragma once
#include "common.hpp"
#include "dungeon.hpp"
#include "rng.hpp"
#include <string>
#include <vector>

enum class EntityKind : uint8_t {
    Player = 0,
    Goblin,
    Bat,
    Slime,
};

struct Entity {
    int id = 0;
    EntityKind kind = EntityKind::Goblin;

    Vec2i pos{0, 0};

    int hp = 1;
    int maxHp = 1;
    int atk = 1;

    uint32_t spriteSeed = 1;
    bool alive = true;
};

enum class Action : uint8_t {
    None = 0,
    MoveUp,
    MoveDown,
    MoveLeft,
    MoveRight,
    Wait,
    StairsDown,
    Restart,
};

class Game {
public:
    static constexpr int MAP_W = 30;
    static constexpr int MAP_H = 20;

    Game();

    void newGame(uint32_t seed);
    void nextLevel();

    void handleAction(Action a);

    const Dungeon& dungeon() const { return dung; }
    const std::vector<Entity>& entities() const { return ents; }

    int level() const { return levelIndex; }
    bool isGameOver() const { return gameOver; }

    const Entity& player() const;
    Entity& playerMut();

    const std::vector<std::string>& messages() const { return log; }

    // Convenience for rendering
    const Entity* entityAt(int x, int y) const;

private:
    RNG rng;
    Dungeon dung;

    std::vector<Entity> ents;
    int playerId = 0;
    int nextId = 1;

    int levelIndex = 1;
    bool gameOver = false;

    std::vector<std::string> log;

    void buildLevel(bool keepPlayerStats);
    Vec2i findPlayerSpawn() const;

    void spawnMonsters();
    void pushMsg(const std::string& s);

    Entity* entityById(int id);
    Entity* entityAtMut(int x, int y);

    bool tryMove(Entity& e, int dx, int dy);
    void attack(Entity& attacker, Entity& defender);

    void monsterTurn();
    void cleanupDead();

    void recomputeFov();
};
