#pragma once
#include "common.hpp"
#include "rng.hpp"
#include <vector>

enum class TileType : uint8_t {
    Wall = 0,
    Floor,
    StairsDown,
};

struct Tile {
    TileType type = TileType::Wall;
    bool visible = false;
    bool explored = false;
};

class Dungeon {
public:
    int width = 0;
    int height = 0;

    // row-major tiles: tiles[y*width + x]
    std::vector<Tile> tiles;

    Vec2i stairsDown{ -1, -1 };

    Dungeon() = default;
    Dungeon(int w, int h) { resize(w, h); }

    void resize(int w, int h);
    bool inBounds(int x, int y) const;

    Tile& at(int x, int y);
    const Tile& at(int x, int y) const;

    bool isWalkable(int x, int y) const;
    bool isOpaque(int x, int y) const;

    // Rooms + corridors generator.
    // After generation, everything is "unexplored" until you compute FOV.
    void generate(RNG& rng, int roomAttempts = 90);

    // Simple raycast FOV with exploration memory.
    void computeFov(int px, int py, int radius);

private:
    bool lineOfSight(int x0, int y0, int x1, int y1) const;
};
