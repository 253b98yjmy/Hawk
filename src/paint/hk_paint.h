/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Software rasterizer: paints a display list into a 32-bit surface (premultiplied
 * BGRA) with a glyph mask cache (docs/design/03-engine.md, 5 and 6).
 */
#ifndef HK_PAINT_H
#define HK_PAINT_H

#include "layout/hk_layout.h"
#include "plat/hk_window.h"

typedef struct hk_painter hk_painter;

hk_painter *hk_painter_create(void);
void hk_painter_destroy(hk_painter *p);
/* Drops cached glyph masks (e.g. when fonts are released with a display list). */
void hk_painter_clear_cache(hk_painter *p);

/* What a form control shows inside its content box. Positions are relative to the content box
 * origin, in CSS px; drawing is clipped to the content box. */
typedef struct hk_control_visual {
    const hk_text_rect *under; /* backgrounds: the selection, check box frames */
    uint32_t nunder;
    const hk_glyph_run *runs;
    uint32_t nruns;
    const hk_text_rect *over; /* the caret, the IME composition underline */
    uint32_t nover;
} hk_control_visual;

typedef struct hk_paint_hooks {
    void *user;
    /* Decoded image pixels (straight ARGB, 0xAARRGGBB) for an <img>, or false when not available yet. */
    bool (*image)(void *user, hk_node_id img, const uint32_t **pixels, int *w, int *h);
    /* The contents of a control; the arrays must stay valid until the next call. */
    bool (*control)(void *user, const hk_item *it, hk_control_visual *v);
} hk_paint_hooks;

/* Paints the part of the document starting at scroll_y (CSS px) at the given device scale. hooks may be NULL. */
void hk_paint(hk_painter *p, const hk_display_list *dl, hk_surface *s, float scroll_y, float scale,
              const hk_paint_hooks *hooks);

/* Bytes held by the glyph cache. */
size_t hk_painter_cache_bytes(const hk_painter *p);

#endif
