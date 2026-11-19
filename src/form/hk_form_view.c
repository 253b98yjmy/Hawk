/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "form/hk_form_view.h"

#include "base/hk_utf8.h"

#include <string.h>

#define ACCENT 0xFF0075FFu
#define FRAME 0xFF767676u
#define SELECTION 0xFFB4D5FEu
#define PLACEHOLDER 0xFF757575u
#define BIG 1e7f

typedef HK_VEC(uint16_t) u16_vec;
typedef HK_VEC(uint32_t) u32_vec;

struct hk_form_view {
    HK_VEC(hk_glyph_run) runs;
    HK_VEC(hk_text_rect) under, over;
    hk_node_id scroll_el; /* the control the scroll offsets belong to */
    float scroll_x, scroll_y;
    u16_vec text;
    u32_vec map; /* UTF-16 index -> byte offset into the value (text.len + 1 entries) */
};

hk_form_view *hk_form_view_create(void)
{
    return hk_alloc_zero(sizeof(hk_form_view));
}

static void free_runs(hk_form_view *v)
{
    for (uint32_t i = 0; i < v->runs.len; i++) {
        hk_glyph_run *r = &v->runs.data[i];
        hk_font_face_release(r->face);
        hk_free(r->glyphs, r->count * sizeof(uint16_t));
        hk_free(r->advances, r->count * sizeof(float));
        hk_free(r->offsets, r->count * 2 * sizeof(float));
    }
    v->runs.len = 0;
}

void hk_form_view_destroy(hk_form_view *v)
{
    if (!v) return;
    free_runs(v);
    hk_vec_free(&v->runs);
    hk_vec_free(&v->under);
    hk_vec_free(&v->over);
    hk_vec_free(&v->text);
    hk_vec_free(&v->map);
    hk_free(v, sizeof *v);
}

static void push_u16(hk_form_view *v, uint32_t cp, uint32_t byte)
{
    if (cp >= 0x10000) {
        cp -= 0x10000;
        hk_vec_push(&v->text, (uint16_t)(0xD800 | (cp >> 10)));
        hk_vec_push(&v->map, byte);
        hk_vec_push(&v->text, (uint16_t)(0xDC00 | (cp & 0x3FF)));
    } else {
        hk_vec_push(&v->text, (uint16_t)cp);
    }
    hk_vec_push(&v->map, byte);
}

/* Appends value[from, to) to the display text (masked for passwords). */
static void push_value(hk_form_view *v, hk_str value, uint32_t from, uint32_t to, bool mask)
{
    uint32_t i = from;
    while (i < to) {
        uint32_t cp;
        uint32_t n = (uint32_t)hk_utf8_decode((const uint8_t *)value.p + i, to - i, &cp);
        push_u16(v, mask ? 0x2022 : cp, i);
        i += n;
    }
}

typedef struct display {
    bool placeholder;
    uint32_t sel_lo, sel_hi; /* UTF-16 */
    uint32_t caret;
    uint32_t comp_start, comp_len;
} display;

/* Builds the text a text control shows: its value (with the composition at the caret) or its placeholder. */
static void build_display(hk_form_view *v, const hk_form_ctx *c, hk_node_id el, hk_control_kind k, display *d)
{
    memset(d, 0, sizeof *d);
    v->text.len = v->map.len = 0;
    hk_str value = hk_forms_value(c->forms, el);
    bool focused = c->focus == el;
    uint32_t caret, anchor;
    hk_forms_selection(c->forms, el, &caret, &anchor);
    uint32_t lo = caret < anchor ? caret : anchor, hi = caret ^ anchor ^ lo;
    bool mask = k == HK_CTRL_PASSWORD;
    bool composing = focused && c->comp_len > 0;
    if (!value.n && !composing) {
        const hk_attr *ph = hk_dom_attr(c->doc, el, HK_ATTR_NS_NONE, HK_ATOM_placeholder);
        if (ph && ph->value.n) {
            push_value(v, ph->value, 0, (uint32_t)ph->value.n, false);
            for (uint32_t i = 0; i < v->map.len; i++) v->map.data[i] = 0;
            hk_vec_push(&v->map, 0);
            d->placeholder = true;
            return;
        }
    }
    if (composing) {
        push_value(v, value, 0, lo, mask);
        d->comp_start = v->text.len;
        for (uint32_t i = 0; i < c->comp_len; i++) {
            hk_vec_push(&v->text, mask ? (uint16_t)0x2022 : c->comp[i]);
            hk_vec_push(&v->map, lo);
        }
        d->comp_len = c->comp_len;
        d->caret = d->comp_start + (c->comp_caret < c->comp_len ? c->comp_caret : c->comp_len);
        push_value(v, value, hi, (uint32_t)value.n, mask);
        hk_vec_push(&v->map, (uint32_t)value.n);
        return;
    }
    push_value(v, value, 0, (uint32_t)value.n, mask);
    hk_vec_push(&v->map, (uint32_t)value.n);
    /* Byte offsets to UTF-16 indices. */
    for (uint32_t i = 0; i < v->map.len; i++) {
        uint32_t b = v->map.data[i];
        if (i > 0 && v->map.data[i - 1] == b) continue; /* the low half of a surrogate pair */
        if (b == lo) d->sel_lo = i;
        if (b == hi) d->sel_hi = i;
        if (b == caret) d->caret = i;
    }
}

static hk_text_layout *make_layout(const hk_form_ctx *c, const hk_style *s, const uint16_t *text, uint32_t n,
                                   float max_width, bool wrap, uint32_t color, hk_str family)
{
    hk_text_style ts = {0};
    ts.family = family.n ? family : hk_styler_family(c->st, s->font_family);
    ts.size = s->font_size;
    ts.weight = s->font_weight;
    ts.italic = s->italic != 0;
    ts.color = color;
    hk_text_para para = {0};
    para.max_width = max_width > 1 ? max_width : 1;
    para.line_height = s->line_height;
    para.wrap = wrap;
    static const uint16_t k_empty[] = {0x200B}; /* a zero-width space keeps the line height of empty text */
    if (!n) {
        text = k_empty;
        n = 1;
    }
    return hk_text_layout_create(text, n, &para, &ts);
}

typedef struct emit_ctx {
    hk_form_view *v;
    float dx, dy;
} emit_ctx;

static void emit_run(void *user, const hk_glyph_run *r)
{
    emit_ctx *e = user;
    hk_glyph_run g = *r;
    g.x += e->dx;
    g.baseline += e->dy;
    hk_vec_push(&e->v->runs, g);
}

static void emit_rect(void *user, const hk_text_rect *r)
{
    emit_ctx *e = user;
    hk_text_rect t = *r;
    t.x += e->dx;
    t.y += e->dy;
    hk_vec_push(&e->v->over, t);
}

static void push_rect(hk_form_view *v, bool over, float x, float y, float w, float h, uint32_t color, float radius)
{
    hk_text_rect r = {x, y, w, h, color, 0, radius};
    if (over) hk_vec_push(&v->over, r);
    else hk_vec_push(&v->under, r);
}

/* Draws a short string at an offset; returns its laid out size through w and h (may be NULL). */
static void draw_label(hk_form_view *v, const hk_form_ctx *c, const hk_style *s, const uint16_t *text, uint32_t n,
                       uint32_t color, hk_str family, float size, float x, float y, int align_x, int align_y,
                       float box_w, float box_h)
{
    hk_style tmp = *s;
    if (size > 0) {
        tmp.font_size = size;
        tmp.line_height = 0;
    }
    hk_text_layout *l = make_layout(c, &tmp, text, n, BIG, false, color, family);
    if (!l) return;
    float w, h;
    hk_text_layout_size(l, &w, &h);
    float min_w, max_w;
    hk_text_layout_intrinsic(l, &min_w, &max_w);
    w = max_w;
    emit_ctx e = {v, x + (align_x == 0 ? 0 : align_x == 1 ? (box_w - w) / 2 : box_w - w),
                  y + (align_y == 0 ? 0 : (box_h - h) / 2)};
    hk_text_sink sink = {&e, emit_run, emit_rect, NULL};
    hk_text_layout_emit(l, &sink);
    hk_text_layout_destroy(l);
}

/* Lays out a text control and keeps the caret of the focused one in view. */
typedef struct text_box {
    hk_text_layout *l;
    display d;
    float dx, dy; /* text origin relative to the content box */
} text_box;

static bool layout_text(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, hk_control_kind k, text_box *t)
{
    const hk_style *s = hk_style_of(c->st, it->node);
    if (!s) return false;
    build_display(v, c, it->node, k, &t->d);
    bool multiline = k == HK_CTRL_TEXTAREA;
    uint32_t color = t->d.placeholder ? PLACEHOLDER : s->color;
    t->l = make_layout(c, s, v->text.data, v->text.len, multiline ? it->w : BIG, multiline, color, (hk_str){0});
    if (!t->l) return false;
    float w, h;
    hk_text_layout_size(t->l, &w, &h);
    float sx = 0, sy = 0;
    if (c->focus == it->node) {
        if (v->scroll_el != it->node) {
            v->scroll_el = it->node;
            v->scroll_x = v->scroll_y = 0;
        }
        float cx, cy, ch;
        hk_text_layout_caret(t->l, t->d.caret, &cx, &cy, &ch);
        if (!multiline) {
            float max_scroll = w + 1 - it->w;
            if (cx - v->scroll_x > it->w - 1) v->scroll_x = cx - it->w + 1;
            if (cx < v->scroll_x) v->scroll_x = cx;
            if (v->scroll_x > max_scroll) v->scroll_x = max_scroll;
            if (v->scroll_x < 0) v->scroll_x = 0;
        } else {
            if (cy + ch - v->scroll_y > it->h) v->scroll_y = cy + ch - it->h;
            if (cy < v->scroll_y) v->scroll_y = cy;
            if (v->scroll_y < 0) v->scroll_y = 0;
        }
        sx = v->scroll_x;
        sy = v->scroll_y;
    }
    t->dx = -sx;
    t->dy = multiline ? -sy : (it->h - h) / 2;
    return true;
}

typedef struct rect_ctx {
    hk_form_view *v;
    float dx, dy;
    bool over;
    uint32_t color;
    bool underline;
} rect_ctx;

static void on_range_rect(void *user, const hk_text_rect *r)
{
    rect_ctx *rc = user;
    if (rc->underline) push_rect(rc->v, true, r->x + rc->dx, r->y + rc->dy + r->h - 2, r->w, 1, rc->color, 0);
    else push_rect(rc->v, rc->over, r->x + rc->dx, r->y + rc->dy, r->w, r->h, rc->color, 0);
}

static void visual_text(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, hk_control_kind k)
{
    text_box t;
    if (!layout_text(v, c, it, k, &t)) return;
    const hk_style *s = hk_style_of(c->st, it->node);
    bool focused = c->focus == it->node;
    if (focused && t.d.sel_hi > t.d.sel_lo && !t.d.placeholder) {
        rect_ctx rc = {v, t.dx, t.dy, false, SELECTION, false};
        hk_text_layout_range_rects(t.l, t.d.sel_lo, t.d.sel_hi - t.d.sel_lo, on_range_rect, &rc);
    }
    emit_ctx e = {v, t.dx, t.dy};
    hk_text_sink sink = {&e, emit_run, emit_rect, NULL};
    hk_text_layout_emit(t.l, &sink);
    if (focused && t.d.comp_len) {
        rect_ctx rc = {v, t.dx, t.dy, true, s->color, true};
        hk_text_layout_range_rects(t.l, t.d.comp_start, t.d.comp_len, on_range_rect, &rc);
    }
    if (focused && c->show_caret) {
        float cx, cy, ch;
        hk_text_layout_caret(t.l, t.d.placeholder ? 0 : t.d.caret, &cx, &cy, &ch);
        push_rect(v, true, cx + t.dx, cy + t.dy, 1, ch, s->color, 0);
    }
    hk_text_layout_destroy(t.l);
}

static void visual_check(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, hk_control_kind k)
{
    const hk_style *s = hk_style_of(c->st, it->node);
    bool on = hk_forms_checked(c->forms, it->node);
    bool disabled = hk_control_disabled(c->doc, it->node);
    uint32_t edge = disabled ? 0xFFC0C0C0u : on ? ACCENT : FRAME;
    float size = it->w < it->h ? it->w : it->h;
    float x = (it->w - size) / 2, y = (it->h - size) / 2;
    if (k == HK_CTRL_RADIO) {
        push_rect(v, false, x, y, size, size, edge, size / 2);
        push_rect(v, false, x + 1, y + 1, size - 2, size - 2, 0xFFFFFFFFu, size / 2 - 1);
        if (on) push_rect(v, false, x + size * 0.25f, y + size * 0.25f, size * 0.5f, size * 0.5f, edge, size * 0.25f);
    } else {
        push_rect(v, false, x, y, size, size, edge, 2);
        push_rect(v, false, x + 1, y + 1, size - 2, size - 2, on ? edge : 0xFFFFFFFFu, 1.5f);
        if (on) {
            static const uint16_t k_check[] = {0x2713};
            draw_label(v, c, s, k_check, 1, 0xFFFFFFFFu, HK_STR("Segoe UI Symbol"), size * 0.85f, x, y, 1, 1, size,
                       size);
        }
    }
    if (c->focus == it->node) {
        push_rect(v, true, x - 1, y + size, size + 2, 1, edge, 0); /* a small focus mark */
    }
}

static void visual_label(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, hk_str label, int align)
{
    const hk_style *s = hk_style_of(c->st, it->node);
    if (!s || !label.n) return;
    hk_arena a;
    hk_arena_init(&a, 0);
    size_t n;
    uint16_t *w = hk_utf8_to_utf16(&a, label, &n);
    draw_label(v, c, s, w, (uint32_t)n, s->color, (hk_str){0}, 0, 0, 0, align, 1, it->w, it->h);
    hk_arena_free(&a);
}

static void visual_select(hk_form_view *v, const hk_form_ctx *c, const hk_item *it)
{
    const hk_style *s = hk_style_of(c->st, it->node);
    if (!s) return;
    int sel = hk_forms_selected(c->forms, it->node);
    hk_node_vec opts = {0};
    hk_charbuf buf = {0};
    hk_select_options(c->doc, it->node, &opts);
    if (sel >= 0 && (uint32_t)sel < opts.len) {
        hk_item clipped = *it;
        clipped.w -= s->font_size * 1.2f;
        visual_label(v, c, &clipped, hk_option_label(c->doc, opts.data[sel], &buf), 0);
    }
    hk_vec_free(&opts);
    hk_vec_free(&buf);
    static const uint16_t k_arrow[] = {0x25BE};
    draw_label(v, c, s, k_arrow, 1, s->color, HK_STR("Segoe UI Symbol"), s->font_size, 0, 0, 2, 1, it->w, it->h);
    if (c->focus == it->node) push_rect(v, true, 0, it->h - 1, it->w, 1, ACCENT, 0);
}

bool hk_form_view_visual(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, hk_control_visual *out)
{
    free_runs(v);
    v->under.len = v->over.len = 0;
    hk_control_kind k = hk_control_kind_of(c->doc, it->node);
    switch (k) {
    case HK_CTRL_TEXT:
    case HK_CTRL_PASSWORD:
    case HK_CTRL_TEXTAREA: visual_text(v, c, it, k); break;
    case HK_CTRL_CHECKBOX:
    case HK_CTRL_RADIO: visual_check(v, c, it, k); break;
    case HK_CTRL_SELECT: visual_select(v, c, it); break;
    case HK_CTRL_SUBMIT:
    case HK_CTRL_IMAGE:
    case HK_CTRL_RESET:
    case HK_CTRL_BUTTON:
    case HK_CTRL_FILE:
        visual_label(v, c, it, hk_control_button_label(c->doc, it->node), 1);
        if (c->focus == it->node) push_rect(v, true, 0, it->h - 1, it->w, 1, ACCENT, 0);
        break;
    default: return false;
    }
    out->under = v->under.data;
    out->nunder = v->under.len;
    out->runs = v->runs.data;
    out->nruns = v->runs.len;
    out->over = v->over.data;
    out->nover = v->over.len;
    return true;
}

uint32_t hk_form_view_hit(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, float x, float y)
{
    hk_control_kind k = hk_control_kind_of(c->doc, it->node);
    if (!hk_control_is_text(k)) return 0;
    hk_form_ctx plain = *c;
    plain.comp_len = 0;
    text_box t;
    if (!layout_text(v, &plain, it, k, &t)) return 0;
    uint32_t pos = hk_text_layout_hit_test(t.l, x - t.dx, y - t.dy);
    hk_text_layout_destroy(t.l);
    if (t.d.placeholder) return 0;
    return pos < v->map.len ? v->map.data[pos] : (v->map.len ? v->map.data[v->map.len - 1] : 0);
}

void hk_form_view_caret(hk_form_view *v, const hk_form_ctx *c, const hk_item *it, float *x, float *y, float *h)
{
    *x = *y = 0;
    *h = it->h;
    hk_control_kind k = hk_control_kind_of(c->doc, it->node);
    if (!hk_control_is_text(k)) return;
    text_box t;
    if (!layout_text(v, c, it, k, &t)) return;
    hk_text_layout_caret(t.l, t.d.placeholder ? 0 : t.d.caret, x, y, h);
    *x += t.dx;
    *y += t.dy;
    hk_text_layout_destroy(t.l);
}
