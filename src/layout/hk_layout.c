/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Layout in one recursive pass that emits the display list directly. Boxes whose
 * position is only known later (inline-blocks, absolutely positioned boxes, table
 * cells) are laid out at a provisional origin and their display items translated
 * afterwards: every box records the range of items and link rectangles it emitted.
 */
#include "layout/hk_layout.h"
#include "base/hk_mem.h"
#include "base/hk_utf8.h"

#include <math.h>
#include <string.h>

#define A(x) HK_ATOM_##x
#define BIG 1e7f

/* --- margin collapsing ------------------------------------------------------------------------ */

typedef struct strut {
    float pos, neg;
} strut;

static void strut_add(strut *s, float m)
{
    if (m > s->pos) s->pos = m;
    if (m < s->neg) s->neg = m;
}

static float strut_value(const strut *s)
{
    return s->pos + s->neg;
}

/* --- context ------------------------------------------------------------------------------------ */

typedef struct float_rect {
    float left, right, top, bottom;
    uint8_t side; /* 1 left, 2 right */
} float_rect;

typedef struct abs_box {
    hk_node_id el;
    const hk_style *s;
    float static_x, static_y;
} abs_box;

typedef HK_VEC(abs_box) abs_vec;

typedef struct ctx {
    const hk_document *d;
    const hk_styler *st;
    hk_display_list *dl;
    float vw, vh;
    const hk_layout_hooks *hooks;
    bool measure; /* intrinsic sizing: nothing is emitted */
    HK_VEC(float_rect) floats;
    uint32_t float_base; /* floats before this index belong to enclosing formatting contexts */
    abs_vec *abs; /* pending absolutely positioned boxes of the nearest positioned ancestor */
    abs_vec fixed;
} ctx;

typedef struct range {
    uint32_t start, len;
    const hk_style *style;
    hk_node_id link;
} range;

typedef struct inline_box {
    uint32_t pos; /* index of its U+FFFC in the paragraph text */
    float w, h, baseline;
    float ml, mt;  /* margins: the content starts at the box origin plus these */
    uint32_t items_start, items_end, links_start, links_end;
    hk_node_id image; /* an image: an image item is emitted at the box */
} inline_box;

typedef HK_VEC(uint16_t) u16_vec;
typedef HK_VEC(range) range_vec;
typedef HK_VEC(inline_box) box_vec;
typedef HK_VEC(uint32_t) u32_vec;

/* The flow of children inside a block container. */
typedef struct flow {
    ctx *c;
    const hk_style *style;
    float x, w;
    float cursor;
    strut s;
    bool placed;
    float first_top;
    /* pending inline content */
    u16_vec text;
    range_vec ranges;
    box_vec boxes;
    bool space_pending;
    const hk_style *space_style; /* the style and link of the text the pending space came from */
    hk_node_id space_link;
    bool has_content;
    /* intrinsic sizing results */
    float imin, imax;
} flow;

static float box_contents(ctx *c, hk_node_id el, const hk_style *s, float x, float y, float content_w, bool bfc,
                          strut *in_out, bool *placed, float *top_out);
static void block_box(ctx *c, hk_node_id el, const hk_style *s, flow *parent);
static void table_box(ctx *c, hk_node_id el, const hk_style *s, flow *parent);
static void intrinsic(ctx *c, hk_node_id el, const hk_style *s, float *mn, float *mx);
static void flow_children(flow *f, hk_node_id parent, hk_node_id link);

/* Form controls laid out as replaced boxes (their contents are drawn by the painter). <button>
   elements are ordinary inline-blocks; their children are laid out. */
static hk_control_kind replaced_control(const ctx *c, hk_node_id el)
{
    if (hk_dom_is_html(c->d, el, A(button))) return HK_CTRL_NONE;
    hk_control_kind k = hk_control_kind_of(c->d, el);
    return k == HK_CTRL_HIDDEN ? HK_CTRL_NONE : k;
}

static bool is_block_level(uint8_t d)
{
    return d == HK_DISPLAY_BLOCK || d == HK_DISPLAY_LIST_ITEM || d == HK_DISPLAY_TABLE || d == HK_DISPLAY_TABLE_ROW_GROUP ||
           d == HK_DISPLAY_TABLE_ROW || d == HK_DISPLAY_TABLE_CELL || d == HK_DISPLAY_TABLE_CAPTION ||
           d == HK_DISPLAY_FLEX || d == HK_DISPLAY_GRID;
}

static float px_or(hk_length l, float base, float fallback)
{
    return hk_length_resolve(l, base, fallback);
}

/* --- display list ------------------------------------------------------------------------------- */

static hk_item *push_item(ctx *c, uint8_t kind)
{
    hk_item it = {0};
    it.kind = kind;
    hk_vec_push(&c->dl->items, it);
    return &c->dl->items.data[c->dl->items.len - 1];
}

static void emit_rect(ctx *c, float x, float y, float w, float h, uint32_t color)
{
    if (c->measure || (color >> 24) == 0 || w <= 0 || h <= 0) return;
    hk_item *it = push_item(c, HK_ITEM_RECT);
    it->color = color;
    it->x = x;
    it->y = y;
    it->w = w;
    it->h = h;
}

static void free_run(const hk_glyph_run *r)
{
    hk_font_face_release(r->face);
    hk_free(r->glyphs, r->count * sizeof(uint16_t));
    hk_free(r->advances, r->count * sizeof(float));
    hk_free(r->offsets, r->count * 2 * sizeof(float));
}

/* Moves the items [from, to) and links [lfrom, lto) by (dx, dy). */
static void translate(ctx *c, uint32_t from, uint32_t to, uint32_t lfrom, uint32_t lto, float dx, float dy)
{
    if (dx == 0 && dy == 0) return;
    for (uint32_t i = from; i < to; i++) {
        hk_item *it = &c->dl->items.data[i];
        it->x += dx;
        it->y += dy;
        if (it->kind == HK_ITEM_GLYPHS) {
            it->run.x += dx;
            it->run.baseline += dy;
        }
    }
    for (uint32_t i = lfrom; i < lto; i++) {
        c->dl->links.data[i].x += dx;
        c->dl->links.data[i].y += dy;
    }
}

/* --- floats --------------------------------------------------------------------------------------- */

/* The horizontal band free of floats at height y within [x0, x1). */
static void float_band(const ctx *c, float y, float h, float x0, float x1, float *left, float *right)
{
    *left = x0;
    *right = x1;
    for (uint32_t i = c->float_base; i < c->floats.len; i++) {
        const float_rect *f = &c->floats.data[i];
        if (f->bottom <= y || f->top >= y + (h > 0 ? h : 1)) continue;
        if (f->side == 1 && f->right > *left) *left = f->right;
        if (f->side == 2 && f->left < *right) *right = f->left;
    }
    if (*right < *left) *right = *left;
}

static float clearance(const ctx *c, uint8_t clear)
{
    float y = -BIG;
    for (uint32_t i = c->float_base; i < c->floats.len; i++) {
        const float_rect *f = &c->floats.data[i];
        if ((clear & f->side) && f->bottom > y) y = f->bottom;
    }
    return y;
}

static float floats_bottom(const ctx *c, uint32_t from)
{
    float y = -BIG;
    for (uint32_t i = from; i < c->floats.len; i++) {
        if (c->floats.data[i].bottom > y) y = c->floats.data[i].bottom;
    }
    return y;
}

/* --- text ----------------------------------------------------------------------------------------- */

static bool is_space(uint32_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* A collapsed space belongs to the text it came from (its underline, its link). */
static void flush_space(flow *f)
{
    if (!f->space_pending) return;
    f->space_pending = false;
    if (!f->has_content) return;
    uint32_t at = f->text.len;
    hk_vec_push(&f->text, (uint16_t)' ');
    if (f->space_style) {
        range r = {at, 1, f->space_style, f->space_link};
        hk_vec_push(&f->ranges, r);
    }
}

static void push_text(flow *f, const hk_style *s, hk_node_id link, hk_str utf8)
{
    flush_space(f);
    uint32_t start = f->text.len;
    bool collapse = s->white_space == HK_WS_NORMAL || s->white_space == HK_WS_NOWRAP || s->white_space == HK_WS_PRE_LINE;
    bool keep_newlines = s->white_space != HK_WS_NORMAL && s->white_space != HK_WS_NOWRAP;
    bool word_start = true;
    size_t i = 0;
    while (i < utf8.n) {
        uint32_t cp;
        i += hk_utf8_decode((const uint8_t *)utf8.p + i, utf8.n - i, &cp);
        if (collapse && is_space(cp) && !(keep_newlines && cp == '\n')) {
            if (!f->space_pending) {
                f->space_pending = true;
                f->space_style = s;
                f->space_link = link;
            }
            word_start = true;
            continue;
        }
        if (f->space_pending) {
            if (f->has_content) hk_vec_push(&f->text, (uint16_t)' ');
            f->space_pending = false;
        }
        if (cp < 0x80 && s->text_transform != HK_TT_NONE) {
            if ((s->text_transform == HK_TT_UPPERCASE || (s->text_transform == HK_TT_CAPITALIZE && word_start)) &&
                cp >= 'a' && cp <= 'z')
                cp -= 32;
            else if (s->text_transform == HK_TT_LOWERCASE && cp >= 'A' && cp <= 'Z')
                cp += 32;
        }
        word_start = is_space(cp);
        f->has_content = true;
        if (cp == '\n' && keep_newlines) {
            hk_vec_push(&f->text, (uint16_t)0x2028);
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            hk_vec_push(&f->text, (uint16_t)(0xD800 | (cp >> 10)));
            hk_vec_push(&f->text, (uint16_t)(0xDC00 | (cp & 0x3FF)));
        } else {
            hk_vec_push(&f->text, (uint16_t)cp);
        }
    }
    if (f->text.len > start) {
        range r = {start, f->text.len - start, s, link};
        hk_vec_push(&f->ranges, r);
    }
}

static void push_box(flow *f, const hk_style *s, hk_node_id link, inline_box *b)
{
    flush_space(f);
    b->pos = f->text.len;
    hk_vec_push(&f->text, (uint16_t)0xFFFC);
    range r = {b->pos, 1, s, link};
    hk_vec_push(&f->ranges, r);
    hk_vec_push(&f->boxes, *b);
    f->has_content = true;
}

static void text_style(ctx *c, const hk_style *s, uint32_t user, hk_text_style *out)
{
    memset(out, 0, sizeof *out);
    out->family = hk_styler_family(c->st, s->font_family);
    out->size = s->font_size;
    out->weight = s->font_weight;
    out->italic = s->italic != 0;
    out->underline = (s->decoration & HK_DECO_UNDERLINE) != 0;
    out->line_through = (s->decoration & HK_DECO_LINE_THROUGH) != 0;
    out->color = s->visible ? s->color : 0;
    out->user = user;
}

typedef struct sink_ctx {
    ctx *c;
    float x, y;
    flow *f;
} sink_ctx;

static void sink_run(void *user, const hk_glyph_run *r)
{
    sink_ctx *p = user;
    if ((r->color >> 24) == 0) {
        free_run(r);
        return;
    }
    hk_item *it = push_item(p->c, HK_ITEM_GLYPHS);
    it->color = r->color;
    it->run = *r;
    it->run.x += p->x;
    it->run.baseline += p->y;
    float w = 0;
    for (uint32_t i = 0; i < r->count; i++) w += r->advances[i];
    it->x = r->rtl ? it->run.x - w : it->run.x;
    it->w = w;
    it->y = it->run.baseline - r->size * 1.2f;
    it->h = r->size * 1.6f;
}

static void sink_rect(void *user, const hk_text_rect *r)
{
    sink_ctx *p = user;
    emit_rect(p->c, p->x + r->x, p->y + r->y, r->w, r->h, r->color);
}

static void sink_box(void *user, float x, float y, uint32_t box_user)
{
    sink_ctx *p = user;
    if (box_user == 0 || box_user > p->f->boxes.len) return;
    inline_box *b = &p->f->boxes.data[box_user - 1];
    float bx = p->x + x, by = p->y + y;
    if (b->image) {
        hk_item *it = push_item(p->c, HK_ITEM_IMAGE);
        it->node = b->image;
        it->x = bx + b->ml;
        it->y = by + b->mt;
        it->w = b->w - b->ml;
        it->h = b->h - b->mt;
        return;
    }
    translate(p->c, b->items_start, b->items_end, b->links_start, b->links_end, bx + b->ml, by + b->mt);
}

typedef struct link_ctx {
    ctx *c;
    float x, y;
    hk_node_id link;
} link_ctx;

static void on_link_rect(void *user, const hk_text_rect *r)
{
    link_ctx *l = user;
    hk_link_rect lr = {l->x + r->x, l->y + r->y, r->w, r->h, l->link, 0};
    hk_vec_push(&l->c->dl->links, lr);
}

static void reset_inline(flow *f)
{
    f->text.len = f->ranges.len = f->boxes.len = 0;
    f->space_pending = false;
    f->has_content = false;
}

static hk_text_layout *build_paragraph(flow *f, float width)
{
    ctx *c = f->c;
    const hk_style *cs = f->style;
    hk_text_para para = {0};
    para.max_width = width > 1 ? width : 1;
    para.line_height = cs->line_height;
    para.wrap = cs->white_space != HK_WS_NOWRAP && cs->white_space != HK_WS_PRE;
    switch (cs->text_align) {
    case HK_ALIGN_RIGHT:
    case HK_ALIGN_END: para.align = HK_TEXT_RIGHT; break;
    case HK_ALIGN_CENTER:
    case HK_ALIGN_LEGACY_CENTER: para.align = HK_TEXT_CENTER; break;
    case HK_ALIGN_JUSTIFY: para.align = HK_TEXT_JUSTIFY; break;
    default: para.align = HK_TEXT_LEFT; break;
    }
    hk_text_style base;
    text_style(c, cs, 0, &base);
    hk_text_layout *l = hk_text_layout_create(f->text.data, f->text.len, &para, &base);
    if (!l) return NULL;
    for (uint32_t i = 0; i < f->ranges.len; i++) {
        const range *r = &f->ranges.data[i];
        hk_text_style ts;
        text_style(c, r->style, r->link, &ts);
        hk_text_layout_set_style(l, r->start, r->len, &ts);
    }
    for (uint32_t i = 0; i < f->boxes.len; i++) {
        const inline_box *b = &f->boxes.data[i];
        hk_text_layout_set_inline_box(l, b->pos, b->w, b->h, b->baseline, i + 1);
    }
    return l;
}

/* Places (or, when measuring, sizes) the pending inline content as one paragraph. */
static void flush_inline(flow *f)
{
    if (!f->has_content || f->text.len == 0) {
        reset_inline(f);
        return;
    }
    ctx *c = f->c;
    if (c->measure) {
        hk_text_layout *l = build_paragraph(f, BIG);
        if (l) {
            float mn, mx;
            hk_text_layout_intrinsic(l, &mn, &mx);
            if (mn > f->imin) f->imin = mn;
            if (mx > f->imax) f->imax = mx;
            hk_text_layout_destroy(l);
        }
        reset_inline(f);
        return;
    }
    float y = f->cursor + strut_value(&f->s);
    f->s = (strut){0, 0};
    float left, right;
    float_band(c, y, f->style->font_size * 1.5f, f->x, f->x + f->w, &left, &right);
    hk_text_layout *l = build_paragraph(f, right - left);
    if (l) {
        float w, h;
        hk_text_layout_size(l, &w, &h);
        if (!f->placed) {
            f->placed = true;
            f->first_top = y;
        }
        sink_ctx sc = {c, left, y, f};
        hk_text_sink sink = {&sc, sink_run, sink_rect, sink_box};
        hk_text_layout_emit(l, &sink);
        for (uint32_t i = 0; i < f->ranges.len;) {
            hk_node_id link = f->ranges.data[i].link;
            uint32_t j = i + 1;
            while (j < f->ranges.len && f->ranges.data[j].link == link) j++;
            if (link) {
                uint32_t start = f->ranges.data[i].start;
                uint32_t end = f->ranges.data[j - 1].start + f->ranges.data[j - 1].len;
                link_ctx lc = {c, left, y, link};
                hk_text_layout_range_rects(l, start, end - start, on_link_rect, &lc);
            }
            i = j;
        }
        f->cursor = y + h;
        hk_text_layout_destroy(l);
    }
    reset_inline(f);
}

/* --- replaced elements (images) --------------------------------------------------------------------- */

/* Used size of an image (CSS 2.1 10.3.2 and 10.6.2). Returns false when it shows its alt text instead. */
static bool image_size(ctx *c, hk_node_id el, const hk_style *s, float cbw, float *w, float *h)
{
    float iw = 0, ih = 0;
    hk_image_state st = HK_IMAGE_PENDING;
    if (c->hooks && c->hooks->image_size) st = c->hooks->image_size(c->hooks->user, el, &iw, &ih);
    if (st == HK_IMAGE_BROKEN) return false;
    bool has_w = s->width.unit != HK_LEN_AUTO, has_h = s->height.unit == HK_LEN_PX;
    float cw = has_w ? px_or(s->width, cbw, 0) : 0, ch = has_h ? s->height.px : 0;
    float ratio = (st == HK_IMAGE_READY && iw > 0 && ih > 0) ? iw / ih : 0;
    if (has_w && has_h) {
        *w = cw;
        *h = ch;
    } else if (has_w) {
        *w = cw;
        *h = ratio > 0 ? cw / ratio : 0;
    } else if (has_h) {
        *h = ch;
        *w = ratio > 0 ? ch * ratio : 0;
    } else {
        *w = iw;
        *h = ih;
    }
    if (s->max_width.unit != HK_LEN_NONE) {
        float mx = px_or(s->max_width, cbw, *w);
        if (*w > mx) {
            if (ratio > 0 && !has_h) *h = mx / ratio;
            *w = mx;
        }
    }
    if (s->max_height.unit == HK_LEN_PX && *h > s->max_height.px) {
        if (ratio > 0 && !has_w) *w = s->max_height.px * ratio;
        *h = s->max_height.px;
    }
    if (*w < 0) *w = 0;
    if (*h < 0) *h = 0;
    return true;
}

static void push_alt(flow *f, hk_node_id el, const hk_style *s, hk_node_id link)
{
    const hk_attr *alt = hk_dom_attr(f->c->d, el, HK_ATTR_NS_NONE, A(alt));
    if (alt && hk_str_trim_ascii_ws(alt->value).n) push_text(f, s, link, alt->value);
}

/* --- form controls ---------------------------------------------------------------------------------- */

static void line_sink_run(void *user, const hk_glyph_run *r)
{
    float *baseline = user;
    if (*baseline < 0) *baseline = r->baseline;
    free_run(r);
}

void hk_layout_control_line(const hk_styler *st, const hk_style *s, float *height, float *baseline)
{
    hk_text_style ts = {0};
    ts.family = hk_styler_family(st, s->font_family);
    ts.size = s->font_size;
    ts.weight = s->font_weight;
    ts.color = 0xFF000000u;
    hk_text_para para = {0};
    para.max_width = BIG;
    para.line_height = s->line_height;
    static const uint16_t k_x[] = {'X'};
    *height = s->font_size * 1.2f;
    *baseline = s->font_size;
    hk_text_layout *l = hk_text_layout_create(k_x, 1, &para, &ts);
    if (!l) return;
    float w, h, b = -1;
    hk_text_layout_size(l, &w, &h);
    hk_text_sink sink = {&b, line_sink_run, NULL, NULL};
    hk_text_layout_emit(l, &sink);
    hk_text_layout_destroy(l);
    if (h > 0) *height = h;
    if (b >= 0) *baseline = b;
}

static float text_width(ctx *c, const hk_style *s, hk_str utf8)
{
    if (!utf8.n) return 0;
    hk_arena a;
    hk_arena_init(&a, 0);
    size_t n;
    uint16_t *w = hk_utf8_to_utf16(&a, utf8, &n);
    hk_text_style ts;
    text_style(c, s, 0, &ts);
    ts.color = 0xFF000000u;
    hk_text_para para = {0};
    para.max_width = BIG;
    float mn = 0, mx = 0;
    hk_text_layout *l = hk_text_layout_create(w, (uint32_t)n, &para, &ts);
    if (l) {
        hk_text_layout_intrinsic(l, &mn, &mx);
        hk_text_layout_destroy(l);
    }
    hk_arena_free(&a);
    return mx;
}

static uint32_t uint_attr(const hk_document *d, hk_node_id el, hk_atom name, uint32_t fallback)
{
    const hk_attr *a = hk_dom_attr(d, el, HK_ATTR_NS_NONE, name);
    if (!a) return fallback;
    hk_str v = hk_str_trim_ascii_ws(a->value);
    uint32_t n = 0;
    size_t i = 0;
    while (i < v.n && v.p[i] >= '0' && v.p[i] <= '9' && n < 10000) n = n * 10 + (uint32_t)(v.p[i++] - '0');
    return i && n ? n : fallback;
}

/* Content box size of a replaced control. */
static void control_size(ctx *c, hk_node_id el, hk_control_kind k, const hk_style *s, float *w, float *h)
{
    float lh, base;
    hk_layout_control_line(c->st, s, &lh, &base);
    float avg = s->font_size * 0.55f;
    *h = lh;
    switch (k) {
    case HK_CTRL_CHECKBOX:
    case HK_CTRL_RADIO: *w = *h = 13; break;
    case HK_CTRL_TEXTAREA:
        *w = avg * (float)uint_attr(c->d, el, A(cols), 20) + 12; /* room for a scroll bar in other browsers */
        *h = lh * (float)uint_attr(c->d, el, A(rows), 2);
        break;
    case HK_CTRL_SELECT: {
        hk_node_vec opts = {0};
        hk_charbuf buf = {0};
        hk_select_options(c->d, el, &opts);
        float widest = 0;
        for (uint32_t i = 0; i < opts.len; i++) {
            float tw = text_width(c, s, hk_option_label(c->d, opts.data[i], &buf));
            if (tw > widest) widest = tw;
        }
        hk_vec_free(&opts);
        hk_vec_free(&buf);
        *w = widest + s->font_size * 1.4f; /* the arrow */
        break;
    }
    case HK_CTRL_SUBMIT:
    case HK_CTRL_IMAGE:
    case HK_CTRL_RESET:
    case HK_CTRL_BUTTON:
    case HK_CTRL_FILE: *w = text_width(c, s, hk_control_button_label(c->d, el)); break;
    default: *w = avg * (float)uint_attr(c->d, el, A(size), 20); break;
    }
}

/* Baseline of a control's first line from its content box top. */
static float control_baseline(ctx *c, hk_control_kind k, const hk_style *s, float content_h)
{
    if (k == HK_CTRL_CHECKBOX || k == HK_CTRL_RADIO) return content_h;
    float lh, base;
    hk_layout_control_line(c->st, s, &lh, &base);
    if (k == HK_CTRL_TEXTAREA) return base;
    return (content_h - lh) / 2 + base;
}

static void push_hit(ctx *c, float x, float y, float w, float h, hk_node_id el)
{
    if (c->measure || w <= 0 || h <= 0) return;
    hk_link_rect lr = {x, y, w, h, el, 0};
    hk_vec_push(&c->dl->links, lr);
}

/* --- inline-level boxes ------------------------------------------------------------------------------- */

static void shrink_to_fit(ctx *c, hk_node_id el, const hk_style *s, float avail, float *content_w)
{
    float frame = s->border_width[1] + s->border_width[3] + px_or(s->padding[1], avail, 0) + px_or(s->padding[3], avail, 0);
    if (s->width.unit != HK_LEN_AUTO) {
        *content_w = px_or(s->width, avail, 0) - (s->border_box ? frame : 0);
    } else {
        float mn, mx;
        intrinsic(c, el, s, &mn, &mx);
        float w = mx;
        if (w > avail) w = avail;
        if (w < mn) w = mn;
        *content_w = w - frame;
    }
    if (s->max_width.unit != HK_LEN_NONE) {
        float mx = px_or(s->max_width, avail, *content_w) - (s->border_box ? frame : 0);
        if (*content_w > mx) *content_w = mx;
    }
    if (s->min_width.unit != HK_LEN_AUTO) {
        float mn = px_or(s->min_width, avail, 0) - (s->border_box ? frame : 0);
        if (*content_w < mn) *content_w = mn;
    }
    if (*content_w < 0) *content_w = 0;
}

/* An inline-block (or inline table/flex): laid out at the origin, moved when its line is placed. */
static void inline_block(flow *f, hk_node_id el, const hk_style *s, hk_node_id link)
{
    ctx *c = f->c;
    float cbw = f->w;
    float ml = px_or(s->margin[3], cbw, 0), mr = px_or(s->margin[1], cbw, 0);
    float mt = px_or(s->margin[0], cbw, 0), mb = px_or(s->margin[2], cbw, 0);
    float content_w;
    shrink_to_fit(c, el, s, cbw - ml - mr, &content_w);
    float frame = s->border_width[1] + s->border_width[3] + px_or(s->padding[1], cbw, 0) + px_or(s->padding[3], cbw, 0);
    inline_box b = {0};
    if (c->measure) {
        b.w = content_w + frame + ml + mr;
        b.h = 1;
        b.baseline = 1;
        push_box(f, s, link, &b);
        return;
    }
    b.items_start = c->dl->items.len;
    b.links_start = c->dl->links.len;
    uint32_t saved_floats = c->floats.len;
    strut st = {0, 0};
    bool placed;
    float top;
    float bottom = box_contents(c, el, s, 0, 0, content_w, true, &st, &placed, &top);
    c->floats.len = saved_floats;
    b.items_end = c->dl->items.len;
    b.links_end = c->dl->links.len;
    b.ml = ml;
    b.mt = mt;
    b.w = content_w + frame + ml + mr;
    b.h = bottom + mt + mb;
    b.baseline = bottom + mt; /* the bottom margin edge sits on the baseline */
    hk_control_kind k = replaced_control(c, el);
    if (k == HK_CTRL_NONE && hk_dom_is_html(c->d, el, A(button))) k = HK_CTRL_SUBMIT; /* one line of text, centered */
    if (k != HK_CTRL_NONE) {
        float bt = s->border_width[0], pt = px_or(s->padding[0], cbw, 0);
        float content_h = bottom - bt - pt - px_or(s->padding[2], cbw, 0) - s->border_width[2];
        b.baseline = mt + bt + pt + control_baseline(c, k, s, content_h);
    }
    push_box(f, s, link, &b);
}

/* --- the flow ---------------------------------------------------------------------------------------- */

static hk_node_id link_of(const hk_document *d, hk_node_id el, hk_node_id inherited)
{
    if (hk_dom_is_html(d, el, A(a)) && hk_dom_attr(d, el, HK_ATTR_NS_NONE, A(href))) return el;
    if (hk_dom_is_html(d, el, A(label)) || hk_dom_is_html(d, el, A(button))) return el;
    return inherited;
}

static bool visually_hidden(const hk_style *s)
{
    /* The common ".sr-only" pattern: a 1px clipped box (clip itself is not implemented). */
    return s->overflow_hidden && ((s->width.unit == HK_LEN_PX && s->width.px <= 1.5f) ||
                                  (s->height.unit == HK_LEN_PX && s->height.px <= 1.5f));
}

static void place_float(flow *f, hk_node_id el, const hk_style *s)
{
    ctx *c = f->c;
    flush_inline(f);
    float cbw = f->w;
    float ml = px_or(s->margin[3], cbw, 0), mr = px_or(s->margin[1], cbw, 0);
    float mt = px_or(s->margin[0], cbw, 0), mb = px_or(s->margin[2], cbw, 0);
    float content_w;
    shrink_to_fit(c, el, s, cbw - ml - mr, &content_w);
    float frame = s->border_width[1] + s->border_width[3] + px_or(s->padding[1], cbw, 0) + px_or(s->padding[3], cbw, 0);
    float outer_w = content_w + frame + ml + mr;
    if (c->measure) {
        if (outer_w > f->imin) f->imin = outer_w;
        if (outer_w > f->imax) f->imax = outer_w;
        return;
    }
    float y = f->cursor + strut_value(&f->s);
    if (s->clear) {
        float cy = clearance(c, s->clear);
        if (cy > y) y = cy;
    }
    /* Move down until the float fits beside the existing ones. */
    float left, right;
    for (int guard = 0; guard < 64; guard++) {
        float_band(c, y, 1, f->x, f->x + f->w, &left, &right);
        if (right - left >= outer_w || (left == f->x && right == f->x + f->w)) break;
        float next = BIG;
        for (uint32_t i = c->float_base; i < c->floats.len; i++) {
            const float_rect *r = &c->floats.data[i];
            if (r->bottom > y && r->bottom < next) next = r->bottom;
        }
        if (next >= BIG) break;
        y = next;
    }
    float x = s->floating == 1 ? left : right - outer_w;
    uint32_t saved = c->floats.len;
    strut st = {0, 0};
    bool placed;
    float top;
    float bottom = box_contents(c, el, s, x + ml, y + mt, content_w, true, &st, &placed, &top);
    float fb = floats_bottom(c, saved);
    if (fb > bottom) bottom = fb;
    c->floats.len = saved;
    float_rect r = {x, x + outer_w, y, bottom + mb, s->floating};
    hk_vec_push(&c->floats, r);
}

static void defer_absolute(flow *f, hk_node_id el, const hk_style *s)
{
    ctx *c = f->c;
    if (c->measure) return;
    abs_box b = {el, s, f->x, f->cursor + strut_value(&f->s)};
    if (f->has_content) b.static_y = f->cursor + strut_value(&f->s); /* approximation: after the pending line */
    hk_vec_push(s->position == HK_POS_FIXED ? &c->fixed : c->abs, b);
}

static void flow_node(flow *f, hk_node_id node, hk_node_id link)
{
    ctx *c = f->c;
    const hk_node *n = hk_dom_node(c->d, node);
    if (n->type == HK_NODE_TEXT) {
        const hk_style *ps = hk_style_of(c->st, n->parent);
        if (ps) push_text(f, ps, link, hk_dom_text(c->d, node));
        return;
    }
    if (n->type != HK_NODE_ELEMENT) return;
    const hk_style *s = hk_style_of(c->st, node);
    if (!s || s->display == HK_DISPLAY_NONE || visually_hidden(s)) return;
    if (s->position == HK_POS_ABSOLUTE || s->position == HK_POS_FIXED) {
        defer_absolute(f, node, s);
        return;
    }
    hk_node_id lk = link_of(c->d, node, link);
    if (n->ns == HK_NS_HTML && hk_dom_element(c->d, node)->name == A(br)) {
        uint32_t start = f->text.len;
        hk_vec_push(&f->text, (uint16_t)0x2028);
        range r = {start, 1, s, lk};
        hk_vec_push(&f->ranges, r);
        f->has_content = true;
        f->space_pending = false;
        return;
    }
    if (s->floating) {
        place_float(f, node, s);
        return;
    }
    if (n->ns == HK_NS_HTML && hk_dom_element(c->d, node)->name == A(img)) {
        float w, h;
        if (!image_size(c, node, s, f->w, &w, &h)) {
            push_alt(f, node, s, lk);
            return;
        }
        float ml = px_or(s->margin[3], f->w, 0), mt = px_or(s->margin[0], f->w, 0);
        float mr = px_or(s->margin[1], f->w, 0), mb = px_or(s->margin[2], f->w, 0);
        if (is_block_level(s->display)) {
            flush_inline(f);
            if (c->measure) {
                if (w + ml + mr > f->imax) f->imax = w + ml + mr;
                if (w + ml + mr > f->imin) f->imin = w + ml + mr;
                return;
            }
            strut_add(&f->s, mt);
            float y = f->cursor + strut_value(&f->s);
            hk_item *it = push_item(c, HK_ITEM_IMAGE);
            it->node = node;
            it->x = f->x + ml;
            it->y = y;
            it->w = w;
            it->h = h;
            if (!f->placed) {
                f->placed = true;
                f->first_top = y;
            }
            f->cursor = y + h;
            f->s = (strut){0, 0};
            strut_add(&f->s, mb);
            return;
        }
        inline_box b = {0};
        b.image = node;
        b.ml = ml;
        b.mt = mt;
        b.w = w + ml + mr;
        b.h = h + mt + mb;
        b.baseline = h + mt;
        push_box(f, s, lk, &b);
        return;
    }
    if (is_block_level(s->display)) {
        flush_inline(f);
        if (c->measure) {
            float mn, mx;
            intrinsic(c, node, s, &mn, &mx);
            float m = px_or(s->margin[1], 0, 0) + px_or(s->margin[3], 0, 0);
            if (mn + m > f->imin) f->imin = mn + m;
            if (mx + m > f->imax) f->imax = mx + m;
            return;
        }
        if (s->display == HK_DISPLAY_TABLE) table_box(c, node, s, f);
        else block_box(c, node, s, f);
        return;
    }
    if (s->display == HK_DISPLAY_INLINE_BLOCK ||
        (s->display == HK_DISPLAY_INLINE && (replaced_control(c, node) != HK_CTRL_NONE ||
                                             hk_dom_is_html(c->d, node, A(button))))) {
        inline_block(f, node, s, lk);
        return;
    }
    /* Inline boxes: their horizontal margins, borders and padding become blank space at their ends. */
    float left = px_or(s->margin[3], f->w, 0) + s->border_width[3] + px_or(s->padding[3], f->w, 0);
    float right = px_or(s->margin[1], f->w, 0) + s->border_width[1] + px_or(s->padding[1], f->w, 0);
    if (left >= 0.5f) {
        inline_box b = {0};
        b.w = left;
        push_box(f, s, lk, &b);
    }
    flow_children(f, node, lk);
    if (right >= 0.5f) {
        inline_box b = {0};
        b.w = right;
        push_box(f, s, lk, &b);
    }
}

static void flow_children(flow *f, hk_node_id parent, hk_node_id link)
{
    const hk_document *d = f->c->d;
    for (hk_node_id ch = hk_dom_node(d, parent)->first_child; ch; ch = hk_dom_node(d, ch)->next) flow_node(f, ch, link);
}

/* --- intrinsic widths ------------------------------------------------------------------------------ */

static void table_intrinsic(ctx *c, hk_node_id el, const hk_style *s, float *mn, float *mx);

static void intrinsic(ctx *c, hk_node_id el, const hk_style *s, float *mn, float *mx)
{
    float frame = s->border_width[1] + s->border_width[3] + px_or(s->padding[1], 0, 0) + px_or(s->padding[3], 0, 0);
    if (s->width.unit == HK_LEN_PX) {
        *mn = *mx = s->width.px + (s->border_box ? 0 : frame);
        return;
    }
    if (s->display == HK_DISPLAY_TABLE) {
        table_intrinsic(c, el, s, mn, mx);
        return;
    }
    hk_control_kind ck = replaced_control(c, el);
    if (ck != HK_CTRL_NONE) {
        float w, h;
        control_size(c, el, ck, s, &w, &h);
        *mn = *mx = w + frame;
        return;
    }
    bool saved = c->measure;
    c->measure = true;
    flow f = {0};
    f.c = c;
    f.style = s;
    f.w = BIG;
    flow_children(&f, el, HK_NODE_NONE);
    flush_inline(&f);
    c->measure = saved;
    hk_vec_free(&f.text);
    hk_vec_free(&f.ranges);
    hk_vec_free(&f.boxes);
    *mn = f.imin + frame;
    *mx = f.imax + frame;
    if (s->max_width.unit == HK_LEN_PX && *mx > s->max_width.px + frame) *mx = s->max_width.px + frame;
    if (*mn > *mx) *mn = *mx;
    if (s->min_width.unit == HK_LEN_PX) {
        float m = s->min_width.px + (s->border_box ? 0 : frame);
        if (*mn < m) *mn = m;
        if (*mx < m) *mx = m;
    }
}

/* --- list markers -------------------------------------------------------------------------------- */

static void emit_marker(ctx *c, hk_node_id li, const hk_style *s, float content_x, float top)
{
    if (c->measure || s->list_style == HK_LIST_NONE || !s->visible) return;
    uint16_t buf[16];
    uint32_t n = 0;
    if (s->list_style == HK_LIST_DECIMAL) {
        int index = 1;
        hk_node_id parent = hk_dom_node(c->d, li)->parent;
        const hk_attr *start = parent ? hk_dom_attr(c->d, parent, HK_ATTR_NS_NONE, A(start)) : NULL;
        if (start) {
            int v = 0, sign = 1;
            size_t i = 0;
            if (start->value.n && start->value.p[0] == '-') sign = -1, i = 1;
            for (; i < start->value.n && start->value.p[i] >= '0' && start->value.p[i] <= '9'; i++)
                v = v * 10 + (start->value.p[i] - '0');
            index = sign * v;
        }
        for (hk_node_id p = hk_dom_node(c->d, li)->prev; p; p = hk_dom_node(c->d, p)->prev) {
            if (hk_dom_node(c->d, p)->type != HK_NODE_ELEMENT) continue;
            const hk_style *ps = hk_style_of(c->st, p);
            if (ps && ps->display == HK_DISPLAY_LIST_ITEM) index++;
        }
        char tmp[12];
        int k = 0;
        bool neg = index < 0;
        if (neg) index = -index;
        do {
            tmp[k++] = (char)('0' + index % 10);
            index /= 10;
        } while (index && k < 10);
        if (neg) buf[n++] = '-';
        while (k) buf[n++] = (uint16_t)tmp[--k];
        buf[n++] = '.';
    } else {
        buf[n++] = s->list_style == HK_LIST_CIRCLE ? 0x25E6 : s->list_style == HK_LIST_SQUARE ? 0x25AA : 0x2022;
    }
    buf[n++] = ' ';
    hk_text_para para = {0};
    para.max_width = 200;
    hk_text_style ts;
    text_style(c, s, 0, &ts);
    ts.underline = ts.line_through = false;
    hk_text_layout *l = hk_text_layout_create(buf, n, &para, &ts);
    if (!l) return;
    float w, h;
    hk_text_layout_size(l, &w, &h);
    float line = s->line_height > 0 ? s->line_height : 0;
    float y = top + (line > 0 ? (line - h) / 2 : 0);
    sink_ctx sc = {c, content_x - w, y, NULL};
    hk_text_sink sink = {&sc, sink_run, NULL, NULL};
    hk_text_layout_emit(l, &sink);
    hk_text_layout_destroy(l);
}

/* --- absolutely positioned boxes -------------------------------------------------------------------- */

typedef struct cb_rect {
    float x, y, w, h;
} cb_rect;

static void layout_absolute(ctx *c, const abs_box *b, cb_rect cb, bool fixed)
{
    const hk_style *s = b->s;
    float ml = px_or(s->margin[3], cb.w, 0), mr = px_or(s->margin[1], cb.w, 0);
    float mt = px_or(s->margin[0], cb.w, 0), mb = px_or(s->margin[2], cb.w, 0);
    bool has_l = s->inset[3].unit != HK_LEN_AUTO, has_r = s->inset[1].unit != HK_LEN_AUTO;
    bool has_t = s->inset[0].unit != HK_LEN_AUTO, has_b = s->inset[2].unit != HK_LEN_AUTO;
    float l = px_or(s->inset[3], cb.w, 0), r = px_or(s->inset[1], cb.w, 0);
    float t = px_or(s->inset[0], cb.h, 0), bo = px_or(s->inset[2], cb.h, 0);
    float frame = s->border_width[1] + s->border_width[3] + px_or(s->padding[1], cb.w, 0) + px_or(s->padding[3], cb.w, 0);
    float content_w;
    if (s->width.unit == HK_LEN_AUTO && has_l && has_r) {
        content_w = cb.w - l - r - ml - mr - frame;
        if (content_w < 0) content_w = 0;
    } else {
        shrink_to_fit(c, b->el, s, cb.w - (has_l ? l : 0) - (has_r ? r : 0) - ml - mr, &content_w);
    }
    uint32_t items = c->dl->items.len, links = c->dl->links.len, saved_floats = c->floats.len;
    abs_vec nested = {0};
    abs_vec *saved_abs = c->abs;
    c->abs = &nested;
    strut st = {0, 0};
    bool placed;
    float top;
    float bottom = box_contents(c, b->el, s, 0, 0, content_w, true, &st, &placed, &top);
    float h = bottom;
    float x = has_l ? cb.x + l + ml : has_r ? cb.x + cb.w - r - mr - content_w - frame : b->static_x + ml;
    float y = has_t ? cb.y + t + mt : has_b ? cb.y + cb.h - bo - mb - h : b->static_y + mt;
    translate(c, items, c->dl->items.len, links, c->dl->links.len, x, y);
    /* Nested absolute boxes use this box as their containing block. */
    cb_rect inner = {x, y, content_w + frame, h};
    for (uint32_t i = 0; i < nested.len; i++) {
        abs_box nb = nested.data[i];
        nb.static_x += x;
        nb.static_y += y;
        layout_absolute(c, &nb, inner, fixed);
    }
    hk_vec_free(&nested);
    c->abs = saved_abs;
    c->floats.len = saved_floats;
    if (fixed) {
        for (uint32_t i = items; i < c->dl->items.len; i++) c->dl->items.data[i].fixed = 1;
        for (uint32_t i = links; i < c->dl->links.len; i++) c->dl->links.data[i].fixed = 1;
    }
}

/* --- block boxes ----------------------------------------------------------------------------------- */

/*
 * Lays out the box's frame and children with its border box left edge at x and its top margin
 * edge at y (margins already resolved by the caller except collapsing). in_out is the margin strut
 * before the box (the box's own top margin already added); on return it holds the strut after it.
 * Returns the bottom border edge; *top_out receives the top border edge.
 */
static float box_contents(ctx *c, hk_node_id el, const hk_style *s, float x, float y, float content_w, bool bfc,
                          strut *in_out, bool *placed_out, float *top_out)
{
    float cbw = content_w;
    float bt = s->border_width[0], br = s->border_width[1], bb = s->border_width[2], bl = s->border_width[3];
    float pt = px_or(s->padding[0], cbw, 0), pr = px_or(s->padding[1], cbw, 0);
    float pb = px_or(s->padding[2], cbw, 0), pl = px_or(s->padding[3], cbw, 0);
    bool positioned = s->position != HK_POS_STATIC;
    bool top_sep = bt + pt > 0 || bfc;
    flow f = {0};
    f.c = c;
    f.style = s;
    f.x = x + bl + pl;
    f.w = content_w;
    float top = 0;
    if (top_sep) {
        top = y + strut_value(in_out);
        *in_out = (strut){0, 0};
        f.cursor = top + bt + pt;
        f.placed = true;
        f.first_top = top;
    } else {
        f.cursor = y;
        f.s = *in_out;
        *in_out = (strut){0, 0};
    }

    uint32_t items_start = c->dl->items.len, links_start = c->dl->links.len;
    uint32_t bg_index = 0, clip_index = 0;
    if (!c->measure) {
        bg_index = c->dl->items.len;
        for (int k = 0; k < 5; k++) push_item(c, HK_ITEM_RECT); /* background + 4 borders */
        if (s->overflow_hidden) {
            clip_index = c->dl->items.len;
            push_item(c, HK_ITEM_CLIP);
        }
    }
    uint32_t saved_floats = c->floats.len, saved_base = c->float_base;
    /* A new block formatting context: floats outside it do not affect its contents (CSS 2.1 9.4.1). */
    if (bfc) c->float_base = c->floats.len;
    abs_vec own_abs = {0};
    abs_vec *saved_abs = c->abs;
    if (positioned && s->position != HK_POS_FIXED) c->abs = &own_abs;

    hk_control_kind ck = replaced_control(c, el);
    uint32_t control_index = UINT32_MAX;
    if (ck != HK_CTRL_NONE) {
        float cw, ch;
        control_size(c, el, ck, s, &cw, &ch);
        if (!c->measure) {
            control_index = c->dl->items.len;
            hk_item *it = push_item(c, HK_ITEM_CONTROL);
            it->node = el;
            it->x = f.x;
            it->y = f.cursor;
            it->w = content_w;
            it->h = ch;
        }
        if (!f.placed) {
            f.placed = true;
            f.first_top = f.cursor;
        }
        f.cursor += ch;
    } else {
        flow_children(&f, el, HK_NODE_NONE);
        flush_inline(&f);
    }

    if (!top_sep) top = f.placed ? f.first_top : f.cursor + strut_value(&f.s);
    float bottom;
    float content_top = top + bt + pt;
    if (s->height.unit == HK_LEN_PX) {
        float h = s->height.px - (s->border_box ? bt + bb + pt + pb : 0);
        if (h < 0) h = 0;
        bottom = content_top + h + pb + bb;
    } else {
        float content_bottom;
        if (bb + pb > 0 || bfc || !f.placed) {
            content_bottom = f.placed ? f.cursor + strut_value(&f.s) : content_top;
            if (!f.placed && !top_sep) {
                content_bottom = top; /* empty: margins collapse through */
                *in_out = f.s;
            }
        } else {
            content_bottom = f.cursor;
            *in_out = f.s; /* the last child's bottom margin collapses with ours */
        }
        if (bfc) {
            float fb = floats_bottom(c, saved_floats);
            if (fb > content_bottom) content_bottom = fb;
        }
        if (s->min_height.unit == HK_LEN_PX) {
            float mh = s->min_height.px - (s->border_box ? bt + bb + pt + pb : 0);
            if (content_bottom - content_top < mh) {
                content_bottom = content_top + mh;
                *in_out = (strut){0, 0};
            }
        }
        if (s->max_height.unit == HK_LEN_PX && content_bottom - content_top > s->max_height.px)
            content_bottom = content_top + s->max_height.px;
        bottom = (bb + pb > 0 || bfc || !f.placed) ? content_bottom + pb + bb : content_bottom;
        if (s->height.unit == HK_LEN_PX || bb + pb > 0) *in_out = (strut){0, 0};
    }
    if (bfc) c->floats.len = saved_floats;
    c->float_base = saved_base;
    if (s->display == HK_DISPLAY_LIST_ITEM) emit_marker(c, el, s, f.x, f.placed ? f.first_top : top);

    float bw = content_w + bl + br + pl + pr, bh = bottom - top;
    if (control_index != UINT32_MAX) {
        hk_item *it = &c->dl->items.data[control_index];
        it->y = top + bt + pt;
        it->h = bottom - bb - pb - it->y;
        if (it->h < 0) it->h = 0;
    }
    if (ck != HK_CTRL_NONE || hk_dom_is_html(c->d, el, A(button))) push_hit(c, x, top, bw, bh, el);
    if (!c->measure) {
        if (s->overflow_hidden) {
            hk_item *cl = &c->dl->items.data[clip_index];
            cl->x = x;
            cl->y = top;
            cl->w = bw;
            cl->h = bh;
            push_item(c, HK_ITEM_UNCLIP);
        }
        if (s->visible) {
            hk_item *bg = &c->dl->items.data[bg_index];
            if ((s->background >> 24) && bw > 0 && bh > 0) {
                bg->color = s->background;
                bg->x = x;
                bg->y = top;
                bg->w = bw;
                bg->h = bh;
            }
            float rects[4][4] = {{x, top, bw, bt}, {x + bw - br, top, br, bh}, {x, bottom - bb, bw, bb}, {x, top, bl, bh}};
            for (int k = 0; k < 4; k++) {
                hk_item *bd = &c->dl->items.data[bg_index + 1 + k];
                if (s->border_width[k] <= 0 || (s->border_color[k] >> 24) == 0) continue;
                bd->color = s->border_color[k];
                bd->x = rects[k][0];
                bd->y = rects[k][1];
                bd->w = rects[k][2];
                bd->h = rects[k][3];
            }
        }
        /* Absolutely positioned descendants: this box is their containing block (padding box). */
        if (c->abs == &own_abs) {
            c->abs = saved_abs;
            cb_rect cb = {x + bl, top + bt, bw - bl - br, bh - bt - bb};
            for (uint32_t i = 0; i < own_abs.len; i++) layout_absolute(c, &own_abs.data[i], cb, false);
        }
        if (s->position == HK_POS_RELATIVE || s->position == HK_POS_STICKY) {
            float dx = s->inset[3].unit != HK_LEN_AUTO ? px_or(s->inset[3], cbw, 0)
                       : s->inset[1].unit != HK_LEN_AUTO ? -px_or(s->inset[1], cbw, 0) : 0;
            float dy = s->inset[0].unit != HK_LEN_AUTO ? px_or(s->inset[0], 0, 0)
                       : s->inset[2].unit != HK_LEN_AUTO ? -px_or(s->inset[2], 0, 0) : 0;
            if (s->position == HK_POS_RELATIVE)
                translate(c, items_start, c->dl->items.len, links_start, c->dl->links.len, dx, dy);
        }
    }
    c->abs = saved_abs;
    hk_vec_free(&own_abs);
    hk_vec_free(&f.text);
    hk_vec_free(&f.ranges);
    hk_vec_free(&f.boxes);
    *placed_out = f.placed;
    *top_out = top;
    return bottom;
}

static void block_box(ctx *c, hk_node_id el, const hk_style *s, flow *parent)
{
    float cbw = parent->w;
    float frame = s->border_width[1] + s->border_width[3] + px_or(s->padding[1], cbw, 0) + px_or(s->padding[3], cbw, 0);
    bool ml_auto = s->margin[3].unit == HK_LEN_AUTO, mr_auto = s->margin[1].unit == HK_LEN_AUTO;
    float mt = px_or(s->margin[0], cbw, 0), mb = px_or(s->margin[2], cbw, 0);
    float ml = px_or(s->margin[3], cbw, 0), mr = px_or(s->margin[1], cbw, 0);
    bool bfc = s->overflow_hidden || s->display == HK_DISPLAY_TABLE_CELL || s->display == HK_DISPLAY_FLEX ||
               s->display == HK_DISPLAY_GRID;

    if (s->clear) {
        float cy = clearance(c, s->clear);
        float y = parent->cursor + strut_value(&parent->s);
        if (cy > y) {
            parent->cursor = cy;
            parent->s = (strut){0, 0};
        }
    }
    float avail_x = parent->x, avail_w = cbw;
    if (bfc && !c->measure) {
        /* A new formatting context does not overlap floats: it is narrowed beside them. */
        float left, right;
        float_band(c, parent->cursor + strut_value(&parent->s), 1, parent->x, parent->x + cbw, &left, &right);
        avail_x = left;
        avail_w = right - left;
    }

    float content_w;
    /* Controls and buttons keep their shrink-to-fit width even when block-level. */
    if (s->width.unit == HK_LEN_AUTO && (replaced_control(c, el) != HK_CTRL_NONE || hk_dom_is_html(c->d, el, A(button)))) {
        shrink_to_fit(c, el, s, avail_w - ml - mr, &content_w);
    } else if (s->width.unit == HK_LEN_AUTO) {
        content_w = avail_w - ml - mr - frame;
    } else {
        content_w = px_or(s->width, cbw, 0);
        if (s->border_box) content_w -= frame;
    }
    if (s->max_width.unit != HK_LEN_NONE) {
        float mx = px_or(s->max_width, cbw, content_w) - (s->border_box ? frame : 0);
        if (content_w > mx) content_w = mx;
    }
    if (s->min_width.unit != HK_LEN_AUTO) {
        float mn = px_or(s->min_width, cbw, 0) - (s->border_box ? frame : 0);
        if (content_w < mn) content_w = mn;
    }
    if (content_w < 0) content_w = 0;
    float rest = avail_w - content_w - frame;
    if (parent->style->text_align == HK_ALIGN_LEGACY_CENTER && !ml_auto && !mr_auto && ml == 0 && mr == 0)
        ml_auto = mr_auto = true; /* <center> and align=center center their block children */
    if (s->width.unit != HK_LEN_AUTO || s->max_width.unit != HK_LEN_NONE || s->min_width.unit != HK_LEN_AUTO) {
        if (ml_auto && mr_auto) ml = mr = rest > 0 ? rest / 2 : 0;
        else if (ml_auto) ml = rest - mr;
        else if (mr_auto) mr = rest - ml;
    }

    strut_add(&parent->s, mt);
    bool placed;
    float top;
    float bottom = box_contents(c, el, s, avail_x + ml, parent->cursor, content_w, bfc, &parent->s, &placed, &top);
    strut_add(&parent->s, mb);
    parent->cursor = bottom;
    if (!parent->placed) {
        parent->placed = true;
        parent->first_top = top;
    }
}

/* --- tables ---------------------------------------------------------------------------------------- */

typedef struct cell {
    hk_node_id el;
    const hk_style *s;
    uint32_t row, col, colspan, rowspan;
    float mn, mx;
    /* layout results */
    float top, height;
    uint32_t items_start, items_end, links_start, links_end, bg_index;
} cell;

typedef struct row {
    hk_node_id el;
    const hk_style *s;
    float top, height;
    uint32_t bg_index;
} row;

typedef struct table_grid {
    HK_VEC(cell) cells;
    HK_VEC(row) rows;
    HK_VEC(hk_node_id) captions;
    uint32_t ncols;
} table_grid;

static uint32_t span_attr(const hk_document *d, hk_node_id el, hk_atom name)
{
    const hk_attr *a = hk_dom_attr(d, el, HK_ATTR_NS_NONE, name);
    if (!a) return 1;
    uint32_t v = 0;
    for (size_t i = 0; i < a->value.n && a->value.p[i] >= '0' && a->value.p[i] <= '9' && v < 1000; i++)
        v = v * 10 + (uint32_t)(a->value.p[i] - '0');
    return v ? v : 1;
}

static void add_row(ctx *c, table_grid *g, hk_node_id tr, const hk_style *rs, u32_vec *occupied)
{
    row r = {tr, rs, 0, 0, 0};
    uint32_t ri = g->rows.len;
    hk_vec_push(&g->rows, r);
    uint32_t col = 0;
    for (hk_node_id ch = tr ? hk_dom_node(c->d, tr)->first_child : HK_NODE_NONE; ch; ch = hk_dom_node(c->d, ch)->next) {
        if (hk_dom_node(c->d, ch)->type != HK_NODE_ELEMENT) continue;
        const hk_style *cs = hk_style_of(c->st, ch);
        if (!cs || cs->display != HK_DISPLAY_TABLE_CELL) continue;
        /* Skip columns still covered by row-spanning cells from earlier rows. */
        while (col < occupied->len && occupied->data[col] > ri) col++;
        cell k = {0};
        k.el = ch;
        k.s = cs;
        k.row = ri;
        k.col = col;
        k.colspan = span_attr(c->d, ch, A(colspan));
        k.rowspan = span_attr(c->d, ch, A(rowspan));
        if (k.colspan > 100) k.colspan = 100;
        hk_vec_push(&g->cells, k);
        for (uint32_t i = 0; i < k.colspan; i++) {
            while (occupied->len <= col + i) hk_vec_push(occupied, 0);
            occupied->data[col + i] = ri + k.rowspan;
        }
        col += k.colspan;
        if (col > g->ncols) g->ncols = col;
    }
}

static void build_grid(ctx *c, hk_node_id table, table_grid *g)
{
    u32_vec occupied = {0};
    for (hk_node_id ch = hk_dom_node(c->d, table)->first_child; ch; ch = hk_dom_node(c->d, ch)->next) {
        if (hk_dom_node(c->d, ch)->type != HK_NODE_ELEMENT) continue;
        const hk_style *s = hk_style_of(c->st, ch);
        if (!s || s->display == HK_DISPLAY_NONE) continue;
        if (s->display == HK_DISPLAY_TABLE_CAPTION) {
            hk_vec_push(&g->captions, ch);
        } else if (s->display == HK_DISPLAY_TABLE_ROW_GROUP) {
            for (hk_node_id tr = hk_dom_node(c->d, ch)->first_child; tr; tr = hk_dom_node(c->d, tr)->next) {
                if (hk_dom_node(c->d, tr)->type != HK_NODE_ELEMENT) continue;
                const hk_style *rs = hk_style_of(c->st, tr);
                if (rs && rs->display == HK_DISPLAY_TABLE_ROW) add_row(c, g, tr, rs, &occupied);
            }
        } else if (s->display == HK_DISPLAY_TABLE_ROW) {
            add_row(c, g, ch, s, &occupied);
        }
    }
    hk_vec_free(&occupied);
}

static void free_grid(table_grid *g)
{
    hk_vec_free(&g->cells);
    hk_vec_free(&g->rows);
    hk_vec_free(&g->captions);
}

/* Column minimum and maximum widths (border boxes of the cells). */
static void column_widths(ctx *c, table_grid *g, float *cmin, float *cmax, float *cfixed)
{
    for (uint32_t i = 0; i < g->ncols; i++) cmin[i] = cmax[i] = cfixed[i] = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < g->cells.len; i++) {
            cell *k = &g->cells.data[i];
            if ((pass == 0) != (k->colspan == 1)) continue;
            if (pass == 0) intrinsic(c, k->el, k->s, &k->mn, &k->mx);
            if (pass == 1) intrinsic(c, k->el, k->s, &k->mn, &k->mx);
            if (k->colspan == 1) {
                if (k->mn > cmin[k->col]) cmin[k->col] = k->mn;
                if (k->mx > cmax[k->col]) cmax[k->col] = k->mx;
                if (k->s->width.unit == HK_LEN_PX && k->mx > cfixed[k->col]) cfixed[k->col] = k->mx;
                continue;
            }
            float smin = 0, smax = 0;
            for (uint32_t j = 0; j < k->colspan; j++) {
                smin += cmin[k->col + j];
                smax += cmax[k->col + j];
            }
            if (k->mn > smin) {
                float extra = (k->mn - smin) / (float)k->colspan;
                for (uint32_t j = 0; j < k->colspan; j++) cmin[k->col + j] += extra;
            }
            if (k->mx > smax) {
                float extra = (k->mx - smax) / (float)k->colspan;
                for (uint32_t j = 0; j < k->colspan; j++) cmax[k->col + j] += extra;
            }
        }
    }
    for (uint32_t i = 0; i < g->ncols; i++) {
        if (cmax[i] < cmin[i]) cmax[i] = cmin[i];
    }
}

static void table_intrinsic(ctx *c, hk_node_id el, const hk_style *s, float *mn, float *mx)
{
    table_grid g = {0};
    build_grid(c, el, &g);
    float spacing = s->border_collapse ? 0 : s->border_spacing;
    float frame = s->border_width[1] + s->border_width[3] + px_or(s->padding[1], 0, 0) + px_or(s->padding[3], 0, 0);
    float *cmin = hk_alloc_zero((g.ncols + 1) * 3 * sizeof(float));
    float *cmax = cmin + g.ncols + 1, *cfixed = cmax + g.ncols + 1;
    column_widths(c, &g, cmin, cmax, cfixed);
    float smin = spacing * (float)(g.ncols + 1), smax = smin;
    for (uint32_t i = 0; i < g.ncols; i++) {
        smin += cmin[i];
        smax += cmax[i];
    }
    hk_free(cmin, (g.ncols + 1) * 3 * sizeof(float));
    free_grid(&g);
    *mn = smin + frame;
    *mx = smax + frame;
    if (s->width.unit == HK_LEN_PX && s->width.px > *mn) *mn = *mx = s->width.px;
}

static void table_box(ctx *c, hk_node_id el, const hk_style *s, flow *parent)
{
    table_grid g = {0};
    build_grid(c, el, &g);
    float cbw = parent->w;
    float bt = s->border_width[0], br = s->border_width[1], bb = s->border_width[2], bl = s->border_width[3];
    float pt = px_or(s->padding[0], cbw, 0), pr = px_or(s->padding[1], cbw, 0);
    float pb = px_or(s->padding[2], cbw, 0), pl = px_or(s->padding[3], cbw, 0);
    float frame = bl + br + pl + pr;
    float spacing = s->border_collapse ? 0 : s->border_spacing;
    float ml = px_or(s->margin[3], cbw, 0), mr = px_or(s->margin[1], cbw, 0);
    float mt = px_or(s->margin[0], cbw, 0), mb = px_or(s->margin[2], cbw, 0);
    bool ml_auto = s->margin[3].unit == HK_LEN_AUTO, mr_auto = s->margin[1].unit == HK_LEN_AUTO;

    uint32_t nc = g.ncols ? g.ncols : 1;
    float *cmin = hk_alloc_zero((nc + 1) * 4 * sizeof(float));
    float *cmax = cmin + nc + 1, *cfixed = cmax + nc + 1, *cw = cfixed + nc + 1;
    column_widths(c, &g, cmin, cmax, cfixed);
    float sum_min = 0, sum_max = 0;
    for (uint32_t i = 0; i < g.ncols; i++) {
        sum_min += cmin[i];
        sum_max += cmax[i];
    }
    float gaps = spacing * (float)(g.ncols + 1);
    float avail = cbw - ml - mr - frame;
    float content_w;
    if (s->width.unit != HK_LEN_AUTO) {
        content_w = px_or(s->width, cbw, 0) - (s->border_box ? frame : 0);
        if (content_w < sum_min + gaps) content_w = sum_min + gaps;
    } else {
        content_w = sum_max + gaps;
        if (content_w > avail) content_w = avail;
        if (content_w < sum_min + gaps) content_w = sum_min + gaps;
    }
    /* Distribute the column space between the minimum and maximum widths. */
    float space = content_w - gaps;
    if (space >= sum_max) {
        float extra = space - sum_max;
        float flexible = 0;
        for (uint32_t i = 0; i < g.ncols; i++) flexible += cfixed[i] > 0 ? 0 : cmax[i];
        for (uint32_t i = 0; i < g.ncols; i++) {
            cw[i] = cmax[i];
            if (extra > 0) {
                if (flexible > 0) cw[i] += cfixed[i] > 0 ? 0 : extra * cmax[i] / flexible;
                else cw[i] += extra / (float)g.ncols;
            }
        }
    } else {
        float f = sum_max > sum_min ? (space - sum_min) / (sum_max - sum_min) : 0;
        if (f < 0) f = 0;
        for (uint32_t i = 0; i < g.ncols; i++) cw[i] = cmin[i] + (cmax[i] - cmin[i]) * f;
    }
    float rest = cbw - content_w - frame;
    if (parent->style->text_align == HK_ALIGN_LEGACY_CENTER && !ml_auto && !mr_auto && ml == 0 && mr == 0)
        ml_auto = mr_auto = true;
    if (ml_auto && mr_auto) ml = mr = rest > 0 ? rest / 2 : 0;
    else if (ml_auto) ml = rest - mr;
    float x = parent->x + ml;

    strut_add(&parent->s, mt);
    float top = parent->cursor + strut_value(&parent->s);
    parent->s = (strut){0, 0};
    if (!parent->placed) {
        parent->placed = true;
        parent->first_top = top;
    }
    uint32_t items_start = c->dl->items.len, links_start = c->dl->links.len;
    uint32_t bg_index = c->dl->items.len;
    for (int k = 0; k < 5; k++) push_item(c, HK_ITEM_RECT);
    float y = top + bt + pt;

    /* Captions above the grid. */
    for (uint32_t i = 0; i < g.captions.len; i++) {
        flow cf = {0};
        cf.c = c;
        cf.style = s;
        cf.x = x + bl + pl;
        cf.w = content_w;
        cf.cursor = y;
        const hk_style *cs = hk_style_of(c->st, g.captions.data[i]);
        block_box(c, g.captions.data[i], cs, &cf);
        y = cf.cursor + strut_value(&cf.s);
        hk_vec_free(&cf.text);
        hk_vec_free(&cf.ranges);
        hk_vec_free(&cf.boxes);
    }

    float *col_x = hk_alloc_zero((nc + 1) * sizeof(float));
    float cx = x + bl + pl + spacing;
    for (uint32_t i = 0; i < g.ncols; i++) {
        col_x[i] = cx;
        cx += cw[i] + spacing;
    }
    col_x[g.ncols] = cx;
    y += spacing;
    uint32_t next_cell = 0;
    for (uint32_t ri = 0; ri < g.rows.len; ri++) {
        row *r = &g.rows.data[ri];
        r->top = y;
        r->bg_index = c->dl->items.len;
        push_item(c, HK_ITEM_RECT);
        float height = r->s && r->s->height.unit == HK_LEN_PX ? r->s->height.px : 0;
        for (; next_cell < g.cells.len && g.cells.data[next_cell].row == ri; next_cell++) {
            cell *k = &g.cells.data[next_cell];
            float w = col_x[k->col + k->colspan > g.ncols ? g.ncols : k->col + k->colspan] - col_x[k->col] - spacing;
            const hk_style *cs = k->s;
            float cframe = cs->border_width[1] + cs->border_width[3] + px_or(cs->padding[1], w, 0) + px_or(cs->padding[3], w, 0);
            k->items_start = c->dl->items.len;
            k->links_start = c->dl->links.len;
            k->bg_index = c->dl->items.len;
            strut st = {0, 0};
            bool placed;
            float ctop;
            float bottom = box_contents(c, k->el, cs, col_x[k->col], y, w - cframe, true, &st, &placed, &ctop);
            k->items_end = c->dl->items.len;
            k->links_end = c->dl->links.len;
            k->top = ctop;
            k->height = bottom - ctop;
            if (cs->height.unit == HK_LEN_PX && cs->height.px > k->height) k->height = cs->height.px;
            if (k->rowspan == 1 && k->height > height) height = k->height;
        }
        r->height = height;
        y += height + spacing;
    }
    /* Row-spanning cells may need more height in their last row. */
    for (uint32_t i = 0; i < g.cells.len; i++) {
        cell *k = &g.cells.data[i];
        if (k->rowspan <= 1) continue;
        uint32_t last = k->row + k->rowspan - 1;
        if (last >= g.rows.len) last = g.rows.len - 1;
        float span = g.rows.data[last].top + g.rows.data[last].height - g.rows.data[k->row].top;
        if (k->height > span) {
            float extra = k->height - span;
            g.rows.data[last].height += extra;
            for (uint32_t j = last + 1; j < g.rows.len; j++) g.rows.data[j].top += extra;
            y += extra;
        }
    }
    /* Stretch the cell boxes to their rows and align their content vertically. */
    for (uint32_t i = 0; i < g.cells.len; i++) {
        cell *k = &g.cells.data[i];
        uint32_t last = k->row + k->rowspan - 1;
        if (last >= g.rows.len) last = g.rows.len - 1;
        float row_top = g.rows.data[k->row].top;
        float avail_h = g.rows.data[last].top + g.rows.data[last].height - row_top;
        float shift_rows = row_top - k->top;
        uint8_t va = k->s->valign ? k->s->valign : (g.rows.data[k->row].s && g.rows.data[k->row].s->valign ? g.rows.data[k->row].s->valign : 2);
        float dy = va == 2 ? (avail_h - k->height) / 2 : va == 3 ? avail_h - k->height : 0;
        if (dy < 0) dy = 0;
        /* Move the content (not the cell's own background/border slots, which are resized). */
        translate(c, k->bg_index + 5, k->items_end, k->links_start, k->links_end, 0, shift_rows + dy);
        for (int q = 0; q < 5; q++) {
            hk_item *it = &c->dl->items.data[k->bg_index + q];
            if (!it->w && !it->h) continue;
            it->y += shift_rows;
            if (q == 0 || q == 2 || q == 4) {
                /* background, right and left borders span the full height */
            }
        }
        hk_item *bgi = &c->dl->items.data[k->bg_index];
        if (bgi->w) bgi->h = avail_h;
        hk_item *right = &c->dl->items.data[k->bg_index + 2], *left = &c->dl->items.data[k->bg_index + 4];
        if (right->w) right->h = avail_h;
        if (left->w) left->h = avail_h;
        hk_item *bottom_b = &c->dl->items.data[k->bg_index + 3];
        if (bottom_b->h) bottom_b->y = row_top + avail_h - bottom_b->h;
    }
    /* Row backgrounds. */
    for (uint32_t ri = 0; ri < g.rows.len; ri++) {
        row *r = &g.rows.data[ri];
        if (!r->s || (r->s->background >> 24) == 0) continue;
        hk_item *it = &c->dl->items.data[r->bg_index];
        it->color = r->s->background;
        it->x = col_x[0];
        it->y = r->top;
        it->w = col_x[g.ncols] - col_x[0] - spacing;
        it->h = r->height;
    }
    float bottom = y + pb + bb;
    if (s->height.unit == HK_LEN_PX && bottom - top < s->height.px) bottom = top + s->height.px;
    float bw = content_w + frame, bh = bottom - top;
    if (s->visible) {
        hk_item *bg = &c->dl->items.data[bg_index];
        if (s->background >> 24) {
            bg->color = s->background;
            bg->x = x;
            bg->y = top;
            bg->w = bw;
            bg->h = bh;
        }
        float rects[4][4] = {{x, top, bw, bt}, {x + bw - br, top, br, bh}, {x, bottom - bb, bw, bb}, {x, top, bl, bh}};
        for (int k = 0; k < 4; k++) {
            hk_item *bd = &c->dl->items.data[bg_index + 1 + k];
            if (s->border_width[k] <= 0 || (s->border_color[k] >> 24) == 0) continue;
            bd->color = s->border_color[k];
            bd->x = rects[k][0];
            bd->y = rects[k][1];
            bd->w = rects[k][2];
            bd->h = rects[k][3];
        }
    }
    if (s->position == HK_POS_RELATIVE) {
        float dx = s->inset[3].unit != HK_LEN_AUTO ? px_or(s->inset[3], cbw, 0) : 0;
        float dy = s->inset[0].unit != HK_LEN_AUTO ? px_or(s->inset[0], 0, 0) : 0;
        translate(c, items_start, c->dl->items.len, links_start, c->dl->links.len, dx, dy);
    }
    parent->cursor = bottom;
    strut_add(&parent->s, mb);
    hk_free(col_x, (nc + 1) * sizeof(float));
    hk_free(cmin, (nc + 1) * 4 * sizeof(float));
    free_grid(&g);
}

/* --- document ------------------------------------------------------------------------------------- */

static hk_node_id child_element(const hk_document *d, hk_node_id parent, hk_atom name)
{
    for (hk_node_id c = hk_dom_node(d, parent)->first_child; c; c = hk_dom_node(d, c)->next) {
        if (hk_dom_is_html(d, c, name)) return c;
    }
    return HK_NODE_NONE;
}

void hk_layout_document(const hk_document *doc, const hk_styler *st, float viewport_width, float viewport_height,
                        const hk_layout_hooks *hooks, hk_display_list *out)
{
    memset(out, 0, sizeof *out);
    ctx c = {0};
    c.d = doc;
    c.st = st;
    c.dl = out;
    c.vw = viewport_width;
    c.vh = viewport_height;
    c.hooks = hooks;
    abs_vec root_abs = {0};
    c.abs = &root_abs;
    out->width = viewport_width;
    out->canvas = 0xFFFFFFFFu;
    hk_node_id html = child_element(doc, doc->root, A(html));
    if (!html) return;
    const hk_style *hs = hk_style_of(st, html);
    hk_node_id body = child_element(doc, html, A(body));
    const hk_style *bs = body ? hk_style_of(st, body) : NULL;
    if (hs && (hs->background >> 24)) out->canvas = hs->background;
    else if (bs && (bs->background >> 24)) out->canvas = bs->background;
    if ((out->canvas >> 24) != 0xFF) out->canvas = 0xFFFFFFFFu;

    hk_style root_flow = {0};
    root_flow.font_size = 16;
    root_flow.color = 0xFF000000u;
    root_flow.visible = 1;
    flow f = {0};
    f.c = &c;
    f.style = &root_flow;
    f.w = viewport_width;
    if (hs && hs->display != HK_DISPLAY_NONE) block_box(&c, html, hs, &f);
    float height = f.cursor + strut_value(&f.s);
    float fb = floats_bottom(&c, 0);
    if (fb > height) height = fb;
    /* The initial containing block for absolutely positioned boxes is the first screen. */
    cb_rect icb = {0, 0, viewport_width, viewport_height};
    for (uint32_t i = 0; i < root_abs.len; i++) layout_absolute(&c, &root_abs.data[i], icb, false);
    for (uint32_t i = 0; i < c.fixed.len; i++) layout_absolute(&c, &c.fixed.data[i], icb, true);
    out->height = height;
    hk_vec_free(&root_abs);
    hk_vec_free(&c.fixed);
    hk_vec_free(&c.floats);
    hk_vec_free(&f.text);
    hk_vec_free(&f.ranges);
    hk_vec_free(&f.boxes);
}

void hk_display_list_free(hk_display_list *dl)
{
    for (uint32_t i = 0; i < dl->items.len; i++) {
        hk_item *it = &dl->items.data[i];
        if (it->kind == HK_ITEM_GLYPHS) free_run(&it->run);
    }
    hk_vec_free(&dl->items);
    hk_vec_free(&dl->links);
    memset(dl, 0, sizeof *dl);
}

const hk_item *hk_display_list_control(const hk_display_list *dl, hk_node_id el)
{
    for (uint32_t i = 0; i < dl->items.len; i++)
        if (dl->items.data[i].kind == HK_ITEM_CONTROL && dl->items.data[i].node == el) return &dl->items.data[i];
    return NULL;
}

hk_node_id hk_display_list_link_at(const hk_display_list *dl, float x, float y, float scroll)
{
    for (uint32_t i = dl->links.len; i-- > 0;) {
        const hk_link_rect *r = &dl->links.data[i];
        float yy = r->fixed ? y : y + scroll;
        if (x >= r->x && x < r->x + r->w && yy >= r->y && yy < r->y + r->h) return r->link;
    }
    return HK_NODE_NONE;
}
