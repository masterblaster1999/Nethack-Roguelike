#pragma once
#include "rng.hpp"
#include "dungeon.hpp"
#include "items.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

// Procedural shop identities
// -------------------------
// Each shop room gets a deterministic profile derived from (run seed, depth, room rect).
// This provides stable shop names/personalities and allows per-shop economy flavor without
// consuming the global RNG stream or changing save formats.

namespace shopgen {

enum class ShopTheme : uint8_t {
    General = 0,
    Armory = 1,
    Magic = 2,
    Supplies = 3,
};

enum class ShopTemperament : uint8_t {
    Greedy = 0,
    Shrewd,
    Fair,
    Generous,
    Eccentric,
};

struct ShopProfile {
    uint32_t seed = 1u; // never 0
    ShopTheme theme = ShopTheme::General;
    ShopTemperament temperament = ShopTemperament::Fair;

    // Multipliers applied on top of the existing shop economy. Percent scale.
    // buyMarkupPct: higher => more expensive to buy from the shop.
    // sellRatePct: higher => the shop pays more when you sell.
    int buyMarkupPct = 100;
    int sellRatePct = 100;
};

inline uint32_t shopSeed(uint32_t runSeed, int depth, const Room& r) {
    uint32_t s = hashCombine(runSeed, "SHOP"_tag);
    s = hashCombine(s, static_cast<uint32_t>(depth));
    s = hashCombine(s, static_cast<uint32_t>(r.x) | (static_cast<uint32_t>(r.y) << 16));
    s = hashCombine(s, static_cast<uint32_t>(r.w) | (static_cast<uint32_t>(r.h) << 16));
    s = hashCombine(s, static_cast<uint32_t>(r.type));
    if (s == 0u) s = 1u;
    return s;
}

inline ShopTheme themeForSeed(uint32_t seed) {
    // Roughly match the previous distribution:
    // 30% general, 25% armory, 25% magic, 20% supplies.
    const int roll = static_cast<int>(hashCombine(seed, "THEME"_tag) % 100u);
    if (roll < 30) return ShopTheme::General;
    if (roll < 55) return ShopTheme::Armory;
    if (roll < 80) return ShopTheme::Magic;
    return ShopTheme::Supplies;
}

inline ShopTemperament temperamentForSeed(uint32_t seed) {
    const int roll = static_cast<int>((hashCombine(seed, "TEMP"_tag) >> 8) % 100u);
    if (roll < 20) return ShopTemperament::Greedy;
    if (roll < 42) return ShopTemperament::Shrewd;
    if (roll < 70) return ShopTemperament::Fair;
    if (roll < 88) return ShopTemperament::Generous;
    return ShopTemperament::Eccentric;
}

inline const char* themeTag(ShopTheme t) {
    switch (t) {
        case ShopTheme::Armory: return "ARMORY";
        case ShopTheme::Magic: return "MAGIC";
        case ShopTheme::Supplies: return "SUPPLIES";
        default: return "GENERAL";
    }
}

inline const char* temperamentTag(ShopTemperament t) {
    switch (t) {
        case ShopTemperament::Greedy: return "GREEDY";
        case ShopTemperament::Shrewd: return "SHREWD";
        case ShopTemperament::Generous: return "GENEROUS";
        case ShopTemperament::Eccentric: return "ECCENTRIC";
        default: return "FAIR";
    }
}

inline ShopProfile profileFor(uint32_t runSeed, int depth, const Room& r) {
    ShopProfile p;
    p.seed = shopSeed(runSeed, depth, r);
    p.theme = themeForSeed(p.seed);
    p.temperament = temperamentForSeed(p.seed);

    switch (p.temperament) {
        case ShopTemperament::Greedy:
            p.buyMarkupPct = 112;
            p.sellRatePct = 90;
            break;
        case ShopTemperament::Shrewd:
            p.buyMarkupPct = 106;
            p.sellRatePct = 96;
            break;
        case ShopTemperament::Fair:
            p.buyMarkupPct = 100;
            p.sellRatePct = 100;
            break;
        case ShopTemperament::Generous:
            p.buyMarkupPct = 94;
            p.sellRatePct = 108;
            break;
        case ShopTemperament::Eccentric:
            p.buyMarkupPct = 98;
            p.sellRatePct = 103;
            break;
        default:
            break;
    }

    // Slight depth drift: deeper floors tend a little more predatory.
    const int d = std::clamp(depth, 1, 12);
    if (d >= 7) {
        p.buyMarkupPct = std::min(120, p.buyMarkupPct + 2);
        p.sellRatePct = std::max(80, p.sellRatePct - 1);
    }

    return p;
}

inline bool isArmoryMerch(ItemKind k) {
    return isWeapon(k) || isArmor(k) || k == ItemKind::Arrow || k == ItemKind::Rock || k == ItemKind::Pickaxe;
}

inline bool isMagicMerch(ItemKind k) {
    return isPotionKind(k) || isScrollKind(k) || isWandKind(k) || isSpellbookKind(k) || isRingKind(k) || k == ItemKind::RuneTablet;
}

inline bool isSupplyMerch(ItemKind k) {
    switch (k) {
        case ItemKind::FoodRation:
        case ItemKind::Torch:
        case ItemKind::Lockpick:
        case ItemKind::Key:
        case ItemKind::FishingRod:
        case ItemKind::Fish:
        case ItemKind::GardenHoe:
        case ItemKind::Seed:
        case ItemKind::CropProduce:
        case ItemKind::CraftingKit:
        case ItemKind::ButcheredMeat:
        case ItemKind::ButcheredHide:
        case ItemKind::ButcheredBones:
        case ItemKind::EssenceShard:
            return true;
        default:
            return false;
    }
}

inline bool matchesTheme(ShopTheme theme, ItemKind k) {
    switch (theme) {
        case ShopTheme::Armory: return isArmoryMerch(k);
        case ShopTheme::Magic: return isMagicMerch(k);
        case ShopTheme::Supplies: return isSupplyMerch(k);
        default: return true;
    }
}

inline int pctMul(int v, int pct) {
    if (pct <= 0) return 0;
    return (v * pct + 50) / 100;
}

inline int themeBuyBiasPct(ShopTheme theme, ItemKind k) {
    if (theme == ShopTheme::General) return 100;
    const bool match = matchesTheme(theme, k);
    return match ? 96 : 108;
}

inline int themeSellBiasPct(ShopTheme theme, ItemKind k) {
    if (theme == ShopTheme::General) return 100;
    const bool match = matchesTheme(theme, k);
    return match ? 110 : 88;
}

inline int eccentricItemJitterPct(const ShopProfile& p, const Item& it) {
    if (p.temperament != ShopTemperament::Eccentric) return 100;

    uint32_t h = hashCombine(p.seed, "JIT"_tag);
    uint32_t key = it.spriteSeed;
    if (key == 0u) {
        key = hashCombine(static_cast<uint32_t>(it.id), static_cast<uint32_t>(it.kind));
    }
    h = hashCombine(h, key);

    const int jitter = static_cast<int>(h % 13u) - 6; // [-6, +6]
    return 100 + jitter;
}

inline int adjustedShopBuyPricePerUnit(int basePerUnit, const ShopProfile& p, const Item& it) {
    if (basePerUnit <= 0) return 0;

    int pct = p.buyMarkupPct;
    pct = pctMul(pct, themeBuyBiasPct(p.theme, it.kind));
    pct = pctMul(pct, eccentricItemJitterPct(p, it));

    int out = pctMul(basePerUnit, pct);
    if (out < 1) out = 1;
    return out;
}

inline int adjustedShopSellPricePerUnit(int basePerUnit, const ShopProfile& p, const Item& it) {
    if (basePerUnit <= 0) return 0;

    int pct = p.sellRatePct;
    pct = pctMul(pct, themeSellBiasPct(p.theme, it.kind));
    pct = pctMul(pct, eccentricItemJitterPct(p, it));

    int out = pctMul(basePerUnit, pct);
    if (out < 0) out = 0;
    return out;
}

inline const Room* shopRoomAt(const Dungeon& d, Vec2i pos) {
    for (const auto& r : d.rooms) {
        if (r.type != RoomType::Shop) continue;
        if (r.contains(pos.x, pos.y)) return &r;
    }
    return nullptr;
}

inline std::string shopkeeperNameFor(const ShopProfile& p) {
    // Small syllable-based name generator (upper-case to match UI tone).
    static constexpr const char* A[] = {
        "AL", "BAR", "COR", "DOR", "EL", "FEN", "GAR", "HAL", "IV", "JOR", "KEL", "LOR", "MOR", "NAL", "OR",
        "PER", "QUIN", "RAN", "SER", "TOR", "UL", "VAL", "WEN", "XAN", "YOR", "ZEL",
    };
    static constexpr const char* V[] = {"A", "E", "I", "O", "U", "AE", "AI", "IO", "OU"};
    static constexpr const char* B[] = {"N", "R", "S", "TH", "K", "L", "M", "ND", "RD", "SK", "NAR", "LIS", "VON", "TIL", "DAN"};

    uint32_t h = hashCombine(p.seed, "KEEP"_tag);
    const char* a1 = A[h % (sizeof(A) / sizeof(A[0]))];
    const char* v1 = V[(h >> 6) % (sizeof(V) / sizeof(V[0]))];
    const char* b1 = B[(h >> 12) % (sizeof(B) / sizeof(B[0]))];

    std::string out;
    out.reserve(14);
    out += a1;
    out += v1;
    out += b1;

    if (((h >> 20) & 1u) != 0u) {
        const char* a2 = A[(h >> 21) % (sizeof(A) / sizeof(A[0]))];
        const char* v2 = V[(h >> 3) % (sizeof(V) / sizeof(V[0]))];
        out += a2;
        out += v2;
    }

    if (((h >> 27) & 1u) != 0u && out.size() >= 6 && out.size() <= 10) {
        out.insert(2, "'");
    }

    if (out.size() > 12) out.resize(12);
    return out;
}

inline std::string shopNameFor(const ShopProfile& p) {
    static constexpr const char* ADJ[] = {
        "GILDED", "RUSTED", "HUMBLE", "SILKEN", "OBSIDIAN", "IVORY", "CRIMSON", "SABLE", "MOSSY", "BRIGHT",
        "WANDERING", "HOLLOW", "BURNISHED", "CROOKED", "MERRY", "WICKED", "SOMBER", "ANCIENT", "FROSTED", "ARCANE",
        "VERDANT", "DUSTY", "VIOLET", "SILVER", "GOLDEN", "SMOKE", "STORM", "SUNLIT",
    };

    static constexpr const char* NOUN_GENERAL[] = {
        "LANTERN", "CABINET", "CURIOS", "MARKET", "COUNTER", "COIN", "CROWN", "SHELF", "STOCK", "EMPORIUM",
        "TRADER", "BAZAAR", "DEPOT",
    };

    static constexpr const char* NOUN_ARMORY[] = {
        "ANVIL", "BLADE", "SHIELD", "QUIVER", "FORGE", "ARMORY", "RAMPART", "EDGE",
    };

    static constexpr const char* NOUN_MAGIC[] = {
        "SIGIL", "TOME", "RUNE", "WAND", "AURA", "CIRCLE", "LENS", "CAULDRON",
    };

    static constexpr const char* NOUN_SUPPLIES[] = {
        "PACK", "PROVISION", "LARDER", "KIT", "SUPPLY", "DEPOT", "CACHE",
    };

    static constexpr const char* SUFFIX[] = {
        "EMPORIUM", "BAZAAR", "DEPOT", "MART", "HOUSE", "TRADING", "STOCKS",
    };

    auto pick = [](const char* const* arr, size_t n, uint32_t h) -> const char* {
        if (n == 0) return "";
        return arr[h % n];
    };

    uint32_t h = hashCombine(p.seed, "NAME"_tag);
    const char* adj = pick(ADJ, sizeof(ADJ) / sizeof(ADJ[0]), h);

    const char* noun = "";
    switch (p.theme) {
        case ShopTheme::Armory:
            noun = pick(NOUN_ARMORY, sizeof(NOUN_ARMORY) / sizeof(NOUN_ARMORY[0]), h >> 8);
            break;
        case ShopTheme::Magic:
            noun = pick(NOUN_MAGIC, sizeof(NOUN_MAGIC) / sizeof(NOUN_MAGIC[0]), h >> 8);
            break;
        case ShopTheme::Supplies:
            noun = pick(NOUN_SUPPLIES, sizeof(NOUN_SUPPLIES) / sizeof(NOUN_SUPPLIES[0]), h >> 8);
            break;
        default:
            noun = pick(NOUN_GENERAL, sizeof(NOUN_GENERAL) / sizeof(NOUN_GENERAL[0]), h >> 8);
            break;
    }

    const char* suf = pick(SUFFIX, sizeof(SUFFIX) / sizeof(SUFFIX[0]), h >> 16);
    const int style = static_cast<int>((h >> 24) % 3u);

    std::string out;
    if (style == 0) {
        out = std::string("THE ") + adj + " " + noun;
    } else if (style == 1) {
        out = std::string("THE ") + noun + " OF " + adj;
    } else {
        out = std::string("THE ") + adj + " " + noun + " " + suf;
    }

    if (out.size() > 28 && style == 2) {
        out = std::string("THE ") + adj + " " + noun;
    }

    return out;
}

inline std::string greetingFor(const ShopProfile& p) {
    static constexpr const char* G_GREEDY[] = {
        "\"NO CREDIT. NO REFUNDS.\"",
        "\"LOOK WITH YOUR EYES, NOT YOUR HANDS.\"",
        "\"PRICES ARE FIRM.\"",
        "\"PAY UP FRONT.\"",
    };
    static constexpr const char* G_SHREWD[] = {
        "\"QUALITY COSTS.\"",
        "\"BUY LOW. SELL HIGH.\"",
        "\"INSPECT BEFORE YOU COMPLAIN.\"",
        "\"I KNOW WHAT IT'S WORTH.\"",
    };
    static constexpr const char* G_FAIR[] = {
        "\"WELCOME. TAKE YOUR TIME.\"",
        "\"FAIR PRICES FOR FAIR FOLK.\"",
        "\"BROWSE AT YOUR LEISURE.\"",
        "\"DON'T TRACK MUD ON THE RUG.\"",
    };
    static constexpr const char* G_GEN[] = {
        "\"AH, A TRAVELER! TODAY'S YOUR LUCKY DAY.\"",
        "\"WE KEEP IT HONEST HERE.\"",
        "\"MAY YOUR COIN RETURN TO YOU.\"",
        "\"NEED SUPPLIES? I'LL CUT YOU A DEAL.\"",
    };
    static constexpr const char* G_ECC[] = {
        "\"DO NOT TOUCH THE CURSED SHELF.\"",
        "\"SOME PRICES ARE... NEGOTIABLE.\"",
        "\"IF IT WHISPERS, IT'S PROBABLY FINE.\"",
        "\"I BUY STRANGE THINGS.\"",
    };

    static constexpr const char* T_ARM[] = {"\"SHARPEN YOUR EDGE.\"", "\"STEEL SOLVES PROBLEMS.\"", "\"MIND THE BLADES.\""};
    static constexpr const char* T_MAG[] = {"\"MIND THE RUNES.\"", "\"MAGIC BITES.\"", "\"DON'T POINT THAT WAND AT ME.\""};
    static constexpr const char* T_SUP[] = {"\"STAY FED. STAY LIT.\"", "\"PACK LIGHT.\"", "\"TOOLS LAST LONGER THAN HEROES.\""};
    static constexpr const char* T_GEN[] = {"\"EVERYTHING HAS A PRICE.\"", "\"IF YOU NEED IT, I HAVE IT.\"", "\"COME BACK ALIVE.\""};

    auto pick = [](const char* const* arr, size_t n, uint32_t h) -> const char* {
        if (n == 0) return "";
        return arr[h % n];
    };

    uint32_t h = hashCombine(p.seed, "GREET"_tag);

    const char* base = "";
    switch (p.temperament) {
        case ShopTemperament::Greedy:
            base = pick(G_GREEDY, sizeof(G_GREEDY) / sizeof(G_GREEDY[0]), h);
            break;
        case ShopTemperament::Shrewd:
            base = pick(G_SHREWD, sizeof(G_SHREWD) / sizeof(G_SHREWD[0]), h);
            break;
        case ShopTemperament::Generous:
            base = pick(G_GEN, sizeof(G_GEN) / sizeof(G_GEN[0]), h);
            break;
        case ShopTemperament::Eccentric:
            base = pick(G_ECC, sizeof(G_ECC) / sizeof(G_ECC[0]), h);
            break;
        default:
            base = pick(G_FAIR, sizeof(G_FAIR) / sizeof(G_FAIR[0]), h);
            break;
    }

    const bool addThemeLine = ((h >> 10) & 1u) != 0u;
    if (!addThemeLine) return base;

    const char* tline = "";
    switch (p.theme) {
        case ShopTheme::Armory:
            tline = pick(T_ARM, sizeof(T_ARM) / sizeof(T_ARM[0]), h >> 12);
            break;
        case ShopTheme::Magic:
            tline = pick(T_MAG, sizeof(T_MAG) / sizeof(T_MAG[0]), h >> 12);
            break;
        case ShopTheme::Supplies:
            tline = pick(T_SUP, sizeof(T_SUP) / sizeof(T_SUP[0]), h >> 12);
            break;
        default:
            tline = pick(T_GEN, sizeof(T_GEN) / sizeof(T_GEN[0]), h >> 12);
            break;
    }

    std::string out = base;
    out += " ";
    out += tline;
    return out;
}

} // namespace shopgen
