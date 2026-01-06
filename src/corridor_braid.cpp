#include "corridor_braid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <vector>

namespace {

inline bool isDoor(TileType t) {
    return t == TileType::DoorClosed || t == TileType::DoorOpen || t == TileType::DoorLocked || t == TileType::DoorSecret;
}

inline bool isStairs(TileType t) {
    return t == TileType::StairsUp || t == TileType::StairsDown;
}

bool nearStairs(const Dungeon& d, int x, int y, int dist) {
    const Vec2i p{x, y};
    if (d.inBounds(d.stairsUp.x, d.stairsUp.y) && ::manhattan(p, d.stairsUp) <= dist) return true;
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y) && ::manhattan(p, d.stairsDown) <= dist) return true;
    return false;
}

bool anyDoorInRadius(const Dungeon& d, int x, int y, int radius) {
    for (int oy = -radius; oy <= radius; ++oy) {
        for (int ox = -radius; ox <= radius; ++ox) {
            const int nx = x + ox;
            const int ny = y + oy;
            if (!d.inBounds(nx, ny)) continue;
            if (isDoor(d.at(nx, ny).type)) return true;
        }
    }
    return false;
}

bool adjacentToRoomMask(const std::vector<uint8_t>& inRoom, int W, int H, int x, int y) {
    auto idx = [&](int xx, int yy) -> size_t { return static_cast<size_t>(yy * W + xx); };
    static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (auto& dv : dirs4) {
        const int nx = x + dv[0];
        const int ny = y + dv[1];
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
        if (inRoom[idx(nx, ny)] != 0) return true;
    }
    return false;
}

bool adjacentToChasm(const Dungeon& d, int x, int y) {
    static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (auto& dv : dirs4) {
        const int nx = x + dv[0];
        const int ny = y + dv[1];
        if (!d.inBounds(nx, ny)) continue;
        if (d.at(nx, ny).type == TileType::Chasm) return true;
    }
    return false;
}

int passableDegree4(const Dungeon& d, int x, int y) {
    static const int dirs4[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    int deg = 0;
    for (auto& dv : dirs4) {
        const int nx = x + dv[0];
        const int ny = y + dv[1];
        if (!d.inBounds(nx, ny)) continue;
        if (d.isPassable(nx, ny)) deg++;
    }
    return deg;
}

} // namespace

CorridorBraidResult applyCorridorBraiding(Dungeon& d, RNG& rng, int depth, CorridorBraidStyle style) {
    CorridorBraidResult out;
    if (style == CorridorBraidStyle::Off) return out;

    const int W = d.width;
    const int H = d.height;
    if (W <= 0 || H <= 0) return out;

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    // Build a simple room mask (room interiors) so we can treat corridors separately.
    std::vector<uint8_t> inRoom(static_cast<size_t>(W * H), 0);
    for (const auto& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                inRoom[idx(x, y)] = 1;
            }
        }
    }

    auto isCorridorFloor = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (x <= 0 || y <= 0 || x >= W - 1 || y >= H - 1) return false;
        if (inRoom[idx(x, y)] != 0) return false;
        const TileType t = d.at(x, y).type;
        if (t != TileType::Floor) return false;
        if (nearStairs(d, x, y, 3)) return false;
        if (anyDoorInRadius(d, x, y, 1)) return false;
        if (adjacentToChasm(d, x, y)) return false;
        return true;
    };

    auto isDigWallOk = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return false;
        if (x <= 1 || y <= 1 || x >= W - 2 || y >= H - 2) return false;
        if (inRoom[idx(x, y)] != 0) return false;
        if (d.at(x, y).type != TileType::Wall) return false;
        if (nearStairs(d, x, y, 3)) return false;
        if (anyDoorInRadius(d, x, y, 1)) return false;
        if (adjacentToChasm(d, x, y)) return false;
        // Avoid carving holes directly adjacent to rooms; keep tunnels in solid stone.
        if (adjacentToRoomMask(inRoom, W, H, x, y)) return false;
        return true;
    };

    // Style parameters.
    float braidChance = 0.35f;
    int maxLen = 7;
    float budgetScale = 1.0f;

    switch (style) {
        case CorridorBraidStyle::Sparse:
            braidChance = 0.22f;
            maxLen = 6;
            budgetScale = 0.70f;
            break;
        case CorridorBraidStyle::Moderate:
            braidChance = 0.38f;
            maxLen = 8;
            budgetScale = 1.00f;
            break;
        case CorridorBraidStyle::Heavy:
            braidChance = 0.60f;
            maxLen = 10;
            budgetScale = 1.40f;
            break;
        case CorridorBraidStyle::Off:
        default:
            break;
    }

    // Mild depth scaling: deeper floors get a little more braided.
    braidChance = std::clamp(braidChance + 0.015f * static_cast<float>(std::clamp(depth - 3, 0, 12)), 0.10f, 0.80f);

    const int area = W * H;
    int maxTunnels = std::max(4, area / 650);
    maxTunnels = static_cast<int>(std::lround(static_cast<float>(maxTunnels) * budgetScale));
    maxTunnels = std::clamp(maxTunnels, 3, 28);

    // Collect corridor dead-ends.
    std::vector<Vec2i> deadEnds;
    deadEnds.reserve(static_cast<size_t>(area / 20));

    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (!isCorridorFloor(x, y)) continue;
            const int deg = passableDegree4(d, x, y);
            if (deg == 1) deadEnds.push_back({x, y});
        }
    }

    out.deadEndsBefore = static_cast<int>(deadEnds.size());

    // Shuffle for variety.
    for (int i = static_cast<int>(deadEnds.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(deadEnds[static_cast<size_t>(i)], deadEnds[static_cast<size_t>(j)]);
    }

    static const Vec2i dirs4[4] = {{1,0},{-1,0},{0,1},{0,-1}};

    struct Node {
        int x;
        int y;
        uint8_t dist;
    };

    auto carvePath = [&](int startX, int startY, int endX, int endY, const std::vector<int>& parent) {
        // Reconstruct from end -> start.
        int cur = static_cast<int>(idx(endX, endY));
        const int root = static_cast<int>(idx(startX, startY));
        int guard = 0;

        while (cur != root && cur >= 0 && guard++ < W * H) {
            const int x = cur % W;
            const int y = cur / W;
            if (d.inBounds(x, y) && d.at(x, y).type == TileType::Wall) {
                d.at(x, y).type = TileType::Floor;
                out.tilesCarved++;
            }
            cur = parent[static_cast<size_t>(cur)];
        }

        // Carve the root tile.
        const int rx = root % W;
        const int ry = root / W;
        if (d.inBounds(rx, ry) && d.at(rx, ry).type == TileType::Wall) {
            d.at(rx, ry).type = TileType::Floor;
            out.tilesCarved++;
        }
    };

    int tunnels = 0;
    for (const Vec2i& p : deadEnds) {
        if (tunnels >= maxTunnels) break;
        if (!d.inBounds(p.x, p.y)) continue;
        if (!isCorridorFloor(p.x, p.y)) continue;
        if (passableDegree4(d, p.x, p.y) != 1) continue; // no longer a dead-end

        if (!rng.chance(braidChance)) continue;

        // Identify the "back" direction (the only passable neighbor).
        Vec2i back{0, 0};
        for (const Vec2i& dv : dirs4) {
            const int nx = p.x + dv.x;
            const int ny = p.y + dv.y;
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;
            back = dv;
            break;
        }

        // BFS through walls (limited radius) to find another corridor tile.
        std::vector<int> parent(static_cast<size_t>(W * H), -1);
        std::deque<Node> q;

        // Randomize neighbor order per dead-end.
        std::array<int, 4> ord = {0, 1, 2, 3};
        for (int i = 3; i > 0; --i) {
            const int j = rng.range(0, i);
            std::swap(ord[static_cast<size_t>(i)], ord[static_cast<size_t>(j)]);
        }

        auto pushStart = [&](Vec2i dv) {
            const int sx = p.x + dv.x;
            const int sy = p.y + dv.y;
            if (!isDigWallOk(sx, sy)) return;
            const int si = static_cast<int>(idx(sx, sy));
            if (parent[static_cast<size_t>(si)] != -1) return;
            parent[static_cast<size_t>(si)] = si;
            q.push_back({sx, sy, 1});
        };

        for (const Vec2i& dv : dirs4) {
            if (dv.x == back.x && dv.y == back.y) continue;
            pushStart(dv);
        }

        if (q.empty()) continue;

        bool found = false;
        int endX = -1, endY = -1;
        int rootX = -1, rootY = -1;

        while (!q.empty() && !found) {
            Node n = q.front();
            q.pop_front();

            if (n.dist > static_cast<uint8_t>(maxLen)) continue;
            if (!isDigWallOk(n.x, n.y)) continue;

            // Is this wall tile adjacent to a corridor floor we can connect to?
            for (const Vec2i& dv : dirs4) {
                const int tx = n.x + dv.x;
                const int ty = n.y + dv.y;
                if (!d.inBounds(tx, ty)) continue;
                if (tx == p.x && ty == p.y) continue;
                if (!isCorridorFloor(tx, ty)) continue;
                // Avoid connecting to the immediate back tile; that's pointless.
                if (tx == p.x + back.x && ty == p.y + back.y) continue;

                // Found.
                found = true;
                endX = n.x;
                endY = n.y;

                // Determine the root start tile for reconstruction.
                int cur = static_cast<int>(idx(endX, endY));
                int guard = 0;
                while (guard++ < W * H) {
                    int pr = parent[static_cast<size_t>(cur)];
                    if (pr == cur) {
                        rootX = cur % W;
                        rootY = cur / W;
                        break;
                    }
                    cur = pr;
                }
                break;
            }

            if (found) break;

            if (n.dist == static_cast<uint8_t>(maxLen)) continue;

            for (int oi = 0; oi < 4; ++oi) {
                const Vec2i dv = dirs4[ord[static_cast<size_t>(oi)]];
                const int nx = n.x + dv.x;
                const int ny = n.y + dv.y;
                if (!isDigWallOk(nx, ny)) continue;
                const int ni = static_cast<int>(idx(nx, ny));
                if (parent[static_cast<size_t>(ni)] != -1) continue;
                parent[static_cast<size_t>(ni)] = static_cast<int>(idx(n.x, n.y));
                q.push_back({nx, ny, static_cast<uint8_t>(n.dist + 1)});
            }
        }

        if (!found) continue;
        if (endX < 0 || endY < 0 || rootX < 0 || rootY < 0) continue;

        carvePath(rootX, rootY, endX, endY, parent);
        tunnels++;
        out.tunnelsCarved++;
    }

    // Recount dead-ends.
    int after = 0;
    for (int y = 1; y < H - 1; ++y) {
        for (int x = 1; x < W - 1; ++x) {
            if (!isCorridorFloor(x, y)) continue;
            if (passableDegree4(d, x, y) == 1) after++;
        }
    }
    out.deadEndsAfter = after;
    return out;
}
