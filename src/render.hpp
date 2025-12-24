#pragma once
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "game.hpp"
#include "spritegen.hpp"

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

    bool ok() const { return initialized; }

    void render(const Game& game);

private:
    int winW = 0;
    int winH = 0;
    int tile = 32;
    int hudH = 160;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    SDL_PixelFormat* pixfmt = nullptr;

    bool initialized = false;

    // Tile texture variants
    std::vector<SDL_Texture*> floorVar;
    std::vector<SDL_Texture*> wallVar;
    std::vector<SDL_Texture*> stairsVar;

    // Entity textures, keyed by (kind, seed)
    std::unordered_map<uint64_t, SDL_Texture*> entityTex;

    SDL_Texture* textureFromSprite(const SpritePixels& s);

    SDL_Texture* tileTexture(TileType t, int x, int y, int level);
    SDL_Texture* entityTexture(const Entity& e);

    void drawHud(const Game& game);
};
