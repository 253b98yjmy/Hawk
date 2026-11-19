/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Layout regressions found on real sites, reduced to minimal documents. */
#include "hk_test.h"
#include "html/hk_html_parser.h"
#include "layout/hk_layout.h"
#include "paint/hk_paint.h"
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

static const hk_item *find_kind(const hk_display_list *dl, uint8_t kind, uint32_t nth)
{
    for (uint32_t i = 0; i < dl->items.len; i++) {
        if (dl->items.data[i].kind == kind && nth-- == 0) return &dl->items.data[i];
    }
    return NULL;
}

HK_TEST(layout_border_radius)
{
    /* Shorthand with "/": horizontal radii 10 20 (repeated), vertical 5. */
    laid_out l = lay_out("<body style='margin:0'><div style='width:200px;height:100px;background:red;"
                         "border-radius:10px 20px / 5px'></div>",
                         800);
    const hk_item *bg = find_kind(&l.dl, HK_ITEM_RBOX, 0);
    HK_CHECK(bg != NULL);
    if (bg) {
        HK_CHECK(bg->w == 200 && bg->h == 100 && bg->color == 0xFFFF0000u);
        const float want[8] = {10, 5, 20, 5, 10, 5, 20, 5};
        for (int i = 0; i < 8; i++) HK_CHECK(bg->rbox.r[i] == want[i]);
        HK_CHECK(bg->rbox.bw[0] == 0); /* a background, not a border ring */
    }
    release(&l);

    /* Percentages are of the border box; radii too large for the box are scaled down together. */
    l = lay_out("<body style='margin:0'><div style='width:100px;height:40px;border:2px solid blue;"
                "border-top-left-radius:50%;border-bottom-right-radius:999px'></div>",
                800);
    const hk_item *ring = find_kind(&l.dl, HK_ITEM_RBOX, 0);
    HK_CHECK(ring != NULL && ring->rbox.bw[0] == 2 && ring->rbox.bc[1] == 0xFF0000FFu);
    if (ring) {
        /* Border box 104 x 44. top-left: 52 x 22; bottom-right: 999 x 999. The right side (0 + 999
           against 44) needs the smallest factor: 44 / 999. */
        float f = 44.0f / 999.0f;
        HK_CHECK(ring->rbox.r[0] > 52 * f - 0.01f && ring->rbox.r[0] < 52 * f + 0.01f);
        HK_CHECK(ring->rbox.r[5] > 999 * f - 0.01f && ring->rbox.r[5] < 999 * f + 0.01f);
        HK_CHECK(ring->rbox.r[2] == 0 && ring->rbox.r[6] == 0);
    }
    release(&l);

    /* No radius: plain rectangles as before. */
    l = lay_out("<body style='margin:0'><div style='height:10px;background:red'></div>", 800);
    HK_CHECK(find_kind(&l.dl, HK_ITEM_RBOX, 0) == NULL);
    release(&l);
}

HK_TEST(paint_border_radius)
{
    laid_out l = lay_out("<body style='margin:0;background:white'><div style='width:40px;height:40px;"
                         "background:#f00;border-radius:20px'></div>",
                         40);
    uint32_t px[40 * 40];
    hk_surface s = {px, 40, 40, 40, NULL};
    hk_painter *p = hk_painter_create();
    hk_paint(p, &l.dl, &s, 0, 1, NULL);
    HK_CHECK((px[0] & 0xFFFFFF) == 0xFFFFFF);            /* the corner is outside the circle */
    HK_CHECK((px[20 * 40 + 20] & 0xFFFFFF) == 0xFF0000); /* the middle is filled */
    HK_CHECK((px[20 * 40 + 0] & 0xFFFFFF) != 0xFFFFFF);  /* the left edge, half way down, is covered */
    uint32_t edge = px[5 * 40 + 6];                      /* near the circle's edge: partly covered */
    HK_CHECK((edge & 0xFF) > 0 && (edge & 0xFF) < 255);
    hk_painter_destroy(p);
    release(&l);
}
