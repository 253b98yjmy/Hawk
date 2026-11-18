/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_plat.h"
#include "base/hk_utf8.h"

#include <windows.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>

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

hk_status hk_plat_write_file_atomic(const char *path, const char *data, size_t n)
{
    hk_arena a;
    hk_arena_init(&a, 2048);
    hk_charbuf tmp = {0};
    hk_charbuf_append(&tmp, path, strlen(path));
    hk_charbuf_append(&tmp, ".tmp", 5);
    const wchar_t *wtmp = to_wide(&a, tmp.data);
    const wchar_t *wpath = to_wide(&a, path);
    hk_vec_free(&tmp);
    hk_status st = HK_ERR_IO;
    HANDLE h = CreateFileW(wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        bool ok = n <= 0x7FFFFFFF && WriteFile(h, data, (DWORD)n, &written, NULL) && written == n;
        ok = ok && FlushFileBuffers(h);
        CloseHandle(h);
        if (ok && MoveFileExW(wtmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) st = HK_OK;
        else DeleteFileW(wtmp);
    }
    hk_arena_free(&a);
    return st;
}

void hk_plat_make_dir(const char *path)
{
    hk_arena a;
    hk_arena_init(&a, 1024);
    CreateDirectoryW(to_wide(&a, path), NULL);
    hk_arena_free(&a);
}

typedef struct file_info {
    uint64_t size;
    uint64_t written;
    wchar_t name[64];
} file_info;

static int by_age(const void *x, const void *y)
{
    const file_info *a = x, *b = y;
    return a->written < b->written ? -1 : a->written > b->written;
}

void hk_plat_trim_dir(const char *dir, uint64_t max_bytes, uint64_t target_bytes)
{
    hk_arena a;
    hk_arena_init(&a, 4096);
    hk_charbuf pattern = {0};
    hk_charbuf_append(&pattern, dir, strlen(dir));
    hk_charbuf_append(&pattern, "\\*", 3);
    const wchar_t *wpat = to_wide(&a, pattern.data);
    hk_vec_free(&pattern);
    HK_VEC(file_info) files = {0};
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (wcslen(fd.cFileName) >= 64) continue;
            file_info fi = {0};
            fi.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            fi.written = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            wcscpy_s(fi.name, 64, fd.cFileName);
            total += fi.size;
            hk_vec_push(&files, fi);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (total > max_bytes) {
        qsort(files.data, files.len, sizeof(file_info), by_age);
        const wchar_t *wdir = to_wide(&a, dir);
        for (uint32_t i = 0; i < files.len && total > target_bytes; i++) {
            wchar_t full[MAX_PATH];
            if (swprintf_s(full, MAX_PATH, L"%s\\%s", wdir, files.data[i].name) > 0 && DeleteFileW(full))
                total -= files.data[i].size;
        }
    }
    hk_vec_free(&files);
    hk_arena_free(&a);
}

int64_t hk_plat_unix_time(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)(t / 10000000ull) - 11644473600ll;
}
