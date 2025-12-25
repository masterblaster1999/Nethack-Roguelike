#include "render.hpp"
#include "ui_font.hpp"
#include "rng.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

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
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
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

    if (game.isHelpOpen()) {
        drawHelpOverlay(game);
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
    drawText5x7(renderer, 8, winH - hudH + 8, 2, white, "PROCROGUE++");

    const Entity& p = game.player();
    std::stringstream ss;
    ss << "HP: " << p.hp << "/" << p.hpMax;
    ss << " | LV: " << game.playerCharLevel();
    ss << " | XP: " << game.playerXp() << "/" << game.playerXpToNext();
    ss << " | GOLD: " << game.goldCount();
    ss << " | DEPTH: " << game.depth();
    ss << " | MAX: " << game.maxDepthReached();
    ss << " | TURNS: " << game.turns();
    ss << " | KILLS: " << game.kills();

    // Status effects (compact)
    if (p.poisonTurns > 0) ss << " | POISON";
    if (p.webTurns > 0) ss << " | WEB";
    if (p.regenTurns > 0) ss << " | REGEN";
    if (p.shieldTurns > 0) ss << " | SHIELD";
    if (p.hasteTurns > 0) ss << " | HASTE";
    if (p.visionTurns > 0) ss << " | VISION";
    if (game.autosaveEveryTurns() > 0) {
        ss << " | AS: " << game.autosaveEveryTurns();
    }
    drawText5x7(renderer, 8, winH - hudH + 24, 2, white, ss.str());

    // Controls (3 compact lines)
    const int controlY1 = winH - 48;
    const int controlY2 = winH - 32;
    const int controlY3 = winH - 16;

    drawText5x7(renderer, 8, controlY1, 2, gray,
        "MOVE: WASD/ARROWS/YUBN | . WAIT | Z REST | < > STAIRS");
    drawText5x7(renderer, 8, controlY2, 2, gray,
        "G PICK | I INV | F FIRE | L LOOK | C SRCH | O AUTO | P PICK");
    drawText5x7(renderer, 8, controlY3, 2, gray,
        "M MINI | TAB STATS | F5 SAVE | F9 LOAD | F12 SHOT | ? HELP");

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


void Renderer::drawHelpOverlay(const Game& game) {
    (void)game;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const int panelW = winW - 40;
    const int panelH = winH - 40;
    SDL_Rect bg{ 20, 20, panelW, panelH };

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 210);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
    SDL_RenderDrawRect(renderer, &bg);

    const int scale = 2;
    const Color white{ 240, 240, 240, 255 };
    const Color gray{ 170, 170, 170, 255 };
    const Color yellow{ 255, 230, 120, 255 };

    int x = bg.x + 12;
    int y = bg.y + 12;

    drawText5x7(renderer, x, y, scale, yellow, "HELP  (PRESS ? OR ESC TO CLOSE)");
    y += 7 * scale + 10;

    auto line = [&](const std::string& s, const Color& c) {
        drawText5x7(renderer, x, y, scale, c, s);
        y += 7 * scale + 6;
    };

    auto lineGray = [&](const std::string& s) {
        line(s, gray);
    };

    line("GOAL: FIND THE AMULET OF YENDOR ON DEPTH 5.", white);
    line("      RETURN TO THE EXIT (<) ON DEPTH 1 TO WIN.", white);
    y += 6;

    lineGray("MOVE: WASD / ARROWS / YUBN (DIAGONALS)");
    lineGray("WAIT: . (PERIOD)");
    lineGray("LOOK: L / V (EXAMINE WITHOUT A TURN)");
    lineGray("REST: Z (WAIT UNTIL HEALED; STOPS ON DANGER)");
    lineGray("PICK UP: G");
    lineGray("SEARCH: C (REVEAL NEARBY TRAPS)");
    lineGray("AUTO-EXPLORE: O (EXPLORE UNTIL INTERRUPTED)");
    lineGray("AUTO-PICKUP: P (CYCLE OFF/GOLD/ALL)");
    lineGray("MINIMAP: M");
    lineGray("STATS / SCORES: TAB");
    lineGray("SCREENSHOT: F12 (SAVES A .BMP)");
    lineGray("MOUSE: LMB AUTO-TRAVEL, RMB LOOK");
    lineGray("INVENTORY: I   (E EQUIP, U USE, X DROP, ESC CLOSE)");
    lineGray("RANGED: F TARGET, ENTER FIRE, ESC CANCEL");
    lineGray("STAIRS: < UP, > DOWN   (ENTER ALSO WORKS WHILE STANDING ON STAIRS)");
    lineGray("LOG SCROLL: PAGEUP / PAGEDOWN");
    lineGray("FULLSCREEN: F11 (TOGGLE)");
    lineGray("SAVE / LOAD: F5 / F9 (SEE AUTOSAVES IN SETTINGS)");
    lineGray("RESTART: R");

    y += 10;
    line("TIPS:", yellow);
    lineGray("- BUMP ENEMIES TO MELEE ATTACK.");
    lineGray("- POTIONS OF STRENGTH PERMANENTLY INCREASE ATK.");
    lineGray("- POTION OF ANTIDOTE CURES POISON.");
    lineGray("- POTION OF REGENERATION HEALS OVER TIME.");
    lineGray("- POTION OF SHIELDING TEMPORARILY BOOSTS DEF.");
    lineGray("- POTION OF HASTE GRANTS EXTRA ACTIONS.");
    lineGray("- POTION OF VISION INCREASES SIGHT RANGE.");
    lineGray("- ENCHANTMENT SCROLLS IMPROVE EQUIPPED GEAR.");
    lineGray("- POTIONS/SCROLLS START UNIDENTIFIED (USE THEM OR READ AN IDENTIFY SCROLL).");
    lineGray("- SPIDERS CAN WEB YOU (YOU CAN'T MOVE UNTIL THE WEB WEARS OFF).");
    lineGray("- DISCOVERED TRAPS ARE DRAWN AS X.");
    lineGray("- SCROLL OF MAPPING REVEALS THE WHOLE FLOOR.");
}


void Renderer::drawMinimapOverlay(const Game& game) {
    const Dungeon& d = game.dungeon();
    const int W = d.width();
    const int H = d.height();

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
        ss << "TURNS: " << game.turns() << "  KILLS: " << game.kills() << "  GOLD: " << game.goldCount();
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
            std::stringstream ss;
            ss << "#" << (i + 1)
               << "  " << e.score
               << "  " << (e.won ? "WIN" : "DEAD")
               << "  D" << e.depth
               << "  T" << e.turns
               << "  K" << e.kills
               << "  L" << e.level
               << "  G" << e.gold
               << "  SEED " << e.seed;
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
