/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Content blocker. M1 implements category A of docs/design/04-blocker.md: host-only
 * network filters ("||example.com^") and hosts-file lines, matched against a host and
 * each of its parent domains. Exceptions ("@@||host^") take precedence.
 *
 * The blocker is built on the main thread and then only read (from any thread).
 */
#ifndef HK_BLOCKER_H
#define HK_BLOCKER_H

#include "base/hk_arena.h"
#include "base/hk_map64.h"
#include "base/hk_str.h"
#include "base/hk_vec.h"

typedef struct hk_blocker {
    hk_arena arena;
    hk_map64 block;     /* host hash -> index into names (plus one) */
    hk_map64 allow;     /* exceptions */
    HK_VEC(hk_str) names;
    uint32_t ignored;   /* filters of other categories (implemented in later milestones) */
} hk_blocker;

void hk_blocker_init(hk_blocker *b);
void hk_blocker_free(hk_blocker *b);

/* Adds the filters of a list (Adblock Plus syntax or hosts file). Returns the number added. */
uint32_t hk_blocker_add_list(hk_blocker *b, hk_str text);

/* True when requests to this host must not be made. host is lowercase ASCII. */
bool hk_blocker_blocks_host(const hk_blocker *b, hk_str host);

uint32_t hk_blocker_count(const hk_blocker *b);

#endif
