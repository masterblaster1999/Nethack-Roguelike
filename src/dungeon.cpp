#include "dungeon.hpp"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <functional>

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
    return (t == TileType::Wall || t == TileType::DoorClosed || t == TileType::DoorLocked || t == TileType::DoorSecret);
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

void Dungeon::generate(RNG& rng) {
    fillWalls(*this);

    // BSP parameters tuned for small-ish maps.
    const int minLeaf = 8;
    std::vector<Leaf> nodes;
    nodes.reserve(64);
    nodes.push_back({1, 1, width - 2, height - 2, -1, -1, -1});

    // Split leaves recursively (iterative stack).
    std::vector<int> stack;
    stack.push_back(0);

    while (!stack.empty()) {
        int idx = stack.back();
        stack.pop_back();

        Leaf& n = nodes[static_cast<size_t>(idx)];

        // Stop if too small.
        if (n.w < minLeaf * 2 && n.h < minLeaf * 2) continue;

        bool splitVert = false;
        if (n.w > n.h) splitVert = true;
        else if (n.h > n.w) splitVert = false;
        else splitVert = rng.chance(0.5f);

        if (splitVert) {
            if (n.w < minLeaf * 2) continue;
            int split = rng.range(minLeaf, n.w - minLeaf);
            Leaf a{ n.x, n.y, split, n.h };
            Leaf b{ n.x + split, n.y, n.w - split, n.h };
            n.left = static_cast<int>(nodes.size());
            nodes.push_back(a);
            n.right = static_cast<int>(nodes.size());
            nodes.push_back(b);
        } else {
            if (n.h < minLeaf * 2) continue;
            int split = rng.range(minLeaf, n.h - minLeaf);
            Leaf a{ n.x, n.y, n.w, split };
            Leaf b{ n.x, n.y + split, n.w, n.h - split };
            n.left = static_cast<int>(nodes.size());
            nodes.push_back(a);
            n.right = static_cast<int>(nodes.size());
            nodes.push_back(b);
        }

        // Depth-ish control: random chance to keep splitting.
        if (n.left >= 0 && n.right >= 0) {
            // Push children for splitting.
            stack.push_back(n.left);
            stack.push_back(n.right);
        }
    }

    // Create rooms in leaf nodes.
    const int minRoomW = 4;
    const int minRoomH = 4;

    for (auto& n : nodes) {
        if (!isLeaf(n)) continue;

        int maxRoomW = std::max(minRoomW, n.w - 2);
        int maxRoomH = std::max(minRoomH, n.h - 2);

        int rw = rng.range(minRoomW, maxRoomW);
        int rh = rng.range(minRoomH, maxRoomH);

        int rx = n.x + rng.range(1, std::max(1, n.w - rw - 1));
        int ry = n.y + rng.range(1, std::max(1, n.h - rh - 1));

        Room r;
        r.x = rx;
        r.y = ry;
        r.w = rw;
        r.h = rh;
        r.type = RoomType::Normal;

        int rIndex = static_cast<int>(rooms.size());
        rooms.push_back(r);
        n.roomIndex = rIndex;

        carveRect(*this, rx, ry, rw, rh, TileType::Floor);
    }

    if (rooms.empty()) {
        // Fallback: carve a simple central room.
        Room r{ width / 4, height / 4, width / 2, height / 2, RoomType::Normal };
        rooms.push_back(r);
        carveRect(*this, r.x, r.y, r.w, r.h, TileType::Floor);
    }

    // Connect rooms following the BSP tree.
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        Leaf& n = nodes[static_cast<size_t>(i)];
        if (n.left < 0 || n.right < 0) continue;

        int ra = pickRandomRoomInSubtree(nodes, n.left, rng);
        int rb = pickRandomRoomInSubtree(nodes, n.right, rng);
        if (ra >= 0 && rb >= 0 && ra != rb) {
            connectRooms(*this, rooms[static_cast<size_t>(ra)], rooms[static_cast<size_t>(rb)], rng);
        }
    }

    // Extra loops: connect random room pairs.
    const int extra = std::max(1, static_cast<int>(rooms.size()) / 3);
    for (int i = 0; i < extra; ++i) {
        int a = rng.range(0, static_cast<int>(rooms.size()) - 1);
        int b = rng.range(0, static_cast<int>(rooms.size()) - 1);
        if (a == b) continue;
        connectRooms(*this, rooms[static_cast<size_t>(a)], rooms[static_cast<size_t>(b)], rng);
    }

    // Mark some special rooms (if enough rooms exist).
    if (rooms.size() >= 3) {
        int treasure = rng.range(0, static_cast<int>(rooms.size()) - 1);
        int lair = rng.range(0, static_cast<int>(rooms.size()) - 1);
        int shrine = rng.range(0, static_cast<int>(rooms.size()) - 1);

        // Ensure distinct
        for (int guard = 0; guard < 50 && (lair == treasure); ++guard) lair = rng.range(0, static_cast<int>(rooms.size()) - 1);
        for (int guard = 0; guard < 50 && (shrine == treasure || shrine == lair); ++guard) shrine = rng.range(0, static_cast<int>(rooms.size()) - 1);

        rooms[static_cast<size_t>(treasure)].type = RoomType::Treasure;
        rooms[static_cast<size_t>(lair)].type = RoomType::Lair;
        rooms[static_cast<size_t>(shrine)].type = RoomType::Shrine;
    }

    // Precompute which tiles are inside rooms (for branch carving).
    std::vector<uint8_t> inRoom(static_cast<size_t>(width * height), 0);
    for (const auto& r : rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (inBounds(x, y)) inRoom[static_cast<size_t>(y * width + x)] = 1;
            }
        }
    }

    // Branch corridors (dead ends)
    int branches = std::max(2, static_cast<int>(rooms.size()));
    for (int i = 0; i < branches; ++i) {
        int x = rng.range(1, width - 2);
        int y = rng.range(1, height - 2);

        if (!inBounds(x, y)) continue;
        if (at(x, y).type != TileType::Floor) continue;
        if (inRoom[static_cast<size_t>(y * width + x)] != 0) continue; // prefer corridors

        const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        int dIdx = rng.range(0, 3);
        int dx = dirs[dIdx][0];
        int dy = dirs[dIdx][1];

        int nx = x + dx;
        int ny = y + dy;
        if (!inBounds(nx, ny)) continue;
        if (at(nx, ny).type != TileType::Wall) continue; // needs to dig into wall

        int len = rng.range(3, 8);
        int cx = x;
        int cy = y;
        for (int step = 0; step < len; ++step) {
            cx += dx;
            cy += dy;
            if (!inBounds(cx, cy)) break;
            if (at(cx, cy).type != TileType::Wall) break;
            carveFloor(*this, cx, cy);
        }
    }

    // Place stairs: up in the first room, down in the farthest room by BFS.
    const Room& startRoom = rooms.front();
    stairsUp = { startRoom.cx(), startRoom.cy() };
    if (inBounds(stairsUp.x, stairsUp.y)) {
        at(stairsUp.x, stairsUp.y).type = TileType::StairsUp;
    }

    auto dist = bfsDistanceMap(*this, stairsUp);
    int bestRoomIdx = 0;
    int bestDist = -1;
    for (int i = 0; i < static_cast<int>(rooms.size()); ++i) {
        const Room& r = rooms[static_cast<size_t>(i)];
        int cx = r.cx();
        int cy = r.cy();
        if (!inBounds(cx, cy)) continue;
        int d0 = dist[static_cast<size_t>(cy * width + cx)];
        if (d0 > bestDist) {
            bestDist = d0;
            bestRoomIdx = i;
        }
    }
    const Room& endRoom = rooms[static_cast<size_t>(bestRoomIdx)];
    stairsDown = { endRoom.cx(), endRoom.cy() };
    if (inBounds(stairsDown.x, stairsDown.y)) {
        at(stairsDown.x, stairsDown.y).type = TileType::StairsDown;
    }

    // ------------------------------------------------------------
    // Optional secret rooms (post-stairs so we don't accidentally make stairs
    // spawn behind a secret door).
    // ------------------------------------------------------------
    {
        int want = 1;
        if (rng.chance(0.50f)) want++;
        // Keep tries bounded; not all maps have enough wall space.
        int carved = 0;
        for (int i = 0; i < want; ++i) {
            if (tryCarveSecretRoom(*this, rng)) carved++;
        }
        (void)carved;
    }

    // ------------------------------------------------------------
    // Optional vault rooms: visible but locked side rooms.
    // These never affect critical connectivity (stairs already placed).
    // ------------------------------------------------------------
    {
        int want = 0;
        if (rng.chance(0.55f)) want = 1;
        if (rng.chance(0.18f)) want++;

        int carved = 0;
        for (int i = 0; i < want; ++i) {
            if (tryCarveVaultRoom(*this, rng)) carved++;
        }
        (void)carved;
    }

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

void Dungeon::computeFov(int px, int py, int radius) {
    // Reset visibility each frame
    for (auto& t : tiles) t.visible = false;
    if (!inBounds(px, py)) return;

    auto isOpaqueTile = [&](int x, int y) {
        if (!inBounds(x, y)) return true;
        TileType tt = at(x, y).type;
        return (tt == TileType::Wall || tt == TileType::DoorClosed || tt == TileType::DoorLocked || tt == TileType::DoorSecret);
    };

    auto markVis = [&](int x, int y) {
        if (!inBounds(x, y)) return;
        Tile& t = at(x, y);
        t.visible = true;
        t.explored = true;
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
