/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Runs the WPT URL parsing tests (tests/data/urltestdata.json, BSD-3-Clause).
 *   hk_url_wpt <urltestdata.json> [-v]
 * Compares href and every component getter. Known gaps are listed in
 * k_known_failures with the reason; any other failure fails the run.
 */
#include "hk_json.h"
#include "base/hk_mem.h"
#include "net/hk_url.h"

#include <stdio.h>
#include <string.h>

/* Inputs whose expected result Hawk deliberately does not match (with the reason). */
static const char *const k_known_failures[] = {
    NULL,
};

static bool known_failure(const char *input)
{
    for (size_t i = 0; k_known_failures[i]; i++) {
        if (strcmp(k_known_failures[i], input) == 0) return true;
    }
    return false;
}

static void component(hk_charbuf *b, const char *name, hk_str v)
{
    hk_charbuf_append(b, name, strlen(name));
    hk_charbuf_push(b, '=');
    hk_charbuf_append(b, v.p, v.n);
    hk_charbuf_push(b, '\n');
}

static void url_components(const hk_url *u, hk_charbuf *b)
{
    char tmp[16];
    b->len = 0;
    component(b, "href", u->href);
    hk_charbuf proto = {0};
    hk_charbuf_append(&proto, u->scheme.p, u->scheme.n);
    hk_charbuf_push(&proto, ':');
    component(b, "protocol", (hk_str){proto.data, proto.len});
    hk_vec_free(&proto);
    component(b, "username", u->username);
    component(b, "password", u->password);
    int pn = u->port >= 0 ? snprintf(tmp, sizeof tmp, "%d", u->port) : 0;
    hk_charbuf host = {0};
    hk_charbuf_append(&host, u->host.p, u->host.n);
    if (pn) {
        hk_charbuf_push(&host, ':');
        hk_charbuf_append(&host, tmp, (size_t)pn);
    }
    component(b, "host", (hk_str){host.data, host.len});
    hk_vec_free(&host);
    component(b, "hostname", u->host);
    component(b, "port", (hk_str){tmp, (size_t)pn});
    component(b, "pathname", u->path);
    hk_charbuf q = {0};
    if (u->has_query && u->query.n) {
        hk_charbuf_push(&q, '?');
        hk_charbuf_append(&q, u->query.p, u->query.n);
    }
    component(b, "search", (hk_str){q.data, q.len});
    q.len = 0;
    if (u->has_fragment && u->fragment.n) {
        hk_charbuf_push(&q, '#');
        hk_charbuf_append(&q, u->fragment.p, u->fragment.n);
    }
    component(b, "hash", (hk_str){q.data, q.len});
    hk_vec_free(&q);
}

static void expected_components(const json *tc, hk_charbuf *b)
{
    static const char *const k_keys[] = {"href", "protocol", "username", "password", "host",
                                         "hostname", "port", "pathname", "search", "hash"};
    b->len = 0;
    for (size_t i = 0; i < HK_ARRAY_LEN(k_keys); i++) {
        const json *v = jget(tc, k_keys[i]);
        component(b, k_keys[i], v ? (hk_str){v->s, v->len} : HK_STR(""));
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <urltestdata.json> [-v]\n", argv[0]);
        return 2;
    }
    bool verbose = argc > 2 && strcmp(argv[2], "-v") == 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n);
    size_t len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    json *root = json_parse(buf, len);

    int64_t heap_before = hk_mem_heap_bytes();
    int passed = 0, failed = 0, known = 0, skipped = 0;
    hk_charbuf got = {0}, want = {0};
    for (size_t i = 0; i < root->count; i++) {
        const json *tc = root->items[i];
        if (tc->type != J_OBJ) continue;
        const json *input = jget(tc, "input"), *base = jget(tc, "base");
        const json *fail = jget(tc, "failure");
        if (has_surrogate(input) || has_surrogate(base)) {
            skipped++;
            continue;
        }
        hk_arena a;
        hk_arena_init(&a, 0);
        hk_url base_url, url;
        bool base_ok = true;
        if (base && base->type == J_STR)
            base_ok = hk_url_parse(&a, (hk_str){base->s, base->len}, NULL, &base_url) == HK_OK;
        bool ok;
        if (!base_ok) {
            ok = fail && fail->b;
            got.len = 0;
        } else {
            hk_status st = hk_url_parse(&a, (hk_str){input->s, input->len},
                                        base && base->type == J_STR ? &base_url : NULL, &url);
            if (fail && fail->b) {
                ok = st != HK_OK;
                got.len = 0;
                if (st == HK_OK) url_components(&url, &got);
                want.len = 0;
                hk_charbuf_append(&want, "failure\n", 8);
            } else {
                expected_components(tc, &want);
                if (st == HK_OK) url_components(&url, &got);
                else {
                    got.len = 0;
                    hk_charbuf_append(&got, "failure\n", 8);
                }
                ok = st == HK_OK && got.len == want.len && memcmp(got.data, want.data, got.len) == 0;
            }
        }
        hk_arena_free(&a);
        if (ok) {
            passed++;
        } else if (known_failure(input->s)) {
            known++;
        } else {
            failed++;
            if (verbose || failed <= 25)
                printf("FAIL input=<%s> base=<%s>\n--- expected\n%.*s--- got\n%.*s\n", input->s,
                       base && base->type == J_STR ? base->s : "null", (int)want.len, want.data,
                       (int)got.len, got.data);
        }
    }
    hk_vec_free(&got);
    hk_vec_free(&want);
    int64_t leaked = hk_mem_heap_bytes() - heap_before;
    jfree(root);
    free(buf);
    printf("\n%d passed, %d failed, %d known failures, %d skipped (lone surrogates)\n", passed,
           failed, known, skipped);
    if (leaked) printf("LEAK: %lld bytes\n", (long long)leaked);
    return failed || leaked ? 1 : 0;
}
