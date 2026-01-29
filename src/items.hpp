#pragma once
#include "common.hpp"
#include <cstdint>
#include <string>
#include <vector>

enum class ProjectileKind : uint8_t {
    Arrow = 0,
    Rock,
    Spark,
    // New projectile kinds (append-only)
    Fireball,
    Torch,
};

enum class AmmoKind : uint8_t {
    None = 0,
    Arrow,
    Rock,
};

enum class EquipSlot : uint8_t {
    None = 0,
    MeleeWeapon,
    RangedWeapon,
    Armor,
    // New equipment types (append-only; NOT serialized)
    Ring,
};

enum class ItemKind : uint8_t {
    // Weapons
    Dagger = 0,
    Sword,
    Bow,
    Sling,
    WandSparks,

    // Armor
    LeatherArmor,
    ChainArmor,

    // Consumables
    PotionHealing,
    PotionStrength,
    ScrollTeleport,
    ScrollMapping,

    // Quest / special
    AmuletYendor,

    // Ammo / misc
    Arrow,
    Rock,
    Gold,

    // --- New consumables / progression (added after existing kinds to keep save compatibility) ---
    PotionAntidote,
    PotionRegeneration,
    PotionShielding,
    ScrollEnchantWeapon,
    ScrollEnchantArmor,

    // --- Even newer consumables (append-only to keep save compatibility) ---
    PotionHaste,
    PotionVision,

    // --- Identification / utility (append-only) ---
    ScrollIdentify,

    // --- New items (append-only to keep save compatibility) ---
    Axe,
    PlateArmor,
    FoodRation,
    ScrollDetectTraps,
    ScrollDetectSecrets,

    // --- Misc (append-only) ---
    Key,

    // --- Locks / doors (append-only) ---
    Lockpick,
    ScrollKnock,

    // --- Dungeon features (append-only) ---
    // A ground-interactable chest. It cannot be picked up.
    Chest,
    // Decorative open chest left behind after looting.
    ChestOpen,

    // --- Stealth / perception (append-only) ---
    PotionInvisibility,

    // --- Lighting (append-only) ---
    Torch,
    TorchLit,

    // --- Curses / blessings (append-only) ---
    ScrollRemoveCurse,

    // --- Mind / control (append-only) ---
    PotionClarity,
    ScrollConfusion,

    // --- Terrain / digging (append-only) ---
    Pickaxe,
    WandDigging,

    // --- Explosives / magic (append-only) ---
    WandFireball,

    // --- Corpses (append-only) ---
    // Dropped by slain monsters. Corpses rot away over time, and can be eaten
    // (at some risk) for hunger and sometimes temporary buffs.
    CorpseGoblin,
    CorpseOrc,
    CorpseBat,
    CorpseSlime,
    CorpseKobold,
    CorpseWolf,
    CorpseTroll,
    CorpseWizard,
    CorpseSnake,
    CorpseSpider,
    CorpseOgre,
    CorpseMimic,
    CorpseMinotaur,

    // --- Rings (append-only) ---
    RingMight,
    RingAgility,
    RingFocus,
    RingProtection,

    // --- Traversal (append-only) ---
    PotionLevitation,

    // --- Morale / control (append-only) ---
    ScrollFear,

    // --- Terrain / fortification (append-only) ---
    // NetHack-inspired utility scroll: raises boulders around the reader.
    ScrollEarth,

    // --- Pets / companions (append-only) ---
    // Charms nearby creatures into friendly companions.
    ScrollTaming,

    // --- Perception / weirdness (append-only) ---
    PotionHallucination,

    // --- Mana / magic (append-only) ---
    PotionEnergy,

    // --- Spellbooks (append-only) ---
    SpellbookMagicMissile,
    SpellbookBlink,
    SpellbookMinorHeal,
    SpellbookDetectTraps,
    SpellbookFireball,
    SpellbookStoneskin,
    SpellbookHaste,
    SpellbookInvisibility,
    SpellbookPoisonCloud,

    // --- New rings (append-only; keep ids stable for save compatibility) ---
    RingSearching,
    RingSustenance,

    // Jewelry enhancement (append-only)
    ScrollEnchantRing,

    // --- Collectibles (append-only) ---
    VtuberFigurine,

    // --- More collectibles (append-only) ---
    VtuberHoloCard,

    // --- Capture spheres (append-only) ---
    // Used for monster capture + companion recall/release.
    CaptureSphere,
    MegaSphere,
    CaptureSphereFull,
    MegaSphereFull,

    // --- Fishing (append-only) ---
    FishingRod,
    Fish,

    // --- Farming (append-only) ---
    GardenHoe,
    Seed,
    // Stationary ground plot (tilling result).
    TilledSoil,
    // Stationary planted crop stages.
    CropSprout,
    CropGrowing,
    CropMature,
    // Harvested produce (consumable).
    CropProduce,

    // --- Crafting (append-only) ---
    // A non-consumable tool used to combine ingredients into procedurally generated outputs.
    CraftingKit,

    // --- Bounties (append-only) ---
    // A guild contract that tracks kills and pays out a deterministic reward.
    BountyContract,

    // --- Procedural rune magic (append-only) ---
    RuneTablet,

    // Butchering outputs (append-only)
    ButcheredMeat,
    ButcheredHide,
    ButcheredBones,

    // Procedural crafting byproducts (append-only)
    EssenceShard,

    // --- Ecosystem resource nodes (append-only) ---
    // Stationary ground props spawned near biome seeds; harvest with CONFIRM.
    SporePod,
    CrystalNode,
    BonePile,
    RustVent,
    AshVent,
    GrottoSpring,
};

// Item "egos" (NetHack-style brands / special properties) applied to some gear.
//
// Append-only: egos are serialized with items, so keep ids stable.
enum class ItemEgo : uint8_t {
    None = 0,

    // Weapon egos (append-only)
    Flaming,
    Venom,
    Vampiric,

    // New weapon egos (append-only to keep save compatibility).
    Webbing,
    Corrosive,
    Dazing,
};

// Keep in sync with ItemEgo (append-only).
inline constexpr int ITEM_EGO_COUNT = static_cast<int>(ItemEgo::Dazing) + 1;

inline const char* egoPrefix(ItemEgo e) {
    switch (e) {
        case ItemEgo::Flaming:   return "FLAMING";
        case ItemEgo::Venom:     return "VENOM";
        case ItemEgo::Vampiric:  return "VAMPIRIC";
        case ItemEgo::Webbing:   return "WEBBING";
        case ItemEgo::Corrosive: return "CORROSIVE";
        case ItemEgo::Dazing:    return "DAZING";
        default:                 return "";
    }
}

// Short UI-friendly description of an ego's primary effect.
inline const char* egoShortDesc(ItemEgo e) {
    switch (e) {
        case ItemEgo::Flaming:   return "BURN ON HIT";
        case ItemEgo::Venom:     return "POISON ON HIT";
        case ItemEgo::Vampiric:  return "LIFE DRAIN";
        case ItemEgo::Webbing:   return "WEB ON HIT";
        case ItemEgo::Corrosive: return "CORRODE ON HIT";
        case ItemEgo::Dazing:    return "DAZE ON HIT";
        default:                 return "";
    }
}

// Compact "trait tag" used in loadout summaries / UI badges.
// These are intentionally short, upper-case keywords.
inline const char* egoTraitTag(ItemEgo e) {
    switch (e) {
        case ItemEgo::Flaming:   return "BURN";
        case ItemEgo::Venom:     return "POISON";
        case ItemEgo::Vampiric:  return "LIFE DRAIN";
        case ItemEgo::Webbing:   return "WEB";
        case ItemEgo::Corrosive: return "CORRODE";
        case ItemEgo::Dazing:    return "DAZE";
        default:                 return "";
    }
}

// A rough shop/value multiplier for ego gear.
// Returned as a percentage (100 = no change).
inline int egoValueMultiplierPct(ItemEgo e) {
    switch (e) {
        case ItemEgo::Flaming:   return 160;
        case ItemEgo::Venom:     return 170;
        case ItemEgo::Vampiric:  return 220;
        case ItemEgo::Webbing:   return 175;
        case ItemEgo::Corrosive: return 185;
        case ItemEgo::Dazing:    return 190;
        default:                 return 100;
    }
}

// Keep in sync with the last enum value (append-only).
inline constexpr int ITEM_KIND_COUNT = static_cast<int>(ItemKind::GrottoSpring) + 1; // keep in sync with last enum value

inline bool isVtuberCollectible(ItemKind k) {
    return k == ItemKind::VtuberFigurine || k == ItemKind::VtuberHoloCard;
}

inline bool isCaptureSphereEmptyKind(ItemKind k) {
    return k == ItemKind::CaptureSphere || k == ItemKind::MegaSphere;
}

inline bool isCaptureSphereFullKind(ItemKind k) {
    return k == ItemKind::CaptureSphereFull || k == ItemKind::MegaSphereFull;
}

inline bool isCaptureSphereKind(ItemKind k) {
    return isCaptureSphereEmptyKind(k) || isCaptureSphereFullKind(k);
}

// --- Fishing helpers (append-only) ---

inline bool isFishingRodKind(ItemKind k) { return k == ItemKind::FishingRod; }
inline bool isFishKind(ItemKind k) { return k == ItemKind::Fish; }

// --- Crafting helpers (append-only) ---

inline bool isCraftingKitKind(ItemKind k) { return k == ItemKind::CraftingKit; }
inline bool isEssenceShardKind(ItemKind k) { return k == ItemKind::EssenceShard; }

// --- Bounty helpers (append-only) ---

inline bool isBountyContractKind(ItemKind k) { return k == ItemKind::BountyContract; }

inline bool isRuneTabletKind(ItemKind k) { return k == ItemKind::RuneTablet; }

// Items eligible as crafting ingredients.
// We intentionally exclude a few "tool" / meta items so crafting stays focused on loot.
inline bool isCraftIngredientKind(ItemKind k) {
    if (k == ItemKind::CraftingKit) return false;
    if (k == ItemKind::Gold) return false;
    if (k == ItemKind::AmuletYendor) return false;
    if (k == ItemKind::BountyContract) return false;

    // Avoid sacrificing key utility systems for now.
    if (k == ItemKind::FishingRod) return false;
    if (k == ItemKind::GardenHoe) return false;
    if (k == ItemKind::CaptureSphere || k == ItemKind::MegaSphere || k == ItemKind::CaptureSphereFull || k == ItemKind::MegaSphereFull) return false;

    // Avoid containers (primarily ground/storage props).
    if (k == ItemKind::Chest || k == ItemKind::ChestOpen) return false;

    return true;
}

// --- Farming helpers (append-only) ---

inline bool isGardenHoeKind(ItemKind k) { return k == ItemKind::GardenHoe; }
inline bool isSeedKind(ItemKind k) { return k == ItemKind::Seed; }

inline bool isFarmPlotKind(ItemKind k) { return k == ItemKind::TilledSoil; }
inline bool isFarmPlantKind(ItemKind k) {
    return k == ItemKind::CropSprout || k == ItemKind::CropGrowing || k == ItemKind::CropMature;
}
inline bool isCropProduceKind(ItemKind k) { return k == ItemKind::CropProduce; }
inline bool isFarmKind(ItemKind k) {
    return isGardenHoeKind(k) || isSeedKind(k) || isFarmPlotKind(k) || isFarmPlantKind(k) || isCropProduceKind(k);
}

// Crop metadata is packed into Item::enchant for farming items to avoid
// changing the save format.
//
// bits 0..3  : variant (0..15)
// bits 4..6  : rarity  (0..7)
// bit  7     : shiny   (0/1)
// bits 8..11 : quality (0..15) [produce only]
inline int cropVariantFromEnchant(int enchant) { return enchant & 0xF; }
inline int cropRarityFromEnchant(int enchant) { return (enchant >> 4) & 0x7; }
inline bool cropIsShinyFromEnchant(int enchant) { return ((enchant >> 7) & 1) != 0; }
inline int cropQualityFromEnchant(int enchant) { return (enchant >> 8) & 0xF; }

inline int packCropMetaEnchant(int variant, int rarity, bool shiny) {
    const int v = clampi(variant, 0, 15);
    const int r = clampi(rarity, 0, 7);
    const int s = shiny ? 1 : 0;
    // Bit 12 is a tiny signature so callers can treat enchant==0 as "unset".
    // (This keeps future migration painless while preserving old saves.)
    return (v & 0xF) | ((r & 0x7) << 4) | ((s & 0x1) << 7) | (1 << 12);
}

inline int packCropProduceEnchant(int variant, int rarity, bool shiny, int quality) {
    const int q = clampi(quality, 0, 15);
    return packCropMetaEnchant(variant, rarity, shiny) | ((q & 0xF) << 8);
}

// Crop seeds are stored in Item::charges (int32 bits preserved), mirroring fish.
inline uint32_t cropSeedFromCharges(int charges) { return static_cast<uint32_t>(charges); }

// Tilled soil metadata is packed into Item::enchant.
// bits 0..7  : fertility (0..100)
// bits 8..11 : affinity code (0 = none; 1..15 = tagIndex+1)
inline int tilledSoilFertilityFromEnchant(int enchant) { return enchant & 0xFF; }
inline int tilledSoilAffinityFromEnchant(int enchant) {
    const int code = (enchant >> 8) & 0xF;
    return (code <= 0) ? -1 : (code - 1);
}

inline int packTilledSoilEnchant(int fertility, int affinityIdx) {
    const int f = clampi(fertility, 0, 100);
    // affinityIdx is -1 for none; otherwise 0..N-1.
    const int code = (affinityIdx < 0) ? 0 : clampi(affinityIdx + 1, 1, 15);
    return (f & 0xFF) | ((code & 0xF) << 8);
}

// Planted crop metadata packer for future use.
// bits 0..3  : variant
// bits 4..6  : rarity
// bit  7     : shiny
// bits 8..15 : fertility (0..100)
// bits 16..19: affinity code (0 = none; 1..15 = tagIndex+1)
inline int farmPlantFertilityFromEnchant(int enchant) { return (enchant >> 8) & 0xFF; }
inline int farmPlantAffinityFromEnchant(int enchant) {
    const int code = (enchant >> 16) & 0xF;
    return (code <= 0) ? -1 : (code - 1);
}

inline int packFarmPlantEnchant(int variant, int rarity, bool shiny, int fertility, int affinityIdx) {
    const int f = clampi(fertility, 0, 100);
    const int code = (affinityIdx < 0) ? 0 : clampi(affinityIdx + 1, 1, 15);
    return packCropMetaEnchant(variant, rarity, shiny) | ((f & 0xFF) << 8) | ((code & 0xF) << 16);
}

// Fish metadata is packed into Item::enchant for ItemKind::Fish to avoid
// changing the save format.
// bits 0..3  : sizeClass (0..15)
// bits 4..6  : rarity    (0..7)
// bit  7     : shiny     (0/1)
inline int fishSizeClassFromEnchant(int enchant) { return enchant & 0xF; }
inline int fishRarityFromEnchant(int enchant) { return (enchant >> 4) & 0x7; }
inline bool fishIsShinyFromEnchant(int enchant) { return ((enchant >> 7) & 1) != 0; }

inline int packFishEnchant(int sizeClass, int rarity, bool shiny) {
    const int sc = clampi(sizeClass, 0, 15);
    const int rr = clampi(rarity, 0, 7);
    const int sh = shiny ? 1 : 0;
    return (sc & 0xF) | ((rr & 0x7) << 4) | ((sh & 0x1) << 7);
}

// Fish seeds are stored in Item::charges (int32 bits preserved).
inline uint32_t fishSeedFromCharges(int charges) { return static_cast<uint32_t>(charges); }

// --- Butchered corpse products metadata (append-only) ---
//
// We store per-piece nutrition and provenance in Item::enchant so saves remain compatible.
// This is used by ItemKind::ButcheredMeat / ButcheredHide / ButcheredBones.
//
// For MEAT (ItemKind::ButcheredMeat):
//   bits 0..7   : hunger restore per piece (0..255)
//   bits 8..15  : heal amount per piece   (0..255)
//   bits 16..23 : source ItemKind (corpse kind id, 0..255)
//   bits 24..27 : tag id (0..15)  (shared tokens with fish/crops: REGEN/HASTE/SHIELD/AURORA/CLARITY/VENOM/EMBER)
//   bits 28..31 : cut id (0..15)  (display-only)
//
// For HIDE/BONES (ItemKind::ButcheredHide / ButcheredBones):
//   bits 0..7   : quality (0..255)
//   bits 8..15  : variant id (0..255) (HideType/BoneType, future-proof)
//   bits 16..23 : source ItemKind (corpse kind id, 0..255)

inline int butcherMeatHungerFromEnchant(int enchant) { return enchant & 0xFF; }
inline int butcherMeatHealFromEnchant(int enchant) { return (enchant >> 8) & 0xFF; }
inline int butcherSourceKindFromEnchant(int enchant) { return (enchant >> 16) & 0xFF; }
inline int butcherMeatTagFromEnchant(int enchant) { return (enchant >> 24) & 0xF; }
inline int butcherMeatCutFromEnchant(int enchant) { return (enchant >> 28) & 0xF; }

inline int packButcherMeatEnchant(int hungerPerPiece, int healPerPiece, int sourceKind, int tagId, int cutId) {
    const int h = clampi(hungerPerPiece, 0, 255);
    const int hp = clampi(healPerPiece, 0, 255);
    const int src = clampi(sourceKind, 0, 255);
    const int tg = clampi(tagId, 0, 15);
    const int ct = clampi(cutId, 0, 15);
    return (h & 0xFF) | ((hp & 0xFF) << 8) | ((src & 0xFF) << 16) | ((tg & 0xF) << 24) | ((ct & 0xF) << 28);
}

inline int butcherMaterialQualityFromEnchant(int enchant) { return enchant & 0xFF; }
inline int butcherMaterialVariantFromEnchant(int enchant) { return (enchant >> 8) & 0xFF; }

inline int packButcherMaterialEnchant(int sourceKind, int quality, int variant) {
    const int src = clampi(sourceKind, 0, 255);
    const int q = clampi(quality, 0, 255);
    const int v = clampi(variant, 0, 255);
    return (q & 0xFF) | ((v & 0xFF) << 8) | ((src & 0xFF) << 16);
}

// Back-compat convenience overload (old callers had no variant).
inline int packButcherMaterialEnchant(int sourceKind, int quality) {
    return packButcherMaterialEnchant(sourceKind, quality, 0);
}

inline int butcherQualityTierFromQuality(int quality) {
    const int q = clampi(quality, 0, 255);
    return q / 64; // 0..3
}

// Cosmetic helper for UI naming (quality adjective).
inline const char* butcherQualityAdj(int quality) {
    const int q = clampi(quality, 0, 255);
    if (q >= 240) return "MASTERWORK";
    if (q >= 192) return "PRIME";
    if (q >= 128) return "FINE";
    if (q >= 64)  return "TOUGH";
    return "RAGGED";
}

// --- Procedural crafting: Essence Shards metadata (append-only) ---
//
// ItemKind::EssenceShard is a stackable crafting ingredient produced as a
// deterministic byproduct of some crafts. Metadata is stored in Item::enchant.
//
// Item::enchant bits:
//   bits 0..4 : craft tag id (0..31) (see craft_tags.hpp)
//   bits 5..8 : tier (0..15)
//   bit 9     : shiny flag
//   bit 15    : signature (always 1)

inline int essenceShardTagFromEnchant(int enchant) { return enchant & 0x1F; }
inline int essenceShardTierFromEnchant(int enchant) { return (enchant >> 5) & 0xF; }
inline bool essenceShardIsShinyFromEnchant(int enchant) { return ((enchant >> 9) & 0x1) != 0; }

inline int packEssenceShardEnchant(int tagId, int tier, bool shiny) {
    const int tg = clampi(tagId, 0, 31);
    const int t = clampi(tier, 0, 15);
    const int sh = shiny ? 1 : 0;
    return 0x8000 | (tg & 0x1F) | ((t & 0xF) << 5) | ((sh & 0x1) << 9);
}

// --- Bounty contract metadata (append-only) ---
//
// We pack contract state into Item::charges/enchant so saves remain compatible.
//
// Item::charges (32-bit):
//   byte0: target EntityKind id
//   byte1: required kill count (1..255)
//   byte2: reward ItemKind id
//   byte3: reward count (stack size / gold amount)
//
// Item::enchant:
//   low 8 bits: current progress (kills credited)
//
inline int bountyTargetKindFromCharges(int charges) { return charges & 0xFF; }
inline int bountyRequiredKillsFromCharges(int charges) { return (charges >> 8) & 0xFF; }
inline int bountyRewardKindFromCharges(int charges) { return (charges >> 16) & 0xFF; }
inline int bountyRewardCountFromCharges(int charges) { return (charges >> 24) & 0xFF; }

inline int packBountyCharges(int targetKind, int requiredKills, int rewardKind, int rewardCount) {
    const int t = clampi(targetKind, 0, 255);
    const int r = clampi(requiredKills, 0, 255);
    const int k = clampi(rewardKind, 0, 255);
    const int c = clampi(rewardCount, 0, 255);
    return (t & 0xFF) | ((r & 0xFF) << 8) | ((k & 0xFF) << 16) | ((c & 0xFF) << 24);
}

inline int bountyProgressFromEnchant(int enchant) { return enchant & 0xFF; }
inline int withBountyProgress(int enchant, int progress) {
    return (enchant & ~0xFF) | (clampi(progress, 0, 255) & 0xFF);
}



// Capture-sphere tuning (UI + balance).
inline int captureSphereRange(ItemKind k) {
    // A modest throw range; Mega has a small advantage.
    return (k == ItemKind::MegaSphere || k == ItemKind::MegaSphereFull) ? 7 : 6;
}

inline float captureSphereCatchMultiplier(ItemKind k) {
    // Mega spheres have a slightly higher catch rate.
    return (k == ItemKind::MegaSphere || k == ItemKind::MegaSphereFull) ? 1.25f : 1.0f;
}

inline ItemKind captureSphereFilledKind(ItemKind emptyKind) {
    return (emptyKind == ItemKind::MegaSphere) ? ItemKind::MegaSphereFull : ItemKind::CaptureSphereFull;
}

inline ItemKind captureSphereEmptyKind(ItemKind fullKind) {
    return (fullKind == ItemKind::MegaSphereFull) ? ItemKind::MegaSphere : ItemKind::CaptureSphere;
}

// Capture-sphere metadata is packed into Item::charges to avoid changing the save format.
// bits 0..7   : bond   (0..255; currently 0..99)
// bits 8..15  : hp%    (0..100)
// bits 16..23 : level  (0..255; 0 means "legacy/default to 1")
// bits 24..31 : xp     (0..255; progress toward next level)
inline int captureSphereBondFromCharges(int charges) { return charges & 0xFF; }
inline int captureSphereHpPctFromCharges(int charges) { return (charges >> 8) & 0xFF; }
inline int captureSphereLevelFromCharges(int charges) { return (charges >> 16) & 0xFF; }
inline int captureSphereXpFromCharges(int charges) { return (charges >> 24) & 0xFF; }

inline int packCaptureSphereCharges(int bond, int hpPct, int level, int xp) {
    const int b  = (bond  < 0) ? 0 : (bond  > 255 ? 255 : bond);
    const int hp = (hpPct < 0) ? 0 : (hpPct > 255 ? 255 : hpPct);
    const int lv = (level < 0) ? 0 : (level > 255 ? 255 : level);
    const int x  = (xp    < 0) ? 0 : (xp    > 255 ? 255 : xp);
    return (b & 0xFF) | ((hp & 0xFF) << 8) | ((lv & 0xFF) << 16) | ((x & 0xFF) << 24);
}

// Legacy packer (bond + HP only). Leaves level/xp as 0 so older saves still decode.
inline int packCaptureSphereCharges(int bond, int hpPct) {
    return packCaptureSphereCharges(bond, hpPct, /*level=*/0, /*xp=*/0);
}

inline int withCaptureSphereBond(int charges, int bond) {
    return packCaptureSphereCharges(
        bond,
        captureSphereHpPctFromCharges(charges),
        captureSphereLevelFromCharges(charges),
        captureSphereXpFromCharges(charges)
    );
}

inline int withCaptureSphereHpPct(int charges, int hpPct) {
    return packCaptureSphereCharges(
        captureSphereBondFromCharges(charges),
        hpPct,
        captureSphereLevelFromCharges(charges),
        captureSphereXpFromCharges(charges)
    );
}

inline int withCaptureSphereLevel(int charges, int level) {
    return packCaptureSphereCharges(
        captureSphereBondFromCharges(charges),
        captureSphereHpPctFromCharges(charges),
        level,
        captureSphereXpFromCharges(charges)
    );
}

inline int withCaptureSphereXp(int charges, int xp) {
    return packCaptureSphereCharges(
        captureSphereBondFromCharges(charges),
        captureSphereHpPctFromCharges(charges),
        captureSphereLevelFromCharges(charges),
        xp
    );
}

// -----------------------------------------------------------------------------
// Captured companion progression tuning.
// These are intentionally small so pets feel like "party members" without
// eclipsing player gearing.
// -----------------------------------------------------------------------------

inline int captureSpherePetLevelCap() { return 30; }

inline int captureSpherePetLevelOrDefault(int charges) {
    const int lv = captureSphereLevelFromCharges(charges);
    return (lv <= 0) ? 1 : lv;
}

inline int captureSpherePetXpOrZero(int charges) {
    return captureSphereXpFromCharges(charges);
}

// XP needed to advance from `level` to `level+1`.
// Kept <= 255 so we can pack progress into a single byte.
inline int captureSpherePetXpToNext(int level) {
    const int lv = clampi(level, 1, 255);
    // Level 1->2 ~18xp; level 30->31 ~192xp.
    return clampi(12 + lv * 6, 12, 220);
}

inline int captureSpherePetAtkBonus(int level) {
    const int lv = clampi(level, 1, 255);
    return (lv - 1) / 6; // +0..+4 by level 30
}

inline int captureSpherePetDefBonus(int level) {
    const int lv = clampi(level, 1, 255);
    return (lv - 1) / 7; // +0..+4 by level 30
}

inline int captureSpherePetHpBonus(int level) {
    const int lv = clampi(level, 1, 255);
    return (lv - 1) / 2; // +0..+14 by level 30
}

inline bool isChestKind(ItemKind k) {
    return k == ItemKind::Chest || k == ItemKind::ChestOpen;
}

// Ecosystem resource nodes: stationary ground props spawned near biome seeds.
inline bool isEcosystemNodeKind(ItemKind k) {
    switch (k) {
        case ItemKind::SporePod:
        case ItemKind::CrystalNode:
        case ItemKind::BonePile:
        case ItemKind::RustVent:
        case ItemKind::AshVent:
        case ItemKind::GrottoSpring:
            return true;
        default:
            return false;
    }
}

// Stationary props are non-pickup ground items that provide interaction.
inline bool isStationaryPropKind(ItemKind k) {
    return isChestKind(k) || isEcosystemNodeKind(k);
}

inline bool isCorpseKind(ItemKind k) {
    switch (k) {
        case ItemKind::CorpseGoblin:
        case ItemKind::CorpseOrc:
        case ItemKind::CorpseBat:
        case ItemKind::CorpseSlime:
        case ItemKind::CorpseKobold:
        case ItemKind::CorpseWolf:
        case ItemKind::CorpseTroll:
        case ItemKind::CorpseWizard:
        case ItemKind::CorpseSnake:
        case ItemKind::CorpseSpider:
        case ItemKind::CorpseOgre:
        case ItemKind::CorpseMimic:
        case ItemKind::CorpseMinotaur:
            return true;
        default:
            return false;
    }
}

inline bool isButcheredMeatKind(ItemKind k) { return k == ItemKind::ButcheredMeat; }
inline bool isButcheredHideKind(ItemKind k) { return k == ItemKind::ButcheredHide; }
inline bool isButcheredBonesKind(ItemKind k) { return k == ItemKind::ButcheredBones; }
inline bool isButcheredProductKind(ItemKind k) {
    return isButcheredMeatKind(k) || isButcheredHideKind(k) || isButcheredBonesKind(k);
}

inline bool isPotionKind(ItemKind k) {
    switch (k) {
        case ItemKind::PotionHealing:
        case ItemKind::PotionStrength:
        case ItemKind::PotionAntidote:
        case ItemKind::PotionRegeneration:
        case ItemKind::PotionShielding:
        case ItemKind::PotionHaste:
        case ItemKind::PotionVision:
        case ItemKind::PotionInvisibility:
        case ItemKind::PotionClarity:
        case ItemKind::PotionLevitation:
        case ItemKind::PotionHallucination:
        case ItemKind::PotionEnergy:
            return true;
        default:
            return false;
    }
}

inline bool isScrollKind(ItemKind k) {
    switch (k) {
        case ItemKind::ScrollTeleport:
        case ItemKind::ScrollMapping:
        case ItemKind::ScrollEnchantWeapon:
        case ItemKind::ScrollEnchantArmor:
        case ItemKind::ScrollIdentify:
        case ItemKind::ScrollDetectTraps:
        case ItemKind::ScrollDetectSecrets:
        case ItemKind::ScrollRemoveCurse:
        case ItemKind::ScrollConfusion:
        case ItemKind::ScrollFear:
        case ItemKind::ScrollEarth:
        case ItemKind::ScrollTaming:
        case ItemKind::ScrollEnchantRing:
            return true;
        case ItemKind::ScrollKnock:
            return true;
        default:
            return false;
    }
}

inline bool isSpellbookKind(ItemKind k) {
    switch (k) {
        case ItemKind::SpellbookMagicMissile:
        case ItemKind::SpellbookBlink:
        case ItemKind::SpellbookMinorHeal:
        case ItemKind::SpellbookDetectTraps:
        case ItemKind::SpellbookFireball:
        case ItemKind::SpellbookStoneskin:
        case ItemKind::SpellbookHaste:
        case ItemKind::SpellbookInvisibility:
        case ItemKind::SpellbookPoisonCloud:
            return true;
        default:
            return false;
    }
}


struct ItemDef {
    ItemKind kind;
    const char* name;

    bool stackable = false;
    bool consumable = false;
    bool isGold = false;

    EquipSlot slot = EquipSlot::None;

    // Stat modifiers
    int meleeAtk = 0;
    int rangedAtk = 0;
    int defense = 0;

    // Ranged properties
    int range = 0;              // 0 means not ranged
    AmmoKind ammo = AmmoKind::None;
    ProjectileKind projectile = ProjectileKind::Arrow;

    // Wand-like charges
    int maxCharges = 0;

    // Consumable effects
    int healAmount = 0;
    int hungerRestore = 0; // 0 = no hunger effect

    // Encumbrance / carrying
    // Simple integer "weight" units used by the optional encumbrance system.
    // 0 means weightless (e.g., gold by default).
    int weight = 0;

    // Economy / shops: base value in gold for one unit of this item.
    // 0 means "not normally sold" (e.g., gold itself, quest items, decorative props).
    int value = 0;

    // Talent/stat modifiers granted while equipped.
    // These are primarily used by rings (and are append-only for future gear types).
    int modMight = 0;
    int modAgility = 0;
    int modVigor = 0;
    int modFocus = 0;
};

struct Item {
    int id = 0;
    ItemKind kind = ItemKind::Dagger;
    int count = 1;          // for stackables
    int charges = 0;        // for wands / torches (fuel)
    int enchant = 0;        // for weapons/armor (+/-), 0 = normal
    int buc = 0;            // -1 = cursed, 0 = uncursed, +1 = blessed (primarily for gear)
    uint32_t spriteSeed = 0;

    // Shops: if >0, this item is tagged with a shop price (per-unit) and ownership.
    // shopDepth tracks which dungeon depth the shop belongs to.
    // In inventory, nonzero shopPrice means the item is UNPAID (debt).
    int shopPrice = 0;
    int shopDepth = 0;

    // Item ego / brand (rare). Used primarily for melee weapons.
    ItemEgo ego = ItemEgo::None;

    // Misc item flags (append-only).
    // Used to tag special ground items (e.g. item mimics).
    uint8_t flags = 0;
};

struct GroundItem {
    Item item;
    Vec2i pos;
};

// Item flags (append-only).
// NOTE: flags are serialized; only add new bits at the end.
inline constexpr uint8_t ITEM_FLAG_MIMIC_BAIT = 1u << 0;
inline constexpr uint8_t ITEM_FLAG_ARTIFACT  = 1u << 1;
// Stationary items cannot be picked up (used for ground-only props like plots).
inline constexpr uint8_t ITEM_FLAG_STATIONARY = 1u << 2;

inline bool itemIsMimicBait(const Item& it) { return (it.flags & ITEM_FLAG_MIMIC_BAIT) != 0; }
inline void setItemMimicBait(Item& it, bool v) {
    if (v) it.flags = static_cast<uint8_t>(it.flags | ITEM_FLAG_MIMIC_BAIT);
    else   it.flags = static_cast<uint8_t>(it.flags & static_cast<uint8_t>(~ITEM_FLAG_MIMIC_BAIT));
}

inline bool itemIsArtifact(const Item& it) { return (it.flags & ITEM_FLAG_ARTIFACT) != 0; }
inline void setItemArtifact(Item& it, bool v) {
    if (v) it.flags = static_cast<uint8_t>(it.flags | ITEM_FLAG_ARTIFACT);
    else   it.flags = static_cast<uint8_t>(it.flags & static_cast<uint8_t>(~ITEM_FLAG_ARTIFACT));
}

inline bool itemIsStationary(const Item& it) { return (it.flags & ITEM_FLAG_STATIONARY) != 0; }
inline void setItemStationary(Item& it, bool v) {
    if (v) it.flags = static_cast<uint8_t>(it.flags | ITEM_FLAG_STATIONARY);
    else   it.flags = static_cast<uint8_t>(it.flags & static_cast<uint8_t>(~ITEM_FLAG_STATIONARY));
}

const ItemDef& itemDef(ItemKind k);

inline bool isStackable(ItemKind k) { return itemDef(k).stackable; }
inline bool isConsumable(ItemKind k) { return itemDef(k).consumable; }
inline bool isGold(ItemKind k) { return itemDef(k).isGold; }
inline EquipSlot equipSlot(ItemKind k) { return itemDef(k).slot; }
inline bool isMeleeWeapon(ItemKind k) { return equipSlot(k) == EquipSlot::MeleeWeapon; }
inline bool isRangedWeapon(ItemKind k) { return equipSlot(k) == EquipSlot::RangedWeapon; }
inline bool isWeapon(ItemKind k) { return isMeleeWeapon(k) || isRangedWeapon(k); }
inline bool isArmor(ItemKind k) { return equipSlot(k) == EquipSlot::Armor; }
inline bool isRingKind(ItemKind k) { return equipSlot(k) == EquipSlot::Ring; }

// Wands are ranged weapons that use charges (maxCharges>0) and do not require ammo.
inline bool isWandKind(ItemKind k) {
    const ItemDef& d = itemDef(k);
    return isRangedWeapon(k) && d.maxCharges > 0 && d.ammo == AmmoKind::None;
}

// Identifiable items start unknown each run and use randomized appearances.
inline bool isIdentifiableKind(ItemKind k) {
    return isPotionKind(k) || isScrollKind(k) || isRingKind(k) || isWandKind(k);
}


// Convenience: "gear" in ProcRogue means an equipable item subject to BUC / enchant rules.
inline bool isWearableGear(ItemKind k) { return isWeapon(k) || isArmor(k) || isRingKind(k); }

std::string itemDisplayName(const Item& it);
std::string itemDisplayNameSingle(ItemKind k);

// Encumbrance helpers
int itemWeight(const Item& it);
int totalWeight(const std::vector<Item>& items);

// Inventory helpers
int countGold(const std::vector<Item>& inv);
int findItemIndexById(const std::vector<Item>& inv, int itemId);
int findFirstAmmoIndex(const std::vector<Item>& inv, AmmoKind ammo);
int ammoCount(const std::vector<Item>& inv, AmmoKind ammo);

// Consumes up to `amount` ammo from matching stacks. Returns true if fully consumed.
bool consumeAmmo(std::vector<Item>& inv, AmmoKind ammo, int amount);

// Consumes exactly 1 ammo and optionally returns a template Item (count=1) preserving metadata
// like shopPrice/shopDepth so projectiles can be recovered without laundering shop debt.
bool consumeOneAmmo(std::vector<Item>& inv, AmmoKind ammo, Item* outConsumed = nullptr);

// Stacking: tries to merge `incoming` into existing stack in `inv` if possible.
// Returns true if merged; false if caller should push as new entry.
bool tryStackItem(std::vector<Item>& inv, const Item& incoming);
