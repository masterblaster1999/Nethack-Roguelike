#include "game_internal.hpp"
#include "farm_gen.hpp"

#include <algorithm>
#include <sstream>

namespace {

uint32_t cropSeedFromAny(const Item& it, uint32_t fallbackSalt) {
    if (it.charges != 0) {
        return cropSeedFromCharges(it.charges);
    }
    if (it.spriteSeed != 0u) {
        return it.spriteSeed;
    }
    return hash32(static_cast<uint32_t>(it.id) ^ fallbackSalt);
}

bool cropHasMeta(const Item& it) {
    // packCropMetaEnchant uses bit 12 as a signature.
    return ((it.enchant & (1 << 12)) != 0);
}

bool isIrrigationWaterTile(const Game& g, int x, int y) {
    const TileType t = g.dungeon().at(x, y).type;
    if (t == TileType::Fountain) return true;

    // The Surface Camp uses Chasm features as the river moat; count them as irrigation water.
    if (g.atHomeCamp() && t == TileType::Chasm) return true;

    return false;
}

int farmWaterTierAt(const Game& g, Vec2i p) {
    // Map a nearby-water distance into a small [0..8] tier.
    // Higher tier => faster growth.
    static constexpr int maxR = 8;
    int best = maxR + 1;

    const Dungeon& dung = g.dungeon();
    const int w = dung.width;
    const int h = dung.height;

    const int minX = std::max(0, p.x - maxR);
    const int maxX = std::min(w - 1, p.x + maxR);
    const int minY = std::max(0, p.y - maxR);
    const int maxY = std::min(h - 1, p.y + maxR);

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            if (!isIrrigationWaterTile(g, x, y)) continue;
            const int d = std::abs(x - p.x) + std::abs(y - p.y);
            if (d < best) best = d;
            if (best == 0) break;
        }
        if (best == 0) break;
    }

    if (best > maxR) return 0;
    return std::max(0, maxR - best);
}

bool isTillageMaterial(TerrainMaterial m) {
    return (m == TerrainMaterial::Dirt || m == TerrainMaterial::Moss);
}

} // namespace

bool Game::useGardenHoeAtPlayer(int invIndex) {
    if (invIndex < 0 || invIndex >= static_cast<int>(inv.size())) return false;

    Item& tool = inv[static_cast<size_t>(invIndex)];
    if (tool.kind != ItemKind::GardenHoe) return false;

    // Ensure durability starts at max.
    const int maxCh = std::max(1, itemDef(ItemKind::GardenHoe).maxCharges);
    if (tool.charges <= 0 || tool.charges > maxCh) tool.charges = maxCh;

    // Farming is only persistent (saved) in the home Surface Camp chunk.
    if (!atHomeCamp()) {
        pushMsg("YOU CAN ONLY FARM AT YOUR SURFACE CAMP.", MessageKind::Info, true);
        return false;
    }

    const Vec2i ppos = player().pos;
    if (!dung.inBounds(ppos.x, ppos.y)) return false;

    // Must be a normal floor tile.
    const TileType tt = dung.at(ppos.x, ppos.y).type;
    if (tt != TileType::Floor) {
        pushMsg("YOU CAN'T TILL THAT.", MessageKind::Bad, true);
        return false;
    }

    // Outdoor-ish ground only.
    dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());
    const TerrainMaterial mat = dung.materialAtCached(ppos.x, ppos.y);
    if (!isTillageMaterial(mat)) {
        pushMsg("THIS GROUND IS TOO HARD TO FARM.", MessageKind::Bad, true);
        return false;
    }

    // Can't place a plot on top of other stationary props.
    for (const auto& gi : ground) {
        if (gi.pos != ppos) continue;
        if (gi.item.kind == ItemKind::Chest || gi.item.kind == ItemKind::ChestOpen || isEcosystemNodeKind(gi.item.kind)) {
            pushMsg("SOMETHING IS IN THE WAY.", MessageKind::Bad, true);
            return false;
        }
        if (isFarmPlotKind(gi.item.kind) || isFarmPlantKind(gi.item.kind)) {
            pushMsg("THE SOIL HERE IS ALREADY WORKED.", MessageKind::Info, true);
            return false;
        }
        if (isStationaryPropKind(gi.item.kind) || itemIsStationary(gi.item)) {
            pushMsg("SOMETHING IS IN THE WAY.", MessageKind::Bad, true);
            return false;
        }
    }

    // Deterministic soil properties per tile.
    const uint32_t levelSeed = levelGenSeed(LevelId{branch_, depth_});
    const uint32_t soilSeed = farmgen::soilSeedAt(hashCombine(levelSeed, "FARM5011"_tag), ppos);
    const farmgen::SoilSpec ss = farmgen::makeSoil(soilSeed);

    const int affinityIdx = farmgen::farmTagIndex(ss.affinityTag);

    Item plot;
    plot.kind = ItemKind::TilledSoil;
    plot.count = 1;
    plot.spriteSeed = soilSeed;
    plot.charges = 0;
    plot.enchant = packTilledSoilEnchant(ss.fertility, affinityIdx);
    setItemStationary(plot, true);

    dropGroundItemItem(ppos, plot);

    {
        std::ostringstream msg;
        msg << "YOU TILL THE SOIL. (FERT " << ss.fertility;
        if (affinityIdx >= 0) {
            msg << ", AFF " << farmgen::farmTagByIndex(affinityIdx);
        }
        msg << ")";
        pushMsg(msg.str(), MessageKind::Info, true);
    }

    // Spend durability.
    tool.charges -= 1;
    if (tool.charges <= 0) {
        pushMsg("YOUR GARDEN HOE BREAKS!", MessageKind::Warning, true);
        inv.erase(inv.begin() + invIndex);
        invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    }

    return true;
}

bool Game::plantSeedAtPlayer(const Item& seedItem) {
    if (seedItem.kind != ItemKind::Seed) return false;

    if (!atHomeCamp()) {
        pushMsg("YOU CAN ONLY PLANT AT YOUR SURFACE CAMP.", MessageKind::Info, true);
        return false;
    }

    const Vec2i ppos = player().pos;
    if (!dung.inBounds(ppos.x, ppos.y)) return false;

    // Find tilled soil at the player's feet.
    int soilIdx = -1;
    for (int i = 0; i < static_cast<int>(ground.size()); ++i) {
        if (ground[static_cast<size_t>(i)].pos != ppos) continue;
        if (ground[static_cast<size_t>(i)].item.kind == ItemKind::TilledSoil) {
            soilIdx = i;
            break;
        }
    }

    if (soilIdx < 0) {
        pushMsg("YOU NEED TILLED SOIL. (USE A GARDEN HOE.)", MessageKind::Info, true);
        return false;
    }

    // Don't allow planting if a crop is already present.
    for (const auto& gi : ground) {
        if (gi.pos != ppos) continue;
        if (isFarmPlantKind(gi.item.kind)) {
            pushMsg("SOMETHING IS ALREADY GROWING HERE.", MessageKind::Info, true);
            return false;
        }
    }

    const Item soilIt = ground[static_cast<size_t>(soilIdx)].item;
    const int fert = std::max(1, tilledSoilFertilityFromEnchant(soilIt.enchant));
    const int affIdx = tilledSoilAffinityFromEnchant(soilIt.enchant);

    // Decode crop seed + meta.
    uint32_t cropSeed = cropSeedFromAny(seedItem, "CROP5EED"_tag);
    if (cropSeed == 0u) cropSeed = 1u;

    const bool hasMeta = cropHasMeta(seedItem);
    const int rarityHint  = hasMeta ? cropRarityFromEnchant(seedItem.enchant) : -1;
    const int variantHint = hasMeta ? cropVariantFromEnchant(seedItem.enchant) : -1;
    const int shinyHint   = hasMeta ? (cropIsShinyFromEnchant(seedItem.enchant) ? 1 : 0) : -1;

    const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);

    Item crop;
    crop.kind = ItemKind::CropSprout;
    crop.count = 1;

    // `charges` on farm plants stores the planted-at turn (used for growth).
    crop.charges = static_cast<int>(turnCount);

    // `spriteSeed` stores the crop seed so names/sprites stay consistent.
    crop.spriteSeed = cropSeed;

    crop.enchant = packFarmPlantEnchant(cs.variant, static_cast<int>(cs.rarity), cs.shiny, fert, affIdx);
    setItemStationary(crop, true);

    // Remove the tilled-soil placeholder and replace it with the planted crop.
    ground.erase(ground.begin() + soilIdx);
    dropGroundItemItem(ppos, crop);

    // Immediately refresh in case the run has advanced many turns before planting.
    updateFarmGrowth();

    const int waterTier = farmWaterTierAt(*this, ppos);
    const int dur = farmgen::growDurationTurns(cs, fert, waterTier);

    std::ostringstream ss;
    ss << "YOU PLANT " << cs.name << " SEEDS.";
    if (dur > 0) ss << " (~" << dur << " TURNS TO MATURITY)";
    pushMsg(ss.str(), MessageKind::Info, true);

    return true;
}

bool Game::harvestFarmAtPlayer() {
    if (!atHomeCamp()) return false;

    // Ensure crop stages are up to date before harvesting.
    updateFarmGrowth();

    const Vec2i ppos = player().pos;
    if (!dung.inBounds(ppos.x, ppos.y)) return false;

    int plantIdx = -1;
    for (int i = 0; i < static_cast<int>(ground.size()); ++i) {
        if (ground[static_cast<size_t>(i)].pos != ppos) continue;
        const ItemKind k = ground[static_cast<size_t>(i)].item.kind;
        if (k == ItemKind::CropMature) {
            plantIdx = i;
            break;
        }
    }

    if (plantIdx < 0) return false;

    const Item plantIt = ground[static_cast<size_t>(plantIdx)].item;

    uint32_t cropSeed = plantIt.spriteSeed;
    if (cropSeed == 0u) cropSeed = hash32(static_cast<uint32_t>(plantIt.id) ^ "CROPPL4NT"_tag);

    const bool hasMeta = cropHasMeta(plantIt);
    const int rarityHint  = hasMeta ? cropRarityFromEnchant(plantIt.enchant) : -1;
    const int variantHint = hasMeta ? cropVariantFromEnchant(plantIt.enchant) : -1;
    const int shinyHint   = hasMeta ? (cropIsShinyFromEnchant(plantIt.enchant) ? 1 : 0) : -1;

    const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);

    const int fert = std::max(1, farmPlantFertilityFromEnchant(plantIt.enchant));
    const int affIdx = farmPlantAffinityFromEnchant(plantIt.enchant);

    const int waterTier = farmWaterTierAt(*this, ppos);

    int quality = farmgen::qualityGradeIndex(fert, cs.rarity, cs.shiny);

    bool affinityMatch = false;
    if (affIdx >= 0 && cs.bonusTag && cs.bonusTag[0]) {
        const int bonusIdx = farmgen::farmTagIndex(cs.bonusTag);
        affinityMatch = (bonusIdx >= 0 && bonusIdx == affIdx);
    }
    if (affinityMatch) quality = std::min(15, quality + 1);
    if (waterTier >= 6) quality = std::min(15, quality + 1);

    // Deterministic yield per planted crop.
    uint32_t harvestSeed = hashCombine(seed_, cropSeed);
    harvestSeed = hashCombine(harvestSeed, static_cast<uint32_t>(plantIt.charges));
    harvestSeed = hashCombine(harvestSeed, "FARMH4RV"_tag);

    int count = farmgen::harvestYieldCount(cs, fert, harvestSeed);
    count = std::max(1, count);

    if (affinityMatch) count += 1;
    if (waterTier >= 4) count += 1;
    if (quality >= 4) count += 1;

    count = clampi(count, 1, 99);

    Item produce;
    produce.kind = ItemKind::CropProduce;
    produce.count = count;
    produce.charges = static_cast<int>(cropSeed);
    produce.spriteSeed = cropSeed;
    produce.enchant = packCropProduceEnchant(cs.variant, static_cast<int>(cs.rarity), cs.shiny, quality);
    produce.shopPrice = 0;
    produce.shopDepth = 0;

    // Seeds returned from harvest (so farming can sustain itself).
    int seedsBack = 0;
    {
        RNG lrng(hash32(harvestSeed ^ 0x5EEDBACCu));
        const float p = std::clamp(0.15f + 0.10f * static_cast<float>(quality), 0.10f, 0.75f);
        if (lrng.next01() < p) seedsBack += 1;
        if (quality >= 3 && lrng.next01() < 0.25f) seedsBack += 1;
        if (quality >= 4 && lrng.next01() < 0.15f) seedsBack += 1;
        seedsBack = clampi(seedsBack, 0, 3);
    }

    auto grantOrDrop = [&](Item out, const std::string& verbLine) {
        out.id = nextItemId++;
        out.shopPrice = 0;
        out.shopDepth = 0;

        const int maxInv = 26;
        bool stacked = tryStackItem(inv, out);
        bool dropped = false;
        if (!stacked) {
            if (static_cast<int>(inv.size()) >= maxInv) {
                dropGroundItemItem(ppos, out);
                dropped = true;
            } else {
                inv.push_back(out);
            }
        }

        std::ostringstream ss;
        ss << verbLine << " " << displayItemName(out);
        if (dropped) ss << " (PACK FULL - DROPPED)";
        ss << ".";
        pushMsg(ss.str(), MessageKind::Loot, true);
    };

    grantOrDrop(produce, "YOU HARVEST");

    if (seedsBack > 0) {
        Item seeds;
        seeds.kind = ItemKind::Seed;
        seeds.count = seedsBack;
        seeds.charges = static_cast<int>(cropSeed);
        seeds.spriteSeed = cropSeed;
        seeds.enchant = packCropMetaEnchant(cs.variant, static_cast<int>(cs.rarity), cs.shiny);
        grantOrDrop(seeds, "YOU GATHER");
    }

    // After harvesting, leave the plot tilled, but slowly deplete fertility.
    const int fertLoss = clampi(1 + (quality / 3), 1, 3);
    const int newFert = clampi(fert - fertLoss, 10, 100);

    Item plot;
    plot.kind = ItemKind::TilledSoil;
    plot.count = 1;
    plot.spriteSeed = hash32(harvestSeed ^ 0x5011u);
    plot.charges = 0;
    plot.enchant = packTilledSoilEnchant(newFert, affIdx);
    setItemStationary(plot, true);

    ground.erase(ground.begin() + plantIdx);
    dropGroundItemItem(ppos, plot);

    return true;
}

bool Game::describeFarmAtPlayer() {
    if (!atHomeCamp()) return false;

    const Vec2i ppos = player().pos;
    if (!dung.inBounds(ppos.x, ppos.y)) return false;

    // Prefer describing a crop if present.
    for (const auto& gi : ground) {
        if (gi.pos != ppos) continue;
        if (!isFarmPlantKind(gi.item.kind)) continue;

        // Compute a rough remaining time.
        uint32_t cropSeed = gi.item.spriteSeed;
        if (cropSeed == 0u) cropSeed = hash32(static_cast<uint32_t>(gi.item.id) ^ "CROPPL4NT"_tag);

        const bool hasMeta = cropHasMeta(gi.item);
        const int rarityHint  = hasMeta ? cropRarityFromEnchant(gi.item.enchant) : -1;
        const int variantHint = hasMeta ? cropVariantFromEnchant(gi.item.enchant) : -1;
        const int shinyHint   = hasMeta ? (cropIsShinyFromEnchant(gi.item.enchant) ? 1 : 0) : -1;
        const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);

        const int fert = std::max(1, farmPlantFertilityFromEnchant(gi.item.enchant));
        const int waterTier = farmWaterTierAt(*this, ppos);
        const int dur = farmgen::growDurationTurns(cs, fert, waterTier);

        const uint32_t plantedTurn = static_cast<uint32_t>(std::max(0, gi.item.charges));
        const uint32_t now = turnCount;
        const uint32_t elapsed = (now >= plantedTurn) ? (now - plantedTurn) : 0u;
        const int left = std::max(0, dur - static_cast<int>(elapsed));

        if (gi.item.kind == ItemKind::CropMature) {
            pushMsg("THIS CROP IS READY TO HARVEST.", MessageKind::Info, true);
            return true;
        }

        std::ostringstream ss;
        if (gi.item.kind == ItemKind::CropSprout) {
            ss << "A SPROUT IS GROWING.";
        } else {
            ss << "THE CROP IS GROWING.";
        }
        if (dur > 0) ss << " (" << left << " TURNS LEFT)";
        pushMsg(ss.str(), MessageKind::Info, true);
        return true;
    }

    // Otherwise, describe tilled soil.
    for (const auto& gi : ground) {
        if (gi.pos != ppos) continue;
        if (gi.item.kind != ItemKind::TilledSoil) continue;

        const int fert = tilledSoilFertilityFromEnchant(gi.item.enchant);
        const int affIdx = tilledSoilAffinityFromEnchant(gi.item.enchant);

        std::ostringstream ss;
        ss << "TILLED SOIL: FERT " << fert;
        if (affIdx >= 0) ss << ", AFF " << farmgen::farmTagByIndex(affIdx);
        ss << ". USE SEEDS TO PLANT.";
        pushMsg(ss.str(), MessageKind::Info, true);
        return true;
    }

    return false;
}

void Game::updateFarmGrowth() {
    if (ground.empty()) return;

    const uint32_t now = turnCount;

    // Update crop stages deterministically based on planted turn + soil fertility + irrigation.
    for (auto& gi : ground) {
        if (!isFarmPlantKind(gi.item.kind)) continue;

        uint32_t cropSeed = gi.item.spriteSeed;
        if (cropSeed == 0u) cropSeed = hash32(static_cast<uint32_t>(gi.item.id) ^ "CROPPL4NT"_tag);

        const bool hasMeta = cropHasMeta(gi.item);
        const int rarityHint  = hasMeta ? cropRarityFromEnchant(gi.item.enchant) : -1;
        const int variantHint = hasMeta ? cropVariantFromEnchant(gi.item.enchant) : -1;
        const int shinyHint   = hasMeta ? (cropIsShinyFromEnchant(gi.item.enchant) ? 1 : 0) : -1;
        const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);

        const int fert = std::max(1, farmPlantFertilityFromEnchant(gi.item.enchant));
        const int waterTier = farmWaterTierAt(*this, gi.pos);
        const int dur = farmgen::growDurationTurns(cs, fert, waterTier);

        const uint32_t plantedTurn = static_cast<uint32_t>(std::max(0, gi.item.charges));
        const uint32_t elapsed = (now >= plantedTurn) ? (now - plantedTurn) : 0u;

        ItemKind newKind = ItemKind::CropSprout;
        if (dur <= 0) {
            newKind = ItemKind::CropMature;
        } else if (elapsed >= static_cast<uint32_t>(dur)) {
            newKind = ItemKind::CropMature;
        } else if (elapsed >= static_cast<uint32_t>(dur / 2)) {
            newKind = ItemKind::CropGrowing;
        } else {
            newKind = ItemKind::CropSprout;
        }

        gi.item.kind = newKind;
    }
}
