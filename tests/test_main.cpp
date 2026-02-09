#include "game.hpp"
#include "action_info.hpp"
#include "overworld.hpp"
#include "settings.hpp"
#include "scent_field.hpp"
#include "wfc.hpp"
#include "vault_prefab_catalog.hpp"
#include "noise_localization.hpp"
#include "proc_spells.hpp"
#include "proc_names.hpp"
#include "crafting_gen.hpp"
#include "graffiti_gen.hpp"
#include "trap_salvage_gen.hpp"
#include "sigil_gen.hpp"
#include "wards.hpp"
#include "ident_gen.hpp"
#include "shop_profile_gen.hpp"
#include "farm_gen.hpp"
#include "ecosystem_loot.hpp"
#include "shrine_profile_gen.hpp"
#include "victory_gen.hpp"
#include "spritegen.hpp"
#include <queue>
#include <unordered_map>


struct GameTestAccess {
    static Dungeon& dung(Game& g) { return g.dung; }
    static const Dungeon& dung(const Game& g) { return g.dung; }

    struct CraftProbe {
        uint32_t sig = 0;
        ItemKind kind = ItemKind::Dagger;
        EcosystemKind eco = EcosystemKind::None;
    };

    static CraftProbe probeCraft(Game& g, const Item& a, const Item& b) {
        const Game::CraftComputed cc = g.computeCraftComputed(a, b);
        CraftProbe p;
        p.sig = cc.out.spriteSeed;
        p.kind = cc.out.kind;
        p.eco = cc.ecosystem;
        return p;
    }

static void discoverOverworldChunk(Game& g, int x, int y) {
    g.markOverworldDiscovered(x, y);
}

static void setOverworldChunkFeatureFlags(Game& g, int x, int y, uint8_t flags) {
    const Game::OverworldKey k{x, y};
    g.overworldVisited_.insert(k);
    g.overworldFeatureFlags_[k] = flags;
}

static void setOverworldChunkTerrainSummary(Game& g, int x, int y, int chasm, int boulder, int pillar) {
    const Game::OverworldKey k{x, y};
    g.overworldVisited_.insert(k);
    Game::OverworldTerrainSummary s;
    s.chasmTiles = chasm;
    s.boulderTiles = boulder;
    s.pillarTiles = pillar;
    g.overworldTerrainSummary_[k] = s;
}


static void clearOverworldChunkCache(Game& g) {
    g.overworldChunks_.clear();
}
};

#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

int currentProcessId() {
#if defined(_WIN32)
    return static_cast<int>(_getpid());
#else
    return static_cast<int>(getpid());
#endif
}

std::string sanitizePathToken(std::string s) {
    for (char& ch : s) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '-' || ch == '_';
        if (!ok) ch = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

std::string readEnvVar(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        if (value) free(value);
        return {};
    }
    std::string out(value);
    free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

fs::path testTempDir() {
    static const fs::path dir = []() {
        // Prefer a real temp directory but fall back to the current directory if
        // the platform doesn't provide one (or it throws).
        fs::path base;
        try {
            base = fs::temp_directory_path();
        } catch (...) {
            base = fs::current_path();
        }

        std::string testName = readEnvVar("PROCROGUE_TEST_NAME");
        if (testName.empty()) testName = "suite";
        testName = sanitizePathToken(testName);

        fs::path p = base / "procrogue_tests" /
                     (testName + "_" + std::to_string(currentProcessId()));
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }();
    return dir;
}

fs::path testTempFile(const std::string& name) {
    return testTempDir() / name;
}

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };

    std::string h; h.reserve(haystack.size());
    for (unsigned char c : haystack) h.push_back(lower(c));

    std::string n; n.reserve(needle.size());
    for (unsigned char c : needle) n.push_back(lower(c));

    return h.find(n) != std::string::npos;
}

int manhattan2(Vec2i a, Vec2i b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

std::string gCurrentTestName = "<unbound>";
bool gFailureLogged = false;

void setCurrentTestName(const char* name) {
    if (name && *name) {
        gCurrentTestName = name;
    } else {
        gCurrentTestName = "<unbound>";
    }
    gFailureLogged = false;
}

bool checkFailed(const char* fn, int line, const char* cond, const std::string& detail = {}) {
    gFailureLogged = true;
    std::cerr << "[FAIL] " << gCurrentTestName << " " << fn << ":" << line << " CHECK(" << cond << ")";
    if (!detail.empty()) {
        std::cerr << " -- " << detail;
    }
    std::cerr << "\n";
    return false;
}

std::string colorToString(const Color& c) {
    std::ostringstream oss;
    oss << "("
        << static_cast<int>(c.r) << ","
        << static_cast<int>(c.g) << ","
        << static_cast<int>(c.b) << ","
        << static_cast<int>(c.a) << ")";
    return oss.str();
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        return checkFailed(__FUNCTION__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_MSG(cond, msgExpr) do { \
    if (!(cond)) { \
        std::ostringstream _checkMsgOss; \
        _checkMsgOss << msgExpr; \
        return checkFailed(__FUNCTION__, __LINE__, #cond, _checkMsgOss.str()); \
    } \
} while (0)


bool test_wfc_solver_basic() {
    const int w = 5;
    const int h = 5;
    const int nTiles = 2;

    std::vector<uint32_t> allow[4];
    for (int dir = 0; dir < 4; ++dir) allow[dir].assign(static_cast<size_t>(nTiles), 0u);

    const uint32_t a = 1u << 0;
    const uint32_t b = 1u << 1;

    // Simple "same-tile adjacency" rule: A next to A, B next to B.
    for (int dir = 0; dir < 4; ++dir) {
        allow[dir][0] = a;
        allow[dir][1] = b;
    }

    std::vector<float> weights = {1.0f, 1.0f};

    RNG rng(12345u);
    RNG ref = rng;
    (void)ref.nextU32(); // solve() should advance rng by one draw per attempt.

    std::vector<uint8_t> out;
    const bool ok = wfc::solve(w, h, nTiles, allow, weights, rng, /*initialDomains=*/{}, out, /*maxRestarts=*/0);
    CHECK(ok);
    CHECK(out.size() == static_cast<size_t>(w * h));
    CHECK(rng.state == ref.state);

    auto at = [&](int x, int y) -> uint8_t {
        return out[static_cast<size_t>(y * w + x)];
    };

    // Verify adjacency constraint.
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t t = at(x, y);
            if (x + 1 < w) CHECK(at(x + 1, y) == t);
            if (y + 1 < h) CHECK(at(x, y + 1) == t);
        }
    }

    return true;
}

bool test_wfc_solver_unsat_forced_contradiction() {
    const int w = 2;
    const int h = 1;
    const int nTiles = 2;

    std::vector<uint32_t> allow[4];
    for (int dir = 0; dir < 4; ++dir) allow[dir].assign(static_cast<size_t>(nTiles), 0u);

    const uint32_t a = 1u << 0;
    const uint32_t b = 1u << 1;

    // Same-tile adjacency rule again.
    for (int dir = 0; dir < 4; ++dir) {
        allow[dir][0] = a;
        allow[dir][1] = b;
    }

    std::vector<float> weights = {1.0f, 1.0f};

    const uint32_t full = wfc::allMask(nTiles);
    std::vector<uint32_t> dom(static_cast<size_t>(w * h), full);

    // Force an immediate contradiction: [A][B] but A cannot neighbor B.
    dom[0] = a;
    dom[1] = b;

    RNG rng(999u);
    RNG ref = rng;
    (void)ref.nextU32();

    std::vector<uint8_t> out;
    const bool ok = wfc::solve(w, h, nTiles, allow, weights, rng, dom, out, /*maxRestarts=*/0);
    CHECK(!ok);
    CHECK(rng.state == ref.state);
    return true;
}

bool test_scent_field_wind_bias() {
    // A tiny self-contained test of the shared scent-field helper.
    //
    // With wind blowing east, scent should spread further to the east than to the west.
    const int W = 7;
    const int H = 3;

    auto idx = [&](int x, int y) -> size_t {
        return static_cast<size_t>(y * W + x);
    };

    auto walkable = [&](int x, int y) -> bool {
        return (x >= 0 && y >= 0 && x < W && y < H);
    };

    auto fxAt = [&](int /*x*/, int /*y*/) -> ScentCellFx {
        return ScentCellFx{};
    };

    const Vec2i src{3, 1};

    // Windy: eastward draft.
    {
        std::vector<uint8_t> f;
        ScentFieldParams p;
        p.windDir = {1, 0};
        p.windStrength = 3;

        updateScentField(W, H, f, src, 255u, walkable, fxAt, p);

        const uint8_t east = f[idx(src.x + 1, src.y)];
        const uint8_t west = f[idx(src.x - 1, src.y)];
        CHECK(east > west);
    }

    // Calm: symmetric spread.
    {
        std::vector<uint8_t> f;
        ScentFieldParams p;
        p.windDir = {0, 0};
        p.windStrength = 0;

        updateScentField(W, H, f, src, 255u, walkable, fxAt, p);

        const uint8_t east = f[idx(src.x + 1, src.y)];
        const uint8_t west = f[idx(src.x - 1, src.y)];
        CHECK(east == west);
    }

    return true;
}

bool test_ecosystem_stealth_fx_sanity() {
    // Basic invariants for the ecosystem stealth ecology table.
    // (This guards against accidental all-zero or sign-flip regressions.)

    {
        const EcosystemFx fx = ecosystemFx(EcosystemKind::FungalBloom);
        CHECK(fx.footstepNoiseDelta < 0);
        CHECK(fx.hearingMaskDelta > 0);
        CHECK(fx.scentDecayDelta <= 0);
    }

    {
        const EcosystemFx fx = ecosystemFx(EcosystemKind::CrystalGarden);
        CHECK(fx.hearingMaskDelta < 0);
        CHECK(fx.listenRangeDelta > 0);
    }

    {
        const EcosystemFx fx = ecosystemFx(EcosystemKind::FloodedGrotto);
        CHECK(fx.footstepNoiseDelta >= 2);
        CHECK(fx.scentDecayDelta > 0);
        CHECK(fx.hearingMaskDelta > 0);
        CHECK(fx.listenRangeDelta < 0);
    }

    return true;
}


bool test_ecosystem_weapon_ego_loot_bias() {
    // Guardrails for the ecosystem->ego weight table.
    // These keep biome flavor stable and prevent accidental sign flips.

    CHECK(ecoWeaponEgoWeightDelta(EcosystemKind::FungalBloom, ItemEgo::Venom) > 0);
    CHECK(ecoWeaponEgoWeightDelta(EcosystemKind::AshenRidge, ItemEgo::Flaming) > 0);
    CHECK(ecoWeaponEgoWeightDelta(EcosystemKind::RustVeins, ItemEgo::Corrosive) > 0);

    // Flooded grotto "quenches" fire brands.
    CHECK(ecoWeaponEgoWeightDelta(EcosystemKind::FloodedGrotto, ItemEgo::Flaming) < 0);

    // None should be neutral.
    CHECK(ecoWeaponEgoWeightDelta(EcosystemKind::None, ItemEgo::Venom) == 0);

    // Chance multipliers are intentionally mild.
    CHECK(ecoWeaponEgoChanceMul(EcosystemKind::CrystalGarden) >= 1.0f);
    CHECK(ecoWeaponEgoChanceMul(EcosystemKind::FloodedGrotto) <= 1.0f);

    return true;
}


bool test_proc_leylines_basic() {
    // Basic sanity + determinism checks for the procedural leyline field.
    //
    // This is intentionally *not* a brittle golden-hash test: we only guard
    // against degenerate outputs (all-zero) and non-determinism.

    const uint32_t seedA = 0xA11CE5EDu;
    const uint32_t seedB = 0xA11CE5EEu;

    auto fillFloor = [](Dungeon& d) {
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                Tile& t = d.at(x, y);
                t.type = TileType::Floor;
                t.explored = true;
                t.visible = true;
            }
        }
    };

    Dungeon d(60, 40);
    fillFloor(d);
    d.ensureMaterials(seedA, DungeonBranch::Main, 7, Game::DUNGEON_MAX_DEPTH);

    int non0 = 0;
    int maxV = 0;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const int v = static_cast<int>(d.leylineAtCached(x, y));
            if (v > 0) ++non0;
            if (v > maxV) maxV = v;
        }
    }

    // Guardrails: should not collapse to all-zero, and should have at least
    // one reasonably strong line/node on a medium map.
    CHECK(non0 > (d.width * d.height) / 30);
    CHECK(maxV >= 180);

    // Determinism: repeated ensureMaterials calls with the same key should not change values.
    const uint8_t sample0 = d.leylineAtCached(10, 10);
    d.ensureMaterials(seedA, DungeonBranch::Main, 7, Game::DUNGEON_MAX_DEPTH);
    CHECK(d.leylineAtCached(10, 10) == sample0);

    // Variation: changing the seed should change the field somewhere.
    Dungeon d2(60, 40);
    fillFloor(d2);
    d2.ensureMaterials(seedB, DungeonBranch::Main, 7, Game::DUNGEON_MAX_DEPTH);

    bool anyDiff = false;
    for (int y = 0; y < d.height && !anyDiff; ++y) {
        for (int x = 0; x < d.width; ++x) {
            if (d2.leylineAtCached(x, y) != d.leylineAtCached(x, y)) {
                anyDiff = true;
                break;
            }
        }
    }
    CHECK(anyDiff);

    return true;
}

bool test_new_game_determinism() {
    Game a;
    a.newGame(123456u);
    const uint64_t ha = a.determinismHash();

    Game b;
    b.newGame(123456u);
    const uint64_t hb = b.determinismHash();

    CHECK(ha == hb);
    return true;
}

bool test_save_load_roundtrip() {
    Game g;
    g.newGame(98765u);

    // Spend a few deterministic turns so we cover more than the turn-0 state.
    for (int i = 0; i < 5; ++i) {
        g.handleAction(Action::Wait);
    }

    const uint32_t turnsBefore = g.turns();
    const uint64_t h1 = g.determinismHash();

    const fs::path p = testTempFile("procrogue_test_save.prs");
    std::error_code ec;
    fs::remove(p, ec);

    CHECK(g.saveToFile(p.string(), true));
    CHECK(fs::exists(p));

    // Saving should not mutate deterministic simulation state.
    // (Historically this could fail if the cached per-level store wasn't kept
    // in sync with the active level and the hash included both.)
    const uint64_t hAfterSave = g.determinismHash();
    CHECK(h1 == hAfterSave);

    Game g2;
    CHECK(g2.loadFromFile(p.string()));

    const uint64_t h2 = g2.determinismHash();
    CHECK(h1 == h2);
    CHECK(g2.turns() == turnsBefore);

    fs::remove(p, ec);
    return true;
}

bool test_save_load_preserves_sneak() {
    Game g;
    g.setPlayerClass(PlayerClass::Rogue);
    g.newGame(424242u);

    const uint64_t hOff = g.determinismHash();

    // Sneak should affect deterministic simulation state (and therefore the hash).
    g.setSneakMode(true, true);
    const uint64_t hOn = g.determinismHash();
    CHECK(hOff != hOn);

    // Spend a few deterministic turns so we cover more than the turn-0 state.
    for (int i = 0; i < 3; ++i) {
        g.handleAction(Action::Wait);
    }

    const uint32_t turnsBefore = g.turns();
    const uint64_t h1 = g.determinismHash();

    const fs::path p = testTempFile("procrogue_test_save_sneak.prs");
    std::error_code ec;
    fs::remove(p, ec);

    CHECK(g.saveToFile(p.string(), true));
    CHECK(fs::exists(p));

    Game g2;
    CHECK(g2.loadFromFile(p.string()));

    CHECK(g2.determinismHash() == h1);
    CHECK(g2.turns() == turnsBefore);
    CHECK(g2.isSneaking());

    fs::remove(p, ec);
    return true;
}


bool test_settings_minimap_zoom_clamp() {
    const fs::path p = testTempFile("procrogue_test_settings_minimap.ini");
    std::error_code ec;
    fs::remove(p, ec);

    // Default should be 0 when not specified.
    {
        std::ofstream out(p);
        out << "# minimal settings for test\n";
    }
    {
        Settings s = loadSettings(p.string());
        CHECK(s.minimapZoom == 0);
    }

    // Clamp high values.
    {
        std::ofstream out(p);
        out << "minimap_zoom = 999\n";
    }
    {
        Settings s = loadSettings(p.string());
        CHECK(s.minimapZoom == 3);
    }

    // Clamp low values.
    {
        std::ofstream out(p);
        out << "minimap_zoom = -999\n";
    }
    {
        Settings s = loadSettings(p.string());
        CHECK(s.minimapZoom == -3);
    }

    // Pass-through in-range values.
    {
        std::ofstream out(p);
        out << "minimap_zoom = 2\n";
    }
    {
        Settings s = loadSettings(p.string());
        CHECK(s.minimapZoom == 2);
    }

    fs::remove(p, ec);
    return true;
}


bool test_action_palette_executes_actions() {
    Game g;
    g.newGame(424242u);

    // Open the extended command prompt and use the action-palette shorthand.
    CHECK(!g.isInventoryOpen());

    g.handleAction(Action::Command);
    CHECK(g.isCommandOpen());

    // Autocomplete should expand action tokens after '@'.
    g.commandTextInput("@inv");
    g.commandAutocomplete();
    CHECK(g.commandBuffer() == "@inventory");

    // Confirm should dispatch the action and open inventory.
    g.handleAction(Action::Confirm);
    CHECK(!g.isCommandOpen());
    CHECK(g.isInventoryOpen());

    // And cancel should close inventory normally.
    g.handleAction(Action::Cancel);
    CHECK(!g.isInventoryOpen());

    return true;
}

bool test_action_info_view_turn_tokens() {
    const actioninfo::ActionInfo* left = actioninfo::findByToken("view_turn_left");
    CHECK(left != nullptr);
    CHECK(left->action == Action::ViewTurnLeft);

    const actioninfo::ActionInfo* right = actioninfo::findByToken("view_turn_right");
    CHECK(right != nullptr);
    CHECK(right->action == Action::ViewTurnRight);

    return true;
}



bool test_extended_command_replay_record_requests() {
    Game g;
    g.newGame(10101u);

    // Start recording via the command prompt.
    g.handleAction(Action::Command);
    CHECK(g.isCommandOpen());

    g.commandTextInput("record my_test_replay.prr");
    g.handleAction(Action::Confirm);
    CHECK(!g.isCommandOpen());

    CHECK(g.replayRecordStartRequested());
    CHECK(g.replayRecordStartPath() == "my_test_replay.prr");
    g.clearReplayRecordStartRequest();

    // Stop recording via command prompt.
    // (We mark the indicator active to simulate main.cpp turning it on.)
    g.setReplayRecordingIndicator(true, "my_test_replay.prr");

    g.handleAction(Action::Command);
    CHECK(g.isCommandOpen());

    g.commandTextInput("stoprecord");
    g.handleAction(Action::Confirm);
    CHECK(!g.isCommandOpen());

    CHECK(g.replayRecordStopRequested());
    g.clearReplayRecordStopRequest();

    return true;
}



bool test_noise_localization_determinism() {
    // Basic sanity: threshold sounds should yield a non-zero search radius.
    const Vec2i src{10, 10};
    const uint32_t seed = 12345u;
    const uint32_t turn = 77u;
    const int volume = 6;
    const int eff = 6;
    const int dist = 6;

    const int r = noiseInvestigateRadius(volume, eff, dist);
    CHECK(r > 0);

    const uint32_t h1 = noiseInvestigateHash(seed, turn, 1, src, volume, eff, dist);
    const uint32_t h2 = noiseInvestigateHash(seed, turn, 1, src, volume, eff, dist);
    CHECK(h1 == h2);

    const Vec2i o1 = noiseInvestigateOffset(h1, r);
    const Vec2i o2 = noiseInvestigateOffset(h2, r);
    CHECK(o1.x == o2.x);
    CHECK(o1.y == o2.y);
    CHECK(std::abs(o1.x) + std::abs(o1.y) <= r);

    // Changing monsterId must affect the hash (even if the offset can coincide by chance).
    const uint32_t hOther = noiseInvestigateHash(seed, turn, 2, src, volume, eff, dist);
    CHECK(hOther != h1);

    // Loud sounds should be treated as precise (radius 0).
    CHECK(noiseInvestigateRadius(18, 18, 10) == 0);

    // Nearby sounds should also be precise (radius 0).
    CHECK(noiseInvestigateRadius(6, 6, 2) == 0);

    // Sample a bunch of hashes and verify offsets stay inside the intended
    // Manhattan-diamond uncertainty radius.
    bool sawOuterRing = false;
    for (uint32_t i = 0; i < 256u; ++i) {
        const uint32_t hs = noiseInvestigateHash(seed + i, turn + i, static_cast<int>(10 + i), src, volume, eff, dist);
        const Vec2i oi = noiseInvestigateOffset(hs, r);
        const int md = std::abs(oi.x) + std::abs(oi.y);
        CHECK(md <= r);
        if (md == r) sawOuterRing = true;
    }
    CHECK(sawOuterRing);

    return true;
}


bool test_proc_spell_generation_determinism() {
    // The same id must always produce the same spell.
    const uint32_t id = makeProcSpellId(/*tier=*/7, /*seed28=*/0x00ABCDEFu);
    const ProcSpell a = generateProcSpell(id);
    const ProcSpell b = generateProcSpell(id);
    CHECK(a.id == b.id);
    CHECK(a.tier == b.tier);
    CHECK(a.element == b.element);
    CHECK(a.form == b.form);
    CHECK(a.mods == b.mods);
    CHECK(a.manaCost == b.manaCost);
    CHECK(a.range == b.range);
    CHECK(a.needsTarget == b.needsTarget);
    CHECK(a.aoeRadius == b.aoeRadius);
    CHECK(a.durationTurns == b.durationTurns);
    CHECK(a.damageDiceCount == b.damageDiceCount);
    CHECK(a.damageDiceSides == b.damageDiceSides);
    CHECK(a.damageFlat == b.damageFlat);
    CHECK(a.noise == b.noise);
    CHECK(a.name == b.name);
    CHECK(a.runeSigil == b.runeSigil);
    CHECK(a.description == b.description);
    CHECK(!a.name.empty());
    CHECK(!a.description.empty());
    CHECK(!a.runeSigil.empty());

    // Different ids should usually differ in at least one major property.
    const ProcSpell c = generateProcSpell(makeProcSpellId(/*tier=*/2, /*seed28=*/0x00123456u));
    const ProcSpell d = generateProcSpell(makeProcSpellId(/*tier=*/12, /*seed28=*/0x0000BEEFu));
    CHECK(c.id != d.id);
    CHECK(c.name != d.name || c.element != d.element || c.form != d.form || c.manaCost != d.manaCost);

    // Tier clamping: tier 0 should be treated as tier 1.
    const ProcSpell z = generateProcSpell(makeProcSpellId(/*tier=*/0, /*seed28=*/0x00000001u));
    CHECK(z.tier == 1);

    return true;
}


bool test_proc_monster_codename_determinism() {
    Entity e{};
    e.id = 42;
    e.kind = EntityKind::Orc;
    e.spriteSeed = 123456u;
    e.friendly = false;

    e.procRank = ProcMonsterRank::Elite;
    e.procAffixMask = procAffixBit(ProcMonsterAffix::Venomous) | procAffixBit(ProcMonsterAffix::Swift);
    e.procAbility1 = ProcMonsterAbility::Pounce;
    e.procAbility2 = ProcMonsterAbility::None;

    CHECK(procname::shouldShowCodename(e));
    const std::string n1 = procname::codename(e);
    const std::string n2 = procname::codename(e);

    CHECK(!n1.empty());
    CHECK(n1 == n2);
    CHECK(n1.find(' ') != std::string::npos);

    // Friendly creatures should not surface hostile-style codenames.
    Entity f = e;
    f.friendly = true;
    CHECK(!procname::shouldShowCodename(f));
    CHECK(procname::codename(f).empty());

    // Non-proc variants should not get codenames.
    Entity n = e;
    n.procRank = ProcMonsterRank::Normal;
    n.procAffixMask = 0u;
    n.procAbility1 = ProcMonsterAbility::None;
    n.procAbility2 = ProcMonsterAbility::None;
    CHECK(!procname::shouldShowCodename(n));
    CHECK(procname::codename(n).empty());

    return true;
}



bool test_rune_ward_words() {
    // Parsing should be forgiving (prefix + optional decorations) but deterministic.
    CHECK(wardWordFromText("RUNE FIRE") == WardWord::RuneFire);
    CHECK(wardWordFromText("rune:fire") == WardWord::RuneFire);
    CHECK(wardWordFromText("RUNE OF RADIANCE: KAR-THO-RAI") == WardWord::RuneRadiance);
    CHECK(wardWordFromText("RUNE SHADOW: XX") == WardWord::RuneShadow);
    CHECK(wardWordFromText("RUNE BANANA") == WardWord::None);

    // Sanity-check a few affinities.
    CHECK(wardAffectsMonster(WardWord::RuneRadiance, EntityKind::Zombie));
    CHECK(!wardAffectsMonster(WardWord::RuneRadiance, EntityKind::Goblin));

    CHECK(wardAffectsMonster(WardWord::RuneShock, EntityKind::Goblin));
    CHECK(wardRepelChance(WardWord::RuneShock, EntityKind::Goblin, 10) > 0.0f);
    CHECK(wardRepelChance(WardWord::RuneShock, EntityKind::Goblin, 0) == 0.0f);

    return true;
}

bool test_crafting_procedural_determinism() {
    // Crafting must be deterministic and order-independent.
    // Also verify that using a gear/material ingredient routes to forging output.

    Item a;
    a.kind = ItemKind::Sword;
    a.count = 1;
    a.enchant = 1;
    a.buc = 0;
    a.spriteSeed = 0x0000BEEFu;

    Item b;
    b.kind = ItemKind::ButcheredHide;
    b.count = 1;
    b.enchant = packButcherMaterialEnchant(/*quality=*/9, /*variant=*/2, /*shiny=*/1);
    b.buc = 0;
    b.spriteSeed = 0;

    const uint32_t runSeed = 0x12345678u;

    const craftgen::Outcome o1 = craftgen::craft(runSeed, a, b);
    const craftgen::Outcome o2 = craftgen::craft(runSeed, b, a);

    CHECK(o1.tier == o2.tier);
    CHECK(o1.tagA == o2.tagA);
    CHECK(o1.tagB == o2.tagB);

    CHECK(o1.out.kind == o2.out.kind);
    CHECK(o1.out.spriteSeed == o2.out.spriteSeed);
    CHECK(o1.out.enchant == o2.out.enchant);
    CHECK(o1.out.buc == o2.out.buc);
    CHECK(o1.out.charges == o2.out.charges);
    CHECK(o1.out.ego == o2.out.ego);
    CHECK(o1.out.flags == o2.out.flags);

    // Forging path should produce wearable gear.
    CHECK(isWearableGear(o1.out.kind));

    return true;
}

bool test_crafting_ecosystem_catalyst_changes_outcome() {
    // Crafting should be deterministic, but *location* now matters: ecosystems act as catalysts.
    // This test verifies that the same ingredients produce a different sigil when crafted
    // outside a biome vs inside a biome (while keeping workstation constant).

    Game g;
    g.newGame(0xC0FFEEu);

    // Ecosystems are only generated on non-camp dungeon branches.
    {
        Dungeon& camp = GameTestAccess::dung(g);
        g.playerMut().pos = camp.stairsDown;
        g.handleAction(Action::StairsDown);
    }

    Dungeon& d = GameTestAccess::dung(g);

    // Build ecosystem cache.
    d.ensureMaterials(g.materialWorldSeed(), g.branch(), g.materialDepth(), g.dungeonMaxDepth());

    auto roomTypeAtLocal = [&](Vec2i p) -> RoomType {
        for (const auto& r : d.rooms) {
            if (r.contains(p)) return r.type;
        }
        return RoomType::Normal;
    };

    Vec2i posNone{-1, -1};
    Vec2i posEco{-1, -1};
    EcosystemKind ecoKind = EcosystemKind::None;

    // Pick two walkable positions that share the same room type (workstation),
    // but differ in ecosystem catalyst.
    bool found = false;
    struct Pick {
        bool hasNone = false;
        bool hasEco = false;
        Vec2i none{-1, -1};
        Vec2i eco{-1, -1};
        EcosystemKind ecoKind = EcosystemKind::None;
    };
    std::unordered_map<int, Pick> picks;
    picks.reserve(16);

    for (int y = 0; y < d.height && !found; ++y) {
        for (int x = 0; x < d.width && !found; ++x) {
            if (!d.isWalkable(x, y)) continue;
            const Vec2i p{x, y};
            const RoomType rt = roomTypeAtLocal(p);
            const EcosystemKind e = d.ecosystemAtCached(x, y);

            Pick& pk = picks[static_cast<int>(rt)];
            if (e == EcosystemKind::None && !pk.hasNone) {
                pk.none = p;
                pk.hasNone = true;
            } else if (e != EcosystemKind::None && !pk.hasEco) {
                pk.eco = p;
                pk.ecoKind = e;
                pk.hasEco = true;
            }

            if (pk.hasNone && pk.hasEco) {
                posNone = pk.none;
                posEco = pk.eco;
                ecoKind = pk.ecoKind;
                found = true;
            }
        }
    }

    CHECK(posNone.x >= 0);
    CHECK(posEco.x >= 0);
    CHECK(ecoKind != EcosystemKind::None);

    Item a;
    a.id = 1;
    a.kind = ItemKind::Dagger;
    a.count = 1;
    a.charges = 0;
    a.enchant = 0;
    a.buc = 0;
    a.spriteSeed = 0x11111111u;
    a.shopPrice = 0;
    a.shopDepth = 0;
    a.ego = ItemEgo::None;
    a.flags = 0;

    Item b;
    b.id = 2;
    b.kind = ItemKind::Rock;
    b.count = 1;
    b.charges = 0;
    b.enchant = 0;
    b.buc = 0;
    b.spriteSeed = 0x22222222u;
    b.shopPrice = 0;
    b.shopDepth = 0;
    b.ego = ItemEgo::None;
    b.flags = 0;

    g.playerMut().pos = posNone;
    const GameTestAccess::CraftProbe ccNone = GameTestAccess::probeCraft(g, a, b);
    CHECK(ccNone.eco == EcosystemKind::None);

    g.playerMut().pos = posEco;
    const GameTestAccess::CraftProbe ccEco = GameTestAccess::probeCraft(g, a, b);
    CHECK(ccEco.eco == ecoKind);

    // Catalyst salting should change the sigil/output seed.
    CHECK(ccNone.sig != ccEco.sig);

    return true;
}

bool test_crafting_shard_refinement_location_invariant() {
    // Combining two matching Essence Shards should deterministically refine them into
    // a higher-tier shard, and the result should not depend on biome/workstation.

    Game g;
    g.newGame(0xC0FFEEu);

    // Ecosystems are only generated on non-camp dungeon branches.
    {
        Dungeon& camp = GameTestAccess::dung(g);
        g.playerMut().pos = camp.stairsDown;
        g.handleAction(Action::StairsDown);
    }

    Dungeon& d = GameTestAccess::dung(g);

    // Build ecosystem cache.
    d.ensureMaterials(g.materialWorldSeed(), g.branch(), g.materialDepth(), g.dungeonMaxDepth());

    auto roomTypeAtLocal = [&](Vec2i p) -> RoomType {
        for (const auto& r : d.rooms) {
            if (r.contains(p)) return r.type;
        }
        return RoomType::Normal;
    };

    Vec2i posNone{-1, -1};
    Vec2i posEco{-1, -1};
    EcosystemKind ecoKind = EcosystemKind::None;

    // Pick two walkable positions that share the same room type (workstation),
    // but differ in ecosystem catalyst.
    bool found = false;
    struct Pick {
        bool hasNone = false;
        bool hasEco = false;
        Vec2i none{-1, -1};
        Vec2i eco{-1, -1};
        EcosystemKind ecoKind = EcosystemKind::None;
    };
    std::unordered_map<int, Pick> picks;
    picks.reserve(16);

    for (int y = 0; y < d.height && !found; ++y) {
        for (int x = 0; x < d.width && !found; ++x) {
            if (!d.isWalkable(x, y)) continue;
            const Vec2i p{x, y};
            const RoomType rt = roomTypeAtLocal(p);
            const EcosystemKind e = d.ecosystemAtCached(x, y);

            Pick& pk = picks[static_cast<int>(rt)];
            if (e == EcosystemKind::None && !pk.hasNone) {
                pk.none = p;
                pk.hasNone = true;
            } else if (e != EcosystemKind::None && !pk.hasEco) {
                pk.eco = p;
                pk.ecoKind = e;
                pk.hasEco = true;
            }

            if (pk.hasNone && pk.hasEco) {
                posNone = pk.none;
                posEco = pk.eco;
                ecoKind = pk.ecoKind;
                found = true;
            }
        }
    }

    CHECK(posNone.x >= 0);
    CHECK(posEco.x >= 0);
    CHECK(ecoKind != EcosystemKind::None);

    const int tagId = crafttags::tagIndex(crafttags::Tag::Ember);
    const int inTier = 4;

    Item s1;
    s1.id = 1;
    s1.kind = ItemKind::EssenceShard;
    s1.count = 1;
    s1.charges = 0;
    s1.enchant = packEssenceShardEnchant(tagId, inTier, false);
    s1.buc = 0;
    s1.spriteSeed = 0x11111111u;
    s1.shopPrice = 0;
    s1.shopDepth = 0;
    s1.ego = ItemEgo::None;
    s1.flags = 0;

    Item s2 = s1;
    s2.id = 2;
    s2.spriteSeed = 0x22222222u;

    g.playerMut().pos = posNone;
    const Game::CraftComputed ccNone = g.computeCraftComputed(s1, s2);
    CHECK(ccNone.ecosystem == EcosystemKind::None);

    g.playerMut().pos = posEco;
    const Game::CraftComputed ccEco = g.computeCraftComputed(s1, s2);
    CHECK(ccEco.ecosystem == ecoKind);

    CHECK(ccNone.out.kind == ItemKind::EssenceShard);
    CHECK(ccEco.out.kind == ItemKind::EssenceShard);

    // Location should not change refinement output.
    CHECK(ccNone.out.enchant == ccEco.out.enchant);
    CHECK(ccNone.out.spriteSeed == ccEco.out.spriteSeed);

    CHECK(essenceShardTagFromEnchant(ccNone.out.enchant) == tagId);
    CHECK(essenceShardTierFromEnchant(ccNone.out.enchant) == inTier + 1);

    // Still order-independent.
    const Game::CraftComputed ccRev = g.computeCraftComputed(s2, s1);
    CHECK(ccNone.out.enchant == ccRev.out.enchant);
    CHECK(ccNone.out.spriteSeed == ccRev.out.spriteSeed);

    return true;
}


bool test_crafting_shard_infusion_upgrades_gear() {
    // Combining an Essence Shard with wearable gear should deterministically
    // upgrade that gear (preserving the base item kind).

    const uint32_t runSeed = 0x1234ABCDu;

    const int tagId = crafttags::tagIndex(crafttags::Tag::Ember);

    Item shard;
    shard.kind = ItemKind::EssenceShard;
    shard.count = 1;
    shard.charges = 0;
    shard.enchant = packEssenceShardEnchant(tagId, /*tier=*/10, /*shiny=*/false);
    shard.buc = 0;
    shard.spriteSeed = 0x11111111u;
    shard.shopPrice = 0;
    shard.shopDepth = 0;
    shard.ego = ItemEgo::None;
    shard.flags = 0;

    Item sword;
    sword.kind = ItemKind::Sword;
    sword.count = 1;
    sword.charges = 0;
    sword.enchant = 0;
    sword.buc = 0;
    sword.spriteSeed = 0x22222222u;
    sword.shopPrice = 0;
    sword.shopDepth = 0;
    sword.ego = ItemEgo::None;
    sword.flags = 0;

    const craftgen::Outcome o1 = craftgen::craft(runSeed, shard, sword);
    const craftgen::Outcome o2 = craftgen::craft(runSeed, sword, shard);

    CHECK(o1.tier == o2.tier);
    CHECK(o1.tagA == o2.tagA);
    CHECK(o1.tagB == o2.tagB);

    CHECK(o1.out.kind == ItemKind::Sword);
    CHECK(o2.out.kind == ItemKind::Sword);

    CHECK(o1.out.spriteSeed == o2.out.spriteSeed);
    CHECK(o1.out.enchant == o2.out.enchant);
    CHECK(o1.out.buc == o2.out.buc);
    CHECK(o1.out.charges == o2.out.charges);
    CHECK(o1.out.ego == o2.out.ego);
    CHECK(o1.out.flags == o2.out.flags);

    // Tier 10 shard should provide a noticeable enchant bump.
    CHECK(o1.out.enchant >= 2);

    CHECK(!o1.hasByproduct);

    return true;
}

bool test_crafting_same_stack_consumes_two_units() {
    Game g;
    g.newGame(0xBADA55u);

    // Build a controlled inventory.
    g.inv.clear();
    g.shopDebtLedger_.fill(0);
    g.nextItemId = 1;
    g.equipMeleeId = 0;
    g.equipRangedId = 0;
    g.equipArmorId = 0;
    g.equipRing1Id = 0;
    g.equipRing2Id = 0;

    Item kit;
    kit.id = g.nextItemId++;
    kit.kind = ItemKind::CraftingKit;
    kit.count = 1;
    kit.charges = 0;
    kit.enchant = 0;
    kit.buc = 0;
    kit.spriteSeed = 0;
    kit.shopPrice = 0;
    kit.shopDepth = 0;
    kit.ego = ItemEgo::None;
    kit.flags = 0;

    Item rocks;
    rocks.id = g.nextItemId++;
    rocks.kind = ItemKind::Rock;
    rocks.count = 3;
    rocks.charges = 0;
    rocks.enchant = 0;
    rocks.buc = 0;
    rocks.spriteSeed = 0;
    rocks.shopPrice = 0;
    rocks.shopDepth = 0;
    rocks.ego = ItemEgo::None;
    rocks.flags = 0;

    const int rockId = rocks.id;

    g.inv.push_back(kit);
    g.inv.push_back(rocks);

    CHECK(g.craftCombineById(rockId, rockId));

    int rockCount = 0;
    for (const auto& it : g.inv) {
        if (it.kind == ItemKind::Rock) rockCount += it.count;
    }
    CHECK(rockCount == 1);

    bool hasKit = false;
    for (const auto& it : g.inv) {
        if (it.kind == ItemKind::CraftingKit) hasKit = true;
    }
    CHECK(hasKit);

    return true;
}

bool test_crafting_blocks_cursed_equipped_ingredients() {
    Game g;
    g.newGame(0x1337u);

    g.inv.clear();
    g.shopDebtLedger_.fill(0);
    g.nextItemId = 1;
    g.equipMeleeId = 0;
    g.equipRangedId = 0;
    g.equipArmorId = 0;
    g.equipRing1Id = 0;
    g.equipRing2Id = 0;

    Item kit;
    kit.id = g.nextItemId++;
    kit.kind = ItemKind::CraftingKit;
    kit.count = 1;
    kit.charges = 0;
    kit.enchant = 0;
    kit.buc = 0;
    kit.spriteSeed = 0;
    kit.shopPrice = 0;
    kit.shopDepth = 0;
    kit.ego = ItemEgo::None;
    kit.flags = 0;

    Item sword;
    sword.id = g.nextItemId++;
    sword.kind = ItemKind::Sword;
    sword.count = 1;
    sword.charges = 0;
    sword.enchant = 0;
    sword.buc = -1; // cursed
    sword.spriteSeed = 0;
    sword.shopPrice = 0;
    sword.shopDepth = 0;
    sword.ego = ItemEgo::None;
    sword.flags = 0;

    Item rock;
    rock.id = g.nextItemId++;
    rock.kind = ItemKind::Rock;
    rock.count = 1;
    rock.charges = 0;
    rock.enchant = 0;
    rock.buc = 0;
    rock.spriteSeed = 0;
    rock.shopPrice = 0;
    rock.shopDepth = 0;
    rock.ego = ItemEgo::None;
    rock.flags = 0;

    g.inv.push_back(kit);
    g.inv.push_back(sword);
    g.inv.push_back(rock);

    g.equipMeleeId = sword.id;

    const size_t invSizeBefore = g.inv.size();
    CHECK(!g.craftCombineById(sword.id, rock.id));
    CHECK(g.equipMeleeId == sword.id);
    CHECK(g.inv.size() == invSizeBefore);

    return true;
}

bool test_crafting_auto_unequips_consumed_gear() {
    Game g;
    g.newGame(0xFEEDBEEFu);

    g.inv.clear();
    g.shopDebtLedger_.fill(0);
    g.nextItemId = 1;
    g.equipMeleeId = 0;
    g.equipRangedId = 0;
    g.equipArmorId = 0;
    g.equipRing1Id = 0;
    g.equipRing2Id = 0;

    Item kit;
    kit.id = g.nextItemId++;
    kit.kind = ItemKind::CraftingKit;
    kit.count = 1;
    kit.charges = 0;
    kit.enchant = 0;
    kit.buc = 0;
    kit.spriteSeed = 0;
    kit.shopPrice = 0;
    kit.shopDepth = 0;
    kit.ego = ItemEgo::None;
    kit.flags = 0;

    Item sword;
    sword.id = g.nextItemId++;
    sword.kind = ItemKind::Sword;
    sword.count = 1;
    sword.charges = 0;
    sword.enchant = 0;
    sword.buc = 0; // not cursed
    sword.spriteSeed = 0;
    sword.shopPrice = 0;
    sword.shopDepth = 0;
    sword.ego = ItemEgo::None;
    sword.flags = 0;

    Item rock;
    rock.id = g.nextItemId++;
    rock.kind = ItemKind::Rock;
    rock.count = 1;
    rock.charges = 0;
    rock.enchant = 0;
    rock.buc = 0;
    rock.spriteSeed = 0;
    rock.shopPrice = 0;
    rock.shopDepth = 0;
    rock.ego = ItemEgo::None;
    rock.flags = 0;

    g.inv.push_back(kit);
    g.inv.push_back(sword);
    g.inv.push_back(rock);

    g.equipMeleeId = sword.id;

    CHECK(g.craftCombineById(sword.id, rock.id));
    CHECK(g.equipMeleeId == 0);

    bool hasOldSword = false;
    for (const auto& it : g.inv) {
        if (it.id == sword.id) hasOldSword = true;
    }
    CHECK(!hasOldSword);

    return true;
}

bool test_graffiti_procgen_determinism() {
    // The procedural graffiti generator is intentionally lightweight and should remain
    // deterministic across platforms. This test uses a tiny hand-constructed dungeon.

    Dungeon d(20, 20);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
        }
    }
    d.stairsUp = {1, 1};
    d.stairsDown = {18, 18};

    // Add a few features to hint at.
    d.at(5, 5).type = TileType::DoorSecret;
    d.at(10, 10).type = TileType::DoorLocked;
    d.at(8, 15).type = TileType::Chasm;
    d.at(8, 14).type = TileType::Boulder; // adjacent to chasm
    d.at(3, 16).type = TileType::Fountain;

    Room vault; vault.x = 2; vault.y = 2; vault.w = 6; vault.h = 6; vault.type = RoomType::Vault;
    Room shrine; shrine.x = 12; shrine.y = 2; shrine.w = 6; shrine.h = 6; shrine.type = RoomType::Shrine;
    Room shop; shop.x = 12; shop.y = 12; shop.w = 6; shop.h = 6; shop.type = RoomType::Shop;
    d.rooms.push_back(vault);
    d.rooms.push_back(shrine);
    d.rooms.push_back(shop);

    const std::vector<graffitigen::Hint> hints = graffitigen::collectHints(d);
    CHECK(!hints.empty());

    auto hasHintKind = [&](graffitigen::HintKind k) {
        for (const auto& h : hints) {
            if (h.kind == k) return true;
        }
        return false;
    };

    CHECK(hasHintKind(graffitigen::HintKind::Fountain));
    CHECK(hasHintKind(graffitigen::HintKind::StairsDown));

    // Same inputs -> identical outputs.
    const std::string a = graffitigen::generateLine(0x12345678u, d, 5, RoomType::Vault, {3, 3}, hints);
    const std::string b = graffitigen::generateLine(0x12345678u, d, 5, RoomType::Vault, {3, 3}, hints);
    CHECK(a == b);
    CHECK(!a.empty());
    CHECK(a.size() <= 72);

    // Hint lines should be short and directional.
    const graffitigen::Hint h{graffitigen::HintKind::SecretDoor, {5, 5}};
    const std::string hl = graffitigen::makeHintLine(0x11111111u, h, {3, 3});
    CHECK(!hl.empty());
    CHECK(hl.size() <= 72);
    CHECK(containsCaseInsensitive(hl, "WALL") || containsCaseInsensitive(hl, "LOOK") || containsCaseInsensitive(hl, "SECRET"));

    const std::string wf = graffitigen::makeHintLine(0x22222222u, {graffitigen::HintKind::Fountain, {3, 16}}, {3, 3});
    CHECK(!wf.empty());
    CHECK(wf.size() <= 72);
    const bool wfOk =
        containsCaseInsensitive(wf, "WATER") ||
        containsCaseInsensitive(wf, "SPRING") ||
        containsCaseInsensitive(wf, "FOUNTAIN") ||
        containsCaseInsensitive(wf, "DRIP") ||
        containsCaseInsensitive(wf, "SPLASH") ||
        containsCaseInsensitive(wf, "DRINK");
    CHECK(wfOk);

    const std::string sd = graffitigen::makeHintLine(0x33333333u, {graffitigen::HintKind::StairsDown, {18, 18}}, {3, 3});
    CHECK(!sd.empty());
    CHECK(sd.size() <= 72);
    CHECK(containsCaseInsensitive(sd, "DOWN") || containsCaseInsensitive(sd, "STAIRS") || containsCaseInsensitive(sd, "DESCEND"));

    const std::string gx = graffitigen::makeGlyphOmenLine(0xA5A5A5A5u, RoomType::Secret, 11);
    const std::string gy = graffitigen::makeGlyphOmenLine(0xA5A5A5A5u, RoomType::Secret, 11);
    CHECK(gx == gy);
    CHECK(gx.size() <= 72);
    CHECK(gx.find('-') != std::string::npos);
    CHECK(gx.find("11") != std::string::npos);

    return true;
}

struct TestCase {
    const char* name;
    bool (*fn)();
};

} // namespace



bool test_trap_salvage_procgen_determinism() {
    // Trap salvage should be deterministic and produce reasonable outputs.
    // This test intentionally avoids using the main RNG stream.

    const uint32_t runSeed = 0xDEADBEEFu;
    const int depth = 9;

    // Spot-check a few stable tag mappings.
    CHECK(trapsalvage::tagForTrap(TrapKind::PoisonDart) == crafttags::Tag::Venom);
    CHECK(trapsalvage::tagForTrap(TrapKind::Teleport) == crafttags::Tag::Rune);
    CHECK(trapsalvage::tagForTrap(TrapKind::CorrosiveGas) == crafttags::Tag::Alch);

    const TrapKind kinds[] = {
        TrapKind::Spike,
        TrapKind::PoisonDart,
        TrapKind::Teleport,
        TrapKind::Alarm,
        TrapKind::Web,
        TrapKind::ConfusionGas,
        TrapKind::RollingBoulder,
        TrapKind::TrapDoor,
        TrapKind::LetheMist,
        TrapKind::PoisonGas,
        TrapKind::CorrosiveGas,
    };

    for (TrapKind k : kinds) {
        bool found = false;
        trapsalvage::SalvageSpec spec;
        Vec2i pos{0, 0};

        // Find any position that yields salvage (chance is ~30-50% so this is fast).
        for (int y = 0; y < 32 && !found; ++y) {
            for (int x = 0; x < 32 && !found; ++x) {
                pos = {x, y};
                const uint32_t s = trapsalvage::seedForFloorTrap(runSeed, depth, pos, k);
                spec = trapsalvage::rollSalvage(s, k, depth, /*chest=*/false);
                if (spec.count > 0) {
                    found = true;

                    // Determinism: same inputs -> identical outputs.
                    const auto spec2 = trapsalvage::rollSalvage(s, k, depth, /*chest=*/false);
                    CHECK(spec.tag == spec2.tag);
                    CHECK(spec.tier == spec2.tier);
                    CHECK(spec.count == spec2.count);
                    CHECK(spec.shiny == spec2.shiny);
                    CHECK(spec.spriteSeed == spec2.spriteSeed);

                    // Bounds.
                    CHECK(spec.count >= 1 && spec.count <= 2);
                    CHECK(spec.tier >= 1 && spec.tier <= 12);
                    CHECK(spec.spriteSeed != 0u);

                    // Tag must match the trap mapping.
                    CHECK(spec.tag == trapsalvage::tagForTrap(k));
                }
            }
        }

        CHECK(found);
    }

    // Chest seed path should also be deterministic.
    {
        const TrapKind k = TrapKind::Teleport;
        const uint32_t bs = trapsalvage::seedForChestTrap(runSeed, depth, /*chestSeed=*/0xABCDEF01u, k, /*chestTier=*/2);
        const auto a = trapsalvage::rollSalvage(bs, k, depth + 4, /*chest=*/true);
        const auto b = trapsalvage::rollSalvage(bs, k, depth + 4, /*chest=*/true);
        CHECK(a.tag == b.tag);
        CHECK(a.tier == b.tier);
        CHECK(a.count == b.count);
        CHECK(a.shiny == b.shiny);
        CHECK(a.spriteSeed == b.spriteSeed);
        if (a.count > 0) {
            CHECK(a.tier >= 1 && a.tier <= 12);
            CHECK(a.count >= 1 && a.count <= 2);
        }
    }

    return true;
}

bool test_sigil_procgen_determinism() {
    // Sigils should be deterministic per (runSeed, depth, pos, keyword).
    const uint32_t runSeed = 0x1234ABCDu;
    const int depth = 7;
    const Vec2i p1{10, 20};
    const Vec2i p2{11, 20};

    const auto a = sigilgen::makeSigil(runSeed, depth, p1, "EMBER");
    const auto b = sigilgen::makeSigil(runSeed, depth, p1, "EMBER");
    CHECK(a.kind == sigilgen::SigilKind::Ember);
    CHECK(a.kind == b.kind);
    CHECK(a.seed == b.seed);
    CHECK(a.uses == b.uses);
    CHECK(a.radius == b.radius);
    CHECK(a.intensity == b.intensity);
    CHECK(a.durationTurns == b.durationTurns);
    CHECK(a.param == b.param);
    CHECK(!a.epithet.empty());

    const auto c = sigilgen::makeSigil(runSeed, depth, p2, "EMBER");
    CHECK(c.kind == sigilgen::SigilKind::Ember);
    CHECK(c.seed != a.seed);

    const auto d = sigilgen::makeSigil(runSeed, depth, p1, "MIASMA");
    CHECK(d.kind == sigilgen::SigilKind::Miasma);
    CHECK(d.seed != a.seed);

    // Spawn picker should be deterministic and always return a valid kind.
    const auto spA = sigilgen::makeSigilForSpawn(runSeed, depth, p1, RoomType::Laboratory);
    const auto spB = sigilgen::makeSigilForSpawn(runSeed, depth, p1, RoomType::Laboratory);
    CHECK(spA.kind == spB.kind);
    CHECK(spA.seed == spB.seed);
    CHECK(spA.uses == spB.uses);
    CHECK(!sigilgen::keywordPlusEpithet(spA).empty());
    return true;
}

bool test_ident_appearance_procgen_determinism() {
    // Identification appearance labels should be deterministic per (runSeed, appearanceId)
    // and safe for HUD quoting.

    const uint32_t runSeed = 0xCAFEBABEu;

    // Potions keep their base gem/color word.
    {
        const std::string a = identgen::potionLabel(runSeed, 2, "SAPPHIRE");
        const std::string b = identgen::potionLabel(runSeed, 2, "SAPPHIRE");
        CHECK(a == b);
        CHECK(!a.empty());
        CHECK(a.size() <= 22);
        CHECK(a.find("SAPPHIRE") != std::string::npos);
        for (char c : a) {
            CHECK(c == ' ' || (c >= 'A' && c <= 'Z'));
        }
    }

    // Rings keep their base material.
    {
        const std::string a = identgen::ringLabel(runSeed, 9, "ONYX");
        const std::string b = identgen::ringLabel(runSeed, 9, "ONYX");
        CHECK(a == b);
        CHECK(!a.empty());
        CHECK(a.find("ONYX") != std::string::npos);
    }

    // Wands keep their base material.
    {
        const std::string a = identgen::wandLabel(runSeed, 1, "OAK");
        const std::string b = identgen::wandLabel(runSeed, 1, "OAK");
        CHECK(a == b);
        CHECK(!a.empty());
        CHECK(a.find("OAK") != std::string::npos);
    }

    // Scrolls: a NetHack-style multi-word incantation, anchored by base word.
    {
        const char* BANK[] = {"ZELGO", "MER", "XANATH", "KLAATU", "BARADA", "NIKTO"};
        const std::string a = identgen::scrollLabel(runSeed, 3, BANK);
        const std::string b = identgen::scrollLabel(runSeed, 3, BANK);
        CHECK(a == b);
        CHECK(!a.empty());
        CHECK(a.size() <= 26);
        // appearanceId=3 anchors to BANK[3] ("KLAATU").
        CHECK(a.rfind("KLAATU", 0) == 0);
        CHECK(a.find('\'') == std::string::npos);
        CHECK(a.find('"') == std::string::npos);
        for (char c : a) {
            CHECK(c == ' ' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
        }
    }

    return true;
}


bool test_proc_shop_profiles()
{
    // Determinism: same inputs produce the same profile.
    Room r;
    r.type = RoomType::Shop;
    r.x = 7; r.y = 5; r.w = 12; r.h = 8;
    const uint32_t runSeed = 0x12345678u;
    const int depth = 4;
    const shopgen::ShopProfile a = shopgen::profileFor(runSeed, depth, r);
    const shopgen::ShopProfile b = shopgen::profileFor(runSeed, depth, r);
    if (a.seed != b.seed || a.theme != b.theme || a.temperament != b.temperament) return false;
    if (a.buyMarkupPct != b.buyMarkupPct || a.sellRatePct != b.sellRatePct) return false;

    const std::string name = shopgen::shopNameFor(a);
    const std::string keeper = shopgen::shopkeeperNameFor(a);
    if (name.empty() || keeper.empty()) return false;
    if (name.size() > 32 || keeper.size() > 16) return false;
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || c == ' ' || c == '\'' || c == '&';
        if (!ok) return false;
    }
    for (const char c : keeper) {
        const bool ok = (c >= 'A' && c <= 'Z') || c == '\'' || c == '-';
        if (!ok) return false;
    }

    // Economy bias sanity: armory should treat weapons/armor better than potions.
    shopgen::ShopProfile armory = a;
    armory.theme = shopgen::ShopTheme::Armory;
    armory.temperament = shopgen::ShopTemperament::Fair;
    armory.buyMarkupPct = 100;
    armory.sellRatePct = 100;

    Item sword; sword.kind = ItemKind::Sword; sword.spriteSeed = 1;
    Item potion; potion.kind = ItemKind::PotionHealing; potion.spriteSeed = 2;
    const int buySword = shopgen::adjustedShopBuyPricePerUnit(100, armory, sword);
    const int buyPotion = shopgen::adjustedShopBuyPricePerUnit(100, armory, potion);
    if (!(buySword < buyPotion)) return false;
    const int sellSword = shopgen::adjustedShopSellPricePerUnit(50, armory, sword);
    const int sellPotion = shopgen::adjustedShopSellPricePerUnit(50, armory, potion);
    if (!(sellSword > sellPotion)) return false;

    return true;
}


static void makeAllVisibleFloor(Dungeon& d) {
    for (auto& t : d.tiles) {
        t.type = TileType::Floor;
        t.visible = true;
        t.explored = true;
    }
}


bool test_shopkeeper_look_shows_deterministic_name() {
    Game g;
    g.newGame(0x0BEEF123u);

    g.branch_ = DungeonBranch::Main;
    g.depth_ = 4;

    g.dung = Dungeon(10, 6);
    g.dung.rooms.clear();
    makeAllVisibleFloor(g.dung);

    Room shop;
    shop.type = RoomType::Shop;
    shop.x = 1;
    shop.y = 1;
    shop.w = 8;
    shop.h = 4;
    g.dung.rooms.push_back(shop);

    const shopgen::ShopProfile prof = shopgen::profileFor(g.seed(), g.depth(), shop);
    const std::string expected = std::string("SHOPKEEPER ") + shopgen::shopkeeperNameFor(prof);

    g.ents.clear();
    g.ground.clear();

    Entity p;
    p.id = 1;
    p.kind = EntityKind::Player;
    p.hpMax = 20;
    p.hp = 20;
    p.pos = {1, 3};
    g.playerId_ = p.id;
    g.ents.push_back(p);

    Entity sk;
    sk.id = 2;
    sk.kind = EntityKind::Shopkeeper;
    sk.hpMax = 12;
    sk.hp = 12;
    sk.pos = {5, 3};
    sk.spriteSeed = hashCombine(prof.seed, "SK"_tag);
    sk.alerted = false;
    g.ents.push_back(sk);

    const std::string desc = g.describeAt(sk.pos);
    CHECK(desc.find(expected) != std::string::npos);
    return true;
}


bool test_targeting_warning_includes_shopkeeper_name() {
    Game g;
    g.newGame(0x0BEEF124u);

    g.branch_ = DungeonBranch::Main;
    g.depth_ = 4;

    g.dung = Dungeon(12, 6);
    g.dung.rooms.clear();
    makeAllVisibleFloor(g.dung);

    Room shop;
    shop.type = RoomType::Shop;
    shop.x = 1;
    shop.y = 1;
    shop.w = 10;
    shop.h = 4;
    g.dung.rooms.push_back(shop);

    const shopgen::ShopProfile prof = shopgen::profileFor(g.seed(), g.depth(), shop);
    const std::string expectedLabel = std::string("SHOPKEEPER ") + shopgen::shopkeeperNameFor(prof);

    g.ents.clear();
    g.ground.clear();

    Entity p;
    p.id = 1;
    p.kind = EntityKind::Player;
    p.hpMax = 20;
    p.hp = 20;
    p.pos = {1, 3};
    g.playerId_ = p.id;
    g.ents.push_back(p);

    Entity sk;
    sk.id = 2;
    sk.kind = EntityKind::Shopkeeper;
    sk.hpMax = 12;
    sk.hp = 12;
    sk.pos = {4, 3};
    sk.spriteSeed = hashCombine(prof.seed, "SK"_tag);
    sk.alerted = false;
    g.ents.push_back(sk);

    // Ensure the player has a valid ranged option (throwing rocks).
    g.inv.clear();
    Item rocks;
    rocks.id = 1;
    rocks.kind = ItemKind::Rock;
    rocks.count = 1;
    g.inv.push_back(rocks);

    g.targeting = true;
    g.targetingMode_ = Game::TargetingMode::Ranged;
    // Keep the cursor in hand-throw range while still tracing through the shopkeeper tile.
    g.targetPos = {5, 3};
    g.recomputeTargetLine();

    CHECK(g.targetValid);
    CHECK(g.targetingWarningText().find(expectedLabel) != std::string::npos);
    return true;
}

bool test_pay_at_shop_requires_local_shopkeeper() {
    Game g;
    g.newGame(0x0BEEF125u);

    g.branch_ = DungeonBranch::Main;
    g.depth_ = 4;

    g.dung = Dungeon(24, 8);
    g.dung.rooms.clear();
    makeAllVisibleFloor(g.dung);

    Room shopA;
    shopA.type = RoomType::Shop;
    shopA.x = 1;
    shopA.y = 1;
    shopA.w = 8;
    shopA.h = 6;
    g.dung.rooms.push_back(shopA);

    Room shopB;
    shopB.type = RoomType::Shop;
    shopB.x = 14;
    shopB.y = 1;
    shopB.w = 8;
    shopB.h = 6;
    g.dung.rooms.push_back(shopB);

    g.ents.clear();
    g.ground.clear();

    Entity p;
    p.id = 1;
    p.kind = EntityKind::Player;
    p.hpMax = 20;
    p.hp = 20;
    p.pos = {3, 3};
    g.playerId_ = p.id;
    g.ents.push_back(p);

    Entity sk;
    sk.id = 2;
    sk.kind = EntityKind::Shopkeeper;
    sk.hpMax = 12;
    sk.hp = 12;
    sk.pos = {17, 3}; // In shopB, not in the player's current shop.
    sk.alerted = false;
    g.ents.push_back(sk);

    g.inv.clear();
    g.nextItemId = 1;

    Item gold;
    gold.id = g.nextItemId++;
    gold.kind = ItemKind::Gold;
    gold.count = 400;
    g.inv.push_back(gold);

    Item unpaid;
    unpaid.id = g.nextItemId++;
    unpaid.kind = ItemKind::PotionHealing;
    unpaid.count = 1;
    unpaid.shopPrice = 120;
    unpaid.shopDepth = g.depth_;
    g.inv.push_back(unpaid);

    CHECK(g.playerInShop());
    CHECK(g.shopDebtThisDepth() == 120);
    CHECK(!g.payAtShop());
    CHECK(g.shopDebtThisDepth() == 120);
    return true;
}

bool test_shopkeeper_death_clears_depth_debt_ledger() {
    Game g;
    g.newGame(0x0BEEF126u);

    g.branch_ = DungeonBranch::Main;
    g.depth_ = 4;

    g.dung = Dungeon(12, 8);
    g.dung.rooms.clear();
    makeAllVisibleFloor(g.dung);

    Room shop;
    shop.type = RoomType::Shop;
    shop.x = 1;
    shop.y = 1;
    shop.w = 10;
    shop.h = 6;
    g.dung.rooms.push_back(shop);

    g.ents.clear();
    g.ground.clear();

    Entity p;
    p.id = 1;
    p.kind = EntityKind::Player;
    p.hpMax = 20;
    p.hp = 20;
    p.pos = {3, 3};
    g.playerId_ = p.id;
    g.ents.push_back(p);

    Entity deadKeeper;
    deadKeeper.id = 2;
    deadKeeper.kind = EntityKind::Shopkeeper;
    deadKeeper.hpMax = 12;
    deadKeeper.hp = 0;
    deadKeeper.pos = {5, 3};
    deadKeeper.alerted = true;
    g.ents.push_back(deadKeeper);

    g.shopDebtLedger_.fill(0);
    g.shopDebtLedger_[4] = 77;
    g.shopDebtLedger_[5] = 33;
    CHECK(g.shopDebtTotal() == 110);

    g.cleanupDead();

    CHECK(g.shopDebtLedger_[4] == 0);
    CHECK(g.shopDebtLedger_[5] == 33);
    CHECK(g.shopDebtTotal() == 33);
    return true;
}


bool test_proc_shrine_profiles()
{
    // Determinism + HUD-safe naming.
    Room r;
    r.type = RoomType::Shrine;
    r.x = 11; r.y = 9; r.w = 10; r.h = 7;

    const uint32_t runSeed = 0x0BADF00Du;
    const int depth = 5;

    const shrinegen::ShrineProfile a = shrinegen::profileFor(runSeed, depth, r);
    const shrinegen::ShrineProfile b = shrinegen::profileFor(runSeed, depth, r);
    if (a.seed != b.seed || a.domain != b.domain) return false;

    const std::string deity = shrinegen::deityNameFor(a);
    const std::string full = shrinegen::deityFullTitleFor(a);
    const std::string hud  = shrinegen::hudLabelFor(a);
    if (deity.empty() || full.empty() || hud.empty()) return false;
    if (deity.size() > 12) return false;

    for (char c : full) {
        const bool ok = (c >= 'A' && c <= 'Z') || c == ' ' || c == '-';
        if (!ok) return false;
    }

    const int favPct = shrinegen::serviceCostPct(a.domain, shrinegen::favoredService(a.domain));
    const int donatePct = shrinegen::serviceCostPct(a.domain, shrinegen::ShrineService::Donate);
    if (!(favPct < donatePct)) return false;
    if (!(favPct >= 50 && favPct <= 100)) return false;
    if (!(donatePct >= 90 && donatePct <= 130)) return false;

    const std::string dom = shrinegen::domainName(a.domain);
    if (dom.empty()) return false;
    for (char c : dom) {
        const bool ok = (c >= 'A' && c <= 'Z') || c == '_';
        if (!ok) return false;
    }

    return true;
}



bool test_victory_plan_procgen_determinism()
{
    const int dungeonMaxDepth = Game::DUNGEON_MAX_DEPTH;

    // Determinism: same inputs -> identical plan + text.
    {
        const uint32_t runSeed = 0xCAFEBABEu;
        const bool infiniteWorld = false;
        const victorygen::VictoryPlan a = victorygen::planFor(runSeed, dungeonMaxDepth, infiniteWorld);
        const victorygen::VictoryPlan b = victorygen::planFor(runSeed, dungeonMaxDepth, infiniteWorld);

        if (!(a.seed == b.seed && a.targetDepth == b.targetDepth && a.reqCount == b.reqCount)) return false;

        for (int i = 0; i < static_cast<int>(a.reqCount); ++i) {
            const auto& ra = a.req[static_cast<size_t>(i)];
            const auto& rb = b.req[static_cast<size_t>(i)];
            if (ra.kind != rb.kind) return false;
            if (ra.amount != rb.amount) return false;
            if (ra.tag != rb.tag) return false;
            if (ra.minTier != rb.minTier) return false;
        }

        const auto ga = victorygen::goalLines(a);
        const auto gb = victorygen::goalLines(b);
        if (ga != gb) return false;
        if (ga.empty()) return false;
    }

    // Invariant checks across a small sample of seeds.
    for (uint32_t s = 1; s <= 200; ++s) {
        const uint32_t runSeed = hash32(s * 0x9E3779B9u);
        const bool infiniteWorld = (s % 2) == 0;
        const victorygen::VictoryPlan p = victorygen::planFor(runSeed, dungeonMaxDepth, infiniteWorld);

        if (p.targetDepth < 6) return false;
        if (p.reqCount < 1 || p.reqCount > 2) return false;
        if (victorygen::requiresAmulet(p) && p.targetDepth != dungeonMaxDepth) return false;

        bool hasHardConduct = false;
        for (int i = 0; i < static_cast<int>(p.reqCount); ++i) {
            const auto& r = p.req[static_cast<size_t>(i)];

            if (r.kind == victorygen::KeyKind::Pacifist || r.kind == victorygen::KeyKind::Foodless) hasHardConduct = true;

            if (r.kind == victorygen::KeyKind::Gold) {
                if (r.amount <= 0) return false;
            }

            if (r.kind == victorygen::KeyKind::EssenceShards) {
                if (r.amount <= 0) return false;
                if (r.tag == crafttags::Tag::None) return false;
                if (r.minTier < 0 || r.minTier > 15) return false;
            }

            if (r.kind == victorygen::KeyKind::HideTrophies || r.kind == victorygen::KeyKind::BoneTrophies) {
                if (r.amount <= 0) return false;
                if (r.minTier < 0 || r.minTier > 3) return false;
            }

            if (r.kind == victorygen::KeyKind::FishTrophies) {
                if (r.amount <= 0) return false;
                if (r.minTier < 0 || r.minTier > 4) return false;

                if (r.tag != crafttags::Tag::None) {
                    const std::string tok = crafttags::tagToken(r.tag);
                    // Fish trophies only use a small subset of core tags.
                    const bool ok = (tok == "REGEN" || tok == "HASTE" || tok == "SHIELD"
                                 || tok == "AURORA" || tok == "CLARITY" || tok == "VENOM" || tok == "EMBER");
                    if (!ok) return false;
                }
            }
        }

        if (hasHardConduct && p.targetDepth > 18) return false;

        // HUD-safe naming (goal lines are displayed in UI/log).
        const auto gl = victorygen::goalLines(p);
        for (const auto& ln : gl) {
            if (ln.empty()) return false;
            for (const char c : ln) {
                const bool ok = (c >= 'A' && c <= 'Z')
                             || (c >= '0' && c <= '9')
                             || c == ' ' || c == ':' || c == '.' || c == ','
                             || c == '(' || c == ')' || c == '-' || c == '+'
                             || c == '/' || c == '&' || c == '_' || c == '>' || c == '=' || c == '<';
                if (!ok) return false;
            }
        }
    }

    return true;
}




bool test_overworld_profiles() {
    const uint32_t seed = 123456u;

    const auto p0 = overworld::profileFor(seed, 1, 2, /*maxDepth=*/25);
    const auto p1 = overworld::profileFor(seed, 1, 2, /*maxDepth=*/25);
    CHECK(p0.seed == p1.seed);
    CHECK(p0.materialSeed == p1.materialSeed);
    CHECK(p0.nameSeed == p1.nameSeed);
    CHECK(p0.biome == p1.biome);
    CHECK(p0.dangerDepth == p1.dangerDepth);
    CHECK(overworld::chunkNameFor(p0) == overworld::chunkNameFor(p1));
    CHECK(!overworld::chunkNameFor(p0).empty());

    // Different chunk should usually differ in at least one identity dimension.
    const auto q0 = overworld::profileFor(seed, 2, 2, /*maxDepth=*/25);
    CHECK(overworld::chunkNameFor(q0) != overworld::chunkNameFor(p0) ||
          q0.biome != p0.biome ||
          q0.dangerDepth != p0.dangerDepth);

    // Danger depth increases (or at least does not decrease) with Manhattan distance.
    const auto near = overworld::profileFor(seed, 1, 0, /*maxDepth=*/25);
    const auto far  = overworld::profileFor(seed, 5, 0, /*maxDepth=*/25);
    CHECK(far.dangerDepth >= near.dangerDepth);

    // Home camp is always depth 0.
    const auto home = overworld::profileFor(seed, 0, 0, /*maxDepth=*/25);
    CHECK(home.dangerDepth == 0);

    return true;
}




    



bool test_overworld_biome_diversity() {
    const uint32_t seed = 424242u;
    const int maxDepth = 25;

    // Sanity: across a neighborhood we should see a mix of biome identities.
    // We keep this threshold modest so it remains robust across tuning.
    bool seen[8] = {false,false,false,false,false,false,false,false};
    int unique = 0;

    for (int cy = -12; cy <= 12; ++cy) {
        for (int cx = -12; cx <= 12; ++cx) {
            const auto p = overworld::profileFor(seed, cx, cy, maxDepth);
            const int bi = static_cast<int>(p.biome);
            if (bi >= 0 && bi < 8 && !seen[bi]) {
                seen[bi] = true;
                unique++;
            }
        }
    }

    CHECK(unique >= 4);

    // Prevailing climate wind is cardinal and never calm.
    const Vec2i wdir = overworld::climateWindDir(seed);
    CHECK((wdir.x == 0) || (wdir.y == 0));
    CHECK(!((wdir.x != 0) && (wdir.y != 0)));
    CHECK((wdir.x >= -1) && (wdir.x <= 1));
    CHECK((wdir.y >= -1) && (wdir.y <= 1));
    CHECK(!(wdir.x == 0 && wdir.y == 0));

    return true;
}

bool test_overworld_chunk_determinism() {
    const uint32_t seed = 424242u;
    const int W = 80;
    const int H = 50;

    Dungeon a(W, H);
    Dungeon b(W, H);

    const int cx = 7;
    const int cy = -3;
    overworld::generateWildernessChunk(a, seed, cx, cy);
    overworld::generateWildernessChunk(b, seed, cx, cy);

    CHECK(a.width == b.width);
    CHECK(a.height == b.height);
    CHECK(a.rooms.size() == b.rooms.size());

    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            CHECK(a.at(x, y).type == b.at(x, y).type);
        }
    }

    // Spot-check a couple of aggregate counters used by the procgen debug UI.
    CHECK(a.fluvialChasmCount == b.fluvialChasmCount);
    CHECK(a.heightfieldScreeBoulderCount == b.heightfieldScreeBoulderCount);
    CHECK(a.heightfieldRidgePillarCount == b.heightfieldRidgePillarCount);

    return true;
}




bool test_overworld_trails_connectivity() {
    const uint32_t seed = 424242u;
    const int W = 80;
    const int H = 50;

    const int cx = 7;
    const int cy = -3;

    Dungeon d(W, H);
    overworld::generateWildernessChunk(d, seed, cx, cy);

    const overworld::ChunkGates g = overworld::gatePositions(d, seed, cx, cy);

    // Gate "throat" tiles (1 step inside the chunk boundary).
    const Vec2i startN{g.north.x, g.north.y + 1};
    const Vec2i startS{g.south.x, g.south.y - 1};
    const Vec2i startW{g.west.x + 1, g.west.y};
    const Vec2i startE{g.east.x - 1, g.east.y};

    auto idx = [&](Vec2i p) -> int { return p.y * d.width + p.x; };

    auto bfsDist = [&](Vec2i s) -> std::vector<int> {
        std::vector<int> dist(static_cast<size_t>(d.width * d.height), -1);
        if (!d.inBounds(s.x, s.y)) return dist;
        if (!d.isWalkable(s.x, s.y)) return dist;

        std::queue<Vec2i> q;
        dist[static_cast<size_t>(idx(s))] = 0;
        q.push(s);

        static const int DX[4] = { 1, -1, 0, 0 };
        static const int DY[4] = { 0, 0, 1, -1 };

        while (!q.empty()) {
            const Vec2i p = q.front();
            q.pop();
            const int base = dist[static_cast<size_t>(idx(p))];

            for (int k = 0; k < 4; ++k) {
                const int nx = p.x + DX[k];
                const int ny = p.y + DY[k];
                if (!d.inBounds(nx, ny)) continue;
                if (!d.isWalkable(nx, ny)) continue;

                const int ni = ny * d.width + nx;
                if (dist[static_cast<size_t>(ni)] >= 0) continue;

                dist[static_cast<size_t>(ni)] = base + 1;
                q.push({nx, ny});
            }
        }

        return dist;
    };

    CHECK(d.isWalkable(startN.x, startN.y));
    CHECK(d.isWalkable(startS.x, startS.y));
    CHECK(d.isWalkable(startW.x, startW.y));
    CHECK(d.isWalkable(startE.x, startE.y));

    // All gates should be mutually reachable (the trail network is required to
    // connect every gate back into the chunk's hub crossroads).
    const auto dist = bfsDist(startN);
    CHECK(dist[static_cast<size_t>(idx(startS))] >= 0);
    CHECK(dist[static_cast<size_t>(idx(startW))] >= 0);
    CHECK(dist[static_cast<size_t>(idx(startE))] >= 0);

    return true;
}

bool test_overworld_rivers_and_banks_present() {
    const uint32_t runSeed = 424242u;
    const int W = 80;
    const int H = 50;

    bool foundWater = false;
    bool foundBankAdjacency = false;

    // Scan a small neighborhood of chunks and assert that we can find:
    //  1) at least one chunk with some water, and
    //  2) at least one chunk where bank obstacles appear adjacent to water.
    // We intentionally don't pin exact counts so that the test remains robust
    // across minor tuning changes.
    for (int cy = -6; cy <= 6; ++cy) {
        for (int cx = -6; cx <= 6; ++cx) {
            Dungeon d(W, H);
            overworld::generateWildernessChunk(d, runSeed, cx, cy);

            int waterCount = 0;
            int bankAdj = 0;

            for (int y = 1; y < H - 1; ++y) {
                for (int x = 1; x < W - 1; ++x) {
                    if (d.at(x, y).type != TileType::Chasm) continue;
                    waterCount++;

                    const TileType n = d.at(x, y - 1).type;
                    const TileType s = d.at(x, y + 1).type;
                    const TileType w = d.at(x - 1, y).type;
                    const TileType e = d.at(x + 1, y).type;

                    auto isBank = [](TileType t) {
                        return t == TileType::Pillar || t == TileType::Boulder;
                    };

                    bankAdj += (int)isBank(n);
                    bankAdj += (int)isBank(s);
                    bankAdj += (int)isBank(w);
                    bankAdj += (int)isBank(e);
                }
            }

            if (waterCount >= 12) foundWater = true;
            if (bankAdj >= 4) foundBankAdjacency = true;

            if (foundWater && foundBankAdjacency) {
                CHECK(true);
                return true;
            }
        }
    }

    CHECK(foundWater);
    CHECK(foundBankAdjacency);
    return foundWater && foundBankAdjacency;
}


bool test_overworld_springs_present() {
    const uint32_t runSeed = 424242u;
    const int W = 80;
    const int H = 50;

    bool foundSpring = false;

    // Scan a neighborhood of chunks and assert that at least one chunk contains
    // a spring (TileType::Fountain). We don't pin exact counts so that tuning
    // remains flexible.
    for (int cy = -6; cy <= 6; ++cy) {
        for (int cx = -6; cx <= 6; ++cx) {
            Dungeon d(W, H);
            overworld::generateWildernessChunk(d, runSeed, cx, cy);

            int fountains = 0;
            for (int y = 1; y < H - 1; ++y) {
                for (int x = 1; x < W - 1; ++x) {
                    if (d.at(x, y).type == TileType::Fountain) ++fountains;
                }
            }

            if (fountains > 0) {
                foundSpring = true;
                CHECK(true);
                return true;
            }
        }
    }

    CHECK(foundSpring);
    return foundSpring;
}

bool test_dungeon_seep_springs_present() {
    constexpr int W = 80;
    constexpr int H = 50;
    constexpr int maxDepth = 20;

    bool foundSpring = false;

    // Sample several run seeds/depths. We don't pin exact counts, but we do
    // require deterministic placement and at least one spring across the sample.
    for (int s = 0; s < 6; ++s) {
        const uint32_t runSeed = 0xA51CEEDu + static_cast<uint32_t>(s) * 977u;

        RNG rngA(runSeed);
        RNG rngB(runSeed);

        for (int depth = 2; depth <= 12; ++depth) {
            Dungeon a(W, H);
            Dungeon b(W, H);

            a.generate(rngA, DungeonBranch::Main, depth, maxDepth, runSeed);
            b.generate(rngB, DungeonBranch::Main, depth, maxDepth, runSeed);

            int fa = 0;
            int fb = 0;

            for (int y = 1; y < H - 1; ++y) {
                for (int x = 1; x < W - 1; ++x) {
                    if (a.at(x, y).type == TileType::Fountain) {
                        ++fa;

                        // Springs are tactical POIs, but should not overlap stairs.
                        CHECK(!(x == a.stairsUp.x && y == a.stairsUp.y));
                        CHECK(!(x == a.stairsDown.x && y == a.stairsDown.y));

                        // The pass intentionally keeps spring tiles away from stair landings.
                        if (a.inBounds(a.stairsUp.x, a.stairsUp.y)) {
                            CHECK(manhattan2({x, y}, a.stairsUp) > 6);
                        }
                        if (a.inBounds(a.stairsDown.x, a.stairsDown.y)) {
                            CHECK(manhattan2({x, y}, a.stairsDown) > 6);
                        }
                    }
                    if (b.at(x, y).type == TileType::Fountain) ++fb;
                }
            }

            CHECK(fa == fb);

            // Main-branch dungeon floors introduce fountains via two deterministic
            // passes: seep springs + hydro confluence weaving.
            CHECK(a.dungeonSeepFountainTiles + a.dungeonConfluenceFountainTiles == fa);
            CHECK(a.dungeonSeepSpringCount <= a.dungeonSeepFountainTiles);
            CHECK(a.dungeonConfluenceCount <= a.dungeonConfluenceFountainTiles);
            if (a.dungeonSeepSpringCount > 0) CHECK(a.dungeonSeepFountainTiles > 0);
            if (a.dungeonConfluenceCount > 0) CHECK(a.dungeonConfluenceFountainTiles > 0);
            CHECK(a.dungeonConfluenceCount == b.dungeonConfluenceCount);
            CHECK(a.dungeonConfluenceFountainTiles == b.dungeonConfluenceFountainTiles);

            if (fa > 0) foundSpring = true;
        }
    }

    CHECK(foundSpring);
    return foundSpring;
}

bool test_dungeon_levels_have_fishing_water() {
    constexpr int W = 80;
    constexpr int H = 50;
    constexpr int maxDepth = 20;

    // Across multiple run seeds and all campaign depths, each generated floor
    // should include at least one fishable water source (fountain or chasm).
    for (int s = 0; s < 4; ++s) {
        const uint32_t runSeed = 0xF157500u + static_cast<uint32_t>(s) * 1337u;
        RNG rng(runSeed);

        for (int depth = 1; depth <= maxDepth; ++depth) {
            Dungeon d(W, H);
            d.generate(rng, DungeonBranch::Main, depth, maxDepth, runSeed);

            int fishable = 0;
            for (int y = 1; y < H - 1; ++y) {
                for (int x = 1; x < W - 1; ++x) {
                    const TileType tt = d.at(x, y).type;
                    if (tt == TileType::Fountain || tt == TileType::Chasm) ++fishable;
                }
            }
            CHECK(fishable > 0);
        }
    }

    return true;
}

bool test_fishing_targeting_accepts_chasm_tiles() {
    Game g;
    g.newGame(0xF15A123u);

    // Keep only the player so line/target validation is not perturbed by incidental spawns.
    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;
    g.ground.clear();
    g.trapsCur.clear();
    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    g.inv.clear();
    g.invSel = 0;
    g.targeting = false;

    Item rod;
    rod.id = g.nextItemId++;
    rod.kind = ItemKind::FishingRod;
    rod.count = 1;
    rod.spriteSeed = 0x1234u;
    rod.charges = itemDef(rod.kind).maxCharges;
    g.inv.push_back(rod);

    // Build a clean, visible floor with one fishable chasm tile.
    for (int y = 0; y < g.dung.height; ++y) {
        for (int x = 0; x < g.dung.width; ++x) {
            Tile& t = g.dung.at(x, y);
            t.type = TileType::Floor;
            t.visible = true;
            t.explored = true;
        }
    }

    Vec2i p{std::clamp(g.dung.width / 2, 2, g.dung.width - 3), std::clamp(g.dung.height / 2, 2, g.dung.height - 3)};
    g.playerMut().pos = p;

    Vec2i water{p.x + 2, p.y};
    if (!g.dung.inBounds(water.x, water.y) || water.x >= g.dung.width - 1) {
        water = {p.x - 2, p.y};
    }
    CHECK(g.dung.inBounds(water.x, water.y));
    g.dung.at(water.x, water.y).type = TileType::Chasm;

    g.beginFishingTargeting(rod.id);

    CHECK(g.targeting);
    CHECK(g.targetingMode_ == Game::TargetingMode::Fish);
    CHECK(g.targetPos.x == water.x);
    CHECK(g.targetPos.y == water.y);
    CHECK(g.targetValid);
    CHECK(g.targetStatusText_.empty());

    return true;
}

bool test_fishing_targeting_no_visible_water_feedback() {
    Game g;
    g.newGame(0xF15A222u);

    // Keep only the player so "no visible water" feedback remains deterministic.
    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;
    g.ground.clear();
    g.trapsCur.clear();
    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    g.inv.clear();
    g.invSel = 0;
    g.targeting = false;

    Item rod;
    rod.id = g.nextItemId++;
    rod.kind = ItemKind::FishingRod;
    rod.count = 1;
    rod.spriteSeed = 0x2222u;
    rod.charges = itemDef(rod.kind).maxCharges;
    g.inv.push_back(rod);

    // Remove all fishable tiles and reveal the map.
    for (int y = 0; y < g.dung.height; ++y) {
        for (int x = 0; x < g.dung.width; ++x) {
            Tile& t = g.dung.at(x, y);
            t.type = TileType::Floor;
            t.visible = true;
            t.explored = true;
        }
    }

    Vec2i p{std::clamp(g.dung.width / 2, 2, g.dung.width - 3), std::clamp(g.dung.height / 2, 2, g.dung.height - 3)};
    g.playerMut().pos = p;

    const size_t msgBefore = g.messages().size();
    g.beginFishingTargeting(rod.id);

    CHECK(!g.targeting);
    CHECK(!g.messages().empty());
    CHECK(g.messages().size() >= msgBefore);
    CHECK(containsCaseInsensitive(g.messages().back().text, "NO VISIBLE WATER"));

    return true;
}

bool test_inventory_context_use_opens_fishing_and_crafting_tools() {
    Game g;
    g.newGame(0xF15A2026u);

    // Keep only the player so inventory->targeting transitions stay deterministic.
    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;
    g.ground.clear();
    g.trapsCur.clear();
    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    g.inv.clear();
    g.invSel = 0;
    g.targeting = false;

    Item rod;
    rod.id = g.nextItemId++;
    rod.kind = ItemKind::FishingRod;
    rod.count = 1;
    rod.spriteSeed = 0x1919u;
    rod.charges = itemDef(rod.kind).maxCharges;
    g.inv.push_back(rod);

    Item kit;
    kit.id = g.nextItemId++;
    kit.kind = ItemKind::CraftingKit;
    kit.count = 1;
    kit.spriteSeed = 0x2020u;
    g.inv.push_back(kit);

    Item rock;
    rock.id = g.nextItemId++;
    rock.kind = ItemKind::Rock;
    rock.count = 2;
    rock.spriteSeed = 0x3030u;
    g.inv.push_back(rock);

    // Build a clean, visible floor with one fishable chasm tile in range.
    for (int y = 0; y < g.dung.height; ++y) {
        for (int x = 0; x < g.dung.width; ++x) {
            Tile& t = g.dung.at(x, y);
            t.type = TileType::Floor;
            t.visible = true;
            t.explored = true;
        }
    }

    Vec2i p{std::clamp(g.dung.width / 2, 2, g.dung.width - 3), std::clamp(g.dung.height / 2, 2, g.dung.height - 3)};
    g.playerMut().pos = p;

    Vec2i water{p.x + 2, p.y};
    if (!g.dung.inBounds(water.x, water.y) || water.x >= g.dung.width - 1) {
        water = {p.x - 2, p.y};
    }
    CHECK(g.dung.inBounds(water.x, water.y));
    g.dung.at(water.x, water.y).type = TileType::Chasm;

    auto findInvByKind = [&](ItemKind k) -> int {
        for (int i = 0; i < static_cast<int>(g.inv.size()); ++i) {
            if (g.inv[static_cast<size_t>(i)].kind == k) return i;
        }
        return -1;
    };

    // ENTER on fishing rod should start fish targeting (no turn spent yet).
    g.openInventory();
    g.invSel = findInvByKind(ItemKind::FishingRod);
    CHECK(g.invSel >= 0);
    const uint32_t t0 = g.turns();
    g.handleAction(Action::Confirm);
    CHECK(g.targeting);
    CHECK(g.targetingMode_ == Game::TargetingMode::Fish);
    CHECK(!g.isInventoryOpen());
    CHECK(g.turns() == t0);

    g.handleAction(Action::Cancel);
    CHECK(!g.targeting);

    // EQUIP/WIELD hotkey on fishing rod should use the same path.
    g.openInventory();
    g.invSel = findInvByKind(ItemKind::FishingRod);
    CHECK(g.invSel >= 0);
    const uint32_t t1 = g.turns();
    g.handleAction(Action::Equip);
    CHECK(g.targeting);
    CHECK(g.targetingMode_ == Game::TargetingMode::Fish);
    CHECK(!g.isInventoryOpen());
    CHECK(g.turns() == t1);

    g.handleAction(Action::Cancel);
    CHECK(!g.targeting);

    // ENTER on crafting kit should open crafting mode in inventory (no turn spent).
    g.openInventory();
    g.invSel = findInvByKind(ItemKind::CraftingKit);
    CHECK(g.invSel >= 0);
    const uint32_t t2 = g.turns();
    g.handleAction(Action::Confirm);
    CHECK(g.isInventoryOpen());
    CHECK(g.isInventoryCraftMode());
    CHECK(g.turns() == t2);

    // Exit crafting mode while keeping inventory open.
    g.handleAction(Action::Inventory);
    CHECK(g.isInventoryOpen());
    CHECK(!g.isInventoryCraftMode());

    // EQUIP/WIELD hotkey on crafting kit should also open crafting mode.
    g.invSel = findInvByKind(ItemKind::CraftingKit);
    CHECK(g.invSel >= 0);
    const uint32_t t3 = g.turns();
    g.handleAction(Action::Equip);
    CHECK(g.isInventoryOpen());
    CHECK(g.isInventoryCraftMode());
    CHECK(g.turns() == t3);

    return true;
}




bool test_overworld_brooks_present() {
    const uint32_t runSeed = 424242u;
    const int W = 80;
    const int H = 50;

    bool foundSpringWithWater = false;

    // Scan a neighborhood of chunks and assert that at least one spring has
    // adjacent water (a spring-fed brook or terminal pond). We don't pin exact
    // counts so tuning remains flexible.
    for (int cy = -6; cy <= 6; ++cy) {
        for (int cx = -6; cx <= 6; ++cx) {
            Dungeon d(W, H);
            overworld::generateWildernessChunk(d, runSeed, cx, cy);

            for (int y = 2; y < H - 2; ++y) {
                for (int x = 2; x < W - 2; ++x) {
                    if (d.at(x, y).type != TileType::Fountain) continue;

                    bool adjWater = false;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            if (d.at(x + dx, y + dy).type == TileType::Chasm) {
                                adjWater = true;
                                break;
                            }
                        }
                        if (adjWater) break;
                    }

                    if (adjWater) {
                        foundSpringWithWater = true;
                        CHECK(true);
                        return true;
                    }
                }
            }
        }
    }

    CHECK(foundSpringWithWater);
    return foundSpringWithWater;
}

bool test_overworld_strongholds_present() {
    const uint32_t runSeed = 424242u;
    const int W = 80;
    const int H = 50;

    bool foundStronghold = false;
    bool foundCache = false;

    // Strongholds are rare, so scan a slightly wider neighborhood. We avoid
    // pinning exact counts so tuning remains flexible.
    for (int cy = -10; cy <= 10; ++cy) {
        for (int cx = -10; cx <= 10; ++cx) {
            Dungeon d(W, H);
            overworld::generateWildernessChunk(d, runSeed, cx, cy);

            if (d.overworldStrongholdCount <= 0) continue;
            foundStronghold = true;

            // Strongholds mark their keep footprint as a Vault room (used for UI detection).
            bool hasVault = false;
            for (const auto& r : d.rooms) {
                if (r.type == RoomType::Vault) {
                    hasVault = true;
                    break;
                }
            }
            CHECK(hasVault);


            if (!d.bonusLootSpots.empty() && d.overworldStrongholdCacheCount > 0) {
                foundCache = true;
            }

            if (foundStronghold && foundCache) {
                CHECK(true);
                return true;
            }
        }
    }

    CHECK(foundStronghold);
    CHECK(foundCache);
    return foundStronghold && foundCache;
}

bool test_overworld_tectonic_ridge_field_extrema() {
    const uint32_t runSeed = 424242u;

    float mn = 1.0f;
    float mx = 0.0f;

    // Sample a broad world-space grid. The Voronoi ridge mask should produce
    // both low "plate interior" values and high "boundary ridge" values.
    for (int wy = -1200; wy <= 1200; wy += 60) {
        for (int wx = -1200; wx <= 1200; wx += 60) {
            const float r = overworld::tectonicRidge01(runSeed, wx, wy);
            mn = std::min(mn, r);
            mx = std::max(mx, r);
        }
    }

    CHECK(mx > 0.70f);
    CHECK(mn < 0.20f);
    return true;
}

bool test_wet_laboratory_chemical_hazards_generate_adhesive_fields() {
    auto setupWetLab = [](Game& g) {
        g.branch_ = DungeonBranch::Main;
        g.depth_ = 9;

        g.dung = Dungeon(28, 18);
        g.dung.rooms.clear();

        for (int y = 0; y < g.dung.height; ++y) {
            for (int x = 0; x < g.dung.width; ++x) {
                Tile& t = g.dung.at(x, y);
                t.type = TileType::Floor;
                t.visible = true;
                t.explored = true;
            }
        }

        g.dung.stairsUp = {1, 1};
        g.dung.stairsDown = {2, 1};

        Room lab;
        lab.x = 8;
        lab.y = 5;
        lab.w = 12;
        lab.h = 8;
        lab.type = RoomType::Laboratory;
        g.dung.rooms.push_back(lab);

        // Active water source inside the laboratory should force an adhesive spill theme.
        const Vec2i f{lab.cx(), lab.cy()};
        g.dung.at(f.x, f.y).type = TileType::Fountain;

        g.playerMut().pos = Vec2i{1, 1};

        const size_t n = static_cast<size_t>(g.dung.width * g.dung.height);
        g.confusionGas_.assign(n, uint8_t{0});
        g.poisonGas_.assign(n, uint8_t{0});
        g.corrosiveGas_.assign(n, uint8_t{0});
        g.fireField_.assign(n, uint8_t{0});
        g.adhesiveFluid_.assign(n, uint8_t{0});
    };

    Game a;
    a.newGame(0xC0DE5EEDu);
    setupWetLab(a);
    a.spawnChemicalHazards();

    int adhesiveA = 0;
    for (uint8_t v : a.adhesiveFluid_) {
        if (v > 0u) ++adhesiveA;
    }
    CHECK(adhesiveA > 0);

    // Determinism check: same run seed + same level setup -> exact same field output.
    Game b;
    b.newGame(0xC0DE5EEDu);
    setupWetLab(b);
    b.spawnChemicalHazards();

    CHECK(a.adhesiveFluid_ == b.adhesiveFluid_);
    CHECK(a.poisonGas_ == b.poisonGas_);
    CHECK(a.corrosiveGas_ == b.corrosiveGas_);
    CHECK(a.confusionGas_ == b.confusionGas_);
    CHECK(a.fireField_ == b.fireField_);
    return true;
}



bool test_overworld_weather_determinism() {
    const uint32_t seed = 424242u;
    const int cx = 7;
    const int cy = -3;

    const auto prof = overworld::profileFor(seed, cx, cy, /*maxDepth=*/25);
    const uint32_t t = 7777u;
    const auto w0 = overworld::weatherFor(seed, cx, cy, prof.biome, t);
    const auto w1 = overworld::weatherFor(seed, cx, cy, prof.biome, t);

    CHECK(w0.kind == w1.kind);
    CHECK(w0.windDir.x == w1.windDir.x && w0.windDir.y == w1.windDir.y);
    CHECK(w0.windStrength == w1.windStrength);
    CHECK(w0.fovPenalty == w1.fovPenalty);
    CHECK(w0.fireQuench == w1.fireQuench);
    CHECK(w0.burnQuench == w1.burnQuench);

    // Wind direction should be cardinal or calm.
    CHECK((w0.windDir.x == 0) || (w0.windDir.y == 0));
    CHECK(!((w0.windDir.x != 0) && (w0.windDir.y != 0)));
    CHECK((w0.windDir.x >= -1) && (w0.windDir.x <= 1));
    CHECK((w0.windDir.y >= -1) && (w0.windDir.y <= 1));

    // Wind strength should be 0..3 and matches calm direction.
    CHECK((w0.windStrength >= 0) && (w0.windStrength <= 3));
    if (w0.windStrength == 0) {
        CHECK(w0.windDir.x == 0 && w0.windDir.y == 0);
    }

    return true;
}


bool test_overworld_weather_time_varying_fronts() {
    const uint32_t seed = 424242u;

    // We expect at least one nearby chunk's weather to differ between two
    // distant times if time-varying fronts are active.
    const uint32_t t0 = 0u;
    const uint32_t t1 = 5000u;

    bool changed = false;
    for (int cy = -4; cy <= 4 && !changed; ++cy) {
        for (int cx = -4; cx <= 4 && !changed; ++cx) {
            const auto prof = overworld::profileFor(seed, cx, cy, /*maxDepth=*/25);
            const auto w0 = overworld::weatherFor(seed, cx, cy, prof.biome, t0);
            const auto w1 = overworld::weatherFor(seed, cx, cy, prof.biome, t1);

            if (w0.kind != w1.kind ||
                w0.windDir.x != w1.windDir.x || w0.windDir.y != w1.windDir.y ||
                w0.windStrength != w1.windStrength ||
                w0.fovPenalty != w1.fovPenalty ||
                w0.fireQuench != w1.fireQuench ||
                w0.burnQuench != w1.burnQuench) {
                changed = true;
            }
        }
    }
    CHECK(changed);

    // But each time slice must remain deterministic.
    const int cx = 7;
    const int cy = -3;
    const auto prof = overworld::profileFor(seed, cx, cy, /*maxDepth=*/25);
    const auto a = overworld::weatherFor(seed, cx, cy, prof.biome, t1);
    const auto b = overworld::weatherFor(seed, cx, cy, prof.biome, t1);

    CHECK(a.kind == b.kind);
    CHECK(a.windDir.x == b.windDir.x && a.windDir.y == b.windDir.y);
    CHECK(a.windStrength == b.windStrength);
    CHECK(a.fovPenalty == b.fovPenalty);
    CHECK(a.fireQuench == b.fireQuench);
    CHECK(a.burnQuench == b.burnQuench);

    return true;
}





bool test_save_load_roundtrip_overworld_chunk() {
    Game g;
    g.newGame(12345u);

    // Travel to a non-home overworld chunk (1, 0).
    {
        Entity& p = g.playerMut();
        const Dungeon& d = g.dungeon();
        const overworld::ChunkGates gates = overworld::gatePositions(d, g.seed(), 0, 0);
        p.pos = gates.east;
        CHECK(d.isWalkable(p.pos.x, p.pos.y));
        g.handleAction(Action::Right);
    }

    CHECK(g.atCamp());
    CHECK(!g.atHomeCamp());
    CHECK(g.overworldX() == 1);
    CHECK(g.overworldY() == 0);

    // Ensure the current tile is explored so markers are allowed.
    g.handleAction(Action::Wait);
    const Vec2i pos = g.player().pos;
    CHECK(g.setMarker(pos, MarkerKind::Note, "OW-SAVE-TEST"));

    const fs::path path = testTempFile("procrogue_test_overworld_save.prs");
    CHECK(g.saveToFile(path.string(), true));

    Game g2;
    CHECK(g2.loadFromFile(path.string(), true));
    CHECK(g2.atCamp());
    CHECK(!g2.atHomeCamp());
    CHECK(g2.overworldX() == 1);
    CHECK(g2.overworldY() == 0);

    const MapMarker* m = g2.markerAt(g2.player().pos);
    CHECK(m != nullptr);
    CHECK(m->kind == MarkerKind::Note);
    CHECK(m->label == "OW-SAVE-TEST");

    return true;
}

bool test_save_load_roundtrip_home_camp() {
    Game g;
    g.newGame(424242u);

    CHECK(g.atCamp());
    CHECK(g.atHomeCamp());
    CHECK(g.overworldX() == 0);
    CHECK(g.overworldY() == 0);

    const fs::path path = testTempFile("procrogue_test_home_camp_save.prs");
    const bool saved = g.saveToFile(path.string(), true);
    CHECK(saved);
    if (!saved) return false;

    Game g2;
    const bool loaded = g2.loadFromFile(path.string(), true);
    CHECK(loaded);
    if (!loaded) return false;

    CHECK(g2.atCamp());
    CHECK(g2.atHomeCamp());
    CHECK(g2.overworldX() == 0);
    CHECK(g2.overworldY() == 0);

    // Regression check: home camp should restore via the normal level store and not leave an empty dungeon.
    CHECK(g2.dungeon().width > 0);
    CHECK(g2.dungeon().height > 0);
    CHECK(!g2.dungeon().tiles.empty());

    return true;
}

bool test_overworld_atlas_discovery_tracking() {
    Game g;
    g.newGame(1337u);

    // Home camp should always be considered discovered.
    CHECK(g.overworldChunkDiscovered(0, 0));

    // Move the player to the east edge gate and step into a new overworld chunk.
    const Dungeon& d = g.dungeon();
    Entity& p = g.playerMut();
    const overworld::ChunkGates g0 = overworld::gatePositions(d, g.seed(), 0, 0);
    p.pos = g0.east;

    CHECK(d.inBounds(p.pos.x, p.pos.y));
    CHECK(d.isWalkable(p.pos.x, p.pos.y));

    g.handleAction(Action::Right);
    CHECK(g.overworldX() == 1);
    CHECK(g.overworldY() == 0);

    // The destination chunk should be marked as discovered for the atlas.
    CHECK(g.overworldChunkDiscovered(1, 0));

    // And the current chunk must have an in-memory dungeon snapshot.
    CHECK(g.overworldChunkDungeon(1, 0) != nullptr);

    return true;
}


bool test_overworld_atlas_feature_flags_save_load() {
    Game g;
    g.newGame(12345u);

    const int cx = 7;
    const int cy = -4;
    const uint8_t flags = static_cast<uint8_t>(Game::OW_FEATURE_WAYSTATION | Game::OW_FEATURE_STRONGHOLD);

    GameTestAccess::discoverOverworldChunk(g, cx, cy);
    GameTestAccess::setOverworldChunkFeatureFlags(g, cx, cy, flags);

    const fs::path path = testTempFile("procrogue_test_atlas_feature_flags_v56.prs");
    const bool saved = g.saveToFile(path.string(), true);
    CHECK(saved);
    if (!saved) return false;

    Game g2;
    const bool loaded = g2.loadFromFile(path.string(), false);
    CHECK(loaded);
    if (!loaded) return false;

    CHECK(g2.overworldChunkFeatureFlags(cx, cy) == flags);
    CHECK(g2.overworldChunkHasWaystation(cx, cy));
    CHECK(g2.overworldChunkHasStronghold(cx, cy));

    // Ensure the atlas flags still work even if the chunk snapshot is evicted.
    GameTestAccess::clearOverworldChunkCache(g2);
    CHECK(g2.overworldChunkHasStronghold(cx, cy));

    return true;
}



bool test_overworld_atlas_terrain_save_load() {
    Game g;
    g.newGame(54321u);

    const int cx = -3;
    const int cy = 9;

    // Fake-but-stable counts: we only validate persistence here, not generation.
    const int chasm = 17;
    const int boulder = 23;
    const int pillar = 5;

    GameTestAccess::discoverOverworldChunk(g, cx, cy);
    GameTestAccess::setOverworldChunkTerrainSummary(g, cx, cy, chasm, boulder, pillar);

    const fs::path path = testTempFile("procrogue_test_atlas_terrain_v57.prs");
    const bool saved = g.saveToFile(path.string(), true);
    CHECK(saved);
    if (!saved) return false;

    Game g2;
    const bool loaded = g2.loadFromFile(path.string(), false);
    CHECK(loaded);
    if (!loaded) return false;

    Game::OverworldTerrainSummary ts;
    CHECK(g2.overworldChunkTerrainSummary(cx, cy, ts));
    CHECK(ts.chasmTiles == chasm);
    CHECK(ts.boulderTiles == boulder);
    CHECK(ts.pillarTiles == pillar);

    // Ensure the terrain summary still works even if the chunk snapshot is evicted.
    GameTestAccess::clearOverworldChunkCache(g2);
    Game::OverworldTerrainSummary ts2;
    CHECK(g2.overworldChunkTerrainSummary(cx, cy, ts2));
    CHECK(ts2.chasmTiles == chasm);
    CHECK(ts2.boulderTiles == boulder);
    CHECK(ts2.pillarTiles == pillar);

    return true;
}






bool test_overworld_atlas_waypoint_save_load() {
    Game g;
    g.newGame(24680u);

    // Waypoint can be anywhere in chunk-space (even undiscovered).
    const Vec2i wp{12, -7};
    g.setOverworldWaypoint(wp);

    const fs::path path = testTempFile("procrogue_test_atlas_waypoint_v58.prs");
    const bool saved = g.saveToFile(path.string(), true);
    CHECK(saved);
    if (!saved) return false;

    Game g2;
    const bool loaded = g2.loadFromFile(path.string(), false);
    CHECK(loaded);
    if (!loaded) return false;

    CHECK(g2.overworldWaypointIsSet());
    CHECK(g2.overworldWaypoint().x == wp.x);
    CHECK(g2.overworldWaypoint().y == wp.y);

    // Clear + re-roundtrip should keep it cleared.
    g2.clearOverworldWaypoint();

    const fs::path path2 = testTempFile("procrogue_test_atlas_waypoint_clear_v58.prs");
    const bool saved2 = g2.saveToFile(path2.string(), true);
    CHECK(saved2);
    if (!saved2) return false;

    Game g3;
    const bool loaded2 = g3.loadFromFile(path2.string(), false);
    CHECK(loaded2);
    if (!loaded2) return false;

    CHECK(!g3.overworldWaypointIsSet());
    return true;
}

bool test_overworld_atlas_route_preview() {
    Game g;
    g.newGame(424242u);

    // Build a small discovered graph:
    // (0,0) -> (1,0) -> (2,0) -> (2,1)
    GameTestAccess::discoverOverworldChunk(g, 1, 0);
    GameTestAccess::discoverOverworldChunk(g, 2, 0);
    GameTestAccess::discoverOverworldChunk(g, 2, 1);

    std::vector<Vec2i> path;
    CHECK(g.overworldRouteDiscoveredChunks(Vec2i{0, 0}, Vec2i{2, 1}, path));
    CHECK(static_cast<int>(path.size()) == 4);

    // Endpoints should match.
    CHECK(path.front().x == 0);
    CHECK(path.front().y == 0);
    CHECK(path.back().x == 2);
    CHECK(path.back().y == 1);

    // Undiscovered goal should fail.
    path.clear();
    CHECK(!g.overworldRouteDiscoveredChunks(Vec2i{0, 0}, Vec2i{9, 9}, path));
    CHECK(path.empty());

    // Disconnected discovery should also fail.
    GameTestAccess::discoverOverworldChunk(g, 10, 10);
    path.clear();
    CHECK(!g.overworldRouteDiscoveredChunks(Vec2i{0, 0}, Vec2i{10, 10}, path));

    return true;
}

bool test_overworld_atlas_nearest_landmark() {
    Game g;
    g.newGame(424242u);

    // Build a small discovered graph:
    // (0,0) -> (1,0) -> (2,0) -> (2,1)
    GameTestAccess::discoverOverworldChunk(g, 1, 0);
    GameTestAccess::discoverOverworldChunk(g, 2, 0);
    GameTestAccess::discoverOverworldChunk(g, 2, 1);

    GameTestAccess::setOverworldChunkFeatureFlags(g, 2, 1, static_cast<uint8_t>(Game::OW_FEATURE_WAYSTATION));
    GameTestAccess::setOverworldChunkFeatureFlags(g, 1, 0, static_cast<uint8_t>(Game::OW_FEATURE_STRONGHOLD));

    Vec2i nearest;
    int legs = 0;

    CHECK(g.overworldNearestLandmark(Game::OW_FEATURE_STRONGHOLD, nearest, &legs));
    CHECK(nearest.x == 1);
    CHECK(nearest.y == 0);
    CHECK(legs == 1);

    legs = 0;
    CHECK(g.overworldNearestLandmark(Game::OW_FEATURE_WAYSTATION, nearest, &legs));
    CHECK(nearest.x == 2);
    CHECK(nearest.y == 1);
    CHECK(legs == 3);

    // Union mask should return the nearest matching type (stronghold at 1,0).
    legs = 0;
    CHECK(g.overworldNearestLandmark(static_cast<uint8_t>(Game::OW_FEATURE_WAYSTATION | Game::OW_FEATURE_STRONGHOLD), nearest, &legs));
    CHECK(nearest.x == 1);
    CHECK(nearest.y == 0);
    CHECK(legs == 1);

    // Disconnected discovery should not be considered reachable.
    Game g2;
    g2.newGame(1337u);
    GameTestAccess::discoverOverworldChunk(g2, 10, 10);
    GameTestAccess::setOverworldChunkFeatureFlags(g2, 10, 10, static_cast<uint8_t>(Game::OW_FEATURE_WAYSTATION));
    CHECK(!g2.overworldNearestLandmark(Game::OW_FEATURE_WAYSTATION, nearest, nullptr));

    return true;
}

bool test_overworld_auto_travel_pauses_and_resumes_in_ui() {
    Game g;
    g.newGame(9999u);

    // Discover a simple adjacent chunk and set it as a waypoint.
    GameTestAccess::discoverOverworldChunk(g, 1, 0);
    g.setOverworldWaypoint(Vec2i{1, 0});

    // Make the current camp level fully explored and walkable so the gate leg can be planned.
    Dungeon& d = GameTestAccess::dung(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
            d.at(x, y).explored = true;
        }
    }

    // Start overworld auto-travel. This should arm the multi-leg state and start the first in-chunk leg.
    g.requestOverworldAutoTravelToWaypoint();
    CHECK(g.overworldAutoTravelActive());
    CHECK(g.isAutoTraveling());

    // Open a UI overlay *without* going through handleAction() (simulates UI appearing mid-leg).
    // update() should stop in-chunk auto-move, but keep overworld auto-travel armed.
    g.openInventory();
    g.update(0.0f);
    CHECK(g.overworldAutoTravelActive());
    CHECK(!g.isAutoActive());

    // Close the overlay: overworld auto-travel should resume by replanning the leg.
    g.closeInventory();
    g.update(0.0f);
    CHECK(g.overworldAutoTravelActive());
    CHECK(g.isAutoTraveling());

    return true;
}

bool test_overworld_auto_travel_manual_pause() {
    Game g;
    g.newGame(424242u);

    // Discover a simple adjacent chunk and set it as a waypoint.
    GameTestAccess::discoverOverworldChunk(g, 1, 0);
    g.setOverworldWaypoint(Vec2i{1, 0});

    // Make the current camp level fully explored and walkable so the gate leg can be planned.
    Dungeon& d = GameTestAccess::dung(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
            d.at(x, y).explored = true;
        }
    }

    // Start overworld auto-travel.
    CHECK(g.requestOverworldAutoTravelToWaypoint());
    CHECK(g.overworldAutoTravelActive());
    CHECK(g.isAutoTraveling());
    CHECK(std::string(g.overworldAutoTravelLabel()) == "WAYPOINT");


    // Pause it via the new action: should stop in-chunk auto-walk but keep overworld travel armed.
    g.handleAction(Action::OverworldAutoTravelTogglePause);
    CHECK(g.overworldAutoTravelActive());
    CHECK(g.overworldAutoTravelPaused());
    CHECK(!g.isAutoActive());

    // update() should not resume while paused.
    g.update(0.0f);
    CHECK(g.overworldAutoTravelActive());
    CHECK(g.overworldAutoTravelPaused());
    CHECK(!g.isAutoActive());

    // Resume it: should re-kick the leg (or at least arm it so update continues).
    g.handleAction(Action::OverworldAutoTravelTogglePause);
    CHECK(g.overworldAutoTravelActive());
    CHECK(!g.overworldAutoTravelPaused());

    g.update(0.0f);
    CHECK(g.overworldAutoTravelActive());
    CHECK(g.isAutoTraveling());

    return true;
}

bool test_overworld_auto_travel_label() {
    Game g;
    g.newGame(777777u);

    // Discover a simple adjacent chunk.
    GameTestAccess::discoverOverworldChunk(g, 1, 0);

    // Make the current camp level fully explored and walkable so the gate leg can be planned.
    Dungeon& d = GameTestAccess::dung(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Floor;
            d.at(x, y).explored = true;
        }
    }

    // Start travel to an explicit destination label.
    CHECK(g.requestOverworldAutoTravelToChunk(Vec2i{1, 0}, "CURSOR"));
    CHECK(g.overworldAutoTravelActive());
    CHECK(std::string(g.overworldAutoTravelLabel()) == "CURSOR");

    // Cancel should reset label back to the default.
    g.cancelOverworldAutoTravel(true);
    CHECK(!g.overworldAutoTravelActive());
    CHECK(std::string(g.overworldAutoTravelLabel()) == "DESTINATION");

    return true;
}





bool test_auto_explore_frontier_levitation_crosses_chasm() {
    Game g;
    g.newGame(123456u);

    // Keep only the player entity so incidental procgen spawns can't interfere with pathing checks.
    const Entity p = g.player();
    g.ents.clear();
    g.ents.push_back(p);
    g.playerId_ = p.id;

    g.ground.clear();
    g.trapsCur.clear();

    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    Dungeon& d = GameTestAccess::dung(g);

    // Make the whole map a stable, explored wall field so only our constructed corridor matters.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            Tile& t = d.at(x, y);
            t.type = TileType::Wall;
            t.explored = true;
            t.visible = false;
        }
    }

    // Construct a minimal explored corridor where the ONLY way to reach the frontier is by crossing a chasm.
    const Vec2i start{5, 5};
    const Vec2i chasm{6, 5};
    const Vec2i frontier{7, 5};
    const Vec2i unknown{8, 5};

    d.at(start.x, start.y).type = TileType::Floor;
    d.at(chasm.x, chasm.y).type = TileType::Chasm;
    d.at(frontier.x, frontier.y).type = TileType::Floor;

    // Unexplored tile adjacent to the frontier (makes frontier a valid explore target).
    d.at(unknown.x, unknown.y).type = TileType::Floor;
    d.at(unknown.x, unknown.y).explored = false;

    // Ensure the corridor itself is explored so auto-explore considers it.
    d.at(start.x, start.y).explored = true;
    d.at(chasm.x, chasm.y).explored = true;
    d.at(frontier.x, frontier.y).explored = true;

    g.playerMut().pos = start;

    // Without levitation: frontier is unreachable (only path crosses chasm).
    g.playerMut().effects.levitationTurns = 0;
    const Vec2i f0 = g.findNearestExploreFrontier();
    CHECK(f0.x == -1 && f0.y == -1);

    // With levitation: chasm becomes traversable; frontier should be discovered.
    g.playerMut().effects.levitationTurns = 10;
    const Vec2i f1 = g.findNearestExploreFrontier();
    CHECK(f1.x == frontier.x);
    CHECK(f1.y == frontier.y);

    return true;
}



bool test_auto_explore_frontier_levitation_diagonal_chasm_corner() {
    Game g;
    g.newGame(123456u);

    // Keep only the player entity so incidental procgen spawns can't interfere with pathing checks.
    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;

    g.ground.clear();
    g.trapsCur.clear();

    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    Dungeon& d = GameTestAccess::dung(g);

    // Make the whole map a stable, explored wall field so only our constructed corridor matters.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            Tile& t = d.at(x, y);
            t.type = TileType::Wall;
            t.explored = true;
            t.visible = false;
        }
    }

    // Construct a minimal explored layout where the ONLY way to reach the frontier is a diagonal step
    // whose adjacent orthogonals are chasms. Those chasm tiles are marked as hazardous so the search
    // cannot step onto them (forcing the diagonal edge to be considered).
    const Vec2i start{5, 5};
    const Vec2i ch1{6, 5};
    const Vec2i ch2{5, 6};
    const Vec2i frontier{6, 6};
    const Vec2i unknown{7, 6};

    d.at(start.x, start.y).type = TileType::Floor;
    d.at(ch1.x, ch1.y).type = TileType::Chasm;
    d.at(ch2.x, ch2.y).type = TileType::Chasm;
    d.at(frontier.x, frontier.y).type = TileType::Floor;

    // Unexplored tile adjacent to the frontier (makes frontier a valid explore target).
    d.at(unknown.x, unknown.y).type = TileType::Floor;
    d.at(unknown.x, unknown.y).explored = false;

    // Ensure the relevant tiles are explored.
    d.at(start.x, start.y).explored = true;
    d.at(ch1.x, ch1.y).explored = true;
    d.at(ch2.x, ch2.y).explored = true;
    d.at(frontier.x, frontier.y).explored = true;

    // Block stepping onto the chasm orthogonals so the frontier is only reachable via the diagonal edge.
    const int W = d.width;
    const int i1 = ch1.x + ch1.y * W;
    const int i2 = ch2.x + ch2.y * W;
    if (i1 >= 0 && static_cast<size_t>(i1) < g.fireField_.size()) g.fireField_[static_cast<size_t>(i1)] = 10u;
    if (i2 >= 0 && static_cast<size_t>(i2) < g.fireField_.size()) g.fireField_[static_cast<size_t>(i2)] = 10u;

    g.playerMut().pos = start;

    // Without levitation: frontier is unreachable.
    g.playerMut().effects.levitationTurns = 0;
    const Vec2i f0 = g.findNearestExploreFrontier();
    CHECK(f0.x == -1 && f0.y == -1);

    // With levitation: chasm-adjacent diagonal should be allowed, making the frontier reachable.
    g.playerMut().effects.levitationTurns = 10;
    const Vec2i f1 = g.findNearestExploreFrontier();
    CHECK(f1.x == frontier.x);
    CHECK(f1.y == frontier.y);

    // And tryMove should allow the diagonal step across the chasm corner while levitating.
    const bool moved = g.tryMove(g.playerMut(), 1, 1);
    CHECK(moved);
    CHECK(g.player().pos.x == frontier.x);
    CHECK(g.player().pos.y == frontier.y);

    return true;
}


bool test_adhesive_fluid_seeds_from_fishable_water() {
    Game g;
    g.newGame(0xAD1501u);

    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;
    g.ground.clear();
    g.trapsCur.clear();

    Dungeon& d = GameTestAccess::dung(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            Tile& t = d.at(x, y);
            t.type = TileType::Floor;
            t.explored = true;
            t.visible = true;
        }
    }

    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    Vec2i water{d.width / 2, d.height / 2};
    if (water == d.stairsUp || water == d.stairsDown) water.x = std::max(1, water.x - 2);
    CHECK(d.inBounds(water.x, water.y));
    d.at(water.x, water.y).type = TileType::Fountain;

    Vec2i p{std::max(1, water.x - 3), water.y};
    if (!d.inBounds(p.x, p.y)) p = {1, 1};
    g.playerMut().pos = p;

    g.applyEndOfTurnEffects();

    int nonZero = 0;
    int nearWater = 0;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const size_t i = static_cast<size_t>(y * d.width + x);
            if (i >= g.adhesiveFluid_.size()) continue;
            if (g.adhesiveFluid_[i] == 0u) continue;
            ++nonZero;
            if (chebyshev(Vec2i{x, y}, water) <= 2) ++nearWater;
        }
    }

    CHECK(nonZero > 0);
    CHECK(nearWater > 0);
    return true;
}

bool test_adhesive_fluid_cohesive_movement() {
    Game g;
    g.newGame(0xAD1502u);

    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;
    g.ground.clear();
    g.trapsCur.clear();

    Dungeon& d = GameTestAccess::dung(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            Tile& t = d.at(x, y);
            t.type = TileType::Floor;
            t.explored = true;
            t.visible = true;
        }
    }

    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    Vec2i center{d.width / 2 - 2, d.height / 2};
    Vec2i water{center.x + 1, center.y};
    if (water == d.stairsUp || water == d.stairsDown) water = {center.x + 2, center.y};
    CHECK(d.inBounds(center.x, center.y));
    CHECK(d.inBounds(water.x, water.y));

    d.at(water.x, water.y).type = TileType::Fountain;

    const size_t iCenter = static_cast<size_t>(center.y * d.width + center.x);
    const size_t iRight = static_cast<size_t>(center.y * d.width + (center.x + 1));
    CHECK(iCenter < g.adhesiveFluid_.size());
    CHECK(iRight < g.adhesiveFluid_.size());

    g.adhesiveFluid_[iCenter] = 90u;
    g.adhesiveFluid_[iRight] = 42u;

    const std::vector<uint8_t> before = g.adhesiveFluid_;

    g.playerMut().pos = {std::max(1, center.x - 4), center.y};
    g.applyEndOfTurnEffects();

    const std::vector<uint8_t> after = g.adhesiveFluid_;
    CHECK(after.size() == before.size());

    bool gainedNewTile = false;
    bool hasAdjacentPair = false;
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const size_t i = static_cast<size_t>(y * d.width + x);
            if (i >= after.size()) continue;
            if (after[i] == 0u) continue;

            if (before[i] == 0u) gainedNewTile = true;

            constexpr Vec2i kDirs[4] = {{1,0},{-1,0},{0,1},{0,-1}};
            for (const Vec2i& dv : kDirs) {
                const int nx = x + dv.x;
                const int ny = y + dv.y;
                if (!d.inBounds(nx, ny)) continue;
                const size_t j = static_cast<size_t>(ny * d.width + nx);
                if (j < after.size() && after[j] > 0u) {
                    hasAdjacentPair = true;
                    break;
                }
            }
        }
    }

    const size_t iWater = static_cast<size_t>(water.y * d.width + water.x);
    CHECK(iWater < after.size());

    CHECK(gainedNewTile);
    CHECK(hasAdjacentPair);
    CHECK(after[iWater] > 0u);
    return true;
}

bool test_adhesive_fluid_does_not_perma_web_player() {
    Game g;
    g.newGame(0xAD1503u);

    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;
    g.ground.clear();
    g.trapsCur.clear();

    Dungeon& d = GameTestAccess::dung(g);
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            Tile& t = d.at(x, y);
            t.type = TileType::Floor;
            t.explored = true;
            t.visible = true;
        }
    }

    for (auto& v : g.confusionGas_) v = 0u;
    for (auto& v : g.poisonGas_) v = 0u;
    for (auto& v : g.corrosiveGas_) v = 0u;
    for (auto& v : g.fireField_) v = 0u;
    for (auto& v : g.adhesiveFluid_) v = 0u;

    const Vec2i start{d.width / 2, d.height / 2};
    CHECK(d.inBounds(start.x, start.y));
    CHECK(d.inBounds(start.x + 1, start.y));

    g.playerMut().pos = start;
    g.playerMut().effects.webTurns = 3;

    const size_t iStart = static_cast<size_t>(start.y * d.width + start.x);
    CHECK(iStart < g.adhesiveFluid_.size());
    g.adhesiveFluid_[iStart] = 220u;

    bool escaped = false;
    for (int i = 0; i < 10; ++i) {
        const Vec2i before = g.player().pos;
        const bool acted = g.tryMove(g.playerMut(), 1, 0);
        CHECK(acted);
        g.applyEndOfTurnEffects();
        if (g.player().pos != before) {
            escaped = true;
            break;
        }
    }

    CHECK(escaped);
    return true;
}



bool test_vault_prefab_catalog_valid() {
    size_t n = 0;
    const VaultPrefabDef* defs = vaultprefabs::catalog(n);
    CHECK(defs != nullptr);
    CHECK(n > 0);

    for (size_t i = 0; i < n; ++i) {
        std::string err;
        CHECK(vaultprefabs::validate(defs[i], err));
    }

    return true;
}

bool test_overworld_gate_alignment() {
    const uint32_t seed = 0xC0FFEEu;

    Dungeon d(64, 40);

    // Ensure gates line up across shared boundaries (east/west and north/south).
    const overworld::ChunkGates a = overworld::gatePositions(d, seed, 7, -3);
    const overworld::ChunkGates b = overworld::gatePositions(d, seed, 8, -3);
    CHECK(a.east.y == b.west.y);

    const overworld::ChunkGates c = overworld::gatePositions(d, seed, -2, 5);
    const overworld::ChunkGates d2 = overworld::gatePositions(d, seed, -2, 6);
    CHECK(c.south.x == d2.north.x);

    // Gates stay away from corners (so the carved "throat" is readable and in-bounds).
    CHECK(a.north.x >= 2);
    CHECK(a.north.x <= d.width - 3);
    CHECK(a.west.y >= 2);
    CHECK(a.west.y <= d.height - 3);

    // Home camp boundaries remain centered (preserves camp layout), and neighbors share that.
    const overworld::ChunkGates home = overworld::gatePositions(d, seed, 0, 0);
    CHECK(home.east.y == d.height / 2);
    CHECK(home.north.x == d.width / 2);

    const overworld::ChunkGates eastN = overworld::gatePositions(d, seed, 1, 0);
    CHECK(home.east.y == eastN.west.y);

    const overworld::ChunkGates southN = overworld::gatePositions(d, seed, 0, 1);
    CHECK(home.south.x == southN.north.x);

    // Integration: ensureBorderGates carves those exact tiles as walkable.
    overworld::ensureBorderWalls(d);
    overworld::ensureBorderGates(d, seed, 7, -3);
    const overworld::ChunkGates carved = overworld::gatePositions(d, seed, 7, -3);
    CHECK(d.isWalkable(carved.north.x, carved.north.y));
    CHECK(d.isWalkable(carved.north.x, carved.north.y + 1));
    CHECK(d.isWalkable(carved.south.x, carved.south.y));
    CHECK(d.isWalkable(carved.south.x, carved.south.y - 1));
    CHECK(d.isWalkable(carved.west.x, carved.west.y));
    CHECK(d.isWalkable(carved.west.x + 1, carved.west.y));
    CHECK(d.isWalkable(carved.east.x, carved.east.y));
    CHECK(d.isWalkable(carved.east.x - 1, carved.east.y));

    return true;
}



bool test_camp_stash_has_farming_starter_kit() {
    Game g;
    g.newGame(12345u);

    int stashIdx = -1;
    for (int i = 0; i < (int)g.chestContainers_.size(); ++i) {
        bool hasRod = false;
        bool hasKit = false;
        bool hasBounty = false;
        for (const auto& it : g.chestContainers_[i].items) {
            if (it.kind == ItemKind::FishingRod) hasRod = true;
            if (it.kind == ItemKind::CraftingKit) hasKit = true;
            if (it.kind == ItemKind::BountyContract) hasBounty = true;
        }
        if (hasRod && hasKit && hasBounty) {
            stashIdx = i;
            break;
        }
    }
    CHECK(stashIdx >= 0);

    bool hasHoe = false;
    bool hasSeeds = false;
    for (const auto& it : g.chestContainers_[stashIdx].items) {
        if (it.kind == ItemKind::GardenHoe) hasHoe = true;
        if (it.kind == ItemKind::Seed) hasSeeds = true;
    }

    CHECK(hasHoe);
    CHECK(hasSeeds);
    return true;
}

bool test_spellbook_study_and_reading_conduct() {
    Game g;
    g.newGame(0x5A17u);

    g.inv.clear();
    g.invSel = 0;
    g.knownSpellsMask_ = 0u;
    g.conductScrollsRead_ = 0u;
    g.conductSpellbooksRead_ = 0u;

    auto pushInv = [&](ItemKind k) {
        Item it;
        it.id = g.nextItemId++;
        it.kind = k;
        it.count = 1;
        it.spriteSeed = 0xABCD1234u;
        g.inv.push_back(it);
        g.invSel = static_cast<int>(g.inv.size()) - 1;
    };

    // First read learns the spell and grants mana insight.
    g.mana_ = 0;
    pushInv(ItemKind::SpellbookFireball);
    CHECK(g.useSelected());
    CHECK(g.knowsSpell(SpellKind::Fireball));
    CHECK(g.playerMana() > 0);
    CHECK(g.conductSpellbooksRead_ == 1u);
    CHECK(g.inv.empty());

    // Re-reading a known spellbook is still useful (mana insight) and counts.
    g.mana_ = 0;
    pushInv(ItemKind::SpellbookFireball);
    CHECK(g.useSelected());
    CHECK(g.knowsSpell(SpellKind::Fireball));
    CHECK(g.playerMana() > 0);
    CHECK(g.conductSpellbooksRead_ == 2u);
    CHECK(g.inv.empty());

    // Scroll reading conduct should increment independently.
    pushInv(ItemKind::ScrollMapping);
    CHECK(g.useSelected());
    CHECK(g.conductScrollsRead_ == 1u);

    return true;
}

bool test_spells_overlay_toggle_and_cast_minor_heal() {
    Game g;
    g.newGame(0x51E117u);

    // Keep simulation deterministic around the cast result.
    const Entity p0 = g.player();
    g.ents.clear();
    g.ents.push_back(p0);
    g.playerId_ = p0.id;

    g.knownSpellsMask_ = 0u;
    g.knownSpellsMask_ |= (1u << static_cast<uint32_t>(SpellKind::MinorHeal));
    g.spellsSel = 0;
    // Ensure this test exercises the successful cast path (not miscast RNG).
    g.talentFocus_ = 20;

    g.mana_ = g.playerManaMax();
    g.playerMut().hp = std::max(1, g.player().hpMax - 4);

    const int hpBefore = g.player().hp;
    const int manaBefore = g.playerMana();
    const uint32_t turnBefore = g.turns();

    g.handleAction(Action::Spells);
    CHECK(g.isSpellsOpen());

    g.handleAction(Action::Confirm);
    CHECK(!g.isSpellsOpen());
    CHECK(g.turns() == turnBefore + 1);
    CHECK(g.playerMana() < manaBefore);
    CHECK(g.player().hp >= hpBefore);

    // Hotkey should toggle the overlay back open.
    g.handleAction(Action::Spells);
    CHECK(g.isSpellsOpen());

    return true;
}

bool test_spells_hotkey_works_from_inventory_prompts() {
    Game g;
    g.newGame(0x51E118u);

    // Identify modal prompt should still allow switching to spells overlay.
    g.openInventory();
    g.invIdentifyMode = true;
    CHECK(g.isInventoryOpen());
    CHECK(g.invIdentifyMode);

    g.handleAction(Action::Spells);
    CHECK(g.isSpellsOpen());
    CHECK(!g.isInventoryOpen());
    CHECK(!g.invIdentifyMode);

    // Close spells, then verify crafting prompt behavior.
    g.handleAction(Action::Spells);
    CHECK(!g.isSpellsOpen());

    g.inv.clear();
    g.invSel = 0;
    Item kit;
    kit.id = g.nextItemId++;
    kit.kind = ItemKind::CraftingKit;
    kit.count = 1;
    g.inv.push_back(kit);

    Item rock;
    rock.id = g.nextItemId++;
    rock.kind = ItemKind::Rock;
    rock.count = 2;
    g.inv.push_back(rock);

    g.openInventory();
    g.beginCrafting();
    CHECK(g.isInventoryOpen());
    CHECK(g.isInventoryCraftMode());

    g.handleAction(Action::Spells);
    CHECK(g.isSpellsOpen());
    CHECK(!g.isInventoryOpen());
    CHECK(!g.isInventoryCraftMode());

    return true;
}

bool test_farming_till_plant_grow_harvest() {
    Game g;
    g.newGame(54321u);

    // Find an outdoor tile at the home camp suitable for farming.
    g.dung.ensureMaterials(g.materialWorldSeed(), g.branch_, g.materialDepth(), g.dungeonMaxDepth());

    Vec2i farmPos{-1, -1};
    for (int y = 1; y < g.dung.height - 1 && farmPos.x < 0; ++y) {
        for (int x = 1; x < g.dung.width - 1; ++x) {
            if (g.dung.at(x, y).type != TileType::Floor) continue;
            const TerrainMaterial m = g.dung.materialAtCached(x, y);
            if (m != TerrainMaterial::Dirt && m != TerrainMaterial::Moss) continue;

            bool blocked = false;
            for (const auto& gi : g.ground) {
                if (gi.pos == Vec2i{x, y} && (itemIsStationary(gi.item) || isStationaryPropKind(gi.item.kind))) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) continue;

            farmPos = Vec2i{x, y};
            break;
        }
    }
    CHECK(farmPos.x >= 0);

    g.playerMut().pos = farmPos;

    // Give the player a garden hoe.
    {
        Item hoe;
        hoe.id = g.nextItemId++;
        hoe.kind = ItemKind::GardenHoe;
        hoe.count = 1;
        hoe.spriteSeed = 0xABCDu;
        hoe.charges = itemDef(hoe.kind).maxCharges;
        g.inv.push_back(hoe);
        g.invSel = (int)g.inv.size() - 1;
    }

    CHECK(g.useGardenHoeAtPlayer(g.invSel));

    // Ensure tilled soil exists.
    int soilIdx = -1;
    for (int i = 0; i < (int)g.ground.size(); ++i) {
        if (g.ground[i].pos == farmPos && g.ground[i].item.kind == ItemKind::TilledSoil) {
            soilIdx = i;
            break;
        }
    }
    CHECK(soilIdx >= 0);

    // Plant seeds.
    const uint32_t cropSeed = 0xC0FFEEu;
    const farmgen::CropSpec cs = farmgen::makeCrop(cropSeed);

    Item seed;
    seed.id = g.nextItemId++;
    seed.kind = ItemKind::Seed;
    seed.count = 1;
    seed.spriteSeed = cropSeed;
    seed.charges = static_cast<int>(cropSeed);
    seed.enchant = packCropMetaEnchant(cs.variant, static_cast<int>(cs.rarity), cs.shiny);

    CHECK(g.plantSeedAtPlayer(seed));

    int cropIdx = -1;
    for (int i = 0; i < (int)g.ground.size(); ++i) {
        if (g.ground[i].pos == farmPos && isFarmPlantKind(g.ground[i].item.kind)) {
            cropIdx = i;
            break;
        }
    }
    CHECK(cropIdx >= 0);
    CHECK(g.ground[cropIdx].item.kind == ItemKind::CropSprout);

    const int plantedTurn = g.ground[cropIdx].item.charges;

    // Fast-forward time and refresh growth.
    g.turnCount = static_cast<uint32_t>(plantedTurn) + 999u;
    g.updateFarmGrowth();

    cropIdx = -1;
    for (int i = 0; i < (int)g.ground.size(); ++i) {
        if (g.ground[i].pos == farmPos && isFarmPlantKind(g.ground[i].item.kind)) {
            cropIdx = i;
            break;
        }
    }
    CHECK(cropIdx >= 0);
    CHECK(g.ground[cropIdx].item.kind == ItemKind::CropMature);

    CHECK(g.harvestFarmAtPlayer());

    bool hasProduce = false;
    for (const auto& it : g.inv) {
        if (it.kind == ItemKind::CropProduce) {
            hasProduce = true;
            break;
        }
    }
    CHECK(hasProduce);

    bool hasTilledAgain = false;
    for (const auto& gi : g.ground) {
        if (gi.pos == farmPos && gi.item.kind == ItemKind::TilledSoil) {
            hasTilledAgain = true;
            break;
        }
    }
    CHECK(hasTilledAgain);

    return true;
}

bool test_lifecycle_traits_are_deterministic() {
    Game g;
    g.newGame(0xA11CE551u);

    Entity a = g.makeMonster(EntityKind::Wolf, {2, 2}, 1, /*allowGear=*/false, 0x1234ABCDu, /*allowProcVariant=*/false);
    Entity b = g.makeMonster(EntityKind::Wolf, {2, 2}, 1, /*allowGear=*/false, 0x1234ABCDu, /*allowProcVariant=*/false);
    CHECK(a.lifeTraitMask == b.lifeTraitMask);
    CHECK(a.lifeSex == b.lifeSex);
    CHECK(a.lifeTraitMask != 0u);

    Entity sk = g.makeMonster(EntityKind::Shopkeeper, {3, 2}, 1, /*allowGear=*/false, 0x1234ABCDu, /*allowProcVariant=*/false);
    CHECK(sk.lifeTraitMask == 0u);
    CHECK(sk.lifeSex == LifeSex::Unknown);

    return true;
}

bool test_lifecycle_growth_progression() {
    Game g;
    g.newGame(0xA11CE552u);

    g.dung = Dungeon(16, 10);
    g.dung.rooms.clear();
    makeAllVisibleFloor(g.dung);
    g.playerMut().pos = {4, 5};

    Entity cub = g.makeMonster(EntityKind::Wolf, {6, 5}, 2, /*allowGear=*/false, 0x77889900u, /*allowProcVariant=*/false);
    cub.lifeStage = LifeStage::Newborn;
    cub.lifeAgeTurns = 0;
    cub.lifeStageTurns = lifecycleStageDurationTurns(LifeStage::Newborn) - 1;
    cub.lifeReproductionCooldown = 0;
    const int baseHp = cub.lifeBaseHpMax;

    g.ents.push_back(cub);
    const size_t idx = g.ents.size() - 1;

    g.tickLifeCycles();
    CHECK(g.ents[idx].lifeStage == LifeStage::Child);

    g.ents[idx].lifeStageTurns = lifecycleStageDurationTurns(LifeStage::Child) - 1;
    g.tickLifeCycles();
    CHECK(g.ents[idx].lifeStage == LifeStage::Adult);
    CHECK(g.ents[idx].hpMax == baseHp);

    return true;
}

bool test_lifecycle_reproduction_spawns_newborn() {
    Game g;
    g.newGame(0xA11CE553u);

    g.dung = Dungeon(20, 12);
    g.dung.rooms.clear();
    makeAllVisibleFloor(g.dung);
    g.playerMut().pos = {3, 6};

    const int matureAge = lifecycleStageDurationTurns(LifeStage::Newborn) +
                          lifecycleStageDurationTurns(LifeStage::Child);

    Entity a = g.makeMonster(EntityKind::Wolf, {8, 6}, 7, /*allowGear=*/false, 0xAAAABBBB, /*allowProcVariant=*/false);
    Entity b = g.makeMonster(EntityKind::Wolf, {9, 6}, 7, /*allowGear=*/false, 0xCCCCDDDD, /*allowProcVariant=*/false);

    a.lifeStage = LifeStage::Adult;
    b.lifeStage = LifeStage::Adult;
    a.lifeAgeTurns = matureAge + 80;
    b.lifeAgeTurns = matureAge + 80;
    a.lifeStageTurns = 120;
    b.lifeStageTurns = 120;
    a.lifeReproductionCooldown = 0;
    b.lifeReproductionCooldown = 0;
    a.hp = a.hpMax;
    b.hp = b.hpMax;
    if (a.lifeSex == b.lifeSex) {
        b.lifeSex = (a.lifeSex == LifeSex::Male) ? LifeSex::Female : LifeSex::Male;
    }

    g.ents.push_back(a);
    g.ents.push_back(b);
    const int parentAId = a.id;
    const int parentBId = b.id;

    const size_t before = g.ents.size();
    g.tickLifeCycles();
    CHECK(g.ents.size() == before + 1);

    bool foundNewborn = false;
    for (const auto& e : g.ents) {
        if (e.id == parentAId || e.id == parentBId) continue;
        if (e.kind != EntityKind::Wolf) continue;
        if (e.lifeStage != LifeStage::Newborn) continue;
        foundNewborn = true;
        CHECK(e.lifeTraitMask != 0u);
        break;
    }
    CHECK(foundNewborn);

    for (auto& e : g.ents) {
        if (e.id == parentAId || e.id == parentBId) {
            CHECK(e.lifeReproductionCooldown > 0);
        }
    }

    return true;
}

bool spritePixelsEqual(const SpritePixels& a, const SpritePixels& b) {
    if (a.w != b.w || a.h != b.h) return false;
    if (a.px.size() != b.px.size()) return false;
    for (size_t i = 0; i < a.px.size(); ++i) {
        const Color& ca = a.px[i];
        const Color& cb = b.px[i];
        if (ca.r != cb.r || ca.g != cb.g || ca.b != cb.b || ca.a != cb.a) {
            return false;
        }
    }
    return true;
}

int spritePixelDiffCount(const SpritePixels& a, const SpritePixels& b) {
    if (a.w != b.w || a.h != b.h || a.px.size() != b.px.size()) return -1;
    int diff = 0;
    for (size_t i = 0; i < a.px.size(); ++i) {
        const Color& ca = a.px[i];
        const Color& cb = b.px[i];
        if (ca.r != cb.r || ca.g != cb.g || ca.b != cb.b || ca.a != cb.a) {
            ++diff;
        }
    }
    return diff;
}

bool test_spritegen_floor_determinism_and_4frame_loop() {
    const uint32_t seed = 0x5EEDBEEFu;
    const uint8_t style = 0;

    const SpritePixels floor0 = generateThemedFloorTile(seed, style, 0, 16);
    const SpritePixels floor0b = generateThemedFloorTile(seed, style, 0, 16);
    const SpritePixels floor4 = generateThemedFloorTile(seed, style, 4, 16);

    CHECK(floor0.w == 16 && floor0.h == 16);
    CHECK(spritePixelsEqual(floor0, floor0b));
    CHECK(spritePixelsEqual(floor0, floor4));

    const SpritePixels iso0 = generateIsometricThemedFloorTile(seed, style, 0, 16);
    const SpritePixels iso0b = generateIsometricThemedFloorTile(seed, style, 0, 16);
    const SpritePixels iso4 = generateIsometricThemedFloorTile(seed, style, 4, 16);

    CHECK(iso0.w == 16 && iso0.h == 8);
    CHECK(spritePixelsEqual(iso0, iso0b));
    CHECK(spritePixelsEqual(iso0, iso4));
    CHECK(iso0.at(0, 0).a == 0);   // outside the diamond
    CHECK(iso0.at(8, 3).a > 0);    // center diamond pixel

    return true;
}

bool test_spritegen_floor_animation_varies_across_frames() {
    const uint32_t seed = 0xA11CE999u;
    const uint8_t style = 5; // no sparkle branch; movement should come from floor animation.

    const SpritePixels floor0 = generateThemedFloorTile(seed, style, 0, 16);
    const SpritePixels floor2 = generateThemedFloorTile(seed, style, 2, 16);
    const int floorDiff = spritePixelDiffCount(floor0, floor2);
    CHECK(floorDiff > 0);

    const SpritePixels iso0 = generateIsometricThemedFloorTile(seed, style, 0, 16);
    const SpritePixels iso2 = generateIsometricThemedFloorTile(seed, style, 2, 16);
    const int isoDiff = spritePixelDiffCount(iso0, iso2);
    CHECK(isoDiff > 0);

    return true;
}


bool test_spritegen_scale3x_edge_rules() {
    // This test exercises the optimized Scale3x rules (as used by resampleSpriteToSize)
    // on a small, deterministic neighborhood that should expand a single corner.
    SpritePixels src;
    src.w = 16;
    src.h = 16;

    const Color W{255, 255, 255, 255};
    const Color B{0, 0, 0, 255};

    src.px.assign(static_cast<size_t>(src.w * src.h), W);

    // Neighborhood around E at (8,8):
    // A B C
    // D E F
    // G H I
    // Choose values so (B != H && D != F) and D==B, E!=C, E!=G to trigger:
    // E0/E1/E3 = black, rest = white.
    src.at(8, 8) = W; // E
    src.at(8, 7) = B; // B
    src.at(9, 7) = B; // C
    src.at(7, 8) = B; // D
    src.at(7, 9) = B; // G
    src.at(9, 8) = W; // F
    src.at(8, 9) = W; // H

    SpritePixels out = resampleSpriteToSize(src, 48);
    CHECK(out.w == 48);
    CHECK(out.h == 48);

    auto eq = [&](const Color& a, const Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    // The 3x block for source pixel (8,8) starts at (24,24) in the output.
    CHECK(eq(out.at(24, 24), B)); // E0
    CHECK(eq(out.at(25, 24), B)); // E1
    CHECK(eq(out.at(26, 24), W)); // E2
    CHECK(eq(out.at(24, 25), B)); // E3
    CHECK(eq(out.at(25, 25), W)); // E4
    CHECK(eq(out.at(26, 25), W)); // E5
    CHECK(eq(out.at(24, 26), W)); // E6
    CHECK(eq(out.at(25, 26), W)); // E7
    CHECK(eq(out.at(26, 26), W)); // E8

    return true;
}

bool test_spritegen_scale2x_transparent_rgb_invariant_corner_rounding() {
    // Scale2x rounding should not depend on RGB values stored in fully transparent
    // pixels (common when sprites are procedurally generated).
    SpritePixels clean;
    clean.w = 16;
    clean.h = 16;
    clean.px.assign(static_cast<size_t>(clean.w * clean.h), {0, 0, 0, 0});

    // 2x2 opaque block at (8,8). The top-left corner pixel (8,8) has B and D as
    // background; Scale2x uses D==B to decide corner rounding.
    const Color O{0, 0, 0, 255};
    clean.at(8, 8) = O;
    clean.at(9, 8) = O;
    clean.at(8, 9) = O;
    clean.at(9, 9) = O;

    SpritePixels noisy = clean;
    // Inject RGB noise into fully transparent background neighbors B and D.
    // B = (8,7), D = (7,8)
    noisy.at(8, 7) = {10, 20, 30, 0};
    noisy.at(7, 8) = {40, 50, 60, 0};

    const SpritePixels outClean = resampleSpriteToSize(clean, 32);
    const SpritePixels outNoisy = resampleSpriteToSize(noisy, 32);
    CHECK(outClean.w == 32 && outClean.h == 32);
    CHECK(outNoisy.w == 32 && outNoisy.h == 32);

    // The top-left quadrant of the 2x2 output block for source E=(8,8) starts at
    // (16,16). It should be background (transparent) in both cases.
    CHECK(outClean.at(16, 16).a == 0);
    CHECK(outNoisy.at(16, 16).a == 0);

    // Entire output should match (including transparent pixels).
    CHECK(outClean.px.size() == outNoisy.px.size());
    for (size_t i = 0; i < outClean.px.size(); ++i) {
        const Color& a = outClean.px[i];
        const Color& b = outNoisy.px[i];
        if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
            const int x = static_cast<int>(i % static_cast<size_t>(outClean.w));
            const int y = static_cast<int>(i / static_cast<size_t>(outClean.w));
            CHECK_MSG(false, "pixel mismatch at (" << x << "," << y
                << "), clean=" << colorToString(a)
                << ", noisy=" << colorToString(b));
        }
    }
    return true;
}

bool test_spritegen_resample_nearest_center_mapping() {
    // The nearest resampler uses pixel-center sampling to avoid bias when scaling
    // by non-integer factors. A simple gradient lets us validate the mapping.
    SpritePixels src;
    src.w = 16;
    src.h = 16;
    src.px.assign(static_cast<size_t>(src.w * src.h), {0, 0, 0, 255});

    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            src.at(x, y) = {static_cast<uint8_t>(x), 0, 0, 255};
        }
    }

    const SpritePixels out = resampleSpriteToSize(src, 24);
    CHECK(out.w == 24 && out.h == 24);

    // For 16 -> 24, pixel-center mapping yields:
    // x=0->0, x=1->1, x=2->1, x=3->2, ...
    CHECK(out.at(0, 0).r == 0);
    CHECK(out.at(1, 0).r == 1);
    CHECK(out.at(2, 0).r == 1);
    CHECK(out.at(3, 0).r == 2);
    CHECK(out.at(23, 0).r == 15);

    return true;
}

bool test_spritegen_resample_factor_6_matches_chain() {
    // Ensure the mixed 2x/3x integer scaling path is used (and stable):
    // resample(16->96) should match resample(resample(16->32)->96).
    SpritePixels src;
    src.w = 16;
    src.h = 16;

    const Color W{255, 255, 255, 255};
    const Color B{0, 0, 0, 255};

    src.px.assign(static_cast<size_t>(src.w * src.h), W);

    // Simple diagonal-ish mark to give the scaler something edgey.
    for (int i = 2; i <= 13; ++i) {
        src.at(i, i) = B;
    }

    const SpritePixels direct = resampleSpriteToSize(src, 96);
    const SpritePixels step32 = resampleSpriteToSize(src, 32);
    const SpritePixels chained = resampleSpriteToSize(step32, 96);

    CHECK(direct.w == 96 && direct.h == 96);
    CHECK(chained.w == 96 && chained.h == 96);
    CHECK(direct.px.size() == chained.px.size());

    for (size_t i = 0; i < direct.px.size(); ++i) {
        const Color& a = direct.px[i];
        const Color& b = chained.px[i];
        if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
            const int x = static_cast<int>(i % static_cast<size_t>(direct.w));
            const int y = static_cast<int>(i / static_cast<size_t>(direct.w));
            CHECK_MSG(false, "pixel mismatch at (" << x << "," << y
                << "), direct=" << colorToString(a)
                << ", chained=" << colorToString(b));
        }
    }

    return true;
}

bool test_spritegen_resample_rect_scale3x_edge_rules() {
    // Verify that the non-square resampler uses the Scale3x rules when the
    // requested size is an exact 3x multiple (e.g. 16x8 -> 48x24).
    SpritePixels src;
    src.w = 16;
    src.h = 8;

    const Color W{255, 255, 255, 255};
    const Color B{0, 0, 0, 255};

    src.px.assign(static_cast<size_t>(src.w * src.h), W);

    // Neighborhood around E at (8,4):
    // A B C
    // D E F
    // G H I
    // Choose values so (B != H && D != F) and D==B, E!=C, E!=G to trigger:
    // E0/E1/E3 = black, rest = white.
    src.at(8, 4) = W; // E
    src.at(8, 3) = B; // B
    src.at(9, 3) = B; // C
    src.at(7, 4) = B; // D
    src.at(7, 5) = B; // G
    src.at(9, 4) = W; // F
    src.at(8, 5) = W; // H

    SpritePixels out = resampleSpriteToRect(src, 48, 24);
    CHECK(out.w == 48);
    CHECK(out.h == 24);

    auto eq = [&](const Color& a, const Color& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };

    // The 3x block for source pixel (8,4) starts at (24,12) in the output.
    CHECK(eq(out.at(24, 12), B)); // E0
    CHECK(eq(out.at(25, 12), B)); // E1
    CHECK(eq(out.at(26, 12), W)); // E2
    CHECK(eq(out.at(24, 13), B)); // E3
    CHECK(eq(out.at(25, 13), W)); // E4
    CHECK(eq(out.at(26, 13), W)); // E5
    CHECK(eq(out.at(24, 14), W)); // E6
    CHECK(eq(out.at(25, 14), W)); // E7
    CHECK(eq(out.at(26, 14), W)); // E8

    return true;
}

bool test_spritegen_resample_rect_factor_6_matches_chain() {
    // Ensure the mixed 2x/3x integer scaling path is used (and stable) for
    // non-square sprites:
    // resample(16x8->96x48) should match resample(resample(16x8->32x16)->96x48).
    SpritePixels src;
    src.w = 16;
    src.h = 8;

    const Color W{255, 255, 255, 255};
    const Color B{0, 0, 0, 255};

    src.px.assign(static_cast<size_t>(src.w * src.h), W);

    // Add a slanted stroke so the scaler's edge rules matter.
    for (int y = 1; y < src.h - 1; ++y) {
        const int x = 2 + y * 2; // gentle slope across the 16-wide sprite
        if (x >= 0 && x < src.w) src.at(x, y) = B;
    }

    const SpritePixels direct = resampleSpriteToRect(src, 96, 48);
    const SpritePixels step32 = resampleSpriteToRect(src, 32, 16);
    const SpritePixels chained = resampleSpriteToRect(step32, 96, 48);

    CHECK(direct.w == 96 && direct.h == 48);
    CHECK(chained.w == 96 && chained.h == 48);
    CHECK(direct.px.size() == chained.px.size());

    for (size_t i = 0; i < direct.px.size(); ++i) {
        const Color& a = direct.px[i];
        const Color& b = chained.px[i];
        if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
            const int x = static_cast<int>(i % static_cast<size_t>(direct.w));
            const int y = static_cast<int>(i / static_cast<size_t>(direct.w));
            CHECK_MSG(false, "pixel mismatch at (" << x << "," << y
                << "), direct=" << colorToString(a)
                << ", chained=" << colorToString(b));
        }
    }

    return true;
}

int main(int argc, char** argv) {
    std::vector<TestCase> tests = {
        {"new_game_determinism", test_new_game_determinism},
        {"scent_field_wind_bias", test_scent_field_wind_bias},
        {"ecosystem_stealth_fx", test_ecosystem_stealth_fx_sanity},
        {"ecosystem_weapon_ego_loot_bias", test_ecosystem_weapon_ego_loot_bias},
        {"proc_leylines",        test_proc_leylines_basic},
        {"wfc_solver_basic",     test_wfc_solver_basic},
        {"wfc_solver_unsat",     test_wfc_solver_unsat_forced_contradiction},
        {"save_load_roundtrip",  test_save_load_roundtrip},
        {"save_load_overworld_chunk",  test_save_load_roundtrip_overworld_chunk},
        {"save_load_home_camp",      test_save_load_roundtrip_home_camp},
        {"save_load_sneak",      test_save_load_preserves_sneak},
        {"settings_minimap_zoom", test_settings_minimap_zoom_clamp},
        {"action_palette",  test_action_palette_executes_actions},
        {"action_info_view_turn", test_action_info_view_turn_tokens},
        {"replay_record_cmd", test_extended_command_replay_record_requests},
        {"noise_localization",  test_noise_localization_determinism},
        {"proc_spells",         test_proc_spell_generation_determinism},
        {"rune_wards",         test_rune_ward_words},
        {"proc_monster_codename", test_proc_monster_codename_determinism},
        {"crafting_procgen",  test_crafting_procedural_determinism},
        {"crafting_ecosystem_catalysts", test_crafting_ecosystem_catalyst_changes_outcome},
        {"crafting_shard_refinement", test_crafting_shard_refinement_location_invariant},
        {"crafting_shard_infusion", test_crafting_shard_infusion_upgrades_gear},
        {"crafting_same_stack", test_crafting_same_stack_consumes_two_units},
        {"crafting_blocks_cursed_equipped", test_crafting_blocks_cursed_equipped_ingredients},
        {"crafting_auto_unequip", test_crafting_auto_unequips_consumed_gear},
        {"trap_salvage_procgen", test_trap_salvage_procgen_determinism},
        {"graffiti_procgen", test_graffiti_procgen_determinism},
        {"sigil_procgen",    test_sigil_procgen_determinism},
        {"overworld_profiles", test_overworld_profiles},
        {"overworld_biome_diversity", test_overworld_biome_diversity},
        {"overworld_tectonics", test_overworld_tectonic_ridge_field_extrema},
        {"wet_lab_chemical_hazards", test_wet_laboratory_chemical_hazards_generate_adhesive_fields},
        {"overworld_chunk",    test_overworld_chunk_determinism},
        {"overworld_trails",  test_overworld_trails_connectivity},
        {"overworld_rivers",  test_overworld_rivers_and_banks_present},
        {"overworld_springs", test_overworld_springs_present},
        {"dungeon_seep_springs", test_dungeon_seep_springs_present},
        {"dungeon_fishing_water", test_dungeon_levels_have_fishing_water},
        {"fishing_target_chasm", test_fishing_targeting_accepts_chasm_tiles},
        {"fishing_target_no_water", test_fishing_targeting_no_visible_water_feedback},
        {"inventory_fishing_crafting_context_use", test_inventory_context_use_opens_fishing_and_crafting_tools},
        {"overworld_brooks",  test_overworld_brooks_present},
        {"overworld_strongholds", test_overworld_strongholds_present},
        {"overworld_weather",  test_overworld_weather_determinism},
        {"overworld_weather_time", test_overworld_weather_time_varying_fronts},
        {"overworld_atlas_discovery", test_overworld_atlas_discovery_tracking},
        {"overworld_atlas_features_save_load", test_overworld_atlas_feature_flags_save_load},
        {"overworld_atlas_terrain_save_load", test_overworld_atlas_terrain_save_load},
        {"overworld_atlas_waypoint_save_load", test_overworld_atlas_waypoint_save_load},
        {"overworld_atlas_route_preview", test_overworld_atlas_route_preview},
        {"overworld_atlas_nearest_landmark", test_overworld_atlas_nearest_landmark},
        {"overworld_auto_travel_pause_resume", test_overworld_auto_travel_pauses_and_resumes_in_ui},
        {"overworld_auto_travel_manual_pause", test_overworld_auto_travel_manual_pause},
        {"overworld_auto_travel_label", test_overworld_auto_travel_label},
        {"auto_explore_levitation_chasm", test_auto_explore_frontier_levitation_crosses_chasm},
        {"auto_explore_levitation_chasm_diagonal", test_auto_explore_frontier_levitation_diagonal_chasm_corner},
        {"adhesive_fluid_seed", test_adhesive_fluid_seeds_from_fishable_water},
        {"adhesive_fluid_escape", test_adhesive_fluid_does_not_perma_web_player},
        {"adhesive_fluid_cohesion", test_adhesive_fluid_cohesive_movement},
        {"overworld_gate_alignment", test_overworld_gate_alignment},
        {"ident_appearances", test_ident_appearance_procgen_determinism},
        {"spritegen_floor_determinism", test_spritegen_floor_determinism_and_4frame_loop},
        {"spritegen_floor_animation", test_spritegen_floor_animation_varies_across_frames},
        {"spritegen_scale3x_rules", test_spritegen_scale3x_edge_rules},
        {"spritegen_scale2x_transparent_rgb", test_spritegen_scale2x_transparent_rgb_invariant_corner_rounding},
        {"spritegen_nearest_center_mapping", test_spritegen_resample_nearest_center_mapping},
        {"spritegen_resample_factor_6", test_spritegen_resample_factor_6_matches_chain},
        {"spritegen_resample_rect_scale3x_rules", test_spritegen_resample_rect_scale3x_edge_rules},
        {"spritegen_resample_rect_factor_6", test_spritegen_resample_rect_factor_6_matches_chain},
        {"shop_profiles",   test_proc_shop_profiles},
        {"shopkeeper_look_name", test_shopkeeper_look_shows_deterministic_name},
        {"shopkeeper_target_warning_name", test_targeting_warning_includes_shopkeeper_name},
        {"shop_pay_requires_local_keeper", test_pay_at_shop_requires_local_shopkeeper},
        {"shopkeeper_death_clears_depth_ledger", test_shopkeeper_death_clears_depth_debt_ledger},
        {"shrine_profiles", test_proc_shrine_profiles},
        {"camp_stash_farming_starter_kit", test_camp_stash_has_farming_starter_kit},
        {"spellbook_study_and_reading_conduct", test_spellbook_study_and_reading_conduct},
        {"spells_overlay_cast", test_spells_overlay_toggle_and_cast_minor_heal},
        {"spells_hotkey_inventory_prompt", test_spells_hotkey_works_from_inventory_prompts},
        {"farming_till_plant_grow_harvest", test_farming_till_plant_grow_harvest},
        {"lifecycle_traits_determinism", test_lifecycle_traits_are_deterministic},
        {"lifecycle_growth", test_lifecycle_growth_progression},
        {"lifecycle_reproduction", test_lifecycle_reproduction_spawns_newborn},
        {"victory_plan",   test_victory_plan_procgen_determinism},
    };

    bool list = false;
    std::string filter;
    std::string exact;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a.rfind("--filter=", 0) == 0) {
            filter = a.substr(std::string("--filter=").size());
        } else if (a.rfind("--exact=", 0) == 0) {
            exact = a.substr(std::string("--exact=").size());
        } else if (a.rfind("--test=", 0) == 0) {
            exact = a.substr(std::string("--test=").size());
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "Unknown option: " << a << "\n";
            return 2;
        } else {
            // Convenience: allow passing a bare substring as a filter.
            filter = a;
        }
    }

    if (list) {
        for (const auto& t : tests) {
            std::cout << t.name << "\n";
        }
        return 0;
    }

    int passed = 0;
    int failed = 0;
    bool ranAny = false;
    std::vector<std::string> failedTests;

    for (const auto& t : tests) {
        if (!exact.empty()) {
            if (t.name != exact) continue;
        } else if (!containsCaseInsensitive(t.name, filter)) {
            continue;
        }
        ranAny = true;

        setCurrentTestName(t.name);
        bool ok = false;
        try {
            ok = t.fn();
        } catch (const std::exception& ex) {
            gFailureLogged = true;
            std::cerr << "[FAIL] " << t.name << " threw std::exception: " << ex.what() << "\n";
            ok = false;
        } catch (...) {
            gFailureLogged = true;
            std::cerr << "[FAIL] " << t.name << " threw unknown exception\n";
            ok = false;
        }
        if (ok) {
            ++passed;
        } else {
            if (!gFailureLogged) {
                std::cerr << "[FAIL] " << t.name
                          << " returned false without CHECK context\n";
            }
            ++failed;
            failedTests.emplace_back(t.name);
        }
    }

    if (!ranAny) {
        if (!exact.empty()) {
            std::cerr << "[FAIL] no test matched --exact=" << exact << "\n";
        } else {
            std::cerr << "[FAIL] no tests matched filter '" << filter << "'\n";
        }
        return 2;
    }

    if (!failedTests.empty()) {
        std::cerr << "[FAIL] Failed tests:";
        for (const std::string& name : failedTests) {
            std::cerr << " " << name;
        }
        std::cerr << "\n";
    }

    std::cout << "Tests: " << passed << " passed, " << failed << " failed." << std::endl;
    return failed == 0 ? 0 : 1;
}
