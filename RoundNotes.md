- Save files now **persist overworld travel**: overworld (Camp depth 0) chunk cache, player’s overworld X/Y, and atlas discovery are serialized (SAVE_VERSION v55). (src/game_save.cpp)
- Removed the old restriction that blocked saving while in a non-home wilderness chunk.
- Refactored level serialization into shared helpers to keep dungeon + overworld chunk saves consistent. (src/game_save.cpp)
- Added a regression test that saves/loads from a wilderness chunk and verifies a placed map marker survives the roundtrip. (tests/test_main.cpp)

How to verify: start a run → walk through a camp gate into the wilderness → save → load → confirm you’re still on the same wilderness chunk and the marker remains.