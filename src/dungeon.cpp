#include "dungeon.hpp"
#include <algorithm>
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
    t.type = TileType::Floor;
}

// ------------------------------------------------------------
// Secret rooms: optional side-rooms hidden behind secret doors.
// These do NOT affect critical connectivity (stairs remain reachable).
// ------------------------------------------------------------

bool tryCarveSecretRoom(Dungeon& d, RNG& rng) {
    // Pick a wall tile adjacent to floor, then carve a small room behind it.
    // Door stays hidden (TileType::DoorSecret) until discovered via searching.
    const int maxTries = 350;

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    for (int tries = 0; tries < maxTries; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;

        // Randomize direction check order for variety.
        int start = rng.range(0, 3);
        for (int i = 0; i < 4; ++i) {
            const Vec2i dir = dirs[(start + i) % 4];

            const int fx = x + dir.x;
            const int fy = y + dir.y;
            if (!d.inBounds(fx, fy)) continue;
            if (d.at(fx, fy).type != TileType::Floor) continue; // must attach to existing floor

            // Room extends opposite the floor neighbor.
            const int dx = -dir.x;
            const int dy = -dir.y;

            const int rw = rng.range(3, 6);
            const int rh = rng.range(3, 6);

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

bool tryCarveVaultRoom(Dungeon& d, RNG& rng) {
    const int maxTries = 350;

    const Vec2i dirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

    for (int tries = 0; tries < maxTries; ++tries) {
        const int x = rng.range(2, d.width - 3);
        const int y = rng.range(2, d.height - 3);

        if (!d.inBounds(x, y)) continue;
        if (d.at(x, y).type != TileType::Wall) continue;

        // Randomize direction check order for variety.
        int start = rng.range(0, 3);
        for (int i = 0; i < 4; ++i) {
            const Vec2i dir = dirs[(start + i) % 4];

            const int fx = x + dir.x;
            const int fy = y + dir.y;
            if (!d.inBounds(fx, fy)) continue;
            if (d.at(fx, fy).type != TileType::Floor) continue; // must attach to existing floor

            // Room extends opposite the floor neighbor.
            const int dx = -dir.x;
            const int dy = -dir.y;

            // Vaults are a bit larger than secrets; they should feel like a "real" reward.
            const int rw = rng.range(4, 7);
            const int rh = rng.range(4, 7);

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

DoorPick pickDoorOnRoom(const Room& r, const Dungeon& d, RNG& rng) {
    // Try several times to find a door that doesn't immediately go out of bounds.
    for (int tries = 0; tries < 20; ++tries) {
        int side = rng.range(0, 3);
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

void connectRooms(Dungeon& d, const Room& a, const Room& b, RNG& rng) {
    DoorPick da = pickDoorOnRoom(a, d, rng);
    DoorPick db = pickDoorOnRoom(b, d, rng);

    // Place doors
    if (d.inBounds(da.doorInside.x, da.doorInside.y))
        d.at(da.doorInside.x, da.doorInside.y).type = TileType::DoorClosed;
    if (d.inBounds(db.doorInside.x, db.doorInside.y))
        d.at(db.doorInside.x, db.doorInside.y).type = TileType::DoorClosed;

    // Ensure corridor starts are floor
    carveFloor(d, da.corridorStart.x, da.corridorStart.y);
    carveFloor(d, db.corridorStart.x, db.corridorStart.y);

    // Carve L-shaped corridor
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
    if (!(cur.type == TileType::Floor || cur.type == TileType::Chasm || cur.type == TileType::Pillar)) {
        return;
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

bool decorateRoomChasm(Dungeon& d, const Room& r, RNG& rng) {
    // Only decorate sufficiently large rooms.
    if (r.w < 8 || r.h < 6) return false;

    std::vector<TileChange> changes;
    changes.reserve(48);

    const bool vertical = rng.chance(0.5f);
    if (vertical) {
        const int x = r.cx();
        // A vertical chasm line with a single bridge tile.
        const int bridgeY = rng.range(r.y + 2, r.y2() - 3);
        for (int y = r.y + 1; y < r.y2() - 1; ++y) {
            if (y == bridgeY) continue;
            trySetTile(d, x, y, TileType::Chasm, changes);
        }
    } else {
        const int y = r.cy();
        const int bridgeX = rng.range(r.x + 2, r.x2() - 3);
        for (int x = r.x + 1; x < r.x2() - 1; ++x) {
            if (x == bridgeX) continue;
            trySetTile(d, x, y, TileType::Chasm, changes);
        }
    }

    if (!stairsConnected(d)) {
        undoChanges(d, changes);
        return false;
    }
    return !changes.empty();
}

void decorateRooms(Dungeon& d, RNG& rng, int depth) {
    // Decoration pacing: more structural variation deeper.
    float pPillars = 0.18f;
    float pChasm = 0.10f;
    if (depth >= 3) { pPillars += 0.07f; pChasm += 0.06f; }
    if (depth >= 5) { pPillars += 0.08f; pChasm += 0.08f; }

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
            (void)decorateRoomChasm(d, r, rng);
        }
        if (rng.chance(pPillars)) {
            (void)decorateRoomPillars(d, r, rng);
        }
    }
}

enum class GenKind : uint8_t {
    RoomsBsp = 0,
    Cavern,
    Maze,
};

GenKind chooseGenKind(int depth, RNG& rng) {
    // The default run now spans 10 floors, so we can pace out variety:
    // - Early: classic rooms
    // - Mid: first cavern / first maze
    // - Deep: alternating cavern/maze spikes
    if (depth <= 3) return GenKind::RoomsBsp;
    if (depth == 4) return GenKind::Cavern;
    if (depth == 5) return GenKind::Maze;
    if (depth == 6) return GenKind::RoomsBsp;
    if (depth == 7) return GenKind::Cavern;
    if (depth == 8) return GenKind::Maze;
    if (depth == 9) return GenKind::RoomsBsp;

    // Beyond the intended run depth (e.g., in tests or future endless mode), sprinkle variety.
    float r = rng.next01();
    if (r < 0.22f) return GenKind::Maze;
    if (r < 0.52f) return GenKind::Cavern;
    return GenKind::RoomsBsp;
}


void markSpecialRooms(Dungeon& d, RNG& rng, int depth) {
    if (d.rooms.empty()) return;

    auto buildPool = [&](bool allowDown) {
        std::vector<int> pool;
        pool.reserve(d.rooms.size());
        for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
            const Room& r = d.rooms[static_cast<size_t>(i)];
            // Prefer leaving the start room "normal" so early turns are fair.
            if (r.contains(d.stairsUp.x, d.stairsUp.y)) continue;
            if (!allowDown && r.contains(d.stairsDown.x, d.stairsDown.y)) continue;
            // Only mark normal rooms.
            if (r.type != RoomType::Normal) continue;
            pool.push_back(i);
        }
        return pool;
    };

    auto pickAndRemove = [&](std::vector<int>& pool) -> int {
        if (pool.empty()) return -1;
        int j = rng.range(0, static_cast<int>(pool.size()) - 1);
        int v = pool[static_cast<size_t>(j)];
        pool[static_cast<size_t>(j)] = pool.back();
        pool.pop_back();
        return v;
    };

    std::vector<int> pool = buildPool(false);
    if (pool.empty()) pool = buildPool(true);
    if (pool.empty()) {
        pool.reserve(d.rooms.size());
        for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
            if (d.rooms[static_cast<size_t>(i)].type == RoomType::Normal) pool.push_back(i);
        }
    }

    // Treasure is the most important for gameplay pacing; lair/shrine are "nice to have".
    int t = pickAndRemove(pool);
    if (t >= 0) d.rooms[static_cast<size_t>(t)].type = RoomType::Treasure;

    // Deep floors can carry extra treasure to support a longer run.
    if (depth >= 7) {
        float extraTreasureChance = std::min(0.55f, 0.25f + 0.05f * static_cast<float>(depth - 7));
        if (rng.chance(extraTreasureChance)) {
            int t2 = pickAndRemove(pool);
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
    if (!pool.empty() && rng.chance(shopChance)) {
        int sh = pickAndRemove(pool);
        if (sh >= 0) d.rooms[static_cast<size_t>(sh)].type = RoomType::Shop;
    }

    int l = pickAndRemove(pool);
    if (l >= 0) d.rooms[static_cast<size_t>(l)].type = RoomType::Lair;

    int s = pickAndRemove(pool);
    if (s >= 0) d.rooms[static_cast<size_t>(s)].type = RoomType::Shrine;
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
    // BSP parameters tuned for small-ish maps.
    const int minLeaf = 8;

    std::vector<Leaf> nodes;
    nodes.reserve(128);

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

    // Connect rooms following the BSP tree.
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        Leaf& n = nodes[static_cast<size_t>(i)];
        if (n.left < 0 || n.right < 0) continue;

        int ra = pickRandomRoomInSubtree(nodes, n.left, rng);
        int rb = pickRandomRoomInSubtree(nodes, n.right, rng);
        if (ra >= 0 && rb >= 0 && ra != rb) {
            connectRooms(d, d.rooms[static_cast<size_t>(ra)], d.rooms[static_cast<size_t>(rb)], rng);
        }
    }

    // Extra loops: connect random room pairs.
    const int extra = std::max(1, static_cast<int>(d.rooms.size()) / 3);
    for (int i = 0; i < extra; ++i) {
        int a = rng.range(0, static_cast<int>(d.rooms.size()) - 1);
        int b = rng.range(0, static_cast<int>(d.rooms.size()) - 1);
        if (a == b) continue;
        connectRooms(d, d.rooms[static_cast<size_t>(a)], d.rooms[static_cast<size_t>(b)], rng);
    }

    // Precompute which tiles are inside rooms (for branch carving).
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (d.inBounds(x, y)) inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
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
        d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
    }
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
    d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.stairsDown = {d.width - 2, d.height - 2};
    d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;
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
    d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.stairsDown = {d.width - 2, d.height - 2};
    d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;

    // Sprinkle some closed doors in corridor chokepoints to make LOS + combat more interesting.
    std::vector<uint8_t> inRoom(static_cast<size_t>(d.width * d.height), 0);
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (d.inBounds(x, y)) inRoom[static_cast<size_t>(y * d.width + x)] = 1;
            }
        }
    }

    auto nearStairs = [&](int x, int y) {
        return (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y) <= 2)
            || (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y) <= 2);
    };

    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (inRoom[static_cast<size_t>(y * d.width + x)] != 0) continue;
            if (nearStairs(x, y)) continue;
            if (!rng.chance(0.035f)) continue;

            const TileType n = d.at(x, y - 1).type;
            const TileType s = d.at(x, y + 1).type;
            const TileType w = d.at(x - 1, y).type;
            const TileType e = d.at(x + 1, y).type;

            const bool nsOpen = (n == TileType::Floor && s == TileType::Floor);
            const bool weOpen = (w == TileType::Floor && e == TileType::Floor);
            const bool nsWall = (w == TileType::Wall && e == TileType::Wall);
            const bool weWall = (n == TileType::Wall && s == TileType::Wall);

            // Corridor segment between two walls.
            if ((nsOpen && nsWall) || (weOpen && weWall)) {
                d.at(x, y).type = TileType::DoorClosed;
            }
        }
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
    int bestDist = 1e9;
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
    if (bestDist >= 1000000000) {
        best = d.randomFloor(rng, true);
    }

    const int sw = rng.range(6, 8);
    const int sh = rng.range(5, 7);
    int sx = clampi(best.x - sw / 2, 1, d.width - sw - 1);
    int sy = clampi(best.y - sh / 2, 1, d.height - sh - 1);
    carveRect(d, sx, sy, sw, sh, TileType::Floor);
    d.stairsUp = { best.x, best.y };
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) d.stairsUp = {1, 1};
    d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;

    auto dist = bfsDistanceMap(d, d.stairsUp);
    d.stairsDown = farthestPassableTile(d, dist, rng);
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) d.stairsDown = {d.width - 2, d.height - 2};

    // Exit chamber around stairsDown.
    const int ew = rng.range(6, 9);
    const int eh = rng.range(5, 8);
    int ex = clampi(d.stairsDown.x - ew / 2, 1, d.width - ew - 1);
    int ey = clampi(d.stairsDown.y - eh / 2, 1, d.height - eh - 1);
    carveRect(d, ex, ey, ew, eh, TileType::Floor);
    d.at(d.stairsDown.x, d.stairsDown.y).type = TileType::StairsDown;

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

    auto nearStairs = [&](int x, int y) {
        return (std::abs(x - d.stairsUp.x) + std::abs(y - d.stairsUp.y) <= 2)
            || (std::abs(x - d.stairsDown.x) + std::abs(y - d.stairsDown.y) <= 2);
    };

    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (d.at(x, y).type != TileType::Floor) continue;
            if (inRoom[static_cast<size_t>(y * d.width + x)] != 0) continue;
            if (nearStairs(x, y)) continue;
            if (inMoatBounds(x, y)) continue;
            if (!rng.chance(0.055f)) continue;

            const TileType n = d.at(x, y - 1).type;
            const TileType s = d.at(x, y + 1).type;
            const TileType w = d.at(x - 1, y).type;
            const TileType e = d.at(x + 1, y).type;

            const bool nsOpen = (n == TileType::Floor && s == TileType::Floor);
            const bool weOpen = (w == TileType::Floor && e == TileType::Floor);
            const bool nsWall = (w == TileType::Wall && e == TileType::Wall);
            const bool weWall = (n == TileType::Wall && s == TileType::Wall);

            if ((nsOpen && nsWall) || (weOpen && weWall)) {
                d.at(x, y).type = TileType::DoorClosed;
            }
        }
    }
}


void generateSanctum(Dungeon& d, RNG& rng, int depth) {
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
    d.at(d.stairsUp.x, d.stairsUp.y).type = TileType::StairsUp;

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
        width = 30;
        height = 20;
    }
    const size_t expect = static_cast<size_t>(width * height);
    if (tiles.size() != expect) tiles.assign(expect, Tile{});

    // Sanity clamp.
    if (maxDepth < 1) maxDepth = 1;

    // Final floor: a bespoke arena-like sanctum that caps the run.
    if (depth >= maxDepth) {
        generateSanctum(*this, rng, depth);
        ensureBorders(*this);
        return;
    }

    // Penultimate floor: a bespoke labyrinth that ramps tension before the sanctum.
    // (Hard-coded so the run has a consistent "final approach" feel.)
    if (maxDepth >= 2 && depth == maxDepth - 1) {
        generateLabyrinth(*this, rng, depth);
        ensureBorders(*this);
        return;
    }

    fillWalls(*this);

    // Choose a generation style (rooms vs caverns vs mazes) and build the base layout.
    GenKind g = chooseGenKind(depth, rng);
    switch (g) {
        case GenKind::Cavern: generateCavern(*this, rng, depth); break;
        case GenKind::Maze:   generateMaze(*this, rng, depth); break;
        case GenKind::RoomsBsp:
        default:
            generateBspRooms(*this, rng);
            break;
    }

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
    if (rng.chance(pSecret)) (void)tryCarveSecretRoom(*this, rng);
    if (rng.chance(pVault)) (void)tryCarveVaultRoom(*this, rng);

    // Structural decoration pass: add interior columns/chasm features that
    // change combat geometry and line-of-sight without breaking the critical
    // stairs path.
    decorateRooms(*this, rng, depth);

    ensureBorders(*this);
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
