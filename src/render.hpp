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
    Renderer(int windowW, int windowH, int tileSize, int hudHeight);
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

    bool initialized = false;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_PixelFormat* pixfmt = nullptr;

    // Tile textures (some have variants)
    std::vector<SDL_Texture*> floorVar;
    std::vector<SDL_Texture*> wallVar;

    std::array<SDL_Texture*, FRAMES> stairsUpTex{};
    std::array<SDL_Texture*, FRAMES> stairsDownTex{};
    std::array<SDL_Texture*, FRAMES> doorClosedTex{};
    std::array<SDL_Texture*, FRAMES> doorOpenTex{};

    // Entity / item / projectile textures (keyed by kind+seed)
    std::unordered_map<uint64_t, std::array<SDL_Texture*, FRAMES>> entityTex;
    std::unordered_map<uint64_t, std::array<SDL_Texture*, FRAMES>> itemTex;
    std::unordered_map<uint64_t, std::array<SDL_Texture*, FRAMES>> projTex;

    SDL_Texture* textureFromSprite(const SpritePixels& s);

    SDL_Texture* tileTexture(TileType t, int x, int y, int level, int frame);
    SDL_Texture* entityTexture(const Entity& e, int frame);
    SDL_Texture* itemTexture(const Item& it, int frame);
    SDL_Texture* projectileTexture(ProjectileKind k, int frame);

    void drawHud(const Game& game);
    void drawInventoryOverlay(const Game& game);
    void drawHelpOverlay(const Game& game);
    void drawTargetingOverlay(const Game& game);
    void drawLookOverlay(const Game& game);

    // New overlays
    void drawMinimapOverlay(const Game& game);
    void drawStatsOverlay(const Game& game);
};
