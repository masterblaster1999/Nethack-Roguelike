#pragma once

// NOTE: This header is included by multiple translation units and defines
// many internal helper functions in an unnamed namespace. Some translation units
// won't reference every helper, so we suppress -Wunused-function in this file
// to keep builds clean without changing global warning settings.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505) // unreferenced function with internal linkage has been removed
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif


// Internal helper utilities and constants shared by the split Game translation units.
// This header intentionally contains implementation-only helpers in an unnamed namespace.
// It is included by the various src/game_*.cpp files.

#include "game.hpp"
#include "action_info.hpp"
#include "settings.hpp"
#include "grid_utils.hpp"
#include "pathfinding.hpp"
#include "slot_utils.hpp"
#include "shop.hpp"
#include "version.hpp"
#include "vtuber_gen.hpp"
#include "pet_gen.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <deque>
#include <sstream>
#include <fstream>
#include <cstring>
#include <optional>
#include <filesystem>
#include <iomanip>
#include <ctime>


namespace {

static std::string ltrim(std::string s) {
    s.erase(s.begin(),
        std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    return s;
}

static std::string rtrim(std::string s) {
    s.erase(
        std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
        s.end());
    return s;
}

static std::string trim(std::string s) {
    return rtrim(ltrim(std::move(s)));
}

static bool parseInt(const std::string& s, int& out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') return false;
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// --- Engraving helpers: sigils (rare magical graffiti) ---
// A sigil is an engraving whose text begins with 'SIGIL' (case-insensitive).
// We treat it as a keyworded, limited-use floor effect that triggers when stepped on.
//
// Examples:
//   'SIGIL: NEXUS'
//   'SIGIL OF EMBER'
//
// Returns the keyword (e.g., 'NEXUS') or empty string if not a sigil.
static std::string sigilKeywordFromText(std::string text) {
    text = trim(toUpper(std::move(text)));
    if (text.size() < 5) return std::string();
    if (text.rfind("SIGIL", 0) != 0) return std::string();

    size_t i = 5;
    // Skip punctuation / whitespace after 'SIGIL'.
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ':' || c == '-' || std::isspace(c)) {
            ++i;
            continue;
        }
        break;
    }

    // Optional 'OF' (as in 'SIGIL OF EMBER').
    if (i + 1 < text.size() && text[i] == 'O' && text[i + 1] == 'F') {
        size_t j = i + 2;
        // Only treat it as 'OF' if followed by delimiter.
        if (j == text.size() || std::isspace(static_cast<unsigned char>(text[j])) || text[j] == ':' || text[j] == '-') {
            i = j;
            while (i < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (c == ':' || c == '-' || std::isspace(c)) {
                    ++i;
                    continue;
                }
                break;
            }
        }
    }

    const size_t start = i;
    while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i <= start) return std::string();
    return text.substr(start, i - start);
}

static bool engravingIsSigil(const Engraving& eg, std::string* keywordOut = nullptr) {
    std::string k = sigilKeywordFromText(eg.text);
    if (k.empty()) return false;
    if (keywordOut) *keywordOut = std::move(k);
    return true;
}

struct ThrowAmmoSpec {
    AmmoKind ammo = AmmoKind::None;
    ProjectileKind proj = ProjectileKind::Rock;
    ItemKind item = ItemKind::Rock;
};

static bool choosePlayerThrowAmmo(const std::vector<Item>& inv, ThrowAmmoSpec& out) {
    // Prefer rocks (a common "throwable") when available; otherwise fall back to arrows.
    const int rocks = ammoCount(inv, AmmoKind::Rock);
    if (rocks > 0) {
        out.ammo = AmmoKind::Rock;
        out.proj = ProjectileKind::Rock;
        out.item = ItemKind::Rock;
        return true;
    }

    const int arrows = ammoCount(inv, AmmoKind::Arrow);
    if (arrows > 0) {
        out.ammo = AmmoKind::Arrow;
        out.proj = ProjectileKind::Arrow;
        out.item = ItemKind::Arrow;
        return true;
    }

    return false;
}

static int throwRangeFor(const Entity& p, AmmoKind ammo) {
    // A small, simple "throw by hand" range.
    // Arrows fly a bit farther than rocks; stronger characters get a small bonus.
    int base = (ammo == AmmoKind::Arrow) ? 5 : 4;
    int bonus = std::max(0, (p.baseAtk - 3) / 2);
    int range = base + bonus;
    range = clampi(range, 2, 12);
    return range;
}



static int stackUnitsForPrice(const Item& it) {
    return isStackable(it.kind) ? std::max(0, it.count) : 1;
}

static int totalShopPrice(const Item& it) {
    if (it.shopPrice <= 0) return 0;
    return it.shopPrice * stackUnitsForPrice(it);
}

static bool spendGoldFromInv(std::vector<Item>& inv, int amount) {
    if (amount <= 0) return true;
    const int have = countGold(inv);
    if (have < amount) return false;

    int need = amount;
    for (auto& it : inv) {
        if (it.kind != ItemKind::Gold) continue;
        const int take = std::min(it.count, need);
        it.count -= take;
        need -= take;
        if (need <= 0) break;
    }

    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& it) {
        return it.kind == ItemKind::Gold && it.count <= 0;
    }), inv.end());

    return true;
}

static void gainGoldToInv(std::vector<Item>& inv, int amount, int& nextItemId, RNG& rng) {
    if (amount <= 0) return;

    Item g;
    g.id = nextItemId++;
    g.kind = ItemKind::Gold;
    g.count = amount;
    g.charges = 0;
    g.enchant = 0;
    g.buc = 0;
    g.spriteSeed = rng.nextU32();
    g.shopPrice = 0;
    g.shopDepth = 0;

    if (!tryStackItem(inv, g)) {
        inv.push_back(g);
    }
}

static bool anyLivingShopkeeper(const std::vector<Entity>& ents, int playerId) {
    for (const auto& e : ents) {
        if (e.id == playerId) continue;
        if (e.hp <= 0) continue;
        if (e.kind == EntityKind::Shopkeeper) return true;
    }
    return false;
}

static bool anyPeacefulShopkeeper(const std::vector<Entity>& ents, int playerId) {
    for (const auto& e : ents) {
        if (e.id == playerId) continue;
        if (e.hp <= 0) continue;
        if (e.kind == EntityKind::Shopkeeper && !e.alerted) return true;
    }
    return false;
}

static void setShopkeepersAlerted(std::vector<Entity>& ents, int playerId, Vec2i playerPos, bool alerted) {
    for (auto& e : ents) {
        if (e.id == playerId) continue;
        if (e.hp <= 0) continue;
        if (e.kind != EntityKind::Shopkeeper) continue;
        e.alerted = alerted;
        if (alerted) {
            e.lastKnownPlayerPos = playerPos;
            e.lastKnownPlayerAge = 0;
        }
    }
}

static std::string formatSearchDiscoveryMessage(int foundTraps, int foundSecrets) {
    std::ostringstream ss;
    ss << "YOU DISCOVER ";
    bool first = true;
    if (foundTraps > 0) {
        ss << foundTraps << " TRAP" << (foundTraps == 1 ? "" : "S");
        first = false;
    }
    if (foundSecrets > 0) {
        if (!first) ss << " AND ";
        ss << foundSecrets << " SECRET DOOR" << (foundSecrets == 1 ? "" : "S");
    }
    ss << "!";
    return ss.str();
}

static void moveFileWithFallback(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) return;

    // Fallback (e.g., Windows rename over existing / cross-device): copy then remove.
    std::error_code ec2;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec2);
    if (ec2) return;
    std::filesystem::remove(from, ec2);
}

static void rotateFileBackups(const std::filesystem::path& path, int keepBackups) {
    if (keepBackups <= 0) return;

    // Example: procrogue_save.dat -> procrogue_save.dat.bak1, bak2, ...
    // We keep this intentionally simple and best-effort; failures should not prevent saving.
    std::error_code ec;

    // Remove the oldest
    const std::filesystem::path oldest = path.string() + ".bak" + std::to_string(keepBackups);
    std::filesystem::remove(oldest, ec);

    // Shift N-1 -> N
    for (int i = keepBackups - 1; i >= 1; --i) {
        const std::filesystem::path src = path.string() + ".bak" + std::to_string(i);
        const std::filesystem::path dst = path.string() + ".bak" + std::to_string(i + 1);
        if (!std::filesystem::exists(src)) continue;
        moveFileWithFallback(src, dst);
    }

    // Current -> bak1
    if (std::filesystem::exists(path)) {
        const std::filesystem::path dst = path.string() + ".bak1";
        moveFileWithFallback(path, dst);
    }
}


static std::string timestampForFilename() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}


// sanitizeSlotName() lives in slot_utils.hpp so main/settings/game share identical behavior.

static std::filesystem::path makeSlotPath(const std::string& basePathStr, const std::string& slot) {
    std::filesystem::path p(basePathStr);
    std::filesystem::path dir = p.parent_path();
    if (dir.empty()) dir = std::filesystem::path(".");
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    return dir / (stem + "_" + slot + ext);
}

static std::filesystem::path baseSavePathForSlots(const Game& game) {
    std::filesystem::path p(game.defaultSavePath());
    std::filesystem::path dir = p.parent_path();
    if (dir.empty()) dir = std::filesystem::path(".");
    return dir / "procrogue_save.dat";
}

static std::filesystem::path baseAutosavePathForSlots(const Game& game) {
    std::filesystem::path p(game.defaultAutosavePath());
    std::filesystem::path dir = p.parent_path();
    if (dir.empty()) dir = std::filesystem::path(".");
    return dir / "procrogue_autosave.dat";
}

static std::filesystem::path exportBaseDir(const Game& game) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(game.defaultSavePath()).parent_path();
    if (dir.empty()) dir = fs::path(".");
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static bool exportRunLogToFile(const Game& game, const std::filesystem::path& outPath) {
    std::ofstream f(outPath);
    if (!f) return false;

    f << PROCROGUE_APPNAME << " " << PROCROGUE_VERSION << "\n";
    f << "Name: " << game.playerName() << "\n";
    f << "Class: " << game.playerClassDisplayName() << " (" << game.playerClassIdString() << ")\n";
    f << "Slot: " << (game.activeSlot().empty() ? std::string("default") : game.activeSlot()) << "\n";
    f << "Seed: " << game.seed() << "\n";

    const char* branchName = (game.branch() == DungeonBranch::Camp) ? "Camp" : "Main";
    f << "Branch: " << branchName << "\n";
    if (game.branch() == DungeonBranch::Main) {
        f << "Depth: " << game.depth() << " (max " << game.maxDepthReached() << ")\n";
    } else {
        // Camp is a distinct hub branch; avoid implying it's "D0".
        f << "Depth: CAMP" << " (deepest main " << game.maxDepthReached() << ")\n";
    }
    f << "Turns: " << game.turns() << "\n";
    f << "Kills: " << game.kills() << "\n";
    f << "Gold: " << game.goldCount() << "\n";
    f << "Level: " << game.playerCharLevel() << "\n";
    if (game.hungerEnabled()) {
        f << "Hunger: " << game.hungerCurrent() << "/" << game.hungerMaximum();
        const std::string tag = game.hungerTag();
        if (!tag.empty()) f << " (" << tag << ")";
        f << "\n";
    }

    if (game.isFinished()) {
        f << "Result: " << (game.isGameWon() ? "WIN" : "DEAD") << "\n";
        if (!game.endCause().empty()) f << "Cause: " << game.endCause() << "\n";
    }

    f << "\nMessages:\n";
    for (const auto& m : game.messages()) {
        const char* k = (m.kind == MessageKind::Info)    ? "INFO"
                      : (m.kind == MessageKind::Combat)  ? "COMBAT"
                      : (m.kind == MessageKind::Loot)    ? "LOOT"
                      : (m.kind == MessageKind::System)  ? "SYSTEM"
                      : (m.kind == MessageKind::Warning) ? "WARN"
                      : (m.kind == MessageKind::Success) ? "SUCCESS"
                                                         : "INFO";
        std::string depthTag;
        if (m.branch == DungeonBranch::Camp) depthTag = "CAMP";
        else depthTag = "D" + std::to_string(m.depth);

        f << "[" << k << "] [" << depthTag << " T" << m.turn << "] " << m.text;
        if (m.repeat > 1) f << " (x" << m.repeat << ")";
        f << "\n";
    }
    return true;
}

static bool exportRunMapToFile(const Game& game, const std::filesystem::path& outPath) {
    std::ofstream f(outPath);
    if (!f) return false;

    const Dungeon& d = game.dungeon();

    f << PROCROGUE_APPNAME << " map export (" << PROCROGUE_VERSION << ")\n";
    const char* branchName = (game.branch() == DungeonBranch::Camp) ? "Camp" : "Main";
    f << "Seed: " << game.seed()
      << "  Branch: " << branchName
      << "  Depth: " << ((game.branch() == DungeonBranch::Camp) ? "CAMP" : std::to_string(game.depth()))
      << "  Turns: " << game.turns() << "\n";
    f << "Legend: # wall, . floor, + door, / open door, * locked door, < up, > down, ~ chasm, I pillar, B boulder, ^ trap, @ you\n";
    f << "        $ gold, ! potion, ? scroll, : food, K key, l lockpick, C chest\n";
    f << "        = note mark, X danger mark, % loot mark\n";
    f << "        g goblin, o orc, b bat, j slime, S skeleton, k kobold, w wolf, T troll, W wizard, n snake, s spider, O ogre\n\n";

    std::vector<std::string> grid(static_cast<size_t>(d.height), std::string(static_cast<size_t>(d.width), ' '));

    // Base tiles (explored only).
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const Tile& t = d.at(x, y);
            if (!t.explored) {
                grid[static_cast<size_t>(y)][static_cast<size_t>(x)] = ' ';
                continue;
            }

            char c = ' ';
            switch (t.type) {
                case TileType::Wall:       c = '#'; break;
                case TileType::Floor:      c = '.'; break;
                case TileType::DoorClosed: c = '+'; break;
                case TileType::DoorOpen:   c = '/'; break;
                case TileType::StairsUp:   c = '<'; break;
                case TileType::StairsDown: c = '>'; break;
                case TileType::DoorSecret: c = '#'; break;
                case TileType::DoorLocked: c = '*'; break;
                case TileType::Chasm:      c = '~'; break;
                case TileType::Pillar:     c = 'I'; break;
                case TileType::Boulder:    c = 'B'; break;
                default:                   c = '?'; break;
            }

            grid[static_cast<size_t>(y)][static_cast<size_t>(x)] = c;
        }
    }

    // Player markers (explored tiles only). Draw before traps/items/monsters so they can override.
    for (const auto& m : game.mapMarkers()) {
        if (!d.inBounds(m.pos.x, m.pos.y)) continue;
        const Tile& t = d.at(m.pos.x, m.pos.y);
        if (!t.explored) continue;

        char c = '=';
        switch (m.kind) {
            case MarkerKind::Danger: c = 'X'; break;
            case MarkerKind::Loot:   c = '%'; break;
            case MarkerKind::Note:
            default:                 c = '='; break;
        }

        grid[static_cast<size_t>(m.pos.y)][static_cast<size_t>(m.pos.x)] = c;
    }

    // Traps (discovered, on explored tiles).
    for (const auto& tr : game.traps()) {
        if (!tr.discovered) continue;
        if (!d.inBounds(tr.pos.x, tr.pos.y)) continue;
        const Tile& t = d.at(tr.pos.x, tr.pos.y);
        if (!t.explored) continue;
        grid[static_cast<size_t>(tr.pos.y)][static_cast<size_t>(tr.pos.x)] = '^';
    }

    // Items (visible only).
    for (const auto& gi : game.groundItems()) {
        if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
        const Tile& t = d.at(gi.pos.x, gi.pos.y);
        if (!t.visible) continue;

        char c = '*';
        if (gi.item.kind == ItemKind::Gold) c = '$';
        else if (isPotionKind(gi.item.kind)) c = '!';
        else if (isScrollKind(gi.item.kind)) c = '?';
        else if (gi.item.kind == ItemKind::FoodRation) c = ':';
        else if (gi.item.kind == ItemKind::Key) c = 'K';
        else if (gi.item.kind == ItemKind::Lockpick) c = 'l';
        else if (isChestKind(gi.item.kind)) c = 'C';

        grid[static_cast<size_t>(gi.pos.y)][static_cast<size_t>(gi.pos.x)] = c;
    }

    // Monsters (visible only).
    auto monsterGlyph = [](EntityKind k) -> char {
        switch (k) {
            case EntityKind::Goblin: return 'g';
            case EntityKind::Orc:    return 'o';
            case EntityKind::Bat:    return 'b';
            case EntityKind::Slime:  return 'j';
            case EntityKind::SkeletonArcher: return 'S';
            case EntityKind::KoboldSlinger:  return 'k';
            case EntityKind::Wolf:   return 'w';
            case EntityKind::Dog:    return 'd';
            case EntityKind::Ghost:  return 'G';
            case EntityKind::Leprechaun: return 'l';
            case EntityKind::Nymph: return 'N';
            case EntityKind::Zombie: return 'Z';
            case EntityKind::Troll:  return 'T';
            case EntityKind::Wizard: return 'W';
            case EntityKind::Snake:  return 'n';
            case EntityKind::Spider: return 's';
            case EntityKind::Ogre:   return 'O';
            case EntityKind::Mimic:  return 'm';
            case EntityKind::Shopkeeper: return 'K';
            case EntityKind::Minotaur: return 'M';
            default:                 return 'M';
        }
    };

    for (const auto& e : game.entities()) {
        if (e.kind == EntityKind::Player) continue;
        if (e.hp <= 0) continue;
        if (!d.inBounds(e.pos.x, e.pos.y)) continue;
        const Tile& t = d.at(e.pos.x, e.pos.y);
        if (!t.visible) continue;
        grid[static_cast<size_t>(e.pos.y)][static_cast<size_t>(e.pos.x)] = monsterGlyph(e.kind);
    }

    // Player
    const Entity& p = game.player();
    if (d.inBounds(p.pos.x, p.pos.y)) {
        grid[static_cast<size_t>(p.pos.y)][static_cast<size_t>(p.pos.x)] = '@';
    }

    for (int y = 0; y < d.height; ++y) {
        f << grid[static_cast<size_t>(y)] << "\n";
    }

    return true;
}


// Returns {ok, mapIncluded}.
static std::pair<bool, bool> exportRunDumpToFile(const Game& game, const std::filesystem::path& outPath) {
    namespace fs = std::filesystem;

    std::ofstream f(outPath);
    if (!f) return {false, false};

    const Entity& p = game.player();

    f << PROCROGUE_APPNAME << " dump (" << PROCROGUE_VERSION << ")\n";
    f << "Name: " << game.playerName() << "\n";
    f << "Class: " << game.playerClassDisplayName() << " (" << game.playerClassIdString() << ")\n";
    f << "Slot: " << (game.activeSlot().empty() ? std::string("default") : game.activeSlot()) << "\n";
    f << "Seed: " << game.seed() << "\n";

    const char* branchName = (game.branch() == DungeonBranch::Camp) ? "Camp" : "Main";
    f << "Branch: " << branchName << "\n";
    if (game.branch() == DungeonBranch::Main) {
        f << "Depth: " << game.depth() << " (max " << game.maxDepthReached() << ")\n";
    } else {
        // Camp is a distinct hub branch; avoid implying it's "D0".
        f << "Depth: CAMP" << " (deepest main " << game.maxDepthReached() << ")\n";
    }
    f << "Turns: " << game.turns() << "\n";
    f << "Kills: " << game.kills() << "\n";
    f << "Gold: " << game.goldCount() << "\n";
    f << "Level: " << game.playerCharLevel() << "  XP: " << game.playerXp() << "/" << game.playerXpToNext() << "\n";

    if (game.isFinished()) {
        f << "Result: " << (game.isGameWon() ? "WIN" : "DEAD") << "\n";
        if (!game.endCause().empty()) f << "Cause: " << game.endCause() << "\n";
    }

    f << "HP: " << p.hp << "/" << p.hpMax << "  ATK: " << game.playerAttack() << "  DEF: " << game.playerDefense() << "\n";

    if (game.hungerEnabled()) {
        f << "Hunger: " << game.hungerCurrent() << "/" << game.hungerMaximum();
        const std::string tag = game.hungerTag();
        if (!tag.empty()) f << " (" << tag << ")";
        f << "\n";
    }

    // Status effects
    f << "Status: ";
    bool any = false;
    auto add = [&](const char* name, int turns) {
        if (turns <= 0) return;
        if (any) f << ", ";
        f << name << "(" << turns << ")";
        any = true;
    };
    add("POISON", p.effects.poisonTurns);
    add("REGEN", p.effects.regenTurns);
    add("SHIELD", p.effects.shieldTurns);
    add("HASTE", p.effects.hasteTurns);
    add("VISION", p.effects.visionTurns);
    add("INVIS", p.effects.invisTurns);
    add("WEB", p.effects.webTurns);
    add("CONF", p.effects.confusionTurns);
    add("BURN", p.effects.burnTurns);
    add("LEV", p.effects.levitationTurns);
    add("FEAR", p.effects.fearTurns);
    add("HALL", p.effects.hallucinationTurns);
    if (!any) f << "(none)";
    f << "\n";

    // Equipment
    f << "\nEquipment:\n";
    f << "  Melee:  " << game.equippedMeleeName() << "\n";
    f << "  Ranged: " << game.equippedRangedName() << "\n";
    f << "  Armor:  " << game.equippedArmorName() << "\n";

    // Inventory
    f << "\nInventory:\n";
    if (game.inventory().empty()) {
        f << "  (empty)\n";
    } else {
        for (const auto& it : game.inventory()) {
            f << "  - " << game.displayItemName(it);
            const std::string tag = game.equippedTag(it.id);
            if (!tag.empty()) f << " {" << tag << "}";
            f << "\n";
        }
    }

    // Messages (tail)
    f << "\nMessages (most recent last):\n";
    const auto& ms = game.messages();
    const size_t start = (ms.size() > 120) ? (ms.size() - 120) : 0;
    for (size_t i = start; i < ms.size(); ++i) {
        std::string depthTag;
        if (ms[i].branch == DungeonBranch::Camp) depthTag = "CAMP";
        else depthTag = "D" + std::to_string(ms[i].depth);

        f << "  [" << depthTag << " T" << ms[i].turn << "] " << ms[i].text;
        if (ms[i].repeat > 1) f << " (x" << ms[i].repeat << ")";
        f << "\n";
    }

    // Map at end (same format as exportmap)
    f << "\n--- MAP ---\n\n";
    f.flush();

    bool mapOk = false;
    try {
        fs::path tmp = outPath;
        tmp += ".map.tmp";

        mapOk = exportRunMapToFile(game, tmp);
        if (mapOk) {
            std::ifstream in(tmp);
            if (in) {
                std::string line;
                bool pastHeader = false;
                while (std::getline(in, line)) {
                    if (!pastHeader) {
                        if (line.empty()) pastHeader = true;
                        continue;
                    }
                    f << line << "\n";
                }
                mapOk = true;
            } else {
                mapOk = false;
            }
        }

        std::error_code ec;
        fs::remove(tmp, ec);
    } catch (...) {
        mapOk = false;
    }

    return {true, mapOk};
}


} // namespace

namespace {

// Forward declarations for helpers referenced before their definitions later in this header.
const char* kindName(EntityKind k);

static std::vector<std::string> splitWS(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

// ------------------------------------------------------------
// Procedural pets: deterministic names + compact trait bitmask.
// ------------------------------------------------------------

static uint32_t petProfileSeedFor(const Entity& e) {
    // Prefer the persisted sprite seed; it is stable across save/load.
    uint32_t s = e.spriteSeed;
    if (s == 0u) {
        // Defensive fallback for malformed/legacy entities.
        const uint32_t a = static_cast<uint32_t>(e.id) ^ 0xBADC0DEu;
        const uint32_t b = static_cast<uint32_t>(static_cast<uint8_t>(e.kind)) ^ 0xC0FFEEu;
        s = hashCombine(a, b);
        if (s == 0u) s = 1u;
    }
    return s;
}

static std::string petGivenNameFor(const Entity& e) {
    return petgen::petGivenName(petProfileSeedFor(e));
}

static void ensurePetTraits(Entity& e) {
    if (e.kind == EntityKind::Player) return;
    if (!e.friendly) return;
    if (e.hp <= 0) return;

    const uint8_t cur = petgen::petTraitMask(e.procAffixMask);
    if (cur != 0) return; // already initialized (and bonuses applied)

    const uint32_t seed = petProfileSeedFor(e);
    const uint8_t traits = petgen::petRollTraitMask(seed);
    petgen::setPetTraitMask(e.procAffixMask, traits);

    // Apply one-time, conservative stat bonuses.
    if (e.speed <= 0) e.speed = baseSpeedFor(e.kind);

    if ((traits & petgen::petTraitBit(petgen::PetTrait::Sprinter)) != 0) {
        e.speed = std::min(220, e.speed + 12);
    }
    if ((traits & petgen::petTraitBit(petgen::PetTrait::Stout)) != 0) {
        e.hpMax = std::max(1, e.hpMax + 3);
        e.baseDef = std::max(0, e.baseDef + 1);
        e.hp = std::min(e.hpMax, e.hp + 3);
    }
    if ((traits & petgen::petTraitBit(petgen::PetTrait::Ferocious)) != 0) {
        e.baseAtk = std::max(0, e.baseAtk + 1);
    }

    e.hpMax = std::max(1, e.hpMax);
    e.hp = clampi(e.hp, 0, e.hpMax);
}

// Hunger helper: 0 = OK, 1 = hungry, 2 = starving, 3 = starving (damage)
static int hungerStateFor(int hunger, int hungerMax) {
    if (hungerMax <= 0) return 0;
    if (hunger <= 0) return 3;
    if (hunger < (hungerMax / 10)) return 2;
    if (hunger < (hungerMax / 4)) return 1;
    return 0;
}

// Chest flags are stored in Item::charges (low bits) to avoid changing save format.
// - bit 0: locked
// - bit 1: trapped
// - bit 2: opened
// - bit 3: trap discovered (for "search" / detect traps UI)
// - bit 4: mimic (looks like a chest until you try to open it)
// Trap kind is stored in charges bits 8..15.
static constexpr int CHEST_FLAG_LOCKED = 1 << 0;
static constexpr int CHEST_FLAG_TRAPPED = 1 << 1;
static constexpr int CHEST_FLAG_OPENED = 1 << 2;
static constexpr int CHEST_FLAG_TRAP_KNOWN = 1 << 3;
static constexpr int CHEST_FLAG_MIMIC = 1 << 4;
static constexpr int CHEST_TRAP_SHIFT = 8;

static bool chestLocked(const Item& it) {
    return (it.charges & CHEST_FLAG_LOCKED) != 0;
}

static bool chestTrapped(const Item& it) {
    return (it.charges & CHEST_FLAG_TRAPPED) != 0;
}

static bool chestTrapKnown(const Item& it) {
    return (it.charges & CHEST_FLAG_TRAP_KNOWN) != 0;
}

static bool chestMimic(const Item& it) {
    return (it.charges & CHEST_FLAG_MIMIC) != 0;
}

static TrapKind chestTrapKind(const Item& it) {
    const int v = (it.charges >> CHEST_TRAP_SHIFT) & 0xFF;
    return static_cast<TrapKind>(v);
}

static int chestTier(const Item& it) {
    // Stored in enchant (0..4). Not shown to the player.
    // Some generators create higher-tier "cache" chests deeper in the dungeon.
    return clampi(it.enchant, 0, 4);
}

static const char* chestTierName(int tier) {
    switch (clampi(tier, 0, 4)) {
        case 0: return "COMMON";
        case 1: return "STURDY";
        case 2: return "ORNATE";
        case 3: return "LARGE";
        case 4: return "ANCIENT";
        default: return "CHEST";
    }
}

// Stack-based capacity limit for container storage.
// The game uses stacks (not item weight/volume) to keep the UI fast and deterministic.
static int chestStackLimitForTier(int tier) {
    tier = clampi(tier, 0, 4);
    return 16 + 4 * tier; // 16..32 stacks
}

static void setChestLocked(Item& it, bool v) {
    if (v) it.charges |= CHEST_FLAG_LOCKED;
    else it.charges &= ~CHEST_FLAG_LOCKED;
}

static void setChestTrapped(Item& it, bool v) {
    if (v) it.charges |= CHEST_FLAG_TRAPPED;
    else it.charges &= ~CHEST_FLAG_TRAPPED;
}

static void setChestTrapKnown(Item& it, bool v) {
    if (v) it.charges |= CHEST_FLAG_TRAP_KNOWN;
    else it.charges &= ~CHEST_FLAG_TRAP_KNOWN;
}

static void setChestMimic(Item& it, bool v) {
    if (v) it.charges |= CHEST_FLAG_MIMIC;
    else it.charges &= ~CHEST_FLAG_MIMIC;
}

static void setChestTrapKind(Item& it, TrapKind k) {
    it.charges &= ~(0xFF << CHEST_TRAP_SHIFT);
    it.charges |= (static_cast<int>(k) & 0xFF) << CHEST_TRAP_SHIFT;
}

// --- Curses / blessings (BUC) helpers ---
static RoomType roomTypeAt(const Dungeon& d, Vec2i p) {
    for (const auto& r : d.rooms) {
        if (r.contains(p.x, p.y)) return r.type;
    }
    return RoomType::Normal;
}

static int rollBucForGear(RNG& rng, int depth, RoomType roomType) {
    // Baseline: mostly uncursed; deeper floors skew slightly toward cursed.
    int cursePct = 8 + std::min(12, std::max(0, depth - 1) * 2);
    int blessPct = 4 + std::min(6, std::max(0, depth - 1));

    switch (roomType) {
        case RoomType::Treasure:
        case RoomType::Vault:
            cursePct -= 3;
            blessPct += 4;
            break;
        case RoomType::Shrine:
            cursePct -= 2;
            blessPct += 3;
            break;
        case RoomType::Lair:
            cursePct += 3;
            blessPct -= 1;
            break;
        case RoomType::Secret:
            cursePct += 4;
            break;
        case RoomType::Shop:
            // Merchants don't love selling cursed junk.
            cursePct -= 5;
            blessPct += 2;
            break;

        case RoomType::Armory:
            // Armories skew toward "usable" gear (but aren't as pristine as shops).
            cursePct -= 3;
            blessPct += 1;
            break;
        case RoomType::Library:
            // Libraries are safer/cleaner spaces on average.
            cursePct -= 1;
            blessPct += 1;
            break;
        case RoomType::Laboratory:
            // Experiments go wrong.
            cursePct += 3;
            break;
        default:
            break;
    }

    cursePct = clampi(cursePct, 0, 80);
    blessPct = clampi(blessPct, 0, 60);

    const int roll = rng.range(1, 100);
    if (roll <= cursePct) return -1;
    if (roll <= cursePct + blessPct) return 1;
    return 0;
}

static std::vector<std::string> extendedCommandList() {
    // Keep these short and stable: they're user-facing and used for completion/prefix matching.
    return {
        "help",
        "shout",
        "yell",
        "whistle",
        "listen",
        "wind",
        "throwvoice",
        "pet",
        "tame",
        "options",
        "preset",
        "sprites3d",
        "isoraytrace",
        "isoterrainvox",
        "isocutaway",
        "binds",
        "bind",
        "unbind",
        "reload",
        "record",
        "rec",
        "stoprecord",
        "stoprec",
        "save",
        "load",
        "loadauto",
        "saves",
        "slot",
        "paths",
        "quit",
        "restart",
        "daily",
        "autopickup",
        "autosave",
        "stepdelay",
        "identify",
        "call",
        "encumbrance",
        "timers",
        "uitheme",
        "palette",
        "pal",
        "vtubers",
        "vt",
        "uipanels",
        "seed",
        "pos",
        "what",
        "mapstats",
        "perf",
        "version",
        "name",
        "class",
        "scores",
        "history",
        "messages",
        "exportlog",
        "exportmap",
        "export",
        "exportall",
        "dump",
        "mortem",
        "bones",
        "explore",
        "mark",
        "unmark",
        "marks",
        "travel",
        "engrave",
        "inscribe",
        "search",
        "rest",
        "sneak",
        "dig",
        "craft",
        "recipes",
        "fish",
        "bounty",
        "bounties",
        "throwtorch",
        "augury",
        "pray",
        "donate",
        "sacrifice",
        "pay",
        "debt",
        "threat",
        "evade",
    };
}


struct ExtendedCommandUiMeta {
    const char* cmd;
    Action action;     // keybind Action for UI hints (Action::None if none)
    const char* desc;  // short UI description; optional
};

static const ExtendedCommandUiMeta* extendedCommandUiMetaFor(const std::string& cmd) {
    // Keep this list small and focused: it powers UI hints (TAB completion dropdown)
    // and avoids scattering "cmd -> action token" knowledge across files.
    static const ExtendedCommandUiMeta kMeta[] = {
        {"options", Action::Options, "Open options menu"},
        {"help",    Action::Help,   "List extended commands"},

        {"save",     Action::Save,      "Save game"},
        {"load",     Action::Load,      "Load save"},
        {"loadauto", Action::LoadAuto, "Load autosave"},
        {"record",   Action::None,     "Start replay recording (.prr file)"},
        {"stoprecord", Action::None,   "Stop replay recording"},
        {"restart",  Action::Restart,   "Restart run"},
        {"scores",   Action::Scores,    "Show high scores"},

        {"messages", Action::MessageHistory, "Open message history"},

        {"search", Action::Search, "Search nearby tiles"},
        {"rest",   Action::Rest,   "Rest until healed / interrupted"},
        {"dig",    Action::Dig,    "Dig (requires pickaxe)"},

        {"craft",  Action::None,  "Craft (requires Crafting Kit)"},

        {"recipes", Action::None, "Show learned crafting recipes"},

        {"fish",    Action::None, "Fish (requires Fishing Rod)"},
        {"sneak",  Action::ToggleSneak,  "Toggle sneak (stealth)"},

        {"explore", Action::AutoExplore, "Auto-explore"},
        {"threat",  Action::ToggleThreatPreview, "Toggle threat preview"},
        {"evade",   Action::Evade, "Smart step away from visible threats"},

        {"perf", Action::TogglePerfOverlay, "Toggle performance overlay"},

        {"debt", Action::None, "Show shop debt ledger"},
        {"isocutaway", Action::None, "Toggle isometric cutaway mode"},
    };

    for (const auto& m : kMeta) {
        if (cmd == m.cmd) return &m;
    }
    return nullptr;
}

static const char* extendedCommandActionToken(const std::string& cmd) {
    if (const auto* m = extendedCommandUiMetaFor(cmd)) {
        if (m->action != Action::None) return actioninfo::token(m->action);
    }
    return nullptr;
}

static const char* extendedCommandShortDesc(const std::string& cmd) {
    if (const auto* m = extendedCommandUiMetaFor(cmd)) return m->desc;
    return nullptr;
}

static std::string normalizeExtendedCommandAlias(const std::string& in) {
    std::string cmd = toLower(in);

    struct Alias { const char* alias; const char* canonical; };
    static const Alias kAliases[] = {
        // NetHack-style shorthands.
        {"?", "help"},
        {"commands", "help"},

        // Common synonyms / muscle-memory.
        {"annotate", "mark"},
        {"note", "mark"},
        {"unannotate", "unmark"},
        {"clearmark", "unmark"},
        {"notes", "marks"},
        {"markers", "marks"},

        {"msghistory", "messages"},
        {"message_history", "messages"},
        {"msglog", "messages"},

        {"controls", "preset"},
        {"keyset", "preset"},

        {"hear", "listen"},

        {"vent", "throwvoice"},
        {"ventriloquism", "throwvoice"},
        {"voice", "throwvoice"},
        {"decoy", "throwvoice"},

        {"divine", "augury"},
        {"divination", "augury"},
        {"omen", "augury"},
        {"prophecy", "augury"},

        {"where", "pos"},
        {"location", "pos"},
        {"loc", "pos"},

        {"label", "call"},


        {"crafting", "craft"},
        {"make", "craft"},
        {"tinker", "craft"},
        {"combine", "craft"},
        {"alchemy", "craft"},


        {"recipe", "recipes"},
        {"recipes", "recipes"},
        {"craftlog", "recipes"},
        {"craft_log", "recipes"},
        {"craftbook", "recipes"},

        {"danger", "threat"},
        {"threatpreview", "threat"},
        {"threat_preview", "threat"},

        {"flee", "evade"},
        {"panic", "evade"},
        {"run_away", "evade"},
        {"escape", "evade"},

        {"tile", "what"},
        {"whatis", "what"},
        {"describe", "what"},

        // Hidden/legacy spellings for view modes.
        {"iso_raytrace", "isoraytrace"},
        {"iso_ray", "isoraytrace"},
        {"isovoxelray", "isoraytrace"},

        {"iso_cutaway", "isocutaway"},
        {"cutaway", "isocutaway"},

        {"isoblocks", "isoterrainvox"},
        {"iso_blocks", "isoterrainvox"},
        {"iso_terrain_voxels", "isoterrainvox"},

        // Quality-of-life shortcuts.
        {"goto", "travel"},
        {"go", "travel"},

        {"ledger", "debt"},

        // Perf overlay variants.
        {"perf_overlay", "perf"},
        {"perfui", "perf"},

        // Back-compat / discoverability.
        {"stealth", "sneak"},
    };

    for (const auto& a : kAliases) {
        if (cmd == a.alias) return std::string(a.canonical);
    }

    return cmd;
}


static bool applyControlPreset(Game& game, ControlPreset preset, bool verbose = true) {
    const std::string settingsPath = game.settingsPath();
    if (settingsPath.empty()) {
        if (verbose) game.pushSystemMessage("SETTINGS PATH UNKNOWN; CAN'T APPLY CONTROL PRESET.");
        return false;
    }

    // Persist selection + bind_* changes.
    bool ok = true;
    ok &= updateIniKey(settingsPath, "control_preset", controlPresetId(preset));

    if (preset == ControlPreset::Nethack) {
        // Movement (vi-keys)
        ok &= updateIniKey(settingsPath, "bind_up", "k, up, kp_8");
        ok &= updateIniKey(settingsPath, "bind_down", "j, down, kp_2");
        ok &= updateIniKey(settingsPath, "bind_left", "h, left, kp_4");
        ok &= updateIniKey(settingsPath, "bind_right", "l, right, kp_6");
        ok &= updateIniKey(settingsPath, "bind_up_left", "y, kp_7");
        ok &= updateIniKey(settingsPath, "bind_up_right", "u, kp_9");
        ok &= updateIniKey(settingsPath, "bind_down_left", "b, kp_1");
        ok &= updateIniKey(settingsPath, "bind_down_right", "n, kp_3");

        // Actions
        ok &= updateIniKey(settingsPath, "bind_search", "s");
        ok &= updateIniKey(settingsPath, "bind_disarm", "t");
        ok &= updateIniKey(settingsPath, "bind_close_door", "c");
        ok &= updateIniKey(settingsPath, "bind_lock_door", "shift+c");
        ok &= updateIniKey(settingsPath, "bind_kick", "ctrl+d");
        ok &= updateIniKey(settingsPath, "bind_dig", "d");
        // Look: ':' is usually shift+semicolon on most layouts.
        ok &= updateIniKey(settingsPath, "bind_look", "shift+semicolon, v");
        // Help: remove 'h' to avoid conflicting with vi movement.
        ok &= updateIniKey(settingsPath, "bind_help", "f1, shift+slash, cmd+?");
        // Sneak: avoid 'n' (movement down-right in vi keys).
        ok &= updateIniKey(settingsPath, "bind_sneak", "shift+n");
        ok &= updateIniKey(settingsPath, "bind_evade", "ctrl+e");
    } else {
        // Modern (WASD)
        ok &= updateIniKey(settingsPath, "bind_up", "w, up, kp_8");
        ok &= updateIniKey(settingsPath, "bind_down", "s, down, kp_2");
        ok &= updateIniKey(settingsPath, "bind_left", "a, left, kp_4");
        ok &= updateIniKey(settingsPath, "bind_right", "d, right, kp_6");
        ok &= updateIniKey(settingsPath, "bind_up_left", "q, kp_7");
        ok &= updateIniKey(settingsPath, "bind_up_right", "e, kp_9");
        ok &= updateIniKey(settingsPath, "bind_down_left", "z, kp_1");
        ok &= updateIniKey(settingsPath, "bind_down_right", "c, kp_3");

        // Actions
        ok &= updateIniKey(settingsPath, "bind_search", "shift+c");
        ok &= updateIniKey(settingsPath, "bind_disarm", "t");
        ok &= updateIniKey(settingsPath, "bind_close_door", "k");
        ok &= updateIniKey(settingsPath, "bind_lock_door", "shift+k");
        ok &= updateIniKey(settingsPath, "bind_kick", "b");
        ok &= updateIniKey(settingsPath, "bind_dig", "shift+d");
        ok &= updateIniKey(settingsPath, "bind_look", "l, v");
        ok &= updateIniKey(settingsPath, "bind_help", "f1, shift+slash, h, cmd+?");
        ok &= updateIniKey(settingsPath, "bind_sneak", "n");
        ok &= updateIniKey(settingsPath, "bind_evade", "ctrl+e");
    }

    // UI/meta: keep these consistent across presets.
    // Note: SHIFT+M is reserved for the overworld map, so message history defaults to F3 only.
    ok &= updateIniKey(settingsPath, "bind_message_history", "f3");
    ok &= updateIniKey(settingsPath, "bind_overworld_map", "shift+m");
    // Extended command prompt: allow classic # plus editor-style palettes.
    ok &= updateIniKey(settingsPath, "bind_command", "shift+3, ctrl+p, shift+ctrl+p, shift+cmd+p");
    // Options: add common desktop shortcuts (Ctrl/Cmd+,).
    ok &= updateIniKey(settingsPath, "bind_options", "f2, ctrl+comma, cmd+comma");

    // Acoustic preview helper (UI-only). Keep a consistent bind across presets.
    ok &= updateIniKey(settingsPath, "bind_sound_preview", "ctrl+n");
    ok &= updateIniKey(settingsPath, "bind_threat_preview", "ctrl+t");
    ok &= updateIniKey(settingsPath, "bind_hearing_preview", "ctrl+h");
    ok &= updateIniKey(settingsPath, "bind_scent_preview", "ctrl+s");
    game.setControlPreset(preset);

    if (ok) {
        game.requestKeyBindsReload();
        if (verbose) {
            game.pushSystemMessage(std::string("CONTROL PRESET APPLIED: ") + game.controlPresetDisplayName());
        }
    } else {
        if (verbose) game.pushSystemMessage("FAILED TO APPLY CONTROL PRESET.");
    }
    return ok;
}


static void runExtendedCommand(Game& game, const std::string& rawLine) {
    std::string line = trim(rawLine);
    if (line.empty()) return;

    // Allow users to paste NetHack-style inputs like "#quit" even though we open the prompt separately.
    if (!line.empty() && line[0] == '#') {
        line = trim(line.substr(1));
    }

    // Action palette: "@<action>" runs an Action by its token (same tokens used for keybinds).
    // This turns the extended command prompt into a searchable command palette without adding new UI.
    if (!line.empty() && line[0] == '@') {
        std::string rest = trim(line.substr(1));
        if (rest.empty()) {
            game.pushSystemMessage("ACTION PALETTE: @<action>  (TIP: press TAB after '@' to complete)");
            game.pushSystemMessage("EXAMPLES: @inventory | @toggle_minimap | @stairs_down | @look");
            return;
        }

        auto atoks = splitWS(rest);
        if (atoks.empty()) return;

        const std::string tokRaw = atoks[0];
        const auto a = actioninfo::parse(tokRaw);
        if (!a.has_value()) {
            game.pushSystemMessage("UNKNOWN ACTION: " + tokRaw);

            // Suggest close action tokens (typos / muscle-memory).
            auto editDistance = [](const std::string& a, const std::string& b) -> int {
                // Levenshtein distance (iterative DP).
                const size_t n = a.size();
                const size_t m = b.size();
                if (n == 0) return static_cast<int>(m);
                if (m == 0) return static_cast<int>(n);

                std::vector<int> prev(m + 1), cur(m + 1);
                for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);

                for (size_t i = 1; i <= n; ++i) {
                    cur[0] = static_cast<int>(i);
                    for (size_t j = 1; j <= m; ++j) {
                        const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                        cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
                    }
                    prev.swap(cur);
                }

                return prev[m];
            };

            const std::string in = actioninfo::normalizeToken(tokRaw);
            struct Cand { int d; std::string tok; };

            std::vector<Cand> cands;
            const size_t n = sizeof(actioninfo::kActionInfoTable) / sizeof(actioninfo::kActionInfoTable[0]);
            cands.reserve(n);

            for (size_t i = 0; i < n; ++i) {
                const auto& info = actioninfo::kActionInfoTable[i];
                if (!info.token || info.token[0] == 0) continue;
                const std::string t = info.token;

                // Very cheap filter: for very short inputs, only suggest tokens that share the first char.
                if (!in.empty() && in.size() <= 3 && !t.empty() && in[0] != t[0]) continue;

                cands.push_back(Cand{editDistance(in, t), t});
            }

            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                if (a.d != b.d) return a.d < b.d;
                return a.tok < b.tok;
            });

            std::vector<std::string> sug;
            for (const auto& c : cands) {
                if (static_cast<int>(sug.size()) >= 5) break;
                // Keep suggestions conservative to avoid noisy spam for tiny inputs.
                if (!in.empty() && in.size() <= 3 && c.d > 2) continue;
                if (!in.empty() && in.size() > 3 && c.d > 3) continue;
                sug.push_back(c.tok);
            }

            if (!sug.empty()) {
                std::string line = "DID YOU MEAN: ";
                for (size_t i = 0; i < sug.size(); ++i) {
                    if (i) line += ", ";
                    line += sug[i];
                }
                game.pushSystemMessage(line);
            }

            game.pushSystemMessage("TIP: press TAB after '@' for completion, or use #binds to list keybind tokens.");
            return;
        }

        game.handleAction(*a);
        return;
    }

    auto toks = splitWS(line);
    if (toks.empty()) return;

    std::string cmdIn = toLower(toks[0]);

    // Normalize common aliases / legacy spellings before matching so prefix-matching
    // stays stable and completion can remain short.
    cmdIn = normalizeExtendedCommandAlias(cmdIn);

    std::vector<std::string> cmds = extendedCommandList();

    // When a command is unknown, suggest close matches to reduce friction (typos, muscle-memory, etc.).
    // This is intentionally conservative to avoid noisy spam for very short inputs.
    auto editDistance = [](const std::string& a, const std::string& b) -> int {
        // Levenshtein distance (iterative DP).
        const size_t n = a.size();
        const size_t m = b.size();
        if (n == 0) return static_cast<int>(m);
        if (m == 0) return static_cast<int>(n);

        std::vector<int> prev(m + 1), cur(m + 1);
        for (size_t j = 0; j <= m; ++j) prev[j] = static_cast<int>(j);

        for (size_t i = 1; i <= n; ++i) {
            cur[0] = static_cast<int>(i);
            for (size_t j = 1; j <= m; ++j) {
                const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
            }
            prev.swap(cur);
        }

        return prev[m];
    };

    auto suggestCommands = [&](const std::string& in) -> std::vector<std::string> {
        struct Cand { int d; std::string cmd; };

        std::vector<Cand> cands;
        cands.reserve(cmds.size());

        for (const auto& c : cmds) {
            // Very cheap filter: for very short inputs, only suggest commands that share the
            // first character to avoid drowning the player in unrelated options.
            if (!in.empty() && in.size() <= 3 && !c.empty() && in[0] != c[0]) continue;

            const int d = editDistance(in, c);
            cands.push_back(Cand{d, c});
        }

        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            if (a.d != b.d) return a.d < b.d;
            return a.cmd < b.cmd;
        });

        int maxDist = 1;
        if (in.size() >= 5) maxDist = 2;
        if (in.size() >= 8) maxDist = 3;
        if (in.size() >= 12) maxDist = 4;
        maxDist = std::min(maxDist, 4);

        std::vector<std::string> out;
        for (const auto& c : cands) {
            if (c.d > maxDist) break;
            out.push_back(c.cmd);
            if (out.size() >= 3) break;
        }

        return out;
    };

    // Exact match first, else unique prefix match.
    std::vector<std::string> matches;
    for (const auto& c : cmds) {
        if (c == cmdIn) {
            matches = {c};
            break;
        }
    }
    if (matches.empty()) {
        for (const auto& c : cmds) {
            if (c.rfind(cmdIn, 0) == 0) matches.push_back(c);
        }
    }

    if (matches.empty()) {
        game.pushSystemMessage("UNKNOWN COMMAND: " + cmdIn);

        const auto sugg = suggestCommands(cmdIn);
        if (!sugg.empty()) {
            std::string msg = "DID YOU MEAN: ";
            for (size_t i = 0; i < sugg.size(); ++i) {
                msg += sugg[i];
                if (i + 1 < sugg.size()) msg += ", ";
            }
            msg += "?";
            game.pushSystemMessage(msg);
        }

        return;
    }

    if (matches.size() > 1) {
        std::string msg = "AMBIGUOUS: " + cmdIn + " (";
        for (size_t i = 0; i < matches.size(); ++i) {
            msg += matches[i];
            if (i + 1 < matches.size()) msg += ", ";
        }
        msg += ")";
        game.pushSystemMessage(msg);
        return;
    }

    const std::string& cmd = matches[0];

    auto arg = [&](size_t i) -> std::string {
        return (i < toks.size()) ? toLower(toks[i]) : std::string();
    };

    auto parseDirToken = [&](std::string tok, int& dx, int& dy) -> bool {
        // Accept vi keys, cardinal words, and numpad digits.
        std::transform(tok.begin(), tok.end(), tok.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

        auto set = [&](int x, int y) { dx = x; dy = y; return true; };

        if (tok == "h" || tok == "left" || tok == "west" || tok == "w" || tok == "4") return set(-1, 0);
        if (tok == "l" || tok == "right" || tok == "east" || tok == "e" || tok == "6") return set(1, 0);
        if (tok == "k" || tok == "up" || tok == "north" || tok == "8") return set(0, -1);
        if (tok == "j" || tok == "down" || tok == "south" || tok == "s" || tok == "2") return set(0, 1);

        if (tok == "y" || tok == "nw" || tok == "upleft" || tok == "7") return set(-1, -1);
        if (tok == "u" || tok == "ne" || tok == "upright" || tok == "9") return set(1, -1);
        if (tok == "b" || tok == "sw" || tok == "downleft" || tok == "1") return set(-1, 1);
        if (tok == "se" || tok == "downright" || tok == "3") return set(1, 1);

        return false;
    };

    // Map an action name to the canonical settings key (bind_<token>). Uses the
    // shared action token registry so #bind/#unbind stay in sync with keybind parsing.
    auto bindKeyForActionName = [&](const std::string& actionRaw, std::string& outKey) -> bool {
        const auto a = actioninfo::parse(actionRaw);
        if (!a.has_value()) return false;

        outKey = actioninfo::bindKey(*a);
        return !outKey.empty();
    };

    if (cmd == "help" || cmd == "?" || cmd == "commands") {
        game.pushSystemMessage("EXTENDED COMMANDS:");
        auto list = extendedCommandList();
        std::string outLine = "  ";
        for (const auto& c : list) {
            if (outLine.size() + c.size() + 1 > 46) {
                game.pushSystemMessage(outLine);
                outLine = "  ";
            }
            outLine += c + " ";
        }
        if (outLine != "  ") game.pushSystemMessage(outLine);
        game.pushSystemMessage("TIP: type a prefix (e.g., 'autop') and press ENTER.");
        game.pushSystemMessage("ACTIONS: @<action> runs a keybind action (TAB completes). EX: @inventory | @toggle_minimap");
        game.pushSystemMessage("INFO: pos [x y] | what [x y] | mapstats (TIP: uses LOOK cursor when active)");
        game.pushSystemMessage("SLOTS: slot [name], save [slot], load [slot], loadauto [slot], saves");
        game.pushSystemMessage("EXPORT: exportlog/exportmap/export/exportall/dump");
        game.pushSystemMessage("REPLAY: record [path] | stoprecord");
        game.pushSystemMessage("MARKS: mark [note|danger|loot] <label> | unmark | marks | travel <index|label>");
        game.pushSystemMessage("ENGRAVE: engrave <text> (costs a turn; wards: 'ELBERETH' | 'SALT' | 'IRON' | 'FIRE')");
        game.pushSystemMessage("SOUND: shout | whistle | listen | throwvoice [x y] (TIP: LOOK cursor works)");
        game.pushSystemMessage("TACTICS: evade (smart step away from visible threats; respects sneak/audibility)");
        game.pushSystemMessage("COMPANIONS: pet [follow|stay|fetch|guard] | tame (needs a FOOD RATION)");
        game.pushSystemMessage("SHRINES: pray [heal|cure|identify|bless|uncurse|recharge] (costs PIETY + cooldown; costs vary by patron domain)");
        game.pushSystemMessage("         donate [amount] (convert gold->piety) | sacrifice (offer a corpse for piety)");
        game.pushSystemMessage("AUGURY: augury (costs gold; shrine/camp only; hints can shift)");
        game.pushSystemMessage("BOUNTIES: bounty (list contracts) | use a completed contract to redeem");
        game.pushSystemMessage("DIG: dig <dir> (requires wielded pickaxe)");
        game.pushSystemMessage("CURSES: CURSED weapons/armor can't be removed until uncursed (scroll or shrine).");
        game.pushSystemMessage("MORTEM: mortem [on/off]");
        game.pushSystemMessage("KEYBINDS: binds | bind <action> <keys> | unbind <action> | reload");
        return;
    }

    if (cmd == "options") {
        game.handleAction(Action::Options);
        return;
    }

    if (cmd == "threat") {
        game.handleAction(Action::ToggleThreatPreview);
        return;
    }

    if (cmd == "evade") {
        game.handleAction(Action::Evade);
        return;
    }

    if (cmd == "messages" || cmd == "msghistory" || cmd == "message_history") {
        game.handleAction(Action::MessageHistory);
        return;
    }

    if (cmd == "preset" || cmd == "controls" || cmd == "keyset") {
        if (toks.size() <= 1) {
            game.pushSystemMessage(std::string("CONTROL PRESET: ") + game.controlPresetDisplayName());
            game.pushSystemMessage("USAGE: #preset modern|nethack");
            game.pushSystemMessage("TIP: this rewrites movement/look/search/kick/sneak binds in procrogue_settings.ini.");
            return;
        }

        ControlPreset p = ControlPreset::Modern;
        if (!parseControlPreset(toks[1], p)) {
            game.pushSystemMessage("UNKNOWN PRESET: " + toks[1]);
            game.pushSystemMessage("VALID: modern | nethack");
            return;
        }

        (void)applyControlPreset(game, p);
        return;
    }

    if (cmd == "binds") {
        // Main thread (SDL) formats the bindings for display.
        game.requestKeyBindsDump();
        return;
    }

    if (cmd == "reload") {
        // Reload settings + keybinds from disk (safe subset applies immediately).
        game.requestConfigReload();
        game.pushSystemMessage("RELOAD REQUESTED.");
        return;
    }

    if (cmd == "record" || cmd == "rec") {
        if (game.replayRecordingActive()) {
            game.pushSystemMessage("ALREADY RECORDING A REPLAY.");
            if (!game.replayRecordingPath().empty()) {
                game.pushSystemMessage("PATH: " + game.replayRecordingPath());
            }
            game.pushSystemMessage("TIP: use stoprecord to finish + close the replay file.");
            return;
        }

        // Optional output path: join remaining tokens to preserve spaces.
        std::string outPath;
        if (toks.size() > 1) {
            for (size_t i = 1; i < toks.size(); ++i) {
                if (i > 1) outPath += " ";
                outPath += toks[i];
            }
            outPath = trim(outPath);
        }

        game.requestReplayRecordStart(outPath);
        return;
    }

    if (cmd == "stoprecord" || cmd == "stoprec") {
        if (!game.replayRecordingActive()) {
            game.pushSystemMessage("NOT RECORDING.");
            game.pushSystemMessage("TIP: use record [path] to start recording a replay.");
            return;
        }
        game.requestReplayRecordStop();
        return;
    }


    if (cmd == "bind" || cmd == "unbind") {
        if (toks.size() <= 1) {
            game.pushSystemMessage("USAGE: #bind <action> <key[,key,...]>");
            game.pushSystemMessage("       #unbind <action>   (resets to defaults)");
            game.pushSystemMessage("TIP: use #binds to list actions + current bindings.");
            return;
        }

        std::string bindKey;
        if (!bindKeyForActionName(toks[1], bindKey)) {
            game.pushSystemMessage("UNKNOWN ACTION: " + toks[1]);
            game.pushSystemMessage("TIP: use #binds to list valid action names.");
            return;
        }

        const std::string settingsPath = game.settingsPath();
        if (settingsPath.empty()) {
            game.pushSystemMessage("SETTINGS PATH UNKNOWN; CAN'T EDIT KEYBINDS.");
            return;
        }

        if (cmd == "unbind") {
            const bool ok = removeIniKey(settingsPath, bindKey);
            if (ok) {
                game.requestKeyBindsReload();
                game.pushSystemMessage("BIND RESET: " + bindKey + " (defaults)");
            } else {
                game.pushSystemMessage("FAILED TO UPDATE SETTINGS FILE.");
            }
            return;
        }

        // bind: join the rest of the tokens to preserve commas/spaces.
        if (toks.size() <= 2) {
            game.pushSystemMessage("USAGE: #bind <action> <key[,key,...]>");
            game.pushSystemMessage("EXAMPLE: #bind inventory i, tab");
            return;
        }

        std::string value;
        for (size_t i = 2; i < toks.size(); ++i) {
            if (i > 2) value += " ";
            value += toks[i];
        }
        value = trim(value);
        if (value.empty()) {
            game.pushSystemMessage("USAGE: #bind <action> <key[,key,...]>");
            return;
        }

        const bool ok = updateIniKey(settingsPath, bindKey, value);
        if (ok) {
            game.requestKeyBindsReload();
            game.pushSystemMessage("BIND SET: " + bindKey + " = " + value);
        } else {
            game.pushSystemMessage("FAILED TO UPDATE SETTINGS FILE.");
        }
        return;
    }

    if (cmd == "save") {
        // Optional save slot: #save <slot>
        const std::string slot = (toks.size() > 1) ? sanitizeSlotName(toks[1]) : std::string();
        const std::string path = slot.empty()
            ? game.defaultSavePath()
            : makeSlotPath(baseSavePathForSlots(game).string(), slot).string();
        (void)game.saveToFile(path);
        return;
    }
    if (cmd == "load") {
        // Optional save slot: #load <slot>
        const std::string slot = (toks.size() > 1) ? sanitizeSlotName(toks[1]) : std::string();
        const std::string path = slot.empty()
            ? game.defaultSavePath()
            : makeSlotPath(baseSavePathForSlots(game).string(), slot).string();
        (void)game.loadFromFileWithBackups(path);
        return;
    }
    if (cmd == "loadauto") {
        // Optional save slot: #loadauto <slot>
        const std::string slot = (toks.size() > 1) ? sanitizeSlotName(toks[1]) : std::string();
        const std::string path = slot.empty()
            ? game.defaultAutosavePath()
            : makeSlotPath(baseAutosavePathForSlots(game).string(), slot).string();
        (void)game.loadFromFileWithBackups(path);
        return;
    }

    if (cmd == "saves") {
        namespace fs = std::filesystem;

        const fs::path saveBase = baseSavePathForSlots(game);
        const fs::path autoBase = baseAutosavePathForSlots(game);

        struct SlotInfo { bool save = false; bool autosave = false; };
        std::map<std::string, SlotInfo> slots;

        auto scanDir = [&](const fs::path& dir, const std::string& stem, const std::string& ext, bool isAuto) {
            std::error_code ec;
            for (const auto& ent : fs::directory_iterator(dir, ec)) {
                if (ec) break;
                if (!ent.is_regular_file(ec)) continue;
                const fs::path p = ent.path();
                if (p.extension().string() != ext) continue;

                const std::string baseName = p.stem().string(); // without extension
                if (baseName == stem) {
                    SlotInfo& si = slots["default"];
                    if (isAuto) si.autosave = true;
                    else si.save = true;
                    continue;
                }

                const std::string prefix = stem + "_";
                if (baseName.rfind(prefix, 0) != 0) continue;

                const std::string slot = baseName.substr(prefix.size());
                if (slot.empty()) continue;

                SlotInfo& si = slots[slot];
                if (isAuto) si.autosave = true;
                else si.save = true;
            }
        };

        fs::path saveDir = saveBase.parent_path();
        if (saveDir.empty()) saveDir = fs::path(".");
        fs::path autoDir = autoBase.parent_path();
        if (autoDir.empty()) autoDir = fs::path(".");

        scanDir(saveDir, saveBase.stem().string(), saveBase.extension().string(), false);
        if (autoDir == saveDir) {
            scanDir(saveDir, autoBase.stem().string(), autoBase.extension().string(), true);
        } else {
            scanDir(autoDir, autoBase.stem().string(), autoBase.extension().string(), true);
        }

        if (slots.empty()) {
            game.pushSystemMessage("NO SAVE SLOTS FOUND.");
            return;
        }

        game.pushSystemMessage("SAVE SLOTS:");
        int shown = 0;
        for (const auto& kv : slots) {
            const std::string& name = kv.first;
            const SlotInfo& si = kv.second;
            std::string slotLine = "  " + name + " [";
            slotLine += si.save ? "save" : "-";
            slotLine += ", ";
            slotLine += si.autosave ? "autosave" : "-";
            slotLine += "]";
            game.pushSystemMessage(slotLine);
            if (++shown >= 30) {
                game.pushSystemMessage("  ...");
                break;
            }
        }
        return;
    }

    if (cmd == "slot") {
        if (toks.size() <= 1) {
            const std::string cur = game.activeSlot().empty() ? std::string("default") : game.activeSlot();
            game.pushSystemMessage("ACTIVE SLOT: " + cur);
            game.pushSystemMessage("USAGE: #slot <name>  (or: #slot default)");
            game.pushSystemMessage("SAVE: " + game.defaultSavePath());
            game.pushSystemMessage("AUTO: " + game.defaultAutosavePath());
            return;
        }

        const std::string raw = toks[1];
        const std::string v = toLower(raw);
        if (v == "default" || v == "none" || v == "off") {
            game.setActiveSlot(std::string());
            game.markSlotDirty();
            game.pushSystemMessage("ACTIVE SLOT SET TO: default");
            return;
        }

        const std::string slot = sanitizeSlotName(raw);
        game.setActiveSlot(slot);
        game.markSlotDirty();
        game.pushSystemMessage("ACTIVE SLOT SET TO: " + slot);
        return;
    }

    if (cmd == "paths") {
        game.pushSystemMessage("PATHS:");
        game.pushSystemMessage("  save: " + game.defaultSavePath());
        game.pushSystemMessage("  autosave: " + game.defaultAutosavePath());
        game.pushSystemMessage("  scores: " + game.defaultScoresPath());
        const std::string sp = game.settingsPath();
        if (!sp.empty()) game.pushSystemMessage("  settings: " + sp);
        else game.pushSystemMessage("  settings: (unknown)");
        return;
    }


    if (cmd == "quit") {
        game.requestQuit();
        game.pushSystemMessage("QUIT REQUESTED. (If nothing happens, press ESC.)");
        return;
    }

    if (cmd == "restart") {
        // Optional: restart with a specific seed (useful for reproducing runs).
        //   #restart 12345
        const std::string v = arg(1);
        if (!v.empty()) {
            try {
                unsigned long s = std::stoul(v, nullptr, 0);
                const uint32_t seed = static_cast<uint32_t>(s);
                game.newGame(seed);
                game.pushSystemMessage("RESTARTED WITH SEED: " + std::to_string(seed));
            } catch (...) {
                game.pushSystemMessage("USAGE: restart [seed]");
            }
            return;
        }

        game.handleAction(Action::Restart);
        return;
    }

    if (cmd == "daily") {
        // Deterministic daily seed (UTC date) for a lightweight "daily challenge".
        //   #daily
        std::string dateIso;
        const uint32_t seed = dailySeedUtc(&dateIso);
        game.newGame(seed);
        game.pushSystemMessage("DAILY RUN (UTC " + dateIso + ") SEED: " + std::to_string(seed));
        return;
    }

    if (cmd == "explore") {
        game.requestAutoExplore();
        return;
    }

    // ---------------------------------------------------------------------
    // Map markers / notes
    // ---------------------------------------------------------------------
    if (cmd == "mark" || cmd == "annotate" || cmd == "note") {
        // Usage:
        //   #mark <label...>                  -> NOTE marker
        //   #mark danger <label...>           -> DANGER marker
        //   #mark loot <label...>             -> LOOT marker
        //   #mark [kind] X Y <label...>       -> marker at coordinates (explored only)
        // TIP: If you're in Look mode, the marker applies to the look cursor.

        if (toks.size() <= 1) {
            game.pushSystemMessage("USAGE: mark [note|danger|loot] <label>");
            game.pushSystemMessage("TIP: open LOOK (:) and move the cursor to mark remote tiles.");
            return;
        }

        MarkerKind kind = MarkerKind::Note;
        size_t i = 1;
        if (i < toks.size()) {
            const std::string k0 = toLower(toks[i]);
            if (k0 == "danger" || k0 == "d" || k0 == "!") {
                kind = MarkerKind::Danger;
                ++i;
            } else if (k0 == "loot" || k0 == "l" || k0 == "$") {
                kind = MarkerKind::Loot;
                ++i;
            } else if (k0 == "note" || k0 == "n") {
                kind = MarkerKind::Note;
                ++i;
            }
        }

        Vec2i pos = game.isLooking() ? game.lookCursor() : game.player().pos;
        // Optional coordinates: #mark [kind] X Y <label...>
        if (i + 2 < toks.size()) {
            try {
                const int x = std::stoi(toks[i], nullptr, 0);
                const int y = std::stoi(toks[i + 1], nullptr, 0);
                pos = { x, y };
                i += 2;
            } catch (...) {
                // Not coordinates; treat as label text.
            }
        }

        std::string label;
        for (size_t j = i; j < toks.size(); ++j) {
            if (j > i) label += " ";
            label += toks[j];
        }
        label = trim(label);
        if (label.empty()) {
            game.pushSystemMessage("USAGE: mark [note|danger|loot] <label>");
            return;
        }

        (void)game.setMarker(pos, kind, label, /*verbose*/true);
        return;
    }

    if (cmd == "unmark" || cmd == "unannotate" || cmd == "clearmark") {
        const Vec2i pos = game.isLooking() ? game.lookCursor() : game.player().pos;
        (void)game.clearMarker(pos, /*verbose*/true);
        return;
    }

    if (cmd == "marks" || cmd == "notes" || cmd == "markers") {
        // Optional:
        //   #marks          -> list marks on this floor
        //   #marks clear    -> clear marks on this floor
        if (toks.size() > 1) {
            const std::string a = toLower(toks[1]);
            if (a == "clear" || a == "reset" || a == "off") {
                game.clearAllMarkers(true);
                return;
            }
        }

        const auto& ms = game.mapMarkers();
        if (ms.empty()) {
            game.pushSystemMessage("NO MARKS ON THIS FLOOR.");
            game.pushSystemMessage("USAGE: #mark <label>");
            return;
        }

        game.pushSystemMessage("MARKS (THIS FLOOR):");
        const size_t maxShow = 30;
        for (size_t i = 0; i < ms.size() && i < maxShow; ++i) {
            const auto& m = ms[i];
            std::ostringstream oss;
            oss << "  [" << (i + 1) << "] (" << m.pos.x << "," << m.pos.y << ") ";
            oss << markerKindName(m.kind) << " \"" << m.label << "\"";
            game.pushSystemMessage(oss.str());
        }
        if (ms.size() > maxShow) {
            game.pushSystemMessage("  ... (" + std::to_string(ms.size() - maxShow) + " more)");
        }
        game.pushSystemMessage("TRAVEL: #travel <index|label-prefix>");
        return;
    }

    if (cmd == "travel" || cmd == "goto" || cmd == "go") {
        // Usage:
        //   #travel 3
        //   #travel 12 34
        //   #travel potion
        // Matches are on the current floor only.
        if (toks.size() <= 1) {
            game.pushSystemMessage("USAGE: travel <mark-index|label-prefix>");
            game.pushSystemMessage("TIP: use #marks to list mark indices.");
            return;
        }

        // Coordinates: #travel X Y (convenient for scripts and map references)
        if (toks.size() >= 3) {
            try {
                const int x = std::stoi(toks[1], nullptr, 0);
                const int y = std::stoi(toks[2], nullptr, 0);
                (void)game.requestAutoTravel({x, y});
                return;
            } catch (...) {
                // fall through to marker lookup
            }
        }

        const auto& ms = game.mapMarkers();
        if (ms.empty()) {
            game.pushSystemMessage("NO MARKS ON THIS FLOOR.");
            game.pushSystemMessage("TIP: #mark <label> to create one, or #travel X Y to travel by coordinates.");
            return;
        }

        // Join the remainder so users can travel to marks with spaces.
        std::string query;
        for (size_t i = 1; i < toks.size(); ++i) {
            if (i > 1) query += " ";
            query += toks[i];
        }
        query = trim(query);
        if (query.empty()) {
            game.pushSystemMessage("USAGE: travel <mark-index|label-prefix>");
            return;
        }

        auto resolvedTravelGoal = [&](Vec2i goal) -> std::optional<Vec2i> {
            const Dungeon& d = game.dungeon();
            if (d.inBounds(goal.x, goal.y) && d.isPassable(goal.x, goal.y)) {
                return goal;
            }

            // If the marker is on a wall/blocked tile (useful for notes), try to
            // find the nearest adjacent passable explored tile.
            Vec2i best{0, 0};
            bool found = false;
            int bestDist = 1'000'000;
            const Vec2i src = game.player().pos;

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const Vec2i q{ goal.x + dx, goal.y + dy };
                    if (!d.inBounds(q.x, q.y)) continue;
                    if (!d.at(q.x, q.y).explored) continue;
                    if (!d.isPassable(q.x, q.y)) continue;
                    const int dist = manhattan(q, src);
                    if (!found || dist < bestDist) {
                        found = true;
                        bestDist = dist;
                        best = q;
                    }
                }
            }

            if (found) {
                game.pushSystemMessage("MARK IS ON A BLOCKED TILE; TRAVELING TO AN ADJACENT TILE.");
                return best;
            }
            return std::nullopt;
        };

        // Try numeric index first.
        int idx = -1;
        try {
            idx = std::stoi(query, nullptr, 0);
        } catch (...) {
            idx = -1;
        }

        if (idx >= 1) {
            const size_t i = static_cast<size_t>(idx - 1);
            if (i >= ms.size()) {
                game.pushSystemMessage("NO SUCH MARK INDEX.");
                return;
            }
            if (auto goal = resolvedTravelGoal(ms[i].pos)) {
                (void)game.requestAutoTravel(*goal);
            } else {
                game.pushSystemMessage("MARK IS ON A BLOCKED TILE WITH NO ADJACENT PASSABLE TILE.");
            }
            return;
        }

        // Otherwise: label prefix match (case-insensitive).
        const std::string pref = toLower(query);
        std::vector<size_t> hits;
        hits.reserve(ms.size());
        for (size_t i = 0; i < ms.size(); ++i) {
            const std::string lab = toLower(ms[i].label);
            if (lab.rfind(pref, 0) == 0) {
                hits.push_back(i);
            }
        }

        if (hits.empty()) {
            game.pushSystemMessage("NO MATCHING MARKS.");
            game.pushSystemMessage("TIP: use #marks to see available labels.");
            return;
        }

        if (hits.size() > 1) {
            game.pushSystemMessage("MULTIPLE MATCHES:");
            const size_t maxShow = 12;
            for (size_t j = 0; j < hits.size() && j < maxShow; ++j) {
                const auto& m = ms[hits[j]];
                std::ostringstream oss;
                oss << "  [" << (hits[j] + 1) << "] (" << m.pos.x << "," << m.pos.y << ") ";
                oss << markerKindName(m.kind) << " \"" << m.label << "\"";
                game.pushSystemMessage(oss.str());
            }
            if (hits.size() > maxShow) {
                game.pushSystemMessage("  ...");
            }
            game.pushSystemMessage("TIP: disambiguate by using an index: #travel <number>.");
            return;
        }

        if (auto goal = resolvedTravelGoal(ms[hits.front()].pos)) {
            (void)game.requestAutoTravel(*goal);
        } else {
            game.pushSystemMessage("MARK IS ON A BLOCKED TILE WITH NO ADJACENT PASSABLE TILE.");
        }
        return;
    }

    if (cmd == "search") {
        // Optional: #search N [all]
        //   #search        -> single search (same as pressing C)
        //   #search 20     -> repeat search up to 20 turns, stop on first discovery or danger
        //   #search 20 all -> repeat full 20 turns even if something is discovered (summary at end)
        if (toks.size() <= 1) {
            game.handleAction(Action::Search);
            return;
        }

        int n = 0;
        try {
            n = std::stoi(toks[1], nullptr, 0);
        } catch (...) {
            game.pushSystemMessage("USAGE: search [N] [all]");
            return;
        }

        n = clampi(n, 1, 2000);

        bool stopOnFind = true;
        if (toks.size() > 2) {
            std::string m = toLower(toks[2]);
            if (m == "all" || m == "full" || m == "continue") stopOnFind = false;
        }

        game.repeatSearch(n, stopOnFind);
        return;
    }

    if (cmd == "rest") {
        game.handleAction(Action::Rest);
        return;
    }

    if (cmd == "craft") {
        game.beginCrafting();
        return;
    }

    if (cmd == "recipes") {
        game.showCraftRecipes();
        return;
    }

    if (cmd == "fish") {
        game.beginFishing();
        return;
    }

    if (cmd == "bounty" || cmd == "bounties") {
        game.showBountyContracts();
        return;
    }

    if (cmd == "sneak") {
        const std::string v = arg(1);
        if (v.empty()) {
            game.toggleSneakMode();
            return;
        }

        bool enabled = game.isSneaking();
        if (v == "on" || v == "1" || v == "true") enabled = true;
        else if (v == "off" || v == "0" || v == "false") enabled = false;
        else {
            game.pushSystemMessage("USAGE: sneak [on|off]");
            return;
        }

        game.setSneakMode(enabled);
        return;
    }


    if (cmd == "dig") {
        if (toks.size() < 2) {
            game.pushSystemMessage("USAGE: dig <dir>    (dir: north/south/east/west, ne/nw/se/sw, or vi/numpad)");
            return;
        }
        int dx = 0, dy = 0;
        if (!parseDirToken(toks[1], dx, dy)) {
            game.pushSystemMessage("UNKNOWN DIRECTION: " + toks[1]);
            return;
        }
        (void)game.digInDirection(dx, dy);
        return;
    }

    if (cmd == "throwtorch") {
        if (toks.size() < 2) {
            game.pushSystemMessage("USAGE: throwtorch <dir>    (throws your currently lit torch)");
            return;
        }
        int dx = 0, dy = 0;
        if (!parseDirToken(toks[1], dx, dy)) {
            game.pushSystemMessage("UNKNOWN DIRECTION: " + toks[1]);
            return;
        }
        (void)game.throwTorchInDirection(dx, dy);
        return;
    }

    if (cmd == "engrave" || cmd == "inscribe") {
        if (toks.size() < 2) {
            game.pushSystemMessage("USAGE: engrave <text>");
            return;
        }
        std::string text;
        for (size_t i = 1; i < toks.size(); ++i) {
            if (!text.empty()) text += " ";
            text += toks[i];
        }
        (void)game.engraveHere(text);
        return;
    }

    if (cmd == "pray") {
        game.prayAtShrine(arg(1));
        return;
    }

    if (cmd == "donate") {
        // #donate [amount]
        // Converts gold into piety. If amount is omitted, a reasonable default is used.
        int amt = 0;
        if (toks.size() > 1) {
            try {
                amt = std::stoi(toks[1], nullptr, 0);
            } catch (...) {
                game.pushSystemMessage("USAGE: donate [amount]");
                return;
            }
        }
        (void)game.donateAtShrine(amt);
        return;
    }

    if (cmd == "sacrifice") {
        (void)game.sacrificeAtShrine();
        return;
    }

    if (cmd == "augury") {
        game.augury();
        return;
    }

    if (cmd == "debt" || cmd == "ledger") {
        game.showDebtLedger();
        return;
    }

    if (cmd == "pay") {
        if (game.playerInShop()) {
            game.payAtShop();
        } else if (game.atCamp()) {
            game.payAtCamp();
        } else {
            game.pushSystemMessage("YOU MUST BE IN A SHOP OR AT CAMP TO PAY.");
        }
        return;
    }

    if (cmd == "timers") {
        if (toks.size() <= 1) {
            game.pushSystemMessage(std::string("EFFECT TIMERS: ") + (game.showEffectTimers() ? "ON" : "OFF"));
            return;
        }

        std::string v = toLower(toks[1]);
        if (v == "on" || v == "true" || v == "1") {
            game.setShowEffectTimers(true);
            game.markSettingsDirty();
            game.pushSystemMessage("EFFECT TIMERS: ON");
            return;
        }
        if (v == "off" || v == "false" || v == "0") {
            game.setShowEffectTimers(false);
            game.markSettingsDirty();
            game.pushSystemMessage("EFFECT TIMERS: OFF");
            return;
        }

        game.pushSystemMessage("USAGE: #timers on/off");
        return;
    }


    if (cmd == "perf" || cmd == "perf_overlay" || cmd == "perfui") {
        if (toks.size() <= 1) {
            game.pushSystemMessage(std::string("PERF OVERLAY: ") + (game.perfOverlayEnabled() ? "ON" : "OFF"));
            game.pushSystemMessage("USAGE: #perf on/off");
            return;
        }

        const std::string v = toLower(toks[1]);
        if (v == "on" || v == "true" || v == "1") {
            game.setPerfOverlayEnabled(true);
            game.markSettingsDirty();
            game.pushSystemMessage("PERF OVERLAY: ON");
            return;
        }
        if (v == "off" || v == "false" || v == "0") {
            game.setPerfOverlayEnabled(false);
            game.markSettingsDirty();
            game.pushSystemMessage("PERF OVERLAY: OFF");
            return;
        }
        if (v == "toggle" || v == "t") {
            game.setPerfOverlayEnabled(!game.perfOverlayEnabled());
            game.markSettingsDirty();
            game.pushSystemMessage(std::string("PERF OVERLAY: ") + (game.perfOverlayEnabled() ? "ON" : "OFF"));
            return;
        }

        game.pushSystemMessage("USAGE: #perf on/off");
        return;
    }

    if (cmd == "seed") {
        game.pushSystemMessage("SEED: " + std::to_string(game.seed()));
        return;
    }

    if (cmd == "pos") {
        // Usage:
        //   #pos          (uses LOOK cursor when active, else player)
        //   #pos X Y
        Vec2i p = game.player().pos;
        bool usedLook = false;

        if (game.isLooking() && toks.size() < 3) {
            p = game.lookCursor();
            usedLook = true;
        } else if (toks.size() >= 3) {
            try {
                const int x = std::stoi(toks[1], nullptr, 0);
                const int y = std::stoi(toks[2], nullptr, 0);
                p = {x, y};
            } catch (...) {
                game.pushSystemMessage("USAGE: pos [X Y]");
                game.pushSystemMessage("TIP: open LOOK (:) and move the cursor, then #pos.");
                return;
            }
        }

        const Dungeon& d = game.dungeon();
        const Vec2i pp = game.player().pos;
        const int dist = std::abs(p.x - pp.x) + std::abs(p.y - pp.y);

        std::string levelTag;
        if (game.branch() == DungeonBranch::Camp) levelTag = "CAMP";
        else levelTag = "D" + std::to_string(game.depth());

        std::ostringstream ss;
        ss << (usedLook ? "LOOK" : "POS") << ": " << p.x << " " << p.y;
        ss << " | LEVEL " << levelTag;
        ss << " | MAP " << d.width << "x" << d.height;
        ss << " | DIST " << dist;
        game.pushSystemMessage(ss.str());
        return;
    }

    if (cmd == "what") {
        // Usage:
        //   #what         (uses LOOK cursor when active, else player)
        //   #what X Y
        Vec2i p = game.player().pos;
        bool usedLook = false;

        if (game.isLooking() && toks.size() < 3) {
            p = game.lookCursor();
            usedLook = true;
        } else if (toks.size() >= 3) {
            try {
                const int x = std::stoi(toks[1], nullptr, 0);
                const int y = std::stoi(toks[2], nullptr, 0);
                p = {x, y};
            } catch (...) {
                game.pushSystemMessage("USAGE: what [X Y]");
                game.pushSystemMessage("TIP: open LOOK (:) and move the cursor, then #what.");
                return;
            }
        }

        std::ostringstream ss;
        ss << (usedLook ? "LOOK" : "AT") << " " << p.x << " " << p.y << ": " << game.describeAt(p);
        game.pushSystemMessage(ss.str());
        return;
    }

    if (cmd == "vtubers" || cmd == "vt") {
        // Lists procedural VTuber personas currently present (inventory + ground).
        // Usage:
        //   #vtubers        (full)
        //   #vtubers short  (omit catchphrase)
        const bool shortMode = (toks.size() >= 2 && toks[1] == "short");

        struct Entry {
            uint32_t seed = 0u;
            ItemKind kind = ItemKind::VtuberFigurine;
        };
        std::vector<Entry> found;
        found.reserve(32);

        auto pushUnique = [&](ItemKind k, uint32_t seed) {
            if (seed == 0u) return;
            for (const auto& e : found) {
                if (e.seed == seed) return;
            }
            found.push_back({seed, k});
        };

        // Inventory
        for (const auto& it : game.inventory()) {
            if (!isVtuberCollectible(it.kind)) continue;
            pushUnique(it.kind, it.spriteSeed);
        }
        // Ground (current level)
        for (const auto& gi : game.groundItems()) {
            if (!isVtuberCollectible(gi.item.kind)) continue;
            pushUnique(gi.item.kind, gi.item.spriteSeed);
        }

        int figs = 0, cards = 0;
        for (const auto& e : found) {
            if (e.kind == ItemKind::VtuberFigurine) ++figs;
            else if (e.kind == ItemKind::VtuberHoloCard) ++cards;
        }

        {
            std::ostringstream ss;
            ss << "VTUBERS " << found.size();
            ss << " | FIG " << figs;
            ss << " | CARD " << cards;
            if (shortMode) ss << " | SHORT";
            game.pushSystemMessage(ss.str());
        }

        if (found.empty()) {
            game.pushSystemMessage("TIP: Treasure rooms can rarely drop VTuber figurines and holo cards.");
            return;
        }

        std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
            if (a.kind != b.kind) return static_cast<int>(a.kind) < static_cast<int>(b.kind);
            return a.seed < b.seed;
        });

        const int maxLines = 18;
        int lines = 0;
        for (const auto& e : found) {
            if (lines++ >= maxLines) {
                game.pushSystemMessage("... (MORE TRUNCATED)");
                break;
            }

            const std::string name = vtuberStageName(e.seed);
            const std::string arch = vtuberArchetype(e.seed);
            const std::string agency = vtuberAgency(e.seed);
            const std::string tag = vtuberStreamTag(e.seed);
            const std::string fol = vtuberFollowerText(e.seed);
            const std::string emo = vtuberEmote(e.seed);
            const VtuberRarity rar = vtuberRarity(e.seed);

            std::string title = name;
            std::string edTag;
            int serial = 0;
            if (e.kind == ItemKind::VtuberHoloCard) {
                const VtuberCardEdition ed = vtuberCardEdition(e.seed);
                if (ed == VtuberCardEdition::Collab) {
                    const uint32_t ps = vtuberCollabPartnerSeed(e.seed);
                    title = title + " x " + vtuberStageName(ps);
                }
                const char* t = vtuberCardEditionTag(ed);
                if (t && t[0]) edTag = t;
                if (vtuberCardHasSerial(ed)) serial = vtuberCardSerial(e.seed);
            }

            std::ostringstream ss;
            ss << (e.kind == ItemKind::VtuberFigurine ? "FIG" : "CARD") << ": ";
            ss << title << " [" << vtuberRarityName(rar) << "]";
            if (!edTag.empty()) {
                ss << " {" << edTag << "}";
                if (serial > 0) ss << " #" << serial;
            }
            ss << " | " << arch;
            ss << " | " << agency;
            ss << " | " << tag;
            ss << " | " << fol;
            ss << " | " << emo;
            if (!shortMode) {
                // Keep it compact for the message log.
                std::string cp = vtuberCatchphrase(e.seed);
                if (cp.size() > 46) cp = cp.substr(0, 46) + "...";
                ss << " | \"" << cp << "\"";
            }
            game.pushSystemMessage(ss.str());
        }

        return;
    }

    if (cmd == "mapstats") {
        const Dungeon& d = game.dungeon();

        const int total = std::max(0, d.width) * std::max(0, d.height);
        int explored = 0;
        int visible = 0;
        int chasm = 0;
        int doors = 0;
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (t.explored) ++explored;
                if (t.visible) ++visible;
                if (t.type == TileType::Chasm) ++chasm;
                if (t.type == TileType::DoorClosed || t.type == TileType::DoorLocked || t.type == TileType::DoorOpen) ++doors;
            }
        }
        const int pct = (total > 0) ? (explored * 100) / total : 0;

        // Monsters (current level)
        int hostiles = 0;
        int allies = 0;
        const int playerId = game.player().id;
        for (const auto& e : game.entities()) {
            if (e.id == playerId) continue;
            if (e.friendly) ++allies;
            else ++hostiles;
        }

        // Traps (current level)
        int trapsTotal = 0;
        int trapsDiscovered = 0;
        for (const auto& tr : game.traps()) {
            ++trapsTotal;
            if (tr.discovered) ++trapsDiscovered;
        }

        const int rooms = static_cast<int>(d.rooms.size());
        const int items = static_cast<int>(game.groundItems().size());
        const int marks = static_cast<int>(game.mapMarkers().size());
        const int engr = static_cast<int>(game.engravings().size());

        {
            std::ostringstream ss;
            ss << "MAP " << d.width << "x" << d.height;
            ss << " | EXPLORED " << explored << "/" << total << " (" << pct << "%)";
            ss << " | VISIBLE " << visible;
            game.pushSystemMessage(ss.str());
        }
        {
            std::ostringstream ss;
            ss << "ROOMS " << rooms;
            ss << " | MONSTERS " << (hostiles + allies) << " (HOSTILE " << hostiles << ", ALLY " << allies << ")";
            ss << " | ITEMS " << items;
            game.pushSystemMessage(ss.str());
        }

        // RoomsGraph ("ruins") generator debug stats (Poisson placement + Delaunay graph).
        // Only shown when this floor actually used the rooms-graph generator.
        if (d.roomsGraphPoissonPointCount > 0 || d.roomsGraphDelaunayEdgeCount > 0) {
            std::ostringstream ss;
            ss << "RUINSGEN";
            ss << " | POISSON " << d.roomsGraphPoissonPointCount;
            ss << " | PLACED " << d.roomsGraphPoissonRoomCount;
            if (d.roomsGraphDelaunayEdgeCount > 0) ss << " | DT " << d.roomsGraphDelaunayEdgeCount;
            if (d.roomsGraphLoopEdgeCount > 0) ss << " | LOOPS " << d.roomsGraphLoopEdgeCount;
            game.pushSystemMessage(ss.str());
        }

        // Cavern generator debug stats: metaballs variant telemetry.
        // Only shown when this floor actually used metaballs.
        if (d.cavernMetaballsUsed) {
            std::ostringstream ss;
            ss << "CAVERNGEN";
            ss << " | METABALLS " << d.cavernMetaballBlobCount;
            if (d.cavernMetaballKeptTiles > 0) ss << " | KEPT " << d.cavernMetaballKeptTiles;
            game.pushSystemMessage(ss.str());
        }

        // Maze generator debug stats: backtracker vs Wilson (loop-erased random walks).
        // Only shown when the floor used the Maze gen kind.
        if (d.mazeAlgorithm != MazeAlgorithm::None) {
            std::ostringstream ss;
            ss << "MAZEGEN";
            ss << " | " << mazeAlgorithmName(d.mazeAlgorithm);
            if (d.mazeChamberCount > 0) ss << " | CHAMBERS " << d.mazeChamberCount;
            if (d.mazeBreakCount > 0) ss << " | BREAKS " << d.mazeBreakCount;
            if (d.mazeAlgorithm == MazeAlgorithm::Wilson) {
                ss << " | WALKS " << d.mazeWilsonWalkCount;
                ss << " | STEPS " << d.mazeWilsonStepCount;
                if (d.mazeWilsonLoopEraseCount > 0) ss << " | ERASED " << d.mazeWilsonLoopEraseCount;
                if (d.mazeWilsonMaxPathLen > 0) ss << " | MAXPATH " << d.mazeWilsonMaxPathLen;
            }
            game.pushSystemMessage(ss.str());
        }


        {
            int treasure = 0;
            int lair = 0;
            int shrine = 0;
            int shop = 0;
            int themed = 0;
            int secret = 0;
            int vault = 0;
            for (const auto& r : d.rooms) {
                switch (r.type) {
                    case RoomType::Treasure: ++treasure; break;
                    case RoomType::Lair: ++lair; break;
                    case RoomType::Shrine: ++shrine; break;
                    case RoomType::Shop: ++shop; break;
                    case RoomType::Secret: ++secret; break;
                    case RoomType::Vault: ++vault; break;
                    case RoomType::Armory:
                    case RoomType::Library:
                    case RoomType::Laboratory:
                        ++themed;
                        break;
                    default:
                        break;
                }
            }

            std::ostringstream ss;
            ss << "SPECIALS";
            ss << " | TREASURE " << treasure;
            ss << " | LAIR " << lair;
            ss << " | SHRINE " << shrine;
            ss << " | SHOP " << shop;
            ss << " | THEMED " << themed;
            if (secret > 0 || vault > 0) {
                ss << " | SECRET " << secret;
                ss << " | VAULT " << vault;
            }
            if (d.spineRoomCount > 0) {
                ss << " | SPINE " << d.spineRoomCount;
            }
            if (d.specialRoomMinSep > 0) {
                ss << " | MINSEP " << d.specialRoomMinSep;
            }
            game.pushSystemMessage(ss.str());
        }
        {
            std::ostringstream ss;
            ss << "TRAPS " << trapsDiscovered << "/" << trapsTotal;
            ss << " | MARKS " << marks;
            ss << " | ENGR " << engr;
            ss << " | DOORS " << doors;
            ss << " | CHASMS " << chasm;
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            ss << "INTERROOM DOORS " << d.interRoomDoorCount;
            if (d.interRoomDoorCount > 0) {
                ss << " | LOCKED " << d.interRoomDoorLockedCount;
                ss << " | SECRET " << d.interRoomDoorSecretCount;
            }
            game.pushSystemMessage(ss.str());
        }
        {
            std::ostringstream ss;
            const bool haveUp = d.inBounds(d.stairsUp.x, d.stairsUp.y);
            const bool haveDown = d.inBounds(d.stairsDown.x, d.stairsDown.y);
            if (!haveUp || !haveDown) {
                ss << "STAIRS PATH N/A";
            } else {
                ss << "STAIRS PATH " << (d.stairsRedundancyOk ? "REDUNDANT" : "BRIDGED");
                ss << " | BRIDGES " << d.stairsBridgeCount;
                ss << " | BYPASSES " << d.stairsBypassLoopCount;
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            // Global bridgeiness (whole-map chokepoints) and how much we "weaved" it away.
            ss << "GRAPH BRIDGES " << d.globalBridgeCountAfter;
            if (d.globalBridgeCountBefore != d.globalBridgeCountAfter) {
                ss << " (WAS " << d.globalBridgeCountBefore << ")";
            }
            ss << " | WEAVES " << d.globalBypassLoopCount;
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            if (d.biomeZoneCount > 0) {
                ss << "BIOMES " << d.biomeZoneCount;
                ss << " | PILLARZ " << d.biomePillarZoneCount;
                ss << " | RUBBLEZ " << d.biomeRubbleZoneCount;
                ss << " | CRACKZ " << d.biomeCrackedZoneCount;
                ss << " | EDITS " << d.biomeEdits;
            } else {
                ss << "BIOMES 0";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            ss << "TERRAIN HF";
            ss << " | RIDGE PILLARS " << d.heightfieldRidgePillarCount;
            ss << " | SCREE BOULDERS " << d.heightfieldScreeBoulderCount;
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            ss << "TERRAIN FLUVIAL";
            ss << " | GULLIES " << d.fluvialGullyCount;
            ss << " | CHASM " << d.fluvialChasmCount;
            ss << " | CAUSEWAYS " << d.fluvialCausewayCount;
            game.pushSystemMessage(ss.str());
        }

        // Overworld-only: deterministic wilderness POIs + hydrology (springs, brooks, ponds, strongholds).
        // Only shown when present.
        if (d.overworldSpringCount > 0 || d.overworldBrookCount > 0 || d.overworldStrongholdCount > 0) {
            std::ostringstream ss;
            ss << "OVERWORLD";
            if (d.overworldSpringCount > 0) ss << " | SPRINGS " << d.overworldSpringCount;
            if (d.overworldBrookCount > 0) {
                ss << " | BROOKS " << d.overworldBrookCount;
                ss << " (" << d.overworldBrookTiles << " TILES)";
                if (d.overworldPondCount > 0) ss << " | PONDS " << d.overworldPondCount;
            }
            if (d.overworldStrongholdCount > 0) {
                ss << " | STRONGHOLDS " << d.overworldStrongholdCount;
                ss << " (" << d.overworldStrongholdBuildingCount << " BLDG";
                if (d.overworldStrongholdCacheCount > 0) ss << ", " << d.overworldStrongholdCacheCount << " CACHE";
                ss << ")";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            // Procedural biolum terrain stats (lichen/crystal glow): counts of tiles that can emit light.
            d.ensureMaterials(game.materialWorldSeed(), game.branch(), game.materialDepth(), game.dungeonMaxDepth());

            int bioTiles = 0;
            int bioStrong = 0;
            int bioCrystal = 0;
            int bioMoss = 0;

            for (int y = 0; y < d.height; ++y) {
                for (int x = 0; x < d.width; ++x) {
                    if (d.at(x, y).type != TileType::Floor) continue;
                    const uint8_t g = d.biolumAtCached(x, y);
                    if (g == 0u) continue;
                    ++bioTiles;
                    if (g >= 48u) ++bioStrong;

                    const TerrainMaterial m = d.materialAtCached(x, y);
                    if (m == TerrainMaterial::Crystal) ++bioCrystal;
                    if (m == TerrainMaterial::Moss) ++bioMoss;
                }
            }

            std::ostringstream ss;
            ss << "BIOLUM " << bioTiles;
            ss << " | STRONG " << bioStrong;
            ss << " | CRYSTAL " << bioCrystal;
            ss << " | MOSS " << bioMoss;
            game.pushSystemMessage(ss.str());
        }


        {
            std::ostringstream ss;
            ss << "FURNISH";
            ss << " | SYMROOMS " << d.symmetryRoomCount;
            ss << " | SYMOBS " << d.symmetryObstacleCount;
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            ss << "PALETTE " << (game.procPaletteEnabled() ? "ON" : "OFF");
            ss << " | STRENGTH " << game.procPaletteStrength();
            ss << " | HUE " << game.procPaletteHueDeg();
            ss << " | SAT " << game.procPaletteSaturationPct();
            ss << " | BRIGHT " << game.procPaletteBrightnessPct();
            ss << " | SPATIAL " << game.procPaletteSpatialStrength();
            game.pushSystemMessage(ss.str());
        }

{
    // Deterministic "substrate materials" (STONE/BRICK/BASALT/...) used for tinting and LOOK adjectives.
    d.ensureMaterials(game.materialWorldSeed(), game.branch(), game.materialDepth(), game.dungeonMaxDepth());

    std::vector<int> counts(static_cast<size_t>(TerrainMaterial::COUNT), 0);
    int materialTotal = 0;

    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const Tile& t = d.at(x, y);
            if (t.type == TileType::Chasm) continue; // void is not a "material"
            const TerrainMaterial m = d.materialAtCached(x, y);
            counts[static_cast<size_t>(m)] += 1;
            materialTotal += 1;
        }
    }

    struct Entry { int idx; int count; };
    std::vector<Entry> top;
    top.reserve(counts.size());
    for (int i = 0; i < static_cast<int>(counts.size()); ++i) {
        top.push_back({i, counts[static_cast<size_t>(i)]});
    }
    std::sort(top.begin(), top.end(), [](const Entry& a, const Entry& b) { return a.count > b.count; });

    std::ostringstream ss;
    ss << "MATERIALS";
    ss << " | FX STEP+SCENT";
    ss << " | CELL " << d.materialCellSize();

    const int kShow = std::min(3, static_cast<int>(top.size()));
    for (int i = 0; i < kShow; ++i) {
        if (top[i].count <= 0 || materialTotal <= 0) break;
        const int materialPct = static_cast<int>(std::round(100.0 * static_cast<double>(top[i].count) / static_cast<double>(materialTotal)));
        ss << " | " << terrainMaterialName(static_cast<TerrainMaterial>(top[i].idx)) << " " << materialPct << "%";
    }

    game.pushSystemMessage(ss.str());
}

{
    // Procedural ecosystems (biome seeds): distribution across walkable tiles.
    // Uses the same deterministic cache as materials.
    d.ensureMaterials(game.materialWorldSeed(), game.branch(), game.materialDepth(), game.dungeonMaxDepth());

    std::vector<int> counts(static_cast<size_t>(EcosystemKind::COUNT), 0);
    int ecoTotal = 0;

    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const Tile& t = d.at(x, y);
            if (t.type != TileType::Floor) continue;
            const EcosystemKind e = d.ecosystemAtCached(x, y);
            counts[static_cast<size_t>(e)] += 1;
            ecoTotal += 1;
        }
    }

    struct Entry { int idx; int count; };
    std::vector<Entry> top;
    top.reserve(counts.size());
    for (int i = 0; i < static_cast<int>(counts.size()); ++i) {
        top.push_back({i, counts[static_cast<size_t>(i)]});
    }
    std::sort(top.begin(), top.end(), [](const Entry& a, const Entry& b) { return a.count > b.count; });

    std::ostringstream ss;
    ss << "ECOSYSTEMS";
    ss << " | SEEDS " << d.ecosystemSeedsCached().size();

    int shown = 0;
    for (const auto& e : top) {
        if (e.idx == static_cast<int>(EcosystemKind::None)) continue;
        if (e.count <= 0 || ecoTotal <= 0) break;
        const int pct = static_cast<int>(std::round(100.0 * static_cast<double>(e.count) / static_cast<double>(ecoTotal)));
        ss << " | " << ecosystemKindName(static_cast<EcosystemKind>(e.idx)) << " " << pct << "%";
        if (++shown >= 3) break;
    }

    game.pushSystemMessage(ss.str());
}

        {
            std::ostringstream ss;
            ss << "ENDLESS " << (game.infiniteWorldEnabled() ? "ON" : "OFF");
            ss << " | KEEP " << game.infiniteKeepWindow();
            game.pushSystemMessage(ss.str());
        }

        // Infinite World macro theming: deep floors belong to larger "strata" bands.
        if (game.infiniteWorldEnabled() && game.branch() == DungeonBranch::Main && game.depth() > Game::DUNGEON_MAX_DEPTH) {
            std::ostringstream ss;
            if (d.endlessStratumIndex >= 0 && d.endlessStratumLen > 0) {
                ss << "STRATUM " << (d.endlessStratumIndex + 1);
                ss << " | THEME " << endlessStratumThemeName(d.endlessStratumTheme);
                ss << " | BAND " << d.endlessStratumStartDepth << "-" << (d.endlessStratumStartDepth + d.endlessStratumLen - 1);
                ss << " | POS " << (d.endlessStratumLocal + 1) << "/" << d.endlessStratumLen;
                if (d.endlessStratumSeed != 0u) {
                    ss << " | SEED 0x" << std::hex << std::uppercase << d.endlessStratumSeed << std::dec;
                }
            } else {
                ss << "STRATUM ?";
            }
            game.pushSystemMessage(ss.str());
        }

        // Infinite World macro terrain: stratum-aligned persistent rift / faultline.
        if (game.infiniteWorldEnabled() && game.branch() == DungeonBranch::Main && game.depth() > Game::DUNGEON_MAX_DEPTH) {
            std::ostringstream ss;
            if (d.endlessRiftActive) {
                ss << "RIFT ON";
                ss << " | INT " << d.endlessRiftIntensityPct << "%";
                ss << " | CHASM " << d.endlessRiftChasmCount;
                ss << " | BRIDGES " << d.endlessRiftBridgeCount;
                ss << " | BOULDERS " << d.endlessRiftBoulderCount;
                if (d.endlessRiftSeed != 0u) {
                    ss << " | SEED 0x" << std::hex << std::uppercase << d.endlessRiftSeed << std::dec;
                }
            } else {
                ss << "RIFT OFF";
                if (d.endlessRiftIntensityPct > 0) {
                    ss << " | INT " << d.endlessRiftIntensityPct << "%";
                }
            }
            game.pushSystemMessage(ss.str());
        }

        // Finite campaign macro terrain: run-seeded fault band (depth <= maxDepth).
        // Only emits a line when this floor is within the band (or if the band was skipped).
        if (game.branch() == DungeonBranch::Main && game.depth() <= Game::DUNGEON_MAX_DEPTH &&
            (d.runFaultBandLen > 0 || d.runFaultIntensityPct > 0)) {
            std::ostringstream ss;
            if (d.runFaultActive) {
                ss << "FAULT ON";
            } else {
                ss << "FAULT SKIP";
            }
            if (d.runFaultBandStartDepth > 0 && d.runFaultBandLen > 0) {
                ss << " | BAND " << d.runFaultBandStartDepth << "-"
                   << (d.runFaultBandStartDepth + d.runFaultBandLen - 1);
                ss << " | POS " << (d.runFaultBandLocal + 1) << "/" << d.runFaultBandLen;
            }
            if (d.runFaultIntensityPct > 0) {
                ss << " | INT " << d.runFaultIntensityPct << "%";
            }
            if (d.runFaultActive) {
                ss << " | CHASM " << d.runFaultChasmCount;
                ss << " | BRIDGES " << d.runFaultBridgeCount;
                ss << " | BOULDERS " << d.runFaultBoulderCount;
            }
            if (d.runFaultSeed != 0u) {
                ss << " | SEED 0x" << std::hex << std::uppercase << d.runFaultSeed << std::dec;
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            if (d.fireLaneMaxAfter > 0) {
                ss << "LANES MAX " << d.fireLaneMaxAfter;
                if (d.fireLaneCoverCount > 0 || d.fireLaneChicaneCount > 0) {
                    ss << " (WAS " << d.fireLaneMaxBefore << ")";
                }
                ss << " | COVER " << d.fireLaneCoverCount;
                ss << " | CHICANES " << d.fireLaneChicaneCount;
            } else {
                ss << "LANES N/A";
            }
            game.pushSystemMessage(ss.str());
        }
        {
            std::ostringstream ss;
            if (d.openSpaceClearanceMaxAfter > 0) {
                ss << "OPEN MAX " << d.openSpaceClearanceMaxAfter;
                if (d.openSpacePillarCount > 0 || d.openSpaceBoulderCount > 0) {
                    ss << " (WAS " << d.openSpaceClearanceMaxBefore << ")";
                }
                ss << " | PILLARS " << d.openSpacePillarCount;
                ss << " | BOULDERS " << d.openSpaceBoulderCount;
            } else {
                ss << "OPEN N/A";
            }
            game.pushSystemMessage(ss.str());
        }
        {
            std::ostringstream ss;
            if (d.moatedRoomCount > 0) {
                ss << "MOATS " << d.moatedRoomCount;
                ss << " | BRIDGES " << d.moatedRoomBridgeCount;
                ss << " | CHASM " << d.moatedRoomChasmCount;
            } else {
                ss << "MOATS 0";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            if (d.riftCacheCount > 0) {
                ss << "POCKET CACHES " << d.riftCacheCount;
                ss << " | BOULDERS " << d.riftCacheBoulderCount;
                ss << " | CHASM " << d.riftCacheChasmCount;
            } else {
                ss << "POCKET CACHES 0";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            if (d.annexCount > 0 || d.annexKeyGateCount > 0 || d.annexWfcCount > 0 || d.annexFractalCount > 0) {
                ss << "ANNEXES " << d.annexCount;
                if (d.annexKeyGateCount > 0) ss << " | KEYGATES " << d.annexKeyGateCount;
                if (d.annexWfcCount > 0) ss << " | WFC " << d.annexWfcCount;
                if (d.annexFractalCount > 0) ss << " | FRACTAL " << d.annexFractalCount;
            } else {
                ss << "ANNEXES 0";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            if (d.perimTunnelCarvedTiles > 0 || d.perimTunnelHatchCount > 0) {
                ss << "PERIM TUNNELS " << d.perimTunnelCarvedTiles;
                ss << " | HATCHES " << d.perimTunnelHatchCount;
                if (d.perimTunnelLockedCount > 0) ss << " | LOCKED " << d.perimTunnelLockedCount;
                if (d.perimTunnelCacheCount > 0) ss << " | CACHES " << d.perimTunnelCacheCount;
            } else {
                ss << "PERIM TUNNELS 0";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            if (d.crawlspaceNetworkCount > 0 || d.crawlspaceDoorCount > 0) {
                ss << "CRAWLSPACES " << d.crawlspaceNetworkCount;
                ss << " | CARVED " << d.crawlspaceCarvedTiles;
                ss << " | DOORS " << d.crawlspaceDoorCount;
                if (d.crawlspaceCacheCount > 0) ss << " | CACHES " << d.crawlspaceCacheCount;
            } else {
                ss << "CRAWLSPACES 0";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            if (d.crosscutTunnelCount > 0 || d.crosscutCarvedTiles > 0) {
                ss << "CROSSCUTS " << d.crosscutTunnelCount;
                ss << " | CARVED " << d.crosscutCarvedTiles;
                if (d.crosscutDoorLockedCount > 0) ss << " | LOCKED " << d.crosscutDoorLockedCount;
                if (d.crosscutDoorSecretCount > 0) ss << " | SECRET " << d.crosscutDoorSecretCount;
            } else {
                ss << "CROSSCUTS 0";
            }
            game.pushSystemMessage(ss.str());
        }

        {
            std::ostringstream ss;
            const int atts = std::max(1, d.genPickAttempts);
            ss << "GEN PICK " << (d.genPickChosenIndex + 1) << "/" << atts;
            ss << " | SCORE " << d.genPickScore;
            if (d.genPickSeed != 0u) {
                ss << " | SEED 0x" << std::hex << std::uppercase << d.genPickSeed << std::dec;
            }
            game.pushSystemMessage(ss.str());
        }
        return;
    }

    if (cmd == "version") {
        game.pushSystemMessage(std::string("VERSION: ") + PROCROGUE_VERSION);
        return;
    }

    if (cmd == "name") {
        if (toks.size() <= 1) {
            game.pushSystemMessage("NAME: " + game.playerName());
            return;
        }

        // Join the rest of the tokens to allow spaces.
        std::string n;
        for (size_t i = 1; i < toks.size(); ++i) {
            if (i > 1) n += " ";
            n += toks[i];
        }

        game.setPlayerName(n);
        game.markSettingsDirty();
        game.pushSystemMessage("NAME SET TO: " + game.playerName());
        return;
    }

    if (cmd == "class") {
        if (toks.size() <= 1) {
            std::ostringstream ss;
            ss << "CLASS: " << game.playerClassDisplayName()
               << " (" << game.playerClassIdString() << ")";
            game.pushSystemMessage(ss.str());
            game.pushSystemMessage("USAGE: #CLASS <adventurer|knight|rogue|archer|wizard> [same|random]");
            game.pushSystemMessage("DEFAULT: same  (restarts the run, preserving seed)");
            return;
        }

        PlayerClass pc = PlayerClass::Adventurer;
        if (!parsePlayerClass(toks[1], pc)) {
            game.pushSystemMessage("UNKNOWN CLASS. TRY: ADVENTURER, KNIGHT, ROGUE, ARCHER, WIZARD.");
            return;
        }

        const uint32_t oldSeed = game.seed();
        game.setPlayerClass(pc);
        game.markSettingsDirty();

        bool randomSeed = false;
        if (toks.size() > 2) {
            std::string mode = toLower(toks[2]);
            if (mode == "random" || mode == "new") randomSeed = true;
        }

        if (randomSeed) {
            game.handleAction(Action::Restart);
            game.pushSystemMessage(std::string("RESTARTED AS ") + game.playerClassDisplayName() + ".");
        } else {
            game.newGame(oldSeed);
            game.pushSystemMessage(std::string("RESTARTED AS ") + game.playerClassDisplayName() + " (SEED PRESERVED).");
        }
        return;
    }

    if (cmd == "scores") {
        int n = 10;
        if (toks.size() > 1) {
            try {
                n = std::stoi(toks[1]);
            } catch (...) {
                n = 10;
            }
        }
        n = clampi(n, 1, 60);

        const auto& es = game.scoreBoard().entries();
        if (es.empty()) {
            game.pushSystemMessage("NO SCORES YET.");
            return;
        }

        game.pushSystemMessage("TOP SCORES:");
        const int count = std::min<int>(n, static_cast<int>(es.size()));
        for (int i = 0; i < count; ++i) {
            const auto& e = es[static_cast<size_t>(i)];
            std::string who = e.name.empty() ? std::string("PLAYER") : e.name;
            if (!e.playerClass.empty()) {
                PlayerClass pc = PlayerClass::Adventurer;
                if (parsePlayerClass(e.playerClass, pc)) {
                    who += " (" + std::string(playerClassDisplayName(pc)) + ")";
                } else {
                    who += " (" + e.playerClass + ")";
                }
            }
            const std::string res = e.won ? std::string("WIN") : std::string("DEAD");

            // Historic scoreboard entries only store a numeric depth. Since the game now
            // starts in the Camp branch at depth 0, show "CAMP" for depth 0 for clarity.
            const std::string depthTag = (e.branch == 0) ? std::string("CAMP")
                : (e.branch == 1) ? ("D" + std::to_string(e.depth))
                                : ("B" + std::to_string(static_cast<int>(e.branch)) + "D" + std::to_string(e.depth));

            std::string scoreLine = "#" + std::to_string(i + 1) + " " + who + " " + res + " ";
            scoreLine += "S" + std::to_string(e.score) + " " + depthTag;
            if (!e.slot.empty() && e.slot != "default") scoreLine += " [" + e.slot + "]";
            scoreLine += " T" + std::to_string(e.turns) + " K" + std::to_string(e.kills);
            scoreLine += " SEED" + std::to_string(e.seed);
            if (!e.cause.empty()) scoreLine += " " + e.cause;

            game.pushSystemMessage(scoreLine);
        }
        return;
    }

    if (cmd == "history") {
        int n = 10;
        if (toks.size() > 1) {
            try {
                n = std::stoi(toks[1]);
            } catch (...) {
                n = 10;
            }
        }
        n = clampi(n, 1, 60);

        const auto& es = game.scoreBoard().entries();
        if (es.empty()) {
            game.pushSystemMessage("NO RUNS RECORDED YET.");
            return;
        }

        std::vector<size_t> idx(es.size());
        std::iota(idx.begin(), idx.end(), size_t{0});

        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            const auto& ea = es[a];
            const auto& eb = es[b];
            if (ea.timestamp != eb.timestamp) return ea.timestamp > eb.timestamp; // newest first
            if (ea.score != eb.score) return ea.score > eb.score;
            return ea.name < eb.name;
        });

        const int count = std::min<int>(n, static_cast<int>(idx.size()));
        game.pushSystemMessage("RECENT RUNS (NEWEST FIRST):");
        for (int i = 0; i < count; ++i) {
            const auto& e = es[idx[static_cast<size_t>(i)]];

            const std::string depthTag = (e.branch == 0) ? std::string("CAMP")
                : (e.branch == 1) ? ("D" + std::to_string(e.depth))
                                : ("B" + std::to_string(static_cast<int>(e.branch)) + "D" + std::to_string(e.depth));

            std::ostringstream oss;
            oss << "#" << (i + 1) << " ";
            oss << (e.timestamp.empty() ? "(no timestamp)" : e.timestamp) << " ";
            oss << (e.name.empty() ? "PLAYER" : e.name);
            if (!e.playerClass.empty()) {
                PlayerClass pc = PlayerClass::Adventurer;
                if (parsePlayerClass(e.playerClass, pc)) {
                    oss << " (" << playerClassDisplayName(pc) << ")";
                } else {
                    oss << " (" << e.playerClass << ")";
                }
            }
            oss << " ";
            oss << (e.won ? "WIN" : "DEAD") << " ";
            oss << "S" << e.score << " " << depthTag << " T" << e.turns << " K" << e.kills;
            oss << " SEED" << e.seed;

            if (!e.slot.empty() && e.slot != "default") oss << " [" << e.slot << "]";
            if (!e.cause.empty()) oss << " " << e.cause;
            if (!e.gameVersion.empty()) oss << " V" << e.gameVersion;

            game.pushSystemMessage(oss.str());
        }
        return;
    }

    if (cmd == "exportlog" || cmd == "exportmap" || cmd == "export" || cmd == "exportall" || cmd == "dump") {
        namespace fs = std::filesystem;

        const fs::path baseDir = exportBaseDir(game);
        const std::string ts = timestampForFilename();
        const std::string argName = (toks.size() > 1) ? toks[1] : std::string();

        auto branchTag = [](DungeonBranch b) -> std::string {
            if (b == DungeonBranch::Camp) return "camp";
            if (b == DungeonBranch::Main) return "main";
            return "b" + std::to_string(static_cast<int>(b));
        };
        const std::string locTag = branchTag(game.branch()) + "_d" + std::to_string(game.depth());

        if (cmd == "exportlog") {
            const fs::path outPath = argName.empty() ? (baseDir / ("procrogue_log_" + locTag + "_" + ts + ".txt")) : (baseDir / fs::path(argName));
            if (!exportRunLogToFile(game, outPath)) {
                game.pushSystemMessage("FAILED TO EXPORT LOG.");
            } else {
                game.pushSystemMessage("EXPORTED LOG: " + outPath.string());
            }
            return;
        }

        if (cmd == "exportmap") {
            const fs::path outPath = argName.empty() ? (baseDir / ("procrogue_map_" + locTag + "_" + ts + ".txt")) : (baseDir / fs::path(argName));
            if (!exportRunMapToFile(game, outPath)) {
                game.pushSystemMessage("FAILED TO EXPORT MAP.");
            } else {
                game.pushSystemMessage("EXPORTED MAP: " + outPath.string());
            }
            return;
        }

        if (cmd == "dump") {
            const fs::path outPath = argName.empty() ? (baseDir / ("procrogue_dump_" + locTag + "_" + ts + ".txt")) : (baseDir / fs::path(argName));
            const auto res = exportRunDumpToFile(game, outPath);
            if (!res.first) {
                game.pushSystemMessage("FAILED TO EXPORT DUMP.");
            } else if (!res.second) {
                game.pushSystemMessage("EXPORTED DUMP (MAP MAY BE MISSING): " + outPath.string());
            } else {
                game.pushSystemMessage("EXPORTED DUMP: " + outPath.string());
            }
            return;
        }

        if (cmd == "exportall") {
            // Optional: #exportall [prefix]
            fs::path prefix = argName.empty() ? fs::path("procrogue_" + locTag + "_" + ts) : fs::path(argName);

            fs::path dir = baseDir;
            if (!prefix.parent_path().empty()) {
                dir = baseDir / prefix.parent_path();
                std::error_code ec;
                fs::create_directories(dir, ec);
            }

            const std::string stem = prefix.stem().string().empty() ? prefix.filename().string() : prefix.stem().string();

            const fs::path logPath  = dir / (stem + "_log.txt");
            const fs::path mapPath  = dir / (stem + "_map.txt");
            const fs::path dumpPath = dir / (stem + "_dump.txt");

            const bool okLog = exportRunLogToFile(game, logPath);
            const bool okMap = exportRunMapToFile(game, mapPath);
            const auto dumpRes = exportRunDumpToFile(game, dumpPath);

            if (okLog) game.pushSystemMessage("EXPORTED LOG: " + logPath.string());
            if (okMap) game.pushSystemMessage("EXPORTED MAP: " + mapPath.string());
            if (dumpRes.first) {
                if (!dumpRes.second) game.pushSystemMessage("EXPORTED DUMP (MAP MAY BE MISSING): " + dumpPath.string());
                else game.pushSystemMessage("EXPORTED DUMP: " + dumpPath.string());
            }

            if (!okLog || !okMap || !dumpRes.first) {
                game.pushSystemMessage("EXPORTALL COMPLETED WITH ERRORS.");
            }
            return;
        }

        // export: do both
        const fs::path logPath = baseDir / ("procrogue_log_" + locTag + "_" + ts + ".txt");
        const fs::path mapPath = baseDir / ("procrogue_map_" + locTag + "_" + ts + ".txt");

        const bool okLog = exportRunLogToFile(game, logPath);
        const bool okMap = exportRunMapToFile(game, mapPath);

        if (okLog) game.pushSystemMessage("EXPORTED LOG: " + logPath.string());
        if (okMap) game.pushSystemMessage("EXPORTED MAP: " + mapPath.string());

        if (!okLog || !okMap) {
            game.pushSystemMessage("EXPORT COMPLETED WITH ERRORS.");
        }
        return;
    }

    if (cmd == "sprites3d") {
        if (toks.size() > 1) {
            const std::string v = toLower(toks[1]);
            if (v == "on" || v == "true" || v == "1") {
                game.setVoxelSpritesEnabled(true);
                game.markSettingsDirty();
                game.pushSystemMessage("3D SPRITES: ON");
                return;
            }
            if (v == "off" || v == "false" || v == "0") {
                game.setVoxelSpritesEnabled(false);
                game.markSettingsDirty();
                game.pushSystemMessage("3D SPRITES: OFF");
                return;
            }
        }
        game.pushSystemMessage(std::string("3D SPRITES: ") + (game.voxelSpritesEnabled() ? "ON" : "OFF"));
        return;
    }

    if (cmd == "isoraytrace" || cmd == "iso_raytrace" || cmd == "iso_ray" || cmd == "isovoxelray") {
        if (toks.size() <= 1) {
            game.pushSystemMessage(std::string("ISO VOXEL RAYTRACE: ") + (game.isoVoxelRaytraceEnabled() ? "ON" : "OFF"));
            game.pushSystemMessage("USAGE: #isoraytrace on/off/toggle");
            return;
        }

        const std::string v = toLower(toks[1]);
        if (v == "on" || v == "true" || v == "1") {
            game.setIsoVoxelRaytraceEnabled(true);
            game.markSettingsDirty();
            game.pushSystemMessage("ISO VOXEL RAYTRACE: ON");
            return;
        }
        if (v == "off" || v == "false" || v == "0") {
            game.setIsoVoxelRaytraceEnabled(false);
            game.markSettingsDirty();
            game.pushSystemMessage("ISO VOXEL RAYTRACE: OFF");
            return;
        }
        if (v == "toggle" || v == "t") {
            game.setIsoVoxelRaytraceEnabled(!game.isoVoxelRaytraceEnabled());
            game.markSettingsDirty();
            game.pushSystemMessage(std::string("ISO VOXEL RAYTRACE: ") + (game.isoVoxelRaytraceEnabled() ? "ON" : "OFF"));
            return;
        }

        game.pushSystemMessage("USAGE: #isoraytrace on/off/toggle");
        return;
    }



    if (cmd == "isoterrainvox" || cmd == "isoblocks" || cmd == "iso_blocks" || cmd == "iso_terrain_voxels") {
        if (toks.size() <= 1) {
            game.pushSystemMessage(std::string("ISO TERRAIN VOXELS: ") + (game.isoTerrainVoxelBlocksEnabled() ? "ON" : "OFF"));
            game.pushSystemMessage("USAGE: #isoterrainvox on/off/toggle");
            return;
        }

        const std::string v = toLower(toks[1]);
        if (v == "on" || v == "true" || v == "1") {
            game.setIsoTerrainVoxelBlocksEnabled(true);
            game.markSettingsDirty();
            game.pushSystemMessage("ISO TERRAIN VOXELS: ON");
            return;
        }
        if (v == "off" || v == "false" || v == "0") {
            game.setIsoTerrainVoxelBlocksEnabled(false);
            game.markSettingsDirty();
            game.pushSystemMessage("ISO TERRAIN VOXELS: OFF");
            return;
        }
        if (v == "toggle" || v == "t") {
            game.setIsoTerrainVoxelBlocksEnabled(!game.isoTerrainVoxelBlocksEnabled());
            game.markSettingsDirty();
            game.pushSystemMessage(std::string("ISO TERRAIN VOXELS: ") + (game.isoTerrainVoxelBlocksEnabled() ? "ON" : "OFF"));
            return;
        }

        game.pushSystemMessage("USAGE: #isoterrainvox on/off/toggle");
        return;
    }

    if (cmd == "isocutaway" || cmd == "iso_cutaway" || cmd == "cutaway") {
        if (toks.size() <= 1) {
            game.pushSystemMessage(std::string("ISO CUTAWAY: ") + (game.isoCutawayEnabled() ? "ON" : "OFF"));
            game.pushSystemMessage("USAGE: #isocutaway on/off/toggle");
            return;
        }

        const std::string v = toLower(toks[1]);
        if (v == "on" || v == "true" || v == "1") {
            game.setIsoCutawayEnabled(true);
            game.markSettingsDirty();
            game.pushSystemMessage("ISO CUTAWAY: ON");
            return;
        }
        if (v == "off" || v == "false" || v == "0") {
            game.setIsoCutawayEnabled(false);
            game.markSettingsDirty();
            game.pushSystemMessage("ISO CUTAWAY: OFF");
            return;
        }
        if (v == "toggle" || v == "t") {
            game.setIsoCutawayEnabled(!game.isoCutawayEnabled());
            game.markSettingsDirty();
            game.pushSystemMessage(std::string("ISO CUTAWAY: ") + (game.isoCutawayEnabled() ? "ON" : "OFF"));
            return;
        }

        game.pushSystemMessage("USAGE: #isocutaway on/off/toggle");
        return;
    }

    if (cmd == "palette" || cmd == "pal") {
        if (toks.size() <= 1) {
            game.pushSystemMessage(std::string("PROC PALETTE: ") + (game.procPaletteEnabled() ? "ON" : "OFF") +
                                   " | STRENGTH " + std::to_string(game.procPaletteStrength()) +
                                   " | HUE " + std::to_string(game.procPaletteHueDeg()) +
                                   " | SAT " + std::to_string(game.procPaletteSaturationPct()) +
                                   " | BRIGHT " + std::to_string(game.procPaletteBrightnessPct()) +
                                   " | SPATIAL " + std::to_string(game.procPaletteSpatialStrength()));
            game.pushSystemMessage("USAGE: #palette on/off/toggle");
            game.pushSystemMessage("       #palette strength <0..100>");
            game.pushSystemMessage("       #palette hue <deg -45..45>");
            game.pushSystemMessage("       #palette sat <pct -80..80>");
            game.pushSystemMessage("       #palette bright <pct -60..60>");
            game.pushSystemMessage("       #palette spatial <0..100>");
            game.pushSystemMessage("       #palette reset");
            return;
        }

        const std::string v = toLower(toks[1]);
        if (v == "on" || v == "true" || v == "1") {
            game.setProcPaletteEnabled(true);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE: ON");
            return;
        }
        if (v == "off" || v == "false" || v == "0") {
            game.setProcPaletteEnabled(false);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE: OFF");
            return;
        }
        if (v == "toggle" || v == "t") {
            game.setProcPaletteEnabled(!game.procPaletteEnabled());
            game.markSettingsDirty();
            game.pushSystemMessage(std::string("PROC PALETTE: ") + (game.procPaletteEnabled() ? "ON" : "OFF"));
            return;
        }
        if (v == "strength" || v == "s") {
            if (toks.size() < 3) {
                game.pushSystemMessage("USAGE: #palette strength <0..100>");
                return;
            }
            int pct = 0;
            if (!parseInt(toks[2], pct)) {
                game.pushSystemMessage("INVALID STRENGTH (EXPECTED INTEGER 0..100).");
                return;
            }
            pct = std::clamp(pct, 0, 100);
            game.setProcPaletteStrength(pct);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE STRENGTH: " + std::to_string(pct));
            return;
        }

        if (v == "hue" || v == "h") {
            if (toks.size() < 3) {
                game.pushSystemMessage("USAGE: #palette hue <deg -45..45>");
                return;
            }
            int deg = 0;
            if (!parseInt(toks[2], deg)) {
                game.pushSystemMessage("INVALID HUE (EXPECTED INTEGER -45..45)." );
                return;
            }
            deg = std::clamp(deg, -45, 45);
            game.setProcPaletteHueDeg(deg);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE HUE: " + std::to_string(deg));
            return;
        }

        if (v == "sat" || v == "saturation") {
            if (toks.size() < 3) {
                game.pushSystemMessage("USAGE: #palette sat <pct -80..80>");
                return;
            }
            int pct = 0;
            if (!parseInt(toks[2], pct)) {
                game.pushSystemMessage("INVALID SATURATION (EXPECTED INTEGER -80..80)." );
                return;
            }
            pct = std::clamp(pct, -80, 80);
            game.setProcPaletteSaturationPct(pct);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE SAT: " + std::to_string(pct));
            return;
        }

        if (v == "bright" || v == "brightness" || v == "val" || v == "value") {
            if (toks.size() < 3) {
                game.pushSystemMessage("USAGE: #palette bright <pct -60..60>");
                return;
            }
            int pct = 0;
            if (!parseInt(toks[2], pct)) {
                game.pushSystemMessage("INVALID BRIGHTNESS (EXPECTED INTEGER -60..60)." );
                return;
            }
            pct = std::clamp(pct, -60, 60);
            game.setProcPaletteBrightnessPct(pct);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE BRIGHT: " + std::to_string(pct));
            return;
        }

        if (v == "spatial" || v == "field") {
            if (toks.size() < 3) {
                game.pushSystemMessage("USAGE: #palette spatial <0..100>");
                return;
            }
            int pct = 0;
            if (!parseInt(toks[2], pct)) {
                game.pushSystemMessage("INVALID SPATIAL (EXPECTED INTEGER 0..100)." );
                return;
            }
            pct = std::clamp(pct, 0, 100);
            game.setProcPaletteSpatialStrength(pct);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE SPATIAL: " + std::to_string(pct));
            return;
        }

        if (v == "reset" || v == "default" || v == "defaults") {
            game.setProcPaletteStrength(70);
            game.setProcPaletteHueDeg(0);
            game.setProcPaletteSaturationPct(0);
            game.setProcPaletteBrightnessPct(0);
            game.setProcPaletteSpatialStrength(35);
            game.markSettingsDirty();
            game.pushSystemMessage("PROC PALETTE: RESET (STRENGTH 70 | HUE 0 | SAT 0 | BRIGHT 0 | SPATIAL 35)");
            return;
        }

        game.pushSystemMessage("USAGE: #palette on/off/toggle");
        game.pushSystemMessage("       #palette strength <0..100>");
        game.pushSystemMessage("       #palette hue <deg -45..45>");
        game.pushSystemMessage("       #palette sat <pct -80..80>");
        game.pushSystemMessage("       #palette bright <pct -60..60>");
        game.pushSystemMessage("       #palette spatial <0..100>");
        game.pushSystemMessage("       #palette reset");
        return;
    }


    if (cmd == "bones") {
        if (toks.size() > 1) {
            const std::string v = toLower(toks[1]);
            if (v == "on" || v == "true" || v == "1") {
                game.setBonesEnabled(true);
                game.markSettingsDirty();
                game.pushSystemMessage("BONES FILES: ON");
                return;
            }
            if (v == "off" || v == "false" || v == "0") {
                game.setBonesEnabled(false);
                game.markSettingsDirty();
                game.pushSystemMessage("BONES FILES: OFF");
                return;
            }
        }

        game.pushSystemMessage(std::string("BONES FILES: ") + (game.bonesEnabled() ? "ON" : "OFF"));
        game.pushSystemMessage("USAGE: #bones on|off");
        return;
    }

    if (cmd == "mortem") {
        if (toks.size() > 1) {
            const std::string v = toLower(toks[1]);
            if (v == "on" || v == "true" || v == "1") {
                game.setAutoMortemEnabled(true);
                game.markSettingsDirty();
                game.pushSystemMessage("AUTO MORTEM: ON");
                return;
            }
            if (v == "off" || v == "false" || v == "0") {
                game.setAutoMortemEnabled(false);
                game.markSettingsDirty();
                game.pushSystemMessage("AUTO MORTEM: OFF");
                return;
            }
            if (v != "now") {
                game.pushSystemMessage("USAGE: mortem [now|on|off]");
                return;
            }
        }

        namespace fs = std::filesystem;
        const fs::path dir = exportBaseDir(game);
        const std::string ts = timestampForFilename();
        const fs::path outPath = dir / ("procrogue_mortem_" + ts + ".txt");

        const auto res = exportRunDumpToFile(game, outPath);
        if (!res.first) {
            game.pushSystemMessage("FAILED TO EXPORT MORTEM.");
        } else {
            game.pushSystemMessage("EXPORTED MORTEM: " + outPath.string());
        }
        return;
    }

    if (cmd == "autopickup") {
        const std::string v = arg(1);
        if (v.empty()) {
            game.handleAction(Action::ToggleAutoPickup);
            return;
        }

        AutoPickupMode m = game.autoPickupMode();
        if (v == "off" || v == "0" || v == "false") m = AutoPickupMode::Off;
        else if (v == "gold") m = AutoPickupMode::Gold;
        else if (v == "smart") m = AutoPickupMode::Smart;
        else if (v == "all") m = AutoPickupMode::All;
        else {
            game.pushSystemMessage("USAGE: autopickup [off|gold|smart|all]");
            return;
        }

        game.setAutoPickupMode(m);
        game.markSettingsDirty();

        const char* label = (m == AutoPickupMode::Off)   ? "OFF"
                            : (m == AutoPickupMode::Gold)  ? "GOLD"
                            : (m == AutoPickupMode::Smart) ? "SMART"
                                                           : "ALL";
        game.pushSystemMessage(std::string("AUTO-PICKUP: ") + label);
        return;
    }

    if (cmd == "autosave") {
        const std::string v = arg(1);
        if (v.empty()) {
            game.pushSystemMessage("AUTOSAVE EVERY: " + std::to_string(game.autosaveEveryTurns()) + " TURNS");
            return;
        }
        try {
            int n = std::stoi(v);
            n = clampi(n, 0, 5000);
            game.setAutosaveEveryTurns(n);
            game.markSettingsDirty();
            game.pushSystemMessage("AUTOSAVE EVERY: " + std::to_string(n) + " TURNS");
        } catch (...) {
            game.pushSystemMessage("USAGE: autosave <turns>");
        }
        return;
    }

    if (cmd == "stepdelay") {
        const std::string v = arg(1);
        if (v.empty()) {
            game.pushSystemMessage("AUTO-STEP DELAY: " + std::to_string(game.autoStepDelayMs()) + " MS");
            return;
        }
        try {
            int ms = std::stoi(v);
            ms = clampi(ms, 10, 500);
            game.setAutoStepDelayMs(ms);
            game.markSettingsDirty();
            game.pushSystemMessage("AUTO-STEP DELAY: " + std::to_string(ms) + " MS");
        } catch (...) {
            game.pushSystemMessage("USAGE: stepdelay <ms>");
        }
        return;
    }

    if (cmd == "identify") {
        const std::string v = arg(1);
        if (v.empty()) {
            game.pushSystemMessage(std::string("IDENTIFY: ") + (game.identificationEnabled() ? "ON" : "OFF"));
            return;
        }

        bool on = game.identificationEnabled();
        if (v == "on" || v == "true" || v == "1") on = true;
        else if (v == "off" || v == "false" || v == "0") on = false;
        else {
            game.pushSystemMessage("USAGE: identify [on|off]");
            return;
        }

        game.setIdentificationEnabled(on);
        game.markSettingsDirty();
        game.pushSystemMessage(std::string("IDENTIFY: ") + (on ? "ON" : "OFF"));
        return;
    }

    if (cmd == "call") {
        // NetHack-style "call" labels for unidentified appearances.
        //
        // Usage:
        //   #call <label...>          (uses LOOK cursor if active, else inventory selection, else item underfoot)
        //   #call <x> <y> <label...>  (explicit ground tile coordinates)
        //   #label ...                (alias)
        //
        // Clearing:
        //   #call clear|none|off|-
        //
        // Notes are attached to the *appearance* (per-run randomized) via the underlying ItemKind.

        auto joinFrom = [&](size_t start) -> std::string {
            std::string out;
            for (size_t i = start; i < toks.size(); ++i) {
                if (i > start) out.push_back(' ');
                out += toks[i];
            }
            return out;
        };

        ItemKind target = static_cast<ItemKind>(0);
        bool haveTarget = false;

        auto setTargetFromKind = [&](ItemKind k) {
            if (!isIdentifiableKind(k)) return false;
            target = k;
            haveTarget = true;
            return true;
        };

        bool usePos = false;
        Vec2i pos{0, 0};
        size_t labelStart = 1;

        const bool looking = game.isLooking();

        // Explicit coords: call x y label...
        if (!looking && toks.size() >= 4) {
            try {
                const int x = std::stoi(toks[1]);
                const int y = std::stoi(toks[2]);
                pos = Vec2i{x, y};
                usePos = true;
                labelStart = 3;
            } catch (...) {
                // Not coords; fall through.
            }
        }

        // LOOK cursor has priority over other contexts.
        if (looking) {
            pos = game.lookCursor();
            usePos = true;
            labelStart = 1;
        }

        if (usePos) {
            // Prefer an *unidentified* identifiable item (most useful), otherwise any identifiable item.
            ItemKind fallback = static_cast<ItemKind>(0);
            bool haveFallback = false;

            for (const GroundItem& gi : game.groundItems()) {
                if (gi.pos != pos) continue;
                const ItemKind k = gi.item.kind;
                if (!isIdentifiableKind(k)) continue;

                if (!game.discoveriesIsIdentified(k)) {
                    setTargetFromKind(k);
                    break;
                }
                if (!haveFallback) {
                    fallback = k;
                    haveFallback = true;
                }
            }

            if (!haveTarget && haveFallback) setTargetFromKind(fallback);
        } else if (game.isInventoryOpen()) {
            const int sel = game.inventorySelection();
            const auto& inv = game.inventory();
            if (sel >= 0 && sel < static_cast<int>(inv.size())) {
                setTargetFromKind(inv[static_cast<size_t>(sel)].kind);
            }
        } else {
            // Default: first identifiable ground item under the player.
            pos = game.player().pos;
            for (const GroundItem& gi : game.groundItems()) {
                if (gi.pos != pos) continue;
                if (setTargetFromKind(gi.item.kind)) break;
            }
        }

        if (!haveTarget) {
            game.pushSystemMessage("CALL: NO POTION/SCROLL/RING/WAND IN CONTEXT (TRY LOOK CURSOR OR INVENTORY).");
            return;
        }

        std::string label = trim(joinFrom(labelStart));
        const std::string labelLow = toLower(label);

        if (label.empty()) {
            if (game.hasItemCallLabel(target)) {
                game.pushSystemMessage("CALL: " + game.discoveryAppearanceLabel(target));
            } else {
                game.pushSystemMessage("CALL: NO LABEL SET. USAGE: #call <label...>  (OR #call clear)");
            }
            return;
        }

        const bool wantClear =
            (labelLow == "clear" || labelLow == "none" || labelLow == "off" || labelLow == "-" || labelLow == "reset");

        if (wantClear) {
            if (game.clearItemCallLabel(target)) {
                game.pushSystemMessage("CALL CLEARED: " + game.discoveryAppearanceLabel(target));
            } else {
                game.pushSystemMessage("CALL: NO LABEL TO CLEAR FOR " + game.discoveryAppearanceLabel(target));
            }
            return;
        }

        if (game.setItemCallLabel(target, label)) {
            game.pushSystemMessage("CALLED: " + game.discoveryAppearanceLabel(target));
        } else {
            // Either unchanged or sanitized to empty (which clears).
            if (game.hasItemCallLabel(target)) {
                game.pushSystemMessage("CALLED: " + game.discoveryAppearanceLabel(target));
            } else {
                game.pushSystemMessage("CALL: CLEARED.");
            }
        }

        return;
    }

    if (cmd == "encumbrance") {
        const std::string v = arg(1);
        if (v.empty()) {
            game.pushSystemMessage(std::string("ENCUMBRANCE: ") + (game.encumbranceEnabled() ? "ON" : "OFF"));
            return;
        }

        bool on = game.encumbranceEnabled();
        if (v == "on" || v == "true" || v == "1") on = true;
        else if (v == "off" || v == "false" || v == "0") on = false;
        else {
            game.pushSystemMessage("USAGE: encumbrance [on|off]");
            return;
        }

        game.setEncumbranceEnabled(on);
        game.markSettingsDirty();
        game.pushSystemMessage(std::string("ENCUMBRANCE: ") + (on ? "ON" : "OFF"));
        return;
    }



    if (cmd == "pet") {
        const std::string v = arg(1);
        if (v.empty() || v == "status") {
            std::vector<const Entity*> comps;
            comps.reserve(8);

            int carrying = 0;
            int packMules = 0;

            AllyOrder order = AllyOrder::Follow;
            bool mixed = false;
            bool first = true;

            for (const auto& e : game.entities()) {
                if (e.id == game.playerId()) continue;
                if (e.hp <= 0) continue;
                if (!e.friendly) continue;

                comps.push_back(&e);

                if (e.stolenGold > 0 || (e.pocketConsumable.id != 0 && e.pocketConsumable.count > 0)) ++carrying;

                if (first) {
                    order = e.allyOrder;
                    first = false;
                } else if (e.allyOrder != order) {
                    mixed = true;
                }

                if (petgen::petHasTrait(e.procAffixMask, petgen::PetTrait::PackMule)) {
                    ++packMules;
                }
            }

            // Capture-sphere pals currently held in inventory.
            // (This is the closest analogue to a "party/box" in Pokemon/Palworld.)
            std::vector<const Item*> captured;
            captured.reserve(16);
            for (const auto& it : game.inventory()) {
                if (isCaptureSphereFullKind(it.kind)) {
                    captured.push_back(&it);
                }
            }

            const int n = static_cast<int>(comps.size());
            const int storedN = static_cast<int>(captured.size());

            auto sphereFor = [&](const Entity& c) -> const Item* {
                for (const Item* it : captured) {
                    if (it->enchant != static_cast<int>(c.kind)) continue;
                    if (it->spriteSeed != c.spriteSeed) continue;
                    return it;
                }
                return nullptr;
            };

            if (n <= 0) {
                if (storedN <= 0) {
                    game.pushSystemMessage("NO COMPANIONS.");
                    return;
                }

                std::string msg = "NO ACTIVE COMPANIONS. STORED PALS: " + std::to_string(storedN);
                msg += " | TIP: USE A FULL SPHERE TO RELEASE ONE.";
                game.pushSystemMessage(msg);

                constexpr int MAX_STORED_LIST = 8;
                const int showStored = std::min(storedN, MAX_STORED_LIST);

                for (int i = 0; i < showStored; ++i) {
                    const Item& it = *captured[i];
                    const EntityKind k = static_cast<EntityKind>(it.enchant);

                    const int bond = clampi(captureSphereBondFromCharges(it.charges), 0, 99);
                    const int lv = clampi(captureSpherePetLevelOrDefault(it.charges), 1, captureSpherePetLevelCap());
                    const int hpPct = clampi(captureSphereHpPctFromCharges(it.charges), 0, 100);

                    std::string row = "S" + std::to_string(i + 1) + ") ";
                    row += petgen::petGivenName(it.spriteSeed);
                    row += " THE ";
                    row += kindName(k);
                    row += " | LV " + std::to_string(lv);
                    row += " | BOND " + std::to_string(bond);
                    row += " | HP " + std::to_string(hpPct) + "%";

                    game.pushSystemMessage(row);
                }

                if (storedN > MAX_STORED_LIST) {
                    game.pushSystemMessage("... +" + std::to_string(storedN - MAX_STORED_LIST) + " MORE STORED PAL(S)." );
                }

                return;
            }

            std::string o = mixed ? "MIXED" :
                (order == AllyOrder::Follow ? "FOLLOW" :
                 order == AllyOrder::Stay ? "STAY" :
                 order == AllyOrder::Fetch ? "FETCH" : "GUARD");

            std::string msg = "COMPANIONS: " + std::to_string(n) + " | ORDER: " + o;
            if (storedN > 0) msg += " | STORED PALS: " + std::to_string(storedN);
            if (carrying > 0) msg += " | CARRYING: " + std::to_string(carrying);
            if (packMules > 0) msg += " | PACK MULES: " + std::to_string(packMules);
            msg += " | USAGE: pet <follow|stay|fetch|guard>";
            game.pushSystemMessage(msg);

            // Detailed list (avoid spam if you have a huge menagerie).
            constexpr int MAX_LIST = 6;
            const int show = std::min(n, MAX_LIST);

            for (int i = 0; i < show; ++i) {
                const Entity& c = *comps[i];
                std::string row = std::to_string(i + 1) + ") ";
                row += petGivenNameFor(c);
                row += " THE ";
                row += kindName(c.kind);

                const std::string traits = petgen::petTraitList(c.procAffixMask);
                if (!traits.empty()) row += " | TRAITS: " + traits;

                if (const Item* sph = sphereFor(c)) {
                    const int bond = clampi(captureSphereBondFromCharges(sph->charges), 0, 99);
                    const int lv = clampi(captureSpherePetLevelOrDefault(sph->charges), 1, captureSpherePetLevelCap());
                    row += " | LV " + std::to_string(lv);
                    row += " | BOND " + std::to_string(bond);
                }

                row += " | HP " + std::to_string(c.hp) + "/" + std::to_string(c.hpMax);

                row += " | ORDER: ";
                row += (c.allyOrder == AllyOrder::Follow ? "FOLLOW" :
                         c.allyOrder == AllyOrder::Stay ? "STAY" :
                         c.allyOrder == AllyOrder::Fetch ? "FETCH" : "GUARD");

                if (c.stolenGold > 0) row += " | " + std::to_string(c.stolenGold) + "G";
                if (c.pocketConsumable.id != 0 && c.pocketConsumable.count > 0) {
                    row += " | PACK: " + game.displayItemName(c.pocketConsumable);
                }

                game.pushSystemMessage(row);
            }

            if (n > MAX_LIST) {
                game.pushSystemMessage("... +" + std::to_string(n - MAX_LIST) + " MORE COMPANION(S)." );
            }

            if (storedN > 0) {
                game.pushSystemMessage("CAPTURED PALS (SPHERES): " + std::to_string(storedN));

                constexpr int MAX_STORED_LIST = 8;
                const int showStored = std::min(storedN, MAX_STORED_LIST);

                for (int i = 0; i < showStored; ++i) {
                    const Item& it = *captured[i];
                    const EntityKind k = static_cast<EntityKind>(it.enchant);

                    const int bond = clampi(captureSphereBondFromCharges(it.charges), 0, 99);
                    const int lv = clampi(captureSpherePetLevelOrDefault(it.charges), 1, captureSpherePetLevelCap());
                    const int hpPct = clampi(captureSphereHpPctFromCharges(it.charges), 0, 100);

                    bool outNow = false;
                    for (const Entity* e : comps) {
                        if (e && e->kind == k && e->spriteSeed == it.spriteSeed) { outNow = true; break; }
                    }

                    std::string row = "S" + std::to_string(i + 1) + ") ";
                    row += petgen::petGivenName(it.spriteSeed);
                    row += " THE ";
                    row += kindName(k);
                    if (outNow) row += " | OUT";
                    row += " | LV " + std::to_string(lv);
                    row += " | BOND " + std::to_string(bond);
                    row += " | HP " + std::to_string(hpPct) + "%";

                    game.pushSystemMessage(row);
                }

                if (storedN > MAX_STORED_LIST) {
                    game.pushSystemMessage("... +" + std::to_string(storedN - MAX_STORED_LIST) + " MORE STORED PAL(S)." );
                }
            }

            return;
        }


        AllyOrder o = AllyOrder::Follow;
        if (v == "follow" || v == "f") o = AllyOrder::Follow;
        else if (v == "stay" || v == "hold" || v == "s") o = AllyOrder::Stay;
        else if (v == "fetch") o = AllyOrder::Fetch;
        else if (v == "guard" || v == "g") o = AllyOrder::Guard;
        else {
            game.pushSystemMessage("USAGE: pet <follow|stay|fetch|guard>");
            return;
        }

        game.setAlliesOrder(o, true);
        return;
    }

    if (cmd == "tame") {
        game.tame();
        return;
    }

    if (cmd == "wind") {
        const Vec2i w = game.windDir();
        const int ws = game.windStrength();

        if (ws <= 0 || (w.x == 0 && w.y == 0)) {
            game.pushSystemMessage("WIND: CALM.");
            return;
        }

        const char* dir =
            (w.x > 0) ? "EAST" :
            (w.x < 0) ? "WEST" :
            (w.y > 0) ? "SOUTH" : "NORTH";

        const char* mag =
            (ws == 1) ? "BREEZE" :
            (ws == 2) ? "DRAFT" : "GALE";

        std::string msg = std::string("WIND: ") + dir + " (" + mag + ").";
        game.pushSystemMessage(msg);
        return;
    }

    if (cmd == "listen") {
        game.listen();
        return;
    }

    if (cmd == "throwvoice") {
        // Usage:
        //   #throwvoice X Y
        //   #throwvoice            (targets LOOK cursor)
        const bool wasLooking = game.isLooking();

        Vec2i pos = { -1, -1 };
        if (wasLooking) {
            pos = game.lookCursor();
        } else if (toks.size() >= 3) {
            try {
                const int x = std::stoi(toks[1], nullptr, 0);
                const int y = std::stoi(toks[2], nullptr, 0);
                pos = { x, y };
            } catch (...) {
                // fallthrough to usage
            }
        }

        if (pos.x < 0 || pos.y < 0) {
            game.pushSystemMessage("USAGE: throwvoice X Y");
            game.pushSystemMessage("TIP: open LOOK (:) and move the cursor, then #throwvoice.");
            return;
        }

        (void)game.throwVoiceAt(pos);
        return;
    }

    if (cmd == "shout" || cmd == "yell") {
        game.shout();
        return;
    }

    if (cmd == "whistle") {
        game.whistle();
        return;
    }

    // Should be unreachable because we validated against the command list, but keep a fallback.
    game.pushSystemMessage("UNHANDLED COMMAND: " + cmd);
}


const char* kindName(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return "YOU";
        case EntityKind::Goblin: return "GOBLIN";
        case EntityKind::Orc: return "ORC";
        case EntityKind::Bat: return "BAT";
        case EntityKind::Slime: return "SLIME";
        case EntityKind::SkeletonArcher: return "SKELETON";
        case EntityKind::KoboldSlinger: return "KOBOLD";
        case EntityKind::Wolf: return "WOLF";
        case EntityKind::Dog: return "DOG";
        case EntityKind::Ghost: return "GHOST";
        case EntityKind::Leprechaun: return "LEPRECHAUN";
        case EntityKind::Zombie: return "ZOMBIE";
        case EntityKind::Troll: return "TROLL";
        case EntityKind::Wizard: return "WIZARD";
        case EntityKind::Snake: return "SNAKE";
        case EntityKind::Spider: return "SPIDER";
        case EntityKind::Ogre: return "OGRE";
        case EntityKind::Mimic: return "MIMIC";
        case EntityKind::Shopkeeper: return "SHOPKEEPER";
        case EntityKind::Minotaur: return "MINOTAUR";
        default: return "THING";
    }
}

static std::string petDisplayName(const Entity& e) {
    // Keep consistent with classic roguelike messaging (NAME THE KIND).
    std::string out = petGivenNameFor(e);
    out += " THE ";
    out += kindName(e.kind);
    return out;
}

// ------------------------------------------------------------
// Identification visuals (run-randomized appearances: potions / scrolls / rings / wands)
// ------------------------------------------------------------

constexpr const char* POTION_APPEARANCES[] = {
    "RUBY", "EMERALD", "SAPPHIRE", "AMBER", "TOPAZ", "ONYX", "PEARL", "IVORY",
    "AZURE", "VIOLET", "CRIMSON", "VERDANT", "SILVER", "GOLDEN", "SMOKE", "MURKY",
};

constexpr const char* SCROLL_APPEARANCES[] = {
    "ZELGO", "XANATH", "KERNOD", "ELBERR", "MAPIRO", "VORPAL", "KLAATU", "BARADA",
    "NIKTO", "RAGNAR", "YENDOR", "MORDOR", "AZATHO", "ALOHOM", "OROBO", "NYARLA",
};

constexpr const char* RING_APPEARANCES[] = {
    "COPPER", "BRASS", "STEEL", "SILVER", "GOLD", "PLATINUM", "IRON", "TIN",
    "OPAL", "ONYX", "JADE", "RUBY", "SAPPHIRE", "EMERALD", "TOPAZ", "GLASS",
};

constexpr const char* WAND_APPEARANCES[] = {
    "OAK", "BONE", "IVORY", "ASH", "EBONY", "PINE", "BAMBOO", "YEW",
    "MAPLE", "ELM", "BIRCH", "WILLOW", "CRYSTAL", "OBSIDIAN", "STONE", "COPPER",
};


// Fixed sets of identifiable kinds (append-only behavior is handled elsewhere).
constexpr ItemKind POTION_KINDS[] = {
    ItemKind::PotionHealing,
    ItemKind::PotionStrength,
    ItemKind::PotionAntidote,
    ItemKind::PotionRegeneration,
    ItemKind::PotionShielding,
    ItemKind::PotionHaste,
    ItemKind::PotionVision,
    ItemKind::PotionInvisibility,
    ItemKind::PotionClarity,
    ItemKind::PotionLevitation,
    ItemKind::PotionHallucination,
    ItemKind::PotionEnergy,
};

constexpr ItemKind SCROLL_KINDS[] = {
    ItemKind::ScrollTeleport,
    ItemKind::ScrollMapping,
    ItemKind::ScrollEnchantWeapon,
    ItemKind::ScrollEnchantArmor,
    ItemKind::ScrollIdentify,
    ItemKind::ScrollDetectTraps,
    ItemKind::ScrollDetectSecrets,
    ItemKind::ScrollKnock,
    ItemKind::ScrollRemoveCurse,
    ItemKind::ScrollConfusion,
    ItemKind::ScrollFear,
    ItemKind::ScrollEarth,
    ItemKind::ScrollTaming,
    ItemKind::ScrollEnchantRing,
};

constexpr ItemKind RING_KINDS[] = {
    ItemKind::RingMight,
    ItemKind::RingAgility,
    ItemKind::RingFocus,
    ItemKind::RingProtection,
    ItemKind::RingSearching,
    ItemKind::RingSustenance,
};

constexpr ItemKind WAND_KINDS[] = {
    ItemKind::WandSparks,
    ItemKind::WandDigging,
    ItemKind::WandFireball,
};


} // namespace

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
