#pragma once
#include "common.hpp"
#include "rng.hpp"
#include <cstdint>
#include <vector>

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
};

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

    // Special floors: fixed-depth handcrafted / alternate generators.
    // These are expressed here so game logic (callouts) and tests can share them.
    static constexpr int SOKOBAN_DEPTH = 3;      // Sokoban-style boulder bridge puzzle floor
    static constexpr int GROTTO_DEPTH = 4;      // Cavern-like floor with a subterranean lake feature
    static constexpr int ROGUE_LEVEL_DEPTH = 6;  // Classic 3x3-room Rogue homage
    static constexpr int MINES_DEPTH = 2;        // Procedural mines: winding tunnels + small chambers
    static constexpr int DEEP_MINES_DEPTH = 7;   // Second mines-style floor deeper in the run
    static constexpr int CATACOMBS_DEPTH = 8;   // Grid-of-rooms + maze corridors (tomb/catacomb feel)

    int width = 0;
    int height = 0;
    std::vector<Tile> tiles;

    std::vector<Room> rooms;

    // Generator hints: optional guaranteed bonus loot spawns (e.g. boulder bridge caches).
    // Used only during floor generation; not serialized.
    std::vector<Vec2i> bonusLootSpots;
    // Generator flags (not serialized): used for callouts/tests.
    bool hasCavernLake = false;
    bool hasWarrens = false; // Organic burrow/tunnel generator.
    int secretShortcutCount = 0;
    // Not serialized: visible locked shortcut doors (DoorLocked) connecting adjacent corridors.
    int lockedShortcutCount = 0;
    // Not serialized: corridor polish pass that widens some hallway junctions/segments.
    int corridorHubCount = 0;
    int corridorHallCount = 0;
    // Not serialized: micro-terrain hazards (sinkholes) carved as small chasm clusters.
    int sinkholeCount = 0;
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
    // Not serialized: surface camp stash anchor (depth 0).
    Vec2i campStashSpot{ -1, -1 };
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
    // `depth` is used to vary generation style (rooms vs caverns vs mazes)
    // and difficulty pacing.
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

    Vec2i randomFloor(RNG& rng, bool avoidDoors = true) const;

private:
    bool lineOfSight(int x0, int y0, int x1, int y1) const;
};
