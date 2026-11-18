/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UTF-8 decoding/encoding following the WHATWG Encoding Standard, and UTF-16 conversion. */
#ifndef HK_UTF8_H
#define HK_UTF8_H

#include "base/hk_arena.h"
#include "base/hk_common.h"
#include "base/hk_str.h"

#define HK_REPLACEMENT_CHAR 0xFFFDu

/*
 * Decodes one code point from s[0..n), n > 0. Returns the number of bytes consumed (>= 1).
 * An invalid or truncated sequence yields U+FFFD and consumes its maximal subpart,
 * so decoding never stalls and matches the WHATWG decoder's error count.
 */
size_t hk_utf8_decode(const uint8_t *s, size_t n, uint32_t *cp);

/* Writes 1-4 bytes. Surrogates and values above U+10FFFF are encoded as U+FFFD. */
size_t hk_utf8_encode(uint32_t cp, uint8_t out[4]);

bool hk_utf8_valid(const uint8_t *s, size_t n);

/*
 * Converts to NUL-terminated UTF-16 allocated in the arena (invalid input becomes U+FFFD).
 * *out_len receives the length in code units, excluding the NUL. out_len may be NULL.
 */
uint16_t *hk_utf8_to_utf16(hk_arena *a, hk_str s, size_t *out_len);

/* Converts UTF-16 to UTF-8 in the arena. Unpaired surrogates become U+FFFD. NUL-terminated. */
hk_str hk_utf16_to_utf8(hk_arena *a, const uint16_t *s, size_t n);

#endif
