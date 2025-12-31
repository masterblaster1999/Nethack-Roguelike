#pragma once
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "dungeon.hpp"
#include "game.hpp"
#include "items.hpp"
#include "spritegen.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class Renderer {
public:
    Renderer(int windowW, int windowH, int tileSize, int hudHeight, bool vsync);
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

    // Extra tile overlays / decals (transparent textures)
    static constexpr int DECAL_STYLES = ROOM_STYLES;
    static constexpr int DECALS_PER_STYLE = 6;
    std::vector<AnimTex> floorDecalVar;
    std::vector<AnimTex> wallDecalVar;

    // Autotile overlays (transparent, layered on top of base tiles)
    // openMask bits: 1=N, 2=E, 4=S, 8=W (bit set means "edge exposed")
    static constexpr int AUTO_MASKS = 16;
    static constexpr int AUTO_VARS = 4;
    std::array<std::array<AnimTex, AUTO_VARS>, AUTO_MASKS> wallEdgeVar{};
    std::array<std::array<AnimTex, AUTO_VARS>, AUTO_MASKS> chasmRimVar{};

    // Confusion gas overlay (procedurally generated animated tiles)
    static constexpr int GAS_VARS = 8;
    std::array<AnimTex, GAS_VARS> gasVar{};

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
    std::array<SDL_Texture*, FRAMES> uiPanelTileTex{};
    std::array<SDL_Texture*, FRAMES> uiOrnamentTex{};

    // Entity / item / projectile textures (keyed by kind+seed)
    std::unordered_map<uint64_t, std::array<SDL_Texture*, FRAMES>> entityTex;
    std::unordered_map<uint64_t, std::array<SDL_Texture*, FRAMES>> itemTex;
    std::unordered_map<uint64_t, std::array<SDL_Texture*, FRAMES>> projTex;

    SDL_Texture* textureFromSprite(const SpritePixels& s);

    SDL_Texture* tileTexture(TileType t, int x, int y, int level, int frame, int roomStyle);
    SDL_Texture* entityTexture(const Entity& e, int frame);
    SDL_Texture* itemTexture(const Item& it, int frame);
    SDL_Texture* projectileTexture(ProjectileKind k, int frame);

    // UI skin helpers
    void ensureUIAssets(const Game& game);
    void drawPanel(const Game& game, const SDL_Rect& rect, uint8_t alpha, int frame);

    void drawHud(const Game& game);
    void drawInventoryOverlay(const Game& game);
    void drawOptionsOverlay(const Game& game);
    void drawCommandOverlay(const Game& game);
    void drawHelpOverlay(const Game& game);
    void drawTargetingOverlay(const Game& game);
    void drawLookOverlay(const Game& game);

    // New overlays
    void drawMinimapOverlay(const Game& game);
    void drawStatsOverlay(const Game& game);
    void drawLevelUpOverlay(const Game& game);
};
