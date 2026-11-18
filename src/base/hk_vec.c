/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_vec.h"

#include <string.h>

void hk_charbuf_append(hk_charbuf *b, const char *p, size_t n)
{
    if (n == 0) return;
    HK_ASSERT(n <= UINT32_MAX - b->len);
    hk_vec_reserve(b, (uint64_t)b->len + n);
    memcpy(b->data + b->len, p, n);
    b->len += (uint32_t)n;
}

void *hk_vec_grow_(void *data, uint32_t *cap, size_t elem_size, uint64_t min_cap)
{
    HK_ASSERT(elem_size > 0);
    HK_ASSERT(min_cap <= UINT32_MAX);
    uint64_t new_cap = *cap ? (uint64_t)*cap * 2 : 8;
    if (new_cap < min_cap) new_cap = min_cap;
    if (new_cap > UINT32_MAX) new_cap = UINT32_MAX;
    HK_ASSERT(new_cap <= SIZE_MAX / elem_size);
    void *nd = hk_realloc(data, (size_t)*cap * elem_size, (size_t)new_cap * elem_size);
    *cap = (uint32_t)new_cap;
    return nd;
}
