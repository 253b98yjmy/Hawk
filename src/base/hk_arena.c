/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_arena.h"
#include "base/hk_mem.h"

#include <string.h>

struct hk_arena_chunk {
    hk_arena_chunk *prev;
    size_t size; /* usable bytes after the header */
};

#define CHUNK_HDR hk_align_up(sizeof(hk_arena_chunk), HK_ARENA_MAX_ALIGN)

static uint8_t *chunk_data(hk_arena_chunk *c)
{
    return (uint8_t *)c + CHUNK_HDR;
}

static hk_arena_chunk *chunk_new(hk_arena *a, size_t data_size)
{
    HK_ASSERT(data_size <= SIZE_MAX - CHUNK_HDR);
    hk_arena_chunk *c = hk_alloc(CHUNK_HDR + data_size);
    c->prev = NULL;
    c->size = data_size;
    a->bytes_reserved += CHUNK_HDR + data_size;
    return c;
}

static void chunk_free(hk_arena *a, hk_arena_chunk *c)
{
    a->bytes_reserved -= CHUNK_HDR + c->size;
    hk_free(c, CHUNK_HDR + c->size);
}

void hk_arena_init(hk_arena *a, size_t chunk_size)
{
    memset(a, 0, sizeof *a);
    a->chunk_size = chunk_size ? chunk_size : HK_ARENA_DEFAULT_CHUNK;
}

static void free_big_until(hk_arena *a, hk_arena_chunk *stop)
{
    while (a->big != stop) {
        hk_arena_chunk *prev = a->big->prev;
        chunk_free(a, a->big);
        a->big = prev;
    }
}

void hk_arena_free(hk_arena *a)
{
    free_big_until(a, NULL);
    while (a->head) {
        hk_arena_chunk *prev = a->head->prev;
        chunk_free(a, a->head);
        a->head = prev;
    }
    size_t chunk_size = a->chunk_size;
    hk_arena_init(a, chunk_size);
}

void hk_arena_reset(hk_arena *a)
{
    free_big_until(a, NULL);
    if (!a->head) return;
    hk_arena_chunk *keep = a->head;
    hk_arena_chunk *c = keep->prev;
    while (c) {
        hk_arena_chunk *prev = c->prev;
        chunk_free(a, c);
        c = prev;
    }
    keep->prev = NULL;
    a->cur = chunk_data(keep);
    a->end = a->cur + keep->size;
    a->bytes_used = 0;
}

void *hk_arena_alloc(hk_arena *a, size_t size, size_t align)
{
    HK_ASSERT(align != 0 && (align & (align - 1)) == 0 && align <= HK_ARENA_MAX_ALIGN);
    if (size == 0) size = 1;

    if (size > a->chunk_size / 4) {
        hk_arena_chunk *c = chunk_new(a, size);
        c->prev = a->big;
        a->big = c;
        a->bytes_used += size;
        return chunk_data(c);
    }

    uintptr_t aligned = ((uintptr_t)a->cur + (align - 1)) & ~(uintptr_t)(align - 1);
    if (!a->cur || size > (uintptr_t)a->end - aligned || aligned > (uintptr_t)a->end) {
        hk_arena_chunk *c = chunk_new(a, a->chunk_size);
        c->prev = a->head;
        a->head = c;
        a->cur = chunk_data(c);
        a->end = a->cur + c->size;
        aligned = (uintptr_t)a->cur;
    }
    a->cur = (uint8_t *)(aligned + size);
    a->bytes_used += size;
    return (void *)aligned;
}

void *hk_arena_alloc_zero(hk_arena *a, size_t size, size_t align)
{
    void *p = hk_arena_alloc(a, size, align);
    memset(p, 0, size ? size : 1);
    return p;
}

void *hk_arena_alloc_array(hk_arena *a, size_t count, size_t elem_size, size_t align)
{
    HK_ASSERT(elem_size == 0 || count <= SIZE_MAX / elem_size);
    return hk_arena_alloc_zero(a, count * elem_size, align);
}

hk_str hk_arena_str_dup(hk_arena *a, hk_str s)
{
    HK_ASSERT(s.n < SIZE_MAX);
    char *p = hk_arena_alloc(a, s.n + 1, 1);
    if (s.n) memcpy(p, s.p, s.n);
    p[s.n] = '\0';
    return (hk_str){p, s.n};
}

hk_arena_mark hk_arena_get_mark(const hk_arena *a)
{
    hk_arena_mark m = {a->head, a->big, a->cur, a->bytes_used};
    return m;
}

void hk_arena_rewind(hk_arena *a, hk_arena_mark m)
{
    free_big_until(a, m.big);
    while (a->head != m.head) {
        HK_ASSERT(a->head != NULL); /* mark does not belong to this arena */
        hk_arena_chunk *prev = a->head->prev;
        chunk_free(a, a->head);
        a->head = prev;
    }
    a->cur = m.cur;
    a->end = a->head ? chunk_data(a->head) + a->head->size : NULL;
    a->bytes_used = m.bytes_used;
}
