#pragma once
#include "common.hpp"
#include "rng.hpp"
#include <cstdint>
#include <vector>

enum class DungeonBranch : uint8_t;

enum class ItemKind : uint8_t;

enum class EndlessStratumTheme : uint8_t {
    Ruins = 0,
    Caverns,
    Labyrinth,
    Warrens,
    Mines,
    Catacombs,
};

inline const char* endlessStratumThemeName(EndlessStratumTheme t) {
    switch (t) {
        case EndlessStratumTheme::Ruins:      return "RUINS";
        case EndlessStratumTheme::Caverns:    return "CAVERNS";
        case EndlessStratumTheme::Labyrinth:  return "LABYRINTH";
        case EndlessStratumTheme::Warrens:    return "WARRENS";
        case EndlessStratumTheme::Mines:      return "MINES";
        case EndlessStratumTheme::Catacombs:  return "CATACOMBS";
        default:                              return "RUINS";
    }
}

enum class MazeAlgorithm : uint8_t {
    None = 0,
    Backtracker,
    Wilson,
};

inline const char* mazeAlgorithmName(MazeAlgorithm a) {
    switch (a) {
        case MazeAlgorithm::Backtracker: return "BACKTRACKER";
        case MazeAlgorithm::Wilson:      return "WILSON";
        default:                         return "NONE";
    }
}


enum class TileType : uint8_t {
    Wall = 0,
    Floor,
    DoorClosed,
    DoorOpen,
    StairsUp,
    StairsDown,
    // Append-only: hidden until discovered by searching.
    DoorSecret,
    // Append-only: visible but requires a Key to open.
    DoorLocked,
    // Append-only: impassable terrain that does NOT block line-of-sight.
    // Used for pits/chasm features that shape room flow without creating "walls".
    Chasm,
    // Append-only: interior column that blocks movement and line-of-sight.
    Pillar,
    // Append-only: pushable boulder obstacle; blocks movement but does NOT block line-of-sight.
    Boulder,
    // Append-only: decorative/interactive feature (walkable) rendered as an overlay.
    Fountain,
    // Append-only: walkable altar overlay used to visually mark shrine rooms.
    Altar,
};

enum class TerrainMaterial : uint8_t {
    Stone = 0,
    Brick,
    Marble,
    Basalt,
    Obsidian,
    Moss,
    Dirt,
    Wood,
    Metal,
    Crystal,
    Bone,
    COUNT,
};

inline const char* terrainMaterialName(TerrainMaterial m) {
    switch (m) {
        case TerrainMaterial::Stone:    return "STONE";
        case TerrainMaterial::Brick:    return "BRICK";
        case TerrainMaterial::Marble:   return "MARBLE";
        case TerrainMaterial::Basalt:   return "BASALT";
        case TerrainMaterial::Obsidian: return "OBSIDIAN";
        case TerrainMaterial::Moss:     return "MOSS";
        case TerrainMaterial::Dirt:     return "DIRT";
        case TerrainMaterial::Wood:     return "WOOD";
        case TerrainMaterial::Metal:    return "METAL";
        case TerrainMaterial::Crystal:  return "CRYSTAL";
        case TerrainMaterial::Bone:     return "BONE";
        default:                        return "STONE";
    }
}

// Short adjective used for LOOK descriptions ("MOSSY FLOOR", "METALLIC WALL", ...).
inline const char* terrainMaterialAdj(TerrainMaterial m) {
    switch (m) {
        case TerrainMaterial::Moss:    return "MOSSY";
        case TerrainMaterial::Dirt:    return "EARTHEN";
        case TerrainMaterial::Wood:    return "WOODEN";
        case TerrainMaterial::Metal:   return "METALLIC";
        case TerrainMaterial::Crystal: return "CRYSTALLINE";
        case TerrainMaterial::Bone:    return "BONY";
        default:                       return terrainMaterialName(m);
    }
}

// Gameplay-adjacent effects of substrate materials.
// These are deliberately small nudges: the substrate field is primarily visual,
// but surfaces that are mossy/earthy dampen sound + scent, while metal/crystal
// makes noise travel farther.
struct TerrainMaterialFx {
    int footstepNoiseDelta = 0;   // additive to footstep emitNoise() volume
    int digNoiseDelta = 0;        // additive to dig emitNoise() volume
    int scentDecayDelta = 0;      // additive to global scent decay per turn
    int scentSpreadDropDelta = 0; // additive to scent spread drop per tile
};

inline TerrainMaterialFx terrainMaterialFx(TerrainMaterial m) {
    TerrainMaterialFx fx;
    switch (m) {
        case TerrainMaterial::Moss:
            fx.footstepNoiseDelta = -2;
            fx.digNoiseDelta = -2;
            fx.scentDecayDelta = 2;
            fx.scentSpreadDropDelta = 6;
            break;
        case TerrainMaterial::Dirt:
            fx.footstepNoiseDelta = -1;
            fx.digNoiseDelta = -1;
            fx.scentDecayDelta = 1;
            fx.scentSpreadDropDelta = 4;
            break;
        case TerrainMaterial::Wood:
            // Wooden boards can creak, but also absorb scent more than stone.
            fx.footstepNoiseDelta = 1;
            fx.digNoiseDelta = -1;
            fx.scentDecayDelta = 1;
            fx.scentSpreadDropDelta = 2;
            break;
        case TerrainMaterial::Metal:
            fx.footstepNoiseDelta = 2;
            fx.digNoiseDelta = 2;
            break;
        case TerrainMaterial::Crystal:
            fx.footstepNoiseDelta = 1;
            fx.digNoiseDelta = 1;
            break;
        case TerrainMaterial::Bone:
            fx.footstepNoiseDelta = 1; // crunch
            break;
        case TerrainMaterial::Basalt:
        case TerrainMaterial::Obsidian:
            fx.footstepNoiseDelta = 1;
            fx.digNoiseDelta = 1;
            break;
        default:
            break;
    }
    return fx;
}

struct Tile {
    TileType type = TileType::Wall;
    bool visible = false;
    bool explored = false;
};

enum class RoomType : uint8_t {
    Normal = 0,
    Treasure,
    Lair,
    Shrine,
    // Append-only: hidden treasure room accessed via a secret door.
    Secret,
    // Append-only: visible treasure room behind a locked door.
    Vault,
    // Append-only: merchant shop stocked with items for sale.
    Shop,

    // --- Themed rooms (append-only) ---
    // Moderate loot rooms that bias spawns toward a particular category.
    Armory,      // weapons / armor / ammo
    Library,     // scrolls / wands
    Laboratory,  // potions / strange hazards

    // Append-only: surface hub / above-ground camp (depth 0).
    Camp,
};

struct Room {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    RoomType type = RoomType::Normal;

    int x2() const { return x + w; }
    int y2() const { return y + h; }
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }

    bool contains(int px, int py) const {
        return px >= x && px < x2() && py >= y && py < y2();
    }
};

// Generation-only item spawn requests. These are set by the dungeon generator
// (vault prefabs, stash closets, setpieces) to guarantee specific ground items
// appear at specific tiles. They are applied during Game::spawnItems() and
// intentionally are NOT serialized.
struct BonusItemSpawn {
    Vec2i pos{0,0};
    ItemKind kind = static_cast<ItemKind>(0);
    int count = 1;
};


class Dungeon {
public:
    // Default map size.
    // Keep this in sync with Game::MAP_W/H (Game uses these values for window sizing
    // and various UI bounds).
    //
    // Round 19: bumped the map up again by +50% area (from 84x55 -> 105x66)
    // to support longer corridors, more interesting door play, and bigger
    // room layouts without cramping generation.
    static constexpr int DEFAULT_W = 105;
    static constexpr int DEFAULT_H = 66;

	// Themed floors: fixed depths that bias generation style.
	// These are still fully procedural; they're pacing anchors for run variety.
	static constexpr int MINES_DEPTH = 2;        // Procedural mines: winding tunnels + small chambers
	static constexpr int GROTTO_DEPTH = 4;       // Cavern-like floor with a subterranean lake feature
	static constexpr int DEEP_MINES_DEPTH = 7;   // Second mines-style floor deeper in the run
	static constexpr int CATACOMBS_DEPTH = 8;    // Grid-of-rooms + maze corridors (tomb/catacomb feel)

    int width = 0;
    int height = 0;
    std::vector<Tile> tiles;

    std::vector<Room> rooms;

// Procedural terrain materials (cosmetic only; not serialized).
// Cached per-cell material ids used for renderer tinting and LOOK descriptions.
mutable uint32_t materialCacheKey = 0u;
mutable int materialCacheW = 0;
mutable int materialCacheH = 0;
mutable int materialCacheCell = 0;
mutable std::vector<uint8_t> materialCache;


// Procedural bioluminescent terrain field (cosmetic only; not serialized).
// Stored as per-tile intensity 0..255 and computed alongside the material cache.
mutable std::vector<uint8_t> biolumCache;


    // Generator hints: optional guaranteed bonus loot spawns (e.g. boulder bridge caches).
    // Used only during floor generation; not serialized.
    std::vector<Vec2i> bonusLootSpots;
    // Generator hints: optional guaranteed item spawns (e.g. keys in keyed vault prefabs).
    // Used only during floor generation; not serialized.
    std::vector<BonusItemSpawn> bonusItemSpawns;
    // Generator flags (not serialized): used for callouts/tests.
    bool hasCavernLake = false;
    // Not serialized: cavern generator variant telemetry (Cavern floors).
    bool cavernMetaballsUsed = false;
    int cavernMetaballBlobCount = 0;
    int cavernMetaballKeptTiles = 0;
    // Not serialized: maze generator telemetry (Maze floors).
    MazeAlgorithm mazeAlgorithm = MazeAlgorithm::None;
    int mazeChamberCount = 0;
    int mazeBreakCount = 0;
    // Not serialized: Wilson's algorithm stats (only when mazeAlgorithm==Wilson).
    int mazeWilsonWalkCount = 0;
    int mazeWilsonStepCount = 0;
    int mazeWilsonLoopEraseCount = 0;
    int mazeWilsonMaxPathLen = 0;
    bool hasWarrens = false; // Organic burrow/tunnel generator.
    int secretShortcutCount = 0;
    // Not serialized: visible locked shortcut doors (DoorLocked) connecting adjacent corridors.
    int lockedShortcutCount = 0;
    // Not serialized: direct room-to-room doors carved between adjacent rooms (a late shortcut/loop pass).
    int interRoomDoorCount = 0;
    int interRoomDoorLockedCount = 0;
    int interRoomDoorSecretCount = 0;
    // Not serialized: corridor polish pass that widens some hallway junctions/segments.
    int corridorHubCount = 0;
    int corridorHallCount = 0;
    // Not serialized: micro-terrain hazards (sinkholes) carved as small chasm clusters.
    int sinkholeCount = 0;
    // Not serialized: optional "rift cache" pockets gated by a boulder-bridge micro-puzzle.
    int riftCacheCount = 0;
    int riftCacheBoulderCount = 0;
    int riftCacheChasmCount = 0;
    // Not serialized: multi-chamber "vault suite" prefab count (vaults with internal walls/doors).
    int vaultSuiteCount = 0;
    // Not serialized: small stash closets carved into dead-end corridors.
    int deadEndClosetCount = 0;
    // Not serialized: small handcrafted-style vault prefabs carved off corridor walls.
    int vaultPrefabCount = 0;
    // Not serialized: terrain sculpt pass edits (Wall<->Floor flips) applied after gen.
    int terrainSculptCount = 0;
    // Not serialized: corridor braiding pass tunnels carved (dead-end reduction / extra loops).
    int corridorBraidCount = 0;
    // Not serialized: optional "annex" micro-dungeons carved into wall pockets (mini-maze/cavern side areas).
    int annexCount = 0;
    // Not serialized: annex key-gate micro-puzzles (internal locked door + key inside annex).
    int annexKeyGateCount = 0;
    // Not serialized: annex Wave Function Collapse layouts (constraint-driven micro-dungeons).
    int annexWfcCount = 0;
    // Not serialized: annex fractal micro-dungeons (stochastic L-system / turtle branching layouts).
    int annexFractalCount = 0;
    // Not serialized: stairs-path connectivity analysis + weaving.
    // We compute how "tree-like" the passable graph is between the stairs, and
    // (optionally) carve a few tiny bypass loops around critical corridor bridges
    // to encourage alternate routes / flanking without rewriting the whole layout.
    int stairsBypassLoopCount = 0;
    // Not serialized: number of bridge edges on the current shortest path between
    // stairsUp and stairsDown (0 means there is no single-edge chokepoint).
    int stairsBridgeCount = 0;
    // Not serialized: true if stairsUp and stairsDown are in the same 2-edge-connected
    // component (i.e., there exist at least two edge-disjoint paths between them).
    bool stairsRedundancyOk = false;

    // Not serialized: global connectivity weaving.
    // Similar in spirit to the stairs-only weaving above, but applied to the *entire* passable
    // tile graph. This reduces major single-edge chokepoints anywhere on the floor by carving a
    // few tiny 2x2 bypass loops around the most impactful bridge edges (by component cut-size),
    // while still being conservative about where we dig (never inside rooms, never near stairs).
    int globalBypassLoopCount = 0;
    int globalBridgeCountBefore = 0;
    int globalBridgeCountAfter = 0;
    // Not serialized: meta-procgen selection stats (generate multiple candidates and pick the best).
    // This is a "generate-and-test" style pass that improves floor quality without changing gameplay rules.
    int genPickAttempts = 1;
    int genPickChosenIndex = 0;
    int genPickScore = 0;
    uint32_t genPickSeed = 0;

    // Not serialized: RoomsGraph ("ruins") generator stats.
    // These are useful for debugging/tuning the Poisson-disc + Delaunay pipeline.
    int roomsGraphPoissonPointCount = 0;   // number of Poisson-disc sampled candidate centers
    int roomsGraphPoissonRoomCount = 0;    // number of rooms actually placed from those centers
    int roomsGraphDelaunayEdgeCount = 0;   // number of Delaunay edges used as the connection graph
    int roomsGraphLoopEdgeCount = 0;       // number of extra loop edges added beyond the MST

    // Not serialized: biome-zone theming (Voronoi-style regions) for more coherent floor "flavors".
    // This is a lightweight post-pass that groups walkable space into a few contiguous regions and
    // applies a distinct obstacle/hazard style to each region without ever blocking stair connectivity.
    int biomeZoneCount = 0;
    int biomePillarZoneCount = 0;
    int biomeRubbleZoneCount = 0;
    int biomeCrackedZoneCount = 0;
    int biomeEdits = 0;

    // Not serialized: tactical "fire lane" dampening.
    // We measure the longest straight line where a projectile could travel without hitting cover,
    // then optionally insert small barricade chicanes (boulder + side-step bypass) to break extreme
    // sniper lanes without harming stair connectivity.
    int fireLaneMaxBefore = 0;
    int fireLaneMaxAfter = 0;
    int fireLaneCoverCount = 0;
    int fireLaneChicaneCount = 0;

    // Not serialized: open-space breakup / cover equalization.
    // We estimate how "wide open" the map is by computing a distance-to-obstacle
    // (clearance) field over passable tiles. When the max clearance is excessively
    // high (large empty kill-box rooms/caverns), a late post-pass can place a few
    // pillars/boulders at clearance maxima to add tactical occlusion/cover while
    // always preserving stair connectivity (rolls back on failure).
    int openSpaceClearanceMaxBefore = 0;
    int openSpaceClearanceMaxAfter = 0;
    int openSpacePillarCount = 0;
    int openSpaceBoulderCount = 0;

    // Not serialized: macro terrain heightfield pass (warped fBm).
    // Adds ridge-line pillar spines and scree boulder clusters biased by a deterministic height/slope field.
    int heightfieldRidgePillarCount = 0;
    int heightfieldScreeBoulderCount = 0;

    // Not serialized: fluvial erosion terrain (heightfield drainage network).
    // Carves a few narrow chasm "gullies" that follow a deterministic flow-accumulation field
    // over the macro heightfield, then preserves stairs connectivity by placing small
    // causeways/bridges as needed (or rolling back).
    int fluvialGullyCount = 0;      // Number of gullies (channels) successfully carved.
    int fluvialChasmCount = 0;      // Total tiles converted to Chasm by the pass.
    int fluvialCausewayCount = 0;   // Bridges/causeways placed to preserve connectivity.



    // Not serialized: perimeter service tunnels (inner-border maintenance corridors).
    //
    // A late procgen pass can carve partial tunnels along the map's inner border
    // (offset=1 from the solid wall boundary) and punch a few "maintenance hatches"
    // back into the interior. This creates alternate flanking routes on larger maps
    // without changing the critical stairs-to-stairs connectivity.
    int perimTunnelCarvedTiles = 0;   // Total Wall->Floor conversions performed by the pass.
    int perimTunnelHatchCount = 0;    // Number of hatches (doors) punched into the interior.
    int perimTunnelLockedCount = 0;   // Subset of hatches that are locked (DoorLocked).
    int perimTunnelCacheCount = 0;    // Bonus cache spots requested (bonusLootSpots).

    // Not serialized: burrow crosscuts (A*-dug wall tunnels)
    //
    // A late procgen pass can carve 1-2 long tunnels *through solid wall mass*
    // between two far-apart corridor points (outside any room rectangles), gated
    // by secret/locked doors. This creates dramatic optional shortcuts that are
    // fundamentally different from adjacent-wall shortcut doors (which only
    // bridge across a single wall tile).
    int crosscutTunnelCount = 0;       // Number of crosscut tunnels successfully carved.
    int crosscutCarvedTiles = 0;       // Total Wall->Floor conversions performed by the pass (excluding door tiles).
    int crosscutDoorLockedCount = 0;   // Number of locked doors used as tunnel gates.
    int crosscutDoorSecretCount = 0;   // Number of secret doors used as tunnel gates.

    // Not serialized: secret crawlspace networks (hidden wall passages).
    //
    // A late procgen pass can carve a small, 1-tile-wide network of tunnels
    // entirely inside solid wall mass, then connect it back to the main corridor
    // graph through 2-3 secret doors. This adds hidden alternate routes and
    // optional cache rewards without altering critical stair connectivity.
    int crawlspaceNetworkCount = 0;  // Number of crawlspace networks carved (0..1).
    int crawlspaceCarvedTiles = 0;   // Total Wall->Floor conversions performed by the pass.
    int crawlspaceDoorCount = 0;     // Number of secret doors (DoorSecret) used as entrances.
    int crawlspaceCacheCount = 0;    // Bonus cache spots requested (bonusLootSpots).


    // Not serialized: endless-depth macro theming ("strata") for Infinite World.
    // Derived from the run's worldSeed and the depth; used to create coherent regions of
    // generator bias across infinite descent (so deep floors feel like they belong to
    // larger themed bands rather than pure per-floor noise).
    int endlessStratumIndex = -1;       // 0-based within endless depths (depth > maxDepth)
    int endlessStratumStartDepth = -1;  // absolute depth where this stratum begins
    int endlessStratumLen = 0;          // number of floors in this stratum band
    int endlessStratumLocal = 0;        // 0..len-1 position within the stratum
    EndlessStratumTheme endlessStratumTheme = EndlessStratumTheme::Ruins;
    uint32_t endlessStratumSeed = 0u;

    // Not serialized: endless-depth persistent rift / faultline (Infinite World macro terrain).
    // This is a stratum-aligned, run-seeded ravine-like feature that drifts smoothly across
    // endless depths, giving a sense of large-scale geological continuity.
    bool endlessRiftActive = false;
    int endlessRiftIntensityPct = 0; // 0..100
    int endlessRiftChasmCount = 0;
    int endlessRiftBridgeCount = 0;
    int endlessRiftBoulderCount = 0;
    uint32_t endlessRiftSeed = 0u;

    // Not serialized: finite-run macro terrain continuity (Main branch, depth <= maxDepth).
    // A run-seeded "fault band" spans a small contiguous range of depths and carves
    // a drifting chasm seam with deterministic parameters, adding cross-floor
    // geological continuity to the fixed-length campaign without making every floor
    // feel like it must have a ravine.
    bool runFaultActive = false;
    int runFaultBandStartDepth = -1;
    int runFaultBandLen = 0;
    int runFaultBandLocal = 0;
    int runFaultIntensityPct = 0; // 0..100
    int runFaultChasmCount = 0;
    int runFaultBridgeCount = 0;
    int runFaultBoulderCount = 0;
    uint32_t runFaultSeed = 0u;

    // Not serialized: special-room setpieces (moated islands).
    // These carve a chasm ring inside select special rooms, leaving a central "island" reached
    // via 1-2 narrow bridges for tactical variety.
    int moatedRoomCount = 0;
    int moatedRoomBridgeCount = 0;
    int moatedRoomChasmCount = 0;

    // Not serialized: symmetric room furnishing pass (mirrored obstacle patterns).
    int symmetryRoomCount = 0;
    int symmetryObstacleCount = 0;

    // Not serialized: special-room placement analytics.
    // We classify rooms as being on the stairs critical path ("spine") or off it, then
    // bias shops/shrines to appear on-spine while treasure/lairs tend to spawn off-spine.
    // These values are for debugging/tuning only and do not affect saves.
    int spineRoomCount = 0;     // Unique rooms that intersect the shortest stairs path.
    int specialRoomMinSep = 0;  // Min BFS distance between representative tiles of special rooms.

    // Not serialized: surface camp stash anchor (depth 0).
    Vec2i campStashSpot{ -1, -1 };

    // Not serialized: surface camp landmarks (depth 0).
    // These are generation hints consumed by Game::setupSurfaceCampInstallations()
    // to add helpful map markers without expensive scanning.
    Vec2i campGateIn{ -1, -1 };
    Vec2i campGateOut{ -1, -1 };
    Vec2i campSideGateIn{ -1, -1 };
    Vec2i campSideGateOut{ -1, -1 };
    Vec2i campWellSpot{ -1, -1 };
    Vec2i campFireSpot{ -1, -1 };
    Vec2i campAltarSpot{ -1, -1 };

    Vec2i stairsUp{ -1, -1 };
    Vec2i stairsDown{ -1, -1 };

    Dungeon() = default;
    Dungeon(int w, int h);

    bool inBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }

    Tile& at(int x, int y) { return tiles[static_cast<size_t>(y * width + x)]; }
    const Tile& at(int x, int y) const { return tiles[static_cast<size_t>(y * width + x)]; }

    bool isWalkable(int x, int y) const;
    bool isPassable(int x, int y) const; // includes closed doors (AI/path)
    bool isOpaque(int x, int y) const;

    // Returns true if this tile blocks projectiles (ranged attacks, bolts).
    // Note: some tiles (like boulders) intentionally do NOT block line-of-sight
    // for readability, but still block projectiles for tactical cover.
    bool blocksProjectiles(int x, int y) const;
    bool isDoorClosed(int x, int y) const;
    bool isDoorLocked(int x, int y) const;
    bool isDoorOpen(int x, int y) const;
    void closeDoor(int x, int y);
    void openDoor(int x, int y);
    void lockDoor(int x, int y);
    void unlockDoor(int x, int y);

    // Terrain modification
    bool isDiggable(int x, int y) const;
    // Converts a diggable tile (wall/pillar/door variants) into floor.
    // Returns true if the tile changed.
    bool dig(int x, int y);

    // Procedural generation.
    //
    // `branch` selects the dungeon branch's layout rules (e.g. Camp hub vs Main dungeon).
    // `depth` is a branch-local depth used for pacing and special floors.
    void generate(RNG& rng, DungeonBranch branch, int depth, int maxDepth, uint32_t worldSeed = 0u);
    void computeEndlessStratumInfo(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth);

// Procedural terrain materials (cosmetic): computed deterministically from the run seed + depth.
// Call ensureMaterials() once (per floor/per frame) before querying materialAtCached() in tight loops.
void ensureMaterials(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const;
TerrainMaterial materialAt(int x, int y, uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const;
TerrainMaterial materialAtCached(int x, int y) const;
uint8_t biolumAt(int x, int y, uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) const;
uint8_t biolumAtCached(int x, int y) const;
int materialCellSize() const { return materialCacheCell; }


    // Convenience overload (legacy): generates using the Main branch rules.
    void generate(RNG& rng, int depth, int maxDepth);

    void computeFov(int px, int py, int radius, bool markExplored = true);

    // Computes a visibility mask (0/1) from (px,py) within `radius` using the same shadowcasting
    // as computeFov, but without mutating tiles.
    // `outMask` is resized and filled with 0/1, length = width*height.
    void computeFovMask(int px, int py, int radius, std::vector<uint8_t>& outMask) const;
    void revealAll();

    bool hasLineOfSight(int x0, int y0, int x1, int y1) const;

    // Compute a simple sound-propagation cost map from (sx, sy).
    //
    // Returns a width*height array of minimum "attenuation cost" from the source,
    // or -1 if unreachable.
    //
    // Walls and secret doors block sound. Closed/locked doors allow sound through
    // but are treated as muffling (higher cost).
    //
    // `maxCost` limits the search for efficiency; tiles beyond this cost remain -1.
    std::vector<int> computeSoundMap(int sx, int sy, int maxCost) const;

    // Sound propagation helpers.
    //
    // These expose the same rules used by computeSoundMap() so other systems
    // (e.g. stealth/hearing fields) can reuse the exact propagation model
    // without re-implementing the tile logic.
    bool soundPassable(int x, int y) const;
    int soundTileCost(int x, int y) const;
    bool soundDiagonalOk(int fromX, int fromY, int dx, int dy) const;

    Vec2i randomFloor(RNG& rng, bool avoidDoors = true) const;

private:
    bool lineOfSight(int x0, int y0, int x1, int y1) const;
};
