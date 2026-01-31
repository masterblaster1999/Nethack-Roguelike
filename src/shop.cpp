#include "shop.hpp"

#include "vtuber_gen.hpp"
#include "farm_gen.hpp"

#include <algorithm>
#include <cmath>

namespace {

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

// A tiny deterministic variance so shops feel a bit more "alive" without needing RNG.
// (Bounded to +/- 4%.)
float smallDeterministicVariance(const Item& it) {
    uint32_t s = it.spriteSeed;
    if (s == 0) {
        s = static_cast<uint32_t>(it.id) * 2654435761u;
    }

    const int pct = static_cast<int>(s % 9u) - 4; // -4..+4
    return 1.0f + static_cast<float>(pct) / 100.0f;
}

} // namespace

bool itemCanBeSoldToShop(const Item& it) {
    if (isGold(it.kind)) return false;
    // Avoid nonsense / quest items.
    if (it.kind == ItemKind::AmuletYendor) return false;
    if (it.kind == ItemKind::Chest || it.kind == ItemKind::ChestOpen) return false;
    if (it.kind == ItemKind::TorchLit) return false;

    return itemDef(it.kind).value > 0;
}

int shopBaseValuePerUnit(const Item& it) {
    if (!itemCanBeSoldToShop(it)) return 0;

    const ItemDef& d = itemDef(it.kind);
    const int base = std::max(0, d.value);
    if (base <= 0) return 0;

    int v = base;

    // Procedural farming items: derive value from crop spec instead of the static ItemDef.
    // This makes seeds/produce meaningful to sell and keeps shops coherent.
    if (it.kind == ItemKind::Seed || it.kind == ItemKind::CropProduce) {
        uint32_t cropSeed = 0u;
        if (it.charges != 0) cropSeed = cropSeedFromCharges(it.charges);
        else if (it.spriteSeed != 0u) cropSeed = it.spriteSeed;
        else cropSeed = hash32(static_cast<uint32_t>(it.id) ^ 0xCROP5EEDu);

        const bool hasMeta = ((it.enchant & (1 << 12)) != 0);
        const int rarityHint = hasMeta ? cropRarityFromEnchant(it.enchant) : -1;
        const int variantHint = hasMeta ? cropVariantFromEnchant(it.enchant) : -1;
        const int shinyHint = hasMeta ? (cropIsShinyFromEnchant(it.enchant) ? 1 : 0) : -1;

        const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed, rarityHint, variantHint, shinyHint);
        int cv = std::max(1, cs.value);

        if (it.kind == ItemKind::Seed) {
            // Seeds are cheaper than the harvested food.
            cv = std::max(1, cv / 3);
        } else {
            // Quality is stored on produce. Higher grades fetch better prices.
            const int q = hasMeta ? cropQualityFromEnchant(it.enchant) : 0;
            cv = (cv * (100 + q * 15)) / 100;
            cv = std::max(1, cv);
        }

        v = std::max(v, cv);
    }


    // Wands / charged tools: scale with remaining charges.
    if (d.maxCharges > 0) {
        const int maxCh = std::max(1, d.maxCharges);
        const int ch = std::clamp(it.charges, 0, maxCh);
        // Even empty wands have a tiny "residual" value.
        v = std::max(1, (base * std::max(1, ch)) / maxCh);
    }

    // Gear: enchantment affects value.
    if (isWeapon(it.kind) || isArmor(it.kind) || isRingKind(it.kind)) {
        // +1 is ~+20% base, -1 is ~-20% base.
        const int step = std::max(1, base / 5);
        v = base + it.enchant * step;
        v = std::max(1, v);

        // Blessed/cursed modifier.
        if (it.buc > 0) v = (v * 120) / 100;
        else if (it.buc < 0) v = (v * 70) / 100;
        v = std::max(1, v);

        // Ego/brand modifier (rare premium gear).
        if (it.ego != ItemEgo::None) {
            v = (v * egoValueMultiplierPct(it.ego)) / 100;
            v = std::max(1, v);
        }
    }

    // Artifacts: a significant premium over normal gear.
    if (itemIsArtifact(it)) {
        v = (v * 280) / 100; // ~2.8x
        v = std::max(1, v);
    }



    // VTuber collectibles: procedural value based on persona rarity/edition/followers.
    // This makes the "gacha" feel real without relying on extra saved state.
    if (it.spriteSeed != 0u && isVtuberCollectible(it.kind)) {
        const uint32_t s = it.spriteSeed;
        const VtuberRarity rar = vtuberRarity(s);

        int vv = (v * vtuberRarityValueMultiplierPct(rar)) / 100;

        // Mild follower-band bonus (kept small; rarity is the main driver).
        const int fol = vtuberFollowerCount(s);
        int folPct = 100;
        if (fol >= 100'000) folPct = 112;
        if (fol >= 800'000) folPct = 125;
        vv = (vv * folPct) / 100;

        if (it.kind == ItemKind::VtuberHoloCard) {
            const VtuberCardEdition ed = vtuberCardEdition(s);
            vv = (vv * vtuberCardEditionValueMultiplierPct(ed)) / 100;

            // Signed/collab cards get a tiny flat premium so their value isn't
            // fully eaten by integer rounding.
            if (ed == VtuberCardEdition::Signed) vv += 15;
            if (ed == VtuberCardEdition::Collab) vv += 25;
        } else if (it.kind == ItemKind::VtuberFigurine) {
            // Figurines are bulkier collectibles; nudge them up slightly.
            vv = (vv * 115) / 100;
        }

        v = std::max(1, vv);
    }
    return std::max(1, v);
}

int shopBuyPricePerUnit(const Item& it, int depth) {
    const int base = shopBaseValuePerUnit(it);
    if (base <= 0) return 0;

    // Markup slowly decreases with depth to keep shops attractive later.
    float markup = 1.50f - 0.02f * static_cast<float>(std::min(15, std::max(0, depth - 1)));
    markup = clampf(markup, 1.15f, 1.50f);

    const float var = smallDeterministicVariance(it);

    int price = static_cast<int>(std::ceil(static_cast<float>(base) * markup * var));
    return std::max(1, price);
}

int shopSellPricePerUnit(const Item& it, int depth) {
    const int base = shopBaseValuePerUnit(it);
    if (base <= 0) return 0;

    // Shops pay roughly half, slightly more on deeper floors.
    float rate = 0.45f + 0.01f * static_cast<float>(std::min(10, std::max(0, depth - 1)));
    rate = clampf(rate, 0.45f, 0.55f);

    const float var = smallDeterministicVariance(it);

    int offer = static_cast<int>(std::floor(static_cast<float>(base) * rate * var));
    return std::max(0, offer);
}
