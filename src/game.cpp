#include "game.hpp"
#include <algorithm>
#include <sstream>

Game::Game() : dung(MAP_W, MAP_H) {}

const Entity& Game::player() const {
    for (const auto& e : ents) if (e.id == playerId) return e;
    // Should not happen.
    return ents.front();
}

Entity& Game::playerMut() {
    for (auto& e : ents) if (e.id == playerId) return e;
    return ents.front();
}

Entity* Game::entityById(int id) {
    for (auto& e : ents) if (e.id == id) return &e;
    return nullptr;
}

const Entity* Game::entityAt(int x, int y) const {
    for (const auto& e : ents) {
        if (!e.alive) continue;
        if (e.pos.x == x && e.pos.y == y) return &e;
    }
    return nullptr;
}

Entity* Game::entityAtMut(int x, int y) {
    for (auto& e : ents) {
        if (!e.alive) continue;
        if (e.pos.x == x && e.pos.y == y) return &e;
    }
    return nullptr;
}

void Game::pushMsg(const std::string& s) {
    log.push_back(s);
    const size_t MAX = 7;
    if (log.size() > MAX) {
        log.erase(log.begin(), log.end() - MAX);
    }
}

Vec2i Game::findPlayerSpawn() const {
    // Find a floor tile (not stairs) scanning from top-left.
    for (int y = 1; y < dung.height - 1; ++y) {
        for (int x = 1; x < dung.width - 1; ++x) {
            const auto& t = dung.at(x, y);
            if (t.type == TileType::Floor) {
                return {x, y};
            }
        }
    }
    // Fallback
    return {1, 1};
}

void Game::buildLevel(bool keepPlayerStats) {
    dung.resize(MAP_W, MAP_H);
    dung.generate(rng);

    Entity savedPlayer{};
    bool haveSavedPlayer = false;

    if (keepPlayerStats && playerId != 0) {
        savedPlayer = player();
        haveSavedPlayer = true;
    }

    ents.clear();

    Vec2i spawn = findPlayerSpawn();

    if (haveSavedPlayer) {
        savedPlayer.pos = spawn;
        savedPlayer.alive = (savedPlayer.hp > 0);
        ents.push_back(savedPlayer);
        playerId = savedPlayer.id;
    } else {
        Entity p;
        p.id = nextId++;
        p.kind = EntityKind::Player;
        p.pos = spawn;
        p.maxHp = 20;
        p.hp = 20;
        p.atk = 6;
        p.spriteSeed = rng.nextU32();
        p.alive = true;

        ents.push_back(p);
        playerId = p.id;
    }

    spawnMonsters();

    recomputeFov();
}

void Game::newGame(uint32_t seed) {
    rng = RNG(seed);
    nextId = 1;
    levelIndex = 1;
    gameOver = false;
    log.clear();

    buildLevel(false);
    pushMsg("WELCOME TO PROCROGUE!");
    pushMsg("ARROWS/WASD MOVE. '.' WAIT. '>' STAIRS. R RESTART.");
}

void Game::nextLevel() {
    levelIndex++;
    buildLevel(true);

    std::ostringstream ss;
    ss << "YOU DESCEND TO LEVEL " << levelIndex << ".";
    pushMsg(ss.str());
}

static Entity makeMonster(int id, EntityKind kind, Vec2i pos, RNG& rng, int levelIndex) {
    Entity e;
    e.id = id;
    e.kind = kind;
    e.pos = pos;
    e.spriteSeed = rng.nextU32();
    e.alive = true;

    switch (kind) {
        case EntityKind::Goblin:
            e.maxHp = 6 + levelIndex;
            e.atk = 4 + levelIndex / 2;
            break;
        case EntityKind::Bat:
            e.maxHp = 4 + levelIndex;
            e.atk = 3 + levelIndex / 2;
            break;
        case EntityKind::Slime:
            e.maxHp = 8 + levelIndex;
            e.atk = 2 + levelIndex / 2;
            break;
        default:
            e.maxHp = 5 + levelIndex;
            e.atk = 3 + levelIndex / 2;
            break;
    }
    e.hp = e.maxHp;
    return e;
}

void Game::spawnMonsters() {
    // Spawn a few monsters on random floor tiles, not too close to the player.
    const int baseCount = 6;
    const int count = baseCount + (levelIndex - 1) / 2;

    auto isFree = [&](int x, int y) -> bool {
        if (!dung.isWalkable(x, y)) return false;
        if (entityAt(x, y)) return false;
        return true;
    };

    int spawned = 0;
    int attempts = 0;
    const int maxAttempts = 500;

    while (spawned < count && attempts < maxAttempts) {
        attempts++;

        int x = rng.range(1, dung.width - 2);
        int y = rng.range(1, dung.height - 2);
        if (!isFree(x, y)) continue;

        int pd = std::abs(x - player().pos.x) + std::abs(y - player().pos.y);
        if (pd < 6) continue;

        EntityKind kind = EntityKind::Goblin;
        int r = rng.range(0, 99);
        if (r < 45) kind = EntityKind::Goblin;
        else if (r < 70) kind = EntityKind::Bat;
        else kind = EntityKind::Slime;

        ents.push_back(makeMonster(nextId++, kind, {x, y}, rng, levelIndex));
        spawned++;
    }
}

bool Game::tryMove(Entity& e, int dx, int dy) {
    int nx = e.pos.x + dx;
    int ny = e.pos.y + dy;

    if (!dung.inBounds(nx, ny)) return false;
    if (!dung.isWalkable(nx, ny)) {
        if (e.kind == EntityKind::Player) pushMsg("YOU BUMP INTO A WALL.");
        return false;
    }

    if (Entity* other = entityAtMut(nx, ny)) {
        if (other->id == e.id) return false;
        attack(e, *other);
        return true;
    }

    e.pos.x = nx;
    e.pos.y = ny;
    return true;
}

static const char* kindName(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return "YOU";
        case EntityKind::Goblin: return "GOBLIN";
        case EntityKind::Bat: return "BAT";
        case EntityKind::Slime: return "SLIME";
        default: return "THING";
    }
}

void Game::attack(Entity& attacker, Entity& defender) {
    if (!attacker.alive || !defender.alive) return;

    int dmg = rng.range(1, std::max(1, attacker.atk));
    defender.hp -= dmg;

    {
        std::ostringstream ss;
        ss << kindName(attacker.kind) << " HIT " << kindName(defender.kind) << " FOR " << dmg << ".";
        pushMsg(ss.str());
    }

    if (defender.hp <= 0) {
        defender.alive = false;
        defender.hp = 0;

        std::ostringstream ss;
        ss << kindName(defender.kind) << " DIES.";
        pushMsg(ss.str());

        if (defender.kind == EntityKind::Player) {
            gameOver = true;
            pushMsg("YOU DIED. PRESS R TO RESTART.");
        }
    }
}

void Game::monsterTurn() {
    const Entity& p = player();

    for (auto& m : ents) {
        if (!m.alive) continue;
        if (m.kind == EntityKind::Player) continue;

        int dx = p.pos.x - m.pos.x;
        int dy = p.pos.y - m.pos.y;
        int manhattan = std::abs(dx) + std::abs(dy);

        bool acted = false;

        if (manhattan <= 8) {
            // Chase: step along the dominant axis (slightly randomized).
            int stepX = 0, stepY = 0;
            if (std::abs(dx) > std::abs(dy)) {
                stepX = sign(dx);
            } else if (std::abs(dy) > std::abs(dx)) {
                stepY = sign(dy);
            } else {
                // Equal: random axis
                if (rng.chance(0.5f)) stepX = sign(dx);
                else stepY = sign(dy);
            }

            if (stepX == 0 && stepY == 0) {
                // On top (shouldn't happen).
            } else {
                acted = tryMove(m, stepX, stepY);
            }
        } else {
            // Wander: 50% random move
            if (rng.chance(0.5f)) {
                int dir = rng.range(0, 3);
                static const int D[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                acted = tryMove(m, D[dir][0], D[dir][1]);
            }
        }

        (void)acted;
    }
}

void Game::cleanupDead() {
    // Remove dead monsters. Keep the player entity even if dead.
    ents.erase(std::remove_if(ents.begin(), ents.end(),
                             [](const Entity& e) {
                                 return (!e.alive && e.kind != EntityKind::Player);
                             }),
              ents.end());
}

void Game::recomputeFov() {
    dung.computeFov(player().pos.x, player().pos.y, 8);
}

void Game::handleAction(Action a) {
    if (a == Action::None) return;

    if (a == Action::Restart) {
        // Derive a fresh seed from current RNG state to make restart feel different.
        uint32_t seed = hash32(rng.nextU32() ^ 0xA5A5A5A5u);
        newGame(seed);
        return;
    }

    if (gameOver) return;

    bool acted = false;

    switch (a) {
        case Action::MoveUp:    acted = tryMove(playerMut(), 0, -1); break;
        case Action::MoveDown:  acted = tryMove(playerMut(), 0, 1);  break;
        case Action::MoveLeft:  acted = tryMove(playerMut(), -1, 0); break;
        case Action::MoveRight: acted = tryMove(playerMut(), 1, 0);  break;
        case Action::Wait:
            pushMsg("YOU WAIT.");
            acted = true;
            break;
        case Action::StairsDown:
            if (dung.inBounds(player().pos.x, player().pos.y) &&
                dung.at(player().pos.x, player().pos.y).type == TileType::StairsDown) {
                nextLevel();
                return; // new level, don't take a monster turn immediately
            } else {
                pushMsg("THERE ARE NO STAIRS HERE.");
            }
            break;
        default:
            break;
    }

    if (acted) {
        monsterTurn();
        cleanupDead();
        recomputeFov();
    }
}
