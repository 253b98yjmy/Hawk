/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "layout/hk_layout.h"
#include "base/hk_mem.h"
#include "base/hk_utf8.h"

#include <math.h>
#include <string.h>

#define A(x) HK_ATOM_##x

/* --- margin collapsing ----------------------------------------------------------------- */

typedef struct strut {
    float pos, neg; /* largest positive and most negative adjoining margins */
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

/* --- context ---------------------------------------------------------------------------- */

typedef struct range {
    uint32_t start, len;
    const hk_style *style;
    hk_node_id link;
} range;

typedef HK_VEC(uint16_t) u16_vec;
typedef HK_VEC(range) range_vec;

typedef struct ctx {
    const hk_document *d;
    const hk_styler *st;
    hk_display_list *dl;
    float viewport;
} ctx;

/* A block container's flow of children. */
typedef struct flow {
    ctx *c;
    const hk_style *style; /* the container */
    float x, w;            /* content box */
    float cursor;          /* bottom border edge of the last placed box */
    strut s;               /* margins after the cursor, not yet resolved */
    bool placed;           /* something was placed (determines the container's top) */
    float first_top;       /* where the first placed thing landed */
    /* pending inline content */
    u16_vec text;
    range_vec ranges;
    bool space_pending;
    bool has_content;
} flow;

static void block_box(ctx *c, hk_node_id el, const hk_style *s, flow *parent);

static bool is_block_level(uint8_t display)
{
    return display == HK_DISPLAY_BLOCK || display == HK_DISPLAY_LIST_ITEM || display == HK_DISPLAY_TABLE ||
           display == HK_DISPLAY_TABLE_ROW_GROUP || display == HK_DISPLAY_TABLE_ROW ||
           display == HK_DISPLAY_TABLE_CELL || display == HK_DISPLAY_TABLE_CAPTION || display == HK_DISPLAY_FLEX ||
           display == HK_DISPLAY_GRID;
}

/* --- display list helpers ----------------------------------------------------------------- */

static void emit_rect(ctx *c, float x, float y, float w, float h, uint32_t color)
{
    if ((color >> 24) == 0 || w <= 0 || h <= 0) return;
    hk_item it = {0};
    it.kind = HK_ITEM_RECT;
    it.color = color;
    it.x = x;
    it.y = y;
    it.w = w;
    it.h = h;
    hk_vec_push(&c->dl->items, it);
}

typedef struct place_ctx {
    ctx *c;
    float x, y;
    bool visible;
} place_ctx;

static void on_run(void *user, const hk_glyph_run *r)
{
    place_ctx *p = user;
    if (!p->visible || (r->color >> 24) == 0) {
        hk_font_face_release(r->face);
        hk_free(r->glyphs, r->count * sizeof(uint16_t));
        hk_free(r->advances, r->count * sizeof(float));
        hk_free(r->offsets, r->count * 2 * sizeof(float));
        return;
    }
    hk_item it = {0};
    it.kind = HK_ITEM_GLYPHS;
    it.color = r->color;
    it.run = *r;
    it.run.x += p->x;
    it.run.baseline += p->y;
    float w = 0;
    for (uint32_t i = 0; i < r->count; i++) w += r->advances[i];
    it.x = r->rtl ? it.run.x - w : it.run.x;
    it.w = w;
    it.y = it.run.baseline - r->size * 1.2f;
    it.h = r->size * 1.6f;
    hk_vec_push(&p->c->dl->items, it);
}

static void on_deco(void *user, const hk_text_rect *r)
{
    place_ctx *p = user;
    if (p->visible) emit_rect(p->c, p->x + r->x, p->y + r->y, r->w, r->h, r->color);
}

/* --- inline content ----------------------------------------------------------------------- */

static bool is_space16(uint16_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static void push_text(flow *f, const hk_style *s, hk_node_id link, hk_str utf8)
{
    uint32_t start = f->text.len;
    bool collapse = s->white_space == HK_WS_NORMAL || s->white_space == HK_WS_NOWRAP || s->white_space == HK_WS_PRE_LINE;
    bool keep_newlines = s->white_space != HK_WS_NORMAL && s->white_space != HK_WS_NOWRAP;
    size_t i = 0;
    bool word_start = true;
    while (i < utf8.n) {
        uint32_t cp;
        i += hk_utf8_decode((const uint8_t *)utf8.p + i, utf8.n - i, &cp);
        if (collapse && cp < 0x80 && is_space16((uint16_t)cp) && !(keep_newlines && cp == '\n')) {
            f->space_pending = true;
            word_start = true;
            continue;
        }
        if (f->space_pending) {
            if (f->has_content) hk_vec_push(&f->text, (uint16_t)' ');
            f->space_pending = false;
        }
        if (cp < 0x80 && s->text_transform != HK_TT_NONE) {
            if (s->text_transform == HK_TT_UPPERCASE || (s->text_transform == HK_TT_CAPITALIZE && word_start)) {
                if (cp >= 'a' && cp <= 'z') cp -= 32;
            } else if (s->text_transform == HK_TT_LOWERCASE) {
                if (cp >= 'A' && cp <= 'Z') cp += 32;
            }
        }
        word_start = cp < 0x80 && is_space16((uint16_t)cp);
        if (cp == '\n' && keep_newlines) {
            hk_vec_push(&f->text, (uint16_t)0x2028); /* line separator: breaks the line, not the paragraph */
            f->has_content = true;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            hk_vec_push(&f->text, (uint16_t)(0xD800 | (cp >> 10)));
            hk_vec_push(&f->text, (uint16_t)(0xDC00 | (cp & 0x3FF)));
        } else {
            hk_vec_push(&f->text, (uint16_t)cp);
        }
        f->has_content = true;
    }
    if (f->text.len > start) {
        range r = {start, f->text.len - start, s, link};
        hk_vec_push(&f->ranges, r);
    }
}

static void text_style(ctx *c, const hk_style *s, hk_node_id link, uint32_t user, hk_text_style *out)
{
    HK_UNUSED(link);
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

typedef struct link_ctx {
    ctx *c;
    float x, y;
    hk_node_id link;
} link_ctx;

static void on_link_rect(void *user, const hk_text_rect *r)
{
    link_ctx *l = user;
    hk_link_rect lr = {l->x + r->x, l->y + r->y, r->w, r->h, l->link};
    hk_vec_push(&l->c->dl->links, lr);
}

/* Places the pending inline content as one paragraph. */
static void flush_inline(flow *f)
{
    if (!f->has_content || f->text.len == 0) {
        f->text.len = f->ranges.len = 0;
        f->space_pending = false;
        f->has_content = false;
        return;
    }
    ctx *c = f->c;
    /* Trailing collapsible space was never emitted (space_pending); leading space is dropped by has_content. */
    const hk_style *cs = f->style;
    hk_text_para para = {0};
    para.max_width = f->w > 1 ? f->w : 1;
    para.line_height = cs->line_height;
    para.wrap = cs->white_space != HK_WS_NOWRAP && cs->white_space != HK_WS_PRE;
    switch (cs->text_align) {
    case HK_ALIGN_RIGHT:
    case HK_ALIGN_END: para.align = HK_TEXT_RIGHT; break;
    case HK_ALIGN_CENTER: para.align = HK_TEXT_CENTER; break;
    case HK_ALIGN_JUSTIFY: para.align = HK_TEXT_JUSTIFY; break;
    default: para.align = HK_TEXT_LEFT; break;
    }
    hk_text_style base;
    text_style(c, cs, HK_NODE_NONE, 0, &base);
    hk_text_layout *l = hk_text_layout_create(f->text.data, f->text.len, &para, &base);
    if (l) {
        for (uint32_t i = 0; i < f->ranges.len; i++) {
            const range *r = &f->ranges.data[i];
            hk_text_style ts;
            text_style(c, r->style, r->link, r->link, &ts);
            hk_text_layout_set_style(l, r->start, r->len, &ts);
        }
        float w, h;
        hk_text_layout_size(l, &w, &h);
        float y = f->cursor + strut_value(&f->s);
        f->s = (strut){0, 0};
        if (!f->placed) {
            f->placed = true;
            f->first_top = y;
        }
        place_ctx pc = {c, f->x, y, true};
        hk_text_layout_runs(l, on_run, on_deco, &pc);
        /* Link hit areas: consecutive ranges of the same link are merged. */
        for (uint32_t i = 0; i < f->ranges.len;) {
            hk_node_id link = f->ranges.data[i].link;
            uint32_t j = i + 1;
            while (j < f->ranges.len && f->ranges.data[j].link == link) j++;
            if (link) {
                uint32_t start = f->ranges.data[i].start;
                uint32_t end = f->ranges.data[j - 1].start + f->ranges.data[j - 1].len;
                link_ctx lc = {c, f->x, y, link};
                hk_text_layout_range_rects(l, start, end - start, on_link_rect, &lc);
            }
            i = j;
        }
        f->cursor = y + h;
        hk_text_layout_destroy(l);
    }
    f->text.len = f->ranges.len = 0;
    f->space_pending = false;
    f->has_content = false;
}

/* --- the flow ---------------------------------------------------------------------------------- */

static hk_node_id link_of(const hk_document *d, hk_node_id el, hk_node_id inherited)
{
    if (hk_dom_is_html(d, el, A(a)) && hk_dom_attr(d, el, HK_ATTR_NS_NONE, A(href))) return el;
    return inherited;
}

static void flow_children(flow *f, hk_node_id parent, hk_node_id link);

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
    if (!s || s->display == HK_DISPLAY_NONE) return;
    if (s->position == HK_POS_ABSOLUTE || s->position == HK_POS_FIXED) return; /* positioning: M3 */
    hk_node_id lk = link_of(c->d, node, link);
    if (hk_dom_is_html(c->d, node, A(br))) {
        /* A forced line break: U+2028 breaks the line without starting a new paragraph. */
        uint32_t start = f->text.len;
        hk_vec_push(&f->text, (uint16_t)0x2028);
        range r = {start, 1, s, lk};
        hk_vec_push(&f->ranges, r);
        f->has_content = true;
        f->space_pending = false;
        return;
    }
    if (is_block_level(s->display)) {
        flush_inline(f);
        block_box(c, node, s, f);
        return;
    }
    flow_children(f, node, lk); /* inline, inline-block and contents: flattened into the paragraph */
}

static void flow_children(flow *f, hk_node_id parent, hk_node_id link)
{
    const hk_document *d = f->c->d;
    for (hk_node_id ch = hk_dom_node(d, parent)->first_child; ch; ch = hk_dom_node(d, ch)->next) flow_node(f, ch, link);
}

/* --- list markers --------------------------------------------------------------------------- */

static void emit_marker(ctx *c, hk_node_id li, const hk_style *s, float content_x, float top)
{
    if (s->list_style == HK_LIST_NONE || !s->visible) return;
    uint16_t buf[16];
    uint32_t n = 0;
    if (s->list_style == HK_LIST_DECIMAL) {
        int index = 1;
        for (hk_node_id p = hk_dom_node(c->d, li)->prev; p; p = hk_dom_node(c->d, p)->prev) {
            if (hk_dom_node(c->d, p)->type != HK_NODE_ELEMENT) continue;
            const hk_style *ps = hk_style_of(c->st, p);
            if (ps && ps->display == HK_DISPLAY_LIST_ITEM) index++;
        }
        char tmp[12];
        int k = 0;
        do {
            tmp[k++] = (char)('0' + index % 10);
            index /= 10;
        } while (index && k < 10);
        while (k) buf[n++] = (uint16_t)tmp[--k];
        buf[n++] = '.';
    } else {
        buf[n++] = s->list_style == HK_LIST_CIRCLE ? 0x25E6 : s->list_style == HK_LIST_SQUARE ? 0x25AA : 0x2022;
    }
    buf[n++] = ' ';
    hk_text_para para = {0};
    para.max_width = 200;
    hk_text_style ts;
    text_style(c, s, HK_NODE_NONE, 0, &ts);
    ts.underline = ts.line_through = false;
    hk_text_layout *l = hk_text_layout_create(buf, n, &para, &ts);
    if (!l) return;
    float w, h;
    hk_text_layout_size(l, &w, &h);
    float line = s->line_height > 0 ? s->line_height : 0;
    float y = top + (line > 0 ? (line - h) / 2 : 0);
    place_ctx pc = {c, content_x - w, y, true};
    hk_text_layout_runs(l, on_run, NULL, &pc);
    hk_text_layout_destroy(l);
}

/* --- block boxes -------------------------------------------------------------------------------- */

static void block_box(ctx *c, hk_node_id el, const hk_style *s, flow *parent)
{
    float cbw = parent->w;
    float bt = s->border_width[0], br = s->border_width[1], bb = s->border_width[2], bl = s->border_width[3];
    float pt = hk_length_resolve(s->padding[0], cbw, 0), pr = hk_length_resolve(s->padding[1], cbw, 0);
    float pb = hk_length_resolve(s->padding[2], cbw, 0), pl = hk_length_resolve(s->padding[3], cbw, 0);
    bool ml_auto = s->margin[3].unit == HK_LEN_AUTO, mr_auto = s->margin[1].unit == HK_LEN_AUTO;
    float mt = hk_length_resolve(s->margin[0], cbw, 0), mb = hk_length_resolve(s->margin[2], cbw, 0);
    float ml = hk_length_resolve(s->margin[3], cbw, 0), mr = hk_length_resolve(s->margin[1], cbw, 0);
    float frame = bl + br + pl + pr;

    /* Width (CSS 2.1 10.3.3). */
    float content_w;
    if (s->width.unit == HK_LEN_AUTO) {
        content_w = cbw - ml - mr - frame;
    } else {
        content_w = hk_length_resolve(s->width, cbw, 0);
        if (s->border_box) content_w -= frame;
    }
    if (s->max_width.unit != HK_LEN_NONE) {
        float mx = hk_length_resolve(s->max_width, cbw, content_w);
        if (s->border_box) mx -= frame;
        if (content_w > mx) content_w = mx;
    }
    if (s->min_width.unit != HK_LEN_AUTO) {
        float mn = hk_length_resolve(s->min_width, cbw, 0);
        if (s->border_box) mn -= frame;
        if (content_w < mn) content_w = mn;
    }
    if (content_w < 0) content_w = 0;
    float rest = cbw - content_w - frame;
    if (s->width.unit != HK_LEN_AUTO || s->max_width.unit != HK_LEN_NONE) {
        if (ml_auto && mr_auto) ml = mr = rest > 0 ? rest / 2 : 0;
        else if (ml_auto) ml = rest - mr;
        else if (mr_auto) mr = rest - ml;
    }
    float x = parent->x + ml;

    /* Vertical position with margin collapsing. */
    strut_add(&parent->s, mt);
    bool top_sep = bt + pt > 0 || s->overflow_hidden;
    float y_in = parent->cursor;
    float top = 0;
    flow f = {0};
    f.c = c;
    f.style = s;
    f.x = x + bl + pl;
    f.w = content_w;
    if (top_sep) {
        top = y_in + strut_value(&parent->s);
        parent->s = (strut){0, 0};
        f.cursor = top + bt + pt;
        f.placed = true;
        f.first_top = top;
    } else {
        f.cursor = y_in;
        f.s = parent->s; /* the first child's top margin collapses with ours */
        parent->s = (strut){0, 0};
    }

    /* Reserve the background slot so it is painted under the content. */
    uint32_t bg_index = c->dl->items.len;
    hk_item placeholder = {0};
    placeholder.kind = HK_ITEM_RECT;
    hk_vec_push(&c->dl->items, placeholder);
    uint32_t border_index = c->dl->items.len;
    for (int k = 0; k < 4; k++) hk_vec_push(&c->dl->items, placeholder);

    flow_children(&f, el, HK_NODE_NONE);
    flush_inline(&f);

    if (!top_sep) top = f.placed ? f.first_top : f.cursor + strut_value(&f.s);
    float bottom;
    bool fixed_height = s->height.unit == HK_LEN_PX;
    if (fixed_height) {
        float h = s->height.px;
        if (s->border_box) h -= bt + bb + pt + pb;
        if (h < 0) h = 0;
        bottom = top + bt + pt + h + pb + bb;
        parent->s = (strut){0, 0};
    } else if (bb + pb > 0 || s->overflow_hidden || !f.placed) {
        float content_bottom = f.placed ? f.cursor + strut_value(&f.s) : top + bt + pt;
        if (!f.placed && !top_sep) {
            /* Empty box: its margins collapse through it. */
            content_bottom = top;
            parent->s = f.s;
        } else {
            parent->s = (strut){0, 0};
        }
        if (s->min_height.unit == HK_LEN_PX && content_bottom - (top + bt + pt) < s->min_height.px)
            content_bottom = top + bt + pt + s->min_height.px;
        bottom = content_bottom + pb + bb;
    } else {
        bottom = f.cursor;
        parent->s = f.s; /* the last child's bottom margin collapses with ours */
        if (s->min_height.unit == HK_LEN_PX && bottom - top < s->min_height.px) {
            bottom = top + s->min_height.px;
            parent->s = (strut){0, 0};
        }
    }
    strut_add(&parent->s, mb);
    parent->cursor = bottom;
    if (!parent->placed) {
        parent->placed = true;
        parent->first_top = top;
    }

    /* Fill the reserved background and border slots. */
    float bw = content_w + frame, bh = bottom - top;
    if (s->visible) {
        hk_item *bg = &c->dl->items.data[bg_index];
        if ((s->background >> 24) && bw > 0 && bh > 0) {
            bg->color = s->background;
            bg->x = x;
            bg->y = top;
            bg->w = bw;
            bg->h = bh;
        }
        hk_item *bd = &c->dl->items.data[border_index];
        float rects[4][4] = {{x, top, bw, bt}, {x + bw - br, top, br, bh}, {x, bottom - bb, bw, bb}, {x, top, bl, bh}};
        for (int k = 0; k < 4; k++) {
            if (s->border_width[k] <= 0 || (s->border_color[k] >> 24) == 0) continue;
            bd[k].color = s->border_color[k];
            bd[k].x = rects[k][0];
            bd[k].y = rects[k][1];
            bd[k].w = rects[k][2];
            bd[k].h = rects[k][3];
        }
    }
    if (s->display == HK_DISPLAY_LIST_ITEM) emit_marker(c, el, s, x + bl + pl, f.placed ? f.first_top : top);
    hk_vec_free(&f.text);
    hk_vec_free(&f.ranges);
}

/* --- document ------------------------------------------------------------------------------------ */

static hk_node_id child_element(const hk_document *d, hk_node_id parent, hk_atom name)
{
    for (hk_node_id c = hk_dom_node(d, parent)->first_child; c; c = hk_dom_node(d, c)->next) {
        if (hk_dom_is_html(d, c, name)) return c;
    }
    return HK_NODE_NONE;
}

void hk_layout_document(const hk_document *doc, const hk_styler *st, float viewport_width, hk_display_list *out)
{
    memset(out, 0, sizeof *out);
    ctx c = {doc, st, out, viewport_width};
    out->width = viewport_width;
    out->canvas = 0xFFFFFFFFu;
    hk_node_id html = child_element(doc, doc->root, A(html));
    if (!html) return;
    const hk_style *hs = hk_style_of(st, html);
    hk_node_id body = child_element(doc, html, A(body));
    const hk_style *bs = body ? hk_style_of(st, body) : NULL;
    /* Canvas background: the root's, else the body's (CSS Backgrounds 3, 2.11). */
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
    f.x = 0;
    f.w = viewport_width;
    if (hs && hs->display != HK_DISPLAY_NONE) block_box(&c, html, hs, &f);
    out->height = f.cursor + strut_value(&f.s);
    hk_vec_free(&f.text);
    hk_vec_free(&f.ranges);
}

void hk_display_list_free(hk_display_list *dl)
{
    for (uint32_t i = 0; i < dl->items.len; i++) {
        hk_item *it = &dl->items.data[i];
        if (it->kind != HK_ITEM_GLYPHS) continue;
        hk_font_face_release(it->run.face);
        hk_free(it->run.glyphs, it->run.count * sizeof(uint16_t));
        hk_free(it->run.advances, it->run.count * sizeof(float));
        hk_free(it->run.offsets, it->run.count * 2 * sizeof(float));
    }
    hk_vec_free(&dl->items);
    hk_vec_free(&dl->links);
    memset(dl, 0, sizeof *dl);
}

hk_node_id hk_display_list_link_at(const hk_display_list *dl, float x, float y)
{
    for (uint32_t i = dl->links.len; i-- > 0;) {
        const hk_link_rect *r = &dl->links.data[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return r->link;
    }
    return HK_NODE_NONE;
}
