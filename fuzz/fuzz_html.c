/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HTML parser on arbitrary input. The first byte chooses a chunk size so that
 * resumption at every possible input boundary is exercised; the tree built from
 * chunks must have the same number of nodes as the one built in a single pass.
 */
#include "dom/hk_dom.h"
#include "html/hk_html_parser.h"

static uint32_t parse_chunked(const uint8_t *data, size_t size, size_t chunk)
{
    hk_document *doc = hk_document_create();
    hk_html_parser *p = hk_html_parser_create(doc);
    for (size_t i = 0; i < size; i += chunk) {
        size_t n = size - i < chunk ? size - i : chunk;
        hk_html_parser_feed(p, (const char *)data + i, n);
    }
    hk_html_parser_finish(p);
    hk_html_parser_destroy(p);
    uint32_t nodes = doc->nodes.len;
    hk_document_destroy(doc);
    return nodes;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool initialized;
    if (!initialized) {
        hk_names_init();
        initialized = true;
    }
    if (size == 0) return 0;
    size_t chunk = (size_t)data[0] % 16 + 1;
    data++;
    size--;
    uint32_t whole = parse_chunked(data, size, size ? size : 1);
    uint32_t pieces = parse_chunked(data, size, chunk);
    HK_ASSERT(whole == pieces);
    return 0;
}
