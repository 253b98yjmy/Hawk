/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Filter list parser and compiler (docs/design/04-blocker.md, 1 and 3.3). */
#include "blocker/hk_blocker.h"
#include "blocker/hk_filter_image.h"
#include "blocker/hk_regex.h"
#include "base/hk_map64.h"
#include "base/hk_mem.h"

#include <string.h>

typedef HK_VEC(uint64_t) u64_vec;
typedef HK_VEC(uint32_t) u32_vec;

typedef struct pfilter {
    img_filter rec;
    uint64_t raw_hash;
    uint32_t tok_first, tok_n; /* candidate tokens in c->tokens */
} pfilter;

typedef HK_VEC(pfilter) pfilter_vec;

struct hk_filter_compiler {
    hk_charbuf str;
    hk_map64 str_index; /* string hash -> offset + 1 */
    u64_vec domains;
    pfilter_vec filters;
    u64_vec tokens;
    hk_map64 token_freq;
    hk_map64 seen; /* raw line hashes, for duplicates */
    hk_map64 bad;  /* raw hashes disabled by $badfilter */
    HK_VEC(img_span) lists;
    hk_filter_stats st;
};

hk_filter_compiler *hk_filter_compiler_create(void)
{
    hk_filter_compiler *c = hk_alloc_zero(sizeof *c);
    hk_map64_init(&c->str_index);
    hk_map64_init(&c->token_freq);
    hk_map64_init(&c->seen);
    hk_map64_init(&c->bad);
    hk_charbuf_push(&c->str, 0); /* offset 0 = the empty string */
    return c;
}

static void compiler_free(hk_filter_compiler *c)
{
    hk_vec_free(&c->str);
    hk_map64_free(&c->str_index);
    hk_vec_free(&c->domains);
    hk_vec_free(&c->filters);
    hk_vec_free(&c->tokens);
    hk_map64_free(&c->token_freq);
    hk_map64_free(&c->seen);
    hk_map64_free(&c->bad);
    hk_vec_free(&c->lists);
    hk_free(c, sizeof *c);
}

static uint32_t add_str(hk_filter_compiler *c, hk_str s)
{
    if (s.n == 0) return 0;
    uint64_t key = hk_key64_nonzero(hk_str_hash64(s));
    uint64_t v;
    if (hk_map64_get(&c->str_index, key, &v) && v - 1 + s.n <= c->str.len &&
        memcmp(c->str.data + v - 1, s.p, s.n) == 0)
        return (uint32_t)(v - 1);
    uint32_t off = c->str.len;
    hk_charbuf_append(&c->str, s.p, s.n);
    hk_map64_put(&c->str_index, key, (uint64_t)off + 1);
    return off;
}

static bool is_token_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '%' || (c >= 'A' && c <= 'Z');
}

static bool is_host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

static uint64_t token_hash(const char *p, size_t n)
{
    char low[64];
    if (n > sizeof low) n = sizeof low; /* long tokens: the prefix is distinctive enough */
    for (size_t i = 0; i < n; i++) low[i] = hk_ascii_lower(p[i]);
    return hk_key64_nonzero(hk_hash64(low, n));
}

/* --- options ------------------------------------------------------------------------------- */

typedef struct type_name {
    const char *name;
    uint32_t mask; /* 0: a type Hawk never requests */
} type_name;

static const type_name k_types[] = {
    {"image", HK_REQ_IMAGE},
    {"stylesheet", HK_REQ_STYLESHEET},
    {"css", HK_REQ_STYLESHEET},
    {"font", HK_REQ_FONT},
    {"document", HK_REQ_DOCUMENT},
    {"doc", HK_REQ_DOCUMENT},
    {"other", HK_REQ_OTHER},
    {"all", HK_REQ_ALL},
    {"script", 0},
    {"xmlhttprequest", 0},
    {"xhr", 0},
    {"subdocument", 0},
    {"frame", 0},
    {"media", 0},
    {"websocket", 0},
    {"ping", 0},
    {"beacon", 0},
    {"popup", 0},
    {"popunder", 0},
    {"object", 0},
    {"object-subrequest", 0},
    {"webrtc", 0},
    {"inline-script", 0},
    {"inline-font", 0},
};

static const char *const k_cosmetic_options[] = {
    "elemhide", "ehide", "generichide", "ghide", "specifichide", "shide",
};

static bool name_in(hk_str name, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (hk_str_eq(name, hk_str_from_cstr(list[i]))) return true;
    return false;
}

static bool lower_copy(hk_str s, char *buf, size_t cap, hk_str *out)
{
    if (s.n > cap) return false;
    for (size_t i = 0; i < s.n; i++) buf[i] = hk_ascii_lower(s.p[i]);
    *out = (hk_str){buf, s.n};
    return true;
}

/* "a.com|~b.com|example.*": hashes with the low bit marking negation. */
static bool parse_domains(hk_filter_compiler *c, hk_str value, bool negate_all, uint32_t *off, uint16_t *n)
{
    uint32_t start = c->domains.len;
    size_t pos = 0;
    while (pos <= value.n) {
        size_t end = pos;
        while (end < value.n && value.p[end] != '|') end++;
        hk_str d = hk_str_trim_ascii_ws(hk_str_slice(value, pos, end));
        pos = end + 1;
        bool neg = negate_all;
        if (d.n && d.p[0] == '~') {
            neg = !neg;
            d = hk_str_slice(d, 1, d.n);
        }
        char buf[256];
        hk_str low;
        if (d.n == 0 || !lower_copy(d, buf, sizeof buf, &low)) {
            c->domains.len = start;
            return false;
        }
        if (low.n && low.p[0] == '/') { /* regex domains (uBO): not supported */
            c->domains.len = start;
            return false;
        }
        uint64_t h = (hk_str_hash64(low) & ~1ull) | (neg ? 1u : 0u);
        hk_vec_push(&c->domains, h);
        if (end >= value.n) break;
    }
    if (c->domains.len - start > 0xFFFF) {
        c->domains.len = start;
        return false;
    }
    if (*n) { /* a second domain= option: keep the first */
        c->domains.len = start;
        return true;
    }
    *off = start;
    *n = (uint16_t)(c->domains.len - start);
    return true;
}

typedef enum { PARSE_OK, PARSE_DROP_UNSUPPORTED, PARSE_DROP_INVALID, PARSE_DROP_COSMETIC, PARSE_BADFILTER } parse_result;

/* Splits the option list at commas, keeping "/regex/" values of removeparam together. */
static size_t next_option_end(hk_str opts, size_t pos)
{
    hk_str rest = hk_str_slice(opts, pos, opts.n);
    size_t eq = hk_str_find_char(rest, '=');
    size_t comma = hk_str_find_char(rest, ',');
    if (eq != HK_STR_NPOS && (comma == HK_STR_NPOS || eq < comma) && eq + 1 < rest.n && rest.p[eq + 1] == '/') {
        /* value is /.../flags: find the closing slash */
        size_t i = eq + 2;
        while (i < rest.n && !(rest.p[i] == '/' && rest.p[i - 1] != '\\')) i++;
        while (i < rest.n && rest.p[i] != ',') i++;
        return pos + i;
    }
    return comma == HK_STR_NPOS ? opts.n : pos + comma;
}

static parse_result parse_options(hk_filter_compiler *c, hk_str opts, img_filter *f, hk_str *param, bool *implicit)
{
    uint32_t types_pos = 0, types_neg = 0;
    bool unsupported_type = false, badfilter = false, cosmetic = false;
    size_t pos = 0;
    while (pos < opts.n) {
        size_t end = next_option_end(opts, pos);
        hk_str opt = hk_str_trim_ascii_ws(hk_str_slice(opts, pos, end));
        pos = end + 1;
        if (opt.n == 0) continue;
        bool neg = opt.p[0] == '~';
        if (neg) opt = hk_str_slice(opt, 1, opt.n);
        hk_str name = opt, value = {0};
        size_t eq = hk_str_find_char(opt, '=');
        if (eq != HK_STR_NPOS) {
            name = hk_str_slice(opt, 0, eq);
            value = hk_str_slice(opt, eq + 1, opt.n);
        }
        char nbuf[32];
        hk_str lname;
        if (!lower_copy(name, nbuf, sizeof nbuf, &lname)) return PARSE_DROP_UNSUPPORTED;

        bool is_type = false;
        for (size_t i = 0; i < HK_ARRAY_LEN(k_types); i++) {
            if (!hk_str_eq(lname, hk_str_from_cstr(k_types[i].name))) continue;
            is_type = true;
            if (neg) types_neg |= k_types[i].mask;
            else if (k_types[i].mask) types_pos |= k_types[i].mask;
            else unsupported_type = true;
            if (!neg && k_types[i].mask == HK_REQ_DOCUMENT && (f->flags & F_EXCEPTION)) f->flags |= F_PAGE;
            break;
        }
        if (is_type) continue;

        if (hk_str_eq(lname, HK_STR("third-party")) || hk_str_eq(lname, HK_STR("3p")) ||
            hk_str_eq(lname, HK_STR("strict3p"))) {
            f->flags |= neg ? F_1P : F_3P;
        } else if (hk_str_eq(lname, HK_STR("first-party")) || hk_str_eq(lname, HK_STR("1p")) ||
                   hk_str_eq(lname, HK_STR("strict1p"))) {
            f->flags |= neg ? F_3P : F_1P;
        } else if (hk_str_eq(lname, HK_STR("domain")) || hk_str_eq(lname, HK_STR("from"))) {
            if (!parse_domains(c, value, false, &f->from_off, &f->from_n)) return PARSE_DROP_INVALID;
        } else if (hk_str_eq(lname, HK_STR("to"))) {
            if (!parse_domains(c, value, false, &f->to_off, &f->to_n)) return PARSE_DROP_INVALID;
        } else if (hk_str_eq(lname, HK_STR("denyallow"))) {
            if (!parse_domains(c, value, true, &f->to_off, &f->to_n)) return PARSE_DROP_INVALID;
        } else if (hk_str_eq(lname, HK_STR("important"))) {
            f->flags |= F_IMPORTANT;
        } else if (hk_str_eq(lname, HK_STR("match-case"))) {
            f->flags |= F_MATCH_CASE;
        } else if (hk_str_eq(lname, HK_STR("badfilter"))) {
            badfilter = true;
        } else if (hk_str_eq(lname, HK_STR("removeparam")) || hk_str_eq(lname, HK_STR("queryprune"))) {
            f->flags |= F_REMOVEPARAM;
            *param = value;
        } else if (hk_str_eq(lname, HK_STR("redirect")) || hk_str_eq(lname, HK_STR("empty")) ||
                   hk_str_eq(lname, HK_STR("mp4"))) {
            /* blocking is all Hawk does with these: the replacement resources are for scripts */
            if (f->flags & F_EXCEPTION) return PARSE_DROP_UNSUPPORTED;
        } else if (hk_str_eq(lname, HK_STR("_")) || hk_str_eq(lname, HK_STR("__"))) {
            /* no-op separators (uBO) */
        } else if (name_in(lname, k_cosmetic_options, HK_ARRAY_LEN(k_cosmetic_options))) {
            cosmetic = true;
        } else {
            /* csp, header, replace, redirect-rule, ... or unknown: an option we cannot honour
               drops the filter, as uBO does for unknown options */
            return PARSE_DROP_UNSUPPORTED;
        }
    }
    if (cosmetic) return PARSE_DROP_COSMETIC;
    uint32_t mask;
    if (types_pos) mask = types_pos & ~types_neg;
    else if (unsupported_type) mask = 0;
    else mask = HK_REQ_ALL & ~(uint32_t)HK_REQ_DOCUMENT & ~types_neg; /* documents: see parse_network */
    *implicit = types_pos == 0;
    if (mask == 0) return PARSE_DROP_UNSUPPORTED;
    if ((f->flags & F_1P) && (f->flags & F_3P)) return PARSE_DROP_UNSUPPORTED;
    f->flags |= mask << F_TYPE_SHIFT;
    if ((f->flags & F_REMOVEPARAM) && (f->flags & F_IMPORTANT)) return PARSE_DROP_UNSUPPORTED;
    return badfilter ? PARSE_BADFILTER : PARSE_OK;
}

/* Finds the "$" that starts the option list: the last one followed by an option name. In a
   line that is entirely "/regex/", a "$" not right after a "/" belongs to the regex. */
static size_t find_options(hk_str line)
{
    bool whole_regex = line.n >= 2 && line.p[0] == '/' && line.p[line.n - 1] == '/';
    for (size_t i = line.n; i-- > 0;) {
        if (line.p[i] != '$') continue;
        if (i + 1 < line.n) {
            char n = line.p[i + 1];
            bool option_start = (n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || n == '~' || n == '_' ||
                                n == '1' || n == '3'; /* $1p, $3p */
            if (!option_start) continue;
        }
        if (whole_regex && (i == 0 || line.p[i - 1] != '/')) continue;
        return i;
    }
    return HK_STR_NPOS;
}

static bool is_cosmetic(hk_str line)
{
    for (size_t i = 0; i < line.n; i++) {
        if (line.p[i] != '#') continue;
        size_t j = i + 1;
        while (j < line.n && (line.p[j] == '@' || line.p[j] == '$' || line.p[j] == '?' || line.p[j] == '%')) j++;
        if (j < line.n && line.p[j] == '#') return true;
    }
    /* AdGuard HTML filtering rules: "example.com$$script[...]" */
    return hk_str_find(line, HK_STR("$$")) != HK_STR_NPOS || hk_str_find(line, HK_STR("$@$")) != HK_STR_NPOS;
}

/* Candidate tokens: runs of [a-z0-9%] that must appear whole in a matching URL. */
static void collect_tokens(hk_filter_compiler *c, pfilter *pf, hk_str pat)
{
    pf->tok_first = c->tokens.len;
    uint32_t flags = pf->rec.flags;
    size_t i = 0;
    while (i < pat.n) {
        if (!is_token_char(pat.p[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < pat.n && is_token_char(pat.p[j])) j++;
        bool left_ok = i == 0 ? (flags & (F_START | F_HOST)) != 0 : pat.p[i - 1] != '*';
        bool right_ok = j == pat.n ? (flags & F_END) != 0 : pat.p[j] != '*';
        if (left_ok && right_ok && j - i >= 2) {
            uint64_t h = token_hash(pat.p + i, j - i);
            hk_vec_push(&c->tokens, h);
            uint64_t n = 0;
            hk_map64_get(&c->token_freq, h, &n);
            hk_map64_put(&c->token_freq, h, n + 1);
        }
        i = j;
    }
    pf->tok_n = c->tokens.len - pf->tok_first;
}

static bool pure_host(const img_filter *f, hk_str pat, hk_str *host)
{
    if ((f->flags & (F_HOST | F_END | F_REGEX)) != F_HOST || pat.n < 2 || pat.p[pat.n - 1] != '^') return false;
    for (size_t i = 0; i + 1 < pat.n; i++)
        if (!is_host_char(pat.p[i])) return false;
    *host = (hk_str){pat.p, pat.n - 1};
    return true;
}

static uint64_t normalized_badfilter_hash(hk_str pattern, hk_str opts)
{
    /* The same filter text without the badfilter option. */
    hk_charbuf b = {0};
    hk_charbuf_append(&b, pattern.p, pattern.n);
    size_t pos = 0;
    bool first = true;
    while (pos < opts.n) {
        size_t end = next_option_end(opts, pos);
        hk_str opt = hk_str_trim_ascii_ws(hk_str_slice(opts, pos, end));
        pos = end + 1;
        if (opt.n == 0 || hk_str_eq_ascii_icase(opt, HK_STR("badfilter"))) continue;
        hk_charbuf_push(&b, first ? '$' : ',');
        hk_charbuf_append(&b, opt.p, opt.n);
        first = false;
    }
    uint64_t h = hk_key64_nonzero(hk_hash64(b.data, b.len));
    hk_vec_free(&b);
    return h;
}

static void parse_network(hk_filter_compiler *c, hk_str line, uint32_t source)
{
    hk_str full = line;
    img_filter f = {0};
    f.source = source;
    if (hk_str_starts_with(line, HK_STR("@@"))) {
        f.flags |= F_EXCEPTION;
        line = hk_str_slice(line, 2, line.n);
    }
    size_t dollar = find_options(line);
    hk_str pattern = line, opts = {0}, param = {0};
    if (dollar != HK_STR_NPOS) {
        pattern = hk_str_slice(line, 0, dollar);
        opts = hk_str_slice(line, dollar + 1, line.n);
    }
    bool implicit_types = false;
    parse_result pr = parse_options(c, opts, &f, &param, &implicit_types);
    if (pr == PARSE_BADFILTER) {
        hk_str prefix = hk_str_slice(full, 0, (size_t)(pattern.p + pattern.n - full.p));
        hk_map64_put(&c->bad, normalized_badfilter_hash(prefix, opts), 1);
        return;
    }
    if (pr == PARSE_DROP_UNSUPPORTED) {
        c->st.unsupported++;
        return;
    }
    if (pr == PARSE_DROP_INVALID) {
        c->st.invalid++;
        return;
    }
    if (pr == PARSE_DROP_COSMETIC) {
        c->st.cosmetic++;
        return;
    }

    /* Pattern: anchors, wildcards, case. */
    if (pattern.n >= 2 && pattern.p[0] == '/' && pattern.p[pattern.n - 1] == '/') {
        f.flags |= F_REGEX;
        pattern = hk_str_slice(pattern, 1, pattern.n - 1);
        hk_regex *re = hk_regex_compile(pattern, !(f.flags & F_MATCH_CASE));
        if (!re) {
            c->st.invalid++;
            return;
        }
        hk_regex_free(re);
    } else {
        if (hk_str_starts_with(pattern, HK_STR("||"))) {
            f.flags |= F_HOST;
            pattern = hk_str_slice(pattern, 2, pattern.n);
        } else if (hk_str_starts_with(pattern, HK_STR("|"))) {
            f.flags |= F_START;
            pattern = hk_str_slice(pattern, 1, pattern.n);
        }
        if (pattern.n && pattern.p[pattern.n - 1] == '|') {
            f.flags |= F_END;
            pattern.n--;
        }
        while (pattern.n && pattern.p[0] == '*') {
            pattern = hk_str_slice(pattern, 1, pattern.n);
            f.flags &= ~(uint32_t)(F_START | F_HOST);
        }
        while (pattern.n && pattern.p[pattern.n - 1] == '*') {
            pattern.n--;
            f.flags &= ~(uint32_t)F_END;
        }
        if (pattern.n == 0) f.flags &= ~(uint32_t)(F_START | F_END | F_HOST);
        if (pattern.n == 0 && !f.from_n && !f.to_n && !(f.flags & (F_REMOVEPARAM | F_EXCEPTION))) {
            c->st.invalid++; /* would block everything */
            return;
        }
    }
    if (pattern.n > 0xFFFF || param.n > 0xFFFF) {
        c->st.invalid++;
        return;
    }
    for (size_t i = 0; i < pattern.n; i++) {
        if (pattern.p[i] == ' ' || pattern.p[i] == '\t') {
            c->st.invalid++;
            return;
        }
    }

    /* $removeparam values */
    if (f.flags & F_REMOVEPARAM) {
        if (param.n && param.p[0] == '~') {
            f.flags |= F_PARAM_NEGATE;
            param = hk_str_slice(param, 1, param.n);
        }
        if (param.n >= 2 && param.p[0] == '/') {
            size_t close = param.n;
            while (close > 1 && param.p[close - 1] != '/') close--;
            if (close <= 1) {
                c->st.invalid++;
                return;
            }
            hk_str flags = hk_str_slice(param, close, param.n);
            hk_str body = hk_str_slice(param, 1, close - 1);
            hk_regex *re = hk_regex_compile(body, hk_str_find_char(flags, 'i') != HK_STR_NPOS);
            if (!re) {
                c->st.invalid++;
                return;
            }
            hk_regex_free(re);
            f.flags |= F_PARAM_REGEX;
        }
        f.param_off = add_str(c, param);
        f.param_len = (uint16_t)param.n;
    }

    uint64_t raw = hk_key64_nonzero(hk_str_hash64(full));
    if (hk_map64_get(&c->seen, raw, NULL)) {
        c->st.duplicate++;
        return;
    }
    hk_map64_put(&c->seen, raw, 1);

    char stack[512];
    hk_charbuf heap = {0};
    char *buf = stack;
    if (pattern.n > sizeof stack) {
        hk_vec_reserve(&heap, pattern.n);
        buf = heap.data;
    }
    /* Regexes keep their case ("\D" is not "\d"); they are compiled case-insensitive instead. */
    for (size_t i = 0; i < pattern.n; i++)
        buf[i] = (f.flags & (F_MATCH_CASE | F_REGEX)) ? pattern.p[i] : hk_ascii_lower(pattern.p[i]);
    hk_str pat = {buf, pattern.n};
    /* Without explicit types, only host-name filters block whole pages (strict blocking);
       exceptions and $removeparam apply to pages too. */
    hk_str host_part;
    if (implicit_types && ((f.flags & (F_EXCEPTION | F_REMOVEPARAM)) || pure_host(&f, pat, &host_part)))
        f.flags |= (uint32_t)HK_REQ_DOCUMENT << F_TYPE_SHIFT;
    f.pat_off = add_str(c, pat);
    f.pat_len = (uint16_t)pat.n;

    pfilter pf = {0};
    pf.rec = f;
    pf.raw_hash = raw;
    if (!(f.flags & (F_REGEX | F_REMOVEPARAM))) collect_tokens(c, &pf, pat);
    hk_vec_free(&heap);
    hk_vec_push(&c->filters, pf);
}

/* "0.0.0.0 host", "127.0.0.1 host", "::1 host", "::0 host" */
static bool parse_hosts_line(hk_filter_compiler *c, hk_str line, uint32_t source)
{
    static const char *const prefixes[] = {"0.0.0.0", "127.0.0.1", "::1", "::0", "::"};
    hk_str rest = {0};
    bool found = false;
    for (size_t i = 0; i < HK_ARRAY_LEN(prefixes) && !found; i++) {
        hk_str p = hk_str_from_cstr(prefixes[i]);
        if (hk_str_starts_with(line, p) && line.n > p.n && (line.p[p.n] == ' ' || line.p[p.n] == '\t')) {
            rest = hk_str_trim_ascii_ws(hk_str_slice(line, p.n, line.n));
            found = true;
        }
    }
    if (!found) return false;
    size_t end = 0;
    while (end < rest.n && rest.p[end] != ' ' && rest.p[end] != '\t' && rest.p[end] != '#') end++;
    hk_str host = hk_str_slice(rest, 0, end);
    if (host.n == 0 || host.n > 253 || hk_str_eq(host, HK_STR("localhost")) ||
        hk_str_eq(host, HK_STR("localhost.localdomain")) || hk_str_eq(host, HK_STR("broadcasthost")) ||
        hk_str_eq(host, HK_STR("local")) || hk_str_eq(host, HK_STR("0.0.0.0")) ||
        hk_str_find_char(host, '.') == HK_STR_NPOS) {
        return true; /* a hosts-file line we deliberately ignore */
    }
    char buf[260];
    buf[0] = '|';
    buf[1] = '|';
    for (size_t i = 0; i < host.n; i++) {
        buf[2 + i] = hk_ascii_lower(host.p[i]);
        if (!is_host_char(buf[2 + i])) {
            c->st.invalid++;
            return true;
        }
    }
    buf[2 + host.n] = '^';
    parse_network(c, (hk_str){buf, host.n + 3}, source);
    return true;
}

/* --- !#if ---------------------------------------------------------------------------------- */

typedef struct cond_parser {
    hk_str s;
    size_t pos;
} cond_parser;

static void skip_ws(cond_parser *p)
{
    while (p->pos < p->s.n && (p->s.p[p->pos] == ' ' || p->s.p[p->pos] == '\t')) p->pos++;
}

static bool cond_or(cond_parser *p);

static bool cond_atom(cond_parser *p)
{
    skip_ws(p);
    if (p->pos < p->s.n && p->s.p[p->pos] == '!') {
        p->pos++;
        return !cond_atom(p);
    }
    if (p->pos < p->s.n && p->s.p[p->pos] == '(') {
        p->pos++;
        bool v = cond_or(p);
        skip_ws(p);
        if (p->pos < p->s.n && p->s.p[p->pos] == ')') p->pos++;
        return v;
    }
    size_t start = p->pos;
    while (p->pos < p->s.n) {
        char ch = p->s.p[p->pos];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) break;
        p->pos++;
    }
    hk_str id = hk_str_slice(p->s, start, p->pos);
    /* Hawk's answers to the conditions used by uBO-flavoured lists. */
    static const char *const k_true[] = {"env_windows", "ext_ublock", "ext_ubol", "false_negated"};
    for (size_t i = 0; i < HK_ARRAY_LEN(k_true); i++)
        if (hk_str_eq(id, hk_str_from_cstr(k_true[i]))) return true;
    return false; /* env_firefox, env_chromium, env_mobile, cap_html_filtering, adguard, ... */
}

static bool cond_and(cond_parser *p)
{
    bool v = cond_atom(p);
    for (;;) {
        skip_ws(p);
        if (p->pos + 1 < p->s.n && p->s.p[p->pos] == '&' && p->s.p[p->pos + 1] == '&') {
            p->pos += 2;
            bool r = cond_atom(p);
            v = v && r;
        } else {
            return v;
        }
    }
}

static bool cond_or(cond_parser *p)
{
    bool v = cond_and(p);
    for (;;) {
        skip_ws(p);
        if (p->pos + 1 < p->s.n && p->s.p[p->pos] == '|' && p->s.p[p->pos + 1] == '|') {
            p->pos += 2;
            bool r = cond_and(p);
            v = v || r;
        } else {
            return v;
        }
    }
}

/* --- lists --------------------------------------------------------------------------------- */

void hk_filter_compiler_add_list(hk_filter_compiler *c, hk_str name, hk_str text)
{
    uint32_t list = c->lists.len;
    if (list >= 4096) return;
    img_span span = {add_str(c, name), (uint32_t)name.n};
    hk_vec_push(&c->lists, span);

    /* Nesting of !#if: bit i of active_mask = the condition at depth i holds. */
    uint64_t active_mask = 0;
    uint32_t depth = 0, inactive_depth = 0; /* inactive_depth: nesting levels that are false */
    uint32_t lineno = 0;
    size_t pos = 0;
    if (text.n >= 3 && memcmp(text.p, "\xEF\xBB\xBF", 3) == 0) pos = 3;
    while (pos < text.n) {
        size_t eol = pos;
        while (eol < text.n && text.p[eol] != '\n') eol++;
        hk_str line = hk_str_trim_ascii_ws(hk_str_slice(text, pos, eol));
        pos = eol + 1;
        lineno++;
        if (line.n == 0) continue;
        if (line.p[0] == '!') {
            if (hk_str_starts_with(line, HK_STR("!#if "))) {
                cond_parser cp = {hk_str_slice(line, 5, line.n), 0};
                bool v = cond_or(&cp);
                if (depth < 64) {
                    if (v) active_mask |= 1ull << depth;
                    else active_mask &= ~(1ull << depth);
                }
                if (!v) inactive_depth++;
                depth++;
            } else if (hk_str_starts_with(line, HK_STR("!#else")) && depth > 0) {
                uint32_t d = depth - 1;
                bool was = d < 64 && (active_mask >> d & 1);
                if (was) {
                    inactive_depth++;
                    active_mask &= ~(1ull << d);
                } else {
                    inactive_depth--;
                    if (d < 64) active_mask |= 1ull << d;
                }
            } else if (hk_str_starts_with(line, HK_STR("!#endif")) && depth > 0) {
                depth--;
                if (!(depth < 64 && (active_mask >> depth & 1))) inactive_depth--;
            }
            /* "!#include" needs the list's URL; included sub-lists are fetched as lists of their own. */
            continue;
        }
        if (inactive_depth) continue;
        if (line.p[0] == '[') continue;
        uint32_t source = list << 20 | (lineno < 0xFFFFF ? lineno : 0xFFFFF);
        if (line.p[0] == '#' && !hk_str_starts_with(line, HK_STR("##")) && !hk_str_starts_with(line, HK_STR("#@#")) &&
            !hk_str_starts_with(line, HK_STR("#?#")) && !hk_str_starts_with(line, HK_STR("#$#")))
            continue; /* hosts-file comment */
        if (is_cosmetic(line)) {
            c->st.cosmetic++;
            continue;
        }
        if (parse_hosts_line(c, line, source)) continue;
        parse_network(c, line, source);
    }
}

/* --- image --------------------------------------------------------------------------------- */

enum { INDEX_NONE, INDEX_HOST, INDEX_TOKEN, INDEX_PARAM };

typedef struct index_builder {
    hk_map64 slot; /* key -> bucket slot */
    HK_VEC(img_bucket) table;
    u32_vec fill;
} index_builder;

static uint32_t pow2_at_least(uint32_t n)
{
    uint32_t c = 4;
    while (c < n) c <<= 1;
    return c;
}

static void index_build(index_builder *ib, const u64_vec *keys, uint32_t *post_base)
{
    /* keys[i] = index key of filter i in this index, or 0. */
    hk_map64 count;
    hk_map64_init(&count);
    for (uint32_t i = 0; i < keys->len; i++) {
        if (!keys->data[i]) continue;
        uint64_t n = 0;
        hk_map64_get(&count, keys->data[i], &n);
        hk_map64_put(&count, keys->data[i], n + 1);
    }
    uint32_t cap = pow2_at_least(count.len + count.len / 3 + 1) /* load factor <= 0.75 */;
    hk_vec_reserve(&ib->table, cap);
    memset(ib->table.data, 0, (size_t)cap * sizeof(img_bucket));
    ib->table.len = cap;
    hk_vec_reserve(&ib->fill, cap);
    memset(ib->fill.data, 0, (size_t)cap * sizeof(uint32_t));
    ib->fill.len = cap;
    hk_map64_init(&ib->slot);
    uint32_t next = *post_base;
    for (uint32_t i = 0; i < keys->len; i++) {
        uint64_t k = keys->data[i];
        if (!k || hk_map64_get(&ib->slot, k, NULL)) continue;
        uint64_t n = 0;
        hk_map64_get(&count, k, &n);
        uint32_t s = (uint32_t)k & (cap - 1);
        while (ib->table.data[s].key) s = (s + 1) & (cap - 1);
        ib->table.data[s].key = k;
        ib->table.data[s].start = next;
        ib->table.data[s].count = (uint32_t)n;
        next += (uint32_t)n;
        hk_map64_put(&ib->slot, k, s);
    }
    *post_base = next;
    hk_map64_free(&count);
}

static void index_fill(index_builder *ib, const u64_vec *keys, uint32_t *post)
{
    for (uint32_t i = 0; i < keys->len; i++) {
        uint64_t s;
        if (!keys->data[i] || !hk_map64_get(&ib->slot, keys->data[i], &s)) continue;
        img_bucket *b = &ib->table.data[s];
        post[b->start + ib->fill.data[s]++] = i;
    }
}

static void index_free(index_builder *ib)
{
    hk_map64_free(&ib->slot);
    hk_vec_free(&ib->table);
    hk_vec_free(&ib->fill);
}

static void put_section(hk_charbuf *out, size_t base, img_header *h, int sec, const void *data, size_t n)
{
    while ((out->len - base) % 8) hk_charbuf_push(out, 0);
    h->sec[sec].off = (uint32_t)(out->len - base);
    h->sec[sec].len = (uint32_t)n;
    if (n) hk_charbuf_append(out, data, n);
}

void hk_filter_compiler_finish(hk_filter_compiler *c, uint64_t stamp, hk_charbuf *out, hk_filter_stats *stats)
{
    /* Keep the filters that survive $badfilter, and decide how each is indexed. */
    HK_VEC(img_filter) recs = {0};
    u64_vec host_keys = {0}, tok_keys = {0};
    u32_vec notok = {0}, param = {0};
    for (uint32_t i = 0; i < c->filters.len; i++) {
        pfilter *pf = &c->filters.data[i];
        if (hk_map64_get(&c->bad, pf->raw_hash, NULL)) {
            c->st.badfiltered++;
            continue;
        }
        uint32_t idx = recs.len;
        hk_vec_push(&recs, pf->rec);
        hk_str pat = {c->str.data + pf->rec.pat_off, pf->rec.pat_len};
        hk_str host;
        uint64_t hkey = 0, tkey = 0;
        if (pf->rec.flags & F_REMOVEPARAM) {
            hk_vec_push(&param, idx);
        } else if (pure_host(&pf->rec, pat, &host)) {
            hkey = hk_key64_nonzero(hk_str_hash64(host));
            c->st.hosts++;
        } else if (pf->tok_n) {
            uint64_t best_n = UINT64_MAX;
            for (uint32_t t = 0; t < pf->tok_n; t++) {
                uint64_t h = c->tokens.data[pf->tok_first + t];
                uint64_t n = 0;
                hk_map64_get(&c->token_freq, h, &n);
                if (n < best_n) best_n = n, tkey = h;
            }
        } else {
            hk_vec_push(&notok, idx);
            c->st.untokenized++;
        }
        if (pf->rec.flags & F_REGEX) c->st.regex++;
        hk_vec_push(&host_keys, hkey);
        hk_vec_push(&tok_keys, tkey);
    }
    c->st.network = recs.len;

    uint32_t npost = 0;
    index_builder hi = {0}, ti = {0};
    index_build(&hi, &host_keys, &npost);
    index_build(&ti, &tok_keys, &npost);
    uint32_t *post = hk_alloc_zero((size_t)(npost ? npost : 1) * sizeof(uint32_t));
    index_fill(&hi, &host_keys, post);
    index_fill(&ti, &tok_keys, post);

    size_t base = out->len;
    img_header h = {0};
    memcpy(h.magic, HK_FILTER_MAGIC, 8);
    h.version = HK_FILTER_VERSION;
    h.nsec = SEC_COUNT;
    h.stamp = stamp ? stamp : 1;
    hk_charbuf_append(out, (const char *)&h, sizeof h);
    put_section(out, base, &h, SEC_STR, c->str.data, c->str.len);
    put_section(out, base, &h, SEC_FILTERS, recs.data, (size_t)recs.len * sizeof(img_filter));
    put_section(out, base, &h, SEC_DOMAINS, c->domains.data, (size_t)c->domains.len * sizeof(uint64_t));
    put_section(out, base, &h, SEC_HOSTIDX, hi.table.data, (size_t)hi.table.len * sizeof(img_bucket));
    put_section(out, base, &h, SEC_TOKIDX, ti.table.data, (size_t)ti.table.len * sizeof(img_bucket));
    put_section(out, base, &h, SEC_POST, post, (size_t)npost * sizeof(uint32_t));
    put_section(out, base, &h, SEC_NOTOK, notok.data, (size_t)notok.len * sizeof(uint32_t));
    put_section(out, base, &h, SEC_PARAM, param.data, (size_t)param.len * sizeof(uint32_t));
    put_section(out, base, &h, SEC_LISTS, c->lists.data, (size_t)c->lists.len * sizeof(img_span));
    memcpy(out->data + base, &h, sizeof h);

    if (stats) *stats = c->st;
    hk_free(post, (size_t)(npost ? npost : 1) * sizeof(uint32_t));
    index_free(&hi);
    index_free(&ti);
    hk_vec_free(&recs);
    hk_vec_free(&host_keys);
    hk_vec_free(&tok_keys);
    hk_vec_free(&notok);
    hk_vec_free(&param);
    compiler_free(c);
}
