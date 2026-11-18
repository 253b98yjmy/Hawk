# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(hk_tests
    hk_test_main.c
    test_base.c
    test_utf8.c
)
target_link_libraries(hk_tests PRIVATE hk_base)
target_include_directories(hk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
hk_copy_runtime(hk_tests)

# One ctest case per entry in tests.def.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tests.def)
file(STRINGS tests.def _entries REGEX "^HK_TEST_ENTRY\\(")
foreach(_line IN LISTS _entries)
    string(REGEX REPLACE "^HK_TEST_ENTRY\\(([A-Za-z0-9_]+)\\).*$" "\\1" _name "${_line}")
    add_test(NAME base.${_name} COMMAND hk_tests ${_name})
endforeach()

# Launches hawk.exe, waits for the first frame, then closes it.
add_test(NAME app.smoke COMMAND hawk --smoke)
set_tests_properties(app.smoke PROPERTIES
    TIMEOUT 20
    PASS_REGULAR_EXPRESSION "first frame: ")
