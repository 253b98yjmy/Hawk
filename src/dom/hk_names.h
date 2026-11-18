/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The global table of static atoms (see hk_names.def). */
#ifndef HK_NAMES_H
#define HK_NAMES_H

#include "base/hk_atom.h"

enum {
    HK_ATOM_NONE_ = 0,
#define HK_NAME(ident, str) HK_ATOM_##ident,
#include "dom/hk_names.def"
#undef HK_NAME
    HK_ATOM_STATIC_COUNT_PLUS_ONE_
};

#define HK_ATOM_STATIC_COUNT (HK_ATOM_STATIC_COUNT_PLUS_ONE_ - 1)

/* Builds the table. Call once at startup, before any other thread starts. */
void hk_names_init(void);
/* The table; per-document atom tables use it as their parent. */
const hk_atom_table *hk_names_table(void);

#endif
