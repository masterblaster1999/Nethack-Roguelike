#include "dungeon.hpp"
#include "items.hpp"
#include "rng.hpp"

#include <cstdint>
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

} // namespace

int main() {
    std::cout << "Running ProcRogue tests...\n";

    test_rng_reproducible();
    test_dungeon_stairs_connected();
    test_item_defs_sane();

    if (failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }

    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
