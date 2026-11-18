/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_utf8.h"

size_t hk_utf8_decode(const uint8_t *s, size_t n, uint32_t *cp)
{
    HK_ASSERT(n > 0);
    uint8_t b0 = s[0];
    if (b0 < 0x80) {
        *cp = b0;
        return 1;
    }

    size_t need;
    uint32_t c;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1;
        c = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (b0 == 0xE0) lo = 0xA0; /* overlong */
        if (b0 == 0xED) hi = 0x9F; /* surrogates */
        need = 2;
        c = b0 & 0x0Fu;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (b0 == 0xF0) lo = 0x90; /* overlong */
        if (b0 == 0xF4) hi = 0x8F; /* > U+10FFFF */
        need = 3;
        c = b0 & 0x07u;
    } else {
        *cp = HK_REPLACEMENT_CHAR;
        return 1;
    }

    for (size_t i = 1; i <= need; i++) {
        if (i >= n || s[i] < lo || s[i] > hi) {
            *cp = HK_REPLACEMENT_CHAR;
            return i;
        }
        lo = 0x80;
        hi = 0xBF;
        c = (c << 6) | (s[i] & 0x3Fu);
    }
    *cp = c;
    return need + 1;
}

size_t hk_utf8_encode(uint32_t cp, uint8_t out[4])
{
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = HK_REPLACEMENT_CHAR;
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

bool hk_utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] < 0x80) {
            i++;
            continue;
        }
        uint32_t cp;
        size_t len = hk_utf8_decode(s + i, n - i, &cp);
        /* A genuine U+FFFD in the input is encoded as 3 bytes; errors consume fewer. */
        if (cp == HK_REPLACEMENT_CHAR && len != 3) return false;
        if (cp == HK_REPLACEMENT_CHAR && !(s[i] == 0xEF && s[i + 1] == 0xBF && s[i + 2] == 0xBD))
            return false;
        i += len;
    }
    return true;
}

uint16_t *hk_utf8_to_utf16(hk_arena *a, hk_str s, size_t *out_len)
{
    /* Every code point takes at least as many UTF-8 bytes as UTF-16 units. */
    HK_ASSERT(s.n < SIZE_MAX / 2);
    uint16_t *out = HK_ARENA_NEW_ARRAY(a, uint16_t, s.n + 1);
    const uint8_t *p = (const uint8_t *)s.p;
    size_t i = 0, o = 0;
    while (i < s.n) {
        uint32_t cp;
        i += hk_utf8_decode(p + i, s.n - i, &cp);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out[o++] = (uint16_t)cp;
        }
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

hk_str hk_utf16_to_utf8(hk_arena *a, const uint16_t *s, size_t n)
{
    HK_ASSERT(n < SIZE_MAX / 3 - 1);
    uint8_t *out = hk_arena_alloc(a, n * 3 + 1, 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00u);
            i++;
        }
        /* Unpaired surrogates fall through and are encoded as U+FFFD. */
        o += hk_utf8_encode(cp, out + o);
    }
    out[o] = 0;
    return (hk_str){(const char *)out, o};
}
