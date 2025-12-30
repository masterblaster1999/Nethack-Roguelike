#include "render.hpp"
#include "ui_font.hpp"
#include "rng.hpp"
#include "version.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <iomanip>

Renderer::Renderer(int windowW, int windowH, int tileSize, int hudHeight, bool vsync)
    : winW(windowW), winH(windowH), tile(tileSize), hudH(hudHeight), vsyncEnabled(vsync) {}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::init() {
    if (initialized) return true;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // nearest-neighbor

    const std::string title = std::string(PROCROGUE_APPNAME) + " v" + PROCROGUE_VERSION;
    window = SDL_CreateWindow(title.c_str(),
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              winW, winH,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return false;
    }

    Uint32 rFlags = SDL_RENDERER_ACCELERATED;
    if (vsyncEnabled) rFlags |= SDL_RENDERER_PRESENTVSYNC;
    renderer = SDL_CreateRenderer(window, -1, rFlags);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window); window = nullptr;
        return false;
    }

    // Keep a fixed "virtual" resolution and let SDL scale the final output.
    // This makes the window resizable while preserving crisp pixel art.
    SDL_RenderSetLogicalSize(renderer, winW, winH);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);

    pixfmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!pixfmt) {
        std::cerr << "SDL_AllocFormat failed\n";
        shutdown();
        return false;
    }

// Pre-generate a few tile variants (with animation frames)
const int tileVars = 10;
floorVar.clear();
wallVar.clear();
chasmVar.clear();
pillarVar.clear();
floorVar.resize(static_cast<size_t>(tileVars));
wallVar.resize(static_cast<size_t>(tileVars));
chasmVar.resize(static_cast<size_t>(tileVars));
pillarVar.resize(static_cast<size_t>(tileVars));

for (int i = 0; i < tileVars; ++i) {
    const uint32_t fSeed = hashCombine(0xF1000u, static_cast<uint32_t>(i));
    const uint32_t wSeed = hashCombine(0xAA110u, static_cast<uint32_t>(i));
    const uint32_t cSeed = hashCombine(0xC1A500u, static_cast<uint32_t>(i));
    const uint32_t pSeed = hashCombine(0x9111A0u, static_cast<uint32_t>(i));

    for (int f = 0; f < FRAMES; ++f) {
        floorVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateFloorTile(fSeed, f));
        wallVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateWallTile(wSeed, f));
        chasmVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateChasmTile(cSeed, f));
        pillarVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generatePillarTile(pSeed, f));
    }
}

    for (int f = 0; f < FRAMES; ++f) {
        stairsUpTex[static_cast<size_t>(f)]   = textureFromSprite(generateStairsTile(0x515A1u, true, f));
        stairsDownTex[static_cast<size_t>(f)] = textureFromSprite(generateStairsTile(0x515A2u, false, f));
        doorClosedTex[static_cast<size_t>(f)] = textureFromSprite(generateDoorTile(0xD00Du, false, f));
        doorLockedTex[static_cast<size_t>(f)] = textureFromSprite(generateLockedDoorTile(0xD00Du, f));
        doorOpenTex[static_cast<size_t>(f)]   = textureFromSprite(generateDoorTile(0xD00Du, true, f));
    }

// Default UI skin assets (will refresh if theme changes at runtime).
uiThemeCached = UITheme::DarkStone;
uiAssetsValid = true;
for (int f = 0; f < FRAMES; ++f) {
    uiPanelTileTex[static_cast<size_t>(f)] = textureFromSprite(generateUIPanelTile(uiThemeCached, 0x51A11u, f));
    uiOrnamentTex[static_cast<size_t>(f)]  = textureFromSprite(generateUIOrnamentTile(uiThemeCached, 0x0ABCDu, f));
}

    initialized = true;
    return true;
}

void Renderer::shutdown() {
    if (!initialized) {
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        return;
    }

for (auto& arr : floorVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : wallVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : chasmVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : pillarVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
floorVar.clear();
wallVar.clear();
chasmVar.clear();
pillarVar.clear();

for (auto& t : uiPanelTileTex) if (t) SDL_DestroyTexture(t);
for (auto& t : uiOrnamentTex) if (t) SDL_DestroyTexture(t);
uiPanelTileTex.fill(nullptr);
uiOrnamentTex.fill(nullptr);
uiAssetsValid = false;

    for (auto& t : stairsUpTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : stairsDownTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorClosedTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorLockedTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorOpenTex) if (t) SDL_DestroyTexture(t);

    stairsUpTex.fill(nullptr);
    stairsDownTex.fill(nullptr);
    doorClosedTex.fill(nullptr);
    doorLockedTex.fill(nullptr);
    doorOpenTex.fill(nullptr);

    for (auto& kv : entityTex) {
        for (SDL_Texture*& t : kv.second) if (t) SDL_DestroyTexture(t);
    }
    entityTex.clear();

    for (auto& kv : itemTex) {
        for (SDL_Texture*& t : kv.second) if (t) SDL_DestroyTexture(t);
    }
    itemTex.clear();

    for (auto& kv : projTex) {
        for (SDL_Texture*& t : kv.second) if (t) SDL_DestroyTexture(t);
    }
    projTex.clear();

    if (pixfmt) { SDL_FreeFormat(pixfmt); pixfmt = nullptr; }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }

    initialized = false;
}

void Renderer::toggleFullscreen() {
    if (!window) return;
    const Uint32 flags = SDL_GetWindowFlags(window);
    const bool isFs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(window, isFs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

bool Renderer::windowToMapTile(int winX, int winY, int& tileX, int& tileY) const {
    if (!renderer) return false;

    float lx = 0.0f, ly = 0.0f;
    SDL_RenderWindowToLogical(renderer, winX, winY, &lx, &ly);

    const int x = static_cast<int>(lx);
    const int y = static_cast<int>(ly);

    if (x < 0 || y < 0) return false;

    tileX = x / tile;
    tileY = y / tile;

    if (tileX < 0 || tileY < 0 || tileX >= Game::MAP_W || tileY >= Game::MAP_H) return false;
    return true;
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

SDL_Texture* Renderer::tileTexture(TileType t, int x, int y, int level, int frame) {
    uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(level), static_cast<uint32_t>(x)),
                             static_cast<uint32_t>(y));

    switch (t) {
        case TileType::Floor: {
            if (floorVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(floorVar.size()));
            return floorVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::Wall: {
            if (wallVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(wallVar.size()));
            return wallVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::Chasm: {
            if (chasmVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(chasmVar.size()));
            return chasmVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::Pillar: {
            if (pillarVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(pillarVar.size()));
            return pillarVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::DoorSecret: {
            // Draw secret doors as walls until discovered (tile is converted to DoorClosed).
            if (wallVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(wallVar.size()));
            return wallVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::StairsUp:
            return stairsUpTex[static_cast<size_t>(frame % FRAMES)];
        case TileType::StairsDown:
            return stairsDownTex[static_cast<size_t>(frame % FRAMES)];
        case TileType::DoorClosed:
            return doorClosedTex[static_cast<size_t>(frame % FRAMES)];
        case TileType::DoorLocked:
            return doorLockedTex[static_cast<size_t>(frame % FRAMES)];
        case TileType::DoorOpen:
            return doorOpenTex[static_cast<size_t>(frame % FRAMES)];
        default:
            return nullptr;
    }
}

SDL_Texture* Renderer::entityTexture(const Entity& e, int frame) {
    uint64_t key = (static_cast<uint64_t>(e.kind) << 32) | static_cast<uint64_t>(e.spriteSeed);
    auto it = entityTex.find(key);
    if (it == entityTex.end()) {
        std::array<SDL_Texture*, FRAMES> arr{};
        arr.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            arr[static_cast<size_t>(f)] = textureFromSprite(generateEntitySprite(e.kind, e.spriteSeed, f));
        }
        it = entityTex.emplace(key, arr).first;
    }
    return it->second[static_cast<size_t>(frame % FRAMES)];
}

SDL_Texture* Renderer::itemTexture(const Item& it, int frame) {
    uint64_t key = (static_cast<uint64_t>(it.kind) << 32) | static_cast<uint64_t>(it.spriteSeed);
    auto itex = itemTex.find(key);
    if (itex == itemTex.end()) {
        std::array<SDL_Texture*, FRAMES> arr{};
        arr.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            arr[static_cast<size_t>(f)] = textureFromSprite(generateItemSprite(it.kind, it.spriteSeed, f));
        }
        itex = itemTex.emplace(key, arr).first;
    }
    return itex->second[static_cast<size_t>(frame % FRAMES)];
}

SDL_Texture* Renderer::projectileTexture(ProjectileKind k, int frame) {
    uint64_t key = static_cast<uint64_t>(k);
    auto it = projTex.find(key);
    if (it == projTex.end()) {
        std::array<SDL_Texture*, FRAMES> arr{};
        arr.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            arr[static_cast<size_t>(f)] = textureFromSprite(generateProjectileSprite(k, 0u, f));
        }
        it = projTex.emplace(key, arr).first;
    }
    return it->second[static_cast<size_t>(frame % FRAMES)];
}

void Renderer::ensureUIAssets(const Game& game) {
    if (!initialized) return;

    const UITheme want = game.uiTheme();
    if (uiAssetsValid && want == uiThemeCached) return;

    for (auto& t : uiPanelTileTex) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
    for (auto& t : uiOrnamentTex) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }

    uiThemeCached = want;

    for (int f = 0; f < FRAMES; ++f) {
        uiPanelTileTex[static_cast<size_t>(f)] = textureFromSprite(generateUIPanelTile(uiThemeCached, 0x51A11u, f));
        uiOrnamentTex[static_cast<size_t>(f)]  = textureFromSprite(generateUIOrnamentTile(uiThemeCached, 0x0ABCDu, f));
    }

    uiAssetsValid = true;
}

static Color uiBorderForTheme(UITheme theme) {
    switch (theme) {
        case UITheme::DarkStone: return {180, 200, 235, 255};
        case UITheme::Parchment: return {235, 215, 160, 255};
        case UITheme::Arcane:    return {230, 170, 255, 255};
    }
    return {200, 200, 200, 255};
}

void Renderer::drawPanel(const Game& game, const SDL_Rect& rect, uint8_t alpha, int frame) {
    if (!renderer) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Drop shadow (subtle)
    SDL_Rect shadow{ rect.x + 2, rect.y + 2, rect.w, rect.h };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<Uint8>(std::min<int>(alpha, 200) / 2));
    SDL_RenderFillRect(renderer, &shadow);

    if (game.uiPanelsTextured()) {
        ensureUIAssets(game);

        SDL_Texture* tileTex = uiPanelTileTex[static_cast<size_t>(frame % FRAMES)];
        if (tileTex) {
            Uint8 oldA = 255;
            SDL_GetTextureAlphaMod(tileTex, &oldA);
            SDL_SetTextureAlphaMod(tileTex, alpha);

            SDL_RenderSetClipRect(renderer, &rect);
            const int step = 16;
            for (int y = rect.y; y < rect.y + rect.h; y += step) {
                for (int x = rect.x; x < rect.x + rect.w; x += step) {
                    SDL_Rect dst{ x, y, step, step };
                    SDL_RenderCopy(renderer, tileTex, nullptr, &dst);
                }
            }
            SDL_RenderSetClipRect(renderer, nullptr);

            SDL_SetTextureAlphaMod(tileTex, oldA);
        } else {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
            SDL_RenderFillRect(renderer, &rect);
        }
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
        SDL_RenderFillRect(renderer, &rect);
    }

    const Color border = uiBorderForTheme(game.uiTheme());
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, static_cast<Uint8>(std::min<int>(alpha + 40, 255)));
    SDL_RenderDrawRect(renderer, &rect);

    if (game.uiPanelsTextured()) {
        SDL_Texture* orn = uiOrnamentTex[static_cast<size_t>(frame % FRAMES)];
        if (orn) {
            Uint8 oldA = 255;
            SDL_GetTextureAlphaMod(orn, &oldA);
            SDL_SetTextureAlphaMod(orn, static_cast<Uint8>(std::min<int>(alpha, 220)));

            const int os = 16;
            SDL_Rect dstTL{ rect.x, rect.y, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstTL, 0.0, nullptr, SDL_FLIP_NONE);

            SDL_Rect dstTR{ rect.x + rect.w - os, rect.y, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstTR, 0.0, nullptr, SDL_FLIP_HORIZONTAL);

            SDL_Rect dstBL{ rect.x, rect.y + rect.h - os, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstBL, 0.0, nullptr, SDL_FLIP_VERTICAL);

            SDL_Rect dstBR{ rect.x + rect.w - os, rect.y + rect.h - os, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstBR, 0.0, nullptr,
                static_cast<SDL_RendererFlip>(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

            SDL_SetTextureAlphaMod(orn, oldA);
        }
    }
}

void Renderer::render(const Game& game) {
    if (!initialized) return;

    const uint32_t ticks = SDL_GetTicks();
    const int frame = static_cast<int>((ticks / 220u) % FRAMES);
    lastFrame = frame;

    // Background clear
    SDL_SetRenderDrawColor(renderer, 8, 8, 12, 255);
    SDL_RenderClear(renderer);

    const Dungeon& d = game.dungeon();

    auto lightMod = [&](int x, int y) -> uint8_t {
        if (!game.darknessActive()) return 255;
        const uint8_t L = game.tileLightLevel(x, y); // 0..255
        constexpr int kMin = 40;
        int mod = kMin + (static_cast<int>(L) * (255 - kMin)) / 255;
        if (mod < kMin) mod = kMin;
        if (mod > 255) mod = 255;
        return static_cast<uint8_t>(mod);
    };


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

            SDL_Texture* tex = tileTexture(t.type, x, y, game.depth(), frame);
            if (!tex) continue;

            if (t.visible) {
                const uint8_t m = lightMod(x, y);
                SDL_SetTextureColorMod(tex, m, m, m);
                SDL_SetTextureAlphaMod(tex, 255);
            } else {
                if (game.darknessActive()) SDL_SetTextureColorMod(tex, 30, 30, 30);
                else SDL_SetTextureColorMod(tex, 80, 80, 80);
                SDL_SetTextureAlphaMod(tex, 255);
            }

            SDL_RenderCopy(renderer, tex, nullptr, &dst);

            SDL_SetTextureColorMod(tex, 255, 255, 255);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }


    // Auto-move path overlay
    if (game.isAutoActive()) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        if (game.isAutoExploring()) {
            SDL_SetRenderDrawColor(renderer, 80, 220, 140, 90);
        } else {
            SDL_SetRenderDrawColor(renderer, 80, 170, 255, 90);
        }

        for (const Vec2i& p : game.autoPath()) {
            if (!d.inBounds(p.x, p.y)) continue;
            const Tile& t = d.at(p.x, p.y);
            if (!t.explored) continue;

            SDL_Rect r{ p.x * tile + tile / 3, p.y * tile + tile / 3, tile / 3, tile / 3 };
            SDL_RenderFillRect(renderer, &r);
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Draw items (visible only)
    for (const auto& gi : game.groundItems()) {
        if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
        const Tile& t = d.at(gi.pos.x, gi.pos.y);
        if (!t.visible) continue;

        SDL_Texture* tex = itemTexture(gi.item, frame);
        if (!tex) continue;

        SDL_Rect dst{ gi.pos.x * tile, gi.pos.y * tile, tile, tile };
        const uint8_t m = lightMod(gi.pos.x, gi.pos.y);
        SDL_SetTextureColorMod(tex, m, m, m);
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
        SDL_SetTextureColorMod(tex, 255, 255, 255);
    }



    // Draw confusion gas (visible tiles only). This is a persistent, tile-based field
    // spawned by Confusion Gas traps.
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;

                const uint8_t g = game.confusionGasAt(x, y);
                if (g == 0u) continue;

                const uint8_t m = lightMod(x, y);
                int a = 18 + static_cast<int>(g) * 12;
                a = (a * static_cast<int>(m)) / 255;
                a = std::max(12, std::min(190, a));

                // Slight frame shimmer so the cloud feels alive.
                a = std::max(12, std::min(190, a + (static_cast<int>((frame + x * 3 + y * 7) % 9) - 4)));

                SDL_SetRenderDrawColor(renderer, 190, 90, 255, static_cast<uint8_t>(a));
                SDL_Rect r{ x * tile, y * tile, tile, tile };
                SDL_RenderFillRect(renderer, &r);
            }
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Draw discovered traps (shown on explored tiles; bright when visible, dim when remembered)
    for (const auto& tr : game.traps()) {
        if (!tr.discovered) continue;
        if (!d.inBounds(tr.pos.x, tr.pos.y)) continue;
        const Tile& t = d.at(tr.pos.x, tr.pos.y);
        if (!t.explored) continue;

        uint8_t r = 220, g = 80, b = 80;
        switch (tr.kind) {
            case TrapKind::Spike:     r = 220; g = 80;  b = 80;  break;
            case TrapKind::PoisonDart:r = 80;  g = 220; b = 80;  break;
            case TrapKind::Teleport: r = 170; g = 110; b = 230; break;
            case TrapKind::Alarm:    r = 220; g = 220; b = 80;  break;
            case TrapKind::Web:       r = 140; g = 180; b = 255; break;
            case TrapKind::ConfusionGas: r = 200; g = 120; b = 255; break;
        }

        const uint8_t a = t.visible ? 220 : 120;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, r, g, b, a);

        const int x0 = tr.pos.x * tile;
        const int y0 = tr.pos.y * tile;
        SDL_RenderDrawLine(renderer, x0 + 4, y0 + 4, x0 + tile - 5, y0 + tile - 5);
        SDL_RenderDrawLine(renderer, x0 + tile - 5, y0 + 4, x0 + 4, y0 + tile - 5);
        SDL_RenderDrawPoint(renderer, x0 + tile / 2, y0 + tile / 2);
    }

    // Draw entities (only if their tile is visible; player always visible)
    for (const auto& e : game.entities()) {
        if (!d.inBounds(e.pos.x, e.pos.y)) continue;

        bool show = (e.id == game.playerId()) || d.at(e.pos.x, e.pos.y).visible;
        if (!show) continue;

        SDL_Texture* tex = entityTexture(e, (frame + e.id) % FRAMES);
        if (!tex) continue;

        SDL_Rect dst{ e.pos.x * tile, e.pos.y * tile, tile, tile };
        const uint8_t m = lightMod(e.pos.x, e.pos.y);
        SDL_SetTextureColorMod(tex, m, m, m);
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
        SDL_SetTextureColorMod(tex, 255, 255, 255);

        // Small HP pip for monsters
        if (e.id != game.playerId() && e.hp > 0) {
            SDL_Rect bar{ dst.x + 2, dst.y + 2, std::max(1, (tile - 4) * e.hp / std::max(1, e.hpMax)), 4 };
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 200, 40, 40, 160);
            SDL_RenderFillRect(renderer, &bar);
        }
    }

    // FX projectiles
    for (const auto& fx : game.fxProjectiles()) {
        if (fx.path.empty()) continue;
        size_t idx = std::min(fx.pathIndex, fx.path.size() - 1);
        Vec2i p = fx.path[idx];
        if (!d.inBounds(p.x, p.y)) continue;
        const Tile& t = d.at(p.x, p.y);
        if (!t.explored) continue;

        SDL_Texture* tex = projectileTexture(fx.kind, frame);
        if (!tex) continue;
        SDL_Rect dst{ p.x * tile, p.y * tile, tile, tile };
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    // FX explosions (visual-only flashes; gameplay already applied)
    if (!game.fxExplosions().empty()) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        for (const auto& ex : game.fxExplosions()) {
            if (ex.delay > 0.0f) continue;
            if (ex.tiles.empty()) continue;

            const float dur = std::max(0.001f, ex.duration);
            const float t01 = std::min(1.0f, std::max(0.0f, ex.timer / dur));
            // Fade out over time.
            const uint8_t a = static_cast<uint8_t>(std::max(0.0f, 210.0f * (1.0f - t01)));
            if (a == 0) continue;

            // Warm fireball-like tint.
            SDL_SetRenderDrawColor(renderer, 255, 150, 70, a);

            for (const Vec2i& p : ex.tiles) {
                if (!d.inBounds(p.x, p.y)) continue;
                const Tile& t = d.at(p.x, p.y);
                if (!t.explored) continue;
                SDL_Rect r{ p.x * tile + 2, p.y * tile + 2, tile - 4, tile - 4 };
                SDL_RenderFillRect(renderer, &r);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Overlays
    if (game.isLooking()) {
        drawLookOverlay(game);
    }

    if (game.isTargeting()) {
        drawTargetingOverlay(game);
    }

    // HUD (messages, stats)
    drawHud(game);

    // Level-up talent allocation overlay (forced while points are pending)
    if (game.isLevelUpOpen()) {
        drawLevelUpOverlay(game);
    }

    if (game.isMinimapOpen()) {
        drawMinimapOverlay(game);
    }

    if (game.isStatsOpen()) {
        drawStatsOverlay(game);
    }

    if (game.isInventoryOpen()) {
        drawInventoryOverlay(game);
    }

    if (game.isOptionsOpen()) {
        drawOptionsOverlay(game);
    }

    if (game.isHelpOpen()) {
        drawHelpOverlay(game);
    }

    if (game.isCommandOpen()) {
        drawCommandOverlay(game);
    }

    SDL_RenderPresent(renderer);
}


std::string Renderer::saveScreenshotBMP(const std::string& directory, const std::string& prefix) const {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!directory.empty()) {
        fs::create_directories(fs::path(directory), ec);
    }

    // Timestamp for filename.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif

    std::ostringstream name;
    name << prefix << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".bmp";

    fs::path outPath;
    if (directory.empty()) {
        outPath = fs::path(name.str());
    } else {
        outPath = fs::path(directory) / name.str();
    }

    // Read back the current backbuffer.
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0) {
        w = winW;
        h = winH;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return {};

    if (SDL_RenderReadPixels(renderer, nullptr, surface->format->format, surface->pixels, surface->pitch) != 0) {
        SDL_FreeSurface(surface);
        return {};
    }

    if (SDL_SaveBMP(surface, outPath.string().c_str()) != 0) {
        SDL_FreeSurface(surface);
        return {};
    }

    SDL_FreeSurface(surface);
    return outPath.string();
}

void Renderer::drawHud(const Game& game) {
    // HUD background
    SDL_Rect hudRect = {0, winH - hudH, winW, hudH};
    drawPanel(game, hudRect, 220, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};
    const Color red{255,80,80,255};
    const Color green{120,255,120,255};

    // Top row: Title and basic stats
    {
        const std::string hudTitle = std::string("PROCROGUE++ V") + PROCROGUE_VERSION;
        drawText5x7(renderer, 8, winH - hudH + 8, 2, white, hudTitle);
    }

    const Entity& p = game.player();
    std::stringstream ss;
    ss << "HP: " << p.hp << "/" << p.hpMax;
    ss << " | LV: " << game.playerCharLevel();
    ss << " | XP: " << game.playerXp() << "/" << game.playerXpToNext();
    ss << " | GOLD: " << game.goldCount();
    const int debtAll = game.shopDebtTotal();
    if (debtAll > 0) {
        const int debtThis = game.shopDebtThisDepth();
        ss << " | DEBT: " << debtAll;
        if (debtThis > 0 && debtThis != debtAll) {
            ss << " (THIS: " << debtThis << ")";
        }
    }
    ss << " | KEYS: " << game.keyCount() << " | PICKS: " << game.lockpickCount();

    const int arrows = ammoCount(game.inventory(), AmmoKind::Arrow);
    const int rocks  = ammoCount(game.inventory(), AmmoKind::Rock);
    if (arrows > 0) ss << " | ARROWS: " << arrows;
    if (rocks > 0)  ss << " | ROCKS: " << rocks;
    ss << " | DEPTH: " << game.depth() << "/" << game.dungeonMaxDepth();
    ss << " | DEEPEST: " << game.maxDepthReached();
    ss << " | TURNS: " << game.turns();
    ss << " | KILLS: " << game.kills();

    // Status effects
    auto addStatus = [&](const char* label, int turns) {
        if (turns <= 0) return;
        if (game.showEffectTimers()) {
            ss << " | " << label << "(" << turns << ")";
        } else {
            ss << " | " << label;
        }
    };

    addStatus("POISON", p.effects.poisonTurns);
    addStatus("WEB", p.effects.webTurns);
    addStatus("REGEN", p.effects.regenTurns);
    addStatus("SHIELD", p.effects.shieldTurns);
    addStatus("HASTE", p.effects.hasteTurns);
    addStatus("VISION", p.effects.visionTurns);
    addStatus("INVIS", p.effects.invisTurns);
    {
        const std::string ht = game.hungerTag();
        if (!ht.empty()) ss << " | " << ht;
    }
    {
        if (game.encumbranceEnabled()) {
            ss << " | WT: " << game.inventoryWeight() << "/" << game.carryCapacity();
            const std::string bt = game.burdenTag();
            if (!bt.empty()) ss << " | " << bt;
        }
    }
    {
        const std::string st = game.sneakTag();
        if (!st.empty()) ss << " | " << st;
    }
    if (game.autosaveEveryTurns() > 0) {
        ss << " | AS: " << game.autosaveEveryTurns();
    }
    drawText5x7(renderer, 8, winH - hudH + 24, 2, white, ss.str());

    // Controls (3 compact lines)
    const int controlY1 = winH - 48;
    const int controlY2 = winH - 32;
    const int controlY3 = winH - 16;

    drawText5x7(renderer, 8, controlY1, 2, gray,
        "MOVE: WASD/ARROWS/NUMPAD | SPACE/. WAIT | R REST | N SNEAK | < > STAIRS");
    drawText5x7(renderer, 8, controlY2, 2, gray,
        "F FIRE | G PICKUP | I INV | O EXPLORE | P AUTOPICKUP | C SEARCH (TRAPS/SECRETS)");
    drawText5x7(renderer, 8, controlY3, 2, gray,
        "F2 OPT | # CMD | M MAP | SHIFT+TAB STATS | F5 SAVE | F9 LOAD | PGUP/PGDN LOG | ? HELP");

    // Message log
    const auto& msgs = game.messages();
    const int lineH = 16;

    // Leave room for 3 control lines.
    const int maxLines = (hudH - 44 - 52) / lineH;
    int start = std::max(0, (int)msgs.size() - maxLines - game.messageScroll());
    int end = std::min((int)msgs.size(), start + maxLines);

    int y = winH - hudH + 44;
    for (int i = start; i < end; ++i) {
        const auto& msg = msgs[i];
        Color c = white;
        switch (msg.kind) {
            case MessageKind::Info: c = white; break;
            case MessageKind::Combat: c = red; break;
            case MessageKind::Loot: c = yellow; break;
            case MessageKind::Warning: c = yellow; break;
            case MessageKind::Success: c = green; break;
            case MessageKind::System: c = gray; break;
        }
        std::string line = msg.text;
        if (msg.repeat > 1) {
            line += " (x" + std::to_string(msg.repeat) + ")";
        }
        drawText5x7(renderer, 8, y, 2, c, line);
        y += lineH;
    }

    // End-game banner
    if (game.isGameOver()) {
        drawText5x7(renderer, winW/2 - 80, winH - hudH + 70, 3, red, "GAME OVER");
    } else if (game.isGameWon()) {
        drawText5x7(renderer, winW/2 - 90, winH - hudH + 70, 3, green, "YOU ESCAPED!");
    }
}


void Renderer::drawInventoryOverlay(const Game& game) {
    const int panelW = winW - 40;
    const int panelH = winH - 40;
    SDL_Rect bg{ 20, 20, panelW, panelH };

    drawPanel(game, bg, 210, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};
    const Color cyan{140,220,255,255};

    const int scale = 2;
    const int pad = 16;

    int x = bg.x + pad;
    int y = bg.y + pad;

    drawText5x7(renderer, x, y, scale, yellow, "INVENTORY");
    drawText5x7(renderer, x + 160, y, scale, gray, "(ENTER: use/equip, D: drop, ESC: close)");
    if (game.encumbranceEnabled()) {
        std::stringstream ws;
        ws << "WT: " << game.inventoryWeight() << "/" << game.carryCapacity();
        const std::string bt = game.burdenTag();
        if (!bt.empty()) ws << " (" << bt << ")";
        drawText5x7(renderer, x, y + 14, scale, gray, ws.str());
        y += 44;
    } else {
        y += 28;
    }

    const auto& inv = game.inventory();
    const int sel = game.inventorySelection();

    // Layout: list (left) + preview/info (right)
    const int colGap = 18;
    const int listW = (bg.w * 58) / 100;
    SDL_Rect listRect{ x, y, listW, bg.y + bg.h - pad - y };
    SDL_Rect infoRect{ x + listW + colGap, y, bg.x + bg.w - pad - (x + listW + colGap), listRect.h };

    // List scroll
    const int lineH = 18;
    const int maxLines = std::max(1, listRect.h / lineH);
    int start = 0;
    if (!inv.empty()) {
        start = std::clamp(sel - maxLines / 2, 0, std::max(0, (int)inv.size() - maxLines));
    }
    const int end = std::min((int)inv.size(), start + maxLines);

    // Selection background
    if (!inv.empty() && sel >= start && sel < end) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_Rect hi{ listRect.x - 6, listRect.y + (sel - start) * lineH - 2, listRect.w + 12, lineH };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 20);
        SDL_RenderFillRect(renderer, &hi);
    }

    // Helpers
    auto fitToChars = [](const std::string& s, int maxChars) -> std::string {
        if ((int)s.size() <= maxChars) return s;
        if (maxChars <= 1) return s.substr(0, 1);
        return s.substr(0, static_cast<size_t>(std::max(0, maxChars - 3))) + "...";
    };

    auto itemEffectDesc = [&](const Item& it, bool identified) -> std::string {
        const ItemDef& def = itemDef(it.kind);
		if (!identified && isIdentifiableKind(it.kind)) return "EFFECT: UNKNOWN";
        switch (it.kind) {
			case ItemKind::PotionHealing:
				return "EFFECT: HEAL +" + std::to_string(std::max(0, def.healAmount)) + " HP";
            case ItemKind::PotionAntidote: return "EFFECT: CURE POISON";
            case ItemKind::PotionStrength: return "EFFECT: +ATK";
            case ItemKind::PotionRegeneration: return "EFFECT: REGEN";
			case ItemKind::PotionShielding: return "EFFECT: STONESKIN";
            case ItemKind::PotionHaste: return "EFFECT: HASTE";
            case ItemKind::PotionVision: return "EFFECT: VISION";
            case ItemKind::ScrollTeleport: return "EFFECT: TELEPORT";
            case ItemKind::ScrollMapping: return "EFFECT: MAPPING";
            case ItemKind::ScrollDetectTraps: return "EFFECT: DETECT TRAPS";
			case ItemKind::ScrollDetectSecrets: return "EFFECT: DETECT SECRETS";
            case ItemKind::ScrollKnock: return "EFFECT: KNOCK";
            case ItemKind::ScrollEnchantWeapon: return "EFFECT: ENCHANT WEAPON";
            case ItemKind::ScrollEnchantArmor: return "EFFECT: ENCHANT ARMOR";
            case ItemKind::ScrollIdentify: return "EFFECT: IDENTIFY";
			case ItemKind::FoodRation:
				return def.hungerRestore > 0
					? ("EFFECT: RESTORE HUNGER +" + std::to_string(def.hungerRestore))
					: "EFFECT: FOOD";
            default: break;
        }
        return "EFFECT: —";
    };

    // Draw list
    int yy = listRect.y;
    const int maxChars = std::max(10, (listRect.w - 24) / (scale * 6));
    for (int i = start; i < end; ++i) {
        const Item& it = inv[static_cast<size_t>(i)];
        const std::string tag = game.equippedTag(it.id); // "" or "M"/"R"/"A"/...

        std::stringstream row;
        row << (i == sel ? "> " : "  ");
        if (!tag.empty()) row << "[" << tag << "] ";
        row << game.displayItemName(it);

        const Color c = (i == sel) ? white : gray;
        drawText5x7(renderer, listRect.x, yy, scale, c, fitToChars(row.str(), maxChars));
        yy += lineH;
    }

    if (inv.empty()) {
        drawText5x7(renderer, listRect.x, listRect.y, scale, gray, "(EMPTY)");
    } else if (sel >= 0 && sel < (int)inv.size()) {
        // Draw preview / info panel
        const Item& it = inv[static_cast<size_t>(sel)];
        const ItemDef& def = itemDef(it.kind);

        bool identified = (game.displayItemNameSingle(it.kind) == itemDisplayNameSingle(it.kind));

        int ix = infoRect.x;
        int iy = infoRect.y;

        // Name (top)
        drawText5x7(renderer, ix, iy, scale, cyan, fitToChars(game.displayItemName(it), 30));
        iy += 22;

        // Sprite preview
        const int icon = std::min(96, infoRect.w);
        SDL_Rect sprDst{ ix, iy, icon, icon };
        SDL_Texture* tex = itemTexture(it, lastFrame);
        if (tex) {
            SDL_RenderCopy(renderer, tex, nullptr, &sprDst);
        }
        iy += icon + 10;

        // Stats lines
        auto statLine = [&](const std::string& s, const Color& c) {
            drawText5x7(renderer, ix, iy, scale, c, fitToChars(s, 32));
            iy += 18;
        };

		// Type / stats
		auto ammoLabel = [](AmmoKind a) -> const char* {
			switch (a) {
				case AmmoKind::Arrow: return "ARROWS";
				case AmmoKind::Rock:  return "ROCKS";
				default: return "NONE";
			}
		};

		auto statCompare = [&](const char* label, int cur, int after) {
			const int delta = after - cur;
			std::ostringstream ss;
			ss << label << ": " << cur << " -> " << after;
			if (delta > 0) ss << " (+" << delta << ")";
			else if (delta < 0) ss << " (" << delta << ")";
			statLine(ss.str(), gray);
		};

		// Find currently equipped gear by tag (renderer can't see equip IDs directly).
		auto findEquippedBy = [&](char ch) -> const Item* {
			for (const Item& v : inv) {
				const std::string t = game.equippedTag(v.id);
				if (t.find(ch) != std::string::npos) return &v;
			}
			return nullptr;
		};

		const Entity& p = game.player();
		const int baseAtk = p.baseAtk;
		const int baseDef = p.baseDef;
		const int shieldBonus = (p.effects.shieldTurns > 0) ? 2 : 0;
		const int curAtk = game.playerAttack();
		const int curDef = game.playerDefense();

		const Item* eqM = findEquippedBy('M');
		const Item* eqR = findEquippedBy('R');
		const Item* eqA = findEquippedBy('A');
		(void)eqM;
		(void)eqA;

		const bool identifiable = isIdentifiableKind(it.kind);
		const bool isWand = isRangedWeapon(it.kind) && def.maxCharges > 0 && def.ammo == AmmoKind::None;
		const bool isFood = (def.hungerRestore > 0) || (it.kind == ItemKind::FoodRation);

		if (isGold(it.kind)) {
			statLine("TYPE: GOLD", white);
			statLine("VALUE: " + std::to_string(it.count), gray);
		} else if (it.kind == ItemKind::Key) {
			statLine("TYPE: KEY", white);
			statLine("USED FOR: LOCKED DOORS / CHESTS", gray);
		} else if (it.kind == ItemKind::Lockpick) {
			statLine("TYPE: LOCKPICK", white);
			statLine("USED FOR: PICK LOCKS (CHANCE)", gray);
		} else if (it.kind == ItemKind::Torch || it.kind == ItemKind::TorchLit) {
			statLine("TYPE: LIGHT SOURCE", white);
			if (it.kind == ItemKind::TorchLit) {
				statLine("STATUS: LIT", gray);
				statLine("FUEL: " + std::to_string(it.charges) + " TURNS", gray);
				statLine("RADIUS: 8", gray);
			} else {
				statLine("STATUS: UNLIT", gray);
				statLine("USE: LIGHT A TORCH", gray);
			}
		} else if (isFood) {
			statLine("TYPE: FOOD", white);
			if (game.hungerEnabled() && def.hungerRestore > 0) {
				statLine("RESTORE: +" + std::to_string(def.hungerRestore) + " HUNGER", gray);
			} else {
				statLine("HUNGER SYSTEM: DISABLED", gray);
			}
		} else if (isMeleeWeapon(it.kind)) {
			statLine("TYPE: MELEE WEAPON", white);
			const int newAtk = baseAtk + def.meleeAtk + it.enchant;
			statCompare("ATK", curAtk, newAtk);
		} else if (isArmor(it.kind)) {
			statLine("TYPE: ARMOR", white);
			const int newDef = baseDef + shieldBonus + def.defense + it.enchant;
			statCompare("DEF", curDef, newDef);
			if (shieldBonus > 0) {
				statLine("(INCLUDES SHIELD +2)", gray);
			}
		} else if (isWand) {
			statLine("TYPE: WAND", white);

			auto wandEffect = [&]() -> std::string {
				if (it.kind == ItemKind::WandDigging) return "DIGGING";
				switch (def.projectile) {
					case ProjectileKind::Spark: return "SPARKS";
					case ProjectileKind::Fireball: return "FIREBALL";
					default: return "MAGIC";
				}
			};

			statLine("EFFECT: " + wandEffect(), gray);
			statLine("RANGE: " + std::to_string(def.range), gray);
			statLine("CHARGES: " + std::to_string(it.charges) + "/" + std::to_string(def.maxCharges), gray);
			const int baseRAtk = std::max(1, baseAtk + def.rangedAtk + it.enchant + 2);
			statLine("RATK (BASE): " + std::to_string(baseRAtk) + "+", gray);
			statLine(std::string("READY: ") + (it.charges > 0 ? "YES" : "NO"), gray);
			if (def.projectile == ProjectileKind::Fireball) {
				statLine("AOE: RADIUS 1 (3x3)", gray);
			}
		} else if (isRangedWeapon(it.kind)) {
			statLine("TYPE: RANGED WEAPON", white);
			const int thisRAtk = std::max(1, baseAtk + def.rangedAtk + it.enchant);
			if (eqR) {
				const ItemDef& curD = itemDef(eqR->kind);
				const int curRAtk = std::max(1, baseAtk + curD.rangedAtk + eqR->enchant);
				statCompare("RATK", curRAtk, thisRAtk);
			} else {
				statLine("RATK (BASE): " + std::to_string(thisRAtk), gray);
			}
			statLine("RANGE: " + std::to_string(def.range), gray);
			if (def.ammo != AmmoKind::None) {
				const int have = ammoCount(inv, def.ammo);
				statLine(std::string("AMMO: ") + ammoLabel(def.ammo) + " (" + std::to_string(have) + ")", gray);
			}
			const bool chargesOk = (def.maxCharges <= 0) || (it.charges > 0);
			const bool ammoOk = (def.ammo == AmmoKind::None) || (ammoCount(inv, def.ammo) > 0);
			const bool ready = (def.range > 0) && chargesOk && ammoOk;
			statLine(std::string("READY: ") + (ready ? "YES" : "NO"), gray);
		} else if (def.consumable) {
			statLine(identifiable ? "TYPE: CONSUMABLE (IDENTIFIABLE)" : "TYPE: CONSUMABLE", white);
			statLine(itemEffectDesc(it, identified), gray);
			if (identifiable) {
				statLine(std::string("IDENTIFIED: ") + (identified ? "YES" : "NO"), gray);
			}
		} else {
			statLine("TYPE: MISC", white);
		}

        if (it.count > 1) {
            statLine("COUNT: " + std::to_string(it.count), gray);
        }

		// Quick equipment summary (useful when comparing gear).
		iy += 6;
		statLine("EQUIPPED", yellow);
		statLine("M: " + game.equippedMeleeName(), gray);
		statLine("R: " + game.equippedRangedName(), gray);
		statLine("A: " + game.equippedArmorName(), gray);
    }
}

void Renderer::drawOptionsOverlay(const Game& game) {
    const int panelW = std::min(winW - 80, 820);
    const int panelH = 440;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - panelH) / 2;

    SDL_Rect bg{x0, y0, panelW, panelH};
    drawPanel(game, bg, 210, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    const int scale = 2;
    int y = y0 + 16;

    drawText5x7(renderer, x0 + 16, y, scale, yellow, "OPTIONS");
    y += 26;

    auto yesNo = [](bool b) { return b ? "ON" : "OFF"; };

    auto autoPickupLabel = [](AutoPickupMode m) -> const char* {
        switch (m) {
            case AutoPickupMode::Off:   return "OFF";
            case AutoPickupMode::Gold:  return "GOLD";
            case AutoPickupMode::Smart: return "SMART";
            case AutoPickupMode::All:   return "ALL";
        }
        return "?";
    };

    auto uiThemeLabel = [](UITheme t) -> const char* {
        switch (t) {
            case UITheme::DarkStone: return "DARKSTONE";
            case UITheme::Parchment: return "PARCHMENT";
            case UITheme::Arcane:    return "ARCANE";
        }
        return "UNKNOWN";
    };

    const int sel = game.optionsSelection();

    auto drawOpt = [&](int idx, const std::string& label, const std::string& value) {
        const Color c = (idx == sel) ? white : gray;
        std::stringstream ss;
        ss << (idx == sel ? "> " : "  ");
        ss << label;
        if (!value.empty()) ss << ": " << value;
        drawText5x7(renderer, x0 + 16, y, scale, c, ss.str());
        y += 18;
    };

    drawOpt(0, "AUTO-PICKUP", autoPickupLabel(game.autoPickupMode()));
    drawOpt(1, "AUTO-STEP DELAY", std::to_string(game.autoStepDelayMs()) + "ms");
    drawOpt(2, "AUTOSAVE", (game.autosaveEveryTurns() > 0 ? ("EVERY " + std::to_string(game.autosaveEveryTurns()) + " TURNS") : "OFF"));
    drawOpt(3, "IDENTIFY ITEMS", yesNo(game.identificationEnabled()));
    drawOpt(4, "HUNGER SYSTEM", yesNo(game.hungerEnabled()));
    drawOpt(5, "ENCUMBRANCE", yesNo(game.encumbranceEnabled()));
    drawOpt(6, "EFFECT TIMERS", yesNo(game.showEffectTimers()));
    drawOpt(7, "CONFIRM QUIT", yesNo(game.confirmQuitEnabled()));
    drawOpt(8, "AUTO MORTEM", yesNo(game.autoMortemEnabled()));
    drawOpt(9, "SAVE BACKUPS", (game.saveBackups() > 0 ? std::to_string(game.saveBackups()) : "OFF"));
    drawOpt(10, "UI THEME", uiThemeLabel(game.uiTheme()));
    drawOpt(11, "UI PANELS", (game.uiPanelsTextured() ? "TEXTURED" : "SOLID"));
    drawOpt(12, "CLOSE", "");

    y += 14;
    drawText5x7(renderer, x0 + 16, y, scale, gray,
        "LEFT/RIGHT: change | ENTER: toggle/next | ESC: close");
}

void Renderer::drawCommandOverlay(const Game& game) {
    const int barH = 52;
    int y0 = winH - hudH - barH - 10;
    if (y0 < 10) y0 = 10;

    SDL_Rect bg{10, y0, winW - 20, barH};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &bg);

    const int pad = 10;
    const int x = bg.x + pad;
    int y = bg.y + 8;

    // Local UI palette.
    const Color white{255, 255, 255, 255};
    const Color gray{180, 180, 180, 255};

    // Fit the command string to the bar width.
    const int maxChars = std::max(0, (bg.w - 2 * pad) / (6 * 2)); // 5x7 font: ~6px per char at scale1
    auto fitTail = [&](const std::string& s) -> std::string {
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(s.size() - maxChars);
        return "..." + s.substr(s.size() - (maxChars - 3));
    };

    const std::string prompt = "EXT CMD: " + fitTail(game.commandBuffer());
    drawText5x7(renderer, x, y, 2, white, prompt);

    y += 24;
    drawText5x7(renderer, x, y, 1, gray, "ENTER RUN  ESC CANCEL  UP/DOWN HISTORY  TAB COMPLETE");
}

void Renderer::drawHelpOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    Color white{255, 255, 255, 255};
    Color gray{180, 180, 180, 255};

    const int panelW = std::min(winW - 80, 820);
    const int panelH = std::min(520, winH - 40);
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - panelH) / 2;
    const int pad = 14;

    SDL_Rect bg{x0, y0, panelW, panelH};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
    SDL_RenderDrawRect(renderer, &bg);

    int y = y0 + pad;
    drawText5x7(renderer, x0 + pad, y, 2, white, "HELP");
    y += 22;

    auto lineWhite = [&](const std::string& s) {
        drawText5x7(renderer, x0 + pad, y, 2, white, s);
        y += 18;
    };
    auto lineGray = [&](const std::string& s) {
        drawText5x7(renderer, x0 + pad, y, 2, gray, s);
        y += 18;
    };

    lineWhite("CONTROLS:");
    lineGray("MOVE: WASD / ARROWS / NUMPAD (DIAGONALS OK)");
    lineGray("SPACE/. WAIT  R REST  < > STAIRS");
    lineGray("F FIRE  G PICKUP  I/TAB INVENTORY");
    lineGray("L/V LOOK  C SEARCH  T DISARM  K CLOSE DOOR  SHIFT+K LOCK DOOR");
    lineGray("O EXPLORE  P AUTOPICKUP  M MINIMAP  SHIFT+TAB STATS");
    lineGray("F2 OPTIONS  # EXTENDED COMMANDS  (TYPE + ENTER)");
    lineGray("F5 SAVE  F9 LOAD  F10 LOAD AUTO  F6 RESTART");
    lineGray("F11 FULLSCREEN  F12 SCREENSHOT (BINDABLE)");
    lineGray("PGUP/PGDN LOG  ESC CANCEL/QUIT");

    y += 6;
    lineWhite("EXTENDED COMMAND EXAMPLES:");
    lineGray("save | load | loadauto | quit | version | seed | name | scores");
    lineGray("autopickup off/gold/all");
    lineGray("name <text>  scores [N]");
    lineGray("autosave <turns>  stepdelay <ms>  identify on/off  timers on/off");
    lineGray("pray [heal|cure|identify|bless|uncurse]");

    y += 6;
    lineWhite("KEYBINDINGS:");
    auto baseName = [](const std::string& p) -> std::string {
        if (p.empty()) return {};
        size_t i = p.find_last_of("/\\");
        if (i == std::string::npos) return p;
        return p.substr(i + 1);
    };
    const std::string settingsFile = baseName(game.settingsPath());
    if (!settingsFile.empty()) lineGray("EDIT " + settingsFile + " (bind_*)");
    else lineGray("EDIT procrogue_settings.ini (bind_*)");

    y += 6;
    lineWhite("TIPS:");
    lineGray("SEARCH CAN REVEAL TRAPS AND SECRET DOORS. EXT: #SEARCH N [ALL]");
    lineGray("LOCKED DOORS: USE KEYS, LOCKPICKS, OR A SCROLL OF KNOCK.");
    lineGray("SOME VAULT DOORS MAY BE TRAPPED.");
    lineGray("AUTO-EXPLORE STOPS IF YOU SEE AN ENEMY OR GET HURT/DEBUFFED.");
    lineGray("INVENTORY: E EQUIP  U USE  X DROP  SHIFT+X DROP ALL");
    lineGray("SCROLL THE MESSAGE LOG WITH PGUP/PGDN.");
}




void Renderer::drawMinimapOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Color white{ 240, 240, 240, 255 };
    const Dungeon& d = game.dungeon();
    const int W = d.width;
    const int H = d.height;

    // Choose a small per-tile pixel size that fits comfortably on screen.
    int px = 4;
    const int pad = 10;
    const int margin = 10;
    // Don't let the minimap eat the whole window.
    const int maxW = winW / 2;
    const int maxH = (winH - hudH) / 2;
    while (px > 2 && (W * px + pad * 2) > maxW) px--;
    while (px > 2 && (H * px + pad * 2) > maxH) px--;

    const int titleH = 16;
    const int panelW = W * px + pad * 2;
    const int panelH = H * px + pad * 2 + titleH;

    const int x0 = winW - panelW - margin;
    const int y0 = margin;

    SDL_Rect panel { x0, y0, panelW, panelH };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
    SDL_RenderDrawRect(renderer, &panel);

    // Title
    drawText5x7(renderer, x0 + pad, y0 + 4, 2, white, "MINIMAP (M)");

    const int mapX = x0 + pad;
    const int mapY = y0 + pad + titleH;

    auto drawCell = [&](int tx, int ty, uint8_t r, uint8_t g, uint8_t b, uint8_t a=255) {
        SDL_Rect rc { mapX + tx * px, mapY + ty * px, px, px };
        SDL_SetRenderDrawColor(renderer, r, g, b, a);
        SDL_RenderFillRect(renderer, &rc);
    };

    // Tiles
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const Tile& t = d.at(x, y);
            if (!t.explored) {
                // Unexplored: don't draw (keep the background)
                continue;
            }

            const bool vis = t.visible;

            // Basic palette
            if (t.type == TileType::Wall) {
                if (vis) drawCell(x, y, 110, 110, 110);
                else     drawCell(x, y, 60, 60, 60);
            } else if (t.type == TileType::Pillar) {
                // Pillars are interior "walls"; show them slightly brighter so
                // they read as distinct from border stone.
                if (vis) drawCell(x, y, 130, 130, 130);
                else     drawCell(x, y, 75, 75, 75);
            } else if (t.type == TileType::Chasm) {
                // Chasms are impassable but not opaque.
                if (vis) drawCell(x, y, 20, 30, 55);
                else     drawCell(x, y, 12, 18, 32);
            } else if (t.type == TileType::DoorClosed) {
                if (vis) drawCell(x, y, 160, 110, 60);
                else     drawCell(x, y, 90, 70, 40);
            } else if (t.type == TileType::DoorLocked) {
                // Slightly more "warning" tint than a normal closed door.
                if (vis) drawCell(x, y, 180, 90, 70);
                else     drawCell(x, y, 100, 60, 50);
            } else if (t.type == TileType::DoorOpen) {
                if (vis) drawCell(x, y, 140, 120, 90);
                else     drawCell(x, y, 80, 70, 55);
            } else if (t.type == TileType::StairsDown || t.type == TileType::StairsUp) {
                if (vis) drawCell(x, y, 220, 220, 120);
                else     drawCell(x, y, 120, 120, 80);
            } else {
                // Floor/other passable
                if (vis) drawCell(x, y, 30, 30, 30);
                else     drawCell(x, y, 18, 18, 18);
            }
        }
    }

    // Entities (only show visible monsters; always show player)
    const Entity& p = game.player();
    drawCell(p.pos.x, p.pos.y, 60, 180, 255);

    for (const auto& e : game.entities()) {
        if (e.id == p.id) continue;
        if (e.hp <= 0) continue;
        const Tile& t = d.at(e.pos.x, e.pos.y);
        if (!t.visible) continue;
        drawCell(e.pos.x, e.pos.y, 255, 80, 80);
    }
}

void Renderer::drawStatsOverlay(const Game& game) {
    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    // Center panel
    const int panelW = winW * 4 / 5;
    const int panelH = (winH - hudH) * 4 / 5;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - hudH - panelH) / 2;

    SDL_Rect panel { x0, y0, panelW, panelH };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 140);
    SDL_RenderDrawRect(renderer, &panel);

    const int pad = 14;
    int y = y0 + pad;

    drawText5x7(renderer, x0 + pad, y, 2, white, "STATS / RUN HISTORY (TAB)");
    y += 22;

    const Entity& p = game.player();

    // Run summary
    {
        std::stringstream ss;
        ss << (game.isGameWon() ? "RESULT: WIN" : (game.isGameOver() ? "RESULT: DEAD" : "RESULT: IN PROGRESS"));
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "SEED: " << game.seed();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "DEPTH: " << game.depth() << "/" << game.dungeonMaxDepth() << "  (DEEPEST: " << game.maxDepthReached() << ")";
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "TURNS: " << game.turns() << "  KILLS: " << game.kills() << "  GOLD: " << game.goldCount() << "  KEYS: " << game.keyCount() << "  PICKS: " << game.lockpickCount();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "HP: " << p.hp << "/" << p.hpMax << "  LV: " << game.playerCharLevel()
           << "  XP: " << game.playerXp() << "/" << game.playerXpToNext();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "TALENTS: M" << game.playerMight()
           << " A" << game.playerAgility()
           << " V" << game.playerVigor()
           << " F" << game.playerFocus();
        if (game.pendingTalentPoints() > 0) ss << "  (PENDING: " << game.pendingTalentPoints() << ")";
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        if (game.autosaveEveryTurns() > 0) {
            ss << "AUTOSAVE: every " << game.autosaveEveryTurns() << " turns (" << game.defaultAutosavePath() << ")";
        } else {
            ss << "AUTOSAVE: OFF";
        }
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 22;
    }

    drawText5x7(renderer, x0 + pad, y, 2, white, "TOP RUNS");
    y += 18;

    const auto& entries = game.scoreBoard().entries();
    const int maxShown = 10;

    if (entries.empty()) {
        drawText5x7(renderer, x0 + pad, y, 2, white, "(NO RUNS RECORDED YET)");
        y += 18;
    } else {
        for (int i = 0; i < (int)entries.size() && i < maxShown; ++i) {
            const auto& e = entries[i];
            auto trunc = [](const std::string& s, size_t n) {
                if (s.size() <= n) return s;
                if (n <= 1) return s.substr(0, n);
                if (n <= 3) return s.substr(0, n);
                return s.substr(0, n - 3) + "...";
            };

            const std::string who = e.name.empty() ? "PLAYER" : e.name;
            const std::string whoCol = trunc(who, 10);
            const std::string cause = e.cause.empty() ? "" : e.cause;
            const std::string causeCol = trunc(cause, 28);

            std::stringstream ss;
            ss << "#" << (i + 1) << " "
               << whoCol;
            if (whoCol.size() < 10) ss << std::string(10 - whoCol.size(), ' ');

            ss << " "
               << (e.won ? "WIN " : "DEAD")
               << " " << e.score
               << " D" << e.depth
               << " T" << e.turns
               << " K" << e.kills
               << " S" << e.seed;

            if (!causeCol.empty()) ss << " " << causeCol;

            drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
            y += 16;
            if (y > y0 + panelH - 36) break;
        }
    }

    // Footer
    drawText5x7(renderer, x0 + pad, y0 + panelH - 20, 2, white, "ESC to close");
}

void Renderer::drawLevelUpOverlay(const Game& game) {
    // A focused, compact overlay that forces the player to spend talent points.
    const int points = game.pendingTalentPoints();
    if (points <= 0) return;

    const int panelW = std::min(winW - 80, 620);
    const int panelH = 260;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - hudH - panelH) / 2;

    SDL_Rect bg{x0, y0, panelW, panelH};
    drawPanel(game, bg, 220, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    const int scale = 2;
    int y = y0 + 14;

    drawText5x7(renderer, x0 + 16, y, scale, yellow, "LEVEL UP!  CHOOSE A TALENT");
    y += 22;

    {
        std::stringstream ss;
        ss << "TALENT POINTS: " << points
           << "   MIGHT:" << game.playerMight()
           << "  AGI:" << game.playerAgility()
           << "  VIG:" << game.playerVigor()
           << "  FOC:" << game.playerFocus();
        drawText5x7(renderer, x0 + 16, y, scale, white, ss.str());
        y += 20;
    }

    {
        std::stringstream ss;
        ss << "MELEE POWER: " << game.playerMeleePower()
           << "   EVASION: " << game.playerEvasion()
           << "   WAND PWR: " << game.playerWandPower();
        drawText5x7(renderer, x0 + 16, y, scale, gray, ss.str());
        y += 22;
    }

    const int sel = game.levelUpSelection();

    auto drawChoice = [&](int idx, const char* label, const char* desc) {
        const Color c = (idx == sel) ? white : gray;
        std::stringstream ss;
        ss << (idx == sel ? "> " : "  ") << label << ": " << desc;
        drawText5x7(renderer, x0 + 16, y, scale, c, ss.str());
        y += 18;
    };

    drawChoice(0, "MIGHT",   "+1 melee power, +carry, +melee dmg bonus");
    drawChoice(1, "AGILITY", "+1 ranged skill, +evasion, better locks/traps");
    drawChoice(2, "VIGOR",   "+2 max HP now, tougher natural regen");
    drawChoice(3, "FOCUS",   "+1 wand power, better searching");

    y += 14;
    drawText5x7(renderer, x0 + 16, y, scale, gray, "UP/DOWN: select  ENTER: spend  ESC: spend all");
}

void Renderer::drawTargetingOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const auto& linePts = game.targetingLine();
    Vec2i cursor = game.targetingCursor();
    bool ok = game.targetingIsValid();

    // Draw LOS line tiles (excluding player tile)
    SDL_SetRenderDrawColor(renderer, ok ? 0 : 255, ok ? 255 : 0, 0, 80);
    for (size_t i = 1; i < linePts.size(); ++i) {
        Vec2i p = linePts[i];
        SDL_Rect r{ p.x * tile + tile/4, p.y * tile + tile/4, tile/2, tile/2 };
        SDL_RenderFillRect(renderer, &r);
    }

    // Crosshair on cursor
    SDL_Rect c{ cursor.x * tile, cursor.y * tile, tile, tile };
    SDL_SetRenderDrawColor(renderer, ok ? 0 : 255, ok ? 255 : 0, 0, 200);
    SDL_RenderDrawRect(renderer, &c);

    // Small label near bottom HUD
    const int scale = 2;
    const Color yellow{ 255, 230, 120, 255 };
    int hudTop = Game::MAP_H * tile;
    drawText5x7(renderer, 10, hudTop - 18, scale, yellow, ok ? "TARGET: ENTER TO FIRE, ESC TO CANCEL" : "TARGET: OUT OF RANGE/NO LOS");
}

void Renderer::drawLookOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Dungeon& d = game.dungeon();
    Vec2i cursor = game.lookCursor();
    if (!d.inBounds(cursor.x, cursor.y)) return;

    // Cursor box
    SDL_Rect c{ cursor.x * tile, cursor.y * tile, tile, tile };
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
    SDL_RenderDrawRect(renderer, &c);

    // Crosshair lines (subtle)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
    SDL_RenderDrawLine(renderer, c.x, c.y + tile / 2, c.x + tile, c.y + tile / 2);
    SDL_RenderDrawLine(renderer, c.x + tile / 2, c.y, c.x + tile / 2, c.y + tile);

    // Label near bottom of map
    const int scale = 2;
    const Color yellow{ 255, 230, 120, 255 };
    int hudTop = Game::MAP_H * tile;

    std::string s = game.lookInfoText();
    if (s.empty()) s = "LOOK";
    drawText5x7(renderer, 10, hudTop - 18, scale, yellow, s);
}
