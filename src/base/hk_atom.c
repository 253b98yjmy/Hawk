/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_atom.h"
#include "base/hk_mem.h"

#include <string.h>

void hk_atom_table_init(hk_atom_table *t, hk_arena *arena, const hk_atom_table *parent)
{
    memset(t, 0, sizeof *t);
    t->arena = arena;
    t->parent = parent;
    t->base = parent ? hk_atom_table_count(parent) : 0;
}

void hk_atom_table_free(hk_atom_table *t)
{
    hk_vec_free(&t->entries);
    hk_free(t->slots, (size_t)t->slot_cap * sizeof(uint32_t));
    t->slots = NULL;
    t->slot_cap = 0;
}

uint32_t hk_atom_table_count(const hk_atom_table *t)
{
    return t->base + t->entries.len;
}

static hk_atom find_local(const hk_atom_table *t, hk_str s, uint64_t h)
{
    if (t->slot_cap == 0) return 0;
    uint32_t mask = t->slot_cap - 1;
    for (uint32_t i = (uint32_t)h & mask; t->slots[i] != 0; i = (i + 1) & mask) {
        const hk_atom_entry *e = &t->entries.data[t->slots[i] - 1];
        if (e->hash == h && hk_str_eq(e->str, s)) return t->base + t->slots[i];
    }
    return 0;
}

static hk_atom find_hashed(const hk_atom_table *t, hk_str s, uint64_t h)
{
    if (t->parent) {
        hk_atom a = find_hashed(t->parent, s, h);
        if (a) return a;
    }
    return find_local(t, s, h);
}

hk_atom hk_atom_find(const hk_atom_table *t, hk_str s)
{
    return find_hashed(t, s, hk_str_hash64(s));
}

static void place(uint32_t *slots, uint32_t cap, uint64_t h, uint32_t value)
{
    uint32_t mask = cap - 1;
    uint32_t i = (uint32_t)h & mask;
    while (slots[i] != 0) i = (i + 1) & mask;
    slots[i] = value;
}

static void grow_slots(hk_atom_table *t)
{
    HK_ASSERT(t->slot_cap <= UINT32_MAX / 2);
    uint32_t cap = t->slot_cap ? t->slot_cap * 2 : 64;
    uint32_t *slots = hk_alloc_zero((size_t)cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < t->entries.len; i++) {
        place(slots, cap, t->entries.data[i].hash, i + 1);
    }
    hk_free(t->slots, (size_t)t->slot_cap * sizeof(uint32_t));
    t->slots = slots;
    t->slot_cap = cap;
}

static hk_atom intern(hk_atom_table *t, hk_str s, bool copy)
{
    HK_ASSERT(!t->parent || hk_atom_table_count(t->parent) == t->base);
    uint64_t h = hk_str_hash64(s);
    hk_atom a = find_hashed(t, s, h);
    if (a) return a;

    HK_ASSERT(hk_atom_table_count(t) < UINT32_MAX - 1);
    if (((uint64_t)t->entries.len + 1) * 10 > (uint64_t)t->slot_cap * 7) grow_slots(t);

    if (copy) {
        HK_ASSERT(t->arena != NULL);
        s = hk_arena_str_dup(t->arena, s);
    }
    hk_atom_entry e = {s, h};
    hk_vec_push(&t->entries, e);
    place(t->slots, t->slot_cap, h, t->entries.len);
    return t->base + t->entries.len;
}

hk_atom hk_atom_intern(hk_atom_table *t, hk_str s)
{
    return intern(t, s, true);
}

hk_atom hk_atom_intern_static(hk_atom_table *t, hk_str s)
{
    return intern(t, s, false);
}

static const hk_atom_entry *entry_of(const hk_atom_table *t, hk_atom a)
{
    HK_ASSERT(a != 0 && a <= hk_atom_table_count(t));
    while (a <= t->base) t = t->parent;
    return &t->entries.data[a - t->base - 1];
}

hk_str hk_atom_str(const hk_atom_table *t, hk_atom a)
{
    return entry_of(t, a)->str;
}

uint64_t hk_atom_hash(const hk_atom_table *t, hk_atom a)
{
    return entry_of(t, a)->hash;
}
