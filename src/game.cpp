#include "game.hpp"
#include "version.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <sstream>
#include <fstream>
#include <cstring>
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
// Trap kind is stored in charges bits 8..15.
static constexpr int CHEST_FLAG_LOCKED = 1 << 0;
static constexpr int CHEST_FLAG_TRAPPED = 1 << 1;
static constexpr int CHEST_FLAG_OPENED = 1 << 2;
static constexpr int CHEST_FLAG_TRAP_KNOWN = 1 << 3;
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

static TrapKind chestTrapKind(const Item& it) {
    const int v = (it.charges >> CHEST_TRAP_SHIFT) & 0xFF;
    return static_cast<TrapKind>(v);
}

static int chestTier(const Item& it) {
    // Stored in enchant (0..2). Not shown to the player.
    return clampi(it.enchant, 0, 2);
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

static void setChestTrapKind(Item& it, TrapKind k) {
    it.charges &= ~(0xFF << CHEST_TRAP_SHIFT);
    it.charges |= (static_cast<int>(k) & 0xFF) << CHEST_TRAP_SHIFT;
}

static std::vector<std::string> extendedCommandList() {
    // Keep these short and stable: they're user-facing and used for completion/prefix matching.
    return {
        "help",
        "options",
        "save",
        "load",
        "loadauto",
        "quit",
        "restart",
        "autopickup",
        "autosave",
        "stepdelay",
        "identify",
        "timers",
        "seed",
        "version",
        "name",
        "scores",
        "explore",
        "search",
        "rest",
        "pray",
    };
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
        return;
    }

    if (cmd == "options") {
        game.handleAction(Action::Options);
        return;
    }

    if (cmd == "save") {
        (void)game.saveToFile(game.defaultSavePath());
        return;
    }
    if (cmd == "load") {
        (void)game.loadFromFile(game.defaultSavePath());
        return;
    }
    if (cmd == "loadauto") {
        (void)game.loadFromFile(game.defaultAutosavePath());
        return;
    }

    if (cmd == "quit") {
        game.requestQuit();
        game.pushSystemMessage("QUIT REQUESTED. (If nothing happens, press ESC.)");
        return;
    }

    if (cmd == "restart") {
        game.handleAction(Action::Restart);
        return;
    }

    if (cmd == "explore") {
        game.requestAutoExplore();
        return;
    }

    if (cmd == "search") {
        game.handleAction(Action::Search);
        return;
    }

    if (cmd == "rest") {
        game.handleAction(Action::Rest);
        return;
    }

    if (cmd == "pray") {
        game.prayAtShrine(arg(1));
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
            const std::string who = e.name.empty() ? std::string("PLAYER") : e.name;
            const std::string res = e.won ? std::string("WIN") : std::string("DEAD");

            std::string line = "#" + std::to_string(i + 1) + " " + who + " " + res + " ";
            line += "S" + std::to_string(e.score) + " D" + std::to_string(e.depth);
            line += " T" + std::to_string(e.turns) + " K" + std::to_string(e.kills);
            if (!e.cause.empty()) line += " " + e.cause;

            game.pushSystemMessage(line);
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
        else if (v == "all") m = AutoPickupMode::All;
        else {
            game.pushSystemMessage("USAGE: autopickup [off|gold|all]");
            return;
        }

        game.setAutoPickupMode(m);
        game.markSettingsDirty();

        const char* label = (m == AutoPickupMode::Off)  ? "OFF"
                            : (m == AutoPickupMode::Gold) ? "GOLD"
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
        case EntityKind::Troll: return "TROLL";
        case EntityKind::Wizard: return "WIZARD";
        case EntityKind::Snake: return "SNAKE";
        case EntityKind::Spider: return "SPIDER";
        case EntityKind::Ogre: return "OGRE";
        default: return "THING";
    }
}

bool isAdjacent8(const Vec2i& a, const Vec2i& b) {
    const int dx = std::abs(a.x - b.x);
    const int dy = std::abs(a.y - b.y);
    return (dx <= 1 && dy <= 1 && (dx + dy) != 0);
}

bool diagonalPassable(const Dungeon& dung, const Vec2i& from, int dx, int dy) {
    // Prevent corner-cutting through two blocked orthogonal tiles.
    if (dx == 0 || dy == 0) return true;
    const int ox1 = from.x + dx;
    const int oy1 = from.y;
    const int ox2 = from.x;
    const int oy2 = from.y + dy;
    // Closed doors are treated as blocking here so you can't slip around them.
    const bool o1 = dung.isWalkable(ox1, oy1);
    const bool o2 = dung.isWalkable(ox2, oy2);
    return o1 || o2;
}

// ------------------------------------------------------------
// Identification visuals (run-randomized potion colors / scroll glyphs)
// ------------------------------------------------------------

constexpr const char* POTION_APPEARANCES[] = {
    "RUBY", "EMERALD", "SAPPHIRE", "AMBER", "TOPAZ", "ONYX", "PEARL", "IVORY",
    "AZURE", "VIOLET", "CRIMSON", "VERDANT", "SILVER", "GOLDEN", "SMOKE", "MURKY",
};

constexpr const char* SCROLL_APPEARANCES[] = {
    "ZELGO", "XANATH", "KERNOD", "ELBERR", "MAPIRO", "VORPAL", "KLAATU", "BARADA",
    "NIKTO", "RAGNAR", "YENDOR", "MORDOR", "AZATHO", "ALOHOM", "OROBO", "NYARLA",
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
};

} // namespace

Game::Game() : dung(MAP_W, MAP_H) {}

const Entity& Game::player() const {
    for (const auto& e : ents) if (e.id == playerId_) return e;
    return ents.front();
}

Entity& Game::playerMut() {
    for (auto& e : ents) if (e.id == playerId_) return e;
    return ents.front();
}

void Game::pushMsg(const std::string& s, MessageKind kind, bool fromPlayer) {
    // Keep some scrollback
    if (msgs.size() > 400) {
        msgs.erase(msgs.begin(), msgs.begin() + 100);
        msgScroll = std::min(msgScroll, static_cast<int>(msgs.size()));
    }
    msgs.push_back({s, kind, fromPlayer});
    // If not scrolled up, stay pinned to newest.
    if (msgScroll == 0) {
        // pinned
    } else {
        // keep viewing older lines; new messages increase effective scroll
        msgScroll = std::min(msgScroll + 1, static_cast<int>(msgs.size()));
    }
}

void Game::pushSystemMessage(const std::string& msg) {
    pushMsg(msg, MessageKind::System, false);
}

Entity* Game::entityById(int id) {
    for (auto& e : ents) if (e.id == id) return &e;
    return nullptr;
}

const Entity* Game::entityById(int id) const {
    for (const auto& e : ents) if (e.id == id) return &e;
    return nullptr;
}

Entity* Game::entityAtMut(int x, int y) {
    for (auto& e : ents) {
        if (e.hp > 0 && e.pos.x == x && e.pos.y == y) return &e;
    }
    return nullptr;
}

const Entity* Game::entityAt(int x, int y) const {
    for (const auto& e : ents) {
        if (e.hp > 0 && e.pos.x == x && e.pos.y == y) return &e;
    }
    return nullptr;
}

int Game::equippedMeleeIndex() const {
    return findItemIndexById(inv, equipMeleeId);
}

int Game::equippedRangedIndex() const {
    return findItemIndexById(inv, equipRangedId);
}

int Game::equippedArmorIndex() const {
    return findItemIndexById(inv, equipArmorId);
}

const Item* Game::equippedMelee() const {
    int idx = equippedMeleeIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedRanged() const {
    int idx = equippedRangedIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedArmor() const {
    int idx = equippedArmorIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

bool Game::isEquipped(int itemId) const {
    return itemId != 0 && (itemId == equipMeleeId || itemId == equipRangedId || itemId == equipArmorId);
}

std::string Game::equippedTag(int itemId) const {
    std::string t;
    if (itemId != 0 && itemId == equipMeleeId) t += "M";
    if (itemId != 0 && itemId == equipRangedId) t += "R";
    if (itemId != 0 && itemId == equipArmorId) t += "A";
    return t;
}

std::string Game::equippedMeleeName() const {
    const Item* w = equippedMelee();
    return w ? displayItemName(*w) : std::string("(NONE)");
}

std::string Game::equippedRangedName() const {
    const Item* w = equippedRanged();
    return w ? displayItemName(*w) : std::string("(NONE)");
}

std::string Game::equippedArmorName() const {
    const Item* a = equippedArmor();
    return a ? displayItemName(*a) : std::string("(NONE)");
}

int Game::playerAttack() const {
    int atk = player().baseAtk;
    if (const Item* w = equippedMelee()) {
        atk += itemDef(w->kind).meleeAtk;
        atk += w->enchant;
    }
    return atk;
}

int Game::playerDefense() const {
    int def = player().baseDef;
    if (const Item* a = equippedArmor()) {
        def += itemDef(a->kind).defense;
        def += a->enchant;
    }
    // Temporary shielding buff
    if (player().shieldTurns > 0) def += 2;
    return def;
}

int Game::playerRangedRange() const {
    const Item* w = equippedRanged();
    if (!w) return 0;
    return itemDef(w->kind).range;
}

bool Game::playerHasRangedReady(std::string* reasonOut) const {
    const Item* w = equippedRanged();
    if (!w) {
        if (reasonOut) *reasonOut = "NO RANGED WEAPON EQUIPPED.";
        return false;
    }
    const ItemDef& d = itemDef(w->kind);
    if (d.range <= 0) {
        if (reasonOut) *reasonOut = "THAT WEAPON CAN'T FIRE.";
        return false;
    }

    if (d.maxCharges > 0 && w->charges <= 0) {
        if (reasonOut) *reasonOut = "THE WAND IS OUT OF CHARGES.";
        return false;
    }

    if (d.ammo != AmmoKind::None) {
        int have = ammoCount(inv, d.ammo);
        if (have <= 0) {
            if (reasonOut) {
                *reasonOut = (d.ammo == AmmoKind::Arrow) ? "NO ARROWS." : "NO ROCKS.";
            }
            return false;
        }
    }

    return true;
}

int Game::xpFor(EntityKind k) const {
    switch (k) {
        case EntityKind::Goblin: return 8;
        case EntityKind::Bat: return 6;
        case EntityKind::Slime: return 10;
        case EntityKind::Snake: return 12;
        case EntityKind::Spider: return 14;
        case EntityKind::KoboldSlinger: return 12;
        case EntityKind::SkeletonArcher: return 16;
        case EntityKind::Wolf: return 10;
        case EntityKind::Orc: return 14;
        case EntityKind::Troll: return 28;
        case EntityKind::Ogre: return 30;
        case EntityKind::Wizard: return 32;
        default: return 10;
    }
}

void Game::grantXp(int amount) {
    if (amount <= 0) return;
    xp += amount;

    std::ostringstream ss;
    ss << "YOU GAIN " << amount << " XP.";
    pushMsg(ss.str(), MessageKind::Success);

    while (xp >= xpNext) {
        xp -= xpNext;
        charLevel += 1;
        // Scale XP requirement for the next level.
        xpNext = static_cast<int>(xpNext * 1.35f + 10);
        onPlayerLevelUp();
    }
}

void Game::onPlayerLevelUp() {
    Entity& p = playerMut();

    int hpGain = 2 + rng.range(0, 2);
    p.hpMax += hpGain;

    bool atkUp = false;
    bool defUp = false;
    if (charLevel % 2 == 0) {
        p.baseAtk += 1;
        atkUp = true;
    }
    if (charLevel % 3 == 0) {
        p.baseDef += 1;
        defUp = true;
    }

    // Full heal on level up.
    p.hp = p.hpMax;

    std::ostringstream ss;
    ss << "LEVEL UP! YOU ARE NOW LEVEL " << charLevel << ".";
    pushMsg(ss.str(), MessageKind::Success);

    std::ostringstream ss2;
    ss2 << "+" << hpGain << " MAX HP";
    if (atkUp) ss2 << ", +1 ATK";
    if (defUp) ss2 << ", +1 DEF";
    ss2 << ".";
    pushMsg(ss2.str(), MessageKind::Success);
}

bool Game::playerHasAmulet() const {
    for (const auto& it : inv) {
        if (it.kind == ItemKind::AmuletYendor) return true;
    }
    return false;
}

// ------------------------------------------------------------
// Identification (potions/scrolls start unknown; appearances randomized per run)
// ------------------------------------------------------------

void Game::initIdentificationTables() {
    identKnown.fill(1);
    identAppearance.fill(0);

    if (!identifyItemsEnabled) {
        // All items show true names.
        return;
    }

    // Mark potions + scrolls as unknown by default.
    for (ItemKind k : POTION_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }
    for (ItemKind k : SCROLL_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }

    // Build a random 1:1 mapping of appearance tokens to each kind.
    auto shuffledIndices = [&](size_t n) {
        std::vector<uint8_t> idx;
        idx.reserve(n);
        for (size_t i = 0; i < n; ++i) idx.push_back(static_cast<uint8_t>(i));
        for (size_t i = n; i-- > 1;) {
            const int j = rng.range(0, static_cast<int>(i));
            std::swap(idx[i], idx[static_cast<size_t>(j)]);
        }
        return idx;
    };

    const size_t potionN = sizeof(POTION_KINDS) / sizeof(POTION_KINDS[0]);
    const size_t scrollN = sizeof(SCROLL_KINDS) / sizeof(SCROLL_KINDS[0]);
    const size_t potionAppearN = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
    const size_t scrollAppearN = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);

    std::vector<uint8_t> p = shuffledIndices(potionAppearN);
    std::vector<uint8_t> s = shuffledIndices(scrollAppearN);

    // If someone later adds more potion/scroll kinds than appearances, we still function
    // (we'll reuse appearances), but keep the common case unique.
    for (size_t i = 0; i < potionN; ++i) {
        const uint8_t app = p[i % p.size()];
        identAppearance[static_cast<size_t>(POTION_KINDS[i])] = app;
    }
    for (size_t i = 0; i < scrollN; ++i) {
        const uint8_t app = s[i % s.size()];
        identAppearance[static_cast<size_t>(SCROLL_KINDS[i])] = app;
    }
}

bool Game::isIdentified(ItemKind k) const {
    if (!identifyItemsEnabled) return true;
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return true;
    return identKnown[idx] != 0;
}

uint8_t Game::appearanceFor(ItemKind k) const {
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return 0;
    return identAppearance[idx];
}

std::string Game::appearanceName(ItemKind k) const {
    if (isPotionKind(k)) {
        const auto n = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
        uint8_t a = appearanceFor(k);
        if (n == 0) return "";
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return POTION_APPEARANCES[a];
    }
    if (isScrollKind(k)) {
        const auto n = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);
        uint8_t a = appearanceFor(k);
        if (n == 0) return "";
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return SCROLL_APPEARANCES[a];
    }
    return "";
}

std::string Game::unknownDisplayName(const Item& it) const {
    std::ostringstream ss;
    if (isPotionKind(it.kind)) {
        const std::string app = appearanceName(it.kind);
        if (it.count > 1) ss << it.count << " " << app << " POTIONS";
        else ss << app << " POTION";
        return ss.str();
    }
    if (isScrollKind(it.kind)) {
        const std::string app = appearanceName(it.kind);
        if (it.count > 1) ss << it.count << " SCROLLS '" << app << "'";
        else ss << "SCROLL '" << app << "'";
        return ss.str();
    }
    return itemDisplayName(it);
}

bool Game::markIdentified(ItemKind k, bool quiet) {
    if (!identifyItemsEnabled) return false;
    if (!isIdentifiableKind(k)) return false;
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return false;
    if (identKnown[idx] != 0) return false;
    identKnown[idx] = 1;

    if (!quiet) {
        Item tmp;
        tmp.kind = k;
        tmp.count = 1;
        const std::string oldName = unknownDisplayName(tmp);
        const std::string newName = itemDisplayNameSingle(k);
        pushMsg("IDENTIFIED: " + oldName + " = " + newName + ".", MessageKind::System, true);
    }

    return true;
}

std::string Game::displayItemName(const Item& it) const {
    if (!identifyItemsEnabled) return itemDisplayName(it);
    if (!isIdentifiableKind(it.kind)) return itemDisplayName(it);
    return isIdentified(it.kind) ? itemDisplayName(it) : unknownDisplayName(it);
}

std::string Game::displayItemNameSingle(ItemKind k) const {
    Item tmp;
    tmp.kind = k;
    tmp.count = 1;
    return displayItemName(tmp);
}

void Game::newGame(uint32_t seed) {
    if (seed == 0) {
        // Fall back to a simple randomized seed if user passes 0.
        seed = hash32(static_cast<uint32_t>(std::rand()) ^ 0xA5A5F00Du);
    }

    rng = RNG(seed);
    seed_ = seed;
    depth_ = 1;
    levels.clear();

    ents.clear();
    ground.clear();
    trapsCur.clear();
    inv.clear();
    fx.clear();

    nextEntityId = 1;
    nextItemId = 1;
    equipMeleeId = 0;
    equipRangedId = 0;
    equipArmorId = 0;

    invOpen = false;
    invSel = 0;
    targeting = false;
    targetLine.clear();
    targetValid = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;

    msgs.clear();
    msgScroll = 0;

    autoPickup = AutoPickupMode::Gold;

    // Randomize potion/scroll appearances and reset identification knowledge.
    initIdentificationTables();

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;

    turnCount = 0;
    naturalRegenCounter = 0;
    lastAutosaveTurn = 0;

    killCount = 0;
    maxDepth = 1;
    runRecorded = false;
    hastePhase = false;
    looking = false;
    lookPos = {0,0};

    inputLock = false;
    gameOver = false;
    gameWon = false;

    endCause_.clear();

    charLevel = 1;
    xp = 0;
    xpNext = 20;

    // Hunger pacing (optional setting; stored per-run in save files).
    hungerMax = 800;
    hunger = hungerMax;
    hungerStatePrev = hungerStateFor(hunger, hungerMax);

    dung.generate(rng);

    // Create player
    Entity p;
    p.id = nextEntityId++;
    p.kind = EntityKind::Player;
    p.pos = dung.stairsUp;
    p.hpMax = 18;
    p.hp = p.hpMax;
    p.baseAtk = 3;
    p.baseDef = 0;
    p.spriteSeed = rng.nextU32();
    playerId_ = p.id;

    ents.push_back(p);

    // Starting gear
    auto give = [&](ItemKind k, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        if (k == ItemKind::WandSparks) it.charges = itemDef(k).maxCharges;
        inv.push_back(it);
        return it.id;
    };

    int bowId = give(ItemKind::Bow, 1);
    give(ItemKind::Arrow, 14);
    int dagId = give(ItemKind::Dagger, 1);
    int armId = give(ItemKind::LeatherArmor, 1);
    give(ItemKind::PotionHealing, 2);
    // New: basic food. Heals a little and (if hunger is enabled) restores hunger.
    give(ItemKind::FoodRation, hungerEnabled_ ? 2 : 1);
    give(ItemKind::ScrollTeleport, 1);
    give(ItemKind::ScrollMapping, 1);
    give(ItemKind::Gold, 10);

    // Equip both melee + ranged so bump-attacks and FIRE both work immediately.
    equipMeleeId = dagId;
    equipRangedId = bowId;
    equipArmorId = armId;

    spawnMonsters();
    spawnItems();
    spawnTraps();

    storeCurrentLevel();
    recomputeFov();

    pushMsg("WELCOME TO PROCROGUE++.", MessageKind::System);
    pushMsg("GOAL: FIND THE AMULET OF YENDOR (DEPTH 5), THEN RETURN TO THE EXIT (<) TO WIN.", MessageKind::System);
    pushMsg("PRESS ? FOR HELP. I INVENTORY. F TARGET/FIRE. M MINIMAP. TAB STATS. F12 SCREENSHOT.", MessageKind::System);
    pushMsg("MOVE: WASD/ARROWS + Y/U/B/N DIAGONALS. TIP: C SEARCH. O AUTO-EXPLORE. P AUTO-PICKUP.", MessageKind::System);
    pushMsg("SAVE: F5   LOAD: F9   LOAD AUTO: F10", MessageKind::System);
}

void Game::storeCurrentLevel() {
    LevelState st;
    st.depth = depth_;
    st.dung = dung;
    st.ground = ground;
    st.traps = trapsCur;
    st.monsters.clear();
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        st.monsters.push_back(e);
    }
    levels[depth_] = std::move(st);
}

bool Game::restoreLevel(int depth) {
    auto it = levels.find(depth);
    if (it == levels.end()) return false;

    dung = it->second.dung;
    ground = it->second.ground;
    trapsCur = it->second.traps;

    // Keep player, restore monsters.
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != playerId_;
    }), ents.end());

    for (const auto& m : it->second.monsters) {
        ents.push_back(m);
    }

    return true;
}

void Game::changeLevel(int newDepth, bool goingDown) {
    if (newDepth < 1) return;

    storeCurrentLevel();

    // Clear transient states.
    fx.clear();
    inputLock = false;

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;
    invOpen = false;
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    depth_ = newDepth;
    maxDepth = std::max(maxDepth, depth_);

    bool restored = restoreLevel(depth_);

    Entity& p = playerMut();

    if (!restored) {
        // New level: generate and populate.
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != playerId_;
        }), ents.end());
        ground.clear();
        trapsCur.clear();

        dung.generate(rng);

        // Place player before spawning so we never spawn on top of them.
        p.pos = goingDown ? dung.stairsUp : dung.stairsDown;
        p.alerted = false;

        spawnMonsters();
        spawnItems();
        spawnTraps();

        // Save this freshly created level.
        storeCurrentLevel();
    } else {
        // Returning to a visited level.
        p.pos = goingDown ? dung.stairsUp : dung.stairsDown;
        p.alerted = false;
    }

    // Small heal on travel.
    p.hp = std::min(p.hpMax, p.hp + 2);

    std::ostringstream ss;
    if (goingDown) ss << "YOU DESCEND TO DEPTH " << depth_ << ".";
    else ss << "YOU ASCEND TO DEPTH " << depth_ << ".";
    pushMsg(ss.str());

    recomputeFov();
}


std::string Game::defaultSavePath() const {
    if (!savePathOverride.empty()) return savePathOverride;
    return "procrogue_save.dat";
}

void Game::setSavePath(const std::string& path) {
    savePathOverride = path;
}

std::string Game::defaultAutosavePath() const {
    if (!autosavePathOverride.empty()) return autosavePathOverride;

    // Default autosave goes next to the normal save file.
    std::filesystem::path basePath = std::filesystem::path(defaultSavePath()).parent_path();
    if (basePath.empty()) return "procrogue_autosave.dat";
    return (basePath / "procrogue_autosave.dat").string();
}

void Game::setAutosavePath(const std::string& path) {
    autosavePathOverride = path;
}

void Game::setAutosaveEveryTurns(int turns) {
    autosaveInterval = std::max(0, std::min(5000, turns));
}

std::string Game::defaultScoresPath() const {
    if (!scoresPathOverride.empty()) return scoresPathOverride;

    std::filesystem::path basePath = std::filesystem::path(defaultSavePath()).parent_path();
    if (basePath.empty()) return "procrogue_scores.csv";
    return (basePath / "procrogue_scores.csv").string();
}

void Game::setScoresPath(const std::string& path) {
    scoresPathOverride = path;
    // Non-fatal if missing; it will be created on first recorded run.
    (void)scores.load(defaultScoresPath());
}

void Game::setSettingsPath(const std::string& path) {
    settingsPath_ = path;
}

int Game::autoStepDelayMs() const {
    // Stored internally in seconds.
    return static_cast<int>(autoStepDelay * 1000.0f + 0.5f);
}

void Game::commandTextInput(const char* utf8) {
    if (!commandOpen) return;
    if (!utf8) return;
    // Basic length cap so the overlay stays sane.
    if (commandBuf.size() > 120) return;
    commandBuf += utf8;
}

static void utf8PopBack(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    // Walk back over UTF-8 continuation bytes (10xxxxxx).
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) {
        --i;
    }
    s.erase(i);
}

void Game::commandBackspace() {
    if (!commandOpen) return;
    utf8PopBack(commandBuf);
}

void Game::commandAutocomplete() {
    if (!commandOpen) return;

    std::string s = trim(commandBuf);
    if (s.empty()) return;

    // Only complete the first token; once you add arguments we assume you know what you're doing.
    if (s.find_first_of(" 	") != std::string::npos) return;

    std::string prefix = toLower(s);
    std::vector<std::string> cmds = extendedCommandList();

    std::vector<std::string> matches;
    for (const auto& c : cmds) {
        if (c.rfind(prefix, 0) == 0) matches.push_back(c);
    }

    if (matches.size() == 1) {
        commandBuf = matches[0] + " ";
        return;
    }

    if (matches.size() > 1) {
        std::string line = "MATCHES:";
        for (const auto& m : matches) line += " " + m;
        pushSystemMessage(line);
        return;
    }
}

void Game::setAutoPickupMode(AutoPickupMode m) {
    autoPickup = m;
}

int Game::keyCount() const {
    int n = 0;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::Key) n += std::max(0, it.count);
    }
    return n;
}

int Game::lockpickCount() const {
    int n = 0;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::Lockpick) n += std::max(0, it.count);
    }
    return n;
}

bool Game::consumeKeys(int n) {
    if (n <= 0) return true;

    int need = n;
    for (auto& it : inv) {
        if (it.kind != ItemKind::Key) continue;
        int take = std::min(it.count, need);
        it.count -= take;
        need -= take;
        if (need <= 0) break;
    }

    // Remove emptied stackables.
    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& it) {
        return isStackable(it.kind) && it.count <= 0;
    }), inv.end());

    return need <= 0;
}

bool Game::consumeLockpicks(int n) {
    if (n <= 0) return true;

    int need = n;
    for (auto& it : inv) {
        if (it.kind != ItemKind::Lockpick) continue;
        int take = std::min(it.count, need);
        it.count -= take;
        need -= take;
        if (need <= 0) break;
    }

    // Remove emptied stackables.
    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& it) {
        return isStackable(it.kind) && it.count <= 0;
    }), inv.end());

    return need <= 0;
}


void Game::setPlayerName(std::string name) {
    std::string n = trim(std::move(name));
    if (n.empty()) n = "PLAYER";

    // Strip control chars (keeps the HUD / CSV clean).
    std::string filtered;
    filtered.reserve(n.size());
    for (char c : n) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127) continue;
        filtered.push_back(c);
    }

    filtered = trim(std::move(filtered));
    if (filtered.empty()) filtered = "PLAYER";
    if (filtered.size() > 24) filtered.resize(24);
    playerName_ = std::move(filtered);
}

void Game::setIdentificationEnabled(bool enabled) {
    identifyItemsEnabled = enabled;
}

void Game::setHungerEnabled(bool enabled) {
    hungerEnabled_ = enabled;

    // Initialize reasonable defaults lazily so older paths don't need to know.
    if (hungerMax <= 0) hungerMax = 800;
    hunger = clampi(hunger, 0, hungerMax);

    hungerStatePrev = hungerStateFor(hunger, hungerMax);
}

std::string Game::hungerTag() const {
    if (!hungerEnabled_) return std::string();
    const int st = hungerStateFor(hunger, hungerMax);
    if (st == 1) return "HUNGRY";
    if (st >= 2) return "STARVING";
    return std::string();
}

void Game::setAutoStepDelayMs(int ms) {
    // Clamp to sane values to avoid accidental 0ms "teleport walking".
    const int clamped = clampi(ms, 10, 500);
    autoStepDelay = clamped / 1000.0f;
}

namespace {
constexpr uint32_t SAVE_MAGIC = 0x50525356u; // 'PRSV'
constexpr uint32_t SAVE_VERSION = 7u;

template <typename T>
void writePod(std::ostream& out, const T& v) {
    out.write(reinterpret_cast<const char*>(&v), static_cast<std::streamsize>(sizeof(T)));
}

template <typename T>
bool readPod(std::istream& in, T& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), static_cast<std::streamsize>(sizeof(T))));
}

void writeString(std::ostream& out, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    writePod(out, len);
    if (len) out.write(s.data(), static_cast<std::streamsize>(len));
}

bool readString(std::istream& in, std::string& s) {
    uint32_t len = 0;
    if (!readPod(in, len)) return false;
    s.assign(len, '\0');
    if (len) {
        if (!in.read(s.data(), static_cast<std::streamsize>(len))) return false;
    }
    return true;
}

void writeItem(std::ostream& out, const Item& it) {
    int32_t id = static_cast<int32_t>(it.id);
    writePod(out, id);
    uint8_t kind = static_cast<uint8_t>(it.kind);
    writePod(out, kind);
    int32_t count = static_cast<int32_t>(it.count);
    writePod(out, count);
    int32_t charges = static_cast<int32_t>(it.charges);
    writePod(out, charges);
    writePod(out, it.spriteSeed);
    int32_t enchant = static_cast<int32_t>(it.enchant);
    writePod(out, enchant);
}

bool readItem(std::istream& in, Item& it, uint32_t version) {
    int32_t id = 0;
    uint8_t kind = 0;
    int32_t count = 0;
    int32_t charges = 0;
    uint32_t seed = 0;
    int32_t enchant = 0;
    if (!readPod(in, id)) return false;
    if (!readPod(in, kind)) return false;
    if (!readPod(in, count)) return false;
    if (!readPod(in, charges)) return false;
    if (!readPod(in, seed)) return false;
    if (version >= 2u) {
        if (!readPod(in, enchant)) return false;
    }
    it.id = id;
    it.kind = static_cast<ItemKind>(kind);
    it.count = count;
    it.charges = charges;
    it.spriteSeed = seed;
    it.enchant = enchant;
    return true;
}

void writeEntity(std::ostream& out, const Entity& e) {
    int32_t id = static_cast<int32_t>(e.id);
    writePod(out, id);
    uint8_t kind = static_cast<uint8_t>(e.kind);
    writePod(out, kind);
    int32_t x = e.pos.x;
    int32_t y = e.pos.y;
    writePod(out, x);
    writePod(out, y);
    int32_t hp = e.hp;
    int32_t hpMax = e.hpMax;
    int32_t atk = e.baseAtk;
    int32_t def = e.baseDef;
    writePod(out, hp);
    writePod(out, hpMax);
    writePod(out, atk);
    writePod(out, def);
    writePod(out, e.spriteSeed);
    int32_t groupId = e.groupId;
    writePod(out, groupId);
    uint8_t alerted = e.alerted ? 1 : 0;
    writePod(out, alerted);

    uint8_t canRanged = e.canRanged ? 1 : 0;
    writePod(out, canRanged);
    int32_t rRange = e.rangedRange;
    int32_t rAtk = e.rangedAtk;
    writePod(out, rRange);
    writePod(out, rAtk);
    uint8_t rAmmo = static_cast<uint8_t>(e.rangedAmmo);
    uint8_t rProj = static_cast<uint8_t>(e.rangedProjectile);
    writePod(out, rAmmo);
    writePod(out, rProj);

    uint8_t packAI = e.packAI ? 1 : 0;
    uint8_t willFlee = e.willFlee ? 1 : 0;
    writePod(out, packAI);
    writePod(out, willFlee);

    int32_t regenChance = e.regenChancePct;
    int32_t regenAmt = e.regenAmount;
    writePod(out, regenChance);
    writePod(out, regenAmt);

    // v2+: timed status effects
    int32_t poison = e.poisonTurns;
    int32_t regenTurns = e.regenTurns;
    int32_t shieldTurns = e.shieldTurns;
    writePod(out, poison);
    writePod(out, regenTurns);
    writePod(out, shieldTurns);

    // v3+: additional buffs
    int32_t hasteTurns = e.hasteTurns;
    int32_t visionTurns = e.visionTurns;
    writePod(out, hasteTurns);
    writePod(out, visionTurns);

    // v6+: additional debuffs
    int32_t webTurns = e.webTurns;
    writePod(out, webTurns);
}

bool readEntity(std::istream& in, Entity& e, uint32_t version) {
    int32_t id = 0;
    uint8_t kind = 0;
    int32_t x = 0, y = 0;
    int32_t hp = 0, hpMax = 0;
    int32_t atk = 0, def = 0;
    uint32_t seed = 0;
    int32_t groupId = 0;
    uint8_t alerted = 0;

    uint8_t canRanged = 0;
    int32_t rRange = 0;
    int32_t rAtk = 0;
    uint8_t rAmmo = 0;
    uint8_t rProj = 0;

    uint8_t packAI = 0;
    uint8_t willFlee = 0;

    int32_t regenChance = 0;
    int32_t regenAmt = 0;

    int32_t poison = 0;
    int32_t regenTurns = 0;
    int32_t shieldTurns = 0;
    int32_t hasteTurns = 0;
    int32_t visionTurns = 0;
    int32_t webTurns = 0;

    if (!readPod(in, id)) return false;
    if (!readPod(in, kind)) return false;
    if (!readPod(in, x)) return false;
    if (!readPod(in, y)) return false;
    if (!readPod(in, hp)) return false;
    if (!readPod(in, hpMax)) return false;
    if (!readPod(in, atk)) return false;
    if (!readPod(in, def)) return false;
    if (!readPod(in, seed)) return false;
    if (!readPod(in, groupId)) return false;
    if (!readPod(in, alerted)) return false;

    if (!readPod(in, canRanged)) return false;
    if (!readPod(in, rRange)) return false;
    if (!readPod(in, rAtk)) return false;
    if (!readPod(in, rAmmo)) return false;
    if (!readPod(in, rProj)) return false;

    if (!readPod(in, packAI)) return false;
    if (!readPod(in, willFlee)) return false;

    if (!readPod(in, regenChance)) return false;
    if (!readPod(in, regenAmt)) return false;

    if (version >= 2u) {
        if (!readPod(in, poison)) return false;
        if (!readPod(in, regenTurns)) return false;
        if (!readPod(in, shieldTurns)) return false;

        if (version >= 3u) {
            if (!readPod(in, hasteTurns)) return false;
            if (!readPod(in, visionTurns)) return false;
        }

        if (version >= 6u) {
            if (!readPod(in, webTurns)) return false;
        }
    }

    e.id = id;
    e.kind = static_cast<EntityKind>(kind);
    e.pos = { x, y };
    e.hp = hp;
    e.hpMax = hpMax;
    e.baseAtk = atk;
    e.baseDef = def;
    e.spriteSeed = seed;
    e.groupId = groupId;
    e.alerted = alerted != 0;

    e.canRanged = canRanged != 0;
    e.rangedRange = rRange;
    e.rangedAtk = rAtk;
    e.rangedAmmo = static_cast<AmmoKind>(rAmmo);
    e.rangedProjectile = static_cast<ProjectileKind>(rProj);

    e.packAI = packAI != 0;
    e.willFlee = willFlee != 0;

    e.regenChancePct = regenChance;
    e.regenAmount = regenAmt;

    e.poisonTurns = poison;
    e.regenTurns = regenTurns;
    e.shieldTurns = shieldTurns;
    e.hasteTurns = hasteTurns;
    e.visionTurns = visionTurns;
    e.webTurns = webTurns;
    return true;
}

} // namespace

bool Game::saveToFile(const std::string& path, bool quiet) {
    // Ensure the currently-loaded level is persisted into `levels`.
    storeCurrentLevel();

    std::filesystem::path p(path);
    std::filesystem::path dir = p.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }

    // Write to a temporary file first, then replace the target.
    std::filesystem::path tmp = p.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary);
    if (!out) {
        if (!quiet) pushMsg("FAILED TO SAVE (CANNOT OPEN FILE).");
        return false;
    }

    writePod(out, SAVE_MAGIC);
    writePod(out, SAVE_VERSION);

    uint32_t rngState = rng.state;
    writePod(out, rngState);

    int32_t depth = depth_;
    writePod(out, depth);

    int32_t playerId = playerId_;
    writePod(out, playerId);

    int32_t nextE = nextEntityId;
    int32_t nextI = nextItemId;
    writePod(out, nextE);
    writePod(out, nextI);

    int32_t eqM = equipMeleeId;
    int32_t eqR = equipRangedId;
    int32_t eqA = equipArmorId;
    writePod(out, eqM);
    writePod(out, eqR);
    writePod(out, eqA);

    int32_t clvl = charLevel;
    int32_t xpNow = xp;
    int32_t xpNeed = xpNext;
    writePod(out, clvl);
    writePod(out, xpNow);
    writePod(out, xpNeed);

    uint8_t over = gameOver ? 1 : 0;
    uint8_t won = gameWon ? 1 : 0;
    writePod(out, over);
    writePod(out, won);

    // v2+: user/options
    uint8_t autoPick = static_cast<uint8_t>(autoPickup);
    writePod(out, autoPick);

    // v3+: pacing state
    uint32_t turnsNow = turnCount;
    int32_t natRegen = naturalRegenCounter;
    uint8_t hasteP = hastePhase ? 1 : 0;
    writePod(out, turnsNow);
    writePod(out, natRegen);
    writePod(out, hasteP);

    // v5+: run meta
    uint32_t seedNow = seed_;
    uint32_t killsNow = killCount;
    int32_t maxD = maxDepth;
    writePod(out, seedNow);
    writePod(out, killsNow);
    writePod(out, maxD);

    // v6+: item identification tables (run knowledge + randomized appearances)
    uint32_t kindCount = static_cast<uint32_t>(ITEM_KIND_COUNT);
    writePod(out, kindCount);
    for (uint32_t i = 0; i < kindCount; ++i) {
        const uint8_t known = identKnown[static_cast<size_t>(i)];
        const uint8_t app = identAppearance[static_cast<size_t>(i)];
        writePod(out, known);
        writePod(out, app);
    }

    // v7+: hunger system state (per-run)
    uint8_t hungerEnabledTmp = hungerEnabled_ ? 1u : 0u;
    int32_t hungerTmp = static_cast<int32_t>(hunger);
    int32_t hungerMaxTmp = static_cast<int32_t>(hungerMax);
    writePod(out, hungerEnabledTmp);
    writePod(out, hungerTmp);
    writePod(out, hungerMaxTmp);

    // Player
    writeEntity(out, player());

    // Inventory
    uint32_t invCount = static_cast<uint32_t>(inv.size());
    writePod(out, invCount);
    for (const auto& it : inv) {
        writeItem(out, it);
    }

    // Messages (for convenience)
    uint32_t msgCount = static_cast<uint32_t>(msgs.size());
    writePod(out, msgCount);
    for (const auto& m : msgs) {
        uint8_t mk = static_cast<uint8_t>(m.kind);
        uint8_t fp = m.fromPlayer ? 1 : 0;
        writePod(out, mk);
        writePod(out, fp);
        writeString(out, m.text);
    }

    // Levels
    uint32_t lvlCount = static_cast<uint32_t>(levels.size());
    writePod(out, lvlCount);
    for (const auto& kv : levels) {
        const int d = kv.first;
        const LevelState& st = kv.second;

        int32_t d32 = d;
        writePod(out, d32);

        // Dungeon
        int32_t w = st.dung.width;
        int32_t h = st.dung.height;
        writePod(out, w);
        writePod(out, h);
        int32_t upx = st.dung.stairsUp.x;
        int32_t upy = st.dung.stairsUp.y;
        int32_t dnx = st.dung.stairsDown.x;
        int32_t dny = st.dung.stairsDown.y;
        writePod(out, upx);
        writePod(out, upy);
        writePod(out, dnx);
        writePod(out, dny);

        uint32_t roomCount = static_cast<uint32_t>(st.dung.rooms.size());
        writePod(out, roomCount);
        for (const auto& r : st.dung.rooms) {
            int32_t rx = r.x, ry = r.y, rw = r.w, rh = r.h;
            writePod(out, rx);
            writePod(out, ry);
            writePod(out, rw);
            writePod(out, rh);
            uint8_t rt = static_cast<uint8_t>(r.type);
            writePod(out, rt);
        }

        uint32_t tileCount = static_cast<uint32_t>(st.dung.tiles.size());
        writePod(out, tileCount);
        for (const auto& t : st.dung.tiles) {
            uint8_t tt = static_cast<uint8_t>(t.type);
            uint8_t explored = t.explored ? 1 : 0;
            writePod(out, tt);
            writePod(out, explored);
        }

        // Monsters
        uint32_t monCount = static_cast<uint32_t>(st.monsters.size());
        writePod(out, monCount);
        for (const auto& m : st.monsters) {
            writeEntity(out, m);
        }

        // Ground items
        uint32_t gCount = static_cast<uint32_t>(st.ground.size());
        writePod(out, gCount);
        for (const auto& gi : st.ground) {
            int32_t gx = gi.pos.x;
            int32_t gy = gi.pos.y;
            writePod(out, gx);
            writePod(out, gy);
            writeItem(out, gi.item);
        }

        // Traps
        uint32_t tCount = static_cast<uint32_t>(st.traps.size());
        writePod(out, tCount);
        for (const auto& tr : st.traps) {
            uint8_t tk = static_cast<uint8_t>(tr.kind);
            int32_t tx = tr.pos.x;
            int32_t ty = tr.pos.y;
            uint8_t disc = tr.discovered ? 1 : 0;
            writePod(out, tk);
            writePod(out, tx);
            writePod(out, ty);
            writePod(out, disc);
        }
    }

    out.flush();
    if (!out.good()) {
        if (!quiet) pushMsg("FAILED TO SAVE (WRITE ERROR).");
        out.close();
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    out.close();

    // Replace the target.
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        // On Windows, rename fails if destination exists; remove then retry.
        std::error_code ec2;
        std::filesystem::remove(p, ec2);
        ec.clear();
        std::filesystem::rename(tmp, p, ec);
    }
    if (ec) {
        // Final fallback: copy then remove tmp.
        std::error_code ec2;
        std::filesystem::copy_file(tmp, p, std::filesystem::copy_options::overwrite_existing, ec2);
        std::filesystem::remove(tmp, ec2);
        if (ec2) {
            if (!quiet) pushMsg("FAILED TO SAVE (CANNOT REPLACE FILE).");
            return false;
        }
    }

    if (!quiet) pushMsg("GAME SAVED.", MessageKind::Success, false);
    return true;
}

bool Game::loadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        pushMsg("NO SAVE FILE FOUND.");
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    if (!readPod(in, magic) || !readPod(in, version) || magic != SAVE_MAGIC || version == 0u || version > SAVE_VERSION) {
        pushMsg("SAVE FILE IS INVALID OR FROM ANOTHER VERSION.");
        return false;
    }

    uint32_t rngState = 0;
    int32_t depth = 1;
    int32_t pId = 0;
    int32_t nextE = 1;
    int32_t nextI = 1;
    int32_t eqM = 0;
    int32_t eqR = 0;
    int32_t eqA = 0;
    int32_t clvl = 1;
    int32_t xpNow = 0;
    int32_t xpNeed = 20;
    uint8_t over = 0;
    uint8_t won = 0;
    uint8_t autoPick = 1; // v2+: default enabled (gold). v4+: mode enum (0/1/2)
    uint32_t turnsNow = 0;
    int32_t natRegen = 0;
    uint8_t hasteP = 0;
    uint32_t seedNow = 0;
    uint32_t killsNow = 0;
    int32_t maxD = 1;

    if (!readPod(in, rngState)) return false;
    if (!readPod(in, depth)) return false;
    if (!readPod(in, pId)) return false;
    if (!readPod(in, nextE)) return false;
    if (!readPod(in, nextI)) return false;
    if (!readPod(in, eqM)) return false;
    if (!readPod(in, eqR)) return false;
    if (!readPod(in, eqA)) return false;
    if (!readPod(in, clvl)) return false;
    if (!readPod(in, xpNow)) return false;
    if (!readPod(in, xpNeed)) return false;
    if (!readPod(in, over)) return false;
    if (!readPod(in, won)) return false;

    if (version >= 2u) {
        if (!readPod(in, autoPick)) return false;
    }

    if (version >= 3u) {
        if (!readPod(in, turnsNow)) return false;
        if (!readPod(in, natRegen)) return false;
        if (!readPod(in, hasteP)) return false;
    }

    if (version >= 5u) {
        if (!readPod(in, seedNow)) return false;
        if (!readPod(in, killsNow)) return false;
        if (!readPod(in, maxD)) return false;
    }

    // v6+: item identification tables
    std::array<uint8_t, ITEM_KIND_COUNT> identKnownTmp{};
    std::array<uint8_t, ITEM_KIND_COUNT> identAppTmp{};
    identKnownTmp.fill(1); // older saves had fully-known item names
    identAppTmp.fill(0);

    if (version >= 6u) {
        uint32_t kindCount = 0;
        if (!readPod(in, kindCount)) return false;
        for (uint32_t i = 0; i < kindCount; ++i) {
            uint8_t known = 1;
            uint8_t app = 0;
            if (!readPod(in, known)) return false;
            if (!readPod(in, app)) return false;
            if (i < static_cast<uint32_t>(ITEM_KIND_COUNT)) {
                identKnownTmp[static_cast<size_t>(i)] = known;
                identAppTmp[static_cast<size_t>(i)] = app;
            }
        }

        // If this save was made with an older build (fewer ItemKind values),
        // initialize any newly-added identifiable kinds so item-ID stays consistent.
        if (identifyItemsEnabled && kindCount < static_cast<uint32_t>(ITEM_KIND_COUNT)) {
            constexpr size_t POTION_APP_COUNT = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
            constexpr size_t SCROLL_APP_COUNT = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);
            std::vector<bool> usedPotionApps(POTION_APP_COUNT, false);
            std::vector<bool> usedScrollApps(SCROLL_APP_COUNT, false);

            auto markUsed = [&](ItemKind k, std::vector<bool>& used, size_t maxApps) {
                const uint32_t idx = static_cast<uint32_t>(k);
                if (idx >= kindCount || idx >= static_cast<uint32_t>(ITEM_KIND_COUNT)) return;
                const uint8_t a = identAppTmp[static_cast<size_t>(idx)];
                if (static_cast<size_t>(a) < maxApps) used[static_cast<size_t>(a)] = true;
            };

            for (ItemKind k : POTION_KINDS) markUsed(k, usedPotionApps, usedPotionApps.size());
            for (ItemKind k : SCROLL_KINDS) markUsed(k, usedScrollApps, usedScrollApps.size());

            auto takeUnused = [&](std::vector<bool>& used) -> uint8_t {
                for (size_t j = 0; j < used.size(); ++j) {
                    if (!used[j]) {
                        used[j] = true;
                        return static_cast<uint8_t>(j);
                    }
                }
                return 0u;
            };

            for (uint32_t i = kindCount; i < static_cast<uint32_t>(ITEM_KIND_COUNT); ++i) {
                ItemKind k = static_cast<ItemKind>(i);
                if (!isIdentifiableKind(k)) continue;

                // Unknown by default in this run (but keep the save file aligned).
                identKnownTmp[static_cast<size_t>(i)] = 0u;

                if (isPotionKind(k)) identAppTmp[static_cast<size_t>(i)] = takeUnused(usedPotionApps);
                else if (isScrollKind(k)) identAppTmp[static_cast<size_t>(i)] = takeUnused(usedScrollApps);
            }
        }
    }

    // v7+: hunger system state (per-run)
    uint8_t hungerEnabledTmp = hungerEnabled_ ? 1u : 0u;
    int32_t hungerTmp = 800;
    int32_t hungerMaxTmp = 800;
    if (version >= 7u) {
        if (!readPod(in, hungerEnabledTmp)) return false;
        if (!readPod(in, hungerTmp)) return false;
        if (!readPod(in, hungerMaxTmp)) return false;
    }

    Entity p;
    if (!readEntity(in, p, version)) return false;

    uint32_t invCount = 0;
    if (!readPod(in, invCount)) return false;
    std::vector<Item> invTmp;
    invTmp.reserve(invCount);
    for (uint32_t i = 0; i < invCount; ++i) {
        Item it;
        if (!readItem(in, it, version)) return false;
        invTmp.push_back(it);
    }

    uint32_t msgCount = 0;
    if (!readPod(in, msgCount)) return false;
    std::vector<Message> msgsTmp;
    msgsTmp.reserve(msgCount);
    for (uint32_t i = 0; i < msgCount; ++i) {
        if (version >= 2u) {
            uint8_t mk = 0;
            uint8_t fp = 1;
            std::string s;
            if (!readPod(in, mk)) return false;
            if (!readPod(in, fp)) return false;
            if (!readString(in, s)) return false;
            Message m;
            m.text = std::move(s);
            m.kind = static_cast<MessageKind>(mk);
            m.fromPlayer = fp != 0;
            msgsTmp.push_back(std::move(m));
        } else {
            std::string s;
            if (!readString(in, s)) return false;
            msgsTmp.push_back({std::move(s), MessageKind::Info, true});
        }
    }

    uint32_t lvlCount = 0;
    if (!readPod(in, lvlCount)) return false;
    std::map<int, LevelState> levelsTmp;

    for (uint32_t li = 0; li < lvlCount; ++li) {
        int32_t d32 = 0;
        if (!readPod(in, d32)) return false;

        int32_t w = 0, h = 0;
        int32_t upx = 0, upy = 0, dnx = 0, dny = 0;
        if (!readPod(in, w)) return false;
        if (!readPod(in, h)) return false;
        if (!readPod(in, upx)) return false;
        if (!readPod(in, upy)) return false;
        if (!readPod(in, dnx)) return false;
        if (!readPod(in, dny)) return false;

        LevelState st;
        st.depth = d32;
        st.dung = Dungeon(w, h);
        st.dung.stairsUp = { upx, upy };
        st.dung.stairsDown = { dnx, dny };

        uint32_t roomCount = 0;
        if (!readPod(in, roomCount)) return false;
        st.dung.rooms.clear();
        st.dung.rooms.reserve(roomCount);
        for (uint32_t ri = 0; ri < roomCount; ++ri) {
            int32_t rx = 0, ry = 0, rw = 0, rh = 0;
            uint8_t rt = 0;
            if (!readPod(in, rx)) return false;
            if (!readPod(in, ry)) return false;
            if (!readPod(in, rw)) return false;
            if (!readPod(in, rh)) return false;
            if (!readPod(in, rt)) return false;
            Room r;
            r.x = rx;
            r.y = ry;
            r.w = rw;
            r.h = rh;
            r.type = static_cast<RoomType>(rt);
            st.dung.rooms.push_back(r);
        }

        uint32_t tileCount = 0;
        if (!readPod(in, tileCount)) return false;
        st.dung.tiles.assign(tileCount, Tile{});
        for (uint32_t ti = 0; ti < tileCount; ++ti) {
            uint8_t tt = 0;
            uint8_t explored = 0;
            if (!readPod(in, tt)) return false;
            if (!readPod(in, explored)) return false;
            st.dung.tiles[ti].type = static_cast<TileType>(tt);
            st.dung.tiles[ti].visible = false;
            st.dung.tiles[ti].explored = explored != 0;
        }

        uint32_t monCount = 0;
        if (!readPod(in, monCount)) return false;
        st.monsters.clear();
        st.monsters.reserve(monCount);
        for (uint32_t mi = 0; mi < monCount; ++mi) {
            Entity m;
            if (!readEntity(in, m, version)) return false;
            st.monsters.push_back(m);
        }

        uint32_t gCount = 0;
        if (!readPod(in, gCount)) return false;
        st.ground.clear();
        st.ground.reserve(gCount);
        for (uint32_t gi = 0; gi < gCount; ++gi) {
            int32_t gx = 0, gy = 0;
            if (!readPod(in, gx)) return false;
            if (!readPod(in, gy)) return false;
            GroundItem gr;
            gr.pos = { gx, gy };
            if (!readItem(in, gr.item, version)) return false;
            st.ground.push_back(gr);
        }

        // Traps (v2+)
        st.traps.clear();
        if (version >= 2u) {
            uint32_t tCount = 0;
            if (!readPod(in, tCount)) return false;
            st.traps.reserve(tCount);
            for (uint32_t ti = 0; ti < tCount; ++ti) {
                uint8_t tk = 0;
                int32_t tx = 0, ty = 0;
                uint8_t disc = 0;
                if (!readPod(in, tk)) return false;
                if (!readPod(in, tx)) return false;
                if (!readPod(in, ty)) return false;
                if (!readPod(in, disc)) return false;
                Trap tr;
                tr.kind = static_cast<TrapKind>(tk);
                tr.pos = { tx, ty };
                tr.discovered = disc != 0;
                st.traps.push_back(tr);
            }
        }

        levelsTmp[d32] = std::move(st);
    }

    // If we got here, we have a fully parsed save. Commit state.
    rng = RNG(rngState);
    depth_ = depth;
    playerId_ = pId;
    nextEntityId = nextE;
    nextItemId = nextI;
    equipMeleeId = eqM;
    equipRangedId = eqR;
    equipArmorId = eqA;
    charLevel = clvl;
    xp = xpNow;
    xpNext = xpNeed;
    gameOver = over != 0;
    gameWon = won != 0;
    if (version >= 4u) {
        autoPickup = static_cast<AutoPickupMode>(autoPick);
        if (autoPickup > AutoPickupMode::All) autoPickup = AutoPickupMode::Gold;
    } else {
        autoPickup = (autoPick != 0) ? AutoPickupMode::Gold : AutoPickupMode::Off;
    }

    // v3+: pacing state
    turnCount = turnsNow;
    naturalRegenCounter = natRegen;
    hastePhase = (hasteP != 0);

    // v5+: run meta
    seed_ = seedNow;
    killCount = killsNow;
    maxDepth = (maxD > 0) ? maxD : depth_;
    if (maxDepth < depth_) maxDepth = depth_;
    // If we loaded an already-finished run, don't record it again.
    runRecorded = isFinished();

    lastAutosaveTurn = 0;

    // v6+: identification tables (or default "all known" for older saves)
    identKnown = identKnownTmp;
    identAppearance = identAppTmp;


    // v7+: hunger state
    if (version >= 7u) {
        hungerEnabled_ = (hungerEnabledTmp != 0);
        hungerMax = (hungerMaxTmp > 0) ? static_cast<int>(hungerMaxTmp) : 800;
        hunger = clampi(static_cast<int>(hungerTmp), 0, hungerMax);
    } else {
        // Pre-hunger saves: keep the current setting, but start fully fed.
        if (hungerMax <= 0) hungerMax = 800;
        hunger = hungerMax;
    }
    hungerStatePrev = hungerStateFor(hunger, hungerMax);

    inv = std::move(invTmp);
    msgs = std::move(msgsTmp);
    msgScroll = 0;

    levels = std::move(levelsTmp);

    // Rebuild entity list: player + monsters for current depth
    ents.clear();
    ents.push_back(p);

    // Sanity: ensure we have the current depth.
    if (levels.find(depth_) == levels.end()) {
        // Fallback: if missing, reconstruct from what's available.
        if (!levels.empty()) depth_ = levels.begin()->first;
    }

    // Close transient UI and effects.
    invOpen = false;
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    looking = false;
    lookPos = {0,0};
    inputLock = false;
    fx.clear();

    restoreLevel(depth_);
    recomputeFov();

    pushMsg("GAME LOADED.");
    return true;
}

void Game::update(float dt) {
    // Animate FX projectiles.
    if (!fx.empty()) {
        inputLock = true;
        for (auto& p : fx) {
            p.stepTimer += dt;
            while (p.stepTimer >= p.stepTime) {
                p.stepTimer -= p.stepTime;
                if (p.pathIndex + 1 < p.path.size()) {
                    p.pathIndex++;
                } else {
                    p.pathIndex = p.path.size();
                    break;
                }
            }
        }
        fx.erase(std::remove_if(fx.begin(), fx.end(), [](const FXProjectile& p) {
            return p.path.empty() || p.pathIndex >= p.path.size();
        }), fx.end());
    }

    if (fx.empty()) {
        inputLock = false;
    }

    // Auto-move (travel / explore) steps are processed here to keep the game turn-based
    // while still providing smooth-ish movement.
    if (autoMode != AutoMoveMode::None) {
        // If the player opened an overlay, stop (don't keep walking while in menus).
        if (invOpen || targeting || helpOpen || looking || minimapOpen || statsOpen || optionsOpen || commandOpen || isFinished()) {
            stopAutoMove(true);
            return;
        }

        if (!inputLock) {
            autoStepTimer += dt;
            if (autoStepTimer >= autoStepDelay) {
                autoStepTimer = 0.0f;
                (void)stepAutoMove();
            }
        }
    }
}

void Game::handleAction(Action a) {
    if (a == Action::None) return;

    // Any manual action stops auto-move (except log scrolling).
    if (autoMode != AutoMoveMode::None && a != Action::LogUp && a != Action::LogDown) {
        stopAutoMove(true);
    }

    // Message log scroll works in any mode.
    if (a == Action::LogUp) {
        int maxScroll = std::max(0, static_cast<int>(msgs.size()) - 1);
        msgScroll = clampi(msgScroll + 1, 0, maxScroll);
        return;
    }
    if (a == Action::LogDown) {
        int maxScroll = std::max(0, static_cast<int>(msgs.size()) - 1);
        msgScroll = clampi(msgScroll - 1, 0, maxScroll);
        return;
    }

    auto closeOverlays = [&]() {
        invOpen = false;
        targeting = false;
        helpOpen = false;
        looking = false;
        minimapOpen = false;
        statsOpen = false;
        optionsOpen = false;

        if (commandOpen) {
            commandOpen = false;
            commandBuf.clear();
            commandDraft.clear();
            commandHistoryPos = -1;
        }

        msgScroll = 0;
    };

    // Global hotkeys (available even while dead/won).
    switch (a) {
        case Action::Save:
            (void)saveToFile(defaultSavePath());
            return;
        case Action::Load:
            (void)loadFromFile(defaultSavePath());
            return;
        case Action::LoadAuto:
            (void)loadFromFile(defaultAutosavePath());
            return;
        case Action::Help:
            // Toggle help overlay.
            helpOpen = !helpOpen;
            if (helpOpen) {
                closeOverlays();
                helpOpen = true;
            }
            return;
        case Action::ToggleMinimap:
            if (minimapOpen) {
                minimapOpen = false;
            } else {
                closeOverlays();
                minimapOpen = true;
            }
            return;
        case Action::ToggleStats:
            if (statsOpen) {
                statsOpen = false;
            } else {
                closeOverlays();
                statsOpen = true;
            }
            return;
        case Action::Options:
            if (optionsOpen) {
                optionsOpen = false;
            } else {
                closeOverlays();
                optionsOpen = true;
                optionsSel = 0;
            }
            return;
        case Action::Command:
            if (commandOpen) {
                commandOpen = false;
                commandBuf.clear();
                commandDraft.clear();
                commandHistoryPos = -1;
            } else {
                closeOverlays();
                commandOpen = true;
                commandBuf.clear();
                commandDraft.clear();
                commandHistoryPos = -1;
            }
            return;
        default:
            break;
    }

    // Toggle auto-pickup (safe to do in any non-finished state).
    if (a == Action::ToggleAutoPickup) {
        switch (autoPickup) {
            case AutoPickupMode::Off:  autoPickup = AutoPickupMode::Gold; break;
            case AutoPickupMode::Gold: autoPickup = AutoPickupMode::All;  break;
            case AutoPickupMode::All:  autoPickup = AutoPickupMode::Off;  break;
            default:                   autoPickup = AutoPickupMode::Gold; break;
        }

        settingsDirtyFlag = true;

        const char* mode =
            (autoPickup == AutoPickupMode::Off)  ? "OFF" :
            (autoPickup == AutoPickupMode::Gold) ? "GOLD" : "ALL";

        std::string msg = std::string("AUTO-PICKUP: ") + mode + ".";
        pushMsg(msg, MessageKind::System);
        return;
    }

    // Auto-explore request.
    if (a == Action::AutoExplore) {
        requestAutoExplore();
        return;
    }

    // Overlay: extended command prompt (does not consume turns)
    if (commandOpen) {
        if (a == Action::Cancel || a == Action::Command) {
            commandOpen = false;
            commandBuf.clear();
            commandDraft.clear();
            commandHistoryPos = -1;
            return;
        }

        if (a == Action::Confirm) {
            std::string line = trim(commandBuf);
            commandOpen = false;
            commandBuf.clear();
            commandDraft.clear();
            commandHistoryPos = -1;

            if (!line.empty()) {
                // Store history (keep it small).
                if (commandHistory.empty() || commandHistory.back() != line) {
                    commandHistory.push_back(line);
                    if (commandHistory.size() > 50) {
                        commandHistory.erase(commandHistory.begin());
                    }
                }
                runExtendedCommand(*this, line);
            }
            return;
        }

        if (a == Action::Up) {
            if (!commandHistory.empty()) {
                if (commandHistoryPos < 0) {
                    commandDraft = commandBuf;
                    commandHistoryPos = static_cast<int>(commandHistory.size()) - 1;
                } else {
                    commandHistoryPos = std::max(0, commandHistoryPos - 1);
                }
                commandBuf = commandHistory[commandHistoryPos];
            }
            return;
        }

        if (a == Action::Down) {
            if (commandHistoryPos >= 0) {
                if (commandHistoryPos + 1 < static_cast<int>(commandHistory.size())) {
                    ++commandHistoryPos;
                    commandBuf = commandHistory[commandHistoryPos];
                } else {
                    commandHistoryPos = -1;
                    commandBuf = commandDraft;
                    commandDraft.clear();
                }
            }
            return;
        }

        // Ignore any other actions while the prompt is open.
        return;
    }

    // Overlay: options menu (does not consume turns)
    if (optionsOpen) {
        constexpr int kOptionCount = 8;

        if (a == Action::Cancel || a == Action::Options) {
            optionsOpen = false;
            return;
        }

        if (a == Action::Up) {
            optionsSel = clampi(optionsSel - 1, 0, kOptionCount - 1);
            return;
        }
        if (a == Action::Down) {
            optionsSel = clampi(optionsSel + 1, 0, kOptionCount - 1);
            return;
        }

        const bool left = (a == Action::Left);
        const bool right = (a == Action::Right);
        const bool confirm = (a == Action::Confirm);

        auto cycleAutoPickup = [&](int dir) {
            int m = static_cast<int>(autoPickup);
            m += dir;
            if (m < 0) m = 2;
            if (m > 2) m = 0;
            autoPickup = static_cast<AutoPickupMode>(m);
            settingsDirtyFlag = true;
        };

        // 0) Auto-pickup
        if (optionsSel == 0) {
            if (left) cycleAutoPickup(-1);
            else if (right || confirm) cycleAutoPickup(+1);
            return;
        }

        // 1) Auto-step delay
        if (optionsSel == 1) {
            if (left || right) {
                int ms = autoStepDelayMs();
                ms += left ? -5 : +5;
                ms = clampi(ms, 10, 500);
                setAutoStepDelayMs(ms);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 2) Autosave interval
        if (optionsSel == 2) {
            if (left || right) {
                int t = autosaveInterval;
                t += left ? -50 : +50;
                t = clampi(t, 0, 5000);
                setAutosaveEveryTurns(t);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 3) Identification helper
        if (optionsSel == 3) {
            if (left || right || confirm) {
                setIdentificationEnabled(!identifyItemsEnabled);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 4) Hunger system
        if (optionsSel == 4) {
            if (left || right || confirm) {
                setHungerEnabled(!hungerEnabled_);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 5) Effect timers (HUD)
        if (optionsSel == 5) {
            if (left || right || confirm) {
                showEffectTimers_ = !showEffectTimers_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 6) Confirm quit (double-ESC)
        if (optionsSel == 6) {
            if (left || right || confirm) {
                confirmQuitEnabled_ = !confirmQuitEnabled_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 7) Close
        if (optionsSel == 7) {
            if (left || right || confirm) optionsOpen = false;
            return;
        }

        return;
    }

    // Finished runs: allow restart (and global UI hotkeys above).
    if (isFinished()) {
        if (a == Action::Restart) {
            newGame(hash32(rng.nextU32()));
        }
        return;
    }

    // If animating FX, only allow Cancel to close overlays.
    if (inputLock) {
        if (a == Action::Cancel) {
            closeOverlays();
        }
        return;
    }

    // Overlay: minimap
    if (minimapOpen) {
        if (a == Action::Cancel) minimapOpen = false;
        return;
    }

    // Overlay: stats
    if (statsOpen) {
        if (a == Action::Cancel) statsOpen = false;
        return;
    }

    // Help overlay mode.
    if (helpOpen) {
        if (a == Action::Cancel || a == Action::Inventory || a == Action::Help) {
            helpOpen = false;
        }
        return;
    }

    // Look / examine mode.
    if (looking) {
        switch (a) {
            case Action::Up:        moveLookCursor(0, -1); break;
            case Action::Down:      moveLookCursor(0, 1); break;
            case Action::Left:      moveLookCursor(-1, 0); break;
            case Action::Right:     moveLookCursor(1, 0); break;
            case Action::UpLeft:    moveLookCursor(-1, -1); break;
            case Action::UpRight:   moveLookCursor(1, -1); break;
            case Action::DownLeft:  moveLookCursor(-1, 1); break;
            case Action::DownRight: moveLookCursor(1, 1); break;
            case Action::Inventory:
                endLook();
                openInventory();
                break;
            case Action::Fire:
                // Convenient: jump straight from look -> targeting (cursor stays where you were looking).
                {
                    Vec2i desired = lookPos;
                    endLook();
                    beginTargeting();
                    if (targeting) {
                        targetPos = desired;
                        recomputeTargetLine();
                    }
                }
                break;
            case Action::Confirm:
                // Auto-travel to the looked-at tile (doesn't consume a turn by itself).
                if (requestAutoTravel(lookPos)) {
                    endLook();
                }
                break;
            case Action::Cancel:
            case Action::Look:
                endLook();
                break;
            default:
                break;
        }
        return;
    }

    bool acted = false;

    // Inventory mode.
    if (invOpen) {
        switch (a) {
            case Action::Up: moveInventorySelection(-1); break;
            case Action::Down: moveInventorySelection(1); break;
            case Action::Inventory:
            case Action::Cancel:
                closeInventory();
                break;

            case Action::Confirm: {
                // Context action: equip if equipable, otherwise use if consumable.
                if (!inv.empty()) {
                    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
                    const Item& it = inv[static_cast<size_t>(invSel)];
                    const ItemDef& d = itemDef(it.kind);
                    if (d.slot != EquipSlot::None) acted = equipSelected();
                    else if (d.consumable) acted = useSelected();
                }
                break;
            }

            case Action::Equip:
                acted = equipSelected();
                break;
            case Action::Use:
                acted = useSelected();
                break;
            case Action::Drop:
                acted = dropSelected();
                break;
            case Action::DropAll:
                acted = dropSelectedAll();
                break;
            case Action::SortInventory:
                sortInventory();
                break;
            default:
                break;
        }

        if (acted) {
            advanceAfterPlayerAction();
        }
        return;
    }

    // Targeting mode.
    if (targeting) {
        switch (a) {
            case Action::Up:        moveTargetCursor(0, -1); break;
            case Action::Down:      moveTargetCursor(0, 1); break;
            case Action::Left:      moveTargetCursor(-1, 0); break;
            case Action::Right:     moveTargetCursor(1, 0); break;
            case Action::UpLeft:    moveTargetCursor(-1, -1); break;
            case Action::UpRight:   moveTargetCursor(1, -1); break;
            case Action::DownLeft:  moveTargetCursor(-1, 1); break;
            case Action::DownRight: moveTargetCursor(1, 1); break;
            case Action::Confirm:
            case Action::Fire:
                endTargeting(true);
                acted = true;
                break;
            case Action::Cancel:
                endTargeting(false);
                break;
            default:
                break;
        }

        if (acted) {
            advanceAfterPlayerAction();
        }
        return;
    }

    // Normal play mode.
    Entity& p = playerMut();
    switch (a) {
        case Action::Up:        acted = tryMove(p, 0, -1); break;
        case Action::Down:      acted = tryMove(p, 0, 1); break;
        case Action::Left:      acted = tryMove(p, -1, 0); break;
        case Action::Right:     acted = tryMove(p, 1, 0); break;
        case Action::UpLeft:    acted = tryMove(p, -1, -1); break;
        case Action::UpRight:   acted = tryMove(p, 1, -1); break;
        case Action::DownLeft:  acted = tryMove(p, -1, 1); break;
        case Action::DownRight: acted = tryMove(p, 1, 1); break;
        case Action::Wait:
            pushMsg("YOU WAIT.", MessageKind::Info);
            acted = true;
            break;
        case Action::Search:
            acted = searchForTraps();
            break;
        case Action::Pickup:
            acted = pickupAtPlayer();
            break;
        case Action::Inventory:
            openInventory();
            break;
        case Action::Fire:
            beginTargeting();
            break;
        case Action::Look:
            beginLook();
            acted = false;
            break;
        case Action::Rest:
            restUntilSafe();
            acted = false;
            break;
        case Action::Confirm: {
            if (p.pos == dung.stairsDown) {
                changeLevel(depth_ + 1, true);
                acted = false;
            } else if (p.pos == dung.stairsUp) {
                // At depth 1, stairs up is the exit.
                if (depth_ <= 1) {
                    if (playerHasAmulet()) {
                        gameWon = true;
                        if (endCause_.empty()) endCause_ = "ESCAPED WITH THE AMULET";
                        pushMsg("YOU ESCAPE WITH THE AMULET OF YENDOR!", MessageKind::Success);
                        pushMsg("VICTORY!", MessageKind::Success);
                        maybeRecordRun();
                    } else {
                        pushMsg("THE EXIT IS HERE... BUT YOU STILL NEED THE AMULET.");
                    }
                } else {
                    changeLevel(depth_ - 1, false);
                }
                acted = false;
            } else {
                // QoL: context action on the current tile.
                // 1) Chests (world-interactable) have priority.
                bool hasChest = false;
                bool hasPickableItem = false;
                for (const auto& gi : ground) {
                    if (gi.pos != p.pos) continue;
                    if (gi.item.kind == ItemKind::Chest) hasChest = true;
                    if (!isChestKind(gi.item.kind)) hasPickableItem = true;
                }

                if (hasChest) {
                    acted = openChestAtPlayer();
                    // If we didn't open the chest (e.g., locked and no keys/picks), still allow picking
                    // up any other items on the tile.
                    if (!acted && hasPickableItem) {
                        acted = pickupAtPlayer();
                    }
                } else if (hasPickableItem) {
                    acted = pickupAtPlayer();
                } else {
                    pushMsg("THERE IS NOTHING HERE.");
                }
            }
        } break;
        case Action::StairsDown:
            if (p.pos == dung.stairsDown) {
                changeLevel(depth_ + 1, true);
                acted = false;
            } else {
                pushMsg("THERE ARE NO STAIRS HERE.");
            }
            break;
        case Action::StairsUp:
            if (p.pos == dung.stairsUp) {
                if (depth_ <= 1) {
                    if (playerHasAmulet()) {
                        gameWon = true;
                        if (endCause_.empty()) endCause_ = "ESCAPED WITH THE AMULET";
                        pushMsg("YOU ESCAPE WITH THE AMULET OF YENDOR!", MessageKind::Success);
                        pushMsg("VICTORY!", MessageKind::Success);
                        maybeRecordRun();
                    } else {
                        pushMsg("THE EXIT IS HERE... BUT YOU STILL NEED THE AMULET.");
                    }
                } else {
                    changeLevel(depth_ - 1, false);
                }
                acted = false;
            } else {
                pushMsg("THERE ARE NO STAIRS HERE.");
            }
            break;
        case Action::Restart:
            newGame(hash32(rng.nextU32()));
            acted = false;
            break;
        default:
            break;
    }

    if (acted) {
        advanceAfterPlayerAction();
    }
}

void Game::advanceAfterPlayerAction() {
    // One "turn" = one player action that consumes time.
    // Haste gives the player an extra action every other turn by skipping the monster turn.
    ++turnCount;

    if (isFinished()) {
        // Don't let monsters act after a decisive player action.
        cleanupDead();
        recomputeFov();
        maybeRecordRun();
        return;
    }

    Entity& p = playerMut();

    bool runMonsters = true;
    if (p.hasteTurns > 0) {
        if (!hastePhase) {
            // Free haste action: skip monsters this time.
            runMonsters = false;
            hastePhase = true;
        } else {
            // Monster turn occurs, and one haste "cycle" is consumed.
            runMonsters = true;
            hastePhase = false;
            p.hasteTurns = std::max(0, p.hasteTurns - 1);
            if (p.hasteTurns == 0) {
                pushMsg("YOUR SPEED RETURNS TO NORMAL.", MessageKind::System, true);
            }
        }
    } else {
        hastePhase = false;
    }

    if (runMonsters) {
        monsterTurn();
    }

    applyEndOfTurnEffects();
    cleanupDead();
    if (isFinished()) {
        maybeRecordRun();
    }
    recomputeFov();
    maybeAutosave();
}

bool Game::anyVisibleHostiles() const {
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
        if (dung.at(e.pos.x, e.pos.y).visible) return true;
    }
    return false;
}


void Game::maybeAutosave() {
    if (autosaveInterval <= 0) return;
    if (isFinished()) return;
    if (turnCount == 0) return;

    const uint32_t interval = static_cast<uint32_t>(autosaveInterval);
    if (interval == 0) return;

    if ((turnCount % interval) != 0) return;
    if (lastAutosaveTurn == turnCount) return;

    const std::string path = defaultAutosavePath();
    if (path.empty()) return;

    if (saveToFile(path, true)) {
        lastAutosaveTurn = turnCount;
    }
}

static std::string nowTimestampLocal() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void Game::maybeRecordRun() {
    if (runRecorded) return;
    if (!isFinished()) return;

    ScoreEntry e;
    e.timestamp = nowTimestampLocal();
    e.won = gameWon;
    e.depth = maxDepth;
    e.turns = turnCount;
    e.kills = killCount;
    e.level = charLevel;
    e.gold = goldCount();
    e.seed = seed_;

    e.name = playerName_;
    e.cause = endCause_;
    e.gameVersion = PROCROGUE_VERSION;

    e.score = computeScore(e);

    const std::string scorePath = defaultScoresPath();
    if (!scorePath.empty()) {
        const bool ok = scores.append(scorePath, e);
        if (ok) {
            pushMsg("RUN RECORDED.", MessageKind::System);
        }
    }

    runRecorded = true;
}

// ------------------------------------------------------------
// Auto-move / auto-explore
// ------------------------------------------------------------

void Game::cancelAutoMove(bool silent) {
    stopAutoMove(silent);
}

void Game::stopAutoMove(bool silent) {
    if (autoMode == AutoMoveMode::None) return;

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;

    if (!silent) {
        pushMsg("AUTO-MOVE: OFF.", MessageKind::System);
    }
}

bool Game::requestAutoTravel(Vec2i goal) {
    if (isFinished()) return false;
    if (!dung.inBounds(goal.x, goal.y)) return false;

    // Close overlays so you can see the walk.
    invOpen = false;
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    // Don't auto-travel into the unknown: keep it deterministic and safe.
    if (!dung.at(goal.x, goal.y).explored) {
        pushMsg("CAN'T AUTO-TRAVEL TO AN UNEXPLORED TILE.", MessageKind::System);
        return false;
    }

    if (!dung.isPassable(goal.x, goal.y)) {
        pushMsg("NO PATH (BLOCKED).", MessageKind::Warning);
        return false;
    }

    if (goal == player().pos) {
        pushMsg("YOU ARE ALREADY THERE.", MessageKind::System);
        return false;
    }

    if (const Entity* occ = entityAt(goal.x, goal.y)) {
        if (occ->id != playerId_) {
            pushMsg("DESTINATION IS OCCUPIED.", MessageKind::Warning);
            return false;
        }
    }

    stopAutoMove(true);

    if (!buildAutoTravelPath(goal, /*requireExplored*/true)) {
        pushMsg("NO PATH FOUND.", MessageKind::Warning);
        return false;
    }

    autoMode = AutoMoveMode::Travel;
    pushMsg("AUTO-TRAVEL: ON (ESC TO CANCEL).", MessageKind::System);
    return true;
}

void Game::requestAutoExplore() {
    if (isFinished()) return;

    // Toggle off if already exploring.
    if (autoMode == AutoMoveMode::Explore) {
        stopAutoMove(false);
        return;
    }

    // Close overlays.
    invOpen = false;
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    looking = false;
    msgScroll = 0;

    if (anyVisibleHostiles()) {
        pushMsg("CANNOT AUTO-EXPLORE: DANGER NEARBY.", MessageKind::Warning);
        return;
    }

    stopAutoMove(true);

    autoMode = AutoMoveMode::Explore;
    if (!buildAutoExplorePath()) {
        autoMode = AutoMoveMode::None;
        pushMsg("NOTHING LEFT TO EXPLORE.", MessageKind::System);
        return;
    }

    pushMsg("AUTO-EXPLORE: ON (ESC TO CANCEL).", MessageKind::System);
}

bool Game::stepAutoMove() {
    if (autoMode == AutoMoveMode::None) return false;

    if (isFinished()) {
        stopAutoMove(true);
        return false;
    }

    // Safety stops.
    if (anyVisibleHostiles()) {
        pushMsg("AUTO-MOVE INTERRUPTED!", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    // In auto-explore mode, stop when you see non-gold loot so you can decide what to do.
    if (autoMode == AutoMoveMode::Explore) {
        for (const auto& gi : ground) {
            if (gi.item.kind == ItemKind::Gold) continue;
            if (dung.inBounds(gi.pos.x, gi.pos.y) && dung.at(gi.pos.x, gi.pos.y).visible) {
                pushMsg("AUTO-EXPLORE STOPPED (LOOT SPOTTED).", MessageKind::System);
                stopAutoMove(true);
                return false;
            }
        }
    }

    // If we're out of path, rebuild (explore) or finish (travel).
    if (autoPathIndex >= autoPathTiles.size()) {
        if (autoMode == AutoMoveMode::Travel) {
            pushMsg("AUTO-TRAVEL COMPLETE.", MessageKind::System);
            stopAutoMove(true);
            return false;
        }

        // Explore: find the next frontier.
        if (!buildAutoExplorePath()) {
            pushMsg("FLOOR FULLY EXPLORED.", MessageKind::System);
            stopAutoMove(true);
            return false;
        }
    }

    if (autoPathIndex >= autoPathTiles.size()) return false;

    Entity& p = playerMut();
    const Vec2i next = autoPathTiles[autoPathIndex];

    // Sanity: we expect a 4-neighbor path.
    if (!isAdjacent8(p.pos, next)) {
        // The world changed (door opened, trap teleported you, etc). Rebuild if exploring, otherwise stop.
        if (autoMode == AutoMoveMode::Explore) {
            if (!buildAutoExplorePath()) {
                pushMsg("AUTO-EXPLORE STOPPED.", MessageKind::System);
                stopAutoMove(true);
                return false;
            }
            return true;
        }
        pushMsg("AUTO-TRAVEL STOPPED (PATH INVALID).", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    // If a monster blocks the next tile, stop and let the player decide.
    if (const Entity* occ = entityAt(next.x, next.y)) {
        if (occ->id != playerId_) {
            pushMsg("AUTO-MOVE STOPPED (MONSTER BLOCKING).", MessageKind::Warning);
            stopAutoMove(true);
            return false;
        }
    }

    const int dx = next.x - p.pos.x;
    const int dy = next.y - p.pos.y;

    const int hpBefore = p.hp;
    const int poisonBefore = p.poisonTurns;
    const int webBefore = p.webTurns;
    const Vec2i posBefore = p.pos;

    const bool acted = tryMove(p, dx, dy);
    if (!acted) {
        pushMsg("AUTO-MOVE STOPPED (BLOCKED).", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    // If we moved onto the intended next tile, advance. If we opened a door, the position won't change,
    // so we'll try again on the next auto-step.
    if (p.pos == next) {
        autoPathIndex++;
    } else if (p.pos != posBefore) {
        // We moved, but not where we expected (shouldn't happen in 4-neighbor movement).
        pushMsg("AUTO-MOVE STOPPED (DESYNC).", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    advanceAfterPlayerAction();

    if (p.hp < hpBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU TOOK DAMAGE).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (p.poisonTurns > poisonBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU WERE POISONED).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    if (p.webTurns > webBefore) {
        pushMsg("AUTO-MOVE STOPPED (YOU WERE WEBBED).", MessageKind::Warning);
        stopAutoMove(true);
        return false;
    }

    // If travel completed after this step, finish.
    if (autoMode == AutoMoveMode::Travel && autoPathIndex >= autoPathTiles.size()) {
        pushMsg("AUTO-TRAVEL COMPLETE.", MessageKind::System);
        stopAutoMove(true);
        return false;
    }

    return true;
}

bool Game::buildAutoTravelPath(Vec2i goal, bool requireExplored) {
    autoPathTiles = findPathBfs(player().pos, goal, requireExplored);
    if (autoPathTiles.empty()) return false;

    // Remove start tile so the vector becomes a list of "next tiles to step into".
    if (!autoPathTiles.empty() && autoPathTiles.front() == player().pos) {
        autoPathTiles.erase(autoPathTiles.begin());
    }

    autoPathIndex = 0;
    autoStepTimer = 0.0f;

    return !autoPathTiles.empty();
}

bool Game::buildAutoExplorePath() {
    Vec2i goal = findNearestExploreFrontier();
    if (goal.x < 0 || goal.y < 0) return false;
    return buildAutoTravelPath(goal, /*requireExplored*/true);
}

Vec2i Game::findNearestExploreFrontier() const {
    const Vec2i start = player().pos;

    std::vector<uint8_t> visited(MAP_W * MAP_H, 0);
    std::deque<Vec2i> q;

    auto idxOf = [](int x, int y) { return y * MAP_W + x; };

    visited[idxOf(start.x, start.y)] = 1;
    q.push_back(start);

    auto isKnownTrap = [&](int x, int y) -> bool {
        for (const auto& t : trapsCur) {
            if (!t.discovered) continue;
            if (t.pos.x == x && t.pos.y == y) return true;
        }
        return false;
    };

    auto isFrontier = [&](int x, int y) -> bool {
        if (!dung.inBounds(x, y)) return false;
        const Tile& t = dung.at(x, y);
        if (!t.explored) return false;
        // Treat locked doors as passable frontiers if we can unlock them.
        if (!dung.isPassable(x, y)) {
            const TileType tt = dung.at(x, y).type;
            if (!(canUnlockDoors && tt == TileType::DoorLocked)) return false;
        }
        if (isKnownTrap(x, y)) return false;

        // Any adjacent unexplored tile means stepping here can reveal something.
        const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };
        for (const auto& d : dirs) {
            int nx = x + d[0], ny = y + d[1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.at(nx, ny).explored) return true;
        }
        return false;
    };

    const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };

    const bool canUnlockDoors = (keyCount() > 0) || (lockpickCount() > 0);

    while (!q.empty()) {
        Vec2i cur = q.front();
        q.pop_front();

        if (!(cur == start) && isFrontier(cur.x, cur.y)) return cur;

        for (const auto& d : dirs) {
            int dx = d[0], dy = d[1];
            int nx = cur.x + dx, ny = cur.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;

            const int ii = idxOf(nx, ny);
            if (visited[ii]) continue;

            const Tile& t = dung.at(nx, ny);
            if (!t.explored) continue; // don't route through unknown
            if (!dung.isPassable(nx, ny)) {
                const TileType tt = dung.at(nx, ny).type;
                if (!(canUnlockDoors && tt == TileType::DoorLocked)) continue;
            }
            if (isKnownTrap(nx, ny)) continue;

            if (const Entity* occ = entityAt(nx, ny)) {
                if (occ->id != playerId_) continue;
            }

            visited[ii] = 1;
            q.push_back({nx, ny});
        }
    }

    return {-1, -1};
}

std::vector<Vec2i> Game::findPathBfs(Vec2i start, Vec2i goal, bool requireExplored) const {
    if (!dung.inBounds(start.x, start.y) || !dung.inBounds(goal.x, goal.y)) return {};
    if (start == goal) return { start };

    std::vector<int> prev(MAP_W * MAP_H, -1);
    std::vector<uint8_t> visited(MAP_W * MAP_H, 0);
    std::deque<Vec2i> q;

    auto idxOf = [](int x, int y) { return y * MAP_W + x; };
    auto inb = [&](int x, int y) { return dung.inBounds(x, y); };

    auto isKnownTrap = [&](int x, int y) -> bool {
        for (const auto& t : trapsCur) {
            if (!t.discovered) continue;
            if (t.pos.x == x && t.pos.y == y) return true;
        }
        return false;
    };

    const int startIdx = idxOf(start.x, start.y);
    const int goalIdx = idxOf(goal.x, goal.y);

    visited[startIdx] = 1;
    q.push_back(start);

    const int dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1} };

    const bool canUnlockDoors = (keyCount() > 0) || (lockpickCount() > 0);

    while (!q.empty()) {
        Vec2i cur = q.front();
        q.pop_front();

        if (cur == goal) break;

        for (const auto& d : dirs) {
            int dx = d[0], dy = d[1];
            int nx = cur.x + dx, ny = cur.y + dy;
            if (!inb(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;

            const int ni = idxOf(nx, ny);
            if (visited[ni]) continue;

            if (requireExplored && !dung.at(nx, ny).explored && !(nx == goal.x && ny == goal.y)) {
                continue;
            }

            // Allow auto-pathing through locked doors if the player has keys or lockpicks.
            // The actual door opening/unlocking happens during movement (tryMove).
            if (!dung.isPassable(nx, ny)) {
                const TileType tt = dung.at(nx, ny).type;
                if (!(canUnlockDoors && tt == TileType::DoorLocked)) {
                    continue;
                }
            }

            // Avoid known traps if possible.
            if (isKnownTrap(nx, ny) && !(nx == goal.x && ny == goal.y)) continue;

            // Don't path through monsters.
            if (const Entity* occ = entityAt(nx, ny)) {
                if (occ->id != playerId_) {
                    // allow goal only if it's the player (it won't be)
                    continue;
                }
            }

            visited[ni] = 1;
            prev[ni] = idxOf(cur.x, cur.y);
            q.push_back({nx, ny});
        }
    }

    if (!visited[goalIdx]) return {};

    // Reconstruct
    std::vector<Vec2i> path;
    int cur = goalIdx;
    while (cur != -1) {
        int x = cur % MAP_W;
        int y = cur / MAP_W;
        path.push_back({x, y});
        if (cur == startIdx) break;
        cur = prev[cur];
    }

    std::reverse(path.begin(), path.end());
    return path;
}

void Game::beginLook() {
    // Close other overlays
    invOpen = false;
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    looking = true;
    lookPos = player().pos;
}

void Game::endLook() {
    looking = false;
}

void Game::beginLookAt(Vec2i p) {
    beginLook();
    setLookCursor(p);
}

void Game::setLookCursor(Vec2i p) {
    if (!looking) return;
    p.x = clampi(p.x, 0, MAP_W - 1);
    p.y = clampi(p.y, 0, MAP_H - 1);
    lookPos = p;
}

void Game::setTargetCursor(Vec2i p) {
    if (!targeting) return;
    p.x = clampi(p.x, 0, MAP_W - 1);
    p.y = clampi(p.y, 0, MAP_H - 1);
    targetPos = p;
    recomputeTargetLine();
}

void Game::moveLookCursor(int dx, int dy) {
    if (!looking) return;
    Vec2i p = lookPos;
    p.x = clampi(p.x + dx, 0, MAP_W - 1);
    p.y = clampi(p.y + dy, 0, MAP_H - 1);
    lookPos = p;
}

std::string Game::describeAt(Vec2i p) const {
    if (!dung.inBounds(p.x, p.y)) return "OUT OF BOUNDS";

    const Tile& t = dung.at(p.x, p.y);
    if (!t.explored) {
        return "UNKNOWN";
    }

    std::ostringstream ss;

    // Base tile description
    switch (t.type) {
        case TileType::Wall: ss << "WALL"; break;
        case TileType::DoorSecret: ss << "WALL"; break; // don't spoil undiscovered secrets
        case TileType::Floor: ss << "FLOOR"; break;
        case TileType::StairsUp: ss << "STAIRS UP"; break;
        case TileType::StairsDown: ss << "STAIRS DOWN"; break;
        case TileType::DoorClosed: ss << "DOOR (CLOSED)"; break;
        case TileType::DoorLocked: ss << "DOOR (LOCKED)"; break;
        case TileType::DoorOpen: ss << "DOOR (OPEN)"; break;
        default: ss << "TILE"; break;
    }

    // Trap (can be remembered once discovered)
    for (const auto& tr : trapsCur) {
        if (!tr.discovered) continue;
        if (tr.pos.x != p.x || tr.pos.y != p.y) continue;
        ss << " | TRAP: ";
        switch (tr.kind) {
            case TrapKind::Spike: ss << "SPIKE"; break;
            case TrapKind::PoisonDart: ss << "POISON DART"; break;
            case TrapKind::Teleport: ss << "TELEPORT"; break;
            case TrapKind::Alarm: ss << "ALARM"; break;
            case TrapKind::Web: ss << "WEB"; break;
        }
        break;
    }

    // Entities/items: only if currently visible.
    if (t.visible) {
        if (const Entity* e = entityAt(p.x, p.y)) {
            if (e->id == playerId_) {
                ss << " | YOU";
            } else {
                ss << " | " << kindName(e->kind) << " " << e->hp << "/" << e->hpMax;
            }
        }

        // Items (show first one + count)
        int itemCount = 0;
        const GroundItem* first = nullptr;
        for (const auto& gi : ground) {
            if (gi.pos.x == p.x && gi.pos.y == p.y) {
                ++itemCount;
                if (!first) first = &gi;
            }
        }
        if (itemCount > 0 && first) {
            std::string itemLabel = displayItemName(first->item);
            if (first->item.kind == ItemKind::Chest) {
                if (chestLocked(first->item)) itemLabel += " (LOCKED)";
                if (chestTrapped(first->item) && chestTrapKnown(first->item)) itemLabel += " (TRAPPED)";
            }
            ss << " | ITEM: " << itemLabel;
            if (itemCount > 1) ss << " (+" << (itemCount - 1) << ")";
        }
    }

    // Distance (Manhattan for clarity)
    Vec2i pp = player().pos;
    int dist = std::abs(p.x - pp.x) + std::abs(p.y - pp.y);
    ss << " | DIST " << dist;

    return ss.str();
}

std::string Game::lookInfoText() const {
    if (!looking) return std::string();
    return describeAt(lookPos);
}

void Game::restUntilSafe() {
    if (isFinished()) return;
    if (inputLock) return;

    // If nothing to do, don't burn time.
    if (player().hp >= player().hpMax) {
        pushMsg("YOU ARE ALREADY AT FULL HEALTH.", MessageKind::System, true);
        return;
    }

    pushMsg("YOU REST...", MessageKind::Info, true);

    // Safety valve to prevent accidental infinite loops.
    const int maxSteps = 2000;
    int steps = 0;
    while (!isFinished() && steps < maxSteps) {
        if (anyVisibleHostiles()) {
            pushMsg("REST INTERRUPTED!", MessageKind::Warning, true);
            break;
        }
        if (player().hp >= player().hpMax) {
            pushMsg("YOU FEEL RESTED.", MessageKind::Success, true);
            break;
        }

        // Consume a "wait" turn without spamming the log.
        advanceAfterPlayerAction();
        ++steps;
    }
}

bool Game::tryMove(Entity& e, int dx, int dy) {
    if (e.hp <= 0) return false;
    if (dx == 0 && dy == 0) return false;

    // Webbed: you can still act (use items, fire, etc.) but cannot move.
    // Attempting to move consumes a turn (so the web can wear off).
    if (e.kind == EntityKind::Player && e.webTurns > 0) {
        pushMsg("YOU STRUGGLE AGAINST STICKY WEBBING!", MessageKind::Warning, true);
        return true;
    }

    // Clamp to single-tile steps (safety: AI/pathing should only request these).
    dx = clampi(dx, -1, 1);
    dy = clampi(dy, -1, 1);

    const int nx = e.pos.x + dx;
    const int ny = e.pos.y + dy;

    if (!dung.inBounds(nx, ny)) return false;

    // Prevent diagonal corner-cutting (no slipping between two blocking tiles).
    if (dx != 0 && dy != 0 && !diagonalPassable(dung, e.pos, dx, dy)) {
        if (e.kind == EntityKind::Player) pushMsg("YOU CAN'T SQUEEZE THROUGH.");
        return false;
    }

    // Closed door: opening consumes a turn.
    if (dung.isDoorClosed(nx, ny)) {
        dung.openDoor(nx, ny);
        if (e.kind == EntityKind::Player) pushMsg("YOU OPEN THE DOOR.");
        return true;
    }

    // Locked door: keys open it instantly; lockpicks can work as a fallback.
    if (dung.isDoorLocked(nx, ny)) {
        if (e.kind != EntityKind::Player) {
            // Monsters can't open locked doors (for now).
            return false;
        }

        // Prefer keys (guaranteed).
        if (consumeKeys(1)) {
            dung.unlockDoor(nx, ny);
            dung.openDoor(nx, ny);
            pushMsg("YOU UNLOCK THE DOOR.", MessageKind::System, true);
            return true;
        }

        // No keys: attempt to pick the lock if you have lockpicks.
        if (lockpickCount() > 0) {
            // Success chance scales a bit with character level.
            float p = 0.55f + 0.03f * static_cast<float>(charLevel);
            p = std::min(0.85f, p);

            if (rng.chance(p)) {
                dung.unlockDoor(nx, ny);
                dung.openDoor(nx, ny);
                pushMsg("YOU PICK THE LOCK.", MessageKind::Success, true);
            } else {
                pushMsg("YOU FAIL TO PICK THE LOCK.", MessageKind::Warning, true);

                // Chance the pick breaks on a failed attempt.
                const float breakChance = 0.25f;
                if (rng.chance(breakChance)) {
                    consumeLockpicks(1);
                    pushMsg("YOUR LOCKPICK BREAKS!", MessageKind::Warning, true);
                }
            }
            return true; // picking takes a turn either way
        }

        pushMsg("THE DOOR IS LOCKED.", MessageKind::Warning, true);
        return false;
    }

    if (!dung.isWalkable(nx, ny)) {
        if (e.kind == EntityKind::Player) pushMsg("YOU BUMP INTO A WALL.");
        return false;
    }

    if (Entity* other = entityAtMut(nx, ny)) {
        if (other->id == e.id) return false;
        attackMelee(e, *other);
        return true;
    }

    e.pos.x = nx;
    e.pos.y = ny;

    if (e.kind == EntityKind::Player) {
        // Convenience / QoL: auto-pickup when stepping on items.
        if (autoPickup != AutoPickupMode::Off) {
            (void)autoPickupAtPlayer();
        }
        // Traps trigger on enter.
        triggerTrapAt(e.pos, e);
    }
    return true;
}

Trap* Game::trapAtMut(int x, int y) {
    for (auto& t : trapsCur) {
        if (t.pos.x == x && t.pos.y == y) return &t;
    }
    return nullptr;
}

void Game::triggerTrapAt(Vec2i pos, Entity& victim) {
    Trap* t = trapAtMut(pos.x, pos.y);
    if (!t) return;

    // At the moment, only the player meaningfully interacts with traps.
    if (victim.kind != EntityKind::Player) return;

    t->discovered = true;

    Entity& p = playerMut();

    switch (t->kind) {
        case TrapKind::Spike: {
            int dmg = rng.range(2, 5) + std::min(3, depth_ / 2);
            p.hp -= dmg;
            std::ostringstream ss;
            ss << "YOU STEP ON A SPIKE TRAP! YOU TAKE " << dmg << ".";
            pushMsg(ss.str(), MessageKind::Combat, false);
            if (p.hp <= 0) {
                pushMsg("YOU DIE.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = "KILLED BY SPIKE TRAP";
                gameOver = true;
            }
            break;
        }
        case TrapKind::PoisonDart: {
            int dmg = rng.range(1, 2);
            p.hp -= dmg;
            p.poisonTurns = std::max(p.poisonTurns, rng.range(6, 12));
            std::ostringstream ss;
            ss << "A POISON DART HITS YOU! YOU TAKE " << dmg << ".";
            pushMsg(ss.str(), MessageKind::Combat, false);
            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
            if (p.hp <= 0) {
                pushMsg("YOU DIE.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = "KILLED BY POISON DART TRAP";
                gameOver = true;
            }
            break;
        }
        case TrapKind::Teleport: {
            pushMsg("A TELEPORT TRAP ACTIVATES!", MessageKind::Warning, false);
            // Teleport the player to a random floor tile.
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
            pushMsg("AN ALARM BLARES!", MessageKind::Warning, false);
            for (auto& m : ents) {
                if (m.id != playerId_) m.alerted = true;
            }
            break;
        }
        case TrapKind::Web: {
            const int turns = rng.range(4, 7) + std::min(6, depth_ / 2);
            p.webTurns = std::max(p.webTurns, turns);
            pushMsg("YOU ARE CAUGHT IN STICKY WEBBING!", MessageKind::Warning, true);
            break;
        }
        default:
            break;
    }
}

bool Game::searchForTraps() {
    Entity& p = playerMut();
    const int radius = 2;

    int foundTraps = 0;
    int foundSecrets = 0;
    float baseChance = 0.35f + 0.05f * static_cast<float>(charLevel);
    baseChance = std::min(0.85f, baseChance);

    for (auto& t : trapsCur) {
        if (t.discovered) continue;
        int dx = std::abs(t.pos.x - p.pos.x);
        int dy = std::abs(t.pos.y - p.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > radius) continue;

        float chance = baseChance;
        if (cheb <= 1) chance = std::min(0.95f, chance + 0.20f);
        if (rng.chance(chance)) {
            t.discovered = true;
            foundTraps += 1;
        }
    }

    // Trapped chests behave like traps for detection purposes.
    for (auto& gi : ground) {
        if (gi.item.kind != ItemKind::Chest) continue;
        if (!chestTrapped(gi.item)) continue;
        if (chestTrapKnown(gi.item)) continue;

        int dx = std::abs(gi.pos.x - p.pos.x);
        int dy = std::abs(gi.pos.y - p.pos.y);
        int cheb = std::max(dx, dy);
        if (cheb > radius) continue;

        float chance = baseChance;
        if (cheb <= 1) chance = std::min(0.95f, chance + 0.20f);
        if (rng.chance(chance)) {
            setChestTrapKnown(gi.item, true);
            foundTraps += 1;
        }
    }

    // Also search for secret doors in nearby walls.
    // Secret doors are encoded as TileType::DoorSecret and behave like walls until discovered.
    for (int y = p.pos.y - radius; y <= p.pos.y + radius; ++y) {
        for (int x = p.pos.x - radius; x <= p.pos.x + radius; ++x) {
            if (!dung.inBounds(x, y)) continue;
            Tile& t = dung.at(x, y);
            if (t.type != TileType::DoorSecret) continue;

            int dx = std::abs(x - p.pos.x);
            int dy = std::abs(y - p.pos.y);
            int cheb = std::max(dx, dy);
            if (cheb > radius) continue;

            float chance = std::max(0.10f, baseChance - 0.10f); // slightly harder than traps
            if (cheb <= 1) chance = std::min(0.95f, chance + 0.20f);

            if (rng.chance(chance)) {
                t.type = TileType::DoorClosed;
                t.explored = true;
                foundSecrets += 1;
            }
        }
    }

    if (foundTraps > 0 || foundSecrets > 0) {
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
        pushMsg(ss.str(), MessageKind::Info, true);
    } else {
        pushMsg("YOU SEARCH, BUT FIND NOTHING.", MessageKind::Info, true);
    }

    return true; // Searching costs a turn.
}


bool Game::prayAtShrine(const std::string& modeIn) {
    if (gameOver || gameWon) return false;

    Entity& p = playerMut();

    // Must be standing inside a shrine room.
    bool inShrine = false;
    for (const Room& r : dung.rooms) {
        if (r.type == RoomType::Shrine && r.contains(p.pos.x, p.pos.y)) {
            inShrine = true;
            break;
        }
    }

    if (!inShrine) {
        pushMsg("YOU ARE NOT AT A SHRINE.", MessageKind::System, true);
        return false;
    }

    std::string mode = toLower(trim(modeIn));
    if (!mode.empty()) {
        if (!(mode == "heal" || mode == "cure" || mode == "identify" || mode == "bless")) {
            pushMsg("UNKNOWN PRAYER: " + mode + ". TRY: heal, cure, identify, bless.", MessageKind::System, true);
            return false;
        }
    } else {
        // Auto-pick the most useful effect right now.
        if (p.poisonTurns > 0 || p.webTurns > 0) mode = "cure";
        else if (p.hp < p.hpMax) mode = "heal";
        else if (identifyItemsEnabled) {
            bool hasUnknown = false;
            for (const auto& it : inv) {
                if (!isIdentifiableKind(it.kind)) continue;
                if (isIdentified(it.kind)) continue;
                hasUnknown = true;
                break;
            }
            mode = hasUnknown ? "identify" : "bless";
        } else {
            mode = "bless";
        }
    }

    // Pricing: scales gently with depth so it stays relevant.
    const int base = 8 + depth_ * 2;
    int cost = base;
    if (mode == "cure") cost = std::max(4, base - 2);
    else if (mode == "identify") cost = base + 6;
    else if (mode == "bless") cost = base + 10;

    if (goldCount() < cost) {
        pushMsg("YOU NEED " + std::to_string(cost) + " GOLD TO PRAY HERE.", MessageKind::Warning, true);
        return false;
    }

    // Spend gold from inventory stacks.
    int remaining = cost;
    for (auto& it : inv) {
        if (remaining <= 0) break;
        if (it.kind != ItemKind::Gold) continue;
        const int take = std::min(it.count, remaining);
        it.count -= take;
        remaining -= take;
    }
    inv.erase(std::remove_if(inv.begin(), inv.end(),
                             [](const Item& it) { return it.kind == ItemKind::Gold && it.count <= 0; }),
              inv.end());

    pushMsg("YOU OFFER " + std::to_string(cost) + " GOLD.", MessageKind::System);

    if (mode == "heal") {
        const int before = p.hp;
        p.hp = p.hpMax;
        if (p.hp > before) pushMsg("A WARM LIGHT MENDS YOUR WOUNDS.", MessageKind::Success, true);
        else pushMsg("YOU FEEL REASSURED.", MessageKind::Info, true);
    } else if (mode == "cure") {
        const bool hadPoison = (p.poisonTurns > 0);
        const bool hadWeb = (p.webTurns > 0);
        p.poisonTurns = 0;
        p.webTurns = 0;
        if (hadPoison || hadWeb) pushMsg("YOU FEEL PURIFIED.", MessageKind::Success, true);
        else pushMsg("NOTHING SEEMS AMISS.", MessageKind::Info, true);
    } else if (mode == "identify") {
        if (!identifyItemsEnabled) {
            pushMsg("THE SHRINE IS SILENT. (IDENTIFY ITEMS IS OFF.)", MessageKind::Info, true);
        } else {
            std::vector<ItemKind> candidates;
            candidates.reserve(inv.size());
            for (const auto& it : inv) {
                if (!isIdentifiableKind(it.kind)) continue;
                if (isIdentified(it.kind)) continue;
                candidates.push_back(it.kind);
            }

            if (candidates.empty()) {
                pushMsg("NOTHING NEW IS REVEALED.", MessageKind::Info, true);
            } else {
                ItemKind k = candidates[static_cast<size_t>(rng.range(0, static_cast<int>(candidates.size()) - 1))];
                (void)markIdentified(k, false);
                pushMsg("DIVINE INSIGHT REVEALS THE TRUTH.", MessageKind::Info, true);
            }
        }
    } else { // bless
        p.shieldTurns = std::max(p.shieldTurns, 18 + depth_ * 2);
        p.regenTurns = std::max(p.regenTurns, 10 + depth_);
        pushMsg("A HOLY AURA SURROUNDS YOU.", MessageKind::Success, true);
    }

    // Praying consumes a turn.
    advanceAfterPlayerAction();
    return true;
}


void Game::attackMelee(Entity& attacker, Entity& defender) {
    int atk = attacker.baseAtk;
    int def = defender.baseDef;

    if (attacker.kind == EntityKind::Player) atk = playerAttack();
    if (defender.kind == EntityKind::Player) def = playerDefense();

    int dmg = std::max(1, atk - def + rng.range(0, 1));
    // Small crit chance for spicy combat.
    if (rng.chance(0.10f)) {
        dmg += std::max(1, dmg / 2);
    }
    defender.hp -= dmg;

    std::ostringstream ss;
    if (attacker.kind == EntityKind::Player) {
        ss << "YOU HIT " << kindName(defender.kind) << " FOR " << dmg << ".";
    } else if (defender.kind == EntityKind::Player) {
        ss << kindName(attacker.kind) << " HITS YOU FOR " << dmg << ".";
    } else {
        ss << kindName(attacker.kind) << " HITS " << kindName(defender.kind) << ".";
    }
    const bool msgFromPlayer = (attacker.kind == EntityKind::Player);
    pushMsg(ss.str(), MessageKind::Combat, msgFromPlayer);

    // Monster special effects.
    if (defender.hp > 0 && defender.kind == EntityKind::Player) {
        if (attacker.kind == EntityKind::Snake && rng.chance(0.35f)) {
            defender.poisonTurns = std::max(defender.poisonTurns, rng.range(4, 8));
            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
        }
        if (attacker.kind == EntityKind::Spider && rng.chance(0.45f)) {
            defender.webTurns = std::max(defender.webTurns, rng.range(2, 4));
            pushMsg("YOU ARE ENSNARED BY WEBBING!", MessageKind::Warning, false);
        }
    }

    if (defender.hp <= 0) {
        if (defender.kind == EntityKind::Player) {
            pushMsg("YOU DIE.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = std::string("KILLED BY ") + kindName(attacker.kind);
            gameOver = true;
        } else {
            std::ostringstream ds;
            ds << kindName(defender.kind) << " DIES.";
            pushMsg(ds.str(), MessageKind::Combat, msgFromPlayer);

            if (attacker.kind == EntityKind::Player) {
                ++killCount;
                grantXp(xpFor(defender.kind));
            }
        }
    }
}

std::vector<Vec2i> Game::bresenhamLine(Vec2i a, Vec2i b) {
    std::vector<Vec2i> pts;
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;

    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        pts.push_back({x0, y0});
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
        if (pts.size() > 512) break;
    }
    return pts;
}

void Game::attackRanged(Entity& attacker, Vec2i target, int range, int atk, ProjectileKind projKind, bool fromPlayer) {
    std::vector<Vec2i> line = bresenhamLine(attacker.pos, target);
    if (line.size() <= 1) return;

    // Clamp to range (+ start tile)
    if (range > 0 && static_cast<int>(line.size()) > range + 1) {
        line.resize(static_cast<size_t>(range + 1));
    }

    bool hitEntity = false;
    bool hitWall = false;
    Entity* hit = nullptr;

    size_t stopIdx = line.size() - 1;

    for (size_t i = 1; i < line.size(); ++i) {
        Vec2i p = line[i];
        if (!dung.inBounds(p.x, p.y)) { stopIdx = i - 1; break; }

        // Walls/closed doors block projectiles
        if (dung.isOpaque(p.x, p.y)) {
            hitWall = true;
            stopIdx = i;
            break;
        }

        if (Entity* e = entityAtMut(p.x, p.y)) {
            if (e->id != attacker.id && e->hp > 0) {
                hitEntity = true;
                hit = e;
                stopIdx = i;
                break;
            }
        }
    }

    // Apply damage immediately (visual projectile is FX only).
    if (hitEntity && hit) {
        int def = hit->baseDef;
        if (hit->kind == EntityKind::Player) def = playerDefense();

        int dmg = std::max(1, atk - def + rng.range(0, 1));
        hit->hp -= dmg;

        std::ostringstream ss;
        if (fromPlayer) {
            ss << "YOU HIT " << kindName(hit->kind) << " FOR " << dmg << ".";
        } else if (hit->kind == EntityKind::Player) {
            ss << kindName(attacker.kind) << " HITS YOU FOR " << dmg << ".";
        } else {
            ss << kindName(attacker.kind) << " HITS " << kindName(hit->kind) << ".";
        }
        pushMsg(ss.str(), MessageKind::Combat, fromPlayer);

        if (hit->hp <= 0) {
            if (hit->kind == EntityKind::Player) {
                pushMsg("YOU DIE.", MessageKind::Combat, false);
                if (endCause_.empty()) endCause_ = std::string("KILLED BY ") + kindName(attacker.kind);
                gameOver = true;
            } else {
                std::ostringstream ds;
                ds << kindName(hit->kind) << " DIES.";
                pushMsg(ds.str(), MessageKind::Combat, fromPlayer);
                if (fromPlayer) {
                    ++killCount;
                    grantXp(xpFor(hit->kind));
                }
            }
        }
    } else if (hitWall) {
        if (fromPlayer) pushMsg("THE SHOT HITS A WALL.", MessageKind::Warning, true);
    } else {
        if (fromPlayer) pushMsg("YOU FIRE.", MessageKind::Combat, true);
    }

    // FX projectile path (truncate)
    std::vector<Vec2i> fxPath;
    fxPath.reserve(stopIdx + 1);
    for (size_t i = 0; i <= stopIdx && i < line.size(); ++i) fxPath.push_back(line[i]);

    FXProjectile fxp;
    fxp.kind = projKind;
    fxp.path = std::move(fxPath);
    fxp.pathIndex = (fxp.path.size() > 1) ? 1 : 0;
    fxp.stepTimer = 0.0f;
    fxp.stepTime = (projKind == ProjectileKind::Spark) ? 0.02f : 0.03f;
    fx.push_back(std::move(fxp));

    inputLock = true;
}

void Game::recomputeFov() {
    Entity& p = playerMut();
    int radius = 9;
    if (p.visionTurns > 0) radius += 3;
    dung.computeFov(p.pos.x, p.pos.y, radius);
}

void Game::openInventory() {
    // Close other overlays
    targeting = false;
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    invOpen = true;
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
}

void Game::closeInventory() {
    invOpen = false;
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
        if (it.id == equipMeleeId || it.id == equipRangedId || it.id == equipArmorId) return 1;

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

    auto shouldPick = [&](const Item& it) -> bool {
        // Chests are world-interactables; never auto-pickup.
        if (isChestKind(it.kind)) return false;
        if (autoPickup == AutoPickupMode::Gold) return it.kind == ItemKind::Gold;
        // AutoPickupMode::All
        return true;
    };

    int pickedCount = 0;
    std::vector<std::string> sampleNames;

    for (size_t i = 0; i < ground.size();) {
        if (ground[i].pos == pos && shouldPick(ground[i].item)) {
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

    // Locked chest: consume a key or attempt lockpick.
    if (chestLocked(chest)) {
        if (keyCount() > 0) {
            (void)consumeKeys(1);
            setChestLocked(chest, false);
            pushMsg("YOU UNLOCK THE CHEST.", MessageKind::Info, true);
        } else if (lockpickCount() > 0) {
            // Lockpicking chance scales with character level, but higher-tier chests are harder.
            float chance = 0.35f + 0.05f * static_cast<float>(charLevel);
            chance -= 0.05f * static_cast<float>(chestTier(chest));
            chance = clampf(chance, 0.15f, 0.95f);

            if (rng.chance(chance)) {
                setChestLocked(chest, false);
                pushMsg("YOU PICK THE CHEST'S LOCK.", MessageKind::Info, true);
            } else {
                // Failed pick still costs a turn.
                pushMsg("YOU FAIL TO PICK THE CHEST'S LOCK.", MessageKind::Info, true);
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

    // Trigger trap if present.
    if (chestTrapped(chest)) {
        const TrapKind tk = chestTrapKind(chest);
        setChestTrapped(chest, false);
        setChestTrapKnown(chest, true);

        Entity& p = playerMut();
        switch (tk) {
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
                p.poisonTurns = std::max(p.poisonTurns, rng.range(6, 12));
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
                for (auto& m : ents) {
                    if (m.id != playerId_) m.alerted = true;
                }
                break;
            }
            case TrapKind::Web: {
                const int turns = rng.range(4, 7) + std::min(6, depth_ / 2);
                p.webTurns = std::max(p.webTurns, turns);
                pushMsg("STICKY WEBBING EXPLODES OUT OF THE CHEST!", MessageKind::Warning, true);
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
        if (k == ItemKind::WandSparks) it.charges = itemDef(k).maxCharges;
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
        int roll = rng.range(0, 139);

        if (roll < 16) {
            // Weapons
            ItemKind wk = (roll < 8) ? ItemKind::Sword : ItemKind::Axe;
            int ench = (rng.chance(0.25f + 0.10f * tier)) ? rng.range(1, 1 + tier) : 0;
            dropItemHere(wk, 1, ench);
        } else if (roll < 34) {
            // Armor
            ItemKind ak = (roll < 26) ? ItemKind::ChainArmor : ItemKind::PlateArmor;
            int ench = (rng.chance(0.25f + 0.10f * tier)) ? rng.range(1, 1 + tier) : 0;
            dropItemHere(ak, 1, ench);
        } else if (roll < 48) {
            dropItemHere(ItemKind::WandSparks, 1);
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
            dropItemHere(ItemKind::PotionVision, 1);
        } else if (roll < 130) {
            dropItemHere(ItemKind::ScrollMapping, 1);
        } else if (roll < 134) {
            dropItemHere(ItemKind::ScrollTeleport, 1);
        } else if (roll < 136) {
            dropItemHere(ItemKind::ScrollEnchantWeapon, 1);
        } else if (roll < 138) {
            dropItemHere(ItemKind::ScrollEnchantArmor, 1);
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

        if (tryStackItem(inv, it)) {
            // stacked
            pickedAny = true;
            pushMsg("YOU PICK UP " + displayItemName(it) + ".", MessageKind::Loot, true);
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
        pushMsg("YOU PICK UP " + displayItemName(it) + ".", MessageKind::Loot, true);
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

    // Unequip if needed
    if (it.id == equipMeleeId) equipMeleeId = 0;
    if (it.id == equipRangedId) equipRangedId = 0;
    if (it.id == equipArmorId) equipArmorId = 0;

    Item drop = it;
    if (isStackable(it.kind) && it.count > 1) {
        drop.count = 1;
        it.count -= 1;
    } else {
        // remove whole item
        inv.erase(inv.begin() + invSel);
        invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    }

    GroundItem gi;
    gi.item = drop;
    gi.pos = player().pos;
    ground.push_back(gi);

    pushMsg("YOU DROP " + displayItemName(drop) + ".");
    return true;
}

bool Game::dropSelectedAll() {
    if (inv.empty()) {
        pushMsg("NOTHING TO DROP.");
        return false;
    }

    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    // Unequip if needed
    if (it.id == equipMeleeId) equipMeleeId = 0;
    if (it.id == equipRangedId) equipRangedId = 0;
    if (it.id == equipArmorId) equipArmorId = 0;

    Item drop = it;

    // Remove whole item/stack.
    inv.erase(inv.begin() + invSel);
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    GroundItem gi;
    gi.item = drop;
    gi.pos = player().pos;
    ground.push_back(gi);

    pushMsg("YOU DROP " + displayItemName(drop) + ".");
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

    if (d.slot == EquipSlot::MeleeWeapon) {
        if (equipMeleeId == it.id) {
            equipMeleeId = 0;
            pushMsg("YOU UNWIELD " + displayItemName(it) + ".");
        } else {
            equipMeleeId = it.id;
            pushMsg("YOU WIELD " + displayItemName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::RangedWeapon) {
        if (equipRangedId == it.id) {
            equipRangedId = 0;
            pushMsg("YOU UNEQUIP " + displayItemName(it) + ".");
        } else {
            equipRangedId = it.id;
            pushMsg("YOU READY " + displayItemName(it) + ".");
        }
        return true;
    }
    if (d.slot == EquipSlot::Armor) {
        if (equipArmorId == it.id) {
            equipArmorId = 0;
            pushMsg("YOU REMOVE " + displayItemName(it) + ".");
        } else {
            equipArmorId = it.id;
            pushMsg("YOU WEAR " + displayItemName(it) + ".");
        }
        return true;
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
        Entity& p = playerMut();
        int heal = itemDef(it.kind).healAmount;
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
        Entity& p = playerMut();
        p.baseAtk += 1;
        std::ostringstream ss;
        ss << "YOU FEEL STRONGER! ATK IS NOW " << p.baseAtk << ".";
        pushMsg(ss.str(), MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollTeleport) {
        // Teleport to a random free floor
        for (int tries = 0; tries < 2000; ++tries) {
            Vec2i p = dung.randomFloor(rng, true);
            if (entityAt(p.x, p.y)) continue;
            playerMut().pos = p;
            break;
        }
        pushMsg("YOU READ A SCROLL. YOU VANISH!", MessageKind::Info, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        recomputeFov();
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

        Entity& p = playerMut();
        const int radius = 6;
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

    if (it.kind == ItemKind::PotionAntidote) {
        Entity& p = playerMut();
        if (p.poisonTurns > 0) {
            p.poisonTurns = 0;
            pushMsg("YOU FEEL THE POISON LEAVE YOUR BODY.", MessageKind::Success, true);
        } else {
            pushMsg("YOU FEEL CLEAN.", MessageKind::Info, true);
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionRegeneration) {
        Entity& p = playerMut();
        p.regenTurns = std::max(p.regenTurns, 18);
        pushMsg("YOUR WOUNDS BEGIN TO KNIT.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionShielding) {
        Entity& p = playerMut();
        p.shieldTurns = std::max(p.shieldTurns, 14);
        pushMsg("YOU FEEL PROTECTED.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionHaste) {
        Entity& p = playerMut();
        p.hasteTurns = std::min(40, p.hasteTurns + 6);
        hastePhase = false; // ensure the next action is the "free" haste action
        pushMsg("YOU FEEL QUICK!", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionVision) {
        Entity& p = playerMut();
        p.visionTurns = std::min(60, p.visionTurns + 20);
        pushMsg("YOUR EYES SHINE WITH INNER LIGHT.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
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
        // Using an identify scroll reveals the true name of one random unidentified potion/scroll.
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
        } else {
            ItemKind k = candidates[static_cast<size_t>(rng.range(0, static_cast<int>(candidates.size()) - 1))];
            (void)markIdentified(k, false);
        }

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::FoodRation) {
        Entity& p = playerMut();
        const ItemDef& d = itemDef(it.kind);

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

    pushMsg("NOTHING HAPPENS.", MessageKind::Info, true);
    return false;
}

void Game::beginTargeting() {
    std::string reason;
    if (!playerHasRangedReady(&reason)) {
        pushMsg(reason);
        return;
    }
    targeting = true;
    invOpen = false;
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;
    targetPos = player().pos;
    recomputeTargetLine();
    pushMsg("TARGETING...");
}

void Game::endTargeting(bool fire) {
    if (!targeting) return;

    if (fire) {
        if (!targetValid) {
            pushMsg("NO CLEAR SHOT.");
        } else {
            int wIdx = equippedRangedIndex();
            if (wIdx < 0) {
                pushMsg("NO RANGED WEAPON.");
            } else {
                Item& w = inv[static_cast<size_t>(wIdx)];
                const ItemDef& d = itemDef(w.kind);

                // Re-check readiness (ammo/charges) to be safe.
                if (d.maxCharges > 0 && w.charges <= 0) {
                    pushMsg("THE WAND IS OUT OF CHARGES.");
                } else if (d.ammo != AmmoKind::None && ammoCount(inv, d.ammo) <= 0) {
                    pushMsg(d.ammo == AmmoKind::Arrow ? "NO ARROWS." : "NO ROCKS.");
                } else {
                    // Consume charge/ammo.
                    if (d.maxCharges > 0) {
                        w.charges -= 1;
                    }
                    if (d.ammo != AmmoKind::None) {
                        consumeAmmo(inv, d.ammo, 1);
                    }

                    // Compute attack.
            int atk = std::max(1, player().baseAtk + d.rangedAtk + w.enchant + rng.range(0, 1));
                    if (w.kind == ItemKind::WandSparks) {
                        atk += 2 + rng.range(0, 2);
                    }

                    attackRanged(playerMut(), targetPos, d.range, atk, d.projectile, true);

                    if (w.kind == ItemKind::WandSparks && w.charges <= 0) {
                        pushMsg("YOUR WAND SPUTTERS OUT.");
                    }
                }
            }
        }
    }

    targeting = false;
    targetLine.clear();
    targetValid = false;
}


void Game::moveTargetCursor(int dx, int dy) {
    if (!targeting) return;
    Vec2i p = targetPos;
    p.x = clampi(p.x + dx, 0, MAP_W - 1);
    p.y = clampi(p.y + dy, 0, MAP_H - 1);
    setTargetCursor(p);
}

void Game::recomputeTargetLine() {
    targetLine = bresenhamLine(player().pos, targetPos);

    // Clamp to range
    int range = playerRangedRange();
    if (range > 0 && static_cast<int>(targetLine.size()) > range + 1) {
        targetLine.resize(static_cast<size_t>(range + 1));
    }

    // Determine validity: must have LOS and be within visible tiles (you can't target what you can't see).
    targetValid = false;

    if (!dung.inBounds(targetPos.x, targetPos.y)) return;
    if (!dung.at(targetPos.x, targetPos.y).visible) return;

    // Verify LOS along clamped line (stop at opaque).
    for (size_t i = 1; i < targetLine.size(); ++i) {
        Vec2i p = targetLine[i];
        if (dung.isOpaque(p.x, p.y)) {
            // If the target is behind an opaque tile, invalid.
            if (p != targetPos) return;
        }
    }

    // Must be within range (by path length)
    if (range > 0) {
        int dist = static_cast<int>(targetLine.size()) - 1;
        if (dist > range) return;
    }

    // Weapon ready?
    std::string reason;
    if (!playerHasRangedReady(&reason)) return;

    targetValid = true;
}

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

    int nextGroup = 1;

    auto addMonster = [&](EntityKind k, Vec2i pos, int groupId) {
    Entity e;
    e.id = nextEntityId++;
    e.kind = k;
    e.pos = pos;
    e.spriteSeed = rng.nextU32();
    e.groupId = groupId;

    switch (k) {
        case EntityKind::Goblin:
            e.hpMax = 7; e.baseAtk = 2; e.baseDef = 0;
            e.willFlee = true;
            break;
        case EntityKind::Orc:
            e.hpMax = 12; e.baseAtk = 3; e.baseDef = 1;
            break;
        case EntityKind::Bat:
            e.hpMax = 5; e.baseAtk = 1; e.baseDef = 0;
            e.willFlee = true;
            break;
        case EntityKind::Slime:
            e.hpMax = 10; e.baseAtk = 2; e.baseDef = 1;
            e.willFlee = false;
            break;
        case EntityKind::SkeletonArcher:
            e.hpMax = 10; e.baseAtk = 2; e.baseDef = 1;
            e.canRanged = true; e.rangedRange = 8; e.rangedAtk = 3;
            e.rangedAmmo = AmmoKind::Arrow;
            e.rangedProjectile = ProjectileKind::Arrow;
            break;
        case EntityKind::KoboldSlinger:
            e.hpMax = 8; e.baseAtk = 2; e.baseDef = 0;
            e.canRanged = true; e.rangedRange = 6; e.rangedAtk = 2;
            e.rangedAmmo = AmmoKind::Rock;
            e.rangedProjectile = ProjectileKind::Rock;
            e.willFlee = true;
            break;
        case EntityKind::Wolf:
            e.hpMax = 10; e.baseAtk = 3; e.baseDef = 0;
            e.packAI = true;
            break;
        case EntityKind::Troll:
            e.hpMax = 16; e.baseAtk = 4; e.baseDef = 1;
            e.willFlee = false;
            e.regenChancePct = 40;
            e.regenAmount = 1;
            break;
        case EntityKind::Wizard:
            e.hpMax = 12; e.baseAtk = 2; e.baseDef = 1;
            e.canRanged = true; e.rangedRange = 7; e.rangedAtk = 4;
            e.rangedAmmo = AmmoKind::None;
            e.rangedProjectile = ProjectileKind::Spark;
            e.willFlee = true;
            break;
        case EntityKind::Snake:
            e.hpMax = 7; e.baseAtk = 2; e.baseDef = 0;
            e.willFlee = false;
            break;
        case EntityKind::Spider:
            e.hpMax = 8; e.baseAtk = 2; e.baseDef = 1;
            e.willFlee = false;
            break;
        case EntityKind::Ogre:
            e.hpMax = 20; e.baseAtk = 5; e.baseDef = 2;
            e.willFlee = false;
            break;
        default:
            e.hpMax = 6; e.baseAtk = 2; e.baseDef = 0;
            break;
    }

    // A small amount of depth scaling.
    int d = std::max(0, depth_ - 1);
    if (d > 0 && k != EntityKind::Player) {
        e.hpMax += d;
        e.baseAtk += d / 3;
        e.baseDef += d / 4;
    }

    e.hp = e.hpMax;
    ents.push_back(e);
};

    // Spawn per room, scaling with level.
    for (size_t i = 0; i < rooms.size(); ++i) {
        const Room& r = rooms[i];

        // Don't spawn in the starting room too aggressively.
        bool isStart = (r.contains(dung.stairsUp.x, dung.stairsUp.y));

        int base = isStart ? 0 : 1;
        if (r.type == RoomType::Secret || r.type == RoomType::Vault) base = 0;
        int n = rng.range(0, base + (depth_ >= 3 ? 2 : 1));
        if (r.type == RoomType::Vault) {
            // Vaults are locked side rooms; keep them dangerous but not overcrowded.
            n = rng.range(0, 1);
        }

        if (r.type == RoomType::Lair && !isStart) {
            // Pack spawns
            int pack = rng.range(3, 5);
            int gid = nextGroup++;
            for (int k = 0; k < pack; ++k) {
                Vec2i p = randomFreeTileInRoom(r);
                addMonster(EntityKind::Wolf, p, gid);
            }
            continue;
        }

        for (int m = 0; m < n; ++m) {
            Vec2i p = randomFreeTileInRoom(r);
            // Choose kind based on level.
            int roll = rng.range(0, 99);
            EntityKind k = EntityKind::Goblin;

            if (depth_ <= 1) {
                if (roll < 40) k = EntityKind::Goblin;
                else if (roll < 60) k = EntityKind::Bat;
                else if (roll < 75) k = EntityKind::Slime;
                else if (roll < 85) k = EntityKind::Snake;
                else k = EntityKind::KoboldSlinger;
            } else if (depth_ == 2) {
                if (roll < 25) k = EntityKind::Goblin;
                else if (roll < 45) k = EntityKind::KoboldSlinger;
                else if (roll < 60) k = EntityKind::Snake;
                else if (roll < 75) k = EntityKind::SkeletonArcher;
                else if (roll < 87) k = EntityKind::Slime;
                else if (roll < 95) k = EntityKind::Orc;
                else k = EntityKind::Spider;
            } else {
                if (depth_ >= 4) {
                    if (roll < 18) k = EntityKind::Orc;
                    else if (roll < 30) k = EntityKind::SkeletonArcher;
                    else if (roll < 42) k = EntityKind::Spider;
                    else if (roll < 52) k = EntityKind::Goblin;
                    else if (roll < 62) k = EntityKind::KoboldSlinger;
                    else if (roll < 72) k = EntityKind::Slime;
                    else if (roll < 80) k = EntityKind::Wolf;
                    else if (roll < 88) k = EntityKind::Bat;
                    else if (roll < 94) k = EntityKind::Snake;
                    else if (roll < 97) k = EntityKind::Troll;
                    else if (roll < 99) k = EntityKind::Ogre;
                    else k = EntityKind::Wizard;
                } else {
                    // depth_ == 3
                    if (roll < 22) k = EntityKind::Orc;
                    else if (roll < 40) k = EntityKind::SkeletonArcher;
                    else if (roll < 52) k = EntityKind::Wolf;
                    else if (roll < 64) k = EntityKind::Goblin;
                    else if (roll < 75) k = EntityKind::KoboldSlinger;
                    else if (roll < 84) k = EntityKind::Slime;
                    else if (roll < 92) k = EntityKind::Snake;
                    else if (roll < 97) k = EntityKind::Bat;
                    else k = EntityKind::Spider;
                }
            }

            addMonster(k, p, 0);
        }

        // Treasure/bonus rooms get a guardian sometimes.
        if ((r.type == RoomType::Treasure || r.type == RoomType::Secret || r.type == RoomType::Vault) && !isStart) {
            float chance = 0.60f;
            if (r.type == RoomType::Secret) chance = 0.75f;
            if (r.type == RoomType::Vault)  chance = 0.85f;
            if (!rng.chance(chance)) continue;
            Vec2i p = randomFreeTileInRoom(r);
            EntityKind g = EntityKind::Goblin;
            if (depth_ >= 4) {
                int gr = rng.range(0, 99);
                if (gr < 25) g = EntityKind::Wizard;
                else if (gr < 55) g = EntityKind::Ogre;
                else g = EntityKind::Troll;
            } else if (depth_ >= 3) {
                g = EntityKind::Orc;
            } else {
                g = EntityKind::Goblin;
            }
            if (r.type == RoomType::Vault && depth_ >= 2 && depth_ < 3) {
                g = EntityKind::Orc;
            }
            addMonster(g, p, 0);
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
        if (k == ItemKind::WandSparks) it.charges = itemDef(k).maxCharges;

        GroundItem gi;
        gi.item = it;
        gi.pos = pos;
        ground.push_back(gi);
    };

    auto dropGoodItem = [&](const Room& r) {
        // Treasure rooms are where you find the "spicy" gear.
        int roll = rng.range(0, 135);

        if (roll < 18) dropItemAt(ItemKind::Sword, randomFreeTileInRoom(r));
        else if (roll < 30) dropItemAt(ItemKind::Axe, randomFreeTileInRoom(r));
        else if (roll < 44) dropItemAt(ItemKind::ChainArmor, randomFreeTileInRoom(r));
        else if (roll < 50) dropItemAt(ItemKind::PlateArmor, randomFreeTileInRoom(r));
        else if (roll < 62) dropItemAt(ItemKind::WandSparks, randomFreeTileInRoom(r));
        else if (roll < 72) dropItemAt(ItemKind::Sling, randomFreeTileInRoom(r));
        else if (roll < 84) dropItemAt(ItemKind::PotionStrength, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 96) dropItemAt(ItemKind::PotionHealing, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 106) dropItemAt(ItemKind::PotionAntidote, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 114) dropItemAt(ItemKind::PotionRegeneration, randomFreeTileInRoom(r), 1);
        else if (roll < 118) dropItemAt(ItemKind::PotionShielding, randomFreeTileInRoom(r), 1);
        else if (roll < 122) dropItemAt(ItemKind::PotionHaste, randomFreeTileInRoom(r), 1);
        else if (roll < 126) dropItemAt(ItemKind::PotionVision, randomFreeTileInRoom(r), 1);
        else if (roll < 129) dropItemAt(ItemKind::ScrollMapping, randomFreeTileInRoom(r), 1);
        else if (roll < 131) {
            int pick = rng.range(0, 3);
            ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                      : (pick == 1) ? ItemKind::ScrollDetectTraps
                                      : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                    : ItemKind::ScrollKnock;
            dropItemAt(sk, randomFreeTileInRoom(r), 1);
        }
        else if (roll < 133) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
        else if (roll < 135) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
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
        if (r < 32) return TrapKind::PoisonDart;
        if (r < 58) return TrapKind::Alarm;
        if (r < 82) return TrapKind::Web;
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
            if (rng.chance(0.15f)) dropItemAt(ItemKind::PotionVision, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.18f)) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.12f)) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.20f)) {
                int pick = rng.range(0, 3);
                ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                          : (pick == 1) ? ItemKind::ScrollDetectTraps
                                          : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                        : ItemKind::ScrollKnock;
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
            int roll = rng.range(0, 107);

            if (roll < 22) dropItemAt(ItemKind::Gold, p, rng.range(3, 10));
            else if (roll < 30) dropItemAt(ItemKind::FoodRation, p, 1);
            else if (roll < 44) dropItemAt(ItemKind::PotionHealing, p, 1);
            else if (roll < 54) dropItemAt(ItemKind::PotionStrength, p, 1);
            else if (roll < 62) dropItemAt(ItemKind::PotionAntidote, p, 1);
            else if (roll < 68) dropItemAt(ItemKind::PotionRegeneration, p, 1);
            else if (roll < 74) dropItemAt(ItemKind::ScrollTeleport, p, 1);
            else if (roll < 80) dropItemAt(ItemKind::ScrollMapping, p, 1);
            else if (roll < 82) {
                int pick = rng.range(0, 3);
                ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                          : (pick == 1) ? ItemKind::ScrollDetectTraps
                                          : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                        : ItemKind::ScrollKnock;
                dropItemAt(sk, p, 1);
            }
            else if (roll < 86) dropItemAt(ItemKind::ScrollEnchantWeapon, p, 1);
            else if (roll < 90) dropItemAt(ItemKind::ScrollEnchantArmor, p, 1);
            else if (roll < 95) dropItemAt(ItemKind::Arrow, p, rng.range(4, 10));
            else if (roll < 100) dropItemAt(ItemKind::Rock, p, rng.range(3, 8));
            else if (roll < 103) dropItemAt(ItemKind::Dagger, p, 1);
            else if (roll < 105) dropItemAt(ItemKind::LeatherArmor, p, 1);
            else if (roll < 106) dropItemAt(ItemKind::PotionShielding, p, 1);
            else if (roll < 107) dropItemAt(ItemKind::PotionHaste, p, 1);
            else dropItemAt(ItemKind::PotionVision, p, 1);
        }
    }

    // Guarantee at least one key on any floor that contains locked doors.
    if (hasLockedDoor && keysPlacedThisFloor <= 0) {
        std::vector<const Room*> candidates;
        candidates.reserve(rooms.size());
        for (const Room& r : rooms) {
            if (r.type == RoomType::Vault) continue; // don't hide keys behind locked doors
            if (r.type == RoomType::Secret) continue; // keep the guarantee discoverable without searching
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
            if (r.type == RoomType::Secret) continue;  // keep the guarantee discoverable without searching
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


    // Quest objective: place the Amulet of Yendor on depth 5.
    if (depth_ == 5 && !playerHasAmulet()) {
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
            Vec2i pos = tr ? randomFreeTileInRoom(*tr) : dung.stairsDown;
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
    const int targetCount = base + depthBonus + rng.range(0, 2);

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
        if (depth_ <= 1) {
            tk = (roll < 70) ? TrapKind::Spike : TrapKind::PoisonDart;
        } else if (depth_ <= 3) {
            if (roll < 45) tk = TrapKind::Spike;
            else if (roll < 75) tk = TrapKind::PoisonDart;
            else if (roll < 88) tk = TrapKind::Alarm;
            else if (roll < 94) tk = TrapKind::Web;
            else tk = TrapKind::Teleport;
        } else {
            if (roll < 35) tk = TrapKind::Spike;
            else if (roll < 65) tk = TrapKind::PoisonDart;
            else if (roll < 82) tk = TrapKind::Alarm;
            else if (roll < 92) tk = TrapKind::Web;
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
            // Bias toward alarm/poison on doors (fits the theme).
            t.kind = rng.chance(0.55f) ? TrapKind::Alarm : TrapKind::PoisonDart;
            trapsCur.push_back(t);
        }
    }

}

void Game::monsterTurn() {
    if (gameOver) return;

    const Entity& p = player();
    const int W = dung.width;
    const int H = dung.height;

    // Build distance map from player (passable tiles).
    std::vector<int> dist(static_cast<size_t>(W * H), -1);
    auto idx = [&](int x, int y) { return y * W + x; };

    std::deque<Vec2i> q;
    dist[static_cast<size_t>(idx(p.pos.x, p.pos.y))] = 0;
    q.push_back(p.pos);

    const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

    while (!q.empty()) {
        Vec2i cur = q.front();
        q.pop_front();
        int cd = dist[static_cast<size_t>(idx(cur.x, cur.y))];
        for (auto& dv : dirs) {
            int dx = dv[0], dy = dv[1];
            int nx = cur.x + dx;
            int ny = cur.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, cur, dx, dy)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (dist[static_cast<size_t>(idx(nx, ny))] != -1) continue;
            dist[static_cast<size_t>(idx(nx, ny))] = cd + 1;
            q.push_back({nx, ny});
        }
    }

    // Helper to choose move based on dist map.
    auto stepToward = [&](const Entity& m) -> Vec2i {
        Vec2i best = m.pos;
        int bestD = 1000000000;
        for (auto& dv : dirs) {
            int dx = dv[0], dy = dv[1];
            int nx = m.pos.x + dx;
            int ny = m.pos.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, m.pos, dx, dy)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (entityAt(nx, ny) && !(nx == p.pos.x && ny == p.pos.y)) continue;
            int d0 = dist[static_cast<size_t>(idx(nx, ny))];
            if (d0 >= 0 && d0 < bestD) {
                bestD = d0;
                best = {nx, ny};
            }
        }
        return best;
    };

    auto stepAway = [&](const Entity& m) -> Vec2i {
        Vec2i best = m.pos;
        int bestD = -1;
        for (auto& dv : dirs) {
            int dx = dv[0], dy = dv[1];
            int nx = m.pos.x + dx;
            int ny = m.pos.y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dx != 0 && dy != 0 && !diagonalPassable(dung, m.pos, dx, dy)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (entityAt(nx, ny)) continue;
            int d0 = dist[static_cast<size_t>(idx(nx, ny))];
            if (d0 >= 0 && d0 > bestD) {
                bestD = d0;
                best = {nx, ny};
            }
        }
        return best;
    };

    for (auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;

        int d0 = dist[static_cast<size_t>(idx(m.pos.x, m.pos.y))];
        int man = manhattan(m.pos, p.pos);

        bool seesPlayer = false;
        if (man <= 12) {
            seesPlayer = dung.hasLineOfSight(m.pos.x, m.pos.y, p.pos.x, p.pos.y);
        }

        if (seesPlayer) m.alerted = true;

        if (!m.alerted) {
            // Idle wander
            float wanderChance = (m.kind == EntityKind::Bat) ? 0.65f : 0.25f;
            if (rng.chance(wanderChance)) {
                int di = rng.range(0, 7);
                tryMove(m, dirs[di][0], dirs[di][1]);
            }
            continue;
        }

        // If adjacent, melee attack.
        if (isAdjacent8(m.pos, p.pos)) {
            Entity& pm = playerMut();
            attackMelee(m, pm);
            continue;
        }

        // Wizard: occasionally "blinks" (teleports) to reposition, especially when wounded.
        if (m.kind == EntityKind::Wizard && seesPlayer) {
            const bool lowHp = (m.hp <= std::max(2, m.hpMax / 3));
            const bool close = (man <= 3);
            if (lowHp || (close && rng.chance(0.25f)) || rng.chance(0.08f)) {
                Vec2i dst = m.pos;
                for (int tries = 0; tries < 300; ++tries) {
                    Vec2i cand = dung.randomFloor(rng, true);
                    if (entityAt(cand.x, cand.y)) continue;
                    if (cand == dung.stairsUp || cand == dung.stairsDown) continue;
                    if (manhattan(cand, p.pos) < 6) continue;
                    dst = cand;
                    break;
                }
                if (dst != m.pos) {
                    const bool wasVisible = dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible;
                    m.pos = dst;
                    if (wasVisible) pushMsg("THE WIZARD BLINKS AWAY!", MessageKind::Warning, false);
                    continue;
                }
            }
        }

        // Fleeing behavior
        if (m.willFlee && m.hp <= std::max(1, m.hpMax / 3) && d0 >= 0) {
            Vec2i to = stepAway(m);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
            }
            continue;
        }

        // Ranged behavior
        if (m.canRanged && seesPlayer && man <= m.rangedRange) {
            // If too close, step back a bit
            if (man <= 2 && d0 >= 0) {
                Vec2i to = stepAway(m);
                if (to != m.pos) {
                    tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                    continue;
                }
            }

            attackRanged(m, p.pos, m.rangedRange, m.rangedAtk, m.rangedProjectile, false);
            continue;
        }

        // Pack behavior: try to occupy adjacent tiles around player
        if (m.packAI) {
            // If any adjacent tile is free, take it.
            Vec2i bestAdj = m.pos;
            bool found = false;
            for (auto& dv : dirs) {
                int ax = p.pos.x + dv[0];
                int ay = p.pos.y + dv[1];
                if (!dung.inBounds(ax, ay)) continue;
                if (!dung.isPassable(ax, ay)) continue;
                if (entityAt(ax, ay)) continue;
                // Prefer closer-to-monster candidate
                if (!found || manhattan({ax, ay}, m.pos) < manhattan(bestAdj, m.pos)) {
                    bestAdj = {ax, ay};
                    found = true;
                }
            }
            if (found) {
                Vec2i lineStep = stepToward(m); // fallback
                // Move toward chosen adjacent tile using a greedy step
                std::vector<Vec2i> path = bresenhamLine(m.pos, bestAdj);
                if (path.size() > 1) {
                    Vec2i step = path[1];
                    tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                    continue;
                }
                if (lineStep != m.pos) {
                    tryMove(m, lineStep.x - m.pos.x, lineStep.y - m.pos.y);
                    continue;
                }
            }
        }

        // Default: step toward using dist map
        if (d0 >= 0) {
            Vec2i to = stepToward(m);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
            }
        }
    }
// Post-turn passive effects (regen, etc.).
for (auto& m : ents) {
    if (m.id == playerId_) continue;
    if (m.hp <= 0) continue;
    if (m.regenAmount <= 0 || m.regenChancePct <= 0) continue;
    if (m.hp >= m.hpMax) continue;
    if (rng.range(1, 100) <= m.regenChancePct) {
        m.hp = std::min(m.hpMax, m.hp + m.regenAmount);

        // Only message if the monster is currently visible to the player.
        if (dung.inBounds(m.pos.x, m.pos.y) && dung.at(m.pos.x, m.pos.y).visible) {
            std::ostringstream ss;
            ss << kindName(m.kind) << " REGENERATES.";
            pushMsg(ss.str());
        }
    }
}

}

void Game::applyEndOfTurnEffects() {
    if (gameOver) return;

    Entity& p = playerMut();

    // Timed poison: hurts once per full turn.
    if (p.poisonTurns > 0) {
        p.poisonTurns = std::max(0, p.poisonTurns - 1);
        p.hp -= 1;
        if (p.hp <= 0) {
            pushMsg("YOU SUCCUMB TO POISON.", MessageKind::Combat, false);
            if (endCause_.empty()) endCause_ = "DIED OF POISON";
            gameOver = true;
            return;
        }

        if (p.poisonTurns == 0) {
            pushMsg("THE POISON WEARS OFF.", MessageKind::System, false);
        }
    }

    // Timed regeneration: gentle healing over time.
    if (p.regenTurns > 0) {
        p.regenTurns = std::max(0, p.regenTurns - 1);
        if (p.hp < p.hpMax) {
            p.hp += 1;
        }
        if (p.regenTurns == 0) {
            pushMsg("REGENERATION FADES.", MessageKind::System, true);
        }
    }

    // Timed shielding: no per-tick effect besides duration.
    if (p.shieldTurns > 0) {
        p.shieldTurns = std::max(0, p.shieldTurns - 1);
        if (p.shieldTurns == 0) {
            pushMsg("YOUR SHIELDING FADES.", MessageKind::System, true);
        }
    }

    // Timed vision boost
    if (p.visionTurns > 0) {
        p.visionTurns = std::max(0, p.visionTurns - 1);
        if (p.visionTurns == 0) {
            pushMsg("YOUR VISION RETURNS TO NORMAL.", MessageKind::System, true);
        }
    }

    // Timed webbing: prevents movement.
    if (p.webTurns > 0) {
        p.webTurns = std::max(0, p.webTurns - 1);
        if (p.webTurns == 0) {
            pushMsg("YOU BREAK FREE OF THE WEB.", MessageKind::System, true);
        }
    }

    // Natural regeneration (slow baseline healing).
    // Intentionally disabled while poisoned to keep poison meaningful.
    if (p.poisonTurns > 0 || p.hp >= p.hpMax) {
        naturalRegenCounter = 0;
    } else if (p.regenTurns <= 0) {
        // Faster natural regen as you level.
        const int interval = std::max(6, 14 - charLevel); // L1:13, L5:9, L10+:6
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

}

void Game::cleanupDead() {
    // Drop loot from dead monsters (before removal)
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp > 0) continue;

        // Simple drops
        if (rng.chance(0.55f)) {
            GroundItem gi;
            gi.pos = e.pos;
            gi.item.id = nextItemId++;
            gi.item.spriteSeed = rng.nextU32();

            int roll = rng.range(0, 107);
            if (roll < 40) { gi.item.kind = ItemKind::Gold; gi.item.count = rng.range(2, 8); }
            else if (roll < 55) { gi.item.kind = ItemKind::Arrow; gi.item.count = rng.range(3, 7); }
            else if (roll < 65) { gi.item.kind = ItemKind::Rock; gi.item.count = rng.range(2, 6); }
            else if (roll < 73) { gi.item.kind = ItemKind::FoodRation; gi.item.count = rng.range(1, 2); }
            else if (roll < 82) { gi.item.kind = ItemKind::PotionHealing; gi.item.count = 1; }
            else if (roll < 88) { gi.item.kind = ItemKind::PotionAntidote; gi.item.count = 1; }
            else if (roll < 92) { gi.item.kind = ItemKind::PotionRegeneration; gi.item.count = 1; }
            else if (roll < 96) { gi.item.kind = ItemKind::ScrollTeleport; gi.item.count = 1; }
            else if (roll < 98) {
                int pick = rng.range(0, 3);
                gi.item.kind = (pick == 0) ? ItemKind::ScrollIdentify
                                           : (pick == 1) ? ItemKind::ScrollDetectTraps
                                           : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                         : ItemKind::ScrollKnock;
                gi.item.count = 1;
            }
            else if (roll < 101) { gi.item.kind = ItemKind::ScrollEnchantWeapon; gi.item.count = 1; }
            else if (roll < 104) { gi.item.kind = ItemKind::ScrollEnchantArmor; gi.item.count = 1; }
            else if (roll < 105) { gi.item.kind = ItemKind::Dagger; gi.item.count = 1; }
            else if (roll < 106) { gi.item.kind = ItemKind::PotionShielding; gi.item.count = 1; }
            else if (roll < 107) { gi.item.kind = ItemKind::PotionHaste; gi.item.count = 1; }
            else { gi.item.kind = ItemKind::PotionVision; gi.item.count = 1; }

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

