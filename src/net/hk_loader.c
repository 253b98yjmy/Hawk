/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net/hk_loader.h"
#include "base/hk_atomic.h"
#include "base/hk_log.h"
#include "base/hk_mem.h"
#include "base/hk_vec.h"
#include "net/hk_cache.h"
#include "net/hk_cookies.h"
#include "net/hk_http.h"
#include "net/hk_psl.h"
#include "net/hk_url.h"
#include "plat/hk_plat.h"
#include "plat/hk_thread.h"

#include <stdio.h>
#include <string.h>

#define MAX_REDIRECTS 20

typedef struct request {
    struct request *next;
    uint32_t id;
    hk_load_callbacks cb;
    char *url;
    size_t url_len;
    volatile int64_t cancelled;
    bool image;
    uint32_t type; /* HK_REQ_* */
    int image_w, image_h;
    hk_charbuf body; /* image requests: the whole body (I/O thread) */
    char *site;      /* top-level page URL; NULL for navigations (each hop is its own site) */
    size_t site_len;
    hk_charbuf post;    /* form submissions: the body ... */
    bool is_post;
    hk_charbuf post_headers; /* ... and its Content-Type and Origin header lines */
    char *referrer;          /* navigations from a page: its URL (or origin) */
    size_t referrer_len;
} request;

struct hk_loader {
    const hk_blocker *blocker; /* I/O thread; replaced between requests */
    void (*blocker_release)(void *);
    void *blocker_ctx;
    bool swap_pending; /* guarded by lock */
    const hk_blocker *next_blocker;
    void (*next_release)(void *);
    void *next_ctx;
    hk_thread *thread;
    hk_mutex lock;
    hk_cond wake;
    request *queue_head, *queue_tail;
    bool stopping;
    uint32_t next_id;
    HK_VEC(request *) live; /* main thread: requests that have not delivered on_done */
    /* I/O thread state */
    char data_dir[512];
    hk_http_pool *pool;
    hk_cookie_jar jar;
    hk_cache *cache;
};

/* --- messages to the main thread ------------------------------------------------------- */

typedef enum { MSG_RESPONSE, MSG_DATA, MSG_DONE } msg_kind;

typedef struct msg {
    msg_kind kind;
    hk_loader *loader;
    request *req;
    hk_status status;
    int http_status;
    char *bytes; /* URL + '\0' + content type for MSG_RESPONSE; body bytes; or the error text */
    size_t len;
    size_t split;
    hk_image image; /* MSG_DONE of image requests */
} msg;

static void remove_live(hk_loader *l, request *r)
{
    for (uint32_t i = 0; i < l->live.len; i++) {
        if (l->live.data[i] == r) {
            l->live.data[i] = l->live.data[--l->live.len];
            return;
        }
    }
}

static void free_request(request *r)
{
    if (r->site) hk_free(r->site, r->site_len + 1);
    hk_vec_free(&r->body);
    hk_vec_free(&r->post);
    hk_vec_free(&r->post_headers);
    if (r->referrer) hk_free(r->referrer, r->referrer_len + 1);
    hk_free(r->url, r->url_len + 1);
    hk_free(r, sizeof *r);
}

static void deliver(void *arg)
{
    msg *m = arg;
    request *r = m->req;
    bool active = hk_atomic_load64(&r->cancelled) == 0;
    if (active) {
        switch (m->kind) {
        case MSG_RESPONSE:
            if (r->cb.on_response) {
                hk_load_response resp = {{m->bytes, m->split}, m->http_status,
                                         {m->bytes + m->split + 1, m->len - m->split - 1}};
                r->cb.on_response(r->cb.user, &resp);
            }
            break;
        case MSG_DATA:
            if (r->cb.on_data) r->cb.on_data(r->cb.user, (const uint8_t *)m->bytes, m->len);
            break;
        case MSG_DONE:
            if (r->image) {
                if (r->cb.on_image) r->cb.on_image(r->cb.user, m->status, &m->image);
                m->image.pixels = NULL; /* ownership moved to the receiver */
            } else if (r->cb.on_done) {
                r->cb.on_done(r->cb.user, m->status, m->bytes ? m->bytes : "");
            }
            break;
        }
    }
    hk_image_free(&m->image);
    if (m->kind == MSG_DONE) {
        remove_live(m->loader, r);
        free_request(r);
    }
    hk_free(m->bytes, m->len + 1);
    hk_free(m, sizeof *m);
}

static void post(hk_loader *l, request *r, msg_kind kind, hk_status st, int http_status, const char *bytes,
                 size_t len, size_t split)
{
    msg *m = hk_alloc_zero(sizeof *m);
    m->kind = kind;
    m->loader = l;
    m->req = r;
    m->status = st;
    m->http_status = http_status;
    if (bytes) {
        m->bytes = hk_alloc(len + 1);
        memcpy(m->bytes, bytes, len);
        m->bytes[len] = 0;
        m->len = len;
    }
    m->split = split;
    hk_main_post(deliver, m);
}

/* --- one request on the I/O thread ------------------------------------------------------- */

typedef struct fetch_ctx {
    hk_loader *loader;
    request *req;
    const char *url;
    size_t url_len;
    bool redirect;
    int redirect_status;
    hk_str location;
    hk_arena *arena;
    const hk_url *target;
    hk_str site_host;
    hk_cache_entry *cached; /* entry being revalidated, or NULL */
    bool not_modified;
    hk_charbuf capture; /* the body, for the cache */
    bool capture_ok;
} fetch_ctx;

#define MAX_CAPTURE (8u * 1024 * 1024)

static void post_response(fetch_ctx *f, int status, hk_str content_type)
{
    hk_charbuf b = {0};
    hk_charbuf_append(&b, f->url, f->url_len);
    hk_charbuf_push(&b, 0);
    hk_charbuf_append(&b, content_type.p, content_type.n);
    post(f->loader, f->req, MSG_RESPONSE, HK_OK, status, b.data, b.len, f->url_len);
    hk_vec_free(&b);
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool on_headers(void *user, const hk_http_response *r)
{
    fetch_ctx *f = user;
    int64_t now = hk_plat_unix_time();
    for (uint32_t i = 0; i < r->headers.len; i++) {
        if (hk_str_eq_ascii_icase(r->headers.data[i].name, HK_STR("Set-Cookie")))
            hk_cookie_set(&f->loader->jar, f->target, f->site_host, r->headers.data[i].value, now);
    }
    if (r->status == 304 && f->cached) {
        f->not_modified = true;
        return false;
    }
    bool has_location = false;
    hk_str location = hk_http_header_get(r, "Location", &has_location);
    if (is_redirect(r->status) && has_location) {
        f->redirect = true;
        f->redirect_status = r->status;
        f->location = hk_arena_str_dup(f->arena, location);
        return false;
    }
    if (!f->req->image) post_response(f, r->status, hk_http_header_get(r, "Content-Type", NULL));
    f->capture_ok = r->status == 200;
    return true;
}

#define MAX_IMAGE_BYTES (32u * 1024 * 1024)

static bool on_body(void *user, const uint8_t *p, size_t n)
{
    fetch_ctx *f = user;
    if (f->req->image) {
        if (f->req->body.len + n > MAX_IMAGE_BYTES) return false;
        hk_charbuf_append(&f->req->body, (const char *)p, n);
        return hk_atomic_load64(&f->req->cancelled) == 0;
    }
    if (f->capture_ok) {
        if (f->capture.len + n > MAX_CAPTURE) {
            f->capture_ok = false;
            hk_vec_free(&f->capture);
        } else {
            hk_charbuf_append(&f->capture, (const char *)p, n);
        }
    }
    post(f->loader, f->req, MSG_DATA, HK_OK, 0, (const char *)p, n, 0);
    return hk_atomic_load64(&f->req->cancelled) == 0;
}

/* Delivers a cached body as if it had been received. */
static void serve_cached(fetch_ctx *f, const hk_cache_entry *e)
{
    if (f->req->image) {
        hk_vec_free(&f->req->body);
        hk_charbuf_append(&f->req->body, e->body.data, e->body.len);
        return;
    }
    post_response(f, e->status, hk_str_from_cstr(e->content_type));
    for (uint32_t off = 0; off < e->body.len; off += 65536) {
        uint32_t n = e->body.len - off < 65536 ? e->body.len - off : 65536;
        post(f->loader, f->req, MSG_DATA, HK_OK, 0, e->body.data + off, n, 0);
    }
}

static bool cancelled(void *user)
{
    fetch_ctx *f = user;
    return hk_atomic_load64(&f->req->cancelled) != 0;
}

static void finish_request(hk_loader *l, request *r, hk_status st, const char *err)
{
    if (hk_atomic_load64(&r->cancelled)) st = HK_ERR_IO;
    if (!r->image) {
        post(l, r, MSG_DONE, st, 0, st == HK_OK ? NULL : err, strlen(err), 0);
        return;
    }
    msg *m = hk_alloc_zero(sizeof *m);
    m->kind = MSG_DONE;
    m->loader = l;
    m->req = r;
    m->status = st;
    if (st == HK_OK)
        m->status = hk_image_decode((const uint8_t *)r->body.data, r->body.len, r->image_w, r->image_h, &m->image);
    hk_vec_free(&r->body);
    hk_main_post(deliver, m);
}

static int b64(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* data: URLs (RFC 2397) are answered without any network access. */
static void run_data_url(hk_loader *l, request *r)
{
    hk_str u = {r->url, r->url_len};
    size_t comma = hk_str_find_char(u, ',');
    if (comma == HK_STR_NPOS) {
        finish_request(l, r, HK_ERR_INVALID, "invalid data: URL");
        return;
    }
    hk_str meta = hk_str_slice(u, 5, comma), payload = hk_str_slice(u, comma + 1, u.n);
    bool base64 = hk_str_ends_with(meta, HK_STR(";base64"));
    if (base64) meta.n -= 7;
    hk_charbuf body = {0};
    if (base64) {
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = 0; i < payload.n; i++) {
            int v = b64((unsigned char)payload.p[i]);
            if (v < 0) continue; /* whitespace and padding */
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                hk_charbuf_push(&body, (char)((acc >> bits) & 0xFF));
            }
        }
    } else {
        for (size_t i = 0; i < payload.n; i++) {
            int h1 = i + 2 < payload.n ? hexv((unsigned char)payload.p[i + 1]) : -1;
            int h2 = i + 2 < payload.n ? hexv((unsigned char)payload.p[i + 2]) : -1;
            if (payload.p[i] == '%' && h1 >= 0 && h2 >= 0) {
                hk_charbuf_push(&body, (char)(h1 * 16 + h2));
                i += 2;
            } else {
                hk_charbuf_push(&body, payload.p[i]);
            }
        }
    }
    if (r->image) {
        hk_vec_free(&r->body);
        r->body = body;
        finish_request(l, r, HK_OK, "");
        return;
    }
    size_t shown = r->url_len > 256 ? 256 : r->url_len;
    hk_charbuf head = {0};
    hk_charbuf_append(&head, r->url, shown);
    hk_charbuf_push(&head, 0);
    if (meta.n) hk_charbuf_append(&head, meta.p, meta.n);
    else hk_charbuf_append(&head, "text/plain", 10);
    post(l, r, MSG_RESPONSE, HK_OK, 200, head.data, head.len, shown);
    if (body.len) post(l, r, MSG_DATA, HK_OK, 0, body.data, body.len, 0);
    hk_vec_free(&head);
    hk_vec_free(&body);
    finish_request(l, r, HK_OK, "");
}

static const char *file_type(hk_str path)
{
    static const struct {
        const char *ext, *type;
    } k_types[] = {
        {".html", "text/html"}, {".htm", "text/html"},  {".xhtml", "application/xhtml+xml"},
        {".css", "text/css"},   {".txt", "text/plain"}, {".json", "application/json"},
        {".png", "image/png"},  {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"},  {".webp", "image/webp"}, {".svg", "image/svg+xml"},
        {".ico", "image/x-icon"},
    };
    for (size_t i = 0; i < HK_ARRAY_LEN(k_types); i++) {
        hk_str ext = hk_str_from_cstr(k_types[i].ext);
        if (path.n >= ext.n && hk_str_eq_ascii_icase(hk_str_slice(path, path.n - ext.n, path.n), ext)) return k_types[i].type;
    }
    return "application/octet-stream";
}

/* file:///C:/dir/a%20b.html -> C:\dir\a b.html (UTF-8). */
static void file_path(const hk_url *u, hk_charbuf *out)
{
    hk_str p = u->path;
    size_t i = 0;
    if (p.n >= 3 && p.p[0] == '/' && p.p[2] == ':') i = 1; /* drive letter */
    if (u->host.n) {                                       /* \\host\share */
        hk_charbuf_append(out, "\\\\", 2);
        hk_charbuf_append(out, u->host.p, u->host.n);
    }
    for (; i < p.n; i++) {
        int h1 = i + 2 < p.n ? hexv((unsigned char)p.p[i + 1]) : -1;
        int h2 = i + 2 < p.n ? hexv((unsigned char)p.p[i + 2]) : -1;
        if (p.p[i] == '%' && h1 >= 0 && h2 >= 0) {
            hk_charbuf_push(out, (char)(h1 * 16 + h2));
            i += 2;
        } else {
            hk_charbuf_push(out, p.p[i] == '/' ? '\\' : p.p[i]);
        }
    }
    hk_charbuf_push(out, 0);
}

static void run_file_url(hk_loader *l, request *r, const hk_url *u)
{
    /* Web pages never read local files: only navigations and pages that are files themselves. */
    if (r->site && !(r->site_len >= 5 && hk_str_eq_ascii_icase((hk_str){r->site, 5}, HK_STR("file:")))) {
        finish_request(l, r, HK_ERR_BLOCKED, "a web page cannot load local files");
        return;
    }
    hk_charbuf path = {0}, body = {0};
    file_path(u, &path);
    hk_status st = hk_plat_read_file(path.data, &body);
    hk_vec_free(&path);
    if (st != HK_OK) {
        hk_vec_free(&body);
        finish_request(l, r, st, "file not found");
        return;
    }
    if (r->image) {
        hk_vec_free(&r->body);
        r->body = body;
        finish_request(l, r, HK_OK, "");
        return;
    }
    hk_charbuf head = {0};
    hk_charbuf_append(&head, u->href.p, u->href.n);
    hk_charbuf_push(&head, 0);
    const char *type = file_type(u->path);
    hk_charbuf_append(&head, type, strlen(type));
    post(l, r, MSG_RESPONSE, HK_OK, 200, head.data, head.len, u->href.n);
    for (uint32_t off = 0; off < body.len; off += 65536) {
        uint32_t n = body.len - off < 65536 ? body.len - off : 65536;
        post(l, r, MSG_DATA, HK_OK, 0, body.data + off, n, 0);
    }
    hk_vec_free(&head);
    hk_vec_free(&body);
    finish_request(l, r, HK_OK, "");
}

static bool same_origin(const hk_url *a, const hk_url *b)
{
    return hk_str_eq(a->scheme, b->scheme) && hk_str_eq(a->host, b->host) && a->port == b->port;
}

/* The Referer header line for a request to target (docs/design/02-network.md, 5), appended to out. */
static void referer_header(hk_arena *a, const request *r, const hk_url *target, hk_charbuf *out)
{
    bool navigation = r->site == NULL;
    hk_str from_s = navigation ? (hk_str){r->referrer, r->referrer_len} : (hk_str){r->site, r->site_len};
    hk_url from;
    if (!from_s.n || hk_url_parse(a, from_s, NULL, &from) != HK_OK || !from.host.n) return;
    if (!hk_str_eq(from.scheme, HK_STR("https")) && !hk_str_eq(from.scheme, HK_STR("http"))) return;
    if (hk_str_eq(from.scheme, HK_STR("https")) && !hk_str_eq(target->scheme, HK_STR("https"))) return;
    bool full = same_origin(&from, target);
    if (!full && !navigation && !hk_psl_same_site(from.host, target->host)) return; /* cross-site subresource */
    hk_charbuf_append(out, "Referer: ", 9);
    hk_charbuf_append(out, from.scheme.p, from.scheme.n);
    hk_charbuf_append(out, "://", 3);
    hk_charbuf_append(out, from.host.p, from.host.n);
    if (from.port >= 0) {
        char port[16];
        int n = snprintf(port, sizeof port, ":%d", from.port);
        hk_charbuf_append(out, port, (size_t)n);
    }
    if (full) {
        hk_charbuf_append(out, from.path.p, from.path.n);
        if (from.has_query) {
            hk_charbuf_push(out, '?');
            hk_charbuf_append(out, from.query.p, from.query.n);
        }
    } else {
        hk_charbuf_push(out, '/');
    }
    hk_charbuf_append(out, "\r\n", 2);
}

static void run_request(hk_loader *l, request *r)
{
    if (r->url_len > 5 && hk_str_eq_ascii_icase((hk_str){r->url, 5}, HK_STR("data:"))) {
        run_data_url(l, r);
        return;
    }
    hk_arena a;
    hk_arena_init(&a, 0);
    char err[256] = "";
    hk_status st = HK_OK;
    hk_url url;
    hk_str current = {r->url, r->url_len};
    const hk_url *base = NULL;
    hk_url prev;
    bool as_post = r->is_post;

    for (int hop = 0;; hop++) {
        if (hop > MAX_REDIRECTS) {
            st = HK_ERR_LIMIT;
            snprintf(err, sizeof err, "too many redirects");
            break;
        }
        if (hk_url_parse(&a, current, base, &url) != HK_OK) {
            st = HK_ERR_INVALID;
            snprintf(err, sizeof err, "invalid URL");
            break;
        }
        if (hop == 0 && hk_str_eq(url.scheme, HK_STR("file"))) {
            run_file_url(l, r, &url);
            hk_arena_free(&a);
            return;
        }
        /* HTTPS-only mode: plain http URLs are upgraded. */
        if (hk_str_eq(url.scheme, HK_STR("http"))) {
            hk_charbuf up = {0};
            hk_charbuf_append(&up, "https", 5);
            hk_charbuf_append(&up, url.href.p + 4, url.href.n - 4);
            hk_url upgraded;
            hk_status ust = hk_url_parse(&a, (hk_str){up.data, up.len}, NULL, &upgraded);
            hk_vec_free(&up);
            if (ust == HK_OK) url = upgraded;
        }
        if (!hk_str_eq(url.scheme, HK_STR("https"))) {
            st = HK_ERR_UNSUPPORTED;
            snprintf(err, sizeof err, "unsupported URL scheme");
            break;
        }
        /* The top-level site decides cookies, the cache partition and third-party filters. */
        hk_str site_host = url.host;
        hk_url site_url = {0};
        bool have_site = r->site && hk_url_parse(&a, (hk_str){r->site, r->site_len}, NULL, &site_url) == HK_OK;
        if (have_site) site_host = site_url.host;
        if (l->blocker) {
            hk_block_request br = {url.href, url.host, have_site ? site_url.href : (hk_str){0}, site_host, r->type};
            hk_charbuf stripped = {0};
            hk_url clean;
            if (hk_blocker_strip_params(l->blocker, &br, &stripped) &&
                hk_url_parse(&a, (hk_str){stripped.data, stripped.len}, NULL, &clean) == HK_OK) {
                HK_LOGD("removeparam: %.*s", (int)clean.href.n, clean.href.p);
                url = clean;
                br.url = url.href;
            }
            hk_vec_free(&stripped);
            hk_block_result res = hk_blocker_match(l->blocker, &br);
            if (res.blocked) {
                hk_str list = hk_blocker_filter_list(l->blocker, res.filter);
                st = HK_ERR_BLOCKED;
                snprintf(err, sizeof err, "blocked by the content blocker (%.*s:%u): %.*s", (int)list.n, list.p,
                         hk_blocker_filter_line(l->blocker, res.filter), (int)url.host.n, url.host.p);
                HK_LOGI("blocked (%.*s:%u): %.*s", (int)list.n, list.p, hk_blocker_filter_line(l->blocker, res.filter),
                        (int)(url.href.n < 160 ? url.href.n : 160), url.href.p);
                break;
            }
        }
        hk_str partition = hk_psl_registrable_domain(site_host);
        if (!partition.n) partition = site_host;
        int64_t now = hk_plat_unix_time();

        fetch_ctx f = {0};
        f.loader = l;
        f.req = r;
        f.url = url.href.p;
        f.url_len = url.href.n;
        f.arena = &a;
        f.target = &url;
        f.site_host = site_host;
        hk_cache_entry entry = {0};
        bool have_entry = !as_post && hk_cache_lookup(l->cache, partition, url.href, &entry);
        if (have_entry && entry.fresh_until > now) {
            serve_cached(&f, &entry);
            hk_cache_entry_free(&entry);
            st = HK_OK;
            break;
        }
        hk_charbuf extra = {0}, cookie = {0};
        hk_cookie_header(&l->jar, &url, site_host, r->site == NULL, now, &cookie);
        if (cookie.len) {
            hk_charbuf_append(&extra, "Cookie: ", 8);
            hk_charbuf_append(&extra, cookie.data, cookie.len);
            hk_charbuf_append(&extra, "\r\n", 2);
        }
        if (have_entry && entry.etag[0]) {
            hk_charbuf_append(&extra, "If-None-Match: ", 15);
            hk_charbuf_append(&extra, entry.etag, strlen(entry.etag));
            hk_charbuf_append(&extra, "\r\n", 2);
        }
        if (have_entry && entry.last_modified[0]) {
            hk_charbuf_append(&extra, "If-Modified-Since: ", 19);
            hk_charbuf_append(&extra, entry.last_modified, strlen(entry.last_modified));
            hk_charbuf_append(&extra, "\r\n", 2);
        }
        if (have_entry) f.cached = &entry;
        if (as_post) hk_charbuf_append(&extra, r->post_headers.data, r->post_headers.len);
        referer_header(&a, r, &url, &extra);
        hk_vec_free(&cookie);
        hk_http_callbacks cb = {&f, on_headers, on_body, cancelled};
        hk_http_response resp;
        hk_str body = as_post ? (hk_str){r->post.data ? r->post.data : "", r->post.len} : (hk_str){0};
        st = hk_http_request(l->pool, &a, &url, (hk_str){extra.data, extra.len}, body, &resp, &cb, err, sizeof err);
        hk_vec_free(&extra);
        if (st == HK_OK && f.not_modified) {
            serve_cached(&f, &entry);
            hk_cache_refresh(l->cache, partition, url.href, &resp, &entry, now);
        } else if (st == HK_OK && resp.status == 200 && !f.redirect && !as_post) {
            if (r->image)
                hk_cache_store(l->cache, partition, url.href, &resp, (const uint8_t *)r->body.data, r->body.len, now);
            else if (f.capture_ok)
                hk_cache_store(l->cache, partition, url.href, &resp, (const uint8_t *)f.capture.data, f.capture.len, now);
        }
        if (have_entry) hk_cache_entry_free(&entry);
        hk_vec_free(&f.capture);
        hk_vec_free(&resp.headers);
        if (st != HK_OK || !f.redirect) break;
        if (f.redirect_status != 307 && f.redirect_status != 308) as_post = false;
        prev = url;
        base = &prev;
        current = f.location;
    }
    finish_request(l, r, st, err);
    hk_arena_free(&a);
}

static void cookies_path(const hk_loader *l, char *out, size_t n)
{
    snprintf(out, n, "%s\\cookies.tsv", l->data_dir);
}

static void save_cookies(hk_loader *l)
{
    if (!l->data_dir[0] || !l->jar.dirty) return;
    hk_charbuf out = {0};
    hk_cookie_jar_save(&l->jar, &out, hk_plat_unix_time());
    char path[600];
    cookies_path(l, path, sizeof path);
    if (hk_plat_write_file_atomic(path, out.data ? out.data : "", out.len) == HK_OK) l->jar.dirty = false;
    hk_vec_free(&out);
}

static void io_thread(void *arg)
{
    hk_loader *l = arg;
    l->pool = hk_http_pool_create(hk_plat_now_ns);
    hk_cookie_jar_init(&l->jar);
    if (l->data_dir[0]) {
        char path[600];
        cookies_path(l, path, sizeof path);
        hk_charbuf text = {0};
        if (hk_plat_read_file(path, &text) == HK_OK)
            hk_cookie_jar_load(&l->jar, (hk_str){text.data, text.len}, hk_plat_unix_time());
        hk_vec_free(&text);
        snprintf(path, sizeof path, "%s\\cache", l->data_dir);
        l->cache = hk_cache_open(path, 256ull * 1024 * 1024);
    }
    for (;;) {
        hk_mutex_lock(&l->lock);
        while (!l->queue_head && !l->stopping && !l->swap_pending) hk_cond_wait(&l->wake, &l->lock);
        if (l->swap_pending) {
            /* A new filter image: requests already running finished with the old one. */
            void (*release)(void *) = l->blocker_release;
            void *ctx = l->blocker_ctx;
            l->blocker = l->next_blocker;
            l->blocker_release = l->next_release;
            l->blocker_ctx = l->next_ctx;
            l->swap_pending = false;
            hk_mutex_unlock(&l->lock);
            if (release) release(ctx);
            continue;
        }
        if (l->stopping) {
            hk_mutex_unlock(&l->lock);
            if (l->blocker_release) l->blocker_release(l->blocker_ctx);
            save_cookies(l);
            hk_cookie_jar_free(&l->jar);
            hk_http_pool_destroy(l->pool);
            hk_cache_close(l->cache);
            return;
        }
        request *r = l->queue_head;
        l->queue_head = r->next;
        if (!l->queue_head) l->queue_tail = NULL;
        hk_mutex_unlock(&l->lock);
        run_request(l, r);
        save_cookies(l);
    }
}

/* --- public API ----------------------------------------------------------------------------- */

hk_loader *hk_loader_create(const hk_blocker *blocker, const char *data_dir)
{
    hk_loader *l = hk_alloc_zero(sizeof *l);
    l->blocker = blocker;
    if (data_dir) snprintf(l->data_dir, sizeof l->data_dir, "%s", data_dir);
    l->next_id = 1;
    l->thread = hk_thread_start("hawk-io", io_thread, l);
    return l;
}

void hk_loader_set_blocker(hk_loader *l, const hk_blocker *b, void (*release)(void *ctx), void *ctx)
{
    hk_mutex_lock(&l->lock);
    if (l->swap_pending && l->next_release) l->next_release(l->next_ctx); /* superseded before use */
    l->next_blocker = b;
    l->next_release = release;
    l->next_ctx = ctx;
    l->swap_pending = true;
    hk_cond_signal(&l->wake);
    hk_mutex_unlock(&l->lock);
}

void hk_loader_destroy(hk_loader *l)
{
    if (!l) return;
    hk_mutex_lock(&l->lock);
    l->stopping = true;
    for (uint32_t i = 0; i < l->live.len; i++) hk_atomic_add64(&l->live.data[i]->cancelled, 1);
    /* Queued requests never ran: no messages refer to them. */
    while (l->queue_head) {
        request *r = l->queue_head;
        l->queue_head = r->next;
        remove_live(l, r);
        free_request(r);
    }
    hk_cond_signal(&l->wake);
    hk_mutex_unlock(&l->lock);
    hk_thread_join(l->thread);
    /* Requests that ran but whose messages were never delivered are freed with the process. */
    hk_vec_free(&l->live);
    hk_free(l, sizeof *l);
}

static request *new_request(hk_str url, hk_str site, uint32_t type, const hk_load_callbacks *cb)
{
    request *r = hk_alloc_zero(sizeof *r);
    r->type = type;
    if (site.n) {
        r->site = hk_alloc(site.n + 1);
        memcpy(r->site, site.p, site.n);
        r->site[site.n] = 0;
        r->site_len = site.n;
    }
    r->cb = *cb;
    r->url = hk_alloc(url.n + 1);
    memcpy(r->url, url.p, url.n);
    r->url[url.n] = 0;
    r->url_len = url.n;
    return r;
}

static uint32_t enqueue(hk_loader *l, request *r);

uint32_t hk_loader_fetch(hk_loader *l, hk_str url, hk_str site, uint32_t type, const hk_load_callbacks *cb)
{
    return enqueue(l, new_request(url, site, site.n ? type : HK_REQ_DOCUMENT, cb));
}

static void set_referrer(request *r, hk_str referrer)
{
    if (!referrer.n) return;
    r->referrer = hk_alloc(referrer.n + 1);
    memcpy(r->referrer, referrer.p, referrer.n);
    r->referrer[referrer.n] = 0;
    r->referrer_len = referrer.n;
}

uint32_t hk_loader_navigate(hk_loader *l, hk_str url, hk_str referrer, const hk_load_callbacks *cb)
{
    request *r = new_request(url, (hk_str){0}, HK_REQ_DOCUMENT, cb);
    set_referrer(r, referrer);
    return enqueue(l, r);
}

uint32_t hk_loader_post(hk_loader *l, hk_str url, hk_str content_type, hk_str body, hk_str origin,
                        hk_str referrer, const hk_load_callbacks *cb)
{
    request *r = new_request(url, (hk_str){0}, HK_REQ_DOCUMENT, cb);
    set_referrer(r, referrer);
    r->is_post = true;
    hk_charbuf_append(&r->post, body.p, body.n);
    hk_charbuf_append(&r->post_headers, "Content-Type: ", 14);
    hk_charbuf_append(&r->post_headers, content_type.p, content_type.n);
    hk_charbuf_append(&r->post_headers, "\r\n", 2);
    if (origin.n) {
        hk_charbuf_append(&r->post_headers, "Origin: ", 8);
        hk_charbuf_append(&r->post_headers, origin.p, origin.n);
        hk_charbuf_append(&r->post_headers, "\r\n", 2);
    }
    return enqueue(l, r);
}

uint32_t hk_loader_fetch_image(hk_loader *l, hk_str url, hk_str site, int max_w, int max_h,
                               const hk_load_callbacks *cb)
{
    request *r = new_request(url, site, HK_REQ_IMAGE, cb);
    r->image = true;
    r->image_w = max_w;
    r->image_h = max_h;
    return enqueue(l, r);
}

static uint32_t enqueue(hk_loader *l, request *r)
{
    r->id = l->next_id++;
    hk_vec_push(&l->live, r);
    hk_mutex_lock(&l->lock);
    if (l->queue_tail) l->queue_tail->next = r;
    else l->queue_head = r;
    l->queue_tail = r;
    hk_cond_signal(&l->wake);
    hk_mutex_unlock(&l->lock);
    return r->id;
}

void hk_loader_cancel(hk_loader *l, uint32_t id)
{
    for (uint32_t i = 0; i < l->live.len; i++) {
        if (l->live.data[i]->id == id) {
            hk_atomic_add64(&l->live.data[i]->cancelled, 1);
            return;
        }
    }
}
