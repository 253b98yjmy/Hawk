/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal test harness. Register each test in tests.def; CMake turns every
 * entry into its own ctest case. The runner also fails a test that leaks
 * memory allocated through hk_alloc.
 */
#ifndef HK_TEST_H
#define HK_TEST_H

#include "base/hk_common.h"
#include "base/hk_str.h"

typedef struct hk_test_ctx {
    const char *name;
    int failures;
} hk_test_ctx;

void hk_test_fail(hk_test_ctx *t, const char *file, int line, const char *fmt, ...);

#define HK_TEST(name) void hk_test_##name(hk_test_ctx *t)

#define HK_CHECK(cond)                                                                             \
    do {                                                                                           \
        if (!(cond)) hk_test_fail(t, __FILE__, __LINE__, "check failed: %s", #cond);               \
    } while (0)

#define HK_CHECK_EQ(a, b)                                                                          \
    do {                                                                                           \
        unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b);             \
        if (a_ != b_)                                                                              \
            hk_test_fail(t, __FILE__, __LINE__, "%s == %s: %llu != %llu", #a, #b, a_, b_);         \
    } while (0)

#define HK_CHECK_STR(s, lit)                                                                       \
    do {                                                                                           \
        hk_str s_ = (s);                                                                           \
        if (!hk_str_eq(s_, HK_STR(lit)))                                                           \
            hk_test_fail(t, __FILE__, __LINE__, "%s == \"%s\": got \"%.*s\"", #s, lit, (int)s_.n,  \
                         s_.p);                                                                    \
    } while (0)

#endif
