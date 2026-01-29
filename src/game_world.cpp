#include "game_internal.hpp"

#include "scent_field.hpp"

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
            case RoomType::Armory:
                amb = 165;
                ambTint = Color{ 234, 240, 255, 255 }; // cool steel
                break;
            case RoomType::Library:
                amb = 160;
                ambTint = Color{ 255, 242, 220, 255 }; // parchment/candles
                break;
            case RoomType::Laboratory:
                amb = 155;
                ambTint = Color{ 220, 255, 236, 255 }; // odd green
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

    // Monster / NPC light sources (carried lit torches).
    // These are intentionally a bit dimmer than the player torch so enemies that
    // bring light into corridors feel readable without totally erasing the dark.
    {
        int count = 0;
        for (const auto& e : ents) {
            if (e.hp <= 0) continue;
            if (!dung.inBounds(e.pos.x, e.pos.y)) continue;

            const Item& pc = e.pocketConsumable;
            if (pc.id == 0 || pc.count <= 0) continue;
            if (pc.kind != ItemKind::TorchLit || pc.charges <= 0) continue;

            sources.push_back({ e.pos, 6, 220, Color{ 255, 196, 152, 255 } });
            if (++count >= 24) break; // hard cap for perf
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


    // Burning creatures act as small moving light sources.
    // This helps fire-based attacks/egos feel impactful in darkness and makes burning monsters trackable.
    {
        struct BurnSrc { Vec2i pos; int turns; };
        std::vector<BurnSrc> burns;
        burns.reserve(24);

        for (const auto& e : ents) {
            if (e.hp <= 0) continue;
            if (e.effects.burnTurns <= 0) continue;
            if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
            burns.push_back({ e.pos, e.effects.burnTurns });
        }

        constexpr size_t MAX_BURN_SOURCES = 24;
        if (burns.size() > MAX_BURN_SOURCES) {
            std::nth_element(burns.begin(), burns.begin() + static_cast<std::ptrdiff_t>(MAX_BURN_SOURCES), burns.end(),
                             [](const BurnSrc& a, const BurnSrc& b) { return a.turns > b.turns; });
            burns.resize(MAX_BURN_SOURCES);
        }

        for (const auto& b : burns) {
            // Scale light with remaining burn duration.
            int radius = 2 + std::min(2, b.turns / 3);
            radius = clampi(radius, 2, 4);

            int intensity = 120 + b.turns * 18;
            intensity = clampi(intensity, 120, 255);

            sources.push_back({ b.pos, radius, static_cast<uint8_t>(intensity), Color{ 255, 175, 120, 255 } });
        }
    }

    // Flaming ego weapons (rare loot) emit a steady glow.
    // This gives them a small utility bump on dark floors without requiring torches.
    {
        struct EgoSrc { Vec2i pos; uint8_t power; };
        std::vector<EgoSrc> egos;
        egos.reserve(16);

        // Player (equipped melee).
        if (const Item* w = equippedMelee()) {
            if (w->ego == ItemEgo::Flaming) {
                egos.push_back({ player().pos, 200u });
            }
        }

        // Monsters (equipped melee gear).
        for (const auto& e : ents) {
            if (e.id == playerId_) continue;
            if (e.hp <= 0) continue;
            if (e.gearMelee.id == 0) continue;
            if (e.gearMelee.ego != ItemEgo::Flaming) continue;
            if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
            egos.push_back({ e.pos, 190u });
        }

        constexpr size_t MAX_EGO_SOURCES = 16;
        if (egos.size() > MAX_EGO_SOURCES) {
            std::nth_element(egos.begin(), egos.begin() + static_cast<std::ptrdiff_t>(MAX_EGO_SOURCES), egos.end(),
                             [](const EgoSrc& a, const EgoSrc& b) { return a.power > b.power; });
            egos.resize(MAX_EGO_SOURCES);
        }

        for (const auto& es : egos) {
            sources.push_back({ es.pos, 3, es.power, Color{ 255, 150, 100, 255 } });
        }
    }



    // Procedural bioluminescent terrain (lichen/crystal) emitters.
    // These are cosmetic light sources derived from the deterministic per-level
    // biolum cache (computed alongside terrain materials). The intent is to create
    // occasional dim navigation landmarks in darkness without replacing torches.
    {
        // Ensure terrain caches exist (biolum is computed in Dungeon::ensureMaterials).
        dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());

        struct GlowCand {
            Vec2i pos;
            uint8_t glow;
            TerrainMaterial mat;
            uint32_t h;
        };

        std::vector<GlowCand> cands;
        cands.reserve(128);

        const uint32_t lvlSeed = hashCombine(levelGenSeed(LevelId{branch_, depth_}), tag32("BIOLUM"));

        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                if (dung.at(x, y).type != TileType::Floor) continue;

                const uint8_t g = dung.biolumAtCached(x, y);
                if (g < 14u) continue;

                const TerrainMaterial mat = dung.materialAtCached(x, y);
                // Restrict sources to materials we expect to plausibly glow.
                if (mat != TerrainMaterial::Crystal &&
                    mat != TerrainMaterial::Moss &&
                    mat != TerrainMaterial::Metal &&
                    mat != TerrainMaterial::Bone &&
                    mat != TerrainMaterial::Dirt) {
                    continue;
                }

                const uint32_t h = hash32(hashCombine(lvlSeed, hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
                cands.push_back({ Vec2i{x, y}, g, mat, h });
            }
        }

        if (!cands.empty()) {
            // Greedy Poisson-style selection: take the brightest candidates first,
            // then reject ones that are too close to previously accepted sources.
            std::sort(cands.begin(), cands.end(), [](const GlowCand& a, const GlowCand& b) {
                if (a.glow != b.glow) return a.glow > b.glow;
                return a.h > b.h;
            });

            const bool minesTheme = (depth_ == Dungeon::MINES_DEPTH || depth_ == Dungeon::DEEP_MINES_DEPTH);
            const int minSep = minesTheme ? 6 : 5;
            const int maxSources = clampi(12 + depth_ / 2, 12, 22);

            std::vector<Vec2i> chosen;
            chosen.reserve(static_cast<size_t>(maxSources));

            auto lerp8 = [&](uint8_t a, uint8_t b, float t) -> uint8_t {
                const float v = (1.0f - t) * static_cast<float>(a) + t * static_cast<float>(b);
                return static_cast<uint8_t>(clampi(static_cast<int>(v + 0.5f), 0, 255));
            };

            for (const auto& c : cands) {
                if (static_cast<int>(chosen.size()) >= maxSources) break;

                bool ok = true;
                for (const auto& p : chosen) {
                    if (chebyshev(p, c.pos) < minSep) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;

                chosen.push_back(c.pos);

                int radius = 2 + static_cast<int>(c.glow) / 45;
                radius = clampi(radius, 2, 5);

                int intensity = 40 + static_cast<int>(c.glow);
                if (c.mat == TerrainMaterial::Crystal) intensity += 20;
                if (c.mat == TerrainMaterial::Moss) intensity += 5;
                intensity = clampi(intensity, 35, 200);

                // Slight per-source color variation (still deterministic).
                const float t = static_cast<float>((c.h >> 8) & 0xFFu) / 255.0f;

                Color tint{ 255, 255, 255, 255 };
                switch (c.mat) {
                    case TerrainMaterial::Moss:
                        // green-cyan
                        tint = Color{ lerp8(120, 160, t), lerp8(235, 255, t), lerp8(150, 210, t), 255 };
                        break;
                    case TerrainMaterial::Crystal:
                        // cyan-purple
                        tint = Color{ lerp8(150, 220, t), lerp8(200, 150, t), 255, 255 };
                        break;
                    case TerrainMaterial::Metal:
                        // cold steel
                        tint = Color{ 210, 228, 255, 255 };
                        break;
                    case TerrainMaterial::Bone:
                        // eerie pale
                        tint = Color{ 235, 230, 195, 255 };
                        break;
                    case TerrainMaterial::Dirt:
                        // faint greenish spores
                        tint = Color{ 200, 245, 210, 255 };
                        break;
                    default:
                        tint = Color{ 255, 255, 255, 255 };
                        break;
                }

                sources.push_back({ c.pos, radius, static_cast<uint8_t>(intensity), tint });
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

    // Overworld weather can reduce visibility in the wilderness.
    if (atCamp() && !atHomeCamp()) {
        const int pen = overworldWeatherFx().fovPenalty;
        if (pen > 0) radius = std::max(4, radius - pen);
    }

    // Ecosystem microclimate: local haze can slightly alter sight radius.
    if (branch_ != DungeonBranch::Camp && dung.inBounds(p.pos.x, p.pos.y)) {
        // ensureMaterials() also populates the per-tile ecosystem cache.
        dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());
        const EcosystemKind ecoHere = dung.ecosystemAtCached(p.pos.x, p.pos.y);
        const int delta = ecosystemFx(ecoHere).fovDelta;
        if (delta != 0) {
            radius = std::max(4, radius + delta);
        }
    }

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

uint8_t Game::poisonGasAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0u;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= poisonGas_.size()) return 0u;
    return poisonGas_[i];
}

uint8_t Game::corrosiveGasAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0u;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= corrosiveGas_.size()) return 0u;
    return corrosiveGas_[i];
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
    if (W <= 0 || H <= 0) return;

    // Substrate materials influence scent: mossy/earthy areas absorb odor faster.
    dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());

    // Deposit at the player's current tile.
    //
    // Sneak mode intentionally reduces the "freshness" of the player's scent trail so
    // smell-capable monsters have a harder time tracking you around corners.
    // Heavy armor / heavy burden reduce the benefit.
    const Entity& p = player();
    uint8_t deposit = 255u;

    if (isSneaking()) {
        // Base sneaking deposit: ~200 down to ~80 with high agility.
        int d = 200 - std::max(0, playerAgility()) * 6;

        // Heavy armor makes it harder to suppress your trail.
        if (const Item* a = equippedArmor()) {
            if (a->kind == ItemKind::ChainArmor) d += 20;
            if (a->kind == ItemKind::PlateArmor) d += 40;
        }

        // Encumbrance makes sneaking clumsier and less subtle.
        if (encumbranceEnabled_) {
            switch (burdenState()) {
                case BurdenState::Unburdened: break;
                case BurdenState::Burdened:   d += 10; break;
                case BurdenState::Stressed:   d += 20; break;
                case BurdenState::Strained:   d += 30; break;
                case BurdenState::Overloaded: d += 40; break;
            }
        }

        if (d < 80) d = 80;
        if (d > 255) d = 255;
        deposit = static_cast<uint8_t>(d);
    }

    // Use the shared helper so we can unit-test the logic and keep it consistent
    // across gameplay and headless builds.
    ScentFieldParams params;
    params.baseDecay = 2;
    params.baseSpreadDrop = 14;
    params.minSpreadDrop = 6;
    params.maxSpreadDrop = 40;
    params.maxDecay = 20;
    params.windDir = windDir();
    params.windStrength = windStrength();

    auto isWalkable = [&](int x, int y) -> bool {
        return dung.inBounds(x, y) && dung.isWalkable(x, y);
    };

    auto fxAt = [&](int x, int y) -> ScentCellFx {
        const TerrainMaterial m = dung.materialAtCached(x, y);
        const TerrainMaterialFx matFx = terrainMaterialFx(m);
        const EcosystemKind eco = dung.ecosystemAtCached(x, y);
        const EcosystemFx ecoFx = ecosystemFx(eco);

        ScentCellFx out;
        out.decayDelta = matFx.scentDecayDelta + ecoFx.scentDecayDelta;
        out.spreadDropDelta = matFx.scentSpreadDropDelta + ecoFx.scentSpreadDropDelta;
        return out;
    };

    updateScentField(W, H, scentField_, p.pos, deposit, isWalkable, fxAt, params);
}

uint8_t Game::scentAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0u;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= scentField_.size()) return 0u;
    return scentField_[i];
}
