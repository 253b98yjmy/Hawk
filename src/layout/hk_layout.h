/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Layout (M2): block formatting with margin collapsing, and inline formatting
 * contexts laid out by the platform text engine. The result is a display list in
 * document coordinates (CSS px); no box tree is kept after layout.
 *
 * Not yet handled (later milestones): floats, positioning, tables, flexbox, grid,
 * replaced elements (images), clipping. Such boxes are laid out as plain blocks,
 * and absolutely positioned boxes are skipped.
 */
#ifndef HK_LAYOUT_H
#define HK_LAYOUT_H

#include "dom/hk_dom.h"
#include "plat/hk_text.h"
#include "style/hk_style.h"

typedef enum hk_item_kind {
    HK_ITEM_RECT,
    HK_ITEM_GLYPHS,
} hk_item_kind;

typedef struct hk_item {
    uint8_t kind;
    uint32_t color;
    float x, y, w, h; /* bounds (for glyphs: an estimate used for culling) */
    hk_glyph_run run; /* glyphs: positions are absolute */
} hk_item;

typedef struct hk_link_rect {
    float x, y, w, h;
    hk_node_id link; /* the <a> element */
} hk_link_rect;

typedef struct hk_display_list {
    HK_VEC(hk_item) items;
    HK_VEC(hk_link_rect) links;
    float width, height;
    uint32_t canvas; /* background color of the canvas */
} hk_display_list;

/* Lays out the document for a viewport width in CSS px. */
void hk_layout_document(const hk_document *doc, const hk_styler *st, float viewport_width, hk_display_list *out);
void hk_display_list_free(hk_display_list *dl);

/* The link element at a document position, or none. */
hk_node_id hk_display_list_link_at(const hk_display_list *dl, float x, float y);

#endif
