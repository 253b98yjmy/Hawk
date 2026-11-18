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


/* --- HTTP body decoding ----------------------------------------------------------------------- */

#include "net/hk_http.h"

static bool collect(void *user, const uint8_t *p, size_t n)
{
    hk_charbuf_append(user, (const char *)p, n);
    return true;
}

/* Feeds input one byte at a time; returns the status and whether the body completed. */
static hk_status decode_body(hk_http_framing framing, uint64_t length, const char *coding, const uint8_t *in,
                             size_t n, hk_charbuf *out, bool *done)
{
    out->len = 0;
    *done = false;
    hk_http_body_decoder *d = hk_http_body_decoder_create(framing, length, hk_str_from_cstr(coding), collect, out);
    hk_status st = HK_OK;
    for (size_t i = 0; i < n && st == HK_OK && !*done; i++) st = hk_http_body_decoder_feed(d, in + i, 1, done);
    if (st == HK_OK && !*done) st = hk_http_body_decoder_finish(d);
    hk_http_body_decoder_destroy(d);
    return st;
}

HK_TEST(http_body_framing)
{
    hk_charbuf out = {0};
    bool done;
    const char *chunked = "4\r\nWiki\r\n5;ext=1\r\npedia\r\n0\r\nX-Trailer: y\r\n\r\nGARBAGE";
    HK_CHECK_EQ(decode_body(HK_HTTP_FRAMING_CHUNKED, 0, "", (const uint8_t *)chunked, strlen(chunked), &out, &done),
                HK_OK);
    HK_CHECK(done);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "Wikipedia");

    const char *bad = "Z\r\nxx\r\n";
    HK_CHECK_EQ(decode_body(HK_HTTP_FRAMING_CHUNKED, 0, "", (const uint8_t *)bad, strlen(bad), &out, &done),
                HK_ERR_INVALID);

    const char *body = "abcdefXYZ";
    HK_CHECK_EQ(decode_body(HK_HTTP_FRAMING_LENGTH, 6, "", (const uint8_t *)body, strlen(body), &out, &done), HK_OK);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "abcdef");
    /* Connection closed before Content-Length bytes arrived. */
    HK_CHECK_EQ(decode_body(HK_HTTP_FRAMING_LENGTH, 20, "", (const uint8_t *)body, strlen(body), &out, &done),
                HK_ERR_IO);
    HK_CHECK_EQ(decode_body(HK_HTTP_FRAMING_CLOSE, 0, "identity", (const uint8_t *)body, strlen(body), &out, &done),
                HK_OK);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "abcdefXYZ");
    hk_vec_free(&out);
}

HK_TEST(http_body_compression)
{
    static const uint8_t k_gzip[] = {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0A, 0xCB,
                                     0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40, 0x26, 0x13, 0xCB, 0xB3,
                                     0x01, 0xB3, 0x90, 0x6D, 0x0A, 0x16, 0x00, 0x00, 0x00};
    static const uint8_t k_zlib[] = {0x78, 0x9C, 0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40,
                                     0x26, 0x13, 0xCB, 0xB3, 0x01, 0x5F, 0x5B, 0x08, 0x48};
    static const uint8_t k_raw[] = {0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40, 0x26, 0x13, 0xCB, 0xB3, 0x01};
    hk_charbuf out = {0};
    bool done;
    HK_CHECK_EQ(decode_body(HK_HTTP_FRAMING_CLOSE, 0, "gzip", k_gzip, sizeof k_gzip, &out, &done), HK_OK);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "hello hello hello hawk");
    HK_CHECK_EQ(decode_body(HK_HTTP_FRAMING_CLOSE, 0, "deflate", k_zlib, sizeof k_zlib, &out, &done), HK_OK);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "hello hello hello hawk");
    /* Raw deflate sent as "deflate" (common server mistake): one-byte feeding retried as raw. */
    hk_http_body_decoder *d = hk_http_body_decoder_create(HK_HTTP_FRAMING_CLOSE, 0, HK_STR("deflate"), collect, &out);
    out.len = 0;
    HK_CHECK_EQ(hk_http_body_decoder_feed(d, k_raw, sizeof k_raw, &done), HK_OK);
    hk_http_body_decoder_destroy(d);
    HK_CHECK_STR(((hk_str){out.data, out.len}), "hello hello hello hawk");
    hk_vec_free(&out);
}
