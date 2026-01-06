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
#include "game.hpp"

#include <cstdint>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <fstream>
#if __has_include(<filesystem>)
#include <filesystem>
#endif
#include <iostream>
#include <queue>
#include <string>
#include <vector>

struct GameTestAccess {
    static std::vector<Item>& inv(Game& g) { return g.inv; }
    static int& invSel(Game& g) { return g.invSel; }
    static int& depth(Game& g) { return g.depth_; }
    static std::array<int, Game::DUNGEON_MAX_DEPTH + 1>& shopLedger(Game& g) { return g.shopDebtLedger_; }
    static std::vector<Entity>& ents(Game& g) { return g.ents; }
    static const Entity* entityAt(Game& g, int x, int y) { return g.entityAt(x, y); }
    static std::vector<Trap>& traps(Game& g) { return g.trapsCur; }
    static std::vector<GroundItem>& ground(Game& g) { return g.ground; }
    static void dropGroundItem(Game& g, Vec2i pos, ItemKind k, int count = 1, int enchant = 0) { g.dropGroundItem(pos, k, count, enchant); }
    static int playerRangedRange(Game& g) { return g.playerRangedRange(); }
    static RNG& rng(Game& g) { return g.rng; }
    static bool equipSelected(Game& g) { return GameTestAccess::equipSelected(g); }
    static void triggerTrapAt(Game& g, Vec2i pos, Entity& victim, bool fromDisarm = false) { g.triggerTrapAt(pos, victim, fromDisarm); }
    static void cleanupDead(Game& g) { g.cleanupDead(); }
    static void monsterTurn(Game& g) { g.monsterTurn(); }
    static void changeLevel(Game& g, int newDepth, bool goingDown) { g.changeLevel(newDepth, goingDown); }
    static void spawnTraps(Game& g) { g.spawnTraps(); }
    static void recomputeFov(Game& g) { g.recomputeFov(); }
    static bool useSelected(Game& g) { return g.useSelected(); }
    static Dungeon& dungeon(Game& g) { return g.dung; }
    static std::vector<uint8_t>& fireField(Game& g) { return g.fireField_; }
    static std::vector<uint8_t>& confusionGas(Game& g) { return g.confusionGas_; }
    static std::vector<uint8_t>& poisonGas(Game& g) { return g.poisonGas_; }
    static void markIdentified(Game& g, ItemKind k, bool quiet = false) { g.markIdentified(k, quiet); }
    static bool stepAutoMove(Game& g) { return g.stepAutoMove(); }
    static Vec2i findNearestExploreFrontier(const Game& g) { return g.findNearestExploreFrontier(); }
};

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
    const int depthsToTest[] = {0, 1, 2, 3, 4, 5, 7, 8};

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

        // Grotto guarantee: the cavern floor at depth 4 should feature a subterranean lake.
        if (depth == Dungeon::GROTTO_DEPTH) {
            expect(d.hasCavernLake, "Grotto floor should generate a lake feature");
        }

        // Catacombs guarantee: depth 8 should generate a dense room-grid with lots of doors.
        if (depth == Dungeon::CATACOMBS_DEPTH) {
            expect(d.rooms.size() >= 4, "Catacombs should generate multiple rooms");

            int doors = 0;
            for (int y = 0; y < d.height; ++y) {
                for (int x = 0; x < d.width; ++x) {
                    const TileType tt = d.at(x, y).type;
                    if (tt == TileType::DoorClosed || tt == TileType::DoorOpen) doors++;
                }
            }
            expect(doors >= 8, "Catacombs should contain many doors (expected at least 8)");
        }

        // Deep Mines guarantee: the global ravine/fissure feature should create a noticeable
        // amount of chasm terrain, without breaking stairs connectivity.
        if (depth == Dungeon::DEEP_MINES_DEPTH) {
            int chasms = 0;
            for (int y = 0; y < d.height; ++y) {
                for (int x = 0; x < d.width; ++x) {
                    if (d.at(x, y).type == TileType::Chasm) chasms++;
                }
            }
            expect(chasms >= 18, "Deep Mines should feature a major fissure (expected many chasm tiles)");
        }
    }
}



void test_vault_room_prefabs_add_obstacles() {
    // Vaults are optional, so search across many seeds and confirm that at least one vault
    // uses a non-trivial interior layout (chasm/pillars/boulder).
    bool foundVault = false;
    bool foundDecoratedVault = false;

    for (uint32_t seed = 1; seed <= 250; ++seed) {
        RNG rng(seed);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/5, /*maxDepth=*/10);

        for (const Room& r : d.rooms) {
            if (r.type != RoomType::Vault) continue;
            foundVault = true;

            int special = 0;
            for (int y = r.y; y < r.y2(); ++y) {
                for (int x = r.x; x < r.x2(); ++x) {
                    if (!d.inBounds(x, y)) continue;
                    const TileType t = d.at(x, y).type;
                    if (t == TileType::Chasm || t == TileType::Pillar || t == TileType::Boulder) {
                        special++;
                    }
                }
            }

            if (special > 0) {
                foundDecoratedVault = true;
                break;
            }
        }

        if (foundDecoratedVault) break;
    }

    expect(foundVault, "Expected at least one vault room across seeds (depth 5)");
    expect(foundDecoratedVault, "Expected at least one decorated vault (chasm/pillar/boulder) across seeds");
}


void test_vault_suite_prefab_partitions_room() {
    // The "vault suite" prefab should occasionally produce interior walls + interior doors
    // inside a vault room (beyond the locked entrance).
    bool foundVault = false;
    bool foundSuite = false;

    for (uint32_t seed = 1; seed <= 400; ++seed) {
        RNG rng(1337u + seed * 101u);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/5, /*maxDepth=*/10);

        for (const Room& r : d.rooms) {
            if (r.type != RoomType::Vault) continue;
            foundVault = true;

            int interiorWalls = 0;
            int interiorDoors = 0;

            for (int y = r.y + 1; y < r.y2() - 1; ++y) {
                for (int x = r.x + 1; x < r.x2() - 1; ++x) {
                    if (!d.inBounds(x, y)) continue;
                    const TileType t = d.at(x, y).type;
                    if (t == TileType::Wall) interiorWalls++;
                    if (t == TileType::DoorClosed) interiorDoors++;
                }
            }

            // A partition wall spans most of the room height/width, so we expect multiple wall tiles.
            if (interiorWalls >= 6 && interiorDoors >= 1) {
                foundSuite = true;
                break;
            }
        }

        if (foundSuite) break;
    }

    expect(foundVault, "Expected at least one vault room across seeds (depth 5)");
    expect(foundSuite, "Expected to find at least one multi-chamber vault suite (internal walls + doors) across seeds");
}


void test_partition_vaults_embed_locked_door_in_wall_line() {
    // Partition-vaults are carved out of an existing normal room using a wall line.
    // The locked entrance door should sit *in* that wall line, meaning it has wall
    // neighbors on one axis (N/S or E/W).
    bool foundPartitionVault = false;

    for (uint32_t seed = 1; seed <= 300; ++seed) {
        RNG rng(4242u + seed * 31u);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/5, /*maxDepth=*/10);

        for (const Room& r : d.rooms) {
            if (r.type != RoomType::Vault) continue;

            for (int y = r.y; y < r.y2(); ++y) {
                for (int x = r.x; x < r.x2(); ++x) {
                    if (!d.inBounds(x, y)) continue;
                    if (d.at(x, y).type != TileType::DoorLocked) continue;

                    const bool wallNS = d.inBounds(x, y - 1) && d.inBounds(x, y + 1)
                        && d.at(x, y - 1).type == TileType::Wall
                        && d.at(x, y + 1).type == TileType::Wall;

                    const bool wallEW = d.inBounds(x - 1, y) && d.inBounds(x + 1, y)
                        && d.at(x - 1, y).type == TileType::Wall
                        && d.at(x + 1, y).type == TileType::Wall;

                    if (wallNS || wallEW) {
                        foundPartitionVault = true;
                        break;
                    }
                }
                if (foundPartitionVault) break;
            }

            if (foundPartitionVault) break;
        }

        if (foundPartitionVault) break;
    }

    expect(foundPartitionVault, "Expected to find at least one partition-style vault entrance (locked door embedded in a wall line) across seeds");
}




void test_themed_room_prefabs_add_obstacles() {
    // Themed rooms are optional. Scan across many seeds and confirm that at least
    // one Armory/Library/Laboratory receives an interior prefab (pillars/boulders/chasms).
    bool foundThemed = false;
    bool foundDecorated = false;

    for (uint32_t seed = 1; seed <= 250; ++seed) {
        RNG rng(9001u + seed * 17u);
        Dungeon d(60, 40);
        // Depth 5 is a good stress point: it supports themed rooms, optional secret/vault rooms,
        // and runs the full decoration pipeline.
        d.generate(rng, /*depth=*/5, /*maxDepth=*/10);

        for (const Room& r : d.rooms) {
            if (r.type != RoomType::Armory && r.type != RoomType::Library && r.type != RoomType::Laboratory) continue;
            foundThemed = true;

            int special = 0;
            for (int y = r.y; y < r.y2(); ++y) {
                for (int x = r.x; x < r.x2(); ++x) {
                    if (!d.inBounds(x, y)) continue;
                    const TileType t = d.at(x, y).type;
                    if (t == TileType::Chasm || t == TileType::Pillar || t == TileType::Boulder) {
                        special++;
                    }
                }
            }

            if (special > 0) {
                foundDecorated = true;
                break;
            }
        }

        if (foundDecorated) break;
    }

    expect(foundThemed, "Expected at least one themed room across seeds (depth 5)");
    expect(foundDecorated, "Expected at least one decorated themed room (pillars/boulders/chasms) across seeds");
}


void test_room_shape_variety_adds_internal_walls() {
    // Room-shaping is semi-random. Scan across seeds and confirm that at least one
    // normal room gains an interior wall partition (non-rectangular room topology).
    bool foundCandidate = false;
    bool foundShaped = false;

    for (uint32_t seed = 1; seed <= 200; ++seed) {
        RNG rng(4242u + seed * 19u);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/1, /*maxDepth=*/10);

        for (const Room& r : d.rooms) {
            if (r.type != RoomType::Normal) continue;
            if (r.w < 8 || r.h < 8) continue;
            foundCandidate = true;

            bool hasInteriorWall = false;
            for (int y = r.y + 1; y < r.y2() - 1 && !hasInteriorWall; ++y) {
                for (int x = r.x + 1; x < r.x2() - 1; ++x) {
                    if (!d.inBounds(x, y)) continue;
                    if (d.at(x, y).type == TileType::Wall) { hasInteriorWall = true; break; }
                }
            }

            if (hasInteriorWall) {
                foundShaped = true;
                break;
            }
        }

        if (foundShaped) break;
    }

    expect(foundCandidate, "Expected at least one normal room candidate across seeds (depth 1)");
    expect(foundShaped, "Expected at least one shaped normal room with interior walls across seeds");
}




void test_secret_shortcut_doors_generate() {
    // Secret shortcuts are hidden doors placed in corridor walls that connect two adjacent
    // regions which are already connected elsewhere (creating an optional loop/shortcut).
    //
    // Scan across seeds and confirm we see at least one such shortcut on a standard rooms floor.
    bool foundAny = false;

    for (uint32_t seed = 1; seed <= 250; ++seed) {
        RNG rng(7777u + seed * 23u);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/1, /*maxDepth=*/10);

        if (d.secretShortcutCount <= 0) continue;
        foundAny = true;

        // Shortcut doors are deliberately placed *outside* any room bounds (in corridor walls),
        // so we can sanity-check that they exist and match the recorded count.
        int outsideRooms = 0;
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                if (d.at(x, y).type != TileType::DoorSecret) continue;

                bool inRoom = false;
                for (const Room& r : d.rooms) {
                    if (r.contains(x, y)) { inRoom = true; break; }
                }

                if (!inRoom) outsideRooms += 1;
            }
        }

        expect(outsideRooms == d.secretShortcutCount,
               "Expected secretShortcutCount to match DoorSecret tiles outside any room bounds");
        break;
    }

    expect(foundAny, "Expected at least one secret shortcut door across seeds (depth 1)");
}

void test_locked_shortcut_gates_generate() {
    // Locked shortcuts are visible locked doors placed in corridor walls that connect
    // two adjacent corridor regions that are already connected elsewhere (optional shortcut).
    //
    // Scan across seeds and confirm we see at least one such gate on a mid-run rooms floor.
    bool foundAny = false;

    for (uint32_t seed = 1; seed <= 300; ++seed) {
        RNG rng(8888u + seed * 29u);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/5, /*maxDepth=*/10);

        if (d.lockedShortcutCount <= 0) continue;
        foundAny = true;

        int outsideRooms = 0;
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                if (d.at(x, y).type != TileType::DoorLocked) continue;

                bool inRoom = false;
                for (const Room& r : d.rooms) {
                    if (r.contains(x, y)) { inRoom = true; break; }
                }

                if (!inRoom) outsideRooms += 1;
            }
        }

        expect(outsideRooms == d.lockedShortcutCount,
               "Expected lockedShortcutCount to match DoorLocked tiles outside any room bounds");
        break;
    }

    expect(foundAny, "Expected at least one locked shortcut gate across seeds (depth 5)");
}

void test_corridor_hubs_and_halls_generate() {
    // The corridor polish pass is semi-random. Scan across seeds and confirm we see
    // at least one floor with widened junction hubs and/or a widened hall segment.
    bool foundAny = false;

    for (uint32_t seed = 1; seed <= 200; ++seed) {
        RNG rng(9090u + seed * 31u);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/1, /*maxDepth=*/10);

        const int n = d.corridorHubCount + d.corridorHallCount;
        if (n <= 0) continue;

        // Sanity: widened corridors should never be recorded on a floor without rooms.
        expect(!d.rooms.empty(), "Expected corridor hubs/halls only on room-driven floors");

        // Basic geometric smoke test: look for a 2x2 block of outside-room floor.
        // We don't assume every hub/hall produces a 2x2 (plus-shaped hubs might not),
        // so we keep scanning until we find a seed where widening clearly manifests.
        std::vector<uint8_t> roomMask(static_cast<size_t>(d.width * d.height), 0);
        auto idx = [&](int x, int y) { return static_cast<size_t>(y * d.width + x); };
        for (const Room& r : d.rooms) {
            for (int y = r.y; y < r.y2(); ++y) {
                for (int x = r.x; x < r.x2(); ++x) {
                    if (d.inBounds(x, y)) roomMask[idx(x, y)] = 1;
                }
            }
        }

        bool foundBlock = false;
        for (int y = 1; y < d.height - 1 && !foundBlock; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                if (roomMask[idx(x, y)] != 0) continue;
                if (roomMask[idx(x + 1, y)] != 0) continue;
                if (roomMask[idx(x, y + 1)] != 0) continue;
                if (roomMask[idx(x + 1, y + 1)] != 0) continue;

                if (d.at(x, y).type != TileType::Floor) continue;
                if (d.at(x + 1, y).type != TileType::Floor) continue;
                if (d.at(x, y + 1).type != TileType::Floor) continue;
                if (d.at(x + 1, y + 1).type != TileType::Floor) continue;

                foundBlock = true;
                break;
            }
        }

        if (!foundBlock) continue;

        foundAny = true;
        break;
    }

    expect(foundAny, "Expected at least one corridor hub/hall floor across seeds (depth 1)");
}



void test_sinkholes_generate_deep_mines() {
    // Sinkholes are small, irregular chasm clusters carved into corridor/tunnel tiles.
    // Deep Mines are intentionally unstable, so we expect at least one sinkhole cluster.
    RNG rng(424242u);
    Dungeon d(60, 40);
    d.generate(rng, Dungeon::DEEP_MINES_DEPTH, /*maxDepth=*/10);

    expect(d.sinkholeCount > 0, "Expected at least one sinkhole cluster on Deep Mines");

    auto inBounds = [&](Vec2i p) { return d.inBounds(p.x, p.y); };
    expect(inBounds(d.stairsUp), "stairsUp out of bounds (deep mines)");
    expect(inBounds(d.stairsDown), "stairsDown out of bounds (deep mines)");

    if (inBounds(d.stairsUp)) {
        expect(d.at(d.stairsUp.x, d.stairsUp.y).type == TileType::StairsUp, "stairsUp tile type incorrect (deep mines)");
    }
    if (inBounds(d.stairsDown)) {
        expect(d.at(d.stairsDown.x, d.stairsDown.y).type == TileType::StairsDown, "stairsDown tile type incorrect (deep mines)");
    }

    // BFS from up to down using passable tiles (match in-game diagonal rules).
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

            // No cutting diagonal corners.
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
        expect(visited[idx(d.stairsDown.x, d.stairsDown.y)] != 0, "stairsDown not reachable from stairsUp (deep mines)");
    }
}

void test_dead_end_stash_closets_generate() {
    // Dead-end stash closets are tiny side rooms carved off corridor/tunnel dead ends.
    // Mines floors are exploration-heavy, so closets should appear fairly often.
    bool foundAny = false;

    for (uint32_t seed = 1; seed <= 120; ++seed) {
        RNG rng(31337u + seed * 97u);
        Dungeon d(60, 40);
        d.generate(rng, Dungeon::MINES_DEPTH, /*maxDepth=*/10);

        if (d.deadEndClosetCount > 0) {
            foundAny = true;
            // Keep the feature sane: don't explode chest count.
            expect(d.deadEndClosetCount <= 3, "Expected dead-end closet count to be small (<= 3)");
            break;
        }
    }

    expect(foundAny, "Expected to find at least one dead-end stash closet across seeds (Mines depth)");
}






void test_special_rooms_paced_by_distance() {
    // Special rooms are assigned using a light distance heuristic:
    //   - Shops tend to be closer to the upstairs (so gold matters earlier)
    //   - Treasure rooms tend to be deeper (so exploration is rewarded)
    //
    // On depth 5, shops are (conditionally) guaranteed when there is room, making
    // it a good depth to smoke-test pacing.

    int checked = 0;

    for (uint32_t seed = 1; seed <= 40; ++seed) {
        RNG rng(2026u + seed * 17u);
        Dungeon d(60, 40);
        d.generate(rng, /*depth=*/5, /*maxDepth=*/10);

        int shopIdx = -1;
        int treasureIdx = -1;
        for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
            const RoomType rt = d.rooms[static_cast<size_t>(i)].type;
            if (rt == RoomType::Shop) shopIdx = i;
            if (rt == RoomType::Treasure && treasureIdx < 0) treasureIdx = i;
        }

        if (shopIdx < 0 || treasureIdx < 0) continue;

        // BFS passable distance from stairsUp.
        std::vector<int> dist(static_cast<size_t>(d.width * d.height), -1);
        auto idx = [&](int x, int y) { return y * d.width + x; };

        if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) continue;
        std::queue<Vec2i> q;
        dist[static_cast<size_t>(idx(d.stairsUp.x, d.stairsUp.y))] = 0;
        q.push(d.stairsUp);

        const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        while (!q.empty()) {
            Vec2i p = q.front();
            q.pop();
            const int cd = dist[static_cast<size_t>(idx(p.x, p.y))];

            for (auto& dv : dirs) {
                const int nx = p.x + dv[0];
                const int ny = p.y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (!d.isPassable(nx, ny)) continue;
                const int ii = idx(nx, ny);
                if (dist[static_cast<size_t>(ii)] != -1) continue;
                dist[static_cast<size_t>(ii)] = cd + 1;
                q.push({nx, ny});
            }
        }

        auto roomMinDist = [&](const Room& r) -> int {
            int best = 1000000000;
            for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
                for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
                    if (!d.inBounds(x, y)) continue;
                    if (!d.isPassable(x, y)) continue;
                    const int di = dist[static_cast<size_t>(idx(x, y))];
                    if (di >= 0 && di < best) best = di;
                }
            }
            return (best == 1000000000) ? -1 : best;
        };

        const int sd = roomMinDist(d.rooms[static_cast<size_t>(shopIdx)]);
        const int td = roomMinDist(d.rooms[static_cast<size_t>(treasureIdx)]);

        expect(sd >= 0, "Shop room should be reachable from stairsUp (depth 5)");
        expect(td >= 0, "Treasure room should be reachable from stairsUp (depth 5)");

        if (sd >= 0 && td >= 0) {
            expect(sd <= td, "Shop should generally be closer to stairsUp than Treasure (pacing)");
            checked += 1;
        }
    }

    expect(checked >= 8, "Expected to validate shop/treasure pacing on many seeds (depth 5)");
}


void test_rogue_level_layout() {
    RNG rng(1337u);
    Dungeon d(60, 40);
    d.generate(rng, Dungeon::ROGUE_LEVEL_DEPTH, /*maxDepth=*/10);

    // The Rogue homage floor should be a simple 3x3 room grid.
    expect(d.rooms.size() == 9, "Rogue level should generate exactly 9 rooms");

    // It is intentionally doorless: no open/closed/locked/secret doors should be present.
    bool hasDoor = false;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const TileType t = d.at(x, y).type;
            if (t == TileType::DoorClosed || t == TileType::DoorOpen || t == TileType::DoorLocked || t == TileType::DoorSecret) {
                hasDoor = true;
                break;
            }
        }
        if (hasDoor) break;
    }
    expect(!hasDoor, "Rogue level should contain no doors");

    // Stairs must exist and be correctly typed.
    expect(d.inBounds(d.stairsUp.x, d.stairsUp.y), "Rogue level has stairsUp");
    expect(d.inBounds(d.stairsDown.x, d.stairsDown.y), "Rogue level has stairsDown");
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        expect(d.at(d.stairsUp.x, d.stairsUp.y).type == TileType::StairsUp, "Rogue level stairsUp tile type is correct");
    }
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        expect(d.at(d.stairsDown.x, d.stairsDown.y).type == TileType::StairsDown, "Rogue level stairsDown tile type is correct");
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

    d.at(7, 5).type = TileType::Boulder;
    expect(!d.isPassable(7, 5), "Boulder should not be passable");
    expect(!d.isWalkable(7, 5), "Boulder should not be walkable");
    expect(!d.isOpaque(7, 5), "Boulder should not block FOV/LOS (current design)");

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

void test_augury_preview_does_not_consume_rng() {
    Game g;
    g.newGame(123u);

    // Move to the surface camp so augury is allowed without requiring a shrine to spawn.
    GameTestAccess::changeLevel(g, 0, false);

    // Remove all monsters so advancing a turn won't consume RNG.
    {
        auto& ents = GameTestAccess::ents(g);
        const int pid = g.playerId();
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != pid;
        }), ents.end());
    }

    // Ensure we have enough gold to pay the augury cost.
    {
        auto& inv = GameTestAccess::inv(g);
        inv.clear();
        Item gold;
        gold.kind = ItemKind::Gold;
        gold.count = 999;
        inv.push_back(gold);
    }

    const uint32_t before = GameTestAccess::rng(g).state;
    const bool spent = g.augury();
    const uint32_t after = GameTestAccess::rng(g).state;

    expect(spent, "augury should spend a turn at camp when you have gold");
    expect(before == after, "augury should not consume the main game RNG state");
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

void test_dig_prompt_digs_adjacent_wall_with_pickaxe() {
    Game g;
    g.newGame(123u);

    // Clear non-player entities for determinism.
    auto& ents = GameTestAccess::ents(g);
    const int pid = g.playerId();
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != pid;
    }), ents.end());

    Dungeon& d = GameTestAccess::dungeon(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            auto& t = d.at(x, y);
            t.type = TileType::Floor;
            t.visible = false;
            t.explored = true;
        }
    }

    GameTestAccess::traps(g).clear();

    // Place the player and a diggable wall.
    g.playerMut().pos = {5, 5};
    d.at(6, 5).type = TileType::Wall;

    // Equip a pickaxe.
    GameTestAccess::inv(g).clear();
    Item pick{};
    pick.id = 1;
    pick.kind = ItemKind::Pickaxe;
    pick.count = 1;
    GameTestAccess::inv(g).push_back(pick);
    GameTestAccess::invSel(g) = 0;
    expect(GameTestAccess::equipSelected(g), "Equipping a pickaxe should succeed");

    const int turns0 = g.turns();
    g.handleAction(Action::Dig);
    expect(g.turns() == turns0, "Entering dig prompt should not spend a turn");

    g.handleAction(Action::Right);
    expect(g.turns() == turns0 + 1, "Digging should spend one turn");
    expect(d.at(6, 5).type == TileType::Floor, "Dig prompt should convert the adjacent wall to floor");
    expect(g.player().pos == Vec2i{5, 5}, "Dig prompt should not move the player");
}

void test_wand_display_shows_charges() {
    Item it;
    it.kind = ItemKind::WandDigging;
    it.count = 1;
    it.charges = 3;

    const std::string name = itemDisplayName(it);
    expect(name.find("(3/8)") != std::string::npos, "Wand of digging should show charges in display name");
}

void test_shop_debt_ledger_persists_after_consumption() {
    Game g;
    g.newGame(123u);

    // Start from a clean inventory and no prior shop ledger.
    GameTestAccess::inv(g).clear();
    GameTestAccess::shopLedger(g).fill(0);
    GameTestAccess::depth(g) = 1;

    Item p{};
    p.id = 1;
    p.kind = ItemKind::PotionHealing;
    p.count = 1;
    p.shopPrice = 50;
    p.shopDepth = 1;

    GameTestAccess::inv(g).push_back(p);
    GameTestAccess::invSel(g) = 0;

    expect(g.shopDebtThisDepth() == 50, "Unpaid shop potion should contribute to shop debt (pre-consumption)");
    expect(GameTestAccess::shopLedger(g)[1] == 0, "Shop debt ledger should start empty");

    expect(GameTestAccess::useSelected(g) == true, "Using a potion should succeed");
    expect(GameTestAccess::inv(g).empty(), "Potion should be removed from inventory after use");

    // Even though the item is gone, we should still owe the shopkeeper.
    expect(GameTestAccess::shopLedger(g)[1] == 50, "Consumed unpaid shop goods should be moved into the shop debt ledger");
    expect(g.shopDebtThisDepth() == 50, "Shop debt should remain after consuming unpaid goods");
}

void test_shop_debt_ledger_save_load_roundtrip() {
    const std::string path = "test_shop_debt_ledger_tmp.sav";
    (void)std::remove(path.c_str());

    Game g;
    g.newGame(321u);
    GameTestAccess::shopLedger(g).fill(0);
    GameTestAccess::shopLedger(g)[2] = 77;

    expect(g.saveToFile(path, true), "Saving should succeed");
    Game g2;
    expect(g2.loadFromFile(path), "Loading should succeed");

    expect(GameTestAccess::shopLedger(g2)[2] == 77, "Shop debt ledger should round-trip through save/load");
    expect(g2.shopDebtTotal() >= 77, "shopDebtTotal should include the shop debt ledger");

    (void)std::remove(path.c_str());
}

void test_engravings_basic_and_save_load_roundtrip() {
    const std::string path = "test_engravings_tmp.sav";
    (void)std::remove(path.c_str());

    Game g;
    g.newGame(123u);
    const Vec2i p = g.player().pos;

    expect(g.engraveHere("Hello World"), "engraveHere should succeed on walkable tiles");
    {
        const Engraving* eg = g.engravingAt(p);
        expect(eg != nullptr, "engravingAt should find the player-engraved text");
        expect(eg->text == "Hello World", "Engraving text should match");
        expect(!eg->isGraffiti, "Player engravings should not be flagged as graffiti");
        expect(!eg->isWard, "Non-ward engravings should not be flagged as ward");
        expect(eg->strength == 255, "Non-ward engravings should default to permanent strength (255)");
    }

    expect(g.engraveHere("Elbereth"), "engraveHere should accept the classic warding word");
    {
        const Engraving* eg = g.engravingAt(p);
        expect(eg != nullptr, "Ward engraving should exist");
        expect(eg->isWard, "Elbereth engraving should be flagged as a ward");
        expect(eg->strength < 255, "Ward engravings should not be permanent");
    }

    expect(g.saveToFile(path, true), "Saving should succeed");

    Game g2;
    expect(g2.loadFromFile(path), "Loading should succeed");

    {
        const Engraving* eg = g2.engravingAt(p);
        expect(eg != nullptr, "Engraving should round-trip through save/load");
        expect(eg->isWard, "Ward flag should round-trip through save/load");
        expect(eg->text == "Elbereth", "Engraving text should round-trip through save/load");
    }
    (void)std::remove(path.c_str());
}

void test_command_prompt_preserves_look_cursor() {
    Game g;
    g.newGame(123u);

    const Vec2i ppos = g.player().pos;

    g.handleAction(Action::Look);
    expect(g.isLooking(), "LOOK mode should activate");

    const Vec2i start = g.lookCursor();
    expect(start == ppos, "LOOK cursor should start at the player position");

    g.handleAction(Action::Right);
    const Vec2i moved = g.lookCursor();
    expect(moved != start, "LOOK cursor should move");
    expect(g.player().pos == ppos, "Player should not move while LOOK is active");

    g.handleAction(Action::Command);
    expect(g.isCommandOpen(), "Command prompt should open from LOOK mode");
    expect(g.isLooking(), "LOOK mode should remain active while the command prompt is open");
    expect(g.lookCursor() == moved, "LOOK cursor should be preserved when opening the command prompt");

    g.handleAction(Action::Cancel);
    expect(!g.isCommandOpen(), "Cancel should close the command prompt");
    expect(g.isLooking(), "Closing the command prompt should not close LOOK mode");

    g.handleAction(Action::Look);
    expect(!g.isLooking(), "LOOK should be closable after using the command prompt");
}

void test_throwvoice_alerts_monster_at_target_tile() {
    Game g;
    g.newGame(123u);

    // Make a simple open map so sound propagation is predictable.
    Dungeon& d = GameTestAccess::dungeon(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            auto& t = d.at(x, y);
            t.type = TileType::Floor;
            t.visible = false;
            t.explored = true;
        }
    }

    // Reposition the player.
    Entity* p = nullptr;
    for (auto& e : GameTestAccess::ents(g)) {
        if (e.id == g.playerId()) {
            p = &e;
            break;
        }
    }
    expect(p != nullptr, "Player entity should exist");
    if (!p) return;

    p->pos = {5, 5};
    p->effects.confusionTurns = 0;

    // Spawn a hostile monster.
    Entity m;
    m.id = 424242;
    m.kind = EntityKind::Goblin;
    m.pos = {8, 5};
    m.hp = 5;
    m.hpMax = 5;
    m.alerted = false;
    m.friendly = false;
    m.lastKnownPlayerPos = {-1, -1};
    GameTestAccess::ents(g).push_back(m);

    const int turns0 = g.turns();
    const bool spent = g.throwVoiceAt({8, 5});
    expect(spent, "throwVoiceAt should succeed on in-range, sound-passable tiles");
    expect(g.turns() == turns0 + 1, "throwVoiceAt should spend a turn");

    Entity* mm = nullptr;
    for (auto& e : GameTestAccess::ents(g)) {
        if (e.id == 424242) {
            mm = &e;
            break;
        }
    }
    expect(mm != nullptr, "Spawned monster should exist");
    if (!mm) return;

    expect(mm->alerted, "Monster should become alerted by thrown-voice noise");
    expect(mm->lastKnownPlayerPos == Vec2i{8, 5},
           "Monster should investigate the thrown-voice location");
}

void test_listen_reports_hidden_monster_and_spends_turn() {
    Game g;
    g.newGame(123u);

    Dungeon& d = GameTestAccess::dungeon(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            auto& t = d.at(x, y);
            t.type = TileType::Floor;
            t.visible = false;
            t.explored = true;
        }
    }

    Entity* p = nullptr;
    for (auto& e : GameTestAccess::ents(g)) {
        if (e.id == g.playerId()) {
            p = &e;
            break;
        }
    }
    expect(p != nullptr, "Player entity should exist");
    if (!p) return;

    p->pos = {5, 5};
    p->effects.confusionTurns = 0;

    Entity m;
    m.id = 777777;
    m.kind = EntityKind::Goblin;
    m.pos = {7, 5};
    m.hp = 5;
    m.hpMax = 5;
    m.friendly = false;
    GameTestAccess::ents(g).push_back(m);

    const int turns0 = g.turns();
    g.listen();
    expect(g.turns() == turns0 + 1, "listen should spend a turn");

    const auto& msgs = g.messages();
    expect(!msgs.empty(), "listen should add at least one message");
    if (!msgs.empty()) {
        expect(msgs.back().text.find("YOU HEAR") != std::string::npos,
               "listen message should mention hearing");
    }
}

void test_scroll_fear_applies_fear_to_visible_monsters() {
    Game g;
    g.newGame(123u);

    // Give the player a scroll of fear.
    GameTestAccess::inv(g).clear();
    Item s;
    s.id = 9001;
    s.kind = ItemKind::ScrollFear;
    s.count = 1;
    GameTestAccess::inv(g).push_back(s);
    GameTestAccess::invSel(g) = 0;

    // Place a hostile monster on a visible, walkable adjacent tile.
    const Dungeon& d = g.dungeon();
    const Vec2i ppos = g.player().pos;

    auto hasEntityAt = [&](int x, int y) -> bool {
        for (const auto& e : GameTestAccess::ents(g)) {
            if (e.hp <= 0) continue;
            if (e.pos.x == x && e.pos.y == y) return true;
        }
        return false;
    };

    Vec2i mpos = ppos;
    bool found = false;
    for (int dy = -1; dy <= 1 && !found; ++dy) {
        for (int dx = -1; dx <= 1 && !found; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const Vec2i q{ppos.x + dx, ppos.y + dy};
            if (!d.inBounds(q.x, q.y)) continue;
            if (!d.isWalkable(q.x, q.y)) continue;

            bool occupied = false;
            for (const Entity& e : GameTestAccess::ents(g)) {
                if (e.hp > 0 && e.pos == q) { occupied = true; break; }
            }
            if (occupied) continue;

            mpos = q;
            found = true;
        }
    }

    expect(found, "Should find an empty walkable adjacent tile for monster placement");
    if (!found) return;

    expect(d.at(mpos.x, mpos.y).visible, "Placed monster tile should be visible to the player");

    Entity m;
    m.id = 424242;
    m.kind = EntityKind::Goblin;
    m.pos = mpos;
    m.hpMax = 6;
    m.hp = 6;
    m.friendly = false;
    GameTestAccess::ents(g).push_back(m);

    const bool ok = GameTestAccess::useSelected(g);
    expect(ok, "Using scroll of fear should succeed");

    int fearTurns = 0;
    for (const Entity& e : GameTestAccess::ents(g)) {
        if (e.id == 424242) {
            fearTurns = e.effects.fearTurns;
            break;
        }
    }

    expect(fearTurns > 0, "Scroll of fear should apply fearTurns to a visible hostile monster");
}

void test_scroll_earth_raises_boulders_around_player() {
    Game g;
    g.newGame(123u);

    // Remove non-player entities for a deterministic neighborhood.
    auto& ents = GameTestAccess::ents(g);
    const int pid = g.player().id;
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != pid;
    }), ents.end());

    // Give the player a scroll of earth.
    GameTestAccess::inv(g).clear();
    Item s;
    s.id = 9002;
    s.kind = ItemKind::ScrollEarth;
    s.count = 1;
    GameTestAccess::inv(g).push_back(s);
    GameTestAccess::invSel(g) = 0;

    // Find a tile with an 8-neighbor walkable ring.
    const Dungeon& d = g.dungeon();
    Vec2i spot = g.player().pos;
    bool found = false;
    for (int y = 1; y < d.height - 1 && !found; ++y) {
        for (int x = 1; x < d.width - 1 && !found; ++x) {
            if (!d.isWalkable(x, y)) continue;
            if (Vec2i{x, y} == d.stairsUp || Vec2i{x, y} == d.stairsDown) continue;

            bool ok = true;
            for (int dy = -1; dy <= 1 && ok; ++dy) {
                for (int dx = -1; dx <= 1 && ok; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int xx = x + dx;
                    const int yy = y + dy;
                    if (!d.inBounds(xx, yy)) { ok = false; break; }
                    if (!d.isWalkable(xx, yy)) { ok = false; break; }
                    if (Vec2i{xx, yy} == d.stairsUp || Vec2i{xx, yy} == d.stairsDown) { ok = false; break; }
                }
            }
            if (!ok) continue;

            spot = {x, y};
            found = true;
        }
    }

    expect(found, "Should find a walkable tile with 8 walkable neighbors for scroll of earth test");
    if (!found) return;

    g.playerMut().pos = spot;

    const bool ok = GameTestAccess::useSelected(g);
    expect(ok, "Using scroll of earth should succeed");

    // The scroll should be consumed.
    expect(GameTestAccess::inv(g).empty(), "Scroll of earth should be removed from inventory after use");

    // All 8 surrounding tiles should now be boulders (we chose a fully-walkable ring).
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const int x = spot.x + dx;
            const int y = spot.y + dy;
            expect(d.at(x, y).type == TileType::Boulder, "Scroll of earth should raise boulders in adjacent tiles");
        }
    }
}

void test_scroll_taming_charms_adjacent_non_undead_monsters() {
    Game g;
    g.newGame(123u);

    // Remove non-player entities for a deterministic neighborhood.
    auto& ents = GameTestAccess::ents(g);
    const int pid = g.player().id;
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != pid;
    }), ents.end());

    // Give the player a scroll of taming.
    GameTestAccess::inv(g).clear();
    Item s;
    s.id = 9003;
    s.kind = ItemKind::ScrollTaming;
    s.count = 1;
    GameTestAccess::inv(g).push_back(s);
    GameTestAccess::invSel(g) = 0;

    // Place a non-undead monster (goblin) and an undead (ghost) adjacent to the player.
    const Dungeon& d = g.dungeon();
    const Vec2i ppos = g.player().pos;

    Vec2i gobPos = ppos;
    Vec2i ghostPos = ppos;
    bool foundGob = false;
    bool foundGhost = false;

    for (int dy = -1; dy <= 1 && !(foundGob && foundGhost); ++dy) {
        for (int dx = -1; dx <= 1 && !(foundGob && foundGhost); ++dx) {
            if (dx == 0 && dy == 0) continue;
            const Vec2i q{ppos.x + dx, ppos.y + dy};
            if (!d.inBounds(q.x, q.y)) continue;
            if (!d.isWalkable(q.x, q.y)) continue;

            if (!foundGob) {
                gobPos = q;
                foundGob = true;
            } else if (!foundGhost && q != gobPos) {
                ghostPos = q;
                foundGhost = true;
            }
        }
    }

    expect(foundGob, "Should find an empty walkable adjacent tile for goblin placement");
    expect(foundGhost, "Should find a second empty walkable adjacent tile for ghost placement");
    if (!foundGob || !foundGhost) return;

    Entity gob;
    gob.id = 424240;
    gob.kind = EntityKind::Goblin;
    gob.pos = gobPos;
    gob.hpMax = 6;
    gob.hp = 6;
    gob.friendly = false;
    ents.push_back(gob);

    Entity ghost;
    ghost.id = 424241;
    ghost.kind = EntityKind::Ghost;
    ghost.pos = ghostPos;
    ghost.hpMax = 8;
    ghost.hp = 8;
    ghost.friendly = false;
    ents.push_back(ghost);

    // Force deterministic success for the goblin charm roll.
    GameTestAccess::rng(g).state = 72u;

    const bool ok = GameTestAccess::useSelected(g);
    expect(ok, "Using scroll of taming should succeed");

    bool gobFriendly = false;
    bool ghostFriendly = false;
    AllyOrder gobOrder = AllyOrder::Stay;
    for (const Entity& e : ents) {
        if (e.id == 424240) { gobFriendly = e.friendly; gobOrder = e.allyOrder; }
        if (e.id == 424241) { ghostFriendly = e.friendly; }
    }

    expect(gobFriendly, "Scroll of taming should make adjacent non-undead monsters friendly");
    expect(gobOrder == AllyOrder::Follow, "Newly tamed monsters should default to Follow order");
    expect(!ghostFriendly, "Scroll of taming should not affect undead monsters");

    // The scroll should be consumed.
    expect(GameTestAccess::inv(g).empty(), "Scroll of taming should be removed from inventory after use");
}


void test_companion_fetch_carries_and_delivers_gold() {
    Game g;
    g.newGame(888u);

    auto& ents = GameTestAccess::ents(g);
    auto& ground = GameTestAccess::ground(g);
    auto& traps = GameTestAccess::traps(g);
    Dungeon& d = GameTestAccess::dungeon(g);

    const int pid = g.player().id;

    int dogId = 0;
    for (const auto& e : ents) {
        if (e.kind == EntityKind::Dog && e.friendly && e.hp > 0) {
            dogId = e.id;
            break;
        }
    }

    expect(dogId != 0, "New game should spawn a friendly dog companion");
    if (dogId == 0) return;

    // Keep only player + dog so behavior is deterministic.
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != pid && e.id != dogId;
    }), ents.end());

    Entity* player = nullptr;
    Entity* dog = nullptr;
    for (auto& e : ents) {
        if (e.id == pid) player = &e;
        else if (e.id == dogId) dog = &e;
    }

    expect(player != nullptr && dog != nullptr, "Should retain player and dog entities");
    if (!player || !dog) return;

    // Clear clutter that could interfere with the test.
    ground.clear();
    traps.clear();

    GameTestAccess::recomputeFov(g);

    auto countGold = [&](const std::vector<Item>& inv) {
        int sum = 0;
        for (const auto& it : inv) {
            if (it.kind == ItemKind::Gold) sum += it.count;
        }
        return sum;
    };

    const int startGold = countGold(GameTestAccess::inv(g));

    const Vec2i ppos = player->pos;

    auto isOccupied = [&](Vec2i p) {
        for (const auto& e : ents) {
            if (e.hp <= 0) continue;
            if (e.pos == p) return true;
        }
        return false;
    };

    // Place the gold somewhere the player can currently see (FETCH only targets visible gold).
    Vec2i goldPos{-1, -1};
    for (int r = 1; r <= 10 && goldPos.x < 0; ++r) {
        for (int dy = -r; dy <= r && goldPos.x < 0; ++dy) {
            for (int dx = -r; dx <= r && goldPos.x < 0; ++dx) {
                if (std::abs(dx) + std::abs(dy) != r) continue; // manhattan ring
                const int x = ppos.x + dx;
                const int y = ppos.y + dy;
                if (!d.inBounds(x, y)) continue;
                if (!d.isWalkable(x, y)) continue;
                if (!d.at(x, y).visible) continue;
                if (isOccupied({x, y})) continue;
                goldPos = {x, y};
            }
        }
    }

    expect(goldPos.x >= 0, "Should find a visible walkable tile for gold placement");
    if (goldPos.x < 0) return;

    // Put the dog a bit away so it has to path to the gold, then return to the player.
    Vec2i dogPos{-1, -1};
    for (int r = 4; r <= 18 && dogPos.x < 0; ++r) {
        for (int dy = -r; dy <= r && dogPos.x < 0; ++dy) {
            for (int dx = -r; dx <= r && dogPos.x < 0; ++dx) {
                if (std::abs(dx) + std::abs(dy) != r) continue;
                const int x = ppos.x + dx;
                const int y = ppos.y + dy;
                if (!d.inBounds(x, y)) continue;
                if (!d.isWalkable(x, y)) continue;
                if (isOccupied({x, y})) continue;
                if (Vec2i{x, y} == goldPos) continue;
                dogPos = {x, y};
            }
        }
    }

    expect(dogPos.x >= 0, "Should find a walkable tile for dog placement");
    if (dogPos.x < 0) return;

    dog->pos = dogPos;
    dog->allyOrder = AllyOrder::Fetch;
    dog->stolenGold = 0;

    // Make scheduling predictable (one action per monster turn).
    dog->speed = 100;
    dog->energy = 0;

    // Drop a gold pile and ensure FOV is up to date.
    GameTestAccess::dropGroundItem(g, goldPos, ItemKind::Gold, 25);
    GameTestAccess::recomputeFov(g);

    const int goalGold = startGold + 25;
    bool delivered = false;

    // Run monster turns until the dog delivers.
    for (int t = 0; t < 60; ++t) {
        GameTestAccess::monsterTurn(g);
        const int curGold = countGold(GameTestAccess::inv(g));
        if (curGold >= goalGold) {
            delivered = (curGold == goalGold);
            break;
        }
    }

    expect(delivered, "FETCH companion should deliver carried gold to the player");
    expect(dog->stolenGold == 0, "After delivery, companion should no longer be carrying gold");
}


void test_monster_ai_avoids_fire_tiles_when_chasing() {
    Game g;
    g.newGame(123u);

    auto& ents = GameTestAccess::ents(g);
    const int pid = g.player().id;

    // Keep only the player so the pathing decision is deterministic.
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != pid;
    }), ents.end());

    // Replace the dungeon with a small open arena.
    Dungeon& d = GameTestAccess::dungeon(g);
    d = Dungeon(7, 7);
    for (auto& t : d.tiles) {
        t.type = TileType::Floor;
        t.visible = true;
        t.explored = true;
    }

    GameTestAccess::ground(g).clear();
    GameTestAccess::traps(g).clear();

    // Place the player and a single hostile goblin.
    ents[0].pos = {5, 3};

    Entity gob;
    gob.id = 2001;
    gob.kind = EntityKind::Goblin;
    gob.pos = {1, 3};
    gob.hpMax = 6;
    gob.hp = 6;
    gob.friendly = false;

    // Make scheduling predictable (one action per monster turn).
    gob.speed = 100;
    gob.energy = 0;

    ents.push_back(gob);

    // Put fire directly on the straight-line route so a naive chase would step into it.
    auto& fire = GameTestAccess::fireField(g);
    fire.assign(static_cast<size_t>(d.width * d.height), 0u);
    fire[static_cast<size_t>(3 * d.width + 2)] = 255u; // tile (2,3)

    GameTestAccess::monsterTurn(g);

    Vec2i mpos{-1, -1};
    for (const auto& e : ents) {
        if (e.id == 2001) {
            mpos = e.pos;
            break;
        }
    }

    expect(mpos != Vec2i{2, 3}, "Monster AI should avoid stepping onto fire when an alternate path exists");
}


void test_targeting_tab_cycles_visible_hostiles() {
    Game g;
    g.newGame(777u);

    // Remove non-player entities to keep the candidate list deterministic.
    auto& ents = GameTestAccess::ents(g);
    const int pid = g.player().id;
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != pid;
    }), ents.end());

    // Find a floor tile where EAST and SOUTH neighbors are walkable so ordering is stable:
    //  - both targets are distance 1
    //  - sort tie-break uses y then x, so EAST (same y) comes before SOUTH (y+1)
    const Dungeon& d = g.dungeon();
    Vec2i center{-1, -1};
    Vec2i east{-1, -1};
    Vec2i south{-1, -1};

    for (int y = 1; y < d.height - 1 && center.x < 0; ++y) {
        for (int x = 1; x < d.width - 1 && center.x < 0; ++x) {
            if (!d.isWalkable(x, y)) continue;
            if (!d.isWalkable(x + 1, y)) continue;
            if (!d.isWalkable(x, y + 1)) continue;

            // Prefer actual floor (avoid stairs) so the description string is stable-ish.
            if (d.at(x, y).type != TileType::Floor) continue;
            if (d.at(x + 1, y).type != TileType::Floor) continue;
            if (d.at(x, y + 1).type != TileType::Floor) continue;

            center = {x, y};
            east = {x + 1, y};
            south = {x, y + 1};
        }
    }

    expect(center.x >= 0, "Should find a suitable floor tile for targeting cycle test");
    if (center.x < 0) return;

    // Move player to the chosen center.
    ents[0].pos = center;
    GameTestAccess::recomputeFov(g);

    // Place two visible hostiles.
    Entity a;
    a.id = 901;
    a.kind = EntityKind::Goblin;
    a.pos = east;
    a.hpMax = 6;
    a.hp = 6;
    a.friendly = false;
    ents.push_back(a);

    Entity b;
    b.id = 902;
    b.kind = EntityKind::Orc;
    b.pos = south;
    b.hpMax = 10;
    b.hp = 10;
    b.friendly = false;
    ents.push_back(b);

    // Start targeting.
    g.handleAction(Action::Fire);
    expect(g.isTargeting(), "FIRE should enter targeting mode when a ranged option is ready");
    if (!g.isTargeting()) return;

    // TAB (Inventory) cycles to the first target in sorted order.
    g.handleAction(Action::Inventory);
    expect(g.targetingCursor() == east, "TAB in targeting mode should cycle to the nearest visible hostile (east)");

    // TAB again cycles to the next target.
    g.handleAction(Action::Inventory);
    expect(g.targetingCursor() == south, "TAB in targeting mode should cycle to the next visible hostile (south)");

    // SHIFT+TAB (ToggleStats) cycles back.
    g.handleAction(Action::ToggleStats);
    expect(g.targetingCursor() == east, "SHIFT+TAB in targeting mode should cycle to the previous hostile (east)");

    // Cancel out (should not crash / consume a turn).
    g.handleAction(Action::Cancel);
    expect(!g.isTargeting(), "Cancel should exit targeting mode");
}

void test_boulder_blocks_projectiles_but_not_opaque() {
    Dungeon d(5, 5);
    // Initialize all tiles to floor for clarity.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
        }
    }

    d.at(2, 2).type = TileType::Boulder;
    expect(!d.isOpaque(2, 2), "Boulder should not be opaque (LOS readability)");
    expect(d.blocksProjectiles(2, 2), "Boulder should block projectiles (tactical cover)");

    d.at(1, 1).type = TileType::DoorClosed;
    expect(d.isOpaque(1, 1), "Closed doors should be opaque");
    expect(d.blocksProjectiles(1, 1), "Closed doors should block projectiles");
}

void test_targeting_out_of_range_sets_status_text() {
    Game g;
    g.setPlayerClass(PlayerClass::Archer);
    g.newGame(2026u);

    // Archer starts with a bow and arrows.
    // Create a clear straight hallway to the east so tiles at distance 9 are visible (FOV radius is 9).
    const Vec2i p = g.player().pos;
    Dungeon& d = GameTestAccess::dungeon(g);

    for (int dx = 0; dx <= 12; ++dx) {
        const int x = p.x + dx;
        const int y = p.y;
        if (!d.inBounds(x, y)) break;
        d.at(x, y).type = TileType::Floor;
    }

    GameTestAccess::recomputeFov(g);

    // Enter targeting mode.
    g.handleAction(Action::Fire);
    expect(g.isTargeting(), "FIRE should enter targeting mode when a ranged option is ready");
    if (!g.isTargeting()) return;

    const int range = GameTestAccess::playerRangedRange(g);
    expect(range > 0, "Ranged range should be positive for bow");
    if (range <= 0) return;

    // Aim 1 tile beyond range, but still within FOV.
    const Vec2i aim{p.x + range + 1, p.y};
    if (!d.inBounds(aim.x, aim.y)) return;

    g.setTargetCursor(aim);
    expect(!g.targetingIsValid(), "Aiming beyond weapon range should be invalid");
    expect(g.targetingStatusText() == "OUT OF RANGE", "Targeting should report OUT OF RANGE status");

    g.handleAction(Action::Cancel);
}

void test_save_load_fear_turns_roundtrip() {
    const std::string path = "test_fear_effect_tmp.sav";
    (void)std::remove(path.c_str());

    Game g;
    g.newGame(321u);
    g.playerMut().effects.fearTurns = 9;

    expect(g.saveToFile(path, true), "Saving should succeed");

    Game g2;
    expect(g2.loadFromFile(path), "Loading should succeed");
    expect(g2.player().effects.fearTurns == 9, "Fear turns should round-trip through save/load");

    (void)std::remove(path.c_str());

}


void test_bonus_cache_guards_spawn_adjacent_floor_trap() {
    Game g;
    g.newGame(424242u);

    // Start from a known state.
    GameTestAccess::traps(g).clear();
    GameTestAccess::ground(g).clear();

    Dungeon& d = GameTestAccess::dungeon(g);
    const Vec2i start = g.player().pos;

    auto inRoomType = [&](RoomType t, Vec2i p) {
        for (const auto& r : d.rooms) {
            if (r.type == t && r.contains(p.x, p.y)) return true;
        }
        return false;
    };

    // Find a walkable floor tile far from the start with at least one valid adjacent tile.
    Vec2i cache{-1, -1};
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            Vec2i p{x, y};
            if (!d.isWalkable(x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) continue;
            if (p == d.stairsUp || p == d.stairsDown) continue;
            if (manhattan(p, start) <= 10) continue;
            if (inRoomType(RoomType::Shop, p) || inRoomType(RoomType::Shrine, p)) continue;

            bool hasAdj = false;
            for (int dy = -1; dy <= 1 && !hasAdj; ++dy) {
                for (int dx = -1; dx <= 1 && !hasAdj; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    Vec2i q{p.x + dx, p.y + dy};
                    if (!d.inBounds(q.x, q.y)) continue;
                    if (!d.isWalkable(q.x, q.y)) continue;
                    if (q == d.stairsUp || q == d.stairsDown) continue;
                    if (inRoomType(RoomType::Shop, q) || inRoomType(RoomType::Shrine, q)) continue;
                    if (manhattan(q, start) <= 4) continue;
                    hasAdj = true;
                }
            }

            if (hasAdj) {
                cache = p;
                break;
            }
        }
        if (cache.x != -1) break;
    }

    expect(cache.x != -1, "Found a suitable cache tile for bonus cache guard trap test");
    if (cache.x == -1) return;

    // Simulate a bonus cache chest on the ground at that location.
    Item chest;
    chest.id = 99999;
    chest.kind = ItemKind::Chest;
    chest.count = 1;
    chest.spriteSeed = 1;

    GameTestAccess::ground(g).push_back(GroundItem{chest, cache});
    d.bonusLootSpots.clear();
    d.bonusLootSpots.push_back(cache);

    GameTestAccess::spawnTraps(g);

    bool foundAdjTrap = false;
    for (const Trap& t : GameTestAccess::traps(g)) {
        if (chebyshev(t.pos, cache) == 1) {
            foundAdjTrap = true;
            break;
        }
    }

    expect(foundAdjTrap, "Bonus cache should spawn at least one adjacent floor trap guard when possible");
}


void test_trap_door_drops_player_to_next_depth() {
    Game g;
    g.newGame(1337u);

    // Ensure deterministic setup: remove any randomly spawned traps.
    GameTestAccess::traps(g).clear();

    const int startDepth = GameTestAccess::depth(g);
    const Vec2i startPos = g.player().pos;

    Trap tr;
    tr.pos = startPos;
    tr.kind = TrapKind::TrapDoor;
    tr.discovered = false;
    GameTestAccess::traps(g).push_back(tr);

    GameTestAccess::triggerTrapAt(g, startPos, g.playerMut());

    expect(GameTestAccess::depth(g) == startDepth + 1, "Trap door should drop the player one dungeon depth");

    const Dungeon& d = g.dungeon();
    const Vec2i ppos = g.player().pos;
    expect(ppos != d.stairsUp, "Trap door landing should not place the player on stairsUp");
    expect(ppos != d.stairsDown, "Trap door landing should not place the player on stairsDown");
}

void test_levitation_skips_trap_door_trigger() {
    Game g;
    g.newGame(1337u);

    GameTestAccess::traps(g).clear();

    const int startDepth = GameTestAccess::depth(g);
    const Vec2i startPos = g.player().pos;

    g.playerMut().effects.levitationTurns = 10;

    Trap tr;
    tr.pos = startPos;
    tr.kind = TrapKind::TrapDoor;
    tr.discovered = false;
    GameTestAccess::traps(g).push_back(tr);

    GameTestAccess::triggerTrapAt(g, startPos, g.playerMut());

    expect(GameTestAccess::depth(g) == startDepth, "Levitating player should float over trap doors");
}

void test_lethe_mist_triggers_amnesia_and_forgets_far_memory() {
    Game g;
    g.newGame(1337u);

    // Ensure deterministic setup: remove any randomly spawned traps.
    GameTestAccess::traps(g).clear();

    Dungeon& d = GameTestAccess::dungeon(g);
    d.revealAll();
    GameTestAccess::recomputeFov(g);

    const Vec2i ppos = g.player().pos;

    auto hasEntityAt = [&](int x, int y) -> bool {
        for (const auto& e : GameTestAccess::ents(g)) {
            if (e.hp <= 0) continue;
            if (e.pos.x == x && e.pos.y == y) return true;
        }
        return false;
    };

    // Find a far, walkable, currently not-visible tile.
    Vec2i far{-1, -1};
    for (int y = 0; y < d.height && far.x < 0; ++y) {
        for (int x = 0; x < d.width && far.x < 0; ++x) {
            if (!d.isWalkable(x, y)) continue;
            if (hasEntityAt(x, y)) continue;
            if (d.at(x, y).visible) continue;
            if (chebyshev(ppos, {x, y}) <= 8) continue;
            far = {x, y};
        }
    }
    expect(far.x >= 0, "Found a far non-visible walkable tile for Lethe mist test");
    if (far.x < 0) return;

    // Find an adjacent walkable tile for a "remembered" marker/trap.
    Vec2i near = ppos;
    bool foundNear = false;
    for (int dy = -1; dy <= 1 && !foundNear; ++dy) {
        for (int dx = -1; dx <= 1 && !foundNear; ++dx) {
            if (dx == 0 && dy == 0) continue;
            Vec2i cand{ppos.x + dx, ppos.y + dy};
            if (!d.inBounds(cand.x, cand.y)) continue;
            if (!d.isWalkable(cand.x, cand.y)) continue;
            if (hasEntityAt(cand.x, cand.y)) continue;
            near = cand;
            foundNear = true;
        }
    }
    expect(foundNear, "Found an adjacent walkable tile for Lethe mist near-memory test");
    if (!foundNear) return;

    // Place discovered traps at far and near.
    GameTestAccess::traps(g).push_back({TrapKind::Spike, far, true});
    GameTestAccess::traps(g).push_back({TrapKind::Spike, near, true});

    // Place markers at far and near.
    g.setMarker(far, MarkerKind::Note, "FAR", false);
    g.setMarker(near, MarkerKind::Note, "NEAR", false);

    // Make an existing monster "alerted" at the far (unseen) position.
    Entity* m = nullptr;
    for (auto& e : GameTestAccess::ents(g)) {
        if (e.id == g.playerId()) continue;
        if (e.hp <= 0) continue;
        if (e.friendly) continue;
        m = &e;
        break;
    }
    expect(m != nullptr, "Found a monster entity for Lethe mist test");
    if (!m) return;
    m->pos = far;
    m->alerted = true;
    m->lastKnownPlayerPos = ppos;
    m->lastKnownPlayerAge = 1;

    // Add a Lethe mist trap under the player and trigger it.
    GameTestAccess::traps(g).push_back({TrapKind::LetheMist, ppos, false});

    expect(d.at(far.x, far.y).explored, "Precondition: far tile should start explored (after revealAll)");

    GameTestAccess::triggerTrapAt(g, ppos, g.playerMut());

    // Far tile should no longer be explored (and marker forgotten).
    expect(!d.at(far.x, far.y).explored, "Lethe mist should clear explored memory for far tiles");
    expect(g.markerAt(far) == nullptr, "Lethe mist should forget far map markers");

    // Far trap should be forgotten; near trap should remain discovered.
    bool farTrapForgotten = false;
    bool nearTrapStillKnown = false;
    for (const auto& tr : GameTestAccess::traps(g)) {
        if (tr.pos == far && tr.kind == TrapKind::Spike) farTrapForgotten = !tr.discovered;
        if (tr.pos == near && tr.kind == TrapKind::Spike) nearTrapStillKnown = tr.discovered;
    }
    expect(farTrapForgotten, "Lethe mist should clear discovered state for far traps");
    expect(nearTrapStillKnown, "Lethe mist should preserve discovered state for nearby traps");

    // Near marker should remain.
    expect(g.markerAt(near) != nullptr, "Lethe mist should preserve nearby markers");

    // Monster at far (unseen) should lose alert state and memory.
    expect(!m->alerted, "Lethe mist should calm unseen monsters (alerted false)");
    expect(m->lastKnownPlayerPos.x == -1 && m->lastKnownPlayerPos.y == -1,
           "Lethe mist should clear unseen monster lastKnownPlayerPos");
    expect(m->lastKnownPlayerAge >= 1000,
           "Lethe mist should set unseen monster lastKnownPlayerAge to a large value");
}


void test_poison_gas_trap_creates_persistent_cloud() {
    Game g;
    g.newGame(777u);

    // Find a walkable adjacent tile to place the trap on.
    const Vec2i p0 = g.player().pos;

    Vec2i target{-1, -1};
    Action moveAct = Action::Wait;

    struct Dir { int dx, dy; Action a; };
    const Dir dirs[] = {
        { 1, 0, Action::Right }, { -1, 0, Action::Left }, { 0, 1, Action::Down }, { 0, -1, Action::Up },
        { 1, 1, Action::DownRight }, { 1, -1, Action::UpRight }, { -1, 1, Action::DownLeft }, { -1, -1, Action::UpLeft },
    };

    for (const auto& d : dirs) {
        const Vec2i p{ p0.x + d.dx, p0.y + d.dy };
        if (!g.dungeon().inBounds(p.x, p.y)) continue;
        if (!g.dungeon().isWalkable(p.x, p.y)) continue;
        if (GameTestAccess::entityAt(g, p.x, p.y) != nullptr) continue;
        target = p;
        moveAct = d.a;
        break;
    }

    bool directTrigger = false;
    if (target.x == -1) {
        // Fallback: if player spawns boxed in (rare), trigger the trap directly on the
        // player's current tile so the test remains robust.
        target = p0;
        directTrigger = true;
    }


    // Place a poison gas trap and step onto it.
    {
        auto& traps = GameTestAccess::traps(g);
        traps.clear();

        Trap t;
        t.kind = TrapKind::PoisonGas;
        t.pos = target;
        t.discovered = false;
        traps.push_back(t);
    }

    const int hpBefore = g.player().hp;
    if (directTrigger) {
        GameTestAccess::triggerTrapAt(g, target, g.playerMut(), /*fromDisarm=*/false);
    } else {
        g.handleAction(moveAct);
    }

    expect(g.player().pos == target, "Player should have stepped onto poison gas trap tile.");
    expect(g.player().effects.poisonTurns > 0, "Poison gas trap should poison the victim.");
    expect(g.poisonGasAt(target.x, target.y) > 0u, "Poison gas trap should seed a persistent poison gas field.");
    expect(g.player().hp <= hpBefore, "Trap should not heal the player.");
}

void test_poison_gas_field_save_load_roundtrip() {
    Game g;
    g.newGame(2025u);

    const Dungeon& d = g.dungeon();
    const Vec2i p0 = g.player().pos;

    // Seed a poison gas tile directly (stable + deterministic for save/load).
    auto& pg = GameTestAccess::poisonGas(g);
    pg.assign(static_cast<size_t>(d.width * d.height), 0u);
    const size_t idx = static_cast<size_t>(p0.y * d.width + p0.x);
    pg[idx] = 42u;

    const std::string path = "test_poison_gas_roundtrip_tmp.sav";
    expect(g.saveToFile(path, /*quiet=*/true), "Saving should succeed");

    Game g2;
    expect(g2.loadFromFile(path), "Loading should succeed");

    expect(g2.poisonGasAt(p0.x, p0.y) == 42u, "Poison gas field should roundtrip through save/load.");
    std::remove(path.c_str());
}


void test_trap_door_monster_falls_to_next_depth_and_persists() {
    Game g;
    g.newGame(1337u);

    // Ensure deterministic setup: remove any randomly spawned traps.
    GameTestAccess::traps(g).clear();

    const int startDepth = GameTestAccess::depth(g);
    expect(startDepth < Game::DUNGEON_MAX_DEPTH, "Test requires a depth with a level below");

    const Dungeon& d = g.dungeon();
    const Vec2i ppos = g.player().pos;

    // Find a nearby walkable tile for the monster + trap.
    Vec2i trapPos = ppos;
    bool found = false;
    for (int dy = -1; dy <= 1 && !found; ++dy) {
        for (int dx = -1; dx <= 1 && !found; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const Vec2i cand{ppos.x + dx, ppos.y + dy};
            if (!d.inBounds(cand.x, cand.y)) continue;
            if (!d.isWalkable(cand.x, cand.y)) continue;
            trapPos = cand;
            found = true;
        }
    }
    expect(found, "Found a walkable adjacent tile for trap door test");
    if (!found) return;

    Entity m{};
    m.id = 424242;
    m.kind = EntityKind::Goblin;
    m.pos = trapPos;
    m.hp = 30;
    m.hpMax = 30;
    m.baseAtk = 1;
    m.baseDef = 0;
    m.spriteSeed = 1;

    GameTestAccess::ents(g).push_back(m);

    Trap tr;
    tr.pos = trapPos;
    tr.kind = TrapKind::TrapDoor;
    tr.discovered = false;
    GameTestAccess::traps(g).push_back(tr);

    // Trigger the trap under the monster.
    Entity& victim = GameTestAccess::ents(g).back();
    GameTestAccess::triggerTrapAt(g, trapPos, victim);

    // Mirror the end-of-turn cleanup behavior (trap door removes monster from this level).
    GameTestAccess::cleanupDead(g);

    // Now visit the level below; the falling monster should be spawned there.
    GameTestAccess::changeLevel(g, startDepth + 1, true);

    bool foundOnNext = false;
    for (const Entity& e : GameTestAccess::ents(g)) {
        if (e.id == 424242) {
            foundOnNext = true;
            expect(e.hp > 0, "Fallen monster should still be alive on the destination level");
            break;
        }
    }

    expect(foundOnNext, "Monster that fell through trap door should appear on the next depth");
}


void test_shrine_recharge_replenishes_wand_charges() {
    Game g;
    g.newGame(1337u);

    // Move the player to the shrine room.
    const Dungeon& d = g.dungeon();
    Vec2i shrinePos = g.player().pos;
    bool foundShrine = false;
    for (const auto& r : d.rooms) {
        if (r.type == RoomType::Shrine) {
            shrinePos = { r.cx(), r.cy() };
            foundShrine = true;
            break;
        }
    }
    expect(foundShrine, "Dungeon should contain a shrine room");
    if (!foundShrine) return;

    g.playerMut().pos = shrinePos;

    // Ensure plenty of gold for the service.
    bool foundGold = false;
    for (auto& it : GameTestAccess::inv(g)) {
        if (it.kind == ItemKind::Gold) {
            it.count = 999;
            foundGold = true;
        }
    }
    expect(foundGold, "Starting inventory should include gold");
    if (!foundGold) return;

    // Add a depleted wand.
    Item w;
    w.id = 999999;
    w.kind = ItemKind::WandFireball;
    w.count = 1;
    w.enchant = 0;
    w.buc = 0;
    w.charges = 0;
    w.shopPrice = 0;
    w.shopDepth = 0;
    w.spriteSeed = 1;
    GameTestAccess::inv(g).push_back(w);

    const int maxCharges = itemDef(ItemKind::WandFireball).maxCharges;
    expect(maxCharges > 0, "WandFireball should have maxCharges > 0");

    const bool ok = g.prayAtShrine("recharge");
    expect(ok, "prayAtShrine(recharge) should succeed");

    int foundCharges = -1;
    for (const auto& it : GameTestAccess::inv(g)) {
        if (it.kind == ItemKind::WandFireball) {
            foundCharges = it.charges;
            break;
        }
    }
    expect(foundCharges == maxCharges, "Shrine recharge should restore wand charges to max");
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


void test_discoveries_list_builds_and_sorts() {
    Game g;
    g.newGame(1337u);

    auto catOrder = [](ItemKind k) -> int {
        if (isPotionKind(k)) return 0;
        if (isScrollKind(k)) return 1;
        if (isRingKind(k)) return 2;
        if (isWandKind(k)) return 3;
        return 4;
    };

    // All identifiable items should be present.
    {
        std::vector<ItemKind> list;
        g.buildDiscoveryList(list, DiscoveryFilter::All, DiscoverySort::Appearance);

        int expected = 0;
        for (int i = 0; i < ITEM_KIND_COUNT; ++i) {
            const ItemKind k = static_cast<ItemKind>(i);
            if (isIdentifiableKind(k)) ++expected;
        }
        expect(static_cast<int>(list.size()) == expected, "Discovery list should include all identifiable item kinds");

        // Should be grouped by category (potions, scrolls, rings, wands) and sorted by appearance label within group.
        int prevCat = -1;
        std::string prevApp;
        for (ItemKind k : list) {
            const int c = catOrder(k);
            expect(c >= 0 && c <= 3, "Discovery list should not contain non-identifiable kinds");
            expect(c >= prevCat, "Discovery list should be grouped by category");

            const std::string app = g.discoveryAppearanceLabel(k);
            if (c == prevCat) {
                expect(app >= prevApp, "Discovery list should be sorted by appearance label within category");
            } else {
                prevApp = app;
                prevCat = c;
            }
        }
    }

    // Filtering should only return the chosen category.
    {
        std::vector<ItemKind> list;
        g.buildDiscoveryList(list, DiscoveryFilter::Potions, DiscoverySort::Appearance);
        for (ItemKind k : list) {
            expect(isPotionKind(k), "Potion filter should only include potions");
        }
    }

    // Identified-first sort should move identified entries to the top within a category.
    {
        GameTestAccess::markIdentified(g, ItemKind::PotionHealing, true);
        std::vector<ItemKind> list;
        g.buildDiscoveryList(list, DiscoveryFilter::Potions, DiscoverySort::IdentifiedFirst);
        expect(!list.empty(), "Potion discovery list should not be empty");
        expect(list[0] == ItemKind::PotionHealing, "Identified potion should appear first when sorting IdentifiedFirst");
    }
}



void test_auto_travel_plans_paths_through_known_traps_but_stops_before_them() {
    Game g;
    g.newGame(123u);

    // Remove all non-player entities so auto-move isn't interrupted by hostiles.
    {
        auto& ents = GameTestAccess::ents(g);
        const int pid = g.playerId();
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) { return e.id != pid; }), ents.end());
    }

    GameTestAccess::ground(g).clear();
    GameTestAccess::traps(g).clear();

    Dungeon& d = GameTestAccess::dungeon(g);

    // Start with an explored solid map, then carve a small corridor:
    //   S . T . G
    // where T is a *known* trap blocking the only route to the goal.
    for (int y = 0; y < Game::MAP_H; ++y) {
        for (int x = 0; x < Game::MAP_W; ++x) {
            d.at(x, y).type = TileType::Wall;
            d.at(x, y).explored = true;
            d.at(x, y).visible = false;
        }
    }

    const int y = 1;
    for (int x = 1; x <= 5; ++x) {
        d.at(x, y).type = TileType::Floor;
        d.at(x, y).explored = true;
    }

    // Place a discovered trap at (3,1).
    {
        Trap t;
        t.pos = Vec2i{3, 1};
        t.kind = TrapKind::Spike;
        t.discovered = true;
        GameTestAccess::traps(g).push_back(t);
    }

    g.playerMut().pos = Vec2i{1, 1};

    // Auto-travel should find a path to the goal even though a known trap blocks the only route.
    const bool ok = g.requestAutoTravel(Vec2i{5, 1});
    expect(ok, "Auto-travel should plan a path even if a known trap blocks the only route");

    // First step: move from S to the tile before the trap.
    const bool cont1 = GameTestAccess::stepAutoMove(g);
    expect(cont1, "Auto-move should take the first step toward the goal");
    expect(g.player().pos == Vec2i{2, 1}, "Auto-move should advance to (2,1)");

    // Second step: stop BEFORE stepping onto the known trap.
    const bool cont2 = GameTestAccess::stepAutoMove(g);
    expect(!cont2, "Auto-move should stop when the next step is a known trap");
    expect(g.player().pos == Vec2i{2, 1}, "Auto-move must not step onto a known trap tile");
    expect(!g.isAutoActive(), "Auto-move should be inactive after stopping at a known trap");
}

void test_auto_explore_guides_to_blocking_trap_when_frontier_is_behind_it() {
    Game g;
    g.newGame(456u);

    // Remove all non-player entities so exploration logic is deterministic.
    {
        auto& ents = GameTestAccess::ents(g);
        const int pid = g.playerId();
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) { return e.id != pid; }), ents.end());
    }

    GameTestAccess::ground(g).clear();
    GameTestAccess::traps(g).clear();

    Dungeon& d = GameTestAccess::dungeon(g);
    for (int y = 0; y < Game::MAP_H; ++y) {
        for (int x = 0; x < Game::MAP_W; ++x) {
            d.at(x, y).type = TileType::Wall;
            d.at(x, y).explored = true;
            d.at(x, y).visible = false;
        }
    }

    // Carve a corridor with a known trap in the middle. The ONLY unexplored tile is beyond the trap,
    // so the nearest frontier is unreachable without dealing with the trap.
    for (int x = 1; x <= 5; ++x) {
        d.at(x, 1).type = TileType::Floor;
        d.at(x, 1).explored = true;
    }
    d.at(5, 1).explored = false; // unexplored tile creates a frontier at (4,1) behind the trap

    {
        Trap t;
        t.pos = Vec2i{3, 1};
        t.kind = TrapKind::Spike;
        t.discovered = true;
        GameTestAccess::traps(g).push_back(t);
    }

    g.playerMut().pos = Vec2i{1, 1};

    const Vec2i goal = GameTestAccess::findNearestExploreFrontier(g);
    expect(goal == Vec2i{3, 1}, "When the nearest frontier is only reachable through a known trap, auto-explore should target the blocking trap");
}
int main() {
    std::cout << "Running ProcRogue tests...\n";

    test_rng_reproducible();
    test_dungeon_stairs_connected();
    test_vault_room_prefabs_add_obstacles();
    test_vault_suite_prefab_partitions_room();
    test_partition_vaults_embed_locked_door_in_wall_line();
    test_themed_room_prefabs_add_obstacles();
    test_room_shape_variety_adds_internal_walls();
    test_secret_shortcut_doors_generate();
    test_locked_shortcut_gates_generate();
    test_corridor_hubs_and_halls_generate();
    test_sinkholes_generate_deep_mines();
    test_dead_end_stash_closets_generate();
    test_special_rooms_paced_by_distance();
    test_rogue_level_layout();
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
    test_augury_preview_does_not_consume_rng();
    test_weighted_pathfinding_prefers_open_route_over_closed_door();
    test_auto_travel_plans_paths_through_known_traps_but_stops_before_them();
    test_auto_explore_guides_to_blocking_trap_when_frontier_is_behind_it();
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

    test_discoveries_list_builds_and_sorts();

    // Patch-specific regression tests.
    test_dungeon_digging();
    test_dig_prompt_digs_adjacent_wall_with_pickaxe();
    test_wand_display_shows_charges();
    test_shop_debt_ledger_persists_after_consumption();
    test_shop_debt_ledger_save_load_roundtrip();
    test_engravings_basic_and_save_load_roundtrip();
    test_command_prompt_preserves_look_cursor();
    test_throwvoice_alerts_monster_at_target_tile();
    test_listen_reports_hidden_monster_and_spends_turn();
    test_scroll_fear_applies_fear_to_visible_monsters();
    test_scroll_earth_raises_boulders_around_player();
    test_scroll_taming_charms_adjacent_non_undead_monsters();
    test_companion_fetch_carries_and_delivers_gold();
    test_monster_ai_avoids_fire_tiles_when_chasing();
    test_targeting_tab_cycles_visible_hostiles();
    test_boulder_blocks_projectiles_but_not_opaque();
    test_targeting_out_of_range_sets_status_text();
    test_save_load_fear_turns_roundtrip();
    test_bonus_cache_guards_spawn_adjacent_floor_trap();
    test_trap_door_drops_player_to_next_depth();
    test_levitation_skips_trap_door_trigger();
    test_lethe_mist_triggers_amnesia_and_forgets_far_memory();
    test_poison_gas_trap_creates_persistent_cloud();
    test_poison_gas_field_save_load_roundtrip();
    test_trap_door_monster_falls_to_next_depth_and_persists();
    test_shrine_recharge_replenishes_wand_charges();
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
