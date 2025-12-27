#include "dungeon.hpp"
#include "items.hpp"
#include "rng.hpp"
#include "scores.hpp"
#include "settings.hpp"

#include <cstdint>
#include <cctype>
#include <cstdio>
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
            // Match in-game diagonal movement rules: no cutting corners.
            if (dxy[0] != 0 && dxy[1] != 0) {
                int ox = p.x + dxy[0];
                int oy = p.y;
                int px = p.x;
                int py = p.y + dxy[1];
                if (!d.inBounds(ox, oy) || !d.inBounds(px, py)) continue;
                if (!d.isWalkable(ox, oy)) continue;
                if (!d.isWalkable(px, py)) continue;
            }
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

void test_secret_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorSecret;

    expect(!d.isPassable(5, 5), "Secret doors should not be passable until discovered");
    expect(d.isOpaque(5, 5), "Secret doors should be opaque (block FOV/LOS) until discovered");
    expect(!d.isWalkable(5, 5), "Secret doors should not be walkable until discovered");
}

void test_locked_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorLocked;

    expect(d.isDoorLocked(5, 5), "DoorLocked should be detected as locked door");
    expect(!d.isPassable(5, 5), "Locked doors should not be passable until unlocked");
    expect(d.isOpaque(5, 5), "Locked doors should be opaque (block FOV/LOS) while closed");
    expect(!d.isWalkable(5, 5), "Locked doors should not be walkable while closed");

    d.unlockDoor(5, 5);
    expect(d.isDoorClosed(5, 5), "unlockDoor should convert a locked door into a closed door");
    d.openDoor(5, 5);
    expect(d.at(5, 5).type == TileType::DoorOpen, "openDoor should open an unlocked door");
}


void test_close_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorOpen;

    expect(d.isDoorOpen(5, 5), "DoorOpen should be detected as open door");
    expect(d.isPassable(5, 5), "Open door should be passable");
    expect(d.isWalkable(5, 5), "Open door should be walkable");
    expect(!d.isOpaque(5, 5), "Open door should not block FOV/LOS");

    d.closeDoor(5, 5);

    expect(d.isDoorClosed(5, 5), "closeDoor should convert an open door into a closed door");
    expect(!d.isDoorOpen(5, 5), "Closed door should not still be reported as open");
    // Closed doors are passable for pathing/AI, but not walkable for movement.
    expect(d.isPassable(5, 5), "Closed door should be passable for pathing/AI");
    expect(!d.isWalkable(5, 5), "Closed door should not be walkable while closed");
    expect(d.isOpaque(5, 5), "Closed door should block FOV/LOS");
}

void test_lock_door_tile_rules() {
    Dungeon d(10, 10);
    d.at(5, 5).type = TileType::DoorClosed;

    d.lockDoor(5, 5);

    expect(d.isDoorLocked(5, 5), "lockDoor should convert a closed door into a locked door");
    expect(!d.isPassable(5, 5), "Locked doors should not be passable after lockDoor");
    expect(d.isOpaque(5, 5), "Locked doors should be opaque after lockDoor");
    expect(!d.isWalkable(5, 5), "Locked doors should not be walkable after lockDoor");

    d.unlockDoor(5, 5);
    expect(d.isDoorClosed(5, 5), "unlockDoor should convert a locked door back into a closed door");
}

void test_fov_locked_door_blocks_visibility() {
    Dungeon d(10, 5);

    // Start with solid walls.
    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            d.at(x, y).type = TileType::Wall;
            d.at(x, y).visible = false;
            d.at(x, y).explored = false;
        }
    }

    // Carve a straight hallway with a locked door.
    for (int x = 1; x <= 4; ++x) {
        d.at(x, 2).type = TileType::Floor;
    }
    d.at(3, 2).type = TileType::DoorLocked;

    // Locked door should block visibility.
    d.computeFov(1, 2, 10);
    expect(d.at(3, 2).visible, "Locked door tile should be visible");
    expect(!d.at(4, 2).visible, "Tile behind locked door should not be visible");

    // Open door should allow visibility through.
    d.at(3, 2).type = TileType::DoorOpen;
    d.computeFov(1, 2, 10);
    expect(d.at(4, 2).visible, "Tile behind open door should be visible");
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




void test_scores_load_utf8_bom_header() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_bom_test.csv";
#else
    const std::string path = "procrogue_scores_bom_test.csv";
#endif

    {
        std::ofstream out(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        out << "\xEF\xBB\xBF" << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01 00:00:00,Tester,default,0,1234,3,100,5,2,10,42,CAUSE,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard BOM load failed");
#else
    expect(sb.load(path), "ScoreBoard BOM load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard BOM entry count");

    const auto& e = sb.entries().front();
    expect(e.name == "Tester", "ScoreBoard BOM name parsed");
    expect(e.slot == "default", "ScoreBoard BOM slot parsed");
    expect(e.cause == "CAUSE", "ScoreBoard BOM cause parsed");
    expect(e.gameVersion == "0.8.0", "ScoreBoard BOM version parsed");
    expect(e.score == 1234u, "ScoreBoard BOM score parsed");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}


void test_scores_quoted_whitespace_preserved() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_whitespace_test.csv";
#else
    const std::string path = "procrogue_scores_whitespace_test.csv";
#endif

    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,name,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01T00:00:00Z,   \"  Spaced Name  \"   ,0,0,1,0,0,1,0,1,   \"  CAUSE WITH SPACES  \"   ,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard whitespace load failed");
#else
    expect(sb.load(path), "ScoreBoard whitespace load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard whitespace entry count");

    const auto& e = sb.entries().front();
    expect(e.name == "  Spaced Name  ", "Quoted whitespace in name should be preserved");
    expect(e.cause == "  CAUSE WITH SPACES  ", "Quoted whitespace in cause should be preserved");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
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
    e.slot = "run1";
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
    expect(r.slot == "run1", "Roundtrip slot preserved");
    expect(r.seed == 77u, "Roundtrip seed preserved");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_scores_trim_keeps_recent_runs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_trim_recent_test.csv";
    std::error_code ec;
    fs::remove(path, ec);
#else
    const std::string path = "procrogue_scores_trim_recent_test.csv";
    std::remove(path.c_str());
#endif

    // Create a file with far more entries than we keep. Old behavior (trim by score only)
    // would discard low-scoring recent runs. New behavior keeps a mix: top scores + recent history.
    {
#if __has_include(<filesystem>)
        std::ofstream out(path);
#else
        std::ofstream out(path);
#endif
        out << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";

        // 150 high-scoring, older runs (day 1)
        for (int i = 0; i < 150; ++i) {
            const int mm = i / 60;
            const int ss = i % 60;
            char ts[64];
            std::snprintf(ts, sizeof(ts), "2025-01-01 00:%02d:%02d", mm, ss);
            out << ts << ",High,default,0," << (1000000 - i) << ",10,100,10,5,0," << i << ",,0.8.0\n";
        }

        // 60 low-scoring, newer runs (day 2)
        for (int i = 0; i < 60; ++i) {
            char ts[64];
            std::snprintf(ts, sizeof(ts), "2025-01-02 00:00:%02d", i);
            out << ts << ",Low,default,0,1,1,1,0,1,0," << (1000 + i) << ",,0.8.0\n";
        }
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard trim/recent load failed");
#else
    expect(sb.load(path), "ScoreBoard trim/recent load failed");
#endif

    // We keep 60 top scores + 60 most recent runs.
    expect(sb.entries().size() == 120, "ScoreBoard should keep 120 entries (top+recent mix)");

    bool foundNewest = false;
    for (const auto& e : sb.entries()) {
        if (e.timestamp == "2025-01-02 00:00:59") {
            foundNewest = true;
            break;
        }
    }
    expect(foundNewest, "ScoreBoard trimming should keep the newest low-score run");

    // Also ensure the top score still survives.
    if (!sb.entries().empty()) {
        expect(sb.entries().front().score == 1000000u, "ScoreBoard should retain the top score");
    }

    // Trimming again to a smaller cap should still preserve some recent history.
    sb.trim(10);
    expect(sb.entries().size() == 10, "ScoreBoard should trim down to 10 entries");

    bool foundNewestAfterSmallTrim = false;
    for (const auto& e : sb.entries()) {
        if (e.timestamp == "2025-01-02 00:00:59") {
            foundNewestAfterSmallTrim = true;
            break;
        }
    }
    expect(foundNewestAfterSmallTrim, "ScoreBoard small trim should still keep the newest low-score run");

#if __has_include(<filesystem>)
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}

void test_scores_u32_parsing_rejects_negative_and_overflow() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_u32_parse_test.csv";
    std::error_code ec;
    fs::remove(path, ec);
#else
    const std::string path = "procrogue_scores_u32_parse_test.csv";
    std::remove(path.c_str());
#endif

    {
        std::ofstream out(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        out << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        // Negative/overflow values should be rejected rather than wrapped into huge uint32_t values.
        out << "2025-01-01 00:00:00,Tester,default,0,-1,3,-1,0,1,0,42949672960,CAUSE,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard u32 parse load failed");
#else
    expect(sb.load(path), "ScoreBoard u32 parse load failed");
#endif
    expect(sb.entries().size() == 1, "ScoreBoard u32 parse entry count");

    const auto& e = sb.entries().front();
    expect(e.turns == 0u, "Negative turns should be rejected (remain default 0)");
    expect(e.seed == 0u, "Overflow seed should be rejected (remain default 0)");
    expect(e.score != 0u, "Invalid/negative score should trigger recompute");
    expect(e.score < 1000000u, "Recomputed score should not be absurdly large");

#if __has_include(<filesystem>)
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}

void test_scores_sort_ties_by_timestamp() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_scores_tie_sort_test.csv";
#else
    const std::string path = "procrogue_scores_tie_sort_test.csv";
#endif

    {
        std::ofstream out(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        out << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
        out << "2025-01-01 00:00:00,Tester,default,0,500,3,100,0,1,0,1,,0.8.0\n";
        out << "2025-01-02 00:00:00,Tester,default,0,500,3,100,0,1,0,1,,0.8.0\n";
    }

    ScoreBoard sb;
#if __has_include(<filesystem>)
    expect(sb.load(path.string()), "ScoreBoard tie sort load failed");
#else
    expect(sb.load(path), "ScoreBoard tie sort load failed");
#endif
    expect(sb.entries().size() == 2, "ScoreBoard tie sort entry count");

    // With identical score/won/turns, newest timestamp should sort first.
    expect(sb.entries()[0].timestamp == "2025-01-02 00:00:00", "ScoreBoard tie sort should prefer newest timestamp");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_scores_append_creates_parent_dirs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "procrogue_scores_nested_dir_test";
    const fs::path file = root / "nested" / "scores.csv";

    std::error_code ec;
    fs::remove_all(root, ec);

    ScoreBoard sb;
    ScoreEntry e;
    e.timestamp = "2025-01-01 00:00:00";
    e.won = false;
    e.depth = 2;
    e.turns = 10;
    e.kills = 0;
    e.level = 1;
    e.gold = 0;
    e.seed = 123;

    expect(sb.append(file.string(), e), "ScoreBoard append should create missing parent directories");
    expect(fs::exists(file), "ScoreBoard append should create the scores file");

    fs::remove_all(root, ec);
#else
    // No filesystem support: skip.
    expect(true, "Skipping directory-creation test (no <filesystem>)");
#endif
}

void test_settings_updateIniKey_creates_parent_dirs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "procrogue_settings_nested_dir_test";
    const fs::path file = root / "nested" / "settings.ini";

    std::error_code ec;
    fs::remove_all(root, ec);

    expect(updateIniKey(file.string(), "save_backups", "4"), "updateIniKey should create missing parent directories");
    Settings s = loadSettings(file.string());
    expect(s.saveBackups == 4, "updateIniKey should write settings in newly created dirs");

    fs::remove_all(root, ec);
#else
    // No filesystem support: skip.
    expect(true, "Skipping directory-creation test (no <filesystem>)");
#endif
}


void test_settings_writeDefaultSettings_creates_parent_dirs() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "procrogue_settings_default_nested_dir_test";
    const fs::path file = root / "nested" / "settings.ini";

    std::error_code ec;
    fs::remove_all(root, ec);

    expect(writeDefaultSettings(file.string()), "writeDefaultSettings should create missing parent directories");
    expect(fs::exists(file), "writeDefaultSettings should create the settings file");

    Settings s = loadSettings(file.string());
    expect(s.tileSize == 32, "writeDefaultSettings should write defaults (tile_size=32)");
    expect(s.playerName == "PLAYER", "writeDefaultSettings should write defaults (player_name=PLAYER)");

    fs::remove_all(root, ec);
#else
    // No filesystem support: skip.
    expect(true, "Skipping writeDefaultSettings directory-creation test (no <filesystem>)");
#endif
}





void test_settings_load_utf8_bom() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_bom_test.ini";
#else
    const std::string path = "procrogue_settings_bom_test.ini";
#endif

    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "\xEF\xBB\xBF" << "save_backups = 4\n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.saveBackups == 4, "Settings BOM should parse save_backups");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}


void test_settings_save_backups_parse() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_save_backups_test.ini";
#else
    const std::string path = "procrogue_settings_save_backups_test.ini";
#endif

    // Basic parse
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "# Test settings\n";
        f << "save_backups = 5\n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.saveBackups == 5, "save_backups should parse to 5");

    // Clamp low
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "save_backups = -1\n";
    }
#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 0, "save_backups should clamp to 0");

    // Clamp high
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "save_backups = 999\n";
    }
#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 10, "save_backups should clamp to 10");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}

void test_settings_autopickup_smart_parse() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_autopickup_smart_test.ini";
#else
    const std::string path = "procrogue_settings_autopickup_smart_test.ini";
#endif

    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "auto_pickup = smart\n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.autoPickup == AutoPickupMode::Smart, "auto_pickup=smart should parse to Smart");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}


void test_settings_default_slot_parse() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_default_slot_test.ini";
#else
    const std::string path = "procrogue_settings_default_slot_test.ini";
#endif

    // Basic parse + sanitize
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "default_slot =  My Run 01  \n";
    }

#if __has_include(<filesystem>)
    Settings s = loadSettings(path.string());
#else
    Settings s = loadSettings(path);
#endif
    expect(s.defaultSlot == "my_run_01", "default_slot should sanitize spaces and case");

    // "default" should clear it
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "default_slot = default\n";
    }

#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.defaultSlot.empty(), "default_slot=default should clear to empty");

    // Windows reserved base names should be prefixed
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        f << "default_slot = con\n";
    }

#if __has_include(<filesystem>)
    s = loadSettings(path.string());
#else
    s = loadSettings(path);
#endif
    expect(s.defaultSlot == "_con", "default_slot should avoid Windows reserved basenames");

#if __has_include(<filesystem>)
    std::error_code ec;
    fs::remove(path, ec);
#endif
}




void test_settings_ini_helpers_create_update_remove() {
#if __has_include(<filesystem>)
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "procrogue_settings_ini_helpers_test.ini";
    std::error_code ec;
    fs::remove(path, ec);
#else
    const std::string path = "procrogue_settings_ini_helpers_test.ini";
    std::remove(path.c_str());
#endif

    // updateIniKey should create the file if it doesn't exist.
#if __has_include(<filesystem>)
    expect(updateIniKey(path.string(), "save_backups", "7"), "updateIniKey should create file when missing");
    Settings s = loadSettings(path.string());
#else
    expect(updateIniKey(path, "save_backups", "7"), "updateIniKey should create file when missing");
    Settings s = loadSettings(path);
#endif
    expect(s.saveBackups == 7, "updateIniKey created save_backups=7");

    // updateIniKey should update an existing key.
#if __has_include(<filesystem>)
    expect(updateIniKey(path.string(), "save_backups", "2"), "updateIniKey should update an existing key");
    s = loadSettings(path.string());
#else
    expect(updateIniKey(path, "save_backups", "2"), "updateIniKey should update an existing key");
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 2, "updateIniKey updated save_backups=2");

    // updateIniKey should deduplicate multiple entries for the same key.
    {
        std::ofstream f(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
            , std::ios::app);
        f << "\n# duplicate\n";
        f << "save_backups = 9\n";
    }

#if __has_include(<filesystem>)
    expect(updateIniKey(path.string(), "save_backups", "4"), "updateIniKey should handle duplicate keys");
    s = loadSettings(path.string());
#else
    expect(updateIniKey(path, "save_backups", "4"), "updateIniKey should handle duplicate keys");
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 4, "updateIniKey should set save_backups=4 even with duplicates");

    auto trimKey = [](std::string s) {
        size_t a = 0;
        while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        size_t b = s.size();
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    };
    auto lowerKey = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };

    int occurrences = 0;
    {
        std::ifstream in(
#if __has_include(<filesystem>)
            path
#else
            path
#endif
        );
        std::string line;
        while (std::getline(in, line)) {
            std::string raw = line;
            auto commentPos = raw.find_first_of("#;");
            if (commentPos != std::string::npos) raw = raw.substr(0, commentPos);

            auto eq = raw.find('=');
            if (eq == std::string::npos) continue;

            std::string k = lowerKey(trimKey(raw.substr(0, eq)));
            if (k == "save_backups") ++occurrences;
        }
    }
    expect(occurrences == 1, "updateIniKey should remove duplicate save_backups entries");

    // removeIniKey should succeed if the file doesn't exist.
#if __has_include(<filesystem>)
    const fs::path missing = fs::temp_directory_path() / "procrogue_settings_ini_helpers_missing.ini";
    fs::remove(missing, ec);
    expect(removeIniKey(missing.string(), "save_backups"), "removeIniKey should succeed when file is missing");
#else
    const std::string missing = "procrogue_settings_ini_helpers_missing.ini";
    std::remove(missing.c_str());
    expect(removeIniKey(missing, "save_backups"), "removeIniKey should succeed when file is missing");
#endif

    // removeIniKey should remove an existing key (defaults should apply again).
#if __has_include(<filesystem>)
    expect(removeIniKey(path.string(), "save_backups"), "removeIniKey should remove an existing key");
    s = loadSettings(path.string());
#else
    expect(removeIniKey(path, "save_backups"), "removeIniKey should remove an existing key");
    s = loadSettings(path);
#endif
    expect(s.saveBackups == 3, "removeIniKey removed save_backups (defaults restored)");

#if __has_include(<filesystem>)
    fs::remove(path, ec);
#else
    std::remove(path.c_str());
#endif
}
} // namespace

int main() {
    std::cout << "Running ProcRogue tests...\n";

    test_rng_reproducible();
    test_dungeon_stairs_connected();
    test_secret_door_tile_rules();
    test_locked_door_tile_rules();
    test_close_door_tile_rules();
    test_lock_door_tile_rules();
    test_fov_locked_door_blocks_visibility();
    test_item_defs_sane();

    test_scores_legacy_load();
    test_scores_new_format_load_and_escape();
    test_scores_load_utf8_bom_header();
    test_scores_quoted_whitespace_preserved();
    test_scores_append_roundtrip();
    test_scores_trim_keeps_recent_runs();
    test_scores_u32_parsing_rejects_negative_and_overflow();
    test_scores_sort_ties_by_timestamp();
    test_scores_append_creates_parent_dirs();
    test_settings_updateIniKey_creates_parent_dirs();
    test_settings_writeDefaultSettings_creates_parent_dirs();

    test_settings_load_utf8_bom();

    test_settings_save_backups_parse();
    test_settings_autopickup_smart_parse();
    test_settings_default_slot_parse();
    test_settings_ini_helpers_create_update_remove();

    if (failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
