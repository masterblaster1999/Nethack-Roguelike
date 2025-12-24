#include "dungeon.hpp"
#include <algorithm>
#include <cmath>

namespace {
struct Room {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    int x2() const { return x + w; }
    int y2() const { return y + h; }
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }

    bool intersects(const Room& o) const {
        return !(x2() <= o.x || o.x2() <= x || y2() <= o.y || o.y2() <= y);
    }
};
} // namespace

void Dungeon::resize(int w, int h) {
    width = w;
    height = h;
    tiles.assign(static_cast<size_t>(w * h), Tile{TileType::Wall, false, false});
    stairsDown = {-1, -1};
}

bool Dungeon::inBounds(int x, int y) const {
    return x >= 0 && y >= 0 && x < width && y < height;
}

Tile& Dungeon::at(int x, int y) {
    return tiles[static_cast<size_t>(y * width + x)];
}

const Tile& Dungeon::at(int x, int y) const {
    return tiles[static_cast<size_t>(y * width + x)];
}

bool Dungeon::isWalkable(int x, int y) const {
    if (!inBounds(x, y)) return false;
    TileType t = at(x, y).type;
    return t == TileType::Floor || t == TileType::StairsDown;
}

bool Dungeon::isOpaque(int x, int y) const {
    if (!inBounds(x, y)) return true;
    return at(x, y).type == TileType::Wall;
}

static void carveRect(Dungeon& d, int x, int y, int w, int h) {
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            if (d.inBounds(xx, yy)) {
                d.at(xx, yy).type = TileType::Floor;
            }
        }
    }
}

static void carveH(Dungeon& d, int x1, int x2, int y) {
    if (x2 < x1) std::swap(x1, x2);
    for (int x = x1; x <= x2; ++x) {
        if (d.inBounds(x, y)) d.at(x, y).type = TileType::Floor;
    }
}

static void carveV(Dungeon& d, int y1, int y2, int x) {
    if (y2 < y1) std::swap(y1, y2);
    for (int y = y1; y <= y2; ++y) {
        if (d.inBounds(x, y)) d.at(x, y).type = TileType::Floor;
    }
}

void Dungeon::generate(RNG& rng, int roomAttempts) {
    // Initialize as solid wall.
    for (auto& t : tiles) {
        t.type = TileType::Wall;
        t.visible = false;
        t.explored = false;
    }
    stairsDown = {-1, -1};

    std::vector<Room> rooms;
    rooms.reserve(32);

    const int minW = 4, maxW = 10;
    const int minH = 4, maxH = 8;
    const int padding = 1;

    for (int i = 0; i < roomAttempts; ++i) {
        int w = rng.range(minW, maxW);
        int h = rng.range(minH, maxH);

        int x = rng.range(1, std::max(1, width - w - 2));
        int y = rng.range(1, std::max(1, height - h - 2));

        Room r{ x, y, w, h };

        // Inflate for spacing check.
        Room inflated{ x - padding, y - padding, w + 2 * padding, h + 2 * padding };

        bool ok = true;
        for (const auto& o : rooms) {
            if (inflated.intersects(o)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        carveRect(*this, r.x, r.y, r.w, r.h);

        if (!rooms.empty()) {
            // Connect to previous room with a corridor.
            const Room& prev = rooms.back();
            int x1 = prev.cx(), y1 = prev.cy();
            int x2 = r.cx(), y2 = r.cy();

            if (rng.chance(0.5f)) {
                carveH(*this, x1, x2, y1);
                carveV(*this, y1, y2, x2);
            } else {
                carveV(*this, y1, y2, x1);
                carveH(*this, x1, x2, y2);
            }
        }

        rooms.push_back(r);
        if (static_cast<int>(rooms.size()) >= 24) break;
    }

    if (rooms.empty()) {
        // Fallback: carve a simple central room.
        Room r{ width / 4, height / 4, width / 2, height / 2 };
        carveRect(*this, r.x, r.y, r.w, r.h);
        rooms.push_back(r);
    }

    // Add stairs at the last room center.
    {
        const Room& last = rooms.back();
        stairsDown = { last.cx(), last.cy() };
        if (inBounds(stairsDown.x, stairsDown.y)) {
            at(stairsDown.x, stairsDown.y).type = TileType::StairsDown;
        }
    }

    // Ensure border stays walls (looks nicer, prevents weird edge cases).
    for (int x = 0; x < width; ++x) {
        at(x, 0).type = TileType::Wall;
        at(x, height - 1).type = TileType::Wall;
    }
    for (int y = 0; y < height; ++y) {
        at(0, y).type = TileType::Wall;
        at(width - 1, y).type = TileType::Wall;
    }
}

bool Dungeon::lineOfSight(int x0, int y0, int x1, int y1) const {
    // Bresenham line (integer grid). Endpoint is always considered visible.
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0;
    int y = y0;

    while (true) {
        if (x == x1 && y == y1) return true;

        if (!(x == x0 && y == y0)) {
            if (isOpaque(x, y)) return false;
        }

        int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }

        if (!inBounds(x, y)) return false;
    }
}

void Dungeon::computeFov(int px, int py, int radius) {
    // Clear visible flags.
    for (auto& t : tiles) t.visible = false;

    if (!inBounds(px, py)) return;

    const int r2 = radius * radius;
    for (int y = py - radius; y <= py + radius; ++y) {
        for (int x = px - radius; x <= px + radius; ++x) {
            if (!inBounds(x, y)) continue;
            int dx = x - px;
            int dy = y - py;
            if (dx * dx + dy * dy > r2) continue;

            if (lineOfSight(px, py, x, y)) {
                Tile& t = at(x, y);
                t.visible = true;
                t.explored = true;
            }
        }
    }

    // Always see your own tile.
    Tile& self = at(px, py);
    self.visible = true;
    self.explored = true;
}
