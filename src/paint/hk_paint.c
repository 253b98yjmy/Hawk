/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "paint/hk_paint.h"
#include "base/hk_map64.h"
#include "base/hk_mem.h"

#include <math.h>
#include <string.h>

#define CACHE_LIMIT ((size_t)8 * 1024 * 1024)
#define SUBPIXEL 4

typedef struct cached_glyph {
    void *face;
    int left, top, w, h;
    uint8_t *alpha; /* NULL for empty glyphs */
} cached_glyph;

struct hk_painter {
    hk_map64 index; /* key -> entry index + 1 */
    HK_VEC(cached_glyph) glyphs;
    size_t bytes;
};

hk_painter *hk_painter_create(void)
{
    hk_painter *p = hk_alloc_zero(sizeof *p);
    hk_map64_init(&p->index);
    return p;
}

void hk_painter_clear_cache(hk_painter *p)
{
    for (uint32_t i = 0; i < p->glyphs.len; i++) {
        cached_glyph *g = &p->glyphs.data[i];
        hk_free(g->alpha, (size_t)g->w * (size_t)g->h);
        hk_font_face_release(g->face);
    }
    p->glyphs.len = 0;
    p->bytes = 0;
    hk_map64_free(&p->index);
    hk_map64_init(&p->index);
}

void hk_painter_destroy(hk_painter *p)
{
    if (!p) return;
    hk_painter_clear_cache(p);
    hk_vec_free(&p->glyphs);
    hk_map64_free(&p->index);
    hk_free(p, sizeof *p);
}

size_t hk_painter_cache_bytes(const hk_painter *p)
{
    return p->bytes + (size_t)p->glyphs.cap * sizeof(cached_glyph) + (size_t)p->index.cap * 16;
}

static const cached_glyph *glyph(hk_painter *p, void *face, float size, uint16_t id, int frac)
{
    uint64_t key = (uint64_t)(uintptr_t)face * 0x9E3779B97F4A7C15ull;
    key ^= ((uint64_t)(uint32_t)lroundf(size * 64) << 20) ^ ((uint64_t)id << 2) ^ (uint64_t)frac;
    key = hk_key64_nonzero(key);
    uint64_t v;
    if (hk_map64_get(&p->index, key, &v)) {
        const cached_glyph *g = &p->glyphs.data[v - 1];
        if (g->face == face) return g;
    }
    if (p->bytes > CACHE_LIMIT) hk_painter_clear_cache(p);
    hk_glyph_mask m = {0};
    cached_glyph g = {0};
    g.face = face;
    hk_font_face_retain(face);
    if (hk_glyph_rasterize(face, size, id, (float)frac / SUBPIXEL, &m)) {
        g.left = m.left;
        g.top = m.top;
        g.w = m.w;
        g.h = m.h;
        g.alpha = m.alpha;
        p->bytes += (size_t)m.w * (size_t)m.h;
    }
    hk_vec_push(&p->glyphs, g);
    hk_map64_put(&p->index, key, p->glyphs.len);
    return &p->glyphs.data[p->glyphs.len - 1];
}

/* --- pixels ---------------------------------------------------------------------------------- */

static inline uint32_t blend(uint32_t dst, uint32_t color, uint32_t cov)
{
    /* color is straight ARGB; cov is 0..255 coverage. */
    uint32_t a = ((color >> 24) * cov + 127) / 255;
    if (a == 0) return dst;
    uint32_t ia = 255 - a;
    uint32_t r = (((color >> 16) & 255) * a + ((dst >> 16) & 255) * ia + 127) / 255;
    uint32_t g = (((color >> 8) & 255) * a + ((dst >> 8) & 255) * ia + 127) / 255;
    uint32_t b = ((color & 255) * a + (dst & 255) * ia + 127) / 255;
    return 0xFF000000u | r << 16 | g << 8 | b;
}

static void fill_rect(hk_surface *s, int x0, int y0, int x1, int y1, uint32_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x0 >= x1 || y0 >= y1) return;
    bool opaque = (color >> 24) == 255;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        if (opaque) {
            for (int x = x0; x < x1; x++) row[x] = color;
        } else {
            for (int x = x0; x < x1; x++) row[x] = blend(row[x], color, 255);
        }
    }
}

static void draw_mask(hk_surface *s, const cached_glyph *g, int px, int py, uint32_t color)
{
    int x0 = px + g->left, y0 = py + g->top;
    for (int j = 0; j < g->h; j++) {
        int y = y0 + j;
        if (y < 0 || y >= s->height) continue;
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        const uint8_t *src = g->alpha + (size_t)j * (size_t)g->w;
        for (int i = 0; i < g->w; i++) {
            int x = x0 + i;
            if (x < 0 || x >= s->width || !src[i]) continue;
            row[x] = blend(row[x], color, src[i]);
        }
    }
}

static int snap(float v)
{
    return (int)floorf(v + 0.5f);
}

void hk_paint(hk_painter *p, const hk_display_list *dl, hk_surface *s, float scroll_y, float scale)
{
    fill_rect(s, 0, 0, s->width, s->height, dl->canvas | 0xFF000000u);
    float view_top = scroll_y, view_bottom = scroll_y + (float)s->height / scale;
    for (uint32_t i = 0; i < dl->items.len; i++) {
        const hk_item *it = &dl->items.data[i];
        if (it->y > view_bottom || it->y + it->h < view_top) continue;
        if (it->kind == HK_ITEM_RECT) {
            if ((it->color >> 24) == 0) continue;
            int x0 = snap(it->x * scale), y0 = snap((it->y - scroll_y) * scale);
            int x1 = snap((it->x + it->w) * scale), y1 = snap((it->y + it->h - scroll_y) * scale);
            if (x1 == x0 && it->w > 0) x1 = x0 + 1; /* hairlines stay visible */
            if (y1 == y0 && it->h > 0) y1 = y0 + 1;
            fill_rect(s, x0, y0, x1, y1, it->color);
            continue;
        }
        const hk_glyph_run *r = &it->run;
        float size = r->size * scale;
        float pen = r->x;
        float base = (r->baseline - scroll_y) * scale;
        for (uint32_t k = 0; k < r->count; k++) {
            float adv = r->advances[k];
            float gx = (r->rtl ? pen - adv - r->offsets[2 * k] : pen + r->offsets[2 * k]) * scale;
            float gy = base - r->offsets[2 * k + 1] * scale;
            int ix = (int)floorf(gx);
            int frac = (int)((gx - (float)ix) * SUBPIXEL);
            if (frac >= SUBPIXEL) frac = SUBPIXEL - 1;
            const cached_glyph *g = glyph(p, r->face, size, r->glyphs[k], frac);
            if (g->alpha) draw_mask(s, g, ix, snap(gy), r->color);
            pen += r->rtl ? -adv : adv;
        }
    }
}
