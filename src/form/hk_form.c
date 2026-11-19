/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "form/hk_form.h"

#include "base/hk_map64.h"
#include "base/hk_utf8.h"

#include <string.h>

#define A(n) HK_ATOM_##n

static hk_str attr(const hk_document *d, hk_node_id el, hk_atom name, bool *found)
{
    const hk_attr *a = hk_dom_attr(d, el, HK_ATTR_NS_NONE, name);
    if (found) *found = a != NULL;
    return a ? a->value : (hk_str){"", 0};
}

static bool has_attr(const hk_document *d, hk_node_id el, hk_atom name)
{
    return hk_dom_attr(d, el, HK_ATTR_NS_NONE, name) != NULL;
}

static bool attr_is(const hk_document *d, hk_node_id el, hk_atom name, const char *value)
{
    return hk_str_eq_ascii_icase(hk_str_trim_ascii_ws(attr(d, el, name, NULL)), hk_str_from_cstr(value));
}

/* Next node in tree order within the subtree of root (root itself excluded). */
static hk_node_id next_in(const hk_document *d, hk_node_id n, hk_node_id root)
{
    const hk_node *x = hk_dom_node(d, n);
    if (x->first_child) return x->first_child;
    while (n != root) {
        x = hk_dom_node(d, n);
        if (x->next) return x->next;
        n = x->parent;
        if (!n) break;
    }
    return HK_NODE_NONE;
}

static hk_node_id element_by_id(const hk_document *d, hk_str id)
{
    if (!id.n) return HK_NODE_NONE;
    for (hk_node_id n = next_in(d, d->root, d->root); n; n = next_in(d, n, d->root)) {
        if (hk_dom_node(d, n)->type != HK_NODE_ELEMENT) continue;
        const hk_attr *a = hk_dom_attr(d, n, HK_ATTR_NS_NONE, A(id));
        if (a && hk_str_eq(a->value, id)) return n;
    }
    return HK_NODE_NONE;
}

static void text_content(const hk_document *d, hk_node_id el, hk_charbuf *out)
{
    for (hk_node_id n = next_in(d, el, el); n; n = next_in(d, n, el)) {
        if (hk_dom_node(d, n)->type != HK_NODE_TEXT) continue;
        hk_str t = hk_dom_text(d, n);
        hk_charbuf_append(out, t.p, t.n);
    }
}

/* --- kinds ---------------------------------------------------------------------------------------- */

hk_control_kind hk_control_kind_of(const hk_document *d, hk_node_id el)
{
    const hk_node *n = hk_dom_node(d, el);
    if (n->type != HK_NODE_ELEMENT || n->ns != HK_NS_HTML) return HK_CTRL_NONE;
    hk_atom name = hk_dom_element(d, el)->name;
    if (name == A(textarea)) return HK_CTRL_TEXTAREA;
    if (name == A(select)) return HK_CTRL_SELECT;
    if (name == A(button)) {
        if (attr_is(d, el, A(type), "reset")) return HK_CTRL_RESET;
        if (attr_is(d, el, A(type), "button")) return HK_CTRL_BUTTON;
        return HK_CTRL_SUBMIT;
    }
    if (name != A(input)) return HK_CTRL_NONE;
    static const struct {
        const char *type;
        hk_control_kind kind;
    } k_types[] = {
        {"password", HK_CTRL_PASSWORD}, {"checkbox", HK_CTRL_CHECKBOX}, {"radio", HK_CTRL_RADIO},
        {"submit", HK_CTRL_SUBMIT},     {"image", HK_CTRL_IMAGE},       {"reset", HK_CTRL_RESET},
        {"button", HK_CTRL_BUTTON},     {"file", HK_CTRL_FILE},         {"hidden", HK_CTRL_HIDDEN},
    };
    hk_str type = hk_str_trim_ascii_ws(attr(d, el, A(type), NULL));
    for (size_t i = 0; i < HK_ARRAY_LEN(k_types); i++)
        if (hk_str_eq_ascii_icase(type, hk_str_from_cstr(k_types[i].type))) return k_types[i].kind;
    return HK_CTRL_TEXT;
}

bool hk_control_is_text(hk_control_kind k)
{
    return k == HK_CTRL_TEXT || k == HK_CTRL_PASSWORD || k == HK_CTRL_TEXTAREA;
}

bool hk_control_disabled(const hk_document *d, hk_node_id el)
{
    if (has_attr(d, el, A(disabled))) return true;
    hk_node_id child = el;
    for (hk_node_id p = hk_dom_node(d, el)->parent; p; child = p, p = hk_dom_node(d, p)->parent) {
        if (!hk_dom_is_html(d, p, A(fieldset)) || !has_attr(d, p, A(disabled))) continue;
        /* Descendants of the fieldset's first legend child stay enabled. */
        hk_node_id legend = HK_NODE_NONE;
        for (hk_node_id c = hk_dom_node(d, p)->first_child; c; c = hk_dom_node(d, c)->next)
            if (hk_dom_is_html(d, c, A(legend))) {
                legend = c;
                break;
            }
        if (!legend || child != legend) return true;
    }
    return false;
}

bool hk_control_readonly(const hk_document *d, hk_node_id el)
{
    return has_attr(d, el, A(readonly)) || hk_control_disabled(d, el);
}

hk_str hk_control_button_label(const hk_document *d, hk_node_id el)
{
    bool found;
    hk_str v = attr(d, el, A(value), &found);
    if (found) return v;
    switch (hk_control_kind_of(d, el)) {
    case HK_CTRL_RESET: return HK_STR("リセット");
    case HK_CTRL_IMAGE: {
        hk_str alt = attr(d, el, A(alt), &found);
        return found && alt.n ? alt : HK_STR("送信");
    }
    case HK_CTRL_FILE: return HK_STR("ファイルを選択（未対応）");
    case HK_CTRL_BUTTON: return HK_STR("");
    default: return HK_STR("送信");
    }
}

hk_node_id hk_form_owner(const hk_document *d, hk_node_id el)
{
    bool found;
    hk_str id = attr(d, el, A(form), &found);
    if (found) {
        hk_node_id f = element_by_id(d, id);
        return f && hk_dom_is_html(d, f, A(form)) ? f : HK_NODE_NONE;
    }
    for (uint32_t i = 0; i < d->form_links.len; i++)
        if (d->form_links.data[i].element == el) return d->form_links.data[i].form;
    for (hk_node_id p = hk_dom_node(d, el)->parent; p; p = hk_dom_node(d, p)->parent)
        if (hk_dom_is_html(d, p, A(form))) return p;
    return HK_NODE_NONE;
}

static bool labelable(const hk_document *d, hk_node_id el)
{
    hk_control_kind k = hk_control_kind_of(d, el);
    return k != HK_CTRL_NONE && k != HK_CTRL_HIDDEN;
}

hk_node_id hk_label_control(const hk_document *d, hk_node_id label)
{
    bool found;
    hk_str id = attr(d, label, A(for), &found);
    if (found) {
        hk_node_id el = element_by_id(d, id);
        return el && labelable(d, el) ? el : HK_NODE_NONE;
    }
    for (hk_node_id n = next_in(d, label, label); n; n = next_in(d, n, label))
        if (labelable(d, n)) return n;
    return HK_NODE_NONE;
}

void hk_select_options(const hk_document *d, hk_node_id select, hk_node_vec *out)
{
    out->len = 0;
    for (hk_node_id c = hk_dom_node(d, select)->first_child; c; c = hk_dom_node(d, c)->next) {
        if (hk_dom_is_html(d, c, A(option))) {
            hk_vec_push(out, c);
        } else if (hk_dom_is_html(d, c, A(optgroup))) {
            for (hk_node_id o = hk_dom_node(d, c)->first_child; o; o = hk_dom_node(d, o)->next)
                if (hk_dom_is_html(d, o, A(option))) hk_vec_push(out, o);
        }
    }
}

/* Strips and collapses ASCII whitespace. */
static hk_str collapse_ws(hk_charbuf *buf, size_t from)
{
    size_t w = from;
    bool space = false;
    for (size_t r = from; r < buf->len; r++) {
        char c = buf->data[r];
        if (hk_ascii_is_ws(c)) {
            space = true;
            continue;
        }
        if (space && w > from) buf->data[w++] = ' ';
        space = false;
        buf->data[w++] = c;
    }
    buf->len = (uint32_t)w;
    return (hk_str){buf->data + from, w - from};
}

hk_str hk_option_label(const hk_document *d, hk_node_id option, hk_charbuf *buf)
{
    bool found;
    hk_str label = attr(d, option, A(label), &found);
    if (found && label.n) return label;
    buf->len = 0;
    text_content(d, option, buf);
    return collapse_ws(buf, 0);
}

static hk_str option_value(const hk_document *d, hk_node_id option, hk_charbuf *buf)
{
    bool found;
    hk_str v = attr(d, option, A(value), &found);
    if (found) return v;
    buf->len = 0;
    text_content(d, option, buf);
    return collapse_ws(buf, 0);
}

/* --- state ---------------------------------------------------------------------------------------- */

typedef struct ctl {
    hk_node_id el;
    hk_control_kind kind;
    bool checked;
    hk_charbuf value;
    uint32_t caret, anchor;
    HK_VEC(uint8_t) sel; /* <select>: selectedness per option */
} ctl;

struct hk_forms {
    const hk_document *d;
    HK_VEC(ctl *) ctls;
    hk_map64 index; /* element -> index into ctls + 1 */
    hk_node_vec controls; /* form-associated elements in tree order */
    uint32_t controls_nodes; /* d->nodes.len when controls was built */
    hk_node_vec scratch;
    hk_charbuf text;
};

hk_forms *hk_forms_create(const hk_document *d)
{
    hk_forms *f = hk_alloc_zero(sizeof *f);
    f->d = d;
    hk_map64_init(&f->index);
    return f;
}

void hk_forms_destroy(hk_forms *f)
{
    if (!f) return;
    for (uint32_t i = 0; i < f->ctls.len; i++) {
        hk_vec_free(&f->ctls.data[i]->value);
        hk_vec_free(&f->ctls.data[i]->sel);
        hk_free(f->ctls.data[i], sizeof(ctl));
    }
    hk_vec_free(&f->ctls);
    hk_map64_free(&f->index);
    hk_vec_free(&f->controls);
    hk_vec_free(&f->scratch);
    hk_vec_free(&f->text);
    hk_free(f, sizeof *f);
}

static const hk_node_vec *all_controls(hk_forms *f)
{
    const hk_document *d = f->d;
    if (f->controls_nodes == d->nodes.len) return &f->controls;
    f->controls.len = 0;
    for (hk_node_id n = next_in(d, d->root, d->root); n; n = next_in(d, n, d->root))
        if (hk_control_kind_of(d, n) != HK_CTRL_NONE) hk_vec_push(&f->controls, n);
    f->controls_nodes = d->nodes.len;
    return &f->controls;
}

static bool same_radio_group(hk_forms *f, hk_node_id a, hk_node_id b)
{
    const hk_document *d = f->d;
    if (hk_control_kind_of(d, b) != HK_CTRL_RADIO) return false;
    hk_str na = attr(d, a, A(name), NULL), nb = attr(d, b, A(name), NULL);
    return na.n && hk_str_eq(na, nb) && hk_form_owner(d, a) == hk_form_owner(d, b);
}

static void set_default(hk_forms *f, ctl *c)
{
    const hk_document *d = f->d;
    c->value.len = 0;
    c->sel.len = 0;
    c->checked = false;
    switch (c->kind) {
    case HK_CTRL_TEXT:
    case HK_CTRL_PASSWORD: {
        hk_str v = attr(d, c->el, A(value), NULL);
        for (size_t i = 0; i < v.n; i++)
            if (v.p[i] != '\r' && v.p[i] != '\n') hk_charbuf_push(&c->value, v.p[i]);
        break;
    }
    case HK_CTRL_TEXTAREA: text_content(d, c->el, &c->value); break;
    case HK_CTRL_CHECKBOX: c->checked = has_attr(d, c->el, A(checked)); break;
    case HK_CTRL_RADIO:
        c->checked = has_attr(d, c->el, A(checked));
        if (c->checked) {
            /* When several radios of a group are checked, the last one wins. */
            const hk_node_vec *all = all_controls(f);
            bool after = false;
            for (uint32_t i = 0; i < all->len; i++) {
                hk_node_id o = all->data[i];
                if (o == c->el) after = true;
                else if (after && same_radio_group(f, c->el, o) && has_attr(d, o, A(checked))) c->checked = false;
            }
        }
        break;
    case HK_CTRL_SELECT: {
        hk_select_options(d, c->el, &f->scratch);
        bool multiple = has_attr(d, c->el, A(multiple));
        int last = -1;
        for (uint32_t i = 0; i < f->scratch.len; i++) {
            uint8_t s = has_attr(d, f->scratch.data[i], A(selected));
            hk_vec_push(&c->sel, s);
            if (s) last = (int)i;
        }
        if (!multiple) {
            for (uint32_t i = 0; i < c->sel.len; i++) c->sel.data[i] = (int)i == last;
            if (last < 0) {
                for (uint32_t i = 0; i < f->scratch.len; i++)
                    if (!has_attr(d, f->scratch.data[i], A(disabled))) {
                        c->sel.data[i] = 1;
                        break;
                    }
            }
        }
        break;
    }
    default: break;
    }
    c->caret = c->anchor = c->value.len;
}

static ctl *get(hk_forms *f, hk_node_id el)
{
    uint64_t v;
    if (hk_map64_get(&f->index, el, &v)) return f->ctls.data[v - 1];
    ctl *c = hk_alloc_zero(sizeof *c);
    c->el = el;
    c->kind = hk_control_kind_of(f->d, el);
    set_default(f, c);
    hk_vec_push(&f->ctls, c);
    hk_map64_put(&f->index, el, f->ctls.len);
    return c;
}

hk_str hk_forms_value(hk_forms *f, hk_node_id el)
{
    ctl *c = get(f, el);
    return (hk_str){c->value.data ? c->value.data : "", c->value.len};
}

void hk_forms_set_value(hk_forms *f, hk_node_id el, hk_str value)
{
    ctl *c = get(f, el);
    c->value.len = 0;
    hk_charbuf_append(&c->value, value.p, value.n);
    c->caret = c->anchor = c->value.len;
}

bool hk_forms_checked(hk_forms *f, hk_node_id el)
{
    return get(f, el)->checked;
}

void hk_forms_set_checked(hk_forms *f, hk_node_id el, bool checked)
{
    ctl *c = get(f, el);
    c->checked = checked;
    if (c->kind != HK_CTRL_RADIO || !checked) return;
    const hk_node_vec *all = all_controls(f);
    for (uint32_t i = 0; i < all->len; i++) {
        hk_node_id o = all->data[i];
        if (o != el && same_radio_group(f, el, o)) get(f, o)->checked = false;
    }
}

int hk_forms_selected(hk_forms *f, hk_node_id select)
{
    ctl *c = get(f, select);
    for (uint32_t i = 0; i < c->sel.len; i++)
        if (c->sel.data[i]) return (int)i;
    return -1;
}

bool hk_forms_option_selected(hk_forms *f, hk_node_id select, uint32_t index)
{
    ctl *c = get(f, select);
    return index < c->sel.len && c->sel.data[index];
}

void hk_forms_select(hk_forms *f, hk_node_id select, uint32_t index)
{
    ctl *c = get(f, select);
    if (index >= c->sel.len) return;
    if (has_attr(f->d, select, A(multiple))) {
        c->sel.data[index] = !c->sel.data[index];
        return;
    }
    for (uint32_t i = 0; i < c->sel.len; i++) c->sel.data[i] = i == index;
}

void hk_forms_reset(hk_forms *f, hk_node_id form)
{
    for (uint32_t i = 0; i < f->ctls.len; i++) {
        ctl *c = f->ctls.data[i];
        if (hk_form_owner(f->d, c->el) == form) set_default(f, c);
    }
}

/* --- editing -------------------------------------------------------------------------------------- */

static uint32_t prev_pos(const hk_charbuf *v, uint32_t pos)
{
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && ((uint8_t)v->data[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static uint32_t next_pos(const hk_charbuf *v, uint32_t pos)
{
    if (pos >= v->len) return v->len;
    pos++;
    while (pos < v->len && ((uint8_t)v->data[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static uint32_t cp_at(const hk_charbuf *v, uint32_t pos)
{
    uint32_t cp;
    hk_utf8_decode((const uint8_t *)v->data + pos, v->len - pos, &cp);
    return cp;
}

/* Word classes for Ctrl+arrows: 0 = spaces and punctuation, 1 = Latin letters and digits, 2 = CJK. */
static int word_class(uint32_t cp)
{
    if (cp < 0x80) return ((cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '_') ? 1 : 0;
    if ((cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) ||
        (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) || (cp >= 0xFF5B && cp <= 0xFF65) ||
        cp == 0xA0)
        return 0;
    if ((cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF66 && cp <= 0xFF9F) ||
        cp >= 0x20000)
        return 2;
    return 1;
}

static uint32_t word_left(const hk_charbuf *v, uint32_t pos)
{
    while (pos > 0 && word_class(cp_at(v, prev_pos(v, pos))) == 0) pos = prev_pos(v, pos);
    if (pos == 0) return 0;
    int cls = word_class(cp_at(v, prev_pos(v, pos)));
    while (pos > 0 && word_class(cp_at(v, prev_pos(v, pos))) == cls) pos = prev_pos(v, pos);
    return pos;
}

static uint32_t word_right(const hk_charbuf *v, uint32_t pos)
{
    if (pos < v->len) {
        int cls = word_class(cp_at(v, pos));
        if (cls)
            while (pos < v->len && word_class(cp_at(v, pos)) == cls) pos = next_pos(v, pos);
    }
    while (pos < v->len && word_class(cp_at(v, pos)) == 0) pos = next_pos(v, pos);
    return pos;
}

static uint32_t line_start(const hk_charbuf *v, uint32_t pos)
{
    while (pos > 0 && v->data[pos - 1] != '\n') pos--;
    return pos;
}

static uint32_t line_end(const hk_charbuf *v, uint32_t pos)
{
    while (pos < v->len && v->data[pos] != '\n') pos++;
    return pos;
}

void hk_forms_selection(hk_forms *f, hk_node_id el, uint32_t *caret, uint32_t *anchor)
{
    ctl *c = get(f, el);
    *caret = c->caret;
    *anchor = c->anchor;
}

void hk_forms_set_caret(hk_forms *f, hk_node_id el, uint32_t pos, bool extend)
{
    ctl *c = get(f, el);
    if (pos > c->value.len) pos = c->value.len;
    while (pos > 0 && pos < c->value.len && ((uint8_t)c->value.data[pos] & 0xC0) == 0x80) pos--;
    c->caret = pos;
    if (!extend) c->anchor = pos;
}

static void delete_range(ctl *c, uint32_t a, uint32_t b)
{
    if (a > b) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    memmove(c->value.data + a, c->value.data + b, c->value.len - b);
    c->value.len -= b - a;
    c->caret = c->anchor = a;
}

bool hk_forms_edit(hk_forms *f, hk_node_id el, hk_edit_cmd cmd, bool extend)
{
    ctl *c = get(f, el);
    const hk_charbuf *v = &c->value;
    bool has_sel = c->caret != c->anchor;
    uint32_t lo = c->caret < c->anchor ? c->caret : c->anchor, hi = c->caret ^ c->anchor ^ lo;
    uint32_t to = c->caret;
    switch (cmd) {
    case HK_EDIT_LEFT: to = has_sel && !extend ? lo : prev_pos(v, c->caret); break;
    case HK_EDIT_RIGHT: to = has_sel && !extend ? hi : next_pos(v, c->caret); break;
    case HK_EDIT_WORD_LEFT: to = word_left(v, c->caret); break;
    case HK_EDIT_WORD_RIGHT: to = word_right(v, c->caret); break;
    case HK_EDIT_HOME: to = line_start(v, c->caret); break;
    case HK_EDIT_END: to = line_end(v, c->caret); break;
    case HK_EDIT_SELECT_ALL:
        c->anchor = 0;
        c->caret = v->len;
        return false;
    default: {
        if (hk_control_readonly(f->d, el)) return false;
        if (has_sel) {
            delete_range(c, lo, hi);
            return true;
        }
        uint32_t other = cmd == HK_EDIT_BACKSPACE        ? prev_pos(v, c->caret)
                         : cmd == HK_EDIT_DELETE         ? next_pos(v, c->caret)
                         : cmd == HK_EDIT_WORD_BACKSPACE ? word_left(v, c->caret)
                                                         : word_right(v, c->caret);
        if (other == c->caret) return false;
        delete_range(c, c->caret, other);
        return true;
    }
    }
    c->caret = to;
    if (!extend) c->anchor = to;
    return false;
}

static uint32_t utf16_units(hk_str s)
{
    uint32_t n = 0;
    for (size_t i = 0; i < s.n; i++) {
        uint8_t b = (uint8_t)s.p[i];
        if ((b & 0xC0) != 0x80) n++;
        if (b >= 0xF0) n++; /* outside the BMP: a surrogate pair */
    }
    return n;
}

bool hk_forms_insert(hk_forms *f, hk_node_id el, hk_str text)
{
    ctl *c = get(f, el);
    if (hk_control_readonly(f->d, el)) return false;
    bool multiline = c->kind == HK_CTRL_TEXTAREA;
    hk_charbuf clean = {0};
    for (size_t i = 0; i < text.n; i++) {
        char ch = text.p[i];
        if (ch == '\r') {
            if (multiline) hk_charbuf_push(&clean, '\n');
            if (i + 1 < text.n && text.p[i + 1] == '\n') i++;
        } else if (ch == '\n') {
            if (multiline) hk_charbuf_push(&clean, '\n');
        } else if ((uint8_t)ch >= 0x20 || ch == '\t') {
            hk_charbuf_push(&clean, ch);
        }
    }
    uint32_t lo = c->caret < c->anchor ? c->caret : c->anchor, hi = c->caret ^ c->anchor ^ lo;
    bool found;
    hk_str ml = attr(f->d, el, A(maxlength), &found);
    if (found) {
        uint32_t max = 0;
        size_t i = 0;
        ml = hk_str_trim_ascii_ws(ml);
        while (i < ml.n && ml.p[i] >= '0' && ml.p[i] <= '9' && max < 1000000) max = max * 10 + (uint32_t)(ml.p[i++] - '0');
        if (i == ml.n && ml.n) {
            uint32_t kept = utf16_units((hk_str){c->value.data, c->value.len}) -
                            utf16_units((hk_str){c->value.data + lo, hi - lo});
            /* Truncate the inserted text at a code point boundary so that the value fits. */
            uint32_t room = kept < max ? max - kept : 0, used = 0;
            size_t cut = 0;
            while (cut < clean.len) {
                uint8_t b = (uint8_t)clean.data[cut];
                uint32_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4, units = b >= 0xF0 ? 2 : 1;
                if (used + units > room || cut + len > clean.len) break;
                used += units;
                cut += len;
            }
            clean.len = (uint32_t)cut;
        }
    }
    if (!clean.len && lo == hi) {
        hk_vec_free(&clean);
        return false;
    }
    uint32_t new_len = c->value.len - (hi - lo) + clean.len;
    hk_vec_reserve(&c->value, new_len);
    memmove(c->value.data + lo + clean.len, c->value.data + hi, c->value.len - hi);
    if (clean.len) memcpy(c->value.data + lo, clean.data, clean.len);
    c->value.len = new_len;
    c->caret = c->anchor = lo + clean.len;
    hk_vec_free(&clean);
    return true;
}

hk_str hk_forms_selected_text(hk_forms *f, hk_node_id el)
{
    ctl *c = get(f, el);
    if (c->kind == HK_CTRL_PASSWORD) return (hk_str){"", 0};
    uint32_t lo = c->caret < c->anchor ? c->caret : c->anchor, hi = c->caret ^ c->anchor ^ lo;
    return (hk_str){c->value.data + lo, hi - lo};
}

/* --- submission ----------------------------------------------------------------------------------- */

void hk_urlencode_append(hk_charbuf *out, hk_str bytes)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < bytes.n; i++) {
        uint8_t b = (uint8_t)bytes.p[i];
        if (b == ' ') {
            hk_charbuf_push(out, '+');
        } else if ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '*' || b == '-' ||
                   b == '.' || b == '_') {
            hk_charbuf_push(out, (char)b);
        } else {
            char e[3] = {'%', hex[b >> 4], hex[b & 15]};
            hk_charbuf_append(out, e, 3);
        }
    }
}

typedef struct entries {
    hk_charbuf out;
    hk_encoding enc;
    hk_charbuf tmp, enc_buf;
} entries;

/* Appends one name or value: newlines normalized to CRLF, encoded, then serialized. */
static void put_part(entries *e, hk_str s)
{
    e->tmp.len = 0;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == '\r' || s.p[i] == '\n') {
            hk_charbuf_append(&e->tmp, "\r\n", 2);
            if (s.p[i] == '\r' && i + 1 < s.n && s.p[i + 1] == '\n') i++;
        } else {
            hk_charbuf_push(&e->tmp, s.p[i]);
        }
    }
    e->enc_buf.len = 0;
    hk_encode(e->enc, (hk_str){e->tmp.data ? e->tmp.data : "", e->tmp.len}, &e->enc_buf);
    hk_urlencode_append(&e->out, (hk_str){e->enc_buf.data ? e->enc_buf.data : "", e->enc_buf.len});
}

static void put_entry(entries *e, hk_str name, hk_str value)
{
    if (e->out.len) hk_charbuf_push(&e->out, '&');
    put_part(e, name);
    hk_charbuf_push(&e->out, '=');
    put_part(e, value);
}

static hk_encoding submission_encoding(const hk_document *d, hk_node_id form, hk_encoding doc_enc)
{
    bool found;
    hk_str v = attr(d, form, A(accept_charset), &found);
    if (found) {
        size_t i = 0;
        while (i < v.n) {
            while (i < v.n && hk_ascii_is_ws(v.p[i])) i++;
            size_t s = i;
            while (i < v.n && !hk_ascii_is_ws(v.p[i])) i++;
            hk_encoding enc = hk_encoding_from_label(hk_str_slice(v, s, i));
            if (enc != HK_ENC_UNKNOWN) return hk_encoding_for_output(enc);
        }
    }
    return hk_encoding_for_output(doc_enc);
}

bool hk_forms_submit(hk_forms *f, hk_node_id form, hk_node_id submitter, const hk_url *base, hk_encoding enc,
                     hk_form_submission *out)
{
    const hk_document *d = f->d;
    memset(out, 0, sizeof *out);
    if (!form) return false;
    bool sub_found;
    /* Method and action, possibly overridden by the submitter. */
    hk_str method = attr(d, form, A(method), NULL);
    if (submitter) {
        hk_str m = attr(d, submitter, A(formmethod), &sub_found);
        if (sub_found) method = m;
    }
    method = hk_str_trim_ascii_ws(method);
    if (hk_str_eq_ascii_icase(method, HK_STR("dialog"))) return false;
    out->post = hk_str_eq_ascii_icase(method, HK_STR("post"));
    hk_str action = attr(d, form, A(action), NULL);
    if (submitter) {
        hk_str a = attr(d, submitter, A(formaction), &sub_found);
        if (sub_found) action = a;
    }
    action = hk_str_trim_ascii_ws(action);
    hk_arena arena;
    hk_arena_init(&arena, 0);
    hk_url url = {0};
    bool ok = action.n ? hk_url_parse(&arena, action, base, &url) == HK_OK : base != NULL;
    if (ok && !action.n) url = *base;
    if (ok && !hk_str_eq(url.scheme, HK_STR("http")) && !hk_str_eq(url.scheme, HK_STR("https"))) ok = false;
    if (!ok) {
        hk_arena_free(&arena);
        return false;
    }

    entries e = {0};
    e.enc = submission_encoding(d, form, enc);
    const hk_node_vec *all = all_controls(f);
    hk_charbuf name_buf = {0};
    for (uint32_t i = 0; i < all->len; i++) {
        hk_node_id el = all->data[i];
        hk_control_kind k = hk_control_kind_of(d, el);
        if (hk_form_owner(d, el) != form || hk_control_disabled(d, el)) continue;
        if ((k == HK_CTRL_SUBMIT || k == HK_CTRL_IMAGE || k == HK_CTRL_RESET || k == HK_CTRL_BUTTON) && el != submitter)
            continue;
        if (hk_dom_is_html(d, el, A(button)) && k != HK_CTRL_SUBMIT) continue;
        bool has_name;
        hk_str name = attr(d, el, A(name), &has_name);
        if (k == HK_CTRL_IMAGE) {
            name_buf.len = 0;
            if (name.n) {
                hk_charbuf_append(&name_buf, name.p, name.n);
                hk_charbuf_push(&name_buf, '.');
            }
            uint32_t base_len = name_buf.len;
            hk_charbuf_push(&name_buf, 'x');
            put_entry(&e, (hk_str){name_buf.data, name_buf.len}, HK_STR("0"));
            name_buf.len = base_len;
            hk_charbuf_push(&name_buf, 'y');
            put_entry(&e, (hk_str){name_buf.data, name_buf.len}, HK_STR("0"));
            continue;
        }
        if (!has_name || !name.n) continue;
        switch (k) {
        case HK_CTRL_CHECKBOX:
        case HK_CTRL_RADIO: {
            if (!hk_forms_checked(f, el)) break;
            bool has_value;
            hk_str v = attr(d, el, A(value), &has_value);
            put_entry(&e, name, has_value ? v : HK_STR("on"));
            break;
        }
        case HK_CTRL_SELECT: {
            hk_select_options(d, el, &f->scratch);
            hk_node_vec opts = f->scratch;
            f->scratch = (hk_node_vec){0};
            for (uint32_t o = 0; o < opts.len; o++) {
                if (!hk_forms_option_selected(f, el, o) || has_attr(d, opts.data[o], A(disabled))) continue;
                put_entry(&e, name, option_value(d, opts.data[o], &f->text));
            }
            hk_vec_free(&f->scratch);
            f->scratch = opts;
            break;
        }
        case HK_CTRL_TEXT:
        case HK_CTRL_PASSWORD:
        case HK_CTRL_TEXTAREA: put_entry(&e, name, hk_forms_value(f, el)); break;
        case HK_CTRL_HIDDEN:
            if (hk_str_eq_ascii_icase(name, HK_STR("_charset_"))) put_entry(&e, name, hk_str_from_cstr(hk_encoding_name(e.enc)));
            else put_entry(&e, name, attr(d, el, A(value), NULL));
            break;
        case HK_CTRL_FILE: put_entry(&e, name, HK_STR("")); break;
        case HK_CTRL_SUBMIT: put_entry(&e, name, attr(d, el, A(value), NULL)); break;
        default: break;
        }
    }
    hk_vec_free(&name_buf);
    hk_vec_free(&e.tmp);
    hk_vec_free(&e.enc_buf);

    /* The URL without its query and fragment. */
    size_t cut = url.href.n;
    size_t q = hk_str_find_char(url.href, '?'), h = hk_str_find_char(url.href, '#');
    if (h != HK_STR_NPOS && h < cut) cut = h;
    if (!out->post && q != HK_STR_NPOS && q < cut) cut = q;
    hk_charbuf_append(&out->url, url.href.p, cut);
    if (out->post) {
        out->body = e.out;
    } else {
        hk_charbuf_push(&out->url, '?');
        hk_charbuf_append(&out->url, e.out.data ? e.out.data : "", e.out.len);
        if (url.has_fragment) {
            hk_charbuf_push(&out->url, '#');
            hk_charbuf_append(&out->url, url.fragment.p, url.fragment.n);
        }
        hk_vec_free(&e.out);
    }
    hk_arena_free(&arena);
    return true;
}

void hk_form_submission_free(hk_form_submission *s)
{
    hk_vec_free(&s->url);
    hk_vec_free(&s->body);
}
