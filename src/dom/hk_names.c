/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dom/hk_names.h"

static const hk_str k_names[] = {
#define HK_NAME(ident, str) HK_STR_INIT(str),
#include "dom/hk_names.def"
#undef HK_NAME
};

static hk_atom_table g_table;
static bool g_initialized;

void hk_names_init(void)
{
    if (g_initialized) return;
    hk_atom_table_init(&g_table, NULL, NULL);
    for (size_t i = 0; i < HK_ARRAY_LEN(k_names); i++) {
        hk_atom a = hk_atom_intern_static(&g_table, k_names[i]);
        HK_ASSERT(a == i + 1); /* a duplicate entry in hk_names.def would break the enum */
    }
    g_initialized = true;
}

const hk_atom_table *hk_names_table(void)
{
    HK_ASSERT(g_initialized);
    return &g_table;
}
