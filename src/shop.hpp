#pragma once

#include "items.hpp"

// Simple shop/economy helpers.
//
// Prices are computed per-unit. For stackable items, the total price is:
//   perUnitPrice * item.count
//
// The game stores the per-unit customer price directly on Item as:
//   item.shopPrice (and item.shopDepth)
//
// NOTE: shopPrice > 0 on an inventory item means it is UNPAID (debt).

// Base value (per unit) before shop markup / resale rates.
int shopBaseValuePerUnit(const Item& it);

// Customer price (per unit) for buying an item from a shop on the given depth.
int shopBuyPricePerUnit(const Item& it, int depth);

// Shop's offer (per unit) for buying an item from the player on the given depth.
int shopSellPricePerUnit(const Item& it, int depth);

// Whether the item is eligible to be sold to shops.
bool itemCanBeSoldToShop(const Item& it);
