/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_str.h"

#include <string.h>

hk_str hk_str_from_cstr(const char *s)
{
    hk_str r = {s, s ? strlen(s) : 0};
    return r;
}

bool hk_str_eq(hk_str a, hk_str b)
{
    return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

bool hk_str_eq_ascii_icase(hk_str a, hk_str b)
{
    if (a.n != b.n) return false;
    for (size_t i = 0; i < a.n; i++) {
        if (hk_ascii_lower(a.p[i]) != hk_ascii_lower(b.p[i])) return false;
    }
    return true;
}

bool hk_str_starts_with(hk_str s, hk_str prefix)
{
    return s.n >= prefix.n && (prefix.n == 0 || memcmp(s.p, prefix.p, prefix.n) == 0);
}

bool hk_str_ends_with(hk_str s, hk_str suffix)
{
    return s.n >= suffix.n &&
           (suffix.n == 0 || memcmp(s.p + (s.n - suffix.n), suffix.p, suffix.n) == 0);
}

size_t hk_str_find_char(hk_str s, char c)
{
    if (s.n == 0) return HK_STR_NPOS;
    const char *hit = memchr(s.p, c, s.n);
    return hit ? (size_t)(hit - s.p) : HK_STR_NPOS;
}

size_t hk_str_find(hk_str h, hk_str needle)
{
    if (needle.n == 0) return 0;
    if (needle.n > h.n) return HK_STR_NPOS;
    size_t last = h.n - needle.n;
    size_t i = 0;
    while (i <= last) {
        const char *hit = memchr(h.p + i, needle.p[0], last - i + 1);
        if (!hit) return HK_STR_NPOS;
        i = (size_t)(hit - h.p);
        if (memcmp(h.p + i, needle.p, needle.n) == 0) return i;
        i++;
    }
    return HK_STR_NPOS;
}

hk_str hk_str_slice(hk_str s, size_t start, size_t end)
{
    if (end > s.n) end = s.n;
    if (start > end) start = end;
    hk_str r = {s.p + start, end - start};
    return r;
}

hk_str hk_str_trim_ascii_ws(hk_str s)
{
    size_t b = 0, e = s.n;
    while (b < e && hk_ascii_is_ws(s.p[b])) b++;
    while (e > b && hk_ascii_is_ws(s.p[e - 1])) e--;
    return hk_str_slice(s, b, e);
}

uint64_t hk_hash64(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint32_t hk_hash32(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}
