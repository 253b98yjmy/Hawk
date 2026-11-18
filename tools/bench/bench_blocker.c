/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Times hk_blocker_match over a list of URLs: bench_blocker <filters.bin> <urls.txt> [rounds].
 * Each line of urls.txt is "<type> <url> <site url>", type being one of doc, css, img, font, other. */
#include "blocker/hk_blocker.h"
#include "net/hk_psl.h"
#include "plat/hk_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct req {
    hk_block_request r;
} req;

static hk_str host_of(hk_str url)
{
    size_t p = hk_str_find(url, HK_STR("://"));
    p = p == HK_STR_NPOS ? 0 : p + 3;
    size_t e = p;
    while (e < url.n && url.p[e] != '/' && url.p[e] != ':' && url.p[e] != '?') e++;
    return hk_str_slice(url, p, e);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: bench_blocker filters.bin urls.txt [rounds]\n");
        return 2;
    }
    hk_plat_init();
    int rounds = argc > 3 ? atoi(argv[3]) : 20;
    hk_mapped_file m;
    if (!hk_plat_map_file(argv[1], &m)) return 1;
    uint64_t t0 = hk_plat_now_ns();
    hk_blocker *b = hk_blocker_open(m.data, m.size);
    printf("open: %.2f ms, %u filters\n", (double)(hk_plat_now_ns() - t0) / 1e6, hk_blocker_count(b));
    if (!b) return 1;
    hk_charbuf text = {0};
    if (hk_plat_read_file(argv[2], &text) != HK_OK) return 1;
    HK_VEC(req) reqs = {0};
    hk_str t = {text.data, text.len};
    size_t pos = 0;
    while (pos < t.n) {
        size_t eol = pos;
        while (eol < t.n && t.p[eol] != '\n') eol++;
        hk_str line = hk_str_trim_ascii_ws(hk_str_slice(t, pos, eol));
        pos = eol + 1;
        size_t s1 = hk_str_find_char(line, ' ');
        if (!line.n || line.p[0] == '#' || s1 == HK_STR_NPOS) continue;
        hk_str type = hk_str_slice(line, 0, s1);
        hk_str rest = hk_str_slice(line, s1 + 1, line.n);
        size_t s2 = hk_str_find_char(rest, ' ');
        hk_str url = s2 == HK_STR_NPOS ? rest : hk_str_slice(rest, 0, s2);
        hk_str site = s2 == HK_STR_NPOS ? (hk_str){0} : hk_str_slice(rest, s2 + 1, rest.n);
        uint32_t ty = hk_str_eq(type, HK_STR("doc"))   ? HK_REQ_DOCUMENT
                      : hk_str_eq(type, HK_STR("css")) ? HK_REQ_STYLESHEET
                      : hk_str_eq(type, HK_STR("img")) ? HK_REQ_IMAGE
                      : hk_str_eq(type, HK_STR("font")) ? HK_REQ_FONT
                                                        : HK_REQ_OTHER;
        req q = {{url, host_of(url), site, host_of(site), ty}};
        hk_vec_push(&reqs, q);
    }
    uint64_t *times = malloc(sizeof(uint64_t) * reqs.len * (size_t)rounds);
    size_t nt = 0, nblocked = 0;
    for (int r = 0; r < rounds; r++) {
        for (uint32_t i = 0; i < reqs.len; i++) {
            uint64_t s = hk_plat_now_ns();
            hk_block_result res = hk_blocker_match(b, &reqs.data[i].r);
            times[nt++] = hk_plat_now_ns() - s;
            if (r == 0) {
                nblocked += res.blocked;
                hk_str list = hk_blocker_filter_list(b, res.filter);
                printf("%s %.*s:%u  %.*s\n", res.blocked ? "BLOCK" : (res.filter >= 0 ? "allow" : "     "), (int)list.n,
                       list.p, hk_blocker_filter_line(b, res.filter), (int)reqs.data[i].r.url.n, reqs.data[i].r.url.p);
            }
        }
    }
    qsort(times, nt, sizeof(uint64_t), cmp_u64);
    uint64_t sum = 0;
    for (size_t i = 0; i < nt; i++) sum += times[i];
    printf("%u requests, %zu blocked; mean %.2f us, p50 %.2f us, p99 %.2f us, max %.2f us\n", reqs.len, nblocked,
           (double)sum / (double)nt / 1e3, (double)times[nt / 2] / 1e3, (double)times[nt * 99 / 100] / 1e3,
           (double)times[nt - 1] / 1e3);
    free(times);
    uint64_t p0 = hk_plat_now_ns();
    int same = 0;
    for (int i = 0; i < 100000; i++) same += hk_psl_same_site(HK_STR("image.itmedia.co.jp"), HK_STR("www.itmedia.co.jp"));
    printf("psl same_site: %.3f us (%d)\n", (double)(hk_plat_now_ns() - p0) / 1e3 / 100000, same);
    hk_blocker_close(b);
    hk_plat_unmap_file(&m);
    return 0;
}
