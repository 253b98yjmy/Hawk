/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Network-facing parsers on arbitrary input. The first byte selects the target:
 * URL parsing (with and without a base), HTTP body decoding (framing x coding),
 * and encoding sniffing + streaming decoding.
 */
#include "base/hk_arena.h"
#include "base/hk_utf8.h"
#include "net/hk_encoding.h"
#include "net/hk_http.h"
#include "net/hk_url.h"

static bool sink(void *user, const uint8_t *p, size_t n)
{
    size_t *total = user;
    *total += n;
    return p != NULL || n == 0;
}

static void fuzz_url(const uint8_t *data, size_t size)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_url base, url, again;
    hk_str in = {(const char *)data, size};
    if (hk_url_parse(&a, HK_STR("https://user:pw@example.com:8080/a/b/c?q=1#f"), NULL, &base) == HK_OK &&
        hk_url_parse(&a, in, &base, &url) == HK_OK) {
        /* Serializing then re-parsing must be stable (idempotence of the serializer). */
        HK_ASSERT(hk_url_parse(&a, url.href, NULL, &again) == HK_OK);
        HK_ASSERT(hk_str_eq(again.href, url.href));
    }
    hk_url_parse(&a, in, NULL, &url);
    hk_arena_free(&a);
}

static void fuzz_body(const uint8_t *data, size_t size)
{
    if (size < 2) return;
    static const char *const k_codings[] = {"", "gzip", "deflate"};
    hk_http_framing framing = (hk_http_framing)(data[0] % 4);
    const char *coding = k_codings[data[1] % 3];
    size_t total = 0;
    hk_http_body_decoder *d =
        hk_http_body_decoder_create(framing, size / 2, hk_str_from_cstr(coding), sink, &total);
    bool done = false;
    hk_status st = HK_OK;
    for (size_t i = 2; i < size && st == HK_OK && !done; i += 7) {
        size_t n = size - i < 7 ? size - i : 7;
        st = hk_http_body_decoder_feed(d, data + i, n, &done);
    }
    if (st == HK_OK && !done) hk_http_body_decoder_finish(d);
    hk_http_body_decoder_destroy(d);
}

static void fuzz_encoding(const uint8_t *data, size_t size)
{
    size_t bom = 0;
    hk_encoding e = hk_encoding_sniff(data, size, HK_ENC_UNKNOWN, size % 2 == 0, &bom);
    HK_ASSERT(bom <= size);
    hk_decoder d;
    hk_decoder_init(&d, e);
    hk_charbuf out = {0};
    for (size_t i = bom; i < size; i += 3) {
        size_t n = size - i < 3 ? size - i : 3;
        hk_decoder_decode(&d, data + i, n, false, &out);
    }
    hk_decoder_decode(&d, NULL, 0, true, &out);
    HK_ASSERT(hk_utf8_valid((const uint8_t *)out.data, out.len));
    hk_decoder_free(&d);
    hk_vec_free(&out);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    switch (data[0] % 3) {
    case 0: fuzz_url(data + 1, size - 1); break;
    case 1: fuzz_body(data + 1, size - 1); break;
    default: fuzz_encoding(data + 1, size - 1); break;
    }
    return 0;
}
