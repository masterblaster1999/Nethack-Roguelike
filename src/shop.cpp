#include "shop.hpp"

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

    // Wands / charged tools: scale with remaining charges.
    if (d.maxCharges > 0) {
        const int maxCh = std::max(1, d.maxCharges);
        const int ch = std::clamp(it.charges, 0, maxCh);
        // Even empty wands have a tiny "residual" value.
        v = std::max(1, (base * std::max(1, ch)) / maxCh);
    }

    // Gear: enchantment affects value.
    if (isWeapon(it.kind) || isArmor(it.kind)) {
        // +1 is ~+20% base, -1 is ~-20% base.
        const int step = std::max(1, base / 5);
        v = base + it.enchant * step;
        v = std::max(1, v);

        // Blessed/cursed modifier.
        if (it.buc > 0) v = (v * 120) / 100;
        else if (it.buc < 0) v = (v * 70) / 100;
        v = std::max(1, v);
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
