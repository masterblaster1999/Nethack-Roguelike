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

    window = SDL_CreateWindow("ProcRogue++",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              winW, winH,
                              SDL_WINDOW_SHOWN);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return false;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window); window = nullptr;
        return false;
    }

    pixfmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!pixfmt) {
        std::cerr << "SDL_AllocFormat failed\n";
        shutdown();
        return false;
    }

    // Pre-generate a few tile variants
    const int tileVars = 10;
    floorVar.reserve(tileVars);
    wallVar.reserve(tileVars);

    for (int i = 0; i < tileVars; ++i) {
        floorVar.push_back(textureFromSprite(generateFloorTile(hashCombine(0xF1000u, static_cast<uint32_t>(i)), 0)));
        wallVar.push_back(textureFromSprite(generateWallTile(hashCombine(0xAA110u, static_cast<uint32_t>(i)), 0)));
    }

    for (int f = 0; f < FRAMES; ++f) {
        stairsUpTex[static_cast<size_t>(f)]   = textureFromSprite(generateStairsTile(0x515A1u, true, f));
        stairsDownTex[static_cast<size_t>(f)] = textureFromSprite(generateStairsTile(0x515A2u, false, f));
        doorClosedTex[static_cast<size_t>(f)] = textureFromSprite(generateDoorTile(0xD00Du, false, f));
        doorOpenTex[static_cast<size_t>(f)]   = textureFromSprite(generateDoorTile(0xD00Du, true, f));
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
    floorVar.clear();
    wallVar.clear();

    for (auto& t : stairsUpTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : stairsDownTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorClosedTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorOpenTex) if (t) SDL_DestroyTexture(t);

    stairsUpTex.fill(nullptr);
    stairsDownTex.fill(nullptr);
    doorClosedTex.fill(nullptr);
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
            return floorVar[idx];
        }
        case TileType::Wall: {
            if (wallVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(wallVar.size()));
            return wallVar[idx];
        }
        case TileType::StairsUp:
            return stairsUpTex[static_cast<size_t>(frame % FRAMES)];
        case TileType::StairsDown:
            return stairsDownTex[static_cast<size_t>(frame % FRAMES)];
        case TileType::DoorClosed:
            return doorClosedTex[static_cast<size_t>(frame % FRAMES)];
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

void Renderer::render(const Game& game) {
    if (!initialized) return;

    const uint32_t ticks = SDL_GetTicks();
    const int frame = static_cast<int>((ticks / 220u) % FRAMES);

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

            SDL_Texture* tex = tileTexture(t.type, x, y, game.level(), frame);
            if (!tex) continue;

            if (t.visible) {
                SDL_SetTextureColorMod(tex, 255, 255, 255);
                SDL_SetTextureAlphaMod(tex, 255);
            } else {
                SDL_SetTextureColorMod(tex, 80, 80, 80);
                SDL_SetTextureAlphaMod(tex, 255);
            }

            SDL_RenderCopy(renderer, tex, nullptr, &dst);

            SDL_SetTextureColorMod(tex, 255, 255, 255);
            SDL_SetTextureAlphaMod(tex, 255);
        }
    }

    // Draw items (visible only)
    for (const auto& gi : game.groundItems()) {
        if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
        const Tile& t = d.at(gi.pos.x, gi.pos.y);
        if (!t.visible) continue;

        SDL_Texture* tex = itemTexture(gi.item, frame);
        if (!tex) continue;

        SDL_Rect dst{ gi.pos.x * tile, gi.pos.y * tile, tile, tile };
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    // Draw entities (only if their tile is visible; player always visible)
    for (const auto& e : game.entities()) {
        if (!d.inBounds(e.pos.x, e.pos.y)) continue;

        bool show = (e.id == game.playerId()) || d.at(e.pos.x, e.pos.y).visible;
        if (!show) continue;

        SDL_Texture* tex = entityTexture(e, (frame + e.id) % FRAMES);
        if (!tex) continue;

        SDL_Rect dst{ e.pos.x * tile, e.pos.y * tile, tile, tile };
        SDL_RenderCopy(renderer, tex, nullptr, &dst);

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

    // Overlays
    if (game.isTargeting()) {
        drawTargetingOverlay(game);
    }

    // HUD (messages, stats)
    drawHud(game);

    if (game.isInventoryOpen()) {
        drawInventoryOverlay(game);
    }

    SDL_RenderPresent(renderer);
}

void Renderer::drawHud(const Game& game) {
    const int hudTop = Game::MAP_H * tile;
    SDL_Rect r{ 0, hudTop, winW, hudH };

    SDL_SetRenderDrawColor(renderer, 15, 15, 20, 255);
    SDL_RenderFillRect(renderer, &r);

    const int scale = 2;
    const Color white{ 240, 240, 240, 255 };
    const Color gray{ 170, 170, 170, 255 };
    const Color red{ 255, 90, 90, 255 };
    const Color green{ 120, 255, 120, 255 };
    const Color yellow{ 255, 230, 120, 255 };

    int x = 10;
    int y = hudTop + 10;

    // Stats line
    const Entity& p = game.player();
    int atk = game.playerAttack();
    int def = game.playerDefense();
    int gold = countGold(game.inventory());

    std::string melee = game.equippedMeleeName();
    std::string ranged = game.equippedRangedName();
    std::string armor = game.equippedArmorName();

    drawText5x7(renderer, x, y, scale, white, "HP " + std::to_string(p.hp) + "/" + std::to_string(p.hpMax) +
        "  ATK " + std::to_string(atk) + "  DEF " + std::to_string(def) +
        "  GOLD " + std::to_string(gold) +
        "  LVL " + std::to_string(game.level()));
    y += 7 * scale + 6;

    drawText5x7(renderer, x, y, scale, gray, "MELEE: " + melee + "   RANGED: " + ranged + "   ARMOR: " + armor);
    y += 7 * scale + 10;

    // Messages
    const auto& msgs = game.messages();
    int lines = std::max(4, (hudH - (y - hudTop) - 22) / (7 * scale + 4));
    int scroll = game.messageScroll();

    int end = static_cast<int>(msgs.size()) - scroll;
    if (end < 0) end = 0;
    int start = std::max(0, end - lines);

    for (int i = start; i < end; ++i) {
        drawText5x7(renderer, x, y, scale, white, toUpper(msgs[static_cast<size_t>(i)]));
        y += 7 * scale + 4;
    }

    // Controls line
    int bottomY = hudTop + hudH - (7 * scale + 10);
    std::string mode = game.isTargeting() ? "TARGET" : (game.isInventoryOpen() ? "INV" : "PLAY");
    std::string lock = game.inputLocked() ? " (ANIM)" : "";
    drawText5x7(renderer, x, bottomY, scale, yellow,
        "WASD/ARROWS MOVE  . WAIT  G GET  I INV  F FIRE(RANGED)  > OR ENTER STAIRS  PGUP/PGDN LOG  R RESTART" + lock);

    // Scroll indicator + game over
    if (scroll > 0) {
        drawText5x7(renderer, winW - 190, hudTop + 10, scale, yellow, "LOG -" + std::to_string(scroll));
    }

    if (game.isGameOver()) {
        drawText5x7(renderer, winW - 250, hudTop + 32, scale, red, "GAME OVER");
        drawText5x7(renderer, winW - 250, hudTop + 52, scale, red, "PRESS R TO RESTART");
    }
}

void Renderer::drawInventoryOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const int panelW = winW - 40;
    const int panelH = winH - 40;
    SDL_Rect bg{ 20, 20, panelW, panelH };

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
    SDL_RenderDrawRect(renderer, &bg);

    const int scale = 2;
    const Color white{ 240, 240, 240, 255 };
    const Color yellow{ 255, 230, 120, 255 };
    const Color gray{ 160, 160, 160, 255 };

    int x = bg.x + 12;
    int y = bg.y + 12;

    drawText5x7(renderer, x, y, scale, yellow, "INVENTORY  (UP/DOWN SELECT, E EQUIP, U USE, X DROP, ESC CLOSE)  [M]=MELEE [R]=RANGED [A]=ARMOR");
    y += 7 * scale + 10;

    const auto& inv = game.inventory();
    int sel = game.inventorySelection();

    if (inv.empty()) {
        drawText5x7(renderer, x, y, scale, white, "(EMPTY)");
        return;
    }

    for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
        const Item& it = inv[static_cast<size_t>(i)];
        std::string line = (i == sel ? "> " : "  ");
        std::string tag = game.equippedTag(it.id);
        if (!tag.empty()) line += "[" + tag + "] ";
        else line += "    ";
        line += itemDisplayName(it);

        drawText5x7(renderer, x, y, scale, (i == sel ? white : gray), toUpper(line));
        y += 7 * scale + 4;

        if (y > bg.y + bg.h - 80) break;
    }

    // Footer hints about current selection
    const Item& cur = inv[static_cast<size_t>(clampi(sel, 0, static_cast<int>(inv.size()) - 1))];
    std::string hint = "SELECTED: " + itemDisplayName(cur);
    drawText5x7(renderer, x, bg.y + bg.h - 40, scale, yellow, toUpper(hint));
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
