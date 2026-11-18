/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "blocker/hk_regex.h"
#include "base/hk_mem.h"
#include "base/hk_vec.h"

#include <string.h>

/* The pattern is parsed into a small syntax tree, which is then compiled to a program
   for the Pike VM. Counted repetition is expanded by emitting the operand repeatedly. */

enum { MAX_PROG = 4096, MAX_NODES = 4096, MAX_COUNT = 32 };

typedef enum { OP_CHAR, OP_ANY, OP_CLASS, OP_SPLIT, OP_JMP, OP_BOL, OP_EOL, OP_WORDB, OP_NWORDB, OP_MATCH } opcode;

typedef struct inst {
    uint8_t op;
    uint8_t c;    /* OP_CHAR */
    uint16_t cls; /* OP_CLASS: index into classes */
    int32_t x, y; /* jump targets (OP_SPLIT prefers x) */
} inst;

typedef struct cls_set {
    uint8_t bits[32];
} cls_set;

typedef HK_VEC(inst) inst_vec;
typedef HK_VEC(cls_set) cls_vec;

struct hk_regex {
    inst_vec prog;
    cls_vec classes;
    bool icase;
};

/* --- syntax tree ---------------------------------------------------------------------------- */

typedef enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_CAT, N_ALT, N_REP } ntype;

typedef struct node {
    uint8_t type;
    uint8_t c;
    uint16_t cls;
    int32_t a, b;     /* children */
    int16_t min, max; /* N_REP; max < 0 = unbounded */
} node;

typedef HK_VEC(node) node_vec;

typedef struct parser {
    const char *p, *end;
    hk_regex *re;
    node_vec nodes;
    bool icase;
    bool bad;
    int depth;
} parser;

static int new_node(parser *ps, node n)
{
    if (ps->nodes.len >= MAX_NODES) {
        ps->bad = true;
        return 0;
    }
    hk_vec_push(&ps->nodes, n);
    return (int)ps->nodes.len - 1;
}

static int leaf(parser *ps, ntype t, int c, int cls)
{
    node n = {(uint8_t)t, (uint8_t)c, (uint16_t)cls, -1, -1, 0, 0};
    return new_node(ps, n);
}

static int pair(parser *ps, ntype t, int a, int b)
{
    node n = {(uint8_t)t, 0, 0, a, b, 0, 0};
    return new_node(ps, n);
}

static void set_bit(cls_set *c, int b)
{
    c->bits[b >> 3] |= (uint8_t)(1u << (b & 7));
}

static void add_range(cls_set *c, int lo, int hi, bool icase)
{
    for (int b = lo; b <= hi; b++) {
        set_bit(c, b);
        if (icase && b >= 'a' && b <= 'z') set_bit(c, b - 32);
        if (icase && b >= 'A' && b <= 'Z') set_bit(c, b + 32);
    }
}

static void add_escape_class(cls_set *c, char e)
{
    char lower = (char)(e | 0x20);
    cls_set t = {0};
    if (lower == 'd') {
        add_range(&t, '0', '9', false);
    } else if (lower == 'w') {
        add_range(&t, '0', '9', false);
        add_range(&t, 'a', 'z', false);
        add_range(&t, 'A', 'Z', false);
        set_bit(&t, '_');
    } else {
        for (const char *ws = " \t\n\r\f\v"; *ws; ws++) set_bit(&t, (unsigned char)*ws);
    }
    bool negate = e >= 'A' && e <= 'Z';
    for (int i = 0; i < 32; i++) c->bits[i] |= negate ? (uint8_t)~t.bits[i] : t.bits[i];
}

static int class_node(parser *ps, const cls_set *c)
{
    if (ps->re->classes.len >= 0xFFFF) {
        ps->bad = true;
        return 0;
    }
    hk_vec_push(&ps->re->classes, *c);
    return leaf(ps, N_CLASS, 0, (int)ps->re->classes.len - 1);
}

static int peek(const parser *ps)
{
    return ps->p < ps->end ? (unsigned char)*ps->p : -1;
}

static int escaped_char(char e)
{
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    default: return (unsigned char)e;
    }
}

static bool is_shorthand(char e)
{
    return e && strchr("dDwWsS", e) != NULL;
}

static int parse_alt(parser *ps);

static int parse_class(parser *ps)
{
    cls_set set = {0};
    bool negate = false;
    if (peek(ps) == '^') {
        negate = true;
        ps->p++;
    }
    bool first = true;
    while (ps->p < ps->end && (*ps->p != ']' || first)) {
        first = false;
        int lo = (unsigned char)*ps->p++;
        if (lo == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            if (is_shorthand(e)) {
                add_escape_class(&set, e);
                continue;
            }
            lo = escaped_char(e);
        }
        int hi = lo;
        if (ps->p + 1 < ps->end && *ps->p == '-' && ps->p[1] != ']') {
            ps->p++;
            hi = (unsigned char)*ps->p++;
            if (hi == '\\') {
                if (ps->p >= ps->end) break;
                char e = *ps->p++;
                if (is_shorthand(e)) {
                    ps->bad = true;
                    return 0;
                }
                hi = escaped_char(e);
            }
        }
        if (hi < lo) {
            ps->bad = true;
            return 0;
        }
        add_range(&set, lo, hi, ps->icase);
    }
    if (peek(ps) != ']') {
        ps->bad = true;
        return 0;
    }
    ps->p++;
    if (negate) {
        for (int i = 0; i < 32; i++) set.bits[i] = (uint8_t)~set.bits[i];
    }
    return class_node(ps, &set);
}

static int parse_atom(parser *ps)
{
    int c = peek(ps);
    if (c == '(') {
        ps->p++;
        if (peek(ps) == '?') {
            if (ps->p + 1 < ps->end && ps->p[1] == ':') {
                ps->p += 2;
            } else {
                ps->bad = true; /* look-around, named groups and inline flags */
                return 0;
            }
        }
        if (++ps->depth > 64) {
            ps->bad = true;
            return 0;
        }
        int inner = parse_alt(ps);
        ps->depth--;
        if (peek(ps) != ')') {
            ps->bad = true;
            return 0;
        }
        ps->p++;
        return inner;
    }
    if (c == '[') {
        ps->p++;
        return parse_class(ps);
    }
    ps->p++;
    switch (c) {
    case '.': return leaf(ps, N_ANY, 0, 0);
    case '^': return leaf(ps, N_BOL, 0, 0);
    case '$': return leaf(ps, N_EOL, 0, 0);
    case '*':
    case '+':
    case '?':
    case '{':
        ps->bad = true; /* a quantifier with nothing to repeat */
        return 0;
    case '\\': {
        if (ps->p >= ps->end) {
            ps->bad = true;
            return 0;
        }
        char e = *ps->p++;
        if (is_shorthand(e)) {
            cls_set set = {0};
            add_escape_class(&set, e);
            return class_node(ps, &set);
        }
        if (e == 'b') return leaf(ps, N_WORDB, 0, 0);
        if (e == 'B') return leaf(ps, N_NWORDB, 0, 0);
        if ((e >= '0' && e <= '9') || e == 'k' || e == 'p' || e == 'P' || e == 'u' || e == 'x' || e == 'c') {
            ps->bad = true; /* back-references and escapes we do not implement */
            return 0;
        }
        c = escaped_char(e);
        break;
    }
    default: break;
    }
    /* Non-ASCII literals are matched byte by byte (the pattern and URL are both UTF-8). */
    return leaf(ps, N_CHAR, ps->icase && c >= 'A' && c <= 'Z' ? c + 32 : c, 0);
}

static bool parse_count(parser *ps, int *min, int *max)
{
    const char *q = ps->p + 1;
    int m = 0, n;
    int digits = 0;
    while (q < ps->end && *q >= '0' && *q <= '9' && m <= MAX_COUNT) m = m * 10 + (*q++ - '0'), digits++;
    if (!digits) return false;
    if (q < ps->end && *q == ',') {
        q++;
        if (q < ps->end && *q >= '0' && *q <= '9') {
            n = 0;
            while (q < ps->end && *q >= '0' && *q <= '9' && n <= MAX_COUNT) n = n * 10 + (*q++ - '0');
        } else {
            n = -1;
        }
    } else {
        n = m;
    }
    if (q >= ps->end || *q != '}') return false;
    if (m > MAX_COUNT || n > MAX_COUNT || (n >= 0 && n < m)) return false;
    ps->p = q + 1;
    *min = m;
    *max = n;
    return true;
}

static int parse_repeat(parser *ps)
{
    int a = parse_atom(ps);
    while (!ps->bad) {
        int c = peek(ps);
        int min, max;
        if (c == '*' || c == '+' || c == '?') {
            min = c == '+';
            max = c == '?' ? 1 : -1;
            ps->p++;
        } else if (c == '{') {
            if (!parse_count(ps, &min, &max)) {
                ps->bad = true;
                return a;
            }
        } else {
            break;
        }
        if (peek(ps) == '?') ps->p++; /* lazy: same language, and we only answer "is there a match" */
        node n = {N_REP, 0, 0, a, -1, (int16_t)min, (int16_t)max};
        a = new_node(ps, n);
    }
    return a;
}

static int parse_concat(parser *ps)
{
    int n = -1;
    while (!ps->bad) {
        int c = peek(ps);
        if (c < 0 || c == '|' || c == ')') break;
        int r = parse_repeat(ps);
        n = n < 0 ? r : pair(ps, N_CAT, n, r);
    }
    return n < 0 ? leaf(ps, N_EMPTY, 0, 0) : n;
}

static int parse_alt(parser *ps)
{
    int n = parse_concat(ps);
    while (!ps->bad && peek(ps) == '|') {
        ps->p++;
        int r = parse_concat(ps);
        n = pair(ps, N_ALT, n, r);
    }
    return n;
}

/* --- code generation ------------------------------------------------------------------------ */

static int emit(parser *ps, opcode op, int c, int cls)
{
    if (ps->re->prog.len >= MAX_PROG) {
        ps->bad = true;
        return 0;
    }
    inst in = {(uint8_t)op, (uint8_t)c, (uint16_t)cls, 0, 0};
    hk_vec_push(&ps->re->prog, in);
    return (int)ps->re->prog.len - 1;
}

static int here(const parser *ps)
{
    return (int)ps->re->prog.len;
}

static void gen(parser *ps, int ni, int depth)
{
    if (ps->bad) return;
    if (depth > 200) {
        ps->bad = true;
        return;
    }
    node n = ps->nodes.data[ni];
    switch ((ntype)n.type) {
    case N_EMPTY: break;
    case N_CHAR: emit(ps, OP_CHAR, n.c, 0); break;
    case N_ANY: emit(ps, OP_ANY, 0, 0); break;
    case N_CLASS: emit(ps, OP_CLASS, 0, n.cls); break;
    case N_BOL: emit(ps, OP_BOL, 0, 0); break;
    case N_EOL: emit(ps, OP_EOL, 0, 0); break;
    case N_WORDB: emit(ps, OP_WORDB, 0, 0); break;
    case N_NWORDB: emit(ps, OP_NWORDB, 0, 0); break;
    case N_CAT:
        gen(ps, n.a, depth + 1);
        gen(ps, n.b, depth + 1);
        break;
    case N_ALT: {
        int s = emit(ps, OP_SPLIT, 0, 0);
        gen(ps, n.a, depth + 1);
        int j = emit(ps, OP_JMP, 0, 0);
        if (ps->bad) return;
        ps->re->prog.data[s].x = s + 1;
        ps->re->prog.data[s].y = here(ps);
        gen(ps, n.b, depth + 1);
        if (ps->bad) return;
        ps->re->prog.data[j].x = here(ps);
        break;
    }
    case N_REP:
        for (int i = 0; i < n.min && !ps->bad; i++) gen(ps, n.a, depth + 1);
        if (n.max < 0) {
            /* L: split body, out; body; jmp L; out: */
            int s = emit(ps, OP_SPLIT, 0, 0);
            gen(ps, n.a, depth + 1);
            int j = emit(ps, OP_JMP, 0, 0);
            if (ps->bad) return;
            ps->re->prog.data[j].x = s;
            ps->re->prog.data[s].x = s + 1;
            ps->re->prog.data[s].y = here(ps);
        } else {
            for (int i = n.min; i < n.max && !ps->bad; i++) {
                int s = emit(ps, OP_SPLIT, 0, 0);
                gen(ps, n.a, depth + 1);
                if (ps->bad) return;
                ps->re->prog.data[s].x = s + 1;
                ps->re->prog.data[s].y = here(ps);
            }
        }
        break;
    }
}

hk_regex *hk_regex_compile(hk_str pattern, bool icase)
{
    hk_regex *re = hk_alloc_zero(sizeof *re);
    if (hk_str_starts_with(pattern, HK_STR("(?i)"))) {
        icase = true;
        pattern = hk_str_slice(pattern, 4, pattern.n);
    }
    re->icase = icase;
    parser ps = {pattern.p, pattern.p + pattern.n, re, {0}, icase, false, 0};
    int root = parse_alt(&ps);
    if (!ps.bad && ps.p != ps.end) ps.bad = true; /* an unbalanced ")" */
    if (!ps.bad) gen(&ps, root, 0);
    if (!ps.bad) emit(&ps, OP_MATCH, 0, 0);
    hk_vec_free(&ps.nodes);
    if (ps.bad) {
        hk_regex_free(re);
        return NULL;
    }
    return re;
}

void hk_regex_free(hk_regex *re)
{
    if (!re) return;
    hk_vec_free(&re->prog);
    hk_vec_free(&re->classes);
    hk_free(re, sizeof *re);
}

/* --- Pike VM -------------------------------------------------------------------------------- */

typedef struct thread_list {
    int *pc;
    size_t *start;
    int n;
} thread_list;

typedef struct vm {
    const hk_regex *re;
    hk_str text;
    uint32_t *seen; /* generation in which each pc was last added */
} vm;

static bool word_at(hk_str t, size_t i)
{
    if (i >= t.n) return false;
    char c = t.p[i];
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static void add_thread(vm *v, thread_list *l, uint32_t gen, int pc, size_t start, size_t pos)
{
    for (;;) {
        if (v->seen[pc] == gen) return;
        v->seen[pc] = gen;
        const inst *in = &v->re->prog.data[pc];
        switch (in->op) {
        case OP_JMP: pc = in->x; continue;
        case OP_SPLIT:
            add_thread(v, l, gen, in->x, start, pos);
            pc = in->y;
            continue;
        case OP_BOL:
            if (pos != 0) return;
            pc++;
            continue;
        case OP_EOL:
            if (pos != v->text.n) return;
            pc++;
            continue;
        case OP_WORDB:
        case OP_NWORDB: {
            bool boundary = (pos > 0 && word_at(v->text, pos - 1)) != word_at(v->text, pos);
            if (boundary != (in->op == OP_WORDB)) return;
            pc++;
            continue;
        }
        default:
            l->pc[l->n] = pc;
            l->start[l->n] = start;
            l->n++;
            return;
        }
    }
}

bool hk_regex_find(const hk_regex *re, hk_str text, size_t *start_out, size_t *end_out)
{
    size_t n = re->prog.len;
    int *pcs = hk_alloc(n * 2 * sizeof(int));
    size_t *starts = hk_alloc(n * 2 * sizeof(size_t));
    uint32_t *seen = hk_alloc_zero(n * sizeof(uint32_t));
    vm v = {re, text, seen};
    thread_list cur = {pcs, starts, 0}, next = {pcs + n, starts + n, 0};
    uint32_t gen = 1;
    bool matched = false;
    size_t mstart = 0, mend = 0;
    add_thread(&v, &cur, gen, 0, 0, 0);
    for (size_t pos = 0;; pos++) {
        uint32_t ngen = gen + 1;
        next.n = 0;
        int c = pos < text.n ? (unsigned char)text.p[pos] : -1;
        int lc = re->icase && c >= 'A' && c <= 'Z' ? c + 32 : c;
        for (int i = 0; i < cur.n; i++) {
            const inst *in = &re->prog.data[cur.pc[i]];
            bool ok = false;
            switch (in->op) {
            case OP_MATCH:
                /* Threads are in priority order; everything after this one has lower priority. */
                matched = true;
                mstart = cur.start[i];
                mend = pos;
                cur.n = i + 1;
                continue;
            case OP_CHAR: ok = lc == in->c; break;
            case OP_ANY: ok = c >= 0 && c != '\n'; break;
            case OP_CLASS: ok = c >= 0 && (re->classes.data[in->cls].bits[c >> 3] & (1u << (c & 7))); break;
            default: break;
            }
            if (ok) add_thread(&v, &next, ngen, cur.pc[i] + 1, cur.start[i], pos + 1);
        }
        if (pos >= text.n) break;
        /* Until something matches, a new attempt starts at every position (lowest priority). */
        if (!matched) add_thread(&v, &next, ngen, 0, pos + 1, pos + 1);
        thread_list t = cur;
        cur = next;
        next = t;
        gen = ngen;
        if (cur.n == 0) break;
    }
    hk_free(pcs, n * 2 * sizeof(int));
    hk_free(starts, n * 2 * sizeof(size_t));
    hk_free(seen, n * sizeof(uint32_t));
    if (matched) {
        if (start_out) *start_out = mstart;
        if (end_out) *end_out = mend;
    }
    return matched;
}

bool hk_regex_search(const hk_regex *re, hk_str text)
{
    return hk_regex_find(re, text, NULL, NULL);
}
