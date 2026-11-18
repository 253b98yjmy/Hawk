/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_map64.h"
#include "base/hk_mem.h"

#include <string.h>

/* splitmix64 finalizer: spreads keys that are small integers or share low bits. */
static uint64_t mix(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

void hk_map64_init(hk_map64 *m)
{
    memset(m, 0, sizeof *m);
}

void hk_map64_free(hk_map64 *m)
{
    hk_free(m->keys, (size_t)m->cap * sizeof(uint64_t));
    hk_free(m->vals, (size_t)m->cap * sizeof(uint64_t));
    hk_map64_init(m);
}

static void insert_raw(uint64_t *keys, uint64_t *vals, uint32_t cap, uint64_t key, uint64_t val)
{
    uint32_t mask = cap - 1;
    uint32_t i = (uint32_t)mix(key) & mask;
    while (keys[i] != 0) i = (i + 1) & mask;
    keys[i] = key;
    vals[i] = val;
}

static void rehash(hk_map64 *m, uint32_t new_cap)
{
    uint64_t *keys = hk_alloc_zero((size_t)new_cap * sizeof(uint64_t));
    uint64_t *vals = hk_alloc_zero((size_t)new_cap * sizeof(uint64_t));
    for (uint32_t i = 0; i < m->cap; i++) {
        if (m->keys[i]) insert_raw(keys, vals, new_cap, m->keys[i], m->vals[i]);
    }
    uint32_t len = m->len;
    hk_map64_free(m);
    m->keys = keys;
    m->vals = vals;
    m->cap = new_cap;
    m->len = len;
}

static bool find_slot(const hk_map64 *m, uint64_t key, uint32_t *out)
{
    if (m->cap == 0) return false;
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)mix(key) & mask;
    while (m->keys[i] != 0) {
        if (m->keys[i] == key) {
            *out = i;
            return true;
        }
        i = (i + 1) & mask;
    }
    return false;
}

bool hk_map64_get(const hk_map64 *m, uint64_t key, uint64_t *out_val)
{
    HK_ASSERT(key != 0);
    uint32_t i;
    if (!find_slot(m, key, &i)) return false;
    if (out_val) *out_val = m->vals[i];
    return true;
}

void hk_map64_put(hk_map64 *m, uint64_t key, uint64_t val)
{
    HK_ASSERT(key != 0);
    uint32_t i;
    if (find_slot(m, key, &i)) {
        m->vals[i] = val;
        return;
    }
    /* Keep the load factor at or below 70%. */
    if (((uint64_t)m->len + 1) * 10 > (uint64_t)m->cap * 7) {
        HK_ASSERT(m->cap <= UINT32_MAX / 2);
        rehash(m, m->cap ? m->cap * 2 : 16);
    }
    insert_raw(m->keys, m->vals, m->cap, key, val);
    m->len++;
}

bool hk_map64_remove(hk_map64 *m, uint64_t key)
{
    HK_ASSERT(key != 0);
    uint32_t i;
    if (!find_slot(m, key, &i)) return false;
    uint32_t mask = m->cap - 1;
    uint32_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (m->keys[j] == 0) break;
        uint32_t home = (uint32_t)mix(m->keys[j]) & mask;
        /* Entry j may stay only if its home slot lies cyclically in (i, j]. */
        bool stays = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
        if (!stays) {
            m->keys[i] = m->keys[j];
            m->vals[i] = m->vals[j];
            i = j;
        }
    }
    m->keys[i] = 0;
    m->vals[i] = 0;
    m->len--;
    return true;
}
