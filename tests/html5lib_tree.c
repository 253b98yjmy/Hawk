/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Runs the html5lib-tests tree-construction suite (*.dat files).
 *   hk_html5lib_tree <dir> [-v]
 * Tests marked #script-on and fragment tests (#document-fragment) are skipped:
 * Hawk parses with scripting disabled and never parses fragments.
 * Every test is also parsed with the input fed one byte at a time, which must
 * produce the same tree (resumable tokenizer).
 */
#include "base/hk_mem.h"
#include "base/hk_vec.h"
#include "dom/hk_dom.h"
#include "html/hk_html_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

typedef HK_VEC(hk_str) str_list;

/* --- tree dump in the html5lib format ----------------------------------------------------- */

static void indent(hk_charbuf *out, int depth)
{
    hk_charbuf_append(out, "| ", 2);
    for (int i = 0; i < depth; i++) hk_charbuf_append(out, "  ", 2);
}

static void puts_buf(hk_charbuf *out, const char *s)
{
    hk_charbuf_append(out, s, strlen(s));
}

static void put_str(hk_charbuf *out, hk_str s)
{
    hk_charbuf_append(out, s.p, s.n);
}

typedef struct attr_line {
    char *text;
    size_t len;
} attr_line;

static int attr_cmp(const void *a, const void *b)
{
    const attr_line *x = a, *y = b;
    size_t n = x->len < y->len ? x->len : y->len;
    int r = memcmp(x->text, y->text, n);
    return r ? r : (x->len < y->len ? -1 : x->len > y->len);
}

static void dump_node(const hk_document *d, hk_node_id id, int depth, hk_charbuf *out)
{
    const hk_node *n = hk_dom_node(d, id);
    switch (n->type) {
    case HK_NODE_DOCTYPE: {
        const hk_doctype *dt = &d->doctypes.data[n->data];
        indent(out, depth);
        puts_buf(out, "<!DOCTYPE ");
        put_str(out, dt->name);
        if (dt->has_public || dt->has_system) {
            puts_buf(out, " \"");
            put_str(out, dt->public_id);
            puts_buf(out, "\" \"");
            put_str(out, dt->system_id);
            puts_buf(out, "\"");
        }
        puts_buf(out, ">\n");
        break;
    }
    case HK_NODE_TEXT:
        indent(out, depth);
        puts_buf(out, "\"");
        put_str(out, hk_dom_text(d, id));
        puts_buf(out, "\"\n");
        break;
    case HK_NODE_COMMENT:
        indent(out, depth);
        puts_buf(out, "<!-- ");
        put_str(out, hk_dom_text(d, id));
        puts_buf(out, " -->\n");
        break;
    case HK_NODE_ELEMENT: {
        const hk_element *e = hk_dom_element(d, id);
        indent(out, depth);
        puts_buf(out, "<");
        if (n->ns == HK_NS_SVG) puts_buf(out, "svg ");
        if (n->ns == HK_NS_MATHML) puts_buf(out, "math ");
        put_str(out, hk_atom_str(&d->atoms, e->name));
        puts_buf(out, ">\n");

        attr_line *lines = calloc(e->attr_count + 1, sizeof *lines);
        for (uint32_t i = 0; i < e->attr_count; i++) {
            const hk_attr *a = &e->attrs[i];
            static const char *const k_prefix[] = {"", "xlink ", "xml ", "xmlns "};
            hk_str name = hk_atom_str(&d->atoms, a->name);
            size_t len = strlen(k_prefix[a->ns]) + name.n + a->value.n + 3;
            lines[i].text = malloc(len + 1);
            lines[i].len = (size_t)snprintf(lines[i].text, len + 1, "%s%.*s=\"%.*s\"", k_prefix[a->ns],
                                            (int)name.n, name.p, (int)a->value.n, a->value.p);
        }
        qsort(lines, e->attr_count, sizeof *lines, attr_cmp);
        for (uint32_t i = 0; i < e->attr_count; i++) {
            indent(out, depth + 1);
            hk_charbuf_append(out, lines[i].text, lines[i].len);
            puts_buf(out, "\n");
            free(lines[i].text);
        }
        free(lines);

        if (e->template_content) {
            indent(out, depth + 1);
            puts_buf(out, "content\n");
            for (hk_node_id c = hk_dom_node(d, e->template_content)->first_child; c;
                 c = hk_dom_node(d, c)->next)
                dump_node(d, c, depth + 2, out);
        } else if (n->ns == HK_NS_HTML && e->name == HK_ATOM_template) {
            indent(out, depth + 1);
            puts_buf(out, "content\n");
        }
        break;
    }
    default: break;
    }
    if (n->type == HK_NODE_ELEMENT || n->type == HK_NODE_DOCUMENT) {
        for (hk_node_id c = n->first_child; c; c = hk_dom_node(d, c)->next)
            dump_node(d, c, n->type == HK_NODE_DOCUMENT ? depth : depth + 1, out);
    }
}

static void dump(const hk_document *d, hk_charbuf *out)
{
    out->len = 0;
    dump_node(d, d->root, 0, out);
}

/* --- .dat parsing ---------------------------------------------------------------------------- */

typedef struct test_case {
    hk_str data;
    hk_str expected;
    bool skip;
    int line;
} test_case;

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

static bool line_is(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    return (size_t)(end - p) >= n && memcmp(p, lit, n) == 0 && (p + n == end || p[n] == '\n');
}

/* Splits a .dat file into test cases. */
static size_t parse_dat(char *buf, size_t len, test_case *out, size_t cap)
{
    const char *end = buf + len;
    const char *p = buf;
    size_t count = 0;
    int line = 1;
    while (p < end && count < cap) {
        if (!line_is(p, end, "#data")) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            p = nl ? nl + 1 : end;
            line++;
            continue;
        }
        test_case tc = {0};
        tc.line = line;
        p += 6;
        line++;
        const char *data_start = p, *data_end = p;
        const char *doc_start = NULL;
        /* Walk the sections. */
        while (p < end) {
            if (*p == '#' && p != data_start && (line_is(p, end, "#errors") || line_is(p, end, "#new-errors") ||
                                                 line_is(p, end, "#document") ||
                                                 line_is(p, end, "#document-fragment") ||
                                                 line_is(p, end, "#script-on") ||
                                                 line_is(p, end, "#script-off") ||
                                                 line_is(p, end, "#data"))) {
                if (!data_end || data_end == data_start) data_end = p;
                if (line_is(p, end, "#data")) break;
                if (line_is(p, end, "#document-fragment") || line_is(p, end, "#script-on")) tc.skip = true;
                if (line_is(p, end, "#document")) {
                    const char *nl = memchr(p, '\n', (size_t)(end - p));
                    doc_start = nl ? nl + 1 : end;
                    /* The document runs until the next "#data" line. */
                    const char *q = doc_start;
                    while (q < end && !line_is(q, end, "#data")) {
                        const char *n2 = memchr(q, '\n', (size_t)(end - q));
                        q = n2 ? n2 + 1 : end;
                        line++;
                    }
                    size_t dl = (size_t)(q - doc_start);
                    /* Drop the blank separator line. */
                    while (dl >= 2 && doc_start[dl - 1] == '\n' && doc_start[dl - 2] == '\n') dl--;
                    tc.expected = (hk_str){doc_start, dl};
                    p = q;
                    break;
                }
            }
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            p = nl ? nl + 1 : end;
            line++;
        }
        /* The data is everything up to the newline before "#errors". */
        size_t n = (size_t)(data_end - data_start);
        if (n && data_start[n - 1] == '\n') n--;
        tc.data = (hk_str){data_start, n};
        if (!doc_start) tc.skip = true;
        out[count++] = tc;
    }
    return count;
}

/* --- running ------------------------------------------------------------------------------------ */

typedef struct stats {
    int passed, failed, skipped, shown;
} stats;

static void parse_bytewise(hk_str data, hk_charbuf *out)
{
    hk_document *doc = hk_document_create();
    hk_html_parser *p = hk_html_parser_create(doc);
    for (size_t i = 0; i < data.n; i++) hk_html_parser_feed(p, data.p + i, 1);
    hk_html_parser_finish(p);
    hk_html_parser_destroy(p);
    dump(doc, out);
    hk_document_destroy(doc);
}

static void run_file(const char *dir, const char *name, stats *st, bool verbose)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t len;
    char *buf = read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", path);
        st->failed++;
        return;
    }
    static test_case cases[4096];
    size_t n = parse_dat(buf, len, cases, 4096);
    hk_charbuf got = {0}, got_bytewise = {0};
    int file_failed = 0;
    for (size_t i = 0; i < n; i++) {
        test_case *tc = &cases[i];
        if (tc->skip) {
            st->skipped++;
            continue;
        }
        hk_document *doc = hk_html_parse(tc->data.p, tc->data.n);
        dump(doc, &got);
        hk_document_destroy(doc);
        parse_bytewise(tc->data, &got_bytewise);

        bool ok = got.len == tc->expected.n && memcmp(got.data, tc->expected.p, got.len) == 0;
        bool same = got.len == got_bytewise.len && memcmp(got.data, got_bytewise.data, got.len) == 0;
        if (ok && same) {
            st->passed++;
            continue;
        }
        st->failed++;
        file_failed++;
        if (verbose || st->shown < 15) {
            st->shown++;
            printf("FAIL %s:%d%s\n--- input\n%.*s\n--- expected\n%.*s--- got\n%.*s\n", name, tc->line,
                   ok ? " (byte-wise parse differs)" : "", (int)tc->data.n, tc->data.p,
                   (int)tc->expected.n, tc->expected.p, (int)got.len, got.data);
        }
    }
    printf("%-32s %3zu tests, %d failed\n", name, n, file_failed);
    hk_vec_free(&got);
    hk_vec_free(&got_bytewise);
    free(buf);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tree-construction dir> [-v]\n", argv[0]);
        return 2;
    }
    bool verbose = argc > 2 && strcmp(argv[2], "-v") == 0;
    hk_names_init();
    int64_t heap_before = hk_mem_heap_bytes();
    stats st = {0};
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*.dat", argv[1]);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "no .dat files in %s\n", argv[1]);
        return 2;
    }
    do {
        run_file(argv[1], fd.cFileName, &st, verbose);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    int64_t leaked = hk_mem_heap_bytes() - heap_before;
    printf("\n%d passed, %d failed, %d skipped (scripting on / fragment)\n", st.passed, st.failed,
           st.skipped);
    if (leaked) printf("LEAK: %lld bytes\n", (long long)leaked);
    return st.failed || leaked ? 1 : 0;
}
