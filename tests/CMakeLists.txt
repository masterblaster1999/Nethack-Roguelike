add_executable(procrogue_tests
    test_main.cpp
    ../src/dungeon.cpp
    ../src/items.cpp
    ../src/scores.cpp
)

target_include_directories(procrogue_tests PRIVATE ../src)
target_compile_features(procrogue_tests PRIVATE cxx_std_17)

# Keep warnings consistent with the main target.
if (MSVC)
    target_compile_options(procrogue_tests PRIVATE /W4)
else()
    target_compile_options(procrogue_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME procrogue_tests COMMAND procrogue_tests)
