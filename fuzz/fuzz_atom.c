/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interns the NUL/newline-separated pieces of the input into a chained table and checks lookups. */
#include "base/hk_atom.h"
#include "base/hk_map64.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_atom_table parent, child;
    hk_atom_table_init(&parent, &a, NULL);
    hk_atom_intern(&parent, (hk_str){"html", 4});
    hk_atom_table_init(&child, &a, &parent);

    hk_map64 removed;
    hk_map64_init(&removed);

    size_t start = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i < size && data[i] != '\n' && data[i] != '\0') continue;
        hk_str s = {(const char *)data + start, i - start};
        hk_atom at = hk_atom_intern(&child, s);
        HK_ASSERT(at != 0);
        HK_ASSERT(hk_atom_find(&child, s) == at);
        HK_ASSERT(hk_str_eq(hk_atom_str(&child, at), s));

        /* Exercise map insert/remove with the atom hash as key. */
        uint64_t key = hk_key64_nonzero(hk_atom_hash(&child, at));
        if (hk_map64_get(&removed, key, NULL)) {
            HK_ASSERT(hk_map64_remove(&removed, key));
        } else {
            hk_map64_put(&removed, key, at);
        }
        start = i + 1;
    }

    hk_map64_free(&removed);
    hk_atom_table_free(&child);
    hk_atom_table_free(&parent);
    hk_arena_free(&a);
    return 0;
}
