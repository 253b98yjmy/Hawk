/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The few atomic operations Hawk needs (statistics counters). */
#ifndef HK_ATOMIC_H
#define HK_ATOMIC_H

#include "base/hk_common.h"

#if defined(_MSC_VER)
#include <intrin.h>

/* Returns the new value. */
static inline int64_t hk_atomic_add64(volatile int64_t *p, int64_t v)
{
    return _InterlockedExchangeAdd64((volatile long long *)p, v) + v;
}

static inline int64_t hk_atomic_load64(volatile int64_t *p)
{
    return _InterlockedOr64((volatile long long *)p, 0);
}
#else
static inline int64_t hk_atomic_add64(volatile int64_t *p, int64_t v)
{
    return __atomic_add_fetch(p, v, __ATOMIC_RELAXED);
}

static inline int64_t hk_atomic_load64(volatile int64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}
#endif

#endif
