#pragma once
#include "common.hpp"
#include "rng.hpp"
#include <cstdint>
#include <vector>

// Forward declares to avoid heavy includes here.
enum class EntityKind : uint8_t;
enum class ItemKind : uint8_t;
enum class ProjectileKind : uint8_t;
// Status effects (defined in effects.hpp via game.hpp).
enum class EffectKind : uint8_t;
// UI skin theme (defined in game.hpp).
enum class UITheme : uint8_t;

struct SpritePixels {
    int w = 0;
    int h = 0;
    std::vector<Color> px; // row-major

    Color& at(int x, int y) { return px[static_cast<size_t>(y * w + x)]; }
    const Color& at(int x, int y) const { return px[static_cast<size_t>(y * w + x)]; }
};

// Procedural sprites.
// The underlying generator operates in a tiny, deterministic 16x16 "design grid" and
// then upscales to the requested pixel size.
//
// Supported range: 16..256 (values outside are clamped).
SpritePixels generateEntitySprite(EntityKind kind, uint32_t seed, int frame, bool use3d, int pxSize = 16);
SpritePixels generateItemSprite(ItemKind kind, uint32_t seed, int frame, bool use3d, int pxSize = 16);
SpritePixels generateProjectileSprite(ProjectileKind kind, uint32_t seed, int frame, bool use3d, int pxSize = 16);

// Procedural terrain tiles (same size range as sprites).
SpritePixels generateFloorTile(uint32_t seed, int frame, int pxSize = 16);
// Themed floor variant (style mapping matches renderer's room-style mapping:
//  0 = Normal, 1 = Treasure, 2 = Lair, 3 = Shrine, 4 = Secret, 5 = Vault, 6 = Shop
SpritePixels generateThemedFloorTile(uint32_t seed, uint8_t style, int frame, int pxSize = 16);
SpritePixels generateWallTile(uint32_t seed, int frame, int pxSize = 16);
// New terrain tiles
SpritePixels generateChasmTile(uint32_t seed, int frame, int pxSize = 16);
// NOTE: Pillar/door/stairs tiles are generated as *transparent overlays*.
// The renderer layers them on top of the underlying themed floor tile.
SpritePixels generatePillarTile(uint32_t seed, int frame, int pxSize = 16);
SpritePixels generateStairsTile(uint32_t seed, bool up, int frame, int pxSize = 16);
SpritePixels generateDoorTile(uint32_t seed, bool open, int frame, int pxSize = 16);
SpritePixels generateLockedDoorTile(uint32_t seed, int frame, int pxSize = 16);

// UI tiles (procedural). These default to 16x16 since the UI is drawn in pixel-space.
SpritePixels generateUIPanelTile(UITheme theme, uint32_t seed, int frame, int pxSize = 16);
SpritePixels generateUIOrnamentTile(UITheme theme, uint32_t seed, int frame, int pxSize = 16);

// Transparent overlay decals (extra tile variety / room theming)
SpritePixels generateFloorDecalTile(uint32_t seed, uint8_t style, int frame, int pxSize = 16);
SpritePixels generateWallDecalTile(uint32_t seed, uint8_t style, int frame, int pxSize = 16);

// Transparent autotile overlays (edge/corner shaping for walls/chasm)
// openMask bits: 1=N, 2=E, 4=S, 8=W (bit set means "edge exposed")
SpritePixels generateWallEdgeOverlay(uint32_t seed, uint8_t openMask, int variant, int frame, int pxSize = 16);
SpritePixels generateChasmRimOverlay(uint32_t seed, uint8_t openMask, int variant, int frame, int pxSize = 16);

// Environmental field tile (confusion gas), drawn as a translucent overlay.
SpritePixels generateConfusionGasTile(uint32_t seed, int frame, int pxSize = 16);

// Environmental field tile (fire), drawn as a translucent overlay.
SpritePixels generateFireTile(uint32_t seed, int frame, int pxSize = 16);

// HUD/status icon (procedural). Defaults to 16x16.
SpritePixels generateEffectIcon(EffectKind kind, int frame, int pxSize = 16);
