#include "dungeon.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <queue>
#include <utility>

namespace {

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
    d.stairsUp = {-1, -1};
    d.stairsDown = {-1, -1};
    d.hasCavernLake = false;
    d.hasWarrens = false;
    d.secretShortcutCount = 0;
    d.lockedShortcutCount = 0;
    d.corridorHubCount = 0;
    d.corridorHallCount = 0;
    d.sinkholeCount = 0;
    d.vaultSuiteCount = 0;
    d.deadEndClosetCount = 0;
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

    std::vector<uint8_t> visited(static_cast<size_t>(d.width * d.height), 0);

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
    std::vector<uint8_t> closed(static_cast<size_t>(S), 0);

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
    std::vector<uint8_t> roomMask(static_cast<size_t>(d.width * d.height), 0);
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
    std::vector<uint8_t> blocked(static_cast<size_t>(d.width * d.height), 0);
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
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    // Protect a shortest path between stairs so sinkholes never block progression.
    std::vector<uint8_t> protectedTile(static_cast<size_t>(d.width * d.height), 0);
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
    keep.assign(static_cast<size_t>(d.width * d.height), 0);
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

    std::vector<uint8_t> visited(static_cast<size_t>(d.width * d.height), 0);
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

    std::vector<uint8_t> isGoal(static_cast<size_t>(W * H), 0);

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

GenKind chooseGenKind(int depth, int maxDepth, RNG& rng) {
    // The default run now spans ~20 floors, so we pace variety in two arcs:
    // - Early: classic rooms (with occasional "ruins" variant)
    // - Early spikes: mines + grotto + an early maze/warrens band
    // - Midpoint: a bigger "you are deep now" spike
    // - Late: a second set of themed generator hits (lower mines/catacombs/cavern)
    // - Endgame: scripted penultimate labyrinth + final sanctum (handled in Dungeon::generate)
    if (maxDepth < 1) maxDepth = 1;

    const int midpoint = std::max(1, maxDepth / 2);

    if (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH) return GenKind::Mines;

    // Early floors: mostly classic BSP rooms, but occasionally use the graph/packed-rooms variant
    // to keep runs from feeling identical.
    if (depth <= 3) {
        // Depth 3 is a handcrafted Sokoban floor (handled earlier), but keep this safe for tests/endless.
        if (depth == 1) {
            // Keep the very first floor mostly familiar, but not always.
            if (rng.chance(0.40f)) return GenKind::RoomsGraph;
        }
        return GenKind::RoomsBsp;
    }

    if (depth == Dungeon::GROTTO_DEPTH) return GenKind::Cavern;

    // Early variety spike (originally the 10-floor "midpoint"; now closer to the first quarter).
    if (depth == 5) {
        const float r = rng.next01();
        // Maze spike, organic warrens, or a "ruins" rooms floor.
        if (r < 0.45f) return GenKind::Maze;
        if (r < 0.65f) return GenKind::Warrens;
        if (r < 0.90f) return GenKind::RoomsGraph;
        return GenKind::RoomsBsp;
    }

    // True midpoint spike: lean harder into non-room layouts so the run's second half
    // feels different even if the player has strong gear already.
    if (depth == midpoint) {
        const float r = rng.next01();
        if (r < 0.30f) return GenKind::Maze;
        if (r < 0.55f) return GenKind::Warrens;
        if (r < 0.72f) return GenKind::Catacombs;
        if (r < 0.84f) return GenKind::Cavern;
        if (r < 0.94f) return GenKind::RoomsGraph;
        return GenKind::RoomsBsp;
    }

    // Note: depth 6 is a fixed Rogue homage floor (handled earlier), but keep this for endless/testing.
    if (depth == 6) return GenKind::RoomsBsp;
    if (depth == Dungeon::CATACOMBS_DEPTH) return GenKind::Catacombs;

    // A consistent breather floor before the midpoint spike.
    if (depth == 9) return GenKind::RoomsBsp;

    // Late-run "second arc" setpieces. These are relative to maxDepth so tests that pass
    // smaller maxDepth values still behave sensibly.
    if (depth == midpoint + 2 && depth < maxDepth - 1) return GenKind::Mines;
    if (depth == midpoint + 4 && depth < maxDepth - 1) return GenKind::Catacombs;
    if (depth == midpoint + 6 && depth < maxDepth - 1) return GenKind::Cavern;

    // Calm before the penultimate labyrinth (Dungeon::generate will handle maxDepth-1).
    if (maxDepth >= 8 && depth == maxDepth - 2) {
        // Slight bias toward the "ruins" generator so the player sees more doors/loops
        // right before the final approach.
        return rng.chance(0.35f) ? GenKind::RoomsGraph : GenKind::RoomsBsp;
    }

    // General case: sprinkle variety, with a slightly "nastier" distribution deeper
    // than the midpoint.
    const float r = rng.next01();
    if (depth > midpoint) {
        if (r < 0.14f) return GenKind::Maze;
        if (r < 0.32f) return GenKind::Warrens;
        if (r < 0.46f) return GenKind::Catacombs;
        if (r < 0.58f) return GenKind::Cavern;
        if (r < 0.70f) return GenKind::Mines;
        if (r < 0.86f) return GenKind::RoomsGraph;
        return GenKind::RoomsBsp;
    }

    // Pre-midpoint band: still mostly rooms, but with occasional spice.
    if (r < 0.08f) return GenKind::Maze;
    if (r < 0.18f) return GenKind::Warrens;
    if (r < 0.26f) return GenKind::Catacombs;
    if (r < 0.40f) return GenKind::Cavern;
    if (r < 0.52f) return GenKind::Mines;
    if (r < 0.72f) return GenKind::RoomsGraph;
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
bool maybeCarveDeadEndClosets(Dungeon& d, RNG& rng, int depth, GenKind g) {
    d.deadEndClosetCount = 0;

    // Skip on cavern floors: organic caves already have lots of pockets and
    // carving rectangular closets tends to look unnatural.
    if (g == GenKind::Cavern) return false;

    const int W = d.width;
    const int H = d.height;
    if (W <= 4 || H <= 4) return false;

    // Build an "in room" mask so we only consider corridor/tunnel dead-ends.
    std::vector<uint8_t> inRoom(static_cast<size_t>(W * H), 0u);
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
        // Secret closets are slightly more likely to be rewarding.
        const float chestChance = secretDoor ? 0.92f : 0.78f;
        if (rng.chance(chestChance)) {
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
                d.bonusLootSpots.push_back(best);
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

    // Treasure is the most important for gameplay pacing; bias toward deeper rooms.
    int t = pickFarthest(pool, 3);
    if (t >= 0) d.rooms[static_cast<size_t>(t)].type = RoomType::Treasure;

    // Deep floors can carry extra treasure to support a longer run.
    if (depth >= 7) {
        float extraTreasureChance = std::min(0.55f, 0.25f + 0.05f * static_cast<float>(depth - 7));
        if (rng.chance(extraTreasureChance)) {
            int t2 = pickFarthest(pool, 2);
            if (t2 >= 0) d.rooms[static_cast<size_t>(t2)].type = RoomType::Treasure;
        }
    }

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
        int sh = pickClosest(pool, 3, minShopDist);
        if (sh >= 0) d.rooms[static_cast<size_t>(sh)].type = RoomType::Shop;
    }

    // Lairs: generally deeper rooms (wolf packs / nastier encounters).
    int l = pickFarthest(pool, 3);
    if (l >= 0) d.rooms[static_cast<size_t>(l)].type = RoomType::Lair;

    // Shrines: mid-ish so they're useful but not right on the stairs.
    int s = pickQuantile(pool, 0.45f, 2);
    if (s >= 0) d.rooms[static_cast<size_t>(s)].type = RoomType::Shrine;

    // Themed rooms: a light-touch extra specialization to diversify loot/encounters.
    if (!pool.empty() && depth >= 2) {
        float themeChance = 0.55f;
        if (depth >= 4) themeChance = 0.70f;
        if (depth >= 7) themeChance = 0.82f;
        // Midpoint floor: slightly increase the chance for a themed room.
        if (depth == 5) themeChance = 0.90f;

        if (rng.chance(std::min(0.95f, themeChance))) {
            int rr = pickQuantile(pool, 0.60f, 3);
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

                d.rooms[static_cast<size_t>(rr)].type = rt;
            }
        }
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
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
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



void generateRoomsGraph(Dungeon& d, RNG& rng, int depth) {
    // "Ruins" room generator:
    // - Randomly pack non-overlapping rectangular rooms (light Poisson-ish spacing)
    // - Connect them with a minimum spanning tree (guaranteed global connectivity)
    // - Add a few extra edges for loops (more interesting navigation / flanking)
    // - Add some corridor branches for treasure pockets / dead ends
    //
    // This complements the BSP generator by producing less hierarchical, more "scattered" layouts.

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
    const int minCenterDist = std::clamp((minDim / 6) + 6, 8, 14);

    const int margin = 2;
    int attempts = target * 160;

    auto centerOk = [&](int cx, int cy) -> bool {
        for (const Room& r : d.rooms) {
            const int md = std::abs(cx - r.cx()) + std::abs(cy - r.cy());
            if (md < minCenterDist) return false;
        }
        return true;
    };

    while (static_cast<int>(d.rooms.size()) < target && attempts-- > 0) {
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

        const int rx = rng.range(2, std::max(2, d.width - rw - 3));
        const int ry = rng.range(2, std::max(2, d.height - rh - 3));

        const int cx = rx + rw / 2;
        const int cy = ry + rh / 2;
        if (!centerOk(cx, cy)) continue;

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
    }

    // If placement failed badly, fall back to a safer generator.
    if (d.rooms.size() < 4) {
        fillWalls(d);
        generateBspRooms(d, rng);
        return;
    }

    // Precompute which tiles are inside rooms for corridor routing + later passes.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
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
    std::vector<Edge> edges;
    edges.reserve(static_cast<size_t>(n * (n - 1) / 2));

    for (int i = 0; i < n; ++i) {
        const Vec2i ca{ d.rooms[static_cast<size_t>(i)].cx(), d.rooms[static_cast<size_t>(i)].cy() };
        for (int j = i + 1; j < n; ++j) {
            const Vec2i cb{ d.rooms[static_cast<size_t>(j)].cx(), d.rooms[static_cast<size_t>(j)].cy() };
            const int w = std::abs(ca.x - cb.x) + std::abs(ca.y - cb.y);
            edges.push_back({i, j, w});
        }
    }

    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.w != b.w) return a.w < b.w;
        if (a.a != b.a) return a.a < b.a;
        return a.b < b.b;
    });

    // Connect rooms with an MST (guaranteed global connectivity).
    DSU dsu(n);
    std::vector<uint8_t> usedEdge(edges.size(), 0);

    int used = 0;
    for (size_t ei = 0; ei < edges.size() && used < n - 1; ++ei) {
        const Edge& e = edges[ei];
        if (dsu.unite(e.a, e.b)) {
            connectRooms(d, d.rooms[static_cast<size_t>(e.a)], d.rooms[static_cast<size_t>(e.b)], rng, inRoom);
            usedEdge[ei] = 1;
            used++;
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

            // Stop if we accidentally connected to existing space; keep it "branchy".
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
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
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
    std::vector<uint8_t> usedEdge(edges.size(), 0);

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


void generateCavern(Dungeon& d, RNG& rng, int depth) {
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

    d.rooms.clear();

    // Start chamber near the center.
    const int cx = d.width / 2;
    const int cy = d.height / 2;
    const int sw = rng.range(6, 8);
    const int sh = rng.range(5, 7);
    int sx = clampi(cx - sw / 2, 1, d.width - sw - 1);
    int sy = clampi(cy - sh / 2, 1, d.height - sh - 1);
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

void generateMaze(Dungeon& d, RNG& rng, int depth) {
    (void)depth;
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

    std::vector<uint8_t> vis(static_cast<size_t>(cellW * cellH), 0);
    std::vector<Vec2i> stack;
    stack.reserve(static_cast<size_t>(cellW * cellH));

    const int startCx = cellW / 2;
    const int startCy = cellH / 2;
    stack.push_back({startCx, startCy});
    vis[cidx(startCx, startCy)] = 1;
    Vec2i sp = cellToPos(startCx, startCy);
    d.at(sp.x, sp.y).type = TileType::Floor;

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!stack.empty()) {
        Vec2i cur = stack.back();

        // Collect unvisited neighbors.
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

    // Add a few loops (break walls) so the maze isn't a strict tree.
    const int breaks = std::max(6, (cellW * cellH) / 6);
    for (int i = 0; i < breaks; ++i) {
        int x = rng.range(2, d.width - 3);
        int y = rng.range(2, d.height - 3);
        if (d.at(x, y).type != TileType::Wall) continue;

        // Break walls that connect two corridors.
        bool horiz = (d.at(x - 1, y).type == TileType::Floor && d.at(x + 1, y).type == TileType::Floor);
        bool vert  = (d.at(x, y - 1).type == TileType::Floor && d.at(x, y + 1).type == TileType::Floor);
        if (!(horiz || vert)) continue;
        d.at(x, y).type = TileType::Floor;
    }

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
        return;
    }

    d.rooms.clear();
    const int sw = rng.range(6, 8);
    const int sh = rng.range(5, 7);
    int sx = clampi(best.x - sw / 2, 1, d.width - sw - 1);
    int sy = clampi(best.y - sh / 2, 1, d.height - sh - 1);
    carveRect(d, sx, sy, sw, sh, TileType::Floor);
    d.rooms.push_back({sx, sy, sw, sh, RoomType::Normal});

    // Additional chambers
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

    // Stairs
    const Room& startRoom = d.rooms.front();
    d.stairsUp = { startRoom.cx(), startRoom.cy() };
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.stairsUp = {1, 1};

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.stairsDown = {d.width - 2, d.height - 2};

    // Sprinkle some closed doors in corridor chokepoints to make LOS + combat more interesting.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
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
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
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
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    // Track which cell-to-cell walls have been opened so we can add loops later.
    std::vector<uint8_t> openE(static_cast<size_t>(cols * rows), 0);
    std::vector<uint8_t> openS(static_cast<size_t>(cols * rows), 0);

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
    std::vector<uint8_t> visited(static_cast<size_t>(cols * rows), 0);
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
void generateLabyrinth(Dungeon& d, RNG& rng, int depth) {
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

    std::vector<uint8_t> vis(static_cast<size_t>(cellW * cellH), 0);
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
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
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

void generateRogueLevel(Dungeon& d, RNG& rng, int depth) {
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


// Surface camp (depth 0): an above-ground hub with a simple palisade + tent layout.
// This acts as a "safe-ish" staging area above the dungeon entrance.
void generateSurfaceCamp(Dungeon& d, RNG& rng) {
    fillWalls(d);
    d.rooms.clear();
    d.bonusLootSpots.clear();
    d.hasCavernLake = false;
    d.hasWarrens = false;
    d.secretShortcutCount = 0;
    d.lockedShortcutCount = 0;
    d.corridorHubCount = 0;
    d.corridorHallCount = 0;
    d.sinkholeCount = 0;
    d.vaultSuiteCount = 0;
    d.deadEndClosetCount = 0;
    d.campStashSpot = {-1, -1};

    // Start with an open outdoor field (floor) with border walls.
    if (d.width >= 3 && d.height >= 3) {
        carveRect(d, 1, 1, d.width - 2, d.height - 2, TileType::Floor);
    }

    // ------------------------------------------------------------
    // Camp geometry: a centered palisade "yard" with a single open gate.
    // ------------------------------------------------------------
    int campW = std::max(12, d.width / 3);
    int campH = std::max(10, d.height / 3);

    campW = std::min(campW, std::max(8, d.width - 6));
    campH = std::min(campH, std::max(6, d.height - 6));

    int campX = (d.width - campW) / 2;
    int campY = (d.height - campH) / 2;

    campX = clampi(campX, 2, std::max(2, d.width - campW - 2));
    campY = clampi(campY, 2, std::max(2, d.height - campH - 2));

    const int campX2 = campX + campW - 1;
    const int campY2 = campY + campH - 1;

    // Palisade walls.
    for (int x = campX; x <= campX2; ++x) {
        if (d.inBounds(x, campY)) d.at(x, campY).type = TileType::Wall;
        if (d.inBounds(x, campY2)) d.at(x, campY2).type = TileType::Wall;
    }
    for (int y = campY; y <= campY2; ++y) {
        if (d.inBounds(campX, y)) d.at(campX, y).type = TileType::Wall;
        if (d.inBounds(campX2, y)) d.at(campX2, y).type = TileType::Wall;
    }

    // Gate: open door on the south wall so the camp is reachable without interaction.
    const int gateX = campX + campW / 2;
    const Vec2i gate{gateX, campY2};
    if (d.inBounds(gate.x, gate.y)) {
        d.at(gate.x, gate.y).type = TileType::DoorOpen;
        carveFloor(d, gate.x, gate.y - 1);
        carveFloor(d, gate.x, gate.y + 1);
    }

    // ------------------------------------------------------------
    // Tent / hut: a small room inside the yard (closed door for flavor).
    // ------------------------------------------------------------
    int tentW = std::min(11, campW - 4);
    int tentH = std::min(8, campH - 5);

    tentW = std::max(8, tentW);
    tentH = std::max(6, tentH);

    tentW = std::min(tentW, std::max(6, campW - 4));
    tentH = std::min(tentH, std::max(5, campH - 4));

    const int tentX = campX + 2;
    const int tentY = campY + 2;
    const int tentX2 = tentX + tentW - 1;
    const int tentY2 = tentY + tentH - 1;

    carveRect(d, tentX, tentY, tentW, tentH, TileType::Floor);

    for (int x = tentX; x <= tentX2; ++x) {
        if (d.inBounds(x, tentY)) d.at(x, tentY).type = TileType::Wall;
        if (d.inBounds(x, tentY2)) d.at(x, tentY2).type = TileType::Wall;
    }
    for (int y = tentY; y <= tentY2; ++y) {
        if (d.inBounds(tentX, y)) d.at(tentX, y).type = TileType::Wall;
        if (d.inBounds(tentX2, y)) d.at(tentX2, y).type = TileType::Wall;
    }

    // Door: center of the south wall.
    Vec2i tentDoor{tentX + tentW / 2, tentY2};
    if (d.inBounds(tentDoor.x, tentDoor.y)) {
        d.at(tentDoor.x, tentDoor.y).type = TileType::DoorClosed;
        carveFloor(d, tentDoor.x, tentDoor.y + 1);
    }

    // Stash anchor in the tent interior (used by Game to place a persistent open chest).
    d.campStashSpot = {tentX + tentW / 2, tentY + tentH / 2};

    // ------------------------------------------------------------
    // Stairs: camp exit (<) and dungeon entrance (>) inside the yard.
    // ------------------------------------------------------------
    d.stairsUp = {2, 2};
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y) || (d.stairsUp.x >= campX && d.stairsUp.x <= campX2 && d.stairsUp.y >= campY && d.stairsUp.y <= campY2)) {
        // Fallback: left edge above the camp.
        d.stairsUp = {2, std::max(2, campY - 2)};
    }

    if (d.inBounds(d.stairsUp.x, d.stairsUp.y)) {
        carveFloor(d, d.stairsUp.x, d.stairsUp.y);
        d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;
    }

    d.stairsDown = {campX2 - 2, campY + campH / 2};
    d.stairsDown.x = clampi(d.stairsDown.x, campX + 1, campX2 - 1);
    d.stairsDown.y = clampi(d.stairsDown.y, campY + 1, campY2 - 1);

    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        carveFloor(d, d.stairsDown.x, d.stairsDown.y);
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }

    // ------------------------------------------------------------
    // Decoration: sparse "trees" (pillars) outside the palisade to suggest wilderness.
    // Keep density low and validate connectivity between stairs.
    // ------------------------------------------------------------
    auto isInCampBounds = [&](Vec2i p) {
        return p.x >= campX && p.x <= campX2 && p.y >= campY && p.y <= campY2;
    };

    const int interior = std::max(0, (d.width - 2) * (d.height - 2));
    const int targetTrees = std::min(120, std::max(8, interior / 80)); // ~1.25% of tiles, capped.
    std::vector<Vec2i> trees;
    trees.reserve(static_cast<size_t>(targetTrees));

    auto canPlaceTree = [&](Vec2i p) {
        if (!d.inBounds(p.x, p.y)) return false;
        if (p == d.stairsUp || p == d.stairsDown) return false;
        if (chebyshev(p, d.stairsUp) <= 3) return false;
        if (chebyshev(p, d.stairsDown) <= 3) return false;
        if (isInCampBounds(p)) return false; // keep the yard clear
        if (d.at(p.x, p.y).type != TileType::Floor) return false;
        return true;
    };

    for (int tries = 0; tries < targetTrees * 6 && static_cast<int>(trees.size()) < targetTrees; ++tries) {
        Vec2i p{rng.range(1, d.width - 2), rng.range(1, d.height - 2)};
        if (!canPlaceTree(p)) continue;
        d.at(p.x, p.y).type = TileType::Pillar;
        trees.push_back(p);
    }

    // Connectivity check: ensure a walkable path from the surface exit to the dungeon entrance.
    auto dist = bfsDistanceMap(d, d.stairsUp);
    auto idx = static_cast<size_t>(d.stairsDown.y * d.width + d.stairsDown.x);
    if (idx >= dist.size() || dist[idx] < 0) {
        // Too many trees in a small map can block; clear them (cheap + deterministic fallback).
        for (const auto& p : trees) {
            if (d.inBounds(p.x, p.y) && d.at(p.x, p.y).type == TileType::Pillar) {
                d.at(p.x, p.y).type = TileType::Floor;
            }
        }
    }

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


void generateSokoban(Dungeon& d, RNG& rng, int depth) {
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


void generateSanctum(Dungeon& d, RNG& rng, int depth) {
    (void)rng;
    (void)depth; // reserved for future per-depth theming
    fillWalls(d);

    // Open the interior: the final floor is an arena-like layout with a central locked sanctum.
    carveRect(d, 1, 1, d.width - 2, d.height - 2, TileType::Floor);

    const int cx = d.width / 2;
    const int cy = d.height / 2;

    // Central sanctum (walled chamber) with a locked door and a chasm moat.
    const int wallW = 13;
    const int wallH = 9;
    int wallX = clampi(cx - wallW / 2, 4, d.width - wallW - 4);
    int wallY = clampi(cy - wallH / 2, 4, d.height - wallH - 4);

    for (int y = wallY; y < wallY + wallH; ++y) {
        for (int x = wallX; x < wallX + wallW; ++x) {
            if (d.inBounds(x, y)) d.at(x, y).type = TileType::Wall;
        }
    }

    carveRect(d, wallX + 1, wallY + 1, wallW - 2, wallH - 2, TileType::Floor);

    // Locked door on the north wall.
    const int doorX = wallX + wallW / 2;
    const int doorY = wallY;
    if (d.inBounds(doorX, doorY)) d.at(doorX, doorY).type = TileType::DoorLocked;

    // Moat ring (1 tile away from the sanctum wall).
    const int moatX = wallX - 1;
    const int moatY = wallY - 1;
    const int moatW = wallW + 2;
    const int moatH = wallH + 2;

    auto setChasm = [&](int x, int y) {
        if (!d.inBounds(x, y)) return;
        // Don't overwrite the sanctum walls or the upstairs.
        TileType t = d.at(x, y).type;
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

    // Bridges across the moat (keep the entrance obvious, with extra flank bridges).
    if (d.inBounds(doorX, doorY - 1)) d.at(doorX, doorY - 1).type = TileType::Floor;
    if (d.inBounds(doorX, doorY + wallH)) d.at(doorX, doorY + wallH).type = TileType::Floor;
    if (d.inBounds(wallX - 1, cy)) d.at(wallX - 1, cy).type = TileType::Floor;
    if (d.inBounds(wallX + wallW, cy)) d.at(wallX + wallW, cy).type = TileType::Floor;

    // Pillars inside the sanctum for cover and to make knockback fights more interesting.
    const int ix0 = wallX + 2;
    const int ix1 = wallX + wallW - 3;
    const int iy0 = wallY + 2;
    const int iy1 = wallY + wallH - 3;
    const Vec2i sanctumPillars[] = {
        {ix0, iy0}, {ix1, iy0},
        {ix0, iy1}, {ix1, iy1},
        {cx - 1, cy}, {cx + 1, cy},
    };
    for (const auto& p : sanctumPillars) {
        if (!d.inBounds(p.x, p.y)) continue;
        if (d.at(p.x, p.y).type == TileType::Floor) d.at(p.x, p.y).type = TileType::Pillar;
    }

    // A few arena pillars outside the moat (symmetrical-ish).
    const Vec2i hallPillars[] = {
        {cx - 10, cy - 4}, {cx + 10, cy - 4},
        {cx - 10, cy + 4}, {cx + 10, cy + 4},
        {cx - 12, cy}, {cx + 12, cy},
    };
    for (const auto& p : hallPillars) {
        if (!d.inBounds(p.x, p.y)) continue;
        if (d.at(p.x, p.y).type == TileType::Floor) d.at(p.x, p.y).type = TileType::Pillar;
    }

    // Define rooms (for spawns and room-type mechanics).
    d.rooms.clear();

    // Start room around the upstairs.
    const int sx = 2;
    const int sy = 2;
    const int sw = 8;
    const int sh = 6;
    d.rooms.push_back({sx, sy, sw, sh, RoomType::Normal});

    // A "last chance" shrine alcove (extra healing/utility before the sanctum).
    const int rx = d.width - 10;
    const int ry = 2;
    const int rw = 8;
    const int rh = 6;
    d.rooms.push_back({rx, ry, rw, rh, RoomType::Shrine});

    // A guard staging area (more monsters can spawn here).
    const int gx = 2;
    const int gy = d.height - 8;
    const int gw = 8;
    const int gh = 6;
    d.rooms.push_back({gx, gy, gw, gh, RoomType::Normal});

    // The sanctum interior is the treasure room.
    d.rooms.push_back({wallX + 1, wallY + 1, wallW - 2, wallH - 2, RoomType::Treasure});

    // Stairs.
    d.stairsUp = {sx + sw / 2, sy + sh / 2};
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.stairsUp = {1, 1};

    // No downstairs on the final floor.
    d.stairsDown = {-1, -1};
}

} // namespace

Dungeon::Dungeon(int w, int h) : width(w), height(h) {
    tiles.resize(static_cast<size_t>(width * height));
}

bool Dungeon::isWalkable(int x, int y) const {
    if (!inBounds(x, y)) return false;
    TileType t = at(x, y).type;
    return (t == TileType::Floor || t == TileType::DoorOpen || t == TileType::StairsDown || t == TileType::StairsUp);
}

bool Dungeon::isPassable(int x, int y) const {
    if (!inBounds(x, y)) return false;
    TileType t = at(x, y).type;
    // Note: locked doors are NOT passable for pathing/AI until unlocked.
    return (t == TileType::Floor || t == TileType::DoorOpen || t == TileType::DoorClosed || t == TileType::StairsDown || t == TileType::StairsUp);
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

void Dungeon::generate(RNG& rng, int depth, int maxDepth) {
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

    secretShortcutCount = 0;
    lockedShortcutCount = 0;
    corridorHubCount = 0;
    corridorHallCount = 0;
    sinkholeCount = 0;
    vaultSuiteCount = 0;
    deadEndClosetCount = 0;
    hasCavernLake = false;
    hasWarrens = false;

    // Sanity clamp.
    if (maxDepth < 1) maxDepth = 1;

    // Surface camp (depth 0): above-ground hub level.
    if (depth <= 0) {
        generateSurfaceCamp(*this, rng);
        ensureBorders(*this);

        // Final safety: ensure stair tiles survive any later carving/decoration overlap.
        if (inBounds(stairsUp.x, stairsUp.y)) at(stairsUp.x, stairsUp.y).type = TileType::StairsUp;
        if (inBounds(stairsDown.x, stairsDown.y)) at(stairsDown.x, stairsDown.y).type = TileType::StairsDown;
        return;
    }

    // Final floor: a bespoke arena-like sanctum that caps the run.
    if (depth >= maxDepth) {
        generateSanctum(*this, rng, depth);
        ensureBorders(*this);

    // Final safety: ensure stair tiles survive any later carving/decoration overlap.
    if (inBounds(stairsUp.x, stairsUp.y)) at(stairsUp.x, stairsUp.y).type = TileType::StairsUp;
    if (depth < maxDepth && inBounds(stairsDown.x, stairsDown.y)) at(stairsDown.x, stairsDown.y).type = TileType::StairsDown;
        return;
    }

    // Penultimate floor: a bespoke labyrinth that ramps tension before the sanctum.
    // (Hard-coded so the run has a consistent "final approach" feel.)
    if (maxDepth >= 2 && depth == maxDepth - 1) {
        generateLabyrinth(*this, rng, depth);
        ensureBorders(*this);
        return;
    }

    // Sokoban-inspired puzzle floor (early-mid game).
    // Keep it at a fixed depth so players learn to recognize the "boulder -> chasm" bridge mechanic.
    if (depth == SOKOBAN_DEPTH) {
        generateSokoban(*this, rng, depth);
        ensureBorders(*this);
        return;
    }

    // Rogue homage floor (mid-run): classic 3x3-room layout with doorless corridors.
    // This deliberately changes the tactical texture vs. door-heavy BSP floors.
    if (depth == ROGUE_LEVEL_DEPTH) {
        generateRogueLevel(*this, rng, depth);
        ensureBorders(*this);
        return;
    }

    fillWalls(*this);

    // Choose a generation style (rooms vs caverns vs mazes) and build the base layout.
    GenKind g = chooseGenKind(depth, maxDepth, rng);
    switch (g) {
        case GenKind::Cavern:     generateCavern(*this, rng, depth); break;
        case GenKind::Maze:       generateMaze(*this, rng, depth); break;
        case GenKind::Warrens:    generateWarrens(*this, rng, depth); break;
        case GenKind::Mines:      generateMines(*this, rng, depth); break;
        case GenKind::Catacombs:  generateCatacombs(*this, rng, depth); break;
        case GenKind::RoomsGraph: generateRoomsGraph(*this, rng, depth); break;
        case GenKind::RoomsBsp:
        default:
            generateBspRooms(*this, rng);
            break;
    }

    // Optional global fissure/ravine terrain feature.
    // This is a late pass on the base layout (stairs already placed) and is always
    // repaired/rolled back if it would disconnect stairs.
    (void)maybeCarveGlobalRavine(*this, rng, depth);

    // Cavern floors: carve a blobby subterranean lake (chasm) and auto-repair connectivity with causeways.
    (void)maybeCarveCavernLake(*this, rng, depth, g == GenKind::Cavern);

    // Mark special rooms after stairs are placed so we can avoid start/end rooms when possible.
    markSpecialRooms(*this, rng, depth);

    // Optional hidden/locked treasure side rooms.
    // These never affect critical connectivity (stairs already placed).
    float pSecret = 0.30f;
    float pVault = 0.22f;
    if (depth >= 6) {
        const float t = static_cast<float>(depth - 5);
        pSecret = std::min(0.55f, pSecret + 0.03f * t);
        pVault = std::min(0.45f, pVault + 0.03f * t);
    }
    if (rng.chance(pSecret)) (void)tryCarveSecretRoom(*this, rng, depth);
    if (rng.chance(pVault)) (void)tryCarveVaultRoom(*this, rng, depth);

    // Room shape variety: carve internal wall partitions / alcoves in some normal rooms.
    addRoomShapeVariety(*this, rng, depth);

    // Structural decoration pass: add interior columns/chasm features that
    // change combat geometry and line-of-sight without breaking the critical
    // stairs path.
    decorateRooms(*this, rng, depth);

    // Themed rooms (armory/library/lab) get bespoke interior prefabs too.
    decorateThemedRooms(*this, rng, depth);

    // Corridor polish pass: widen a few hallway junctions/segments into small hubs/great halls.
    // This only applies to room/corridor driven generators.
    (void)maybeCarveCorridorHubsAndHalls(*this, rng, depth, (g == GenKind::RoomsBsp || g == GenKind::RoomsGraph || g == GenKind::Mines));

    // Non-room layouts (caverns/mazes) still benefit from a bit of movable terrain.
    if (rooms.empty()) {
        (void)scatterBoulders(*this, rng, depth);
    }

    // Secret shortcut doors: hidden doors that connect two adjacent corridor regions
    // separated by a single wall tile. Adds optional loops/shortcuts without risking
    // disconnected floor pockets.
    (void)maybePlaceSecretShortcuts(*this, rng, depth);

    // Locked shortcut gates: visible locked doors that connect adjacent corridor regions
    // (already connected elsewhere), creating optional key/lockpick-powered shortcuts.
    (void)maybePlaceLockedShortcuts(*this, rng, depth, (g == GenKind::RoomsBsp || g == GenKind::RoomsGraph || g == GenKind::Maze || g == GenKind::Warrens || g == GenKind::Mines || g == GenKind::Catacombs));

    // Sinkholes: carve small chasm clusters in corridors to create local navigation puzzles.
    // This pass protects a core stairs path and rolls back if it would break connectivity.
    (void)maybeCarveSinkholes(*this, rng, depth, (g == GenKind::RoomsBsp || g == GenKind::RoomsGraph || g == GenKind::Warrens || g == GenKind::Mines || g == GenKind::Catacombs));

    // Dead-end stash closets: carve tiny side closets off corridor/tunnel dead ends.
    // These are optional rewards and never gate main progression.
    (void)maybeCarveDeadEndClosets(*this, rng, depth, g);

    ensureBorders(*this);

    // Final safety: ensure stair tiles survive any later carving/decoration overlap.
    if (inBounds(stairsUp.x, stairsUp.y)) at(stairsUp.x, stairsUp.y).type = TileType::StairsUp;
    if (depth < maxDepth && inBounds(stairsDown.x, stairsDown.y)) at(stairsDown.x, stairsDown.y).type = TileType::StairsDown;
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

std::vector<int> Dungeon::computeSoundMap(int sx, int sy, int maxCost) const {
    std::vector<int> dist(static_cast<size_t>(width * height), -1);
    if (maxCost < 0) return dist;
    if (!inBounds(sx, sy)) return dist;

    auto soundPassable = [&](int x, int y) -> bool {
        if (!inBounds(x, y)) return false;
        const TileType t = at(x, y).type;
        // Walls, pillars, and secret doors completely block sound propagation.
        return (t != TileType::Wall && t != TileType::Pillar && t != TileType::DoorSecret);
    };

    auto tileCost = [&](int x, int y) -> int {
        if (!inBounds(x, y)) return 1000000000;
        const TileType t = at(x, y).type;
        // Closed/locked doors muffle sound more than open spaces.
        switch (t) {
            case TileType::DoorClosed: return 2;
            case TileType::DoorLocked: return 3;
            default: return 1;
        }
    };

    if (!soundPassable(sx, sy)) return dist;

    auto idx = [&](int x, int y) -> int { return y * width + x; };

    using Node = std::pair<int, int>; // (cost, index)
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

    const int startI = idx(sx, sy);
    dist[static_cast<size_t>(startI)] = 0;
    pq.push({0, startI});

    static const int DIRS8[8][2] = {
        {1,0},{-1,0},{0,1},{0,-1},
        {1,1},{1,-1},{-1,1},{-1,-1}
    };

    while (!pq.empty()) {
        const Node cur = pq.top();
        pq.pop();

        const int costHere = cur.first;
        const int i = cur.second;
        if (costHere < 0) continue;
        if (costHere > maxCost) continue;
        if (dist[static_cast<size_t>(i)] != costHere) continue;

        const int x = i % width;
        const int y = i / width;

        for (const auto& dv : DIRS8) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!inBounds(nx, ny)) continue;
            if (!soundPassable(nx, ny)) continue;

            // Prevent diagonal "corner cutting" through two blocking tiles.
            if (dv[0] != 0 && dv[1] != 0) {
                const int ax = x + dv[0];
                const int ay = y;
                const int bx = x;
                const int by = y + dv[1];
                // For sound, use soundPassable (not walkable) because closed doors still transmit sound.
                const bool aPass = soundPassable(ax, ay);
                const bool bPass = soundPassable(bx, by);
                if (!aPass && !bPass) continue;
            }

            const int step = tileCost(nx, ny);
            if (step <= 0) continue;
            const int ncost = costHere + step;
            if (ncost > maxCost) continue;

            const int ni = idx(nx, ny);
            int& slot = dist[static_cast<size_t>(ni)];
            if (slot < 0 || ncost < slot) {
                slot = ncost;
                pq.push({ncost, ni});
            }
        }
    }

    return dist;
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
    outMask.assign(static_cast<size_t>(width * height), 0);
    if (!inBounds(px, py)) return;

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * width + x); };

    auto isOpaqueTile = [&](int x, int y) {
        if (!inBounds(x, y)) return true;
        TileType tt = at(x, y).type;
        return (tt == TileType::Wall || tt == TileType::Pillar || tt == TileType::DoorClosed || tt == TileType::DoorLocked || tt == TileType::DoorSecret);
    };

    auto markVis = [&](int x, int y) {
        if (!inBounds(x, y)) return;
        outMask[idx(x, y)] = 1;
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
