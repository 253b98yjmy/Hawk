/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * M1: turns a parsed document into readable paragraphs (block elements become
 * paragraph breaks, whitespace is collapsed, non-rendered elements are skipped).
 */
#ifndef HK_TEXTDOC_H
#define HK_TEXTDOC_H

#include "dom/hk_dom.h"
#include "plat/hk_textview.h"

typedef void (*hk_textdoc_emit_fn)(void *user, hk_tv_kind kind, hk_str text);

/* Emits the paragraphs of the body; *title receives the <title> text (in the document arena). */
void hk_textdoc_build(hk_document *d, hk_textdoc_emit_fn emit, void *user, hk_str *title);

#endif
