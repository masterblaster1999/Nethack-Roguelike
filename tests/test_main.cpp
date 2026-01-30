#include "game.hpp"
#include "action_info.hpp"
#include "overworld.hpp"
#include "settings.hpp"
#include "scent_field.hpp"
#include "wfc.hpp"
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
#include "ecosystem_loot.hpp"
#include "shrine_profile_gen.hpp"
#include "victory_gen.hpp"
#include <queue>


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
};

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path testTempDir() {
    // Prefer a real temp directory but fall back to the current directory if
    // the platform doesn't provide one (or it throws).
    fs::path base;
    try {
        base = fs::temp_directory_path();
    } catch (...) {
        base = fs::current_path();
    }

    fs::path dir = base / "procrogue_tests";
    std::error_code ec;
    fs::create_directories(dir, ec);
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

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "[FAIL] " << __FUNCTION__ << ":" << __LINE__ << " CHECK(" #cond ")\n"; \
        return false; \
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

    // Changing monsterId must affect the hash (even if the offset can coincide by chance).
    const uint32_t hOther = noiseInvestigateHash(seed, turn, 2, src, volume, eff, dist);
    CHECK(hOther != h1);

    // Loud sounds should be treated as precise (radius 0).
    CHECK(noiseInvestigateRadius(18, 18, 10) == 0);

    // Nearby sounds should also be precise (radius 0).
    CHECK(noiseInvestigateRadius(6, 6, 2) == 0);

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

    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            if (!d.isWalkable(x, y)) continue;
            Vec2i p{x, y};
            if (roomTypeAtLocal(p) != RoomType::Normal) continue; // keep workstation constant
            const EcosystemKind e = d.ecosystemAtCached(x, y);
            if (e == EcosystemKind::None && posNone.x < 0) posNone = p;
            if (e != EcosystemKind::None && posEco.x < 0) { posEco = p; ecoKind = e; }
            if (posNone.x >= 0 && posEco.x >= 0) break;
        }
        if (posNone.x >= 0 && posEco.x >= 0) break;
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

    Room vault; vault.x = 2; vault.y = 2; vault.w = 6; vault.h = 6; vault.type = RoomType::Vault;
    Room shrine; shrine.x = 12; shrine.y = 2; shrine.w = 6; shrine.h = 6; shrine.type = RoomType::Shrine;
    Room shop; shop.x = 12; shop.y = 12; shop.w = 6; shop.h = 6; shop.type = RoomType::Shop;
    d.rooms.push_back(vault);
    d.rooms.push_back(shrine);
    d.rooms.push_back(shop);

    const std::vector<graffitigen::Hint> hints = graffitigen::collectHints(d);
    CHECK(!hints.empty());

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

int main(int argc, char** argv) {
    std::vector<TestCase> tests = {
        {"new_game_determinism", test_new_game_determinism},
        {"scent_field_wind_bias", test_scent_field_wind_bias},
        {"ecosystem_stealth_fx", test_ecosystem_stealth_fx_sanity},
        {"proc_leylines",        test_proc_leylines_basic},
        {"wfc_solver_basic",     test_wfc_solver_basic},
        {"wfc_solver_unsat",     test_wfc_solver_unsat_forced_contradiction},
        {"save_load_roundtrip",  test_save_load_roundtrip},
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
        {"trap_salvage_procgen", test_trap_salvage_procgen_determinism},
        {"graffiti_procgen", test_graffiti_procgen_determinism},
        {"sigil_procgen",    test_sigil_procgen_determinism},
        {"overworld_profiles", test_overworld_profiles},
        {"overworld_biome_diversity", test_overworld_biome_diversity},
        {"overworld_tectonics", test_overworld_tectonic_ridge_field_extrema},
        {"overworld_chunk",    test_overworld_chunk_determinism},
        {"overworld_trails",  test_overworld_trails_connectivity},
        {"overworld_rivers",  test_overworld_rivers_and_banks_present},
        {"overworld_springs", test_overworld_springs_present},
        {"overworld_brooks",  test_overworld_brooks_present},
        {"overworld_strongholds", test_overworld_strongholds_present},
        {"overworld_weather",  test_overworld_weather_determinism},
        {"overworld_weather_time", test_overworld_weather_time_varying_fronts},
        {"overworld_atlas_discovery", test_overworld_atlas_discovery_tracking},
        {"overworld_gate_alignment", test_overworld_gate_alignment},
        {"ident_appearances", test_ident_appearance_procgen_determinism},
        {"shop_profiles",   test_proc_shop_profiles},
        {"shrine_profiles", test_proc_shrine_profiles},
        {"victory_plan",   test_victory_plan_procgen_determinism},
    };

    bool list = false;
    std::string filter;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a.rfind("--filter=", 0) == 0) {
            filter = a.substr(std::string("--filter=").size());
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

    for (const auto& t : tests) {
        if (!containsCaseInsensitive(t.name, filter)) continue;

        const bool ok = t.fn();
        if (ok) {
            ++passed;
        } else {
            ++failed;
        }
    }

    std::cout << "Tests: " << passed << " passed, " << failed << " failed." << std::endl;
    return failed == 0 ? 0 : 1;
}