/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests for css/ and style/: tokenizer, selectors, media queries and the cascade. */
#include "hk_test.h"
#include "css/hk_css.h"
#include "html/hk_html_parser.h"
#include "style/hk_style.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

HK_TEST(css_tokenizer)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_css_tokens tk = hk_css_tokenize(&a, HK_STR("a.b > #c{color:red;width:calc(50% - 10px)} /*x*/ @media"
                                                 " url( x.png ) 1.5em -3 10% \"s\\41\""));
    static const uint8_t want[] = {
        HK_CT_IDENT, HK_CT_DELIM, HK_CT_IDENT, HK_CT_WS, HK_CT_DELIM, HK_CT_WS, HK_CT_HASH, HK_CT_LBRACE,
        HK_CT_IDENT, HK_CT_COLON, HK_CT_IDENT, HK_CT_SEMI, HK_CT_IDENT, HK_CT_COLON, HK_CT_FUNCTION,
        HK_CT_PERCENT, HK_CT_WS, HK_CT_DELIM, HK_CT_WS, HK_CT_DIMENSION, HK_CT_RPAREN, HK_CT_RBRACE,
        HK_CT_WS, HK_CT_WS, HK_CT_AT, HK_CT_WS, HK_CT_URL, HK_CT_WS, HK_CT_DIMENSION, HK_CT_WS, HK_CT_NUMBER,
        HK_CT_WS, HK_CT_PERCENT, HK_CT_WS, HK_CT_STRING, HK_CT_EOF,
    };
    /* The comment disappears; its surrounding whitespace stays as two tokens. */
    HK_CHECK_EQ(tk.n, sizeof want);
    for (uint32_t i = 0; i < tk.n && i < sizeof want; i++) {
        if (tk.t[i].type != want[i]) hk_test_fail(t, __FILE__, __LINE__, "token %u", i);
    }
    HK_CHECK_STR(tk.t[26].value, "x.png");
    HK_CHECK(tk.t[28].num == 1.5);
    HK_CHECK_STR(tk.t[28].unit, "em");
    HK_CHECK(tk.t[30].num == -3);
    HK_CHECK_STR(tk.t[34].value, "sA");
    hk_arena_free(&a);
}

static bool media(const char *q, float width)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_css_tokens t = hk_css_tokenize(&a, hk_str_from_cstr(q));
    hk_css_media_env env = {width, 800, false};
    bool r = hk_css_media_matches(t.t, t.n - 1, &env);
    hk_arena_free(&a);
    return r;
}

HK_TEST(css_media)
{
    HK_CHECK(media("", 1000));
    HK_CHECK(media("screen", 1000));
    HK_CHECK(!media("print", 1000));
    HK_CHECK(media("print, screen and (min-width: 600px)", 1000));
    HK_CHECK(!media("screen and (max-width: 600px)", 1000));
    HK_CHECK(media("(max-width: 40em)", 600));
    HK_CHECK(media("(width >= 600px)", 600));
    HK_CHECK(media("(400px <= width < 700px)", 600));
    HK_CHECK(!media("(400px <= width < 700px)", 700));
    HK_CHECK(media("not print", 1000));
    HK_CHECK(media("(prefers-color-scheme: light)", 1000));
    HK_CHECK(media("(scripting: none)", 1000));
    HK_CHECK(!media("(unknown-feature: 1)", 1000));
    HK_CHECK(media("(min-width: 100px) and ((max-width: 2000px) or (orientation: portrait))", 1000));
}

/* Parses html + css, computes styles at 1000px, returns the style of the element with id. */
typedef struct fixture {
    hk_document *doc;
    hk_styler *st;
} fixture;

static fixture setup(const char *html, const char *css)
{
    fixture f;
    f.doc = hk_html_parse(html, strlen(html));
    f.st = hk_styler_create(f.doc);
    if (css) hk_styler_add_sheet(f.st, hk_str_from_cstr(css));
    hk_css_media_env env = {1000, 800, false};
    hk_styler_compute(f.st, &env);
    return f;
}

static void teardown(fixture *f)
{
    hk_styler_destroy(f->st);
    hk_document_destroy(f->doc);
}

static hk_node_id by_id(hk_document *d, const char *id)
{
    hk_str want = hk_str_from_cstr(id);
    for (hk_node_id n = 1; n < d->nodes.len; n++) {
        if (d->nodes.data[n].type != HK_NODE_ELEMENT) continue;
        const hk_attr *a = hk_dom_attr(d, n, HK_ATTR_NS_NONE, HK_ATOM_id);
        if (a && hk_str_eq(a->value, want)) return n;
    }
    return HK_NODE_NONE;
}

static const hk_style *style(fixture *f, const char *id)
{
    return hk_style_of(f->st, by_id(f->doc, id));
}

HK_TEST(css_selectors)
{
    fixture f = setup("<div id=root class='a b'><p id=p1>1</p><p id=p2 class=c>2</p><p id=p3 lang=en-US>3</p>"
                      "<span id=s1 data-x='foo bar'>s</span></div><ul><li id=l1>a<li id=l2>b<li id=l3>c</ul>",
                      ".a.b > p { color: #010203 }"
                      "#p2 { color: rgb(0 0 255) }"
                      "div.a p + p.c ~ p { color: red }"
                      "p:first-child { font-weight: bold }"
                      "[lang|=en] { font-style: italic }"
                      "span[data-x~=bar] { color: green }"
                      "li:nth-child(2n+1) { color: #abc }"
                      "li:not(:first-child):not(:last-child) { color: blue }"
                      ":is(#l3, #nothing) { font-weight: 300 }"
                      "p:hover { color: purple }"
                      "p::before { color: purple }");
    HK_CHECK_EQ(style(&f, "p1")->color, 0xFF010203u);
    HK_CHECK_EQ(style(&f, "p2")->color, 0xFF0000FFu);
    HK_CHECK_EQ(style(&f, "p3")->color, 0xFFFF0000u);
    HK_CHECK_EQ(style(&f, "p1")->font_weight, 700);
    HK_CHECK_EQ(style(&f, "p2")->font_weight, 400);
    HK_CHECK(style(&f, "p3")->italic);
    HK_CHECK_EQ(style(&f, "s1")->color, 0xFF008000u);
    HK_CHECK_EQ(style(&f, "l1")->color, 0xFFAABBCCu);
    HK_CHECK_EQ(style(&f, "l2")->color, 0xFF0000FFu);
    HK_CHECK_EQ(style(&f, "l3")->color, 0xFFAABBCCu);
    HK_CHECK_EQ(style(&f, "l3")->font_weight, 300);
    teardown(&f);
}

HK_TEST(css_cascade)
{
    fixture f = setup("<div id=d style='color: #111; margin: 0 auto'><p id=p class=x>t</p><em id=e>e</em></div>",
                      ":root { --main: #00ff00; --size: 20px; }"
                      "div { color: red !important; font-size: var(--size); width: calc(50% - 2em) }"
                      "p { color: var(--main); margin: 1em 2em; padding: 5px 10px 15px; border: 2px solid; }"
                      ".x { line-height: 1.5; font: italic bold 12px/2 Arial, sans-serif }"
                      "em { display: block; color: var(--missing, rgba(10, 20, 30, .5)); font-size: 150% }"
                      "@media (max-width: 500px) { p { color: black } }");
    const hk_style *d = style(&f, "d");
    HK_CHECK_EQ(d->color, 0xFFFF0000u); /* !important beats the style attribute */
    HK_CHECK(fabsf(d->font_size - 20) < 0.01f);
    HK_CHECK_EQ(d->width.unit, HK_LEN_CALC);
    HK_CHECK(fabsf(d->width.px + 40) < 0.01f && fabsf(d->width.pct - 50) < 0.01f);
    HK_CHECK_EQ(d->margin[1].unit, HK_LEN_AUTO);
    HK_CHECK_EQ(d->display, HK_DISPLAY_BLOCK);
    const hk_style *p = style(&f, "p");
    HK_CHECK_EQ(p->color, 0xFF00FF00u);
    HK_CHECK(fabsf(p->font_size - 12) < 0.01f);
    HK_CHECK(fabsf(p->margin[0].px - 12) < 0.01f); /* 1em of the element's own font size */
    HK_CHECK(fabsf(p->padding[3].px - 10) < 0.01f && fabsf(p->padding[2].px - 15) < 0.01f);
    HK_CHECK(fabsf(p->border_width[2] - 2) < 0.01f);
    HK_CHECK_EQ(p->border_color[0], p->color); /* currentcolor */
    HK_CHECK(p->italic && p->font_weight == 700);
    HK_CHECK(fabsf(p->line_height - 24) < 0.01f);
    const hk_style *e = style(&f, "e");
    HK_CHECK_EQ(e->color, 0x800A141Eu);
    HK_CHECK(fabsf(e->font_size - 30) < 0.01f);
    HK_CHECK_EQ(e->display, HK_DISPLAY_BLOCK);
    /* Interning: both <p> and <em> children of the same parent would share only if equal. */
    uint32_t distinct, elements;
    size_t bytes;
    hk_styler_stats(f.st, &distinct, &elements, &bytes);
    HK_CHECK(distinct <= elements);
    teardown(&f);
}

static bool keep_no_drop(void *user, const hk_css_selector *sel)
{
    const hk_atom *drop = user;
    for (uint32_t i = 0; i < sel->n; i++)
        for (uint32_t k = 0; k < sel->c[i].n; k++)
            if (sel->c[i].s[k].kind == HK_SS_CLASS && sel->c[i].s[k].name == *drop) return false;
    return true;
}

HK_TEST(css_filtered_sheet)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_atom_table atoms;
    hk_atom_table_init(&atoms, &a, NULL);
    hk_atom drop = hk_atom_intern(&atoms, HK_STR("drop"));
    /* Escaped strings are unescaped into the temporary arena: they must be copied into a. */
    hk_str src = HK_STR(".drop { color: red } .keep, .drop { content: \"\\41 b\" } "
                        "@media (min-width: 1px) { .drop .x { color: blue } a[title=\"\\41\"] { --v: \"\\42\" } }"
                        "@import \"ignored-after-rules.css\";");
    hk_css_sheet sheet;
    hk_css_parse_sheet_filtered(&a, &atoms, src, keep_no_drop, &drop, &sheet);
    HK_CHECK_EQ(sheet.rules.len, 2u);
    if (sheet.rules.len == 2) {
        const hk_css_rule *r0 = &sheet.rules.data[0], *r1 = &sheet.rules.data[1];
        HK_CHECK_EQ(r0->ndecls, 1u);
        HK_CHECK(r0->ndecls && r0->decls[0].vn > 0);
        bool found = false;
        for (uint32_t i = 0; r0->ndecls && i < r0->decls[0].vn; i++)
            if (r0->decls[0].v[i].type == HK_CT_STRING) found = hk_str_eq(r0->decls[0].v[i].value, HK_STR("Ab"));
        HK_CHECK(found);
        HK_CHECK(r1->cond != NULL && r1->cond->media_n > 0);
        HK_CHECK(hk_str_eq(r1->selectors.sel[0].c[0].s[1].value, HK_STR("A")));
        HK_CHECK(r1->ndecls == 1 && hk_str_eq(r1->decls[0].custom, HK_STR("--v")));
    }
    hk_css_sheet_free(&sheet);
    hk_atom_table_free(&atoms);
    hk_arena_free(&a);
}
