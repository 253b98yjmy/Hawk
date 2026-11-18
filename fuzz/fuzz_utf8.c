/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Invariants of the UTF-8 / UTF-16 conversions on arbitrary bytes. */
#include "base/hk_utf8.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hk_arena a;
    hk_arena_init(&a, 0);

    /* The decoder always makes progress and stays in bounds. */
    size_t i = 0, count = 0;
    while (i < size) {
        uint32_t cp;
        size_t len = hk_utf8_decode(data + i, size - i, &cp);
        HK_ASSERT(len >= 1 && len <= 4 && len <= size - i);
        HK_ASSERT(cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF));
        i += len;
        count++;
    }

    /* UTF-8 -> UTF-16 -> UTF-8 yields valid UTF-8; identical to the input iff it was valid. */
    hk_str in = {(const char *)data, size};
    size_t n16 = 0;
    uint16_t *w = hk_utf8_to_utf16(&a, in, &n16);
    HK_ASSERT(n16 >= count && n16 <= 2 * count);
    hk_str back = hk_utf16_to_utf8(&a, w, n16);
    HK_ASSERT(hk_utf8_valid((const uint8_t *)back.p, back.n));
    HK_ASSERT(hk_utf8_valid(data, size) == hk_str_eq(back, in));

    /* Arbitrary UTF-16 (input reinterpreted) always converts to valid UTF-8. */
    hk_str from16 = hk_utf16_to_utf8(&a, (const uint16_t *)data, size / 2);
    HK_ASSERT(hk_utf8_valid((const uint8_t *)from16.p, from16.n));

    hk_arena_free(&a);
    return 0;
}
