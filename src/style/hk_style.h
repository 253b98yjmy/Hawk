/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Style computation: the cascade over the UA and author stylesheets plus style=""
 * attributes, producing one computed style per element (docs/design/03-engine.md, 3).
 * Identical computed styles are shared (interned), so most elements of a page point
 * to a small set of style objects.
 */
#ifndef HK_STYLE_H
#define HK_STYLE_H

#include "css/hk_css.h"
#include "dom/hk_dom.h"

typedef enum hk_display {
    HK_DISPLAY_NONE = 0,
    HK_DISPLAY_INLINE,
    HK_DISPLAY_BLOCK,
    HK_DISPLAY_INLINE_BLOCK,
    HK_DISPLAY_LIST_ITEM,
    HK_DISPLAY_TABLE,
    HK_DISPLAY_TABLE_ROW_GROUP,
    HK_DISPLAY_TABLE_ROW,
    HK_DISPLAY_TABLE_CELL,
    HK_DISPLAY_TABLE_CAPTION,
    HK_DISPLAY_FLEX,
    HK_DISPLAY_GRID,
    HK_DISPLAY_CONTENTS,
} hk_display;

typedef enum hk_len_unit {
    HK_LEN_PX = 0,
    HK_LEN_PCT,
    HK_LEN_AUTO,
    HK_LEN_NONE,
    HK_LEN_CALC, /* px + pct% */
} hk_len_unit;

typedef struct hk_length {
    float px;
    float pct;
    uint8_t unit;
} hk_length;

typedef enum { HK_POS_STATIC, HK_POS_RELATIVE, HK_POS_ABSOLUTE, HK_POS_FIXED, HK_POS_STICKY } hk_position;
/* LEGACY_CENTER (-webkit-center, <center>, align=center) also centers block-level children. */
typedef enum { HK_ALIGN_START, HK_ALIGN_LEFT, HK_ALIGN_RIGHT, HK_ALIGN_CENTER, HK_ALIGN_JUSTIFY, HK_ALIGN_END, HK_ALIGN_LEGACY_CENTER } hk_text_align;
typedef enum { HK_WS_NORMAL, HK_WS_NOWRAP, HK_WS_PRE, HK_WS_PRE_WRAP, HK_WS_PRE_LINE } hk_white_space;
typedef enum { HK_LIST_NONE, HK_LIST_DISC, HK_LIST_CIRCLE, HK_LIST_SQUARE, HK_LIST_DECIMAL } hk_list_style;
typedef enum { HK_BORDER_NONE, HK_BORDER_SOLID, HK_BORDER_DASHED, HK_BORDER_DOTTED, HK_BORDER_DOUBLE } hk_border_style;
typedef enum { HK_TT_NONE, HK_TT_UPPERCASE, HK_TT_LOWERCASE, HK_TT_CAPITALIZE } hk_text_transform;

#define HK_DECO_UNDERLINE 1u
#define HK_DECO_LINE_THROUGH 2u
#define HK_DECO_OVERLINE 4u

typedef struct hk_custom_props hk_custom_props;

/* Computed values. Lengths are CSS px (1/96 in) except percentages and auto. */
typedef struct hk_style {
    /* inherited */
    uint32_t color;    /* 0xAARRGGBB */
    float font_size;
    float line_height; /* px; 0 = normal */
    float line_height_factor; /* > 0: a <number> line-height, recomputed for each element */
    float text_indent;
    uint16_t font_weight;
    uint16_t font_family; /* index into hk_styler_family() */
    uint8_t italic;
    uint8_t text_align;
    uint8_t white_space;
    uint8_t text_transform;
    uint8_t list_style;
    uint8_t visible;
    uint8_t decoration; /* propagated to descendants (approximation of decorating boxes) */
    uint8_t pad0_;
    const hk_custom_props *vars;
    /* not inherited */
    uint8_t display;
    uint8_t position;
    uint8_t floating;
    uint8_t border_box;
    uint8_t overflow_hidden;
    uint8_t border_style[4];
    uint8_t clear;           /* 1 left, 2 right, 3 both */
    uint8_t border_collapse;
    uint8_t valign;          /* 0 baseline, 1 top, 2 middle, 3 bottom (table cells) */
    uint32_t background;
    uint32_t border_color[4];
    float border_width[4]; /* top, right, bottom, left; 0 when the style is none */
    hk_length margin[4];
    hk_length padding[4];
    hk_length width, height, min_width, max_width, min_height, max_height;
    hk_length inset[4]; /* top, right, bottom, left; auto by default */
    float border_spacing;
    int32_t z_index;
} hk_style;

typedef struct hk_styler hk_styler;

hk_styler *hk_styler_create(hk_document *doc);
void hk_styler_destroy(hk_styler *s);

/* Adds an author stylesheet (in document order). The text is copied into the document arena. */
void hk_styler_add_sheet(hk_styler *s, hk_str css);
/* Number of author sheets added so far. */
uint32_t hk_styler_sheet_count(const hk_styler *s);
/* @import URLs found in the most recently added sheet. */
const hk_css_sheet *hk_styler_last_sheet(const hk_styler *s);

/* Computes styles for the whole document. Call again when the viewport changes (media queries). */
void hk_styler_compute(hk_styler *s, const hk_css_media_env *env);

/* Computed style of an element (after hk_styler_compute); NULL for non-elements. */
const hk_style *hk_style_of(const hk_styler *s, hk_node_id id);

/* Resolved font family name (UTF-8) for hk_style.font_family. */
hk_str hk_styler_family(const hk_styler *s, uint16_t index);

/* Statistics: distinct computed styles and elements styled. */
void hk_styler_stats(const hk_styler *s, uint32_t *distinct, uint32_t *elements, size_t *bytes);

/* Resolves a length against a percentage base (px). auto/none resolve to fallback. */
float hk_length_resolve(hk_length l, float base, float fallback);

#endif
