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
    if (game.isTargeting()) {
        drawTargetingOverlay(game);
    }

    // HUD (messages, stats)
    drawHud(game);

    if (game.isInventoryOpen()) {
        drawInventoryOverlay(game);
    }

    if (game.isHelpOpen()) {
        drawHelpOverlay(game);
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
    const Color orange{ 255, 190, 120, 255 };

    const Entity& p = game.player();

    // Stats line
    {
        std::stringstream ss;
        ss << "HP " << p.hp << "/" << p.hpMax
           << "  ATK " << game.playerAttack()
           << "  DEF " << game.playerDefense()
           << "  GOLD " << game.goldCount()
           << "  DEPTH " << game.depth()
           << "  LVL " << game.characterLevel()
           << "  XP " << game.experience() << "/" << game.experienceToNext();
        if (game.playerHasAmulet()) ss << "  AMULET";

        // QoL / status indicators
        ss << (game.autoPickupEnabled() ? "  AUTO$" : "  AUTO$OFF");
        if (p.poisonTurns > 0) ss << "  POISON(" << p.poisonTurns << ")";
        if (p.regenTurns > 0) ss << "  REGEN(" << p.regenTurns << ")";
        if (p.shieldTurns > 0) ss << "  SHIELD(" << p.shieldTurns << ")";
        drawText5x7(renderer, 4, hudTop + 2, scale, white, ss.str());
    }

    // Equipped gear line
    {
        std::stringstream eq;
        eq << "EQUIP: "
           << "MELEE=" << game.equippedMeleeName() << "  "
           << "RANGED=" << game.equippedRangedName() << "  "
           << "ARMOR=" << game.equippedArmorName();
        drawText5x7(renderer, 4, hudTop + 16, scale, gray, eq.str());
    }

    // Objective line
    {
        std::string obj = game.playerHasAmulet()
            ? "OBJECTIVE: RETURN TO THE EXIT (<) ON DEPTH 1."
            : "OBJECTIVE: FIND THE AMULET OF YENDOR ON DEPTH 5.";
        drawText5x7(renderer, 4, hudTop + 30, scale, yellow, obj);
    }

    // Messages (scrolled)
    const auto& msgs = game.messages();
    const int scroll = game.messageScroll();
    const int lineH = 16;
    const int startY = hudTop + 44;
    const int maxLines = (hudH - 44 - 20) / lineH;

    int end = static_cast<int>(msgs.size()) - scroll;
    end = clampi(end, 0, static_cast<int>(msgs.size()));
    int start = std::max(0, end - maxLines);

    for (int i = start; i < end; ++i) {
        const int y = startY + (i - start) * lineH;
        Color c = white;
        switch (msgs[i].kind) {
            case MessageKind::Combat:  c = red; break;
            case MessageKind::Loot:    c = yellow; break;
            case MessageKind::Success: c = green; break;
            case MessageKind::Warning: c = orange; break;
            case MessageKind::System:  c = gray; break;
            case MessageKind::Info:
            default: c = msgs[i].fromPlayer ? white : gray; break;
        }
        drawText5x7(renderer, 4, y, scale, c, msgs[i].text);
    }

    if (scroll > 0) {
        std::stringstream ss;
        ss << "LOG -" << scroll;
        drawText5x7(renderer, winW - 130, hudTop + 2, scale, gray, ss.str());
    }

    // Controls line
    {
        const std::string help =
            "MOVE WASD/ARROWS  . WAIT  G GET  I INV  C SEARCH  P AUTO$  ? HELP  F FIRE  < UP  > DOWN  F5 SAVE  F9 LOAD  PGUP/PGDN LOG  R RESTART";
        drawText5x7(renderer, 4, hudTop + hudH - 16, scale, gray, help);
    }

    // Game over / win overlays
    if (game.isGameWon()) {
        drawText5x7(renderer, winW / 2 - 60, hudTop + 54, 4, green, "VICTORY!");
        drawText5x7(renderer, winW / 2 - 110, hudTop + 82, 3, gray, "R RESTART   F9 LOAD");
    } else if (game.isGameOver()) {
        drawText5x7(renderer, winW / 2 - 70, hudTop + 54, 4, red, "GAME OVER");
        drawText5x7(renderer, winW / 2 - 110, hudTop + 82, 3, gray, "R RESTART   F9 LOAD");
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

    auto line = [&](const std::string& s, Color c = gray) {
        drawText5x7(renderer, x, y, scale, c, s);
        y += 7 * scale + 6;
    };

    line("GOAL: FIND THE AMULET OF YENDOR ON DEPTH 5.", white);
    line("      RETURN TO THE EXIT (<) ON DEPTH 1 TO WIN.", white);
    y += 6;

    line("MOVE: WASD / ARROW KEYS");
    line("WAIT: . (PERIOD)");
    line("PICK UP: G");
    line("SEARCH: C (REVEAL NEARBY TRAPS)");
    line("AUTO-PICKUP GOLD: P (TOGGLE)");
    line("INVENTORY: I   (E EQUIP, U USE, X DROP, ESC CLOSE)");
    line("RANGED: F TARGET, ENTER FIRE, ESC CANCEL");
    line("STAIRS: < UP, > DOWN   (ENTER ALSO WORKS WHILE STANDING ON STAIRS)");
    line("LOG SCROLL: PAGEUP / PAGEDOWN");
    line("SAVE / LOAD: F5 / F9");
    line("RESTART: R");

    y += 10;
    line("TIPS:", yellow);
    line("- BUMP ENEMIES TO MELEE ATTACK.");
    line("- POTIONS OF STRENGTH PERMANENTLY INCREASE ATK.");
    line("- POTION OF ANTIDOTE CURES POISON.");
    line("- POTION OF REGENERATION HEALS OVER TIME.");
    line("- POTION OF SHIELDING TEMPORARILY BOOSTS DEF.");
    line("- ENCHANTMENT SCROLLS IMPROVE EQUIPPED GEAR.");
    line("- DISCOVERED TRAPS ARE DRAWN AS X.");
    line("- SCROLL OF MAPPING REVEALS THE WHOLE FLOOR.");
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
