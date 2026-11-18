/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Runs the html5lib-tests tokenizer suite (*.test, JSON).
 *   hk_html5lib_tokenizer <dir> [-v]
 * Parse errors are not compared (Hawk does not report them). Tests whose input or
 * output contains lone surrogates are skipped: Hawk's input is always valid UTF-8.
 */
#include "base/hk_mem.h"
#include "base/hk_utf8.h"
#include "base/hk_vec.h"
#include "html/hk_html_tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "hk_json.h"

/* --- token serialization (both sides use the same compact format) ------------------------------ */

static void put(hk_charbuf *b, const char *s, size_t n)
{
    hk_charbuf_append(b, s, n);
}

static void puts_(hk_charbuf *b, const char *s)
{
    put(b, s, strlen(s));
}

typedef struct collector {
    hk_charbuf out;
    hk_charbuf pending_chars;
} collector;

static void flush_chars(collector *c)
{
    if (!c->pending_chars.len) return;
    puts_(&c->out, "Character|");
    put(&c->out, c->pending_chars.data, c->pending_chars.len);
    puts_(&c->out, "\n");
    c->pending_chars.len = 0;
}

static int attr_cmp(const void *a, const void *b)
{
    const hk_html_tok_attr *x = a, *y = b;
    size_t n = HK_MIN(x->name.n, y->name.n);
    int r = memcmp(x->name.p, y->name.p, n);
    return r ? r : (int)(x->name.n > y->name.n) - (int)(x->name.n < y->name.n);
}

static void on_token(void *user, hk_html_token *t)
{
    collector *c = user;
    hk_charbuf *o = &c->out;
    if (t->type == HK_TOK_CHARS) {
        put(&c->pending_chars, t->data.p, t->data.n);
        return;
    }
    flush_chars(c);
    switch (t->type) {
    case HK_TOK_START_TAG: {
        puts_(o, "StartTag|");
        put(o, t->name.p, t->name.n);
        hk_html_tok_attr *sorted = calloc(t->attr_count + 1, sizeof *sorted);
        if (t->attr_count) memcpy(sorted, t->attrs, t->attr_count * sizeof *sorted);
        qsort(sorted, t->attr_count, sizeof *sorted, attr_cmp);
        for (uint32_t i = 0; i < t->attr_count; i++) {
            puts_(o, "|");
            put(o, sorted[i].name.p, sorted[i].name.n);
            puts_(o, "=");
            put(o, sorted[i].value.p, sorted[i].value.n);
        }
        free(sorted);
        if (t->self_closing) puts_(o, "|/");
        puts_(o, "\n");
        break;
    }
    case HK_TOK_END_TAG:
        puts_(o, "EndTag|");
        put(o, t->name.p, t->name.n);
        puts_(o, "\n");
        break;
    case HK_TOK_COMMENT:
        puts_(o, "Comment|");
        put(o, t->data.p, t->data.n);
        puts_(o, "\n");
        break;
    case HK_TOK_DOCTYPE:
        puts_(o, "DOCTYPE|");
        if (t->has_name) put(o, t->name.p, t->name.n);
        else puts_(o, "<null>");
        puts_(o, "|");
        if (t->has_public) put(o, t->public_id.p, t->public_id.n);
        else puts_(o, "<null>");
        puts_(o, "|");
        if (t->has_system) put(o, t->system_id.p, t->system_id.n);
        else puts_(o, "<null>");
        puts_(o, t->force_quirks ? "|quirks\n" : "|ok\n");
        break;
    default: break;
    }
}

static void expected_tokens(const json *output, hk_charbuf *o)
{
    collector c = {0};
    for (size_t i = 0; i < output->count; i++) {
        const json *tk = output->items[i];
        const char *kind = tk->items[0]->s;
        if (strcmp(kind, "Character") == 0) {
            put(&c.pending_chars, tk->items[1]->s, tk->items[1]->len);
            continue;
        }
        flush_chars(&c);
        if (strcmp(kind, "StartTag") == 0) {
            puts_(&c.out, "StartTag|");
            puts_(&c.out, tk->items[1]->s);
            const json *attrs = tk->items[2];
            size_t na = attrs->count / 2;
            hk_html_tok_attr *sorted = calloc(na + 1, sizeof *sorted);
            for (size_t k = 0; k < na; k++) {
                sorted[k].name = (hk_str){attrs->items[2 * k]->s, attrs->items[2 * k]->len};
                sorted[k].value = (hk_str){attrs->items[2 * k + 1]->s, attrs->items[2 * k + 1]->len};
            }
            qsort(sorted, na, sizeof *sorted, attr_cmp);
            for (size_t k = 0; k < na; k++) {
                puts_(&c.out, "|");
                put(&c.out, sorted[k].name.p, sorted[k].name.n);
                puts_(&c.out, "=");
                put(&c.out, sorted[k].value.p, sorted[k].value.n);
            }
            free(sorted);
            if (tk->count > 3 && tk->items[3]->b) puts_(&c.out, "|/");
            puts_(&c.out, "\n");
        } else if (strcmp(kind, "EndTag") == 0) {
            puts_(&c.out, "EndTag|");
            puts_(&c.out, tk->items[1]->s);
            puts_(&c.out, "\n");
        } else if (strcmp(kind, "Comment") == 0) {
            puts_(&c.out, "Comment|");
            put(&c.out, tk->items[1]->s, tk->items[1]->len);
            puts_(&c.out, "\n");
        } else if (strcmp(kind, "DOCTYPE") == 0) {
            puts_(&c.out, "DOCTYPE|");
            for (int k = 1; k <= 3; k++) {
                const json *f = tk->items[k];
                if (f->type == J_NULL) puts_(&c.out, "<null>");
                else put(&c.out, f->s, f->len);
                puts_(&c.out, "|");
            }
            puts_(&c.out, tk->items[4]->b ? "ok\n" : "quirks\n");
        }
    }
    flush_chars(&c);
    o->len = 0;
    put(o, c.out.data, c.out.len);
    hk_vec_free(&c.out);
    hk_vec_free(&c.pending_chars);
}

/* --- running ---------------------------------------------------------------------------------- */

static bool state_from_name(const char *s, hk_html_tok_state *out)
{
    static const struct {
        const char *name;
        hk_html_tok_state state;
    } k[] = {
        {"Data state", HK_TS_DATA},           {"PLAINTEXT state", HK_TS_PLAINTEXT},
        {"RCDATA state", HK_TS_RCDATA},       {"RAWTEXT state", HK_TS_RAWTEXT},
        {"Script data state", HK_TS_SCRIPT_DATA}, {"CDATA section state", HK_TS_CDATA_SECTION},
    };
    for (size_t i = 0; i < HK_ARRAY_LEN(k); i++) {
        if (strcmp(s, k[i].name) == 0) {
            *out = k[i].state;
            return true;
        }
    }
    return false;
}

static void tokenize(const json *input, hk_html_tok_state state, const json *last_start, bool bytewise,
                     hk_charbuf *out)
{
    collector c = {0};
    hk_html_tokenizer t;
    hk_html_tokenizer_init(&t, on_token, &c);
    hk_html_tokenizer_set_state(&t, state);
    if (state == HK_TS_CDATA_SECTION) t.cdata_allowed = true;
    if (last_start) hk_html_tokenizer_set_last_start_tag(&t, (hk_str){last_start->s, last_start->len});
    if (bytewise) {
        for (size_t i = 0; i < input->len; i++) hk_html_tokenizer_feed(&t, input->s + i, 1);
    } else {
        hk_html_tokenizer_feed(&t, input->s, input->len);
    }
    hk_html_tokenizer_finish(&t);
    hk_html_tokenizer_free(&t);
    flush_chars(&c);
    out->len = 0;
    put(out, c.out.data, c.out.len);
    hk_vec_free(&c.out);
    hk_vec_free(&c.pending_chars);
}

typedef struct stats {
    int passed, failed, skipped, shown;
} stats;

static void run_file(const char *dir, const char *name, stats *st, bool verbose)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    jparser jp = {buf, buf + len};
    json *root = jvalue(&jp);
    const json *tests = jget(root, "tests");
    int file_failed = 0, file_total = 0;
    hk_charbuf got = {0}, got_bytewise = {0}, want = {0};
    for (size_t i = 0; tests && i < tests->count; i++) {
        const json *tc = tests->items[i];
        const json *input = jget(tc, "input"), *output = jget(tc, "output");
        const json *states = jget(tc, "initialStates");
        const json *dbl = jget(tc, "doubleEscaped");
        if (dbl && dbl->b) {
            /* Unescape \\uXXXX sequences once more. */
            json *in2 = (json *)input;
            jparser sub = {NULL, NULL};
            (void)sub;
            char *q = malloc(in2->len + 3);
            q[0] = '"';
            memcpy(q + 1, in2->s, in2->len);
            q[in2->len + 1] = '"';
            jparser j2 = {q, q + in2->len + 2};
            json *un = jstring(&j2);
            free(q);
            free(in2->s);
            in2->s = un->s;
            in2->len = un->len;
            in2->lone_surrogate = un->lone_surrogate;
            un->s = NULL;
            jfree(un);
            for (size_t k = 0; k < output->count; k++) {
                json *tk = output->items[k];
                for (size_t m = 1; m < tk->count; m++) {
                    json *f2 = tk->items[m];
                    if (f2->type != J_STR) continue;
                    char *q2 = malloc(f2->len + 3);
                    q2[0] = '"';
                    memcpy(q2 + 1, f2->s, f2->len);
                    q2[f2->len + 1] = '"';
                    jparser j3 = {q2, q2 + f2->len + 2};
                    json *u2 = jstring(&j3);
                    free(q2);
                    free(f2->s);
                    f2->s = u2->s;
                    f2->len = u2->len;
                    f2->lone_surrogate = u2->lone_surrogate;
                    u2->s = NULL;
                    jfree(u2);
                }
            }
        }
        size_t nstates = states ? states->count : 1;
        for (size_t s = 0; s < nstates; s++) {
            file_total++;
            hk_html_tok_state state = HK_TS_DATA;
            if (states && !state_from_name(states->items[s]->s, &state)) {
                st->skipped++;
                continue;
            }
            if (has_surrogate(input) || has_surrogate(output)) {
                st->skipped++;
                continue;
            }
            expected_tokens(output, &want);
            tokenize(input, state, jget(tc, "lastStartTag"), false, &got);
            tokenize(input, state, jget(tc, "lastStartTag"), true, &got_bytewise);
            bool ok = got.len == want.len && memcmp(got.data, want.data, got.len) == 0;
            bool same = got.len == got_bytewise.len && memcmp(got.data, got_bytewise.data, got.len) == 0;
            if (ok && same) {
                st->passed++;
                continue;
            }
            st->failed++;
            file_failed++;
            if (verbose || st->shown < 15) {
                st->shown++;
                const json *desc = jget(tc, "description");
                printf("FAIL %s: %s%s\n--- input\n%.*s\n--- expected\n%.*s--- got\n%.*s\n", name,
                       desc ? desc->s : "?", ok ? " (byte-wise differs)" : "", (int)input->len,
                       input->s, (int)want.len, want.data, (int)got.len, got.data);
            }
        }
    }
    printf("%-28s %4d tests, %d failed\n", name, file_total, file_failed);
    hk_vec_free(&got);
    hk_vec_free(&got_bytewise);
    hk_vec_free(&want);
    jfree(root);
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tokenizer dir> [-v]\n", argv[0]);
        return 2;
    }
    bool verbose = argc > 2 && strcmp(argv[2], "-v") == 0;
    int64_t heap_before = hk_mem_heap_bytes();
    stats st = {0};
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*.test", argv[1]);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "no .test files in %s\n", argv[1]);
        return 2;
    }
    do {
        run_file(argv[1], fd.cFileName, &st, verbose);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    int64_t leaked = hk_mem_heap_bytes() - heap_before;
    printf("\n%d passed, %d failed, %d skipped (lone surrogates)\n", st.passed, st.failed, st.skipped);
    if (leaked) printf("LEAK: %lld bytes\n", (long long)leaked);
    return st.failed || leaked ? 1 : 0;
}
