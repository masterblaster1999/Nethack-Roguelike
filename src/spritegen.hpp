#pragma once
#include "common.hpp"
#include "rng.hpp"
#include <cstdint>
#include <vector>

// Forward declare to avoid include cycles.
enum class EntityKind : uint8_t;

struct SpritePixels {
    int w = 0;
    int h = 0;
    std::vector<Color> px; // row-major

    Color& at(int x, int y) { return px[static_cast<size_t>(y * w + x)]; }
    const Color& at(int x, int y) const { return px[static_cast<size_t>(y * w + x)]; }
};

// 16x16 sprites
SpritePixels generateEntitySprite(EntityKind kind, uint32_t seed);

// 16x16 tiles
SpritePixels generateFloorTile(uint32_t seed);
SpritePixels generateWallTile(uint32_t seed);
SpritePixels generateStairsTile(uint32_t seed);
