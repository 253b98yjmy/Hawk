/* SPDX-License-Identifier: GPL-3.0-or-later */
/* String slices: pointer + length, never assumed NUL-terminated. */
#ifndef HK_STR_H
#define HK_STR_H

#include "base/hk_common.h"

typedef struct hk_str {
    const char *p;
    size_t n;
} hk_str;

#define HK_STR_NPOS SIZE_MAX

/* For expressions: HK_STR("literal"). For static initializers: HK_STR_INIT("literal"). */
#define HK_STR(lit) ((hk_str){(lit), sizeof(lit) - 1})
#define HK_STR_INIT(lit) {(lit), sizeof(lit) - 1}

hk_str hk_str_from_cstr(const char *s);
bool hk_str_eq(hk_str a, hk_str b);
bool hk_str_eq_ascii_icase(hk_str a, hk_str b);
bool hk_str_starts_with(hk_str s, hk_str prefix);
bool hk_str_ends_with(hk_str s, hk_str suffix);
/* Index of the first occurrence, or HK_STR_NPOS. An empty needle matches at 0. */
size_t hk_str_find(hk_str haystack, hk_str needle);
size_t hk_str_find_char(hk_str s, char c);
/* Clamped to the string bounds: never out of range. */
hk_str hk_str_slice(hk_str s, size_t start, size_t end);
/* Trims ASCII whitespace as defined by HTML: TAB, LF, FF, CR, SPACE. */
hk_str hk_str_trim_ascii_ws(hk_str s);

static inline char hk_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static inline bool hk_ascii_is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* FNV-1a. Stable across runs and builds (used in on-disk formats). */
uint64_t hk_hash64(const void *data, size_t n);
uint32_t hk_hash32(const void *data, size_t n);

static inline uint64_t hk_str_hash64(hk_str s)
{
    return hk_hash64(s.p, s.n);
}

#endif
