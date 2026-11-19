/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Hawk application (M3): loads a page with its stylesheets, lays it out and paints it; form controls.
 *
 *   hawk.exe [options] [URL]
 *     --filters FILE      add a filter list (in addition to %LOCALAPPDATA%\Hawk\filters\*.txt)
 *     --width CSSPX       window client width in CSS px (for reproducible measurements)
 *     --exit-after-load   log load statistics and exit when the page is displayed
 *     --screenshot FILE   with --exit-after-load: save the window as a BMP before exiting
 *     --wait-images       with --exit-after-load: also wait for the images near the viewport
 *     --fill NAME=VALUE   after the first page loads: set the text control named NAME (repeatable)
 *     --submit            after --fill: submit the form of the last filled control
 *     --smoke             exit after the first frame (startup time and memory)
 */
#include "base/hk_log.h"
#include "base/hk_map64.h"
#include "base/hk_mem.h"
#include "base/hk_utf8.h"
#include "blocker/hk_filter_store.h"
#include "css/hk_css.h"
#include "dom/hk_dom.h"
#include "form/hk_form.h"
#include "form/hk_form_view.h"
#include "html/hk_html_parser.h"
#include "layout/hk_layout.h"
#include "net/hk_encoding.h"
#include "net/hk_loader.h"
#include "net/hk_url.h"
#include "paint/hk_paint.h"
#include "plat/hk_plat.h"
#include "plat/hk_window.h"
#include "style/hk_style.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SNIFF_BYTES 1024
#define MAX_IMPORT_DEPTH 4
#define A(x) HK_ATOM_##x

typedef struct app app;

/* One stylesheet of the page, in document order. */
typedef struct sheet_slot {
    app *app;
    uint32_t generation;
    char *url; /* NULL for <style> */
    size_t url_len;
    hk_charbuf css;
    bool done;
    int depth;
    uint32_t load_id;
} sheet_slot;

typedef struct page {
    uint32_t generation;
    char url[2048];
    uint32_t load_id;
    bool loading;
    bool html, plain, unsupported;
    hk_encoding transport, encoding;
    bool decoder_ready;
    hk_decoder decoder;
    hk_charbuf sniff, utf8, plain_text;
    hk_document *doc;
    hk_html_parser *parser;
    hk_styler *styler;
    HK_VEC(sheet_slot *) sheets;
    uint32_t pending_sheets;
    bool parsed;
    bool laid_out;
    hk_display_list dl;
    float layout_width;
    uint64_t t_start, t_first_byte, t_parsed, t_shown;
    size_t bytes, css_bytes;
    char content_type[128];
    hk_arena url_arena;
    hk_url base;
    bool has_base;
    HK_VEC(struct page_image) images;
    hk_map64 image_index; /* node -> index + 1 */
    bool relayout_pending;
    size_t image_bytes;
    uint32_t images_loaded;
    /* forms */
    hk_forms *forms; /* created on first use */
    hk_node_id focus;   /* the focused control */
    hk_node_id pressed; /* the clickable element under the mouse button */
    bool selecting;     /* dragging a selection in the focused text control */
    bool autofocus_done;
    uint16_t comp[256]; /* IME composition */
    uint32_t comp_len, comp_caret;
} page;

typedef enum { IMG_NONE, IMG_REQUESTED, IMG_READY, IMG_BROKEN } img_state;

typedef struct page_image {
    hk_node_id node;
    uint8_t state;
    bool sized_by_css; /* the layout size did not depend on the image */
    float density;     /* image pixels per CSS px (srcset x descriptors) */
    uint32_t load_id;
    hk_image img;
} page_image;

typedef struct history_entry {
    char url[2048];
    float scroll;
    char *post; /* the body of a form submission (POST), or NULL */
    size_t post_len;
} history_entry;

struct app {
    hk_window *window;
    hk_loader *loader;
    char data_dir[512]; /* empty when there is no per-user data directory */
    hk_painter *painter;
    page page;
    float scroll; /* CSS px */
    HK_VEC(history_entry) history;
    uint32_t history_pos; /* index of the current entry + 1 (0 = empty) */
    float restore_scroll;
    bool smoke, exit_after_load, wait_images;
    const char *screenshot;
    hk_node_id hover_link;
    hk_form_view *form_view;
    const char *fill[8]; /* --fill NAME=VALUE */
    int nfill;
    bool submit_after_fill;
};

static void navigate(app *a, const char *url, bool push);
static void show_document(app *a);
static void relayout_with(app *a, bool restyle);
static void apply_fill(app *a);
static void autofocus(app *a);
static void scroll_by(app *a, float dy);

/* --- helpers ---------------------------------------------------------------------------------- */

static float scale_of(app *a)
{
    return hk_window_scale(a->window);
}

static void viewport_css(app *a, float *w, float *h)
{
    int cw, ch;
    hk_window_client_size(a->window, &cw, &ch);
    *w = (float)cw / scale_of(a);
    *h = (float)ch / scale_of(a);
}

static void html_escape(hk_charbuf *out, const char *s)
{
    for (; *s; s++) {
        if (*s == '<') hk_charbuf_append(out, "&lt;", 4);
        else if (*s == '&') hk_charbuf_append(out, "&amp;", 5);
        else if (*s == '"') hk_charbuf_append(out, "&quot;", 6);
        else hk_charbuf_push(out, *s);
    }
}

/* --- page lifecycle ------------------------------------------------------------------------------ */

static void page_reset(app *a)
{
    page *pg = &a->page;
    for (uint32_t i = 0; i < pg->sheets.len; i++) {
        sheet_slot *s = pg->sheets.data[i];
        if (!s->done) hk_loader_cancel(a->loader, s->load_id);
        hk_vec_free(&s->css);
        if (s->url) hk_free(s->url, s->url_len + 1);
        hk_free(s, sizeof *s);
    }
    hk_vec_free(&pg->sheets);
    if (pg->loading) hk_loader_cancel(a->loader, pg->load_id);
    if (pg->parser) hk_html_parser_destroy(pg->parser);
    if (pg->styler) hk_styler_destroy(pg->styler);
    if (pg->doc) hk_document_destroy(pg->doc);
    if (pg->decoder_ready) hk_decoder_free(&pg->decoder);
    for (uint32_t i = 0; i < pg->images.len; i++) {
        page_image *im = &pg->images.data[i];
        if (im->state == IMG_REQUESTED) hk_loader_cancel(a->loader, im->load_id);
        hk_image_free(&im->img);
    }
    hk_vec_free(&pg->images);
    hk_map64_free(&pg->image_index);
    hk_display_list_free(&pg->dl);
    hk_forms_destroy(pg->forms);
    if (a->window) hk_window_set_text_input(a->window, false, 0, 0, 0);
    hk_painter_clear_cache(a->painter); /* glyph masks keep the old page's fonts alive */
    hk_vec_free(&pg->sniff);
    hk_vec_free(&pg->utf8);
    hk_vec_free(&pg->plain_text);
    hk_arena_free(&pg->url_arena);
    uint32_t gen = pg->generation;
    memset(pg, 0, sizeof *pg);
    pg->generation = gen + 1;
    hk_arena_init(&pg->url_arena, 4096);
}

/* Replaces the page with a generated document (messages and errors). */
static void show_message(app *a, const char *title, const char *detail)
{
    page *pg = &a->page;
    hk_charbuf h = {0};
    hk_charbuf_append(&h, "<!doctype html><title>", 22);
    html_escape(&h, title);
    hk_charbuf_append(&h, "</title><body style='font-family: sans-serif; margin: 3em; color: #333'><h2>", 76);
    html_escape(&h, title);
    hk_charbuf_append(&h, "</h2><p style='color: #666'>", 28);
    html_escape(&h, detail);
    hk_charbuf_append(&h, "</p>", 4);
    if (pg->doc) {
        hk_html_parser_destroy(pg->parser);
        pg->parser = NULL;
        if (pg->styler) hk_styler_destroy(pg->styler);
        pg->styler = NULL;
        hk_document_destroy(pg->doc);
    }
    pg->doc = hk_html_parse(h.data, h.len);
    hk_vec_free(&h);
    pg->parsed = true;
    pg->html = true;
    show_document(a);
}

/* --- layout and display --------------------------------------------------------------------------- */

static uint32_t images_pending(const page *pg)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < pg->images.len; i++) n += pg->images.data[i].state == IMG_REQUESTED;
    return n;
}

/* --exit-after-load: once the page (and, with --wait-images, its visible images) is shown. */
static void maybe_exit(app *a)
{
    page *pg = &a->page;
    if (!a->exit_after_load || !pg->t_shown) return;
    if (a->wait_images && images_pending(pg)) return;
    if (pg->relayout_pending) relayout_with(a, false);
    if (a->wait_images) {
        hk_plat_mem_info mem;
        hk_plat_mem_info_get(&mem);
        HK_LOGI("images: %u loaded, %.2f MiB decoded (scroll %.0f of %.0f); private %.1f MiB", pg->images_loaded,
                (double)pg->image_bytes / 1048576.0, a->scroll, pg->dl.height, (double)mem.private_bytes / 1048576.0);
    }
    if (a->screenshot && hk_window_save_bmp(a->window, a->screenshot) != HK_OK)
        HK_LOGW("could not save %s", a->screenshot);
    a->exit_after_load = false;
    hk_window_close(a->window);
}

/* --- images ------------------------------------------------------------------------------------------ */

static page_image *image_entry(page *pg, hk_node_id node, bool create)
{
    uint64_t v;
    if (hk_map64_get(&pg->image_index, node, &v)) return &pg->images.data[v - 1];
    if (!create) return NULL;
    page_image im = {0};
    im.node = node;
    im.density = 1;
    hk_vec_push(&pg->images, im);
    hk_map64_put(&pg->image_index, node, pg->images.len);
    return &pg->images.data[pg->images.len - 1];
}

static hk_image_state hook_image_size(void *user, hk_node_id img, float *w, float *h)
{
    page *pg = user;
    page_image *im = image_entry(pg, img, false);
    if (!im) return HK_IMAGE_PENDING;
    if (im->state == IMG_BROKEN) return HK_IMAGE_BROKEN;
    if (im->state != IMG_READY) return HK_IMAGE_PENDING;
    *w = (float)im->img.intrinsic_w / im->density;
    *h = (float)im->img.intrinsic_h / im->density;
    return HK_IMAGE_READY;
}

static bool paint_image(void *user, hk_node_id img, const uint32_t **pixels, int *w, int *h)
{
    page_image *im = image_entry(&((app *)user)->page, img, false);
    if (!im || im->state != IMG_READY || !im->img.pixels) return false;
    *pixels = im->img.pixels;
    *w = im->img.width;
    *h = im->img.height;
    return true;
}

static bool media_attr_matches(app *a, const hk_attr *media)
{
    if (!media) return true;
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    hk_css_tokens t = hk_css_tokenize(&tmp, media->value);
    float w, h;
    viewport_css(a, &w, &h);
    hk_css_media_env env = {w, h, false};
    bool ok = hk_css_media_matches(t.t, t.n - 1, &env);
    hk_arena_free(&tmp);
    return ok;
}

/* Picks a candidate from a srcset for a box `want` CSS px wide. */
static bool pick_srcset(hk_str srcset, float want, float scale, hk_str *url, float *density)
{
    hk_str best = {0}, largest = {0};
    float best_d = 0, largest_d = 0;
    size_t i = 0;
    while (i < srcset.n) {
        while (i < srcset.n && (hk_ascii_is_ws(srcset.p[i]) || srcset.p[i] == ',')) i++;
        size_t us = i;
        while (i < srcset.n && !hk_ascii_is_ws(srcset.p[i])) i++;
        hk_str u = hk_str_slice(srcset, us, i);
        /* A URL may end with commas that separate candidates. */
        while (u.n && u.p[u.n - 1] == ',') u.n--;
        while (i < srcset.n && hk_ascii_is_ws(srcset.p[i])) i++;
        size_t ds = i;
        while (i < srcset.n && srcset.p[i] != ',') i++;
        hk_str desc = hk_str_trim_ascii_ws(hk_str_slice(srcset, ds, i));
        if (!u.n) continue;
        float d = 1;
        if (desc.n > 1) {
            float v = 0;
            size_t k = 0;
            for (; k < desc.n && ((desc.p[k] >= '0' && desc.p[k] <= '9') || desc.p[k] == '.'); k++) {
                if (desc.p[k] != '.') v = v * 10 + (float)(desc.p[k] - '0');
            }
            if (k < desc.n && desc.p[k] == 'w' && want > 0) d = v / want;
            else if (k < desc.n && desc.p[k] == 'x') d = v;
        }
        if (d > largest_d) {
            largest = u;
            largest_d = d;
        }
        if (d >= scale && (best_d == 0 || d < best_d)) {
            best = u;
            best_d = d;
        }
    }
    if (best.n) {
        *url = best;
        *density = best_d;
        return true;
    }
    if (largest.n) {
        *url = largest;
        *density = largest_d > 0 ? largest_d : 1;
        return true;
    }
    return false;
}

static bool supported_image_type(hk_str type)
{
    return !(hk_str_eq_ascii_icase(type, HK_STR("image/avif")) || hk_str_eq_ascii_icase(type, HK_STR("image/jxl")) ||
             hk_str_eq_ascii_icase(type, HK_STR("image/svg+xml")));
}

static bool select_image_url(app *a, hk_node_id img, float want, hk_str *url, float *density)
{
    page *pg = &a->page;
    hk_document *d = pg->doc;
    float scale = scale_of(a);
    *density = 1;
    hk_node_id parent = hk_dom_node(d, img)->parent;
    if (parent && hk_dom_is_html(d, parent, A(picture))) {
        for (hk_node_id s = hk_dom_node(d, parent)->first_child; s && s != img; s = hk_dom_node(d, s)->next) {
            if (!hk_dom_is_html(d, s, A(source))) continue;
            const hk_attr *srcset = hk_dom_attr(d, s, HK_ATTR_NS_NONE, A(srcset));
            const hk_attr *type = hk_dom_attr(d, s, HK_ATTR_NS_NONE, A(type));
            if (!srcset || (type && !supported_image_type(type->value))) continue;
            if (!media_attr_matches(a, hk_dom_attr(d, s, HK_ATTR_NS_NONE, A(media)))) continue;
            if (pick_srcset(srcset->value, want, scale, url, density)) return true;
        }
    }
    const hk_attr *srcset = hk_dom_attr(d, img, HK_ATTR_NS_NONE, A(srcset));
    if (srcset && pick_srcset(srcset->value, want, scale, url, density)) return true;
    const hk_attr *src = hk_dom_attr(d, img, HK_ATTR_NS_NONE, A(src));
    if (src && hk_str_trim_ascii_ws(src->value).n) {
        *url = hk_str_trim_ascii_ws(src->value);
        return true;
    }
    return false;
}

typedef struct image_req {
    app *app;
    uint32_t generation;
    hk_node_id node;
} image_req;

static void on_image(void *user, hk_status st, hk_image *img)
{
    image_req *r = user;
    app *a = r->app;
    page *pg = &a->page;
    if (r->generation == pg->generation) {
        page_image *im = image_entry(pg, r->node, false);
        if (im) {
            if (st == HK_OK && img->pixels) {
                im->img = *img;
                img->pixels = NULL;
                im->state = IMG_READY;
                pg->image_bytes += (size_t)im->img.width * (size_t)im->img.height * 4;
                pg->images_loaded++;
            } else {
                im->state = IMG_BROKEN;
            }
            /* Images without a size in CSS change the layout once their size is known. */
            if (!im->sized_by_css || im->state == IMG_BROKEN) pg->relayout_pending = true;
            hk_window_invalidate(a->window);
            maybe_exit(a);
        }
    }
    hk_image_free(img);
    hk_free(r, sizeof *r);
}

/* Requests the images within a few screens of the viewport (all images load lazily). */
static void request_visible_images(app *a)
{
    page *pg = &a->page;
    if (!pg->laid_out || !pg->has_base) return;
    float vw, vh;
    viewport_css(a, &vw, &vh);
    float top = a->scroll - vh, bottom = a->scroll + vh * 2.5f, scale = scale_of(a);
    for (uint32_t i = 0; i < pg->dl.items.len; i++) {
        const hk_item *it = &pg->dl.items.data[i];
        if (it->kind != HK_ITEM_IMAGE) continue;
        if (!it->fixed && (it->y > bottom || it->y + it->h < top)) continue;
        page_image *im = image_entry(pg, it->node, true);
        if (im->state != IMG_NONE) continue;
        hk_str src;
        float density;
        float want = it->w > 0 ? it->w : vw;
        hk_url u;
        if (!select_image_url(a, it->node, want, &src, &density) ||
            hk_url_parse(&pg->url_arena, src, &pg->base, &u) != HK_OK) {
            im->state = IMG_BROKEN;
            pg->relayout_pending = true;
            continue;
        }
        im->density = density > 0 ? density : 1;
        im->sized_by_css = it->w > 0 && it->h > 0;
        int max_w = im->sized_by_css ? (int)ceilf(it->w * scale) : (int)ceilf(vw * scale);
        int max_h = im->sized_by_css ? (int)ceilf(it->h * scale) : 8192;
        image_req *r = hk_alloc_zero(sizeof *r);
        r->app = a;
        r->generation = pg->generation;
        r->node = it->node;
        hk_load_callbacks cb = {r, NULL, NULL, NULL, on_image};
        im->state = IMG_REQUESTED;
        im->load_id = hk_loader_fetch_image(a->loader, u.href, hk_str_from_cstr(pg->url), max_w, max_h, &cb);
    }
}

static void relayout_with(app *a, bool restyle)
{
    page *pg = &a->page;
    if (!pg->styler) return;
    float w, h;
    viewport_css(a, &w, &h);
    if (restyle || !pg->laid_out) {
        hk_css_media_env env = {w, h, false};
        hk_styler_compute(pg->styler, &env);
    }
    hk_display_list_free(&pg->dl);
    hk_layout_hooks hooks = {pg, hook_image_size};
    hk_layout_document(pg->doc, pg->styler, w, h, &hooks, &pg->dl);
    pg->layout_width = w;
    pg->laid_out = true;
    pg->relayout_pending = false;
    float max = pg->dl.height - h;
    if (a->scroll > max) a->scroll = max;
    if (a->scroll < 0) a->scroll = 0;
    request_visible_images(a);
    hk_window_invalidate(a->window);
}

static void relayout(app *a)
{
    relayout_with(a, true);
}

static hk_node_id find_element(const hk_document *d, hk_node_id from, hk_atom name)
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

static void set_title(app *a)
{
    page *pg = &a->page;
    hk_charbuf t = {0};
    hk_node_id title = find_element(pg->doc, pg->doc->root, A(title));
    if (title) {
        bool space = false;
        for (hk_node_id c = hk_dom_node(pg->doc, title)->first_child; c; c = hk_dom_node(pg->doc, c)->next) {
            if (hk_dom_node(pg->doc, c)->type != HK_NODE_TEXT) continue;
            hk_str s = hk_dom_text(pg->doc, c);
            for (size_t i = 0; i < s.n; i++) {
                if (hk_ascii_is_ws(s.p[i])) {
                    space = t.len > 0;
                    continue;
                }
                if (space) hk_charbuf_push(&t, ' ');
                space = false;
                hk_charbuf_push(&t, s.p[i]);
            }
        }
    }
    if (t.len) hk_charbuf_append(&t, " - ", 3);
    hk_charbuf_append(&t, "Hawk", 4);
    hk_window_set_title(a->window, (hk_str){t.data, t.len});
    hk_vec_free(&t);
}

static void log_stats(app *a)
{
    page *pg = &a->page;
    hk_plat_mem_info mem;
    hk_plat_mem_info_get(&mem);
    uint32_t distinct = 0, elements = 0;
    size_t style_bytes = 0;
    if (pg->styler) hk_styler_stats(pg->styler, &distinct, &elements, &style_bytes);
    HK_LOGI("shown %s: %zu bytes HTML + %zu bytes CSS (%u sheets), first byte %.0f ms, parsed %.0f ms, shown %.0f ms; "
            "%u nodes, %u styles for %u elements, %u display items; document %.2f MiB, styles %.2f MiB, "
            "private %.1f MiB",
            pg->url, pg->bytes, pg->css_bytes, pg->sheets.len,
            pg->t_first_byte ? (double)(pg->t_first_byte - pg->t_start) / 1e6 : 0.0,
            pg->t_parsed ? (double)(pg->t_parsed - pg->t_start) / 1e6 : 0.0,
            (double)(pg->t_shown - pg->t_start) / 1e6, pg->doc ? pg->doc->nodes.len - 1 : 0, distinct, elements,
            pg->dl.items.len, pg->doc ? (double)hk_document_memory(pg->doc) / 1048576.0 : 0.0,
            (double)style_bytes / 1048576.0, (double)mem.private_bytes / 1048576.0);
}

/* All stylesheets are in: build the styles and display the page. */
static void show_document(app *a)
{
    page *pg = &a->page;
    if (!pg->styler) {
        pg->styler = hk_styler_create(pg->doc);
        for (uint32_t i = 0; i < pg->sheets.len; i++) {
            sheet_slot *s = pg->sheets.data[i];
            if (s->css.len) hk_styler_add_sheet(pg->styler, (hk_str){s->css.data, s->css.len});
        }
    }
    set_title(a);
    a->scroll = a->restore_scroll;
    a->restore_scroll = 0;
    relayout(a);
    hk_window_set_cursor(a->window, HK_CURSOR_ARROW);
    pg->t_shown = hk_plat_now_ns();
    log_stats(a);
    if (a->nfill || a->submit_after_fill) {
        apply_fill(a);
        return;
    }
    autofocus(a);
    maybe_exit(a);
}

/* --- stylesheets ------------------------------------------------------------------------------------ */

static void sheet_finished(app *a)
{
    page *pg = &a->page;
    if (pg->pending_sheets > 0) pg->pending_sheets--;
    if (pg->parsed && pg->pending_sheets == 0) show_document(a);
}

static void start_sheet(app *a, uint32_t index);

static void on_sheet_data(void *user, const uint8_t *p, size_t n)
{
    sheet_slot *s = user;
    if (s->generation != s->app->page.generation) return;
    hk_charbuf_append(&s->css, (const char *)p, n);
}

static void insert_imports(app *a, sheet_slot *s)
{
    if (s->depth >= MAX_IMPORT_DEPTH || s->css.len == 0) return;
    /* Parse just to find @import rules; the sheet is parsed again by the styler. */
    page *pg = &a->page;
    hk_arena tmp;
    hk_arena_init(&tmp, 0);
    hk_atom_table atoms;
    hk_atom_table_init(&atoms, &tmp, NULL);
    hk_css_sheet sheet;
    hk_css_parse_sheet(&tmp, &atoms, (hk_str){s->css.data, s->css.len}, &sheet);
    uint32_t index = 0;
    while (index < pg->sheets.len && pg->sheets.data[index] != s) index++;
    hk_url base;
    bool base_ok = s->url && hk_url_parse(&tmp, (hk_str){s->url, s->url_len}, NULL, &base) == HK_OK;
    for (uint32_t k = 0; k < sheet.imports.len && base_ok; k++) {
        hk_url u;
        if (hk_url_parse(&tmp, sheet.imports.data[k], &base, &u) != HK_OK) continue;
        sheet_slot *imp = hk_alloc_zero(sizeof *imp);
        imp->app = a;
        imp->generation = pg->generation;
        imp->depth = s->depth + 1;
        imp->url = hk_alloc(u.href.n + 1);
        memcpy(imp->url, u.href.p, u.href.n);
        imp->url[u.href.n] = 0;
        imp->url_len = u.href.n;
        /* Imported rules come before the importing sheet's own rules. */
        hk_vec_push(&pg->sheets, imp);
        memmove(&pg->sheets.data[index + k + 1], &pg->sheets.data[index + k],
                (pg->sheets.len - 1 - index - k) * sizeof(sheet_slot *));
        pg->sheets.data[index + k] = imp;
        pg->pending_sheets++;
        start_sheet(a, index + k);
    }
    hk_css_sheet_free(&sheet);
    hk_atom_table_free(&atoms);
    hk_arena_free(&tmp);
}

static void on_sheet_done(void *user, hk_status st, const char *message)
{
    sheet_slot *s = user;
    app *a = s->app;
    if (s->generation != a->page.generation) return;
    s->done = true;
    if (st != HK_OK) {
        HK_LOGW("stylesheet %s: %s", s->url, message);
        s->css.len = 0;
    }
    a->page.css_bytes += s->css.len;
    insert_imports(a, s);
    sheet_finished(a);
}

static void start_sheet(app *a, uint32_t index)
{
    sheet_slot *s = a->page.sheets.data[index];
    hk_load_callbacks cb = {s, NULL, on_sheet_data, on_sheet_done};
    s->load_id = hk_loader_fetch(a->loader, (hk_str){s->url, s->url_len}, hk_str_from_cstr(a->page.url),
                                 HK_REQ_STYLESHEET, &cb);
}

static hk_str text_content(const hk_document *d, hk_node_id el, hk_charbuf *buf)
{
    buf->len = 0;
    for (hk_node_id c = hk_dom_node(d, el)->first_child; c; c = hk_dom_node(d, c)->next) {
        if (hk_dom_node(d, c)->type == HK_NODE_TEXT) {
            hk_str t = hk_dom_text(d, c);
            hk_charbuf_append(buf, t.p, t.n);
        }
    }
    return (hk_str){buf->data, buf->len};
}

static bool rel_has(hk_str rel, const char *word)
{
    hk_str w = hk_str_from_cstr(word);
    size_t i = 0;
    while (i < rel.n) {
        while (i < rel.n && hk_ascii_is_ws(rel.p[i])) i++;
        size_t st = i;
        while (i < rel.n && !hk_ascii_is_ws(rel.p[i])) i++;
        if (i > st && hk_str_eq_ascii_icase(hk_str_slice(rel, st, i), w)) return true;
    }
    return false;
}

/* Walks the document for <style> and <link rel=stylesheet>, in tree order. */
static void collect_sheets(app *a, hk_node_id node)
{
    page *pg = &a->page;
    hk_document *d = pg->doc;
    for (hk_node_id c = hk_dom_node(d, node)->first_child; c; c = hk_dom_node(d, c)->next) {
        const hk_node *n = hk_dom_node(d, c);
        if (n->type != HK_NODE_ELEMENT) continue;
        if (n->ns != HK_NS_HTML) continue;
        if (hk_dom_element(d, c)->template_content) continue;
        hk_atom name = hk_dom_element(d, c)->name;
        const hk_attr *media = hk_dom_attr(d, c, HK_ATTR_NS_NONE, A(media));
        bool media_ok = true;
        if (media) {
            hk_arena tmp;
            hk_arena_init(&tmp, 1024);
            hk_css_tokens t = hk_css_tokenize(&tmp, media->value);
            float w, h;
            viewport_css(a, &w, &h);
            hk_css_media_env env = {w, h, false};
            media_ok = hk_css_media_matches(t.t, t.n - 1, &env);
            hk_arena_free(&tmp);
        }
        if (name == A(style) && media_ok) {
            sheet_slot *s = hk_alloc_zero(sizeof *s);
            s->app = a;
            s->generation = pg->generation;
            s->done = true;
            hk_charbuf buf = {0};
            hk_str css = text_content(d, c, &buf);
            hk_charbuf_append(&s->css, css.p, css.n);
            hk_vec_free(&buf);
            hk_vec_push(&pg->sheets, s);
            continue;
        }
        if (name == A(link) && media_ok) {
            const hk_attr *rel = hk_dom_attr(d, c, HK_ATTR_NS_NONE, A(rel));
            const hk_attr *href = hk_dom_attr(d, c, HK_ATTR_NS_NONE, A(href));
            if (rel && href && rel_has(rel->value, "stylesheet") && !rel_has(rel->value, "alternate")) {
                hk_url u;
                if (pg->has_base && hk_url_parse(&pg->url_arena, href->value, &pg->base, &u) == HK_OK) {
                    sheet_slot *s = hk_alloc_zero(sizeof *s);
                    s->app = a;
                    s->generation = pg->generation;
                    s->url = hk_alloc(u.href.n + 1);
                    memcpy(s->url, u.href.p, u.href.n);
                    s->url[u.href.n] = 0;
                    s->url_len = u.href.n;
                    hk_vec_push(&pg->sheets, s);
                }
            }
            continue;
        }
        collect_sheets(a, c);
    }
}

/* --- the main document ---------------------------------------------------------------------------- */

static void decode_and_feed(app *a, const uint8_t *p, size_t n, bool last)
{
    page *pg = &a->page;
    pg->utf8.len = 0;
    hk_decoder_decode(&pg->decoder, p, n, last, &pg->utf8);
    if (pg->html) hk_html_parser_feed(pg->parser, pg->utf8.data, pg->utf8.len);
    else hk_charbuf_append(&pg->plain_text, pg->utf8.data, pg->utf8.len);
}

static void start_decoding(app *a)
{
    page *pg = &a->page;
    size_t bom = 0;
    pg->encoding = hk_encoding_sniff((const uint8_t *)pg->sniff.data, pg->sniff.len, pg->transport,
                                     hk_plat_locale_is_japanese(), &bom);
    hk_decoder_init(&pg->decoder, pg->encoding);
    pg->decoder_ready = true;
    if (pg->html) {
        pg->doc = hk_document_create();
        pg->parser = hk_html_parser_create(pg->doc);
    }
    decode_and_feed(a, (const uint8_t *)pg->sniff.data + bom, pg->sniff.len - bom, false);
    hk_vec_free(&pg->sniff);
}

static void on_response(void *user, const hk_load_response *r)
{
    app *a = user;
    page *pg = &a->page;
    pg->t_first_byte = hk_plat_now_ns();
    snprintf(pg->url, sizeof pg->url, "%.*s", (int)r->url.n, r->url.p);
    snprintf(pg->content_type, sizeof pg->content_type, "%.*s", (int)r->content_type.n, r->content_type.p);
    pg->has_base = hk_url_parse(&pg->url_arena, r->url, NULL, &pg->base) == HK_OK;
    if (a->history_pos) snprintf(a->history.data[a->history_pos - 1].url, sizeof a->history.data[0].url, "%s", pg->url);
    hk_str mime = r->content_type;
    size_t semi = hk_str_find_char(mime, ';');
    if (semi != HK_STR_NPOS) mime = hk_str_slice(mime, 0, semi);
    mime = hk_str_trim_ascii_ws(mime);
    pg->html = mime.n == 0 || hk_str_eq_ascii_icase(mime, HK_STR("text/html")) ||
               hk_str_eq_ascii_icase(mime, HK_STR("application/xhtml+xml"));
    /* Other text (CSS, JSON, XML sources, ...) is shown as plain text. */
    pg->plain = !pg->html && (hk_str_starts_with(mime, HK_STR("text/")) || hk_str_starts_with(mime, HK_STR("TEXT/")) ||
                              hk_str_eq_ascii_icase(mime, HK_STR("application/json")) ||
                              hk_str_eq_ascii_icase(mime, HK_STR("application/xml")) ||
                              hk_str_eq_ascii_icase(mime, HK_STR("application/javascript")) ||
                              hk_str_ends_with(mime, HK_STR("+json")) || hk_str_ends_with(mime, HK_STR("+xml")));
    pg->unsupported = !pg->html && !pg->plain;
    pg->transport = hk_encoding_from_content_type(r->content_type);
}

static void on_data(void *user, const uint8_t *p, size_t n)
{
    app *a = user;
    page *pg = &a->page;
    pg->bytes += n;
    if (pg->unsupported) return;
    if (!pg->decoder_ready) {
        hk_charbuf_append(&pg->sniff, (const char *)p, n);
        if (pg->sniff.len >= SNIFF_BYTES) start_decoding(a);
        return;
    }
    decode_and_feed(a, p, n, false);
}

static void on_done(void *user, hk_status st, const char *message)
{
    app *a = user;
    page *pg = &a->page;
    pg->loading = false;
    if (st == HK_ERR_BLOCKED) {
        show_message(a, "\xE3\x83\x96\xE3\x83\xAD\xE3\x83\x83\xE3\x82\xAB\xE3\x83\xBC\xE3\x81\x8C\xE3\x81\x93\xE3\x81"
                        "\xAE\xE3\x83\x9A\xE3\x83\xBC\xE3\x82\xB8\xE3\x82\x92\xE9\x81\xAE\xE6\x96\xAD\xE3\x81\x97\xE3"
                        "\x81\xBE\xE3\x81\x97\xE3\x81\x9F", /* ブロッカーがこのページを遮断しました */
                     message);
        return;
    }
    if (st != HK_OK) {
        char detail[2400];
        snprintf(detail, sizeof detail, "%s (%s)", message, pg->url);
        show_message(a, "\xE8\xAA\xAD\xE3\x81\xBF\xE8\xBE\xBC\xE3\x82\x81\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93\xE3\x81"
                        "\xA7\xE3\x81\x97\xE3\x81\x9F", /* 読み込めませんでした */
                     detail);
        return;
    }
    if (pg->unsupported) {
        show_message(a, "\xE3\x81\x93\xE3\x81\xAE\xE5\xBD\xA2\xE5\xBC\x8F\xE3\x81\xAF\xE8\xA1\xA8\xE7\xA4\xBA\xE3\x81"
                        "\xA7\xE3\x81\x8D\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93", /* この形式は表示できません */
                     pg->content_type);
        return;
    }
    if (!pg->decoder_ready) start_decoding(a);
    decode_and_feed(a, NULL, 0, true);
    if (pg->plain) {
        hk_charbuf h = {0};
        hk_charbuf_append(&h, "<pre style='white-space: pre-wrap; margin: 8px'>", 48);
        hk_charbuf_push(&pg->plain_text, '\0');
        html_escape(&h, pg->plain_text.data);
        pg->doc = hk_html_parse(h.data, h.len);
        hk_vec_free(&h);
        pg->parsed = true;
        pg->t_parsed = hk_plat_now_ns();
        show_document(a);
        return;
    }
    hk_html_parser_finish(pg->parser);
    hk_html_parser_destroy(pg->parser);
    pg->parser = NULL;
    pg->parsed = true;
    pg->t_parsed = hk_plat_now_ns();

    /* <base href> */
    hk_node_id base = find_element(pg->doc, pg->doc->root, A(base));
    const hk_attr *bh = base ? hk_dom_attr(pg->doc, base, HK_ATTR_NS_NONE, A(href)) : NULL;
    hk_url bu;
    if (bh && pg->has_base && hk_url_parse(&pg->url_arena, bh->value, &pg->base, &bu) == HK_OK) pg->base = bu;

    collect_sheets(a, pg->doc->root);
    for (uint32_t i = 0; i < pg->sheets.len; i++) {
        if (!pg->sheets.data[i]->done) pg->pending_sheets++;
    }
    if (pg->pending_sheets == 0) {
        show_document(a);
        return;
    }
    for (uint32_t i = 0, n = pg->sheets.len; i < n; i++) {
        if (!pg->sheets.data[i]->done) start_sheet(a, i);
    }
}

static void show_internal(app *a, const char *url);

static void free_history_from(app *a, uint32_t from)
{
    for (uint32_t i = from; i < a->history.len; i++)
        if (a->history.data[i].post) hk_free(a->history.data[i].post, a->history.data[i].post_len + 1);
    a->history.len = from;
}

static void push_history(app *a, const char *url, hk_str post)
{
    if (a->history_pos) a->history.data[a->history_pos - 1].scroll = a->scroll;
    free_history_from(a, a->history_pos); /* drop the forward entries */
    history_entry e = {{0}, 0, NULL, 0};
    snprintf(e.url, sizeof e.url, "%s", url);
    if (post.p) {
        e.post = hk_alloc(post.n + 1);
        memcpy(e.post, post.p, post.n);
        e.post[post.n] = 0;
        e.post_len = post.n;
    }
    hk_vec_push(&a->history, e);
    a->history_pos = a->history.len;
    a->restore_scroll = 0;
}

/* What a navigation from the current page sends as its referrer: the page URL, its origin, or
   nothing, following the page's <meta name=referrer> (the loader then applies Hawk's own policy). */
static void page_referrer(app *a, hk_charbuf *out)
{
    page *pg = &a->page;
    out->len = 0;
    if (!pg->doc || !pg->has_base || strncmp(pg->url, "http", 4) != 0) return;
    hk_str policy = {0};
    for (hk_node_id n = 1; n < pg->doc->nodes.len; n++) {
        if (!hk_dom_is_html(pg->doc, n, A(meta))) continue;
        const hk_attr *name = hk_dom_attr(pg->doc, n, HK_ATTR_NS_NONE, A(name));
        const hk_attr *content = hk_dom_attr(pg->doc, n, HK_ATTR_NS_NONE, A(content));
        if (name && content && hk_str_eq_ascii_icase(hk_str_trim_ascii_ws(name->value), HK_STR("referrer")))
            policy = hk_str_trim_ascii_ws(content->value);
    }
    if (hk_str_eq_ascii_icase(policy, HK_STR("no-referrer")) || hk_str_eq_ascii_icase(policy, HK_STR("never"))) return;
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    hk_url u;
    if (hk_url_parse(&tmp, hk_str_from_cstr(pg->url), NULL, &u) == HK_OK) {
        if (hk_str_eq_ascii_icase(policy, HK_STR("origin")) || hk_str_eq_ascii_icase(policy, HK_STR("strict-origin"))) {
            hk_charbuf_append(out, u.scheme.p, u.scheme.n);
            hk_charbuf_append(out, "://", 3);
            hk_charbuf_append(out, u.host.p, u.host.n);
            if (u.port >= 0) {
                char port[16];
                int n = snprintf(port, sizeof port, ":%d", u.port);
                hk_charbuf_append(out, port, (size_t)n);
            }
            hk_charbuf_push(out, '/');
        } else {
            hk_charbuf_append(out, u.href.p, u.href.n);
        }
    }
    hk_arena_free(&tmp);
}

/* Submits a form body to url (a navigation that replaces the page). */
static void navigate_post(app *a, const char *url, hk_str body, hk_str origin)
{
    hk_charbuf ref = {0};
    page_referrer(a, &ref);
    page_reset(a);
    page *pg = &a->page;
    snprintf(pg->url, sizeof pg->url, "%s", url);
    pg->loading = true;
    pg->t_start = hk_plat_now_ns();
    hk_window_set_cursor(a->window, HK_CURSOR_WAIT);
    hk_load_callbacks cb = {a, on_response, on_data, on_done};
    pg->load_id = hk_loader_post(a->loader, hk_str_from_cstr(url), HK_STR("application/x-www-form-urlencoded"), body,
                                 origin, (hk_str){ref.data, ref.len}, &cb);
    hk_vec_free(&ref);
}

static void navigate_from(app *a, const char *url, bool push, hk_str referrer);

static void navigate(app *a, const char *url, bool push)
{
    navigate_from(a, url, push, (hk_str){0});
}

/* A navigation from the current page (a link or a GET form): sends the page as the referrer. */
static void navigate_link(app *a, const char *url)
{
    hk_charbuf ref = {0};
    page_referrer(a, &ref);
    navigate_from(a, url, true, (hk_str){ref.data, ref.len});
    hk_vec_free(&ref);
}

static void navigate_from(app *a, const char *url, bool push, hk_str referrer)
{
    if (push) push_history(a, url, (hk_str){0});
    page_reset(a);
    page *pg = &a->page;
    snprintf(pg->url, sizeof pg->url, "%s", url);
    pg->t_start = hk_plat_now_ns();
    if (strncmp(url, "hawk:", 5) == 0) {
        show_internal(a, url);
        return;
    }
    pg->loading = true;
    pg->t_start = hk_plat_now_ns();
    hk_window_set_cursor(a->window, HK_CURSOR_WAIT);
    hk_load_callbacks cb = {a, on_response, on_data, on_done};
    pg->load_id = hk_loader_navigate(a->loader, hk_str_from_cstr(url), referrer, &cb);
}

static void go_history(app *a, int delta)
{
    if (a->history_pos == 0) return;
    int target = (int)a->history_pos - 1 + delta;
    if (target < 0 || target >= (int)a->history.len) return;
    a->history.data[a->history_pos - 1].scroll = a->scroll;
    a->history_pos = (uint32_t)target + 1;
    a->restore_scroll = a->history.data[target].scroll;
    char url[2048];
    snprintf(url, sizeof url, "%s", a->history.data[target].url);
    if (a->history.data[target].post) {
        /* A form submission is not sent again without asking. */
        page_reset(a);
        snprintf(a->page.url, sizeof a->page.url, "%s", url);
        /* フォームの送信結果 / このページはフォームを送信した結果です。… */
        show_message(a, "\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xBC\xE3\x83\xA0\xE3\x81\xAE\xE9\x80\x81\xE4\xBF\xA1"
                        "\xE7\xB5\x90\xE6\x9E\x9C",
                     "\xE3\x81\x93\xE3\x81\xAE\xE3\x83\x9A\xE3\x83\xBC\xE3\x82\xB8\xE3\x81\xAF\xE3\x83\x95\xE3"
                     "\x82\xA9\xE3\x83\xBC\xE3\x83\xA0\xE3\x82\x92\xE9\x80\x81\xE4\xBF\xA1\xE3\x81\x97\xE3\x81"
                     "\x9F\xE7\xB5\x90\xE6\x9E\x9C\xE3\x81\xA7\xE3\x81\x99\xE3\x80\x82\xE5\x86\x8D\xE9\x80\x81"
                     "\xE4\xBF\xA1\xE3\x81\x99\xE3\x82\x8B\xE3\x81\xA8\xE3\x80\x81\xE5\x90\x8C\xE3\x81\x98\xE6"
                     "\x93\x8D\xE4\xBD\x9C\xEF\xBC\x88\xE8\xB3\xBC\xE5\x85\xA5\xE3\x82\x84\xE6\x8A\x95\xE7\xA8"
                     "\xBF\xE3\x81\xAA\xE3\x81\xA9\xEF\xBC\x89\xE3\x81\x8C\xE7\xB9\xB0\xE3\x82\x8A\xE8\xBF\x94"
                     "\xE3\x81\x95\xE3\x82\x8C\xE3\x82\x8B\xE3\x81\x93\xE3\x81\xA8\xE3\x81\x8C\xE3\x81\x82\xE3"
                     "\x82\x8A\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82 Shift+F5: "
                     "\xE5\x86\x8D\xE9\x80\x81\xE4\xBF\xA1");
        return;
    }
    navigate(a, url, false);
}

/* Shift+F5 on a form submission's entry: sends it again. */
static void resubmit(app *a)
{
    if (!a->history_pos) return;
    history_entry *e = &a->history.data[a->history_pos - 1];
    if (!e->post) return;
    char url[2048];
    snprintf(url, sizeof url, "%s", e->url);
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    hk_url u;
    hk_charbuf origin = {0};
    if (hk_url_parse(&tmp, hk_str_from_cstr(url), NULL, &u) == HK_OK) {
        hk_charbuf_append(&origin, u.scheme.p, u.scheme.n);
        hk_charbuf_append(&origin, "://", 3);
        hk_charbuf_append(&origin, u.host.p, u.host.n);
    }
    navigate_post(a, url, (hk_str){e->post, e->post_len}, (hk_str){origin.data, origin.len});
    hk_vec_free(&origin);
    hk_arena_free(&tmp);
}

static void follow_link(app *a, hk_node_id link)
{
    page *pg = &a->page;
    const hk_attr *href = hk_dom_attr(pg->doc, link, HK_ATTR_NS_NONE, A(href));
    if (href && hk_str_starts_with(href->value, HK_STR("hawk:"))) {
        char internal[256];
        snprintf(internal, sizeof internal, "%.*s", (int)href->value.n, href->value.p);
        navigate(a, internal, true);
        return;
    }
    if (!href || !pg->has_base) return;
    hk_url u;
    if (hk_url_parse(&pg->url_arena, href->value, &pg->base, &u) != HK_OK) return;
    if (!hk_str_eq(u.scheme, HK_STR("http")) && !hk_str_eq(u.scheme, HK_STR("https"))) {
        HK_LOGI("link with unsupported scheme ignored: %.*s", (int)u.href.n, u.href.p);
        return;
    }
    char url[2048];
    snprintf(url, sizeof url, "%.*s", (int)u.href.n, u.href.p);
    navigate_link(a, url);
}

/* --- forms --------------------------------------------------------------------------------------- */

static hk_forms *page_forms(app *a)
{
    page *pg = &a->page;
    if (!pg->forms && pg->doc) pg->forms = hk_forms_create(pg->doc);
    return pg->forms;
}

static hk_form_ctx form_ctx(app *a)
{
    page *pg = &a->page;
    hk_form_ctx c = {0};
    c.doc = pg->doc;
    c.st = pg->styler;
    c.forms = page_forms(a);
    c.focus = pg->focus;
    c.show_caret = true;
    c.comp = pg->comp;
    c.comp_len = pg->comp_len;
    c.comp_caret = pg->comp_caret;
    return c;
}

static bool paint_control(void *user, const hk_item *it, hk_control_visual *v)
{
    app *a = user;
    if (!a->page.doc || !a->page.styler) return false;
    hk_form_ctx c = form_ctx(a);
    return hk_form_view_visual(a->form_view, &c, it, v);
}

static bool focused_text(app *a)
{
    page *pg = &a->page;
    return pg->focus && hk_control_is_text(hk_control_kind_of(pg->doc, pg->focus));
}

/* Keeps the IME's candidate window at the caret of the focused text control. */
static void update_text_input(app *a)
{
    page *pg = &a->page;
    const hk_item *it = focused_text(a) ? hk_display_list_control(&pg->dl, pg->focus) : NULL;
    if (!it || hk_control_readonly(pg->doc, pg->focus)) {
        hk_window_set_text_input(a->window, false, 0, 0, 0);
        return;
    }
    hk_form_ctx c = form_ctx(a);
    float x, y, h;
    hk_form_view_caret(a->form_view, &c, it, &x, &y, &h);
    float scale = scale_of(a), sy = it->fixed ? 0 : a->scroll;
    hk_window_set_text_input(a->window, true, (int)((it->x + x) * scale), (int)((it->y + y - sy) * scale),
                             (int)(h * scale));
}

static void set_focus(app *a, hk_node_id el)
{
    page *pg = &a->page;
    if (pg->focus != el) {
        pg->comp_len = 0;
        pg->focus = el;
    }
    update_text_input(a);
    hk_window_invalidate(a->window);
}

/* Scrolls the page so that the focused control is visible. */
static void scroll_to_focus(app *a)
{
    page *pg = &a->page;
    const hk_item *it = pg->focus ? hk_display_list_control(&pg->dl, pg->focus) : NULL;
    if (!it || it->fixed) return;
    float vw, vh;
    viewport_css(a, &vw, &vh);
    if (it->y < a->scroll) scroll_by(a, it->y - a->scroll - 20);
    else if (it->y + it->h > a->scroll + vh) scroll_by(a, it->y + it->h - a->scroll - vh + 20);
    update_text_input(a);
}

static void submit_form(app *a, hk_node_id form, hk_node_id submitter)
{
    page *pg = &a->page;
    if (!form || !pg->has_base) return;
    hk_form_submission sub;
    if (!hk_forms_submit(page_forms(a), form, submitter, &pg->base, pg->encoding, &sub)) {
        HK_LOGI("form not submitted (unsupported method or action)");
        return;
    }
    hk_charbuf_push(&sub.url, 0);
    HK_LOGI("submit %s %s%s%.*s", sub.post ? "POST" : "GET", sub.url.data, sub.post ? " body " : "",
            sub.post ? (int)(sub.body.len < 200 ? sub.body.len : 200) : 0, sub.body.data ? sub.body.data : "");
    if (sub.post) {
        hk_arena tmp;
        hk_arena_init(&tmp, 1024);
        hk_url u;
        hk_charbuf origin = {0};
        if (hk_url_parse(&tmp, hk_str_from_cstr(pg->url), NULL, &u) == HK_OK && u.host.n) {
            hk_charbuf_append(&origin, u.scheme.p, u.scheme.n);
            hk_charbuf_append(&origin, "://", 3);
            hk_charbuf_append(&origin, u.host.p, u.host.n);
            if (u.port >= 0) {
                char port[16];
                int n = snprintf(port, sizeof port, ":%d", u.port);
                hk_charbuf_append(&origin, port, (size_t)n);
            }
        }
        hk_arena_free(&tmp);
        push_history(a, sub.url.data, (hk_str){sub.body.data ? sub.body.data : "", sub.body.len});
        navigate_post(a, sub.url.data, (hk_str){sub.body.data ? sub.body.data : "", sub.body.len},
                      (hk_str){origin.data, origin.len});
        hk_vec_free(&origin);
    } else {
        navigate_link(a, sub.url.data);
    }
    hk_form_submission_free(&sub);
}

/* Implicit submission (Enter in a text field): the form's first submit button is the submitter. */
static void implicit_submit(app *a, hk_node_id el)
{
    page *pg = &a->page;
    hk_node_id form = hk_form_owner(pg->doc, el);
    if (!form) return;
    hk_node_id submitter = HK_NODE_NONE;
    for (hk_node_id n = pg->doc->root; n && !submitter;) {
        const hk_node *x = hk_dom_node(pg->doc, n);
        hk_control_kind k = hk_control_kind_of(pg->doc, n);
        if ((k == HK_CTRL_SUBMIT || k == HK_CTRL_IMAGE) && hk_form_owner(pg->doc, n) == form) submitter = n;
        if (x->first_child) {
            n = x->first_child;
            continue;
        }
        while (n && !hk_dom_node(pg->doc, n)->next) n = hk_dom_node(pg->doc, n)->parent;
        if (n) n = hk_dom_node(pg->doc, n)->next;
    }
    if (submitter && hk_control_disabled(pg->doc, submitter)) return;
    submit_form(a, form, submitter);
}

static void open_select(app *a, hk_node_id select)
{
    page *pg = &a->page;
    const hk_item *it = hk_display_list_control(&pg->dl, select);
    if (!it) return;
    hk_node_vec opts = {0};
    hk_select_options(pg->doc, select, &opts);
    if (!opts.len) {
        hk_vec_free(&opts);
        return;
    }
    hk_forms *f = page_forms(a);
    hk_charbuf *bufs = hk_alloc_zero(opts.len * sizeof(hk_charbuf));
    hk_str *labels = hk_alloc(opts.len * sizeof(hk_str));
    uint8_t *flags = hk_alloc(opts.len);
    for (uint32_t i = 0; i < opts.len; i++) {
        hk_str l = hk_option_label(pg->doc, opts.data[i], &bufs[i]);
        if (l.p != bufs[i].data) {
            hk_charbuf_append(&bufs[i], l.p, l.n); /* the label attribute: copy, as bufs own the strings */
            l = (hk_str){bufs[i].data, l.n};
        }
        labels[i] = l;
        hk_node_id parent = hk_dom_node(pg->doc, opts.data[i])->parent;
        bool disabled = hk_dom_attr(pg->doc, opts.data[i], HK_ATTR_NS_NONE, A(disabled)) ||
                        (hk_dom_is_html(pg->doc, parent, A(optgroup)) &&
                         hk_dom_attr(pg->doc, parent, HK_ATTR_NS_NONE, A(disabled)));
        flags[i] = (uint8_t)((disabled ? 1 : 0) | (hk_forms_option_selected(f, select, i) ? 2 : 0));
    }
    float scale = scale_of(a), sy = it->fixed ? 0 : a->scroll;
    const hk_style *s = hk_style_of(pg->styler, select);
    float bottom = it->y + it->h + (s ? s->border_width[2] + 2 : 2);
    int choice = hk_window_popup_menu(a->window, labels, flags, opts.len, (int)((it->x - 3) * scale),
                                      (int)((bottom - sy) * scale));
    if (choice >= 0 && !(flags[choice] & 1)) hk_forms_select(f, select, (uint32_t)choice);
    for (uint32_t i = 0; i < opts.len; i++) hk_vec_free(&bufs[i]);
    hk_free(bufs, opts.len * sizeof(hk_charbuf));
    hk_free(labels, opts.len * sizeof(hk_str));
    hk_free(flags, opts.len);
    hk_vec_free(&opts);
    hk_window_invalidate(a->window);
}

/* A click on (or Space/Enter on) a control. */
static void activate_control(app *a, hk_node_id el)
{
    page *pg = &a->page;
    if (hk_control_disabled(pg->doc, el)) return;
    hk_forms *f = page_forms(a);
    switch (hk_control_kind_of(pg->doc, el)) {
    case HK_CTRL_CHECKBOX: hk_forms_set_checked(f, el, !hk_forms_checked(f, el)); break;
    case HK_CTRL_RADIO: hk_forms_set_checked(f, el, true); break;
    case HK_CTRL_SUBMIT:
    case HK_CTRL_IMAGE: submit_form(a, hk_form_owner(pg->doc, el), el); return;
    case HK_CTRL_RESET: hk_forms_reset(f, hk_form_owner(pg->doc, el)); break;
    case HK_CTRL_SELECT: open_select(a, el); return;
    default: break;
    }
    hk_window_invalidate(a->window);
}

static void autofocus(app *a)
{
    page *pg = &a->page;
    if (pg->autofocus_done || pg->focus) return;
    pg->autofocus_done = true;
    for (uint32_t i = 0; i < pg->dl.items.len; i++) {
        const hk_item *it = &pg->dl.items.data[i];
        if (it->kind == HK_ITEM_CONTROL && hk_dom_attr(pg->doc, it->node, HK_ATTR_NS_NONE, A(autofocus)) &&
            !hk_control_disabled(pg->doc, it->node)) {
            set_focus(a, it->node);
            return;
        }
    }
}

/* --fill NAME=VALUE and --submit: drives a form without the mouse (tests and measurements). */
static void apply_fill(app *a)
{
    page *pg = &a->page;
    hk_forms *f = page_forms(a);
    hk_node_id last = HK_NODE_NONE;
    for (int i = 0; i < a->nfill && f; i++) {
        const char *eq = strchr(a->fill[i], '=');
        if (!eq) continue;
        hk_str name = {a->fill[i], (size_t)(eq - a->fill[i])};
        for (uint32_t k = 0; k < pg->dl.items.len; k++) {
            const hk_item *it = &pg->dl.items.data[k];
            if (it->kind != HK_ITEM_CONTROL) continue;
            const hk_attr *n = hk_dom_attr(pg->doc, it->node, HK_ATTR_NS_NONE, A(name));
            if (!n || !hk_str_eq(n->value, name) || !hk_control_is_text(hk_control_kind_of(pg->doc, it->node))) continue;
            hk_forms_set_value(f, it->node, hk_str_from_cstr(eq + 1));
            last = it->node;
            break;
        }
    }
    a->nfill = 0;
    if (!last) HK_LOGW("--fill: no matching text control");
    if (a->screenshot && !a->submit_after_fill && hk_window_save_bmp(a->window, a->screenshot) != HK_OK)
        HK_LOGW("could not save %s", a->screenshot);
    if (a->submit_after_fill && last) {
        a->submit_after_fill = false;
        set_focus(a, last);
        implicit_submit(a, last);
        return;
    }
    a->submit_after_fill = false;
    maybe_exit(a);
}

/* Keys for the focused control. Returns true when consumed. */
static bool form_key(app *a, hk_key key, unsigned mods)
{
    page *pg = &a->page;
    if (!pg->focus) return false;
    hk_control_kind k = hk_control_kind_of(pg->doc, pg->focus);
    hk_forms *f = page_forms(a);
    bool shift = (mods & HK_MOD_SHIFT) != 0, ctrl = (mods & HK_MOD_CTRL) != 0;
    if (key == HK_KEY_ESCAPE) {
        set_focus(a, HK_NODE_NONE);
        return true;
    }
    if (key == HK_KEY_TAB) {
        /* The next (or previous) control in layout order. */
        hk_node_id first = 0, last = 0, prev = 0, next = 0;
        bool seen = false;
        for (uint32_t i = 0; i < pg->dl.items.len; i++) {
            const hk_item *it = &pg->dl.items.data[i];
            if (it->kind != HK_ITEM_CONTROL || hk_control_disabled(pg->doc, it->node)) continue;
            if (!first) first = it->node;
            last = it->node;
            if (it->node == pg->focus) seen = true;
            else if (!seen) prev = it->node;
            else if (!next) next = it->node;
        }
        hk_node_id to = shift ? (prev ? prev : last) : (next ? next : first);
        set_focus(a, to);
        scroll_to_focus(a);
        return true;
    }
    if (!hk_control_is_text(k)) {
        if (key == HK_KEY_SPACE || (key == HK_KEY_ENTER && k != HK_CTRL_CHECKBOX && k != HK_CTRL_RADIO)) {
            activate_control(a, pg->focus);
            return true;
        }
        return false;
    }
    bool changed = false;
    hk_edit_cmd cmd;
    switch (key) {
    case HK_KEY_LEFT: cmd = ctrl ? HK_EDIT_WORD_LEFT : HK_EDIT_LEFT; break;
    case HK_KEY_RIGHT: cmd = ctrl ? HK_EDIT_WORD_RIGHT : HK_EDIT_RIGHT; break;
    case HK_KEY_HOME: cmd = HK_EDIT_HOME; break;
    case HK_KEY_END: cmd = HK_EDIT_END; break;
    case HK_KEY_BACKSPACE: cmd = ctrl ? HK_EDIT_WORD_BACKSPACE : HK_EDIT_BACKSPACE; break;
    case HK_KEY_DELETE: cmd = ctrl ? HK_EDIT_WORD_DELETE : HK_EDIT_DELETE; break;
    case HK_KEY_ENTER:
        if (k == HK_CTRL_TEXTAREA) {
            hk_forms_insert(f, pg->focus, HK_STR("\n"));
            update_text_input(a);
            hk_window_invalidate(a->window);
        } else {
            implicit_submit(a, pg->focus);
        }
        return true;
    case HK_KEY_SPACE: return true; /* the character arrives through on_char */
    case HK_KEY_UP:
    case HK_KEY_DOWN:
    case HK_KEY_PAGE_UP:
    case HK_KEY_PAGE_DOWN: return k == HK_CTRL_TEXTAREA;
    default:
        if (ctrl && key == HK_KEY_LETTER('A')) {
            cmd = HK_EDIT_SELECT_ALL;
            break;
        }
        if (ctrl && (key == HK_KEY_LETTER('C') || key == HK_KEY_LETTER('X'))) {
            hk_str sel = hk_forms_selected_text(f, pg->focus);
            if (sel.n) hk_plat_clipboard_set(sel);
            if (key == HK_KEY_LETTER('X') && sel.n && hk_forms_edit(f, pg->focus, HK_EDIT_DELETE, false)) changed = true;
            if (changed) hk_window_invalidate(a->window);
            return true;
        }
        if (ctrl && key == HK_KEY_LETTER('V')) {
            hk_charbuf clip = {0};
            if (hk_plat_clipboard_get(&clip)) hk_forms_insert(f, pg->focus, (hk_str){clip.data ? clip.data : "", clip.len});
            hk_vec_free(&clip);
            update_text_input(a);
            hk_window_invalidate(a->window);
            return true;
        }
        return false;
    }
    hk_forms_edit(f, pg->focus, cmd, shift);
    update_text_input(a);
    hk_window_invalidate(a->window);
    return true;
}

static void on_char(void *user, hk_window *w, uint32_t cp)
{
    HK_UNUSED(w);
    app *a = user;
    page *pg = &a->page;
    if (!focused_text(a)) return; /* Space on other controls is handled as a key */
    uint8_t u[4];
    size_t n = hk_utf8_encode(cp, u);
    hk_forms_insert(page_forms(a), pg->focus, (hk_str){(const char *)u, n});
    update_text_input(a);
    hk_window_invalidate(a->window);
}

static void on_composition(void *user, hk_window *w, const uint16_t *text, uint32_t len, uint32_t caret)
{
    HK_UNUSED(w);
    app *a = user;
    page *pg = &a->page;
    if (!focused_text(a)) return;
    if (len > HK_ARRAY_LEN(pg->comp)) len = HK_ARRAY_LEN(pg->comp);
    if (len) memcpy(pg->comp, text, len * sizeof(uint16_t));
    pg->comp_len = len;
    pg->comp_caret = caret;
    update_text_input(a);
    hk_window_invalidate(a->window);
}

/* --- window callbacks ------------------------------------------------------------------------------ */

static void on_paint(void *user, hk_window *w, hk_surface *s)
{
    app *a = user;
    page *pg = &a->page;
    if (pg->laid_out) {
        if (pg->relayout_pending) relayout_with(a, false); /* images changed sizes */
        hk_paint_hooks hooks = {a, paint_image, paint_control};
        hk_paint(a->painter, &pg->dl, s, a->scroll, hk_window_scale(w), &hooks);
        return;
    }
    for (int y = 0; y < s->height; y++) {
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        for (int x = 0; x < s->width; x++) row[x] = 0xFFFFFFFFu;
    }
}

static void on_resize(void *user, hk_window *w, int width, int height)
{
    HK_UNUSED(w);
    HK_UNUSED(height);
    app *a = user;
    if (!a->page.laid_out || width == 0) return;
    float vw, vh;
    viewport_css(a, &vw, &vh);
    if (vw != a->page.layout_width) relayout(a);
}

static void scroll_by(app *a, float dy)
{
    float vw, vh;
    viewport_css(a, &vw, &vh);
    float max = a->page.dl.height - vh;
    float next = a->scroll + dy;
    if (next > max) next = max;
    if (next < 0) next = 0;
    if (next != a->scroll) {
        a->scroll = next;
        request_visible_images(a);
        hk_window_invalidate(a->window);
    }
}

static void on_key(void *user, hk_window *w, hk_key key, unsigned mods)
{
    HK_UNUSED(w);
    app *a = user;
    float vw, vh;
    viewport_css(a, &vw, &vh);
    float line = 48, pagestep = vh * 0.9f;
    if (!(mods & HK_MOD_ALT) && form_key(a, key, mods)) return;
    if ((mods & HK_MOD_ALT) && key == HK_KEY_LEFT) key = HK_KEY_BROWSER_BACK;
    if ((mods & HK_MOD_ALT) && key == HK_KEY_RIGHT) key = HK_KEY_BROWSER_FORWARD;
    switch (key) {
    case HK_KEY_UP: scroll_by(a, -line); break;
    case HK_KEY_DOWN: scroll_by(a, line); break;
    case HK_KEY_PAGE_UP: scroll_by(a, -pagestep); break;
    case HK_KEY_PAGE_DOWN: scroll_by(a, pagestep); break;
    case HK_KEY_SPACE: scroll_by(a, (mods & HK_MOD_SHIFT) ? -pagestep : pagestep); break;
    case HK_KEY_HOME: scroll_by(a, -a->scroll); break;
    case HK_KEY_END: scroll_by(a, a->page.dl.height); break;
    case HK_KEY_BROWSER_BACK: go_history(a, -1); break;
    case HK_KEY_BROWSER_FORWARD: go_history(a, 1); break;
    case HK_KEY_TAB: {
        /* Tab without a focused control: the first control on screen. */
        for (uint32_t i = 0; i < a->page.dl.items.len; i++) {
            const hk_item *it = &a->page.dl.items.data[i];
            if (it->kind == HK_ITEM_CONTROL && !hk_control_disabled(a->page.doc, it->node) &&
                (it->fixed || it->y + it->h > a->scroll)) {
                set_focus(a, it->node);
                scroll_to_focus(a);
                break;
            }
        }
        break;
    }
    case HK_KEY_F5:
        if ((mods & HK_MOD_SHIFT) && a->history_pos && a->history.data[a->history_pos - 1].post) {
            resubmit(a);
            break;
        }
        if (a->history_pos) {
            a->restore_scroll = a->scroll;
            go_history(a, 0);
        }
        break;
    case HK_KEY_ESCAPE:
        if (a->page.loading) {
            hk_loader_cancel(a->loader, a->page.load_id);
            a->page.loading = false;
            hk_window_set_cursor(a->window, HK_CURSOR_ARROW);
        }
        break;
    default: break;
    }
}

static void on_wheel(void *user, hk_window *w, float notches)
{
    HK_UNUSED(w);
    scroll_by(user, -notches * 3 * 40);
}

static void on_mouse(void *user, hk_window *w, hk_mouse_event ev, hk_mouse_button button, int x, int y)
{
    app *a = user;
    if (ev == HK_MOUSE_UP && button == HK_BUTTON_BACK) {
        go_history(a, -1);
        return;
    }
    if (ev == HK_MOUSE_UP && button == HK_BUTTON_FORWARD) {
        go_history(a, 1);
        return;
    }
    page *pg = &a->page;
    if (!pg->laid_out) return;
    float scale = hk_window_scale(w), cx = (float)x / scale, cy = (float)y / scale;
    hk_node_id link = hk_display_list_link_at(&pg->dl, cx, cy, a->scroll);
    hk_control_kind kind = link ? hk_control_kind_of(pg->doc, link) : HK_CTRL_NONE;
    if (ev == HK_MOUSE_MOVE) {
        if (pg->selecting && focused_text(a)) {
            const hk_item *it = hk_display_list_control(&pg->dl, pg->focus);
            if (it) {
                float sy = it->fixed ? 0 : a->scroll;
                hk_form_ctx c = form_ctx(a);
                uint32_t pos = hk_form_view_hit(a->form_view, &c, it, cx - it->x, cy + sy - it->y);
                hk_forms_set_caret(page_forms(a), pg->focus, pos, true);
                update_text_input(a);
                hk_window_invalidate(w);
            }
            return;
        }
        if (!pg->loading) {
            bool is_link = link && hk_dom_is_html(pg->doc, link, A(a));
            hk_window_set_cursor(w, hk_control_is_text(kind) ? HK_CURSOR_TEXT : is_link ? HK_CURSOR_HAND : HK_CURSOR_ARROW);
        }
        a->hover_link = link;
        return;
    }
    if (button != HK_BUTTON_LEFT) return;
    if (ev == HK_MOUSE_DOWN) {
        pg->pressed = link;
        if (hk_control_is_text(kind) && !hk_control_disabled(pg->doc, link)) {
            set_focus(a, link);
            const hk_item *it = hk_display_list_control(&pg->dl, link);
            if (it) {
                float sy = it->fixed ? 0 : a->scroll;
                hk_form_ctx c = form_ctx(a);
                uint32_t pos = hk_form_view_hit(a->form_view, &c, it, cx - it->x, cy + sy - it->y);
                hk_forms_set_caret(page_forms(a), link, pos, false);
                pg->selecting = true;
                update_text_input(a);
            }
        } else if (kind != HK_CTRL_NONE && !hk_control_disabled(pg->doc, link)) {
            set_focus(a, link);
        } else if (!link || hk_dom_is_html(pg->doc, link, A(a))) {
            if (pg->focus) set_focus(a, HK_NODE_NONE);
        }
        return;
    }
    /* HK_MOUSE_UP */
    pg->selecting = false;
    hk_node_id pressed = pg->pressed;
    pg->pressed = HK_NODE_NONE;
    if (!link || link != pressed) return;
    if (hk_dom_is_html(pg->doc, link, A(a))) {
        follow_link(a, link);
    } else if (hk_dom_is_html(pg->doc, link, A(label))) {
        hk_node_id ctl = hk_label_control(pg->doc, link);
        if (!ctl || hk_control_disabled(pg->doc, ctl)) return;
        set_focus(a, ctl);
        if (!hk_control_is_text(hk_control_kind_of(pg->doc, ctl))) activate_control(a, ctl);
    } else if (kind != HK_CTRL_NONE && !hk_control_is_text(kind)) {
        activate_control(a, link);
    }
}

static void on_close(void *user, hk_window *w)
{
    HK_UNUSED(w);
    ((app *)user)->window = NULL;
}

static void on_first_present(void *user, hk_window *w)
{
    app *a = user;
    hk_plat_mem_info mem;
    hk_plat_mem_info_get(&mem);
    HK_LOGI("first frame: %.1f ms after process start, private %.1f MiB, working set %.1f MiB",
            hk_plat_ms_since_process_start(), (double)mem.private_bytes / (1024.0 * 1024.0),
            (double)mem.working_set / (1024.0 * 1024.0));
    if (a->smoke) hk_window_close(w);
}

/* --- startup ------------------------------------------------------------------------------------------ */

static const char k_start_page[] =
    "<!doctype html><title>Hawk</title>"
    "<body style='font-family: sans-serif; margin: 4em auto; max-width: 40em; color: #333'>"
    "<h1 style='font-weight: 300'>Hawk</h1>"
    "<p>URL \xE3\x82\x92\xE3\x82\xB3\xE3\x83\x9E\xE3\x83\xB3\xE3\x83\x89\xE3\x83\xA9\xE3\x82\xA4\xE3\x83\xB3\xE3\x81\xA7"
    "\xE6\x8C\x87\xE5\xAE\x9A\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82</p>"
    "<p><code>hawk.exe https://ja.wikipedia.org/</code></p>"
    "<ul><li>Alt+\xE2\x86\x90 / Alt+\xE2\x86\x92: back / forward<li>F5: reload<li>Esc: stop</ul>";

/* フィルタリスト / ブロッカー用のフィルタリストがまだありません。 / 既定のリストをダウンロードする */
static const char k_lists_missing[] =
    "<h2 style='font-weight: 400'>\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xAB\xE3\x82\xBF\xE3\x83\xAA\xE3\x82\xB9\xE3\x83\x88"
    "</h2><p>\xE3\x83\x96\xE3\x83\xAD\xE3\x83\x83\xE3\x82\xAB\xE3\x83\xBC\xE7\x94\xA8\xE3\x81\xAE\xE3\x83\x95\xE3\x82\xA3"
    "\xE3\x83\xAB\xE3\x82\xBF\xE3\x83\xAA\xE3\x82\xB9\xE3\x83\x88\xE3\x81\x8C\xE3\x81\xBE\xE3\x81\xA0\xE3\x81\x82\xE3\x82"
    "\x8A\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93\xE3\x80\x82 EasyList, EasyPrivacy, uBlock filters, AdGuard Japanese, "
    "Peter Lowe's list</p>"
    "<p><a href='hawk:update-filters'>\xE6\x97\xA2\xE5\xAE\x9A\xE3\x81\xAE\xE3\x83\xAA\xE3\x82\xB9\xE3\x83\x88\xE3\x82"
    "\x92\xE3\x83\x80\xE3\x82\xA6\xE3\x83\xB3\xE3\x83\xAD\xE3\x83\xBC\xE3\x83\x89\xE3\x81\x99\xE3\x82\x8B</a></p>";

static void show_html(app *a, const char *html, size_t n)
{
    page *pg = &a->page;
    pg->doc = hk_html_parse(html, n);
    pg->parsed = true;
    pg->html = true;
    bool exit_after = a->exit_after_load;
    a->exit_after_load = false;
    show_document(a);
    a->exit_after_load = exit_after;
}

static void on_filters_updated(void *user, const char *summary)
{
    app *a = user;
    HK_LOGI("filter lists: %s", summary);
    if (strcmp(a->page.url, "hawk:update-filters") == 0) {
        if (a->exit_after_load) {
            hk_window_close(a->window);
            return;
        }
        /* フィルタリストを更新しました */
        show_message(a,
                     "\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xAB\xE3\x82\xBF\xE3\x83\xAA\xE3\x82\xB9\xE3\x83\x88\xE3\x82\x92"
                     "\xE6\x9B\xB4\xE6\x96\xB0\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F",
                     summary);
    }
}

static void show_internal(app *a, const char *url)
{
    hk_window_set_cursor(a->window, HK_CURSOR_ARROW);
    if (strcmp(url, "hawk:update-filters") == 0 && a->data_dir[0]) {
        bool exit_after = a->exit_after_load;
        a->exit_after_load = false;
        /* フィルタリストをダウンロードしています… */
        show_message(a,
                     "\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xAB\xE3\x82\xBF\xE3\x83\xAA\xE3\x82\xB9\xE3\x83\x88\xE3\x82\x92"
                     "\xE3\x83\x80\xE3\x82\xA6\xE3\x83\xB3\xE3\x83\xAD\xE3\x83\xBC\xE3\x83\x89\xE3\x81\x97\xE3\x81\xA6"
                     "\xE3\x81\x84\xE3\x81\xBE\xE3\x81\x99\xE2\x80\xA6",
                     "EasyList, EasyPrivacy, uBlock filters, AdGuard Japanese, Peter Lowe's list");
        a->exit_after_load = exit_after; /* --exit-after-load waits for the lists */
        hk_filter_lists_update(a->loader, a->data_dir, true, on_filters_updated, a);
        return;
    }
    hk_charbuf h = {0};
    hk_charbuf_append(&h, k_start_page, sizeof k_start_page - 1);
    if (hk_filter_lists_missing(a->data_dir[0] ? a->data_dir : NULL))
        hk_charbuf_append(&h, k_lists_missing, sizeof k_lists_missing - 1);
    show_html(a, h.data, h.len);
    hk_vec_free(&h);
}

int hk_app_main(int argc, char **argv)
{
    static app a; /* lives for the whole process */
    const char *url = NULL;
    int width_css = 1200;
    hk_names_init();
    const char *extra_lists[8];
    int nextra = 0;
    bool update_filters = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0) a.smoke = true;
        else if (strcmp(argv[i], "--exit-after-load") == 0) a.exit_after_load = true;
        else if (strcmp(argv[i], "--wait-images") == 0) a.wait_images = true;
        else if (strcmp(argv[i], "--filters") == 0 && i + 1 < argc) {
            if (nextra < 8) extra_lists[nextra++] = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--update-filters") == 0) update_filters = true;
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) a.screenshot = argv[++i];
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) width_css = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fill") == 0 && i + 1 < argc) {
            if (a.nfill < 8) a.fill[a.nfill++] = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--submit") == 0) a.submit_after_fill = true;
        else if (argv[i][0] != '-' && !url) url = argv[i];
        else HK_LOGW("unknown argument: %s", argv[i]);
    }
    hk_charbuf dir = {0};
    if (hk_plat_data_dir(&dir) && dir.len < sizeof a.data_dir) memcpy(a.data_dir, dir.data, dir.len);
    hk_vec_free(&dir);
    const char *data_dir = a.data_dir[0] ? a.data_dir : NULL;
    a.loader = hk_loader_create(NULL, data_dir);
    hk_filter_store *store = hk_filter_store_open(data_dir, extra_lists, nextra);
    hk_loader_set_blocker(a.loader, hk_filter_store_blocker(store), hk_filter_store_release, store);
    a.painter = hk_painter_create();
    a.form_view = hk_form_view_create();
    hk_arena_init(&a.page.url_arena, 4096);

    hk_window_desc desc = {0};
    desc.title = HK_STR("Hawk");
    desc.width = width_css > 200 ? width_css : 1200;
    desc.height = 800;
    desc.cb.user = &a;
    desc.cb.on_paint = on_paint;
    desc.cb.on_resize = on_resize;
    desc.cb.on_key = on_key;
    desc.cb.on_wheel = on_wheel;
    desc.cb.on_mouse = on_mouse;
    desc.cb.on_char = on_char;
    desc.cb.on_close = on_close;
    desc.cb.on_composition = on_composition;
    desc.cb.on_first_present = on_first_present;

    hk_status st = hk_window_create(&desc, &a.window);
    if (st != HK_OK) {
        HK_LOGE("could not create the main window: %s", hk_status_name(st));
        return 1;
    }
    if (update_filters) {
        navigate(&a, "hawk:update-filters", true); /* every default list, now */
    } else {
        navigate(&a, url ? url : "hawk:start", true);
        /* Lists that are due are refreshed in the background (the first download is the user's call). */
        if (data_dir && !hk_filter_lists_missing(data_dir))
            hk_filter_lists_update(a.loader, data_dir, false, on_filters_updated, &a);
    }
    int rc = hk_plat_run_loop();

    page_reset(&a); /* cancels the page's requests, so before the loader goes */
    hk_loader_destroy(a.loader);
    hk_arena_free(&a.page.url_arena);
    hk_painter_destroy(a.painter);
    hk_form_view_destroy(a.form_view);
    free_history_from(&a, 0);
    hk_vec_free(&a.history);
    return rc;
}
