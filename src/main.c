/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Hawk application (M1): loads one URL and shows its text.
 *
 *   hawk.exe [options] [URL]
 *     --filters FILE      add a filter list (in addition to %LOCALAPPDATA%\Hawk\filters\*.txt)
 *     --exit-after-load   log load statistics and exit when the page has loaded
 *     --screenshot FILE   with --exit-after-load: save the window as a BMP before exiting
 *     --smoke             exit after the first frame (startup time and memory)
 */
#include "base/hk_log.h"
#include "base/hk_mem.h"
#include "blocker/hk_blocker.h"
#include "dom/hk_dom.h"
#include "html/hk_html_parser.h"
#include "net/hk_encoding.h"
#include "net/hk_loader.h"
#include "plat/hk_plat.h"
#include "plat/hk_textview.h"
#include "plat/hk_window.h"
#include "shell/hk_textdoc.h"

#include <stdio.h>
#include <string.h>

#define COLOR_BG_LIGHT 0xFFFFFFFFu
#define COLOR_BG_DARK 0xFF1E1E1Eu
#define SNIFF_BYTES 1024

typedef struct page {
    char url[2048];
    uint32_t load_id;
    bool loading;
    bool html, plain, unsupported;
    hk_encoding transport, encoding;
    bool decoder_ready;
    hk_decoder decoder;
    hk_charbuf sniff;
    hk_charbuf utf8;
    hk_charbuf plain_text;
    hk_document *doc;
    hk_html_parser *parser;
    uint64_t t_start, t_first_byte, t_done;
    size_t bytes;
    char content_type[128];
} page;

typedef struct app {
    hk_window *window;
    hk_textview *view;
    hk_loader *loader;
    hk_blocker blocker;
    page page;
    int scroll;
    bool smoke, exit_after_load;
    const char *screenshot;
    bool layout_dirty;
} app;

/* --- page lifecycle ------------------------------------------------------------------------ */

static void page_reset(page *pg)
{
    if (pg->parser) hk_html_parser_destroy(pg->parser);
    if (pg->doc) hk_document_destroy(pg->doc);
    if (pg->decoder_ready) hk_decoder_free(&pg->decoder);
    hk_vec_free(&pg->sniff);
    hk_vec_free(&pg->utf8);
    hk_vec_free(&pg->plain_text);
    memset(pg, 0, sizeof *pg);
}

static void show_note(app *a, const char *text)
{
    hk_textview_add(a->view, HK_TV_NOTE, hk_str_from_cstr(text));
}

static void relayout(app *a)
{
    int w, h;
    hk_window_client_size(a->window, &w, &h);
    hk_textview_layout(a->view, w, hk_window_scale(a->window));
    int max = hk_textview_height(a->view) - h;
    if (a->scroll > max) a->scroll = max;
    if (a->scroll < 0) a->scroll = 0;
    a->layout_dirty = false;
    hk_window_invalidate(a->window);
}

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
    hk_str mime = r->content_type;
    size_t semi = hk_str_find_char(mime, ';');
    if (semi != HK_STR_NPOS) mime = hk_str_slice(mime, 0, semi);
    mime = hk_str_trim_ascii_ws(mime);
    pg->html = mime.n == 0 || hk_str_eq_ascii_icase(mime, HK_STR("text/html")) ||
               hk_str_eq_ascii_icase(mime, HK_STR("application/xhtml+xml"));
    pg->plain = !pg->html && hk_str_eq_ascii_icase(mime, HK_STR("text/plain"));
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

static void add_paragraph(void *user, hk_tv_kind kind, hk_str text)
{
    hk_textview_add(user, kind, text);
}

static void on_done(void *user, hk_status st, const char *message)
{
    app *a = user;
    page *pg = &a->page;
    pg->loading = false;
    pg->t_done = hk_plat_now_ns();
    hk_textview_clear(a->view);
    char line[2400];

    if (st == HK_OK && !pg->unsupported) {
        if (!pg->decoder_ready) start_decoding(a);
        decode_and_feed(a, NULL, 0, true);
    }
    if (st == HK_ERR_BLOCKED) {
        snprintf(line, sizeof line, "\xE3\x83\x96\xE3\x83\xAD\xE3\x83\x83\xE3\x82\xAB\xE3\x83\xBC\xE3\x81\x8C"
                                    "\xE3\x81\x93\xE3\x81\xAE\xE3\x83\x9A\xE3\x83\xBC\xE3\x82\xB8\xE3\x82\x92"
                                    "\xE9\x81\xAE\xE6\x96\xAD\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F: %s",
                 message); /* ブロッカーがこのページを遮断しました */
        show_note(a, line);
    } else if (st != HK_OK) {
        snprintf(line, sizeof line, "\xE8\xAA\xAD\xE3\x81\xBF\xE8\xBE\xBC\xE3\x82\x81\xE3\x81\xBE\xE3\x81\x9B"
                                    "\xE3\x82\x93\xE3\x81\xA7\xE3\x81\x97\xE3\x81\x9F: %s (%s)",
                 message, a->page.url[0] ? a->page.url : ""); /* 読み込めませんでした */
        show_note(a, line);
    } else if (pg->unsupported) {
        snprintf(line, sizeof line, "\xE3\x81\x93\xE3\x81\xAE\xE5\xBD\xA2\xE5\xBC\x8F\xE3\x81\xAF\xE8\xA1\xA8"
                                    "\xE7\xA4\xBA\xE3\x81\xA7\xE3\x81\x8D\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93: %s",
                 pg->content_type); /* この形式は表示できません */
        show_note(a, line);
    } else if (pg->plain) {
        hk_textview_add(a->view, HK_TV_PRE, (hk_str){pg->plain_text.data, pg->plain_text.len});
        hk_window_set_title(a->window, hk_str_from_cstr(pg->url));
    } else {
        hk_html_parser_finish(pg->parser);
        hk_str title;
        hk_textdoc_build(pg->doc, add_paragraph, a->view, &title);
        hk_charbuf t = {0};
        if (title.n) {
            hk_charbuf_append(&t, title.p, title.n);
            hk_charbuf_append(&t, " - ", 3);
        }
        hk_charbuf_append(&t, "Hawk", 4);
        hk_window_set_title(a->window, (hk_str){t.data, t.len});
        hk_vec_free(&t);
    }
    a->scroll = 0;
    relayout(a);

    hk_plat_mem_info mem;
    hk_plat_mem_info_get(&mem);
    double ms = (double)(pg->t_done - pg->t_start) / 1e6;
    double ttfb = pg->t_first_byte ? (double)(pg->t_first_byte - pg->t_start) / 1e6 : 0;
    HK_LOGI("loaded %s: %s, %zu bytes in %.0f ms (first byte %.0f ms), encoding %s, %u nodes, "
            "document %.2f MiB, private %.1f MiB",
            pg->url, st == HK_OK ? "ok" : message, pg->bytes, ms, ttfb, hk_encoding_name(pg->encoding),
            pg->doc ? pg->doc->nodes.len - 1 : 0, pg->doc ? (double)hk_document_memory(pg->doc) / 1048576.0 : 0.0,
            (double)mem.private_bytes / 1048576.0);
    if (a->exit_after_load) {
        if (a->screenshot && hk_window_save_bmp(a->window, a->screenshot) != HK_OK)
            HK_LOGW("could not save %s", a->screenshot);
        hk_window_close(a->window);
    }
}

static void navigate(app *a, const char *url)
{
    if (a->page.loading) hk_loader_cancel(a->loader, a->page.load_id);
    page_reset(&a->page);
    page *pg = &a->page;
    snprintf(pg->url, sizeof pg->url, "%s", url);
    pg->loading = true;
    pg->t_start = hk_plat_now_ns();
    hk_textview_clear(a->view);
    char line[2100];
    snprintf(line, sizeof line, "\xE8\xAA\xAD\xE3\x81\xBF\xE8\xBE\xBC\xE3\x81\xBF\xE4\xB8\xAD: %s", url); /* 読み込み中 */
    show_note(a, line);
    relayout(a);
    hk_load_callbacks cb = {a, on_response, on_data, on_done};
    pg->load_id = hk_loader_fetch(a->loader, hk_str_from_cstr(url), &cb);
}

/* --- window callbacks ------------------------------------------------------------------------- */

static void on_paint(void *user, hk_window *w, hk_surface *s)
{
    app *a = user;
    if (a->layout_dirty) relayout(a);
    hk_textview_paint(a->view, s, a->scroll, hk_window_is_dark(w));
}

static void on_resize(void *user, hk_window *w, int width, int height)
{
    HK_UNUSED(w);
    HK_UNUSED(width);
    HK_UNUSED(height);
    app *a = user;
    a->layout_dirty = true;
}

static void scroll_by(app *a, int dy)
{
    int w, h;
    hk_window_client_size(a->window, &w, &h);
    int max = hk_textview_height(a->view) - h;
    int next = a->scroll + dy;
    if (next > max) next = max;
    if (next < 0) next = 0;
    if (next != a->scroll) {
        a->scroll = next;
        hk_window_invalidate(a->window);
    }
}

static void on_key(void *user, hk_window *w, hk_key key, unsigned mods)
{
    app *a = user;
    int cw, ch;
    hk_window_client_size(w, &cw, &ch);
    int line = (int)(48 * hk_window_scale(w));
    int pagestep = ch * 9 / 10;
    switch (key) {
    case HK_KEY_UP: scroll_by(a, -line); break;
    case HK_KEY_DOWN: scroll_by(a, line); break;
    case HK_KEY_PAGE_UP: scroll_by(a, -pagestep); break;
    case HK_KEY_PAGE_DOWN: scroll_by(a, pagestep); break;
    case HK_KEY_SPACE: scroll_by(a, (mods & HK_MOD_SHIFT) ? -pagestep : pagestep); break;
    case HK_KEY_HOME: scroll_by(a, -a->scroll); break;
    case HK_KEY_END: scroll_by(a, hk_textview_height(a->view)); break;
    case HK_KEY_F5:
        if (a->page.url[0]) {
            char url[2048];
            snprintf(url, sizeof url, "%s", a->page.url);
            navigate(a, url);
        }
        break;
    case HK_KEY_ESCAPE:
        if (a->page.loading) {
            hk_loader_cancel(a->loader, a->page.load_id);
            a->page.loading = false;
        }
        break;
    default: break;
    }
}

static void on_wheel(void *user, hk_window *w, float notches)
{
    app *a = user;
    scroll_by(a, (int)(-notches * 3 * 40 * hk_window_scale(w)));
}

static void on_theme_changed(void *user, hk_window *w, bool dark)
{
    HK_UNUSED(user);
    HK_UNUSED(dark);
    hk_window_invalidate(w);
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

/* --- startup ------------------------------------------------------------------------------------ */

static void load_filter_file(void *user, const char *path)
{
    hk_blocker *b = user;
    hk_charbuf text = {0};
    if (hk_plat_read_file(path, &text) == HK_OK) {
        uint32_t n = hk_blocker_add_list(b, (hk_str){text.data, text.len});
        HK_LOGI("filter list %s: %u host filters", path, n);
    } else {
        HK_LOGW("cannot read filter list %s", path);
    }
    hk_vec_free(&text);
}

int hk_app_main(int argc, char **argv)
{
    static app a; /* lives for the whole process */
    const char *url = NULL;
    hk_names_init();
    hk_blocker_init(&a.blocker);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0) a.smoke = true;
        else if (strcmp(argv[i], "--exit-after-load") == 0) a.exit_after_load = true;
        else if (strcmp(argv[i], "--filters") == 0 && i + 1 < argc) load_filter_file(&a.blocker, argv[++i]);
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) a.screenshot = argv[++i];
        else if (argv[i][0] != '-' && !url) url = argv[i];
        else HK_LOGW("unknown argument: %s", argv[i]);
    }
    hk_charbuf dir = {0};
    if (hk_plat_data_dir(&dir)) {
        hk_charbuf_append(&dir, "\\filters", 9); /* includes the NUL */
        hk_plat_list_files(dir.data, ".txt", load_filter_file, &a.blocker);
    }
    hk_vec_free(&dir);

    a.view = hk_textview_create();
    a.loader = hk_loader_create(&a.blocker);

    hk_window_desc desc = {0};
    desc.title = HK_STR("Hawk");
    desc.width = 1200;
    desc.height = 800;
    desc.cb.user = &a;
    desc.cb.on_paint = on_paint;
    desc.cb.on_resize = on_resize;
    desc.cb.on_key = on_key;
    desc.cb.on_wheel = on_wheel;
    desc.cb.on_theme_changed = on_theme_changed;
    desc.cb.on_first_present = on_first_present;

    hk_status st = hk_window_create(&desc, &a.window);
    if (st != HK_OK) {
        HK_LOGE("could not create the main window: %s", hk_status_name(st));
        return 1;
    }
    if (url) {
        navigate(&a, url);
    } else {
        show_note(&a, "Hawk M1: " /* URL をコマンドラインで指定してください */
                      "URL \xE3\x82\x92\xE3\x82\xB3\xE3\x83\x9E\xE3\x83\xB3\xE3\x83\x89\xE3\x83\xA9\xE3\x82"
                      "\xA4\xE3\x83\xB3\xE3\x81\xA7\xE6\x8C\x87\xE5\xAE\x9A\xE3\x81\x97\xE3\x81\xA6\xE3\x81"
                      "\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82 hawk.exe https://ja.wikipedia.org/");
        a.layout_dirty = true;
    }
    int rc = hk_plat_run_loop();

    hk_loader_destroy(a.loader);
    page_reset(&a.page);
    hk_textview_destroy(a.view);
    hk_blocker_free(&a.blocker);
    return rc;
}
