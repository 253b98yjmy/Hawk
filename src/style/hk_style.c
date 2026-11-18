/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "style/hk_style.h"
#include "base/hk_map64.h"
#include "base/hk_mem.h"
#include "css/hk_css_props.h"
#include "plat/hk_text.h"
#include "style/hk_css_values.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "style/hk_ua_css.inc"

#define A(x) HK_ATOM_##x

enum { ORIGIN_UA = 0, ORIGIN_AUTHOR = 1 };

typedef struct rule_ref {
    const hk_css_rule *rule;
    const hk_css_selector *sel;
    uint32_t order;
    uint8_t origin;
} rule_ref;

typedef HK_VEC(rule_ref) ref_vec;

struct hk_custom_props {
    const hk_custom_props *parent;
    uint32_t n;
    const hk_str *names;
    const hk_cv_span *values;
};

typedef struct inline_decls {
    const hk_css_decl *d;
    uint32_t n;
    const hk_css_decl *hints; /* presentational attributes */
    uint32_t nhints;
    bool parsed;
} inline_decls;

struct hk_styler {
    hk_document *doc;
    HK_VEC(hk_css_sheet) sheets;
    HK_VEC(uint8_t) origins;
    /* rule index */
    bool index_valid;
    hk_map64 by_id, by_class, by_tag;
    HK_VEC(ref_vec) buckets;
    ref_vec universal;
    /* per node (indexed by node id) */
    HK_VEC(const hk_style *) styles;
    HK_VEC(uint32_t) class_first;
    HK_VEC(uint16_t) class_count;
    HK_VEC(hk_atom) class_atoms;
    HK_VEC(inline_decls) inline_style;
    /* computed styles */
    hk_arena style_arena;
    hk_map64 interned;
    uint32_t distinct;
    /* font families */
    HK_VEC(hk_str) families;
    hk_map64 family_cache;
    hk_arena family_arena;
    hk_css_media_env env;
    float root_font_size;
    uint32_t elements;
};

/* --- setup -------------------------------------------------------------------------------- */

static void add_sheet(hk_styler *s, hk_str css, uint8_t origin)
{
    hk_css_sheet sheet;
    hk_str copy = hk_arena_str_dup(&s->doc->arena, css);
    hk_css_parse_sheet(&s->doc->arena, &s->doc->atoms, copy, &sheet);
    hk_vec_push(&s->sheets, sheet);
    hk_vec_push(&s->origins, origin);
    s->index_valid = false;
}

static uint16_t family_index(hk_styler *s, hk_str name);

hk_styler *hk_styler_create(hk_document *doc)
{
    hk_styler *s = hk_alloc_zero(sizeof *s);
    s->doc = doc;
    hk_arena_init(&s->style_arena, 0);
    hk_arena_init(&s->family_arena, 4096);
    hk_map64_init(&s->by_id);
    hk_map64_init(&s->by_class);
    hk_map64_init(&s->by_tag);
    hk_map64_init(&s->interned);
    hk_map64_init(&s->family_cache);
    family_index(s, HK_STR("Segoe UI")); /* index 0: the default family */
    add_sheet(s, (hk_str){k_ua_css, sizeof k_ua_css - 1}, ORIGIN_UA);
    if (doc->quirks == HK_QUIRKS_FULL) add_sheet(s, (hk_str){k_quirks_css, sizeof k_quirks_css - 1}, ORIGIN_UA);
    return s;
}

static void free_index(hk_styler *s)
{
    for (uint32_t i = 0; i < s->buckets.len; i++) hk_vec_free(&s->buckets.data[i]);
    hk_vec_free(&s->buckets);
    hk_vec_free(&s->universal);
    hk_map64_free(&s->by_id);
    hk_map64_free(&s->by_class);
    hk_map64_free(&s->by_tag);
    hk_map64_init(&s->by_id);
    hk_map64_init(&s->by_class);
    hk_map64_init(&s->by_tag);
}

void hk_styler_destroy(hk_styler *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < s->sheets.len; i++) hk_css_sheet_free(&s->sheets.data[i]);
    hk_vec_free(&s->sheets);
    hk_vec_free(&s->origins);
    free_index(s);
    hk_map64_free(&s->by_id);
    hk_map64_free(&s->by_class);
    hk_map64_free(&s->by_tag);
    hk_vec_free(&s->styles);
    hk_vec_free(&s->class_first);
    hk_vec_free(&s->class_count);
    hk_vec_free(&s->class_atoms);
    hk_vec_free(&s->inline_style);
    hk_map64_free(&s->interned);
    hk_map64_free(&s->family_cache);
    hk_vec_free(&s->families);
    hk_arena_free(&s->style_arena);
    hk_arena_free(&s->family_arena);
    hk_free(s, sizeof *s);
}

void hk_styler_add_sheet(hk_styler *s, hk_str css)
{
    add_sheet(s, css, ORIGIN_AUTHOR);
}

uint32_t hk_styler_sheet_count(const hk_styler *s)
{
    return s->sheets.len - 1;
}

const hk_css_sheet *hk_styler_last_sheet(const hk_styler *s)
{
    return &s->sheets.data[s->sheets.len - 1];
}

hk_str hk_styler_family(const hk_styler *s, uint16_t index)
{
    return index < s->families.len ? s->families.data[index] : HK_STR("Segoe UI");
}

/* --- rule index ------------------------------------------------------------------------------ */

static void bucket_add(hk_styler *s, hk_map64 *m, hk_atom key, rule_ref r)
{
    uint64_t v;
    if (!hk_map64_get(m, key, &v)) {
        ref_vec empty = {0};
        hk_vec_push(&s->buckets, empty);
        v = s->buckets.len - 1;
        hk_map64_put(m, key, v);
    }
    hk_vec_push(&s->buckets.data[v], r);
}

static void build_index(hk_styler *s)
{
    free_index(s);
    uint32_t order = 0;
    for (uint32_t si = 0; si < s->sheets.len; si++) {
        const hk_css_sheet *sh = &s->sheets.data[si];
        for (uint32_t ri = 0; ri < sh->rules.len; ri++) {
            const hk_css_rule *r = &sh->rules.data[ri];
            order++;
            for (uint32_t k = 0; k < r->selectors.n; k++) {
                const hk_css_selector *sel = &r->selectors.sel[k];
                rule_ref ref = {r, sel, order, s->origins.data[si]};
                const hk_css_compound *subject = &sel->c[0];
                hk_atom id = 0, cls = 0, tag = 0;
                for (uint32_t q = 0; q < subject->n; q++) {
                    const hk_css_simple *x = &subject->s[q];
                    if (x->kind == HK_SS_ID && !id) id = x->name;
                    if (x->kind == HK_SS_CLASS && !cls) cls = x->name;
                    if (x->kind == HK_SS_TYPE && !tag) tag = x->name;
                    if (x->kind == HK_SS_NEVER) goto skip; /* can never match: not indexed */
                }
                if (id) bucket_add(s, &s->by_id, id, ref);
                else if (cls) bucket_add(s, &s->by_class, cls, ref);
                else if (tag) bucket_add(s, &s->by_tag, tag, ref);
                else hk_vec_push(&s->universal, ref);
            skip:;
            }
        }
    }
    s->index_valid = true;
}

/* --- per-element data -------------------------------------------------------------------------- */

static void prepare_nodes(hk_styler *s)
{
    hk_document *d = s->doc;
    uint32_t n = d->nodes.len;
    if (s->class_first.len == n) return;
    s->class_first.len = s->class_count.len = 0;
    s->class_atoms.len = 0;
    hk_vec_reserve(&s->class_first, n);
    hk_vec_reserve(&s->class_count, n);
    s->inline_style.len = 0;
    hk_vec_reserve(&s->inline_style, n);
    for (uint32_t id = 0; id < n; id++) {
        s->class_first.data[id] = s->class_atoms.len;
        uint16_t count = 0;
        if (id && d->nodes.data[id].type == HK_NODE_ELEMENT) {
            const hk_attr *cls = hk_dom_attr(d, id, HK_ATTR_NS_NONE, A(class));
            if (cls) {
                hk_str v = cls->value;
                size_t i = 0;
                while (i < v.n) {
                    while (i < v.n && hk_ascii_is_ws(v.p[i])) i++;
                    size_t st = i;
                    while (i < v.n && !hk_ascii_is_ws(v.p[i])) i++;
                    if (i > st && count < UINT16_MAX) {
                        hk_atom a = hk_atom_intern(&d->atoms, hk_str_slice(v, st, i));
                        hk_vec_push(&s->class_atoms, a);
                        count++;
                    }
                }
            }
        }
        s->class_count.data[id] = count;
        inline_decls none = {0};
        s->inline_style.data[id] = none;
    }
    s->class_first.len = s->class_count.len = s->inline_style.len = n;
}

static bool has_class(const hk_styler *s, hk_node_id el, hk_atom cls)
{
    uint32_t first = s->class_first.data[el];
    for (uint32_t i = 0; i < s->class_count.data[el]; i++) {
        if (s->class_atoms.data[first + i] == cls) return true;
    }
    return false;
}

/* --- selector matching -------------------------------------------------------------------------- */

static hk_node_id parent_element(const hk_document *d, hk_node_id el)
{
    hk_node_id p = hk_dom_node(d, el)->parent;
    return p && hk_dom_node(d, p)->type == HK_NODE_ELEMENT ? p : HK_NODE_NONE;
}

static hk_node_id prev_element(const hk_document *d, hk_node_id el)
{
    for (hk_node_id c = hk_dom_node(d, el)->prev; c; c = hk_dom_node(d, c)->prev) {
        if (hk_dom_node(d, c)->type == HK_NODE_ELEMENT) return c;
    }
    return HK_NODE_NONE;
}

static hk_node_id next_element(const hk_document *d, hk_node_id el)
{
    for (hk_node_id c = hk_dom_node(d, el)->next; c; c = hk_dom_node(d, c)->next) {
        if (hk_dom_node(d, c)->type == HK_NODE_ELEMENT) return c;
    }
    return HK_NODE_NONE;
}

static bool nth_matches(int32_t a, int32_t b, int32_t index)
{
    if (a == 0) return index == b;
    int32_t diff = index - b;
    return diff / a >= 0 && diff % a == 0;
}

static int32_t position(const hk_document *d, hk_node_id el, bool from_end, bool of_type)
{
    hk_atom name = hk_dom_element(d, el)->name;
    int32_t i = 1;
    for (hk_node_id c = from_end ? next_element(d, el) : prev_element(d, el); c;
         c = from_end ? next_element(d, c) : prev_element(d, c)) {
        if (!of_type || hk_dom_element(d, c)->name == name) i++;
    }
    return i;
}

static bool attr_matches(const hk_css_simple *x, hk_str v)
{
    hk_str want = x->value;
    bool (*eq)(hk_str, hk_str) = x->icase ? hk_str_eq_ascii_icase : hk_str_eq;
    switch (x->kind) {
    case HK_SS_ATTR_EXISTS: return true;
    case HK_SS_ATTR_EQ: return eq(v, want);
    case HK_SS_ATTR_INCLUDES: {
        if (want.n == 0) return false;
        size_t i = 0;
        while (i < v.n) {
            while (i < v.n && hk_ascii_is_ws(v.p[i])) i++;
            size_t st = i;
            while (i < v.n && !hk_ascii_is_ws(v.p[i])) i++;
            if (i > st && eq(hk_str_slice(v, st, i), want)) return true;
        }
        return false;
    }
    case HK_SS_ATTR_DASH:
        return eq(v, want) || (v.n > want.n && v.p[want.n] == '-' && eq(hk_str_slice(v, 0, want.n), want));
    case HK_SS_ATTR_PREFIX: return want.n && v.n >= want.n && eq(hk_str_slice(v, 0, want.n), want);
    case HK_SS_ATTR_SUFFIX: return want.n && v.n >= want.n && eq(hk_str_slice(v, v.n - want.n, v.n), want);
    case HK_SS_ATTR_SUBSTR:
        if (!want.n) return false;
        for (size_t i = 0; i + want.n <= v.n; i++) {
            if (eq(hk_str_slice(v, i, i + want.n), want)) return true;
        }
        return false;
    default: return false;
    }
}

static bool match_list(const hk_styler *s, const hk_css_selector_list *l, hk_node_id el);

static bool match_compound(const hk_styler *s, const hk_css_compound *c, hk_node_id el)
{
    const hk_document *d = s->doc;
    const hk_node *node = hk_dom_node(d, el);
    const hk_element *e = hk_dom_element(d, el);
    for (uint32_t i = 0; i < c->n; i++) {
        const hk_css_simple *x = &c->s[i];
        switch (x->kind) {
        case HK_SS_TYPE:
            if (e->name != x->name || node->ns != HK_NS_HTML) {
                /* Foreign elements keep their case; compare names case-insensitively there. */
                if (node->ns == HK_NS_HTML ||
                    !hk_str_eq_ascii_icase(hk_atom_str(&d->atoms, e->name), hk_atom_str(&d->atoms, x->name)))
                    return false;
            }
            break;
        case HK_SS_UNIVERSAL: break;
        case HK_SS_ID: {
            const hk_attr *id = hk_dom_attr(d, el, HK_ATTR_NS_NONE, A(id));
            if (!id || !hk_str_eq(id->value, hk_atom_str(&d->atoms, x->name))) return false;
            break;
        }
        case HK_SS_CLASS:
            if (!has_class(s, el, x->name)) return false;
            break;
        case HK_SS_ATTR_EXISTS:
        case HK_SS_ATTR_EQ:
        case HK_SS_ATTR_INCLUDES:
        case HK_SS_ATTR_DASH:
        case HK_SS_ATTR_PREFIX:
        case HK_SS_ATTR_SUFFIX:
        case HK_SS_ATTR_SUBSTR: {
            const hk_attr *a = hk_dom_attr(d, el, HK_ATTR_NS_NONE, x->name);
            if (!a || !attr_matches(x, a->value)) return false;
            break;
        }
        case HK_SS_ROOT:
            if (parent_element(d, el)) return false;
            break;
        case HK_SS_EMPTY:
            for (hk_node_id ch = node->first_child; ch; ch = hk_dom_node(d, ch)->next) {
                uint8_t ty = hk_dom_node(d, ch)->type;
                if (ty == HK_NODE_ELEMENT || (ty == HK_NODE_TEXT && hk_dom_text(d, ch).n)) return false;
            }
            break;
        case HK_SS_FIRST_CHILD:
            if (prev_element(d, el)) return false;
            break;
        case HK_SS_LAST_CHILD:
            if (next_element(d, el)) return false;
            break;
        case HK_SS_ONLY_CHILD:
            if (prev_element(d, el) || next_element(d, el)) return false;
            break;
        case HK_SS_FIRST_OF_TYPE:
            if (position(d, el, false, true) != 1) return false;
            break;
        case HK_SS_LAST_OF_TYPE:
            if (position(d, el, true, true) != 1) return false;
            break;
        case HK_SS_ONLY_OF_TYPE:
            if (position(d, el, false, true) != 1 || position(d, el, true, true) != 1) return false;
            break;
        case HK_SS_NTH_CHILD:
            if (!nth_matches(x->a, x->b, position(d, el, false, false))) return false;
            break;
        case HK_SS_NTH_LAST_CHILD:
            if (!nth_matches(x->a, x->b, position(d, el, true, false))) return false;
            break;
        case HK_SS_NTH_OF_TYPE:
            if (!nth_matches(x->a, x->b, position(d, el, false, true))) return false;
            break;
        case HK_SS_NTH_LAST_OF_TYPE:
            if (!nth_matches(x->a, x->b, position(d, el, true, true))) return false;
            break;
        case HK_SS_LINK:
            if (!((e->name == A(a) || e->name == A(area)) && node->ns == HK_NS_HTML &&
                  hk_dom_attr(d, el, HK_ATTR_NS_NONE, A(href))))
                return false;
            break;
        case HK_SS_NOT:
            if (match_list(s, x->inner, el)) return false;
            break;
        case HK_SS_IS:
            if (!match_list(s, x->inner, el)) return false;
            break;
        case HK_SS_NEVER:
        default: return false;
        }
    }
    return true;
}

/* Matches compounds c[i..n) against el and its context. */
static bool match_from(const hk_styler *s, const hk_css_selector *sel, uint32_t i, hk_node_id el)
{
    if (!match_compound(s, &sel->c[i], el)) return false;
    if (i + 1 == sel->n) return true;
    const hk_document *d = s->doc;
    switch (sel->c[i].combinator) {
    case HK_COMB_CHILD: {
        hk_node_id p = parent_element(d, el);
        return p && match_from(s, sel, i + 1, p);
    }
    case HK_COMB_DESCENDANT:
        for (hk_node_id p = parent_element(d, el); p; p = parent_element(d, p)) {
            if (match_from(s, sel, i + 1, p)) return true;
        }
        return false;
    case HK_COMB_NEXT: {
        hk_node_id p = prev_element(d, el);
        return p && match_from(s, sel, i + 1, p);
    }
    case HK_COMB_SUBSEQUENT:
        for (hk_node_id p = prev_element(d, el); p; p = prev_element(d, p)) {
            if (match_from(s, sel, i + 1, p)) return true;
        }
        return false;
    default: return false;
    }
}

static bool match_list(const hk_styler *s, const hk_css_selector_list *l, hk_node_id el)
{
    for (uint32_t i = 0; i < l->n; i++) {
        if (match_from(s, &l->sel[i], 0, el)) return true;
    }
    return false;
}

/* --- cascade -------------------------------------------------------------------------------------- */

typedef struct matched {
    const hk_css_decl *decl;
    uint64_t key; /* level << 56 | specificity << 24 | order */
} matched;

typedef HK_VEC(matched) matched_vec;

static int cmp_matched(const void *a, const void *b)
{
    uint64_t x = ((const matched *)a)->key, y = ((const matched *)b)->key;
    return x < y ? -1 : x > y;
}

static void add_decls(matched_vec *out, const hk_css_decl *d, uint32_t n, uint8_t origin, uint32_t spec,
                      uint32_t order)
{
    for (uint32_t i = 0; i < n; i++) {
        uint64_t level = origin == ORIGIN_UA ? (d[i].important ? 3 : 0) : (d[i].important ? 2 : 1);
        matched m = {&d[i], level << 62 | (uint64_t)(spec & 0x3FFFFFFF) << 32 | (uint64_t)(order & 0xFFFFFF) << 8 |
                                (i & 0xFF)};
        hk_vec_push(out, m);
    }
}

static void consider(const hk_styler *s, const ref_vec *refs, hk_node_id el, matched_vec *out)
{
    for (uint32_t i = 0; i < refs->len; i++) {
        const rule_ref *r = &refs->data[i];
        if (r->rule->cond && !hk_css_condition_matches(r->rule->cond, &s->env)) continue;
        if (!match_from(s, r->sel, 0, el)) continue;
        /* A rule matched through several of its selectors counts once, at its highest specificity. */
        add_decls(out, r->rule->decls, r->rule->ndecls, r->origin, r->sel->specificity, r->order);
    }
}

/* --- presentational hints (HTML 15.3) --------------------------------------------------------- */

static void put(hk_charbuf *b, const char *s)
{
    hk_charbuf_append(b, s, strlen(s));
}

static void put_str(hk_charbuf *b, hk_str s)
{
    /* Attribute text goes into CSS: drop characters that could end the declaration. */
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == ';' || c == '{' || c == '}' || c == '\\' || c == '"' || c == '\'' || c == '<') continue;
        hk_charbuf_push(b, c);
    }
}

static bool all_hex(hk_str v)
{
    if (v.n != 3 && v.n != 6) return false;
    for (size_t i = 0; i < v.n; i++) {
        char c = hk_ascii_lower(v.p[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

/* A legacy color attribute value ("ff6600", "#f60", "red"). */
static void put_color(hk_charbuf *b, hk_str v)
{
    v = hk_str_trim_ascii_ws(v);
    if (all_hex(v)) hk_charbuf_push(b, '#');
    put_str(b, v);
}

/* A dimension attribute ("100", "50%", "100px"): unitless numbers are pixels. */
static bool put_length(hk_charbuf *b, const char *prop, hk_str v)
{
    v = hk_str_trim_ascii_ws(v);
    size_t i = 0;
    while (i < v.n && ((v.p[i] >= '0' && v.p[i] <= '9') || v.p[i] == '.')) i++;
    if (i == 0) return false;
    put(b, prop);
    put(b, ":");
    hk_charbuf_append(b, v.p, i);
    if (i < v.n && v.p[i] == '%') hk_charbuf_push(b, '%');
    else hk_charbuf_append(b, "px", 2);
    put(b, ";");
    return true;
}

static const hk_attr *attr(const hk_styler *s, hk_node_id el, hk_atom name)
{
    return hk_dom_attr(s->doc, el, HK_ATTR_NS_NONE, name);
}

static hk_node_id ancestor_table(const hk_document *d, hk_node_id el)
{
    for (hk_node_id p = hk_dom_node(d, el)->parent; p; p = hk_dom_node(d, p)->parent) {
        if (hk_dom_node(d, p)->type != HK_NODE_ELEMENT) break;
        if (hk_dom_is_html(d, p, A(table))) return p;
    }
    return HK_NODE_NONE;
}

static void presentational_hints(const hk_styler *s, hk_node_id el, hk_charbuf *b)
{
    const hk_document *d = s->doc;
    if (hk_dom_node(d, el)->ns != HK_NS_HTML) return;
    hk_atom n = hk_dom_element(d, el)->name;
    const hk_attr *a;
    bool table_part = n == A(table) || n == A(tr) || n == A(td) || n == A(th) || n == A(thead) || n == A(tbody) ||
                      n == A(tfoot);
    if ((a = attr(s, el, A(bgcolor))) && (table_part || n == A(body))) {
        put(b, "background-color:");
        put_color(b, a->value);
        put(b, ";");
    }
    if (n == A(body) && (a = attr(s, el, A(text)))) {
        put(b, "color:");
        put_color(b, a->value);
        put(b, ";");
    }
    if (n == A(font)) {
        if ((a = attr(s, el, A(color)))) {
            put(b, "color:");
            put_color(b, a->value);
            put(b, ";");
        }
        if ((a = attr(s, el, A(face)))) {
            put(b, "font-family:");
            put_str(b, a->value);
            put(b, ";");
        }
        if ((a = attr(s, el, A(size)))) {
            static const char *const k_sizes[] = {"x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"};
            hk_str v = hk_str_trim_ascii_ws(a->value);
            int val = 0;
            bool rel = v.n && (v.p[0] == '+' || v.p[0] == '-');
            for (size_t i = rel ? 1 : 0; i < v.n && v.p[i] >= '0' && v.p[i] <= '9'; i++) val = val * 10 + (v.p[i] - '0');
            int size = rel ? (v.p[0] == '+' ? 3 + val : 3 - val) : val;
            if (size < 1) size = 1;
            if (size > 7) size = 7;
            put(b, "font-size:");
            put(b, k_sizes[size - 1]);
            put(b, ";");
        }
    }
    if ((a = attr(s, el, A(align)))) {
        hk_str v = hk_str_trim_ascii_ws(a->value);
        bool center = hk_str_eq_ascii_icase(v, HK_STR("center")) || hk_str_eq_ascii_icase(v, HK_STR("middle"));
        bool left = hk_str_eq_ascii_icase(v, HK_STR("left")), right = hk_str_eq_ascii_icase(v, HK_STR("right"));
        if (n == A(table) || n == A(hr)) {
            if (center) put(b, "margin-left:auto;margin-right:auto;");
            else if (left && n == A(table)) put(b, "float:left;");
            else if (right && n == A(table)) put(b, "float:right;");
            else if (left) put(b, "margin-right:auto;margin-left:0;");
            else if (right) put(b, "margin-left:auto;margin-right:0;");
        } else if (n == A(img) || n == A(iframe) || n == A(object) || n == A(embed)) {
            if (left) put(b, "float:left;");
            else if (right) put(b, "float:right;");
        } else if (center && (n == A(div) || n == A(td) || n == A(th) || n == A(tr) || n == A(thead) ||
                              n == A(tbody) || n == A(tfoot))) {
            put(b, "text-align:-webkit-center;");
        } else if (n == A(div) || n == A(p) || n == A(h1) || n == A(h2) || n == A(h3) || n == A(h4) || n == A(h5) ||
                   n == A(h6) || n == A(caption) || table_part) {
            if (center) put(b, "text-align:center;");
            else if (left) put(b, "text-align:left;");
            else if (right) put(b, "text-align:right;");
            else if (hk_str_eq_ascii_icase(v, HK_STR("justify"))) put(b, "text-align:justify;");
        }
    }
    if ((n == A(td) || n == A(th) || n == A(tr)) && (a = attr(s, el, A(valign)))) {
        put(b, "vertical-align:");
        put_str(b, a->value);
        put(b, ";");
    }
    bool sized = table_part || n == A(img) || n == A(hr) || n == A(col) || n == A(iframe) || n == A(object) ||
                 n == A(embed) || n == A(video) || n == A(canvas) || n == A(input);
    if (sized && n != A(tr) && (a = attr(s, el, A(width)))) put_length(b, "width", a->value);
    if (sized && n != A(hr) && (a = attr(s, el, A(height)))) put_length(b, "height", a->value);
    if ((n == A(td) || n == A(th)) && attr(s, el, A(nowrap))) put(b, "white-space:nowrap;");
    if (n == A(table)) {
        if ((a = attr(s, el, A(border)))) {
            hk_str v = hk_str_trim_ascii_ws(a->value);
            if (!(v.n == 1 && v.p[0] == '0')) {
                put(b, "border-style:outset;border-color:#808080;");
                if (!put_length(b, "border-width", v)) put(b, "border-width:1px;");
            }
        }
        if ((a = attr(s, el, A(cellspacing)))) put_length(b, "border-spacing", a->value);
    }
    if (n == A(td) || n == A(th)) {
        hk_node_id table = ancestor_table(d, el);
        if (table) {
            if ((a = attr(s, table, A(cellpadding)))) put_length(b, "padding", a->value);
            const hk_attr *border = attr(s, table, A(border));
            if (border && !(border->value.n == 1 && border->value.p[0] == '0')) put(b, "border:1px inset #808080;");
        }
    }
    if (n == A(hr)) {
        if ((a = attr(s, el, A(color)))) {
            put(b, "border-color:");
            put_color(b, a->value);
            put(b, ";background-color:");
            put_color(b, a->value);
            put(b, ";");
        }
        if (attr(s, el, A(noshade))) put(b, "border-style:solid;");
        if ((a = attr(s, el, A(size))) && put_length(b, "height", a->value)) put(b, "border-width:0;");
    }
    if (n == A(img) || n == A(object)) {
        if ((a = attr(s, el, A(hspace)))) {
            put_length(b, "margin-left", a->value);
            put_length(b, "margin-right", a->value);
        }
        if ((a = attr(s, el, A(vspace)))) {
            put_length(b, "margin-top", a->value);
            put_length(b, "margin-bottom", a->value);
        }
    }
}

static void collect(hk_styler *s, hk_node_id el, matched_vec *out)
{
    out->len = 0;
    const hk_document *d = s->doc;
    uint64_t v;
    const hk_attr *id = hk_dom_attr(d, el, HK_ATTR_NS_NONE, A(id));
    if (id) {
        hk_atom a = hk_atom_find(&d->atoms, id->value);
        if (a && hk_map64_get(&s->by_id, a, &v)) consider(s, &s->buckets.data[v], el, out);
    }
    uint32_t first = s->class_first.data[el];
    for (uint32_t i = 0; i < s->class_count.data[el]; i++) {
        if (hk_map64_get(&s->by_class, s->class_atoms.data[first + i], &v)) consider(s, &s->buckets.data[v], el, out);
    }
    if (hk_map64_get(&s->by_tag, hk_dom_element(d, el)->name, &v)) consider(s, &s->buckets.data[v], el, out);
    consider(s, &s->universal, el, out);

    /* style="" attribute: author level, above every selector. */
    inline_decls *in = &s->inline_style.data[el];
    if (!in->parsed) {
        in->parsed = true;
        const hk_attr *st = hk_dom_attr(d, el, HK_ATTR_NS_NONE, A(style));
        if (st) hk_css_parse_declarations(&s->doc->arena, st->value, &in->d, &in->n);
        hk_charbuf hints = {0};
        presentational_hints(s, el, &hints);
        if (hints.len)
            hk_css_parse_declarations(&s->doc->arena, (hk_str){hints.data, hints.len}, &in->hints, &in->nhints);
        hk_vec_free(&hints);
    }
    /* Presentational hints: author level, specificity zero, before all author rules. */
    if (in->nhints) add_decls(out, in->hints, in->nhints, ORIGIN_AUTHOR, 0, 0);
    if (in->n) add_decls(out, in->d, in->n, ORIGIN_AUTHOR, 0x3FFFFFFF, 0xFFFFFF);

    /* Cascade order: level, then specificity, then source order. */
    qsort(out->data, out->len, sizeof(matched), cmp_matched);
}

/* --- var() --------------------------------------------------------------------------------------- */

typedef HK_VEC(hk_css_token) token_vec;

static const hk_cv_span *lookup_var(const hk_custom_props *v, hk_str name)
{
    for (; v; v = v->parent) {
        for (uint32_t i = v->n; i-- > 0;) {
            if (hk_str_eq(v->names[i], name)) return &v->values[i];
        }
    }
    return NULL;
}

static bool has_var(hk_cv_span s)
{
    for (uint32_t i = 0; i < s.n; i++) {
        if (s.t[i].type == HK_CT_FUNCTION && hk_str_eq_ascii_icase(s.t[i].value, HK_STR("var"))) return true;
    }
    return false;
}

static bool expand_vars(hk_cv_span s, const hk_custom_props *vars, token_vec *out, int depth)
{
    if (depth > 16) return false;
    for (uint32_t i = 0; i < s.n; i++) {
        const hk_css_token *t = &s.t[i];
        if (t->type == HK_CT_FUNCTION && hk_str_eq_ascii_icase(t->value, HK_STR("var"))) {
            uint32_t end = hk_cv_skip(s.t, s.n, i); /* past ')' */
            uint32_t j = i + 1;
            while (j < end - 1 && s.t[j].type == HK_CT_WS) j++;
            if (j >= end - 1 || s.t[j].type != HK_CT_IDENT) return false;
            hk_str name = s.t[j].value;
            j++;
            while (j < end - 1 && s.t[j].type == HK_CT_WS) j++;
            const hk_cv_span *val = lookup_var(vars, name);
            if (val && val->n) {
                if (!expand_vars(*val, vars, out, depth + 1)) return false;
            } else if (j < end - 1 && s.t[j].type == HK_CT_COMMA) {
                hk_cv_span fb = {s.t + j + 1, end - 1 - (j + 1)};
                if (!expand_vars(fb, vars, out, depth + 1)) return false;
            } else {
                return false;
            }
            i = end - 1;
            continue;
        }
        hk_vec_push(out, *t);
    }
    return true;
}

/* --- font families ----------------------------------------------------------------------------- */

static uint16_t family_index(hk_styler *s, hk_str name)
{
    for (uint32_t i = 0; i < s->families.len; i++) {
        if (hk_str_eq_ascii_icase(s->families.data[i], name)) return (uint16_t)i;
    }
    if (s->families.len >= UINT16_MAX) return 0;
    hk_vec_push(&s->families, hk_arena_str_dup(&s->family_arena, name));
    return (uint16_t)(s->families.len - 1);
}

static hk_str generic_family(hk_str g)
{
    if (hk_str_eq_ascii_icase(g, HK_STR("serif")) || hk_str_eq_ascii_icase(g, HK_STR("ui-serif")))
        return HK_STR("Times New Roman");
    if (hk_str_eq_ascii_icase(g, HK_STR("monospace")) || hk_str_eq_ascii_icase(g, HK_STR("ui-monospace")))
        return HK_STR("Consolas");
    if (hk_str_eq_ascii_icase(g, HK_STR("cursive"))) return HK_STR("Comic Sans MS");
    if (hk_str_eq_ascii_icase(g, HK_STR("fantasy"))) return HK_STR("Impact");
    if (hk_str_eq_ascii_icase(g, HK_STR("sans-serif")) || hk_str_eq_ascii_icase(g, HK_STR("system-ui")) ||
        hk_str_eq_ascii_icase(g, HK_STR("ui-sans-serif")) || hk_str_eq_ascii_icase(g, HK_STR("-apple-system")) ||
        hk_str_eq_ascii_icase(g, HK_STR("blinkmacsystemfont")) || hk_str_eq_ascii_icase(g, HK_STR("ui-rounded")) ||
        hk_str_eq_ascii_icase(g, HK_STR("emoji")) || hk_str_eq_ascii_icase(g, HK_STR("math")))
        return HK_STR("Segoe UI");
    return HK_STR("");
}

/* Resolves a font-family list to the first installed family. */
static bool resolve_family(hk_styler *s, hk_cv_span v, uint16_t *out)
{
    hk_cv_span items[32];
    uint32_t n = hk_cv_split_commas(v, items, 32);
    /* Cache by the list's text. */
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < v.n; i++) {
        h ^= v.t[i].type;
        h *= 1099511628211ull;
        h ^= hk_str_hash64(v.t[i].value);
        h *= 1099511628211ull;
    }
    h = hk_key64_nonzero(h);
    uint64_t cached;
    if (hk_map64_get(&s->family_cache, h, &cached)) {
        *out = (uint16_t)cached;
        return true;
    }
    bool any = false;
    for (uint32_t i = 0; i < n; i++) {
        hk_cv_span it = hk_cv_trim(items[i]);
        if (it.n == 0) continue;
        char buf[128];
        size_t o = 0;
        hk_str name;
        if (it.n == 1 && it.t[0].type == HK_CT_STRING) {
            name = it.t[0].value;
        } else {
            bool generic_ok = it.n == 1 && it.t[0].type == HK_CT_IDENT;
            for (uint32_t k = 0; k < it.n; k++) {
                if (it.t[k].type == HK_CT_WS) {
                    if (o && o < sizeof buf) buf[o++] = ' ';
                    continue;
                }
                if (it.t[k].type != HK_CT_IDENT) {
                    o = 0;
                    break;
                }
                for (size_t c = 0; c < it.t[k].value.n && o < sizeof buf; c++) buf[o++] = it.t[k].value.p[c];
            }
            name = (hk_str){buf, o};
            if (generic_ok) {
                hk_str g = generic_family(name);
                if (g.n) {
                    *out = family_index(s, g);
                    hk_map64_put(&s->family_cache, h, *out);
                    return true;
                }
            }
        }
        if (name.n == 0) continue;
        any = true;
        if (hk_font_family_exists(name)) {
            *out = family_index(s, name);
            hk_map64_put(&s->family_cache, h, *out);
            return true;
        }
    }
    if (!any && n == 0) return false;
    *out = 0;
    hk_map64_put(&s->family_cache, h, 0);
    return true;
}

/* --- applying declarations ------------------------------------------------------------------------ */

typedef struct apply_ctx {
    hk_styler *s;
    hk_style *st;
    const hk_style *parent;
    hk_cv_ctx cv;
} apply_ctx;

static const hk_style k_initial = {
    .color = 0xFF000000u,
    .font_size = 16,
    .font_weight = 400,
    .visible = 1,
    .list_style = HK_LIST_DISC,
    .display = HK_DISPLAY_INLINE,
    .width = {0, 0, HK_LEN_AUTO},
    .height = {0, 0, HK_LEN_AUTO},
    .max_width = {0, 0, HK_LEN_NONE},
    .max_height = {0, 0, HK_LEN_NONE},
    .min_width = {0, 0, HK_LEN_AUTO},
    .min_height = {0, 0, HK_LEN_AUTO},
    .inset = {{0, 0, HK_LEN_AUTO}, {0, 0, HK_LEN_AUTO}, {0, 0, HK_LEN_AUTO}, {0, 0, HK_LEN_AUTO}},
};

static bool kw(hk_cv_span v, const char *k)
{
    hk_str id;
    return hk_cv_single_ident(v, &id) && hk_str_eq_ascii_icase(id, hk_str_from_cstr(k));
}

static float font_size_keyword(hk_str k, float parent, bool *ok)
{
    static const struct {
        const char *name;
        float px;
    } t[] = {{"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18},
             {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}};
    *ok = true;
    for (size_t i = 0; i < HK_ARRAY_LEN(t); i++) {
        if (hk_str_eq_ascii_icase(k, hk_str_from_cstr(t[i].name))) return t[i].px;
    }
    if (hk_str_eq_ascii_icase(k, HK_STR("smaller"))) return parent / 1.2f;
    if (hk_str_eq_ascii_icase(k, HK_STR("larger"))) return parent * 1.2f;
    *ok = false;
    return parent;
}

static bool apply_font_size(apply_ctx *c, hk_cv_span v)
{
    float parent = c->parent ? c->parent->font_size : 16;
    hk_str id;
    if (hk_cv_single_ident(v, &id)) {
        bool ok;
        float px = font_size_keyword(id, parent, &ok);
        if (ok) c->st->font_size = px;
        return ok;
    }
    hk_cv_ctx cv = c->cv;
    cv.em = parent;
    hk_length l;
    if (!hk_cv_length(v, &cv, HK_CV_PERCENT, &l)) return false;
    c->st->font_size = l.px + l.pct * parent / 100;
    if (c->st->font_size < 0) c->st->font_size = 0;
    return true;
}

static bool apply_font_weight(apply_ctx *c, hk_cv_span v)
{
    hk_str id;
    float w;
    uint16_t pw = c->parent ? c->parent->font_weight : 400;
    if (hk_cv_single_ident(v, &id)) {
        if (hk_str_eq_ascii_icase(id, HK_STR("normal"))) c->st->font_weight = 400;
        else if (hk_str_eq_ascii_icase(id, HK_STR("bold"))) c->st->font_weight = 700;
        else if (hk_str_eq_ascii_icase(id, HK_STR("bolder"))) c->st->font_weight = pw < 350 ? 400 : pw < 550 ? 700 : 900;
        else if (hk_str_eq_ascii_icase(id, HK_STR("lighter"))) c->st->font_weight = pw < 550 ? 100 : pw < 750 ? 400 : 700;
        else return false;
        return true;
    }
    if (!hk_cv_number(v, &c->cv, &w) || w < 1 || w > 1000) return false;
    c->st->font_weight = (uint16_t)w;
    return true;
}

static bool apply_line_height(apply_ctx *c, hk_cv_span v)
{
    float num;
    if (kw(v, "normal")) {
        c->st->line_height = 0;
        c->st->line_height_factor = 0;
        return true;
    }
    if (hk_cv_number(v, &c->cv, &num)) {
        if (num < 0) return false;
        c->st->line_height_factor = num;
        c->st->line_height = num * c->st->font_size;
        return true;
    }
    hk_length l;
    if (!hk_cv_length(v, &c->cv, HK_CV_PERCENT, &l)) return false;
    c->st->line_height_factor = 0;
    c->st->line_height = l.px + l.pct * c->st->font_size / 100;
    return true;
}

static bool apply_font_shorthand(apply_ctx *c, hk_cv_span v)
{
    hk_cv_span parts[16];
    uint32_t n = hk_cv_split(v, parts, 16);
    if (n == 1) {
        hk_str id;
        /* System fonts (caption, menu, ...): use the defaults. */
        if (hk_cv_single_ident(parts[0], &id)) {
            c->st->font_size = 16;
            c->st->font_weight = 400;
            c->st->italic = 0;
            return true;
        }
        return false;
    }
    uint32_t i = 0;
    c->st->italic = 0;
    c->st->font_weight = 400;
    c->st->line_height = 0;
    c->st->line_height_factor = 0;
    for (; i < n; i++) {
        hk_str id;
        if (hk_cv_single_ident(parts[i], &id)) {
            if (hk_str_eq_ascii_icase(id, HK_STR("italic")) || hk_str_eq_ascii_icase(id, HK_STR("oblique"))) {
                c->st->italic = 1;
                continue;
            }
            if (hk_str_eq_ascii_icase(id, HK_STR("normal")) || hk_str_eq_ascii_icase(id, HK_STR("small-caps"))) continue;
            if (apply_font_weight(c, parts[i])) continue;
            bool ok;
            font_size_keyword(id, 16, &ok);
            if (ok) break;
            if (hk_str_ends_with(id, HK_STR("condensed")) || hk_str_ends_with(id, HK_STR("expanded"))) continue;
            return false;
        }
        float num;
        if (hk_cv_number(parts[i], &c->cv, &num)) {
            if (apply_font_weight(c, parts[i])) continue;
            return false;
        }
        break;
    }
    if (i >= n) return false;
    /* size[/line-height] */
    const hk_css_token *slash = NULL;
    hk_cv_span size = parts[i];
    if (!apply_font_size(c, size)) return false;
    i++;
    if (i < n && parts[i].n == 1 && parts[i].t[0].type == HK_CT_DELIM && parts[i].t[0].delim == '/') {
        slash = parts[i].t;
        i++;
        if (i >= n || !apply_line_height(c, parts[i])) return false;
        i++;
    }
    (void)slash;
    if (i >= n) return false;
    hk_cv_span family = {parts[i].t, (uint32_t)(v.t + v.n - parts[i].t)};
    uint16_t f;
    if (resolve_family(c->s, family, &f)) c->st->font_family = f;
    return true;
}

static bool apply_display(apply_ctx *c, hk_cv_span v)
{
    hk_cv_span parts[3];
    uint32_t n = hk_cv_split(v, parts, 3);
    hk_str a, b = HK_STR("");
    if (n == 0 || !hk_cv_single_ident(parts[0], &a)) return false;
    if (n > 1 && !hk_cv_single_ident(parts[1], &b)) return false;
    static const struct {
        const char *name;
        uint8_t d;
    } k[] = {
        {"none", HK_DISPLAY_NONE},
        {"inline", HK_DISPLAY_INLINE},
        {"block", HK_DISPLAY_BLOCK},
        {"inline-block", HK_DISPLAY_INLINE_BLOCK},
        {"list-item", HK_DISPLAY_LIST_ITEM},
        {"table", HK_DISPLAY_TABLE},
        {"inline-table", HK_DISPLAY_INLINE_BLOCK},
        {"table-row-group", HK_DISPLAY_TABLE_ROW_GROUP},
        {"table-header-group", HK_DISPLAY_TABLE_ROW_GROUP},
        {"table-footer-group", HK_DISPLAY_TABLE_ROW_GROUP},
        {"table-row", HK_DISPLAY_TABLE_ROW},
        {"table-cell", HK_DISPLAY_TABLE_CELL},
        {"table-caption", HK_DISPLAY_TABLE_CAPTION},
        {"table-column", HK_DISPLAY_NONE},
        {"table-column-group", HK_DISPLAY_NONE},
        {"flex", HK_DISPLAY_FLEX},
        {"inline-flex", HK_DISPLAY_INLINE_BLOCK},
        {"grid", HK_DISPLAY_GRID},
        {"inline-grid", HK_DISPLAY_INLINE_BLOCK},
        {"contents", HK_DISPLAY_CONTENTS},
        {"flow-root", HK_DISPLAY_BLOCK},
        {"ruby", HK_DISPLAY_INLINE},
        {"ruby-text", HK_DISPLAY_INLINE},
        {"-webkit-box", HK_DISPLAY_BLOCK},
        {"-webkit-inline-box", HK_DISPLAY_INLINE_BLOCK},
        {"-ms-flexbox", HK_DISPLAY_FLEX},
    };
    if (n == 2) {
        /* Two-value syntax: outer + inner. */
        bool inline_outer = hk_str_eq_ascii_icase(a, HK_STR("inline")) || hk_str_eq_ascii_icase(b, HK_STR("inline"));
        bool flow = hk_str_eq_ascii_icase(a, HK_STR("flow")) || hk_str_eq_ascii_icase(b, HK_STR("flow"));
        c->st->display = (uint8_t)(inline_outer ? (flow ? HK_DISPLAY_INLINE : HK_DISPLAY_INLINE_BLOCK) : HK_DISPLAY_BLOCK);
        return true;
    }
    for (size_t i = 0; i < HK_ARRAY_LEN(k); i++) {
        if (hk_str_eq_ascii_icase(a, hk_str_from_cstr(k[i].name))) {
            c->st->display = k[i].d;
            return true;
        }
    }
    return false;
}

static bool apply_border_style_value(hk_cv_span v, uint8_t *out)
{
    hk_str id;
    if (!hk_cv_single_ident(v, &id)) return false;
    if (hk_str_eq_ascii_icase(id, HK_STR("none")) || hk_str_eq_ascii_icase(id, HK_STR("hidden"))) *out = HK_BORDER_NONE;
    else if (hk_str_eq_ascii_icase(id, HK_STR("dashed"))) *out = HK_BORDER_DASHED;
    else if (hk_str_eq_ascii_icase(id, HK_STR("dotted"))) *out = HK_BORDER_DOTTED;
    else if (hk_str_eq_ascii_icase(id, HK_STR("double"))) *out = HK_BORDER_DOUBLE;
    else if (hk_str_eq_ascii_icase(id, HK_STR("solid")) || hk_str_eq_ascii_icase(id, HK_STR("groove")) ||
             hk_str_eq_ascii_icase(id, HK_STR("ridge")) || hk_str_eq_ascii_icase(id, HK_STR("inset")) ||
             hk_str_eq_ascii_icase(id, HK_STR("outset")))
        *out = HK_BORDER_SOLID;
    else return false;
    return true;
}

static bool border_width_value(apply_ctx *c, hk_cv_span v, float *out)
{
    hk_str id;
    if (hk_cv_single_ident(v, &id)) {
        if (hk_str_eq_ascii_icase(id, HK_STR("thin"))) *out = 1;
        else if (hk_str_eq_ascii_icase(id, HK_STR("medium"))) *out = 3;
        else if (hk_str_eq_ascii_icase(id, HK_STR("thick"))) *out = 5;
        else return false;
        return true;
    }
    hk_length l;
    if (!hk_cv_length(v, &c->cv, 0, &l)) return false;
    *out = l.px;
    return true;
}

/* "border: 1px solid red" for the given sides (bit mask: 1 top, 2 right, 4 bottom, 8 left). */
static bool apply_border(apply_ctx *c, hk_cv_span v, unsigned sides)
{
    hk_cv_span parts[4];
    uint32_t n = hk_cv_split(v, parts, 4);
    float width = 3;
    uint8_t style = HK_BORDER_NONE;
    uint32_t color = c->st->color;
    for (uint32_t i = 0; i < n; i++) {
        float w;
        uint8_t s;
        uint32_t col;
        if (border_width_value(c, parts[i], &w)) width = w;
        else if (apply_border_style_value(parts[i], &s)) style = s;
        else if (hk_cv_color(parts[i], c->st->color, &col)) color = col;
        else return false;
    }
    for (int k = 0; k < 4; k++) {
        if (!(sides & (1u << k))) continue;
        c->st->border_width[k] = width;
        c->st->border_style[k] = style;
        c->st->border_color[k] = color;
    }
    return true;
}

/* margin / padding / border-* four-value expansion. */
static uint32_t four(hk_cv_span v, hk_cv_span out[4])
{
    hk_cv_span p[4];
    uint32_t n = hk_cv_split(v, p, 4);
    if (n == 0) return 0;
    out[0] = p[0];
    out[1] = n > 1 ? p[1] : p[0];
    out[2] = n > 2 ? p[2] : p[0];
    out[3] = n > 3 ? p[3] : out[1];
    return n;
}

static bool apply_box(apply_ctx *c, hk_cv_span v, hk_length *box, uint32_t flags)
{
    hk_cv_span s[4];
    if (!four(v, s)) return false;
    hk_length l[4];
    for (int i = 0; i < 4; i++) {
        if (!hk_cv_length(s[i], &c->cv, flags, &l[i])) return false;
    }
    memcpy(box, l, sizeof l);
    return true;
}

static void inherit_one(apply_ctx *c, uint16_t prop);
static void initial_one(apply_ctx *c, uint16_t prop);

static bool apply_decl(apply_ctx *c, uint16_t prop, hk_cv_span v)
{
    hk_style *st = c->st;
    hk_str id;
    if (hk_cv_single_ident(v, &id)) {
        bool inherit = hk_str_eq_ascii_icase(id, HK_STR("inherit"));
        bool initial = hk_str_eq_ascii_icase(id, HK_STR("initial"));
        bool unset = hk_str_eq_ascii_icase(id, HK_STR("unset")) || hk_str_eq_ascii_icase(id, HK_STR("revert")) ||
                     hk_str_eq_ascii_icase(id, HK_STR("revert-layer"));
        if (inherit || initial || unset) {
            if (inherit && c->parent) inherit_one(c, prop);
            else if (initial) initial_one(c, prop);
            else if (unset) {
                if (c->parent) inherit_one(c, prop); /* inherited properties keep the parent value */
                initial_one(c, prop);
            }
            return true;
        }
    }
    hk_length l;
    uint32_t col;
    uint8_t bs;
    float f;
    switch (prop) {
    case HK_PROP_DISPLAY: return apply_display(c, v);
    case HK_PROP_POSITION:
        if (kw(v, "static")) st->position = HK_POS_STATIC;
        else if (kw(v, "relative")) st->position = HK_POS_RELATIVE;
        else if (kw(v, "absolute")) st->position = HK_POS_ABSOLUTE;
        else if (kw(v, "fixed")) st->position = HK_POS_FIXED;
        else if (kw(v, "sticky") || kw(v, "-webkit-sticky")) st->position = HK_POS_STICKY;
        else return false;
        return true;
    case HK_PROP_FLOAT:
        if (kw(v, "none")) st->floating = 0;
        else if (kw(v, "left") || kw(v, "inline-start")) st->floating = 1;
        else if (kw(v, "right") || kw(v, "inline-end")) st->floating = 2;
        else return false;
        return true;
    case HK_PROP_VISIBILITY:
        if (kw(v, "visible")) st->visible = 1;
        else if (kw(v, "hidden") || kw(v, "collapse")) st->visible = 0;
        else return false;
        return true;
    case HK_PROP_BOX_SIZING:
        if (kw(v, "border-box")) st->border_box = 1;
        else if (kw(v, "content-box")) st->border_box = 0;
        else return false;
        return true;
    case HK_PROP_MARGIN: return apply_box(c, v, st->margin, HK_CV_PERCENT | HK_CV_AUTO | HK_CV_NEGATIVE);
    case HK_PROP_PADDING: return apply_box(c, v, st->padding, HK_CV_PERCENT);
    case HK_PROP_MARGIN_TOP:
    case HK_PROP_MARGIN_RIGHT:
    case HK_PROP_MARGIN_BOTTOM:
    case HK_PROP_MARGIN_LEFT:
    case HK_PROP_MARGIN_BLOCK_START:
    case HK_PROP_MARGIN_BLOCK_END:
    case HK_PROP_MARGIN_INLINE_START:
    case HK_PROP_MARGIN_INLINE_END: {
        if (!hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_AUTO | HK_CV_NEGATIVE, &l)) return false;
        int side = prop == HK_PROP_MARGIN_TOP || prop == HK_PROP_MARGIN_BLOCK_START     ? 0
                   : prop == HK_PROP_MARGIN_RIGHT || prop == HK_PROP_MARGIN_INLINE_END  ? 1
                   : prop == HK_PROP_MARGIN_BOTTOM || prop == HK_PROP_MARGIN_BLOCK_END  ? 2
                                                                                        : 3;
        st->margin[side] = l;
        return true;
    }
    case HK_PROP_MARGIN_BLOCK:
    case HK_PROP_MARGIN_INLINE:
    case HK_PROP_PADDING_BLOCK:
    case HK_PROP_PADDING_INLINE: {
        hk_cv_span p[2];
        uint32_t n = hk_cv_split(v, p, 2);
        if (n == 0) return false;
        bool margin = prop == HK_PROP_MARGIN_BLOCK || prop == HK_PROP_MARGIN_INLINE;
        uint32_t flags = margin ? HK_CV_PERCENT | HK_CV_AUTO | HK_CV_NEGATIVE : HK_CV_PERCENT;
        hk_length a, b;
        if (!hk_cv_length(p[0], &c->cv, flags, &a)) return false;
        if (n < 2) b = a;
        else if (!hk_cv_length(p[1], &c->cv, flags, &b)) return false;
        hk_length *box = margin ? st->margin : st->padding;
        bool block = prop == HK_PROP_MARGIN_BLOCK || prop == HK_PROP_PADDING_BLOCK;
        box[block ? 0 : 3] = a;
        box[block ? 2 : 1] = b;
        return true;
    }
    case HK_PROP_PADDING_TOP:
    case HK_PROP_PADDING_RIGHT:
    case HK_PROP_PADDING_BOTTOM:
    case HK_PROP_PADDING_LEFT:
    case HK_PROP_PADDING_INLINE_START:
    case HK_PROP_PADDING_INLINE_END: {
        if (!hk_cv_length(v, &c->cv, HK_CV_PERCENT, &l)) return false;
        int side = prop == HK_PROP_PADDING_TOP                                            ? 0
                   : prop == HK_PROP_PADDING_RIGHT || prop == HK_PROP_PADDING_INLINE_END ? 1
                   : prop == HK_PROP_PADDING_BOTTOM                                       ? 2
                                                                                          : 3;
        st->padding[side] = l;
        return true;
    }
    case HK_PROP_BORDER: return apply_border(c, v, 15);
    case HK_PROP_BORDER_TOP: return apply_border(c, v, 1);
    case HK_PROP_BORDER_RIGHT: return apply_border(c, v, 2);
    case HK_PROP_BORDER_BOTTOM: return apply_border(c, v, 4);
    case HK_PROP_BORDER_LEFT: return apply_border(c, v, 8);
    case HK_PROP_BORDER_WIDTH: {
        hk_cv_span s[4];
        if (!four(v, s)) return false;
        float w[4];
        for (int i = 0; i < 4; i++) {
            if (!border_width_value(c, s[i], &w[i])) return false;
        }
        memcpy(st->border_width, w, sizeof w);
        return true;
    }
    case HK_PROP_BORDER_STYLE: {
        hk_cv_span s[4];
        if (!four(v, s)) return false;
        uint8_t b[4];
        for (int i = 0; i < 4; i++) {
            if (!apply_border_style_value(s[i], &b[i])) return false;
        }
        memcpy(st->border_style, b, sizeof b);
        return true;
    }
    case HK_PROP_BORDER_COLOR: {
        hk_cv_span s[4];
        if (!four(v, s)) return false;
        uint32_t b[4];
        for (int i = 0; i < 4; i++) {
            if (!hk_cv_color(s[i], st->color, &b[i])) return false;
        }
        memcpy(st->border_color, b, sizeof b);
        return true;
    }
    case HK_PROP_BORDER_TOP_WIDTH:
    case HK_PROP_BORDER_RIGHT_WIDTH:
    case HK_PROP_BORDER_BOTTOM_WIDTH:
    case HK_PROP_BORDER_LEFT_WIDTH:
        if (!border_width_value(c, v, &f)) return false;
        st->border_width[prop - HK_PROP_BORDER_TOP_WIDTH] = f;
        return true;
    case HK_PROP_BORDER_TOP_STYLE:
    case HK_PROP_BORDER_RIGHT_STYLE:
    case HK_PROP_BORDER_BOTTOM_STYLE:
    case HK_PROP_BORDER_LEFT_STYLE:
        if (!apply_border_style_value(v, &bs)) return false;
        st->border_style[prop - HK_PROP_BORDER_TOP_STYLE] = bs;
        return true;
    case HK_PROP_BORDER_TOP_COLOR:
    case HK_PROP_BORDER_RIGHT_COLOR:
    case HK_PROP_BORDER_BOTTOM_COLOR:
    case HK_PROP_BORDER_LEFT_COLOR:
        if (!hk_cv_color(v, st->color, &col)) return false;
        st->border_color[prop - HK_PROP_BORDER_TOP_COLOR] = col;
        return true;
    case HK_PROP_WIDTH:
    case HK_PROP_INLINE_SIZE: return hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_AUTO, &st->width);
    case HK_PROP_HEIGHT: return hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_AUTO, &st->height);
    case HK_PROP_MIN_WIDTH: return hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_AUTO, &st->min_width);
    case HK_PROP_MAX_WIDTH:
    case HK_PROP_MAX_INLINE_SIZE: return hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_NONE, &st->max_width);
    case HK_PROP_MIN_HEIGHT: return hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_AUTO, &st->min_height);
    case HK_PROP_MAX_HEIGHT: return hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_NONE, &st->max_height);
    case HK_PROP_COLOR:
        if (!hk_cv_color(v, c->parent ? c->parent->color : 0xFF000000u, &col)) return false;
        st->color = col;
        return true;
    case HK_PROP_BACKGROUND_COLOR:
        if (!hk_cv_color(v, st->color, &col)) return false;
        st->background = col;
        return true;
    case HK_PROP_BACKGROUND: {
        /* Only the color layer is used until background images arrive (M3). */
        hk_cv_span layers[8];
        uint32_t nl = hk_cv_split_commas(v, layers, 8);
        hk_cv_span parts[16];
        uint32_t n = hk_cv_split(layers[nl - 1], parts, 16);
        st->background = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (hk_cv_color(parts[i], st->color, &col)) st->background = col;
        }
        return true;
    }
    case HK_PROP_FONT_FAMILY: {
        uint16_t fam;
        if (!resolve_family(c->s, v, &fam)) return false;
        st->font_family = fam;
        return true;
    }
    case HK_PROP_FONT_SIZE: return apply_font_size(c, v);
    case HK_PROP_FONT_WEIGHT: return apply_font_weight(c, v);
    case HK_PROP_FONT_STYLE:
        if (kw(v, "normal")) st->italic = 0;
        else if (kw(v, "italic") || kw(v, "oblique")) st->italic = 1;
        else {
            hk_cv_span p[2];
            if (hk_cv_split(v, p, 2) == 2 && kw(p[0], "oblique")) st->italic = 1;
            else return false;
        }
        return true;
    case HK_PROP_FONT: return apply_font_shorthand(c, v);
    case HK_PROP_LINE_HEIGHT: return apply_line_height(c, v);
    case HK_PROP_TEXT_ALIGN:
        if (kw(v, "left")) st->text_align = HK_ALIGN_LEFT;
        else if (kw(v, "right")) st->text_align = HK_ALIGN_RIGHT;
        else if (kw(v, "center")) st->text_align = HK_ALIGN_CENTER;
        else if (kw(v, "-webkit-center") || kw(v, "-moz-center") || kw(v, "-khtml-center"))
            st->text_align = HK_ALIGN_LEGACY_CENTER;
        else if (kw(v, "justify")) st->text_align = HK_ALIGN_JUSTIFY;
        else if (kw(v, "start")) st->text_align = HK_ALIGN_START;
        else if (kw(v, "end")) st->text_align = HK_ALIGN_END;
        else return false;
        return true;
    case HK_PROP_TEXT_DECORATION:
    case HK_PROP_TEXT_DECORATION_LINE: {
        hk_cv_span parts[8];
        uint32_t n = hk_cv_split(v, parts, 8);
        uint8_t deco = 0;
        bool none = false;
        for (uint32_t i = 0; i < n; i++) {
            if (kw(parts[i], "underline")) deco |= HK_DECO_UNDERLINE;
            else if (kw(parts[i], "line-through")) deco |= HK_DECO_LINE_THROUGH;
            else if (kw(parts[i], "overline")) deco |= HK_DECO_OVERLINE;
            else if (kw(parts[i], "none")) none = true;
            /* colors, styles and thickness are ignored */
        }
        if (none || deco) st->decoration = deco;
        return true;
    }
    case HK_PROP_TEXT_INDENT:
        if (!hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_NEGATIVE, &l)) return false;
        st->text_indent = l.px; /* percentages: not supported */
        return true;
    case HK_PROP_TEXT_TRANSFORM:
        if (kw(v, "none")) st->text_transform = HK_TT_NONE;
        else if (kw(v, "uppercase")) st->text_transform = HK_TT_UPPERCASE;
        else if (kw(v, "lowercase")) st->text_transform = HK_TT_LOWERCASE;
        else if (kw(v, "capitalize")) st->text_transform = HK_TT_CAPITALIZE;
        else return false;
        return true;
    case HK_PROP_WHITE_SPACE:
        if (kw(v, "normal")) st->white_space = HK_WS_NORMAL;
        else if (kw(v, "nowrap")) st->white_space = HK_WS_NOWRAP;
        else if (kw(v, "pre")) st->white_space = HK_WS_PRE;
        else if (kw(v, "pre-wrap") || kw(v, "break-spaces")) st->white_space = HK_WS_PRE_WRAP;
        else if (kw(v, "pre-line")) st->white_space = HK_WS_PRE_LINE;
        else return false;
        return true;
    case HK_PROP_LIST_STYLE:
    case HK_PROP_LIST_STYLE_TYPE: {
        hk_cv_span parts[4];
        uint32_t n = hk_cv_split(v, parts, 4);
        for (uint32_t i = 0; i < n; i++) {
            if (kw(parts[i], "none")) st->list_style = HK_LIST_NONE;
            else if (kw(parts[i], "disc")) st->list_style = HK_LIST_DISC;
            else if (kw(parts[i], "circle")) st->list_style = HK_LIST_CIRCLE;
            else if (kw(parts[i], "square")) st->list_style = HK_LIST_SQUARE;
            else if (kw(parts[i], "decimal") || kw(parts[i], "decimal-leading-zero") || kw(parts[i], "lower-alpha") ||
                     kw(parts[i], "upper-alpha") || kw(parts[i], "lower-roman") || kw(parts[i], "upper-roman") ||
                     kw(parts[i], "lower-latin") || kw(parts[i], "upper-latin"))
                st->list_style = HK_LIST_DECIMAL;
        }
        return true;
    }
    case HK_PROP_OVERFLOW:
        st->overflow_hidden = (uint8_t)(!kw(v, "visible"));
        return true;
    case HK_PROP_TOP:
    case HK_PROP_RIGHT:
    case HK_PROP_BOTTOM:
    case HK_PROP_LEFT:
        if (!hk_cv_length(v, &c->cv, HK_CV_PERCENT | HK_CV_AUTO | HK_CV_NEGATIVE, &l)) return false;
        st->inset[prop - HK_PROP_TOP] = l;
        return true;
    case HK_PROP_INSET: {
        hk_cv_span s4[4];
        if (!four(v, s4)) return false;
        hk_length in[4];
        for (int i = 0; i < 4; i++) {
            if (!hk_cv_length(s4[i], &c->cv, HK_CV_PERCENT | HK_CV_AUTO | HK_CV_NEGATIVE, &in[i])) return false;
        }
        memcpy(st->inset, in, sizeof in);
        return true;
    }
    case HK_PROP_CLEAR:
        if (kw(v, "none")) st->clear = 0;
        else if (kw(v, "left") || kw(v, "inline-start")) st->clear = 1;
        else if (kw(v, "right") || kw(v, "inline-end")) st->clear = 2;
        else if (kw(v, "both")) st->clear = 3;
        else return false;
        return true;
    case HK_PROP_Z_INDEX:
        if (kw(v, "auto")) st->z_index = 0;
        else if (hk_cv_number(v, &c->cv, &f)) st->z_index = (int32_t)f;
        else return false;
        return true;
    case HK_PROP_BORDER_COLLAPSE:
        if (kw(v, "collapse")) st->border_collapse = 1;
        else if (kw(v, "separate")) st->border_collapse = 0;
        else return false;
        return true;
    case HK_PROP_BORDER_SPACING: {
        hk_cv_span p2[2];
        if (hk_cv_split(v, p2, 2) == 0 || !hk_cv_length(p2[0], &c->cv, 0, &l)) return false;
        st->border_spacing = l.px;
        return true;
    }
    case HK_PROP_VERTICAL_ALIGN:
        if (kw(v, "top") || kw(v, "text-top")) st->valign = 1;
        else if (kw(v, "middle")) st->valign = 2;
        else if (kw(v, "bottom") || kw(v, "text-bottom")) st->valign = 3;
        else st->valign = 0;
        return true;
    case HK_PROP_OVERFLOW_X:
    case HK_PROP_OVERFLOW_Y:
        if (!kw(v, "visible")) st->overflow_hidden = 1;
        return true;
    case HK_PROP_CLIP:
    case HK_PROP_CLIP_PATH:
    case HK_PROP_OPACITY:
    case HK_PROP_CONTENT: return true;
    default: return false;
    }
}

static void inherit_one(apply_ctx *c, uint16_t prop)
{
    hk_style *st = c->st;
    const hk_style *p = c->parent;
    switch (prop) {
    case HK_PROP_COLOR: st->color = p->color; break;
    case HK_PROP_BACKGROUND:
    case HK_PROP_BACKGROUND_COLOR: st->background = p->background; break;
    case HK_PROP_DISPLAY: st->display = p->display; break;
    case HK_PROP_FONT_SIZE: st->font_size = p->font_size; break;
    case HK_PROP_FONT_WEIGHT: st->font_weight = p->font_weight; break;
    case HK_PROP_FONT_FAMILY: st->font_family = p->font_family; break;
    case HK_PROP_FONT_STYLE: st->italic = p->italic; break;
    case HK_PROP_LINE_HEIGHT:
        st->line_height = p->line_height;
        st->line_height_factor = p->line_height_factor;
        break;
    case HK_PROP_TEXT_ALIGN: st->text_align = p->text_align; break;
    case HK_PROP_WHITE_SPACE: st->white_space = p->white_space; break;
    case HK_PROP_VISIBILITY: st->visible = p->visible; break;
    case HK_PROP_WIDTH: st->width = p->width; break;
    case HK_PROP_HEIGHT: st->height = p->height; break;
    case HK_PROP_MARGIN: memcpy(st->margin, p->margin, sizeof st->margin); break;
    case HK_PROP_PADDING: memcpy(st->padding, p->padding, sizeof st->padding); break;
    case HK_PROP_BORDER_COLOR: memcpy(st->border_color, p->border_color, sizeof st->border_color); break;
    default: break;
    }
}

static void initial_one(apply_ctx *c, uint16_t prop)
{
    hk_style *st = c->st;
    switch (prop) {
    case HK_PROP_COLOR: st->color = k_initial.color; break;
    case HK_PROP_BACKGROUND:
    case HK_PROP_BACKGROUND_COLOR: st->background = 0; break;
    case HK_PROP_DISPLAY: st->display = HK_DISPLAY_INLINE; break;
    case HK_PROP_FONT_SIZE: st->font_size = 16; break;
    case HK_PROP_FONT_WEIGHT: st->font_weight = 400; break;
    case HK_PROP_FONT_STYLE: st->italic = 0; break;
    case HK_PROP_LINE_HEIGHT: st->line_height = st->line_height_factor = 0; break;
    case HK_PROP_TEXT_ALIGN: st->text_align = HK_ALIGN_START; break;
    case HK_PROP_WHITE_SPACE: st->white_space = HK_WS_NORMAL; break;
    case HK_PROP_VISIBILITY: st->visible = 1; break;
    case HK_PROP_WIDTH: st->width = k_initial.width; break;
    case HK_PROP_HEIGHT: st->height = k_initial.height; break;
    case HK_PROP_MAX_WIDTH: st->max_width = k_initial.max_width; break;
    case HK_PROP_MARGIN: memset(st->margin, 0, sizeof st->margin); break;
    case HK_PROP_PADDING: memset(st->padding, 0, sizeof st->padding); break;
    case HK_PROP_POSITION: st->position = HK_POS_STATIC; break;
    case HK_PROP_FLOAT: st->floating = 0; break;
    case HK_PROP_TEXT_DECORATION:
    case HK_PROP_TEXT_DECORATION_LINE: st->decoration = 0; break;
    default: break;
    }
}

static bool is_font_prop(uint16_t p)
{
    return p == HK_PROP_FONT || p == HK_PROP_FONT_SIZE || p == HK_PROP_FONT_FAMILY || p == HK_PROP_FONT_WEIGHT ||
           p == HK_PROP_FONT_STYLE || p == HK_PROP_LINE_HEIGHT;
}

static void apply_with_vars(apply_ctx *c, const hk_css_decl *d, token_vec *tmp)
{
    hk_cv_span v = {d->v, d->vn};
    if (has_var(v)) {
        tmp->len = 0;
        if (!expand_vars(v, c->st->vars, tmp, 0)) {
            /* Invalid at computed-value time: behaves as unset. */
            if (c->parent) inherit_one(c, d->prop);
            initial_one(c, d->prop);
            return;
        }
        v = (hk_cv_span){tmp->data, tmp->len};
    }
    apply_decl(c, d->prop, v);
}

/* --- per element ---------------------------------------------------------------------------------- */

static const hk_style *intern_style(hk_styler *s, const hk_style *st)
{
    uint64_t h = hk_key64_nonzero(hk_hash64(st, sizeof *st));
    for (;;) {
        uint64_t v;
        if (!hk_map64_get(&s->interned, h, &v)) break;
        const hk_style *existing = (const hk_style *)(uintptr_t)v;
        if (memcmp(existing, st, sizeof *st) == 0) return existing;
        h = hk_key64_nonzero(h + 0x9E3779B97F4A7C15ull); /* collision: probe another key */
    }
    hk_style *copy = HK_ARENA_NEW(&s->style_arena, hk_style);
    memcpy(copy, st, sizeof *st);
    hk_map64_put(&s->interned, h, (uint64_t)(uintptr_t)copy);
    s->distinct++;
    return copy;
}

static void compute_element(hk_styler *s, hk_node_id el, const hk_style *parent, matched_vec *m, token_vec *tmp)
{
    hk_style st;
    memset(&st, 0, sizeof st);
    memcpy(&st, &k_initial, sizeof st);
    st.font_family = 0;
    if (parent) {
        st.color = parent->color;
        st.font_size = parent->font_size;
        st.line_height = parent->line_height;
        st.line_height_factor = parent->line_height_factor;
        st.text_indent = parent->text_indent;
        st.font_weight = parent->font_weight;
        st.font_family = parent->font_family;
        st.italic = parent->italic;
        st.text_align = parent->text_align;
        st.white_space = parent->white_space;
        st.text_transform = parent->text_transform;
        st.list_style = parent->list_style;
        st.visible = parent->visible;
        st.decoration = parent->decoration;
        st.vars = parent->vars;
    }
    collect(s, el, m);

    /* Custom properties first: they may be used by any other declaration. */
    uint32_t ncustom = 0;
    for (uint32_t i = 0; i < m->len; i++) ncustom += m->data[i].decl->prop == HK_PROP_CUSTOM;
    if (ncustom) {
        hk_custom_props *cp = HK_ARENA_NEW(&s->style_arena, hk_custom_props);
        hk_str *names = HK_ARENA_NEW_ARRAY(&s->style_arena, hk_str, ncustom);
        hk_cv_span *values = HK_ARENA_NEW_ARRAY(&s->style_arena, hk_cv_span, ncustom);
        uint32_t k = 0;
        for (uint32_t i = 0; i < m->len; i++) {
            const hk_css_decl *d = m->data[i].decl;
            if (d->prop != HK_PROP_CUSTOM) continue;
            names[k] = d->custom;
            values[k] = (hk_cv_span){d->v, d->vn};
            k++;
        }
        cp->parent = st.vars;
        cp->n = k;
        cp->names = names;
        cp->values = values;
        st.vars = cp;
    }

    apply_ctx c = {s, &st, parent, {parent ? parent->font_size : 16, s->root_font_size, s->env.width, s->env.height}};
    for (uint32_t i = 0; i < m->len; i++) {
        const hk_css_decl *d = m->data[i].decl;
        if (is_font_prop(d->prop)) apply_with_vars(&c, d, tmp);
    }
    /* A <number> line-height is inherited as the number. */
    if (st.line_height_factor > 0) st.line_height = st.line_height_factor * st.font_size;
    c.cv.em = st.font_size;
    if (!parent) {
        s->root_font_size = st.font_size;
        c.cv.rem = st.font_size;
    }
    for (uint32_t i = 0; i < m->len; i++) {
        const hk_css_decl *d = m->data[i].decl;
        if (!is_font_prop(d->prop) && d->prop != HK_PROP_CUSTOM) apply_with_vars(&c, d, tmp);
    }
    for (int k = 0; k < 4; k++) {
        if (st.border_style[k] == HK_BORDER_NONE) st.border_width[k] = 0;
    }
    /* CSS 2.1 9.7: floats and absolutely positioned boxes are blockified. */
    if ((st.floating || st.position == HK_POS_ABSOLUTE || st.position == HK_POS_FIXED) &&
        (st.display == HK_DISPLAY_INLINE || st.display == HK_DISPLAY_INLINE_BLOCK))
        st.display = HK_DISPLAY_BLOCK;
    if (!parent && st.display == HK_DISPLAY_INLINE) st.display = HK_DISPLAY_BLOCK;
    s->styles.data[el] = intern_style(s, &st);
    s->elements++;
}

static void compute_tree(hk_styler *s, hk_node_id node, const hk_style *parent, matched_vec *m, token_vec *tmp)
{
    const hk_document *d = s->doc;
    for (hk_node_id c = hk_dom_node(d, node)->first_child; c; c = hk_dom_node(d, c)->next) {
        if (hk_dom_node(d, c)->type != HK_NODE_ELEMENT) continue;
        compute_element(s, c, parent, m, tmp);
        const hk_style *st = s->styles.data[c];
        /* display:none subtrees are not rendered; their descendants keep no style. */
        if (st->display != HK_DISPLAY_NONE) compute_tree(s, c, st, m, tmp);
    }
}

void hk_styler_compute(hk_styler *s, const hk_css_media_env *env)
{
    s->env = *env;
    if (!s->index_valid) build_index(s);
    prepare_nodes(s);
    hk_arena_reset(&s->style_arena);
    hk_map64_free(&s->interned);
    hk_map64_init(&s->interned);
    s->distinct = 0;
    s->elements = 0;
    s->root_font_size = 16;
    uint32_t n = s->doc->nodes.len;
    s->styles.len = 0;
    hk_vec_reserve(&s->styles, n);
    memset(s->styles.data, 0, n * sizeof(const hk_style *));
    s->styles.len = n;
    matched_vec m = {0};
    token_vec tmp = {0};
    compute_tree(s, s->doc->root, NULL, &m, &tmp);
    hk_vec_free(&m);
    hk_vec_free(&tmp);
}

const hk_style *hk_style_of(const hk_styler *s, hk_node_id id)
{
    return id < s->styles.len ? s->styles.data[id] : NULL;
}

void hk_styler_stats(const hk_styler *s, uint32_t *distinct, uint32_t *elements, size_t *bytes)
{
    *distinct = s->distinct;
    *elements = s->elements;
    *bytes = s->style_arena.bytes_reserved + (size_t)s->styles.cap * sizeof(void *) +
             (size_t)s->class_atoms.cap * sizeof(hk_atom) + (size_t)s->class_first.cap * 6 +
             (size_t)s->inline_style.cap * sizeof(inline_decls);
}

float hk_length_resolve(hk_length l, float base, float fallback)
{
    switch (l.unit) {
    case HK_LEN_PX: return l.px;
    case HK_LEN_PCT: return l.pct * base / 100;
    case HK_LEN_CALC: return l.px + l.pct * base / 100;
    default: return fallback;
    }
}
