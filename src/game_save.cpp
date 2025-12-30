#include "game_internal.hpp"

std::string Game::defaultSavePath() const {
    if (!savePathOverride.empty()) return savePathOverride;
    return "procrogue_save.dat";
}

void Game::setSavePath(const std::string& path) {
    savePathOverride = path;
}

void Game::setActiveSlot(std::string slot) {
    // Normalize/sanitize to keep slot filenames portable.
    slot = trim(slot);
    std::string low = toLower(slot);
    if (slot.empty() || low == "default" || low == "none" || low == "off") {
        slot.clear();
    } else {
        slot = sanitizeSlotName(slot);
    }

    // Compute base paths from the current save directory.
    const std::filesystem::path baseSave = baseSavePathForSlots(*this);
    const std::filesystem::path baseAuto = baseAutosavePathForSlots(*this);

    activeSlot_ = slot;

    if (activeSlot_.empty()) {
        savePathOverride = baseSave.string();
        autosavePathOverride = baseAuto.string();
    } else {
        savePathOverride = makeSlotPath(baseSave.string(), activeSlot_).string();
        autosavePathOverride = makeSlotPath(baseAuto.string(), activeSlot_).string();
    }
}

void Game::setSaveBackups(int count) {
    saveBackups_ = clampi(count, 0, 10);
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

int Game::shopDebtTotal() const {
    int total = 0;
    for (const auto& it : inv) {
        if (it.shopPrice <= 0 || it.shopDepth <= 0) continue;
        const int n = isStackable(it.kind) ? std::max(0, it.count) : 1;
        total += it.shopPrice * n;
    }
    return total;
}

int Game::shopDebtThisDepth() const {
    const int d = depth_;
    int total = 0;
    for (const auto& it : inv) {
        if (it.shopPrice <= 0 || it.shopDepth != d) continue;
        const int n = isStackable(it.kind) ? std::max(0, it.count) : 1;
        total += it.shopPrice * n;
    }
    return total;
}

bool Game::playerInShop() const {
    const Entity& p = player();
    return roomTypeAt(dung, p.pos) == RoomType::Shop;
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

void Game::alertMonstersTo(Vec2i pos, int radius) {
    // radius<=0 means "global" (all monsters regardless of distance)
    for (auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;
        // Peaceful shopkeepers ignore generic alerts/noise.
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) continue;

        if (radius > 0) {
            int dx = std::abs(m.pos.x - pos.x);
            int dy = std::abs(m.pos.y - pos.y);
            int cheb = std::max(dx, dy);
            if (cheb > radius) continue;
        }

        m.alerted = true;
        m.lastKnownPlayerPos = pos;
        m.lastKnownPlayerAge = 0;
    }
}

namespace {
constexpr int BASE_HEARING = 8;

// A small amount of per-monster flavor: some creatures are better/worse at hearing.
// This value is used as a modifier against noise "volume" (both are in tile-cost units).
int hearingFor(EntityKind k) {
    switch (k) {
        case EntityKind::Bat:            return 12;
        case EntityKind::Wolf:           return 10;
        case EntityKind::Snake:          return 9;
        case EntityKind::Wizard:         return 9;
        case EntityKind::Spider:         return 8;
        case EntityKind::Goblin:         return 8;
        case EntityKind::Orc:            return 8;
        case EntityKind::KoboldSlinger:  return 8;
        case EntityKind::SkeletonArcher: return 7;
        case EntityKind::Troll:          return 7;
        case EntityKind::Ogre:           return 7;
        case EntityKind::Shopkeeper:     return 10;
        case EntityKind::Slime:          return 6;
        case EntityKind::Mimic:          return 5;
        default:                         return BASE_HEARING;
    }
}
} // namespace

void Game::emitNoise(Vec2i pos, int volume) {
    if (volume <= 0) return;

    const int W = dung.width;
    auto idx = [&](int x, int y) { return y * W + x; };

    // Compute the max effective volume we might need for the loudest-hearing monster.
    int maxEff = volume;
    for (const auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) continue;
        const int eff = volume + (hearingFor(m.kind) - BASE_HEARING);
        if (eff > maxEff) maxEff = eff;
    }
    maxEff = std::max(0, maxEff);

    // Dungeon-aware propagation: walls/secret doors block sound; doors muffle.
    const std::vector<int> sound = dung.computeSoundMap(pos.x, pos.y, maxEff);

    for (auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) continue;
        if (!dung.inBounds(m.pos.x, m.pos.y)) continue;

        const int eff = volume + (hearingFor(m.kind) - BASE_HEARING);
        if (eff <= 0) continue;

        const int d = sound[static_cast<size_t>(idx(m.pos.x, m.pos.y))];
        if (d < 0 || d > eff) continue;

        m.alerted = true;
        m.lastKnownPlayerPos = pos;
        m.lastKnownPlayerAge = 0;
    }
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

namespace {
BurdenState burdenStateForWeights(int weight, int capacity) {
    // Use integer comparisons to avoid float edge cases.
    // Thresholds (ratio = weight/capacity):
    //   <=1.0: unburdened
    //   <=1.2: burdened
    //   <=1.4: stressed
    //   <=1.6: strained
    //   > 1.6: overloaded
    if (capacity <= 0) {
        return (weight > 0) ? BurdenState::Overloaded : BurdenState::Unburdened;
    }

    const int64_t w = static_cast<int64_t>(std::max(0, weight));
    const int64_t cap = static_cast<int64_t>(std::max(1, capacity));

    if (w <= cap) return BurdenState::Unburdened;
    if (w <= (cap * 6) / 5) return BurdenState::Burdened;  // 1.2x
    if (w <= (cap * 7) / 5) return BurdenState::Stressed;  // 1.4x
    if (w <= (cap * 8) / 5) return BurdenState::Strained;  // 1.6x
    return BurdenState::Overloaded;
}
}

void Game::setEncumbranceEnabled(bool enabled) {
    encumbranceEnabled_ = enabled;
    burdenPrev_ = burdenState();
}

int Game::inventoryWeight() const {
    return totalWeight(inv);
}

int Game::carryCapacity() const {
    // Derive a simple carrying capacity from progression.
    // We deliberately reuse baseAtk as a "strength-like" stat to avoid bloating the save format.
    const Entity& p = player();

    const int strLike = std::max(1, p.baseAtk + talentMight_);
    int cap = 80 + (strLike * 18) + (std::max(1, charLevel) * 6);
    cap = clampi(cap, 60, 9999);
    return cap;
}

BurdenState Game::burdenState() const {
    if (!encumbranceEnabled_) return BurdenState::Unburdened;
    return burdenStateForWeights(inventoryWeight(), carryCapacity());
}

std::string Game::burdenTag() const {
    if (!encumbranceEnabled_) return std::string();
    switch (burdenState()) {
        case BurdenState::Unburdened: return std::string();
        case BurdenState::Burdened:   return "BURDENED";
        case BurdenState::Stressed:   return "STRESSED";
        case BurdenState::Strained:   return "STRAINED";
        case BurdenState::Overloaded: return "OVERLOADED";
    }
    return std::string();
}

void Game::setSneakMode(bool enabled, bool quiet) {
    if (sneakMode_ == enabled) return;
    sneakMode_ = enabled;

    if (!quiet) {
        if (sneakMode_) pushMsg("YOU BEGIN SNEAKING.", MessageKind::System, true);
        else pushMsg("YOU STOP SNEAKING.", MessageKind::System, true);
    }
}

void Game::toggleSneakMode(bool quiet) {
    setSneakMode(!sneakMode_, quiet);
}

std::string Game::sneakTag() const {
    return sneakMode_ ? "SNEAK" : std::string();
}

void Game::setLightingEnabled(bool enabled) {
    lightingEnabled_ = enabled;
    // Ensure cached lighting/FOV state matches the new mode.
    recomputeLightMap();
    recomputeFov();
}

bool Game::darknessActive() const {
    // Keep early floors bright by default; darkness starts deeper.
    return lightingEnabled_ && depth_ >= 4;
}

uint8_t Game::tileLightLevel(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= lightMap_.size()) return 255;
    return lightMap_[i];
}

std::string Game::lightTag() const {
    if (!darknessActive()) return std::string();

    // If carrying a lit torch, show remaining fuel (min across lit torches).
    int best = -1;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::TorchLit && it.charges > 0) {
            if (best < 0) best = it.charges;
            else best = std::min(best, it.charges);
        }
    }
    if (best >= 0) {
        std::ostringstream ss;
        ss << "TORCH(" << best << ")";
        return ss.str();
    }

    // Warning when you're standing in darkness without a light source.
    const Vec2i p = player().pos;
    if (dung.inBounds(p.x, p.y) && tileLightLevel(p.x, p.y) == 0) {
        return "DARK";
    }
    return std::string();
}

void Game::setAutoStepDelayMs(int ms) {
    // Clamp to sane values to avoid accidental 0ms "teleport walking".
    const int clamped = clampi(ms, 10, 500);
    autoStepDelay = clamped / 1000.0f;
}

namespace {
constexpr uint32_t SAVE_MAGIC = 0x50525356u; // 'PRSV'
constexpr uint32_t SAVE_VERSION = 18u;


// v13+: append CRC32 of the entire payload (all bytes up to but excluding the CRC field).
static uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        inited = true;
    }

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

static void appendU32LE(std::string& s, uint32_t v) {
    char b[4];
    b[0] = static_cast<char>(v & 0xFFu);
    b[1] = static_cast<char>((v >> 8) & 0xFFu);
    b[2] = static_cast<char>((v >> 16) & 0xFFu);
    b[3] = static_cast<char>((v >> 24) & 0xFFu);
    s.append(b, 4);
}

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

    // v10: blessed/uncursed/cursed state (-1..1)
    int8_t buc = static_cast<int8_t>(clampi(it.buc, -1, 1));
    writePod(out, buc);

    // v11: shop metadata (per-unit price + owning depth)
    int32_t shopPrice = static_cast<int32_t>(std::max(0, it.shopPrice));
    int32_t shopDepth = static_cast<int32_t>(std::max(0, it.shopDepth));
    writePod(out, shopPrice);
    writePod(out, shopDepth);
}

bool readItem(std::istream& in, Item& it, uint32_t version) {
    int32_t id = 0;
    uint8_t kind = 0;
    int32_t count = 0;
    int32_t charges = 0;
    uint32_t seed = 0;
    int32_t enchant = 0;
    int8_t buc = 0;
    int32_t shopPrice = 0;
    int32_t shopDepth = 0;
    if (!readPod(in, id)) return false;
    if (!readPod(in, kind)) return false;
    if (!readPod(in, count)) return false;
    if (!readPod(in, charges)) return false;
    if (!readPod(in, seed)) return false;
    if (version >= 2u) {
        if (!readPod(in, enchant)) return false;
    }
    if (version >= 10u) {
        if (!readPod(in, buc)) return false;
    }
    if (version >= 11u) {
        if (!readPod(in, shopPrice)) return false;
        if (!readPod(in, shopDepth)) return false;
    }
    it.id = id;
    it.kind = static_cast<ItemKind>(kind);
    it.count = count;
    it.charges = charges;
    it.spriteSeed = seed;
    it.enchant = enchant;
    it.buc = static_cast<int>(buc);
    it.shopPrice = static_cast<int>(shopPrice);
    it.shopDepth = static_cast<int>(shopDepth);
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
    int32_t poison = e.effects.poisonTurns;
    int32_t regenTurns = e.effects.regenTurns;
    int32_t shieldTurns = e.effects.shieldTurns;
    writePod(out, poison);
    writePod(out, regenTurns);
    writePod(out, shieldTurns);

    // v3+: additional buffs
    int32_t hasteTurns = e.effects.hasteTurns;
    int32_t visionTurns = e.effects.visionTurns;
    writePod(out, hasteTurns);
    writePod(out, visionTurns);

    // v6+: additional debuffs
    int32_t webTurns = e.effects.webTurns;
    writePod(out, webTurns);

    // v8+: invisibility
    int32_t invisTurns = e.effects.invisTurns;
    writePod(out, invisTurns);

    // v12+: confusion
    int32_t confusionTurns = e.effects.confusionTurns;
    writePod(out, confusionTurns);

    // v14+: ranged ammo count (ammo-based ranged monsters)
    int32_t ammoCount = e.rangedAmmoCount;
    writePod(out, ammoCount);


    // v17+: monster gear (melee weapon + armor). Player ignores these fields.
    writeItem(out, e.gearMelee);
    writeItem(out, e.gearArmor);
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
    int32_t rangedAmmoCount = 0;

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
    int32_t invisTurns = 0;
    int32_t confusionTurns = 0;


    Item gearMelee;
    Item gearArmor;

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

        if (version >= 8u) {
            if (!readPod(in, invisTurns)) return false;
        }

        if (version >= 12u) {
            if (!readPod(in, confusionTurns)) return false;
        }
    }

    if (version >= 14u) {
        if (!readPod(in, rangedAmmoCount)) return false;
    }


    if (version >= 17u) {
        if (!readItem(in, gearMelee, version)) return false;
        if (!readItem(in, gearArmor, version)) return false;
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

    if (version >= 14u) {
        e.rangedAmmoCount = rangedAmmoCount;
    } else {
        // Older saves had implicit infinite ammo; give ammo-based ranged monsters a reasonable default.
        if (e.kind != EntityKind::Player && e.canRanged && e.rangedAmmo != AmmoKind::None) {
            if (e.kind == EntityKind::KoboldSlinger) e.rangedAmmoCount = 18;
            else if (e.kind == EntityKind::SkeletonArcher) e.rangedAmmoCount = 12;
            else e.rangedAmmoCount = 10;
        }
    }

    e.packAI = packAI != 0;
    e.willFlee = willFlee != 0;

    e.regenChancePct = regenChance;
    e.regenAmount = regenAmt;

    e.effects.poisonTurns = poison;
    e.effects.regenTurns = regenTurns;
    e.effects.shieldTurns = shieldTurns;
    e.effects.hasteTurns = hasteTurns;
    e.effects.visionTurns = visionTurns;
    e.effects.webTurns = webTurns;
    e.effects.invisTurns = invisTurns;
    e.effects.confusionTurns = confusionTurns;


    if (version >= 17u) {
        e.gearMelee = gearMelee;
        e.gearArmor = gearArmor;
    } else {
        // Older saves: monsters had no explicit gear.
        e.gearMelee.id = 0;
        e.gearArmor.id = 0;
    }

    // Monster speed scheduling fields aren't serialized (derived from kind).
    e.speed = baseSpeedFor(e.kind);
    e.energy = 0;

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

    // Build the save payload in-memory so we can append an integrity footer (CRC)
    // while still writing atomically via a temp file.
    std::ostringstream mem(std::ios::binary | std::ios::out);

    writePod(mem, SAVE_MAGIC);
    writePod(mem, SAVE_VERSION);

    uint32_t rngState = rng.state;
    writePod(mem, rngState);

    int32_t depth = depth_;
    writePod(mem, depth);

    int32_t playerId = playerId_;
    writePod(mem, playerId);

    int32_t nextE = nextEntityId;
    int32_t nextI = nextItemId;
    writePod(mem, nextE);
    writePod(mem, nextI);

    int32_t eqM = equipMeleeId;
    int32_t eqR = equipRangedId;
    int32_t eqA = equipArmorId;
    writePod(mem, eqM);
    writePod(mem, eqR);
    writePod(mem, eqA);

    int32_t clvl = charLevel;
    int32_t xpNow = xp;
    int32_t xpNeed = xpNext;
    writePod(mem, clvl);
    writePod(mem, xpNow);
    writePod(mem, xpNeed);

    // v16+: talent allocations
    if (SAVE_VERSION >= 16u) {
        writePod(mem, static_cast<int32_t>(talentMight_));
        writePod(mem, static_cast<int32_t>(talentAgility_));
        writePod(mem, static_cast<int32_t>(talentVigor_));
        writePod(mem, static_cast<int32_t>(talentFocus_));
        writePod(mem, static_cast<int32_t>(talentPointsPending_));
        writePod(mem, static_cast<int32_t>(levelUpSel));
    }

    uint8_t over = gameOver ? 1 : 0;
    uint8_t won = gameWon ? 1 : 0;
    writePod(mem, over);
    writePod(mem, won);

    // v2+: user/options
    uint8_t autoPick = static_cast<uint8_t>(autoPickup);
    writePod(mem, autoPick);

    // v3+: pacing state
    uint32_t turnsNow = turnCount;
    int32_t natRegen = naturalRegenCounter;
    uint8_t hasteP = hastePhase ? 1 : 0;
    writePod(mem, turnsNow);
    writePod(mem, natRegen);
    writePod(mem, hasteP);

    // v5+: run meta
    uint32_t seedNow = seed_;
    uint32_t killsNow = killCount;
    int32_t maxD = maxDepth;
    writePod(mem, seedNow);
    writePod(mem, killsNow);
    writePod(mem, maxD);

    // v6+: item identification tables (run knowledge + randomized appearances)
    uint32_t kindCount = static_cast<uint32_t>(ITEM_KIND_COUNT);
    writePod(mem, kindCount);
    for (uint32_t i = 0; i < kindCount; ++i) {
        const uint8_t known = identKnown[static_cast<size_t>(i)];
        const uint8_t app = identAppearance[static_cast<size_t>(i)];
        writePod(mem, known);
        writePod(mem, app);
    }

    // v7+: hunger system state (per-run)
    uint8_t hungerEnabledTmp = hungerEnabled_ ? 1u : 0u;
    int32_t hungerTmp = static_cast<int32_t>(hunger);
    int32_t hungerMaxTmp = static_cast<int32_t>(hungerMax);
    writePod(mem, hungerEnabledTmp);
    writePod(mem, hungerTmp);
    writePod(mem, hungerMaxTmp);

    // v9+: lighting system state (per-run)
    uint8_t lightingEnabledTmp = lightingEnabled_ ? 1u : 0u;
    writePod(mem, lightingEnabledTmp);

    // v18+: sneak mode (per-run)
    if (SAVE_VERSION >= 18u) {
        uint8_t sneakEnabledTmp = sneakMode_ ? 1u : 0u;
        writePod(mem, sneakEnabledTmp);
    }

    // Player
    writeEntity(mem, player());

    // Inventory
    uint32_t invCount = static_cast<uint32_t>(inv.size());
    writePod(mem, invCount);
    for (const auto& it : inv) {
        writeItem(mem, it);
    }

    // Messages (for convenience)
    uint32_t msgCount = static_cast<uint32_t>(msgs.size());
    writePod(mem, msgCount);
    for (const auto& m : msgs) {
        uint8_t mk = static_cast<uint8_t>(m.kind);
        uint8_t fp = m.fromPlayer ? 1 : 0;
        writePod(mem, mk);
        writePod(mem, fp);
        writeString(mem, m.text);
    }

    // Levels
    uint32_t lvlCount = static_cast<uint32_t>(levels.size());
    writePod(mem, lvlCount);
    for (const auto& kv : levels) {
        const int d = kv.first;
        const LevelState& st = kv.second;

        int32_t d32 = d;
        writePod(mem, d32);

        // Dungeon
        int32_t w = st.dung.width;
        int32_t h = st.dung.height;
        writePod(mem, w);
        writePod(mem, h);
        int32_t upx = st.dung.stairsUp.x;
        int32_t upy = st.dung.stairsUp.y;
        int32_t dnx = st.dung.stairsDown.x;
        int32_t dny = st.dung.stairsDown.y;
        writePod(mem, upx);
        writePod(mem, upy);
        writePod(mem, dnx);
        writePod(mem, dny);

        uint32_t roomCount = static_cast<uint32_t>(st.dung.rooms.size());
        writePod(mem, roomCount);
        for (const auto& r : st.dung.rooms) {
            int32_t rx = r.x, ry = r.y, rw = r.w, rh = r.h;
            writePod(mem, rx);
            writePod(mem, ry);
            writePod(mem, rw);
            writePod(mem, rh);
            uint8_t rt = static_cast<uint8_t>(r.type);
            writePod(mem, rt);
        }

        uint32_t tileCount = static_cast<uint32_t>(st.dung.tiles.size());
        writePod(mem, tileCount);
        for (const auto& t : st.dung.tiles) {
            uint8_t tt = static_cast<uint8_t>(t.type);
            uint8_t explored = t.explored ? 1 : 0;
            writePod(mem, tt);
            writePod(mem, explored);
        }

        // Monsters
        uint32_t monCount = static_cast<uint32_t>(st.monsters.size());
        writePod(mem, monCount);
        for (const auto& m : st.monsters) {
            writeEntity(mem, m);
        }

        // Ground items
        uint32_t gCount = static_cast<uint32_t>(st.ground.size());
        writePod(mem, gCount);
        for (const auto& gi : st.ground) {
            int32_t gx = gi.pos.x;
            int32_t gy = gi.pos.y;
            writePod(mem, gx);
            writePod(mem, gy);
            writeItem(mem, gi.item);
        }

        // Traps
        uint32_t tCount = static_cast<uint32_t>(st.traps.size());
        writePod(mem, tCount);
        for (const auto& tr : st.traps) {
            uint8_t tk = static_cast<uint8_t>(tr.kind);
            int32_t tx = tr.pos.x;
            int32_t ty = tr.pos.y;
            uint8_t disc = tr.discovered ? 1 : 0;
            writePod(mem, tk);
            writePod(mem, tx);
            writePod(mem, ty);
            writePod(mem, disc);
        }

        // Confusion gas field (v15+)
        // Stored as a per-tile intensity map.
        if (SAVE_VERSION >= 15u) {
            const uint32_t expected = tileCount;
            uint32_t gasCount = static_cast<uint32_t>(st.confusionGas.size());
            if (gasCount != expected) gasCount = expected;
            writePod(mem, gasCount);
            for (uint32_t gi = 0; gi < gasCount; ++gi) {
                uint8_t v = 0u;
                if (gi < st.confusionGas.size()) v = st.confusionGas[static_cast<size_t>(gi)];
                writePod(mem, v);
            }
        }
    }

    std::string payload = mem.str();

    // v13+: integrity footer (CRC32 over the entire payload)
    if (SAVE_VERSION >= 13u) {
        const uint32_t c = crc32(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        appendU32LE(payload, c);
    }

    // Write to a temporary file first, then replace the target.
    std::filesystem::path tmp = p.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary);
    if (!out) {
        if (!quiet) pushMsg("FAILED TO SAVE (CANNOT OPEN FILE).");
        return false;
    }

    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out.good()) {
        if (!quiet) pushMsg("FAILED TO SAVE (WRITE ERROR).");
        out.close();
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    out.close();

    // Rotate backups of the previous file (best-effort).
    rotateFileBackups(p, saveBackups_);

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
    // Read whole file so we can verify integrity (v13+) and also attempt
    // to recover from a historical v9-v12 layout bug (missing lighting byte).
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        pushMsg("NO SAVE FILE FOUND.");
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
        return false;
    }
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
        pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
        return false;
    }

    if (bytes.size() < 8u) {
        pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
        return false;
    }

    const uint32_t magic = readU32LE(bytes.data());
    const uint32_t version = readU32LE(bytes.data() + 4);

    if (magic != SAVE_MAGIC || version == 0u || version > SAVE_VERSION) {
        pushMsg("SAVE FILE IS INVALID OR FROM ANOTHER VERSION.");
        return false;
    }

    // v13+: verify CRC32 footer (last 4 bytes).
    std::string payload;
    payload.assign(reinterpret_cast<const char*>(bytes.data()),
                   reinterpret_cast<const char*>(bytes.data() + bytes.size()));

    if (version >= 13u) {
        if (bytes.size() < 12u) {
            pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
            return false;
        }

        const uint32_t storedCrc = readU32LE(bytes.data() + bytes.size() - 4u);
        const uint32_t computedCrc = crc32(bytes.data(), bytes.size() - 4u);

        if (storedCrc != computedCrc) {
            pushMsg("SAVE FILE FAILED INTEGRITY CHECK (CRC MISMATCH).");
            return false;
        }

        // Exclude CRC footer from the parser.
        payload.assign(reinterpret_cast<const char*>(bytes.data()),
                       reinterpret_cast<const char*>(bytes.data() + (bytes.size() - 4u)));
    }

    auto tryParse = [&](bool assumeLightingByte, bool reportErrors) -> bool {
        std::istringstream in(payload, std::ios::binary | std::ios::in);

        uint32_t magic2 = 0;
        uint32_t ver2 = 0;
        if (!readPod(in, magic2) || !readPod(in, ver2) || magic2 != SAVE_MAGIC || ver2 == 0u || ver2 > SAVE_VERSION) {
            if (reportErrors) pushMsg("SAVE FILE IS INVALID OR FROM ANOTHER VERSION.");
            return false;
        }

        const uint32_t ver = ver2;

        auto fail = [&]() -> bool {
            if (reportErrors) pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
            return false;
        };

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

        if (!readPod(in, rngState)) return fail();
        if (!readPod(in, depth)) return fail();
        if (!readPod(in, pId)) return fail();
        if (!readPod(in, nextE)) return fail();
        if (!readPod(in, nextI)) return fail();
        if (!readPod(in, eqM)) return fail();
        if (!readPod(in, eqR)) return fail();
        if (!readPod(in, eqA)) return fail();
        if (!readPod(in, clvl)) return fail();
        if (!readPod(in, xpNow)) return fail();
        if (!readPod(in, xpNeed)) return fail();

        // v16+: talent allocations
        int32_t tMight = 0, tAgi = 0, tVig = 0, tFoc = 0, tPending = 0, tSel = 0;
        if (ver >= 16u) {
            if (!readPod(in, tMight)) return fail();
            if (!readPod(in, tAgi)) return fail();
            if (!readPod(in, tVig)) return fail();
            if (!readPod(in, tFoc)) return fail();
            if (!readPod(in, tPending)) return fail();
            if (!readPod(in, tSel)) return fail();
        }

        if (!readPod(in, over)) return fail();
        if (!readPod(in, won)) return fail();

        if (ver >= 2u) {
            if (!readPod(in, autoPick)) return fail();
        }

        if (ver >= 3u) {
            if (!readPod(in, turnsNow)) return fail();
            if (!readPod(in, natRegen)) return fail();
            if (!readPod(in, hasteP)) return fail();
        }

        if (ver >= 5u) {
            if (!readPod(in, seedNow)) return fail();
            if (!readPod(in, killsNow)) return fail();
            if (!readPod(in, maxD)) return fail();
        }

        // v6+: item identification tables
        std::array<uint8_t, ITEM_KIND_COUNT> identKnownTmp{};
        std::array<uint8_t, ITEM_KIND_COUNT> identAppTmp{};
        identKnownTmp.fill(1); // older saves had fully-known item names
        identAppTmp.fill(0);

        if (ver >= 6u) {
            uint32_t kindCount = 0;
            if (!readPod(in, kindCount)) return fail();
            for (uint32_t i = 0; i < kindCount; ++i) {
                uint8_t known = 1;
                uint8_t app = 0;
                if (!readPod(in, known)) return fail();
                if (!readPod(in, app)) return fail();
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
        if (ver >= 7u) {
            if (!readPod(in, hungerEnabledTmp)) return fail();
            if (!readPod(in, hungerTmp)) return fail();
            if (!readPod(in, hungerMaxTmp)) return fail();
        }

        // v9+: lighting system state (per-run)
        uint8_t lightingEnabledTmp = lightingEnabled_ ? 1u : 0u;
        if (ver >= 9u) {
            if (assumeLightingByte) {
                if (!readPod(in, lightingEnabledTmp)) return fail();
            } else {
                // Legacy bug: some v9-v12 builds forgot to write this byte.
                // Keep the current setting (from settings.ini) in that case.
                lightingEnabledTmp = lightingEnabled_ ? 1u : 0u;
            }
        }

        // v18+: sneak mode (per-run)
        uint8_t sneakEnabledTmp = 0u;
        if (ver >= 18u) {
            if (!readPod(in, sneakEnabledTmp)) return fail();
        }

        Entity p;
        if (!readEntity(in, p, ver)) return fail();

        // Sanity checks to catch stream misalignment (e.g., legacy missing lighting byte).
        if (p.kind != EntityKind::Player || p.id != pId || p.id == 0) {
            return fail();
        }

        uint32_t invCount = 0;
        if (!readPod(in, invCount)) return fail();
        std::vector<Item> invTmp;
        invTmp.reserve(invCount);
        for (uint32_t i = 0; i < invCount; ++i) {
            Item it;
            if (!readItem(in, it, ver)) return fail();
            invTmp.push_back(it);
        }

        uint32_t msgCount = 0;
        if (!readPod(in, msgCount)) return fail();
        std::vector<Message> msgsTmp;
        msgsTmp.reserve(msgCount);
        for (uint32_t i = 0; i < msgCount; ++i) {
            if (ver >= 2u) {
                uint8_t mk = 0;
                uint8_t fp = 1;
                std::string s;
                if (!readPod(in, mk)) return fail();
                if (!readPod(in, fp)) return fail();
                if (!readString(in, s)) return fail();
                Message m;
                m.text = std::move(s);
                m.kind = static_cast<MessageKind>(mk);
                m.fromPlayer = fp != 0;
                msgsTmp.push_back(std::move(m));
            } else {
                std::string s;
                if (!readString(in, s)) return fail();
                msgsTmp.push_back({std::move(s), MessageKind::Info, true});
            }
        }

        uint32_t lvlCount = 0;
        if (!readPod(in, lvlCount)) return fail();
        std::map<int, LevelState> levelsTmp;

        for (uint32_t li = 0; li < lvlCount; ++li) {
            int32_t d32 = 0;
            if (!readPod(in, d32)) return fail();

            int32_t w = 0, h = 0;
            int32_t upx = 0, upy = 0, dnx = 0, dny = 0;
            if (!readPod(in, w)) return fail();
            if (!readPod(in, h)) return fail();
            if (!readPod(in, upx)) return fail();
            if (!readPod(in, upy)) return fail();
            if (!readPod(in, dnx)) return fail();
            if (!readPod(in, dny)) return fail();

            LevelState st;
            st.depth = d32;
            st.dung = Dungeon(w, h);
            st.dung.stairsUp = { upx, upy };
            st.dung.stairsDown = { dnx, dny };

            uint32_t roomCount = 0;
            if (!readPod(in, roomCount)) return fail();
            st.dung.rooms.clear();
            st.dung.rooms.reserve(roomCount);
            for (uint32_t ri = 0; ri < roomCount; ++ri) {
                int32_t rx = 0, ry = 0, rw = 0, rh = 0;
                uint8_t rt = 0;
                if (!readPod(in, rx)) return fail();
                if (!readPod(in, ry)) return fail();
                if (!readPod(in, rw)) return fail();
                if (!readPod(in, rh)) return fail();
                if (!readPod(in, rt)) return fail();
                Room r;
                r.x = rx;
                r.y = ry;
                r.w = rw;
                r.h = rh;
                r.type = static_cast<RoomType>(rt);
                st.dung.rooms.push_back(r);
            }

            uint32_t tileCount = 0;
            if (!readPod(in, tileCount)) return fail();
            st.dung.tiles.assign(tileCount, Tile{});
            for (uint32_t ti = 0; ti < tileCount; ++ti) {
                uint8_t tt = 0;
                uint8_t explored = 0;
                if (!readPod(in, tt)) return fail();
                if (!readPod(in, explored)) return fail();
                st.dung.tiles[ti].type = static_cast<TileType>(tt);
                st.dung.tiles[ti].visible = false;
                st.dung.tiles[ti].explored = explored != 0;
            }

            uint32_t monCount = 0;
            if (!readPod(in, monCount)) return fail();
            st.monsters.clear();
            st.monsters.reserve(monCount);
            for (uint32_t mi = 0; mi < monCount; ++mi) {
                Entity m;
                if (!readEntity(in, m, ver)) return fail();
                st.monsters.push_back(m);
            }

            uint32_t gCount = 0;
            if (!readPod(in, gCount)) return fail();
            st.ground.clear();
            st.ground.reserve(gCount);
            for (uint32_t gi = 0; gi < gCount; ++gi) {
                int32_t gx = 0, gy = 0;
                if (!readPod(in, gx)) return fail();
                if (!readPod(in, gy)) return fail();
                GroundItem gr;
                gr.pos = { gx, gy };
                if (!readItem(in, gr.item, ver)) return fail();
                st.ground.push_back(gr);
            }

            // Traps (v2+)
            st.traps.clear();
            if (ver >= 2u) {
                uint32_t tCount = 0;
                if (!readPod(in, tCount)) return fail();
                st.traps.reserve(tCount);
                for (uint32_t ti = 0; ti < tCount; ++ti) {
                    uint8_t tk = 0;
                    int32_t tx = 0, ty = 0;
                    uint8_t disc = 0;
                    if (!readPod(in, tk)) return fail();
                    if (!readPod(in, tx)) return fail();
                    if (!readPod(in, ty)) return fail();
                    if (!readPod(in, disc)) return fail();
                    Trap tr;
                    tr.kind = static_cast<TrapKind>(tk);
                    tr.pos = { tx, ty };
                    tr.discovered = disc != 0;
                    st.traps.push_back(tr);
                }
            }

            // Confusion gas field (v15+)
            st.confusionGas.clear();
            if (ver >= 15u) {
                uint32_t gasCount = 0;
                if (!readPod(in, gasCount)) return fail();

                std::vector<uint8_t> gasTmp;
                gasTmp.assign(gasCount, 0u);
                for (uint32_t gi = 0; gi < gasCount; ++gi) {
                    uint8_t gv = 0;
                    if (!readPod(in, gv)) return fail();
                    gasTmp[gi] = gv;
                }

                // Normalize size to the dungeon tile count when possible (defensive against older/partial saves).
                if (tileCount > 0) {
                    st.confusionGas.assign(tileCount, 0u);
                    const uint32_t copyN = std::min(gasCount, tileCount);
                    for (uint32_t i = 0; i < copyN; ++i) {
                        st.confusionGas[static_cast<size_t>(i)] = gasTmp[static_cast<size_t>(i)];
                    }
                } else {
                    st.confusionGas = std::move(gasTmp);
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
        if (ver >= 16u) {
            talentMight_ = clampi(tMight, -5, 50);
            talentAgility_ = clampi(tAgi, -5, 50);
            talentVigor_ = clampi(tVig, -5, 50);
            talentFocus_ = clampi(tFoc, -5, 50);
            talentPointsPending_ = clampi(tPending, 0, 50);
            levelUpSel = clampi(tSel, 0, 3);
        } else {
            talentMight_ = 0;
            talentAgility_ = 0;
            talentVigor_ = 0;
            talentFocus_ = 0;
            talentPointsPending_ = 0;
            levelUpSel = 0;
        }
        levelUpOpen = (talentPointsPending_ > 0);
        gameOver = over != 0;
        gameWon = won != 0;
        if (ver >= 4u) {
            autoPickup = static_cast<AutoPickupMode>(autoPick);
            // Accept known modes; clamp anything else to Gold.
            if (autoPick > static_cast<uint8_t>(AutoPickupMode::Smart)) autoPickup = AutoPickupMode::Gold;
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
        if (ver >= 7u) {
            hungerEnabled_ = (hungerEnabledTmp != 0);
            hungerMax = (hungerMaxTmp > 0) ? static_cast<int>(hungerMaxTmp) : 800;
            hunger = clampi(static_cast<int>(hungerTmp), 0, hungerMax);
        } else {
            // Pre-hunger saves: keep the current setting, but start fully fed.
            if (hungerMax <= 0) hungerMax = 800;
            hunger = hungerMax;
        }
        hungerStatePrev = hungerStateFor(hunger, hungerMax);

        // v9+: lighting state
        lightingEnabled_ = (lightingEnabledTmp != 0);

        // v18+: sneak mode
        sneakMode_ = (ver >= 18u) ? (sneakEnabledTmp != 0) : false;

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
        invIdentifyMode = false;
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

        // Encumbrance message throttling: avoid spurious "YOU FEEL BURDENED" on the first post-load turn.
        burdenPrev_ = burdenState();

        if (reportErrors) pushMsg("GAME LOADED.");
        return true;
    };

    // Normal parse first. For versions 9-12, some builds accidentally omitted the
    // lighting byte; if so, fall back to the legacy layout.
    const bool canFallback = (version >= 9u && version < 13u);
    if (canFallback) {
        if (tryParse(true, false)) {
            pushMsg("GAME LOADED.");
            return true;
        }
        if (tryParse(false, true)) {
            pushMsg("LOADED LEGACY SAVE (FIXED LIGHTING STATE FORMAT).", MessageKind::System);
            return true;
        }
        return false;
    }

    return tryParse(true, true);
}
