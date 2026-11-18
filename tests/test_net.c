/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests for net/: encodings (URLs are covered by the WPT suite in url_wpt.c). */
#include "hk_test.h"
#include "net/hk_encoding.h"

#include <string.h>

/* "日本語" in several encodings. */
#define NIHONGO_UTF8 "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
#define NIHONGO_SJIS "\x93\xFA\x96\x7B\x8C\xEA"
#define NIHONGO_EUC "\xC6\xFC\xCB\xDC\xB8\xEC"
#define NIHONGO_JIS "\x1B$BF|K\x5C" "8l\x1B(B"

HK_TEST(encoding_labels)
{
    HK_CHECK_EQ(hk_encoding_from_label(HK_STR(" UTF8 ")), HK_ENC_UTF8);
    HK_CHECK_EQ(hk_encoding_from_label(HK_STR("Shift_JIS")), HK_ENC_SHIFT_JIS);
    HK_CHECK_EQ(hk_encoding_from_label(HK_STR("windows-31j")), HK_ENC_SHIFT_JIS);
    HK_CHECK_EQ(hk_encoding_from_label(HK_STR("x-euc-jp")), HK_ENC_EUC_JP);
    HK_CHECK_EQ(hk_encoding_from_label(HK_STR("latin1")), HK_ENC_WINDOWS_1252);
    HK_CHECK_EQ(hk_encoding_from_label(HK_STR("bogus")), HK_ENC_UNKNOWN);
    HK_CHECK_EQ(hk_encoding_from_content_type(HK_STR("text/html; charset=\"EUC-JP\"")), HK_ENC_EUC_JP);
    HK_CHECK_EQ(hk_encoding_from_content_type(HK_STR("text/html;Charset=utf-8;x=y")), HK_ENC_UTF8);
    HK_CHECK_EQ(hk_encoding_from_content_type(HK_STR("text/html")), HK_ENC_UNKNOWN);
}

static hk_encoding prescan(const char *s)
{
    return hk_encoding_prescan((const uint8_t *)s, strlen(s));
}

HK_TEST(encoding_prescan)
{
    HK_CHECK_EQ(prescan("<!doctype html><meta charset=shift_jis>"), HK_ENC_SHIFT_JIS);
    HK_CHECK_EQ(prescan("<META CHARSET=\"EUC-JP\">"), HK_ENC_EUC_JP);
    HK_CHECK_EQ(prescan("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=Shift_JIS\">"),
                HK_ENC_SHIFT_JIS);
    /* content= without the http-equiv pragma is ignored. */
    HK_CHECK_EQ(prescan("<meta content=\"text/html; charset=Shift_JIS\">"), HK_ENC_UNKNOWN);
    HK_CHECK_EQ(prescan("<!-- <meta charset=euc-jp> --><meta charset=utf-8>"), HK_ENC_UTF8);
    HK_CHECK_EQ(prescan("<title a='<meta charset=euc-jp>'><meta charset=big5>"), HK_ENC_BIG5);
    HK_CHECK_EQ(prescan("<meta charset=utf-16>"), HK_ENC_UTF8);
    HK_CHECK_EQ(prescan("<p>no declaration</p>"), HK_ENC_UNKNOWN);
}

static hk_encoding guess(const char *s)
{
    return hk_encoding_guess((const uint8_t *)s, strlen(s), false);
}

HK_TEST(encoding_guess)
{
    HK_CHECK_EQ(guess("<p>" NIHONGO_UTF8 "</p>"), HK_ENC_UTF8);
    HK_CHECK_EQ(guess("<p>" NIHONGO_SJIS "</p>"), HK_ENC_SHIFT_JIS);
    HK_CHECK_EQ(guess("<p>" NIHONGO_EUC "</p>"), HK_ENC_EUC_JP);
    HK_CHECK_EQ(guess("<p>" NIHONGO_JIS "</p>"), HK_ENC_ISO_2022_JP);
    HK_CHECK_EQ(guess("plain ascii"), HK_ENC_WINDOWS_1252);
    HK_CHECK_EQ(hk_encoding_guess((const uint8_t *)"ascii", 5, true), HK_ENC_SHIFT_JIS);
    size_t bom = 0;
    HK_CHECK_EQ(hk_encoding_sniff((const uint8_t *)"\xEF\xBB\xBFx", 4, HK_ENC_SHIFT_JIS, false, &bom),
                HK_ENC_UTF8);
    HK_CHECK_EQ(bom, 3);
}

/* Decodes s fed in chunks of the given size. */
static void decode_chunked(hk_encoding e, const char *s, size_t chunk, hk_charbuf *out)
{
    hk_decoder d;
    hk_decoder_init(&d, e);
    size_t n = strlen(s);
    out->len = 0;
    for (size_t i = 0; i < n; i += chunk) {
        size_t k = n - i < chunk ? n - i : chunk;
        hk_decoder_decode(&d, (const uint8_t *)s + i, k, false, out);
    }
    hk_decoder_decode(&d, NULL, 0, true, out);
    hk_decoder_free(&d);
}

HK_TEST(decoder_streaming)
{
    hk_charbuf out = {0};
    for (size_t chunk = 1; chunk <= 4; chunk++) {
        decode_chunked(HK_ENC_UTF8, "a" NIHONGO_UTF8 "b", chunk, &out);
        HK_CHECK_STR(((hk_str){out.data, out.len}), "a" NIHONGO_UTF8 "b");
        decode_chunked(HK_ENC_SHIFT_JIS, "a" NIHONGO_SJIS "b", chunk, &out);
        HK_CHECK_STR(((hk_str){out.data, out.len}), "a" NIHONGO_UTF8 "b");
        decode_chunked(HK_ENC_EUC_JP, NIHONGO_EUC, chunk, &out);
        HK_CHECK_STR(((hk_str){out.data, out.len}), NIHONGO_UTF8);
        decode_chunked(HK_ENC_ISO_2022_JP, NIHONGO_JIS, chunk, &out);
        HK_CHECK_STR(((hk_str){out.data, out.len}), NIHONGO_UTF8);
    }
    decode_chunked(HK_ENC_UTF8, "x\xE6\x97", 1, &out); /* truncated at the end */
    HK_CHECK_STR(((hk_str){out.data, out.len}), "x\xEF\xBF\xBD");
    decode_chunked(HK_ENC_WINDOWS_1252, "\x80\xE9", 1, &out);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "\xE2\x82\xAC\xC3\xA9");
    /* UTF-16LE "a" U+1F600, fed one byte at a time. */
    static const uint8_t utf16[] = {'a', 0, 0x3D, 0xD8, 0x00, 0xDE};
    hk_decoder d;
    hk_decoder_init(&d, HK_ENC_UTF16LE);
    out.len = 0;
    for (size_t i = 0; i < sizeof utf16; i++) hk_decoder_decode(&d, utf16 + i, 1, false, &out);
    hk_decoder_decode(&d, NULL, 0, true, &out);
    hk_decoder_free(&d);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "a\xF0\x9F\x98\x80");
    hk_vec_free(&out);
}
