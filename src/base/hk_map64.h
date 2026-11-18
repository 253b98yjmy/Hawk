/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Open-addressing hash map from uint64_t keys to uint64_t values.
 * Linear probing, backward-shift deletion. Key 0 is reserved (use hk_key64_nonzero).
 */
#ifndef HK_MAP64_H
#define HK_MAP64_H

#include "base/hk_common.h"

typedef struct hk_map64 {
    uint64_t *keys;
    uint64_t *vals;
    uint32_t cap; /* power of two, or 0 */
    uint32_t len;
} hk_map64;

static inline uint64_t hk_key64_nonzero(uint64_t k)
{
    return k ? k : 1;
}

void hk_map64_init(hk_map64 *m);
void hk_map64_free(hk_map64 *m);
bool hk_map64_get(const hk_map64 *m, uint64_t key, uint64_t *out_val);
/* Inserts or overwrites. */
void hk_map64_put(hk_map64 *m, uint64_t key, uint64_t val);
bool hk_map64_remove(hk_map64 *m, uint64_t key);

#endif
