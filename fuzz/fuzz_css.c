/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CSS tokenizer, parser and cascade on arbitrary stylesheets applied to a small document. */
#include "dom/hk_dom.h"
#include "html/hk_html_parser.h"
#include "style/hk_style.h"

static const char k_html[] =
    "<!doctype html><html><body><div id=a class='x y'><p class=x>one <em>two</em></p><ul><li>a<li>b</ul>"
    "<span style='color: red; margin: 1em'>s</span></div></body></html>";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool initialized;
    if (!initialized) {
        hk_names_init();
        initialized = true;
    }
    hk_document *doc = hk_html_parse(k_html, sizeof k_html - 1);
    hk_styler *st = hk_styler_create(doc);
    hk_styler_add_sheet(st, (hk_str){(const char *)data, size});
    hk_css_media_env env = {1000, 800, false};
    hk_styler_compute(st, &env);
    const hk_css_decl *decls;
    uint32_t n;
    hk_css_parse_declarations(&doc->arena, (hk_str){(const char *)data, size}, &decls, &n);
    hk_styler_destroy(st);
    hk_document_destroy(doc);
    return 0;
}
