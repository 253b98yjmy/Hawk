/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HTTP cache on disk (docs/design/02-network.md, 8). Entries are keyed by the top-level
 * site and the URL, so one site cannot detect what another site loaded. Bodies are stored
 * decoded (after Content-Encoding). Used from the I/O thread only.
 */
#ifndef HK_CACHE_H
#define HK_CACHE_H

#include "base/hk_vec.h"
#include "net/hk_http.h"

typedef struct hk_cache hk_cache;

typedef struct hk_cache_entry {
    int status;
    char content_type[128];
    char etag[128];
    char last_modified[64];
    int64_t stored;
    int64_t fresh_until; /* seconds since 1970 */
    hk_charbuf body;
} hk_cache_entry;

/* dir NULL: caching disabled. Entries above max_bytes in total are evicted, oldest first. */
hk_cache *hk_cache_open(const char *dir, uint64_t max_bytes);
void hk_cache_close(hk_cache *c);

bool hk_cache_lookup(hk_cache *c, hk_str site, hk_str url, hk_cache_entry *out);
void hk_cache_entry_free(hk_cache_entry *e);

/* Freshness lifetime (seconds) of a response, or -1 when it must not be stored. */
int64_t hk_cache_lifetime(const hk_http_response *r, int64_t now);

/* Stores a 200 response; does nothing when the headers forbid it. */
void hk_cache_store(hk_cache *c, hk_str site, hk_str url, const hk_http_response *r, const uint8_t *body, size_t n,
                    int64_t now);
/* Updates the freshness of an entry after a 304 Not Modified. */
void hk_cache_refresh(hk_cache *c, hk_str site, hk_str url, const hk_http_response *r, const hk_cache_entry *e,
                      int64_t now);

#endif
