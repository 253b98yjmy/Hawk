/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Network filter matching over a compiled image (docs/design/04-blocker.md, 3). */
#include "blocker/hk_blocker.h"
#include "blocker/hk_filter_image.h"
#include "blocker/hk_regex.h"
#include "base/hk_map64.h"
#include "base/hk_mem.h"
#include "net/hk_psl.h"

#include <string.h>

struct hk_blocker {
    const char *str;
    uint32_t str_len;
    const img_filter *filters;
    uint32_t nfilters;
    const uint64_t *domains;
    uint32_t ndomains;
    const img_bucket *hostidx;
    uint32_t hostcap;
    const img_bucket *tokidx;
    uint32_t tokcap;
    const uint32_t *post;
    uint32_t npost;
    const uint32_t *notok;
    uint32_t nnotok;
    const uint32_t *param;
    uint32_t nparam;
    const img_span *lists;
    uint32_t nlists;
    hk_map64 regex; /* 2*filter+1: pattern, 2*filter+2: removeparam value -> hk_regex * */
    uint16_t *notok_gram; /* per NOTOK filter: a trigram every match contains, or NO_GRAM */
    hk_charbuf owned;
};

/* Untokenized filters (category C) are checked for every request. A trigram that each one
   requires is tested against a per-request Bloom filter of the URL's trigrams first. */
enum { GRAM_BITS = 4096, NO_GRAM = 0xFFFF };

static uint32_t gram_of(const char *p)
{
    uint32_t h = (uint8_t)hk_ascii_lower(p[0]) * 961u + (uint8_t)hk_ascii_lower(p[1]) * 31u +
                 (uint8_t)hk_ascii_lower(p[2]);
    return (h * 2654435761u) >> 20; /* 12 bits */
}

/* --- opening ------------------------------------------------------------------------------- */

static bool section(const img_header *h, size_t n, int s, size_t elem, uint32_t *count)
{
    uint64_t off = h->sec[s].off, len = h->sec[s].len;
    if (off % 8 || off + len > n || off < sizeof *h || len % elem) return false;
    *count = (uint32_t)(len / elem);
    return true;
}

uint64_t hk_blocker_image_stamp(const void *image, size_t n)
{
    if (n < sizeof(img_header)) return 0;
    const img_header *h = image;
    if (memcmp(h->magic, HK_FILTER_MAGIC, 8) != 0 || h->version != HK_FILTER_VERSION || h->nsec != SEC_COUNT)
        return 0;
    return h->stamp;
}

static bool span_ok(const hk_blocker *b, uint32_t off, uint32_t len)
{
    return (uint64_t)off + len <= b->str_len;
}

static bool index_ok(const hk_blocker *b, const img_bucket *t, uint32_t cap)
{
    if (cap == 0 || (cap & (cap - 1))) return false;
    bool empty = false;
    for (uint32_t i = 0; i < cap; i++) {
        if (!t[i].key) {
            empty = true;
            continue;
        }
        if ((uint64_t)t[i].start + t[i].count > b->npost) return false;
    }
    return empty; /* lookups stop at an empty bucket */
}

/* A pattern regex with a literal that every match contains: checking for the literal first
   avoids running the VM over almost every URL. */
typedef struct regex_entry {
    hk_regex *re;
    uint8_t literal_n;
    char literal[47];
} regex_entry;

/* The longest run of literal characters outside groups, or nothing when the pattern has a
   top-level alternation. Lowercase (used only for case-insensitive patterns). */
static void required_literal(hk_str pat, regex_entry *e)
{
    char best[sizeof e->literal], cur[sizeof e->literal];
    size_t best_n = 0, cur_n = 0;
    int depth = 0;
    bool last_literal = false; /* the previous atom is the last char of cur */
#define END_RUN()                                                                                  \
    do {                                                                                           \
        if (cur_n > best_n) memcpy(best, cur, best_n = cur_n);                                     \
        cur_n = 0;                                                                                 \
        last_literal = false;                                                                      \
    } while (0)
    for (size_t i = 0; i < pat.n; i++) {
        char c = pat.p[i];
        int lit = -1;
        if (c == '\\' && i + 1 < pat.n) {
            char x = pat.p[++i];
            bool alnum = (x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z') || (x >= '0' && x <= '9');
            if (!alnum) lit = (unsigned char)x;
        } else if (c == '[') {
            END_RUN();
            for (i++; i < pat.n && pat.p[i] != ']'; i++)
                if (pat.p[i] == '\\') i++;
            continue;
        } else if (c == '(' || c == ')') {
            depth += c == '(' ? 1 : -1;
            END_RUN();
            continue;
        } else if (c == '|') {
            if (depth == 0) {
                e->literal_n = 0;
                return;
            }
            END_RUN();
            continue;
        } else if (c == '*' || c == '?' || c == '{' || c == '+') {
            bool optional = c == '*' || c == '?' || (c == '{' && i + 1 < pat.n && pat.p[i + 1] == '0');
            if (optional && last_literal && cur_n) cur_n--; /* the repeated char may be absent */
            END_RUN();
            if (c == '{')
                while (i < pat.n && pat.p[i] != '}') i++;
            continue;
        } else if (c != '.' && c != '^' && c != '$') {
            lit = (unsigned char)c;
        }
        if (lit < 0 || depth > 0) {
            END_RUN();
            continue;
        }
        if (cur_n == sizeof cur) END_RUN();
        cur[cur_n++] = hk_ascii_lower((char)lit);
        last_literal = true;
    }
    END_RUN();
#undef END_RUN
    memcpy(e->literal, best, best_n);
    e->literal_n = (uint8_t)(best_n >= 3 ? best_n : 0);
}

static bool compile_regexes(hk_blocker *b)
{
    for (uint32_t i = 0; i < b->nfilters; i++) {
        const img_filter *f = &b->filters[i];
        if (f->flags & F_REGEX) {
            hk_str pat = {b->str + f->pat_off, f->pat_len};
            hk_regex *re = hk_regex_compile(pat, !(f->flags & F_MATCH_CASE));
            if (!re) return false;
            regex_entry *e = hk_alloc_zero(sizeof *e);
            e->re = re;
            if (!(f->flags & F_MATCH_CASE)) required_literal(pat, e);
            hk_map64_put(&b->regex, 2ull * i + 1, (uint64_t)(uintptr_t)e);
        }
        if (f->flags & F_PARAM_REGEX) {
            hk_str v = {b->str + f->param_off, f->param_len};
            size_t close = v.n;
            while (close > 1 && v.p[close - 1] != '/') close--;
            if (close <= 1) return false;
            hk_str flags = hk_str_slice(v, close, v.n);
            hk_regex *re = hk_regex_compile(hk_str_slice(v, 1, close - 1), hk_str_find_char(flags, 'i') != HK_STR_NPOS);
            if (!re) return false;
            hk_map64_put(&b->regex, 2ull * i + 2, (uint64_t)(uintptr_t)re);
        }
    }
    return true;
}

static uint16_t required_gram(const hk_blocker *b, uint32_t idx)
{
    const img_filter *f = &b->filters[idx];
    if (f->flags & F_REGEX) {
        uint64_t v;
        if (!hk_map64_get(&b->regex, 2ull * idx + 1, &v)) return NO_GRAM;
        const regex_entry *e = (const regex_entry *)(uintptr_t)v;
        return e->literal_n >= 3 ? (uint16_t)gram_of(e->literal) : NO_GRAM;
    }
    /* The longest literal segment between wildcards and separators. */
    const char *p = b->str + f->pat_off;
    size_t n = f->pat_len, best = 0, best_n = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && p[j] != '*' && p[j] != '^') j++;
        if (j - i > best_n) best = i, best_n = j - i;
        i = j + 1;
    }
    return best_n >= 3 ? (uint16_t)gram_of(p + best) : NO_GRAM;
}

hk_blocker *hk_blocker_open(const void *image, size_t n)
{
    if (!hk_blocker_image_stamp(image, n) || ((uintptr_t)image % 8) || n > UINT32_MAX) return NULL;
    const img_header *h = image;
    const char *base = image;
    hk_blocker *b = hk_alloc_zero(sizeof *b);
    hk_map64_init(&b->regex);
    bool ok = section(h, n, SEC_STR, 1, &b->str_len) && section(h, n, SEC_FILTERS, sizeof(img_filter), &b->nfilters) &&
              section(h, n, SEC_DOMAINS, sizeof(uint64_t), &b->ndomains) &&
              section(h, n, SEC_HOSTIDX, sizeof(img_bucket), &b->hostcap) &&
              section(h, n, SEC_TOKIDX, sizeof(img_bucket), &b->tokcap) &&
              section(h, n, SEC_POST, sizeof(uint32_t), &b->npost) &&
              section(h, n, SEC_NOTOK, sizeof(uint32_t), &b->nnotok) &&
              section(h, n, SEC_PARAM, sizeof(uint32_t), &b->nparam) &&
              section(h, n, SEC_LISTS, sizeof(img_span), &b->nlists);
    if (ok) {
        b->str = base + h->sec[SEC_STR].off;
        b->filters = (const img_filter *)(base + h->sec[SEC_FILTERS].off);
        b->domains = (const uint64_t *)(base + h->sec[SEC_DOMAINS].off);
        b->hostidx = (const img_bucket *)(base + h->sec[SEC_HOSTIDX].off);
        b->tokidx = (const img_bucket *)(base + h->sec[SEC_TOKIDX].off);
        b->post = (const uint32_t *)(base + h->sec[SEC_POST].off);
        b->notok = (const uint32_t *)(base + h->sec[SEC_NOTOK].off);
        b->param = (const uint32_t *)(base + h->sec[SEC_PARAM].off);
        b->lists = (const img_span *)(base + h->sec[SEC_LISTS].off);
        ok = index_ok(b, b->hostidx, b->hostcap) && index_ok(b, b->tokidx, b->tokcap);
    }
    for (uint32_t i = 0; ok && i < b->nfilters; i++) {
        const img_filter *f = &b->filters[i];
        ok = span_ok(b, f->pat_off, f->pat_len) && span_ok(b, f->param_off, f->param_len) &&
             (uint64_t)f->from_off + f->from_n <= b->ndomains && (uint64_t)f->to_off + f->to_n <= b->ndomains &&
             (f->source >> 20) < b->nlists;
    }
    for (uint32_t i = 0; ok && i < b->npost; i++) ok = b->post[i] < b->nfilters;
    for (uint32_t i = 0; ok && i < b->nnotok; i++) ok = b->notok[i] < b->nfilters;
    for (uint32_t i = 0; ok && i < b->nparam; i++) ok = b->param[i] < b->nfilters;
    for (uint32_t i = 0; ok && i < b->nlists; i++) ok = span_ok(b, b->lists[i].off, b->lists[i].len);
    if (ok) ok = compile_regexes(b);
    if (ok) {
        b->notok_gram = hk_alloc((size_t)(b->nnotok ? b->nnotok : 1) * sizeof(uint16_t));
        for (uint32_t i = 0; i < b->nnotok; i++) b->notok_gram[i] = required_gram(b, b->notok[i]);
    }
    if (!ok) {
        hk_blocker_close(b);
        return NULL;
    }
    return b;
}

void hk_blocker_close(hk_blocker *b)
{
    if (!b) return;
    for (uint32_t i = 0; i < b->regex.cap; i++)
    {
        if (!b->regex.keys[i]) continue;
        if (b->regex.keys[i] & 1) { /* 2 * filter + 1: a pattern */
            regex_entry *e = (regex_entry *)(uintptr_t)b->regex.vals[i];
            hk_regex_free(e->re);
            hk_free(e, sizeof *e);
        } else {
            hk_regex_free((hk_regex *)(uintptr_t)b->regex.vals[i]);
        }
    }
    hk_map64_free(&b->regex);
    if (b->notok_gram) hk_free(b->notok_gram, (size_t)(b->nnotok ? b->nnotok : 1) * sizeof(uint16_t));
    hk_vec_free(&b->owned);
    hk_free(b, sizeof *b);
}

hk_blocker *hk_blocker_from_text(hk_str text, hk_filter_stats *stats)
{
    hk_filter_compiler *c = hk_filter_compiler_create();
    hk_filter_compiler_add_list(c, HK_STR("text"), text);
    hk_charbuf img = {0};
    hk_filter_compiler_finish(c, 1, &img, stats);
    hk_blocker *b = hk_blocker_open(img.data, img.len);
    if (b) b->owned = img;
    else hk_vec_free(&img);
    return b;
}

uint32_t hk_blocker_count(const hk_blocker *b)
{
    return b ? b->nfilters : 0;
}

hk_str hk_blocker_filter_list(const hk_blocker *b, int32_t filter)
{
    if (!b || filter < 0 || (uint32_t)filter >= b->nfilters) return (hk_str){0};
    const img_span *s = &b->lists[b->filters[filter].source >> 20];
    return (hk_str){b->str + s->off, s->len};
}

uint32_t hk_blocker_filter_line(const hk_blocker *b, int32_t filter)
{
    if (!b || filter < 0 || (uint32_t)filter >= b->nfilters) return 0;
    return b->filters[filter].source & 0xFFFFF;
}

/* --- request context ----------------------------------------------------------------------- */

/* Hashes of a host's label suffixes ("a.b.example.com", "b.example.com", ...) and of their
   entity forms ("b.example.*"), as used by $domain= and $to=. Computed once per request. */
enum { MAX_LEVELS = 24 };

typedef struct host_levels {
    uint64_t h[MAX_LEVELS];
    int n;
    bool done;
} host_levels;

typedef struct ctx {
    const hk_blocker *b;
    hk_str url;  /* lowercase, without the fragment */
    hk_str orig; /* original case, without the fragment */
    size_t host_start, host_end;
    hk_str host, site_host;
    uint32_t type;
    bool third_party;
    host_levels host_lv, site_lv; /* filled on first use */
} ctx;

/* Lowercased URLs up to this size are kept on the stack. */
enum { URL_STACK = 2048 };

static bool is_token_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '%';
}

static bool is_separator(char c)
{
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
             c == '.' || c == '%');
}

static void locate_host(ctx *x)
{
    hk_str u = x->url;
    size_t p = hk_str_find(u, HK_STR("://"));
    p = p == HK_STR_NPOS ? 0 : p + 3;
    size_t end = p;
    while (end < u.n && u.p[end] != '/' && u.p[end] != '?' && u.p[end] != '#') end++;
    /* userinfo */
    for (size_t i = end; i > p; i--) {
        if (u.p[i - 1] == '@') {
            p = i;
            break;
        }
    }
    size_t he = p;
    if (he < end && u.p[he] == '[') {
        while (he < end && u.p[he] != ']') he++;
        if (he < end) he++;
    } else {
        while (he < end && u.p[he] != ':') he++;
    }
    x->host_start = p;
    x->host_end = he;
}

/* stack holds URL_STACK bytes; longer URLs go to heap. */
static void ctx_init(ctx *x, const hk_blocker *b, const hk_block_request *r, char *stack, hk_charbuf *heap)
{
    x->b = b;
    x->host_lv.done = x->site_lv.done = false;
    hk_str u = r->url;
    size_t hash = hk_str_find_char(u, '#');
    if (hash != HK_STR_NPOS) u.n = hash;
    char *low = stack;
    if (u.n > URL_STACK) {
        hk_vec_reserve(heap, u.n);
        low = heap->data;
    }
    for (size_t i = 0; i < u.n; i++) low[i] = hk_ascii_lower(u.p[i]);
    x->url = (hk_str){low, u.n};
    x->orig = u;
    locate_host(x);
    x->host = r->host;
    if (x->host.n && x->host.p[x->host.n - 1] == '.') x->host.n--;
    x->site_host = r->site_url.n ? r->site_host : x->host;
    if (x->site_host.n && x->site_host.p[x->site_host.n - 1] == '.') x->site_host.n--;
    x->type = r->type;
    x->third_party = r->site_url.n && !hk_psl_same_site(x->host, x->site_host);
}

/* --- options ------------------------------------------------------------------------------- */

static void compute_levels(host_levels *lv, hk_str host)
{
    lv->n = 0;
    lv->done = true;
    hk_str suffix = hk_psl_public_suffix(host);
    for (hk_str level = host; level.n && lv->n + 2 <= MAX_LEVELS;) {
        lv->h[lv->n++] = hk_str_hash64(level) & ~1ull;
        if (suffix.n && level.n > suffix.n + 1 && level.p[level.n - suffix.n - 1] == '.') {
            char ent[260];
            size_t k = level.n - suffix.n;
            if (k + 1 <= sizeof ent) {
                memcpy(ent, level.p, k);
                ent[k] = '*';
                lv->h[lv->n++] = hk_hash64(ent, k + 1) & ~1ull;
            }
        }
        size_t dot = hk_str_find_char(level, '.');
        if (dot == HK_STR_NPOS) break;
        level = hk_str_slice(level, dot + 1, level.n);
    }
}

/* Does the host satisfy a $domain= style list? A negated entry that matches any level wins. */
static bool domains_match(const hk_blocker *b, uint32_t off, uint32_t n, host_levels *lv, hk_str host)
{
    if (n == 0) return true;
    if (!lv->done) compute_levels(lv, host);
    const uint64_t *list = b->domains + off;
    bool any_pos = false, pos_hit = false;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t e = list[i];
        bool neg = e & 1;
        any_pos |= !neg;
        uint64_t h = e & ~1ull;
        for (int k = 0; k < lv->n; k++) {
            if (lv->h[k] != h) continue;
            if (neg) return false;
            pos_hit = true;
        }
    }
    return any_pos ? pos_hit : true;
}

/* --- patterns ------------------------------------------------------------------------------ */

/* Glob match of pat (literals, "*", "^") against s from position si. Backtracks only to the
   last "*", so the cost is O(|pat| * |s|) at worst. */
static bool glob_at(hk_str pat, hk_str s, size_t si, bool end_anchor)
{
    size_t pi = 0, star_p = SIZE_MAX, star_s = 0;
    for (;;) {
        if (pi < pat.n && pat.p[pi] == '*') {
            star_p = ++pi;
            star_s = si;
            continue;
        }
        if (pi == pat.n) {
            if (!end_anchor || si == s.n) return true;
        } else if (si < s.n && (pat.p[pi] == s.p[si] || (pat.p[pi] == '^' && is_separator(s.p[si])))) {
            pi++;
            si++;
            continue;
        } else if (si == s.n && pat.p[pi] == '^') {
            pi++; /* "^" also matches the end of the URL */
            continue;
        }
        if (star_p == SIZE_MAX || star_s >= s.n) return false;
        si = ++star_s;
        pi = star_p;
    }
}

static bool pattern_matches(const ctx *x, uint32_t idx, const img_filter *f)
{
    if (f->flags & F_REGEX) {
        uint64_t v;
        if (!hk_map64_get(&x->b->regex, 2ull * idx + 1, &v)) return false;
        const regex_entry *e = (const regex_entry *)(uintptr_t)v;
        if (e->literal_n && hk_str_find(x->url, (hk_str){e->literal, e->literal_n}) == HK_STR_NPOS) return false;
        return hk_regex_search(e->re, x->orig);
    }
    hk_str pat = {x->b->str + f->pat_off, f->pat_len};
    hk_str s = (f->flags & F_MATCH_CASE) ? x->orig : x->url;
    bool end = (f->flags & F_END) != 0;
    if (pat.n == 0) return true;
    if (f->flags & F_START) return glob_at(pat, s, 0, end);
    if (f->flags & F_HOST) {
        for (size_t i = x->host_start; i < x->host_end; i++) {
            if (i != x->host_start && s.p[i - 1] != '.') continue;
            if (glob_at(pat, s, i, end)) return true;
        }
        return false;
    }
    char first = pat.p[0];
    if (first == '^') {
        for (size_t i = 0; i <= s.n; i++)
            if (glob_at(pat, s, i, end)) return true;
        return false;
    }
    /* Candidate starts are the occurrences of the leading literal segment. */
    size_t lit = 0;
    while (lit < pat.n && pat.p[lit] != '*' && pat.p[lit] != '^') lit++;
    hk_str seg = {pat.p, lit};
    for (size_t i = 0; i + lit <= s.n;) {
        const char *hit = memchr(s.p + i, first, s.n - i);
        if (!hit) return false;
        i = (size_t)(hit - s.p);
        if (i + lit > s.n) return false;
        if (memcmp(s.p + i, seg.p, lit) == 0 && glob_at(pat, s, i, end)) return true;
        i++;
    }
    return false;
}

static bool filter_applies(ctx *x, uint32_t idx)
{
    const img_filter *f = &x->b->filters[idx];
    if (!(img_filter_types(f) & x->type)) return false;
    if ((f->flags & F_3P) && !x->third_party) return false;
    if ((f->flags & F_1P) && x->third_party) return false;
    if (f->from_n && !domains_match(x->b, f->from_off, f->from_n, &x->site_lv, x->site_host)) return false;
    if (f->to_n && !domains_match(x->b, f->to_off, f->to_n, &x->host_lv, x->host)) return false;
    return pattern_matches(x, idx, f);
}

/* --- candidates ---------------------------------------------------------------------------- */

typedef struct scan {
    uint32_t want;    /* filters must have all of these flags... */
    uint32_t reject;  /* ...and none of these */
    int32_t found;
    bool stop_on_first;
} scan;

static bool visit(ctx *x, scan *s, uint32_t idx)
{
    uint32_t flags = x->b->filters[idx].flags;
    if ((flags & s->want) != s->want || (flags & s->reject)) return false;
    if (s->found >= 0 && !(flags & F_IMPORTANT)) return false; /* only $important can still change the answer */
    if (!filter_applies(x, idx)) return false;
    if (s->found < 0 || (flags & F_IMPORTANT)) s->found = (int32_t)idx;
    return s->stop_on_first || (flags & F_IMPORTANT);
}

static bool visit_bucket(ctx *x, scan *s, const img_bucket *t, uint32_t cap, uint64_t key)
{
    for (uint32_t i = (uint32_t)key & (cap - 1);; i = (i + 1) & (cap - 1)) {
        if (!t[i].key) return false;
        if (t[i].key != key) continue;
        for (uint32_t k = 0; k < t[i].count; k++)
            if (visit(x, s, x->b->post[t[i].start + k])) return true;
        return false;
    }
}

static uint64_t token_hash(const char *p, size_t n)
{
    if (n > 64) n = 64;
    return hk_key64_nonzero(hk_hash64(p, n));
}

/* Visits every filter that could match; returns true when the scan stopped early. */
static bool scan_candidates(ctx *x, scan *s)
{
    const hk_blocker *b = x->b;
    for (hk_str level = x->host; level.n;) {
        if (visit_bucket(x, s, b->hostidx, b->hostcap, hk_key64_nonzero(hk_str_hash64(level)))) return true;
        size_t dot = hk_str_find_char(level, '.');
        if (dot == HK_STR_NPOS) break;
        level = hk_str_slice(level, dot + 1, level.n);
    }
    hk_str u = x->url;
    for (size_t i = 0; i < u.n;) {
        if (!is_token_char(u.p[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < u.n && is_token_char(u.p[j])) j++;
        if (j - i >= 2 && visit_bucket(x, s, b->tokidx, b->tokcap, token_hash(u.p + i, j - i))) return true;
        i = j;
    }
    uint64_t bloom[GRAM_BITS / 64] = {0};
    for (size_t i = 0; i + 3 <= u.n; i++) {
        uint32_t g = gram_of(u.p + i);
        bloom[g >> 6] |= 1ull << (g & 63);
    }
    for (uint32_t i = 0; i < b->nnotok; i++) {
        uint32_t g = b->notok_gram[i];
        if (g != NO_GRAM && !(bloom[g >> 6] & (1ull << (g & 63)))) continue;
        if (visit(x, s, b->notok[i])) return true;
    }
    return false;
}

static bool page_allowed(const hk_blocker *b, const hk_block_request *r, char *stack, hk_charbuf *heap)
{
    hk_block_request page = {r->site_url, r->site_host, {0}, {0}, HK_REQ_DOCUMENT};
    ctx x;
    ctx_init(&x, b, &page, stack, heap);
    scan s = {F_EXCEPTION | F_PAGE, 0, -1, true};
    scan_candidates(&x, &s);
    return s.found >= 0;
}

hk_block_result hk_blocker_match(const hk_blocker *b, const hk_block_request *r)
{
    hk_block_result res = {false, -1};
    if (!b || !r->url.n) return res;
    char stack[URL_STACK];
    hk_charbuf heap = {0};
    ctx x;
    ctx_init(&x, b, r, stack, &heap);
    /* 1. blocking filters ($important ones end the search) */
    scan blk = {0, F_EXCEPTION | F_REMOVEPARAM, -1, false};
    scan_candidates(&x, &blk);
    if (blk.found >= 0) {
        bool important = (b->filters[blk.found].flags & F_IMPORTANT) != 0;
        res.blocked = true;
        res.filter = blk.found;
        if (!important) {
            /* 2. exceptions */
            scan exc = {F_EXCEPTION, F_REMOVEPARAM, -1, true};
            scan_candidates(&x, &exc);
            if (exc.found >= 0) {
                res.blocked = false;
                res.filter = exc.found;
            } else if (r->site_url.n && page_allowed(b, r, stack, &heap)) {
                res.blocked = false; /* @@...$document on the top-level page */
            }
        }
    }
    hk_vec_free(&heap);
    return res;
}

/* --- $removeparam -------------------------------------------------------------------------- */

static bool param_removed(const hk_blocker *b, const uint32_t *hits, uint32_t nhits, hk_str param)
{
    hk_str name = param;
    size_t eq = hk_str_find_char(param, '=');
    if (eq != HK_STR_NPOS) name = hk_str_slice(param, 0, eq);
    for (uint32_t i = 0; i < nhits; i++) {
        const img_filter *f = &b->filters[hits[i]];
        hk_str v = {b->str + f->param_off, f->param_len};
        bool m;
        if (v.n == 0) {
            m = true;
        } else if (f->flags & F_PARAM_REGEX) {
            uint64_t re;
            m = hk_map64_get(&b->regex, 2ull * hits[i] + 2, &re) && hk_regex_search((const hk_regex *)(uintptr_t)re, param);
        } else {
            m = hk_str_eq(name, v);
        }
        if (f->flags & F_PARAM_NEGATE) m = !m;
        if (m) return true;
    }
    return false;
}

bool hk_blocker_strip_params(const hk_blocker *b, const hk_block_request *r, hk_charbuf *out)
{
    if (!b || !b->nparam) return false;
    size_t q = hk_str_find_char(r->url, '?');
    if (q == HK_STR_NPOS) return false;
    char stack[URL_STACK];
    hk_charbuf heap = {0};
    ctx x;
    ctx_init(&x, b, r, stack, &heap);
    HK_VEC(uint32_t) hits = {0};
    bool disabled = false;
    for (uint32_t i = 0; i < b->nparam; i++) {
        uint32_t idx = b->param[i];
        if (!filter_applies(&x, idx)) continue;
        hk_vec_push(&hits, idx);
    }
    /* Exceptions cancel filters with the same value; an empty exception cancels them all. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < hits.len && !disabled; i++) {
        const img_filter *f = &b->filters[hits.data[i]];
        if (!(f->flags & F_EXCEPTION)) continue;
        if (f->param_len == 0) disabled = true;
    }
    for (uint32_t i = 0; i < hits.len && !disabled; i++) {
        const img_filter *f = &b->filters[hits.data[i]];
        if (f->flags & F_EXCEPTION) continue;
        bool cancelled = false;
        for (uint32_t k = 0; k < hits.len && !cancelled; k++) {
            const img_filter *e = &b->filters[hits.data[k]];
            cancelled = (e->flags & F_EXCEPTION) && e->param_len == f->param_len &&
                        memcmp(b->str + e->param_off, b->str + f->param_off, f->param_len) == 0 &&
                        (e->flags & (F_PARAM_NEGATE | F_PARAM_REGEX)) == (f->flags & (F_PARAM_NEGATE | F_PARAM_REGEX));
        }
        if (!cancelled) hits.data[kept++] = hits.data[i];
    }
    bool changed = false;
    if (!disabled && kept) {
        hk_str u = r->url;
        size_t hash = hk_str_find_char(u, '#');
        size_t qend = hash == HK_STR_NPOS ? u.n : hash;
        hk_charbuf nb = {0};
        hk_charbuf_append(&nb, u.p, q);
        bool first = true;
        size_t pos = q + 1;
        while (pos <= qend) {
            size_t amp = pos;
            while (amp < qend && u.p[amp] != '&') amp++;
            hk_str p = {u.p + pos, amp - pos};
            if (p.n && param_removed(b, hits.data, kept, p)) {
                changed = true;
            } else if (p.n) {
                hk_charbuf_push(&nb, first ? '?' : '&');
                hk_charbuf_append(&nb, p.p, p.n);
                first = false;
            }
            pos = amp + 1;
        }
        if (hash != HK_STR_NPOS) hk_charbuf_append(&nb, u.p + hash, u.n - hash);
        if (changed) {
            out->len = 0;
            hk_charbuf_append(out, nb.data, nb.len);
        }
        hk_vec_free(&nb);
    }
    hk_vec_free(&hits);
    hk_vec_free(&heap);
    return changed;
}
