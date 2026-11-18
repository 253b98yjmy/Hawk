/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_mem.h"
#include "base/hk_atomic.h"

#include <stdlib.h>

static volatile int64_t g_heap_bytes;

static void account(size_t size, int sign)
{
    hk_atomic_add64(&g_heap_bytes, sign * (int64_t)size);
}

void *hk_alloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) hk_panic(__FILE__, __LINE__, "out of memory");
    account(size, 1);
    return p;
}

void *hk_alloc_zero(size_t size)
{
    void *p = calloc(1, size ? size : 1);
    if (!p) hk_panic(__FILE__, __LINE__, "out of memory");
    account(size, 1);
    return p;
}

void *hk_realloc(void *p, size_t old_size, size_t new_size)
{
    if (!p) return new_size ? hk_alloc(new_size) : NULL;
    if (new_size == 0) {
        hk_free(p, old_size);
        return NULL;
    }
    void *np = realloc(p, new_size);
    if (!np) hk_panic(__FILE__, __LINE__, "out of memory");
    account(old_size, -1);
    account(new_size, 1);
    return np;
}

void hk_free(void *p, size_t size)
{
    if (!p) return;
    account(size, -1);
    free(p);
}

int64_t hk_mem_heap_bytes(void)
{
    return hk_atomic_load64(&g_heap_bytes);
}
