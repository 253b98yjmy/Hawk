/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Drawing of form controls inside their content boxes (the painter's control hook), and the
 * mapping between points and text positions for editing. Text controls scroll so that the caret
 * stays visible; the IME composition is drawn inline at the caret.
 */
#ifndef HK_FORM_VIEW_H
#define HK_FORM_VIEW_H

#include "form/hk_form.h"
#include "layout/hk_layout.h"
#include "paint/hk_paint.h"

typedef struct hk_form_ctx {
    const hk_document *doc;
    const hk_styler *st;
    hk_forms *forms;
    hk_node_id focus;  /* the focused control, or none */
    bool show_caret;
    const uint16_t *comp; /* IME composition string (UTF-16), shown at the caret of the focused control */
    uint32_t comp_len, comp_caret;
} hk_form_ctx;

typedef struct hk_form_view hk_form_view;

hk_form_view *hk_form_view_create(void);
void hk_form_view_destroy(hk_form_view *v);

/* Fills out for a control item; the arrays stay valid until the next call. */
bool hk_form_view_visual(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, hk_control_visual *out);
/* The text position (byte offset into the value) for a point relative to the content box. */
uint32_t hk_form_view_hit(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, float x, float y);
/* The caret of the focused control relative to its content box (for placing IME windows). */
void hk_form_view_caret(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, float *x, float *y, float *h);

#endif
