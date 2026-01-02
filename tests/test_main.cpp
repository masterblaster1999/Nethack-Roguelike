#include "dungeon.hpp"
#include "items.hpp"
#include "combat_rules.hpp"
#include "grid_utils.hpp"
#include "physics.hpp"
#include "pathfinding.hpp"
#include "rng.hpp"
#include "scores.hpp"
#include "settings.hpp"
#include "slot_utils.hpp"
#include "replay.hpp"
#include "replay_runner.hpp"
#include "content.hpp"
#include "version.hpp"

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <fstream>
#if __has_include(<filesystem>)
#include <filesystem>
#endif
#include <iostream>
#include <queue>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        ++failures;
        std::cerr << "[FAIL] " << msg << "\n";
    }
}

void test_rng_reproducible() {
    RNG rng(123u);
    const std::vector<uint32_t> expected = {
        31682556u,
        4018661298u,
        2101636938u,
        3842487452u,
        1628673942u,
    };

    for (size_t i = 0; i < expected.size(); ++i) {
        const uint32_t v = rng.nextU32();
        expect(v == expected[i], "RNG sequence mismatch at index " + std::to_string(i));
    }

    // Also validate range() stays within bounds.
    for (int i = 0; i < 1000; ++i) {
        int r = rng.range(-3, 7);
        expect(r >= -3 && r <= 7, "RNG range() out of bounds");
    }
}

void test_dungeon_stairs_connected() {
    const int depthsToTest[] = {1, 3, 4, 5};

    for (int depth : depthsToTest) {
        RNG rng(42u + static_cast<uint32_t>(depth));
        Dungeon d(30, 20);
        d.generate(rng, depth, /*maxDepth=*/10);

        auto inBounds = [&](Vec2i p) {
            return d.inBounds(p.x, p.y);
        };

        expect(inBounds(d.stairsUp), "stairsUp out of bounds (depth " + std::to_string(depth) + ")");
        expect(inBounds(d.stairsDown), "stairsDown out of bounds (depth " + std::to_string(depth) + ")");

        if (inBounds(d.stairsUp)) {
            expect(d.at(d.stairsUp.x, d.stairsUp.y).type == TileType::StairsUp,
                   "stairsUp tile type incorrect (depth " + std::to_string(depth) + ")");
        }
        if (inBounds(d.stairsDown)) {
            expect(d.at(d.stairsDown.x, d.stairsDown.y).type == TileType::StairsDown,
                   "stairsDown tile type incorrect (depth " + std::to_string(depth) + ")");
        }

        // BFS from up to down using passable tiles.
        std::vector<uint8_t> visited(static_cast<size_t>(d.width * d.height), 0);
        auto idx = [&](int x, int y) { return static_cast<size_t>(y * d.width + x); };

        std::queue<Vec2i> q;
        if (inBounds(d.stairsUp)) {
            q.push(d.stairsUp);
            visited[idx(d.stairsUp.x, d.stairsUp.y)] = 1;
        }

        const int dirs[8][2] = {
            {1,0},{-1,0},{0,1},{0,-1},
            {1,1},{1,-1},{-1,1},{-1,-1},
        };

        while (!q.empty()) {
            Vec2i p = q.front();
            q.pop();

            for (auto& dxy : dirs) {
                int nx = p.x + dxy[0];
                int ny = p.y + dxy[1];
                if (!d.inBounds(nx, ny)) continue;
                // Match in-game diagonal movement rules: no cutting corners.
                if (dxy[0] != 0 && dxy[1] != 0) {
                    int ox = p.x + dxy[0];
                    int oy = p.y;
                    int px = p.x;
                    int py = p.y + dxy[1];
                    if (!d.inBounds(ox, oy) || !d.inBounds(px, py)) continue;
                    if (!d.isWalkable(ox, oy)) continue;
                    if (!d.isWalkable(px, py)) continue;
                }
                if (!d.isPassable(nx, ny)) continue;
                size_t id = idx(nx, ny);
                if (visited[id]) continue;
                visited[id] = 1;
                q.push({nx, ny});
            }
        }

        if (inBounds(d.stairsDown)) {
            expect(visited[idx(d.stairsDown.x, d.stairsDown.y)] != 0,
                   "stairsDown not reachable from stairsUp (depth " + std::to_string(depth) + ")");
        }
    }
}

void test_final_floor_sanctum_layout() {
    RNG rng(999u);
    Dungeon d;
    d.generate(rng, /*depth=*/10, /*maxDepth=*/10);

    expect(d.inBounds(d.stairsUp.x, d.stairsUp.y), "final floor has stairs up");
    expect(!d.inBounds(d.stairsDown.x, d.stairsDown.y), "final floor has no stairs down");

    bool hasLockedDoor = false;
    bool hasChasm = false;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const TileType t = d.at(x, y).type;
            if (t == TileType::DoorLocked) hasLockedDoor = true;
            if (t == TileType::Chasm) hasChasm = true;
        }
    }

    expect(hasLockedDoor, "final floor contains at least one locked door");
    expect(hasChasm, "final floor contains chasms (moat) for tactical play");

    bool hasTreasureRoom = false;
    for (const Room& r : d.rooms) {
        if (r.type == RoomType::Treasure) {
            hasTreasureRoom = true;
            break;
        }
    }
    expect(hasTreasureRoom, "final floor defines a treasure room (amulet anchor)");
}

void test_secret_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorSecret;

    expect(!d.isPassable(5, 5), "Secret doors should not be passable until discovered");
    expect(d.isOpaque(5, 5), "Secret doors should be opaque (block FOV/LOS) until discovered");
    expect(!d.isWalkable(5, 5), "Secret doors should not be walkable until discovered");
}

void test_chasm_and_pillar_tile_rules() {
    Dungeon d(10, 10);

    d.at(5, 5).type = TileType::Chasm;
    expect(!d.isPassable(5, 5), "Chasm should not be passable");
    expect(!d.isWalkable(5, 5), "Chasm should not be walkable");
    expect(!d.isOpaque(5, 5), "Chasm should not block FOV/LOS");

    d.at(6, 5).type = TileType::Pillar;
    expect(!d.isPassable(6, 5), "Pillar should not be passable");
    expect(!d.isWalkable(6, 5), "Pillar should not be walkable");
    expect(d.isOpaque(6, 5), "Pillar should block FOV/LOS");

    // LOS sanity: a chasm tile shouldn't block visibility.
    // Carve a 1x5 corridor with a chasm in the middle.
    for (int x = 1; x <= 8; ++x) d.at(x, 2).type = TileType::Floor;
    d.at(4, 2).type = TileType::Chasm;
    d.computeFov(1, 2, 20);
    expect(d.at(8, 2).visible, "Chasm should not block FOV in a corridor");

    // Pillar should block visibility.
    d.at(6, 2).type = TileType::Pillar;
    d.computeFov(1, 2, 20);
    expect(!d.at(8, 2).visible, "Pillar should block FOV in a corridor");
}

void test_locked_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorLocked;

    expect(d.isDoorLocked(5, 5), "DoorLocked should be detected as locked door");
    expect(!d.isPassable(5, 5), "Locked doors should not be passable until unlocked");
    expect(d.isOpaque(5, 5), "Locked doors should be opaque (block FOV/LOS) while closed");
    expect(!d.isWalkable(5, 5), "Locked doors should not be walkable while closed");

    d.unlockDoor(5, 5);
    expect(d.isDoorClosed(5, 5), "unlockDoor should convert a locked door into a closed door");
    d.openDoor(5, 5);
    expect(d.at(5, 5).type == TileType::DoorOpen, "openDoor should open an unlocked door");
}


void test_close_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorOpen;

    expect(d.isDoorOpen(5, 5), "DoorOpen should be detected as open door");
    expect(d.isPassable(5, 5), "Open door should be passable");
    expect(d.isWalkable(5, 5), "Open door should be walkable");
    expect(!d.isOpaque(5, 5), "Open door should not block FOV/LOS");

    d.closeDoor(5, 5);

    expect(d.isDoorClosed(5, 5), "closeDoor should convert an open door into a closed door");
    expect(!d.isDoorOpen(5, 5), "Closed door should not still be reported as open");
    // Closed doors are passable for pathing/AI, but not walkable for movement.
    expect(d.isPassable(5, 5), "Closed door should be passable for pathing/AI");
    expect(!d.isWalkable(5, 5), "Closed door should not be walkable while closed");
    expect(d.isOpaque(5, 5), "Closed door should block FOV/LOS");
}

void test_lock_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorClosed;

    d.lockDoor(5, 5);

    expect(d.isDoorLocked(5, 5), "lockDoor should convert a closed door into a locked door");
    expect(!d.isPassable(5, 5), "Locked doors should not be passable after lockDoor");
    expect(d.isOpaque(5, 5), "Locked doors should be opaque after lockDoor");
    expect(!d.isWalkable(5, 5), "Locked doors should not be walkable after lockDoor");

    d.unlockDoor(5, 5);
    expect(d.isDoorClosed(5, 5), "unlockDoor should convert a locked door back into a closed door");
}

void test_fov_locked_door_blocks_visibility() {
    Dungeon d(10, 5);

    // Start with solid walls.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Wall;
            d.at(x, y).visible = false;
            d.at(x, y).explored = false;
        }
    }

    // Carve a straight hallway with a locked door.
    for (int x = 1; x <= 4; ++x) {
        d.at(x, 2).type = TileType::Floor;
    }
    d.at(3, 2).type = TileType::DoorLocked;

    // Locked door should block visibility.
    d.computeFov(1, 2, 10);
    expect(d.at(3, 2).visible, "Locked door tile should be visible");
    expect(!d.at(4, 2).visible, "Tile behind locked door should not be visible");

    // Open door should allow visibility through.
    d.at(3, 2).type = TileType::DoorOpen;
    d.computeFov(1, 2, 10);
    expect(d.at(4, 2).visible, "Tile behind open door should be visible");
}


void test_los_blocks_diagonal_corner_peek() {
    Dungeon d(5, 5);

    // Start with open floor everywhere.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
        }
    }

    // Two adjacent orthogonal walls should block diagonal LOS between (1,1) and (2,2).
    // Layout (P=origin, X=target, #=wall):
    //   . . . . .
    //   . P # . .
    //   . # X . .
    //   . . . . .
    //   . . . . .
    d.at(2, 1).type = TileType::Wall;
    d.at(1, 2).type = TileType::Wall;

    expect(!d.hasLineOfSight(1, 1, 2, 2),
           "LOS should be blocked by diagonal corner walls (no corner peeking)");

    // If one side is open, LOS should be allowed.
    d.at(2, 1).type = TileType::Floor;
    expect(d.hasLineOfSight(1, 1, 2, 2),
           "LOS should allow diagonal visibility if at least one side is open");
}


void test_sound_propagation_respects_walls_and_muffling_doors() {
    Dungeon d(5, 5);

    // Start with solid walls.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Wall;
        }
    }

    // Carve a 1-tile corridor: (1,2) -> (3,2)
    d.at(1, 2).type = TileType::Floor;
    d.at(2, 2).type = TileType::DoorClosed; // muffling, but transmits sound
    d.at(3, 2).type = TileType::Floor;

    // Through a closed door: cost should be 2 (door) + 1 (floor) = 3.
    auto sound = d.computeSoundMap(1, 2, 10);
    expect(sound[2 * d.width + 1] == 0, "Sound source should be 0 cost");
    expect(sound[2 * d.width + 3] == 3, "Sound should pass through closed door with muffling cost");

    // If we cap maxCost below 3, the target should remain unreachable (-1).
    auto soundTight = d.computeSoundMap(1, 2, 2);
    expect(soundTight[2 * d.width + 3] == -1, "maxCost should limit sound propagation");

    // Replace the door with a wall: sound should not reach.
    d.at(2, 2).type = TileType::Wall;
    auto soundBlocked = d.computeSoundMap(1, 2, 10);
    expect(soundBlocked[2 * d.width + 3] == -1, "Walls should block sound propagation");
}

void test_sound_diagonal_corner_cutting_is_blocked() {
    Dungeon d(3, 3);

    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Wall;
        }
    }

    // Origin and diagonal target.
    d.at(1, 1).type = TileType::Floor;
    d.at(2, 2).type = TileType::Floor;

    // Both orthogonal tiles are walls -> diagonal should be blocked.
    d.at(2, 1).type = TileType::Wall;
    d.at(1, 2).type = TileType::Wall;

    auto sound = d.computeSoundMap(1, 1, 5);
    expect(sound[2 * d.width + 2] == -1, "Sound should not cut diagonally through two blocking tiles");

    // If one orthogonal tile is passable, diagonal propagation is allowed.
    d.at(2, 1).type = TileType::Floor;
    auto sound2 = d.computeSoundMap(1, 1, 5);
    expect(sound2[2 * d.width + 2] == 1, "Sound should propagate diagonally if a corner is open");
}

void test_weighted_pathfinding_prefers_open_route_over_closed_door() {
    Dungeon d(7, 5);

    // Start with solid walls.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Wall;
        }
    }

    // Two routes from (1,2) -> (5,2):
    //  - Straight hallway with a CLOSED door in the middle.
    //  - A diagonal-capable "upper" hallway that avoids the door.
    for (int x = 1; x <= 5; ++x) {
        d.at(x, 2).type = TileType::Floor;
        d.at(x, 1).type = TileType::Floor;
    }
    d.at(3, 2).type = TileType::DoorClosed;

    const Vec2i start{1, 2};
    const Vec2i goal{5, 2};

    auto passable = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        const TileType t = d.at(x, y).type;
        return t != TileType::Wall;
    };

    auto stepCost = [&](int x, int y) -> int {
        const TileType t = d.at(x, y).type;
        if (t == TileType::DoorClosed) return 2;
        return 1;
    };

    auto diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return diagonalPassable(d, {fromX, fromY}, dx, dy);
    };

    const auto path = dijkstraPath(d.width, d.height, start, goal, passable, stepCost, diagOk);
    expect(!path.empty(), "Weighted Dijkstra path should find a route in a simple corridor");

    bool usesClosedDoor = false;
    for (const auto& p : path) {
        if (p.x == 3 && p.y == 2) usesClosedDoor = true;
    }
    expect(!usesClosedDoor,
           "Weighted Dijkstra should avoid a closed door when an equally short open route exists");

    // Cost-to-target sanity: optimal route is 4 floor entries.
    const auto cost = dijkstraCostToTarget(d.width, d.height, goal, passable, stepCost, diagOk);
    expect(cost[static_cast<size_t>(start.y * d.width + start.x)] == 4,
           "Cost-to-target map should reflect the 4-turn open route");
}

void test_item_defs_sane() {
    for (int k = 0; k < ITEM_KIND_COUNT; ++k) {
        const ItemKind kind = static_cast<ItemKind>(k);
        const ItemDef& def = itemDef(kind);

        expect(def.kind == kind, "ItemDef kind mismatch for kind " + std::to_string(k));
        expect(def.name != nullptr && std::string(def.name).size() > 0,
               "ItemDef name missing for kind " + std::to_string(k));

        // Design invariant: all consumables in ProcRogue are stackable.
        if (def.consumable) {
            expect(def.stackable, "Consumable item should be stackable (kind " + std::to_string(k) + ")");
        }

        expect(def.weight >= 0, "ItemDef weight should be non-negative (kind " + std::to_string(k) + ")");
    }
}

void test_item_weight_helpers() {
    // Stackable items scale with count.
    Item arrows;
    arrows.kind = ItemKind::Arrow;
    arrows.count = 25;
    expect(itemWeight(arrows) == itemDef(ItemKind::Arrow).weight * 25, "Arrow stack weight scales with count");

    // Non-stackable items use a single-item weight regardless of count.
    Item sword;
    sword.kind = ItemKind::Sword;
    sword.count = 99;
    expect(itemWeight(sword) == itemDef(ItemKind::Sword).weight, "Non-stackable items use single-item weight");

    // Gold is weightless by default.
    Item gold;
    gold.kind = ItemKind::Gold;
    gold.count = 500;
    expect(itemWeight(gold) == 0, "Gold should be weightless by default");

    std::vector<Item> v;
    v.push_back(arrows);
    v.push_back(sword);
    v.push_back(gold);
    expect(totalWeight(v) == itemWeight(arrows) + itemWeight(sword), "totalWeight sums itemWeight across a container");
}

void test_combat_dice_rules() {
    // Weapon dice table sanity.
    {
        const DiceExpr d = meleeDiceForWeapon(ItemKind::Dagger);
        expect(d.count == 1 && d.sides == 4 && d.bonus == 0, "Dagger base dice should be 1d4");
    }
    {
        const DiceExpr d = meleeDiceForWeapon(ItemKind::Sword);
        expect(d.count == 1 && d.sides == 6 && d.bonus == 0, "Sword base dice should be 1d6");
    }
    {
        const DiceExpr d = meleeDiceForWeapon(ItemKind::Axe);
        expect(d.count == 1 && d.sides == 8 && d.bonus == 0, "Axe base dice should be 1d8");
    }

    // Projectile dice table sanity.
    {
        const DiceExpr d = rangedDiceForProjectile(ProjectileKind::Arrow, false);
        expect(d.count == 1 && d.sides == 6, "Arrow base dice should be 1d6");
    }
    {
        const DiceExpr d = rangedDiceForProjectile(ProjectileKind::Rock, false);
        expect(d.count == 1 && d.sides == 4, "Rock base dice should be 1d4");
    }
    {
        const DiceExpr d = rangedDiceForProjectile(ProjectileKind::Spark, false);
        expect(d.count == 1 && d.sides == 6, "Spark base dice should be 1d6");
    }

    // Formatting.
    expect(diceToString({1, 6, 0}) == "1d6", "diceToString 1d6");
    expect(diceToString({2, 4, 2}) == "2d4+2", "diceToString 2d4+2");
    expect(diceToString({3, 8, -1}) == "3d8-1", "diceToString 3d8-1");

    // rollDice stays in expected bounds.
    RNG rng(123u);
    for (int i = 0; i < 200; ++i) {
        const int v = rollDice(rng, {2, 6, 3});
        expect(v >= 5 && v <= 15, "rollDice(2d6+3) out of bounds");
    }
}

void test_physics_knockback_fall_into_chasm_kills_monster() {
    Dungeon d(5, 5);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
        }
    }

    // A bottomless chasm directly behind the defender.
    d.at(3, 2).type = TileType::Chasm;

    RNG rng(123u);
    std::vector<Entity> ents;

    Entity attacker;
    attacker.id = 1;
    attacker.kind = EntityKind::Player;
    attacker.pos = {1, 2};
    attacker.hp = attacker.hpMax = 10;

    Entity defender;
    defender.id = 2;
    defender.kind = EntityKind::Goblin;
    defender.pos = {2, 2};
    defender.hp = defender.hpMax = 5;

    ents.push_back(attacker);
    ents.push_back(defender);

    KnockbackConfig cfg;
    cfg.distance = 1;
    cfg.power = 2;
    cfg.collisionMin = cfg.collisionMax = 1;

    KnockbackResult r = applyKnockback(d, ents, rng, attacker.id, defender.id, 1, 0, cfg);
    expect(r.stop == KnockbackStop::FellIntoChasm, "knockback into chasm should report FellIntoChasm");
    expect(ents[1].hp <= 0, "monster knocked into chasm should die");
}

void test_physics_knockback_slam_into_wall_deals_collision_damage() {
    Dungeon d(5, 5);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
        }
    }

    // Solid wall directly behind the defender.
    d.at(3, 2).type = TileType::Wall;

    RNG rng(1u);
    std::vector<Entity> ents;

    Entity attacker;
    attacker.id = 1;
    attacker.kind = EntityKind::Player;
    attacker.pos = {1, 2};
    attacker.hp = attacker.hpMax = 10;

    Entity defender;
    defender.id = 2;
    defender.kind = EntityKind::Orc;
    defender.pos = {2, 2};
    defender.hp = defender.hpMax = 10;

    ents.push_back(attacker);
    ents.push_back(defender);

    KnockbackConfig cfg;
    cfg.distance = 1;
    cfg.power = 1;
    cfg.collisionMin = cfg.collisionMax = 4; // deterministic

    KnockbackResult r = applyKnockback(d, ents, rng, attacker.id, defender.id, 1, 0, cfg);
    expect(r.stop == KnockbackStop::SlammedWall, "knockback into wall should report SlammedWall");
    expect(r.stepsMoved == 0, "defender should not move into a wall");
    expect(r.collisionDamageDefender == 4, "collision damage should match configured fixed amount");
    expect(ents[1].hp == 6, "defender HP should be reduced by collision damage");
}

void test_physics_knockback_slam_into_closed_door_when_smash_disabled() {
    Dungeon d(5, 5);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
        }
    }

    d.at(3, 2).type = TileType::DoorClosed;

    RNG rng(1u);
    std::vector<Entity> ents;

    Entity attacker;
    attacker.id = 1;
    attacker.kind = EntityKind::Player;
    attacker.pos = {1, 2};
    attacker.hp = attacker.hpMax = 10;

    Entity defender;
    defender.id = 2;
    defender.kind = EntityKind::Orc;
    defender.pos = {2, 2};
    defender.hp = defender.hpMax = 10;

    ents.push_back(attacker);
    ents.push_back(defender);

    KnockbackConfig cfg;
    cfg.distance = 1;
    cfg.power = 1;
    cfg.allowDoorSmash = false;
    cfg.collisionMin = cfg.collisionMax = 3; // deterministic

    KnockbackResult r = applyKnockback(d, ents, rng, attacker.id, defender.id, 1, 0, cfg);
    expect(r.stop == KnockbackStop::SlammedDoor, "knockback into closed door (smash disabled) should report SlammedDoor");
    expect(d.at(3, 2).type == TileType::DoorClosed, "door should remain closed when door-smash disabled");
    expect(ents[1].hp == 7, "defender HP should be reduced by deterministic collision damage");
}

void test_physics_knockback_hits_other_entity_damages_both() {
    Dungeon d(6, 5);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
        }
    }

    RNG rng(1u);
    std::vector<Entity> ents;

    Entity attacker;
    attacker.id = 1;
    attacker.kind = EntityKind::Player;
    attacker.pos = {1, 2};
    attacker.hp = attacker.hpMax = 10;

    Entity defender;
    defender.id = 2;
    defender.kind = EntityKind::Goblin;
    defender.pos = {2, 2};
    defender.hp = defender.hpMax = 10;

    Entity other;
    other.id = 3;
    other.kind = EntityKind::Orc;
    other.pos = {3, 2};
    other.hp = other.hpMax = 10;

    ents.push_back(attacker);
    ents.push_back(defender);
    ents.push_back(other);

    KnockbackConfig cfg;
    cfg.distance = 1;
    cfg.power = 1;
    cfg.collisionMin = cfg.collisionMax = 2; // deterministic

    KnockbackResult r = applyKnockback(d, ents, rng, attacker.id, defender.id, 1, 0, cfg);
    expect(r.stop == KnockbackStop::HitEntity, "knockback into another entity should report HitEntity");
    expect(r.otherEntityId == 3, "HitEntity should report the ID of the blocking entity");
    expect(ents[1].hp == 8, "defender should take collision damage");
    expect(ents[2].hp == 9, "other entity should take some collision spill damage");
}

void test_scores_legacy_load() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_legacy_test.csv";
#else
    const std::string path = "procrogue_scores_legacy_test.csv";
#endif

    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,won,score,depth,turns,kills,level,gold,seed\n";
        out << "2025-01-01T00:00:00Z,0,1234,3,100,5,2,10,42\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard legacy load failed");
#else
    expect(sb.load(path), "ScoreBoard legacy load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard legacy entry count");

    const auto& e = sb.entries().front();
    expect(e.score == 1234u, "Legacy score parsed");
    expect(e.depth == 3, "Legacy depth parsed");
    expect(e.turns == 100u, "Legacy turns parsed");
    expect(e.kills == 5u, "Legacy kills parsed");
    expect(e.level == 2, "Legacy level parsed");
    expect(e.gold == 10, "Legacy gold parsed");
    expect(e.seed == 42u, "Legacy seed parsed");
    expect(e.name.empty(), "Legacy name should be empty");
    expect(e.cause.empty(), "Legacy cause should be empty");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_scores_new_format_load_and_escape() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_newfmt_test.csv";
#else
    const std::string path = "procrogue_scores_newfmt_test.csv";
#endif

    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,name,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01T00:00:00Z,\"The, Name\",1,0,10,200,7,5,123,999,\"ESCAPED WITH \"\"THE\"\" AMULET\",0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard newfmt load failed");
#else
    expect(sb.load(path), "ScoreBoard newfmt load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard newfmt entry count");

    const auto& e = sb.entries().front();
    expect(e.won == true, "Newfmt won parsed");
    expect(e.name == "The, Name", "Newfmt name parsed/escaped");
    expect(e.cause == "ESCAPED WITH \"THE\" AMULET", "Newfmt cause parsed/escaped");
    expect(e.gameVersion == "0.8.0", "Newfmt version parsed");

    // Score was 0 in file; should have been recomputed.
    expect(e.score != 0u, "Newfmt score recomputed");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}




void test_scores_load_utf8_bom_header() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_bom_test.csv";
#else
    const std::string path = "procrogue_scores_bom_test.csv";
#endif

    {
        std::ofstream out(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        out << "\xEF\xBB\xBF" << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01 00:00:00,Tester,default,0,1234,3,100,5,2,10,42,CAUSE,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard BOM load failed");
#else
    expect(sb.load(path), "ScoreBoard BOM load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard BOM entry count");

    const auto& e = sb.entries().front();
    expect(e.name == "Tester", "ScoreBoard BOM name parsed");
    expect(e.slot == "default", "ScoreBoard BOM slot parsed");
    expect(e.cause == "CAUSE", "ScoreBoard BOM cause parsed");
    expect(e.gameVersion == "0.8.0", "ScoreBoard BOM version parsed");
    expect(e.score == 1234u, "ScoreBoard BOM score parsed");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}


void test_scores_quoted_whitespace_preserved() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_whitespace_test.csv";
#else
    const std::string path = "procrogue_scores_whitespace_test.csv";
#endif

    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,name,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01T00:00:00Z,   \"  Spaced Name  \"   ,0,0,1,0,0,1,0,1,   \"  CAUSE WITH SPACES  \"   ,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard whitespace load failed");
#else
    expect(sb.load(path), "ScoreBoard whitespace load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard whitespace entry count");

    const auto& e = sb.entries().front();
    expect(e.name == "  Spaced Name  ", "Quoted whitespace in name should be preserved");
    expect(e.cause == "  CAUSE WITH SPACES  ", "Quoted whitespace in cause should be preserved");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}
void test_scores_append_roundtrip() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_roundtrip_test.csv";
#else
    const std::string path = "procrogue_scores_roundtrip_test.csv";
#endif

    ScoreBoard sb;
    ScoreEntry e;
    e.timestamp = "2025-01-01T00:00:00Z";
    e.name = "Tester";
    e.slot = "run1";
    e.won = false;
    e.depth = 4;
    e.turns = 50;
    e.kills = 2;
    e.level = 3;
    e.gold = 10;
    e.seed = 77;
    e.cause = "KILLED BY GOBLIN";
    e.gameVersion = "0.8.0";

#if __has_include(<filesystem>)
    expect(sb.append(path.string(), e), "ScoreBoard append failed");
#else
    expect(sb.append(path, e), "ScoreBoard append failed");
#endif

    ScoreBoard sb2;
#if __has_include(<filesystem>)
    expect(sb2.load(path.string()), "ScoreBoard roundtrip load failed");
#else
    expect(sb2.load(path), "ScoreBoard roundtrip load failed");
#endif
    expect(sb2.entries().size() == 1, "ScoreBoard roundtrip entry count");

    const auto& r = sb2.entries().front();
    expect(r.name == "Tester", "Roundtrip name preserved");
    expect(r.cause == "KILLED BY GOBLIN", "Roundtrip cause preserved");
    expect(r.gameVersion == "0.8.0", "Roundtrip version preserved");
    expect(r.slot == "run1", "Roundtrip slot preserved");
    expect(r.seed == 77u, "Roundtrip seed preserved");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_scores_trim_keeps_recent_runs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_trim_recent_test.csv";
    std::error_code ec;
    fs::remove(path, ec);
#else
    const std::string path = "procrogue_scores_trim_recent_test.csv";
    std::remove(path.c_str());
#endif

    // Create a file with far more entries than we keep. Old behavior (trim by score only)
    // would discard low-scoring recent runs. New behavior keeps a mix: top scores + recent history.
    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";

        // 150 high-scoring, older runs (day 1)
        for (int i = 0; i < 150; ++i) {
            const int mm = i / 60;
            const int ss = i % 60;
            char ts[64];
            std::snprintf(ts, sizeof(ts), "2025-01-01 00:%02d:%02d", mm, ss);
            out << ts << ",High,default,0," << (1000000 - i) << ",10,100,10,5,0," << i << ",,0.8.0\n";
        }

        // 60 low-scoring, newer runs (day 2)
        for (int i = 0; i < 60; ++i) {
            char ts[64];
            std::snprintf(ts, sizeof(ts), "2025-01-02 00:00:%02d", i);
            out << ts << ",Low,default,0,1,1,1,0,1,0," << (1000 + i) << ",,0.8.0\n";
        }
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard trim/recent load failed");
#else
    expect(sb.load(path), "ScoreBoard trim/recent load failed");
#endif

    // We keep 60 top scores + 60 most recent runs.
    expect(sb.entries().size() == 120, "ScoreBoard should keep 120 entries (top+recent mix)");

    bool foundNewest = false;
    for (const auto& e : sb.entries()) {
        if (e.timestamp == "2025-01-02 00:00:59") {
            foundNewest = true;
            break;
        }
    }
    expect(foundNewest, "ScoreBoard trimming should keep the newest low-score run");

    // Also ensure the top score still survives.
    if (!sb.entries().empty()) {
        expect(sb.entries().front().score == 1000000u, "ScoreBoard should retain the top score");
    }

    // Trimming again to a smaller cap should still preserve some recent history.
    sb.trim(10);
    expect(sb.entries().size() == 10, "ScoreBoard should trim down to 10 entries");

    bool foundNewestAfterSmallTrim = false;
    for (const auto& e : sb.entries()) {
        if (e.timestamp == "2025-01-02 00:00:59") {
            foundNewestAfterSmallTrim = true;
            break;
        }
    }
    expect(foundNewestAfterSmallTrim, "ScoreBoard small trim should still keep the newest low-score run");

#if __has_include(<filesystem>)
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}

void test_scores_u32_parsing_rejects_negative_and_overflow() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_u32_parse_test.csv";
    std::error_code ec;
    fs::remove(path, ec);
#else
    const std::string path = "procrogue_scores_u32_parse_test.csv";
    std::remove(path.c_str());
#endif

    {
        std::ofstream out(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        out << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        // Negative/overflow values should be rejected rather than wrapped into huge uint32_t values.
        out << "2025-01-01 00:00:00,Tester,default,0,-1,3,-1,0,1,0,42949672960,CAUSE,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard u32 parse load failed");
#else
    expect(sb.load(path), "ScoreBoard u32 parse load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard u32 parse entry count");

    const auto& e = sb.entries().front();
    expect(e.turns == 0u, "Negative turns should be rejected (remain default 0)");
    expect(e.seed == 0u, "Overflow seed should be rejected (remain default 0)");
    expect(e.score != 0u, "Invalid/negative score should trigger recompute");
    expect(e.score < 1000000u, "Recomputed score should not be absurdly large");

#if __has_include(<filesystem>)
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}

void test_scores_sort_ties_by_timestamp() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_tie_sort_test.csv";
#else
    const std::string path = "procrogue_scores_tie_sort_test.csv";
#endif

    {
        std::ofstream out(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        out << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01 00:00:00,Tester,default,0,500,3,100,0,1,0,1,,0.8.0\n";
        out << "2025-01-02 00:00:00,Tester,default,0,500,3,100,0,1,0,1,,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard tie sort load failed");
#else
    expect(sb.load(path), "ScoreBoard tie sort load failed");
#endif
    expect(sb.entries().size() == 2, "ScoreBoard tie sort entry count");

    // With identical score/won/turns, newest timestamp should sort first.
    expect(sb.entries()[0].timestamp == "2025-01-02 00:00:00", "ScoreBoard tie sort should prefer newest timestamp");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_scores_append_creates_parent_dirs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "procrogue_scores_nested_dir_test";
    const fs::path file = root / "nested" / "scores.csv";

    std::error_code ec;
    fs::remove_all(root, ec);

    ScoreBoard sb;
    ScoreEntry e;
    e.timestamp = "2025-01-01 00:00:00";
    e.won = false;
    e.depth = 2;
    e.turns = 10;
    e.kills = 0;
    e.level = 1;
    e.gold = 0;
    e.seed = 123;

    expect(sb.append(file.string(), e), "ScoreBoard append should create missing parent directories");
    expect(fs::exists(file), "ScoreBoard append should create the scores file");

    fs::remove_all(root, ec);
#else
    // No filesystem support: skip.
    expect(true, "Skipping directory-creation test (no <filesystem>)");
#endif
}

void test_settings_updateIniKey_creates_parent_dirs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "procrogue_settings_nested_dir_test";
    const fs::path file = root / "nested" / "settings.ini";

    std::error_code ec;
    fs::remove_all(root, ec);

    expect(updateIniKey(file.string(), "save_backups", "4"), "updateIniKey should create missing parent directories");
    Settings s = loadSettings(file.string());
    expect(s.saveBackups == 4, "updateIniKey should write settings in newly created dirs");

    fs::remove_all(root, ec);
#else
    // No filesystem support: skip.
    expect(true, "Skipping directory-creation test (no <filesystem>)");
#endif
}


void test_settings_writeDefaultSettings_creates_parent_dirs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "procrogue_settings_default_nested_dir_test";
    const fs::path file = root / "nested" / "settings.ini";

    std::error_code ec;
    fs::remove_all(root, ec);

    expect(writeDefaultSettings(file.string()), "writeDefaultSettings should create missing parent directories");
    expect(fs::exists(file), "writeDefaultSettings should create the settings file");

    Settings s = loadSettings(file.string());
    Settings defaults;
    expect(s.tileSize == defaults.tileSize, "writeDefaultSettings should write defaults (tile_size)");
    expect(s.playerName == "PLAYER", "writeDefaultSettings should write defaults (player_name=PLAYER)");

    fs::remove_all(root, ec);
#else
    // No filesystem support: skip.
    expect(true, "Skipping writeDefaultSettings directory-creation test (no <filesystem>)");
#endif
}





void test_settings_load_utf8_bom() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_bom_test.ini";
#else
    const std::string path = "procrogue_settings_bom_test.ini";
#endif

    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "\xEF\xBB\xBF" << "save_backups = 4\n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.saveBackups == 4, "Settings BOM should parse save_backups");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}


void test_settings_save_backups_parse() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_save_backups_test.ini";
#else
    const std::string path = "procrogue_settings_save_backups_test.ini";
#endif

    // Basic parse
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "# Test settings\n";
        f << "save_backups = 5\n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.saveBackups == 5, "save_backups should parse to 5");

    // Clamp low
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "save_backups = -1\n";
    }
#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 0, "save_backups should clamp to 0");

    // Clamp high
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "save_backups = 999\n";
    }
#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 10, "save_backups should clamp to 10");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_settings_autopickup_smart_parse() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_autopickup_smart_test.ini";
#else
    const std::string path = "procrogue_settings_autopickup_smart_test.ini";
#endif

    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "auto_pickup = smart\n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.autoPickup == AutoPickupMode::Smart, "auto_pickup=smart should parse to Smart");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}


void test_settings_default_slot_parse() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_default_slot_test.ini";
#else
    const std::string path = "procrogue_settings_default_slot_test.ini";
#endif

    // Basic parse + sanitize
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "default_slot =  My Run 01  \n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.defaultSlot == "my_run_01", "default_slot should sanitize spaces and case");

    // "default" should clear it
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "default_slot = default\n";
    }

#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.defaultSlot.empty(), "default_slot=default should clear to empty");

    // Windows reserved base names should be prefixed
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "default_slot = con\n";
    }

#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.defaultSlot == "_con", "default_slot should avoid Windows reserved basenames");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}




void test_settings_ini_helpers_create_update_remove() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_ini_helpers_test.ini";
    std::error_code ec;
    fs::remove(path, ec);
#else
    const std::string path = "procrogue_settings_ini_helpers_test.ini";
    std::remove(path.c_str());
#endif

    // updateIniKey should create the file if it doesn't exist.
#if __has_include(<filesystem>)
    expect(updateIniKey(path.string(), "save_backups", "7"), "updateIniKey should create file when missing");
    Settings s = loadSettings(path.string());
#else
    expect(updateIniKey(path, "save_backups", "7"), "updateIniKey should create file when missing");
    Settings s = loadSettings(path);
#endif
    expect(s.saveBackups == 7, "updateIniKey created save_backups=7");

    // updateIniKey should update an existing key.
#if __has_include(<filesystem>)
    expect(updateIniKey(path.string(), "save_backups", "2"), "updateIniKey should update an existing key");
    s = loadSettings(path.string());
#else
    expect(updateIniKey(path, "save_backups", "2"), "updateIniKey should update an existing key");
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 2, "updateIniKey updated save_backups=2");

    // updateIniKey should deduplicate multiple entries for the same key.
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
            , std::ios::app);
        f << "\n# duplicate\n";
        f << "save_backups = 9\n";
    }

#if __has_include(<filesystem>)
    expect(updateIniKey(path.string(), "save_backups", "4"), "updateIniKey should handle duplicate keys");
    s = loadSettings(path.string());
#else
    expect(updateIniKey(path, "save_backups", "4"), "updateIniKey should handle duplicate keys");
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 4, "updateIniKey should set save_backups=4 even with duplicates");

    auto trimKey = [](std::string s) {
        size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        size_t b = s.size();
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };
    auto lowerKey = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    int occurrences = 0;
    {
        std::ifstream in(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        std::string line;
        while (std::getline(in, line)) {
            std::string raw = line;
            auto commentPos = raw.find_first_of("#;");
            if (commentPos != std::string::npos) raw = raw.substr(0, commentPos);

            auto eq = raw.find('=');
            if (eq == std::string::npos) continue;

            std::string k = lowerKey(trimKey(raw.substr(0, eq)));
            if (k == "save_backups") ++occurrences;
        }
    }
    expect(occurrences == 1, "updateIniKey should remove duplicate save_backups entries");

    // removeIniKey should succeed if the file doesn't exist.
#if __has_include(<filesystem>)
    const fs::path missing = fs::temp_directory_path() / "procrogue_settings_ini_helpers_missing.ini";
    fs::remove(missing, ec);
    expect(removeIniKey(missing.string(), "save_backups"), "removeIniKey should succeed when file is missing");
#else
    const std::string missing = "procrogue_settings_ini_helpers_missing.ini";
    std::remove(missing.c_str());
    expect(removeIniKey(missing, "save_backups"), "removeIniKey should succeed when file is missing");
#endif

    // removeIniKey should remove an existing key (defaults should apply again).
#if __has_include(<filesystem>)
    expect(removeIniKey(path.string(), "save_backups"), "removeIniKey should remove an existing key");
    s = loadSettings(path.string());
#else
    expect(removeIniKey(path, "save_backups"), "removeIniKey should remove an existing key");
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 3, "removeIniKey removed save_backups (defaults restored)");

#if __has_include(<filesystem>)
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}

void test_sanitize_slot_name() {
    expect(sanitizeSlotName("  My Slot  ") == "my_slot", "sanitizeSlotName should trim/lower and replace spaces");
    expect(sanitizeSlotName("../../evil") == "evil", "sanitizeSlotName should strip path-like characters");
    expect(sanitizeSlotName("COM1") == "_com1", "sanitizeSlotName should guard Windows reserved basenames");
    expect(sanitizeSlotName("   ---___   ") == "slot", "sanitizeSlotName should fall back to 'slot' on empty");

    std::string longName(100, 'a');
    const std::string capped = sanitizeSlotName(longName);
    expect(capped.size() == 32, "sanitizeSlotName should cap to 32 characters");
}

void test_message_dedup_consecutive() {
    Game g;
    g.newGame(123u);

    const size_t base = g.messages().size();

    g.pushSystemMessage("HELLO");
    expect(g.messages().size() == base + 1, "pushSystemMessage should append a message");
    expect(g.messages().back().text == "HELLO", "message text should match");
    expect(g.messages().back().repeat == 1, "new message should start with repeat=1");

    // Same message, consecutive: should merge.
    g.pushSystemMessage("HELLO");
    expect(g.messages().size() == base + 1, "consecutive duplicate messages should be merged");
    expect(g.messages().back().repeat == 2, "merged message should increment repeat count");

    // Different message: should append.
    g.pushSystemMessage("WORLD");
    expect(g.messages().size() == base + 2, "different message should append a new entry");

    // Non-consecutive duplicate: should append.
    g.pushSystemMessage("HELLO");
    expect(g.messages().size() == base + 3, "non-consecutive duplicates should not be merged");

    // Scroll interaction: when scrolled up, new messages should increase scroll offset;
    // merged duplicates should NOT.
    // Ensure there are enough messages to scroll.
    for (int i = 0; i < 10; ++i) g.pushSystemMessage("MSG " + std::to_string(i));
    g.handleAction(Action::LogUp);
    g.handleAction(Action::LogUp);
    const int scrollBefore = g.messageScroll();
    expect(scrollBefore > 0, "log should be scrolled up for this test");

    g.pushSystemMessage("SCROLLTEST");
    const int scrollAfterNew = g.messageScroll();
    expect(scrollAfterNew == scrollBefore + 1, "new message should increase msgScroll when scrolled up");

    g.pushSystemMessage("SCROLLTEST");
    const int scrollAfterDup = g.messageScroll();
    expect(scrollAfterDup == scrollAfterNew, "merged duplicate should not change msgScroll");
    expect(g.messages().back().repeat >= 2, "merged duplicate should increase repeat count (scrolled case)");
}
} // namespace


void test_fov_mask_matches_compute_fov() {
    RNG rng(123u);
    Dungeon d(15, 9);
    d.generate(rng, /*depth=*/1, /*maxDepth=*/10);

    const int cx = 3;
    const int cy = 3;
    const int radius = 8;

    std::vector<uint8_t> mask;
    d.computeFovMask(cx, cy, radius, mask);

    // computeFov (no exploring) and compare the visible flags to the mask.
    d.computeFov(cx, cy, radius, false);

    expect(static_cast<int>(mask.size()) == d.width * d.height, "mask size should match dungeon size");
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const size_t i = static_cast<size_t>(y * d.width + x);
            expect((mask[i] != 0) == d.at(x, y).visible, "mask visibility should match computeFov visibility");
        }
    }
}

void test_fov_mark_explored_flag() {
    RNG rng(123u);
    Dungeon d(15, 9);
    d.generate(rng, /*depth=*/1, /*maxDepth=*/10);

    // Clear explored flags.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).explored = false;
        }
    }

    d.computeFov(3, 3, 8, false);
    bool anyExplored = false;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            anyExplored = anyExplored || d.at(x, y).explored;
        }
    }
    expect(anyExplored == false, "markExplored=false should not set explored tiles");

    d.computeFov(3, 3, 8, true);
    anyExplored = false;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            anyExplored = anyExplored || d.at(x, y).explored;
        }
    }
    expect(anyExplored == true, "markExplored=true should set explored tiles");
}



void test_dungeon_digging() {
    Dungeon d(5, 5);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            d.at(x, y).type = TileType::Wall;
        }
    }

    expect(d.isDiggable(2, 2), "Wall should be diggable");
    expect(d.dig(2, 2), "Digging a wall should succeed");
    expect(d.at(2, 2).type == TileType::Floor, "Dig should convert wall to floor");
    expect(d.isWalkable(2, 2), "Dug tile should become walkable");
    expect(!d.isOpaque(2, 2), "Dug tile should no longer be opaque");

    d.at(1, 1).type = TileType::DoorLocked;
    expect(d.isDiggable(1, 1), "Locked door should be diggable");
    expect(d.dig(1, 1), "Digging a locked door should succeed");
    expect(d.at(1, 1).type == TileType::Floor, "Dig should destroy door into floor");
}

void test_wand_display_shows_charges() {
    Item it;
    it.kind = ItemKind::WandDigging;
    it.count = 1;
    it.charges = 3;

    const std::string name = itemDisplayName(it);
    expect(name.find("(3/8)") != std::string::npos, "Wand of digging should show charges in display name");
}

void test_monster_energy_scheduling_basic() {
    // Basic sanity checks for the monster speed/energy scheduler.
    // Fast monsters should sometimes take 2 actions per player turn; slow monsters should sometimes skip.

    constexpr int ENERGY_PER_ACTION = 100;
    constexpr int MAX_ACTIONS_PER_TURN = 3;

    // Bat is fast.
    {
        Entity bat;
        bat.kind = EntityKind::Bat;
        bat.speed = baseSpeedFor(bat.kind);
        bat.energy = 0;

        // Turn 1: 150 energy -> 1 action (50 remaining)
        bat.energy += clampi(bat.speed, 10, 200);
        int a1 = 0;
        while (bat.energy >= ENERGY_PER_ACTION && a1 < MAX_ACTIONS_PER_TURN) {
            bat.energy -= ENERGY_PER_ACTION;
            ++a1;
        }
        expect(a1 == 1, "Fast monsters should act at least once per turn");

        // Turn 2: 50 + 150 = 200 energy -> 2 actions
        bat.energy += clampi(bat.speed, 10, 200);
        int a2 = 0;
        while (bat.energy >= ENERGY_PER_ACTION && a2 < MAX_ACTIONS_PER_TURN) {
            bat.energy -= ENERGY_PER_ACTION;
            ++a2;
        }
        expect(a2 == 2, "Fast monsters should sometimes act twice per turn");
    }

    // Slime is slow.
    {
        Entity slime;
        slime.kind = EntityKind::Slime;
        slime.speed = baseSpeedFor(slime.kind);
        slime.energy = 0;

        // Turn 1: 70 energy -> 0 actions
        slime.energy += clampi(slime.speed, 10, 200);
        int a1 = 0;
        while (slime.energy >= ENERGY_PER_ACTION && a1 < MAX_ACTIONS_PER_TURN) {
            slime.energy -= ENERGY_PER_ACTION;
            ++a1;
        }
        expect(a1 == 0, "Slow monsters should sometimes skip turns");

        // Turn 2: 70 + 70 = 140 energy -> 1 action
        slime.energy += clampi(slime.speed, 10, 200);
        int a2 = 0;
        while (slime.energy >= ENERGY_PER_ACTION && a2 < MAX_ACTIONS_PER_TURN) {
            slime.energy -= ENERGY_PER_ACTION;
            ++a2;
        }
        expect(a2 == 1, "Slow monsters should still eventually act");
    }
}

void test_replay_roundtrip_basic() {
    // Basic sanity: write a replay file, read it back, verify meta + events.
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "procrogue_replay_test.prr";

    {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }

    ReplayMeta meta;
    meta.gameVersion = "unit_test";
    meta.seed = 123456u;
    meta.playerClassId = "adventurer";
    meta.autoStepDelayMs = 70;
    meta.autoExploreSearch = true;
    meta.autoPickup = AutoPickupMode::Smart;
    meta.identifyItems = true;
    meta.hungerEnabled = true;
    meta.encumbranceEnabled = false;
    meta.lightingEnabled = true;
    meta.yendorDoomEnabled = false;
    meta.bonesEnabled = false;

    {
        ReplayWriter w;
        std::string err;
        expect(w.open(p, meta, &err), "ReplayWriter.open should succeed");

        w.writeAction(0, Action::Left);
        w.writeStateHash(1, 0, 0x0123456789abcdefULL);
        w.writeAction(5, Action::Rest);
        w.writeStateHash(6, 1, 0xfedcba9876543210ULL);
        w.writeTextInput(10, "hello world");
        w.writeCommandBackspace(15);
        w.writeCommandAutocomplete(20);
        w.writeHistoryToggleSearch(25);
        w.writeHistoryClearSearch(30);
        w.writeHistoryBackspace(35);
        w.writeAutoTravel(40, Vec2i{12, 34});
        w.writeBeginLook(45, Vec2i{9, 8});
        w.writeTargetCursor(50, Vec2i{1, 2});
        w.writeLookCursor(55, Vec2i{3, 4});

        w.close();
    }

    ReplayFile rf;
    {
        std::string err;
        expect(loadReplayFile(p, rf, &err), "loadReplayFile should succeed");
    }

    expect(rf.meta.seed == meta.seed, "Replay meta seed should roundtrip");
    expect(rf.meta.playerClassId == meta.playerClassId, "Replay meta class should roundtrip");
    expect(rf.meta.autoStepDelayMs == meta.autoStepDelayMs, "Replay meta autoStepDelayMs should roundtrip");
    expect(rf.meta.autoExploreSearch == meta.autoExploreSearch, "Replay meta autoExploreSearch should roundtrip");
    expect(rf.meta.autoPickup == meta.autoPickup, "Replay meta autoPickup should roundtrip");
    expect(rf.events.size() == size_t(14), "Replay should load expected number of events");

    expect(rf.events[0].kind == ReplayEventType::Action && rf.events[0].action == Action::Left, "Event 0 should be Action::Left");
    expect(rf.events[1].kind == ReplayEventType::StateHash && rf.events[1].turn == 0u && rf.events[1].hash == 0x0123456789abcdefULL, "Event 1 should be the initial state hash");
    expect(rf.events[3].kind == ReplayEventType::StateHash && rf.events[3].turn == 1u && rf.events[3].hash == 0xfedcba9876543210ULL, "Event 3 should be the next state hash");
    expect(rf.events[4].kind == ReplayEventType::TextInput && rf.events[4].text == "hello world", "Event 4 should be text input");
    expect(rf.events[10].kind == ReplayEventType::AutoTravel && rf.events[10].pos == Vec2i{12, 34}, "AutoTravel event should roundtrip pos");

    {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
}

void test_headless_replay_runner_verifies_hashes() {
    // Construct a tiny replay in-memory, where hashes are generated from an initial run,
    // then verify the headless runner can reproduce it exactly.
    ReplayFile rf;

    rf.meta.gameVersion = PROCROGUE_VERSION;
    rf.meta.seed = 123456u;
    rf.meta.playerClassId = "adventurer";
    rf.meta.autoPickup = AutoPickupMode::Off;
    rf.meta.autoStepDelayMs = 45;
    rf.meta.autoExploreSearch = false;
    rf.meta.identifyItems = true;
    rf.meta.hungerEnabled = false;
    rf.meta.encumbranceEnabled = false;
    rf.meta.lightingEnabled = false;
    rf.meta.yendorDoomEnabled = true;
    rf.meta.bonesEnabled = false;

    // First run: generate expected hashes by actually simulating the actions.
    {
        Game g;
        std::string err;
        expect(prepareGameForReplay(g, rf, &err), "prepareGameForReplay should succeed (baseline run)");

        uint32_t t = 0;

        // Turn 0 checkpoint.
        ReplayEvent h0;
        h0.tMs = t;
        h0.kind = ReplayEventType::StateHash;
        h0.turn = g.turns();
        h0.hash = g.determinismHash();
        rf.events.push_back(h0);

        const int kSteps = 12;
        for (int i = 0; i < kSteps; ++i) {
            t += 10;

            ReplayEvent a;
            a.tMs = t;
            a.kind = ReplayEventType::Action;
            a.action = Action::Wait;
            rf.events.push_back(a);

            g.handleAction(Action::Wait);

            ReplayEvent h;
            h.tMs = t;
            h.kind = ReplayEventType::StateHash;
            h.turn = g.turns();
            h.hash = g.determinismHash();
            rf.events.push_back(h);
        }
    }

    // Second run: verify via the headless replay runner.
    {
        Game g;
        std::string err;
        expect(prepareGameForReplay(g, rf, &err), "prepareGameForReplay should succeed (verify run)");

        ReplayRunOptions opt;
        opt.frameMs = 16;
        opt.verifyHashes = true;
        opt.maxSimMs = 20000;
        opt.maxFrames = 0;

        ReplayRunStats stats;
        const bool ok = runReplayHeadless(g, rf, opt, &stats, &err);
        expect(ok, std::string("runReplayHeadless should succeed: ") + err);
        expect(stats.turns >= 12u, "runReplayHeadless should advance turns");
    }
}



void test_content_overrides_basic() {
#if __has_include(<filesystem>)
    std::filesystem::path p = std::filesystem::temp_directory_path() / "procrogue_test_content.ini";
    {
        std::ofstream f(p);
        expect(bool(f), "Should open temp content ini for writing");

        f << "# ProcRogue test content overrides\n";
        f << "monster.goblin.hp_max = 42\n";
        f << "monster.goblin.base_atk = 7\n";
        f << "monster.goblin.base_def = 5\n";
        f << "item.dagger.melee_atk = 99\n";
        f << "spawn.room.1.bat = 1\n";
        f << "spawn.room.1.goblin = 0\n";
        f << "spawn.room.1.orc = 0\n";
        f << "spawn.guardian.1.goblin = 0\n";
        f << "spawn.guardian.1.orc = 0\n";
        f << "spawn.guardian.1.bat = 1\n";
    }

    ContentOverrides co;
    std::string warns;
    const bool ok = loadContentOverridesIni(p.string(), co, &warns);
    expect(ok, "loadContentOverridesIni should succeed for valid file");
    setContentOverrides(co);

    {
        const MonsterBaseStats g = baseMonsterStatsFor(EntityKind::Goblin);
        expect(g.hpMax == 42, "Monster override should change goblin.hpMax");
        expect(g.baseAtk == 7, "Monster override should change goblin.baseAtk");
        expect(g.baseDef == 5, "Monster override should change goblin.baseDef");
    }

    {
        const ItemDef& d = itemDef(ItemKind::Dagger);
        expect(d.meleeAtk == 99, "Item override should change dagger.meleeAtk");
    }

    {
        RNG rng(123u);
        for (int i = 0; i < 10; ++i) {
            const EntityKind k = pickSpawnMonster(SpawnCategory::Room, rng, 1);
            expect(k == EntityKind::Bat, "Spawn override should force bat-only spawns on room depth 1");
        }
    }

    {
        RNG rng(123u);
        for (int i = 0; i < 10; ++i) {
            const EntityKind k = pickSpawnMonster(SpawnCategory::Guardian, rng, 1);
            expect(k == EntityKind::Bat, "Spawn override should force bat-only spawns on guardian depth 1");
        }
    }

    clearContentOverrides();

    {
        const ItemDef& d = itemDef(ItemKind::Dagger);
        expect(d.meleeAtk != 99, "Clearing content overrides should restore default dagger.meleeAtk");
    }

    {
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
#else
    (void)0;
#endif
}


int main() {
    std::cout << "Running ProcRogue tests...\n";

    test_rng_reproducible();
    test_dungeon_stairs_connected();
    test_final_floor_sanctum_layout();
    test_secret_door_tile_rules();
    test_chasm_and_pillar_tile_rules();
    test_locked_door_tile_rules();
    test_close_door_tile_rules();
    test_lock_door_tile_rules();
    test_fov_locked_door_blocks_visibility();
    test_los_blocks_diagonal_corner_peek();
    test_sound_propagation_respects_walls_and_muffling_doors();
    test_sound_diagonal_corner_cutting_is_blocked();
    test_weighted_pathfinding_prefers_open_route_over_closed_door();
    test_item_defs_sane();
    test_item_weight_helpers();
    test_combat_dice_rules();

    test_physics_knockback_fall_into_chasm_kills_monster();
    test_physics_knockback_slam_into_wall_deals_collision_damage();
    test_physics_knockback_slam_into_closed_door_when_smash_disabled();
    test_physics_knockback_hits_other_entity_damages_both();

    test_scores_legacy_load();
    test_scores_new_format_load_and_escape();
    test_scores_load_utf8_bom_header();
    test_scores_quoted_whitespace_preserved();
    test_scores_append_roundtrip();
    test_scores_trim_keeps_recent_runs();
    test_scores_u32_parsing_rejects_negative_and_overflow();
    test_scores_sort_ties_by_timestamp();
    test_scores_append_creates_parent_dirs();
    test_settings_updateIniKey_creates_parent_dirs();
    test_settings_writeDefaultSettings_creates_parent_dirs();

    test_settings_load_utf8_bom();

    test_settings_save_backups_parse();
    test_settings_autopickup_smart_parse();
    test_settings_default_slot_parse();
    test_settings_ini_helpers_create_update_remove();
    test_sanitize_slot_name();
    test_message_dedup_consecutive();

    test_fov_mask_matches_compute_fov();
    test_fov_mark_explored_flag();

    // Patch-specific regression tests.
    test_dungeon_digging();
    test_wand_display_shows_charges();
    test_monster_energy_scheduling_basic();
    test_replay_roundtrip_basic();
    test_headless_replay_runner_verifies_hashes();
    test_content_overrides_basic();

    if (failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}