/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_plat.h"
#include "base/hk_utf8.h"

#include <windows.h>
#include <string.h>
#include <wchar.h>

static const wchar_t *to_wide(hk_arena *a, const char *path)
{
    return (const wchar_t *)hk_utf8_to_utf16(a, hk_str_from_cstr(path), NULL);
}

hk_status hk_plat_read_file(const char *path, hk_charbuf *out)
{
    hk_arena a;
    hk_arena_init(&a, 1024);
    HANDLE h = CreateFileW(to_wide(&a, path), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    hk_arena_free(&a);
    if (h == INVALID_HANDLE_VALUE) return HK_ERR_NOT_FOUND;
    LARGE_INTEGER size;
    hk_status st = HK_OK;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 256 * 1024 * 1024) {
        st = HK_ERR_LIMIT;
    } else {
        hk_vec_reserve(out, (uint64_t)out->len + (uint64_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, out->data + out->len, (DWORD)size.QuadPart, &got, NULL)) st = HK_ERR_IO;
        else out->len += got;
    }
    CloseHandle(h);
    return st;
}

bool hk_plat_data_dir(hk_charbuf *out)
{
    /* SHGetKnownFolderPath lives in shell32; the environment variable keeps shell32 out. */
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 8) return false;
    wcscat_s(base, MAX_PATH, L"\\Hawk");
    CreateDirectoryW(base, NULL);
    hk_arena a;
    hk_arena_init(&a, 1024);
    hk_str s = hk_utf16_to_utf8(&a, (const uint16_t *)base, wcslen(base));
    hk_charbuf_append(out, s.p, s.n);
    hk_arena_free(&a);
    return true;
}

void hk_plat_list_files(const char *dir, const char *suffix, void (*fn)(void *user, const char *path), void *user)
{
    hk_arena a;
    hk_arena_init(&a, 4096);
    hk_charbuf pattern = {0};
    hk_charbuf_append(&pattern, dir, strlen(dir));
    hk_charbuf_append(&pattern, "\\*", 3); /* includes the terminating NUL */
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(to_wide(&a, pattern.data), &fd);
    hk_vec_free(&pattern);
    if (h != INVALID_HANDLE_VALUE) {
        hk_str suf = hk_str_from_cstr(suffix);
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            hk_arena_mark m = hk_arena_get_mark(&a);
            hk_str name = hk_utf16_to_utf8(&a, (const uint16_t *)fd.cFileName, wcslen(fd.cFileName));
            if (hk_str_ends_with(name, suf)) {
                hk_charbuf full = {0};
                hk_charbuf_append(&full, dir, strlen(dir));
                hk_charbuf_push(&full, '\\');
                hk_charbuf_append(&full, name.p, name.n);
                hk_charbuf_push(&full, '\0');
                fn(user, full.data);
                hk_vec_free(&full);
            }
            hk_arena_rewind(&a, m);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    hk_arena_free(&a);
}
