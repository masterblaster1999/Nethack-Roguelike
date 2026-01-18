#pragma once
#include "spritegen.hpp"

// Render a 2D sprite as a small 3D voxel "mini-model" (extruded + beveled),
// then re-render it back down to a requested pixel size (up to 256) with
// lighting/shadows.
// This keeps the rest of the game 2D, but gives entities/items/projectiles a 3D look.
SpritePixels renderSprite3DExtruded(const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16);

// Higher-level helpers that can choose between "native" procedural voxel models
// (true 3D generation) and the 2D->3D extrusion fallback.
SpritePixels renderSprite3DEntity(EntityKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16);
SpritePixels renderSprite3DItem(ItemKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16);
SpritePixels renderSprite3DProjectile(ProjectileKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16);

// --- 3D UI "turntable" previews ---
//
// The main game sprites are generated in small discrete animation frames.
// For UI overlays (Codex/Discoveries/etc.), we sometimes want a more explicit
// 3D read: a large, smoothly rotating preview. These helpers expose a camera
// yaw parameter while keeping the underlying procedural voxel model the same.
//
// yawRad is in radians and rotates the camera around the model's vertical axis.
SpritePixels renderSprite3DExtrudedTurntable(const SpritePixels& base2d, uint32_t seed, int frame, float yawRad, int outPx = 128);
SpritePixels renderSprite3DEntityTurntable(EntityKind kind, const SpritePixels& base2d, uint32_t seed, int frame, float yawRad, int outPx = 128);
SpritePixels renderSprite3DItemTurntable(ItemKind kind, const SpritePixels& base2d, uint32_t seed, int frame, float yawRad, int outPx = 128);

// --- Isometric voxel rendering ---
//
// In ViewMode::Isometric, the renderer draws terrain in a 2:1 dimetric/isometric
// projection. These helpers re-render voxel sprites using that same projection,
// producing a tiny projected 2D triangle mesh (from visible voxel faces) and
// rasterizing it back into a SpritePixels.
//
// This is intentionally separate from the default voxel sprite renderer (which uses
// a small perspective camera) so that the same voxel "model" can read correctly
// in both view modes.
SpritePixels renderSprite3DExtrudedIso(const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16, bool isoRaytrace = false);
SpritePixels renderSprite3DEntityIso(EntityKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16, bool isoRaytrace = false);
SpritePixels renderSprite3DItemIso(ItemKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16, bool isoRaytrace = false);
SpritePixels renderSprite3DProjectileIso(ProjectileKind kind, const SpritePixels& base2d, uint32_t seed, int frame, int outPx = 16, bool isoRaytrace = false);

// --- Isometric terrain voxel blocks ----------------------------------------
// Optional: render isometric terrain "block" tiles (walls/doors/pillars/boulders)
// as true voxel models so they match the 3D voxel sprite style.
//
// NOTE: These are purely cosmetic and are generated/cached like other procedural
// sprites.

enum class IsoTerrainBlockKind : uint8_t {
    Wall,
    DoorClosed,
    DoorLocked,
    DoorOpen,
    Pillar,
    Boulder,
};

// Render an isometric terrain block as a voxel sprite.
// - outPx: output sprite size (clamped internally to 16..256)
// - isoRaytrace: when true, use the orthographic voxel raytracer (slower to generate).
SpritePixels renderIsoTerrainBlockVoxel(IsoTerrainBlockKind kind, uint32_t seed, int frame, int outPx = 16, bool isoRaytrace = false);

