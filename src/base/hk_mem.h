/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Heap allocation. The only place (with plat/) allowed to call malloc/free.
 * Sizes are passed to free/realloc so the live heap size can be accounted
 * exactly for hawk:about and for leak checks in tests.
 * Allocation failure is fatal: these functions never return NULL
 * (except hk_realloc to size 0).
 */
#ifndef HK_MEM_H
#define HK_MEM_H

#include "base/hk_common.h"

void *hk_alloc(size_t size);
void *hk_alloc_zero(size_t size);
/* old_size must be the size p was allocated with. new_size == 0 frees p and returns NULL. */
void *hk_realloc(void *p, size_t old_size, size_t new_size);
/* size must be the size p was allocated with. p may be NULL. */
void hk_free(void *p, size_t size);

/* Bytes currently allocated through this API (all threads). */
int64_t hk_mem_heap_bytes(void);

#endif
