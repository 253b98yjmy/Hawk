/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Layout: block formatting with margin collapsing, floats, positioning, tables,
 * inline formatting contexts laid out by the platform text engine (with inline
 * boxes for inline-blocks and images), and overflow clipping. The result is a
 * display list in document coordinates (CSS px); no box tree is kept.
 *
 * Simplifications are listed in docs/design/03-engine.md, section 4.
 */
#ifndef HK_LAYOUT_H
#define HK_LAYOUT_H

#include "dom/hk_dom.h"
#include "plat/hk_text.h"
#include "style/hk_style.h"

typedef enum hk_item_kind {
    HK_ITEM_RECT,
    HK_ITEM_GLYPHS,
    HK_ITEM_IMAGE,  /* node = the <img> element */
    HK_ITEM_CLIP,   /* starts clipping to (x, y, w, h) until the matching HK_ITEM_UNCLIP */
    HK_ITEM_UNCLIP,
} hk_item_kind;

typedef struct hk_item {
    uint8_t kind;
    uint8_t fixed; /* position: fixed content: painted without scrolling */
    uint32_t color;
    float x, y, w, h; /* bounds (for glyphs: an estimate used for culling) */
    hk_node_id node;
    hk_glyph_run run; /* glyphs: positions are absolute */
} hk_item;

typedef struct hk_link_rect {
    float x, y, w, h;
    hk_node_id link; /* the <a> element */
    uint8_t fixed;
} hk_link_rect;

typedef struct hk_display_list {
    HK_VEC(hk_item) items;
    HK_VEC(hk_link_rect) links;
    float width, height;
    uint32_t canvas; /* background color of the canvas */
} hk_display_list;

typedef enum hk_image_state {
    HK_IMAGE_PENDING = 0, /* size not known yet */
    HK_IMAGE_READY,
    HK_IMAGE_BROKEN, /* failed or blocked: the alt text is shown */
} hk_image_state;

typedef struct hk_layout_hooks {
    void *user;
    /* Intrinsic size of an <img> in CSS px. */
    hk_image_state (*image_size)(void *user, hk_node_id img, float *w, float *h);
} hk_layout_hooks;

/* Lays out the document for a viewport in CSS px. hooks may be NULL. */
void hk_layout_document(const hk_document *doc, const hk_styler *st, float viewport_width, float viewport_height,
                        const hk_layout_hooks *hooks, hk_display_list *out);
void hk_display_list_free(hk_display_list *dl);

/* The link element at a viewport-relative position (scroll in CSS px), or none. */
hk_node_id hk_display_list_link_at(const hk_display_list *dl, float x, float y, float scroll);

#endif
