/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Filter lists on disk (docs/design/04-blocker.md, 6):
 *   <data>\filters\<name>.txt   the lists as downloaded
 *   <data>\filters.bin          the compiled image, mapped read-only
 * filters.bin records a stamp of the lists it was built from and is rebuilt when they change.
 */
#ifndef HK_FILTER_STORE_H
#define HK_FILTER_STORE_H

#include "blocker/hk_blocker.h"
#include "net/hk_loader.h"

typedef struct hk_filter_store hk_filter_store;

/* Loads (or rebuilds) the image for the lists in data_dir plus extra list files. data_dir may be
 * NULL (then only the extra lists are used, and nothing is written). Never returns NULL; the
 * blocker of a store without lists blocks nothing. */
hk_filter_store *hk_filter_store_open(const char *data_dir, const char *const *extra, int nextra);
const hk_blocker *hk_filter_store_blocker(const hk_filter_store *s);
/* Frees the store (a release function for hk_loader_set_blocker). */
void hk_filter_store_release(void *store);

/* The default lists (README, D3). */
typedef struct hk_filter_list_info {
    const char *name; /* file name without ".txt" */
    const char *url;
} hk_filter_list_info;

extern const hk_filter_list_info hk_default_filter_lists[];
extern const size_t hk_default_filter_list_count;

/* True when no default list has been downloaded yet (the first run). */
bool hk_filter_lists_missing(const char *data_dir);

/* Downloads the default lists that are missing or expired ("! Expires:", 1-14 days, default 4;
 * all of them when force), then rebuilds filters.bin on a worker thread and hands the new
 * blocker to the loader. done runs on the main thread with a short summary. Main thread only. */
void hk_filter_lists_update(hk_loader *l, const char *data_dir, bool force, void (*done)(void *user, const char *summary),
                            void *user);

#endif
