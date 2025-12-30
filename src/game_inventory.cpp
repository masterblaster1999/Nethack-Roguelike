#include "game_internal.hpp"

void Game::openInventory() {
    // Close other overlays
    targeting = false;
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    invOpen = true;
    invIdentifyMode = false;
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
}

void Game::closeInventory() {
    invOpen = false;
    invIdentifyMode = false;
}

void Game::moveInventorySelection(int dy) {
    if (inv.empty()) { invSel = 0; return; }
    invSel = clampi(invSel + dy, 0, static_cast<int>(inv.size()) - 1);
}


void Game::sortInventory() {
    if (inv.empty()) {
        pushMsg("NOTHING TO SORT.", MessageKind::Info, true);
        return;
    }

    // Remember the currently selected item (by id) so we can restore selection after sort.
    int selectedId = 0;
    if (invSel >= 0 && invSel < static_cast<int>(inv.size())) {
        selectedId = inv[static_cast<size_t>(invSel)].id;
    }

    auto category = [&](const Item& it) -> int {
        // 0 = quest/special
        if (it.kind == ItemKind::AmuletYendor) return 0;

        // 1 = equipped gear
        if (it.id == equipMeleeId || it.id == equipRangedId || it.id == equipArmorId || it.id == equipRing1Id || it.id == equipRing2Id) return 1;

        // 2 = other equipment
        const ItemDef& d = itemDef(it.kind);
        if (d.slot != EquipSlot::None) return 2;

        // 3 = consumables (potions/scrolls)
        if (d.consumable) return 3;

        // 4 = ammo
        if (it.kind == ItemKind::Arrow || it.kind == ItemKind::Rock) return 4;

        // 5 = gold
        if (it.kind == ItemKind::Gold) return 5;

        return 6;
    };

    std::stable_sort(inv.begin(), inv.end(), [&](const Item& a, const Item& b) {
        const int ca = category(a);
        const int cb = category(b);
        if (ca != cb) return ca < cb;

        const std::string na = displayItemName(a);
        const std::string nb = displayItemName(b);
        if (na != nb) return na < nb;

        // Tie-breaker for stability.
        return a.id < b.id;
    });

    if (selectedId != 0) {
        int idx = findItemIndexById(inv, selectedId);
        if (idx >= 0) invSel = idx;
    }
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    pushMsg("INVENTORY SORTED.", MessageKind::System, true);
}

bool Game::autoPickupAtPlayer() {
    const Vec2i pos = player().pos;
    const int maxInv = 26;

    if (autoPickup == AutoPickupMode::Off) return false;

    int pickedCount = 0;
    std::vector<std::string> sampleNames;

    for (size_t i = 0; i < ground.size();) {
        if (ground[i].pos == pos && ground[i].item.shopPrice <= 0 && autoPickupWouldPick(ground[i].item.kind)) {
            Item it = ground[i].item;

            // Merge into existing stacks if possible.
            if (!tryStackItem(inv, it)) {
                if (static_cast<int>(inv.size()) >= maxInv) {
                    // Silent failure (avoid spam while walking).
                    ++i;
                    continue;
                }
                inv.push_back(it);
            }

            ++pickedCount;
            if (sampleNames.size() < 3) sampleNames.push_back(displayItemName(it));

            ground.erase(ground.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }

    if (pickedCount <= 0) return false;

    // Aggregate to reduce log spam during auto-travel.
    if (pickedCount == 1) {
        pushMsg("YOU PICK UP " + sampleNames[0] + ".", MessageKind::Loot, true);
    } else {
        std::ostringstream ss;
        ss << "YOU PICK UP " << sampleNames[0];
        if (sampleNames.size() >= 2) ss << ", " << sampleNames[1];
        if (sampleNames.size() >= 3) ss << ", " << sampleNames[2];
        if (pickedCount > static_cast<int>(sampleNames.size())) {
            ss << " (+" << (pickedCount - static_cast<int>(sampleNames.size())) << " MORE)";
        }
        ss << ".";
        pushMsg(ss.str(), MessageKind::Loot, true);
    }

    return true;
}

bool Game::openChestAtPlayer() {
    const Vec2i pos = player().pos;

    // Find a closed chest at the player's position.
    GroundItem* chestGi = nullptr;
    for (auto& gi : ground) {
        if (gi.pos == pos && gi.item.kind == ItemKind::Chest) {
            chestGi = &gi;
            break;
        }
    }
    if (!chestGi) return false;

    Item& chest = chestGi->item;

    // Mimic: a fake chest that turns into a monster when you try to open it.
    if (chestMimic(chest)) {
        // Remove the chest first.
        Vec2i chestPos = chestGi->pos;
        ground.erase(std::remove_if(ground.begin(), ground.end(), [&](const GroundItem& gi) {
            return gi.pos == chestPos && gi.item.id == chest.id;
        }), ground.end());

        pushMsg("THE CHEST WAS A MIMIC!", MessageKind::Warning, true);

        // A mimic reveal is loud.
        emitNoise(chestPos, 14);

        // Prefer spawning adjacent so we don't overlap the player (chests are opened underfoot).
        static const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        Vec2i spawn = {-1, -1};
        // Randomize direction order a bit.
        int order[8] = {0,1,2,3,4,5,6,7};
        for (int i = 7; i > 0; --i) {
            int j = rng.range(0, i);
            std::swap(order[i], order[j]);
        }
        for (int ii = 0; ii < 8; ++ii) {
            int di = order[ii];
            int nx = chestPos.x + dirs[di][0];
            int ny = chestPos.y + dirs[di][1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.isWalkable(nx, ny)) continue;
            if (entityAt(nx, ny)) continue;
            if (Vec2i{nx, ny} == dung.stairsUp || Vec2i{nx, ny} == dung.stairsDown) continue;
            spawn = {nx, ny};
            break;
        }

        // Worst-case: if surrounded, shove the player to a nearby free tile and spawn in place.
        if (spawn.x < 0) {
            Vec2i dst = chestPos;
            for (int r = 2; r <= 6 && dst == chestPos; ++r) {
                for (int y = chestPos.y - r; y <= chestPos.y + r; ++y) {
                    for (int x = chestPos.x - r; x <= chestPos.x + r; ++x) {
                        if (!dung.inBounds(x, y)) continue;
                        if (!dung.isWalkable(x, y)) continue;
                        if (entityAt(x, y)) continue;
                        Vec2i cand{x, y};
                        if (cand == dung.stairsUp || cand == dung.stairsDown) continue;
                        dst = cand;
                        break;
                    }
                    if (dst != chestPos) break;
                }
            }
            if (dst != chestPos) {
                playerMut().pos = dst;
                pushMsg("THE MIMIC SHOVES YOU BACK!", MessageKind::Warning, true);
            }
            spawn = chestPos;
        }

        // Spawn the mimic.
        Entity m;
        m.id = nextEntityId++;
        m.kind = EntityKind::Mimic;
        m.speed = baseSpeedFor(m.kind);
        m.energy = 0;
        m.pos = spawn;
        m.spriteSeed = rng.nextU32();
        m.groupId = 0;
        m.hpMax = 16;
        m.baseAtk = 4;
        m.baseDef = 2;
        m.willFlee = false;

        // Depth scaling (match regular monsters).
        int dd = std::max(0, depth_ - 1);
        if (dd > 0) {
            m.hpMax += dd;
            m.baseAtk += dd / 3;
            m.baseDef += dd / 4;
        }
        m.hp = m.hpMax;
        m.alerted = true;
        m.lastKnownPlayerPos = player().pos;
        m.lastKnownPlayerAge = 0;

        ents.push_back(m);
        return true; // Opening costs a turn.
    }

    // Locked chest: consume a key or attempt lockpick.
    if (chestLocked(chest)) {
        if (keyCount() > 0) {
            (void)consumeKeys(1);
            setChestLocked(chest, false);
            pushMsg("YOU UNLOCK THE CHEST.", MessageKind::Info, true);
            emitNoise(pos, 10);
        } else if (lockpickCount() > 0) {
            // Lockpicking chance scales with character level, but higher-tier chests are harder.
            float chance = 0.35f + 0.05f * static_cast<float>(charLevel);
            chance -= 0.05f * static_cast<float>(chestTier(chest));

            if (rng.chance(chance)) {
                setChestLocked(chest, false);
                pushMsg("YOU PICK THE CHEST'S LOCK.", MessageKind::Info, true);
                emitNoise(pos, 10);
            } else {
                // Failed pick still costs a turn.
                pushMsg("YOU FAIL TO PICK THE CHEST'S LOCK.", MessageKind::Info, true);
                emitNoise(pos, 10);
                // Chance to break a lockpick.
                float breakChance = 0.10f + 0.05f * static_cast<float>(chestTier(chest));
                if (rng.chance(breakChance)) {
                    (void)consumeLockpicks(1);
                    pushMsg("YOUR LOCKPICK BREAKS!", MessageKind::Warning, true);
                }
                return true;
            }
        } else {
            pushMsg("THE CHEST IS LOCKED.", MessageKind::Info, true);
            return false;
        }
    }

    // Opening the chest consumes a turn.
    pushMsg("YOU OPEN THE CHEST.", MessageKind::Loot, true);
    emitNoise(pos, 12);

    // Trigger trap if present.
    if (chestTrapped(chest)) {
        const TrapKind tk = chestTrapKind(chest);
        setChestTrapped(chest, false);
        setChestTrapKnown(chest, true);

        Entity& p = playerMut();        switch (tk) {
            case TrapKind::Spike: {
                int dmg = rng.range(2, 5) + std::min(3, depth_ / 2);
                p.hp -= dmg;
                std::ostringstream ss;
                ss << "A NEEDLE TRAP JABS YOU! YOU TAKE " << dmg << ".";
                pushMsg(ss.str(), MessageKind::Combat, false);
                if (p.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "KILLED BY CHEST TRAP";
                    gameOver = true;
                }
                break;
            }
            case TrapKind::PoisonDart: {
                int dmg = rng.range(1, 2);
                p.hp -= dmg;
                p.effects.poisonTurns = std::max(p.effects.poisonTurns, rng.range(6, 12));
                std::ostringstream ss;
                ss << "POISON NEEDLES HIT YOU! YOU TAKE " << dmg << ".";
                pushMsg(ss.str(), MessageKind::Combat, false);
                pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                if (p.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "KILLED BY POISON CHEST TRAP";
                    gameOver = true;
                }
                break;
            }
            case TrapKind::Teleport: {
                pushMsg("A TELEPORT GLYPH FLARES FROM THE CHEST!", MessageKind::Warning, false);
                Vec2i dst = dung.randomFloor(rng, true);
                for (int tries = 0; tries < 200; ++tries) {
                    dst = dung.randomFloor(rng, true);
                    if (!entityAt(dst.x, dst.y) && dst != dung.stairsUp && dst != dung.stairsDown) break;
                }
                p.pos = dst;
                recomputeFov();
                break;
            }
            case TrapKind::Alarm: {
                pushMsg("AN ALARM BLARES FROM THE CHEST!", MessageKind::Warning, false);
                // The alarm reveals the chest's location to the whole floor.
                alertMonstersTo(pos, 0);
                break;
            }
            case TrapKind::Web: {
                const int turns = rng.range(4, 7) + std::min(6, depth_ / 2);
                p.effects.webTurns = std::max(p.effects.webTurns, turns);
                pushMsg("STICKY WEBBING EXPLODES OUT OF THE CHEST!", MessageKind::Warning, true);
                break;
            }
            case TrapKind::ConfusionGas: {
                const int turns = rng.range(8, 14) + std::min(6, depth_ / 2);
                p.effects.confusionTurns = std::max(p.effects.confusionTurns, turns);
                pushMsg("A NOXIOUS GAS BURSTS FROM THE CHEST!", MessageKind::Warning, true);
                pushMsg("YOU FEEL CONFUSED!", MessageKind::Warning, true);
                emitNoise(pos, 8);
                break;
            }
            default:
                break;
        }
    }

    if (gameOver) {
        // Don't generate loot if the trap killed the player.
        return true;
    }

    // Loot: gold + a few items based on tier and depth.
    auto dropItemHere = [&](ItemKind k, int count = 1, int enchant = 0) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        it.enchant = enchant;
        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

        // Roll BUC (blessed/uncursed/cursed) for gear; and light enchant chance on deeper floors.
        if (isWearableGear(k)) {
            const RoomType rt = roomTypeAt(dung, pos);
            it.buc = rollBucForGear(rng, depth_, rt);

            if (it.enchant == 0 && depth_ >= 3) {
                float enchChance = 0.15f;
                if (rt == RoomType::Treasure || rt == RoomType::Vault || rt == RoomType::Secret) enchChance += 0.10f;
                if (rt == RoomType::Lair) enchChance -= 0.05f;
                enchChance = std::max(0.05f, std::min(0.35f, enchChance));

                if (rng.chance(enchChance)) {
                    it.enchant = 1;
                    if (depth_ >= 6 && rng.chance(0.08f)) {
                        it.enchant = 2;
                    }
                }
            }
        }
        ground.push_back({it, pos});
    };

    const int tier = chestTier(chest);
    int goldBase = rng.range(8, 16) + depth_ * 4;
    if (tier == 1) goldBase = static_cast<int>(goldBase * 1.5f);
    if (tier >= 2) goldBase = goldBase * 2;
    dropItemHere(ItemKind::Gold, goldBase);

    int rolls = 1 + tier;
    if (depth_ >= 4 && rng.chance(0.50f)) rolls += 1;

    for (int r = 0; r < rolls; ++r) {
        int roll = rng.range(0, 143);

        if (roll < 16) {
            // Weapons
            // Weighted: swords/axes are most common, pickaxes are rarer.
            int wroll = rng.range(0, 99);
            ItemKind wk = (wroll < 45) ? ItemKind::Sword : (wroll < 80) ? ItemKind::Axe : ItemKind::Pickaxe;
            int ench = (rng.chance(0.25f + 0.10f * tier)) ? rng.range(1, 1 + tier) : 0;
            dropItemHere(wk, 1, ench);
        } else if (roll < 34) {
            // Armor
            ItemKind ak = (roll < 26) ? ItemKind::ChainArmor : ItemKind::PlateArmor;
            int ench = (rng.chance(0.25f + 0.10f * tier)) ? rng.range(1, 1 + tier) : 0;
            dropItemHere(ak, 1, ench);
        } else if (roll < 38) {
            // Rings (rare)
            int rr = rng.range(0, 99);
            ItemKind rk = ItemKind::RingProtection;
            if (rr < 40) rk = ItemKind::RingProtection;
            else if (rr < 65) rk = ItemKind::RingMight;
            else if (rr < 90) rk = ItemKind::RingAgility;
            else rk = ItemKind::RingFocus;
            int ench = (rng.chance(0.20f + 0.08f * tier)) ? rng.range(1, 1 + tier) : 0;
            dropItemHere(rk, 1, ench);
        } else if (roll < 48) {
            ItemKind wk;
            if (depth_ >= 6 && tier >= 1 && rng.chance(0.12f)) {
                wk = ItemKind::WandFireball;
            } else {
                wk = rng.chance(0.30f) ? ItemKind::WandDigging : ItemKind::WandSparks;
            }
            dropItemHere(wk, 1);
        } else if (roll < 60) {
            dropItemHere(ItemKind::PotionStrength, rng.range(1, 2));
        } else if (roll < 78) {
            dropItemHere(ItemKind::PotionHealing, rng.range(1, 2));
        } else if (roll < 90) {
            dropItemHere(ItemKind::PotionAntidote, rng.range(1, 2));
        } else if (roll < 100) {
            dropItemHere(ItemKind::PotionRegeneration, 1);
        } else if (roll < 108) {
            dropItemHere(ItemKind::PotionShielding, 1);
        } else if (roll < 116) {
            dropItemHere(ItemKind::PotionHaste, 1);
        } else if (roll < 124) {
            const ItemKind pk = rng.chance(0.25f) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
            dropItemHere(pk, 1);
        } else if (roll < 130) {
            dropItemHere(ItemKind::ScrollMapping, 1);
        } else if (roll < 134) {
            dropItemHere(ItemKind::ScrollTeleport, 1);
        } else if (roll < 136) {
            dropItemHere(ItemKind::ScrollEnchantWeapon, 1);
        } else if (roll < 138) {
            dropItemHere(ItemKind::ScrollEnchantArmor, 1);
        } else if (roll < 142) {
            dropItemHere(ItemKind::ScrollRemoveCurse, 1);
        } else {
            int pick = rng.range(0, 3);
            ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                      : (pick == 1) ? ItemKind::ScrollDetectTraps
                                      : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                    : ItemKind::ScrollKnock;
            dropItemHere(sk, 1);
        }
    }

    // Turn the chest into a decorative open chest.
    chest.kind = ItemKind::ChestOpen;
    chest.charges = CHEST_FLAG_OPENED;

    // Respect auto-pickup preference after loot spills out (mostly useful for gold).
    (void)autoPickupAtPlayer();

    return true;
}

bool Game::pickupAtPlayer() {
    Vec2i ppos = player().pos;

    std::vector<size_t> idxs;
    for (size_t i = 0; i < ground.size(); ++i) {
        if (ground[i].pos == ppos) idxs.push_back(i);
    }
    if (idxs.empty()) {
        pushMsg("NOTHING HERE.", MessageKind::Info, true);
        return false;
    }

    // Chests are not pick-up items.
    bool hasPickable = false;
    for (size_t gi : idxs) {
        if (gi < ground.size() && !isChestKind(ground[gi].item.kind)) {
            hasPickable = true;
            break;
        }
    }
    if (!hasPickable) {
        pushMsg("NOTHING TO PICK UP.", MessageKind::Info, true);
        return false;
    }

    const int maxInv = 26;
    bool pickedAny = false;

    // Pick up in reverse order so erase indices stay valid.
    for (size_t k = idxs.size(); k-- > 0;) {
        size_t gi = idxs[k];
        if (gi >= ground.size()) continue;

        Item it = ground[gi].item;

        if (isChestKind(it.kind)) {
            // Skip non-pickable world items.
            continue;
        }

        std::string msg;
        const bool inShop = playerInShop();
        const bool isShopStockHere = (inShop && it.shopPrice > 0 && it.shopDepth == depth_);
        if (isShopStockHere && anyPeacefulShopkeeper(ents, playerId_)) {
            const int cost = totalShopPrice(it);
            Item named = it;
            named.shopPrice = 0;
            named.shopDepth = 0;

            if (spendGoldFromInv(inv, cost)) {
                it.shopPrice = 0;
                it.shopDepth = 0;
                msg = "YOU BUY " + displayItemName(it) + " FOR " + std::to_string(cost) + " GOLD.";
            } else {
                // Not enough gold: you can still pick up, but you now OWE the shop.
                msg = "YOU PICK UP " + displayItemName(named) + ". YOU OWE " + std::to_string(cost) + " GOLD.";
            }
        } else {
            msg = "YOU PICK UP " + displayItemName(it) + ".";
        }

        if (tryStackItem(inv, it)) {
            // stacked
            pickedAny = true;
            pushMsg(msg, MessageKind::Loot, true);
            if (it.kind == ItemKind::AmuletYendor) {
                pushMsg("YOU HAVE FOUND THE AMULET OF YENDOR! RETURN TO THE EXIT (<) TO WIN.", MessageKind::Success, true);
            }
            ground.erase(ground.begin() + static_cast<long>(gi));
            continue;
        }

        if (static_cast<int>(inv.size()) >= maxInv) {
            pushMsg("YOUR PACK IS FULL.", MessageKind::Warning, true);
            break;
        }

        inv.push_back(it);
        pickedAny = true;
        pushMsg(msg, MessageKind::Loot, true);
        if (it.kind == ItemKind::AmuletYendor) {
            pushMsg("YOU HAVE FOUND THE AMULET OF YENDOR! RETURN TO THE EXIT (<) TO WIN.", MessageKind::Success, true);
        }
        ground.erase(ground.begin() + static_cast<long>(gi));
    }

    return pickedAny;
}

bool Game::dropSelected() {
    if (inv.empty()) {
        pushMsg("NOTHING TO DROP.");
        return false;
    }

    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    // Cursed equipped items can't be removed/dropped (NetHack-style "welded" gear).
    if (it.buc < 0 && (it.id == equipMeleeId || it.id == equipRangedId || it.id == equipArmorId || it.id == equipRing1Id || it.id == equipRing2Id)) {
        if (it.id == equipMeleeId) pushMsg("YOUR WEAPON IS CURSED AND WELDED TO YOUR HAND!", MessageKind::Warning, true);
        else if (it.id == equipRangedId) pushMsg("YOUR RANGED WEAPON IS CURSED AND WON'T LET GO!", MessageKind::Warning, true);
        else if (it.id == equipArmorId) pushMsg("YOUR ARMOR IS CURSED AND WON'T COME OFF!", MessageKind::Warning, true);
        else pushMsg("YOUR RING IS CURSED AND STUCK TO YOUR FINGER!", MessageKind::Warning, true);
        return false;
    }

    // Unequip if needed
    if (it.id == equipMeleeId) equipMeleeId = 0;
    if (it.id == equipRangedId) equipRangedId = 0;
    if (it.id == equipArmorId) equipArmorId = 0;
    if (it.id == equipRing1Id) equipRing1Id = 0;
    if (it.id == equipRing2Id) equipRing2Id = 0;

    Item drop = it;
    if (isStackable(it.kind) && it.count > 1) {
        drop.count = 1;
        it.count -= 1;
    } else {
        // remove whole item
        inv.erase(inv.begin() + invSel);
        invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    }

    std::string msg;
    const bool inShop = playerInShop();
    const bool peacefulShop = inShop && anyPeacefulShopkeeper(ents, playerId_);

    if (inShop && drop.shopPrice > 0 && drop.shopDepth == depth_) {
        // Returning unpaid goods to the same shop reduces your debt automatically.
        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU RETURN " + displayItemName(named) + ".";
    } else if (peacefulShop && drop.shopPrice <= 0 && itemCanBeSoldToShop(drop)) {
        const int perUnit = shopSellPricePerUnit(drop, depth_);
        const int gold = std::max(0, perUnit) * stackUnitsForPrice(drop);
        if (gold > 0) gainGoldToInv(inv, gold, nextItemId, rng);

        // The shop now owns the item and will resell it.
        drop.shopPrice = shopBuyPricePerUnit(drop, depth_);
        drop.shopDepth = depth_;

        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU SELL " + displayItemName(named) + " FOR " + std::to_string(gold) + " GOLD.";
    } else {
        msg = "YOU DROP " + displayItemName(drop) + ".";
    }

    GroundItem gi;
    gi.item = drop;
    gi.pos = player().pos;
    ground.push_back(gi);

    pushMsg(msg);
    return true;
}

bool Game::dropSelectedAll() {
    if (inv.empty()) {
        pushMsg("NOTHING TO DROP.");
        return false;
    }

    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    // Cursed equipped items can't be removed/dropped (NetHack-style "welded" gear).
    if (it.buc < 0 && (it.id == equipMeleeId || it.id == equipRangedId || it.id == equipArmorId || it.id == equipRing1Id || it.id == equipRing2Id)) {
        if (it.id == equipMeleeId) pushMsg("YOUR WEAPON IS CURSED AND WELDED TO YOUR HAND!", MessageKind::Warning, true);
        else if (it.id == equipRangedId) pushMsg("YOUR RANGED WEAPON IS CURSED AND WON'T LET GO!", MessageKind::Warning, true);
        else if (it.id == equipArmorId) pushMsg("YOUR ARMOR IS CURSED AND WON'T COME OFF!", MessageKind::Warning, true);
        else pushMsg("YOUR RING IS CURSED AND STUCK TO YOUR FINGER!", MessageKind::Warning, true);
        return false;
    }

    // Unequip if needed
    if (it.id == equipMeleeId) equipMeleeId = 0;
    if (it.id == equipRangedId) equipRangedId = 0;
    if (it.id == equipArmorId) equipArmorId = 0;
    if (it.id == equipRing1Id) equipRing1Id = 0;
    if (it.id == equipRing2Id) equipRing2Id = 0;

    Item drop = it;

    // Remove whole item/stack.
    inv.erase(inv.begin() + invSel);
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    std::string msg;
    const bool inShop = playerInShop();
    const bool peacefulShop = inShop && anyPeacefulShopkeeper(ents, playerId_);

    if (inShop && drop.shopPrice > 0 && drop.shopDepth == depth_) {
        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU RETURN " + displayItemName(named) + ".";
    } else if (peacefulShop && drop.shopPrice <= 0 && itemCanBeSoldToShop(drop)) {
        const int perUnit = shopSellPricePerUnit(drop, depth_);
        const int gold = std::max(0, perUnit) * stackUnitsForPrice(drop);
        if (gold > 0) gainGoldToInv(inv, gold, nextItemId, rng);

        drop.shopPrice = shopBuyPricePerUnit(drop, depth_);
        drop.shopDepth = depth_;

        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU SELL " + displayItemName(named) + " FOR " + std::to_string(gold) + " GOLD.";
    } else {
        msg = "YOU DROP " + displayItemName(drop) + ".";
    }

    GroundItem gi;
    gi.item = drop;
    gi.pos = player().pos;
    ground.push_back(gi);

    pushMsg(msg);
    return true;
}

bool Game::equipSelected() {
    if (inv.empty()) {
        pushMsg("NOTHING TO EQUIP.");
        return false;
    }
    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    const Item& it = inv[static_cast<size_t>(invSel)];
    const ItemDef& d = itemDef(it.kind);

    auto equippedItemCursed = [&](int id) -> bool {
        if (id == 0) return false;
        const int idx = findItemIndexById(inv, id);
        if (idx < 0) return false;
        return inv[static_cast<size_t>(idx)].buc < 0;
    };

    if (d.slot == EquipSlot::MeleeWeapon) {
        if (equipMeleeId == it.id) {
            if (it.buc < 0) {
                pushMsg("YOUR WEAPON IS CURSED AND WELDED TO YOUR HAND!", MessageKind::Warning, true);
                return false;
            }
            equipMeleeId = 0;
            pushMsg("YOU UNWIELD " + displayItemName(it) + ".");
        } else {
            if (equippedItemCursed(equipMeleeId)) {
                pushMsg("YOUR CURSED WEAPON WON'T LET GO!", MessageKind::Warning, true);
                return false;
            }
            equipMeleeId = it.id;
            pushMsg("YOU WIELD " + displayItemName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::RangedWeapon) {
        if (equipRangedId == it.id) {
            if (it.buc < 0) {
                pushMsg("YOUR RANGED WEAPON IS CURSED AND WON'T LET GO!", MessageKind::Warning, true);
                return false;
            }
            equipRangedId = 0;
            pushMsg("YOU UNEQUIP " + displayItemName(it) + ".");
        } else {
            if (equippedItemCursed(equipRangedId)) {
                pushMsg("YOUR CURSED RANGED WEAPON WON'T LET GO!", MessageKind::Warning, true);
                return false;
            }
            equipRangedId = it.id;
            pushMsg("YOU READY " + displayItemName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::Armor) {
        if (equipArmorId == it.id) {
            if (it.buc < 0) {
                pushMsg("YOUR ARMOR IS CURSED AND WON'T COME OFF!", MessageKind::Warning, true);
                return false;
            }
            equipArmorId = 0;
            pushMsg("YOU REMOVE " + displayItemName(it) + ".");
        } else {
            if (equippedItemCursed(equipArmorId)) {
                pushMsg("YOUR CURSED ARMOR WON'T COME OFF!", MessageKind::Warning, true);
                return false;
            }
            equipArmorId = it.id;
            pushMsg("YOU WEAR " + displayItemName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::Ring) {
        auto ringNameById = [&](int id) -> std::string {
            if (id == 0) return std::string("(NONE)");
            const int idx = findItemIndexById(inv, id);
            if (idx < 0) return std::string("(MISSING)");
            return displayItemName(inv[static_cast<size_t>(idx)]);
        };

        auto removeRing = [&](int& slotId) -> bool {
            if (slotId == 0) return false;
            const int idx = findItemIndexById(inv, slotId);
            if (idx < 0) {
                slotId = 0;
                return true;
            }
            const Item& worn = inv[static_cast<size_t>(idx)];
            if (worn.buc < 0) {
                pushMsg("YOUR RING IS CURSED AND STUCK TO YOUR FINGER!", MessageKind::Warning, true);
                return false;
            }
            slotId = 0;
            pushMsg("YOU REMOVE " + displayItemName(worn) + ".");
            return true;
        };

        // Toggle off if the selected ring is already worn.
        if (equipRing1Id == it.id) {
            return removeRing(equipRing1Id);
        }
        if (equipRing2Id == it.id) {
            return removeRing(equipRing2Id);
        }

        // Prefer an empty slot.
        if (equipRing1Id == 0) {
            equipRing1Id = it.id;
            pushMsg("YOU PUT ON " + displayItemName(it) + ".");
            return true;
        }
        if (equipRing2Id == 0) {
            equipRing2Id = it.id;
            pushMsg("YOU PUT ON " + displayItemName(it) + ".");
            return true;
        }

        // Both slots are filled: replace the first removable ring.
        if (!equippedItemCursed(equipRing1Id)) {
            std::string oldName = ringNameById(equipRing1Id);
            equipRing1Id = it.id;
            pushMsg("YOU SWAP " + oldName + " FOR " + displayItemName(it) + ".");
            return true;
        }
        if (!equippedItemCursed(equipRing2Id)) {
            std::string oldName = ringNameById(equipRing2Id);
            equipRing2Id = it.id;
            pushMsg("YOU SWAP " + oldName + " FOR " + displayItemName(it) + ".");
            return true;
        }

        pushMsg("BOTH YOUR RINGS ARE CURSED AND WON'T BUDGE!", MessageKind::Warning, true);
        return false;
    }

    pushMsg("YOU CAN'T EQUIP THAT.");
    return false;
}

bool Game::useSelected() {
    if (inv.empty()) {
        pushMsg("NOTHING TO USE.", MessageKind::Info, true);
        return false;
    }
    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    auto consumeOneStackable = [&]() {
        if (!isStackable(it.kind)) return;
        it.count -= 1;
        if (it.count <= 0) {
            inv.erase(inv.begin() + invSel);
            invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
        }
    };

    if (it.kind == ItemKind::PotionHealing) {
        Entity& p = playerMut();        int heal = itemDef(it.kind).healAmount;
        int before = p.hp;
        p.hp = std::min(p.hpMax, p.hp + heal);

        std::ostringstream ss;
        ss << "YOU DRINK A POTION. HP " << before << "->" << p.hp << ".";
        pushMsg(ss.str(), MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionStrength) {
        // Potions can be blessed/uncursed/cursed; reflect that in how many talent points are gained.
        int delta = 1;
        if (it.buc > 0) delta = 2;
        else if (it.buc < 0) delta = -1;

        talentMight_ = clampi(talentMight_ + delta, -5, 50);

        std::ostringstream ss;
        if (delta > 0) {
            ss << "YOU FEEL STRONGER! MIGHT IS NOW " << talentMight_ << ".";
            pushMsg(ss.str(), MessageKind::Success, true);
        } else {
            ss << "YOU FEEL WEAKER... MIGHT IS NOW " << talentMight_ << ".";
            pushMsg(ss.str(), MessageKind::Warning, true);
        }

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }


    if (it.kind == ItemKind::ScrollTeleport) {
        // Teleport to a random free floor.
        Vec2i prevPos = player().pos;
        Vec2i dst = prevPos;

        for (int tries = 0; tries < 2000; ++tries) {
            Vec2i p = dung.randomFloor(rng, true);
            if (entityAt(p.x, p.y)) continue;
            dst = p;
            break;
        }

        playerMut().pos = dst;

        pushMsg("YOU READ A SCROLL. YOU VANISH!", MessageKind::Info, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        recomputeFov();

        const bool wasInShop = (roomTypeAt(dung, prevPos) == RoomType::Shop);
        const bool nowInShop = (roomTypeAt(dung, dst) == RoomType::Shop);
        if (wasInShop && !nowInShop) {
            const int debt = shopDebtThisDepth();
            if (debt > 0 && anyPeacefulShopkeeper(ents, playerId_)) {
                setShopkeepersAlerted(ents, playerId_, dst, true);
                pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
            }
        }
        return true;
    }

    if (it.kind == ItemKind::ScrollMapping) {
        dung.revealAll();
        pushMsg("THE DUNGEON MAP IS REVEALED.", MessageKind::Info, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        recomputeFov();
        return true;
    }

    if (it.kind == ItemKind::ScrollDetectTraps) {
        (void)markIdentified(it.kind, false);

        int newly = 0;
        int total = 0;

        for (auto& tr : trapsCur) {
            total += 1;
            if (!tr.discovered) newly += 1;
            tr.discovered = true;
        }

        // Chests can also be trapped; reveal those too.
        for (auto& gi : ground) {
            if (gi.item.kind != ItemKind::Chest) continue;
            if (!chestTrapped(gi.item)) continue;
            total += 1;
            if (!chestTrapKnown(gi.item)) newly += 1;
            setChestTrapKnown(gi.item, true);
        }

        if (total == 0) {
            pushMsg("YOU SENSE NO TRAPS.", MessageKind::Info, true);
        } else if (newly == 0) {
            pushMsg("YOU SENSE NO NEW TRAPS.", MessageKind::Info, true);
        } else {
            std::ostringstream ss;
            ss << "YOU SENSE " << newly << " TRAP" << (newly == 1 ? "" : "S") << "!";
            pushMsg(ss.str(), MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollDetectSecrets) {
        (void)markIdentified(it.kind, false);

        int newly = 0;
        for (auto& t : dung.tiles) {
            if (t.type == TileType::DoorSecret) {
                t.type = TileType::DoorClosed;
                t.explored = true; // show on the map once discovered
                newly++;
            }
        }

        if (newly == 0) {
            pushMsg("YOU SENSE NO SECRET DOORS.", MessageKind::Info, true);
        } else {
            std::ostringstream ss;
            ss << "YOU SENSE " << newly << " SECRET DOOR" << (newly == 1 ? "" : "S") << "!";
            pushMsg(ss.str(), MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }


    if (it.kind == ItemKind::ScrollKnock) {
        (void)markIdentified(it.kind, false);

        Entity& p = playerMut();        const int radius = 6;
        int opened = 0;

        for (int y = p.pos.y - radius; y <= p.pos.y + radius; ++y) {
            for (int x = p.pos.x - radius; x <= p.pos.x + radius; ++x) {
                if (!dung.inBounds(x, y)) continue;
                int dx = std::abs(x - p.pos.x);
                int dy = std::abs(y - p.pos.y);
                int cheb = std::max(dx, dy);
                if (cheb > radius) continue;

                if (dung.isDoorLocked(x, y)) {
                    dung.unlockDoor(x, y);
                    dung.openDoor(x, y);
                    opened++;
                }
            }
        }

        // Also unlock nearby chests.
        for (auto& gi : ground) {
            if (gi.item.kind != ItemKind::Chest) continue;
            if (!chestLocked(gi.item)) continue;
            int dx = std::abs(gi.pos.x - p.pos.x);
            int dy = std::abs(gi.pos.y - p.pos.y);
            int cheb = std::max(dx, dy);
            if (cheb > radius) continue;
            setChestLocked(gi.item, false);
            opened += 1;
        }

        if (opened == 0) {
            pushMsg("NOTHING SEEMS TO HAPPEN.", MessageKind::Info, true);
        } else if (opened == 1) {
            pushMsg("YOU HEAR A LOCK CLICK OPEN.", MessageKind::System, true);
        } else {
            pushMsg("YOU HEAR A CHORUS OF LOCKS CLICK OPEN.", MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollRemoveCurse) {
        (void)markIdentified(it.kind, false);

        int uncursed = 0;
        for (auto& invIt : inv) {
            if (!isWearableGear(invIt.kind)) continue;
            if (invIt.buc < 0) {
                invIt.buc = 0;
                uncursed++;
            }
        }

        if (uncursed == 0) {
            pushMsg("NOTHING SEEMS TO HAPPEN.", MessageKind::Info, true);
        } else {
            pushMsg("YOU FEEL A MALEVOLENT WEIGHT LIFT FROM YOUR GEAR.", MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollConfusion) {
        Entity& p = playerMut();

        int affected = 0;
        for (Entity& e : ents) {
            if (e.id == p.id) continue;
            if (e.kind == EntityKind::Shopkeeper) continue;
            if (e.hp <= 0) continue;
            if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
            if (!dung.at(e.pos.x, e.pos.y).visible) continue;

            const int turns = rng.range(6, 12) + std::min(6, depth_ / 2);
            e.effects.confusionTurns = std::max(e.effects.confusionTurns, turns);
            e.alerted = true;
            affected++;
        }

        if (affected > 0) pushMsg("THE AIR SHIMMERS. YOUR FOES LOOK CONFUSED!", MessageKind::Success, true);
        else pushMsg("NOTHING SEEMS TO HAPPEN.", MessageKind::Info, true);

        emitNoise(p.pos, 4);

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionAntidote) {
        Entity& p = playerMut();        if (p.effects.poisonTurns > 0) {
            p.effects.poisonTurns = 0;
            pushMsg("YOU FEEL THE POISON LEAVE YOUR BODY.", MessageKind::Success, true);
        } else {
            pushMsg("YOU FEEL CLEAN.", MessageKind::Info, true);
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionRegeneration) {
        Entity& p = playerMut();        p.effects.regenTurns = std::max(p.effects.regenTurns, 18);
        pushMsg("YOUR WOUNDS BEGIN TO KNIT.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionShielding) {
        Entity& p = playerMut();        p.effects.shieldTurns = std::max(p.effects.shieldTurns, 14);
        pushMsg("YOU FEEL PROTECTED.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionHaste) {
        Entity& p = playerMut();        p.effects.hasteTurns = std::min(40, p.effects.hasteTurns + 6);
        hastePhase = false; // ensure the next action is the "free" haste action
        pushMsg("YOU FEEL QUICK!", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionVision) {
        Entity& p = playerMut();        p.effects.visionTurns = std::min(60, p.effects.visionTurns + 20);
        pushMsg("YOUR EYES SHINE WITH INNER LIGHT.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        recomputeFov();
        return true;
    }

    if (it.kind == ItemKind::PotionInvisibility) {
        Entity& p = playerMut();        p.effects.invisTurns = std::min(60, p.effects.invisTurns + 18);
        pushMsg("YOU FADE FROM SIGHT!", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionClarity) {
        Entity& p = playerMut();
        const bool wasConfused = (p.effects.confusionTurns > 0);
        p.effects.confusionTurns = 0;
        if (wasConfused) pushMsg("YOUR MIND CLEARS.", MessageKind::Success, true);
        else pushMsg("YOU FEEL FOCUSED.", MessageKind::Info, true);

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::Torch) {
        // Light a torch: consumes one TORCH from the stack and creates a LIT TORCH item that burns over time.
        // (The LIT TORCH can be dropped to create a stationary light source.)
        const int fuel = 180 + rng.range(0, 120);

        // Consume one torch from the selected stack first (to avoid reference invalidation from inv push_back).
        if (it.count > 1) {
            it.count -= 1;
        } else {
            inv.erase(inv.begin() + static_cast<std::vector<Item>::difference_type>(invSel));
            if (invSel >= static_cast<int>(inv.size())) invSel = static_cast<int>(inv.size()) - 1;
        }

        Item lit;
        lit.id = nextItemId++;
        lit.kind = ItemKind::TorchLit;
        lit.count = 1;
        lit.enchant = 0;
        lit.charges = fuel;
        lit.spriteSeed = rng.nextU32();

        inv.push_back(lit);

        pushMsg("YOU LIGHT A TORCH.", MessageKind::System, true);
        // The flare is small but noticeable.
        emitNoise(player().pos, 4);

        // Lighting changes sight in dark levels.
        recomputeFov();
        return true;
    }

    if (it.kind == ItemKind::ScrollEnchantWeapon) {
        int idx = equippedMeleeIndex();
        if (idx < 0) {
            pushMsg("YOUR HANDS TINGLE... BUT NOTHING HAPPENS.", MessageKind::Info, true);
        } else {
            Item& w = inv[static_cast<size_t>(idx)];
            w.enchant += 1;
            pushMsg("YOUR WEAPON GLOWS BRIEFLY.", MessageKind::Success, true);
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollEnchantArmor) {
        int idx = equippedArmorIndex();
        if (idx < 0) {
            pushMsg("YOUR SKIN TINGLES... BUT NOTHING HAPPENS.", MessageKind::Info, true);
        } else {
            Item& a = inv[static_cast<size_t>(idx)];
            a.enchant += 1;
            pushMsg("YOUR ARMOR GLOWS BRIEFLY.", MessageKind::Success, true);
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollIdentify) {
        // Using an identify scroll reveals the true name of one unidentified potion/scroll.
        // If multiple candidates exist, the player can choose which one to learn.
        (void)markIdentified(it.kind, false);

        if (!identifyItemsEnabled) {
            pushMsg("YOUR MIND FEELS CLEAR.", MessageKind::Info, true);
            consumeOneStackable();
            return true;
        }

        std::vector<ItemKind> candidates;
        candidates.reserve(16);
        auto seen = [&](ItemKind k) {
            for (ItemKind x : candidates) if (x == k) return true;
            return false;
        };

        for (const auto& invIt : inv) {
            if (!isIdentifiableKind(invIt.kind)) continue;
            if (invIt.kind == ItemKind::ScrollIdentify) continue;
            if (isIdentified(invIt.kind)) continue;
            if (!seen(invIt.kind)) candidates.push_back(invIt.kind);
        }

        if (candidates.empty()) {
            pushMsg("YOU STUDY THE SCROLL, BUT LEARN NOTHING NEW.", MessageKind::Info, true);
            consumeOneStackable();
            return true;
        }

        if (candidates.size() == 1) {
            (void)markIdentified(candidates[0], false);
            consumeOneStackable();
            return true;
        }

        // Multiple unknown kinds: consume the scroll now (reading takes the turn regardless).
        consumeOneStackable();

        // Enter a temporary inventory sub-mode so the player can choose.
        invIdentifyMode = true;

        // Move selection to the first eligible item to reduce friction.
        for (size_t i = 0; i < inv.size(); ++i) {
            const Item& cand = inv[i];
            if (!isIdentifiableKind(cand.kind)) continue;
            if (cand.kind == ItemKind::ScrollIdentify) continue;
            if (isIdentified(cand.kind)) continue;
            invSel = static_cast<int>(i);
            break;
        }

        pushMsg("SELECT AN ITEM TO IDENTIFY (ENTER = CHOOSE, ESC = RANDOM).", MessageKind::System, true);
        return true;
    }

    if (it.kind == ItemKind::FoodRation) {
        Entity& p = playerMut();        const ItemDef& d = itemDef(it.kind);

        const int beforeState = hungerStateFor(hunger, hungerMax);

        // Small heal (always), plus hunger restoration if enabled.
        if (d.healAmount > 0 && p.hp < p.hpMax) {
            p.hp = std::min(p.hpMax, p.hp + d.healAmount);
        }

        if (hungerEnabled_) {
            if (hungerMax <= 0) hungerMax = 800;
            hunger = std::min(hungerMax, hunger + d.hungerRestore);
        }

        const int afterState = hungerStateFor(hunger, hungerMax);
        if (hungerEnabled_) {
            if (beforeState >= 2 && afterState < 2) {
                pushMsg("YOU FEEL LESS STARVED.", MessageKind::System, true);
            } else if (beforeState >= 1 && afterState == 0) {
                pushMsg("YOU FEEL SATIATED.", MessageKind::System, true);
            }
        }

        // Sync the throttling state so we don't immediately re-announce hunger next tick.
        hungerStatePrev = hungerStateFor(hunger, hungerMax);

        pushMsg("YOU EAT A FOOD RATION.", MessageKind::Loot, true);
        consumeOneStackable();
        return true;
    }

    if (isCorpseKind(it.kind)) {
        Entity& p = playerMut();
        const ItemDef& d = itemDef(it.kind);

        // Corpse decay state (charges = remaining freshness in turns).
        const int ch = it.charges;
        const bool rotten = (ch <= 60);
        const bool stale = (ch <= 160);

        const int beforeState = hungerStateFor(hunger, hungerMax);

        if (ch <= 0) {
            pushMsg("THE CORPSE CRUMBLES INTO ROT.", MessageKind::Warning, true);
            consumeOneStackable();
            return true;
        }

        // Base nourishment/heal from ItemDef, scaled by freshness.
        int heal = d.healAmount;
        int restore = d.hungerRestore;
        if (rotten) {
            heal = std::max(0, heal - 1);
            restore = std::max(0, restore / 2);
        } else if (stale) {
            restore = std::max(0, (restore * 3) / 4);
        }

        // Apply the basic food effects.
        if (heal > 0 && p.hp < p.hpMax) {
            p.hp = std::min(p.hpMax, p.hp + heal);
        }
        if (hungerEnabled_) {
            if (hungerMax <= 0) hungerMax = 800;
            hunger = std::min(hungerMax, hunger + restore);
        }

        // Risk/bonus table.
        float poisonChance = 0.0f;
        int poisonTurns = 0;
        float confuseChance = 0.0f;
        int confuseTurns = 0;

        enum class Bonus { None, Regen, Haste, Vision, Shield, Strength };
        Bonus bonus = Bonus::None;
        float bonusChance = 0.0f;
        int bonusTurns = 0;
        int strengthInc = 0;

        switch (it.kind) {
            case ItemKind::CorpseGoblin:
                poisonChance = 0.10f; poisonTurns = 6;
                break;
            case ItemKind::CorpseOrc:
                poisonChance = 0.15f; poisonTurns = 6;
                break;
            case ItemKind::CorpseBat:
                poisonChance = 0.08f; poisonTurns = 5;
                bonus = Bonus::Haste; bonusChance = 0.18f; bonusTurns = 10;
                break;
            case ItemKind::CorpseSlime:
                poisonChance = 0.50f; poisonTurns = 10;
                confuseChance = 0.25f; confuseTurns = 12;
                break;
            case ItemKind::CorpseKobold:
                poisonChance = 0.12f; poisonTurns = 6;
                break;
            case ItemKind::CorpseWolf:
                poisonChance = 0.08f; poisonTurns = 6;
                bonus = Bonus::Regen; bonusChance = 0.20f; bonusTurns = 12;
                break;
            case ItemKind::CorpseTroll:
                poisonChance = 0.12f; poisonTurns = 8;
                bonus = Bonus::Regen; bonusChance = 1.00f; bonusTurns = 18;
                break;
            case ItemKind::CorpseWizard:
                poisonChance = 0.06f; poisonTurns = 6;
                confuseChance = 0.20f; confuseTurns = 12;
                bonus = Bonus::Vision; bonusChance = 0.35f; bonusTurns = 18;
                break;
            case ItemKind::CorpseSnake:
                poisonChance = 0.35f; poisonTurns = 10;
                break;
            case ItemKind::CorpseSpider:
                poisonChance = 0.40f; poisonTurns = 11;
                break;
            case ItemKind::CorpseOgre:
                poisonChance = 0.20f; poisonTurns = 8;
                bonus = Bonus::Strength; bonusChance = 0.08f; strengthInc = 1;
                break;
            case ItemKind::CorpseMimic:
                poisonChance = 0.22f; poisonTurns = 8;
                confuseChance = 0.18f; confuseTurns = 10;
                bonus = Bonus::Shield; bonusChance = 0.18f; bonusTurns = 14;
                break;
            case ItemKind::CorpseMinotaur:
                poisonChance = 0.25f; poisonTurns = 9;
                bonus = Bonus::Strength; bonusChance = 0.15f; strengthInc = 1;
                break;
            default:
                poisonChance = 0.18f; poisonTurns = 7;
                break;
        }

        // Freshness modifies risk/benefit.
        if (rotten) {
            poisonChance += 0.35f;
            confuseChance += 0.20f;
            bonusChance *= 0.25f;
        } else if (stale) {
            poisonChance += 0.15f;
            bonusChance *= 0.75f;
        }

        poisonChance = std::min(0.95f, poisonChance);
        confuseChance = std::min(0.80f, confuseChance);

        // Messaging.
        pushMsg("YOU EAT THE " + itemDisplayNameSingle(it.kind) + ".", MessageKind::Loot, true);
        if (rotten) {
            pushMsg("IT TASTES RANCID.", MessageKind::Warning, true);
        }

        // Apply negative effects.
        bool poisoned = false;
        if (poisonChance > 0.0f && rng.chance(poisonChance)) {
            const int extra = rotten ? 4 : (stale ? 1 : 0);
            const int turns = std::max(1, poisonTurns + extra);
            p.effects.poisonTurns = std::max(p.effects.poisonTurns, turns);
            poisoned = true;
        }
        if (confuseChance > 0.0f && rng.chance(confuseChance)) {
            const int extra = rotten ? 4 : 0;
            const int turns = std::max(1, confuseTurns + extra);
            p.effects.confusionTurns = std::max(p.effects.confusionTurns, turns);
            pushMsg("YOU FEEL CONFUSED!", MessageKind::Warning, true);
        }

        if (poisoned) {
            pushMsg("UGH... YOU FEEL SICK.", MessageKind::Warning, true);
        }

        // Apply a possible positive bonus.
        if (bonus != Bonus::None && bonusChance > 0.0f && rng.chance(bonusChance)) {
            switch (bonus) {
                case Bonus::Regen:
                    p.effects.regenTurns = std::max(p.effects.regenTurns, bonusTurns);
                    pushMsg("YOU FEEL A STRANGE VITALITY.", MessageKind::Success, true);
                    break;
                case Bonus::Haste:
                    p.effects.hasteTurns = std::max(p.effects.hasteTurns, bonusTurns);
                    pushMsg("YOUR BLOOD RUNS HOT.", MessageKind::Success, true);
                    break;
                case Bonus::Vision:
                    p.effects.visionTurns = std::max(p.effects.visionTurns, bonusTurns);
                    pushMsg("YOUR EYES SHARPEN.", MessageKind::Success, true);
                    break;
                case Bonus::Shield:
                    p.effects.shieldTurns = std::max(p.effects.shieldTurns, bonusTurns);
                    pushMsg("A PROTECTIVE AURA SURROUNDS YOU.", MessageKind::Success, true);
                    break;
                case Bonus::Strength:
                    if (strengthInc != 0) {
                        p.baseAtk += strengthInc;
                        std::ostringstream ss;
                        ss << "YOU FEEL STRONGER! ATK IS NOW " << p.baseAtk << ".";
                        pushMsg(ss.str(), MessageKind::Success, true);
                    }
                    break;
                default:
                    break;
            }
        }

        // Special: minotaur meat is powerful, but dangerous.
        if (it.kind == ItemKind::CorpseMinotaur && !rotten) {
            p.effects.shieldTurns = std::max(p.effects.shieldTurns, 16);
        }

        // Hunger feedback (mirrors Food Ration).
        const int afterState = hungerStateFor(hunger, hungerMax);
        if (hungerEnabled_) {
            if (beforeState >= 2 && afterState < 2) {
                pushMsg("YOU FEEL LESS STARVED.", MessageKind::System, true);
            } else if (beforeState >= 1 && afterState == 0) {
                pushMsg("YOU FEEL SATIATED.", MessageKind::System, true);
            }
        }

        // Sync the throttling state so we don't immediately re-announce hunger next tick.
        hungerStatePrev = hungerStateFor(hunger, hungerMax);

        consumeOneStackable();
        return true;
    }

    pushMsg("NOTHING HAPPENS.", MessageKind::Info, true);
    return false;
}

