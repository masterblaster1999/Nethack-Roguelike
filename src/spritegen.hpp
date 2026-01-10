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

// Special seed flag for item sprites.
//
// When this bit is set, generateItemSprite() treats the low 8 bits of `seed`
// as a run-randomized "appearance id" for NetHack-style identification
// (potions/scrolls/rings/wands), and generates art based on that appearance
// rather than the true item kind.
//
// The renderer sets this when item identification is enabled so players can't
// visually identify potion/scroll/ring/wand types from their sprite alone.
inline constexpr uint32_t SPRITE_SEED_IDENT_APPEARANCE_FLAG = 0x80000000u;

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

// --- Isometric helpers ---
// Convert a *top-down* square tile sprite into a 2:1 isometric diamond tile.
// This is used by the renderer when ViewMode::Isometric is enabled so that
// terrain reads as proper diamonds instead of a vertically-squashed square.
//
// The returned sprite has dimensions:
//   w = src.w
//   h = max(1, src.h / 2)
//
// NOTE: The input is assumed to be a square sprite.
SpritePixels projectToIsometricDiamond(const SpritePixels& src, uint32_t seed, int frame, bool outline = true);

// Generates a simple 2.5D isometric wall "block" sprite (square output) that can be
// drawn using the renderer's sprite anchoring (mapSpriteDst) to add verticality.
SpritePixels generateIsometricWallBlockTile(uint32_t seed, int frame, int pxSize = 16);
// 2.5D isometric door "block" sprites (square output) used in isometric view so
// closed/locked doors read as volumetric pieces of wall geometry rather than flat
// top-down overlays.
SpritePixels generateIsometricDoorBlockTile(uint32_t seed, bool locked, int frame, int pxSize = 16);
// 2.5D isometric open-door doorway frame (square output).
// Used in isometric view so open doors still feel like vertical wall geometry
// (a passable archway) instead of a purely flat floor overlay.
SpritePixels generateIsometricDoorwayBlockTile(uint32_t seed, int frame, int pxSize = 16);
// 2.5D isometric pillar/boulder sprites (square output) for props that should read as volumetric
// blockers in isometric view.
SpritePixels generateIsometricPillarBlockTile(uint32_t seed, int frame, int pxSize = 16);
SpritePixels generateIsometricBoulderBlockTile(uint32_t seed, int frame, int pxSize = 16);
// Isometric ground contact shadow / rim overlay (diamond, transparent).
// Mask bits: 1=N, 2=E, 4=S, 8=W (bit set means "neighbor is an occluder")
SpritePixels generateIsometricEdgeShadeOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize = 16);
// Isometric cast shadow overlay (diamond, transparent).
// Mask bits: 1=N, 2=E, 4=S, 8=W (bit set means "neighbor is a tall shadow caster")
// In the current lighting model (light from top-left), only N and W are used by the renderer.
SpritePixels generateIsometricCastShadowOverlay(uint32_t seed, uint8_t mask, int frame, int pxSize = 16);
// Isometric entity ground shadow overlay (diamond, transparent).
// Intended to be drawn under sprites in isometric view to anchor them to the ground plane.
SpritePixels generateIsometricEntityShadowOverlay(uint32_t seed, int frame, int pxSize = 16);
// Isometric stairs overlay (diamond, transparent).
// Intended to be drawn on top of the themed floor diamond so stairs read clearly
// in 2.5D isometric view without relying on a projected top-down sprite.
SpritePixels generateIsometricStairsOverlay(uint32_t seed, bool up, int frame, int pxSize = 16);
// NOTE: Pillar/door/stairs tiles are generated as *transparent overlays*.
// The renderer layers them on top of the underlying themed floor tile.
SpritePixels generatePillarTile(uint32_t seed, int frame, int pxSize = 16);
SpritePixels generateBoulderTile(uint32_t seed, int frame, int pxSize = 16);
SpritePixels generateFountainTile(uint32_t seed, int frame, int pxSize = 16);
SpritePixels generateAltarTile(uint32_t seed, int frame, int pxSize = 16);
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
