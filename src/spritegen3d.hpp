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
