/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Bump-pointer arena. Individual allocations are never freed; the whole arena
 * is reset or freed at once (see docs/design/01-architecture.md, 4.1).
 * Not thread-safe: an arena belongs to one thread at a time.
 */
#ifndef HK_ARENA_H
#define HK_ARENA_H

#include "base/hk_common.h"
#include "base/hk_str.h"

#define HK_ARENA_DEFAULT_CHUNK ((size_t)64 * 1024)
#define HK_ARENA_MAX_ALIGN 16

typedef struct hk_arena_chunk hk_arena_chunk;

typedef struct hk_arena {
    hk_arena_chunk *head; /* current regular chunk; older ones via ->prev */
    hk_arena_chunk *big;  /* dedicated chunks for large allocations */
    uint8_t *cur;
    uint8_t *end;
    size_t chunk_size;
    size_t bytes_reserved; /* chunk memory obtained from the heap */
    size_t bytes_used;     /* bytes handed out to callers */
} hk_arena;

typedef struct hk_arena_mark {
    hk_arena_chunk *head;
    hk_arena_chunk *big;
    uint8_t *cur;
    size_t bytes_used;
} hk_arena_mark;

/* chunk_size == 0 selects HK_ARENA_DEFAULT_CHUNK. */
void hk_arena_init(hk_arena *a, size_t chunk_size);
/* Releases all memory. The arena may be reused after hk_arena_init. */
void hk_arena_free(hk_arena *a);
/* Invalidates all allocations but keeps one chunk for reuse. */
void hk_arena_reset(hk_arena *a);

/* align must be a power of two <= HK_ARENA_MAX_ALIGN. Never returns NULL. */
void *hk_arena_alloc(hk_arena *a, size_t size, size_t align);
void *hk_arena_alloc_zero(hk_arena *a, size_t size, size_t align);
/* Panics if count * elem_size overflows. Memory is zeroed. */
void *hk_arena_alloc_array(hk_arena *a, size_t count, size_t elem_size, size_t align);

/* Copies s and appends a NUL (not counted in the returned length). */
hk_str hk_arena_str_dup(hk_arena *a, hk_str s);

/* Scoped temporary allocations: everything allocated after the mark is released by rewind. */
hk_arena_mark hk_arena_get_mark(const hk_arena *a);
void hk_arena_rewind(hk_arena *a, hk_arena_mark m);

#define HK_ARENA_NEW(a, T) ((T *)hk_arena_alloc_zero((a), sizeof(T), _Alignof(T)))
#define HK_ARENA_NEW_ARRAY(a, T, n) ((T *)hk_arena_alloc_array((a), (n), sizeof(T), _Alignof(T)))

#endif
