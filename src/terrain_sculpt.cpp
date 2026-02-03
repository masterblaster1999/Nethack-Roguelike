#include "terrain_sculpt.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace {

inline bool isDoorTile(TileType t) {
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

inline bool isStairsTileType(TileType t) {
    return t == TileType::StairsUp || t == TileType::StairsDown;
}

inline bool isFloorOrWall(TileType t) {
    return t == TileType::Floor || t == TileType::Wall;
}

// Conservative stairs connectivity check for procgen safety.
// We use the same notion of "passable" as the game: floors, open doors, *and* closed doors
// (the player/AI can open them). Locked doors remain non-passable.
static bool stairsConnected(const Dungeon& d) {
    if (!d.inBounds(d.stairsUp.x, d.stairsUp.y)) return true;
    if (!d.inBounds(d.stairsDown.x, d.stairsDown.y)) return true;
    if (d.stairsUp == d.stairsDown) return true;

    const size_t n = static_cast<size_t>(d.width * d.height);
    std::vector<uint8_t> vis(n, uint8_t{0});
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    std::deque<Vec2i> q;
    q.push_back(d.stairsUp);
    vis[idx(d.stairsUp.x, d.stairsUp.y)] = 1;

    const int dirs[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
    while (!q.empty()) {
        const Vec2i p = q.front();
        q.pop_front();
        if (p == d.stairsDown) return true;

        for (const auto& dv : dirs) {
            const int nx = p.x + dv[0];
            const int ny = p.y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;
            const size_t ii = idx(nx, ny);
            if (vis[ii]) continue;
            vis[ii] = 1;
            q.push_back({nx, ny});
        }
    }

    return false;
}

static int floorNeighbors4(const Dungeon& d, int x, int y) {
    int c = 0;
    if (d.inBounds(x + 1, y) && d.at(x + 1, y).type == TileType::Floor) ++c;
    if (d.inBounds(x - 1, y) && d.at(x - 1, y).type == TileType::Floor) ++c;
    if (d.inBounds(x, y + 1) && d.at(x, y + 1).type == TileType::Floor) ++c;
    if (d.inBounds(x, y - 1) && d.at(x, y - 1).type == TileType::Floor) ++c;
    return c;
}

} // namespace

TerrainSculptResult applyTerrainSculpt(Dungeon& d, RNG& rng, int depth, TerrainSculptStyle style) {
    TerrainSculptResult out;

    if (d.width < 8 || d.height < 8) return out;
    if (style == TerrainSculptStyle::Subtle && d.rooms.empty()) {
        // On pure caverns/mazes we generally already have organic walls.
        // (We still allow callers to use Ruins/Tunnels explicitly.)
        return out;
    }

    const int area = std::max(1, d.width * d.height);

    // ------------------------------------------------------------
    // Parameterization
    // ------------------------------------------------------------
    int bandRadius = 1;
    float carveSeedP = 0.025f;
    float collapseSeedP = 0.010f;
    int smoothIters = 1;
    bool preferOutsideRooms = true;

    switch (style) {
        case TerrainSculptStyle::Subtle:
            bandRadius = 1;
            carveSeedP = 0.018f;
            collapseSeedP = 0.004f;
            smoothIters = 1;
            preferOutsideRooms = true;
            break;
        case TerrainSculptStyle::Ruins:
            bandRadius = 2;
            carveSeedP = 0.040f;
            collapseSeedP = 0.014f;
            smoothIters = 2;
            preferOutsideRooms = false;
            break;
        case TerrainSculptStyle::Tunnels:
            bandRadius = 1;
            carveSeedP = 0.060f;
            collapseSeedP = 0.006f;
            smoothIters = 1;
            preferOutsideRooms = true;
            break;
        default:
            break;
    }

    // Slightly intensify with depth (but keep it sane).
    const float depthBoost = 0.004f * static_cast<float>(std::clamp(depth - 1, 0, 8));
    carveSeedP = std::clamp(carveSeedP + depthBoost, 0.0f, 0.10f);
    collapseSeedP = std::clamp(collapseSeedP + depthBoost * 0.60f, 0.0f, 0.06f);

    // ------------------------------------------------------------
    // Build a per-tile room-type cache so we can protect special rooms.
    // 0xFF means "not inside any room".
    // ------------------------------------------------------------
    const size_t n = static_cast<size_t>(d.width * d.height);
    constexpr uint8_t kNoRoomType = uint8_t{0xFFu};
    std::vector<uint8_t> roomType(n, kNoRoomType);
    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * d.width + x); };

    for (const Room& r : d.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!d.inBounds(x, y)) continue;
                roomType[idx(x, y)] = static_cast<uint8_t>(r.type);
            }
        }
    }

    auto inProtectedRoom = [&](int x, int y) -> bool {
        const uint8_t rt = roomType[idx(x, y)];
        if (rt == kNoRoomType) return false;
        const RoomType rtt = static_cast<RoomType>(rt);
        return rtt != RoomType::Normal;
    };

    // If we are in "outside-room" modes, avoid editing *inside* rooms.
    auto insideAnyRoom = [&](int x, int y) -> bool {
        return roomType[idx(x, y)] != kNoRoomType;
    };

    // ------------------------------------------------------------
    // Protection mask: doors/stairs and their nearby tiles are immutable.
    // ------------------------------------------------------------
    std::vector<uint8_t> protect(n, uint8_t{0});

    auto protectRadius = [&](Vec2i p, int r) {
        if (!d.inBounds(p.x, p.y)) return;
        for (int oy = -r; oy <= r; ++oy) {
            for (int ox = -r; ox <= r; ++ox) {
                if (std::abs(ox) + std::abs(oy) > r) continue;
                const int x = p.x + ox;
                const int y = p.y + oy;
                if (!d.inBounds(x, y)) continue;
                protect[idx(x, y)] = 1;
            }
        }
    };

    // Stairs: larger radius because these areas must remain navigable and readable.
    protectRadius(d.stairsUp, 3);
    protectRadius(d.stairsDown, 3);

    // Door radius: smaller, but still prevents ugly threshold deformation.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const TileType t = d.at(x, y).type;
            if (isDoorTile(t)) {
                protectRadius({x, y}, 2);
            }
        }
    }

    // Always protect the border ring.
    for (int x = 0; x < d.width; ++x) {
        protect[idx(x, 0)] = 1;
        protect[idx(x, d.height - 1)] = 1;
    }
    for (int y = 0; y < d.height; ++y) {
        protect[idx(0, y)] = 1;
        protect[idx(d.width - 1, y)] = 1;
    }

    // ------------------------------------------------------------
    // Identify an edge band: tiles where Wall touches Floor (4-neighborhood).
    // ------------------------------------------------------------
    std::vector<uint8_t> band(n, uint8_t{0});

    auto isEdge = [&](int x, int y) -> bool {
        const TileType t = d.at(x, y).type;
        if (!isFloorOrWall(t)) return false;
        if (protect[idx(x, y)] != 0) return false;
        if (inProtectedRoom(x, y)) return false;
        if (preferOutsideRooms && insideAnyRoom(x, y)) return false;

        const int dirs[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
        for (const auto& dv : dirs) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!d.inBounds(nx, ny)) continue;
            const TileType nt = d.at(nx, ny).type;
            if ((t == TileType::Wall && nt == TileType::Floor) || (t == TileType::Floor && nt == TileType::Wall)) {
                // Extra guard: don't deform the boundary of special rooms, even when the *wall* is
                // technically outside the room.
                if (inProtectedRoom(nx, ny)) return false;
                if (preferOutsideRooms && insideAnyRoom(nx, ny)) {
                    // For outside-room modes, we still allow carving walls adjacent to room floors,
                    // but we disallow collapsing room floors themselves (handled later).
                    // This keeps rooms from shrinking in corridor-focused modes.
                    if (t == TileType::Floor) return false;
                }
                return true;
            }
        }
        return false;
    };

    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (isEdge(x, y)) band[idx(x, y)] = 1;
        }
    }

    // Expand edge band into a slightly thicker "mutable" region so the smooth pass
    // has some space to operate.
    std::vector<uint8_t> mut(n, uint8_t{0});
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            if (band[idx(x, y)] == 0) continue;
            for (int oy = -bandRadius; oy <= bandRadius; ++oy) {
                for (int ox = -bandRadius; ox <= bandRadius; ++ox) {
                    const int xx = x + ox;
                    const int yy = y + oy;
                    if (!d.inBounds(xx, yy)) continue;
                    const size_t ii = idx(xx, yy);
                    if (protect[ii] != 0) continue;
                    const TileType t = d.at(xx, yy).type;
                    if (!isFloorOrWall(t)) continue;
                    if (inProtectedRoom(xx, yy)) continue;
                    if (preferOutsideRooms && insideAnyRoom(xx, yy)) {
                        // Allow expanding into walls adjacent to rooms, but don't edit floors inside rooms.
                        if (t == TileType::Floor) continue;
                    }
                    mut[ii] = 1;
                }
            }
        }
    }

    // Early out if the band is empty (happens on tiny or degenerate maps).
    int bandCount = 0;
    for (uint8_t b : band) bandCount += (b != 0);
    if (bandCount == 0) return out;

    // Backup full tile types for safe rollback.
    std::vector<TileType> before(n, TileType::Wall);
    for (size_t i = 0; i < n; ++i) before[i] = d.tiles[i].type;

    // ------------------------------------------------------------
    // 1) Stochastic seed noise on the thin band.
    // ------------------------------------------------------------
    for (int y = 1; y < d.height - 1; ++y) {
        for (int x = 1; x < d.width - 1; ++x) {
            const size_t ii = idx(x, y);
            if (band[ii] == 0) continue;
            if (protect[ii] != 0) continue;
            const TileType t = d.at(x, y).type;
            if (!isFloorOrWall(t)) continue;

            if (t == TileType::Wall) {
                if (rng.chance(carveSeedP)) {
                    d.at(x, y).type = TileType::Floor;
                    out.wallToFloor++;
                }
            } else { // Floor
                // Only collapse tiles that are clearly in an open area (prevents blocking 1-wide corridors).
                if (floorNeighbors4(d, x, y) < 3) continue;
                if (rng.chance(collapseSeedP)) {
                    d.at(x, y).type = TileType::Wall;
                    out.floorToWall++;
                }
            }
        }
    }

    // ------------------------------------------------------------
    // 2) Cellular smoothing in the wider mutable band.
    // ------------------------------------------------------------
    auto wallCount8 = [&](int x, int y) -> int {
        int c = 0;
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                if (ox == 0 && oy == 0) continue;
                const int nx = x + ox;
                const int ny = y + oy;
                if (!d.inBounds(nx, ny)) { c++; continue; }

                const TileType tt = d.at(nx, ny).type;
                // Treat anything non-floor as "wall" for smoothing.
                if (tt != TileType::Floor) c++;
            }
        }
        return c;
    };

    std::vector<TileType> next(before.begin(), before.end());

    for (int it = 0; it < smoothIters; ++it) {
        // Initialize next with current so "keep" is implicit.
        for (size_t i = 0; i < n; ++i) next[i] = d.tiles[i].type;

        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                const size_t ii = idx(x, y);
                if (mut[ii] == 0) continue;
                if (protect[ii] != 0) continue;

                const TileType cur = d.at(x, y).type;
                if (!isFloorOrWall(cur)) continue;

                const int wc = wallCount8(x, y);
                if (wc >= 5) next[ii] = TileType::Wall;
                else if (wc <= 2) next[ii] = TileType::Floor;
                else next[ii] = cur;
            }
        }

        // Apply next.
        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                const size_t ii = idx(x, y);
                if (mut[ii] == 0) continue;
                if (protect[ii] != 0) continue;
                const TileType cur = d.at(x, y).type;
                const TileType nt = next[ii];
                if (!isFloorOrWall(cur) || !isFloorOrWall(nt)) continue;
                if (cur == nt) continue;
                d.at(x, y).type = nt;
                out.smoothed++;
            }
        }
    }

    // ------------------------------------------------------------
    // Safety: if we broke stairs connectivity, revert completely.
    // ------------------------------------------------------------
    if (!stairsConnected(d)) {
        for (size_t i = 0; i < n; ++i) d.tiles[i].type = before[i];
        return TerrainSculptResult{};
    }

    // Cap runaway edits on very large/odd maps (prevents pathological seeds).
    // If this triggers, we still keep the edits; it's just for stats/debug and sanity.
    const int cap = std::clamp(area / 4, 500, 4000);
    if (out.totalEdits() > cap) {
        // Don't revert; just clamp the reported values.
        const int extra = out.totalEdits() - cap;
        // Prefer to reduce "smoothed" first since it can double-count changes.
        const int take = std::min(extra, out.smoothed);
        out.smoothed -= take;
    }

    return out;
}
