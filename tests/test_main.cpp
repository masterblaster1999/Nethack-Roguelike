#include "game.hpp"

#include <cctype>
#include <filesystem>
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

    Game g2;
    CHECK(g2.loadFromFile(p.string()));

    const uint64_t h2 = g2.determinismHash();
    CHECK(h1 == h2);
    CHECK(g2.turns() == turnsBefore);

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
        {"save_load_roundtrip",  test_save_load_roundtrip},
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
