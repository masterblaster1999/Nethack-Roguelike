#include "game.hpp"
#include "settings.hpp"
#include "scent_field.hpp"
#include "wfc.hpp"

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


struct TestCase {
    const char* name;
    bool (*fn)();
};

} // namespace

int main(int argc, char** argv) {
    std::vector<TestCase> tests = {
        {"new_game_determinism", test_new_game_determinism},
        {"scent_field_wind_bias", test_scent_field_wind_bias},
        {"wfc_solver_basic",     test_wfc_solver_basic},
        {"wfc_solver_unsat",     test_wfc_solver_unsat_forced_contradiction},
        {"save_load_roundtrip",  test_save_load_roundtrip},
        {"save_load_sneak",      test_save_load_preserves_sneak},
        {"settings_minimap_zoom", test_settings_minimap_zoom_clamp},
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
