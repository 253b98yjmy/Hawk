/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HTML tree construction (WHATWG HTML 13.2.6) with the scripting flag disabled,
 * so <noscript> content is parsed as markup. Fragment parsing is not needed by
 * Hawk and is not implemented.
 */
#ifndef HK_HTML_PARSER_H
#define HK_HTML_PARSER_H

#include "dom/hk_dom.h"

typedef struct hk_html_parser hk_html_parser;

/* The parser fills doc, which must be freshly created. */
hk_html_parser *hk_html_parser_create(hk_document *doc);
void hk_html_parser_destroy(hk_html_parser *p);

/* Input is UTF-8 (already decoded from the document's encoding). */
void hk_html_parser_feed(hk_html_parser *p, const char *utf8, size_t n);
void hk_html_parser_finish(hk_html_parser *p);

/* Convenience: parses a complete document. */
hk_document *hk_html_parse(const char *utf8, size_t n);

#endif
