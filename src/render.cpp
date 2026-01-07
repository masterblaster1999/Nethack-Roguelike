#include "render.hpp"
#include "ui_font.hpp"
#include "hallucination.hpp"
#include "rng.hpp"
#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <iomanip>

namespace {

// Small RAII helper to scope SDL clip rectangles safely (restores previous clip).
struct ClipRectGuard {
    SDL_Renderer* r = nullptr;
    SDL_Rect prev{};
    bool hadPrev = false;

    ClipRectGuard(SDL_Renderer* renderer, const SDL_Rect* rect) : r(renderer) {
        if (!r) return;
        hadPrev = SDL_RenderIsClipEnabled(r) == SDL_TRUE;
        if (hadPrev) SDL_RenderGetClipRect(r, &prev);
        SDL_RenderSetClipRect(r, rect);
    }

    ~ClipRectGuard() {
        if (!r) return;
        if (hadPrev) SDL_RenderSetClipRect(r, &prev);
        else SDL_RenderSetClipRect(r, nullptr);
    }

    ClipRectGuard(const ClipRectGuard&) = delete;
    ClipRectGuard& operator=(const ClipRectGuard&) = delete;
};

// --- Isometric helpers ---
// mapTileDst() returns the bounding box of the diamond tile in iso mode.
inline void isoDiamondCorners(const SDL_Rect& base, SDL_Point& top, SDL_Point& right, SDL_Point& bottom, SDL_Point& left) {
    const int cx = base.x + base.w / 2;
    const int cy = base.y + base.h / 2;

    top    = SDL_Point{cx, base.y};
    right  = SDL_Point{base.x + base.w, cy};
    bottom = SDL_Point{cx, base.y + base.h};
    left   = SDL_Point{base.x, cy};
}

inline void drawIsoDiamondOutline(SDL_Renderer* r, const SDL_Rect& base) {
    if (!r) return;
    SDL_Point top{}, right{}, bottom{}, left{};
    isoDiamondCorners(base, top, right, bottom, left);
    SDL_RenderDrawLine(r, top.x, top.y, right.x, right.y);
    SDL_RenderDrawLine(r, right.x, right.y, bottom.x, bottom.y);
    SDL_RenderDrawLine(r, bottom.x, bottom.y, left.x, left.y);
    SDL_RenderDrawLine(r, left.x, left.y, top.x, top.y);
}

inline void drawIsoDiamondCross(SDL_Renderer* r, const SDL_Rect& base) {
    if (!r) return;
    SDL_Point top{}, right{}, bottom{}, left{};
    isoDiamondCorners(base, top, right, bottom, left);
    SDL_RenderDrawLine(r, left.x, left.y, right.x, right.y);
    SDL_RenderDrawLine(r, top.x, top.y, bottom.x, bottom.y);
}

inline bool pointInIsoDiamond(int px, int py, const SDL_Rect& base) {
    // Diamond equation in normalized coordinates:
    //   |dx|/(w/2) + |dy|/(h/2) <= 1
    const int hw = std::max(1, base.w / 2);
    const int hh = std::max(1, base.h / 2);
    const int cx = base.x + hw;
    const int cy = base.y + hh;

    const float nx = std::abs(static_cast<float>(px - cx)) / static_cast<float>(hw);
    const float ny = std::abs(static_cast<float>(py - cy)) / static_cast<float>(hh);
    return (nx + ny) <= 1.0f;
}

inline void fillIsoDiamond(SDL_Renderer* r, int cx, int cy, int halfW, int halfH) {
    if (!r) return;
    halfW = std::max(1, halfW);
    halfH = std::max(1, halfH);

    // Rasterize a small diamond using horizontal scanlines.
    // The width scales linearly with vertical distance from the center.
    for (int dy = -halfH; dy <= halfH; ++dy) {
        const float t = 1.0f - (static_cast<float>(std::abs(dy)) / static_cast<float>(halfH));
        const int w = std::max(0, static_cast<int>(std::round(static_cast<float>(halfW) * t)));
        SDL_RenderDrawLine(r, cx - w, cy + dy, cx + w, cy + dy);
    }
}

} // namespace


Renderer::Renderer(int windowW, int windowH, int tileSize, int hudHeight, bool vsync, int textureCacheMB_)
    : winW(windowW), winH(windowH), tile(tileSize), hudH(hudHeight), vsyncEnabled(vsync), textureCacheMB(textureCacheMB_) {
    // Derive viewport size in tiles from the logical window size.
    // The bottom HUD area is not part of the map viewport.
    const int t = std::max(1, tile);
    viewTilesW = std::max(1, winW / t);
    viewTilesH = std::max(1, std::max(0, winH - hudH) / t);

    camX = 0;
    camY = 0;
}

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

    // Pre-generate tile variants (with animation frames)
    // Procedural sprite generation now supports higher-res output (up to 256x256).
    // We generate map sprites at (tile) resolution to avoid renderer scaling artifacts.
    const int spritePx = std::clamp(tile, 16, 256);

    // Sprite cache sizing:
    // - Each cached entry stores FRAMES textures of size spritePx*spritePx RGBA.
    // - This is an approximation, but it's stable and lets us cap VRAM usage.
    spriteEntryBytes = static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx) * sizeof(uint32_t) * static_cast<size_t>(FRAMES);

    // Scale some overlay variant counts down for huge tile sizes (keeps VRAM in check).
    decalsPerStyleUsed = (spritePx <= 48) ? 6 : (spritePx <= 96 ? 5 : (spritePx <= 160 ? 4 : 3));
    decalsPerStyleUsed = std::clamp(decalsPerStyleUsed, 1, DECALS_PER_STYLE);

    autoVarsUsed = (spritePx <= 96) ? 4 : (spritePx <= 160 ? 3 : 2);
    autoVarsUsed = std::clamp(autoVarsUsed, 1, AUTO_VARS);

    // Configure the sprite texture cache budget.
    // 0 => unlimited (no eviction).
    size_t budgetBytes = 0;
    if (textureCacheMB > 0) {
        budgetBytes = static_cast<size_t>(textureCacheMB) * 1024ull * 1024ull;
        // Ensure the budget can hold at least a small working set (prevents thrash).
        const size_t minBudget = spriteEntryBytes * 12ull; // ~12 sprites worth
        if (budgetBytes < minBudget) budgetBytes = minBudget;
    }
    spriteTex.setBudgetBytes(budgetBytes);
    spriteTex.resetStats();

    // More variants reduce visible repetition, but large tile sizes can become
    // expensive in VRAM. Scale the variant count down as tile size increases.
    const int tileVars = (spritePx <= 48) ? 18 : (spritePx <= 96 ? 14 : (spritePx <= 160 ? 10 : 8));

    for (auto& v : floorThemeVar) v.clear();
    wallVar.clear();
    chasmVar.clear();
    pillarOverlayVar.clear();
    boulderOverlayVar.clear();

    for (auto& v : floorThemeVar) v.resize(static_cast<size_t>(tileVars));
    wallVar.resize(static_cast<size_t>(tileVars));
    chasmVar.resize(static_cast<size_t>(tileVars));
    pillarOverlayVar.resize(static_cast<size_t>(tileVars));
    boulderOverlayVar.resize(static_cast<size_t>(tileVars));

    for (int i = 0; i < tileVars; ++i) {
        // Floor: build a full themed tileset so special rooms pop.
        for (int st = 0; st < ROOM_STYLES; ++st) {
            const uint32_t fSeed = hashCombine(hashCombine(0xF1000u, static_cast<uint32_t>(st)), static_cast<uint32_t>(i));
            for (int f = 0; f < FRAMES; ++f) {
                floorThemeVar[static_cast<size_t>(st)][static_cast<size_t>(i)][static_cast<size_t>(f)] =
                    textureFromSprite(generateThemedFloorTile(fSeed, static_cast<uint8_t>(st), f, spritePx));
            }
        }

        // Other base terrain (not room-themed yet).
        const uint32_t wSeed = hashCombine(0xAA110u, static_cast<uint32_t>(i));
        const uint32_t cSeed = hashCombine(0xC1A500u, static_cast<uint32_t>(i));
        const uint32_t pSeed = hashCombine(0x9111A0u, static_cast<uint32_t>(i));
        const uint32_t bSeed = hashCombine(0xB011D3u, static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            wallVar[static_cast<size_t>(i)][static_cast<size_t>(f)]  = textureFromSprite(generateWallTile(wSeed, f, spritePx));
            chasmVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateChasmTile(cSeed, f, spritePx));
            // Pillar is generated as a transparent overlay; it will be layered over the
            // underlying themed floor at render-time.
            pillarOverlayVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generatePillarTile(pSeed, f, spritePx));
            boulderOverlayVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateBoulderTile(bSeed, f, spritePx));
        }
    }

    for (int f = 0; f < FRAMES; ++f) {
        // Doors and stairs are rendered as overlays layered over the underlying themed floor.
        stairsUpOverlayTex[static_cast<size_t>(f)]   = textureFromSprite(generateStairsTile(0x515A1u, true, f, spritePx));
        stairsDownOverlayTex[static_cast<size_t>(f)] = textureFromSprite(generateStairsTile(0x515A2u, false, f, spritePx));
        doorClosedOverlayTex[static_cast<size_t>(f)] = textureFromSprite(generateDoorTile(0xD00Du, false, f, spritePx));
        doorLockedOverlayTex[static_cast<size_t>(f)] = textureFromSprite(generateLockedDoorTile(0xD00Du, f, spritePx));
        doorOpenOverlayTex[static_cast<size_t>(f)]   = textureFromSprite(generateDoorTile(0xD00Du, true, f, spritePx));
    }

// Default UI skin assets (will refresh if theme changes at runtime).
uiThemeCached = UITheme::DarkStone;
uiAssetsValid = true;
for (int f = 0; f < FRAMES; ++f) {
    uiPanelTileTex[static_cast<size_t>(f)] = textureFromSprite(generateUIPanelTile(uiThemeCached, 0x51A11u, f, 16));
    uiOrnamentTex[static_cast<size_t>(f)]  = textureFromSprite(generateUIOrnamentTile(uiThemeCached, 0x0ABCDu, f, 16));
}

// Pre-generate decal overlays (small transparent patterns blended onto tiles).
floorDecalVar.clear();
wallDecalVar.clear();
floorDecalVar.resize(static_cast<size_t>(DECAL_STYLES * static_cast<size_t>(decalsPerStyleUsed)));
wallDecalVar.resize(static_cast<size_t>(DECAL_STYLES * static_cast<size_t>(decalsPerStyleUsed)));
for (int st = 0; st < DECAL_STYLES; ++st) {
    for (int i = 0; i < decalsPerStyleUsed; ++i) {
        const uint32_t fSeed = hashCombine(0xD3CA10u + static_cast<uint32_t>(st) * 131u, static_cast<uint32_t>(i));
        const uint32_t wSeed = hashCombine(0xBADC0DEu + static_cast<uint32_t>(st) * 191u, static_cast<uint32_t>(i));
        const size_t idx = static_cast<size_t>(st * decalsPerStyleUsed + i);
        for (int f = 0; f < FRAMES; ++f) {
            floorDecalVar[idx][static_cast<size_t>(f)] = textureFromSprite(generateFloorDecalTile(fSeed, static_cast<uint8_t>(st), f, spritePx));
            wallDecalVar[idx][static_cast<size_t>(f)]  = textureFromSprite(generateWallDecalTile(wSeed, static_cast<uint8_t>(st), f, spritePx));
        }
    }
}


// Pre-generate autotile overlays (edge/corner shaping for walls and chasm rims).
for (int mask = 0; mask < AUTO_MASKS; ++mask) {
    for (int v = 0; v < autoVarsUsed; ++v) {
        const uint32_t wSeed = hashCombine(0xE0D6E00u + static_cast<uint32_t>(mask) * 131u, static_cast<uint32_t>(v));
        const uint32_t cSeed = hashCombine(0xC0A5E00u + static_cast<uint32_t>(mask) * 191u, static_cast<uint32_t>(v));
        for (int f = 0; f < FRAMES; ++f) {
            wallEdgeVar[static_cast<size_t>(mask)][static_cast<size_t>(v)][static_cast<size_t>(f)] =
                (mask == 0) ? nullptr : textureFromSprite(generateWallEdgeOverlay(wSeed, static_cast<uint8_t>(mask), v, f, spritePx));
            chasmRimVar[static_cast<size_t>(mask)][static_cast<size_t>(v)][static_cast<size_t>(f)] =
                (mask == 0) ? nullptr : textureFromSprite(generateChasmRimOverlay(cSeed, static_cast<uint8_t>(mask), v, f, spritePx));
        }
    }
}

// Pre-generate confusion gas overlay tiles.
for (int i = 0; i < GAS_VARS; ++i) {
    const uint32_t gSeed = hashCombine(0x6A5u, static_cast<uint32_t>(i));
    for (int f = 0; f < FRAMES; ++f) {
        gasVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateConfusionGasTile(gSeed, f, spritePx));
    }
}

// Pre-generate fire overlay tiles.
for (int i = 0; i < FIRE_VARS; ++i) {
    const uint32_t fSeed = hashCombine(0xF17Eu, static_cast<uint32_t>(i));
    for (int f = 0; f < FRAMES; ++f) {
        fireVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateFireTile(fSeed, f, spritePx));
    }
}

// Pre-generate HUD effect icons.
for (int k = 0; k < EFFECT_KIND_COUNT; ++k) {
    const EffectKind ek = static_cast<EffectKind>(k);
    for (int f = 0; f < FRAMES; ++f) {
        effectIconTex[static_cast<size_t>(k)][static_cast<size_t>(f)] = textureFromSprite(generateEffectIcon(ek, f, 16));
    }
}

// Reset room-type cache (rebuilt lazily in render()).
roomTypeCache.clear();
roomCacheDungeon = nullptr;
roomCacheDepth = -1;
roomCacheW = 0;
roomCacheH = 0;
roomCacheRooms = 0;

    initialized = true;
    return true;
}

void Renderer::shutdown() {
    if (!initialized) {
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        return;
    }

for (auto& styleVec : floorThemeVar) {
    for (auto& arr : styleVec) {
        for (SDL_Texture* t : arr) {
            if (t) SDL_DestroyTexture(t);
        }
    }
    styleVec.clear();
}

// Isometric terrain tiles (generated lazily).
for (auto& styleVec : floorThemeVarIso) {
    for (auto& arr : styleVec) {
        for (SDL_Texture* t : arr) {
            if (t) SDL_DestroyTexture(t);
        }
    }
    styleVec.clear();
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
for (auto& arr : chasmVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : pillarOverlayVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : boulderOverlayVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : wallBlockVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
wallVar.clear();
chasmVar.clear();
pillarOverlayVar.clear();
boulderOverlayVar.clear();

chasmVarIso.clear();
wallBlockVarIso.clear();

for (auto& t : stairsUpOverlayIsoTex) if (t) SDL_DestroyTexture(t);
for (auto& t : stairsDownOverlayIsoTex) if (t) SDL_DestroyTexture(t);
for (auto& t : doorOpenOverlayIsoTex) if (t) SDL_DestroyTexture(t);
stairsUpOverlayIsoTex.fill(nullptr);
stairsDownOverlayIsoTex.fill(nullptr);
doorOpenOverlayIsoTex.fill(nullptr);
isoTerrainAssetsValid = false;

// Decal overlay textures
for (auto& arr : floorDecalVar) {
    for (SDL_Texture*& t : arr) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}
for (auto& arr : wallDecalVar) {
    for (SDL_Texture*& t : arr) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}
floorDecalVar.clear();
wallDecalVar.clear();


// Autotile overlays
for (auto& maskArr : wallEdgeVar) {
    for (auto& anim : maskArr) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
}
for (auto& maskArr : chasmRimVar) {
    for (auto& anim : maskArr) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
}

// Confusion gas overlays
for (auto& anim : gasVar) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Confusion gas overlays (isometric)
for (auto& anim : gasVarIso) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Fire overlays
for (auto& anim : fireVar) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Fire overlays (isometric)
for (auto& anim : fireVarIso) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Status effect icons
for (auto& arr : effectIconTex) {
    for (SDL_Texture*& t : arr) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

roomTypeCache.clear();
roomCacheDungeon = nullptr;
roomCacheDepth = -1;
roomCacheW = 0;
roomCacheH = 0;
roomCacheRooms = 0;

for (auto& t : uiPanelTileTex) if (t) SDL_DestroyTexture(t);
for (auto& t : uiOrnamentTex) if (t) SDL_DestroyTexture(t);
uiPanelTileTex.fill(nullptr);
uiOrnamentTex.fill(nullptr);
uiAssetsValid = false;

    for (auto& t : stairsUpOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : stairsDownOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorClosedOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorLockedOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorOpenOverlayTex) if (t) SDL_DestroyTexture(t);

    stairsUpOverlayTex.fill(nullptr);
    stairsDownOverlayTex.fill(nullptr);
    doorClosedOverlayTex.fill(nullptr);
    doorLockedOverlayTex.fill(nullptr);
    doorOpenOverlayTex.fill(nullptr);

    // Entity/item/projectile textures are budget-cached in spriteTex.
    spriteTex.clear();

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

SDL_Rect Renderer::mapTileDst(int mapX, int mapY) const {
    // Map-space tiles are drawn relative to the camera and then optionally offset by transient
    // screen shake (mapOffX/Y).
    //
    // TopDown: camX/camY represents the viewport's top-left map tile.
    // Isometric: isoCamX/isoCamY represents the camera center tile.
    if (viewMode_ != ViewMode::Isometric) {
        return SDL_Rect{ (mapX - camX) * tile + mapOffX, (mapY - camY) * tile + mapOffY, tile, tile };
    }

    const int tileW = std::max(1, tile);
    const int tileH = std::max(1, tile / 2);

    const int halfW = std::max(1, tileW / 2);
    const int halfH = std::max(1, tileH / 2);

    const int mapH = std::max(0, winH - hudH);

    // Anchor the camera tile at the center of the map viewport (not including the HUD).
    const int cx = winW / 2 + mapOffX;
    const int cy = mapH / 2 + mapOffY;

    const int dx = mapX - isoCamX;
    const int dy = mapY - isoCamY;

    // Standard isometric projection (diamond grid).
    const int centerX = cx + (dx - dy) * halfW;
    const int centerY = cy + (dx + dy) * halfH;

    return SDL_Rect{ centerX - tileW / 2, centerY - tileH / 2, tileW, tileH };
}

SDL_Rect Renderer::mapSpriteDst(int mapX, int mapY) const {
    if (viewMode_ != ViewMode::Isometric) return mapTileDst(mapX, mapY);

    // Place sprites so their "feet" land on the center of the isometric tile.
    const SDL_Rect base = mapTileDst(mapX, mapY);
    const int cx = base.x + base.w / 2;
    const int cy = base.y + base.h / 2;

    const int spriteW = std::max(1, tile);
    const int spriteH = std::max(1, tile);

    // Nudge the foot point slightly downward so the sprite reads as standing on the tile.
    const int footY = cy + (base.h / 4);

    return SDL_Rect{ cx - spriteW / 2, footY - spriteH, spriteW, spriteH };
}

bool Renderer::mapTileInView(int mapX, int mapY) const {
    if (viewMode_ != ViewMode::Isometric) {
        return mapX >= camX && mapY >= camY &&
               mapX < (camX + viewTilesW) && mapY < (camY + viewTilesH);
    }

    // In isometric mode, the "viewport" is not axis-aligned in map-space, so we cull by screen rect.
    const SDL_Rect r = mapTileDst(mapX, mapY);
    const int mapH = std::max(0, winH - hudH);
    const int pad = std::max(0, tile); // allow for tall sprites that extend beyond the tile rect

    return !(r.x + r.w < -pad || r.y + r.h < -pad || r.x > (winW + pad) || r.y > (mapH + pad));
}

void Renderer::updateCamera(const Game& game) {
    const Dungeon& d = game.dungeon();


    // Re-derive viewport size in case logical sizing changed.
    const int t = std::max(1, tile);
    viewTilesW = std::max(1, winW / t);
    viewTilesH = std::max(1, std::max(0, winH - hudH) / t);

    // If the viewport fully contains the map, keep camera locked at origin.
    const int maxCamX = std::max(0, d.width - viewTilesW);
    const int maxCamY = std::max(0, d.height - viewTilesH);
    if (maxCamX == 0) camX = 0;
    if (maxCamY == 0) camY = 0;

    // Focus point selection:
    // - Normal: follow the player.
    // - Look: follow the look cursor (so you can pan around).
    // - Targeting: try to keep BOTH player and cursor on-screen if they fit,
    //   otherwise follow the cursor.
    Vec2i playerPos = game.player().pos;

    Vec2i cursorPos = playerPos;
    bool usingCursor = false;
    if (game.isLooking()) {
        cursorPos = game.lookCursor();
        usingCursor = true;
    } else if (game.isTargeting()) {
        cursorPos = game.targetingCursor();
        usingCursor = true;
    }

    // Isometric view: first pass is a simple centered camera on the current focus tile.
    // (TopDown mode retains the existing deadzone + targeting camera logic below.)
    if (viewMode_ == ViewMode::Isometric) {
        Vec2i focus = usingCursor ? cursorPos : playerPos;

        focus.x = std::clamp(focus.x, 0, std::max(0, d.width - 1));
        focus.y = std::clamp(focus.y, 0, std::max(0, d.height - 1));

        isoCamX = focus.x;
        isoCamY = focus.y;
        return;
    }

    auto clampCam = [&]() {
        camX = std::clamp(camX, 0, maxCamX);
        camY = std::clamp(camY, 0, maxCamY);
    };

    // Targeting: keep both points in view when possible.
    if (game.isTargeting() && usingCursor && (maxCamX > 0 || maxCamY > 0)) {
        const int minX = std::min(playerPos.x, cursorPos.x);
        const int maxX = std::max(playerPos.x, cursorPos.x);
        const int minY = std::min(playerPos.y, cursorPos.y);
        const int maxY = std::max(playerPos.y, cursorPos.y);

        if ((maxX - minX + 1) <= viewTilesW && (maxY - minY + 1) <= viewTilesH) {
            const int cx = (minX + maxX) / 2;
            const int cy = (minY + maxY) / 2;
            camX = cx - viewTilesW / 2;
            camY = cy - viewTilesH / 2;
            clampCam();
            return;
        }
    }

    // Deadzone follow (prevents jitter when moving near the center).
    Vec2i focus = usingCursor ? cursorPos : playerPos;

    // Clamp focus to map bounds defensively.
    focus.x = std::clamp(focus.x, 0, std::max(0, d.width - 1));
    focus.y = std::clamp(focus.y, 0, std::max(0, d.height - 1));

    // Margins: smaller viewports need smaller deadzones.
    const int marginX = std::clamp(viewTilesW / 4, 0, std::max(0, (viewTilesW - 1) / 2));
    const int marginY = std::clamp(viewTilesH / 4, 0, std::max(0, (viewTilesH - 1) / 2));

    if (maxCamX > 0) {
        const int left = camX + marginX;
        const int right = camX + viewTilesW - 1 - marginX;
        if (focus.x < left) camX = focus.x - marginX;
        else if (focus.x > right) camX = focus.x - (viewTilesW - 1 - marginX);
    }

    if (maxCamY > 0) {
        const int top = camY + marginY;
        const int bottom = camY + viewTilesH - 1 - marginY;
        if (focus.y < top) camY = focus.y - marginY;
        else if (focus.y > bottom) camY = focus.y - (viewTilesH - 1 - marginY);
    }

    clampCam();
}

bool Renderer::windowToMapTile(int winX, int winY, int& tileX, int& tileY) const {
    if (!renderer) return false;

    float lx = 0.0f, ly = 0.0f;
    SDL_RenderWindowToLogical(renderer, winX, winY, &lx, &ly);

    const int x = static_cast<int>(lx);
    const int y = static_cast<int>(ly);

    if (x < 0 || y < 0) return false;

    // Map rendering can be temporarily offset (screen shake). Convert clicks in window coordinates
    // back into stable viewport coordinates.
    const int mx = x - mapOffX;
    const int my = y - mapOffY;

    if (mx < 0 || my < 0) return false;

    const int mapH = std::max(0, winH - hudH);

    // Reject clicks outside the map viewport (e.g., HUD area).
    if (my >= mapH) return false;

    if (viewMode_ == ViewMode::Isometric) {
        // Invert the isometric projection used by mapTileDst(), then refine by
        // diamond hit-testing so mouse clicks feel crisp near tile edges.
        const int tileW = std::max(1, tile);
        const int tileH = std::max(1, tile / 2);

        const int halfW = std::max(1, tileW / 2);
        const int halfH = std::max(1, tileH / 2);

        const int cx = winW / 2;
        const int cy = mapH / 2;

        const float dx = static_cast<float>(mx - cx);
        const float dy = static_cast<float>(my - cy);

        const float fx = (dx / static_cast<float>(halfW) + dy / static_cast<float>(halfH)) * 0.5f;
        const float fy = (dy / static_cast<float>(halfH) - dx / static_cast<float>(halfW)) * 0.5f;

        auto roundToInt = [](float v) -> int {
            // Symmetric rounding for negatives.
            return (v >= 0.0f) ? static_cast<int>(std::floor(v + 0.5f)) : static_cast<int>(std::ceil(v - 0.5f));
        };

        const int rx = isoCamX + roundToInt(fx);
        const int ry = isoCamY + roundToInt(fy);

        // Candidate search: the point should lie within the diamond of one of the
        // nearby tiles. We check a small neighborhood around the rounded guess.
        int bestX = rx;
        int bestY = ry;
        int bestD2 = std::numeric_limits<int>::max();
        bool found = false;

        auto isoTileRectStable = [&](int mapX, int mapY) -> SDL_Rect {
            const int dxm = mapX - isoCamX;
            const int dym = mapY - isoCamY;

            const int centerX = cx + (dxm - dym) * halfW;
            const int centerY = cy + (dxm + dym) * halfH;

            return SDL_Rect{ centerX - tileW / 2, centerY - tileH / 2, tileW, tileH };
        };

        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int candX = rx + ox;
                const int candY = ry + oy;
                if (candX < 0 || candY < 0 || candX >= Game::MAP_W || candY >= Game::MAP_H) continue;

                const SDL_Rect rect = isoTileRectStable(candX, candY);
                if (!pointInIsoDiamond(mx, my, rect)) continue;

                const int ccx = rect.x + rect.w / 2;
                const int ccy = rect.y + rect.h / 2;
                const int ddx = mx - ccx;
                const int ddy = my - ccy;
                const int d2 = ddx * ddx + ddy * ddy;

                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestX = candX;
                    bestY = candY;
                    found = true;
                }
            }
        }

        tileX = found ? bestX : rx;
        tileY = found ? bestY : ry;

        if (tileX < 0 || tileY < 0 || tileX >= Game::MAP_W || tileY >= Game::MAP_H) return false;
        return true;
    }

    const int localX = mx / std::max(1, tile);
    const int localY = my / std::max(1, tile);

    // Reject clicks outside the map viewport.
    if (localX < 0 || localY < 0 || localX >= viewTilesW || localY >= viewTilesH) return false;

    tileX = localX + camX;
    tileY = localY + camY;

    if (tileX < 0 || tileY < 0 || tileX >= Game::MAP_W || tileY >= Game::MAP_H) return false;
    return true;
}

bool Renderer::windowToMinimapTile(const Game& game, int winX, int winY, int& tileX, int& tileY) const {
    if (!renderer) return false;

    float lx = 0.0f, ly = 0.0f;
    SDL_RenderWindowToLogical(renderer, winX, winY, &lx, &ly);

    const int x = static_cast<int>(lx);
    const int y = static_cast<int>(ly);
    if (x < 0 || y < 0) return false;

    const Dungeon& d = game.dungeon();
    const int W = d.width;
    const int H = d.height;
    if (W <= 0 || H <= 0) return false;

    // Mirror drawMinimapOverlay layout so hit-testing matches visuals.
    int px = 4;
    const int pad = 10;
    const int margin = 10;
    const int maxW = winW / 2;
    const int maxH = (winH - hudH) / 2;
    while (px > 2 && (W * px + pad * 2) > maxW) px--;
    while (px > 2 && (H * px + pad * 2) > maxH) px--;

    const int titleH = 16;
    const int panelW = W * px + pad * 2;
    const int panelH = H * px + pad * 2 + titleH;

    const int x0 = winW - panelW - margin;
    const int y0 = margin;

    const int mapX = x0 + pad;
    const int mapY = y0 + pad + titleH;

    if (x < mapX || y < mapY) return false;
    if (x >= mapX + W * px || y >= mapY + H * px) return false;

    const int tx = (x - mapX) / px;
    const int ty = (y - mapY) / px;

    tileX = std::clamp(tx, 0, W - 1);
    tileY = std::clamp(ty, 0, H - 1);
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

SDL_Texture* Renderer::tileTexture(TileType t, int x, int y, int level, int frame, int roomStyle) {
    uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(level), static_cast<uint32_t>(x)),
                             static_cast<uint32_t>(y));

    // Slightly decorrelate themed floors between styles.
    h = hashCombine(h, static_cast<uint32_t>(roomStyle));

    const bool iso = (viewMode_ == ViewMode::Isometric);

    switch (t) {
        case TileType::Floor: {
            const int s = std::clamp(roomStyle, 0, ROOM_STYLES - 1);
            const auto& vec = (iso && !floorThemeVarIso[static_cast<size_t>(s)].empty())
                                ? floorThemeVarIso[static_cast<size_t>(s)]
                                : floorThemeVar[static_cast<size_t>(s)];
            if (vec.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(vec.size()));
            return vec[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::Wall: {
            if (wallVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(wallVar.size()));
            return wallVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::Chasm: {
            const auto& vec = (iso && !chasmVarIso.empty()) ? chasmVarIso : chasmVar;
            if (vec.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(vec.size()));
            return vec[idx][static_cast<size_t>(frame % FRAMES)];
        }
        // Pillars/doors/stairs are rendered as overlays layered on top of the underlying floor.
        // Base tile fetch returns nullptr so the caller doesn't accidentally draw a standalone
        // overlay without its floor.
        case TileType::Pillar:
        case TileType::Boulder:
            return nullptr;
        case TileType::DoorSecret: {
            // Draw secret doors as walls until discovered (tile is converted to DoorClosed).
            if (wallVar.empty()) return nullptr;
            size_t idx = static_cast<size_t>(h % static_cast<uint32_t>(wallVar.size()));
            return wallVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::StairsUp:
        case TileType::StairsDown:
        case TileType::DoorClosed:
        case TileType::DoorLocked:
        case TileType::DoorOpen:
            return nullptr;
        default:
            return nullptr;
    }
}

namespace {
    // Sprite cache categories (packed into the high byte of the cache key).
    constexpr uint8_t CAT_ENTITY = 1;
    constexpr uint8_t CAT_ITEM = 2;
    constexpr uint8_t CAT_PROJECTILE = 3;

    // Key layout (uint64): [cat:8][kind:8][seed:32][unused:16]
    inline uint64_t makeSpriteKey(uint8_t cat, uint8_t kind, uint32_t seed) {
        return (static_cast<uint64_t>(cat) << 56) |
               (static_cast<uint64_t>(kind) << 48) |
               (static_cast<uint64_t>(seed) << 16);
    }

    // For NetHack-style identification, identifiable items have randomized
    // *appearances* each run (e.g., "ruby potion", "scroll labeled KLAATU").
    // If we rendered their true item-kind sprites, you'd be able to ID them
    // visually, which undermines the system.
    //
    // To fix this (and to add more procedural art variety), we switch the
    // sprite seed for identifiable items to a stable per-run "appearance seed"
    // and set SPRITE_SEED_IDENT_APPEARANCE_FLAG so spritegen can draw
    // appearance-based art.
    inline uint32_t identAppearanceSpriteSeed(const Game& game, ItemKind k) {
        const uint8_t app = game.appearanceFor(k);

        // Category salt keeps potion/scroll/ring/wand appearance id spaces separate.
        // (These are just arbitrary constants; determinism is all that matters.)
        uint32_t salt = 0x1D3A3u;
        if (isPotionKind(k)) salt = 0xA17C0DE1u;
        else if (isScrollKind(k)) salt = 0x5C2011D5u;
        else if (isRingKind(k)) salt = 0xBADC0FFEu;
        else if (isWandKind(k)) salt = 0xC001D00Du;

        const uint32_t mixed = hash32(hashCombine(game.seed() ^ salt, static_cast<uint32_t>(app)));
        return SPRITE_SEED_IDENT_APPEARANCE_FLAG | (mixed & 0x7FFFFF00u) | static_cast<uint32_t>(app);
    }

    inline void applyIdentificationVisuals(const Game& game, Item& it) {
        if (!game.identificationEnabled()) return;
        if (!isIdentifiableKind(it.kind)) return;
        it.spriteSeed = identAppearanceSpriteSeed(game, it.kind);
    }
}

SDL_Texture* Renderer::entityTexture(const Entity& e, int frame) {
    const int spritePx = std::clamp(tile, 16, 256);
    const uint64_t key = makeSpriteKey(CAT_ENTITY, static_cast<uint8_t>(e.kind), e.spriteSeed);

    auto arr = spriteTex.get(key);
    if (!arr) {
        std::array<SDL_Texture*, FRAMES> tex{};
        tex.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            tex[static_cast<size_t>(f)] = textureFromSprite(generateEntitySprite(e.kind, e.spriteSeed, f, voxelSpritesCached, spritePx));
        }
        const size_t bytes = (spriteEntryBytes != 0)
            ? spriteEntryBytes
            : (static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx) * sizeof(uint32_t) * static_cast<size_t>(FRAMES));

        spriteTex.put(key, tex, bytes);
        arr = spriteTex.get(key);
        if (!arr) return nullptr;
    }
    return (*arr)[static_cast<size_t>(frame % FRAMES)];
}

SDL_Texture* Renderer::itemTexture(const Item& it, int frame) {
    const int spritePx = std::clamp(tile, 16, 256);
    const uint64_t key = makeSpriteKey(CAT_ITEM, static_cast<uint8_t>(it.kind), it.spriteSeed);

    auto arr = spriteTex.get(key);
    if (!arr) {
        std::array<SDL_Texture*, FRAMES> tex{};
        tex.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            tex[static_cast<size_t>(f)] = textureFromSprite(generateItemSprite(it.kind, it.spriteSeed, f, voxelSpritesCached, spritePx));
        }

        const size_t bytes = (spriteEntryBytes != 0)
            ? spriteEntryBytes
            : (static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx) * sizeof(uint32_t) * static_cast<size_t>(FRAMES));

        spriteTex.put(key, tex, bytes);
        arr = spriteTex.get(key);
        if (!arr) return nullptr;
    }
    return (*arr)[static_cast<size_t>(frame % FRAMES)];
}

void Renderer::drawItemIcon(const Game& game, const Item& it, int x, int y, int px) {
    if (!renderer) return;

    SDL_BlendMode prevBlend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &prevBlend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Center within a typical UI row (18px) with a slight vertical inset.
    SDL_Rect dst{ x, y + 1, px, px };

    // Subtle dark backdrop so bright sprites remain readable on any panel theme.
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 55);
    SDL_RenderFillRect(renderer, &dst);

    Item visIt = it;
    if (isHallucinating(game)) {
        visIt.kind = hallucinatedItemKind(game, it);
    }

    applyIdentificationVisuals(game, visIt);

    SDL_Texture* tex = itemTexture(visIt, lastFrame);
    if (tex) {
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    // Stack count label (tiny) for stackable items.
    if (it.count > 1) {
        const Color white{240, 240, 240, 255};
        const int scale = 1;

        // 16px icons can only comfortably fit 2 digits; clamp larger stacks.
        const int shown = (it.count > 99) ? 99 : it.count;
        const std::string s = std::to_string(shown);

        const int charW = (5 + 1) * scale;
        const int textW = static_cast<int>(s.size()) * charW;
        const int textH = 7 * scale;

        const int tx = dst.x + dst.w - textW;
        const int ty = dst.y + dst.h - textH;

        SDL_Rect bg{ tx - 1, ty - 1, textW + 2, textH + 2 };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
        SDL_RenderFillRect(renderer, &bg);

        drawText5x7(renderer, tx, ty, scale, white, s);
    }

    SDL_SetRenderDrawBlendMode(renderer, prevBlend);
}

SDL_Texture* Renderer::projectileTexture(ProjectileKind k, int frame) {
    const int spritePx = std::clamp(tile, 16, 256);
    const uint64_t key = makeSpriteKey(CAT_PROJECTILE, static_cast<uint8_t>(k), 0u);

    auto arr = spriteTex.get(key);
    if (!arr) {
        std::array<SDL_Texture*, FRAMES> tex{};
        tex.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            tex[static_cast<size_t>(f)] = textureFromSprite(generateProjectileSprite(k, 0u, f, voxelSpritesCached, spritePx));
        }

        const size_t bytes = (spriteEntryBytes != 0)
            ? spriteEntryBytes
            : (static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx) * sizeof(uint32_t) * static_cast<size_t>(FRAMES));

        spriteTex.put(key, tex, bytes);
        arr = spriteTex.get(key);
        if (!arr) return nullptr;
    }
    return (*arr)[static_cast<size_t>(frame % FRAMES)];
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
        uiPanelTileTex[static_cast<size_t>(f)] = textureFromSprite(generateUIPanelTile(uiThemeCached, 0x51A11u, f, 16));
        uiOrnamentTex[static_cast<size_t>(f)]  = textureFromSprite(generateUIOrnamentTile(uiThemeCached, 0x0ABCDu, f, 16));
    }

    uiAssetsValid = true;
}

void Renderer::ensureIsoTerrainAssets() {
    if (isoTerrainAssetsValid) return;
    if (!renderer || !pixfmt) return;

    // Tile textures are generated in a clamped "sprite" resolution to keep VRAM reasonable
    // for very large tile sizes. This matches the logic in Renderer::init().
    const int spritePx = std::max(16, std::min(256, tile));
    const int tileVars = (spritePx >= 224) ? 8 : (spritePx >= 160) ? 10 : (spritePx >= 96) ? 14 : 18;

    // Defensive cleanup in case we ever re-generate (e.g., future runtime tile-size changes).
    for (auto& styleVec : floorThemeVarIso) {
        for (auto& arr : styleVec) {
            for (SDL_Texture*& t : arr) {
                if (t) SDL_DestroyTexture(t);
                t = nullptr;
            }
        }
        styleVec.clear();
    }
    for (auto& arr : chasmVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    chasmVarIso.clear();
    for (auto& arr : wallBlockVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    wallBlockVarIso.clear();

    for (auto& t : stairsUpOverlayIsoTex) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    for (auto& t : stairsDownOverlayIsoTex) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    for (auto& t : doorOpenOverlayIsoTex) { if (t) SDL_DestroyTexture(t); t = nullptr; }

    for (auto& anim : gasVarIso) {
        for (SDL_Texture*& t : anim) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    }
    for (auto& anim : fireVarIso) {
        for (SDL_Texture*& t : anim) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    }

    // --- Build isometric terrain ---
    // Floors/chasm are converted to true 2:1 diamond tiles via projectToIsometricDiamond().
    for (int st = 0; st < ROOM_STYLES; ++st) {
        auto& vec = floorThemeVarIso[static_cast<size_t>(st)];
        vec.resize(static_cast<size_t>(tileVars));
        for (int i = 0; i < tileVars; ++i) {
            for (int f = 0; f < FRAMES; ++f) {
                const uint32_t seed = hashCombine(0xC011Du, static_cast<uint32_t>(i * 1000 + f * 17));
                const SpritePixels sq = generateThemedFloorTile(seed, static_cast<uint8_t>(st), f, spritePx);
                const SpritePixels iso = projectToIsometricDiamond(sq, hashCombine(seed, static_cast<uint32_t>(st)), f, /*outline=*/true);
                vec[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
            }
        }
    }

    chasmVarIso.resize(static_cast<size_t>(tileVars));
    for (int i = 0; i < tileVars; ++i) {
        const uint32_t seed = hashCombine(0xC1A500u, static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            const SpritePixels sq = generateChasmTile(seed, f, spritePx);
            const SpritePixels iso = projectToIsometricDiamond(sq, seed, f, /*outline=*/true);
            chasmVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }

    // 2.5D walls are drawn as sprites (square textures) so they can extend above the ground plane.
    wallBlockVarIso.resize(static_cast<size_t>(tileVars));
    for (int i = 0; i < tileVars; ++i) {
        const uint32_t seed = hashCombine(0xAA110u ^ 0xB10Cu, static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            wallBlockVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] =
                textureFromSprite(generateIsometricWallBlockTile(seed, f, spritePx));
        }
    }

    // Ground-plane overlays that should sit on the diamond.
    for (int f = 0; f < FRAMES; ++f) {
        {
            const uint32_t seed = 0x515A1u;
            const SpritePixels sq = generateStairsTile(seed, true, f, spritePx);
            const SpritePixels iso = projectToIsometricDiamond(sq, seed, f, /*outline=*/false);
            stairsUpOverlayIsoTex[static_cast<size_t>(f)] = textureFromSprite(iso);
        }
        {
            const uint32_t seed = 0x515A2u;
            const SpritePixels sq = generateStairsTile(seed, false, f, spritePx);
            const SpritePixels iso = projectToIsometricDiamond(sq, seed, f, /*outline=*/false);
            stairsDownOverlayIsoTex[static_cast<size_t>(f)] = textureFromSprite(iso);
        }
        {
            const uint32_t seed = 0xD00Du;
            const SpritePixels sq = generateDoorTile(seed, true, f, spritePx);
            const SpritePixels iso = projectToIsometricDiamond(sq, seed, f, /*outline=*/false);
            doorOpenOverlayIsoTex[static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }

    // Isometric environmental overlays (gas/fire) so effects follow the diamond grid.
    for (int i = 0; i < GAS_VARS; ++i) {
        const uint32_t gSeed = hashCombine(0x6A5u, static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            const SpritePixels sq = generateConfusionGasTile(gSeed, f, spritePx);
            const SpritePixels iso = projectToIsometricDiamond(sq, gSeed, f, /*outline=*/false);
            gasVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }
    for (int i = 0; i < FIRE_VARS; ++i) {
        const uint32_t fSeed = hashCombine(0xF17Eu, static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            const SpritePixels sq = generateFireTile(fSeed, f, spritePx);
            const SpritePixels iso = projectToIsometricDiamond(sq, fSeed, f, /*outline=*/false);
            fireVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }

    isoTerrainAssetsValid = true;
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

// Map sprite helper: draws an optional soft shadow + crisp outline, then the sprite.
// This is a cheap way to dramatically improve sprite readability on noisy tiles.
static void drawSpriteWithShadowOutline(SDL_Renderer* r,
                                        SDL_Texture* tex,
                                        const SDL_Rect& dst,
                                        Color mod,
                                        Uint8 alpha,
                                        bool shadow,
                                        bool outline) {
    if (!r || !tex) return;

    // Scale the outline/shadow strength based on how bright the tile lighting is.
    const int lum = (static_cast<int>(mod.r) + static_cast<int>(mod.g) + static_cast<int>(mod.b)) / 3;
    const Uint8 outA = static_cast<Uint8>(std::clamp((lum * 170) / 255, 40, 190));
    const Uint8 shA  = static_cast<Uint8>(std::clamp((lum * 120) / 255, 28, 150));

    auto renderPass = [&](int dx, int dy, Uint8 rMod, Uint8 gMod, Uint8 bMod, Uint8 aMod) {
        SDL_Rect d = dst;
        d.x += dx;
        d.y += dy;
        SDL_SetTextureColorMod(tex, rMod, gMod, bMod);
        SDL_SetTextureAlphaMod(tex, aMod);
        SDL_RenderCopy(r, tex, nullptr, &d);
    };

    // Shadow first (offset down-right).
    if (shadow && shA > 0) {
        renderPass(2, 2, 0, 0, 0, shA);
    }

    // 4-neighbor outline (1px).
    if (outline && outA > 0) {
        renderPass(-1, 0, 0, 0, 0, outA);
        renderPass( 1, 0, 0, 0, 0, outA);
        renderPass( 0,-1, 0, 0, 0, outA);
        renderPass( 0, 1, 0, 0, 0, outA);
    }

    // Main sprite.
    SDL_SetTextureColorMod(tex, mod.r, mod.g, mod.b);
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_RenderCopy(r, tex, nullptr, &dst);

    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, 255);
}

// Simple post-process: a gentle vignette that improves focus and mood while
// keeping the HUD crisp (it's applied only to the map region).
static void drawVignette(SDL_Renderer* r, const SDL_Rect& area, int thickness, int maxAlpha) {
    if (!r) return;
    thickness = std::clamp(thickness, 6, 64);
    maxAlpha = std::clamp(maxAlpha, 0, 200);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < thickness; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(std::max(1, thickness - 1));
        // Quadratic falloff: lighter near center, heavier at edges.
        const int a = static_cast<int>(std::round(static_cast<float>(maxAlpha) * (t * t)));
        SDL_SetRenderDrawColor(r, 0, 0, 0, static_cast<uint8_t>(std::clamp(a, 0, 255)));

        SDL_Rect top{ area.x, area.y + i, area.w, 1 };
        SDL_Rect bot{ area.x, area.y + area.h - 1 - i, area.w, 1 };
        SDL_Rect left{ area.x + i, area.y, 1, area.h };
        SDL_Rect right{ area.x + area.w - 1 - i, area.y, 1, area.h };
        SDL_RenderFillRect(r, &top);
        SDL_RenderFillRect(r, &bot);
        SDL_RenderFillRect(r, &left);
        SDL_RenderFillRect(r, &right);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void Renderer::render(const Game& game) {
    if (!initialized) return;

    // Keep renderer-side view mode synced (main also calls setViewMode each frame).
    viewMode_ = game.viewMode();

    const uint32_t ticks = SDL_GetTicks();
    const int frame = static_cast<int>((ticks / 220u) % FRAMES);
    lastFrame = frame;

    // If the user toggled 3D voxel sprites, invalidate cached textures so they regenerate.
    const bool wantVoxelSprites = game.voxelSpritesEnabled();
    if (wantVoxelSprites != voxelSpritesCached) {
        // Entity/item/projectile textures are budget-cached in spriteTex.
        spriteTex.clear();
        spriteTex.resetStats();
        voxelSpritesCached = wantVoxelSprites;
    }

    // Background clear
    SDL_SetRenderDrawColor(renderer, 8, 8, 12, 255);
    SDL_RenderClear(renderer);

    const Dungeon& d = game.dungeon();

    // Update camera based on player/cursor and current viewport.
    updateCamera(game);

    // Clip all map-space drawing to the map region so that screen shake / FX never
    // bleed into the HUD area.
    const SDL_Rect mapClip{ 0, 0, viewTilesW * tile, viewTilesH * tile };
    SDL_RenderSetClipRect(renderer, &mapClip);

    // Transient screen shake based on active explosions.
    // (Small and deterministic to avoid nausea and keep capture/replay stable.)
    mapOffX = 0;
    mapOffY = 0;
    {
        int shake = 0;
        for (const auto& ex : game.fxExplosions()) {
            if (ex.delay > 0.0f) continue;
            const float dur = std::max(0.001f, ex.duration);
            const float t01 = std::clamp(ex.timer / dur, 0.0f, 1.0f);
            // Strong at the start, quickly decays.
            const int s = static_cast<int>(std::round((1.0f - t01) * 5.0f));
            if (s > shake) shake = s;
        }

        shake = std::clamp(shake, 0, 6);
        if (shake > 0) {
            const uint32_t seed = hashCombine(static_cast<uint32_t>(ticks), static_cast<uint32_t>(game.turns()));
            const uint32_t rx = hash32(seed ^ 0xA53u);
            const uint32_t ry = hash32(seed ^ 0xC11u);
            mapOffX = static_cast<int>(rx % static_cast<uint32_t>(shake * 2 + 1)) - shake;
            mapOffY = static_cast<int>(ry % static_cast<uint32_t>(shake * 2 + 1)) - shake;
        }
    }

    const bool isoView = (viewMode_ == ViewMode::Isometric);

    // Build isometric-diamond terrain textures lazily so top-down mode doesn't pay
    // the VRAM + CPU cost unless it is actually used.
    if (isoView) {
        ensureIsoTerrainAssets();
    }

    auto tileDst = [&](int x, int y) -> SDL_Rect {
        return mapTileDst(x, y);
    };

    auto spriteDst = [&](int x, int y) -> SDL_Rect {
        return mapSpriteDst(x, y);
    };

    // Room type cache (used for themed decals / minimap)
    auto rebuildRoomTypeCache = [&]() {
        roomCacheDungeon = &d;
        roomCacheDepth = game.depth();
        roomCacheW = d.width;
        roomCacheH = d.height;
        roomCacheRooms = d.rooms.size();

        roomTypeCache.assign(static_cast<size_t>(d.width * d.height), static_cast<uint8_t>(RoomType::Normal));
        for (const Room& r : d.rooms) {
            for (int yy = r.y; yy < r.y2(); ++yy) {
                for (int xx = r.x; xx < r.x2(); ++xx) {
                    if (!d.inBounds(xx, yy)) continue;
                    roomTypeCache[static_cast<size_t>(yy * d.width + xx)] = static_cast<uint8_t>(r.type);
                }
            }
        }
    };

    if (roomCacheDungeon != &d || roomCacheDepth != game.depth() ||
        roomCacheW != d.width || roomCacheH != d.height || roomCacheRooms != d.rooms.size() ||
        roomTypeCache.size() != static_cast<size_t>(d.width * d.height)) {
        rebuildRoomTypeCache();
    }

    auto lightMod = [&](int x, int y) -> uint8_t {
        if (!game.darknessActive()) return 255;
        const uint8_t L = game.tileLightLevel(x, y); // 0..255
        constexpr int kMin = 40;
        int mod = kMin + (static_cast<int>(L) * (255 - kMin)) / 255;
        if (mod < kMin) mod = kMin;
        if (mod > 255) mod = 255;
        return static_cast<uint8_t>(mod);
    };

    // Subtle per-depth color grading so each floor feels distinct.
    auto depthTint = [&]() -> Color {
        auto lerpU8 = [](uint8_t a, uint8_t b, float t) -> uint8_t {
            t = std::clamp(t, 0.0f, 1.0f);
            const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
            int iv = static_cast<int>(v + 0.5f);
            if (iv < 0) iv = 0;
            if (iv > 255) iv = 255;
            return static_cast<uint8_t>(iv);
        };

        const int depth = std::max(1, game.depth());
        const int maxDepth = std::max(1, game.dungeonMaxDepth());
        const float t = (maxDepth > 1) ? (static_cast<float>(depth - 1) / static_cast<float>(maxDepth - 1)) : 0.0f;

        // Warm torchlit stone up top -> colder, bluer depths below.
        const Color warm{255, 246, 232, 255};
        const Color deep{222, 236, 255, 255};

        return { lerpU8(warm.r, deep.r, t),
                 lerpU8(warm.g, deep.g, t),
                 lerpU8(warm.b, deep.b, t),
                 255 };
    };




    // Draw map tiles
    const Color tint = depthTint();

    // Gather dynamic torch light sources so we can add subtle flame flicker in the renderer.
    // (The lightmap itself updates on turns; flicker is a purely visual, per-frame modulation.)
    struct TorchSrc {
        Vec2i pos;
        int radius = 7;
        float strength = 1.0f;
    };

    std::vector<TorchSrc> torches;
    if (game.darknessActive()) {
        // Player-held lit torch.
        bool playerTorch = false;
        for (const Item& it : game.inventory()) {
            if (it.kind == ItemKind::TorchLit && it.charges > 0) { playerTorch = true; break; }
        }
        if (playerTorch) {
            torches.push_back(TorchSrc{ game.player().pos, 9, 1.0f });
        }

        // Ground torches.
        for (const auto& gi : game.groundItems()) {
            if (gi.item.kind == ItemKind::TorchLit && gi.item.charges > 0) {
                torches.push_back(TorchSrc{ gi.pos, 7, 0.85f });
            }
        }
    }

    auto torchFlicker = [&](int x, int y) -> float {
        if (torches.empty()) return 1.0f;

        float best = 0.0f;
        TorchSrc bestT{};
        for (const TorchSrc& t : torches) {
            const int dx = x - t.pos.x;
            const int dy = y - t.pos.y;
            const int d2 = dx * dx + dy * dy;
            const int r2 = t.radius * t.radius;
            if (d2 > r2) continue;
            const float dist = std::sqrt(static_cast<float>(d2));
            const float att = std::max(0.0f, 1.0f - dist / static_cast<float>(t.radius)) * t.strength;
            if (att > best) { best = att; bestT = t; }
        }
        if (best <= 0.0f) return 1.0f;

        // Smooth-ish multi-frequency flicker, seeded by the strongest torch position.
        const float time = static_cast<float>(ticks) * 0.014f;
        const float seed = static_cast<float>(bestT.pos.x * 17 + bestT.pos.y * 31);
        const float w = (std::sin(time + seed) * 0.6f + std::sin(time * 2.13f + seed * 0.7f) * 0.4f);
        const float f = 1.0f + best * 0.05f * w; // very subtle (about +/-5% max near the torch)
        return std::clamp(f, 0.90f, 1.10f);
    };

    // Helper: compute per-tile texture color modulation (RGB) from lighting + depth tint.
    auto tileColorMod = [&](int x, int y, bool visible) -> Color {
        if (!visible) {
            const uint8_t base = game.darknessActive() ? 30u : 80u;
            return {
                static_cast<uint8_t>((static_cast<int>(base) * tint.r) / 255),
                static_cast<uint8_t>((static_cast<int>(base) * tint.g) / 255),
                static_cast<uint8_t>((static_cast<int>(base) * tint.b) / 255),
                255
            };
        }

        if (!game.darknessActive()) {
            return { tint.r, tint.g, tint.b, 255 };
        }

        const uint8_t m = lightMod(x, y);
        Color lc = game.tileLightColor(x, y);

        // If the light color is (0,0,0) but the tile is still "visible" due to the short dark-vision radius,
        // fall back to a grayscale minimum brightness so the player can still read nearby terrain.
        if (lc.r == 0 && lc.g == 0 && lc.b == 0) {
            lc = { m, m, m, 255 };
        } else {
            const int minChan = std::max<int>(0, static_cast<int>(m) / 4);
            lc.r = static_cast<uint8_t>(std::max<int>(lc.r, minChan));
            lc.g = static_cast<uint8_t>(std::max<int>(lc.g, minChan));
            lc.b = static_cast<uint8_t>(std::max<int>(lc.b, minChan));
            lc.a = 255;
        }

        Color out{
            static_cast<uint8_t>((static_cast<int>(lc.r) * tint.r) / 255),
            static_cast<uint8_t>((static_cast<int>(lc.g) * tint.g) / 255),
            static_cast<uint8_t>((static_cast<int>(lc.b) * tint.b) / 255),
            255
        };

        // Flame flicker: only modulate colors near active torch sources.
        const float f = torchFlicker(x, y);
        if (f != 1.0f) {
            out.r = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.r) * f)), 0, 255));
            out.g = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.g) * f)), 0, 255));
            out.b = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.b) * f)), 0, 255));
        }
        return out;
    };

    auto styleForRoomType = [&](uint8_t rt) -> int {
        switch (static_cast<RoomType>(rt)) {
            case RoomType::Treasure: return 1;
            case RoomType::Lair:     return 2;
            case RoomType::Shrine:   return 3;
            case RoomType::Secret:   return 4;
            case RoomType::Vault:    return 5;
            case RoomType::Shop:     return 6;
            case RoomType::Armory:   return 5; // reuse Vault style
            case RoomType::Library:  return 3; // reuse Shrine style
            case RoomType::Laboratory: return 4; // reuse Secret style
            case RoomType::Normal:
            default: return 0;
        }
    };

    const std::array<uint8_t, DECAL_STYLES> decalChance = { 34, 64, 56, 72, 58, 52, 54 };

    // Returns the themed floor style for a tile coordinate, even when that tile is a
    // door/stairs/pillar. We primarily query the cached room type, and fall back to
    // adjacent tiles so door thresholds inherit the room style.
    auto floorStyleAt = [&](int tx, int ty) -> int {
        if (!d.inBounds(tx, ty)) return 0;
        const size_t ii = static_cast<size_t>(ty * d.width + tx);
        if (ii < roomTypeCache.size()) {
            const int s = styleForRoomType(roomTypeCache[ii]);
            if (s != 0) return s;
        }

        // Neighbor bias (useful for doors placed on room boundaries).
        const int dx[4] = { 1, -1, 0, 0 };
        const int dy[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; ++k) {
            const int nx = tx + dx[k];
            const int ny = ty + dy[k];
            if (!d.inBounds(nx, ny)) continue;
            const size_t jj = static_cast<size_t>(ny * d.width + nx);
            if (jj >= roomTypeCache.size()) continue;
            const int s2 = styleForRoomType(roomTypeCache[jj]);
            if (s2 != 0) return s2;
        }
        return 0;
    };

    auto isWallMass = [&](TileType tt) -> bool {
        switch (tt) {
            case TileType::Wall:
            case TileType::DoorClosed:
            case TileType::DoorLocked:
            case TileType::DoorSecret:
            case TileType::Pillar:
                return true;
            default:
                return false;
        }
    };

    auto wallOpenMaskAt = [&](int tx, int ty) -> uint8_t {
        uint8_t m = 0;
        if (!d.inBounds(tx, ty - 1) || !isWallMass(d.at(tx, ty - 1).type)) m |= 0x01u; // N
        if (!d.inBounds(tx + 1, ty) || !isWallMass(d.at(tx + 1, ty).type)) m |= 0x02u; // E
        if (!d.inBounds(tx, ty + 1) || !isWallMass(d.at(tx, ty + 1).type)) m |= 0x04u; // S
        if (!d.inBounds(tx - 1, ty) || !isWallMass(d.at(tx - 1, ty).type)) m |= 0x08u; // W
        return m;
    };

    auto chasmOpenMaskAt = [&](int tx, int ty) -> uint8_t {
        uint8_t m = 0;
        auto isCh = [&](int xx, int yy) -> bool {
            return d.inBounds(xx, yy) && d.at(xx, yy).type == TileType::Chasm;
        };
        if (!isCh(tx, ty - 1)) m |= 0x01u; // N
        if (!isCh(tx + 1, ty)) m |= 0x02u; // E
        if (!isCh(tx, ty + 1)) m |= 0x04u; // S
        if (!isCh(tx - 1, ty)) m |= 0x08u; // W
        return m;
    };



    auto drawMapTile = [&](int x, int y) {
        if (!mapTileInView(x, y)) return;
        const Tile& t = d.at(x, y);
        SDL_Rect dst = tileDst(x, y);

        if (!t.explored) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, &dst);
            return;
        }

        // Isometric mode: draw a diamond-projected ground tile, then draw any tall
        // blocking terrain (walls/doors/pillars/boulders) as sprite-sized overlays.
        // This keeps the ground plane clean (no square squashing artifacts) and gives
        // a more convincing 2.5D feel.
        if (isoView) {
            // Ground base: chasms keep their own material; everything else gets a themed
            // floor so wall blocks sit on something consistent.
            TileType base = (t.type == TileType::Chasm) ? TileType::Chasm : TileType::Floor;
            const int style = (base == TileType::Floor) ? floorStyleAt(x, y) : 0;

            SDL_Texture* btex = tileTexture(base, x, y, game.depth(), frame, style);
            const Color mod = tileColorMod(x, y, t.visible);
            const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 115 : 175);

            if (btex) {
                SDL_SetTextureColorMod(btex, mod.r, mod.g, mod.b);
                SDL_SetTextureAlphaMod(btex, a);
                SDL_RenderCopy(renderer, btex, nullptr, &dst);
                SDL_SetTextureColorMod(btex, 255, 255, 255);
                SDL_SetTextureAlphaMod(btex, 255);
            }

            // Ground-plane overlays that should stay on the diamond tile.
            if (t.type == TileType::StairsUp) {
                SDL_Texture* otex = stairsUpOverlayIsoTex[static_cast<size_t>(frame % FRAMES)];
                if (!otex) otex = stairsUpOverlayTex[static_cast<size_t>(frame % FRAMES)];
                if (otex) {
                    SDL_SetTextureColorMod(otex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(otex, a);
                    SDL_RenderCopy(renderer, otex, nullptr, &dst);
                    SDL_SetTextureColorMod(otex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(otex, 255);
                }
            } else if (t.type == TileType::StairsDown) {
                SDL_Texture* otex = stairsDownOverlayIsoTex[static_cast<size_t>(frame % FRAMES)];
                if (!otex) otex = stairsDownOverlayTex[static_cast<size_t>(frame % FRAMES)];
                if (otex) {
                    SDL_SetTextureColorMod(otex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(otex, a);
                    SDL_RenderCopy(renderer, otex, nullptr, &dst);
                    SDL_SetTextureColorMod(otex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(otex, 255);
                }
            } else if (t.type == TileType::DoorOpen) {
                SDL_Texture* otex = doorOpenOverlayIsoTex[static_cast<size_t>(frame % FRAMES)];
                if (!otex) otex = doorOpenOverlayTex[static_cast<size_t>(frame % FRAMES)];
                if (otex) {
                    SDL_SetTextureColorMod(otex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(otex, a);
                    SDL_RenderCopy(renderer, otex, nullptr, &dst);
                    SDL_SetTextureColorMod(otex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(otex, 255);
                }
            }

            // Tall blockers & objects.
            auto drawTall = [&](SDL_Texture* tex, bool outline) {
                if (!tex) return;
                SDL_Rect sdst = spriteDst(x, y);
                // In explored-but-not-visible memory view we draw a bit darker so the
                // player can still navigate without everything looking "lit".
                const Uint8 aa = t.visible ? 255 : (game.darknessActive() ? 150 : 190);
                drawSpriteWithShadowOutline(renderer, tex, sdst, mod, aa, /*shadow=*/false, outline);
            };

            if (t.type == TileType::Wall || t.type == TileType::DoorSecret) {
                if (!wallBlockVarIso.empty()) {
                    const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xAA110u ^ 0xB10Cu;
                    const size_t v = static_cast<size_t>(hash32(h) % static_cast<uint32_t>(wallBlockVarIso.size()));
                    SDL_Texture* wtex = wallBlockVarIso[v][static_cast<size_t>(frame % FRAMES)];
                    // Wall blocks are already outlined in their procedural art.
                    drawTall(wtex, /*outline=*/false);
                }
            } else if (t.type == TileType::DoorClosed) {
                drawTall(doorClosedOverlayTex[static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
            } else if (t.type == TileType::DoorLocked) {
                drawTall(doorLockedOverlayTex[static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
            } else if (t.type == TileType::Pillar) {
                if (!pillarOverlayVar.empty()) {
                    const uint32_t hh = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0x9111A0u;
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(pillarOverlayVar.size()));
                    drawTall(pillarOverlayVar[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            } else if (t.type == TileType::Boulder) {
                if (!boulderOverlayVar.empty()) {
                    const uint32_t hh = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xB011D3u;
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(boulderOverlayVar.size()));
                    drawTall(boulderOverlayVar[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            }

            return;
        }

        // Doors/stairs/pillars are rendered as transparent overlays layered on top of the
        // underlying floor so they inherit themed room flooring.
        const bool isOverlay =
            (t.type == TileType::Pillar) ||
            (t.type == TileType::Boulder) ||
            (t.type == TileType::StairsUp) || (t.type == TileType::StairsDown) ||
            (t.type == TileType::DoorClosed) || (t.type == TileType::DoorLocked) || (t.type == TileType::DoorOpen);

        TileType baseType = t.type;
        if (isOverlay) baseType = TileType::Floor;

        int floorStyle = (baseType == TileType::Floor) ? floorStyleAt(x, y) : 0;

        SDL_Texture* tex = tileTexture(baseType, x, y, game.depth(), frame, floorStyle);
        if (!tex) return;

        const Color mod = tileColorMod(x, y, t.visible);
        SDL_SetTextureColorMod(tex, mod.r, mod.g, mod.b);
        SDL_SetTextureAlphaMod(tex, 255);

        SDL_RenderCopy(renderer, tex, nullptr, &dst);

        SDL_SetTextureColorMod(tex, 255, 255, 255);
        SDL_SetTextureAlphaMod(tex, 255);

        // Themed floor decals add subtle detail and make special rooms stand out.
        // Applied to any tile whose *base* is floor (including overlay tiles).
        if (baseType == TileType::Floor && !floorDecalVar.empty()) {
            const int style = floorStyle;

            const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                           static_cast<uint32_t>(y)) ^ 0xDECA151u;
            const uint32_t r = hash32(h);
            const uint8_t roll = static_cast<uint8_t>(r & 0xFFu);

            if (roll < decalChance[static_cast<size_t>(style)]) {
                const int var = static_cast<int>((r >> 8) % static_cast<uint32_t>(decalsPerStyleUsed));
                const size_t di = static_cast<size_t>(style * decalsPerStyleUsed + var);

                if (di < floorDecalVar.size()) {
                    SDL_Texture* dtex = floorDecalVar[di][static_cast<size_t>(frame % FRAMES)];
                    if (dtex) {
                        const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 120 : 160);
                        SDL_SetTextureColorMod(dtex, mod.r, mod.g, mod.b);
                        SDL_SetTextureAlphaMod(dtex, a);
                        SDL_RenderCopy(renderer, dtex, nullptr, &dst);
                        SDL_SetTextureColorMod(dtex, 255, 255, 255);
                        SDL_SetTextureAlphaMod(dtex, 255);
                    }
                }
            }
        }

        // Occasional wall stains/cracks (very low frequency; helps break large flat walls).
        if ((t.type == TileType::Wall || t.type == TileType::DoorSecret) && !wallDecalVar.empty()) {
            const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                           static_cast<uint32_t>(y)) ^ 0xBADC0DEu;
            const uint32_t r = hash32(h);
            if (static_cast<uint8_t>(r & 0xFFu) < 18u) {
                int style = 0;
                // If a neighboring floor belongs to a special room, bias the wall decal style.
                const int dx[4] = {1,-1,0,0};
                const int dy[4] = {0,0,1,-1};
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx[k];
                    const int ny = y + dy[k];
                    if (!d.inBounds(nx, ny)) continue;
                    if (d.at(nx, ny).type != TileType::Floor) continue;
                    const size_t jj = static_cast<size_t>(ny * d.width + nx);
                    if (jj >= roomTypeCache.size()) continue;
                    const int s2 = styleForRoomType(roomTypeCache[jj]);
                    if (s2 != 0) { style = s2; break; }
                }

                const int var = static_cast<int>((r >> 8) % static_cast<uint32_t>(decalsPerStyleUsed));
                const size_t di = static_cast<size_t>(style * decalsPerStyleUsed + var);
                if (di < wallDecalVar.size()) {
                    SDL_Texture* dtex = wallDecalVar[di][static_cast<size_t>(frame % FRAMES)];
                    if (dtex) {
                        const Uint8 a = t.visible ? 220 : 120;
                        SDL_SetTextureColorMod(dtex, mod.r, mod.g, mod.b);
                        SDL_SetTextureAlphaMod(dtex, a);
                        SDL_RenderCopy(renderer, dtex, nullptr, &dst);
                        SDL_SetTextureColorMod(dtex, 255, 255, 255);
                        SDL_SetTextureAlphaMod(dtex, 255);
                    }
                }
            }
        }

        // Autotile edge/rim overlays add crisp silhouette and depth for large wall/chasm fields.
        if ((t.type == TileType::Wall || t.type == TileType::DoorSecret)) {
            const uint8_t mask = wallOpenMaskAt(x, y);
            if (mask != 0u) {
                const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                               static_cast<uint32_t>(y)) ^ 0xED6E7u ^ static_cast<uint32_t>(mask);
                const uint32_t r = hash32(h);
                const size_t v = static_cast<size_t>(r % static_cast<uint32_t>(autoVarsUsed));

                SDL_Texture* etex = wallEdgeVar[static_cast<size_t>(mask)][v][static_cast<size_t>(frame % FRAMES)];
                if (etex) {
                    const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 150 : 190);
                    SDL_SetTextureColorMod(etex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(etex, a);
                    SDL_RenderCopy(renderer, etex, nullptr, &dst);
                    SDL_SetTextureColorMod(etex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(etex, 255);
                }
            }
        } else if (t.type == TileType::Chasm) {
            const uint8_t mask = chasmOpenMaskAt(x, y);
            if (mask != 0u) {
                const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                               static_cast<uint32_t>(y)) ^ 0xC11A5u ^ static_cast<uint32_t>(mask);
                const uint32_t r = hash32(h);
                const size_t v = static_cast<size_t>(r % static_cast<uint32_t>(autoVarsUsed));

                SDL_Texture* rtex = chasmRimVar[static_cast<size_t>(mask)][v][static_cast<size_t>(frame % FRAMES)];
                if (rtex) {
                    const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 135 : 175);
                    SDL_SetTextureColorMod(rtex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(rtex, a);
                    SDL_RenderCopy(renderer, rtex, nullptr, &dst);
                    SDL_SetTextureColorMod(rtex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(rtex, 255);
                }
            }
        }

        // Render overlays on top of floor base.
        if (isOverlay) {
            SDL_Texture* otex = nullptr;
            switch (t.type) {
                case TileType::Pillar: {
                    if (!pillarOverlayVar.empty()) {
                        const uint32_t hh = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                       static_cast<uint32_t>(y)) ^ 0x9111A0u;
                        const uint32_t rr = hash32(hh);
                        const size_t idx = static_cast<size_t>(rr % static_cast<uint32_t>(pillarOverlayVar.size()));
                        otex = pillarOverlayVar[idx][static_cast<size_t>(frame % FRAMES)];
                    }
                    break;
                }
                case TileType::Boulder: {
                    if (!boulderOverlayVar.empty()) {
                        const uint32_t hh = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                       static_cast<uint32_t>(y)) ^ 0xB011D3u;
                        const uint32_t rr = hash32(hh);
                        const size_t idx = static_cast<size_t>(rr % static_cast<uint32_t>(boulderOverlayVar.size()));
                        otex = boulderOverlayVar[idx][static_cast<size_t>(frame % FRAMES)];
                    }
                    break;
                }
                case TileType::StairsUp:
                    otex = stairsUpOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::StairsDown:
                    otex = stairsDownOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::DoorClosed:
                    otex = doorClosedOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::DoorLocked:
                    otex = doorLockedOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::DoorOpen:
                    otex = doorOpenOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                default:
                    break;
            }

            if (otex) {
                SDL_SetTextureColorMod(otex, mod.r, mod.g, mod.b);
                SDL_SetTextureAlphaMod(otex, 255);
                SDL_RenderCopy(renderer, otex, nullptr, &dst);
                SDL_SetTextureColorMod(otex, 255, 255, 255);
                SDL_SetTextureAlphaMod(otex, 255);
            }
        }


    };

    if (isoView) {
        // Painter's order for isometric tiles: back-to-front by diagonal (x+y).
        const int maxSum = (d.width - 1) + (d.height - 1);
        for (int s = 0; s <= maxSum; ++s) {
            for (int y = 0; y < d.height; ++y) {
                const int x = s - y;
                if (x < 0 || x >= d.width) continue;
                drawMapTile(x, y);
            }
        }
    } else {
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                drawMapTile(x, y);
            }
        }
    }

    // Ambient-occlusion + directional shadows are tuned for the top-down tileset.
    // For isometric mode we rely on the diamond-projected ground tiles + taller
    // wall blocks for depth/readability.
    if (!isoView) {
// Ambient-occlusion style edge shading (walls/pillars/chasm) makes rooms and corridors pop.
        {
        auto isOccluder = [&](TileType tt) -> bool {
            switch (tt) {
                case TileType::Wall:
                case TileType::DoorClosed:
                case TileType::DoorLocked:
                case TileType::DoorSecret:
                case TileType::Pillar:
                case TileType::Boulder:
                case TileType::Chasm:
                    return true;
                default:
                    return false;
            }
        };

        const int thick = std::max(1, tile / 8);

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.explored) continue;
                if (isOccluder(t.type)) continue;

                // Fade AO with visibility/light.
                const uint8_t lm = t.visible ? lightMod(x, y) : (game.darknessActive() ? 120u : 170u);
                int baseA = 38;
                baseA = (baseA * static_cast<int>(lm)) / 255;
                if (!t.visible) baseA = std::min(baseA, 26);

                const auto nType = (y > 0) ? d.at(x, y - 1).type : TileType::Wall;
                const auto sType = (y + 1 < d.height) ? d.at(x, y + 1).type : TileType::Wall;
                const auto wType = (x > 0) ? d.at(x - 1, y).type : TileType::Wall;
                const auto eType = (x + 1 < d.width) ? d.at(x + 1, y).type : TileType::Wall;

                const bool nOcc = isOccluder(nType);
                const bool sOcc = isOccluder(sType);
                const bool wOcc = isOccluder(wType);
                const bool eOcc = isOccluder(eType);

                if (!nOcc && !sOcc && !wOcc && !eOcc) continue;

                SDL_Rect dst = tileDst(x, y);

                auto drawEdge = [&](const SDL_Rect& r, int a, bool chasmEdge) {
                    if (a <= 0) return;
                    if (a > 255) a = 255;

                    // A subtle blue rim for chasms reads as "danger" without being loud.
                    if (chasmEdge) {
                        const int ga = std::max(8, a / 2);
                        SDL_SetRenderDrawColor(renderer, 40, 80, 160, static_cast<Uint8>(ga));
                        SDL_RenderFillRect(renderer, &r);
                    }

                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<Uint8>(a));
                    SDL_RenderFillRect(renderer, &r);
                };

                const int aTop = static_cast<int>(baseA * 0.82f);
                const int aLeft = static_cast<int>(baseA * 0.82f);
                const int aBot = std::min(255, baseA + 10);
                const int aRight = std::min(255, baseA + 10);

                if (nOcc) drawEdge(SDL_Rect{ dst.x, dst.y, dst.w, thick }, aTop, nType == TileType::Chasm);
                if (wOcc) drawEdge(SDL_Rect{ dst.x, dst.y, thick, dst.h }, aLeft, wType == TileType::Chasm);
                if (sOcc) drawEdge(SDL_Rect{ dst.x, dst.y + dst.h - thick, dst.w, thick }, aBot, sType == TileType::Chasm);
                if (eOcc) drawEdge(SDL_Rect{ dst.x + dst.w - thick, dst.y, thick, dst.h }, aRight, eType == TileType::Chasm);

                // Darken corners a touch so diagonal contacts don't feel "open".
                if (nOcc && wOcc) drawEdge(SDL_Rect{ dst.x, dst.y, thick, thick }, baseA, (nType == TileType::Chasm) || (wType == TileType::Chasm));
                if (nOcc && eOcc) drawEdge(SDL_Rect{ dst.x + dst.w - thick, dst.y, thick, thick }, baseA, (nType == TileType::Chasm) || (eType == TileType::Chasm));
                if (sOcc && wOcc) drawEdge(SDL_Rect{ dst.x, dst.y + dst.h - thick, thick, thick }, baseA + 6, (sType == TileType::Chasm) || (wType == TileType::Chasm));
                if (sOcc && eOcc) drawEdge(SDL_Rect{ dst.x + dst.w - thick, dst.y + dst.h - thick, thick, thick }, baseA + 6, (sType == TileType::Chasm) || (eType == TileType::Chasm));
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }


    // Directional occluder shadows: adds a subtle sense of "height" for walls/pillars/closed doors
    // without requiring any new tile art. This pass is intentionally very light.
    {
        auto isTall = [&](TileType tt) -> bool {
            switch (tt) {
                case TileType::Wall:
                case TileType::Pillar:
                case TileType::Boulder:
                case TileType::DoorClosed:
                case TileType::DoorLocked:
                case TileType::DoorSecret:
                    return true;
                default:
                    return false;
            }
        };
        auto receives = [&](TileType tt) -> bool {
            switch (tt) {
                case TileType::Floor:
                case TileType::DoorOpen:
                case TileType::StairsUp:
                case TileType::StairsDown:
                case TileType::Chasm:
                    return true;
                default:
                    return false;
            }
        };

        const int grad = std::max(2, tile / 4);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        auto castShadow = [&](int tx, int ty, int baseA) {
            if (!d.inBounds(tx, ty)) return;
            const Tile& rt = d.at(tx, ty);
            if (!rt.explored) return;
            if (!receives(rt.type)) return;

            // Fade the shadow in darkness / memory.
            const uint8_t lm = rt.visible ? lightMod(tx, ty) : (game.darknessActive() ? 110u : 160u);
            int a = (baseA * static_cast<int>(lm)) / 255;
            a = std::clamp(a, 0, 110);
            if (a <= 0) return;

            SDL_Rect base = tileDst(tx, ty);
            // Draw a top-to-bottom gradient strip at the top of the receiving tile.
            for (int i = 0; i < grad; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(std::max(1, grad - 1));
                const int ai = static_cast<int>(std::round(static_cast<float>(a) * (1.0f - t)));
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<uint8_t>(std::clamp(ai, 0, 255)));
                SDL_Rect r{ base.x, base.y + i, base.w, 1 };
                SDL_RenderFillRect(renderer, &r);
            }
        };

        // Assume a gentle ambient light direction from top-left => shadows fall down/right.
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.explored) continue;
                if (!isTall(t.type)) continue;

                // Don't over-darken in the explored-but-not-visible memory view.
                const int baseA = t.visible ? 54 : 34;

                castShadow(x, y + 1, baseA);
                // A slightly weaker diagonal shadow helps break the grid feel.
                castShadow(x + 1, y + 1, baseA / 2);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
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

            SDL_Rect base = tileDst(p.x, p.y);
            SDL_Rect r{ base.x + tile / 3, base.y + tile / 3, tile / 3, tile / 3 };
            SDL_RenderFillRect(renderer, &r);
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Draw items (visible only)
    if (isoView) {
        // Sort by isometric draw order so items layer nicely.
        std::vector<const GroundItem*> draw;
        draw.reserve(game.groundItems().size());
        for (const auto& gi : game.groundItems()) {
            if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
            const Tile& t = d.at(gi.pos.x, gi.pos.y);
            if (!t.visible) continue;
            draw.push_back(&gi);
        }

        std::sort(draw.begin(), draw.end(), [](const GroundItem* a, const GroundItem* b) {
            const int sa = a->pos.x + a->pos.y;
            const int sb = b->pos.x + b->pos.y;
            if (sa != sb) return sa < sb;
            if (a->pos.y != b->pos.y) return a->pos.y < b->pos.y;
            return a->pos.x < b->pos.x;
        });

        for (const GroundItem* gip : draw) {
            const GroundItem& gi = *gip;

            Item visIt = gi.item;
            if (isHallucinating(game)) {
                visIt.kind = hallucinatedItemKind(game, gi.item);
            }

            applyIdentificationVisuals(game, visIt);

            SDL_Texture* tex = itemTexture(visIt, frame);
            if (!tex) continue;

            SDL_Rect dst = spriteDst(gi.pos.x, gi.pos.y);
            const Color mod = tileColorMod(gi.pos.x, gi.pos.y, /*visible*/true);
            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/false, /*outline*/true);
        }
    } else {
        for (const auto& gi : game.groundItems()) {
            if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
            const Tile& t = d.at(gi.pos.x, gi.pos.y);
            if (!t.visible) continue;

            Item visIt = gi.item;
            if (isHallucinating(game)) {
                visIt.kind = hallucinatedItemKind(game, gi.item);
            }

            applyIdentificationVisuals(game, visIt);

            SDL_Texture* tex = itemTexture(visIt, frame);
            if (!tex) continue;

            SDL_Rect dst = spriteDst(gi.pos.x, gi.pos.y);
            const Color mod = tileColorMod(gi.pos.x, gi.pos.y, /*visible*/true);
            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/false, /*outline*/true);
        }
    }




    // Draw confusion gas (visible tiles only). This is a persistent, tile-based field
    // spawned by Confusion Gas traps.
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const bool haveGasTex = isoView ? (gasVarIso[0][0] != nullptr) : (gasVar[0][0] != nullptr);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;

                const uint8_t g = game.confusionGasAt(x, y);
                if (g == 0u) continue;

                const uint8_t m = lightMod(x, y);

                // Scale intensity by light; keep a minimum so it reads even in deep shadow.
                int a = 70 + static_cast<int>(g) * 12;
                a = (a * static_cast<int>(m)) / 255;
                a = std::max(24, std::min(230, a));

                // Slight shimmer so the cloud feels alive (deterministic per tile/frame).
                a = std::max(24, std::min(240, a + (static_cast<int>((frame + x * 3 + y * 7) % 9) - 4)));

                SDL_Rect r = tileDst(x, y);

                if (haveGasTex) {
                    const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0x6A5u;
                    const size_t vi = static_cast<size_t>(hash32(h) % static_cast<uint32_t>(GAS_VARS));
                    const size_t fi = static_cast<size_t>((frame + ((x + y) & 1)) % FRAMES);

                    SDL_Texture* gtex = (isoView && gasVarIso[0][0] != nullptr) ? gasVarIso[vi][fi] : gasVar[vi][fi];
                    if (gtex) {
                        // Multiply a "signature" purple by the tile lighting/tint so it feels embedded in the world.
                        const Color lmod = tileColorMod(x, y, /*visible=*/true);
                        const Color base{200, 120, 255, 255};

                        const uint8_t mr = static_cast<uint8_t>((static_cast<int>(base.r) * lmod.r) / 255);
                        const uint8_t mg = static_cast<uint8_t>((static_cast<int>(base.g) * lmod.g) / 255);
                        const uint8_t mb = static_cast<uint8_t>((static_cast<int>(base.b) * lmod.b) / 255);

                        SDL_SetTextureColorMod(gtex, mr, mg, mb);
                        SDL_SetTextureAlphaMod(gtex, static_cast<uint8_t>(a));
                        SDL_RenderCopy(renderer, gtex, nullptr, &r);
                        SDL_SetTextureColorMod(gtex, 255, 255, 255);
                        SDL_SetTextureAlphaMod(gtex, 255);
                        continue;
                    }
                }

                // Fallback: simple tinted quad (should rarely be used).
                SDL_SetRenderDrawColor(renderer, 190, 90, 255, static_cast<uint8_t>(a));
                SDL_RenderFillRect(renderer, &r);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }


    // Draw poison gas (visible tiles only). This is a persistent, tile-based hazard
    // spawned by Poison Gas traps.
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const bool haveGasTex = (gasVar[0][0] != nullptr);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;

                const uint8_t g = game.poisonGasAt(x, y);
                if (g == 0u) continue;

                const uint8_t m = lightMod(x, y);

                // Scale intensity by light; keep a minimum so it reads even in deep shadow.
                int a = 80 + static_cast<int>(g) * 14;
                a = (a * static_cast<int>(m)) / 255;
                a = std::max(30, std::min(235, a));

                // Slight shimmer so the cloud feels alive (deterministic per tile/frame).
                a = std::max(30, std::min(245, a + (static_cast<int>((frame + x * 5 + y * 11) % 9) - 4)));

                SDL_Rect r = tileDst(x, y);

                if (haveGasTex) {
                    const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xC41u;
                    const size_t vi = static_cast<size_t>(hash32(h) % static_cast<uint32_t>(GAS_VARS));
                    const size_t fi = static_cast<size_t>((frame + ((x + y) & 1)) % FRAMES);

                    SDL_Texture* gtex = gasVar[vi][fi];
                    if (gtex) {
                        // Multiply a "signature" green by the tile lighting/tint so it feels embedded in the world.
                        const Color lmod = tileColorMod(x, y, /*visible=*/true);
                        const Color base{120, 255, 120, 255};

                        const uint8_t mr = static_cast<uint8_t>((static_cast<int>(base.r) * lmod.r) / 255);
                        const uint8_t mg = static_cast<uint8_t>((static_cast<int>(base.g) * lmod.g) / 255);
                        const uint8_t mb = static_cast<uint8_t>((static_cast<int>(base.b) * lmod.b) / 255);

                        SDL_SetTextureColorMod(gtex, mr, mg, mb);
                        SDL_SetTextureAlphaMod(gtex, static_cast<uint8_t>(a));
                        SDL_RenderCopy(renderer, gtex, nullptr, &r);
                        SDL_SetTextureColorMod(gtex, 255, 255, 255);
                        SDL_SetTextureAlphaMod(gtex, 255);
                        continue;
                    }
                }

                // Fallback: simple tinted quad (should rarely be used).
                SDL_SetRenderDrawColor(renderer, 90, 220, 90, static_cast<uint8_t>(a));
                SDL_RenderFillRect(renderer, &r);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }


    // Draw fire field (visible tiles only). This is a persistent, tile-based hazard
    // spawned primarily by Fireball explosions.
    {
        // Additive blend gives a nice glow without completely obscuring tiles.
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);

        const bool haveFireTex = isoView ? (fireVarIso[0][0] != nullptr) : (fireVar[0][0] != nullptr);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;

                const uint8_t f = game.fireAt(x, y);
                if (f == 0u) continue;

                const uint8_t m = lightMod(x, y);

                // Scale intensity by light; keep a minimum so it reads even in deep shadow.
                int a = 40 + static_cast<int>(f) * 22;
                a = (a * static_cast<int>(m)) / 255;
                a = std::max(28, std::min(235, a));

                // Flicker
                a = std::max(24, std::min(245, a + (static_cast<int>((frame + x * 5 + y * 11) % 7) - 3)));

                SDL_Rect r = tileDst(x, y);

                if (haveFireTex) {
                    const uint32_t h = hashCombine(hashCombine(static_cast<uint32_t>(game.depth()), static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xF17Eu;
                    const size_t vi = static_cast<size_t>(hash32(h) % static_cast<uint32_t>(FIRE_VARS));
                    const size_t fi = static_cast<size_t>((frame + ((x + y) & 1)) % FRAMES);

                    SDL_Texture* ftex = (isoView && fireVarIso[0][0] != nullptr) ? fireVarIso[vi][fi] : fireVar[vi][fi];
                    if (ftex) {
                        // Warm fire tint, modulated by world lighting.
                        const Color lmod = tileColorMod(x, y, /*visible=*/true);
                        const Color base{255, 160, 80, 255};

                        const uint8_t mr = static_cast<uint8_t>((static_cast<int>(base.r) * lmod.r) / 255);
                        const uint8_t mg = static_cast<uint8_t>((static_cast<int>(base.g) * lmod.g) / 255);
                        const uint8_t mb = static_cast<uint8_t>((static_cast<int>(base.b) * lmod.b) / 255);

                        SDL_SetTextureColorMod(ftex, mr, mg, mb);
                        SDL_SetTextureAlphaMod(ftex, static_cast<uint8_t>(a));
                        SDL_RenderCopy(renderer, ftex, nullptr, &r);
                        SDL_SetTextureColorMod(ftex, 255, 255, 255);
                        SDL_SetTextureAlphaMod(ftex, 255);
                        continue;
                    }
                }

                // Fallback: simple tinted quad (should rarely be used).
                SDL_SetRenderDrawColor(renderer, 255, 140, 70, static_cast<uint8_t>(a));
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
            case TrapKind::PoisonGas: r = 90; g = 220; b = 90; break;
            case TrapKind::RollingBoulder: r = 200; g = 170; b = 90; break;
            case TrapKind::TrapDoor: r = 180; g = 130; b = 90; break;
            case TrapKind::LetheMist: r = 160; g = 160; b = 210; break;
        }

        const uint8_t a = t.visible ? 220 : 120;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, r, g, b, a);

        SDL_Rect base = mapTileDst(tr.pos.x, tr.pos.y);
        const int x0 = base.x;
        const int y0 = base.y;
        const int x1 = x0 + base.w - 5;
        const int y1 = y0 + base.h - 5;
        SDL_RenderDrawLine(renderer, x0 + 4, y0 + 4, x1, y1);
        SDL_RenderDrawLine(renderer, x1, y0 + 4, x0 + 4, y1);
        SDL_RenderDrawPoint(renderer, x0 + base.w / 2, y0 + base.h / 2);
    }

    // Draw player map markers / notes (shown on explored tiles; subtle indicator).
    for (const auto& m : game.mapMarkers()) {
        if (!d.inBounds(m.pos.x, m.pos.y)) continue;
        const Tile& t = d.at(m.pos.x, m.pos.y);
        if (!t.explored) continue;
        if (!mapTileInView(m.pos.x, m.pos.y)) continue;

        uint8_t r = 220, g = 220, b = 220;
        switch (m.kind) {
            case MarkerKind::Danger: r = 230; g = 80;  b = 80;  break;
            case MarkerKind::Loot:   r = 235; g = 200; b = 80;  break;
            case MarkerKind::Note:
            default:                 r = 220; g = 220; b = 220; break;
        }

        const uint8_t a = t.visible ? 220 : 120;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, r, g, b, a);

        SDL_Rect base = mapTileDst(m.pos.x, m.pos.y);
        const int s = (m.kind == MarkerKind::Danger) ? 6 : 4;
        SDL_Rect pip{ base.x + tile - s - 2, base.y + 2, s, s };
        SDL_RenderFillRect(renderer, &pip);
    }

    // Draw entities (only if their tile is visible; player always visible)
    if (isoView) {
        // Sort entities for isometric painter's algorithm (back-to-front).
        std::vector<const Entity*> draw;
        draw.reserve(game.entities().size());
        for (const auto& e : game.entities()) {
            if (!d.inBounds(e.pos.x, e.pos.y)) continue;
            const bool show = (e.id == game.playerId()) || d.at(e.pos.x, e.pos.y).visible;
            if (!show) continue;
            draw.push_back(&e);
        }

        const int playerId = game.playerId();
        std::sort(draw.begin(), draw.end(), [&](const Entity* a, const Entity* b) {
            const bool aIsPlayer = (a->id == playerId);
            const bool bIsPlayer = (b->id == playerId);

            // Player last so they don't get hidden behind other sprites.
            if (aIsPlayer != bIsPlayer) return !aIsPlayer && bIsPlayer;

            const int sa = a->pos.x + a->pos.y;
            const int sb = b->pos.x + b->pos.y;
            if (sa != sb) return sa < sb;
            if (a->pos.y != b->pos.y) return a->pos.y < b->pos.y;
            if (a->pos.x != b->pos.x) return a->pos.x < b->pos.x;
            return a->id < b->id;
        });

        for (const Entity* ep : draw) {
            const Entity& e = *ep;
            const bool isPlayer = (e.id == playerId);

            Entity visE = e;
            if (isHallucinating(game)) {
                visE.kind = hallucinatedEntityKind(game, e);
            }

            SDL_Texture* tex = entityTexture(visE, (frame + e.id) % FRAMES);
            if (!tex) continue;

            SDL_Rect dst = spriteDst(e.pos.x, e.pos.y);
            const bool tileVis = isPlayer ? true : d.at(e.pos.x, e.pos.y).visible;
            const Color mod = tileColorMod(e.pos.x, e.pos.y, tileVis);
            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/true, /*outline*/true);

            // Small HP pip for monsters
            if (!isPlayer && e.hp > 0) {
                SDL_Rect bar{ dst.x + 2, dst.y + 2, std::max(1, (tile - 4) * e.hp / std::max(1, e.hpMax)), 4 };
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 200, 40, 40, 160);
                SDL_RenderFillRect(renderer, &bar);
            }
        }
    } else {
        for (const auto& e : game.entities()) {
            if (!d.inBounds(e.pos.x, e.pos.y)) continue;

            bool show = (e.id == game.playerId()) || d.at(e.pos.x, e.pos.y).visible;
            if (!show) continue;

            Entity visE = e;
            if (isHallucinating(game)) {
                visE.kind = hallucinatedEntityKind(game, e);
            }

            SDL_Texture* tex = entityTexture(visE, (frame + e.id) % FRAMES);
            if (!tex) continue;

            SDL_Rect dst = spriteDst(e.pos.x, e.pos.y);
            const bool tileVis = (e.id == game.playerId()) ? true : d.at(e.pos.x, e.pos.y).visible;
            const Color mod = tileColorMod(e.pos.x, e.pos.y, tileVis);
            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/true, /*outline*/true);

            // Small HP pip for monsters
            if (e.id != game.playerId() && e.hp > 0) {
                SDL_Rect bar{ dst.x + 2, dst.y + 2, std::max(1, (tile - 4) * e.hp / std::max(1, e.hpMax)), 4 };
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 200, 40, 40, 160);
                SDL_RenderFillRect(renderer, &bar);
            }
        }
    }

    // Hallucination "phantoms": purely visual, fake monsters that appear on
    // empty visible tiles while the player is hallucinating.
    //
    // These are intentionally derived from stable hashes of (seed, phase, tile)
    // so that they do NOT consume RNG state and remain compatible with
    // replay/state-hash verification.
    if (isHallucinating(game)) {
        const int w = d.width;
        const int h = d.height;
        if (w > 0 && h > 0) {
            // Occupancy map so we don't spawn phantoms on top of real entities/items.
            std::vector<uint8_t> occ;
            occ.assign(static_cast<size_t>(w * h), 0u);
            auto idx = [&](int x, int y) -> size_t {
                return static_cast<size_t>(y * w + x);
            };

            for (const auto& e : game.entities()) {
                if (!d.inBounds(e.pos.x, e.pos.y)) continue;
                occ[idx(e.pos.x, e.pos.y)] = 1u;
            }
            for (const auto& gi : game.groundItems()) {
                if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
                occ[idx(gi.pos.x, gi.pos.y)] |= 2u;
            }

            struct Phantom {
                Vec2i pos;
                EntityKind kind;
                uint32_t seed;
                uint32_t h;
            };

            // Keep the number of phantoms low to avoid overwhelming the player
            // and to keep sprite cache churn under control.
            const int maxPhantoms = 12;
            constexpr uint32_t kCount = static_cast<uint32_t>(ENTITY_KIND_COUNT);

            std::vector<Phantom> ph;
            ph.reserve(static_cast<size_t>(maxPhantoms));

            const uint32_t phase = hallucinationPhase(game);
            const uint32_t base = hashCombine(game.seed() ^ 0xF00DFACEu, phase);

            // Keep phantoms grounded in places that are normally passable and
            // unambiguous to read: avoid spawning them on stairs/doors.
            auto phantomAllowedTile = [](TileType tt) -> bool {
                return tt == TileType::Floor || tt == TileType::DoorOpen;
            };

            // Sample tiles in scanline order but gate via a hash so the
            // distribution feels "random" and deterministic.
            for (int y = 0; y < h && static_cast<int>(ph.size()) < maxPhantoms; ++y) {
                for (int x = 0; x < w && static_cast<int>(ph.size()) < maxPhantoms; ++x) {
                    if (!mapTileInView(x, y)) continue;
                    const Tile& t = d.at(x, y);
                    if (!t.visible) continue;
                    if (!phantomAllowedTile(t.type)) continue;
                    if (occ[idx(x, y)] != 0u) continue;
                    if (x == game.player().pos.x && y == game.player().pos.y) continue;

                    // Roughly ~2% chance per visible tile, then capped by maxPhantoms.
                    const uint32_t h0 = hashCombine(base, static_cast<uint32_t>(x) ^ (static_cast<uint32_t>(y) << 16));
                    const uint32_t r = hash32(h0);
                    if ((r % 1000u) >= 20u) continue;

                    if (kCount <= 1u) continue;
                    const uint32_t kk = 1u + (hash32(r ^ 0x9E3779B9u) % (kCount - 1u));

                    Phantom p;
                    p.pos = {x, y};
                    p.kind = static_cast<EntityKind>(kk);
                    p.seed = hash32(r ^ 0xA53A9u);
                    p.h = r;
                    ph.push_back(p);
                }
            }

            if (!ph.empty()) {
                // For isometric view, draw in painter order so they sit nicely in the world.
                if (isoView) {
                    std::sort(ph.begin(), ph.end(), [](const Phantom& a, const Phantom& b) {
                        const int sa = a.pos.x + a.pos.y;
                        const int sb = b.pos.x + b.pos.y;
                        if (sa != sb) return sa < sb;
                        if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
                        return a.pos.x < b.pos.x;
                    });
                }

                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                for (const Phantom& p : ph) {
                    Entity e{};
                    e.kind = p.kind;
                    e.spriteSeed = p.seed;
                    e.pos = p.pos;

                    SDL_Texture* tex = entityTexture(e, (frame + static_cast<int>(p.seed & 3u)) % FRAMES);
                    if (!tex) continue;

                    SDL_Rect dst = spriteDst(p.pos.x, p.pos.y);

                    // Subtle jitter so the phantoms feel unstable.
                    const int jx = ((static_cast<int>(hash32(p.h ^ static_cast<uint32_t>(frame))) & 1) ? 1 : -1);
                    const int jy = ((static_cast<int>(hash32(p.h ^ static_cast<uint32_t>(frame + 17))) & 1) ? 1 : -1);
                    if ((frame & 1) != 0) {
                        dst.x += jx;
                        dst.y += jy;
                    }

                    const Color mod = tileColorMod(p.pos.x, p.pos.y, /*visible*/true);

                    // Flickering alpha in a readable range.
                    const uint8_t a = static_cast<uint8_t>(std::clamp<int>(110 + static_cast<int>(hash32(p.h ^ static_cast<uint32_t>(frame * 31))) % 120, 60, 210));
                    drawSpriteWithShadowOutline(renderer, tex, dst, mod, a, /*shadow*/true, /*outline*/true);
                }
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }
        }
    }

    // Soft bloom on brightly lit visible tiles.
    // This provides a cheap "glow" effect without shaders by using additive blending.
    if (game.darknessActive()) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;
                const uint8_t L = game.tileLightLevel(x, y);
                if (L < 200u) continue;

                Color lc = game.tileLightColor(x, y);
                if (lc.r == 0 && lc.g == 0 && lc.b == 0) continue;

                // Intensity ramps up only in the top ~20% of the light range.
                const int strength = static_cast<int>(L) - 200;
                uint8_t a = static_cast<uint8_t>(std::clamp(strength * 3, 0, 70));
                // Torch flame flicker adds life to the bloom.
                const float f = torchFlicker(x, y);
                if (f != 1.0f) {
                    a = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(a) * f)), 0, 90));
                }
                if (a == 0) continue;

                SDL_Rect base = tileDst(x, y);

                // Two-layer bloom: wide + soft, then a tighter core.
                SDL_SetRenderDrawColor(renderer, lc.r, lc.g, lc.b, a);
                SDL_Rect wide{ base.x - 1, base.y - 1, base.w + 2, base.h + 2 };
                SDL_RenderFillRect(renderer, &wide);

                SDL_SetRenderDrawColor(renderer, lc.r, lc.g, lc.b, static_cast<uint8_t>(std::min<int>(static_cast<int>(a) + 10, 90)));
                SDL_Rect tight{ base.x + 2, base.y + 2, base.w - 4, base.h - 4 };
                SDL_RenderFillRect(renderer, &tight);
            }
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
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
        SDL_Rect dst = spriteDst(p.x, p.y);
        const Color mod = tileColorMod(p.x, p.y, t.visible);
        drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/false, /*outline*/true);
    }

    // FX explosions (visual-only flashes; gameplay already applied)
    // Upgraded to a layered "white-hot" core + warm bloom + spark specks.
    if (!game.fxExplosions().empty()) {
        for (const auto& ex : game.fxExplosions()) {
            if (ex.delay > 0.0f) continue;
            if (ex.tiles.empty()) continue;

            const float dur = std::max(0.001f, ex.duration);
            const float t01 = std::clamp(ex.timer / dur, 0.0f, 1.0f);
            const float inv = 1.0f - t01;

            const int aBase = static_cast<int>(std::round(240.0f * inv));
            if (aBase <= 0) continue;

            // Approximate center so the effect can be slightly brighter in the middle.
            float cx = 0.0f;
            float cy = 0.0f;
            for (const Vec2i& p : ex.tiles) {
                cx += static_cast<float>(p.x) + 0.5f;
                cy += static_cast<float>(p.y) + 0.5f;
            }
            cx /= static_cast<float>(ex.tiles.size());
            cy /= static_cast<float>(ex.tiles.size());

            auto lerpU8 = [](uint8_t a, uint8_t b, float t) -> uint8_t {
                t = std::clamp(t, 0.0f, 1.0f);
                const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
                int iv = static_cast<int>(v + 0.5f);
                return static_cast<uint8_t>(std::clamp(iv, 0, 255));
            };

            // Color shifts from a bright white-hot flash to a warmer orange as it fades.
            const Color hot{ 255, 250, 235, 255 };
            const Color warm{ 255, 150,  70, 255 };
            const Color core{ lerpU8(hot.r, warm.r, t01),
                              lerpU8(hot.g, warm.g, t01),
                              lerpU8(hot.b, warm.b, t01),
                              255 };

            // Bright core uses additive blending to "pop" without obscuring tile detail.
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);

            for (const Vec2i& p : ex.tiles) {
                if (!d.inBounds(p.x, p.y)) continue;
                const Tile& t = d.at(p.x, p.y);
                if (!t.explored) continue;

                const float dx = (static_cast<float>(p.x) + 0.5f) - cx;
                const float dy = (static_cast<float>(p.y) + 0.5f) - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float centerBoost = std::clamp(1.0f - dist * 0.45f, 0.4f, 1.0f);

                const int aCore = static_cast<int>(std::round(static_cast<float>(aBase) * centerBoost));
                if (aCore <= 0) continue;

                SDL_Rect base = tileDst(p.x, p.y);

                // Inner flash.
                SDL_SetRenderDrawColor(renderer, core.r, core.g, core.b, static_cast<uint8_t>(std::min(255, aCore)));
                SDL_Rect inner{ base.x + 4, base.y + 4, tile - 8, tile - 8 };
                SDL_RenderFillRect(renderer, &inner);

                // Soft bloom ring.
                SDL_SetRenderDrawColor(renderer, 255, 190, 110, static_cast<uint8_t>(std::min(255, aCore / 2)));
                SDL_Rect mid{ base.x + 2, base.y + 2, tile - 4, tile - 4 };
                SDL_RenderFillRect(renderer, &mid);

                // Tiny spark specks (deterministic) for texture.
                uint32_t seed = hashCombine(hashCombine(static_cast<uint32_t>(game.turns()), static_cast<uint32_t>(ticks / 40u)),
                                            hashCombine(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y)));
                const int sparks = 1 + static_cast<int>(seed & 0x3u);

                SDL_SetRenderDrawColor(renderer, 255, 240, 200, static_cast<uint8_t>(std::min(255, (aCore * 2) / 3)));
                for (int s = 0; s < sparks; ++s) {
                    seed = hash32(seed + 0x9e3779b9u + static_cast<uint32_t>(s) * 101u);
                    const int sx = base.x + 2 + static_cast<int>(seed % static_cast<uint32_t>(std::max(1, tile - 4)));
                    const int sy = base.y + 2 + static_cast<int>((seed >> 8) % static_cast<uint32_t>(std::max(1, tile - 4)));
                    SDL_RenderDrawPoint(renderer, sx, sy);
                }
            }

            // A very subtle warm "smoke" pass using normal alpha blending.
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 40, 18, 8, static_cast<uint8_t>(std::min(110, aBase / 3)));
            for (const Vec2i& p : ex.tiles) {
                if (!d.inBounds(p.x, p.y)) continue;
                const Tile& t = d.at(p.x, p.y);
                if (!t.explored) continue;
                SDL_Rect base = tileDst(p.x, p.y);
                SDL_Rect outer{ base.x + 1, base.y + 1, tile - 2, tile - 2 };
                SDL_RenderFillRect(renderer, &outer);
            }

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
    }

    // Overlays
    if (game.isLooking()) {
        drawLookOverlay(game);
    }

    if (game.isTargeting()) {
        drawTargetingOverlay(game);
    }

    // Post FX: subtle vignette over map region only.
    drawVignette(renderer, mapClip, /*thickness*/tile / 2, /*maxAlpha*/70);

    // Map drawing complete; release clip so HUD/UI can render normally.
    SDL_RenderSetClipRect(renderer, nullptr);

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

    if (game.isCodexOpen()) {
        drawCodexOverlay(game);
    }

    if (game.isDiscoveriesOpen()) {
        drawDiscoveriesOverlay(game);
    }

    if (game.isScoresOpen()) {
        drawScoresOverlay(game);
    }

    if (game.isMessageHistoryOpen()) {
        drawMessageHistoryOverlay(game);
    }

    if (game.isInventoryOpen()) {
        drawInventoryOverlay(game);
    }

    if (game.isChestOpen()) {
        drawChestOverlay(game);
    }

    if (game.isOptionsOpen()) {
        drawOptionsOverlay(game);
    }

    if (game.isKeybindsOpen()) {
        drawKeybindsOverlay(game);
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
    const Color important{255,160,255,255};

    // Top row: Title and basic stats
    {
        const std::string hudTitle = std::string("PROCROGUE++ V") + PROCROGUE_VERSION;
        drawText5x7(renderer, 8, winH - hudH + 8, 2, white, hudTitle);
    }

    const Entity& p = game.player();

    // Status effect icons (right side of the top HUD row).
    {
        std::vector<std::pair<EffectKind, int>> effs;
        effs.reserve(static_cast<size_t>(EFFECT_KIND_COUNT));
        for (int k = 0; k < EFFECT_KIND_COUNT; ++k) {
            const EffectKind ek = static_cast<EffectKind>(k);
            const int turns = p.effects.get(ek);
            if (turns > 0) effs.emplace_back(ek, turns);
        }

        if (!effs.empty()) {
            const int icon = 16;
            const int gap = 3;
            const int totalW = static_cast<int>(effs.size()) * (icon + gap) - gap;
            int x0 = winW - 8 - totalW;
            const int y0 = winH - hudH + 6;

            for (int i = 0; i < static_cast<int>(effs.size()); ++i) {
                const int k = static_cast<int>(effs[static_cast<size_t>(i)].first);
                SDL_Texture* tex = effectIconTex[static_cast<size_t>(k)][static_cast<size_t>(lastFrame)];
                if (!tex) continue;

                SDL_Rect dst { x0 + i * (icon + gap), y0, icon, icon };
                SDL_SetTextureAlphaMod(tex, 240);
                SDL_RenderCopy(renderer, tex, nullptr, &dst);
                SDL_SetTextureAlphaMod(tex, 255);

                if (game.showEffectTimers()) {
                    int turns = effs[static_cast<size_t>(i)].second;
                    if (turns > 99) turns = 99;
                    const std::string tstr = std::to_string(turns);

                    // Bottom-right corner
                    const int tx = dst.x + icon - static_cast<int>(tstr.size()) * 6;
                    const int ty = dst.y + icon - 7;
                    drawText5x7(renderer, tx, ty, 1, white, tstr);
                }
            }
        }
    }

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
    if (game.depth() == 0) ss << " | DEPTH: CAMP";
    else ss << " | DEPTH: " << game.depth() << "/" << game.dungeonMaxDepth();
    ss << " | DEEPEST: " << game.maxDepthReached();
    ss << " | TURNS: " << game.turns();
    ss << " | KILLS: " << game.kills();

    // Companions
    {
        int allies = 0;
        for (const auto& e : game.entities()) {
            if (e.id == p.id) continue;
            if (e.hp <= 0) continue;
            if (e.friendly) ++allies;
        }
        if (allies > 0) ss << " | ALLIES: " << allies;
    }

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
    addStatus("CONF", p.effects.confusionTurns);
    addStatus("FEAR", p.effects.fearTurns);
    addStatus("BURN", p.effects.burnTurns);
    addStatus("REGEN", p.effects.regenTurns);
    addStatus("SHIELD", p.effects.shieldTurns);
    addStatus("HASTE", p.effects.hasteTurns);
    addStatus("VISION", p.effects.visionTurns);
    addStatus("INVIS", p.effects.invisTurns);
    addStatus("LEV", p.effects.levitationTurns);
    addStatus("HALL", p.effects.hallucinationTurns);
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
    {
        const std::string lt = game.lightTag();
        if (!lt.empty()) ss << " | " << lt;
    }
    if (game.yendorDoomActive()) {
        ss << " | DOOM: " << game.yendorDoomLevel();
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
    if (game.isKicking()) {
        drawText5x7(renderer, 8, controlY2, 2, yellow,
            "KICK: CHOOSE DIRECTION (ESC CANCEL)");
    } else if (game.isDigging()) {
        drawText5x7(renderer, 8, controlY2, 2, yellow,
            "DIG: CHOOSE DIRECTION (ESC CANCEL)");
    } else {
        drawText5x7(renderer, 8, controlY2, 2, gray,
            "D DIG | B KICK | F FIRE | G PICKUP | I INV | O EXPLORE | P AUTOPICKUP | C SEARCH (TRAPS/SECRETS)");
    }
    drawText5x7(renderer, 8, controlY3, 2, gray,
        "F2 OPT | F3 MSGS | # CMD | M MAP | SHIFT+TAB STATS | F5 SAVE | F6 SCORES | F9 LOAD | PGUP/PGDN LOG | ? HELP");

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
            case MessageKind::Info:      c = white; break;
            case MessageKind::Combat:    c = red; break;
            case MessageKind::Loot:      c = yellow; break;
            case MessageKind::Warning:   c = yellow; break;
            case MessageKind::ImportantMsg: c = important; break;
            case MessageKind::Success:   c = green; break;
            case MessageKind::System:    c = gray; break;
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
            case ItemKind::PotionInvisibility: return "EFFECT: INVISIBILITY";
            case ItemKind::PotionClarity: return "EFFECT: CLARITY";
            case ItemKind::PotionLevitation: return "EFFECT: LEVITATION";
            case ItemKind::PotionHallucination: return "EFFECT: HALLUCINATION";
            case ItemKind::ScrollTeleport: return "EFFECT: TELEPORT";
            case ItemKind::ScrollMapping: return "EFFECT: MAPPING";
            case ItemKind::ScrollDetectTraps: return "EFFECT: DETECT TRAPS";
			case ItemKind::ScrollDetectSecrets: return "EFFECT: DETECT SECRETS";
            case ItemKind::ScrollKnock: return "EFFECT: KNOCK";
            case ItemKind::ScrollEnchantWeapon: return "EFFECT: ENCHANT WEAPON";
            case ItemKind::ScrollEnchantArmor: return "EFFECT: ENCHANT ARMOR";
            case ItemKind::ScrollIdentify: return "EFFECT: IDENTIFY";
            case ItemKind::ScrollRemoveCurse: return "EFFECT: REMOVE CURSE";
            case ItemKind::ScrollConfusion: return "EFFECT: CONFUSION";
            case ItemKind::ScrollFear: return "EFFECT: FEAR";
            case ItemKind::ScrollEarth: return "EFFECT: EARTH";
            case ItemKind::ScrollTaming: return "EFFECT: TAMING";
			case ItemKind::FoodRation:
				return def.hungerRestore > 0
					? ("EFFECT: RESTORE HUNGER +" + std::to_string(def.hungerRestore))
					: "EFFECT: FOOD";
            default: break;
        }
        return "EFFECT: —";
    };

    // Draw list (with item icons)
    int yy = listRect.y;

    const int icon = 16;
    const int arrowW = scale * 6 * 2; // "> " column
    const int iconX = listRect.x + arrowW;
    const int textX = iconX + icon + 6;
    const int maxChars = std::max(10, (listRect.w - (textX - listRect.x)) / (scale * 6));

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (int i = start; i < end; ++i) {
        const Item& it = inv[static_cast<size_t>(i)];
        const std::string tag = game.equippedTag(it.id); // "" or "M"/"R"/"A"/...

        const Color c = (i == sel) ? white : gray;

        // Selection arrow
        drawText5x7(renderer, listRect.x, yy, scale, c, (i == sel) ? ">" : " ");

        // Icon background (subtle), then sprite
        SDL_Rect iconDst{ iconX, yy + (lineH - icon) / 2, icon, icon };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (i == sel) ? 70 : 45);
        SDL_RenderFillRect(renderer, &iconDst);

        Item visIt = it;
        if (isHallucinating(game)) {
            visIt.kind = hallucinatedItemKind(game, it);
        }

        applyIdentificationVisuals(game, visIt);

        SDL_Texture* itex = itemTexture(visIt, lastFrame);
        if (itex) {
            SDL_RenderCopy(renderer, itex, nullptr, &iconDst);
        }

        // Text (tag + name)
        std::string row;
        if (!tag.empty()) {
            row += "[";
            row += tag;
            row += "] ";
        }
        row += game.displayItemName(it);
        drawText5x7(renderer, textX, yy, scale, c, fitToChars(row, maxChars));

        yy += lineH;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

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
        const int previewPx = std::min(96, infoRect.w);
        SDL_Rect sprDst{ ix, iy, previewPx, previewPx };
        Item visIt = it;
        if (isHallucinating(game)) {
            visIt.kind = hallucinatedItemKind(game, it);
        }

        applyIdentificationVisuals(game, visIt);

        SDL_Texture* tex = itemTexture(visIt, lastFrame);
        if (tex) {
            SDL_RenderCopy(renderer, tex, nullptr, &sprDst);
        }
        iy += previewPx + 10;

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
			statLine(identifiable ? "TYPE: WAND (IDENTIFIABLE)" : "TYPE: WAND", white);

			if (identifiable && !identified) {
				statLine("EFFECT: UNKNOWN", gray);
				statLine("RANGE: UNKNOWN", gray);
				statLine("CHARGES: UNKNOWN", gray);
				statLine("READY: UNKNOWN", gray);
				statLine("IDENTIFIED: NO", gray);
			} else {
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
				if (identifiable) {
					statLine("IDENTIFIED: YES", gray);
				}
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
		} else if (isRingKind(it.kind)) {
			statLine(identifiable ? "TYPE: RING (IDENTIFIABLE)" : "TYPE: RING", white);

			if (identifiable && !identified) {
				statLine("EFFECT: UNKNOWN", gray);
				statLine("IDENTIFIED: NO", gray);
			} else {
				const int bucBonus = (it.buc < 0 ? -1 : (it.buc > 0 ? 1 : 0));
				auto fmtMod = [&](const char* label, int base) {
					if (base == 0) return;
					// Only apply ench/buc if the ring actually provides the stat.
					const int v = base + it.enchant + bucBonus;
					const std::string s = (v >= 0 ? "+" : "") + std::to_string(v);
					statLine(std::string(label) + s, gray);
				};
				fmtMod("MIGHT: ", def.modMight);
				fmtMod("AGILITY: ", def.modAgility);
				fmtMod("VIGOR: ", def.modVigor);
				fmtMod("FOCUS: ", def.modFocus);
				if (def.defense != 0) {
					const int v = def.defense + it.enchant + bucBonus;
					const std::string s = (v >= 0 ? "+" : "") + std::to_string(v);
					statLine("DEF BONUS: " + s, gray);
				}
				if (identifiable) {
					statLine("IDENTIFIED: YES", gray);
				}
			}
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
		statLine("1: " + game.equippedRing1Name(), gray);
		statLine("2: " + game.equippedRing2Name(), gray);
    }
}


void Renderer::drawChestOverlay(const Game& game) {
    const int panelW = winW - 40;
    const int panelH = winH - 40;
    SDL_Rect bg{ 20, 20, panelW, panelH };

    drawPanel(game, bg, 210, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    const int scale = 2;
    const int pad = 16;

    int x = bg.x + pad;
    int y = bg.y + pad;

    auto tierName = [](int tier) -> const char* {
        switch (tier) {
            case 0: return "COMMON";
            case 1: return "STURDY";
            case 2: return "ORNATE";
            case 3: return "LARGE";
            case 4: return "ANCIENT";
            default: return "CHEST";
        }
    };

    const int tier = game.chestOpenTier();
    const int limit = game.chestOpenStackLimit();
    const int chestStacks = static_cast<int>(game.chestOpenItems().size());

    std::stringstream hs;
    hs << "CHEST (" << tierName(tier) << ")";
    drawText5x7(renderer, x, y, scale, yellow, hs.str());

    drawText5x7(renderer, x + 220, y, scale, gray, "(ENTER: move, D: move 1, G: all, S: sort, ESC/I: close)");

    std::stringstream cap;
    cap << "CAP: " << chestStacks << "/" << limit << " STACKS  (LEFT/RIGHT: switch pane)";
    drawText5x7(renderer, x, y + 14, scale, gray, cap.str());

    y += 44;

    const bool paneChest = game.chestPaneIsChest();

    const int colGap = 18;
    const int colW = (bg.w - pad * 2 - colGap) / 2;

    // Column headers
    drawText5x7(renderer, x, y, scale, paneChest ? yellow : gray, "CHEST CONTENTS");
    drawText5x7(renderer, x + colW + colGap, y, scale, paneChest ? gray : yellow, "INVENTORY");

    y += 28;

    SDL_Rect chestRect{ x, y, colW, bg.y + bg.h - pad - y };
    SDL_Rect invRect{ x + colW + colGap, y, colW, chestRect.h };

    const auto& chestItems = game.chestOpenItems();
    const auto& inv = game.inventory();

    const int chestSel = game.chestSelection();
    const int invSel = game.inventorySelection();

    const int lineH = 18;
    const int maxLines = std::max(1, chestRect.h / lineH);

    auto startIndex = [&](int sel, int count) -> int {
        if (count <= 0) return 0;
        return std::clamp(sel - maxLines / 2, 0, std::max(0, count - maxLines));
    };

    const int chestStart = startIndex(chestSel, (int)chestItems.size());
    const int invStart = startIndex(invSel, (int)inv.size());

    const int chestEnd = std::min((int)chestItems.size(), chestStart + maxLines);
    const int invEnd = std::min((int)inv.size(), invStart + maxLines);

    // Selection highlight
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (paneChest && !chestItems.empty() && chestSel >= chestStart && chestSel < chestEnd) {
        SDL_Rect hi{ chestRect.x - 6, chestRect.y + (chestSel - chestStart) * lineH - 2, chestRect.w + 12, lineH };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 20);
        SDL_RenderFillRect(renderer, &hi);
    }
    if (!paneChest && !inv.empty() && invSel >= invStart && invSel < invEnd) {
        SDL_Rect hi{ invRect.x - 6, invRect.y + (invSel - invStart) * lineH - 2, invRect.w + 12, lineH };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 20);
        SDL_RenderFillRect(renderer, &hi);
    }

    // Helpers
    auto fitToChars = [](const std::string& s, int maxChars) -> std::string {
        if ((int)s.size() <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, std::max(0, maxChars));
        return s.substr(0, maxChars - 3) + "...";
    };

    auto drawList = [&](const std::vector<Item>& items, const SDL_Rect& r, int start, int end, int sel, bool active, bool showEquippedTag) {
        int rowY = r.y;
        const int iconX = r.x;
        const int textX = iconX + 20;
        const int maxChars = std::max(8, (r.w - 26) / ((5 + 1) * scale));

        if (items.empty()) {
            drawText5x7(renderer, r.x, r.y, scale, gray, "(EMPTY)");
            return;
        }

        for (int i = start; i < end; ++i) {
            const Item& it = items[(size_t)i];

            // Selected arrow (active pane only).
            if (active && i == sel) {
                drawText5x7(renderer, r.x - 12, rowY + 3, scale, yellow, ">");
            }

            drawItemIcon(game, it, iconX, rowY, 16);

            std::string line = game.displayItemName(it);
            if (showEquippedTag) {
                const std::string tag = game.equippedTag(it.id);
                if (!tag.empty()) {
                    line += " " + tag;
                }
            }
            line = fitToChars(line, maxChars);

            drawText5x7(renderer, textX, rowY + 3, scale, white, line);

            rowY += lineH;
        }
    };

    drawList(chestItems, chestRect, chestStart, chestEnd, chestSel, paneChest, false);
    drawList(inv, invRect, invStart, invEnd, invSel, !paneChest, true);
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
    drawOpt(2, "AUTO-EXPLORE SEARCH", yesNo(game.autoExploreSearchEnabled()));
    drawOpt(3, "AUTOSAVE", (game.autosaveEveryTurns() > 0 ? ("EVERY " + std::to_string(game.autosaveEveryTurns()) + " TURNS") : "OFF"));
    drawOpt(4, "IDENTIFY ITEMS", yesNo(game.identificationEnabled()));
    drawOpt(5, "HUNGER SYSTEM", yesNo(game.hungerEnabled()));
    drawOpt(6, "ENCUMBRANCE", yesNo(game.encumbranceEnabled()));
    drawOpt(7, "LIGHTING", yesNo(game.lightingEnabled()));
    drawOpt(8, "YENDOR DOOM", yesNo(game.yendorDoomEnabled()));
    drawOpt(9, "EFFECT TIMERS", yesNo(game.showEffectTimers()));
    drawOpt(10, "CONFIRM QUIT", yesNo(game.confirmQuitEnabled()));
    drawOpt(11, "AUTO MORTEM", yesNo(game.autoMortemEnabled()));
    drawOpt(12, "BONES FILES", yesNo(game.bonesEnabled()));
    drawOpt(13, "SAVE BACKUPS", (game.saveBackups() > 0 ? std::to_string(game.saveBackups()) : "OFF"));
    drawOpt(14, "UI THEME", uiThemeLabel(game.uiTheme()));
    drawOpt(15, "UI PANELS", (game.uiPanelsTextured() ? "TEXTURED" : "SOLID"));
    drawOpt(16, "3D SPRITES", yesNo(game.voxelSpritesEnabled()));
    drawOpt(17, "CONTROL PRESET", game.controlPresetDisplayName());
    drawOpt(18, "KEYBINDS", "");
    drawOpt(19, "CLOSE", "");

    y += 14;
    drawText5x7(renderer, x0 + 16, y, scale, gray,
        "LEFT/RIGHT: change | ENTER: toggle/next/open | ESC: close");
}

void Renderer::drawKeybindsOverlay(const Game& game) {
    const int panelW = std::min(winW - 80, 980);
    const int panelH = std::min(winH - 80, 640);
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - panelH) / 2;

    SDL_Rect bg{x0, y0, panelW, panelH};
    drawPanel(game, bg, 220, lastFrame);

    const Color white{240, 240, 240, 255};
    const Color gray{160, 160, 160, 255};
    const Color yellow{255, 230, 120, 255};
    const Color warn{255, 170, 120, 255};

    const int scale = 2;

    int y = y0 + 16;
    drawText5x7(renderer, x0 + 16, y, scale, yellow, "KEYBINDS");
    y += 24;

    const auto& rows = game.keybindsDescription();
    const int n = static_cast<int>(rows.size());
    const int sel = game.keybindsSelection();
    const int scroll = game.keybindsScroll();

    auto upperSpaces = [&](std::string s) {
        for (char& ch : s) {
            if (ch == '_') ch = ' ';
            else ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return s;
    };

    auto fit = [&](const std::string& s, int maxChars) -> std::string {
        if (maxChars <= 0) return "";
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, maxChars);
        return s.substr(0, maxChars - 3) + "...";
    };

    const int lineH = 18;
    const int footerH = 54;
    const int headerPad = 8;
    const int listTop = y + headerPad;
    const int listH = panelH - (listTop - y0) - footerH;
    const int visibleRows = std::max(1, listH / lineH);

    const int start = clampi(scroll, 0, std::max(0, n - visibleRows));
    int yy = listTop;

    if (n <= 0) {
        drawText5x7(renderer, x0 + 16, yy, scale, warn, "NO KEYBINDS DATA (TRY REOPENING OPTIONS).");
    } else {
        // Column sizing (monospace-ish 5x7): ~6px per char at scale1.
        const int maxCharsTotal = std::max(0, (panelW - 32) / (6 * scale));
        const int labelChars = 20;
        const int valueChars = std::max(0, maxCharsTotal - 4 - labelChars); // 4 for prefix + spaces

        for (int i = start; i < n && i < start + visibleRows; ++i) {
            const Color c = (i == sel) ? white : gray;
            std::string label = upperSpaces(rows[i].first);
            std::string val = rows[i].second;

            // Build a padded label column for alignment.
            label = fit(label, labelChars);
            if (static_cast<int>(label.size()) < labelChars) {
                label += std::string(labelChars - label.size(), ' ');
            }

            const std::string prefix = (i == sel) ? "> " : "  ";
            const std::string line = prefix + label + " : " + fit(val, valueChars);

            drawText5x7(renderer, x0 + 16, yy, scale, c, line);
            yy += lineH;
        }
    }

    // Footer / instructions
    int fy = y0 + panelH - footerH + 10;
    drawText5x7(renderer, x0 + 16, fy, 1, gray,
        "UP/DOWN SELECT  ENTER REBIND  RIGHT ADD  LEFT RESET  ESC BACK");

    fy += 16;

    if (game.isKeybindsCapturing()) {
        const int capIdx = game.keybindsCaptureActionIndex();
        std::string target = "UNKNOWN";
        if (capIdx >= 0 && capIdx < n) target = upperSpaces(rows[capIdx].first);

        std::string mode = game.keybindsCaptureAddMode() ? "ADD" : "REPLACE";
        drawText5x7(renderer, x0 + 16, fy, 2, warn, "PRESS KEY: " + target + " (" + mode + ")");
    } else {
        drawText5x7(renderer, x0 + 16, fy, 1, gray, "TIP: EXT CMD #bind / #unbind / #binds ALSO AVAILABLE");
    }
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
    if (game.controlPreset() == ControlPreset::Nethack) {
        lineGray("MOVE: HJKL + YUBN (ARROWS/NUMPAD OK)");
        lineGray("SPACE/. WAIT  R REST  SHIFT+N SNEAK  < > STAIRS");
        lineGray("F FIRE  G/, PICKUP  I/TAB INVENTORY");
        lineGray("D DIG  CTRL+D KICK  :/V LOOK  S SEARCH  T DISARM  C CLOSE  SHIFT+C LOCK");
    } else {
        lineGray("MOVE: WASD / ARROWS / NUMPAD + Q/E/Z/C DIAGONALS");
        lineGray("SPACE/. WAIT  R REST  N SNEAK  < > STAIRS");
        lineGray("F FIRE  G/, PICKUP  I/TAB INVENTORY");
        lineGray("D DIG  B KICK  L/V LOOK  SHIFT+C SEARCH  T DISARM  K CLOSE  SHIFT+K LOCK");
    }
    lineGray("O EXPLORE  P AUTOPICKUP  M MINIMAP  SHIFT+TAB STATS");
    lineGray("MINIMAP: MOVE CURSOR (ARROWS/WASD), ENTER TRAVEL, L/RMB LOOK, LMB TRAVEL");
    lineGray("F2 OPTIONS  # EXTENDED COMMANDS  (TYPE + ENTER)");
    lineGray("F5 SAVE  F9 LOAD  F10 LOAD AUTO  F6 RESTART");
    lineGray("F11 FULLSCREEN  F12 SCREENSHOT (BINDABLE)");
    lineGray("F3/SHIFT+M MESSAGE HISTORY  (/ SEARCH, CTRL+L CLEAR)");
    lineGray("F4 MONSTER CODEX  (TAB SORT, LEFT/RIGHT FILTER)");
    lineGray("\\ DISCOVERIES  (TAB/LEFT/RIGHT FILTER, SHIFT+S SORT)");
    lineGray("PGUP/PGDN LOG  ESC CANCEL/QUIT");

    y += 6;
    lineWhite("EXTENDED COMMAND EXAMPLES:");
    lineGray("save | load | loadauto | quit | version | seed | name | scores");
    lineGray("autopickup off/gold/all");
    lineGray("mark [note|danger|loot] <label>  marks  travel <index|label>");
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
    lineGray("LOCKED DOORS: USE KEYS, LOCKPICKS, A SCROLL OF KNOCK, OR KICK THEM IN (RISKY).");
    lineGray("KICKING CHESTS MAY TRIGGER TRAPS AND CAN SLIDE THEM.");
    lineGray("OPEN CHESTS CAN STORE ITEMS: ENTER OPENS, ENTER MOVES STACK, D MOVES 1, G MOVES ALL.");
    lineGray("SOME VAULT DOORS MAY BE TRAPPED.");
    lineGray("AUTO-EXPLORE STOPS IF YOU SEE AN ENEMY OR GET HURT/DEBUFFED.");
    lineGray("INVENTORY: E EQUIP  U USE  X DROP  SHIFT+X DROP ALL");
    lineGray("SCROLL THE MESSAGE LOG WITH PGUP/PGDN.");
}




void Renderer::drawMinimapOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Color white{ 240, 240, 240, 255 };
    const Dungeon& d = game.dungeon();
    // Room type cache (minimap) — rebuilt if the dungeon changed.
    if (roomCacheDungeon != &d || roomCacheDepth != game.depth() ||
        roomCacheW != d.width || roomCacheH != d.height || roomCacheRooms != d.rooms.size() ||
        roomTypeCache.size() != static_cast<size_t>(d.width * d.height)) {

        roomCacheDungeon = &d;
        roomCacheDepth = game.depth();
        roomCacheW = d.width;
        roomCacheH = d.height;
        roomCacheRooms = d.rooms.size();

        roomTypeCache.assign(static_cast<size_t>(d.width * d.height), static_cast<uint8_t>(RoomType::Normal));
        for (const Room& r : d.rooms) {
            for (int yy = r.y; yy < r.y2(); ++yy) {
                for (int xx = r.x; xx < r.x2(); ++xx) {
                    if (!d.inBounds(xx, yy)) continue;
                    roomTypeCache[static_cast<size_t>(yy * d.width + xx)] = static_cast<uint8_t>(r.type);
                }
            }
        }
    }

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
    drawPanel(game, panel, 210, lastFrame);

    // Title
    drawText5x7(renderer, x0 + pad, y0 + 4, 2, white, "MINIMAP (M)");

    // Hint line (fit inside the title band).
    const Color gray{ 160, 160, 160, 255 };
    drawText5x7(renderer, x0 + pad, y0 + 4 + 14, 1, gray, "LMB/ENTER:TRAVEL  RMB/L:LOOK");

    // Cursor coordinates (right aligned).
    if (game.minimapCursorActive()) {
        const Vec2i c = game.minimapCursor();
        const std::string coords = std::to_string(c.x) + "," + std::to_string(c.y);
        const int charW = (5 + 1) * 1;
        const int textW = static_cast<int>(coords.size()) * charW;
        drawText5x7(renderer, x0 + panelW - pad - textW, y0 + 4 + 14, 1, gray, coords);
    }

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
            } else if (t.type == TileType::Boulder) {
                // Boulders are pushable obstacles; display them darker than pillars.
                if (vis) drawCell(x, y, 95, 98, 104);
                else     drawCell(x, y, 55, 58, 62);
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
                // Floor/other passable (tinted by discovered room type)
                const size_t ii = static_cast<size_t>(y * W + x);
                const uint8_t rt = (ii < roomTypeCache.size()) ? roomTypeCache[ii] : static_cast<uint8_t>(RoomType::Normal);

                uint8_t r = 30, g = 30, b = 30;
                switch (static_cast<RoomType>(rt)) {
                    case RoomType::Treasure: r = 55; g = 45; b = 22; break;
                    case RoomType::Shrine:   r = 25; g = 35; b = 58; break;
                    case RoomType::Lair:     r = 24; g = 42; b = 24; break;
                    case RoomType::Secret:   r = 40; g = 26; b = 45; break;
                    case RoomType::Vault:    r = 30; g = 38; b = 58; break;
                    case RoomType::Shop:     r = 45; g = 35; b = 24; break;
                    case RoomType::Normal:
                    default: break;
                }

                if (vis) {
                    drawCell(x, y, r, g, b);
                } else {
                    drawCell(x, y, static_cast<uint8_t>(std::max<int>(10, r / 2)),
                                   static_cast<uint8_t>(std::max<int>(10, g / 2)),
                                   static_cast<uint8_t>(std::max<int>(10, b / 2)));
                }
            }
        }
    }

    // Room outlines (only if at least one tile has been explored).
    auto outlineColor = [&](RoomType rt) -> Color {
        switch (rt) {
            case RoomType::Treasure: return Color{220, 200, 120, 90};
            case RoomType::Shrine:   return Color{140, 200, 255, 90};
            case RoomType::Lair:     return Color{140, 220, 140, 90};
            case RoomType::Secret:   return Color{220, 140, 255, 90};
            case RoomType::Vault:    return Color{200, 220, 255, 90};
            case RoomType::Shop:     return Color{220, 180, 120, 90};
            case RoomType::Normal:
            default:                 return Color{160, 160, 160, 70};
        }
    };

    for (const Room& r : d.rooms) {
        bool discovered = false;
        for (int yy = r.y; yy < r.y2() && !discovered; ++yy) {
            for (int xx = r.x; xx < r.x2(); ++xx) {
                if (!d.inBounds(xx, yy)) continue;
                if (d.at(xx, yy).explored) { discovered = true; break; }
            }
        }
        if (!discovered) continue;

        const Color c = outlineColor(r.type);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        SDL_Rect rr { mapX + r.x * px, mapY + r.y * px, r.w * px, r.h * px };
        SDL_RenderDrawRect(renderer, &rr);
    }

    // Player map markers / notes (explored tiles only).
    for (const auto& m : game.mapMarkers()) {
        if (!d.inBounds(m.pos.x, m.pos.y)) continue;
        const Tile& t = d.at(m.pos.x, m.pos.y);
        if (!t.explored) continue;

        const bool vis = t.visible;
        uint8_t r = 230, g = 230, b = 230;
        switch (m.kind) {
            case MarkerKind::Danger: r = 255; g = 80;  b = 80;  break;
            case MarkerKind::Loot:   r = 255; g = 220; b = 120; break;
            case MarkerKind::Note:
            default:                 r = 230; g = 230; b = 230; break;
        }

        // Fade markers in the fog-of-war (still visible, but less prominent).
        if (!vis) {
            r = static_cast<uint8_t>(std::max<int>(40, r / 2));
            g = static_cast<uint8_t>(std::max<int>(40, g / 2));
            b = static_cast<uint8_t>(std::max<int>(40, b / 2));
        }

        drawCell(m.pos.x, m.pos.y, r, g, b, 220);
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

    // Viewport indicator (camera): draw the currently visible map region on the minimap.
    {
        const int vw = std::min(viewTilesW, W);
        const int vh = std::min(viewTilesH, H);
        if (vw > 0 && vh > 0) {
            const int vx = std::clamp(camX, 0, std::max(0, W - vw));
            const int vy = std::clamp(camY, 0, std::max(0, H - vh));

            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
            SDL_Rect vr { mapX + vx * px, mapY + vy * px, vw * px, vh * px };
            SDL_RenderDrawRect(renderer, &vr);

            // Slightly thicker border for readability (if space allows).
            SDL_Rect vr2 { vr.x - 1, vr.y - 1, vr.w + 2, vr.h + 2 };
            SDL_RenderDrawRect(renderer, &vr2);
        }
    }

    // Minimap cursor highlight (UI-only)
    if (game.minimapCursorActive()) {
        const Vec2i c = game.minimapCursor();
        if (d.inBounds(c.x, c.y)) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
            SDL_Rect rc { mapX + c.x * px, mapY + c.y * px, px, px };
            SDL_RenderDrawRect(renderer, &rc);
            // Slightly thicker border when the minimap is large enough.
            if (px >= 4) {
                SDL_Rect rc2 { rc.x - 1, rc.y - 1, rc.w + 2, rc.h + 2 };
                SDL_RenderDrawRect(renderer, &rc2);
            }
        }
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
    drawPanel(game, panel, 230, lastFrame);

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
        ss << "CLASS: " << game.playerClassDisplayName();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        if (game.depth() == 0) ss << "DEPTH: CAMP  (DEEPEST: " << game.maxDepthReached() << ")";
        else ss << "DEPTH: " << game.depth() << "/" << game.dungeonMaxDepth() << "  (DEEPEST: " << game.maxDepthReached() << ")";
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

    // Renderer performance/debug info.
    {
        std::stringstream ss;
        ss << "RENDER: TILE " << std::clamp(tile, 16, 256) << "px"
           << "  VOXEL: " << (game.voxelSpritesEnabled() ? "ON" : "OFF")
           << "  VIEW: " << viewTilesW << "x" << viewTilesH
           << "  CAM: " << camX << "," << camY
           << "  DECALS/STYLE: " << decalsPerStyleUsed
           << "  AUTOTILE VARS: " << autoVarsUsed;
        drawText5x7(renderer, x0 + pad, y, 2, gray, ss.str());
        y += 18;
    }
    {
        size_t ent = 0, item = 0, proj = 0;
        spriteTex.countByCategory(ent, item, proj);

        const size_t usedMB = spriteTex.usedBytes() / (1024ull * 1024ull);
        const size_t budgetMB = spriteTex.budgetBytes() / (1024ull * 1024ull);

        std::stringstream ss;
        ss << "SPRITE CACHE: " << usedMB << "MB / ";
        if (spriteTex.budgetBytes() == 0) ss << "UNLIMITED";
        else ss << budgetMB << "MB";
        ss << "  (E:" << ent << " I:" << item << " P:" << proj << ")"
           << "  H:" << spriteTex.hits() << " M:" << spriteTex.misses() << " EV:" << spriteTex.evictions();

        drawText5x7(renderer, x0 + pad, y, 2, gray, ss.str());
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



void Renderer::drawScoresOverlay(const Game& game) {
    const int pad = 14;
    const int panelW = winW * 9 / 10;
    const int panelH = winH * 9 / 10;
    const int panelX = (winW - panelW) / 2;
    const int panelY = (winH - panelH) / 2;

    drawPanel(panelX, panelY, panelW, panelH);

    const int titleScale = 2;
    const int bodyScale = 1;
    const int lineH = 10 * bodyScale;

    const SDL_Color white = {255, 255, 255, 255};
    const SDL_Color gray = {160, 160, 160, 255};
    const SDL_Color selCol = {240, 240, 120, 255};

    int x = panelX + pad;
    int y = panelY + pad;

    drawText5x7(renderer, x, y, titleScale, white, "SCORES");
    y += 20;

    std::ostringstream header;
    header << "VIEW: " << scoresViewDisplayName(game.scoresView())
           << "  (LEFT/RIGHT TO TOGGLE)   UP/DOWN SELECT   PGUP/PGDN JUMP   ESC CLOSE";
    drawTextWrapped5x7(renderer, x, y, bodyScale, gray, header.str(), panelW - pad * 2);
    y += 30;

    const int topH = (y - panelY) + 10;
    const int innerX = panelX + pad;
    const int innerY = panelY + topH;
    const int innerW = panelW - pad * 2;
    const int innerH = panelH - topH - pad;

    const int listW = innerW * 6 / 10;
    const int detailX = innerX + listW + pad;
    const int detailW = innerW - listW - pad;

    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
    SDL_RenderDrawLine(renderer, detailX - pad / 2, innerY, detailX - pad / 2, innerY + innerH);

    std::vector<size_t> order;
    game.buildScoresList(order);
    const auto& entries = game.scoreBoard().entries();
    const int total = static_cast<int>(order.size());
    const int sel = clampi(game.scoresSelection(), 0, std::max(0, total - 1));

    auto fitToChars = [&](const std::string& s, int maxChars) -> std::string {
        if (maxChars <= 0) return "";
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, maxChars);
        return s.substr(0, maxChars - 3) + "...";
    };

    // Left: list
    {
        const SDL_Rect clip = {innerX, innerY, listW, innerH};
        ClipRectGuard g(renderer, clip);

        if (total <= 0) {
            drawText5x7(renderer, innerX, innerY, bodyScale, gray, "NO RUNS RECORDED YET.");
        } else {
            const int rows = std::max(1, innerH / lineH);
            const int maxScroll = std::max(0, total - rows);
            const int scroll = clampi(sel - rows / 2, 0, maxScroll);

            for (int row = 0; row < rows; ++row) {
                const int viewIdx = scroll + row;
                if (viewIdx >= total) break;

                const ScoreEntry& e = entries[order[viewIdx]];
                std::ostringstream ss;

                if (game.scoresView() == ScoresView::Top) {
                    ss << "#" << std::setw(3) << (viewIdx + 1) << "  ";
                    ss << "S" << std::setw(6) << e.score << "  ";
                    ss << "D" << std::setw(2) << e.depth << "  ";
                    ss << (e.won ? "W " : "D ");
                    ss << e.name;
                    if (!e.className.empty()) ss << " (" << e.className << ")";
                } else {
                    std::string date = e.timestamp;
                    if (date.size() >= 10) date = date.substr(0, 10);
                    ss << date << "  " << (e.won ? "W " : "D ");
                    ss << "S" << e.score << " D" << e.depth << " ";
                    ss << e.name;
                    if (!e.className.empty()) ss << " (" << e.className << ")";
                }

                const int maxChars = std::max(1, (listW - 4) / 6);
                const std::string line = fitToChars(ss.str(), maxChars);
                drawText5x7(renderer, innerX, innerY + row * lineH, bodyScale,
                            (viewIdx == sel) ? selCol : white, line);
            }
        }
    }

    // Right: details
    {
        const SDL_Rect clip = {detailX, innerY, detailW, innerH};
        ClipRectGuard g(renderer, clip);

        if (total > 0) {
            const ScoreEntry& e = entries[order[sel]];

            int dy = innerY;
            std::ostringstream title;
            title << "DETAILS";
            drawText5x7(renderer, detailX, dy, bodyScale + 1, white, title.str());
            dy += 18;

            // Rank by score (always meaningful since entries are stored score-sorted)
            const int rankByScore = static_cast<int>(order[sel]) + 1;

            {
                std::ostringstream ss;
                ss << "RANK: #" << rankByScore;
                if (game.scoresView() == ScoresView::Top) ss << "  (VIEW #" << (sel + 1) << ")";
                drawText5x7(renderer, detailX, dy, bodyScale, gray, ss.str());
                dy += lineH;
            }

            if (!e.timestamp.empty()) {
                std::ostringstream ss;
                ss << "WHEN: " << e.timestamp;
                drawText5x7(renderer, detailX, dy, bodyScale, gray, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "NAME: " << e.name;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            if (!e.className.empty()) {
                std::ostringstream ss;
                ss << "CLASS: " << e.className;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "RESULT: " << (e.won ? "ESCAPED ALIVE" : "DIED");
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "SCORE: " << e.score;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "DEPTH: " << e.depth << "   TURNS: " << e.turns;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "KILLS: " << e.kills << "   LVL: " << e.level << "   GOLD: " << e.gold;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            if (e.seed != 0) {
                std::ostringstream ss;
                ss << "SEED: " << e.seed << "   SLOT: " << e.slot;
                drawText5x7(renderer, detailX, dy, bodyScale, gray, ss.str());
                dy += lineH;
            }

            if (!e.cause.empty()) {
                std::ostringstream ss;
                ss << "CAUSE: " << e.cause;
                drawTextWrapped5x7(renderer, detailX, dy, bodyScale, gray, ss.str(), detailW);
            }
        }

        // Footer: scores file path (handy for backups / sharing)
        {
            const std::string path = game.defaultScoresPath();
            const std::string line = "FILE: " + path;
            drawTextWrapped5x7(renderer, detailX, innerY + innerH - lineH * 2, bodyScale, gray, line, detailW);
        }
    }
}

void Renderer::drawCodexOverlay(const Game& game) {
    const int pad = 14;
    const int panelW = winW * 9 / 10;
    const int panelH = (winH - hudH) * 9 / 10;
    const int panelX = (winW - panelW) / 2;
    const int panelY = (winH - hudH - panelH) / 2;

    SDL_Rect panel{ panelX, panelY, panelW, panelH };
    drawPanel(game, panel, 230, lastFrame);

    const Color white{240, 240, 240, 255};
    const Color gray{170, 170, 170, 255};
    const Color dark{110, 110, 110, 255};
    const Color yellow{255, 230, 120, 255};

    const int titleScale = 2;
    const int bodyScale = 1;
    const int lineH = 10 * bodyScale;

    int x = panelX + pad;
    int y = panelY + pad;

    drawText5x7(renderer, x, y, titleScale, white, "MONSTER CODEX");
    y += 20;

    // Filter / sort summary + quick controls.
    auto filterName = [&]() -> const char* {
        switch (game.codexFilter()) {
            case CodexFilter::All:    return "ALL";
            case CodexFilter::Seen:   return "SEEN";
            case CodexFilter::Killed: return "KILLED";
            default:                  return "ALL";
        }
    };
    auto sortName = [&]() -> const char* {
        switch (game.codexSort()) {
            case CodexSort::Kind:      return "KIND";
            case CodexSort::KillsDesc: return "KILLS";
            default:                   return "KIND";
        }
    };

    {
        std::string meta = "FILTER: ";
        meta += filterName();
        meta += "   SORT: ";
        meta += sortName();
        meta += "   (TAB/I SORT, LEFT/RIGHT FILTER)";
        drawText5x7(renderer, x, y, bodyScale, gray, meta);
        y += 14;
        drawText5x7(renderer, x, y, bodyScale, gray,
                    "UP/DOWN SELECT   ENTER/ESC CLOSE");
        y += 18;
    }

    // Build filtered/sorted list.
    std::vector<EntityKind> list;
    game.buildCodexList(list);

    // Layout: list column + details column.
    const int innerW = panelW - pad * 2;
    const int innerH = panelH - pad * 2 - (y - (panelY + pad));
    const int listW = innerW * 4 / 10;
    const int detailsW = innerW - listW - pad;
    const int listX = x;
    const int listY = y;
    const int detailsX = listX + listW + pad;
    const int detailsY = listY;

    const int maxLines = std::max(1, innerH / lineH);

    int sel = game.codexSelection();
    if (list.empty()) {
        sel = 0;
    } else {
        sel = clampi(sel, 0, static_cast<int>(list.size()) - 1);
    }

    // Keep selection visible by auto-scrolling.
    int first = 0;
    if (sel >= maxLines) first = sel - maxLines + 1;
    const int maxFirst = std::max(0, static_cast<int>(list.size()) - maxLines);
    first = clampi(first, 0, maxFirst);

    // Draw list.
    {
        SDL_Rect clip{listX, listY, listW, innerH};
        SDL_RenderSetClipRect(renderer, &clip);

        for (int row = 0; row < maxLines; ++row) {
            const int idx = first + row;
            if (idx >= static_cast<int>(list.size())) break;

            const EntityKind k = list[idx];
            const bool seen = game.codexHasSeen(k);
            const uint16_t kills = game.codexKills(k);

            const int rowY = listY + row * lineH;

            if (idx == sel) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 36);
                SDL_Rect r{listX, rowY - 1, listW, lineH};
                SDL_RenderFillRect(renderer, &r);
            }

            const Color nameCol = seen ? white : dark;
            const char* nm = seen ? entityKindName(k) : "??????";

            // Left: name. Right: kill count.
            const std::string killsStr = (kills > 0) ? ("K:" + std::to_string(kills)) : "";

            drawText5x7(renderer, listX + 4, rowY, bodyScale, nameCol, nm);

            if (!killsStr.empty()) {
                const int wKills = static_cast<int>(killsStr.size()) * 6 * bodyScale;
                drawText5x7(renderer, listX + listW - 4 - wKills, rowY, bodyScale,
                            seen ? gray : dark, killsStr);
            }
        }

        SDL_RenderSetClipRect(renderer, nullptr);

        // Divider.
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
        SDL_RenderDrawLine(renderer, listX + listW + pad / 2, listY,
                           listX + listW + pad / 2, listY + innerH);
    }

    // Draw details.
    {
        SDL_Rect detailsClip{detailsX, detailsY, detailsW, innerH};
        ClipRectGuard _clip(renderer, &detailsClip);

        auto dline = [&](const std::string& s, const Color& c) {
            drawText5x7(renderer, detailsX, y, bodyScale, c, s);
            y += 14;
        };

        y = detailsY;

        if (list.empty()) {
            dline("NO ENTRIES", gray);
            dline("(TRY EXPLORING TO DISCOVER MONSTERS)", dark);
            return;
        }

        const EntityKind k = list[sel];
        const bool seen = game.codexHasSeen(k);
        const uint16_t kills = game.codexKills(k);

        if (!seen) {
            dline("UNKNOWN CREATURE", gray);
            dline("YOU HAVEN'T ENCOUNTERED THIS MONSTER YET.", dark);
            dline("FILTER: ALL SHOWS PLACEHOLDERS FOR UNSEEN KINDS.", dark);
            return;
        }

        // Header.
        dline(std::string(entityKindName(k)), white);

        // Stats.
        const MonsterBaseStats base = baseMonsterStatsFor(k);
        const MonsterBaseStats scaled = monsterStatsForDepth(k, game.depth());

        dline("SEEN: YES   KILLS: " + std::to_string(kills), gray);
        dline("XP (ON KILL): " + std::to_string(game.xpFor(k)), gray);
        dline("SPEED: " + std::to_string(baseSpeedFor(k)), gray);

        dline("BASE STATS (DEPTH 1):", gray);
        dline("  HP " + std::to_string(base.hpMax) + "   ATK " + std::to_string(base.baseAtk) +
              "   DEF " + std::to_string(base.baseDef), white);

        if (game.depth() != 1) {
            dline("SCALED STATS (CURRENT DEPTH " + std::to_string(game.depth()) + "):", gray);
            dline("  HP " + std::to_string(scaled.hpMax) + "   ATK " + std::to_string(scaled.baseAtk) +
                  "   DEF " + std::to_string(scaled.baseDef), white);
        } else {
            dline("(STATS SCALE WITH DEPTH)", dark);
        }

        // Behavior / abilities.
        {
            if (base.canRanged) {
                std::string r = "RANGED: ";
                switch (base.rangedProjectile) {
                    case ProjectileKind::Arrow: r += "ARROWS"; break;
                    case ProjectileKind::Rock:  r += "ROCKS"; break;
                    case ProjectileKind::Spark: r += "SPARK"; break;
                    case ProjectileKind::Fireball: r += "FIREBALL"; break;
                    case ProjectileKind::Torch: r += "TORCH"; break;
                    default:                    r += "PROJECTILE"; break;
                }
                r += "  (R" + std::to_string(base.rangedRange) + " ATK " + std::to_string(base.rangedAtk) + ")";
                dline(r, gray);
            }

            if (base.regenChancePct > 0 && base.regenAmount > 0) {
                dline("REGEN: " + std::to_string(base.regenChancePct) + "% CHANCE / TURN (" +
                      std::to_string(base.regenAmount) + " HP)", gray);
            }

            if (base.packAI) {
                dline("BEHAVIOR: PACK HUNTER", gray);
            }
            if (base.willFlee) {
                dline("BEHAVIOR: MAY FLEE WHEN HURT", gray);
            }
        }

        // Monster-specific notes. These are intentionally short & gameplay-focused.
        {
            auto note = [&](const char* s) { dline(std::string("NOTE: ") + s, dark); };
            switch (k) {
                case EntityKind::Snake:  note("POISONOUS BITE."); break;
                case EntityKind::Spider: note("CAN WEB YOU, LIMITING MOVEMENT."); break;
                case EntityKind::Mimic:  note("DISGUISES ITSELF AS LOOT."); break;
                case EntityKind::Ghost:  note("RARE; CAN REGENERATE."); break;
                case EntityKind::Leprechaun: note("STEALS GOLD AND BLINKS AWAY."); break;
                case EntityKind::Zombie: note("SLOW UNDEAD; OFTEN RISES FROM CORPSES. IMMUNE TO POISON."); break;
                case EntityKind::Minotaur: note("BOSS-LIKE THREAT; SCALES MORE SLOWLY UNTIL DEEPER LEVELS."); break;
                case EntityKind::Shopkeeper: note("ATTACKING MAY ANGER THE SHOP."); break;
                default: break;
            }
        }
    }
}

void Renderer::drawDiscoveriesOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Color white{240, 240, 240, 255};
    const Color gray{180, 180, 180, 255};
    const Color dark{120, 120, 120, 255};

    const int pad = 18;
    const int titleScale = 2;
    const int bodyScale = 2;
    const int lineH = 14;

    const int panelW = std::min(winW - 80, 980);
    const int panelH = std::min(winH - 80, 600);
    const int px = (winW - panelW) / 2;
    const int py = (winH - panelH) / 2;
    SDL_Rect panel{px, py, panelW, panelH};

    drawPanel(game, panel, 220, lastFrame);

    int x = px + pad;
    int y = py + pad;

    drawText5x7(renderer, x, y, titleScale, white, "DISCOVERIES");
    y += 22;

    // Header: filter/sort + known count.
    const DiscoveryFilter filter = game.discoveriesFilter();
    const DiscoverySort sort = game.discoveriesSort();

    auto matches = [&](ItemKind k) -> bool {
        switch (filter) {
            case DiscoveryFilter::All:     return true;
            case DiscoveryFilter::Potions: return isPotionKind(k);
            case DiscoveryFilter::Scrolls: return isScrollKind(k);
            case DiscoveryFilter::Rings:   return isRingKind(k);
            case DiscoveryFilter::Wands:   return isWandKind(k);
            default:                       return true;
        }
    };

    int total = 0;
    int known = 0;
    for (int i = 0; i < ITEM_KIND_COUNT; ++i) {
        const ItemKind k = static_cast<ItemKind>(i);
        if (!isIdentifiableKind(k)) continue;
        if (!matches(k)) continue;
        ++total;
        if (game.discoveriesIsIdentified(k)) ++known;
    }

    {
        std::ostringstream ss;
        ss << "FILTER: " << discoveryFilterDisplayName(filter)
           << "  SORT: " << discoverySortDisplayName(sort)
           << "  KNOWN: " << known << "/" << total;
        drawText5x7(renderer, x, y, bodyScale, gray, ss.str());
    }
    y += 16;
    drawText5x7(renderer, x, y, bodyScale, dark, "LEFT/RIGHT/TAB FILTER  SHIFT+S SORT  ESC CLOSE");
    y += 18;

    // Build current list.
    std::vector<ItemKind> list;
    game.buildDiscoveryList(list);

    int sel = game.discoveriesSelection();
    if (list.empty()) sel = 0;
    else sel = clampi(sel, 0, static_cast<int>(list.size()) - 1);

    // Layout.
    const int innerW = panelW - pad * 2;
    const int innerH = (py + panelH - pad) - y;
    const int listW = std::max(260, innerW * 5 / 11);
    const int detailsW = innerW - listW - pad;
    const int listX = x;
    const int listY = y;
    const int detailsX = listX + listW + pad;
    const int detailsY = listY;
    const int maxLines = std::max(1, innerH / lineH);

    // Keep selection visible by auto-scrolling.
    int first = 0;
    if (sel >= maxLines) first = sel - maxLines + 1;
    const int maxFirst = std::max(0, static_cast<int>(list.size()) - maxLines);
    first = clampi(first, 0, maxFirst);

    // Draw list.
    {
        SDL_Rect clip{listX, listY, listW, innerH};
        SDL_RenderSetClipRect(renderer, &clip);

        for (int row = 0; row < maxLines; ++row) {
            const int idx = first + row;
            if (idx >= static_cast<int>(list.size())) break;

            const ItemKind k = list[idx];
            const bool id = game.discoveriesIsIdentified(k);
            const int rowY = listY + row * lineH;

            if (idx == sel) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 36);
                SDL_Rect r{listX, rowY - 1, listW, lineH};
                SDL_RenderFillRect(renderer, &r);
            }

            const std::string app = game.discoveryAppearanceLabel(k);
            const std::string prefix = id ? "* " : "  ";
            drawText5x7(renderer, listX + 4, rowY, bodyScale, id ? white : dark, prefix + app);
        }

        SDL_RenderSetClipRect(renderer, nullptr);

        // Divider.
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
        SDL_RenderDrawLine(renderer, listX + listW + pad / 2, listY,
                           listX + listW + pad / 2, listY + innerH);
    }

    // Draw details.
    {
        SDL_Rect detailsClip{detailsX, detailsY, detailsW, innerH};
        ClipRectGuard _clip(renderer, &detailsClip);

        int dy = detailsY;
        auto dline = [&](const std::string& s, const Color& c) {
            drawText5x7(renderer, detailsX, dy, bodyScale, c, s);
            dy += 14;
        };

        if (list.empty()) {
            dline("NO IDENTIFIABLE ITEMS", gray);
            dline("(PICK UP POTIONS/SCROLLS/RINGS/WANDS TO START)", dark);
            return;
        }

        const ItemKind k = list[sel];
        const bool id = game.discoveriesIsIdentified(k);
        const std::string app = game.discoveryAppearanceLabel(k);
        const std::string trueName = itemDisplayNameSingle(k);

        auto category = [&]() -> const char* {
            if (isPotionKind(k)) return "POTION";
            if (isScrollKind(k)) return "SCROLL";
            if (isRingKind(k))   return "RING";
            if (isWandKind(k))   return "WAND";
            return "ITEM";
        };

        // A lightweight, UI-only summary of the known effect.
        struct Blurb { const char* a; const char* b; const char* c; };
        auto blurbFor = [&](ItemKind kk) -> Blurb {
            switch (kk) {
                // Potions
                case ItemKind::PotionHealing:       return {"HEALS YOU.", "", ""};
                case ItemKind::PotionStrength:      return {"CHANGES MIGHT TALENT.", "(BLESSED STRONGER, CURSED WEAKER)", ""};
                case ItemKind::PotionAntidote:      return {"CURES POISON.", "", ""};
                case ItemKind::PotionRegeneration:  return {"GRANTS REGENERATION.", "", ""};
                case ItemKind::PotionShielding:     return {"GRANTS A TEMPORARY SHIELD.", "", ""};
                case ItemKind::PotionHaste:         return {"GRANTS HASTE.", "", ""};
                case ItemKind::PotionVision:        return {"GRANTS SHARPENED VISION.", "(INCREASES FOV TEMPORARILY)", ""};
                case ItemKind::PotionInvisibility:  return {"MAKES YOU INVISIBLE.", "", ""};
                case ItemKind::PotionClarity:       return {"CURES CONFUSION.", "(ALSO ENDS HALLUCINATIONS)", ""};
                case ItemKind::PotionLevitation:    return {"GRANTS LEVITATION.", "(FLOAT OVER TRAPS/CHASMS)", ""};
                case ItemKind::PotionHallucination: return {"CAUSES HALLUCINATIONS.", "BLESSED: SHORT + VISION.", "CURSED: LONG + CONFUSION."};

                // Scrolls
                case ItemKind::ScrollTeleport:      return {"TELEPORTS YOU.", "CONFUSED: SHORT-RANGE BLINK.", ""};
                case ItemKind::ScrollMapping:       return {"REVEALS THE MAP.", "CONFUSED: CAUSES AMNESIA.", ""};
                case ItemKind::ScrollEnchantWeapon: return {"ENCHANTS YOUR WEAPON.", "", ""};
                case ItemKind::ScrollEnchantArmor:  return {"ENCHANTS YOUR ARMOR.", "", ""};
                case ItemKind::ScrollIdentify:      return {"IDENTIFIES AN UNKNOWN ITEM.", "", ""};
                case ItemKind::ScrollDetectTraps:   return {"DETECTS TRAPS NEARBY.", "", ""};
                case ItemKind::ScrollDetectSecrets: return {"REVEALS SECRET DOORS.", "", ""};
                case ItemKind::ScrollKnock:         return {"UNLOCKS DOORS/CONTAINERS.", "", ""};
                case ItemKind::ScrollRemoveCurse:   return {"REMOVES CURSES (AND CAN BLESS).", "", ""};
                case ItemKind::ScrollConfusion:     return {"CAUSES CONFUSION AROUND YOU.", "", ""};
                case ItemKind::ScrollFear:          return {"CAUSES FEAR AROUND YOU.", "", ""};
                case ItemKind::ScrollEarth:         return {"CREATES BOULDERS.", "", ""};
                case ItemKind::ScrollTaming:        return {"TAMES A CREATURE.", "", ""};

                // Rings
                case ItemKind::RingMight:           return {"PASSIVE MIGHT BONUS.", "", ""};
                case ItemKind::RingAgility:         return {"PASSIVE AGILITY BONUS.", "", ""};
                case ItemKind::RingFocus:           return {"PASSIVE FOCUS BONUS.", "", ""};
                case ItemKind::RingProtection:      return {"PASSIVE DEFENSE BONUS.", "", ""};

                // Wands
                case ItemKind::WandSparks:          return {"FIRES SPARKS.", "(RANGED, USES CHARGES)", ""};
                case ItemKind::WandDigging:         return {"DIGS THROUGH WALLS.", "(RANGED, USES CHARGES)", ""};
                case ItemKind::WandFireball:        return {"FIRES AN EXPLOSIVE FIREBALL.", "", ""};

                default:
                    return {"", "", ""};
            }
        };

        // Header.
        dline(id ? trueName : "UNKNOWN ITEM", white);
        dline(std::string("CATEGORY: ") + category(), gray);
        dline(std::string("APPEARANCE: ") + app, gray);
        dline(std::string("IDENTIFIED: ") + (id ? "YES" : "NO"), gray);

        if (!id) {
            dline("", gray);
            dline("USE IT TO IDENTIFY... OR READ A", dark);
            dline("SCROLL OF IDENTIFY FOR SAFETY.", dark);
            return;
        }

        const Blurb b = blurbFor(k);
        if (b.a && b.a[0]) {
            dline("", gray);
            dline(b.a, white);
            if (b.b && b.b[0]) dline(b.b, dark);
            if (b.c && b.c[0]) dline(b.c, dark);
        }

        // If the item has an underlying stat modifier, show it.
        if (isRingKind(k)) {
            const ItemDef& d = itemDef(k);
            std::ostringstream ss;
            ss << "BONUSES: ";
            bool any = false;
            if (d.modMight != 0)   { ss << "MIGHT " << d.modMight << "  "; any = true; }
            if (d.modAgility != 0) { ss << "AGI " << d.modAgility << "  "; any = true; }
            if (d.modVigor != 0)   { ss << "VIG " << d.modVigor << "  "; any = true; }
            if (d.modFocus != 0)   { ss << "FOC " << d.modFocus << "  "; any = true; }
            if (d.defense != 0)    { ss << "DEF " << d.defense; any = true; }
            if (!any) ss << "(NONE)";
            dline(ss.str(), gray);
        }
    }
}


void Renderer::drawMessageHistoryOverlay(const Game& game) {
    ensureUIAssets(game);

    const Color white{255,255,255,255};
    const Color gray{180,180,180,255};

    // Center panel
    const int panelW = winW * 9 / 10;
    const int panelH = (winH - hudH) * 9 / 10;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - hudH - panelH) / 2;

    SDL_Rect panel { x0, y0, panelW, panelH };
    drawPanel(game, panel, 230, lastFrame);

    const int pad = 14;
    int y = y0 + pad;

    drawText5x7(renderer, x0 + pad, y, 2, white, "MESSAGE HISTORY");
    y += 22;

    {
        std::stringstream ss;
        ss << "FILTER: " << messageFilterDisplayName(game.messageHistoryFilter());
        if (!game.messageHistorySearch().empty()) {
            ss << "  SEARCH: \"" << game.messageHistorySearch() << "\"";
        }
        if (game.isMessageHistorySearchMode()) {
            ss << "  (TYPE)";
        }
        drawText5x7(renderer, x0 + pad, y, 2, gray, ss.str());
        y += 20;
    }

    drawText5x7(renderer, x0 + pad, y, 1, gray, "UP/DOWN scroll  LEFT/RIGHT filter  PGUP/PGDN scroll  / search  CTRL+L clear  ESC close");
    y += 18;

    // Build filtered view.
    const auto& msgs = game.messages();
    std::vector<int> idx;
    idx.reserve(msgs.size());

    auto icontainsAscii = [](const std::string& haystack, const std::string& needle) -> bool {
        if (needle.empty()) return true;
        auto lower = [](unsigned char c) -> unsigned char {
            if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c - 'A' + 'a');
            return c;
        };

        const size_t n = needle.size();
        const size_t m = haystack.size();
        if (n > m) return false;

        for (size_t i = 0; i + n <= m; ++i) {
            bool ok = true;
            for (size_t j = 0; j < n; ++j) {
                if (lower(static_cast<unsigned char>(haystack[i + j])) != lower(static_cast<unsigned char>(needle[j]))) {
                    ok = false;
                    break;
                }
            }
            if (ok) return true;
        }
        return false;
    };

    const MessageFilter filter = game.messageHistoryFilter();
    const std::string& needle = game.messageHistorySearch();
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if (!messageFilterMatches(filter, m.kind)) continue;
        if (!needle.empty() && !icontainsAscii(m.text, needle)) continue;
        idx.push_back(static_cast<int>(i));
    }

    int scroll = game.messageHistoryScroll();
    const int maxScroll = std::max(0, static_cast<int>(idx.size()) - 1);
    scroll = std::max(0, std::min(scroll, maxScroll));

    // Text area
    const int scale = 2;
    const int charW = 6 * scale;
    const int lineH = 16;
    const int textTop = y;
    const int footerH = 18;
    const int textBottom = y0 + panelH - pad - footerH;

    const int availH = std::max(0, textBottom - textTop);
    const int maxLines = std::max(1, availH / lineH);

    const int start = std::max(0, static_cast<int>(idx.size()) - maxLines - scroll);
    const int end = std::min(static_cast<int>(idx.size()), start + maxLines);

    auto kindColor = [&](MessageKind k) -> Color {
        switch (k) {
            case MessageKind::Combat:       return Color{255,230,120,255};
            case MessageKind::Loot:         return Color{120,255,120,255};
            case MessageKind::System:       return Color{160,200,255,255};
            case MessageKind::Warning:      return Color{255,120,120,255};
            case MessageKind::ImportantMsg: return Color{255,170,80,255};
            case MessageKind::Success:      return Color{120,255,255,255};
            case MessageKind::Info:
            default:                        return Color{255,255,255,255};
        }
    };

    auto fitToChars = [](const std::string& s, int maxChars) -> std::string {
        if (maxChars <= 0) return "";
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, static_cast<size_t>(maxChars));
        return s.substr(0, static_cast<size_t>(maxChars - 3)) + "...";
    };

    const int maxChars = (panelW - 2 * pad) / charW;

    if (idx.empty()) {
        drawText5x7(renderer, x0 + pad, y + 10, 2, gray, "NO MESSAGES MATCH.");
    } else {
        int yy = y;
        for (int row = start; row < end; ++row) {
            const auto& m = msgs[idx[row]];
            const Color c = kindColor(m.kind);

            std::string prefix = "D" + std::to_string(m.depth) + " T" + std::to_string(m.turn) + " ";
            std::string body = m.text;
            if (m.repeat > 1) {
                body += " (x" + std::to_string(m.repeat) + ")";
            }

            const int prefixChars = static_cast<int>(prefix.size());
            const int bodyChars = std::max(0, maxChars - prefixChars);

            drawText5x7(renderer, x0 + pad, yy, scale, gray, fitToChars(prefix, prefixChars));
            drawText5x7(renderer, x0 + pad + prefixChars * charW, yy, scale, c, fitToChars(body, bodyChars));

            yy += lineH;
        }
    }

    // Footer status
    {
        std::stringstream ss;
        ss << "SHOWING " << idx.size() << "/" << msgs.size();
        if (maxScroll > 0) ss << "  SCROLL " << scroll << "/" << maxScroll;
        drawText5x7(renderer, x0 + pad, y0 + panelH - pad - 12, 1, gray, ss.str());
    }
}

void Renderer::drawTargetingOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const bool iso = (viewMode_ == ViewMode::Isometric);

    const auto& linePts = game.targetingLine();
    Vec2i cursor = game.targetingCursor();
    bool ok = game.targetingIsValid();

    // Draw LOS line tiles (excluding player tile)
    SDL_SetRenderDrawColor(renderer, ok ? 0 : 255, ok ? 255 : 0, 0, 80);
    for (size_t i = 1; i < linePts.size(); ++i) {
        Vec2i p = linePts[i];
        SDL_Rect base = mapTileDst(p.x, p.y);
        if (iso) {
            const int cx = base.x + base.w / 2;
            const int cy = base.y + base.h / 2;
            const int hw = std::max(1, base.w / 8);
            const int hh = std::max(1, base.h / 4);
            fillIsoDiamond(renderer, cx, cy, hw, hh);
        } else {
            SDL_Rect r{ base.x + tile / 4, base.y + tile / 4, tile / 2, tile / 2 };
            SDL_RenderFillRect(renderer, &r);
        }
    }

    // Crosshair on cursor
    SDL_Rect c = mapTileDst(cursor.x, cursor.y);
    SDL_SetRenderDrawColor(renderer, ok ? 0 : 255, ok ? 255 : 0, 0, 200);
    if (iso) {
        drawIsoDiamondOutline(renderer, c);
        SDL_SetRenderDrawColor(renderer, ok ? 0 : 255, ok ? 255 : 0, 0, 110);
        drawIsoDiamondCross(renderer, c);
    } else {
        SDL_RenderDrawRect(renderer, &c);
    }

    // Small label near bottom HUD
    const int scale = 2;
    const Color yellow{ 255, 230, 120, 255 };
    const int hudTop = winH - hudH;
    auto fitToChars = [](const std::string& s, int maxChars) -> std::string {
        if (maxChars <= 0) return std::string();
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, static_cast<size_t>(maxChars));
        return s.substr(0, static_cast<size_t>(maxChars - 3)) + "...";
    };

    const std::string info = game.targetingInfoText();
    const std::string preview = game.targetingCombatPreviewText();
    const std::string status = game.targetingStatusText();

    std::string label;
    if (!info.empty()) label = "TARGET: " + info;
    else label = "TARGET:";

    if (!preview.empty()) {
        label += " | " + preview;
    }

    if (ok) {
        label += " | ENTER FIRE  ESC CANCEL  TAB NEXT  SHIFT+TAB PREV";
    } else {
        label += " | " + (status.empty() ? std::string("NO CLEAR SHOT") : status);
    }

    const int charW = 6 * scale;
    const int maxChars = (winW - 20) / std::max(1, charW);
    drawText5x7(renderer, 10, hudTop - 18, scale, yellow, fitToChars(label, maxChars));
}

void Renderer::drawLookOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const bool iso = (viewMode_ == ViewMode::Isometric);

    const Dungeon& d = game.dungeon();
    Vec2i cursor = game.lookCursor();
    if (!d.inBounds(cursor.x, cursor.y)) return;

    // Cursor box
    SDL_Rect c = mapTileDst(cursor.x, cursor.y);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
    if (iso) {
        drawIsoDiamondOutline(renderer, c);
    } else {
        SDL_RenderDrawRect(renderer, &c);
    }

    // Crosshair lines (subtle)
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 90);
    if (iso) {
        drawIsoDiamondCross(renderer, c);
    } else {
        SDL_RenderDrawLine(renderer, c.x, c.y + c.h / 2, c.x + c.w, c.y + c.h / 2);
        SDL_RenderDrawLine(renderer, c.x + c.w / 2, c.y, c.x + c.w / 2, c.y + c.h);
    }

    // Label near bottom of map
    const int scale = 2;
    const Color yellow{ 255, 230, 120, 255 };
    const int hudTop = winH - hudH;

    if (!game.isCommandOpen()) {
        std::string s = game.lookInfoText();
        if (s.empty()) s = "LOOK";
        drawText5x7(renderer, 10, hudTop - 18, scale, yellow, s);
    }
}
