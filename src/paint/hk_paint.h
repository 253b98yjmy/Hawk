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

/* Decoded image pixels (straight ARGB, 0xAARRGGBB) for an <img>, or false when not available yet. */
typedef bool (*hk_paint_image_fn)(void *user, hk_node_id img, const uint32_t **pixels, int *w, int *h);

/* Paints the part of the document starting at scroll_y (CSS px) at the given device scale. */
void hk_paint(hk_painter *p, const hk_display_list *dl, hk_surface *s, float scroll_y, float scale,
              hk_paint_image_fn image, void *image_user);

/* Bytes held by the glyph cache. */
size_t hk_painter_cache_bytes(const hk_painter *p);

#endif
