/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * M1 text view: draws a document as a sequence of plain paragraphs with the OS
 * text renderer. It is a stopgap until the CSS layout engine (M2) replaces it.
 */
#ifndef HK_TEXTVIEW_H
#define HK_TEXTVIEW_H

#include "base/hk_str.h"
#include "plat/hk_window.h"

typedef enum hk_tv_kind {
    HK_TV_BODY,
    HK_TV_H1,
    HK_TV_H2,
    HK_TV_H3,
    HK_TV_PRE,
    HK_TV_NOTE, /* status and error messages */
} hk_tv_kind;

typedef struct hk_textview hk_textview;

hk_textview *hk_textview_create(void);
void hk_textview_destroy(hk_textview *tv);
void hk_textview_clear(hk_textview *tv);
/* Adds a paragraph (UTF-8). */
void hk_textview_add(hk_textview *tv, hk_tv_kind kind, hk_str text);
/* Lays the paragraphs out for a viewport width (physical pixels) and DPI scale. */
void hk_textview_layout(hk_textview *tv, int width, float scale);
int hk_textview_height(const hk_textview *tv);
void hk_textview_paint(hk_textview *tv, hk_surface *s, int scroll_y, bool dark);

#endif
