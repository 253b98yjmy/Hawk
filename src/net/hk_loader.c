/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net/hk_loader.h"
#include "base/hk_atomic.h"
#include "base/hk_log.h"
#include "base/hk_mem.h"
#include "base/hk_vec.h"
#include "net/hk_http.h"
#include "net/hk_url.h"
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
    int image_w, image_h;
    hk_charbuf body; /* image requests: the whole body (I/O thread) */
} request;

struct hk_loader {
    const hk_blocker *blocker;
    hk_thread *thread;
    hk_mutex lock;
    hk_cond wake;
    request *queue_head, *queue_tail;
    bool stopping;
    uint32_t next_id;
    HK_VEC(request *) live; /* main thread: requests that have not delivered on_done */
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
    hk_vec_free(&r->body);
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
    hk_str location;
    hk_arena *arena;
} fetch_ctx;

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool on_headers(void *user, const hk_http_response *r)
{
    fetch_ctx *f = user;
    bool has_location = false;
    hk_str location = hk_http_header_get(r, "Location", &has_location);
    if (is_redirect(r->status) && has_location) {
        f->redirect = true;
        f->location = hk_arena_str_dup(f->arena, location);
        return false;
    }
    hk_str ct = hk_http_header_get(r, "Content-Type", NULL);
    /* "url\0content-type" in one buffer. */
    hk_charbuf b = {0};
    hk_charbuf_append(&b, f->url, f->url_len);
    hk_charbuf_push(&b, '\0');
    hk_charbuf_append(&b, ct.p, ct.n);
    post(f->loader, f->req, MSG_RESPONSE, HK_OK, r->status, b.data, b.len, f->url_len);
    hk_vec_free(&b);
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
    post(f->loader, f->req, MSG_DATA, HK_OK, 0, (const char *)p, n, 0);
    return hk_atomic_load64(&f->req->cancelled) == 0;
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
        if (l->blocker && hk_blocker_blocks_host(l->blocker, url.host)) {
            st = HK_ERR_BLOCKED;
            snprintf(err, sizeof err, "blocked by the content blocker: %.*s", (int)url.host.n, url.host.p);
            break;
        }
        fetch_ctx f = {l, r, url.href.p, url.href.n, false, {0}, &a};
        hk_http_callbacks cb = {&f, on_headers, on_body, cancelled};
        hk_http_response resp;
        st = hk_http_get(&a, &url, &resp, &cb, err, sizeof err);
        hk_vec_free(&resp.headers);
        if (st != HK_OK || !f.redirect) break;
        prev = url;
        base = &prev;
        current = f.location;
    }
    finish_request(l, r, st, err);
    hk_arena_free(&a);
}

static void io_thread(void *arg)
{
    hk_loader *l = arg;
    for (;;) {
        hk_mutex_lock(&l->lock);
        while (!l->queue_head && !l->stopping) hk_cond_wait(&l->wake, &l->lock);
        if (l->stopping) {
            hk_mutex_unlock(&l->lock);
            return;
        }
        request *r = l->queue_head;
        l->queue_head = r->next;
        if (!l->queue_head) l->queue_tail = NULL;
        hk_mutex_unlock(&l->lock);
        run_request(l, r);
    }
}

/* --- public API ----------------------------------------------------------------------------- */

hk_loader *hk_loader_create(const hk_blocker *blocker)
{
    hk_loader *l = hk_alloc_zero(sizeof *l);
    l->blocker = blocker;
    l->next_id = 1;
    l->thread = hk_thread_start("hawk-io", io_thread, l);
    return l;
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

static request *new_request(hk_str url, const hk_load_callbacks *cb)
{
    request *r = hk_alloc_zero(sizeof *r);
    r->cb = *cb;
    r->url = hk_alloc(url.n + 1);
    memcpy(r->url, url.p, url.n);
    r->url[url.n] = 0;
    r->url_len = url.n;
    return r;
}

static uint32_t enqueue(hk_loader *l, request *r);

uint32_t hk_loader_fetch(hk_loader *l, hk_str url, const hk_load_callbacks *cb)
{
    return enqueue(l, new_request(url, cb));
}

uint32_t hk_loader_fetch_image(hk_loader *l, hk_str url, int max_w, int max_h, const hk_load_callbacks *cb)
{
    request *r = new_request(url, cb);
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
