#include "render.hpp"
#include "ui_font.hpp"
#include "rng.hpp"

#include <algorithm>
#include <iostream>

Renderer::Renderer(int windowW, int windowH, int tileSize, int hudHeight)
    : winW(windowW), winH(windowH), tile(tileSize), hudH(hudHeight) {}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::init() {
    if (initialized) return true;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // nearest-neighbor

    window = SDL_CreateWindow("ProcRogue",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              winW, winH,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return false;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        // fallback
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        return false;
    }

    pixfmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!pixfmt) {
        std::cerr << "SDL_AllocFormat failed: " << SDL_GetError() << "\n";
        return false;
    }

    // Pre-generate a handful of tile variants.
    const int VAR = 8;
    floorVar.reserve(VAR);
    wallVar.reserve(VAR);
    stairsVar.reserve(4);

    for (int i = 0; i < VAR; ++i) {
        uint32_t seed = hash32(static_cast<uint32_t>(i + 1) * 1337u);
        floorVar.push_back(textureFromSprite(generateFloorTile(seed)));
        wallVar.push_back(textureFromSprite(generateWallTile(seed ^ 0xC0FFEEu)));
    }

    for (int i = 0; i < 4; ++i) {
        uint32_t seed = hash32(static_cast<uint32_t>(i + 1) * 4242u);
        stairsVar.push_back(textureFromSprite(generateStairsTile(seed)));
    }

    initialized = true;
    return true;
}

void Renderer::shutdown() {
    if (!initialized) {
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        return;
    }

    for (SDL_Texture* t : floorVar) if (t) SDL_DestroyTexture(t);
    for (SDL_Texture* t : wallVar) if (t) SDL_DestroyTexture(t);
    for (SDL_Texture* t : stairsVar) if (t) SDL_DestroyTexture(t);
    floorVar.clear();
    wallVar.clear();
    stairsVar.clear();

    for (auto& kv : entityTex) {
        if (kv.second) SDL_DestroyTexture(kv.second);
    }
    entityTex.clear();

    if (pixfmt) { SDL_FreeFormat(pixfmt); pixfmt = nullptr; }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }

    initialized = false;
}

SDL_Texture* Renderer::textureFromSprite(const SpritePixels& s) {
    if (!renderer || !pixfmt) return nullptr;

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, s.w, s.h);
    if (!tex) return nullptr;

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    std::vector<uint32_t> mapped;
    mapped.resize(static_cast<size_t>(s.w * s.h));

    for (int i = 0; i < s.w * s.h; ++i) {
        const Color& c = s.px[static_cast<size_t>(i)];
        mapped[static_cast<size_t>(i)] = SDL_MapRGBA(pixfmt, c.r, c.g, c.b, c.a);
    }

    SDL_UpdateTexture(tex, nullptr, mapped.data(), s.w * static_cast<int>(sizeof(uint32_t)));
    return tex;
}

SDL_Texture* Renderer::tileTexture(TileType t, int x, int y, int level) {
    uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(level), static_cast<uint32_t>(x)),
                             static_cast<uint32_t>(y));
    switch (t) {
        case TileType::Floor:
            return floorVar.empty() ? nullptr : floorVar[h % floorVar.size()];
        case TileType::Wall:
            return wallVar.empty() ? nullptr : wallVar[h % wallVar.size()];
        case TileType::StairsDown:
            return stairsVar.empty() ? nullptr : stairsVar[h % stairsVar.size()];
        default:
            return nullptr;
    }
}

SDL_Texture* Renderer::entityTexture(const Entity& e) {
    uint64_t key = (static_cast<uint64_t>(e.kind) << 32) | static_cast<uint64_t>(e.spriteSeed);
    auto it = entityTex.find(key);
    if (it != entityTex.end()) return it->second;

    SDL_Texture* tex = textureFromSprite(generateEntitySprite(e.kind, e.spriteSeed));
    entityTex[key] = tex;
    return tex;
}

void Renderer::render(const Game& game) {
    if (!initialized) return;

    // Background clear
    SDL_SetRenderDrawColor(renderer, 8, 8, 12, 255);
    SDL_RenderClear(renderer);

    const Dungeon& d = game.dungeon();

    // Draw map tiles
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const Tile& t = d.at(x, y);

            SDL_Rect dst{ x * tile, y * tile, tile, tile };

            if (!t.explored) {
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderFillRect(renderer, &dst);
                continue;
            }

            SDL_Texture* tex = tileTexture(t.type, x, y, game.level());
            if (!tex) continue;

            if (t.visible) {
                SDL_SetTextureColorMod(tex, 255, 255, 255);
                SDL_SetTextureAlphaMod(tex, 255);
            } else {
                // Darken unseen-but-explored tiles.
                SDL_SetTextureColorMod(tex, 80, 80, 80);
                SDL_SetTextureAlphaMod(tex, 255);
            }
            SDL_RenderCopy(renderer, tex, nullptr, &dst);

            // Restore (important because textures are shared)
            SDL_SetTextureColorMod(tex, 255, 255, 255);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }

    // Draw entities (only if their tile is visible; player always visible)
    for (const auto& e : game.entities()) {
        if (!e.alive && e.kind != EntityKind::Player) continue;

        bool draw = true;
        if (e.kind != EntityKind::Player) {
            const Tile& tileAt = d.at(e.pos.x, e.pos.y);
            draw = tileAt.visible;
        }

        if (!draw) continue;

        SDL_Texture* tex = entityTexture(e);
        if (!tex) continue;

        SDL_Rect dst{ e.pos.x * tile, e.pos.y * tile, tile, tile };
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    // Player highlight
    {
        const Entity& p = game.player();
        SDL_Rect r{ p.pos.x * tile, p.pos.y * tile, tile, tile };
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 70);
        SDL_RenderDrawRect(renderer, &r);
    }

    drawHud(game);

    SDL_RenderPresent(renderer);
}

void Renderer::drawHud(const Game& game) {
    int hudTop = Game::MAP_H * tile;

    SDL_Rect panel{ 0, hudTop, winW, hudH };
    SDL_SetRenderDrawColor(renderer, 16, 16, 24, 255);
    SDL_RenderFillRect(renderer, &panel);

    // separator line
    SDL_SetRenderDrawColor(renderer, 70, 70, 90, 255);
    SDL_RenderDrawLine(renderer, 0, hudTop, winW, hudTop);

    const Entity& p = game.player();

    Color white{ 230, 230, 240, 255 };
    Color green{ 120, 240, 140, 255 };
    Color red{ 240, 120, 120, 255 };

    int scale = 2;
    int x = 10;
    int y = hudTop + 10;

    // HP + Level
    {
        std::string hp = "HP " + std::to_string(p.hp) + "/" + std::to_string(p.maxHp);
        std::string lvl = "LEVEL " + std::to_string(game.level());
        drawText5x7(renderer, x, y, scale, (p.hp > 0 ? green : red), hp);
        drawText5x7(renderer, x + 220, y, scale, white, lvl);
    }

    y += 7 * scale + 10;

    // Messages (last N)
    {
        const auto& msgs = game.messages();
        const int maxLines = 5;
        int start = std::max(0, static_cast<int>(msgs.size()) - maxLines);

        for (int i = start; i < static_cast<int>(msgs.size()); ++i) {
            drawText5x7(renderer, x, y, scale, white, toUpper(msgs[static_cast<size_t>(i)]));
            y += 7 * scale + 4;
        }
    }

    // Controls + game over
    int bottomY = hudTop + hudH - (7 * scale + 10);
    drawText5x7(renderer, x, bottomY, scale, white, "ARROWS/WASD MOVE  . WAIT  > STAIRS  R RESTART  ESC QUIT");

    if (game.isGameOver()) {
        drawText5x7(renderer, winW - 270, hudTop + 10, scale, red, "GAME OVER");
    }
}
