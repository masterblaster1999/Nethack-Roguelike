#include "content.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

ContentOverrides g_overrides;
uint32_t g_generation = 0;
uint64_t g_hash = 0;

uint64_t fnv1a64(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::string ltrim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    return s;
}

std::string rtrim(std::string s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
    return s;
}

std::string trim(std::string s) {
    return rtrim(ltrim(std::move(s)));
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void stripUtf8Bom(std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

std::string sanitizeId(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    auto pushUnderscore = [&]() {
        if (!out.empty() && out.back() != '_') out.push_back('_');
    };

    for (unsigned char c : raw) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (c == ' ' || c == '_' || c == '-') {
            pushUnderscore();
        } else {
            // Skip punctuation.
        }
    }

    // Trim leading/trailing underscores.
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();

    return out;
}

bool parseInt(const std::string& raw, int& out) {
    try {
        size_t idx = 0;
        const int v = std::stoi(trim(raw), &idx, 0);
        // Ensure the whole string was consumed (allow trailing whitespace).
        for (size_t i = idx; i < raw.size(); ++i) {
            if (!std::isspace(static_cast<unsigned char>(raw[i]))) return false;
        }
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseBool(const std::string& raw, bool& out) {
    const std::string v = sanitizeId(raw);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parseProjectileKind(const std::string& raw, ProjectileKind& out) {
    const std::string v = sanitizeId(raw);
    if (v == "arrow") {
        out = ProjectileKind::Arrow;
        return true;
    }
    if (v == "rock") {
        out = ProjectileKind::Rock;
        return true;
    }
    if (v == "spark" || v == "sparks") {
        out = ProjectileKind::Spark;
        return true;
    }
    if (v == "fireball") {
        out = ProjectileKind::Fireball;
        return true;
    }
    if (v == "torch" || v == "lit_torch" || v == "torchlit") {
        out = ProjectileKind::Torch;
        return true;
    }
    return false;
}

bool parseAmmoKind(const std::string& raw, AmmoKind& out) {
    const std::string v = sanitizeId(raw);
    if (v == "none") {
        out = AmmoKind::None;
        return true;
    }
    if (v == "arrow" || v == "arrows") {
        out = AmmoKind::Arrow;
        return true;
    }
    if (v == "rock" || v == "rocks") {
        out = AmmoKind::Rock;
        return true;
    }
    return false;
}

bool parseSpawnCategory(const std::string& raw, SpawnCategory& out) {
    const std::string v = sanitizeId(raw);
    if (v == "room" || v == "rooms" || v == "normal") {
        out = SpawnCategory::Room;
        return true;
    }
    if (v == "guardian" || v == "guard" || v == "guards") {
        out = SpawnCategory::Guardian;
        return true;
    }
    return false;
}

std::vector<std::string> splitDot(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string joinTokensUnderscore(const std::vector<std::string>& toks, size_t start) {
    std::string out;
    for (size_t i = start; i < toks.size(); ++i) {
        if (!out.empty()) out.push_back('_');
        out += toks[i];
    }
    return out;
}

void appendWarning(std::string& w, int lineNo, const std::string& msg, int& warnCount, int warnLimit = 30) {
    if (warnCount < warnLimit) {
        w += "Line " + std::to_string(lineNo) + ": " + msg + "\n";
    } else if (warnCount == warnLimit) {
        w += "(more warnings omitted...)\n";
    }
    ++warnCount;
}

const std::unordered_map<std::string, EntityKind>& entityIdMap() {
    static const std::unordered_map<std::string, EntityKind> map = [] {
        std::unordered_map<std::string, EntityKind> m;
        m.reserve(static_cast<size_t>(ENTITY_KIND_COUNT));
        for (int i = 0; i < ENTITY_KIND_COUNT; ++i) {
            EntityKind k = static_cast<EntityKind>(i);
            m.emplace(sanitizeId(entityKindName(k)), k);
        }
        // Some friendly aliases.
        m.emplace("skeletonarcher", EntityKind::SkeletonArcher);
        m.emplace("koboldslinger", EntityKind::KoboldSlinger);
        return m;
    }();
    return map;
}

const std::unordered_map<std::string, ItemKind>& itemIdMap() {
    static std::unordered_map<std::string, ItemKind> map = [] {
        std::unordered_map<std::string, ItemKind> m;
        m.reserve(static_cast<size_t>(ITEM_KIND_COUNT));
        for (int i = 0; i < ITEM_KIND_COUNT; ++i) {
            ItemKind k = static_cast<ItemKind>(i);
            m.emplace(sanitizeId(itemDef(k).name), k);
        }
        return m;
    }();
    return map;
}

// ------------------------------------------------------------
// Default spawn tables (weights roughly match the previous hand-coded thresholds).
// ------------------------------------------------------------

std::vector<SpawnEntry> defaultRoomSpawnTable(int depth) {
    depth = std::max(1, std::min(depth, Game::DUNGEON_MAX_DEPTH));

    if (depth <= 1) {
        return {
            {EntityKind::Goblin, 65},
            {EntityKind::Orc, 35},
        };
    }
    if (depth == 2) {
        return {
            {EntityKind::Goblin, 35},
            {EntityKind::Orc, 20},
            {EntityKind::Bat, 15},
            {EntityKind::Slime, 15},
            {EntityKind::KoboldSlinger, 15},
        };
    }
    if (depth == 3) {
        return {
            {EntityKind::Orc, 20},
            {EntityKind::SkeletonArcher, 15},
            {EntityKind::Spider, 15},
            {EntityKind::Snake, 15},
            {EntityKind::Bat, 15},
            {EntityKind::Wolf, 20},
        };
    }

    if (depth == 4 || depth == 5) {
        return {
            {EntityKind::Orc, 12},
            {EntityKind::SkeletonArcher, 10},
            {EntityKind::Spider, 10},
            {EntityKind::Wolf, 10},
            {EntityKind::Slime, 10},
            {EntityKind::Bat, 8},
            {EntityKind::Snake, 10},
            {EntityKind::KoboldSlinger, 10},
            {EntityKind::Troll, 10},
            {EntityKind::Ogre, 5},
            {EntityKind::Wizard, 3},
            {EntityKind::Leprechaun, 2},
        };
    }

    if (depth == 6) {
        return {
            {EntityKind::Orc, 12},
            {EntityKind::SkeletonArcher, 10},
            {EntityKind::Spider, 10},
            {EntityKind::Wolf, 10},
            {EntityKind::Slime, 10},
            {EntityKind::Bat, 8},
            {EntityKind::Snake, 10},
            {EntityKind::KoboldSlinger, 10},
            {EntityKind::Troll, 10},
            {EntityKind::Ogre, 5},
            {EntityKind::Mimic, 3},
            {EntityKind::Leprechaun, 2},
        };
    }

    // --- Deeper run support ---
    // The run is longer now (default 20 floors), so we use broader depth "bands"
    // rather than a single 7+ table. This keeps the last 10 floors from feeling
    // like pure filler and introduces undead/ethereal threats gradually.

    // Depth 7-9: early-deep (introduce mimics/wizards; rare Minotaur).
    if (depth <= 9) {
        return {
            {EntityKind::Orc, 10},
            {EntityKind::SkeletonArcher, 12},
            {EntityKind::Spider, 10},
            {EntityKind::Troll, 12},
            {EntityKind::Ogre, 10},
            {EntityKind::Mimic, 10},
            {EntityKind::Wizard, 10},
            {EntityKind::Wolf, 10},
            {EntityKind::KoboldSlinger, 6},
            {EntityKind::Slime, 4},
            {EntityKind::Snake, 2},
            {EntityKind::Leprechaun, 1},
            {EntityKind::Minotaur, 1},
        };
    }

    // Depth 10: true midpoint spike (stronger mixed packs + first real undead pressure).
    if (depth == 10) {
        return {
            {EntityKind::SkeletonArcher, 12},
            {EntityKind::Troll, 14},
            {EntityKind::Ogre, 14},
            {EntityKind::Mimic, 12},
            {EntityKind::Wizard, 12},
            {EntityKind::Zombie, 6},
            {EntityKind::Minotaur, 3},
            {EntityKind::Ghost, 2},
            {EntityKind::Spider, 6},
            {EntityKind::Wolf, 6},
            {EntityKind::KoboldSlinger, 4},
            {EntityKind::Leprechaun, 1},
        };
    }

    // Depth 11-14: late band (undead + heavier elites).
    if (depth <= 14) {
        return {
            {EntityKind::SkeletonArcher, 10},
            {EntityKind::Troll, 14},
            {EntityKind::Ogre, 14},
            {EntityKind::Mimic, 12},
            {EntityKind::Wizard, 12},
            {EntityKind::Zombie, 10},
            {EntityKind::Minotaur, 4},
            {EntityKind::Ghost, 4},
            {EntityKind::Spider, 4},
            {EntityKind::Wolf, 4},
            {EntityKind::KoboldSlinger, 2},
            {EntityKind::Leprechaun, 1},
        };
    }

    // Depth 15-19: very deep (frequent undead/Minotaurs; rooms are dangerous).
    if (depth < Game::QUEST_DEPTH) {
        return {
            {EntityKind::Wizard, 14},
            {EntityKind::Mimic, 12},
            {EntityKind::Troll, 14},
            {EntityKind::Ogre, 14},
            {EntityKind::Zombie, 12},
            {EntityKind::Ghost, 6},
            {EntityKind::Minotaur, 6},
            {EntityKind::SkeletonArcher, 8},
            {EntityKind::Spider, 3},
            {EntityKind::Wolf, 3},
            {EntityKind::Leprechaun, 1},
        };
    }

    // Final floor: keep Minotaurs off the sanctum (endgame boss is different).
    return {
        {EntityKind::Mimic, 14},
        {EntityKind::Troll, 12},
        {EntityKind::Ogre, 12},
        {EntityKind::SkeletonArcher, 10},
        {EntityKind::Zombie, 12},
        {EntityKind::Ghost, 8},
        {EntityKind::Wizard, 8},
        {EntityKind::Spider, 4},
    };
}

std::vector<SpawnEntry> defaultGuardianSpawnTable(int depth) {
    depth = std::max(1, std::min(depth, Game::DUNGEON_MAX_DEPTH));

    if (depth >= 7) {
        // Deeper floors: guardians skew toward elites and ranged pressure.
        if (depth == Game::QUEST_DEPTH) {
            // Keep Minotaurs off the final floor; the endgame boss is different.
            return {
                {EntityKind::Wizard, 24},
                {EntityKind::Ogre, 14},
                {EntityKind::Troll, 14},
                {EntityKind::Mimic, 16},
                {EntityKind::SkeletonArcher, 18},
                {EntityKind::Zombie, 8},
                {EntityKind::Ghost, 6},
            };
        }

        if (depth >= 15) {
            return {
                {EntityKind::Wizard, 22},
                {EntityKind::Ogre, 12},
                {EntityKind::Troll, 12},
                {EntityKind::Mimic, 16},
                {EntityKind::SkeletonArcher, 16},
                {EntityKind::Minotaur, 8},
                {EntityKind::Zombie, 10},
                {EntityKind::Ghost, 4},
                {EntityKind::Spider, 4},
            };
        }

        if (depth >= 10) {
            return {
                {EntityKind::Wizard, 20},
                {EntityKind::Ogre, 14},
                {EntityKind::Troll, 14},
                {EntityKind::Mimic, 16},
                {EntityKind::SkeletonArcher, 18},
                {EntityKind::Minotaur, 6},
                {EntityKind::Zombie, 6},
                {EntityKind::Ghost, 2},
                {EntityKind::Spider, 4},
            };
        }

        // Depth 7-9: baseline deep guardian mix (rare Minotaur).
        return {
            {EntityKind::Wizard, 20},
            {EntityKind::Ogre, 15},
            {EntityKind::Troll, 15},
            {EntityKind::Mimic, 15},
            {EntityKind::Spider, 7},
            {EntityKind::Minotaur, 3},
            {EntityKind::SkeletonArcher, 25},
        };
    }

    if (depth >= 4) {
        return {
            {EntityKind::Wizard, 25},
            {EntityKind::Ogre, 30},
            {EntityKind::Troll, 45},
        };
    }

    if (depth == 3) {
        return {
            {EntityKind::Orc, 25},
            {EntityKind::SkeletonArcher, 35},
            {EntityKind::Spider, 40},
        };
    }

    return {
        {EntityKind::Goblin, 50},
        {EntityKind::Orc, 50},
    };
}

void applySpawnOverride(std::vector<SpawnEntry>& table, const SpawnTableOverride& ov) {
    if (ov.weights.empty()) return;

    std::unordered_map<EntityKind, size_t, EnumClassHash> idx;
    idx.reserve(table.size());
    for (size_t i = 0; i < table.size(); ++i) idx.emplace(table[i].kind, i);

    for (const auto& kv : ov.weights) {
        const EntityKind k = kv.first;
        const int w = kv.second;
        auto it = idx.find(k);
        if (it != idx.end()) {
            table[it->second].weight = w;
        } else if (w > 0) {
            idx.emplace(k, table.size());
            table.push_back({k, w});
        }
    }

    table.erase(std::remove_if(table.begin(), table.end(), [](const SpawnEntry& e) { return e.weight <= 0; }), table.end());

    if (table.empty()) {
        table.push_back({EntityKind::Goblin, 1});
    }
}

struct SpawnCaches {
    std::array<std::vector<SpawnEntry>, Game::DUNGEON_MAX_DEPTH + 1> room;
    std::array<std::vector<SpawnEntry>, Game::DUNGEON_MAX_DEPTH + 1> guardian;
    uint32_t generation = 0;
};

SpawnCaches g_spawnCaches;

void rebuildSpawnCachesIfNeeded() {
    if (g_spawnCaches.generation == g_generation) return;

    for (int depth = 1; depth <= Game::DUNGEON_MAX_DEPTH; ++depth) {
        g_spawnCaches.room[depth] = defaultRoomSpawnTable(depth);
        g_spawnCaches.guardian[depth] = defaultGuardianSpawnTable(depth);

        auto itRoom = g_overrides.spawnRoom.find(depth);
        if (itRoom != g_overrides.spawnRoom.end()) {
            applySpawnOverride(g_spawnCaches.room[depth], itRoom->second);
        }
        auto itG = g_overrides.spawnGuardian.find(depth);
        if (itG != g_overrides.spawnGuardian.end()) {
            applySpawnOverride(g_spawnCaches.guardian[depth], itG->second);
        }
    }

    g_spawnCaches.generation = g_generation;
}

} // namespace

bool parseEntityKindId(const std::string& id, EntityKind& out) {
    const std::string key = sanitizeId(id);
    const auto& map = entityIdMap();
    auto it = map.find(key);
    if (it == map.end()) return false;
    out = it->second;
    return true;
}

bool parseItemKindId(const std::string& id, ItemKind& out) {
    const std::string key = sanitizeId(id);
    const auto& map = itemIdMap();
    auto it = map.find(key);
    if (it == map.end()) return false;
    out = it->second;
    return true;
}

bool loadContentOverridesIni(const std::string& path, ContentOverrides& out, std::string* outWarnings) {
    out = ContentOverrides{};

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (outWarnings) *outWarnings = "Could not open content file: " + path;
        return false;
    }

    std::string contents;
    {
        std::ostringstream oss;
        oss << f.rdbuf();
        contents = oss.str();
    }

    out.sourceHash = fnv1a64(contents.data(), contents.size());

    std::istringstream iss(contents);
    std::string line;

    std::string warnings;
    int warnCount = 0;

    for (int lineNo = 1; std::getline(iss, line); ++lineNo) {
        stripUtf8Bom(line);

        // Strip comments (# or ;) but do not attempt to handle quoted strings.
        size_t commentPos = std::string::npos;
        size_t pHash = line.find('#');
        size_t pSemi = line.find(';');
        if (pHash != std::string::npos) commentPos = pHash;
        if (pSemi != std::string::npos) commentPos = std::min(commentPos, pSemi);
        if (commentPos != std::string::npos) line = line.substr(0, commentPos);

        line = trim(std::move(line));
        if (line.empty()) continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            appendWarning(warnings, lineNo, "Expected key=value", warnCount);
            continue;
        }

        std::string key = toLower(trim(line.substr(0, eq)));
        std::string val = trim(line.substr(eq + 1));

        if (key.empty()) {
            appendWarning(warnings, lineNo, "Empty key", warnCount);
            continue;
        }

        std::vector<std::string> toks = splitDot(key);
        if (toks.empty()) continue;

        const std::string head = toks[0];

        if (head == "monster") {
            if (toks.size() < 3) {
                appendWarning(warnings, lineNo, "Monster key should be monster.<id>.<field>", warnCount);
                continue;
            }

            EntityKind mk;
            if (!parseEntityKindId(toks[1], mk)) {
                appendWarning(warnings, lineNo, "Unknown monster id: " + toks[1], warnCount);
                continue;
            }

            const std::string field = joinTokensUnderscore(toks, 2);
            MonsterStatsOverride& ov = out.monsters[mk];

            int iv = 0;
            bool bv = false;
            ProjectileKind pk;
            AmmoKind ak;

            if (field == "hp" || field == "hpmax" || field == "hp_max") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for hp_max", warnCount);
                    continue;
                }
                ov.hpMax = iv;
            } else if (field == "atk" || field == "base_atk" || field == "baseatk") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for base_atk", warnCount);
                    continue;
                }
                ov.baseAtk = iv;
            } else if (field == "def" || field == "base_def" || field == "basedef") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for base_def", warnCount);
                    continue;
                }
                ov.baseDef = iv;
            } else if (field == "will_flee" || field == "flee") {
                if (!parseBool(val, bv)) {
                    appendWarning(warnings, lineNo, "Invalid bool for will_flee", warnCount);
                    continue;
                }
                ov.willFlee = bv;
            } else if (field == "pack_ai" || field == "pack") {
                if (!parseBool(val, bv)) {
                    appendWarning(warnings, lineNo, "Invalid bool for pack_ai", warnCount);
                    continue;
                }
                ov.packAI = bv;
            } else if (field == "can_ranged" || field == "ranged") {
                if (!parseBool(val, bv)) {
                    appendWarning(warnings, lineNo, "Invalid bool for can_ranged", warnCount);
                    continue;
                }
                ov.canRanged = bv;
            } else if (field == "ranged_range" || field == "range_ranged") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for ranged_range", warnCount);
                    continue;
                }
                ov.rangedRange = iv;
            } else if (field == "ranged_atk" || field == "atk_ranged") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for ranged_atk", warnCount);
                    continue;
                }
                ov.rangedAtk = iv;
            } else if (field == "ranged_projectile" || field == "projectile") {
                if (!parseProjectileKind(val, pk)) {
                    appendWarning(warnings, lineNo, "Invalid projectile kind", warnCount);
                    continue;
                }
                ov.rangedProjectile = pk;
            } else if (field == "ranged_ammo" || field == "ammo") {
                if (!parseAmmoKind(val, ak)) {
                    appendWarning(warnings, lineNo, "Invalid ammo kind", warnCount);
                    continue;
                }
                ov.rangedAmmo = ak;
            } else if (field == "regen_chance" || field == "regen_chance_pct") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for regen_chance_pct", warnCount);
                    continue;
                }
                ov.regenChancePct = iv;
            } else if (field == "regen_amount") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for regen_amount", warnCount);
                    continue;
                }
                ov.regenAmount = iv;
            } else {
                appendWarning(warnings, lineNo, "Unknown monster field: " + field, warnCount);
                continue;
            }
        } else if (head == "item") {
            if (toks.size() < 3) {
                appendWarning(warnings, lineNo, "Item key should be item.<id>.<field>", warnCount);
                continue;
            }

            ItemKind ik;
            if (!parseItemKindId(toks[1], ik)) {
                appendWarning(warnings, lineNo, "Unknown item id: " + toks[1], warnCount);
                continue;
            }

            const std::string field = joinTokensUnderscore(toks, 2);
            ItemDefOverride& ov = out.items[ik];

            int iv = 0;
            if (field == "melee_atk" || field == "atk" || field == "melee") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for melee_atk", warnCount);
                    continue;
                }
                ov.meleeAtk = iv;
            } else if (field == "ranged_atk" || field == "atk_ranged" || field == "ranged") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for ranged_atk", warnCount);
                    continue;
                }
                ov.rangedAtk = iv;
            } else if (field == "defense" || field == "def") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for defense", warnCount);
                    continue;
                }
                ov.defense = iv;
            } else if (field == "range") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for range", warnCount);
                    continue;
                }
                ov.range = iv;
            } else if (field == "max_charges" || field == "charges") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for max_charges", warnCount);
                    continue;
                }
                ov.maxCharges = iv;
            } else if (field == "heal_amount" || field == "heal") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for heal_amount", warnCount);
                    continue;
                }
                ov.healAmount = iv;
            } else if (field == "hunger_restore" || field == "hunger") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for hunger_restore", warnCount);
                    continue;
                }
                ov.hungerRestore = iv;
            } else if (field == "weight") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for weight", warnCount);
                    continue;
                }
                ov.weight = iv;
            } else if (field == "value" || field == "price") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for value", warnCount);
                    continue;
                }
                ov.value = iv;
            } else if (field == "mod_might" || field == "might") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for mod_might", warnCount);
                    continue;
                }
                ov.modMight = iv;
            } else if (field == "mod_agility" || field == "agility") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for mod_agility", warnCount);
                    continue;
                }
                ov.modAgility = iv;
            } else if (field == "mod_vigor" || field == "vigor") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for mod_vigor", warnCount);
                    continue;
                }
                ov.modVigor = iv;
            } else if (field == "mod_focus" || field == "focus") {
                if (!parseInt(val, iv)) {
                    appendWarning(warnings, lineNo, "Invalid int for mod_focus", warnCount);
                    continue;
                }
                ov.modFocus = iv;
            } else {
                appendWarning(warnings, lineNo, "Unknown item field: " + field, warnCount);
                continue;
            }
        } else if (head == "spawn") {
            if (toks.size() < 4) {
                appendWarning(warnings, lineNo, "Spawn key should be spawn.<room|guardian>.<depth>.<monster>", warnCount);
                continue;
            }

            SpawnCategory cat;
            if (!parseSpawnCategory(toks[1], cat)) {
                appendWarning(warnings, lineNo, "Unknown spawn category: " + toks[1], warnCount);
                continue;
            }

            int depth = 0;
            if (!parseInt(toks[2], depth)) {
                appendWarning(warnings, lineNo, "Invalid spawn depth: " + toks[2], warnCount);
                continue;
            }
            if (depth < 1 || depth > Game::DUNGEON_MAX_DEPTH) {
                appendWarning(warnings, lineNo, "Spawn depth out of range: " + std::to_string(depth), warnCount);
                continue;
            }

            EntityKind mk;
            if (!parseEntityKindId(toks[3], mk)) {
                appendWarning(warnings, lineNo, "Unknown spawn monster id: " + toks[3], warnCount);
                continue;
            }

            int w = 0;
            if (!parseInt(val, w)) {
                appendWarning(warnings, lineNo, "Invalid int spawn weight", warnCount);
                continue;
            }

            SpawnTableOverride* dst = nullptr;
            if (cat == SpawnCategory::Room) {
                dst = &out.spawnRoom[depth];
            } else {
                dst = &out.spawnGuardian[depth];
            }

            dst->weights[mk] = w;
        } else {
            appendWarning(warnings, lineNo, "Unknown key group: " + head, warnCount);
            continue;
        }
    }

    if (outWarnings) {
        *outWarnings = warnings;
    }

    return true;
}

void setContentOverrides(ContentOverrides overrides) {
    g_overrides = std::move(overrides);
    g_hash = g_overrides.sourceHash;
    ++g_generation;
}

void clearContentOverrides() {
    g_overrides = ContentOverrides{};
    g_hash = 0;
    ++g_generation;
}

const ContentOverrides& contentOverrides() {
    return g_overrides;
}

uint32_t contentOverridesGeneration() {
    return g_generation;
}

uint64_t contentOverridesHash() {
    return g_hash;
}

std::vector<SpawnEntry> effectiveSpawnTable(SpawnCategory category, int depth) {
    rebuildSpawnCachesIfNeeded();
    depth = std::max(1, std::min(depth, Game::DUNGEON_MAX_DEPTH));

    if (category == SpawnCategory::Guardian) {
        return g_spawnCaches.guardian[depth];
    }
    return g_spawnCaches.room[depth];
}

EntityKind pickSpawnMonster(SpawnCategory category, RNG& rng, int depth) {
    rebuildSpawnCachesIfNeeded();
    depth = std::max(1, std::min(depth, Game::DUNGEON_MAX_DEPTH));

    const std::vector<SpawnEntry>& table = (category == SpawnCategory::Guardian)
        ? g_spawnCaches.guardian[depth]
        : g_spawnCaches.room[depth];

    int total = 0;
    for (const auto& e : table) {
        if (e.weight > 0) total += e.weight;
    }
    if (total <= 0) return EntityKind::Goblin;

    int roll = rng.range(0, total - 1);
    for (const auto& e : table) {
        roll -= e.weight;
        if (roll < 0) return e.kind;
    }
    return table.empty() ? EntityKind::Goblin : table.back().kind;
}

// ------------------------------------------------------------
// Monster base stats
// ------------------------------------------------------------

MonsterBaseStats baseMonsterStatsFor(EntityKind k) {
    MonsterBaseStats s;
    switch (k) {
        case EntityKind::Goblin: s.hpMax = 7; s.baseAtk = 1; s.baseDef = 0; s.willFlee = true; break;
        case EntityKind::Orc: s.hpMax = 10; s.baseAtk = 2; s.baseDef = 1; s.willFlee = false; break;
        case EntityKind::Bat: s.hpMax = 5; s.baseAtk = 1; s.baseDef = 0; s.willFlee = true; break;
        case EntityKind::Slime: s.hpMax = 12; s.baseAtk = 2; s.baseDef = 1; s.willFlee = false; break;
        case EntityKind::SkeletonArcher:
            s.hpMax = 9; s.baseAtk = 2; s.baseDef = 1; s.willFlee = false;
            s.canRanged = true;
            s.rangedRange = 8;
            s.rangedAtk = 6;
            s.rangedProjectile = ProjectileKind::Arrow;
            s.rangedAmmo = AmmoKind::Arrow;
            break;
        case EntityKind::KoboldSlinger:
            s.hpMax = 8; s.baseAtk = 2; s.baseDef = 0; s.willFlee = true;
            s.canRanged = true;
            s.rangedRange = 6;
            s.rangedAtk = 5;
            s.rangedProjectile = ProjectileKind::Rock;
            s.rangedAmmo = AmmoKind::Rock;
            break;
        case EntityKind::Wolf:
            s.hpMax = 6; s.baseAtk = 2; s.baseDef = 0; s.willFlee = false;
            s.packAI = true;
            break;
        case EntityKind::Troll:
            s.hpMax = 16; s.baseAtk = 4; s.baseDef = 2; s.willFlee = false;
            s.regenChancePct = 25;
            s.regenAmount = 1;
            break;
        case EntityKind::Wizard:
            s.hpMax = 12; s.baseAtk = 3; s.baseDef = 1; s.willFlee = false;
            s.canRanged = true;
            s.rangedRange = 7;
            s.rangedAtk = 7;
            s.rangedProjectile = ProjectileKind::Spark;
            s.rangedAmmo = AmmoKind::None;
            break;
        case EntityKind::Snake: s.hpMax = 7; s.baseAtk = 2; s.baseDef = 0; s.willFlee = false; break;
        case EntityKind::Spider: s.hpMax = 8; s.baseAtk = 3; s.baseDef = 1; s.willFlee = false; break;
        case EntityKind::Ogre: s.hpMax = 18; s.baseAtk = 5; s.baseDef = 2; s.willFlee = false; break;
        case EntityKind::Mimic: s.hpMax = 14; s.baseAtk = 4; s.baseDef = 2; s.willFlee = false; break;
        case EntityKind::Shopkeeper: s.hpMax = 18; s.baseAtk = 6; s.baseDef = 4; s.willFlee = false; break;
        case EntityKind::Minotaur: s.hpMax = 38; s.baseAtk = 7; s.baseDef = 3; s.willFlee = false; break;
        case EntityKind::Dog: s.hpMax = 10; s.baseAtk = 2; s.baseDef = 0; s.willFlee = false; break;
        case EntityKind::Ghost:
            // Bones ghosts are meant to be scary, but their new "ethereal" movement
            // (phasing through walls/doors) is a big power bump. Keep their raw stats
            // slightly lower so they remain threatening without feeling unfair.
            s.hpMax = 18; s.baseAtk = 4; s.baseDef = 2; s.willFlee = false;
            s.regenChancePct = 15;
            s.regenAmount = 1;
            break;
        case EntityKind::Leprechaun:
            // Fast, fragile thief: relies on stealing and blinking away rather than brawling.
            s.hpMax = 8; s.baseAtk = 2; s.baseDef = 1; s.willFlee = true;
            break;
        case EntityKind::Zombie:
            // Slow, tough undead: does not flee. Often created when corpses rise.
            s.hpMax = 14; s.baseAtk = 3; s.baseDef = 2; s.willFlee = false;
            break;
        default:
            // Player and unknown kinds fall back to a tame baseline.
            s.hpMax = 6; s.baseAtk = 1; s.baseDef = 0; s.willFlee = true;
            break;
    }

    // Apply optional overrides (used by procrogue_content.ini).
    auto it = g_overrides.monsters.find(k);
    if (it != g_overrides.monsters.end()) {
        const MonsterStatsOverride& o = it->second;
        if (o.hpMax) s.hpMax = *o.hpMax;
        if (o.baseAtk) s.baseAtk = *o.baseAtk;
        if (o.baseDef) s.baseDef = *o.baseDef;

        if (o.willFlee) s.willFlee = *o.willFlee;
        if (o.packAI) s.packAI = *o.packAI;

        if (o.canRanged) s.canRanged = *o.canRanged;
        if (o.rangedRange) s.rangedRange = *o.rangedRange;
        if (o.rangedAtk) s.rangedAtk = *o.rangedAtk;
        if (o.rangedProjectile) s.rangedProjectile = *o.rangedProjectile;
        if (o.rangedAmmo) s.rangedAmmo = *o.rangedAmmo;

        if (o.regenChancePct) s.regenChancePct = *o.regenChancePct;
        if (o.regenAmount) s.regenAmount = *o.regenAmount;
    }

    // Sanity clamps.
    s.hpMax = std::max(1, s.hpMax);
    s.baseAtk = std::max(0, s.baseAtk);
    s.baseDef = std::max(0, s.baseDef);
    s.rangedRange = std::max(0, s.rangedRange);
    s.rangedAtk = std::max(0, s.rangedAtk);
    s.regenChancePct = std::max(0, std::min(100, s.regenChancePct));
    s.regenAmount = std::max(0, s.regenAmount);

    if (!s.canRanged) {
        s.rangedRange = 0;
        s.rangedAtk = 0;
        s.rangedAmmo = AmmoKind::None;
    }

    return s;
}
