/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui/hk_chrome.h"

#include "base/hk_utf8.h"

#include <string.h>

#define BIG 1e7f

typedef struct palette {
    uint32_t strip, toolbar, text, dim, hover, address, border, disabled, accent;
} palette;

static const palette k_light = {0xFFDEE1E6u, 0xFFFFFFFFu, 0xFF1F1F1Fu, 0xFF5F6368u, 0x1A000000u,
                                0xFFF1F3F4u, 0xFFC7C9CCu, 0xFFB8BBBEu, 0xFF1A73E8u};
static const palette k_dark = {0xFF202124u, 0xFF35363Au, 0xFFE8EAEDu, 0xFF9AA0A6u, 0x26FFFFFFu,
                               0xFF202124u, 0xFF4A4D51u, 0xFF6A6E73u, 0xFF8AB4F8u};

void hk_chrome_address_colors(bool dark, uint32_t *background, uint32_t *text)
{
    const palette *p = dark ? &k_dark : &k_light;
    *background = p->address;
    *text = p->text;
}

/* --- geometry ----------------------------------------------------------------------------------- */

typedef struct geo {
    float tab_x, tab_w;
    uint32_t ntabs;
    hk_chrome_rect new_tab, back, forward, reload, address;
} geo;

static void geometry(const hk_chrome_state *s, float width, geo *g)
{
    g->ntabs = s->ntabs ? s->ntabs : 1;
    g->tab_x = 8;
    float avail = width - 8 - 44;
    g->tab_w = avail / (float)g->ntabs;
    if (g->tab_w > 240) g->tab_w = 240;
    if (g->tab_w < 36) g->tab_w = 36;
    float end = g->tab_x + g->tab_w * (float)g->ntabs;
    if (end > width - 40) end = width - 40;
    g->new_tab = (hk_chrome_rect){end + 4, 7, 28, 24};
    float ty = HK_CHROME_TAB_ROW + 4;
    g->back = (hk_chrome_rect){6, ty, 30, 30};
    g->forward = (hk_chrome_rect){38, ty, 30, 30};
    g->reload = (hk_chrome_rect){70, ty, 30, 30};
    g->address = (hk_chrome_rect){108, ty + 1, width - 108 - 10, 28};
    if (g->address.w < 40) g->address.w = 40;
}

static hk_chrome_rect tab_rect(const geo *g, uint32_t i)
{
    return (hk_chrome_rect){g->tab_x + g->tab_w * (float)i, 6, g->tab_w, HK_CHROME_TAB_ROW - 6};
}

static bool has_close(const geo *g, uint32_t i, uint32_t active)
{
    return g->tab_w >= 72 || i == active;
}

static hk_chrome_rect close_rect(const geo *g, uint32_t i)
{
    hk_chrome_rect t = tab_rect(g, i);
    return (hk_chrome_rect){t.x + t.w - 26, t.y + 5, 18, 18};
}

static bool inside(hk_chrome_rect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

hk_chrome_hit hk_chrome_hit_test(const hk_chrome_state *s, float width, float x, float y)
{
    geo g;
    geometry(s, width, &g);
    hk_chrome_hit h = {HK_CHROME_NONE, 0};
    if (y < HK_CHROME_TAB_ROW) {
        for (uint32_t i = 0; i < s->ntabs; i++) {
            if (!inside(tab_rect(&g, i), x, y)) continue;
            h.index = i;
            h.part = has_close(&g, i, s->active) && inside(close_rect(&g, i), x, y) ? HK_CHROME_TAB_CLOSE : HK_CHROME_TAB;
            return h;
        }
        h.part = inside(g.new_tab, x, y) ? HK_CHROME_NEW_TAB : HK_CHROME_STRIP;
        return h;
    }
    if (y >= HK_CHROME_HEIGHT) return h;
    if (inside(g.back, x, y)) h.part = HK_CHROME_BACK;
    else if (inside(g.forward, x, y)) h.part = HK_CHROME_FORWARD;
    else if (inside(g.reload, x, y)) h.part = HK_CHROME_RELOAD;
    else if (inside(g.address, x, y)) h.part = HK_CHROME_ADDRESS;
    return h;
}

/* --- drawing ------------------------------------------------------------------------------------- */

static hk_item *push(hk_display_list *dl, uint8_t kind)
{
    hk_item it = {0};
    it.kind = kind;
    hk_vec_push(&dl->items, it);
    return &dl->items.data[dl->items.len - 1];
}

static void rect(hk_display_list *dl, hk_chrome_rect r, uint32_t color)
{
    if (!(color >> 24) || r.w <= 0 || r.h <= 0) return;
    hk_item *it = push(dl, HK_ITEM_RECT);
    it->x = r.x;
    it->y = r.y;
    it->w = r.w;
    it->h = r.h;
    it->color = color;
}

static void clip(hk_display_list *dl, hk_chrome_rect r)
{
    hk_item *it = push(dl, HK_ITEM_CLIP);
    it->x = r.x;
    it->y = r.y;
    it->w = r.w;
    it->h = r.h;
}

static void sink_run(void *user, const hk_glyph_run *r)
{
    hk_display_list *dl = ((void **)user)[0];
    const float *off = ((void **)user)[1];
    hk_item *it = push(dl, HK_ITEM_GLYPHS);
    it->color = r->color;
    it->run = *r;
    it->run.x += off[0];
    it->run.baseline += off[1];
    float w = 0;
    for (uint32_t i = 0; i < r->count; i++) w += r->advances[i];
    it->x = it->run.x;
    it->w = w;
    it->y = it->run.baseline - r->size * 1.2f;
    it->h = r->size * 1.6f;
}

/* Draws text inside box: left-aligned (align 0) or centered (1), vertically centered. */
static void text(hk_display_list *dl, const uint16_t *u, uint32_t n, hk_chrome_rect box, float size, uint32_t color,
                 const char *family, int align)
{
    if (!n) return;
    hk_text_style ts = {0};
    ts.family = hk_str_from_cstr(family);
    ts.size = size;
    ts.weight = 400;
    ts.color = color;
    hk_text_para para = {0};
    para.max_width = BIG;
    hk_text_layout *l = hk_text_layout_create(u, n, &para, &ts);
    if (!l) return;
    float w, h, mn, mx;
    hk_text_layout_size(l, &w, &h);
    hk_text_layout_intrinsic(l, &mn, &mx);
    float off[2] = {box.x + (align ? (box.w - mx) / 2 : 0), box.y + (box.h - h) / 2};
    void *user[2] = {dl, off};
    hk_text_sink sink = {user, sink_run, NULL, NULL};
    hk_text_layout_emit(l, &sink);
    hk_text_layout_destroy(l);
}

static void text_utf8(hk_display_list *dl, hk_str s, hk_chrome_rect box, float size, uint32_t color, int align)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    size_t n;
    uint16_t *u = hk_utf8_to_utf16(&a, s, &n);
    text(dl, u, (uint32_t)n, box, size, color, "Segoe UI", align);
    hk_arena_free(&a);
}

/* An icon from the system icon font (Segoe Fluent Icons or Segoe MDL2 Assets), else a fallback character. */
static void icon(hk_display_list *dl, uint16_t glyph, uint16_t fallback, hk_chrome_rect box, float size, uint32_t color)
{
    static int font = -1; /* 0 none, 1 Fluent, 2 MDL2 */
    if (font < 0)
        font = hk_font_family_exists(HK_STR("Segoe Fluent Icons"))  ? 1
               : hk_font_family_exists(HK_STR("Segoe MDL2 Assets")) ? 2
                                                                     : 0;
    if (font) text(dl, &glyph, 1, box, size, color, font == 1 ? "Segoe Fluent Icons" : "Segoe MDL2 Assets", 1);
    else text(dl, &fallback, 1, box, size * 1.3f, color, "Segoe UI Symbol", 1);
}

static bool hovered(const hk_chrome_state *s, hk_chrome_part part, uint32_t index)
{
    return s->hover.part == part && s->hover.index == index;
}

static void button(hk_display_list *dl, const hk_chrome_state *s, const palette *p, hk_chrome_part part,
                   hk_chrome_rect r, uint16_t glyph, uint16_t fallback, bool enabled)
{
    if (enabled && hovered(s, part, 0)) rect(dl, r, p->hover);
    icon(dl, glyph, fallback, r, 14, enabled ? p->text : p->disabled);
}

void hk_chrome_build(const hk_chrome_state *s, float width, hk_display_list *out, hk_chrome_rect *address)
{
    memset(out, 0, sizeof *out);
    const palette *p = s->dark ? &k_dark : &k_light;
    out->width = width;
    out->height = HK_CHROME_HEIGHT;
    out->canvas = p->strip;
    geo g;
    geometry(s, width, &g);

    /* The tab row. */
    for (uint32_t i = 0; i < s->ntabs; i++) {
        hk_chrome_rect t = tab_rect(&g, i);
        bool active = i == s->active;
        if (active) rect(out, (hk_chrome_rect){t.x, t.y, t.w, t.h + 1}, p->toolbar);
        else if (hovered(s, HK_CHROME_TAB, i) || hovered(s, HK_CHROME_TAB_CLOSE, i)) rect(out, t, p->hover);
        else if (i + 1 != s->active && i + 1 < s->ntabs)
            rect(out, (hk_chrome_rect){t.x + t.w - 0.5f, t.y + 7, 1, t.h - 14}, p->border);
        bool close = has_close(&g, i, s->active);
        hk_chrome_rect title = {t.x + 10, t.y, t.w - 20 - (close ? 22 : 0), t.h};
        clip(out, title);
        hk_str label = s->tabs[i].title.n ? s->tabs[i].title : HK_STR("新しいタブ");
        text_utf8(out, label, title, 12.5f, active ? p->text : p->dim, 0);
        push(out, HK_ITEM_UNCLIP);
        if (close) {
            hk_chrome_rect c = close_rect(&g, i);
            if (hovered(s, HK_CHROME_TAB_CLOSE, i)) rect(out, c, p->hover);
            icon(out, 0xE711, 0x00D7, c, 9, active ? p->text : p->dim);
        }
        if (s->tabs[i].loading) rect(out, (hk_chrome_rect){t.x + 8, t.y + t.h - 3, t.w - 16, 2}, p->accent);
    }
    if (hovered(s, HK_CHROME_NEW_TAB, 0)) rect(out, g.new_tab, p->hover);
    icon(out, 0xE710, '+', g.new_tab, 12, p->text);

    /* The toolbar. */
    rect(out, (hk_chrome_rect){0, HK_CHROME_TAB_ROW, width, HK_CHROME_HEIGHT - HK_CHROME_TAB_ROW}, p->toolbar);
    rect(out, (hk_chrome_rect){0, HK_CHROME_HEIGHT - 1, width, 1}, p->border);
    button(out, s, p, HK_CHROME_BACK, g.back, 0xE72B, 0x2190, s->can_back);
    button(out, s, p, HK_CHROME_FORWARD, g.forward, 0xE72A, 0x2192, s->can_forward);
    button(out, s, p, HK_CHROME_RELOAD, g.reload, s->loading ? 0xE711 : 0xE72C, s->loading ? 0x00D7 : 0x21BB, true);
    rect(out, g.address, p->address);
    *address = g.address;
}
