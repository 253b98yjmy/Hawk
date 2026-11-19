/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net/hk_http.h"
#include "base/hk_mem.h"
#include "plat/hk_conn.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

/* Keep in step with the current Firefox ESR (docs/design/README.md, D2). */
const char hk_http_user_agent[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0";

#define MAX_HEADER_BYTES (64 * 1024)
#define MAX_DECODED_BYTES ((uint64_t)64 * 1024 * 1024)

/* --- body decoding ------------------------------------------------------------------------ */

typedef enum {
    CH_SIZE,
    CH_EXT,
    CH_DATA,
    CH_DATA_CR,
    CH_DATA_LF,
    CH_TRAILER,
    CH_DONE,
} chunk_state;

struct hk_http_body_decoder {
    hk_http_framing framing;
    uint64_t remaining;
    chunk_state cs;
    uint64_t chunk_left;
    bool size_digits;
    uint32_t trailer_line;
    bool inflate;
    bool z_init, z_raw, z_end;
    z_stream z;
    uint64_t in_total, out_total;
    bool (*sink)(void *user, const uint8_t *p, size_t n);
    void *user;
    bool stopped;
    bool done;
};

hk_http_body_decoder *hk_http_body_decoder_create(hk_http_framing framing, uint64_t length, hk_str coding,
                                                  bool (*sink)(void *user, const uint8_t *p, size_t n), void *user)
{
    hk_http_body_decoder *d = hk_alloc_zero(sizeof *d);
    d->framing = framing;
    d->remaining = length;
    d->sink = sink;
    d->user = user;
    coding = hk_str_trim_ascii_ws(coding);
    d->inflate = hk_str_eq_ascii_icase(coding, HK_STR("gzip")) || hk_str_eq_ascii_icase(coding, HK_STR("x-gzip")) ||
                 hk_str_eq_ascii_icase(coding, HK_STR("deflate"));
    if (framing == HK_HTTP_FRAMING_NONE || (framing == HK_HTTP_FRAMING_LENGTH && length == 0)) d->done = true;
    return d;
}

void hk_http_body_decoder_destroy(hk_http_body_decoder *d)
{
    if (!d) return;
    if (d->z_init) inflateEnd(&d->z);
    hk_free(d, sizeof *d);
}

static hk_status deliver(hk_http_body_decoder *d, const uint8_t *p, size_t n)
{
    if (d->stopped || n == 0) return HK_OK;
    if (!d->inflate) {
        d->out_total += n;
        if (d->out_total > MAX_DECODED_BYTES) return HK_ERR_LIMIT;
        if (!d->sink(d->user, p, n)) d->stopped = true;
        return HK_OK;
    }
    if (d->z_end) return HK_OK; /* trailing garbage after the compressed stream is ignored */
    if (!d->z_init) {
        memset(&d->z, 0, sizeof d->z);
        if (inflateInit2(&d->z, d->z_raw ? -15 : 15 + 32) != Z_OK) return HK_ERR_INVALID;
        d->z_init = true;
    }
    uint8_t out[16384];
    d->z.next_in = (Bytef *)p;
    d->z.avail_in = (uInt)n;
    while (d->z.avail_in > 0 && !d->stopped) {
        d->z.next_out = out;
        d->z.avail_out = sizeof out;
        int r = inflate(&d->z, Z_NO_FLUSH);
        size_t produced = sizeof out - d->z.avail_out;
        if (r == Z_DATA_ERROR && !d->z_raw && d->out_total == 0 && produced == 0 && d->in_total == 0) {
            /* "deflate" is often sent as raw deflate instead of zlib-wrapped data. */
            inflateEnd(&d->z);
            d->z_init = false;
            d->z_raw = true;
            return deliver(d, p, n);
        }
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) return HK_ERR_INVALID;
        d->out_total += produced;
        if (d->out_total > MAX_DECODED_BYTES) return HK_ERR_LIMIT;
        if (produced && !d->sink(d->user, out, produced)) d->stopped = true;
        if (r == Z_STREAM_END) {
            d->z_end = true;
            break;
        }
        if (r == Z_BUF_ERROR && produced == 0) break;
    }
    d->in_total += n;
    return HK_OK;
}

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static hk_status feed_chunked(hk_http_body_decoder *d, const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n && d->cs != CH_DONE) {
        uint8_t c = p[i];
        switch (d->cs) {
        case CH_SIZE: {
            int v = hex_digit(c);
            if (v >= 0) {
                if (d->chunk_left >> 56) return HK_ERR_INVALID;
                d->chunk_left = d->chunk_left * 16 + (uint64_t)v;
                d->size_digits = true;
            } else if (c == ';' || c == ' ' || c == '\t' || c == '\r') {
                d->cs = CH_EXT;
            } else if (c == '\n') {
                goto size_line_done;
            } else {
                return HK_ERR_INVALID;
            }
            i++;
            break;
        }
        case CH_EXT:
            i++;
            if (c != '\n') break;
        size_line_done:
            if (d->cs == CH_SIZE) i++;
            if (!d->size_digits) return HK_ERR_INVALID;
            d->cs = d->chunk_left ? CH_DATA : CH_TRAILER;
            d->trailer_line = 0;
            break;
        case CH_DATA: {
            size_t k = (size_t)HK_MIN((uint64_t)(n - i), d->chunk_left);
            hk_status st = deliver(d, p + i, k);
            if (st != HK_OK) return st;
            i += k;
            d->chunk_left -= k;
            if (d->chunk_left == 0) d->cs = CH_DATA_CR;
            break;
        }
        case CH_DATA_CR:
            i++;
            if (c == '\r') d->cs = CH_DATA_LF;
            else if (c == '\n') d->cs = CH_SIZE, d->size_digits = false;
            else return HK_ERR_INVALID;
            break;
        case CH_DATA_LF:
            i++;
            if (c != '\n') return HK_ERR_INVALID;
            d->cs = CH_SIZE;
            d->size_digits = false;
            break;
        case CH_TRAILER:
            i++;
            if (c == '\n') {
                if (d->trailer_line == 0) d->cs = CH_DONE;
                d->trailer_line = 0;
            } else if (c != '\r') {
                if (++d->trailer_line > MAX_HEADER_BYTES) return HK_ERR_INVALID;
            }
            break;
        case CH_DONE: break;
        }
    }
    if (d->cs == CH_DONE) d->done = true;
    return HK_OK;
}

hk_status hk_http_body_decoder_feed(hk_http_body_decoder *d, const uint8_t *p, size_t n, bool *done)
{
    hk_status st = HK_OK;
    if (!d->done) {
        switch (d->framing) {
        case HK_HTTP_FRAMING_NONE: d->done = true; break;
        case HK_HTTP_FRAMING_LENGTH: {
            size_t k = (size_t)HK_MIN((uint64_t)n, d->remaining);
            st = deliver(d, p, k);
            d->remaining -= k;
            if (d->remaining == 0) d->done = true;
            break;
        }
        case HK_HTTP_FRAMING_CHUNKED: st = feed_chunked(d, p, n); break;
        case HK_HTTP_FRAMING_CLOSE: st = deliver(d, p, n); break;
        }
    }
    *done = d->done || d->stopped;
    return st;
}

hk_status hk_http_body_decoder_finish(hk_http_body_decoder *d)
{
    if (d->framing == HK_HTTP_FRAMING_CLOSE) d->done = true;
    return d->done || d->stopped ? HK_OK : HK_ERR_IO;
}

/* --- headers -------------------------------------------------------------------------------- */

hk_str hk_http_header_get(const hk_http_response *r, const char *name, bool *found)
{
    hk_str n = hk_str_from_cstr(name);
    for (uint32_t i = 0; i < r->headers.len; i++) {
        if (hk_str_eq_ascii_icase(r->headers.data[i].name, n)) {
            if (found) *found = true;
            return r->headers.data[i].value;
        }
    }
    if (found) *found = false;
    return HK_STR("");
}

/* Parses one header block (status line + fields). Returns false if malformed. */
static bool parse_head(hk_arena *a, hk_str head, hk_http_response *r)
{
    r->headers.len = 0;
    size_t pos = 0;
    bool first = true;
    while (pos < head.n) {
        size_t eol = pos;
        while (eol < head.n && head.p[eol] != '\n') eol++;
        hk_str line = hk_str_slice(head, pos, eol);
        if (line.n && line.p[line.n - 1] == '\r') line.n--;
        pos = eol + 1;
        if (first) {
            first = false;
            if (!hk_str_starts_with(line, HK_STR("HTTP/")) || line.n < 12) return false;
            size_t sp = hk_str_find_char(line, ' ');
            if (sp == HK_STR_NPOS || sp + 4 > line.n) return false;
            int status = 0;
            for (size_t k = sp + 1; k < sp + 4; k++) {
                if (line.p[k] < '0' || line.p[k] > '9') return false;
                status = status * 10 + (line.p[k] - '0');
            }
            r->status = status;
            r->reason = hk_arena_str_dup(a, hk_str_trim_ascii_ws(hk_str_slice(line, sp + 4, line.n)));
            continue;
        }
        if (line.n == 0) break;
        if (line.p[0] == ' ' || line.p[0] == '\t') continue; /* obsolete line folding: ignored */
        size_t colon = hk_str_find_char(line, ':');
        if (colon == HK_STR_NPOS || colon == 0) continue;
        hk_http_header h;
        h.name = hk_arena_str_dup(a, hk_str_trim_ascii_ws(hk_str_slice(line, 0, colon)));
        h.value = hk_arena_str_dup(a, hk_str_trim_ascii_ws(hk_str_slice(line, colon + 1, line.n)));
        hk_vec_push(&r->headers, h);
    }
    return !first;
}

/* Position just after the blank line ending the header block, or 0 if not complete yet. */
static size_t find_head_end(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '\n') continue;
        if (i + 1 < n && p[i + 1] == '\n') return i + 2;
        if (i + 2 < n && p[i + 1] == '\r' && p[i + 2] == '\n') return i + 3;
    }
    return 0;
}

static bool parse_content_length(hk_str v, uint64_t *out)
{
    v = hk_str_trim_ascii_ws(v);
    /* Repeated identical values ("10, 10") are allowed. */
    size_t comma = hk_str_find_char(v, ',');
    if (comma != HK_STR_NPOS) v = hk_str_trim_ascii_ws(hk_str_slice(v, 0, comma));
    if (v.n == 0 || v.n > 18) return false;
    uint64_t x = 0;
    for (size_t i = 0; i < v.n; i++) {
        if (v.p[i] < '0' || v.p[i] > '9') return false;
        x = x * 10 + (uint64_t)(v.p[i] - '0');
    }
    *out = x;
    return true;
}

/* --- the request ----------------------------------------------------------------------------- */

typedef struct body_ctx {
    const hk_http_callbacks *cb;
} body_ctx;

static bool body_sink(void *user, const uint8_t *p, size_t n)
{
    body_ctx *b = user;
    return b->cb->on_body ? b->cb->on_body(b->cb->user, p, n) : true;
}

/* --- connection pool -------------------------------------------------------------------------- */

#define POOL_MAX 12
#define POOL_IDLE_NS (30ull * 1000000000ull)

typedef struct pooled {
    char key[300];
    hk_conn *conn;
    uint64_t idle_since;
} pooled;

struct hk_http_pool {
    pooled conns[POOL_MAX];
    uint32_t count;
    uint64_t (*now)(void);
};

hk_http_pool *hk_http_pool_create(uint64_t (*now_ns)(void))
{
    hk_http_pool *p = hk_alloc_zero(sizeof *p);
    p->now = now_ns;
    return p;
}

void hk_http_pool_destroy(hk_http_pool *p)
{
    if (!p) return;
    for (uint32_t i = 0; i < p->count; i++) hk_conn_close(p->conns[i].conn);
    hk_free(p, sizeof *p);
}

static void pool_key(const hk_url *url, char *key, size_t n)
{
    snprintf(key, n, "%.*s://%.*s:%d", (int)url->scheme.n, url->scheme.p, (int)url->host.n, url->host.p,
             hk_url_effective_port(url));
}

static hk_conn *pool_take(hk_http_pool *p, const char *key)
{
    if (!p) return NULL;
    uint64_t now = p->now();
    for (uint32_t i = 0; i < p->count;) {
        pooled *e = &p->conns[i];
        if (now - e->idle_since > POOL_IDLE_NS) {
            hk_conn_close(e->conn);
            *e = p->conns[--p->count];
            continue;
        }
        if (strcmp(e->key, key) == 0) {
            hk_conn *c = e->conn;
            *e = p->conns[--p->count];
            return c;
        }
        i++;
    }
    return NULL;
}

static void pool_put(hk_http_pool *p, const char *key, hk_conn *c)
{
    if (!p) {
        hk_conn_close(c);
        return;
    }
    if (p->count == POOL_MAX) {
        /* Drop the connection idle the longest. */
        uint32_t oldest = 0;
        for (uint32_t i = 1; i < p->count; i++) {
            if (p->conns[i].idle_since < p->conns[oldest].idle_since) oldest = i;
        }
        hk_conn_close(p->conns[oldest].conn);
        p->conns[oldest] = p->conns[--p->count];
    }
    pooled *e = &p->conns[p->count++];
    snprintf(e->key, sizeof e->key, "%s", key);
    e->conn = c;
    e->idle_since = p->now();
}

/* --- one exchange ------------------------------------------------------------------------------- */

typedef enum { EX_OK, EX_RETRY, EX_FAIL } exchange_result;

static exchange_result exchange(hk_arena *a, hk_conn *c, bool reused, const hk_charbuf *req, hk_http_response *resp,
                                const hk_http_callbacks *cb, bool *reusable, hk_status *st_out, char *err, size_t errlen)
{
    *reusable = false;
    hk_status st = hk_conn_write(c, req->data, req->len);
    if (st != HK_OK) {
        if (reused) return EX_RETRY;
        snprintf(err, errlen, "could not send the request");
        *st_out = st;
        return EX_FAIL;
    }
    hk_charbuf in = {0};
    size_t head_end = 0;
    for (;;) {
        head_end = find_head_end(in.data ? in.data : "", in.len);
        if (head_end) {
            if (!parse_head(a, (hk_str){in.data, head_end}, resp)) {
                st = HK_ERR_INVALID;
                snprintf(err, errlen, "malformed HTTP response");
                break;
            }
            if (resp->status >= 100 && resp->status < 200) {
                memmove(in.data, in.data + head_end, in.len - head_end);
                in.len -= (uint32_t)head_end;
                head_end = 0;
                continue;
            }
            break;
        }
        if (in.len > MAX_HEADER_BYTES) {
            st = HK_ERR_LIMIT;
            snprintf(err, errlen, "HTTP response headers too large");
            break;
        }
        if (cb->cancelled && cb->cancelled(cb->user)) {
            st = HK_ERR_IO;
            snprintf(err, errlen, "cancelled");
            break;
        }
        hk_vec_reserve(&in, (uint64_t)in.len + 16384);
        size_t got = 0;
        st = hk_conn_read(c, in.data + in.len, in.cap - in.len, &got);
        if (st != HK_OK || got == 0) {
            if (reused && in.len == 0) {
                /* The server closed an idle keep-alive connection: try again on a new one. */
                hk_vec_free(&in);
                return EX_RETRY;
            }
            st = HK_ERR_IO;
            snprintf(err, errlen, "connection closed before the response was complete");
            break;
        }
        in.len += (uint32_t)got;
    }
    if (st != HK_OK) {
        hk_vec_free(&in);
        *st_out = st;
        return EX_FAIL;
    }

    if (cb->on_headers && !cb->on_headers(cb->user, resp)) {
        hk_vec_free(&in);
        *st_out = HK_OK;
        return EX_OK;
    }

    /* Body framing (RFC 9112, 6.3). */
    hk_http_framing framing = HK_HTTP_FRAMING_CLOSE;
    uint64_t length = 0;
    bool has_te = false, has_cl = false;
    hk_str te = hk_http_header_get(resp, "Transfer-Encoding", &has_te);
    hk_str cl = hk_http_header_get(resp, "Content-Length", &has_cl);
    if (resp->status == 204 || resp->status == 304) framing = HK_HTTP_FRAMING_NONE;
    else if (has_te && hk_str_ends_with(hk_str_trim_ascii_ws(te), HK_STR("chunked"))) framing = HK_HTTP_FRAMING_CHUNKED;
    else if (has_cl && parse_content_length(cl, &length)) framing = HK_HTTP_FRAMING_LENGTH;

    hk_str coding = hk_str_trim_ascii_ws(hk_http_header_get(resp, "Content-Encoding", NULL));
    if (coding.n && !hk_str_eq_ascii_icase(coding, HK_STR("identity")) && !hk_str_eq_ascii_icase(coding, HK_STR("gzip")) &&
        !hk_str_eq_ascii_icase(coding, HK_STR("x-gzip")) && !hk_str_eq_ascii_icase(coding, HK_STR("deflate"))) {
        snprintf(err, errlen, "unsupported content encoding");
        hk_vec_free(&in);
        *st_out = HK_ERR_UNSUPPORTED;
        return EX_FAIL;
    }

    body_ctx bc = {cb};
    hk_http_body_decoder *d = hk_http_body_decoder_create(framing, length, coding, body_sink, &bc);
    bool done = false;
    st = hk_http_body_decoder_feed(d, (const uint8_t *)in.data + head_end, in.len - head_end, &done);
    hk_vec_free(&in);
    uint8_t buf[16384];
    bool closed = false;
    while (st == HK_OK && !done) {
        if (cb->cancelled && cb->cancelled(cb->user)) {
            st = HK_ERR_IO;
            snprintf(err, errlen, "cancelled");
            break;
        }
        size_t got = 0;
        st = hk_conn_read(c, buf, sizeof buf, &got);
        if (st != HK_OK) {
            snprintf(err, errlen, "network error while reading the response");
            break;
        }
        if (got == 0) {
            closed = true;
            st = hk_http_body_decoder_finish(d);
            if (st != HK_OK) snprintf(err, errlen, "connection closed before the body was complete");
            break;
        }
        st = hk_http_body_decoder_feed(d, buf, got, &done);
    }
    if (st == HK_ERR_INVALID) snprintf(err, errlen, "malformed response body");
    if (st == HK_ERR_LIMIT) snprintf(err, errlen, "response body too large");
    hk_http_body_decoder_destroy(d);
    hk_str conn_hdr = hk_http_header_get(resp, "Connection", NULL);
    *reusable = st == HK_OK && done && !closed && framing != HK_HTTP_FRAMING_CLOSE &&
                hk_str_find(conn_hdr, HK_STR("close")) == HK_STR_NPOS;
    *st_out = st;
    return st == HK_OK ? EX_OK : EX_FAIL;
}

hk_status hk_http_request(hk_http_pool *pool, hk_arena *a, const hk_url *url, hk_str extra_headers, hk_str body,
                          hk_http_response *resp, const hk_http_callbacks *cb, char *err, size_t errlen)
{
    memset(resp, 0, sizeof *resp);
    bool tls = hk_str_eq(url->scheme, HK_STR("https"));
    if (!tls && !hk_str_eq(url->scheme, HK_STR("http"))) {
        snprintf(err, errlen, "unsupported scheme");
        return HK_ERR_UNSUPPORTED;
    }
    hk_charbuf req = {0};
    hk_str target = hk_url_request_target(a, url);
    char line[512];
    bool post = body.p != NULL;
    if (post) hk_charbuf_append(&req, "POST ", 5);
    else hk_charbuf_append(&req, "GET ", 4);
    hk_charbuf_append(&req, target.p, target.n);
    hk_charbuf_append(&req, " HTTP/1.1\r\nHost: ", 17);
    hk_charbuf_append(&req, url->host.p, url->host.n);
    if (url->port >= 0) {
        int n = snprintf(line, sizeof line, ":%d", url->port);
        hk_charbuf_append(&req, line, (size_t)n);
    }
    int n = snprintf(line, sizeof line,
                     "\r\nUser-Agent: %s\r\n"
                     "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
                     "Accept-Language: ja,en-US;q=0.7,en;q=0.3\r\n"
                     "Accept-Encoding: gzip, deflate\r\n"
                     "Sec-GPC: 1\r\n"
                     "Connection: %s\r\n",
                     hk_http_user_agent, pool ? "keep-alive" : "close");
    hk_charbuf_append(&req, line, (size_t)n);
    hk_charbuf_append(&req, extra_headers.p, extra_headers.n);
    if (post) {
        n = snprintf(line, sizeof line, "Content-Length: %zu\r\n", body.n);
        hk_charbuf_append(&req, line, (size_t)n);
    }
    hk_charbuf_append(&req, "\r\n", 2);
    if (post) hk_charbuf_append(&req, body.p, body.n);

    char key[300];
    pool_key(url, key, sizeof key);
    hk_status st = HK_OK;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (post && attempt > 0) break;
        hk_conn *c = attempt == 0 && !post ? pool_take(pool, key) : NULL;
        bool reused = c != NULL;
        if (!c) {
            st = hk_conn_open(url->host, (uint16_t)hk_url_effective_port(url), tls, &c, err, errlen);
            if (st != HK_OK) break;
        }
        bool reusable;
        hk_vec_free(&resp->headers);
        memset(resp, 0, sizeof *resp);
        exchange_result r = exchange(a, c, reused, &req, resp, cb, &reusable, &st, err, errlen);
        if (r == EX_RETRY) {
            hk_conn_close(c);
            continue;
        }
        if (reusable) pool_put(pool, key, c);
        else hk_conn_close(c);
        break;
    }
    hk_vec_free(&req);
    return st;
}
