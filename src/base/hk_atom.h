/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Atom tables: intern strings into small integers so names compare as integers.
 *
 * Tables can be chained: a per-document table is created with the global
 * static table (tag names, attribute names, CSS keywords, ...) as its parent.
 * Atoms of the parent keep their ids in the child; the child's own atoms are
 * numbered after them. A parent must not gain new atoms once it has children.
 *
 * Atom 0 means "no atom". Comparison is case-sensitive: callers lowercase
 * where the relevant specification says names are case-insensitive.
 */
#ifndef HK_ATOM_H
#define HK_ATOM_H

#include "base/hk_arena.h"
#include "base/hk_common.h"
#include "base/hk_str.h"
#include "base/hk_vec.h"

typedef uint32_t hk_atom;

typedef struct hk_atom_entry {
    hk_str str;
    uint64_t hash;
} hk_atom_entry;

typedef struct hk_atom_table {
    const struct hk_atom_table *parent;
    uint32_t base;            /* number of atoms in the parent chain */
    hk_arena *arena;          /* storage for copied strings; may be NULL for static-only tables */
    HK_VEC(hk_atom_entry) entries; /* entries[i] is atom base + 1 + i */
    uint32_t *slots;          /* entry index + 1, 0 = empty */
    uint32_t slot_cap;
} hk_atom_table;

void hk_atom_table_init(hk_atom_table *t, hk_arena *arena, const hk_atom_table *parent);
/* Frees the index. Strings live in the arena and are released with it. */
void hk_atom_table_free(hk_atom_table *t);
uint32_t hk_atom_table_count(const hk_atom_table *t);

/* Returns the atom for s, creating it (and copying s into the arena) if needed. */
hk_atom hk_atom_intern(hk_atom_table *t, hk_str s);
/* Like hk_atom_intern but does not copy: s must outlive the table (string literals). */
hk_atom hk_atom_intern_static(hk_atom_table *t, hk_str s);
/* Returns 0 when s has never been interned in t or its parents. */
hk_atom hk_atom_find(const hk_atom_table *t, hk_str s);

hk_str hk_atom_str(const hk_atom_table *t, hk_atom a);
uint64_t hk_atom_hash(const hk_atom_table *t, hk_atom a);

#endif
