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
    if (it.spriteSeed == 0) it.spriteSeed = rng.nextU32();

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

    // Fire field (tile-based hazard) as a dynamic warm light source.
    // We keep this bounded for performance: if there are many burning tiles, only the
    // strongest few contribute as LOS-aware sources (the renderer still draws all flames).
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (fireField_.size() == expect && expect > 0) {
            struct FireSrc { int x; int y; uint8_t s; };
            std::vector<FireSrc> fires;
            fires.reserve(24);

            for (int y = 0; y < dung.height; ++y) {
                for (int x = 0; x < dung.width; ++x) {
                    const uint8_t f = fireField_[static_cast<size_t>(y * dung.width + x)];
                    if (f == 0u) continue;
                    if (!dung.isWalkable(x, y)) continue;
                    fires.push_back({ x, y, f });
                }
            }

            constexpr size_t MAX_FIRE_SOURCES = 24;
            if (fires.size() > MAX_FIRE_SOURCES) {
                std::nth_element(fires.begin(), fires.begin() + static_cast<std::ptrdiff_t>(MAX_FIRE_SOURCES), fires.end(),
                                 [](const FireSrc& a, const FireSrc& b) { return a.s > b.s; });
                fires.resize(MAX_FIRE_SOURCES);
            }

            for (const auto& f : fires) {
                int radius = 2;
                if (f.s >= 8u) radius = 3;
                if (f.s >= 12u) radius = 4;

                const int b = std::min(255, 110 + static_cast<int>(f.s) * 14);
                sources.push_back({ Vec2i{f.x, f.y}, radius, static_cast<uint8_t>(b), Color{ 255, 170, 110, 255 } });
            }
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
    Entity& p = playerMut();
    int radius = 9;
    if (p.effects.visionTurns > 0) radius += 3;

    recomputeLightMap();

    if (!darknessActive()) {
        dung.computeFov(p.pos.x, p.pos.y, radius, true);
    } else {
        // If darkness is active, compute FOV without auto-explore marking so we can
        // apply a light-threshold filter first.
        dung.computeFov(p.pos.x, p.pos.y, radius, false);

        // Then apply a light threshold: only tiles lit above a minimum are visible.
        const float minLight = 0.35f;
        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                if (!dung.at(x, y).visible) continue;

                // lightMap_ stores 0..255 brightness per-tile.
                const size_t i = static_cast<size_t>(y * dung.width + x);
                const float lit = (i < lightMap_.size()) ? (static_cast<float>(lightMap_[i]) / 255.0f) : 0.0f;
                if (lit < minLight) {
                    dung.at(x, y).visible = false;
                }
            }
        }

        // Mark explored tiles after darkness filtering.
        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                if (dung.at(x, y).visible) {
                    dung.at(x, y).explored = true;
                }
            }
        }
    }

    // Monster codex: any monster kind currently visible to the player is considered
    // "seen" for this run. (Idempotent: we only store a boolean.)
    for (const Entity& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
        if (!dung.at(e.pos.x, e.pos.y).visible) continue;
        const size_t idx = static_cast<size_t>(e.kind);
        if (idx < codexSeen_.size()) {
            codexSeen_[idx] = 1;
        }
    }
}

uint8_t Game::confusionGasAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0u;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= confusionGas_.size()) return 0u;
    return confusionGas_[i];
}

uint8_t Game::fireAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0u;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= fireField_.size()) return 0u;
    return fireField_[i];
}
void Game::updateScentMap() {
    const int W = dung.width;
    const int H = dung.height;
    const size_t n = static_cast<size_t>(W * H);
    if (n == 0) return;

    if (scentField_.size() != n) {
        scentField_.assign(n, 0u);
    }

    auto idx = [&](int x, int y) -> size_t { return static_cast<size_t>(y * W + x); };

    // --- Tunables ---
    // How fast scent fades everywhere each player turn.
    constexpr uint8_t DECAY = 2u;
    // How much scent is lost when it spreads across one tile of distance.
    constexpr uint8_t SPREAD_DROP = 14u;
    // How strong a fresh scent deposit is at the player position.
    constexpr uint8_t DEPOSIT = 255u;

    // Global decay.
    for (size_t i = 0; i < n; ++i) {
        const uint8_t v = scentField_[i];
        scentField_[i] = (v > DECAY) ? static_cast<uint8_t>(v - DECAY) : 0u;
    }

    // Deposit at the player's current tile (even if the player didn't move this turn).
    const Entity& p = player();
    if (dung.inBounds(p.pos.x, p.pos.y)) {
        scentField_[idx(p.pos.x, p.pos.y)] = std::max(scentField_[idx(p.pos.x, p.pos.y)], DEPOSIT);
    }

    // Spread along walkable tiles so the "trail" forms a gradient that can be followed around corners.
    // This is intentionally cheap: one relaxation pass per player turn.
    std::vector<uint8_t> next = scentField_;

    auto passableForScent = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        // Scent travels through walkable terrain (floors, open doors, stairs).
        // Closed/locked/secret doors and walls block it.
        return dung.isWalkable(x, y);
    };

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = idx(x, y);

            if (!passableForScent(x, y)) {
                // Keep non-walkable tiles scent-free so it can't "leak" through walls.
                next[i] = 0u;
                continue;
            }

            uint8_t bestN = 0u;
            const int dirs4[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
            for (const auto& d : dirs4) {
                const int nx = x + d[0];
                const int ny = y + d[1];
                if (!passableForScent(nx, ny)) continue;
                const uint8_t nv = scentField_[idx(nx, ny)];
                if (nv > bestN) bestN = nv;
            }

            const uint8_t spread = (bestN > SPREAD_DROP) ? static_cast<uint8_t>(bestN - SPREAD_DROP) : 0u;
            if (spread > next[i]) next[i] = spread;
        }
    }

    scentField_.swap(next);
}

uint8_t Game::scentAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0u;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= scentField_.size()) return 0u;
    return scentField_[i];
}
