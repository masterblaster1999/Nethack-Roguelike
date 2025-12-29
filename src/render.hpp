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

    int winW = 0;
    int winH = 0;
    int tile = 32;
    int hudH = 160;
    bool vsyncEnabled = false;

    bool initialized = false;
	// Cached animation frame index for overlay/UI draws.
	int lastFrame = 0;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_PixelFormat* pixfmt = nullptr;

    // Tile textures (some have variants + animation frames)
    using AnimTex = std::array<SDL_Texture*, FRAMES>;
    std::vector<AnimTex> floorVar;
    std::vector<AnimTex> wallVar;
    std::vector<AnimTex> chasmVar;
    std::vector<AnimTex> pillarVar;

    std::array<SDL_Texture*, FRAMES> stairsUpTex{};
    std::array<SDL_Texture*, FRAMES> stairsDownTex{};
    std::array<SDL_Texture*, FRAMES> doorClosedTex{};
    std::array<SDL_Texture*, FRAMES> doorLockedTex{};
    std::array<SDL_Texture*, FRAMES> doorOpenTex{};

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

    SDL_Texture* tileTexture(TileType t, int x, int y, int level, int frame);
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
};
