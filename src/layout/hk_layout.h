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
#include "form/hk_form.h"
#include "plat/hk_text.h"
#include "style/hk_style.h"

typedef enum hk_item_kind {
    HK_ITEM_RECT,
    HK_ITEM_GLYPHS,
    HK_ITEM_IMAGE,  /* node = the <img> element */
    HK_ITEM_CLIP,   /* starts clipping to (x, y, w, h) until the matching HK_ITEM_UNCLIP */
    HK_ITEM_UNCLIP,
    HK_ITEM_CONTROL, /* node = a form control; (x, y, w, h) is its content box, drawn by the painter's hook */
    HK_ITEM_RBOX,    /* a box with rounded corners: (x, y, w, h) is the border box; see hk_rbox */
} hk_item_kind;

/* A rounded box. Without borders (all of bw 0) it is a background filled with the item's color;
 * otherwise it is the border ring between the border box and the padding box, each side in its color.
 * Image items use r alone, to round the image. */
typedef struct hk_rbox {
    float r[8];     /* horizontal and vertical radius per corner (top-left, top-right, bottom-right,
                       bottom-left), in px, already reduced so that adjacent corners do not overlap */
    float bw[4];    /* border widths: top, right, bottom, left */
    uint32_t bc[4]; /* border colors */
} hk_rbox;

typedef struct hk_item {
    uint8_t kind;
    uint8_t fixed; /* position: fixed content: painted without scrolling */
    uint32_t color;
    float x, y, w, h; /* bounds (for glyphs: an estimate used for culling) */
    hk_node_id node;
    union {
        hk_glyph_run run; /* glyphs: positions are absolute */
        hk_rbox rbox;     /* HK_ITEM_RBOX, and HK_ITEM_IMAGE with rounded corners */
    };
} hk_item;

/* A clickable area: a link, a <label>, a <button> or a form control (their border boxes). */
typedef struct hk_link_rect {
    float x, y, w, h;
    hk_node_id link; /* the <a>, <label>, <button> or control element */
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
/* The content box of a control's HK_ITEM_CONTROL item in document coordinates, or NULL. */
const hk_item *hk_display_list_control(const hk_display_list *dl, hk_node_id el);

/* The single line box of a control's text in its font: height and baseline (from the line top). */
void hk_layout_control_line(const hk_styler *st, const hk_style *s, float *height, float *baseline);

#endif
