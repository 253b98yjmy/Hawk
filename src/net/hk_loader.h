/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Loader: fetches URLs on the I/O thread and reports to the main thread
 * (docs/design/02-network.md, 9). Applies, per request and per redirect hop:
 * HTTPS-only upgrade, the content blocker, then the HTTP request. file: URLs are read from disk for
 * navigations and for the subresources of file: pages only (never for web pages).
 */
#ifndef HK_LOADER_H
#define HK_LOADER_H

#include "base/hk_common.h"
#include "base/hk_str.h"
#include "blocker/hk_blocker.h"
#include "plat/hk_image.h"

typedef struct hk_loader hk_loader;

typedef struct hk_load_response {
    hk_str url; /* final URL after redirects */
    int status;
    hk_str content_type;
} hk_load_response;

/* All callbacks run on the main thread, in order: on_response?, on_data*, on_done. */
typedef struct hk_load_callbacks {
    void *user;
    void (*on_response)(void *user, const hk_load_response *r);
    void (*on_data)(void *user, const uint8_t *p, size_t n);
    /* message is a short English description when st != HK_OK. */
    void (*on_done)(void *user, hk_status st, const char *message);
    /* Image requests only (instead of on_data/on_done): the decoded image, owned by the receiver. */
    void (*on_image)(void *user, hk_status st, hk_image *img);
} hk_load_callbacks;

/* blocker may be NULL; it must outlive the loader. data_dir (may be NULL) holds cookies and the cache. */
hk_loader *hk_loader_create(const hk_blocker *blocker, const char *data_dir);
/* Replaces the blocker for requests that start from now on. Any thread. release(ctx) is called
 * (on the I/O thread) once the loader no longer uses b: after the next swap or at destroy. */
void hk_loader_set_blocker(hk_loader *l, const hk_blocker *b, void (*release)(void *ctx), void *ctx);
/* Cancels outstanding requests and stops the I/O thread. */
void hk_loader_destroy(hk_loader *l);

/* Starts a fetch; returns its id. Main thread only. site is the top-level page URL for subresources
 * (cookies and the cache are partitioned by it) and empty for navigations. type is one HK_REQ_*
 * (the content blocker's request type); navigations are always HK_REQ_DOCUMENT. */
uint32_t hk_loader_fetch(hk_loader *l, hk_str url, hk_str site, uint32_t type, const hk_load_callbacks *cb);
/* A navigation from a page (a link): referrer is that page's URL, or its origin when the page's
 * referrer policy asks for less. The Referer header follows docs/design/02-network.md, 5: the full URL
 * (without its fragment) to the same origin, only the origin elsewhere. Subresources send their page's
 * URL to the same origin, its origin within the same site and nothing to other sites. */
uint32_t hk_loader_navigate(hk_loader *l, hk_str url, hk_str referrer, const hk_load_callbacks *cb);
/* Submits a form as a navigation: a POST of body with the given Content-Type. origin is the submitting
 * page's origin, sent as the Origin header. A 301/302/303 redirect turns it into a GET. */
uint32_t hk_loader_post(hk_loader *l, hk_str url, hk_str content_type, hk_str body, hk_str origin,
                        hk_str referrer, const hk_load_callbacks *cb);
/* Fetches and decodes an image on the I/O thread, scaled down to fit max_w x max_h (0 = original size). */
uint32_t hk_loader_fetch_image(hk_loader *l, hk_str url, hk_str site, int max_w, int max_h,
                               const hk_load_callbacks *cb);
/* No further callbacks are made for a cancelled request. Main thread only. */
void hk_loader_cancel(hk_loader *l, uint32_t id);

#endif
