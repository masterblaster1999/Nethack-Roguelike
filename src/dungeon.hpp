#pragma once
#include "common.hpp"
#include "rng.hpp"
#include <cstdint>
#include <vector>

enum class TileType : uint8_t {
    Wall = 0,
    Floor,
    DoorClosed,
    DoorOpen,
    StairsUp,
    StairsDown,
    // Append-only: hidden until discovered by searching.
    DoorSecret,
    // Append-only: visible but requires a Key to open.
    DoorLocked,
    // Append-only: impassable terrain that does NOT block line-of-sight.
    // Used for pits/chasm features that shape room flow without creating "walls".
    Chasm,
    // Append-only: interior column that blocks movement and line-of-sight.
    Pillar,
    // Append-only: pushable boulder obstacle; blocks movement but does NOT block line-of-sight.
    Boulder,
};

struct Tile {
    TileType type = TileType::Wall;
    bool visible = false;
    bool explored = false;
};

enum class RoomType : uint8_t {
    Normal = 0,
    Treasure,
    Lair,
    Shrine,
    // Append-only: hidden treasure room accessed via a secret door.
    Secret,
    // Append-only: visible treasure room behind a locked door.
    Vault,
    // Append-only: merchant shop stocked with items for sale.
    Shop,
};

struct Room {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    RoomType type = RoomType::Normal;

    int x2() const { return x + w; }
    int y2() const { return y + h; }
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }

    bool contains(int px, int py) const {
        return px >= x && px < x2() && py >= y && py < y2();
    }
};

class Dungeon {
public:
    int width = 0;
    int height = 0;
    std::vector<Tile> tiles;

    std::vector<Room> rooms;

    // Generator hints: optional guaranteed bonus loot spawns (e.g. boulder bridge caches).
    // Used only during floor generation; not serialized.
    std::vector<Vec2i> bonusLootSpots;
    Vec2i stairsUp{ -1, -1 };
    Vec2i stairsDown{ -1, -1 };

    Dungeon() = default;
    Dungeon(int w, int h);

    bool inBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }

    Tile& at(int x, int y) { return tiles[static_cast<size_t>(y * width + x)]; }
    const Tile& at(int x, int y) const { return tiles[static_cast<size_t>(y * width + x)]; }

    bool isWalkable(int x, int y) const;
    bool isPassable(int x, int y) const; // includes closed doors (AI/path)
    bool isOpaque(int x, int y) const;
    bool isDoorClosed(int x, int y) const;
    bool isDoorLocked(int x, int y) const;
    bool isDoorOpen(int x, int y) const;
    void closeDoor(int x, int y);
    void openDoor(int x, int y);
    void lockDoor(int x, int y);
    void unlockDoor(int x, int y);

    // Terrain modification
    bool isDiggable(int x, int y) const;
    // Converts a diggable tile (wall/pillar/door variants) into floor.
    // Returns true if the tile changed.
    bool dig(int x, int y);

    // Procedural generation.
    //
    // `depth` is used to vary generation style (rooms vs caverns vs mazes)
    // and difficulty pacing.
    void generate(RNG& rng, int depth, int maxDepth);

    void computeFov(int px, int py, int radius, bool markExplored = true);

    // Computes a visibility mask (0/1) from (px,py) within `radius` using the same shadowcasting
    // as computeFov, but without mutating tiles.
    // `outMask` is resized and filled with 0/1, length = width*height.
    void computeFovMask(int px, int py, int radius, std::vector<uint8_t>& outMask) const;
    void revealAll();

    bool hasLineOfSight(int x0, int y0, int x1, int y1) const;

    // Compute a simple sound-propagation cost map from (sx, sy).
    //
    // Returns a width*height array of minimum "attenuation cost" from the source,
    // or -1 if unreachable.
    //
    // Walls and secret doors block sound. Closed/locked doors allow sound through
    // but are treated as muffling (higher cost).
    //
    // `maxCost` limits the search for efficiency; tiles beyond this cost remain -1.
    std::vector<int> computeSoundMap(int sx, int sy, int maxCost) const;

    Vec2i randomFloor(RNG& rng, bool avoidDoors = true) const;

private:
    bool lineOfSight(int x0, int y0, int x1, int y1) const;
};
