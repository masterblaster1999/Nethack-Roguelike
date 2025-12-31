#include "game_internal.hpp"

uint32_t dailySeedUtc(std::string* outDateIso) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    const int year = tm.tm_year + 1900;
    const int mon = tm.tm_mon + 1;
    const int day = tm.tm_mday;

    if (outDateIso) {
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(4) << year << "-" << std::setw(2) << mon << "-" << std::setw(2) << day;
        *outDateIso = ss.str();
    }

    // YYYYMMDD -> stable hash (not crypto; just deterministic across platforms).
    const uint32_t ymd = static_cast<uint32_t>(year * 10000 + mon * 100 + day);
    return hash32(ymd ^ 0xDABA0B1Du);
}



Game::Game() : dung(MAP_W, MAP_H) {}

namespace {
// During early boot, main.cpp applies user settings before a run is created/loaded.
// Some getters/setters (lighting/encumbrance) may consult the player entity. In that
// phase, `ents` is still empty. Returning a stable dummy prevents UB (and Windows
// access violations) while keeping the rest of the code simple.
Entity& dummyPlayerEntity() {
    static Entity dummy;
    static bool init = false;
    if (!init) {
        dummy.id = 0;
        dummy.kind = EntityKind::Player;
        dummy.hpMax = 1;
        dummy.hp = 1;
        dummy.pos = {0, 0};
        init = true;
    }
    return dummy;
}
}

const Entity& Game::player() const {
    for (const auto& e : ents) if (e.id == playerId_) return e;
    if (!ents.empty()) return ents.front();
    return dummyPlayerEntity();
}

Entity& Game::playerMut() {
    for (auto& e : ents) if (e.id == playerId_) return e;
    if (!ents.empty()) return ents.front();
    return dummyPlayerEntity();
}

void Game::pushMsg(const std::string& s, MessageKind kind, bool fromPlayer) {
    // Coalesce consecutive identical messages to reduce spam in combat / auto-move.
    // This preserves the original text and adds a repeat counter for the renderer.
    if (!msgs.empty()) {
        Message& last = msgs.back();
        if (last.text == s && last.kind == kind && last.fromPlayer == fromPlayer) {
            if (last.repeat < 9999) {
                ++last.repeat;
            }
            return;
        }
    }

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

int Game::equippedRing1Index() const {
    return findItemIndexById(inv, equipRing1Id);
}

int Game::equippedRing2Index() const {
    return findItemIndexById(inv, equipRing2Id);
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

const Item* Game::equippedRing1() const {
    int idx = equippedRing1Index();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedRing2() const {
    int idx = equippedRing2Index();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

bool Game::isEquipped(int itemId) const {
    return itemId != 0 && (itemId == equipMeleeId || itemId == equipRangedId || itemId == equipArmorId || itemId == equipRing1Id || itemId == equipRing2Id);
}

std::string Game::equippedTag(int itemId) const {
    std::string t;
    if (itemId != 0 && itemId == equipMeleeId) t += "M";
    if (itemId != 0 && itemId == equipRangedId) t += "R";
    if (itemId != 0 && itemId == equipArmorId) t += "A";
    if (itemId != 0 && itemId == equipRing1Id) t += "1";
    if (itemId != 0 && itemId == equipRing2Id) t += "2";
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

std::string Game::equippedRing1Name() const {
    const Item* r = equippedRing1();
    return r ? displayItemName(*r) : std::string("(NONE)");
}

std::string Game::equippedRing2Name() const {
    const Item* r = equippedRing2();
    return r ? displayItemName(*r) : std::string("(NONE)");
}

namespace {
inline int bucScalar(const Item& it) {
    return (it.buc < 0 ? -1 : (it.buc > 0 ? 1 : 0));
}

inline int applyGearModIfNonZero(int base, const Item& it) {
    if (base == 0) return 0;
    return base + it.enchant + bucScalar(it);
}
}

int Game::ringTalentBonusMight() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modMight, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modMight, *r);
    }
    return b;
}

int Game::ringTalentBonusAgility() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modAgility, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modAgility, *r);
    }
    return b;
}

int Game::ringTalentBonusVigor() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modVigor, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modVigor, *r);
    }
    return b;
}

int Game::ringTalentBonusFocus() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modFocus, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modFocus, *r);
    }
    return b;
}

int Game::ringDefenseBonus() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.defense, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.defense, *r);
    }
    return b;
}

int Game::playerAttack() const {
    int atk = playerMeleePower();
    if (const Item* w = equippedMelee()) {
        atk += itemDef(w->kind).meleeAtk;
        atk += w->enchant;
        atk += (w->buc < 0 ? -1 : (w->buc > 0 ? 1 : 0));
    }
    return atk;
}

int Game::playerDefense() const {
    int def = playerEvasion();
    if (const Item* a = equippedArmor()) {
        def += itemDef(a->kind).defense;
        def += a->enchant;
        def += (a->buc < 0 ? -1 : (a->buc > 0 ? 1 : 0));
    }

    // Rings can provide small passive defense (e.g., Ring of Protection).
    def += ringDefenseBonus();
    // Temporary shielding buff
    if (player().effects.shieldTurns > 0) def += 2;
    return def;
}

int Game::playerRangedRange() const {
    // Preferred: an equipped ranged weapon that is actually ready (ammo/charges).
    if (const Item* w = equippedRanged()) {
        const ItemDef& d = itemDef(w->kind);
        const bool hasRange = (d.range > 0);
        const bool chargesOk = (d.maxCharges <= 0) || (w->charges > 0);
        const bool ammoOk = (d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0);

        if (hasRange && chargesOk && ammoOk) {
            return d.range;
        }
    }

    // Fallback: "throw by hand" when you have ammo (rocks/arrows) but no usable ranged weapon.
    ThrowAmmoSpec spec;
    if (choosePlayerThrowAmmo(inv, spec)) {
        return throwRangeFor(player(), spec.ammo);
    }

    return 0;
}


bool Game::playerHasRangedReady(std::string* reasonOut) const {
    // Prefer an equipped ranged weapon when it is ready (ammo/charges).
    if (const Item* w = equippedRanged()) {
        const ItemDef& d = itemDef(w->kind);

        const bool hasRange = (d.range > 0);
        const bool chargesOk = (d.maxCharges <= 0) || (w->charges > 0);
        const bool ammoOk = (d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0);

        if (hasRange && chargesOk && ammoOk) {
            return true;
        }

        // Fallback: if the equipped weapon can't be used (no ammo/charges), allow throwing
        // (rocks/arrows) so the player still has a ranged option without menu friction.
        ThrowAmmoSpec spec;
        if (choosePlayerThrowAmmo(inv, spec)) {
            return true;
        }

        // No fallback available: explain why the equipped weapon can't be used.
        if (!hasRange) {
            if (reasonOut) *reasonOut = "THAT WEAPON CAN'T FIRE.";
            return false;
        }
        if (!chargesOk) {
            if (reasonOut) *reasonOut = "THE WAND IS OUT OF CHARGES.";
            return false;
        }
        if (!ammoOk) {
            if (reasonOut) {
                *reasonOut = (d.ammo == AmmoKind::Arrow) ? "NO ARROWS." : "NO ROCKS.";
            }
            return false;
        }
    }

    // No equipped ranged weapon: allow throwing ammo by hand if available.
    ThrowAmmoSpec spec;
    if (choosePlayerThrowAmmo(inv, spec)) return true;

    if (reasonOut) *reasonOut = "NO RANGED WEAPON OR THROWABLE AMMO.";
    return false;
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
        case EntityKind::Mimic: return 22;
        case EntityKind::Minotaur: return 45;
        case EntityKind::Shopkeeper: return 0;
        case EntityKind::Dog: return 0;
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

    const int hpGain = 2 + rng.range(0, 2);
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

    // Award a talent point and force the allocation overlay to open.
    talentPointsPending_ += 1;
    levelUpOpen = true;
    levelUpSel = std::clamp(levelUpSel, 0, 3);

    // Cancel other UI modes / automation so the player can make a choice.
    invOpen = false;
    invIdentifyMode = false;
    targeting = false;
    targetLine.clear();
    targetValid = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    optionsOpen = false;
    commandOpen = false;
    looking = false;
    stopAutoMove(true);

    pushMsg("CHOOSE A TALENT: MIGHT / AGILITY / VIGOR / FOCUS (ARROWS + ENTER).", MessageKind::System, true);
}

bool Game::playerHasAmulet() const {
    for (const auto& it : inv) {
        if (it.kind == ItemKind::AmuletYendor) return true;
    }
    return false;
}

// ------------------------------------------------------------
// Yendor Doom (endgame escalation)
//
// Once the player acquires the Amulet of Yendor, the dungeon begins applying
// pressure through periodic "noise pulses" (waking/alerting monsters) and
// hunter packs that spawn out of sight.
// ------------------------------------------------------------

int Game::computeYendorDoomLevel() const {
    if (!yendorDoomActive_) return 0;

    const uint32_t now = turnCount;
    const uint32_t start = yendorDoomStartTurn_;
    const int turnsSince = (now >= start) ? static_cast<int>(now - start) : 0;

    // 0 at quest depth, grows as the player ascends toward the surface.
    const int ascension = std::max(0, QUEST_DEPTH - depth_);

    // Time pressure: +1 every ~40 player turns after acquiring the Amulet.
    const int timeTerm = turnsSince / 40;

    int lvl = 1 + (ascension * 2) + timeTerm;
    return std::clamp(lvl, 1, 20);
}

void Game::onAmuletAcquired() {
    if (!yendorDoomEnabled_) return;
    if (gameOver || gameWon) return;
    if (yendorDoomActive_) return;

    yendorDoomActive_ = true;
    if (yendorDoomStartTurn_ == 0u) yendorDoomStartTurn_ = turnCount;
    yendorDoomLastPulseTurn_ = turnCount;
    yendorDoomLastSpawnTurn_ = turnCount;
    yendorDoomMsgStage_ = 0;
    yendorDoomLevel_ = computeYendorDoomLevel();

    pushMsg("THE DUNGEON STIRS. SOMETHING HUNTS YOU...", MessageKind::ImportantMsg, true);
}

void Game::spawnYendorHunterPack(int doomLevel) {
    if (dung.rooms.empty()) return;
    if (ents.size() >= 120u) return; // keep things sane on huge levels

    const Vec2i ppos = player().pos;

    // How many hunters? Scale slowly with doom.
    int count = 2 + (doomLevel / 5);
    if (rng.range(0, 3) == 0) count += 1;
    count = std::clamp(count, 2, 6);

    auto pickKind = [&](int dlvl) -> EntityKind {
        // Weighted selection by doom intensity.
        const int r = rng.range(0, 99);
        if (dlvl >= 18) {
            if (r < 10) return EntityKind::Minotaur;
            if (r < 30) return EntityKind::Wizard;
            if (r < 55) return EntityKind::Troll;
            if (r < 80) return EntityKind::Ogre;
            return EntityKind::SkeletonArcher;
        }
        if (dlvl >= 12) {
            if (r < 10) return EntityKind::Wizard;
            if (r < 35) return EntityKind::Troll;
            if (r < 60) return EntityKind::Ogre;
            if (r < 80) return EntityKind::SkeletonArcher;
            return EntityKind::Wolf;
        }
        if (dlvl >= 6) {
            if (r < 25) return EntityKind::Ogre;
            if (r < 50) return EntityKind::SkeletonArcher;
            if (r < 70) return EntityKind::Orc;
            if (r < 85) return EntityKind::Wolf;
            return EntityKind::KoboldSlinger;
        }
        // Early doom: mostly fast/annoying hunters.
        if (r < 35) return EntityKind::Wolf;
        if (r < 60) return EntityKind::Orc;
        if (r < 80) return EntityKind::SkeletonArcher;
        return EntityKind::KoboldSlinger;
    };

    // Find a spawn point out of sight and not too close to the player.
    Vec2i anchor{-1, -1};
    for (int attempt = 0; attempt < 50; ++attempt) {
        const Room& r = dung.rooms[static_cast<size_t>(rng.range(0, static_cast<int>(dung.rooms.size() - 1)))];
        if (r.type == RoomType::Shop) continue;

        // Avoid spawning in the player's current room.
        if (ppos.x >= r.x && ppos.x < r.x + r.w && ppos.y >= r.y && ppos.y < r.y + r.h) continue;

        Vec2i p = randomFreeTileInRoom(r);
        if (!dung.inBounds(p.x, p.y)) continue;
        if (dung.at(p.x, p.y).visible) continue;
        const int dist = std::abs(p.x - ppos.x) + std::abs(p.y - ppos.y);
        if (dist < 10) continue;

        anchor = p;
        break;
    }

    // Fallback: random hidden floor tile.
    if (anchor.x < 0) {
        for (int attempt = 0; attempt < 400; ++attempt) {
            const int x = rng.range(1, dung.width - 2);
            const int y = rng.range(1, dung.height - 2);
            if (!dung.inBounds(x, y)) continue;
            const TileType t = dung.at(x, y).type;
            if (!(t == TileType::Floor || t == TileType::DoorOpen || t == TileType::StairsUp || t == TileType::StairsDown)) continue;
            if (entityAt(x, y)) continue;
            if (dung.at(x, y).visible) continue;
            const int dist = std::abs(x - ppos.x) + std::abs(y - ppos.y);
            if (dist < 10) continue;
            anchor = {x, y};
            break;
        }
    }

    if (anchor.x < 0) return;

    const int gid = nextEntityId; // good enough unique group id
    for (int i = 0; i < count; ++i) {
        Vec2i pos = anchor;

        // Spread slightly around the anchor.
        for (int j = 0; j < 12; ++j) {
            const int dx = rng.range(-2, 2);
            const int dy = rng.range(-2, 2);
            if (dx == 0 && dy == 0) continue;
            Vec2i q{anchor.x + dx, anchor.y + dy};
            if (!dung.inBounds(q.x, q.y)) continue;
            const TileType t = dung.at(q.x, q.y).type;
            if (!(t == TileType::Floor || t == TileType::DoorOpen || t == TileType::StairsUp || t == TileType::StairsDown)) continue;
            if (entityAt(q.x, q.y)) continue;
            if (dung.at(q.x, q.y).visible) continue;
            pos = q;
            break;
        }

        const EntityKind kind = pickKind(doomLevel);
        spawnMonster(kind, pos, gid, true);

        // Hunters begin alerted and immediately know (roughly) where you are.
        Entity& m = ents.back();
        m.alerted = true;
        m.lastKnownPlayerPos = ppos;
        m.lastKnownPlayerAge = 0;
    }

    // Lightly telegraph without being too spammy.
    if (rng.range(0, 2) == 0) {
        pushMsg("YOU FEEL A MALEVOLENT PRESENCE DRAWING NEAR.", MessageKind::ImportantMsg);
    }
}

void Game::tickYendorDoom() {
    if (!yendorDoomEnabled_) return;
    if (gameOver || gameWon) return;

    // Safety: if the player no longer has the Amulet, the system should not run.
    if (!playerHasAmulet()) {
        yendorDoomActive_ = false;
        yendorDoomLevel_ = 0;
        return;
    }

    // If we loaded a legacy save with an Amulet already in hand, start silently.
    if (!yendorDoomActive_) {
        yendorDoomActive_ = true;
        if (yendorDoomStartTurn_ == 0u) yendorDoomStartTurn_ = turnCount;
        if (yendorDoomLastPulseTurn_ == 0u) yendorDoomLastPulseTurn_ = turnCount;
        if (yendorDoomLastSpawnTurn_ == 0u) yendorDoomLastSpawnTurn_ = turnCount;
    }

    const int lvl = computeYendorDoomLevel();
    yendorDoomLevel_ = lvl;

    // One-time escalating warnings.
    if (lvl >= 6 && yendorDoomMsgStage_ < 1) {
        yendorDoomMsgStage_ = 1;
        pushMsg("THE AIR GROWS HEAVY WITH DREAD.", MessageKind::ImportantMsg);
    } else if (lvl >= 12 && yendorDoomMsgStage_ < 2) {
        yendorDoomMsgStage_ = 2;
        pushMsg("THE DUNGEON'S RAGE BUILDS BEHIND YOU.", MessageKind::ImportantMsg);
    } else if (lvl >= 18 && yendorDoomMsgStage_ < 3) {
        yendorDoomMsgStage_ = 3;
        pushMsg("THE VERY STONES SCREAM FOR YOUR BLOOD!", MessageKind::ImportantMsg);
    }

    const uint32_t now = turnCount;
    const int pulseEvery = std::clamp(60 - (lvl * 2), 15, 60);
    const int spawnEvery = std::clamp(90 - (lvl * 3), 25, 90);

    if (now - yendorDoomLastPulseTurn_ >= static_cast<uint32_t>(pulseEvery)) {
        yendorDoomLastPulseTurn_ = now;
        const int volume = 10 + lvl; // louder as doom rises
        emitNoise(player().pos, volume);

        if (lvl >= 10 && rng.range(0, 4) == 0) {
            pushMsg("THE AMULET PULSES WITH DARK POWER.", MessageKind::System);
        }
    }

    if (now - yendorDoomLastSpawnTurn_ >= static_cast<uint32_t>(spawnEvery)) {
        yendorDoomLastSpawnTurn_ = now;
        spawnYendorHunterPack(lvl);
    }
}

// ------------------------------------------------------------
// Identification (items start unknown; appearances randomized per run)
// ------------------------------------------------------------

void Game::initIdentificationTables() {
    identKnown.fill(1);
    identAppearance.fill(0);

    if (!identifyItemsEnabled) {
        // All items show true names.
        return;
    }

    // Mark identifiable kinds as unknown by default.
    for (ItemKind k : POTION_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }
    for (ItemKind k : SCROLL_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }
    for (ItemKind k : RING_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }
    for (ItemKind k : WAND_KINDS) {
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
    const size_t ringN = sizeof(RING_KINDS) / sizeof(RING_KINDS[0]);
    const size_t wandN = sizeof(WAND_KINDS) / sizeof(WAND_KINDS[0]);

    const size_t potionAppearN = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
    const size_t scrollAppearN = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);
    const size_t ringAppearN = sizeof(RING_APPEARANCES) / sizeof(RING_APPEARANCES[0]);
    const size_t wandAppearN = sizeof(WAND_APPEARANCES) / sizeof(WAND_APPEARANCES[0]);

    std::vector<uint8_t> p = shuffledIndices(potionAppearN);
    std::vector<uint8_t> s = shuffledIndices(scrollAppearN);
    std::vector<uint8_t> r = shuffledIndices(ringAppearN);
    std::vector<uint8_t> w = shuffledIndices(wandAppearN);

    // If someone later adds more kinds than appearances, we still function
    // (we'll reuse appearances), but keep the common case unique.
    for (size_t i = 0; i < potionN; ++i) {
        const uint8_t app = p[i % p.size()];
        identAppearance[static_cast<size_t>(POTION_KINDS[i])] = app;
    }
    for (size_t i = 0; i < scrollN; ++i) {
        const uint8_t app = s[i % s.size()];
        identAppearance[static_cast<size_t>(SCROLL_KINDS[i])] = app;
    }
    for (size_t i = 0; i < ringN; ++i) {
        const uint8_t app = r[i % r.size()];
        identAppearance[static_cast<size_t>(RING_KINDS[i])] = app;
    }
    for (size_t i = 0; i < wandN; ++i) {
        const uint8_t app = w[i % w.size()];
        identAppearance[static_cast<size_t>(WAND_KINDS[i])] = app;
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
        constexpr size_t n = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
        static_assert(n > 0, "POTION_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return POTION_APPEARANCES[a];
    }
    if (isScrollKind(k)) {
        constexpr size_t n = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);
        static_assert(n > 0, "SCROLL_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return SCROLL_APPEARANCES[a];
    }
    if (isRingKind(k)) {
        constexpr size_t n = sizeof(RING_APPEARANCES) / sizeof(RING_APPEARANCES[0]);
        static_assert(n > 0, "RING_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return RING_APPEARANCES[a];
    }
    if (isWandKind(k)) {
        constexpr size_t n = sizeof(WAND_APPEARANCES) / sizeof(WAND_APPEARANCES[0]);
        static_assert(n > 0, "WAND_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return WAND_APPEARANCES[a];
    }
    return "";
}

std::string Game::unknownDisplayName(const Item& it) const {
    std::ostringstream ss;

    auto appendGearPrefix = [&]() {
        if (!isWearableGear(it.kind)) return;

        if (it.buc < 0) ss << "CURSED ";
        else if (it.buc > 0) ss << "BLESSED ";

        if (it.enchant != 0) {
            if (it.enchant > 0) ss << "+";
            ss << it.enchant << " ";
        }
    };

    if (isPotionKind(it.kind)) {
        const std::string app = appearanceName(it.kind);
        if (it.count > 1) ss << it.count << " " << app << " POTIONS";
        else ss << app << " POTION";
    } else if (isScrollKind(it.kind)) {
        const std::string app = appearanceName(it.kind);
        if (it.count > 1) ss << it.count << " SCROLLS '" << app << "'";
        else ss << "SCROLL '" << app << "'";
    } else if (isRingKind(it.kind)) {
        appendGearPrefix();
        const std::string app = appearanceName(it.kind);
        ss << app << " RING";
    } else if (isWandKind(it.kind)) {
        appendGearPrefix();
        const std::string app = appearanceName(it.kind);
        ss << app << " WAND";
    } else {
        // Non-identifiable kinds keep their normal names.
        return itemDisplayName(it);
    }

    // Preserve shop price tags even while unidentified.
    if (it.shopPrice > 0 && it.shopDepth > 0) {
        const ItemDef& d = itemDef(it.kind);
        const int n = d.stackable ? std::max(1, it.count) : 1;
        const int total = it.shopPrice * n;
        ss << " [PRICE " << total << "G]";
    }

    return ss.str();
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
    confusionGas_.clear();
    inv.clear();
    fx.clear();
    fxExpl.clear();

    // Reset endgame escalation state.
    yendorDoomActive_ = false;
    yendorDoomLevel_ = 0;
    yendorDoomStartTurn_ = 0u;
    yendorDoomLastPulseTurn_ = 0u;
    yendorDoomLastSpawnTurn_ = 0u;
    yendorDoomMsgStage_ = 0;

    nextEntityId = 1;
    nextItemId = 1;
    equipMeleeId = 0;
    equipRangedId = 0;
    equipArmorId = 0;
    equipRing1Id = 0;
    equipRing2Id = 0;

    invOpen = false;
    invIdentifyMode = false;
    invSel = 0;
    targeting = false;
    targetLine.clear();
    targetValid = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    levelUpOpen = false;
    levelUpSel = 0;

    msgs.clear();
    msgScroll = 0;

    // autoPickup is a user setting; do not reset it between runs.

    sneakMode_ = false;

    // Randomize identifiable item appearances and reset identification knowledge.
    initIdentificationTables();

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;
    autoExploreGoalIsLoot = false;
    autoExploreGoalPos = Vec2i{-1, -1};

    turnCount = 0;
    naturalRegenCounter = 0;
    lastAutosaveTurn = 0;

    killCount = 0;
    maxDepth = 1;
    runRecorded = false;
    mortemWritten_ = false;
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

    talentMight_ = 0;
    talentAgility_ = 0;
    talentVigor_ = 0;
    talentFocus_ = 0;
    talentPointsPending_ = 0;

    // Class starting bias (small passive nudge; most growth comes from level-ups).
    // This is applied before the first level is generated so it is part of the run identity.
    switch (playerClass_) {
        case PlayerClass::Knight: {
            talentMight_ = 1;
            talentVigor_ = 1;
        } break;
        case PlayerClass::Rogue: {
            talentAgility_ = 1;
            talentFocus_ = 1;
        } break;
        case PlayerClass::Archer: {
            talentAgility_ = 2;
        } break;
        case PlayerClass::Wizard: {
            talentFocus_ = 2;
        } break;
        case PlayerClass::Adventurer:
        default:
            break;
    }

    // Hunger pacing (optional setting; stored per-run in save files).
    hungerMax = 800;
    hunger = hungerMax;
    hungerStatePrev = hungerStateFor(hunger, hungerMax);

    dung.generate(rng, depth_, DUNGEON_MAX_DEPTH);
    // Environmental fields reset per floor (no lingering gas on a fresh level).
    confusionGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);

    // Create player
    Entity p;
    p.id = nextEntityId++;
    p.kind = EntityKind::Player;
    p.pos = dung.stairsUp;

    // Class baseline stats.
    // These are intentionally modest; items and talents still matter a lot.
    p.hpMax = 18;
    p.baseAtk = 3;
    p.baseDef = 0;

    switch (playerClass_) {
        case PlayerClass::Knight:
            p.hpMax = 22;
            p.baseAtk = 4;
            p.baseDef = 1;
            break;
        case PlayerClass::Rogue:
            p.hpMax = 16;
            p.baseAtk = 3;
            p.baseDef = 0;
            break;
        case PlayerClass::Archer:
            p.hpMax = 17;
            p.baseAtk = 3;
            p.baseDef = 0;
            break;
        case PlayerClass::Wizard:
            p.hpMax = 14;
            p.baseAtk = 2;
            p.baseDef = 0;
            break;
        case PlayerClass::Adventurer:
        default:
            break;
    }

    p.hp = p.hpMax;
    p.spriteSeed = rng.nextU32();
    playerId_ = p.id;

    ents.push_back(p);

    // Starting companion: a friendly dog.
    {
        Entity d;
        d.id = nextEntityId++;
        d.kind = EntityKind::Dog;
        d.hpMax = 10;
        d.hp = d.hpMax;
        d.baseAtk = 2;
        d.baseDef = 0;
        d.spriteSeed = rng.nextU32();
        d.speed = baseSpeedFor(d.kind);
        d.willFlee = false;
        d.alerted = false;

        // Spawn near the player (prefer adjacent, but fall back to a wider search).
        Vec2i spawn{-1, -1};
        const int dirs8[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int i = 0; i < 8; ++i) {
            Vec2i c{p.pos.x + dirs8[i][0], p.pos.y + dirs8[i][1]};
            if (!dung.inBounds(c.x, c.y)) continue;
            if (!dung.isWalkable(c.x, c.y)) continue;
            if (entityAt(c.x, c.y)) continue;
            spawn = c;
            break;
        }
        if (spawn.x < 0) {
            // Extremely rare: player started boxed-in. Find the first free walkable tile.
            for (int y = 0; y < dung.height && spawn.x < 0; ++y) {
                for (int x = 0; x < dung.width && spawn.x < 0; ++x) {
                    if (!dung.isWalkable(x, y)) continue;
                    if (entityAt(x, y)) continue;
                    spawn = {x, y};
                }
            }
        }

        if (spawn.x >= 0) {
            d.pos = spawn;
            ents.push_back(d);
        }
    }

    // Starting gear
    auto give = [&](ItemKind k, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

        inv.push_back(it);
        return it.id;
    };

    int startGold = 10;
    int meleeId = 0;
    int rangedId = 0;
    int armorId = 0;

    // Class-specific kit
    switch (playerClass_) {
        case PlayerClass::Knight: {
            meleeId = give(ItemKind::Sword, 1);
            rangedId = give(ItemKind::Sling, 1);
            give(ItemKind::Rock, 10);
            armorId = give(ItemKind::ChainArmor, 1);
            give(ItemKind::PotionHealing, 1);
            give(ItemKind::PotionStrength, 1);
            startGold = 8;
        } break;
        case PlayerClass::Rogue: {
            meleeId = give(ItemKind::Dagger, 1);
            rangedId = give(ItemKind::Sling, 1);
            give(ItemKind::Rock, 8);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::Lockpick, 2);
            give(ItemKind::PotionHealing, 1);
            give(ItemKind::PotionInvisibility, 1);
            give(ItemKind::ScrollDetectTraps, 1);
            startGold = 15;
        } break;
        case PlayerClass::Archer: {
            rangedId = give(ItemKind::Bow, 1);
            give(ItemKind::Arrow, 24);
            meleeId = give(ItemKind::Dagger, 1);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::PotionHealing, 1);
            startGold = 12;
        } break;
        case PlayerClass::Wizard: {
            rangedId = give(ItemKind::WandSparks, 1);
            meleeId = give(ItemKind::Dagger, 1);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::PotionHealing, 1);
            give(ItemKind::PotionClarity, 1);
            give(ItemKind::ScrollIdentify, 2);
            startGold = 6;
        } break;
        case PlayerClass::Adventurer:
        default: {
            rangedId = give(ItemKind::Bow, 1);
            give(ItemKind::Arrow, 14);
            meleeId = give(ItemKind::Dagger, 1);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::PotionHealing, 2);
            startGold = 10;
        } break;
    }

    // Shared baseline resources (survivability + escape + scouting)
    give(ItemKind::FoodRation, hungerEnabled_ ? 2 : 1);

    // If lighting/darkness is enabled, start with a couple torches so early dark floors are survivable.
    if (lightingEnabled_) {
        give(ItemKind::Torch, 2);
    }

    give(ItemKind::ScrollTeleport, 1);
    give(ItemKind::ScrollMapping, 1);
    give(ItemKind::Gold, startGold);

    // Equip both melee + ranged so bump-attacks and FIRE both work immediately.
    equipMeleeId = meleeId;
    equipRangedId = rangedId;
    equipArmorId = armorId;

    spawnMonsters();
    spawnItems();
    spawnTraps();

    storeCurrentLevel();
    recomputeFov();


    // Encumbrance message throttling: establish initial burden state for this run.
    burdenPrev_ = burdenState();

    pushMsg("WELCOME TO PROCROGUE++.", MessageKind::System);
    pushMsg(std::string("CLASS: ") + playerClassDisplayName(), MessageKind::System);
    pushMsg("A DOG TROTS AT YOUR HEELS.", MessageKind::System);
    {
        std::ostringstream ss;
        ss << "GOAL: FIND THE AMULET OF YENDOR (DEPTH " << QUEST_DEPTH
           << "), THEN RETURN TO THE EXIT (<) TO WIN.";
        pushMsg(ss.str(), MessageKind::System);
    }
    pushMsg("PRESS ? FOR HELP. I INVENTORY. F TARGET/FIRE. M MINIMAP. TAB STATS. F12 SCREENSHOT.", MessageKind::System);
    pushMsg("MOVE: WASD/ARROWS + Y/U/B/N DIAGONALS. TIP: C SEARCH. T DISARM TRAPS. O AUTO-EXPLORE. P AUTO-PICKUP.", MessageKind::System);
    pushMsg("SAVE: F5   LOAD: F9   LOAD AUTO: F10", MessageKind::System);
}

void Game::storeCurrentLevel() {
    LevelState st;
    st.depth = depth_;
    st.dung = dung;
    st.ground = ground;
    st.traps = trapsCur;
    st.confusionGas = confusionGas_;
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

    confusionGas_ = it->second.confusionGas;
    const size_t expect = static_cast<size_t>(dung.width * dung.height);
    if (confusionGas_.size() != expect) confusionGas_.assign(expect, 0u);

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
    if (newDepth > DUNGEON_MAX_DEPTH) {
        pushMsg("THE STAIRS END HERE. YOU SENSE YOU ARE AT THE BOTTOM.", MessageKind::System, true);
        return;
    }

    // If we're leaving a shop while still owing money, the shopkeeper becomes hostile.
    if (playerInShop()) {
        const int debt = shopDebtThisDepth();
        if (debt > 0 && anyPeacefulShopkeeper(ents, playerId_)) {
            setShopkeepersAlerted(ents, playerId_, player().pos, true);
            pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);
        }
    }

    // --- Stair travel now has teeth ---
    // Using stairs is noisy, and monsters adjacent to the stairs may follow you between floors.
    // This makes "stair-dancing" less of a guaranteed reset without making stairs unusable.

    const Vec2i leavePos = player().pos;

    // Alert nearby monsters before selecting followers (so "unaware" creatures standing next to the
    // stairs can still react to you clomping up/down).
    emitNoise(leavePos, 12);

    auto canMonsterUseStairs = [&](const Entity& m) -> bool {
        if (m.hp <= 0) return false;
        if (m.id == playerId_) return false;

        // Shopkeepers don't follow you between levels (keeps shops stable and prevents leaving the
        // shop floor from turning into a cross-dungeon chase).
        if (m.kind == EntityKind::Shopkeeper) return false;

        // Webbed monsters are physically stuck.
        if (m.effects.webTurns > 0) return false;

        // Everything else can traverse stairs for now.
        return true;
    };

    // Carry companions across floors (NetHack-style): they always follow the player.
    std::vector<Entity> companions;
    companions.reserve(2);
    for (size_t i = 0; i < ents.size(); ) {
        if (ents[i].kind == EntityKind::Dog && ents[i].hp > 0) {
            companions.push_back(ents[i]);
            ents.erase(ents.begin() + static_cast<std::vector<Entity>::difference_type>(i));
            continue;
        }
        ++i;
    }

    // Collect eligible followers (adjacent to the stairs tile the player is using).
    std::vector<size_t> followerIdx;
    followerIdx.reserve(8);

    for (size_t i = 0; i < ents.size(); ++i) {
        const Entity& m = ents[i];
        if (!canMonsterUseStairs(m)) continue;
        if (chebyshev(m.pos, leavePos) > 1) continue;
        if (!m.alerted) continue;
        followerIdx.push_back(i);
    }

    // Shuffle follower order for variety, then cap (prevents pathological surround-deaths).
    for (int i = static_cast<int>(followerIdx.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(followerIdx[static_cast<size_t>(i)], followerIdx[static_cast<size_t>(j)]);
    }

    const size_t maxFollowers = 4;
    if (followerIdx.size() > maxFollowers) followerIdx.resize(maxFollowers);

    // Remove followers from the current level so the old level saves without them.
    // Erase from high -> low indices so offsets stay valid.
    std::sort(followerIdx.begin(), followerIdx.end(), [](size_t a, size_t b) { return a > b; });

    std::vector<Entity> followers;
    followers.reserve(followerIdx.size());

    for (size_t idx : followerIdx) {
        if (idx >= ents.size()) continue;
        followers.push_back(ents[idx]);
        ents.erase(ents.begin() + static_cast<std::vector<Entity>::difference_type>(idx));
    }

    storeCurrentLevel();

    // Clear transient states.
    fx.clear();
    fxExpl.clear();
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

    // Helper: find a nearby free, walkable tile (used for safe stair arrival + follower placement).
    auto isFreeTile = [&](int x, int y, bool ignorePlayer) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (!dung.isWalkable(x, y)) return false;

        const Entity* e = entityAt(x, y);
        if (!e) return true;
        if (ignorePlayer && e->id == playerId_) return true;
        return false;
    };

    auto findNearbyFreeTile = [&](Vec2i center, int maxRadius, bool avoidStairs, bool ignorePlayer) -> Vec2i {
        // Search expanding rings (chebyshev distance).
        std::vector<Vec2i> candidates;

        auto isAvoided = [&](const Vec2i& v) -> bool {
            if (!avoidStairs) return false;
            if (v == dung.stairsUp) return true;
            if (v == dung.stairsDown) return true;
            return false;
        };

        for (int r = 1; r <= maxRadius; ++r) {
            candidates.clear();
            candidates.reserve(static_cast<size_t>(8 * r));

            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int d = std::max(std::abs(dx), std::abs(dy));
                    if (d != r) continue;
                    Vec2i v{center.x + dx, center.y + dy};
                    if (!dung.inBounds(v.x, v.y)) continue;
                    if (isAvoided(v)) continue;
                    candidates.push_back(v);
                }
            }

            // Randomize the ring for less predictable spawns.
            for (int i = static_cast<int>(candidates.size()) - 1; i > 0; --i) {
                const int j = rng.range(0, i);
                std::swap(candidates[static_cast<size_t>(i)], candidates[static_cast<size_t>(j)]);
            }

            for (const Vec2i& v : candidates) {
                if (isFreeTile(v.x, v.y, ignorePlayer)) return v;
            }
        }

        // Fallback: brute scan.
        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                Vec2i v{x, y};
                if (isAvoided(v)) continue;
                if (isFreeTile(x, y, ignorePlayer)) return v;
            }
        }

        return center; // Worst-case (shouldn't happen).
    };

    auto placeFollowersNear = [&](Vec2i anchor) -> size_t {
        size_t placed = 0;

        for (auto& m : followers) {
            // Followers arrive "ready": they know where you are and will act on their next turn.
            m.alerted = true;
            m.lastKnownPlayerPos = p.pos;
            m.lastKnownPlayerAge = 0;
            // Treat stair-travel as consuming their action budget.
            m.energy = 0;

            // Avoid spawning on stairs tiles to reduce accidental soft-locks.
            Vec2i spawn = findNearbyFreeTile(anchor, 6, true, false);
            if (!dung.inBounds(spawn.x, spawn.y) || entityAt(spawn.x, spawn.y) != nullptr || !dung.isWalkable(spawn.x, spawn.y)) {
                continue;
            }

            m.pos = spawn;
            ents.push_back(m);
            ++placed;
        }

        return placed;
    };

    size_t followedCount = 0;

    if (!restored) {
        // New level: generate and populate.
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != playerId_;
        }), ents.end());
        ground.clear();
        trapsCur.clear();
        confusionGas_.clear();

        dung.generate(rng, depth_, DUNGEON_MAX_DEPTH);
        confusionGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);

        const Vec2i desiredArrival = goingDown ? dung.stairsUp : dung.stairsDown;

        // Place player before spawning so we never spawn on top of them.
        p.pos = desiredArrival;
        p.alerted = false;

        // Place stair followers before spawning so spawns avoid them.
        followedCount = placeFollowersNear(p.pos);

        // Place companions (allies) near the player on the new level.
        size_t companionCount = 0;
        for (auto& c : companions) {
            // Reset hostile-tracking state for allies.
            c.alerted = false;
            c.lastKnownPlayerPos = {-1, -1};
            c.lastKnownPlayerAge = 9999;
            // Treat stair-travel as consuming their action budget.
            c.energy = 0;

            Vec2i spawn = findNearbyFreeTile(p.pos, 4, true, false);
            if (!dung.inBounds(spawn.x, spawn.y) || entityAt(spawn.x, spawn.y) != nullptr || !dung.isWalkable(spawn.x, spawn.y)) {
                continue;
            }

            c.pos = spawn;
            ents.push_back(c);
            ++companionCount;
        }
        if (companionCount > 0) {
            pushMsg("YOUR DOG FOLLOWS YOU.", MessageKind::System, true);
        }

        spawnMonsters();
        spawnItems();
        spawnTraps();

        // Stair arrival is noisy: nearby monsters may wake and investigate.
        emitNoise(p.pos, 12);

        // Save this freshly created level.
        storeCurrentLevel();
    } else {
        // Returning to a visited level.
        const Vec2i desiredArrival = goingDown ? dung.stairsUp : dung.stairsDown;

        // If a monster is camping the stairs, don't overlap: step off to the side.
        const Entity* blocker = entityAt(desiredArrival.x, desiredArrival.y);
        if (blocker && blocker->id != playerId_) {
            p.pos = findNearbyFreeTile(desiredArrival, 6, true, true);
            pushMsg("THE STAIRS ARE BLOCKED! YOU STUMBLE ASIDE.", MessageKind::Warning, true);
        } else {
            p.pos = desiredArrival;
        }

        p.alerted = false;

        followedCount = placeFollowersNear(p.pos);

        // Place companions (allies) near the player on the new level.
        size_t companionCount = 0;
        for (auto& c : companions) {
            // Reset hostile-tracking state for allies.
            c.alerted = false;
            c.lastKnownPlayerPos = {-1, -1};
            c.lastKnownPlayerAge = 9999;
            // Treat stair-travel as consuming their action budget.
            c.energy = 0;

            Vec2i spawn = findNearbyFreeTile(p.pos, 4, true, false);
            if (!dung.inBounds(spawn.x, spawn.y) || entityAt(spawn.x, spawn.y) != nullptr || !dung.isWalkable(spawn.x, spawn.y)) {
                continue;
            }

            c.pos = spawn;
            ents.push_back(c);
            ++companionCount;
        }
        if (companionCount > 0) {
            pushMsg("YOUR DOG FOLLOWS YOU.", MessageKind::System, true);
        }

        // Stair arrival is noisy on visited floors too.
        emitNoise(p.pos, 12);
    }

    // Small heal on travel.
    p.hp = std::min(p.hpMax, p.hp + 2);

    std::ostringstream ss;
    if (goingDown) ss << "YOU DESCEND TO DEPTH " << depth_ << ".";
    else ss << "YOU ASCEND TO DEPTH " << depth_ << ".";
    pushMsg(ss.str());

    if (followedCount > 0) {
        std::ostringstream ms;
        if (goingDown) {
            if (followedCount == 1) ms << "SOMETHING FOLLOWS YOU DOWN THE STAIRS...";
            else ms << followedCount << " CREATURES FOLLOW YOU DOWN THE STAIRS...";
        } else {
            if (followedCount == 1) ms << "SOMETHING FOLLOWS YOU UP THE STAIRS...";
            else ms << followedCount << " CREATURES FOLLOW YOU UP THE STAIRS...";
        }
        pushMsg(ms.str(), MessageKind::Warning, true);
    }

    recomputeFov();

    if (goingDown && depth_ == MIDPOINT_DEPTH) {
        pushMsg("YOU HAVE REACHED THE MIDPOINT OF THE DUNGEON.", MessageKind::System, true);
    }

    if (goingDown && depth_ == QUEST_DEPTH - 1) {
        pushMsg("THE PASSAGES TWIST AND TURN... YOU ENTER A LABYRINTH.", MessageKind::System, true);
        pushMsg("IN THE DISTANCE, YOU HEAR THE DRUMMING OF HEAVY HOOVES...", MessageKind::Warning, true);
    }
    if (goingDown && depth_ == QUEST_DEPTH && !playerHasAmulet()) {
        pushMsg("A SINISTER PRESENCE LURKS AHEAD. THE AMULET MUST BE NEAR...", MessageKind::System, true);
    }

    // Safety: when autosave is enabled, also autosave on floor transitions.
    // This avoids losing progress between levels even if the turn-based autosave interval hasn't triggered yet.
    if (autosaveInterval > 0 && !isFinished()) {
        const std::string ap = defaultAutosavePath();
        if (!ap.empty()) {
            if (saveToFile(ap, true)) {
                lastAutosaveTurn = turnCount;
            }
        }
    }
}


