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
