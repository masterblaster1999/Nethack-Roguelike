#include "game_internal.hpp"

void Game::dropGroundItem(Vec2i pos, ItemKind k, int count, int enchant) {
    count = std::max(1, count);

    // Merge into an existing stack on the same tile when possible.
    // (Only for stackables and when metadata matches.)
    if (isStackable(k)) {
        for (auto& gi : ground) {
            if (gi.pos != pos) continue;
            if (gi.item.kind != k) continue;
            // Only merge stacks when all metadata matches (e.g., enchant / charges / BUC).
            if (gi.item.enchant != enchant) continue;
            if (gi.item.charges != 0) continue;
            if (gi.item.buc != 0) continue;
            gi.item.count += count;
            return;
        }
    }

    Item it;
    it.id = nextItemId++;
    it.kind = k;
    it.count = count;
    it.enchant = enchant;
    it.spriteSeed = rng.nextU32();
    const ItemDef& d = itemDef(k);
    if (d.maxCharges > 0) it.charges = d.maxCharges;

    GroundItem gi;
    gi.item = it;
    gi.pos = pos;
    ground.push_back(gi);
}

std::vector<Vec2i> Game::bresenhamLine(Vec2i a, Vec2i b) {
    std::vector<Vec2i> pts;
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;

    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        pts.push_back({x0, y0});
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
        if (pts.size() > 512) break;
    }
    return pts;
}

void Game::recomputeLightMap() {
    const size_t n = static_cast<size_t>(dung.width * dung.height);
    lightMap_.assign(n, 255);

    if (!darknessActive()) {
        // Treat early depths as fully lit for accessibility.
        return;
    }

    lightMap_.assign(n, 0);

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * dung.width + x); };
    auto setLight = [&](int x, int y, uint8_t v) {
        if (!dung.inBounds(x, y)) return;
        const size_t i = idx(x, y);
        if (i >= lightMap_.size()) return;
        if (lightMap_[i] < v) lightMap_[i] = v;
    };

    // Ambient room light: rooms are lit, corridors/caverns are dark.
    for (const Room& r : dung.rooms) {
        uint8_t amb = 140;
        switch (r.type) {
            case RoomType::Shrine:   amb = 190; break;
            case RoomType::Treasure: amb = 170; break;
            case RoomType::Vault:    amb = 175; break;
            case RoomType::Secret:   amb = 120; break;
            default:                 amb = 140; break;
        }

        for (int y = r.y; y < r.y + r.h; ++y) {
            for (int x = r.x; x < r.x + r.w; ++x) {
                setLight(x, y, amb);
            }
        }
    }

    struct LightSource {
        Vec2i pos;
        int radius;
        uint8_t intensity;
    };

    std::vector<LightSource> sources;
    sources.reserve(16);

    // Player-carried light source (lit torch).
    bool playerHasTorch = false;
    for (const Item& it : inv) {
        if (it.kind == ItemKind::TorchLit && it.charges > 0) {
            playerHasTorch = true;
            break;
        }
    }
    if (playerHasTorch) {
        sources.push_back({ player().pos, 8, 255 });
    }

    // Ground light sources (dropped lit torches).
    for (const auto& gi : ground) {
        if (gi.item.kind == ItemKind::TorchLit && gi.item.charges > 0) {
            sources.push_back({ gi.pos, 6, 230 });
        }
    }

    // Apply each source using shadowcasting LOS from the source.
    std::vector<uint8_t> mask;
    for (const auto& s : sources) {
        dung.computeFovMask(s.pos.x, s.pos.y, s.radius, mask);
        if (mask.size() != lightMap_.size()) continue;

        const int falloff = std::max(1, static_cast<int>(s.intensity) / (s.radius + 1));

        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                const size_t i = idx(x, y);
                if (!mask[i]) continue;

                const int dist = std::max(std::abs(x - s.pos.x), std::abs(y - s.pos.y));
                if (dist > s.radius) continue;

                int b = static_cast<int>(s.intensity) - dist * falloff;
                b = clampi(b, 0, 255);

                if (lightMap_[i] < static_cast<uint8_t>(b)) {
                    lightMap_[i] = static_cast<uint8_t>(b);
                }
            }
        }
    }
}

void Game::recomputeFov() {
    Entity& p = playerMut();    int radius = 9;
    if (p.effects.visionTurns > 0) radius += 3;

    // Lighting is cached separately from FOV; keep it current.
    recomputeLightMap();

    if (!darknessActive()) {
        // Classic behavior (fully lit).
        dung.computeFov(p.pos.x, p.pos.y, radius, true);
        return;
    }

    // Compute raw LOS without auto-exploring; we'll apply darkness filtering first.
    dung.computeFov(p.pos.x, p.pos.y, radius, false);

    constexpr int kDarkVisionRadius = 2;

    for (int y = 0; y < dung.height; ++y) {
        for (int x = 0; x < dung.width; ++x) {
            Tile& t = dung.at(x, y);
            if (!t.visible) continue;

            const int dist = std::max(std::abs(x - p.pos.x), std::abs(y - p.pos.y));
            if (dist <= kDarkVisionRadius) continue;

            // Beyond "feel around" range, you only see lit tiles.
            if (tileLightLevel(x, y) == 0) {
                t.visible = false;
            }
        }
    }

    // Mark explored tiles only after darkness filtering.
    for (int y = 0; y < dung.height; ++y) {
        for (int x = 0; x < dung.width; ++x) {
            Tile& t = dung.at(x, y);
            if (t.visible) t.explored = true;
        }
    }
}

