/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hk_test.h"
#include "base/hk_mem.h"
#include "dom/hk_names.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define HK_TEST_ENTRY(name) void hk_test_##name(hk_test_ctx *t);
#include "tests.def"
#undef HK_TEST_ENTRY

typedef struct test_case {
    const char *name;
    void (*fn)(hk_test_ctx *t);
} test_case;

static const test_case k_tests[] = {
#define HK_TEST_ENTRY(name) {#name, hk_test_##name},
#include "tests.def"
#undef HK_TEST_ENTRY
};

void hk_test_fail(hk_test_ctx *t, const char *file, int line, const char *fmt, ...)
{
    t->failures++;
    fprintf(stderr, "%s:%d: [%s] ", file, line, t->name);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int run(const test_case *tc)
{
    hk_test_ctx t = {tc->name, 0};
    int64_t heap_before = hk_mem_heap_bytes();
    tc->fn(&t);
    int64_t leaked = hk_mem_heap_bytes() - heap_before;
    if (leaked != 0) hk_test_fail(&t, __FILE__, __LINE__, "leaked %lld bytes", (long long)leaked);
    printf("%s %s\n", t.failures ? "FAIL" : "ok  ", tc->name);
    return t.failures ? 1 : 0;
}

/* Usage: hk_tests            run all tests
 *        hk_tests NAME...    run the named tests
 *        hk_tests --list     print test names */
int main(int argc, char **argv)
{
    int failed = 0, ran = 0;
    hk_names_init(); /* process-lifetime table, outside the per-test leak check */
    if (argc > 1 && strcmp(argv[1], "--list") == 0) {
        for (size_t i = 0; i < HK_ARRAY_LEN(k_tests); i++) printf("%s\n", k_tests[i].name);
        return 0;
    }
    for (size_t i = 0; i < HK_ARRAY_LEN(k_tests); i++) {
        bool selected = argc <= 1;
        for (int a = 1; a < argc && !selected; a++) selected = strcmp(argv[a], k_tests[i].name) == 0;
        if (!selected) continue;
        failed += run(&k_tests[i]);
        ran++;
    }
    if (ran == 0) {
        fprintf(stderr, "no matching tests\n");
        return 2;
    }
    printf("%d/%d passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
