/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Common definitions shared by every Hawk module. */
#ifndef HK_COMMON_H
#define HK_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HK_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define HK_UNUSED(x) ((void)(x))
#define HK_MIN(a, b) ((a) < (b) ? (a) : (b))
#define HK_MAX(a, b) ((a) > (b) ? (a) : (b))

#if defined(_MSC_VER) && !defined(__clang__)
#define HK_NORETURN __declspec(noreturn)
#define HK_LIKELY(x) (x)
#define HK_UNLIKELY(x) (x)
#else
#define HK_NORETURN __attribute__((noreturn))
#define HK_LIKELY(x) __builtin_expect(!!(x), 1)
#define HK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

typedef enum hk_status {
    HK_OK = 0,
    HK_ERR_NOMEM,
    HK_ERR_INVALID,
    HK_ERR_IO,
    HK_ERR_UNSUPPORTED,
    HK_ERR_NOT_FOUND,
    HK_ERR_LIMIT,
    HK_ERR_BLOCKED, /* refused by the content blocker or a privacy rule */
} hk_status;

const char *hk_status_name(hk_status s);

/* Called by hk_panic after logging, before aborting. */
typedef void (*hk_panic_hook_fn)(const char *file, int line, const char *msg);
void hk_set_panic_hook(hk_panic_hook_fn fn);

HK_NORETURN void hk_panic(const char *file, int line, const char *msg);

/* Enabled in every build type: an inconsistent state must never continue. */
#define HK_ASSERT(cond)                                                                            \
    do {                                                                                           \
        if (HK_UNLIKELY(!(cond))) hk_panic(__FILE__, __LINE__, #cond);                             \
    } while (0)

static inline size_t hk_align_up(size_t v, size_t align)
{
    return (v + (align - 1)) & ~(align - 1);
}

#endif
