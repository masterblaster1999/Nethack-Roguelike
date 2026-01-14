#include "game_internal.hpp"
#include "content.hpp"

namespace {

bool canHaveWeaponEgo(ItemKind k) {
    // Keep egos limited to the "core" melee weapons for now.
    // (Avoids branding tools like pickaxes, and keeps UI readable.)
    return k == ItemKind::Dagger || k == ItemKind::Sword || k == ItemKind::Axe;
}

ItemEgo rollWeaponEgo(RNG& rng, ItemKind k, int depth, RoomType rt, bool fromShop, bool forMonster) {
    if (!canHaveWeaponEgo(k)) return ItemEgo::None;
    if (depth < 3) return ItemEgo::None;

    // Base chance grows gently with depth.
    float chance = 0.04f + 0.01f * static_cast<float>(std::min(10, std::max(0, depth - 3)));

    // Treasure-y rooms are more likely to contain branded gear.
    if (rt == RoomType::Treasure || rt == RoomType::Vault || rt == RoomType::Secret) chance += 0.06f;
    if (rt == RoomType::Lair) chance -= 0.03f;

    // Shops occasionally stock a premium item.
    if (fromShop) chance += 0.05f;

    // Monsters shouldn't carry too many premium weapons.
    if (forMonster) chance *= 0.60f;

    chance = std::max(0.0f, std::min(0.22f, chance));
    if (!rng.chance(chance)) return ItemEgo::None;

    // Vampiric is deeper + rarer.
    const int roll = rng.range(0, 99);
    if (depth >= 6 && roll >= 92) return ItemEgo::Vampiric;
    if (roll < 48) return ItemEgo::Flaming;
    return ItemEgo::Venom;
}

ItemKind pickSpellbookKind(RNG& rng, int depth) {
    // Depth-based distribution for spellbooks (WIP).
    // New books unlock as depth increases; early floors mostly contain the basics.
    depth = std::max(1, depth);

    struct Entry { ItemKind kind; int minDepth; int weight; };
    static constexpr Entry table[] = {
        { ItemKind::SpellbookMagicMissile, 1, 32 },
        { ItemKind::SpellbookMinorHeal,    1, 28 },
        { ItemKind::SpellbookBlink,        1, 22 },
        { ItemKind::SpellbookDetectTraps,  2, 18 },
        { ItemKind::SpellbookStoneskin,    3, 16 },
        { ItemKind::SpellbookHaste,        4, 14 },
        { ItemKind::SpellbookInvisibility, 5, 12 },
        { ItemKind::SpellbookPoisonCloud,  6, 10 },
        { ItemKind::SpellbookFireball,     8,  8 }, // deeper + rarer
    };

    int total = 0;
    for (const Entry& e : table) {
        if (depth >= e.minDepth) total += e.weight;
    }
    if (total <= 0) return ItemKind::SpellbookMagicMissile;

    int r = rng.range(1, total);
    for (const Entry& e : table) {
        if (depth < e.minDepth) continue;
        r -= e.weight;
        if (r <= 0) return e.kind;
    }

    return table[0].kind;
}


// -----------------------------------------------------------------------------
// Procedural monster variants (rank + affixes)
// -----------------------------------------------------------------------------

struct ProcAffixWeight {
    ProcMonsterAffix affix;
    int weight;
};

static bool procVariantEligible(EntityKind k, RoomType rt, int depth) {
    if (depth < 3) return false;
    if (rt == RoomType::Shop) return false;

    switch (k) {
        case EntityKind::Player:
        case EntityKind::Shopkeeper:
        case EntityKind::Dog:
        case EntityKind::Guard:
        case EntityKind::Minotaur:
            return false;
        default:
            return true;
    }
}

static ProcMonsterRank rollProcRank(RNG& rr, EntityKind k, int depth, RoomType rt) {
    const float t = std::clamp((depth - 1) / float(Game::DUNGEON_MAX_DEPTH - 1), 0.0f, 1.0f);

    // Base chances ramp with depth.
    float elite = 0.03f + 0.10f * t; // 3% -> 13%
    float champ = (t < 0.20f) ? 0.0f : (0.01f + 0.05f * (t - 0.20f) / 0.80f); // ~0% -> 6%
    float myth  = (t < 0.55f) ? 0.0f : (0.004f + 0.016f * (t - 0.55f) / 0.45f); // ~0% -> 2%

    // Room spice: treasure areas are a bit nastier.
    if (rt == RoomType::Vault || rt == RoomType::Treasure || rt == RoomType::Secret) {
        elite += 0.04f;
        champ += 0.02f;
        myth  += 0.01f;
    } else if (rt == RoomType::Lair) {
        elite += 0.02f;
    } else if (rt == RoomType::Laboratory) {
        champ += 0.01f;
    }

    // Kind bias: frail critters are less likely to show up as mythic.
    if (k == EntityKind::Bat || k == EntityKind::Slime) {
        myth *= 0.40f;
        champ *= 0.70f;
    }

    elite = std::clamp(elite, 0.0f, 0.30f);
    champ = std::clamp(champ, 0.0f, 0.18f);
    myth  = std::clamp(myth,  0.0f, 0.06f);

    float x = rr.next01();
    if (x < myth) return ProcMonsterRank::Mythic;
    x -= myth;
    if (x < champ) return ProcMonsterRank::Champion;
    x -= champ;
    if (x < elite) return ProcMonsterRank::Elite;
    return ProcMonsterRank::Normal;
}

static void buildProcAffixPool(std::vector<ProcAffixWeight>& out, EntityKind k, RoomType rt, int depth) {
    out.clear();
    out.reserve(12);

    auto add = [&](ProcMonsterAffix a, int w) {
        if (w <= 0) return;
        out.push_back({a, w});
    };

    const bool fast = (k == EntityKind::Bat || k == EntityKind::Wolf || k == EntityKind::Snake ||
                       k == EntityKind::Nymph || k == EntityKind::Leprechaun);
    const bool tough = (k == EntityKind::Ogre || k == EntityKind::Troll || k == EntityKind::Zombie || k == EntityKind::Wizard);
    const bool cunning = (k == EntityKind::Wizard || k == EntityKind::Nymph || k == EntityKind::Leprechaun || k == EntityKind::Mimic);

    int wSwift = fast ? 9 : 3;
    int wStone = tough ? 8 : 3;
    int wSavage = tough ? 6 : 4;
    int wBlink = (depth >= 4 && cunning) ? 7 : (depth >= 6 ? 2 : 0);
    int wGold = 2;

    // Combat-proc affixes.
    const bool undead = entityIsUndead(k);
    const bool beast = (k == EntityKind::Bat || k == EntityKind::Wolf || k == EntityKind::Snake || k == EntityKind::Spider || k == EntityKind::Dog);
    const bool humanoid = (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k));

    int wVenom = 0;
    if (beast || cunning) wVenom = 4;
    if (k == EntityKind::Snake || k == EntityKind::Spider) wVenom += 12;
    if (rt == RoomType::Lair) wVenom += 7;
    if (undead) wVenom = std::max(0, wVenom - 3);

    int wWeb = 0;
    if (k == EntityKind::Spider || k == EntityKind::Mimic) wWeb = 10;
    else if (rt == RoomType::Lair) wWeb = 5;
    if (cunning) wWeb += 2;
    if (undead) wWeb = std::max(0, wWeb - 2);

    int wFlame = 1 + depth / 5;
    if (rt == RoomType::Laboratory) wFlame += 9;
    if (rt == RoomType::Shrine) wFlame += 6;
    if (k == EntityKind::Wizard) wFlame += 6;
    if (k == EntityKind::Slime) wFlame = std::max(0, wFlame - 2);

    int wVamp = 0;
    if (depth >= 5) {
        wVamp = undead ? (8 + depth / 4) : 2;
        if (k == EntityKind::Ghost) wVamp += 6;
        if (rt == RoomType::Shrine) wVamp += 4;
        if (humanoid && depth >= 9) wVamp += 2;
    }

    // Humanoid-ish enemies are more likely to be gilded.
    if (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k)) wGold += 3;

    if (rt == RoomType::Vault || rt == RoomType::Treasure) wGold += 4;
    if (rt == RoomType::Lair) wSavage += 2;

    add(ProcMonsterAffix::Swift, wSwift);
    add(ProcMonsterAffix::Stonehide, wStone);
    add(ProcMonsterAffix::Savage, wSavage);
    add(ProcMonsterAffix::Blinking, wBlink);
    add(ProcMonsterAffix::Gilded, wGold);

    // Proc affixes that add on-hit status effects / sustain.
    add(ProcMonsterAffix::Venomous, wVenom);
    add(ProcMonsterAffix::Flaming, wFlame);
    add(ProcMonsterAffix::Vampiric, wVamp);
    add(ProcMonsterAffix::Webbing, wWeb);
}

static uint32_t rollProcAffixes(RNG& rr, EntityKind k, ProcMonsterRank rank, RoomType rt, int depth) {
    const int tier = procRankTier(rank);
    if (tier <= 0) return 0u;

    int want = (tier == 1) ? 1 : (tier == 2) ? 2 : 3;

    // Some early mythics roll only 2 affixes to keep spikes sane.
    if (rank == ProcMonsterRank::Mythic && depth < 12 && rr.chance(0.35f)) want = 2;

    std::vector<ProcAffixWeight> pool;
    buildProcAffixPool(pool, k, rt, depth);

    uint32_t mask = 0u;
    for (int n = 0; n < want; ++n) {
        int total = 0;
        for (const auto& e : pool) {
            if ((mask & procAffixBit(e.affix)) != 0u) continue;
            total += std::max(0, e.weight);
        }
        if (total <= 0) break;

        int roll = rr.range(1, total);
        ProcMonsterAffix picked = ProcMonsterAffix::None;
        for (const auto& e : pool) {
            if ((mask & procAffixBit(e.affix)) != 0u) continue;
            roll -= std::max(0, e.weight);
            if (roll <= 0) {
                picked = e.affix;
                break;
            }
        }
        if (picked == ProcMonsterAffix::None) break;
        mask |= procAffixBit(picked);
    }

    return mask;
}

// -----------------------------------------------------------------------------
// Procedural monster abilities (active kits)
// -----------------------------------------------------------------------------

struct ProcAbilityWeight {
    ProcMonsterAbility ability = ProcMonsterAbility::None;
    int weight = 0;
};

static void buildProcAbilityPool(std::vector<ProcAbilityWeight>& out,
                                 EntityKind k, RoomType rt, int depth, uint32_t affixMask) {
    out.clear();

    auto add = [&](ProcMonsterAbility a, int w) {
        if (a == ProcMonsterAbility::None) return;
        if (w <= 0) return;
        out.push_back({a, w});
    };

    const bool undead = entityIsUndead(k);
    const bool humanoid = (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k));

    const bool beast = (k == EntityKind::Wolf || k == EntityKind::Bat || k == EntityKind::Snake || k == EntityKind::Spider);
    const bool brute = (k == EntityKind::Ogre || k == EntityKind::Troll || k == EntityKind::Orc);
    const bool trickster = (k == EntityKind::Leprechaun || k == EntityKind::Nymph);
    const bool caster = (k == EntityKind::Wizard || rt == RoomType::Library || rt == RoomType::Laboratory || rt == RoomType::Shrine);

    // Mobility pressure: pounce is common on beasts and fast tricksters.
    int wPounce = 0;
    if (beast) wPounce += 10;
    if (trickster) wPounce += 8;
    if (k == EntityKind::Wolf) wPounce += 4;
    if (k == EntityKind::Bat) wPounce += 3;
    if (procHasAffix(affixMask, ProcMonsterAffix::Swift)) wPounce += 3;
    if (undead) wPounce = std::max(0, wPounce - 4);

    // Poison control: slimes / snakes / lairs / labs.
    int wToxic = 0;
    if (k == EntityKind::Slime) wToxic += 16;
    if (k == EntityKind::Snake || k == EntityKind::Spider) wToxic += 10;
    if (rt == RoomType::Lair) wToxic += 8;
    if (rt == RoomType::Laboratory) wToxic += 6;
    if (procHasAffix(affixMask, ProcMonsterAffix::Venomous)) wToxic += 4;
    if (undead) wToxic = std::max(0, wToxic - 3);

    // Fire control: wizards / shrines / labs; ramps slowly with depth.
    int wCinder = 1 + depth / 4;
    if (caster) wCinder += 8;
    if (k == EntityKind::Wizard) wCinder += 8;
    if (rt == RoomType::Shrine) wCinder += 4;
    if (procHasAffix(affixMask, ProcMonsterAffix::Flaming)) wCinder += 4;
    if (k == EntityKind::Slime) wCinder = std::max(0, wCinder - 2);

    // Defensive ward: brutes and humanoids like it.
    int wWard = 0;
    if (humanoid) wWard += 7;
    if (brute) wWard += 9;
    if (caster) wWard += 4;
    if (procHasAffix(affixMask, ProcMonsterAffix::Stonehide)) wWard += 3;
    if (undead) wWard = std::max(0, wWard - 1);

    // Summoning: necromancy / swarm rooms / deep dungeon.
    int wSummon = 0;
    if (caster) wSummon += 6;
    if (undead) wSummon += 10;
    if (rt == RoomType::Lair) wSummon += 7;
    if (k == EntityKind::Slime) wSummon += 6;
    if (depth >= 6) wSummon += 2;

    // Screech: confusion pressure (bats, tricksters, spiders).
    int wScreech = 0;
    if (k == EntityKind::Bat) wScreech += 14;
    if (trickster) wScreech += 10;
    if (k == EntityKind::Spider) wScreech += 6;
    if (rt == RoomType::Lair) wScreech += 3;
    if (undead) wScreech = std::max(0, wScreech - 2);

    add(ProcMonsterAbility::Pounce, wPounce);
    add(ProcMonsterAbility::ToxicMiasma, wToxic);
    add(ProcMonsterAbility::CinderNova, wCinder);
    add(ProcMonsterAbility::ArcaneWard, wWard);
    add(ProcMonsterAbility::SummonMinions, wSummon);
    add(ProcMonsterAbility::Screech, wScreech);
}

static void rollProcAbilities(RNG& rr, EntityKind k, ProcMonsterRank rank, RoomType rt, int depth, uint32_t affixMask,
                              ProcMonsterAbility& a1, ProcMonsterAbility& a2) {
    a1 = ProcMonsterAbility::None;
    a2 = ProcMonsterAbility::None;

    const int tier = procRankTier(rank);
    if (tier <= 0) return;

    int want = 1;
    if (tier >= 3) want = 2;
    else if (tier == 2 && rr.chance(0.35f)) want = 2;

    std::vector<ProcAbilityWeight> pool;
    buildProcAbilityPool(pool, k, rt, depth, affixMask);
    if (pool.empty()) return;

    auto pickOne = [&](ProcMonsterAbility avoid) -> ProcMonsterAbility {
        int total = 0;
        for (const auto& e : pool) {
            if (e.ability == avoid) continue;
            total += std::max(0, e.weight);
        }
        if (total <= 0) return ProcMonsterAbility::None;
        int roll = rr.range(1, total);
        for (const auto& e : pool) {
            if (e.ability == avoid) continue;
            roll -= std::max(0, e.weight);
            if (roll <= 0) return e.ability;
        }
        return ProcMonsterAbility::None;
    };

    a1 = pickOne(ProcMonsterAbility::None);
    if (want >= 2) {
        a2 = pickOne(a1);
        if (a2 == ProcMonsterAbility::None) {
            // If we couldn't pick a distinct second ability, fall back to a single-slot kit.
            a2 = ProcMonsterAbility::None;
        }
    }
}


static int scaledInt(int v, float mult) {
    const float f = static_cast<float>(v) * mult;
    return std::max(1, static_cast<int>(f + 0.5f));
}

static void applyProcVariant(Entity& e, ProcMonsterRank rank, uint32_t affixMask) {
    e.procRank = rank;
    e.procAffixMask = affixMask;

    const int tier = procRankTier(rank);
    if (tier <= 0 && affixMask == 0u) return;

    // Rank-based scaling.
    if (tier == 1) {
        e.hpMax = scaledInt(e.hpMax, 1.35f);
        e.baseAtk += 1;
        e.baseDef += 1;
        if (e.canRanged) e.rangedAtk += 1;
        e.speed = clampi(scaledInt(e.speed, 1.05f) + 4, 10, 230);
    } else if (tier == 2) {
        e.hpMax = scaledInt(e.hpMax, 1.60f);
        e.baseAtk += 2;
        e.baseDef += 2;
        if (e.canRanged) e.rangedAtk += 2;
        e.speed = clampi(scaledInt(e.speed, 1.08f) + 7, 10, 235);
        e.willFlee = false;
    } else if (tier >= 3) {
        e.hpMax = scaledInt(e.hpMax, 1.90f);
        e.baseAtk += 3;
        e.baseDef += 3;
        if (e.canRanged) e.rangedAtk += 3;
        e.speed = clampi(scaledInt(e.speed, 1.10f) + 10, 10, 240);
        e.willFlee = false;
    }

    // Affix-based scaling.
    if (procHasAffix(affixMask, ProcMonsterAffix::Swift)) {
        e.speed = clampi(scaledInt(e.speed, 1.25f), 10, 250);
    }
    if (procHasAffix(affixMask, ProcMonsterAffix::Stonehide)) {
        e.baseDef += 2;
        e.hpMax = scaledInt(e.hpMax, 1.15f);
    }
    if (procHasAffix(affixMask, ProcMonsterAffix::Savage)) {
        e.baseAtk += 2;
        if (e.canRanged) e.rangedAtk += 1;
    }

    // Keep numbers sane.
    e.baseAtk = std::max(0, e.baseAtk);
    e.baseDef = std::max(0, e.baseDef);
    if (e.canRanged) e.rangedAtk = std::max(0, e.rangedAtk);

    // After scaling: reset current HP.
    e.hp = e.hpMax;
}

} // namespace

Vec2i Game::randomFreeTileInRoom(const Room& r, int tries) {
    tries = std::max(10, tries);

    auto isValid = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        const TileType t = dung.at(x, y).type;
        if (!(t == TileType::Floor || t == TileType::StairsUp || t == TileType::StairsDown || t == TileType::DoorOpen)) return false;
        if (entityAt(x, y)) return false;
        return true;
    };

    for (int i = 0; i < tries; ++i) {
        const int x0 = rng.range(r.x + 1, std::max(r.x + 1, r.x + r.w - 2));
        const int y0 = rng.range(r.y + 1, std::max(r.y + 1, r.y + r.h - 2));
        if (!isValid(x0, y0)) continue;
        return {x0, y0};
    }

    // Fallback: brute scan the room interior for any valid tile.
    for (int y = r.y + 1; y < r.y + r.h - 1; ++y) {
        for (int x = r.x + 1; x < r.x + r.w - 1; ++x) {
            if (!isValid(x, y)) continue;
            return {x, y};
        }
    }

    // Degenerate rooms can end up completely packed (or even malformed). Avoid returning
    // an invalid tile that could place spawns inside walls or stacked on other entities.
    // Try a few random floors from the whole dungeon, then fall back to a full scan.
    for (int i = 0; i < tries * 4; ++i) {
        const Vec2i p = dung.randomFloor(rng, false);
        if (isValid(p.x, p.y)) return p;
    }

    for (int y = 1; y < dung.height - 1; ++y) {
        for (int x = 1; x < dung.width - 1; ++x) {
            if (isValid(x, y)) return {x, y};
        }
    }

    // Absolute last resort: clamp the room center to bounds.
    Vec2i c{r.cx(), r.cy()};
    if (!dung.inBounds(c.x, c.y)) {
        c.x = clampi(c.x, 0, std::max(0, dung.width - 1));
        c.y = clampi(c.y, 0, std::max(0, dung.height - 1));
    }
    return c;
}

Entity Game::makeMonster(EntityKind k, Vec2i pos, int groupId, bool allowGear, uint32_t forcedSpriteSeed, bool allowProcVariant) {
    Entity e;
    e.id = nextEntityId++;
    e.kind = k;
    e.pos = pos;
    e.groupId = groupId;
    e.spriteSeed = (forcedSpriteSeed != 0u) ? forcedSpriteSeed : rng.nextU32();

    const RoomType rtHere = roomTypeAt(dung, pos);

    // Monster turn scheduling (fix: ensure spawned monsters use their intended speed).
    e.speed = baseSpeedFor(k);

    // Seed perception with something reasonable so newly-spawned pack AI doesn't do
    // obviously-stupid things when the player is nearby.
    if (!ents.empty() && playerId_ != 0) {
        e.lastKnownPlayerPos = player().pos;
    }

    // Baselines per kind. Depth scaling happens below.
    const MonsterBaseStats ms = monsterStatsForDepth(k, depth_);
    e.hpMax = ms.hpMax;
    e.hp = e.hpMax;
    e.baseAtk = ms.baseAtk;
    e.baseDef = ms.baseDef;

    e.willFlee = ms.willFlee;
    e.packAI = ms.packAI;

    // Ranged stats are stored per-entity (saved/loaded), so set them here on spawn.
    e.canRanged = ms.canRanged;
    e.rangedRange = ms.rangedRange;
    e.rangedAtk = ms.rangedAtk;
    e.rangedProjectile = ms.rangedProjectile;
    e.rangedAmmo = ms.rangedAmmo;

    e.regenChancePct = ms.regenChancePct;
    e.regenAmount = ms.regenAmount;

    // Fix: ammo-based ranged monsters should spawn with a sensible quiver.
    if (e.rangedAmmo != AmmoKind::None) {
        const int depthBonus = std::max(0, (depth_ - 1) / 3);
        if (e.rangedAmmo == AmmoKind::Arrow) {
            e.rangedAmmoCount = 12 + depthBonus;
        } else if (e.rangedAmmo == AmmoKind::Rock) {
            e.rangedAmmoCount = 18 + depthBonus;
        }
        e.rangedAmmoCount = std::clamp(e.rangedAmmoCount, 6, 30);
    }

    // Spawn with basic gear for humanoid-ish monsters.
    // This makes loot feel more "earned" (you can take what they were using),
    // and creates emergent difficulty when monsters pick up better weapons/armor.
    if (allowGear && (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k))) {
        const RoomType rt = rtHere;

        auto makeGear = [&](ItemKind kind) -> Item {
            Item it;
            it.id = 1; // non-zero => present
            it.kind = kind;
            it.count = 1;
            it.spriteSeed = rng.nextU32();
            it.shopPrice = 0;
            it.shopDepth = 0;

            if (isWearableGear(kind)) {
                it.buc = rollBucForGear(rng, depth_, rt);

                // A little bit of enchantment scaling with depth.
                if (depth_ >= 4 && rng.chance(0.18f)) {
                    it.enchant = 1;
                    if (depth_ >= 7 && rng.chance(0.07f)) it.enchant = 2;
                }
            }

            // Rare ego weapons.
            it.ego = rollWeaponEgo(rng, kind, depth_, rt, /*fromShop=*/false, /*forMonster=*/true);

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

    // Pocket consumables: some intelligent monsters can spawn with a potion and
    // may use it mid-fight (see AI).
    if (allowGear && k == EntityKind::Wizard) {
        auto makePocket = [&](ItemKind kind, int count) -> Item {
            Item it;
            it.id = 1; // non-zero => present
            it.kind = kind;
            it.count = count;
            it.spriteSeed = rng.nextU32();
            it.shopPrice = 0;
            it.shopDepth = 0;
            // Consumables carried by monsters are always uncursed.
            it.buc = 0;
            it.enchant = 0;
            it.ego = ItemEgo::None;
            return it;
        };

        // Scale chance slightly with depth so deeper wizards are a bit more
        // prepared.
        const float chance = std::clamp(0.30f + 0.03f * static_cast<float>(depth_), 0.30f, 0.70f);
        if (rng.chance(chance)) {
            struct Opt { ItemKind k; int w; };
            std::vector<Opt> opts;
            opts.push_back({ItemKind::PotionHealing, 38});
            opts.push_back({ItemKind::PotionShielding, 26});
            opts.push_back({ItemKind::PotionRegeneration, (depth_ >= 6) ? 18 : 12});
            opts.push_back({ItemKind::PotionInvisibility, (depth_ >= 5) ? 14 : 7});
            if (depth_ >= 4) {
                // Levitation is... useful for navigating fissures and moats.
                opts.push_back({ItemKind::PotionLevitation, 10});
            }

            int total = 0;
            for (const auto& o : opts) total += std::max(0, o.w);
            if (total > 0) {
                int roll = rng.range(1, total);
                ItemKind picked = ItemKind::PotionHealing;
                for (const auto& o : opts) {
                    roll -= std::max(0, o.w);
                    if (roll <= 0) {
                        picked = o.k;
                        break;
                    }
                }

                int count = 1;
                // Occasional double-heal potion on very deep floors.
                if (picked == ItemKind::PotionHealing && depth_ >= 8 && rng.chance(0.25f)) {
                    count = 2;
                }
                e.pocketConsumable = makePocket(picked, count);
            }
        }
    }

    // Procedural monster variants (rank + affixes + abilities).
    // Applied after baseline stats/gear so modifiers scale the final creature.
    if (allowProcVariant && branch_ == DungeonBranch::Main && procVariantEligible(k, rtHere, depth_)) {
        const uint32_t seed = hashCombine(e.spriteSeed ^ 0xC0FFEEu,
                                          hashCombine(static_cast<uint32_t>(k),
                                                      hashCombine(static_cast<uint32_t>(depth_), static_cast<uint32_t>(rtHere))));
        RNG prng(seed);
        const ProcMonsterRank pr = rollProcRank(prng, k, depth_, rtHere);
        const uint32_t pm = rollProcAffixes(prng, k, pr, rtHere, depth_);
        applyProcVariant(e, pr, pm);

        // Roll a small active-ability kit for ranked monsters.
        rollProcAbilities(prng, k, pr, rtHere, depth_, pm, e.procAbility1, e.procAbility2);
        e.procAbility1Cd = 0;
        e.procAbility2Cd = 0;
    }

    return e;
}

Entity& Game::spawnMonster(EntityKind k, Vec2i pos, int groupId, bool allowGear) {
    ents.push_back(makeMonster(k, pos, groupId, allowGear));
    return ents.back();
}

void Game::spawnMonsters() {
    if (branch_ == DungeonBranch::Camp) return;

    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    int nextGroup = 1000;

    for (const Room& r : rooms) {
        // Shops: spawn a single shopkeeper and keep the shop otherwise free of hostiles.
        // (Shops already avoid trap placement; this makes them a safe-ish economic space.)
        if (r.type == RoomType::Shop) {
            // Prefer the room center so the shopkeeper doesn't block the doorway.
            Vec2i sp{r.cx(), r.cy()};
            if (!dung.inBounds(sp.x, sp.y) || !dung.isWalkable(sp.x, sp.y) || entityAt(sp.x, sp.y)) {
                sp = randomFreeTileInRoom(r);
            }
            if (sp == dung.stairsUp || sp == dung.stairsDown) {
                sp = randomFreeTileInRoom(r);
            }

            Entity& sk = spawnMonster(EntityKind::Shopkeeper, sp, 0, /*allowGear=*/false);
            sk.alerted = false;
            sk.energy = 0;
            continue;
        }

        const bool isStart = r.contains(dung.stairsUp.x, dung.stairsUp.y);
        int base = isStart ? 0 : 1;

        int depthTerm = (depth_ >= 3 ? 2 : 1);
        if (depth_ >= 7) depthTerm += 1;
        if (depth_ >= 9) depthTerm += 1;

        int n = rng.range(0, base + depthTerm);
        if (r.type == RoomType::Vault) n = rng.range(0, 1);

        for (int i = 0; i < n; ++i) {
            Vec2i p = randomFreeTileInRoom(r);

            EntityKind k = pickSpawnMonster(SpawnCategory::Room, rng, depth_);

            if (k == EntityKind::Wolf) {
                spawnMonster(k, p, nextGroup++);
            } else {
                spawnMonster(k, p, 0);
            }
        }

        // Guards in high-value rooms (plus some light security in themed rooms).
        const bool themedRoom = (r.type == RoomType::Armory || r.type == RoomType::Library || r.type == RoomType::Laboratory);
        if (r.type == RoomType::Secret || r.type == RoomType::Treasure || r.type == RoomType::Vault || themedRoom) {
            int guardians = 0;
            if (r.type == RoomType::Vault) guardians = rng.range(0, 1);
            else if (themedRoom) guardians = rng.range(0, 1);
            else guardians = rng.range(0, 2);
            for (int i = 0; i < guardians; ++i) {
                Vec2i p = randomFreeTileInRoom(r);
                EntityKind k = pickSpawnMonster(SpawnCategory::Guardian, rng, depth_);

                spawnMonster(k, p, 0);
            }

            // Thieves love rooms with loot. (Themed rooms are a bit less enticing.)
            if (depth_ >= 2) {
                float chance = 0.20f;
                if (r.type == RoomType::Vault) chance = 0.35f;
                else if (themedRoom) chance = 0.12f;

                if (rng.chance(chance)) {
                    Vec2i tp = randomFreeTileInRoom(r);
                    spawnMonster(EntityKind::Leprechaun, tp, 0);
                }
            }
        }

        // Lairs: wolf packs.
        if (r.type == RoomType::Lair) {
            const int pack = rng.range(2, 5);
            const int gid = nextGroup++;
            for (int i = 0; i < pack; ++i) {
                Vec2i p = randomFreeTileInRoom(r);
                spawnMonster(EntityKind::Wolf, p, gid);
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
            spawnMonster(EntityKind::Ogre, p, 0);

            // A couple of guards nearby.
            for (int i = 0; i < 2; ++i) {
                Vec2i q = randomFreeTileInRoom(*treasure);
                spawnMonster(EntityKind::Wolf, q, nextGroup++);
            }
        }

        // Deep milestone (roughly 3/4 through the run): introduce an ethereal threat
        // before the final approach. This keeps longer runs from feeling like a flat
        // difficulty plateau once the player is geared up.
        //
        // NOTE: we guard this with a runtime condition so MSVC doesn't warn about
        // constant-condition branches (C4127).
        if (depth_ > 0 && QUEST_DEPTH >= 16) {
            const int deepMilestone = MIDPOINT_DEPTH + std::max(2, (QUEST_DEPTH - MIDPOINT_DEPTH) / 2);
            if (depth_ == deepMilestone && depth_ < QUEST_DEPTH - 1) {
                Vec2i p = randomFreeTileInRoom(*treasure);
                spawnMonster(EntityKind::Ghost, p, 0);

                // A few shambling allies.
                for (int i = 0; i < 3; ++i) {
                    Vec2i q = randomFreeTileInRoom(*treasure);
                    spawnMonster(EntityKind::Zombie, q, nextGroup++);
                }
            }
        }

        // Penultimate floor: the Minotaur guards the central hoard.
        if (depth_ == QUEST_DEPTH - 1) {
            Vec2i p = randomFreeTileInRoom(*treasure);
            spawnMonster(EntityKind::Minotaur, p, 0);
        }

        // Final floor: a hostile archwizard guards the Amulet.
        if (depth_ == QUEST_DEPTH) {
            Vec2i p = randomFreeTileInRoom(*treasure);
            Entity& w = spawnMonster(EntityKind::Wizard, p, 0);

            // Upgrade into an "archwizard" (stronger ranged profile).
            w.rangedProjectile = ProjectileKind::Fireball;
            w.rangedRange = std::max(w.rangedRange, 6);
            w.rangedAtk += 2;
            w.hpMax += 6;
            w.hp = std::min(w.hpMax, w.hp + 6);
        }
    }
}

void Game::spawnItems() {
    if (branch_ == DungeonBranch::Camp) return;

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

            // Rare ego weapons (brands).
            it.ego = rollWeaponEgo(rng, k, depth_, rt, /*fromShop=*/false, /*forMonster=*/false);
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
        if (isWearableGear(k)) {
            it.buc = rollBucForGear(rng, depth_, rt);
            // A slightly higher chance of +1 items compared to the floor.
            float enchChance = (depth_ >= 2) ? 0.22f : 0.12f;
            enchChance += std::min(0.18f, depth_ * 0.02f);
            if (rng.chance(enchChance)) {
                it.enchant = 1;
                if (depth_ >= 6 && rng.chance(0.08f)) it.enchant = 2;
            }

            // Rare premium ego weapons.
            it.ego = rollWeaponEgo(rng, k, depth_, rt, /*fromShop=*/true, /*forMonster=*/false);
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
        // Expanded table to accommodate new gear (rings).
        int roll = rng.range(0, 199);

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
        else if (roll < 146) dropItemAt(ItemKind::ScrollMapping, randomFreeTileInRoom(r), 1);
        else if (roll < 147) {
            // A strange (mostly cosmetic) potion; keep it rare.
            dropItemAt(ItemKind::PotionHallucination, randomFreeTileInRoom(r), 1);
        }
        else if (roll < 149) {
            int pick = rng.range(0, 4);
            ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                      : (pick == 1) ? ItemKind::ScrollDetectTraps
                                      : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                      : (pick == 3) ? ItemKind::ScrollKnock
                                                    : ItemKind::ScrollEnchantRing;
            dropItemAt(sk, randomFreeTileInRoom(r), 1);
        }
        else if (roll < 151) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
        else if (roll < 153) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
        else if (roll < 156) dropItemAt(ItemKind::ScrollRemoveCurse, randomFreeTileInRoom(r), 1);
        else if (roll < 158) dropItemAt(ItemKind::ScrollConfusion, randomFreeTileInRoom(r), 1);
        else if (roll < 160) dropItemAt(ItemKind::ScrollFear, randomFreeTileInRoom(r), 1);
        else if (roll < 162) dropItemAt(ItemKind::ScrollEarth, randomFreeTileInRoom(r), 1);
        else if (roll < 163) dropItemAt(ItemKind::ScrollTaming, randomFreeTileInRoom(r), 1);
        else if (roll < 166) dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
        else if (roll < 172) {
            // Rare traversal utility in treasure rooms.
            if (depth_ >= 3 && rng.chance(0.33f)) {
                dropItemAt(ItemKind::PotionLevitation, randomFreeTileInRoom(r), 1);
            } else {
                dropItemAt(ItemKind::RingProtection, randomFreeTileInRoom(r), 1);
            }
        }
        else if (roll < 175) dropItemAt(ItemKind::RingMight, randomFreeTileInRoom(r), 1);
        else if (roll < 178) dropItemAt(ItemKind::RingAgility, randomFreeTileInRoom(r), 1);
        else if (roll < 181) dropItemAt(ItemKind::RingFocus, randomFreeTileInRoom(r), 1);
        else if (roll < 184) dropItemAt(ItemKind::RingSearching, randomFreeTileInRoom(r), 1);
        else if (roll < 187) dropItemAt(ItemKind::RingSustenance, randomFreeTileInRoom(r), 1);
        else if (roll < 193) dropItemAt(ItemKind::PotionEnergy, randomFreeTileInRoom(r), 1);
        else {
            // Rare: a spellbook.
            ItemKind bk = (depth_ >= 2) ? pickSpellbookKind(rng, depth_) : ItemKind::ScrollIdentify;
            dropItemAt(bk, randomFreeTileInRoom(r), 1);
        }
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
        // Deeper floors can also roll a lingering poison gas trap.
        int r = rng.range(0, 99);
        if (r < 28) return TrapKind::PoisonDart;
        if (r < 52) return TrapKind::Alarm;
        if (r < 72) return TrapKind::Web;
        if (depth_ >= 4) {
            if (r < 84) return TrapKind::ConfusionGas;
            if (r < 92) return TrapKind::PoisonGas;
            return TrapKind::Teleport;
        }
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
        chest.enchant = clampi(tier, 0, 4);
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
                    // Magic shop (wands/scrolls/potions + occasional spellbooks)
                    if (roll < 8) { k = pickSpellbookKind(rng, depth_); }
                    else if (roll < 20) { k = ItemKind::WandSparks; }
                    else if (roll < 28) { k = ItemKind::WandDigging; }
                    else if (roll < 32) { k = (depth_ >= 6 ? ItemKind::WandFireball : ItemKind::WandDigging); }
                    else if (roll < 40) { k = ItemKind::ScrollTeleport; }
                    else if (roll < 52) { k = ItemKind::ScrollMapping; }
                    else if (roll < 66) { k = ItemKind::ScrollIdentify; }
                    else if (roll < 72) { k = ItemKind::ScrollRemoveCurse; }
                    else if (roll < 78) { k = ItemKind::ScrollFear; }
                    else if (roll < 82) { k = ItemKind::ScrollEarth; }
                    else if (roll < 84) { k = ItemKind::ScrollTaming; }
                    else if (roll < 86) { k = ItemKind::PotionStrength; }
                    else if (roll < 92) { k = ItemKind::PotionRegeneration; }
                    else if (roll < 96) { k = ItemKind::PotionHaste; }
                    else if (roll < 98) { k = ItemKind::PotionEnergy; }
                    else if (roll < 99) {
                        // A small chance of rings showing up in the magic shop.
                        const int rr = rng.range(0, 99);
                        if (rr < 28) k = ItemKind::RingProtection;
                        else if (rr < 50) k = ItemKind::RingMight;
                        else if (rr < 70) k = ItemKind::RingAgility;
                        else if (rr < 85) k = ItemKind::RingFocus;
                        else if (rr < 95) k = ItemKind::RingSearching;
                        else k = ItemKind::RingSustenance;
                    } else {
                        // Rare traversal utility.
                        if (rng.chance(0.18f)) {
                            k = ItemKind::PotionHallucination;
                        } else if (depth_ >= 3 && rng.chance(0.25f)) {
                            k = ItemKind::PotionLevitation;
                        } else {
                            k = (depth_ >= 5 ? ItemKind::PotionInvisibility : ItemKind::PotionVision);
                        }
                    }
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
            if (rng.chance(0.10f)) dropItemAt(ItemKind::ScrollEnchantRing, randomFreeTileInRoom(r), 1);
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

        if (r.type == RoomType::Armory) {
            // A moderate gear cache: some weapons/armor/ammo. Less "spicy" than Treasure.
            dropItemAt(ItemKind::Gold, p, rng.range(6, 16) + depth_);

            const int drops = rng.range(2, 3);
            for (int i = 0; i < drops; ++i) {
                const int roll = rng.range(0, 99);
                if (roll < 18) {
                    dropItemAt(ItemKind::Sword, randomFreeTileInRoom(r), 1);
                } else if (roll < 34) {
                    dropItemAt(ItemKind::Axe, randomFreeTileInRoom(r), 1);
                } else if (roll < 48) {
                    dropItemAt(ItemKind::Dagger, randomFreeTileInRoom(r), 1);
                } else if (roll < 58) {
                    dropItemAt(ItemKind::Bow, randomFreeTileInRoom(r), 1);
                } else if (roll < 64) {
                    dropItemAt(ItemKind::Sling, randomFreeTileInRoom(r), 1);
                } else if (roll < 82) {
                    ItemKind ak = ItemKind::LeatherArmor;
                    if (depth_ >= 4 && rng.chance(0.40f)) ak = ItemKind::ChainArmor;
                    if (depth_ >= 7 && rng.chance(0.18f)) ak = ItemKind::PlateArmor;
                    dropItemAt(ak, randomFreeTileInRoom(r), 1);
                } else if (roll < 92) {
                    dropItemAt(ItemKind::Arrow, randomFreeTileInRoom(r), rng.range(6, 14));
                } else {
                    dropItemAt(ItemKind::Rock, randomFreeTileInRoom(r), rng.range(4, 12));
                }
            }

            // Small chance of a starter chest.
            if (rng.chance(0.30f)) {
                dropChestInRoom(r, 1, 0.40f, 0.30f);
            }
            continue;
        }

        if (r.type == RoomType::Library) {
            // Utility room: scrolls + the occasional wand.
            dropItemAt(ItemKind::Gold, p, rng.range(4, 14) + depth_);

            const int drops = rng.range(2, 4);
            for (int i = 0; i < drops; ++i) {
                // Occasionally a spellbook shows up (more likely on deeper floors).
                const float bookChance = std::min(0.24f, 0.06f + 0.02f * static_cast<float>(std::max(0, depth_ - 2)));
                if (depth_ >= 2 && rng.chance(bookChance)) {
                    dropItemAt(pickSpellbookKind(rng, depth_), randomFreeTileInRoom(r), 1);
                    continue;
                }

                const int roll = rng.range(0, 99);
                if (roll < 18) dropItemAt(ItemKind::ScrollIdentify, randomFreeTileInRoom(r), 1);
                else if (roll < 32) dropItemAt(ItemKind::ScrollMapping, randomFreeTileInRoom(r), 1);
                else if (roll < 46) dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
                else if (roll < 56) dropItemAt(ItemKind::ScrollKnock, randomFreeTileInRoom(r), 1);
                else if (roll < 64) dropItemAt(ItemKind::ScrollDetectTraps, randomFreeTileInRoom(r), 1);
                else if (roll < 72) dropItemAt(ItemKind::ScrollDetectSecrets, randomFreeTileInRoom(r), 1);
                else if (roll < 80) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
                else if (roll < 86) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
                else if (roll < 88) dropItemAt(ItemKind::ScrollEnchantRing, randomFreeTileInRoom(r), 1);
                else if (roll < 90) dropItemAt(ItemKind::ScrollRemoveCurse, randomFreeTileInRoom(r), 1);
                else if (roll < 93) dropItemAt(ItemKind::ScrollConfusion, randomFreeTileInRoom(r), 1);
                else if (roll < 95) dropItemAt(ItemKind::ScrollFear, randomFreeTileInRoom(r), 1);
                else if (roll < 97) dropItemAt(ItemKind::ScrollEarth, randomFreeTileInRoom(r), 1);
                else if (roll < 98) dropItemAt(ItemKind::ScrollTaming, randomFreeTileInRoom(r), 1);
                else {
                    ItemKind wk = ItemKind::WandSparks;
                    if (depth_ >= 4 && rng.chance(0.35f)) wk = ItemKind::WandDigging;
                    if (depth_ >= 7 && rng.chance(0.10f)) wk = ItemKind::WandFireball;
                    dropItemAt(wk, randomFreeTileInRoom(r), 1);
                }
            }

            if (rng.chance(0.22f)) {
                dropChestInRoom(r, 1, 0.35f, 0.35f);
            }
            continue;
        }

        if (r.type == RoomType::Laboratory) {
            // Potion-heavy room. Safer than Vault, but with a little "weird" edge.
            dropItemAt(ItemKind::Gold, p, rng.range(4, 14) + depth_);

            const int drops = rng.range(2, 4);
            for (int i = 0; i < drops; ++i) {
                const int roll = rng.range(0, 99);
                if (roll < 18) dropItemAt(ItemKind::PotionHealing, randomFreeTileInRoom(r), 1);
                else if (roll < 30) dropItemAt(ItemKind::PotionAntidote, randomFreeTileInRoom(r), 1);
                else if (roll < 40) dropItemAt(ItemKind::PotionStrength, randomFreeTileInRoom(r), 1);
                else if (roll < 50) dropItemAt(ItemKind::PotionClarity, randomFreeTileInRoom(r), 1);
                else if (roll < 60) dropItemAt(ItemKind::PotionRegeneration, randomFreeTileInRoom(r), 1);
                else if (roll < 70) dropItemAt(ItemKind::PotionShielding, randomFreeTileInRoom(r), 1);
                else if (roll < 78) dropItemAt(ItemKind::PotionHaste, randomFreeTileInRoom(r), 1);
                else if (roll < 88) {
                    const ItemKind pk = rng.chance(0.25f) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
                    dropItemAt(pk, randomFreeTileInRoom(r), 1);
                } else if (roll < 92) {
                    // The occasional utility scroll fits the "lab notes" vibe.
                    const ItemKind pool[] = { ItemKind::ScrollIdentify, ItemKind::ScrollRemoveCurse, ItemKind::ScrollTeleport };
                    dropItemAt(pool[rng.range(0, static_cast<int>(sizeof(pool) / sizeof(pool[0])) - 1)], randomFreeTileInRoom(r), 1);
                } else if (roll < 94) {
                    // Rare "experimental" potion.
                    dropItemAt(ItemKind::PotionHallucination, randomFreeTileInRoom(r), 1);
                } else {
                    // Rare: a wand (labs have tools).
                    ItemKind wk = ItemKind::WandSparks;
                    if (depth_ >= 4 && rng.chance(0.30f)) wk = ItemKind::WandDigging;
                    if (depth_ >= 8 && rng.chance(0.10f)) wk = ItemKind::WandFireball;
                    dropItemAt(wk, randomFreeTileInRoom(r), 1);
                }
            }

            if (rng.chance(0.28f)) {
                // Slightly higher trap chance than a library chest.
                dropChestInRoom(r, 1, 0.45f, 0.45f);
            }
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
                    ItemKind::ScrollEnchantRing,
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

    // Generator requested bonus loot spawns (e.g. behind boulder-bridge caches).
    // These are always "bonus" rewards and should never be required for floor traversal.
    for (const Vec2i& p : dung.bonusLootSpots) {
        if (!dung.inBounds(p.x, p.y)) continue;
        if (dung.at(p.x, p.y).type != TileType::Floor) continue;
        if (entityAt(p.x, p.y)) continue;

        Item chest;
        chest.kind = ItemKind::Chest;
        chest.id = nextItemId++;
        chest.count = 1;
        chest.buc = 0; // Uncursed
        chest.enchant = 0; // chest tier (see chestTier())
        chest.charges = 0; // lock/trap bits (see setChestLocked/Trapped)
        chest.spriteSeed = rng.nextU32();

        // Scale the cache a bit with depth.
        int tier = (depth_ <= 2) ? 1 : ((depth_ <= 5) ? 2 : 3);
        if (depth_ >= 6 && rng.chance(0.35f)) tier = 4;
        chest.enchant = std::clamp(tier, 1, 4);

        // Some caches are a bit spicy.
        if (rng.chance(0.40f)) setChestLocked(chest, true);
        if (rng.chance(0.30f)) {
            setChestTrapped(chest, true);
            setChestTrapKnown(chest, false);
            setChestTrapKind(chest, rollChestTrap());
        }

        ground.push_back(GroundItem{ chest, p });
    }
    // NOTE: do not clear bonusLootSpots here. The trap generator may place guard traps
    // near these bonus caches, and the list is consumed/cleared in spawnTraps().

    // A little extra ammo somewhere on the map.
    if (rng.chance(0.75f)) {
        Vec2i pos = dung.randomFloor(rng, true);
        if (!entityAt(pos.x, pos.y)) {
            if (rng.chance(0.55f)) dropItemAt(ItemKind::Arrow, pos, rng.range(6, 14));
            else dropItemAt(ItemKind::Rock, pos, rng.range(4, 12));
        }
    }

    // Item mimics: rare ground loot that turns into a Mimic when picked up.
    // This complements chest mimics and gives Mimics a more NetHack-flavored role.
    if (depth_ >= 2) {
        struct Cand { size_t idx; int w; };
        std::vector<Cand> cands;
        cands.reserve(ground.size());
        int totalW = 0;

        for (size_t i = 0; i < ground.size(); ++i) {
            const GroundItem& gi = ground[i];
            const Item& it = gi.item;

            // Never place item mimics in shops (too punishing / confusing with shop rules).
            if (it.shopPrice > 0) continue;

            // Skip world-interactables / noisy clutter.
            if (isChestKind(it.kind)) continue;
            if (isCorpseKind(it.kind)) continue;
            if (it.kind == ItemKind::Gold) continue;
            if (it.kind == ItemKind::AmuletYendor) continue;
            if (isStackable(it.kind)) continue;

            const ItemDef& def = itemDef(it.kind);
            if (def.value <= 0) continue;

            const RoomType rt = roomTypeAt(dung, gi.pos);
            if (rt == RoomType::Shop) continue;

            int roomW = 0;
            switch (rt) {
                case RoomType::Treasure:    roomW = 55; break;
                case RoomType::Vault:       roomW = 70; break;
                case RoomType::Secret:      roomW = 45; break;
                case RoomType::Armory:      roomW = 40; break;
                case RoomType::Library:     roomW = 35; break;
                case RoomType::Laboratory:  roomW = 35; break;
                default: break;
            }
            if (roomW <= 0) continue;

            // Weight toward tempting, high-value single items.
            int w = roomW;
            w += std::min(120, def.value / 2);
            w += std::min(30, depth_ * 2);
            if (w <= 0) continue;

            cands.push_back(Cand{i, w});
            totalW += w;
        }

        auto pickWeightedIndex = [&]() -> size_t {
            if (cands.empty() || totalW <= 0) return static_cast<size_t>(-1);
            int r = rng.range(1, totalW);
            for (const Cand& c : cands) {
                r -= c.w;
                if (r <= 0) return c.idx;
            }
            return cands.back().idx;
        };

        auto markOne = [&]() -> bool {
            const size_t pick = pickWeightedIndex();
            if (pick == static_cast<size_t>(-1) || pick >= ground.size()) return false;
            setItemMimicBait(ground[pick].item, true);

            // Remove from candidates so we don't double-mark the same item.
            for (size_t ci = 0; ci < cands.size(); ++ci) {
                if (cands[ci].idx == pick) {
                    totalW -= cands[ci].w;
                    cands.erase(cands.begin() + static_cast<std::vector<Cand>::difference_type>(ci));
                    break;
                }
            }
            return true;
        };

        // Chance to place 0..2 item mimics on a floor (rare, scaled gently with depth).
        float p1 = 0.10f + 0.02f * static_cast<float>(std::min(8, std::max(0, depth_ - 2)));
        p1 = std::min(0.35f, p1);
        if (rng.chance(p1)) {
            (void)markOne();

            float p2 = std::min(0.18f, p1 * 0.6f);
            if (depth_ >= 7 && rng.chance(p2)) {
                (void)markOne();
            }
        }
    }
}

void Game::spawnTraps() {
    if (branch_ == DungeonBranch::Camp) return;

    trapsCur.clear();

    // A small number of traps per floor, scaling gently with depth.
    // (Setpieces below may "spend" some of this budget by placing traps in patterns,
    // so the total density stays roughly stable.)
    const int base = 2;
    const int depthBonus = std::min(6, depth_ / 2);
    int targetCount = base + depthBonus + rng.range(0, 2);

    // Penultimate floor (the labyrinth) is intentionally trap-heavy.
    if (depth_ == QUEST_DEPTH - 1) {
        targetCount += 4;
    }

    auto alreadyHasTrap = [&](Vec2i p) {
        for (const auto& t : trapsCur) {
            if (t.pos == p) return true;
        }
        return false;
    };

    auto trapNear = [&](Vec2i p, int chebDist) {
        for (const auto& t : trapsCur) {
            if (chebyshev(t.pos, p) <= chebDist) return true;
        }
        return false;
    };

    auto isBadFloorPos = [&](Vec2i p) {
        if (!dung.inBounds(p.x, p.y)) return true;
        if (!dung.isWalkable(p.x, p.y)) return true;
        if (p == dung.stairsUp || p == dung.stairsDown) return true;

        // Avoid the immediate start area.
        if (manhattan(p, player().pos) <= 4) return true;

        // Don't place floor traps inside shops (keeps shopping from feeling punitive).
        // Shrines are also treated as relatively safe spaces.
        const RoomType rt = roomTypeAt(dung, p);
        if (rt == RoomType::Shop) return true;
        if (rt == RoomType::Shrine) return true;

        return false;
    };

    auto addFloorTrap = [&](Vec2i p, TrapKind tk, bool discovered = false, bool allowAdjacent = false) -> bool {
        if (isBadFloorPos(p)) return false;
        if (alreadyHasTrap(p)) return false;

        // Default: keep traps slightly spaced so floors aren't accidentally "minefields".
        if (!allowAdjacent && trapNear(p, 1)) return false;

        Trap t;
        t.kind = tk;
        t.pos = p;
        t.discovered = discovered;
        trapsCur.push_back(t);
        return true;
    };

    // ------------------------------------------------------------
    // Cache guards: bonus loot caches (requested by the dungeon generator)
    // get an extra little sting. These caches are always optional side objectives,
    // so guarding them increases risk/reward without blocking progression.
    // ------------------------------------------------------------
    auto hasChestAt = [&](Vec2i p) {
        for (const auto& gi : ground) {
            if (gi.pos == p && gi.item.kind == ItemKind::Chest) return true;
        }
        return false;
    };

    auto pickCacheGuardTrap = [&]() -> TrapKind {
        // Bias toward "security" traps rather than raw damage.
        // (The chest itself may also be trapped.)
        int r = rng.range(0, 99);
        if (depth_ <= 2) {
            if (r < 55) return TrapKind::Alarm;
            if (r < 88) return TrapKind::PoisonDart;
            return TrapKind::Web;
        }
        if (depth_ <= 5) {
            if (r < 40) return TrapKind::Alarm;
            if (r < 68) return TrapKind::PoisonDart;
            if (r < 88) return TrapKind::Web;
            return TrapKind::ConfusionGas;
        }
        // Deep floors: a touch more chaos.
        if (r < 30) return TrapKind::Alarm;
        if (r < 56) return TrapKind::PoisonDart;
        if (r < 74) return TrapKind::Web;
        if (r < 86) return TrapKind::ConfusionGas;
        if (r < 92) return TrapKind::PoisonGas;
        if (r < 95) return TrapKind::LetheMist;
        return TrapKind::Teleport;
    };

    for (const Vec2i& c : dung.bonusLootSpots) {
        if (!dung.inBounds(c.x, c.y)) continue;
        if (!hasChestAt(c)) continue;

        // Don't "ambush" the player in the start area even if a cache spawns close.
        if (manhattan(c, player().pos) <= 6) continue;

        // Try to place 1-2 guard traps around the cache.
        int want = 1;
        if (depth_ >= 6 && rng.chance(0.35f)) want = 2;
        if (depth_ == QUEST_DEPTH - 1 && rng.chance(0.40f)) want += 1;

        std::vector<Vec2i> adj;
        adj.reserve(8);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                Vec2i p{c.x + dx, c.y + dy};
                if (!dung.inBounds(p.x, p.y)) continue;
                adj.push_back(p);
            }
        }

        // Shuffle adjacency list for variety.
        for (int i = static_cast<int>(adj.size()) - 1; i > 0; --i) {
            const int j = rng.range(0, i);
            std::swap(adj[static_cast<size_t>(i)], adj[static_cast<size_t>(j)]);
        }

        int placed = 0;
        for (const Vec2i& p : adj) {
            if (placed >= want) break;
            // Allow adjacent guards here (cache rooms can get spicy).
            if (addFloorTrap(p, pickCacheGuardTrap(), false, true)) {
                placed += 1;
            }
        }
    }


    // ------------------------------------------------------------
    // Corridor gauntlets: sometimes place a short "strip" of traps along a
    // long straight corridor segment. This creates readable, avoidable hazards
    // and makes corridor navigation feel less uniform.
    // ------------------------------------------------------------
    const int W = dung.width;
    const int H = dung.height;
    auto idx = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y * W + x);
    };

    std::vector<uint8_t> inRoom(static_cast<size_t>(W * H), uint8_t{0});
    for (const Room& r : dung.rooms) {
        for (int y = r.y; y < r.y2(); ++y) {
            for (int x = r.x; x < r.x2(); ++x) {
                if (!dung.inBounds(x, y)) continue;
                inRoom[idx(x, y)] = 1u;
            }
        }
    }

    auto inAnyRoom = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        return inRoom[idx(x, y)] != 0u;
    };

    auto isCorridorFloor = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (inAnyRoom(x, y)) return false;
        if (!dung.isWalkable(x, y)) return false;
        const TileType tt = dung.at(x, y).type;
        return tt == TileType::Floor;
    };

    auto pickStripTrap = [&]() -> TrapKind {
        // Strips lean toward classic damage/control traps.
        int r = rng.range(0, 99);
        if (depth_ <= 2) {
            return (r < 70) ? TrapKind::Spike : TrapKind::PoisonDart;
        }
        if (depth_ <= 5) {
            if (r < 45) return TrapKind::Spike;
            if (r < 78) return TrapKind::PoisonDart;
            if (r < 90) return TrapKind::Web;
            return TrapKind::Alarm;
        }
        if (r < 33) return TrapKind::Spike;
        if (r < 61) return TrapKind::PoisonDart;
        if (r < 74) return TrapKind::Web;
        if (r < 84) return TrapKind::Alarm;
        if (r < 92) return TrapKind::ConfusionGas;
        return TrapKind::PoisonGas;
    };

    struct StraightCorr {
        Vec2i p;
        int axis; // 0 = horizontal, 1 = vertical
    };

    std::vector<StraightCorr> straight;
    straight.reserve(512);

    std::vector<Vec2i> candidatesAll;
    candidatesAll.reserve(static_cast<size_t>(W * H / 3));

    std::vector<Vec2i> chokepoints;
    chokepoints.reserve(512);

    auto walk4 = [&](int x, int y) -> bool {
        return dung.inBounds(x, y) && dung.isWalkable(x, y);
    };

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            Vec2i p{x, y};
            if (isBadFloorPos(p)) continue;

            // Keep the candidate pool to true floor-like tiles.
            const TileType tt = dung.at(x, y).type;
            if (!(tt == TileType::Floor || tt == TileType::DoorOpen)) continue;

            candidatesAll.push_back(p);

            if (isCorridorFloor(x, y)) {
                const bool L = walk4(x - 1, y);
                const bool R = walk4(x + 1, y);
                const bool U = walk4(x, y - 1);
                const bool D = walk4(x, y + 1);
                const int deg = (L ? 1 : 0) + (R ? 1 : 0) + (U ? 1 : 0) + (D ? 1 : 0);

                // Corridor chokepoints are good trap candidates.
                if (deg <= 2) chokepoints.push_back(p);

                // Identify straight 1-wide corridor segments for trap strips.
                if (deg == 2) {
                    if (L && R && !U && !D) straight.push_back(StraightCorr{p, 0});
                    else if (U && D && !L && !R) straight.push_back(StraightCorr{p, 1});
                }
            }
        }
    }

    int gauntletsWanted = 0;
    if (depth_ >= 3 && rng.chance(0.22f)) gauntletsWanted = 1;
    if (depth_ == QUEST_DEPTH - 1) gauntletsWanted = 1;

    for (int gk = 0; gk < gauntletsWanted; ++gk) {
        if (straight.empty()) break;

        bool placed = false;
        for (int tries = 0; tries < 120 && !placed; ++tries) {
            const StraightCorr sc = straight[static_cast<size_t>(rng.range(0, static_cast<int>(straight.size()) - 1))];

            // Avoid the start area.
            if (manhattan(sc.p, player().pos) <= 7) continue;

            Vec2i a = sc.p;
            Vec2i b = sc.p;

            auto stepBack = [&](Vec2i v) {
                if (sc.axis == 0) return Vec2i{v.x - 1, v.y};
                return Vec2i{v.x, v.y - 1};
            };
            auto stepFwd = [&](Vec2i v) {
                if (sc.axis == 0) return Vec2i{v.x + 1, v.y};
                return Vec2i{v.x, v.y + 1};
            };

            // Extend to find the corridor run.
            for (int i = 0; i < 32; ++i) {
                Vec2i na = stepBack(a);
                if (!dung.inBounds(na.x, na.y)) break;
                if (!isCorridorFloor(na.x, na.y)) break;
                a = na;
            }
            for (int i = 0; i < 32; ++i) {
                Vec2i nb = stepFwd(b);
                if (!dung.inBounds(nb.x, nb.y)) break;
                if (!isCorridorFloor(nb.x, nb.y)) break;
                b = nb;
            }

            const int len = (sc.axis == 0) ? (b.x - a.x + 1) : (b.y - a.y + 1);
            if (len < 8) continue;

            // Decide how many traps to place along the run.
            int want = 3;
            if (depth_ >= 4) want += 1;
            if (depth_ >= 7 && rng.chance(0.35f)) want += 1;
            want = std::min(want, 6);

            // Place every other tile to keep it readable (and reduce chain triggers).
            const int stride = 2;
            const int maxSlots = (len - 2) / stride;
            if (maxSlots < want) want = std::max(3, maxSlots);
            if (want <= 0) continue;

            int placedHere = 0;
            int startOff = 1 + rng.range(0, 1); // 1 or 2

            for (int i = 0; i < want; ++i) {
                int step = startOff + i * stride;
                if (step <= 0) continue;
                if (step >= len - 1) break;

                Vec2i p = a;
                if (sc.axis == 0) p.x += step;
                else p.y += step;

                if (addFloorTrap(p, pickStripTrap(), false, true)) {
                    placedHere += 1;
                }
            }

            if (placedHere >= 3) {
                placed = true;
            }
        }
    }


    // ------------------------------------------------------------
    // Baseline trap scatter: fill the remaining budget, biased toward
    // corridors and junction-y spaces.
    // ------------------------------------------------------------
    auto pickBaseTrap = [&]() -> TrapKind {
        // Choose trap type (deeper floors skew deadlier).
        int roll = rng.range(0, 99);
        TrapKind tk = TrapKind::Spike;
        if (depth_ == QUEST_DEPTH - 1) {
            // Labyrinth: more "tactical" traps than raw damage.
            if (roll < 22) tk = TrapKind::Spike;
            else if (roll < 44) tk = TrapKind::PoisonDart;
            else if (roll < 64) tk = TrapKind::Alarm;
            else if (roll < 80) tk = TrapKind::Web;
            else if (roll < 86) tk = TrapKind::ConfusionGas;
            else if (roll < 90) tk = TrapKind::PoisonGas;
            else if (roll < 92) tk = TrapKind::LetheMist;
            else if (roll < 96) tk = TrapKind::RollingBoulder;
            else if (depth_ < DUNGEON_MAX_DEPTH && roll < 98) tk = TrapKind::TrapDoor;
            else tk = TrapKind::Teleport;
        } else if (depth_ <= 1) {
            tk = (roll < 70) ? TrapKind::Spike : TrapKind::PoisonDart;
        } else if (depth_ <= 3) {
            if (roll < 43) tk = TrapKind::Spike;
            else if (roll < 73) tk = TrapKind::PoisonDart;
            else if (roll < 85) tk = TrapKind::Alarm;
            else if (roll < 91) tk = TrapKind::Web;
            else if (roll < 95) tk = TrapKind::ConfusionGas;
            else if (roll < 97) tk = TrapKind::RollingBoulder;
            else tk = TrapKind::Teleport;
        } else {
            if (roll < 33) tk = TrapKind::Spike;
            else if (roll < 61) tk = TrapKind::PoisonDart;
            else if (roll < 76) tk = TrapKind::Alarm;
            else if (roll < 86) tk = TrapKind::Web;
            else if (roll < 90) tk = TrapKind::ConfusionGas;
            else if (roll < 93) tk = TrapKind::PoisonGas;
            else if (roll < 95) tk = TrapKind::LetheMist;
            else if (roll < 97) tk = TrapKind::RollingBoulder;
            else if (depth_ < DUNGEON_MAX_DEPTH && roll < 99) tk = TrapKind::TrapDoor;
            else tk = TrapKind::Teleport;
        }
        return tk;
    };

    auto pickFrom = [&](const std::vector<Vec2i>& v) -> Vec2i {
        return v[static_cast<size_t>(rng.range(0, static_cast<int>(v.size()) - 1))];
    };

    int attempts = 0;
    while (static_cast<int>(trapsCur.size()) < targetCount && attempts < targetCount * 90) {
        ++attempts;

        Vec2i p{-1, -1};
        const float r = rng.next01();

        // Bias toward corridor chokepoints when available.
        if (r < 0.55f && !chokepoints.empty()) {
            p = pickFrom(chokepoints);
        } else if (!candidatesAll.empty()) {
            p = pickFrom(candidatesAll);
        } else {
            p = dung.randomFloor(rng, true);
        }

        if (alreadyHasTrap(p)) continue;

        // Note: addFloorTrap() handles spacing + shop/shrine avoidance.
        (void)addFloorTrap(p, pickBaseTrap(), false, false);
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
            if (depth_ >= 4 && rng.chance(0.10f)) t.kind = TrapKind::PoisonGas;
            else if (rng.chance(0.10f)) t.kind = TrapKind::ConfusionGas;
            else t.kind = rng.chance(0.55f) ? TrapKind::Alarm : TrapKind::PoisonDart;
            trapsCur.push_back(t);
        }
    }

    // Themed hazard: laboratories tend to have extra volatile traps.
    // This is intentionally light-touch (0-2 extra) so it adds flavor without
    // turning every floor into a minefield.
    for (const Room& r : dung.rooms) {
        if (r.type != RoomType::Laboratory) continue;

        int extra = rng.chance(0.60f) ? 1 : 0;
        if (depth_ >= 6 && rng.chance(0.25f)) extra += 1;

        for (int i = 0; i < extra; ++i) {
            Vec2i p = randomFreeTileInRoom(r);
            if (isBadFloorPos(p)) continue;
            if (alreadyHasTrap(p)) continue;

            Trap t;
            t.pos = p;
            t.discovered = false;
            const int roll = rng.range(0, 99);
            if (roll < 45) t.kind = TrapKind::ConfusionGas;
            else if (roll < 62) t.kind = TrapKind::PoisonGas;
            else if (roll < 88) t.kind = TrapKind::PoisonDart;
            else if (roll < 95) t.kind = TrapKind::Alarm;
            else t.kind = TrapKind::Teleport;
            trapsCur.push_back(t);
        }
    }

    // Consume generator hints (bonus cache locations) now that traps have been placed.
    dung.bonusLootSpots.clear();

}

void Game::applyEndOfTurnEffects() {
    if (gameOver) return;

    Entity& p = playerMut();

    // Per-level wind: biases drifting hazards (gas, fire). Deterministic from run seed + level id.
    const Vec2i wind = windDir();
    const int windStr = windStrength();
    const Vec2i upWind = {-wind.x, -wind.y};


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

    // ------------------------------------------------------------
    // Environmental fields: Poison Gas (persistent, tile-based)
    //
    // Poison gas is stored as an intensity map (0..255). Entities standing
    // in gas have their poison duration "topped up" each turn.
    // ------------------------------------------------------------
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (poisonGas_.size() != expect) poisonGas_.assign(expect, 0u);

        auto gasIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto gasAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return 0u;
            const size_t i = gasIdx(x, y);
            if (i >= poisonGas_.size()) return 0u;
            return poisonGas_[i];
        };

        auto applyGasTo = [&](Entity& e, bool isPlayer) {
            const uint8_t g = gasAt(e.pos.x, e.pos.y);
            if (g == 0u) return;

            // Scale poison severity with gas intensity.
            int minTurns = 2 + static_cast<int>(g) / 2;
            minTurns = clampi(minTurns, 2, 10);

            const int before = e.effects.poisonTurns;
            if (before < minTurns) e.effects.poisonTurns = minTurns;

            // Message only on first exposure (avoids log spam while standing in gas).
            if (before == 0 && e.effects.poisonTurns > 0) {
                if (isPlayer) {
                    pushMsg("YOU INHALE TOXIC VAPORS!", MessageKind::Warning, true);
                    pushMsg("YOU ARE POISONED!", MessageKind::Warning, true);
                } else if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(e.kind) << " CHOKES ON TOXIC VAPORS!";
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

// ------------------------------------------------------------
    // Environmental fields: Fire (persistent, tile-based)
    //
    // Fire is stored as an intensity map (0..255). Entities standing on fire have
    // their burn duration "topped up" each turn.
    // ------------------------------------------------------------
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (fireField_.size() != expect) fireField_.assign(expect, 0u);

        auto fireIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto fireAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return 0u;
            const size_t i = fireIdx(x, y);
            if (i >= fireField_.size()) return 0u;
            return fireField_[i];
        };

        auto applyFireTo = [&](Entity& e, bool isPlayer) {
            const uint8_t f = fireAt(e.pos.x, e.pos.y);
            if (f == 0u) return;

            // Scale burn severity with fire intensity. Keep the minimum at 2 so it
            // doesn't instantly expire on the same turn it is applied.
            int minTurns = 2 + static_cast<int>(f) / 3;
            minTurns = clampi(minTurns, 2, 10);

            const int before = e.effects.burnTurns;
            if (before < minTurns) e.effects.burnTurns = minTurns;

            // Message only on first ignition.
            if (before == 0 && e.effects.burnTurns > 0) {
                if (isPlayer) {
                    pushMsg("YOU ARE ENGULFED IN FLAMES!", MessageKind::Warning, true);
                } else if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(e.kind) << " CATCHES FIRE!";
                    pushMsg(ss.str(), MessageKind::Info, false);
                }
            }
        };

        applyFireTo(p, true);
        for (auto& m : ents) {
            if (m.id == playerId_) continue;
            if (m.hp <= 0) continue;
            applyFireTo(m, false);
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

    // Burning: hurts once per full turn.
    if (p.effects.burnTurns > 0) {
        p.effects.burnTurns = std::max(0, p.effects.burnTurns - 1);
        p.hp -= 1;
        if (p.hp <= 0) {
            pushMsg("YOU BURN TO DEATH.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = "BURNED TO DEATH";
            gameOver = true;
            return;
        }

        if (p.effects.burnTurns == 0) {
            pushMsg(effectEndMessage(EffectKind::Burn), MessageKind::System, true);
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

    // Timed levitation: lets you traverse chasms safely while >0.
    if (p.effects.levitationTurns > 0) {
        const int before = p.effects.levitationTurns;
        p.effects.levitationTurns = std::max(0, p.effects.levitationTurns - 1);
        if (before > 0 && p.effects.levitationTurns == 0) {
            // If levitation ends while over a chasm, you fall.
            if (dung.inBounds(p.pos.x, p.pos.y) && dung.at(p.pos.x, p.pos.y).type == TileType::Chasm) {
                const int dmg = rng.range(4, 8) + std::min(4, depth_ / 2);
                pushMsg("YOUR LEVITATION ENDS! YOU FALL!", MessageKind::Warning, true);

                // Try to "spill" you onto the nearest solid tile rather than softlocking you in a chasm.
                Vec2i landing = {-1, -1};
                for (int r = 1; r <= 8 && landing.x < 0; ++r) {
                    std::vector<Vec2i> cand;
                    for (int dy = -r; dy <= r; ++dy) {
                        for (int dx = -r; dx <= r; ++dx) {
                            if (std::max(std::abs(dx), std::abs(dy)) != r) continue; // ring
                            const int x = p.pos.x + dx;
                            const int y = p.pos.y + dy;
                            if (!dung.inBounds(x, y)) continue;
                            if (!dung.isWalkable(x, y)) continue;
                            if (Entity* o = entityAtMut(x, y)) {
                                if (o->id != p.id) continue;
                            }
                            cand.push_back({x, y});
                        }
                    }
                    if (!cand.empty()) {
                        landing = cand[static_cast<size_t>(rng.range(0, static_cast<int>(cand.size()) - 1))];
                    }
                }

                if (landing.x >= 0) {
                    p.pos = landing;
                } else {
                    // Emergency fallback: collapse the chasm tile into a floor tile.
                    dung.at(p.pos.x, p.pos.y).type = TileType::Floor;
                    pushMsg("YOU CRASH DOWN, FILLING IN THE CHASM BENEATH YOU!", MessageKind::Warning, true);
                }

                emitNoise(p.pos, 18);
                p.hp -= dmg;
                {
                    std::ostringstream ss;
                    ss << "YOU TAKE " << dmg << ".";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                }
                if (p.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "FELL INTO A CHASM";
                    gameOver = true;
                    return;
                }
            } else {
                pushMsg(effectEndMessage(EffectKind::Levitation), MessageKind::System, true);
            }
        }
    }

    // Timed fear: primarily affects monster AI, but is tracked generically as a status effect.
    if (p.effects.fearTurns > 0) {
        p.effects.fearTurns = std::max(0, p.effects.fearTurns - 1);
        if (p.effects.fearTurns == 0) {
            pushMsg(effectEndMessage(EffectKind::Fear), MessageKind::System, true);
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

    // Timed hallucinations: mostly a perception hazard.
    if (p.effects.hallucinationTurns > 0) {
        p.effects.hallucinationTurns = std::max(0, p.effects.hallucinationTurns - 1);
        if (p.effects.hallucinationTurns == 0) {
            pushMsg(effectEndMessage(EffectKind::Hallucination), MessageKind::System, true);
        } else {
            // Occasional deterministic flavor without consuming RNG state.
            static constexpr const char* kMsgs[] = {
                "THE WALLS BREATHE.",
                "YOU HEAR COLORS AND SEE SOUNDS.",
                "A DISTANT LAUGH ECHOES THROUGH THE STONE.",
                "THE AIR TASTES LIKE LIGHTNING.",
                "YOUR SHADOW MOVES A LITTLE LATE.",
            };

            // Salt "HALL" in ASCII (0x48 0x41 0x4C 0x4C) to keep the hash deterministic without
            // consuming RNG state.
            const uint32_t h = hashCombine(hash32(seed_ ^ 0xC0FFEEu), hashCombine(turnCount, 0x48414C4Cu));
            if ((h % 37u) == 0u) {
                const size_t idx = (h / 37u) % (sizeof(kMsgs) / sizeof(kMsgs[0]));
                pushMsg(kMsgs[idx], MessageKind::Info, true);
            }
        }
    }

    // Natural regeneration (slow baseline healing).
    // Intentionally disabled while poisoned to keep poison meaningful.
    if (p.effects.poisonTurns > 0 || p.effects.burnTurns > 0 || p.hp >= p.hpMax) {
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

    // Mana regeneration (deterministic; keyed off turnCount so save/load remains consistent).
    // Intentionally slower than HP regen and primarily scaled by FOCUS.
    {
        const int manaMax = playerManaMax();
        if (manaMax > 0 && mana_ < manaMax) {
            const int focus = playerFocus();
            const int level = std::max(1, playerCharLevel());
            // Baseline: 1 mana per ~9 turns at low focus, improving with focus/level.
            int interval = 11 - (focus / 2) - (level / 3);
            interval = clampi(interval, 2, 12);
            if (interval <= 0) interval = 2;

            if ((turnCount % static_cast<uint32_t>(interval)) == 0u) {
                mana_ = std::min(manaMax, mana_ + 1);
            }
        }
    }

    // Hunger ticking (optional).
    if (hungerEnabled_) {
        if (hungerMax <= 0) hungerMax = 800;

        // Ring of Sustenance slows hunger loss (deterministic; uses turnCount so save/load stays consistent).
        int sustainInterval = 1;
        bool hasSustenance = false;
        int bestPower = -9999;

        auto consider = [&](const Item* r) {
            if (!r) return;
            if (r->kind != ItemKind::RingSustenance) return;
            hasSustenance = true;

            int p = r->enchant;
            if (r->buc < 0) p -= 1;
            else if (r->buc > 0) p += 1;

            bestPower = std::max(bestPower, p);
        };

        consider(equippedRing1());
        consider(equippedRing2());

        if (hasSustenance) {
            // Base: drain 1 hunger every 2 turns (power 0).
            // Enchant/blessing increases the interval; curses remove the benefit.
            sustainInterval = 2 + bestPower;
            sustainInterval = clampi(sustainInterval, 1, 5);
        }

        if (!hasSustenance || (turnCount % static_cast<uint32_t>(sustainInterval)) == 0u) {
            hunger = std::max(0, hunger - 1);
        }

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

                // Corpse revival: when a corpse becomes stale it may rise once.
                // NOTE: We use Item::enchant as a tiny per-stack flag for corpses:
                //   0 = not checked for rising yet
                //   1 = rising check has been performed (success or failure)
                //
                // This avoids growing the save format and is safe because corpses do not
                // use enchantment gameplay in the inventory UI.
                if (gi.item.charges > 60 && gi.item.charges <= 160 && gi.item.enchant == 0) {
                    // Only attempt to spawn something if the corpse is on a valid walkable tile.
                    if (dung.inBounds(gi.pos.x, gi.pos.y) && dung.isWalkable(gi.pos.x, gi.pos.y)) {
                        // Only if the tile is empty. (If someone is standing on the corpse,
                        // it can't get up; we'll try again later.)
                        if (entityAt(gi.pos.x, gi.pos.y) == nullptr) {
                            gi.item.enchant = 1;

                            EntityKind riseKind = EntityKind::Zombie;
                            int bonusHp = 0;
                            int bonusAtk = 0;
                            int bonusDef = 0;

                            float chance = 0.06f + 0.01f * std::min(depth_, 20);

                            // A few special cases for "NetHack-ish" flavor.
                            switch (gi.item.kind) {
                                case ItemKind::CorpseTroll:
                                    // Trolls are infamous for regenerating.
                                    riseKind = EntityKind::Troll;
                                    chance = 0.20f + 0.02f * std::min(depth_, 15);
                                    break;

                                case ItemKind::CorpseSlime:
                                    // Slimes can reconstitute.
                                    riseKind = EntityKind::Slime;
                                    chance = 0.18f + 0.02f * std::min(depth_, 12);
                                    break;

                                case ItemKind::CorpseMimic:
                                    // Mimics are weird.
                                    riseKind = EntityKind::Mimic;
                                    chance = 0.14f + 0.015f * std::min(depth_, 12);
                                    break;

                                case ItemKind::CorpseWizard:
                                    // A wizard's spirit may linger.
                                    riseKind = EntityKind::Ghost;
                                    chance = 0.12f + 0.015f * std::min(depth_, 12);
                                    break;

                                case ItemKind::CorpseMinotaur:
                                    // Big corpse -> beefier zombie.
                                    riseKind = EntityKind::Zombie;
                                    chance = 0.10f + 0.015f * std::min(depth_, 12);
                                    bonusHp = 8;
                                    bonusAtk = 2;
                                    bonusDef = 1;
                                    break;

                                default:
                                    break;
                            }

                            chance = std::clamp(chance, 0.02f, 0.40f);

                            if (rng.chance(chance)) {
                                const bool vis = dung.at(gi.pos.x, gi.pos.y).visible;

                                if (vis) {
                                    std::ostringstream ss;
                                    if (riseKind == EntityKind::Zombie) {
                                        ss << "A CORPSE RISES AS A ZOMBIE!";
                                    } else {
                                        ss << "THE " << itemDef(gi.item.kind).name << " RISES!";
                                    }
                                    pushMsg(ss.str(), MessageKind::System, true);
                                }

                                // Loud enough to wake nearby monsters even if the player doesn't see it.
                                emitNoise(gi.pos, 14);

                                Entity risen = makeMonster(riseKind, gi.pos, 0, false);

                                // If this happened in view, the risen creature is immediately "alerted".
                                if (vis) {
                                    risen.alerted = true;
                                    risen.lastKnownPlayerPos = player().pos;
                                }

                                // Corpse-specific stat bumps (used for big bodies like Minotaurs).
                                if (bonusHp > 0) {
                                    risen.hpMax += bonusHp;
                                    risen.hp = risen.hpMax;
                                }
                                risen.baseAtk += bonusAtk;
                                risen.baseDef += bonusDef;

                                ents.push_back(risen);

                                // Consume one corpse from the stack (if stacked).
                                if (gi.item.count > 1) {
                                    gi.item.count -= 1;
                                } else {
                                    ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));
                                    continue;
                                }
                            }
                        }
                    }
                }

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

        // Timed poison: lose 1 HP per full turn (except undead).
        if (m.effects.poisonTurns > 0) {
            const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;

            if (entityIsUndead(m.kind)) {
                // Undead don't suffer poison damage, but the effect still times out.
                m.effects.poisonTurns = std::max(0, m.effects.poisonTurns - 1);

                if (m.effects.poisonTurns == 0 && vis) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " SHRUGS OFF THE POISON.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            } else {
                m.effects.poisonTurns = std::max(0, m.effects.poisonTurns - 1);
                m.hp -= 1;

                if (m.hp <= 0) {
                    if (vis) {
                        std::ostringstream ss;
                        ss << kindName(m.kind) << " SUCCUMBS TO POISON.";
                        pushMsg(ss.str(), MessageKind::Combat, false);
                    }
                } else if (m.effects.poisonTurns == 0) {
                    if (vis) {
                        std::ostringstream ss;
                        ss << kindName(m.kind) << " RECOVERS FROM POISON.";
                        pushMsg(ss.str(), MessageKind::System, false);
                    }
                }
            }
        }

        // Burning: damage over time.
        if (m.effects.burnTurns > 0) {
            m.effects.burnTurns = std::max(0, m.effects.burnTurns - 1);
            m.hp -= 1;

            if (m.hp <= 0) {
                // Only message if the monster is currently visible to the player.
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " BURNS TO DEATH.";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                }
            } else if (m.effects.burnTurns == 0) {
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " STOPS BURNING.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            }
        }

        // Regeneration potion (or similar): heals 1 HP per turn while active.
        if (m.effects.regenTurns > 0) {
            m.effects.regenTurns = std::max(0, m.effects.regenTurns - 1);
            if (m.hp > 0 && m.hp < m.hpMax) {
                m.hp = std::min(m.hpMax, m.hp + 1);
            }

            if (m.effects.regenTurns == 0) {
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " STOPS REGENERATING.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            }
        }

        // Temporary shielding: just ticks down (damage reduction is applied in combat.cpp).
        if (m.effects.shieldTurns > 0) {
            m.effects.shieldTurns = std::max(0, m.effects.shieldTurns - 1);
            if (m.effects.shieldTurns == 0) {
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " LOOKS LESS PROTECTED.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            }
        }

        // Invisibility: keep monster timers sane even though rendering/AI treats invis mostly as
        // a player-stealth mechanic for now.
        if (m.effects.invisTurns > 0) {
            m.effects.invisTurns = std::max(0, m.effects.invisTurns - 1);
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

        // Timed fear: scared monsters prefer fleeing.
        if (m.effects.fearTurns > 0) {
            m.effects.fearTurns = std::max(0, m.effects.fearTurns - 1);
            if (m.effects.fearTurns == 0) {
                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " REGAINS ITS NERVE.";
                    pushMsg(ss.str(), MessageKind::System, false);
                }
            }
        }

        // Timed hallucination: currently does not affect monster AI, but decays for consistency.
        if (m.effects.hallucinationTurns > 0) {
            m.effects.hallucinationTurns = std::max(0, m.effects.hallucinationTurns - 1);
        }

        // Timed levitation (rare for monsters for now, but kept consistent with player rules).
        if (m.effects.levitationTurns > 0) {
            const int before = m.effects.levitationTurns;
            m.effects.levitationTurns = std::max(0, m.effects.levitationTurns - 1);
            if (before > 0 && m.effects.levitationTurns == 0) {
                const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;

                if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).type == TileType::Chasm) {
                    const int dmg = rng.range(4, 8) + std::min(4, depth_ / 2);
                    if (vis) {
                        std::ostringstream ss;
                        ss << kindName(m.kind) << " FALLS!";
                        pushMsg(ss.str(), MessageKind::Warning, false);
                    }

                    Vec2i landing = {-1, -1};
                    for (int r = 1; r <= 8 && landing.x < 0; ++r) {
                        std::vector<Vec2i> cand;
                        for (int dy = -r; dy <= r; ++dy) {
                            for (int dx = -r; dx <= r; ++dx) {
                                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                                const int x = m.pos.x + dx;
                                const int y = m.pos.y + dy;
                                if (!dung.inBounds(x, y)) continue;
                                if (!dung.isWalkable(x, y)) continue;
                                if (Entity* o = entityAtMut(x, y)) {
                                    if (o->id != m.id) continue;
                                }
                                cand.push_back({x, y});
                            }
                        }
                        if (!cand.empty()) {
                            landing = cand[static_cast<size_t>(rng.range(0, static_cast<int>(cand.size()) - 1))];
                        }
                    }

                    if (landing.x >= 0) {
                        m.pos = landing;
                    } else {
                        // Emergency fallback: collapse the chasm tile.
                        dung.at(m.pos.x, m.pos.y).type = TileType::Floor;
                    }

                    emitNoise(m.pos, 18);
                    m.hp -= dmg;
                    if (m.hp <= 0) {
                        if (vis) {
                            std::ostringstream ss;
                            ss << kindName(m.kind) << " DIES.";
                            pushMsg(ss.str(), MessageKind::Combat, false);
                        }
                    }
                } else {
                    if (vis) {
                        std::ostringstream ss;
                        ss << kindName(m.kind) << " SINKS TO THE GROUND.";
                        pushMsg(ss.str(), MessageKind::System, false);
                    }
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

            std::vector<uint8_t> next(n, uint8_t{0});
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
                    //
                    // Wind bias: downwind tiles get a slightly "stronger" spread, while upwind tiles
                    // dissipate a bit faster. This makes gas feel like it's drifting through corridors.
                    if (s >= 3u) {
                        const uint8_t baseSpread = static_cast<uint8_t>(s - 2u);
                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;

                            uint8_t spread = baseSpread;
                            if (windStr > 0) {
                                if (d.x == wind.x && d.y == wind.y) {
                                    int sp = static_cast<int>(baseSpread) + windStr;
                                    if (sp > static_cast<int>(s)) sp = static_cast<int>(s);
                                    spread = static_cast<uint8_t>(sp);
                                } else if (d.x == upWind.x && d.y == upWind.y) {
                                    int sp = static_cast<int>(baseSpread) - windStr;
                                    if (sp < 0) sp = 0;
                                    spread = static_cast<uint8_t>(sp);
                                }
                            }

                            if (spread == 0u) continue;
                            const size_t j = idx2(nx, ny);
                            if (next[j] < spread) next[j] = spread;
                        }
                    }
                }
            }

            confusionGas_.swap(next);
        }
    }


    // Update poison gas cloud diffusion/decay.
    // Similar to confusion gas, but we keep it slightly more localized.
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (expect > 0 && poisonGas_.size() != expect) {
            poisonGas_.assign(expect, 0u);
        }

        if (!poisonGas_.empty()) {
            const int w = dung.width;
            const int h = dung.height;
            const size_t n = static_cast<size_t>(w * h);

            std::vector<uint8_t> next(n, uint8_t{0});
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
                    const uint8_t s = poisonGas_[i];
                    if (s == 0u) continue;
                    if (!passable(x, y)) continue;

                    // Always decay in place.
                    const uint8_t self = (s > 0u) ? static_cast<uint8_t>(s - 1u) : 0u;
                    if (next[i] < self) next[i] = self;

                    // Spread to neighbors with extra decay (more dissipative than confusion gas).
                    //
                    // Wind bias: poison gas stays localized, but still drifts downwind in corridors.
                    if (s >= 4u) {
                        const uint8_t baseSpread = static_cast<uint8_t>(s - 3u);
                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;

                            uint8_t spread = baseSpread;
                            if (windStr > 0) {
                                // Slightly weaker than confusion gas so poison doesn't become too "flowy".
                                const int bonus = std::max(1, windStr - 1);
                                if (d.x == wind.x && d.y == wind.y) {
                                    int sp = static_cast<int>(baseSpread) + bonus;
                                    if (sp > static_cast<int>(s)) sp = static_cast<int>(s);
                                    spread = static_cast<uint8_t>(sp);
                                } else if (d.x == upWind.x && d.y == upWind.y) {
                                    int sp = static_cast<int>(baseSpread) - bonus;
                                    if (sp < 0) sp = 0;
                                    spread = static_cast<uint8_t>(sp);
                                }
                            }

                            if (spread == 0u) continue;
                            const size_t j = idx2(nx, ny);
                            if (next[j] < spread) next[j] = spread;
                        }
                    }
                }
            }

            poisonGas_.swap(next);
        }
    }

    // Update fire field decay/spread.
    // The fire field generally decays over time, with a small chance to spread when strong.
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (expect > 0 && fireField_.size() != expect) {
            fireField_.assign(expect, 0u);
        }

        if (!fireField_.empty()) {
            // Fire burns away any web traps it overlaps.
            int websBurnedSeen = 0;
            for (size_t ti = 0; ti < trapsCur.size(); ) {
                Trap& tr = trapsCur[ti];
                if (tr.kind == TrapKind::Web && dung.inBounds(tr.pos.x, tr.pos.y)) {
                    const size_t i = static_cast<size_t>(tr.pos.y * dung.width + tr.pos.x);
                    if (i < fireField_.size() && fireField_[i] > 0u) {
                        if (dung.at(tr.pos.x, tr.pos.y).visible) ++websBurnedSeen;
                        trapsCur.erase(trapsCur.begin() + static_cast<std::vector<Trap>::difference_type>(ti));
                        continue;
                    }
                }
                ++ti;
            }
            if (websBurnedSeen > 0) {
                pushMsg(websBurnedSeen == 1 ? "A WEB BURNS AWAY." : "WEBS BURN AWAY.", MessageKind::System, true);
            }

            const int w = dung.width;
            const int h = dung.height;
            const size_t n = static_cast<size_t>(w * h);

            std::vector<uint8_t> next(n, uint8_t{0});
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
            auto passable = [&](int x, int y) -> bool {
                if (!dung.inBounds(x, y)) return false;
                // Keep fire on walkable tiles (floors, open doors, stairs).
                return dung.isWalkable(x, y);
            };

            constexpr Vec2i kDirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    const uint8_t s = fireField_[i];
                    if (s == 0u) continue;
                    if (!passable(x, y)) continue;

                    // Always decay in place.
                    const uint8_t self = (s > 0u) ? static_cast<uint8_t>(s - 1u) : 0u;
                    if (next[i] < self) next[i] = self;

                    // Strong fires can spread a bit, but we keep this rare to avoid runaway map-wide burns.
                    if (s >= 8u) {
                        const float baseChance = std::min(0.12f, 0.02f * static_cast<float>(s - 7u));
                        const uint8_t spread = static_cast<uint8_t>(std::max(1, static_cast<int>(s) - 3));
                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;
                            const size_t j = idx2(nx, ny);
                            if (fireField_[j] != 0u) continue;

                            float chance = baseChance;
                            if (windStr > 0) {
                                // Downwind flames jump more readily; upwind spread is suppressed.
                                if (d.x == wind.x && d.y == wind.y) {
                                    chance *= (1.0f + 0.35f * static_cast<float>(windStr));
                                } else if (d.x == upWind.x && d.y == upWind.y) {
                                    chance *= std::max(0.20f, (1.0f - 0.25f * static_cast<float>(windStr)));
                                }
                            }
                            chance = std::min(0.35f, std::max(0.0f, chance));

                            if (rng.chance(chance)) {
                                if (next[j] < spread) next[j] = spread;
                            }
                        }
                    }
                }
            }

            fireField_.swap(next);
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

        // If an entity died off-map (e.g. fell through a trap door), don't drop loot/corpses here.
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;

        const int tier = procRankTier(e.procRank);
        const bool gilded = procHasAffix(e.procAffixMask, ProcMonsterAffix::Gilded);

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
                case EntityKind::Ghost:         ok = false; break;
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

        // Thief loot: drop any carried stolen gold (so the player can recover it).
        if (e.stolenGold > 0) {
            dropGroundItem(e.pos, ItemKind::Gold, e.stolenGold);
        }

        // Gilded affix: bonus gold drop (in addition to any stolen gold).
        if (gilded) {
            const int depthBonus = std::max(0, depth_ - 1);
            int bonus = rng.range(4, 10) + depthBonus * 2 + std::min(3, tier) * 4;
            bonus = std::max(1, bonus);
            dropGroundItem(e.pos, ItemKind::Gold, bonus);
        }

        // Pocket consumable: drop any remaining carried consumable so the player
        // can recover it.
        if (e.pocketConsumable.id != 0 && e.pocketConsumable.count > 0) {
            Item it = e.pocketConsumable;
            it.shopPrice = 0;
            it.shopDepth = 0;
            dropGroundItemItem(e.pos, it);
        }


        // Simple drops
        float dropChance = 0.55f;
        if (tier > 0) dropChance += 0.10f * static_cast<float>(std::min(3, tier));
        if (gilded) dropChance += 0.05f;
        dropChance = std::min(dropChance, 0.90f);

        if (rng.chance(dropChance)) {
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
                int pick = rng.range(0, 4);
                gi.item.kind = (pick == 0) ? ItemKind::ScrollIdentify
                                           : (pick == 1) ? ItemKind::ScrollDetectTraps
                                           : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                           : (pick == 3) ? ItemKind::ScrollKnock
                                                         : ItemKind::ScrollEnchantRing;
                gi.item.count = 1;
            }
            else if (roll < 108) { gi.item.kind = ItemKind::ScrollEnchantWeapon; gi.item.count = 1; }
            else if (roll < 111) { gi.item.kind = ItemKind::ScrollEnchantArmor; gi.item.count = 1; }
            else if (roll < 113) { gi.item.kind = ItemKind::ScrollRemoveCurse; gi.item.count = 1; }
            else if (roll < 114) { gi.item.kind = ItemKind::Dagger; gi.item.count = 1; }
            else if (roll < 115) { gi.item.kind = ItemKind::PotionShielding; gi.item.count = 1; }
            else if (roll < 116) { gi.item.kind = ItemKind::PotionHaste; gi.item.count = 1; }
            else {
                if (depth_ >= 3 && rng.chance(0.20f)) {
                    gi.item.kind = ItemKind::PotionLevitation;
                } else {
                    gi.item.kind = (rng.range(1, 4) == 1) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
                }
                gi.item.count = 1;
            }

            // Roll BUC (blessed/uncursed/cursed) for dropped gear.
            if (isWearableGear(gi.item.kind)) {
                gi.item.buc = rollBucForGear(rng, depth_, roomTypeAt(dung, gi.pos));
            }

            // Chance for dropped gear to be lightly enchanted on deeper floors.
            if (isWearableGear(gi.item.kind) && depth_ >= 3) {
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
            float keyChance = 0.07f + 0.03f * static_cast<float>(std::min(3, tier));
            if (gilded) keyChance += 0.03f;
            if (depth_ >= 10) keyChance += 0.02f;
            keyChance = std::min(keyChance, 0.25f);
            if (keyCarrier && rng.chance(keyChance)) {
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

void Game::spawnAltars() {
    if (branch_ == DungeonBranch::Camp) return;

    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    auto nearDoor = [&](Vec2i p) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int x = p.x + dx;
                const int y = p.y + dy;
                if (!dung.inBounds(x, y)) continue;
                const TileType tt = dung.at(x, y).type;
                if (tt == TileType::DoorClosed || tt == TileType::DoorOpen || tt == TileType::DoorLocked) return true;
            }
        }
        return false;
    };

    auto canPlace = [&](Vec2i p) {
        if (!dung.inBounds(p.x, p.y)) return false;
        if (p == dung.stairsUp || p == dung.stairsDown) return false;
        if (dung.at(p.x, p.y).type != TileType::Floor) return false;
        if (nearDoor(p)) return false;
        return true;
    };

    // One altar per shrine room, placed near the center so it reads clearly.
    for (const auto& r : rooms) {
        if (r.type != RoomType::Shrine) continue;

        Vec2i c{r.cx(), r.cy()};
        const std::array<Vec2i, 9> cand = {{
            c,
            {c.x - 1, c.y},
            {c.x + 1, c.y},
            {c.x, c.y - 1},
            {c.x, c.y + 1},
            {c.x - 1, c.y - 1},
            {c.x + 1, c.y - 1},
            {c.x - 1, c.y + 1},
            {c.x + 1, c.y + 1},
        }};

        for (const auto& p : cand) {
            if (!canPlace(p)) continue;
            dung.at(p.x, p.y).type = TileType::Altar;
            break;
        }
    }
}


void Game::spawnFountains() {
    if (branch_ == DungeonBranch::Camp) return;

    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    // Decide how many fountains to place.
    // Kept deliberately sparse: fountains are flavorful but can be risky.
    int want = 0;
    float p1 = 0.35f;
    if (depth_ >= 4) p1 = 0.45f;
    if (depth_ >= 8) p1 = 0.55f;
    if (depth_ >= 12) p1 = 0.60f;

    if (rng.chance(p1)) want = 1;
    if (depth_ >= 8 && rng.chance(0.20f)) want += 1;
    if (depth_ >= 14 && rng.chance(0.10f)) want += 1;

    want = clampi(want, 0, 3);
    if (want <= 0) return;

    auto hasTrapAt = [&](Vec2i p) {
        for (const auto& t : trapsCur) {
            if (t.pos == p) return true;
        }
        return false;
    };

    auto hasGroundItemAt = [&](Vec2i p) {
        for (const auto& gi : ground) {
            if (gi.pos == p) return true;
        }
        return false;
    };

    auto hasEngravingAt = [&](Vec2i p) {
        for (const auto& e : engravings_) {
            if (e.pos == p) return true;
        }
        return false;
    };

    auto nearDoor = [&](Vec2i p) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int x = p.x + dx;
                const int y = p.y + dy;
                if (!dung.inBounds(x, y)) continue;
                const TileType tt = dung.at(x, y).type;
                if (tt == TileType::DoorClosed || tt == TileType::DoorOpen || tt == TileType::DoorLocked || tt == TileType::DoorSecret) {
                    return true;
                }
            }
        }
        return false;
    };

    auto isBadPos = [&](Vec2i p) {
        if (!dung.inBounds(p.x, p.y)) return true;
        if (p == dung.stairsUp || p == dung.stairsDown) return true;

        // Avoid stair adjacency so the entrance/exit areas remain readable.
        if (chebyshev(p, dung.stairsUp) <= 2) return true;
        if (chebyshev(p, dung.stairsDown) <= 2) return true;

        // Only place fountains on plain floor tiles.
        if (dung.at(p.x, p.y).type != TileType::Floor) return true;

        // Don't overwrite other sparse systems.
        if (hasTrapAt(p)) return true;
        if (hasGroundItemAt(p)) return true;
        if (hasEngravingAt(p)) return true;
        if (entityAt(p.x, p.y) != nullptr) return true;

        // Keep doorways uncluttered.
        if (nearDoor(p)) return true;

        // Avoid shops: shops are meant to feel safe-ish and consistent.
        const RoomType rt = roomTypeAt(dung, p);
        if (rt == RoomType::Shop) return true;
        return false;
    };

    // Build a list of candidate rooms that have a usable interior.
    std::vector<int> candidates;
    candidates.reserve(rooms.size());

    for (size_t i = 0; i < rooms.size(); ++i) {
        const Room& r = rooms[i];
        if (r.type == RoomType::Shop || r.type == RoomType::Camp) continue;
        if (r.w < 4 || r.h < 4) continue;

        // Avoid very tiny vault/secret rooms where fountains feel like visual noise.
        if (r.type == RoomType::Vault || r.type == RoomType::Secret) continue;

        candidates.push_back(static_cast<int>(i));
    }

    if (candidates.empty()) return;

    int placed = 0;
    int tries = 0;
    const int maxTries = 120 + 80 * want;

    while (placed < want && tries < maxTries) {
        tries += 1;

        const int ri = candidates[rng.range(0, static_cast<int>(candidates.size()) - 1)];
        const Room& r = rooms[static_cast<size_t>(ri)];

        // Choose a random interior tile (avoid walls).
        const int x0 = r.x + 1;
        const int y0 = r.y + 1;
        const int x1 = r.x + r.w - 2;
        const int y1 = r.y + r.h - 2;
        if (x1 < x0 || y1 < y0) continue;

        Vec2i p{rng.range(x0, x1), rng.range(y0, y1)};
        if (isBadPos(p)) continue;

        dung.at(p.x, p.y).type = TileType::Fountain;
        placed += 1;
    }
}
