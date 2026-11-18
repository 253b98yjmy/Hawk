/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Filter list compiler, image validation, matching, and the regular expression engine. */
#include "blocker/hk_blocker.h"
#include "blocker/hk_regex.h"

#include <stdlib.h>
#include <string.h>

static const char *const k_urls[] = {
    "https://ads.example.com/banner/ad.png?utm_source=x&id=1",
    "https://www.example.co.jp/path/to/page.html#frag",
    "https://cdn.tracker.net:8443/p.gif?a=b&c",
    "https://example.org/",
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hk_str text = {(const char *)data, size};

    /* The input as a regular expression, searched over itself. */
    hk_regex *re = hk_regex_compile(hk_str_slice(text, 0, 256), size & 1);
    if (re) {
        hk_regex_search(re, text);
        hk_regex_free(re);
    }

    /* The input as a filter list. */
    hk_filter_stats st;
    hk_blocker *b = hk_blocker_from_text(text, &st);
    if (!b) abort(); /* the compiler must always produce a valid image */
    hk_charbuf out = {0};
    for (size_t i = 0; i < sizeof k_urls / sizeof k_urls[0]; i++) {
        hk_str url = hk_str_from_cstr(k_urls[i]);
        hk_str host = hk_str_slice(url, 8, 8 + strcspn(k_urls[i] + 8, ":/"));
        for (uint32_t type = 1; type <= HK_REQ_OTHER; type <<= 1) {
            hk_block_request r = {url, host, HK_STR("https://www.example.com/"), HK_STR("www.example.com"), type};
            hk_blocker_match(b, &r);
            hk_blocker_strip_params(b, &r, &out);
            r.site_url = (hk_str){0};
            hk_blocker_match(b, &r);
        }
    }
    hk_vec_free(&out);

    /* Corrupted images must be rejected, not crash. */
    hk_filter_compiler *c = hk_filter_compiler_create();
    hk_filter_compiler_add_list(c, HK_STR("x"), HK_STR("||a.com^\n/ad[0-9]+/\n@@||b.com^$image\n"));
    hk_charbuf img = {0};
    hk_filter_compiler_finish(c, 7, &img, NULL);
    for (size_t i = 0; i < size && i < 64; i++) img.data[(data[i] * 131u + i * 7u) % img.len] ^= data[i];
    hk_blocker *bad = hk_blocker_open(img.data, img.len);
    if (bad) {
        hk_block_request r = {hk_str_from_cstr(k_urls[0]), HK_STR("ads.example.com"), {0}, {0}, HK_REQ_IMAGE};
        hk_blocker_match(bad, &r);
        hk_blocker_close(bad);
    }
    hk_vec_free(&img);
    hk_blocker_close(b);
    return 0;
}
