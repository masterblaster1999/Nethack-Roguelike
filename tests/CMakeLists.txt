add_executable(procrogue_tests
    test_main.cpp
)
procrogue_enable_windows_link_unlock(procrogue_tests procrogue_tests)

target_link_libraries(procrogue_tests PRIVATE procrogue_spritegen)


# The test suite intentionally peeks at internal game state for deterministic validation.
# GCC/Clang provide -fno-access-control to allow this without adding hundreds of bespoke accessors.
if (MSVC)
    # MSVC has no direct equivalent to -fno-access-control.
    # Keep declarations consistent across core + tests to avoid mangling mismatches.
    target_compile_definitions(procrogue_core PRIVATE PROCROGUE_TEST_ACCESS=1)
    target_compile_definitions(procrogue_tests PRIVATE PROCROGUE_TEST_ACCESS=1)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(procrogue_tests PRIVATE -fno-access-control)
endif()

target_include_directories(procrogue_tests PRIVATE ../src)
target_compile_features(procrogue_tests PRIVATE cxx_std_17)

# Keep warnings consistent with the main targets (and apply to the test TU itself).
procrogue_apply_warnings(procrogue_tests)

# Register each test case as its own CTest test for precise failure reporting.
set(_PROCROGUE_TEST_SOURCE "${CMAKE_CURRENT_LIST_DIR}/test_main.cpp")
file(READ "${_PROCROGUE_TEST_SOURCE}" _PROCROGUE_TEST_MAIN)
string(REGEX MATCHALL
    "\\{\"[^\"]+\",[ \t]*[A-Za-z_][A-Za-z0-9_]*\\}"
    _PROCROGUE_TEST_ENTRIES
    "${_PROCROGUE_TEST_MAIN}"
)

set(_PROCROGUE_REGISTERED_TESTS 0)
foreach(_PROCROGUE_ENTRY IN LISTS _PROCROGUE_TEST_ENTRIES)
    string(REGEX REPLACE "^\\{\"([^\"]+)\".*$" "\\1" _PROCROGUE_TEST_NAME "${_PROCROGUE_ENTRY}")
    set(_PROCROGUE_TEST_LABELS "unit")
    set(_PROCROGUE_TEST_TIMEOUT 30)

    if (_PROCROGUE_TEST_NAME MATCHES "^(overworld_|dungeon_|proc_leylines|wet_lab_)")
        list(APPEND _PROCROGUE_TEST_LABELS "worldgen")
        set(_PROCROGUE_TEST_TIMEOUT 60)
    endif()
    if (_PROCROGUE_TEST_NAME MATCHES "^spritegen_")
        list(APPEND _PROCROGUE_TEST_LABELS "spritegen")
    endif()
    if (_PROCROGUE_TEST_NAME MATCHES "^save_load_")
        list(APPEND _PROCROGUE_TEST_LABELS "io")
    endif()
    if (_PROCROGUE_TEST_NAME MATCHES "(shop|fishing|adhesive|lifecycle|farming|crafting)")
        list(APPEND _PROCROGUE_TEST_LABELS "gameplay")
    endif()

    add_test(
        NAME "procrogue_${_PROCROGUE_TEST_NAME}"
        COMMAND procrogue_tests "--exact=${_PROCROGUE_TEST_NAME}"
    )
    set_tests_properties("procrogue_${_PROCROGUE_TEST_NAME}" PROPERTIES
        LABELS "${_PROCROGUE_TEST_LABELS}"
        TIMEOUT "${_PROCROGUE_TEST_TIMEOUT}"
        ENVIRONMENT "PROCROGUE_TEST_NAME=${_PROCROGUE_TEST_NAME}"
    )
    if (_PROCROGUE_TEST_NAME MATCHES "^save_load_")
        set_tests_properties("procrogue_${_PROCROGUE_TEST_NAME}" PROPERTIES RESOURCE_LOCK "save_io")
    endif()
    math(EXPR _PROCROGUE_REGISTERED_TESTS "${_PROCROGUE_REGISTERED_TESTS}+1")
endforeach()

if (_PROCROGUE_REGISTERED_TESTS EQUAL 0)
    message(FATAL_ERROR "Failed to discover tests from ${_PROCROGUE_TEST_SOURCE}.")
endif()

message(STATUS "Registered ${_PROCROGUE_REGISTERED_TESTS} CTest tests from test_main.cpp")

unset(_PROCROGUE_TEST_SOURCE)
unset(_PROCROGUE_TEST_MAIN)
unset(_PROCROGUE_TEST_ENTRIES)
unset(_PROCROGUE_REGISTERED_TESTS)
unset(_PROCROGUE_TEST_LABELS)
unset(_PROCROGUE_TEST_TIMEOUT)
