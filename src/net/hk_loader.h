/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Loader: fetches URLs on the I/O thread and reports to the main thread
 * (docs/design/02-network.md, 9). Applies, per request and per redirect hop:
 * HTTPS-only upgrade, the content blocker, then the HTTP request.
 */
#ifndef HK_LOADER_H
#define HK_LOADER_H

#include "base/hk_common.h"
#include "base/hk_str.h"
#include "blocker/hk_blocker.h"

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
} hk_load_callbacks;

/* blocker may be NULL; it must outlive the loader. */
hk_loader *hk_loader_create(const hk_blocker *blocker);
/* Cancels outstanding requests and stops the I/O thread. */
void hk_loader_destroy(hk_loader *l);

/* Starts a fetch; returns its id. Main thread only. */
uint32_t hk_loader_fetch(hk_loader *l, hk_str url, const hk_load_callbacks *cb);
/* No further callbacks are made for a cancelled request. Main thread only. */
void hk_loader_cancel(hk_loader *l, uint32_t id);

#endif
