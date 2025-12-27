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
        doorLockedTex[static_cast<size_t>(f)] = textureFromSprite(generateLockedDoorTile(0xD00Du, f));
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
            return floorVar[idx];
        }
        case TileType::Wall: {
            if (wallVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(wallVar.size()));
            return wallVar[idx];
        }
        case TileType::DoorSecret: {
            // Draw secret doors as walls until discovered (tile is converted to DoorClosed).
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

            SDL_Texture* tex = tileTexture(t.type, x, y, game.depth(), frame);
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
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
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
    if (game.isLooking()) {
        drawLookOverlay(game);
    }

    if (game.isTargeting()) {
        drawTargetingOverlay(game);
    }

    // HUD (messages, stats)
    drawHud(game);

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
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    SDL_RenderFillRect(renderer, &hudRect);

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
    ss << " | KEYS: " << game.keyCount() << " | PICKS: " << game.lockpickCount();
    ss << " | DEPTH: " << game.depth();
    ss << " | MAX: " << game.maxDepthReached();
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

    addStatus("POISON", p.poisonTurns);
    addStatus("WEB", p.webTurns);
    addStatus("REGEN", p.regenTurns);
    addStatus("SHIELD", p.shieldTurns);
    addStatus("HASTE", p.hasteTurns);
    addStatus("VISION", p.visionTurns);
    {
        const std::string ht = game.hungerTag();
        if (!ht.empty()) ss << " | " << ht;
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
        "MOVE: WASD/ARROWS/NUMPAD | SPACE/. WAIT | R REST | < > STAIRS");
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
        drawText5x7(renderer, 8, y, 2, c, msg.text);
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

    drawText5x7(renderer, x, y, scale, yellow,
               "INVENTORY  (UP/DOWN SELECT, ENTER ACT, E EQUIP, U USE, X DROP, SHIFT+X DROP ALL, SHIFT+S SORT, ESC CLOSE)");
    y += 7 * scale + 4;
    drawText5x7(renderer, x, y, scale, yellow, "[M]=MELEE [R]=RANGED [A]=ARMOR");
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
        line += game.displayItemName(it);

        drawText5x7(renderer, x, y, scale, (i == sel ? white : gray), toUpper(line));
        y += 7 * scale + 4;

        if (y > bg.y + bg.h - 80) break;
    }

    // Footer hints about current selection
    const Item& cur = inv[static_cast<size_t>(clampi(sel, 0, static_cast<int>(inv.size()) - 1))];
    std::string hint = "SELECTED: " + game.displayItemName(cur);
    drawText5x7(renderer, x, bg.y + bg.h - 40, scale, yellow, toUpper(hint));
}



void Renderer::drawOptionsOverlay(const Game& game) {
    const int panelW = std::min(winW - 80, 760);
    const int panelH = 288;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - panelH) / 2;

    SDL_Rect bg{x0, y0, panelW, panelH};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 210);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &bg);

    const int pad = 12;
    int x = x0 + pad;
    int y = y0 + pad;

    // Local UI palette.
    const Color white{255, 255, 255, 255};
    const Color gray{180, 180, 180, 255};

    drawText5x7(renderer, x, y, 2, white, "OPTIONS");
    y += 24;
    drawText5x7(renderer, x, y, 1, gray, "UP/DOWN SELECT  LEFT/RIGHT CHANGE  ESC CLOSE");
    y += 18;

    auto apLabel = [&]() -> const char* {
        switch (game.autoPickupMode()) {
            case AutoPickupMode::Off: return "OFF";
            case AutoPickupMode::Gold: return "GOLD";
            case AutoPickupMode::All: return "ALL";
        }
        return "GOLD";
    };

    const int sel = game.optionsSelection();

    auto drawOpt = [&](int idx, const std::string& label, const std::string& value) {
        const Color c = (sel == idx) ? white : gray;
        const std::string line = (sel == idx ? "> " : "  ") + label + value;
        drawText5x7(renderer, x, y, 2, c, line);
        y += 22;
    };

    drawOpt(0, "AUTO-PICKUP: ", apLabel());
    drawOpt(1, "AUTO-STEP DELAY: ", std::to_string(game.autoStepDelayMs()) + " MS");
    drawOpt(2, "AUTOSAVE EVERY: ", std::to_string(game.autosaveEveryTurns()) + " TURNS");
    drawOpt(3, "IDENTIFY ITEMS: ", game.identificationEnabled() ? "ON" : "OFF");
    drawOpt(4, "HUNGER SYSTEM: ", game.hungerEnabled() ? "ON" : "OFF");
    drawOpt(5, "EFFECT TIMERS: ", game.showEffectTimers() ? "ON" : "OFF");
    drawOpt(6, "CONFIRM QUIT: ", game.confirmQuitEnabled() ? "ON" : "OFF");
    drawOpt(7, "", "CLOSE");

    y = y0 + panelH - 42;

    auto baseName = [](const std::string& p) -> std::string {
        if (p.empty()) return {};
        size_t i = p.find_last_of("/\\");
        if (i == std::string::npos) return p;
        return p.substr(i + 1);
    };

    const std::string settingsFile = baseName(game.settingsPath());
    if (!settingsFile.empty()) {
        drawText5x7(renderer, x, y, 1, gray, "KEYBINDS: EDIT " + settingsFile + " (bind_*)");
    } else {
        drawText5x7(renderer, x, y, 1, gray, "KEYBINDS: EDIT procrogue_settings.ini (bind_*)");
    }
    y += 14;
    drawText5x7(renderer, x, y, 1, gray, "TIP: PRESS # FOR EXTENDED COMMANDS (save, load, pray, quit...)");
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
    lineGray("L/V LOOK  C SEARCH (TRAPS/SECRETS)  O EXPLORE  P AUTOPICKUP");
    lineGray("M MINIMAP  SHIFT+TAB STATS  F2 OPTIONS");
    lineGray("# EXTENDED COMMANDS  (TYPE + ENTER)");
    lineGray("F5 SAVE  F9 LOAD  F10 LOAD AUTO  F6 RESTART");
    lineGray("F11 FULLSCREEN  F12 SCREENSHOT (BINDABLE)");
    lineGray("PGUP/PGDN LOG  ESC CANCEL/QUIT");

    y += 6;
    lineWhite("EXTENDED COMMAND EXAMPLES:");
    lineGray("save | load | loadauto | quit | version | seed | name | scores");
    lineGray("autopickup off/gold/all");
    lineGray("name <text>  scores [N]");
    lineGray("autosave <turns>  stepdelay <ms>  identify on/off  timers on/off");
    lineGray("pray [heal|cure|identify|bless]");

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
    lineGray("SEARCH CAN REVEAL TRAPS AND SECRET DOORS.");
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
        ss << "DEPTH: " << game.depth() << "  (MAX: " << game.maxDepthReached() << ")";
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
