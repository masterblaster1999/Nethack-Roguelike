#pragma once

#include "common.hpp"

#include <cstdint>
#include <string>

// Shared, stable tag IDs for procedural crafting metadata.
//
// IMPORTANT: Append-only once shipped (save compatibility).
// These IDs are used to pack data into Item::enchant for things like Essence Shards.
namespace crafttags {

// Tag IDs are intentionally aligned with existing gameplay tags
// (fish/crops/butchery/crafting). Keep these stable once introduced.
enum class Tag : uint8_t {
    None = 0,
    Regen = 1,
    Haste = 2,
    Shield = 3,
    Aurora = 4,
    Clarity = 5,
    Venom = 6,
    Ember = 7,
    Stone = 8,
    Rune = 9,
    Arc = 10,
    Alch = 11,
    Luck = 12,
    Daze = 13,
    // Append-only.
};

inline constexpr int kMaxTagId = static_cast<int>(Tag::Daze);

inline const char* tagToken(Tag t) {
    switch (t) {
        case Tag::Regen:   return "REGEN";
        case Tag::Haste:   return "HASTE";
        case Tag::Shield:  return "SHIELD";
        case Tag::Aurora:  return "AURORA";
        case Tag::Clarity: return "CLARITY";
        case Tag::Venom:   return "VENOM";
        case Tag::Ember:   return "EMBER";
        case Tag::Stone:   return "STONE";
        case Tag::Rune:    return "RUNE";
        case Tag::Arc:     return "ARC";
        case Tag::Alch:    return "ALCH";
        case Tag::Luck:    return "LUCK";
        case Tag::Daze:    return "DAZE";
        default:           return "";
    }
}

inline Tag tagFromIndex(int idx) {
    return static_cast<Tag>(clampi(idx, 0, kMaxTagId));
}

inline int tagIndex(Tag t) {
    return static_cast<int>(t);
}

inline Tag tagFromToken(const std::string& token) {
    if (token.empty()) return Tag::None;
    const std::string u = toUpper(token);
    if (u == "REGEN") return Tag::Regen;
    if (u == "HASTE") return Tag::Haste;
    if (u == "SHIELD") return Tag::Shield;
    if (u == "AURORA") return Tag::Aurora;
    if (u == "CLARITY") return Tag::Clarity;
    if (u == "VENOM") return Tag::Venom;
    if (u == "EMBER") return Tag::Ember;
    if (u == "STONE") return Tag::Stone;
    if (u == "RUNE") return Tag::Rune;
    if (u == "ARC") return Tag::Arc;
    if (u == "ALCH") return Tag::Alch;
    if (u == "LUCK") return Tag::Luck;
    if (u == "DAZE") return Tag::Daze;
    return Tag::None;
}

inline const char* tokenByIndex(int idx) {
    return tagToken(tagFromIndex(idx));
}

} // namespace crafttags
