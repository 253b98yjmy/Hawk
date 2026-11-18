/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Text shaping, line breaking and glyph rasterization (DirectWrite on Windows).
 * A text layout is one paragraph of styled text, laid out for a maximum width.
 * Its glyph runs are copied out (hk_text_layout_runs) so that the layout itself
 * can be released right after layout; drawing only needs the runs.
 */
#ifndef HK_TEXT_H
#define HK_TEXT_H

#include "base/hk_common.h"
#include "base/hk_str.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Does an installed font family with this name exist? */
bool hk_font_family_exists(hk_str family);

typedef struct hk_text_style {
    hk_str family; /* UTF-8 */
    float size;    /* px */
    uint16_t weight;
    bool italic;
    bool underline;
    bool line_through;
    uint32_t color;   /* 0xAARRGGBB */
    uint32_t user;    /* passed back with runs and hit tests (e.g. a link id) */
} hk_text_style;

typedef enum hk_text_align_mode { HK_TEXT_LEFT, HK_TEXT_RIGHT, HK_TEXT_CENTER, HK_TEXT_JUSTIFY } hk_text_align_mode;

typedef struct hk_text_para {
    float max_width;     /* px */
    float line_height;   /* px; 0 = font default */
    hk_text_align_mode align;
    bool wrap;
    float indent;        /* first-line indent, px */
} hk_text_para;

typedef struct hk_text_layout hk_text_layout;

/* text is UTF-16. The default style applies to all text; ranges can be restyled. */
hk_text_layout *hk_text_layout_create(const uint16_t *text, uint32_t len, const hk_text_para *para,
                                      const hk_text_style *base);
void hk_text_layout_set_style(hk_text_layout *l, uint32_t start, uint32_t len, const hk_text_style *s);
/* Size of the laid out text (after the first call, styles can no longer be changed). */
void hk_text_layout_size(hk_text_layout *l, float *width, float *height);
/* Width of the longest unbreakable piece and of the text without wrapping (for shrink-to-fit). */
void hk_text_layout_intrinsic(hk_text_layout *l, float *min_width, float *max_width);

/* A run of glyphs sharing one font face, size and color, positioned at (x, baseline). */
typedef struct hk_glyph_run {
    void *face;          /* font face; reference held by the run until hk_glyph_run_release */
    float size;
    float x, baseline;   /* relative to the layout origin */
    uint32_t count;
    uint16_t *glyphs;
    float *advances;
    float *offsets;      /* pairs: advance offset, ascender offset */
    uint32_t color;
    uint32_t user;
    bool rtl;
} hk_glyph_run;

typedef struct hk_text_rect {
    float x, y, w, h;
    uint32_t color;
    uint32_t user;
} hk_text_rect;

/* Reserves a w x h box at text position pos (the text must hold U+FFFC there). baseline is the
 * distance from the box top to its baseline. user identifies the box in hk_text_sink.box. */
void hk_text_layout_set_inline_box(hk_text_layout *l, uint32_t pos, float w, float h, float baseline, uint32_t user);

typedef struct hk_text_sink {
    void *user;
    /* Glyph arrays are heap-allocated and owned by the receiver. */
    void (*run)(void *user, const hk_glyph_run *r);
    void (*rect)(void *user, const hk_text_rect *r); /* underlines and strike-throughs */
    void (*box)(void *user, float x, float y, uint32_t box_user); /* top-left of an inline box */
} hk_text_sink;

void hk_text_layout_emit(hk_text_layout *l, const hk_text_sink *sink);
/* Rectangles covering a text range (for links). */
void hk_text_layout_range_rects(hk_text_layout *l, uint32_t start, uint32_t len,
                                void (*on_rect)(void *user, const hk_text_rect *r), void *user);
void hk_text_layout_destroy(hk_text_layout *l);

void hk_font_face_retain(void *face);
void hk_font_face_release(void *face);

/* Rasterizes one glyph as an 8-bit coverage mask. x_frac is the subpixel offset (0..1).
 * Returns false for empty glyphs. The mask is heap-allocated (hk_alloc, w * h bytes). */
typedef struct hk_glyph_mask {
    int left, top; /* offset of the mask from the pen position (top is negative above the baseline) */
    int w, h;
    uint8_t *alpha;
} hk_glyph_mask;

bool hk_glyph_rasterize(void *face, float size, uint16_t glyph, float x_frac, hk_glyph_mask *out);

#ifdef __cplusplus
}
#endif

#endif
