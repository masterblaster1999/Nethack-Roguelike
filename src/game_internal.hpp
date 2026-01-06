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
#include "settings.hpp"
#include "grid_utils.hpp"
#include "pathfinding.hpp"
#include "slot_utils.hpp"
#include "shop.hpp"
#include "version.hpp"
#include <algorithm>
#include <numeric>
#include <cctype>
#include <cstdlib>
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
    f << "Depth: " << game.depth() << " (max " << game.maxDepthReached() << ")\n";
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
        f << "[" << k << "] [D" << m.depth << " T" << m.turn << "] " << m.text;
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
    f << "Seed: " << game.seed() << "  Depth: " << game.depth() << "  Turns: " << game.turns() << "\n";
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
    f << "Depth: " << game.depth() << " (max " << game.maxDepthReached() << ")\n";
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
        f << "  [D" << ms[i].depth << " T" << ms[i].turn << "] " << ms[i].text;
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

static std::vector<std::string> splitWS(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
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
        "throwvoice",
        "pet",
        "tame",
        "options",
        "preset",
        "sprites3d",
        "binds",
        "bind",
        "unbind",
        "reload",
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
        "encumbrance",
        "timers",
        "uitheme",
        "uipanels",
        "seed",
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
        "throwtorch",
        "augury",
        "pray",
        "pay",
    };
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
        ok &= updateIniKey(settingsPath, "bind_help", "f1, shift+slash");
        // Sneak: avoid 'n' (movement down-right in vi keys).
        ok &= updateIniKey(settingsPath, "bind_sneak", "shift+n");
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
        ok &= updateIniKey(settingsPath, "bind_help", "f1, shift+slash, h");
        ok &= updateIniKey(settingsPath, "bind_sneak", "n");
    }

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

    auto toks = splitWS(line);
    if (toks.empty()) return;

    std::string cmdIn = toLower(toks[0]);

    if (cmdIn == "?" || cmdIn == "commands") cmdIn = "help";
// Common aliases (kept out of the completion list so it stays short/stable).
if (cmdIn == "annotate" || cmdIn == "note") cmdIn = "mark";
else if (cmdIn == "unannotate" || cmdIn == "clearmark") cmdIn = "unmark";
else if (cmdIn == "notes" || cmdIn == "markers") cmdIn = "marks";
else if (cmdIn == "msghistory" || cmdIn == "message_history" || cmdIn == "msglog") cmdIn = "messages";
else if (cmdIn == "controls" || cmdIn == "keyset") cmdIn = "preset";
else if (cmdIn == "hear") cmdIn = "listen";
else if (cmdIn == "vent" || cmdIn == "ventriloquism" || cmdIn == "voice" || cmdIn == "decoy") cmdIn = "throwvoice";
else if (cmdIn == "divine" || cmdIn == "divination" || cmdIn == "omen" || cmdIn == "prophecy") cmdIn = "augury";

    std::vector<std::string> cmds = extendedCommandList();

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

    // Map an action name to the canonical settings key (bind_<action>). Accepts a few aliases.
    auto bindKeyForActionName = [&](const std::string& actionRaw, std::string& outKey) -> bool {
        std::string a = toLower(trim(actionRaw));
    
        // Allow users to pass 'bind_<action>' too.
        if (a.rfind("bind_", 0) == 0) a = a.substr(5);
        // Normalize separators.
        for (char& c : a) {
            if (c == '-') c = '_';
        }

        // Movement
        if (a == "up") { outKey = "bind_up"; return true; }
        if (a == "down") { outKey = "bind_down"; return true; }
        if (a == "left") { outKey = "bind_left"; return true; }
        if (a == "right") { outKey = "bind_right"; return true; }
        if (a == "up_left" || a == "upleft") { outKey = "bind_up_left"; return true; }
        if (a == "up_right" || a == "upright") { outKey = "bind_up_right"; return true; }
        if (a == "down_left" || a == "downleft") { outKey = "bind_down_left"; return true; }
        if (a == "down_right" || a == "downright") { outKey = "bind_down_right"; return true; }

        // Actions
        if (a == "confirm" || a == "ok") { outKey = "bind_confirm"; return true; }
        if (a == "cancel" || a == "escape" || a == "esc") { outKey = "bind_cancel"; return true; }
        if (a == "wait") { outKey = "bind_wait"; return true; }
        if (a == "rest") { outKey = "bind_rest"; return true; }
        if (a == "sneak" || a == "toggle_sneak" || a == "togglesneak") { outKey = "bind_sneak"; return true; }
        if (a == "pickup" || a == "pick_up" || a == "pick") { outKey = "bind_pickup"; return true; }
        if (a == "inventory" || a == "inv") { outKey = "bind_inventory"; return true; }
        if (a == "fire") { outKey = "bind_fire"; return true; }
        if (a == "search") { outKey = "bind_search"; return true; }
        if (a == "disarm") { outKey = "bind_disarm"; return true; }
        if (a == "close_door" || a == "closedoor" || a == "close") { outKey = "bind_close_door"; return true; }
        if (a == "lock_door" || a == "lockdoor" || a == "lock") { outKey = "bind_lock_door"; return true; }
        if (a == "kick") { outKey = "bind_kick"; return true; }
        if (a == "dig" || a == "tunnel") { outKey = "bind_dig"; return true; }
        if (a == "look") { outKey = "bind_look"; return true; }
        if (a == "stairs_up" || a == "stairsup") { outKey = "bind_stairs_up"; return true; }
        if (a == "stairs_down" || a == "stairsdown") { outKey = "bind_stairs_down"; return true; }
        if (a == "auto_explore" || a == "autoexplore") { outKey = "bind_auto_explore"; return true; }
        if (a == "toggle_auto_pickup" || a == "toggleautopickup" || a == "autopickup") { outKey = "bind_toggle_auto_pickup"; return true; }

        // Inventory-specific
        if (a == "equip") { outKey = "bind_equip"; return true; }
        if (a == "use") { outKey = "bind_use"; return true; }
        if (a == "drop") { outKey = "bind_drop"; return true; }
        if (a == "drop_all" || a == "dropall") { outKey = "bind_drop_all"; return true; }
        if (a == "sort_inventory" || a == "sortinventory") { outKey = "bind_sort_inventory"; return true; }

        // UI / meta
        if (a == "help") { outKey = "bind_help"; return true; }
        if (a == "message_history" || a == "messagehistory" || a == "messages" || a == "msglog" || a == "msghistory") { outKey = "bind_message_history"; return true; }
        if (a == "options") { outKey = "bind_options"; return true; }
        if (a == "command" || a == "extcmd") { outKey = "bind_command"; return true; }
        if (a == "toggle_minimap" || a == "minimap") { outKey = "bind_toggle_minimap"; return true; }
        if (a == "toggle_stats" || a == "stats") { outKey = "bind_toggle_stats"; return true; }
        if (a == "fullscreen" || a == "toggle_fullscreen" || a == "togglefullscreen") { outKey = "bind_fullscreen"; return true; }
        if (a == "screenshot") { outKey = "bind_screenshot"; return true; }
        if (a == "save") { outKey = "bind_save"; return true; }
        if (a == "restart" || a == "newgame") { outKey = "bind_restart"; return true; }
        if (a == "load") { outKey = "bind_load"; return true; }
        if (a == "load_auto" || a == "loadauto") { outKey = "bind_load_auto"; return true; }
        if (a == "log_up" || a == "logup") { outKey = "bind_log_up"; return true; }
        if (a == "log_down" || a == "logdown") { outKey = "bind_log_down"; return true; }

        return false;
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
        game.pushSystemMessage("SLOTS: slot [name], save [slot], load [slot], loadauto [slot], saves");
        game.pushSystemMessage("EXPORT: exportlog/exportmap/export/exportall/dump");
        game.pushSystemMessage("MARKS: mark [note|danger|loot] <label> | unmark | marks | travel <index|label>");
        game.pushSystemMessage("ENGRAVE: engrave <text> (costs a turn; try 'ELBERETH' for a ward)");
        game.pushSystemMessage("SOUND: shout | whistle | listen | throwvoice [x y] (TIP: LOOK cursor works)");
        game.pushSystemMessage("SHRINES: pray [heal|cure|identify|bless|uncurse|recharge] (costs gold)");
        game.pushSystemMessage("AUGURY: augury (costs gold; shrine/camp only; hints can shift)");
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
        (void)game.loadFromFile(path);
        return;
    }
    if (cmd == "loadauto") {
        // Optional save slot: #loadauto <slot>
        const std::string slot = (toks.size() > 1) ? sanitizeSlotName(toks[1]) : std::string();
        const std::string path = slot.empty()
            ? game.defaultAutosavePath()
            : makeSlotPath(baseAutosavePathForSlots(game).string(), slot).string();
        (void)game.loadFromFile(path);
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

    if (cmd == "augury") {
        game.augury();
        return;
    }

    if (cmd == "pay") {
        game.payAtShop();
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

    if (cmd == "seed") {
        game.pushSystemMessage("SEED: " + std::to_string(game.seed()));
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

            std::string scoreLine = "#" + std::to_string(i + 1) + " " + who + " " + res + " ";
            scoreLine += "S" + std::to_string(e.score) + " D" + std::to_string(e.depth);
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
            oss << "S" << e.score << " D" << e.depth << " T" << e.turns << " K" << e.kills;
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

        if (cmd == "exportlog") {
            const fs::path outPath = argName.empty() ? (baseDir / ("procrogue_log_" + ts + ".txt")) : (baseDir / fs::path(argName));
            if (!exportRunLogToFile(game, outPath)) {
                game.pushSystemMessage("FAILED TO EXPORT LOG.");
            } else {
                game.pushSystemMessage("EXPORTED LOG: " + outPath.string());
            }
            return;
        }

        if (cmd == "exportmap") {
            const fs::path outPath = argName.empty() ? (baseDir / ("procrogue_map_" + ts + ".txt")) : (baseDir / fs::path(argName));
            if (!exportRunMapToFile(game, outPath)) {
                game.pushSystemMessage("FAILED TO EXPORT MAP.");
            } else {
                game.pushSystemMessage("EXPORTED MAP: " + outPath.string());
            }
            return;
        }

        if (cmd == "dump") {
            const fs::path outPath = argName.empty() ? (baseDir / ("procrogue_dump_" + ts + ".txt")) : (baseDir / fs::path(argName));
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
            fs::path prefix = argName.empty() ? fs::path("procrogue_" + ts) : fs::path(argName);

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
        const fs::path logPath = baseDir / ("procrogue_log_" + ts + ".txt");
        const fs::path mapPath = baseDir / ("procrogue_map_" + ts + ".txt");

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
            int n = 0;
            AllyOrder order = AllyOrder::Follow;
            bool mixed = false;
            bool first = true;

            for (const auto& e : game.entities()) {
                if (e.id == game.playerId()) continue;
                if (e.hp <= 0) continue;
                if (!e.friendly) continue;
                ++n;
                if (first) { order = e.allyOrder; first = false; }
                else if (e.allyOrder != order) mixed = true;
            }

            if (n <= 0) {
                game.pushSystemMessage("NO COMPANIONS.");
            } else {
                std::string o = mixed ? "MIXED" :
                    (order == AllyOrder::Follow ? "FOLLOW" :
                     order == AllyOrder::Stay ? "STAY" :
                     order == AllyOrder::Fetch ? "FETCH" : "GUARD");
                game.pushSystemMessage("COMPANIONS: " + std::to_string(n) + " | ORDER: " + o + " | USAGE: pet <follow|stay|fetch|guard>");
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
};

constexpr ItemKind RING_KINDS[] = {
    ItemKind::RingMight,
    ItemKind::RingAgility,
    ItemKind::RingFocus,
    ItemKind::RingProtection,
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
