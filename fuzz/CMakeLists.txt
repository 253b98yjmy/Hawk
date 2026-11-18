# SPDX-License-Identifier: GPL-3.0-or-later
# Fuzz targets (libFuzzer + ASan). Build with: scripts\build.cmd fuzz
# Run e.g.: build\fuzz\fuzz\fuzz_utf8.exe -max_total_time=60 fuzz\corpus\utf8
function(hk_add_fuzzer name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE hk_base)
    hk_copy_runtime(${name})
endfunction()

hk_add_fuzzer(fuzz_utf8 fuzz_utf8.c)
hk_add_fuzzer(fuzz_atom fuzz_atom.c)
hk_add_fuzzer(fuzz_html fuzz_html.c)
target_link_libraries(fuzz_html PRIVATE hk_engine)
hk_add_fuzzer(fuzz_net fuzz_net.c)
target_link_libraries(fuzz_net PRIVATE hk_net)
hk_add_fuzzer(fuzz_css fuzz_css.c)
target_link_libraries(fuzz_css PRIVATE hk_style)
hk_add_fuzzer(fuzz_filters fuzz_filters.c)
target_link_libraries(fuzz_filters PRIVATE hk_net)
