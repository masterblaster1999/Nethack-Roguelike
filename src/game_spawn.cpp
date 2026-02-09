#include "game_internal.hpp"
#include "content.hpp"
#include "craft_tags.hpp"
#include "ecosystem_loot.hpp"
#include "proc_rd.hpp"
#include "proc_spells.hpp"
#include "wards.hpp"
#include "shop_profile_gen.hpp"

namespace {

bool canHaveWeaponEgo(ItemKind k) {
    // Keep egos limited to the "core" melee weapons for now.
    // (Avoids branding tools like pickaxes, and keeps UI readable.)
    return k == ItemKind::Dagger || k == ItemKind::Sword || k == ItemKind::Axe;
}

ItemEgo rollWeaponEgo(RNG& rng, uint32_t runSeed, ItemKind k, int depth, RoomType rt, TerrainMaterial mat, EcosystemKind eco, bool fromShop, bool forMonster) {
    if (!canHaveWeaponEgo(k)) return ItemEgo::None;
    if (depth < 3) return ItemEgo::None;

    // Base chance grows gently with depth.
    float chance = 0.04f + 0.012f * static_cast<float>(std::min(10, std::max(0, depth - 3)));

    // Treasure-y rooms are more likely to contain branded gear.
    if (rt == RoomType::Treasure || rt == RoomType::Vault || rt == RoomType::Secret) chance += 0.06f;
    if (rt == RoomType::Armory || rt == RoomType::Shrine) chance += 0.03f;
    if (rt == RoomType::Lair) chance -= 0.03f;

    // Substrate nudges: volcanic stone tends to yield more branded weapons.
    if (mat == TerrainMaterial::Obsidian || mat == TerrainMaterial::Basalt) chance += 0.015f;
    if (mat == TerrainMaterial::Moss || mat == TerrainMaterial::Dirt) chance -= 0.008f;

    // Shops occasionally stock a premium item.
    if (fromShop) chance += 0.05f;

    // Monsters shouldn't carry too many premium weapons.
    if (forMonster) chance *= 0.60f;

    // Biome ecology: some ecosystems "forge" or "quench" branded weapons.
    // (Small multiplier; balance stays driven by depth/rooms.)
    chance *= ecoWeaponEgoChanceMul(eco);

    chance = std::max(0.0f, std::min(0.26f, chance));
    if (!rng.chance(chance)) return ItemEgo::None;

    // ---------------------------------------------------------------------
    // Ego ecology
    //
    // We don't just roll a flat distribution; we bias brands by:
    // - floor theme (deterministic by runSeed + depth)
    // - room type
    // - substrate material
    // This makes floors feel more coherent without making outcomes predictable.
    // ---------------------------------------------------------------------

    auto floorDominant = [&]() -> ItemEgo {
        // Pick a dominant ego per depth in a deterministic way.
        // (This doesn't consume RNG, so it stays stable under reorderings.)
        uint32_t h = hash32(runSeed ^ (static_cast<uint32_t>(depth) * 0x9E3779B1u) ^ 0xA11CE5EDu);

        struct Pair { ItemEgo e; int minDepth; };
        static constexpr Pair pool[] = {
            { ItemEgo::Flaming,   3 },
            { ItemEgo::Venom,     3 },
            { ItemEgo::Webbing,   3 },
            { ItemEgo::Corrosive, 4 },
            { ItemEgo::Dazing,    4 },
            { ItemEgo::Vampiric,  6 },
        };

        ItemEgo allowed[sizeof(pool) / sizeof(pool[0])]{};
        int n = 0;
        for (const auto& p : pool) {
            if (depth >= p.minDepth) {
                allowed[n++] = p.e;
            }
        }
        if (n <= 0) return ItemEgo::Flaming;
        const int idx = static_cast<int>(h % static_cast<uint32_t>(n));
        return allowed[idx];
    }();

    struct Opt { ItemEgo e; int w; };
    std::vector<Opt> opts;
    opts.reserve(6);

    auto addOpt = [&](ItemEgo e, int w, int minDepth) {
        if (depth < minDepth) return;
        if (w <= 0) return;
        opts.push_back(Opt{e, w});
    };

    // Baseline weights (before ecology biases).
    addOpt(ItemEgo::Flaming,   54, 3);
    addOpt(ItemEgo::Venom,     52, 3);
    addOpt(ItemEgo::Webbing,   28, 3);
    addOpt(ItemEgo::Corrosive, 24, 4);
    addOpt(ItemEgo::Dazing,    22, 4);
    addOpt(ItemEgo::Vampiric,  12, 6);

    // Depth progression: gently tilt toward rarer brands on deeper floors.
    const int deep = std::max(0, depth - 4);
    for (auto& o : opts) {
        if (o.e == ItemEgo::Vampiric) o.w += deep * 2;
        else if (o.e == ItemEgo::Corrosive || o.e == ItemEgo::Dazing) o.w += deep;
    }

    // Deterministic floor theme bias.
    for (auto& o : opts) {
        if (o.e == floorDominant) {
            o.w += std::max(2, o.w / 2);
        }
    }

    // Room type ecology.
    for (auto& o : opts) {
        switch (rt) {
            case RoomType::Laboratory:
                if (o.e == ItemEgo::Corrosive) o.w += 18;
                if (o.e == ItemEgo::Dazing) o.w += 10;
                break;
            case RoomType::Library:
                if (o.e == ItemEgo::Dazing) o.w += 14;
                break;
            case RoomType::Shrine:
                if (o.e == ItemEgo::Dazing) o.w += 10;
                // Deep shrines can lean dark.
                if (o.e == ItemEgo::Vampiric && depth >= 7) o.w += 6;
                break;
            case RoomType::Lair:
                if (o.e == ItemEgo::Webbing) o.w += 18;
                if (o.e == ItemEgo::Venom) o.w += 10;
                break;
            case RoomType::Armory:
                if (o.e == ItemEgo::Flaming) o.w += 12;
                break;
            case RoomType::Vault:
            case RoomType::Treasure:
            case RoomType::Secret:
                if (o.e == ItemEgo::Vampiric) o.w += 10;
                if (o.e == ItemEgo::Flaming) o.w += 6;
                break;
            default:
                break;
        }
    }

    // Substrate ecology.
    for (auto& o : opts) {
        switch (mat) {
            case TerrainMaterial::Obsidian:
            case TerrainMaterial::Basalt:
                if (o.e == ItemEgo::Flaming) o.w += 16;
                break;
            case TerrainMaterial::Moss:
            case TerrainMaterial::Dirt:
                if (o.e == ItemEgo::Venom) o.w += 14;
                if (o.e == ItemEgo::Webbing) o.w += 10;
                break;
            case TerrainMaterial::Bone:
                if (o.e == ItemEgo::Vampiric) o.w += 10;
                if (o.e == ItemEgo::Venom) o.w += 6;
                break;
            case TerrainMaterial::Metal:
                if (o.e == ItemEgo::Corrosive) o.w += 14;
                break;
            case TerrainMaterial::Crystal:
                if (o.e == ItemEgo::Dazing) o.w += 12;
                break;
            case TerrainMaterial::Marble:
            case TerrainMaterial::Brick:
                if (o.e == ItemEgo::Dazing) o.w += 6;
                break;
            case TerrainMaterial::Wood:
                if (o.e == ItemEgo::Webbing) o.w += 6;
                break;
            default:
                break;
        }
    }

    // Ecosystem ecology.
    // A final nudge based on the *local* biome region at the spawn position.
    // This stacks with room/substrate so (for example) a crystal floor shrine still
    // tends to yield DAZING gear, but the local ecosystem can tip edge cases.
    if (eco != EcosystemKind::None) {
        for (auto& o : opts) {
            o.w += ecoWeaponEgoWeightDelta(eco, o.e);
        }
    }

    // Shop bias: skew toward mid/rare brands (premium inventory).
    if (fromShop) {
        for (auto& o : opts) {
            if (o.e == ItemEgo::Corrosive || o.e == ItemEgo::Dazing) o.w += 8;
            if (o.e == ItemEgo::Vampiric) o.w += 4;
        }
    }

    // Monster bias: avoid too much hard-disable frustration.
    if (forMonster) {
        for (auto& o : opts) {
            if (o.e == ItemEgo::Webbing || o.e == ItemEgo::Dazing) {
                o.w = std::max(1, (o.w * 2) / 3);
            }
            if (o.e == ItemEgo::Vampiric) {
                o.w = std::max(1, (o.w * 3) / 4);
            }
        }
    }

    int total = 0;
    for (const auto& o : opts) total += std::max(0, o.w);
    if (total <= 0) return ItemEgo::None;

    int roll = rng.range(1, total);
    for (const auto& o : opts) {
        roll -= std::max(0, o.w);
        if (roll <= 0) return o.e;
    }

    return ItemEgo::None;
}

bool canBeArtifact(ItemKind k) {
    if (k == ItemKind::AmuletYendor) return false;
    if (isChestKind(k)) return false;
    if (!isWearableGear(k)) return false;

    // Keep artifacts focused on weapons/armor/rings for now.
    // (Avoids stacking with wand identification/charge mechanics.)
    if (isWandKind(k)) return false;

    return true;
}

bool rollArtifact(RNG& rng, ItemKind k, int depth, RoomType rt, bool fromShop, bool forMonster) {
    if (!canBeArtifact(k)) return false;
    if (depth < 3) return false;

    // Base chance ramps gently with depth.
    float chance = 0.006f + 0.004f * static_cast<float>(std::min(10, std::max(0, depth - 3)));

    // Treasure-y rooms are more likely to contain artifacts.
    if (rt == RoomType::Treasure || rt == RoomType::Vault || rt == RoomType::Secret) chance += 0.010f;
    if (rt == RoomType::Shrine) chance += 0.006f;
    if (rt == RoomType::Lair) chance -= 0.004f;

    // Shops and monsters should be stingier.
    if (fromShop) chance *= 0.35f;
    if (forMonster) chance *= 0.45f;

    chance = std::max(0.0f, std::min(0.035f, chance));
    return rng.chance(chance);
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

    // Aura affix: COMMANDER.
    // Leaders are more common among humanoids and organized foes.
    int wCommander = 0;
    if (humanoid) wCommander += 6;
    if (cunning) wCommander += 2;
    if (undead) wCommander += 1;
    if (k == EntityKind::Orc) wCommander += 4;
    if (k == EntityKind::Goblin) wCommander += 3;
    if (k == EntityKind::KoboldSlinger) wCommander += 3;
    if (k == EntityKind::Wizard) wCommander += 2;
    if (rt == RoomType::Vault || rt == RoomType::Treasure || rt == RoomType::Secret) wCommander += 2;
    if (rt == RoomType::Lair) wCommander = std::max(0, wCommander - 1);
    if (beast) wCommander = 0;

    // Humanoid-ish enemies are more likely to be gilded.
    if (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k)) wGold += 3;

    if (rt == RoomType::Vault || rt == RoomType::Treasure) wGold += 4;
    if (rt == RoomType::Lair) wSavage += 2;

    add(ProcMonsterAffix::Swift, wSwift);
    add(ProcMonsterAffix::Stonehide, wStone);
    add(ProcMonsterAffix::Savage, wSavage);
    add(ProcMonsterAffix::Blinking, wBlink);
    add(ProcMonsterAffix::Gilded, wGold);
    add(ProcMonsterAffix::Commander, wCommander);

    // Proc affixes that add on-hit status effects / sustain.
    add(ProcMonsterAffix::Venomous, wVenom);
    add(ProcMonsterAffix::Flaming, wFlame);
    add(ProcMonsterAffix::Vampiric, wVamp);
    add(ProcMonsterAffix::Webbing, wWeb);
}

static void bumpExistingProcAffix(std::vector<ProcAffixWeight>& pool, ProcMonsterAffix a, int delta) {
    if (delta == 0) return;
    for (auto& e : pool) {
        if (e.affix != a) continue;
        e.weight = std::max(0, e.weight + delta);
        return;
    }
}

static void applyEcosystemProcAffixBias(std::vector<ProcAffixWeight>& pool, EcosystemKind eco, EntityKind k, RoomType rt, int depth) {
    (void)rt;
    (void)depth;

    if (eco == EcosystemKind::None) return;

    const bool humanoid = (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k));
    const bool undead = entityIsUndead(k);

    switch (eco) {
        case EcosystemKind::FungalBloom:
            // Damp, toxic warrens: more venom/webs, less fire.
            bumpExistingProcAffix(pool, ProcMonsterAffix::Venomous, 4);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Webbing, 3);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Swift, 2);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Flaming, -3);
            break;

        case EcosystemKind::CrystalGarden:
            // Glittering growth: more blinking/stonehide/gilded.
            bumpExistingProcAffix(pool, ProcMonsterAffix::Blinking, 4);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Stonehide, 3);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Gilded, 2);
            break;

        case EcosystemKind::BoneField:
            // Ossuary pressure: vampiric + commanders (when it makes sense).
            if (!undead) bumpExistingProcAffix(pool, ProcMonsterAffix::Vampiric, 3);
            if (humanoid) bumpExistingProcAffix(pool, ProcMonsterAffix::Commander, 3);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Flaming, -2);
            break;

        case EcosystemKind::RustVeins:
            // Metal seams: stonehide + gilded nudges.
            bumpExistingProcAffix(pool, ProcMonsterAffix::Stonehide, 2);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Gilded, 2);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Venomous, -2);
            break;

        case EcosystemKind::AshenRidge:
            // Hot stone: more flaming + savage.
            bumpExistingProcAffix(pool, ProcMonsterAffix::Flaming, 5);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Savage, 2);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Venomous, -3);
            break;

        case EcosystemKind::FloodedGrotto:
            // Wet caves: toxic/webby and slightly faster.
            bumpExistingProcAffix(pool, ProcMonsterAffix::Venomous, 3);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Webbing, 2);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Swift, 1);
            bumpExistingProcAffix(pool, ProcMonsterAffix::Flaming, -2);
            break;

        default:
            break;
    }
}

static uint32_t rollProcAffixes(RNG& rr, EntityKind k, ProcMonsterRank rank, RoomType rt, int depth, EcosystemKind eco) {
    const int tier = procRankTier(rank);
    if (tier <= 0) return 0u;

    int want = (tier == 1) ? 1 : (tier == 2) ? 2 : 3;

    // Some early mythics roll only 2 affixes to keep spikes sane.
    if (rank == ProcMonsterRank::Mythic && depth < 12 && rr.chance(0.35f)) want = 2;

    std::vector<ProcAffixWeight> pool;
    buildProcAffixPool(pool, k, rt, depth);
    applyEcosystemProcAffixBias(pool, eco, k, rt, depth);

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

    // Void hook: reposition the player (brutes/humanoids).
    int wHook = 0;
    if (depth >= 4) {
        if (humanoid) wHook += 7;
        if (brute) wHook += 10;
        if (k == EntityKind::Ogre) wHook += 6;
        if (k == EntityKind::Orc) wHook += 3;
        if (caster) wHook += 2;
        if (rt == RoomType::Vault || rt == RoomType::Treasure) wHook += 3;
        if (procHasAffix(affixMask, ProcMonsterAffix::Savage)) wHook += 2;
        if (procHasAffix(affixMask, ProcMonsterAffix::Stonehide)) wHook += 1;
        if (undead) wHook = std::max(0, wHook - 2);
    }

    add(ProcMonsterAbility::Pounce, wPounce);
    add(ProcMonsterAbility::ToxicMiasma, wToxic);
    add(ProcMonsterAbility::CinderNova, wCinder);
    add(ProcMonsterAbility::ArcaneWard, wWard);
    add(ProcMonsterAbility::SummonMinions, wSummon);
    add(ProcMonsterAbility::Screech, wScreech);
    add(ProcMonsterAbility::VoidHook, wHook);
}

static void bumpExistingProcAbility(std::vector<ProcAbilityWeight>& pool, ProcMonsterAbility a, int delta) {
    if (a == ProcMonsterAbility::None) return;
    if (delta == 0) return;
    for (auto& e : pool) {
        if (e.ability != a) continue;
        e.weight = std::max(0, e.weight + delta);
        return;
    }
}

static void applyEcosystemProcAbilityBias(std::vector<ProcAbilityWeight>& pool, EcosystemKind eco, EntityKind k, RoomType rt, int depth, uint32_t affixMask) {
    (void)rt;
    (void)depth;
    (void)affixMask;

    if (eco == EcosystemKind::None) return;

    const bool undead = entityIsUndead(k);
    const bool humanoid = (monsterCanEquipWeapons(k) || monsterCanEquipArmor(k));

    switch (eco) {
        case EcosystemKind::FungalBloom:
            bumpExistingProcAbility(pool, ProcMonsterAbility::ToxicMiasma, 7);
            bumpExistingProcAbility(pool, ProcMonsterAbility::Pounce, 3);
            bumpExistingProcAbility(pool, ProcMonsterAbility::CinderNova, -4);
            break;

        case EcosystemKind::CrystalGarden:
            bumpExistingProcAbility(pool, ProcMonsterAbility::ArcaneWard, 6);
            bumpExistingProcAbility(pool, ProcMonsterAbility::VoidHook, 3);
            bumpExistingProcAbility(pool, ProcMonsterAbility::Screech, 2);
            break;

        case EcosystemKind::BoneField:
            // Encourage summoning on casters/undead-ish foes.
            if (undead || humanoid) bumpExistingProcAbility(pool, ProcMonsterAbility::SummonMinions, 6);
            bumpExistingProcAbility(pool, ProcMonsterAbility::Screech, 3);
            bumpExistingProcAbility(pool, ProcMonsterAbility::ToxicMiasma, -3);
            break;

        case EcosystemKind::RustVeins:
            bumpExistingProcAbility(pool, ProcMonsterAbility::VoidHook, 4);
            bumpExistingProcAbility(pool, ProcMonsterAbility::ArcaneWard, 2);
            bumpExistingProcAbility(pool, ProcMonsterAbility::Pounce, -2);
            break;

        case EcosystemKind::AshenRidge:
            bumpExistingProcAbility(pool, ProcMonsterAbility::CinderNova, 7);
            bumpExistingProcAbility(pool, ProcMonsterAbility::VoidHook, 2);
            bumpExistingProcAbility(pool, ProcMonsterAbility::ToxicMiasma, -4);
            break;

        case EcosystemKind::FloodedGrotto:
            bumpExistingProcAbility(pool, ProcMonsterAbility::ToxicMiasma, 5);
            bumpExistingProcAbility(pool, ProcMonsterAbility::Pounce, 2);
            bumpExistingProcAbility(pool, ProcMonsterAbility::CinderNova, -3);
            break;

        default:
            break;
    }
}

static void rollProcAbilities(RNG& rr, EntityKind k, ProcMonsterRank rank, RoomType rt, int depth, uint32_t affixMask, EcosystemKind eco,
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
    applyEcosystemProcAbilityBias(pool, eco, k, rt, depth, affixMask);
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
        // In normal dungeon generation rooms do not overlap, but overworld chunks
        // intentionally include a large catch-all room for spawn logic.
        // If we introduce special sub-rooms inside that space (shops, shrines, etc.),
        // spawns for the large room should not bleed into those safe zones.
        if (roomTypeAt(dung, Vec2i{x, y}) != r.type) return false;
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
        const TerrainMaterial matHere = dung.materialAtCached(e.pos.x, e.pos.y);
        const EcosystemKind ecoHere = dung.ecosystemAtCached(e.pos.x, e.pos.y);

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
            it.ego = rollWeaponEgo(rng, seed_, kind, depth_, rt, matHere, ecoHere, /*fromShop=*/false, /*forMonster=*/true);

            // Rare artifacts on monster gear.
            if (rollArtifact(rng, kind, depth_, rt, /*fromShop=*/false, /*forMonster=*/true)) {
                setItemArtifact(it, true);
                // Keep artifacts visually distinct from ego gear.
                it.ego = ItemEgo::None;
                // Artifacts tend to be at least +1.
                it.enchant = std::max(it.enchant, 1);
                if (depth_ >= 7 && rng.chance(0.30f)) it.enchant = std::max(it.enchant, 2);
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

    // Torch carriers: some humanoid-ish monsters may spawn with a spare torch on
    // dark floors. This makes darkness less binary: the player can play around
    // pockets of light that *move*.
    //
    // We intentionally keep this separate from the Wizard pocket potion logic
    // (single pocket slot) and only assign a torch when the slot is empty.
    if (allowGear && darknessActive() && e.pocketConsumable.id == 0) {
        auto makePocketTorch = [&](bool lit) -> Item {
            Item it;
            it.id = 1; // non-zero => present
            it.kind = lit ? ItemKind::TorchLit : ItemKind::Torch;
            it.count = 1;
            it.spriteSeed = rng.nextU32();
            it.shopPrice = 0;
            it.shopDepth = 0;
            // Torches carried by monsters are always uncursed.
            it.buc = 0;
            it.enchant = 0;
            it.ego = ItemEgo::None;

            if (lit) {
                int fuel = 160 + rng.range(0, 140);
                // Guards tend to have higher quality torches.
                if (k == EntityKind::Guard) fuel += 40;
                it.charges = fuel;
            }
            return it;
        };

        float chance = 0.0f;
        switch (k) {
            case EntityKind::Goblin:
                chance = 0.10f + 0.02f * static_cast<float>(std::min(6, depth_));
                break;
            case EntityKind::Orc:
                chance = 0.18f + 0.03f * static_cast<float>(std::min(6, depth_));
                break;
            case EntityKind::Guard:
                chance = 0.45f;
                break;
            case EntityKind::Shopkeeper:
                // Shopkeepers generally stay in lit shops, but if they chase you into a
                // corridor, a torch prevents them from being trivially kited in darkness.
                chance = 0.35f;
                break;
            case EntityKind::KoboldSlinger:
                chance = 0.12f + 0.02f * static_cast<float>(std::min(6, depth_));
                break;
            default:
                break;
        }

        // Avoid handing out too much free mobile light in already-lit rooms.
        if (rtHere == RoomType::Shop || rtHere == RoomType::Shrine || rtHere == RoomType::Library) {
            chance *= 0.35f;
        }

        if (chance > 0.0f && rng.chance(std::clamp(chance, 0.0f, 0.75f))) {
            const bool startLit = rng.chance((k == EntityKind::Guard) ? 0.65f : 0.40f);
            e.pocketConsumable = makePocketTorch(startLit);
        }
    }


    // Procedural monster variants (rank + affixes + abilities).
    // Applied after baseline stats/gear so modifiers scale the final creature.
    if (allowProcVariant && branch_ == DungeonBranch::Main && procVariantEligible(k, rtHere, depth_)) {
        // Consult the deterministic ecosystem field so proc variants differ subtly
        // between biome patches on the same floor.
        const EcosystemKind ecoHere = dung.ecosystemAt(e.pos.x, e.pos.y, materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());

        const uint32_t seed = hashCombine(e.spriteSeed ^ 0xC0FFEEu,
                                          hashCombine(static_cast<uint32_t>(k),
                                                      hashCombine(static_cast<uint32_t>(depth_),
                                                                  hashCombine(static_cast<uint32_t>(rtHere),
                                                                              static_cast<uint32_t>(ecoHere)))));
        RNG prng(seed);
        const ProcMonsterRank pr = rollProcRank(prng, k, depth_, rtHere);
        const uint32_t pm = rollProcAffixes(prng, k, pr, rtHere, depth_, ecoHere);
        applyProcVariant(e, pr, pm);

        // Roll a small active-ability kit for ranked monsters.
        rollProcAbilities(prng, k, pr, rtHere, depth_, pm, ecoHere, e.procAbility1, e.procAbility2);
        e.procAbility1Cd = 0;
        e.procAbility2Cd = 0;
    }

    // Lifecycle + character traits.
    if (lifecycleEligibleKind(e.kind)) {
        e.lifeStage = LifeStage::Adult;
        e.lifeSex = lifecycleRollSex(e.spriteSeed, e.kind);
        e.lifeTraitMask = lifecycleRollTraitMask(e.spriteSeed, e.kind);

        if (lifeHasTrait(e.lifeTraitMask, LifeTrait::Hardy)) {
            const int oldMax = std::max(1, e.hpMax);
            e.hpMax = std::max(oldMax + 1, (oldMax * 125 + 99) / 100);
            e.hp += (e.hpMax - oldMax);
        }
        if (lifeHasTrait(e.lifeTraitMask, LifeTrait::Fierce)) {
            e.baseAtk += 1;
        }
        if (lifeHasTrait(e.lifeTraitMask, LifeTrait::Tough)) {
            e.baseDef += 1;
        }
        if (lifeHasTrait(e.lifeTraitMask, LifeTrait::Swift)) {
            e.speed += 12;
        }

        e.hpMax = std::max(1, e.hpMax);
        e.hp = std::clamp(e.hp, 1, e.hpMax);
        e.speed = std::max(40, e.speed);

        // Adult baseline snapshot used for later stage scaling.
        e.lifeBaseHpMax = e.hpMax;
        e.lifeBaseAtk = e.baseAtk;
        e.lifeBaseDef = e.baseDef;
        e.lifeBaseSpeed = e.speed;

        const int matureAge = lifecycleStageDurationTurns(LifeStage::Newborn)
                            + lifecycleStageDurationTurns(LifeStage::Child);
        const uint32_t h = hash32(hashCombine(e.spriteSeed ^ 0xA61ED00Du,
                                              static_cast<uint32_t>(std::max(0, depth_))));
        e.lifeAgeTurns = matureAge + static_cast<int>(h % 241u);
        e.lifeStageTurns = static_cast<int>((h >> 8) % 240u);
        e.lifeReproductionCooldown = static_cast<int>(
            (h >> 16) % static_cast<uint32_t>(lifecycleReproductionCooldownTurns(e.lifeTraitMask) + 1));
        e.lifeBirthCount = 0;
    } else {
        e.lifeStage = LifeStage::Adult;
        e.lifeSex = LifeSex::Unknown;
        e.lifeTraitMask = 0u;
        e.lifeAgeTurns = 0;
        e.lifeStageTurns = 0;
        e.lifeReproductionCooldown = 0;
        e.lifeBirthCount = 0;
        e.lifeBaseHpMax = std::max(1, e.hpMax);
        e.lifeBaseAtk = e.baseAtk;
        e.lifeBaseDef = e.baseDef;
        e.lifeBaseSpeed = std::max(1, e.speed);
    }

    return e;
}

Entity& Game::spawnMonster(EntityKind k, Vec2i pos, int groupId, bool allowGear) {
    ents.push_back(makeMonster(k, pos, groupId, allowGear));
    return ents.back();
}

namespace {

inline void mulWeight(std::vector<SpawnEntry>& table, EntityKind k, int num, int den = 1) {
    if (num <= 0 || den <= 0) return;
    for (auto& e : table) {
        if (e.kind != k) continue;
        if (e.weight <= 0) return;
        const int w = e.weight;
        e.weight = std::max(1, (w * num + den / 2) / den);
        return;
    }
}

inline EntityKind rollFromTable(RNG& rng, const std::vector<SpawnEntry>& table) {
    int total = 0;
    for (const auto& e : table) {
        if (e.weight > 0) total += e.weight;
    }
    if (total <= 0) return EntityKind::Goblin;

    int roll = rng.range(0, total - 1);
    for (const auto& e : table) {
        if (e.weight <= 0) continue;
        roll -= e.weight;
        if (roll < 0) return e.kind;
    }
    return table.empty() ? EntityKind::Goblin : table.back().kind;
}

inline void applyRoomBias(std::vector<SpawnEntry>& table, SpawnCategory category, RoomType rt, int depth) {
    (void)depth;

    switch (rt) {
        case RoomType::Shrine: {
            mulWeight(table, EntityKind::Ghost, 3, 2);
            mulWeight(table, EntityKind::Zombie, 3, 2);
            mulWeight(table, EntityKind::SkeletonArcher, 3, 2);
            if (category == SpawnCategory::Guardian) {
                mulWeight(table, EntityKind::Guard, 3, 2);
            }
        } break;
        case RoomType::Library: {
            mulWeight(table, EntityKind::Wizard, 3, 2);
            mulWeight(table, EntityKind::SkeletonArcher, 3, 2);
            mulWeight(table, EntityKind::Bat, 3, 2);
        } break;
        case RoomType::Laboratory: {
            mulWeight(table, EntityKind::Slime, 2, 1);
            mulWeight(table, EntityKind::Wizard, 3, 2);
            mulWeight(table, EntityKind::Spider, 3, 2);
        } break;
        case RoomType::Armory: {
            mulWeight(table, EntityKind::Orc, 3, 2);
            mulWeight(table, EntityKind::Ogre, 3, 2);
            if (category == SpawnCategory::Guardian) {
                mulWeight(table, EntityKind::Guard, 2, 1);
            }
        } break;
        case RoomType::Vault:
        case RoomType::Treasure:
        case RoomType::Secret: {
            mulWeight(table, EntityKind::Mimic, 2, 1);
            mulWeight(table, EntityKind::Leprechaun, 3, 2);
            mulWeight(table, EntityKind::Nymph, 3, 2);
            if (category == SpawnCategory::Guardian) {
                mulWeight(table, EntityKind::Guard, 2, 1);
                mulWeight(table, EntityKind::Ogre, 3, 2);
            }
        } break;
        case RoomType::Lair: {
            mulWeight(table, EntityKind::Wolf, 2, 1);
            mulWeight(table, EntityKind::Spider, 3, 2);
        } break;
        default:
            break;
    }
}

inline void applyMaterialBias(std::vector<SpawnEntry>& table, SpawnCategory category, TerrainMaterial mat, int depth) {
    // Mild, deterministic ecology: materials subtly bias spawns without overriding spawn-table mods.
    // Deeper floors get slightly stronger biases.
    const int d = std::clamp(depth, 1, 12);
    const bool deep = (d >= 7);

    auto bump15 = [&](EntityKind k) { mulWeight(table, k, 3, 2); };
    auto bump20 = [&](EntityKind k) { mulWeight(table, k, 2, 1); };

    switch (mat) {
        case TerrainMaterial::Dirt: {
            bump15(EntityKind::Snake);
            bump20(EntityKind::Spider);
            bump15(EntityKind::Wolf);
        } break;
        case TerrainMaterial::Moss: {
            bump20(EntityKind::Slime);
            bump15(EntityKind::Bat);
            bump15(EntityKind::Spider);
        } break;
        case TerrainMaterial::Crystal: {
            bump20(EntityKind::Slime);
            bump15(EntityKind::Wizard);
            bump15(EntityKind::Mimic);
            bump15(EntityKind::Nymph);
        } break;
        case TerrainMaterial::Marble: {
            bump20(EntityKind::SkeletonArcher);
            bump15(EntityKind::Zombie);
            bump15(EntityKind::Ghost);
            if (category == SpawnCategory::Guardian) {
                bump15(EntityKind::Guard);
            }
        } break;
        case TerrainMaterial::Brick: {
            bump15(EntityKind::Orc);
            bump15(EntityKind::SkeletonArcher);
            bump15(EntityKind::Zombie);
            if (category == SpawnCategory::Guardian) {
                bump15(EntityKind::Guard);
            }
        } break;
        case TerrainMaterial::Basalt:
        case TerrainMaterial::Obsidian: {
            bump15(EntityKind::Orc);
            bump15(EntityKind::Troll);
            bump15(EntityKind::Ogre);
            if (deep) bump15(EntityKind::Wizard);
        } break;
        case TerrainMaterial::Metal: {
            bump20(EntityKind::KoboldSlinger);
            bump15(EntityKind::Mimic);
            if (category == SpawnCategory::Guardian) {
                bump20(EntityKind::Guard);
            }
        } break;
        case TerrainMaterial::Stone:
        default:
            break;
    }
}



struct EcoCtx {
    EcosystemKind here = EcosystemKind::None;
    EcosystemKind other = EcosystemKind::None; // dominant neighbor ecosystem (ecotone)
    int diversity = 0;                         // distinct non-None ecosystems in {here + cardinal neighbors}
    bool ecotone = false;                      // true when diversity >= 2 and other != None
};

inline EcoCtx ecoCtxAt(const Dungeon& d, int x, int y) {
    EcoCtx out;
    out.here = d.ecosystemAtCached(x, y);
    if (out.here == EcosystemKind::None) return out;

    EcosystemKind ns[4] = {EcosystemKind::None, EcosystemKind::None, EcosystemKind::None, EcosystemKind::None};
    int n = 0;

    const Vec2i dirs[4] = {{1,0}, {-1,0}, {0,1}, {0,-1}};
    for (const auto& dd : dirs) {
        const int nx = x + dd.x;
        const int ny = y + dd.y;
        if (!d.inBounds(nx, ny)) continue;
        if (d.at(nx, ny).type != TileType::Floor) continue;
        const EcosystemKind e = d.ecosystemAtCached(nx, ny);
        if (e == EcosystemKind::None) continue;
        if (n < 4) ns[n++] = e;
    }

    // Distinct ecosystem count (ignore None).
    EcosystemKind uniq[5] = {EcosystemKind::None, EcosystemKind::None, EcosystemKind::None, EcosystemKind::None, EcosystemKind::None};
    int uniqN = 0;
    uniq[uniqN++] = out.here;

    for (int i = 0; i < n; ++i) {
        const EcosystemKind e = ns[i];
        bool seen = false;
        for (int j = 0; j < uniqN; ++j) {
            if (uniq[j] == e) { seen = true; break; }
        }
        if (!seen && uniqN < 5) {
            uniq[uniqN++] = e;
        }
    }

    out.diversity = uniqN;
    if (uniqN < 2) return out;

    // Choose the most frequent "other" ecosystem among neighbors.
    EcosystemKind other = EcosystemKind::None;
    int bestCount = 0;
    for (int j = 0; j < uniqN; ++j) {
        const EcosystemKind e = uniq[j];
        if (e == out.here) continue;
        int c = 0;
        for (int i = 0; i < n; ++i) {
            if (ns[i] == e) ++c;
        }
        if (c > bestCount) {
            bestCount = c;
            other = e;
        }
    }

    out.other = other;
    out.ecotone = (other != EcosystemKind::None);
    return out;
}

inline void applyEcosystemBias(std::vector<SpawnEntry>& table, SpawnCategory category, EcosystemKind eco, int depth, bool weak) {
    if (eco == EcosystemKind::None) return;

    // Mild, deterministic ecology: ecosystems subtly bias spawns without overriding spawn-table mods.
    // Ecotones can apply a second, weaker pass for the neighboring ecosystem.
    const int d = std::clamp(depth, 1, 12);
    const bool deep = (d >= 7);

    // Strength knobs.
    // strong: 2.0x (or 1.5x when weak)
    // mid:    1.5x (or 1.33x when weak)
    // damp:   0.66x (or 0.83x when weak)
    const int strongNum = weak ? 3 : 2;
    const int strongDen = weak ? 2 : 1;

    const int midNum = weak ? 4 : 3;
    const int midDen = weak ? 3 : 2;

    const int dampNum = weak ? 5 : 2;
    const int dampDen = weak ? 6 : 3;

    auto bumpStrong = [&](EntityKind k) { mulWeight(table, k, strongNum, strongDen); };
    auto bumpMid = [&](EntityKind k) { mulWeight(table, k, midNum, midDen); };
    auto damp = [&](EntityKind k) { mulWeight(table, k, dampNum, dampDen); };

    switch (eco) {
        case EcosystemKind::FungalBloom: {
            bumpStrong(EntityKind::Spider);
            bumpStrong(EntityKind::Slime);
            bumpMid(EntityKind::Snake);
            bumpMid(EntityKind::Bat);

            damp(EntityKind::SkeletonArcher);
            damp(EntityKind::Zombie);
            damp(EntityKind::Ghost);
        } break;

        case EcosystemKind::CrystalGarden: {
            bumpStrong(EntityKind::Wizard);
            bumpStrong(EntityKind::Mimic);
            bumpMid(EntityKind::Nymph);
            bumpMid(EntityKind::Slime);

            damp(EntityKind::Snake);
            damp(EntityKind::Spider);

            if (category == SpawnCategory::Guardian && deep) {
                // Deep crystal halls lean toward "constructed" resistance.
                bumpMid(EntityKind::Guard);
            }
        } break;

        case EcosystemKind::BoneField: {
            bumpStrong(EntityKind::SkeletonArcher);
            bumpStrong(EntityKind::Zombie);
            bumpMid(EntityKind::Ghost);

            damp(EntityKind::Slime);
            damp(EntityKind::Spider);
            damp(EntityKind::Snake);

            if (category == SpawnCategory::Guardian) {
                // Tomb-adjacent security.
                bumpMid(EntityKind::Guard);
            }
        } break;

        case EcosystemKind::RustVeins: {
            bumpStrong(EntityKind::KoboldSlinger);
            bumpStrong(EntityKind::Mimic);
            bumpMid(EntityKind::Orc);

            damp(EntityKind::Slime);
            damp(EntityKind::Bat);

            if (category == SpawnCategory::Guardian) {
                bumpMid(EntityKind::Guard);
            }
        } break;

        case EcosystemKind::AshenRidge: {
            bumpStrong(EntityKind::Orc);
            bumpStrong(EntityKind::Troll);
            bumpMid(EntityKind::Ogre);
            if (deep) bumpMid(EntityKind::Wizard);

            damp(EntityKind::Bat);
            damp(EntityKind::Slime);
        } break;

        case EcosystemKind::FloodedGrotto: {
            bumpStrong(EntityKind::Bat);
            bumpStrong(EntityKind::Slime);
            bumpMid(EntityKind::Snake);

            damp(EntityKind::Orc);
            damp(EntityKind::Ogre);
        } break;

        case EcosystemKind::None:
        default:
            break;
    }
}

inline void applyEcotoneBias(std::vector<SpawnEntry>& table, SpawnCategory /*category*/, const EcoCtx& ctx, int depth) {
    if (!ctx.ecotone) return;

    const int d = std::clamp(depth, 1, 12);
    const bool chaotic = (ctx.diversity >= 3);

    auto bump = [&](EntityKind k) {
        if (chaotic) mulWeight(table, k, 2, 1);
        else mulWeight(table, k, 3, 2);
    };

    // Boundaries are liminal: tricksters + weirdness.
    bump(EntityKind::Mimic);
    bump(EntityKind::Leprechaun);
    bump(EntityKind::Nymph);

    if (d >= 4) {
        // Wizards show up more often once depth introduces mid-tier magic threats.
        if (chaotic) mulWeight(table, EntityKind::Wizard, 3, 2);
        else mulWeight(table, EntityKind::Wizard, 4, 3);
    }

    // Undead bleed through bonefield borders.
    if (ctx.here == EcosystemKind::BoneField || ctx.other == EcosystemKind::BoneField) {
        mulWeight(table, EntityKind::Ghost, 3, 2);
        mulWeight(table, EntityKind::Zombie, 4, 3);
    }
}

inline EntityKind pickSpawnMonsterEcology(SpawnCategory category, RNG& rng, int depth, RoomType rt, TerrainMaterial mat, const EcoCtx& eco) {
    std::vector<SpawnEntry> table = effectiveSpawnTable(category, depth);

    applyRoomBias(table, category, rt, depth);
    applyMaterialBias(table, category, mat, depth);

    // Ecosystem bias: strong toward the local biome. If the tile is an ecotone,
    // blend in a weaker pass from the neighboring biome and add liminal "weirdness".
    applyEcosystemBias(table, category, eco.here, depth, /*weak=*/false);
    if (eco.ecotone && eco.other != EcosystemKind::None && eco.other != eco.here) {
        applyEcosystemBias(table, category, eco.other, depth, /*weak=*/true);
        applyEcotoneBias(table, category, eco, depth);
    }

    // Synergy nudges: shrines in dressed stone feel more "haunted".
    if (rt == RoomType::Shrine && (mat == TerrainMaterial::Marble || mat == TerrainMaterial::Brick)) {
        mulWeight(table, EntityKind::Ghost, 2, 1);
        mulWeight(table, EntityKind::Zombie, 3, 2);
    }

    return rollFromTable(rng, table);
}


} // namespace

void Game::spawnMonsters() {
    if (atHomeCamp()) return;

    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    int nextGroup = 1000;

    // Spawn ecology consults the deterministic terrain-material field.
    dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());

    // Use a depth-like scalar for the overworld (Camp/0 wilderness chunks).
    const int spawnDepth = materialDepth();

    // Find a nearby free tile inside the room interior (keeps clusters feeling like nests).
    auto freeTileNearInRoom = [&](Vec2i center, const Room& room, int radius) -> Vec2i {
        std::vector<Vec2i> candidates;
        candidates.reserve(static_cast<size_t>((radius * 2 + 1) * (radius * 2 + 1)));

        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int x = center.x + dx;
                const int y = center.y + dy;

                // Stay in the room *interior* so we don't place spawns inside walls.
                if (x <= room.x || y <= room.y || x >= room.x2() - 1 || y >= room.y2() - 1) continue;
                if (!dung.inBounds(x, y)) continue;
                if (!dung.isWalkable(x, y)) continue;
                if (entityAt(x, y)) continue;

                candidates.push_back({x, y});
            }
        }

        if (candidates.empty()) return {-1, -1};
        return candidates[static_cast<size_t>(rng.range(0, static_cast<int>(candidates.size()) - 1))];
    };
    auto maybeSpawnEcologyCluster = [&](Entity& leader, EntityKind kind, const Room& room, TerrainMaterial mat, EcosystemKind eco, bool isStartRoom) {
        // Keep the start room calmer so the player isn't immediately nested by a pack.
        if (isStartRoom) return;

        int extra = 0;
        float chance = 0.0f;

        auto isMat = [&](TerrainMaterial a, TerrainMaterial b) { return mat == a || mat == b; };

        switch (kind) {
            case EntityKind::Spider: {
                if (isMat(TerrainMaterial::Dirt, TerrainMaterial::Moss)) {
                    chance = 0.38f;
                    if (eco == EcosystemKind::FungalBloom) chance += 0.10f;
                    if (eco == EcosystemKind::FloodedGrotto) chance += 0.05f;
                    if (eco == EcosystemKind::BoneField) chance -= 0.06f;
                    chance = std::clamp(chance, 0.0f, 0.60f);

                    extra = rng.chance(chance) ? 1 : 0;
                    if (spawnDepth >= 6 && rng.chance(0.12f)) extra += 1;
                }
            } break;
            case EntityKind::Snake: {
                if (mat == TerrainMaterial::Dirt) {
                    chance = 0.28f;
                    if (eco == EcosystemKind::FungalBloom) chance += 0.06f;
                    if (eco == EcosystemKind::FloodedGrotto) chance += 0.05f;
                    if (eco == EcosystemKind::CrystalGarden) chance -= 0.04f;
                    chance = std::clamp(chance, 0.0f, 0.50f);

                    extra = rng.chance(chance) ? 1 : 0;
                }
            } break;
            case EntityKind::Slime: {
                if (isMat(TerrainMaterial::Moss, TerrainMaterial::Crystal)) {
                    chance = 0.22f;
                    if (eco == EcosystemKind::FungalBloom) chance += 0.05f;
                    if (eco == EcosystemKind::CrystalGarden) chance += 0.08f;
                    if (eco == EcosystemKind::FloodedGrotto) chance += 0.06f;
                    if (eco == EcosystemKind::AshenRidge) chance -= 0.05f;
                    chance = std::clamp(chance, 0.0f, 0.55f);

                    extra = rng.chance(chance) ? 1 : 0;
                }
            } break;
            case EntityKind::Bat: {
                if (isMat(TerrainMaterial::Moss, TerrainMaterial::Stone) && spawnDepth >= 2) {
                    chance = 0.18f;
                    if (eco == EcosystemKind::FloodedGrotto) chance += 0.12f;
                    if (eco == EcosystemKind::FungalBloom) chance += 0.04f;
                    if (eco == EcosystemKind::BoneField) chance -= 0.05f;
                    chance = std::clamp(chance, 0.0f, 0.45f);

                    extra = rng.chance(chance) ? 1 : 0;
                }
            } break;
            case EntityKind::Zombie: {
                // Bone-field clusters: shambling packs near ossuaries.
                if (eco == EcosystemKind::BoneField || isMat(TerrainMaterial::Marble, TerrainMaterial::Brick)) {
                    chance = 0.16f;
                    if (eco == EcosystemKind::BoneField) chance += 0.10f;
                    if (spawnDepth >= 7) chance += 0.04f;
                    chance = std::clamp(chance, 0.0f, 0.40f);

                    extra = rng.chance(chance) ? 1 : 0;
                    if (spawnDepth >= 9 && rng.chance(0.10f)) extra += 1;
                }
            } break;
            default:
                break;
        }

        if (extra <= 0) return;

        // Give the cluster a shared groupId so one wake-up can alert nearby nestmates.
        const int gid = nextGroup++;
        leader.groupId = gid;

        for (int i = 0; i < extra; ++i) {
            Vec2i q = freeTileNearInRoom(leader.pos, room, 3);
            if (!dung.inBounds(q.x, q.y) || !dung.isWalkable(q.x, q.y) || entityAt(q.x, q.y)) {
                // Fallback: any free interior tile in the room.
                q = randomFreeTileInRoom(room);
            }

            if (!dung.inBounds(q.x, q.y) || !dung.isWalkable(q.x, q.y) || entityAt(q.x, q.y)) break;
            spawnMonster(kind, q, gid);
        }
    };


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

            // Procedural shop identity: tie the shopkeeper's spriteSeed to the room
            // so their look stays stable even if other RNG consumers shift.
            {
                const shopgen::ShopProfile prof = shopgen::profileFor(seed_, spawnDepth, r);
                sk.spriteSeed = hashCombine(prof.seed, "SK"_tag);
            }

            sk.alerted = false;
            sk.energy = 0;
            continue;
        }

        const bool isStart = r.contains(dung.stairsUp.x, dung.stairsUp.y);
        int base = isStart ? 0 : 1;

        int depthTerm = (spawnDepth >= 3 ? 2 : 1);
        if (spawnDepth >= 7) depthTerm += 1;
        if (spawnDepth >= 9) depthTerm += 1;

        int n = rng.range(0, base + depthTerm);
        if (r.type == RoomType::Vault) n = rng.range(0, 1);

        for (int i = 0; i < n; ++i) {
            Vec2i p = randomFreeTileInRoom(r);
            const TerrainMaterial mat = dung.materialAtCached(p.x, p.y);
            const EcoCtx eco = ecoCtxAt(dung, p.x, p.y);
            EntityKind k = pickSpawnMonsterEcology(SpawnCategory::Room, rng, spawnDepth, r.type, mat, eco);

            if (k == EntityKind::Wolf) {
                spawnMonster(k, p, nextGroup++);
            } else {
                Entity& m0 = spawnMonster(k, p, 0);
                maybeSpawnEcologyCluster(m0, k, r, mat, eco.here, isStart);
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
                const TerrainMaterial mat = dung.materialAtCached(p.x, p.y);
                const EcoCtx eco = ecoCtxAt(dung, p.x, p.y);
                EntityKind k = pickSpawnMonsterEcology(SpawnCategory::Guardian, rng, spawnDepth, r.type, mat, eco);

                spawnMonster(k, p, 0);
            }

            // Thieves love rooms with loot. (Themed rooms are a bit less enticing.)
            if (spawnDepth >= 2) {
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


    // ---------------------------------------------------------------------
    // Ecosystem guardians: rare proc-elite packs spawned near biome cores.
    //
    // These are *additional* encounters that make biome cores feel like places
    // with an apex predator / champion cult, and provide a consistent source of
    // ecosystem-aligned Essence Shards (see cleanupDead()).
    //
    // RNG-isolated: uses a derived seed so it doesn't perturb other spawns.
    // ---------------------------------------------------------------------
    if (branch_ == DungeonBranch::Main && spawnDepth >= 2 && depth_ < QUEST_DEPTH - 1) {
        const auto& ecoSeeds = dung.ecosystemSeedsCached();
        if (!ecoSeeds.empty()) {
            struct Cand { size_t idx; int w; };
            std::vector<Cand> cands;
            cands.reserve(ecoSeeds.size());

            for (size_t i = 0; i < ecoSeeds.size(); ++i) {
                const EcosystemSeed& s = ecoSeeds[i];
                if (s.kind == EcosystemKind::None) continue;
                if (s.radius < 5) continue; // tiny specks don't get guardians

                int w = 8 + std::min(24, s.radius);
                switch (s.kind) {
                    case EcosystemKind::CrystalGarden: w += 3; break;
                    case EcosystemKind::BoneField:     w += 2; break;
                    case EcosystemKind::AshenRidge:    w += 2; break;
                    default: break;
                }
                cands.push_back({i, w});
            }

            if (!cands.empty()) {
                uint32_t baseSeed = hashCombine(seed_, "ECO_GUARDIANS"_tag);
                baseSeed = hashCombine(baseSeed, static_cast<uint32_t>(branch_));
                baseSeed = hashCombine(baseSeed, static_cast<uint32_t>(spawnDepth));
                baseSeed = hashCombine(baseSeed, static_cast<uint32_t>(depth_));

                RNG grng(baseSeed);

                int budget = 0;
                float chance = 0.16f + 0.035f * static_cast<float>(std::min(12, spawnDepth));
                chance = std::min(0.72f, std::max(0.0f, chance));
                if (grng.chance(chance)) budget = 1;
                if (spawnDepth >= 9 && grng.chance(0.25f)) budget += 1;
                budget = clampi(budget, 0, 2);
                budget = std::min(budget, static_cast<int>(cands.size()));

                auto pickWeighted = [&](RNG& rr, std::vector<Cand>& pool) -> size_t {
                    int total = 0;
                    for (const auto& c : pool) total += std::max(0, c.w);
                    if (total <= 0) {
                        const size_t j = static_cast<size_t>(rr.range(0, static_cast<int>(pool.size()) - 1));
                        const size_t idx = pool[j].idx;
                        pool.erase(pool.begin() + static_cast<std::vector<Cand>::difference_type>(j));
                        return idx;
                    }

                    int roll = rr.range(1, total);
                    for (size_t j = 0; j < pool.size(); ++j) {
                        roll -= std::max(0, pool[j].w);
                        if (roll <= 0) {
                            const size_t idx = pool[j].idx;
                            pool.erase(pool.begin() + static_cast<std::vector<Cand>::difference_type>(j));
                            return idx;
                        }
                    }

                    const size_t idx = pool.back().idx;
                    pool.pop_back();
                    return idx;
                };

                auto hasTrapAt = [&](Vec2i p) -> bool {
                    for (const auto& t : trapsCur) {
                        if (t.pos == p) return true;
                    }
                    return false;
                };
                auto hasEngravingAt = [&](Vec2i p) -> bool {
                    for (const auto& eg : engravings_) {
                        if (eg.pos == p) return true;
                    }
                    return false;
                };

                auto isBadGuardianPos = [&](Vec2i p, EcosystemKind wantEco) -> bool {
                    if (!dung.inBounds(p.x, p.y)) return true;
                    if (!dung.isWalkable(p.x, p.y)) return true;
                    if (entityAt(p.x, p.y) != nullptr) return true;

                    const TileType tt = dung.at(p.x, p.y).type;
                    if (tt == TileType::DoorClosed || tt == TileType::DoorLocked) return true;
                    if (tt == TileType::Fountain || tt == TileType::Altar) return true;

                    if (p == dung.stairsUp || p == dung.stairsDown) return true;
                    if (dung.inBounds(dung.stairsUp.x, dung.stairsUp.y) && manhattan(p, dung.stairsUp) <= 5) return true;
                    if (dung.inBounds(dung.stairsDown.x, dung.stairsDown.y) && manhattan(p, dung.stairsDown) <= 4) return true;

                    if (dung.ecosystemAtCached(p.x, p.y) != wantEco) return true;

                    const RoomType rt = roomTypeAt(dung, p);
                    if (rt == RoomType::Shop || rt == RoomType::Camp) return true;
                    // Avoid high-value rooms so loot rooms don't become pure death traps.
                    if (rt == RoomType::Vault || rt == RoomType::Treasure || rt == RoomType::Secret) return true;

                    // Avoid stacking with other sparse systems.
                    if (hasTrapAt(p)) return true;
                    if (hasEngravingAt(p)) return true;

                    return false;
                };

                auto findEcoCorePos = [&](RNG& rr, const EcosystemSeed& s) -> Vec2i {
                    const int r0 = clampi(s.radius, 6, 18);

                    auto sample = [&](int r) -> Vec2i {
                        const int r2 = r * r;
                        for (int it = 0; it < 90; ++it) {
                            int dx = rr.range(-r, r);
                            int dy = rr.range(-r, r);
                            if (dx * dx + dy * dy > r2) continue;
                            Vec2i p{s.pos.x + dx, s.pos.y + dy};
                            if (isBadGuardianPos(p, s.kind)) continue;
                            return p;
                        }
                        return Vec2i{-1, -1};
                    };

                    // Prefer the inner core first, then expand.
                    Vec2i p = sample(std::max(4, r0 / 2));
                    if (dung.inBounds(p.x, p.y)) return p;
                    p = sample(r0);
                    if (dung.inBounds(p.x, p.y)) return p;

                    // Fallback scan: brute spiral-ish search around the seed center.
                    for (int rad = 2; rad <= r0 + 6; ++rad) {
                        const int rad2 = rad * rad;
                        for (int dy = -rad; dy <= rad; ++dy) {
                            for (int dx = -rad; dx <= rad; ++dx) {
                                if (dx * dx + dy * dy > rad2) continue;
                                Vec2i q{s.pos.x + dx, s.pos.y + dy};
                                if (isBadGuardianPos(q, s.kind)) continue;
                                return q;
                            }
                        }
                    }

                    return Vec2i{-1, -1};
                };

                auto pickGuardianKind = [&](RNG& rr, EcosystemKind eco) -> EntityKind {
                    struct Opt { EntityKind k; int w; int minDepth; };
                    std::vector<Opt> opts;

                    auto add = [&](EntityKind k, int w, int minD = 0) {
                        if (w <= 0) return;
                        opts.push_back({k, w, minD});
                    };

                    switch (eco) {
                        case EcosystemKind::FungalBloom:
                            add(EntityKind::Spider, 10, 0);
                            add(EntityKind::Slime, 6, 0);
                            add(EntityKind::Snake, 4, 0);
                            add(EntityKind::Troll, 2, 6);
                            break;
                        case EcosystemKind::CrystalGarden:
                            add(EntityKind::Mimic, 8, 0);
                            add(EntityKind::Wizard, 6, 3);
                            add(EntityKind::Slime, 5, 0);
                            add(EntityKind::Nymph, 4, 2);
                            break;
                        case EcosystemKind::BoneField:
                            add(EntityKind::SkeletonArcher, 8, 0);
                            add(EntityKind::Zombie, 7, 0);
                            add(EntityKind::Ghost, 5, 4);
                            add(EntityKind::Ogre, 2, 7);
                            break;
                        case EcosystemKind::RustVeins:
                            add(EntityKind::KoboldSlinger, 9, 0);
                            add(EntityKind::Mimic, 6, 0);
                            add(EntityKind::Orc, 5, 2);
                            add(EntityKind::Guard, 2, 6);
                            break;
                        case EcosystemKind::AshenRidge:
                            add(EntityKind::Orc, 8, 0);
                            add(EntityKind::Ogre, 6, 4);
                            add(EntityKind::Troll, 5, 5);
                            add(EntityKind::Wizard, 2, 8);
                            break;
                        case EcosystemKind::FloodedGrotto:
                            add(EntityKind::Slime, 10, 0);
                            add(EntityKind::Snake, 6, 0);
                            add(EntityKind::Spider, 4, 1);
                            add(EntityKind::Bat, 4, 1);
                            break;
                        default:
                            break;
                    }

                    int total = 0;
                    for (const auto& o : opts) {
                        if (spawnDepth < o.minDepth) continue;
                        total += std::max(0, o.w);
                    }
                    if (total <= 0) return EntityKind::Goblin;

                    int roll = rr.range(1, total);
                    for (const auto& o : opts) {
                        if (spawnDepth < o.minDepth) continue;
                        roll -= std::max(0, o.w);
                        if (roll <= 0) return o.k;
                    }
                    return opts.back().k;
                };

                auto pickMinionKind = [&](RNG& rr, EcosystemKind eco, EntityKind leaderKind) -> EntityKind {
                    // Bias strongly toward the leader kind, but allow some variety.
                    if (rr.chance(0.70f)) return leaderKind;

                    switch (eco) {
                        case EcosystemKind::FungalBloom:   return rr.chance(0.50f) ? EntityKind::Spider : EntityKind::Slime;
                        case EcosystemKind::CrystalGarden: return rr.chance(0.55f) ? EntityKind::Slime : EntityKind::Mimic;
                        case EcosystemKind::BoneField:     return rr.chance(0.50f) ? EntityKind::Zombie : EntityKind::SkeletonArcher;
                        case EcosystemKind::RustVeins:     return rr.chance(0.60f) ? EntityKind::KoboldSlinger : EntityKind::Orc;
                        case EcosystemKind::AshenRidge:    return rr.chance(0.55f) ? EntityKind::Orc : EntityKind::Troll;
                        case EcosystemKind::FloodedGrotto: return rr.chance(0.55f) ? EntityKind::Slime : EntityKind::Snake;
                        default:
                            break;
                    }
                    return leaderKind;
                };

                auto findNear = [&](RNG& rr, Vec2i center, const EcosystemSeed& s, int radius) -> Vec2i {
                    for (int it = 0; it < 70; ++it) {
                        const int dx = rr.range(-radius, radius);
                        const int dy = rr.range(-radius, radius);
                        if (dx == 0 && dy == 0) continue;
                        Vec2i p{center.x + dx, center.y + dy};
                        if (isBadGuardianPos(p, s.kind)) continue;
                        return p;
                    }
                    return Vec2i{-1, -1};
                };

                for (int gi = 0; gi < budget; ++gi) {
                    const size_t idx = pickWeighted(grng, cands);
                    if (idx >= ecoSeeds.size()) continue;

                    const EcosystemSeed& s = ecoSeeds[idx];

                    uint32_t localSeed = hashCombine(baseSeed, static_cast<uint32_t>(idx));
                    localSeed = hashCombine(localSeed, static_cast<uint32_t>(s.pos.x));
                    localSeed = hashCombine(localSeed, static_cast<uint32_t>(s.pos.y));
                    localSeed = hashCombine(localSeed, static_cast<uint32_t>(s.kind));
                    RNG rr(localSeed);

                    const Vec2i p = findEcoCorePos(rr, s);
                    if (!dung.inBounds(p.x, p.y)) continue;

                    const RoomType rtHere = roomTypeAt(dung, p);

                    // Decide base kind and proc rank.
                    const EntityKind baseKind = pickGuardianKind(rr, s.kind);

                    ProcMonsterRank rank = ProcMonsterRank::Elite;
                    if (spawnDepth >= 6) rank = ProcMonsterRank::Champion;
                    if (spawnDepth >= 10 && rr.chance(0.65f)) rank = ProcMonsterRank::Mythic;

                    // Guardians are rare and should never be trivially weak.
                    if (spawnDepth >= 12 && rr.chance(0.20f)) rank = ProcMonsterRank::Mythic;

                    // Shared group id for the pack.
                    const int gid = nextGroup++;

                    // Spawn leader with RNG-isolated sprite seed and no gear rolls.
                    const uint32_t leaderSpriteSeed = hashCombine(localSeed, "ECO_GUARD_LEADER"_tag);
                    ents.push_back(makeMonster(baseKind, p, gid, /*allowGear=*/false,
                                               /*forcedSpriteSeed=*/leaderSpriteSeed, /*allowProcVariant=*/false));
                    Entity& leader = ents.back();

                    // Apply a themed proc kit (rank + affixes + abilities), biased by ecosystem.
                    {
                        RNG prng(hashCombine(localSeed, "ECO_GUARD_PROC"_tag));
                        uint32_t aff = rollProcAffixes(prng, baseKind, rank, rtHere, spawnDepth, s.kind);

                        // Guarantee some reward / identity.
                        aff |= procAffixBit(ProcMonsterAffix::Gilded);

                        // Encourage a signature affix so different biomes feel distinct.
                        switch (s.kind) {
                            case EcosystemKind::FungalBloom:   aff |= procAffixBit(prng.chance(0.60f) ? ProcMonsterAffix::Venomous : ProcMonsterAffix::Webbing); break;
                            case EcosystemKind::CrystalGarden: aff |= procAffixBit(prng.chance(0.55f) ? ProcMonsterAffix::Blinking : ProcMonsterAffix::Stonehide); break;
                            case EcosystemKind::BoneField:     aff |= procAffixBit(prng.chance(0.55f) ? ProcMonsterAffix::Commander : ProcMonsterAffix::Vampiric); break;
                            case EcosystemKind::RustVeins:     aff |= procAffixBit(prng.chance(0.60f) ? ProcMonsterAffix::Stonehide : ProcMonsterAffix::Swift); break;
                            case EcosystemKind::AshenRidge:    aff |= procAffixBit(prng.chance(0.70f) ? ProcMonsterAffix::Flaming : ProcMonsterAffix::Savage); break;
                            case EcosystemKind::FloodedGrotto: aff |= procAffixBit(prng.chance(0.60f) ? ProcMonsterAffix::Venomous : ProcMonsterAffix::Swift); break;
                            default:
                                break;
                        }

                        applyProcVariant(leader, rank, aff);

                        rollProcAbilities(prng, baseKind, leader.procRank, rtHere, spawnDepth, leader.procAffixMask, s.kind,
                                          leader.procAbility1, leader.procAbility2);
                    }

                    // Guardians shouldn't flee; they are the biome's apex.
                    leader.willFlee = false;

                    // Spawn a small escort pack.
                    const int tier = std::max(1, procRankTier(leader.procRank));
                    int minions = 1 + tier; // 2..4
                    if (spawnDepth <= 3) minions = std::min(minions, 2);
                    if (spawnDepth >= 12) minions = std::min(4, minions + 1);
                    minions = clampi(minions, 2, 4);

                    for (int mi = 0; mi < minions; ++mi) {
                        Vec2i q = findNear(rr, leader.pos, s, 4);
                        if (!dung.inBounds(q.x, q.y)) break;

                        const EntityKind mk = pickMinionKind(rr, s.kind, baseKind);
                        const uint32_t ms = hashCombine(localSeed, hashCombine("ECO_GUARD_MINION"_tag, static_cast<uint32_t>(mi + 1)));
                        ents.push_back(makeMonster(mk, q, gid, /*allowGear=*/false,
                                                   /*forcedSpriteSeed=*/ms, /*allowProcVariant=*/false));
                    }
                }
            }
        }
    }
}

void Game::spawnItems() {
    if (atHomeCamp()) return;

    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    // Use a depth-like scalar for the overworld (Camp/0 wilderness chunks).
    const int spawnDepth = materialDepth();

    // Spawn item ecology consults the deterministic terrain-material field (ego rolls, etc).
    dung.ensureMaterials(materialWorldSeed(), branch_, spawnDepth, dungeonMaxDepth());

    auto dropItemAtWithRng = [&](RNG& rr, ItemKind k, Vec2i pos, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rr.nextU32();
        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

        // Procedural rune tablets: spriteSeed encodes a packed proc spell id (tier + seed),
        // not a purely cosmetic variation seed.
        if (k == ItemKind::RuneTablet) {
            const RoomType rt = roomTypeAt(dung, pos);

            // Tier loosely tracks depth with small room-based adjustments.
            int tier = 1 + spawnDepth / 2;
            if (rt == RoomType::Treasure || rt == RoomType::Vault || rt == RoomType::Shrine) tier += 1;
            if (rt == RoomType::Shop) tier = std::max(1, tier - 1);

            // A small depth-based chance to bump tier upward so deep tablets feel spicy.
            if (spawnDepth >= 6 && rr.chance(0.18f)) tier += 1;

            tier = clampi(tier, 1, 15);
            const uint32_t seed28 = rr.nextU32() & PROC_SPELL_SEED_MASK;
            it.spriteSeed = makeProcSpellId(static_cast<uint8_t>(tier), seed28);
        }

        // Roll BUC (blessed/uncursed/cursed) for gear; and light enchant chance on deeper floors.
        if (isWearableGear(k)) {
            const RoomType rt = roomTypeAt(dung, pos);
            it.buc = rollBucForGear(rr, spawnDepth, rt);

            if (it.enchant == 0 && spawnDepth >= 3) {
                float enchChance = 0.15f;
                if (rt == RoomType::Treasure || rt == RoomType::Vault || rt == RoomType::Secret) enchChance += 0.10f;
                if (rt == RoomType::Lair) enchChance -= 0.05f;
                enchChance = std::max(0.05f, std::min(0.35f, enchChance));

                if (rr.chance(enchChance)) {
                    it.enchant = 1;
                    if (spawnDepth >= 6 && rr.chance(0.08f)) {
                        it.enchant = 2;
                    }
                }
            }

            // Rare ego weapons (brands).
            it.ego = rollWeaponEgo(rr, seed_, k, spawnDepth, rt, dung.materialAtCached(pos.x, pos.y), dung.ecosystemAtCached(pos.x, pos.y), /*fromShop=*/false, /*forMonster=*/false);

            // Rare artifacts.
            if (rollArtifact(rr, k, spawnDepth, rt, /*fromShop=*/false, /*forMonster=*/false)) {
                setItemArtifact(it, true);
                // Keep artifacts visually distinct from ego gear.
                it.ego = ItemEgo::None;
                // Artifacts tend to be at least +1.
                it.enchant = std::max(it.enchant, 1);
                if (spawnDepth >= 7 && rr.chance(0.30f)) it.enchant = std::max(it.enchant, 2);
            }
        }

        GroundItem gi;
        gi.item = it;
        gi.pos = pos;
        ground.push_back(gi);
    };

    auto dropItemAt = [&](ItemKind k, Vec2i pos, int count = 1) {
        dropItemAtWithRng(rng, k, pos, count);
    };
;

    auto dropShopItemAt = [&](const shopgen::ShopProfile& prof, ItemKind k, Vec2i pos, int count = 1) {
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

        // Procedural rune tablets: shops can stock tablets too.
        if (k == ItemKind::RuneTablet) {
            int tier = 1 + spawnDepth / 2;

            // Magic shops tend to have slightly better rune stock deeper down.
            if (prof.theme == shopgen::ShopTheme::Magic) {
                if (spawnDepth >= 4 && rng.chance(0.25f)) tier += 1;
                if (spawnDepth >= 7 && rng.chance(0.12f)) tier += 1;
            } else {
                // Off-theme shops still get the occasional spicy tablet, but less often.
                if (spawnDepth >= 6 && rng.chance(0.12f)) tier += 1;
            }

            tier = clampi(tier, 1, 15);
            const uint32_t seed28 = rng.nextU32() & PROC_SPELL_SEED_MASK;
            it.spriteSeed = makeProcSpellId(static_cast<uint8_t>(tier), seed28);
        }

        // Shops sell mostly "clean" gear.
        RoomType rt = RoomType::Shop;
        if (isWearableGear(k)) {
            it.buc = rollBucForGear(rng, spawnDepth, rt);
            // A slightly higher chance of +1 items compared to the floor.
            float enchChance = (spawnDepth >= 2) ? 0.22f : 0.12f;
            enchChance += std::min(0.18f, spawnDepth * 0.02f);
            if (rng.chance(enchChance)) {
                it.enchant = 1;
                if (spawnDepth >= 6 && rng.chance(0.08f)) it.enchant = 2;
            }

            // Rare premium ego weapons.
            it.ego = rollWeaponEgo(rng, seed_, k, spawnDepth, rt, dung.materialAtCached(pos.x, pos.y), dung.ecosystemAtCached(pos.x, pos.y), /*fromShop=*/true, /*forMonster=*/false);


            // Extremely rare artifacts in shops.
            if (rollArtifact(rng, k, spawnDepth, rt, /*fromShop=*/true, /*forMonster=*/false)) {
                setItemArtifact(it, true);
                // Keep artifacts visually distinct from ego gear.
                it.ego = ItemEgo::None;
                // Artifacts tend to be at least +1.
                it.enchant = std::max(it.enchant, 1);
                if (spawnDepth >= 7 && rng.chance(0.25f)) it.enchant = std::max(it.enchant, 2);
            }
        }

        const int basePrice = shopBuyPricePerUnit(it, spawnDepth);
        it.shopPrice = shopgen::adjustedShopBuyPricePerUnit(basePrice, prof, it);
        it.shopDepth = spawnDepth;

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
            ItemKind wk = (spawnDepth >= 5) ? ItemKind::WandFireball : ItemKind::WandSparks;
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
        else if (roll < 166) {
            // Rare treasure-room find: capture spheres.
            // Kept relatively uncommon here; magic shops are the primary source.
            if (rng.chance(0.60f)) {
                ItemKind sp = ItemKind::CaptureSphere;
                if (spawnDepth >= 6 && rng.chance(0.40f)) sp = ItemKind::MegaSphere;
                dropItemAt(sp, randomFreeTileInRoom(r), rng.range(1, 2));
            } else {
                dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
            }
        }
        else if (roll < 172) {
            // Rare traversal utility in treasure rooms.
            if (spawnDepth >= 3 && rng.chance(0.33f)) {
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
        else if (roll < 190) dropItemAt(ItemKind::RuneTablet, randomFreeTileInRoom(r), 1);
        else if (roll < 194) dropItemAt(ItemKind::PotionEnergy, randomFreeTileInRoom(r), 1);
        else {
            // Rare: a spellbook (or occasionally a collectible VTuber merch drop).
            // Cards are a bit more common than figurines.
            if (rng.chance(0.12f)) {
                dropItemAt(ItemKind::VtuberFigurine, randomFreeTileInRoom(r), 1);
            } else if (rng.chance(0.22f)) {
                dropItemAt(ItemKind::VtuberHoloCard, randomFreeTileInRoom(r), 1);
            } else {
                ItemKind bk = (spawnDepth >= 2) ? pickSpellbookKind(rng, spawnDepth) : ItemKind::ScrollIdentify;
                dropItemAt(bk, randomFreeTileInRoom(r), 1);
            }
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
        if (spawnDepth >= 4) {
            if (r < 84) return TrapKind::ConfusionGas;
            if (r < 91) return TrapKind::PoisonGas;
            if (spawnDepth >= 6 && r < 95) return TrapKind::CorrosiveGas;
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
        if (spawnDepth >= 2) {
            float mimicChance = 0.04f + 0.01f * static_cast<float>(std::min(6, spawnDepth - 2));
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
            dropItemAt(ItemKind::Gold, p, rng.range(25, 55) + spawnDepth * 4);
            dropChestInRoom(r, 2, 0.75f, 0.55f);
            if (spawnDepth >= 4 && rng.chance(0.25f)) {
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

            // Procedurally generated shop profile (stable per room/run).
            const shopgen::ShopProfile prof = shopgen::profileFor(seed_, spawnDepth, r);
            // 0=General, 1=Armory, 2=Magic, 3=Supplies
            const int theme = static_cast<int>(prof.theme);

            // Anchor item so every shop feels useful.
            if (theme == 2) {
                dropShopItemAt(prof, ItemKind::ScrollIdentify, randomEmptyTileInRoom(r), 1);
            } else {
                dropShopItemAt(prof, ItemKind::PotionHealing, randomEmptyTileInRoom(r), 1);
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
                    else { k = (spawnDepth >= 6 ? ItemKind::PlateArmor : ItemKind::ChainArmor); }
                } else if (theme == 2) {
                    // Magic shop (wands/scrolls/potions + spellbooks + rune tablets)
                    // NOTE: Keep this table self-contained (0..99) so every outcome is reachable.
                    if (roll < 6) { k = ItemKind::RuneTablet; }
                    else if (roll < 14) { k = pickSpellbookKind(rng, spawnDepth); }
                    else if (roll < 26) { k = ItemKind::WandSparks; }
                    else if (roll < 36) { k = ItemKind::WandDigging; }
                    else if (roll < 40) { k = (spawnDepth >= 6 ? ItemKind::WandFireball : ItemKind::WandDigging); }
                    else if (roll < 48) { k = ItemKind::ScrollTeleport; }
                    else if (roll < 58) { k = ItemKind::ScrollMapping; }
                    else if (roll < 70) { k = ItemKind::ScrollIdentify; }
                    else if (roll < 76) { k = ItemKind::ScrollRemoveCurse; }
                    else if (roll < 82) { k = ItemKind::ScrollFear; }
                    else if (roll < 86) { k = ItemKind::ScrollEarth; }
                    else if (roll < 88) { k = ItemKind::ScrollTaming; }
                    else if (roll < 92) {
                        // Capture spheres: staple item for monster collecting.
                        k = (spawnDepth >= 6 && rng.chance(0.25f)) ? ItemKind::MegaSphere : ItemKind::CaptureSphere;
                        count = rng.range(1, 3);
                    }
                    else if (roll < 94) { k = ItemKind::PotionStrength; }
                    else if (roll < 96) { k = ItemKind::PotionRegeneration; }
                    else if (roll < 97) { k = ItemKind::PotionHaste; }
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
                        } else if (spawnDepth >= 3 && rng.chance(0.25f)) {
                            k = ItemKind::PotionLevitation;
                        } else {
                            k = (spawnDepth >= 5 ? ItemKind::PotionInvisibility : ItemKind::PotionVision);
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
                if (k == ItemKind::LeatherArmor && spawnDepth >= 4 && rng.chance(0.12f)) k = ItemKind::ChainArmor;
                if (k == ItemKind::ChainArmor && spawnDepth >= 7 && rng.chance(0.06f)) k = ItemKind::PlateArmor;

                dropShopItemAt(prof, k, randomEmptyTileInRoom(r), count);
            }
            continue;
        }

        if (r.type == RoomType::Secret) {
            // Secret rooms are optional bonus finds; keep them rewarding but not as
            // rich as full treasure rooms.
            dropItemAt(ItemKind::Gold, p, rng.range(8, 22) + spawnDepth);
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
            dropItemAt(ItemKind::Gold, p, rng.range(15, 40) + spawnDepth * 3);
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
            if (spawnDepth >= 2 && rng.chance(0.10f)) dropItemAt(ItemKind::RuneTablet, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.50f)) dropItemAt(ItemKind::Gold, randomFreeTileInRoom(r), rng.range(6, 18));
            continue;
        }

        if (r.type == RoomType::Lair) {
            if (rng.chance(0.50f)) dropItemAt(ItemKind::Rock, p, rng.range(3, 9));
            if (rng.chance(0.10f)) dropKeyAt(randomFreeTileInRoom(r), 1);
            if (rng.chance(0.12f)) dropLockpickAt(randomFreeTileInRoom(r), 1);
            if (rng.chance(hungerEnabled_ ? 0.25f : 0.10f)) dropItemAt(ItemKind::FoodRation, randomFreeTileInRoom(r), 1);
            if (spawnDepth >= 2 && rng.chance(0.20f)) dropItemAt(ItemKind::Sling, randomFreeTileInRoom(r), 1);
            continue;
        }

        if (r.type == RoomType::Armory) {
            // A moderate gear cache: some weapons/armor/ammo. Less "spicy" than Treasure.
            dropItemAt(ItemKind::Gold, p, rng.range(6, 16) + spawnDepth);

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
                    if (spawnDepth >= 4 && rng.chance(0.40f)) ak = ItemKind::ChainArmor;
                    if (spawnDepth >= 7 && rng.chance(0.18f)) ak = ItemKind::PlateArmor;
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
            dropItemAt(ItemKind::Gold, p, rng.range(4, 14) + spawnDepth);

            const int drops = rng.range(2, 4);
            for (int i = 0; i < drops; ++i) {
                // Occasionally a spellbook shows up (more likely on deeper floors).
                const float bookChance = std::min(0.24f, 0.06f + 0.02f * static_cast<float>(std::max(0, spawnDepth - 2)));
                if (spawnDepth >= 2 && rng.chance(bookChance)) {
                    dropItemAt(pickSpellbookKind(rng, spawnDepth), randomFreeTileInRoom(r), 1);
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
                    if (spawnDepth >= 4 && rng.chance(0.35f)) wk = ItemKind::WandDigging;
                    if (spawnDepth >= 7 && rng.chance(0.10f)) wk = ItemKind::WandFireball;
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
            dropItemAt(ItemKind::Gold, p, rng.range(4, 14) + spawnDepth);

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
                    if (spawnDepth >= 4 && rng.chance(0.30f)) wk = ItemKind::WandDigging;
                    if (spawnDepth >= 8 && rng.chance(0.10f)) wk = ItemKind::WandFireball;
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

    // Procgen may request specific guaranteed ground items (e.g. a key inside a keyed
    // vault prefab, or a utility drop in a dead-end stash closet).
    // Apply them after the generic per-room rolls so we can avoid collisions.
    for (const BonusItemSpawn& req : dung.bonusItemSpawns) {
        const Vec2i pos = req.pos;
        if (!dung.inBounds(pos.x, pos.y)) continue;
        if (dung.at(pos.x, pos.y).type != TileType::Floor) continue;
        if (entityAt(pos.x, pos.y)) continue;
        if (hasGroundAt(pos)) continue;

        const int cnt = std::max(1, req.count);
        if (req.kind == ItemKind::Key) {
            dropKeyAt(pos, cnt);
        } else if (req.kind == ItemKind::Lockpick) {
            dropLockpickAt(pos, cnt);
        } else {
            dropItemAt(req.kind, pos, cnt);
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
    if (branch_ == DungeonBranch::Main && spawnDepth == QUEST_DEPTH && !playerHasAmulet()) {
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
        int tier = (spawnDepth <= 2) ? 1 : ((spawnDepth <= 5) ? 2 : 3);
        if (spawnDepth >= 6 && rng.chance(0.35f)) tier = 4;
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
    if (spawnDepth >= 2) {
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
            if (isStationaryPropKind(it.kind) || itemIsStationary(it)) continue;
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
            w += std::min(30, spawnDepth * 2);
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
        float p1 = 0.10f + 0.02f * static_cast<float>(std::min(8, std::max(0, spawnDepth - 2)));
        p1 = std::min(0.35f, p1);
        if (rng.chance(p1)) {
            (void)markOne();

            float p2 = std::min(0.18f, p1 * 0.6f);
            if (spawnDepth >= 7 && rng.chance(p2)) {
                (void)markOne();
            }
        }
    }

    // ---------------------------------------------------------------------
    // Ecosystem resource spawns: small clusters of Essence Shards aligned
    // to procedural biome seeds. These feed the crafting loop and make
    // biome regions feel materially distinct.
    // ---------------------------------------------------------------------
    if (branch_ != DungeonBranch::Camp) {
        const auto& seeds = dung.ecosystemSeedsCached();
        if (!seeds.empty()) {
            // Budget is deliberately conservative so this feels like
            // "interesting pockets" rather than floor-wide loot spam.
            int budget = 2 + std::min(6, std::max(0, spawnDepth) / 3);
            budget = clampi(budget, 2, 8);

            struct SeedCand { size_t idx; int w; };
            std::vector<SeedCand> cands;
            cands.reserve(seeds.size());

            int totalW = 0;
            for (size_t i = 0; i < seeds.size(); ++i) {
                const EcosystemSeed& s = seeds[i];
                if (s.kind == EcosystemKind::None) continue;
                int w = 10;
                w += std::min(16, std::max(0, s.radius));
                if (s.kind == EcosystemKind::CrystalGarden) w += 10;
                if (s.kind == EcosystemKind::FloodedGrotto) w += 4;
                if (s.kind == EcosystemKind::BoneField && spawnDepth >= 6) w += 6;
                if (w <= 0) continue;
                cands.push_back({i, w});
                totalW += w;
            }

            auto pickSeedIndex = [&]() -> size_t {
                if (cands.empty() || totalW <= 0) return static_cast<size_t>(-1);
                int r = rng.range(1, totalW);
                for (const auto& c : cands) {
                    r -= c.w;
                    if (r <= 0) return c.idx;
                }
                return cands.back().idx;
            };

            auto countGroundAt = [&](Vec2i p) -> int {
                int n = 0;
                for (const auto& gi : ground) {
                    if (gi.pos == p) {
                        ++n;
                        if (n >= 3) break;
                    }
                }
                return n;
            };

            auto findEcoDropPos = [&](const EcosystemSeed& s) -> Vec2i {
                const int r = std::max(6, s.radius);
                const int r2 = r * r;

                for (int tries = 0; tries < 220; ++tries) {
                    const int dx = rng.range(-r, r);
                    const int dy = rng.range(-r, r);
                    if (dx * dx + dy * dy > r2) continue;
                    Vec2i p{s.pos.x + dx, s.pos.y + dy};
                    if (!dung.inBounds(p.x, p.y)) continue;
                    if (dung.at(p.x, p.y).type != TileType::Floor) continue;
                    if (roomTypeAt(dung, p) == RoomType::Shop) continue;

                    // Keep the stair landing zones readable.
                    if (dung.inBounds(dung.stairsUp.x, dung.stairsUp.y) && manhattan(p, dung.stairsUp) <= 2) continue;
                    if (dung.inBounds(dung.stairsDown.x, dung.stairsDown.y) && manhattan(p, dung.stairsDown) <= 2) continue;

                    // Stay within the intended ecosystem region.
                    if (dung.ecosystemAtCached(p.x, p.y) != s.kind) continue;

                    // Avoid stacking too much clutter on one tile.
                    if (countGroundAt(p) >= 2) continue;

                    return p;
                }
                return Vec2i{-1, -1};
            };

            auto pickEssenceTag = [&](EcosystemKind eco, TerrainMaterial mat) -> crafttags::Tag {
                // A small "material-sensitive" tag mapping makes biomes feel like
                // more than just color: the same ecosystem can yield different
                // essences when it grows through different substrates.
                switch (eco) {
                    case EcosystemKind::FungalBloom: {
                        if (mat == TerrainMaterial::Moss || mat == TerrainMaterial::Dirt) {
                            return rng.chance(0.55f) ? crafttags::Tag::Regen : crafttags::Tag::Venom;
                        }
                        return rng.chance(0.80f) ? crafttags::Tag::Venom : crafttags::Tag::Regen;
                    }
                    case EcosystemKind::CrystalGarden: {
                        float u = rng.next01();
                        if (mat == TerrainMaterial::Crystal) {
                            if (u < 0.45f) return crafttags::Tag::Rune;
                            if (u < 0.85f) return crafttags::Tag::Arc;
                            return crafttags::Tag::Shield;
                        }
                        if (u < 0.60f) return crafttags::Tag::Arc;
                        if (u < 0.90f) return crafttags::Tag::Rune;
                        return crafttags::Tag::Shield;
                    }
                    case EcosystemKind::BoneField: {
                        return rng.chance(0.65f) ? crafttags::Tag::Daze : crafttags::Tag::Clarity;
                    }
                    case EcosystemKind::RustVeins: {
                        if (mat == TerrainMaterial::Metal) {
                            return rng.chance(0.70f) ? crafttags::Tag::Alch : crafttags::Tag::Stone;
                        }
                        return rng.chance(0.55f) ? crafttags::Tag::Stone : crafttags::Tag::Alch;
                    }
                    case EcosystemKind::AshenRidge: {
                        return rng.chance(0.75f) ? crafttags::Tag::Ember : crafttags::Tag::Stone;
                    }
                    case EcosystemKind::FloodedGrotto: {
                        return rng.chance(0.55f) ? crafttags::Tag::Aurora : crafttags::Tag::Regen;
                    }
                    default:
                        break;
                }
                return crafttags::Tag::None;
            };

            for (int i = 0; i < budget; ++i) {
                const size_t si = pickSeedIndex();
                if (si == static_cast<size_t>(-1) || si >= seeds.size()) break;

                const EcosystemSeed& s = seeds[si];
                const Vec2i pos = findEcoDropPos(s);
                if (!dung.inBounds(pos.x, pos.y)) continue;

                const TerrainMaterial mat = dung.materialAtCached(pos.x, pos.y);
                const crafttags::Tag tag = pickEssenceTag(s.kind, mat);
                if (tag == crafttags::Tag::None) continue;

                int tier = 1 + std::max(0, spawnDepth) / 6;
                if (spawnDepth >= 10 && rng.chance(0.15f)) tier += 1;
                if (s.kind == EcosystemKind::CrystalGarden && rng.chance(0.25f)) tier += 1;
                tier = clampi(tier, 1, 8);

                float shinyChance = 0.04f + 0.01f * static_cast<float>(std::min(10, std::max(0, spawnDepth)));
                if (s.kind == EcosystemKind::CrystalGarden) shinyChance += 0.08f;
                if (s.kind == EcosystemKind::FloodedGrotto) shinyChance += 0.02f;
                shinyChance = std::min(0.22f, shinyChance);

                const bool shiny = rng.chance(shinyChance);

                int count = 1;
                if (rng.chance(0.40f)) count += 1;
                if (spawnDepth >= 8 && rng.chance(0.18f)) count += 1;
                count = clampi(count, 1, 4);

                Item shard;
                shard.id = nextItemId++;
                shard.kind = ItemKind::EssenceShard;
                shard.count = count;
                shard.charges = 0;
                shard.enchant = packEssenceShardEnchant(crafttags::tagIndex(tag), tier, shiny);
                shard.buc = 0;
                shard.spriteSeed = rng.nextU32();
                shard.ego = ItemEgo::None;
                shard.flags = 0;
                shard.shopPrice = 0;
                shard.shopDepth = 0;

                GroundItem gi;
                gi.item = shard;
                gi.pos = pos;
                ground.push_back(gi);
            }


            // ---------------------------------------------------------------------
            // Ecosystem resource nodes: stationary props (Spore Pods, Crystal Nodes,
            // etc.) spawned near biome seeds. Harvest with CONFIRM for shards, but
            // expect a small biome-appropriate backlash (gas/embers/etc.).
            //
            // Uses a derived RNG so node placement doesn't perturb the main loot RNG.
            // ---------------------------------------------------------------------
            {
                const uint32_t nodeSeed = hash32(hashCombine(seed_, 0xB10DE5EDu ^ static_cast<uint32_t>(depth_) ^ (static_cast<uint32_t>(branch_) << 16)));
                RNG nodeRng(nodeSeed);

                int nodeBudget = 1 + std::min(5, std::max(0, spawnDepth) / 4);
                if (spawnDepth >= 9 && nodeRng.chance(0.35f)) nodeBudget += 1;
                nodeBudget = clampi(nodeBudget, 1, 8);

                auto pickSeedIndexNode = [&]() -> size_t {
                    if (cands.empty() || totalW <= 0) return static_cast<size_t>(-1);
                    int r = nodeRng.range(1, totalW);
                    for (const auto& c : cands) {
                        r -= c.w;
                        if (r <= 0) return c.idx;
                    }
                    return cands.back().idx;
                };

                auto nodeKindForEco = [&](EcosystemKind eco) -> ItemKind {
                    switch (eco) {
                        case EcosystemKind::FungalBloom:   return ItemKind::SporePod;
                        case EcosystemKind::CrystalGarden: return ItemKind::CrystalNode;
                        case EcosystemKind::BoneField:     return ItemKind::BonePile;
                        case EcosystemKind::RustVeins:     return ItemKind::RustVent;
                        case EcosystemKind::AshenRidge:    return ItemKind::AshVent;
                        case EcosystemKind::FloodedGrotto: return ItemKind::GrottoSpring;
                        default: break;
                    }
                    return ItemKind::SporePod; // unreachable in practice
                };

                auto findNodePos = [&](const EcosystemSeed& s) -> Vec2i {
                    const int r = std::max(5, s.radius);
                    const int r2 = r * r;

                    for (int tries = 0; tries < 240; ++tries) {
                        const int dx = nodeRng.range(-r, r);
                        const int dy = nodeRng.range(-r, r);
                        if (dx * dx + dy * dy > r2) continue;
                        Vec2i p{s.pos.x + dx, s.pos.y + dy};
                        if (!dung.inBounds(p.x, p.y)) continue;
                        if (dung.at(p.x, p.y).type != TileType::Floor) continue;
                        if (roomTypeAt(dung, p) == RoomType::Shop) continue;

                        // Keep the stair landing zones readable.
                        if (dung.inBounds(dung.stairsUp.x, dung.stairsUp.y) && manhattan(p, dung.stairsUp) <= 2) continue;
                        if (dung.inBounds(dung.stairsDown.x, dung.stairsDown.y) && manhattan(p, dung.stairsDown) <= 2) continue;

                        // Stay within the intended ecosystem region.
                        if (dung.ecosystemAtCached(p.x, p.y) != s.kind) continue;

                        // Prefer clean tiles: don't stack with other loot.
                        if (countGroundAt(p) > 0) continue;

                        // Also avoid placing directly under an entity start position (rare).
                        if (entityAt(p.x, p.y)) continue;

                        return p;
                    }
                    return Vec2i{-1, -1};
                };

                // Bias: fewer nodes if seeds are sparse.
                if (static_cast<int>(cands.size()) < 3) nodeBudget = std::min(nodeBudget, 2);

                for (int i = 0; i < nodeBudget; ++i) {
                    const size_t si = pickSeedIndexNode();
                    if (si == static_cast<size_t>(-1) || si >= seeds.size()) break;

                    const EcosystemSeed& s = seeds[si];
                    const ItemKind nodeKind = nodeKindForEco(s.kind);
                    if (!isEcosystemNodeKind(nodeKind)) continue;

                    const Vec2i pos = findNodePos(s);
                    if (!dung.inBounds(pos.x, pos.y)) continue;

                    Item node;
                    node.id = nextItemId++;
                    node.kind = nodeKind;
                    node.count = 1;

                    // Remaining harvest uses stored in charges.
                    int taps = 1;
                    if (nodeRng.chance(0.38f)) taps += 1;
                    if (spawnDepth >= 8 && nodeRng.chance(0.18f)) taps += 1;
                    taps = clampi(taps, 1, 3);
                    node.charges = taps;

                    node.enchant = 0;
                    node.buc = 0;
                    node.spriteSeed = (nodeRng.nextU32() | 1u);
                    node.ego = ItemEgo::None;
                    node.flags = 0;
                    setItemStationary(node, true);
                    node.shopPrice = 0;
                    node.shopDepth = 0;

                    ground.push_back(GroundItem{node, pos});
                }
            }

            // ---------------------------------------------------------------------
            // Ecosystem loot caches: small themed piles of "real" items near biome seeds.
            //
            // These are intentionally modest (two items + occasional third) and are placed
            // using a derived RNG so they don't perturb the main loot RNG stream.
            // ---------------------------------------------------------------------
            {
                const uint32_t cacheSeed = hash32(hashCombine(seed_, 0xEC0CA5E5u ^ static_cast<uint32_t>(depth_) ^ (static_cast<uint32_t>(branch_) << 16)));
                RNG cacheRng(cacheSeed);

                int cacheBudget = 0;
                if (spawnDepth >= 2 && cacheRng.chance(0.55f)) cacheBudget = 1;
                if (spawnDepth >= 5 && cacheRng.chance(0.30f)) cacheBudget += 1;
                if (spawnDepth >= 9 && cacheRng.chance(0.20f)) cacheBudget += 1;

                // Don't over-clutter sparse biome layouts.
                if (static_cast<int>(cands.size()) < 3) cacheBudget = std::min(cacheBudget, 1);
                cacheBudget = clampi(cacheBudget, 0, 3);

                auto pickSeedIndexCache = [&]() -> size_t {
                    if (cands.empty() || totalW <= 0) return static_cast<size_t>(-1);
                    int r = cacheRng.range(1, totalW);
                    for (const auto& c : cands) {
                        r -= c.w;
                        if (r <= 0) return c.idx;
                    }
                    return cands.back().idx;
                };

                auto findCacheAnchor = [&](const EcosystemSeed& s) -> Vec2i {
                    const int r = std::max(4, std::min(10, s.radius));
                    const int r2 = r * r;

                    for (int tries = 0; tries < 200; ++tries) {
                        const int dx = cacheRng.range(-r, r);
                        const int dy = cacheRng.range(-r, r);
                        if (dx * dx + dy * dy > r2) continue;
                        Vec2i p{s.pos.x + dx, s.pos.y + dy};
                        if (!dung.inBounds(p.x, p.y)) continue;
                        if (dung.at(p.x, p.y).type != TileType::Floor) continue;
                        if (roomTypeAt(dung, p) == RoomType::Shop) continue;

                        // Keep the stair landing zones readable.
                        if (dung.inBounds(dung.stairsUp.x, dung.stairsUp.y) && manhattan(p, dung.stairsUp) <= 2) continue;
                        if (dung.inBounds(dung.stairsDown.x, dung.stairsDown.y) && manhattan(p, dung.stairsDown) <= 2) continue;

                        // Stay within the intended ecosystem region.
                        if (dung.ecosystemAtCached(p.x, p.y) != s.kind) continue;

                        // Keep caches readable: don't stack on existing piles or nodes.
                        if (countGroundAt(p) > 0) continue;

                        return p;
                    }
                    return Vec2i{-1, -1};
                };

                auto findCacheItemPos = [&](Vec2i anchor, EcosystemKind eco) -> Vec2i {
                    // Prefer keeping the cache as a tight pile, but avoid absurd stacking.
                    if (countGroundAt(anchor) < 2) return anchor;

                    for (int tries = 0; tries < 60; ++tries) {
                        const int dx = cacheRng.range(-1, 1);
                        const int dy = cacheRng.range(-1, 1);
                        if (dx == 0 && dy == 0) continue;
                        Vec2i p{anchor.x + dx, anchor.y + dy};
                        if (!dung.inBounds(p.x, p.y)) continue;
                        if (dung.at(p.x, p.y).type != TileType::Floor) continue;
                        if (roomTypeAt(dung, p) == RoomType::Shop) continue;
                        if (dung.ecosystemAtCached(p.x, p.y) != eco) continue;
                        if (countGroundAt(p) >= 2) continue;
                        return p;
                    }
                    return anchor;
                };

                auto dropCache = [&](const EcosystemSeed& s) {
                    const Vec2i anchor = findCacheAnchor(s);
                    if (!dung.inBounds(anchor.x, anchor.y)) return;

                    // Two themed items, plus a small chance for a third.
                    std::vector<std::pair<ItemKind, int>> items;
                    items.reserve(3);

                    switch (s.kind) {
                        case EcosystemKind::FungalBloom: {
                            ItemKind p = cacheRng.chance(0.22f) ? ItemKind::PotionRegeneration : ItemKind::PotionAntidote;
                            int pc = (cacheRng.chance(0.35f) ? 2 : 1);
                            items.push_back({p, pc});

                            ItemKind b = cacheRng.chance(0.55f) ? ItemKind::Dagger : ItemKind::ScrollConfusion;
                            items.push_back({b, 1});

                            if (spawnDepth >= 5 && cacheRng.chance(0.25f)) {
                                items.push_back({ItemKind::PotionClarity, 1});
                            }
                        } break;
                        case EcosystemKind::CrystalGarden: {
                            ItemKind a = cacheRng.chance(0.32f) ? ItemKind::RuneTablet : ItemKind::WandSparks;
                            items.push_back({a, 1});

                            ItemKind b = cacheRng.chance(0.58f) ? ItemKind::ScrollIdentify : ItemKind::PotionVision;
                            items.push_back({b, 1});

                            if (spawnDepth >= 6 && cacheRng.chance(0.18f)) {
                                ItemKind rk = cacheRng.chance(0.50f) ? ItemKind::RingFocus : ItemKind::RingSearching;
                                items.push_back({rk, 1});
                            }
                        } break;
                        case EcosystemKind::BoneField: {
                            ItemKind a = cacheRng.chance(0.55f) ? ItemKind::ScrollRemoveCurse : ItemKind::ScrollEnchantArmor;
                            items.push_back({a, 1});

                            ItemKind b = cacheRng.chance(0.60f) ? ItemKind::ButcheredBones : ItemKind::PotionClarity;
                            int bc = (b == ItemKind::ButcheredBones) ? cacheRng.range(2, 4) : 1;
                            items.push_back({b, bc});

                            if (spawnDepth >= 7 && cacheRng.chance(0.20f)) {
                                items.push_back({ItemKind::Sword, 1});
                            }
                        } break;
                        case EcosystemKind::RustVeins: {
                            ItemKind a = cacheRng.chance(0.60f) ? ItemKind::Lockpick : ItemKind::Key;
                            items.push_back({a, 1});

                            ItemKind b = cacheRng.chance(0.50f) ? ItemKind::ScrollKnock : ItemKind::Dagger;
                            items.push_back({b, 1});

                            if (spawnDepth >= 4 && cacheRng.chance(0.22f)) {
                                items.push_back({ItemKind::PotionStrength, 1});
                            }
                        } break;
                        case EcosystemKind::AshenRidge: {
                            ItemKind a = cacheRng.chance(0.55f) ? ItemKind::PotionHaste : ItemKind::PotionStrength;
                            items.push_back({a, 1});

                            ItemKind w = (spawnDepth >= 5 && cacheRng.chance(0.40f)) ? ItemKind::WandFireball : ItemKind::WandSparks;
                            items.push_back({w, 1});

                            if (spawnDepth >= 6 && cacheRng.chance(0.20f)) {
                                items.push_back({ItemKind::ScrollEarth, 1});
                            }
                        } break;
                        case EcosystemKind::FloodedGrotto: {
                            int hc = 1 + (cacheRng.chance(0.45f) ? 1 : 0);
                            items.push_back({ItemKind::PotionHealing, hc});

                            ItemKind b = cacheRng.chance(0.55f) ? ItemKind::ScrollMapping : ItemKind::PotionVision;
                            items.push_back({b, 1});

                            if (spawnDepth >= 4 && cacheRng.chance(0.18f)) {
                                items.push_back({ItemKind::PotionLevitation, 1});
                            }
                        } break;
                        default:
                            return;
                    }

                    // Place items, allowing a tight pile but avoiding absurd stacking.
                    for (const auto& kv : items) {
                        const Vec2i p = findCacheItemPos(anchor, s.kind);
                        dropItemAtWithRng(cacheRng, kv.first, p, kv.second);
                    }
                };

                for (int i = 0; i < cacheBudget; ++i) {
                    const size_t si = pickSeedIndexCache();
                    if (si == static_cast<size_t>(-1) || si >= seeds.size()) break;
                    const EcosystemSeed& s = seeds[si];
                    if (s.kind == EcosystemKind::None) continue;
                    dropCache(s);
                }
            }


        }
    }
    // ------------------------------------------------------------
    // Leyline rune caches: if the level generator carved an *ancient rune ward*
    // (spawnGraffiti), occasionally place a Rune Tablet nearby with a matching
    // element.
    //
    // This is intentionally sparse: it's meant to be a small “follow the leyline”
    // breadcrumb rather than a guaranteed power spike.
    // ------------------------------------------------------------
    if (branch_ != DungeonBranch::Camp && spawnDepth >= 3) {
        auto runeElementForWard = [&](WardWord ww) -> ProcSpellElement {
            switch (ww) {
                case WardWord::RuneFire:     return ProcSpellElement::Fire;
                case WardWord::RuneFrost:    return ProcSpellElement::Frost;
                case WardWord::RuneShock:    return ProcSpellElement::Shock;
                case WardWord::RuneWind:     return ProcSpellElement::Wind;
                case WardWord::RuneStone:    return ProcSpellElement::Stone;
                case WardWord::RuneVenom:    return ProcSpellElement::Venom;
                case WardWord::RuneShadow:   return ProcSpellElement::Shadow;
                case WardWord::RuneRadiance: return ProcSpellElement::Radiance;
                case WardWord::RuneBlood:    return ProcSpellElement::Blood;
                case WardWord::RuneArcane:   return ProcSpellElement::Arcane;
                default: break;
            }
            return ProcSpellElement::Arcane;
        };

        auto isRuneWard = [&](WardWord ww) -> bool {
            switch (ww) {
                case WardWord::RuneFire:
                case WardWord::RuneFrost:
                case WardWord::RuneShock:
                case WardWord::RuneWind:
                case WardWord::RuneStone:
                case WardWord::RuneVenom:
                case WardWord::RuneShadow:
                case WardWord::RuneRadiance:
                case WardWord::RuneBlood:
                case WardWord::RuneArcane:
                    return true;
                default:
                    return false;
            }
        };

        auto findDropNear = [&](Vec2i c, RNG& rr) -> Vec2i {
            // Try the ward tile first, then expand out.
            for (int r = 0; r <= 2; ++r) {
                for (int tries = 0; tries < 80; ++tries) {
                    const int dx = rr.range(-r, r);
                    const int dy = rr.range(-r, r);
                    Vec2i p{c.x + dx, c.y + dy};
                    if (!dung.inBounds(p.x, p.y)) continue;
                    if (dung.at(p.x, p.y).type != TileType::Floor) continue;
                    if (roomTypeAt(dung, p) == RoomType::Shop) continue;
                    if (entityAt(p.x, p.y)) continue;
                    if (hasGroundAt(p)) continue;
                    return p;
                }
            }
            return Vec2i{-1, -1};
        };

        // Cap the number of rune caches so we don't over-inflate early tablet counts.
        const int maxCaches = (spawnDepth >= 10) ? 2 : 1;
        int cachesPlaced = 0;

        for (const Engraving& eg : engravings_) {
            if (cachesPlaced >= maxCaches) break;

            // Only consider procedurally generated wards.
            if (!eg.isWard || !eg.isGraffiti) continue;

            const WardWord ww = wardWordFromText(eg.text);
            if (!isRuneWard(ww)) continue;

            const uint32_t h = hash32(hashCombine(hashCombine(seed_, "RUNE_CACHE"_tag), static_cast<uint32_t>(spawnDepth)) ^ static_cast<uint32_t>(eg.pos.x * 73856093u) ^ static_cast<uint32_t>(eg.pos.y * 19349663u));
            RNG rr(h);

            float chance = 0.32f + 0.018f * static_cast<float>(std::min(12, std::max(0, spawnDepth - 3)));
            chance = std::min(0.58f, chance);
            if (!rr.chance(chance)) continue;

            const Vec2i dropPos = findDropNear(eg.pos, rr);
            if (!dung.inBounds(dropPos.x, dropPos.y)) continue;

            // Build a proc spell id that *matches the ward element* (up to a small
            // bounded search). This makes the cache feel connected to the ward.
            int tier = 1 + spawnDepth / 2;
            if (spawnDepth >= 6 && rr.chance(0.18f)) tier += 1;
            tier = clampi(tier, 1, 15);

            const ProcSpellElement wantElem = runeElementForWard(ww);
            const uint32_t baseSeed28 = rr.nextU32() & PROC_SPELL_SEED_MASK;

            uint32_t chosenId = makeProcSpellId(static_cast<uint8_t>(tier), baseSeed28);
            for (uint32_t i = 0; i < 96; ++i) {
                const uint32_t seed28 = (baseSeed28 + i) & PROC_SPELL_SEED_MASK;
                const uint32_t pid = makeProcSpellId(static_cast<uint8_t>(tier), seed28);
                if (generateProcSpell(pid).element == wantElem) { chosenId = pid; break; }
            }

            Item tab;
            tab.id = nextItemId++;
            tab.kind = ItemKind::RuneTablet;
            tab.count = 1;
            tab.enchant = 0;
            tab.buc = 0;
            tab.charges = 0;
            tab.spriteSeed = chosenId;
            tab.ego = ItemEgo::None;
            tab.flags = 0;
            tab.shopPrice = 0;
            tab.shopDepth = 0;

            ground.push_back(GroundItem{tab, dropPos});
            cachesPlaced += 1;
        }
    }
}

void Game::spawnTraps() {
    if (atHomeCamp()) return;

    trapsCur.clear();

    // Use a depth-like scalar for the overworld (Camp/0 wilderness chunks).
    const int spawnDepth = materialDepth();

    // Ecosystem field is computed alongside the material cache; ensure it exists
    // before any ecosystem-aware trap placement below.
    dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());

    // A small number of traps per floor, scaling gently with depth.
    // (Setpieces below may "spend" some of this budget by placing traps in patterns,
    // so the total density stays roughly stable.)
    const int base = 2;
    const int depthBonus = std::min(6, spawnDepth / 2);
    int targetCount = base + depthBonus + rng.range(0, 2);

    // Penultimate floor (the labyrinth) is intentionally trap-heavy.
    if (spawnDepth == QUEST_DEPTH - 1) {
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
        if (spawnDepth <= 2) {
            if (r < 55) return TrapKind::Alarm;
            if (r < 88) return TrapKind::PoisonDart;
            return TrapKind::Web;
        }
        if (spawnDepth <= 5) {
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
        if (r < 91) return TrapKind::PoisonGas;
        if (spawnDepth >= 8 && r < 94) return TrapKind::CorrosiveGas;
        if (r < 96) return TrapKind::LetheMist;
        return TrapKind::Teleport;
    };

    for (const Vec2i& c : dung.bonusLootSpots) {
        if (!dung.inBounds(c.x, c.y)) continue;
        if (!hasChestAt(c)) continue;

        // Don't "ambush" the player in the start area even if a cache spawns close.
        if (manhattan(c, player().pos) <= 6) continue;

        // Try to place 1-2 guard traps around the cache.
        int want = 1;
        if (spawnDepth >= 6 && rng.chance(0.35f)) want = 2;
        if (spawnDepth == QUEST_DEPTH - 1 && rng.chance(0.40f)) want += 1;

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
        if (spawnDepth <= 2) {
            return (r < 70) ? TrapKind::Spike : TrapKind::PoisonDart;
        }
        if (spawnDepth <= 5) {
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

    // Corridor junctions (degree >= 3) are distinct from chokepoints and make
    // good candidates for "high-traffic" trap placement.
    std::vector<Vec2i> junctions;
    junctions.reserve(512);

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

                // Corridor junctions (3-way/4-way) tend to be high-traffic spaces.
                if (deg >= 3) junctions.push_back(p);

                // Identify straight 1-wide corridor segments for trap strips.
                if (deg == 2) {
                    if (L && R && !U && !D) straight.push_back(StraightCorr{p, 0});
                    else if (U && D && !L && !R) straight.push_back(StraightCorr{p, 1});
                }
            }
        }
    }

    int gauntletsWanted = 0;
    if (spawnDepth >= 3 && rng.chance(0.22f)) gauntletsWanted = 1;
    if (spawnDepth == QUEST_DEPTH - 1) gauntletsWanted = 1;

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
            if (spawnDepth >= 4) want += 1;
            if (spawnDepth >= 7 && rng.chance(0.35f)) want += 1;
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
    // Traffic traps: place 1-2 traps in corridor junctions that lie on many
    // sampled shortest paths between important points (stairs + special rooms).
    // This approximates a "betweenness"/centrality signal and makes trap
    // placement feel less uniform than pure random scatter.
    // ------------------------------------------------------------
    auto pickTrafficTrap = [&]() -> TrapKind {
        int r = rng.range(0, 99);
        if (spawnDepth <= 2) {
            return (r < 65) ? TrapKind::Alarm : TrapKind::PoisonDart;
        }
        if (spawnDepth <= 5) {
            if (r < 32) return TrapKind::Alarm;
            if (r < 56) return TrapKind::Web;
            if (r < 76) return TrapKind::PoisonDart;
            if (r < 90) return TrapKind::ConfusionGas;
            return TrapKind::Teleport;
        }
        if (r < 24) return TrapKind::Alarm;
        if (r < 44) return TrapKind::Web;
        if (r < 60) return TrapKind::ConfusionGas;
        if (r < 72) return TrapKind::PoisonDart;
        if (r < 78) return TrapKind::PoisonGas;
        if (spawnDepth >= 8 && r < 82) return TrapKind::CorrosiveGas;
        if (r < 88) return TrapKind::LetheMist;
        if (r < 92) return TrapKind::Teleport;
        return TrapKind::RollingBoulder;
    };

    int trafficTrapsWanted = 0;
    if (spawnDepth >= 3 && rng.chance(0.28f)) trafficTrapsWanted = 1;
    if (spawnDepth >= 7 && rng.chance(0.18f)) trafficTrapsWanted += 1;

    if (trafficTrapsWanted > 0 && (!junctions.empty() || !chokepoints.empty()) && !candidatesAll.empty()) {
        auto pickFrom = [&](const std::vector<Vec2i>& v) -> Vec2i {
            if (v.empty()) return Vec2i{-1, -1};
            const int i = rng.range(0, static_cast<int>(v.size()) - 1);
            return v[static_cast<size_t>(i)];
        };

        // Build a small set of "hub" points: stairs + special rooms + a few random tiles.
        std::vector<Vec2i> hubs;
        hubs.reserve(32);

        hubs.push_back(player().pos);
        if (dung.inBounds(dung.stairsDown.x, dung.stairsDown.y)) hubs.push_back(dung.stairsDown);

        auto addHub = [&](Vec2i p) {
            if (!dung.inBounds(p.x, p.y)) return;
            if (!dung.isPassable(p.x, p.y)) return;
            hubs.push_back(p);
        };

        for (const Room& r : dung.rooms) {
            switch (r.type) {
                case RoomType::Treasure:
                case RoomType::Lair:
                case RoomType::Vault:
                case RoomType::Secret:
                case RoomType::Shop:
                case RoomType::Shrine:
                case RoomType::Armory:
                case RoomType::Library:
                case RoomType::Laboratory:
                    addHub(Vec2i{r.cx(), r.cy()});
                    break;
                default:
                    break;
            }
        }

        // Add a few random hubs to capture generic movement patterns.
        const int extraHubs = std::min(8, std::max(3, static_cast<int>(candidatesAll.size() / 250)));
        for (int i = 0; i < extraHubs && !candidatesAll.empty(); ++i) {
            addHub(pickFrom(candidatesAll));
        }

        // If we still have too few hubs, bail (not enough structure).
        if (hubs.size() >= 4) {
            const int N = W * H;
            std::vector<int> traffic(static_cast<size_t>(N), 0);
            std::vector<int> prev(static_cast<size_t>(N), -1);
            std::vector<int> q;
            q.reserve(static_cast<size_t>(N));

            auto passable = [&](int x, int y) -> bool {
                return dung.inBounds(x, y) && dung.isPassable(x, y);
            };

            auto bfsAccumulate = [&](Vec2i src, Vec2i dst) -> bool {
                if (!passable(src.x, src.y)) return false;
                if (!passable(dst.x, dst.y)) return false;

                const int s = src.y * W + src.x;
                const int t = dst.y * W + dst.x;
                if (s == t) return false;

                std::fill(prev.begin(), prev.end(), -1);
                q.clear();

                prev[static_cast<size_t>(s)] = s;
                q.push_back(s);
                size_t qi = 0;

                while (qi < q.size()) {
                    const int cur = q[qi++];
                    if (cur == t) break;
                    const int cx = cur % W;
                    const int cy = cur / W;

                    const int nx[4] = {cx + 1, cx - 1, cx, cx};
                    const int ny[4] = {cy, cy, cy + 1, cy - 1};

                    for (int k = 0; k < 4; ++k) {
                        const int x = nx[k];
                        const int y = ny[k];
                        if (!passable(x, y)) continue;
                        const int ni = y * W + x;
                        if (ni < 0 || ni >= N) continue;
                        if (prev[static_cast<size_t>(ni)] != -1) continue;
                        prev[static_cast<size_t>(ni)] = cur;
                        q.push_back(ni);
                    }
                }

                if (prev[static_cast<size_t>(t)] == -1) return false;

                // Reconstruct and accumulate. (Don't bother counting endpoints twice.)
                int cur = t;
                int safety = 0;
                while (cur != s && safety < N + 8) {
                    traffic[static_cast<size_t>(cur)] += 1;
                    cur = prev[static_cast<size_t>(cur)];
                    if (cur < 0) break;
                    safety += 1;
                }
                traffic[static_cast<size_t>(s)] += 1;
                return true;
            };

            // Sample a handful of hub-to-hub paths.
            const int wantSamples = std::min(26, 10 + static_cast<int>(hubs.size()));
            int attempts = 0;
            int successes = 0;
            while (successes < wantSamples && attempts < wantSamples * 5) {
                attempts += 1;
                const Vec2i a = hubs[static_cast<size_t>(rng.range(0, static_cast<int>(hubs.size()) - 1))];
                const Vec2i b = hubs[static_cast<size_t>(rng.range(0, static_cast<int>(hubs.size()) - 1))];
                if (a == b) continue;
                if (bfsAccumulate(a, b)) successes += 1;
            }

            if (successes >= 6) {
                struct TCand { Vec2i p; int score; };
                std::vector<TCand> tcands;
                tcands.reserve(256);

	            	const std::vector<Vec2i>& trafficBase = !junctions.empty() ? junctions : chokepoints;
	            	for (const Vec2i& p : trafficBase) {
                    if (!dung.inBounds(p.x, p.y)) continue;
                    if (!dung.isWalkable(p.x, p.y)) continue;
                    if (manhattan(p, player().pos) <= 7) continue;
                    if (manhattan(p, dung.stairsUp) <= 5) continue;
                    if (manhattan(p, dung.stairsDown) <= 5) continue;
                    const int ii = p.y * W + p.x;
                    if (ii < 0 || ii >= N) continue;
                    const int score = traffic[static_cast<size_t>(ii)];
                    if (score <= 0) continue;
                    tcands.push_back(TCand{p, score});
                }

                if (!tcands.empty()) {
                    std::sort(tcands.begin(), tcands.end(), [](const TCand& a, const TCand& b) {
                        return a.score > b.score;
                    });

                    int placed = 0;
                    int tries = 0;
                    int window = std::min(12, static_cast<int>(tcands.size()));
                    while (placed < trafficTrapsWanted && tries < trafficTrapsWanted * 8 && !tcands.empty()) {
                        window = std::min(window, static_cast<int>(tcands.size()));
                        if (window <= 0) break;
                        const int pick = rng.range(0, window - 1);
                        const Vec2i p = tcands[static_cast<size_t>(pick)].p;
                        if (addFloorTrap(p, pickTrafficTrap(), false, false)) {
                            placed += 1;
                        }
                        tcands.erase(tcands.begin() + pick);
                        tries += 1;
                    }
                }
            }
        }
    }



    // ------------------------------------------------------------
    // Ecosystem-biased trap clusters: small regional hazards that make
    // biome patches feel mechanically distinct.
    //
    // NOTE: These use an isolated RNG stream so they don't perturb other
    // setpieces within spawnTraps(). They still consume trap *budget*
    // naturally (they count toward trapsCur.size()).
    // ------------------------------------------------------------
    {
        const auto& ecoSeeds = dung.ecosystemSeedsCached();
        if (!ecoSeeds.empty()) {
            RNG erng(hashCombine(levelGenSeed(LevelId{branch_, depth_}), 0xEC057A2Bu));

            // How many ecosystem traps to try to add this floor.
            // Keep it subtle: this is flavor, not a new global difficulty knob.
            int ecoBudget = 0;
            if (spawnDepth >= 2 && erng.chance(0.65f)) ecoBudget += 1;
            if (spawnDepth >= 5 && erng.chance(0.50f)) ecoBudget += 1;
            if (spawnDepth >= 9 && erng.chance(0.35f)) ecoBudget += 1;
            if (spawnDepth >= 12 && erng.chance(0.25f)) ecoBudget += 1;
            if (spawnDepth == QUEST_DEPTH - 1 && erng.chance(0.55f)) ecoBudget += 1;
            ecoBudget = clampi(ecoBudget, 0, 4);
            ecoBudget = std::min(ecoBudget, std::max(0, targetCount - 1));

            auto ecoTrapWeight = [&](EcosystemKind k) -> int {
                // Slight bias toward more "readable"/distinct hazards.
                switch (k) {
                    case EcosystemKind::FungalBloom:   return 8;
                    case EcosystemKind::CrystalGarden: return 9;
                    case EcosystemKind::BoneField:     return 7;
                    case EcosystemKind::RustVeins:     return 7;
                    case EcosystemKind::AshenRidge:    return 8;
                    case EcosystemKind::FloodedGrotto: return 6;
                    default:                           return 0;
                }
            };

            struct EcoPick { int idx = 0; int w = 0; };
            std::vector<EcoPick> table;
            table.reserve(ecoSeeds.size());
            for (int i = 0; i < static_cast<int>(ecoSeeds.size()); ++i) {
                const EcosystemSeed& s = ecoSeeds[static_cast<size_t>(i)];
                if (s.kind == EcosystemKind::None) continue;
                int w = ecoTrapWeight(s.kind);
                w += clampi(s.radius, 2, 7);
                if (spawnDepth >= 10) w += 1;
                if (w > 0) table.push_back(EcoPick{i, w});
            }

            auto pickSeedIndex = [&]() -> int {
                if (table.empty()) return -1;
                int total = 0;
                for (const auto& e : table) total += std::max(0, e.w);
                if (total <= 0) return -1;
                int r = erng.range(1, total);
                for (const auto& e : table) {
                    r -= std::max(0, e.w);
                    if (r <= 0) return e.idx;
                }
                return table.back().idx;
            };

            auto pickEcoTrap = [&](EcosystemKind eco) -> TrapKind {
                // Keep these as "floor feel" hazards rather than instant-kill spikes.
                const int r = erng.range(0, 99);
                switch (eco) {
                    case EcosystemKind::FungalBloom: {
                        if (r < 50) return TrapKind::Web;
                        if (r < 78) return TrapKind::ConfusionGas;
                        if (spawnDepth >= 4 && r < 92) return TrapKind::PoisonGas;
                        return TrapKind::PoisonDart;
                    }
                    case EcosystemKind::CrystalGarden: {
                        // "Runes" and sudden angles.
                        if (r < 42) return TrapKind::Alarm;
                        if (r < 70) return TrapKind::Teleport;
                        if (spawnDepth >= 6 && r < 86) return TrapKind::LetheMist;
                        return TrapKind::Spike;
                    }
                    case EcosystemKind::BoneField: {
                        if (r < 62) return TrapKind::Spike;
                        if (spawnDepth != DUNGEON_MAX_DEPTH && spawnDepth >= 4 && r < 74) return TrapKind::TrapDoor;
                        if (r < 88) return TrapKind::Alarm;
                        return TrapKind::PoisonDart;
                    }
                    case EcosystemKind::RustVeins: {
                        if (spawnDepth >= 8 && r < 35) return TrapKind::CorrosiveGas;
                        if (r < 62) return TrapKind::Spike;
                        if (r < 84) return TrapKind::PoisonDart;
                        return TrapKind::Alarm;
                    }
                    case EcosystemKind::AshenRidge: {
                        if (r < 50) return TrapKind::LetheMist;
                        if (r < 74) return TrapKind::ConfusionGas;
                        if (spawnDepth >= 6 && r < 86) return TrapKind::RollingBoulder;
                        return TrapKind::Spike;
                    }
                    case EcosystemKind::FloodedGrotto: {
                        if (r < 55) return TrapKind::LetheMist;
                        if (r < 78) return TrapKind::Alarm;
                        if (spawnDepth >= 5 && r < 90) return TrapKind::Teleport;
                        return TrapKind::Web;
                    }
                    default:
                        break;
                }
                return TrapKind::Spike;
            };

            auto findEcoTrapPos = [&](const EcosystemSeed& s) -> Vec2i {
                // Try a handful of points near the seed center.
                const int rr = clampi(s.radius, 2, 7);
                for (int tries = 0; tries < 60; ++tries) {
                    const int dx = erng.range(-rr, rr);
                    const int dy = erng.range(-rr, rr);
                    if (std::max(std::abs(dx), std::abs(dy)) > rr) continue;
                    Vec2i p{s.pos.x + dx, s.pos.y + dy};
                    if (!dung.inBounds(p.x, p.y)) continue;
                    if (dung.at(p.x, p.y).type != TileType::Floor) continue;
                    if (dung.ecosystemAtCached(p.x, p.y) != s.kind) continue;
                    if (isBadFloorPos(p)) continue;
                    return p;
                }
                return Vec2i{-1, -1};
            };

            int ecoPlaced = 0;
            int ecoTries = 0;
            while (ecoPlaced < ecoBudget && ecoTries < 80 + ecoBudget * 40 && static_cast<int>(trapsCur.size()) < targetCount) {
                ecoTries += 1;
                const int si = pickSeedIndex();
                if (si < 0 || si >= static_cast<int>(ecoSeeds.size())) break;
                const EcosystemSeed& s = ecoSeeds[static_cast<size_t>(si)];
                const Vec2i p0 = findEcoTrapPos(s);
                if (!dung.inBounds(p0.x, p0.y)) continue;

                // Place the anchor trap.
                const TrapKind t0 = pickEcoTrap(s.kind);
                if (!addFloorTrap(p0, t0, false, true)) continue;
                ecoPlaced += 1;

                // Optional small cluster (adjacent tile). This makes biome hazards "read" as a patch.
                const bool cluster = (ecoPlaced < ecoBudget) && erng.chance(0.55f);
                if (!cluster) continue;

                std::vector<Vec2i> adj;
                adj.reserve(8);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        Vec2i p{p0.x + dx, p0.y + dy};
                        if (!dung.inBounds(p.x, p.y)) continue;
                        if (dung.at(p.x, p.y).type != TileType::Floor) continue;
                        if (dung.ecosystemAtCached(p.x, p.y) != s.kind) continue;
                        adj.push_back(p);
                    }
                }
                for (int i = static_cast<int>(adj.size()) - 1; i > 0; --i) {
                    const int j = erng.range(0, i);
                    std::swap(adj[static_cast<size_t>(i)], adj[static_cast<size_t>(j)]);
                }

                for (const Vec2i& p : adj) {
                    if (ecoPlaced >= ecoBudget) break;
                    if (static_cast<int>(trapsCur.size()) >= targetCount) break;
                    if (addFloorTrap(p, pickEcoTrap(s.kind), false, true)) {
                        ecoPlaced += 1;
                    }
                }
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
        if (spawnDepth == QUEST_DEPTH - 1) {
            // Labyrinth: more "tactical" traps than raw damage.
            if (roll < 22) tk = TrapKind::Spike;
            else if (roll < 44) tk = TrapKind::PoisonDart;
            else if (roll < 64) tk = TrapKind::Alarm;
            else if (roll < 80) tk = TrapKind::Web;
            else if (roll < 86) tk = TrapKind::ConfusionGas;
            else if (roll < 89) tk = TrapKind::PoisonGas;
            else if (roll < 91) tk = TrapKind::CorrosiveGas;
            else if (roll < 93) tk = TrapKind::LetheMist;
            else if (roll < 96) tk = TrapKind::RollingBoulder;
            else if (spawnDepth != DUNGEON_MAX_DEPTH && roll < 98) tk = TrapKind::TrapDoor;
            else tk = TrapKind::Teleport;
        } else if (spawnDepth <= 1) {
            tk = (roll < 70) ? TrapKind::Spike : TrapKind::PoisonDart;
        } else if (spawnDepth <= 3) {
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
            else if (roll < 92) tk = TrapKind::PoisonGas;
            else if (spawnDepth >= 8 && roll < 94) tk = TrapKind::CorrosiveGas;
            else if (roll < 96) tk = TrapKind::LetheMist;
            else if (roll < 97) tk = TrapKind::RollingBoulder;
            else if (spawnDepth != DUNGEON_MAX_DEPTH && roll < 99) tk = TrapKind::TrapDoor;
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
    const float doorTrapDepth = 0.02f * static_cast<float>(std::min(8, spawnDepth));
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
            if (spawnDepth >= 8 && rng.chance(0.05f)) t.kind = TrapKind::CorrosiveGas;
            else if (spawnDepth >= 4 && rng.chance(0.10f)) t.kind = TrapKind::PoisonGas;
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
        if (spawnDepth >= 6 && rng.chance(0.25f)) extra += 1;

        for (int i = 0; i < extra; ++i) {
            Vec2i p = randomFreeTileInRoom(r);
            if (isBadFloorPos(p)) continue;
            if (alreadyHasTrap(p)) continue;

            Trap t;
            t.pos = p;
            t.discovered = false;
            const int roll = rng.range(0, 99);
            if (roll < 42) t.kind = TrapKind::ConfusionGas;
            else if (roll < 56) t.kind = TrapKind::PoisonGas;
            else if (spawnDepth >= 8 && roll < 70) t.kind = TrapKind::CorrosiveGas;
            else if (roll < 88) t.kind = TrapKind::PoisonDart;
            else if (roll < 95) t.kind = TrapKind::Alarm;
            else t.kind = TrapKind::Teleport;
            trapsCur.push_back(t);
        }
    }

    // Procedural field hazards: labs can spawn persistent chemical spill fields.
    spawnChemicalHazards();

    // Consume generator hints (bonus cache locations) now that traps have been placed.
    dung.bonusLootSpots.clear();

}

void Game::spawnChemicalHazards() {
    if (atHomeCamp()) return;
    if (dung.rooms.empty()) return;

    const size_t n = static_cast<size_t>(dung.width * dung.height);
    if (n == 0) return;

    // Ensure hazard fields are sized.
    if (confusionGas_.size() != n) confusionGas_.assign(n, uint8_t{0});
    if (poisonGas_.size() != n) poisonGas_.assign(n, uint8_t{0});
    if (corrosiveGas_.size() != n) corrosiveGas_.assign(n, uint8_t{0});
    if (fireField_.size() != n) fireField_.assign(n, uint8_t{0});
    if (adhesiveFluid_.size() != n) adhesiveFluid_.assign(n, uint8_t{0});

    auto idx = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y * dung.width + x);
    };

    // Use an isolated RNG stream so chemical hazards do not perturb other generation
    // (monsters/items/traps remain stable for a given level seed).
    RNG crng(hashCombine(levelGenSeed(LevelId{branch_, depth_}), 0xC4EFC0DEu));

    // Safety: don't spawn spill fields right on top of arrivals / stairs.
    auto safeTile = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (!dung.isWalkable(x, y)) return false;
        const Vec2i p{x, y};
        if (p == dung.stairsUp || p == dung.stairsDown) return false;
        if (manhattan(p, dung.stairsUp) <= 4) return false;
        if (manhattan(p, dung.stairsDown) <= 4) return false;
        if (manhattan(p, player().pos) <= 6) return false;
        return true;
    };

    // Local variant that uses the isolated RNG stream.
    auto randomFreeTileInRoomChem = [&](const Room& r) -> Vec2i {
        int loX = r.x + 1;
        int hiX = r.x + r.w - 2;
        int loY = r.y + 1;
        int hiY = r.y + r.h - 2;
        if (hiX < loX) { loX = r.x; hiX = r.x + r.w - 1; }
        if (hiY < loY) { loY = r.y; hiY = r.y + r.h - 1; }

        Vec2i best{r.cx(), r.cy()};
        int bestScore = -999999;

        for (int it = 0; it < 200; ++it) {
            const int x = crng.range(loX, hiX);
            const int y = crng.range(loY, hiY);
            if (!dung.inBounds(x, y)) continue;
            if (!dung.isWalkable(x, y)) continue;

            const Vec2i p{x, y};
            int score = 0;
            score -= manhattan(p, Vec2i{r.cx(), r.cy()});
            if (safeTile(x, y)) score += 1000;

            if (score > bestScore) {
                bestScore = score;
                best = p;
            }

            if (safeTile(x, y)) return p;
        }

        return best;
    };

    auto clampf = [&](float v, float lo, float hi) -> float {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    };

    enum class ChemTheme : uint8_t {
        Noxious = 0,  // confusion gas
        Toxic,        // poison gas
        Acidic,       // corrosive gas
        Mixed,        // corrosive + poison (reacts into confusion)
        Volatile,     // poison + embers (fire)
        Adhesive,     // sticky polymer sludge (adhesive fluid)
    };

    // A few Gray-Scott reaction-diffusion presets that produce distinct "spill" patterns.
    // (Values loosely based on classic parameter sets.)
    const proc::GrayScottParams presets[] = {
        {1.0f, 0.50f, 0.0367f, 0.0649f},
        {1.0f, 0.50f, 0.0300f, 0.0620f},
        {1.0f, 0.50f, 0.0220f, 0.0510f},
        {1.0f, 0.50f, 0.0460f, 0.0630f},
    };

    auto chooseTheme = [&](int wetScore) -> ChemTheme {
        // Wet laboratories with active water sources produce deterministic sticky runoff.
        if (wetScore >= 4) return ChemTheme::Adhesive;

        // Deeper floors bias toward nastier chemistry.
        int r = crng.range(0, 99);
        if (wetScore > 0 && r < 18) return ChemTheme::Adhesive;
        if (depth_ >= 8 && r < 16) return ChemTheme::Mixed;
        if (depth_ >= 6 && r < 36) return ChemTheme::Acidic;
        if (depth_ >= 5 && r >= 92) return ChemTheme::Volatile;
        if (r < 55) return ChemTheme::Toxic;
        return ChemTheme::Noxious;
    };

    int labsSeeded = 0;
    const int labBudget = (depth_ >= 6 && crng.chance(0.40f)) ? 2 : 1;

    for (const Room& r : dung.rooms) {
        if (labsSeeded >= labBudget) break;
        if (r.type != RoomType::Laboratory) continue;

        // Wetness score from fishable water inside the room interior.
        int wetScore = 0;
        const int wx0 = r.x + 1;
        const int wy0 = r.y + 1;
        const int wx1 = r.x + r.w - 2;
        const int wy1 = r.y + r.h - 2;
        for (int y = wy0; y <= wy1; ++y) {
            for (int x = wx0; x <= wx1; ++x) {
                if (!dung.inBounds(x, y)) continue;
                const TileType tt = dung.at(x, y).type;
                if (tt == TileType::Fountain) wetScore += 4;
                else if (tt == TileType::Chasm) wetScore += 2;
                if (wetScore >= 12) break;
            }
            if (wetScore >= 12) break;
        }

        // Avoid seeding hazards in/near the start room.
        const Vec2i c{r.cx(), r.cy()};
        const int distStart = manhattan(c, player().pos);
        float chance = 0.18f + 0.02f * static_cast<float>(std::min(depth_, 12));
        if (distStart <= 10) chance *= 0.35f;
        if (r.w * r.h >= 70) chance += 0.06f;
        if (wetScore > 0) chance += 0.12f;
        chance = clampf(chance, 0.08f, 0.70f);

        // Active in-room water in a lab almost always causes a spill signature.
        const bool forcedWetLab = (wetScore >= 4);
        if (!forcedWetLab && !crng.chance(chance)) continue;

        const ChemTheme theme = chooseTheme(wetScore);

        // Work on the room interior (skip the perimeter tiles so doors remain less "spammy").
        const int x0 = r.x + 1;
        const int y0 = r.y + 1;
        const int iw = std::max(1, r.w - 2);
        const int ih = std::max(1, r.h - 2);

        // If the room is tiny, fall back to a simple blob spill.
        const bool tiny = (iw * ih) < 12;

        std::vector<float> A;
        std::vector<float> B;

        if (!tiny) {
            // Seed B with a few droplets.
            A.assign(static_cast<size_t>(iw * ih), 1.0f);
            B.assign(static_cast<size_t>(iw * ih), 0.0f);

            const int presetIdx = crng.range(0, static_cast<int>(sizeof(presets) / sizeof(presets[0])) - 1);
            const proc::GrayScottParams pset = presets[static_cast<size_t>(presetIdx)];

            const int seeds = clampi(2 + (iw * ih > 60 ? 1 : 0), 1, 5);
            const int padX = std::max(0, iw / 4);
            const int padY = std::max(0, ih / 4);
            const int loX = padX;
            const int hiX = std::max(loX, iw - 1 - padX);
            const int loY = padY;
            const int hiY = std::max(loY, ih - 1 - padY);

            for (int s = 0; s < seeds; ++s) {
                const int sx = crng.range(loX, hiX);
                const int sy = crng.range(loY, hiY);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = clampi(sx + dx, 0, iw - 1);
                        const int yy = clampi(sy + dy, 0, ih - 1);
                        const size_t i = static_cast<size_t>(yy * iw + xx);
                        B[i] = 1.0f;
                        A[i] = 0.0f;
                    }
                }
            }

            // A touch of noise so different labs don't converge on the same stable pattern.
            for (size_t i = 0; i < B.size(); ++i) {
                const float n01 = crng.next01();
                if (n01 < 0.08f) {
                    B[i] = clampf(B[i] + (0.05f + 0.12f * crng.next01()), 0.0f, 1.0f);
                }
            }

            const int iters = 18 + crng.range(0, 14);
            proc::runGrayScott(iw, ih, pset, iters, A, B);
        }

        // Compute min/max for dynamic range normalization.
        float minB = 1.0f;
        float maxB = 0.0f;
        if (!tiny) {
            for (float v : B) {
                if (v < minB) minB = v;
                if (v > maxB) maxB = v;
            }
        }

        const int maxI = clampi(9 + depth_ / 2, 9, 16);

        auto addField = [&](std::vector<uint8_t>& field, size_t gi, uint8_t v) {
            if (gi >= field.size()) return;
            if (field[gi] < v) field[gi] = v;
        };

        if (tiny || maxB <= minB + 0.0001f) {
            // Simple radial spill (tiny labs or degenerate RD output).
            const Vec2i seed = randomFreeTileInRoomChem(r);
            if (!safeTile(seed.x, seed.y)) continue;

            const int radius = 2 + (crng.chance(0.20f) ? 1 : 0);
            for (int yy = seed.y - radius; yy <= seed.y + radius; ++yy) {
                for (int xx = seed.x - radius; xx <= seed.x + radius; ++xx) {
                    if (!safeTile(xx, yy)) continue;
                    const int dist = std::max(std::abs(xx - seed.x), std::abs(yy - seed.y));
                    int s = maxI - dist * 4;
                    if (s < 2) continue;
                    const uint8_t v = static_cast<uint8_t>(clampi(s, 0, 255));
                    const size_t gi = idx(xx, yy);

                    switch (theme) {
                        case ChemTheme::Noxious:  addField(confusionGas_, gi, v); break;
                        case ChemTheme::Toxic:    addField(poisonGas_, gi, v); break;
                        case ChemTheme::Acidic:   addField(corrosiveGas_, gi, v); break;
                        case ChemTheme::Mixed:
                            addField(corrosiveGas_, gi, v);
                            if (dist <= 1) addField(poisonGas_, gi, static_cast<uint8_t>(clampi(s - 1, 0, 255)));
                            break;
                        case ChemTheme::Volatile:
                            addField(poisonGas_, gi, v);
                            if (dist == 0) addField(fireField_, gi, static_cast<uint8_t>(clampi(7 + s / 3, 0, 255)));
                            break;
                        case ChemTheme::Adhesive:
                            addField(adhesiveFluid_, gi, static_cast<uint8_t>(clampi(s + 2, 0, 255)));
                            if (dist == 0) addField(poisonGas_, gi, static_cast<uint8_t>(clampi(2 + s / 3, 0, 255)));
                            break;
                    }
                }
            }

            ++labsSeeded;
            continue;
        }

        // Reaction-diffusion spill mapping.
        for (int yy = 0; yy < ih; ++yy) {
            for (int xx = 0; xx < iw; ++xx) {
                const int wx = x0 + xx;
                const int wy = y0 + yy;
                if (!safeTile(wx, wy)) continue;

                const size_t li = static_cast<size_t>(yy * iw + xx);
                const float b = (li < B.size()) ? B[li] : 0.0f;
                float bn = (b - minB) / (maxB - minB);
                bn = clampf(bn, 0.0f, 1.0f);

                // Emphasize peaks so the spill has clear "hot" spots.
                bn = bn * bn;

                int s = static_cast<int>(bn * static_cast<float>(maxI));
                if (s < 2) continue;

                const uint8_t v = static_cast<uint8_t>(clampi(s, 0, 255));
                const size_t gi = idx(wx, wy);

                switch (theme) {
                    case ChemTheme::Noxious:
                        addField(confusionGas_, gi, v);
                        break;

                    case ChemTheme::Toxic:
                        addField(poisonGas_, gi, v);
                        break;

                    case ChemTheme::Acidic:
                        addField(corrosiveGas_, gi, v);
                        break;

                    case ChemTheme::Mixed:
                        // Concentrated acid cores with a toxic fringe that tends to react into confusion later.
                        addField(corrosiveGas_, gi, v);
                        if (bn > 0.28f && bn < 0.72f) {
                            const uint8_t pv = static_cast<uint8_t>(clampi(s - 1, 0, 255));
                            addField(poisonGas_, gi, pv);
                        }
                        break;

                    case ChemTheme::Volatile:
                        // Toxic vapor with occasional embers that can trigger flash ignition when dense.
                        addField(poisonGas_, gi, v);
                        if (bn > 0.78f) {
                            const uint8_t fv = static_cast<uint8_t>(clampi(6 + s / 2, 0, 255));
                            addField(fireField_, gi, fv);
                        }
                        break;

                    case ChemTheme::Adhesive:
                        // Polymer sludge clusters: cohesive sticky patches with a mildly toxic core.
                        addField(adhesiveFluid_, gi, static_cast<uint8_t>(clampi(s + ((bn > 0.60f) ? 3 : 0), 0, 255)));
                        if (bn > 0.86f) {
                            const uint8_t pv = static_cast<uint8_t>(clampi(2 + s / 3, 0, 255));
                            addField(poisonGas_, gi, pv);
                        }
                        break;
                }
            }
        }

        ++labsSeeded;
    }
}


// ----------------------------------------------------------------------------
// Field chemistry: laboratory doors with procedural seals
// ----------------------------------------------------------------------------

namespace {

// Identify the two "sides" of a door (the two opposite walkable tiles it connects).
// This is used for gas leakage / pressure puffs when doors open.
static bool doorOpposingSides(const Dungeon& d, Vec2i door, Vec2i& aOut, Vec2i& bOut) {
    auto walk = [&](int x, int y) -> bool {
        return d.inBounds(x, y) && d.isWalkable(x, y);
    };

    const bool ew = walk(door.x - 1, door.y) && walk(door.x + 1, door.y);
    const bool ns = walk(door.x, door.y - 1) && walk(door.x, door.y + 1);

    if (!ew && !ns) return false;

    // Most doors are unambiguous.
    if (ew && !ns) {
        aOut = {door.x - 1, door.y};
        bOut = {door.x + 1, door.y};
        return true;
    }
    if (ns && !ew) {
        aOut = {door.x, door.y - 1};
        bOut = {door.x, door.y + 1};
        return true;
    }

    // Rare ambiguous case (both pairs walkable): infer orientation by nearby blocking tiles.
    auto blocks = [&](int x, int y) -> bool {
        if (!d.inBounds(x, y)) return true;
        const TileType tt = d.at(x, y).type;
        return (tt == TileType::Wall || tt == TileType::Pillar || tt == TileType::DoorSecret);
    };

    const bool bu = blocks(door.x, door.y - 1);
    const bool bd = blocks(door.x, door.y + 1);
    const bool bl = blocks(door.x - 1, door.y);
    const bool br = blocks(door.x + 1, door.y);

    // If the door sits in a vertical wall (up+down blocked), it connects left-right.
    if (bu && bd && !bl && !br) {
        aOut = {door.x - 1, door.y};
        bOut = {door.x + 1, door.y};
        return true;
    }
    // If the door sits in a horizontal wall (left+right blocked), it connects up-down.
    if (bl && br && !bu && !bd) {
        aOut = {door.x, door.y - 1};
        bOut = {door.x, door.y + 1};
        return true;
    }

    // Fallback: prefer the pair that looks "more corridor-like".
    // (We bias toward the side tiles that themselves have fewer open neighbors.)
    static const std::array<Vec2i, 4> dirs = {{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
    auto openness = [&](Vec2i p) -> int {
        int s = 0;
        for (const auto& dxy : dirs) {
            if (walk(p.x + dxy.x, p.y + dxy.y)) s++;
        }
        return s;
    };

    const Vec2i aEW{door.x - 1, door.y};
    const Vec2i bEW{door.x + 1, door.y};
    const Vec2i aNS{door.x, door.y - 1};
    const Vec2i bNS{door.x, door.y + 1};

    const int openEW = openness(aEW) + openness(bEW);
    const int openNS = openness(aNS) + openness(bNS);

    if (openNS > openEW) {
        aOut = aNS;
        bOut = bNS;
    } else {
        aOut = aEW;
        bOut = bEW;
    }

    return true;
}

static Vec2i stepBeyond(Vec2i from, Vec2i toward) {
    return Vec2i{toward.x + (toward.x - from.x), toward.y + (toward.y - from.y)};
}

} // namespace

Game::DoorSealKind Game::doorSealKindAt(int x, int y) const {
    if (!dung.inBounds(x, y)) return DoorSealKind::Normal;

    const TileType tt = dung.at(x, y).type;
    if (tt != TileType::DoorClosed && tt != TileType::DoorLocked && tt != TileType::DoorOpen) {
        return DoorSealKind::Normal;
    }

    // Only special-case lab doors (keeps the rest of the game feeling familiar).
    Vec2i a{0, 0}, b{0, 0};
    if (!doorOpposingSides(dung, {x, y}, a, b)) return DoorSealKind::Normal;

    const RoomType ra = roomTypeAt(dung, a);
    const RoomType rb = roomTypeAt(dung, b);
    if (ra != RoomType::Laboratory && rb != RoomType::Laboratory) return DoorSealKind::Normal;

    // Deterministic per-level + per-position.
    uint32_t s = levelGenSeed(LevelId{branch_, depth_});
    s = hashCombine(s, 0xD005EA1u);
    s = hashCombine(s, static_cast<uint32_t>(x));
    s = hashCombine(s, static_cast<uint32_t>(y));

    const uint32_t h = hash32(s);
    const int roll = static_cast<int>(h % 100u);

    // Rough distribution for lab doors:
    // - Airlock: tight seal (no seepage while closed; strong pressure puff when opened).
    // - Vented: slow seepage even while closed.
    const int airlockPct = 20;
    const int ventedPct = 34;

    if (roll < airlockPct) return DoorSealKind::Airlock;
    if (roll < airlockPct + ventedPct) return DoorSealKind::Vented;
    return DoorSealKind::Normal;
}

void Game::onDoorOpened(Vec2i doorPos, bool openerIsPlayer) {
    if (!dung.inBounds(doorPos.x, doorPos.y)) return;

    // Find the two connected sides.
    Vec2i a{0, 0}, b{0, 0};
    if (!doorOpposingSides(dung, doorPos, a, b)) return;

    const int w = dung.width;
    const int h = dung.height;
    const size_t expect = static_cast<size_t>(w * h);
    if (expect == 0) return;

    // Ensure arrays exist (safety for older saves / edge cases).
    if (confusionGas_.size() != expect) confusionGas_.assign(expect, uint8_t{0});
    if (poisonGas_.size() != expect) poisonGas_.assign(expect, uint8_t{0});
    if (corrosiveGas_.size() != expect) corrosiveGas_.assign(expect, uint8_t{0});

    auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
    auto scoreAt = [&](Vec2i p) -> int {
        const size_t i = idx2(p.x, p.y);
        int s = 0;
        if (i < poisonGas_.size()) s += static_cast<int>(poisonGas_[i]);
        if (i < corrosiveGas_.size()) s += static_cast<int>(corrosiveGas_[i]);
        if (i < confusionGas_.size()) s += static_cast<int>(confusionGas_[i]) / 2; // magical haze is "lighter"
        return s;
    };

    int sa = scoreAt(a);
    int sb = scoreAt(b);
    if (sa == 0 && sb == 0) return;

    Vec2i src = a;
    Vec2i dst = b;
    int sSrc = sa;
    int sDst = sb;
    if (sb > sa) {
        src = b;
        dst = a;
        sSrc = sb;
        sDst = sa;
    }

    // If the sides are already similar, don't bother.
    if (sSrc - sDst < 6) return;

    const DoorSealKind seal = doorSealKindAt(doorPos.x, doorPos.y);
    float mult = 1.0f;
    int maxPuff = 28;
    if (seal == DoorSealKind::Airlock) {
        mult = 1.70f;
        maxPuff = 44;
    } else if (seal == DoorSealKind::Vented) {
        mult = 0.85f;
        maxPuff = 22;
    }

    auto inWalk = [&](Vec2i p) -> bool {
        return dung.inBounds(p.x, p.y) && dung.isWalkable(p.x, p.y);
    };

    auto puffOne = [&](std::vector<uint8_t>& f, int minDiff, int baseMax) -> int {
        if (f.size() != expect) return 0;

        const size_t iSrc = idx2(src.x, src.y);
        const size_t iDst = idx2(dst.x, dst.y);
        const size_t iDoor = idx2(doorPos.x, doorPos.y);

        const int vSrc = static_cast<int>(f[iSrc]);
        const int vDst = static_cast<int>(f[iDst]);
        if (vSrc <= vDst + minDiff) return 0;

        const int diff = vSrc - vDst;
        int want = static_cast<int>(static_cast<float>(diff) * 0.45f * mult);
        want = std::clamp(want, 1, baseMax);

        // Pull volume from the source side (and one tile deeper if available).
        Vec2i src2 = stepBeyond(doorPos, src);
        const bool hasSrc2 = inWalk(src2);
        const size_t iSrc2 = hasSrc2 ? idx2(src2.x, src2.y) : iSrc;

        int pulled = 0;
        int rem = want;

        const int take0 = std::min(rem, static_cast<int>(f[iSrc]));
        if (take0 > 0) {
            f[iSrc] = static_cast<uint8_t>(static_cast<int>(f[iSrc]) - take0);
            pulled += take0;
            rem -= take0;
        }

        if (rem > 0 && hasSrc2) {
            const int take1 = std::min(rem, static_cast<int>(f[iSrc2]));
            if (take1 > 0) {
                f[iSrc2] = static_cast<uint8_t>(static_cast<int>(f[iSrc2]) - take1);
                pulled += take1;
                rem -= take1;
            }
        }

        if (pulled <= 0) return 0;

        // Distribute into the doorway + destination side.
        Vec2i dst2 = stepBeyond(doorPos, dst);
        const bool hasDst2 = inWalk(dst2);
        const size_t iDst2 = hasDst2 ? idx2(dst2.x, dst2.y) : iDst;

        const int toDoor = pulled / 3; // ~33%
        const int toSide = pulled - toDoor;
        const int toSide2 = hasDst2 ? (toSide / 2) : 0;
        const int toSide1 = toSide - toSide2;

        auto add = [&](size_t i, int amt) {
            if (amt <= 0) return;
            const int nv = std::clamp(static_cast<int>(f[i]) + amt, 0, 255);
            f[i] = static_cast<uint8_t>(nv);
        };

        add(iDoor, toDoor);
        add(iDst, toSide1);
        if (toSide2 > 0) add(iDst2, toSide2);

        return pulled;
    };

    const int movedPoison = puffOne(poisonGas_, 6, maxPuff);
    const int movedCorrosive = puffOne(corrosiveGas_, 6, maxPuff);
    const int movedConfusion = puffOne(confusionGas_, 6, maxPuff);

    const int movedTotal = movedPoison + movedCorrosive + movedConfusion;
    if (movedTotal <= 0) return;

    // Message if the event is relevant to the player.
    bool relevant = openerIsPlayer;
    if (!relevant) {
        if (dung.inBounds(doorPos.x, doorPos.y) && dung.at(doorPos.x, doorPos.y).visible) relevant = true;
        const Vec2i pp = player().pos;
        const int dx = std::abs(pp.x - doorPos.x);
        const int dy = std::abs(pp.y - doorPos.y);
        if (std::max(dx, dy) <= 1) relevant = true;
    }

    if (relevant && movedTotal >= 12) {
        std::string msg;
        if (seal == DoorSealKind::Airlock) {
            msg = "THE AIRLOCK WHOOSHES OPEN! ";
        }

        if (movedCorrosive >= movedPoison && movedCorrosive >= movedConfusion) {
            msg += "CORROSIVE FUMES BURST OUT!";
        } else if (movedPoison >= movedConfusion) {
            msg += "TOXIC VAPORS POUR OUT!";
        } else {
            msg += "A STRANGE VAPOR SWIRLS OUT!";
        }

        pushMsg(msg, MessageKind::Warning, false);
    }
}

void Game::applyEndOfTurnEffects() {
    if (gameOver) return;

    Entity& p = playerMut();

    const OverworldWeatherFx wx = overworldWeatherFx();

    // Per-level wind: biases drifting hazards (gas, fire). Deterministic from run seed + level id.
    // Overworld wilderness chunks override this with their weather wind.
    const Vec2i wind = wx.active ? wx.wind : windDir();
    const int windStr = wx.active ? wx.windStrength : windStrength();
    const Vec2i upWind = {-wind.x, -wind.y};

    // Overworld weather modifiers.
    const int wxFireQuench = wx.active ? wx.fireQuench : 0;
    const int wxBurnQuench = wx.active ? wx.burnQuench : 0;

    // Ensure the terrain material cache is populated for this floor so the
    // hazard simulation can query materialAtCached() cheaply and deterministically.
    dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());

    // Substrate chemistry helpers: porous materials absorb fumes; smooth sealed
    // surfaces let vapors drift a little farther.
    auto gasAbsorb = [&](TerrainMaterial m) -> int {
        switch (m) {
            case TerrainMaterial::Moss:
            case TerrainMaterial::Dirt:
            case TerrainMaterial::Wood:
            case TerrainMaterial::Bone:
                return 1;
            default:
                return 0;
        }
    };

    auto gasSlick = [&](TerrainMaterial m) -> int {
        switch (m) {
            case TerrainMaterial::Metal:
            case TerrainMaterial::Crystal:
            case TerrainMaterial::Obsidian:
            case TerrainMaterial::Marble:
                return 1;
            default:
                return 0;
        }
    };


    // ------------------------------------------------------------
    // Field chemistry: fire / gas reactions
    //
    // Fire can burn away lingering gas clouds, and dense poison vapors
    // can occasionally ignite into a brief flash-fire explosion. This
    // adds emergent interactions between hazards without introducing
    // a separate simulation system (it operates directly on existing
    // per-tile hazard fields).
    // ------------------------------------------------------------
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (expect > 0) {
            if (confusionGas_.size() != expect) confusionGas_.assign(expect, uint8_t{0});
            if (poisonGas_.size() != expect) poisonGas_.assign(expect, uint8_t{0});
            if (corrosiveGas_.size() != expect) corrosiveGas_.assign(expect, uint8_t{0});
            if (fireField_.size() != expect) fireField_.assign(expect, uint8_t{0});
            if (adhesiveFluid_.size() != expect) adhesiveFluid_.assign(expect, uint8_t{0});
        }

        // ------------------------------------------------------------
        // Ecosystem pulses: periodic, deterministic biome events
        //
        // These inject small localized hazard/boon "pulses" near ecosystem cores.
        // They are RNG-isolated (hash-derived) and keyed off turnCount so they
        // remain stable across save/load and don't perturb the main RNG stream.
        // ------------------------------------------------------------
        if (branch_ == DungeonBranch::Main) {
            const auto& ecoSeeds = dung.ecosystemSeedsCached();
            if (!ecoSeeds.empty()) {
                std::vector<size_t> candidates;
                candidates.reserve(ecoSeeds.size());
                for (size_t i = 0; i < ecoSeeds.size(); ++i) {
                    const EcosystemSeed& s = ecoSeeds[i];
                    if (s.kind == EcosystemKind::None) continue;
                    if (s.radius <= 0) continue;
                    if (!dung.inBounds(s.pos.x, s.pos.y)) continue;
                    candidates.push_back(i);
                }

                if (!candidates.empty()) {
                    const int md = materialDepth();
                    int interval = 34 - std::min(12, std::max(0, md) / 2);
                    interval = clampi(interval, 22, 34);

                    uint32_t base = hashCombine(materialWorldSeed(), "ECO_PULSE"_tag);
                    base = hashCombine(base, static_cast<uint32_t>(md));
                    base = hashCombine(base, static_cast<uint32_t>(dungeonMaxDepth()));
                    base = hashCombine(base, static_cast<uint32_t>(branch_));

                    const uint32_t uInterval = static_cast<uint32_t>(std::max(1, interval));
                    const uint32_t phase = base % uInterval;

                    if (((turnCount + phase) % uInterval) == 0u) {
                        const uint32_t pulseIdx = (turnCount + phase) / uInterval;

                        const Vec2i pp = p.pos;

                        uint32_t h = hashCombine(base, pulseIdx);
                        const size_t pick = candidates[static_cast<size_t>(h % static_cast<uint32_t>(candidates.size()))];
                        const EcosystemSeed& s = ecoSeeds[pick];

                        RNG prng(hashCombine(h, static_cast<uint32_t>(pick)));

                        // Choose a pulse center within the seed radius, on a walkable tile of the same ecosystem.
                        Vec2i center{-1, -1};
                        const int maxOff = clampi(std::max(2, s.radius / 3), 2, 6);

                        for (int attempt = 0; attempt < 28; ++attempt) {
                            const int ox = prng.range(-maxOff, maxOff);
                            const int oy = prng.range(-maxOff, maxOff);
                            const Vec2i cand{ s.pos.x + ox, s.pos.y + oy };

                            if (!dung.inBounds(cand.x, cand.y)) continue;
                            if (cand.x == pp.x && cand.y == pp.y) continue;
                            if (cand == dung.stairsUp || cand == dung.stairsDown) continue;

                            // Prefer pulses on solid ground, but allow gas/fire to drift over chasms after emission.
                            if (!dung.isWalkable(cand.x, cand.y)) continue;

                            if (dung.ecosystemAtCached(cand.x, cand.y) != s.kind) continue;

                            // Avoid spawning pulses directly under another entity (especially shopkeepers).
                            if (const Entity* occ = entityAt(cand.x, cand.y)) {
                                if (occ->id != playerId_) continue;
                            }

                            center = cand;
                            break;
                        }

                        if (center.x >= 0) {
                            auto addToField = [&](std::vector<uint8_t>& field, int x, int y, int add) {
                                if (!dung.inBounds(x, y)) return;
                                const size_t i = static_cast<size_t>(y * dung.width + x);
                                if (i >= field.size()) return;
                                const int nv = static_cast<int>(field[i]) + add;
                                field[i] = static_cast<uint8_t>(clampi(nv, 0, 255));
                            };

                            auto subFromField = [&](std::vector<uint8_t>& field, int x, int y, int sub) {
                                if (!dung.inBounds(x, y)) return;
                                const size_t i = static_cast<size_t>(y * dung.width + x);
                                if (i >= field.size()) return;
                                const int nv = static_cast<int>(field[i]) - sub;
                                field[i] = static_cast<uint8_t>(clampi(nv, 0, 255));
                            };

                            auto bloom = [&](std::vector<uint8_t>& field, Vec2i c, int radius, int peak, bool requireWalkable) {
                                radius = clampi(radius, 0, 12);
                                peak = clampi(peak, 0, 255);

                                const int fall = std::max(1, peak / std::max(1, radius + 1));

                                for (int dy = -radius; dy <= radius; ++dy) {
                                    for (int dx = -radius; dx <= radius; ++dx) {
                                        const int x = c.x + dx;
                                        const int y = c.y + dy;
                                        if (!dung.inBounds(x, y)) continue;
                                        if (x == pp.x && y == pp.y) continue;

                                        if (requireWalkable) {
                                            // Allow fields to be *applied* to walkable tiles only; they can still drift
                                            // over pits later via the normal hazard simulation.
                                            if (!dung.isWalkable(x, y)) continue;
                                        }

                                        const int dist = std::max(std::abs(dx), std::abs(dy));
                                        int add = peak - dist * fall;
                                        if (add <= 0) continue;
                                        addToField(field, x, y, add);
                                    }
                                }
                            };

                            // Mild depth scaling; keep pulses flavorful, not a global difficulty spike.
                            const int depthN = std::max(1, md);
                            const bool vis = dung.inBounds(center.x, center.y) && dung.at(center.x, center.y).visible;
                            const int near = chebyshev(pp, center);

                            std::string msg;
                            MessageKind mk = MessageKind::Info;
                            bool fromPlayer = false;

                            switch (s.kind) {
                                case EcosystemKind::FungalBloom: {
                                    const int radius = 2 + ((s.radius >= 10) ? 1 : 0);
                                    int peak = 10 + depthN / 3;
                                    peak = clampi(peak, 10, 22);
                                    bloom(confusionGas_, center, radius, peak, /*requireWalkable=*/true);

                                    if (vis) {
                                        pushFxParticle(FXParticlePreset::Detect, center, 18, 0.22f, 0.0f, hashCombine(h, "SPORE"_tag));
                                    }
                                    if (vis || near <= 6) {
                                        msg = "SPORES BURST FROM THE FUNGAL GROWTH!";
                                        mk = MessageKind::Warning;
                                    }
                                    break;
                                }
                                case EcosystemKind::RustVeins: {
                                    const int radius = 2;
                                    int peak = 10 + depthN / 4;
                                    peak = clampi(peak, 10, 20);
                                    bloom(corrosiveGas_, center, radius, peak, /*requireWalkable=*/true);

                                    if (vis) {
                                        pushFxParticle(FXParticlePreset::Poison, center, 16, 0.20f, 0.0f, hashCombine(h, "RUST"_tag));
                                    }
                                    if (vis || near <= 6) {
                                        msg = "ACRID VAPORS SEEP FROM THE RUST VEINS!";
                                        mk = MessageKind::Warning;
                                    }
                                    break;
                                }
                                case EcosystemKind::AshenRidge: {
                                    const int radius = 1 + ((depthN >= 10) ? 1 : 0);
                                    int peak = 10 + depthN / 5;
                                    peak = clampi(peak, 10, 22);
                                    bloom(fireField_, center, radius, peak, /*requireWalkable=*/true);

                                    // Small smoke-like confusion fringe at higher depths.
                                    if (depthN >= 12) {
                                        bloom(confusionGas_, center, 1, 6 + depthN / 6, /*requireWalkable=*/true);
                                    }

                                    if (vis) {
                                        pushFxParticle(FXParticlePreset::EmberBurst, center, 20, 0.18f, 0.0f, hashCombine(h, "EMBER"_tag));
                                    }
                                    if (vis || near <= 6) {
                                        msg = "EMBERS ERUPT FROM A SMOLDERING FISSURE!";
                                        mk = MessageKind::Warning;
                                    }
                                    break;
                                }
                                case EcosystemKind::FloodedGrotto: {
                                    // Cool mist: gently dampens nearby fire and can ease burning.
                                    const int radius = 3;
                                    const int quench = 10 + depthN / 6;
                                    for (int dy = -radius; dy <= radius; ++dy) {
                                        for (int dx = -radius; dx <= radius; ++dx) {
                                            const int x = center.x + dx;
                                            const int y = center.y + dy;
                                            if (!dung.inBounds(x, y)) continue;
                                            const int dist = std::max(std::abs(dx), std::abs(dy));
                                            if (dist > radius) continue;
                                            const int sub = std::max(0, quench - dist * 3);
                                            if (sub <= 0) continue;
                                            subFromField(fireField_, x, y, sub);
                                        }
                                    }

                                    if (near <= 5 && p.effects.burnTurns > 0) {
                                        const int before = p.effects.burnTurns;
                                        p.effects.burnTurns = std::max(0, p.effects.burnTurns - 2);
                                        if (before > 0 && p.effects.burnTurns == 0) {
                                            pushMsg(effectEndMessage(EffectKind::Burn), MessageKind::System, true);
                                        }
                                    }

                                    // Drips echo: a small audible cue that can also attract monsters.
                                    emitNoise(center, 10 + std::min(8, depthN));

                                    if (vis) {
                                        pushFxParticle(FXParticlePreset::Detect, center, 14, 0.22f, 0.0f, hashCombine(h, "DRIP"_tag));
                                    }
                                    if (vis || near <= 6) {
                                        msg = "COOL MIST RISES FROM THE GROTTO.";
                                        mk = MessageKind::Info;
                                    }
                                    break;
                                }
                                case EcosystemKind::CrystalGarden: {
                                    // Resonance: crystals chime, sometimes restoring a bit of mana if you're nearby.
                                    emitNoise(center, 12 + std::min(10, depthN));

                                    const int manaMax = playerManaMax();
                                    if (near <= 6 && manaMax > 0 && mana_ < manaMax) {
                                        const int before = mana_;
                                        const int gain = 1 + ((depthN >= 12) ? 1 : 0);
                                        mana_ = std::min(manaMax, mana_ + gain);

                                        if (mana_ > before) {
                                            msg = "THE CRYSTALS HUM WITH ARCANE POWER. YOU FEEL ENERGIZED.";
                                            mk = MessageKind::Success;
                                            fromPlayer = true;
                                        }
                                    }

                                    if (vis) {
                                        pushFxParticle(FXParticlePreset::Buff, center, 18, 0.22f, 0.0f, hashCombine(h, "CHIME"_tag));
                                    }
                                    if (msg.empty() && (vis || near <= 6)) {
                                        msg = "THE CRYSTALS RING SOFTLY.";
                                        mk = MessageKind::Info;
                                    }
                                    break;
                                }
                                case EcosystemKind::BoneField: {
                                    // Necrotic haze: a mild toxic puff (flavor + small hazard).
                                    const int radius = 2;
                                    int peak = 9 + depthN / 4;
                                    peak = clampi(peak, 9, 18);
                                    bloom(poisonGas_, center, radius, peak, /*requireWalkable=*/true);

                                    if (vis) {
                                        pushFxParticle(FXParticlePreset::Poison, center, 14, 0.20f, 0.0f, hashCombine(h, "BONE"_tag));
                                    }
                                    if (vis || near <= 6) {
                                        msg = "A FOUL MIASMA RISES FROM THE OSSUARY.";
                                        mk = MessageKind::Warning;
                                    }
                                    break;
                                }
                                default:
                                    break;
                            }

                            if (!msg.empty()) {
                                pushMsg(msg, mk, fromPlayer);
                            }
                        }
                    }
                }
            }
        }

        // Only do any work if there is any overlap potential.
        {
            const int w = dung.width;
            const int h = dung.height;
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };

            // Avoid runaway chain reactions.
            constexpr int MAX_IGNITIONS = 4;

            int ignitions = 0;
            bool anyVisible = false;
            bool playerHit = false;

            // Pass 1: fire burns away gas in place; dense poison gas can ignite.
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    const uint8_t f = fireField_[i];
                    if (f == 0u) continue;

                    const uint8_t aPre = (i < corrosiveGas_.size()) ? corrosiveGas_[i] : 0u;

                    // Confusion gas is not meant to be explosive; fire simply
                    // cleans it up a bit.
                    if (i < confusionGas_.size() && confusionGas_[i] > 0u) {
                        const uint8_t g = confusionGas_[i];
                        const int burn = 1 + static_cast<int>(f) / 6;
                        confusionGas_[i] = (g > static_cast<uint8_t>(burn)) ? static_cast<uint8_t>(g - static_cast<uint8_t>(burn)) : 0u;
                    }

                    // Poison vapors are combustible: fire consumes them quickly.
                    uint8_t gPre = 0u;
                    if (i < poisonGas_.size()) {
                        uint8_t g = poisonGas_[i];
                        gPre = g;

                        if (g > 0u) {
                            const int burn = 2 + static_cast<int>(f) / 4;
                            g = (g > static_cast<uint8_t>(burn)) ? static_cast<uint8_t>(g - static_cast<uint8_t>(burn)) : 0u;
                            poisonGas_[i] = g;

                            // A little extra flame when vapor burns.
                            if (g > 0u) {
                                const uint8_t boosted = static_cast<uint8_t>(std::min(255, std::max<int>(static_cast<int>(f), static_cast<int>(g) + 2)));
                                fireField_[i] = boosted;
                            }

                            // Rare flash ignition (dense gas + strong flame).
                            if (ignitions < MAX_IGNITIONS) {
                                // We base the ignition chance on the *pre-burn* gas level to
                                // keep it intuitive: fresh, dense gas clouds are the risk.
                                const uint8_t g0 = gPre;
                                if (f >= 9u && g0 >= 10u) {
                                    float chance = 0.10f;
                                    chance += 0.02f * static_cast<float>(g0 - 10u);
                                    chance += 0.015f * static_cast<float>(f - 9u);
                                    chance = std::min(0.28f, std::max(0.0f, chance));

                                    if (rng.chance(chance)) {
                                        ++ignitions;

                                        const int radius = (g0 >= 12u && rng.chance(0.25f)) ? 2 : 1;

                                        std::vector<uint8_t> mask;
                                        dung.computeFovMask(x, y, radius, mask);

                                        const int minX = std::max(0, x - radius);
                                        const int maxX = std::min(w - 1, x + radius);
                                        const int minY = std::max(0, y - radius);
                                        const int maxY = std::min(h - 1, y + radius);

                                        // A flash fire is loud.
                                        emitNoise({x, y}, 16);

                                        for (int yy = minY; yy <= maxY; ++yy) {
                                            for (int xx = minX; xx <= maxX; ++xx) {
                                                const size_t j = idx2(xx, yy);
                                                if (j >= mask.size() || mask[j] == 0u) continue;

                                                // Consume poison gas in the blast.
                                                if (j < poisonGas_.size()) poisonGas_[j] = 0u;

                                                // Fire lingers in the blast area on walkable tiles.
                                                if (dung.isWalkable(xx, yy)) {
                                                    const int dist = std::max(std::abs(xx - x), std::abs(yy - y));
                                                    const int base = 10 + static_cast<int>(g0) / 2 + static_cast<int>(f) / 2;
                                                    const int s = std::max(2, base - dist * 3);
                                                    const uint8_t su = static_cast<uint8_t>(clampi(s, 0, 255));
                                                    if (j < fireField_.size() && fireField_[j] < su) fireField_[j] = su;
                                                }

                                                // Damage entities caught in the blast; also ignite them.
                                                if (Entity* e = entityAtMut(xx, yy)) {
                                                    if (e->hp > 0) {
                                                        const int dist = std::max(std::abs(xx - x), std::abs(yy - y));
                                                        int dmg = rng.range(2, 4) + static_cast<int>(g0) / 6 + static_cast<int>(f) / 8;
                                                        dmg = std::max(0, dmg - dist);

                                                        if (dmg > 0) {
                                                            e->hp -= dmg;
                                                            const bool vis = (dung.inBounds(xx, yy) && dung.at(xx, yy).visible);
                                                            if (e->id == playerId_) {
                                                                playerHit = true;
                                                                if (e->hp <= 0) {
                                                                    pushMsg("YOU ARE INCINERATED BY IGNITING VAPORS.", MessageKind::Combat, false);
                                                                    if (endCause_.empty()) endCause_ = "INCINERATED BY IGNITING VAPORS";
                                                                    gameOver = true;
                                                                    return;
                                                                }
                                                            } else if (vis && e->hp <= 0) {
                                                                std::ostringstream ss;
                                                                ss << kindName(e->kind) << " IS INCINERATED.";
                                                                pushMsg(ss.str(), MessageKind::Combat, false);
                                                            }
                                                        }

                                                        const int burnTurns = clampi(2 + static_cast<int>(g0) / 4, 2, 10);
                                                        if (e->effects.burnTurns < burnTurns) e->effects.burnTurns = burnTurns;
                                                    }
                                                }
                                            }
                                        }

                                        if (dung.inBounds(x, y) && dung.at(x, y).visible) anyVisible = true;
                                    }
                                }
                            }
                        }
                    }

                    // Corrosive vapors are not explosive, but heat can aerosolize them into
                    // acrid smoke and slightly quench flames.
                    if (aPre > 0u && i < corrosiveGas_.size()) {
                        const int burn = 1 + static_cast<int>(f) / 7;
                        const uint8_t b = static_cast<uint8_t>(burn);
                        corrosiveGas_[i] = (aPre > b) ? static_cast<uint8_t>(aPre - b) : 0u;

                        // Dense acid + open flame -> brief toxic smoke (adds poison gas after the burn step).
                        if (aPre >= 10u && f >= 8u && i < poisonGas_.size()) {
                            const int add = 1 + static_cast<int>(aPre) / 7;
                            const int nv = static_cast<int>(poisonGas_[i]) + add;
                            poisonGas_[i] = static_cast<uint8_t>(clampi(nv, 0, 255));
                        }

                        // Acid slightly damps flames when dense.
                        if (aPre >= 12u && i < fireField_.size()) {
                            if ((turnCount & 1u) == 0u && fireField_[i] > 0u) {
                                fireField_[i] = static_cast<uint8_t>(fireField_[i] - 1u);
                            }
                        }
                    }
                }
            }

            if (ignitions > 0 && (anyVisible || playerHit)) {
                pushMsg("TOXIC VAPORS IGNITE!", MessageKind::Warning, playerHit);
            }

            // Pass 2: corrosive + poison can react into an irritant haze.
            // (This is non-explosive; it mostly converts some of the mixture into confusion gas.)
            constexpr int MAX_MIX_MSGS = 6;
            int strongMixes = 0;
            bool mixVisible = false;
            bool playerMixed = false;

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    if (i >= poisonGas_.size() || i >= corrosiveGas_.size() || i >= confusionGas_.size()) continue;

                    const uint8_t pg = poisonGas_[i];
                    const uint8_t ag = corrosiveGas_[i];
                    if (pg == 0u || ag == 0u) continue;

                    const int m = (pg < ag) ? static_cast<int>(pg) : static_cast<int>(ag);
                    if (m < 4) continue;

                    int react = 1 + m / 8;
                    react = clampi(react, 1, 4);

                    poisonGas_[i] = (pg > static_cast<uint8_t>(react)) ? static_cast<uint8_t>(pg - static_cast<uint8_t>(react)) : 0u;
                    corrosiveGas_[i] = (ag > static_cast<uint8_t>(react)) ? static_cast<uint8_t>(ag - static_cast<uint8_t>(react)) : 0u;

                    const int add = react + m / 10;
                    const int nv = static_cast<int>(confusionGas_[i]) + add;
                    confusionGas_[i] = static_cast<uint8_t>(clampi(nv, 0, 255));

                    // Only message on strong reactions in view to avoid spam.
                    if (m >= 10 && strongMixes < MAX_MIX_MSGS) {
                        ++strongMixes;
                        if (dung.inBounds(x, y) && dung.at(x, y).visible) mixVisible = true;
                        if (p.pos.x == x && p.pos.y == y) playerMixed = true;
                    }
                }
            }

            if (strongMixes > 0 && (mixVisible || playerMixed)) {
                pushMsg("CHEMICAL FUMES REACT!", MessageKind::Warning, playerMixed);
            }
        }

    }

    // ------------------------------------------------------------
    // Environmental fields: Confusion Gas (persistent, tile-based)
    //
    // The gas itself is stored as an intensity map (0..255). Entities standing
    // in gas have their confusion duration "topped up" each turn.
    // ------------------------------------------------------------
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (confusionGas_.size() != expect) confusionGas_.assign(expect, uint8_t{0});

        auto gasIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto gasAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return uint8_t{0};
            const size_t i = gasIdx(x, y);
            if (i >= confusionGas_.size()) return uint8_t{0};
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
        if (poisonGas_.size() != expect) poisonGas_.assign(expect, uint8_t{0});

        auto gasIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto gasAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return uint8_t{0};
            const size_t i = gasIdx(x, y);
            if (i >= poisonGas_.size()) return uint8_t{0};
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
    // Environmental fields: Corrosive Gas (persistent, tile-based)
    //
    // Corrosive vapors are stored as an intensity map (0..255). Entities standing
    // in vapor have their corrosion duration "topped up" each turn.
    // ------------------------------------------------------------
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (corrosiveGas_.size() != expect) corrosiveGas_.assign(expect, uint8_t{0});

        auto gasIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto gasAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return uint8_t{0};
            const size_t i = gasIdx(x, y);
            if (i >= corrosiveGas_.size()) return uint8_t{0};
            return corrosiveGas_[i];
        };

        auto applyGasTo = [&](Entity& e, bool isPlayer) {
            const uint8_t g = gasAt(e.pos.x, e.pos.y);
            if (g == 0u) return;

            // Corrosive gas is slightly "heavier" than poison: shorter, sharper exposure.
            int minTurns = 2 + static_cast<int>(g) / 3;
            minTurns = clampi(minTurns, 2, 8);

            const int before = e.effects.corrosionTurns;
            if (before < minTurns) e.effects.corrosionTurns = minTurns;

            // Message only on first exposure (avoids log spam while standing in vapor).
            if (before == 0 && e.effects.corrosionTurns > 0) {
                if (isPlayer) {
                    pushMsg("ACRID VAPORS BURN YOUR SKIN!", MessageKind::Warning, true);
                } else if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                    std::ostringstream ss;
                    ss << kindName(e.kind) << " IS SPLASHED BY ACRID VAPORS!";
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
        if (fireField_.size() != expect) fireField_.assign(expect, uint8_t{0});

        auto fireIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto fireAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return uint8_t{0};
            const size_t i = fireIdx(x, y);
            if (i >= fireField_.size()) return uint8_t{0};
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

    // ------------------------------------------------------------
    // Environmental fields: Adhesive fluid (persistent, tile-based)
    //
    // Sticky ooze is stored as an intensity map (0..255). Higher intensities
    // can briefly ensnare entities standing in it.
    // ------------------------------------------------------------
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (adhesiveFluid_.size() != expect) adhesiveFluid_.assign(expect, uint8_t{0});

        auto fluidIdx = [&](int x, int y) -> size_t {
            return static_cast<size_t>(y * dung.width + x);
        };
        auto fluidAt = [&](int x, int y) -> uint8_t {
            if (!dung.inBounds(x, y)) return uint8_t{0};
            const size_t i = fluidIdx(x, y);
            if (i >= adhesiveFluid_.size()) return uint8_t{0};
            return adhesiveFluid_[i];
        };

        auto applyFluidTo = [&](Entity& e, bool isPlayer) {
            const uint8_t a = fluidAt(e.pos.x, e.pos.y);
            if (a < 8u) return;

            // Only stronger concentrations impose brief movement friction.
            if (a >= 20u) {
                const int before = e.effects.webTurns;
                // Important: only apply while currently unwebbed.
                // Reapplying every end-of-turn can lock entities indefinitely.
                if (before <= 0) {
                    int minTurns = 1 + static_cast<int>(a) / 64;
                    minTurns = clampi(minTurns, 1, 3);
                    e.effects.webTurns = std::max(e.effects.webTurns, minTurns);

                    if (e.effects.webTurns > 0) {
                        if (isPlayer) {
                            pushMsg("YOU'RE BOGGED DOWN BY STICKY OOZE!", MessageKind::Warning, true);
                        } else if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                            std::ostringstream ss;
                            ss << kindName(e.kind) << " GETS BOGGED DOWN!";
                            pushMsg(ss.str(), MessageKind::Info, false);
                        }
                    }
                }
            } else if (isPlayer && (turnCount % 6u) == 0u) {
                // Low-intensity feedback (throttled) so players can read the field.
                pushMsg("YOUR BOOTS DRAG THROUGH STICKY SLIME.", MessageKind::System, true);
            }
        };

        applyFluidTo(p, true);
        for (auto& m : ents) {
            if (m.id == playerId_) continue;
            if (m.hp <= 0) continue;
            applyFluidTo(m, false);
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
        const int burnDecay = 1 + wxBurnQuench;
        p.effects.burnTurns = std::max(0, p.effects.burnTurns - burnDecay);
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

    // Corrosion: stinging damage over time + defense penalty while active.
    if (p.effects.corrosionTurns > 0) {
        p.effects.corrosionTurns = std::max(0, p.effects.corrosionTurns - 1);

        // Corrosion is intentionally a little slower than poison/burn.
        // We key the tick off turnCount so it's deterministic across save/load.
        if ((turnCount & 1u) == 0u) {
            p.hp -= 1;
            if (p.hp <= 0) {
                pushMsg("YOU ARE DISSOLVED BY CORROSIVE VAPORS.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = "DISSOLVED BY CORROSIVE VAPORS";
                gameOver = true;
                return;
            }

            // Secondary effect: acid can pit exposed equipment, reducing enchantment.
            // Shielding acts like a barrier against gear damage.
            if (p.effects.shieldTurns <= 0) {
                uint8_t g = 0u;
                if (dung.inBounds(p.pos.x, p.pos.y) && !corrosiveGas_.empty()) {
                    const size_t gi = static_cast<size_t>(p.pos.y * dung.width + p.pos.x);
                    if (gi < corrosiveGas_.size()) g = corrosiveGas_[gi];
                }

                // Consider equipped gear (armor / melee / ranged).
                std::array<int, 3> slots = {equippedArmorIndex(), equippedMeleeIndex(), equippedRangedIndex()};
                std::array<int, 3> picks = {-1, -1, -1};
                int n = 0;
                for (int idx : slots) {
                    if (idx < 0) continue;
                    if (idx >= static_cast<int>(inv.size())) continue;
                    const Item& it = inv[static_cast<size_t>(idx)];
                    if (!(isArmor(it.kind) || isWeapon(it.kind))) continue;
                    if (itemIsArtifact(it)) continue;
                    picks[n++] = idx;
                }

                if (n > 0) {
                    const int idx = picks[static_cast<size_t>(rng.range(0, n - 1))];
                    Item& it = inv[static_cast<size_t>(idx)];

                    int chancePct = 12 + p.effects.corrosionTurns * 3 + static_cast<int>(g) / 12;
                    chancePct = clampi(chancePct, 8, 60);

                    // Blessed gear resists; cursed gear suffers.
                    if (it.buc > 0) chancePct = std::max(0, chancePct - 10);
                    else if (it.buc < 0) chancePct = std::min(90, chancePct + 10);

                    if (chancePct > 0 && rng.range(1, 100) <= chancePct) {
                        const int before = it.enchant;
                        const int after = std::max(-3, before - 1);
                        if (after != before) {
                            const std::string nm = itemDisplayName(it);
                            it.enchant = after;
                            std::ostringstream ss;
                            ss << "YOUR " << nm << " CORRODES!";
                            pushMsg(ss.str(), MessageKind::Warning, true);
                        }
                    }
                }
            }
        }

        if (p.effects.corrosionTurns == 0) {
            pushMsg(effectEndMessage(EffectKind::Corrosion), MessageKind::System, true);
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

    // Parry stance: improves defense briefly; expires at end of turn if not consumed.
    if (p.effects.parryTurns > 0) {
        p.effects.parryTurns = std::max(0, p.effects.parryTurns - 1);
        if (p.effects.parryTurns == 0) {
            pushMsg(effectEndMessage(EffectKind::Parry), MessageKind::System, true);
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
    // Intentionally disabled while taking sustained damage to keep DOT hazards meaningful.
    if (p.effects.poisonTurns > 0 || p.effects.burnTurns > 0 || p.effects.corrosionTurns > 0 || p.hp >= p.hpMax) {
        naturalRegenCounter = 0;
    } else if (p.effects.regenTurns <= 0) {
        // Faster natural regen as you level.
        // VIGOR bonuses from rings/artifacts now matter immediately (not just on level-up).
        // Cursed vigor penalties can also slow healing, but we clamp the impact.
        const int vigorBonus = clampi(playerVigor(), -2, 4);
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

            // Round 211: procedural leylines (arcane resonance) can nudge mana regen.
            // This is kept deterministic by keying any bonus purely off (turnCount, tile intensity).
            uint8_t ley = 0u;
            if (branch_ != DungeonBranch::Camp) {
                ley = dung.leylineAt(p.pos.x, p.pos.y,
                                     materialWorldSeed(),
                                     branch_,
                                     materialDepth(),
                                     dungeonMaxDepth());
            }

            int intervalDelta = 0;
            if (ley >= 220u) intervalDelta = 3;
            else if (ley >= 170u) intervalDelta = 2;
            else if (ley >= 120u) intervalDelta = 1;

            interval = clampi(interval - intervalDelta, 1, 12);
            if (interval <= 0) interval = 1;

            if ((turnCount % static_cast<uint32_t>(interval)) == 0u) {
                int gain = 1;
                // Very strong currents occasionally grant an extra tick.
                if (ley >= 235u) {
                    if ((turnCount % 5u) == 0u) gain += 1;
                } else if (ley >= 220u) {
                    if ((turnCount % 9u) == 0u) gain += 1;
                }
                mana_ = std::min(manaMax, mana_ + gain);
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

        int burntMobVis = 0;
        for (auto& e : ents) {
            if (e.id == playerId_) continue;
            if (e.hp <= 0) continue;

            Item& pc = e.pocketConsumable;
            if (pc.id == 0 || pc.count <= 0) continue;
            if (pc.kind != ItemKind::TorchLit) continue;

            if (pc.charges > 0) pc.charges -= 1;
            if (pc.charges <= 0) {
                if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) {
                    ++burntMobVis;
                }
                // Clear the pocket slot.
                pc = Item{};
            }
        }
        if (burntMobVis > 0) {
            pushMsg(burntMobVis == 1 ? "A MOVING TORCH FLICKERS OUT." : "SOME MOVING TORCHES FLICKER OUT.", MessageKind::System, true);
        }
    }

    // Corpses (and butchered meat) rot away
    {
        int rottedInvCorpses = 0;
        int rottedInvMeat = 0;

        // Corpses/meat in inventory
        for (size_t i = 0; i < inv.size();) {
            Item& it = inv[i];
            if (isCorpseKind(it.kind) || it.kind == ItemKind::ButcheredMeat) {
                if (it.charges > 0) it.charges--;
                if (it.charges <= 0) {
                    if (isCorpseKind(it.kind)) ++rottedInvCorpses;
                    else ++rottedInvMeat;
                    inv.erase(inv.begin() + static_cast<std::vector<Item>::difference_type>(i));
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        }

        if (rottedInvCorpses > 0) {
            pushMsg(rottedInvCorpses == 1 ? "A CORPSE ROTS AWAY IN YOUR PACK." : "SOME CORPSES ROT AWAY IN YOUR PACK.",
                MessageKind::Bad, true);
        }
        if (rottedInvMeat > 0) {
            pushMsg(rottedInvMeat == 1 ? "MEAT ROTS AWAY IN YOUR PACK." : "SOME MEAT ROTS AWAY IN YOUR PACK.",
                MessageKind::Bad, true);
        }

        int rottedGroundVis = 0;
        int rottedMeatVis = 0;
        int revivedVis = 0;

        auto inView = [&](Vec2i pos) -> bool {
            return dung.inBounds(pos.x, pos.y) && dung.at(pos.x, pos.y).visible;
        };

        // Deterministic per-corpse, per-turn "one in N" without consuming gameplay RNG.
        auto oneInThisTurn = [&](const Item& corpse, int n) -> bool {
            if (n <= 1) return true;
            const uint32_t h = hashCombine(hash32(static_cast<uint32_t>(corpse.id)), static_cast<uint32_t>(turnCount) ^ 0xC0FFEE5Eu);
            return (h % static_cast<uint32_t>(n)) == 0u;
        };

        auto findReviveSpot = [&](Vec2i origin) -> Vec2i {
            if (dung.inBounds(origin.x, origin.y) && dung.isWalkable(origin.x, origin.y) && !entityAt(origin.x, origin.y)) {
                return origin;
            }
            constexpr Vec2i dirs[8] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
            for (int r = 1; r <= 2; ++r) {
                for (const Vec2i& d : dirs) {
                    Vec2i p{ origin.x + d.x * r, origin.y + d.y * r };
                    if (!dung.inBounds(p.x, p.y)) continue;
                    if (!dung.isWalkable(p.x, p.y)) continue;
                    if (entityAt(p.x, p.y)) continue;
                    return p;
                }
            }
            return { -1, -1 };
        };

        for (size_t i = 0; i < ground.size();) {
            auto& gi = ground[i];

            if (gi.item.kind == ItemKind::ButcheredMeat) {
                if (gi.item.charges > 0) gi.item.charges--;
                if (gi.item.charges <= 0) {
                    if (inView(gi.pos)) ++rottedMeatVis;
                    ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));
                    continue;
                }
            }

            if (isCorpseKind(gi.item.kind)) {
                // Corpses rot away over time.
                if (gi.item.charges > 0) gi.item.charges--;
                if (gi.item.charges <= 0) {
                    if (inView(gi.pos)) ++rottedGroundVis;
                    ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));
                    continue;
                }

                // A few corpses can revive while fresh.
                if (gi.item.charges > 160) {
                    EntityKind revivedKind = EntityKind::Goblin;
                    int n = 0;
                    bool trollMsg = false;

                    switch (gi.item.kind) {
                        case ItemKind::CorpseTroll: revivedKind = EntityKind::Troll; n = 70; trollMsg = true; break;
                        case ItemKind::CorpseBat: revivedKind = EntityKind::Bat; n = 90; break;
                        case ItemKind::CorpseSnake: revivedKind = EntityKind::Snake; n = 130; break;
                        case ItemKind::CorpseSpider: revivedKind = EntityKind::Spider; n = 200; break;
                        default: break;
                    }

                    if (n > 0 && oneInThisTurn(gi.item, n)) {
                        const Vec2i spot = findReviveSpot(gi.pos);
                        if (spot.x != -1) {
                            if (trollMsg && inView(spot)) {
                                pushMsg("THE TROLL CORPSE REGENERATES!", MessageKind::Warning, true);
                            }
                            spawnMonster(revivedKind, spot, /*groupId=*/0, /*allowGear=*/false);
                            revivedVis += inView(spot) ? 1 : 0;
                            ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));
                            continue;
                        }
                    }
                }
            }

            ++i;
        }

        if (rottedGroundVis > 0) {
            pushMsg(rottedGroundVis == 1 ? "A CORPSE ROTS AWAY." : "SOME CORPSES ROT AWAY.", MessageKind::Bad, true);
        }
        if (rottedMeatVis > 0) {
            pushMsg(rottedMeatVis == 1 ? "MEAT ROTS AWAY." : "SOME MEAT ROTS AWAY.", MessageKind::Bad, true);
        }
        if (revivedVis > 0) {
            pushMsg(revivedVis == 1 ? "A CORPSE TWITCHES AND STANDS UP!" : "SOME CORPSES TWITCH AND STAND UP!", MessageKind::Warning, true);
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
            const int burnDecay = 1 + wxBurnQuench;
            m.effects.burnTurns = std::max(0, m.effects.burnTurns - burnDecay);
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

        // Corrosion: stinging damage over time + defense penalty while active.
        if (m.effects.corrosionTurns > 0) {
            const bool vis = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
            m.effects.corrosionTurns = std::max(0, m.effects.corrosionTurns - 1);

            // Corrosion ticks every other turn (slower than poison/burn).
            if ((turnCount & 1u) == 0u) {
                m.hp -= 1;

                // Acid can pit monster gear too (mostly affects dropped loot).
                // Shielding protects gear the same way it protects skin/hide.
                if (m.hp > 0 && m.effects.shieldTurns <= 0) {
                    uint8_t g = 0u;
                    if (dung.inBounds(m.pos.x, m.pos.y) && !corrosiveGas_.empty()) {
                        const size_t gi = static_cast<size_t>(m.pos.y * dung.width + m.pos.x);
                        if (gi < corrosiveGas_.size()) g = corrosiveGas_[gi];
                    }

                    std::array<Item*, 2> picks = {nullptr, nullptr};
                    int n = 0;
                    if (m.gearArmor.id != 0 && isArmor(m.gearArmor.kind) && !itemIsArtifact(m.gearArmor)) {
                        picks[n++] = &m.gearArmor;
                    }
                    if (m.gearMelee.id != 0 && isWeapon(m.gearMelee.kind) && !itemIsArtifact(m.gearMelee)) {
                        picks[n++] = &m.gearMelee;
                    }

                    if (n > 0) {
                        Item& it = *picks[static_cast<size_t>(rng.range(0, n - 1))];

                        int chancePct = 10 + m.effects.corrosionTurns * 3 + static_cast<int>(g) / 12;
                        chancePct = clampi(chancePct, 6, 45);

                        if (it.buc > 0) chancePct = std::max(0, chancePct - 8);
                        else if (it.buc < 0) chancePct = std::min(90, chancePct + 8);

                        if (chancePct > 0 && rng.range(1, 100) <= chancePct) {
                            const int before = it.enchant;
                            const int after = std::max(-3, before - 1);
                            if (after != before) {
                                const std::string nm = itemDisplayName(it);
                                it.enchant = after;

                                if (vis) {
                                    std::ostringstream ss;
                                    ss << kindName(m.kind) << "'S " << nm << " CORRODES!";
                                    pushMsg(ss.str(), MessageKind::Info, false);
                                }
                            }
                        }
                    }
                }
            }

            if (m.hp <= 0) {
                if (vis) {
                    std::ostringstream ss;
                    ss << kindName(m.kind) << " DISSOLVES.";
                    pushMsg(ss.str(), MessageKind::Combat, false);
                }
            } else if (m.effects.corrosionTurns == 0 && vis) {
                std::ostringstream ss;
                ss << kindName(m.kind) << " SHAKES OFF THE CORROSION.";
                pushMsg(ss.str(), MessageKind::System, false);
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
    //
    // Substrate chemistry:
    //  - Porous surfaces (moss/dirt/wood/bone) absorb fumes faster.
    //  - Smooth sealed surfaces (metal/crystal/obsidian/marble) let vapor drift a bit farther.
    //  - Chasms behave like open pits: heavy fumes tend to sink; light vapors disperse.
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (expect > 0 && confusionGas_.size() != expect) {
            confusionGas_.assign(expect, uint8_t{0});
        }

        if (!confusionGas_.empty()) {
            const int w = dung.width;
            const int h = dung.height;
            const size_t n = static_cast<size_t>(w * h);

            std::vector<uint8_t> next(n, uint8_t{0});
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
            auto passable = [&](int x, int y) -> bool {
                if (!dung.inBounds(x, y)) return false;
                const TileType tt = dung.at(x, y).type;
                // Vapor can drift over chasms (open pits) even though they are not walkable.
                return dung.isWalkable(x, y) || (tt == TileType::Chasm);
            };

            constexpr Vec2i kDirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    const uint8_t s = confusionGas_[i];
                    if (s == 0u) continue;
                    if (!passable(x, y)) continue;

                    const TileType tt = dung.at(x, y).type;
                    const TerrainMaterial mat = dung.materialAtCached(x, y);
                    const EcosystemKind eco = dung.ecosystemAtCached(x, y);
                    const EcosystemFx ecoFx = ecosystemFx(eco);

                    int decay = 1 + gasAbsorb(mat);
                    // Light haze disperses quickly over open pits.
                    if (tt == TileType::Chasm) decay += 1;

                    // Ecosystem microclimate: some regions scrub vapors quickly, others let dense clouds linger.
                    if (ecoFx.confusionGasQuenchDelta > 0) decay += ecoFx.confusionGasQuenchDelta;

                    // Always decay in place.
                    uint8_t self = (s > static_cast<uint8_t>(decay)) ? static_cast<uint8_t>(s - static_cast<uint8_t>(decay)) : 0u;
                    if (ecoFx.confusionGasQuenchDelta < 0 && s >= 8u) {
                        const int boost = -ecoFx.confusionGasQuenchDelta;
                        self = static_cast<uint8_t>(clampi(static_cast<int>(self) + boost, 0, 255));
                    }
                    if (next[i] < self) next[i] = self;

                    // Spread to neighbors with extra decay.
                    //
                    // Wind bias: downwind tiles get a slightly "stronger" spread, while upwind tiles
                    // dissipate a bit faster. This makes gas feel like it's drifting through corridors.
                    if (s >= 3u) {
                        int base = static_cast<int>(s) - 2;
                        base -= gasAbsorb(mat);
                        base += gasSlick(mat); // smooth surfaces let vapor slide a little farther
                        base += ecoFx.confusionGasSpreadDelta;
                        base = std::clamp(base, 0, static_cast<int>(s));

                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;

                            int spread = base;

                            if (windStr > 0) {
                                if (d.x == wind.x && d.y == wind.y) {
                                    spread = std::min(static_cast<int>(s), spread + windStr);
                                } else if (d.x == upWind.x && d.y == upWind.y) {
                                    spread = std::max(0, spread - windStr);
                                }
                            }

                            const TileType nt = dung.at(nx, ny).type;

                            // Light vapor prefers to "rise out" of chasms and resists sinking into them.
                            if (nt == TileType::Chasm && tt != TileType::Chasm) {
                                spread = std::max(0, spread - 2);
                            } else if (tt == TileType::Chasm && nt != TileType::Chasm) {
                                spread = std::min(static_cast<int>(s), spread + 2);
                            }

                            if (spread <= 0) continue;
                            const size_t j = idx2(nx, ny);
                            const uint8_t su = static_cast<uint8_t>(std::clamp(spread, 0, 255));
                            if (next[j] < su) next[j] = su;
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
            poisonGas_.assign(expect, uint8_t{0});
        }

        if (!poisonGas_.empty()) {
            const int w = dung.width;
            const int h = dung.height;
            const size_t n = static_cast<size_t>(w * h);

            std::vector<uint8_t> next(n, uint8_t{0});
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
            auto passable = [&](int x, int y) -> bool {
                if (!dung.inBounds(x, y)) return false;
                const TileType tt = dung.at(x, y).type;
                // Heavy-ish gas can drift over open pits.
                return dung.isWalkable(x, y) || (tt == TileType::Chasm);
            };

            constexpr Vec2i kDirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    const uint8_t s = poisonGas_[i];
                    if (s == 0u) continue;
                    if (!passable(x, y)) continue;

                    const TileType tt = dung.at(x, y).type;
                    const TerrainMaterial mat = dung.materialAtCached(x, y);
                    const EcosystemKind eco = dung.ecosystemAtCached(x, y);
                    const EcosystemFx ecoFx = ecosystemFx(eco);

                    int decay = 1 + gasAbsorb(mat);
                    if (ecoFx.poisonGasQuenchDelta > 0) decay += ecoFx.poisonGasQuenchDelta;

                    // Always decay in place.
                    uint8_t self = (s > static_cast<uint8_t>(decay)) ? static_cast<uint8_t>(s - static_cast<uint8_t>(decay)) : 0u;
                    if (ecoFx.poisonGasQuenchDelta < 0 && s >= 8u) {
                        const int boost = -ecoFx.poisonGasQuenchDelta;
                        self = static_cast<uint8_t>(clampi(static_cast<int>(self) + boost, 0, 255));
                    }
                    if (next[i] < self) next[i] = self;

                    // Spread to neighbors with extra decay (more dissipative than confusion gas).
                    //
                    // Wind bias: poison gas stays localized, but still drifts downwind in corridors.
                    if (s >= 4u) {
                        int base = static_cast<int>(s) - 3;
                        base -= gasAbsorb(mat);
                        base += gasSlick(mat); // sealed surfaces let fumes "slide" a bit
                        base += ecoFx.poisonGasSpreadDelta;
                        base = std::clamp(base, 0, static_cast<int>(s));

                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;

                            int spread = base;

                            if (windStr > 0) {
                                // Slightly weaker than confusion gas so poison doesn't become too "flowy".
                                const int bonus = std::max(1, windStr - 1);
                                if (d.x == wind.x && d.y == wind.y) {
                                    spread = std::min(static_cast<int>(s), spread + bonus);
                                } else if (d.x == upWind.x && d.y == upWind.y) {
                                    spread = std::max(0, spread - bonus);
                                }
                            }

                            const TileType nt = dung.at(nx, ny).type;

                            // Poison vapors are heavier than haze: they tend to sink into pits and stay there.
                            if (nt == TileType::Chasm && tt != TileType::Chasm) {
                                spread = std::min(static_cast<int>(s), spread + 2);
                            } else if (tt == TileType::Chasm && nt != TileType::Chasm) {
                                spread = std::max(0, spread - 2);
                            }

                            if (spread <= 0) continue;
                            const size_t j = idx2(nx, ny);
                            const uint8_t su = static_cast<uint8_t>(std::clamp(spread, 0, 255));
                            if (next[j] < su) next[j] = su;
                        }
                    }
                }
            }

            poisonGas_.swap(next);
        }
    }


    // Update corrosive gas cloud diffusion/decay.
    // Corrosive vapors are heavier and stay more localized than poison.
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (expect > 0 && corrosiveGas_.size() != expect) {
            corrosiveGas_.assign(expect, uint8_t{0});
        }

        if (!corrosiveGas_.empty()) {
            const int w = dung.width;
            const int h = dung.height;
            const size_t n = static_cast<size_t>(w * h);

            std::vector<uint8_t> next(n, uint8_t{0});
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
            auto passable = [&](int x, int y) -> bool {
                if (!dung.inBounds(x, y)) return false;
                const TileType tt = dung.at(x, y).type;
                // Acid fumes can drift over open pits.
                return dung.isWalkable(x, y) || (tt == TileType::Chasm);
            };

            constexpr Vec2i kDirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    const uint8_t s = corrosiveGas_[i];
                    if (s == 0u) continue;
                    if (!passable(x, y)) continue;

                    const TileType tt = dung.at(x, y).type;
                    const TerrainMaterial mat = dung.materialAtCached(x, y);
                    const EcosystemKind eco = dung.ecosystemAtCached(x, y);
                    const EcosystemFx ecoFx = ecosystemFx(eco);

                    int decay = 1 + gasAbsorb(mat);
                    if (ecoFx.corrosiveGasQuenchDelta > 0) decay += ecoFx.corrosiveGasQuenchDelta;

                    // Always decay in place.
                    uint8_t self = (s > static_cast<uint8_t>(decay)) ? static_cast<uint8_t>(s - static_cast<uint8_t>(decay)) : 0u;
                    if (ecoFx.corrosiveGasQuenchDelta < 0 && s >= 8u) {
                        const int boost = -ecoFx.corrosiveGasQuenchDelta;
                        self = static_cast<uint8_t>(clampi(static_cast<int>(self) + boost, 0, 255));
                    }
                    if (next[i] < self) next[i] = self;

                    // Spread is more dissipative than poison gas.
                    // Wind bias is weaker: this vapor tends to cling.
                    if (s >= 5u) {
                        int base = static_cast<int>(s) - 4;
                        base -= gasAbsorb(mat);

                        // Corrosive vapor is sticky on some substrates (it condenses rather than drifting).
                        if (mat == TerrainMaterial::Metal || mat == TerrainMaterial::Obsidian || mat == TerrainMaterial::Basalt) {
                            base -= 1;
                        }

                        base += ecoFx.corrosiveGasSpreadDelta;
                        base = std::clamp(base, 0, static_cast<int>(s));

                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;

                            int spread = base;

                            if (windStr > 0) {
                                const int bonus = std::max(0, windStr - 2);
                                if (d.x == wind.x && d.y == wind.y) {
                                    spread = std::min(static_cast<int>(s), spread + bonus);
                                } else if (d.x == upWind.x && d.y == upWind.y) {
                                    spread = std::max(0, spread - bonus);
                                }
                            }

                            const TileType nt = dung.at(nx, ny).type;

                            // Acid fumes are the heaviest: they strongly pool into pits.
                            if (nt == TileType::Chasm && tt != TileType::Chasm) {
                                spread = std::min(static_cast<int>(s), spread + 3);
                            } else if (tt == TileType::Chasm && nt != TileType::Chasm) {
                                spread = std::max(0, spread - 3);
                            }

                            if (spread <= 0) continue;
                            const size_t j = idx2(nx, ny);
                            const uint8_t su = static_cast<uint8_t>(std::clamp(spread, 0, 255));
                            if (next[j] < su) next[j] = su;
                        }
                    }
                }
            }

            corrosiveGas_.swap(next);
        }
    }

    // ------------------------------------------------------------
    // Adhesive fluid simulation (procedural + cohesive movement).
    //
    // The field is seeded deterministically from wet topology (fountains/chasms)
    // and then updated as a cohesive ooze that prefers to stay clumped while
    // slowly creeping along local moisture/flow gradients.
    // ------------------------------------------------------------
    {
        const int w = dung.width;
        const int h = dung.height;
        const size_t expect = static_cast<size_t>(w * h);

        if (expect > 0 && adhesiveFluid_.size() != expect) {
            adhesiveFluid_.assign(expect, uint8_t{0});
        }

        if (!adhesiveFluid_.empty()) {
            auto idx2 = [&](int x, int y) -> size_t {
                return static_cast<size_t>(y * w + x);
            };
            auto passable = [&](int x, int y) -> bool {
                return dung.inBounds(x, y) && dung.isWalkable(x, y);
            };
            constexpr Vec2i kDirs[4] = { {1,0}, {-1,0}, {0,1}, {0,-1} };

            uint32_t glueSeed = levelGenSeed(LevelId{branch_, depth_});
            if (atCamp()) {
                glueSeed = hashCombine(glueSeed, static_cast<uint32_t>(overworldX_));
                glueSeed = hashCombine(glueSeed, static_cast<uint32_t>(overworldY_));
            }
            glueSeed = hashCombine(glueSeed, 0xAD1500F1u);

            // Precompute local wetness once per turn (0..255) from nearby fishable water.
            std::vector<uint8_t> wetness(expect, uint8_t{0});
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (!passable(x, y)) continue;
                    int wet = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx;
                            const int ny = y + dy;
                            if (!dung.inBounds(nx, ny)) continue;
                            const TileType tt = dung.at(nx, ny).type;
                            if (tt == TileType::Fountain) wet += 3;
                            else if (tt == TileType::Chasm) wet += 1;
                        }
                    }
                    if (dung.at(x, y).type == TileType::Fountain) wet += 5;
                    wetness[idx2(x, y)] = static_cast<uint8_t>(clampi(wet, 0, 255));
                }
            }

            // One-shot deterministic seed if the field is currently empty on this level.
            bool anyAdhesive = false;
            for (uint8_t v : adhesiveFluid_) {
                if (v > 0u) {
                    anyAdhesive = true;
                    break;
                }
            }

            if (!anyAdhesive) {
                int seeded = 0;
                size_t fallbackI = expect;
                uint32_t fallbackH = UINT32_MAX;

                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        if (!passable(x, y)) continue;
                        const Vec2i tp{x, y};
                        if (tp == dung.stairsUp || tp == dung.stairsDown) continue;

                        const size_t i = idx2(x, y);
                        const int wet = static_cast<int>(wetness[i]);

                        const uint32_t tileTag = static_cast<uint32_t>(i);
                        const uint32_t h0 = hash32(hashCombine(glueSeed, tileTag));
                        if (h0 < fallbackH) {
                            fallbackH = h0;
                            fallbackI = i;
                        }

                        if (wet <= 0) continue;

                        const int chance = clampi(8 + wet * 8, 0, 92);
                        if (static_cast<int>(h0 % 100u) >= chance) continue;

                        int base = 6 + wet * 2 + static_cast<int>((h0 >> 9) % 12u);
                        if (dung.at(x, y).type == TileType::Fountain) base += 16;
                        base = clampi(base, 0, 140);

                        if (base > 0) {
                            adhesiveFluid_[i] = static_cast<uint8_t>(base);
                            ++seeded;
                        }
                    }
                }

                if (seeded == 0 && fallbackI < expect) {
                    adhesiveFluid_[fallbackI] = uint8_t{14};
                }
            }

            // Calm levels still get a deterministic gentle ooze drift.
            Vec2i oozeFlow = wind;
            if (oozeFlow.x == 0 && oozeFlow.y == 0) {
                constexpr Vec2i kDrift[4] = { {1,0}, {0,1}, {-1,0}, {0,-1} };
                const uint32_t phaseSeed = hashCombine(glueSeed, static_cast<uint32_t>(turnCount / 10u));
                oozeFlow = kDrift[hash32(phaseSeed) & 3u];
            }
            const Vec2i oozeUp{-oozeFlow.x, -oozeFlow.y};

            std::vector<int> accum(expect, 0);

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = idx2(x, y);
                    const int s = static_cast<int>(adhesiveFluid_[i]);
                    if (s <= 0) continue;
                    if (!passable(x, y)) continue;

                    const int wet = static_cast<int>(wetness[i]);
                    const TerrainMaterial mat = dung.materialAtCached(x, y);

                    int decay = 1;
                    if (wet <= 0) decay += 1;

                    switch (mat) {
                        case TerrainMaterial::Moss:
                        case TerrainMaterial::Dirt:
                        case TerrainMaterial::Wood:
                            decay = std::max(1, decay - 1);
                            break;
                        case TerrainMaterial::Metal:
                        case TerrainMaterial::Crystal:
                        case TerrainMaterial::Obsidian:
                        case TerrainMaterial::Marble:
                            decay += 1;
                            break;
                        default:
                            break;
                    }
                    decay = clampi(decay, 1, 4);

                    int retain = std::max(0, s - decay);
                    if (retain <= 0) continue;

                    int bestX = x;
                    int bestY = y;
                    int bestScore = s * 4 + wet * 8 + 12; // cohesion bias to stay clumped.

                    for (const Vec2i& d : kDirs) {
                        const int nx = x + d.x;
                        const int ny = y + d.y;
                        if (!passable(nx, ny)) continue;
                        const size_t j = idx2(nx, ny);

                        int score = static_cast<int>(adhesiveFluid_[j]) * 5 + static_cast<int>(wetness[j]) * 10;
                        if (d.x == oozeFlow.x && d.y == oozeFlow.y) score += 4 + windStr;
                        if (d.x == oozeUp.x && d.y == oozeUp.y) score -= 2 + std::max(0, windStr - 1);

                        if (score > bestScore) {
                            bestScore = score;
                            bestX = nx;
                            bestY = ny;
                        }
                    }

                    int move = 0;
                    if ((bestX != x || bestY != y) && retain > 2) {
                        move = retain / 3;
                        if (move > 18) move = 18;
                        if (move < 1 && retain >= 9) move = 1;
                    }

                    int self = retain - move;
                    if (self > 0) accum[i] += self;

                    if (move > 0) {
                        const size_t j = idx2(bestX, bestY);
                        accum[j] += move;
                    }

                    // Small neighbor bleed keeps contiguous blobs from breaking into checkerboards.
                    if (retain >= 10) {
                        for (const Vec2i& d : kDirs) {
                            const int nx = x + d.x;
                            const int ny = y + d.y;
                            if (!passable(nx, ny)) continue;
                            const size_t j = idx2(nx, ny);
                            if (adhesiveFluid_[j] < 6u) continue;
                            accum[j] += 1;
                            if (accum[i] > 0) accum[i] -= 1;
                            break;
                        }
                    }
                }
            }

            // Moisture sources continuously feed the ooze field.
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    if (!passable(x, y)) continue;
                    const size_t i = idx2(x, y);
                    const int wet = static_cast<int>(wetness[i]);
                    if (wet <= 0) continue;

                    int source = 0;
                    if (dung.at(x, y).type == TileType::Fountain) {
                        source = 16 + wet * 2;
                    } else if (wet >= 3) {
                        source = 2 + wet;
                    }

                    if (source > accum[i]) accum[i] = source;

                    if (wet >= 4) {
                        const uint32_t h0 = hash32(hashCombine(glueSeed ^ 0x91E5u, static_cast<uint32_t>(i)));
                        if (((turnCount + (h0 & 3u)) % 4u) == 0u) {
                            accum[i] += 1 + static_cast<int>((h0 >> 8) & 1u);
                        }
                    }
                }
            }

            std::vector<uint8_t> next(expect, uint8_t{0});
            for (size_t i = 0; i < expect; ++i) {
                next[i] = static_cast<uint8_t>(clampi(accum[i], 0, 255));
            }
            adhesiveFluid_.swap(next);
        }
    }


    // ------------------------------------------------------------
    // Field chemistry: vented laboratory doors leak fumes even while closed.
    //
    // This keeps gas hazards from being perfectly binary (sealed / not sealed)
    // and creates interesting pressure build-up behind airtight airlocks.
    // ------------------------------------------------------------
    {
        const int w = dung.width;
        const int h = dung.height;
        const size_t expect = static_cast<size_t>(w * h);

        if (expect > 0 && poisonGas_.size() == expect && corrosiveGas_.size() == expect) {
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };

            auto leakAcrossVentedDoors = [&](std::vector<uint8_t>& f, int minDiff, int div, int maxLeak) {
                if (f.size() != expect) return;

                std::vector<int16_t> delta(expect, 0);

                for (int y = 0; y < h; ++y) {
                    for (int x = 0; x < w; ++x) {
                        const TileType tt = dung.at(x, y).type;
                        if (tt != TileType::DoorClosed && tt != TileType::DoorLocked) continue;

                        if (doorSealKindAt(x, y) != DoorSealKind::Vented) continue;

                        Vec2i a{0, 0}, b{0, 0};
                        if (!doorOpposingSides(dung, {x, y}, a, b)) continue;

                        const size_t ia = idx2(a.x, a.y);
                        const size_t ib = idx2(b.x, b.y);
                        const int va = static_cast<int>(f[ia]);
                        const int vb = static_cast<int>(f[ib]);
                        const int diff = va - vb;
                        const int ad = std::abs(diff);
                        if (ad < minDiff) continue;

                        int amt = ad / div;
                        if (amt < 1) amt = 1;
                        if (amt > maxLeak) amt = maxLeak;

                        if (diff > 0) {
                            delta[ia] -= static_cast<int16_t>(amt);
                            delta[ib] += static_cast<int16_t>(amt);
                        } else {
                            delta[ib] -= static_cast<int16_t>(amt);
                            delta[ia] += static_cast<int16_t>(amt);
                        }
                    }
                }

                for (size_t i = 0; i < expect; ++i) {
                    const int nv = std::clamp(static_cast<int>(f[i]) + static_cast<int>(delta[i]), 0, 255);
                    f[i] = static_cast<uint8_t>(nv);
                }
            };

            // Poison vapor is lighter/more mobile than acid fumes.
            leakAcrossVentedDoors(poisonGas_, 8, 24, 6);
            leakAcrossVentedDoors(corrosiveGas_, 8, 26, 5);
        }
    }

    
    // ------------------------------------------------------------
    // Field chemistry: Corrosive vapor can pit gear and eat through doors.
    //
    // This makes Corrosive Gas a long-term, dungeon-shaping hazard: leaving
    // equipment in acid is bad, but clever players can also weaken locks at a cost.
    // ------------------------------------------------------------
    {
        const int w = dung.width;
        const int h = dung.height;
        const size_t expect = static_cast<size_t>(w * h);

        if (expect > 0 && corrosiveGas_.size() == expect) {
            auto idx2 = [&](int x, int y) -> size_t { return static_cast<size_t>(y * w + x); };
            auto gasAt = [&](Vec2i p) -> uint8_t {
                if (!dung.inBounds(p.x, p.y)) return uint8_t{0};
                const size_t i = idx2(p.x, p.y);
                if (i >= corrosiveGas_.size()) return uint8_t{0};
                return corrosiveGas_[i];
            };

            // Doors: high acid exposure can unlock locks and eventually force doors open.
            int unlockSeen = 0;
            int openSeen = 0;

            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const TileType tt = dung.at(x, y).type;
                    if (tt != TileType::DoorLocked && tt != TileType::DoorClosed) continue;

                    Vec2i a{0, 0}, b{0, 0};
                    if (!doorOpposingSides(dung, {x, y}, a, b)) continue;

                    const uint8_t ga = gasAt(a);
                    const uint8_t gb = gasAt(b);
                    const uint8_t g = (ga > gb) ? ga : gb;
                    if (g == 0u) continue;

                    DoorSealKind seal = doorSealKindAt(x, y);

                    int threshUnlock = 20;
                    int threshOpen = 18;
                    int maxChance = 22;

                    // Airlocks are sturdier; vented doors are a bit weaker.
                    if (seal == DoorSealKind::Airlock) {
                        threshUnlock += 6;
                        threshOpen += 6;
                        maxChance = 16;
                    } else if (seal == DoorSealKind::Vented) {
                        threshUnlock = std::max(0, threshUnlock - 2);
                        threshOpen = std::max(0, threshOpen - 2);
                        maxChance = 26;
                    }

                    if (tt == TileType::DoorLocked) {
                        if (g < static_cast<uint8_t>(threshUnlock)) continue;
                        int chancePct = 2 + (static_cast<int>(g) - threshUnlock) / 3;
                        chancePct = clampi(chancePct, 2, maxChance);

                        if (rng.range(1, 100) <= chancePct) {
                            dung.at(x, y).type = TileType::DoorClosed;
                            emitNoise({x, y}, 8);

                            if (dung.at(x, y).visible) ++unlockSeen;
                        }
                    } else { // DoorClosed
                        if (g < static_cast<uint8_t>(threshOpen)) continue;
                        int chancePct = 2 + (static_cast<int>(g) - threshOpen) / 3;
                        chancePct = clampi(chancePct, 2, maxChance);

                        if (rng.range(1, 100) <= chancePct) {
                            dung.at(x, y).type = TileType::DoorOpen;
                            emitNoise({x, y}, 10);

                            // Opening a door can cause a pressure/gas puff (esp. airlocks).
                            onDoorOpened({x, y}, /*openerIsPlayer=*/false);

                            if (dung.at(x, y).visible) ++openSeen;
                        }
                    }
                }
            }

            if (unlockSeen > 0) {
                pushMsg(unlockSeen == 1 ? "A LOCK HISSES AND FAILS." : "SOME LOCKS HISS AND FAIL.", MessageKind::System, true);
            }
            if (openSeen > 0) {
                pushMsg(openSeen == 1 ? "A DOOR SIZZLES OPEN." : "SOME DOORS SIZZLE OPEN.", MessageKind::System, true);
            }

            // Ground gear: items left in strong acid can slowly lose enchantment (rare).
            int pittedSeen = 0;
            if ((turnCount & 1u) == 0u) {
                for (auto& gi : ground) {
                    const Item& it0 = gi.item;
                    if (!(isArmor(it0.kind) || isWeapon(it0.kind))) continue;
                    if (itemIsArtifact(it0)) continue;
                    if (!dung.inBounds(gi.pos.x, gi.pos.y)) continue;

                    const uint8_t gv = gasAt(gi.pos);
                    if (gv < 20u) continue;

                    int chancePct = 1 + (static_cast<int>(gv) - 20) / 6;
                    chancePct = clampi(chancePct, 1, 10);

                    // Blessed gear resists; cursed gear suffers.
                    if (gi.item.buc > 0) chancePct = std::max(0, chancePct - 2);
                    else if (gi.item.buc < 0) chancePct = std::min(90, chancePct + 2);

                    if (chancePct > 0 && rng.range(1, 100) <= chancePct) {
                        const int before = gi.item.enchant;
                        const int after = std::max(-3, before - 1);
                        if (after != before) {
                            gi.item.enchant = after;
                            if (dung.at(gi.pos.x, gi.pos.y).visible) ++pittedSeen;
                        }
                    }
                }
            }

            if (pittedSeen > 0) {
                pushMsg(pittedSeen == 1 ? "SOMETHING SIZZLES IN THE ACID." : "SOME THINGS SIZZLE IN THE ACID.", MessageKind::System, true);
            }
        }
    }


    // Update fire field decay/spread.
    // The fire field generally decays over time, with a small chance to spread when strong.
    {
        const size_t expect = static_cast<size_t>(dung.width * dung.height);
        if (expect > 0 && fireField_.size() != expect) {
            fireField_.assign(expect, uint8_t{0});
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

                    // Ecosystem microclimate + overworld weather can quench or sustain fires.
                    const EcosystemKind eco = dung.ecosystemAtCached(x, y);
                    const EcosystemFx ecoFx = ecosystemFx(eco);

                    int decay = 1 + wxFireQuench;
                    if (ecoFx.fireQuenchDelta > 0) decay += ecoFx.fireQuenchDelta;

                    uint8_t self = (s > static_cast<uint8_t>(decay)) ? static_cast<uint8_t>(s - static_cast<uint8_t>(decay)) : 0u;
                    // In hot/dry regions, strong fires can linger a bit longer before guttering out.
                    if (ecoFx.fireQuenchDelta < 0 && s >= 6u) {
                        const int boost = -ecoFx.fireQuenchDelta;
                        self = static_cast<uint8_t>(clampi(static_cast<int>(self) + boost, 0, 255));
                    }
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

                            float chance = baseChance * (static_cast<float>(ecoFx.fireSpreadMulPct) / 100.0f);
                            if (windStr > 0) {
                                // Downwind flames jump more readily; upwind spread is suppressed.
                                if (d.x == wind.x && d.y == wind.y) {
                                    chance *= (1.0f + 0.35f * static_cast<float>(windStr));
                                } else if (d.x == upWind.x && d.y == upWind.y) {
                                    chance *= std::max(0.20f, (1.0f - 0.25f * static_cast<float>(windStr)));
                                }
                            }
                            if (wxFireQuench > 0) {
                                chance *= std::max(0.10f, 1.0f - 0.25f * static_cast<float>(wxFireQuench));
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
        int forgivenLedger = 0;
        if (depth_ >= 1 && depth_ <= DUNGEON_MAX_DEPTH) {
            forgivenLedger = std::max(0, shopDebtLedger_[depth_]);
            shopDebtLedger_[depth_] = 0;
        }

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
        if (forgivenLedger > 0) {
            pushMsg("THE SHOPKEEPER'S LEDGER BURNS TO ASH.", MessageKind::System, true);
        }
        pushMsg("THE SHOPKEEPER IS DEAD. EVERYTHING IS FREE!", MessageKind::Success, true);

        if (merchantGuildAlerted_ && shopDebtTotal() <= 0) {
            standDownMerchantGuild();
            pushMsg("THE MERCHANT GUILD STANDS DOWN.", MessageKind::System, true);
        }
    }

    // Ensure ecosystem/material caches are ready for any deterministic biome-aligned drops.
    dung.ensureMaterials(materialWorldSeed(), branch_, materialDepth(), dungeonMaxDepth());


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

        // Proc-ranked essence: Champion/Mythic proc foes can shed a small stack of biome-aligned
        // Essence Shards. This is deterministic (hash-derived) to avoid perturbing the main RNG stream.
        if (tier >= 2) {
            const EcosystemKind ecoHere = dung.ecosystemAtCached(e.pos.x, e.pos.y);
            const TerrainMaterial matHere = dung.materialAtCached(e.pos.x, e.pos.y);

            if (ecoHere != EcosystemKind::None) {
                // Hash stream: stable for this run+floor+entity.
                uint32_t h = hashCombine(seed_, "PROC_ESS_DROP"_tag);
                h = hashCombine(h, static_cast<uint32_t>(branch_));
                h = hashCombine(h, static_cast<uint32_t>(materialDepth()));
                h = hashCombine(h, static_cast<uint32_t>(depth_));
                h = hashCombine(h, static_cast<uint32_t>(e.id));
                h = hashCombine(h, static_cast<uint32_t>(e.kind));
                h = hashCombine(h, static_cast<uint32_t>(e.spriteSeed));

                auto u01 = [](uint32_t v) -> float {
                    return static_cast<float>(v & 0xFFFFu) / 65535.0f;
                };

                float dropChance = (tier >= 3) ? 0.85f : 0.55f;
                if (gilded) dropChance += 0.10f;

                // Subtle ecosystem nudges.
                if (ecoHere == EcosystemKind::CrystalGarden) dropChance += 0.05f;
                if (ecoHere == EcosystemKind::FloodedGrotto) dropChance += 0.03f;
                dropChance = std::min(0.98f, std::max(0.0f, dropChance));

                if (u01(hashCombine(h, "D"_tag)) < dropChance) {
                    crafttags::Tag tag = crafttags::Tag::None;
                    const float uTag = u01(hashCombine(h, "TAG"_tag));

                    // Keep the same "eco + substrate" mapping used by the ecosystem resource spawner.
                    switch (ecoHere) {
                        case EcosystemKind::FungalBloom: {
                            if (matHere == TerrainMaterial::Moss || matHere == TerrainMaterial::Dirt) {
                                tag = (uTag < 0.55f) ? crafttags::Tag::Regen : crafttags::Tag::Venom;
                            } else {
                                tag = (uTag < 0.80f) ? crafttags::Tag::Venom : crafttags::Tag::Regen;
                            }
                        } break;
                        case EcosystemKind::CrystalGarden: {
                            if (matHere == TerrainMaterial::Crystal) {
                                if (uTag < 0.45f) tag = crafttags::Tag::Rune;
                                else if (uTag < 0.85f) tag = crafttags::Tag::Arc;
                                else tag = crafttags::Tag::Shield;
                            } else {
                                if (uTag < 0.60f) tag = crafttags::Tag::Arc;
                                else if (uTag < 0.90f) tag = crafttags::Tag::Rune;
                                else tag = crafttags::Tag::Shield;
                            }
                        } break;
                        case EcosystemKind::BoneField: {
                            tag = (uTag < 0.65f) ? crafttags::Tag::Daze : crafttags::Tag::Clarity;
                        } break;
                        case EcosystemKind::RustVeins: {
                            const float cut = (matHere == TerrainMaterial::Metal) ? 0.70f : 0.55f;
                            tag = (uTag < cut) ? ((matHere == TerrainMaterial::Metal) ? crafttags::Tag::Alch : crafttags::Tag::Stone)
                                               : ((matHere == TerrainMaterial::Metal) ? crafttags::Tag::Stone : crafttags::Tag::Alch);
                        } break;
                        case EcosystemKind::AshenRidge: {
                            tag = (uTag < 0.75f) ? crafttags::Tag::Ember : crafttags::Tag::Stone;
                        } break;
                        case EcosystemKind::FloodedGrotto: {
                            tag = (uTag < 0.55f) ? crafttags::Tag::Aurora : crafttags::Tag::Regen;
                        } break;
                        default:
                            break;
                    }

                    if (tag != crafttags::Tag::None) {
                        const int spawnDepth = materialDepth();

                        int shardTier = 1 + std::max(0, spawnDepth) / 6;
                        shardTier += (tier - 1); // champion/mythic bonus
                        if (spawnDepth >= 10 && u01(hashCombine(h, "T10"_tag)) < 0.15f) shardTier += 1;
                        if (ecoHere == EcosystemKind::CrystalGarden && u01(hashCombine(h, "TCR"_tag)) < 0.25f) shardTier += 1;
                        shardTier = std::max(1, std::min(8, shardTier));

                        float shinyChance = 0.04f + 0.008f * static_cast<float>(std::min(12, spawnDepth));
                        if (tier >= 3) shinyChance += 0.05f;
                        if (ecoHere == EcosystemKind::CrystalGarden) shinyChance += 0.08f;
                        if (ecoHere == EcosystemKind::FloodedGrotto) shinyChance += 0.02f;
                        shinyChance = std::min(0.50f, shinyChance);

                        const bool shiny = u01(hashCombine(h, "SH"_tag)) < shinyChance;

                        int count = 1;
                        if (u01(hashCombine(h, "C1"_tag)) < 0.40f) count += 1;
                        if (tier >= 3 && u01(hashCombine(h, "C2"_tag)) < 0.35f) count += 1;
                        if (spawnDepth >= 12 && u01(hashCombine(h, "C3"_tag)) < 0.25f) count += 1;
                        count = std::max(1, std::min(4, count));

                        Item shard;
                        shard.kind = ItemKind::EssenceShard;
                        shard.count = count;
                        shard.enchant = packEssenceShardEnchant(crafttags::tagIndex(tag), shardTier, shiny);
                        shard.spriteSeed = hashCombine(h, "ESS"_tag);
                        if (shard.spriteSeed == 0) shard.spriteSeed = 1;

                        dropGroundItemItem(e.pos, shard);
                    }
                }
            }
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
    if (atHomeCamp()) return;

    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    // Use a depth-like scalar for the overworld (Camp/0 wilderness chunks).
    const int spawnDepth = materialDepth();


    // Decide how many fountains to place.
    // Kept deliberately sparse: fountains are flavorful but can be risky.
    int want = 0;
    float p1 = 0.35f;
    if (spawnDepth >= 4) p1 = 0.45f;
    if (spawnDepth >= 8) p1 = 0.55f;
    if (spawnDepth >= 12) p1 = 0.60f;

    if (rng.chance(p1)) want = 1;
    if (spawnDepth >= 8 && rng.chance(0.20f)) want += 1;
    if (spawnDepth >= 14 && rng.chance(0.10f)) want += 1;

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
