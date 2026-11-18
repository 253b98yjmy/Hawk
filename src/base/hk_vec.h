/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Heap-backed growable arrays.
 *   HK_VEC(int) v = {0};
 *   hk_vec_push(&v, 42);
 *   hk_vec_free(&v);
 */
#ifndef HK_VEC_H
#define HK_VEC_H

#include "base/hk_common.h"
#include "base/hk_mem.h"

#define HK_VEC(T)                                                                                  \
    struct {                                                                                       \
        T *data;                                                                                   \
        uint32_t len;                                                                              \
        uint32_t cap;                                                                              \
    }

/* Returns the reallocated buffer; updates *cap. Panics on overflow. */
void *hk_vec_grow_(void *data, uint32_t *cap, size_t elem_size, uint64_t min_cap);

#define hk_vec_reserve(v, n)                                                                       \
    do {                                                                                           \
        if ((uint64_t)(n) > (v)->cap)                                                              \
            (v)->data = hk_vec_grow_((v)->data, &(v)->cap, sizeof(*(v)->data), (uint64_t)(n));     \
    } while (0)

#define hk_vec_push(v, x)                                                                          \
    do {                                                                                           \
        hk_vec_reserve((v), (uint64_t)(v)->len + 1);                                               \
        (v)->data[(v)->len++] = (x);                                                               \
    } while (0)

#define hk_vec_free(v)                                                                             \
    do {                                                                                           \
        hk_free((v)->data, (size_t)(v)->cap * sizeof(*(v)->data));                                 \
        (v)->data = NULL;                                                                          \
        (v)->len = (v)->cap = 0;                                                                   \
    } while (0)

#endif
