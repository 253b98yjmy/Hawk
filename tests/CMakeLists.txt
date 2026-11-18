# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(hk_tests
    hk_test_main.c
    test_base.c
    test_utf8.c
    test_net.c
    test_css.c
)
target_link_libraries(hk_tests PRIVATE hk_engine hk_net hk_style)
target_include_directories(hk_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
hk_copy_runtime(hk_tests)

# One ctest case per entry in tests.def.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tests.def)
file(STRINGS tests.def _entries REGEX "^HK_TEST_ENTRY\\(")
foreach(_line IN LISTS _entries)
    string(REGEX REPLACE "^HK_TEST_ENTRY\\(([A-Za-z0-9_]+)\\).*$" "\\1" _name "${_line}")
    add_test(NAME base.${_name} COMMAND hk_tests ${_name})
endforeach()

# html5lib-tests tree construction suite.
add_executable(hk_html5lib_tree html5lib_tree.c)
target_link_libraries(hk_html5lib_tree PRIVATE hk_engine)
hk_copy_runtime(hk_html5lib_tree)
add_test(NAME html5lib.tree
         COMMAND hk_html5lib_tree ${PROJECT_SOURCE_DIR}/third_party/html5lib-tests/tree-construction)

add_executable(hk_html5lib_tokenizer html5lib_tokenizer.c)
target_link_libraries(hk_html5lib_tokenizer PRIVATE hk_engine)
hk_copy_runtime(hk_html5lib_tokenizer)
add_test(NAME html5lib.tokenizer
         COMMAND hk_html5lib_tokenizer ${PROJECT_SOURCE_DIR}/third_party/html5lib-tests/tokenizer)

# WPT URL parsing tests.
add_executable(hk_url_wpt url_wpt.c)
target_link_libraries(hk_url_wpt PRIVATE hk_net)
target_include_directories(hk_url_wpt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
hk_copy_runtime(hk_url_wpt)
add_test(NAME wpt.url COMMAND hk_url_wpt ${CMAKE_CURRENT_SOURCE_DIR}/data/urltestdata.json)

# Launches hawk.exe, waits for the first frame, then closes it.
add_test(NAME app.smoke COMMAND hawk --smoke)
set_tests_properties(app.smoke PROPERTIES
    TIMEOUT 20
    PASS_REGULAR_EXPRESSION "first frame: ")
