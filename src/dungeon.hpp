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
    void openDoor(int x, int y);
    void unlockDoor(int x, int y);

    void generate(RNG& rng);

    void computeFov(int px, int py, int radius);
    void revealAll();

    bool hasLineOfSight(int x0, int y0, int x1, int y1) const;

    Vec2i randomFloor(RNG& rng, bool avoidDoors = true) const;

private:
    bool lineOfSight(int x0, int y0, int x1, int y1) const;
};
