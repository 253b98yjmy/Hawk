/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A small regular expression engine for filter lists (docs/design/04-blocker.md, 9):
 * literals, ".", classes, \d \w \s (and negations), ^ $, groups, "|", * + ? {m,n}.
 * Matching simulates the NFA in lockstep (Pike VM), so time is linear in the input:
 * hostile patterns cannot cause catastrophic backtracking. Back-references and
 * look-around are rejected.
 */
#ifndef HK_REGEX_H
#define HK_REGEX_H

#include "base/hk_common.h"
#include "base/hk_str.h"

typedef struct hk_regex hk_regex;

/* Returns NULL when the pattern uses unsupported syntax. */
hk_regex *hk_regex_compile(hk_str pattern, bool icase);
void hk_regex_free(hk_regex *re);
/* Unanchored search. */
bool hk_regex_search(const hk_regex *re, hk_str text);
/* Like search, but also reports the span of the leftmost match. */
bool hk_regex_find(const hk_regex *re, hk_str text, size_t *start, size_t *end);

#endif
