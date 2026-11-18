/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Cookie jar (RFC 6265bis) with Hawk's policy (docs/design/02-network.md, 7):
 * cookies are neither sent nor stored for third-party requests (the request's site
 * differs from the top-level site), SameSite defaults to Lax, and only persistent
 * first-party cookies are saved to disk. Used from the I/O thread only.
 */
#ifndef HK_COOKIES_H
#define HK_COOKIES_H

#include "base/hk_arena.h"
#include "base/hk_vec.h"
#include "net/hk_url.h"

typedef struct hk_cookie {
    char *name, *value, *domain, *path;
    bool host_only, secure, http_only, persistent;
    uint8_t same_site; /* 0 lax (default), 1 strict, 2 none */
    int64_t expires;   /* seconds since 1970, persistent cookies only */
    int64_t created;
} hk_cookie;

typedef struct hk_cookie_jar {
    HK_VEC(hk_cookie) cookies;
    bool dirty;
} hk_cookie_jar;

void hk_cookie_jar_init(hk_cookie_jar *j);
void hk_cookie_jar_free(hk_cookie_jar *j);

/* Processes a Set-Cookie header for a response from url. site_host is the top-level site's host. */
void hk_cookie_set(hk_cookie_jar *j, const hk_url *url, hk_str site_host, hk_str header, int64_t now);
/* The Cookie header value for a request (empty when nothing applies). */
void hk_cookie_header(hk_cookie_jar *j, const hk_url *url, hk_str site_host, bool top_level, int64_t now,
                      hk_charbuf *out);

/* Persistence: a tab-separated file of persistent cookies. */
void hk_cookie_jar_load(hk_cookie_jar *j, hk_str text, int64_t now);
void hk_cookie_jar_save(const hk_cookie_jar *j, hk_charbuf *out, int64_t now);

/* Parses an HTTP date (IMF-fixdate and the common legacy forms); returns -1 if invalid. */
int64_t hk_http_parse_date(hk_str s);

#endif
