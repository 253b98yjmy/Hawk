/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HTTP/1.1 client for one request on one connection (docs/design/02-network.md, 5).
 * Redirects, the blocker and HTTPS upgrades are the loader's job.
 */
#ifndef HK_HTTP_H
#define HK_HTTP_H

#include "base/hk_arena.h"
#include "base/hk_vec.h"
#include "net/hk_url.h"

typedef struct hk_http_header {
    hk_str name;
    hk_str value;
} hk_http_header;

typedef struct hk_http_response {
    int status;
    hk_str reason;
    HK_VEC(hk_http_header) headers;
} hk_http_response;

/* Case-insensitive lookup of the first header with this name. */
hk_str hk_http_header_get(const hk_http_response *r, const char *name, bool *found);

typedef struct hk_http_callbacks {
    void *user;
    /* Called once the headers are complete. Return false to stop (the body is not read). */
    bool (*on_headers)(void *user, const hk_http_response *r);
    /* Decoded body bytes (after Transfer-Encoding and Content-Encoding). Return false to stop. */
    bool (*on_body)(void *user, const uint8_t *p, size_t n);
    /* Polled between reads; return true to abort the transfer. */
    bool (*cancelled)(void *user);
} hk_http_callbacks;

/* The User-Agent Hawk sends (Firefox ESR on Windows; README decision D2). */
extern const char hk_http_user_agent[];

/* Keep-alive connections, reused across requests (I/O thread only). */
typedef struct hk_http_pool hk_http_pool;
hk_http_pool *hk_http_pool_create(uint64_t (*now_ns)(void));
void hk_http_pool_destroy(hk_http_pool *p);

/*
 * Performs a request: GET, or POST when body.p is not NULL (extra_headers then carries its
 * Content-Type; Content-Length is added). pool may be NULL (one connection per request). A POST
 * always uses a new connection, so that it is never retried after a stale keep-alive failure.
 * extra_headers are complete header lines, each ending in CRLF ("Cookie: a=b"). Response strings
 * are allocated in the arena. On failure err receives a human-readable message.
 */
hk_status hk_http_request(hk_http_pool *pool, hk_arena *a, const hk_url *url, hk_str extra_headers, hk_str body,
                          hk_http_response *resp, const hk_http_callbacks *cb, char *err, size_t errlen);

/* Response body decoding, exposed for tests and fuzzing. */
typedef struct hk_http_body_decoder hk_http_body_decoder;

typedef enum hk_http_framing {
    HK_HTTP_FRAMING_NONE,
    HK_HTTP_FRAMING_LENGTH,
    HK_HTTP_FRAMING_CHUNKED,
    HK_HTTP_FRAMING_CLOSE,
} hk_http_framing;

hk_http_body_decoder *hk_http_body_decoder_create(hk_http_framing framing, uint64_t length, hk_str content_encoding,
                                                  bool (*sink)(void *user, const uint8_t *p, size_t n), void *user);
/* Feeds raw bytes from the connection. Returns HK_ERR_INVALID on malformed framing or compression,
 * HK_ERR_LIMIT when the decompressed size exceeds the limit. *done becomes true at the end of the body. */
hk_status hk_http_body_decoder_feed(hk_http_body_decoder *d, const uint8_t *p, size_t n, bool *done);
/* Called when the connection closed; returns an error if the body was incomplete. */
hk_status hk_http_body_decoder_finish(hk_http_body_decoder *d);
void hk_http_body_decoder_destroy(hk_http_body_decoder *d);

#endif
