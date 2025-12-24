#include "game.hpp"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <sstream>
#include <fstream>
#include <cstring>

namespace {

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
        default: return "THING";
    }
}

bool isAdjacent4(const Vec2i& a, const Vec2i& b) {
    return (std::abs(a.x - b.x) + std::abs(a.y - b.y)) == 1;
}

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
    return w ? itemDisplayName(*w) : std::string("(NONE)");
}

std::string Game::equippedRangedName() const {
    const Item* w = equippedRanged();
    return w ? itemDisplayName(*w) : std::string("(NONE)");
}

std::string Game::equippedArmorName() const {
    const Item* a = equippedArmor();
    return a ? itemDisplayName(*a) : std::string("(NONE)");
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

void Game::newGame(uint32_t seed) {
    if (seed == 0) {
        // Fall back to a simple randomized seed if user passes 0.
        seed = hash32(static_cast<uint32_t>(std::rand()) ^ 0xA5A5F00Du);
    }

    rng = RNG(seed);
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

    msgs.clear();
    msgScroll = 0;

    autoPickupGold = true;

    inputLock = false;
    gameOver = false;
    gameWon = false;

    charLevel = 1;
    xp = 0;
    xpNext = 20;

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
    pushMsg("PRESS ? FOR HELP. PRESS I FOR INVENTORY. PRESS F TO TARGET/FIRE.", MessageKind::System);
    pushMsg("NEW: C SEARCH REVEALS TRAPS. P TOGGLE AUTO-PICKUP GOLD.", MessageKind::System);
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
    invOpen = false;
    targeting = false;
    helpOpen = false;
    msgScroll = 0;

    depth_ = newDepth;

    bool restored = restoreLevel(depth_);

    Entity& p = playerMut();

    if (!restored) {
        // New level: generate and populate.
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != playerId_;
        }), ents.end());
        ground.clear();
        trapsCur.clear();
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
    return "procrogue_save.dat";
}

namespace {
constexpr uint32_t SAVE_MAGIC = 0x50525356u; // 'PRSV'
constexpr uint32_t SAVE_VERSION = 2u;

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
    return true;
}

} // namespace

bool Game::saveToFile(const std::string& path) {
    // Ensure the currently-loaded level is persisted into `levels`.
    storeCurrentLevel();

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        pushMsg("FAILED TO SAVE (CANNOT OPEN FILE).");
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
    uint8_t autoPick = autoPickupGold ? 1 : 0;
    writePod(out, autoPick);

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

    if (!out.good()) {
        pushMsg("FAILED TO SAVE (WRITE ERROR).");
        return false;
    }

    pushMsg("GAME SAVED.");
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
    uint8_t autoPick = 1; // v2+, default enabled for v1

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
    autoPickupGold = autoPick != 0;

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
}

void Game::handleAction(Action a) {
    if (a == Action::None) return;

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

    // Global hotkeys (available even while dead/won).
    if (a == Action::Save) {
        (void)saveToFile(defaultSavePath());
        return;
    }
    if (a == Action::Load) {
        (void)loadFromFile(defaultSavePath());
        return;
    }
    if (a == Action::Help) {
        // Toggle help overlay.
        helpOpen = !helpOpen;
        if (helpOpen) {
            // Close other overlays when opening help.
            invOpen = false;
            targeting = false;
            msgScroll = 0;
        }
        return;
    }

    if (a == Action::ToggleAutoPickup) {
        autoPickupGold = !autoPickupGold;
        pushMsg(autoPickupGold ? "AUTO-PICKUP GOLD: ON." : "AUTO-PICKUP GOLD: OFF.", MessageKind::System);
        return;
    }

    if (isFinished()) {
        if (a == Action::Restart) {
            newGame(hash32(rng.nextU32()));
        }
        return;
    }

    if (inputLock) {
        // Ignore actions while animating.
        if (a == Action::Cancel && (invOpen || targeting || helpOpen)) {
            invOpen = false;
            targeting = false;
            helpOpen = false;
        }
        return;
    }

    bool acted = false;

    // Help overlay mode.
    if (helpOpen) {
        if (a == Action::Cancel || a == Action::Inventory || a == Action::Help) {
            helpOpen = false;
        }
        return;
    }

    // Inventory mode.
    if (invOpen) {
        switch (a) {
            case Action::Up: moveInventorySelection(-1); break;
            case Action::Down: moveInventorySelection(1); break;
            case Action::Inventory:
            case Action::Cancel:
                closeInventory();
                break;
            case Action::Equip:
                acted = equipSelected();
                break;
            case Action::Use:
                acted = useSelected();
                break;
            case Action::Drop:
                acted = dropSelected();
                break;
            default:
                break;
        }

        if (acted) {
            monsterTurn();
            applyEndOfTurnEffects();
            cleanupDead();
            recomputeFov();
        }
        return;
    }

    // Targeting mode.
    if (targeting) {
        switch (a) {
            case Action::Up: moveTargetCursor(0, -1); break;
            case Action::Down: moveTargetCursor(0, 1); break;
            case Action::Left: moveTargetCursor(-1, 0); break;
            case Action::Right: moveTargetCursor(1, 0); break;
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
            monsterTurn();
            applyEndOfTurnEffects();
            cleanupDead();
            recomputeFov();
        }
        return;
    }

    // Normal play mode.
    Entity& p = playerMut();
    switch (a) {
        case Action::Up:    acted = tryMove(p, 0, -1); break;
        case Action::Down:  acted = tryMove(p, 0, 1); break;
        case Action::Left:  acted = tryMove(p, -1, 0); break;
        case Action::Right: acted = tryMove(p, 1, 0); break;
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
        case Action::Confirm: {
            if (p.pos == dung.stairsDown) {
                changeLevel(depth_ + 1, true);
                acted = false;
            } else if (p.pos == dung.stairsUp) {
                // At depth 1, stairs up is the exit.
                if (depth_ <= 1) {
                    if (playerHasAmulet()) {
                        gameWon = true;
                        pushMsg("YOU ESCAPE WITH THE AMULET OF YENDOR!", MessageKind::Success);
                        pushMsg("VICTORY!");
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
                        pushMsg("YOU ESCAPE WITH THE AMULET OF YENDOR!", MessageKind::Success);
                        pushMsg("VICTORY!");
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
        monsterTurn();
        applyEndOfTurnEffects();
        cleanupDead();
        recomputeFov();
    }
}

bool Game::tryMove(Entity& e, int dx, int dy) {
    int nx = e.pos.x + dx;
    int ny = e.pos.y + dy;

    if (!dung.inBounds(nx, ny)) return false;

    // Closed door: opening consumes a turn.
    if (dung.isDoorClosed(nx, ny)) {
        dung.openDoor(nx, ny);
        if (e.kind == EntityKind::Player) pushMsg("YOU OPEN THE DOOR.");
        return true;
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
        // Convenience / QoL: auto-pick up gold when stepping on it.
        if (autoPickupGold) {
            (void)autoPickupGoldAtPlayer();
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
        default:
            break;
    }
}

bool Game::searchForTraps() {
    Entity& p = playerMut();
    const int radius = 2;

    int found = 0;
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
            found += 1;
        }
    }

    if (found > 0) {
        std::ostringstream ss;
        ss << "YOU DISCOVER " << found << " TRAP" << (found == 1 ? "" : "S") << "!";
        pushMsg(ss.str(), MessageKind::Info, true);
    } else {
        pushMsg("YOU SEARCH, BUT FIND NOTHING.", MessageKind::Info, true);
    }

    return true; // Searching costs a turn.
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

    // Venomous monsters can inflict poison.
    if (defender.hp > 0 && defender.kind == EntityKind::Player) {
        if (attacker.kind == EntityKind::Snake && rng.chance(0.35f)) {
            defender.poisonTurns = std::max(defender.poisonTurns, rng.range(4, 8));
            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
        }
        if (attacker.kind == EntityKind::Spider && rng.chance(0.45f)) {
            defender.poisonTurns = std::max(defender.poisonTurns, rng.range(6, 10));
            pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
        }
    }

    if (defender.hp <= 0) {
        if (defender.kind == EntityKind::Player) {
            pushMsg("YOU DIE.", MessageKind::Combat, false);
            gameOver = true;
        } else {
            std::ostringstream ds;
            ds << kindName(defender.kind) << " DIES.";
            pushMsg(ds.str(), MessageKind::Combat, msgFromPlayer);

            if (attacker.kind == EntityKind::Player) {
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
                gameOver = true;
            } else {
                std::ostringstream ds;
                ds << kindName(hit->kind) << " DIES.";
                pushMsg(ds.str(), MessageKind::Combat, fromPlayer);
                if (fromPlayer) {
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
    dung.computeFov(p.pos.x, p.pos.y, 9);
}

void Game::openInventory() {
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

bool Game::autoPickupGoldAtPlayer() {
    const Vec2i pos = player().pos;
    const int maxInv = 26;

    bool pickedAny = false;

    for (size_t i = 0; i < ground.size();) {
        if (ground[i].pos == pos && ground[i].item.kind == ItemKind::Gold) {
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

            pickedAny = true;
            pushMsg("YOU PICK UP " + itemDisplayName(it) + ".", MessageKind::Loot, true);
            ground.erase(ground.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }

    return pickedAny;
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

    const int maxInv = 26;
    bool pickedAny = false;

    // Pick up in reverse order so erase indices stay valid.
    for (size_t k = idxs.size(); k-- > 0;) {
        size_t gi = idxs[k];
        if (gi >= ground.size()) continue;

        Item it = ground[gi].item;

        if (tryStackItem(inv, it)) {
            // stacked
            pickedAny = true;
            pushMsg("YOU PICK UP " + itemDisplayName(it) + ".", MessageKind::Loot, true);
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
        pushMsg("YOU PICK UP " + itemDisplayName(it) + ".", MessageKind::Loot, true);
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

    pushMsg("YOU DROP " + itemDisplayName(drop) + ".");
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
            pushMsg("YOU UNWIELD " + itemDisplayName(it) + ".");
        } else {
            equipMeleeId = it.id;
            pushMsg("YOU WIELD " + itemDisplayName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::RangedWeapon) {
        if (equipRangedId == it.id) {
            equipRangedId = 0;
            pushMsg("YOU UNEQUIP " + itemDisplayName(it) + ".");
        } else {
            equipRangedId = it.id;
            pushMsg("YOU READY " + itemDisplayName(it) + ".");
        }
        return true;
    }
    if (d.slot == EquipSlot::Armor) {
        if (equipArmorId == it.id) {
            equipArmorId = 0;
            pushMsg("YOU REMOVE " + itemDisplayName(it) + ".");
        } else {
            equipArmorId = it.id;
            pushMsg("YOU WEAR " + itemDisplayName(it) + ".");
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

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionStrength) {
        Entity& p = playerMut();
        p.baseAtk += 1;
        std::ostringstream ss;
        ss << "YOU FEEL STRONGER! ATK IS NOW " << p.baseAtk << ".";
        pushMsg(ss.str(), MessageKind::Success, true);
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
        consumeOneStackable();
        recomputeFov();
        return true;
    }

    if (it.kind == ItemKind::ScrollMapping) {
        dung.revealAll();
        pushMsg("THE DUNGEON MAP IS REVEALED.", MessageKind::Info, true);
        consumeOneStackable();
        recomputeFov();
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
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionRegeneration) {
        Entity& p = playerMut();
        p.regenTurns = std::max(p.regenTurns, 18);
        pushMsg("YOUR WOUNDS BEGIN TO KNIT.", MessageKind::Success, true);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionShielding) {
        Entity& p = playerMut();
        p.shieldTurns = std::max(p.shieldTurns, 14);
        pushMsg("YOU FEEL PROTECTED.", MessageKind::Success, true);
        consumeOneStackable();
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
        int n = rng.range(0, base + (depth_ >= 3 ? 2 : 1));

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

        // Treasure rooms get a guardian sometimes.
        if (r.type == RoomType::Treasure && !isStart && rng.chance(0.6f)) {
            Vec2i p = randomFreeTileInRoom(r);
            addMonster(depth_ >= 4 ? (rng.chance(0.35f) ? EntityKind::Wizard : EntityKind::Troll) : (depth_ >= 3 ? EntityKind::Orc : EntityKind::Goblin), p, 0);
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
        int roll = rng.range(0, 99);
        if (roll < 18) dropItemAt(ItemKind::Sword, randomFreeTileInRoom(r));
        else if (roll < 32) dropItemAt(ItemKind::ChainArmor, randomFreeTileInRoom(r));
        else if (roll < 44) dropItemAt(ItemKind::WandSparks, randomFreeTileInRoom(r));
        else if (roll < 54) dropItemAt(ItemKind::Sling, randomFreeTileInRoom(r));
        else if (roll < 64) dropItemAt(ItemKind::PotionStrength, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 74) dropItemAt(ItemKind::PotionHealing, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 82) dropItemAt(ItemKind::PotionAntidote, randomFreeTileInRoom(r), rng.range(1, 2));
        else if (roll < 88) dropItemAt(ItemKind::PotionRegeneration, randomFreeTileInRoom(r), 1);
        else if (roll < 92) dropItemAt(ItemKind::PotionShielding, randomFreeTileInRoom(r), 1);
        else if (roll < 95) dropItemAt(ItemKind::ScrollMapping, randomFreeTileInRoom(r), 1);
        else if (roll < 97) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
        else if (roll < 99) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
        else dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
    };

    for (const Room& r : rooms) {
        Vec2i p = randomFreeTileInRoom(r);

        if (r.type == RoomType::Treasure) {
            dropItemAt(ItemKind::Gold, p, rng.range(15, 40) + depth_ * 3);
            dropGoodItem(r);
            continue;
        }

        if (r.type == RoomType::Shrine) {
            dropItemAt(ItemKind::PotionHealing, p, rng.range(1, 2));
            if (rng.chance(0.45f)) dropItemAt(ItemKind::PotionStrength, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.35f)) dropItemAt(ItemKind::PotionAntidote, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.30f)) dropItemAt(ItemKind::PotionRegeneration, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.22f)) dropItemAt(ItemKind::PotionShielding, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.18f)) dropItemAt(ItemKind::ScrollEnchantWeapon, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.12f)) dropItemAt(ItemKind::ScrollEnchantArmor, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.45f)) dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.35f)) dropItemAt(ItemKind::ScrollMapping, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.50f)) dropItemAt(ItemKind::Gold, randomFreeTileInRoom(r), rng.range(6, 18));
            continue;
        }

        if (r.type == RoomType::Lair) {
            if (rng.chance(0.50f)) dropItemAt(ItemKind::Rock, p, rng.range(3, 9));
            if (depth_ >= 2 && rng.chance(0.20f)) dropItemAt(ItemKind::Sling, randomFreeTileInRoom(r), 1);
            continue;
        }

        // Normal rooms: small chance for loot
        if (rng.chance(0.35f)) {
            int roll = rng.range(0, 99);
            if (roll < 22) dropItemAt(ItemKind::Gold, p, rng.range(3, 10));
            else if (roll < 36) dropItemAt(ItemKind::PotionHealing, p, 1);
            else if (roll < 46) dropItemAt(ItemKind::PotionStrength, p, 1);
            else if (roll < 54) dropItemAt(ItemKind::PotionAntidote, p, 1);
            else if (roll < 60) dropItemAt(ItemKind::PotionRegeneration, p, 1);
            else if (roll < 66) dropItemAt(ItemKind::ScrollTeleport, p, 1);
            else if (roll < 72) dropItemAt(ItemKind::ScrollMapping, p, 1);
            else if (roll < 76) dropItemAt(ItemKind::ScrollEnchantWeapon, p, 1);
            else if (roll < 80) dropItemAt(ItemKind::ScrollEnchantArmor, p, 1);
            else if (roll < 88) dropItemAt(ItemKind::Arrow, p, rng.range(4, 10));
            else if (roll < 94) dropItemAt(ItemKind::Rock, p, rng.range(3, 8));
            else if (roll < 97) dropItemAt(ItemKind::Dagger, p, 1);
            else if (roll < 99) dropItemAt(ItemKind::LeatherArmor, p, 1);
            else dropItemAt(ItemKind::PotionShielding, p, 1);
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
            else if (roll < 90) tk = TrapKind::Alarm;
            else tk = TrapKind::Teleport;
        } else {
            if (roll < 35) tk = TrapKind::Spike;
            else if (roll < 65) tk = TrapKind::PoisonDart;
            else if (roll < 85) tk = TrapKind::Alarm;
            else tk = TrapKind::Teleport;
        }

        Trap t;
        t.kind = tk;
        t.pos = p;
        t.discovered = false;
        trapsCur.push_back(t);
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

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        Vec2i cur = q.front();
        q.pop_front();
        int cd = dist[static_cast<size_t>(idx(cur.x, cur.y))];
        for (auto& dv : dirs) {
            int nx = cur.x + dv[0];
            int ny = cur.y + dv[1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (dist[static_cast<size_t>(idx(nx, ny))] != -1) continue;
            dist[static_cast<size_t>(idx(nx, ny))] = cd + 1;
            q.push_back({nx, ny});
        }
    }

    // Helper to choose move based on dist map.
    auto stepToward = [&](const Entity& m) -> Vec2i {
        Vec2i best = m.pos;
        int bestD = 1e9;
        for (auto& dv : dirs) {
            int nx = m.pos.x + dv[0];
            int ny = m.pos.y + dv[1];
            if (!dung.inBounds(nx, ny)) continue;
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
            int nx = m.pos.x + dv[0];
            int ny = m.pos.y + dv[1];
            if (!dung.inBounds(nx, ny)) continue;
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
                int di = rng.range(0, 3);
                tryMove(m, dirs[di][0], dirs[di][1]);
            }
            continue;
        }

        // If adjacent, melee attack.
        if (isAdjacent4(m.pos, p.pos)) {
            Entity& pm = playerMut();
            attackMelee(m, pm);
            continue;
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

            int roll = rng.range(0, 99);
            if (roll < 40) { gi.item.kind = ItemKind::Gold; gi.item.count = rng.range(2, 8); }
            else if (roll < 55) { gi.item.kind = ItemKind::Arrow; gi.item.count = rng.range(3, 7); }
            else if (roll < 65) { gi.item.kind = ItemKind::Rock; gi.item.count = rng.range(2, 6); }
            else if (roll < 74) { gi.item.kind = ItemKind::PotionHealing; gi.item.count = 1; }
            else if (roll < 80) { gi.item.kind = ItemKind::PotionAntidote; gi.item.count = 1; }
            else if (roll < 84) { gi.item.kind = ItemKind::PotionRegeneration; gi.item.count = 1; }
            else if (roll < 88) { gi.item.kind = ItemKind::ScrollTeleport; gi.item.count = 1; }
            else if (roll < 91) { gi.item.kind = ItemKind::ScrollEnchantWeapon; gi.item.count = 1; }
            else if (roll < 94) { gi.item.kind = ItemKind::ScrollEnchantArmor; gi.item.count = 1; }
            else if (roll < 97) { gi.item.kind = ItemKind::Dagger; gi.item.count = 1; }
            else { gi.item.kind = ItemKind::PotionShielding; gi.item.count = 1; }

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
        }
    }

    // Remove dead monsters
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return (e.id != playerId_) && (e.hp <= 0);
    }), ents.end());

    // Player death handled in attack functions
}

