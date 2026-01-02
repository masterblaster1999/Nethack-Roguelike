#pragma once

#include "game.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Hash helper for enum class keys in unordered_map.
struct EnumClassHash {
    template <typename T>
    std::size_t operator()(T t) const noexcept {
        return static_cast<std::size_t>(t);
    }
};

enum class SpawnCategory : uint8_t {
    Room = 0,
    Guardian = 1,
};

struct SpawnEntry {
    EntityKind kind = EntityKind::Goblin;
    int weight = 1;
};

struct MonsterStatsOverride {
    std::optional<int> hpMax;
    std::optional<int> baseAtk;
    std::optional<int> baseDef;

    std::optional<bool> willFlee;
    std::optional<bool> packAI;

    std::optional<bool> canRanged;
    std::optional<int> rangedRange;
    std::optional<int> rangedAtk;
    std::optional<ProjectileKind> rangedProjectile;
    std::optional<AmmoKind> rangedAmmo;

    std::optional<int> regenChancePct;
    std::optional<int> regenAmount;
};

struct ItemDefOverride {
    std::optional<int> meleeAtk;
    std::optional<int> rangedAtk;
    std::optional<int> defense;
    std::optional<int> range;
    std::optional<int> maxCharges;
    std::optional<int> healAmount;
    std::optional<int> hungerRestore;
    std::optional<int> weight;
    std::optional<int> value;
    std::optional<int> modMight;
    std::optional<int> modAgility;
    std::optional<int> modVigor;
    std::optional<int> modFocus;
};

struct SpawnTableOverride {
    std::unordered_map<EntityKind, int, EnumClassHash> weights;
};

struct ContentOverrides {
    // Parsed from an INI-ish override file.
    std::unordered_map<EntityKind, MonsterStatsOverride, EnumClassHash> monsters;
    std::unordered_map<ItemKind, ItemDefOverride, EnumClassHash> items;

    // Spawn tables: weights per monster per depth.
    std::unordered_map<int, SpawnTableOverride> spawnRoom;
    std::unordered_map<int, SpawnTableOverride> spawnGuardian;

    // Hash of the source file (FNV-1a 64-bit) for reproducibility.
    uint64_t sourceHash = 0;
};

// Load content overrides from a user-editable INI-ish file.
// Returns false only if the file could not be read. Parsing issues are reported in outWarnings.
bool loadContentOverridesIni(const std::string& path, ContentOverrides& out, std::string* outWarnings = nullptr);

// Global override state.
void setContentOverrides(ContentOverrides overrides);
void clearContentOverrides();
const ContentOverrides& contentOverrides();
uint32_t contentOverridesGeneration();
uint64_t contentOverridesHash();

// Spawn helpers (effective tables = defaults + overrides).
std::vector<SpawnEntry> effectiveSpawnTable(SpawnCategory category, int depth);
EntityKind pickSpawnMonster(SpawnCategory category, RNG& rng, int depth);

// Helper parsers exposed for tooling/tests.
bool parseEntityKindId(const std::string& id, EntityKind& out);
bool parseItemKindId(const std::string& id, ItemKind& out);
