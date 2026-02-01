add_executable(procrogue_tests
    test_main.cpp
)

target_link_libraries(procrogue_tests PRIVATE procrogue_core)


# The test suite intentionally peeks at internal game state for deterministic validation.
# GCC/Clang provide -fno-access-control to allow this without adding hundreds of bespoke accessors.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(procrogue_tests PRIVATE -fno-access-control)
endif()

target_include_directories(procrogue_tests PRIVATE ../src)
target_compile_features(procrogue_tests PRIVATE cxx_std_17)

# Keep warnings consistent with the main targets (and apply to the test TU itself).
procrogue_apply_warnings(procrogue_tests)

add_test(NAME procrogue_tests COMMAND procrogue_tests)
