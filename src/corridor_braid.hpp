#pragma once

#include "dungeon.hpp"

#include <cstdint>

// Corridor braiding pass
//
// This pass reduces corridor dead-ends by carving short connecting tunnels through
// solid wall, creating more loops and alternate routes. It is designed to be:
//  - Conservative (avoids rooms/doors/stairs and special tiles)
//  - Local (small max tunnel length)
//  - Deterministic for a given RNG seed

enum class CorridorBraidStyle : uint8_t {
    Off = 0,
    Sparse,
    Moderate,
    Heavy,
};

struct CorridorBraidResult {
    int tunnelsCarved = 0;
    int tilesCarved = 0;     // wall->floor conversions
    int deadEndsBefore = 0;
    int deadEndsAfter = 0;
};

CorridorBraidResult applyCorridorBraiding(Dungeon& d, RNG& rng, int depth, CorridorBraidStyle style);
