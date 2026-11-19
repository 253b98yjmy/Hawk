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
    plat/win32/hk_thread_win32.c
    plat/win32/hk_conn_win32.c
    plat/win32/hk_files_win32.c
    plat/win32/hk_text_dwrite.cpp
    plat/win32/hk_image_wic.c
)
target_link_libraries(hk_plat PUBLIC hk_base PRIVATE user32 gdi32 imm32 dwmapi advapi32 normaliz ws2_32 secur32 dwrite windowscodecs ole32)

# --- css + style: parsing and the cascade ------------------------------------
add_library(hk_style STATIC
    css/hk_css_tokenizer.c
    css/hk_css_parser.c
    style/hk_css_values.c
    style/hk_style.c
)
target_link_libraries(hk_style PUBLIC hk_engine hk_plat)

# --- layout + paint ------------------------------------------------------------
add_library(hk_render STATIC
    layout/hk_layout.c
    paint/hk_paint.c
    form/hk_form_view.c
)
target_link_libraries(hk_render PUBLIC hk_style hk_form)

# --- third_party/zlib (inflate only) -----------------------------------------
set(ZLIB_DIR ${PROJECT_SOURCE_DIR}/third_party/zlib)
add_library(hk_zlib STATIC
    ${ZLIB_DIR}/adler32.c ${ZLIB_DIR}/crc32.c ${ZLIB_DIR}/inffast.c
    ${ZLIB_DIR}/inflate.c ${ZLIB_DIR}/inftrees.c ${ZLIB_DIR}/zutil.c
)
target_include_directories(hk_zlib PUBLIC ${ZLIB_DIR})
target_compile_options(hk_zlib PRIVATE /W0 /utf-8)
target_compile_definitions(hk_zlib PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)

# --- net: URLs, encodings, HTTP ----------------------------------------------
add_library(hk_net STATIC
    net/hk_url.c
    net/hk_encoding.c
    net/hk_http.c
    net/hk_loader.c
    net/hk_psl.c
    net/hk_cookies.c
    net/hk_cache.c
    blocker/hk_blocker.c
    blocker/hk_filter_compile.c
    blocker/hk_filter_store.c
    blocker/hk_regex.c
)
target_link_libraries(hk_net PUBLIC hk_base hk_plat PRIVATE hk_zlib)

# --- form: control state, editing and submission --------------------------------
add_library(hk_form STATIC
    form/hk_form.c
)
target_link_libraries(hk_form PUBLIC hk_engine hk_net)

if(HK_FUZZ)
    return()
endif()

# --- hawk.exe ---------------------------------------------------------------
add_executable(hawk WIN32
    main.c
    ui/hk_chrome.c
    plat/win32/hk_entry_win32.c
    plat/win32/hawk.manifest
)
target_link_libraries(hawk PRIVATE hk_render hk_net hk_plat)
# wWinMain lives in hk_plat.lib, so the linker cannot infer the Unicode CRT entry point.
target_link_options(hawk PRIVATE /ENTRY:wWinMainCRTStartup)
hk_copy_runtime(hawk)
