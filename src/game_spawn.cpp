#include "game_internal.hpp"

Vec2i Game::randomFreeTileInRoom(const Room& r, int tries) {
    for (int i = 0; i < tries; ++i) {
        int x0 = rng.range(r.x + 1, std::max(r.x + 1, r.x + r.w - 2));
        int y0 = rng.range(r.y + 1, std::max(r.y + 1, r.y + r.h - 2));
        if (!dung.inBounds(x0, y0)) continue;
        TileType t = dung.at(x0, y0).type;
        if (!(t == TileType::Floor || t == TileType::StairsUp || t == TileType::StairsDown || t == TileType::DoorOpen)) continue;
        if (entityAt(x0, y0)) continue;
        return {x0, y0};
    }
    return {r.cx(), r.cy()};
}

void Game::spawnMonsters() {
    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    int nextGroup = 1000;

    auto addMonster = [&](EntityKind k, Vec2i pos, int groupId) {
        Entity e;
        e.id = nextEntityId++;
        e.kind = k;
        e.pos = pos;
        e.groupId = groupId;
        e.spriteSeed = rng.nextU32();
        e.lastKnownPlayerPos = player().pos;

        // Baselines per kind. Depth scaling happens below.
        switch (k) {
            case EntityKind::Goblin:
                e.hpMax = 7; e.baseAtk = 1; e.baseDef = 0;
                e.willFlee = true;
                break;
            case EntityKind::Orc:
                e.hpMax = 10; e.baseAtk = 2; e.baseDef = 1;
                e.willFlee = false;
                break;
            case EntityKind::Bat:
                e.hpMax = 5; e.baseAtk = 1; e.baseDef = 0;
                e.willFlee = true;
                break;
            case EntityKind::Slime:
                e.hpMax = 12; e.baseAtk = 2; e.baseDef = 1;
                e.willFlee = false;
                break;
            case EntityKind::SkeletonArcher:
                e.hpMax = 9; e.baseAtk = 2; e.baseDef = 1;
                e.willFlee = false;
                e.canRanged = true;
                // Ranged stats are stored per-entity (saved/loaded), so set them here on spawn.
                e.rangedRange = 8;
                e.rangedAtk = 6;
                e.rangedProjectile = ProjectileKind::Arrow;
                e.rangedAmmo = AmmoKind::Arrow;
                break;
            case EntityKind::KoboldSlinger:
                e.hpMax = 8; e.baseAtk = 2; e.baseDef = 0;
                e.willFlee = true;
                e.canRanged = true;
                e.rangedRange = 6;
                e.rangedAtk = 5;
                e.rangedProjectile = ProjectileKind::Rock;
                e.rangedAmmo = AmmoKind::Rock;
                break;
            case EntityKind::Wolf:
                e.hpMax = 6; e.baseAtk = 2; e.baseDef = 0;
                e.willFlee = false;
                e.packAI = true;
                break;
            case EntityKind::Troll:
                e.hpMax = 16; e.baseAtk = 4; e.baseDef = 2;
                e.willFlee = false;
                // Trolls regenerate slowly; makes them scary if you can't finish them quickly.
                e.regenChancePct = 25;
                e.regenAmount = 1;
                break;
            case EntityKind::Wizard:
                e.hpMax = 12; e.baseAtk = 3; e.baseDef = 1;
                e.willFlee = false;
                e.canRanged = true;
                e.rangedRange = 7;
                e.rangedAtk = 7;
                e.rangedProjectile = ProjectileKind::Spark;
                e.rangedAmmo = AmmoKind::None;
                break;
            case EntityKind::Snake:
                e.hpMax = 7; e.baseAtk = 2; e.baseDef = 0;
                e.willFlee = false;
                break;
            case EntityKind::Spider:
                e.hpMax = 8; e.baseAtk = 3; e.baseDef = 1;
                e.willFlee = false;
                break;
            case EntityKind::Ogre:
                e.hpMax = 18; e.baseAtk = 5; e.baseDef = 2;
                e.willFlee = false;
                break;
            case EntityKind::Mimic:
                e.hpMax = 14; e.baseAtk = 4; e.baseDef = 2;
                e.willFlee = false;
                break;
            case EntityKind::Shopkeeper:
                e.hpMax = 18; e.baseAtk = 6; e.baseDef = 4;
                e.willFlee = false;
                break;
            case EntityKind::Minotaur:
                e.hpMax = 38; e.baseAtk = 7; e.baseDef = 3;
                e.willFlee = false;
                break;
            default:
                e.hpMax = 6; e.baseAtk = 1; e.baseDef = 0;
                e.willFlee = true;
                break;
        }

        // Depth scaling. Keep early monsters relevant without letting endgame balloon out of control.
        int d = std::max(0, depth_ - 1);
        if (k == EntityKind::Goblin || k == EntityKind::Bat || k == EntityKind::Slime || k == EntityKind::Snake) {
            d = d / 2;
        }
        if (k == EntityKind::Minotaur) {
            // Boss-tier baseline already; scale a bit slower.
            d = std::max(0, depth_ - 6);
        }

        e.hpMax += d;
        e.hp = e.hpMax;
        e.baseAtk += (d / 3);
        e.baseDef += (d / 4);

        // Spawn with basic gear for humanoid-ish monsters.
        // This makes loot feel more "earned" (you can take what they were using),
        // and creates emergent difficulty when monsters pick up better weapons/armor.
        if (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k)) {
            const RoomType rt = roomTypeAt(dung, pos);

            auto makeGear = [&](ItemKind kind) -> Item {
                Item it;
                it.id = 1; // non-zero => present
                it.kind = kind;
                it.count = 1;
                it.spriteSeed = rng.nextU32();
                it.shopPrice = 0;
                it.shopDepth = 0;

                if (isWeapon(kind) || isArmor(kind)) {
                    it.buc = rollBucForGear(rng, depth_, rt);

                    // A little bit of enchantment scaling with depth.
                    if (depth_ >= 4 && rng.chance(0.18f)) {
                        it.enchant = 1;
                        if (depth_ >= 7 && rng.chance(0.07f)) it.enchant = 2;
                    }
                }

                return it;
            };

            switch (k) {
                case EntityKind::Goblin:
                    if (rng.chance(0.60f)) {
                        e.gearMelee = makeGear(ItemKind::Dagger);
                    }
                    break;

                case EntityKind::Orc:
                    if (rng.chance(0.80f)) {
                        const ItemKind wk = (depth_ >= 4 && rng.chance(0.25f)) ? ItemKind::Axe : ItemKind::Sword;
                        e.gearMelee = makeGear(wk);
                    }
                    if (rng.chance(0.30f)) {
                        const ItemKind ak = (depth_ >= 6 && rng.chance(0.20f)) ? ItemKind::ChainArmor : ItemKind::LeatherArmor;
                        e.gearArmor = makeGear(ak);
                    }
                    break;

                case EntityKind::SkeletonArcher:
                    if (rng.chance(0.55f)) {
                        e.gearMelee = makeGear(ItemKind::Dagger);
                    }
                    if (rng.chance(0.20f)) {
                        e.gearArmor = makeGear(ItemKind::ChainArmor);
                    }
                    break;

                case EntityKind::KoboldSlinger:
                    if (rng.chance(0.55f)) {
                        e.gearMelee = makeGear(ItemKind::Dagger);
                    }
                    break;

                case EntityKind::Wizard:
                    if (rng.chance(0.50f)) {
                        e.gearMelee = makeGear(ItemKind::Dagger);
                    }
                    if (depth_ >= 5 && rng.chance(0.15f)) {
                        e.gearArmor = makeGear(ItemKind::LeatherArmor);
                    }
                    break;

                default:
                    break;
            }
        }

        ents.push_back(e);
    };

    auto randomFreeTileInRoom = [&](const Room& r) {
        // 200 tries, then fallback to brute scan.
        for (int i = 0; i < 200; ++i) {
            const int x = rng.range(r.x + 1, r.x + r.w - 2);
            const int y = rng.range(r.y + 1, r.y + r.h - 2);
            Vec2i p{x, y};
            const auto& t = dung.at(p.x, p.y);
            if (t.type != TileType::Floor && t.type != TileType::StairsUp && t.type != TileType::StairsDown && t.type != TileType::DoorOpen) {
                continue;
            }
            if (entityAt(p.x, p.y)) continue;
            return p;
        }

        for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
            for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
                Vec2i p{x, y};
                const auto& t = dung.at(p.x, p.y);
                if (t.type != TileType::Floor && t.type != TileType::StairsUp && t.type != TileType::StairsDown && t.type != TileType::DoorOpen) {
                    continue;
                }
                if (entityAt(p.x, p.y)) continue;
                return p;
            }
        }

        // As a last resort, drop at center-ish.
        return Vec2i{r.x + r.w / 2, r.y + r.h / 2};
    };

    for (const Room& r : rooms) {
        const bool isStart = r.contains(dung.stairsUp.x, dung.stairsUp.y);
        int base = isStart ? 0 : 1;

        int depthTerm = (depth_ >= 3 ? 2 : 1);
        if (depth_ >= 7) depthTerm += 1;
        if (depth_ >= 9) depthTerm += 1;

        int n = rng.range(0, base + depthTerm);
        if (r.type == RoomType::Vault) n = rng.range(0, 1);

        for (int i = 0; i < n; ++i) {
            Vec2i p = randomFreeTileInRoom(r);

            EntityKind k = EntityKind::Goblin;
            const int roll = rng.range(0, 99);

            if (depth_ <= 1) {
                if (roll < 65) k = EntityKind::Goblin;
                else k = EntityKind::Orc;
            } else if (depth_ == 2) {
                if (roll < 35) k = EntityKind::Goblin;
                else if (roll < 55) k = EntityKind::Orc;
                else if (roll < 70) k = EntityKind::Bat;
                else if (roll < 85) k = EntityKind::Slime;
                else k = EntityKind::KoboldSlinger;
            } else if (depth_ == 3) {
                if (roll < 20) k = EntityKind::Orc;
                else if (roll < 35) k = EntityKind::SkeletonArcher;
                else if (roll < 50) k = EntityKind::Spider;
                else if (roll < 65) k = EntityKind::Snake;
                else if (roll < 80) k = EntityKind::Bat;
                else k = EntityKind::Wolf;
            } else if (depth_ <= 6) {
                // Midgame band (4-6): introduce heavy hitters, but keep variety.
                if (roll < 12) k = EntityKind::Orc;
                else if (roll < 22) k = EntityKind::SkeletonArcher;
                else if (roll < 32) k = EntityKind::Spider;
                else if (roll < 42) k = EntityKind::Wolf;
                else if (roll < 52) k = EntityKind::Slime;
                else if (roll < 60) k = EntityKind::Bat;
                else if (roll < 70) k = EntityKind::Snake;
                else if (roll < 80) k = EntityKind::KoboldSlinger;
                else if (roll < 90) k = EntityKind::Troll;
                else if (roll < 96) k = EntityKind::Ogre;
                else if (roll < 100) k = (depth_ >= 6 ? EntityKind::Mimic : EntityKind::Wizard);
            } else {
                // Deep band (7+): higher threat density and occasional rarities.
                if (roll < 12) k = EntityKind::Orc;
                else if (roll < 24) k = EntityKind::SkeletonArcher;
                else if (roll < 34) k = EntityKind::Spider;
                else if (roll < 46) k = EntityKind::Troll;
                else if (roll < 56) k = EntityKind::Ogre;
                else if (roll < 66) k = EntityKind::Mimic;
                else if (roll < 76) k = EntityKind::Wizard;
                else if (roll < 86) k = EntityKind::Wolf;
                else if (roll < 92) k = EntityKind::KoboldSlinger;
                else if (roll < 96) k = EntityKind::Slime;
                else if (roll < 98) k = EntityKind::Snake;
                else {
                    // Keep Minotaurs rare and avoid spawning extras on the final floor.
                    if (depth_ < QUEST_DEPTH && depth_ >= 8) k = EntityKind::Minotaur;
                    else k = EntityKind::Bat;
                }
            }

            if (k == EntityKind::Wolf) {
                addMonster(k, p, nextGroup++);
            } else {
                addMonster(k, p, 0);
            }
        }

        // Guards in high-value rooms.
        if (r.type == RoomType::Secret || r.type == RoomType::Treasure || r.type == RoomType::Vault) {
            int guardians = (r.type == RoomType::Vault) ? rng.range(0, 1) : rng.range(0, 2);
            for (int i = 0; i < guardians; ++i) {
                Vec2i p = randomFreeTileInRoom(r);
                EntityKind k = EntityKind::Troll;
                const int roll = rng.range(0, 99);

                if (depth_ >= 7) {
                    if (roll < 20) k = EntityKind::Wizard;
                    else if (roll < 35) k = EntityKind::Ogre;
                    else if (roll < 50) k = EntityKind::Troll;
                    else if (roll < 65) k = EntityKind::Mimic;
                    else if (roll < 72) k = EntityKind::Spider;
                    else if (roll < 75) {
                        // Keep Minotaurs off the final floor; the endgame boss is different now.
                        k = (depth_ == QUEST_DEPTH) ? EntityKind::Troll : EntityKind::Minotaur;
                    }
                    else k = EntityKind::SkeletonArcher;
                } else if (depth_ >= 4) {
                    if (roll < 25) k = EntityKind::Wizard;
                    else if (roll < 55) k = EntityKind::Ogre;
                    else k = EntityKind::Troll;
                } else if (depth_ == 3) {
                    if (roll < 25) k = EntityKind::Orc;
                    else if (roll < 60) k = EntityKind::SkeletonArcher;
                    else k = EntityKind::Spider;
                } else {
                    if (roll < 50) k = EntityKind::Goblin;
                    else k = EntityKind::Orc;
                }

                addMonster(k, p, 0);
            }
        }

        // Lairs: wolf packs.
        if (r.type == RoomType::Lair) {
            const int pack = rng.range(2, 5);
            const int gid = nextGroup++;
            for (int i = 0; i < pack; ++i) {
                Vec2i p = randomFreeTileInRoom(r);
                addMonster(EntityKind::Wolf, p, gid);
            }
        }
    }

    // Milestone spawns (outside the per-room loop so they stay stable).
    const Room* treasure = nullptr;
    for (const Room& r : rooms) {
        if (r.type == RoomType::Treasure) {
            treasure = &r;
            break;
        }
    }

    if (treasure) {
        // Midpoint: a mini-boss to signal the run's second half.
        if (depth_ == MIDPOINT_DEPTH) {
            Vec2i p = randomFreeTileInRoom(*treasure);
            addMonster(EntityKind::Ogre, p, 0);

            // A couple of guards nearby.
            for (int i = 0; i < 2; ++i) {
                Vec2i q = randomFreeTileInRoom(*treasure);
                addMonster(EntityKind::Wolf, q, nextGroup++);
            }
        }

        // Penultimate floor: the Minotaur guards the central hoard.
        if (depth_ == QUEST_DEPTH - 1) {
            Vec2i p = randomFreeTileInRoom(*treasure);
            addMonster(EntityKind::Minotaur, p, 0);
        }

        // Final floor: a hostile archwizard guards the Amulet.
        if (depth_ == QUEST_DEPTH) {
            Vec2i p = randomFreeTileInRoom(*treasure);
            addMonster(EntityKind::Wizard, p, 0);

            // Upgrade into an "archwizard" (stronger ranged profile).
            if (!ents.empty()) {
                Entity& w = ents.back();
                if (w.kind == EntityKind::Wizard) {
                    w.rangedProjectile = ProjectileKind::Fireball;
                    w.rangedRange = std::max(w.rangedRange, 6);
                    w.rangedAtk += 2;
                    w.hpMax += 6;
                    w.hp = std::min(w.hpMax, w.hp + 6);
                }
            }
        }
    }
}

void Game::spawnItems() {
    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    auto dropItemAt = [&](ItemKind k, Vec2i pos, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

        // Roll BUC (blessed/uncursed/cursed) for gear; and light enchant chance on deeper floors.
        if (isWeapon(k) || isArmor(k)) {
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

        GroundItem gi;
        gi.item = it;
        gi.pos = pos;
        ground.push_back(gi);
    };

    auto dropShopItemAt = [&](ItemKind k, Vec2i pos, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.enchant = 0;
        it.buc = 0;
        it.charges = 0;
        it.spriteSeed = rng.nextU32();
        it.shopPrice = 0;
        it.shopDepth = 0;

        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

        // Shops sell mostly "clean" gear.
        RoomType rt = RoomType::Shop;
        if (isWeapon(k) || isArmor(k)) {
            it.buc = rollBucForGear(rng, depth_, rt);
            // A slightly higher chance of +1 items compared to the floor.
            float enchChance = (depth_ >= 2) ? 0.22f : 0.12f;
            enchChance += std::min(0.18f, depth_ * 0.02f);
            if (rng.chance(enchChance)) {
                it.enchant = 1;
                if (depth_ >= 6 && rng.chance(0.08f)) it.enchant = 2;
            }
        }

        it.shopPrice = shopBuyPricePerUnit(it, depth_);
        it.shopDepth = depth_;

        GroundItem gi;
        gi.item = it;
        gi.pos = pos;
        ground.push_back(gi);
    };

    auto dropGoodItem = [&](const Room& r) {
        // Treasure rooms are where you find the "spicy" gear.
        // Slightly expanded table to accommodate new gear.
        int roll = rng.range(0, 163);

        if (roll < 18) dropItemAt(ItemKind::Sword, randomFreeTileInRoom(r));
        else if (roll < 30) dropItemAt(ItemKind::Axe, randomFreeTileInRoom(r));
        else if (roll < 38) dropItemAt(ItemKind::Pickaxe, randomFreeTileInRoom(r));
        else if (roll < 52) dropItemAt(ItemKind::ChainArmor, randomFreeTileInRoom(r));
        else if (roll < 58) dropItemAt(ItemKind::PlateArmor, randomFreeTileInRoom(r));
        else if (roll < 70) dropItemAt(ItemKind::WandSparks, randomFreeTileInRoom(r));
        else if (roll < 78) dropItemAt(ItemKind::WandDigging, randomFreeTileInRoom(r));
        else if (roll < 82) {
            // Fireball wand is a mid/deep treasure find.
            ItemKind wk = (depth_ >= 5) ? ItemKind::WandFireball : ItemKind::WandSparks;
            dropItemAt(wk, randomFreeTileInRoom(r));
        }
        else if (roll < 92) dropItemAt(ItemKind::Sling, randomFreeTileInRoom(r));
        else if (roll < 104) dropItemAt(ItemKind::PotionStrength, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 116) dropItemAt(ItemKind::PotionHealing, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 126) dropItemAt(ItemKind::PotionAntidote, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 130) dropItemAt(ItemKind::PotionClarity, randomFreeTileInRoom(r), 1);
        else if (roll < 132) dropItemAt(ItemKind::PotionRegeneration, randomFreeTileInRoom(r), 1);
        else if (roll < 136) dropItemAt(ItemKind::PotionShielding, randomFreeTileInRoom(r), 1);
        else if (roll < 140) dropItemAt(ItemKind::PotionHaste, randomFreeTileInRoom(r), 1);
        else if (roll < 144) {
            const ItemKind pk = rng.chance(0.25f) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
            dropItemAt(pk, randomFreeTileInRoom(r), 1);
        }
        else if (roll < 147) dropItemAt(ItemKind::ScrollMapping, randomFreeTileInRoom(r), 1);
        else if (roll < 149) {
            int pick = rng.range(0, 3);
            ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                      : (pick == 1) ? ItemKind::ScrollDetectTraps
                                      : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                    : ItemKind::ScrollKnock;
            dropItemAt(sk, randomFreeTileInRoom(r), 1);
        }
        else if (roll < 151) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
        else if (roll < 153) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
        else if (roll < 156) dropItemAt(ItemKind::ScrollRemoveCurse, randomFreeTileInRoom(r), 1);
        else if (roll < 159) dropItemAt(ItemKind::ScrollConfusion, randomFreeTileInRoom(r), 1);
        else dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
    };

    int keysPlacedThisFloor = 0;
    int lockpicksPlacedThisFloor = 0;
    auto dropKeyAt = [&](Vec2i pos, int count = 1) {
        dropItemAt(ItemKind::Key, pos, count);
        keysPlacedThisFloor += std::max(1, count);
    };
    auto dropLockpickAt = [&](Vec2i pos, int count = 1) {
        dropItemAt(ItemKind::Lockpick, pos, count);
        lockpicksPlacedThisFloor += std::max(1, count);
    };

    auto rollChestTrap = [&]() -> TrapKind {
        // Weighted: mostly poison/alarm/web; teleport is rarer.
        int r = rng.range(0, 99);
        if (r < 30) return TrapKind::PoisonDart;
        if (r < 55) return TrapKind::Alarm;
        if (r < 76) return TrapKind::Web;
        if (r < 90) return TrapKind::ConfusionGas;
        return TrapKind::Teleport;
    };

    auto hasGroundAt = [&](Vec2i pos) -> bool {
        for (const auto& gi : ground) {
            if (gi.pos == pos) return true;
        }
        return false;
    };

    auto randomEmptyTileInRoom = [&](const Room& r) -> Vec2i {
        for (int tries = 0; tries < 200; ++tries) {
            Vec2i pos = randomFreeTileInRoom(r);
            if (!hasGroundAt(pos) && !entityAt(pos.x, pos.y)) return pos;
        }
        return randomFreeTileInRoom(r);
    };

    auto dropChestInRoom = [&](const Room& r, int tier, float lockedChance, float trappedChance) {
        Item chest;
        chest.id = nextItemId++;
        chest.kind = ItemKind::Chest;
        chest.count = 1;
        chest.spriteSeed = rng.nextU32();
        chest.enchant = clampi(tier, 0, 2);
        chest.charges = 0;

        if (rng.chance(lockedChance)) {
            setChestLocked(chest, true);
        }
        if (rng.chance(trappedChance)) {
            setChestTrapped(chest, true);
            setChestTrapKnown(chest, false);
            setChestTrapKind(chest, rollChestTrap());
        }

        // Mimic chance (NetHack flavor): some chests are actually monsters.
        // Starts appearing a bit deeper; higher-tier chests are more likely.
        if (depth_ >= 2) {
            float mimicChance = 0.04f + 0.01f * static_cast<float>(std::min(6, depth_ - 2));
            mimicChance += 0.03f * static_cast<float>(tier);
            mimicChance = std::min(0.20f, mimicChance);

            if (rng.chance(mimicChance)) {
                setChestMimic(chest, true);
                // Avoid "double gotcha" stacking with locks/traps.
                setChestLocked(chest, false);
                setChestTrapped(chest, false);
                setChestTrapKnown(chest, false);
                setChestTrapKind(chest, TrapKind::Spike);
            }
        }

        Vec2i pos = randomEmptyTileInRoom(r);
        ground.push_back({chest, pos});
    };

    bool hasLockedDoor = false;
    for (const auto& t : dung.tiles) {
        if (t.type == TileType::DoorLocked) {
            hasLockedDoor = true;
            break;
        }
    }

    for (const Room& r : rooms) {
        Vec2i p = randomFreeTileInRoom(r);

        if (r.type == RoomType::Vault) {
            // Vaults are locked bonus rooms: high reward, higher risk.
            dropItemAt(ItemKind::Gold, p, rng.range(25, 55) + depth_ * 4);
            dropChestInRoom(r, 2, 0.75f, 0.55f);
            if (depth_ >= 4 && rng.chance(0.25f)) {
                dropChestInRoom(r, 2, 0.85f, 0.65f);
            }
            dropGoodItem(r);
            if (rng.chance(0.65f)) dropGoodItem(r);
            if (rng.chance(0.35f)) dropItemAt(ItemKind::PotionHealing, randomFreeTileInRoom(r), 1);
            // No keys inside vaults; keys should be found outside.
            continue;
        }

        if (r.type == RoomType::Shop) {
            // Shops: a stocked room + a shopkeeper (spawned in spawnMonsters).
            // Items are tagged with shopPrice/shopDepth and must be paid for.

            // Pick a simple theme.
            const int themeRoll = rng.range(0, 99);
            // 0=General, 1=Armory, 2=Magic, 3=Supplies
            const int theme = (themeRoll < 30) ? 0 : (themeRoll < 55) ? 1 : (themeRoll < 80) ? 2 : 3;

            // Anchor item so every shop feels useful.
            if (theme == 2) {
                dropShopItemAt(ItemKind::ScrollIdentify, randomEmptyTileInRoom(r), 1);
            } else {
                dropShopItemAt(ItemKind::PotionHealing, randomEmptyTileInRoom(r), 1);
            }

            const int n = rng.range(7, 11);
            for (int i = 0; i < n; ++i) {
                ItemKind k = ItemKind::FoodRation;
                int count = 1;

                const int roll = rng.range(0, 99);
                if (theme == 0) {
                    // General store
                    if (roll < 14) { k = ItemKind::FoodRation; count = rng.range(1, 3); }
                    else if (roll < 26) { k = ItemKind::Torch; count = rng.range(1, 3); }
                    else if (roll < 40) { k = ItemKind::PotionHealing; count = rng.range(1, 2); }
                    else if (roll < 48) { k = ItemKind::PotionAntidote; }
                    else if (roll < 58) { k = ItemKind::ScrollIdentify; }
                    else if (roll < 64) { k = ItemKind::ScrollDetectTraps; }
                    else if (roll < 70) { k = ItemKind::ScrollDetectSecrets; }
                    else if (roll < 75) { k = ItemKind::ScrollKnock; }
                    else if (roll < 80) { k = ItemKind::Lockpick; }
                    else if (roll < 84) { k = ItemKind::Key; }
                    else if (roll < 92) { k = ItemKind::Arrow; count = rng.range(8, 18); }
                    else if (roll < 96) { k = ItemKind::Dagger; }
                    else { k = (rng.chance(0.50f) ? ItemKind::LeatherArmor : ItemKind::Bow); }
                } else if (theme == 1) {
                    // Armory
                    if (roll < 15) { k = ItemKind::Dagger; }
                    else if (roll < 34) { k = ItemKind::Sword; }
                    else if (roll < 44) { k = ItemKind::Axe; }
                    else if (roll < 52) { k = ItemKind::Pickaxe; }
                    else if (roll < 61) { k = ItemKind::Bow; }
                    else if (roll < 70) { k = ItemKind::Sling; }
                    else if (roll < 84) { k = ItemKind::Arrow; count = rng.range(10, 24); }
                    else if (roll < 92) { k = ItemKind::LeatherArmor; }
                    else if (roll < 98) { k = ItemKind::ChainArmor; }
                    else { k = (depth_ >= 6 ? ItemKind::PlateArmor : ItemKind::ChainArmor); }
                } else if (theme == 2) {
                    // Magic shop
                    if (roll < 14) { k = ItemKind::WandSparks; }
                    else if (roll < 22) { k = ItemKind::WandDigging; }
                    else if (roll < 26) { k = (depth_ >= 6 ? ItemKind::WandFireball : ItemKind::WandDigging); }
                    else if (roll < 33) { k = ItemKind::ScrollTeleport; }
                    else if (roll < 45) { k = ItemKind::ScrollMapping; }
                    else if (roll < 60) { k = ItemKind::ScrollIdentify; }
                    else if (roll < 68) { k = ItemKind::ScrollRemoveCurse; }
                    else if (roll < 76) { k = ItemKind::PotionStrength; }
                    else if (roll < 84) { k = ItemKind::PotionRegeneration; }
                    else if (roll < 91) { k = ItemKind::PotionHaste; }
                    else { k = (depth_ >= 5 ? ItemKind::PotionInvisibility : ItemKind::PotionVision); }
                } else {
                    // Supplies
                    if (roll < 40) { k = ItemKind::FoodRation; count = rng.range(1, 4); }
                    else if (roll < 60) { k = ItemKind::PotionHealing; count = rng.range(1, 2); }
                    else if (roll < 78) { k = ItemKind::Torch; count = rng.range(1, 4); }
                    else if (roll < 90) { k = ItemKind::PotionAntidote; count = rng.range(1, 2); }
                    else if (roll < 96) { k = ItemKind::ScrollDetectTraps; }
                    else { k = (rng.chance(0.55f) ? ItemKind::Lockpick : ItemKind::Key); }
                }

                // Depth-based small upgrades.
                if (k == ItemKind::LeatherArmor && depth_ >= 4 && rng.chance(0.12f)) k = ItemKind::ChainArmor;
                if (k == ItemKind::ChainArmor && depth_ >= 7 && rng.chance(0.06f)) k = ItemKind::PlateArmor;

                dropShopItemAt(k, randomEmptyTileInRoom(r), count);
            }
            continue;
        }

        if (r.type == RoomType::Secret) {
            // Secret rooms are optional bonus finds; keep them rewarding but not as
            // rich as full treasure rooms.
            dropItemAt(ItemKind::Gold, p, rng.range(8, 22) + depth_);
            if (rng.chance(0.55f)) {
                dropChestInRoom(r, 1, 0.45f, 0.35f);
            }
            if (rng.chance(0.70f)) {
                dropGoodItem(r);
            } else if (rng.chance(0.50f)) {
                dropItemAt(ItemKind::PotionHealing, randomFreeTileInRoom(r), 1);
            }
            continue;
        }

        if (r.type == RoomType::Treasure) {
            dropItemAt(ItemKind::Gold, p, rng.range(15, 40) + depth_ * 3);
            dropGoodItem(r);
            if (rng.chance(0.40f)) {
                dropChestInRoom(r, 1, 0.50f, 0.25f);
            }
            if (rng.chance(0.35f)) dropKeyAt(randomFreeTileInRoom(r), 1);
            if (rng.chance(0.25f)) dropLockpickAt(randomFreeTileInRoom(r), rng.range(1, 2));
            continue;
        }

        if (r.type == RoomType::Shrine) {
            dropItemAt(ItemKind::PotionHealing, p, rng.range(1, 2));
            if (rng.chance(0.25f)) dropKeyAt(randomFreeTileInRoom(r), 1);
            if (rng.chance(0.20f)) dropLockpickAt(randomFreeTileInRoom(r), 1);
            if (rng.chance(hungerEnabled_ ? 0.75f : 0.35f)) dropItemAt(ItemKind::FoodRation, randomFreeTileInRoom(r), rng.range(1, 2));
            if (rng.chance(0.45f)) dropItemAt(ItemKind::PotionStrength, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.35f)) dropItemAt(ItemKind::PotionAntidote, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.30f)) dropItemAt(ItemKind::PotionRegeneration, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.22f)) dropItemAt(ItemKind::PotionShielding, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.15f)) dropItemAt(ItemKind::PotionHaste, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.15f)) {
            const ItemKind pk = rng.chance(0.20f) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
            dropItemAt(pk, randomFreeTileInRoom(r), 1);
        }
            if (rng.chance(0.18f)) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.12f)) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.08f)) dropItemAt(ItemKind::ScrollRemoveCurse, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.20f)) {
                int pick = rng.range(0, 4);
                ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                          : (pick == 1) ? ItemKind::ScrollDetectTraps
                                          : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                          : (pick == 3) ? ItemKind::ScrollKnock
                                                        : ItemKind::ScrollRemoveCurse;
                dropItemAt(sk, randomFreeTileInRoom(r), 1);
            }
            if (rng.chance(0.45f)) dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.35f)) dropItemAt(ItemKind::ScrollMapping, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.50f)) dropItemAt(ItemKind::Gold, randomFreeTileInRoom(r), rng.range(6, 18));
            continue;
        }

        if (r.type == RoomType::Lair) {
            if (rng.chance(0.50f)) dropItemAt(ItemKind::Rock, p, rng.range(3, 9));
            if (rng.chance(0.10f)) dropKeyAt(randomFreeTileInRoom(r), 1);
            if (rng.chance(0.12f)) dropLockpickAt(randomFreeTileInRoom(r), 1);
            if (rng.chance(hungerEnabled_ ? 0.25f : 0.10f)) dropItemAt(ItemKind::FoodRation, randomFreeTileInRoom(r), 1);
            if (depth_ >= 2 && rng.chance(0.20f)) dropItemAt(ItemKind::Sling, randomFreeTileInRoom(r), 1);
            continue;
        }

        // Normal rooms: small chance for loot
        if (rng.chance(0.06f)) {
            dropKeyAt(p, 1);
        }
        if (rng.chance(0.05f)) {
            dropLockpickAt(p, 1);
        }

        if (rng.chance(0.35f)) {
            // Expanded table (added food rations).
            int roll = rng.range(0, 115);
            if (roll < 21) dropItemAt(ItemKind::Gold, p, rng.range(10, 55));
            else if (roll < 29) dropItemAt(ItemKind::FoodRation, p, 1);
            else if (roll < 37) dropItemAt(ItemKind::Torch, p, 1 + ((rng.range(1, 6) == 1) ? 1 : 0));
            else if (roll < 51) dropItemAt(ItemKind::PotionHealing, p, 1);
            else if (roll < 61) dropItemAt(ItemKind::PotionStrength, p, 1);
            else if (roll < 69) dropItemAt(ItemKind::PotionAntidote, p, 1);
            else if (roll < 75) dropItemAt(ItemKind::PotionRegeneration, p, 1);
            else if (roll < 81) dropItemAt(ItemKind::ScrollTeleport, p, 1);
            else if (roll < 87) dropItemAt(ItemKind::ScrollMapping, p, 1);
            else if (roll < 89) {
                // Small chance of a utility scroll.
                const ItemKind pool[] = {
                    ItemKind::ScrollEnchantWeapon,
                    ItemKind::ScrollEnchantArmor,
                    ItemKind::ScrollTeleport,
                    ItemKind::ScrollMapping,
                };
                const ItemKind sk = pool[rng.range(0, static_cast<int>(sizeof(pool) / sizeof(pool[0])) - 1)];
                dropItemAt(sk, p, 1);
            } else if (roll < 93) dropItemAt(ItemKind::ScrollEnchantWeapon, p, 1);
            else if (roll < 96) dropItemAt(ItemKind::ScrollEnchantArmor, p, 1);
            else if (roll < 98) dropItemAt(ItemKind::ScrollRemoveCurse, p, 1);
            else if (roll < 103) dropItemAt(ItemKind::Arrow, p, rng.range(4, 10));
            else if (roll < 108) dropItemAt(ItemKind::Rock, p, rng.range(3, 8));
            else if (roll < 111) dropItemAt(ItemKind::Dagger, p, 1);
            else if (roll < 113) dropItemAt(ItemKind::LeatherArmor, p, 1);
            else if (roll < 114) dropItemAt(ItemKind::PotionShielding, p, 1);
            else if (roll < 115) dropItemAt(ItemKind::PotionHaste, p, 1);
            else {
                // Very rare: perception/stealth potions.
                const ItemKind pk = rng.chance(0.25f) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
                dropItemAt(pk, p, 1);
            }
        }
            }

    // Guarantee at least one key on any floor that contains locked doors.
    if (hasLockedDoor && keysPlacedThisFloor <= 0) {
        std::vector<const Room*> candidates;
        candidates.reserve(rooms.size());
        for (const Room& r : rooms) {
            if (r.type == RoomType::Vault) continue; // don't hide keys behind locked doors
            if (r.type == RoomType::Secret) continue;
            if (r.type == RoomType::Treasure) continue; // keep the guarantee discoverable without searching
            candidates.push_back(&r);
        }

        if (!candidates.empty()) {
            for (int tries = 0; tries < 50; ++tries) {
                const Room& rr = *candidates[static_cast<size_t>(rng.range(0, static_cast<int>(candidates.size()) - 1))];
                Vec2i pos = randomFreeTileInRoom(rr);
                if (entityAt(pos.x, pos.y)) continue;
                dropKeyAt(pos, 1);
                break;
            }
        }
    }
    // Guarantee at least one lockpick on any floor that contains locked doors.
    // (Lockpicks are a fallback if you can't find enough keys.)
    if (hasLockedDoor && lockpicksPlacedThisFloor <= 0) {
        std::vector<const Room*> candidates;
        candidates.reserve(rooms.size());
        for (const Room& r : rooms) {
            if (r.type == RoomType::Vault) continue;   // don't hide picks behind locked doors
            if (r.type == RoomType::Secret) continue;
            if (r.type == RoomType::Treasure) continue;  // keep the guarantee discoverable without searching
            candidates.push_back(&r);
        }

        if (!candidates.empty()) {
            for (int tries = 0; tries < 50; ++tries) {
                const Room& rr = *candidates[static_cast<size_t>(rng.range(0, static_cast<int>(candidates.size()) - 1))];
                Vec2i pos = randomFreeTileInRoom(rr);
                if (entityAt(pos.x, pos.y)) continue;
                dropLockpickAt(pos, 1);
                break;
            }
        }
    }


    // Quest objective: place the Amulet of Yendor on the final depth.
    if (depth_ == QUEST_DEPTH && !playerHasAmulet()) {
        bool alreadyHere = false;
        for (const auto& gi : ground) {
            if (gi.item.kind == ItemKind::AmuletYendor) {
                alreadyHere = true;
                break;
            }
        }
        if (!alreadyHere) {
            const Room* tr = nullptr;
            for (const Room& r : rooms) {
                if (r.type == RoomType::Treasure) { tr = &r; break; }
            }
            Vec2i pos = tr ? randomFreeTileInRoom(*tr) : (dung.inBounds(dung.stairsDown.x, dung.stairsDown.y) ? dung.stairsDown : dung.stairsUp);
            dropItemAt(ItemKind::AmuletYendor, pos, 1);
        }
    }

    // A little extra ammo somewhere on the map.
    if (rng.chance(0.75f)) {
        Vec2i pos = dung.randomFloor(rng, true);
        if (!entityAt(pos.x, pos.y)) {
            if (rng.chance(0.55f)) dropItemAt(ItemKind::Arrow, pos, rng.range(6, 14));
            else dropItemAt(ItemKind::Rock, pos, rng.range(4, 12));
        }
    }
}

void Game::spawnTraps() {
    trapsCur.clear();

    // A small number of traps per floor, scaling gently with depth.
    const int base = 2;
    const int depthBonus = std::min(6, depth_ / 2);
    int targetCount = base + depthBonus + rng.range(0, 2);

    // Penultimate floor (the labyrinth) is intentionally trap-heavy.
    if (depth_ == QUEST_DEPTH - 1) {
        targetCount += 4;
    }

    auto isBadPos = [&](Vec2i p) {
        if (!dung.inBounds(p.x, p.y)) return true;
        if (!dung.isWalkable(p.x, p.y)) return true;
        if (p == dung.stairsUp || p == dung.stairsDown) return true;
        // Avoid the immediate start area.
        if (manhattan(p, player().pos) <= 4) return true;
        return false;
    };

    auto alreadyHasTrap = [&](Vec2i p) {
        for (const auto& t : trapsCur) {
            if (t.pos == p) return true;
        }
        return false;
    };

    int attempts = 0;
    while (static_cast<int>(trapsCur.size()) < targetCount && attempts < targetCount * 60) {
        ++attempts;
        Vec2i p = dung.randomFloor(rng, true);
        if (isBadPos(p)) continue;
        if (alreadyHasTrap(p)) continue;

        // Choose trap type (deeper floors skew deadlier).
        int roll = rng.range(0, 99);
        TrapKind tk = TrapKind::Spike;
        if (depth_ == QUEST_DEPTH - 1) {
            // Labyrinth: more "tactical" traps than raw damage.
            if (roll < 25) tk = TrapKind::Spike;
            else if (roll < 50) tk = TrapKind::PoisonDart;
            else if (roll < 72) tk = TrapKind::Alarm;
            else if (roll < 88) tk = TrapKind::Web;
            else if (roll < 96) tk = TrapKind::ConfusionGas;
            else tk = TrapKind::Teleport;
        } else if (depth_ <= 1) {
            tk = (roll < 70) ? TrapKind::Spike : TrapKind::PoisonDart;
        } else if (depth_ <= 3) {
            if (roll < 45) tk = TrapKind::Spike;
            else if (roll < 75) tk = TrapKind::PoisonDart;
            else if (roll < 87) tk = TrapKind::Alarm;
            else if (roll < 93) tk = TrapKind::Web;
            else if (roll < 97) tk = TrapKind::ConfusionGas;
            else tk = TrapKind::Teleport;
        } else {
            if (roll < 35) tk = TrapKind::Spike;
            else if (roll < 64) tk = TrapKind::PoisonDart;
            else if (roll < 79) tk = TrapKind::Alarm;
            else if (roll < 89) tk = TrapKind::Web;
            else if (roll < 96) tk = TrapKind::ConfusionGas;
            else tk = TrapKind::Teleport;
        }

        Trap t;
        t.kind = tk;
        t.pos = p;
        t.discovered = false;
        trapsCur.push_back(t);
    }

    // Vault security: some locked doors are trapped.
    // Traps are attached to the door tile and will trigger when you step through.
    const float doorTrapBase = 0.18f;
    const float doorTrapDepth = 0.02f * static_cast<float>(std::min(8, depth_));
    const float doorTrapChance = std::min(0.40f, doorTrapBase + doorTrapDepth);

    for (int y = 0; y < dung.height; ++y) {
        for (int x = 0; x < dung.width; ++x) {
            if (dung.at(x, y).type != TileType::DoorLocked) continue;
            Vec2i p{ x, y };
            if (alreadyHasTrap(p)) continue;
            // Avoid trapping doors right next to the start.
            if (manhattan(p, player().pos) <= 6) continue;

            if (!rng.chance(doorTrapChance)) continue;

            Trap t;
            t.pos = p;
            t.discovered = false;
            // Bias toward alarm/poison on doors (fits the theme), with occasional gas traps.
            if (rng.chance(0.12f)) t.kind = TrapKind::ConfusionGas;
            else t.kind = rng.chance(0.55f) ? TrapKind::Alarm : TrapKind::PoisonDart;
            trapsCur.push_back(t);
        }
    }

}

void Game::applyEndOfTurnEffects() {
    if (gameOver) return;

    Entity& p = playerMut();


    // ------------------------------------------------------------
    // Environmental fields: Confusion Gas (persistent, tile-based)
    //
    // The gas itself is stored as an intensity map (0..255). Entities standing
    // in gas have their confusion duration "topped up" each turn.
    // ------------------------------------------------------------
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (confusionGas_.size() != expect) confusionGas_.assign(expect, 0u);

        auto gasIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto gasAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return 0u;
            const size_t i = gasIdx(x, y);
            if (i >= confusionGas_.size()) return 0u;
            return confusionGas_[i];
        };

        auto applyGasTo = [&](Entity& e, bool isPlayer) {
            const uint8_t g = gasAt(e.pos.x, e.pos.y);
            if (g == 0u) return;

            // Scale confusion severity with gas intensity.
            int minTurns = 2 + static_cast<int>(g) / 2;
            minTurns = clampi(minTurns, 2, 10);

            const int before = e.effects.confusionTurns;
            if (before < minTurns) e.effects.confusionTurns = minTurns;

            // Message only on first exposure (avoids log spam while standing in gas).
            if (before == 0 && e.effects.confusionTurns > 0) {
                if (isPlayer) {
                    pushMsg("YOU INHALE NOXIOUS GAS!", MessageKind::Warning, true);
                } else if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(e.kind) << " INHALES NOXIOUS GAS!";
                    pushMsg(ss.str(), MessageKind::Info, false);
                }
            }
        };

        applyGasTo(p, true);
        for (auto& m : ents) {
            if (m.id == playerId_) continue;
            if (m.hp <= 0) continue;
            applyGasTo(m, false);
        }
    }
    // Timed poison: hurts once per full turn.
    if (p.effects.poisonTurns > 0) {
        p.effects.poisonTurns = std::max(0, p.effects.poisonTurns - 1);
        p.hp -= 1;
        if (p.hp <= 0) {
            pushMsg("YOU SUCCUMB TO POISON.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = "DIED OF POISON";
            gameOver = true;
            return;
        }

        if (p.effects.poisonTurns == 0) {
            pushMsg("THE POISON WEARS OFF.", MessageKind::System, false);
        }
    }

    // Timed regeneration: gentle healing over time.
    if (p.effects.regenTurns > 0) {
        p.effects.regenTurns = std::max(0, p.effects.regenTurns - 1);
        if (p.hp < p.hpMax) {
            p.hp += 1;
        }
        if (p.effects.regenTurns == 0) {
            pushMsg("REGENERATION FADES.", MessageKind::System, true);
        }
    }

    // Timed shielding: no per-tick effect besides duration.
    if (p.effects.shieldTurns > 0) {
        p.effects.shieldTurns = std::max(0, p.effects.shieldTurns - 1);
        if (p.effects.shieldTurns == 0) {
            pushMsg("YOUR SHIELDING FADES.", MessageKind::System, true);
        }
    }

    // Timed vision boost
    if (p.effects.visionTurns > 0) {
        p.effects.visionTurns = std::max(0, p.effects.visionTurns - 1);
        if (p.effects.visionTurns == 0) {
            pushMsg("YOUR VISION RETURNS TO NORMAL.", MessageKind::System, true);
        }
    }

    // Timed invisibility: affects monster perception.
    if (p.effects.invisTurns > 0) {
        p.effects.invisTurns = std::max(0, p.effects.invisTurns - 1);
        if (p.effects.invisTurns == 0) {
            pushMsg("YOU FADE INTO VIEW.", MessageKind::System, true);
        }
    }

    // Timed webbing: prevents movement.
    if (p.effects.webTurns > 0) {
        p.effects.webTurns = std::max(0, p.effects.webTurns - 1);
        if (p.effects.webTurns == 0) {
            pushMsg("YOU BREAK FREE OF THE WEB.", MessageKind::System, true);
        }
    }

    // Timed confusion: scramble player (and monster) intent.
    if (p.effects.confusionTurns > 0) {
        p.effects.confusionTurns = std::max(0, p.effects.confusionTurns - 1);
        if (p.effects.confusionTurns == 0) {
            pushMsg(effectEndMessage(EffectKind::Confusion), MessageKind::System, true);
        }
    }

    // Natural regeneration (slow baseline healing).
    // Intentionally disabled while poisoned to keep poison meaningful.
    if (p.effects.poisonTurns > 0 || p.hp >= p.hpMax) {
        naturalRegenCounter = 0;
    } else if (p.effects.regenTurns <= 0) {
        // Faster natural regen as you level.
        const int vigorBonus = std::min(4, talentVigor_);
        const int interval = std::max(6, 14 - charLevel - vigorBonus); // L1:13, L5:9, L10+:6 (vigor speeds this up)
        naturalRegenCounter++;
        if (naturalRegenCounter >= interval) {
            p.hp = std::min(p.hpMax, p.hp + 1);
            naturalRegenCounter = 0;
        }
    }
    // Hunger ticking (optional).
    if (hungerEnabled_) {
        if (hungerMax <= 0) hungerMax = 800;

        hunger = std::max(0, hunger - 1);

        const int st = hungerStateFor(hunger, hungerMax);
        if (st != hungerStatePrev) {
            if (st == 1) {
                pushMsg("YOU FEEL HUNGRY.", MessageKind::System, true);
            } else if (st == 2) {
                pushMsg("YOU ARE STARVING!", MessageKind::Warning, true);
            } else if (st == 3) {
                pushMsg("YOU ARE STARVING TO DEATH!", MessageKind::Warning, true);
            }
            hungerStatePrev = st;
        }

        // Starvation damage (every other turn so it isn't instant death).
        if (st == 3 && (turnCount % 2u) == 0u) {
            p.hp -= 1;
            if (p.hp <= 0) {
                pushMsg("YOU STARVE.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = "STARVED TO DEATH";
                gameOver = true;
                return;
            }
        }
    }


    // Torches burn down (carried and dropped).
    {
        int burntInv = 0;
        for (size_t i = 0; i < inv.size(); ) {
            Item& it = inv[i];
            if (it.kind == ItemKind::TorchLit) {
                if (it.charges > 0) it.charges -= 1;
                if (it.charges <= 0) {
                    ++burntInv;
                    inv.erase(inv.begin() + static_cast<std::vector<Item>::difference_type>(i));
                    continue;
                }
            }
            ++i;
        }
        if (burntInv > 0) {
            pushMsg(burntInv == 1 ? "YOUR TORCH BURNS OUT." : "YOUR TORCHES BURN OUT.", MessageKind::System, true);
        }

        int burntGroundVis = 0;
        for (size_t i = 0; i < ground.size(); ) {
            auto& gi = ground[i];
            if (gi.item.kind == ItemKind::TorchLit) {
                if (gi.item.charges > 0) gi.item.charges -= 1;
                if (gi.item.charges <= 0) {
                    if (dung.inBounds(gi.pos.x, gi.pos.y) && dung.at(gi.pos.x, gi.pos.y).visible) {
                        ++burntGroundVis;
                    }
                    ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));
                    continue;
                }
            }
            ++i;
        }
        if (burntGroundVis > 0) {
            pushMsg(burntGroundVis == 1 ? "A TORCH FLICKERS OUT." : "SOME TORCHES FLICKER OUT.", MessageKind::System, true);
        }
    }


    // Corpses rot away (carried and dropped).
    // We reuse the Item::charges field as a simple "freshness" timer in turns.
    {
        int rottedInv = 0;
        for (size_t i = 0; i < inv.size();) {
            Item& it = inv[i];
            if (isCorpseKind(it.kind)) {
                if (it.charges > 0) it.charges -= 1;
                if (it.charges <= 0) {
                    ++rottedInv;
                    inv.erase(inv.begin() + static_cast<std::vector<Item>::difference_type>(i));
                    continue;
                }
            }
            ++i;
        }
        if (rottedInv > 0) {
            pushMsg(rottedInv == 1 ? "A CORPSE ROTS AWAY IN YOUR PACK." : "CORPSES ROT AWAY IN YOUR PACK.", MessageKind::System, true);
        }

        int rottedGroundVis = 0;
        for (size_t i = 0; i < ground.size();) {
            auto& gi = ground[i];
            if (isCorpseKind(gi.item.kind)) {
                if (gi.item.charges > 0) gi.item.charges -= 1;
                if (gi.item.charges <= 0) {
                    if (dung.inBounds(gi.pos.x, gi.pos.y) && dung.at(gi.pos.x, gi.pos.y).visible) {
                        ++rottedGroundVis;
                    }
                    ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));
                    continue;
                }
            }
            ++i;
        }
        if (rottedGroundVis > 0) {
            pushMsg(rottedGroundVis == 1 ? "A CORPSE ROTS AWAY." : "SOME CORPSES ROT AWAY.", MessageKind::System, true);
        }
    }


    // Timed effects for monsters (poison, web). These tick with time just like the player.
    for (auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;

        // Timed poison: lose 1 HP per full turn.
        if (m.effects.poisonTurns > 0) {
            m.effects.poisonTurns = std::max(0, m.effects.poisonTurns - 1);
            m.hp -= 1;

            if (m.hp <= 0) {
                // Only message if the monster is currently visible to the player.
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " SUCCUMBS TO POISON.";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                }
            } else if (m.effects.poisonTurns == 0) {
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " RECOVERS FROM POISON.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            }
        }

        // Timed webbing: prevents movement while >0, then wears off.
        if (m.effects.webTurns > 0) {
            m.effects.webTurns = std::max(0, m.effects.webTurns - 1);
            if (m.effects.webTurns == 0) {
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " BREAKS FREE OF THE WEB.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            }
        }

        // Timed confusion: wears off with time (just like the player).
        if (m.effects.confusionTurns > 0) {
            m.effects.confusionTurns = std::max(0, m.effects.confusionTurns - 1);
            if (m.effects.confusionTurns == 0) {
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " SEEMS LESS CONFUSED.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            }
        }
    }

    // Update confusion gas cloud diffusion/decay.
    // This is a cheap per-turn diffusion on the small map grid.
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (expect > 0 && confusionGas_.size() != expect) {
            confusionGas_.assign(expect, 0u);
        }

        if (!confusionGas_.empty()) {
            const int w = dung.width;
            const int h = dung.height;
            const size_t n = static_cast<size_t>(w * h);

            std::vector<uint8_t> next(n, 0u);
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
            auto passable = [&](int x, int y) -> bool {
                if (!dung.inBounds(x, y)) return false;
                // Keep gas on walkable tiles (floors, open doors, stairs).
                return dung.isWalkable(x, y);
            };

            constexpr Vec2i kDirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    const uint8_t s = confusionGas_[i];
                    if (s == 0u) continue;
                    if (!passable(x, y)) continue;

                    // Always decay in place.
                    const uint8_t self = (s > 0u) ? static_cast<uint8_t>(s - 1u) : 0u;
                    if (next[i] < self) next[i] = self;

                    // Spread to neighbors with extra decay.
                    if (s >= 3u) {
                        const uint8_t spread = static_cast<uint8_t>(s - 2u);
                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;
                            const size_t j = idx2(nx, ny);
                            if (next[j] < spread) next[j] = spread;
                        }
                    }
                }
            }

            confusionGas_.swap(next);
        }
    }

}

void Game::cleanupDead() {
    // If a shopkeeper dies, the shop is effectively abandoned.
    // Make all shop stock (and any unpaid goods) on this depth free.
    bool shopkeeperDied = false;
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp > 0) continue;
        if (e.kind == EntityKind::Shopkeeper) {
            shopkeeperDied = true;
            break;
        }
    }
    if (shopkeeperDied) {
        for (auto& gi : ground) {
            if (gi.item.shopDepth == depth_ && gi.item.shopPrice > 0) {
                gi.item.shopPrice = 0;
                gi.item.shopDepth = 0;
            }
        }
        for (auto& it : inv) {
            if (it.shopDepth == depth_ && it.shopPrice > 0) {
                it.shopPrice = 0;
                it.shopDepth = 0;
            }
        }
        pushMsg("THE SHOPKEEPER IS DEAD. EVERYTHING IS FREE!", MessageKind::Success, true);
    }

    // Drop loot from dead monsters (before removal)
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp > 0) continue;

        // Corpse drops (organic remains).
        // These are heavy, rot away over time, and can be eaten.
        {
            ItemKind corpseKind = ItemKind::Dagger;
            float chance = 0.0f;
            bool ok = true;

            switch (e.kind) {
                case EntityKind::Goblin:        corpseKind = ItemKind::CorpseGoblin;   chance = 0.75f; break;
                case EntityKind::Orc:           corpseKind = ItemKind::CorpseOrc;      chance = 0.75f; break;
                case EntityKind::Bat:           corpseKind = ItemKind::CorpseBat;      chance = 0.65f; break;
                case EntityKind::Slime:         corpseKind = ItemKind::CorpseSlime;    chance = 0.50f; break;
                case EntityKind::KoboldSlinger: corpseKind = ItemKind::CorpseKobold;   chance = 0.70f; break;
                case EntityKind::Wolf:          corpseKind = ItemKind::CorpseWolf;     chance = 0.75f; break;
                case EntityKind::Troll:         corpseKind = ItemKind::CorpseTroll;    chance = 0.85f; break;
                case EntityKind::Wizard:        corpseKind = ItemKind::CorpseWizard;   chance = 0.70f; break;
                case EntityKind::Snake:         corpseKind = ItemKind::CorpseSnake;    chance = 0.70f; break;
                case EntityKind::Spider:        corpseKind = ItemKind::CorpseSpider;   chance = 0.70f; break;
                case EntityKind::Ogre:          corpseKind = ItemKind::CorpseOgre;     chance = 0.85f; break;
                case EntityKind::Mimic:         corpseKind = ItemKind::CorpseMimic;    chance = 0.60f; break;
                case EntityKind::Minotaur:      corpseKind = ItemKind::CorpseMinotaur; chance = 0.90f; break;
                default:
                    ok = false;
                    break;
            }

            if (ok && chance > 0.0f && rng.chance(chance)) {
                GroundItem ci;
                ci.pos = e.pos;
                ci.item.id = nextItemId++;
                ci.item.spriteSeed = rng.nextU32();
                ci.item.kind = corpseKind;
                ci.item.count = 1;

                // Freshness timer scales with "mass" so bigger corpses last longer.
                const int w = std::max(1, itemDef(corpseKind).weight);
                const int base = 180 + w * 6;
                const int var = rng.range(-20, 25);
                ci.item.charges = std::max(120, std::min(380, base + var));

                ground.push_back(ci);
            }
        }

        // Drop equipped monster gear (weapon/armor) before the generic loot roll.
        // (Monsters can also pick up better gear during play.)
        if (e.gearMelee.id != 0 && isWeapon(e.gearMelee.kind)) {
            Item it = e.gearMelee;
            it.count = 1;
            it.shopPrice = 0;
            it.shopDepth = 0;
            dropGroundItemItem(e.pos, it);
        }
        if (e.gearArmor.id != 0 && isArmor(e.gearArmor.kind)) {
            Item it = e.gearArmor;
            it.count = 1;
            it.shopPrice = 0;
            it.shopDepth = 0;
            dropGroundItemItem(e.pos, it);
        }

        // Ammo drop: ammo-based ranged monsters can have leftover ammo; drop it on death.
        if (e.rangedAmmo != AmmoKind::None && e.rangedAmmoCount > 0) {
            const ItemKind ammoK = (e.rangedAmmo == AmmoKind::Arrow) ? ItemKind::Arrow : ItemKind::Rock;

            // Lose a few to breakage or being scattered during the fight.
            int n = e.rangedAmmoCount;
            if (n > 1) {
                n -= rng.range(0, std::max(0, n / 5));
            }
            if (n > 0) {
                dropGroundItem(e.pos, ammoK, n);
            }
        }

        // Simple drops
        if (rng.chance(0.55f)) {
            GroundItem gi;
            gi.pos = e.pos;
            gi.item.id = nextItemId++;
            gi.item.spriteSeed = rng.nextU32();

            int roll = rng.range(0, 119);
            if (roll < 39) { gi.item.kind = ItemKind::Gold; gi.item.count = rng.range(2, 8); }
            else if (roll < 54) { gi.item.kind = ItemKind::Arrow; gi.item.count = rng.range(3, 7); }
            else if (roll < 64) { gi.item.kind = ItemKind::Rock; gi.item.count = rng.range(2, 6); }
            else if (roll < 72) { gi.item.kind = ItemKind::Torch; gi.item.count = 1; }
            else if (roll < 80) { gi.item.kind = ItemKind::FoodRation; gi.item.count = rng.range(1, 2); }
            else if (roll < 89) { gi.item.kind = ItemKind::PotionHealing; gi.item.count = 1; }
            else if (roll < 95) { gi.item.kind = ItemKind::PotionAntidote; gi.item.count = 1; }
            else if (roll < 99) { gi.item.kind = ItemKind::PotionRegeneration; gi.item.count = 1; }
            else if (roll < 103) { gi.item.kind = ItemKind::ScrollTeleport; gi.item.count = 1; }
            else if (roll < 105) {
                int pick = rng.range(0, 3);
                gi.item.kind = (pick == 0) ? ItemKind::ScrollIdentify
                                           : (pick == 1) ? ItemKind::ScrollDetectTraps
                                           : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                         : ItemKind::ScrollKnock;
                gi.item.count = 1;
            }
            else if (roll < 108) { gi.item.kind = ItemKind::ScrollEnchantWeapon; gi.item.count = 1; }
            else if (roll < 111) { gi.item.kind = ItemKind::ScrollEnchantArmor; gi.item.count = 1; }
            else if (roll < 113) { gi.item.kind = ItemKind::ScrollRemoveCurse; gi.item.count = 1; }
            else if (roll < 114) { gi.item.kind = ItemKind::Dagger; gi.item.count = 1; }
            else if (roll < 115) { gi.item.kind = ItemKind::PotionShielding; gi.item.count = 1; }
            else if (roll < 116) { gi.item.kind = ItemKind::PotionHaste; gi.item.count = 1; }
            else {
                gi.item.kind = (rng.range(1, 4) == 1) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
                gi.item.count = 1;
            }

            // Roll BUC (blessed/uncursed/cursed) for dropped gear.
            if (isWeapon(gi.item.kind) || isArmor(gi.item.kind)) {
                gi.item.buc = rollBucForGear(rng, depth_, roomTypeAt(dung, gi.pos));
            }

            // Chance for dropped gear to be lightly enchanted on deeper floors.
            if ((isWeapon(gi.item.kind) || isArmor(gi.item.kind)) && depth_ >= 3) {
                if (rng.chance(0.25f)) {
                    gi.item.enchant = 1;
                    if (depth_ >= 6 && rng.chance(0.10f)) {
                        gi.item.enchant = 2;
                    }
                }
            }

            ground.push_back(gi);

            // Rare extra drop: keys (humanoid-ish enemies are more likely to carry them).
            const bool keyCarrier = (e.kind == EntityKind::Goblin || e.kind == EntityKind::Orc || e.kind == EntityKind::KoboldSlinger ||
                                     e.kind == EntityKind::SkeletonArcher || e.kind == EntityKind::Wizard || e.kind == EntityKind::Ogre ||
                                     e.kind == EntityKind::Troll);
            if (keyCarrier && rng.chance(0.07f)) {
                GroundItem kg;
                kg.pos = e.pos;
                kg.item.id = nextItemId++;
                kg.item.spriteSeed = rng.nextU32();
                kg.item.kind = ItemKind::Key;
                kg.item.count = 1;
                ground.push_back(kg);
            }
        }
    }

    // Remove dead monsters
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return (e.id != playerId_) && (e.hp <= 0);
    }), ents.end());

    // Player death handled in attack functions
}
