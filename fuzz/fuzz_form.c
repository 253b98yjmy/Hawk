/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Forms on arbitrary HTML: every control is initialized, edited with the bytes of the input as
 * commands and text, and every form is submitted with each encoding class (UTF-8, a legacy code
 * page, a multi-byte one).
 */
#include "dom/hk_dom.h"
#include "form/hk_form.h"
#include "html/hk_html_parser.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool initialized;
    if (!initialized) {
        hk_names_init();
        initialized = true;
    }
    if (size < 2) return 0;
    /* The first half is the document, the second the edit script. */
    size_t split = size / 2;
    hk_document *d = hk_html_parse((const char *)data, split);
    const uint8_t *script = data + split;
    size_t script_n = size - split;
    hk_forms *f = hk_forms_create(d);
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_url base = {0};
    hk_url_parse(&a, HK_STR("https://example.com/a/b?c#d"), NULL, &base);
    size_t si = 0;
    for (hk_node_id n = 1; n < d->nodes.len; n++) {
        hk_control_kind k = hk_control_kind_of(d, n);
        if (k == HK_CTRL_NONE) continue;
        hk_form_owner(d, n);
        hk_control_disabled(d, n);
        if (hk_dom_is_html(d, n, HK_ATOM_label)) hk_label_control(d, n);
        if (hk_control_is_text(k)) {
            for (int step = 0; step < 8 && si < script_n; step++) {
                uint8_t op = script[si++];
                if (op < 0x40) {
                    hk_forms_edit(f, n, (hk_edit_cmd)(op % (HK_EDIT_SELECT_ALL + 1)), (op & 0x20) != 0);
                } else if (op < 0x50) {
                    hk_forms_set_caret(f, n, si < script_n ? script[si++] : 0, (op & 1) != 0);
                } else {
                    size_t len = (size_t)(op & 7);
                    if (len > script_n - si) len = script_n - si;
                    hk_forms_insert(f, n, (hk_str){(const char *)script + si, len});
                    si += len;
                }
                uint32_t caret, anchor;
                hk_forms_selection(f, n, &caret, &anchor);
                hk_str v = hk_forms_value(f, n);
                HK_ASSERT(caret <= v.n && anchor <= v.n);
                hk_forms_selected_text(f, n);
            }
        } else if (k == HK_CTRL_CHECKBOX || k == HK_CTRL_RADIO) {
            hk_forms_set_checked(f, n, si < script_n && (script[si++] & 1));
        } else if (k == HK_CTRL_SELECT) {
            hk_forms_select(f, n, si < script_n ? script[si++] % 8 : 0);
            hk_forms_selected(f, n);
        }
    }
    static const hk_encoding encs[] = {HK_ENC_UTF8, HK_ENC_WINDOWS_1252, HK_ENC_SHIFT_JIS};
    for (hk_node_id n = 1; n < d->nodes.len; n++) {
        if (!hk_dom_is_html(d, n, HK_ATOM_form)) continue;
        for (size_t e = 0; e < HK_ARRAY_LEN(encs); e++) {
            hk_form_submission s;
            if (hk_forms_submit(f, n, HK_NODE_NONE, &base, encs[e], &s)) hk_form_submission_free(&s);
        }
        hk_forms_reset(f, n);
    }
    hk_arena_free(&a);
    hk_forms_destroy(f);
    hk_document_destroy(d);
    return 0;
}
