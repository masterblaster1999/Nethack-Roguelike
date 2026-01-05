#pragma once
#include "sdl.hpp"

#include "dungeon.hpp"
#include "game.hpp"
#include "items.hpp"
#include "spritegen.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

// A tiny, self-contained LRU texture cache.
//
// This project can generate high-resolution procedural sprites (up to 256x256).
// Caching every unique (kind, seed) sprite forever can balloon VRAM usage.
// This cache caps memory usage (approximately) and evicts least-recently-used
// entries when the budget is exceeded.
//
// NOTE: SDL textures must be destroyed on the same thread that owns the SDL
// renderer on some backends. This cache assumes it is accessed from the main
// thread only (which is how rendering is done).
template <size_t N>
class LRUTextureCache {
public:
    struct Entry {
        std::array<SDL_Texture*, N> tex{};
        size_t bytes = 0;
        typename std::list<uint64_t>::iterator lruIt;
    };

    LRUTextureCache() = default;
    ~LRUTextureCache() { clear(); }

    LRUTextureCache(const LRUTextureCache&) = delete;
    LRUTextureCache& operator=(const LRUTextureCache&) = delete;

    void setBudgetBytes(size_t bytes) {
        budgetBytes_ = bytes;
        trim();
    }

    size_t budgetBytes() const { return budgetBytes_; }
    size_t usedBytes() const { return usedBytes_; }
    size_t size() const { return map_.size(); }

    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t evictions() const { return evictions_; }

    void resetStats() {
        hits_ = 0;
        misses_ = 0;
        evictions_ = 0;
    }

    // Returns a pointer to the cached texture array, or nullptr if not found.
    std::array<SDL_Texture*, N>* get(uint64_t key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            ++misses_;
            return nullptr;
        }
        ++hits_;
        touch(it);
        return &it->second.tex;
    }

    // Inserts (or replaces) an entry, then trims to budget.
    void put(uint64_t key, const std::array<SDL_Texture*, N>& tex, size_t bytes) {
        // Replace existing.
        auto it = map_.find(key);
        if (it != map_.end()) {
            const size_t oldBytes = it->second.bytes;
            usedBytes_ = (usedBytes_ >= oldBytes) ? (usedBytes_ - oldBytes) : 0;
            destroyEntry(it->second);
            lru_.erase(it->second.lruIt);
            map_.erase(it);
        }

        lru_.push_front(key);
        Entry e;
        e.tex = tex;
        e.bytes = bytes;
        e.lruIt = lru_.begin();
        map_.emplace(key, e);
        usedBytes_ += bytes;

        trim();
    }

    void clear() {
        for (auto& kv : map_) {
            destroyEntry(kv.second);
        }
        map_.clear();
        lru_.clear();
        usedBytes_ = 0;
        resetStats();
    }

    // Simple category stats helper: interpret the top byte as a category id.
    void countByCategory(size_t& cat1, size_t& cat2, size_t& cat3) const {
        cat1 = cat2 = cat3 = 0;
        for (const auto& kv : map_) {
            const uint8_t cat = static_cast<uint8_t>(kv.first >> 56);
            if (cat == 1) ++cat1;
            else if (cat == 2) ++cat2;
            else if (cat == 3) ++cat3;
        }
    }

private:
    using MapT = std::unordered_map<uint64_t, Entry>;
    void touch(typename MapT::iterator it) {
        const uint64_t key = it->first;
        lru_.erase(it->second.lruIt);
        lru_.push_front(key);
        it->second.lruIt = lru_.begin();
    }

    void destroyEntry(Entry& e) {
        for (SDL_Texture*& t : e.tex) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
        e.bytes = 0;
    }

    void trim() {
        // 0 means "unlimited" (no eviction).
        if (budgetBytes_ == 0) return;

        while (usedBytes_ > budgetBytes_ && !lru_.empty()) {
            const uint64_t victim = lru_.back();
            lru_.pop_back();

            auto it = map_.find(victim);
            if (it == map_.end()) continue;

            const size_t oldBytes = it->second.bytes;
            usedBytes_ = (usedBytes_ >= oldBytes) ? (usedBytes_ - oldBytes) : 0;
            destroyEntry(it->second);
            map_.erase(it);
            ++evictions_;
        }
    }

    MapT map_;
    std::list<uint64_t> lru_;
    size_t budgetBytes_ = 0;
    size_t usedBytes_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    uint64_t evictions_ = 0;
};

class Renderer {
public:
    Renderer(int windowW, int windowH, int tileSize, int hudHeight, bool vsync, int textureCacheMB);
    ~Renderer();

    bool init();
    void shutdown();

    void render(const Game& game);

    // Window controls
    void toggleFullscreen();

    // Input helpers
    // Converts a window pixel coordinate to a map tile coordinate.
    // Returns false if the coordinate is outside the map region.
    bool windowToMapTile(int winX, int winY, int& tileX, int& tileY) const;

    // Screenshot helper: saves a BMP of the current frame.
    // Returns the full path written, or an empty string on failure.
    std::string saveScreenshotBMP(const std::string& directory, const std::string& prefix = "procrogue_shot") const;

private:
    static constexpr int FRAMES = 2;

    // Room-style / decal style count.
    // style mapping (matches renderer helpers + spritegen themed floors):
    //  0 = Normal, 1 = Treasure, 2 = Lair, 3 = Shrine, 4 = Secret, 5 = Vault, 6 = Shop
    static constexpr int ROOM_STYLES = 7;

    int winW = 0;
    int winH = 0;
    int tile = 32;
    int hudH = 160;
    bool vsyncEnabled = false;

    // Viewport size in tiles (derived from winW/winH and tile size).
    // When this is smaller than the dungeon dimensions, a scrolling camera is used.
    int viewTilesW = 0;
    int viewTilesH = 0;

    // Camera top-left (in map tiles). Only meaningful when the viewport is smaller than the map.
    int camX = 0;
    int camY = 0;

    bool initialized = false;
	// Cached animation frame index for overlay/UI draws.
	int lastFrame = 0;

    // Transient map render offset (used for screen shake / impact FX).
    // This is applied to map-space primitives (tiles/entities/items/FX), but NOT to the HUD/UI.
    int mapOffX = 0;
    int mapOffY = 0;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_PixelFormat* pixfmt = nullptr;

    // Tile textures (some have variants + animation frames)
    using AnimTex = std::array<SDL_Texture*, FRAMES>;
    // Floors have a themed tileset per room style.
    std::array<std::vector<AnimTex>, ROOM_STYLES> floorThemeVar;
    std::vector<AnimTex> wallVar;
    std::vector<AnimTex> chasmVar;
    // Pillars are rendered as an overlay on top of the underlying floor so the
    // pillar always matches the room's themed floor.
    std::vector<AnimTex> pillarOverlayVar;
    // Boulders are also overlays so they inherit the underlying room floor theme.
    std::vector<AnimTex> boulderOverlayVar;

    // Extra tile overlays / decals (transparent textures)
    static constexpr int DECAL_STYLES = ROOM_STYLES;
    static constexpr int DECALS_PER_STYLE = 6;
    // Runtime-scaled subset of decals to reduce VRAM for huge tiles.
    int decalsPerStyleUsed = DECALS_PER_STYLE;
    std::vector<AnimTex> floorDecalVar;
    std::vector<AnimTex> wallDecalVar;

    // Autotile overlays (transparent, layered on top of base tiles)
    // openMask bits: 1=N, 2=E, 4=S, 8=W (bit set means "edge exposed")
    static constexpr int AUTO_MASKS = 16;
    static constexpr int AUTO_VARS = 4;
    // Runtime-scaled subset of autotile variants (same goal as decalsPerStyleUsed).
    int autoVarsUsed = AUTO_VARS;
    std::array<std::array<AnimTex, AUTO_VARS>, AUTO_MASKS> wallEdgeVar{};
    std::array<std::array<AnimTex, AUTO_VARS>, AUTO_MASKS> chasmRimVar{};

    // Confusion gas overlay (procedurally generated animated tiles)
    static constexpr int GAS_VARS = 8;
    std::array<AnimTex, GAS_VARS> gasVar{};

    // Fire overlay (procedurally generated animated tiles)
    static constexpr int FIRE_VARS = 8;
    std::array<AnimTex, FIRE_VARS> fireVar{};

    // HUD status icons (procedurally generated)
    std::array<std::array<SDL_Texture*, FRAMES>, EFFECT_KIND_COUNT> effectIconTex{};

    // Cached room type per tile (for themed decals / minimap)
    const Dungeon* roomCacheDungeon = nullptr;
    int roomCacheDepth = -1;
    int roomCacheW = 0;
    int roomCacheH = 0;
    size_t roomCacheRooms = 0;
    std::vector<uint8_t> roomTypeCache;

    // Door/stairs are also rendered as overlays on top of the underlying floor.
    std::array<SDL_Texture*, FRAMES> stairsUpOverlayTex{};
    std::array<SDL_Texture*, FRAMES> stairsDownOverlayTex{};
    std::array<SDL_Texture*, FRAMES> doorClosedOverlayTex{};
    std::array<SDL_Texture*, FRAMES> doorLockedOverlayTex{};
    std::array<SDL_Texture*, FRAMES> doorOpenOverlayTex{};

    // UI skin textures (procedurally generated; refreshed if theme changes)
    bool uiAssetsValid = false;
    UITheme uiThemeCached = UITheme::DarkStone;
    // Cached sprite mode (2D vs 3D voxel-extruded) so we can invalidate caches on change.
    bool voxelSpritesCached = true;
    std::array<SDL_Texture*, FRAMES> uiPanelTileTex{};
    std::array<SDL_Texture*, FRAMES> uiOrnamentTex{};

    // Sprite texture cache (entities/items/projectiles), budgeted + LRU-evicted.
    // Key layout (uint64): [cat:8][kind:8][seed:32][unused:16]
    LRUTextureCache<FRAMES> spriteTex;
    int textureCacheMB = 0;
    size_t spriteEntryBytes = 0;

    // Map-space -> screen-space helpers (respect camera + screen shake).
    SDL_Rect mapTileDst(int mapX, int mapY) const;
    bool mapTileInView(int mapX, int mapY) const;

    // Updates camera position based on player/cursor and current viewport size.
    void updateCamera(const Game& game);

    SDL_Texture* textureFromSprite(const SpritePixels& s);

    SDL_Texture* tileTexture(TileType t, int x, int y, int level, int frame, int roomStyle);
    SDL_Texture* entityTexture(const Entity& e, int frame);
    SDL_Texture* itemTexture(const Item& it, int frame);

    // Small UI helper: draw a tiny item sprite (used in inventory/chest lists).
    void drawItemIcon(const Game& game, const Item& it, int x, int y, int px);
    SDL_Texture* projectileTexture(ProjectileKind k, int frame);

    // UI skin helpers
    void ensureUIAssets(const Game& game);
    void drawPanel(const Game& game, const SDL_Rect& rect, uint8_t alpha, int frame);

    void drawHud(const Game& game);
    void drawInventoryOverlay(const Game& game);
    void drawChestOverlay(const Game& game);
    void drawOptionsOverlay(const Game& game);
    void drawKeybindsOverlay(const Game& game);
    void drawCommandOverlay(const Game& game);
    void drawHelpOverlay(const Game& game);
    void drawMessageHistoryOverlay(const Game& game);
    void drawCodexOverlay(const Game& game);
    void drawDiscoveriesOverlay(const Game& game);
    void drawTargetingOverlay(const Game& game);
    void drawLookOverlay(const Game& game);

    // New overlays
    void drawMinimapOverlay(const Game& game);
    void drawStatsOverlay(const Game& game);
    void drawLevelUpOverlay(const Game& game);
};
