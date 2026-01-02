add_executable(procrogue_tests
    test_main.cpp
)

target_link_libraries(procrogue_tests PRIVATE procrogue_core)

target_include_directories(procrogue_tests PRIVATE ../src)
target_compile_features(procrogue_tests PRIVATE cxx_std_17)

# Keep warnings consistent with the main targets (and apply to the test TU itself).
procrogue_apply_warnings(procrogue_tests)

add_test(NAME procrogue_tests COMMAND procrogue_tests)
