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
            if (gi.item.shopPrice != 0) continue;
            if (gi.item.shopDepth != 0) continue;
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

void Game::dropGroundItemItem(Vec2i pos, Item it) {
    if (isStackable(it.kind)) it.count = std::max(1, it.count);
    else it.count = 1;

    // Merge into an existing matching stack on the same tile when possible.
    if (isStackable(it.kind)) {
        for (auto& gi : ground) {
            if (gi.pos != pos) continue;
            if (gi.item.kind != it.kind) continue;
            if (gi.item.enchant != it.enchant) continue;
            if (gi.item.charges != it.charges) continue;
            if (gi.item.buc != it.buc) continue;
            if (gi.item.shopPrice != it.shopPrice) continue;
            if (gi.item.shopDepth != it.shopDepth) continue;

            gi.item.count += it.count;
            return;
        }
    }

    it.id = nextItemId++;
    it.spriteSeed = rng.nextU32();

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

    // Always keep caches sized correctly (even when lighting is "off") so the renderer
    // can safely query light color without special-casing.
    lightMap_.assign(n, 255);
    lightColorMap_.assign(n, Color{ 255, 255, 255, 255 });

    if (!darknessActive()) {
        // Treat early depths as fully lit for accessibility.
        return;
    }

    // Darkness mode: build a per-tile brightness map (for gameplay) + a per-tile RGB light
    // modulation map (for rendering).
    lightMap_.assign(n, 0);
    lightColorMap_.assign(n, Color{ 0, 0, 0, 255 });

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * dung.width + x); };

    auto setLight = [&](int x, int y, uint8_t v) {
        if (!dung.inBounds(x, y)) return;
        const size_t i = idx(x, y);
        if (i >= lightMap_.size()) return;
        if (lightMap_[i] < v) lightMap_[i] = v;
    };

    auto setAmbient = [&](int x, int y, uint8_t amb, Color tint) {
        if (!dung.inBounds(x, y)) return;
        const size_t i = idx(x, y);
        if (i >= lightMap_.size() || i >= lightColorMap_.size()) return;

        if (lightMap_[i] < amb) lightMap_[i] = amb;

        // Encode ambient color as "already intensity-scaled" modulation.
        const auto scale = [&](uint8_t c) -> uint8_t {
            const int v = (static_cast<int>(amb) * static_cast<int>(c)) / 255;
            return static_cast<uint8_t>(clampi(v, 0, 255));
        };

        const Color ambC{ scale(tint.r), scale(tint.g), scale(tint.b), 255 };
        Color& dst = lightColorMap_[i];
        dst.r = std::max(dst.r, ambC.r);
        dst.g = std::max(dst.g, ambC.g);
        dst.b = std::max(dst.b, ambC.b);
    };

    auto addLight = [&](int x, int y, uint8_t b, Color tint) {
        if (!dung.inBounds(x, y)) return;
        const size_t i = idx(x, y);
        if (i >= lightMap_.size() || i >= lightColorMap_.size()) return;

        if (lightMap_[i] < b) lightMap_[i] = b;

        // Additive RGB lighting. Each channel is scaled by intensity and saturates at 255.
        auto addChan = [&](uint8_t& dst, uint8_t srcChan) {
            const int add = (static_cast<int>(b) * static_cast<int>(srcChan)) / 255;
            const int v = static_cast<int>(dst) + add;
            dst = static_cast<uint8_t>((v > 255) ? 255 : v);
        };

        Color& dst = lightColorMap_[i];
        addChan(dst.r, tint.r);
        addChan(dst.g, tint.g);
        addChan(dst.b, tint.b);
    };

    // Ambient room light: rooms are softly lit, corridors/caverns are dark.
    for (const Room& r : dung.rooms) {
        uint8_t amb = 140;
        Color ambTint{ 255, 246, 236, 255 }; // warm stone by default
        switch (r.type) {
            case RoomType::Shrine:
                amb = 190;
                ambTint = Color{ 206, 222, 255, 255 }; // cool/holy
                break;
            case RoomType::Treasure:
                amb = 170;
                ambTint = Color{ 255, 238, 200, 255 }; // warm/golden
                break;
            case RoomType::Vault:
                amb = 175;
                ambTint = Color{ 224, 232, 255, 255 }; // cold steel
                break;
            case RoomType::Secret:
                amb = 120;
                ambTint = Color{ 220, 206, 190, 255 }; // dusty
                break;
            case RoomType::Shop:
                amb = 175;
                ambTint = Color{ 255, 232, 205, 255 }; // cozy
                break;
            default:
                amb = 140;
                ambTint = Color{ 255, 246, 236, 255 };
                break;
        }

        for (int y = r.y; y < r.y + r.h; ++y) {
            for (int x = r.x; x < r.x + r.w; ++x) {
                setAmbient(x, y, amb, ambTint);
            }
        }
    }

    struct LightSource {
        Vec2i pos;
        int radius;
        uint8_t intensity;
        Color tint;
    };

    std::vector<LightSource> sources;

    // Player light sources (carried lit torches).
    bool playerHasTorch = false;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::TorchLit && it.charges > 0) {
            playerHasTorch = true;
            break;
        }
    }
    if (playerHasTorch) {
        // Warm torchlight
        sources.push_back({ player().pos, 8, 255, Color{ 255, 208, 168, 255 } });
    }

    // Ground light sources (dropped lit torches).
    for (const auto& gi : ground) {
        if (gi.item.kind == ItemKind::TorchLit && gi.item.charges > 0) {
            sources.push_back({ gi.pos, 6, 230, Color{ 255, 196, 152, 255 } });
        }
    }

    // Apply each source using shadowcasting LOS from the source.
    std::vector<uint8_t> mask;
    for (const auto& s : sources) {
        dung.computeFovMask(s.pos.x, s.pos.y, s.radius, mask);
        if (mask.size() != lightMap_.size()) continue;

        const int r  = std::max(1, s.radius);
        const int r2 = r * r;

        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                const size_t i = idx(x, y);
                if (!mask[i]) continue;

                const int dx = x - s.pos.x;
                const int dy = y - s.pos.y;
                const int d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;

                // Smooth quadratic falloff (0 at edge) for nicer, round torchlight.
                const float t = static_cast<float>(d2) / static_cast<float>(r2);
                float atten = 1.0f - t;
                atten = atten * atten;

                int b = static_cast<int>(static_cast<float>(s.intensity) * atten + 0.5f);
                b = clampi(b, 0, 255);

                addLight(x, y, static_cast<uint8_t>(b), s.tint);
            }
        }
    }

    // If a tile has brightness but ended up with no RGB tint (should be rare),
    // fall back to grayscale to avoid a "black light" edge case.
    for (size_t i = 0; i < lightMap_.size() && i < lightColorMap_.size(); ++i) {
        if (lightMap_[i] == 0) continue;
        Color& c = lightColorMap_[i];
        if (c.r == 0 && c.g == 0 && c.b == 0) {
            c.r = lightMap_[i];
            c.g = lightMap_[i];
            c.b = lightMap_[i];
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

uint8_t Game::confusionGasAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0u;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= confusionGas_.size()) return 0u;
    return confusionGas_[i];
}
