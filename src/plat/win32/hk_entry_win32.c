/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Process entry point. Kept out of hk_plat.lib so tools and tests can link the library. */
#include "plat/hk_plat.h"
#include "base/hk_utf8.h"
#include "base/hk_vec.h"

#include <windows.h>
#include <string.h>

/* --- command line -------------------------------------------------------------- */

/*
 * Splits the command line with the MSVC CRT rules (quotes, 2n / 2n+1 backslashes).
 * Avoids CommandLineToArgvW, which would pull shell32.dll into the process.
 */
static int split_args(hk_arena *a, const wchar_t *cmd, char ***out_argv)
{
    size_t cmd_len = wcslen(cmd);
    wchar_t *buf = HK_ARENA_NEW_ARRAY(a, wchar_t, cmd_len + 1);
    HK_VEC(char *) args = {0};

    const wchar_t *p = cmd;
    for (;;) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        size_t n = 0;
        bool quoted = false;
        while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
            size_t slashes = 0;
            while (*p == L'\\') {
                slashes++;
                p++;
            }
            if (*p == L'"') {
                for (size_t i = 0; i < slashes / 2; i++) buf[n++] = L'\\';
                if (slashes % 2) {
                    buf[n++] = L'"';
                } else if (quoted && p[1] == L'"') {
                    buf[n++] = L'"'; /* "" inside quotes is a literal quote */
                    p++;
                } else {
                    quoted = !quoted;
                }
                p++;
            } else {
                for (size_t i = 0; i < slashes; i++) buf[n++] = L'\\';
                if (*p && (quoted || (*p != L' ' && *p != L'\t'))) buf[n++] = *p++;
            }
        }
        hk_str arg = hk_utf16_to_utf8(a, (const uint16_t *)buf, n);
        hk_vec_push(&args, (char *)arg.p);
    }

    char **argv = HK_ARENA_NEW_ARRAY(a, char *, (size_t)args.len + 1);
    if (args.len) memcpy(argv, args.data, args.len * sizeof(char *));
    int argc = (int)args.len;
    hk_vec_free(&args);
    *out_argv = argv;
    return argc;
}

/* --- entry point ------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmd_line, int show)
{
    HK_UNUSED(instance);
    HK_UNUSED(prev);
    HK_UNUSED(cmd_line);
    HK_UNUSED(show);

    hk_plat_init();

    hk_arena args_arena;
    hk_arena_init(&args_arena, 4096);
    char **argv;
    int argc = split_args(&args_arena, GetCommandLineW(), &argv);
    int rc = hk_app_main(argc, argv);
    hk_arena_free(&args_arena);
    return rc;
}
