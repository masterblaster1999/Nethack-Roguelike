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

const Entity& Game::player() const {
    for (const auto& e : ents) if (e.id == playerId_) return e;
    return ents.front();
}

Entity& Game::playerMut() {
    for (auto& e : ents) if (e.id == playerId_) return e;
    return ents.front();
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
        atk += (w->buc < 0 ? -1 : (w->buc > 0 ? 1 : 0));
    }
    return atk;
}

int Game::playerDefense() const {
    int def = player().baseDef;
    if (const Item* a = equippedArmor()) {
        def += itemDef(a->kind).defense;
        def += a->enchant;
        def += (a->buc < 0 ? -1 : (a->buc > 0 ? 1 : 0));
    }
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
    invIdentifyMode = false;
    invSel = 0;
    targeting = false;
    targetLine.clear();
    targetValid = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;

    msgs.clear();
    msgScroll = 0;

    // autoPickup is a user setting; do not reset it between runs.

    // Randomize potion/scroll appearances and reset identification knowledge.
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

    // Hunger pacing (optional setting; stored per-run in save files).
    hungerMax = 800;
    hunger = hungerMax;
    hungerStatePrev = hungerStateFor(hunger, hungerMax);

    dung.generate(rng, depth_, DUNGEON_MAX_DEPTH);

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
        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

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

    // If lighting/darkness is enabled, start with a couple torches so early dark floors are survivable.
    if (lightingEnabled_) {
        give(ItemKind::Torch, 2);
    }
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


    // Encumbrance message throttling: establish initial burden state for this run.
    burdenPrev_ = burdenState();

    pushMsg("WELCOME TO PROCROGUE++.", MessageKind::System);
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

        dung.generate(rng, depth_, DUNGEON_MAX_DEPTH);

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

    if (goingDown && depth_ == MIDPOINT_DEPTH) {
        pushMsg("YOU HAVE REACHED THE MIDPOINT OF THE DUNGEON.", MessageKind::System, true);
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


