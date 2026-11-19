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

static int snap(float v)
{
    return (int)floorf(v + 0.5f);
}

typedef struct clip {
    int x0, y0, x1, y1;
} clip;

static void fill_clipped(hk_surface *s, const clip *c, int x0, int y0, int x1, int y1, uint32_t color)
{
    if (x0 < c->x0) x0 = c->x0;
    if (y0 < c->y0) y0 = c->y0;
    if (x1 > c->x1) x1 = c->x1;
    if (y1 > c->y1) y1 = c->y1;
    fill_rect(s, x0, y0, x1, y1, color);
}

static void draw_mask_clipped(hk_surface *s, const clip *c, const cached_glyph *g, int px, int py, uint32_t color)
{
    int x0 = px + g->left, y0 = py + g->top;
    for (int j = 0; j < g->h; j++) {
        int y = y0 + j;
        if (y < c->y0 || y >= c->y1) continue;
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        const uint8_t *src = g->alpha + (size_t)j * (size_t)g->w;
        for (int i = 0; i < g->w; i++) {
            int x = x0 + i;
            if (x < c->x0 || x >= c->x1 || !src[i]) continue;
            row[x] = blend(row[x], color, src[i]);
        }
    }
}

/* --- rounded boxes -------------------------------------------------------------------------------- */

/* Signed distance (device px, negative inside) from a point to a rectangle whose corners are quarter
   ellipses with radii r (top-left x, y, top-right x, y, bottom-right x, y, bottom-left x, y). Inside a
   corner the ellipse distance is estimated to first order, which is exact enough within a pixel of the
   edge, where it decides the coverage. */
static float rbox_dist(float px, float py, float x0, float y0, float x1, float y1, const float r[8])
{
    float cx = 0, cy = 0, rx = 0, ry = 0;
    if (px < x0 + r[0] && py < y0 + r[1]) cx = x0 + (rx = r[0]), cy = y0 + (ry = r[1]);
    else if (px > x1 - r[2] && py < y0 + r[3]) cx = x1 - (rx = r[2]), cy = y0 + (ry = r[3]);
    else if (px > x1 - r[4] && py > y1 - r[5]) cx = x1 - (rx = r[4]), cy = y1 - (ry = r[5]);
    else if (px < x0 + r[6] && py > y1 - r[7]) cx = x0 + (rx = r[6]), cy = y1 - (ry = r[7]);
    if (rx > 0 && ry > 0) {
        float u = (px - cx) / rx, v = (py - cy) / ry;
        float g = sqrtf(u * u / (rx * rx) + v * v / (ry * ry));
        if (g < 1e-6f) return -(rx < ry ? rx : ry);
        return (u * u + v * v - 1) / (2 * g);
    }
    float d = x0 - px;
    if (px - x1 > d) d = px - x1;
    if (y0 - py > d) d = y0 - py;
    if (py - y1 > d) d = py - y1;
    return d;
}

static inline float coverage(float d)
{
    float c = 0.5f - d;
    return c <= 0 ? 0 : c >= 1 ? 1 : c;
}

/* A rounded background, or (b != NULL) a rounded border ring with widths b->bw and colors b->bc. The
   box is (x0, y0)-(x1, y1) in device px; r and the widths are in device px too. */
static void fill_rbox(hk_surface *s, const clip *cl, float x0, float y0, float x1, float y1, const float r[8],
                      const float *bw, const uint32_t *bc, uint32_t color)
{
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < cl->x0) ix0 = cl->x0;
    if (iy0 < cl->y0) iy0 = cl->y0;
    if (ix1 > cl->x1) ix1 = cl->x1;
    if (iy1 > cl->y1) iy1 = cl->y1;
    if (ix0 >= ix1 || iy0 >= iy1) return;
    /* The padding box: the inner edge of the ring, its radii reduced by the border widths. */
    float in_x0 = 0, in_y0 = 0, in_x1 = 0, in_y1 = 0, ir[8] = {0}, band0 = 0, band1 = 0;
    if (bw) {
        in_x0 = x0 + bw[3], in_y0 = y0 + bw[0], in_x1 = x1 - bw[1], in_y1 = y1 - bw[2];
        const float sub[8] = {bw[3], bw[0], bw[1], bw[0], bw[1], bw[2], bw[3], bw[2]};
        for (int i = 0; i < 8; i++) ir[i] = r[i] - sub[i] > 0 ? r[i] - sub[i] : 0;
        for (int k = 0; k < 4; k++) {
            if (ir[2 * k] <= 0 || ir[2 * k + 1] <= 0) ir[2 * k] = ir[2 * k + 1] = 0;
        }
        /* Rows whose inside lies on the straight part of the padding box: skip that inside. */
        band0 = in_y0 + (ir[1] > ir[3] ? ir[1] : ir[3]);
        band1 = in_y1 - (ir[5] > ir[7] ? ir[5] : ir[7]);
    }
    for (int y = iy0; y < iy1; y++) {
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        float py = (float)y + 0.5f;
        int skip0 = ix1, skip1 = ix1;
        if (bw && py - 0.5f >= band0 && py + 0.5f <= band1) {
            skip0 = (int)ceilf(in_x0 + (ir[0] > ir[6] ? ir[0] : ir[6]));
            skip1 = (int)floorf(in_x1 - (ir[2] > ir[4] ? ir[2] : ir[4]));
            if (skip1 < skip0) skip0 = skip1 = ix1;
        }
        for (int x = ix0; x < ix1; x++) {
            if (x == skip0) {
                x = skip1 - 1;
                continue;
            }
            float px = (float)x + 0.5f;
            float cov = coverage(rbox_dist(px, py, x0, y0, x1, y1, r));
            if (cov <= 0) continue;
            uint32_t c = color;
            if (bw) {
                cov *= 1 - coverage(rbox_dist(px, py, in_x0, in_y0, in_x1, in_y1, ir));
                if (cov <= 0) continue;
                /* The side the pixel belongs to: the nearest edge relative to its width, which splits the
                   corners along the line between the outer and inner corners. */
                float best = 1e30f;
                int side = 0;
                const float dist[4] = {py - y0, x1 - px, y1 - py, px - x0};
                for (int k = 0; k < 4; k++) {
                    if (bw[k] <= 0) continue;
                    float t = dist[k] / bw[k];
                    if (t < best) best = t, side = k;
                }
                c = bc[side];
                if ((c >> 24) == 0) continue;
            }
            if (cov >= 1 && (c >> 24) == 255) row[x] = c;
            else row[x] = blend(row[x], c, (uint32_t)(cov * 255 + 0.5f));
        }
    }
}

/* Scales an image into the device rectangle (bilinear when shrinking little, nearest otherwise). With
   radii (device px) the corners are rounded. */
static void draw_image(hk_surface *s, const clip *c, int x0, int y0, int x1, int y1, const uint32_t *px, int pw, int ph,
                       const float *radii)
{
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0 || pw <= 0 || ph <= 0) return;
    int cx0 = x0 < c->x0 ? c->x0 : x0, cy0 = y0 < c->y0 ? c->y0 : y0;
    int cx1 = x1 > c->x1 ? c->x1 : x1, cy1 = y1 > c->y1 ? c->y1 : y1;
    for (int y = cy0; y < cy1; y++) {
        int sy = (int)(((int64_t)(y - y0) * ph) / h);
        if (sy >= ph) sy = ph - 1;
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        const uint32_t *src = px + (size_t)sy * (size_t)pw;
        for (int x = cx0; x < cx1; x++) {
            int sx = (int)(((int64_t)(x - x0) * pw) / w);
            if (sx >= pw) sx = pw - 1;
            uint32_t v = src[sx];
            uint32_t a = v >> 24;
            if (radii) {
                float cov = coverage(rbox_dist((float)x + 0.5f, (float)y + 0.5f, (float)x0, (float)y0, (float)x1,
                                               (float)y1, radii));
                a = (uint32_t)(a * cov + 0.5f);
            }
            if (a == 255) row[x] = v;
            else if (a) row[x] = blend(row[x], v | 0xFF000000u, a);
        }
    }
}

/* Draws a glyph run offset by (dx, dy) CSS px (dy already includes the scroll). */
static void draw_run(hk_painter *p, hk_surface *s, const clip *cl, const hk_glyph_run *r, float dx, float dy, float scale)
{
    float size = r->size * scale;
    float pen = r->x + dx;
    float base = (r->baseline + dy) * scale;
    for (uint32_t k = 0; k < r->count; k++) {
        float adv = r->advances[k];
        float gx = (r->rtl ? pen - adv - r->offsets[2 * k] : pen + r->offsets[2 * k]) * scale;
        float gy = base - r->offsets[2 * k + 1] * scale;
        int ix = (int)floorf(gx);
        int frac = (int)((gx - (float)ix) * SUBPIXEL);
        if (frac >= SUBPIXEL) frac = SUBPIXEL - 1;
        const cached_glyph *g = glyph(p, r->face, size, r->glyphs[k], frac);
        if (g->alpha) draw_mask_clipped(s, cl, g, ix, snap(gy), r->color);
        pen += r->rtl ? -adv : adv;
    }
}

/* An anti-aliased rounded rectangle (device px), coverage from the signed distance at pixel centers. */
static void fill_round(hk_surface *s, const clip *cl, float x0, float y0, float x1, float y1, float radius,
                       uint32_t color)
{
    int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
    if (ix0 < cl->x0) ix0 = cl->x0;
    if (iy0 < cl->y0) iy0 = cl->y0;
    if (ix1 > cl->x1) ix1 = cl->x1;
    if (iy1 > cl->y1) iy1 = cl->y1;
    float hw = (x1 - x0) / 2, hh = (y1 - y0) / 2, cx = x0 + hw, cy = y0 + hh;
    if (radius > hw) radius = hw;
    if (radius > hh) radius = hh;
    for (int y = iy0; y < iy1; y++) {
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        float qy = fabsf((float)y + 0.5f - cy) - (hh - radius);
        for (int x = ix0; x < ix1; x++) {
            float qx = fabsf((float)x + 0.5f - cx) - (hw - radius);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float inside = qx > qy ? qx : qy;
            float d = sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - radius;
            float cov = 0.5f - d;
            if (cov <= 0) continue;
            row[x] = blend(row[x], color, cov >= 1 ? 255u : (uint32_t)(cov * 255));
        }
    }
}

static void draw_text_rects(hk_surface *s, const clip *cl, const hk_text_rect *r, uint32_t n, float dx, float dy,
                            float scale)
{
    for (uint32_t i = 0; i < n; i++) {
        if (r[i].radius > 0) {
            fill_round(s, cl, (r[i].x + dx) * scale, (r[i].y + dy) * scale, (r[i].x + r[i].w + dx) * scale,
                       (r[i].y + r[i].h + dy) * scale, r[i].radius * scale, r[i].color);
            continue;
        }
        int x0 = snap((r[i].x + dx) * scale), y0 = snap((r[i].y + dy) * scale);
        int x1 = snap((r[i].x + r[i].w + dx) * scale), y1 = snap((r[i].y + r[i].h + dy) * scale);
        if (x1 == x0 && r[i].w > 0) x1 = x0 + 1;
        if (y1 == y0 && r[i].h > 0) y1 = y0 + 1;
        fill_clipped(s, cl, x0, y0, x1, y1, r[i].color);
    }
}

static void paint_control(hk_painter *p, hk_surface *s, const clip *outer, const hk_item *it, float scroll, float scale,
                          const hk_paint_hooks *hooks)
{
    hk_control_visual v = {0};
    if (!hooks || !hooks->control || !hooks->control(hooks->user, it, &v)) return;
    /* Clip to the content box, widened a little so that a caret at either end stays visible. */
    clip c = {snap((it->x - 1) * scale), snap((it->y - scroll) * scale), snap((it->x + it->w + 1) * scale),
              snap((it->y + it->h - scroll) * scale)};
    if (c.x0 < outer->x0) c.x0 = outer->x0;
    if (c.y0 < outer->y0) c.y0 = outer->y0;
    if (c.x1 > outer->x1) c.x1 = outer->x1;
    if (c.y1 > outer->y1) c.y1 = outer->y1;
    if (c.x0 >= c.x1 || c.y0 >= c.y1) return;
    float dy = it->y - scroll;
    draw_text_rects(s, &c, v.under, v.nunder, it->x, dy, scale);
    for (uint32_t i = 0; i < v.nruns; i++) draw_run(p, s, &c, &v.runs[i], it->x, dy, scale);
    draw_text_rects(s, &c, v.over, v.nover, it->x, dy, scale);
}

static void paint_pass(hk_painter *p, const hk_display_list *dl, hk_surface *s, float scroll_y, float scale,
                       const hk_paint_hooks *hooks, bool fixed_pass)
{
    float scroll = fixed_pass ? 0 : scroll_y;
    float view_top = scroll, view_bottom = scroll + (float)s->height / scale;
    clip stack[64];
    int depth = 0;
    stack[0] = (clip){0, 0, s->width, s->height};
    int skipped_clips = 0; /* clips opened by items of the other pass */
    for (uint32_t i = 0; i < dl->items.len; i++) {
        const hk_item *it = &dl->items.data[i];
        if ((it->fixed != 0) != fixed_pass) {
            if (it->kind == HK_ITEM_CLIP) skipped_clips++;
            else if (it->kind == HK_ITEM_UNCLIP && skipped_clips) skipped_clips--;
            continue;
        }
        const clip *cl = &stack[depth];
        if (it->kind == HK_ITEM_CLIP) {
            clip n = {snap(it->x * scale), snap((it->y - scroll) * scale), snap((it->x + it->w) * scale),
                      snap((it->y + it->h - scroll) * scale)};
            if (n.x0 < cl->x0) n.x0 = cl->x0;
            if (n.y0 < cl->y0) n.y0 = cl->y0;
            if (n.x1 > cl->x1) n.x1 = cl->x1;
            if (n.y1 > cl->y1) n.y1 = cl->y1;
            if (depth < 63) stack[++depth] = n;
            continue;
        }
        if (it->kind == HK_ITEM_UNCLIP) {
            if (depth > 0) depth--;
            continue;
        }
        if (it->y > view_bottom || it->y + it->h < view_top) continue;
        if (cl->x0 >= cl->x1 || cl->y0 >= cl->y1) continue;
        if (it->kind == HK_ITEM_RECT) {
            if ((it->color >> 24) == 0) continue;
            int x0 = snap(it->x * scale), y0 = snap((it->y - scroll) * scale);
            int x1 = snap((it->x + it->w) * scale), y1 = snap((it->y + it->h - scroll) * scale);
            if (x1 == x0 && it->w > 0) x1 = x0 + 1;
            if (y1 == y0 && it->h > 0) y1 = y0 + 1;
            fill_clipped(s, cl, x0, y0, x1, y1, it->color);
            continue;
        }
        if (it->kind == HK_ITEM_CONTROL) {
            paint_control(p, s, cl, it, scroll, scale, hooks);
            continue;
        }
        if (it->kind == HK_ITEM_IMAGE) {
            const uint32_t *px;
            int pw, ph;
            if (hooks && hooks->image && hooks->image(hooks->user, it->node, &px, &pw, &ph)) {
                float r[8];
                bool round = false;
                for (int k = 0; k < 8; k++) {
                    r[k] = it->rbox.r[k] * scale;
                    if (r[k] > 0) round = true;
                }
                draw_image(s, cl, snap(it->x * scale), snap((it->y - scroll) * scale), snap((it->x + it->w) * scale),
                           snap((it->y + it->h - scroll) * scale), px, pw, ph, round ? r : NULL);
            }
            continue;
        }
        if (it->kind == HK_ITEM_RBOX) {
            float r[8], bw[4];
            bool ring = false;
            for (int k = 0; k < 8; k++) r[k] = it->rbox.r[k] * scale;
            for (int k = 0; k < 4; k++) {
                bw[k] = it->rbox.bw[k] * scale;
                if (bw[k] > 0) ring = true;
            }
            fill_rbox(s, cl, it->x * scale, (it->y - scroll) * scale, (it->x + it->w) * scale,
                      (it->y + it->h - scroll) * scale, r, ring ? bw : NULL, it->rbox.bc, it->color);
            continue;
        }
        if (it->kind == HK_ITEM_GLYPHS) draw_run(p, s, cl, &it->run, 0, -scroll, scale);
    }
}

void hk_paint(hk_painter *p, const hk_display_list *dl, hk_surface *s, float scroll_y, float scale,
              const hk_paint_hooks *hooks)
{
    fill_rect(s, 0, 0, s->width, s->height, dl->canvas | 0xFF000000u);
    paint_pass(p, dl, s, scroll_y, scale, hooks, false);
    paint_pass(p, dl, s, scroll_y, scale, hooks, true);
}
