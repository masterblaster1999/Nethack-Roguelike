#include "dungeon.hpp"
#include "game.hpp"
#include "corridor_braid.hpp"
#include "terrain_sculpt.hpp"
#include "pathfinding.hpp"
#include "wfc.hpp"
#include "vault_prefab_catalog.hpp"
#include "proc_rd.hpp"
#include "poisson_disc.hpp"
#include "spatial_hash.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <queue>
#include <string>
#include <utility>

namespace {

// Forward declaration: some late procgen passes live earlier in this file
// than the generator-kind enumeration definition.
enum class GenKind : uint8_t;

struct Leaf {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int left = -1;
    int right = -1;
    int roomIndex = -1;
};

inline bool isLeaf(const Leaf& n) { return n.left < 0 && n.right < 0; }

int splitLeaf(const Leaf& n, bool splitH, RNG& rng, int minLeaf) {
    // Returns the split offset (in tiles) or -1 if the leaf is too small.
    if (splitH) {
        if (n.h < minLeaf * 2) return -1;
        return rng.range(minLeaf, n.h - minLeaf);
    }
    if (n.w < minLeaf * 2) return -1;
    return rng.range(minLeaf, n.w - minLeaf);
}

void fillWalls(Dungeon& d) {
    for (auto& t : d.tiles) {
        t.type = TileType::Wall;
        t.visible = false;
        t.explored = false;
    }
    d.rooms.clear();
    d.bonusLootSpots.clear();
    d.bonusItemSpawns.clear();
    d.stairsUp = {-1, -1};
    d.stairsDown = {-1, -1};
    d.hasCavernLake = false;
    d.hasWarrens = false;
    d.mazeAlgorithm = MazeAlgorithm::None;
    d.mazeChamberCount = 0;
    d.mazeBreakCount = 0;
    d.mazeWilsonWalkCount = 0;
    d.mazeWilsonStepCount = 0;
    d.mazeWilsonLoopEraseCount = 0;
    d.mazeWilsonMaxPathLen = 0;
    d.secretShortcutCount = 0;
    d.lockedShortcutCount = 0;
    d.corridorHubCount = 0;
    d.corridorHallCount = 0;
    d.sinkholeCount = 0;
    d.riftCacheCount = 0;
    d.riftCacheBoulderCount = 0;
    d.riftCacheChasmCount = 0;
    d.vaultSuiteCount = 0;
    d.vaultPrefabCount = 0;
    d.terrainSculptCount = 0;
    d.corridorBraidCount = 0;
    d.deadEndClosetCount = 0;
    d.annexCount = 0;
    d.annexKeyGateCount = 0;
    d.annexWfcCount = 0;
    d.annexFractalCount = 0;
    d.stairsBypassLoopCount = 0;
    d.stairsBridgeCount = 0;
    d.stairsRedundancyOk = false;
    d.globalBypassLoopCount = 0;
    d.globalBridgeCountBefore = 0;
    d.globalBridgeCountAfter = 0;
    d.genPickAttempts = 1;
    d.genPickChosenIndex = 0;
    d.genPickScore = 0;
    d.genPickSeed = 0;

    d.roomsGraphPoissonPointCount = 0;
    d.roomsGraphPoissonRoomCount = 0;
    d.roomsGraphDelaunayEdgeCount = 0;
    d.roomsGraphLoopEdgeCount = 0;

    d.biomeZoneCount = 0;
    d.biomePillarZoneCount = 0;
    d.biomeRubbleZoneCount = 0;
    d.biomeCrackedZoneCount = 0;
    d.biomeEdits = 0;

    d.fireLaneMaxBefore = 0;
    d.fireLaneMaxAfter = 0;
    d.fireLaneCoverCount = 0;
    d.fireLaneChicaneCount = 0;

    d.openSpaceClearanceMaxBefore = 0;
    d.openSpaceClearanceMaxAfter = 0;
    d.openSpacePillarCount = 0;
    d.openSpaceBoulderCount = 0;

    d.heightfieldRidgePillarCount = 0;
    d.heightfieldScreeBoulderCount = 0;


    d.fluvialGullyCount = 0;
    d.fluvialChasmCount = 0;
    d.fluvialCausewayCount = 0;


    d.perimTunnelCarvedTiles = 0;
    d.perimTunnelHatchCount = 0;
    d.perimTunnelLockedCount = 0;
    d.perimTunnelCacheCount = 0;

    d.crosscutTunnelCount = 0;
    d.crosscutCarvedTiles = 0;
    d.crosscutDoorLockedCount = 0;
    d.crosscutDoorSecretCount = 0;

    d.crawlspaceNetworkCount = 0;
    d.crawlspaceCarvedTiles = 0;
    d.crawlspaceDoorCount = 0;
    d.crawlspaceCacheCount = 0;

    d.moatedRoomCount = 0;
    d.moatedRoomBridgeCount = 0;
    d.moatedRoomChasmCount = 0;

    d.symmetryRoomCount = 0;
    d.symmetryObstacleCount = 0;

    d.spineRoomCount = 0;
    d.specialRoomMinSep = 0;
}

void carveRect(Dungeon& d, int x, int y, int w, int h, TileType type = TileType::Floor) {
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            if (!d.inBounds(xx, yy)) continue;
            d.at(xx, yy).type = type;
        }
    }
}

void carveFloor(Dungeon& d, int x, int y) {
    if (!d.inBounds(x, y)) return;
    Tile& t = d.at(x, y);
    // Don't overwrite doors or stairs.
    if (t.type == TileType::DoorClosed || t.type == TileType::DoorOpen || t.type == TileType::DoorSecret || t.type == TileType::DoorLocked ||
        t.type == TileType::StairsDown || t.type == TileType::StairsUp)
        return;
    // Don't overwrite special terrain features (they shape flow and/or are interactable).
    if (t.type == TileType::Chasm || t.type == TileType::Pillar || t.type == TileType::Boulder)
        return;
    t.type = TileType::Floor;
}

// Forward decls: secret/vault carving wants to reuse the shared door helpers.
inline bool isDoorTileType(TileType t);
bool anyDoorInRadius(const Dungeon& d, int x, int y, int radius);
const Room* findRoomContaining(const Dungeon& d, int x, int y);
bool stairsConnected(const Dungeon& d);

// Forward decls: bonus room decoration (vault/secret prefabs).
void decorateSecretBonusRoom(Dungeon& d, const Room& r, RNG& rng,
                            Vec2i doorPos, Vec2i doorInside, Vec2i intoDir,
                            int depth);
void decorateVaultBonusRoom(Dungeon& d, const Room& r, RNG& rng,
                           Vec2i doorPos, Vec2i doorInside, Vec2i intoDir,
                           int depth);

// ------------------------------------------------------------
// Secret rooms: optional side-rooms hidden behind secret doors.
// These do NOT affect critical connectivity (stairs remain reachable).
// ------------------------------------------------------------

bool tryCarveSecretRoom(Dungeon& d, RNG& rng, int depth) {
    // Pick a wall tile adjacent to floor, then carve a small room behind it.
    // Door stays hidden (TileType::DoorSecret) until discovered via searching.
    const int maxTries = 350;

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    for (int tries = 0; tries < maxTries; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;

        // Avoid making secret doors trivial/obvious:
        // - don't hug stairs
        // - don't cluster near other doors
        // - prefer "quiet" wall tiles that border exactly one floor tile
        auto tooCloseToStairs = [&](int tx, int ty) {
            const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
                ? (std::abs(tx - d.stairsUp.x) + std::abs(ty - d.stairsUp.y))
                : 9999;
            const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
                ? (std::abs(tx - d.stairsDown.x) + std::abs(ty - d.stairsDown.y))
                : 9999;
            return du <= 3 || dd <= 3;
        };
        if (tooCloseToStairs(x, y)) continue;
        if (anyDoorInRadius(d, x, y, 2)) continue;

        int adjFloors = 0;
        for (const Vec2i& dv : dirs) {
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Floor) adjFloors++;
        }
        if (adjFloors != 1) continue;

        // Randomize direction check order for variety.
        int start = rng.range(0, 3);
        for (int i = 0; i < 4; ++i) {
            const Vec2i dir = dirs[(start + i) % 4];

            const int fx = x + dir.x;
            const int fy = y + dir.y;
            if (!d.inBounds(fx, fy)) continue;
            if (d.at(fx, fy).type != TileType::Floor) continue; // must attach to existing floor

            // Avoid attaching a secret door directly into special rooms (shops/shrines/etc)
            // or other already-carved bonus rooms.
            if (const Room* rr = findRoomContaining(d, fx, fy)) {
                if (rr->type != RoomType::Normal) continue;
            }

            // Room extends opposite the floor neighbor.
            const int dx = -dir.x;
            const int dy = -dir.y;

            int maxS = 6;
            if (depth >= 5) maxS = 7;
            if (depth >= 8) maxS = 8;
            const int rw = rng.range(3, maxS);
            const int rh = rng.range(3, maxS);

            int rx = x;
            int ry = y;

            if (dx != 0) {
                // Door is on the left/right wall.
                rx = (dx > 0) ? x : (x - rw + 1);
                ry = y - rh / 2;
            } else {
                // Door is on the top/bottom wall.
                ry = (dy > 0) ? y : (y - rh + 1);
                rx = x - rw / 2;
            }

            // Keep a 1-tile margin for borders.
            if (rx < 1 || ry < 1 || (rx + rw) >= d.width - 1 || (ry + rh) >= d.height - 1) continue;

            // Validate that the room footprint is entirely solid wall (we don't want overlaps).
            bool ok = true;
            for (int yy = ry; yy < ry + rh && ok; ++yy) {
                for (int xx = rx; xx < rx + rw; ++xx) {
                    if (!d.inBounds(xx, yy)) { ok = false; break; }
                    if (d.at(xx, yy).type != TileType::Wall) { ok = false; break; }
                }
            }
            if (!ok) continue;

            // Carve room + place secret door.
            carveRect(d, rx, ry, rw, rh, TileType::Floor);
            d.at(x, y).type = TileType::DoorSecret;

            Room r;
            r.x = rx;
            r.y = ry;
            r.w = rw;
            r.h = rh;
            r.type = RoomType::Secret;

            // Round 21: add small interior "prefabs" to secret rooms (pillars/caches) so they
            // feel distinct from plain rectangles.
            decorateSecretBonusRoom(d, r, rng, {x, y}, {x + dx, y + dy}, {dx, dy}, depth);

            d.rooms.push_back(r);

            return true;
        }
    }

    return false;
}

// ------------------------------------------------------------
// Vault rooms: optional side-rooms behind *locked* doors.
// Doors are visible (TileType::DoorLocked) but require a Key to open.
// ------------------------------------------------------------


static bool tryPartitionVaultFromNormalRoom(Dungeon& d, RNG& rng, int depth) {
    (void)depth;

    // Partition-style vaults: split an existing normal room with a wall line and
    // carve a single locked door into that partition. This is far more reliable
    // than hunting for large solid wall blocks (which may be rare on dense layouts).
    const int minVaultFloor = 4; // walkable floor thickness (partition wall is an extra tile)
    const int minRemain = 5;

    std::vector<int> candidates;
    candidates.reserve(d.rooms.size());

    for (size_t i = 0; i < d.rooms.size(); ++i) {
        const Room& r = d.rooms[i];
        if (r.type != RoomType::Normal) continue;

        // Don't split rooms that contain stairs.
        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;

        // Need enough space to carve (vault floor + partition + remaining).
        if (r.w < (minVaultFloor + 1 + minRemain) && r.h < (minVaultFloor + 1 + minRemain)) continue;

        // Skip rooms already disrupted by global terrain passes (ravines/lakes),
        // since partitioning assumes a clean floor interior.
        bool clean = true;
        for (int y = r.y + 1; y < r.y2() - 1 && clean; ++y) {
            for (int x = r.x + 1; x < r.x2() - 1; ++x) {
                if (!d.inBounds(x, y)) { clean = false; break; }
                if (d.at(x, y).type != TileType::Floor) { clean = false; break; }
            }
        }
        if (!clean) continue;

        candidates.push_back(static_cast<int>(i));
    }

    if (candidates.empty()) return false;

    // Shuffle candidates for variety.
    for (int i = static_cast<int>(candidates.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(candidates[static_cast<size_t>(i)], candidates[static_cast<size_t>(j)]);
    }

    struct SplitOpt { bool vertical = true; bool vaultOnMin = true; }; // vaultOnMin: left/top if true.
    SplitOpt opts[4] = { {true,true}, {true,false}, {false,true}, {false,false} };

    auto shuffleOpts = [&]() {
        for (int i = 3; i > 0; --i) {
            const int j = rng.range(0, i);
            std::swap(opts[i], opts[j]);
        }
    };

    auto gatherBoundaryDoors = [&](const Room& r, std::vector<Vec2i>& out) {
        out.clear();
        out.reserve(static_cast<size_t>(r.w + r.h) * 2);

        auto consider = [&](int x, int y) {
            if (!d.inBounds(x, y)) return;
            if (!isDoorTileType(d.at(x, y).type)) return;
            out.push_back({x, y});
        };

        for (int x = r.x; x < r.x2(); ++x) {
            consider(x, r.y);
            consider(x, r.y2() - 1);
        }
        for (int y = r.y; y < r.y2(); ++y) {
            consider(r.x, y);
            consider(r.x2() - 1, y);
        }
    };

    const int maxRoomTries = std::min(12, static_cast<int>(candidates.size()));
    for (int attemptRoom = 0; attemptRoom < maxRoomTries; ++attemptRoom) {
        const int roomIdx = candidates[static_cast<size_t>(attemptRoom)];
        const Room orig = d.rooms[static_cast<size_t>(roomIdx)];

        std::vector<Vec2i> doors;
        gatherBoundaryDoors(orig, doors);
        if (doors.empty()) continue;

        shuffleOpts();

        for (const SplitOpt& opt : opts) {
            const bool vertical = opt.vertical;
            const bool vaultOnMin = opt.vaultOnMin;

            const int axisLen = vertical ? orig.w : orig.h;
            const int otherLen = vertical ? orig.h : orig.w;

            // Don't create "slit vaults".
            if (otherLen < 6) continue;

            const int maxVaultFloor = std::min(9, axisLen - (minRemain + 1));
            if (maxVaultFloor < minVaultFloor) continue;

            // Try vault sizes in random order (smaller sizes can dodge boundary doors).
            std::vector<int> sizes;
            sizes.reserve(static_cast<size_t>(maxVaultFloor - minVaultFloor + 1));
            for (int s = minVaultFloor; s <= maxVaultFloor; ++s) sizes.push_back(s);
            for (int i = static_cast<int>(sizes.size()) - 1; i > 0; --i) {
                const int j = rng.range(0, i);
                std::swap(sizes[static_cast<size_t>(i)], sizes[static_cast<size_t>(j)]);
            }

            for (int s = 0; s < static_cast<int>(sizes.size()); ++s) {
                const int vFloor = sizes[static_cast<size_t>(s)];

                Room vault;
                Room remain;
                Vec2i doorPos{0,0};
                Vec2i doorInside{0,0};
                Vec2i intoDir{0,0};

                if (vertical) {
                    if (vaultOnMin) {
                        // Vault on the left side.
                        const int wallX = orig.x + vFloor;

                        bool ok = true;
                        for (const Vec2i& dp : doors) {
                            if (dp.x <= wallX) { ok = false; break; }
                        }
                        if (!ok) continue;

                        vault = {orig.x, orig.y, vFloor + 1, orig.h, RoomType::Vault};
                        remain = {wallX + 1, orig.y, orig.w - (vFloor + 1), orig.h, RoomType::Normal};
                        if (remain.w < minRemain) continue;

                        intoDir = {-1, 0};

                        const int minY = orig.y + ((orig.h >= 7) ? 2 : 1);
                        const int maxY = orig.y2() - 1 - ((orig.h >= 7) ? 3 : 2);
                        if (minY > maxY) continue;
                        const int doorY = rng.range(minY, maxY);
                        doorPos = {wallX, doorY};
                        doorInside = {wallX - 1, doorY};
                    } else {
                        // Vault on the right side.
                        const int wallX = (orig.x2() - 1) - vFloor;

                        bool ok = true;
                        for (const Vec2i& dp : doors) {
                            if (dp.x >= wallX) { ok = false; break; }
                        }
                        if (!ok) continue;

                        vault = {wallX, orig.y, vFloor + 1, orig.h, RoomType::Vault};
                        remain = {orig.x, orig.y, wallX - orig.x, orig.h, RoomType::Normal};
                        if (remain.w < minRemain) continue;

                        intoDir = {1, 0};

                        const int minY = orig.y + ((orig.h >= 7) ? 2 : 1);
                        const int maxY = orig.y2() - 1 - ((orig.h >= 7) ? 3 : 2);
                        if (minY > maxY) continue;
                        const int doorY = rng.range(minY, maxY);
                        doorPos = {wallX, doorY};
                        doorInside = {wallX + 1, doorY};
                    }

                    // Validate doorway tiles before building the partition.
                    if (!d.inBounds(doorPos.x, doorPos.y)) continue;
                    if (!d.inBounds(doorInside.x, doorInside.y)) continue;
                    const Vec2i outside = {doorPos.x - intoDir.x, doorPos.y - intoDir.y};
                    if (!d.inBounds(outside.x, outside.y)) continue;

                    if (d.at(doorPos.x, doorPos.y).type != TileType::Floor) continue;
                    if (d.at(doorInside.x, doorInside.y).type != TileType::Floor) continue;
                    if (d.at(outside.x, outside.y).type != TileType::Floor) continue;
                    if (anyDoorInRadius(d, doorPos.x, doorPos.y, 1)) continue;

                    struct LocalChange { int x; int y; TileType prev; };
                    std::vector<LocalChange> changes;
                    changes.reserve(static_cast<size_t>(orig.h + 2));

                    auto setTile = [&](int x, int y, TileType t) {
                        if (!d.inBounds(x, y)) return;
                        Tile& cur = d.at(x, y);
                        if (cur.type == t) return;
                        changes.push_back({x, y, cur.type});
                        cur.type = t;
                    };

                    // Full-height partition wall.
                    for (int y = orig.y; y < orig.y2(); ++y) {
                        setTile(doorPos.x, y, TileType::Wall);
                    }
                    setTile(doorPos.x, doorPos.y, TileType::DoorLocked);

                    const Room saved = d.rooms[static_cast<size_t>(roomIdx)];
                    const size_t savedCount = d.rooms.size();
                    d.rooms[static_cast<size_t>(roomIdx)] = remain;
                    d.rooms.push_back(vault);

                    if (!stairsConnected(d)) {
                        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
                            if (d.inBounds(it->x, it->y)) d.at(it->x, it->y).type = it->prev;
                        }
                        d.rooms.resize(savedCount);
                        d.rooms[static_cast<size_t>(roomIdx)] = saved;
                        continue;
                    }

                    decorateVaultBonusRoom(d, vault, rng, doorPos, doorInside, intoDir, depth);
                    return true;
                } else {
                    if (vaultOnMin) {
                        // Vault on the top side.
                        const int wallY = orig.y + vFloor;

                        bool ok = true;
                        for (const Vec2i& dp : doors) {
                            if (dp.y <= wallY) { ok = false; break; }
                        }
                        if (!ok) continue;

                        vault = {orig.x, orig.y, orig.w, vFloor + 1, RoomType::Vault};
                        remain = {orig.x, wallY + 1, orig.w, orig.h - (vFloor + 1), RoomType::Normal};
                        if (remain.h < minRemain) continue;

                        intoDir = {0, -1};

                        const int minX = orig.x + ((orig.w >= 7) ? 2 : 1);
                        const int maxX = orig.x2() - 1 - ((orig.w >= 7) ? 3 : 2);
                        if (minX > maxX) continue;
                        const int doorX = rng.range(minX, maxX);
                        doorPos = {doorX, wallY};
                        doorInside = {doorX, wallY - 1};
                    } else {
                        // Vault on the bottom side.
                        const int wallY = (orig.y2() - 1) - vFloor;

                        bool ok = true;
                        for (const Vec2i& dp : doors) {
                            if (dp.y >= wallY) { ok = false; break; }
                        }
                        if (!ok) continue;

                        vault = {orig.x, wallY, orig.w, vFloor + 1, RoomType::Vault};
                        remain = {orig.x, orig.y, orig.w, wallY - orig.y, RoomType::Normal};
                        if (remain.h < minRemain) continue;

                        intoDir = {0, 1};

                        const int minX = orig.x + ((orig.w >= 7) ? 2 : 1);
                        const int maxX = orig.x2() - 1 - ((orig.w >= 7) ? 3 : 2);
                        if (minX > maxX) continue;
                        const int doorX = rng.range(minX, maxX);
                        doorPos = {doorX, wallY};
                        doorInside = {doorX, wallY + 1};
                    }

                    if (!d.inBounds(doorPos.x, doorPos.y)) continue;
                    if (!d.inBounds(doorInside.x, doorInside.y)) continue;
                    const Vec2i outside = {doorPos.x - intoDir.x, doorPos.y - intoDir.y};
                    if (!d.inBounds(outside.x, outside.y)) continue;

                    if (d.at(doorPos.x, doorPos.y).type != TileType::Floor) continue;
                    if (d.at(doorInside.x, doorInside.y).type != TileType::Floor) continue;
                    if (d.at(outside.x, outside.y).type != TileType::Floor) continue;
                    if (anyDoorInRadius(d, doorPos.x, doorPos.y, 1)) continue;

                    struct LocalChange { int x; int y; TileType prev; };
                    std::vector<LocalChange> changes;
                    changes.reserve(static_cast<size_t>(orig.w + 2));

                    auto setTile = [&](int x, int y, TileType t) {
                        if (!d.inBounds(x, y)) return;
                        Tile& cur = d.at(x, y);
                        if (cur.type == t) return;
                        changes.push_back({x, y, cur.type});
                        cur.type = t;
                    };

                    for (int x = orig.x; x < orig.x2(); ++x) {
                        setTile(x, doorPos.y, TileType::Wall);
                    }
                    setTile(doorPos.x, doorPos.y, TileType::DoorLocked);

                    const Room saved = d.rooms[static_cast<size_t>(roomIdx)];
                    const size_t savedCount = d.rooms.size();
                    d.rooms[static_cast<size_t>(roomIdx)] = remain;
                    d.rooms.push_back(vault);

                    if (!stairsConnected(d)) {
                        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
                            if (d.inBounds(it->x, it->y)) d.at(it->x, it->y).type = it->prev;
                        }
                        d.rooms.resize(savedCount);
                        d.rooms[static_cast<size_t>(roomIdx)] = saved;
                        continue;
                    }

                    decorateVaultBonusRoom(d, vault, rng, doorPos, doorInside, intoDir, depth);
                    return true;
                }
            }
        }
    }

    return false;
}

bool tryCarveVaultRoom(Dungeon& d, RNG& rng, int depth) {
    // Prefer partition-vaults carved out of existing normal rooms.
    // This is far more reliable on dense layouts than carving into solid wall blocks.
    if (tryPartitionVaultFromNormalRoom(d, rng, depth)) return true;

    const int maxTries = 350;

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    for (int tries = 0; tries < maxTries; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;

        // Avoid placing vault entrances right next to stairs or clustered with
        // other doors. Prefer walls that border exactly one floor tile so the
        // vault reads as a discrete "side door".
        auto tooCloseToStairs = [&](int tx, int ty) {
            const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
                ? (std::abs(tx - d.stairsUp.x) + std::abs(ty - d.stairsUp.y))
                : 9999;
            const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
                ? (std::abs(tx - d.stairsDown.x) + std::abs(ty - d.stairsDown.y))
                : 9999;
            return du <= 3 || dd <= 3;
        };
        if (tooCloseToStairs(x, y)) continue;
        if (anyDoorInRadius(d, x, y, 2)) continue;

        int adjFloors = 0;
        for (const Vec2i& dv : dirs) {
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Floor) adjFloors++;
        }
        if (adjFloors != 1) continue;

        // Randomize direction check order for variety.
        int start = rng.range(0, 3);
        for (int i = 0; i < 4; ++i) {
            const Vec2i dir = dirs[(start + i) % 4];

            const int fx = x + dir.x;
            const int fy = y + dir.y;
            if (!d.inBounds(fx, fy)) continue;
            if (d.at(fx, fy).type != TileType::Floor) continue; // must attach to existing floor

            // Avoid vault doors opening straight into special rooms/bonus rooms.
            if (const Room* rr = findRoomContaining(d, fx, fy)) {
                if (rr->type != RoomType::Normal) continue;
            }

            // Room extends opposite the floor neighbor.
            const int dx = -dir.x;
            const int dy = -dir.y;

            // Vaults are a bit larger than secrets; they should feel like a "real" reward.
            // Scale size gently with depth so deeper floors can host more interesting layouts (moats, trenches, etc.).
            int minS = 4;
            int maxS = 7;
            if (depth >= 5) maxS = 8;
            if (depth >= 7) maxS = 9;
            if (depth >= 9) minS = 5;
            const int rw = rng.range(minS, maxS);
            const int rh = rng.range(minS, maxS);

            int rx = x;
            int ry = y;

            if (dx != 0) {
                // Door is on the left/right wall.
                rx = (dx > 0) ? x : (x - rw + 1);
                ry = y - rh / 2;
            } else {
                // Door is on the top/bottom wall.
                ry = (dy > 0) ? y : (y - rh + 1);
                rx = x - rw / 2;
            }

            // Keep a 1-tile margin for borders.
            if (rx < 1 || ry < 1 || (rx + rw) >= d.width - 1 || (ry + rh) >= d.height - 1) continue;

            // Validate that the room footprint is entirely solid wall (no overlaps).
            bool ok = true;
            for (int yy = ry; yy < ry + rh && ok; ++yy) {
                for (int xx = rx; xx < rx + rw; ++xx) {
                    if (!d.inBounds(xx, yy)) { ok = false; break; }
                    if (d.at(xx, yy).type != TileType::Wall) { ok = false; break; }
                }
            }
            if (!ok) continue;

            // Carve room + place locked door.
            carveRect(d, rx, ry, rw, rh, TileType::Floor);
            d.at(x, y).type = TileType::DoorLocked;

            Room r;
            r.x = rx;
            r.y = ry;
            r.w = rw;
            r.h = rh;
            r.type = RoomType::Vault;

            // Round 21: vaults get bespoke interior layouts (moats/trenches/pillar grids) and
            // can request additional "bonus" loot caches in hard-to-reach pockets.
            decorateVaultBonusRoom(d, r, rng, {x, y}, {x + dx, y + dy}, {dx, dy}, depth);

            d.rooms.push_back(r);

            return true;
        }
    }

    return false;
}

void carveH(Dungeon& d, int x1, int x2, int y) {
    if (x2 < x1) std::swap(x1, x2);
    for (int x = x1; x <= x2; ++x) carveFloor(d, x, y);
}

void carveV(Dungeon& d, int y1, int y2, int x) {
    if (y2 < y1) std::swap(y1, y2);
    for (int y = y1; y <= y2; ++y) carveFloor(d, x, y);
}

std::vector<int> collectRoomsInSubtree(const std::vector<Leaf>& nodes, int idx) {
    std::vector<int> out;
    if (idx < 0) return out;
    const Leaf& n = nodes[static_cast<size_t>(idx)];
    if (n.roomIndex >= 0) out.push_back(n.roomIndex);
    if (n.left >= 0) {
        auto v = collectRoomsInSubtree(nodes, n.left);
        out.insert(out.end(), v.begin(), v.end());
    }
    if (n.right >= 0) {
        auto v = collectRoomsInSubtree(nodes, n.right);
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

int pickRandomRoomInSubtree(const std::vector<Leaf>& nodes, int idx, RNG& rng) {
    auto rooms = collectRoomsInSubtree(nodes, idx);
    if (rooms.empty()) return -1;
    return rooms[static_cast<size_t>(rng.range(0, static_cast<int>(rooms.size()) - 1))];
}

struct DoorPick {
    Vec2i doorInside;
    Vec2i corridorStart;
};

inline bool isDoorTileType(TileType t) {
    switch (t) {
        case TileType::DoorClosed:
        case TileType::DoorOpen:
        case TileType::DoorLocked:
        case TileType::DoorSecret:
            return true;
        default:
            return false;
    }
}

inline bool isWallLikeForDoor(TileType t) {
    // What counts as a "solid" boundary for a corridor chokepoint door.
    // Note: Chasm is impassable but does not behave like a wall visually/for LOS.
    return (t == TileType::Wall || t == TileType::Pillar || t == TileType::Boulder);
}

inline bool isOpenForDoorGeom(TileType t) {
    // What counts as an "open" tile when deciding whether a corridor segment is a valid
    // door chokepoint.
    return (t == TileType::Floor || t == TileType::StairsUp || t == TileType::StairsDown || t == TileType::DoorOpen);
}

bool anyDoorInRadius(const Dungeon& d, int x, int y, int radius) {
    radius = std::max(1, radius);
    for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int nx = x + ox;
            const int ny = y + oy;
            if (!d.inBounds(nx, ny)) continue;
            if (isDoorTileType(d.at(nx, ny).type)) return true;
        }
    }
    return false;
}

const Room* findRoomContaining(const Dungeon& d, int x, int y) {
    for (const Room& rr : d.rooms) {
        if (rr.contains(x, y)) return &rr;
    }
    return nullptr;
}

bool isCorridorDoorCandidate(const Dungeon& d, int x, int y) {
    if (!d.inBounds(x, y)) return false;
    if (d.at(x, y).type != TileType::Floor) return false;

    const TileType n = d.at(x, y - 1).type;
    const TileType s = d.at(x, y + 1).type;
    const TileType w = d.at(x - 1, y).type;
    const TileType e = d.at(x + 1, y).type;

    const bool nOpen = isOpenForDoorGeom(n);
    const bool sOpen = isOpenForDoorGeom(s);
    const bool wOpen = isOpenForDoorGeom(w);
    const bool eOpen = isOpenForDoorGeom(e);

    const int openCount = static_cast<int>(nOpen) + static_cast<int>(sOpen) + static_cast<int>(wOpen) + static_cast<int>(eOpen);
    if (openCount != 2) return false;

    // We only allow straight chokepoints (no corners/intersections).
    const bool nsStraight = (nOpen && sOpen && !wOpen && !eOpen);
    const bool weStraight = (wOpen && eOpen && !nOpen && !sOpen);
    if (!(nsStraight || weStraight)) return false;

    // Require walls (or wall-like obstacles) on the perpendicular sides.
    if (nsStraight) {
        if (!isWallLikeForDoor(w) || !isWallLikeForDoor(e)) return false;
    } else {
        if (!isWallLikeForDoor(n) || !isWallLikeForDoor(s)) return false;
    }

    // Never place doors adjacent to any other door.
    if (anyDoorInRadius(d, x, y, 1)) return false;

    return true;
}

// ------------------------------------------------------------
// Strategic corridor doors: rather than sprinkling doors randomly,
// analyze the corridor graph and place doors in the *middle* of long,
// straight hallway segments (between intersections).
//
// This avoids "door spam" on large maps and produces more readable,
// intentional chokepoints.
// ------------------------------------------------------------

void placeStrategicCorridorDoors(Dungeon& d, RNG& rng, const std::vector<uint8_t>& inRoom, float intensity,
                                 const std::function<bool(int,int)>& extraReject = {}) {
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    auto isInRoom = [&](int x, int y) -> bool {
        if (inRoom.empty()) return false;
        const size_t ii = idx(x, y);
        if (ii >= inRoom.size()) return false;
        return inRoom[ii] != 0;
    };

    auto isCorridorFloor = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (extraReject && extraReject(x, y)) return false;
        if (isInRoom(x, y)) return false;
        const TileType t = d.at(x, y).type;
        if (t != TileType::Floor) return false;
        return true;
    };

    auto degree = [&](int x, int y) -> int {
        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        int c = 0;
        for (auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (isCorridorFloor(nx, ny)) c++;
        }
        return c;
    };

    auto nearStairs = [&](int x, int y) {
        return (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y) <= 2)
            || (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y) <= 2);
    };

    // Scale intensity down on maps larger than our default baseline.
    // This keeps corridor-door density sane if we later experiment with even bigger levels.
    const float baseArea = static_cast<float>(Dungeon::DEFAULT_W) * static_cast<float>(Dungeon::DEFAULT_H);
    const float area = static_cast<float>(std::max(1, d.width * d.height));
    const float areaScale = std::clamp(baseArea / area, 0.40f, 1.00f);
    const float k = std::clamp(intensity * areaScale, 0.0f, 2.0f);

    // Hard cap for additional corridor doors (room-connection doors are placed elsewhere).
    const int maxDoors = std::max(4, (d.width * d.height) / 300);
    int placed = 0;

    std::vector<uint8_t> visited(static_cast<size_t>(d.width * d.height), uint8_t{0});

    auto tryPlaceOnSegment = [&](const std::vector<Vec2i>& seg) {
        if (placed >= maxDoors) return;
        const int L = static_cast<int>(seg.size());
        if (L < 6) return;

        // Base chance that *this* segment gets at least one door.
        float p = 0.0f;
        if (L >= 18) p = 0.90f;
        else if (L >= 14) p = 0.75f;
        else if (L >= 10) p = 0.55f;
        else p = 0.35f;
        p *= k;
        p = std::min(0.95f, p);
        if (!rng.chance(p)) return;

        // Long, straight corridors benefit from *multiple* LOS breakers.
        // We keep it rare and only for genuinely long hallway segments.
        int wantDoors = 1;
        if (L >= 34) {
            float p2 = 0.0f;
            if (L >= 52) p2 = 0.65f;
            else p2 = 0.45f;
            p2 *= k;
            p2 = std::min(0.80f, p2);
            if (rng.chance(p2)) wantDoors = 2;
        }
        if (placed + wantDoors > maxDoors) wantDoors = std::max(1, maxDoors - placed);

        // Never place right next to an endpoint.
        const int margin = (wantDoors >= 2) ? 3 : 2;
        const int lo = margin;
        const int hi = L - 1 - margin;
        if (lo >= hi) return;

        auto ok = [&](const Vec2i& p0) -> bool {
            if (!d.inBounds(p0.x, p0.y)) return false;
            if (!isCorridorFloor(p0.x, p0.y)) return false;
            if (nearStairs(p0.x, p0.y)) return false;
            // Keep doors away from other doors (including room doors).
            if (anyDoorInRadius(d, p0.x, p0.y, 2)) return false;
            if (!isCorridorDoorCandidate(d, p0.x, p0.y)) return false;
            return true;
        };

        auto placeNearIndex = [&](int targetIdx) -> bool {
            // Search outward from the target index. (Segment tiles are already in order.)
            for (int off = 0; off <= (hi - lo); ++off) {
                const int a = targetIdx - off;
                const int b = targetIdx + off;
                if (a >= lo && a <= hi) {
                    if (ok(seg[static_cast<size_t>(a)])) {
                        d.at(seg[static_cast<size_t>(a)].x, seg[static_cast<size_t>(a)].y).type = TileType::DoorClosed;
                        placed++;
                        return true;
                    }
                }
                if (b >= lo && b <= hi && b != a) {
                    if (ok(seg[static_cast<size_t>(b)])) {
                        d.at(seg[static_cast<size_t>(b)].x, seg[static_cast<size_t>(b)].y).type = TileType::DoorClosed;
                        placed++;
                        return true;
                    }
                }
            }
            return false;
        };

        // Door targets:
        // - 1 door: center
        // - 2 doors: ~1/3 and ~2/3 of the segment (better spacing than center+quarter)
        std::vector<int> targets;
        targets.reserve(static_cast<size_t>(wantDoors));
        if (wantDoors == 1) {
            targets.push_back(L / 2);
        } else {
            targets.push_back(L / 3);
            targets.push_back((2 * L) / 3);
            // Randomize placement order so one bad target doesn't always dominate.
            if (rng.chance(0.5f)) std::swap(targets[0], targets[1]);
        }

        for (int ti : targets) {
            if (placed >= maxDoors) break;
            (void)placeNearIndex(ti);
        }
    };

    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    // 1) Walk segments that originate at a "node" (degree != 2).
    for (int y = 1; y < d.height - 1 && placed < maxDoors; ++y) {
        for (int x = 1; x < d.width - 1 && placed < maxDoors; ++x) {
            if (!isCorridorFloor(x, y)) continue;
            const int deg0 = degree(x, y);
            if (deg0 == 2) continue; // not a node

            for (auto& dv : dirs) {
                const int nx = x + dv[0];
                const int ny = y + dv[1];
                if (!isCorridorFloor(nx, ny)) continue;

                // If the first step is an internal corridor tile we've already consumed,
                // this segment has already been processed from the other side.
                if (degree(nx, ny) == 2 && visited[idx(nx, ny)] != 0) continue;

                std::vector<Vec2i> seg;
                seg.reserve(64);

                Vec2i prev{x, y};
                Vec2i cur{nx, ny};

                // Traverse until we hit another node (degree != 2) or stop.
                while (true) {
                    if (!d.inBounds(cur.x, cur.y)) break;
                    seg.push_back(cur);

                    const int cd = degree(cur.x, cur.y);
                    if (cd != 2) break;
                    visited[idx(cur.x, cur.y)] = 1;

                    // Pick the next tile that isn't "prev".
                    Vec2i next = prev;
                    bool found = false;
                    for (auto& dv2 : dirs) {
                        const int tx = cur.x + dv2[0];
                        const int ty = cur.y + dv2[1];
                        if (!isCorridorFloor(tx, ty)) continue;
                        if (tx == prev.x && ty == prev.y) continue;
                        next = {tx, ty};
                        found = true;
                        break;
                    }
                    if (!found) break;
                    prev = cur;
                    cur = next;

                    // Safety: avoid pathological infinite loops.
                    if (seg.size() > static_cast<size_t>(d.width * d.height)) break;
                }

                tryPlaceOnSegment(seg);
                if (placed >= maxDoors) break;
            }
        }
    }

    // 2) Handle pure cycles (no nodes): any remaining unvisited degree==2 tile belongs to a loop.
    for (int y = 1; y < d.height - 1 && placed < maxDoors; ++y) {
        for (int x = 1; x < d.width - 1 && placed < maxDoors; ++x) {
            if (!isCorridorFloor(x, y)) continue;
            if (degree(x, y) != 2) continue;
            if (visited[idx(x, y)] != 0) continue;

            // Find one neighbor to start walking the loop.
            Vec2i start{x, y};
            Vec2i prev{x, y};
            Vec2i cur{-1, -1};
            for (auto& dv : dirs) {
                const int nx = x + dv[0];
                const int ny = y + dv[1];
                if (isCorridorFloor(nx, ny)) { cur = {nx, ny}; break; }
            }
            if (cur.x < 0) { visited[idx(x, y)] = 1; continue; }

            std::vector<Vec2i> seg;
            seg.reserve(96);
            seg.push_back(start);
            visited[idx(start.x, start.y)] = 1;

            while (cur.x != start.x || cur.y != start.y) {
                seg.push_back(cur);
                visited[idx(cur.x, cur.y)] = 1;

                Vec2i next = prev;
                bool found = false;
                for (auto& dv2 : dirs) {
                    const int tx = cur.x + dv2[0];
                    const int ty = cur.y + dv2[1];
                    if (!isCorridorFloor(tx, ty)) continue;
                    if (tx == prev.x && ty == prev.y) continue;
                    next = {tx, ty};
                    found = true;
                    break;
                }
                if (!found) break;
                prev = cur;
                cur = next;

                if (seg.size() > static_cast<size_t>(d.width * d.height)) break;
            }

            tryPlaceOnSegment(seg);
        }
    }
}

DoorPick pickDoorOnRoomRandom(const Room& r, const Dungeon& d, RNG& rng) {
    // Legacy behavior (kept as a fallback): pick a random side and a random offset.
    // This is fast, but can create awkward corridors on larger maps.
    for (int tries = 0; tries < 20; ++tries) {
        const int side = rng.range(0, 3);
        Vec2i door{ r.cx(), r.cy() };
        Vec2i out{ r.cx(), r.cy() };

        if (side == 0) { // north
            door.x = rng.range(r.x + 1, r.x + r.w - 2);
            door.y = r.y;
            out = { door.x, door.y - 1 };
        } else if (side == 1) { // south
            door.x = rng.range(r.x + 1, r.x + r.w - 2);
            door.y = r.y + r.h - 1;
            out = { door.x, door.y + 1 };
        } else if (side == 2) { // west
            door.x = r.x;
            door.y = rng.range(r.y + 1, r.y + r.h - 2);
            out = { door.x - 1, door.y };
        } else { // east
            door.x = r.x + r.w - 1;
            door.y = rng.range(r.y + 1, r.y + r.h - 2);
            out = { door.x + 1, door.y };
        }

        if (d.inBounds(out.x, out.y) && d.inBounds(door.x, door.y)) {
            return { door, out };
        }
    }

    // Fallback: center-ish.
    Vec2i door{ r.cx(), r.cy() };
    Vec2i out{ r.cx(), r.cy() + 1 };
    if (!d.inBounds(out.x, out.y)) out = { r.cx(), r.cy() - 1 };
    if (!d.inBounds(out.x, out.y)) out = { r.cx() + 1, r.cy() };
    if (!d.inBounds(out.x, out.y)) out = { r.cx() - 1, r.cy() };
    return { door, out };
}

DoorPick pickDoorOnRoomSmart(const Room& r, const Dungeon& d, RNG& rng, Vec2i target, const Room* selfRoom) {
    struct Cand {
        Vec2i door;
        Vec2i out;
        int score = 0;
    };

    std::vector<Cand> cands;
    cands.reserve(static_cast<size_t>(std::max(8, (r.w + r.h) * 2)));

    // Preferred side based on where the target room is.
    const int dx = target.x - r.cx();
    const int dy = target.y - r.cy();
    int prefSide = 0; // 0=N, 1=S, 2=W, 3=E
    if (std::abs(dx) >= std::abs(dy)) prefSide = (dx >= 0) ? 3 : 2;
    else prefSide = (dy >= 0) ? 1 : 0;

    auto opposite = [](int side) -> int {
        if (side == 0) return 1;
        if (side == 1) return 0;
        if (side == 2) return 3;
        return 2;
    };

    auto consider = [&](int side, int doorX, int doorY, int outX, int outY) {
        if (!d.inBounds(doorX, doorY) || !d.inBounds(outX, outY)) return;

        const TileType dt = d.at(doorX, doorY).type;
        // Don't trample special content.
        if (!(dt == TileType::Floor || dt == TileType::DoorClosed || dt == TileType::DoorOpen)) return;

        const TileType ot = d.at(outX, outY).type;
        if (isDoorTileType(ot) || ot == TileType::StairsUp || ot == TileType::StairsDown) return;

        // Out tile should be something a corridor can sensibly occupy / carve into.
        // Avoid carving into chasms/pillars/boulders and avoid routing corridors through any room interiors.
        if (!(ot == TileType::Wall || ot == TileType::Floor)) return;
        if (findRoomContaining(d, outX, outY) != nullptr) return;

        // Avoid clustering doors.
        if (anyDoorInRadius(d, doorX, doorY, 1)) return;

        // Score: prefer facing the target, prefer carving into solid wall, prefer shorter corridors.
        int score = 0;
        if (side == prefSide) score += 35;
        else if (side == opposite(prefSide)) score -= 10;

        const int dist = std::abs(outX - target.x) + std::abs(outY - target.y);
        score -= dist;

        if (ot == TileType::Wall) score += 40;
        else if (ot == TileType::Floor) score -= 8; // likely already a corridor; still ok
        else score -= 20; // unusual (chasm/pillar/boulder)

        // Penalize doors that open directly into another room interior.
        if (const Room* rr = findRoomContaining(d, outX, outY)) {
            if (rr != selfRoom) score -= 45;
        }

        // Tiny jitter so ties don't always pick the same spot.
        score += rng.range(-2, 2);

        cands.push_back({{doorX, doorY}, {outX, outY}, score});
    };

    // Enumerate candidates on each wall (excluding corners).
    for (int x = r.x + 1; x <= r.x2() - 2; ++x) {
        consider(0, x, r.y, x, r.y - 1);           // N
        consider(1, x, r.y2() - 1, x, r.y2());     // S
    }
    for (int y = r.y + 1; y <= r.y2() - 2; ++y) {
        consider(2, r.x, y, r.x - 1, y);           // W
        consider(3, r.x2() - 1, y, r.x2(), y);     // E
    }

    if (cands.empty()) {
        return pickDoorOnRoomRandom(r, d, rng);
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.score > b.score;
    });

    // Pick randomly among the top few to keep layouts varied.
    const int topN = std::min(4, static_cast<int>(cands.size()));
    const int pick = rng.range(0, topN - 1);
    return { cands[static_cast<size_t>(pick)].door, cands[static_cast<size_t>(pick)].out };
}


// ------------------------------------------------------------
// Corridor routing: A* tunneling that tries hard to avoid carving
// through other rooms (which creates ugly "room cuts" and door-less
// openings), while still producing reasonably short, mostly-straight
// hallways on larger maps.
// ------------------------------------------------------------

struct AStarEntry {
    int f = 0;
    int g = 0;
    int state = 0;
};

struct AStarEntryCmp {
    bool operator()(const AStarEntry& a, const AStarEntry& b) const {
        return a.f > b.f;
    }
};

inline bool corridorTileOk(TileType t) {
    return (t == TileType::Wall || t == TileType::Floor);
}

inline int corridorStepCost(TileType t) {
    // Slightly prefer reusing existing corridors over digging new ones.
    if (t == TileType::Floor) return 9;
    return 10; // Wall
}

bool carveCorridorAStar(Dungeon& d, RNG& rng, Vec2i start, Vec2i goal, const std::vector<uint8_t>& roomMask) {
    const int W = d.width;
    const int H = d.height;

    auto inside = [&](int x, int y) {
        return x >= 1 && y >= 1 && x < W - 1 && y < H - 1;
    };

    if (!inside(start.x, start.y) || !inside(goal.x, goal.y)) return false;

    auto idx = [&](int x, int y) { return y * W + x; };

    const int DIR_NONE = 4;
    const int dirs[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    // Shuffle direction order once per corridor to vary shapes without consuming lots of RNG.
    int order[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; --i) {
        int j = rng.range(0, i);
        std::swap(order[i], order[j]);
    }

    auto inRoom = [&](int x, int y) -> bool {
        if (roomMask.empty()) return false;
        const size_t ii = static_cast<size_t>(idx(x, y));
        if (ii >= roomMask.size()) return false;
        return roomMask[ii] != 0;
    };

    auto nearRoomPenalty = [&](int x, int y) -> int {
        // Small penalty for hugging rooms (keeps corridors from "skimming" rooms and creating
        // accidental extra entrances).
        if (roomMask.empty()) return 0;
        for (int k = 0; k < 4; ++k) {
            int nx = x + dirs[k][0];
            int ny = y + dirs[k][1];
            if (!inside(nx, ny)) continue;
            if (inRoom(nx, ny)) return 2;
        }
        return 0;
    };

    auto heuristic = [&](int x, int y) {
        // Manhattan distance; scale close to step costs.
        return (std::abs(x - goal.x) + std::abs(y - goal.y)) * 9;
    };

    auto stateOf = [&](int x, int y, int dir) {
        return (idx(x, y) * 5 + dir);
    };

    const int N = W * H;
    const int S = N * 5;
    const int INF = 1'000'000'000;

    std::vector<int> gCost(static_cast<size_t>(S), INF);
    std::vector<int> parent(static_cast<size_t>(S), -1);
    std::vector<uint8_t> closed(static_cast<size_t>(S), uint8_t{0});

    const int startState = stateOf(start.x, start.y, DIR_NONE);
    gCost[static_cast<size_t>(startState)] = 0;

    std::priority_queue<AStarEntry, std::vector<AStarEntry>, AStarEntryCmp> open;
    open.push({heuristic(start.x, start.y), 0, startState});

    int goalStateFound = -1;

    while (!open.empty()) {
        AStarEntry cur = open.top();
        open.pop();

        const int state = cur.state;
        if (state < 0 || state >= S) continue;
        if (closed[static_cast<size_t>(state)] != 0) continue;
        closed[static_cast<size_t>(state)] = 1;

        const int cell = state / 5;
        const int prevDir = state % 5;
        const int cx = cell % W;
        const int cy = cell / W;

        if (cx == goal.x && cy == goal.y) {
            goalStateFound = state;
            break;
        }

        const int gHere = gCost[static_cast<size_t>(state)];
        if (gHere >= INF) continue;

        for (int oi = 0; oi < 4; ++oi) {
            const int nd = order[oi];
            const int nx = cx + dirs[nd][0];
            const int ny = cy + dirs[nd][1];
            if (!inside(nx, ny)) continue;

            // Never tunnel through rooms (except the endpoints which are outside room walls anyway).
            if (!(nx == goal.x && ny == goal.y) && inRoom(nx, ny)) continue;

            const TileType tt = d.at(nx, ny).type;
            if (!corridorTileOk(tt)) continue;

            const int step = corridorStepCost(tt);
            const int turnPenalty = (prevDir != DIR_NONE && nd != prevDir) ? 6 : 0;

            const int g2 = gHere + step + turnPenalty + nearRoomPenalty(nx, ny);
            const int ns = stateOf(nx, ny, nd);

            if (g2 < gCost[static_cast<size_t>(ns)]) {
                gCost[static_cast<size_t>(ns)] = g2;
                parent[static_cast<size_t>(ns)] = state;

                // Deterministic 0/1 tie-breaker without consuming RNG.
                const int jitter = (nx * 17 + ny * 31 + nd * 7) & 1;
                const int f2 = g2 + heuristic(nx, ny) + jitter;
                open.push({f2, g2, ns});
            }
        }
    }

    if (goalStateFound < 0) return false;

    // Reconstruct path.
    std::vector<Vec2i> path;
    path.reserve(256);

    int st = goalStateFound;
    while (st >= 0) {
        const int cell = st / 5;
        const int px = cell % W;
        const int py = cell / W;
        path.push_back({px, py});
        if (st == startState) break;
        st = parent[static_cast<size_t>(st)];
    }

    if (path.empty()) return false;
    if (path.back().x != start.x || path.back().y != start.y) return false;

    std::reverse(path.begin(), path.end());

    // Carve corridor (only convert walls to floor).
    for (const Vec2i& p : path) {
        if (!d.inBounds(p.x, p.y)) continue;
        TileType& tt = d.at(p.x, p.y).type;
        if (tt == TileType::Wall) tt = TileType::Floor;
    }

    return true;
}

void connectRooms(Dungeon& d, const Room& a, const Room& b, RNG& rng, const std::vector<uint8_t>& roomMask) {
    DoorPick da = pickDoorOnRoomSmart(a, d, rng, {b.cx(), b.cy()}, &a);
    DoorPick db = pickDoorOnRoomSmart(b, d, rng, {a.cx(), a.cy()}, &b);

    auto placeRoomDoor = [&](const Vec2i& p) {
        if (!d.inBounds(p.x, p.y)) return;
        TileType& tt = d.at(p.x, p.y).type;
        // Never override special doors (vault/secret) if they happen to be in the room list.
        if (tt == TileType::DoorLocked || tt == TileType::DoorSecret) return;
        if (tt == TileType::StairsUp || tt == TileType::StairsDown) return;
        // Normalize to a closed door.
        tt = TileType::DoorClosed;
    };

    // Place the two room-connection doors.
    placeRoomDoor(da.doorInside);
    placeRoomDoor(db.doorInside);

    // Ensure corridor starts are floor
    carveFloor(d, da.corridorStart.x, da.corridorStart.y);
    carveFloor(d, db.corridorStart.x, db.corridorStart.y);


    // Prefer A* tunneling that avoids cutting through other rooms.
    // If it fails (rare), fall back to the classic L-shaped corridor.
    if (!carveCorridorAStar(d, rng, da.corridorStart, db.corridorStart, roomMask)) {
        const int x1 = da.corridorStart.x;
        const int y1 = da.corridorStart.y;
        const int x2 = db.corridorStart.x;
        const int y2 = db.corridorStart.y;

        if (rng.chance(0.5f)) {
            carveH(d, x1, x2, y1);
            carveV(d, y1, y2, x2);
        } else {
            carveV(d, y1, y2, x1);
            carveH(d, x1, x2, y2);
        }
    }
}

void ensureBorders(Dungeon& d) {
    for (int x = 0; x < d.width; ++x) {
        d.at(x, 0).type = TileType::Wall;
        d.at(x, d.height - 1).type = TileType::Wall;
    }
    for (int y = 0; y < d.height; ++y) {
        d.at(0, y).type = TileType::Wall;
        d.at(d.width - 1, y).type = TileType::Wall;
    }
}

std::vector<int> bfsDistanceMap(const Dungeon& d, Vec2i start) {
    std::vector<int> dist(static_cast<size_t>(d.width * d.height), -1);
    auto idx = [&](int x, int y) { return y * d.width + x; };

    if (!d.inBounds(start.x, start.y)) return dist;
    dist[static_cast<size_t>(idx(start.x, start.y))] = 0;

    std::deque<Vec2i> q;
    q.push_back(start);

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        Vec2i p = q.front();
        q.pop_front();
        int cd = dist[static_cast<size_t>(idx(p.x, p.y))];

        for (auto& dv : dirs) {
            int nx = p.x + dv[0];
            int ny = p.y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;
            int ii = idx(nx, ny);
            if (dist[static_cast<size_t>(ii)] != -1) continue;
            dist[static_cast<size_t>(ii)] = cd + 1;
            q.push_back({nx, ny});
        }
    }

    return dist;
}

bool stairsConnected(const Dungeon& d) {
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return true;
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) return true;
    const auto dist = bfsDistanceMap(d, d.stairsUp);
    const size_t ii = static_cast<size_t>(d.stairsDown.y * d.width + d.stairsDown.x);
    if (ii >= dist.size()) return false;
    return dist[ii] >= 0;
}

void ensureStairLandingPads(Dungeon& d) {
    // A tiny fairness/robustness pass: ensure stairs aren't surrounded by
    // impassable micro-terrain (chasm/pillars/boulders) and have at least a
    // minimal walkable "landing".
    auto clearAround = [&](Vec2i s) {
        if (!d.inBounds(s.x, s.y)) return;

        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int x = s.x + ox;
                const int y = s.y + oy;
                if (!d.inBounds(x, y)) continue;

                Tile& t = d.at(x, y);
                if (t.type == TileType::StairsUp || t.type == TileType::StairsDown) continue;
                if (t.type == TileType::DoorClosed || t.type == TileType::DoorOpen || t.type == TileType::DoorSecret || t.type == TileType::DoorLocked)
                    continue;

                // Clear "blocking" micro-terrain in the immediate 3x3.
                if (t.type == TileType::Chasm || t.type == TileType::Pillar || t.type == TileType::Boulder) {
                    t.type = TileType::Floor;
                    continue;
                }

                // If we somehow ended up with walls directly adjacent, open a minimum landing.
                if ((std::abs(ox) + std::abs(oy)) <= 1 && t.type == TileType::Wall) {
                    t.type = TileType::Floor;
                }
            }
        }
    };

    clearAround(d.stairsUp);
    clearAround(d.stairsDown);
}

bool repairStairsConnectivity(Dungeon& d, RNG& rng) {
    // A last-ditch repair pass: if a rare late procgen step disconnects stairs,
    // carve a conservative tunnel between them.
    //
    // We search with a weighted Dijkstra that prefers existing corridors/floor
    // but can "dig" through walls if needed.
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return true;
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) return true;
    if (stairsConnected(d)) return true;

    const int W = d.width;
    const int H = d.height;
    const int INF = 1'000'000'000;

    auto idx = [&](int x, int y) { return y * W + x; };
    auto inInner = [&](int x, int y) {
        // Keep a 1-tile border intact; generation relies on solid borders.
        return x >= 1 && y >= 1 && x < W - 1 && y < H - 1;
    };

    auto stepCost = [&](TileType t) {
        switch (t) {
            case TileType::Floor:
            case TileType::StairsUp:
            case TileType::StairsDown:
            case TileType::DoorOpen:
            case TileType::Fountain:
            case TileType::Altar:
                return 1;
            case TileType::DoorClosed:
            case TileType::DoorSecret:
            case TileType::DoorLocked:
                // Doors are traversable but slightly penalized.
                return 2;
            case TileType::Boulder:
                // Pushable, but treat as costly; we may clear it while carving.
                return 6;
            case TileType::Pillar:
                return 8;
            case TileType::Chasm:
                return 10;
            case TileType::Wall:
            default:
                return 7;
        }
    };

    struct QN {
        int f;
        int g;
        int x;
        int y;
    };

    auto cmp = [](const QN& a, const QN& b) {
        if (a.f != b.f) return a.f > b.f;
        return a.g > b.g;
    };

    std::priority_queue<QN, std::vector<QN>, decltype(cmp)> pq(cmp);
    std::vector<int> dist(static_cast<size_t>(W * H), INF);
    std::vector<int> parent(static_cast<size_t>(W * H), -1);

    const Vec2i start = d.stairsUp;
    const Vec2i goal = d.stairsDown;

    dist[static_cast<size_t>(idx(start.x, start.y))] = 0;
    const int h0 = std::abs(start.x - goal.x) + std::abs(start.y - goal.y);
    pq.push({h0, 0, start.x, start.y});

    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!pq.empty()) {
        const QN cur = pq.top();
        pq.pop();

        const int ci = idx(cur.x, cur.y);
        if (cur.g != dist[static_cast<size_t>(ci)]) continue;
        if (cur.x == goal.x && cur.y == goal.y) break;

        // Small shuffle by random start index for tie-breaking variety.
        const int startDir = rng.range(0, 3);
        for (int di = 0; di < 4; ++di) {
            const Vec2i dv = dirs[(startDir + di) % 4];
            const int nx = cur.x + dv.x;
            const int ny = cur.y + dv.y;
            if (!inInner(nx, ny)) continue;

            const int ni = idx(nx, ny);
            const int cost = stepCost(d.at(nx, ny).type);
            const int ng = cur.g + cost;
            if (ng >= dist[static_cast<size_t>(ni)]) continue;

            dist[static_cast<size_t>(ni)] = ng;
            parent[static_cast<size_t>(ni)] = ci;

            const int h = std::abs(nx - goal.x) + std::abs(ny - goal.y);
            pq.push({ng + h, ng, nx, ny});
        }
    }

    const int goalIdx = idx(goal.x, goal.y);
    if (parent[static_cast<size_t>(goalIdx)] < 0) return false;

    // Carve along the found path.
    int walk = goalIdx;
    while (walk != idx(start.x, start.y) && walk >= 0) {
        const int px = walk % W;
        const int py = walk / W;
        Tile& t = d.at(px, py);

        if (t.type == TileType::Wall || t.type == TileType::Chasm || t.type == TileType::Pillar || t.type == TileType::Boulder) {
            t.type = TileType::Floor;
        }

        walk = parent[static_cast<size_t>(walk)];
    }

    // Reassert stairs tiles (in case any carving logic touched them).
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;

    ensureStairLandingPads(d);
    return stairsConnected(d);
}



// ------------------------------------------------------------
// Stairs path "weaving" / redundancy pass
//
// Roguelikes tend to feel best when the critical route between the stairs isn't
// purely a single chain of 1-tile chokepoints. We analyze the passable tile
// graph, compute bridge edges (cut-edges), and then conservatively carve a few
// tiny 2x2 bypass loops around bridges on the current *shortest* stairs path.
//
// This is intentionally conservative:
//  - we only dig through solid wall tiles (never touch doors/stairs/special tiles)
//  - we avoid room interiors (so room layouts remain readable)
//  - we cap the number of bypasses per floor
// ------------------------------------------------------------
using EdgeKey = uint64_t;

inline EdgeKey packEdgeKey(int a, int b) {
    const uint32_t u = static_cast<uint32_t>(std::min(a, b));
    const uint32_t v = static_cast<uint32_t>(std::max(a, b));
    return (static_cast<EdgeKey>(u) << 32) | static_cast<EdgeKey>(v);
}

inline bool isBridgeEdge(const std::vector<EdgeKey>& bridgesSorted, int a, int b) {
    const EdgeKey k = packEdgeKey(a, b);
    return std::binary_search(bridgesSorted.begin(), bridgesSorted.end(), k);
}

void computePassableBridges(const Dungeon& d, std::vector<EdgeKey>& outBridges) {
    outBridges.clear();

    const int W = d.width;
    const int H = d.height;
    if (W <= 0 || H <= 0) return;

    const int N = W * H;
    std::vector<int> disc(static_cast<size_t>(N), -1);
    std::vector<int> low(static_cast<size_t>(N), 0);
    std::vector<int> parent(static_cast<size_t>(N), -1);

    int t = 0;

    auto isNode = [&](int i) -> bool {
        const int x = i % W;
        const int y = i / W;
        return d.isPassable(x, y);
    };

    auto forEachNeighbor = [&](int u, const std::function<void(int)>& fn) {
        const int x = u % W;
        const int y = u / W;
        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        for (const auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;
            fn(ny * W + nx);
        }
    };

    std::function<void(int)> dfs = [&](int u) {
        disc[static_cast<size_t>(u)] = t;
        low[static_cast<size_t>(u)] = t;
        ++t;

        forEachNeighbor(u, [&](int v) {
            if (disc[static_cast<size_t>(v)] == -1) {
                parent[static_cast<size_t>(v)] = u;
                dfs(v);
                low[static_cast<size_t>(u)] = std::min(low[static_cast<size_t>(u)], low[static_cast<size_t>(v)]);

                if (low[static_cast<size_t>(v)] > disc[static_cast<size_t>(u)]) {
                    outBridges.push_back(packEdgeKey(u, v));
                }
            } else if (v != parent[static_cast<size_t>(u)]) {
                low[static_cast<size_t>(u)] = std::min(low[static_cast<size_t>(u)], disc[static_cast<size_t>(v)]);
            }
        });
    };

    for (int i = 0; i < N; ++i) {
        if (!isNode(i)) continue;
        if (disc[static_cast<size_t>(i)] != -1) continue;
        dfs(i);
    }

    std::sort(outBridges.begin(), outBridges.end());
    outBridges.erase(std::unique(outBridges.begin(), outBridges.end()), outBridges.end());
}

bool buildShortestPassablePath(const Dungeon& d, Vec2i start, Vec2i goal, std::vector<int>& outPath) {
    outPath.clear();
    if (!d.inBounds(start.x, start.y) || !d.inBounds(goal.x, goal.y)) return false;
    if (!d.isPassable(start.x, start.y) || !d.isPassable(goal.x, goal.y)) return false;

    const int W = d.width;
    const int H = d.height;

    auto idx = [&](int x, int y) { return y * W + x; };

    const int s = idx(start.x, start.y);
    const int g = idx(goal.x, goal.y);

    std::vector<int> parent(static_cast<size_t>(W * H), -1);
    std::deque<int> q;

    parent[static_cast<size_t>(s)] = s;
    q.push_back(s);

    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        const int u = q.front();
        q.pop_front();
        if (u == g) break;

        const int ux = u % W;
        const int uy = u / W;

        for (const auto& dv : dirs) {
            const int nx = ux + dv[0];
            const int ny = uy + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;

            const int v = idx(nx, ny);
            if (parent[static_cast<size_t>(v)] != -1) continue;

            parent[static_cast<size_t>(v)] = u;
            q.push_back(v);
        }
    }

    if (parent[static_cast<size_t>(g)] == -1) return false;

    // Reconstruct.
    int walk = g;
    while (walk != s) {
        outPath.push_back(walk);
        walk = parent[static_cast<size_t>(walk)];
        if (walk < 0) break;
    }
    outPath.push_back(s);
    std::reverse(outPath.begin(), outPath.end());
    return true;
}

int countBridgesOnPath(const std::vector<int>& path, const std::vector<EdgeKey>& bridgesSorted) {
    int c = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        if (isBridgeEdge(bridgesSorted, path[i - 1], path[i])) ++c;
    }
    return c;
}

bool reachableAvoidingBridgeEdges(const Dungeon& d, Vec2i start, Vec2i goal, const std::vector<EdgeKey>& bridgesSorted) {
    if (!d.inBounds(start.x, start.y) || !d.inBounds(goal.x, goal.y)) return false;
    if (!d.isPassable(start.x, start.y) || !d.isPassable(goal.x, goal.y)) return false;

    const int W = d.width;
    const int N = W * d.height;

    auto idx = [&](int x, int y) { return y * W + x; };

    const int s = idx(start.x, start.y);
    const int g = idx(goal.x, goal.y);

    std::vector<uint8_t> vis(static_cast<size_t>(N), uint8_t{0});
    std::deque<int> q;
    vis[static_cast<size_t>(s)] = 1;
    q.push_back(s);

    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        const int u = q.front();
        q.pop_front();
        if (u == g) return true;

        const int ux = u % W;
        const int uy = u / W;

        for (const auto& dv : dirs) {
            const int nx = ux + dv[0];
            const int ny = uy + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;

            const int v = idx(nx, ny);
            if (vis[static_cast<size_t>(v)]) continue;

            // Skip cut-edges.
            if (isBridgeEdge(bridgesSorted, u, v)) continue;

            vis[static_cast<size_t>(v)] = 1;
            q.push_back(v);
        }
    }

    return false;
}

bool canWeaveCarveWall(const Dungeon& d, int x, int y) {
    if (!d.inBounds(x, y)) return false;
    // Preserve a solid 1-tile border.
    if (x <= 1 || y <= 1 || x >= d.width - 2 || y >= d.height - 2) return false;

    const TileType t = d.at(x, y).type;
    if (t != TileType::Wall) return false;

    // Avoid carving into any defined room rectangle (keeps rooms readable, preserves doors).
    if (findRoomContaining(d, x, y) != nullptr) return false;

    // Avoid carving too close to stairs landings (visual clarity / fairness).
    auto tooClose = [&](Vec2i s) {
        if (!d.inBounds(s.x, s.y)) return false;
        return (std::abs(x - s.x) + std::abs(y - s.y)) <= 4;
    };
    if (tooClose(d.stairsUp) || tooClose(d.stairsDown)) return false;

    return true;
}

int bypassOptionsForEdge(const Dungeon& d, int u, int v) {
    const int W = d.width;

    const int ux = u % W;
    const int uy = u / W;
    const int vx = v % W;
    const int vy = v / W;

    const int dx = vx - ux;
    const int dy = vy - uy;
    if (std::abs(dx) + std::abs(dy) != 1) return 0;

    // Only weave around plain-ish floor segments (not doors/stairs).
    auto okEnd = [&](TileType t) {
        return (t == TileType::Floor || t == TileType::Fountain || t == TileType::Altar ||
                t == TileType::DoorOpen || t == TileType::DoorClosed);
    };
    if (!okEnd(d.at(ux, uy).type) || !okEnd(d.at(vx, vy).type)) return 0;

    int options = 0;
    if (dx != 0) {
        // Horizontal edge -> try up/down.
        if (canWeaveCarveWall(d, ux, uy + 1) && canWeaveCarveWall(d, vx, vy + 1)) ++options;
        if (canWeaveCarveWall(d, ux, uy - 1) && canWeaveCarveWall(d, vx, vy - 1)) ++options;
    } else {
        // Vertical edge -> try left/right.
        if (canWeaveCarveWall(d, ux + 1, uy) && canWeaveCarveWall(d, vx + 1, vy)) ++options;
        if (canWeaveCarveWall(d, ux - 1, uy) && canWeaveCarveWall(d, vx - 1, vy)) ++options;
    }
    return options;
}

bool tryCarveBridgeBypassLoop(Dungeon& d, RNG& rng, int u, int v) {
    const int W = d.width;

    const int ux = u % W;
    const int uy = u / W;
    const int vx = v % W;
    const int vy = v / W;

    const int dx = vx - ux;
    const int dy = vy - uy;
    if (std::abs(dx) + std::abs(dy) != 1) return false;

    Vec2i offs[2];
    if (dx != 0) {
        offs[0] = {0, 1};
        offs[1] = {0, -1};
    } else {
        offs[0] = {1, 0};
        offs[1] = {-1, 0};
    }

    // Shuffle which side we try first for variety (seeded).
    if (rng.chance(0.5f)) std::swap(offs[0], offs[1]);

    for (const Vec2i off : offs) {
        const int ax = ux + off.x;
        const int ay = uy + off.y;
        const int bx = vx + off.x;
        const int by = vy + off.y;

        if (!canWeaveCarveWall(d, ax, ay)) continue;
        if (!canWeaveCarveWall(d, bx, by)) continue;

        // Carve the 2x2 bypass: u -> a -> b -> v
        d.at(ax, ay).type = TileType::Floor;
        d.at(bx, by).type = TileType::Floor;
        return true;
    }

    return false;
}

void weaveStairsConnectivity(Dungeon& d, RNG& rng, int depth) {
    d.stairsBypassLoopCount = 0;
    d.stairsBridgeCount = 0;
    d.stairsRedundancyOk = false;

    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return;
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) return;
    if (!stairsConnected(d)) return;

    // The deeper we go, the more we allow ourselves to "weave" (still conservative).
    const int maxWeaves = std::clamp(2 + depth / 4, 2, 5);

    for (int pass = 0; pass < maxWeaves; ++pass) {
        std::vector<EdgeKey> bridges;
        computePassableBridges(d, bridges);

        std::vector<int> path;
        if (!buildShortestPassablePath(d, d.stairsUp, d.stairsDown, path)) break;

        const int bridgeCount = countBridgesOnPath(path, bridges);
        d.stairsBridgeCount = bridgeCount;
        d.stairsRedundancyOk = reachableAvoidingBridgeEdges(d, d.stairsUp, d.stairsDown, bridges);

        if (d.stairsRedundancyOk) break;

        // Find a promising bridge edge on the stairs path to patch.
        struct Cand {
            int u = -1;
            int v = -1;
            int score = 0;
        };
        std::vector<Cand> cands;
        cands.reserve(16);

        for (size_t i = 1; i < path.size(); ++i) {
            const int u = path[i - 1];
            const int v = path[i];
            if (!isBridgeEdge(bridges, u, v)) continue;

            // Keep bypasses away from stair landings and keep them from hugging the very ends.
            if (i < 6 || i + 6 >= path.size()) continue;

            const int opts = bypassOptionsForEdge(d, u, v);
            if (opts <= 0) continue;

            // Score: prefer central path positions, and prefer edges with 2 bypass options.
            const int mid = static_cast<int>(std::min(i, path.size() - i));
            int score = opts * 100 + mid * 2;

            // Slight seeded jitter to avoid always carving in the exact same place on similar layouts.
            score += rng.range(-3, 3);

            cands.push_back({u, v, score});
        }

        if (cands.empty()) break;

        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.score > b.score;
        });

        // Pick among the best few for variety while staying "good".
        const int topN = std::min(3, static_cast<int>(cands.size()));
        const Cand pick = cands[rng.range(0, topN - 1)];

        if (tryCarveBridgeBypassLoop(d, rng, pick.u, pick.v)) {
            d.stairsBypassLoopCount++;
        } else {
            break;
        }
    }

    // Final stats after weaving.
    {
        std::vector<EdgeKey> bridges;
        computePassableBridges(d, bridges);

        std::vector<int> path;
        if (buildShortestPassablePath(d, d.stairsUp, d.stairsDown, path)) {
            d.stairsBridgeCount = countBridgesOnPath(path, bridges);
            d.stairsRedundancyOk = reachableAvoidingBridgeEdges(d, d.stairsUp, d.stairsDown, bridges);
        }
    }
}


// ------------------------------------------------------------
// Global connectivity weaving / anti-chokepoint pass
//
// The stairs weaving pass focuses specifically on making the *critical* route
// between stairsUp and stairsDown less tree-like. However, many floors still end
// up with other major chokepoints (bridge edges) that aren't on the current
// shortest stairs path:
//   - long side branches that dead-end behind a single 1-tile corridor edge
//   - entire "wings" of the level gated by a single cut-edge
//
// This pass analyzes the full passable-tile graph, computes bridge edges, then
// estimates which bridges are most impactful by contracting the graph into
// 2-edge-connected components (non-bridge edges unioned into components). The
// bridge edges become a forest over these components; we compute the component
// cut-size for each bridge and then carve a small number of conservative 2x2
// bypass loops around the best candidates.
//
// Constraints (same philosophy as stairs weaving):
//  - only dig through plain wall tiles
//  - never carve inside defined rooms (keeps room layouts readable)
//  - never carve too close to stairs landings
//  - cap the number of bypasses per floor (scaled by depth)
// ------------------------------------------------------------
void weaveGlobalConnectivity(Dungeon& d, RNG& rng, int depth) {
    d.globalBypassLoopCount = 0;
    d.globalBridgeCountBefore = 0;
    d.globalBridgeCountAfter = 0;

    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return;
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) return;
    if (!stairsConnected(d)) return;

    std::vector<EdgeKey> bridges;
    computePassableBridges(d, bridges);
    d.globalBridgeCountBefore = static_cast<int>(bridges.size());
    d.globalBridgeCountAfter = d.globalBridgeCountBefore;

    // No bridges -> already richly loopy.
    if (bridges.empty()) return;

    // Make this a deeper-dungeon flavor: early floors stay a bit more readable.
    float pAny = 0.12f + 0.035f * static_cast<float>(std::min(depth, 12));
    pAny = std::min(0.70f, pAny);

    // If the floor is already pretty loopy, reduce the chance even more.
    if (d.globalBridgeCountBefore < 10) pAny *= 0.20f;
    else if (d.globalBridgeCountBefore < 18) pAny *= 0.55f;

    if (!rng.chance(pAny)) return;

    int maxWeaves = 0;
    if (depth >= 3) maxWeaves = 1;
    if (depth >= 6) maxWeaves = 2;
    if (depth >= 10) maxWeaves = 3;
    if (depth >= 16) maxWeaves = 4;

    if (maxWeaves <= 0) return;

    // Also cap by available bridge count so tiny layouts don't over-weave.
    maxWeaves = std::min(maxWeaves, 1 + d.globalBridgeCountBefore / 35);

    const int W = d.width;
    const int H = d.height;
    const int N = W * H;

    auto idx = [&](int x, int y) { return y * W + x; };

    auto isNode = [&](int i) -> bool {
        const int x = i % W;
        const int y = i / W;
        return d.isPassable(x, y);
    };

    struct DSU {
        std::vector<int> p;
        std::vector<int> sz;
        explicit DSU(int n) : p(static_cast<size_t>(std::max(0, n))), sz(static_cast<size_t>(std::max(0, n)), 1) {
            for (int i = 0; i < n; ++i) p[static_cast<size_t>(i)] = i;
        }
        int find(int a) {
            int x = a;
            while (p[static_cast<size_t>(x)] != x) x = p[static_cast<size_t>(x)];
            while (p[static_cast<size_t>(a)] != a) {
                const int parent = p[static_cast<size_t>(a)];
                p[static_cast<size_t>(a)] = x;
                a = parent;
            }
            return x;
        }
        void unite(int a, int b) {
            a = find(a);
            b = find(b);
            if (a == b) return;
            if (sz[static_cast<size_t>(a)] < sz[static_cast<size_t>(b)]) std::swap(a, b);
            p[static_cast<size_t>(b)] = a;
            sz[static_cast<size_t>(a)] += sz[static_cast<size_t>(b)];
        }
    };

    struct Adj {
        int to = -1;
        int edgeIdx = -1;
    };

    struct BridgeInfo {
        int u = -1;
        int v = -1;
        int cu = -1;
        int cv = -1;
        int importance = 0; // approx cut-size (smaller side tile count)
    };

    // Iteratively carve a few bypasses (bridges change after each carve).
    for (int pass = 0; pass < maxWeaves; ++pass) {
        computePassableBridges(d, bridges);
        if (bridges.empty()) break;

        // Build 2-edge-connected components by unioning all non-bridge edges.
        DSU dsu(N);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (!d.isPassable(x, y)) continue;
                const int u = idx(x, y);

                if (x + 1 < W && d.isPassable(x + 1, y)) {
                    const int v = idx(x + 1, y);
                    if (!isBridgeEdge(bridges, u, v)) dsu.unite(u, v);
                }
                if (y + 1 < H && d.isPassable(x, y + 1)) {
                    const int v = idx(x, y + 1);
                    if (!isBridgeEdge(bridges, u, v)) dsu.unite(u, v);
                }
            }
        }

        // Map DSU roots -> compact component ids.
        std::vector<int> rootToComp(static_cast<size_t>(N), -1);
        std::vector<int> compId(static_cast<size_t>(N), -1);
        std::vector<int> compSize;
        compSize.reserve(static_cast<size_t>(std::max(8, N / 32)));

        for (int i = 0; i < N; ++i) {
            if (!isNode(i)) continue;
            const int r = dsu.find(i);
            int& cid = rootToComp[static_cast<size_t>(r)];
            if (cid < 0) {
                cid = static_cast<int>(compSize.size());
                compSize.push_back(0);
            }
            compId[static_cast<size_t>(i)] = cid;
            compSize[static_cast<size_t>(cid)] += 1;
        }

        const int C = static_cast<int>(compSize.size());
        if (C <= 1) break;

        // Build component bridge forest.
        std::vector<BridgeInfo> bedges;
        bedges.reserve(bridges.size());

        std::vector<std::vector<Adj>> adj(static_cast<size_t>(C));

        for (const EdgeKey k : bridges) {
            const int u = static_cast<int>(static_cast<uint32_t>(k >> 32));
            const int v = static_cast<int>(static_cast<uint32_t>(k & 0xFFFFFFFFu));
            if (u < 0 || v < 0 || u >= N || v >= N) continue;

            const int cu = compId[static_cast<size_t>(u)];
            const int cv = compId[static_cast<size_t>(v)];
            if (cu < 0 || cv < 0 || cu == cv) continue;

            const int ei = static_cast<int>(bedges.size());
            bedges.push_back({u, v, cu, cv, 0});

            adj[static_cast<size_t>(cu)].push_back({cv, ei});
            adj[static_cast<size_t>(cv)].push_back({cu, ei});
        }

        if (bedges.empty()) break;

        // Compute bridge importance via subtree sizes on the component forest.
        std::vector<int> parent(static_cast<size_t>(C), -1);
        std::vector<int> parentEdge(static_cast<size_t>(C), -1);
        std::vector<int> subtree(static_cast<size_t>(C), 0);
        std::vector<uint8_t> seen(static_cast<size_t>(C), uint8_t{0});

        for (int root = 0; root < C; ++root) {
            if (seen[static_cast<size_t>(root)] != 0) continue;

            std::vector<int> order;
            order.reserve(64);

            std::vector<int> stack;
            stack.reserve(64);

            seen[static_cast<size_t>(root)] = 1;
            parent[static_cast<size_t>(root)] = -1;
            parentEdge[static_cast<size_t>(root)] = -1;
            stack.push_back(root);

            while (!stack.empty()) {
                const int c = stack.back();
                stack.pop_back();
                order.push_back(c);

                for (const Adj& e : adj[static_cast<size_t>(c)]) {
                    if (e.to < 0 || e.to >= C) continue;
                    if (seen[static_cast<size_t>(e.to)] != 0) continue;

                    seen[static_cast<size_t>(e.to)] = 1;
                    parent[static_cast<size_t>(e.to)] = c;
                    parentEdge[static_cast<size_t>(e.to)] = e.edgeIdx;
                    stack.push_back(e.to);
                }
            }

            // Subtree sizes (reverse order).
            for (int oi = static_cast<int>(order.size()) - 1; oi >= 0; --oi) {
                const int c = order[static_cast<size_t>(oi)];
                int sum = compSize[static_cast<size_t>(c)];
                for (const Adj& e : adj[static_cast<size_t>(c)]) {
                    if (e.to < 0 || e.to >= C) continue;
                    if (parent[static_cast<size_t>(e.to)] == c) {
                        sum += subtree[static_cast<size_t>(e.to)];
                    }
                }
                subtree[static_cast<size_t>(c)] = sum;
            }

            const int total = subtree[static_cast<size_t>(root)];

            // Edge importance from child subtree sizes.
            for (const int c : order) {
                const int p = parent[static_cast<size_t>(c)];
                if (p < 0) continue;
                const int ei = parentEdge[static_cast<size_t>(c)];
                if (ei < 0 || ei >= static_cast<int>(bedges.size())) continue;
                const int sub = subtree[static_cast<size_t>(c)];
                const int smallSide = std::min(sub, total - sub);
                bedges[static_cast<size_t>(ei)].importance = smallSide;
            }
        }

        struct Cand {
            int u = -1;
            int v = -1;
            int score = 0;
        };

        std::vector<Cand> cands;
        cands.reserve(32);

        for (const BridgeInfo& be : bedges) {
            const int opts = bypassOptionsForEdge(d, be.u, be.v);
            if (opts <= 0) continue;

            // Score: prioritize big "cuts" and edges with multiple bypass options.
            // Cap importance so huge maps don't overpower everything.
            const int imp = std::min(260, be.importance);
            int score = imp * 5 + opts * 140;

            // Light deterministic jitter for variety.
            score += rng.range(-8, 8);

            cands.push_back({be.u, be.v, score});
        }

        if (cands.empty()) break;

        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.score > b.score;
        });

        const int topN = std::min(4, static_cast<int>(cands.size()));
        const Cand pick = cands[static_cast<size_t>(rng.range(0, topN - 1))];

        if (tryCarveBridgeBypassLoop(d, rng, pick.u, pick.v)) {
            d.globalBypassLoopCount++;
        } else {
            break;
        }
    }

    // Final bridge stats after global weaving.
    {
        computePassableBridges(d, bridges);
        d.globalBridgeCountAfter = static_cast<int>(bridges.size());

        // Recompute stairs-path stats so #mapstats reflects the final graph state.
        std::vector<int> path;
        if (buildShortestPassablePath(d, d.stairsUp, d.stairsDown, path)) {
            d.stairsBridgeCount = countBridgesOnPath(path, bridges);
            d.stairsRedundancyOk = reachableAvoidingBridgeEdges(d, d.stairsUp, d.stairsDown, bridges);
        }
    }
}




// ------------------------------------------------------------
// Secret shortcut doors
//
// A late procgen pass that plants a small number of *hidden* doors (DoorSecret)
// in corridor walls where two already-passable regions run adjacent but are
// separated by a single wall tile.
//
// This creates optional loops/shortcuts that reward searching without creating
// disconnected "hidden corridor" floor pockets (which could break spawning/pathing).
// ------------------------------------------------------------
bool maybePlaceSecretShortcuts(Dungeon& d, RNG& rng, int depth) {
    d.secretShortcutCount = 0;

    // Slightly more likely on deeper floors, but still rare enough to feel special.
    float pAny = 0.22f + 0.03f * static_cast<float>(std::clamp(depth - 1, 0, 8));
    pAny = std::min(0.50f, pAny);
    if (!rng.chance(pAny)) return false;

    int want = 1;
    if (depth >= 7 && rng.chance(0.35f)) want += 1;
    want = std::min(2, want);

    auto tooCloseToStairs = [&](int tx, int ty) {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(tx - d.stairsUp.x) + std::abs(ty - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(tx - d.stairsDown.x) + std::abs(ty - d.stairsDown.y))
            : 9999;
        return du <= 4 || dd <= 4;
    };

    auto disallowedRoom = [&](const Room* r) {
        if (!r) return false;
        // Don't create alternate entrances into bonus rooms or shops.
        return r->type == RoomType::Vault || r->type == RoomType::Secret || r->type == RoomType::Shop;
    };

    const int maxTries = 900;

    for (int tries = 0; tries < maxTries && d.secretShortcutCount < want; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;

        // Avoid making secrets trivial/obvious.
        if (tooCloseToStairs(x, y)) continue;
        if (anyDoorInRadius(d, x, y, 2)) continue;

        // Keep shortcut doors out of room interiors (including shaped rooms).
        if (findRoomContaining(d, x, y) != nullptr) continue;

        Vec2i a{-1, -1};
        Vec2i b{-1, -1};

        // Candidate must separate two passable tiles in a straight line.
        if (d.isPassable(x - 1, y) && d.isPassable(x + 1, y) && !d.isPassable(x, y - 1) && !d.isPassable(x, y + 1)) {
            a = {x - 1, y};
            b = {x + 1, y};
        } else if (d.isPassable(x, y - 1) && d.isPassable(x, y + 1) && !d.isPassable(x - 1, y) && !d.isPassable(x + 1, y)) {
            a = {x, y - 1};
            b = {x, y + 1};
        } else {
            continue;
        }

        if (tooCloseToStairs(a.x, a.y) || tooCloseToStairs(b.x, b.y)) continue;

        const Room* ra = findRoomContaining(d, a.x, a.y);
        const Room* rb = findRoomContaining(d, b.x, b.y);
        if (disallowedRoom(ra) || disallowedRoom(rb)) continue;

        // Require that the existing shortest path between the two regions is "long enough"
        // so the hidden door actually acts as a meaningful shortcut instead of a tiny bypass.
        const auto dist = bfsDistanceMap(d, a);
        const size_t ii = static_cast<size_t>(b.y * d.width + b.x);
        if (ii >= dist.size()) continue;
        const int cur = dist[ii];
        if (cur < 0) continue;

        const int minDist = std::clamp(12 + depth, 14, 26);
        if (cur < minDist) continue;

        d.at(x, y).type = TileType::DoorSecret;
        d.secretShortcutCount += 1;
    }

    return d.secretShortcutCount > 0;
}

// ------------------------------------------------------------
// Locked shortcut gates
//
// A sibling pass to secret shortcuts: place a small number of *visible* locked
// doors (DoorLocked) in corridor walls where two already-passable regions run
// adjacent but are separated by a single wall tile.
//
// Unlike secret doors, these are immediately readable on the map, but require
// a key/lockpick to open. Because they connect regions that are already
// connected elsewhere, they never block progression.
//
// Safety:
// - Only placed in corridor walls outside all room rectangles.
// - Requires the existing shortest path between the two sides to be long enough
//   so the gate is a meaningful shortcut.
// ------------------------------------------------------------
bool maybePlaceLockedShortcuts(Dungeon& d, RNG& rng, int depth, bool eligible) {
    d.lockedShortcutCount = 0;
    if (!eligible) return false;

    // Keep the very first floor simple.
    if (depth <= 1) return false;

    // Slightly rarer than secret shortcuts. Ramps up gently with depth.
    float pAny = 0.16f + 0.03f * static_cast<float>(std::clamp(depth - 2, 0, 8));
    pAny = std::min(0.50f, pAny);
    if (!rng.chance(pAny)) return false;

    int want = 1;
    if (depth >= 6 && rng.chance(0.45f)) want += 1;
    if (depth >= 9 && rng.chance(0.25f)) want += 1;
    want = std::min(3, want);

    auto tooCloseToStairs = [&](int tx, int ty) {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(tx - d.stairsUp.x) + std::abs(ty - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(tx - d.stairsDown.x) + std::abs(ty - d.stairsDown.y))
            : 9999;
        return du <= 4 || dd <= 4;
    };

    auto corridorFloor = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (d.at(x, y).type != TileType::Floor) return false;
        // Avoid connecting into any room interior/border; keep these as corridor gates.
        return findRoomContaining(d, x, y) == nullptr;
    };

    const int maxTries = 1100;

    for (int tries = 0; tries < maxTries && d.lockedShortcutCount < want; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;

        if (tooCloseToStairs(x, y)) continue;
        if (anyDoorInRadius(d, x, y, 2)) continue;

        // Keep gates out of rooms (including shaped rooms).
        if (findRoomContaining(d, x, y) != nullptr) continue;

        Vec2i a{-1, -1};
        Vec2i b{-1, -1};

        // Candidate must separate two corridor floor tiles in a straight line.
        if (corridorFloor(x - 1, y) && corridorFloor(x + 1, y) && !d.isPassable(x, y - 1) && !d.isPassable(x, y + 1)) {
            a = {x - 1, y};
            b = {x + 1, y};
        } else if (corridorFloor(x, y - 1) && corridorFloor(x, y + 1) && !d.isPassable(x - 1, y) && !d.isPassable(x + 1, y)) {
            a = {x, y - 1};
            b = {x, y + 1};
        } else {
            continue;
        }

        if (tooCloseToStairs(a.x, a.y) || tooCloseToStairs(b.x, b.y)) continue;

        // Require that the existing shortest path between the two corridor regions is
        // long enough that unlocking the gate matters.
        const auto dist = bfsDistanceMap(d, a);
        const size_t ii = static_cast<size_t>(b.y * d.width + b.x);
        if (ii >= dist.size()) continue;
        const int cur = dist[ii];
        if (cur < 0) continue;

        const int minDist = std::clamp(10 + depth, 14, 28);
        if (cur < minDist) continue;

        d.at(x, y).type = TileType::DoorLocked;
        d.lockedShortcutCount += 1;
    }

    return d.lockedShortcutCount > 0;
}



// ------------------------------------------------------------
// Inter-room doorways
//
// A late procgen pass that adds a small number of direct doors between
// *adjacent rooms* that are separated by a single wall tile.
//
// This creates optional loops/flanking routes inside the room graph
// (complementing corridor braids/tunnels) without rewriting the whole layout.
//
// Safety:
// - Only places doors in wall tiles whose two opposite neighbors are passable
//   and belong to *different* room rectangles.
// - Never targets treasure/vault/secret/shrine/shop/camp rooms.
// - Avoids stairs vicinity and existing doors.
// - Requires the current shortest path between the two sides to be long enough
//   so the new door is a meaningful shortcut rather than noise.
// ------------------------------------------------------------
bool applyInterRoomDoorways(Dungeon& d, RNG& rng, int depth) {
    d.interRoomDoorCount = 0;
    d.interRoomDoorLockedCount = 0;
    d.interRoomDoorSecretCount = 0;

    if (d.rooms.empty()) return false;
    // Keep the very first floor simple/readable.
    if (depth <= 1) return false;

    float pAny = 0.28f + 0.03f * static_cast<float>(std::clamp(depth - 2, 0, 10));
    pAny = std::min(0.62f, pAny);

    // Bigger maps can afford more internal connectivity.
    const int area = d.width * d.height;
    if (area >= 105 * 66) pAny += 0.05f;
    if (area >= 120 * 74) pAny += 0.05f;

    pAny = std::min(0.70f, pAny);
    if (!rng.chance(pAny)) return false;

    int want = 1;
    if (depth >= 4) want += 1;
    if (depth >= 7 && rng.chance(0.35f)) want += 1;
    want = std::min(3, want);

    auto tooCloseToStairs = [&](int tx, int ty) {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(tx - d.stairsUp.x) + std::abs(ty - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(tx - d.stairsDown.x) + std::abs(ty - d.stairsDown.y))
            : 9999;
        return du <= 4 || dd <= 4;
    };

    auto disallowedRoom = [&](const Room* r) -> bool {
        if (!r) return true;
        switch (r->type) {
            case RoomType::Treasure:
            case RoomType::Secret:
            case RoomType::Vault:
            case RoomType::Shrine:
            case RoomType::Shop:
            case RoomType::Camp:
                return true;
            default:
                return false;
        }
    };

    auto packPair = [&](int a, int b) -> uint64_t {
        const uint32_t u = static_cast<uint32_t>(std::min(a, b));
        const uint32_t v = static_cast<uint32_t>(std::max(a, b));
        return (static_cast<uint64_t>(u) << 32) | static_cast<uint64_t>(v);
    };

    std::vector<uint64_t> usedPairs;
    usedPairs.reserve(static_cast<size_t>(want));

    const int maxTries = 1300;

    for (int tries = 0; tries < maxTries && d.interRoomDoorCount < want; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;

        if (tooCloseToStairs(x, y)) continue;
        if (anyDoorInRadius(d, x, y, 2)) continue;

        Vec2i a{-1, -1};
        Vec2i b{-1, -1};

        // Candidate must separate two *room interior* passable tiles in a straight line.
        if (d.isPassable(x - 1, y) && d.isPassable(x + 1, y) && !d.isPassable(x, y - 1) && !d.isPassable(x, y + 1)) {
            a = {x - 1, y};
            b = {x + 1, y};
        } else if (d.isPassable(x, y - 1) && d.isPassable(x, y + 1) && !d.isPassable(x - 1, y) && !d.isPassable(x + 1, y)) {
            a = {x, y - 1};
            b = {x, y + 1};
        } else {
            continue;
        }

        if (tooCloseToStairs(a.x, a.y) || tooCloseToStairs(b.x, b.y)) continue;

        const Room* ra = findRoomContaining(d, a.x, a.y);
        const Room* rb = findRoomContaining(d, b.x, b.y);
        if (!ra || !rb) continue;
        if (ra == rb) continue;
        if (disallowedRoom(ra) || disallowedRoom(rb)) continue;

        // Sanity: ensure both sides are actually walkable (avoid connecting into chasm/pillar partitions).
        if (!d.isWalkable(a.x, a.y) || !d.isWalkable(b.x, b.y)) continue;

        const int ia = static_cast<int>(ra - d.rooms.data());
        const int ib = static_cast<int>(rb - d.rooms.data());
        const uint64_t pk = packPair(ia, ib);
        bool already = false;
        for (uint64_t u : usedPairs) {
            if (u == pk) { already = true; break; }
        }
        if (already) continue;

        // Require that the existing route between the two room sides is meaningfully long,
        // so this door creates a real shortcut rather than just noise.
        const auto dist = bfsDistanceMap(d, a);
        const size_t ii = static_cast<size_t>(b.y * d.width + b.x);
        if (ii >= dist.size()) continue;
        const int cur = dist[ii];
        if (cur < 0) continue;

        const int minDist = std::clamp(10 + depth / 2, 12, 22);
        if (cur < minDist) continue;

        // Choose door style.
        TileType dt = TileType::DoorClosed;
        if (rng.chance(0.20f)) dt = TileType::DoorOpen;

        if (depth >= 4 && rng.chance(0.10f)) {
            dt = TileType::DoorLocked;
        } else if (depth >= 3 && rng.chance(0.08f)) {
            dt = TileType::DoorSecret;
        }

        d.at(x, y).type = dt;

        d.interRoomDoorCount += 1;
        if (dt == TileType::DoorLocked) d.interRoomDoorLockedCount += 1;
        if (dt == TileType::DoorSecret) d.interRoomDoorSecretCount += 1;

        usedPairs.push_back(pk);
    }

    return d.interRoomDoorCount > 0;
}

struct TileChange {
    int x = 0;
    int y = 0;
    TileType prev{};
};

bool isStairsTile(const Dungeon& d, int x, int y) {
    if (!d.inBounds(x, y)) return false;
    return (x == d.stairsUp.x && y == d.stairsUp.y)
        || (x == d.stairsDown.x && y == d.stairsDown.y);
}

void trySetTile(Dungeon& d, int x, int y, TileType t, std::vector<TileChange>& changes) {
    if (!d.inBounds(x, y)) return;
    if (isStairsTile(d, x, y)) return;

    Tile& cur = d.at(x, y);
    if (cur.type == t) return;

    // Only allow replacing plain floor (or an already-decorated tile if we are layering).
    // Boulders are only placed on plain floor.
    if (t == TileType::Boulder) {
        if (cur.type != TileType::Floor) return;
    } else {
        if (!(cur.type == TileType::Floor || cur.type == TileType::Chasm || cur.type == TileType::Pillar)) {
            return;
        }
    }

    changes.push_back({x, y, cur.type});
    cur.type = t;
}

void undoChanges(Dungeon& d, const std::vector<TileChange>& changes) {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        if (!d.inBounds(it->x, it->y)) continue;
        d.at(it->x, it->y).type = it->prev;
    }
}


// ------------------------------------------------------------
// Corridor hubs + great halls
//
// A late procgen pass that widens a few hallway intersections into small
// "junction hubs" and broadens selected long corridor segments into 2-wide
// (rarely 3-wide) "great halls".
//
// Goals:
// - Add tactical variety (less single-tile chokepoint spam).
// - Create occasional open "breathing rooms" outside formal room rectangles.
// - Preserve readability and never create door-less openings into rooms.
//
// Safety:
// - Only converts *walls* to floor (never reduces connectivity).
// - Avoids carving adjacent to any room footprint tile.
// - Avoids carving near doors and stairs.
// ------------------------------------------------------------
bool maybeCarveCorridorHubsAndHalls(Dungeon& d, RNG& rng, int depth, bool eligible) {
    d.corridorHubCount = 0;
    d.corridorHallCount = 0;

    if (!eligible) return false;
    if (d.width < 12 || d.height < 12) return false;
    if (d.rooms.empty()) return false;

    // Chance to apply the pass at all.
    float pAny = 0.50f + 0.03f * static_cast<float>(std::clamp(depth - 1, 0, 10));
    pAny = std::min(0.78f, pAny);
    if (!rng.chance(pAny)) return false;

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    // Room footprint mask: includes all tiles inside room rectangles (even if later shaped).
    std::vector<uint8_t> roomMask(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                roomMask[idx(x, y)] = 1;
            }
        }
    }

    auto inRoom = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return roomMask[idx(x, y)] != 0;
    };

    auto tooCloseToStairs = [&](int tx, int ty) {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(tx - d.stairsUp.x) + std::abs(ty - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(tx - d.stairsDown.x) + std::abs(ty - d.stairsDown.y))
            : 9999;
        return du <= 5 || dd <= 5;
    };

    auto corridorFloor = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (inRoom(x, y)) return false;
        return d.at(x, y).type == TileType::Floor;
    };

    auto safeWallToCarve = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (x <= 1 || y <= 1 || x >= d.width - 2 || y >= d.height - 2) return false;
        if (d.at(x, y).type != TileType::Wall) return false;
        if (inRoom(x, y)) return false;
        if (tooCloseToStairs(x, y)) return false;
        if (anyDoorInRadius(d, x, y, 2)) return false;

        // Never carve a wall tile that borders any room footprint. This prevents
        // creating accidental door-less room entrances.
        const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
        for (const Vec2i& dv : dirs) {
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (inRoom(nx, ny)) return false;
        }

        return true;
    };

    auto openCountAt = [&](int x, int y) -> int {
        const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
        int c = 0;
        for (const Vec2i& dv : dirs) {
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (d.isPassable(nx, ny)) c++;
        }
        return c;
    };

    // Spacing mask so we don't cluster hubs/halls.
    std::vector<uint8_t> blocked(static_cast<size_t>(d.width * d.height), uint8_t{0});
    auto isBlocked = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return blocked[idx(x, y)] != 0;
    };
    auto markBlocked = [&](int cx, int cy, int rad) {
        for (int oy = -rad; oy <= rad; ++oy) {
            for (int ox = -rad; ox <= rad; ++ox) {
                const int x = cx + ox;
                const int y = cy + oy;
                if (!d.inBounds(x, y)) continue;
                blocked[idx(x, y)] = 1;
            }
        }
    };

    // --- 1) Junction hubs ---
    {
        std::vector<Vec2i> cands;
        cands.reserve(static_cast<size_t>(d.width * d.height / 20));

        for (int y = 2; y < d.height - 2; ++y) {
            for (int x = 2; x < d.width - 2; ++x) {
                if (!corridorFloor(x, y)) continue;
                if (isBlocked(x, y)) continue;
                if (tooCloseToStairs(x, y)) continue;
                if (anyDoorInRadius(d, x, y, 2)) continue;
                if (openCountAt(x, y) < 3) continue;
                cands.push_back({x, y});
            }
        }

        // Shuffle deterministically.
        for (int i = static_cast<int>(cands.size()) - 1; i > 0; --i) {
            const int j = rng.range(0, i);
            std::swap(cands[static_cast<size_t>(i)], cands[static_cast<size_t>(j)]);
        }

        int want = 1;
        if (depth >= 5) want += 1;
        if (depth >= 8 && rng.chance(0.35f)) want += 1;
        want = std::min(3, want);

        for (const Vec2i& c : cands) {
            if (d.corridorHubCount >= want) break;

            const bool full3x3 = rng.chance(0.60f);
            int carved = 0;

            // Carve a 3x3-ish footprint around the hub center.
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    if (!full3x3 && std::abs(ox) + std::abs(oy) == 2) continue; // skip corners

                    const int x = c.x + ox;
                    const int y = c.y + oy;
                    if (!safeWallToCarve(x, y)) continue;
                    d.at(x, y).type = TileType::Floor;
                    carved++;
                }
            }

            if (carved <= 0) continue;

            // Optional corner support columns (pillars). Only place in corners so we
            // don't block the actual junction.
            if (full3x3 && rng.chance(0.35f)) {
                const Vec2i corners[4] = { {c.x - 1, c.y - 1}, {c.x + 1, c.y - 1}, {c.x - 1, c.y + 1}, {c.x + 1, c.y + 1} };
                const int start = rng.range(0, 3);
                for (int k = 0; k < 4; ++k) {
                    const Vec2i p = corners[(start + k) & 3];
                    if (!d.inBounds(p.x, p.y)) continue;
                    if (inRoom(p.x, p.y)) continue;
                    if (tooCloseToStairs(p.x, p.y)) continue;
                    if (anyDoorInRadius(d, p.x, p.y, 1)) continue;
                    if (d.at(p.x, p.y).type != TileType::Floor) continue;
                    // A corner pillar never blocks the junction.
                    d.at(p.x, p.y).type = TileType::Pillar;
                    break;
                }
            }

            d.corridorHubCount += 1;
            markBlocked(c.x, c.y, 6);
        }
    }

    // --- 2) Great halls (widened long corridor segments) ---
    {
        std::vector<Vec2i> cands;
        cands.reserve(static_cast<size_t>(d.width * d.height / 15));

        auto isStraightCorridor = [&](int x, int y, Vec2i& dirOut) -> bool {
            if (!corridorFloor(x, y)) return false;
            if (anyDoorInRadius(d, x, y, 2)) return false;
            if (tooCloseToStairs(x, y)) return false;
            // Only treat passable tiles (floors/doors/stairs) as open for shape.
            const bool n = d.isPassable(x, y - 1);
            const bool s = d.isPassable(x, y + 1);
            const bool w = d.isPassable(x - 1, y);
            const bool e = d.isPassable(x + 1, y);

            const bool horiz = (w && e && !n && !s);
            const bool vert  = (n && s && !w && !e);
            if (horiz) { dirOut = {1, 0}; return true; }
            if (vert)  { dirOut = {0, 1}; return true; }
            return false;
        };

        for (int y = 2; y < d.height - 2; ++y) {
            for (int x = 2; x < d.width - 2; ++x) {
                if (isBlocked(x, y)) continue;
                Vec2i dir{0, 0};
                if (!isStraightCorridor(x, y, dir)) continue;
                cands.push_back({x, y});
            }
        }

        for (int i = static_cast<int>(cands.size()) - 1; i > 0; --i) {
            const int j = rng.range(0, i);
            std::swap(cands[static_cast<size_t>(i)], cands[static_cast<size_t>(j)]);
        }

        int want = 1;
        if (depth >= 4) want += 1;
        if (depth >= 7 && rng.chance(0.35f)) want += 1;
        want = std::min(3, want);

        const int minRun = 8;

        for (const Vec2i& c : cands) {
            if (d.corridorHallCount >= want) break;
            if (isBlocked(c.x, c.y)) continue;

            Vec2i dir{0, 0};
            if (!isStraightCorridor(c.x, c.y, dir)) continue;

            // Measure straight run length.
            int left = 0;
            for (int k = 1; k < 80; ++k) {
                Vec2i p{ c.x - dir.x * k, c.y - dir.y * k };
                Vec2i tmp{0, 0};
                if (!isStraightCorridor(p.x, p.y, tmp)) break;
                if (tmp.x != dir.x || tmp.y != dir.y) break;
                left++;
            }
            int right = 0;
            for (int k = 1; k < 80; ++k) {
                Vec2i p{ c.x + dir.x * k, c.y + dir.y * k };
                Vec2i tmp{0, 0};
                if (!isStraightCorridor(p.x, p.y, tmp)) break;
                if (tmp.x != dir.x || tmp.y != dir.y) break;
                right++;
            }

            const int runLen = left + right + 1;
            if (runLen < minRun) continue;

            const int maxWiden = std::min(10, 6 + depth / 2);
            int widenLen = rng.range(5, maxWiden);
            widenLen = std::min(widenLen, runLen - 2);
            widenLen = std::max(4, widenLen);

            const int minStart = -left;
            const int maxStart = right - (widenLen - 1);
            if (maxStart < minStart) continue;
            const int startOff = rng.range(minStart, maxStart);

            // Perpendicular direction for widening.
            const Vec2i perp = (dir.x != 0) ? Vec2i{0, 1} : Vec2i{1, 0};

            int carved = 0;

            for (int i = 0; i < widenLen; ++i) {
                const int off = startOff + i;
                const int x = c.x + dir.x * off;
                const int y = c.y + dir.y * off;
                if (!corridorFloor(x, y)) continue;
                if (anyDoorInRadius(d, x, y, 2)) continue;

                const int ax = x + perp.x;
                const int ay = y + perp.y;
                const int bx = x - perp.x;
                const int by = y - perp.y;

                const bool aOk = safeWallToCarve(ax, ay);
                const bool bOk = safeWallToCarve(bx, by);

                if (!aOk && !bOk) continue;

                if (aOk && bOk) {
                    // Mostly 2-wide, with an occasional 3-wide flare.
                    const bool carveBoth = rng.chance(0.12f);
                    if (carveBoth) {
                        d.at(ax, ay).type = TileType::Floor;
                        d.at(bx, by).type = TileType::Floor;
                        carved += 2;
                    } else {
                        if (rng.chance(0.5f)) {
                            d.at(ax, ay).type = TileType::Floor;
                        } else {
                            d.at(bx, by).type = TileType::Floor;
                        }
                        carved += 1;
                    }
                } else if (aOk) {
                    d.at(ax, ay).type = TileType::Floor;
                    carved += 1;
                } else if (bOk) {
                    d.at(bx, by).type = TileType::Floor;
                    carved += 1;
                }
            }

            if (carved < 2) continue;

            // Optional support columns: add a pillar at one end of the widened strip
            // (off to the side) to create cover without blocking traversal.
            if (rng.chance(0.25f)) {
                const int pickEnd = rng.chance(0.5f) ? 0 : (widenLen - 1);
                const int off = startOff + pickEnd;
                const int x = c.x + dir.x * off;
                const int y = c.y + dir.y * off;
                const Vec2i opts[2] = { {x + perp.x, y + perp.y}, {x - perp.x, y - perp.y} };
                const int start = rng.range(0, 1);
                for (int k = 0; k < 2; ++k) {
                    const Vec2i p = opts[(start + k) & 1];
                    if (!d.inBounds(p.x, p.y)) continue;
                    if (inRoom(p.x, p.y)) continue;
                    if (anyDoorInRadius(d, p.x, p.y, 1)) continue;
                    if (tooCloseToStairs(p.x, p.y)) continue;
                    if (d.at(p.x, p.y).type != TileType::Floor) continue;
                    // Ensure there's still a way around the pillar locally.
                    if (openCountAt(p.x, p.y) <= 1) continue;
                    d.at(p.x, p.y).type = TileType::Pillar;
                    break;
                }
            }

            d.corridorHallCount += 1;
            markBlocked(c.x, c.y, 7);
        }
    }

    return (d.corridorHubCount + d.corridorHallCount) > 0;
}



// ------------------------------------------------------------
// Perimeter service tunnels
//
// A late procgen pass that carves partial walkable tunnels along the inner
// border of the map (offset=1 from the solid boundary), then punches a few
// short connecting hatches back into the interior.
//
// Goals:
// - Add macro-level alternate routes (flanking / reduced backtracking) on
//   larger room-based floors without rewriting the core generator.
// - Keep the feature optional and conservative: only adds floor by digging
//   walls (never removes connectivity).
// - Avoid cluttering the start/end areas and avoid door spam near existing doors.
//
// Notes:
// - Intentionally keys off d.rooms.size() so cavern/maze layouts (no rooms) are
//   not affected.
// - Can optionally request a bonus cache chest (via bonusLootSpots) to reward
//   exploring the tunnels.
// ------------------------------------------------------------
static Vec2i innerRingInteriorDir(const Dungeon& d, const Vec2i& p) {
    // Prefer the axis that clearly indicates which side of the ring we're on.
    if (p.x <= 1) return {1, 0};
    if (p.x >= d.width - 2) return {-1, 0};
    if (p.y <= 1) return {0, 1};
    return {0, -1};
}

static void buildInnerPerimeterRing(const Dungeon& d, std::vector<Vec2i>& out) {
    out.clear();
    const int x0 = 1;
    const int y0 = 1;
    const int x1 = d.width - 2;
    const int y1 = d.height - 2;
    if (x1 <= x0 || y1 <= y0) return;

    const int perim = (x1 - x0 + 1) * 2 + (y1 - y0 - 1) * 2;
    out.reserve(static_cast<size_t>(std::max(0, perim)));

    for (int x = x0; x <= x1; ++x) out.push_back({x, y0});
    for (int y = y0 + 1; y <= y1; ++y) out.push_back({x1, y});
    for (int x = x1 - 1; x >= x0; --x) out.push_back({x, y1});
    for (int y = y1 - 1; y >= y0 + 1; --y) out.push_back({x0, y});
}

static bool applyPerimeterServiceTunnels(Dungeon& d, RNG& rng, DungeonBranch branch, int depth) {
    d.perimTunnelCarvedTiles = 0;
    d.perimTunnelHatchCount = 0;
    d.perimTunnelLockedCount = 0;
    d.perimTunnelCacheCount = 0;

    if (branch != DungeonBranch::Main) return false;
    // Room-based floors only: avoids cavern/maze layouts that do not use d.rooms.
    if (d.rooms.size() < 6) return false;
    if (d.width < 40 || d.height < 28) return false;
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return false;

    // Chance scales mildly with depth and map size (larger maps benefit more).
    float p = 0.16f + 0.02f * static_cast<float>(std::min(8, std::max(0, depth)));
    const int area = d.width * d.height;
    const int baseArea = Dungeon::DEFAULT_W * Dungeon::DEFAULT_H;
    if (area >= baseArea + baseArea / 6) p += 0.05f;
    p = std::clamp(p, 0.14f, 0.45f);
    if (!rng.chance(p)) return false;

    std::vector<Vec2i> ring;
    buildInnerPerimeterRing(d, ring);
    const int N = static_cast<int>(ring.size());
    if (N < 24) return false;

    auto carveRingArc = [&](int start, int len, float gapChance) {
        int carved = 0;
        for (int i = 0; i < len; ++i) {
            const Vec2i p = ring[static_cast<size_t>((start + i) % N)];
            if (!d.inBounds(p.x, p.y)) continue;
            Tile& t = d.at(p.x, p.y);
            if (t.type != TileType::Wall) continue;
            if (rng.chance(gapChance)) continue;
            t.type = TileType::Floor;
            carved += 1;
        }
        return carved;
    };

    // Carve a long primary arc (often looks like a broken ring), plus an optional secondary arc.
    float cover = 0.60f + 0.03f * static_cast<float>(std::min(8, std::max(0, depth)));
    cover += static_cast<float>(rng.range(-6, 6)) * 0.01f;
    cover = std::clamp(cover, 0.55f, 0.92f);

    const int start1 = rng.range(0, N - 1);
    const int len1 = std::clamp(static_cast<int>(std::round(cover * static_cast<float>(N))), N / 3, N);
    d.perimTunnelCarvedTiles += carveRingArc(start1, len1, 0.09f);

    if (rng.chance(0.35f) && N > 60) {
        const int jitter = rng.range(-N / 10, N / 10);
        const int start2 = (start1 + N / 2 + jitter + N) % N;
        const int len2 = std::clamp(N / 4 + rng.range(-N / 12, N / 12), N / 6, N / 2);
        d.perimTunnelCarvedTiles += carveRingArc(start2, len2, 0.11f);
    }

    // If we didn't carve anything new, don't claim the feature triggered.
    if (d.perimTunnelCarvedTiles <= 0) return false;

    const auto distFromUp = bfsDistanceMap(d, d.stairsUp);
    auto distAt = [&](const Vec2i& p) -> int {
        if (!d.inBounds(p.x, p.y)) return -1;
        const size_t ii = static_cast<size_t>(p.y * d.width + p.x);
        if (ii >= distFromUp.size()) return -1;
        return distFromUp[ii];
    };

    auto farFromStairs = [&](const Vec2i& p) {
        const int du = std::abs(p.x - d.stairsUp.x) + std::abs(p.y - d.stairsUp.y);
        const int dd = d.inBounds(d.stairsDown.x, d.stairsDown.y)
            ? (std::abs(p.x - d.stairsDown.x) + std::abs(p.y - d.stairsDown.y))
            : 999;
        return du >= 8 && dd >= 8;
    };

    // How many hatches do we want? (Large maps + deep floors get more.)
    int wantHatches = 2;
    if (area >= baseArea + baseArea / 5) wantHatches += 1;
    if (depth >= 6) wantHatches += 1;
    wantHatches = std::clamp(wantHatches, 2, 4);

    struct HatchCand {
        Vec2i p;
        Vec2i dir;
        int score = 0;
    };

    std::vector<HatchCand> cands;
    cands.reserve(ring.size());

	for (const Vec2i& pos : ring) {
	    if (!d.inBounds(pos.x, pos.y)) continue;
	    const TileType tt = d.at(pos.x, pos.y).type;
        if (tt == TileType::Chasm || tt == TileType::Pillar || tt == TileType::Boulder) continue;
	    if (!farFromStairs(pos)) continue;
	    if (anyDoorInRadius(d, pos.x, pos.y, 2)) continue;

	    const Vec2i dir = innerRingInteriorDir(d, pos);
	    const Vec2i first{pos.x + dir.x, pos.y + dir.y};
        if (!d.inBounds(first.x, first.y)) continue;
        if (d.at(first.x, first.y).type != TileType::Wall) continue;

	    const int sc = std::abs(pos.x - d.stairsUp.x) + std::abs(pos.y - d.stairsUp.y);
	    cands.push_back({pos, dir, sc});
    }

    // Prefer hatches far from stairs so the tunnels tend to create meaningful alternative routes.
    std::sort(cands.begin(), cands.end(), [&](const HatchCand& a, const HatchCand& b) {
        if (a.score != b.score) return a.score > b.score;
        // Deterministic tie-break.
        const uint32_t ha = hash32(static_cast<uint32_t>(a.p.x * 73856093u) ^ static_cast<uint32_t>(a.p.y * 19349663u) ^ rng.state);
        const uint32_t hb = hash32(static_cast<uint32_t>(b.p.x * 73856093u) ^ static_cast<uint32_t>(b.p.y * 19349663u) ^ rng.state);
        return ha > hb;
    });

    std::vector<Vec2i> placed;
    placed.reserve(4);

    bool placedPassableHatch = false;

    auto tryPlaceHatch = [&](const HatchCand& hc, bool allowLocked) -> bool {
        const Vec2i p = hc.p;
        const Vec2i dir = hc.dir;
        const Vec2i first{p.x + dir.x, p.y + dir.y};
        if (!d.inBounds(first.x, first.y)) return false;
        if (d.at(first.x, first.y).type != TileType::Wall) return false;
        if (!farFromStairs(p) || !farFromStairs(first)) return false;
        if (anyDoorInRadius(d, first.x, first.y, 2)) return false;

        std::vector<TileChange> changes;
        int localCarved = 0;

        auto recordSet = [&](int x, int y, TileType nt) {
            if (!d.inBounds(x, y)) return;
            if (isStairsTile(d, x, y)) return;
            Tile& t = d.at(x, y);
            if (t.type == nt) return;
            changes.push_back({x, y, t.type});
            t.type = nt;
        };

        // Ensure the ring entry tile is walkable too (in case the ring arc skipped it).
        if (d.at(p.x, p.y).type == TileType::Wall) {
            recordSet(p.x, p.y, TileType::Floor);
            localCarved += 1;
        }

        const int maxLen = std::clamp(12 + depth * 2, 12, 24);
        Vec2i cur = first;
        bool success = false;
        for (int step = 0; step < maxLen; ++step) {
            if (!d.inBounds(cur.x, cur.y)) break;
            const TileType tt = d.at(cur.x, cur.y).type;

            if (d.isPassable(cur.x, cur.y)) {
                // Only connect to the main component (reachable from stairs before we dig).
                if (distAt(cur) >= 0) success = true;
                break;
            }

            if (tt != TileType::Wall) break; // don't dig through features/doors
            recordSet(cur.x, cur.y, TileType::Floor);
            localCarved += 1;
            cur.x += dir.x;
            cur.y += dir.y;
        }

        if (!success || localCarved <= 0) {
            undoChanges(d, changes);
            return false;
        }

        d.perimTunnelCarvedTiles += localCarved;

        // Door on the first interior tile: reads as a small maintenance hatch.
        TileType doorType = TileType::DoorClosed;
        if (allowLocked && placedPassableHatch && depth >= 4 && rng.chance(0.18f)) {
            doorType = TileType::DoorLocked;
        }
        d.at(first.x, first.y).type = doorType;
        d.perimTunnelHatchCount += 1;
        if (doorType == TileType::DoorLocked) d.perimTunnelLockedCount += 1;
        if (doorType != TileType::DoorLocked) placedPassableHatch = true;

        placed.push_back(p);
        return true;
    };

    for (const HatchCand& hc : cands) {
        if (d.perimTunnelHatchCount >= wantHatches) break;

        bool tooClose = false;
        for (const Vec2i& pp : placed) {
            if (std::abs(pp.x - hc.p.x) + std::abs(pp.y - hc.p.y) < 18) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) continue;

        const bool allowLocked = (d.perimTunnelHatchCount > 0);
        (void)tryPlaceHatch(hc, allowLocked);
    }

    // Rare safety: ensure at least one passable hatch exists if we placed any hatches at all.
    if (!placedPassableHatch && d.perimTunnelHatchCount > 0) {
        for (const HatchCand& hc : cands) {
            const Vec2i first{hc.p.x + hc.dir.x, hc.p.y + hc.dir.y};
            if (!d.inBounds(first.x, first.y)) continue;
            if (d.at(first.x, first.y).type == TileType::DoorLocked) {
                d.at(first.x, first.y).type = TileType::DoorClosed;
                d.perimTunnelLockedCount = std::max(0, d.perimTunnelLockedCount - 1);
                placedPassableHatch = true;
                break;
            }
        }
    }

    // Optional exploration reward: request a bonus chest somewhere in the tunnels.
    if (depth >= 2 && rng.chance(0.35f)) {
        const auto distAfter = bfsDistanceMap(d, d.stairsUp);
        auto distAfterAt = [&](const Vec2i& p) -> int {
            if (!d.inBounds(p.x, p.y)) return -1;
            const size_t ii = static_cast<size_t>(p.y * d.width + p.x);
            if (ii >= distAfter.size()) return -1;
            return distAfter[ii];
        };

        Vec2i best{-1, -1};
        int bestD = -1;
	    for (const Vec2i& pos : ring) {
	        if (!d.inBounds(pos.x, pos.y)) continue;
	        if (d.at(pos.x, pos.y).type != TileType::Floor) continue;
	        if (anyDoorInRadius(d, pos.x, pos.y, 1)) continue;
	        const int dd = distAfterAt(pos);
            if (dd < 0) continue;
            if (dd < 22) continue;
            if (dd > bestD) {
                bestD = dd;
	            best = pos;
            }
        }

        if (d.inBounds(best.x, best.y)) {
            bool dup = false;
            for (const Vec2i& q : d.bonusLootSpots) {
                if (q.x == best.x && q.y == best.y) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                d.bonusLootSpots.push_back(best);
                d.perimTunnelCacheCount += 1;
            }
        }
    }

    return (d.perimTunnelCarvedTiles > 0);
}



// ------------------------------------------------------------
// Burrow crosscuts (A*-dug wall tunnels)
//
// A late procgen pass that carves 1-2 long tunnels *through solid wall mass*
// between two far-apart corridor points (outside any room rectangles), gated
// by doors (often locked/secret on one end). This creates dramatic optional
// shortcuts that are fundamentally different from adjacent-wall shortcut doors
// (which only bridge across a single wall tile).
//
// Safety:
// - Only applies to room-based floors with a healthy room count.
// - Never tunnels through any room rectangle (A* forbidden mask).
// - Avoids stairs vicinity and existing doors.
// - Only converts walls to floor (never reduces connectivity).
// ------------------------------------------------------------

inline bool crosscutTileOk(TileType t) {
    return (t == TileType::Wall || t == TileType::Floor);
}

inline int crosscutStepCost(TileType t) {
    // Strongly prefer digging new tunnel through wall mass.
    if (t == TileType::Wall) return 8;
    // Existing floor is allowed but expensive; prevents the A* from simply walking
    // the already-carved corridor network.
    return 34;
}

static bool findCrosscutAStarPath(const Dungeon& d, RNG& rng, Vec2i start, Vec2i goal,
                                 const std::vector<uint8_t>& roomMask,
                                 std::vector<Vec2i>& outPath) {
    outPath.clear();

    const int W = d.width;
    const int H = d.height;

    auto inside = [&](int x, int y) {
        return x >= 1 && y >= 1 && x < W - 1 && y < H - 1;
    };

    if (!inside(start.x, start.y) || !inside(goal.x, goal.y)) return false;

    auto idx = [&](int x, int y) { return y * W + x; };

    const int DIR_NONE = 4;
    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    // Shuffle direction order once per tunnel so shapes vary without excessive RNG usage.
    int order[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; --i) {
        int j = rng.range(0, i);
        std::swap(order[i], order[j]);
    }

    auto inRoom = [&](int x, int y) -> bool {
        if (roomMask.empty()) return false;
        const size_t ii = static_cast<size_t>(idx(x, y));
        if (ii >= roomMask.size()) return false;
        return roomMask[ii] != 0;
    };

    auto nearRoomPenalty = [&](int x, int y) -> int {
        // Higher penalty than corridor routing: crosscuts should not skim rooms.
        if (roomMask.empty()) return 0;
        for (int k = 0; k < 4; ++k) {
            int nx = x + dirs[k][0];
            int ny = y + dirs[k][1];
            if (!inside(nx, ny)) continue;
            if (inRoom(nx, ny)) return 4;
        }
        return 0;
    };

    auto nearBorderPenalty = [&](int x, int y) -> int {
        // Keep tunnels away from the outer wall for readability and to avoid fighting
        // the perimeter service tunnel pass.
        const int m = std::min(std::min(x, y), std::min(W - 1 - x, H - 1 - y));
        if (m <= 2) return 6;
        if (m <= 3) return 3;
        return 0;
    };

    auto heuristic = [&](int x, int y) {
        // Manhattan distance; admissible because each step costs >= 8.
        return (std::abs(x - goal.x) + std::abs(y - goal.y)) * 8;
    };

    auto stateOf = [&](int x, int y, int dir) {
        return (idx(x, y) * 5 + dir);
    };

    const int N = W * H;
    const int S = N * 5;
    const int INF = 1'000'000'000;

    std::vector<int> gCost(static_cast<size_t>(S), INF);
    std::vector<int> parent(static_cast<size_t>(S), -1);
    std::vector<uint8_t> closed(static_cast<size_t>(S), uint8_t{0});

    const int startState = stateOf(start.x, start.y, DIR_NONE);
    gCost[static_cast<size_t>(startState)] = 0;

    std::priority_queue<AStarEntry, std::vector<AStarEntry>, AStarEntryCmp> open;
    open.push({heuristic(start.x, start.y), 0, startState});

    int goalStateFound = -1;

    while (!open.empty()) {
        AStarEntry cur = open.top();
        open.pop();

        const int state = cur.state;
        if (state < 0 || state >= S) continue;
        if (closed[static_cast<size_t>(state)] != 0) continue;
        closed[static_cast<size_t>(state)] = 1;

        const int cell = state / 5;
        const int prevDir = state % 5;
        const int cx = cell % W;
        const int cy = cell / W;

        if (cx == goal.x && cy == goal.y) {
            goalStateFound = state;
            break;
        }

        const int gHere = gCost[static_cast<size_t>(state)];
        if (gHere >= INF) continue;

        for (int oi = 0; oi < 4; ++oi) {
            const int nd = order[oi];
            const int nx = cx + dirs[nd][0];
            const int ny = cy + dirs[nd][1];
            if (!inside(nx, ny)) continue;

            // Never tunnel through rooms (endpoints are corridor-wall tiles outside rooms).
            if (!(nx == goal.x && ny == goal.y) && inRoom(nx, ny)) continue;

            const TileType tt = d.at(nx, ny).type;
            if (!crosscutTileOk(tt)) continue;

            const int step = crosscutStepCost(tt);
            const int turnPenalty = (prevDir != DIR_NONE && nd != prevDir) ? 7 : 0;

            const int g2 = gHere + step + turnPenalty + nearRoomPenalty(nx, ny) + nearBorderPenalty(nx, ny);
            const int ns = stateOf(nx, ny, nd);

            if (g2 < gCost[static_cast<size_t>(ns)]) {
                gCost[static_cast<size_t>(ns)] = g2;
                parent[static_cast<size_t>(ns)] = state;

                // Deterministic 0/1 tie-breaker.
                const int jitter = (nx * 17 + ny * 31 + nd * 7) & 1;
                const int f2 = g2 + heuristic(nx, ny) + jitter;
                open.push({f2, g2, ns});
            }
        }
    }

    if (goalStateFound < 0) return false;

    // Reconstruct path.
    std::vector<Vec2i> path;
    path.reserve(256);

    int st = goalStateFound;
    while (st >= 0) {
        const int cell = st / 5;
        const int px = cell % W;
        const int py = cell / W;
        path.push_back({px, py});
        if (st == startState) break;
        st = parent[static_cast<size_t>(st)];
    }

    if (path.empty()) return false;
    if (path.back().x != start.x || path.back().y != start.y) return false;

    std::reverse(path.begin(), path.end());
    outPath.swap(path);
    return true;
}

static bool applyBurrowCrosscuts(Dungeon& d, RNG& rng, DungeonBranch branch, int depth) {
    d.crosscutTunnelCount = 0;
    d.crosscutCarvedTiles = 0;
    d.crosscutDoorLockedCount = 0;
    d.crosscutDoorSecretCount = 0;

    if (branch != DungeonBranch::Main) return false;
    // Keep early floors readable; let the player learn baseline doors/shortcuts first.
    if (depth <= 2) return false;
    // Only apply on room-based floors (corridor network exists, and roomMask is meaningful).
    if (d.rooms.size() < 6) return false;
    if (d.width < 40 || d.height < 28) return false;
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return false;

    float p = 0.10f + 0.02f * static_cast<float>(std::min(10, std::max(0, depth)));
    const int area = d.width * d.height;
    const int baseArea = Dungeon::DEFAULT_W * Dungeon::DEFAULT_H;
    if (area >= baseArea + baseArea / 5) p += 0.05f;
    if (area >= baseArea + baseArea / 2) p += 0.05f;
    p = std::clamp(p, 0.10f, 0.55f);
    if (!rng.chance(p)) return false;

    int want = 1;
    if (depth >= 7 && rng.chance(0.35f)) want += 1;
    if (area >= baseArea + baseArea / 3 && rng.chance(0.25f)) want += 1;
    want = std::clamp(want, 1, 2);

    // Build an in-room mask so crosscuts never tunnel through room rectangles.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    const auto distFromUp = bfsDistanceMap(d, d.stairsUp);
    auto distUpAt = [&](int x, int y) -> int {
        if (!d.inBounds(x, y)) return -1;
        const size_t ii = static_cast<size_t>(y * d.width + x);
        if (ii >= distFromUp.size()) return -1;
        return distFromUp[ii];
    };

    auto farFromStairs = [&](int x, int y) {
        const int du = std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y);
        const int dd = d.inBounds(d.stairsDown.x, d.stairsDown.y)
            ? (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y))
            : 9999;
        return du >= 9 && dd >= 9;
    };

    struct EndCand {
        Vec2i floor;
        Vec2i doors[4];
        int doorCount = 0;
        int score = 0;
    };

    std::vector<EndCand> cands;
    cands.reserve(static_cast<size_t>(d.width * d.height / 8));

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    auto inRoomAt = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return inRoom[static_cast<size_t>(y * d.width + x)] != 0;
    };

    for (int y = 2; y < d.height - 2; ++y) {
        for (int x = 2; x < d.width - 2; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (inRoomAt(x, y)) continue;
            if (!farFromStairs(x, y)) continue;
            if (anyDoorInRadius(d, x, y, 2)) continue;

            EndCand ec;
            ec.floor = {x, y};
            ec.doorCount = 0;

            for (int k = 0; k < 4; ++k) {
                const int wx = x + dirs[k][0];
                const int wy = y + dirs[k][1];
                const int bx = wx + dirs[k][0];
                const int by = wy + dirs[k][1];

                if (!d.inBounds(wx, wy) || !d.inBounds(bx, by)) continue;
                if (!farFromStairs(wx, wy) || !farFromStairs(bx, by)) continue;

                if (d.at(wx, wy).type != TileType::Wall) continue;
                if (d.at(bx, by).type != TileType::Wall) continue;
                if (inRoomAt(wx, wy) || inRoomAt(bx, by)) continue;

                // Keep entrances readable and avoid door clutter.
                if (anyDoorInRadius(d, wx, wy, 2)) continue;

                ec.doors[ec.doorCount] = {wx, wy};
                ec.doorCount += 1;
                if (ec.doorCount >= 4) break;
            }

            if (ec.doorCount <= 0) continue;

            const int du = distUpAt(x, y);
            ec.score = (du >= 0) ? du : (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y));
            cands.push_back(ec);
        }
    }

    if (cands.size() < 8) return false;

    // Prefer endpoints farther from the up stairs so crosscuts tend to impact mid/late traversal.
    std::sort(cands.begin(), cands.end(), [&](const EndCand& a, const EndCand& b) {
        return a.score > b.score;
    });

    std::vector<Vec2i> usedDoors;
    usedDoors.reserve(static_cast<size_t>(want) * 2);

    auto doorTooClose = [&](const Vec2i& p) {
        for (const Vec2i& q : usedDoors) {
            if (std::abs(p.x - q.x) + std::abs(p.y - q.y) < 18) return true;
        }
        return false;
    };

    auto pickDoorToward = [&](const EndCand& ec, const Vec2i& toward) -> Vec2i {
        Vec2i best = ec.doors[0];
        int bestScore = 1'000'000;
        for (int i = 0; i < ec.doorCount; ++i) {
            const Vec2i p = ec.doors[i];
            int sc = std::abs(p.x - toward.x) + std::abs(p.y - toward.y);
            // Favor doors that "point" into thicker wall mass (more wall neighbors).
            int wallN = 0;
            for (int k = 0; k < 4; ++k) {
                const int nx = p.x + dirs[k][0];
                const int ny = p.y + dirs[k][1];
                if (!d.inBounds(nx, ny)) continue;
                if (d.at(nx, ny).type == TileType::Wall) wallN += 1;
            }
            sc -= wallN * 2;
            sc += rng.range(0, 2); // tiny jitter

            if (sc < bestScore) {
                bestScore = sc;
                best = p;
            }
        }
        return best;
    };

    auto recordSet = [&](int x, int y, TileType nt, std::vector<TileChange>& changes) {
        if (!d.inBounds(x, y)) return;
        if (isStairsTile(d, x, y)) return;
        Tile& t = d.at(x, y);
        if (t.type == nt) return;
        changes.push_back({x, y, t.type});
        t.type = nt;
    };

    const int maxAttempts = 40 + 25 * want;

    for (int attempt = 0; attempt < maxAttempts && d.crosscutTunnelCount < want; ++attempt) {
        // Pick a start endpoint among the top slice (far from stairs).
        const int slice = std::max(10, static_cast<int>(cands.size()) / 5);
        const int ia = rng.range(0, slice - 1);
        const EndCand& A = cands[static_cast<size_t>(ia)];

        // Compute BFS distance to measure how meaningful the shortcut would be.
        const auto distA = bfsDistanceMap(d, A.floor);

        const int minDist = std::clamp(26 + depth * 2, 32, 64);

        int bestI = -1;
        int bestD = -1;

        // Scan a subset of candidates for speed, with a bias toward far endpoints.
        const int scan = std::min(static_cast<int>(cands.size()), 220);
        for (int si = 0; si < scan; ++si) {
            const int ib = rng.range(0, static_cast<int>(cands.size()) - 1);
            if (ib == ia) continue;
            const EndCand& B = cands[static_cast<size_t>(ib)];

            const size_t ii = static_cast<size_t>(B.floor.y * d.width + B.floor.x);
            if (ii >= distA.size()) continue;
            const int dd = distA[ii];
            if (dd < minDist) continue;

            // Endpoints should not be too close in raw space either (avoid micro bypasses).
            const int man = std::abs(A.floor.x - B.floor.x) + std::abs(A.floor.y - B.floor.y);
            if (man < 18) continue;

            if (dd > bestD) {
                bestD = dd;
                bestI = ib;
            }
        }

        if (bestI < 0) continue;
        const EndCand& B = cands[static_cast<size_t>(bestI)];

        // Choose door tiles that point roughly toward the other endpoint.
        const Vec2i doorA = pickDoorToward(A, B.floor);
        const Vec2i doorB = pickDoorToward(B, A.floor);

        if (doorTooClose(doorA) || doorTooClose(doorB)) continue;
        if (anyDoorInRadius(d, doorA.x, doorA.y, 2) || anyDoorInRadius(d, doorB.x, doorB.y, 2)) continue;

        std::vector<Vec2i> path;
        if (!findCrosscutAStarPath(d, rng, doorA, doorB, inRoom, path)) continue;

        // Reject absurdly long tunnels.
        const int maxLen = std::clamp(72 + depth * 5, 78, 140);
        if (static_cast<int>(path.size()) > maxLen) continue;

        // Require the tunnel to actually dig through wall mass (not mostly reuse existing floors).
        int wallTiles = 0;
	for (const Vec2i& pos : path) {
	    if (!d.inBounds(pos.x, pos.y)) continue;
	    if (d.at(pos.x, pos.y).type == TileType::Wall) wallTiles += 1;
        }

        // Door endpoints are walls by construction.
        const int carvedFloors = std::max(0, wallTiles - 2);
        const int minCarve = std::clamp(10 + depth, 12, 26);
        if (carvedFloors < minCarve) continue;
        if (carvedFloors * 2 < static_cast<int>(path.size())) continue;

        // Apply changes with rollback support.
        std::vector<TileChange> changes;
        changes.reserve(static_cast<size_t>(wallTiles) + 8);

        // Carve interior.
        for (size_t i = 0; i < path.size(); ++i) {
	    const Vec2i pos = path[i];
	    if (!d.inBounds(pos.x, pos.y)) continue;

            // Keep endpoints reserved for doors.
	    if (pos.x == doorA.x && pos.y == doorA.y) continue;
	    if (pos.x == doorB.x && pos.y == doorB.y) continue;

	    if (d.at(pos.x, pos.y).type == TileType::Wall) {
	        recordSet(pos.x, pos.y, TileType::Floor, changes);
            }
        }

        // Gate doors. Always keep at least one passable entrance.
        TileType doorTypeA = TileType::DoorClosed;
        TileType doorTypeB = TileType::DoorClosed;

        if (depth >= 5 && rng.chance(0.30f)) {
            doorTypeB = TileType::DoorLocked;
        } else if (depth >= 4 && rng.chance(0.22f)) {
            doorTypeB = TileType::DoorSecret;
        } else if (rng.chance(0.12f)) {
            // Rare: both ends visible, creating a very readable macro shortcut.
            doorTypeB = TileType::DoorOpen;
        }

        recordSet(doorA.x, doorA.y, doorTypeA, changes);
        recordSet(doorB.x, doorB.y, doorTypeB, changes);

        // Sanity: ensure the corridor-side neighbors are still floors (we didn't dig into a room).
        // If something is off, roll back.
        if (findRoomContaining(d, doorA.x, doorA.y) != nullptr || findRoomContaining(d, doorB.x, doorB.y) != nullptr) {
            undoChanges(d, changes);
            continue;
        }

        // Success.
        d.crosscutTunnelCount += 1;
        d.crosscutCarvedTiles += carvedFloors;
        if (doorTypeA == TileType::DoorLocked) d.crosscutDoorLockedCount += 1;
        if (doorTypeA == TileType::DoorSecret) d.crosscutDoorSecretCount += 1;
        if (doorTypeB == TileType::DoorLocked) d.crosscutDoorLockedCount += 1;
        if (doorTypeB == TileType::DoorSecret) d.crosscutDoorSecretCount += 1;

        usedDoors.push_back(doorA);
        usedDoors.push_back(doorB);
    }

    return (d.crosscutTunnelCount > 0);
}



// ------------------------------------------------------------
// Secret crawlspace networks (hidden wall passages)
//
// A late procgen pass that carves a small, 1-tile-wide network of tunnels
// entirely inside solid wall mass, then connects it back to the main corridor
// graph through 2-3 secret doors.
//
// Design goals:
// - Create "castle vibes" hidden bypass routes without turning the whole level
//   into Swiss cheese.
// - Keep the feature fully optional and conservative: it only digs *new* floor
//   inside walls and never alters existing critical connectivity.
// - Avoid accidental open connections: the crawlspace is kept at least 1 wall
//   tile away from existing passable space except at the deliberate secret-door
//   entrances.
// - Optionally request a bonus cache chest deep inside the crawlspace.
// ------------------------------------------------------------
static bool applySecretCrawlspaceNetwork(Dungeon& d, RNG& rng, DungeonBranch branch, int depth, GenKind g) {
    d.crawlspaceNetworkCount = 0;
    d.crawlspaceCarvedTiles = 0;
    d.crawlspaceDoorCount = 0;
    d.crawlspaceCacheCount = 0;

    if (branch != DungeonBranch::Main) return false;
    // Keep early floors readable; let the player learn baseline doors/shortcuts first.
    if (depth <= 2) return false;
    (void)g; // heuristic-gated by room count / wall mass instead of generator kind.

    // Only apply on room-based floors with meaningful wall mass.
    if (d.rooms.size() < 6) return false;
    if (d.width < 40 || d.height < 28) return false;
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return false;

    // Chance scales mildly with depth and map size.
    float p = 0.10f + 0.018f * static_cast<float>(std::min(12, std::max(0, depth)));
    const int area = d.width * d.height;
    const int baseArea = Dungeon::DEFAULT_W * Dungeon::DEFAULT_H;
    if (area >= baseArea + baseArea / 6) p += 0.04f;
    if (area >= baseArea + baseArea / 3) p += 0.04f;
    p = std::clamp(p, 0.10f, 0.42f);
    if (!rng.chance(p)) return false;

    const int W = d.width;
    const int H = d.height;
    auto idx = [&](int x, int y) -> int { return y * W + x; };

    // Build an in-room mask so we never dig through room rectangles.
    std::vector<uint8_t> inRoom(static_cast<size_t>(W * H), uint8_t{0});
    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(idx(x, y))] = 1;
            }
        }
    }

    auto inRoomAt = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return inRoom[static_cast<size_t>(idx(x, y))] != 0;
    };

    auto farFromStairs = [&](int x, int y) {
        const int du = std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y);
        const int dd = d.inBounds(d.stairsDown.x, d.stairsDown.y)
            ? (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y))
            : 9999;
        return du >= 9 && dd >= 9;
    };

    auto nearAnyPassable = [&](int x, int y, int r) -> bool {
        for (int oy = -r; oy <= r; ++oy) {
            for (int ox = -r; ox <= r; ++ox) {
                const int nx = x + ox;
                const int ny = y + oy;
                if (!d.inBounds(nx, ny)) continue;
                if (d.isPassable(nx, ny)) return true;
            }
        }
        return false;
    };

    auto looksLikeWallPocket = [&](int x, int y) -> bool {
        // Require substantial wall mass in a 5x5 window.
        int walls = 0;
        int cells = 0;
        for (int oy = -2; oy <= 2; ++oy) {
            for (int ox = -2; ox <= 2; ++ox) {
                const int nx = x + ox;
                const int ny = y + oy;
                if (!d.inBounds(nx, ny)) continue;
                cells += 1;
                if (inRoomAt(nx, ny)) continue;
                if (d.at(nx, ny).type == TileType::Wall) walls += 1;
            }
        }
        if (cells <= 0) return false;
        return walls >= 18;
    };

    auto tooNearBorder = [&](int x, int y) {
        const int m = std::min(std::min(x, y), std::min(W - 1 - x, H - 1 - y));
        return m <= 2;
    };

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    // Attempt to build a network a few times; if we can't place entrances, roll back.
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<TileChange> changes;
        changes.reserve(512);

        auto recordSet = [&](int x, int y, TileType nt) {
            if (!d.inBounds(x, y)) return;
            if (isStairsTile(d, x, y)) return;
            Tile& t = d.at(x, y);
            if (t.type == nt) return;
            changes.push_back({x, y, t.type});
            t.type = nt;
        };

        // Pick a seed wall tile inside wall mass but not *too* far from existing corridors.
        Vec2i seed{-1, -1};
        for (int tries = 0; tries < 420; ++tries) {
            const int x = rng.range(3, W - 4);
            const int y = rng.range(3, H - 4);
            if (d.at(x, y).type != TileType::Wall) continue;
            if (inRoomAt(x, y)) continue;
            if (tooNearBorder(x, y)) continue;
            if (!farFromStairs(x, y)) continue;
            if (anyDoorInRadius(d, x, y, 3)) continue;
            // Ensure there is *some* nearby open space so entrances are plausible.
            if (!nearAnyPassable(x, y, 12)) continue;
            if (!looksLikeWallPocket(x, y)) continue;
            seed = {x, y};
            break;
        }

        if (!d.inBounds(seed.x, seed.y)) {
            // Couldn't find a suitable wall pocket.
            return false;
        }

        // Carve a narrow tunnel network by "branchy" random walks.
        const int targetTiles = std::clamp(70 + depth * 6 + rng.range(-10, 18), 70, 160);
        std::vector<uint8_t> carved(static_cast<size_t>(W * H), uint8_t{0});
        std::vector<Vec2i> frontier;
        frontier.reserve(12);

        auto markCarved = [&](int x, int y) {
            const int ii = idx(x, y);
            if (ii < 0) return;
            carved[static_cast<size_t>(ii)] = 1;
        };

        auto isCarved = [&](int x, int y) -> bool {
            if (!d.inBounds(x, y)) return false;
            return carved[static_cast<size_t>(idx(x, y))] != 0;
        };

        auto adjacentToNonCrawlspacePassable = [&](int x, int y) -> bool {
            // Prevent accidentally opening the crawlspace to the main layout without a door.
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dirs[k][0];
                const int ny = y + dirs[k][1];
                if (!d.inBounds(nx, ny)) continue;
                if (d.isPassable(nx, ny) && !isCarved(nx, ny)) return true;
            }
            return false;
        };

        auto canCarve = [&](int x, int y) -> bool {
            if (!d.inBounds(x, y)) return false;
            if (tooNearBorder(x, y)) return false;
            if (inRoomAt(x, y)) return false;
            if (d.at(x, y).type != TileType::Wall) return false;
            if (anyDoorInRadius(d, x, y, 2)) return false;
            if (adjacentToNonCrawlspacePassable(x, y)) return false;

            // Keep the crawlspace embedded in wall mass (avoid carving adjacent to open air
            // via diagonals as well, which can make secret doors visually "obvious").
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    const int nx = x + ox;
                    const int ny = y + oy;
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.isPassable(nx, ny) && !isCarved(nx, ny)) return false;
                }
            }
            return true;
        };

        // Seed.
        recordSet(seed.x, seed.y, TileType::Floor);
        markCarved(seed.x, seed.y);
        d.crawlspaceCarvedTiles += 1;
        frontier.push_back(seed);

        int guard = 0;
        const int guardMax = targetTiles * 12;
        while (d.crawlspaceCarvedTiles < targetTiles && !frontier.empty() && guard < guardMax) {
            guard += 1;

            const int fi = rng.range(0, static_cast<int>(frontier.size()) - 1);
            const Vec2i cur = frontier[static_cast<size_t>(fi)];

            // Shuffle directions.
            int order[4] = {0, 1, 2, 3};
            for (int i = 3; i > 0; --i) {
                const int j = rng.range(0, i);
                std::swap(order[i], order[j]);
            }

            bool moved = false;
            for (int oi = 0; oi < 4; ++oi) {
                const int k = order[oi];
                const int nx = cur.x + dirs[k][0];
                const int ny = cur.y + dirs[k][1];
                if (!canCarve(nx, ny)) continue;

                recordSet(nx, ny, TileType::Floor);
                markCarved(nx, ny);
                d.crawlspaceCarvedTiles += 1;

                // Branching: sometimes keep the old frontier node and add a new one.
                if (rng.chance(0.22f) && frontier.size() < 10) {
                    frontier.push_back({nx, ny});
                } else {
                    frontier[static_cast<size_t>(fi)] = {nx, ny};
                }

                // Rarely punch a tiny 2x2 nook (still embedded) to create little "nodes".
                if (rng.chance(0.08f) && d.crawlspaceCarvedTiles + 2 < targetTiles) {
                    const int px = nx + dirs[(k + 1) & 3][0];
                    const int py = ny + dirs[(k + 1) & 3][1];
                    const int qx = nx + dirs[(k + 2) & 3][0];
                    const int qy = ny + dirs[(k + 2) & 3][1];
                    if (canCarve(px, py)) {
                        recordSet(px, py, TileType::Floor);
                        markCarved(px, py);
                        d.crawlspaceCarvedTiles += 1;
                    }
                    if (canCarve(qx, qy)) {
                        recordSet(qx, qy, TileType::Floor);
                        markCarved(qx, qy);
                        d.crawlspaceCarvedTiles += 1;
                    }
                }

                moved = true;
                break;
            }

            if (!moved) {
                // Dead-end: remove this frontier tip.
                frontier[static_cast<size_t>(fi)] = frontier.back();
                frontier.pop_back();
            }
        }

        if (d.crawlspaceCarvedTiles < 30) {
            // Too small to matter; roll back and try a new seed.
            undoChanges(d, changes);
            d.crawlspaceCarvedTiles = 0;
            continue;
        }

        // Identify secret-door entrance candidates.
        const auto distFromUp = bfsDistanceMap(d, d.stairsUp);
        auto distUpAt = [&](int x, int y) -> int {
            if (!d.inBounds(x, y)) return -1;
            const size_t ii = static_cast<size_t>(idx(x, y));
            if (ii >= distFromUp.size()) return -1;
            return distFromUp[ii];
        };

        struct EntryCand {
            Vec2i door;    // wall tile to become DoorSecret
            Vec2i hall;    // corridor-side floor tile
            Vec2i inside;  // crawlspace-side floor tile
            int dist = 0;  // distance from stairsUp to hall
        };

        std::vector<EntryCand> entries;
        entries.reserve(static_cast<size_t>(d.crawlspaceCarvedTiles));

        for (int y = 2; y < H - 2; ++y) {
            for (int x = 2; x < W - 2; ++x) {
                if (!isCarved(x, y)) continue;

                for (int k = 0; k < 4; ++k) {
                    const int dx = dirs[k][0];
                    const int dy = dirs[k][1];

                    const int doorX = x + dx;
                    const int doorY = y + dy;
                    const int hallX = doorX + dx;
                    const int hallY = doorY + dy;

                    if (!d.inBounds(doorX, doorY) || !d.inBounds(hallX, hallY)) continue;
                    if (d.at(doorX, doorY).type != TileType::Wall) continue;
                    if (inRoomAt(doorX, doorY) || inRoomAt(hallX, hallY)) continue;
                    if (isStairsTile(d, hallX, hallY)) continue;

                    // Corridor side must be plain floor for readability.
                    if (d.at(hallX, hallY).type != TileType::Floor) continue;
                    if (!d.isPassable(hallX, hallY)) continue;

                    // Keep entrances away from existing doors and stairs.
                    if (!farFromStairs(doorX, doorY)) continue;
                    if (!farFromStairs(hallX, hallY)) continue;
                    if (anyDoorInRadius(d, doorX, doorY, 2)) continue;

                    const int dd = distUpAt(hallX, hallY);
                    if (dd < 0) continue;
                    if (dd < 14) continue; // don't clutter the immediate spawn zone

                    entries.push_back({{doorX, doorY}, {hallX, hallY}, {x, y}, dd});
                }
            }
        }

        if (entries.size() < 6) {
            // Can't connect cleanly; roll back and try a new seed.
            undoChanges(d, changes);
            d.crawlspaceCarvedTiles = 0;
            continue;
        }

        std::sort(entries.begin(), entries.end(), [&](const EntryCand& a, const EntryCand& b) {
            return a.dist < b.dist;
        });

        const int wantDoors = std::clamp(2 + ((depth >= 6 && rng.chance(0.25f)) ? 1 : 0), 2, 3);
        std::vector<EntryCand> chosen;
        chosen.reserve(static_cast<size_t>(wantDoors));

        auto farFromChosen = [&](const EntryCand& e) {
            for (const EntryCand& c : chosen) {
                if (std::abs(c.door.x - e.door.x) + std::abs(c.door.y - e.door.y) < 18) return false;
            }
            return true;
        };

        // Pick one far and one mid/near to encourage meaningful bypass routes.
        {
            const int n = static_cast<int>(entries.size());
            const int hi0 = std::max(0, n - std::max(6, n / 5));
            const int hiPick = rng.range(hi0, n - 1);
            chosen.push_back(entries[static_cast<size_t>(hiPick)]);

            // Prefer a lower-distance entry for the second door.
            const int lo1 = std::max(0, std::min(n - 1, n / 3));
            for (int tries = 0; tries < 60 && static_cast<int>(chosen.size()) < 2; ++tries) {
                const int cand = rng.range(0, lo1);
                if (!farFromChosen(entries[static_cast<size_t>(cand)])) continue;
                chosen.push_back(entries[static_cast<size_t>(cand)]);
            }
        }

        // Fill any remaining desired entrances.
        for (int tries = 0; tries < 120 && static_cast<int>(chosen.size()) < wantDoors; ++tries) {
            const EntryCand& e = entries[static_cast<size_t>(rng.range(0, static_cast<int>(entries.size()) - 1))];
            if (!farFromChosen(e)) continue;
            chosen.push_back(e);
        }

        if (chosen.size() < 2) {
            undoChanges(d, changes);
            d.crawlspaceCarvedTiles = 0;
            continue;
        }

        // Place doors.
        for (const EntryCand& e : chosen) {
            recordSet(e.door.x, e.door.y, TileType::DoorSecret);
            d.crawlspaceDoorCount += 1;
        }

        // Optional cache chest deep inside the crawlspace.
        if (depth >= 3 && rng.chance(0.55f)) {
            std::vector<int> dist(static_cast<size_t>(W * H), -1);
            std::deque<Vec2i> q;

            for (const EntryCand& e : chosen) {
                const int ii = idx(e.inside.x, e.inside.y);
                if (ii < 0) continue;
                dist[static_cast<size_t>(ii)] = 0;
                q.push_back(e.inside);
            }

            while (!q.empty()) {
                const Vec2i p0 = q.front();
                q.pop_front();
                const int d0 = dist[static_cast<size_t>(idx(p0.x, p0.y))];
                for (int k = 0; k < 4; ++k) {
                    const int nx = p0.x + dirs[k][0];
                    const int ny = p0.y + dirs[k][1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (!isCarved(nx, ny)) continue;
                    const int ii = idx(nx, ny);
                    if (ii < 0) continue;
                    if (dist[static_cast<size_t>(ii)] >= 0) continue;
                    dist[static_cast<size_t>(ii)] = d0 + 1;
                    q.push_back({nx, ny});
                }
            }

            Vec2i best{-1, -1};
            int bestD = -1;
            for (int y = 2; y < H - 2; ++y) {
                for (int x = 2; x < W - 2; ++x) {
                    if (!isCarved(x, y)) continue;
                    const int ii = idx(x, y);
                    const int dd = dist[static_cast<size_t>(ii)];
                    if (dd < 0) continue;
                    if (dd < 8) continue;
                    // Avoid placing a cache directly adjacent to an entrance.
                    bool nearDoor = false;
                    for (const EntryCand& e : chosen) {
                        if (std::abs(e.inside.x - x) + std::abs(e.inside.y - y) <= 2) {
                            nearDoor = true;
                            break;
                        }
                    }
                    if (nearDoor) continue;

                    if (dd > bestD) {
                        bestD = dd;
                        best = {x, y};
                    }
                }
            }

            if (d.inBounds(best.x, best.y)) {
                bool dup = false;
                for (const Vec2i& q0 : d.bonusLootSpots) {
                    if (q0 == best) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) {
                    d.bonusLootSpots.push_back(best);
                    d.crawlspaceCacheCount += 1;
                }
            }
        }

        d.crawlspaceNetworkCount = 1;
        return true;
    }

    // Failed all attempts.
    d.crawlspaceCarvedTiles = 0;
    d.crawlspaceDoorCount = 0;
    d.crawlspaceCacheCount = 0;
    d.crawlspaceNetworkCount = 0;
    return false;
}



// ------------------------------------------------------------
// Sinkholes / micro-chasm fields
//
// A late procgen pass that converts a handful of corridor tiles into small,
// irregular clusters of chasm. This creates local navigation puzzles (often
// solvable by levitation or by pushing boulders into the gap) without ever
// blocking the core path between stairs.
//
// Safety:
// - We compute and protect a shortest passable path between stairsUp and stairsDown.
// - Each sinkhole placement is rolled back if it would break stairs connectivity.
//
// Notes:
// - Uses existing TileType::Chasm + TileType::Boulder mechanics.
// - Tracks d.sinkholeCount as "clusters placed" (not tiles carved).
// ------------------------------------------------------------
static std::vector<Vec2i> shortestPassablePath(const Dungeon& d, Vec2i start, Vec2i goal) {
    std::vector<int> prev(static_cast<size_t>(d.width * d.height), -1);
    auto idx = [&](int x, int y) -> int { return y * d.width + x; };

    if (!d.inBounds(start.x, start.y) || !d.inBounds(goal.x, goal.y)) return {};

    const int s = idx(start.x, start.y);
    const int g = idx(goal.x, goal.y);

    std::deque<Vec2i> q;
    prev[static_cast<size_t>(s)] = s;
    q.push_back(start);

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        Vec2i p = q.front();
        q.pop_front();

        if (p == goal) break;

        for (auto& dv : dirs) {
            int nx = p.x + dv[0];
            int ny = p.y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;

            const int ii = idx(nx, ny);
            if (prev[static_cast<size_t>(ii)] != -1) continue;

            prev[static_cast<size_t>(ii)] = idx(p.x, p.y);
            q.push_back({nx, ny});
        }
    }

    if (prev[static_cast<size_t>(g)] == -1) return {};

    std::vector<Vec2i> path;
    path.reserve(static_cast<size_t>(d.width + d.height));

    int cur = g;
    while (cur != s) {
        const int x = cur % d.width;
        const int y = cur / d.width;
        path.push_back({x, y});
        cur = prev[static_cast<size_t>(cur)];
        if (cur < 0) break;
    }
    path.push_back(start);
    std::reverse(path.begin(), path.end());
    return path;
}

bool maybeCarveSinkholes(Dungeon& d, RNG& rng, int depth, bool eligible) {
    d.sinkholeCount = 0;
    d.vaultSuiteCount = 0;
    if (!eligible) return false;

    // Start introducing sinkholes mid-run, ramping up slightly with depth.
    float pAny = 0.0f;
    if (depth >= 4) {
        pAny = 0.16f + 0.04f * static_cast<float>(std::clamp(depth - 4, 0, 6));
        pAny = std::min(0.55f, pAny);
    }

    // Deep Mines are intentionally unstable: guarantee at least one sinkhole cluster.
    if (depth == Dungeon::DEEP_MINES_DEPTH) pAny = 1.0f;

    if (!rng.chance(pAny)) return false;

    int want = 1;
    if (depth >= 6 && rng.chance(0.55f)) want += 1;
    if (depth >= 8 && rng.chance(0.40f)) want += 1;
    if (depth == Dungeon::DEEP_MINES_DEPTH) want = std::max(want, 2);
    want = std::clamp(want, 1, 4);

    // Build in-room mask so we can preferentially carve sinkholes in corridors/tunnels.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    // Protect a shortest path between stairs so sinkholes never block progression.
    std::vector<uint8_t> protectedTile(static_cast<size_t>(d.width * d.height), uint8_t{0});
    auto markProtect = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        protectedTile[static_cast<size_t>(y * d.width + x)] = 1;
    };

    const auto corePath = shortestPassablePath(d, d.stairsUp, d.stairsDown);
    for (const Vec2i& p : corePath) {
        markProtect(p.x, p.y);
        // Also protect immediate neighbors (helps preserve diagonal cornering options).
        markProtect(p.x + 1, p.y);
        markProtect(p.x - 1, p.y);
        markProtect(p.x, p.y + 1);
        markProtect(p.x, p.y - 1);
    }

    // Extra safety radius near stairs.
    for (int oy = -2; oy <= 2; ++oy) {
        for (int ox = -2; ox <= 2; ++ox) {
            markProtect(d.stairsUp.x + ox, d.stairsUp.y + oy);
            markProtect(d.stairsDown.x + ox, d.stairsDown.y + oy);
        }
    }

    auto isProtected = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return protectedTile[static_cast<size_t>(y * d.width + x)] != 0;
    };

    auto neighborHasDoorLike = [&](int x, int y) -> bool {
        const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        for (auto& dv : dirs) {
            int nx = x + dv[0];
            int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            TileType tt = d.at(nx, ny).type;
            if (tt == TileType::DoorClosed || tt == TileType::DoorOpen || tt == TileType::DoorLocked || tt == TileType::DoorSecret) return true;
        }
        return false;
    };

    const int minFromUp = std::clamp(6 + depth / 2, 6, 12);

    // Candidate centers.
    std::vector<Vec2i> candidates;
    candidates.reserve(static_cast<size_t>(d.width * d.height / 8));

    for (int y = 2; y < d.height - 2; ++y) {
        for (int x = 2; x < d.width - 2; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (isProtected(x, y)) continue;
            if (neighborHasDoorLike(x, y)) continue;

            // Prefer corridors/tunnels (outside room rectangles).
            if (!inRoom.empty() && inRoom[static_cast<size_t>(y * d.width + x)] != 0) continue;

            const int du = d.inBounds(d.stairsUp.x, d.stairsUp.y)
                ? (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y))
                : 9999;
            if (du < minFromUp) continue;

            candidates.push_back({x, y});
        }
    }

    if (candidates.empty()) return false;

    // Bias selection deeper into the level (farther from up stairs).
    const auto distFromUp = bfsDistanceMap(d, d.stairsUp);
    auto scoreCandidate = [&](Vec2i p) -> int {
        if (!d.inBounds(p.x, p.y)) return -1;
        const int di = distFromUp[static_cast<size_t>(p.y * d.width + p.x)];
        if (di >= 0) return di;
        return std::abs(p.x - d.stairsUp.x) + std::abs(p.y - d.stairsUp.y);
    };

    std::sort(candidates.begin(), candidates.end(), [&](const Vec2i& a, const Vec2i& b) {
        return scoreCandidate(a) > scoreCandidate(b);
    });

    auto removeNear = [&](Vec2i c, int rad) {
        candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Vec2i& p) {
            return (std::abs(p.x - c.x) + std::abs(p.y - c.y)) <= rad;
        }), candidates.end());
    };

    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    for (int i = 0; i < want && !candidates.empty(); ++i) {
        const int slice = std::max(1, static_cast<int>(candidates.size()) / 5);
        const int j = rng.range(0, slice - 1);
        Vec2i center = candidates[static_cast<size_t>(j)];
        // Remove the chosen center from candidate list.
        candidates.erase(candidates.begin() + j);

        if (!d.inBounds(center.x, center.y)) continue;
        if (d.at(center.x, center.y).type != TileType::Floor) continue;

        std::vector<TileChange> changes;
        changes.reserve(96);

        // Carve an irregular cluster by random walk.
        int steps = rng.range(4, 7);
        if (depth >= 7) steps += rng.range(0, 4);
        if (depth == Dungeon::DEEP_MINES_DEPTH) steps += rng.range(2, 6);
        steps = std::clamp(steps, 4, 14);

        Vec2i cur = center;
        for (int s = 0; s < steps; ++s) {
            if (d.inBounds(cur.x, cur.y) && d.at(cur.x, cur.y).type == TileType::Floor && !isProtected(cur.x, cur.y)) {
                trySetTile(d, cur.x, cur.y, TileType::Chasm, changes);
            }

            // Occasionally widen.
            if (rng.chance(0.35f)) {
                Vec2i w = dirs[rng.range(0, 3)];
                int wx = cur.x + w.x;
                int wy = cur.y + w.y;
                if (d.inBounds(wx, wy) && d.at(wx, wy).type == TileType::Floor && !isProtected(wx, wy)) {
                    trySetTile(d, wx, wy, TileType::Chasm, changes);
                }
            }

            // Step.
            Vec2i dv = dirs[rng.range(0, 3)];
            Vec2i nxt{cur.x + dv.x, cur.y + dv.y};
            if (!d.inBounds(nxt.x, nxt.y) || nxt.x <= 1 || nxt.y <= 1 || nxt.x >= d.width - 2 || nxt.y >= d.height - 2) {
                // Bounce back toward center.
                nxt = center;
            }
            cur = nxt;
        }

        // Place 0-2 nearby boulders as optional bridge tools / cover.
        int boulders = 0;
        const int maxB = (rng.chance(0.78f) ? 1 : 0) + (rng.chance(0.33f) ? 1 : 0);

        for (int bi = 0; bi < 28 && boulders < maxB; ++bi) {
            if (changes.empty()) break;

            const TileChange& tc = changes[static_cast<size_t>(rng.range(0, static_cast<int>(changes.size()) - 1))];
            if (!d.inBounds(tc.x, tc.y)) continue;
            if (d.at(tc.x, tc.y).type != TileType::Chasm) continue;

            Vec2i dv = dirs[rng.range(0, 3)];
            int bx = tc.x + dv.x;
            int by = tc.y + dv.y;
            if (!d.inBounds(bx, by)) continue;
            if (isProtected(bx, by)) continue;
            if (d.at(bx, by).type != TileType::Floor) continue;

            trySetTile(d, bx, by, TileType::Boulder, changes);
            boulders++;
        }

        // Safety: ensure stairs remain connected.
        if (!stairsConnected(d)) {
            undoChanges(d, changes);
            continue;
        }

        d.sinkholeCount += 1;
        removeNear(center, 6);
    }

    return d.sinkholeCount > 0;
}


// ------------------------------------------------------------
// Bonus room prefabs (Secret/Vault)
//
// Round 21: Increase variety of carved "bonus" rooms by adding interior layouts.
// These are strictly optional side areas; they never affect core stairs connectivity.
// ------------------------------------------------------------

static bool inRoomInterior(const Room& r, int x, int y) {
    return (x >= r.x + 1 && x < r.x2() - 1 && y >= r.y + 1 && y < r.y2() - 1);
}

static int manhattan2(Vec2i a, Vec2i b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

static Vec2i farthestInteriorCorner(const Dungeon& d, const Room& r, Vec2i from) {
    Vec2i corners[4] = {
        {r.x + 1, r.y + 1},
        {r.x2() - 2, r.y + 1},
        {r.x + 1, r.y2() - 2},
        {r.x2() - 2, r.y2() - 2},
    };

    Vec2i best = corners[0];
    int bestD = -1;
    for (const Vec2i& c : corners) {
        if (!d.inBounds(c.x, c.y)) continue;
        if (d.at(c.x, c.y).type != TileType::Floor) continue;
        const int dd = manhattan2(c, from);
        if (dd > bestD) { bestD = dd; best = c; }
    }
    return best;
}


static bool carveVaultSuite(Dungeon& d,
                            const Room& r,
                            RNG& rng,
                            Vec2i doorPos,
                            Vec2i doorInside,
                            Vec2i intoDir,
                            int depth) {
    // A "vault suite" is a locked bonus room that is internally partitioned into
    // multiple chambers. This creates a short tactical mini-dungeon behind the
    // entrance door with LOS breaks and staged fights.
    const bool axisX = (intoDir.x != 0);
    const int axisLen  = axisX ? r.w : r.h;
    const int crossLen = axisX ? r.h : r.w;

    // Need enough length to form at least two chambers, plus some breathing room.
    if (axisLen < 9 || crossLen < 6) return false;

    // Decide number of partitions (1 or 2). Two is only used for larger rooms.
    int partitions = 1;
    if (axisLen >= 12 && rng.chance(0.70f)) partitions = 2;

    // Compute partition offsets from the door wall along intoDir.
    const int minOff = 3;
    const int maxOff = axisLen - 4; // leave at least 3 tiles for the deepest chamber
    if (maxOff < minOff) return false;

    // Push the first wall relatively close to the entrance so the layout reads immediately.
    int off1 = rng.range(minOff, std::min(maxOff, minOff + 2));
    int off2 = off1;

    if (partitions == 2) {
        off2 = off1 + rng.range(3, 5);
        if (off2 > maxOff) partitions = 1;
    }

    auto clampOpenCross = [&](int v) {
        return axisX ? clampi(v, r.y + 1, r.y2() - 2) : clampi(v, r.x + 1, r.x2() - 2);
    };

    std::vector<Vec2i> internalDoors;
    internalDoors.reserve(2);

    auto carvePartition = [&](int off, int openCross) -> int {
        const int coord = (axisX ? doorPos.x : doorPos.y) + (axisX ? intoDir.x : intoDir.y) * off;

        if (axisX) {
            const int x = coord;
            for (int y = r.y; y < r.y2(); ++y) {
                if (!d.inBounds(x, y)) continue;
                const Vec2i p{x, y};
                if (p == doorPos || p == doorInside) continue;
                d.at(x, y).type = TileType::Wall;
            }
            if (d.inBounds(x, openCross)) {
                const Vec2i p{x, openCross};
                if (p != doorPos && p != doorInside) {
                    d.at(x, openCross).type = TileType::DoorClosed;
                    internalDoors.push_back(p);
                }
            }
        } else {
            const int y = coord;
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                const Vec2i p{x, y};
                if (p == doorPos || p == doorInside) continue;
                d.at(x, y).type = TileType::Wall;
            }
            if (d.inBounds(openCross, y)) {
                const Vec2i p{openCross, y};
                if (p != doorPos && p != doorInside) {
                    d.at(openCross, y).type = TileType::DoorClosed;
                    internalDoors.push_back(p);
                }
            }
        }

        return coord;
    };

    // First partition door roughly lines up with the entrance; second drifts toward center for variety.
    const int open1 = clampOpenCross(axisX ? (doorInside.y + rng.range(-1, 1)) : (doorInside.x + rng.range(-1, 1)));
    const int open2 = clampOpenCross(axisX ? (r.cy() + rng.range(-1, 1)) : (r.cx() + rng.range(-1, 1)));

    const int coord1 = carvePartition(off1, open1);
    int coordLast = coord1;
    if (partitions == 2) {
        coordLast = carvePartition(off2, open2);
    }

    // Deepest chamber bounds (the far side of the last partition).
    int fx0 = r.x;
    int fx1 = r.x2() - 1;
    int fy0 = r.y;
    int fy1 = r.y2() - 1;

    if (axisX) {
        if (intoDir.x > 0) fx0 = coordLast + 1;
        else fx1 = coordLast - 1;
    } else {
        if (intoDir.y > 0) fy0 = coordLast + 1;
        else fy1 = coordLast - 1;
    }

    if (fx0 > fx1 || fy0 > fy1) return false;

    std::vector<TileChange> deco;
    deco.reserve(32);

    auto tooCloseToAccess = [&](Vec2i p) {
        if (manhattan2(p, doorInside) <= 2) return true;
        for (const Vec2i& q : internalDoors) {
            if (manhattan2(p, q) <= 1) return true;
        }
        return false;
    };

    // Pillars near corners of the final chamber (cover + LOS breaks).
    Vec2i corners[4] = {
        {fx0 + 1, fy0 + 1},
        {fx1 - 1, fy0 + 1},
        {fx0 + 1, fy1 - 1},
        {fx1 - 1, fy1 - 1},
    };

    for (const Vec2i& c : corners) {
        if (!d.inBounds(c.x, c.y)) continue;
        if (!inRoomInterior(r, c.x, c.y)) continue;
        if (tooCloseToAccess(c)) continue;
        if (rng.chance(0.55f)) trySetTile(d, c.x, c.y, TileType::Pillar, deco);
    }

    // Small chasm pool in a random final-chamber corner (hazard flavor, but not mandatory).
    if (rng.chance(0.55f) && (fx1 - fx0) >= 5 && (fy1 - fy0) >= 5) {
        Vec2i base = corners[static_cast<size_t>(rng.range(0, 3))];
        base.x = clampi(base.x, fx0 + 1, fx1 - 2);
        base.y = clampi(base.y, fy0 + 1, fy1 - 2);

        for (int oy = 0; oy < 2; ++oy) {
            for (int ox = 0; ox < 2; ++ox) {
                Vec2i p{base.x + ox, base.y + oy};
                if (!d.inBounds(p.x, p.y)) continue;
                if (!inRoomInterior(r, p.x, p.y)) continue;
                if (tooCloseToAccess(p)) continue;
                trySetTile(d, p.x, p.y, TileType::Chasm, deco);
            }
        }

        // A boulder nearby can sometimes be used to patch part of the pool into a bridge.
        if (rng.chance(0.45f)) {
            Vec2i bp{base.x + 2, base.y};
            if (d.inBounds(bp.x, bp.y) && inRoomInterior(r, bp.x, bp.y) && !tooCloseToAccess(bp)) {
                trySetTile(d, bp.x, bp.y, TileType::Boulder, deco);
            }
        }
    }

    // Guaranteed loot cache: farthest reachable floor tile inside the final chamber.
    Vec2i loot{-1, -1};
    int bestD = -1;

    const auto dist = bfsDistanceMap(d, doorInside);
    auto distAt = [&](int x, int y) -> int {
        if (!d.inBounds(x, y)) return -1;
        return dist[static_cast<size_t>(y * d.width + x)];
    };

    for (int y = fy0 + 1; y <= fy1 - 1; ++y) {
        for (int x = fx0 + 1; x <= fx1 - 1; ++x) {
            if (!d.inBounds(x, y)) continue;
            if (!r.contains(x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) continue;

            const int dd = distAt(x, y);
            if (dd < 0) continue;

            // Light bias: prefer tiles that are not too close to the entrance.
            int score = dd - manhattan2({x, y}, doorInside) / 2;
            if (score > bestD) { bestD = score; loot = {x, y}; }
        }
    }

    if (loot.x == -1) {
        loot = farthestInteriorCorner(d, r, doorInside);
    }

    if (d.inBounds(loot.x, loot.y) && d.at(loot.x, loot.y).type == TileType::Floor) {
        d.bonusLootSpots.push_back(loot);

        // Sometimes add a second cache deeper in the run.
        if (depth >= 7 && rng.chance(0.20f)) {
            Vec2i loot2 = farthestInteriorCorner(d, r, loot);
            if (d.inBounds(loot2.x, loot2.y) && d.at(loot2.x, loot2.y).type == TileType::Floor) {
                d.bonusLootSpots.push_back(loot2);
            }
        }
    }

    d.vaultSuiteCount += 1;
    return true;
}

static bool carveVaultMoat(Dungeon& d,
                           const Room& r,
                           RNG& rng,
                           Vec2i doorPos,
                           Vec2i doorInside,
                           Vec2i intoDir,
                           int depth) {
    if (r.w < 6 || r.h < 6) return false;

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(r.w * r.h));

    // Island dimensions: keep them modest so we have room for a moat + approach.
    int iw = std::clamp(r.w - 5, 1, 4);
    int ih = std::clamp(r.h - 5, 1, 4);

    // Place the island roughly centered, then bias it away from the door wall.
    int ix = r.x + (r.w - iw) / 2;
    int iy = r.y + (r.h - ih) / 2;

    // Bias away from door by shifting in the direction that points deeper into the room.
    ix += intoDir.x;
    iy += intoDir.y;

    // Clamp so the moat ring stays inside the room (leave a 1-tile outer walkway).
    const int minIx = r.x + 2;
    const int maxIx = r.x + r.w - 2 - iw;
    const int minIy = r.y + 2;
    const int maxIy = r.y + r.h - 2 - ih;
    ix = clampi(ix, minIx, maxIx);
    iy = clampi(iy, minIy, maxIy);

    auto inIsland = [&](int x, int y) -> bool {
        return (x >= ix && x < ix + iw && y >= iy && y < iy + ih);
    };

    // 1) Carve the moat ring (chasm) around the island.
    for (int y = iy - 1; y <= iy + ih; ++y) {
        for (int x = ix - 1; x <= ix + iw; ++x) {
            if (!d.inBounds(x, y)) continue;
            if (inIsland(x, y)) continue; // keep island floor
            // Only the ring boundary becomes chasm.
            if (x == ix - 1 || x == ix + iw || y == iy - 1 || y == iy + ih) {
                // Keep at least a small approach area near the door.
                if (manhattan2({x, y}, doorInside) <= 1) continue;
                trySetTile(d, x, y, TileType::Chasm, changes);
            }
        }
    }

    // 2) Prefer a boulder-bridge breach on the moat tile closest to the door approach.
    struct Cand {
        Vec2i breach;
        Vec2i outer;   // boulder tile (outer walkway)
        Vec2i inner;   // island-adjacent floor
        Vec2i pushFrom;
        Vec2i dir;     // push direction (outer -> breach -> inner)
        int score = 0;
    };

    std::vector<Cand> cands;
    cands.reserve(24);

    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (int y = iy - 1; y <= iy + ih; ++y) {
        for (int x = ix - 1; x <= ix + iw; ++x) {
            if (!d.inBounds(x, y)) continue;
            if (d.at(x, y).type != TileType::Chasm) continue;

            for (const Vec2i& dv : dirs) {
                Vec2i outer{x - dv.x, y - dv.y};
                Vec2i inner{x + dv.x, y + dv.y};
                Vec2i pushFrom{outer.x - dv.x, outer.y - dv.y};

                if (!inRoomInterior(r, outer.x, outer.y)) continue;
                if (!inRoomInterior(r, inner.x, inner.y)) continue;
                if (!inRoomInterior(r, pushFrom.x, pushFrom.y)) continue;

                if (outer == doorPos) continue; // never place boulder on the door tile
                if (!inIsland(inner.x, inner.y)) continue;

                if (!d.inBounds(outer.x, outer.y) || !d.inBounds(inner.x, inner.y) || !d.inBounds(pushFrom.x, pushFrom.y)) continue;

                if (d.at(outer.x, outer.y).type != TileType::Floor) continue; // boulder requires plain floor
                if (d.at(inner.x, inner.y).type != TileType::Floor) continue;

                // pushFrom can be floor or a door tile (vault door may be used as the push point).
                const TileType pf = d.at(pushFrom.x, pushFrom.y).type;
                if (!(pf == TileType::Floor || pf == TileType::DoorOpen || pf == TileType::DoorClosed || pf == TileType::DoorLocked)) continue;

                // Favor the breach on the door-facing side of the island.
                int score = 0;
                score -= manhattan2(outer, doorInside) * 3;
                score -= manhattan2(inner, {ix + iw / 2, iy + ih / 2});
                score += rng.range(-2, 2);

                cands.push_back({{x, y}, outer, inner, pushFrom, dv, score});
            }
        }
    }

    if (!cands.empty()) {
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.score > b.score;
        });

        // Pick among the best few to avoid sameness.
        const int topN = std::min(4, static_cast<int>(cands.size()));
        const Cand pick = cands[static_cast<size_t>(rng.range(0, topN - 1))];

        // Place the boulder on the outer walkway.
        trySetTile(d, pick.outer.x, pick.outer.y, TileType::Boulder, changes);

        if (d.at(pick.outer.x, pick.outer.y).type == TileType::Boulder) {
            // Request a "bonus" loot cache on the island (guaranteed chest spawn).
            Vec2i loot{ix + iw / 2, iy + ih / 2};
            if (d.inBounds(loot.x, loot.y) && d.at(loot.x, loot.y).type == TileType::Floor) {
                d.bonusLootSpots.push_back(loot);
            }

            // Deeper vaults can hide a second cache.
            if (depth >= 7 && rng.chance(0.22f)) {
                for (int tries = 0; tries < 30; ++tries) {
                    Vec2i p{rng.range(ix, ix + iw - 1), rng.range(iy, iy + ih - 1)};
                    if (!d.inBounds(p.x, p.y)) continue;
                    if (d.at(p.x, p.y).type != TileType::Floor) continue;
                    d.bonusLootSpots.push_back(p);
                    break;
                }
            }

            return true;
        }
    }

    // Fallback: carve a permanent bridge tile so the island is still reachable.
    // Choose the moat tile closest to the door and open it.
    Vec2i best{-1, -1};
    int bestD = 1000000000; // large sentinel; avoids double->int warning
    for (int y = iy - 1; y <= iy + ih; ++y) {
        for (int x = ix - 1; x <= ix + iw; ++x) {
            if (!d.inBounds(x, y)) continue;
            if (d.at(x, y).type != TileType::Chasm) continue;
            int dd = manhattan2({x, y}, doorInside);
            if (dd < bestD) { bestD = dd; best = {x, y}; }
        }
    }

    if (best.x != -1 && d.inBounds(best.x, best.y)) {
        // Directly overwrite: bridge is a structural choice, not a reversible decoration.
        d.at(best.x, best.y).type = TileType::Floor;
    }

    return true;
}

static bool carveVaultTrench(Dungeon& d,
                             const Room& r,
                             RNG& rng,
                             Vec2i doorPos,
                             Vec2i doorInside,
                             Vec2i intoDir,
                             int depth) {
    // Require enough interior to meaningfully split.
    if (r.w < 6 && r.h < 6) return false;

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(r.w * r.h));

    const bool vertical = (intoDir.x != 0);

    Vec2i breach{-1, -1};

    if (vertical) {
        int lineX = r.x + r.w / 2;
        lineX = clampi(lineX, r.x + 2, r.x2() - 3);

        // Avoid carving the trench on top of the entrance corridor.
        if (lineX == doorInside.x) lineX += (intoDir.x > 0 ? 1 : -1);
        lineX = clampi(lineX, r.x + 2, r.x2() - 3);

        for (int y = r.y + 1; y < r.y2() - 1; ++y) {
            if (!d.inBounds(lineX, y)) continue;
            if (Vec2i{lineX, y} == doorPos) continue;
            if (Vec2i{lineX, y} == doorInside) continue;
            trySetTile(d, lineX, y, TileType::Chasm, changes);
        }

        const int by = clampi(r.cy(), r.y + 2, r.y2() - 3);
        breach = {lineX, by};
    } else {
        int lineY = r.y + r.h / 2;
        lineY = clampi(lineY, r.y + 2, r.y2() - 3);

        if (lineY == doorInside.y) lineY += (intoDir.y > 0 ? 1 : -1);
        lineY = clampi(lineY, r.y + 2, r.y2() - 3);

        for (int x = r.x + 1; x < r.x2() - 1; ++x) {
            if (!d.inBounds(x, lineY)) continue;
            if (Vec2i{x, lineY} == doorPos) continue;
            if (Vec2i{x, lineY} == doorInside) continue;
            trySetTile(d, x, lineY, TileType::Chasm, changes);
        }

        const int bx = clampi(r.cx(), r.x + 2, r.x2() - 3);
        breach = {bx, lineY};
    }

    if (!d.inBounds(breach.x, breach.y)) return false;

    // Ensure breach is actually a chasm (we may have skipped it due to overlaps).
    d.at(breach.x, breach.y).type = TileType::Chasm;

    // Attempt a boulder-bridge puzzle on the breach aligned with the entrance direction.
    const Vec2i outer{breach.x - intoDir.x, breach.y - intoDir.y};
    const Vec2i pushFrom{breach.x - 2 * intoDir.x, breach.y - 2 * intoDir.y};
    const Vec2i inner{breach.x + intoDir.x, breach.y + intoDir.y};

    auto isOkPushFrom = [&](Vec2i p) -> bool {
        if (!inRoomInterior(r, p.x, p.y) && p != doorPos) return false;
        if (!d.inBounds(p.x, p.y)) return false;
        const TileType t = d.at(p.x, p.y).type;
        return (t == TileType::Floor || t == TileType::DoorOpen || t == TileType::DoorClosed || t == TileType::DoorLocked);
    };

    bool placed = false;
    if (d.inBounds(outer.x, outer.y) && d.inBounds(inner.x, inner.y)) {
        if (outer != doorPos && d.at(outer.x, outer.y).type == TileType::Floor && d.at(inner.x, inner.y).type == TileType::Floor && isOkPushFrom(pushFrom)) {
            trySetTile(d, outer.x, outer.y, TileType::Boulder, changes);
            placed = (d.at(outer.x, outer.y).type == TileType::Boulder);
        }
    }

    if (placed) {
        // Request a loot cache on the far side of the trench.
        Vec2i loot{inner.x + intoDir.x, inner.y + intoDir.y};
        if (!d.inBounds(loot.x, loot.y) || d.at(loot.x, loot.y).type != TileType::Floor) {
            // Fallback: farthest reachable interior corner on the far side.
            loot = farthestInteriorCorner(d, r, doorInside);
        }
        if (d.inBounds(loot.x, loot.y) && d.at(loot.x, loot.y).type == TileType::Floor) {
            d.bonusLootSpots.push_back(loot);
        }

        // Deeper trenches can have a second cache.
        if (depth >= 6 && rng.chance(0.18f)) {
            Vec2i loot2 = farthestInteriorCorner(d, r, {loot.x, loot.y});
            if (d.inBounds(loot2.x, loot2.y) && d.at(loot2.x, loot2.y).type == TileType::Floor) {
                d.bonusLootSpots.push_back(loot2);
            }
        }

        return true;
    }

    // Fallback: carve a permanent bridge so the room isn't a dead puzzle.
    d.at(breach.x, breach.y).type = TileType::Floor;
    return true;
}

static void carveVaultPillarGrid(Dungeon& d, const Room& r, RNG& rng, Vec2i doorInside) {
    std::vector<TileChange> changes;
    changes.reserve(64);

    // A simple symmetric lattice: pillars every other tile, skipping near the entrance so it stays fair.
    for (int y = r.y + 2; y <= r.y2() - 3; y += 2) {
        for (int x = r.x + 2; x <= r.x2() - 3; x += 2) {
            if (!d.inBounds(x, y)) continue;
            if (manhattan2({x, y}, doorInside) <= 2) continue;
            if (rng.chance(0.85f)) trySetTile(d, x, y, TileType::Pillar, changes);
        }
    }

    // Sometimes add a "dais" pillar near the center for extra cover.
    if (r.w >= 7 && r.h >= 7 && rng.chance(0.35f)) {
        const int cx = r.cx();
        const int cy = r.cy();
        if (d.inBounds(cx, cy) && manhattan2({cx, cy}, doorInside) > 2) {
            trySetTile(d, cx, cy, TileType::Pillar, changes);
        }
    }
}

void decorateSecretBonusRoom(Dungeon& d,
                            const Room& r,
                            RNG& rng,
                            Vec2i doorPos,
                            Vec2i doorInside,
                            Vec2i intoDir,
                            int depth) {
    (void)doorPos;
    (void)intoDir;

    std::vector<TileChange> changes;
    changes.reserve(16);

    auto farFromDoor = [&](int x, int y) -> bool {
        return manhattan2({x, y}, doorInside) >= 2;
    };

    // Pillars make the room feel hand-crafted and provide LOS blockers.
    if (r.w >= 6 && r.h >= 6) {
        Vec2i pts[4] = {
            {r.x + 2, r.y + 2},
            {r.x2() - 3, r.y + 2},
            {r.x + 2, r.y2() - 3},
            {r.x2() - 3, r.y2() - 3},
        };
        for (const Vec2i& p : pts) {
            if (!d.inBounds(p.x, p.y)) continue;
            if (!farFromDoor(p.x, p.y)) continue;
            if (rng.chance(0.70f)) trySetTile(d, p.x, p.y, TileType::Pillar, changes);
        }
    } else if (r.w >= 5 && r.h >= 5) {
        const int cx = r.cx();
        const int cy = r.cy();
        if (d.inBounds(cx, cy) && farFromDoor(cx, cy)) {
            trySetTile(d, cx, cy, TileType::Pillar, changes);
        }
    } else {
        // Tiny secrets: small chance for a single corner pillar.
        if (rng.chance(0.45f)) {
            Vec2i p{r.x + 1, r.y + 1};
            if (d.inBounds(p.x, p.y) && farFromDoor(p.x, p.y)) {
                trySetTile(d, p.x, p.y, TileType::Pillar, changes);
            }
        }
    }

    // Hidden stash: sometimes request a bonus chest spawn.
    float stashChance = 0.15f;
    if (depth >= 3) stashChance = 0.24f;
    if (depth >= 6) stashChance = 0.34f;
    if (depth >= 8) stashChance = 0.42f;

    if (rng.chance(stashChance)) {
        Vec2i loot = farthestInteriorCorner(d, r, doorInside);
        if (d.inBounds(loot.x, loot.y) && d.at(loot.x, loot.y).type == TileType::Floor) {
            d.bonusLootSpots.push_back(loot);
        }
    }
}

void decorateVaultBonusRoom(Dungeon& d,
                           const Room& r,
                           RNG& rng,
                           Vec2i doorPos,
                           Vec2i doorInside,
                           Vec2i intoDir,
                           int depth) {
    // Pick a layout. Bigger rooms favor moats/trenches/suites; small rooms default to pillars.
    const bool canMoat = (r.w >= 6 && r.h >= 6);
    const bool canTrench = (r.w >= 6 || r.h >= 6);

    // Suites are best when we have enough length in the direction we enter the vault.
    const bool axisX = (intoDir.x != 0);
    const int axisLen  = axisX ? r.w : r.h;
    const int crossLen = axisX ? r.h : r.w;
    const bool canSuite = (axisLen >= 9 && crossLen >= 6);

    float pSuite = canSuite ? 0.22f : 0.0f;
    float pMoat = canMoat ? 0.46f : 0.0f;
    float pTrench = canTrench ? 0.30f : 0.0f;

    // Deeper floors bias toward more puzzle-y vaults.
    if (depth >= 6) { pSuite += 0.06f; pMoat += 0.08f; pTrench += 0.05f; }
    if (depth >= 8) { pSuite += 0.05f; pMoat += 0.06f; pTrench += 0.06f; }

    // Keep a healthy chance to fall back to the always-safe pillar lattice.
    float sum = pSuite + pMoat + pTrench;
    if (sum > 0.92f && sum > 0.0f) {
        float scale = 0.92f / sum;
        pSuite *= scale;
        pMoat *= scale;
        pTrench *= scale;
    }

    const float roll = rng.next01();

    if (roll < pSuite) {
        if (carveVaultSuite(d, r, rng, doorPos, doorInside, intoDir, depth)) return;
        // If the suite fails (rare), fall through into the other prefabs.
    }

    if (roll < pSuite + pMoat) {
        if (carveVaultMoat(d, r, rng, doorPos, doorInside, intoDir, depth)) return;
    } else if (roll < pSuite + pMoat + pTrench) {
        if (carveVaultTrench(d, r, rng, doorPos, doorInside, intoDir, depth)) return;
    }

    // Default: pillar lattice (always safe).
    carveVaultPillarGrid(d, r, rng, doorInside);
}


bool decorateRoomPillars(Dungeon& d, const Room& r, RNG& rng) {
    // Only decorate sufficiently large rooms.
    if (r.w < 7 || r.h < 7) return false;

    std::vector<TileChange> changes;
    changes.reserve(32);

    // Pick a pattern: 0 = corner pillars, 1 = grid pillars, 2 = cross pillars
    int pattern = 0;
    if (r.w >= 10 && r.h >= 10) pattern = rng.range(0, 2);
    else pattern = rng.range(0, 1);

    auto inInterior = [&](int x, int y) {
        return x >= r.x + 1 && x < r.x2() - 1 && y >= r.y + 1 && y < r.y2() - 1;
    };

    if (pattern == 0) {
        // Four pillars near the corners.
        const int xs[2] = {r.x + 2, r.x2() - 3};
        const int ys[2] = {r.y + 2, r.y2() - 3};
        for (int yy : ys) {
            for (int xx : xs) {
                if (!inInterior(xx, yy)) continue;
                trySetTile(d, xx, yy, TileType::Pillar, changes);
            }
        }
    } else if (pattern == 1) {
        // A loose grid of pillars.
        const int stepX = (r.w >= 12) ? 3 : 4;
        const int stepY = (r.h >= 12) ? 3 : 4;
        for (int y = r.y + 2; y < r.y2() - 2; y += stepY) {
            for (int x = r.x + 2; x < r.x2() - 2; x += stepX) {
                if (!inInterior(x, y)) continue;
                if (rng.chance(0.75f)) trySetTile(d, x, y, TileType::Pillar, changes);
            }
        }
    } else {
        // Cross pillars: a vertical/horizontal line near the center.
        const int cx = r.cx();
        const int cy = r.cy();
        for (int y = r.y + 2; y < r.y2() - 2; ++y) {
            if (rng.chance(0.45f)) trySetTile(d, cx, y, TileType::Pillar, changes);
        }
        for (int x = r.x + 2; x < r.x2() - 2; ++x) {
            if (rng.chance(0.45f)) trySetTile(d, x, cy, TileType::Pillar, changes);
        }

        // Clear the exact center to avoid total blockage.
        if (d.inBounds(cx, cy) && d.at(cx, cy).type == TileType::Pillar) {
            changes.push_back({cx, cy, TileType::Pillar});
            d.at(cx, cy).type = TileType::Floor;
        }
    }

    // Avoid breaking the critical path between stairs.
    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }
    return !changes.empty();
}

bool decorateRoomChasm(Dungeon& d, const Room& r, RNG& rng, int depth) {
    // Only decorate sufficiently large rooms.
    if (r.w < 8 || r.h < 6) return false;

    std::vector<TileChange> changes;
    changes.reserve(48);

    const bool vertical = rng.chance(0.5f);

    int lineX = -1;
    int lineY = -1;
    int bridgeX = -1;
    int bridgeY = -1;

    if (vertical) {
        lineX = r.cx();
        // A vertical chasm line with a single bridge tile.
        bridgeY = rng.range(r.y + 2, r.y2() - 3);
        for (int y = r.y + 1; y < r.y2() - 1; ++y) {
            if (y == bridgeY) continue;
            trySetTile(d, lineX, y, TileType::Chasm, changes);
        }
    } else {
        lineY = r.cy();
        bridgeX = rng.range(r.x + 2, r.x2() - 3);
        for (int x = r.x + 1; x < r.x2() - 1; ++x) {
            if (x == bridgeX) continue;
            trySetTile(d, x, lineY, TileType::Chasm, changes);
        }
    }

    if (changes.empty()) return false;

    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    // Optional boulder-bridge puzzle variant:
    // - remove the fixed bridge tile (turn it into chasm)
    // - place a pushable boulder adjacent to the gap
    // - request a guaranteed loot cache on the far side
    float puzzleChance = 0.10f;
    if (depth >= 3) puzzleChance = 0.18f;
    if (depth >= 6) puzzleChance = 0.28f;

    if (rng.chance(puzzleChance)) {
        const Vec2i bridgePos = vertical ? Vec2i{ lineX, bridgeY } : Vec2i{ bridgeX, lineY };

        auto isPlainFloor = [&](const Vec2i& p) -> bool {
            if (!d.inBounds(p.x, p.y)) return false;
            if (isStairsTile(d, p.x, p.y)) return false;
            return d.at(p.x, p.y).type == TileType::Floor;
        };

        if (isPlainFloor(bridgePos)) {
            struct Candidate {
                Vec2i boulder;
                Vec2i pushFrom;
                int dx = 0;
                int dy = 0;
                int side = 0; // -1 = negative half, +1 = positive half
                bool ok = false;
            };

            Candidate cand[2];

            if (vertical) {
                // Chasm line at x=lineX, bridge at (lineX, bridgeY)
                // Candidate 0: boulder on west side, push east.
                cand[0].boulder  = { lineX - 1, bridgeY };
                cand[0].pushFrom = { lineX - 2, bridgeY };
                cand[0].dx = +1; cand[0].dy = 0; cand[0].side = +1;
                // Candidate 1: boulder on east side, push west.
                cand[1].boulder  = { lineX + 1, bridgeY };
                cand[1].pushFrom = { lineX + 2, bridgeY };
                cand[1].dx = -1; cand[1].dy = 0; cand[1].side = -1;

                for (int i = 0; i < 2; ++i) {
                    const Vec2i farAdj = { bridgePos.x + cand[i].dx, bridgePos.y + cand[i].dy };
                    cand[i].ok = isPlainFloor(cand[i].boulder) && isPlainFloor(cand[i].pushFrom) && isPlainFloor(farAdj);
                }
            } else {
                // Chasm line at y=lineY, bridge at (bridgeX, lineY)
                // Candidate 0: boulder on north side, push south.
                cand[0].boulder  = { bridgeX, lineY - 1 };
                cand[0].pushFrom = { bridgeX, lineY - 2 };
                cand[0].dx = 0; cand[0].dy = +1; cand[0].side = +1;
                // Candidate 1: boulder on south side, push north.
                cand[1].boulder  = { bridgeX, lineY + 1 };
                cand[1].pushFrom = { bridgeX, lineY + 2 };
                cand[1].dx = 0; cand[1].dy = -1; cand[1].side = -1;

                for (int i = 0; i < 2; ++i) {
                    const Vec2i farAdj = { bridgePos.x + cand[i].dx, bridgePos.y + cand[i].dy };
                    cand[i].ok = isPlainFloor(cand[i].boulder) && isPlainFloor(cand[i].pushFrom) && isPlainFloor(farAdj);
                }
            }

            // Try candidates in random order.
            const int start = rng.chance(0.5f) ? 0 : 1;
            for (int k = 0; k < 2; ++k) {
                Candidate c = cand[(start + k) & 1];
                if (!c.ok) continue;

                std::vector<TileChange> extra;
                extra.reserve(8);

                // Close the bridge.
                extra.push_back({ bridgePos.x, bridgePos.y, d.at(bridgePos.x, bridgePos.y).type });
                d.at(bridgePos.x, bridgePos.y).type = TileType::Chasm;

                // Place the boulder next to the gap.
                trySetTile(d, c.boulder.x, c.boulder.y, TileType::Boulder, extra);
                if (d.at(c.boulder.x, c.boulder.y).type != TileType::Boulder) {
                    undoChanges(d, extra);
                    continue;
                }

                // Ensure we didn't break stairs connectivity, and that the "push from" tile is reachable.
                const auto dist = bfsDistanceMap(d, d.stairsUp);
                auto distAt = [&](const Vec2i& p) -> int {
                    if (!d.inBounds(p.x, p.y)) return -1;
                    const size_t ii = static_cast<size_t>(p.y * d.width + p.x);
                    if (ii >= dist.size()) return -1;
                    return dist[ii];
                };

                bool okStairs = true;
                if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
                    okStairs = (distAt(d.stairsDown) >= 0);
                }
                const bool okPush = (distAt(c.pushFrom) >= 0);

                if (!okStairs || !okPush) {
                    undoChanges(d, extra);
                    continue;
                }

                // Find a loot tile on the far side of the chasm within this room.
                Vec2i loot{-1, -1};
                for (int tries = 0; tries < 80; ++tries) {
                    int lx = 0, ly = 0;
                    if (vertical) {
                        if (c.side > 0) {
                            lx = rng.range(lineX + 1, r.x2() - 2);
                        } else {
                            lx = rng.range(r.x + 1, lineX - 1);
                        }
                        ly = rng.range(r.y + 1, r.y2() - 2);
                    } else {
                        lx = rng.range(r.x + 1, r.x2() - 2);
                        if (c.side > 0) {
                            ly = rng.range(lineY + 1, r.y2() - 2);
                        } else {
                            ly = rng.range(r.y + 1, lineY - 1);
                        }
                    }
                    Vec2i p{lx, ly};
                    if (!isPlainFloor(p)) continue;
                    loot = p;
                    break;
                }

                if (loot.x != -1) {
                    d.bonusLootSpots.push_back(loot);
                }

                // Keep the variant.
                break;
            }
        }
    }

    return true;
}


bool decorateRoomBoulders(Dungeon& d, const Room& r, RNG& rng, int depth) {
    // Scatter a few pushable boulders inside rooms to create cover and choke points.
    if (r.w < 7 || r.h < 7) return false;

    const int interiorW = std::max(0, r.w - 2);
    const int interiorH = std::max(0, r.h - 2);
    const int area = interiorW * interiorH;

    int maxCount = 1;
    if (area >= 60) maxCount = 2;
    if (area >= 90) maxCount = 3;
    if (depth >= 6) maxCount += 1;
    maxCount = std::clamp(maxCount, 1, 5);

    const int count = rng.range(1, maxCount);

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(count + 4));

    auto okSpot = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (isStairsTile(d, x, y)) return false;
        if (d.at(x, y).type != TileType::Floor) return false;

        // Avoid directly blocking doors/thresholds (rough heuristic).
        const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        for (auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            const TileType tt = d.at(nx, ny).type;
            if (tt == TileType::DoorClosed || tt == TileType::DoorLocked || tt == TileType::DoorOpen) return false;
        }
        return true;
    };

    int placed = 0;
    for (int i = 0; i < count; ++i) {
        for (int tries = 0; tries < 60; ++tries) {
            // Bias toward corners/edges so boulders feel like clutter, not "random blockers".
            int x = rng.range(r.x + 1, r.x2() - 2);
            int y = rng.range(r.y + 1, r.y2() - 2);
            if (rng.chance(0.55f)) {
                x = (rng.chance(0.5f)) ? rng.range(r.x + 1, r.x + 3) : rng.range(r.x2() - 4, r.x2() - 2);
            }
            if (rng.chance(0.55f)) {
                y = (rng.chance(0.5f)) ? rng.range(r.y + 1, r.y + 3) : rng.range(r.y2() - 4, r.y2() - 2);
            }

            if (!okSpot(x, y)) continue;

            trySetTile(d, x, y, TileType::Boulder, changes);
            if (d.at(x, y).type == TileType::Boulder) {
                placed++;
                break;
            }
        }
    }

    if (placed <= 0) {
        undoChanges(d, changes);
        return false;
    }

    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    return true;
}

bool scatterBoulders(Dungeon& d, RNG& rng, int depth) {
    // For non-room layouts (caverns/mazes), sprinkle a small number of boulders to
    // create micro-terrain without needing room metadata.
    const int area = d.width * d.height;
    int target = std::clamp(area / 180, 2, 10);
    target += std::min(6, depth / 2);

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(target + 8));

    auto tooCloseToStairs = [&](int x, int y) {
        const int du = std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y);
        const int dd = std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y);
        return du <= 2 || dd <= 2;
    };

    int placed = 0;
    for (int i = 0; i < target; ++i) {
        for (int tries = 0; tries < 120; ++tries) {
            int x = rng.range(1, d.width - 2);
            int y = rng.range(1, d.height - 2);
            if (!d.inBounds(x, y)) continue;
            if (tooCloseToStairs(x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) continue;

            // Avoid dense clustering.
            bool near = false;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    int nx = x + ox, ny = y + oy;
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type == TileType::Boulder) { near = true; break; }
                }
                if (near) break;
            }
            if (near) continue;

            trySetTile(d, x, y, TileType::Boulder, changes);
            if (d.at(x, y).type == TileType::Boulder) {
                placed++;
                break;
            }
        }
    }

    if (placed <= 0) {
        undoChanges(d, changes);
        return false;
    }

    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    return true;
}


void decorateRooms(Dungeon& d, RNG& rng, int depth) {
    // Decoration pacing: more structural variation deeper.
    float pPillars = 0.18f;
    float pChasm   = 0.10f;
    float pBoulders = 0.10f;
    if (depth >= 3) { pPillars += 0.07f; pChasm += 0.06f; pBoulders += 0.08f; }
    if (depth >= 5) { pPillars += 0.08f; pChasm += 0.08f; pBoulders += 0.10f; }

    for (const Room& r : d.rooms) {
        // Don't decorate special rooms: they have bespoke gameplay (shops, shrines, etc.).
        if (r.type != RoomType::Normal) continue;

        // Avoid the start/end rooms that hold stairs.
        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;

        // Skip tiny rooms.
        if (r.w < 6 || r.h < 6) continue;

        // One or two decorations per room (rare).
        if (rng.chance(pChasm)) {
            (void)decorateRoomChasm(d, r, rng, depth);
        }
        if (rng.chance(pPillars)) {
            (void)decorateRoomPillars(d, r, rng);
        }
        if (rng.chance(pBoulders)) {
            (void)decorateRoomBoulders(d, r, rng, depth);
        }
    }
}



// ------------------------------------------------------------
// Themed room interior prefabs
//
// Armory / Library / Laboratory rooms are already special-cased by spawn logic.
// This pass adds lightweight, connectivity-safe "furniture" layouts so these
// rooms feel distinct at a glance: racks/shelves/vats, occasional spill hazards,
// and the rare bonus cache tucked deep in the stacks.
//
// Design constraints:
//  - Never block the global stairs path (verify + undo on failure).
//  - Keep door-adjacent tiles clear so rooms remain enterable.
//  - Prefer pure tile decoration (pillars/boulders/chasm) so gameplay systems
//    don't need extra rules.
// ------------------------------------------------------------

static bool isThemedRoom(RoomType t) {
    return t == RoomType::Armory || t == RoomType::Library || t == RoomType::Laboratory;
}

static void buildRoomDoorInfo(const Dungeon& d, const Room& r,
                              std::vector<Vec2i>& doors,
                              std::vector<Vec2i>& doorInside) {
    doors.clear();
    doorInside.clear();

    auto consider = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        if (!isDoorTileType(d.at(x, y).type)) return;

        const Vec2i dp{x, y};
        doors.push_back(dp);

        int ix = x;
        int iy = y;

        // Compute the tile just inside the room (one step inward from the door).
        if (x == r.x) ix = x + 1;
        else if (x == r.x2() - 1) ix = x - 1;

        if (y == r.y) iy = y + 1;
        else if (y == r.y2() - 1) iy = y - 1;

        if (d.inBounds(ix, iy)) doorInside.push_back({ix, iy});

        // Also reserve one more step inward to reduce "immediately blocked" entrances.
        const int iix = ix + (ix - x);
        const int iiy = iy + (iy - y);
        if (d.inBounds(iix, iiy)) doorInside.push_back({iix, iiy});
    };

    for (int x = r.x; x < r.x2(); ++x) {
        consider(x, r.y);
        consider(x, r.y2() - 1);
    }
    for (int y = r.y; y < r.y2(); ++y) {
        consider(r.x, y);
        consider(r.x2() - 1, y);
    }
}

static void buildRoomKeepMask(const Dungeon& d, const std::vector<Vec2i>& pts, std::vector<uint8_t>& keep) {
    keep.assign(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const Vec2i& p : pts) {
        if (!d.inBounds(p.x, p.y)) continue;
        keep[static_cast<size_t>(p.y * d.width + p.x)] = 1;
    }
}



// ------------------------------------------------------------
// Room shape variety (normal rooms)
//
// Round 24: Add a lightweight shaping pass that introduces internal wall
// partitions / alcoves inside some *normal* rooms. This creates non-rectangular
// combat spaces (L-bites, donut-ring blocks, partition walls) without requiring
// new tile types.
//
// Constraints:
//  - Never touch stairs tiles.
//  - Keep door-adjacent interior clear (via a keep-mask).
//  - Ensure the room's passable tiles remain a single connected component,
//    so we don't accidentally sever corridor connectivity.
//  - Validate global stairs connectivity and roll back on failure.
// ------------------------------------------------------------

static bool roomInteriorConnectedSingleComponent(const Dungeon& d, const Room& r, const std::vector<Vec2i>& doorInside) {
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    std::vector<uint8_t> visited(static_cast<size_t>(d.width * d.height), uint8_t{0});
    std::queue<Vec2i> q;

    auto seed = [&](Vec2i p) {
        if (!d.inBounds(p.x, p.y)) return;
        if (!r.contains(p.x, p.y)) return;
        if (!d.isPassable(p.x, p.y)) return;
        const size_t ii = idx(p.x, p.y);
        if (ii >= visited.size() || visited[ii]) return;
        visited[ii] = 1;
        q.push(p);
    };

    // Prefer seeding from door-adjacent interior tiles.
    for (const Vec2i& p : doorInside) seed(p);

    // Fallback: use the room center.
    if (q.empty()) seed({r.cx(), r.cy()});
    if (q.empty()) return false;

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    while (!q.empty()) {
        const Vec2i p = q.front();
        q.pop();
        for (auto& dv : dirs) {
            const int nx = p.x + dv[0];
            const int ny = p.y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!r.contains(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;
            const size_t ii = idx(nx, ny);
            if (visited[ii]) continue;
            visited[ii] = 1;
            q.push({nx, ny});
        }
    }

    int totalPassable = 0;
    int reachedPassable = 0;
    for (int y = r.y; y < r.y2(); ++y) {
        for (int x = r.x; x < r.x2(); ++x) {
            if (!d.inBounds(x, y)) continue;
            if (!d.isPassable(x, y)) continue;
            totalPassable++;
            if (visited[idx(x, y)]) reachedPassable++;
        }
    }

    return reachedPassable == totalPassable;
}

static bool tryShapeNormalRoom(Dungeon& d, const Room& r, RNG& rng, int depth) {
    // Small rooms don't benefit from this and can become too cramped.
    if (r.w < 8 || r.h < 8) return false;

    // Skip rooms that already contain non-floor terrain (e.g., ravines/lakes).
    for (int y = r.y + 1; y < r.y2() - 1; ++y) {
        for (int x = r.x + 1; x < r.x2() - 1; ++x) {
            if (!d.inBounds(x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) return false;
        }
    }

    std::vector<Vec2i> doors;
    std::vector<Vec2i> doorInside;
    buildRoomDoorInfo(d, r, doors, doorInside);
    if (doors.empty()) return false;

    // Keep door tiles + their immediate interior clear.
    std::vector<Vec2i> keepPts = doors;
    keepPts.insert(keepPts.end(), doorInside.begin(), doorInside.end());

    std::vector<uint8_t> keep;
    buildRoomKeepMask(d, keepPts, keep);

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(r.w * r.h / 3));

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    auto trySetWall = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        if (!r.contains(x, y)) return;
        if (!keep.empty() && keep[idx(x, y)] != 0) return;
        if (isStairsTile(d, x, y)) return;
        Tile& t = d.at(x, y);
        if (t.type != TileType::Floor) return;
        changes.push_back({x, y, t.type});
        t.type = TileType::Wall;
    };

    auto trySetInteriorDoor = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        if (!r.contains(x, y)) return;
        if (!keep.empty() && keep[idx(x, y)] != 0) return;
        if (isStairsTile(d, x, y)) return;
        Tile& t = d.at(x, y);
        if (t.type != TileType::Floor) return;
        changes.push_back({x, y, t.type});
        t.type = rng.chance(0.75f) ? TileType::DoorClosed : TileType::DoorOpen;
    };

    enum class Variant { CornerBite, CentralBlock, Partition };

    // Weight variant selection slightly by depth (deeper -> more partitions).
    const int roll = rng.range(0, 99);
    Variant v = Variant::CornerBite;
    if (r.w >= 10 && r.h >= 10 && roll < 22) {
        v = Variant::CentralBlock;
    } else {
        const int bias = std::min(35, depth * 4); // up to +35% partition bias
        if (roll < 40 + bias) v = Variant::Partition;
        else v = Variant::CornerBite;
    }

    // Apply the chosen variant.
    if (v == Variant::CornerBite) {
        // Carve out an interior corner to form an L-shaped room.
        const int depthBonus = std::clamp(depth / 4, 0, 3);
        const int maxBW = std::max(2, std::min((r.w - 4), (r.w / 2) + depthBonus));
        const int maxBH = std::max(2, std::min((r.h - 4), (r.h / 2) + depthBonus));
        if (maxBW < 2 || maxBH < 2) return false;

        const int bw = rng.range(2, maxBW);
        const int bh = rng.range(2, maxBH);
        const int corner = rng.range(0, 3);

        int bx = r.x + 1;
        int by = r.y + 1;
        if (corner == 1) { // top-right
            bx = (r.x2() - 1) - bw;
            by = r.y + 1;
        } else if (corner == 2) { // bottom-left
            bx = r.x + 1;
            by = (r.y2() - 1) - bh;
        } else if (corner == 3) { // bottom-right
            bx = (r.x2() - 1) - bw;
            by = (r.y2() - 1) - bh;
        }

        for (int y = by; y < by + bh; ++y) {
            for (int x = bx; x < bx + bw; ++x) {
                trySetWall(x, y);
            }
        }
    } else if (v == Variant::CentralBlock) {
        // Central wall block -> donut/ring corridor feel.
        int bw = rng.range(2, std::max(2, r.w / 2));
        int bh = rng.range(2, std::max(2, r.h / 2));
        bw = std::clamp(bw, 2, r.w - 4);
        bh = std::clamp(bh, 2, r.h - 4);
        if (bw < 2 || bh < 2) return false;

        int bx = r.cx() - bw / 2;
        int by = r.cy() - bh / 2;
        bx = std::clamp(bx, r.x + 2, (r.x2() - 2) - bw);
        by = std::clamp(by, r.y + 2, (r.y2() - 2) - bh);

        for (int y = by; y < by + bh; ++y) {
            for (int x = bx; x < bx + bw; ++x) {
                trySetWall(x, y);
            }
        }
    } else { // Partition
        // Add a wall stripe across the room with a small opening (sometimes an inner door).
        const bool vertical = (r.w >= r.h) ? rng.chance(0.65f) : rng.chance(0.35f);

        const int doorChance = std::clamp(12 + depth * 4, 12, 55); // % chance
        const int gapLen = (rng.range(0, 99) < std::clamp(70 - depth * 3, 40, 70)) ? 2 : 3;

        if (vertical) {
            const int xLineMin = r.x + 2;
            const int xLineMax = r.x2() - 3;
            if (xLineMin > xLineMax) return false;
            const int xLine = rng.range(xLineMin, xLineMax);

            const int gapMin = r.y + 2;
            const int gapMax = (r.y2() - 2) - gapLen;
            if (gapMin > gapMax) return false;
            const int gapY = rng.range(gapMin, gapMax);

            for (int y = r.y + 1; y < r.y2() - 1; ++y) {
                if (y >= gapY && y < gapY + gapLen) continue;
                trySetWall(xLine, y);
            }

            if (rng.range(0, 99) < doorChance) {
                trySetInteriorDoor(xLine, gapY + gapLen / 2);
            }
        } else {
            const int yLineMin = r.y + 2;
            const int yLineMax = r.y2() - 3;
            if (yLineMin > yLineMax) return false;
            const int yLine = rng.range(yLineMin, yLineMax);

            const int gapMin = r.x + 2;
            const int gapMax = (r.x2() - 2) - gapLen;
            if (gapMin > gapMax) return false;
            const int gapX = rng.range(gapMin, gapMax);

            for (int x = r.x + 1; x < r.x2() - 1; ++x) {
                if (x >= gapX && x < gapX + gapLen) continue;
                trySetWall(x, yLine);
            }

            if (rng.range(0, 99) < doorChance) {
                trySetInteriorDoor(gapX + gapLen / 2, yLine);
            }
        }
    }

    if (changes.empty()) return false;

    // Validate local (room) connectivity and global stairs connectivity.
    if (!roomInteriorConnectedSingleComponent(d, r, doorInside) || !stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    return true;
}

static void addRoomShapeVariety(Dungeon& d, RNG& rng, int depth) {
    if (d.rooms.empty()) return;

    std::vector<int> candidates;
    candidates.reserve(d.rooms.size());

    for (size_t i = 0; i < d.rooms.size(); ++i) {
        const Room& r = d.rooms[i];
        if (r.type != RoomType::Normal) continue;

        // Avoid the start/end rooms that hold stairs.
        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;

        // Skip tiny rooms.
        if (r.w < 8 || r.h < 8) continue;

        // Must have at least one door on its boundary.
        bool hasDoor = false;
        for (int x = r.x; x < r.x2(); ++x) {
            if (d.inBounds(x, r.y) && isDoorTileType(d.at(x, r.y).type)) { hasDoor = true; break; }
            if (d.inBounds(x, r.y2() - 1) && isDoorTileType(d.at(x, r.y2() - 1).type)) { hasDoor = true; break; }
        }
        if (!hasDoor) {
            for (int y = r.y; y < r.y2(); ++y) {
                if (d.inBounds(r.x, y) && isDoorTileType(d.at(r.x, y).type)) { hasDoor = true; break; }
                if (d.inBounds(r.x2() - 1, y) && isDoorTileType(d.at(r.x2() - 1, y).type)) { hasDoor = true; break; }
            }
        }
        if (!hasDoor) continue;

        candidates.push_back(static_cast<int>(i));
    }

    if (candidates.empty()) return;

    // Shuffle candidates deterministically via RNG.
    for (int i = static_cast<int>(candidates.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(candidates[static_cast<size_t>(i)], candidates[static_cast<size_t>(j)]);
    }

    const int maxShapes = std::clamp(1 + depth / 4, 1, 3);
    int target = std::min(maxShapes, static_cast<int>(candidates.size()));
    if (target > 1) target = rng.range(1, target);

    int shaped = 0;
    for (int idxRoom : candidates) {
        if (shaped >= target) break;

        // Slightly more aggressive deeper in the dungeon.
        float p = 0.40f + 0.05f * std::min(depth, 10);
        p = std::clamp(p, 0.35f, 0.90f);

        if (shaped == 0 || rng.chance(p)) {
            if (tryShapeNormalRoom(d, d.rooms[static_cast<size_t>(idxRoom)], rng, depth)) {
                shaped++;
            }
        }
    }
}

static Vec2i pickFarthestFloorInRoom(const Dungeon& d, const Room& r,
                                     const std::vector<Vec2i>& from,
                                     const std::vector<uint8_t>& keep) {
    Vec2i best{-1, -1};
    int bestScore = -1;

    for (int y = r.y + 1; y < r.y2() - 1; ++y) {
        for (int x = r.x + 1; x < r.x2() - 1; ++x) {
            if (!d.inBounds(x, y)) continue;
            if (!keep.empty() && keep[static_cast<size_t>(y * d.width + x)] != 0) continue;
            if (d.at(x, y).type != TileType::Floor) continue;

            int mind = 999999;
            if (from.empty()) {
                mind = std::abs(x - r.cx()) + std::abs(y - r.cy());
            } else {
                for (const Vec2i& q : from) {
                    mind = std::min(mind, std::abs(x - q.x) + std::abs(y - q.y));
                }
            }

            if (mind > bestScore) {
                bestScore = mind;
                best = {x, y};
            }
        }
    }

    return best;
}

static bool decorateArmoryRoom(Dungeon& d, const Room& r, RNG& rng, int depth) {
    (void)depth;
    if (r.w < 8 || r.h < 8) return false;

    std::vector<Vec2i> doors;
    std::vector<Vec2i> doorInside;
    buildRoomDoorInfo(d, r, doors, doorInside);

    // Keep door tiles + their immediate interior clear.
    std::vector<Vec2i> keepPts = doors;
    keepPts.insert(keepPts.end(), doorInside.begin(), doorInside.end());

    std::vector<uint8_t> keep;
    buildRoomKeepMask(d, keepPts, keep);

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(r.w * r.h / 4));

    const int minX = r.x + 2;
    const int maxX = r.x2() - 3;
    const int minY = r.y + 2;
    const int maxY = r.y2() - 3;
    if (minX > maxX || minY > maxY) return false;

    // Weapon racks: long lines of pillars with 1-2 "aisle" gaps.
    const bool vertical = (r.w >= r.h);
    if (vertical) {
        int startX = minX + (rng.chance(0.5f) ? 0 : 1);
        for (int x = startX; x <= maxX; x += 3) {
            const int gapY = rng.range(minY, maxY);
            for (int y = minY; y <= maxY; ++y) {
                if (std::abs(y - gapY) <= 1) continue;
                if (keep[static_cast<size_t>(y * d.width + x)] != 0) continue;
                trySetTile(d, x, y, TileType::Pillar, changes);
            }
        }
    } else {
        int startY = minY + (rng.chance(0.5f) ? 0 : 1);
        for (int y = startY; y <= maxY; y += 3) {
            const int gapX = rng.range(minX, maxX);
            for (int x = minX; x <= maxX; ++x) {
                if (std::abs(x - gapX) <= 1) continue;
                if (keep[static_cast<size_t>(y * d.width + x)] != 0) continue;
                trySetTile(d, x, y, TileType::Pillar, changes);
            }
        }
    }

    // A few crates / armor stands (boulders) for cover and boulder-bridge shenanigans.
    const int crates = rng.range(2, 4);
    for (int i = 0; i < crates; ++i) {
        for (int tries = 0; tries < 80; ++tries) {
            const int x = rng.range(minX, maxX);
            const int y = rng.range(minY, maxY);
            if (keep[static_cast<size_t>(y * d.width + x)] != 0) continue;
            trySetTile(d, x, y, TileType::Boulder, changes);
            if (d.at(x, y).type == TileType::Boulder) break;
        }
    }

    if (changes.empty()) return false;

    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    // Rare armory stash (bonus cache). Kept low so it doesn't flood the game with chests.
    if (rng.chance(0.18f)) {
        Vec2i p = pickFarthestFloorInRoom(d, r, doorInside, keep);
        if (d.inBounds(p.x, p.y)) d.bonusLootSpots.push_back(p);
    }

    return true;
}

static bool decorateLibraryRoom(Dungeon& d, const Room& r, RNG& rng, int depth) {
    (void)depth;
    if (r.w < 9 || r.h < 8) return false;

    std::vector<Vec2i> doors;
    std::vector<Vec2i> doorInside;
    buildRoomDoorInfo(d, r, doors, doorInside);

    std::vector<Vec2i> keepPts = doors;
    keepPts.insert(keepPts.end(), doorInside.begin(), doorInside.end());

    std::vector<uint8_t> keep;
    buildRoomKeepMask(d, keepPts, keep);

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(r.w * r.h / 3));

    const int minX = r.x + 2;
    const int maxX = r.x2() - 3;
    const int minY = r.y + 2;
    const int maxY = r.y2() - 3;
    if (minX > maxX || minY > maxY) return false;

    // Shelves: 2-tile-thick pillar "stacks" with alternating gaps to create aisles.
    const bool vertical = (r.w >= r.h);

    if (vertical) {
        int startX = minX + (rng.chance(0.5f) ? 0 : 1);
        for (int x = startX; x <= maxX; x += 4) {
            const bool gapTop = rng.chance(0.5f);
            const int gapY = gapTop ? (minY + rng.range(0, 1)) : (maxY - rng.range(0, 1));

            for (int y = minY; y <= maxY; ++y) {
                if (std::abs(y - gapY) <= 1) continue;

                for (int sx = 0; sx < 2; ++sx) {
                    const int xx = x + sx;
                    if (xx > maxX) continue;
                    if (keep[static_cast<size_t>(y * d.width + xx)] != 0) continue;
                    trySetTile(d, xx, y, TileType::Pillar, changes);
                }
            }
        }
    } else {
        int startY = minY + (rng.chance(0.5f) ? 0 : 1);
        for (int y = startY; y <= maxY; y += 4) {
            const bool gapLeft = rng.chance(0.5f);
            const int gapX = gapLeft ? (minX + rng.range(0, 1)) : (maxX - rng.range(0, 1));

            for (int x = minX; x <= maxX; ++x) {
                if (std::abs(x - gapX) <= 1) continue;

                for (int sy = 0; sy < 2; ++sy) {
                    const int yy = y + sy;
                    if (yy > maxY) continue;
                    if (keep[static_cast<size_t>(yy * d.width + x)] != 0) continue;
                    trySetTile(d, x, yy, TileType::Pillar, changes);
                }
            }
        }
    }

    // A couple of movable "book piles" (boulders) for soft cover.
    if (rng.chance(0.45f)) {
        const int piles = rng.range(1, 2);
        for (int i = 0; i < piles; ++i) {
            for (int tries = 0; tries < 80; ++tries) {
                const int x = rng.range(minX, maxX);
                const int y = rng.range(minY, maxY);
                if (keep[static_cast<size_t>(y * d.width + x)] != 0) continue;
                trySetTile(d, x, y, TileType::Boulder, changes);
                if (d.at(x, y).type == TileType::Boulder) break;
            }
        }
    }

    if (changes.empty()) return false;

    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    // Libraries frequently hide a "bonus" cache deep in the stacks.
    if (rng.chance(0.40f)) {
        Vec2i p = pickFarthestFloorInRoom(d, r, doorInside, keep);
        if (d.inBounds(p.x, p.y)) d.bonusLootSpots.push_back(p);
    }

    return true;
}

static void carveChasmBlobInRoom(Dungeon& d, const Room& r, RNG& rng,
                                 Vec2i start, int steps,
                                 const std::vector<uint8_t>& keep,
                                 std::vector<TileChange>& changes) {
    Vec2i p = start;

    const int minX = r.x + 2;
    const int maxX = r.x2() - 3;
    const int minY = r.y + 2;
    const int maxY = r.y2() - 3;

    auto clampToInterior = [&](Vec2i v) -> Vec2i {
        v.x = std::clamp(v.x, minX, maxX);
        v.y = std::clamp(v.y, minY, maxY);
        return v;
    };

    p = clampToInterior(p);

    for (int i = 0; i < steps; ++i) {
        // Paint a small "spill" footprint.
        const int radius = rng.chance(0.40f) ? 1 : 0;
        for (int oy = -radius; oy <= radius; ++oy) {
            for (int ox = -radius; ox <= radius; ++ox) {
                const int x = p.x + ox;
                const int y = p.y + oy;
                if (x < minX || x > maxX || y < minY || y > maxY) continue;
                if (!keep.empty() && keep[static_cast<size_t>(y * d.width + x)] != 0) continue;
                if (rng.chance(0.70f)) trySetTile(d, x, y, TileType::Chasm, changes);
            }
        }

        // Drunkard walk.
        const int dir = rng.range(0, 3);
        if (dir == 0) p.x += 1;
        else if (dir == 1) p.x -= 1;
        else if (dir == 2) p.y += 1;
        else p.y -= 1;

        p = clampToInterior(p);
    }
}

static bool decorateLaboratoryRoom(Dungeon& d, const Room& r, RNG& rng, int depth) {
    if (r.w < 8 || r.h < 8) return false;

    std::vector<Vec2i> doors;
    std::vector<Vec2i> doorInside;
    buildRoomDoorInfo(d, r, doors, doorInside);

    std::vector<Vec2i> keepPts = doors;
    keepPts.insert(keepPts.end(), doorInside.begin(), doorInside.end());

    std::vector<uint8_t> keep;
    buildRoomKeepMask(d, keepPts, keep);

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(r.w * r.h / 3));

    const int minX = r.x + 2;
    const int maxX = r.x2() - 3;
    const int minY = r.y + 2;
    const int maxY = r.y2() - 3;
    if (minX > maxX || minY > maxY) return false;

    // Chemical spill: one guaranteed blob, with a chance of a second on deeper floors.
    {
        Vec2i start{ rng.range(minX, maxX), rng.range(minY, maxY) };
        const int steps = rng.range(14, 26);
        carveChasmBlobInRoom(d, r, rng, start, steps, keep, changes);

        if (depth >= 5 && rng.chance(0.35f)) {
            Vec2i start2{ rng.range(minX, maxX), rng.range(minY, maxY) };
            const int steps2 = rng.range(10, 20);
            carveChasmBlobInRoom(d, r, rng, start2, steps2, keep, changes);
        }
    }

    // Lab benches / containment pods: small pillar clusters.
    const int clusters = rng.range(2, 4) + (depth >= 7 ? 1 : 0);
    for (int i = 0; i < clusters; ++i) {
        const int cx = rng.range(minX, maxX);
        const int cy = rng.range(minY, maxY);

        const Vec2i pts[5] = {
            {cx, cy},
            {cx + 1, cy},
            {cx - 1, cy},
            {cx, cy + 1},
            {cx, cy - 1},
        };

        for (const Vec2i& p : pts) {
            if (p.x < minX || p.x > maxX || p.y < minY || p.y > maxY) continue;
            if (!keep.empty() && keep[static_cast<size_t>(p.y * d.width + p.x)] != 0) continue;
            if (rng.chance(0.65f)) trySetTile(d, p.x, p.y, TileType::Pillar, changes);
        }
    }

    // Optional loose debris (movable).
    if (rng.chance(0.30f)) {
        for (int tries = 0; tries < 120; ++tries) {
            const int x = rng.range(minX, maxX);
            const int y = rng.range(minY, maxY);
            if (keep[static_cast<size_t>(y * d.width + x)] != 0) continue;
            trySetTile(d, x, y, TileType::Boulder, changes);
            if (d.at(x, y).type == TileType::Boulder) break;
        }
    }

    if (changes.empty()) return false;

    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    // Labs occasionally hide a cache in the cleanest corner.
    if (rng.chance(0.28f)) {
        Vec2i p = pickFarthestFloorInRoom(d, r, doorInside, keep);
        if (d.inBounds(p.x, p.y)) d.bonusLootSpots.push_back(p);
    }

    return true;
}

void decorateThemedRooms(Dungeon& d, RNG& rng, int depth) {
    for (const Room& r : d.rooms) {
        if (!isThemedRoom(r.type)) continue;

        // Avoid the start/end rooms that hold stairs (rare for themed rooms, but possible).
        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;

        if (r.type == RoomType::Armory) {
            (void)decorateArmoryRoom(d, r, rng, depth);
        } else if (r.type == RoomType::Library) {
            (void)decorateLibraryRoom(d, r, rng, depth);
        } else if (r.type == RoomType::Laboratory) {
            (void)decorateLaboratoryRoom(d, r, rng, depth);
        }
    }
}


// ------------------------------------------------------------
// Symmetric room furnishing (mirrored "furniture" patterns)
//
// A mid/late procgen pass that picks a few rooms and lays down mirrored
// pillar/boulder patterns. Symmetry makes spaces readable and memorable, but
// the biggest risk is accidentally cluttering a critical connection between
// corridors (rooms are sometimes used as through-paths).
//
// We address that by reserving a small "navigation spine" inside the room:
// for each doorway, we keep a short L-shaped corridor from the interior door
// tile to an interior anchor near the room center. Furnishing is then applied
// only off that reserved spine. As with other decoration passes, we validate
// global stair connectivity and roll back if anything goes wrong.
// ------------------------------------------------------------

enum class SymFurnishMode : uint8_t {
    Vertical = 0,
    Horizontal,
    Both,
};

static Vec2i pickRoomInteriorAnchor(const Dungeon& d, const Room& r) {
    // Prefer a passable tile near center (keeps the reserved paths short).
    const int cx = r.cx();
    const int cy = r.cy();

    auto interior = [&](int x, int y) {
        return x > r.x && x < (r.x2() - 1) && y > r.y && y < (r.y2() - 1);
    };

    const int maxRad = std::max(r.w, r.h);
    for (int rad = 0; rad <= maxRad; ++rad) {
        for (int dy = -rad; dy <= rad; ++dy) {
            for (int dx = -rad; dx <= rad; ++dx) {
                const int x = cx + dx;
                const int y = cy + dy;
                if (!d.inBounds(x, y)) continue;
                if (!interior(x, y)) continue;
                if (!d.isPassable(x, y)) continue;
                if (d.at(x, y).type != TileType::Floor) continue;
                return {x, y};
            }
        }
    }

    return {cx, cy};
}

static void appendKeepPoint(std::vector<Vec2i>& keepPts, const Dungeon& d, const Room& r, Vec2i p, int rad) {
    if (!d.inBounds(p.x, p.y)) return;
    // Keep within room bounds (including boundary) so we don't reserve outside space.
    if (!r.contains(p.x, p.y)) return;

    const int rr = std::max(0, rad);
    for (int oy = -rr; oy <= rr; ++oy) {
        for (int ox = -rr; ox <= rr; ++ox) {
            const int x = p.x + ox;
            const int y = p.y + oy;
            if (!d.inBounds(x, y)) continue;
            if (!r.contains(x, y)) continue;
            if (std::abs(ox) + std::abs(oy) > rr) continue;
            keepPts.push_back({x, y});
        }
    }
}

static void appendKeepLPath(std::vector<Vec2i>& keepPts, const Dungeon& d, const Room& r, Vec2i a, Vec2i b, bool horizFirst) {
    // Clamp to room interior-ish region.
    auto clampRoom = [&](Vec2i p) {
        p.x = std::clamp(p.x, r.x + 1, r.x2() - 2);
        p.y = std::clamp(p.y, r.y + 1, r.y2() - 2);
        return p;
    };

    a = clampRoom(a);
    b = clampRoom(b);

    auto stepTo = [&](int& v, int target) {
        if (v < target) v++;
        else if (v > target) v--;
    };

    int x = a.x;
    int y = a.y;

    // Always keep a small radius around the path so it doesn't become a single-tile choke.
    appendKeepPoint(keepPts, d, r, {x, y}, 1);

    if (horizFirst) {
        while (x != b.x) {
            stepTo(x, b.x);
            appendKeepPoint(keepPts, d, r, {x, y}, 1);
        }
        while (y != b.y) {
            stepTo(y, b.y);
            appendKeepPoint(keepPts, d, r, {x, y}, 1);
        }
    } else {
        while (y != b.y) {
            stepTo(y, b.y);
            appendKeepPoint(keepPts, d, r, {x, y}, 1);
        }
        while (x != b.x) {
            stepTo(x, b.x);
            appendKeepPoint(keepPts, d, r, {x, y}, 1);
        }
    }
}

static bool trySymmetricFurnishRoom(Dungeon& d, const Room& r, RNG& rng, int depth, int& outPlaced) {
    outPlaced = 0;

    if (r.w < 9 || r.h < 8) return false;

    // Skip rooms with stairs (critical flow + readability).
    if (r.contains(d.stairsUp.x, d.stairsUp.y)) return false;
    if (r.contains(d.stairsDown.x, d.stairsDown.y)) return false;

    // Gather doors + interior door tiles.
    std::vector<Vec2i> doors;
    std::vector<Vec2i> doorInside;
    buildRoomDoorInfo(d, r, doors, doorInside);
    if (doors.empty()) return false;

    // Prefer not to double-decorate heavily cluttered rooms.
    int interior = 0;
    int blocked = 0;
    for (int y = r.y + 1; y < r.y2() - 1; ++y) {
        for (int x = r.x + 1; x < r.x2() - 1; ++x) {
            if (!d.inBounds(x, y)) continue;
            const TileType tt = d.at(x, y).type;
            if (tt == TileType::Wall) continue;
            ++interior;
            if (tt == TileType::Pillar || tt == TileType::Boulder || tt == TileType::Chasm) ++blocked;
        }
    }

    // If more than ~10% of interior is already blocked, skip.
    if (interior > 0 && (blocked * 100) / interior > 10) return false;

    // Choose symmetry mode.
    SymFurnishMode mode = SymFurnishMode::Vertical;
    if (r.w >= 10 && r.h >= 10 && rng.chance(0.25f)) {
        mode = SymFurnishMode::Both;
    } else if (rng.chance(0.5f)) {
        mode = SymFurnishMode::Horizontal;
    }

    // Anchor for reserved door-to-door movement inside the room.
    const Vec2i anchor = pickRoomInteriorAnchor(d, r);

    // Build keep points: doors + a short corridor from each interior door tile to the anchor.
    std::vector<Vec2i> keepPts;
    keepPts.reserve(64);
    for (const Vec2i& p : doors) appendKeepPoint(keepPts, d, r, p, 0);
    for (const Vec2i& p : doorInside) appendKeepPoint(keepPts, d, r, p, 1);

    // Door-to-anchor spines.
    for (const Vec2i& p : doorInside) {
        if (!d.inBounds(p.x, p.y)) continue;
        if (!r.contains(p.x, p.y)) continue;
        if (!d.isPassable(p.x, p.y)) continue;
        appendKeepLPath(keepPts, d, r, p, anchor, rng.chance(0.5f));
    }

    std::vector<uint8_t> keep;
    buildRoomKeepMask(d, keepPts, keep);

    // Placement region (avoid hugging walls).
    const int minX = r.x + 2;
    const int maxX = r.x2() - 3;
    const int minY = r.y + 2;
    const int maxY = r.y2() - 3;
    if (minX > maxX || minY > maxY) return false;

    auto reflectX = [&](int x) { return (r.x + r.w - 1) - x; };
    auto reflectY = [&](int y) { return (r.y + r.h - 1) - y; };

    auto isPlaceable = [&](int x, int y) {
        if (x < minX || x > maxX || y < minY || y > maxY) return false;
        if (!d.inBounds(x, y)) return false;
        if (!r.contains(x, y)) return false;
        if (!keep.empty() && keep[static_cast<size_t>(y * d.width + x)] != 0) return false;
        return d.at(x, y).type == TileType::Floor;
    };

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>((r.w * r.h) / 6));

    // Target obstacle density: small and readable.
    const int area = std::max(1, (maxX - minX + 1) * (maxY - minY + 1));
    int target = std::clamp(area / 16, 4, 16);
    // Deeper floors can be slightly more cluttered.
    target = std::min(18, target + std::max(0, depth - 4) / 2);

    int placed = 0;

    auto placeSet = [&](const std::vector<Vec2i>& pts, TileType tt) {
        for (const Vec2i& p : pts) {
            trySetTile(d, p.x, p.y, tt, changes);
        }
    };

    // Place mirrored obstacles with retries.
    for (int tries = 0; tries < 220 && placed < target; ++tries) {
        const int x = rng.range(minX, maxX);
        const int y = rng.range(minY, maxY);

        std::vector<Vec2i> pts;
        pts.reserve(4);
        pts.push_back({x, y});

        if (mode == SymFurnishMode::Vertical || mode == SymFurnishMode::Both) {
            pts.push_back({reflectX(x), y});
        }
        if (mode == SymFurnishMode::Horizontal || mode == SymFurnishMode::Both) {
            pts.push_back({x, reflectY(y)});
        }
        if (mode == SymFurnishMode::Both) {
            pts.push_back({reflectX(x), reflectY(y)});
        }

        // Unique points only.
        {
            std::vector<Vec2i> uniq;
            uniq.reserve(pts.size());
            for (const Vec2i& p : pts) {
                bool seen = false;
                for (const Vec2i& q : uniq) {
                    if (p.x == q.x && p.y == q.y) { seen = true; break; }
                }
                if (!seen) uniq.push_back(p);
            }
            pts.swap(uniq);
        }

        bool ok = true;
        for (const Vec2i& p : pts) {
            if (!isPlaceable(p.x, p.y)) { ok = false; break; }
        }
        if (!ok) continue;

        // Choose obstacle type. Early floors: pillars read cleaner; deeper: more boulders.
        float pBoulder = 0.22f + 0.03f * std::clamp(depth - 1, 0, 10);
        pBoulder = std::clamp(pBoulder, 0.18f, 0.55f);

        TileType tt = rng.chance(pBoulder) ? TileType::Boulder : TileType::Pillar;

        // Rarely introduce a tiny symmetric chasm "pool" on deeper floors.
        if (depth >= 6 && rng.chance(0.06f)) {
            tt = TileType::Chasm;
        }

        placeSet(pts, tt);
        placed += static_cast<int>(pts.size());
    }

    if (changes.empty()) return false;

    // Safety: never break global connectivity.
    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }

    outPlaced = placed;
    return true;
}

static void applySymmetricRoomFurnishings(Dungeon& d, RNG& rng, int depth) {
    d.symmetryRoomCount = 0;
    d.symmetryObstacleCount = 0;

    if (d.rooms.empty()) return;

    // Chance ramps up slightly with depth (deeper floors benefit from more visual variety).
    float chance = 0.45f + 0.04f * std::min(depth, 10);
    chance = std::clamp(chance, 0.40f, 0.85f);
    if (!rng.chance(chance)) return;

    std::vector<int> candidates;
    candidates.reserve(d.rooms.size());

    for (size_t i = 0; i < d.rooms.size(); ++i) {
        const Room& r = d.rooms[i];

        // Keep high-impact special rooms clean/readable.
        if (r.type == RoomType::Shop) continue;
        if (r.type == RoomType::Shrine) continue;
        if (r.type == RoomType::Secret) continue;
        if (r.type == RoomType::Vault) continue;

        // Themed rooms have their own furniture.
        if (isThemedRoom(r.type)) continue;

        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;

        if (r.w < 9 || r.h < 8) continue;

        candidates.push_back(static_cast<int>(i));
    }

    if (candidates.empty()) return;

    // Shuffle candidates deterministically.
    for (int i = static_cast<int>(candidates.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(candidates[static_cast<size_t>(i)], candidates[static_cast<size_t>(j)]);
    }

    const int maxRooms = std::clamp(1 + depth / 6, 1, 2);
    int targetRooms = std::min(maxRooms, static_cast<int>(candidates.size()));
    if (targetRooms > 1) targetRooms = rng.range(1, targetRooms);

    int furnished = 0;
    int obstacles = 0;

    for (int ri : candidates) {
        if (furnished >= targetRooms) break;
        if (ri < 0 || ri >= static_cast<int>(d.rooms.size())) continue;

        int placed = 0;
        if (trySymmetricFurnishRoom(d, d.rooms[static_cast<size_t>(ri)], rng, depth, placed)) {
            furnished += 1;
            obstacles += placed;
        }
    }

    d.symmetryRoomCount = furnished;
    d.symmetryObstacleCount = obstacles;
}

// ------------------------------------------------------------
// WFC-based room furnishing (constraint-driven micro-patterns)
//
// This pass uses a lightweight Wave Function Collapse solver to stamp
// recognizable "motifs" (colonnades / rubble clusters / broken zigzags)
// inside a couple of large *normal* rooms.
//
// Key constraints:
//  - Deterministic: all choices are driven by the dungeon RNG.
//  - Connectivity safe: reserve door-to-anchor spines + validate both room
//    interior connectivity and global stairs connectivity (rollback on fail).
//  - Low density: operates on a coarse grid so obstacles stay readable.
// ------------------------------------------------------------

enum class WfcFurnishTile : uint8_t {
    Floor  = 0,
    Pillar = 1,
    Boulder = 2,
    Chasm  = 3,
};

static int wfcFurnishTileFromChar(char c) {
    switch (c) {
        case '#': return static_cast<int>(WfcFurnishTile::Pillar);
        case 'o': return static_cast<int>(WfcFurnishTile::Boulder);
        case '~': return static_cast<int>(WfcFurnishTile::Chasm);
        default:  return static_cast<int>(WfcFurnishTile::Floor);
    }
}

static std::vector<std::string> rot90(const std::vector<std::string>& g) {
    if (g.empty() || g[0].empty()) return g;
    const int h = static_cast<int>(g.size());
    const int w = static_cast<int>(g[0].size());
    std::vector<std::string> out(static_cast<size_t>(w), std::string(static_cast<size_t>(h), '.'));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            out[static_cast<size_t>(x)][static_cast<size_t>((h - 1) - y)] = g[static_cast<size_t>(y)][static_cast<size_t>(x)];
        }
    }
    return out;
}

static std::vector<std::string> flipH(const std::vector<std::string>& g) {
    if (g.empty() || g[0].empty()) return g;
    const int h = static_cast<int>(g.size());
    const int w = static_cast<int>(g[0].size());
    std::vector<std::string> out = g;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            out[static_cast<size_t>(y)][static_cast<size_t>(x)] = g[static_cast<size_t>(y)][static_cast<size_t>((w - 1) - x)];
        }
    }
    return out;
}

static void buildWfcFurnishRules(std::vector<uint32_t> allow[4]) {
    constexpr int nTiles = 4;
    for (int dir = 0; dir < 4; ++dir) allow[dir].assign(nTiles, 0u);

    auto addSample = [&](const std::vector<std::string>& g) {
        if (g.empty() || g[0].empty()) return;
        const int h = static_cast<int>(g.size());
        const int w = static_cast<int>(g[0].size());
        for (int y = 1; y < h; ++y) {
            if (static_cast<int>(g[static_cast<size_t>(y)].size()) != w) return; // must be rectangular
        }

        auto at = [&](int x, int y) -> int {
            // Wrap so the sample behaves like a repeating pattern.
            x = (x % w + w) % w;
            y = (y % h + h) % h;
            return wfcFurnishTileFromChar(g[static_cast<size_t>(y)][static_cast<size_t>(x)]);
        };

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int a = at(x, y);
                const int bx = at(x + 1, y);
                const int bxn = at(x - 1, y);
                const int by = at(x, y + 1);
                const int byn = at(x, y - 1);

                allow[0][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(bx));
                allow[1][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(bxn));
                allow[2][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(by));
                allow[3][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(byn));
            }
        }
    };

    auto addSampleWithSym = [&](const std::vector<std::string>& base) {
        std::vector<std::string> g = base;
        for (int r = 0; r < 4; ++r) {
            addSample(g);
            addSample(flipH(g));
            g = rot90(g);
        }
    };

    // Motif samples (coarse-grid). Characters:
    //  '.' floor, '#' pillar, 'o' boulder.
    // Chasm is handled by explicit rules below.

    // Colonnade stripes.
    addSampleWithSym({
        "...........",
        "..#..#..#..",
        "..#..#..#..",
        "..#..#..#..",
        "..#..#..#..",
        "...........",
        "..#..#..#..",
        "..#..#..#..",
        "..#..#..#..",
        "..#..#..#..",
        "...........",
    });

    // Rubble blobs.
    addSampleWithSym({
        "...........",
        "....o......",
        "...ooo.....",
        "....oo.....",
        "...........",
        "..o....o...",
        ".ooo..ooo..",
        "..o....o...",
        "...........",
        ".....oo....",
        "...........",
    });

    // Broken zig-zag walls (pillars) with gaps.
    addSampleWithSym({
        "...........",
        "..###......",
        ".....###...",
        "..###......",
        ".....###...",
        "..###......",
        ".....###...",
        "..###......",
        "...........",
        "...###.....",
        "...........",
    });

    const uint32_t all = wfc::allMask(nTiles);
    const uint32_t floorBit = 1u << static_cast<uint32_t>(WfcFurnishTile::Floor);
    const uint32_t chasmBit = 1u << static_cast<uint32_t>(WfcFurnishTile::Chasm);

    // Safety: fill any missing adjacency with "anything" to avoid accidental unsat.
    for (int dir = 0; dir < 4; ++dir) {
        for (int t = 0; t < nTiles; ++t) {
            if (allow[dir][static_cast<size_t>(t)] == 0u) allow[dir][static_cast<size_t>(t)] = all;
        }
    }

    // Floor is permissive.
    for (int dir = 0; dir < 4; ++dir) {
        allow[dir][static_cast<size_t>(WfcFurnishTile::Floor)] = all;
    }

    // Chasm: only surrounded by floor on the coarse grid.
    for (int dir = 0; dir < 4; ++dir) {
        allow[dir][static_cast<size_t>(WfcFurnishTile::Chasm)] = floorBit;

        // Keep chasm symmetric: only floors may allow chasms as neighbors.
        allow[dir][static_cast<size_t>(WfcFurnishTile::Pillar)] &= ~chasmBit;
        allow[dir][static_cast<size_t>(WfcFurnishTile::Boulder)] &= ~chasmBit;
    }
}

static bool tryWfcFurnishRoom(Dungeon& d, const Room& r, RNG& rng, int depth, int& outPlaced) {
    outPlaced = 0;

    if (r.type != RoomType::Normal) return false;
    if (r.w < 12 || r.h < 10) return false;

    if (r.contains(d.stairsUp.x, d.stairsUp.y)) return false;
    if (r.contains(d.stairsDown.x, d.stairsDown.y)) return false;

    // Require a clean interior (rooms already shaped/decorated are already memorable).
    for (int y = r.y + 1; y < r.y2() - 1; ++y) {
        for (int x = r.x + 1; x < r.x2() - 1; ++x) {
            if (!d.inBounds(x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) return false;
        }
    }

    std::vector<Vec2i> doors;
    std::vector<Vec2i> doorInside;
    buildRoomDoorInfo(d, r, doors, doorInside);
    if (doors.empty()) return false;

    const Vec2i anchor = pickRoomInteriorAnchor(d, r);

    // Keep door tiles + interior door tiles, and reserve door-to-anchor spines.
    std::vector<Vec2i> keepPts;
    keepPts.reserve(128);
    for (const Vec2i& p : doors) appendKeepPoint(keepPts, d, r, p, 0);
    for (const Vec2i& p : doorInside) appendKeepPoint(keepPts, d, r, p, 1);
    for (const Vec2i& p : doorInside) {
        if (!d.inBounds(p.x, p.y)) continue;
        if (!r.contains(p.x, p.y)) continue;
        appendKeepLPath(keepPts, d, r, p, anchor, rng.chance(0.5f));
    }

    std::vector<uint8_t> keep;
    buildRoomKeepMask(d, keepPts, keep);

    const int minX = r.x + 2;
    const int maxX = r.x2() - 3;
    const int minY = r.y + 2;
    const int maxY = r.y2() - 3;
    if (minX > maxX || minY > maxY) return false;

    // WFC works on a coarse lattice so we don't over-clutter.
    int step = 2;
    const int interiorArea = (maxX - minX + 1) * (maxY - minY + 1);
    if (interiorArea > 900) step = 3;

    // Build rules once.
    static bool rulesBuilt = false;
    static std::vector<uint32_t> allow[4];
    if (!rulesBuilt) {
        buildWfcFurnishRules(allow);
        rulesBuilt = true;
    }

    constexpr int nTiles = 4;
    const uint32_t floorBit = 1u << static_cast<uint32_t>(WfcFurnishTile::Floor);
    const uint32_t pillarBit = 1u << static_cast<uint32_t>(WfcFurnishTile::Pillar);
    const uint32_t boulderBit = 1u << static_cast<uint32_t>(WfcFurnishTile::Boulder);
    const uint32_t chasmBit = 1u << static_cast<uint32_t>(WfcFurnishTile::Chasm);

    // Slightly more chasm variety deeper, but keep it rare.
    const bool allowChasm = (depth >= 6) && rng.chance(0.22f);

    // Tile weights: goal is ~25-35% obstacles on the coarse grid (~6-9% per-tile).
    const float dd = static_cast<float>(std::clamp(depth, 0, 12));
    std::vector<float> weights(static_cast<size_t>(nTiles), 0.0f);
    weights[static_cast<size_t>(WfcFurnishTile::Floor)] = 100.0f;
    weights[static_cast<size_t>(WfcFurnishTile::Pillar)] = 16.0f + 1.4f * dd;
    weights[static_cast<size_t>(WfcFurnishTile::Boulder)] = 12.0f + 1.2f * dd;
    weights[static_cast<size_t>(WfcFurnishTile::Chasm)] = allowChasm ? (1.0f + 0.25f * std::max(0.0f, dd - 6.0f)) : 0.0f;

    // Try a few layouts (different coarse offsets) to avoid unlucky contradictions.
    const int maxAttempts = 5;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        int startX = minX + rng.range(0, step - 1);
        int startY = minY + rng.range(0, step - 1);
        if (startX > maxX) startX = minX;
        if (startY > maxY) startY = minY;

        const int gw = (maxX - startX) / step + 1;
        const int gh = (maxY - startY) / step + 1;
        if (gw <= 0 || gh <= 0) continue;
        if (gw * gh > 800) continue;

        auto idx = [&](int cx, int cy) -> size_t { return static_cast<size_t>(cy * gw + cx); };

        std::vector<uint32_t> domains(static_cast<size_t>(gw * gh), floorBit | pillarBit | boulderBit);

        for (int cy = 0; cy < gh; ++cy) {
            for (int cx = 0; cx < gw; ++cx) {
                const int x = startX + cx * step;
                const int y = startY + cy * step;
                if (!d.inBounds(x, y) || !r.contains(x, y)) {
                    domains[idx(cx, cy)] = floorBit;
                    continue;
                }

                const size_t ii = static_cast<size_t>(y * d.width + x);
                if (!keep.empty() && ii < keep.size() && keep[ii] != 0) {
                    domains[idx(cx, cy)] = floorBit;
                    continue;
                }

                if (isStairsTile(d, x, y)) {
                    domains[idx(cx, cy)] = floorBit;
                    continue;
                }

                // Chasm only far from entrances/anchor.
                if (allowChasm) {
                    int mind = std::abs(x - anchor.x) + std::abs(y - anchor.y);
                    for (const Vec2i& p : doorInside) {
                        mind = std::min(mind, std::abs(x - p.x) + std::abs(y - p.y));
                    }
                    if (mind >= 7) {
                        domains[idx(cx, cy)] |= chasmBit;
                    }
                }
            }
        }

        std::vector<uint8_t> sol;
        wfc::SolveStats stats;
        const bool ok = wfc::solve(gw, gh, nTiles, allow, weights, rng, domains, sol, /*maxRestarts=*/10, &stats);
        (void)stats;
        if (!ok) continue;

        std::vector<TileChange> changes;
        changes.reserve(static_cast<size_t>((gw * gh) / 2));

        int placed = 0;
        for (int cy = 0; cy < gh; ++cy) {
            for (int cx = 0; cx < gw; ++cx) {
                const int x = startX + cx * step;
                const int y = startY + cy * step;
                if (!d.inBounds(x, y) || !r.contains(x, y)) continue;
                const size_t ii = static_cast<size_t>(y * d.width + x);
                if (!keep.empty() && ii < keep.size() && keep[ii] != 0) continue;

                const uint8_t t = sol[idx(cx, cy)];
                if (t == static_cast<uint8_t>(WfcFurnishTile::Pillar)) {
                    trySetTile(d, x, y, TileType::Pillar, changes);
                } else if (t == static_cast<uint8_t>(WfcFurnishTile::Boulder)) {
                    trySetTile(d, x, y, TileType::Boulder, changes);
                } else if (t == static_cast<uint8_t>(WfcFurnishTile::Chasm)) {
                    trySetTile(d, x, y, TileType::Chasm, changes);
                }
            }
        }

        placed = static_cast<int>(changes.size());
        if (placed < 6) {
            // Too subtle; try again.
            undoChanges(d, changes);
            continue;
        }

        // Validate both local (room) connectivity and global stairs connectivity.
        if (!roomInteriorConnectedSingleComponent(d, r, doorInside) || !stairsConnected(d)) {
            undoChanges(d, changes);
            continue;
        }

        outPlaced = placed;
        return true;
    }

    return false;
}

static void applyWfcRoomFurnishings(Dungeon& d, RNG& rng, int depth) {
    if (d.rooms.empty()) return;

    // Ramp chance with depth; deeper floors benefit from more micro-structure.
    float p = 0.22f + 0.04f * std::min(depth, 10);
    p = std::clamp(p, 0.18f, 0.70f);
    if (!rng.chance(p)) return;

    std::vector<int> candidates;
    candidates.reserve(d.rooms.size());

    for (size_t i = 0; i < d.rooms.size(); ++i) {
        const Room& r = d.rooms[i];
        if (r.type != RoomType::Normal) continue;
        if (isThemedRoom(r.type)) continue;
        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;
        if (r.w < 12 || r.h < 10) continue;
        candidates.push_back(static_cast<int>(i));
    }

    if (candidates.empty()) return;

    // Shuffle deterministically.
    for (int i = static_cast<int>(candidates.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(candidates[static_cast<size_t>(i)], candidates[static_cast<size_t>(j)]);
    }

    const int maxRooms = std::clamp(1 + depth / 10, 1, 2);
    int target = std::min(maxRooms, static_cast<int>(candidates.size()));
    if (target > 1 && rng.chance(0.55f)) target = 1;

    int furnished = 0;
    for (int ri : candidates) {
        if (furnished >= target) break;
        if (ri < 0 || ri >= static_cast<int>(d.rooms.size())) continue;
        int placed = 0;
        if (tryWfcFurnishRoom(d, d.rooms[static_cast<size_t>(ri)], rng, depth, placed)) {
            furnished++;
        }
    }
}
// ------------------------------------------------------------
// Global fissure / ravine feature
//
// A long, meandering chasm line that can slice through any procedural floor.
// We keep it strictly optional and always preserve (or repair) stairs connectivity.
// Deep Mines always get at least one fissure for extra tactical terrain.
// ------------------------------------------------------------

inline bool nearStairs(const Dungeon& d, int x, int y, int rad) {
    if (!d.inBounds(x, y)) return true;
    if (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y) <= rad) return true;
    if (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y) <= rad) return true;
    return false;
}

void forceSetTileFeature(Dungeon& d, int x, int y, TileType t, std::vector<TileChange>& changes) {
    if (!d.inBounds(x, y)) return;
    if (isStairsTile(d, x, y)) return;

    Tile& cur = d.at(x, y);
    if (cur.type == t) return;

    // Don't destroy doors via global terrain features.
    if (isDoorTileType(cur.type)) return;

    changes.push_back({x, y, cur.type});
    cur.type = t;
}

std::vector<int> computePassableComponents(const Dungeon& d, int& outCount) {
    const int W = d.width;
    const int H = d.height;
    outCount = 0;
    std::vector<int> comp(static_cast<size_t>(W * H), -1);

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    std::deque<Vec2i> q;

    auto flood = [&](int sx, int sy, int label) {
        q.clear();
        q.push_back({sx, sy});
        comp[idx(sx, sy)] = label;
        while (!q.empty()) {
            Vec2i p = q.front();
            q.pop_front();
            for (auto& dv : dirs) {
                const int nx = p.x + dv[0];
                const int ny = p.y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (!d.isPassable(nx, ny)) continue;
                const size_t ii = idx(nx, ny);
                if (comp[ii] != -1) continue;
                comp[ii] = label;
                q.push_back({nx, ny});
            }
        }
    };

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (!d.isPassable(x, y)) continue;
            const size_t ii = idx(x, y);
            if (comp[ii] != -1) continue;
            flood(x, y, outCount);
            outCount++;
        }
    }

    return comp;
}

bool placeRavineBridge(Dungeon& d, RNG& rng, std::vector<TileChange>& changes,
                       const std::vector<int>* comp = nullptr, int compA = -1, int compB = -1) {
    const int W = d.width;
    const int H = d.height;
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    struct Cand {
        Vec2i p;
        int score = 0;
    };

    std::vector<Cand> cands;
    cands.reserve(static_cast<size_t>((W * H) / 32));

    auto okSide = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        return d.isPassable(x, y);
    };

    auto compAt = [&](int x, int y) -> int {
        if (!comp) return -1;
        const size_t ii = idx(x, y);
        if (ii >= comp->size()) return -1;
        return (*comp)[ii];
    };

    auto consider = [&](int x, int y, int ax, int ay, int bx, int by) {
        if (!d.inBounds(x, y)) return;
        if (d.at(x, y).type != TileType::Chasm) return;
        if (nearStairs(d, x, y, 3)) return;

        if (!okSide(ax, ay) || !okSide(bx, by)) return;

        if (comp && compA >= 0 && compB >= 0) {
            const int ca = compAt(ax, ay);
            const int cb = compAt(bx, by);
            if (ca < 0 || cb < 0) return;
            if (!((ca == compA && cb == compB) || (ca == compB && cb == compA))) return;
        }

        // Favor bridges nearer the middle of the map and nearer the stairs line.
        int score = 0;
        const int cx = W / 2;
        const int cy = H / 2;
        score -= std::abs(x - cx) + std::abs(y - cy);
        score -= (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y)) / 4;
        score -= (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y)) / 4;
        score += rng.range(-3, 3);

        cands.push_back({{x, y}, score});
    };

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            // Horizontal bridge.
            consider(x, y, x - 1, y, x + 1, y);
            // Vertical bridge.
            consider(x, y, x, y - 1, x, y + 1);
        }
    }

    if (cands.empty()) return false;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.score > b.score;
    });

    const int topN = std::min(6, static_cast<int>(cands.size()));
    const int pick = rng.range(0, topN - 1);
    const Vec2i p = cands[static_cast<size_t>(pick)].p;

    forceSetTileFeature(d, p.x, p.y, TileType::Floor, changes);
    return true;
}

std::vector<Vec2i> buildRavinePath(const Dungeon& d, RNG& rng) {
    const int W = d.width;
    const int H = d.height;

    const bool horizontal = rng.chance(0.5f);
    Vec2i start;
    Vec2i goal;
    if (horizontal) {
        start = {1, rng.range(2, std::max(2, H - 3))};
        goal  = {W - 2, rng.range(2, std::max(2, H - 3))};
    } else {
        start = {rng.range(2, std::max(2, W - 3)), 1};
        goal  = {rng.range(2, std::max(2, W - 3)), H - 2};
    }

    auto clampInterior = [&](Vec2i p) -> Vec2i {
        p.x = clampi(p.x, 1, W - 2);
        p.y = clampi(p.y, 1, H - 2);
        return p;
    };

    Vec2i cur = clampInterior(start);
    goal = clampInterior(goal);

    std::vector<Vec2i> path;
    path.reserve(static_cast<size_t>(std::max(16, (W + H) * 3)));
    path.push_back(cur);

    Vec2i lastDir{0, 0};

    auto sgn = [](int v) { return (v > 0) - (v < 0); };

    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    const int maxSteps = std::max(64, (W + H) * 6);
    for (int step = 0; step < maxSteps && !(cur == goal); ++step) {
        const int dx = goal.x - cur.x;
        const int dy = goal.y - cur.y;

        struct Opt { Vec2i d; int w; };
        Opt opts[8];
        int n = 0;

        auto add = [&](Vec2i d, int w) {
            if (w <= 0) return;
            if (d.x == 0 && d.y == 0) return;
            // Don't add duplicates.
            for (int i = 0; i < n; ++i) {
                if (opts[i].d.x == d.x && opts[i].d.y == d.y) {
                    opts[i].w = std::max(opts[i].w, w);
                    return;
                }
            }
            opts[n++] = {d, w};
        };

        // Strong bias toward making progress.
        if (dx != 0) add({sgn(dx), 0}, 9);
        if (dy != 0) add({0, sgn(dy)}, 9);

        // Gentle meander.
        for (const Vec2i& d0 : dirs) add(d0, 3);

        // Favor continuing direction, avoid immediate backtrack.
        for (int i = 0; i < n; ++i) {
            if (opts[i].d.x == lastDir.x && opts[i].d.y == lastDir.y) opts[i].w += 3;
            if (opts[i].d.x == -lastDir.x && opts[i].d.y == -lastDir.y) opts[i].w = std::max(1, opts[i].w - 6);
        }

        int total = 0;
        for (int i = 0; i < n; ++i) total += opts[i].w;
        if (total <= 0) break;

        int roll = rng.range(1, total);
        Vec2i chosen{0, 0};
        for (int i = 0; i < n; ++i) {
            roll -= opts[i].w;
            if (roll <= 0) {
                chosen = opts[i].d;
                break;
            }
        }

        Vec2i nxt = clampInterior({cur.x + chosen.x, cur.y + chosen.y});
        if (!(nxt == cur)) {
            cur = nxt;
            path.push_back(cur);
            lastDir = chosen;
        }

        // If we get "stuck" due to clamping, force a move toward the goal.
        if (path.size() > 8 && path.back() == path[path.size() - 2]) {
            Vec2i force{ sgn(dx), 0 };
            if (std::abs(dx) < std::abs(dy)) force = {0, sgn(dy)};
            cur = clampInterior({cur.x + force.x, cur.y + force.y});
            path.push_back(cur);
            lastDir = force;
        }
    }

    // Hard guarantee: finish with a direct march if random walk didn't reach the goal.
    while (cur.x != goal.x) {
        cur.x += sgn(goal.x - cur.x);
        cur = clampInterior(cur);
        path.push_back(cur);
    }
    while (cur.y != goal.y) {
        cur.y += sgn(goal.y - cur.y);
        cur = clampInterior(cur);
        path.push_back(cur);
    }

    return path;
}

bool maybeCarveGlobalRavine(Dungeon& d, RNG& rng, int depth) {
    // Avoid very early floors; introduce rifts as the dungeon gets deeper.
    float p = 0.0f;
    if (depth >= 4) p = 0.18f;
    if (depth >= 7) p = 0.28f;

    const bool force = (depth == Dungeon::DEEP_MINES_DEPTH);
    if (!force && !rng.chance(p)) return false;

    std::vector<TileChange> changes;
    changes.reserve(static_cast<size_t>(d.width * 2));

    // Build a meandering line across the map.
    const auto path = buildRavinePath(d, rng);
    if (path.size() < 10) return false;

    const float widen = force ? 0.42f : 0.30f;
    const float splinter = force ? 0.10f : 0.06f;

    // Carve chasm along the path, with some sideways widening.
    for (size_t i = 0; i < path.size(); ++i) {
        const Vec2i p0 = path[i];
        forceSetTileFeature(d, p0.x, p0.y, TileType::Chasm, changes);

        Vec2i dir{0, 0};
        if (i > 0) {
            dir = { path[i].x - path[i - 1].x, path[i].y - path[i - 1].y };
            if (dir.x != 0) dir.x = (dir.x > 0) ? 1 : -1;
            if (dir.y != 0) dir.y = (dir.y > 0) ? 1 : -1;
        }

        if (dir.x != 0 || dir.y != 0) {
            Vec2i perp{ dir.y, -dir.x };
            if (rng.chance(widen)) {
                forceSetTileFeature(d, p0.x + perp.x, p0.y + perp.y, TileType::Chasm, changes);
            }
            if (rng.chance(widen * 0.40f)) {
                forceSetTileFeature(d, p0.x - perp.x, p0.y - perp.y, TileType::Chasm, changes);
            }
        }

        // Occasional splinter cracks.
        if (rng.chance(splinter)) {
            static const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};
            const Vec2i dv = dirs[rng.range(0, 3)];
            forceSetTileFeature(d, p0.x + dv.x, p0.y + dv.y, TileType::Chasm, changes);
        }
    }

    // If we barely carved anything (tiny maps), just skip.
    if (changes.size() < static_cast<size_t>(std::max(8, (d.width + d.height) / 3))) {
        undoChanges(d, changes);
        return false;
    }

    // Ensure the ravine has at least one (and sometimes two) natural stone bridges.
    int wantBridges = force ? 2 : 1;
    if (!force && depth >= 7 && rng.chance(0.35f)) wantBridges = 2;

    for (int i = 0; i < wantBridges; ++i) {
        if (!placeRavineBridge(d, rng, changes)) break;
    }

    // Repair: if we accidentally severed the critical path between stairs, add a bridge that
    // explicitly reconnects the stairs components.
    if (!stairsConnected(d)) {
        for (int tries = 0; tries < 8 && !stairsConnected(d); ++tries) {
            int compCount = 0;
            auto comp = computePassableComponents(d, compCount);
            if (compCount <= 1) break;

            auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };
            const int compUp = (d.inBounds(d.stairsUp.x, d.stairsUp.y)) ? comp[idx(d.stairsUp.x, d.stairsUp.y)] : -1;
            const int compDown = (d.inBounds(d.stairsDown.x, d.stairsDown.y)) ? comp[idx(d.stairsDown.x, d.stairsDown.y)] : -1;
            if (compUp < 0 || compDown < 0) break;
            if (compUp == compDown) break;

            // Prefer a bridge that directly connects the two relevant components.
            if (!placeRavineBridge(d, rng, changes, &comp, compUp, compDown)) {
                // Otherwise, connect compUp to *some* other component and try again.
                bool placed = false;
                for (int c = 0; c < compCount; ++c) {
                    if (c == compUp) continue;
                    if (placeRavineBridge(d, rng, changes, &comp, compUp, c)) { placed = true; break; }
                }
                if (!placed) break;
            }
        }

        if (!stairsConnected(d)) {
            // Give up: don't keep the feature if we couldn't preserve core connectivity.
            undoChanges(d, changes);
            return false;
        }
    }

    // Optional: sprinkle a few boulders near the ravine edge (deep mines especially).
    // These can be pushed into the chasm to create additional crossings.
    if (force || rng.chance(0.40f)) {
        std::vector<TileChange> bchanges;
        bchanges.reserve(16);

        const int want = force ? rng.range(3, 6) : rng.range(1, 3);
        int placed = 0;
        int attempts = want * 60;

        static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        auto adjChasm = [&](int x, int y) {
            for (auto& dv : dirs4) {
                int nx = x + dv[0];
                int ny = y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (d.at(nx, ny).type == TileType::Chasm) return true;
            }
            return false;
        };

        auto passableDeg = [&](int x, int y) {
            int c = 0;
            for (auto& dv : dirs4) {
                int nx = x + dv[0];
                int ny = y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (d.isPassable(nx, ny)) c++;
            }
            return c;
        };

        while (placed < want && attempts-- > 0) {
            const int x = rng.range(2, d.width - 3);
            const int y = rng.range(2, d.height - 3);
            if (!d.inBounds(x, y)) continue;
            if (isStairsTile(d, x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) continue;
            if (!adjChasm(x, y)) continue;
            // Avoid hard-blocking 1-wide corridors.
            if (passableDeg(x, y) <= 1) continue;
            // Keep them away from doors (doors + boulders together can feel unfair).
            if (anyDoorInRadius(d, x, y, 1)) continue;

            // Place.
            bchanges.push_back({x, y, d.at(x, y).type});
            d.at(x, y).type = TileType::Boulder;
            placed++;
        }

        if (!stairsConnected(d)) {
            // Don't let boulders ever break the guaranteed path between stairs.
            undoChanges(d, bchanges);
        }
    }

    return true;
}

// ------------------------------------------------------------
// Cavern lake / flooded grotto feature
//
// On cavern-style floors, carve a blobby chasm "lake" using a drunkard-walk.
// If it disconnects the stairs, repair by laying a stone causeway across chasm
// tiles using BFS. This creates a distinct tactical texture vs. the linear ravine.
// ------------------------------------------------------------

bool placeChasmCauseway(Dungeon& d, RNG& rng, std::vector<TileChange>& changes,
                        const std::vector<int>& comp, int compA, int compB, int maxLen) {
    const int W = d.width;
    const int H = d.height;
    if (W <= 0 || H <= 0) return false;

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    std::vector<Vec2i> starts;
    starts.reserve(static_cast<size_t>((W * H) / 16));

    std::vector<uint8_t> isGoal(static_cast<size_t>(W * H), uint8_t{0});

    static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (d.at(x, y).type != TileType::Chasm) continue;
            if (nearStairs(d, x, y, 2)) continue;

            bool adjA = false;
            bool adjB = false;

            for (auto& dv : dirs4) {
                const int nx = x + dv[0];
                const int ny = y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (!d.isPassable(nx, ny)) continue;
                const size_t ii = idx(nx, ny);
                if (ii >= comp.size()) continue;
                const int c = comp[ii];
                if (c == compA) adjA = true;
                else if (c == compB) adjB = true;
            }

            if (adjA) starts.push_back({x, y});
            if (adjB) isGoal[idx(x, y)] = 1;
        }
    }

    if (starts.empty()) return false;

    bool anyGoal = false;
    for (uint8_t v : isGoal) {
        if (v) { anyGoal = true; break; }
    }
    if (!anyGoal) return false;

    // Shuffle starts for variety.
    for (int i = static_cast<int>(starts.size()) - 1; i > 0; --i) {
        int j = rng.range(0, i);
        std::swap(starts[static_cast<size_t>(i)], starts[static_cast<size_t>(j)]);
    }

    // Randomized neighbor order.
    int order[4] = {0, 1, 2, 3};
    for (int i = 3; i > 0; --i) {
        int j = rng.range(0, i);
        std::swap(order[i], order[j]);
    }

    std::deque<Vec2i> q;
    std::vector<int> parent(static_cast<size_t>(W * H), -1);

    for (const Vec2i& s : starts) {
        const size_t si = idx(s.x, s.y);
        if (si >= parent.size()) continue;
        if (parent[si] != -1) continue;
        parent[si] = static_cast<int>(si); // root
        q.push_back(s);
    }

    int found = -1;
    while (!q.empty()) {
        Vec2i p0 = q.front();
        q.pop_front();
        const size_t pi = idx(p0.x, p0.y);
        if (pi >= parent.size()) continue;

        if (isGoal[pi]) {
            found = static_cast<int>(pi);
            break;
        }

        for (int k = 0; k < 4; ++k) {
            const auto& dv = dirs4[order[k]];
            const int nx = p0.x + dv[0];
            const int ny = p0.y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            const size_t ni = idx(nx, ny);
            if (ni >= parent.size()) continue;
            if (parent[ni] != -1) continue;
            if (d.at(nx, ny).type != TileType::Chasm) continue;
            if (nearStairs(d, nx, ny, 1)) continue;

            parent[ni] = static_cast<int>(pi);
            q.push_back({nx, ny});
        }
    }

    if (found < 0) return false;

    // Reconstruct path (chasm indices) from found back to a root.
    std::vector<int> path;
    path.reserve(64);

    int cur = found;
    for (int guard = 0; guard < W * H; ++guard) {
        path.push_back(cur);
        int pr = parent[static_cast<size_t>(cur)];
        if (pr == cur) break;
        cur = pr;
    }

    if (path.empty()) return false;
    if (maxLen > 0 && static_cast<int>(path.size()) > maxLen) return false;

    for (int lin : path) {
        const int x = lin % W;
        const int y = lin / W;
        forceSetTileFeature(d, x, y, TileType::Floor, changes);
    }

    return true;
}

bool maybeCarveCavernLake(Dungeon& d, RNG& rng, int depth, bool isCavernLevel) {
    d.hasCavernLake = false;
    if (!isCavernLevel) return false;

    const bool force = (depth == Dungeon::GROTTO_DEPTH);
    const float p = force ? 1.0f : 0.45f;
    if (!force && !rng.chance(p)) return false;

    const int W = d.width;
    const int H = d.height;
    const int area = W * H;
    if (W < 12 || H < 10) return false;

    auto countChasms = [&]() {
        int c = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (d.at(x, y).type == TileType::Chasm) c++;
            }
        }
        return c;
    };

    // Target size scales with map area, but stays modest on tiny maps.
    int baseTarget = std::clamp(area / 28, 18, 320);

    // If a ravine already exists, don't overdo the chasm density.
    const int existing = countChasms();
    if (existing > 0) {
        baseTarget = std::max(12, baseTarget - existing / 2);
    }

    // Pick a lake center far from stairs.
    auto pickCenter = [&]() -> Vec2i {
        const int minDim = std::min(W, H);
        const int minDist = std::clamp(minDim / 3, 4, 9);

        Vec2i best{-1, -1};
        int bestScore = -999999;

        for (int tries = 0; tries < 220; ++tries) {
            Vec2i p0 = d.randomFloor(rng, true);
            if (!d.inBounds(p0.x, p0.y)) continue;
            if (nearStairs(d, p0.x, p0.y, minDist)) continue;

            const int du = std::abs(p0.x - d.stairsUp.x) + std::abs(p0.y - d.stairsUp.y);
            const int dd = std::abs(p0.x - d.stairsDown.x) + std::abs(p0.y - d.stairsDown.y);
            const int score = du + dd + rng.range(-5, 5);

            if (score > bestScore) {
                bestScore = score;
                best = p0;
            }
        }

        if (best.x != -1) return best;

        // Fallback: try the center.
        Vec2i c{W / 2, H / 2};
        if (d.inBounds(c.x, c.y) && d.at(c.x, c.y).type == TileType::Floor && !nearStairs(d, c.x, c.y, 4)) return c;

        // Last resort: any floor.
        return d.randomFloor(rng, true);
    };

    // Try a couple different lake sizes before giving up (rare on very small maps).
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int target = std::max(10, baseTarget / (attempt + 1));
        const int maxSteps = std::max(80, target * 28);

        std::vector<TileChange> changes;
        changes.reserve(static_cast<size_t>(target * 2 + 32));

        const Vec2i center = pickCenter();
        if (!d.inBounds(center.x, center.y)) continue;

        Vec2i cur = center;
        Vec2i lastDir{0, 0};

        std::vector<Vec2i> anchors;
        anchors.reserve(static_cast<size_t>(std::max(64, target * 2)));
        anchors.push_back(cur);

        auto tryChasm = [&](int x, int y) -> bool {
            if (!d.inBounds(x, y)) return false;
            if (x <= 1 || y <= 1 || x >= W - 2 || y >= H - 2) return false;
            if (nearStairs(d, x, y, 3)) return false;
            if (isDoorTileType(d.at(x, y).type)) return false;
            if (d.at(x, y).type == TileType::Chasm) return false;

            // Only flood existing floors; don't destroy walls.
            if (d.at(x, y).type != TileType::Floor) return false;

            forceSetTileFeature(d, x, y, TileType::Chasm, changes);
            anchors.push_back({x, y});
            return true;
        };

        int carved = 0;
        static const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

        for (int step = 0; step < maxSteps && carved < target; ++step) {
            // Occasionally jump to an existing carved point to thicken the blob.
            if (!anchors.empty() && rng.chance(0.22f)) {
                cur = anchors[static_cast<size_t>(rng.range(0, static_cast<int>(anchors.size()) - 1))];
            }

            if (tryChasm(cur.x, cur.y)) carved++;

            // Widen around the current point.
            if (rng.chance(0.45f)) {
                const Vec2i dv = dirs[rng.range(0, 3)];
                if (tryChasm(cur.x + dv.x, cur.y + dv.y)) carved++;
            }
            if (rng.chance(0.18f)) {
                const Vec2i dv = dirs[rng.range(0, 3)];
                if (tryChasm(cur.x + dv.x, cur.y + dv.y)) carved++;
            }

            // Pick a movement direction (inertia + bias to stay near center).
            int order[4] = {0, 1, 2, 3};
            for (int i = 3; i > 0; --i) {
                int j = rng.range(0, i);
                std::swap(order[i], order[j]);
            }

            const int curDist = std::abs(cur.x - center.x) + std::abs(cur.y - center.y);

            bool moved = false;
            for (int k = 0; k < 4; ++k) {
                Vec2i dv = dirs[order[k]];
                // Avoid immediate backtrack most of the time.
                if ((dv.x == -lastDir.x && dv.y == -lastDir.y) && rng.chance(0.75f)) continue;

                Vec2i nxt{cur.x + dv.x, cur.y + dv.y};
                if (nxt.x <= 1 || nxt.y <= 1 || nxt.x >= W - 2 || nxt.y >= H - 2) continue;

                const int nd = std::abs(nxt.x - center.x) + std::abs(nxt.y - center.y);
                // Bias toward staying near the center of the lake.
                if (nd > curDist + 3 && rng.chance(0.70f)) continue;

                cur = nxt;
                lastDir = dv;
                moved = true;
                break;
            }

            if (!moved && !anchors.empty()) {
                cur = anchors[static_cast<size_t>(rng.range(0, static_cast<int>(anchors.size()) - 1))];
                lastDir = {0, 0};
            }
        }

        if (carved < std::max(8, target / 3)) {
            undoChanges(d, changes);
            continue;
        }

        // Cleanup: remove lonely speckles for a more lake-like silhouette.
        std::vector<Vec2i> toFill;
        toFill.reserve(64);

        auto chasmNeighbors8 = [&](int x, int y) {
            int c = 0;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    int nx = x + ox, ny = y + oy;
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type == TileType::Chasm) c++;
                }
            }
            return c;
        };

        for (int y = 1; y < H - 1; ++y) {
            for (int x = 1; x < W - 1; ++x) {
                if (d.at(x, y).type != TileType::Chasm) continue;
                if (nearStairs(d, x, y, 2)) continue;
                if (chasmNeighbors8(x, y) <= 1 && rng.chance(0.85f)) {
                    toFill.push_back({x, y});
                }
            }
        }
        for (const Vec2i& p0 : toFill) {
            forceSetTileFeature(d, p0.x, p0.y, TileType::Floor, changes);
        }

        // Repair connectivity if the lake severed stairs.
        if (!stairsConnected(d)) {
            for (int tries = 0; tries < 10 && !stairsConnected(d); ++tries) {
                int compCount = 0;
                auto comp = computePassableComponents(d, compCount);
                if (compCount <= 1) break;

                auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };
                const int compUp = (d.inBounds(d.stairsUp.x, d.stairsUp.y)) ? comp[idx2(d.stairsUp.x, d.stairsUp.y)] : -1;
                const int compDown = (d.inBounds(d.stairsDown.x, d.stairsDown.y)) ? comp[idx2(d.stairsDown.x, d.stairsDown.y)] : -1;
                if (compUp < 0 || compDown < 0) break;
                if (compUp == compDown) break;

                const int maxLen = std::clamp(target / 2 + 12, 18, 80);

                bool placed = placeChasmCauseway(d, rng, changes, comp, compUp, compDown, maxLen);
                if (!placed) {
                    // Fall back to a single-tile bridge if there's a narrow pinch point.
                    placed = placeRavineBridge(d, rng, changes, &comp, compUp, compDown);
                }
                if (!placed) {
                    // Connect the stairs component to any other component and try again.
                    for (int c = 0; c < compCount; ++c) {
                        if (c == compUp) continue;
                        if (placeChasmCauseway(d, rng, changes, comp, compUp, c, maxLen)) { placed = true; break; }
                    }
                }

                if (!placed) break;
            }

            if (!stairsConnected(d)) {
                undoChanges(d, changes);
                continue;
            }
        }

        // Optional: sprinkle a few boulders near the lake edge as "spare bridges".
        if (rng.chance(0.55f)) {
            std::vector<TileChange> bchanges;
            bchanges.reserve(16);

            const int want = std::clamp(area / 600, 2, 5);
            int placed = 0;
            int attempts = want * 80;

            static const int dirs4b[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
            auto adjChasm = [&](int x, int y) {
                for (auto& dv : dirs4b) {
                    int nx = x + dv[0];
                    int ny = y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type == TileType::Chasm) return true;
                }
                return false;
            };

            auto passableDeg = [&](int x, int y) {
                int c = 0;
                for (auto& dv : dirs4b) {
                    int nx = x + dv[0];
                    int ny = y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.isPassable(nx, ny)) c++;
                }
                return c;
            };

            while (placed < want && attempts-- > 0) {
                const int x = rng.range(2, W - 3);
                const int y = rng.range(2, H - 3);
                if (!d.inBounds(x, y)) continue;
                if (isStairsTile(d, x, y)) continue;
                if (nearStairs(d, x, y, 2)) continue;
                if (d.at(x, y).type != TileType::Floor) continue;
                if (!adjChasm(x, y)) continue;

                // Avoid hard-blocking 1-wide corridors.
                if (passableDeg(x, y) <= 1) continue;
                if (anyDoorInRadius(d, x, y, 1)) continue;

                bchanges.push_back({x, y, d.at(x, y).type});
                d.at(x, y).type = TileType::Boulder;
                placed++;
            }

            if (!stairsConnected(d)) {
                undoChanges(d, bchanges);
            } else {
                changes.insert(changes.end(), bchanges.begin(), bchanges.end());
            }
        }

        d.hasCavernLake = true;
        return true;
    }

    return false;
}




struct EndlessStratumInfo {
    int index = -1;
    int startDepth = -1;
    int len = 0;
    int local = 0;
    EndlessStratumTheme theme = EndlessStratumTheme::Ruins;
    uint32_t seed = 0u;
};

static EndlessStratumTheme endlessThemeForIndex(uint32_t worldSeed, int idx) {
    // Deterministically pick a "macro theme" for an endless stratum band.
    // Ensures adjacent strata don't repeat the same theme (avoids long samey stretches).
    const uint32_t base = hashCombine(worldSeed, 0xA11B1A0Du);
    const uint32_t v = hash32(base ^ static_cast<uint32_t>(idx));
    constexpr int kCount = 6;
    int t = static_cast<int>(v % static_cast<uint32_t>(kCount));

    if (idx > 0) {
        const uint32_t pv = hash32(base ^ static_cast<uint32_t>(idx - 1));
        const int prev = static_cast<int>(pv % static_cast<uint32_t>(kCount));
        if (t == prev) {
            // Deterministically pick a different theme using higher bits.
            const int bump = 1 + static_cast<int>((v / static_cast<uint32_t>(kCount)) % static_cast<uint32_t>(kCount - 1));
            t = (t + bump) % kCount;
        }
    }

    return static_cast<EndlessStratumTheme>(t);
}

static int endlessStratumLengthForIndex(uint32_t worldSeed, int idx) {
    // 5..9 floors per stratum, run-seed dependent but stable.
    const uint32_t base = hashCombine(worldSeed, 0x57A7A11Eu);
    return 5 + static_cast<int>(hash32(base ^ static_cast<uint32_t>(idx)) % 5u);
}

static EndlessStratumInfo computeEndlessStratum(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    EndlessStratumInfo out;
    if (worldSeed == 0u) return out;
    if (branch != DungeonBranch::Main) return out;
    if (depth <= maxDepth) return out;

    const int endlessStart = maxDepth + 1;
    const int endlessD = depth - endlessStart;
    if (endlessD < 0) return out;

    int idx = 0;
    int cursor = 0;
    int d = endlessD;

    for (;;) {
        const int len = endlessStratumLengthForIndex(worldSeed, idx);
        if (d < len) {
            out.index = idx;
            out.len = len;
            out.local = d;
            out.startDepth = endlessStart + cursor;
            out.theme = endlessThemeForIndex(worldSeed, idx);

            out.seed = hashCombine(hashCombine(worldSeed, 0x517A7A5Eu), static_cast<uint32_t>(idx));
            if (out.seed == 0u) out.seed = 1u;
            return out;
        }

        d -= len;
        cursor += len;
        idx += 1;

        // Safety guard for absurd depths (should never trigger in normal play).
        if (idx > 200000) {
            out.index = idx;
            out.len = len;
            out.local = 0;
            out.startDepth = endlessStart + cursor;
            out.theme = endlessThemeForIndex(worldSeed, idx);
            out.seed = hashCombine(hashCombine(worldSeed, 0x517A7A5Eu), static_cast<uint32_t>(idx));
            if (out.seed == 0u) out.seed = 1u;
            return out;
        }
    }
}


// ------------------------------------------------------------
// Campaign Strata (finite 1..maxDepth macro theming)
//
// The Infinite World uses endless, run-seeded strata to keep deep floors from
// feeling like uncorrelated noise. The fixed-length main campaign benefits from
// the same idea: group the 1..maxDepth arc into a handful of themed bands.
//
// This is intentionally lightweight and deterministic: it never needs saving.
// ------------------------------------------------------------

static int campaignStratumCountForMaxDepth(int maxDepth) {
    if (maxDepth <= 1) return 1;

    // Aim for ~5 floors per stratum; clamp so the arc stays readable.
    int k = (maxDepth + 4) / 5;
    k = std::clamp(k, 2, 7);

    // Very short campaigns shouldn't explode into many strata.
    if (maxDepth <= 8) k = std::min(k, 2);

    // Never exceed maxDepth (avoids degenerate 1-tile strata in tiny test runs).
    k = std::min(k, maxDepth);
    return std::max(1, k);
}

static EndlessStratumTheme campaignThemeForIndex(uint32_t worldSeed, int idx, int count, EndlessStratumTheme prev) {
    if (idx <= 0) return EndlessStratumTheme::Ruins;

    // Progress 0..1 across the campaign strata.
    const float u = (count > 1)
        ? (static_cast<float>(idx) / static_cast<float>(count - 1))
        : 0.0f;

    // Deterministic hash per (run, stratumIdx).
    const uint32_t base = hashCombine(worldSeed, 0xC0A11E5Eu);
    const uint32_t v = hash32(base ^ static_cast<uint32_t>(idx));

    // Stage weights (coarse and readable).
    // Order: Ruins, Caverns, Labyrinth, Warrens, Mines, Catacombs.
    int w[6] = {0, 0, 0, 0, 0, 0};

    if (u < 0.34f) {
        w[0] = 7;  // Ruins (onboarding: readable rooms)
        w[1] = 2;  // Caverns
        w[2] = 1;  // Labyrinth
        w[3] = 3;  // Warrens
        w[4] = 4;  // Mines
        w[5] = 1;  // Catacombs
    } else if (u < 0.67f) {
        w[0] = 2;
        w[1] = 4;
        w[2] = 2;
        w[3] = 3;
        w[4] = 3;
        w[5] = 4;
    } else {
        w[0] = 1;
        w[1] = 4;
        w[2] = 5;
        w[3] = 2;
        w[4] = 2;
        w[5] = 6;  // Catacombs (late game identity)
    }

    int total = 0;
    for (int i = 0; i < 6; ++i) total += std::max(0, w[i]);
    if (total <= 0) return EndlessStratumTheme::Ruins;

    int r = static_cast<int>(v % static_cast<uint32_t>(total));
    int t = 0;
    for (int i = 0; i < 6; ++i) {
        r -= std::max(0, w[i]);
        if (r < 0) { t = i; break; }
    }

    if (t == static_cast<int>(prev)) {
        // Deterministically pick a different theme using higher bits.
        const int bump = 1 + static_cast<int>((v / static_cast<uint32_t>(total)) % 5u);
        t = (t + bump) % 6;
        if (t == static_cast<int>(prev)) t = (t + 1) % 6;
    }

    return static_cast<EndlessStratumTheme>(t);
}

static EndlessStratumInfo computeCampaignStratum(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    EndlessStratumInfo out;
    if (worldSeed == 0u) return out;
    if (branch != DungeonBranch::Main) return out;
    if (depth <= 0 || depth > maxDepth) return out;

    const int count = campaignStratumCountForMaxDepth(maxDepth);
    if (count <= 0) return out;

    // Evenly split maxDepth into `count` bands (difference at most 1).
    const int baseLen = std::max(1, maxDepth / count);
    const int rem = std::max(0, maxDepth - baseLen * count); // 0..count-1

    int start = 1;
    EndlessStratumTheme prev = EndlessStratumTheme::Ruins;

    for (int idx = 0; idx < count; ++idx) {
        const int len = baseLen + ((idx < rem) ? 1 : 0);

        const EndlessStratumTheme theme =
            (idx == 0) ? EndlessStratumTheme::Ruins
                       : campaignThemeForIndex(worldSeed, idx, count, prev);

        if (depth >= start && depth < start + len) {
            out.index = idx;
            out.startDepth = start;
            out.len = len;
            out.local = depth - start;
            out.theme = theme;

            out.seed = hash32(hashCombine(hashCombine(worldSeed, 0xD1A6E5EDu), static_cast<uint32_t>(idx)));
            if (out.seed == 0u) out.seed = 1u;
            return out;
        }

        prev = theme;
        start += len;
        if (start > maxDepth + 1) break;
    }

    return out;
}

static EndlessStratumInfo computeRunStratum(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    // For the Main branch we use:
    //   - campaign strata for depth 1..maxDepth
    //   - endless strata for depth > maxDepth (Infinite World)
    if (depth > maxDepth) {
        return computeEndlessStratum(worldSeed, branch, depth, maxDepth);
    }
    return computeCampaignStratum(worldSeed, branch, depth, maxDepth);
}



// ------------------------------------------------------------
// Endless Rift (Infinite World macro terrain continuity)
//
// Infinite World turns the dungeon into an effectively unbounded descent.
// Pure per-floor randomness can feel "episodic": each depth is interesting, but
// the world doesn't develop recognizable large-scale structure.
//
// This feature adds a run-seeded, stratum-aligned "fault line" that drifts
// smoothly across endless depths. It's essentially a ravine-like chasm seam,
// but driven by macro parameters (stratum knots) instead of independent RNG.
// The result: you occasionally experience long stretches of related terrain,
// and the rift subtly migrates as you go deeper (geological continuity).
//
// Design constraints:
// - Never overwrite stairs or doors.
// - Always preserve stairs connectivity (rollback if we can't).
// - Keep deterministic behavior per run/depth (seeded from worldSeed + depth).
// - Keep it exclusive to endless depths (depth > maxDepth) so the main run's
//   fixed "arc" stays readable.
// ------------------------------------------------------------

struct EndlessRiftKnot {
    // Normalized edge offsets in [0,1] (mapped to inner map coordinates).
    float uStart = 0.5f;
    float uEnd = 0.5f;
    float uCtrl = 0.5f;

    // 0..1 "roughness" controlling widening/splinters.
    float rough = 0.5f;

    // 0..1 "width driver" (mapped to 1..3 tile thickness).
    float width = 0.5f;

    bool active = false;
    uint32_t seed = 0u;
};

static float hash01u(uint32_t x) {
    // Stable float in [0,1) from a hash.
    return (hash32(x) / (static_cast<float>(std::numeric_limits<uint32_t>::max()) + 1.0f));
}

static float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static float riftBaseProbForTheme(EndlessStratumTheme theme) {
    switch (theme) {
        case EndlessStratumTheme::Caverns:   return 0.68f;
        case EndlessStratumTheme::Catacombs: return 0.62f;
        case EndlessStratumTheme::Mines:     return 0.56f;
        case EndlessStratumTheme::Warrens:   return 0.46f;
        case EndlessStratumTheme::Ruins:     return 0.34f;
        case EndlessStratumTheme::Labyrinth: return 0.24f;
        default:                              return 0.40f;
    }
}

static EndlessRiftKnot computeEndlessRiftKnot(uint32_t worldSeed, int idx) {
    EndlessRiftKnot k;
    if (worldSeed == 0u || idx < 0) return k;

    // Knot seed is per (run, stratumIndex).
    const uint32_t base = hashCombine(worldSeed, 0xC4B17F23u);
    const uint32_t s = hash32(base ^ static_cast<uint32_t>(idx));
    k.seed = s;

    auto h = [&](uint32_t salt) -> float {
        return hash01u(hashCombine(s, salt));
    };

    // Keep offsets away from extreme edges for readability.
    k.uStart = 0.14f + 0.72f * h(0x10u);
    k.uEnd   = 0.14f + 0.72f * h(0x11u);

    const float mid = 0.5f * (k.uStart + k.uEnd);
    const float bend = (h(0x12u) * 2.0f - 1.0f) * 0.26f; // [-0.26, +0.26]
    k.uCtrl = std::clamp(mid + bend, 0.08f, 0.92f);

    k.rough = h(0x13u);
    k.width = h(0x14u);

    // Theme-aware presence.
    const EndlessStratumTheme theme = endlessThemeForIndex(worldSeed, idx);
    float p = riftBaseProbForTheme(theme);

    // Very deep strata slightly increase the chance so long endless runs don't feel too uniform.
    // Clamp so we don't devolve into "always a rift".
    p += std::min(0.22f, 0.0045f * static_cast<float>(std::min(idx, 80)));
    p = std::clamp(p, 0.12f, 0.88f);

    k.active = (h(0x15u) < p);
    return k;
}

static std::vector<Vec2i> buildEndlessRiftBezierPath(const Dungeon& d, bool horizontal,
                                                     float uStart, float uCtrl, float uEnd) {
    const int W = d.width;
    const int H = d.height;
    std::vector<Vec2i> path;
    if (W < 6 || H < 6) return path;

    auto clampInteriorX = [&](int x) { return clampi(x, 1, W - 2); };
    auto clampInteriorY = [&](int y) { return clampi(y, 1, H - 2); };

    auto xFromU = [&](float u) -> int {
        const int span = std::max(1, W - 3);
        const int x = 1 + static_cast<int>(std::lround(std::clamp(u, 0.0f, 1.0f) * static_cast<float>(span)));
        return clampInteriorX(x);
    };
    auto yFromU = [&](float u) -> int {
        const int span = std::max(1, H - 3);
        const int y = 1 + static_cast<int>(std::lround(std::clamp(u, 0.0f, 1.0f) * static_cast<float>(span)));
        return clampInteriorY(y);
    };

    Vec2i P0{0, 0}, P1{0, 0}, P2{0, 0};
    if (horizontal) {
        P0 = {1, yFromU(uStart)};
        P2 = {W - 2, yFromU(uEnd)};
        P1 = {W / 2, yFromU(uCtrl)};
    } else {
        P0 = {xFromU(uStart), 1};
        P2 = {xFromU(uEnd), H - 2};
        P1 = {xFromU(uCtrl), H / 2};
    }

    const int steps = std::max(48, std::max(W, H) * 3);
    path.reserve(static_cast<size_t>(steps + 1));

    auto bez2 = [](float a, float b, float c, float t) {
        const float it = 1.0f - t;
        return it * it * a + 2.0f * it * t * b + t * t * c;
    };

    Vec2i last{-9999, -9999};

    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float xf = bez2(static_cast<float>(P0.x), static_cast<float>(P1.x), static_cast<float>(P2.x), t);
        const float yf = bez2(static_cast<float>(P0.y), static_cast<float>(P1.y), static_cast<float>(P2.y), t);
        const int x = clampInteriorX(static_cast<int>(std::lround(xf)));
        const int y = clampInteriorY(static_cast<int>(std::lround(yf)));

        Vec2i p{x, y};
        if (!(p == last)) {
            path.push_back(p);
            last = p;
        }
    }

    return path;
}

static bool maybeCarveEndlessRift(Dungeon& d, int depth, int maxDepth, uint32_t worldSeed, const EndlessStratumInfo& st) {
    d.endlessRiftActive = false;
    d.endlessRiftIntensityPct = 0;
    d.endlessRiftChasmCount = 0;
    d.endlessRiftBridgeCount = 0;
    d.endlessRiftBoulderCount = 0;
    d.endlessRiftSeed = 0u;

    if (worldSeed == 0u) return false;
    if (depth <= maxDepth) return false;
    if (st.index < 0 || st.len <= 0) return false;

    // Skip tiny maps (rift becomes too dominant).
    if (d.width < 22 || d.height < 16) return false;

    const float t = (st.len > 1) ? (static_cast<float>(st.local) / static_cast<float>(st.len - 1)) : 0.0f;
    const float s = smoothstep01(t);

    const EndlessRiftKnot a = computeEndlessRiftKnot(worldSeed, st.index);
    const EndlessRiftKnot b = computeEndlessRiftKnot(worldSeed, st.index + 1);

    const float ia = a.active ? 1.0f : 0.0f;
    const float ib = b.active ? 1.0f : 0.0f;
    float intensity = lerpf(ia, ib, s);

    // Current stratum theme can modulate intensity slightly.
    // (Keeps labyrinth bands from getting "over-chasmed".)
    const EndlessStratumTheme theme = st.theme;
    if (theme == EndlessStratumTheme::Labyrinth) intensity *= 0.70f;
    if (theme == EndlessStratumTheme::Caverns) intensity *= 1.12f;
    intensity = std::clamp(intensity, 0.0f, 1.0f);

    d.endlessRiftIntensityPct = static_cast<int>(std::lround(intensity * 100.0f));

    if (intensity < 0.18f) return false;

    const float u0 = std::clamp(lerpf(a.uStart, b.uStart, s), 0.0f, 1.0f);
    const float u1 = std::clamp(lerpf(a.uEnd, b.uEnd, s), 0.0f, 1.0f);
    const float uc = std::clamp(lerpf(a.uCtrl, b.uCtrl, s), 0.0f, 1.0f);
    const float rough = std::clamp(lerpf(a.rough, b.rough, s), 0.0f, 1.0f);
    const float widthDriver = std::clamp(lerpf(a.width, b.width, s), 0.0f, 1.0f);

    // Run+depth deterministic seed, independent of candidate-generation RNG.
    uint32_t rSeed = hashCombine(hashCombine(worldSeed, 0x52F7D11Eu), static_cast<uint32_t>(depth));
    if (rSeed == 0u) rSeed = 1u;
    d.endlessRiftSeed = rSeed;

    RNG rr(rSeed);

    // Global orientation is per-run (so the rift "feels like a world feature").
    const bool horizontal = ((hash32(hashCombine(worldSeed, 0x9E3B1C4Au)) & 1u) != 0u);

    const auto path = buildEndlessRiftBezierPath(d, horizontal, u0, uc, u1);
    if (path.size() < 10) return false;

    std::vector<TileChange> changes;
    changes.reserve(path.size() * 2);

    // Width/thickness mapping: 1..3 tiles.
    int thick = 1 + static_cast<int>(std::lround(widthDriver * 1.6f));
    if (theme == EndlessStratumTheme::Caverns && intensity > 0.55f) thick += 1;
    thick = std::clamp(thick, 1, 3);

    float widen = 0.20f + 0.32f * intensity;
    float splinter = 0.02f + 0.08f * intensity * (0.4f + 0.6f * rough);

    if (theme == EndlessStratumTheme::Caverns) { widen *= 1.10f; splinter *= 1.35f; }
    if (theme == EndlessStratumTheme::Mines)   { widen *= 0.95f; splinter *= 0.75f; }
    if (theme == EndlessStratumTheme::Labyrinth) { widen *= 0.90f; splinter *= 0.70f; }

    widen = std::clamp(widen, 0.05f, 0.70f);
    splinter = std::clamp(splinter, 0.0f, 0.18f);

    int carved = 0;

    auto carveChasmAt = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        if (nearStairs(d, x, y, 4)) return;
        TileType before = d.at(x, y).type;
        forceSetTileFeature(d, x, y, TileType::Chasm, changes);
        if (before != TileType::Chasm && d.at(x, y).type == TileType::Chasm) carved += 1;
    };

    for (size_t i = 0; i < path.size(); ++i) {
        const Vec2i p0 = path[i];
        carveChasmAt(p0.x, p0.y);

        // Thicken via a small Manhattan disk around the centerline.
        if (thick >= 2) {
            carveChasmAt(p0.x + 1, p0.y);
            carveChasmAt(p0.x - 1, p0.y);
            carveChasmAt(p0.x, p0.y + 1);
            carveChasmAt(p0.x, p0.y - 1);
        }
        if (thick >= 3) {
            // Diamond corners.
            carveChasmAt(p0.x + 1, p0.y + 1);
            carveChasmAt(p0.x - 1, p0.y + 1);
            carveChasmAt(p0.x + 1, p0.y - 1);
            carveChasmAt(p0.x - 1, p0.y - 1);
        }

        Vec2i dir{0, 0};
        if (i > 0) {
            dir = { path[i].x - path[i - 1].x, path[i].y - path[i - 1].y };
            if (dir.x != 0) dir.x = (dir.x > 0) ? 1 : -1;
            if (dir.y != 0) dir.y = (dir.y > 0) ? 1 : -1;
        }

        if (dir.x != 0 || dir.y != 0) {
            Vec2i perp{ dir.y, -dir.x };

            if (rr.chance(widen)) carveChasmAt(p0.x + perp.x, p0.y + perp.y);
            if (rr.chance(widen * 0.42f)) carveChasmAt(p0.x - perp.x, p0.y - perp.y);

            // Occasional splinter cracks.
            if (rr.chance(splinter)) {
                static const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};
                const Vec2i dv = dirs[rr.range(0, 3)];
                carveChasmAt(p0.x + dv.x, p0.y + dv.y);
            }
        }
    }

    // If we barely carved anything (degenerate maps), skip.
    if (carved < std::max(10, (d.width + d.height) / 2)) {
        undoChanges(d, changes);
        return false;
    }

    // Ensure the rift has at least one (and sometimes two) natural stone bridges.
    int wantBridges = 1;
    if (theme == EndlessStratumTheme::Mines || theme == EndlessStratumTheme::Catacombs) wantBridges = 2;
    if (intensity > 0.85f && rr.chance(0.33f)) wantBridges += 1;
    wantBridges = std::clamp(wantBridges, 1, 3);

    int bridges = 0;
    for (int i = 0; i < wantBridges; ++i) {
        if (placeRavineBridge(d, rr, changes)) bridges += 1;
        else break;
    }

    // If we accidentally severed the critical path between stairs, add bridges that
    // explicitly reconnect stairs components (or rollback if we cannot).
    if (!stairsConnected(d)) {
        for (int tries = 0; tries < 10 && !stairsConnected(d); ++tries) {
            int compCount = 0;
            auto comp = computePassableComponents(d, compCount);
            if (compCount <= 1) break;

            auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };
            const int compUp = (d.inBounds(d.stairsUp.x, d.stairsUp.y)) ? comp[idx(d.stairsUp.x, d.stairsUp.y)] : -1;
            const int compDown = (d.inBounds(d.stairsDown.x, d.stairsDown.y)) ? comp[idx(d.stairsDown.x, d.stairsDown.y)] : -1;
            if (compUp < 0 || compDown < 0) break;
            if (compUp == compDown) break;

            if (placeRavineBridge(d, rr, changes, &comp, compUp, compDown)) {
                bridges += 1;
            } else {
                // Otherwise connect compUp to some other component and try again.
                bool placed = false;
                for (int c = 0; c < compCount; ++c) {
                    if (c == compUp) continue;
                    if (placeRavineBridge(d, rr, changes, &comp, compUp, c)) { placed = true; bridges += 1; break; }
                }
                if (!placed) break;
            }
        }

        if (!stairsConnected(d)) {
            undoChanges(d, changes);
            return false;
        }
    }

    // Optional: sprinkle a few boulders near the rift edge (mines especially).
    // These can be pushed into the chasm to create additional crossings.
    int bouldersPlaced = 0;
    {
        float pb = 0.28f + 0.30f * intensity;
        if (theme == EndlessStratumTheme::Mines) pb *= 1.25f;
        if (theme == EndlessStratumTheme::Labyrinth) pb *= 0.70f;

        if (rr.chance(std::clamp(pb, 0.0f, 0.85f))) {
            std::vector<TileChange> bchanges;
            bchanges.reserve(24);

            const int want = (theme == EndlessStratumTheme::Mines) ? rr.range(2, 5) : rr.range(1, 3);
            int placed = 0;
            int attempts = want * 80;

            static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

            auto adjChasm = [&](int x, int y) {
                for (auto& dv : dirs4) {
                    const int nx = x + dv[0];
                    const int ny = y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type == TileType::Chasm) return true;
                }
                return false;
            };

            auto passableDeg = [&](int x, int y) {
                int c = 0;
                for (auto& dv : dirs4) {
                    const int nx = x + dv[0];
                    const int ny = y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.isPassable(nx, ny)) c++;
                }
                return c;
            };

            while (placed < want && attempts-- > 0) {
                const int x = rr.range(2, d.width - 3);
                const int y = rr.range(2, d.height - 3);
                if (!d.inBounds(x, y)) continue;
                if (isStairsTile(d, x, y)) continue;
                if (nearStairs(d, x, y, 4)) continue;
                if (d.at(x, y).type != TileType::Floor) continue;
                if (!adjChasm(x, y)) continue;
                if (passableDeg(x, y) <= 1) continue;
                if (anyDoorInRadius(d, x, y, 1)) continue;

                bchanges.push_back({x, y, d.at(x, y).type});
                d.at(x, y).type = TileType::Boulder;
                placed++;
            }

            if (!stairsConnected(d)) {
                undoChanges(d, bchanges);
            } else {
                bouldersPlaced = placed;
            }
        }
    }

    d.endlessRiftActive = true;
    d.endlessRiftChasmCount = carved;
    d.endlessRiftBridgeCount = bridges;
    d.endlessRiftBoulderCount = bouldersPlaced;

    return true;
}



enum class GenKind : uint8_t {
    RoomsBsp = 0,
    // New: room packer + graph connectivity (MST + extra loops) for more varied "ruins" floors.
    RoomsGraph,
    Cavern,
    Maze,
    Warrens,
    Mines,
    Catacombs,
};

// ------------------------------------------------------------
// Run Faultline (finite campaign macro terrain continuity)
//
// Infinite World has endless strata + a persistent rift that drifts across depth.
// The fixed-length main campaign (depth <= maxDepth) can still feel "episodic":
// ravines/sinkholes are interesting, but they are typically single-floor RNG events.
//
// This feature adds a run-seeded "fault band" across the Main branch: a small
// contiguous range of depths (usually 3-6 floors) shares a drifting chasm seam
// with deterministic parameters. It preserves the same safety constraints as
// other global terrain features:
//   - Never overwrite doors or stairs
//   - Always preserve stairs connectivity (repair with bridges or rollback)
//   - Deterministic per run/depth (seeded from worldSeed + depth; independent
//     of candidate-generation RNG)
//
// Compared to maybeCarveGlobalRavine (single-floor), this creates cross-floor
// geological continuity without requiring infinite descent.
// ------------------------------------------------------------

struct RunFaultBandInfo {
    bool active = false;      // true if this depth is within the band
    int startDepth = -1;      // absolute depth where the band begins
    int len = 0;              // number of floors in the band
    int local = 0;            // 0..len-1 index within the band
    float intensity = 0.0f;   // 0..1 strength (peaks mid-band)
    uint32_t seed = 0u;       // stable run seed for the band
};

struct RunFaultKnot {
    float uStart = 0.5f;
    float uEnd = 0.5f;
    float uCtrl = 0.5f;
    float rough = 0.5f;
    float width = 0.5f;
    uint32_t seed = 0u;
};

static RunFaultBandInfo computeRunFaultBand(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    RunFaultBandInfo out;
    if (worldSeed == 0u) return out;
    if (branch != DungeonBranch::Main) return out;
    if (depth < 1 || depth > maxDepth) return out;

    // Keep early-game onboarding stable; the early depths already contain bespoke floors.
    const int minStart = 7;

    // Standard floor generation never runs on (maxDepth-1) and maxDepth.
    const int maxEnd = std::max(1, maxDepth - 2);
    if (maxEnd - minStart + 1 < 3) return out;

    const uint32_t base = hashCombine(worldSeed, 0xF17A17EDu);

    // Run-level chance that the campaign features a fault band at all.
    const float p = 0.62f;
    if (hash01u(hashCombine(base, 0x01u)) > p) return out;

    int len = 3 + static_cast<int>(hash32(hashCombine(base, 0x02u)) % 3u); // 3..5
    // Longer campaigns can occasionally get a slightly longer band.
    if (maxDepth >= 18 && ((hash32(hashCombine(base, 0x02u)) & 0x10u) != 0u)) len += 1;
    len = std::clamp(len, 3, 6);

    const int maxStart = maxEnd - len + 1;
    if (maxStart < minStart) return out;

    const int start = minStart + static_cast<int>(hash32(hashCombine(base, 0x03u)) % static_cast<uint32_t>(maxStart - minStart + 1));

    out.startDepth = start;
    out.len = len;

    if (depth < start || depth >= start + len) return out;

    out.active = true;
    out.local = depth - start;

    const float t = (len > 1) ? (static_cast<float>(out.local) / static_cast<float>(len - 1)) : 0.0f;

    // Triangular band profile (peaks mid-band). Keep a non-zero baseline at the edges
    // so the seam feels continuous across the whole band.
    const float tri = 1.0f - std::abs(2.0f * t - 1.0f);
    const float s = smoothstep01(tri);
    out.intensity = std::clamp(0.28f + 0.60f * s, 0.0f, 1.0f);

    out.seed = hashCombine(hashCombine(base, 0x0BADC0DEu), static_cast<uint32_t>(start));
    if (out.seed == 0u) out.seed = 1u;

    return out;
}

static RunFaultKnot computeRunFaultKnot(uint32_t worldSeed, int bandStart, int bandLen, int which) {
    RunFaultKnot k;
    if (worldSeed == 0u) return k;

    const uint32_t base = hashCombine(worldSeed, 0xA5F0C0DEu);
    const uint32_t mix = hashCombine(static_cast<uint32_t>(bandStart), static_cast<uint32_t>(bandLen));
    const uint32_t s = hash32(hashCombine(base, mix) ^ static_cast<uint32_t>(which));
    k.seed = s;

    auto h = [&](uint32_t salt) -> float {
        return hash01u(hashCombine(s, salt));
    };

    // Keep offsets away from extreme edges for readability.
    k.uStart = 0.12f + 0.76f * h(0x10u);
    k.uEnd   = 0.12f + 0.76f * h(0x11u);

    const float mid = 0.5f * (k.uStart + k.uEnd);
    const float bend = (h(0x12u) * 2.0f - 1.0f) * 0.22f; // [-0.22, +0.22]
    k.uCtrl = std::clamp(mid + bend, 0.06f, 0.94f);

    k.rough = h(0x13u);
    k.width = h(0x14u);

    return k;
}

static bool maybeCarveRunFaultline(Dungeon& d, DungeonBranch branch, int depth, int maxDepth, uint32_t worldSeed, GenKind g) {
    // Reset debug fields (per-floor).
    d.runFaultActive = false;
    d.runFaultBandStartDepth = -1;
    d.runFaultBandLen = 0;
    d.runFaultBandLocal = 0;
    d.runFaultIntensityPct = 0;
    d.runFaultChasmCount = 0;
    d.runFaultBridgeCount = 0;
    d.runFaultBoulderCount = 0;
    d.runFaultSeed = 0u;

    const RunFaultBandInfo band = computeRunFaultBand(worldSeed, branch, depth, maxDepth);
    if (!band.active) return false;

    d.runFaultBandStartDepth = band.startDepth;
    d.runFaultBandLen = band.len;
    d.runFaultBandLocal = band.local;
    d.runFaultIntensityPct = static_cast<int>(std::lround(band.intensity * 100.0f));

    // Skip tiny maps (feature becomes too dominant).
    if (d.width < 22 || d.height < 16) return false;

    const RunFaultKnot a = computeRunFaultKnot(worldSeed, band.startDepth, band.len, 0);
    const RunFaultKnot b = computeRunFaultKnot(worldSeed, band.startDepth, band.len, 1);

    const float t = (band.len > 1) ? (static_cast<float>(band.local) / static_cast<float>(band.len - 1)) : 0.0f;
    const float s = smoothstep01(t);

    const float u0 = std::clamp(lerpf(a.uStart, b.uStart, s), 0.0f, 1.0f);
    const float u1 = std::clamp(lerpf(a.uEnd,   b.uEnd,   s), 0.0f, 1.0f);
    const float uc = std::clamp(lerpf(a.uCtrl,  b.uCtrl,  s), 0.0f, 1.0f);
    const float rough = std::clamp(lerpf(a.rough, b.rough, s), 0.0f, 1.0f);
    const float widthDriver = std::clamp(lerpf(a.width, b.width, s), 0.0f, 1.0f);

    // Deterministic per-run/per-depth RNG, independent of candidate RNG.
    uint32_t rSeed = hashCombine(hashCombine(worldSeed, 0xD00DFEEDu), static_cast<uint32_t>(depth));
    rSeed = hashCombine(rSeed, static_cast<uint32_t>(band.startDepth));
    if (rSeed == 0u) rSeed = 1u;
    d.runFaultSeed = rSeed;

    RNG rr(rSeed);

    // Orientation is per-run so the faultline reads as a global world feature.
    const bool horizontal = ((hash32(hashCombine(worldSeed, 0xC001D00Du)) & 1u) != 0u);

    const auto path = buildEndlessRiftBezierPath(d, horizontal, u0, uc, u1);
    if (path.size() < 10) return false;

    std::vector<TileChange> changes;
    changes.reserve(path.size() * 2);

    float intensity = band.intensity;

    // Gen-kind modulation (keeps some layouts from feeling over-chasmed).
    if (g == GenKind::Maze) intensity *= 0.72f;
    if (g == GenKind::Warrens) intensity *= 0.92f;
    if (g == GenKind::Cavern) intensity *= 1.08f;
    if (g == GenKind::Mines) intensity *= 1.10f;
    if (g == GenKind::Catacombs) intensity *= 1.05f;
    intensity = std::clamp(intensity, 0.0f, 1.0f);

    d.runFaultIntensityPct = static_cast<int>(std::lround(intensity * 100.0f));

    // Width/thickness mapping: 1..2 (rarely 3 on peak intensity).
    int thick = 1 + static_cast<int>(std::lround(widthDriver * 1.2f));
    if (intensity > 0.82f && rr.chance(0.33f)) thick += 1;
    thick = std::clamp(thick, 1, 3);

    float widen = 0.14f + 0.28f * intensity;
    float splinter = 0.012f + 0.055f * intensity * (0.35f + 0.65f * rough);

    if (g == GenKind::Cavern) { widen *= 1.12f; splinter *= 1.20f; }
    if (g == GenKind::Mines)  { widen *= 0.98f; splinter *= 1.10f; }
    if (g == GenKind::Maze)   { widen *= 0.88f; splinter *= 0.75f; }

    widen = std::clamp(widen, 0.05f, 0.65f);
    splinter = std::clamp(splinter, 0.0f, 0.16f);

    int carved = 0;

    auto carveChasmAt = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        if (nearStairs(d, x, y, 4)) return;
        const TileType before = d.at(x, y).type;
        forceSetTileFeature(d, x, y, TileType::Chasm, changes);
        if (before != TileType::Chasm && d.at(x, y).type == TileType::Chasm) carved += 1;
    };

    for (size_t i = 0; i < path.size(); ++i) {
        const Vec2i p0 = path[i];
        carveChasmAt(p0.x, p0.y);

        // Thicken via a small Manhattan disk around the centerline.
        if (thick >= 2) {
            carveChasmAt(p0.x + 1, p0.y);
            carveChasmAt(p0.x - 1, p0.y);
            carveChasmAt(p0.x, p0.y + 1);
            carveChasmAt(p0.x, p0.y - 1);
        }
        if (thick >= 3) {
            // Diamond corners.
            carveChasmAt(p0.x + 1, p0.y + 1);
            carveChasmAt(p0.x - 1, p0.y + 1);
            carveChasmAt(p0.x + 1, p0.y - 1);
            carveChasmAt(p0.x - 1, p0.y - 1);
        }

        Vec2i dir{0, 0};
        if (i > 0) {
            dir = { path[i].x - path[i - 1].x, path[i].y - path[i - 1].y };
            if (dir.x != 0) dir.x = (dir.x > 0) ? 1 : -1;
            if (dir.y != 0) dir.y = (dir.y > 0) ? 1 : -1;
        }

        if (dir.x != 0 || dir.y != 0) {
            Vec2i perp{ dir.y, -dir.x };

            if (rr.chance(widen)) carveChasmAt(p0.x + perp.x, p0.y + perp.y);
            if (rr.chance(widen * 0.38f)) carveChasmAt(p0.x - perp.x, p0.y - perp.y);

            // Occasional splinter cracks.
            if (rr.chance(splinter)) {
                static const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};
                const Vec2i dv = dirs[rr.range(0, 3)];
                carveChasmAt(p0.x + dv.x, p0.y + dv.y);
            }
        }
    }

    // If we barely carved anything (degenerate), skip.
    if (carved < std::max(10, (d.width + d.height) / 2)) {
        undoChanges(d, changes);
        return false;
    }

    // Ensure the seam has at least one natural bridge (two on peak intensity or mines).
    int wantBridges = 1;
    if (g == GenKind::Mines || g == GenKind::Catacombs) wantBridges = 2;
    if (intensity > 0.78f && rr.chance(0.40f)) wantBridges += 1;
    wantBridges = std::clamp(wantBridges, 1, 3);

    int bridges = 0;
    for (int i = 0; i < wantBridges; ++i) {
        if (placeRavineBridge(d, rr, changes)) bridges += 1;
        else break;
    }

    // Repair: if we severed stairs connectivity, add explicit bridges or rollback.
    if (!stairsConnected(d)) {
        for (int tries = 0; tries < 10 && !stairsConnected(d); ++tries) {
            int compCount = 0;
            auto comp = computePassableComponents(d, compCount);
            if (compCount <= 1) break;

            auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };
            const int compUp = (d.inBounds(d.stairsUp.x, d.stairsUp.y)) ? comp[idx(d.stairsUp.x, d.stairsUp.y)] : -1;
            const int compDown = (d.inBounds(d.stairsDown.x, d.stairsDown.y)) ? comp[idx(d.stairsDown.x, d.stairsDown.y)] : -1;
            if (compUp < 0 || compDown < 0) break;
            if (compUp == compDown) break;

            if (placeRavineBridge(d, rr, changes, &comp, compUp, compDown)) {
                bridges += 1;
            } else {
                bool placed = false;
                for (int c = 0; c < compCount; ++c) {
                    if (c == compUp) continue;
                    if (placeRavineBridge(d, rr, changes, &comp, compUp, c)) { placed = true; bridges += 1; break; }
                }
                if (!placed) break;
            }
        }

        if (!stairsConnected(d)) {
            undoChanges(d, changes);
            return false;
        }
    }

    // Optional: boulders near the fault edge (for player-made crossings).
    int bouldersPlaced = 0;
    {
        float pb = 0.18f + 0.32f * intensity;
        if (g == GenKind::Mines) pb *= 1.20f;
        if (g == GenKind::Maze) pb *= 0.70f;

        if (rr.chance(std::clamp(pb, 0.0f, 0.80f))) {
            std::vector<TileChange> bchanges;
            bchanges.reserve(24);

            const int want = (g == GenKind::Mines) ? rr.range(2, 5) : rr.range(1, 3);
            int placed = 0;
            int attempts = want * 70;

            static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

            auto adjChasm = [&](int x, int y) {
                for (auto& dv : dirs4) {
                    const int nx = x + dv[0];
                    const int ny = y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type == TileType::Chasm) return true;
                }
                return false;
            };

            auto passableDeg = [&](int x, int y) {
                int c = 0;
                for (auto& dv : dirs4) {
                    const int nx = x + dv[0];
                    const int ny = y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.isPassable(nx, ny)) c++;
                }
                return c;
            };

            while (placed < want && attempts-- > 0) {
                const int x = rr.range(2, d.width - 3);
                const int y = rr.range(2, d.height - 3);
                if (!d.inBounds(x, y)) continue;
                if (isStairsTile(d, x, y)) continue;
                if (nearStairs(d, x, y, 4)) continue;
                if (d.at(x, y).type != TileType::Floor) continue;
                if (!adjChasm(x, y)) continue;
                if (passableDeg(x, y) <= 1) continue;
                if (anyDoorInRadius(d, x, y, 1)) continue;

                bchanges.push_back({x, y, d.at(x, y).type});
                d.at(x, y).type = TileType::Boulder;
                placed++;
            }

            if (!stairsConnected(d)) {
                undoChanges(d, bchanges);
            } else {
                bouldersPlaced = placed;
            }
        }
    }

    d.runFaultActive = true;
    d.runFaultChasmCount = carved;
    d.runFaultBridgeCount = bridges;
    d.runFaultBoulderCount = bouldersPlaced;

    return true;
}


GenKind chooseGenKind(DungeonBranch branch, int depth, int maxDepth, uint32_t worldSeed, RNG& rng) {
    // The default run now spans ~20 floors, so we pace variety in two arcs:
    // - Early: classic rooms (with occasional "ruins" variant)
    // - Early spikes: mines + grotto + an early maze/warrens band
    // - Midpoint: a bigger "you are deep now" spike
	// - Late: a second set of themed generator hits (lower mines/catacombs/cavern)
	// - Endgame: the deepest floors lean harder into irregular layouts (still fully procedural)
    if (maxDepth < 1) maxDepth = 1;

    // Only the Main branch uses the full suite of generator pacing rules.
    if (branch != DungeonBranch::Main) {
        return GenKind::RoomsBsp;
    }

    // Endless depths (depth > maxDepth) only occur when Infinite World mode is enabled.
    // Instead of "locking" the generator into a single late-game distribution forever,
    // we gradually bias deeper floors toward more irregular layouts (maze/cavern/catacombs)
    // while keeping a small chance of room-based floors for pacing.
    if (depth > maxDepth) {
        const int extra = depth - maxDepth;

        // How far past the nominal "bottom" we are, remapped to [0, 1].
        // The bias ramps slowly so depth 26 still feels like "late game", not instantly alien.
        const float u = std::clamp(extra / 40.0f, 0.0f, 1.0f);

        auto lerp = [&](float a, float b) -> float { return a + (b - a) * u; };

        // Base weights match the regular deep-floor distribution (depth > midpoint).
        float wMaze      = lerp(0.14f, 0.20f);
        float wWarrens   = lerp(0.18f, 0.22f);
        float wCatacombs = lerp(0.14f, 0.18f);
        float wCavern    = lerp(0.12f, 0.18f);
        float wMines     = lerp(0.12f, 0.12f);
        float wRoomsG    = lerp(0.16f, 0.07f);
        float wRoomsB    = lerp(0.14f, 0.03f);

        // Macro theming: group endless depths into run-seeded "strata" (5-9 floors each).
        // Each stratum biases generator choice so infinite descent has large-scale texture
        // (ruins -> mines -> catacombs...) rather than independent per-floor noise.
        const EndlessStratumInfo st = computeEndlessStratum(worldSeed, branch, depth, maxDepth);
        const EndlessStratumTheme theme = st.theme;

        float signatureBoost = 1.0f;
        if (st.index >= 0 && st.len > 1) {
            const float tpos = static_cast<float>(st.local) / static_cast<float>(st.len - 1);
            // Start-of-stratum floors are more "pure"; later floors blend back toward the baseline mix.
            signatureBoost = 1.25f - 0.25f * std::clamp(tpos, 0.0f, 1.0f);
        }

        auto clampW = [&](float& w) {
            if (!std::isfinite(w) || w < 0.0001f) w = 0.0001f;
        };

        auto applyThemeBias = [&]() {
            switch (theme) {
                case EndlessStratumTheme::Ruins:
                    wRoomsG *= 2.2f * signatureBoost;
                    wRoomsB *= 2.0f;
                    wMaze   *= 0.80f;
                    wCavern *= 0.80f;
                    wWarrens *= 0.90f;
                    wCatacombs *= 0.90f;
                    wMines *= 0.90f;
                    break;
                case EndlessStratumTheme::Caverns:
                    wCavern *= 2.4f * signatureBoost;
                    wWarrens *= 1.10f;
                    wRoomsG *= 0.80f;
                    wRoomsB *= 0.70f;
                    wMaze *= 0.85f;
                    wCatacombs *= 0.90f;
                    wMines *= 0.90f;
                    break;
                case EndlessStratumTheme::Labyrinth:
                    wMaze *= 2.5f * signatureBoost;
                    wCatacombs *= 1.20f;
                    wRoomsG *= 0.70f;
                    wRoomsB *= 0.60f;
                    wCavern *= 0.80f;
                    wMines *= 0.90f;
                    // Warrens still plausible in labyrinth bands.
                    wWarrens *= 1.00f;
                    break;
                case EndlessStratumTheme::Warrens:
                    wWarrens *= 2.5f * signatureBoost;
                    wMaze *= 1.10f;
                    wRoomsG *= 0.80f;
                    wRoomsB *= 0.70f;
                    wCavern *= 0.90f;
                    wMines *= 0.90f;
                    wCatacombs *= 0.90f;
                    break;
                case EndlessStratumTheme::Mines:
                    wMines *= 2.7f * signatureBoost;
                    wRoomsG *= 0.95f;
                    wRoomsB *= 0.80f;
                    wCavern *= 0.85f;
                    wMaze *= 0.80f;
                    wWarrens *= 0.90f;
                    wCatacombs *= 0.90f;
                    break;
                case EndlessStratumTheme::Catacombs:
                    wCatacombs *= 2.6f * signatureBoost;
                    wMaze *= 1.15f;
                    wRoomsG *= 0.75f;
                    wRoomsB *= 0.65f;
                    wCavern *= 0.85f;
                    wMines *= 0.90f;
                    wWarrens *= 0.95f;
                    break;
                default:
                    break;
            }
        };

        applyThemeBias();

        clampW(wMaze);
        clampW(wWarrens);
        clampW(wCatacombs);
        clampW(wCavern);
        clampW(wMines);
        clampW(wRoomsG);
        clampW(wRoomsB);

        float sum = wMaze + wWarrens + wCatacombs + wCavern + wMines + wRoomsG + wRoomsB;
        if (sum <= 0.0001f) return GenKind::RoomsBsp;

        const float r0 = rng.next01() * sum;
        float r = r0;

        if ((r -= wMaze) < 0.0f) return GenKind::Maze;
        if ((r -= wWarrens) < 0.0f) return GenKind::Warrens;
        if ((r -= wCatacombs) < 0.0f) return GenKind::Catacombs;
        if ((r -= wCavern) < 0.0f) return GenKind::Cavern;
        if ((r -= wMines) < 0.0f) return GenKind::Mines;
        if ((r -= wRoomsG) < 0.0f) return GenKind::RoomsGraph;
        return GenKind::RoomsBsp;
    }

    const int midpoint = std::max(1, maxDepth / 2);

    if (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH) return GenKind::Mines;

    // Early floors: mostly classic BSP rooms, but occasionally use the graph/packed-rooms variant
    // to keep runs from feeling identical.
	if (depth <= 3) {
		// Onboarding band: keep the first few floors room-heavy for readability.
        if (depth == 1) {
            // Keep the very first floor mostly familiar, but not always.
            if (rng.chance(0.40f)) return GenKind::RoomsGraph;
        }
        return GenKind::RoomsBsp;
    }

    if (depth == Dungeon::GROTTO_DEPTH) return GenKind::Cavern;

// Early variety spike (originally the 10-floor "midpoint"; now closer to the first quarter).
if (depth == 5) {
    // Weighted selection (so campaign strata can bias it a bit without changing the pacing).
    float wMaze    = 0.45f;
    float wWarrens = 0.20f;
    float wRoomsG  = 0.25f;
    float wRoomsB  = 0.10f;

    // Light campaign-stratum bias (keeps the spike feeling "of a piece" with the current band).
    const EndlessStratumInfo cst = computeCampaignStratum(worldSeed, branch, depth, maxDepth);
    if (cst.index >= 0 && cst.len > 0) {
        const EndlessStratumTheme theme = cst.theme;

        float signatureBoost = 1.0f;
        if (cst.len > 1) {
            const float tpos = static_cast<float>(cst.local) / static_cast<float>(cst.len - 1);
            signatureBoost = 1.18f - 0.18f * std::clamp(tpos, 0.0f, 1.0f);
        }

        auto applyThemeBias = [&]() {
            switch (theme) {
                case EndlessStratumTheme::Ruins:
                    wRoomsG *= 1.6f * signatureBoost;
                    wRoomsB *= 1.4f;
                    wMaze   *= 0.90f;
                    wWarrens *= 0.95f;
                    break;
                case EndlessStratumTheme::Caverns:
                    // Caverns strata prefer irregular layouts over room floors.
                    wMaze   *= 1.05f;
                    wWarrens *= 1.10f;
                    wRoomsG *= 0.90f;
                    wRoomsB *= 0.85f;
                    break;
                case EndlessStratumTheme::Labyrinth:
                    wMaze   *= 1.30f * signatureBoost;
                    wWarrens *= 1.05f;
                    wRoomsG *= 0.85f;
                    wRoomsB *= 0.80f;
                    break;
                case EndlessStratumTheme::Warrens:
                    wWarrens *= 1.30f * signatureBoost;
                    wMaze   *= 1.10f;
                    wRoomsG *= 0.90f;
                    wRoomsB *= 0.85f;
                    break;
                case EndlessStratumTheme::Mines:
                    // Mines strata still allow the spike, but slightly prefer rooms to avoid
                    // over-tunneling too early.
                    wRoomsG *= 1.05f;
                    wRoomsB *= 1.05f;
                    wMaze   *= 0.95f;
                    wWarrens *= 0.95f;
                    break;
                case EndlessStratumTheme::Catacombs:
                    wMaze   *= 1.10f;
                    wWarrens *= 1.00f;
                    wRoomsG *= 0.90f;
                    wRoomsB *= 0.85f;
                    break;
                default:
                    break;
            }
        };

        applyThemeBias();
    }

    const float sum = wMaze + wWarrens + wRoomsG + wRoomsB;
    if (sum <= 0.0001f) return GenKind::RoomsBsp;

    float r = rng.next01() * sum;
    if ((r -= wMaze) < 0.0f) return GenKind::Maze;
    if ((r -= wWarrens) < 0.0f) return GenKind::Warrens;
    if ((r -= wRoomsG) < 0.0f) return GenKind::RoomsGraph;
    return GenKind::RoomsBsp;
}

// True midpoint spike: lean harder into non-room layouts so the run's second half
// feels different even if the player has strong gear already.
if (depth == midpoint) {
    // Weighted selection (so campaign strata can bias it a bit without changing the pacing).
    float wMaze      = 0.30f;
    float wWarrens   = 0.25f;
    float wCatacombs = 0.17f;
    float wCavern    = 0.12f;
    float wRoomsG    = 0.10f;
    float wRoomsB    = 0.06f;

    const EndlessStratumInfo cst = computeCampaignStratum(worldSeed, branch, depth, maxDepth);
    if (cst.index >= 0 && cst.len > 0) {
        const EndlessStratumTheme theme = cst.theme;

        float signatureBoost = 1.0f;
        if (cst.len > 1) {
            const float tpos = static_cast<float>(cst.local) / static_cast<float>(cst.len - 1);
            signatureBoost = 1.18f - 0.18f * std::clamp(tpos, 0.0f, 1.0f);
        }

        auto applyThemeBias = [&]() {
            switch (theme) {
                case EndlessStratumTheme::Ruins:
                    wRoomsG *= 1.6f * signatureBoost;
                    wRoomsB *= 1.4f;
                    wMaze   *= 0.90f;
                    wCavern *= 0.90f;
                    break;
                case EndlessStratumTheme::Caverns:
                    wCavern *= 1.35f * signatureBoost;
                    wMaze   *= 1.05f;
                    wRoomsG *= 0.90f;
                    wRoomsB *= 0.85f;
                    break;
                case EndlessStratumTheme::Labyrinth:
                    wMaze *= 1.40f * signatureBoost;
                    wCatacombs *= 1.10f;
                    wRoomsG *= 0.85f;
                    wRoomsB *= 0.80f;
                    break;
                case EndlessStratumTheme::Warrens:
                    wWarrens *= 1.35f * signatureBoost;
                    wMaze   *= 1.08f;
                    wRoomsG *= 0.90f;
                    wRoomsB *= 0.85f;
                    break;
                case EndlessStratumTheme::Mines:
                    // Keep the midpoint spicy even in mines strata.
                    wMaze *= 1.05f;
                    wCatacombs *= 1.05f;
                    wRoomsG *= 0.95f;
                    wRoomsB *= 0.90f;
                    break;
                case EndlessStratumTheme::Catacombs:
                    wCatacombs *= 1.45f * signatureBoost;
                    wMaze *= 1.10f;
                    wRoomsG *= 0.85f;
                    wRoomsB *= 0.80f;
                    break;
                default:
                    break;
            }
        };

        applyThemeBias();
    }

    const float sum = wMaze + wWarrens + wCatacombs + wCavern + wRoomsG + wRoomsB;
    if (sum <= 0.0001f) return GenKind::RoomsBsp;

    float r = rng.next01() * sum;
    if ((r -= wMaze) < 0.0f) return GenKind::Maze;
    if ((r -= wWarrens) < 0.0f) return GenKind::Warrens;
    if ((r -= wCatacombs) < 0.0f) return GenKind::Catacombs;
    if ((r -= wCavern) < 0.0f) return GenKind::Cavern;
    if ((r -= wRoomsG) < 0.0f) return GenKind::RoomsGraph;
    return GenKind::RoomsBsp;
}

	// Depth 6: keep a predictable rooms floor for pacing (and for endless/testing).
    if (depth == 6) return GenKind::RoomsBsp;
    if (depth == Dungeon::CATACOMBS_DEPTH) return GenKind::Catacombs;

    // A consistent breather floor before the midpoint spike.
    if (depth == 9) return GenKind::RoomsBsp;

    // Late-run "second arc" setpieces. These are relative to maxDepth so tests that pass
    // smaller maxDepth values still behave sensibly.
    if (depth == midpoint + 2 && depth < maxDepth - 1) return GenKind::Mines;
    if (depth == midpoint + 4 && depth < maxDepth - 1) return GenKind::Catacombs;
    if (depth == midpoint + 6 && depth < maxDepth - 1) return GenKind::Cavern;

	// Calm before the bottom: give the player one more relatively "readable" rooms floor
	// right before the deepest depths.
    if (maxDepth >= 8 && depth == maxDepth - 2) {
        // Slight bias toward the "ruins" generator so the player sees more doors/loops
        // right before the final approach.
        return rng.chance(0.35f) ? GenKind::RoomsGraph : GenKind::RoomsBsp;
    }

    // General case: sprinkle variety, with a slightly "nastier" distribution deeper
    // than the midpoint. For the finite campaign we additionally apply a light
    // run-seeded stratum bias so the 1..maxDepth arc develops recognizable bands.
    float wMaze = 0.0f;
    float wWarrens = 0.0f;
    float wCatacombs = 0.0f;
    float wCavern = 0.0f;
    float wMines = 0.0f;
    float wRoomsG = 0.0f;
    float wRoomsB = 0.0f;

    if (depth > midpoint) {
        // Post-midpoint band: more irregular layouts.
        wMaze      = 0.14f;
        wWarrens   = 0.18f;
        wCatacombs = 0.14f;
        wCavern    = 0.12f;
        wMines     = 0.12f;
        wRoomsG    = 0.16f;
        wRoomsB    = 0.14f;
    } else {
        // Pre-midpoint band: still mostly rooms, with occasional spice.
        wMaze      = 0.08f;
        wWarrens   = 0.10f;
        wCatacombs = 0.08f;
        wCavern    = 0.14f;
        wMines     = 0.12f;
        wRoomsG    = 0.20f;
        wRoomsB    = 0.28f;
    }

    // Campaign-stratum bias (finite run only).
    const EndlessStratumInfo cst = computeCampaignStratum(worldSeed, branch, depth, maxDepth);
    if (cst.index >= 0 && cst.len > 0) {
        const EndlessStratumTheme theme = cst.theme;

        float signatureBoost = 1.0f;
        if (cst.len > 1) {
            const float tpos = static_cast<float>(cst.local) / static_cast<float>(cst.len - 1);
            // Start-of-stratum floors are more "pure"; later floors blend back toward the baseline mix.
            signatureBoost = 1.18f - 0.18f * std::clamp(tpos, 0.0f, 1.0f);
        }

        auto clampW = [&](float& w) {
            if (!std::isfinite(w) || w < 0.0001f) w = 0.0001f;
        };

        auto applyThemeBias = [&]() {
            switch (theme) {
                case EndlessStratumTheme::Ruins:
                    wRoomsG *= 1.8f * signatureBoost;
                    wRoomsB *= 1.6f;
                    wMaze   *= 0.85f;
                    wCavern *= 0.85f;
                    wWarrens *= 0.90f;
                    wCatacombs *= 0.90f;
                    wMines *= 0.90f;
                    break;
                case EndlessStratumTheme::Caverns:
                    wCavern *= 1.9f * signatureBoost;
                    wWarrens *= 1.10f;
                    wRoomsG *= 0.90f;
                    wRoomsB *= 0.80f;
                    wMaze *= 0.90f;
                    wCatacombs *= 0.95f;
                    wMines *= 0.95f;
                    break;
                case EndlessStratumTheme::Labyrinth:
                    wMaze *= 2.0f * signatureBoost;
                    wCatacombs *= 1.10f;
                    wRoomsG *= 0.85f;
                    wRoomsB *= 0.75f;
                    wCavern *= 0.85f;
                    wMines *= 0.95f;
                    wWarrens *= 1.05f;
                    break;
                case EndlessStratumTheme::Warrens:
                    wWarrens *= 2.0f * signatureBoost;
                    wMaze *= 1.10f;
                    wRoomsG *= 0.90f;
                    wRoomsB *= 0.80f;
                    wCavern *= 0.95f;
                    wMines *= 0.95f;
                    wCatacombs *= 0.95f;
                    break;
                case EndlessStratumTheme::Mines:
                    wMines *= 2.1f * signatureBoost;
                    wRoomsG *= 0.95f;
                    wRoomsB *= 0.85f;
                    wCavern *= 0.90f;
                    wMaze *= 0.85f;
                    wWarrens *= 0.95f;
                    wCatacombs *= 0.95f;
                    break;
                case EndlessStratumTheme::Catacombs:
                    wCatacombs *= 2.0f * signatureBoost;
                    wMaze *= 1.10f;
                    wRoomsG *= 0.85f;
                    wRoomsB *= 0.75f;
                    wCavern *= 0.90f;
                    wMines *= 0.95f;
                    wWarrens *= 1.00f;
                    break;
                default:
                    break;
            }
        };

        applyThemeBias();

        clampW(wMaze);
        clampW(wWarrens);
        clampW(wCatacombs);
        clampW(wCavern);
        clampW(wMines);
        clampW(wRoomsG);
        clampW(wRoomsB);
    }

    const float sum = wMaze + wWarrens + wCatacombs + wCavern + wMines + wRoomsG + wRoomsB;
    if (sum <= 0.0001f) return GenKind::RoomsBsp;

    float r = rng.next01() * sum;
    if ((r -= wMaze) < 0.0f) return GenKind::Maze;
    if ((r -= wWarrens) < 0.0f) return GenKind::Warrens;
    if ((r -= wCatacombs) < 0.0f) return GenKind::Catacombs;
    if ((r -= wCavern) < 0.0f) return GenKind::Cavern;
    if ((r -= wMines) < 0.0f) return GenKind::Mines;
    if ((r -= wRoomsG) < 0.0f) return GenKind::RoomsGraph;
    return GenKind::RoomsBsp;
}



// ------------------------------------------------------------
// Dead-end stash closets
//
// A late procgen pass that looks for corridor/tunnel dead-ends and carves
// tiny "closet" rooms behind a door (sometimes secret).
//
// The goal is to make exploring dead ends feel like a meaningful risk/reward
// choice, without affecting critical path connectivity between the stairs.
// ------------------------------------------------------------

// ------------------------------------------------------------
// Annex micro-dungeons: larger optional side areas carved into solid rock.
// These act like "pocket dungeons" behind a (sometimes secret/locked) door,
// providing extra exploration and optional rewards without ever gating the
// critical stairs-to-stairs path.
//
// The generator uses local sub-grid algorithms (perfect maze via recursive
// backtracking, or a small cellular-automata cavern) and then stamps the
// result into a wall pocket off an existing corridor.
// ------------------------------------------------------------
enum class AnnexStyle : uint8_t {
    MiniMaze = 0,
    MiniCavern = 1,
    // New: a tiny packed-room + corridors annex style (mini "ruins" dungeon).
    MiniRuins = 2,
    // New: a constraint-driven annex style (WFC on a coarse connectivity lattice).
    MiniWfc = 3,
    // New: a stochastic fractal annex style (L-system / turtle branching).
    MiniFractal = 4,
};

struct AnnexGrid {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> cell; // 0 = wall, 1 = floor, 2 = floor (internal door marker)

    uint8_t& at(int x, int y) { return cell[static_cast<size_t>(y * w + x)]; }
    uint8_t at(int x, int y) const { return cell[static_cast<size_t>(y * w + x)]; }

    bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
};

static Vec2i annexFarthestFloor(const AnnexGrid& g, Vec2i start) {
    if (!g.inBounds(start.x, start.y)) return {-1, -1};
    if (g.at(start.x, start.y) == 0) return {-1, -1};

    std::vector<int> dist(static_cast<size_t>(g.w * g.h), -1);
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * g.w + x); };

    std::deque<Vec2i> q;
    q.push_back(start);
    dist[idx(start.x, start.y)] = 0;

    Vec2i best = start;
    int bestD = 0;

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        Vec2i p = q.front();
        q.pop_front();
        const int d0 = dist[idx(p.x, p.y)];
        // Prefer "pure floor" tiles (cell==1) as loot anchors; treat doors (cell==2)
        // as passable during search, but avoid selecting them as the farthest target.
        if (d0 > bestD && g.at(p.x, p.y) == 1u) {
            bestD = d0;
            best = p;
        }

        for (const auto& dv : dirs) {
            const int nx = p.x + dv[0];
            const int ny = p.y + dv[1];
            if (!g.inBounds(nx, ny)) continue;
            if (g.at(nx, ny) == 0) continue;
            const size_t ii = idx(nx, ny);
            if (dist[ii] >= 0) continue;
            dist[ii] = d0 + 1;
            q.push_back({nx, ny});
        }
    }

    return best;
}

struct AnnexKeyGatePlan {
    Vec2i gate{ -1, -1 };   // tile that becomes an internal locked door
    Vec2i key{ -1, -1 };    // key spawn position (entry-side)
    Vec2i loot{ -1, -1 };   // chest spawn position (gated side)
    int gatedComponentSize = 0;
};

// Plan a small internal lock-and-key micro-puzzle inside an annex.
//
// We look for an *articulation point* (cut-vertex) in the annex walk graph
// such that locking that tile splits the annex into:
//   - an entry-side region containing the entrance
//   - a gated region big enough to feel meaningful
//
// We then place:
//   - a locked door at the articulation tile
//   - a key somewhere in the entry-side region
//   - the annex chest reward in the gated region
//
// This is fully optional content: annexes never gate stairs traversal.
static bool annexPlanKeyGate(const AnnexGrid& g, RNG& rng, Vec2i entry, AnnexKeyGatePlan& out) {
    out = {};
    const int W = g.w;
    const int H = g.h;
    if (W <= 0 || H <= 0) return false;
    if (!g.inBounds(entry.x, entry.y)) return false;
    if (g.at(entry.x, entry.y) == 0u) return false;

    auto idx = [&](int x, int y) -> int { return y * W + x; };

    auto passable = [&](int x, int y) -> bool {
        return g.inBounds(x, y) && g.at(x, y) != 0u;
    };

    auto selectable = [&](int x, int y) -> bool {
        // Avoid picking internal door markers as key/loot anchors.
        return g.inBounds(x, y) && g.at(x, y) == 1u;
    };

    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    // Distance map from entry (treat any non-wall cell as passable).
    std::vector<int> dist0(static_cast<size_t>(W * H), -1);
    {
        std::deque<Vec2i> q;
        q.push_back(entry);
        dist0[static_cast<size_t>(idx(entry.x, entry.y))] = 0;

        while (!q.empty()) {
            const Vec2i p = q.front();
            q.pop_front();
            const int d0 = dist0[static_cast<size_t>(idx(p.x, p.y))];

            for (const auto& dv : dirs) {
                const int nx = p.x + dv[0];
                const int ny = p.y + dv[1];
                if (!passable(nx, ny)) continue;
                const int ii = idx(nx, ny);
                if (dist0[static_cast<size_t>(ii)] >= 0) continue;
                dist0[static_cast<size_t>(ii)] = d0 + 1;
                q.push_back({nx, ny});
            }
        }
    }

    // Compute a rough size + farthest distance so we can clamp gate placement.
    int reachable = 0;
    int maxD = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int ii = idx(x, y);
            const int d0 = dist0[static_cast<size_t>(ii)];
            if (d0 < 0) continue;
            reachable += 1;
            if (d0 > maxD) maxD = d0;
        }
    }

    // Too small / too trivial: skip.
    if (reachable < 60 || maxD < 14) return false;

    // --- Articulation points (Tarjan) on the reachable subgraph ---
    std::vector<int> disc(static_cast<size_t>(W * H), -1);
    std::vector<int> low(static_cast<size_t>(W * H), -1);
    std::vector<int> parent(static_cast<size_t>(W * H), -1);
    std::vector<uint8_t> art(static_cast<size_t>(W * H), uint8_t{0});

    int time = 0;

    std::function<void(int)> dfs = [&](int v) {
        disc[static_cast<size_t>(v)] = low[static_cast<size_t>(v)] = ++time;
        const int vx = v % W;
        const int vy = v / W;

        int children = 0;
        for (const auto& dv : dirs) {
            const int nx = vx + dv[0];
            const int ny = vy + dv[1];
            if (!passable(nx, ny)) continue;
            const int u = idx(nx, ny);
            if (dist0[static_cast<size_t>(u)] < 0) continue; // unreachable

            if (disc[static_cast<size_t>(u)] < 0) {
                parent[static_cast<size_t>(u)] = v;
                children += 1;
                dfs(u);
                low[static_cast<size_t>(v)] = std::min(low[static_cast<size_t>(v)], low[static_cast<size_t>(u)]);

                if (parent[static_cast<size_t>(v)] < 0 && children > 1) art[static_cast<size_t>(v)] = 1u;
                if (parent[static_cast<size_t>(v)] >= 0 && low[static_cast<size_t>(u)] >= disc[static_cast<size_t>(v)]) art[static_cast<size_t>(v)] = 1u;
            } else if (u != parent[static_cast<size_t>(v)]) {
                low[static_cast<size_t>(v)] = std::min(low[static_cast<size_t>(v)], disc[static_cast<size_t>(u)]);
            }
        }
    };

    dfs(idx(entry.x, entry.y));

    // BFS helper: build a distance map from entry while treating one tile as blocked.
    auto bfsExcluding = [&](int blockIdx, std::vector<int>& outDist) {
        outDist.assign(static_cast<size_t>(W * H), -1);
        if (blockIdx == idx(entry.x, entry.y)) return;

        std::deque<Vec2i> q;
        q.push_back(entry);
        outDist[static_cast<size_t>(idx(entry.x, entry.y))] = 0;

        while (!q.empty()) {
            const Vec2i p = q.front();
            q.pop_front();
            const int d0 = outDist[static_cast<size_t>(idx(p.x, p.y))];

            for (const auto& dv : dirs) {
                const int nx = p.x + dv[0];
                const int ny = p.y + dv[1];
                if (!passable(nx, ny)) continue;
                const int ii = idx(nx, ny);
                if (ii == blockIdx) continue;
                if (outDist[static_cast<size_t>(ii)] >= 0) continue;
                outDist[static_cast<size_t>(ii)] = d0 + 1;
                q.push_back({nx, ny});
            }
        }
    };

    // Candidate search.
    AnnexKeyGatePlan best;
    int bestScore = -1;

    const int entryIdx = idx(entry.x, entry.y);

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            const int gateIdx = idx(x, y);
            if (gateIdx == entryIdx) continue;
            if (art[static_cast<size_t>(gateIdx)] == 0u) continue;
            if (!selectable(x, y)) continue;

            const int gateDist = dist0[static_cast<size_t>(gateIdx)];
            if (gateDist < 8) continue;
            if (gateDist > maxD - 6) continue;

            // Prefer corridor-ish tiles: degree 2 is ideal; degree 3 acceptable.
            int deg = 0;
            for (const auto& dv : dirs) {
                const int nx = x + dv[0];
                const int ny = y + dv[1];
                if (passable(nx, ny)) deg += 1;
            }
            if (deg < 2 || deg > 3) continue;

            // Compute the entry-side component if the gate were blocked.
            std::vector<int> distA;
            bfsExcluding(gateIdx, distA);
            if (distA[static_cast<size_t>(entryIdx)] < 0) continue;

            // Pick a key location in the entry-side component.
            Vec2i key{-1, -1};
            int keyD = -1;
            for (int yy = 1; yy < H - 1; ++yy) {
                for (int xx = 1; xx < W - 1; ++xx) {
                    const int ii = idx(xx, yy);
                    const int d0 = distA[static_cast<size_t>(ii)];
                    if (d0 < 0) continue;
                    if (!selectable(xx, yy)) continue;
                    if (d0 < 4) continue;
                    if (std::abs(xx - x) + std::abs(yy - y) < 2) continue;
                    if (d0 > keyD) {
                        keyD = d0;
                        key = {xx, yy};
                    }
                }
            }
            if (keyD < 0) continue;

            // Explore gated-side components (neighbors not reachable from entry without the gate).
            std::vector<uint8_t> seen(static_cast<size_t>(W * H), uint8_t{0});
            int bestComp = 0;
            int bestLootD = -1;
            Vec2i bestLoot{-1, -1};

            for (const auto& dv : dirs) {
                const int sx = x + dv[0];
                const int sy = y + dv[1];
                if (!passable(sx, sy)) continue;
                const int si = idx(sx, sy);
                if (si == gateIdx) continue;
                if (distA[static_cast<size_t>(si)] >= 0) continue; // entry-side
                if (seen[static_cast<size_t>(si)] != 0u) continue;

                int compSize = 0;
                int lootD = -1;
                Vec2i loot{-1, -1};

                std::deque<Vec2i> q;
                q.push_back({sx, sy});
                seen[static_cast<size_t>(si)] = 1u;

                while (!q.empty()) {
                    const Vec2i p = q.front();
                    q.pop_front();

                    const int pi = idx(p.x, p.y);
                    compSize += 1;

                    // Choose loot anchor by deepest original distance from entry.
                    const int dOrig = dist0[static_cast<size_t>(pi)];
                    if (dOrig >= 0 && selectable(p.x, p.y) && dOrig > lootD) {
                        lootD = dOrig;
                        loot = p;
                    }

                    for (const auto& dv2 : dirs) {
                        const int nx = p.x + dv2[0];
                        const int ny = p.y + dv2[1];
                        if (!passable(nx, ny)) continue;
                        const int ni = idx(nx, ny);
                        if (ni == gateIdx) continue;
                        if (distA[static_cast<size_t>(ni)] >= 0) continue; // don't cross into entry-side
                        if (seen[static_cast<size_t>(ni)] != 0u) continue;
                        seen[static_cast<size_t>(ni)] = 1u;
                        q.push_back({nx, ny});
                    }
                }

                // Require a meaningful gated region.
                if (compSize < 18) continue;
                if (lootD < 0) continue;

                if (compSize > bestComp || (compSize == bestComp && lootD > bestLootD)) {
                    bestComp = compSize;
                    bestLootD = lootD;
                    bestLoot = loot;
                }
            }

            if (bestComp <= 0 || bestLootD < 0) continue;

            // Candidate score.
            int score = bestComp * 120 + bestLootD * 14 + gateDist * 6;
            if (deg == 3) score -= 180; // prefer corridor chokepoints
            // Deterministic jitter to avoid always picking the same gate on symmetric grids.
            score += ((x * 17 + y * 31) & 7);

            // Slight randomness among close candidates.
            score += static_cast<int>(rng.range(0, 3));

            if (score > bestScore) {
                bestScore = score;
                best.gate = {x, y};
                best.key = key;
                best.loot = bestLoot;
                best.gatedComponentSize = bestComp;
            }
        }
    }

    if (bestScore < 0) return false;
    if (!g.inBounds(best.gate.x, best.gate.y)) return false;
    if (!g.inBounds(best.key.x, best.key.y)) return false;
    if (!g.inBounds(best.loot.x, best.loot.y)) return false;

    out = best;
    return true;
}

static bool annexGenPerfectMaze(AnnexGrid& g, RNG& rng, Vec2i entry) {
    // Expect odd dimensions so that cell centers are on odd coordinates.
    if (g.w < 9 || g.h < 9) return false;
    if ((g.w % 2) == 0 || (g.h % 2) == 0) return false;

    // Clear to walls.
    std::fill(g.cell.begin(), g.cell.end(), uint8_t{0});

    const int cellW = (g.w - 1) / 2;
    const int cellH = (g.h - 1) / 2;
    if (cellW <= 1 || cellH <= 1) return false;

    auto cidx = [&](int cx, int cy) -> size_t { return static_cast<size_t>(cy * cellW + cx); };
    auto cellToPos = [&](int cx, int cy) -> Vec2i { return {1 + cx * 2, 1 + cy * 2}; };

    // Clamp entry to a valid odd cell center.
    int startCx = std::clamp((entry.x - 1) / 2, 0, cellW - 1);
    int startCy = std::clamp((entry.y - 1) / 2, 0, cellH - 1);

    std::vector<uint8_t> vis(static_cast<size_t>(cellW * cellH), uint8_t{0});
    std::vector<Vec2i> st;
    st.reserve(static_cast<size_t>(cellW * cellH));

    st.push_back({startCx, startCy});
    vis[cidx(startCx, startCy)] = 1u;
    {
        const Vec2i p = cellToPos(startCx, startCy);
        g.at(p.x, p.y) = 1u;
    }

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!st.empty()) {
        const Vec2i cur = st.back();

        // Collect unvisited neighbors.
        int opts[4] = {0,1,2,3};
        // Shuffle directions a bit.
        for (int i = 3; i > 0; --i) {
            const int j = rng.range(0, i);
            std::swap(opts[i], opts[j]);
        }

        bool advanced = false;
        for (int k = 0; k < 4; ++k) {
            const int di = opts[k];
            const int ncx = cur.x + dirs[di][0];
            const int ncy = cur.y + dirs[di][1];
            if (ncx < 0 || ncy < 0 || ncx >= cellW || ncy >= cellH) continue;
            const size_t ni = cidx(ncx, ncy);
            if (vis[ni]) continue;

            // Carve the wall between the two cells.
            const Vec2i a = cellToPos(cur.x, cur.y);
            const Vec2i b = cellToPos(ncx, ncy);
            const Vec2i mid{ (a.x + b.x) / 2, (a.y + b.y) / 2 };

            g.at(mid.x, mid.y) = 1u;
            g.at(b.x, b.y) = 1u;

            vis[ni] = 1u;
            st.push_back({ncx, ncy});
            advanced = true;
            break;
        }

        if (!advanced) {
            st.pop_back();
        }
    }

    // Ensure the entry is floor (and on the interior ring, not the border wall ring).
    if (g.inBounds(entry.x, entry.y) && entry.x > 0 && entry.y > 0 && entry.x < g.w - 1 && entry.y < g.h - 1) {
        g.at(entry.x, entry.y) = 1u;
    }

    // Add a few loops by breaking random internal walls (keeps it from being a strict tree).
    const int wantBreaks = std::clamp((cellW * cellH) / 6, 6, 26);
    for (int i = 0; i < wantBreaks; ++i) {
        // Pick a candidate wall location on even coordinates within the interior ring.
        int x = rng.range(2, g.w - 3);
        int y = rng.range(2, g.h - 3);
        if ((x % 2) == 1 && (y % 2) == 1) continue; // skip cell centers
        if (g.at(x, y) != 0u) continue;

        // Break walls that connect two existing floor regions.
        bool ok = false;
        if (x % 2 == 0 && (g.at(x - 1, y) != 0u) && (g.at(x + 1, y) != 0u)) ok = true;
        if (y % 2 == 0 && (g.at(x, y - 1) != 0u) && (g.at(x, y + 1) != 0u)) ok = true;
        if (!ok) continue;

        g.at(x, y) = 1u;
    }

    return true;
}


// ------------------------------------------------------------
// Annex fractal layout (stochastic L-system / turtle branching)
//
// This annex style generates a compact, highly-branching pocket-dungeon by
// interpreting a small stochastic L-system (classic turtle + stack grammar)
// onto the annex grid. Compared to the perfect maze and CA cavern styles,
// this produces a readable "coral" of corridors with lots of micro-choices
// and a strong sense of local structure without needing room rectangles.
// ------------------------------------------------------------

static bool annexGenFractalAnnex(AnnexGrid& g, RNG& rng, Vec2i entry, int depth) {
    if (g.w < 13 || g.h < 13) return false;

    // Start with walls; carve only within the interior ring so the annex
    // always has a solid boundary.
    std::fill(g.cell.begin(), g.cell.end(), uint8_t{0});

    if (!g.inBounds(entry.x, entry.y)) return false;

    // Direction encoding: 0=E,1=S,2=W,3=N.
    const Vec2i dirs[4] = {{1,0},{0,1},{-1,0},{0,-1}};

    auto inInterior = [&](int x, int y) -> bool {
        return x > 0 && y > 0 && x < g.w - 1 && y < g.h - 1;
    };

    auto carve = [&](int x, int y) {
        if (!inInterior(x, y)) return;
        g.at(x, y) = 1u;
    };

    // Guess the initial heading based on where the entry tile sits near the pocket border.
    int dir = 0;
    if (entry.x <= 1) dir = 0;
    else if (entry.x >= g.w - 2) dir = 2;
    else if (entry.y <= 1) dir = 1;
    else if (entry.y >= g.h - 2) dir = 3;
    else dir = rng.range(0, 3);

    // Build a small stochastic L-system string.
    // Grammar:
    //   F: step forward + carve
    //   +: turn right 90
    //   -: turn left 90
    //   [: push turtle state
    //   ]: pop turtle state
    int iters = 3;
    if (depth >= 6 && rng.chance(0.55f)) iters = 4;

    const size_t maxLen = 9000;
    std::string s = "F";
    for (int it = 0; it < iters; ++it) {
        std::string out;
        out.reserve(std::min(maxLen, s.size() * 6));
        for (char c : s) {
            if (c == 'F') {
                const int r = rng.range(0, 99);
                // Bias toward the classic branching rule, with a few simpler variants
                // to keep density under control on small pockets.
                if (r < 42) {
                    out += "F[+F]F[-F]F";
                } else if (r < 66) {
                    out += "F[+F]FF";
                } else if (r < 90) {
                    out += "F[-F]FF";
                } else {
                    out += "FF";
                }
            } else {
                out.push_back(c);
            }
            if (out.size() >= maxLen) break;
        }
        s.swap(out);
        if (s.size() >= maxLen) break;
    }

    struct TurtleState { Vec2i pos; int dir = 0; };
    std::vector<TurtleState> st;
    st.reserve(64);

    Vec2i pos = entry;
    carve(pos.x, pos.y);

    // Interpret the L-system into carved corridors.
    const int maxSteps = std::max(200, (g.w * g.h) * 6);
    int steps = 0;

    for (char c : s) {
        if (steps >= maxSteps) break;

        switch (c) {
            case 'F': {
                // Attempt a forward step; if it would exit the interior ring,
                // rotate a bit and retry to keep the pattern inside bounds.
                for (int k = 0; k < 4; ++k) {
                    const Vec2i np{pos.x + dirs[dir].x, pos.y + dirs[dir].y};
                    if (inInterior(np.x, np.y)) {
                        pos = np;
                        carve(pos.x, pos.y);

                        // Occasional thickening to avoid an overly 1-tile "wire" look.
                        if (depth >= 4 && rng.chance(0.12f)) {
                            const int side = (dir + (rng.chance(0.5f) ? 1 : 3)) & 3;
                            const Vec2i sp{pos.x + dirs[side].x, pos.y + dirs[side].y};
                            carve(sp.x, sp.y);
                        }
                        break;
                    }
                    dir = (dir + (rng.chance(0.5f) ? 1 : 3)) & 3;
                }
                steps += 1;
            } break;
            case '+': dir = (dir + 1) & 3; break;
            case '-': dir = (dir + 3) & 3; break;
            case '[': {
                if (st.size() < 256) st.push_back({pos, dir});
            } break;
            case ']': {
                if (!st.empty()) {
                    TurtleState t = st.back();
                    st.pop_back();
                    pos = t.pos;
                    dir = t.dir;
                }
            } break;
            default: break;
        }
    }

    // Ensure the entry area is open so the annex doesn't feel immediately cramped.
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            carve(entry.x + dx, entry.y + dy);
        }
    }

    // Compute degree on carved tiles (4-neighborhood).
    auto isFloor = [&](int x, int y) -> bool {
        return g.inBounds(x, y) && g.at(x, y) != 0u;
    };

    auto deg4 = [&](int x, int y) -> int {
        int c = 0;
        c += isFloor(x + 1, y) ? 1 : 0;
        c += isFloor(x - 1, y) ? 1 : 0;
        c += isFloor(x, y + 1) ? 1 : 0;
        c += isFloor(x, y - 1) ? 1 : 0;
        return c;
    };

    // Inflate some junctions into tiny chambers.
    float pChamber = 0.18f + 0.02f * static_cast<float>(std::clamp(depth - 4, 0, 12));
    pChamber = std::clamp(pChamber, 0.16f, 0.45f);

    int chambers = 0;
    const int chamberCap = std::clamp((g.w * g.h) / 45, 3, 10);

    for (int y = 2; y < g.h - 2; ++y) {
        for (int x = 2; x < g.w - 2; ++x) {
            if (g.at(x, y) == 0u) continue;
            if (deg4(x, y) < 3) continue;
            if (!rng.chance(pChamber)) continue;

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    carve(x + dx, y + dy);
                }
            }

            chambers += 1;
            if (chambers >= chamberCap) break;
        }
        if (chambers >= chamberCap) break;
    }

    // Bud a few alcoves off dead-ends so tip exploration is rewarding.
    float pAlcove = 0.26f + 0.02f * static_cast<float>(std::clamp(depth - 3, 0, 12));
    pAlcove = std::clamp(pAlcove, 0.22f, 0.52f);

    for (int y = 2; y < g.h - 2; ++y) {
        for (int x = 2; x < g.w - 2; ++x) {
            if (g.at(x, y) == 0u) continue;
            if (deg4(x, y) != 1) continue;
            if (!rng.chance(pAlcove)) continue;

            // Find the single neighbor and extend away from it.
            Vec2i nb{-1, -1};
            if (isFloor(x + 1, y)) nb = {x + 1, y};
            else if (isFloor(x - 1, y)) nb = {x - 1, y};
            else if (isFloor(x, y + 1)) nb = {x, y + 1};
            else if (isFloor(x, y - 1)) nb = {x, y - 1};

            if (nb.x < 0) continue;

            const Vec2i out{ x - (nb.x - x), y - (nb.y - y) };
            carve(out.x, out.y);

            if (depth >= 7 && rng.chance(0.40f)) {
                const Vec2i out2{ out.x - (nb.x - x), out.y - (nb.y - y) };
                carve(out2.x, out2.y);
            }
        }
    }

    // Sanity: ensure we carved a meaningful amount of walkable space.
    int floors = 0;
    for (int y = 1; y < g.h - 1; ++y) {
        for (int x = 1; x < g.w - 1; ++x) {
            if (g.at(x, y) != 0u) floors += 1;
        }
    }

    const int minFloors = std::max(40, (g.w * g.h) / 6);
    if (floors < minFloors) return false;

    return true;
}

// ------------------------------------------------------------
// Annex WFC layout (constraint-driven "pipe network")
//
// Unlike the room-furnishing WFC pass (which only places obstacles), this
// generator uses WFC to synthesize an entire *walkable* micro-dungeon.
//
// We run WFC on a coarse lattice where each cell chooses a connectivity tile
// (dead-end / corner / straight / tee / cross / empty). The connectivity bits
// must match across neighbors (a Wang-tile style constraint). The resulting
// graph is then stamped into the AnnexGrid as 1-tile corridors on odd coords,
// and we "inflate" a few junctions into tiny chambers so it doesn't read as
// purely plumbing.
// ------------------------------------------------------------

enum class AnnexWfcConnTile : uint8_t {
    Empty = 0,
    DeadN,
    DeadE,
    DeadS,
    DeadW,
    StraightH,
    StraightV,
    CornerNE,
    CornerES,
    CornerSW,
    CornerWN,
    TeeN,   // missing N (E+S+W)
    TeeE,   // missing E (N+S+W)
    TeeS,   // missing S (N+E+W)
    TeeW,   // missing W (N+E+S)
    Cross,
};

static bool annexGenWfcAnnex(AnnexGrid& g, RNG& rng, Vec2i entry, int depth) {
    // Expect odd dimensions so cell centers are on odd coordinates.
    if (g.w < 17 || g.h < 11) return false;
    if ((g.w % 2) == 0 || (g.h % 2) == 0) return false;

    std::fill(g.cell.begin(), g.cell.end(), uint8_t{0});

    const int cellW = (g.w - 1) / 2;
    const int cellH = (g.h - 1) / 2;
    if (cellW < 4 || cellH < 3) return false;

    constexpr uint8_t N = 1u;
    constexpr uint8_t E = 2u;
    constexpr uint8_t S = 4u;
    constexpr uint8_t W = 8u;

    // Tile index -> connectivity mask.
    constexpr uint8_t masks[16] = {
        0u,        // Empty
        N, E, S, W, // Dead-ends
        uint8_t(E | W), // StraightH
        uint8_t(N | S), // StraightV
        uint8_t(N | E), // CornerNE
        uint8_t(E | S), // CornerES
        uint8_t(S | W), // CornerSW
        uint8_t(W | N), // CornerWN
        uint8_t(E | S | W), // TeeN
        uint8_t(N | S | W), // TeeE
        uint8_t(N | E | W), // TeeS
        uint8_t(N | E | S), // TeeW
        uint8_t(N | E | S | W), // Cross
    };

    auto pop4 = [&](uint8_t m) -> int {
        int c = 0;
        c += (m & N) ? 1 : 0;
        c += (m & E) ? 1 : 0;
        c += (m & S) ? 1 : 0;
        c += (m & W) ? 1 : 0;
        return c;
    };

    auto idx = [&](int cx, int cy) -> size_t { return static_cast<size_t>(cy * cellW + cx); };
    auto cellToPos = [&](int cx, int cy) -> Vec2i { return {1 + cx * 2, 1 + cy * 2}; };

    // Clamp entry to a valid cell center.
    int startCx = std::clamp((entry.x - 1) / 2, 0, cellW - 1);
    int startCy = std::clamp((entry.y - 1) / 2, 0, cellH - 1);

    // Build adjacency rules once.
    static bool rulesBuilt = false;
    static std::vector<uint32_t> allow[4];
    if (!rulesBuilt) {
        constexpr int nTiles = 16;
        for (int dir = 0; dir < 4; ++dir) allow[dir].assign(nTiles, 0u);

        for (int a = 0; a < nTiles; ++a) {
            const uint8_t ma = masks[a];
            for (int b = 0; b < nTiles; ++b) {
                const uint8_t mb = masks[b];

                // Dir ordering is defined by wfc.hpp: 0=+X,1=-X,2=+Y,3=-Y.
                const bool okE = ((ma & E) != 0u) == ((mb & W) != 0u);
                const bool okW = ((ma & W) != 0u) == ((mb & E) != 0u);
                const bool okS = ((ma & S) != 0u) == ((mb & N) != 0u);
                const bool okN = ((ma & N) != 0u) == ((mb & S) != 0u);

                if (okE) allow[0][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(b));
                if (okW) allow[1][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(b));
                if (okS) allow[2][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(b));
                if (okN) allow[3][static_cast<size_t>(a)] |= (1u << static_cast<uint32_t>(b));
            }
        }

        rulesBuilt = true;
    }

    constexpr int nTiles = 16;
    const uint32_t full = wfc::allMask(nTiles);

    // Depth-controlled weights (higher depth -> more junctions / fewer dead-ends).
    const float dd = static_cast<float>(std::clamp(depth - 4, 0, 12));
    const float wEmpty = std::clamp(64.0f - 2.0f * dd, 34.0f, 72.0f);
    const float wDead = std::clamp(18.0f - 0.6f * dd, 8.0f, 20.0f);
    const float wStraight = 22.0f + 0.2f * dd;
    const float wCorner = 22.0f + 0.2f * dd;
    const float wTee = 7.0f + 1.25f * dd;
    const float wCross = 2.0f + 0.6f * dd;

    std::vector<float> weights(static_cast<size_t>(nTiles), 0.0f);
    for (int t = 0; t < nTiles; ++t) {
        const uint8_t m = masks[t];
        if (t == static_cast<int>(AnnexWfcConnTile::Empty)) {
            weights[static_cast<size_t>(t)] = wEmpty;
            continue;
        }

        const int deg = pop4(m);
        if (deg == 1) weights[static_cast<size_t>(t)] = wDead;
        else if (deg == 2) {
            const bool straight = (m == (N | S)) || (m == (E | W));
            weights[static_cast<size_t>(t)] = straight ? wStraight : wCorner;
        } else if (deg == 3) {
            weights[static_cast<size_t>(t)] = wTee;
        } else {
            weights[static_cast<size_t>(t)] = wCross;
        }
    }

    // Per-cell initial domains with border constraints.
    std::vector<uint32_t> domains(static_cast<size_t>(cellW * cellH), full);
    for (int cy = 0; cy < cellH; ++cy) {
        for (int cx = 0; cx < cellW; ++cx) {
            uint8_t forbid = 0u;
            if (cx == 0) forbid |= W;
            if (cx == cellW - 1) forbid |= E;
            if (cy == 0) forbid |= N;
            if (cy == cellH - 1) forbid |= S;

            uint32_t dom = full;
            for (int t = 0; t < nTiles; ++t) {
                if ((masks[t] & forbid) != 0u) {
                    dom &= ~(1u << static_cast<uint32_t>(t));
                }
            }

            domains[idx(cx, cy)] = dom;
        }
    }

    // Entry must be walkable (non-empty).
    domains[idx(startCx, startCy)] &= ~(1u << static_cast<uint32_t>(AnnexWfcConnTile::Empty));

    auto inCellBounds = [&](int cx, int cy) { return cx >= 0 && cy >= 0 && cx < cellW && cy < cellH; };

    const int wantMin = std::max(10, (cellW * cellH) / 3);
    const int maxAttempts = 18;

    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        std::vector<uint8_t> sol;
        wfc::SolveStats stats;
        const bool ok = wfc::solve(cellW, cellH, nTiles, allow, weights, rng, domains, sol, /*maxRestarts=*/18, &stats);
        (void)stats;
        if (!ok) continue;

        // Flood-fill the entry-connected component on the coarse graph.
        std::vector<int> dist(static_cast<size_t>(cellW * cellH), -1);
        std::deque<Vec2i> q;

        const uint8_t startMask = masks[sol[idx(startCx, startCy)]];
        if (startMask == 0u) continue;

        dist[idx(startCx, startCy)] = 0;
        q.push_back({startCx, startCy});

        auto oppBit = [&](int dir) -> uint8_t {
            switch (dir) {
                case 0: return W; // east neighbor must have W
                case 1: return E;
                case 2: return N;
                default: return S;
            }
        };

        const int dcx[4] = {1, -1, 0, 0};
        const int dcy[4] = {0, 0, 1, -1};
        const uint8_t bitOut[4] = {E, W, S, N};

        while (!q.empty()) {
            const Vec2i cur = q.front();
            q.pop_front();

            const int cd = dist[idx(cur.x, cur.y)];
            const uint8_t m = masks[sol[idx(cur.x, cur.y)]];

            for (int dir = 0; dir < 4; ++dir) {
                if ((m & bitOut[dir]) == 0u) continue;
                const int nx = cur.x + dcx[dir];
                const int ny = cur.y + dcy[dir];
                if (!inCellBounds(nx, ny)) continue;
                const uint8_t nm = masks[sol[idx(nx, ny)]];
                if (nm == 0u) continue;
                if ((nm & oppBit(dir)) == 0u) continue;
                if (dist[idx(nx, ny)] >= 0) continue;
                dist[idx(nx, ny)] = cd + 1;
                q.push_back({nx, ny});
            }
        }

        int nodes = 0;
        int branches = 0;
        int maxD = 0;
        for (int cy = 0; cy < cellH; ++cy) {
            for (int cx = 0; cx < cellW; ++cx) {
                const int d0 = dist[idx(cx, cy)];
                if (d0 < 0) continue;
                nodes += 1;
                maxD = std::max(maxD, d0);
                const int deg = pop4(masks[sol[idx(cx, cy)]]);
                if (deg >= 3) branches += 1;
            }
        }

        if (nodes < wantMin) continue;
        if (depth >= 5 && branches == 0) continue;
        if (maxD < 4) continue;

        // Prefer at least one cycle on deep floors (but don't hard-require it).
        if (depth >= 8 && nodes >= wantMin + 4 && rng.chance(0.65f)) {
            int edges = 0;
            for (int cy = 0; cy < cellH; ++cy) {
                for (int cx = 0; cx < cellW; ++cx) {
                    if (dist[idx(cx, cy)] < 0) continue;
                    const uint8_t m = masks[sol[idx(cx, cy)]];
                    if ((m & E) != 0u && inCellBounds(cx + 1, cy) && dist[idx(cx + 1, cy)] >= 0) edges += 1;
                    if ((m & S) != 0u && inCellBounds(cx, cy + 1) && dist[idx(cx, cy + 1)] >= 0) edges += 1;
                }
            }
            const int cycles = edges - (nodes - 1);
            if (cycles < 1) continue;
        }

        // Stamp the entry-connected component into the annex grid.
        for (int cy = 0; cy < cellH; ++cy) {
            for (int cx = 0; cx < cellW; ++cx) {
                if (dist[idx(cx, cy)] < 0) continue;
                const Vec2i p = cellToPos(cx, cy);
                if (!g.inBounds(p.x, p.y)) continue;
                g.at(p.x, p.y) = 1u;
            }
        }

        for (int cy = 0; cy < cellH; ++cy) {
            for (int cx = 0; cx < cellW; ++cx) {
                if (dist[idx(cx, cy)] < 0) continue;
                const uint8_t m = masks[sol[idx(cx, cy)]];
                const Vec2i p = cellToPos(cx, cy);
                if ((m & E) != 0u && inCellBounds(cx + 1, cy) && dist[idx(cx + 1, cy)] >= 0) {
                    if (g.inBounds(p.x + 1, p.y)) g.at(p.x + 1, p.y) = 1u;
                }
                if ((m & S) != 0u && inCellBounds(cx, cy + 1) && dist[idx(cx, cy + 1)] >= 0) {
                    if (g.inBounds(p.x, p.y + 1)) g.at(p.x, p.y + 1) = 1u;
                }
            }
        }

        // Ensure the entry area is open.
        if (g.inBounds(entry.x, entry.y)) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = entry.x + dx;
                    const int ny = entry.y + dy;
                    if (nx <= 0 || ny <= 0 || nx >= g.w - 1 || ny >= g.h - 1) continue;
                    g.at(nx, ny) = 1u;
                }
            }
            g.at(entry.x, entry.y) = 1u;
        }

        // Inflate some junctions into tiny chambers (for readability / loot staging).
        float pChamber = 0.15f + 0.02f * static_cast<float>(std::clamp(depth - 4, 0, 12));
        pChamber = std::clamp(pChamber, 0.12f, 0.40f);

        for (int cy = 0; cy < cellH; ++cy) {
            for (int cx = 0; cx < cellW; ++cx) {
                if (dist[idx(cx, cy)] < 0) continue;
                const uint8_t m = masks[sol[idx(cx, cy)]];
                if (pop4(m) < 3) continue;
                if (!rng.chance(pChamber)) continue;

                const Vec2i p = cellToPos(cx, cy);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = p.x + dx;
                        const int ny = p.y + dy;
                        if (nx <= 0 || ny <= 0 || nx >= g.w - 1 || ny >= g.h - 1) continue;
                        g.at(nx, ny) = 1u;
                    }
                }
            }
        }

        // Carve a few tiny alcoves beyond dead-ends.
        float pAlcove = 0.22f + 0.02f * static_cast<float>(std::clamp(depth - 3, 0, 12));
        pAlcove = std::clamp(pAlcove, 0.18f, 0.45f);

        for (int cy = 0; cy < cellH; ++cy) {
            for (int cx = 0; cx < cellW; ++cx) {
                if (dist[idx(cx, cy)] < 0) continue;
                const uint8_t m = masks[sol[idx(cx, cy)]];
                if (pop4(m) != 1) continue;
                if (!rng.chance(pAlcove)) continue;

                const Vec2i p = cellToPos(cx, cy);
                int ax = p.x;
                int ay = p.y;

                if ((m & N) != 0u) ay += 1;
                else if ((m & S) != 0u) ay -= 1;
                else if ((m & E) != 0u) ax -= 1;
                else if ((m & W) != 0u) ax += 1;

                if (ax <= 0 || ay <= 0 || ax >= g.w - 1 || ay >= g.h - 1) continue;
                g.at(ax, ay) = 1u;

                if (depth >= 7 && rng.chance(0.45f)) {
                    // A second tile deeper into the alcove.
                    int bx = ax;
                    int by = ay;
                    if ((m & N) != 0u) by += 1;
                    else if ((m & S) != 0u) by -= 1;
                    else if ((m & E) != 0u) bx -= 1;
                    else if ((m & W) != 0u) bx += 1;
                    if (bx > 0 && by > 0 && bx < g.w - 1 && by < g.h - 1) {
                        g.at(bx, by) = 1u;
                    }
                }
            }
        }

        // Optional: sprinkle 0-2 internal door markers on straight corridor tiles.
        // (Only meaningful if the caller converts cell==2 into Door tiles.)
        int wantDoors = 0;
        if (rng.chance(0.55f)) wantDoors = 1;
        if (depth >= 9 && rng.chance(0.25f)) wantDoors += 1;
        wantDoors = std::clamp(wantDoors, 0, 2);

        if (wantDoors > 0) {
            std::vector<Vec2i> cands;
            cands.reserve(static_cast<size_t>((g.w * g.h) / 8));

            auto isFloor = [&](int x, int y) -> bool {
                return g.inBounds(x, y) && g.at(x, y) != 0u;
            };

            for (int y = 2; y < g.h - 2; ++y) {
                for (int x = 2; x < g.w - 2; ++x) {
                    if (g.at(x, y) != 1u) continue; // don't stack door markers
                    if (std::abs(x - entry.x) + std::abs(y - entry.y) <= 2) continue;

                    const bool L = isFloor(x - 1, y);
                    const bool R = isFloor(x + 1, y);
                    const bool U = isFloor(x, y - 1);
                    const bool D = isFloor(x, y + 1);
                    const int deg = (L ? 1 : 0) + (R ? 1 : 0) + (U ? 1 : 0) + (D ? 1 : 0);
                    if (deg != 2) continue;
                    if ((L && R && !U && !D) || (U && D && !L && !R)) {
                        cands.push_back({x, y});
                    }
                }
            }

            // Shuffle deterministically.
            for (int i = static_cast<int>(cands.size()) - 1; i > 0; --i) {
                const int j = rng.range(0, i);
                std::swap(cands[static_cast<size_t>(i)], cands[static_cast<size_t>(j)]);
            }

            std::vector<Vec2i> placed;
            placed.reserve(static_cast<size_t>(wantDoors));

            for (const Vec2i& p : cands) {
                if (static_cast<int>(placed.size()) >= wantDoors) break;

                bool near = false;
                for (const Vec2i& prev : placed) {
                    if (chebyshev(p, prev) <= 1) { near = true; break; }
                }
                if (near) continue;

                g.at(p.x, p.y) = 2u;
                placed.push_back(p);
            }
        }

        return true;
    }

    return false;
}

static bool annexGenSmallCavern(AnnexGrid& g, RNG& rng, Vec2i entry, int depth) {
    if (g.w < 11 || g.h < 11) return false;

    // Start with walls; only carve within the interior ring so we always keep a solid border.
    std::fill(g.cell.begin(), g.cell.end(), uint8_t{0});

    // Depth influences density a bit (deeper = a little more closed).
    float floorChance = 0.58f - 0.015f * static_cast<float>(std::clamp(depth - 3, 0, 12));
    floorChance = std::clamp(floorChance, 0.42f, 0.62f);

    for (int y = 1; y < g.h - 1; ++y) {
        for (int x = 1; x < g.w - 1; ++x) {
            g.at(x, y) = rng.chance(floorChance) ? 1u : 0u;
        }
    }

    auto countWalls8 = [&](int x, int y) -> int {
        int w = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = x + dx;
                const int ny = y + dy;
                if (!g.inBounds(nx, ny)) { w += 1; continue; }
                if (g.at(nx, ny) == 0u) w += 1;
            }
        }
        return w;
    };

    // A few smoothing passes (classic CA cave trick).
    for (int it = 0; it < 5; ++it) {
        AnnexGrid next = g;
        for (int y = 1; y < g.h - 1; ++y) {
            for (int x = 1; x < g.w - 1; ++x) {
                const int w8 = countWalls8(x, y);
                // 4-5-ish rule: lots of walls nearby -> wall; otherwise floor.
                next.at(x, y) = (w8 >= 5) ? 0u : 1u;
            }
        }
        g.cell.swap(next.cell);
    }

    // Flood-fill connected components; keep the largest one.
    std::vector<int> comp(static_cast<size_t>(g.w * g.h), -1);
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * g.w + x); };

    int bestComp = -1;
    int bestSize = -1;
    int compId = 0;

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    for (int y = 1; y < g.h - 1; ++y) {
        for (int x = 1; x < g.w - 1; ++x) {
            if (g.at(x, y) == 0u) continue;
            if (comp[idx(x, y)] >= 0) continue;

            int size = 0;
            std::deque<Vec2i> q;
            q.push_back({x, y});
            comp[idx(x, y)] = compId;

            while (!q.empty()) {
                Vec2i p = q.front();
                q.pop_front();
                size += 1;

                for (const auto& dv : dirs) {
                    const int nx = p.x + dv[0];
                    const int ny = p.y + dv[1];
                    if (!g.inBounds(nx, ny)) continue;
                    if (g.at(nx, ny) == 0u) continue;
                    const size_t ii = idx(nx, ny);
                    if (comp[ii] >= 0) continue;
                    comp[ii] = compId;
                    q.push_back({nx, ny});
                }
            }

            if (size > bestSize) {
                bestSize = size;
                bestComp = compId;
            }
            compId += 1;
        }
    }

    if (bestComp < 0) return false;

    // Cull non-largest components.
    for (int y = 1; y < g.h - 1; ++y) {
        for (int x = 1; x < g.w - 1; ++x) {
            if (g.at(x, y) == 0u) continue;
            if (comp[idx(x, y)] != bestComp) g.at(x, y) = 0u;
        }
    }

    // Ensure the entry area is open.
    if (g.inBounds(entry.x, entry.y)) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int nx = entry.x + dx;
                const int ny = entry.y + dy;
                if (nx <= 0 || ny <= 0 || nx >= g.w - 1 || ny >= g.h - 1) continue;
                g.at(nx, ny) = 1u;
            }
        }
        g.at(entry.x, entry.y) = 1u;
    }

    // If entry is not connected to the main component, tunnel to the nearest floor.
    // (This keeps annexes always solvable once the door is opened.)
    {
        // BFS from entry over floors.
        std::vector<int> dist(static_cast<size_t>(g.w * g.h), -1);
        std::deque<Vec2i> q;
        auto didx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * g.w + x); };

        if (g.inBounds(entry.x, entry.y) && g.at(entry.x, entry.y) != 0u) {
            dist[didx(entry.x, entry.y)] = 0;
            q.push_back(entry);
        }

        while (!q.empty()) {
            Vec2i p = q.front();
            q.pop_front();
            const int d0 = dist[didx(p.x, p.y)];
            for (const auto& dv : dirs) {
                const int nx = p.x + dv[0];
                const int ny = p.y + dv[1];
                if (!g.inBounds(nx, ny)) continue;
                if (g.at(nx, ny) == 0u) continue;
                const size_t ii = didx(nx, ny);
                if (dist[ii] >= 0) continue;
                dist[ii] = d0 + 1;
                q.push_back({nx, ny});
            }
        }

        // Find any floor tile not reached.
        Vec2i target{-1, -1};
        int bestMan = 999999;

        for (int y = 1; y < g.h - 1; ++y) {
            for (int x = 1; x < g.w - 1; ++x) {
                if (g.at(x, y) == 0u) continue;
                if (dist[didx(x, y)] >= 0) continue;
                const int man = std::abs(x - entry.x) + std::abs(y - entry.y);
                if (man < bestMan) {
                    bestMan = man;
                    target = {x, y};
                }
            }
        }

        if (g.inBounds(target.x, target.y)) {
            // Drill a simple L-shaped tunnel to connect.
            int x = entry.x;
            int y = entry.y;
            while (x != target.x) {
                x += (target.x > x) ? 1 : -1;
                if (x <= 0 || x >= g.w - 1) break;
                g.at(x, y) = 1u;
            }
            while (y != target.y) {
                y += (target.y > y) ? 1 : -1;
                if (y <= 0 || y >= g.h - 1) break;
                g.at(x, y) = 1u;
            }
        }
    }

    return true;
}

static bool annexGenMiniRuins(AnnexGrid& g, RNG& rng, Vec2i entry, int depth) {
    // A compact "classic roguelike" annex generator:
    // - Pack a handful of rectangular rooms into the pocket.
    // - Connect room centers using a Minimum Spanning Tree-ish graph (Prim),
    //   then sprinkle a couple extra edges to form loops.
    // - Carve 1-tile corridors and mark room exits as internal door candidates (cell==2).
    //
    // This creates a tiny, readable optional side-dungeon that feels different from
    // both the perfect-maze and cellular-cavern annex styles.
    if (g.w < 17 || g.h < 11) return false;

    // Clear to solid walls. We only carve within the interior ring so the pocket always
    // has a solid border when stamped into the main dungeon.
    std::fill(g.cell.begin(), g.cell.end(), uint8_t{0});

    struct Rm {
        int x = 0, y = 0, w = 0, h = 0;
        int x2() const { return x + w; }
        int y2() const { return y + h; }
        Vec2i c() const { return {x + w / 2, y + h / 2}; }
    };

    auto intersectsPadded = [&](const Rm& a, const Rm& b, int pad) -> bool {
        // Treat rooms as expanded by pad so corridors have breathing room.
        return !(a.x2() + pad <= b.x || b.x2() + pad <= a.x || a.y2() + pad <= b.y || b.y2() + pad <= a.y);
    };

    const int area = g.w * g.h;
    int wantRooms = std::clamp(area / 90, 3, 8);
    wantRooms += std::clamp((depth - 5) / 4, 0, 2); // slightly denser deeper
    wantRooms = std::clamp(wantRooms, 3, 9);

    std::vector<Rm> rooms;
    rooms.reserve(12);

    // Room placement (rejection sampling).
    int attempts = wantRooms * 90;
    while (attempts-- > 0 && static_cast<int>(rooms.size()) < wantRooms) {
        // Keep rooms comfortably sized for the pocket.
        int rw = rng.range(4, std::min(10, g.w - 4));
        int rh = rng.range(4, std::min(8,  g.h - 4));

        // Often prefer odd sizes (looks nicer with 1-tile corridors).
        if (rng.chance(0.55f)) {
            if ((rw % 2) == 0) rw += 1;
            if ((rh % 2) == 0) rh += 1;
        }

        rw = std::clamp(rw, 4, g.w - 4);
        rh = std::clamp(rh, 4, g.h - 4);

        const int rx = rng.range(1, g.w - rw - 1);
        const int ry = rng.range(1, g.h - rh - 1);

        Rm r{rx, ry, rw, rh};

        bool ok = true;
        for (const auto& o : rooms) {
            if (intersectsPadded(r, o, 1)) { ok = false; break; }
        }
        if (!ok) continue;

        rooms.push_back(r);

        // Carve the room as floor.
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (x <= 0 || y <= 0 || x >= g.w - 1 || y >= g.h - 1) continue;
                g.at(x, y) = 1u;
            }
        }
    }

    if (static_cast<int>(rooms.size()) < 2) return false;

    auto manDist = [&](int i, int j) -> int {
        const Vec2i a = rooms[static_cast<size_t>(i)].c();
        const Vec2i b = rooms[static_cast<size_t>(j)].c();
        return std::abs(a.x - b.x) + std::abs(a.y - b.y);
    };

    // Build an MST using Prim's algorithm over Manhattan distances.
    const int n = static_cast<int>(rooms.size());
    std::vector<uint8_t> used(static_cast<size_t>(n), uint8_t{0});
    std::vector<int> best(static_cast<size_t>(n), 1'000'000);
    std::vector<int> parent(static_cast<size_t>(n), -1);

    used[0] = 1u;
    best[0] = 0;
    for (int i = 1; i < n; ++i) {
        best[static_cast<size_t>(i)] = manDist(0, i);
        parent[static_cast<size_t>(i)] = 0;
    }

    std::vector<std::pair<int,int>> edges;
    edges.reserve(static_cast<size_t>(n + 4));

    for (int it = 1; it < n; ++it) {
        int v = -1;
        int vBest = 1'000'000;
        for (int i = 0; i < n; ++i) {
            if (used[static_cast<size_t>(i)]) continue;
            const int bi = best[static_cast<size_t>(i)];
            if (bi < vBest) {
                vBest = bi;
                v = i;
            }
        }
        if (v < 0) break;

        used[static_cast<size_t>(v)] = 1u;
        edges.push_back({v, parent[static_cast<size_t>(v)]});

        for (int k = 0; k < n; ++k) {
            if (used[static_cast<size_t>(k)]) continue;
            const int d = manDist(v, k);
            if (d < best[static_cast<size_t>(k)]) {
                best[static_cast<size_t>(k)] = d;
                parent[static_cast<size_t>(k)] = v;
            }
        }
    }

    // Add a couple extra random edges to create loops (keeps it from being a strict tree).
    std::vector<uint8_t> adj(static_cast<size_t>(n * n), uint8_t{0});
    auto setAdj = [&](int a, int b) {
        if (a < 0 || b < 0) return;
        adj[static_cast<size_t>(a * n + b)] = 1u;
        adj[static_cast<size_t>(b * n + a)] = 1u;
    };
    for (const auto& e : edges) setAdj(e.first, e.second);

    int extra = 0;
    if (n >= 4) extra = rng.range(0, std::min(2, n - 3));
    if (n >= 6 && rng.chance(0.55f)) extra += 1;

    int extraTries = 80;
    while (extra > 0 && extraTries-- > 0) {
        const int a = rng.range(0, n - 1);
        const int b = rng.range(0, n - 1);
        if (a == b) continue;
        if (adj[static_cast<size_t>(a * n + b)]) continue;

        // Prefer short-ish loop edges; allow occasional long edge for spice.
        const int d = manDist(a, b);
        const int softMax = (g.w + g.h) / 2;
        if (d > softMax && !rng.chance(0.12f)) continue;

        setAdj(a, b);
        edges.push_back({a, b});
        extra -= 1;
    }

    auto clampInsideRoomAxis = [&](int v, int lo, int hi) -> int {
        // Avoid doors in corners when possible.
        if (hi - lo >= 2) return std::clamp(v, lo + 1, hi - 1);
        return std::clamp(v, lo, hi);
    };

    auto doorPoint = [&](const Rm& r, Vec2i toward) -> Vec2i {
        const Vec2i c = r.c();
        const int dx = toward.x - c.x;
        const int dy = toward.y - c.y;

        // Pick the dominant axis toward the target.
        if (std::abs(dx) >= std::abs(dy)) {
            const int x = (dx >= 0) ? (r.x + r.w - 1) : r.x;
            const int y = clampInsideRoomAxis(toward.y, r.y, r.y + r.h - 1);
            return {x, y};
        } else {
            const int y = (dy >= 0) ? (r.y + r.h - 1) : r.y;
            const int x = clampInsideRoomAxis(toward.x, r.x, r.x + r.w - 1);
            return {x, y};
        }
    };

    auto carveAt = [&](int x, int y) {
        if (x <= 0 || y <= 0 || x >= g.w - 1 || y >= g.h - 1) return;
        if (g.at(x, y) == 0u) g.at(x, y) = 1u;
    };

    auto carveCorridorL = [&](Vec2i a, Vec2i b) {
        // Randomize whether we go X-then-Y or Y-then-X.
        int x = a.x;
        int y = a.y;
        carveAt(x, y);

        const bool xFirst = rng.chance(0.50f);
        if (xFirst) {
            while (x != b.x) {
                x += (b.x > x) ? 1 : -1;
                carveAt(x, y);
            }
            while (y != b.y) {
                y += (b.y > y) ? 1 : -1;
                carveAt(x, y);
            }
        } else {
            while (y != b.y) {
                y += (b.y > y) ? 1 : -1;
                carveAt(x, y);
            }
            while (x != b.x) {
                x += (b.x > x) ? 1 : -1;
                carveAt(x, y);
            }
        }
    };

    // Carve corridors for all edges, and mark the room-side endpoints as internal doors.
    for (const auto& e : edges) {
        const int a = e.first;
        const int b = e.second;
        if (a < 0 || b < 0 || a >= n || b >= n) continue;

        const Vec2i ca = rooms[static_cast<size_t>(a)].c();
        const Vec2i cb = rooms[static_cast<size_t>(b)].c();

        const Vec2i da = doorPoint(rooms[static_cast<size_t>(a)], cb);
        const Vec2i db = doorPoint(rooms[static_cast<size_t>(b)], ca);

        carveCorridorL(da, db);

        if (g.inBounds(da.x, da.y)) g.at(da.x, da.y) = 2u;
        if (g.inBounds(db.x, db.y)) g.at(db.x, db.y) = 2u;
    }

    // Ensure the entry tile is floor.
    if (g.inBounds(entry.x, entry.y) && entry.x > 0 && entry.y > 0 && entry.x < g.w - 1 && entry.y < g.h - 1) {
        if (g.at(entry.x, entry.y) == 0u) g.at(entry.x, entry.y) = 1u;
    }

    // Connect entry to the nearest carved floor tile (other than itself) if needed.
    Vec2i target{-1, -1};
    int bestMan = 999999;
    for (int y = 1; y < g.h - 1; ++y) {
        for (int x = 1; x < g.w - 1; ++x) {
            if (g.at(x, y) == 0u) continue;
            if (x == entry.x && y == entry.y) continue;
            const int man = std::abs(x - entry.x) + std::abs(y - entry.y);
            if (man < bestMan) {
                bestMan = man;
                target = {x, y};
            }
        }
    }

    if (!g.inBounds(target.x, target.y)) return false;
    carveCorridorL(entry, target);

    return true;
}



bool maybeCarveAnnexMicroDungeon(Dungeon& d, RNG& rng, int depth, GenKind g) {
    d.annexCount = 0;
    d.annexKeyGateCount = 0;
    d.annexWfcCount = 0;
    d.annexFractalCount = 0;

    // NOTE: special floors (camp/sokoban/finale) are handled by early returns in Dungeon::generate.

    const int W = d.width;
    const int H = d.height;
    if (W < 40 || H < 30) return false;

    // Decide whether this floor gets an annex.
    float pAny = 0.08f + 0.03f * static_cast<float>(std::clamp(depth - 2, 0, 12));
    if (g == GenKind::Mines) pAny += 0.10f;
    if (g == GenKind::Catacombs) pAny += 0.08f;
    if (g == GenKind::Maze) pAny += 0.05f;
    if (g == GenKind::Cavern) pAny *= 0.75f;
    pAny = std::clamp(pAny, 0.05f, 0.55f);

    if (!rng.chance(pAny)) return false;

    // Distance map from stairs-up helps avoid "free annex right next to stairs".
    const auto distFromUp = bfsDistanceMap(d, d.stairsUp);
    auto distAt = [&](int x, int y) -> int {
        const size_t ii = static_cast<size_t>(y * W + x);
        if (ii >= distFromUp.size()) return -1;
        return distFromUp[ii];
    };

    auto tooCloseToStairs = [&](Vec2i p) -> bool {
        if (d.inBounds(d.stairsUp.x, d.stairsUp.y) && manhattan2(p, d.stairsUp) <= 7) return true;
        if (d.inBounds(d.stairsDown.x, d.stairsDown.y) && manhattan2(p, d.stairsDown) <= 7) return true;
        return false;
    };

    struct Cand {
        Vec2i door;      // wall tile that becomes a door
        Vec2i outside;   // adjacent corridor/room floor
        Vec2i intoDir;   // direction from door -> inside the annex pocket
        int dist = 0;    // dist from stairs-up (proxy for "not too early")
    };

    std::vector<Cand> cands;
    cands.reserve(static_cast<size_t>((W * H) / 24));

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    for (int y = 2; y < H - 2; ++y) {
        for (int x = 2; x < W - 2; ++x) {
            if (d.at(x, y).type != TileType::Wall) continue;

            // Require at least one adjacent floor to serve as an "outside" anchor.
            Vec2i outside{-1, -1};
            Vec2i intoDir{0, 0};
            int found = 0;

            for (const auto& dv : dirs) {
                const int nx = x + dv[0];
                const int ny = y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (d.at(nx, ny).type != TileType::Floor) continue;

                // Avoid attaching annex doors inside special rooms (shops/vaults/etc).
                const Room* rr = findRoomContaining(d, nx, ny);
                if (rr) {
                    if (rr->type == RoomType::Shop) continue;
                    if (rr->type == RoomType::Vault) continue;
                    if (rr->type == RoomType::Secret) continue;
                    if (rr->type == RoomType::Treasure) continue;
                }

                found += 1;
                // Prefer corridor-like anchors (not deep inside rooms) by taking the first hit;
                // later we sort by distance anyway.
                outside = {nx, ny};
                intoDir = {x - nx, y - ny}; // points from outside -> door; annex extends same direction beyond door
                break;
            }

            if (found <= 0) continue;

            const Vec2i doorPos{x, y};
            if (tooCloseToStairs(doorPos) || tooCloseToStairs(outside)) continue;
            if (anyDoorInRadius(d, x, y, 2)) continue;

            const int di = distAt(outside.x, outside.y);
            if (di < 0) continue;
            if (di < 14) continue;

            cands.push_back({doorPos, outside, intoDir, di});
        }
    }

    if (cands.empty()) return false;

    // Prefer far candidates.
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.dist > b.dist;
    });

    auto allWalls = [&](int rx, int ry, int rw, int rh) -> bool {
        for (int yy = ry; yy < ry + rh; ++yy) {
            for (int xx = rx; xx < rx + rw; ++xx) {
                if (!d.inBounds(xx, yy)) return false;
                if (d.at(xx, yy).type != TileType::Wall) return false;
            }
        }
        return true;
    };

    auto chooseDoorType = [&]() -> TileType {
        float secretChance = 0.20f + 0.04f * static_cast<float>(std::clamp(depth - 3, 0, 12));
        float lockedChance = 0.10f + 0.04f * static_cast<float>(std::clamp(depth - 4, 0, 12));
        if (g == GenKind::Mines) lockedChance += 0.08f;
        if (g == GenKind::Catacombs) secretChance += 0.10f;
        secretChance = std::clamp(secretChance, 0.10f, 0.65f);
        lockedChance = std::clamp(lockedChance, 0.05f, 0.55f);

        if (rng.chance(secretChance)) return TileType::DoorSecret;
        if (rng.chance(lockedChance)) return TileType::DoorLocked;
        return TileType::DoorClosed;
    };

    auto chooseStyle = [&]() -> AnnexStyle {
        // Contrast the main floor generator a bit for variety.
        // MiniWfc is a constraint-driven annex that tends to read as "manufactured".
        float pWfc = 0.07f + 0.03f * static_cast<float>(std::clamp(depth - 5, 0, 12));
        if (g == GenKind::Catacombs) pWfc += 0.16f;
        if (g == GenKind::RoomsGraph) pWfc += 0.10f;
        if (g == GenKind::RoomsBsp) pWfc += 0.08f;
        if (g == GenKind::Mines) pWfc += 0.05f;
        if (g == GenKind::Maze) pWfc -= 0.10f;
        if (g == GenKind::Cavern || g == GenKind::Warrens) pWfc -= 0.12f;
        pWfc = std::clamp(pWfc, 0.03f, 0.45f);

        if (rng.chance(pWfc)) return AnnexStyle::MiniWfc;

        // MiniFractal is a branchy L-system annex; it reads well on organic floors and
        // adds dense micro-choices without feeling like a strict maze.
        float pFractal = 0.08f + 0.02f * static_cast<float>(std::clamp(depth - 4, 0, 12));
        if (g == GenKind::Cavern || g == GenKind::Warrens) pFractal += 0.16f;
        if (g == GenKind::Catacombs) pFractal += 0.06f;
        if (g == GenKind::RoomsGraph) pFractal -= 0.04f;
        if (g == GenKind::RoomsBsp) pFractal -= 0.02f;
        if (g == GenKind::Maze) pFractal -= 0.10f;
        if (g == GenKind::Mines) pFractal -= 0.03f;
        pFractal = std::clamp(pFractal, 0.03f, 0.38f);

        if (rng.chance(pFractal)) return AnnexStyle::MiniFractal;

        // MiniRuins is a packed-rooms annex; it fits best on more structured floors.
        float pRuins = 0.18f + 0.03f * static_cast<float>(std::clamp(depth - 4, 0, 12));
        if (g == GenKind::Catacombs) pRuins += 0.14f;
        if (g == GenKind::RoomsGraph) pRuins += 0.10f;
        if (g == GenKind::RoomsBsp) pRuins += 0.06f;
        if (g == GenKind::Mines) pRuins += 0.04f;
        if (g == GenKind::Maze) pRuins -= 0.08f;
        if (g == GenKind::Cavern || g == GenKind::Warrens) pRuins -= 0.10f;
        pRuins = std::clamp(pRuins, 0.05f, 0.55f);

        if (rng.chance(pRuins)) return AnnexStyle::MiniRuins;

        if (g == GenKind::Cavern || g == GenKind::Warrens) {
            return rng.chance(0.65f) ? AnnexStyle::MiniMaze : AnnexStyle::MiniCavern;
        }
        if (g == GenKind::Maze) {
            return rng.chance(0.70f) ? AnnexStyle::MiniCavern : AnnexStyle::MiniMaze;
        }
        return rng.chance(0.62f) ? AnnexStyle::MiniMaze : AnnexStyle::MiniCavern;
    };

    auto makeOdd = [&](int v) -> int { return (v % 2 == 0) ? (v + 1) : v; };

    auto tryCarveAt = [&](const Cand& c) -> bool {
        AnnexStyle style = chooseStyle();

        // Pick pocket size. Keep odd sizes for maze-friendly grids.
        int len = rng.range(17, (depth >= 9 ? 29 : 25));
        int span = rng.range(11, (depth >= 9 ? 19 : 17));
        len = makeOdd(len);
        span = makeOdd(span);

        // Slightly larger pockets for caverns (they look better when roomy).
        if (style == AnnexStyle::MiniCavern) {
            len = makeOdd(len + 2);
            span = makeOdd(span + 2);
        }

        // MiniWfc benefits from a touch more area so junction chambers have room.
        if (style == AnnexStyle::MiniWfc) {
            len = makeOdd(len + 3);
            span = makeOdd(span + 1);
        }

        // MiniFractal wants a bit more breathing room so the branching pattern has space.
        if (style == AnnexStyle::MiniFractal) {
            len = makeOdd(len + 3);
            span = makeOdd(span + 2);
        }

        // Mini-ruins wants a bit more room for multiple chambers.
        if (style == AnnexStyle::MiniRuins) {
            len = makeOdd(len + 4);
            span = makeOdd(span + 2);
        }

        // Clamp to sane ranges.
        len = std::clamp(len, 17, 31);
        span = std::clamp(span, 11, 21);

        // Decide door offset along the perpendicular span; ensure it's odd so that
        // the entry tile lands on an odd coordinate for maze generation.
        int off = span / 2;
        if ((off % 2) == 0) off += rng.chance(0.5f) ? 1 : -1;
        off = std::clamp(off, 1, span - 2);
        if ((off % 2) == 0) off = (off == span - 2) ? (off - 1) : (off + 1);

        int rx = 0, ry = 0, rw = 0, rh = 0;

        if (c.intoDir.x != 0) {
            rw = len;
            rh = span;
            ry = c.door.y - off;
            rx = (c.intoDir.x > 0) ? c.door.x : (c.door.x - rw + 1);
        } else {
            rw = span;
            rh = len;
            rx = c.door.x - off;
            ry = (c.intoDir.y > 0) ? c.door.y : (c.door.y - rh + 1);
        }

        // Keep a safe border margin.
        if (rx <= 1 || ry <= 1 || (rx + rw) >= W - 1 || (ry + rh) >= H - 1) return false;

        // Ensure we don't overlap existing geometry.
        if (!allWalls(rx, ry, rw, rh)) return false;

        const Vec2i doorInside{c.door.x + c.intoDir.x, c.door.y + c.intoDir.y};
        if (!d.inBounds(doorInside.x, doorInside.y)) return false;

        // Entry relative to pocket origin.
        const Vec2i entryRel{doorInside.x - rx, doorInside.y - ry};
        if (!entryRel.x || !entryRel.y || entryRel.x >= rw - 1 || entryRel.y >= rh - 1) return false;

        AnnexGrid grid;
        grid.w = rw;
        grid.h = rh;
        grid.cell.assign(static_cast<size_t>(rw * rh), uint8_t{0});

        bool ok = false;
        if (style == AnnexStyle::MiniMaze) ok = annexGenPerfectMaze(grid, rng, entryRel);
        else if (style == AnnexStyle::MiniCavern) ok = annexGenSmallCavern(grid, rng, entryRel, depth);
        else if (style == AnnexStyle::MiniRuins) ok = annexGenMiniRuins(grid, rng, entryRel, depth);
        else if (style == AnnexStyle::MiniFractal) ok = annexGenFractalAnnex(grid, rng, entryRel, depth);
        else ok = annexGenWfcAnnex(grid, rng, entryRel, depth);

        if (!ok) return false;

        const TileType entranceDoorType = chooseDoorType();

        // Optional: some annexes get an internal lock-and-key micro-puzzle.
        // We keep this rare, and avoid stacking it on top of a locked entrance door so the
        // annex doesn't demand multiple keys.
        AnnexKeyGatePlan keyGate;
        bool hasKeyGate = false;

        // Default reward location: farthest floor tile from the entry within the annex grid.
        // If an internal key-gate is planned, we will override this with a gated-side target.
        Vec2i farRel = annexFarthestFloor(grid, entryRel);

        if (entranceDoorType != TileType::DoorLocked && (style == AnnexStyle::MiniMaze || style == AnnexStyle::MiniRuins || style == AnnexStyle::MiniWfc || style == AnnexStyle::MiniFractal)) {
            float pGate = 0.08f + 0.03f * static_cast<float>(std::clamp(depth - 5, 0, 12));
            if (style == AnnexStyle::MiniMaze) pGate += 0.08f;
            if (style == AnnexStyle::MiniWfc) pGate += 0.05f;
            if (style == AnnexStyle::MiniFractal) pGate += 0.07f;
            if (g == GenKind::Catacombs) pGate += 0.06f;
            if (g == GenKind::Mines) pGate += 0.04f;
            pGate = std::clamp(pGate, 0.05f, 0.40f);

            if (rng.chance(pGate)) {
                hasKeyGate = annexPlanKeyGate(grid, rng, entryRel, keyGate);
                if (hasKeyGate && grid.inBounds(keyGate.loot.x, keyGate.loot.y)) {
                    farRel = keyGate.loot;
                }
            }
        }

        // Stamp the interior floors into the dungeon; keep the pocket border ring as solid wall.
        for (int yy = 1; yy < rh - 1; ++yy) {
            for (int xx = 1; xx < rw - 1; ++xx) {
                if (grid.at(xx, yy) == 0u) continue;
                carveFloor(d, rx + xx, ry + yy);
            }
        }
        // Mini-ruins (+WFC annexes): translate internal door markers (cell==2) into actual door tiles.
        if (style == AnnexStyle::MiniRuins || style == AnnexStyle::MiniWfc) {
            const float pOpen = (style == AnnexStyle::MiniRuins) ? 0.35f : 0.25f;
            for (int yy = 1; yy < rh - 1; ++yy) {
                for (int xx = 1; xx < rw - 1; ++xx) {
                    if (grid.at(xx, yy) != 2u) continue;

                    const int gx = rx + xx;
                    const int gy = ry + yy;
                    if (!d.inBounds(gx, gy)) continue;

                    // Only place doors on plain floor (avoid overwriting special overlays).
                    if (d.at(gx, gy).type != TileType::Floor) continue;

                    // Some doors are broken open to sell the "ruins" / "bulkhead" vibe.
                    d.at(gx, gy).type = rng.chance(pOpen) ? TileType::DoorOpen : TileType::DoorClosed;
                }
            }
        }


        // Place the entrance door tile.
        d.at(c.door.x, c.door.y).type = entranceDoorType;

        // Apply the internal key gate (locked door + key) if we planned one.
        if (hasKeyGate) {
            bool applied = false;

            const Vec2i gate{rx + keyGate.gate.x, ry + keyGate.gate.y};
            if (d.inBounds(gate.x, gate.y) && d.at(gate.x, gate.y).type == TileType::Floor) {
                d.at(gate.x, gate.y).type = TileType::DoorLocked;
                applied = true;
            }

            const Vec2i keyPos{rx + keyGate.key.x, ry + keyGate.key.y};
            if (d.inBounds(keyPos.x, keyPos.y) && d.at(keyPos.x, keyPos.y).type == TileType::Floor) {
                d.bonusItemSpawns.push_back({keyPos, ItemKind::Key, 1});
                applied = true;
            }

            if (applied) d.annexKeyGateCount += 1;
        }

        // Reward: a chest deep inside the annex (spawns via bonusLootSpots).
        // Default: farthest reachable floor tile from the entry.
        // If an internal key gate is present, farRel was overridden to target the gated side.
        if (grid.inBounds(farRel.x, farRel.y)) {
            const Vec2i loot{rx + farRel.x, ry + farRel.y};
            if (d.inBounds(loot.x, loot.y) && d.at(loot.x, loot.y).type == TileType::Floor) {
                d.bonusLootSpots.push_back(loot);
                // Deeper floors sometimes get a second cache.
                if (depth >= 7 && rng.chance(0.35f)) {
                    // Nudge toward another distant-ish tile by stepping a bit.
                    Vec2i alt = farRel;
                    for (int k = 0; k < 20; ++k) {
                        const int dd = rng.range(0, 3);
                        const int nx = alt.x + dirs[dd][0];
                        const int ny = alt.y + dirs[dd][1];
                        if (!grid.inBounds(nx, ny)) continue;
                        if (grid.at(nx, ny) == 0u) continue;
                        alt = {nx, ny};
                    }
                    const Vec2i loot2{rx + alt.x, ry + alt.y};
                    if (d.inBounds(loot2.x, loot2.y) && d.at(loot2.x, loot2.y).type == TileType::Floor) {
                        if (manhattan2(loot2, loot) >= 6) d.bonusLootSpots.push_back(loot2);
                    }
                }
            }
        }

        d.annexCount += 1;
        if (style == AnnexStyle::MiniWfc) d.annexWfcCount += 1;
        if (style == AnnexStyle::MiniFractal) d.annexFractalCount += 1;
        return true;
    };

    // Try a handful of candidates.
    const int maxTries = std::min(40, static_cast<int>(cands.size()));
    for (int i = 0; i < maxTries; ++i) {
        const Cand& c = cands[static_cast<size_t>(i)];
        if (tryCarveAt(c)) return true;
    }

    return false;
}

bool maybeCarveDeadEndClosets(Dungeon& d, RNG& rng, int depth, GenKind g) {
    d.deadEndClosetCount = 0;

    // Skip on cavern floors: organic caves already have lots of pockets and
    // carving rectangular closets tends to look unnatural.
    if (g == GenKind::Cavern) return false;

    const int W = d.width;
    const int H = d.height;
    if (W <= 4 || H <= 4) return false;

    // Build an "in room" mask so we only consider corridor/tunnel dead-ends.
    std::vector<uint8_t> inRoom(static_cast<size_t>(W * H), uint8_t{0});
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(y * W + x)] = 1u;
            }
        }
    }

    auto inAnyRoom = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        return inRoom[static_cast<size_t>(y * W + x)] != 0u;
    };

    const auto distFromUp = bfsDistanceMap(d, d.stairsUp);

    auto distAt = [&](int x, int y) -> int {
        const size_t ii = static_cast<size_t>(y * W + x);
        if (ii >= distFromUp.size()) return -1;
        return distFromUp[ii];
    };

    auto tooCloseToStairs = [&](int x, int y) -> bool {
        const Vec2i p{x, y};
        if (d.inBounds(d.stairsUp.x, d.stairsUp.y) && manhattan2(p, d.stairsUp) <= 6) return true;
        if (d.inBounds(d.stairsDown.x, d.stairsDown.y) && manhattan2(p, d.stairsDown) <= 6) return true;
        return false;
    };

    struct Cand {
        Vec2i end;   // corridor dead-end floor tile
        Vec2i dir;   // outward direction into wall (unit)
        int dist = 0;
    };

    std::vector<Cand> cands;
    cands.reserve(static_cast<size_t>((W * H) / 16));

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (inAnyRoom(x, y)) continue;
            if (tooCloseToStairs(x, y)) continue;

            // A corridor dead-end is a floor tile with exactly one passable neighbor.
            int passN = 0;
            Vec2i back{-999, -999};

            for (const auto& dv : dirs) {
                const int nx = x + dv[0];
                const int ny = y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (!d.isPassable(nx, ny)) continue;
                passN += 1;
                back = {nx, ny};
            }

            if (passN != 1) continue;

            // Ensure the "back" tile is also corridor floor (not a door/room boundary).
            if (!d.inBounds(back.x, back.y)) continue;
            if (inAnyRoom(back.x, back.y)) continue;
            if (d.at(back.x, back.y).type != TileType::Floor) continue;

            // Outward direction points into the wall we can convert into a door.
            const Vec2i dir{x - back.x, y - back.y};
            const Vec2i doorPos{x + dir.x, y + dir.y};

            if (!d.inBounds(doorPos.x, doorPos.y)) continue;
            // Keep a margin so we don't carve into the border ring.
            if (doorPos.x <= 1 || doorPos.y <= 1 || doorPos.x >= W - 2 || doorPos.y >= H - 2) continue;
            if (d.at(doorPos.x, doorPos.y).type != TileType::Wall) continue;

            // Avoid door clusters (including special doors).
            if (anyDoorInRadius(d, doorPos.x, doorPos.y, 1)) continue;

            const int di = distAt(x, y);
            if (di < 0) continue;
            // Don't place stashes too early (avoid "free chest next to stairs").
            if (di < 10) continue;

            cands.push_back({{x, y}, dir, di});
        }
    }

    if (cands.empty()) return false;

    // Decide whether this floor gets closets.
    float pAny = 0.30f + 0.05f * static_cast<float>(std::clamp(depth - 1, 0, 10));
    // Mines & catacombs are exploration-heavy: closets fit them well.
    if (g == GenKind::Mines) pAny = 1.0f;
    else if (g == GenKind::Catacombs) pAny = std::min(0.92f, pAny + 0.15f);
    else if (g == GenKind::Maze) pAny = std::max(0.18f, pAny * 0.75f);
    pAny = std::clamp(pAny, 0.15f, 1.0f);

    if (!rng.chance(pAny)) return false;

    // Prefer far dead-ends.
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.dist > b.dist;
    });

    int want = 1;
    if (depth >= 5 && rng.chance(0.45f)) want += 1;
    if (g == GenKind::Mines && depth >= 2 && rng.chance(0.45f)) want += 1;
    // Keep the chest count sane.
    want = std::clamp(want, 1, 2);
    want = std::min(want, static_cast<int>(cands.size()));

    std::vector<Vec2i> placedEnds;
    placedEnds.reserve(static_cast<size_t>(want));

    auto farFromOtherClosets = [&](Vec2i p) -> bool {
        for (const Vec2i& q : placedEnds) {
            if (manhattan2(p, q) <= 10) return false;
        }
        return true;
    };

    auto allWalls = [&](int rx, int ry, int rw, int rh) -> bool {
        for (int yy = ry; yy < ry + rh; ++yy) {
            for (int xx = rx; xx < rx + rw; ++xx) {
                if (!d.inBounds(xx, yy)) return false;
                if (d.at(xx, yy).type != TileType::Wall) return false;
            }
        }
        return true;
    };

    auto tryCarveCloset = [&](const Cand& c, bool secretDoor) -> bool {
        const Vec2i doorPos{c.end.x + c.dir.x, c.end.y + c.dir.y};

        // Closet dimensions: a small rectangle.
        int len = rng.range(3, (depth >= 7 ? 6 : 5));
        int span = rng.chance(0.60f) ? 3 : 5;

        // Keep span odd so the closet centers on the door axis.
        if ((span % 2) == 0) span += 1;

        int rx = 0, ry = 0, rw = 0, rh = 0;

        if (c.dir.x != 0) {
            // Horizontal extension.
            rw = len;
            rh = span;
            ry = doorPos.y - span / 2;
            rx = (c.dir.x > 0) ? (doorPos.x + 1) : (doorPos.x - len);
        } else {
            // Vertical extension.
            rw = span;
            rh = len;
            rx = doorPos.x - span / 2;
            ry = (c.dir.y > 0) ? (doorPos.y + 1) : (doorPos.y - len);
        }

        // Bounds + border margin.
        if (rx <= 1 || ry <= 1 || (rx + rw) >= W - 1 || (ry + rh) >= H - 1) return false;

        // Avoid carving into existing geometry.
        if (!allWalls(rx, ry, rw, rh)) return false;

        // Carve the closet interior.
        carveRect(d, rx, ry, rw, rh, TileType::Floor);

        // Place door tile in the wall.
        d.at(doorPos.x, doorPos.y).type = secretDoor ? TileType::DoorSecret : TileType::DoorClosed;

        // Light "clutter" for texture: one pillar or boulder, but never on the entry tile.
        const Vec2i entry{doorPos.x + c.dir.x, doorPos.y + c.dir.y};
        if (d.inBounds(entry.x, entry.y) && rng.chance(0.35f)) {
            for (int tries = 0; tries < 40; ++tries) {
                const int xx = rng.range(rx, rx + rw - 1);
                const int yy = rng.range(ry, ry + rh - 1);
                if (xx == entry.x && yy == entry.y) continue;
                if (d.at(xx, yy).type != TileType::Floor) continue;

                const bool usePillar = (depth >= 6 && rng.chance(0.45f));
                d.at(xx, yy).type = usePillar ? TileType::Pillar : TileType::Boulder;
                break;
            }
        }

        // Bonus cache: usually a chest deep inside the closet (spawned via bonusLootSpots).
        // If we don't roll a chest, sometimes leave a small utility item (key/lockpick/gold).
        // Secret closets are slightly more likely to be rewarding.
        const float chestChance = secretDoor ? 0.92f : 0.78f;

        Vec2i best{-1, -1};
        int bestScore = -1;

        for (int yy = ry; yy < ry + rh; ++yy) {
            for (int xx = rx; xx < rx + rw; ++xx) {
                if (!d.inBounds(xx, yy)) continue;
                if (d.at(xx, yy).type != TileType::Floor) continue;

                const int score = std::abs(xx - entry.x) + std::abs(yy - entry.y);
                if (score > bestScore) {
                    bestScore = score;
                    best = {xx, yy};
                } else if (score == bestScore && bestScore >= 0 && rng.chance(0.35f)) {
                    best = {xx, yy};
                }
            }
        }

        if (d.inBounds(best.x, best.y)) {
            if (rng.chance(chestChance)) {
                d.bonusLootSpots.push_back(best);
            } else {
                // Occasionally: a little "tools" drop so dead-end exploration still pays off.
                const float toolChance = secretDoor ? 0.55f : 0.35f;
                if (rng.chance(toolChance)) {
                    ItemKind kind = ItemKind::Gold;
                    int count = rng.range(15, 45);

                    if (depth >= 4 && rng.chance(0.45f)) {
                        kind = ItemKind::Lockpick;
                        count = 1;
                    } else if (rng.chance(0.35f)) {
                        kind = ItemKind::Key;
                        count = 1;
                    }

                    d.bonusItemSpawns.push_back({best, kind, count});
                }
            }
        }

        d.deadEndClosetCount += 1;
        return true;
    };

    int placed = 0;

    // Try farthest candidates first; allow a few failures before giving up.
    for (const Cand& c : cands) {
        if (placed >= want) break;
        if (!farFromOtherClosets(c.end)) continue;

        // Early floors: mostly visible closet doors.
        // Deeper floors: increase secret-door closets.
        float secretChance = 0.10f + 0.05f * static_cast<float>(std::clamp(depth - 2, 0, 10));
        if (g == GenKind::Mines) secretChance += 0.10f;
        if (g == GenKind::Maze) secretChance += 0.05f;
        secretChance = std::clamp(secretChance, 0.08f, 0.55f);

        const bool secretDoor = rng.chance(secretChance);

        if (tryCarveCloset(c, secretDoor)) {
            placedEnds.push_back(c.end);
            placed += 1;
        }
    }

    return d.deadEndClosetCount > 0;
}

// ------------------------------------------------------------
// Vault prefabs: small hand-authored set pieces carved into solid wall
// pockets off corridors. Inspired by "vault" / "room" prefabs common in
// classic roguelikes, but kept strictly optional so they never gate the
// main stairs-to-stairs critical path.
// ------------------------------------------------------------
// Prefab definition data lives in vault_prefab_catalog.* (shared with unit tests).
using PrefabDef = VaultPrefabDef;

struct PrefabVariant {
    const PrefabDef* def = nullptr;
    int w = 0;
    int h = 0;
    std::vector<std::string> grid;
    int doorX = 0;
    int doorY = 0;
    Vec2i outsideDir{0, 0}; // direction from door -> corridor
    char doorChar = '+';
    bool valid = false;
};

static inline bool isPrefabDoorChar(char c) {
    return c == '+' || c == 's' || c == 'L';
}

static std::vector<std::string> buildPrefabBase(const PrefabDef& def) {
    std::vector<std::string> base;
    base.reserve(static_cast<size_t>(def.h));
    for (int y = 0; y < def.h; ++y) {
        base.emplace_back(def.rows[y]);
    }
    return base;
}

static std::vector<std::string> transformPrefabGrid(const std::vector<std::string>& base,
                                                    int w, int h,
                                                    int rot90cw, bool mirrorX,
                                                    int& outW, int& outH) {
    const int r = rot90cw & 3;
    outW = (r % 2 == 0) ? w : h;
    outH = (r % 2 == 0) ? h : w;

    std::vector<std::string> out(static_cast<size_t>(outH),
                                 std::string(static_cast<size_t>(outW), '#'));

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const char c = base[static_cast<size_t>(y)][static_cast<size_t>(x)];
            const int mx = mirrorX ? (w - 1 - x) : x;
            const int my = y;

            int ox = 0;
            int oy = 0;
            switch (r) {
                case 0: ox = mx;              oy = my;              break;
                case 1: ox = (outW - 1 - my); oy = mx;              break;
                case 2: ox = (outW - 1 - mx); oy = (outH - 1 - my); break;
                case 3: ox = my;              oy = (outH - 1 - mx); break;
                default: break;
            }

            if (oy >= 0 && oy < outH && ox >= 0 && ox < outW) {
                out[static_cast<size_t>(oy)][static_cast<size_t>(ox)] = c;
            }
        }
    }

    return out;
}

static bool buildPrefabVariant(const PrefabDef& def,
                               const std::vector<std::string>& base,
                               int rot90cw, bool mirrorX,
                               PrefabVariant& out) {
    int w2 = 0;
    int h2 = 0;
    auto g = transformPrefabGrid(base, def.w, def.h, rot90cw, mirrorX, w2, h2);

    // Validate: boundary must be solid wall except for exactly ONE entrance door.
    int doorCount = 0;
    int doorX = -1;
    int doorY = -1;
    Vec2i outside{0, 0};
    char doorChar = '+';

    for (int y = 0; y < h2; ++y) {
        for (int x = 0; x < w2; ++x) {
            const bool boundary = (x == 0 || y == 0 || x == (w2 - 1) || y == (h2 - 1));
            if (!boundary) continue;

            const char c = g[static_cast<size_t>(y)][static_cast<size_t>(x)];

            if (isPrefabDoorChar(c)) {
                // Avoid corner doors (ambiguous outside direction).
                if ((x == 0 || x == (w2 - 1)) && (y == 0 || y == (h2 - 1))) return false;

                doorCount += 1;
                doorX = x;
                doorY = y;
                doorChar = c;

                if (x == 0) outside = {-1, 0};
                else if (x == (w2 - 1)) outside = {1, 0};
                else if (y == 0) outside = {0, -1};
                else outside = {0, 1};
            } else if (c != '#') {
                // Any boundary floor/chasm/etc would create unintended extra connections.
                return false;
            }
        }
    }

    if (doorCount != 1) return false;
    if (doorX < 0 || doorY < 0) return false;

    out.def = &def;
    out.w = w2;
    out.h = h2;
    out.grid = std::move(g);
    out.doorX = doorX;
    out.doorY = doorY;
    out.outsideDir = outside;
    out.doorChar = doorChar;
    out.valid = true;
    return true;
}

static bool applyPrefabAt(Dungeon& d, const PrefabVariant& v, int x0, int y0) {
    if (!v.valid) return false;
    if (x0 < 1 || y0 < 1) return false;
    if ((x0 + v.w) > (d.width - 1) || (y0 + v.h) > (d.height - 1)) return false;

    auto tooCloseToStairs = [&](int x, int y) -> bool {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y))
            : 9999;
        return du <= 2 || dd <= 2;
    };

    // Validate target area: must be solid wall and must not carve too close to stairs.
    for (int yy = 0; yy < v.h; ++yy) {
        for (int xx = 0; xx < v.w; ++xx) {
            const int wx = x0 + xx;
            const int wy = y0 + yy;
            if (!d.inBounds(wx, wy)) return false;

            const char c = v.grid[static_cast<size_t>(yy)][static_cast<size_t>(xx)];
            if (c != '#' && tooCloseToStairs(wx, wy)) return false;

            if (d.at(wx, wy).type != TileType::Wall) return false;
        }
    }

    // Apply: carve floors/doors/features into the wall pocket.
    for (int yy = 0; yy < v.h; ++yy) {
        for (int xx = 0; xx < v.w; ++xx) {
            const int wx = x0 + xx;
            const int wy = y0 + yy;

            const char c = v.grid[static_cast<size_t>(yy)][static_cast<size_t>(xx)];
            if (c == '#') continue;

            switch (c) {
                case '.':
                    d.at(wx, wy).type = TileType::Floor;
                    break;
                case '+':
                    d.at(wx, wy).type = TileType::DoorClosed;
                    break;
                case 's':
                    d.at(wx, wy).type = TileType::DoorSecret;
                    break;
                case 'L':
                    d.at(wx, wy).type = TileType::DoorLocked;
                    break;
                case 'C':
                    d.at(wx, wy).type = TileType::Chasm;
                    break;
                case 'P':
                    d.at(wx, wy).type = TileType::Pillar;
                    break;
                case 'O':
                    d.at(wx, wy).type = TileType::Boulder;
                    break;
                case 'T':
                    d.at(wx, wy).type = TileType::Floor;
                    d.bonusLootSpots.push_back({wx, wy});
                    break;
                case 'K':
                    d.at(wx, wy).type = TileType::Floor;
                    d.bonusItemSpawns.push_back({{wx, wy}, ItemKind::Key, 1});
                    break;
                case 'R':
                    d.at(wx, wy).type = TileType::Floor;
                    d.bonusItemSpawns.push_back({{wx, wy}, ItemKind::Lockpick, 1});
                    break;
                default:
                    // Unknown char: treat as wall (no-op).
                    break;
            }
        }
    }

    return true;
}

static bool tryPlaceVaultPrefab(Dungeon& d, RNG& rng, int depth, const PrefabDef& def) {
    (void)depth;

    const auto base = buildPrefabBase(def);

    std::vector<PrefabVariant> variants;
    variants.reserve(8);
    for (int rot = 0; rot < 4; ++rot) {
        for (int mirror = 0; mirror < 2; ++mirror) {
            PrefabVariant v;
            if (buildPrefabVariant(def, base, rot, mirror != 0, v)) {
                variants.push_back(std::move(v));
            }
        }
    }
    if (variants.empty()) return false;

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    auto tooCloseToStairsDoor = [&](int x, int y) -> bool {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y))
            : 9999;
        return du <= 4 || dd <= 4;
    };

    const int maxTries = 500;
    for (int tries = 0; tries < maxTries; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;
        if (tooCloseToStairsDoor(x, y)) continue;

        // Avoid clustering doors; these prefabs should feel like tucked-away finds.
        if (anyDoorInRadius(d, x, y, 2)) continue;

        int adjFloors = 0;
        Vec2i outDir{0, 0}; // direction from door -> corridor floor
        for (const Vec2i& dv : dirs) {
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Floor) {
                adjFloors += 1;
                outDir = dv;
            }
        }
        if (adjFloors != 1) continue;

        const int fx = x + outDir.x;
        const int fy = y + outDir.y;
        if (!d.inBounds(fx, fy)) continue;
        if (d.at(fx, fy).type != TileType::Floor) continue;

        // Avoid attaching directly into special rooms (shops/shrines/etc).
        if (const Room* rr = findRoomContaining(d, fx, fy)) {
            if (rr->type != RoomType::Normal) continue;
        }

        // Pick a variant whose entrance faces the corridor direction.
        std::vector<int> matches;
        matches.reserve(variants.size());
        for (int i = 0; i < static_cast<int>(variants.size()); ++i) {
            if (variants[static_cast<size_t>(i)].outsideDir == outDir) matches.push_back(i);
        }
        if (matches.empty()) continue;

        // Try all matching variants in a random cycle (in case size/fit differs).
        const int start = rng.range(0, static_cast<int>(matches.size()) - 1);
        for (int k = 0; k < static_cast<int>(matches.size()); ++k) {
            const PrefabVariant& v = variants[static_cast<size_t>(matches[static_cast<size_t>((start + k) % matches.size())])];

            const int x0 = x - v.doorX;
            const int y0 = y - v.doorY;

            if (applyPrefabAt(d, v, x0, y0)) return true;
        }
    }

    return false;
}


// ------------------------------------------------------------
// Procedural vault prefab: a tiny perfect-maze cache
//
// We already have a small library of hand-authored vault templates above.
// This generator adds a *dynamic* option that creates a solvable micro-maze
// pocket carved into solid wall. It is always optional and never gates the
// stairs path.
//
// Technique: classic recursive-backtracker perfect maze on a coarse cell grid,
// then we place a treasure marker at the farthest reachable dead-end.
// ------------------------------------------------------------
static bool buildPrefabVariantFromGrid(const std::vector<std::string>& base,
                                      int w, int h,
                                      int rot90cw, bool mirrorX,
                                      PrefabVariant& out) {
    int w2 = 0;
    int h2 = 0;
    auto g = transformPrefabGrid(base, w, h, rot90cw, mirrorX, w2, h2);

    // Validate: boundary must be solid wall except for exactly ONE entrance door.
    int doorCount = 0;
    int doorX = -1;
    int doorY = -1;
    Vec2i outside{0, 0};
    char doorChar = '+';

    for (int y = 0; y < h2; ++y) {
        for (int x = 0; x < w2; ++x) {
            const bool boundary = (x == 0 || y == 0 || x == (w2 - 1) || y == (h2 - 1));
            if (!boundary) continue;

            const char c = g[static_cast<size_t>(y)][static_cast<size_t>(x)];

            if (isPrefabDoorChar(c)) {
                // Avoid corner doors (ambiguous outside direction).
                if ((x == 0 || x == (w2 - 1)) && (y == 0 || y == (h2 - 1))) return false;

                doorCount += 1;
                doorX = x;
                doorY = y;
                doorChar = c;

                if (x == 0) outside = {-1, 0};
                else if (x == (w2 - 1)) outside = {1, 0};
                else if (y == 0) outside = {0, -1};
                else outside = {0, 1};
            } else if (c != '#') {
                // Any boundary floor/chasm/etc would create unintended extra connections.
                return false;
            }
        }
    }

    if (doorCount != 1) return false;
    if (doorX < 0 || doorY < 0) return false;

    out.def = nullptr;
    out.w = w2;
    out.h = h2;
    out.grid = std::move(g);
    out.doorX = doorX;
    out.doorY = doorY;
    out.outsideDir = outside;
    out.doorChar = doorChar;
    out.valid = true;
    return true;
}

static std::vector<std::string> makeMazeVaultBase(RNG& rng, int w, int h, char doorChar, int depth,
                                                 int& outDoorX, int& outDoorY) {
    std::vector<std::string> g(static_cast<size_t>(h), std::string(static_cast<size_t>(w), '#'));

    // Entrance door on the bottom edge in the base orientation.
    outDoorY = h - 1;
    outDoorX = w / 2;
    if ((outDoorX & 1) == 0) outDoorX = std::clamp(outDoorX - 1, 1, w - 2);
    outDoorX = std::clamp(outDoorX, 1, w - 2);
    g[static_cast<size_t>(outDoorY)][static_cast<size_t>(outDoorX)] = doorChar;

    // Coarse cell grid: cell centers live on odd coordinates.
    const int cellsX = std::max(1, (w - 1) / 2);
    const int cellsY = std::max(1, (h - 1) / 2);

    std::vector<uint8_t> vis(static_cast<size_t>(cellsX * cellsY), uint8_t{0});

    auto cidx = [&](int cx, int cy) -> size_t { return static_cast<size_t>(cy * cellsX + cx); };
    auto cellX = [&](int cx) -> int { return 1 + cx * 2; };
    auto cellY = [&](int cy) -> int { return 1 + cy * 2; };

    auto carveCell = [&](int cx, int cy) {
        const int x = cellX(cx);
        const int y = cellY(cy);
        if (x >= 1 && x < (w - 1) && y >= 1 && y < (h - 1)) {
            g[static_cast<size_t>(y)][static_cast<size_t>(x)] = '.';
        }
    };

    auto carveBetween = [&](int ax, int ay, int bx, int by) {
        const int x1 = cellX(ax);
        const int y1 = cellY(ay);
        const int x2 = cellX(bx);
        const int y2 = cellY(by);
        const int wx = (x1 + x2) / 2;
        const int wy = (y1 + y2) / 2;
        if (wx >= 1 && wx < (w - 1) && wy >= 1 && wy < (h - 1)) {
            g[static_cast<size_t>(wy)][static_cast<size_t>(wx)] = '.';
        }
    };

    // Start from the cell directly inside the entrance.
    const int startCx = std::clamp((outDoorX - 1) / 2, 0, cellsX - 1);
    const int startCy = std::clamp(cellsY - 1, 0, cellsY - 1);

    std::vector<std::pair<int, int>> st;
    st.reserve(static_cast<size_t>(cellsX * cellsY));

    st.push_back({startCx, startCy});
    vis[cidx(startCx, startCy)] = 1;
    carveCell(startCx, startCy);

    const int dirs[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    while (!st.empty()) {
        const int cx = st.back().first;
        const int cy = st.back().second;

        int opts[4];
        int optN = 0;
        for (int di = 0; di < 4; ++di) {
            const int nx = cx + dirs[di][0];
            const int ny = cy + dirs[di][1];
            if (nx < 0 || ny < 0 || nx >= cellsX || ny >= cellsY) continue;
            if (vis[cidx(nx, ny)]) continue;
            opts[optN++] = di;
        }

        if (optN <= 0) {
            st.pop_back();
            continue;
        }

        const int pick = opts[rng.range(0, optN - 1)];
        const int nx = cx + dirs[pick][0];
        const int ny = cy + dirs[pick][1];

        vis[cidx(nx, ny)] = 1;
        carveCell(nx, ny);
        carveBetween(cx, cy, nx, ny);
        st.push_back({nx, ny});
    }

    // Ensure the tile just inside the door is open.
    const int sx = outDoorX;
    const int sy = outDoorY - 1;
    if (sy >= 0 && sy < h) {
        g[static_cast<size_t>(sy)][static_cast<size_t>(sx)] = '.';
    }

    // BFS on the tile grid to find farthest reachable floors.
    auto passable = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        return g[static_cast<size_t>(y)][static_cast<size_t>(x)] != '#';
    };

    std::vector<int> dist(static_cast<size_t>(w * h), -1);
    auto tidx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
    std::deque<Vec2i> q;

    if (passable(sx, sy)) {
        dist[tidx(sx, sy)] = 0;
        q.push_back({sx, sy});
    }

    while (!q.empty()) {
        const Vec2i p = q.front();
        q.pop_front();
        const int cd = dist[tidx(p.x, p.y)];

        for (int di = 0; di < 4; ++di) {
            const int nx = p.x + dirs[di][0];
            const int ny = p.y + dirs[di][1];
            if (!passable(nx, ny)) continue;
            const size_t ii = tidx(nx, ny);
            if (dist[ii] >= 0) continue;
            dist[ii] = cd + 1;
            q.push_back({nx, ny});
        }
    }

    // Pick a farthest interior '.' tile as treasure.
    int bestD = -1;
    std::vector<Vec2i> best;

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            if (g[static_cast<size_t>(y)][static_cast<size_t>(x)] != '.') continue;
            const int d0 = dist[tidx(x, y)];
            if (d0 <= 0) continue;

            if (d0 > bestD) {
                bestD = d0;
                best.clear();
                best.push_back({x, y});
            } else if (d0 == bestD) {
                best.push_back({x, y});
            }
        }
    }

    if (!best.empty()) {
        const Vec2i t = best[static_cast<size_t>(rng.range(0, static_cast<int>(best.size()) - 1))];
        g[static_cast<size_t>(t.y)][static_cast<size_t>(t.x)] = 'T';
    }

    // Bonus: place a tool (lockpick/key) in a random dead end deeper in the maze.
    std::vector<Vec2i> dead;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            if (g[static_cast<size_t>(y)][static_cast<size_t>(x)] != '.') continue;
            const int d0 = dist[tidx(x, y)];
            if (d0 < 0) continue;

            int nPass = 0;
            for (int di = 0; di < 4; ++di) {
                if (passable(x + dirs[di][0], y + dirs[di][1])) nPass += 1;
            }
            if (nPass <= 1 && d0 >= 3) dead.push_back({x, y});
        }
    }

    if (!dead.empty() && rng.chance(0.70f)) {
        const Vec2i p = dead[static_cast<size_t>(rng.range(0, static_cast<int>(dead.size()) - 1))];
        const char tool = (rng.chance(depth >= 4 ? 0.65f : 0.35f)) ? 'R' : 'K';
        g[static_cast<size_t>(p.y)][static_cast<size_t>(p.x)] = tool;
    }

    return g;
}

static bool tryPlaceProceduralMazeVaultPrefab(Dungeon& d, RNG& rng, int depth, GenKind g) {
    (void)g;

    // Size: keep it small enough to fit into dense wall pockets, but add occasional variety.
    int w = 7;
    int h = 7;
    if (depth >= 6 && rng.chance(0.55f)) w = 9;
    if (depth >= 8 && rng.chance(0.40f)) h = 9;

    // Entrance style: deeper floors trend toward secret/locked finds.
    char doorChar = '+';
    const float pLocked = (depth >= 7) ? 0.10f : 0.0f;
    const float pSecret = std::clamp(0.18f + 0.05f * static_cast<float>(std::max(0, depth - 2)), 0.18f, 0.70f);
    if (pLocked > 0.0f && rng.chance(pLocked)) doorChar = 'L';
    else if (rng.chance(pSecret)) doorChar = 's';

    int baseDoorX = 0;
    int baseDoorY = 0;
    const auto base = makeMazeVaultBase(rng, w, h, doorChar, depth, baseDoorX, baseDoorY);

    // Build rotated/mirrored variants so we can attach to any corridor-facing wall.
    std::vector<PrefabVariant> variants;
    variants.reserve(8);
    for (int rot = 0; rot < 4; ++rot) {
        for (int mirror = 0; mirror < 2; ++mirror) {
            PrefabVariant v;
            if (buildPrefabVariantFromGrid(base, w, h, rot, mirror != 0, v)) {
                variants.push_back(std::move(v));
            }
        }
    }
    if (variants.empty()) return false;

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    auto tooCloseToStairsDoor = [&](int x, int y) -> bool {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y))
            : 9999;
        return du <= 4 || dd <= 4;
    };

    auto hasBonusItemAt = [&](const Vec2i& p) -> bool {
        for (const auto& it : d.bonusItemSpawns) {
            if (it.pos.x == p.x && it.pos.y == p.y) return true;
        }
        return false;
    };

    const int maxTries = 500;
    for (int tries = 0; tries < maxTries; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;
        if (tooCloseToStairsDoor(x, y)) continue;

        // Avoid clustering doors; these should feel like tucked-away finds.
        if (anyDoorInRadius(d, x, y, 2)) continue;

        int adjFloors = 0;
        Vec2i outDir{0, 0};
        for (const Vec2i& dv : dirs) {
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Floor) {
                adjFloors += 1;
                outDir = dv;
            }
        }
        if (adjFloors != 1) continue;

        const int fx = x + outDir.x;
        const int fy = y + outDir.y;
        if (!d.inBounds(fx, fy)) continue;
        if (d.at(fx, fy).type != TileType::Floor) continue;

        // Avoid attaching directly into special rooms (shops/shrines/etc).
        if (const Room* rr = findRoomContaining(d, fx, fy)) {
            if (rr->type != RoomType::Normal) continue;
        }

        // Pick a variant whose entrance faces the corridor direction.
        std::vector<int> matches;
        matches.reserve(variants.size());
        for (int i = 0; i < static_cast<int>(variants.size()); ++i) {
            if (variants[static_cast<size_t>(i)].outsideDir == outDir) matches.push_back(i);
        }
        if (matches.empty()) continue;

        const int start = rng.range(0, static_cast<int>(matches.size()) - 1);
        for (int k = 0; k < static_cast<int>(matches.size()); ++k) {
            const PrefabVariant& v = variants[static_cast<size_t>(matches[static_cast<size_t>((start + k) % matches.size())])];

            const int x0 = x - v.doorX;
            const int y0 = y - v.doorY;

            if (!applyPrefabAt(d, v, x0, y0)) continue;

            // If this is a locked entrance, drop a key right outside so the micro-vault is always solvable.
            if (v.doorChar == 'L' && d.inBounds(fx, fy) && !hasBonusItemAt({fx, fy})) {
                d.bonusItemSpawns.push_back({{fx, fy}, ItemKind::Key, 1});
            }

            return true;
        }
    }

    return false;
}


// ------------------------------------------------------------
// Procedural vault prefab: WFC "ruin pocket"
//
// A second fully procedural micro-vault generator.
// Instead of a perfect-maze corridor, we use a tiny Wave Function Collapse
// solver to synthesize an aperiodic ruin layout in a wall pocket.
//
// Goals:
// - More visual variety than the perfect maze cache
// - Always solvable: entrance -> treasure path exists
// - Still optional and off-corridor, so it never gates the critical path
// ------------------------------------------------------------

enum class WfcVaultTile : uint8_t {
    Wall = 0,
    Floor,
    Pillar,
    Chasm,
    Door,
};

static void buildWfcVaultRules(std::vector<uint32_t> allow[4]) {
    constexpr int nTiles = 5;
    const uint32_t ALL = (1u << nTiles) - 1u;

    auto bit = [](WfcVaultTile t) -> uint32_t {
        return 1u << static_cast<uint32_t>(t);
    };

    const uint32_t wallMask   = ALL;
    const uint32_t floorMask  = bit(WfcVaultTile::Wall) | bit(WfcVaultTile::Floor) | bit(WfcVaultTile::Pillar) | bit(WfcVaultTile::Chasm) | bit(WfcVaultTile::Door);
    const uint32_t pillarMask = bit(WfcVaultTile::Floor) | bit(WfcVaultTile::Wall);
    const uint32_t chasmMask  = bit(WfcVaultTile::Floor) | bit(WfcVaultTile::Wall) | bit(WfcVaultTile::Chasm);
    const uint32_t doorMask   = bit(WfcVaultTile::Wall) | bit(WfcVaultTile::Floor);

    uint32_t m[nTiles];
    m[static_cast<int>(WfcVaultTile::Wall)]   = wallMask;
    m[static_cast<int>(WfcVaultTile::Floor)]  = floorMask;
    m[static_cast<int>(WfcVaultTile::Pillar)] = pillarMask;
    m[static_cast<int>(WfcVaultTile::Chasm)]  = chasmMask;
    m[static_cast<int>(WfcVaultTile::Door)]   = doorMask;

    for (int dir = 0; dir < 4; ++dir) {
        allow[dir].assign(nTiles, 0u);
        for (int t = 0; t < nTiles; ++t) {
            allow[dir][t] = m[t];
        }
    }
}

static bool isWfcVaultPassableChar(char c) {
    return c == '.' || c == '+' || c == 's' || c == 'L' || c == 'T' || c == 'K' || c == 'R';
}

static std::vector<std::string> makeWfcVaultBase(RNG& rng, int w, int h, char doorChar, int depth, int& outDoorX, int& outDoorY) {
    outDoorX = 0;
    outDoorY = 0;

    std::vector<std::string> base(static_cast<size_t>(h), std::string(static_cast<size_t>(w), '#'));
    if (w < 5 || h < 5) return base;

    // Pick a boundary door (non-corner).
    const int side = rng.range(0, 3); // 0=left,1=right,2=top,3=bottom
    if (side == 0 || side == 1) {
        const int y = (h > 6) ? rng.range(2, h - 3) : rng.range(1, h - 2);
        outDoorY = y;
        outDoorX = (side == 0) ? 0 : (w - 1);
    } else {
        const int x = (w > 6) ? rng.range(2, w - 3) : rng.range(1, w - 2);
        outDoorX = x;
        outDoorY = (side == 2) ? 0 : (h - 1);
    }
    base[static_cast<size_t>(outDoorY)][static_cast<size_t>(outDoorX)] = doorChar;

    // WFC solve on full grid with fixed boundary/door.
    constexpr int nTiles = 5;
    static bool rulesBuilt = false;
    static std::vector<uint32_t> allow[4];
    if (!rulesBuilt) {
        buildWfcVaultRules(allow);
        rulesBuilt = true;
    }

    auto bit = [](WfcVaultTile t) -> uint32_t { return 1u << static_cast<uint32_t>(t); };

    const uint32_t wallBit   = bit(WfcVaultTile::Wall);
    const uint32_t floorBit  = bit(WfcVaultTile::Floor);
    const uint32_t pillarBit = bit(WfcVaultTile::Pillar);
    const uint32_t chasmBit  = bit(WfcVaultTile::Chasm);
    const uint32_t doorBit   = bit(WfcVaultTile::Door);

    const bool allowChasm = (depth >= 7) && rng.chance(0.28f);
    const float dd = static_cast<float>(std::clamp(depth, 0, 12));

    std::vector<float> weights(static_cast<size_t>(nTiles), 0.0f);
    weights[static_cast<size_t>(WfcVaultTile::Wall)]   = 55.0f + 3.5f * dd;
    weights[static_cast<size_t>(WfcVaultTile::Floor)]  = 100.0f;
    weights[static_cast<size_t>(WfcVaultTile::Pillar)] = 9.0f + 0.9f * dd;
    weights[static_cast<size_t>(WfcVaultTile::Chasm)]  = allowChasm ? (2.0f + 0.35f * std::max(0.0f, dd - 6.0f)) : 0.0f;
    weights[static_cast<size_t>(WfcVaultTile::Door)]   = 1.0f;

    // Domain setup: boundary fixed to wall, door fixed to Door.
    std::vector<uint32_t> domains(static_cast<size_t>(w * h), wallBit | floorBit | pillarBit | (allowChasm ? chasmBit : 0u));
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };

    // Determine interior neighbor of the door to keep the entrance open.
    Vec2i inDir{0, 0};
    if (outDoorX == 0) inDir = {1, 0};
    else if (outDoorX == w - 1) inDir = {-1, 0};
    else if (outDoorY == 0) inDir = {0, 1};
    else inDir = {0, -1};

    const int ix = outDoorX + inDir.x;
    const int iy = outDoorY + inDir.y;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool boundary = (x == 0 || y == 0 || x == (w - 1) || y == (h - 1));
            if (boundary) {
                if (x == outDoorX && y == outDoorY) domains[idx(x, y)] = doorBit;
                else domains[idx(x, y)] = wallBit;
                continue;
            }

            uint32_t m = wallBit | floorBit | pillarBit;
            if (allowChasm) m |= chasmBit;

            // Keep a small safe buffer around the entrance so the pocket is legible.
            const int md = std::abs(x - ix) + std::abs(y - iy);
            if (md <= 2) {
                m &= ~chasmBit;
                if (md <= 1) m &= ~pillarBit;
            }

            domains[idx(x, y)] = m;
        }
    }

    // Force the interior entrance cell to be floor.
    if (ix >= 1 && ix <= w - 2 && iy >= 1 && iy <= h - 2) {
        domains[idx(ix, iy)] = floorBit;
    }

    std::vector<uint8_t> sol;
    wfc::SolveStats stats;
    const bool ok = wfc::solve(w, h, nTiles, allow, weights, rng, domains, sol, /*maxRestarts=*/24, &stats);
    (void)stats;
    if (!ok || static_cast<int>(sol.size()) != (w * h)) return base;

    // Build char grid from solution.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t t = sol[idx(x, y)];
            char c = '#';
            if (t == static_cast<uint8_t>(WfcVaultTile::Wall)) c = '#';
            else if (t == static_cast<uint8_t>(WfcVaultTile::Floor)) c = '.';
            else if (t == static_cast<uint8_t>(WfcVaultTile::Pillar)) c = 'P';
            else if (t == static_cast<uint8_t>(WfcVaultTile::Chasm)) c = 'C';
            else if (t == static_cast<uint8_t>(WfcVaultTile::Door)) c = doorChar;

            base[static_cast<size_t>(y)][static_cast<size_t>(x)] = c;
        }
    }

    // Validate: need a non-trivial reachable floor region.
    std::vector<int> dist(static_cast<size_t>(w * h), -1);
    std::deque<Vec2i> q;

    auto passable = [&](int x, int y) -> bool {
        if (x < 0 || y < 0 || x >= w || y >= h) return false;
        return isWfcVaultPassableChar(base[static_cast<size_t>(y)][static_cast<size_t>(x)]);
    };

    if (!passable(ix, iy)) {
        base[static_cast<size_t>(iy)][static_cast<size_t>(ix)] = '.';
    }

    dist[idx(ix, iy)] = 0;
    q.push_back({ix, iy});

    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    while (!q.empty()) {
        const Vec2i p = q.front();
        q.pop_front();
        const int d0 = dist[idx(p.x, p.y)];

        for (int di = 0; di < 4; ++di) {
            const int nx = p.x + dirs[di][0];
            const int ny = p.y + dirs[di][1];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if (!passable(nx, ny)) continue;
            if (dist[idx(nx, ny)] >= 0) continue;
            dist[idx(nx, ny)] = d0 + 1;
            q.push_back({nx, ny});
        }
    }

    int floorCount = 0;
    int wallCount = 0;
    int obstacleCount = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const char c = base[static_cast<size_t>(y)][static_cast<size_t>(x)];
            if (c == '.') floorCount += 1;
            else if (c == '#') wallCount += 1;
            else if (c == 'P' || c == 'C') obstacleCount += 1;
        }
    }

    (void)obstacleCount;

    // Heuristic quality gates.
    if (floorCount < (w * h) / 6) return base;
    if (wallCount < (w * h) / 10) return base; // avoid "empty box" layouts

    // Pick farthest reachable '.' tile as treasure.
    int bestD = -1;
    std::vector<Vec2i> best;

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            if (base[static_cast<size_t>(y)][static_cast<size_t>(x)] != '.') continue;
            const int d0 = dist[idx(x, y)];
            if (d0 <= 0) continue;

            if (d0 > bestD) {
                bestD = d0;
                best.clear();
                best.push_back({x, y});
            } else if (d0 == bestD) {
                best.push_back({x, y});
            }
        }
    }

    if (best.empty() || bestD < 3) return base;

    const Vec2i t = best[static_cast<size_t>(rng.range(0, static_cast<int>(best.size()) - 1))];
    base[static_cast<size_t>(t.y)][static_cast<size_t>(t.x)] = 'T';

    // Bonus: drop a tool in a reachable dead-end.
    std::vector<Vec2i> dead;
    dead.reserve(32);

    auto countPassNeighbors = [&](int x, int y) -> int {
        int n = 0;
        for (int di = 0; di < 4; ++di) {
            const int nx = x + dirs[di][0];
            const int ny = y + dirs[di][1];
            if (passable(nx, ny)) n += 1;
        }
        return n;
    };

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            if (base[static_cast<size_t>(y)][static_cast<size_t>(x)] != '.') continue;
            if (x == t.x && y == t.y) continue;
            const int d0 = dist[idx(x, y)];
            if (d0 < 0) continue;
            if (d0 < 2) continue;
            if (countPassNeighbors(x, y) <= 1) dead.push_back({x, y});
        }
    }

    if (!dead.empty() && rng.chance(0.65f)) {
        const Vec2i p = dead[static_cast<size_t>(rng.range(0, static_cast<int>(dead.size()) - 1))];
        const char tool = (rng.chance(depth >= 6 ? 0.70f : 0.40f)) ? 'R' : 'K';
        base[static_cast<size_t>(p.y)][static_cast<size_t>(p.x)] = tool;
    }

    return base;
}

static bool tryPlaceProceduralWfcVaultPrefab(Dungeon& d, RNG& rng, int depth, GenKind g) {
    (void)g;

    int w = 9;
    int h = 7;

    if (depth >= 5 && rng.chance(0.55f)) w = 11;
    if (depth >= 7 && rng.chance(0.50f)) h = 9;
    if (depth >= 10 && rng.chance(0.35f)) { w = 11; h = 9; }

    // Entrance style: deeper floors trend toward secret/locked finds.
    char doorChar = '+';
    const float pLocked = (depth >= 8) ? 0.12f : 0.0f;
    const float pSecret = std::clamp(0.16f + 0.05f * static_cast<float>(std::max(0, depth - 3)), 0.16f, 0.70f);
    if (pLocked > 0.0f && rng.chance(pLocked)) doorChar = 'L';
    else if (rng.chance(pSecret)) doorChar = 's';

    int doorX = 0;
    int doorY = 0;

    // Try a few WFC layouts; some seeds will produce low-quality or disconnected pockets.
    std::vector<std::string> base;
    const int maxLayoutAttempts = 10;
    for (int a = 0; a < maxLayoutAttempts; ++a) {
        base = makeWfcVaultBase(rng, w, h, doorChar, depth, doorX, doorY);

        bool hasT = false;
        for (const auto& row : base) {
            if (row.find('T') != std::string::npos) { hasT = true; break; }
        }
        if (hasT) break;

        // Occasionally tweak size slightly to avoid repeated contradictions on unlucky seeds.
        if (a == 4 && w == 9 && depth >= 6) w = 11;
        if (a == 7 && h == 7 && depth >= 7) h = 9;
    }

    bool hasTreasure = false;
    for (const auto& row : base) {
        if (row.find('T') != std::string::npos) { hasTreasure = true; break; }
    }
    if (!hasTreasure) return false;

    // Build rotated/mirrored variants so we can attach to any corridor-facing wall.
    std::vector<PrefabVariant> variants;
    variants.reserve(8);
    for (int rot = 0; rot < 4; ++rot) {
        for (int mirror = 0; mirror < 2; ++mirror) {
            PrefabVariant v;
            if (buildPrefabVariantFromGrid(base, w, h, rot, mirror != 0, v)) {
                variants.push_back(std::move(v));
            }
        }
    }
    if (variants.empty()) return false;

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    auto tooCloseToStairsDoor = [&](int x, int y) -> bool {
        const int du = (d.inBounds(d.stairsUp.x, d.stairsUp.y))
            ? (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y))
            : 9999;
        const int dd = (d.inBounds(d.stairsDown.x, d.stairsDown.y))
            ? (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y))
            : 9999;
        return du <= 4 || dd <= 4;
    };

    auto hasBonusItemAt = [&](const Vec2i& p) -> bool {
        for (const auto& it : d.bonusItemSpawns) {
            if (it.pos.x == p.x && it.pos.y == p.y) return true;
        }
        return false;
    };

    const int maxTries = 500;
    for (int tries = 0; tries < maxTries; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;
        if (tooCloseToStairsDoor(x, y)) continue;

        // Avoid clustering doors; these should feel like tucked-away finds.
        if (anyDoorInRadius(d, x, y, 2)) continue;

        int adjFloors = 0;
        Vec2i outDir{0, 0};
        for (const Vec2i& dv : dirs) {
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Floor) {
                adjFloors += 1;
                outDir = dv;
            }
        }
        if (adjFloors != 1) continue;

        const int fx = x + outDir.x;
        const int fy = y + outDir.y;
        if (!d.inBounds(fx, fy)) continue;
        if (d.at(fx, fy).type != TileType::Floor) continue;

        // Avoid attaching directly into special rooms (shops/shrines/etc).
        if (const Room* rr = findRoomContaining(d, fx, fy)) {
            if (rr->type != RoomType::Normal) continue;
        }

        // Pick a variant whose entrance faces the corridor direction.
        std::vector<int> matches;
        matches.reserve(variants.size());
        for (int i = 0; i < static_cast<int>(variants.size()); ++i) {
            if (variants[static_cast<size_t>(i)].outsideDir == outDir) matches.push_back(i);
        }
        if (matches.empty()) continue;

        const int start = rng.range(0, static_cast<int>(matches.size()) - 1);
        for (int k = 0; k < static_cast<int>(matches.size()); ++k) {
            const PrefabVariant& v = variants[static_cast<size_t>(matches[static_cast<size_t>((start + k) % matches.size())])];

            const int x0 = x - v.doorX;
            const int y0 = y - v.doorY;

            if (!applyPrefabAt(d, v, x0, y0)) continue;

            // If this is a locked entrance, drop a key right outside so the micro-vault is always solvable.
            if (v.doorChar == 'L' && d.inBounds(fx, fy) && !hasBonusItemAt({fx, fy})) {
                d.bonusItemSpawns.push_back({{fx, fy}, ItemKind::Key, 1});
            }

            return true;
        }
    }

    return false;
}

bool maybePlaceVaultPrefabs(Dungeon& d, RNG& rng, int depth, GenKind g) {
    d.vaultPrefabCount = 0;

    // Avoid flooding the dungeon with prefabs; these are meant to be rare little "finds".
    float pAny = 0.0f;
    if (depth >= 1) pAny = 0.14f;
    if (depth >= 4) pAny = 0.22f;
    if (depth >= 7) pAny = 0.30f;
    if (depth >= 10) pAny = 0.36f;

    // Organic caves already have lots of pockets and special terrain.
    if (g == GenKind::Cavern) pAny *= 0.55f;

    if (!rng.chance(pAny)) return false;

    int want = 1;
    if (depth >= 6 && rng.chance(0.45f)) want += 1;
    if (depth >= 9 && rng.chance(0.35f)) want += 1;
    want = std::clamp(want, 1, 3);

        // Prefab library (single-entrance, border-walled).
    size_t prefabCount = 0;
    const PrefabDef* prefabs = vaultprefabs::catalog(prefabCount);

    auto pickWeightedPrefab = [&]() -> const PrefabDef* {
        int total = 0;
        for (size_t i = 0; i < prefabCount; ++i) {
            const PrefabDef& p = prefabs[i];
            if (depth < p.minDepth) continue;
            total += std::max(1, p.weight);
        }
        if (total <= 0) return nullptr;

        int r = rng.range(1, total);
        for (size_t i = 0; i < prefabCount; ++i) {
            const PrefabDef& p = prefabs[i];
            if (depth < p.minDepth) continue;
            r -= std::max(1, p.weight);
            if (r <= 0) return &p;
        }

        // Fallback (should be unreachable).
        for (size_t i = 0; i < prefabCount; ++i) {
            const PrefabDef& p = prefabs[i];
            if (depth >= p.minDepth) return &p;
        }
        return nullptr;
    };

    // Mix in *procedural* micro-vaults for extra variety (still rare):
    //  - a perfect-maze cache
    //  - a WFC-synthesized "ruin pocket"
    float pProc = 0.0f;
    if (depth >= 2) pProc = 0.22f;
    if (depth >= 6) pProc = 0.32f;
    if (depth >= 9) pProc = 0.38f;
    if (g == GenKind::Cavern) pProc *= 0.60f;
    pProc = std::clamp(pProc, 0.0f, 0.45f);

    float pWfc = 0.0f;
    if (depth >= 4) pWfc = 0.45f;
    if (depth >= 7) pWfc = 0.55f;
    if (depth >= 10) pWfc = 0.62f;
    if (g == GenKind::Cavern) pWfc *= 0.75f;
    pWfc = std::clamp(pWfc, 0.0f, 0.75f);

    int placed = 0;
    for (int i = 0; i < want; ++i) {
        bool ok = false;
        // Give each slot a few attempts to find a fit on dense/odd maps.
        for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
            bool didProc = false;

            // First: sometimes try a fully procedural micro-vault.
            // We pick between the maze-cache and WFC-ruin styles.
            if (pProc > 0.0f && rng.chance(pProc)) {
                didProc = true;

                bool procOk = false;
                if (depth >= 4 && pWfc > 0.0f && rng.chance(pWfc)) {
                    procOk = tryPlaceProceduralWfcVaultPrefab(d, rng, depth, g);
                    if (!procOk) {
                        // Fallback to maze if WFC couldn't find a valid layout/fit.
                        procOk = tryPlaceProceduralMazeVaultPrefab(d, rng, depth, g);
                    }
                } else {
                    procOk = tryPlaceProceduralMazeVaultPrefab(d, rng, depth, g);
                    // Occasionally try WFC as a secondary attempt for variety.
                    if (!procOk && depth >= 4 && pWfc > 0.0f && rng.chance(0.55f)) {
                        procOk = tryPlaceProceduralWfcVaultPrefab(d, rng, depth, g);
                    }
                }

                if (procOk) {
                    d.vaultPrefabCount += 1;
                    ok = true;
                    break;
                }
            }

            // Fallback: hand-authored prefab library.
            const PrefabDef* def = pickWeightedPrefab();
            if (!def) break;
            if (tryPlaceVaultPrefab(d, rng, depth, *def)) {
                d.vaultPrefabCount += 1;
                ok = true;
                break;
            }

            // If we didn't roll a procedural attempt and the static attempt failed,
            // try the procedural one as a last chance (helps on maps with little wall mass).
            if (!didProc && pProc > 0.0f && rng.chance(0.35f)) {
                bool procOk = false;
                if (depth >= 4 && pWfc > 0.0f && rng.chance(pWfc)) {
                    procOk = tryPlaceProceduralWfcVaultPrefab(d, rng, depth, g);
                }
                if (!procOk) procOk = tryPlaceProceduralMazeVaultPrefab(d, rng, depth, g);

                if (procOk) {
                    d.vaultPrefabCount += 1;
                    ok = true;
                    break;
                }
            }
        }
        if (ok) placed += 1;
    }

    return placed > 0;
}


void markSpecialRooms(Dungeon& d, RNG& rng, int depth) {
    if (d.rooms.empty()) return;

    // Distance map from the upstairs. Used to:
    //  - avoid assigning key rooms into disconnected pockets created by late terrain passes
    //  - pace room types (shops closer, treasure/lairs deeper)
    const auto distFromUp = bfsDistanceMap(d, d.stairsUp);
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    auto roomReachDist = [&](const Room& r) -> int {
        int best = 1000000000;
        // Scan the interior for any passable tile with a valid BFS distance.
        for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
            for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
                if (!d.inBounds(x, y)) continue;
                if (!d.isPassable(x, y)) continue;
                const int di = distFromUp[idx(x, y)];
                if (di >= 0 && di < best) best = di;
            }
        }
        if (best == 1000000000) return -1;
        return best;
    };

    auto buildPool = [&](bool allowDown, bool requireReachable) {
        std::vector<int> pool;
        pool.reserve(d.rooms.size());
        for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
            const Room& r = d.rooms[static_cast<size_t>(i)];
            // Prefer leaving the start room "normal" so early turns are fair.
            if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
            if (!allowDown && r.contains(d.stairsDown.x, d.stairsDown.y)) continue;
            if (r.type != RoomType::Normal) continue;

            if (requireReachable) {
                const int rd = roomReachDist(r);
                if (rd < 0) continue;
            }

            pool.push_back(i);
        }
        return pool;
    };

    auto removeFromPool = [&](std::vector<int>& pool, int roomIdx) {
        for (size_t i = 0; i < pool.size(); ++i) {
            if (pool[i] == roomIdx) {
                pool[i] = pool.back();
                pool.pop_back();
                return;
            }
        }
    };

    auto sortedByDist = [&](const std::vector<int>& pool) {
        std::vector<std::pair<int, int>> v;
        v.reserve(pool.size());
        for (int ri : pool) {
            if (ri < 0 || ri >= static_cast<int>(d.rooms.size())) continue;
            const int rd = roomReachDist(d.rooms[static_cast<size_t>(ri)]);
            if (rd < 0) continue;
            v.push_back({rd, ri});
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });
        return v;
    };

    auto pickClosest = [&](std::vector<int>& pool, int topN, int minDist) -> int {
        const auto v = sortedByDist(pool);
        if (v.empty()) return -1;

        // Try to honor a minimum distance so "shops" don't spawn immediately adjacent to the start.
        int start = 0;
        if (minDist > 0) {
            while (start < static_cast<int>(v.size()) && v[static_cast<size_t>(start)].first < minDist) start++;
            if (start >= static_cast<int>(v.size())) start = 0; // can't honor; fall back
        }

        const int end = std::min(static_cast<int>(v.size()) - 1, start + std::max(1, topN) - 1);
        const int pick = rng.range(start, end);
        const int roomIdx = v[static_cast<size_t>(pick)].second;
        removeFromPool(pool, roomIdx);
        return roomIdx;
    };

    auto pickFarthest = [&](std::vector<int>& pool, int topN) -> int {
        const auto v = sortedByDist(pool);
        if (v.empty()) return -1;

        const int end = static_cast<int>(v.size()) - 1;
        const int start = std::max(0, end - std::max(1, topN) + 1);
        const int pick = rng.range(start, end);
        const int roomIdx = v[static_cast<size_t>(pick)].second;
        removeFromPool(pool, roomIdx);
        return roomIdx;
    };

    auto pickQuantile = [&](std::vector<int>& pool, float q, int radius) -> int {
        const auto v = sortedByDist(pool);
        if (v.empty()) return -1;
        const int n = static_cast<int>(v.size());
        const int target = std::clamp(static_cast<int>(std::lround(q * static_cast<float>(n - 1))), 0, n - 1);
        const int start = std::max(0, target - std::max(0, radius));
        const int end = std::min(n - 1, target + std::max(0, radius));
        const int pick = rng.range(start, end);
        const int roomIdx = v[static_cast<size_t>(pick)].second;
        removeFromPool(pool, roomIdx);
        return roomIdx;
    };

    // Prefer pools where rooms are actually reachable from the upstairs.
    std::vector<int> pool = buildPool(false, true);
    if (pool.empty()) pool = buildPool(true, true);
    if (pool.empty()) pool = buildPool(false, false);
    if (pool.empty()) pool = buildPool(true, false);

    // Extreme fallback: just take any normal room.
    if (pool.empty()) {
        pool.reserve(d.rooms.size());
        for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
            if (d.rooms[static_cast<size_t>(i)].type == RoomType::Normal) pool.push_back(i);
        }
    }

    // ------------------------------------------------------------
    // Round 36: Path-aware special room placement.
    //
    // We classify rooms that intersect the *shortest passable path* between stairs
    // as being on the "spine" (critical path). We then:
    //  - bias SHOPS/SHRINES to appear on-spine (so players reliably encounter them)
    //  - bias TREASURE/LAIRS to appear off-spine (to reward exploration)
    //  - enforce a soft minimum separation between chosen special rooms to avoid
    //    clumping several high-impact rooms together.
    // ------------------------------------------------------------

    // Identify "spine" rooms.
    std::vector<uint8_t> onSpine(static_cast<size_t>(d.rooms.size()), uint8_t{0});
    {
        int spineCount = 0;
        const auto spinePath = shortestPassablePath(d, d.stairsUp, d.stairsDown);
        if (!spinePath.empty()) {
            for (const Vec2i& p : spinePath) {
                for (size_t i = 0; i < d.rooms.size(); ++i) {
                    const Room& r = d.rooms[i];
                    if (!r.contains(p.x, p.y)) continue;
                    if (!onSpine[i]) {
                        onSpine[i] = 1;
                        ++spineCount;
                    }
                    break;
                }
            }
        }
        d.spineRoomCount = spineCount;
    }

    // Representative passable tile per room (used for BFS-based separation).
    std::vector<Vec2i> rep(static_cast<size_t>(d.rooms.size()), {-1, -1});
    auto roomRep = [&](const Room& r) -> Vec2i {
        // Prefer interior tiles (avoid doors on the boundary).
        const int cx = r.cx();
        const int cy = r.cy();

        auto interiorContains = [&](int x, int y) {
            return x > r.x && x < (r.x2() - 1) && y > r.y && y < (r.y2() - 1);
        };

        const int maxRad = std::max(r.w, r.h);
        for (int rad = 0; rad <= maxRad; ++rad) {
            for (int dy = -rad; dy <= rad; ++dy) {
                for (int dx = -rad; dx <= rad; ++dx) {
                    const int x = cx + dx;
                    const int y = cy + dy;
                    if (!d.inBounds(x, y)) continue;
                    if (!interiorContains(x, y)) continue;
                    if (!d.isPassable(x, y)) continue;
                    return {x, y};
                }
            }
        }

        // Fallback: scan interior.
        for (int y = r.y + 1; y < r.y2() - 1; ++y) {
            for (int x = r.x + 1; x < r.x2() - 1; ++x) {
                if (!d.inBounds(x, y)) continue;
                if (!d.isPassable(x, y)) continue;
                return {x, y};
            }
        }

        return {cx, cy};
    };

    for (size_t i = 0; i < d.rooms.size(); ++i) {
        rep[i] = roomRep(d.rooms[i]);
    }

    // Split pool into spine / side candidates.
    std::vector<int> spinePool;
    std::vector<int> sidePool;
    spinePool.reserve(pool.size());
    sidePool.reserve(pool.size());
    for (int ri : pool) {
        if (ri < 0 || ri >= static_cast<int>(d.rooms.size())) continue;
        if (onSpine[static_cast<size_t>(ri)] != 0) spinePool.push_back(ri);
        else sidePool.push_back(ri);
    }

    auto removeEverywhere = [&](int roomIdx) {
        removeFromPool(spinePool, roomIdx);
        removeFromPool(sidePool, roomIdx);
        removeFromPool(pool, roomIdx);
    };

    auto pruneBySep = [&](int chosenRoomIdx, int minSep) {
        if (chosenRoomIdx < 0 || minSep <= 0) return;
        if (chosenRoomIdx >= static_cast<int>(rep.size())) return;
        const Vec2i c = rep[static_cast<size_t>(chosenRoomIdx)];
        if (!d.inBounds(c.x, c.y)) return;
        if (!d.isPassable(c.x, c.y)) return;

        const auto dist = bfsDistanceMap(d, c);
        auto prunePool = [&](std::vector<int>& pp) {
            if (pp.empty()) return;
            size_t out = 0;
            for (size_t i = 0; i < pp.size(); ++i) {
                const int ri = pp[i];
                if (ri < 0 || ri >= static_cast<int>(rep.size())) continue;
                const Vec2i p = rep[static_cast<size_t>(ri)];
                if (!d.inBounds(p.x, p.y)) {
                    pp[out++] = ri;
                    continue;
                }
                const int di = dist[idx(p.x, p.y)];
                if (di >= 0 && di < minSep) continue;
                pp[out++] = ri;
            }
            pp.resize(out);
        };

        prunePool(spinePool);
        prunePool(sidePool);
        prunePool(pool);
    };

    auto claim = [&](int roomIdx, RoomType rt, int sep) {
        if (roomIdx < 0 || roomIdx >= static_cast<int>(d.rooms.size())) return;
        d.rooms[static_cast<size_t>(roomIdx)].type = rt;
        removeEverywhere(roomIdx);
        pruneBySep(roomIdx, sep);
    };

    const int sepMajor = std::clamp(9 + depth / 3, 9, 14);
    const int sepMinor = std::max(6, sepMajor - 3);

    // TREASURE: bias toward deeper OFF-SPINE rooms when possible.
    {
        std::vector<int>& p = !sidePool.empty() ? sidePool : (!spinePool.empty() ? spinePool : pool);
        const int t = pickFarthest(p, 3);
        if (t >= 0) claim(t, RoomType::Treasure, sepMajor);
    }

    // Deep floors can carry extra treasure to support a longer run.
    if (depth >= 7) {
        float extraTreasureChance = std::min(0.55f, 0.25f + 0.05f * static_cast<float>(depth - 7));
        if (rng.chance(extraTreasureChance)) {
            std::vector<int>& p = !sidePool.empty() ? sidePool : (!spinePool.empty() ? spinePool : pool);
            const int t2 = pickFarthest(p, 2);
            if (t2 >= 0) claim(t2, RoomType::Treasure, sepMinor);
        }
    }

    // SHOPS: bias onto the stairs spine so players reliably find them.
    {
        // Shops: give gold real meaning and provide a mid-run power curve. More common deeper.
        float shopChance = 0.25f;
        if (depth >= 2) shopChance = 0.55f;
        if (depth >= 4) shopChance = 0.70f;
        // Tiny ramp for longer runs.
        shopChance = std::min(0.85f, shopChance + 0.02f * std::max(0, depth - 4));
        // Midpoint floor: guarantee at least one shop if there's room.
        if (depth == 5) shopChance = 1.0f;

        // Keep a soft minimum distance so the start isn't immediately a "free shop room".
        const int minShopDist = (depth <= 2) ? 4 : 6;

        if (!pool.empty() && rng.chance(shopChance)) {
            std::vector<int>& p = !spinePool.empty() ? spinePool : (!sidePool.empty() ? sidePool : pool);
            const int sh = pickClosest(p, 3, minShopDist);
            if (sh >= 0) claim(sh, RoomType::Shop, sepMinor);
        }
    }

    // LAIRS: prefer deeper OFF-SPINE rooms.
    {
        std::vector<int>& p = !sidePool.empty() ? sidePool : (!spinePool.empty() ? spinePool : pool);
        const int l = pickFarthest(p, 3);
        if (l >= 0) claim(l, RoomType::Lair, sepMajor);
    }

    // SHRINES: mid-ish rooms on the spine so they're useful but not right on the stairs.
    {
        std::vector<int>& p = !spinePool.empty() ? spinePool : (!sidePool.empty() ? sidePool : pool);
        const int s = pickQuantile(p, 0.45f, 2);
        if (s >= 0) claim(s, RoomType::Shrine, sepMinor);
    }

    // Themed rooms: a light-touch extra specialization to diversify loot/encounters.
    if (!pool.empty() && depth >= 2) {
        float themeChance = 0.55f;
        if (depth >= 4) themeChance = 0.70f;
        if (depth >= 7) themeChance = 0.82f;
        // Midpoint floor: slightly increase the chance for a themed room.
        if (depth == 5) themeChance = 0.90f;

        if (rng.chance(std::min(0.95f, themeChance))) {
            // Mild bias: themed rooms are a bit nicer on-spine, but can also appear off-spine.
            const bool preferSpine = (!spinePool.empty() && (sidePool.empty() || rng.chance(0.62f)));
            std::vector<int>& p = preferSpine ? spinePool : (!sidePool.empty() ? sidePool : pool);
            const int rr = pickQuantile(p, 0.60f, 3);
            if (rr >= 0) {
                const float r01 = rng.next01();
                RoomType rt = RoomType::Armory;

                // Early: more armories (gear stabilizes runs).
                // Mid: libraries become common (utility scrolls/wands).
                // Late: laboratories creep in (potions + weirdness).
                if (depth <= 2) {
                    rt = (r01 < 0.70f) ? RoomType::Armory : (r01 < 0.90f ? RoomType::Library : RoomType::Laboratory);
                } else if (depth <= 4) {
                    rt = (r01 < 0.45f) ? RoomType::Armory : (r01 < 0.82f ? RoomType::Library : RoomType::Laboratory);
                } else if (depth <= 6) {
                    rt = (r01 < 0.30f) ? RoomType::Armory : (r01 < 0.72f ? RoomType::Library : RoomType::Laboratory);
                } else {
                    rt = (r01 < 0.20f) ? RoomType::Armory : (r01 < 0.58f ? RoomType::Library : RoomType::Laboratory);
                }

                claim(rr, rt, std::max(5, sepMinor - 1));
            }
        }
    }

    // Debug/tuning: compute min BFS separation between special rooms (representative tiles).
    {
        std::vector<int> specials;
        specials.reserve(d.rooms.size());
        for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
            const RoomType rt = d.rooms[static_cast<size_t>(i)].type;
            if (rt == RoomType::Normal) continue;
            // Exclude bonus rooms (secret/vault) here; they are carved later and have their own pacing.
            if (rt == RoomType::Secret || rt == RoomType::Vault) continue;
            specials.push_back(i);
        }

        int best = std::numeric_limits<int>::max();
        for (size_t a = 0; a < specials.size(); ++a) {
            const int ia = specials[a];
            if (ia < 0 || ia >= static_cast<int>(rep.size())) continue;
            const Vec2i pa = rep[static_cast<size_t>(ia)];
            if (!d.inBounds(pa.x, pa.y) || !d.isPassable(pa.x, pa.y)) continue;
            const auto dist = bfsDistanceMap(d, pa);
            for (size_t b = a + 1; b < specials.size(); ++b) {
                const int ib = specials[b];
                if (ib < 0 || ib >= static_cast<int>(rep.size())) continue;
                const Vec2i pb = rep[static_cast<size_t>(ib)];
                if (!d.inBounds(pb.x, pb.y)) continue;
                const int di = dist[idx(pb.x, pb.y)];
                if (di >= 0 && di < best) best = di;
            }
        }

        if (best == std::numeric_limits<int>::max()) best = 0;
        d.specialRoomMinSep = best;
    }
}

Vec2i farthestPassableTile(const Dungeon& d, const std::vector<int>& dist, RNG& rng) {
    int bestDist = -1;
    std::vector<Vec2i> best;
    best.reserve(16);

    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (!d.isPassable(x, y)) continue;
            const int di = dist[static_cast<size_t>(y * d.width + x)];
            if (di < 0) continue;
            if (di > bestDist) {
                bestDist = di;
                best.clear();
                best.push_back({x, y});
            } else if (di == bestDist) {
                best.push_back({x, y});
            }
        }
    }

    if (best.empty()) return {1, 1};
    return best[static_cast<size_t>(rng.range(0, static_cast<int>(best.size()) - 1))];
}

void generateBspRooms(Dungeon& d, RNG& rng) {
    // BSP parameters tuned for "classic" ProcRogue maps.
    // As the map grows, keep leaves (and thus room granularity) roughly stable by
    // scaling the minimum leaf size with the map's linear dimension.
    //
    // Baseline: the previous standard 84x55 used minLeaf=8.
    constexpr float kTuneBaseArea = 84.0f * 55.0f;
    const float area = static_cast<float>(std::max(1, d.width * d.height));
    const float linear = std::sqrt(area / kTuneBaseArea);
    const int minLeaf = std::clamp(static_cast<int>(std::lround(8.0f * linear)), 8, 16);

    std::vector<Leaf> nodes;
    // A rough upper bound on BSP node count is O(area/minLeaf^2). Reserve enough to
    // avoid churn on larger maps while staying cheap on small maps.
    const int estLeaves = std::max(32, (d.width * d.height) / (minLeaf * minLeaf));
    nodes.reserve(static_cast<size_t>(estLeaves * 2));

    nodes.push_back({1, 1, d.width - 2, d.height - 2, -1, -1, -1}); // root

    // Build BSP tree
    for (size_t i = 0; i < nodes.size(); ++i) {
        Leaf& n = nodes[i];
        // Don't split too small leaves.
        if (n.w < minLeaf * 2 && n.h < minLeaf * 2) continue;

        // Random split orientation.
        bool splitH = rng.chance(0.5f);
        // Bias: split along longer dimension.
        if (n.w > n.h && n.w / n.h >= 2) splitH = false;
        else if (n.h > n.w && n.h / n.w >= 2) splitH = true;

        int split = splitLeaf(n, splitH, rng, minLeaf);
        if (split < 0) continue;

        Leaf a = n;
        Leaf b = n;
        if (splitH) {
            a.h = split;
            b.y = n.y + split;
            b.h = n.h - split;
        } else {
            a.w = split;
            b.x = n.x + split;
            b.w = n.w - split;
        }

        int leftIndex = static_cast<int>(nodes.size());
        nodes.push_back(a);
        int rightIndex = static_cast<int>(nodes.size());
        nodes.push_back(b);
        n.left = leftIndex;
        n.right = rightIndex;
    }

    // Create rooms in each leaf that has no children.
    d.rooms.clear();
    d.rooms.reserve(nodes.size());

    for (auto& n : nodes) {
        if (n.left >= 0 || n.right >= 0) continue;

        // Room size within leaf.
        int rw = rng.range(4, std::max(4, n.w - 2));
        int rh = rng.range(4, std::max(4, n.h - 2));
        int rx = rng.range(n.x + 1, std::max(n.x + 1, n.x + n.w - rw - 1));
        int ry = rng.range(n.y + 1, std::max(n.y + 1, n.y + n.h - rh - 1));

        // Clamp.
        rw = std::min(rw, n.w - 2);
        rh = std::min(rh, n.h - 2);
        if (rw < 4 || rh < 4) continue;

        carveRect(d, rx, ry, rw, rh, TileType::Floor);
        Room r;
        r.x = rx;
        r.y = ry;
        r.w = rw;
        r.h = rh;
        r.type = RoomType::Normal;
        d.rooms.push_back(r);
        n.roomIndex = static_cast<int>(d.rooms.size()) - 1;
    }

    if (d.rooms.empty()) {
        // Fallback to a basic room if BSP fails.
        carveRect(d, 2, 2, d.width - 4, d.height - 4, TileType::Floor);
        d.rooms.push_back({2, 2, d.width - 4, d.height - 4, RoomType::Normal});
    }


    // Precompute which tiles are inside rooms. Used both for smarter corridor routing
    // (avoid tunneling through other rooms) and for later branch/door placement passes.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (d.inBounds(x, y)) inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    // Connect rooms following the BSP tree.
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        Leaf& n = nodes[static_cast<size_t>(i)];
        if (n.left < 0 || n.right < 0) continue;

        int ra = pickRandomRoomInSubtree(nodes, n.left, rng);
        int rb = pickRandomRoomInSubtree(nodes, n.right, rng);
        if (ra >= 0 && rb >= 0 && ra != rb) {
            connectRooms(d, d.rooms[static_cast<size_t>(ra)], d.rooms[static_cast<size_t>(rb)], rng, inRoom);
        }
    }

    // Extra loops: connect random room pairs.
    const int extra = std::max(1, static_cast<int>(d.rooms.size()) / 3);
    for (int i = 0; i < extra; ++i) {
        int a = rng.range(0, static_cast<int>(d.rooms.size()) - 1);
        int b = rng.range(0, static_cast<int>(d.rooms.size()) - 1);
        if (a == b) continue;
        connectRooms(d, d.rooms[static_cast<size_t>(a)], d.rooms[static_cast<size_t>(b)], rng, inRoom);
    }
    // Branch corridors (dead ends)
    int branches = std::max(2, static_cast<int>(d.rooms.size()));
    for (int i = 0; i < branches; ++i) {
        int x = rng.range(1, d.width - 2);
        int y = rng.range(1, d.height - 2);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Floor) continue;
        if (inRoom[static_cast<size_t>(y * d.width + x)] != 0) continue; // prefer corridors

        const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        int dIdx = rng.range(0, 3);
        int dx = dirs[dIdx][0];
        int dy = dirs[dIdx][1];

        int nx = x + dx;
        int ny = y + dy;
        if (!d.inBounds(nx, ny)) continue;
        if (d.at(nx, ny).type != TileType::Wall) continue; // needs to dig into wall

        int len = rng.range(3, 8);
        int cx = x;
        int cy = y;
        for (int step = 0; step < len; ++step) {
            cx += dx;
            cy += dy;
            if (!d.inBounds(cx, cy)) break;
            if (d.at(cx, cy).type != TileType::Wall) break;
            carveFloor(d, cx, cy);
        }
    }

    // Place stairs: up in the first room, down in the farthest room by BFS.
    const Room& startRoom = d.rooms.front();
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        carveFloor(d, d.stairsUp.x, d.stairsUp.y);
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    auto dist = bfsDistanceMap(d, d.stairsUp);
    int bestRoomIdx = 0;
    int bestDist = -1;
    for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
        const Room& r = d.rooms[static_cast<size_t>(i)];
        int cx = r.cx();
        int cy = r.cy();
        if (!d.inBounds(cx, cy)) continue;
        int d0 = dist[static_cast<size_t>(cy * d.width + cx)];
        if (d0 > bestDist) {
            bestDist = d0;
            bestRoomIdx = i;
        }
    }
    const Room& endRoom = d.rooms[static_cast<size_t>(bestRoomIdx)];
    d.stairsDown = { endRoom.cx(), endRoom.cy() };
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }

    // Extra corridor doors (beyond the room-connection doors) make long halls more
    // tactically interesting.
    //
    // Round 18: use a corridor-graph analysis pass (place doors in the middle of long,
    // straight hallway segments) to avoid "door spam" while still producing meaningful
    // chokepoints.
    placeStrategicCorridorDoors(d, rng, inRoom, 0.85f);
}


struct DSU {
    std::vector<int> p;
    std::vector<int> r;
    explicit DSU(int n) : p(static_cast<size_t>(std::max(0, n))), r(static_cast<size_t>(std::max(0, n)), 0) {
        for (int i = 0; i < n; ++i) p[static_cast<size_t>(i)] = i;
    }
    int find(int a) {
        int x = a;
        while (p[static_cast<size_t>(x)] != x) x = p[static_cast<size_t>(x)];
        // Path compression.
        while (p[static_cast<size_t>(a)] != a) {
            int parent = p[static_cast<size_t>(a)];
            p[static_cast<size_t>(a)] = x;
            a = parent;
        }
        return x;
    }
    bool unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        if (r[static_cast<size_t>(a)] < r[static_cast<size_t>(b)]) std::swap(a, b);
        p[static_cast<size_t>(b)] = a;
        if (r[static_cast<size_t>(a)] == r[static_cast<size_t>(b)]) r[static_cast<size_t>(a)]++;
        return true;
    }
};

inline bool rectsOverlap(const Room& a, int bx, int by, int bw, int bh, int margin) {
    const int ax0 = a.x - margin;
    const int ay0 = a.y - margin;
    const int ax1 = a.x + a.w + margin;
    const int ay1 = a.y + a.h + margin;

    const int bx0 = bx - margin;
    const int by0 = by - margin;
    const int bx1 = bx + bw + margin;
    const int by1 = by + bh + margin;

    return !(bx1 <= ax0 || bx0 >= ax1 || by1 <= ay0 || by0 >= ay1);
}

// A corridor carver that "wanders" toward the goal (biased random walk).
// This makes tunnels feel more organic than strict L-corridors, while still guaranteeing connectivity
// via an A* fallback in the caller.
bool carveCorridorWander(Dungeon& d,
                         RNG& rng,
                         Vec2i start,
                         Vec2i goal,
                         const std::vector<uint8_t>& roomMask,
                         int maxSteps,
                         float biasTowardGoal) {
    if (!d.inBounds(start.x, start.y) || !d.inBounds(goal.x, goal.y)) return false;
    if (maxSteps <= 0) return false;
    biasTowardGoal = std::clamp(biasTowardGoal, 0.0f, 1.0f);

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };
    auto inRoom = [&](int x, int y) -> bool {
        if (roomMask.empty()) return false;
        size_t ii = idx(x, y);
        if (ii >= roomMask.size()) return false;
        return roomMask[ii] != 0;
    };

    Vec2i cur = start;
    Vec2i lastDir = {0, 0};

    carveFloor(d, cur.x, cur.y);

    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    std::vector<Vec2i> trace;
    trace.reserve(static_cast<size_t>(std::max(16, maxSteps)));
    trace.push_back(cur);

    for (int step = 0; step < maxSteps && !(cur == goal); ++step) {
        int dx = goal.x - cur.x;
        int dy = goal.y - cur.y;

        // Preferred moves reduce Manhattan distance.
        Vec2i pref[2];
        int prefCount = 0;
        if (dx != 0) pref[prefCount++] = { (dx > 0) ? 1 : -1, 0 };
        if (dy != 0) pref[prefCount++] = { 0, (dy > 0) ? 1 : -1 };

        auto tryStep = [&](Vec2i dir) -> bool {
            // Avoid immediate backtracking unless we're stuck.
            if (dir.x == -lastDir.x && dir.y == -lastDir.y && (lastDir.x != 0 || lastDir.y != 0)) return false;

            const int nx = cur.x + dir.x;
            const int ny = cur.y + dir.y;
            if (!d.inBounds(nx, ny)) return false;

            // Keep borders intact; the final ensureBorders() pass is not an excuse to carve out-of-range.
            if (nx <= 0 || ny <= 0 || nx >= d.width - 1 || ny >= d.height - 1) return false;

            if (inRoom(nx, ny)) return false;

            // Only carve into corridor-compatible tiles.
            const TileType t = d.at(nx, ny).type;
            if (!(t == TileType::Wall || t == TileType::Floor)) return false;

            carveFloor(d, nx, ny);
            cur = {nx, ny};
            lastDir = dir;
            trace.push_back(cur);
            return true;
        };

        bool moved = false;

        // Bias toward the goal.
        if (prefCount > 0 && rng.chance(biasTowardGoal)) {
            // If we have two preferred axes, randomize which we try first.
            if (prefCount == 2 && rng.chance(0.5f)) std::swap(pref[0], pref[1]);
            for (int i = 0; i < prefCount; ++i) {
                if (tryStep(pref[i])) { moved = true; break; }
            }
        }

        // Otherwise, wander. Shuffle-ish by starting index.
        if (!moved) {
            int startIdx = rng.range(0, 3);
            for (int i = 0; i < 4; ++i) {
                Vec2i dir = dirs[(startIdx + i) & 3];
                if (tryStep(dir)) { moved = true; break; }
            }
        }

        // If we couldn't move without backtracking, allow it as a last resort.
        if (!moved && (lastDir.x != 0 || lastDir.y != 0)) {
            Vec2i back = {-lastDir.x, -lastDir.y};
            const int nx = cur.x + back.x;
            const int ny = cur.y + back.y;
            if (d.inBounds(nx, ny) && !(nx <= 0 || ny <= 0 || nx >= d.width - 1 || ny >= d.height - 1) && !inRoom(nx, ny)) {
                const TileType t = d.at(nx, ny).type;
                if (t == TileType::Floor) {
                    cur = {nx, ny};
                    lastDir = back;
                    trace.push_back(cur);
                    moved = true;
                }
            }
        }

        if (!moved) return false;
    }

    if (!(cur == goal)) return false;

    // Roughen the main tunnel path slightly (adds little alcoves/width variance).
    const float roughen = 0.05f;
    for (const Vec2i& p : trace) {
        if (rng.chance(roughen)) {
            const int pick = rng.range(0, 3);
            const Vec2i dv = dirs[pick];
            const int nx = p.x + dv.x;
            const int ny = p.y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (nx <= 0 || ny <= 0 || nx >= d.width - 1 || ny >= d.height - 1) continue;
            if (inRoom(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Wall) carveFloor(d, nx, ny);
        }
    }

    return true;
}


// -----------------------------------------------------------------------------
// Poisson-disc sampling + Delaunay triangulation helpers
//
// Used by the RoomsGraph (\"ruins\") generator to place room centers with a
// blue-noise distribution and to build a sparse planar adjacency graph.
// -----------------------------------------------------------------------------
// NOTE: Poisson-disc sampling itself lives in src/poisson_disc.hpp
// (procgen::poissonDiscSample2D). Keeping it in a header allows reuse across
// multiple procgen systems (rooms, overworld POIs, etc.) without duplicating
// the Bridson implementation.

struct DelaunayTri {
    int a = 0;
    int b = 0;
    int c = 0;
    double cx = 0.0;
    double cy = 0.0;
    double r2 = 0.0;
};

static bool computeCircumcircle(const Vec2i& A, const Vec2i& B, const Vec2i& C, double& outCx, double& outCy, double& outR2) {
    const double ax = static_cast<double>(A.x);
    const double ay = static_cast<double>(A.y);
    const double bx = static_cast<double>(B.x);
    const double by = static_cast<double>(B.y);
    const double cx = static_cast<double>(C.x);
    const double cy = static_cast<double>(C.y);

    const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::fabs(d) < 1e-12) return false;

    const double a2 = ax * ax + ay * ay;
    const double b2 = bx * bx + by * by;
    const double c2 = cx * cx + cy * cy;

    outCx = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d;
    outCy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;

    const double dx = outCx - ax;
    const double dy = outCy - ay;
    outR2 = dx * dx + dy * dy;
    return true;
}

static std::vector<std::pair<int, int>> delaunayEdges2D(const std::vector<Vec2i>& pts) {
    std::vector<std::pair<int, int>> out;
    const int n = static_cast<int>(pts.size());
    if (n < 2) return out;
    if (n == 2) {
        out.push_back({0, 1});
        return out;
    }

    // Build a super-triangle that encloses all points.
    int minX = pts[0].x;
    int minY = pts[0].y;
    int maxX = pts[0].x;
    int maxY = pts[0].y;
    for (int i = 1; i < n; ++i) {
        minX = std::min(minX, pts[static_cast<size_t>(i)].x);
        minY = std::min(minY, pts[static_cast<size_t>(i)].y);
        maxX = std::max(maxX, pts[static_cast<size_t>(i)].x);
        maxY = std::max(maxY, pts[static_cast<size_t>(i)].y);
    }

    const double dx = static_cast<double>(maxX - minX);
    const double dy = static_cast<double>(maxY - minY);
    const double delta = std::max(dx, dy);
    const double midx = static_cast<double>(minX + maxX) * 0.5;
    const double midy = static_cast<double>(minY + maxY) * 0.5;

    // Very large triangle.
    Vec2i p0{ static_cast<int>(std::lround(midx - 20.0 * delta)), static_cast<int>(std::lround(midy - 1.0 * delta)) };
    Vec2i p1{ static_cast<int>(std::lround(midx)),                  static_cast<int>(std::lround(midy + 20.0 * delta)) };
    Vec2i p2{ static_cast<int>(std::lround(midx + 20.0 * delta)), static_cast<int>(std::lround(midy - 1.0 * delta)) };

    std::vector<Vec2i> allPts = pts;
    allPts.push_back(p0);
    allPts.push_back(p1);
    allPts.push_back(p2);

    const int s0 = n;
    const int s1 = n + 1;
    const int s2 = n + 2;

    std::vector<DelaunayTri> tris;
    tris.reserve(static_cast<size_t>(n) * 8);

    {
        double ccx = 0.0, ccy = 0.0, r2 = 0.0;
        if (!computeCircumcircle(allPts[static_cast<size_t>(s0)], allPts[static_cast<size_t>(s1)], allPts[static_cast<size_t>(s2)], ccx, ccy, r2)) {
            // Degenerate super-triangle should never happen; bail.
            return out;
        }
        tris.push_back({s0, s1, s2, ccx, ccy, r2});
    }

    auto inCircumcircle = [&](const DelaunayTri& t, const Vec2i& p) {
        const double px = static_cast<double>(p.x);
        const double py = static_cast<double>(p.y);
        const double dx = t.cx - px;
        const double dy = t.cy - py;
        const double dist2 = dx * dx + dy * dy;
        return dist2 <= t.r2 * 1.0000000001; // small epsilon
    };

    for (int pi = 0; pi < n; ++pi) {
        const Vec2i p = allPts[static_cast<size_t>(pi)];

        std::vector<std::pair<int, int>> edgeBuf;
        edgeBuf.reserve(64);

        // Mark triangles whose circumcircle contains p.
        std::vector<uint8_t> bad(tris.size(), uint8_t{0});
        for (size_t ti = 0; ti < tris.size(); ++ti) {
            if (inCircumcircle(tris[ti], p)) {
                bad[ti] = 1;
                const DelaunayTri& t = tris[ti];
                auto addEdge = [&](int u, int v) {
                    if (u > v) std::swap(u, v);
                    edgeBuf.push_back({u, v});
                };
                addEdge(t.a, t.b);
                addEdge(t.b, t.c);
                addEdge(t.c, t.a);
            }
        }

        // Remove bad triangles.
        if (!edgeBuf.empty()) {
            std::vector<DelaunayTri> keep;
            keep.reserve(tris.size());
            for (size_t ti = 0; ti < tris.size(); ++ti) {
                if (!bad[ti]) keep.push_back(tris[ti]);
            }
            tris.swap(keep);

            // Boundary edges are those that appear exactly once.
            std::sort(edgeBuf.begin(), edgeBuf.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });

            std::vector<std::pair<int, int>> boundary;
            for (size_t i = 0; i < edgeBuf.size();) {
                size_t j = i + 1;
                while (j < edgeBuf.size() && edgeBuf[j] == edgeBuf[i]) ++j;
                if (j - i == 1) boundary.push_back(edgeBuf[i]);
                i = j;
            }

            // Stitch new triangles from boundary edges to p.
            for (const auto& e : boundary) {
                const int a = e.first;
                const int b = e.second;
                const int c = pi;

                double ccx = 0.0, ccy = 0.0, r2 = 0.0;
                if (!computeCircumcircle(allPts[static_cast<size_t>(a)], allPts[static_cast<size_t>(b)], allPts[static_cast<size_t>(c)], ccx, ccy, r2)) {
                    continue;
                }
                tris.push_back({a, b, c, ccx, ccy, r2});
            }
        }
    }

    // Collect unique edges from triangles not touching super vertices.
    std::vector<std::pair<int, int>> edges;
    edges.reserve(static_cast<size_t>(n) * 4);

    auto addEdge = [&](int u, int v) {
        if (u < 0 || v < 0) return;
        if (u >= n || v >= n) return;
        if (u == v) return;
        if (u > v) std::swap(u, v);
        edges.push_back({u, v});
    };

    for (const DelaunayTri& t : tris) {
        if (t.a >= n || t.b >= n || t.c >= n) continue;
        addEdge(t.a, t.b);
        addEdge(t.b, t.c);
        addEdge(t.c, t.a);
    }

    std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });
    edges.erase(std::unique(edges.begin(), edges.end(), [](const auto& a, const auto& b) {
        return a.first == b.first && a.second == b.second;
    }), edges.end());

    out = std::move(edges);
    return out;
}



void generateRoomsGraph(Dungeon& d, RNG& rng, int depth) {
    // "Ruins" room generator:
    // - Poisson-disc sample candidate room centers (blue-noise distribution)
    // - Stamp non-overlapping rectangular rooms around those centers
    // - Connect them using Delaunay triangulation -> MST (guaranteed global connectivity)
    // - Add a few extra Delaunay edges for loops (more interesting navigation / flanking)
    // - Add some corridor branches for treasure pockets / dead ends
    //
    // This complements the BSP generator by producing less hierarchical, more "scattered" layouts.

    // Debug stats for #mapstats. (Kept 0 when this generator falls back.)
    d.roomsGraphPoissonPointCount = 0;
    d.roomsGraphPoissonRoomCount = 0;
    d.roomsGraphDelaunayEdgeCount = 0;
    d.roomsGraphLoopEdgeCount = 0;

    // Needs some breathing room; fall back gracefully on tiny maps (unit tests, etc).
    if (d.width < 22 || d.height < 16) {
        generateBspRooms(d, rng);
        return;
    }

    d.rooms.clear();

    const int area = std::max(1, d.width * d.height);

    // Target room count scales with area. Deeper floors get slightly more rooms
    // (more decisions per floor, supports longer runs).
    int target = std::clamp(static_cast<int>(area / 700) + 8, 8, 22);
    if (depth >= 4) target += 1;
    if (depth >= 7) target += 1;
    target = std::clamp(target, 8, 22);

    // Avoid clumping: enforce a minimum center distance. Keep it modest so placement
    // doesn't fail on small maps.
    const int minDim = std::max(1, std::min(d.width, d.height));
    const int baseMinCenterDist = std::clamp((minDim / 6) + 6, 8, 14);

    const int margin = 2;

    // -----------------------------------------------------------------
    // 1) Place rooms: Poisson-disc sample centers, then stamp rectangles.
    // -----------------------------------------------------------------
    const int domMinX = 3;
    const int domMinY = 3;
    const int domMaxX = d.width - 4;
    const int domMaxY = d.height - 4;

    std::vector<Vec2i> sites;
    int minDist = baseMinCenterDist;

    // If we sample too few sites for the current map size/target, relax the spacing and retry.
    for (int pass = 0; pass < 3; ++pass) {
        sites = procgen::poissonDiscSample2D(rng, domMinX, domMinY, domMaxX, domMaxY, static_cast<float>(minDist), 30);
        if (static_cast<int>(sites.size()) >= target) break;

        // Only relax if we're meaningfully under target (keeps distribution stable on small maps).
        if (static_cast<int>(sites.size()) < std::max(4, target / 2)) {
            minDist = std::max(6, minDist - 2);
        } else {
            break;
        }
    }

    d.roomsGraphPoissonPointCount = static_cast<int>(sites.size());

    // Shuffle sites for variety (otherwise Poisson expansion order can bias patterns a bit).
    for (int i = static_cast<int>(sites.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(sites[static_cast<size_t>(i)], sites[static_cast<size_t>(j)]);
    }

    auto tryPlaceRoomAround = [&](const Vec2i& c) -> bool {
        // More tries than the old generator because we're respecting an external center distribution.
        const int tries = 14;

        for (int t = 0; t < tries; ++t) {
            // Room sizes: slightly larger than mines chambers; more "architected" feel.
            int rw = rng.range(5, 15);
            int rh = rng.range(5, 11);

            // Deeper: occasionally allow bigger rooms for set-piece fights.
            if (depth >= 5 && rng.chance(0.35f)) rw = rng.range(8, 18);
            if (depth >= 5 && rng.chance(0.35f)) rh = rng.range(6, 13);

            // Clamp for small maps.
            rw = std::min(rw, d.width - 6);
            rh = std::min(rh, d.height - 6);
            if (rw < 4 || rh < 4) continue;

            // Small center jitter keeps rooms from looking stamped-on-grid.
            const int jx = rng.range(-2, 2);
            const int jy = rng.range(-2, 2);

            int rx = c.x - rw / 2 + jx;
            int ry = c.y - rh / 2 + jy;

            // Keep within safe bounds.
            const int maxRx = std::max(2, d.width - rw - 3);
            const int maxRy = std::max(2, d.height - rh - 3);
            rx = std::clamp(rx, 2, maxRx);
            ry = std::clamp(ry, 2, maxRy);

            bool ok = true;
            for (const Room& r : d.rooms) {
                if (rectsOverlap(r, rx, ry, rw, rh, margin)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;

            carveRect(d, rx, ry, rw, rh, TileType::Floor);
            d.rooms.push_back({rx, ry, rw, rh, RoomType::Normal});
            return true;
        }

        return false;
    };

    // Primary placement: attempt rooms at Poisson sites.
    for (const Vec2i& s : sites) {
        if (static_cast<int>(d.rooms.size()) >= target) break;
        (void)tryPlaceRoomAround(s);
    }

    // Opportunistic fill: try a small number of random centers (keeps room count stable across seeds).
    int fillAttempts = std::max(0, (target - static_cast<int>(d.rooms.size())) * 160);
    while (static_cast<int>(d.rooms.size()) < target && fillAttempts-- > 0) {
        Vec2i c{ rng.range(domMinX, domMaxX), rng.range(domMinY, domMaxY) };
        (void)tryPlaceRoomAround(c);
    }

    d.roomsGraphPoissonRoomCount = static_cast<int>(d.rooms.size());

    // If placement failed badly, fall back to a safer generator.
    if (d.rooms.size() < 4) {
        // Clear stats: this level is effectively BSP, not ruins-graph.
        d.roomsGraphPoissonPointCount = 0;
        d.roomsGraphPoissonRoomCount = 0;
        d.roomsGraphDelaunayEdgeCount = 0;
        d.roomsGraphLoopEdgeCount = 0;

        fillWalls(d);
        generateBspRooms(d, rng);
        return;
    }

    // Precompute which tiles are inside rooms for corridor routing + later passes.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (d.inBounds(x, y)) inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    struct Edge {
        int a = 0;
        int b = 0;
        int w = 0;
    };

    const int n = static_cast<int>(d.rooms.size());

    // Centers used for graph construction (use actual room centers after stamping).
    std::vector<Vec2i> centers;
    centers.reserve(d.rooms.size());
    for (const Room& r : d.rooms) centers.push_back({r.cx(), r.cy()});

    // -----------------------------------------------------------------
    // 2) Connect rooms: Delaunay triangulation -> MST (+ extra loops)
    // -----------------------------------------------------------------
    std::vector<Edge> edges;

    auto buildCompleteGraph = [&]() {
        edges.clear();
        edges.reserve(static_cast<size_t>(n * (n - 1) / 2));
        for (int i = 0; i < n; ++i) {
            const Vec2i ca = centers[static_cast<size_t>(i)];
            for (int j = i + 1; j < n; ++j) {
                const Vec2i cb = centers[static_cast<size_t>(j)];
                const int w = std::abs(ca.x - cb.x) + std::abs(ca.y - cb.y);
                edges.push_back({i, j, w});
            }
        }
    };

    auto graphConnected = [&](const std::vector<Edge>& ed) -> bool {
        if (n <= 1) return true;
        DSU tdsu(n);
        for (const Edge& e : ed) (void)tdsu.unite(e.a, e.b);
        const int root = tdsu.find(0);
        for (int i = 1; i < n; ++i) {
            if (tdsu.find(i) != root) return false;
        }
        return true;
    };

    // Candidate edges from Delaunay triangulation (sparse planar adjacency graph).
    const auto dtEdges = delaunayEdges2D(centers);
    edges.reserve(dtEdges.size());
    for (const auto& e : dtEdges) {
        const int a = e.first;
        const int b = e.second;
        if (a < 0 || b < 0 || a >= n || b >= n || a == b) continue;
        const Vec2i ca = centers[static_cast<size_t>(a)];
        const Vec2i cb = centers[static_cast<size_t>(b)];
        const int w = std::abs(ca.x - cb.x) + std::abs(ca.y - cb.y);
        edges.push_back({a, b, w});
    }

    bool usedDelaunay = !edges.empty() && edges.size() >= static_cast<size_t>(n - 1) && graphConnected(edges);
    if (!usedDelaunay) {
        buildCompleteGraph();
    }

    if (usedDelaunay) {
        d.roomsGraphDelaunayEdgeCount = static_cast<int>(edges.size());
    } else {
        d.roomsGraphDelaunayEdgeCount = 0;
    }

    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.w != b.w) return a.w < b.w;
        if (a.a != b.a) return a.a < b.a;
        return a.b < b.b;
    });

    // Connect rooms with an MST (guaranteed global connectivity).
    DSU dsu(n);
    std::vector<uint8_t> usedEdge(edges.size(), uint8_t{0});

    int used = 0;
    for (size_t ei = 0; ei < edges.size() && used < n - 1; ++ei) {
        const Edge& e = edges[ei];
        if (dsu.unite(e.a, e.b)) {
            connectRooms(d, d.rooms[static_cast<size_t>(e.a)], d.rooms[static_cast<size_t>(e.b)], rng, inRoom);
            usedEdge[ei] = 1;
            used++;
        }
    }

    // Very rare: if something went wrong and we didn't fully connect, patch-connect components with shortest remaining edges.
    if (used < n - 1) {
        for (size_t guard = 0; guard < edges.size() && used < n - 1; ++guard) {
            for (size_t ei = 0; ei < edges.size() && used < n - 1; ++ei) {
                const Edge& e = edges[ei];
                if (dsu.unite(e.a, e.b)) {
                    connectRooms(d, d.rooms[static_cast<size_t>(e.a)], d.rooms[static_cast<size_t>(e.b)], rng, inRoom);
                    usedEdge[ei] = 1;
                    used++;
                }
            }
        }
    }

    // Add some extra loops so the floor isn't a pure tree.
    int loops = 0;
    const int wantLoops = std::clamp(n / 4, 1, 6);
    const float loopChance = 0.18f + 0.01f * static_cast<float>(std::clamp(depth - 1, 0, 8));

    for (size_t ei = 0; ei < edges.size() && loops < wantLoops; ++ei) {
        if (usedEdge[ei]) continue;
        if (!rng.chance(loopChance)) continue;
        const Edge& e = edges[ei];
        connectRooms(d, d.rooms[static_cast<size_t>(e.a)], d.rooms[static_cast<size_t>(e.b)], rng, inRoom);
        usedEdge[ei] = 1;
        loops++;
    }

    // Ensure at least one loop when possible (helps avoid overly linear seeds).
    if (loops == 0) {
        for (size_t ei = 0; ei < edges.size(); ++ei) {
            if (usedEdge[ei]) continue;
            const Edge& e = edges[ei];
            connectRooms(d, d.rooms[static_cast<size_t>(e.a)], d.rooms[static_cast<size_t>(e.b)], rng, inRoom);
            loops = 1;
            break;
        }
    }

    d.roomsGraphLoopEdgeCount = loops;

    // Branch corridors (dead ends) for optional treasure pockets / escape routes.
    const int branches = std::max(6, n * 2);
    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    for (int i = 0; i < branches; ++i) {
        int x = rng.range(2, d.width - 3);
        int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Floor) continue;
        if (inRoom[idx(x, y)] != 0) continue; // prefer corridors

        Vec2i dir = dirs[rng.range(0, 3)];
        int nx = x + dir.x;
        int ny = y + dir.y;
        if (!d.inBounds(nx, ny)) continue;
        if (nx <= 0 || ny <= 0 || nx >= d.width - 1 || ny >= d.height - 1) continue;
        if (inRoom[idx(nx, ny)] != 0) continue;
        if (d.at(nx, ny).type != TileType::Wall) continue;

        int len = rng.range(3, 10);
        int cx = x;
        int cy = y;
        Vec2i last = dir;

        for (int step = 0; step < len; ++step) {
            // Occasional bend.
            if (step >= 2 && rng.chance(0.22f)) {
                Vec2i cand = dirs[rng.range(0, 3)];
                if (cand.x == -last.x && cand.y == -last.y) continue;
                last = cand;
            }

            cx += last.x;
            cy += last.y;
            if (!d.inBounds(cx, cy)) break;
            if (cx <= 0 || cy <= 0 || cx >= d.width - 1 || cy >= d.height - 1) break;
            if (inRoom[idx(cx, cy)] != 0) break;

            TileType tt = d.at(cx, cy).type;
            if (!(tt == TileType::Wall || tt == TileType::Floor)) break;

            carveFloor(d, cx, cy);

            // Stop if we accidentally connected to existing space; keep it branchy.
            if (tt == TileType::Floor && step >= 1) break;
        }
    }

    // Place stairs: start in the room closest to map center (gentler openings),
    // then pick the farthest room by BFS for the down stairs.
    Vec2i mid{ d.width / 2, d.height / 2 };
    int startRoomIdx = 0;
    int bestMd = 1000000000;
    for (int i = 0; i < n; ++i) {
        const Room& r = d.rooms[static_cast<size_t>(i)];
        const int md = std::abs(r.cx() - mid.x) + std::abs(r.cy() - mid.y);
        if (md < bestMd) {
            bestMd = md;
            startRoomIdx = i;
        }
    }

    const Room& startRoom = d.rooms[static_cast<size_t>(startRoomIdx)];
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        carveFloor(d, d.stairsUp.x, d.stairsUp.y);
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    auto dist = bfsDistanceMap(d, d.stairsUp);
    int bestRoomIdx = startRoomIdx;
    int bestDist = -1;
    for (int i = 0; i < n; ++i) {
        const Room& r = d.rooms[static_cast<size_t>(i)];
        const int cx = r.cx();
        const int cy = r.cy();
        if (!d.inBounds(cx, cy)) continue;
        const int d0 = dist[static_cast<size_t>(cy * d.width + cx)];
        if (d0 > bestDist) {
            bestDist = d0;
            bestRoomIdx = i;
        }
    }

    const Room& endRoom = d.rooms[static_cast<size_t>(bestRoomIdx)];
    d.stairsDown = { endRoom.cx(), endRoom.cy() };
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }

    // Extra corridor doors (beyond room-connection doors) make corridors tactically interesting.
    placeStrategicCorridorDoors(d, rng, inRoom, 0.82f);
}

void generateMines(Dungeon& d, RNG& rng, int depth) {
    (void)depth;

    // A mines floor wants enough room for chambers + winding connections.
    if (d.width < 22 || d.height < 16) {
        generateBspRooms(d, rng);
        return;
    }

    d.rooms.clear();

    // Carve many small chambers, then connect them with wandering tunnels.
    const int area = std::max(1, d.width * d.height);

    int chamberCount = std::clamp(static_cast<int>(area / 800) + 4, 5, 20);
    if (depth >= 6) chamberCount = std::min(22, chamberCount + 2);

    // Scale down a bit on non-default map sizes so tiny test maps don't overpack.
    const float baseArea = static_cast<float>(Dungeon::DEFAULT_W) * static_cast<float>(Dungeon::DEFAULT_H);
    const float areaScale = std::clamp(static_cast<float>(area) / baseArea, 0.25f, 2.0f);
    chamberCount = std::clamp(static_cast<int>(std::lround(static_cast<float>(chamberCount) / std::sqrt(areaScale))), 4, 22);

    const int margin = 2;
    int attempts = chamberCount * 90;

    while (static_cast<int>(d.rooms.size()) < chamberCount && attempts-- > 0) {
        // Chamber sizes: keep them modest so tunnels matter.
        int rw = rng.range(5, 12);
        int rh = rng.range(5, 10);

        // Clamp for small maps.
        rw = std::min(rw, d.width - 6);
        rh = std::min(rh, d.height - 6);
        if (rw < 4 || rh < 4) continue;

        const int rx = rng.range(2, std::max(2, d.width - rw - 3));
        const int ry = rng.range(2, std::max(2, d.height - rh - 3));

        bool ok = true;
        for (const Room& r : d.rooms) {
            if (rectsOverlap(r, rx, ry, rw, rh, margin)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        carveRect(d, rx, ry, rw, rh, TileType::Floor);

        Room r;
        r.x = rx;
        r.y = ry;
        r.w = rw;
        r.h = rh;
        r.type = RoomType::Normal;
        d.rooms.push_back(r);
    }

    // If placement failed badly, fall back to a safer generator.
    if (d.rooms.size() < 3) {
        fillWalls(d);
        generateBspRooms(d, rng);
        return;
    }

    // Mark chamber footprint so tunnel carving avoids cutting through rooms.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    struct Edge {
        int a = 0;
        int b = 0;
        int w = 0;
    };

    const int n = static_cast<int>(d.rooms.size());
    std::vector<Edge> edges;
    edges.reserve(static_cast<size_t>(n * (n - 1) / 2));
    for (int i = 0; i < n; ++i) {
        Vec2i ca = { d.rooms[static_cast<size_t>(i)].cx(), d.rooms[static_cast<size_t>(i)].cy() };
        for (int j = i + 1; j < n; ++j) {
            Vec2i cb = { d.rooms[static_cast<size_t>(j)].cx(), d.rooms[static_cast<size_t>(j)].cy() };
            const int w = std::abs(ca.x - cb.x) + std::abs(ca.y - cb.y);
            edges.push_back({i, j, w});
        }
    }

    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.w != b.w) return a.w < b.w;
        if (a.a != b.a) return a.a < b.a;
        return a.b < b.b;
    });

    auto connectRoomsMine = [&](const Room& a, const Room& b, float doorChance) {
        DoorPick da = pickDoorOnRoomSmart(a, d, rng, {b.cx(), b.cy()}, &a);
        DoorPick db = pickDoorOnRoomSmart(b, d, rng, {a.cx(), a.cy()}, &b);

        // Mines feel more like open tunnels; use doors sparingly.
        if (rng.chance(doorChance)) {
            if (d.inBounds(da.doorInside.x, da.doorInside.y)) d.at(da.doorInside.x, da.doorInside.y).type = TileType::DoorClosed;
        }
        if (rng.chance(doorChance)) {
            if (d.inBounds(db.doorInside.x, db.doorInside.y)) d.at(db.doorInside.x, db.doorInside.y).type = TileType::DoorClosed;
        }

        // Wander-carve the tunnel; fall back to A* if we get stuck.
        const int man = std::abs(da.corridorStart.x - db.corridorStart.x) + std::abs(da.corridorStart.y - db.corridorStart.y);
        const int maxSteps = std::max(20, man * 6);

        if (!carveCorridorWander(d, rng, da.corridorStart, db.corridorStart, inRoom, maxSteps, 0.78f)) {
            (void)carveCorridorAStar(d, rng, da.corridorStart, db.corridorStart, inRoom);
        }
    };

    // Connect chambers with a minimum spanning tree so the level is always fully navigable.
    DSU dsu(n);
    std::vector<uint8_t> usedEdge(edges.size(), uint8_t{0});

    int used = 0;
    for (size_t ei = 0; ei < edges.size() && used < n - 1; ++ei) {
        const Edge& e = edges[ei];
        if (dsu.unite(e.a, e.b)) {
            connectRoomsMine(d.rooms[static_cast<size_t>(e.a)], d.rooms[static_cast<size_t>(e.b)], 0.18f);
            usedEdge[ei] = 1;
            used++;
        }
    }

    // Add a few extra loops so the mines aren't a pure tree (supports tactical flanking / escape routes).
    int loops = 0;
    const int maxLoops = std::clamp(n / 3, 1, 6);
    for (size_t ei = 0; ei < edges.size() && loops < maxLoops; ++ei) {
        if (usedEdge[ei]) continue;
        if (!rng.chance(0.16f)) continue;
        const Edge& e = edges[ei];
        connectRoomsMine(d.rooms[static_cast<size_t>(e.a)], d.rooms[static_cast<size_t>(e.b)], 0.10f);
        loops++;
    }

    // Branch tunnels (dead ends / ore pockets).
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };
    auto isInRoom = [&](int x, int y) -> bool { return inRoom[idx(x, y)] != 0; };

    const int branches = std::max(6, n * 2);
    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    for (int i = 0; i < branches; ++i) {
        int x = rng.range(1, d.width - 2);
        int y = rng.range(1, d.height - 2);
        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Floor) continue;
        if (isInRoom(x, y)) continue; // prefer corridors

        const int d0 = rng.range(0, 3);
        Vec2i dir = dirs[d0];

        // Must be able to dig into wall.
        int nx = x + dir.x;
        int ny = y + dir.y;
        if (!d.inBounds(nx, ny)) continue;
        if (nx <= 0 || ny <= 0 || nx >= d.width - 1 || ny >= d.height - 1) continue;
        if (isInRoom(nx, ny)) continue;
        if (d.at(nx, ny).type != TileType::Wall) continue;

        int len = rng.range(4, 12);
        int cx = x;
        int cy = y;
        Vec2i last = dir;

        for (int step = 0; step < len; ++step) {
            // Occasional bend.
            if (step >= 2 && rng.chance(0.18f)) {
                Vec2i cand = dirs[rng.range(0, 3)];
                // Don't reverse.
                if (cand.x == -last.x && cand.y == -last.y) {
                    cand = dirs[(rng.range(0, 2) + 1) & 3];
                }
                last = cand;
            }

            cx += last.x;
            cy += last.y;
            if (!d.inBounds(cx, cy)) break;
            if (cx <= 0 || cy <= 0 || cx >= d.width - 1 || cy >= d.height - 1) break;
            if (isInRoom(cx, cy)) break;

            TileType tt = d.at(cx, cy).type;
            if (!(tt == TileType::Wall || tt == TileType::Floor)) break;

            carveFloor(d, cx, cy);

            // Stop if we accidentally connected to existing corridor space; this keeps the branch "branchy".
            if (tt == TileType::Floor && step >= 1) break;
        }

        // Sometimes carve a tiny pocket at the end (feels like a miner cut a side alcove).
        if (rng.chance(0.35f)) {
            int pw = rng.range(2, 4);
            int ph = rng.range(2, 4);
            int px = cx - pw / 2;
            int py = cy - ph / 2;
            for (int yy = py; yy < py + ph; ++yy) {
                for (int xx = px; xx < px + pw; ++xx) {
                    if (!d.inBounds(xx, yy)) continue;
                    if (xx <= 0 || yy <= 0 || xx >= d.width - 1 || yy >= d.height - 1) continue;
                    if (isInRoom(xx, yy)) continue;
                    if (d.at(xx, yy).type == TileType::Wall) carveFloor(d, xx, yy);
                }
            }
        }
    }

    // A final gentle roughening pass (wider tunnels / small nicks).
    const float roughChance = 0.055f + 0.005f * static_cast<float>(std::min(8, std::max(0, depth - 1)));
    for (int y = 2; y < d.height - 2; ++y) {
        for (int x = 2; x < d.width - 2; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (isInRoom(x, y)) continue;
            if (!rng.chance(roughChance)) continue;

            const Vec2i dv = dirs[rng.range(0, 3)];
            const int ax = x + dv.x;
            const int ay = y + dv.y;
            if (!d.inBounds(ax, ay)) continue;
            if (ax <= 0 || ay <= 0 || ax >= d.width - 1 || ay >= d.height - 1) continue;
            if (isInRoom(ax, ay)) continue;

            if (d.at(ax, ay).type == TileType::Wall) carveFloor(d, ax, ay);
        }
    }

    // Place stairs: choose an arbitrary chamber as the start, then pick the farthest chamber by BFS.
    const int startRoomIdx = rng.range(0, static_cast<int>(d.rooms.size()) - 1);
    const Room& startRoom = d.rooms[static_cast<size_t>(startRoomIdx)];
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        carveFloor(d, d.stairsUp.x, d.stairsUp.y);
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    auto dist = bfsDistanceMap(d, d.stairsUp);
    int bestRoomIdx = startRoomIdx;
    int bestDist = -1;

    for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
        const Room& r = d.rooms[static_cast<size_t>(i)];
        int cx = r.cx();
        int cy = r.cy();
        if (!d.inBounds(cx, cy)) continue;
        int d0 = dist[static_cast<size_t>(cy * d.width + cx)];
        if (d0 > bestDist) {
            bestDist = d0;
            bestRoomIdx = i;
        }
    }

    const Room& endRoom = d.rooms[static_cast<size_t>(bestRoomIdx)];
    d.stairsDown = { endRoom.cx(), endRoom.cy() };
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }

    // Mines have fewer "manufactured" doors than BSP floors, but still benefit from an occasional LOS breaker.
    placeStrategicCorridorDoors(d, rng, inRoom, 0.55f);
}


// ------------------------------------------------------------
// Cavern generator variants
//
// The default cavern generator uses a classic cellular automata smooth+largest-component pass.
// This round adds an alternate "metaball" cavern mode:
//   1) Sample a handful of blob centers with radii.
//   2) Evaluate an implicit field f(x,y) = Σ (r_i^2 / (d^2 + 1)).
//   3) Threshold by quantile to hit a stable target floor coverage.
//
// This produces smoother, more organic cave silhouettes with fewer grid artifacts,
// while keeping the same robustness guarantees (largest component + fallback).
// ------------------------------------------------------------

static bool generateCavernMetaballsBase(Dungeon& d, RNG& rng, int depth, int& outBlobCount, int& outKept) {
    outBlobCount = 0;
    outKept = 0;

    const int W = d.width;
    const int H = d.height;
    const int area = std::max(1, W * H);

    // Blob count: scales with area; gently increases with depth.
    // Default map: ~9 blobs.
    const float base = static_cast<float>(area) / 750.0f;
    int blobs = static_cast<int>(std::lround(base));
    blobs += std::min(6, std::max(0, depth - 3)) / 2;
    blobs = std::clamp(blobs, 6, 18);

    struct Blob {
        float cx = 0.0f;
        float cy = 0.0f;
        float r = 0.0f;
        float r2 = 0.0f;
    };

    std::vector<Blob> bs;
    bs.reserve(static_cast<size_t>(blobs));

    auto frand = [&](float lo, float hi) -> float {
        return lo + (hi - lo) * rng.next01();
    };

    // Deeper caves trend slightly tighter.
    const float tight = static_cast<float>(std::max(0, depth - 3));
    const float rMin = std::clamp(6.0f - 0.15f * tight, 4.5f, 6.5f);
    const float rMax = std::clamp(15.0f - 0.25f * tight, 11.0f, 15.5f);

    for (int i = 0; i < blobs; ++i) {
        const float cx = frand(2.0f, static_cast<float>(W - 3));
        const float cy = frand(2.0f, static_cast<float>(H - 3));
        const float r = frand(rMin, rMax);
        bs.push_back({cx, cy, r, r * r});
    }

    outBlobCount = blobs;

    std::vector<float> field(static_cast<size_t>(W * H), 0.0f);
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    // Evaluate implicit field over interior.
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            float v = 0.0f;
            const float fx = static_cast<float>(x) + 0.5f;
            const float fy = static_cast<float>(y) + 0.5f;
            for (const Blob& b : bs) {
                const float dx = fx - b.cx;
                const float dy = fy - b.cy;
                const float d2 = dx * dx + dy * dy;
                v += b.r2 / (d2 + 1.0f);
            }
            field[idx(x, y)] = v;
        }
    }

    // Pick a threshold by quantile so we get a stable floor coverage fraction.
    // Slightly tighter deeper.
    float floorFrac = 0.56f;
    if (depth >= 4) floorFrac -= 0.03f;
    if (depth >= 7) floorFrac -= 0.03f;
    floorFrac = std::clamp(floorFrac, 0.44f, 0.60f);

    std::vector<float> vals;
    vals.reserve(static_cast<size_t>((W - 2) * (H - 2)));
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            vals.push_back(field[idx(x, y)]);
        }
    }
    if (vals.empty()) return false;

    const size_t cut = static_cast<size_t>(
        std::clamp<int>(
            static_cast<int>(std::lround((1.0f - floorFrac) * static_cast<float>(vals.size() - 1))),
            0,
            static_cast<int>(vals.size() - 1)
        )
    );

    std::nth_element(vals.begin(), vals.begin() + static_cast<std::vector<float>::difference_type>(cut), vals.end());
    const float thr = vals[cut];

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            d.at(x, y).type = (field[idx(x, y)] >= thr) ? TileType::Floor : TileType::Wall;
        }
    }

    // Gentle smoothing pass to remove tiny pinholes.
    auto wallCount8 = [&](int x, int y) {
        int c = 0;
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                if (ox == 0 && oy == 0) continue;
                int nx = x + ox;
                int ny = y + oy;
                if (!d.inBounds(nx, ny)) { c++; continue; }
                if (d.at(nx, ny).type == TileType::Wall) c++;
            }
        }
        return c;
    };

    std::vector<TileType> next(static_cast<size_t>(W * H), TileType::Wall);

    const int iters = 2;
    for (int it = 0; it < iters; ++it) {
        for (int y = 1; y < H - 1; ++y) {
            for (int x = 1; x < W - 1; ++x) {
                int wc = wallCount8(x, y);
                TileType cur = d.at(x, y).type;
                if (wc >= 5) next[idx(x, y)] = TileType::Wall;
                else if (wc <= 2) next[idx(x, y)] = TileType::Floor;
                else next[idx(x, y)] = cur;
            }
        }
        for (int y = 1; y < H - 1; ++y) {
            for (int x = 1; x < W - 1; ++x) {
                d.at(x, y).type = next[idx(x, y)];
            }
        }
    }

    // Keep the largest connected floor region (4-neighborhood).
    std::vector<int> comp(static_cast<size_t>(W * H), -1);
    std::vector<int> compSize;
    compSize.reserve(64);

    auto isFloor = [&](int x, int y) {
        return d.at(x, y).type == TileType::Floor;
    };

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    int compIdx = 0;
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (!isFloor(x, y)) continue;
            size_t ii = idx(x, y);
            if (comp[ii] != -1) continue;

            // BFS
            int count = 0;
            std::deque<Vec2i> q;
            q.push_back({x, y});
            comp[ii] = compIdx;
            while (!q.empty()) {
                Vec2i p = q.front();
                q.pop_front();
                count++;
                for (auto& dv : dirs) {
                    int nx = p.x + dv[0];
                    int ny = p.y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (!isFloor(nx, ny)) continue;
                    size_t jj = idx(nx, ny);
                    if (comp[jj] != -1) continue;
                    comp[jj] = compIdx;
                    q.push_back({nx, ny});
                }
            }
            compSize.push_back(count);
            compIdx++;
        }
    }

    if (compSize.empty()) return false;

    int bestComp = 0;
    for (int i = 1; i < static_cast<int>(compSize.size()); ++i) {
        if (compSize[static_cast<size_t>(i)] > compSize[static_cast<size_t>(bestComp)]) bestComp = i;
    }

    int kept = 0;
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (!isFloor(x, y)) continue;
            if (comp[idx(x, y)] != bestComp) {
                d.at(x, y).type = TileType::Wall;
            } else {
                kept++;
            }
        }
    }

    outKept = kept;
    if (kept < area / 6) return false;

    return true;
}

static void finishCavernRoomsAndStairs(Dungeon& d, RNG& rng) {
    d.rooms.clear();

    // Start chamber on a floor tile nearest the center (robust for odd cave shapes).
    const Vec2i center{ d.width / 2, d.height / 2 };
    Vec2i best = center;
    int bestMd = std::numeric_limits<int>::max();

    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            const int md = std::abs(x - center.x) + std::abs(y - center.y);
            if (md < bestMd) {
                bestMd = md;
                best = {x, y};
            }
        }
    }

    if (bestMd == std::numeric_limits<int>::max()) {
        // Shouldn't happen if caller ensured a kept region, but be defensive.
        best = {1, 1};
    }

    const int sw = rng.range(6, 8);
    const int sh = rng.range(5, 7);
    int sx = clampi(best.x - sw / 2, 1, d.width - sw - 1);
    int sy = clampi(best.y - sh / 2, 1, d.height - sh - 1);
    carveRect(d, sx, sy, sw, sh, TileType::Floor);
    d.rooms.push_back({sx, sy, sw, sh, RoomType::Normal});

    // Extra chambers scattered through the cavern to create "landmarks".
    const int extraRooms = rng.range(6, 10);
    for (int i = 0; i < extraRooms; ++i) {
        Vec2i p = d.randomFloor(rng, true);
        int rw = rng.range(4, 8);
        int rh = rng.range(4, 7);
        int rx = clampi(p.x - rw / 2, 1, d.width - rw - 1);
        int ry = clampi(p.y - rh / 2, 1, d.height - rh - 1);
        carveRect(d, rx, ry, rw, rh, TileType::Floor);
        d.rooms.push_back({rx, ry, rw, rh, RoomType::Normal});
    }

    // Place stairs using distance on passable tiles.
    const Room& startRoom = d.rooms.front();
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.stairsUp = {1, 1};

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.stairsDown = {d.width - 2, d.height - 2};
}

void generateCavern(Dungeon& d, RNG& rng, int depth) {
    // Reset telemetry (not serialized).
    d.cavernMetaballsUsed = false;
    d.cavernMetaballBlobCount = 0;
    d.cavernMetaballKeptTiles = 0;

    // Sometimes use metaballs to form a smoother, more organic cave silhouette.
    // Keep early floors slightly more predictable by requiring depth >= 3.
    const float pMeta = std::min(0.70f, 0.35f + 0.04f * static_cast<float>(std::max(0, depth - 3)));

    if (depth >= 3 && rng.chance(pMeta)) {
        int blobs = 0;
        int kept = 0;
        if (generateCavernMetaballsBase(d, rng, depth, blobs, kept)) {
            d.cavernMetaballsUsed = true;
            d.cavernMetaballBlobCount = blobs;
            d.cavernMetaballKeptTiles = kept;
            finishCavernRoomsAndStairs(d, rng);
            return;
        }
        // Failed (rare): fall back to cellular automata below.
    }

    // Cellular automata cavern generator.
    // Start with noisy walls/floors, smooth, then keep the largest connected region.
    const float baseFloor = 0.58f;
    const float depthTighten = 0.01f * static_cast<float>(std::min(10, std::max(0, depth - 3)));
    const float floorChance = std::max(0.45f, baseFloor - depthTighten);

    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            d.at(x, y).type = rng.chance(floorChance) ? TileType::Floor : TileType::Wall;
        }
    }

    auto wallCount8 = [&](int x, int y) {
        int c = 0;
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                if (ox == 0 && oy == 0) continue;
                int nx = x + ox;
                int ny = y + oy;
                if (!d.inBounds(nx, ny)) { c++; continue; }
                if (d.at(nx, ny).type == TileType::Wall) c++;
            }
        }
        return c;
    };

    std::vector<TileType> next(static_cast<size_t>(d.width * d.height), TileType::Wall);
    auto idx = [&](int x, int y) { return static_cast<size_t>(y * d.width + x); };

    const int iters = 5;
    for (int it = 0; it < iters; ++it) {
        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                int wc = wallCount8(x, y);
                TileType cur = d.at(x, y).type;
                if (wc >= 5) next[idx(x, y)] = TileType::Wall;
                else if (wc <= 2) next[idx(x, y)] = TileType::Floor;
                else next[idx(x, y)] = cur;
            }
        }
        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                d.at(x, y).type = next[idx(x, y)];
            }
        }
    }

    // Keep the largest connected floor region (4-neighborhood).
    std::vector<int> comp(static_cast<size_t>(d.width * d.height), -1);
    std::vector<int> compSize;
    compSize.reserve(64);

    auto isFloor = [&](int x, int y) {
        TileType t = d.at(x, y).type;
        return (t == TileType::Floor);
    };

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    int compIdx = 0;
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (!isFloor(x, y)) continue;
            size_t ii = idx(x, y);
            if (comp[ii] != -1) continue;

            // BFS
            int count = 0;
            std::deque<Vec2i> q;
            q.push_back({x, y});
            comp[ii] = compIdx;
            while (!q.empty()) {
                Vec2i p = q.front();
                q.pop_front();
                count++;
                for (auto& dv : dirs) {
                    int nx = p.x + dv[0];
                    int ny = p.y + dv[1];
                    if (!d.inBounds(nx, ny)) continue;
                    if (!isFloor(nx, ny)) continue;
                    size_t jj = idx(nx, ny);
                    if (comp[jj] != -1) continue;
                    comp[jj] = compIdx;
                    q.push_back({nx, ny});
                }
            }
            compSize.push_back(count);
            compIdx++;
        }
    }

    if (compSize.empty()) {
        // Fallback.
        fillWalls(d);
        generateBspRooms(d, rng);
        return;
    }

    int bestComp = 0;
    for (int i = 1; i < static_cast<int>(compSize.size()); ++i) {
        if (compSize[static_cast<size_t>(i)] > compSize[static_cast<size_t>(bestComp)]) bestComp = i;
    }

    int kept = 0;
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (!isFloor(x, y)) continue;
            if (comp[idx(x, y)] != bestComp) {
                d.at(x, y).type = TileType::Wall;
            } else {
                kept++;
            }
        }
    }

    // If we ended up with a tiny cavern, fall back.
    if (kept < (d.width * d.height) / 6) {
        fillWalls(d);
        generateBspRooms(d, rng);
        return;
    }

    finishCavernRoomsAndStairs(d, rng);
}

void generateMaze(Dungeon& d, RNG& rng, int depth) {
    // Maze floors are "perfect maze" corridors on an odd-cell lattice,
    // with a handful of carved chambers and strategic doors for LOS play.
    //
    // Round 97: add Wilson's algorithm (loop-erased random walks) as an
    // alternate perfect-maze generator. This produces a *uniform spanning tree*
    // over the grid cells, giving a notably different corridor texture vs.
    // the classic recursive backtracker.

    (void)depth;

    // Telemetry reset (also cleared in Dungeon::generate()).
    d.mazeAlgorithm = MazeAlgorithm::None;
    d.mazeChamberCount = 0;
    d.mazeBreakCount = 0;
    d.mazeWilsonWalkCount = 0;
    d.mazeWilsonStepCount = 0;
    d.mazeWilsonLoopEraseCount = 0;
    d.mazeWilsonMaxPathLen = 0;

    const int cellW = (d.width - 1) / 2;
    const int cellH = (d.height - 1) / 2;
    if (cellW <= 1 || cellH <= 1) {
        generateBspRooms(d, rng);
        return;
    }

    auto cellToPos = [&](int cx, int cy) -> Vec2i {
        return { 1 + cx * 2, 1 + cy * 2 };
    };
    auto cidx = [&](int cx, int cy) { return static_cast<size_t>(cy * cellW + cx); };

    // Pick the perfect-maze algorithm.
    // Bias Wilson a little more at deeper depths so mazes keep feeling fresh.
    float pWilson = 0.38f;
    if (depth > 1) {
        const int t = std::clamp(depth - 2, 0, 7);
        pWilson = std::clamp(0.38f + 0.05f * static_cast<float>(t), 0.38f, 0.72f);
    }
    const bool useWilson = rng.chance(pWilson);
    d.mazeAlgorithm = useWilson ? MazeAlgorithm::Wilson : MazeAlgorithm::Backtracker;

    // ---------------------------
    // 1) Carve a perfect maze
    // ---------------------------

    const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    auto carveEdge = [&](int aCx, int aCy, int bCx, int bCy) {
        Vec2i a = cellToPos(aCx, aCy);
        Vec2i b = cellToPos(bCx, bCy);
        Vec2i mid{ (a.x + b.x) / 2, (a.y + b.y) / 2 };
        if (d.inBounds(a.x, a.y)) d.at(a.x, a.y).type = TileType::Floor;
        if (d.inBounds(mid.x, mid.y)) d.at(mid.x, mid.y).type = TileType::Floor;
        if (d.inBounds(b.x, b.y)) d.at(b.x, b.y).type = TileType::Floor;
    };

    if (!useWilson) {
        // Classic recursive backtracker on the cell grid.
        std::vector<uint8_t> vis(static_cast<size_t>(cellW * cellH), uint8_t{0});
        std::vector<Vec2i> stack;
        stack.reserve(static_cast<size_t>(cellW * cellH));

        const int startCx = cellW / 2;
        const int startCy = cellH / 2;
        stack.push_back({startCx, startCy});
        vis[cidx(startCx, startCy)] = 1;
        Vec2i sp = cellToPos(startCx, startCy);
        d.at(sp.x, sp.y).type = TileType::Floor;

        while (!stack.empty()) {
            Vec2i cur = stack.back();

            // Collect unvisited neighbors.
            std::vector<Vec2i> neigh;
            neigh.reserve(4);
            for (auto& dv : dirs4) {
                int nx = cur.x + dv[0];
                int ny = cur.y + dv[1];
                if (nx < 0 || ny < 0 || nx >= cellW || ny >= cellH) continue;
                if (vis[cidx(nx, ny)] != 0) continue;
                neigh.push_back({nx, ny});
            }

            if (neigh.empty()) {
                stack.pop_back();
                continue;
            }

            Vec2i nxt = neigh[static_cast<size_t>(rng.range(0, static_cast<int>(neigh.size()) - 1))];
            carveEdge(cur.x, cur.y, nxt.x, nxt.y);
            vis[cidx(nxt.x, nxt.y)] = 1;
            stack.push_back(nxt);
        }

    } else {
        // Wilson's algorithm: build a uniform spanning tree using loop-erased random walks.
        //
        // Implementation notes:
        //  - We store the current walk as a vector<int> of cell indices.
        //  - posInPath[cell] stores its index in the current path, or -1 if not present.
        //  - When the walk revisits a cell already in the path, we erase the loop by
        //    truncating the vector back to that index.

        const int nCells = cellW * cellH;
        auto idxToCxCy = [&](int idx) -> Vec2i { return { idx % cellW, idx / cellW }; };

        std::vector<uint8_t> inTree(static_cast<size_t>(nCells), uint8_t{0});
        std::vector<int> posInPath(static_cast<size_t>(nCells), -1);

        // Choose a root in (slightly) random central-ish area.
        const int rootCx = clampi(cellW / 2 + rng.range(-1, 1), 0, cellW - 1);
        const int rootCy = clampi(cellH / 2 + rng.range(-1, 1), 0, cellH - 1);
        const int rootIdx = static_cast<int>(cidx(rootCx, rootCy));
        inTree[static_cast<size_t>(rootIdx)] = 1;
        Vec2i rp = cellToPos(rootCx, rootCy);
        d.at(rp.x, rp.y).type = TileType::Floor;

        // Random processing order for start cells (Fisher-Yates).
        std::vector<int> order(static_cast<size_t>(nCells), 0);
        for (int i = 0; i < nCells; ++i) order[static_cast<size_t>(i)] = i;
        for (int i = nCells - 1; i > 0; --i) {
            const int j = rng.range(0, i);
            std::swap(order[static_cast<size_t>(i)], order[static_cast<size_t>(j)]);
        }

        std::vector<int> path;
        std::vector<int> touched;
        path.reserve(static_cast<size_t>(nCells));
        touched.reserve(static_cast<size_t>(nCells / 8));

        auto randomNeighbor = [&](int idx) -> int {
            Vec2i c = idxToCxCy(idx);
            // Try a few times to pick a valid step without building a full neighbor list.
            for (int t = 0; t < 8; ++t) {
                const int k = rng.range(0, 3);
                const int nx = c.x + dirs4[k][0];
                const int ny = c.y + dirs4[k][1];
                if (nx < 0 || ny < 0 || nx >= cellW || ny >= cellH) continue;
                return static_cast<int>(cidx(nx, ny));
            }
            // Fallback (should be rare): brute-force collect.
            std::vector<int> nbs;
            nbs.reserve(4);
            for (int k = 0; k < 4; ++k) {
                const int nx = c.x + dirs4[k][0];
                const int ny = c.y + dirs4[k][1];
                if (nx < 0 || ny < 0 || nx >= cellW || ny >= cellH) continue;
                nbs.push_back(static_cast<int>(cidx(nx, ny)));
            }
            return nbs.empty() ? idx : nbs[static_cast<size_t>(rng.range(0, static_cast<int>(nbs.size()) - 1))];
        };

        for (int startIdx : order) {
            if (inTree[static_cast<size_t>(startIdx)] != 0) continue;

            path.clear();
            touched.clear();

            int cur = startIdx;
            while (inTree[static_cast<size_t>(cur)] == 0) {
                // Add cur to the path.
                const int ppos = posInPath[static_cast<size_t>(cur)];
                if (ppos == -1) {
                    posInPath[static_cast<size_t>(cur)] = static_cast<int>(path.size());
                    path.push_back(cur);
                    touched.push_back(cur);
                    if (static_cast<int>(path.size()) > d.mazeWilsonMaxPathLen) d.mazeWilsonMaxPathLen = static_cast<int>(path.size());
                }

                const int nxt = randomNeighbor(cur);
                d.mazeWilsonStepCount += 1;
                cur = nxt;

                // Loop erasure: if we stepped onto a cell already in the current path,
                // truncate back to it.
                if (inTree[static_cast<size_t>(cur)] == 0) {
                    const int seen = posInPath[static_cast<size_t>(cur)];
                    if (seen != -1) {
                        // Remove all vertices after 'seen'.
                        const int oldN = static_cast<int>(path.size());
                        for (int i = seen + 1; i < oldN; ++i) {
                            const int v = path[static_cast<size_t>(i)];
                            posInPath[static_cast<size_t>(v)] = -1;
                        }
                        const int erased = oldN - (seen + 1);
                        if (erased > 0) d.mazeWilsonLoopEraseCount += erased;
                        path.resize(static_cast<size_t>(seen + 1));
                    }
                }
            }

            // cur is now in the tree; commit the loop-erased path.
            const int hit = cur;
            for (size_t i = 0; i < path.size(); ++i) {
                const int aIdx = path[i];
                const int bIdx = (i + 1 < path.size()) ? path[i + 1] : hit;

                Vec2i a = idxToCxCy(aIdx);
                Vec2i b = idxToCxCy(bIdx);
                carveEdge(a.x, a.y, b.x, b.y);

                inTree[static_cast<size_t>(aIdx)] = 1;
            }

            d.mazeWilsonWalkCount += 1;

            // Clear temporary marks.
            for (int v : touched) posInPath[static_cast<size_t>(v)] = -1;
        }
    }

    // ---------------------------
    // 2) Add loops (break walls)
    // ---------------------------
    const int breaksTarget = std::max(6, (cellW * cellH) / 6);
    int breaksDone = 0;
    for (int i = 0; i < breaksTarget; ++i) {
        int x = rng.range(2, d.width - 3);
        int y = rng.range(2, d.height - 3);
        if (d.at(x, y).type != TileType::Wall) continue;

        // Break walls that connect two corridors.
        bool horiz = (d.at(x - 1, y).type == TileType::Floor && d.at(x + 1, y).type == TileType::Floor);
        bool vert  = (d.at(x, y - 1).type == TileType::Floor && d.at(x, y + 1).type == TileType::Floor);
        if (!(horiz || vert)) continue;
        d.at(x, y).type = TileType::Floor;
        breaksDone++;
    }
    d.mazeBreakCount = breaksDone;

    // ---------------------------
    // 3) Chambers + stairs
    // ---------------------------

    // Carve a start chamber on top of an existing corridor near the center.
    Vec2i best = { d.width / 2, d.height / 2 };
    int bestDist = 1000000000;
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            int md = std::abs(x - best.x) + std::abs(y - best.y);
            if (md < bestDist) {
                bestDist = md;
                best = {x, y};
            }
        }
    }
    if (bestDist >= 1e9) {
        fillWalls(d);
        generateBspRooms(d, rng);
        d.mazeAlgorithm = MazeAlgorithm::None;
        return;
    }

    d.rooms.clear();
    const int sw = rng.range(6, 8);
    const int sh = rng.range(5, 7);
    int sx = clampi(best.x - sw / 2, 1, d.width - sw - 1);
    int sy = clampi(best.y - sh / 2, 1, d.height - sh - 1);
    carveRect(d, sx, sy, sw, sh, TileType::Floor);
    d.rooms.push_back({sx, sy, sw, sh, RoomType::Normal});

    // Additional chambers.
    const int extraRooms = rng.range(5, 8);
    for (int i = 0; i < extraRooms; ++i) {
        Vec2i p = d.randomFloor(rng, true);
        int rw = rng.range(4, 8);
        int rh = rng.range(4, 7);
        int rx = clampi(p.x - rw / 2, 1, d.width - rw - 1);
        int ry = clampi(p.y - rh / 2, 1, d.height - rh - 1);
        carveRect(d, rx, ry, rw, rh, TileType::Floor);
        d.rooms.push_back({rx, ry, rw, rh, RoomType::Normal});
    }
    d.mazeChamberCount = static_cast<int>(d.rooms.size());

    // Stairs.
    const Room& startRoom = d.rooms.front();
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.stairsUp = {1, 1};

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.stairsDown = {d.width - 2, d.height - 2};

    // Sprinkle some closed doors in corridor chokepoints to make LOS + combat more interesting.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (d.inBounds(x, y)) inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    // Use strategic doors (segment-based) so the maze gets occasional LOS-breakers
    // without turning every intersection into a door cluster.
    placeStrategicCorridorDoors(d, rng, inRoom, 0.95f);
}



// Organic "warrens" floor: narrow burrows carved by biased random walkers,
// then widened with a handful of chambers so navigation has landmarks.
//
// Design goals:
// - Much less rectilinear than BSP/ruins floors.
// - Less grid-like than the perfect maze floors.
// - Lots of corridor decisions + dead ends (good for stash closets / secret doors).
void generateWarrens(Dungeon& d, RNG& rng, int depth) {
    // Needs some breathing room; fall back gracefully on tiny maps (unit tests, etc).
    if (d.width < 22 || d.height < 16) {
        d.hasWarrens = false;
        generateBspRooms(d, rng);
        return;
    }

    d.rooms.clear();
    d.hasWarrens = true;

    // Start chamber near the middle so the level has an obvious "anchor".
    const int cx = d.width / 2;
    const int cy = d.height / 2;

    int sw = rng.range(7, 11);
    int sh = rng.range(6, 9);
    sw = std::min(sw, d.width - 6);
    sh = std::min(sh, d.height - 6);
    sw = std::max(5, sw);
    sh = std::max(5, sh);

    int sx = clampi(cx - sw / 2, 1, d.width - sw - 1);
    int sy = clampi(cy - sh / 2, 1, d.height - sh - 1);

    carveRect(d, sx, sy, sw, sh, TileType::Floor);
    d.rooms.push_back({sx, sy, sw, sh, RoomType::Normal});

    const int area = std::max(1, d.width * d.height);

    // Target walkable coverage. Keep it moderately low so the burrows feel tight.
    float frac = 0.30f + 0.01f * static_cast<float>(std::clamp(depth - 1, 0, 10));
    frac = std::clamp(frac, 0.28f, 0.42f);
    int targetFloors = static_cast<int>(std::lround(frac * static_cast<float>(area)));
    targetFloors = std::clamp(targetFloors, area / 6, (area * 3) / 5);

    int floorCount = sw * sh;

    // Helpers.
    const Vec2i dirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    auto randDir = [&]() -> Vec2i {
        return dirs[static_cast<size_t>(rng.range(0, 3))];
    };

    auto pickDirNoReverse = [&](Vec2i cur) -> Vec2i {
        for (int tries = 0; tries < 12; ++tries) {
            Vec2i nd = dirs[static_cast<size_t>(rng.range(0, 3))];
            if (nd.x == -cur.x && nd.y == -cur.y) continue;
            return nd;
        }
        return randDir();
    };

    auto carve = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        // Keep a 1-tile wall ring (the border looks better and prevents edge leaks).
        if (x <= 0 || y <= 0 || x >= d.width - 1 || y >= d.height - 1) return;

        Tile& t = d.at(x, y);
        if (t.type == TileType::Wall) {
            t.type = TileType::Floor;
            floorCount++;
        }
    };

    struct Digger {
        Vec2i p;
        Vec2i dir;
        int life = 0;
    };

    const int maxDiggers = std::clamp(4 + depth / 2, 4, 9);
    std::vector<Digger> diggers;
    diggers.reserve(static_cast<size_t>(maxDiggers));

    // Seed a couple of diggers in the start chamber.
    Vec2i start{sx + sw / 2, sy + sh / 2};
    start.x = clampi(start.x, 2, d.width - 3);
    start.y = clampi(start.y, 2, d.height - 3);

    const int baseLife = std::clamp(32 + depth * 6, 32, 110);
    diggers.push_back({start, randDir(), baseLife + rng.range(-10, 15)});
    diggers.push_back({start, randDir(), baseLife + rng.range(-10, 15)});

    const float turnChance   = 0.22f;
    const float branchChance = 0.045f;
    const float widenChance  = 0.10f;
    const float nodeChance   = 0.030f;

    // Upper bound so pathological seeds can't loop forever.
    const int maxSteps = std::max(2000, area * 14);

    int steps = 0;
    while (floorCount < targetFloors && steps < maxSteps) {
        if (diggers.empty()) {
            // Respawn from existing tunnel space so we never create disconnected pockets.
            Vec2i rp = d.randomFloor(rng, true);
            diggers.push_back({rp, randDir(), baseLife});
        }

        for (size_t i = 0; i < diggers.size() && floorCount < targetFloors; /*manual*/) {
            Digger& g = diggers[i];

            // Carve the tunnel tile.
            carve(g.p.x, g.p.y);

            // Occasional widening for pockets / 2-wide hall segments.
            if (rng.chance(widenChance)) {
                if (g.dir.x != 0) {
                    int side = rng.chance(0.5f) ? 1 : -1;
                    carve(g.p.x, g.p.y + side);
                } else {
                    int side = rng.chance(0.5f) ? 1 : -1;
                    carve(g.p.x + side, g.p.y);
                }
            }

            // Rare "junction node": a small 3x3 pocket that feels like a dug-out hub.
            if (rng.chance(nodeChance)) {
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        carve(g.p.x + ox, g.p.y + oy);
                    }
                }
            }

            // Branching: spawn a new digger that heads off in a new direction.
            if (static_cast<int>(diggers.size()) < maxDiggers && rng.chance(branchChance)) {
                Digger nb;
                nb.p = g.p;
                nb.dir = pickDirNoReverse(g.dir);
                nb.life = baseLife + rng.range(-18, 18);
                diggers.push_back(nb);
            }

            // Turn sometimes (keeps tunnels from being too straight).
            if (rng.chance(turnChance)) {
                g.dir = pickDirNoReverse(g.dir);
            }

            // Step. If we hit the border, bounce by picking a new direction.
            Vec2i np{g.p.x + g.dir.x, g.p.y + g.dir.y};
            if (np.x <= 1 || np.y <= 1 || np.x >= d.width - 2 || np.y >= d.height - 2) {
                g.dir = pickDirNoReverse(g.dir);
            } else {
                g.p = np;
            }

            g.life--;
            if (g.life <= 0) {
                // Remove digger (swap-pop).
                diggers[i] = diggers.back();
                diggers.pop_back();
                continue;
            }

            ++i;
        }

        steps++;
    }

    // If something went badly wrong (very small/odd maps), fall back.
    if (floorCount < area / 8) {
        d.hasWarrens = false;
        fillWalls(d);
        generateBspRooms(d, rng);
        return;
    }

    // Keep only the largest connected passable region (guards against rare disconnected pockets).
    int compCount = 0;
    auto comp = computePassableComponents(d, compCount);

    if (compCount > 1) {
        std::vector<int> sizes(static_cast<size_t>(compCount), 0);
        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                if (!d.isPassable(x, y)) continue;
                const int c = comp[static_cast<size_t>(y * d.width + x)];
                if (c >= 0 && c < compCount) sizes[static_cast<size_t>(c)]++;
            }
        }

        int best = 0;
        for (int i = 1; i < compCount; ++i) {
            if (sizes[static_cast<size_t>(i)] > sizes[static_cast<size_t>(best)]) best = i;
        }

        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                if (!d.isPassable(x, y)) continue;
                const int c = comp[static_cast<size_t>(y * d.width + x)];
                if (c != best) d.at(x, y).type = TileType::Wall;
            }
        }
    }

    // Carve additional chambers as landmarks (always connected because we start from a floor tile).
    const int extraChambers = std::clamp(4 + depth / 2, 4, 10);
    for (int i = 0; i < extraChambers; ++i) {
        Vec2i p = d.randomFloor(rng, true);

        int rw = rng.range(4, 10);
        int rh = rng.range(4, 8);
        rw = std::min(rw, d.width - 6);
        rh = std::min(rh, d.height - 6);

        const int rx = clampi(p.x - rw / 2, 1, d.width - rw - 1);
        const int ry = clampi(p.y - rh / 2, 1, d.height - rh - 1);

        carveRect(d, rx, ry, rw, rh, TileType::Floor);
        d.rooms.push_back({rx, ry, rw, rh, RoomType::Normal});

        // Light furniture so chambers aren't empty boxes.
        if (rw >= 6 && rh >= 6 && rng.chance(0.22f)) {
            const int fx = clampi(p.x + rng.range(-1, 1), rx + 2, rx + rw - 3);
            const int fy = clampi(p.y + rng.range(-1, 1), ry + 2, ry + rh - 3);
            if (d.inBounds(fx, fy) && d.at(fx, fy).type == TileType::Floor) {
                d.at(fx, fy).type = rng.chance(0.55f) ? TileType::Pillar : TileType::Boulder;
            }
        }
    }

    // Stairs: start at the first (central) chamber, then pick the farthest reachable tile.
    const Room& startRoom = d.rooms.front();
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        carveFloor(d, d.stairsUp.x, d.stairsUp.y);
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }

    // Sparse corridor doors: warrens should feel claustrophobic, but still benefit from LOS breaks.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (d.inBounds(x, y)) inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    placeStrategicCorridorDoors(d, rng, inRoom, 0.58f);
}




void generateCatacombs(Dungeon& d, RNG& rng, int depth) {
    (void)depth;

    // A catacombs floor is a dense grid of small "crypt" rooms connected by a maze
    // carved through the solid stone between them.
    //
    // Goals:
    // - Lots of doors (tactical LOS breaks / ambush points)
    // - Short sight-lines and frequent junctions (more "room-to-room" play)
    // - Guaranteed global connectivity via a cell-maze spanning tree + extra loops

    // Needs some breathing room; fall back gracefully on tiny maps (unit tests, etc).
    if (d.width < 22 || d.height < 16) {
        generateBspRooms(d, rng);
        return;
    }

    d.rooms.clear();

    // Coarse grid size. Keep this fairly large so each cell can host a real room
    // with wall thickness around it for corridors.
    const int cellSize = 9;

    int cols = (d.width - 2) / cellSize;
    int rows = (d.height - 2) / cellSize;
    if (cols < 2 || rows < 2) {
        generateBspRooms(d, rng);
        return;
    }

    struct Cell {
        int roomIdx = -1;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // [x0,x1), [y0,y1)
    };

    std::vector<Cell> cells(static_cast<size_t>(cols * rows));

    auto cidx = [&](int cx, int cy) -> int { return cy * cols + cx; };
    auto clampi = [&](int v, int lo, int hi) -> int { return (v < lo) ? lo : ((v > hi) ? hi : v); };

    // 1) Carve one room per grid cell.
    for (int cy = 0; cy < rows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            Cell& c = cells[static_cast<size_t>(cidx(cx, cy))];
            c.x0 = 1 + cx * cellSize;
            c.y0 = 1 + cy * cellSize;
            c.x1 = (cx == cols - 1) ? (d.width - 1) : (c.x0 + cellSize);
            c.y1 = (cy == rows - 1) ? (d.height - 1) : (c.y0 + cellSize);

            const int cellW = c.x1 - c.x0;
            const int cellH = c.y1 - c.y0;

            if (cellW < 6 || cellH < 6) continue;

            // Keep at least a 1-tile wall margin inside the cell.
            int rwMin = std::max(4, cellW - 4);
            int rwMax = std::max(rwMin, cellW - 2);
            int rhMin = std::max(4, cellH - 4);
            int rhMax = std::max(rhMin, cellH - 2);

            int rw = rng.range(rwMin, rwMax);
            int rh = rng.range(rhMin, rhMax);
            rw = std::min(rw, cellW - 2);
            rh = std::min(rh, cellH - 2);
            if (rw < 4 || rh < 4) continue;

            const int rxMin = c.x0 + 1;
            const int ryMin = c.y0 + 1;
            int rxMax = c.x1 - rw - 1;
            int ryMax = c.y1 - rh - 1;
            if (rxMax < rxMin) rxMax = rxMin;
            if (ryMax < ryMin) ryMax = ryMin;

            const int rx = rng.range(rxMin, rxMax);
            const int ry = rng.range(ryMin, ryMax);

            carveRect(d, rx, ry, rw, rh, TileType::Floor);

            Room r;
            r.x = rx;
            r.y = ry;
            r.w = rw;
            r.h = rh;
            r.type = RoomType::Normal;

            c.roomIdx = static_cast<int>(d.rooms.size());
            d.rooms.push_back(r);
        }
    }

    if (d.rooms.size() < 4) {
        // Something went wrong (usually only possible on tiny odd sizes).
        fillWalls(d);
        generateBspRooms(d, rng);
        return;
    }

    // 2) Mark room footprint so corridor carving can avoid slicing through rooms.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    // Track which cell-to-cell walls have been opened so we can add loops later.
    std::vector<uint8_t> openE(static_cast<size_t>(cols * rows), uint8_t{0});
    std::vector<uint8_t> openS(static_cast<size_t>(cols * rows), uint8_t{0});

    auto markOpen = [&](int ax, int ay, int bx, int by) {
        if (bx == ax + 1 && by == ay) openE[static_cast<size_t>(cidx(ax, ay))] = 1;
        else if (bx == ax - 1 && by == ay) openE[static_cast<size_t>(cidx(bx, by))] = 1;
        else if (by == ay + 1 && bx == ax) openS[static_cast<size_t>(cidx(ax, ay))] = 1;
        else if (by == ay - 1 && bx == ax) openS[static_cast<size_t>(cidx(bx, by))] = 1;
    };

    auto isOpen = [&](int ax, int ay, int bx, int by) -> bool {
        if (bx == ax + 1 && by == ay) return openE[static_cast<size_t>(cidx(ax, ay))] != 0;
        if (bx == ax - 1 && by == ay) return openE[static_cast<size_t>(cidx(bx, by))] != 0;
        if (by == ay + 1 && bx == ax) return openS[static_cast<size_t>(cidx(ax, ay))] != 0;
        if (by == ay - 1 && bx == ax) return openS[static_cast<size_t>(cidx(bx, by))] != 0;
        return false;
    };

    auto pickDoorOnSide = [&](const Room& r, int side) -> Vec2i {
        // side: 0=N, 1=S, 2=W, 3=E
        for (int tries = 0; tries < 30; ++tries) {
            Vec2i door{ r.cx(), r.cy() };

            if (side == 0) { // N
                door.x = rng.range(r.x + 1, r.x2() - 2);
                door.y = r.y;
            } else if (side == 1) { // S
                door.x = rng.range(r.x + 1, r.x2() - 2);
                door.y = r.y2() - 1;
            } else if (side == 2) { // W
                door.x = r.x;
                door.y = rng.range(r.y + 1, r.y2() - 2);
            } else { // E
                door.x = r.x2() - 1;
                door.y = rng.range(r.y + 1, r.y2() - 2);
            }

            if (!d.inBounds(door.x, door.y)) continue;
            if (anyDoorInRadius(d, door.x, door.y, 1)) continue;
            return door;
        }

        // Fallback: center of the side.
        Vec2i door{ r.cx(), r.cy() };
        if (side == 0) door = { clampi(r.cx(), r.x + 1, r.x2() - 2), r.y };
        else if (side == 1) door = { clampi(r.cx(), r.x + 1, r.x2() - 2), r.y2() - 1 };
        else if (side == 2) door = { r.x, clampi(r.cy(), r.y + 1, r.y2() - 2) };
        else door = { r.x2() - 1, clampi(r.cy(), r.y + 1, r.y2() - 2) };
        return door;
    };

    auto outFromDoor = [&](Vec2i door, int side) -> Vec2i {
        if (side == 0) return { door.x, door.y - 1 };
        if (side == 1) return { door.x, door.y + 1 };
        if (side == 2) return { door.x - 1, door.y };
        return { door.x + 1, door.y };
    };

    auto placeDoorTile = [&](Vec2i p) {
        if (!d.inBounds(p.x, p.y)) return;
        TileType& tt = d.at(p.x, p.y).type;
        if (!(tt == TileType::Floor || tt == TileType::DoorClosed || tt == TileType::DoorOpen)) return;

        // Catacombs have a lot of doors, but keep traversal from feeling too "spammy"
        // by leaving some already-open.
        tt = rng.chance(0.22f) ? TileType::DoorOpen : TileType::DoorClosed;
    };

    auto connectCells = [&](int ax, int ay, int bx, int by) {
        const int ia = cells[static_cast<size_t>(cidx(ax, ay))].roomIdx;
        const int ib = cells[static_cast<size_t>(cidx(bx, by))].roomIdx;
        if (ia < 0 || ib < 0) return;

        const Room& ra = d.rooms[static_cast<size_t>(ia)];
        const Room& rb = d.rooms[static_cast<size_t>(ib)];

        // Determine connection orientation.
        int sideA = 0;
        int sideB = 0;
        if (bx == ax + 1 && by == ay) { sideA = 3; sideB = 2; }        // A -> E, B -> W
        else if (bx == ax - 1 && by == ay) { sideA = 2; sideB = 3; }   // A -> W, B -> E
        else if (by == ay + 1 && bx == ax) { sideA = 1; sideB = 0; }   // A -> S, B -> N
        else if (by == ay - 1 && bx == ax) { sideA = 0; sideB = 1; }   // A -> N, B -> S
        else return;

        Vec2i doorA = pickDoorOnSide(ra, sideA);
        Vec2i doorB = pickDoorOnSide(rb, sideB);

        Vec2i outA = outFromDoor(doorA, sideA);
        Vec2i outB = outFromDoor(doorB, sideB);

        if (!d.inBounds(outA.x, outA.y) || !d.inBounds(outB.x, outB.y)) return;

        // Carve a corridor that avoids cutting through room interiors.
        // If A* fails (rare), fall back to a simple L-shaped tunnel.
        if (!carveCorridorAStar(d, rng, outA, outB, inRoom)) {
            if (rng.chance(0.5f)) {
                carveH(d, outA.x, outB.x, outA.y);
                carveV(d, outA.y, outB.y, outB.x);
            } else {
                carveV(d, outA.y, outB.y, outA.x);
                carveH(d, outA.x, outB.x, outB.y);
            }
        }

        // Now place the two doors.
        placeDoorTile(doorA);
        placeDoorTile(doorB);
    };

    // 3) Build a maze over the cell grid (recursive backtracker) so all rooms are reachable.
    std::vector<uint8_t> visited(static_cast<size_t>(cols * rows), uint8_t{0});
    std::vector<Vec2i> stack;
    stack.reserve(static_cast<size_t>(cols * rows));

    Vec2i start{ cols / 2, rows / 2 };
    visited[static_cast<size_t>(cidx(start.x, start.y))] = 1;
    stack.push_back(start);

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    while (!stack.empty()) {
        Vec2i cur = stack.back();

        // Collect unvisited neighbors.
        std::vector<Vec2i> nbs;
        nbs.reserve(4);
        for (const Vec2i& dv : dirs) {
            const int nx = cur.x + dv.x;
            const int ny = cur.y + dv.y;
            if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) continue;
            const int ii = cidx(nx, ny);
            if (visited[static_cast<size_t>(ii)] != 0) continue;
            nbs.push_back({nx, ny});
        }

        if (nbs.empty()) {
            stack.pop_back();
            continue;
        }

        Vec2i nxt = nbs[static_cast<size_t>(rng.range(0, static_cast<int>(nbs.size()) - 1))];
        connectCells(cur.x, cur.y, nxt.x, nxt.y);
        markOpen(cur.x, cur.y, nxt.x, nxt.y);

        visited[static_cast<size_t>(cidx(nxt.x, nxt.y))] = 1;
        stack.push_back(nxt);
    }

    // 4) Add extra random connections to create loops (avoid a pure tree).
    const float loopChance = (depth >= 6) ? 0.22f : 0.16f;
    for (int cy = 0; cy < rows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            if (cx + 1 < cols && !isOpen(cx, cy, cx + 1, cy) && rng.chance(loopChance)) {
                connectCells(cx, cy, cx + 1, cy);
                markOpen(cx, cy, cx + 1, cy);
            }
            if (cy + 1 < rows && !isOpen(cx, cy, cx, cy + 1) && rng.chance(loopChance)) {
                connectCells(cx, cy, cx, cy + 1);
                markOpen(cx, cy, cx, cy + 1);
            }
        }
    }

    // 5) Light corridor roughening (slightly wider halls / niches), but never through rooms.
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };
    auto isInRoom = [&](int x, int y) -> bool { return inRoom[idx(x, y)] != 0; };

    const float roughChance = 0.030f + 0.004f * static_cast<float>(std::min(6, std::max(0, depth - 1)));
    for (int y = 2; y < d.height - 2; ++y) {
        for (int x = 2; x < d.width - 2; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (isInRoom(x, y)) continue;
            if (!rng.chance(roughChance)) continue;

            const Vec2i dv = dirs[rng.range(0, 3)];
            const int nx = x + dv.x;
            const int ny = y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (nx <= 0 || ny <= 0 || nx >= d.width - 1 || ny >= d.height - 1) continue;
            if (isInRoom(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Wall) carveFloor(d, nx, ny);
        }
    }

    // 6) Add a bit of "tomb furniture" inside rooms: pillars as sarcophagi/tombstones.
    for (const Room& r : d.rooms) {
        if (r.w < 5 || r.h < 5) continue;
        if (!rng.chance(0.22f)) continue;

        const int cx = r.cx();
        const int cy = r.cy();
        if (!d.inBounds(cx, cy)) continue;
        if (d.at(cx, cy).type != TileType::Floor) continue;
        d.at(cx, cy).type = TileType::Pillar;

        // Occasionally add a second pillar offset from center (for larger rooms).
        if (r.w >= 7 && r.h >= 7 && rng.chance(0.28f)) {
            const int px = clampi(cx + rng.range(-1, 1), r.x + 2, r.x2() - 3);
            const int py = clampi(cy + rng.range(-1, 1), r.y + 2, r.y2() - 3);
            if (d.inBounds(px, py) && d.at(px, py).type == TileType::Floor) {
                d.at(px, py).type = TileType::Pillar;
            }
        }
    }

    // 7) Place stairs: start near the middle, then choose the farthest reachable tile.
    const int scx = cols / 2;
    const int scy = rows / 2;
    const int startIdx = cells[static_cast<size_t>(cidx(scx, scy))].roomIdx;
    const Room& startRoom = d.rooms[static_cast<size_t>(std::max(0, startIdx))];

    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        carveFloor(d, d.stairsUp.x, d.stairsUp.y);
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }
}


// A bespoke late-game floor: a maze-like labyrinth with a central treasure lair.
// This is meant to be a spike in navigation + trap/door play right before the final floor.
[[maybe_unused]] void generateLabyrinth(Dungeon& d, RNG& rng, int depth) {
    (void)depth;

    fillWalls(d);

    // Perfect maze (recursive backtracker) carved on odd coordinates.
    const int cellW = (d.width - 1) / 2;
    const int cellH = (d.height - 1) / 2;
    if (cellW <= 1 || cellH <= 1) {
        generateBspRooms(d, rng);
        return;
    }

    auto cellToPos = [&](int cx, int cy) -> Vec2i {
        return { 1 + cx * 2, 1 + cy * 2 };
    };
    auto cidx = [&](int cx, int cy) { return static_cast<size_t>(cy * cellW + cx); };

    std::vector<uint8_t> vis(static_cast<size_t>(cellW * cellH), uint8_t{0});
    std::vector<Vec2i> stack;
    stack.reserve(static_cast<size_t>(cellW * cellH));

    // Start carving from a slightly random central-ish cell so runs differ, while keeping
    // the "lair" region likely to be reachable from the carved graph.
    const int startCx = clampi(cellW / 2 + rng.range(-2, 2), 0, cellW - 1);
    const int startCy = clampi(cellH / 2 + rng.range(-2, 2), 0, cellH - 1);
    stack.push_back({startCx, startCy});
    vis[cidx(startCx, startCy)] = 1;
    Vec2i sp = cellToPos(startCx, startCy);
    d.at(sp.x, sp.y).type = TileType::Floor;

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!stack.empty()) {
        Vec2i cur = stack.back();

        std::vector<Vec2i> neigh;
        neigh.reserve(4);
        for (auto& dv : dirs) {
            int nx = cur.x + dv[0];
            int ny = cur.y + dv[1];
            if (nx < 0 || ny < 0 || nx >= cellW || ny >= cellH) continue;
            if (vis[cidx(nx, ny)] != 0) continue;
            neigh.push_back({nx, ny});
        }

        if (neigh.empty()) {
            stack.pop_back();
            continue;
        }

        Vec2i nxt = neigh[static_cast<size_t>(rng.range(0, static_cast<int>(neigh.size()) - 1))];
        Vec2i a = cellToPos(cur.x, cur.y);
        Vec2i b = cellToPos(nxt.x, nxt.y);
        Vec2i mid{ (a.x + b.x) / 2, (a.y + b.y) / 2 };
        d.at(mid.x, mid.y).type = TileType::Floor;
        d.at(b.x, b.y).type = TileType::Floor;
        vis[cidx(nxt.x, nxt.y)] = 1;
        stack.push_back(nxt);
    }

    // Add lots of loops: the labyrinth should feel less like a tree and more like a twisting
    // "real" maze, especially under pressure.
    const int breaks = std::max(12, (cellW * cellH) / 3);
    for (int i = 0; i < breaks; ++i) {
        int x = rng.range(2, d.width - 3);
        int y = rng.range(2, d.height - 3);
        if (d.at(x, y).type != TileType::Wall) continue;

        bool horiz = (d.at(x - 1, y).type == TileType::Floor && d.at(x + 1, y).type == TileType::Floor);
        bool vert  = (d.at(x, y - 1).type == TileType::Floor && d.at(x, y + 1).type == TileType::Floor);
        if (!(horiz || vert)) continue;
        d.at(x, y).type = TileType::Floor;
    }

    // ---------------------------
    // Central lair with moat
    // ---------------------------
    // Dimensions: keep odd-ish and within bounds.
    int wallW = 15;
    int wallH = 11;
    wallW = std::min(wallW, d.width - 6);
    wallH = std::min(wallH, d.height - 6);
    wallW = std::max(11, wallW | 1);
    wallH = std::max(9,  wallH | 1);

    const int cx = d.width / 2;
    const int cy = d.height / 2;
    int wallX = clampi(cx - wallW / 2, 2, d.width - wallW - 3);
    int wallY = clampi(cy - wallH / 2, 2, d.height - wallH - 3);

    // Hard-wall the ring (overwrites parts of the maze), then carve the interior.
    carveRect(d, wallX, wallY, wallW, wallH, TileType::Wall);
    carveRect(d, wallX + 1, wallY + 1, wallW - 2, wallH - 2, TileType::Floor);

    // A few pillars inside for tactical cover.
    for (int y = wallY + 2; y < wallY + wallH - 2; y += 3) {
        for (int x = wallX + 2; x < wallX + wallW - 2; x += 4) {
            if (!rng.chance(0.35f)) continue;
            if (d.inBounds(x, y)) d.at(x, y).type = TileType::Pillar;
        }
    }

    // Entrances: locked doors on all 4 sides.
    const int doorN_x = wallX + wallW / 2;
    const int doorN_y = wallY;
    const int doorS_x = wallX + wallW / 2;
    const int doorS_y = wallY + wallH - 1;
    const int doorW_x = wallX;
    const int doorW_y = wallY + wallH / 2;
    const int doorE_x = wallX + wallW - 1;
    const int doorE_y = wallY + wallH / 2;

    d.at(doorN_x, doorN_y).type = TileType::DoorLocked;
    d.at(doorS_x, doorS_y).type = TileType::DoorLocked;
    d.at(doorW_x, doorW_y).type = TileType::DoorLocked;
    d.at(doorE_x, doorE_y).type = TileType::DoorLocked;

    // Moat ring (chasm) one tile around the lair walls. This doesn't block LOS but does block
    // movement, forcing you to approach via bridges.
    const int moatX = wallX - 1;
    const int moatY = wallY - 1;
    const int moatW = wallW + 2;
    const int moatH = wallH + 2;
    for (int y = moatY; y < moatY + moatH; ++y) {
        for (int x = moatX; x < moatX + moatW; ++x) {
            if (!d.inBounds(x, y)) continue;
            const bool border = (x == moatX || x == moatX + moatW - 1 || y == moatY || y == moatY + moatH - 1);
            if (!border) continue;
            // Don't overwrite the lair walls or doors.
            if (x >= wallX && x < wallX + wallW && y >= wallY && y < wallY + wallH) continue;
            d.at(x, y).type = TileType::Chasm;
        }
    }

    auto setBridge = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        d.at(x, y).type = TileType::Floor;
    };

    // Bridges aligned with each door.
    setBridge(doorN_x, doorN_y - 1);
    setBridge(doorS_x, doorS_y + 1);
    setBridge(doorW_x - 1, doorW_y);
    setBridge(doorE_x + 1, doorE_y);

    auto tunnelOut = [&](Vec2i start, Vec2i dir) {
        Vec2i p = start;
        for (int i = 0; i < 24; ++i) {
            p.x += dir.x;
            p.y += dir.y;
            if (!d.inBounds(p.x, p.y)) break;
            if (d.at(p.x, p.y).type == TileType::Floor) break;
            // Don't tunnel through the lair walls.
            if (p.x >= wallX && p.x < wallX + wallW && p.y >= wallY && p.y < wallY + wallH) break;
            d.at(p.x, p.y).type = TileType::Floor;
        }
    };

    tunnelOut({doorN_x, doorN_y - 1}, {0, -1});
    tunnelOut({doorS_x, doorS_y + 1}, {0, 1});
    tunnelOut({doorW_x - 1, doorW_y}, {-1, 0});
    tunnelOut({doorE_x + 1, doorE_y}, {1, 0});

    // ---------------------------
    // Start / exit rooms + shrine
    // ---------------------------
    auto inMoatBounds = [&](int x, int y) {
        return x >= moatX && x < moatX + moatW && y >= moatY && y < moatY + moatH;
    };

    // Start chamber near the upper-left to encourage traversal.
    Vec2i prefer{2, 2};
    Vec2i best = { d.width / 2, d.height / 2 };
    constexpr int kInfDist = 1000000000;
    int bestDist = kInfDist;
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (inMoatBounds(x, y)) continue;
            int md = std::abs(x - prefer.x) + std::abs(y - prefer.y);
            if (md < bestDist) {
                bestDist = md;
                best = {x, y};
            }
        }
    }
    if (bestDist >= kInfDist) {
        best = d.randomFloor(rng, true);
    }

    const int sw = rng.range(6, 8);
    const int sh = rng.range(5, 7);
    int sx = clampi(best.x - sw / 2, 1, d.width - sw - 1);
    int sy = clampi(best.y - sh / 2, 1, d.height - sh - 1);
    carveRect(d, sx, sy, sw, sh, TileType::Floor);
    d.stairsUp = { best.x, best.y };
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.stairsUp = {1, 1};

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.stairsDown = {d.width - 2, d.height - 2};

    // Exit chamber around stairsDown.
    const int ew = rng.range(6, 9);
    const int eh = rng.range(5, 8);
    int ex = clampi(d.stairsDown.x - ew / 2, 1, d.width - ew - 1);
    int ey = clampi(d.stairsDown.y - eh / 2, 1, d.height - eh - 1);
    carveRect(d, ex, ey, ew, eh, TileType::Floor);

    // Shrine chamber somewhere mid-far from the start.
    Room shrine;
    bool haveShrine = false;
    for (int tries = 0; tries < 120; ++tries) {
        Vec2i p = d.randomFloor(rng, true);
        if (inMoatBounds(p.x, p.y)) continue;
        const int di = dist.empty() ? 0 : dist[static_cast<size_t>(p.y * d.width + p.x)];
        if (di < 10) continue;
        const int rw = rng.range(5, 8);
        const int rh = rng.range(5, 7);
        int rx = clampi(p.x - rw / 2, 1, d.width - rw - 1);
        int ry = clampi(p.y - rh / 2, 1, d.height - rh - 1);
        // Avoid overlapping the lair/moat.
        if (rx < moatX + moatW && rx + rw > moatX && ry < moatY + moatH && ry + rh > moatY) continue;
        carveRect(d, rx, ry, rw, rh, TileType::Floor);
        shrine = {rx, ry, rw, rh, RoomType::Shrine};
        haveShrine = true;
        break;
    }

    // Build room list.
    d.rooms.clear();
    d.rooms.push_back({sx, sy, sw, sh, RoomType::Normal});
    d.rooms.push_back({ex, ey, ew, eh, RoomType::Normal});
    if (haveShrine) d.rooms.push_back(shrine);

    // Lair interior as treasure room.
    d.rooms.push_back({wallX + 1, wallY + 1, wallW - 2, wallH - 2, RoomType::Treasure});

    // Sprinkle some doors in corridor chokepoints.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), uint8_t{0});
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (d.inBounds(x, y)) inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    // Place doors strategically (segment-based) while respecting the moat region.
    placeStrategicCorridorDoors(d, rng, inRoom, 1.15f, [&](int x, int y) {
        return inMoatBounds(x, y);
    });
}


// -----------------------------------------------------------------------------
// Special floors (hand-authored / alternate generation styles)
// -----------------------------------------------------------------------------
//
//  - Rogue homage: a classic 3x3 room grid connected by open corridors (no doors).
//  - Sokoban: a boulder-into-chasm bridging puzzle floor.
//

[[maybe_unused]] void generateRogueLevel(Dungeon& d, RNG& rng, int depth) {
    // Rogue homage floor: a classic 3x3 grid of rooms connected by open corridors.
    //
    // Design goals:
    //  - Doorless layout to create a distinctly different combat texture vs. BSP floors.
    //  - Strong connectivity (no "oops" unreachable staircases).
    //  - Still uses room typing (treasure/shop/shrine/etc.) for pacing, but without
    //    adding secret/vault doors.

    fillWalls(d);

    d.rooms.clear();
    d.rooms.reserve(9);

    constexpr int cols = 3;
    constexpr int rows = 3;

    const int x0 = 1;
    const int y0 = 1;
    const int innerW = std::max(1, d.width - 2);
    const int innerH = std::max(1, d.height - 2);

    const int cellW = std::max(3, innerW / cols);
    const int cellH = std::max(3, innerH / rows);

    int roomIndex[rows][cols];
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) roomIndex[r][c] = -1;
    }

    auto cellX1 = [&](int c) { return x0 + c * cellW; };
    auto cellY1 = [&](int r) { return y0 + r * cellH; };
    auto cellX2 = [&](int c) { return (c == cols - 1) ? (x0 + innerW) : (x0 + (c + 1) * cellW); };
    auto cellY2 = [&](int r) { return (r == rows - 1) ? (y0 + innerH) : (y0 + (r + 1) * cellH); };

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int cx1 = cellX1(c);
            const int cy1 = cellY1(r);
            const int cx2 = cellX2(c);
            const int cy2 = cellY2(r);

            const int cw = std::max(3, cx2 - cx1);
            const int ch = std::max(3, cy2 - cy1);

            // Keep a 1-tile buffer inside each cell so rooms are visually distinct.
            const int maxW = std::max(2, cw - 2);
            const int maxH = std::max(2, ch - 2);

            const int minW = std::max(2, (maxW * 2) / 3);
            const int minH = std::max(2, (maxH * 2) / 3);

            const int rw = rng.range(minW, maxW);
            const int rh = rng.range(minH, maxH);

            const int rx = cx1 + 1 + rng.range(0, std::max(0, maxW - rw));
            const int ry = cy1 + 1 + rng.range(0, std::max(0, maxH - rh));

            carveRect(d, rx, ry, rw, rh, TileType::Floor);

            Room room;
            room.x = rx;
            room.y = ry;
            room.w = rw;
            room.h = rh;
            room.type = RoomType::Normal;
            d.rooms.push_back(room);

            roomIndex[r][c] = static_cast<int>(d.rooms.size()) - 1;
        }
    }

    auto sgn = [](int v) { return (v > 0) - (v < 0); };

    auto carveL = [&](Vec2i a, Vec2i b) {
        int x = a.x;
        int y = a.y;
        if (d.inBounds(x, y)) carveFloor(d, x, y);

        const bool horizFirst = rng.chance(0.5f);
        if (horizFirst) {
            while (x != b.x) {
                x += sgn(b.x - x);
                if (!d.inBounds(x, y)) break;
                carveFloor(d, x, y);
            }
            while (y != b.y) {
                y += sgn(b.y - y);
                if (!d.inBounds(x, y)) break;
                carveFloor(d, x, y);
            }
        } else {
            while (y != b.y) {
                y += sgn(b.y - y);
                if (!d.inBounds(x, y)) break;
                carveFloor(d, x, y);
            }
            while (x != b.x) {
                x += sgn(b.x - x);
                if (!d.inBounds(x, y)) break;
                carveFloor(d, x, y);
            }
        }
    };

    auto roomCenter = [&](int ri) -> Vec2i {
        const Room& r = d.rooms[static_cast<size_t>(ri)];
        return {r.cx(), r.cy()};
    };

    // Connect rooms in a grid. This produces multiple loops (which is very Rogue-ish)
    // while guaranteeing connectivity.
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols - 1; ++c) {
            const int a = roomIndex[r][c];
            const int b = roomIndex[r][c + 1];
            if (a >= 0 && b >= 0) carveL(roomCenter(a), roomCenter(b));
        }
    }
    for (int c = 0; c < cols; ++c) {
        for (int r = 0; r < rows - 1; ++r) {
            const int a = roomIndex[r][c];
            const int b = roomIndex[r + 1][c];
            if (a >= 0 && b >= 0) carveL(roomCenter(a), roomCenter(b));
        }
    }

    // Place stairs: start in a random room, then put the down stairs in the farthest room
    // (by BFS distance). This mirrors the BSP "farthest room" logic.
    int startIdx = 0;
    if (!d.rooms.empty()) {
        startIdx = rng.range(0, static_cast<int>(d.rooms.size()) - 1);
    }

    const Room& startRoom = d.rooms[static_cast<size_t>(startIdx)];
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        carveFloor(d, d.stairsUp.x, d.stairsUp.y);
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    const auto dist = bfsDistanceMap(d, d.stairsUp);
    int bestRoomIdx = startIdx;
    int bestDist = -1;
    for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
        const Room& rr = d.rooms[static_cast<size_t>(i)];
        const int cx = rr.cx();
        const int cy = rr.cy();
        if (!d.inBounds(cx, cy)) continue;
        const int di = dist[static_cast<size_t>(cy * d.width + cx)];
        if (di > bestDist) {
            bestDist = di;
            bestRoomIdx = i;
        }
    }

    const Room& endRoom = d.rooms[static_cast<size_t>(bestRoomIdx)];
    d.stairsDown = { endRoom.cx(), endRoom.cy() };
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }

    // Apply room typing (treasure/shop/lair/shrine/themed rooms) so spawn logic can
    // still bias content, but we intentionally avoid adding secret/vault doors here.
    markSpecialRooms(d, rng, depth);
}



// Surface camp (depth 0): an above-ground hub with a palisade, tents, and a little
// surrounding wilderness. This acts as a "safe-ish" staging area above the dungeon
// entrance.
//
// Design goals:
// - Much more per-run variety than the old static rectangle.
// - Always readable: a clear walkable route from EXIT (<) to DUNGEON (>) without
//   requiring door interaction.
// - Still cheap: this is generated often (start + hub returns).
void generateSurfaceCamp(Dungeon& d, RNG& rng) {
    fillWalls(d);
    d.rooms.clear();
    d.bonusLootSpots.clear();
    d.bonusItemSpawns.clear();
    d.hasCavernLake = false;
    d.hasWarrens = false;
    d.secretShortcutCount = 0;
    d.lockedShortcutCount = 0;
    d.corridorHubCount = 0;
    d.corridorHallCount = 0;
    d.sinkholeCount = 0;
    d.riftCacheCount = 0;
    d.riftCacheBoulderCount = 0;
    d.riftCacheChasmCount = 0;
    d.vaultSuiteCount = 0;
    d.deadEndClosetCount = 0;
    d.campStashSpot = {-1, -1};
    d.campGateIn = {-1, -1};
    d.campGateOut = {-1, -1};
    d.campSideGateIn = {-1, -1};
    d.campSideGateOut = {-1, -1};
    d.campWellSpot = {-1, -1};
    d.campFireSpot = {-1, -1};
    d.campAltarSpot = {-1, -1};

    // Start with an open outdoor field (floor) with border walls.
    if (d.width >= 3 && d.height >= 3) {
        carveRect(d, 1, 1, d.width - 2, d.height - 2, TileType::Floor);
    }

    enum class Side : uint8_t { North = 0, South, West, East };

    auto oppositeSide = [&](Side s) {
        switch (s) {
            case Side::North: return Side::South;
            case Side::South: return Side::North;
            case Side::West:  return Side::East;
            case Side::East:  return Side::West;
        }
        return Side::South;
    };

    auto sideInward = [&](Side s) -> Vec2i {
        switch (s) {
            case Side::North: return {0, 1};
            case Side::South: return {0, -1};
            case Side::West:  return {1, 0};
            case Side::East:  return {-1, 0};
        }
        return {0, 0};
    };

    auto sideOutward = [&](Side s) -> Vec2i {
        Vec2i in = sideInward(s);
        return {-in.x, -in.y};
    };

    auto idx = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y * d.width + x);
    };

    // ------------------------------------------------------------
    // Exit placement (stairs up): pick a map edge so it feels like an exterior trail.
    // ------------------------------------------------------------
    Side exitSide = static_cast<Side>(rng.range(0, 3));
    Vec2i exit{-1, -1};

    auto pickEdgePos = [&](Side s) -> Vec2i {
        // Place on the *inner* edge of the border wall (y==1, y==h-2, etc).
        // Keep away from corners for readability.

        switch (s) {
            case Side::North: return { rng.range(2, d.width - 3), 1 };
            case Side::South: return { rng.range(2, d.width - 3), d.height - 2 };
            case Side::West:  return { 1, rng.range(2, d.height - 3) };
            case Side::East:  return { d.width - 2, rng.range(2, d.height - 3) };
        }
        return {2, 2};
    };

    // Defensive fallback for tiny maps.
    if (d.width <= 6 || d.height <= 6) {
        exitSide = Side::North;
        exit = {2, 1};
    } else {
        exit = pickEdgePos(exitSide);
    }

    d.stairsUp = exit;
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    // ------------------------------------------------------------
    // Palisade yard: sized and shifted away from the exit to create a convincing
    // wilderness approach, with the gate facing the exit.
    // ------------------------------------------------------------
    const int minOuter = 6; // minimum ring of wilderness around the palisade

    const int maxCampW = std::max(18, d.width - 2 * minOuter);
    const int maxCampH = std::max(14, d.height - 2 * minOuter);

    int campW = clampi(d.width / 2 + rng.range(-8, 8), 18, maxCampW);
    int campH = clampi(d.height / 2 + rng.range(-6, 6), 14, maxCampH);

    int campX = (d.width - campW) / 2;
    int campY = (d.height - campH) / 2;

    // Shift the camp away from the exit side so the approach space varies.
    const int roomX = std::max(0, (d.width - campW) / 2 - 2);
    const int roomY = std::max(0, (d.height - campH) / 2 - 2);
    int shiftX = 0;
    int shiftY = 0;

    auto randShift = [&](int mag) {
        if (mag <= 0) return 0;
        if (mag == 1) return 1;
        return rng.range(1, mag);
    };

    switch (exitSide) {
        case Side::North: shiftY += randShift(std::min(8, roomY)); break;
        case Side::South: shiftY -= randShift(std::min(8, roomY)); break;
        case Side::West:  shiftX += randShift(std::min(12, roomX)); break;
        case Side::East:  shiftX -= randShift(std::min(12, roomX)); break;
    }

    campX = clampi(campX + shiftX, 2, std::max(2, d.width - campW - 2));
    campY = clampi(campY + shiftY, 2, std::max(2, d.height - campH - 2));

    const int campX2 = campX + campW - 1;
    const int campY2 = campY + campH - 1;

    auto isInCampBounds = [&](Vec2i p) {
        return p.x >= campX && p.x <= campX2 && p.y >= campY && p.y <= campY2;
    };

    // Palisade walls.
    for (int x = campX; x <= campX2; ++x) {
        if (d.inBounds(x, campY)) d.at(x, campY).type = TileType::Wall;
        if (d.inBounds(x, campY2)) d.at(x, campY2).type = TileType::Wall;
    }
    for (int y = campY; y <= campY2; ++y) {
        if (d.inBounds(campX, y)) d.at(campX, y).type = TileType::Wall;
        if (d.inBounds(campX2, y)) d.at(campX2, y).type = TileType::Wall;
    }

    // Gate: always faces the exit (open, so hub traversal never requires interaction).
    const Side gateSide = exitSide;
    Vec2i gate{-1, -1};

    switch (gateSide) {
        case Side::North: gate = { clampi(exit.x, campX + 2, campX2 - 2), campY }; break;
        case Side::South: gate = { clampi(exit.x, campX + 2, campX2 - 2), campY2 }; break;
        case Side::West:  gate = { campX, clampi(exit.y, campY + 2, campY2 - 2) }; break;
        case Side::East:  gate = { campX2, clampi(exit.y, campY + 2, campY2 - 2) }; break;
    }

    const Vec2i gateInStep  = sideInward(gateSide);
    const Vec2i gateOutStep = sideOutward(gateSide);
    const Vec2i gateIn{gate.x + gateInStep.x, gate.y + gateInStep.y};
    const Vec2i gateOut{gate.x + gateOutStep.x, gate.y + gateOutStep.y};

    d.campGateIn = gateIn;
    d.campGateOut = gateOut;

    if (d.inBounds(gate.x, gate.y)) {
        d.at(gate.x, gate.y).type = TileType::DoorOpen;
        carveFloor(d, gateIn.x, gateIn.y);
        carveFloor(d, gateOut.x, gateOut.y);

        // A tiny threshold pad so the gate reads clearly.
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                carveFloor(d, gateIn.x + dx, gateIn.y + dy);
                carveFloor(d, gateOut.x + dx, gateOut.y + dy);
            }
        }
    }


    // Optional side sallyport: a second open gate on a perpendicular wall.
    // This gives the outside wilderness more purpose (trails/POIs) without
    // compromising the main EXIT->GATE->DUNGEON readability.
    bool haveSideGate = false;
    Side sideGateSide = gateSide;
    Vec2i sideGate{-1, -1};

    if (d.width >= 26 && d.height >= 20 && rng.chance(0.70f)) {
        std::array<Side, 2> cand;
        if (gateSide == Side::North || gateSide == Side::South) {
            cand = {Side::West, Side::East};
        } else {
            cand = {Side::North, Side::South};
        }
        sideGateSide = cand[static_cast<size_t>(rng.range(0, 1))];

        const int scx = campX + campW / 2;
        const int scy = campY + campH / 2;
        const int sjx = std::max(1, campW / 6);
        const int sjy = std::max(1, campH / 6);

        // Place near the camp midline with a little jitter so it doesn't look perfectly symmetric.
        switch (sideGateSide) {
            case Side::North: sideGate = { clampi(scx + rng.range(-sjx, sjx), campX + 3, campX2 - 3), campY }; break;
            case Side::South: sideGate = { clampi(scx + rng.range(-sjx, sjx), campX + 3, campX2 - 3), campY2 }; break;
            case Side::West:  sideGate = { campX, clampi(scy + rng.range(-sjy, sjy), campY + 3, campY2 - 3) }; break;
            case Side::East:  sideGate = { campX2, clampi(scy + rng.range(-sjy, sjy), campY + 3, campY2 - 3) }; break;
        }

        // Avoid duplicating the main gate if something degenerates.
        if (sideGate != gate) {
            const Vec2i sgInStep  = sideInward(sideGateSide);
            const Vec2i sgOutStep = sideOutward(sideGateSide);
            const Vec2i sgIn{sideGate.x + sgInStep.x, sideGate.y + sgInStep.y};
            const Vec2i sgOut{sideGate.x + sgOutStep.x, sideGate.y + sgOutStep.y};

            if (d.inBounds(sideGate.x, sideGate.y) && d.inBounds(sgIn.x, sgIn.y) && d.inBounds(sgOut.x, sgOut.y)) {
                d.at(sideGate.x, sideGate.y).type = TileType::DoorOpen;
                carveFloor(d, sgIn.x, sgIn.y);
                carveFloor(d, sgOut.x, sgOut.y);

                // Smaller pad than the main gate (a side path).
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        carveFloor(d, sgIn.x + dx, sgIn.y + dy);
                        carveFloor(d, sgOut.x + dx, sgOut.y + dy);
                    }
                }

                d.campSideGateIn = sgIn;
                d.campSideGateOut = sgOut;
                haveSideGate = true;
            }
        }
    }

    // Dungeon entrance (stairs down): bias toward the opposite side of the yard.
    const Side downSide = oppositeSide(gateSide);
    Vec2i down{-1, -1};

    auto interiorClampX = [&](int x) { return clampi(x, campX + 2, campX2 - 2); };
    auto interiorClampY = [&](int y) { return clampi(y, campY + 2, campY2 - 2); };

    const int cx = campX + campW / 2;
    const int cy = campY + campH / 2;

    const int jitterX = std::max(1, campW / 6);
    const int jitterY = std::max(1, campH / 6);

    switch (downSide) {
        case Side::North: down = { interiorClampX(cx + rng.range(-jitterX, jitterX)), campY + 2 }; break;
        case Side::South: down = { interiorClampX(cx + rng.range(-jitterX, jitterX)), campY2 - 2 }; break;
        case Side::West:  down = { campX + 2, interiorClampY(cy + rng.range(-jitterY, jitterY)) }; break;
        case Side::East:  down = { campX2 - 2, interiorClampY(cy + rng.range(-jitterY, jitterY)) }; break;
    }

    d.stairsDown = down;
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }

    // ------------------------------------------------------------
    // Protection mask: we reserve a clear "spine" so travel between stairs is
    // guaranteed to be walkable even before opening any doors.
    // ------------------------------------------------------------
    std::vector<uint8_t> protect(static_cast<size_t>(d.width * d.height), uint8_t{0});

    auto markProtect = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        protect[idx(x, y)] = 1;
    };

    auto markProtectRadius = [&](Vec2i p, int r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                markProtect(p.x + dx, p.y + dy);
            }
        }
    };

    auto isProtected = [&](int x, int y) {
        if (!d.inBounds(x, y)) return true;
        return protect[idx(x, y)] != 0;
    };

    // Stairs clarity.
    markProtectRadius(d.stairsUp, 3);
    markProtectRadius(d.stairsDown, 3);

    // Keep the palisade interior immediately around the gate clear.
    markProtectRadius(gateIn, 2);
    // Also keep the outer bridge tile open so later terrain dressing does not block entry.
    markProtectRadius(gateOut, 2);

    if (haveSideGate) {
        markProtectRadius(d.campSideGateIn, 2);
        markProtectRadius(d.campSideGateOut, 2);
    }

    // Build a simple "keepout" spine between the gate and the dungeon stairs.
    auto markLPath = [&](Vec2i a, Vec2i b, int r) {
        // Randomize whether we go horiz->vert or vert->horiz for subtle layout variety.
        const bool hv = rng.chance(0.5f);

        Vec2i mid1{b.x, a.y};
        Vec2i mid2{a.x, b.y};
        Vec2i m = hv ? mid1 : mid2;

        auto markSeg = [&](Vec2i s, Vec2i t) {
            Vec2i p = s;
            markProtectRadius(p, r);

            int guard = 0;
            while (p != t && guard++ < 5000) {
                if (p.x < t.x) p.x++;
                else if (p.x > t.x) p.x--;
                else if (p.y < t.y) p.y++;
                else if (p.y > t.y) p.y--;
                markProtectRadius(p, r);
            }
        };

        markSeg(a, m);
        markSeg(m, b);
    };

    // Inside-camp spine.
    markLPath(gateIn, d.stairsDown, 1);

    // Optional palisade ditch/trench: a thin chasm ring just outside the walls.
    // This strengthens the surface hub silhouette and makes the bridges read clearly.
    const bool wantDitch = (d.width >= 40 && d.height >= 28) ? rng.chance(0.75f) : rng.chance(0.55f);
    if (wantDitch) {
        const int x0 = campX - 1;
        const int y0 = campY - 1;
        const int x1 = campX2 + 1;
        const int y1 = campY2 + 1;

        std::array<Vec2i, 2> gapCenters = {{ gateOut, d.campSideGateOut }};
        const int gapCount = haveSideGate ? 2 : 1;

        auto nearGap = [&](Vec2i p) {
            for (int i = 0; i < gapCount; ++i) {
                if (chebyshev(p, gapCenters[static_cast<size_t>(i)]) <= 2) return true;
            }
            return false;
        };

        auto maybeChasm = [&](int x, int y, float p) {
            if (!d.inBounds(x, y)) return;
            Vec2i v{x, y};
            if (isInCampBounds(v)) return;
            if (isProtected(x, y)) return;
            if (nearGap(v)) return;
            if (d.at(x, y).type != TileType::Floor) return;
            if (!rng.chance(p)) return;
            d.at(x, y).type = TileType::Chasm;
        };

        // Border ring.
        for (int x = x0; x <= x1; ++x) {
            maybeChasm(x, y0, 0.92f);
            maybeChasm(x, y1, 0.92f);
        }
        for (int y = y0; y <= y1; ++y) {
            maybeChasm(x0, y, 0.92f);
            maybeChasm(x1, y, 0.92f);
        }

        // A few stray cuts extending outward so it looks less like a perfect rectangle.
        const int tendrils = rng.range(4, 8);
        for (int i = 0; i < tendrils; ++i) {
            Vec2i s{-1, -1};
            if (rng.chance(0.5f)) {
                const int x = rng.range(x0, x1);
                s = rng.chance(0.5f) ? Vec2i{x, y0} : Vec2i{x, y1};
            } else {
                const int y = rng.range(y0, y1);
                s = rng.chance(0.5f) ? Vec2i{x0, y} : Vec2i{x1, y};
            }
            if (!d.inBounds(s.x, s.y)) continue;
            if (nearGap(s)) continue;

            Vec2i dir{0, 0};
            if (s.x == x0) dir = {-1, 0};
            else if (s.x == x1) dir = {1, 0};
            else if (s.y == y0) dir = {0, -1};
            else if (s.y == y1) dir = {0, 1};

            Vec2i p = s;
            const int len = rng.range(2, 6);
            for (int k = 0; k < len; ++k) {
                p = {p.x + dir.x, p.y + dir.y};
                if (!d.inBounds(p.x, p.y)) break;
                if (isInCampBounds(p)) break;
                if (isProtected(p.x, p.y)) break;
                if (d.at(p.x, p.y).type != TileType::Floor) break;
                if (rng.chance(0.85f)) d.at(p.x, p.y).type = TileType::Chasm;
            }
        }
    }

    // Outside approach path: a light, biased meander so the approach isn't always a straight line.
    // This only matters for obstacle placement (we keep it clear); visually it's still just floor.
    auto markMeanderPath = [&](Vec2i a, Vec2i b, int r) {
        Vec2i p = a;
        markProtectRadius(p, r);

        int guard = 0;
        while (p != b && guard++ < 12000) {
            // Candidate steps.
            std::array<Vec2i, 4> cand = {{ {p.x + 1, p.y}, {p.x - 1, p.y}, {p.x, p.y + 1}, {p.x, p.y - 1} }};

            // Bias toward the target, but allow lateral wobble.
            Vec2i best = p;
            int bestScore = 1 << 30;

            for (const Vec2i& q : cand) {
                if (!d.inBounds(q.x, q.y)) continue;
                if (isInCampBounds(q)) continue; // don't wander through the palisade
                if (!d.isWalkable(q.x, q.y)) continue; // avoid chasms/walls so the path stays usable

                // Prefer tiles that reduce Manhattan distance, but add a tiny stable noise term.
                const int md = std::abs(q.x - b.x) + std::abs(q.y - b.y);
                const uint32_t h = hash32(static_cast<uint32_t>(q.x * 73856093u) ^ static_cast<uint32_t>(q.y * 19349663u) ^ rng.state);
                const int n = static_cast<int>(h & 3u);

                // 0..3 noise breaks ties and creates gentle curvature.
                const int score = md * 6 + n;
                if (score < bestScore) {
                    bestScore = score;
                    best = q;
                }
            }

            // Occasionally take a non-best step to keep it organic.
            if (rng.chance(0.18f)) {
                const Vec2i q = cand[rng.range(0, 3)];
                if (d.inBounds(q.x, q.y) && !isInCampBounds(q) && d.isWalkable(q.x, q.y)) best = q;
            }

            p = best;
            markProtectRadius(p, r);

            // Safety: if we get stuck (shouldn't happen in open field), break.
            if (guard > 2000 && manhattan(p, b) > manhattan(a, b) + 20) break;
        }
    };

    if (d.inBounds(gateOut.x, gateOut.y)) {
        markMeanderPath(d.stairsUp, gateOut, 2);
    }

    // ------------------------------------------------------------
    // Camp structures: a small set of tents/huts packed around the spine.
    // ------------------------------------------------------------
    struct RectI {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int x2() const { return x + w - 1; }
        int y2() const { return y + h - 1; }
        int cx() const { return x + w / 2; }
        int cy() const { return y + h / 2; }
    };

    auto rectInflate = [&](const RectI& r, int m) {
        RectI o = r;
        o.x -= m;
        o.y -= m;
        o.w += 2 * m;
        o.h += 2 * m;
        return o;
    };

    auto rectOverlaps = [&](const RectI& a, const RectI& b) {
        return !(a.x2() < b.x || b.x2() < a.x || a.y2() < b.y || b.y2() < a.y);
    };

    auto rectInCampInterior = [&](const RectI& r, int marginToPalisade) {
        return r.x >= campX + 1 + marginToPalisade && r.y >= campY + 1 + marginToPalisade &&
               r.x2() <= campX2 - 1 - marginToPalisade && r.y2() <= campY2 - 1 - marginToPalisade;
    };

    auto rectTouchesProtected = [&](const RectI& r) {
        for (int yy = r.y; yy <= r.y2(); ++yy) {
            for (int xx = r.x; xx <= r.x2(); ++xx) {
                if (!d.inBounds(xx, yy)) return true;
                if (isProtected(xx, yy)) return true;
                if (d.at(xx, yy).type != TileType::Floor) return true;
            }
        }
        return false;
    };

    auto chooseDoorSideToward = [&](const RectI& r, Vec2i toward) -> Side {
        const int dx = toward.x - r.cx();
        const int dy = toward.y - r.cy();
        if (std::abs(dx) > std::abs(dy)) {
            return (dx >= 0) ? Side::East : Side::West;
        }
        return (dy >= 0) ? Side::South : Side::North;
    };

    struct DoorInfo {
        Vec2i door{-1, -1};
        Vec2i outside{-1, -1};
    };

    auto stampBuilding = [&](const RectI& r, Side doorSide, TileType doorType) -> DoorInfo {
        carveRect(d, r.x, r.y, r.w, r.h, TileType::Floor);

        for (int x = r.x; x <= r.x2(); ++x) {
            if (d.inBounds(x, r.y)) d.at(x, r.y).type = TileType::Wall;
            if (d.inBounds(x, r.y2())) d.at(x, r.y2()).type = TileType::Wall;
        }
        for (int y = r.y; y <= r.y2(); ++y) {
            if (d.inBounds(r.x, y)) d.at(r.x, y).type = TileType::Wall;
            if (d.inBounds(r.x2(), y)) d.at(r.x2(), y).type = TileType::Wall;
        }

        Vec2i door{-1, -1};
        Vec2i doorOutside{-1, -1};
        switch (doorSide) {
            case Side::North:
                door = {r.x + r.w / 2, r.y};
                doorOutside = {door.x, door.y - 1};
                break;
            case Side::South:
                door = {r.x + r.w / 2, r.y2()};
                doorOutside = {door.x, door.y + 1};
                break;
            case Side::West:
                door = {r.x, r.y + r.h / 2};
                doorOutside = {door.x - 1, door.y};
                break;
            case Side::East:
                door = {r.x2(), r.y + r.h / 2};
                doorOutside = {door.x + 1, door.y};
                break;
        }

        DoorInfo out;
        out.door = door;
        out.outside = doorOutside;

        if (d.inBounds(door.x, door.y)) {
            d.at(door.x, door.y).type = doorType;
            carveFloor(d, doorOutside.x, doorOutside.y);
        }
        return out;
    };

    // Decide how many structures fit.
    const int yardArea = campW * campH;
    int wantBuildings = 2;
    if (yardArea > 680) wantBuildings = 3;
    if (yardArea > 900) wantBuildings = 4;

    // Corner watchtowers: small defensive sheds near palisade corners.
    // These are fixed-placed structures that give the hub a stronger silhouette
    // and more "lived-in" geometry, without relying on the building packing RNG.
    std::vector<Vec2i> fixedEntrances;
    fixedEntrances.reserve(4);

    auto markProtectRect = [&](const RectI& r) {
        for (int yy = r.y; yy <= r.y2(); ++yy) {
            for (int xx = r.x; xx <= r.x2(); ++xx) {
                markProtect(xx, yy);
            }
        }
    };

    auto tryPlaceTowerAt = [&](int x, int y, int tw, int th) {
        RectI tr;
        tr.x = x;
        tr.y = y;
        tr.w = tw;
        tr.h = th;

        if (!rectInCampInterior(tr, /*marginToPalisade=*/0)) return;
        if (rectTouchesProtected(tr)) return;

        // Avoid crowding the main spine endpoints.
        if (chebyshev({tr.cx(), tr.cy()}, gateIn) <= 4) return;
        if (chebyshev({tr.cx(), tr.cy()}, d.stairsDown) <= 3) return;

        Side ds = chooseDoorSideToward(tr, {cx, cy});
        const DoorInfo di = stampBuilding(tr, ds, TileType::DoorOpen);

        // Protect the whole tower (and its immediate surroundings) from later clutter.
        markProtectRect(rectInflate(tr, 1));

        if (d.inBounds(di.outside.x, di.outside.y)) {
            fixedEntrances.push_back(di.outside);
            markProtectRadius(di.outside, 1);
        }
    };

    if (yardArea >= 850 && rng.chance(0.75f)) {
        const int tw = clampi(6 + rng.range(0, 2), 6, 8);
        const int th = clampi(6 + rng.range(0, 2), 6, 8);

        std::array<Vec2i, 4> corners = {{
            {campX + 1, campY + 1},
            {campX2 - tw, campY + 1},
            {campX + 1, campY2 - th},
            {campX2 - tw, campY2 - th}
        }};

        // Shuffle order with a few swaps (stable enough, no heavy utilities required).
        for (int s = 0; s < 8; ++s) {
            int a = rng.range(0, 3);
            int b = rng.range(0, 3);
            Vec2i tmp = corners[a];
            corners[a] = corners[b];
            corners[b] = tmp;
        }

        int placedTowers = 0;
        for (int i = 0; i < 4 && placedTowers < 2; ++i) {
            const size_t before = fixedEntrances.size();
            tryPlaceTowerAt(corners[i].x, corners[i].y, tw, th);
            if (fixedEntrances.size() > before) placedTowers++;
        }
    }


    // Always include the stash hut.
    struct BuildingSpec {
        int w0, w1;
        int h0, h1;
        bool isStash = false;
        bool doorOpen = false;
        const char* name = "";
    };

    std::vector<BuildingSpec> specs;
    specs.reserve(4);

    specs.push_back({10, 14, 7, 10, true,  false, "STASH"});
    if (wantBuildings >= 2) specs.push_back({7,  10, 6,  8, false, true,  "BUNKS"});
    if (wantBuildings >= 3) specs.push_back({7,  11, 6,  9, false, true,  "WORK"});
    if (wantBuildings >= 4) specs.push_back({6,  9,  5,  7, false, true,  "MESS"});

    std::vector<RectI> placed;
    placed.reserve(specs.size());

    std::vector<Vec2i> buildingEntrances;
    buildingEntrances.reserve(specs.size());
    Vec2i stashDoorOutside{-1, -1};

    // Place buildings with a light packing algorithm.
    for (size_t si = 0; si < specs.size(); ++si) {
        const BuildingSpec& sp = specs[si];

        int bw = clampi(rng.range(sp.w0, sp.w1), 6, std::max(6, campW - 6));
        int bh = clampi(rng.range(sp.h0, sp.h1), 5, std::max(5, campH - 6));

        // Ensure the building fits in the interior with a small margin.
        bw = std::min(bw, std::max(6, campW - 6));
        bh = std::min(bh, std::max(5, campH - 6));

        RectI best;
        int bestScore = -1;

        const int tries = sp.isStash ? 320 : 180;
        for (int t = 0; t < tries; ++t) {
            RectI r;
            r.w = bw;
            r.h = bh;
            r.x = rng.range(campX + 2, std::max(campX + 2, campX2 - 2 - r.w));
            r.y = rng.range(campY + 2, std::max(campY + 2, campY2 - 2 - r.h));

            if (!rectInCampInterior(r, /*marginToPalisade=*/1)) continue;

            // Keep a one-tile moat between buildings for readability.
            RectI rInfl = rectInflate(r, 1);

            bool bad = rectTouchesProtected(rInfl);
            if (bad) continue;

            for (const RectI& o : placed) {
                if (rectOverlaps(rInfl, rectInflate(o, 1))) { bad = true; break; }
            }
            if (bad) continue;

            // Score: stash tries to be far from the gate spine; others mildly prefer edges.
            int score = 0;
            if (sp.isStash) {
                score += std::abs(r.cx() - gateIn.x) + std::abs(r.cy() - gateIn.y);
            } else {
                const int edgeDist = std::min({r.x - (campX + 1), (campX2 - 1) - r.x2(), r.y - (campY + 1), (campY2 - 1) - r.y2()});
                score += 4 * (6 - std::min(6, edgeDist));
                score += rng.range(0, 6);
            }

            if (score > bestScore) {
                bestScore = score;
                best = r;
            }
        }

        if (bestScore < 0) {
            // If we fail to place, just skip the non-essential buildings.
            if (sp.isStash) {
                // Last-resort stash fallback: a small hut near the camp center.
                best = {cx - bw / 2, cy - bh / 2, bw, bh};
                best.x = clampi(best.x, campX + 2, campX2 - 2 - best.w);
                best.y = clampi(best.y, campY + 2, campY2 - 2 - best.h);
            } else {
                continue;
            }
        }

        // Door faces toward the camp center/spine.
        Side doorSide = chooseDoorSideToward(best, {cx, cy});
        TileType doorType = sp.doorOpen ? TileType::DoorOpen : TileType::DoorClosed;
        const DoorInfo di = stampBuilding(best, doorSide, doorType);

        placed.push_back(best);

        // Record entrance so we can lay down protected footpaths/roads later.
        if (d.inBounds(di.outside.x, di.outside.y)) {
            buildingEntrances.push_back(di.outside);
        }

        if (sp.isStash) {
            // Stash anchor in the interior.
            d.campStashSpot = {best.cx(), best.cy()};
            if (d.inBounds(di.outside.x, di.outside.y)) stashDoorOutside = di.outside;
        }

        // Protect around building doorways a bit so the approach stays clear.
        markProtectRadius({best.cx(), best.cy()}, 1);
        if (d.inBounds(di.door.x, di.door.y)) markProtectRadius(di.door, 1);
        if (d.inBounds(di.outside.x, di.outside.y)) markProtectRadius(di.outside, 1);
    }

    // If the stash somehow didn't place, keep the old behavior of anchoring to stairsDown.
    if (!d.inBounds(d.campStashSpot.x, d.campStashSpot.y)) {
        d.campStashSpot = d.stairsDown;
    }

    // ------------------------------------------------------------
    // Camp props: a well and a small fire-ring (boulders), plus sparse supplies.
    // ------------------------------------------------------------
    auto canPlaceProp = [&](Vec2i p) {
        if (!d.inBounds(p.x, p.y)) return false;
        if (p == d.stairsUp || p == d.stairsDown) return false;
        if (chebyshev(p, d.stairsDown) <= 2) return false;
        if (chebyshev(p, gateIn) <= 2) return false;
        if (d.at(p.x, p.y).type != TileType::Floor) return false;
        return true;
    };

    // Well (fountain) near the camp center.
    if (rng.chance(0.70f)) {
        Vec2i well{-1, -1};
        int best = 1 << 30;
        for (int t = 0; t < 180; ++t) {
            Vec2i p{ rng.range(campX + 2, campX2 - 2), rng.range(campY + 2, campY2 - 2) };
            if (!canPlaceProp(p)) continue;
            // Prefer central but not on the main spine.
            const int sc = std::abs(p.x - cx) + std::abs(p.y - cy) + 4 * (isProtected(p.x, p.y) ? 1 : 0);
            if (sc < best) {
                best = sc;
                well = p;
            }
        }
        if (d.inBounds(well.x, well.y)) {
            d.at(well.x, well.y).type = TileType::Fountain;
            d.campWellSpot = well;
            markProtectRadius(well, 1);
        }
    }

    // Fire ring: 4 boulders around a center tile (decorative, but doesn't seal the area).
    if (rng.chance(0.65f)) {
        Vec2i fire{-1, -1};
        for (int t = 0; t < 160; ++t) {
            Vec2i p{ rng.range(campX + 3, campX2 - 3), rng.range(campY + 3, campY2 - 3) };
            if (!canPlaceProp(p)) continue;
            if (isProtected(p.x, p.y)) continue;
            // Avoid blocking the stash.
            if (chebyshev(p, d.campStashSpot) <= 3) continue;
            fire = p;
            break;
        }
        if (d.inBounds(fire.x, fire.y)) {
            d.campFireSpot = fire;
            markProtectRadius(fire, 1);
            const std::array<Vec2i, 4> ring = {{ {fire.x + 1, fire.y}, {fire.x - 1, fire.y}, {fire.x, fire.y + 1}, {fire.x, fire.y - 1} }};
            for (const auto& q : ring) {
                if (!canPlaceProp(q)) continue;
                d.at(q.x, q.y).type = TileType::Boulder;
            }
        }
    }

    // Optional sacred grove outside the palisade: a small stone circle with an altar,
    // connected to the camp by a protected footpath.
    if (rng.chance(0.55f)) {
        const int R = 4;
        const int m = R + 2;

        std::vector<Side> eligible;
        eligible.reserve(4);
        if (campX - m - 1 >= m) eligible.push_back(Side::West);
        if (campX2 + m + 1 <= d.width - m - 1) eligible.push_back(Side::East);
        if (campY - m - 1 >= m) eligible.push_back(Side::North);
        if (campY2 + m + 1 <= d.height - m - 1) eligible.push_back(Side::South);

        auto isPerpToGate = [&](Side s) {
            if (gateSide == Side::North || gateSide == Side::South) return (s == Side::West || s == Side::East);
            return (s == Side::North || s == Side::South);
        };

        Side groveSide = gateSide;
        bool haveGroveSide = false;

        // If we have a side gate, prefer placing the grove on that side.
        if (haveSideGate) {
            for (Side s : eligible) {
                if (s == sideGateSide) { groveSide = s; haveGroveSide = true; break; }
            }
        }

        if (!haveGroveSide) {
            // Prefer perpendicular-to-main-gate sides (keeps the approach spine clean).
            std::vector<Side> pref;
            for (Side s : eligible) if (isPerpToGate(s)) pref.push_back(s);
            if (!pref.empty()) {
                groveSide = pref[static_cast<size_t>(rng.range(0, (int)pref.size() - 1))];
                haveGroveSide = true;
            } else if (!eligible.empty()) {
                groveSide = eligible[static_cast<size_t>(rng.range(0, (int)eligible.size() - 1))];
                haveGroveSide = true;
            }
        }

        if (haveGroveSide) {
            // Choose trail start: side gate if it lines up, otherwise main gate.
            Vec2i trailStart = gateOut;
            if (haveSideGate && groveSide == sideGateSide && d.inBounds(d.campSideGateOut.x, d.campSideGateOut.y)) {
                trailStart = d.campSideGateOut;
            }

            auto canPlaceGroveCenter = [&](Vec2i c) {
                if (!d.inBounds(c.x, c.y)) return false;
                if (isInCampBounds(c)) return false;
                if (isProtected(c.x, c.y)) return false;
                if (d.at(c.x, c.y).type != TileType::Floor) return false;
                if (chebyshev(c, d.stairsUp) <= 7) return false;
                if (chebyshev(c, gateOut) <= 5) return false;
                if (haveSideGate && chebyshev(c, d.campSideGateOut) <= 4) return false;

                for (int dy = -R; dy <= R; ++dy) {
                    for (int dx = -R; dx <= R; ++dx) {
                        const int x = c.x + dx;
                        const int y = c.y + dy;
                        if (!d.inBounds(x, y)) return false;
                        if (isInCampBounds({x, y})) return false;
                        // Don't overwrite the moat with the grove.
                        if (d.at(x, y).type == TileType::Chasm) return false;
                    }
                }
                return true;
            };

            auto pickBiased = [&](Side s) -> Vec2i {
                const int xLo = m;
                const int xHi = d.width - m - 1;
                const int yLo = m;
                const int yHi = d.height - m - 1;

                int x = rng.range(xLo, xHi);
                int y = rng.range(yLo, yHi);

                switch (s) {
                    case Side::West:
                        x = rng.range(xLo, std::max(xLo, campX - m - 1));
                        break;
                    case Side::East:
                        x = rng.range(std::min(xHi, campX2 + m + 1), xHi);
                        break;
                    case Side::North:
                        y = rng.range(yLo, std::max(yLo, campY - m - 1));
                        break;
                    case Side::South:
                        y = rng.range(std::min(yHi, campY2 + m + 1), yHi);
                        break;
                }
                return {x, y};
            };

            Vec2i grove{-1, -1};
            int bestScore = -1;

            for (int t = 0; t < 260; ++t) {
                Vec2i p = pickBiased(groveSide);
                if (!canPlaceGroveCenter(p)) continue;

                const int dist = std::abs(p.x - cx) + std::abs(p.y - cy);
                // Encourage "not too close, not too far".
                int score = dist;
                if (dist < 14) score -= (14 - dist) * 3;
                if (dist > 38) score -= (dist - 38) * 2;

                // Prefer points that are roughly in line with the chosen gate, so the trail reads naturally.
                const int align = std::abs((p.x - trailStart.x)) + std::abs((p.y - trailStart.y));
                score += std::max(0, 20 - std::min(20, align)) / 2;

                score += rng.range(0, 4);
                if (score > bestScore) {
                    bestScore = score;
                    grove = p;
                }
            }

            if (d.inBounds(grove.x, grove.y)) {
                // Carve the clearing.
                for (int dy = -R; dy <= R; ++dy) {
                    for (int dx = -R; dx <= R; ++dx) {
                        carveFloor(d, grove.x + dx, grove.y + dy);
                    }
                }

                // Determine the entrance gap toward the trail start (toward camp).
                Vec2i v{trailStart.x - grove.x, trailStart.y - grove.y};
                Vec2i entrance = grove;
                if (std::abs(v.x) > std::abs(v.y)) {
                    entrance = {grove.x + (v.x >= 0 ? R : -R), grove.y};
                } else {
                    entrance = {grove.x, grove.y + (v.y >= 0 ? R : -R)};
                }

                // Standing stones (pillars) around the border, leaving an entrance gap.
                for (int dy = -R; dy <= R; ++dy) {
                    for (int dx = -R; dx <= R; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != R) continue;
                        Vec2i q{grove.x + dx, grove.y + dy};
                        if (!d.inBounds(q.x, q.y)) continue;

                        // Leave a 3-wide gap at the entrance.
                        if (chebyshev(q, entrance) <= 1) continue;

                        // Sparse pattern for readability.
                        if (((dx + dy) & 1) != 0) continue;

                        if (d.at(q.x, q.y).type == TileType::Floor) {
                            d.at(q.x, q.y).type = TileType::Pillar;
                        }
                    }
                }

                // Altar at the center.
                d.at(grove.x, grove.y).type = TileType::Altar;
                d.campAltarSpot = grove;

                // Keep the grove and trail clear of later wilderness clutter.
                markProtectRadius(grove, R + 2);
                markMeanderPath(trailStart, grove, 2);
            }
        }
    }

    // Protected camp footpaths: mark walkable routes so later clutter doesn't block them.
    // This is purely procedural: we don't add a special tile type, we just reserve space.
    auto protectCampPath = [&](Vec2i a, Vec2i b, int radius) {
        if (!d.inBounds(a.x, a.y) || !d.inBounds(b.x, b.y)) return;
        if (!isInCampBounds(a) || !isInCampBounds(b)) return;

        PassableFn passable = [&](int x, int y) {
            return isInCampBounds({x, y}) && d.isWalkable(x, y);
        };
        StepCostFn stepCost = [&](int, int) { return 1; };
        DiagonalOkFn diag = [&](int, int, int, int) { return false; };

        std::vector<Vec2i> path = dijkstraPath(d.width, d.height, a, b, passable, stepCost, diag);
        if (path.empty()) {
            // Fallback: protect an L path (should be rare with open yard).
            markLPath(a, b, radius);
            return;
        }

        for (const Vec2i& p : path) {
            markProtectRadius(p, radius);
        }
    };

    auto addUnique = [&](std::vector<Vec2i>& v, Vec2i p) {
        if (!d.inBounds(p.x, p.y)) return;
        for (const auto& q : v) {
            if (q == p) return;
        }
        v.push_back(p);
    };

    // Targets for internal footpaths.
    std::vector<Vec2i> roadTargets;
    roadTargets.reserve(16);

    addUnique(roadTargets, d.stairsDown);
    if (haveSideGate) addUnique(roadTargets, d.campSideGateIn);
    if (d.inBounds(d.campWellSpot.x, d.campWellSpot.y)) addUnique(roadTargets, d.campWellSpot);
    if (d.inBounds(d.campFireSpot.x, d.campFireSpot.y)) addUnique(roadTargets, d.campFireSpot);
    if (d.inBounds(stashDoorOutside.x, stashDoorOutside.y)) addUnique(roadTargets, stashDoorOutside);

    for (const Vec2i& e : fixedEntrances) addUnique(roadTargets, e);
    for (const Vec2i& e : buildingEntrances) addUnique(roadTargets, e);

    for (const Vec2i& t : roadTargets) {
        protectCampPath(gateIn, t, 1);
    }


    // Sparse supplies inside the palisade.
    const int supplyCount = clampi((campW * campH) / 180, 2, 10);
    for (int i = 0; i < supplyCount; ++i) {
        for (int t = 0; t < 80; ++t) {
            Vec2i p{ rng.range(campX + 2, campX2 - 2), rng.range(campY + 2, campY2 - 2) };
            if (!canPlaceProp(p)) continue;
            if (isProtected(p.x, p.y)) continue;
            if (rng.chance(0.60f)) d.at(p.x, p.y).type = TileType::Boulder; // crates/logs
            else d.at(p.x, p.y).type = TileType::Pillar; // totems
            break;
        }
    }

    // ------------------------------------------------------------
    // Wilderness dressing: clustered trees/rocks outside the palisade.
    // ------------------------------------------------------------
    struct ObstacleUndo {
        Vec2i p;
        TileType prev;
    };

    std::vector<ObstacleUndo> placedObs;
    placedObs.reserve(256);

    auto canPlaceOutside = [&](Vec2i p) {
        if (!d.inBounds(p.x, p.y)) return false;
        if (isInCampBounds(p)) return false;
        if (p == d.stairsUp || p == d.stairsDown) return false;
        if (chebyshev(p, d.stairsUp) <= 3) return false;
        if (chebyshev(p, gateOut) <= 2) return false;
        if (isProtected(p.x, p.y)) return false;
        if (d.at(p.x, p.y).type != TileType::Floor) return false;
        return true;
    };

    const int outsideArea = (d.width - 2) * (d.height - 2) - (campW * campH);
    int clusterCount = 3 + std::min(5, std::max(0, outsideArea / 1200));
    clusterCount = clampi(clusterCount, 3, 8);

    for (int ci = 0; ci < clusterCount; ++ci) {
        // Pick a center outside the palisade.
        Vec2i c{-1, -1};
        for (int t = 0; t < 220; ++t) {
            Vec2i p{ rng.range(1, d.width - 2), rng.range(1, d.height - 2) };
            if (!canPlaceOutside(p)) continue;
            // Prefer tiles further from the camp gate and stairs.
            const int dg = chebyshev(p, gateOut);
            const int du = chebyshev(p, d.stairsUp);
            if (dg < 6 || du < 6) continue;
            c = p;
            break;
        }
        if (!d.inBounds(c.x, c.y)) continue;

        const bool rocky = rng.chance(0.35f);
        const int radius = rocky ? rng.range(4, 9) : rng.range(5, 11);
        const int placements = rocky ? radius * radius * 2 : radius * radius * 3;

        for (int k = 0; k < placements; ++k) {
            const int ox = rng.range(-radius, radius);
            const int oy = rng.range(-radius, radius);
            if (ox * ox + oy * oy > radius * radius) continue;

            Vec2i p{c.x + ox, c.y + oy};
            if (!canPlaceOutside(p)) continue;

            TileType tt = TileType::Pillar;
            if (rocky) {
                // Mostly boulders with some scrub.
                tt = rng.chance(0.65f) ? TileType::Boulder : TileType::Pillar;
            } else {
                // Mostly trees.
                if (rng.chance(0.15f)) tt = TileType::Boulder;
            }

            placedObs.push_back({p, d.at(p.x, p.y).type});
            d.at(p.x, p.y).type = tt;
        }
    }

    // ------------------------------------------------------------
    // Connectivity guarantee: ensure a walkable path from EXIT to DUNGEON.
    // If wilderness clutter blocks it, we clear only the obstacles along the
    // cheapest "brush-clearing" path.
    // ------------------------------------------------------------
    auto walkableDist = [&]() {
        std::vector<int> dist(static_cast<size_t>(d.width * d.height), -1);
        if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return dist;
        std::deque<Vec2i> q;
        dist[idx(d.stairsUp.x, d.stairsUp.y)] = 0;
        q.push_back(d.stairsUp);
        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        while (!q.empty()) {
            Vec2i p = q.front();
            q.pop_front();
            const int cd = dist[idx(p.x, p.y)];
            for (const auto& dv : dirs) {
                const int nx = p.x + dv[0];
                const int ny = p.y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (!d.isWalkable(nx, ny)) continue;
                const size_t ii = idx(nx, ny);
                if (dist[ii] != -1) continue;
                dist[ii] = cd + 1;
                q.push_back({nx, ny});
            }
        }
        return dist;
    };

    auto ensureWalkableConnectivity = [&]() {
        const auto dist = walkableDist();
        const size_t ii = idx(d.stairsDown.x, d.stairsDown.y);
        if (ii < dist.size() && dist[ii] >= 0) return;

        // Pathfind allowing us to "clear" pillars/boulders (but never walls).
        auto passable = [&](int x, int y) {
            if (!d.inBounds(x, y)) return false;
            const TileType t = d.at(x, y).type;
            if (t == TileType::Wall || t == TileType::Chasm || t == TileType::DoorLocked || t == TileType::DoorSecret) return false;
            if (t == TileType::DoorClosed) return false; // keep the main spine door-free
            return true;
        };

        auto stepCost = [&](int x, int y) {
            if (!d.inBounds(x, y)) return 0;
            const TileType t = d.at(x, y).type;
            if (t == TileType::Floor || t == TileType::DoorOpen || t == TileType::StairsUp || t == TileType::StairsDown || t == TileType::Fountain || t == TileType::Altar) return 1;
            if (t == TileType::Boulder) return 9;
            if (t == TileType::Pillar) return 13;
            return 0;
        };

        auto diagonalOk = [&](int /*fromX*/, int /*fromY*/, int /*dx*/, int /*dy*/) {
            return false; // 4-way only for carving
        };

        std::vector<Vec2i> path = dijkstraPath(d.width, d.height, d.stairsUp, d.stairsDown, passable, stepCost, diagonalOk);
        if (path.empty()) {
            // Last-resort: clear all outside obstacles.
            for (const auto& u : placedObs) {
                if (!d.inBounds(u.p.x, u.p.y)) continue;
                if (d.at(u.p.x, u.p.y).type == TileType::Pillar || d.at(u.p.x, u.p.y).type == TileType::Boulder) {
                    d.at(u.p.x, u.p.y).type = TileType::Floor;
                }
            }
            return;
        }

        // Clear along the path (and a tiny shoulder) so it's definitely walkable.
        for (const Vec2i& p : path) {
            if (!d.inBounds(p.x, p.y)) continue;
            Tile& t = d.at(p.x, p.y);
            if (t.type == TileType::Pillar || t.type == TileType::Boulder) {
                t.type = TileType::Floor;
            }
            // Shoulder.
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = p.x + dx;
                    const int ny = p.y + dy;
                    if (!d.inBounds(nx, ny)) continue;
                    Tile& tt = d.at(nx, ny);
                    if (tt.type == TileType::Pillar || tt.type == TileType::Boulder) {
                        tt.type = TileType::Floor;
                    }
                }
            }
        }
    };

    ensureWalkableConnectivity();

    // One big "camp" room so the renderer can theme the floor as a natural surface.
    Room camp;
    camp.x = 1;
    camp.y = 1;
    camp.w = std::max(1, d.width - 2);
    camp.h = std::max(1, d.height - 2);
    camp.type = RoomType::Camp;
    d.rooms.push_back(camp);
}


// A Sokoban-inspired puzzle floor: the critical path is blocked by multi-tile chasms.
// The player must push boulders into chasms to create bridges.
//
// This is intentionally hand-authored (like the labyrinth/sanctum) so it is always solvable
// as long as the player uses the provided boulders.


[[maybe_unused]] void generateSokoban(Dungeon& d, RNG& rng, int depth) {
    (void)depth;
    fillWalls(d);

    d.rooms.clear();

    const int cy = d.height / 2;

    // --- Core geometry ---
    // Start and exit chambers on the left/right, connected by a 3-wide corridor.
    const int roomW = 16;
    const int roomH = 11;

    const int sx = 2;
    const int sy = clampi(cy - roomH / 2, 2, d.height - roomH - 2);
    carveRect(d, sx, sy, roomW, roomH, TileType::Floor);

    const int ex = d.width - roomW - 3;
    const int ey = clampi(cy - roomH / 2, 2, d.height - roomH - 2);
    carveRect(d, ex, ey, roomW, roomH, TileType::Floor);

    d.stairsUp = {sx + roomW / 2, sy + roomH / 2};

    d.stairsDown = {ex + roomW / 2, ey + roomH / 2};

    const int corX = sx + roomW;
    const int corY = cy - 1;
    const int corW = std::max(1, ex - corX);
    const int corH = 3;
    carveRect(d, corX, corY, corW, corH, TileType::Floor);

    // --- Chasm barriers ---
    // Two multi-column chasm blocks that force incremental bridge-building.
    int b1w = rng.range(3, 5);
    int b2w = rng.range(2, 4);
    b1w = std::clamp(b1w, 3, 6);
    b2w = std::clamp(b2w, 2, 6);

    int b1x = corX + corW / 3 - b1w / 2;
    int b2x = corX + (2 * corW) / 3 - b2w / 2;

    // Ensure a healthy gap between barriers; fall back to stable placements if needed.
    const int b1Min = corX + 10;
    const int b2Max = corX + corW - b2w - 10;
    b1x = clampi(b1x, b1Min, std::max(b1Min, b2Max - (b1w + 18)));
    b2x = clampi(b2x, b1x + b1w + 14, b2Max);
    if (b2x < b1x + b1w + 10) {
        b1x = corX + 16;
        b2x = corX + corW - b2w - 16;
    }

    for (int y = corY; y < corY + corH; ++y) {
        for (int x = b1x; x < b1x + b1w; ++x) {
            if (d.inBounds(x, y)) d.at(x, y).type = TileType::Chasm;
        }
        for (int x = b2x; x < b2x + b2w; ++x) {
            if (d.inBounds(x, y)) d.at(x, y).type = TileType::Chasm;
        }
    }

    // --- Boulder storage (supply) ---
    const int storW = 22;
    const int storH = 11;
    int storX = corX + 6;
    int storY = corY + corH + 4; // leave a wall buffer below the corridor
    storX = clampi(storX, 2, d.width - storW - 2);
    storY = clampi(storY, 2, d.height - storH - 2);
    carveRect(d, storX, storY, storW, storH, TileType::Floor);

    // Narrow vertical access hallway from the main corridor to the storage.
    const int hallX = storX + storW / 2;
    for (int y = corY + corH; y <= storY; ++y) {
        if (d.inBounds(hallX, y)) d.at(hallX, y).type = TileType::Floor;
    }

    // Provide enough boulders to solve both barriers + the optional treasure bridge.
    // Required for main path is b1w + b2w. The treasure detour requires 2 more.
    const int treasureGap = 2;
    const int required = b1w + b2w + treasureGap;
    const int targetBoulders = required + rng.range(2, 5); // extra slack to reduce deadlocks

    int placed = 0;
    for (int y = storY + 2; y <= storY + storH - 3 && placed < targetBoulders; y += 2) {
        for (int x = storX + 2; x <= storX + storW - 3 && placed < targetBoulders; x += 2) {
            // Keep the hallway mouth clear so the player can always access the storage.
            if (x == hallX && y <= storY + 3) continue;
            if (!d.inBounds(x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) continue;
            d.at(x, y).type = TileType::Boulder;
            ++placed;
        }
    }

    // Fallback placement if the grid didn't fit (should be rare, but be safe).
    for (int y = storY + 1; y < storY + storH - 1 && placed < targetBoulders; ++y) {
        for (int x = storX + 1; x < storX + storW - 1 && placed < targetBoulders; ++x) {
            if (x == hallX && y <= storY + 3) continue;
            if (!d.inBounds(x, y)) continue;
            if (d.at(x, y).type != TileType::Floor) continue;
            d.at(x, y).type = TileType::Boulder;
            ++placed;
        }
    }

    // --- Optional treasure detour ---
    // A small room above the main corridor, reachable only by building a short vertical bridge.
    const int rw = 18;
    const int rh = 9;
    const int midX = (b1x + b1w + b2x) / 2;
    int rx = clampi(midX - rw / 2, 2, d.width - rw - 2);
    int ry = clampi(corY - rh - 7, 2, d.height - rh - 2);
    carveRect(d, rx, ry, rw, rh, TileType::Floor);

    const int hall2X = rx + rw / 2;
    for (int y = ry + rh; y <= corY - 1; ++y) {
        if (d.inBounds(hall2X, y)) d.at(hall2X, y).type = TileType::Floor;
    }

    // Insert a 2-tile chasm gap in the hallway (must be bridged with boulders).
    int gapY0 = corY - 4;
    int gapY1 = corY - 3;
    if (gapY0 < ry + rh) {
        gapY0 = ry + rh + 1;
        gapY1 = gapY0 + 1;
    }
    if (gapY1 <= corY - 1) {
        if (d.inBounds(hall2X, gapY0)) d.at(hall2X, gapY0).type = TileType::Chasm;
        if (d.inBounds(hall2X, gapY1)) d.at(hall2X, gapY1).type = TileType::Chasm;
    }

    // Bonus loot spots inside the detour room (spawned as chests by Game::spawnItems).
    d.bonusLootSpots.push_back({rx + rw / 2, ry + rh / 2});
    // rw is currently fixed (18), so this secondary spot is always valid.
    d.bonusLootSpots.push_back({rx + rw / 2 - 3, ry + rh / 2});

    // Rooms (for spawns and room-type mechanics).
    d.rooms.push_back({sx, sy, roomW, roomH, RoomType::Normal});
    d.rooms.push_back({ex, ey, roomW, roomH, RoomType::Normal});
    d.rooms.push_back({storX, storY, storW, storH, RoomType::Normal});
    d.rooms.push_back({rx, ry, rw, rh, RoomType::Treasure});

    // Safety: in small maps, clamped sub-rooms can overlap. Ensure stairs survive any later carving.
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;





}


[[maybe_unused]] void generateSanctum(Dungeon& d, RNG& rng, int depth) {
    (void)depth; // reserved for future per-depth theming
    fillWalls(d);

    // Arena-like final floor with a central locked sanctum and a chasm moat.
    carveRect(d, 1, 1, d.width - 2, d.height - 2, TileType::Floor);

    // Choose three distinct corners for: start, shrine, guard staging.
    enum Corner { TL = 0, TR = 1, BL = 2, BR = 3 };

    int corners[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
        const int j = rng.range(i, 3);
        std::swap(corners[i], corners[j]);
    }
    const Corner startCorner = static_cast<Corner>(corners[0]);
    const Corner shrineCorner = static_cast<Corner>(corners[1]);
    const Corner guardCorner = static_cast<Corner>(corners[2]);

    auto cornerRoom = [&](Corner c, int w, int h, RoomType type) -> Room {
        int x = 2;
        int y = 2;
        switch (c) {
            case TL: x = 2; y = 2; break;
            case TR: x = d.width - w - 2; y = 2; break;
            case BL: x = 2; y = d.height - h - 2; break;
            case BR: x = d.width - w - 2; y = d.height - h - 2; break;
        }

        // Small jitter (inward) so corners aren't identical every run.
        const int jx = rng.range(0, 1);
        const int jy = rng.range(0, 1);
        if (c == TR || c == BR) x -= jx; else x += jx;
        if (c == BL || c == BR) y -= jy; else y += jy;
        x = clampi(x, 2, d.width - w - 2);
        y = clampi(y, 2, d.height - h - 2);
        return Room{x, y, w, h, type};
    };

    // Corner room sizes.
    const int sw = rng.range(7, 10);
    const int sh = rng.range(5, 7);
    const int rw = rng.range(7, 10);
    const int rh = rng.range(5, 7);
    const int gw = rng.range(7, 10);
    const int gh = rng.range(5, 7);

    Room startR = cornerRoom(startCorner, sw, sh, RoomType::Normal);
    Room shrineR = cornerRoom(shrineCorner, rw, rh, RoomType::Shrine);
    Room guardR = cornerRoom(guardCorner, gw, gh, RoomType::Normal);

    // Stairs.
    d.stairsUp = {startR.x + startR.w / 2, startR.y + startR.h / 2};
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.stairsUp = {1, 1};
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;

    // No downstairs on the final floor (Infinite World adds one later if enabled).
    d.stairsDown = {-1, -1};

    const int baseCx = d.width / 2;
    const int baseCy = d.height / 2;
    const int cx = clampi(baseCx + rng.range(-2, 2), 4, d.width - 5);
    const int cy = clampi(baseCy + rng.range(-2, 2), 4, d.height - 5);

    auto makeOdd = [&](int v) { return (v % 2 == 0) ? (v + 1) : v; };

    // Central sanctum size (odd for symmetry).
    int wallW = makeOdd(rng.range(11, 17));
    int wallH = makeOdd(rng.range(7, 11));

    int maxWallW = d.width - 10;
    int maxWallH = d.height - 10;
    if (maxWallW < 11) maxWallW = 11;
    if (maxWallH < 7) maxWallH = 7;
    if (maxWallW % 2 == 0) maxWallW -= 1;
    if (maxWallH % 2 == 0) maxWallH -= 1;
    wallW = clampi(wallW, 11, maxWallW);
    wallH = clampi(wallH, 7, maxWallH);

    int wallX = clampi(cx - wallW / 2, 4, d.width - wallW - 4);
    int wallY = clampi(cy - wallH / 2, 4, d.height - wallH - 4);

    // Walled chamber.
    for (int y = wallY; y < wallY + wallH; ++y) {
        for (int x = wallX; x < wallX + wallW; ++x) {
            if (d.inBounds(x, y)) d.at(x, y).type = TileType::Wall;
        }
    }
    carveRect(d, wallX + 1, wallY + 1, wallW - 2, wallH - 2, TileType::Floor);

    // Locked door on a random wall.
    const int doorSide = rng.range(0, 3); // 0=N,1=E,2=S,3=W
    int doorX = wallX + wallW / 2;
    int doorY = wallY;
    switch (doorSide) {
        case 0: doorX = wallX + wallW / 2; doorY = wallY; break;
        case 1: doorX = wallX + wallW - 1; doorY = wallY + wallH / 2; break;
        case 2: doorX = wallX + wallW / 2; doorY = wallY + wallH - 1; break;
        case 3: doorX = wallX; doorY = wallY + wallH / 2; break;
        default: break;
    }
    if (d.inBounds(doorX, doorY)) d.at(doorX, doorY).type = TileType::DoorLocked;

    // Chasm moat ring (1 tile away from the sanctum wall).
    const int moatX = wallX - 1;
    const int moatY = wallY - 1;
    const int moatW = wallW + 2;
    const int moatH = wallH + 2;

    auto setChasm = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        const TileType t = d.at(x, y).type;
        if (t == TileType::Wall || t == TileType::StairsUp) return;
        d.at(x, y).type = TileType::Chasm;
    };

    for (int x = moatX; x < moatX + moatW; ++x) {
        setChasm(x, moatY);
        setChasm(x, moatY + moatH - 1);
    }
    for (int y = moatY; y < moatY + moatH; ++y) {
        setChasm(moatX, y);
        setChasm(moatX + moatW - 1, y);
    }

    auto carveBridgeAtSide = [&](int side) {
        int bx = wallX + wallW / 2;
        int by = wallY - 1;
        switch (side) {
            case 0: bx = wallX + wallW / 2; by = wallY - 1; break;
            case 1: bx = wallX + wallW;     by = wallY + wallH / 2; break;
            case 2: bx = wallX + wallW / 2; by = wallY + wallH; break;
            case 3: bx = wallX - 1;         by = wallY + wallH / 2; break;
            default: break;
        }
        if (!d.inBounds(bx, by)) return;
        if (d.at(bx, by).type == TileType::StairsUp) return;
        d.at(bx, by).type = TileType::Floor;
    };

    // Always carve the main bridge aligned with the locked door.
    carveBridgeAtSide(doorSide);

    // Add 1-2 extra bridges on other sides to reduce chokepoint frustration.
    int sides[3];
    int n = 0;
    for (int s = 0; s < 4; ++s) {
        if (s == doorSide) continue;
        sides[n++] = s;
    }
    for (int i = 0; i < n; ++i) {
        const int j = rng.range(i, n - 1);
        std::swap(sides[i], sides[j]);
    }
    const int extra = rng.range(1, 2);
    for (int i = 0; i < extra && i < n; ++i) {
        carveBridgeAtSide(sides[i]);
    }

    // Pillars inside the sanctum for cover.
    const int ix0 = wallX + 2;
    const int ix1 = wallX + wallW - 3;
    const int iy0 = wallY + 2;
    const int iy1 = wallY + wallH - 3;
    const Vec2i innerCorners[] = {{ix0, iy0}, {ix1, iy0}, {ix0, iy1}, {ix1, iy1}};
    for (const auto& p : innerCorners) {
        if (!d.inBounds(p.x, p.y)) continue;
        if (d.at(p.x, p.y).type == TileType::Floor) d.at(p.x, p.y).type = TileType::Pillar;
    }

    // Optional inner pair (horizontal or vertical) to vary fight geometry.
    const int icx = wallX + wallW / 2;
    const int icy = wallY + wallH / 2;
    if (rng.chance(0.50f)) {
        const bool horiz = rng.chance(0.50f);
        const Vec2i pair[] = {horiz ? Vec2i{icx - 2, icy} : Vec2i{icx, icy - 1},
                              horiz ? Vec2i{icx + 2, icy} : Vec2i{icx, icy + 1}};
        for (const auto& p : pair) {
            if (!d.inBounds(p.x, p.y)) continue;
            if (d.at(p.x, p.y).type == TileType::Floor) d.at(p.x, p.y).type = TileType::Pillar;
        }
    }

    // Scatter some arena pillars outside the moat (avoid the moat + stairs).
    int wantOuter = rng.range(5, 9);
    int placedOuter = 0;
    int tries = 0;
    while (placedOuter < wantOuter && tries < wantOuter * 60) {
        ++tries;
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);
        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Floor) continue;
        if (Vec2i{x, y} == d.stairsUp) continue;

        // Keep the moat perimeter readable.
        if (x >= moatX - 1 && x <= moatX + moatW && y >= moatY - 1 && y <= moatY + moatH) continue;

        d.at(x, y).type = TileType::Pillar;
        placedOuter++;
    }

    // Define rooms (for spawns and room-type mechanics).
    d.rooms.clear();
    d.rooms.push_back(startR);
    d.rooms.push_back(shrineR);
    d.rooms.push_back(guardR);

    // Sanctum interior is the treasure room.
    d.rooms.push_back({wallX + 1, wallY + 1, wallW - 2, wallH - 2, RoomType::Treasure});
}

} // namespace

Dungeon::Dungeon(int w, int h) : width(w), height(h) {
    tiles.resize(static_cast<size_t>(width * height));
}

bool Dungeon::isWalkable(int x, int y) const {
    if (!inBounds(x, y)) return false;
    TileType t = at(x, y).type;
    return (t == TileType::Floor || t == TileType::Fountain || t == TileType::Altar || t == TileType::DoorOpen || t == TileType::StairsDown || t == TileType::StairsUp);
}

bool Dungeon::isPassable(int x, int y) const {
    if (!inBounds(x, y)) return false;
    TileType t = at(x, y).type;
    // Note: locked doors are NOT passable for pathing/AI until unlocked.
    return (t == TileType::Floor || t == TileType::Fountain || t == TileType::Altar || t == TileType::DoorOpen || t == TileType::DoorClosed || t == TileType::StairsDown || t == TileType::StairsUp);
}

bool Dungeon::isOpaque(int x, int y) const {
    if (!inBounds(x, y)) return true;
    TileType t = at(x, y).type;
    return (t == TileType::Wall || t == TileType::Pillar || t == TileType::DoorClosed || t == TileType::DoorLocked || t == TileType::DoorSecret);
}

bool Dungeon::blocksProjectiles(int x, int y) const {
    if (!inBounds(x, y)) return true;
    const TileType t = at(x, y).type;

    // Projectiles are blocked by any opaque tile, plus boulders.
    // Boulders are intentionally NOT opaque for readability (you can see over/around them),
    // but they should still behave as solid cover for arrows/bolts.
    switch (t) {
        case TileType::Wall:
        case TileType::Pillar:
        case TileType::DoorClosed:
        case TileType::DoorLocked:
        case TileType::DoorSecret:
        case TileType::Boulder:
            return true;
        default:
            return false;
    }
}

bool Dungeon::isDoorClosed(int x, int y) const {
    if (!inBounds(x, y)) return false;
    return at(x, y).type == TileType::DoorClosed;
}

bool Dungeon::isDoorLocked(int x, int y) const {
    if (!inBounds(x, y)) return false;
    return at(x, y).type == TileType::DoorLocked;
}

bool Dungeon::isDoorOpen(int x, int y) const {
    if (!inBounds(x, y)) return false;
    return at(x, y).type == TileType::DoorOpen;
}

void Dungeon::closeDoor(int x, int y) {
    if (!inBounds(x, y)) return;
    if (at(x, y).type == TileType::DoorOpen) {
        at(x, y).type = TileType::DoorClosed;
    }
}

void Dungeon::openDoor(int x, int y) {
    if (!inBounds(x, y)) return;
    if (at(x, y).type == TileType::DoorClosed) {
        at(x, y).type = TileType::DoorOpen;
    }
}

void Dungeon::lockDoor(int x, int y) {
    if (!inBounds(x, y)) return;
    if (at(x, y).type == TileType::DoorClosed) {
        at(x, y).type = TileType::DoorLocked;
    }
}

void Dungeon::unlockDoor(int x, int y) {
    if (!inBounds(x, y)) return;
    if (at(x, y).type == TileType::DoorLocked) {
        // Unlocking converts the door to a normal closed door.
        at(x, y).type = TileType::DoorClosed;
    }
}


bool Dungeon::isDiggable(int x, int y) const {
    if (!inBounds(x, y)) return false;
    TileType t = at(x, y).type;
    switch (t) {
        case TileType::Wall:
        case TileType::Pillar:
        case TileType::DoorClosed:
        case TileType::DoorLocked:
        case TileType::DoorSecret:
            return true;
        default:
            return false;
    }
}

bool Dungeon::dig(int x, int y) {
    if (!inBounds(x, y)) return false;
    if (!isDiggable(x, y)) return false;

    // Digging destroys the obstacle and leaves a clear floor tile behind.
    at(x, y).type = TileType::Floor;
    return true;
}

namespace {


enum class BiomeZoneStyle : uint8_t {
    Clear = 0,
    PillarField,
    Rubble,
    Cracked,
};

// ------------------------------------------------------------
// Biome zones: coherent region theming
//
// Many procgen passes place interesting micro-terrain (pillars, boulders, chasms),
// but purely local randomness can make a floor feel "salt-and-pepper".
// This pass adds a *macro* layer: partition the *walkable* map into a few contiguous
// regions (a Voronoi diagram on the passable graph) and apply a distinct style to
// each region.
//
// Design goals:
// - Stronger spatial identity ("this half is colonnaded ruins", "that corner is rubble").
// - Still fully optional: never blocks stairs connectivity.
// - Never overwrites doors/stairs/special room interiors; only decorates plain Floor.
// - Cheap enough to run during the meta-procgen candidate loop.
//
// Implementation:
// - Pick 2–4 seed tiles using greedy k-center (farthest-from-seeds on passable graph).
// - Multi-source BFS to assign each passable tile to its nearest seed (graph Voronoi).
// - For each region, choose a style and place a small amount of terrain.
// - Roll back a region if it would disconnect stairs.
// ------------------------------------------------------------
static bool applyBiomeZones(Dungeon& d, RNG& rng, int depth, GenKind g, EndlessStratumTheme theme) {
    d.biomeZoneCount = 0;
    d.biomePillarZoneCount = 0;
    d.biomeRubbleZoneCount = 0;
    d.biomeCrackedZoneCount = 0;
    d.biomeEdits = 0;

    // Keep early floors readable and avoid tiny maps.
    if (depth <= 1) return false;
    if (d.width < 24 || d.height < 16) return false;

    // Don't force this every time; it should feel like a "macro-feature" that appears often,
    // but not universally.
    float pAny = 0.42f + 0.03f * static_cast<float>(std::clamp(depth - 2, 0, 10));
    if (g == GenKind::Maze) pAny *= 0.65f;
    if (g == GenKind::Cavern) pAny *= 0.75f;
    pAny = std::min(0.78f, pAny);
    if (!rng.chance(pAny)) return false;

    const int W = d.width;
    const int H = d.height;
    auto idx = [&](int x, int y) -> int { return y * W + x; };

    // Room footprint mask: includes all tiles inside room rectangles.
    std::vector<uint8_t> roomMask(static_cast<size_t>(W * H), uint8_t{0});
    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                roomMask[static_cast<size_t>(idx(x, y))] = 1;
            }
        }
    }

    // Protect the shortest stairs path (+ immediate neighbors) so we avoid placing
    // region obstacles on the "mainline" traversal spine.
    std::vector<uint8_t> protect(static_cast<size_t>(W * H), uint8_t{0});
    auto markProtect = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        protect[static_cast<size_t>(idx(x, y))] = 1;
    };

    // Core path protection.
    std::vector<int> corePath;
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y) && d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        if (buildShortestPassablePath(d, d.stairsUp, d.stairsDown, corePath)) {
            for (int u : corePath) {
                const int x = u % W;
                const int y = u / W;
                markProtect(x, y);
                markProtect(x + 1, y);
                markProtect(x - 1, y);
                markProtect(x, y + 1);
                markProtect(x, y - 1);
            }
        }
    }

    // Extra safety radius near stairs.
    for (int oy = -2; oy <= 2; ++oy) {
        for (int ox = -2; ox <= 2; ++ox) {
            markProtect(d.stairsUp.x + ox, d.stairsUp.y + oy);
            markProtect(d.stairsDown.x + ox, d.stairsDown.y + oy);
        }
    }

    auto isProtected = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return protect[static_cast<size_t>(idx(x, y))] != 0;
    };

    auto nearStairs = [&](int x, int y) -> bool {
        const int du = d.inBounds(d.stairsUp.x, d.stairsUp.y)
            ? (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y))
            : 9999;
        const int dd = d.inBounds(d.stairsDown.x, d.stairsDown.y)
            ? (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y))
            : 9999;
        return du <= 4 || dd <= 4;
    };

    auto passableDeg = [&](int x, int y) -> int {
        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        int c = 0;
        for (const auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (d.isPassable(nx, ny)) ++c;
        }
        return c;
    };

    auto adjacentToWallMass = [&](int x, int y) -> bool {
        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        for (const auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (d.at(nx, ny).type == TileType::Wall) return true;
        }
        return false;
    };

    // Candidate seed tiles: plain floor, not too close to stairs, and not on the protected spine.
    std::vector<int> seedCands;
    seedCands.reserve(static_cast<size_t>(W * H / 3));

    for (int y = 2; y < H - 2; ++y) {
        for (int x = 2; x < W - 2; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (nearStairs(x, y)) continue;
            if (isProtected(x, y)) continue;
            if (anyDoorInRadius(d, x, y, 1)) continue;
            seedCands.push_back(idx(x, y));
        }
    }

    if (seedCands.size() < 220) return false;

    int wantZones = 2;
    if (depth >= 4) wantZones = 3;
    if (depth >= 7 && rng.chance(0.55f)) wantZones = 4;
    wantZones = std::clamp(wantZones, 2, 4);

    // Compute nearest-seed distance via multi-source BFS.
    auto nearestSeedDistance = [&](const std::vector<int>& seeds) -> std::vector<int> {
        std::vector<int> dist(static_cast<size_t>(W * H), -1);
        std::deque<int> q;

        for (int s : seeds) {
            if (s < 0 || s >= W * H) continue;
            dist[static_cast<size_t>(s)] = 0;
            q.push_back(s);
        }

        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        while (!q.empty()) {
            const int u = q.front();
            q.pop_front();

            const int ux = u % W;
            const int uy = u / W;
            const int cd = dist[static_cast<size_t>(u)];

            for (const auto& dv : dirs) {
                const int nx = ux + dv[0];
                const int ny = uy + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (!d.isPassable(nx, ny)) continue;

                const int v = idx(nx, ny);
                if (dist[static_cast<size_t>(v)] != -1) continue;

                dist[static_cast<size_t>(v)] = cd + 1;
                q.push_back(v);
            }
        }

        return dist;
    };

    // 1) Pick seeds by greedy k-center (farthest from current seeds).
    std::vector<int> seeds;
    seeds.reserve(static_cast<size_t>(wantZones));

    seeds.push_back(seedCands[static_cast<size_t>(rng.range(0, static_cast<int>(seedCands.size()) - 1))]);

    const int minSpacing = std::clamp(14 + depth / 2, 14, 20);

    while (static_cast<int>(seeds.size()) < wantZones) {
        const auto dist = nearestSeedDistance(seeds);
        int best = -1;
        int bestD = -1;

        for (int c : seedCands) {
            const int dd = dist[static_cast<size_t>(c)];
            if (dd < 0) continue;
            if (dd > bestD) {
                bestD = dd;
                best = c;
            }
        }

        if (best < 0 || bestD < minSpacing) break;
        seeds.push_back(best);
    }

    if (seeds.size() < 2) return false;

    const int K = static_cast<int>(seeds.size());

    // 2) Assign regions (graph Voronoi) by multi-source BFS.
    std::vector<int> region(static_cast<size_t>(W * H), -1);
    std::deque<int> q;

    // Shuffle seed visitation order a bit to avoid deterministic tie artifacts.
    std::vector<int> seedOrder(static_cast<size_t>(K), 0);
    for (int i = 0; i < K; ++i) seedOrder[static_cast<size_t>(i)] = i;
    for (int i = K - 1; i >= 1; --i) {
        const int j = rng.range(0, i);
        std::swap(seedOrder[static_cast<size_t>(i)], seedOrder[static_cast<size_t>(j)]);
    }

    for (int si : seedOrder) {
        const int s = seeds[static_cast<size_t>(si)];
        if (s < 0 || s >= W * H) continue;
        region[static_cast<size_t>(s)] = si;
        q.push_back(s);
    }

    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    while (!q.empty()) {
        const int u = q.front();
        q.pop_front();

        const int rid = region[static_cast<size_t>(u)];
        const int ux = u % W;
        const int uy = u / W;

        for (const auto& dv : dirs) {
            const int nx = ux + dv[0];
            const int ny = uy + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;

            const int v = idx(nx, ny);
            if (region[static_cast<size_t>(v)] != -1) continue;

            region[static_cast<size_t>(v)] = rid;
            q.push_back(v);
        }
    }

    // Per-region stats to bias style choice.
    std::vector<int> regionArea(static_cast<size_t>(K), 0);
    std::vector<int> regionRoomArea(static_cast<size_t>(K), 0);
    std::vector<int> regionOpenArea(static_cast<size_t>(K), 0);

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (!d.isPassable(x, y)) continue;
            const int i = idx(x, y);
            const int rid = region[static_cast<size_t>(i)];
            if (rid < 0 || rid >= K) continue;

            regionArea[static_cast<size_t>(rid)] += 1;
            if (roomMask[static_cast<size_t>(i)] != 0) regionRoomArea[static_cast<size_t>(rid)] += 1;
            if (passableDeg(x, y) >= 3) regionOpenArea[static_cast<size_t>(rid)] += 1;
        }
    }

    // 3) Choose a style per region.
    std::vector<BiomeZoneStyle> styles(static_cast<size_t>(K), BiomeZoneStyle::Clear);

    int cracksAssigned = 0;

    for (int rid = 0; rid < K; ++rid) {
        const int a = std::max(1, regionArea[static_cast<size_t>(rid)]);
        const float roomFrac = static_cast<float>(regionRoomArea[static_cast<size_t>(rid)]) / static_cast<float>(a);
        const float openFrac = static_cast<float>(regionOpenArea[static_cast<size_t>(rid)]) / static_cast<float>(a);

        // Weight heuristics:
        // - Rooms/open regions skew toward PillarField.
        // - Corridor-heavy regions skew toward Rubble.
        // - Cracks are rare and depth-gated (and avoided on mazes/mines where they can feel unfair).
        int wClear = 1;
        int wPillar = (roomFrac > 0.28f || openFrac > 0.45f) ? 4 : 1;
        int wRubble = (roomFrac > 0.28f || openFrac > 0.45f) ? 1 : 4;

        bool allowCracks = (depth >= 4) && (g != GenKind::Maze) && (g != GenKind::Mines);
        int wCrack = allowCracks ? ((depth >= 8) ? 2 : 1) : 0;
        if (cracksAssigned >= 1) wCrack = 0;

        // Endless strata bias: nudge region styles to reinforce the macro theme.
        // This is intentionally subtle: it shouldn't override the region's own geometry heuristics.
        switch (theme) {
            case EndlessStratumTheme::Ruins:
                wPillar += 1;
                wRubble = std::max(0, wRubble - 1);
                break;
            case EndlessStratumTheme::Caverns:
                wClear += 1;
                wPillar = std::max(0, wPillar - 1);
                break;
            case EndlessStratumTheme::Labyrinth:
                // Keep labyrinth floors readable: slightly favor clear zones.
                wClear += 1;
                break;
            case EndlessStratumTheme::Warrens:
                wRubble += 1;
                wPillar = std::max(0, wPillar - 1);
                break;
            case EndlessStratumTheme::Mines:
                wRubble += 2;
                wPillar = std::max(0, wPillar - 1);
                break;
            case EndlessStratumTheme::Catacombs:
                if (allowCracks) wCrack += 1;
                wRubble += 1;
                break;
            default:
                break;
        }

        const int total = std::max(1, wClear + wPillar + wRubble + wCrack);
        int r = rng.range(1, total);

        if ((r -= wClear) <= 0) styles[static_cast<size_t>(rid)] = BiomeZoneStyle::Clear;
        else if ((r -= wPillar) <= 0) styles[static_cast<size_t>(rid)] = BiomeZoneStyle::PillarField;
        else if ((r -= wRubble) <= 0) styles[static_cast<size_t>(rid)] = BiomeZoneStyle::Rubble;
        else styles[static_cast<size_t>(rid)] = BiomeZoneStyle::Cracked;

        if (styles[static_cast<size_t>(rid)] == BiomeZoneStyle::Cracked) cracksAssigned += 1;
    }

    // Ensure at least one clear zone for readability.
    bool anyClear = false;
    for (BiomeZoneStyle s : styles) {
        if (s == BiomeZoneStyle::Clear) { anyClear = true; break; }
    }
    if (!anyClear) {
        styles[static_cast<size_t>(rng.range(0, K - 1))] = BiomeZoneStyle::Clear;
    }

    // If everything ended up with the same style, force variety.
    bool allSame = true;
    for (int i = 1; i < K; ++i) {
        if (styles[static_cast<size_t>(i)] != styles[0]) { allSame = false; break; }
    }
    if (allSame) styles[0] = BiomeZoneStyle::Clear;

    // Distance-from-up stairs to bias placements away from the start.
    const auto distFromUp = bfsDistanceMap(d, d.stairsUp);
    auto distUpAt = [&](int x, int y) -> int {
        if (!d.inBounds(x, y)) return -1;
        return distFromUp[static_cast<size_t>(idx(x, y))];
    };

    auto isTileOk = [&](int rid, int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (region[static_cast<size_t>(idx(x, y))] != rid) return false;

        if (d.at(x, y).type != TileType::Floor) return false;
        if (isProtected(x, y)) return false;
        if (nearStairs(x, y)) return false;
        if (anyDoorInRadius(d, x, y, 1)) return false;

        // Keep out of non-normal rooms; these have their own bespoke decorations/logic.
        if (!d.rooms.empty() && roomMask[static_cast<size_t>(idx(x, y))] != 0) {
            const Room* rr = findRoomContaining(d, x, y);
            if (rr && rr->type != RoomType::Normal) return false;
        }

        // Keep closer-to-start areas cleaner.
        const int du = distUpAt(x, y);
        if (du >= 0 && du < (10 + depth / 2)) return false;

        return true;
    };

    auto shuffleVec = [&](std::vector<Vec2i>& v) {
        for (int i = static_cast<int>(v.size()) - 1; i >= 1; --i) {
            const int j = rng.range(0, i);
            std::swap(v[static_cast<size_t>(i)], v[static_cast<size_t>(j)]);
        }
    };

    auto applyPillarField = [&](int rid, std::vector<TileChange>& changes) -> int {
        const int area = std::max(1, regionArea[static_cast<size_t>(rid)]);
        int want = std::clamp(area / 180, 6, 26);
        if (depth >= 7 && rng.chance(0.25f)) want += 2;
        want = std::clamp(want, 6, 30);

        const int spacing = (depth >= 7) ? 4 : 3;
        const int offX = rng.range(0, spacing - 1);
        const int offY = rng.range(0, spacing - 1);

        std::vector<Vec2i> cands;
        cands.reserve(static_cast<size_t>(want * 4));

        for (int y = 2; y < H - 2; ++y) {
            for (int x = 2; x < W - 2; ++x) {
                if (!isTileOk(rid, x, y)) continue;
                if (((x + offX) % spacing) != 0) continue;
                if (((y + offY) % spacing) != 0) continue;

                // Only place pillars in reasonably open tiles so we don't create new chokepoints.
                if (passableDeg(x, y) < 3) continue;

                cands.push_back({x, y});
            }
        }

        if (cands.empty()) return 0;
        shuffleVec(cands);

        int placed = 0;
        for (const Vec2i& p : cands) {
            if (placed >= want) break;
            trySetTile(d, p.x, p.y, TileType::Pillar, changes);
            if (d.at(p.x, p.y).type == TileType::Pillar) placed += 1;
        }
        return placed;
    };

    auto applyRubble = [&](int rid, std::vector<TileChange>& changes) -> int {
        int clusters = 1;
        if (depth >= 6 && rng.chance(0.40f)) clusters += 1;
        clusters = std::min(2, clusters);

        std::vector<Vec2i> centers;
        centers.reserve(256);

        for (int y = 2; y < H - 2; ++y) {
            for (int x = 2; x < W - 2; ++x) {
                if (!isTileOk(rid, x, y)) continue;
                if (passableDeg(x, y) < 3) continue;
                if (!adjacentToWallMass(x, y)) continue;
                centers.push_back({x, y});
            }
        }

        // If the region is very open (caverns), loosen the wall-adjacent requirement.
        if (centers.empty()) {
            for (int y = 2; y < H - 2; ++y) {
                for (int x = 2; x < W - 2; ++x) {
                    if (!isTileOk(rid, x, y)) continue;
                    if (passableDeg(x, y) < 3) continue;
                    centers.push_back({x, y});
                }
            }
        }

        if (centers.empty()) return 0;

        // Bias toward deeper (farther from upstairs) for pacing.
        std::sort(centers.begin(), centers.end(), [&](const Vec2i& a, const Vec2i& b) {
            return distUpAt(a.x, a.y) > distUpAt(b.x, b.y);
        });

        int placed = 0;

        for (int c = 0; c < clusters && !centers.empty(); ++c) {
            const int slice = std::max(1, static_cast<int>(centers.size()) / 4);
            Vec2i center = centers[static_cast<size_t>(rng.range(0, slice - 1))];

            const int drops = rng.range(5, 9);
            for (int i = 0; i < drops; ++i) {
                const int ox = rng.range(-2, 2);
                const int oy = rng.range(-2, 2);
                const int x = center.x + ox;
                const int y = center.y + oy;

                if (!isTileOk(rid, x, y)) continue;
                if (passableDeg(x, y) < 3) continue;

                // Prefer boulders that "lean" against walls for visual coherence.
                if (!adjacentToWallMass(x, y) && rng.chance(0.65f)) continue;

                trySetTile(d, x, y, TileType::Boulder, changes);
                if (d.at(x, y).type == TileType::Boulder) placed += 1;
            }
        }

        return placed;
    };

    auto applyCracked = [&](int rid, std::vector<TileChange>& changes) -> int {
        int cracks = 1;
        if (depth >= 7 && rng.chance(0.35f)) cracks += 1;

        std::vector<Vec2i> starts;
        starts.reserve(256);

        for (int y = 2; y < H - 2; ++y) {
            for (int x = 2; x < W - 2; ++x) {
                if (!isTileOk(rid, x, y)) continue;
                if (passableDeg(x, y) < 3) continue;
                starts.push_back({x, y});
            }
        }
        if (starts.empty()) return 0;

        // Bias deeper, pick from the best slice.
        std::sort(starts.begin(), starts.end(), [&](const Vec2i& a, const Vec2i& b) {
            return distUpAt(a.x, a.y) > distUpAt(b.x, b.y);
        });

        const Vec2i dir4[4] = {{1,0},{-1,0},{0,1},{0,-1}};

        int placed = 0;

        for (int k = 0; k < cracks && !starts.empty(); ++k) {
            const int slice = std::max(1, static_cast<int>(starts.size()) / 5);
            Vec2i p = starts[static_cast<size_t>(rng.range(0, slice - 1))];

            Vec2i dir = dir4[rng.range(0, 3)];
            const int len = std::clamp(rng.range(6, 10 + depth / 2), 6, 18);

            for (int step = 0; step < len; ++step) {
                if (!isTileOk(rid, p.x, p.y)) break;

                trySetTile(d, p.x, p.y, TileType::Chasm, changes);
                if (d.at(p.x, p.y).type == TileType::Chasm) placed += 1;

                // Direction inertia with occasional kinks.
                if (rng.chance(0.22f)) {
                    // Turn left/right (avoid full reversal most of the time).
                    if (dir.x != 0) dir = rng.chance(0.5f) ? Vec2i{0, 1} : Vec2i{0, -1};
                    else dir = rng.chance(0.5f) ? Vec2i{1, 0} : Vec2i{-1, 0};
                } else if (rng.chance(0.08f)) {
                    // Rare zig-zag: pick any direction.
                    dir = dir4[rng.range(0, 3)];
                }

                Vec2i nxt{p.x + dir.x, p.y + dir.y};
                if (!d.inBounds(nxt.x, nxt.y)) break;
                p = nxt;
            }

            // Safety check after each crack: never allow disconnection.
            if (!changes.empty() && !stairsConnected(d)) {
                undoChanges(d, changes);
                return 0;
            }
        }

        // Deep cracked zones sometimes have a convenient boulder nearby, hinting at bridge-play.
        if (placed > 0 && depth >= 6 && rng.chance(0.35f)) {
            for (int tries = 0; tries < 60; ++tries) {
                Vec2i pp = starts[static_cast<size_t>(rng.range(0, static_cast<int>(starts.size()) - 1))];
                if (!isTileOk(rid, pp.x, pp.y)) continue;
                if (passableDeg(pp.x, pp.y) < 3) continue;
                // Place boulder near existing chasm in this region.
                bool nearChasm = false;
                for (const auto& dv : dir4) {
                    const int nx = pp.x + dv.x;
                    const int ny = pp.y + dv.y;
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type == TileType::Chasm) { nearChasm = true; break; }
                }
                if (!nearChasm) continue;

                trySetTile(d, pp.x, pp.y, TileType::Boulder, changes);
                if (d.at(pp.x, pp.y).type == TileType::Boulder) { placed += 1; break; }
            }
        }

        return placed;
    };

    // 4) Apply regions (with rollback if anything breaks stairs connectivity).
    int edits = 0;
    int pillarsZones = 0;
    int rubbleZones = 0;
    int crackedZones = 0;

    for (int rid = 0; rid < K; ++rid) {
        const BiomeZoneStyle s = styles[static_cast<size_t>(rid)];
        if (s == BiomeZoneStyle::Clear) continue;

        std::vector<TileChange> changes;
        changes.reserve(96);

        int placed = 0;
        if (s == BiomeZoneStyle::PillarField) placed = applyPillarField(rid, changes);
        else if (s == BiomeZoneStyle::Rubble) placed = applyRubble(rid, changes);
        else if (s == BiomeZoneStyle::Cracked) placed = applyCracked(rid, changes);

        if (placed <= 0) continue;

        if (!stairsConnected(d)) {
            undoChanges(d, changes);
            continue;
        }

        edits += placed;

        if (s == BiomeZoneStyle::PillarField) pillarsZones += 1;
        else if (s == BiomeZoneStyle::Rubble) rubbleZones += 1;
        else if (s == BiomeZoneStyle::Cracked) crackedZones += 1;
    }

    if (edits <= 0) {
        // Treat as not-applied so #mapstats doesn't report a "zone pass" that did nothing.
        d.biomeZoneCount = 0;
        d.biomePillarZoneCount = 0;
        d.biomeRubbleZoneCount = 0;
        d.biomeCrackedZoneCount = 0;
        d.biomeEdits = 0;
        return false;
    }

    d.biomeZoneCount = K;
    d.biomePillarZoneCount = pillarsZones;
    d.biomeRubbleZoneCount = rubbleZones;
    d.biomeCrackedZoneCount = crackedZones;
    d.biomeEdits = edits;
    return true;
}

// ------------------------------------------------------------
// Open-space breakup (clearance-driven cover equalization)
//
// Some generators (especially room packs / caverns) can occasionally produce
// very large open "kill boxes" where ranged combat becomes overly deterministic.
// This pass adds a lightweight analysis-driven correction:
//
// - Compute a Manhattan distance-to-obstacle "clearance" field via multi-source BFS.
// - Treat the maximum clearance as a proxy for "largest empty open area radius".
// - If the max is above a depth-tuned target, place a few pillars/boulders at
//   clearance maxima to add occlusion/cover.
// - Always protect the shortest stairs path and roll back any placement that
//   would disconnect stairs connectivity.
// ------------------------------------------------------------
static std::vector<int> computeClearanceFieldL1(const Dungeon& d) {
    const int W = d.width;
    const int H = d.height;
    const int N = W * H;
    const int INF = 1'000'000'000;

    std::vector<int> dist(static_cast<size_t>(N), INF);
    std::deque<int> q;

    auto idx = [&](int x, int y) -> int { return y * W + x; };

    // Multi-source BFS from all impassable tiles (walls, chasms, pillars, boulders, locked doors).
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!d.isPassable(x, y)) {
                const int i = idx(x, y);
                dist[static_cast<size_t>(i)] = 0;
                q.push_back(i);
            }
        }
    }

    if (q.empty()) {
        // Degenerate (shouldn't happen): if everything is passable, treat clearance as 0.
        std::fill(dist.begin(), dist.end(), 0);
        return dist;
    }

    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        const int u = q.front();
        q.pop_front();

        const int ux = u % W;
        const int uy = u / W;
        const int du = dist[static_cast<size_t>(u)];

        for (const auto& dv : dirs) {
            const int nx = ux + dv[0];
            const int ny = uy + dv[1];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;

            const int v = idx(nx, ny);
            const int nd = du + 1;

            int& slot = dist[static_cast<size_t>(v)];
            if (nd < slot) {
                slot = nd;
                q.push_back(v);
            }
        }
    }

    return dist;
}

static int maxPassableClearance(const Dungeon& d, const std::vector<int>& clearance) {
    const int W = d.width;
    const int H = d.height;

    auto idx = [&](int x, int y) -> int { return y * W + x; };

    int best = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!d.isPassable(x, y)) continue;
            const int c = clearance[static_cast<size_t>(idx(x, y))];
            if (c > best) best = c;
        }
    }
    return best;
}

static bool applyOpenSpaceBreakup(Dungeon& d, RNG& rng, int depth, GenKind g) {
    d.openSpacePillarCount = 0;
    d.openSpaceBoulderCount = 0;

    // Always compute baseline clearance (useful for scoring/debug even if the pass doesn't apply).
    {
        const auto base = computeClearanceFieldL1(d);
        d.openSpaceClearanceMaxBefore = maxPassableClearance(d, base);
        d.openSpaceClearanceMaxAfter = d.openSpaceClearanceMaxBefore;
    }

    // Keep early floors readable and avoid tiny maps.
    if (depth <= 1) return false;
    if (d.width < 24 || d.height < 16) return false;

    // Depth-tuned "how open is too open" target (Manhattan clearance).
    int target = std::clamp(9 - depth / 2, 5, 9);
    if (g == GenKind::Cavern) target += 1;                 // caverns can stay a bit more open
    if (g == GenKind::Mines || g == GenKind::Warrens) target = std::max(5, target - 1);
    target = std::clamp(target, 5, 10);

    // Only bother when there is a genuinely huge open area.
    if (d.openSpaceClearanceMaxBefore <= target + 1) return false;

    float p = 0.50f + 0.03f * static_cast<float>(std::clamp(depth - 2, 0, 12));
    if (g == GenKind::Cavern) p *= 0.78f;
    if (g == GenKind::Maze) p *= 0.55f;
    if (g == GenKind::Mines || g == GenKind::Warrens) p *= 0.82f;
    if (g == GenKind::RoomsGraph) p *= 1.05f;
    p = std::min(0.82f, p);
    if (!rng.chance(p)) return false;

    const int W = d.width;
    const int H = d.height;
    auto idx = [&](int x, int y) -> int { return y * W + x; };

    // Protect shortest stairs path (+ neighbors) so we don't spike the mainline.
    std::vector<uint8_t> protect(static_cast<size_t>(W * H), uint8_t{0});
    auto markProtect = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        protect[static_cast<size_t>(idx(x, y))] = 1;
    };

    std::vector<int> corePath;
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y) && d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        if (buildShortestPassablePath(d, d.stairsUp, d.stairsDown, corePath)) {
            for (int u : corePath) {
                const int x = u % W;
                const int y = u / W;
                markProtect(x, y);
                markProtect(x + 1, y);
                markProtect(x - 1, y);
                markProtect(x, y + 1);
                markProtect(x, y - 1);
            }
        }
    }

    for (int oy = -2; oy <= 2; ++oy) {
        for (int ox = -2; ox <= 2; ++ox) {
            markProtect(d.stairsUp.x + ox, d.stairsUp.y + oy);
            markProtect(d.stairsDown.x + ox, d.stairsDown.y + oy);
        }
    }

    auto isProtected = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return protect[static_cast<size_t>(idx(x, y))] != 0;
    };

    auto passableDeg = [&](int x, int y) -> int {
        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        int c = 0;
        for (const auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (d.isPassable(nx, ny)) ++c;
        }
        return c;
    };

    // Optional spacing so we don't stack cover on itself.
    std::vector<uint8_t> blocked(static_cast<size_t>(W * H), uint8_t{0});
    auto isBlocked = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        return blocked[static_cast<size_t>(idx(x, y))] != 0;
    };
    auto markBlocked = [&](int cx, int cy, int rad) {
        for (int oy = -rad; oy <= rad; ++oy) {
            for (int ox = -rad; ox <= rad; ++ox) {
                if (std::abs(ox) + std::abs(oy) > rad) continue;
                const int x = cx + ox;
                const int y = cy + oy;
                if (!d.inBounds(x, y)) continue;
                blocked[static_cast<size_t>(idx(x, y))] = 1;
            }
        }
    };

    auto isCandidateOk = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (x <= 1 || y <= 1 || x >= W - 2 || y >= H - 2) return false;

        if (isProtected(x, y)) return false;
        if (isStairsTile(d, x, y)) return false;
        if (nearStairs(d, x, y, 4)) return false;
        if (isBlocked(x, y)) return false;

        if (d.at(x, y).type != TileType::Floor) return false;
        if (anyDoorInRadius(d, x, y, 1)) return false;

        // Avoid non-normal special rooms (shops/shrines/treasure/vaults).
        if (const Room* rr = findRoomContaining(d, x, y)) {
            if (rr->type != RoomType::Normal) return false;
        }

        // Don't drop hard blockers into narrow hallways; keep this for open spaces.
        if (passableDeg(x, y) < 3) return false;

        return true;
    };

    const int maxOps = std::clamp(1 + depth / 2, 2, 7);
    const int minSep = std::clamp(3 + depth / 4, 3, 5);

    int ops = 0;
    while (ops < maxOps) {
        const auto clearance = computeClearanceFieldL1(d);
        const int curMax = maxPassableClearance(d, clearance);
        d.openSpaceClearanceMaxAfter = curMax;

        if (curMax <= target) break;

        // Collect candidates at the current maximum clearance.
        std::vector<Vec2i> cands;
        cands.reserve(128);

        for (int y = 2; y < H - 2; ++y) {
            for (int x = 2; x < W - 2; ++x) {
                if (!isCandidateOk(x, y)) continue;
                const int c = clearance[static_cast<size_t>(idx(x, y))];
                if (c < curMax) continue;
                cands.push_back({x, y});
            }
        }

        if (cands.empty()) break;

        // Choose from the best slice to keep variety while still targeting maxima.
        const int slice = std::max(1, static_cast<int>(cands.size()) / 3);
        Vec2i pos = cands[static_cast<size_t>(rng.range(0, slice - 1))];

        // Decide cover type: rooms like pillars (LOS blockers), caverns/tunnels like boulders.
        TileType place = TileType::Pillar;
        float pBoulder = 0.25f;
        if (g == GenKind::Cavern) pBoulder = 0.60f;
        else if (g == GenKind::Mines || g == GenKind::Warrens) pBoulder = 0.55f;
        else if (g == GenKind::Maze) pBoulder = 0.45f;

        if (rng.chance(pBoulder)) place = TileType::Boulder;

        const TileType prev = d.at(pos.x, pos.y).type;
        d.at(pos.x, pos.y).type = place;

        if (!stairsConnected(d)) {
            d.at(pos.x, pos.y).type = prev;
            markBlocked(pos.x, pos.y, minSep);
            continue;
        }

        if (place == TileType::Pillar) d.openSpacePillarCount += 1;
        else if (place == TileType::Boulder) d.openSpaceBoulderCount += 1;

        markBlocked(pos.x, pos.y, minSep);
        ops += 1;
    }

    // Final recompute.
    {
        const auto final = computeClearanceFieldL1(d);
        d.openSpaceClearanceMaxAfter = maxPassableClearance(d, final);
    }

    return (d.openSpacePillarCount + d.openSpaceBoulderCount) > 0;
}


struct FireLaneInfo {
    int len = 0;
    int sx = 0;
    int sy = 0;
    int dx = 0;
    int dy = 0;
};

// A "fire lane" is a straight horizontal or vertical line of tiles where a projectile could
// travel without being blocked by cover (walls/doors/pillars/boulders).
//
// We use this metric as a tactical readability knob: extremely long corridors become "sniper
// alleys" that can make ranged combat swingy. A small post-pass can add micro-cover that breaks
// those lines while never harming stair connectivity.
static bool isFireLaneTile(const Dungeon& d, int x, int y) {
    if (!d.inBounds(x, y)) return false;
    if (!d.isPassable(x, y)) return false;
    return !d.blocksProjectiles(x, y);
}

static FireLaneInfo findLongestFireLane(const Dungeon& d) {
    FireLaneInfo best;
    const int W = d.width;
    const int H = d.height;

    // Horizontal scan.
    for (int y = 0; y < H; ++y) {
        int x = 0;
        while (x < W) {
            if (!isFireLaneTile(d, x, y)) { ++x; continue; }
            const int sx = x;
            int len = 0;
            while (x < W && isFireLaneTile(d, x, y)) { ++x; ++len; }
            if (len > best.len) best = {len, sx, y, 1, 0};
        }
    }

    // Vertical scan.
    for (int x = 0; x < W; ++x) {
        int y = 0;
        while (y < H) {
            if (!isFireLaneTile(d, x, y)) { ++y; continue; }
            const int sy = y;
            int len = 0;
            while (y < H && isFireLaneTile(d, x, y)) { ++y; ++len; }
            if (len > best.len) best = {len, x, sy, 0, 1};
        }
    }

    return best;
}

static int computeMaxFireLane(const Dungeon& d) {
    return findLongestFireLane(d).len;
}

// ------------------------------------------------------------
// Fire lane dampening (tactical cover)
//
// Strategy:
// - Measure the longest straight projectile lane (maxFireLane).
// - If it's over a depth-tuned target, iteratively place micro-cover.
// - Prefer "barricade chicanes": put a boulder in the lane AND carve a 1-tile side-step bypass
//   so movement isn't blocked.
// - If chicane carving isn't possible, fall back to dropping a single cover tile in an open area,
//   but always rollback if stair connectivity would break.
//
// This keeps maps tactically interesting without heavy-handed corridor rewrites.
// ------------------------------------------------------------
static bool applyFireLaneDampening(Dungeon& d, RNG& rng, int depth, GenKind g) {
    d.fireLaneCoverCount = 0;
    d.fireLaneChicaneCount = 0;

    d.fireLaneMaxBefore = computeMaxFireLane(d);
    d.fireLaneMaxAfter = d.fireLaneMaxBefore;

    // Keep early floors readable and avoid tiny maps.
    if (depth <= 1) return false;
    if (d.width < 24 || d.height < 16) return false;

    // Only bother when there are genuinely long straight lanes.
    if (d.fireLaneMaxBefore < 14) return false;

    float p = 0.55f + 0.03f * static_cast<float>(std::clamp(depth - 2, 0, 10));
    if (g == GenKind::Maze) p *= 0.75f;
    if (g == GenKind::Mines || g == GenKind::Warrens) p *= 0.85f;
    p = std::min(0.85f, p);
    if (!rng.chance(p)) return false;

    const int target = std::clamp(22 - depth, 14, 22);
    const int maxOps = std::clamp(1 + depth / 3, 1, 5);

    auto passableDeg = [&](int x, int y) -> int {
        static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        int c = 0;
        for (const auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (d.isPassable(nx, ny)) ++c;
        }
        return c;
    };

    auto isOkBaseTile = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (isStairsTile(d, x, y)) return false;
        if (nearStairs(d, x, y, 4)) return false;
        if (anyDoorInRadius(d, x, y, 1)) return false;
        if (d.at(x, y).type != TileType::Floor) return false;

        // Avoid messing with special room interiors (shops/shrines/etc).
        if (const Room* rr = findRoomContaining(d, x, y)) {
            if (rr->type != RoomType::Normal) return false;
        }
        return true;
    };

    auto isDigWall = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        return d.at(x, y).type == TileType::Wall;
    };

    auto tryChicaneAt = [&](int x, int y, int dx, int /*dy*/) -> bool {
        // Place a boulder in (x,y) and carve a 3-tile bypass on one side.
        // Horizontal lane -> carve above/below; vertical lane -> carve left/right.
        if (!isOkBaseTile(x, y)) return false;
        if (passableDeg(x, y) != 2) return false;

        int sideFirst = rng.chance(0.5f) ? 1 : -1;
        for (int sTry = 0; sTry < 2; ++sTry) {
            const int side = (sTry == 0) ? sideFirst : -sideFirst;

            std::vector<TileChange> changes;
            changes.reserve(8);

            if (dx != 0) {
                // Horizontal lane: carve (x-1..x+1, y+side)
                if (!d.inBounds(x - 1, y + side) || !d.inBounds(x + 1, y + side)) continue;
                if (!isDigWall(x - 1, y + side) || !isDigWall(x, y + side) || !isDigWall(x + 1, y + side)) continue;
                if (nearStairs(d, x, y + side, 3)) continue;
                if (anyDoorInRadius(d, x - 1, y + side, 1) || anyDoorInRadius(d, x, y + side, 1) || anyDoorInRadius(d, x + 1, y + side, 1)) continue;

                forceSetTileFeature(d, x, y, TileType::Boulder, changes);
                forceSetTileFeature(d, x - 1, y + side, TileType::Floor, changes);
                forceSetTileFeature(d, x, y + side, TileType::Floor, changes);
                forceSetTileFeature(d, x + 1, y + side, TileType::Floor, changes);
            } else {
                // Vertical lane: carve (x+side, y-1..y+1)
                if (!d.inBounds(x + side, y - 1) || !d.inBounds(x + side, y + 1)) continue;
                if (!isDigWall(x + side, y - 1) || !isDigWall(x + side, y) || !isDigWall(x + side, y + 1)) continue;
                if (nearStairs(d, x + side, y, 3)) continue;
                if (anyDoorInRadius(d, x + side, y - 1, 1) || anyDoorInRadius(d, x + side, y, 1) || anyDoorInRadius(d, x + side, y + 1, 1)) continue;

                forceSetTileFeature(d, x, y, TileType::Boulder, changes);
                forceSetTileFeature(d, x + side, y - 1, TileType::Floor, changes);
                forceSetTileFeature(d, x + side, y, TileType::Floor, changes);
                forceSetTileFeature(d, x + side, y + 1, TileType::Floor, changes);
            }

            if (changes.empty()) continue;
            if (!stairsConnected(d)) {
                undoChanges(d, changes);
                continue;
            }

            d.fireLaneCoverCount += 1;
            d.fireLaneChicaneCount += 1;
            return true;
        }

        return false;
    };

    auto tryDropCoverAt = [&](int x, int y) -> bool {
        if (!isOkBaseTile(x, y)) return false;

        // Only do this when the tile is reasonably open, so we don't create a new chokepoint.
        const int deg = passableDeg(x, y);
        if (deg < 3) return false;

        std::vector<TileChange> changes;
        changes.reserve(2);

        // Deeper floors can occasionally get a solid pillar; otherwise use a boulder for movable cover.
        TileType cover = TileType::Boulder;
        if (depth >= 6 && rng.chance(0.25f)) cover = TileType::Pillar;

        forceSetTileFeature(d, x, y, cover, changes);
        if (changes.empty()) return false;

        if (!stairsConnected(d)) {
            undoChanges(d, changes);
            return false;
        }

        d.fireLaneCoverCount += 1;
        return true;
    };

    auto tryBreakLane = [&](const FireLaneInfo& lane) -> bool {
        if (lane.len <= 0) return false;

        // Search near the lane midpoint (a small symmetric sweep) for a good pivot.
        const int mid = lane.len / 2;
        const int maxSweep = std::min(8, std::max(2, lane.len / 4));

        for (int step = 0; step <= maxSweep; ++step) {
            const int candOff[2] = { step, -step };
            for (int k = 0; k < 2; ++k) {
                const int off = candOff[k];
                const int t = mid + off;
                if (t < 3 || t >= lane.len - 3) continue;

                const int x = lane.sx + lane.dx * t;
                const int y = lane.sy + lane.dy * t;

                if (!isOkBaseTile(x, y)) continue;

                // Prefer chicanes (they preserve movement flow), then fall back to open-area cover.
                if (tryChicaneAt(x, y, lane.dx, lane.dy)) return true;
                if (tryDropCoverAt(x, y)) return true;
            }
        }

        return false;
    };

    int ops = 0;
    while (ops < maxOps) {
        const FireLaneInfo lane = findLongestFireLane(d);
        if (lane.len <= target) break;

        const bool ok = tryBreakLane(lane);
        if (!ok) break;
        ops += 1;
    }

    d.fireLaneMaxAfter = computeMaxFireLane(d);
    return d.fireLaneCoverCount > 0;
}


// ------------------------------------------------------------
// Moated room setpieces (special-room islands)
//
// Carves a 1-tile chasm ring *inside* select special rooms (Treasure/Shrine),
// leaving a central floor "island" reached via 1-2 narrow bridges.
//
// This creates a clean landmark and interesting micro-chokepoints without
// affecting global stairs connectivity (rolled back if it ever does).
// ------------------------------------------------------------
static bool applyMoatedRoomSetpieces(Dungeon& d, RNG& rng, int depth) {
    d.moatedRoomCount = 0;
    d.moatedRoomBridgeCount = 0;
    d.moatedRoomChasmCount = 0;

    if (d.rooms.empty()) return false;

    struct Cand {
        int idx = -1;
        int pri = 0;
        int area = 0;
    };

    std::vector<Cand> cands;
    cands.reserve(d.rooms.size());

    auto isCandidateType = [&](RoomType t) -> bool {
        return (t == RoomType::Treasure) || (t == RoomType::Shrine);
    };

    for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
        const Room& r = d.rooms[static_cast<size_t>(i)];
        if (!isCandidateType(r.type)) continue;

        // Avoid the stair rooms entirely.
        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;

        // Need enough space for: 2-tile outer walking band + 1-tile moat + 2x2 island.
        if (r.w < 8 || r.h < 8) continue;

        const int pri = (r.type == RoomType::Treasure) ? 0 : 1;
        cands.push_back({i, pri, r.w * r.h});
    }

    if (cands.empty()) return false;

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.pri != b.pri) return a.pri < b.pri;
        return a.area > b.area;
    });

    const int maxRooms = (depth >= 10 && rng.chance(0.25f)) ? 2 : 1;

    auto clampiLocal = [](int v, int lo, int hi) -> int {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    };

    enum class Side { North, South, West, East };

    auto opposite = [](Side s) -> Side {
        switch (s) {
            case Side::North: return Side::South;
            case Side::South: return Side::North;
            case Side::West:  return Side::East;
            case Side::East:  return Side::West;
        }
        return Side::North;
    };

    auto sideFromPos = [&](const Room& r, Vec2i p) -> Side {
        const int left = std::abs(p.x - r.x);
        const int right = std::abs((r.x + r.w - 1) - p.x);
        const int top = std::abs(p.y - r.y);
        const int bottom = std::abs((r.y + r.h - 1) - p.y);
        const int m = std::min(std::min(left, right), std::min(top, bottom));

        if (m == left) return Side::West;
        if (m == right) return Side::East;
        if (m == top) return Side::North;
        return Side::South;
    };

    auto sideTowardStairs = [&](const Room& r) -> Side {
        const int cx = r.cx();
        const int cy = r.cy();
        const int dx = d.stairsUp.x - cx;
        const int dy = d.stairsUp.y - cy;
        if (std::abs(dx) >= std::abs(dy)) return (dx < 0) ? Side::West : Side::East;
        return (dy < 0) ? Side::North : Side::South;
    };

    auto carveOne = [&](const Room& r) -> bool {
        // Per-type probability (flavorful but not every floor).
        float p = (r.type == RoomType::Treasure)
                    ? (0.55f + 0.03f * static_cast<float>(std::clamp(depth - 2, 0, 10)))
                    : 0.40f;
        p = std::min(0.85f, p);
        if (!rng.chance(p)) return false;

        const int outer = 2;
        const int x0 = r.x + outer;
        const int y0 = r.y + outer;
        const int x1 = r.x + r.w - outer - 1;
        const int y1 = r.y + r.h - outer - 1;

        // Need at least a 1-tile ring and a 2x2 island.
        if (x1 - x0 < 3 || y1 - y0 < 3) return false;

        const int ix0 = x0 + 1;
        const int iy0 = y0 + 1;
        const int ix1 = x1 - 1;
        const int iy1 = y1 - 1;

        std::vector<Tile> before = d.tiles;

        // 1) Clear the room interior to plain floor (except doors/stairs).
        for (int y = r.y; y < r.y + r.h; ++y) {
            for (int x = r.x; x < r.x + r.w; ++x) {
                if (!d.inBounds(x, y)) continue;
                if (isStairsTile(d, x, y)) continue;

                TileType& tt = d.at(x, y).type;
                if (isDoorTileType(tt)) continue;
                tt = TileType::Floor;
            }
        }

        // 2) Carve the moat ring.
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (!(x == x0 || x == x1 || y == y0 || y == y1)) continue;
                if (!d.inBounds(x, y)) continue;
                if (isStairsTile(d, x, y)) continue;

                TileType& tt = d.at(x, y).type;
                if (isDoorTileType(tt)) continue;
                tt = TileType::Chasm;
            }
        }

        // 3) Ensure island interior is floor.
        for (int y = iy0; y <= iy1; ++y) {
            for (int x = ix0; x <= ix1; ++x) {
                if (!d.inBounds(x, y)) continue;
                if (isStairsTile(d, x, y)) continue;

                TileType& tt = d.at(x, y).type;
                if (isDoorTileType(tt)) continue;
                tt = TileType::Floor;
            }
        }

        // Orient the main bridge toward the first door (if any).
        std::vector<Vec2i> doorTiles;
        doorTiles.reserve(4);
        for (int y = r.y; y < r.y + r.h; ++y) {
            for (int x = r.x; x < r.x + r.w; ++x) {
                if (!d.inBounds(x, y)) continue;
                const TileType tt = d.at(x, y).type;
                if (!isDoorTileType(tt)) continue;

                const bool border = (x == r.x || x == r.x + r.w - 1 || y == r.y || y == r.y + r.h - 1);
                if (border) doorTiles.push_back({x, y});
            }
        }

        Side mainSide = !doorTiles.empty() ? sideFromPos(r, doorTiles[0]) : sideTowardStairs(r);

        auto placeBridge = [&](Side s, int coord) -> bool {
            Vec2i p{-1, -1};
            if (s == Side::West) {
                const int yy = clampiLocal(coord, iy0, iy1);
                p = {x0, yy};
            } else if (s == Side::East) {
                const int yy = clampiLocal(coord, iy0, iy1);
                p = {x1, yy};
            } else if (s == Side::North) {
                const int xx = clampiLocal(coord, ix0, ix1);
                p = {xx, y0};
            } else { // South
                const int xx = clampiLocal(coord, ix0, ix1);
                p = {xx, y1};
            }

            if (!d.inBounds(p.x, p.y)) return false;
            if (d.at(p.x, p.y).type != TileType::Chasm) return false;
            d.at(p.x, p.y).type = TileType::Floor;
            return true;
        };

        int bridges = 0;

        // Main bridge aligned with first door when possible.
        if (!doorTiles.empty()) {
            if (mainSide == Side::West || mainSide == Side::East) {
                if (placeBridge(mainSide, doorTiles[0].y)) bridges++;
            } else {
                if (placeBridge(mainSide, doorTiles[0].x)) bridges++;
            }
        }

        // Always ensure at least one bridge, even if the first attempt failed.
        if (bridges <= 0) {
            bool ok = false;
            if (mainSide == Side::West || mainSide == Side::East) {
                ok = placeBridge(mainSide, (iy0 + iy1) / 2);
            } else {
                ok = placeBridge(mainSide, (ix0 + ix1) / 2);
            }

            if (!ok) {
                d.tiles = std::move(before);
                return false;
            }

            bridges = 1;
        }

        // Optional second bridge (opposite side) if the island is big enough.
        const int islandW = ix1 - ix0 + 1;
        const int islandH = iy1 - iy0 + 1;
        if (islandW >= 4 && islandH >= 4) {
            float p2 = (r.type == RoomType::Treasure) ? 0.45f : 0.30f;
            if (depth >= 8) p2 += 0.10f;
            p2 = std::min(0.70f, p2);

            if (rng.chance(p2)) {
                const Side s2 = opposite(mainSide);
                if (s2 == Side::West || s2 == Side::East) {
                    const int yy = (iy0 + iy1) / 2;
                    if (placeBridge(s2, yy)) bridges++;
                } else {
                    const int xx = (ix0 + ix1) / 2;
                    if (placeBridge(s2, xx)) bridges++;
                }
            }
        }

        // Count remaining chasm tiles in the ring for stats.
        int ringChasm = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (!(x == x0 || x == x1 || y == y0 || y == y1)) continue;
                if (!d.inBounds(x, y)) continue;
                if (d.at(x, y).type == TileType::Chasm) ringChasm++;
            }
        }

        // Safety: never allow this setpiece to break the critical stairs path.
        if (!stairsConnected(d)) {
            d.tiles = std::move(before);
            return false;
        }

        d.moatedRoomCount += 1;
        d.moatedRoomBridgeCount += bridges;
        d.moatedRoomChasmCount += ringChasm;
        return true;
    };

    int placed = 0;
    for (const Cand& c : cands) {
        if (placed >= maxRooms) break;
        const Room& r = d.rooms[static_cast<size_t>(c.idx)];
        if (carveOne(r)) placed++;
    }

    return d.moatedRoomCount > 0;
}




// ------------------------------------------------------------
// Rift cache pockets (boulder-bridge micro puzzles)
//
// Carves a small wall pocket off an ordinary room, gated by:
//   - a pushable BOULDER placed on a 1-tile "approach lane" outside the room
//   - a 1-tile CHASM gap immediately beyond the boulder
//
// Pushing the boulder into the gap creates a rough bridge, revealing a
// guaranteed bonus chest spawn requested via Dungeon::bonusLootSpots.
//
// This yields tiny, readable "physics puzzles" that are always optional side
// objectives (never required for stair connectivity).
// ------------------------------------------------------------
static bool maybeCarveRiftCachePockets(Dungeon& d, RNG& rng, int depth, GenKind g) {
    d.riftCacheCount = 0;
    d.riftCacheBoulderCount = 0;
    d.riftCacheChasmCount = 0;

    if (d.rooms.empty()) return false;
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return false;

    // Base chance: shallow floors rarely get them; deeper floors get a bit more spice.
    float p = 0.10f + 0.02f * static_cast<float>(std::min(depth, 10));
    if (g == GenKind::Cavern) p *= 0.55f;
    else if (g == GenKind::Maze) p *= 0.60f;
    else if (g == GenKind::Warrens) p *= 0.75f;
    p = std::clamp(p, 0.0f, 0.35f);

    int target = 0;
    if (rng.chance(p)) target = 1;
    if (depth >= 6 && rng.chance(p * 0.33f)) target += 1;
    if (target <= 0) return false;

    // Candidate rooms: plain, medium+ sized, and not containing stairs.
    std::vector<int> rooms;
    rooms.reserve(d.rooms.size());
    for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
        const Room& r = d.rooms[static_cast<size_t>(i)];
        if (r.type != RoomType::Normal) continue;
        if (r.w < 7 || r.h < 7) continue;
        if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
        if (r.contains(d.stairsDown.x, d.stairsDown.y)) continue;
        rooms.push_back(i);
    }
    if (rooms.empty()) return false;

    const auto dist = bfsDistanceMap(d, d.stairsUp);
    auto distAt = [&](Vec2i p) -> int {
        if (!d.inBounds(p.x, p.y)) return -1;
        const size_t ii = static_cast<size_t>(p.y * d.width + p.x);
        if (ii >= dist.size()) return -1;
        return dist[ii];
    };

    auto isPlainFloor = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (isStairsTile(d, x, y)) return false;
        return d.at(x, y).type == TileType::Floor;
    };

    auto doorNear = [&](Vec2i p, int cheb) -> bool {
        for (int dy = -cheb; dy <= cheb; ++dy) {
            for (int dx = -cheb; dx <= cheb; ++dx) {
                const int x = p.x + dx;
                const int y = p.y + dy;
                if (!d.inBounds(x, y)) continue;
                if (isDoorTileType(d.at(x, y).type)) return true;
            }
        }
        return false;
    };

    auto rectAllWall = [&](int x0, int y0, int w, int h) -> bool {
        if (w <= 0 || h <= 0) return false;
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                if (!d.inBounds(x, y)) return false;
                if (d.at(x, y).type != TileType::Wall) return false;
            }
        }
        return true;
    };

    auto carvePocketFloor = [&](int x0, int y0, int w, int h) {
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                if (!d.inBounds(x, y)) continue;
                d.at(x, y).type = TileType::Floor;
            }
        }
    };

    auto pickPocketLoot = [&](int x0, int y0, int w, int h, Vec2i entryFar) -> Vec2i {
        // Prefer an inner-corner away from the entry (keeps chest out of the approach).
        const int ix0 = x0 + 1;
        const int iy0 = y0 + 1;
        const int ix1 = x0 + w - 2;
        const int iy1 = y0 + h - 2;

        Vec2i cands[4] = {
            {ix0, iy0},
            {ix1, iy0},
            {ix0, iy1},
            {ix1, iy1},
        };

        int bestD = -1;
        Vec2i best = cands[0];

        // Pick a farthest corner; break ties randomly for variety.
        for (int i = 0; i < 4; ++i) {
            const Vec2i p = cands[i];
            if (!d.inBounds(p.x, p.y)) continue;
            if (d.at(p.x, p.y).type != TileType::Floor) continue;
            const int md = manhattan2(p, entryFar);
            if (md > bestD || (md == bestD && rng.chance(0.5f))) {
                bestD = md;
                best = p;
            }
        }
        return best;
    };

    struct Side { int dx; int dy; };
    static const Side kSides[4] = {{+1,0},{-1,0},{0,+1},{0,-1}};

    int placed = 0;
    const int maxAttempts = target * 220;

    for (int attempt = 0; attempt < maxAttempts && placed < target; ++attempt) {
        const Room& r = d.rooms[static_cast<size_t>(rooms[static_cast<size_t>(rng.range(0, static_cast<int>(rooms.size()) - 1))])];

        const Side s = kSides[rng.range(0, 3)];
        const int dx = s.dx;
        const int dy = s.dy;

        // Pick a room-edge floor tile (avoids corners).
        Vec2i roomEdge{-1, -1};
        if (dx > 0) {
            roomEdge.x = r.x2() - 1;
            roomEdge.y = rng.range(r.y + 1, r.y2() - 2);
        } else if (dx < 0) {
            roomEdge.x = r.x;
            roomEdge.y = rng.range(r.y + 1, r.y2() - 2);
        } else if (dy > 0) {
            roomEdge.y = r.y2() - 1;
            roomEdge.x = rng.range(r.x + 1, r.x2() - 2);
        } else {
            roomEdge.y = r.y;
            roomEdge.x = rng.range(r.x + 1, r.x2() - 2);
        }

        if (!isPlainFloor(roomEdge.x, roomEdge.y)) continue;
        if (doorNear(roomEdge, 1)) continue;

        // Ensure the approach tile is reachable from the start.
        if (distAt(roomEdge) < 0) continue;

        // Keep these away from the immediate start area so they read as "optional detours".
        if (manhattan2(roomEdge, d.stairsUp) <= 8) continue;

        // The micro-puzzle geometry (from the room outward):
        //   roomEdge (floor) -> lane (boulder) -> gap (chasm) -> pocket (floor)
        const Vec2i lane{roomEdge.x + dx, roomEdge.y + dy};
        const Vec2i gap{roomEdge.x + 2 * dx, roomEdge.y + 2 * dy};
        const Vec2i entryFar{roomEdge.x + 3 * dx, roomEdge.y + 3 * dy};

        if (!d.inBounds(lane.x, lane.y) || !d.inBounds(gap.x, gap.y) || !d.inBounds(entryFar.x, entryFar.y)) continue;

        // Must be solid rock outside the room so we aren't cutting into existing corridors.
        if (d.at(lane.x, lane.y).type != TileType::Wall) continue;
        if (d.at(gap.x, gap.y).type != TileType::Wall) continue;

        // Pocket size (kept small so it doesn't distort overall density/scoring).
        const int pw = rng.range(4, 6);
        const int ph = rng.range(4, 6);

        int x0 = 0, y0 = 0;
        if (dx != 0) {
            // Horizontal pocket: extend outward in X, center in Y around entryFar.
            y0 = entryFar.y - ph / 2;
            if (dx > 0) {
                x0 = entryFar.x;
            } else {
                x0 = entryFar.x - pw + 1;
            }
        } else {
            // Vertical pocket: extend outward in Y, center in X around entryFar.
            x0 = entryFar.x - pw / 2;
            if (dy > 0) {
                y0 = entryFar.y;
            } else {
                y0 = entryFar.y - ph + 1;
            }
        }

        // Keep a small safety margin from outer borders so ensureBorders() doesn't overwrite.
        if (x0 < 2 || y0 < 2) continue;
        if (x0 + pw >= d.width - 2) continue;
        if (y0 + ph >= d.height - 2) continue;

        // The pocket must be carved entirely from solid wall.
        if (!rectAllWall(x0, y0, pw, ph)) continue;

        // Ensure our entry tile is inside the pocket (should always be, but be defensive).
        if (!(entryFar.x >= x0 && entryFar.x < x0 + pw && entryFar.y >= y0 && entryFar.y < y0 + ph)) continue;

        // Carve the pocket and place the puzzle pieces.
        carvePocketFloor(x0, y0, pw, ph);
        d.at(gap.x, gap.y).type = TileType::Chasm;
        d.at(lane.x, lane.y).type = TileType::Boulder;

        // Request a bonus chest inside the pocket.
        const Vec2i loot = pickPocketLoot(x0, y0, pw, ph, entryFar);
        if (d.inBounds(loot.x, loot.y) && d.at(loot.x, loot.y).type == TileType::Floor) {
            d.bonusLootSpots.push_back(loot);
        }

        // Rare deeper-floor variant: a second cache (keeps risk/reward spicy).
        if (depth >= 8 && rng.chance(0.18f)) {
            Vec2i loot2{-1, -1};
            // Try a few times to find a different floor tile far from the first.
	            for (int t = 0; t < 24; ++t) {
	                Vec2i pos{rng.range(x0 + 1, x0 + pw - 2), rng.range(y0 + 1, y0 + ph - 2)};
	                if (!d.inBounds(pos.x, pos.y)) continue;
	                if (d.at(pos.x, pos.y).type != TileType::Floor) continue;
	                if (manhattan2(pos, loot) < 4) continue;
	                loot2 = pos;
                break;
            }
            if (d.inBounds(loot2.x, loot2.y) && d.at(loot2.x, loot2.y).type == TileType::Floor) {
                d.bonusLootSpots.push_back(loot2);
            }
        }

        d.riftCacheCount += 1;
        d.riftCacheBoulderCount += 1;
        d.riftCacheChasmCount += 1;

        placed += 1;
    }

    return d.riftCacheCount > 0;
}

static int countDeadEnds(const Dungeon& d) {
    int dead = 0;
    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            if (!d.isPassable(x, y)) continue;

            int n = 0;
            for (const auto& dv : dirs) {
                const int nx = x + dv[0];
                const int ny = y + dv[1];
                if (!d.inBounds(nx, ny)) continue;
                if (d.isPassable(nx, ny)) ++n;
            }

            if (n <= 1) ++dead;
        }
    }

    return dead;
}


// -----------------------------------------------------------------------------
// Macro terrain: warped fBm heightfield
//
// This is a late post-pass that does NOT alter the fundamental connectivity of the floor.
// Instead, it uses a deterministic "geology" field (height + ridges + slope) to place
// coherent pillar spines (ridges) and small boulder scree clusters (steep slopes).
//
// Safety rules:
// - Never touches stairs tiles or their landing pads
// - Never touches the shortest stairs path "halo"
// - Avoids doors and special rooms
// - Rolls back any placement group that would disconnect stairs
// -----------------------------------------------------------------------------

inline float hfSmooth3(float t) { return t * t * (3.0f - 2.0f * t); }
inline float hfLerp(float a, float b, float t) { return a + (b - a) * t; }

inline uint32_t hfHash2(uint32_t seed, int x, int y) {
    // hashCombine is stable and cheap; casting negatives to uint32_t is fine (wraparound)
    // and still produces deterministic variation.
    const uint32_t hx = hashCombine(seed ^ 0xB5297A4Du, static_cast<uint32_t>(x));
    return hash32(hashCombine(hx, static_cast<uint32_t>(y)));
}

float hfValueNoise(uint32_t seed, float x, float y) {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi);
    const float fy = y - static_cast<float>(yi);

    const float u = hfSmooth3(fx);
    const float v = hfSmooth3(fy);

    const float v00 = rand01(hfHash2(seed, xi, yi));
    const float v10 = rand01(hfHash2(seed, xi + 1, yi));
    const float v01 = rand01(hfHash2(seed, xi, yi + 1));
    const float v11 = rand01(hfHash2(seed, xi + 1, yi + 1));

    const float a = hfLerp(v00, v10, u);
    const float b = hfLerp(v01, v11, u);
    return hfLerp(a, b, v);
}

// Returns fBm in [-1, 1].
float hfFbm(uint32_t seed, float x, float y, int octaves) {
    float sum = 0.0f;
    float amp = 1.0f;
    float norm = 0.0f;
    float freq = 1.0f;

    octaves = std::clamp(octaves, 1, 8);

    for (int i = 0; i < octaves; ++i) {
        const uint32_t s = seed ^ hash32(static_cast<uint32_t>(i) * 0x9E3779B9u);
        const float n01 = hfValueNoise(s, x * freq, y * freq);
        const float n = n01 * 2.0f - 1.0f;
        sum += n * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }

    if (norm <= 0.0f) return 0.0f;
    return sum / norm;
}

void hfDomainWarp(uint32_t seed, float x, float y, float& outX, float& outY) {
    // Low-frequency warp field; keeps features organic and less grid-aligned.
    const float warpFreq = 0.85f;
    const float warpAmp = 0.60f;

    const float wx = hfFbm(seed ^ 0xA17D2C3Bu, x * warpFreq, y * warpFreq, 3);
    const float wy = hfFbm(seed ^ 0xC0FFEE11u, x * warpFreq, y * warpFreq, 3);

    outX = x + wx * warpAmp;
    outY = y + wy * warpAmp;
}

float hfHeight01(uint32_t seed, float nx, float ny) {
    // nx,ny expected in [0,1]. Scale picks the "macro feature" size.
    const float scale = 3.35f;

    float x = nx * scale;
    float y = ny * scale;

    float wx = x;
    float wy = y;
    hfDomainWarp(seed, x, y, wx, wy);

    const float n = hfFbm(seed ^ 0x51F15EEDu, wx, wy, 5);
    return std::clamp(0.5f + 0.5f * n, 0.0f, 1.0f);
}

float hfRidge01(uint32_t seed, float nx, float ny) {
    const float scale = 3.35f;

    float x = nx * scale;
    float y = ny * scale;

    float wx = x;
    float wy = y;
    hfDomainWarp(seed ^ 0x9E3779B9u, x, y, wx, wy);

    const float n = hfFbm(seed ^ 0xD00DFEEDu, wx, wy, 4);
    const float n01 = std::clamp(0.5f + 0.5f * n, 0.0f, 1.0f);

    float ridge = 1.0f - std::fabs(n01 * 2.0f - 1.0f); // 0..1, peaks at mid-values
    ridge = ridge * ridge; // sharpen
    return std::clamp(ridge, 0.0f, 1.0f);
}

uint32_t heightfieldSeedKey(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth, const EndlessStratumInfo& st) {
    uint32_t k = hashCombine(worldSeed ^ 0xA8173D55u, static_cast<uint32_t>(branch));
    k = hashCombine(k, hashCombine(static_cast<uint32_t>(depth), static_cast<uint32_t>(maxDepth)));
    if (st.seed != 0u) {
        // Align macro terrain to Infinite World strata bands when applicable.
        k = hashCombine(k, st.seed ^ 0x51A7D00Du);
    }
    k = hash32(k);
    return k ? k : 1u;
}

void hfMarkDisk(std::vector<uint8_t>& mask, int w, int h, int cx, int cy, int r) {
    r = std::max(0, r);
    const int r2 = r * r;
    for (int oy = -r; oy <= r; ++oy) {
        for (int ox = -r; ox <= r; ++ox) {
            if (ox * ox + oy * oy > r2) continue;
            const int x = cx + ox;
            const int y = cy + oy;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            mask[static_cast<size_t>(y * w + x)] = 1u;
        }
    }
}

void hfReserveShortestStairsHalo(const Dungeon& d, std::vector<uint8_t>& mask, int haloR) {
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return;
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) return;

    const auto dist = bfsDistanceMap(d, d.stairsUp);
    const auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    const size_t endIdx = idx(d.stairsDown.x, d.stairsDown.y);
    if (endIdx >= dist.size()) return;
    int cd = dist[endIdx];
    if (cd < 0) return;

    Vec2i cur = d.stairsDown;
    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    // Walk "downhill" in the distance field to reconstruct one shortest path.
    for (int guard = 0; guard < d.width * d.height + 8; ++guard) {
        hfMarkDisk(mask, d.width, d.height, cur.x, cur.y, haloR);
        if (cur.x == d.stairsUp.x && cur.y == d.stairsUp.y) break;

        bool stepped = false;
        for (auto& dv : dirs) {
            const int nx = cur.x + dv[0];
            const int ny = cur.y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            const size_t ni = idx(nx, ny);
            if (ni >= dist.size()) continue;
            if (dist[ni] == cd - 1) {
                cur = {nx, ny};
                cd = dist[ni];
                stepped = true;
                break;
            }
        }

        if (!stepped) break;
        if (cd <= 0) break;
    }
}

int passableCardinalNeighbors(const Dungeon& d, int x, int y) {
    static const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    int n = 0;
    for (auto& dv : dirs) {
        const int nx = x + dv[0];
        const int ny = y + dv[1];
        if (!d.inBounds(nx, ny)) continue;
        if (d.isPassable(nx, ny)) ++n;
    }
    return n;
}

bool hfIsLocalMax(const std::vector<float>& field, int w, int h, int x, int y) {
    const float v = field[static_cast<size_t>(y * w + x)];
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int nx = x + ox;
            const int ny = y + oy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const float nv = field[static_cast<size_t>(ny * w + nx)];
            if (nv > v) return false;
        }
    }
    return true;
}

void applyHeightfieldTerrain(Dungeon& d, DungeonBranch branch, int depth, int maxDepth, GenKind g, uint32_t worldSeed, const EndlessStratumInfo& st) {
    d.heightfieldRidgePillarCount = 0;
    d.heightfieldScreeBoulderCount = 0;

    if (branch != DungeonBranch::Main) return;
    if (d.width <= 0 || d.height <= 0) return;

    const int W = d.width;
    const int H = d.height;
    const int N = W * H;

    const uint32_t seed = heightfieldSeedKey(worldSeed, branch, depth, maxDepth, st);

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    // Precompute height + ridge fields.
    std::vector<float> height(static_cast<size_t>(N), 0.0f);
    std::vector<float> ridge(static_cast<size_t>(N), 0.0f);

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(W);
            const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(H);
            height[idx(x, y)] = hfHeight01(seed, nx, ny);
            ridge[idx(x, y)] = hfRidge01(seed, nx, ny);
        }
    }

    // Approximate slope magnitude from the height field.
    std::vector<float> slope(static_cast<size_t>(N), 0.0f);
    auto hAt = [&](int x, int y) -> float {
        x = std::clamp(x, 0, W - 1);
        y = std::clamp(y, 0, H - 1);
        return height[idx(x, y)];
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float dx = hAt(x + 1, y) - hAt(x - 1, y);
            const float dy = hAt(x, y + 1) - hAt(x, y - 1);
            slope[idx(x, y)] = std::sqrt(dx * dx + dy * dy);
        }
    }

    // Reserved mask (1 = do not modify).
    std::vector<uint8_t> reserved(static_cast<size_t>(N), uint8_t{0});

    // Stairs halos: keep landings readable and avoid cheap stair traps.
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) hfMarkDisk(reserved, W, H, d.stairsUp.x, d.stairsUp.y, 2);
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) hfMarkDisk(reserved, W, H, d.stairsDown.x, d.stairsDown.y, 2);

    // Shortest-path halo between stairs (keeps at least one clean "spine").
    hfReserveShortestStairsHalo(d, reserved, 1);

    // Doors (and near-doors) are always left unobstructed.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (isDoorTileType(d.at(x, y).type)) {
                hfMarkDisk(reserved, W, H, x, y, 1);
            }
        }
    }

    // Avoid special rooms entirely.
    for (const Room& rr : d.rooms) {
        if (rr.type == RoomType::Normal) continue;

        // Expand by 1 to avoid placing right on special-room borders.
        const int x0 = std::max(0, rr.x - 1);
        const int y0 = std::max(0, rr.y - 1);
        const int x1 = std::min(W - 1, rr.x2());
        const int y1 = std::min(H - 1, rr.y2());

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                reserved[idx(x, y)] = 1u;
            }
        }
    }

    // Intensity tuning by generator kind.
    float styleMul = 1.0f;
    switch (g) {
        case GenKind::Cavern:     styleMul = 1.30f; break;
        case GenKind::Warrens:    styleMul = 1.15f; break;
        case GenKind::Maze:       styleMul = 0.85f; break;
        case GenKind::RoomsGraph: styleMul = 0.85f; break;
        case GenKind::RoomsBsp:   styleMul = 0.80f; break;
        case GenKind::Catacombs:  styleMul = 0.90f; break;
        case GenKind::Mines:      styleMul = 0.95f; break;
        default:                 styleMul = 1.0f; break;
    }

    const float depth01 = (maxDepth > 0)
        ? std::clamp(static_cast<float>(std::min(depth, maxDepth)) / static_cast<float>(maxDepth), 0.0f, 1.0f)
        : 0.0f;

    // Use passable area rather than total area so caverns (high open area) naturally get more features.
    int passable = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) if (d.isPassable(x, y)) ++passable;

    const float areaF = static_cast<float>(std::max(1, passable));

    const int ridgeTarget = static_cast<int>(std::clamp(styleMul * (areaF / 430.0f) * (0.75f + 0.65f * depth01) + 6.0f, 6.0f, 48.0f));
    const int screeTarget = static_cast<int>(std::clamp(styleMul * (areaF / 320.0f) * (0.70f + 0.75f * depth01) + 8.0f, 8.0f, 80.0f));

    // -------------------------------------------------------------------------
    // Ridge pillars: place on ridge maxima with spacing, rollback on failure.
    // -------------------------------------------------------------------------
    struct Cand { int x; int y; float v; };
    std::vector<Cand> ridgeCands;
    ridgeCands.reserve(static_cast<size_t>(N / 4));

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            const size_t ii = idx(x, y);
            if (reserved[ii]) continue;
            if (d.at(x, y).type != TileType::Floor) continue;
            const float rv = ridge[ii];
            if (rv < 0.55f) continue;
            if (!hfIsLocalMax(ridge, W, H, x, y)) continue;
            if (passableCardinalNeighbors(d, x, y) < 3) continue; // avoid narrow corridors
            ridgeCands.push_back({x, y, rv});
        }
    }

    std::sort(ridgeCands.begin(), ridgeCands.end(), [](const Cand& a, const Cand& b) { return a.v > b.v; });

    const int minSep = (g == GenKind::Cavern) ? 4 : 3;

    SpatialHashGrid2D pillarGrid(W, H, minSep);

    int placedRidge = 0;
    for (const Cand& c : ridgeCands) {
        if (placedRidge >= ridgeTarget) break;

        const Vec2i pos{c.x, c.y};
        if (pillarGrid.anyWithinRadius(pos, minSep)) continue;

        Tile& t = d.at(pos.x, pos.y);
        if (t.type != TileType::Floor) continue;

        t.type = TileType::Pillar;
        if (!stairsConnected(d)) {
            t.type = TileType::Floor;
            continue;
        }

        pillarGrid.insert(pos);
        placedRidge += 1;
    }

    d.heightfieldRidgePillarCount = placedRidge;

    // -------------------------------------------------------------------------
    // Scree boulders: clustered boulders on steep slopes.
    // Rollback whole clusters on failure.
    // -------------------------------------------------------------------------
    struct Seed { int x; int y; float s; };
    std::vector<Seed> slopeSeeds;
    slopeSeeds.reserve(static_cast<size_t>(N / 3));

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            const size_t ii = idx(x, y);
            if (reserved[ii]) continue;
            if (d.at(x, y).type != TileType::Floor) continue;

            const float sv = slope[ii];
            if (sv < 0.08f) continue;
            if (passableCardinalNeighbors(d, x, y) < 3) continue;
            slopeSeeds.push_back({x, y, sv});
        }
    }

    std::sort(slopeSeeds.begin(), slopeSeeds.end(), [](const Seed& a, const Seed& b) { return a.s > b.s; });

    const int clustersTarget = std::clamp(screeTarget / 7, 2, 10);
    const int clusterSep = 8;

    // Precompute offsets in a radius-3 disk (excluding center), sorted by distance for natural clusters.
    std::vector<Vec2i> offsets;
    offsets.reserve(64);
    const int R = 3;
    for (int oy = -R; oy <= R; ++oy) {
        for (int ox = -R; ox <= R; ++ox) {
            if (ox == 0 && oy == 0) continue;
            if (ox * ox + oy * oy > R * R) continue;
            offsets.push_back({ox, oy});
        }
    }
    std::sort(offsets.begin(), offsets.end(), [](const Vec2i& a, const Vec2i& b) {
        const int da = a.x * a.x + a.y * a.y;
        const int db = b.x * b.x + b.y * b.y;
        if (da != db) return da < db;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });

    SpatialHashGrid2D clusterGrid(W, H, clusterSep);

    int clustersPlaced = 0;
    int bouldersPlaced = 0;

    for (const Seed& s : slopeSeeds) {
        if (clustersPlaced >= clustersTarget) break;
        if (bouldersPlaced >= screeTarget) break;

        const Vec2i center{s.x, s.y};
        if (clusterGrid.anyWithinRadius(center, clusterSep)) continue;

        // Deterministic "cluster personality".
        const uint32_t sh = hash32(hashCombine(seed, hashCombine(static_cast<uint32_t>(s.x), static_cast<uint32_t>(s.y))));
        int want = 3 + static_cast<int>(sh % 5u); // 3..7
        // Slightly larger clusters on steep seeds.
        if (s.s > 0.14f) want += 2;
        want = std::clamp(want, 3, 10);

        // Order offsets by a per-cluster hash so clusters look less uniform.
        struct Off { uint32_t k; Vec2i o; };
        std::vector<Off> ordered;
        ordered.reserve(offsets.size());
        for (size_t i = 0; i < offsets.size(); ++i) {
            const uint32_t k = hash32(hashCombine(sh, static_cast<uint32_t>(i)));
            ordered.push_back({k, offsets[i]});
        }
        std::sort(ordered.begin(), ordered.end(), [](const Off& a, const Off& b) { return a.k < b.k; });

        std::vector<Vec2i> changed;
        changed.reserve(static_cast<size_t>(want));

        for (const Off& of : ordered) {
            if (static_cast<int>(changed.size()) >= want) break;
            if (bouldersPlaced + static_cast<int>(changed.size()) >= screeTarget) break;

            const int x = s.x + of.o.x;
            const int y = s.y + of.o.y;
            if (!d.inBounds(x, y)) continue;

            const size_t ii = idx(x, y);
            if (reserved[ii]) continue;

            Tile& t = d.at(x, y);
            if (t.type != TileType::Floor) continue;

            // Keep the cluster on the same "slope band".
            if (slope[ii] < s.s * 0.55f) continue;

            if (passableCardinalNeighbors(d, x, y) < 3) continue;

            // Prevent excessive clumping: allow at most a couple adjacent boulders.
            int near = 0;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    if (ox == 0 && oy == 0) continue;
                    const int nx = x + ox;
                    const int ny = y + oy;
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type == TileType::Boulder) near += 1;
                }
            }
            if (near >= 3) continue;

            t.type = TileType::Boulder;
            changed.push_back({x, y});
        }

        if (changed.empty()) continue;

        if (!stairsConnected(d)) {
            // Rollback this cluster.
            for (const Vec2i& p : changed) {
                if (d.inBounds(p.x, p.y) && d.at(p.x, p.y).type == TileType::Boulder) {
                    d.at(p.x, p.y).type = TileType::Floor;
                }
            }
            continue;
        }

        // Commit cluster.
        clusterGrid.insert(center);
        clustersPlaced += 1;
        bouldersPlaced += static_cast<int>(changed.size());
    }

    d.heightfieldScreeBoulderCount = bouldersPlaced;
}


// -------------------------------------------------------------------------
// Fluvial erosion gullies (drainage network over the macro heightfield)
//
// This pass carves a few narrow Chasm "gullies" that roughly follow a
// deterministic drainage field derived from the same macro heightfield used by
// applyHeightfieldTerrain(). Unlike the run faultline / ravine seams, gullies
// form shorter dendritic cuts that feel like local erosion.
// Safety rules:
// - Never touch stairs or doors.
// - Reserve a small halo around the shortest stairs path (keeps a clean spine).
// - Avoid special rooms entirely.
// - Always preserve (or repair) stairs connectivity; rollback on failure.
// -------------------------------------------------------------------------

static uint32_t fluvialSeedKey(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth, const EndlessStratumInfo& st) {
    uint32_t k = hashCombine(worldSeed ^ 0xF17A51A1u, static_cast<uint32_t>(branch));
    k = hashCombine(k, static_cast<uint32_t>(depth));
    k = hashCombine(k, static_cast<uint32_t>(maxDepth));
    if (st.index >= 0) k = hashCombine(k, st.seed ^ 0xA11A57A1u);
    k = hash32(k);
    if (k == 0u) k = 1u;
    return k;
}

static void applyFluvialGullies(Dungeon& d, DungeonBranch branch, int depth, int maxDepth, GenKind g, uint32_t worldSeed, const EndlessStratumInfo& st) {
    d.fluvialGullyCount = 0;
    d.fluvialChasmCount = 0;
    d.fluvialCausewayCount = 0;

    if (branch != DungeonBranch::Main) return;
    if (d.width < 22 || d.height < 16) return;

    const int W = d.width;
    const int H = d.height;
    const int N = W * H;

    // Deterministic per-run/per-depth RNG independent of candidate RNG.
    const uint32_t seed = fluvialSeedKey(worldSeed, branch, depth, maxDepth, st);
    RNG rr(seed);

    // Modulate chance by generator kind and other macro terrain features.
    float p = 0.14f + 0.017f * static_cast<float>(std::max(0, depth));
    if (g == GenKind::Maze) p *= 0.35f;
    if (g == GenKind::RoomsBsp) p *= 0.90f;
    if (g == GenKind::RoomsGraph) p *= 0.90f;
    if (g == GenKind::Mines) p *= 1.05f;
    if (g == GenKind::Cavern) p *= 1.20f;

    if (d.hasCavernLake) p *= 0.55f;
    if (d.runFaultActive) p *= 0.65f;

    p = std::clamp(p, 0.0f, 0.55f);
    if (!rr.chance(p)) return;

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    // Reserved mask (1 = do not modify).
    std::vector<uint8_t> reserved(static_cast<size_t>(N), uint8_t{0});

    // Keep landings readable and avoid cheap stair traps.
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) hfMarkDisk(reserved, W, H, d.stairsUp.x, d.stairsUp.y, 3);
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) hfMarkDisk(reserved, W, H, d.stairsDown.x, d.stairsDown.y, 3);

    // Keep at least one clean "spine" between stairs.
    hfReserveShortestStairsHalo(d, reserved, 2);

    // Doors (and near-doors) are always left unobstructed.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (isDoorTileType(d.at(x, y).type)) {
                hfMarkDisk(reserved, W, H, x, y, 1);
            }
        }
    }

    // Avoid special rooms entirely.
    for (const Room& rr0 : d.rooms) {
        if (rr0.type == RoomType::Normal) continue;

        const int x0 = std::max(0, rr0.x - 1);
        const int y0 = std::max(0, rr0.y - 1);
        const int x1 = std::min(W - 1, rr0.x2());
        const int y1 = std::min(H - 1, rr0.y2());

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                reserved[idx(x, y)] = 1u;
            }
        }
    }

    // Use the same macro heightfield as the ridge/scree pass so gullies "agree" with the terrain.
    const uint32_t hfSeed = heightfieldSeedKey(worldSeed, branch, depth, maxDepth, st);

    // Heightfield samples for all tiles (independent of layout).
    std::vector<float> height(static_cast<size_t>(N), 0.0f);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(W);
            const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(H);
            height[idx(x, y)] = hfHeight01(hfSeed, nx, ny);
        }
    }

    // Approximate slope magnitude from the height field.
    std::vector<float> slope(static_cast<size_t>(N), 0.0f);
    auto hAt = [&](int x, int y) -> float {
        x = std::clamp(x, 0, W - 1);
        y = std::clamp(y, 0, H - 1);
        return height[idx(x, y)];
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float dx = hAt(x + 1, y) - hAt(x - 1, y);
            const float dy = hAt(x, y + 1) - hAt(x, y - 1);
            slope[idx(x, y)] = std::sqrt(dx * dx + dy * dy);
        }
    }

    // Walkable mask for flow routing (passable tiles, excluding impassable terrain).
    std::vector<uint8_t> flowOk(static_cast<size_t>(N), uint8_t{0});
    std::vector<size_t> flowCells;
    flowCells.reserve(static_cast<size_t>(N / 2));

    int floorCount = 0;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const TileType t = d.at(x, y).type;
            if (t == TileType::Floor) floorCount += 1;

            const bool ok = d.isPassable(x, y) && t != TileType::Chasm && t != TileType::Pillar && t != TileType::Boulder;
            if (!ok) continue;

            const size_t ii = idx(x, y);
            flowOk[ii] = 1u;
            flowCells.push_back(ii);
        }
    }

    if (floorCount < 300) return;

    // D8 flow direction: for each cell, pick the steepest descent neighbor (8-neighborhood).
    std::vector<int> flowTo(static_cast<size_t>(N), -1);
    static const int dirs8[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

    for (size_t ii : flowCells) {
        const int x = static_cast<int>(ii % static_cast<size_t>(W));
        const int y = static_cast<int>(ii / static_cast<size_t>(W));

        const float curH = height[ii];
        float bestH = curH;
        int best = -1;

        // Tie-breaker hash per cell (stable).
        const uint32_t th = hash32(hashCombine(seed ^ 0x9E3779B9u, static_cast<uint32_t>(ii)));

        for (int k = 0; k < 8; ++k) {
            const int nx = x + dirs8[k][0];
            const int ny = y + dirs8[k][1];
            if (!d.inBounds(nx, ny)) continue;
            const size_t ni = idx(nx, ny);
            if (!flowOk[ni]) continue;

            const float nh = height[ni];

            if (nh < bestH - 1e-6f) {
                bestH = nh;
                best = static_cast<int>(ni);
            } else if (best >= 0 && std::abs(nh - bestH) <= 1e-6f && nh < curH - 1e-6f) {
                // Deterministic tie-break in favor of one neighbor.
                if (((th >> static_cast<uint32_t>(k)) & 1u) != 0u) {
                    bestH = nh;
                    best = static_cast<int>(ni);
                }
            }
        }

        // Only accept a strictly downhill step.
        if (best >= 0 && bestH < curH - 1e-6f) {
            flowTo[ii] = best;
        }
    }

    // Flow accumulation: count how many cells drain through each cell (including itself).
    std::vector<int> acc(static_cast<size_t>(N), 0);
    for (size_t ii : flowCells) acc[ii] = 1;

    std::vector<size_t> order = flowCells;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return height[a] > height[b];
    });

    int accMax = 1;
    for (size_t ii : order) {
        const int dst = flowTo[ii];
        if (dst >= 0) {
            acc[static_cast<size_t>(dst)] += acc[ii];
            accMax = std::max(accMax, acc[static_cast<size_t>(dst)]);
        }
    }

    // Channel sources: pick high-elevation, high-slope floor tiles (spaced out).
    struct SourceCand { int x; int y; float score; };
    std::vector<SourceCand> sources;
    sources.reserve(static_cast<size_t>(floorCount / 8));

    for (int y = 2; y < H - 2; ++y) {
        for (int x = 2; x < W - 2; ++x) {
            const size_t ii = idx(x, y);
            if (reserved[ii]) continue;
            if (d.at(x, y).type != TileType::Floor) continue;
            if (!flowOk[ii]) continue;
            if (nearStairs(d, x, y, 4)) continue;
            if (anyDoorInRadius(d, x, y, 1)) continue;
            if (passableCardinalNeighbors(d, x, y) < 3) continue;

            const float h0 = height[ii];
            const float s0 = slope[ii];

            // Prefer higher/steeper points; avoid extreme peaks that tend to be isolated.
            if (h0 < 0.52f) continue;
            if (s0 < 0.045f) continue;

            float score = h0 * 1.20f + s0 * 1.35f;
            // Light bias away from edges.
            score -= 0.004f * static_cast<float>(std::abs(x - W / 2) + std::abs(y - H / 2));

            sources.push_back({x, y, score});
        }
    }

    if (sources.empty()) return;

    std::sort(sources.begin(), sources.end(), [](const SourceCand& a, const SourceCand& b) { return a.score > b.score; });

    int wantChannels = 1;
    if (depth >= 6 && rr.chance(0.60f)) wantChannels += 1;
    if (g == GenKind::Cavern && rr.chance(0.55f)) wantChannels += 1;
    if (g == GenKind::Mines && rr.chance(0.35f)) wantChannels += 1;
    wantChannels = std::clamp(wantChannels, 1, 3);

    const int pool = std::min(60, static_cast<int>(sources.size()));

    const int minSep = (g == GenKind::Cavern) ? 14 : 12;
    const int minSep2 = minSep * minSep;

    std::vector<Vec2i> chosenStarts;
    chosenStarts.reserve(static_cast<size_t>(wantChannels));

    // Budget: don't let gullies dominate already-hazardous floors.
    const int maxTotalChasm = std::clamp(floorCount / 18, 70, 220);

    int totalCarved = 0;

    for (int ch = 0; ch < wantChannels; ++ch) {
        // Pick a start.
        Vec2i start{-1, -1};
        for (int tries = 0; tries < 120; ++tries) {
            const int k = rr.range(0, pool - 1);
            const SourceCand& c = sources[static_cast<size_t>(k)];
            bool ok = true;
            for (const Vec2i& prevStart : chosenStarts) {
                const int dx = c.x - prevStart.x;
                const int dy = c.y - prevStart.y;
                if (dx * dx + dy * dy < minSep2) { ok = false; break; }
            }
            if (!ok) continue;
            start = {c.x, c.y};
            break;
        }

        if (start.x < 0) break;
        chosenStarts.push_back(start);

        // Trace downhill along the flow field.
        std::vector<Vec2i> path;
        path.reserve(256);

        std::vector<uint8_t> visited(static_cast<size_t>(N), uint8_t{0});

        Vec2i cur = start;
        const int maxLen = std::clamp((W + H) * 2, 120, 260);

        for (int step = 0; step < maxLen; ++step) {
            if (!d.inBounds(cur.x, cur.y)) break;
            const size_t ii = idx(cur.x, cur.y);
            if (!flowOk[ii]) break;
            if (visited[ii]) break;
            visited[ii] = 1u;

            path.push_back(cur);

            const int nxt = flowTo[ii];
            if (nxt < 0) break;

            const int nx = nxt % W;
            const int ny = nxt / W;
            cur = {nx, ny};
        }

        if (path.size() < 12) continue;

        // Carve a gully along the traced path.
        std::vector<TileChange> changes;
        changes.reserve(path.size() * 2);

        const int accCarveMin = std::max(10, floorCount / 120);
        const int accWideMin  = std::max(accCarveMin + 8, floorCount / 45);
        const int accWiderMin = std::max(accWideMin + 10, floorCount / 28);

        int carved = 0;
        int causeways = 0;

        auto tryCarve = [&](int x, int y) {
            if (!d.inBounds(x, y)) return;
            const size_t ii = idx(x, y);
            if (reserved[ii]) return;
            if (nearStairs(d, x, y, 4)) return;
            if (anyDoorInRadius(d, x, y, 1)) return;

            Tile& t = d.at(x, y);
            if (t.type != TileType::Floor) return;

            // Avoid shredding narrow corridors; these gullies are meant to be open-space terrain.
            if (passableCardinalNeighbors(d, x, y) < 3) return;

            forceSetTileFeature(d, x, y, TileType::Chasm, changes);
            if (d.at(x, y).type == TileType::Chasm) carved += 1;
        };

        for (size_t i = 0; i < path.size(); ++i) {
            if (totalCarved >= maxTotalChasm) break;

            const Vec2i p0 = path[i];
            const size_t ii = idx(p0.x, p0.y);

            // Only carve where the drainage area is non-trivial (streams deepen downstream).
            if (acc[ii] < accCarveMin) continue;

            // Mild slope gate: avoid painting gullies across ultra-flat basins.
            if (slope[ii] < 0.018f && acc[ii] < accWideMin) continue;

            tryCarve(p0.x, p0.y);
            if (d.at(p0.x, p0.y).type != TileType::Chasm) continue;

            totalCarved += 1;

            // Direction for widening.
            Vec2i dir{0, 0};
            if (i + 1 < path.size()) {
                dir = { path[i + 1].x - p0.x, path[i + 1].y - p0.y };
            } else if (i > 0) {
                dir = { p0.x - path[i - 1].x, p0.y - path[i - 1].y };
            }
            if (dir.x != 0) dir.x = (dir.x > 0) ? 1 : -1;
            if (dir.y != 0) dir.y = (dir.y > 0) ? 1 : -1;

            const Vec2i perp{ dir.y, -dir.x };

            // Widen based on drainage area (thicker downstream).
            if (acc[ii] >= accWideMin) {
                if (rr.chance(0.55f)) tryCarve(p0.x + perp.x, p0.y + perp.y);
                if (rr.chance(0.18f)) tryCarve(p0.x - perp.x, p0.y - perp.y);
            }
            if (acc[ii] >= accWiderMin) {
                if (rr.chance(0.35f)) tryCarve(p0.x + perp.x * 2, p0.y + perp.y * 2);
            }
        }

        if (carved < 10) {
            undoChanges(d, changes);
            continue;
        }

        // Add at least one natural ford if possible (even if we didn't break connectivity).
        if (carved >= 28) {
            const int want = (carved >= 70 && rr.chance(0.40f)) ? 2 : 1;
            for (int i = 0; i < want; ++i) {
                if (placeRavineBridge(d, rr, changes)) causeways += 1;
                else break;
            }
        }

        // Repair connectivity if we severed stairs.
        if (!stairsConnected(d)) {
            for (int tries = 0; tries < 12 && !stairsConnected(d); ++tries) {
                int compCount = 0;
                auto comp = computePassableComponents(d, compCount);
                if (compCount <= 1) break;

                auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };
                const int compUp = (d.inBounds(d.stairsUp.x, d.stairsUp.y)) ? comp[idx2(d.stairsUp.x, d.stairsUp.y)] : -1;
                const int compDown = (d.inBounds(d.stairsDown.x, d.stairsDown.y)) ? comp[idx2(d.stairsDown.x, d.stairsDown.y)] : -1;
                if (compUp < 0 || compDown < 0) break;
                if (compUp == compDown) break;

                const int causewayMaxLen = std::clamp(carved / 2 + 14, 20, 95);

                bool placed = placeChasmCauseway(d, rr, changes, comp, compUp, compDown, causewayMaxLen);
                if (!placed) {
                    placed = placeRavineBridge(d, rr, changes, &comp, compUp, compDown);
                }
                if (!placed) {
                    for (int c = 0; c < compCount; ++c) {
                        if (c == compUp) continue;
                        if (placeChasmCauseway(d, rr, changes, comp, compUp, c, causewayMaxLen)) { placed = true; break; }
                    }
                }

                if (placed) causeways += 1;
                else break;
            }

            if (!stairsConnected(d)) {
                undoChanges(d, changes);
                continue;
            }
        }

        d.fluvialGullyCount += 1;
        d.fluvialChasmCount += carved;
        d.fluvialCausewayCount += causeways;

        // Safety: don't carve too many channels on a single floor.
        if (totalCarved >= maxTotalChasm) break;
    }
}


static void generateStandardFloorWithKind(Dungeon& d, RNG& rng, DungeonBranch branch, int depth, int maxDepth, GenKind g, uint32_t worldSeed, const EndlessStratumInfo& stratum) {
    [[maybe_unused]] const EndlessStratumTheme theme = stratum.theme;
    d.bonusLootSpots.clear();
    d.bonusItemSpawns.clear();
    fillWalls(d);

    switch (g) {
        case GenKind::Cavern:     generateCavern(d, rng, depth); break;
        case GenKind::Maze:       generateMaze(d, rng, depth); break;
        case GenKind::Warrens:    generateWarrens(d, rng, depth); break;
        case GenKind::Mines:      generateMines(d, rng, depth); break;
        case GenKind::Catacombs:  generateCatacombs(d, rng, depth); break;
        case GenKind::RoomsGraph: generateRoomsGraph(d, rng, depth); break;
        case GenKind::RoomsBsp:
        default:
            generateBspRooms(d, rng);
            break;
    }

    // Global fissure/ravine terrain feature.
    // In Infinite World endless depths, use a stratum-aligned persistent rift that drifts smoothly
    // across floors to create large-scale geological continuity.
    if (worldSeed != 0u && depth > maxDepth && stratum.index >= 0) {
        (void)maybeCarveEndlessRift(d, depth, maxDepth, worldSeed, stratum);
    } else if (worldSeed != 0u && depth <= maxDepth) {
        // Finite campaign: optionally apply the run-seeded fault band.
        // If inactive on this depth, fall back to the standard per-floor ravine.
        if (!maybeCarveRunFaultline(d, branch, depth, maxDepth, worldSeed, g)) {
            (void)maybeCarveGlobalRavine(d, rng, depth);
        }
    } else {
        (void)maybeCarveGlobalRavine(d, rng, depth);
    }

    // Cavern floors: carve a blobby subterranean lake (chasm) and auto-repair connectivity with causeways.
    (void)maybeCarveCavernLake(d, rng, depth, g == GenKind::Cavern);

    // Mark special rooms after stairs are placed so we can avoid start/end rooms when possible.
    markSpecialRooms(d, rng, depth);

    // Optional hidden/locked treasure side rooms.
    float pSecret = 0.30f;
    float pVault = 0.22f;
    if (depth >= 6) {
        const float t = static_cast<float>(depth - 5);
        pSecret = std::min(0.55f, pSecret + 0.03f * t);
        pVault = std::min(0.45f, pVault + 0.03f * t);
    }
    if (rng.chance(pSecret)) (void)tryCarveSecretRoom(d, rng, depth);
    if (rng.chance(pVault)) (void)tryCarveVaultRoom(d, rng, depth);

    // Room shape variety: carve internal wall partitions / alcoves in some normal rooms.
    addRoomShapeVariety(d, rng, depth);

    // Structural decoration pass: add interior columns/chasm features that
    // change combat geometry and line-of-sight without breaking the critical
    // stairs path.
    decorateRooms(d, rng, depth);

    // Themed rooms (armory/library/lab) get bespoke interior prefabs too.
    decorateThemedRooms(d, rng, depth);

    // Symmetric furnishings: mirrored pillar/boulder patterns in select rooms,
    // with a reserved navigation spine between doorways. (Always safe: rolls back on failure.)
    applySymmetricRoomFurnishings(d, rng, depth);

    // WFC furnishings: constraint-driven micro-patterns on a coarse grid (colonnades / rubble)
    // with the same safety guarantees (reserved door-to-anchor spines + rollback if needed).
    applyWfcRoomFurnishings(d, rng, depth);

    // Corridor polish pass: widen a few hallway junctions/segments into small hubs/great halls.
    (void)maybeCarveCorridorHubsAndHalls(d, rng, depth, (g == GenKind::RoomsBsp || g == GenKind::RoomsGraph || g == GenKind::Mines));

    // Non-room layouts (caverns/mazes) still benefit from a bit of movable terrain.
    if (d.rooms.empty()) {
        (void)scatterBoulders(d, rng, depth);
    }

    // Secret shortcut doors: hidden doors that connect two adjacent corridor regions.
    (void)maybePlaceSecretShortcuts(d, rng, depth);

    // Locked shortcut gates: visible locked doors that connect adjacent corridor regions.
    (void)maybePlaceLockedShortcuts(d, rng, depth, (g == GenKind::RoomsBsp || g == GenKind::RoomsGraph || g == GenKind::Maze || g == GenKind::Warrens || g == GenKind::Mines || g == GenKind::Catacombs));

    // Sinkholes: carve small chasm clusters in corridors to create local navigation puzzles.
    (void)maybeCarveSinkholes(d, rng, depth, (g == GenKind::RoomsBsp || g == GenKind::RoomsGraph || g == GenKind::Warrens || g == GenKind::Mines || g == GenKind::Catacombs));

    // Inter-room doorways: carve a few direct doors between adjacent rooms (single-wall separation)
    // to create internal loops/flanking routes without changing the core layout.
    (void)applyInterRoomDoorways(d, rng, depth);

    // Corridor braiding: reduce corridor dead-ends by carving short wall tunnels that
    // connect back into the corridor network, creating extra loops and alternate routes.
    {
        CorridorBraidStyle braid = CorridorBraidStyle::Off;
        if (g == GenKind::Maze) braid = CorridorBraidStyle::Heavy;
        else if (g == GenKind::Catacombs) braid = CorridorBraidStyle::Moderate;
        else if (g == GenKind::Warrens) braid = CorridorBraidStyle::Moderate;
        else if (g == GenKind::Mines) braid = CorridorBraidStyle::Sparse;

        if (braid != CorridorBraidStyle::Off) {
            const CorridorBraidResult br = applyCorridorBraiding(d, rng, depth, braid);
            d.corridorBraidCount = br.tunnelsCarved;
        }
    }

    // Terrain sculpt pass: subtle Wall/Floor edge erosion + smoothing to break up
    // overly-rectilinear corridor/room edges.
    {
        bool doSculpt = false;
        TerrainSculptStyle sculptStyle = TerrainSculptStyle::Subtle;

        if (g == GenKind::RoomsBsp) {
            doSculpt = true;
            sculptStyle = TerrainSculptStyle::Subtle;
        } else if (g == GenKind::RoomsGraph) {
            doSculpt = true;
            sculptStyle = TerrainSculptStyle::Ruins;
        } else if (g == GenKind::Mines) {
            doSculpt = true;
            sculptStyle = TerrainSculptStyle::Tunnels;
        } else if (g == GenKind::Catacombs) {
            doSculpt = true;
            sculptStyle = TerrainSculptStyle::Ruins;
        }

        if (doSculpt) {
            const TerrainSculptResult r = applyTerrainSculpt(d, rng, depth, sculptStyle);
            d.terrainSculptCount = r.totalEdits();
        }
    }

    // Annex micro-dungeons: carve a larger optional side area (mini-maze/cavern/ruins)
    // into solid rock behind a secret/locked door. Always optional; never gates the critical stairs path.
    (void)maybeCarveAnnexMicroDungeon(d, rng, depth, g);

    // Dead-end stash closets: carve tiny side closets off corridor/tunnel dead ends.
    (void)maybeCarveDeadEndClosets(d, rng, depth, g);

    // Vault prefabs: carve small handcrafted set-pieces into wall pockets off corridors.
    (void)maybePlaceVaultPrefabs(d, rng, depth, g);

    // Perimeter service tunnels: carve partial inner-border maintenance corridors
    // and punch 2-4 short hatches back into the interior for macro alternate routes.
    (void)applyPerimeterServiceTunnels(d, rng, branch, depth);

    // Burrow crosscuts: A*-dug tunnels through wall mass that create dramatic optional shortcuts.
    (void)applyBurrowCrosscuts(d, rng, branch, depth);

    // Secret crawlspace networks: carve a small hidden network of 1-tile wall passages
    // connected back to the main corridor graph via 2-3 secret doors.
    (void)applySecretCrawlspaceNetwork(d, rng, branch, depth, g);

    // Biome zones: partition the walkable map into a few contiguous regions and apply
    // coherent obstacle/hazard styles per-region (without ever blocking stairs).
    (void)applyBiomeZones(d, rng, depth, g, theme);

    // Macro terrain: warped heightfield ridges + scree (deterministic; safe/rollback).
    applyHeightfieldTerrain(d, branch, depth, maxDepth, g, worldSeed, stratum);
    applyFluvialGullies(d, branch, depth, maxDepth, g, worldSeed, stratum);

    // Final procgen robustness: ensure stair landings remain usable and repair the
    // rare case where late passes accidentally disconnect stairs.
    ensureStairLandingPads(d);
    if (depth != maxDepth && !stairsConnected(d)) {
        (void)repairStairsConnectivity(d, rng);
    }

    // Open-space breakup: if the floor contains a very large open "kill box" area,
    // place a few pillars/boulders at clearance maxima to add tactical occlusion/cover
    // while always preserving stairs connectivity.
    (void)applyOpenSpaceBreakup(d, rng, depth, g);

    // Fire-lane dampening: reduce extreme straight projectile corridors by inserting
    // small barricade chicanes (boulder + side-step bypass) where safe.
    (void)applyFireLaneDampening(d, rng, depth, g);

    // Stairs path weaving: reduce single-edge chokepoints by carving a few tiny
    // bypass loops around bridge edges on the shortest stairs path (when safe).
    weaveStairsConnectivity(d, rng, depth);
    weaveGlobalConnectivity(d, rng, depth);

    // Moated room setpieces: carve chasm-ring "islands" inside select special rooms.
    (void)applyMoatedRoomSetpieces(d, rng, depth);

    // Rift cache pockets: tiny optional boulder-bridge micro-puzzles off normal rooms.
    (void)maybeCarveRiftCachePockets(d, rng, depth, g);

    ensureBorders(d);

    // Final safety: ensure stair tiles survive any later carving/decoration overlap.
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    if (depth != maxDepth && d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
}

static int scoreFloorCandidate(const Dungeon& d, int depth, int maxDepth, uint32_t seed) {
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return std::numeric_limits<int>::min() / 4;
    if (depth != maxDepth && !d.inBounds(d.stairsDown.x, d.stairsDown.y)) return std::numeric_limits<int>::min() / 4;

    const auto dist = bfsDistanceMap(d, d.stairsUp);
    const size_t di = static_cast<size_t>(d.stairsDown.y * d.width + d.stairsDown.x);
    if (di >= dist.size()) return std::numeric_limits<int>::min() / 4;
    const int stairsLen = dist[di];
    if (stairsLen < 0) return std::numeric_limits<int>::min() / 4;

    const int total = d.width * d.height;
    int passable = 0;
    int chasms = 0;
    int doors = 0;

    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const TileType tt = d.at(x, y).type;
            if (d.isPassable(x, y)) ++passable;
            if (tt == TileType::Chasm) ++chasms;
            if (isDoorTileType(tt)) ++doors;
        }
    }

    const int deadEnds = countDeadEnds(d);

    const int densityPct = (total > 0) ? (passable * 100 / total) : 0;
    const int deadPct = (passable > 0) ? (deadEnds * 100 / passable) : 0;
    const int chasmPct = (total > 0) ? (chasms * 100 / total) : 0;

    int score = 0;

    // Strongly prefer stairs redundancy; penalize remaining bridge chokepoints.
    score += 10000;
    if (d.stairsRedundancyOk) score += 6500;
    score -= d.stairsBridgeCount * 1800;
    score -= d.stairsBypassLoopCount * 250;

    // Prefer globally loopy connectivity (reduce major chokepoints across the whole floor).
    // This is a softer preference than the stairs-only redundancy metric.
    score -= std::min(220, d.globalBridgeCountAfter) * 14;
    score -= d.globalBypassLoopCount * 120;

    // Prefer a medium-length critical path (too short = trivial, too long = slog).
    const int targetLen = std::clamp(60 + depth * 6, 60, 180);
    score += 2800 - std::abs(stairsLen - targetLen) * 20;

    // Prefer readable density (avoid super-cramped or super-open extremes).
    const int targetDensity = 34;
    score += 2000 - std::abs(densityPct - targetDensity) * 90;

    // Prefer a moderate dead-end ratio (some dead ends are good for stashes, too many = backtracking pain).
    const int targetDeadPct = 9;
    score += 1200 - std::abs(deadPct - targetDeadPct) * 70;

    // Chasms are cool but too many can overly constrain traversal.
    score += 500 - chasmPct * 80;

    // Fire lanes: prefer a *moderate* maximum straight projectile corridor length.
    // Extremely long lanes become "sniper alleys" where ranged combat is too deterministic.
    {
        const int lane = (d.fireLaneMaxAfter > 0) ? d.fireLaneMaxAfter : computeMaxFireLane(d);
        const int laneTarget = std::clamp(22 - depth, 14, 22);
        score += 900 - std::abs(lane - laneTarget) * 60;
    }

    // Open-space clearance: prefer a moderate max clearance (avoid huge empty kill-box rooms).
    {
        const int openMax = (d.openSpaceClearanceMaxAfter > 0) ? d.openSpaceClearanceMaxAfter : 0;
        const int openTarget = std::clamp(8 - depth / 3, 4, 8);
        score += 700 - std::abs(openMax - openTarget) * 90;
    }

    // Doors: reward a moderate amount (tactical interest) but don't overdo it.
    if (doors > 0) {
        score += 400 - std::abs(doors - 12) * 10;
    }

    // Small bonuses for optional side content (these are additive, not required).
    score += d.secretShortcutCount * 200;
    score += d.lockedShortcutCount * 120;
    score += d.interRoomDoorCount * 110;
    score += d.riftCacheCount * 90;
    score += d.annexCount * 250;
    score += d.vaultPrefabCount * 80;
    score += d.deadEndClosetCount * 60;
    score += d.perimTunnelHatchCount * 70;
    score += d.perimTunnelCacheCount * 60;
    score += d.crosscutTunnelCount * 170;
    score += std::min(80, d.crosscutCarvedTiles) * 1;
    score += d.crawlspaceNetworkCount * 140;
    score += d.crawlspaceDoorCount * 50;
    score += d.crawlspaceCacheCount * 60;

    // Slight room-count preference (keeps room-based floors from collapsing into corridor spaghetti).
    score += std::min(20, static_cast<int>(d.rooms.size())) * 30;

    // Deterministic jitter so ties don't always resolve to attempt 0.
    score += static_cast<int>(hash32(seed ^ 0xA17B0C2Du) % 11u) - 5;

    return score;
}

} // namespace

void Dungeon::generate(RNG& rng, int depth, int maxDepth) {
    generate(rng, DungeonBranch::Main, depth, maxDepth, 0u);
}

void Dungeon::generate(RNG& rng, DungeonBranch branch, int depth, int maxDepth, uint32_t worldSeed) {
    // A default-constructed Dungeon starts at 0x0. Ensure we have a valid grid
    // allocated before generation begins (especially for special layouts that return early).
    if (width <= 0 || height <= 0) {
        // Keep consistent with Game::MAP_W/H.
        width = Dungeon::DEFAULT_W;
        height = Dungeon::DEFAULT_H;
    }
    const size_t expect = static_cast<size_t>(width * height);
    if (tiles.size() != expect) tiles.assign(expect, Tile{});

    bonusLootSpots.clear();
    bonusItemSpawns.clear();

    secretShortcutCount = 0;
    lockedShortcutCount = 0;
    interRoomDoorCount = 0;
    interRoomDoorLockedCount = 0;
    interRoomDoorSecretCount = 0;
    corridorHubCount = 0;
    corridorHallCount = 0;
    sinkholeCount = 0;
    riftCacheCount = 0;
    riftCacheBoulderCount = 0;
    riftCacheChasmCount = 0;
    vaultSuiteCount = 0;
    vaultPrefabCount = 0;
    deadEndClosetCount = 0;
    corridorBraidCount = 0;
    annexCount = 0;
    annexKeyGateCount = 0;
    annexWfcCount = 0;
    annexFractalCount = 0;
    stairsBypassLoopCount = 0;
    stairsBridgeCount = 0;
    stairsRedundancyOk = false;
    hasCavernLake = false;
    hasWarrens = false;

    mazeAlgorithm = MazeAlgorithm::None;
    mazeChamberCount = 0;
    mazeBreakCount = 0;
    mazeWilsonWalkCount = 0;
    mazeWilsonStepCount = 0;
    mazeWilsonLoopEraseCount = 0;
    mazeWilsonMaxPathLen = 0;


    genPickAttempts = 1;
    genPickChosenIndex = 0;
    genPickScore = 0;
    genPickSeed = 0;

    roomsGraphPoissonPointCount = 0;
    roomsGraphPoissonRoomCount = 0;
    roomsGraphDelaunayEdgeCount = 0;
    roomsGraphLoopEdgeCount = 0;

    biomeZoneCount = 0;
    biomePillarZoneCount = 0;
    biomeRubbleZoneCount = 0;
    biomeCrackedZoneCount = 0;
    biomeEdits = 0;

    fireLaneMaxBefore = 0;
    fireLaneMaxAfter = 0;
    fireLaneCoverCount = 0;
    fireLaneChicaneCount = 0;

    openSpaceClearanceMaxBefore = 0;
    openSpaceClearanceMaxAfter = 0;
    openSpacePillarCount = 0;
    openSpaceBoulderCount = 0;

    heightfieldRidgePillarCount = 0;
    heightfieldScreeBoulderCount = 0;

    fluvialGullyCount = 0;
    fluvialChasmCount = 0;
    fluvialCausewayCount = 0;

    overworldSpringCount = 0;
    overworldBrookCount = 0;
    overworldBrookTiles = 0;
    overworldPondCount = 0;

    overworldStrongholdCount = 0;
    overworldStrongholdBuildingCount = 0;
    overworldStrongholdCacheCount = 0;
    overworldStrongholdWallTiles = 0;

    perimTunnelCarvedTiles = 0;
    perimTunnelHatchCount = 0;
    perimTunnelLockedCount = 0;
    perimTunnelCacheCount = 0;

    crosscutTunnelCount = 0;
    crosscutCarvedTiles = 0;
    crosscutDoorLockedCount = 0;
    crosscutDoorSecretCount = 0;

    crawlspaceNetworkCount = 0;
    crawlspaceCarvedTiles = 0;
    crawlspaceDoorCount = 0;
    crawlspaceCacheCount = 0;

    moatedRoomCount = 0;
    moatedRoomBridgeCount = 0;
    moatedRoomChasmCount = 0;

    symmetryRoomCount = 0;
    symmetryObstacleCount = 0;

    spineRoomCount = 0;
    specialRoomMinSep = 0;

    // Campaign stratum debug info (filled only for depth 1..maxDepth in Main branch).
    campaignStratumIndex = -1;
    campaignStratumStartDepth = -1;
    campaignStratumLen = 0;
    campaignStratumLocal = 0;
    campaignStratumTheme = EndlessStratumTheme::Ruins;
    campaignStratumSeed = 0u;


    // Endless stratum debug info (filled only for depth > maxDepth in Main branch).
    endlessStratumIndex = -1;
    endlessStratumStartDepth = -1;
    endlessStratumLen = 0;
    endlessStratumLocal = 0;
    endlessStratumTheme = EndlessStratumTheme::Ruins;
    endlessStratumSeed = 0u;

    // Endless rift debug info (Infinite World macro terrain; not serialized).
    endlessRiftActive = false;
    endlessRiftIntensityPct = 0;
    endlessRiftChasmCount = 0;
    endlessRiftBridgeCount = 0;
    endlessRiftBoulderCount = 0;
    endlessRiftSeed = 0u;

    // Run faultline debug info (finite main-campaign macro terrain; not serialized).
    runFaultActive = false;
    runFaultBandStartDepth = -1;
    runFaultBandLen = 0;
    runFaultBandLocal = 0;
    runFaultIntensityPct = 0;
    runFaultChasmCount = 0;
    runFaultBridgeCount = 0;
    runFaultBoulderCount = 0;
    runFaultSeed = 0u;

    // Sanity clamp.
    if (maxDepth < 1) maxDepth = 1;

    // Camp branch: above-ground hub level.
    if (branch == DungeonBranch::Camp) {
        generateSurfaceCamp(*this, rng);
        ensureBorders(*this);

        // Final safety: ensure stair tiles survive any later carving/decoration overlap.
        if (inBounds(stairsUp.x, stairsUp.y)) at(stairsUp.x, stairsUp.y).type = TileType::StairsUp;
        if (inBounds(stairsDown.x, stairsDown.y)) at(stairsDown.x, stairsDown.y).type = TileType::StairsDown;
        return;
    }

    // Non-camp branches should never have depth <= 0, but clamp for safety.
    if (depth < 1) depth = 1;

    // Main campaign (Main branch) floors are now fully procedural by default.
    //
    // Legacy bespoke set-piece floors (Sokoban/Rogue/Labyrinth/Sanctum) remain in the
    // codebase for potential future opt-in modes, but they are no longer used for the
    // default depth 1..maxDepth run.

    // Choose a generation style (rooms vs caverns vs mazes) once (depth pacing),
    // then generate a few candidate layouts and pick the best by deterministic scoring.
    //
    // This is a lightweight "generate-and-test" meta-pass: it improves floor quality
    // (stairs redundancy, path length, density, dead-end ratio) without changing any
    // gameplay rules or requiring backtracking-heavy retries.
    const EndlessStratumInfo stratum = computeRunStratum(worldSeed, branch, depth, maxDepth);
    [[maybe_unused]] const EndlessStratumTheme theme = stratum.theme;
    const GenKind g = chooseGenKind(branch, depth, maxDepth, worldSeed, rng);

    int attempts = 1;
    if (depth >= 2) attempts = 2;
    if (depth >= 6) attempts = 3;

    // Room-based layouts benefit the most from a bit of extra search.
    if ((g == GenKind::RoomsBsp || g == GenKind::RoomsGraph) && depth >= 2) {
        attempts = std::min(3, attempts + 1);
    }

    attempts = std::clamp(attempts, 1, 3);
    genPickAttempts = attempts;
    genPickChosenIndex = 0;
    genPickScore = 0;
    genPickSeed = 0;

    std::vector<uint32_t> attemptSeeds;
    attemptSeeds.reserve(static_cast<size_t>(attempts));
    for (int i = 0; i < attempts; ++i) {
        uint32_t s = rng.nextU32();
        if (s == 0u) s = 1u;
        // Mix in depth + attempt index so seeds can't collide in pathological cases.
        s = hashCombine(s, hashCombine(static_cast<uint32_t>(depth), static_cast<uint32_t>(i)));
        attemptSeeds.push_back(s);
    }

    int bestScore = std::numeric_limits<int>::min();
    int bestIdx = 0;
    Dungeon best(width, height);

    for (int i = 0; i < attempts; ++i) {
        RNG rr(attemptSeeds[static_cast<size_t>(i)]);
        Dungeon cand(width, height);

        generateStandardFloorWithKind(cand, rr, branch, depth, maxDepth, g, worldSeed, stratum);
        const int score = scoreFloorCandidate(cand, depth, maxDepth, attemptSeeds[static_cast<size_t>(i)]);

        if (i == 0 || score > bestScore) {
            bestScore = score;
            bestIdx = i;
            best = std::move(cand);
        }
    }

    *this = std::move(best);
    genPickAttempts = attempts;
    genPickChosenIndex = bestIdx;
    genPickScore = bestScore;
    genPickSeed = attemptSeeds[static_cast<size_t>(bestIdx)];
    // Persist run stratum info for debug/UI (not serialized).
    if (stratum.index >= 0) {
        if (depth > maxDepth) {
            // Infinite World endless strata.
            endlessStratumIndex = stratum.index;
            endlessStratumStartDepth = stratum.startDepth;
            endlessStratumLen = stratum.len;
            endlessStratumLocal = stratum.local;
            endlessStratumTheme = stratum.theme;
            endlessStratumSeed = stratum.seed;
        } else {
            // Finite campaign strata.
            campaignStratumIndex = stratum.index;
            campaignStratumStartDepth = stratum.startDepth;
            campaignStratumLen = stratum.len;
            campaignStratumLocal = stratum.local;
            campaignStratumTheme = stratum.theme;
            campaignStratumSeed = stratum.seed;
        }
    }
}

void Dungeon::computeEndlessStratumInfo(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    // Reset.
    endlessStratumIndex = -1;
    endlessStratumStartDepth = -1;
    endlessStratumLen = 0;
    endlessStratumLocal = 0;
    endlessStratumTheme = EndlessStratumTheme::Ruins;
    endlessStratumSeed = 0u;

    const EndlessStratumInfo st = computeEndlessStratum(worldSeed, branch, depth, maxDepth);
    if (st.index < 0) return;

    endlessStratumIndex = st.index;
    endlessStratumStartDepth = st.startDepth;
    endlessStratumLen = st.len;
    endlessStratumLocal = st.local;
    endlessStratumTheme = st.theme;
    endlessStratumSeed = st.seed;
}


void Dungeon::computeCampaignStratumInfo(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    // Reset.
    campaignStratumIndex = -1;
    campaignStratumStartDepth = -1;
    campaignStratumLen = 0;
    campaignStratumLocal = 0;
    campaignStratumTheme = EndlessStratumTheme::Ruins;
    campaignStratumSeed = 0u;

    const EndlessStratumInfo st = computeCampaignStratum(worldSeed, branch, depth, maxDepth);
    if (st.index < 0) return;

    campaignStratumIndex = st.index;
    campaignStratumStartDepth = st.startDepth;
    campaignStratumLen = st.len;
    campaignStratumLocal = st.local;
    campaignStratumTheme = st.theme;
    campaignStratumSeed = st.seed;
}



// -----------------------------------------------------------------------------
// Procedural terrain materials (visual / descriptive only)
//
// A deterministic, cellular-noise (Worley/Voronoi) material field gives each tile a
// stable "substrate" (STONE/BRICK/BASALT/...) derived from the run seed + depth.
// This is intentionally *not* serialized; it's recomputed on demand and remains
// stable even if tiles are dug/modified during play.
// -----------------------------------------------------------------------------
namespace {

inline TerrainMaterial pickFromPool(const TerrainMaterial* pool, size_t n, uint32_t r) {
    if (!pool || n == 0) return TerrainMaterial::Stone;
    return pool[static_cast<size_t>(r % static_cast<uint32_t>(n))];
}

TerrainMaterial pickSiteMaterial(uint32_t siteHash, DungeonBranch branch, int depth, int /*maxDepth*/, const EndlessStratumInfo& st) {
    // siteHash is already well-mixed; rehash once with a fixed salt so other uses don't correlate.
    const uint32_t r = hash32(siteHash ^ 0x4D475D5Bu);

    // Surface hub / above-ground camp: bias toward organic materials.
    if (branch == DungeonBranch::Camp) {
        static const TerrainMaterial pool[] = {
            TerrainMaterial::Dirt, TerrainMaterial::Wood, TerrainMaterial::Dirt,
            TerrainMaterial::Moss, TerrainMaterial::Stone, TerrainMaterial::Wood,
        };
        return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
    }
    // Signature floors (finite campaign): keep their identity even if the surrounding
    // stratum theme is different.
    if (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH) {
        static const TerrainMaterial pool[] = {
            TerrainMaterial::Basalt, TerrainMaterial::Stone, TerrainMaterial::Metal,
            TerrainMaterial::Dirt, TerrainMaterial::Metal, TerrainMaterial::Crystal,
        };
        return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
    }

    if (depth == Dungeon::CATACOMBS_DEPTH) {
        static const TerrainMaterial pool[] = {
            TerrainMaterial::Bone, TerrainMaterial::Marble, TerrainMaterial::Basalt,
            TerrainMaterial::Bone, TerrainMaterial::Obsidian,
        };
        return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
    }

    // Strata themes (campaign + endless) get stable macro material palettes.

    if (st.index >= 0 && st.len > 0) {
        switch (st.theme) {
            case EndlessStratumTheme::Ruins: {
                static const TerrainMaterial pool[] = {
                    TerrainMaterial::Stone, TerrainMaterial::Brick, TerrainMaterial::Stone,
                    TerrainMaterial::Marble, TerrainMaterial::Moss, TerrainMaterial::Brick,
                };
                return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
            }
            case EndlessStratumTheme::Caverns: {
                static const TerrainMaterial pool[] = {
                    TerrainMaterial::Basalt, TerrainMaterial::Stone, TerrainMaterial::Basalt,
                    TerrainMaterial::Moss, TerrainMaterial::Dirt,
                };
                return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
            }
            case EndlessStratumTheme::Labyrinth: {
                static const TerrainMaterial pool[] = {
                    TerrainMaterial::Brick, TerrainMaterial::Basalt, TerrainMaterial::Brick,
                    TerrainMaterial::Stone, TerrainMaterial::Obsidian,
                };
                return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
            }
            case EndlessStratumTheme::Warrens: {
                static const TerrainMaterial pool[] = {
                    TerrainMaterial::Dirt, TerrainMaterial::Moss, TerrainMaterial::Dirt,
                    TerrainMaterial::Stone, TerrainMaterial::Wood,
                };
                return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
            }
            case EndlessStratumTheme::Mines: {
                static const TerrainMaterial pool[] = {
                    TerrainMaterial::Metal, TerrainMaterial::Basalt, TerrainMaterial::Stone,
                    TerrainMaterial::Dirt, TerrainMaterial::Metal, TerrainMaterial::Crystal,
                };
                return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
            }
            case EndlessStratumTheme::Catacombs: {
                static const TerrainMaterial pool[] = {
                    TerrainMaterial::Bone, TerrainMaterial::Marble, TerrainMaterial::Bone,
                    TerrainMaterial::Basalt, TerrainMaterial::Obsidian,
                };
                return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
            }
            default:
                break;
        }
    }

    // Finite-depth "classic" dungeon: bias materials gradually as depth increases.
    // We keep this coarse and readable (not overly noisy).

    if (depth <= 1) {
        static const TerrainMaterial pool[] = {
            TerrainMaterial::Brick, TerrainMaterial::Stone, TerrainMaterial::Brick,
            TerrainMaterial::Stone, TerrainMaterial::Moss, TerrainMaterial::Dirt,
        };
        return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
    }

    if (depth <= 4) {
        static const TerrainMaterial pool[] = {
            TerrainMaterial::Stone, TerrainMaterial::Stone, TerrainMaterial::Brick,
            TerrainMaterial::Basalt, TerrainMaterial::Moss, TerrainMaterial::Dirt,
        };
        return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
    }

    if (depth <= 7) {
        static const TerrainMaterial pool[] = {
            TerrainMaterial::Stone, TerrainMaterial::Basalt, TerrainMaterial::Basalt,
            TerrainMaterial::Brick, TerrainMaterial::Marble, TerrainMaterial::Moss,
        };
        return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
    }

    // Deepest finite floors: darker/rarer materials become more common.
    static const TerrainMaterial pool[] = {
        TerrainMaterial::Basalt, TerrainMaterial::Obsidian, TerrainMaterial::Basalt,
        TerrainMaterial::Marble, TerrainMaterial::Bone, TerrainMaterial::Stone,
    };
    return pickFromPool(pool, sizeof(pool) / sizeof(pool[0]), r);
}

inline uint32_t materialCacheKeyFor(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    uint32_t k = hashCombine(worldSeed ^ 0x6D41574Du, static_cast<uint32_t>(branch));
    k = hashCombine(k, hashCombine(static_cast<uint32_t>(depth), static_cast<uint32_t>(maxDepth)));
    k = hash32(k);
    return k ? k : 1u;
}

inline int materialCellSizeFor(int w, int h, int depth, int maxDepth) {
    const int minWH = std::max(1, std::min(w, h));
    int cell = std::clamp((minWH * 2) / 5, 18, 42);

    // Slight depth bias: deeper => a bit more variation (smaller cell).
    if (maxDepth > 0) {
        const float d01 = std::clamp(static_cast<float>(depth) / static_cast<float>(std::max(1, maxDepth)), 0.0f, 1.0f);
        const float scale = 1.05f - 0.25f * d01; // 1.05 .. 0.80
        cell = std::clamp(static_cast<int>(static_cast<float>(cell) * scale + 0.5f), 14, 48);
    }
    return cell;
}

} // namespace

void Dungeon::ensureMaterials(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const {
    const uint32_t key = materialCacheKeyFor(worldSeed, branch, depth, maxDepth);

    const size_t expected = static_cast<size_t>(std::max(0, width) * std::max(0, height));

    if (materialCacheKey == key &&
        materialCacheW == width &&
        materialCacheH == height &&
        materialCache.size() == expected &&
        biolumCache.size() == expected &&
        ecosystemCache.size() == expected &&
        leylineCache.size() == expected) {
        return;
    }

    materialCacheKey = key;
    materialCacheW = width;
    materialCacheH = height;

    if (width <= 0 || height <= 0) {
        materialCache.clear();
        biolumCache.clear();
        ecosystemCache.clear();
        leylineCache.clear();
        ecosystemSeeds.clear();
        materialCacheCell = 0;
        return;
    }

    materialCacheCell = materialCellSizeFor(width, height, depth, maxDepth);

    // Use endless stratum seed (if any) to keep materials aligned to the macro theme bands.
    const EndlessStratumInfo st = computeEndlessStratum(worldSeed, branch, depth, maxDepth);

    const uint32_t seedKey = hash32(hashCombine(key ^ 0xA5B3571Du, st.seed ^ 0xC0FFEEu));

    const uint32_t hfSeed = heightfieldSeedKey(worldSeed, branch, depth, maxDepth, st);

    materialCache.assign(static_cast<size_t>(width * height), static_cast<uint8_t>(TerrainMaterial::Stone));

    const int cell = std::max(1, materialCacheCell);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int cx = x / cell;
            const int cy = y / cell;

            int bestDist = std::numeric_limits<int>::max();
            uint32_t bestSite = 0u;

            // Worley/cellular noise: consider sites in neighboring cells (3x3) and pick the nearest.
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    const int scx = cx + ox;
                    const int scy = cy + oy;

                    const uint32_t sh = hash32(hashCombine(seedKey,
                                                          hashCombine(static_cast<uint32_t>(scx),
                                                                      static_cast<uint32_t>(scy))));
                    const int sx = scx * cell + static_cast<int>(sh % static_cast<uint32_t>(cell));
                    const int sy = scy * cell + static_cast<int>((sh >> 8) % static_cast<uint32_t>(cell));

                    const int dx = x - sx;
                    const int dy = y - sy;
                    const int dist = dx * dx + dy * dy;

                    if (dist < bestDist) {
                        bestDist = dist;
                        bestSite = sh;
                    }
                }
            }

            TerrainMaterial m = pickSiteMaterial(bestSite, branch, depth, maxDepth, st);

            // Heightfield material bias: basins tend mossy/dirt, ridges trend basalt/obsidian,
            // and Mines-like themes get occasional ore seams along ridges.
            const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
            const float h01 = hfHeight01(hfSeed, nx, ny);
            const float r01 = hfRidge01(hfSeed, nx, ny);

            const uint32_t th = hash32(hashCombine(hfSeed ^ 0xB00B135u,
                                                  hashCombine(static_cast<uint32_t>(x),
                                                              static_cast<uint32_t>(y))));

            // Basins: add a bit of moss/dirt.
            if (m != TerrainMaterial::Metal && m != TerrainMaterial::Crystal &&
                m != TerrainMaterial::Bone && m != TerrainMaterial::Wood) {
                if (h01 < 0.20f && (th & 3u) == 0u) {
                    m = TerrainMaterial::Moss;
                } else if (h01 < 0.26f && (th & 7u) == 0u) {
                    m = TerrainMaterial::Dirt;
                }
            }

            // Ridges: bias toward basalt/obsidian.
            if (r01 > 0.82f && h01 > 0.55f &&
                m != TerrainMaterial::Metal && m != TerrainMaterial::Crystal &&
                m != TerrainMaterial::Bone && m != TerrainMaterial::Wood) {
                const bool deep = (depth >= 7) || (st.index >= 0 && st.theme == EndlessStratumTheme::Labyrinth);
                if (deep && ((th >> 8) & 1u)) m = TerrainMaterial::Obsidian;
                else m = TerrainMaterial::Basalt;
            }

            const bool minesTheme =
                (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH) ||
                (st.index >= 0 && st.theme == EndlessStratumTheme::Mines);

            if (minesTheme && r01 > 0.78f && h01 > 0.55f) {
                const bool canOre =
                    (m == TerrainMaterial::Stone || m == TerrainMaterial::Brick || m == TerrainMaterial::Marble ||
                     m == TerrainMaterial::Basalt || m == TerrainMaterial::Obsidian);

                // Rare seams: 1/32 chance.
                if (canOre && (th & 31u) == 0u) {
                    m = (((th >> 5) & 1u) != 0u) ? TerrainMaterial::Metal : TerrainMaterial::Crystal;
                }
            }

            materialCache[static_cast<size_t>(y * width + x)] = static_cast<uint8_t>(m);
        }
    }


    // ---------------------------------------------------------------------
    // Ecosystem / biome-seed field (non-serialized)
    // ---------------------------------------------------------------------
    //
    // This is a deterministic patch-biome system layered on top of the base
    // TerrainMaterial field. It is computed from the same level key as materials
    // so it stays stable for the life of a floor, but it is NOT serialized.
    //
    ecosystemCache.assign(static_cast<size_t>(width * height), static_cast<uint8_t>(EcosystemKind::None));
    ecosystemSeeds.clear();

    if (branch != DungeonBranch::Camp) {
        const bool cavernTheme = (depth == Dungeon::GROTTO_DEPTH) ||
                                 (st.index >= 0 && st.theme == EndlessStratumTheme::Caverns);
        const bool minesTheme = (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH) ||
                                (st.index >= 0 && st.theme == EndlessStratumTheme::Mines);
        const bool tombTheme = (depth == Dungeon::CATACOMBS_DEPTH) ||
                               (st.index >= 0 && (st.theme == EndlessStratumTheme::Labyrinth || st.theme == EndlessStratumTheme::Catacombs));

        const bool deep = (depth >= 7) || (maxDepth > 0 && depth >= maxDepth);

        const uint32_t ecoSeed = hash32(hashCombine(seedKey ^ "ECO"_tag, hfSeed ^ 0xE012ABCDu));
        RNG erng(ecoSeed);

        // Biome palette: each (campaign/endless) stratum tends to feature a stable
        // primary/secondary ecosystem so adjacent floors feel like connected regions,
        // while still allowing minor "accents" for variety.
        struct K { EcosystemKind k; int w; };

        auto buildBaseTable = [&]() -> std::vector<K> {
            std::vector<K> table;
            table.reserve(8);

            auto add = [&](EcosystemKind k, int w) {
                if (w <= 0) return;
                table.push_back({k, w});
            };

            // Baseline weights.
            add(EcosystemKind::FungalBloom, 8);
            add(EcosystemKind::RustVeins, 6);
            add(EcosystemKind::CrystalGarden, 4);
            add(EcosystemKind::BoneField, deep ? 4 : 2);
            add(EcosystemKind::AshenRidge, deep ? 5 : 3);
            add(EcosystemKind::FloodedGrotto, cavernTheme ? 5 : 1);

            // Theme nudges.
            if (cavernTheme) {
                for (auto& e : table) {
                    if (e.k == EcosystemKind::FungalBloom) e.w += 6;
                    if (e.k == EcosystemKind::FloodedGrotto) e.w += 6;
                    if (e.k == EcosystemKind::CrystalGarden) e.w += 2;
                }
            }
            if (minesTheme) {
                for (auto& e : table) {
                    if (e.k == EcosystemKind::RustVeins) e.w += 8;
                    if (e.k == EcosystemKind::CrystalGarden) e.w += 4;
                    if (e.k == EcosystemKind::FungalBloom) e.w = std::max(0, e.w - 4);
                    if (e.k == EcosystemKind::FloodedGrotto) e.w = std::max(0, e.w - 3);
                }
            }
            if (tombTheme) {
                for (auto& e : table) {
                    if (e.k == EcosystemKind::BoneField) e.w += 10;
                    if (e.k == EcosystemKind::FungalBloom) e.w = std::max(0, e.w - 5);
                    if (e.k == EcosystemKind::FloodedGrotto) e.w = std::max(0, e.w - 3);
                }
            }
            return table;
        };

        auto pickFromTable = [&](RNG& rr, const std::vector<K>& table, EcosystemKind forbid) -> EcosystemKind {
            int total = 0;
            for (const auto& e : table) {
                if (e.k == forbid) continue;
                total += std::max(0, e.w);
            }
            if (total <= 0) return EcosystemKind::None;

            int r = rr.range(1, total);
            for (const auto& e : table) {
                if (e.k == forbid) continue;
                r -= std::max(0, e.w);
                if (r <= 0) return e.k;
            }
            // Fallback: last non-forbidden entry.
            for (auto it = table.rbegin(); it != table.rend(); ++it) {
                if (it->k == forbid) continue;
                if (it->w > 0) return it->k;
            }
            return EcosystemKind::None;
        };

        const std::vector<K> baseTable = buildBaseTable();

        // Palette RNG: use stratum seed so floors in the same macro band lean toward
        // the same ecosystems. Signature floors get their own palette.
        uint32_t palSeed = st.seed;
        if (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH ||
            depth == Dungeon::CATACOMBS_DEPTH || depth == Dungeon::GROTTO_DEPTH) {
            palSeed = hash32(hashCombine(seedKey ^ "ECO_PAL"_tag, static_cast<uint32_t>(depth)));
        } else {
            palSeed = hash32(hashCombine(st.seed ^ "ECO_PAL"_tag, static_cast<uint32_t>(st.theme)));
        }
        RNG prng(palSeed);

        EcosystemKind primary = pickFromTable(prng, baseTable, EcosystemKind::None);
        EcosystemKind secondary = pickFromTable(prng, baseTable, primary);
        if (secondary == primary) secondary = EcosystemKind::None;

        // Apply palette bias to the working table.
        std::vector<K> table = baseTable;
        if (primary != EcosystemKind::None) {
            for (auto& e : table) {
                if (e.k == primary) e.w += 10;
            }
        }
        if (secondary != EcosystemKind::None) {
            for (auto& e : table) {
                if (e.k == secondary) e.w += 6;
            }
        }

        auto rollKind = [&]() -> EcosystemKind {
            return pickFromTable(erng, table, EcosystemKind::None);
        };

        int wantSeeds = 2 + std::max(0, depth) / 3;
        if (cavernTheme) wantSeeds += 1;
        if (tombTheme && depth >= 6) wantSeeds += 1;
        wantSeeds = clampi(wantSeeds, 2, 7);

        const int cellBase = std::max(12, std::min(42, materialCacheCell));
        const int minSep = clampi(cellBase / 2, 10, 20);

        auto okStairs = [&](Vec2i p) -> bool {
            const int stairR = 6;
            if (inBounds(stairsUp.x, stairsUp.y) && manhattan(p, stairsUp) <= stairR) return false;
            if (inBounds(stairsDown.x, stairsDown.y) && manhattan(p, stairsDown) <= stairR) return false;
            return true;
        };

        auto farFromOtherSeeds = [&](Vec2i p) -> bool {
            for (const auto& s : ecosystemSeeds) {
                if (manhattan(p, s.pos) < minSep) return false;
            }
            return true;
        };

        const int maxAttemptsPerSeed = 220;
        for (int i = 0; i < wantSeeds; ++i) {
            Vec2i best{-1, -1};
            for (int attempt = 0; attempt < maxAttemptsPerSeed; ++attempt) {
                const int x = erng.range(1, std::max(1, width - 2));
                const int y = erng.range(1, std::max(1, height - 2));
                if (!inBounds(x, y)) continue;
                if (at(x, y).type != TileType::Floor) continue;
                Vec2i p{x, y};
                if (!okStairs(p)) continue;
                if (!farFromOtherSeeds(p)) continue;
                best = p;
                break;
            }
            if (!inBounds(best.x, best.y)) break;

            EcosystemKind kind = rollKind();
            // Ensure the stratum palette actually shows up (gives floors a readable identity).
            if (i == 0 && primary != EcosystemKind::None) {
                kind = primary;
            } else if (i == 1 && secondary != EcosystemKind::None) {
                kind = secondary;
            }
            int r = cellBase * 3 / 4 + erng.range(-3, 4);
            r += depth / 6;
            if (kind == EcosystemKind::FloodedGrotto) r += 2;
            if (kind == EcosystemKind::AshenRidge) r += 1;
            r = clampi(r, 10, 28);

            ecosystemSeeds.push_back(EcosystemSeed{best, kind, r});
        }

        if (!ecosystemSeeds.empty()) {
            // Warp field used for organic boundaries.
            const uint32_t warpSeedX = hash32(hashCombine(hfSeed ^ "ECO_WX"_tag, seedKey));
            const uint32_t warpSeedY = hash32(hashCombine(hfSeed ^ "ECO_WY"_tag, seedKey));
            const float warpAmp = 3.25f;

            // For overlay pass, track which seed influenced each tile.
            std::vector<int8_t> seedIndex(expected, static_cast<int8_t>(-1));

            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const size_t idx = static_cast<size_t>(y * width + x);

                    const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                    const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);

                    const float wx = hfHeight01(warpSeedX, nx, ny) - 0.5f;
                    const float wy = hfHeight01(warpSeedY, nx, ny) - 0.5f;

                    const float fx = static_cast<float>(x) + wx * warpAmp;
                    const float fy = static_cast<float>(y) + wy * warpAmp;

                    float bestScore = 999.0f;
                    int best = -1;

                    for (int s = 0; s < static_cast<int>(ecosystemSeeds.size()); ++s) {
                        const EcosystemSeed& es = ecosystemSeeds[static_cast<size_t>(s)];
                        const float dx = fx - static_cast<float>(es.pos.x);
                        const float dy = fy - static_cast<float>(es.pos.y);
                        const float rr = std::max(1.0f, static_cast<float>(es.radius));
                        const float d2 = dx * dx + dy * dy;
                        float score = d2 / (rr * rr);

                        // Small deterministic per-seed jitter to avoid perfectly smooth contours.
                        const uint32_t h = hash32(hashCombine(ecoSeed ^ static_cast<uint32_t>(s) * 0x9E3779B9u,
                                                             hashCombine(static_cast<uint32_t>(x),
                                                                         static_cast<uint32_t>(y))));
                        score += (rand01(h) - 0.5f) * 0.06f;

                        if (score < bestScore) {
                            bestScore = score;
                            best = s;
                        }
                    }

                    // Only mark tiles as part of an ecosystem when within the seed radius envelope.
                    if (best >= 0 && bestScore < 1.12f) {
                        const EcosystemKind ek = ecosystemSeeds[static_cast<size_t>(best)].kind;
                        ecosystemCache[idx] = static_cast<uint8_t>(ek);
                        seedIndex[idx] = static_cast<int8_t>(best);
                    }
                }
            }

            // Apply ecosystem material overlays (cosmetic + minor substrate effects).
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const size_t idx = static_cast<size_t>(y * width + x);
                    const int si = (idx < seedIndex.size()) ? static_cast<int>(seedIndex[idx]) : -1;
                    if (si < 0) continue;

                    if (at(x, y).type != TileType::Floor) continue;

                    const EcosystemSeed& es = ecosystemSeeds[static_cast<size_t>(si)];

                    const float dx = static_cast<float>(x - es.pos.x);
                    const float dy = static_cast<float>(y - es.pos.y);
                    const float rr = std::max(1.0f, static_cast<float>(es.radius));
                    float t = std::sqrt((dx * dx + dy * dy) / (rr * rr)); // 0..~1
                    t = std::clamp(t, 0.0f, 1.0f);

                    float strength = 1.0f - t;
                    strength = std::clamp(strength, 0.0f, 1.0f);
                    strength = strength * strength; // emphasize centers

                    const uint32_t h = hash32(hashCombine(ecoSeed ^ 0xB10F00Du,
                                                         hashCombine(static_cast<uint32_t>(x),
                                                                     static_cast<uint32_t>(y))));
                    const float r01 = rand01(h);

                    TerrainMaterial base = static_cast<TerrainMaterial>(materialCache[idx]);
                    TerrainMaterial out = base;

                    auto canOverride = [&](TerrainMaterial m) -> bool {
                        // Avoid stomping highly distinctive/man-made materials.
                        if (m == TerrainMaterial::Wood) return false;
                        return true;
                    };

                    if (!canOverride(base)) continue;

                    const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                    const float ny = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
                    const float h01 = hfHeight01(hfSeed, nx, ny);
                    const float ridge01 = hfRidge01(hfSeed, nx, ny);

                    switch (es.kind) {
                        case EcosystemKind::FungalBloom: {
                            // Mossy / earthy patches, stronger in basins.
                            float pMoss = (0.08f + 0.55f * strength) * (0.70f + 0.60f * (1.0f - h01));
                            float pDirt = (0.03f + 0.25f * strength);
                            pMoss = std::clamp(pMoss, 0.0f, 0.80f);
                            pDirt = std::clamp(pDirt, 0.0f, 0.40f);

                            if (base != TerrainMaterial::Metal && base != TerrainMaterial::Crystal && base != TerrainMaterial::Bone) {
                                if (r01 < pMoss) out = TerrainMaterial::Moss;
                                else if (r01 < pMoss + pDirt) out = TerrainMaterial::Dirt;
                            }
                        } break;

                        case EcosystemKind::CrystalGarden: {
                            // Crystalline growths, strongest near ridges (heightfield peaks).
                            float p = (0.05f + 0.45f * strength) * (0.60f + 0.70f * ridge01);
                            p = std::clamp(p, 0.0f, 0.70f);
                            if (base != TerrainMaterial::Bone) {
                                if (r01 < p) out = TerrainMaterial::Crystal;
                            }
                        } break;

                        case EcosystemKind::BoneField: {
                            // Bone dust / ossuary patches.
                            float p = 0.06f + 0.42f * strength;
                            if (tombTheme) p += 0.08f;
                            p = std::clamp(p, 0.0f, 0.75f);
                            if (base != TerrainMaterial::Metal && base != TerrainMaterial::Crystal) {
                                if (r01 < p) out = TerrainMaterial::Bone;
                            }
                        } break;

                        case EcosystemKind::RustVeins: {
                            // Metal/rust seams aligned to ridges.
                            float p = (0.04f + 0.32f * strength) * (0.45f + 0.85f * ridge01);
                            if (minesTheme) p += 0.05f;
                            p = std::clamp(p, 0.0f, 0.65f);

                            if (base != TerrainMaterial::Crystal && base != TerrainMaterial::Bone) {
                                if (r01 < p) out = TerrainMaterial::Metal;
                            }
                        } break;

                        case EcosystemKind::AshenRidge: {
                            // Volcanic stone: basalt/obsidian; stronger on high ridges.
                            float p = (0.05f + 0.28f * strength) * (0.55f + 0.80f * ridge01);
                            p = std::clamp(p, 0.0f, 0.70f);

                            if (base != TerrainMaterial::Metal && base != TerrainMaterial::Crystal && base != TerrainMaterial::Bone) {
                                if (r01 < p) {
                                    // Deep => more obsidian.
                                    const bool obs = deep && (((h >> 8) & 1u) != 0u);
                                    out = obs ? TerrainMaterial::Obsidian : TerrainMaterial::Basalt;
                                }
                            }
                        } break;

                        case EcosystemKind::FloodedGrotto: {
                            // Damp sediment, favors basins.
                            float p = (0.06f + 0.30f * strength) * (0.60f + 0.70f * (1.0f - h01));
                            p = std::clamp(p, 0.0f, 0.55f);

                            if (base != TerrainMaterial::Metal && base != TerrainMaterial::Crystal && base != TerrainMaterial::Bone) {
                                if (r01 < p) out = TerrainMaterial::Dirt;
                                else if (cavernTheme && r01 < p + 0.18f * strength) out = TerrainMaterial::Moss;
                            }
                        } break;

                        default:
                            break;
                    }

                    materialCache[idx] = static_cast<uint8_t>(out);
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // Leyline / arcane resonance field (gameplay + UI)
    // ---------------------------------------------------------------------
    //
    // This produces a thin, deterministic "energy network" layered over the
    // floor plan. It is NOT serialized.
    //
    // Drivers:
    //  - Heightfield ridges (long continuous linework)
    //  - Ecosystem ecotones/junctions (reinforces boundaries + 3-way nodes)
    //  - Conductive substrates (metal/crystal bias)
    //
    // Output:
    //  - 0..255 intensity (higher => stronger ambient mana flow)
    //
    leylineCache.assign(static_cast<size_t>(width * height), uint8_t{0});

    if (branch != DungeonBranch::Camp) {
        const uint32_t leySeed = hash32(hashCombine(seedKey ^ "LEY"_tag, hfSeed ^ 0xA11CE5EDu));

        auto popcount32 = [](uint32_t v) {
            int c = 0;
            while (v) {
                v &= v - 1u;
                ++c;
            }
            return c;
        };

        auto smooth = [](float a, float b, float x) {
            if (b <= a) return 0.0f;
            const float t = (x - a) / (b - a);
            return smoothstep01(t);
        };

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t idx = static_cast<size_t>(y * width + x);

                const TileType tt = at(x, y).type;
                const bool ok = isWalkable(x, y) || (tt == TileType::Chasm);
                if (!ok) continue;

                const EcosystemKind self = static_cast<EcosystemKind>(ecosystemCache[idx]);
                uint32_t mask = 0u;
                int edgeCount = 0;

                auto addMask = [&](EcosystemKind k) {
                    if (k != EcosystemKind::None) {
                        mask |= 1u << static_cast<uint32_t>(k);
                    }
                };

                addMask(self);

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (!inBounds(nx, ny)) continue;

                        const size_t nidx = static_cast<size_t>(ny * width + nx);
                        const EcosystemKind nk = static_cast<EcosystemKind>(ecosystemCache[nidx]);
                        addMask(nk);

                        if (nk != self) {
                            if (nk != EcosystemKind::None || self != EcosystemKind::None) {
                                ++edgeCount;
                            }
                        }
                    }
                }

                const int distinct = popcount32(mask);

                const float fx = (static_cast<float>(x) + 0.5f) / static_cast<float>(std::max(1, width));
                const float fy = (static_cast<float>(y) + 0.5f) / static_cast<float>(std::max(1, height));
                const float ridge01 = hfRidge01(hfSeed, fx, fy);

                const uint32_t h = hash32(hashCombine(leySeed, hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
                const float jitter = (rand01(h) - 0.5f) * 0.20f;

                float v = 0.0f;

                // Ridges: narrow, continuous strands.
                v += 0.72f * smooth(0.78f, 0.93f, ridge01);

                // Ecotones: more energy at biome boundaries.
                const float edge01 = std::clamp(static_cast<float>(edgeCount) / 8.0f, 0.0f, 1.0f);
                v += 0.25f * smooth(0.10f, 1.0f, edge01);

                // Junction nodes.
                if (distinct >= 3) v += 0.16f;
                else if (distinct == 2) v += 0.06f;

                // Substrate conduction.
                const TerrainMaterial m = static_cast<TerrainMaterial>(materialCache[idx]);
                if (m == TerrainMaterial::Metal) v += 0.08f;
                else if (m == TerrainMaterial::Crystal) v += 0.10f;

                // Chasms act as shallow sinks (levitation paths).
                if (tt == TileType::Chasm) v += 0.05f;

                v *= 1.0f + jitter;
                v = std::clamp(v, 0.0f, 1.0f);

                // Sharpen: emphasize hot lines/nodes.
                v = v * v;

                int out = static_cast<int>(std::round(v * 255.0f));
                if (out < 8) out = 0; // dead-zone cutoff
                leylineCache[idx] = static_cast<uint8_t>(clampi(out, 0, 255));
            }
        }
    }

    // ---------------------------------------------------------------------
    // Bioluminescent terrain field (cosmetic)
    // ---------------------------------------------------------------------
    // This is computed deterministically from the same level key as materials.
    // It uses a small Gray-Scott reaction-diffusion sim (masked to floor tiles)
    // to create organic glow/lichen patches that can serve as subtle light sources
    // in darkness mode.
    biolumCache.assign(static_cast<size_t>(width * height), uint8_t{0});

    if (branch == DungeonBranch::Camp) {
        return;
    }

    // Keep this mostly on deeper floors / thematically appropriate strata.
    const bool cavernTheme = (depth == Dungeon::GROTTO_DEPTH) ||
                             (st.index >= 0 && st.theme == EndlessStratumTheme::Caverns);
    const bool minesTheme = (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH) ||
                            (st.index >= 0 && st.theme == EndlessStratumTheme::Mines);
    const bool tombTheme = (depth == Dungeon::CATACOMBS_DEPTH) ||
                           (st.index >= 0 && st.theme == EndlessStratumTheme::Labyrinth);

    const bool wantBiolum = (depth >= 3) || cavernTheme || minesTheme || tombTheme;
    if (!wantBiolum) {
        return;
    }

    const size_t N = static_cast<size_t>(width * height);

    // Active mask: restrict the sim to floor tiles so patterns conform to walkable space.
    std::vector<uint8_t> active(N, uint8_t{0});
    int activeCount = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y * width + x);
            if (at(x, y).type == TileType::Floor) {
                active[i] = 1u;
                ++activeCount;
            }
        }
    }
    if (activeCount <= 0) {
        return;
    }

    std::vector<float> A(N, 1.0f);
    std::vector<float> B(N, 0.0f);

    proc::GrayScottParams pset{};
    if (cavernTheme) {
        // Spotty/lichen-like.
        pset = proc::GrayScottParams{1.0f, 0.50f, 0.0367f, 0.0649f};
    } else if (minesTheme) {
        // Tend toward maze/vein shapes.
        pset = proc::GrayScottParams{1.0f, 0.50f, 0.0220f, 0.0510f};
    } else if (tombTheme) {
        // Softer, more diffuse patches.
        pset = proc::GrayScottParams{1.0f, 0.50f, 0.0300f, 0.0620f};
    } else {
        // General fallback.
        pset = proc::GrayScottParams{1.0f, 0.50f, 0.0460f, 0.0630f};
    }

    float depth01 = 0.0f;
    if (maxDepth > 0) {
        depth01 = std::clamp(static_cast<float>(depth) / static_cast<float>(std::max(1, maxDepth)), 0.0f, 1.0f);
    }
    float seedScale = 0.65f + 0.70f * depth01;
    if (cavernTheme) seedScale *= 1.10f;
    if (minesTheme) seedScale *= 0.95f;

    const uint32_t bioSeed = hash32(hashCombine(seedKey ^ 0xB10B1A5u, hfSeed ^ 0x51A7u));

    auto seedDroplet = [&](int sx, int sy, float strength) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = clampi(sx + dx, 0, width - 1);
                const int yy = clampi(sy + dy, 0, height - 1);
                const size_t j = static_cast<size_t>(yy * width + xx);
                if (j >= active.size() || active[j] == 0u) continue;
                B[j] = std::max(B[j], strength);
                A[j] = std::min(A[j], 1.0f - strength);
            }
        }
    };

    // Seed B with droplets biased by substrate material and theme.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y * width + x);
            if (i >= active.size() || active[i] == 0u) continue;

            TerrainMaterial m = static_cast<TerrainMaterial>(materialCache[i]);
            float p = 0.0f;
            switch (m) {
                case TerrainMaterial::Moss:    p = 0.030f; break;
                case TerrainMaterial::Crystal: p = 0.026f; break;
                case TerrainMaterial::Metal:   p = minesTheme ? 0.014f : 0.006f; break;
                case TerrainMaterial::Bone:    p = tombTheme ? 0.012f : 0.006f; break;
                case TerrainMaterial::Dirt:    p = cavernTheme ? 0.010f : 0.004f; break;
                default:                       p = 0.0f; break;
            }

            if (p <= 0.0f) continue;
            p *= seedScale;

            const uint32_t h = hash32(hashCombine(bioSeed, hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
            const float r01 = static_cast<float>(h & 0xFFFFu) * (1.0f / 65535.0f);

            if (r01 < p) {
                // Strong seed.
                seedDroplet(x, y, 1.0f);
            } else if (r01 < p * 1.75f && ((h >> 16) & 3u) == 0u) {
                // Occasional weaker speckle to keep patterns from collapsing into a few blobs.
                seedDroplet(x, y, 0.60f);
            }
        }
    }

    const int iters = clampi(16 + depth / 2 + (cavernTheme ? 4 : 0) + (minesTheme ? 1 : 0), 14, 32);
    proc::runGrayScott(width, height, pset, iters, A, B, &active);

    // Normalize B into 0..1 for dynamic range mapping.
    float minB = 1.0f;
    float maxB = 0.0f;
    for (size_t i = 0; i < N; ++i) {
        if (i >= active.size() || active[i] == 0u) continue;
        const float v = B[i];
        if (v < minB) minB = v;
        if (v > maxB) maxB = v;
    }

    if (maxB <= minB + 0.0001f) {
        return;
    }

    const int maxI = clampi(70 + depth * 6, 80, 160);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = static_cast<size_t>(y * width + x);
            if (i >= active.size() || active[i] == 0u) continue;

            const float b = B[i];
            float bn = (b - minB) / (maxB - minB);
            bn = proc::clampf(bn, 0.0f, 1.0f);

            // Emphasize peaks so glows read as distinct clusters.
            bn = bn * bn;

            const TerrainMaterial m = static_cast<TerrainMaterial>(materialCache[i]);
            float scale = 0.0f;
            switch (m) {
                case TerrainMaterial::Crystal: scale = 1.00f; break;
                case TerrainMaterial::Moss:    scale = 0.82f; break;
                case TerrainMaterial::Metal:   scale = 0.70f; break;
                case TerrainMaterial::Bone:    scale = 0.62f; break;
                case TerrainMaterial::Dirt:    scale = 0.55f; break;
                default:                       scale = 0.0f; break;
            }
            if (scale <= 0.0f) continue;

            const int s = static_cast<int>(std::round(bn * static_cast<float>(maxI) * scale));
            if (s < 10) continue;

            biolumCache[i] = static_cast<uint8_t>(clampi(s, 0, 255));
        }
    }
}

TerrainMaterial Dungeon::materialAtCached(int x, int y) const {
    if (!inBounds(x, y)) return TerrainMaterial::Stone;
    const size_t idx = static_cast<size_t>(y * width + x);
    if (idx >= materialCache.size()) return TerrainMaterial::Stone;
    const uint8_t v = materialCache[idx];
    if (v >= static_cast<uint8_t>(TerrainMaterial::COUNT)) return TerrainMaterial::Stone;
    return static_cast<TerrainMaterial>(v);
}

uint8_t Dungeon::biolumAtCached(int x, int y) const {
    if (!inBounds(x, y)) return uint8_t{0};
    const size_t idx = static_cast<size_t>(y * width + x);
    if (idx >= biolumCache.size()) return uint8_t{0};
    return biolumCache[idx];
}

uint8_t Dungeon::biolumAt(int x, int y, uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const {
    ensureMaterials(worldSeed, branch, depth, maxDepth);
    return biolumAtCached(x, y);
}

TerrainMaterial Dungeon::materialAt(int x, int y, uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const {
    ensureMaterials(worldSeed, branch, depth, maxDepth);
    return materialAtCached(x, y);
}

EcosystemKind Dungeon::ecosystemAtCached(int x, int y) const {
    if (!inBounds(x, y)) return EcosystemKind::None;
    const size_t idx = static_cast<size_t>(y * width + x);
    if (idx >= ecosystemCache.size()) return EcosystemKind::None;
    const uint8_t v = ecosystemCache[idx];
    if (v >= static_cast<uint8_t>(EcosystemKind::COUNT)) return EcosystemKind::None;
    return static_cast<EcosystemKind>(v);
}

EcosystemKind Dungeon::ecosystemAt(int x, int y, uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const {
    ensureMaterials(worldSeed, branch, depth, maxDepth);
    return ecosystemAtCached(x, y);
}


uint8_t Dungeon::leylineAtCached(int x, int y) const {
    if (!inBounds(x, y)) return uint8_t{0};
    const size_t idx = static_cast<size_t>(y * width + x);
    if (idx >= leylineCache.size()) return uint8_t{0};
    return leylineCache[idx];
}

uint8_t Dungeon::leylineAt(int x, int y, uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const {
    ensureMaterials(worldSeed, branch, depth, maxDepth);
    return leylineAtCached(x, y);
}



bool Dungeon::lineOfSight(int x0, int y0, int x1, int y1) const {
    // Bresenham line; stop if opaque tile blocks.
    // Additionally, prevent "corner peeking": if the line takes a diagonal step
    // between two opaque tiles, we treat LOS as blocked. This keeps monster LOS
    // consistent with player FOV and diagonal movement rules.
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0;
    int y = y0;

    while (true) {
        if (!(x == x0 && y == y0)) {
            if (isOpaque(x, y)) return false;
        }
        if (x == x1 && y == y1) break;

        const int prevX = x;
        const int prevY = y;

        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 <  dx) { err += dx; y += sy; }

        if (!inBounds(x, y)) return false;

        const int stepX = x - prevX;
        const int stepY = y - prevY;
        if (stepX != 0 && stepY != 0) {
            // Diagonal step: check the two cardinal neighbors we are "cutting" between.
            const int ax = prevX + stepX;
            const int ay = prevY;
            const int bx = prevX;
            const int by = prevY + stepY;

            if (inBounds(ax, ay) && inBounds(bx, by)) {
                if (isOpaque(ax, ay) && isOpaque(bx, by)) return false;
            }
        }
    }

    return true;
}

bool Dungeon::hasLineOfSight(int x0, int y0, int x1, int y1) const {
    if (!inBounds(x0, y0) || !inBounds(x1, y1)) return false;
    return lineOfSight(x0, y0, x1, y1);
}

bool Dungeon::soundPassable(int x, int y) const {
    if (!inBounds(x, y)) return false;
    const TileType t = at(x, y).type;
    // Walls, pillars, and secret doors completely block sound propagation.
    return (t != TileType::Wall && t != TileType::Pillar && t != TileType::DoorSecret);
}

int Dungeon::soundTileCost(int x, int y) const {
    if (!inBounds(x, y)) return 1000000000;
    const TileType t = at(x, y).type;

    // Closed/locked doors muffle sound more than open spaces.
    int cost = 1;
    switch (t) {
        case TileType::DoorClosed: cost = 2; break;
        case TileType::DoorLocked: cost = 3; break;
        default: cost = 1; break;
    }

    // Substrate acoustics: moss/dirt absorb sound (higher cost), while
    // metal/crystal can carry it a bit farther (lower cost). This uses the
    // deterministic per-floor material cache.
    const TerrainMaterial m = materialAtCached(x, y);
    cost += terrainMaterialFx(m).soundTileCostDelta;

    // Never allow <=0 costs (pathfinding treats non-positive as blocked).
    return std::max(1, cost);
}

bool Dungeon::soundDiagonalOk(int fromX, int fromY, int dx, int dy) const {
    if (dx == 0 || dy == 0) return true;
    const int ax = fromX + dx;
    const int ay = fromY;
    const int bx = fromX;
    const int by = fromY + dy;
    // For sound, we use soundPassable (not walkable) because closed doors still transmit sound.
    return soundPassable(ax, ay) || soundPassable(bx, by);
}

std::vector<int> Dungeon::computeSoundMap(int sx, int sy, int maxCost) const {
    std::vector<int> dist(static_cast<size_t>(width * height), -1);
    if (maxCost < 0) return dist;
    if (!inBounds(sx, sy)) return dist;

    auto soundPassableFn = [&](int x, int y) -> bool { return soundPassable(x, y); };
    auto tileCostFn = [&](int x, int y) -> int { return soundTileCost(x, y); };

    if (!soundPassableFn(sx, sy)) return dist;

    // Prevent diagonal "corner cutting" through two blocking tiles.
    DiagonalOkFn diagOk = [&](int fromX, int fromY, int dx, int dy) -> bool {
        return soundDiagonalOk(fromX, fromY, dx, dy);
    };

    const std::vector<Vec2i> sources = {Vec2i{sx, sy}};
    return dijkstraCostFromSources(width, height, sources, soundPassableFn, tileCostFn, diagOk, maxCost);
}



void Dungeon::computeFov(int px, int py, int radius, bool markExplored) {
    // Reset visibility each frame
    for (auto& t : tiles) t.visible = false;
    if (!inBounds(px, py)) return;

    auto isOpaqueTile = [&](int x, int y) {
        if (!inBounds(x, y)) return true;
        TileType tt = at(x, y).type;
        return (tt == TileType::Wall || tt == TileType::Pillar || tt == TileType::DoorClosed || tt == TileType::DoorLocked || tt == TileType::DoorSecret);
    };

    auto markVis = [&](int x, int y) {
        if (!inBounds(x, y)) return;
        Tile& t = at(x, y);
        t.visible = true;
        if (markExplored) t.explored = true;
    };

    // Always see your own tile
    markVis(px, py);

    // Recursive shadowcasting for 8 octants.
    // Reference: RogueBasin "Recursive Shadowcasting".
    const int r2 = radius * radius;

    std::function<void(int, float, float, int, int, int, int)> castLight;
    castLight = [&](int row, float start, float end, int xx, int xy, int yx, int yy) {
        if (start < end) return;
        float newStart = start;
        for (int dist = row; dist <= radius; ++dist) {
            bool blocked = false;

            for (int dx = -dist, dy = -dist; dx <= 0; ++dx) {
                const float lSlope = (dx - 0.5f) / (dy + 0.5f);
                const float rSlope = (dx + 0.5f) / (dy - 0.5f);
                if (start < rSlope) continue;
                if (end > lSlope) break;

                const int sax = dx * xx + dy * xy;
                const int say = dx * yx + dy * yy;
                const int ax = px + sax;
                const int ay = py + say;

                if (!inBounds(ax, ay)) continue;
                const int d2 = (ax - px) * (ax - px) + (ay - py) * (ay - py);
                if (d2 <= r2) {
                    markVis(ax, ay);
                }

                if (blocked) {
                    if (isOpaqueTile(ax, ay)) {
                        newStart = rSlope;
                        continue;
                    } else {
                        blocked = false;
                        start = newStart;
                    }
                } else {
                    if (isOpaqueTile(ax, ay) && dist < radius) {
                        blocked = true;
                        castLight(dist + 1, start, lSlope, xx, xy, yx, yy);
                        newStart = rSlope;
                    }
                }
            }

            if (blocked) break;
        }
    };

    // Octant transforms
    castLight(1, 1.0f, 0.0f, 1, 0, 0, 1);
    castLight(1, 1.0f, 0.0f, 0, 1, 1, 0);
    castLight(1, 1.0f, 0.0f, 0, -1, 1, 0);
    castLight(1, 1.0f, 0.0f, -1, 0, 0, 1);
    castLight(1, 1.0f, 0.0f, -1, 0, 0, -1);
    castLight(1, 1.0f, 0.0f, 0, -1, -1, 0);
    castLight(1, 1.0f, 0.0f, 0, 1, -1, 0);
    castLight(1, 1.0f, 0.0f, 1, 0, 0, -1);
}


void Dungeon::computeFovMask(int px, int py, int radius, std::vector<uint8_t>& outMask) const {
    outMask.assign(static_cast<size_t>(width * height), uint8_t{0});
    if (!inBounds(px, py)) return;

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * width + x); };

    auto isOpaqueTile = [&](int x, int y) {
        if (!inBounds(x, y)) return true;
        TileType tt = at(x, y).type;
        return (tt == TileType::Wall || tt == TileType::Pillar || tt == TileType::DoorClosed || tt == TileType::DoorLocked || tt == TileType::DoorSecret);
    };

    auto markVis = [&](int x, int y) {
        if (!inBounds(x, y)) return;
        outMask[idx(x, y)] = uint8_t{1};
    };

    // Always see your own tile
    markVis(px, py);

    // Recursive shadowcasting for 8 octants.
    // Reference: RogueBasin "Recursive Shadowcasting".
    const int r2 = radius * radius;

    std::function<void(int, float, float, int, int, int, int)> castLight;
    castLight = [&](int row, float start, float end, int xx, int xy, int yx, int yy) {
        if (start < end) return;
        float newStart = start;
        for (int dist = row; dist <= radius; ++dist) {
            bool blocked = false;

            for (int dx = -dist, dy = -dist; dx <= 0; ++dx) {
                const float lSlope = (dx - 0.5f) / (dy + 0.5f);
                const float rSlope = (dx + 0.5f) / (dy - 0.5f);
                if (start < rSlope) continue;
                if (end > lSlope) break;

                const int sax = dx * xx + dy * xy;
                const int say = dx * yx + dy * yy;
                const int ax = px + sax;
                const int ay = py + say;

                if (!inBounds(ax, ay)) continue;
                const int d2 = (ax - px) * (ax - px) + (ay - py) * (ay - py);
                if (d2 <= r2) {
                    markVis(ax, ay);
                }

                if (blocked) {
                    if (isOpaqueTile(ax, ay)) {
                        newStart = rSlope;
                        continue;
                    } else {
                        blocked = false;
                        start = newStart;
                    }
                } else {
                    if (isOpaqueTile(ax, ay) && dist < radius) {
                        blocked = true;
                        castLight(dist + 1, start, lSlope, xx, xy, yx, yy);
                        newStart = rSlope;
                    }
                }
            }

            if (blocked) break;
        }
    };

    // Octant transforms
    castLight(1, 1.0f, 0.0f, 1, 0, 0, 1);
    castLight(1, 1.0f, 0.0f, 0, 1, 1, 0);
    castLight(1, 1.0f, 0.0f, 0, -1, 1, 0);
    castLight(1, 1.0f, 0.0f, -1, 0, 0, 1);
    castLight(1, 1.0f, 0.0f, -1, 0, 0, -1);
    castLight(1, 1.0f, 0.0f, 0, -1, -1, 0);
    castLight(1, 1.0f, 0.0f, 0, 1, -1, 0);
    castLight(1, 1.0f, 0.0f, 1, 0, 0, -1);
}


void Dungeon::revealAll() {
    for (auto& t : tiles) {
        t.explored = true;
    }
}

Vec2i Dungeon::randomFloor(RNG& rng, bool avoidDoors) const {
    for (int tries = 0; tries < 4000; ++tries) {
        int x = rng.range(1, width - 2);
        int y = rng.range(1, height - 2);
        TileType t = at(x, y).type;
        if (t == TileType::Floor || t == TileType::StairsDown || t == TileType::StairsUp || (!avoidDoors && (t == TileType::DoorOpen || t == TileType::DoorClosed))) {
            return {x, y};
        }
    }
    // Fallback: scan
    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            if (at(x, y).type == TileType::Floor) return {x, y};
        }
    }
    return {1, 1};
}
