#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Large catalog of handcrafted-style "vault" prefabs used by dungeon generation.
// These are tiny single-entrance wall pockets carved off corridors.
//
// The catalog lives in a .cpp file to keep compile times manageable and to avoid
// duplicating large static data in every translation unit that includes the header.

struct VaultPrefabDef {
    const char* name = nullptr;
    int w = 0;
    int h = 0;
    const char* const* rows = nullptr;
    int minDepth = 1;
    int weight = 1;
};

namespace vaultprefabs {

// Returns a pointer to the internal static catalog; outCount is set to the number of entries.
const VaultPrefabDef* catalog(size_t& outCount);

// Validation helper used by unit tests and debug tooling.
// Rules:
//  - w/h must be positive.
//  - rows must contain exactly h strings, each of length w.
//  - boundary must be solid wall '#' except for EXACTLY ONE entrance door char
//    ('+', 's', or 'L') which must not be a corner.
//  - interior may contain any glyphs supported by the prefab applier.
bool validate(const VaultPrefabDef& def, std::string& outErr);

// Convenience helpers for catalog filtering/weighting.
bool hasGlyph(const VaultPrefabDef& def, char glyph);
int countGlyph(const VaultPrefabDef& def, char glyph);

} // namespace vaultprefabs
