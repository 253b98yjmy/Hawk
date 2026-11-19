/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Layout regressions found on real sites, reduced to minimal documents. */
#include "hk_test.h"
#include "html/hk_html_parser.h"
#include "layout/hk_layout.h"
#include "style/hk_style.h"

#include <string.h>

typedef struct laid_out {
    hk_document *doc;
    hk_styler *st;
    hk_display_list dl;
} laid_out;

static laid_out lay_out(const char *html, float width)
{
    laid_out l;
    l.doc = hk_html_parse(html, strlen(html));
    l.st = hk_styler_create(l.doc);
    hk_css_media_env env = {width, 600, false};
    hk_styler_compute(l.st, &env);
    hk_layout_document(l.doc, l.st, width, 600, NULL, &l.dl);
    return l;
}

static void release(laid_out *l)
{
    hk_display_list_free(&l->dl);
    hk_styler_destroy(l->st);
    hk_document_destroy(l->doc);
}

/* The leftmost glyph run in the vertical band [y0, y1). */
static float first_glyph_x(const hk_display_list *dl, float y0, float y1)
{
    float x = 1e9f;
    for (uint32_t i = 0; i < dl->items.len; i++) {
        const hk_item *it = &dl->items.data[i];
        if (it->kind == HK_ITEM_GLYPHS && it->run.baseline >= y0 && it->run.baseline < y1 && it->x < x) x = it->x;
    }
    return x;
}

HK_TEST(layout_bfc_ignores_outer_floats)
{
    /* DuckDuckGo's results: a float in the header, and result titles that are inline-blocks. An
       inline-block is laid out at the origin before it is moved into its line; the header float must
       not squeeze its text (it is a new block formatting context). */
    laid_out l = lay_out("<body style='margin:0'><div style='float:left;width:300px;height:50px'></div>"
                         "<div style='clear:both;padding-top:100px'><a style='display:inline-block' href=x>Title</a>"
                         "</div>",
                         800);
    float x = first_glyph_x(&l.dl, 100, 200);
    HK_CHECK(x < 10);
    release(&l);

    /* Line boxes beside a float are still shortened in the same formatting context. */
    l = lay_out("<body style='margin:0'><div style='float:left;width:300px;height:50px'></div><p style='margin:0'>Text",
                800);
    x = first_glyph_x(&l.dl, 0, 50);
    HK_CHECK(x >= 300 && x < 310);
    release(&l);
}

HK_TEST(layout_inline_margins)
{
    /* Hacker News: "<b style='margin-right:5px'>Hacker News</b><a>new</a>" keeps the margin. */
    laid_out a = lay_out("<body style='margin:0'><p style='margin:0'><b>AB</b><span>C</span>", 800);
    laid_out b = lay_out("<body style='margin:0'><p style='margin:0'><b style='margin-right:20px'>AB</b><span>C</span>", 800);
    float wa = 0, wb = 0;
    for (uint32_t i = 0; i < a.dl.items.len; i++)
        if (a.dl.items.data[i].kind == HK_ITEM_GLYPHS) wa = a.dl.items.data[i].x;
    for (uint32_t i = 0; i < b.dl.items.len; i++)
        if (b.dl.items.data[i].kind == HK_ITEM_GLYPHS) wb = b.dl.items.data[i].x;
    HK_CHECK(wb - wa > 19 && wb - wa < 21); /* the last run ("C") moved by the margin */
    release(&a);
    release(&b);
}
