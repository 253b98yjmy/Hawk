# SPDX-License-Identifier: GPL-3.0-or-later

# --- base: platform-independent foundation -------------------------------
add_library(hk_base STATIC
    base/hk_common.c
    base/hk_log.c
    base/hk_mem.c
    base/hk_arena.c
    base/hk_str.c
    base/hk_vec.c
    base/hk_map64.c
    base/hk_atom.c
    base/hk_utf8.c
)
target_include_directories(hk_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hk_base PUBLIC hk_options)

# --- engine: DOM and HTML parsing ------------------------------------------------
add_library(hk_engine STATIC
    dom/hk_names.c
    dom/hk_dom.c
    html/hk_html_tokenizer.c
    html/hk_html_parser.c
)
target_link_libraries(hk_engine PUBLIC hk_base)

# --- plat/win32: OS integration --------------------------------------------
add_library(hk_plat STATIC
    plat/win32/hk_plat_win32.c
    plat/win32/hk_window_win32.c
)
target_link_libraries(hk_plat PUBLIC hk_base PRIVATE user32 gdi32 dwmapi advapi32 normaliz)

# --- net: URLs, encodings, HTTP ----------------------------------------------
add_library(hk_net STATIC
    net/hk_url.c
    net/hk_encoding.c
)
target_link_libraries(hk_net PUBLIC hk_base hk_plat)

if(HK_FUZZ)
    return()
endif()

# --- hawk.exe ---------------------------------------------------------------
add_executable(hawk WIN32
    main.c
    plat/win32/hk_entry_win32.c
    plat/win32/hawk.manifest
)
target_link_libraries(hawk PRIVATE hk_plat)
# wWinMain lives in hk_plat.lib, so the linker cannot infer the Unicode CRT entry point.
target_link_options(hawk PRIVATE /ENTRY:wWinMainCRTStartup)
hk_copy_runtime(hawk)
