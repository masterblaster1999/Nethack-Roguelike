#include "dungeon.hpp"
#include "items.hpp"
#include "rng.hpp"
#include "scores.hpp"

#include <cstdint>
#include <fstream>
#if __has_include(<filesystem>)
#include <filesystem>
#endif
#include <iostream>
#include <queue>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool cond, const std::string& msg) {
    if (!cond) {
        ++failures;
        std::cerr << "[FAIL] " << msg << "\n";
    }
}

void test_rng_reproducible() {
    RNG rng(123u);
    const std::vector<uint32_t> expected = {
        31682556u,
        4018661298u,
        2101636938u,
        3842487452u,
        1628673942u,
    };

    for (size_t i = 0; i < expected.size(); ++i) {
        const uint32_t v = rng.nextU32();
        expect(v == expected[i], "RNG sequence mismatch at index " + std::to_string(i));
    }

    // Also validate range() stays within bounds.
    for (int i = 0; i < 1000; ++i) {
        int r = rng.range(-3, 7);
        expect(r >= -3 && r <= 7, "RNG range() out of bounds");
    }
}

void test_dungeon_stairs_connected() {
    RNG rng(42u);
    Dungeon d(30, 20);
    d.generate(rng);

    auto inBounds = [&](Vec2i p) {
        return d.inBounds(p.x, p.y);
    };

    expect(inBounds(d.stairsUp), "stairsUp out of bounds");
    expect(inBounds(d.stairsDown), "stairsDown out of bounds");

    if (inBounds(d.stairsUp)) {
        expect(d.at(d.stairsUp.x, d.stairsUp.y).type == TileType::StairsUp,
               "stairsUp tile type incorrect");
    }
    if (inBounds(d.stairsDown)) {
        expect(d.at(d.stairsDown.x, d.stairsDown.y).type == TileType::StairsDown,
               "stairsDown tile type incorrect");
    }

    // BFS from up to down using passable tiles.
    std::vector<uint8_t> visited(static_cast<size_t>(d.width * d.height), 0);
    auto idx = [&](int x, int y) { return static_cast<size_t>(y * d.width + x); };

    std::queue<Vec2i> q;
    if (inBounds(d.stairsUp)) {
        q.push(d.stairsUp);
        visited[idx(d.stairsUp.x, d.stairsUp.y)] = 1;
    }

    const int dirs[8][2] = {
        {1,0},{-1,0},{0,1},{0,-1},
        {1,1},{1,-1},{-1,1},{-1,-1},
    };

    while (!q.empty()) {
        Vec2i p = q.front();
        q.pop();

        for (auto& dxy : dirs) {
            int nx = p.x + dxy[0];
            int ny = p.y + dxy[1];
            if (!d.inBounds(nx, ny)) continue;
            if (!d.isPassable(nx, ny)) continue;
            size_t id = idx(nx, ny);
            if (visited[id]) continue;
            visited[id] = 1;
            q.push({nx, ny});
        }
    }

    if (inBounds(d.stairsDown)) {
        expect(visited[idx(d.stairsDown.x, d.stairsDown.y)] != 0,
               "stairsDown not reachable from stairsUp");
    }
}

void test_item_defs_sane() {
    for (int k = 0; k < ITEM_KIND_COUNT; ++k) {
        const ItemKind kind = static_cast<ItemKind>(k);
        const ItemDef& def = itemDef(kind);

        expect(def.kind == kind, "ItemDef kind mismatch for kind " + std::to_string(k));
        expect(def.name != nullptr && std::string(def.name).size() > 0,
               "ItemDef name missing for kind " + std::to_string(k));

        // Design invariant: all consumables in ProcRogue are stackable.
        if (def.consumable) {
            expect(def.stackable, "Consumable item should be stackable (kind " + std::to_string(k) + ")");
        }
    }
}

void test_scores_legacy_load() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_legacy_test.csv";
#else
    const std::string path = "procrogue_scores_legacy_test.csv";
#endif

    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,won,score,depth,turns,kills,level,gold,seed\n";
        out << "2025-01-01T00:00:00Z,0,1234,3,100,5,2,10,42\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard legacy load failed");
#else
    expect(sb.load(path), "ScoreBoard legacy load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard legacy entry count");

    const auto& e = sb.entries().front();
    expect(e.score == 1234u, "Legacy score parsed");
    expect(e.depth == 3, "Legacy depth parsed");
    expect(e.turns == 100u, "Legacy turns parsed");
    expect(e.kills == 5u, "Legacy kills parsed");
    expect(e.level == 2, "Legacy level parsed");
    expect(e.gold == 10, "Legacy gold parsed");
    expect(e.seed == 42u, "Legacy seed parsed");
    expect(e.name.empty(), "Legacy name should be empty");
    expect(e.cause.empty(), "Legacy cause should be empty");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_scores_new_format_load_and_escape() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_newfmt_test.csv";
#else
    const std::string path = "procrogue_scores_newfmt_test.csv";
#endif

    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,name,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01T00:00:00Z,\"The, Name\",1,0,10,200,7,5,123,999,\"ESCAPED WITH \"\"THE\"\" AMULET\",0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard newfmt load failed");
#else
    expect(sb.load(path), "ScoreBoard newfmt load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard newfmt entry count");

    const auto& e = sb.entries().front();
    expect(e.won == true, "Newfmt won parsed");
    expect(e.name == "The, Name", "Newfmt name parsed/escaped");
    expect(e.cause == "ESCAPED WITH \"THE\" AMULET", "Newfmt cause parsed/escaped");
    expect(e.gameVersion == "0.8.0", "Newfmt version parsed");

    // Score was 0 in file; should have been recomputed.
    expect(e.score != 0u, "Newfmt score recomputed");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_scores_append_roundtrip() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_roundtrip_test.csv";
#else
    const std::string path = "procrogue_scores_roundtrip_test.csv";
#endif

    ScoreBoard sb;
    ScoreEntry e;
    e.timestamp = "2025-01-01T00:00:00Z";
    e.name = "Tester";
    e.won = false;
    e.depth = 4;
    e.turns = 50;
    e.kills = 2;
    e.level = 3;
    e.gold = 10;
    e.seed = 77;
    e.cause = "KILLED BY GOBLIN";
    e.gameVersion = "0.8.0";

#if __has_include(<filesystem>)
    expect(sb.append(path.string(), e), "ScoreBoard append failed");
#else
    expect(sb.append(path, e), "ScoreBoard append failed");
#endif

    ScoreBoard sb2;
#if __has_include(<filesystem>)
    expect(sb2.load(path.string()), "ScoreBoard roundtrip load failed");
#else
    expect(sb2.load(path), "ScoreBoard roundtrip load failed");
#endif
    expect(sb2.entries().size() == 1, "ScoreBoard roundtrip entry count");

    const auto& r = sb2.entries().front();
    expect(r.name == "Tester", "Roundtrip name preserved");
    expect(r.cause == "KILLED BY GOBLIN", "Roundtrip cause preserved");
    expect(r.gameVersion == "0.8.0", "Roundtrip version preserved");
    expect(r.seed == 77u, "Roundtrip seed preserved");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

} // namespace

int main() {
    std::cout << "Running ProcRogue tests...\n";

    test_rng_reproducible();
    test_dungeon_stairs_connected();
    test_item_defs_sane();

    test_scores_legacy_load();
    test_scores_new_format_load_and_escape();
    test_scores_append_roundtrip();

    if (failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
