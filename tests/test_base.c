/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests for base/: memory, arena, strings, vectors, hash map, atoms. */
#include "hk_test.h"
#include "base/hk_arena.h"
#include "base/hk_atom.h"
#include "base/hk_map64.h"
#include "base/hk_mem.h"
#include "base/hk_vec.h"

#include <stdio.h>
#include <string.h>

HK_TEST(mem_accounting)
{
    int64_t before = hk_mem_heap_bytes();
    void *p = hk_alloc(100);
    HK_CHECK_EQ(hk_mem_heap_bytes() - before, 100);
    p = hk_realloc(p, 100, 300);
    HK_CHECK_EQ(hk_mem_heap_bytes() - before, 300);
    hk_free(p, 300);
    HK_CHECK_EQ(hk_mem_heap_bytes(), before);
    HK_CHECK(hk_realloc(hk_alloc(8), 8, 0) == NULL);
    HK_CHECK_EQ(hk_mem_heap_bytes(), before);
}

HK_TEST(arena_alignment)
{
    hk_arena a;
    hk_arena_init(&a, 1024);
    for (size_t align = 1; align <= HK_ARENA_MAX_ALIGN; align *= 2) {
        hk_arena_alloc(&a, 3, 1); /* misalign the cursor */
        void *p = hk_arena_alloc(&a, 24, align);
        HK_CHECK(((uintptr_t)p & (align - 1)) == 0);
    }
    /* Fill past one chunk; allocations must not overlap. */
    uint8_t *prev = NULL;
    for (int i = 0; i < 200; i++) {
        uint8_t *p = hk_arena_alloc(&a, 16, 1);
        memset(p, i, 16);
        if (prev) HK_CHECK(prev[0] == (uint8_t)(i - 1));
        prev = p;
    }
    HK_CHECK(a.bytes_reserved >= 2 * 1024);
    hk_arena_free(&a);
    HK_CHECK_EQ(a.bytes_reserved, 0);
}

HK_TEST(arena_big_alloc)
{
    hk_arena a;
    hk_arena_init(&a, 1024);
    uint8_t *small = hk_arena_alloc(&a, 8, 8);
    uint8_t *big = hk_arena_alloc(&a, 4000, 8); /* > chunk/4: dedicated chunk */
    uint8_t *small2 = hk_arena_alloc(&a, 8, 8);
    memset(big, 0xAB, 4000);
    HK_CHECK(small2 == small + 8); /* the regular chunk keeps being used */
    HK_CHECK(a.big != NULL);
    hk_arena_free(&a);
}

HK_TEST(arena_reset)
{
    hk_arena a;
    hk_arena_init(&a, 1024);
    for (int i = 0; i < 100; i++) hk_arena_alloc(&a, 100, 4);
    hk_arena_alloc(&a, 5000, 4);
    size_t one_chunk = a.chunk_size;
    hk_arena_reset(&a);
    HK_CHECK_EQ(a.bytes_used, 0);
    HK_CHECK(a.big == NULL);
    HK_CHECK(a.bytes_reserved >= one_chunk && a.bytes_reserved < 2 * one_chunk);
    void *p = hk_arena_alloc(&a, 10, 1);
    HK_CHECK(p != NULL);
    hk_arena_free(&a);
}

HK_TEST(arena_rewind)
{
    hk_arena a;
    hk_arena_init(&a, 1024);
    hk_arena_alloc(&a, 100, 1);
    hk_arena_mark m = hk_arena_get_mark(&a);
    size_t reserved = a.bytes_reserved;
    uint8_t *first_after_mark = hk_arena_alloc(&a, 10, 1);
    for (int i = 0; i < 50; i++) hk_arena_alloc(&a, 100, 1);
    hk_arena_alloc(&a, 3000, 1);
    hk_arena_rewind(&a, m);
    HK_CHECK_EQ(a.bytes_reserved, reserved);
    HK_CHECK(hk_arena_alloc(&a, 10, 1) == first_after_mark);

    /* Mark on an empty arena. */
    hk_arena b;
    hk_arena_init(&b, 0);
    hk_arena_mark empty = hk_arena_get_mark(&b);
    hk_arena_alloc(&b, 64, 8);
    hk_arena_rewind(&b, empty);
    HK_CHECK_EQ(b.bytes_reserved, 0);
    hk_arena_free(&b);
    hk_arena_free(&a);
}

HK_TEST(str_compare)
{
    HK_CHECK(hk_str_eq(HK_STR("abc"), HK_STR("abc")));
    HK_CHECK(!hk_str_eq(HK_STR("abc"), HK_STR("abd")));
    HK_CHECK(!hk_str_eq(HK_STR("abc"), HK_STR("ab")));
    HK_CHECK(hk_str_eq(HK_STR(""), hk_str_from_cstr(NULL)));
    HK_CHECK(hk_str_eq_ascii_icase(HK_STR("Content-Type"), HK_STR("content-type")));
    HK_CHECK(!hk_str_eq_ascii_icase(HK_STR("\xC3\x84"), HK_STR("\xC3\xA4"))); /* ASCII only */
    HK_CHECK(hk_str_starts_with(HK_STR("https://x"), HK_STR("https:")));
    HK_CHECK(!hk_str_starts_with(HK_STR("http"), HK_STR("https")));
    HK_CHECK(hk_str_ends_with(HK_STR("a.css"), HK_STR(".css")));
    HK_CHECK(hk_str_ends_with(HK_STR("a"), HK_STR("")));
}

HK_TEST(str_find)
{
    HK_CHECK_EQ(hk_str_find(HK_STR("hello world"), HK_STR("world")), 6);
    HK_CHECK_EQ(hk_str_find(HK_STR("aaab"), HK_STR("aab")), 1);
    HK_CHECK_EQ(hk_str_find(HK_STR("abc"), HK_STR("abcd")), HK_STR_NPOS);
    HK_CHECK_EQ(hk_str_find(HK_STR("abc"), HK_STR("")), 0);
    HK_CHECK_EQ(hk_str_find(HK_STR("abc"), HK_STR("c")), 2);
    HK_CHECK_EQ(hk_str_find(HK_STR(""), HK_STR("a")), HK_STR_NPOS);
    HK_CHECK_EQ(hk_str_find_char(HK_STR("a=b"), '='), 1);
    HK_CHECK_EQ(hk_str_find_char(HK_STR("ab"), '='), HK_STR_NPOS);
}

HK_TEST(str_slice_trim)
{
    hk_str s = HK_STR("abcdef");
    HK_CHECK_STR(hk_str_slice(s, 1, 3), "bc");
    HK_CHECK_STR(hk_str_slice(s, 4, 100), "ef");
    HK_CHECK_STR(hk_str_slice(s, 10, 2), "");
    HK_CHECK_STR(hk_str_trim_ascii_ws(HK_STR(" \t\r\n\fx y\n")), "x y");
    HK_CHECK_STR(hk_str_trim_ascii_ws(HK_STR("   ")), "");
}

HK_TEST(str_hash_stable)
{
    /* Reference FNV-1a values: the hash is used in on-disk formats and must not change. */
    HK_CHECK_EQ(hk_hash64("", 0), 0xcbf29ce484222325ull);
    HK_CHECK_EQ(hk_hash64("a", 1), 0xaf63dc4c8601ec8cull);
    HK_CHECK_EQ(hk_hash32("", 0), 0x811c9dc5u);
    HK_CHECK_EQ(hk_hash32("a", 1), 0xe40c292cu);
}

HK_TEST(vec_push)
{
    HK_VEC(uint32_t) v = {0};
    for (uint32_t i = 0; i < 1000; i++) hk_vec_push(&v, i * 3);
    HK_CHECK_EQ(v.len, 1000);
    HK_CHECK(v.cap >= 1000);
    bool ok = true;
    for (uint32_t i = 0; i < v.len; i++) ok = ok && v.data[i] == i * 3;
    HK_CHECK(ok);
    hk_vec_free(&v);
    HK_CHECK(v.data == NULL && v.len == 0 && v.cap == 0);
}

HK_TEST(map64_basic)
{
    hk_map64 m;
    hk_map64_init(&m);
    uint64_t v = 0;
    HK_CHECK(!hk_map64_get(&m, 5, &v));
    hk_map64_put(&m, 5, 50);
    hk_map64_put(&m, 6, 60);
    hk_map64_put(&m, 5, 55);
    HK_CHECK(hk_map64_get(&m, 5, &v) && v == 55);
    HK_CHECK(hk_map64_get(&m, 6, &v) && v == 60);
    HK_CHECK_EQ(m.len, 2);
    HK_CHECK(hk_map64_remove(&m, 5));
    HK_CHECK(!hk_map64_remove(&m, 5));
    HK_CHECK(!hk_map64_get(&m, 5, &v));
    HK_CHECK(hk_map64_get(&m, 6, &v) && v == 60);
    hk_map64_free(&m);
}

HK_TEST(map64_many)
{
    hk_map64 m;
    hk_map64_init(&m);
    const uint64_t n = 20000;
    for (uint64_t i = 1; i <= n; i++) hk_map64_put(&m, i * 0x1000, i); /* low bits all equal */
    HK_CHECK_EQ(m.len, n);
    for (uint64_t i = 1; i <= n; i += 2) HK_CHECK(hk_map64_remove(&m, i * 0x1000));
    HK_CHECK_EQ(m.len, n / 2);
    int bad = 0;
    for (uint64_t i = 1; i <= n; i++) {
        uint64_t v = 0;
        bool found = hk_map64_get(&m, i * 0x1000, &v);
        if ((i % 2 == 1) == found || (found && v != i)) bad++;
    }
    HK_CHECK_EQ(bad, 0);
    hk_map64_free(&m);
}

HK_TEST(atom_intern)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_atom_table tab;
    hk_atom_table_init(&tab, &a, NULL);

    char buf[8] = "div";
    hk_atom div = hk_atom_intern(&tab, hk_str_from_cstr(buf));
    memcpy(buf, "xyz", 4); /* the table must have copied the string */
    HK_CHECK(div != 0);
    HK_CHECK_STR(hk_atom_str(&tab, div), "div");
    HK_CHECK_EQ(hk_atom_intern(&tab, HK_STR("div")), div);
    HK_CHECK(hk_atom_intern(&tab, HK_STR("span")) != div);
    HK_CHECK_EQ(hk_atom_find(&tab, HK_STR("p")), 0);
    HK_CHECK_EQ(hk_atom_hash(&tab, div), hk_str_hash64(HK_STR("div")));

    /* Enough atoms to force several index resizes. */
    char name[16];
    for (int i = 0; i < 5000; i++) {
        int len = snprintf(name, sizeof name, "n%d", i);
        hk_atom_intern(&tab, (hk_str){name, (size_t)len});
    }
    HK_CHECK_EQ(hk_atom_table_count(&tab), 5002);
    HK_CHECK_STR(hk_atom_str(&tab, hk_atom_find(&tab, HK_STR("n4321"))), "n4321");

    hk_atom_table_free(&tab);
    hk_arena_free(&a);
}

HK_TEST(atom_parent_chain)
{
    hk_atom_table global;
    hk_atom_table_init(&global, NULL, NULL);
    hk_atom html = hk_atom_intern_static(&global, HK_STR("html"));
    hk_atom body = hk_atom_intern_static(&global, HK_STR("body"));
    HK_CHECK_EQ(html, 1);
    HK_CHECK_EQ(body, 2);

    hk_arena a;
    hk_arena_init(&a, 0);
    hk_atom_table doc;
    hk_atom_table_init(&doc, &a, &global);
    HK_CHECK_EQ(hk_atom_intern(&doc, HK_STR("body")), body); /* parent ids are shared */
    hk_atom cls = hk_atom_intern(&doc, HK_STR("ad-banner"));
    HK_CHECK_EQ(cls, 3);
    HK_CHECK_STR(hk_atom_str(&doc, html), "html");
    HK_CHECK_STR(hk_atom_str(&doc, cls), "ad-banner");
    HK_CHECK_EQ(hk_atom_find(&global, HK_STR("ad-banner")), 0); /* parent is untouched */

    hk_atom_table_free(&doc);
    hk_arena_free(&a);
    hk_atom_table_free(&global);
}
