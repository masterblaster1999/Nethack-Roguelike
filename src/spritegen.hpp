#pragma once
#include "common.hpp"
#include "rng.hpp"
#include <cstdint>
#include <vector>

// Forward declares to avoid heavy includes here.
enum class EntityKind : uint8_t;
enum class ItemKind : uint8_t;
enum class ProjectileKind : uint8_t;
// UI skin theme (defined in game.hpp).
enum class UITheme : uint8_t;

struct SpritePixels {
    int w = 0;
    int h = 0;
    std::vector<Color> px; // row-major

    Color& at(int x, int y) { return px[static_cast<size_t>(y * w + x)]; }
    const Color& at(int x, int y) const { return px[static_cast<size_t>(y * w + x)]; }
};

// 16x16 sprites
SpritePixels generateEntitySprite(EntityKind kind, uint32_t seed, int frame);
SpritePixels generateItemSprite(ItemKind kind, uint32_t seed, int frame);
SpritePixels generateProjectileSprite(ProjectileKind kind, uint32_t seed, int frame);

// 16x16 tiles
SpritePixels generateFloorTile(uint32_t seed, int frame);
SpritePixels generateWallTile(uint32_t seed, int frame);
SpritePixels generateStairsTile(uint32_t seed, bool up, int frame);
SpritePixels generateDoorTile(uint32_t seed, bool open, int frame);
SpritePixels generateLockedDoorTile(uint32_t seed, int frame);

// UI tiles (16x16)
SpritePixels generateUIPanelTile(UITheme theme, uint32_t seed, int frame);
SpritePixels generateUIOrnamentTile(UITheme theme, uint32_t seed, int frame);
