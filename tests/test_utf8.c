/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hk_test.h"
#include "base/hk_utf8.h"

#include <string.h>

/* Decodes the whole input; returns the number of code points written to out. */
static size_t decode_all(const char *s, size_t n, uint32_t *out, size_t cap)
{
    size_t i = 0, count = 0;
    while (i < n && count < cap) {
        i += hk_utf8_decode((const uint8_t *)s + i, n - i, &out[count++]);
    }
    return count;
}

HK_TEST(utf8_decode_valid)
{
    uint32_t cp[8];
    const char s[] = "a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80"; /* a é あ 😀 */
    size_t n = decode_all(s, sizeof s - 1, cp, 8);
    HK_CHECK_EQ(n, 4);
    HK_CHECK_EQ(cp[0], 'a');
    HK_CHECK_EQ(cp[1], 0xE9);
    HK_CHECK_EQ(cp[2], 0x3042);
    HK_CHECK_EQ(cp[3], 0x1F600);
}

HK_TEST(utf8_decode_invalid)
{
    uint32_t cp[8];
    /* Each case: input, expected number of code points (all U+FFFD except noted). */
    struct {
        const char *s;
        size_t n;
        size_t expect;
    } cases[] = {
        {"\x80", 1, 1},             /* lone continuation */
        {"\xC0\xAF", 2, 2},         /* overlong: C0 invalid lead, AF lone continuation */
        {"\xE0\x80\xAF", 3, 3},     /* overlong 3-byte: E0 then 80 out of range */
        {"\xED\xA0\x80", 3, 3},     /* surrogate */
        {"\xF4\x90\x80\x80", 4, 4}, /* above U+10FFFF */
        {"\xE3\x81", 2, 1},         /* truncated: one error for the maximal subpart */
        {"\xF0\x9F\x98", 3, 1},     /* truncated 4-byte */
        {"\xFF", 1, 1},
    };
    for (size_t i = 0; i < HK_ARRAY_LEN(cases); i++) {
        size_t n = decode_all(cases[i].s, cases[i].n, cp, 8);
        HK_CHECK_EQ(n, cases[i].expect);
        for (size_t k = 0; k < n; k++) HK_CHECK_EQ(cp[k], HK_REPLACEMENT_CHAR);
    }
    /* Error recovery keeps the following valid character. */
    size_t n = decode_all("\xE3\x81" "a", 3, cp, 8);
    HK_CHECK_EQ(n, 2);
    HK_CHECK_EQ(cp[0], HK_REPLACEMENT_CHAR);
    HK_CHECK_EQ(cp[1], 'a');
}

HK_TEST(utf8_encode)
{
    uint8_t b[4];
    HK_CHECK_EQ(hk_utf8_encode('A', b), 1);
    HK_CHECK_EQ(hk_utf8_encode(0x3042, b), 3);
    HK_CHECK(b[0] == 0xE3 && b[1] == 0x81 && b[2] == 0x82);
    HK_CHECK_EQ(hk_utf8_encode(0x10FFFF, b), 4);
    HK_CHECK_EQ(hk_utf8_encode(0xD800, b), 3); /* surrogate -> U+FFFD */
    HK_CHECK(b[0] == 0xEF && b[1] == 0xBF && b[2] == 0xBD);
    HK_CHECK_EQ(hk_utf8_encode(0x110000, b), 3);
}

HK_TEST(utf8_validate)
{
    HK_CHECK(hk_utf8_valid((const uint8_t *)"", 0));
    HK_CHECK(hk_utf8_valid((const uint8_t *)"plain", 5));
    HK_CHECK(hk_utf8_valid((const uint8_t *)"\xEF\xBF\xBD", 3)); /* a real U+FFFD */
    HK_CHECK(hk_utf8_valid((const uint8_t *)"\xE6\x97\xA5\xE6\x9C\xAC", 6));
    HK_CHECK(!hk_utf8_valid((const uint8_t *)"\xC0\x80", 2));
    HK_CHECK(!hk_utf8_valid((const uint8_t *)"ab\xE3\x81", 4));
    HK_CHECK(!hk_utf8_valid((const uint8_t *)"\xED\xBF\xBF", 3));
}

HK_TEST(utf16_roundtrip)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_str s = HK_STR("Hawk \xE3\x83\x96\xE3\x83\xA9\xE3\x82\xA6\xE3\x82\xB6 \xF0\x9F\xA6\x85");
    size_t n16 = 0;
    uint16_t *w = hk_utf8_to_utf16(&a, s, &n16);
    HK_CHECK_EQ(n16, 5 + 4 + 1 + 2); /* the eagle is a surrogate pair */
    HK_CHECK_EQ(w[n16], 0);
    HK_CHECK_EQ(w[10], 0xD83E);
    hk_str back = hk_utf16_to_utf8(&a, w, n16);
    HK_CHECK(hk_str_eq(back, s));

    const uint16_t lone[] = {'x', 0xDC00, 'y', 0xD800};
    HK_CHECK_STR(hk_utf16_to_utf8(&a, lone, 4), "x\xEF\xBF\xBDy\xEF\xBF\xBD");
    hk_arena_free(&a);
}
