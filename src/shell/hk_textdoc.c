/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "shell/hk_textdoc.h"
#include "base/hk_vec.h"

#include <string.h>

#define A(x) HK_ATOM_##x

typedef struct builder {
    hk_document *d;
    hk_textdoc_emit_fn emit;
    void *user;
    hk_charbuf text;
    hk_tv_kind kind;
    int pre_depth;
    bool pending_space;
} builder;

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void flush(builder *b)
{
    /* Trim trailing spaces. */
    while (b->text.len && b->text.data[b->text.len - 1] == ' ') b->text.len--;
    if (b->text.len) b->emit(b->user, b->kind, (hk_str){b->text.data, b->text.len});
    b->text.len = 0;
    b->pending_space = false;
    b->kind = HK_TV_BODY;
}

static void add_text(builder *b, hk_str s)
{
    if (b->pre_depth) {
        hk_charbuf_append(&b->text, s.p, s.n);
        return;
    }
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (is_ws(c)) {
            if (b->text.len) b->pending_space = true;
            continue;
        }
        if (b->pending_space) {
            hk_charbuf_push(&b->text, ' ');
            b->pending_space = false;
        }
        hk_charbuf_push(&b->text, c);
    }
}

static bool is_block(hk_atom n)
{
    static const hk_atom k[] = {
        A(address), A(article), A(aside), A(blockquote), A(body), A(caption), A(center), A(dd),
        A(details), A(dialog), A(dir), A(div), A(dl), A(dt), A(fieldset), A(figcaption), A(figure),
        A(footer), A(form), A(h1), A(h2), A(h3), A(h4), A(h5), A(h6), A(header), A(hgroup), A(hr),
        A(li), A(listing), A(main), A(menu), A(nav), A(ol), A(p), A(plaintext), A(pre), A(search),
        A(section), A(summary), A(table), A(tr), A(ul), A(xmp),
    };
    for (size_t i = 0; i < HK_ARRAY_LEN(k); i++) {
        if (k[i] == n) return true;
    }
    return false;
}

static bool is_hidden(hk_atom n)
{
    return n == A(head) || n == A(script) || n == A(style) || n == A(template) || n == A(title) ||
           n == A(noembed) || n == A(noframes) || n == A(select);
}

static void walk(builder *b, hk_node_id id)
{
    hk_document *d = b->d;
    const hk_node *n = hk_dom_node(d, id);
    if (n->type == HK_NODE_TEXT) {
        add_text(b, hk_dom_text(d, id));
        return;
    }
    if (n->type != HK_NODE_ELEMENT && n->type != HK_NODE_DOCUMENT) return;
    if (n->type == HK_NODE_ELEMENT) {
        if (n->ns != HK_NS_HTML) return; /* SVG and MathML are not rendered in M1 */
        hk_atom name = hk_dom_element(d, id)->name;
        if (is_hidden(name)) return;
        if (hk_dom_attr(d, id, HK_ATTR_NS_NONE, A(hidden))) return;
        if (name == A(br)) {
            if (b->pre_depth) hk_charbuf_push(&b->text, '\n');
            else flush(b);
            return;
        }
        if (name == A(img)) {
            const hk_attr *alt = hk_dom_attr(d, id, HK_ATTR_NS_NONE, A(alt));
            if (alt && hk_str_trim_ascii_ws(alt->value).n) {
                add_text(b, HK_STR(" ["));
                add_text(b, alt->value);
                add_text(b, HK_STR("] "));
            }
            return;
        }
        if (name == A(iframe)) {
            const hk_attr *src = hk_dom_attr(d, id, HK_ATTR_NS_NONE, A(src));
            flush(b);
            b->kind = HK_TV_NOTE;
            add_text(b, HK_STR("[embedded content: "));
            add_text(b, src ? src->value : HK_STR("(none)"));
            add_text(b, HK_STR("]"));
            flush(b);
            return;
        }
        if (name == A(td) || name == A(th)) add_text(b, HK_STR("  "));
        bool block = is_block(name);
        if (block) {
            flush(b);
            if (name == A(h1)) b->kind = HK_TV_H1;
            else if (name == A(h2)) b->kind = HK_TV_H2;
            else if (name == A(h3) || name == A(h4) || name == A(h5) || name == A(h6)) b->kind = HK_TV_H3;
            else if (name == A(pre) || name == A(listing) || name == A(xmp) || name == A(plaintext)) b->kind = HK_TV_PRE;
            if (name == A(li)) hk_charbuf_append(&b->text, "\xE3\x83\xBB ", 4); /* "・ " */
        }
        bool pre = name == A(pre) || name == A(listing) || name == A(xmp) || name == A(plaintext) ||
                   name == A(textarea);
        if (pre) b->pre_depth++;
        for (hk_node_id c = n->first_child; c; c = hk_dom_node(d, c)->next) walk(b, c);
        if (pre) b->pre_depth--;
        if (block) flush(b);
        return;
    }
    for (hk_node_id c = n->first_child; c; c = hk_dom_node(d, c)->next) walk(b, c);
}

static hk_node_id find_element(hk_document *d, hk_node_id from, hk_atom name)
{
    for (hk_node_id c = hk_dom_node(d, from)->first_child; c; c = hk_dom_node(d, c)->next) {
        if (hk_dom_is_html(d, c, name)) return c;
        if (hk_dom_node(d, c)->type == HK_NODE_ELEMENT) {
            hk_node_id r = find_element(d, c, name);
            if (r) return r;
        }
    }
    return HK_NODE_NONE;
}

void hk_textdoc_build(hk_document *d, hk_textdoc_emit_fn emit, void *user, hk_str *title)
{
    *title = HK_STR("");
    hk_node_id t = find_element(d, d->root, A(title));
    if (t) {
        hk_charbuf tb = {0};
        for (hk_node_id c = hk_dom_node(d, t)->first_child; c; c = hk_dom_node(d, c)->next) {
            if (hk_dom_node(d, c)->type == HK_NODE_TEXT) {
                hk_str s = hk_dom_text(d, c);
                hk_charbuf_append(&tb, s.p, s.n);
            }
        }
        /* Collapse whitespace. */
        hk_charbuf clean = {0};
        bool space = false;
        for (uint32_t i = 0; i < tb.len; i++) {
            if (is_ws(tb.data[i])) {
                space = clean.len > 0;
                continue;
            }
            if (space) hk_charbuf_push(&clean, ' ');
            space = false;
            hk_charbuf_push(&clean, tb.data[i]);
        }
        *title = hk_arena_str_dup(&d->arena, (hk_str){clean.data, clean.len});
        hk_vec_free(&tb);
        hk_vec_free(&clean);
    }

    builder b = {d, emit, user, {0}, HK_TV_BODY, 0, false};
    walk(&b, d->root);
    flush(&b);
    hk_vec_free(&b.text);
}
