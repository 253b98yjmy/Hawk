/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CSS Syntax Level 3 parsing (section 5), Selectors Level 4 (subset), Media Queries Level 4 (subset). */
#include "css/hk_css.h"
#include "css/hk_css_props.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- properties ---------------------------------------------------------------------------- */

static const char *const k_prop_names[HK_PROP_COUNT_] = {
    [HK_PROP_NONE_] = "",
#define HK_PROP_NAME(id, name) [HK_PROP_##id] = name,
    HK_CSS_PROPERTIES(HK_PROP_NAME)
#undef HK_PROP_NAME
    [HK_PROP_CUSTOM] = "--*",
};

hk_css_property hk_css_property_from_name(hk_str name)
{
    for (int i = 1; i < HK_PROP_CUSTOM; i++) {
        if (hk_str_eq_ascii_icase(name, hk_str_from_cstr(k_prop_names[i]))) return (hk_css_property)i;
    }
    return HK_PROP_NONE_;
}

const char *hk_css_property_name(hk_css_property p)
{
    return p < HK_PROP_COUNT_ ? k_prop_names[p] : "";
}

/* --- token walking ------------------------------------------------------------------------- */

typedef struct span {
    const hk_css_token *t;
    uint32_t n;
} span;

static bool is_open(uint8_t type)
{
    return type == HK_CT_LBRACE || type == HK_CT_LBRACKET || type == HK_CT_LPAREN || type == HK_CT_FUNCTION;
}

static uint8_t closer(uint8_t type)
{
    return type == HK_CT_LBRACE ? HK_CT_RBRACE : type == HK_CT_LBRACKET ? HK_CT_RBRACKET : HK_CT_RPAREN;
}

/* Index just past the component value starting at i. */
static uint32_t skip_component(const hk_css_token *t, uint32_t n, uint32_t i)
{
    if (i >= n) return n;
    if (!is_open(t[i].type)) return i + 1;
    /* Track the stack of open block types so mismatched closers are treated correctly. */
    uint8_t stack[64];
    int depth = 0;
    stack[depth++] = closer(t[i].type);
    for (i++; i < n; i++) {
        uint8_t ty = t[i].type;
        if (ty == HK_CT_EOF) return n;
        if (is_open(ty)) {
            if (depth < 64) stack[depth] = closer(ty);
            depth++;
        } else if (depth <= 64 && depth > 0 && ty == stack[depth - 1]) {
            if (--depth == 0) return i + 1;
        } else if (depth > 64 && (ty == HK_CT_RBRACE || ty == HK_CT_RBRACKET || ty == HK_CT_RPAREN)) {
            depth--;
        }
    }
    return n;
}

static uint32_t skip_ws(const hk_css_token *t, uint32_t n, uint32_t i)
{
    while (i < n && t[i].type == HK_CT_WS) i++;
    return i;
}

static span trim(span s)
{
    while (s.n && s.t[0].type == HK_CT_WS) {
        s.t++;
        s.n--;
    }
    while (s.n && (s.t[s.n - 1].type == HK_CT_WS || s.t[s.n - 1].type == HK_CT_EOF)) s.n--;
    return s;
}

static bool is_delim(const hk_css_token *t, uint32_t c)
{
    return t->type == HK_CT_DELIM && t->delim == c;
}

/* --- declarations ----------------------------------------------------------------------------- */

typedef HK_VEC(hk_css_decl) decl_vec;

static void parse_declarations(const hk_css_token *t, uint32_t n, decl_vec *out)
{
    uint32_t i = 0;
    while (i < n) {
        uint8_t ty = t[i].type;
        if (ty == HK_CT_WS || ty == HK_CT_SEMI) {
            i++;
            continue;
        }
        if (ty == HK_CT_EOF) break;
        if (ty == HK_CT_AT) {
            /* At-rules inside a declaration block are skipped. */
            while (i < n && t[i].type != HK_CT_SEMI && t[i].type != HK_CT_LBRACE) i = skip_component(t, n, i);
            if (i < n && t[i].type == HK_CT_LBRACE) i = skip_component(t, n, i);
            continue;
        }
        uint32_t start = i;
        /* Find the end of this declaration (a top-level ';'). */
        uint32_t end = i;
        while (end < n && t[end].type != HK_CT_SEMI && t[end].type != HK_CT_EOF) end = skip_component(t, n, end);
        i = end + 1;
        if (t[start].type != HK_CT_IDENT) continue;
        uint32_t k = skip_ws(t, end, start + 1);
        if (k >= end || t[k].type != HK_CT_COLON) continue;
        span v = trim((span){t + k + 1, end - k - 1});
        bool important = false;
        if (v.n >= 2 && hk_css_ident_is(&v.t[v.n - 1], "important")) {
            uint32_t b = v.n - 2;
            while (b > 0 && v.t[b].type == HK_CT_WS) b--;
            if (is_delim(&v.t[b], '!')) {
                important = true;
                v.n = b;
                v = trim(v);
            }
        }
        hk_css_decl d = {0};
        hk_str name = t[start].value;
        if (name.n > 2 && name.p[0] == '-' && name.p[1] == '-') {
            d.prop = HK_PROP_CUSTOM;
            d.custom = name;
        } else {
            d.prop = (uint16_t)hk_css_property_from_name(name);
            if (!d.prop || v.n == 0) continue;
        }
        d.important = important;
        d.v = v.t;
        d.vn = v.n;
        hk_vec_push(out, d);
    }
}

void hk_css_parse_declarations(hk_arena *a, hk_str src, const hk_css_decl **out, uint32_t *n)
{
    hk_css_tokens toks = hk_css_tokenize(a, src);
    decl_vec v = {0};
    parse_declarations(toks.t, toks.n, &v);
    hk_css_decl *arr = HK_ARENA_NEW_ARRAY(a, hk_css_decl, v.len);
    if (v.len) memcpy(arr, v.data, v.len * sizeof *arr);
    *out = arr;
    *n = v.len;
    hk_vec_free(&v);
}

/* --- selectors --------------------------------------------------------------------------------- */

typedef struct sel_parser {
    hk_arena *a;
    hk_atom_table *atoms;
    const hk_css_token *t;
    uint32_t n, i;
} sel_parser;

static hk_atom intern_lower(sel_parser *p, hk_str s)
{
    char buf[128];
    if (s.n > sizeof buf) return hk_atom_intern(p->atoms, s);
    for (size_t k = 0; k < s.n; k++) buf[k] = hk_ascii_lower(s.p[k]);
    return hk_atom_intern(p->atoms, (hk_str){buf, s.n});
}

static bool parse_list_into(sel_parser *p, const hk_css_token *t, uint32_t n, bool forgiving,
                            hk_css_selector_list *out);

/* Parses "an+b" from the text of the argument tokens. */
static bool parse_nth(const hk_css_token *t, uint32_t n, int32_t *a, int32_t *b)
{
    char buf[64];
    size_t o = 0;
    for (uint32_t i = 0; i < n && o < sizeof buf - 24; i++) {
        const hk_css_token *k = &t[i];
        if (k->type == HK_CT_WS) continue;
        if (k->type == HK_CT_NUMBER || k->type == HK_CT_DIMENSION) {
            if (!(k->flags & HK_CSS_INTEGER)) return false;
            bool need_plus = k->num >= 0 && o > 0 && buf[o - 1] != '+' && buf[o - 1] != '-';
            o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%d", need_plus ? "+" : "", (int)k->num);
            if (k->type == HK_CT_DIMENSION) {
                for (size_t c = 0; c < k->unit.n && o < sizeof buf - 1; c++) buf[o++] = hk_ascii_lower(k->unit.p[c]);
            }
        } else if (k->type == HK_CT_IDENT) {
            for (size_t c = 0; c < k->value.n && o < sizeof buf - 1; c++) buf[o++] = hk_ascii_lower(k->value.p[c]);
        } else if (is_delim(k, '+') || is_delim(k, '-')) {
            buf[o++] = (char)k->delim;
        } else {
            return false;
        }
    }
    buf[o] = 0;
    if (strcmp(buf, "odd") == 0) {
        *a = 2;
        *b = 1;
        return true;
    }
    if (strcmp(buf, "even") == 0) {
        *a = 2;
        *b = 0;
        return true;
    }
    const char *s = buf;
    char *n_pos = strchr(buf, 'n');
    char *end;
    if (!n_pos) {
        long v = strtol(s, &end, 10);
        if (end == s || *end != 0) return false;
        *a = 0;
        *b = (int32_t)v;
        return true;
    }
    long av;
    if (n_pos == s || (n_pos == s + 1 && s[0] == '+')) av = 1;
    else if (n_pos == s + 1 && s[0] == '-') av = -1;
    else {
        av = strtol(s, &end, 10);
        if (end != n_pos) return false;
    }
    const char *rest = n_pos + 1;
    long bv = 0;
    if (*rest) {
        if (rest[0] != '+' && rest[0] != '-') return false;
        bv = strtol(rest, &end, 10);
        if (end == rest + 1 || *end != 0) return false;
    }
    *a = (int32_t)av;
    *b = (int32_t)bv;
    return true;
}

static uint32_t list_max_specificity(const hk_css_selector_list *l)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < l->n; i++) m = HK_MAX(m, l->sel[i].specificity);
    return m;
}

typedef HK_VEC(hk_css_simple) simple_vec;

/* Parses one compound selector; returns false when invalid. *empty is set when nothing was consumed. */
static bool parse_compound(sel_parser *p, simple_vec *out, uint32_t *spec, bool *empty)
{
    *empty = true;
    out->len = 0;
    const hk_css_token *t = p->t;
    uint32_t n = p->n;
    /* Type or universal selector. */
    if (p->i < n && t[p->i].type == HK_CT_IDENT) {
        hk_css_simple s = {0};
        s.kind = HK_SS_TYPE;
        s.name = intern_lower(p, t[p->i].value);
        hk_vec_push(out, s);
        *spec += 1;
        p->i++;
        *empty = false;
    } else if (p->i < n && is_delim(&t[p->i], '*')) {
        hk_css_simple s = {0};
        s.kind = HK_SS_UNIVERSAL;
        hk_vec_push(out, s);
        p->i++;
        *empty = false;
    }
    if (p->i < n && is_delim(&t[p->i], '|')) return false; /* namespaces are not supported */
    for (;;) {
        if (p->i >= n) break;
        const hk_css_token *k = &t[p->i];
        hk_css_simple s = {0};
        if (k->type == HK_CT_HASH) {
            if (!(k->flags & HK_CSS_HASH_ID)) return false;
            s.kind = HK_SS_ID;
            s.name = hk_atom_intern(p->atoms, k->value);
            *spec += 1u << 20;
            p->i++;
        } else if (is_delim(k, '.')) {
            if (p->i + 1 >= n || t[p->i + 1].type != HK_CT_IDENT) return false;
            s.kind = HK_SS_CLASS;
            s.name = hk_atom_intern(p->atoms, t[p->i + 1].value);
            *spec += 1u << 10;
            p->i += 2;
        } else if (k->type == HK_CT_LBRACKET) {
            uint32_t end = skip_component(t, n, p->i) - 1; /* index of ']' */
            uint32_t j = skip_ws(t, end, p->i + 1);
            if (j >= end || t[j].type != HK_CT_IDENT) return false;
            s.name = intern_lower(p, t[j].value);
            s.kind = HK_SS_ATTR_EXISTS;
            j = skip_ws(t, end, j + 1);
            if (j < end) {
                uint32_t op = 0;
                if (is_delim(&t[j], '=')) {
                    op = '=';
                    j++;
                } else if (t[j].type == HK_CT_DELIM && j + 1 < end && is_delim(&t[j + 1], '=')) {
                    op = t[j].delim;
                    j += 2;
                } else {
                    return false;
                }
                switch (op) {
                case '=': s.kind = HK_SS_ATTR_EQ; break;
                case '~': s.kind = HK_SS_ATTR_INCLUDES; break;
                case '|': s.kind = HK_SS_ATTR_DASH; break;
                case '^': s.kind = HK_SS_ATTR_PREFIX; break;
                case '$': s.kind = HK_SS_ATTR_SUFFIX; break;
                case '*': s.kind = HK_SS_ATTR_SUBSTR; break;
                default: return false;
                }
                j = skip_ws(t, end, j);
                if (j >= end || (t[j].type != HK_CT_IDENT && t[j].type != HK_CT_STRING)) return false;
                s.value = t[j].value;
                j = skip_ws(t, end, j + 1);
                if (j < end) {
                    if (hk_css_ident_is(&t[j], "i")) s.icase = true;
                    else if (!hk_css_ident_is(&t[j], "s")) return false;
                    j = skip_ws(t, end, j + 1);
                }
                if (j != end) return false;
            }
            *spec += 1u << 10;
            p->i = end + 1;
        } else if (k->type == HK_CT_COLON) {
            bool element = p->i + 1 < n && t[p->i + 1].type == HK_CT_COLON;
            uint32_t j = p->i + (element ? 2 : 1);
            if (j >= n) return false;
            const hk_css_token *name = &t[j];
            hk_str nm = name->value;
            if (element || (name->type == HK_CT_IDENT && (hk_str_eq_ascii_icase(nm, HK_STR("before")) ||
                                                          hk_str_eq_ascii_icase(nm, HK_STR("after")) ||
                                                          hk_str_eq_ascii_icase(nm, HK_STR("first-line")) ||
                                                          hk_str_eq_ascii_icase(nm, HK_STR("first-letter"))))) {
                /* Pseudo-elements are not generated yet: such selectors never match. */
                if (name->type != HK_CT_IDENT && name->type != HK_CT_FUNCTION) return false;
                s.kind = HK_SS_NEVER;
                *spec += 1;
                p->i = name->type == HK_CT_FUNCTION ? skip_component(t, n, j) : j + 1;
            } else if (name->type == HK_CT_IDENT) {
                static const struct {
                    const char *name;
                    uint8_t kind;
                } k_pc[] = {
                    {"root", HK_SS_ROOT},
                    {"empty", HK_SS_EMPTY},
                    {"first-child", HK_SS_FIRST_CHILD},
                    {"last-child", HK_SS_LAST_CHILD},
                    {"only-child", HK_SS_ONLY_CHILD},
                    {"first-of-type", HK_SS_FIRST_OF_TYPE},
                    {"last-of-type", HK_SS_LAST_OF_TYPE},
                    {"only-of-type", HK_SS_ONLY_OF_TYPE},
                    {"link", HK_SS_LINK},
                    {"any-link", HK_SS_LINK},
                    {"visited", HK_SS_NEVER},
                    {"hover", HK_SS_NEVER},
                    {"active", HK_SS_NEVER},
                    {"focus", HK_SS_NEVER},
                    {"focus-within", HK_SS_NEVER},
                    {"focus-visible", HK_SS_NEVER},
                    {"target", HK_SS_NEVER},
                    {"checked", HK_SS_NEVER},
                    {"disabled", HK_SS_NEVER},
                    {"enabled", HK_SS_NEVER},
                    {"required", HK_SS_NEVER},
                    {"optional", HK_SS_NEVER},
                    {"invalid", HK_SS_NEVER},
                    {"valid", HK_SS_NEVER},
                    {"placeholder-shown", HK_SS_NEVER},
                    {"default", HK_SS_NEVER},
                    {"indeterminate", HK_SS_NEVER},
                    {"read-only", HK_SS_NEVER},
                    {"read-write", HK_SS_NEVER},
                    {"defined", HK_SS_NEVER},
                    {"fullscreen", HK_SS_NEVER},
                    {"modal", HK_SS_NEVER},
                    {"popover-open", HK_SS_NEVER},
                    {"open", HK_SS_NEVER},
                    {"closed", HK_SS_NEVER},
                    {"autofill", HK_SS_NEVER},
                    {"user-invalid", HK_SS_NEVER},
                    {"user-valid", HK_SS_NEVER},
                    {"in-range", HK_SS_NEVER},
                    {"out-of-range", HK_SS_NEVER},
                    {"scope", HK_SS_ROOT},
                };
                bool found = false;
                for (size_t q = 0; q < HK_ARRAY_LEN(k_pc) && !found; q++) {
                    if (hk_str_eq_ascii_icase(nm, hk_str_from_cstr(k_pc[q].name))) {
                        s.kind = k_pc[q].kind;
                        found = true;
                    }
                }
                if (!found) return false;
                *spec += 1u << 10;
                p->i = j + 1;
            } else if (name->type == HK_CT_FUNCTION) {
                uint32_t end = skip_component(t, n, j); /* past ')' */
                const hk_css_token *args = t + j + 1;
                uint32_t nargs = end - j - 2;
                if (hk_str_eq_ascii_icase(nm, HK_STR("not")) || hk_str_eq_ascii_icase(nm, HK_STR("is")) ||
                    hk_str_eq_ascii_icase(nm, HK_STR("where")) || hk_str_eq_ascii_icase(nm, HK_STR("matches")) ||
                    hk_str_eq_ascii_icase(nm, HK_STR("-webkit-any"))) {
                    bool is_not = hk_str_eq_ascii_icase(nm, HK_STR("not"));
                    hk_css_selector_list *inner = HK_ARENA_NEW(p->a, hk_css_selector_list);
                    if (!parse_list_into(p, args, nargs, !is_not, inner)) return false;
                    s.kind = is_not ? HK_SS_NOT : HK_SS_IS;
                    s.inner = inner;
                    if (!hk_str_eq_ascii_icase(nm, HK_STR("where"))) *spec += list_max_specificity(inner);
                } else if (hk_str_eq_ascii_icase(nm, HK_STR("nth-child")) ||
                           hk_str_eq_ascii_icase(nm, HK_STR("nth-last-child")) ||
                           hk_str_eq_ascii_icase(nm, HK_STR("nth-of-type")) ||
                           hk_str_eq_ascii_icase(nm, HK_STR("nth-last-of-type"))) {
                    /* "of S" filters are not supported: such selectors never match. */
                    bool has_of = false;
                    for (uint32_t q = 0; q < nargs; q++) has_of = has_of || hk_css_ident_is(&args[q], "of");
                    if (has_of) {
                        s.kind = HK_SS_NEVER;
                    } else {
                        if (!parse_nth(args, nargs, &s.a, &s.b)) return false;
                        if (hk_str_eq_ascii_icase(nm, HK_STR("nth-child"))) s.kind = HK_SS_NTH_CHILD;
                        else if (hk_str_eq_ascii_icase(nm, HK_STR("nth-last-child"))) s.kind = HK_SS_NTH_LAST_CHILD;
                        else if (hk_str_eq_ascii_icase(nm, HK_STR("nth-of-type"))) s.kind = HK_SS_NTH_OF_TYPE;
                        else s.kind = HK_SS_NTH_LAST_OF_TYPE;
                    }
                    *spec += 1u << 10;
                } else if (hk_str_eq_ascii_icase(nm, HK_STR("has")) || hk_str_eq_ascii_icase(nm, HK_STR("lang")) ||
                           hk_str_eq_ascii_icase(nm, HK_STR("dir")) || hk_str_eq_ascii_icase(nm, HK_STR("host")) ||
                           hk_str_eq_ascii_icase(nm, HK_STR("state"))) {
                    s.kind = HK_SS_NEVER; /* :has() arrives in M5 */
                    *spec += 1u << 10;
                } else {
                    return false;
                }
                p->i = end;
            } else {
                return false;
            }
        } else {
            break;
        }
        hk_vec_push(out, s);
        *empty = false;
    }
    return true;
}

/* Parses one complex selector spanning t[0..n). */
static bool parse_complex(sel_parser *p, const hk_css_token *t, uint32_t n, hk_css_selector *out)
{
    sel_parser sp = {p->a, p->atoms, t, n, 0};
    HK_VEC(hk_css_compound) comps = {0};
    simple_vec simples = {0};
    uint32_t spec = 0;
    bool ok = true;
    uint8_t pending = HK_COMB_NONE;
    sp.i = skip_ws(t, n, 0);
    while (sp.i < n) {
        bool empty;
        if (!parse_compound(&sp, &simples, &spec, &empty) || empty) {
            ok = false;
            break;
        }
        hk_css_compound c = {0};
        hk_css_simple *arr = HK_ARENA_NEW_ARRAY(p->a, hk_css_simple, simples.len);
        memcpy(arr, simples.data, simples.len * sizeof *arr);
        c.s = arr;
        c.n = simples.len;
        c.combinator = pending;
        hk_vec_push(&comps, c);
        /* Combinator. */
        uint32_t j = skip_ws(t, n, sp.i);
        bool had_ws = j > sp.i;
        sp.i = j;
        if (sp.i >= n) break;
        if (is_delim(&t[sp.i], '>')) pending = HK_COMB_CHILD, sp.i++;
        else if (is_delim(&t[sp.i], '+')) pending = HK_COMB_NEXT, sp.i++;
        else if (is_delim(&t[sp.i], '~')) pending = HK_COMB_SUBSEQUENT, sp.i++;
        else if (had_ws) pending = HK_COMB_DESCENDANT;
        else {
            ok = false;
            break;
        }
        sp.i = skip_ws(t, n, sp.i);
        if (sp.i >= n) {
            ok = false;
            break;
        }
    }
    if (ok && comps.len == 0) ok = false;
    if (ok) {
        /* Store right to left: c[0] is the subject; each compound's combinator links it to c[i+1]. */
        hk_css_compound *arr = HK_ARENA_NEW_ARRAY(p->a, hk_css_compound, comps.len);
        for (uint32_t k = 0; k < comps.len; k++) {
            arr[k] = comps.data[comps.len - 1 - k];
            arr[k].combinator = k + 1 < comps.len ? comps.data[comps.len - 1 - k].combinator : HK_COMB_NONE;
        }
        out->c = arr;
        out->n = comps.len;
        out->specificity = spec;
    }
    hk_vec_free(&comps);
    hk_vec_free(&simples);
    return ok;
}

static bool parse_list_into(sel_parser *p, const hk_css_token *t, uint32_t n, bool forgiving,
                            hk_css_selector_list *out)
{
    HK_VEC(hk_css_selector) sels = {0};
    uint32_t start = 0;
    bool ok = true;
    for (uint32_t i = 0; i <= n; i = (i < n ? skip_component(t, n, i) : n + 1)) {
        if (i < n && t[i].type != HK_CT_COMMA) continue;
        hk_css_selector s = {0};
        if (parse_complex(p, t + start, i - start, &s)) hk_vec_push(&sels, s);
        else if (!forgiving) ok = false;
        start = i + 1;
        if (i >= n) break;
    }
    if (ok) {
        hk_css_selector *arr = HK_ARENA_NEW_ARRAY(p->a, hk_css_selector, sels.len);
        if (sels.len) memcpy(arr, sels.data, sels.len * sizeof *arr);
        out->sel = arr;
        out->n = sels.len;
    }
    hk_vec_free(&sels);
    return ok && (forgiving || out->n > 0);
}

bool hk_css_parse_selectors(hk_arena *a, hk_atom_table *atoms, const hk_css_token *t, uint32_t n,
                            hk_css_selector_list *out)
{
    sel_parser p = {a, atoms, t, n, 0};
    span s = trim((span){t, n});
    return parse_list_into(&p, s.t, s.n, false, out);
}

/* --- media queries ---------------------------------------------------------------------------- */

static bool length_px(const hk_css_token *t, float *px)
{
    if (t->type == HK_CT_NUMBER && t->num == 0) {
        *px = 0;
        return true;
    }
    if (t->type != HK_CT_DIMENSION) return false;
    double v = t->num;
    hk_str u = t->unit;
    if (hk_str_eq_ascii_icase(u, HK_STR("px"))) *px = (float)v;
    else if (hk_str_eq_ascii_icase(u, HK_STR("em")) || hk_str_eq_ascii_icase(u, HK_STR("rem"))) *px = (float)(v * 16);
    else if (hk_str_eq_ascii_icase(u, HK_STR("pt"))) *px = (float)(v * 96 / 72);
    else if (hk_str_eq_ascii_icase(u, HK_STR("cm"))) *px = (float)(v * 96 / 2.54);
    else if (hk_str_eq_ascii_icase(u, HK_STR("mm"))) *px = (float)(v * 96 / 25.4);
    else if (hk_str_eq_ascii_icase(u, HK_STR("in"))) *px = (float)(v * 96);
    else return false;
    return true;
}

static bool compare(float a, const char *op, float b)
{
    if (strcmp(op, "<") == 0) return a < b;
    if (strcmp(op, "<=") == 0) return a <= b;
    if (strcmp(op, ">") == 0) return a > b;
    if (strcmp(op, ">=") == 0) return a >= b;
    return fabsf(a - b) < 0.01f;
}

/* Evaluates the inside of one "( ... )" media feature. */
static bool media_feature(const hk_css_token *t, uint32_t n, const hk_css_media_env *env)
{
    span s = trim((span){t, n});
    if (s.n == 0) return false;
    /* Collect the significant tokens. */
    const hk_css_token *v[8];
    uint32_t k = 0;
    for (uint32_t i = 0; i < s.n && k < 8; i++) {
        if (s.t[i].type != HK_CT_WS) v[k++] = &s.t[i];
    }
    if (v[0]->type == HK_CT_IDENT) {
        hk_str name = v[0]->value;
        if (k == 1) {
            /* Boolean context. */
            if (hk_str_eq_ascii_icase(name, HK_STR("color")) || hk_str_eq_ascii_icase(name, HK_STR("hover")) ||
                hk_str_eq_ascii_icase(name, HK_STR("pointer")) || hk_str_eq_ascii_icase(name, HK_STR("any-hover")) ||
                hk_str_eq_ascii_icase(name, HK_STR("any-pointer")) || hk_str_eq_ascii_icase(name, HK_STR("width")) ||
                hk_str_eq_ascii_icase(name, HK_STR("height")))
                return true;
            return false;
        }
        if (k >= 3 && v[1]->type == HK_CT_COLON) {
            const hk_css_token *val = v[2];
            float px;
            bool is_min = hk_str_starts_with(name, HK_STR("min-")), is_max = hk_str_starts_with(name, HK_STR("max-"));
            hk_str base = (is_min || is_max) ? hk_str_slice(name, 4, name.n) : name;
            if (hk_str_eq_ascii_icase(base, HK_STR("width")) || hk_str_eq_ascii_icase(base, HK_STR("height"))) {
                if (!length_px(val, &px)) return false;
                float actual = hk_str_eq_ascii_icase(base, HK_STR("width")) ? env->width : env->height;
                return is_min ? actual >= px : is_max ? actual <= px : fabsf(actual - px) < 0.5f;
            }
            if (hk_str_eq_ascii_icase(base, HK_STR("aspect-ratio")) || hk_str_eq_ascii_icase(base, HK_STR("resolution")) ||
                hk_str_eq_ascii_icase(base, HK_STR("device-pixel-ratio")) ||
                hk_str_eq_ascii_icase(base, HK_STR("-webkit-device-pixel-ratio"))) {
                double num = val->num;
                if (val->type == HK_CT_DIMENSION && hk_str_eq_ascii_icase(val->unit, HK_STR("dpi"))) num /= 96;
                return is_min ? 1.0 >= num : is_max ? 1.0 <= num : num == 1.0;
            }
            if (val->type != HK_CT_IDENT) return false;
            hk_str kw = val->value;
            if (hk_str_eq_ascii_icase(name, HK_STR("orientation")))
                return hk_str_eq_ascii_icase(kw, env->width > env->height ? HK_STR("landscape") : HK_STR("portrait"));
            if (hk_str_eq_ascii_icase(name, HK_STR("prefers-color-scheme")))
                return hk_str_eq_ascii_icase(kw, env->dark ? HK_STR("dark") : HK_STR("light"));
            if (hk_str_eq_ascii_icase(name, HK_STR("prefers-reduced-motion"))) return hk_str_eq_ascii_icase(kw, HK_STR("reduce"));
            if (hk_str_eq_ascii_icase(name, HK_STR("scripting"))) return hk_str_eq_ascii_icase(kw, HK_STR("none"));
            if (hk_str_eq_ascii_icase(name, HK_STR("hover")) || hk_str_eq_ascii_icase(name, HK_STR("any-hover")))
                return hk_str_eq_ascii_icase(kw, HK_STR("hover"));
            if (hk_str_eq_ascii_icase(name, HK_STR("pointer")) || hk_str_eq_ascii_icase(name, HK_STR("any-pointer")))
                return hk_str_eq_ascii_icase(kw, HK_STR("fine"));
            if (hk_str_eq_ascii_icase(name, HK_STR("forced-colors")) ||
                hk_str_eq_ascii_icase(name, HK_STR("inverted-colors")))
                return hk_str_eq_ascii_icase(kw, HK_STR("none"));
            if (hk_str_eq_ascii_icase(name, HK_STR("prefers-contrast")) ||
                hk_str_eq_ascii_icase(name, HK_STR("prefers-reduced-transparency")) ||
                hk_str_eq_ascii_icase(name, HK_STR("prefers-reduced-data")))
                return hk_str_eq_ascii_icase(kw, HK_STR("no-preference"));
            if (hk_str_eq_ascii_icase(name, HK_STR("display-mode"))) return hk_str_eq_ascii_icase(kw, HK_STR("browser"));
            return false;
        }
    }
    /* Range syntax: (width >= 600px), (400px <= width < 800px). */
    char ops[2][3] = {{0}, {0}};
    const hk_css_token *items[3];
    uint32_t ni = 0, no = 0;
    for (uint32_t i = 0; i < k; i++) {
        if (v[i]->type == HK_CT_DELIM && (v[i]->delim == '<' || v[i]->delim == '>' || v[i]->delim == '=')) {
            if (no >= 2) return false;
            char *o = ops[no++];
            o[0] = (char)v[i]->delim;
            if (i + 1 < k && is_delim(v[i + 1], '=') && v[i]->delim != '=') {
                o[1] = '=';
                i++;
            }
        } else {
            if (ni >= 3) return false;
            items[ni++] = v[i];
        }
    }
    if (no == 0 || ni != no + 1) return false;
    float vals[3];
    int feature = -1;
    for (uint32_t i = 0; i < ni; i++) {
        if (items[i]->type == HK_CT_IDENT) {
            if (hk_css_ident_is(items[i], "width")) vals[i] = env->width;
            else if (hk_css_ident_is(items[i], "height")) vals[i] = env->height;
            else return false;
            feature = (int)i;
        } else if (!length_px(items[i], &vals[i])) {
            return false;
        }
    }
    if (feature < 0) return false;
    for (uint32_t i = 0; i < no; i++) {
        if (!compare(vals[i], ops[i], vals[i + 1])) return false;
    }
    return true;
}

/* Evaluates a media condition / query over t[0..n): handles not/and/or/only and media types. */
static bool media_condition(const hk_css_token *t, uint32_t n, const hk_css_media_env *env)
{
    bool result = true, negate = false, have = false;
    int op = 0; /* 0 = and, 1 = or */
    for (uint32_t i = skip_ws(t, n, 0); i < n; i = skip_ws(t, n, i)) {
        const hk_css_token *k = &t[i];
        bool value;
        if (k->type == HK_CT_IDENT) {
            if (hk_css_ident_is(k, "not")) {
                negate = !negate;
                i++;
                continue;
            }
            if (hk_css_ident_is(k, "only")) {
                i++;
                continue;
            }
            if (hk_css_ident_is(k, "and")) {
                op = 0;
                i++;
                continue;
            }
            if (hk_css_ident_is(k, "or")) {
                op = 1;
                i++;
                continue;
            }
            value = hk_css_ident_is(k, "all") || hk_css_ident_is(k, "screen");
            i++;
        } else if (k->type == HK_CT_LPAREN) {
            uint32_t end = skip_component(t, n, i);
            /* A nested condition "((a) and (b))" or a feature. */
            uint32_t inner = skip_ws(t, end - 1, i + 1);
            if (inner < end - 1 && (t[inner].type == HK_CT_LPAREN || hk_css_ident_is(&t[inner], "not")))
                value = media_condition(t + i + 1, end - i - 2, env);
            else
                value = media_feature(t + i + 1, end - i - 2, env);
            i = end;
        } else {
            return false; /* malformed query: "not all" */
        }
        if (negate) value = !value;
        negate = false;
        if (!have) result = value;
        else if (op == 0) result = result && value;
        else result = result || value;
        have = true;
    }
    return have ? result : true;
}

bool hk_css_media_matches(const hk_css_token *t, uint32_t n, const hk_css_media_env *env)
{
    span s = trim((span){t, n});
    if (s.n == 0) return true;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= s.n; i = i < s.n ? skip_component(s.t, s.n, i) : s.n + 1) {
        if (i < s.n && s.t[i].type != HK_CT_COMMA) continue;
        if (media_condition(s.t + start, i - start, env)) return true;
        start = i + 1;
        if (i >= s.n) break;
    }
    return false;
}

bool hk_css_condition_matches(const hk_css_condition *c, const hk_css_media_env *env)
{
    for (; c; c = c->parent) {
        if (!hk_css_media_matches(c->media, c->media_n, env)) return false;
    }
    return true;
}

/* --- @supports ---------------------------------------------------------------------------------- */

static bool supports_condition(const hk_css_token *t, uint32_t n)
{
    bool result = true, negate = false, have = false;
    int op = 0;
    for (uint32_t i = skip_ws(t, n, 0); i < n; i = skip_ws(t, n, i)) {
        const hk_css_token *k = &t[i];
        bool value = false;
        if (hk_css_ident_is(k, "not")) {
            negate = !negate;
            i++;
            continue;
        }
        if (hk_css_ident_is(k, "and")) {
            op = 0;
            i++;
            continue;
        }
        if (hk_css_ident_is(k, "or")) {
            op = 1;
            i++;
            continue;
        }
        if (k->type == HK_CT_LPAREN) {
            uint32_t end = skip_component(t, n, i);
            uint32_t j = skip_ws(t, end - 1, i + 1);
            if (j < end - 1 && t[j].type == HK_CT_IDENT && skip_ws(t, end - 1, j + 1) < end - 1 &&
                t[skip_ws(t, end - 1, j + 1)].type == HK_CT_COLON) {
                hk_str name = t[j].value;
                value = (name.n > 2 && name.p[0] == '-' && name.p[1] == '-') || hk_css_property_from_name(name) != 0;
            } else {
                value = supports_condition(t + i + 1, end - i - 2);
            }
            i = end;
        } else if (k->type == HK_CT_FUNCTION) {
            i = skip_component(t, n, i); /* selector(), font-tech(), ...: not claimed */
        } else {
            return false;
        }
        if (negate) value = !value;
        negate = false;
        if (!have) result = value;
        else result = op == 0 ? (result && value) : (result || value);
        have = true;
    }
    return have && result;
}

/* --- stylesheets ---------------------------------------------------------------------------------- */

typedef struct sheet_parser {
    hk_arena *a;
    hk_atom_table *atoms;
    hk_css_sheet *sheet;
    bool rules_seen;
    /* filtering: tokens live in tmp and are copied into a when a rule is kept */
    hk_arena *tmp;
    hk_str src;
    bool (*keep)(void *user, const hk_css_selector *sel);
    void *user;
} sheet_parser;

/* A string of a token: inside the source (kept as is) or unescaped in the temporary arena (copied). */
static hk_str own_str(sheet_parser *sp, hk_str s)
{
    if (!sp->tmp || !s.n || (s.p >= sp->src.p && s.p + s.n <= sp->src.p + sp->src.n)) return s;
    return hk_arena_str_dup(sp->a, s);
}

/* Tokens referenced by the sheet: copied out of the temporary arena when filtering. */
static const hk_css_token *own_tokens(sheet_parser *sp, const hk_css_token *t, uint32_t n)
{
    if (!sp->tmp || !n) return t;
    hk_css_token *c = HK_ARENA_NEW_ARRAY(sp->a, hk_css_token, n);
    for (uint32_t i = 0; i < n; i++) {
        c[i] = t[i];
        c[i].value = own_str(sp, t[i].value);
        c[i].unit = own_str(sp, t[i].unit);
    }
    return c;
}

static void parse_rules(sheet_parser *sp, const hk_css_token *t, uint32_t n, const hk_css_condition *cond, bool top);

/* Attribute values in selectors may be unescaped strings of the temporary arena: copy them. */
static void own_selectors(sheet_parser *sp, const hk_css_selector_list *l)
{
    for (uint32_t i = 0; i < l->n; i++)
        for (uint32_t j = 0; j < l->sel[i].n; j++)
            for (uint32_t k = 0; k < l->sel[i].c[j].n; k++) {
                hk_css_simple *x = (hk_css_simple *)&l->sel[i].c[j].s[k];
                x->value = own_str(sp, x->value);
                if (x->inner) own_selectors(sp, x->inner);
            }
}

static void style_rule(sheet_parser *sp, span prelude, span block, const hk_css_condition *cond)
{
    hk_css_rule r = {0};
    if (sp->keep) {
        /* Selectors are checked in the temporary arena first; most rules of big sheets stop here. */
        hk_arena_mark m = hk_arena_get_mark(sp->tmp);
        hk_css_selector_list probe;
        bool any = false;
        if (hk_css_parse_selectors(sp->tmp, sp->atoms, prelude.t, prelude.n, &probe))
            for (uint32_t i = 0; i < probe.n && !any; i++) any = sp->keep(sp->user, &probe.sel[i]);
        hk_arena_rewind(sp->tmp, m);
        if (!any) return;
    }
    if (!hk_css_parse_selectors(sp->a, sp->atoms, prelude.t, prelude.n, &r.selectors)) return;
    if (sp->tmp) own_selectors(sp, &r.selectors);
    decl_vec decls = {0};
    parse_declarations(block.t, block.n, &decls);
    if (decls.len == 0) {
        hk_vec_free(&decls);
        return;
    }
    hk_css_decl *arr = HK_ARENA_NEW_ARRAY(sp->a, hk_css_decl, decls.len);
    memcpy(arr, decls.data, decls.len * sizeof *arr);
    for (uint32_t i = 0; i < decls.len; i++) {
        arr[i].v = own_tokens(sp, arr[i].v, arr[i].vn);
        arr[i].custom = own_str(sp, arr[i].custom);
    }
    r.decls = arr;
    r.ndecls = decls.len;
    r.cond = cond;
    hk_vec_push(&sp->sheet->rules, r);
    hk_vec_free(&decls);
}

static void at_rule(sheet_parser *sp, const hk_css_token *at, span prelude, span block, bool has_block,
                    const hk_css_condition *cond, bool top)
{
    hk_str name = at->value;
    if (hk_str_eq_ascii_icase(name, HK_STR("media")) && has_block) {
        hk_css_condition *c = HK_ARENA_NEW(sp->a, hk_css_condition);
        c->parent = cond;
        c->media = own_tokens(sp, prelude.t, prelude.n);
        c->media_n = prelude.n;
        parse_rules(sp, block.t, block.n, c, false);
    } else if (hk_str_eq_ascii_icase(name, HK_STR("supports")) && has_block) {
        if (supports_condition(prelude.t, prelude.n)) parse_rules(sp, block.t, block.n, cond, false);
    } else if (hk_str_eq_ascii_icase(name, HK_STR("layer")) && has_block) {
        parse_rules(sp, block.t, block.n, cond, false); /* layer ordering is approximated by source order */
    } else if (hk_str_eq_ascii_icase(name, HK_STR("import")) && !has_block && top && !sp->rules_seen) {
        span p = trim(prelude);
        if (p.n && (p.t[0].type == HK_CT_URL || p.t[0].type == HK_CT_STRING)) {
            hk_vec_push(&sp->sheet->imports, own_str(sp, p.t[0].value));
        } else if (p.n && p.t[0].type == HK_CT_FUNCTION && hk_str_eq_ascii_icase(p.t[0].value, HK_STR("url"))) {
            uint32_t j = skip_ws(p.t, p.n, 1);
            if (j < p.n && p.t[j].type == HK_CT_STRING) hk_vec_push(&sp->sheet->imports, own_str(sp, p.t[j].value));
        }
        return;
    }
    /* Everything else (@font-face, @keyframes, @page, @container, ...) is ignored. */
}

static void parse_rules(sheet_parser *sp, const hk_css_token *t, uint32_t n, const hk_css_condition *cond, bool top)
{
    uint32_t i = 0;
    while (i < n) {
        uint8_t ty = t[i].type;
        if (ty == HK_CT_EOF) break;
        if (ty == HK_CT_WS || (top && (ty == HK_CT_CDO || ty == HK_CT_CDC))) {
            i++;
            continue;
        }
        if (ty == HK_CT_AT) {
            const hk_css_token *at = &t[i];
            uint32_t j = i + 1;
            while (j < n && t[j].type != HK_CT_SEMI && t[j].type != HK_CT_LBRACE && t[j].type != HK_CT_EOF)
                j = skip_component(t, n, j);
            span prelude = trim((span){t + i + 1, j - i - 1});
            if (j < n && t[j].type == HK_CT_LBRACE) {
                uint32_t end = skip_component(t, n, j);
                span block = {t + j + 1, end - j - 1};
                if (block.n && t[end - 1].type == HK_CT_RBRACE) block.n--;
                at_rule(sp, at, prelude, block, true, cond, top);
                i = end;
            } else {
                at_rule(sp, at, prelude, (span){0}, false, cond, top);
                i = j + 1;
            }
            if (!hk_str_eq_ascii_icase(at->value, HK_STR("import")) && !hk_str_eq_ascii_icase(at->value, HK_STR("charset")))
                sp->rules_seen = true;
            continue;
        }
        /* Qualified rule. */
        uint32_t j = i;
        while (j < n && t[j].type != HK_CT_LBRACE && t[j].type != HK_CT_EOF) j = skip_component(t, n, j);
        if (j >= n || t[j].type != HK_CT_LBRACE) break; /* no block: dropped */
        uint32_t end = skip_component(t, n, j);
        span block = {t + j + 1, end - j - 1};
        if (block.n && t[end - 1].type == HK_CT_RBRACE) block.n--;
        style_rule(sp, (span){t + i, j - i}, block, cond);
        sp->rules_seen = true;
        i = end;
    }
}

void hk_css_parse_sheet(hk_arena *a, hk_atom_table *atoms, hk_str src, hk_css_sheet *out)
{
    memset(out, 0, sizeof *out);
    out->arena = a;
    out->source = src;
    hk_css_tokens toks = hk_css_tokenize(a, src);
    sheet_parser sp = {a, atoms, out, false, NULL, src, NULL, NULL};
    parse_rules(&sp, toks.t, toks.n, NULL, true);
}

void hk_css_parse_sheet_filtered(hk_arena *a, hk_atom_table *atoms, hk_str src,
                                 bool (*keep)(void *user, const hk_css_selector *sel), void *user,
                                 hk_css_sheet *out)
{
    memset(out, 0, sizeof *out);
    out->arena = a;
    out->source = src;
    hk_arena tmp;
    hk_arena_init(&tmp, 0);
    hk_css_tokens toks = hk_css_tokenize(&tmp, src);
    sheet_parser sp = {a, atoms, out, false, &tmp, src, keep, user};
    parse_rules(&sp, toks.t, toks.n, NULL, true);
    hk_arena_free(&tmp);
}

void hk_css_sheet_free(hk_css_sheet *s)
{
    hk_vec_free(&s->rules);
    hk_vec_free(&s->imports);
}
