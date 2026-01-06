#pragma once

#include "dungeon.hpp"
#include "rng.hpp"

#include <cstdint>

// Terrain sculpting is a lightweight post-process that operates on *only* Wall/Floor
// tiles to break up overly-rectilinear layouts (especially on room-based generators).
//
// It works in three steps:
//   1) Identify an "edge band" where Wall touches Floor.
//   2) Apply a small stochastic "chip/collapse" noise on that band.
//   3) Run 1-2 iterations of a cellular-automata-style smooth on a small band radius.
//
// The pass is deliberately conservative:
// - Never edits doors, stairs, chasms, pillars, boulders, etc.
// - Avoids special rooms (anything not RoomType::Normal).
// - Avoids a safety radius around doors and stairs.
// - Reverts entirely if it would break stairs connectivity.

enum class TerrainSculptStyle : uint8_t {
    Subtle = 0, // light corridor roughening (little to no room deformation)
    Ruins,      // stronger edge noise + a couple smoothing iterations
    Tunnels,    // wider/nastier tunnel edges (mostly outside rooms)
};

struct TerrainSculptResult {
    int wallToFloor = 0; // tiles chipped into floors
    int floorToWall = 0; // tiles collapsed into walls
    int smoothed    = 0; // additional changes from smoothing iterations

    int totalEdits() const { return wallToFloor + floorToWall + smoothed; }
};

TerrainSculptResult applyTerrainSculpt(Dungeon& d, RNG& rng, int depth, TerrainSculptStyle style);
