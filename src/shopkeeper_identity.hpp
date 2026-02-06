#pragma once

#include "game.hpp"
#include "shop_profile_gen.hpp"

#include <string>

namespace shopid {

// Attempts to recover the deterministic ShopProfile for a given shopkeeper entity.
//
// Shopkeepers are spawned with a spriteSeed derived from the shop profile seed
// (see Game::spawnMonsters). This helper maps a shopkeeper back to its originating
// shop even if it has wandered outside the room.
inline bool shopProfileForShopkeeper(const Game& g, const Entity& shopkeeper, shopgen::ShopProfile* outProf) {
    if (!outProf) return false;
    if (shopkeeper.kind != EntityKind::Shopkeeper) return false;

    const Dungeon& d = g.dungeon();
    const uint32_t worldSeed = g.seed();
    const int depth = g.depth();

    // Prefer a strict spriteSeed match: this works even if the shopkeeper has moved.
    if (shopkeeper.spriteSeed != 0u) {
        for (const Room& r : d.rooms) {
            if (r.type != RoomType::Shop) continue;
            const shopgen::ShopProfile p = shopgen::profileFor(worldSeed, depth, r);
            if (hashCombine(p.seed, "SK"_tag) == shopkeeper.spriteSeed) {
                *outProf = p;
                return true;
            }
        }
    }

    // Fallback: use current position to find the containing shop room.
    if (const Room* rr = shopgen::shopRoomAt(d, shopkeeper.pos)) {
        *outProf = shopgen::profileFor(worldSeed, depth, *rr);
        return true;
    }

    return false;
}

inline std::string shopkeeperLabelForUI(const Game& g, const Entity& shopkeeper) {
    shopgen::ShopProfile prof;
    if (shopProfileForShopkeeper(g, shopkeeper, &prof)) {
        return std::string("SHOPKEEPER ") + shopgen::shopkeeperNameFor(prof);
    }
    return "SHOPKEEPER";
}

} // namespace shopid
