/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Content blocker: network filters (docs/design/04-blocker.md, 1.1-1.2 and 3).
 *
 * Filter lists are compiled into a flat, position-independent image ("filters.bin",
 * section 5) which is then used in place: mapped from disk, or from a heap buffer.
 * A blocker is immutable once opened and may be used from any thread.
 *
 * Supported: "||host^", "|", "*", "^", "/regex/", "@@", comments, "!#if"/"!#else"/"!#endif",
 * hosts-file lines and the options in 1.2 (types, 1p/3p, domain=/from=, to=, denyallow=,
 * important, match-case, badfilter, removeparam, redirect). Filters for request types Hawk
 * never makes (script, xhr, frames, ...) and options that cannot be honoured are dropped,
 * and counted in hk_filter_stats.
 */
#ifndef HK_BLOCKER_H
#define HK_BLOCKER_H

#include "base/hk_common.h"
#include "base/hk_str.h"
#include "base/hk_vec.h"

typedef enum hk_request_type {
    HK_REQ_DOCUMENT = 1u << 0,
    HK_REQ_STYLESHEET = 1u << 1,
    HK_REQ_IMAGE = 1u << 2,
    HK_REQ_FONT = 1u << 3,
    HK_REQ_OTHER = 1u << 4,
} hk_request_type;

#define HK_REQ_ALL 0x1Fu

/* --- compiling ----------------------------------------------------------------------------- */

typedef struct hk_filter_stats {
    uint32_t network;     /* filters in the image */
    uint32_t hosts;       /* ... of which are indexed by host name (category A) */
    uint32_t untokenized; /* ... of which are checked for every request (category C) */
    uint32_t regex;       /* ... of which are regular expressions (category D) */
    uint32_t cosmetic;    /* element hiding: not compiled yet (M4) */
    uint32_t unsupported; /* options or request types Hawk cannot honour */
    uint32_t invalid;     /* syntax errors, unsupported regular expressions */
    uint32_t duplicate;
    uint32_t badfiltered; /* disabled by $badfilter */
} hk_filter_stats;

typedef struct hk_filter_compiler hk_filter_compiler;

hk_filter_compiler *hk_filter_compiler_create(void);
/* name identifies the list in logs; the text is copied as needed. */
void hk_filter_compiler_add_list(hk_filter_compiler *c, hk_str name, hk_str text);
/* Writes the image to out (appending), frees the compiler. stamp is stored in the header so the
 * caller can tell whether an image on disk is up to date (hk_blocker_image_stamp). */
void hk_filter_compiler_finish(hk_filter_compiler *c, uint64_t stamp, hk_charbuf *out, hk_filter_stats *stats);

/* The stamp of a valid image, or 0. */
uint64_t hk_blocker_image_stamp(const void *image, size_t n);

/* --- matching ------------------------------------------------------------------------------ */

typedef struct hk_blocker hk_blocker;

/* Validates the image and uses it in place: it must stay valid (and unchanged) until
 * hk_blocker_close. Returns NULL when the image is malformed. */
hk_blocker *hk_blocker_open(const void *image, size_t n);
void hk_blocker_close(hk_blocker *b);

typedef struct hk_block_request {
    hk_str url;       /* serialized absolute URL */
    hk_str host;      /* its host, lowercase */
    hk_str site_url;  /* the top-level page; empty for navigations */
    hk_str site_host;
    uint32_t type;    /* one HK_REQ_* */
} hk_block_request;

typedef struct hk_block_result {
    bool blocked;
    int32_t filter; /* the filter that decided (block or exception), or -1 */
} hk_block_result;

hk_block_result hk_blocker_match(const hk_blocker *b, const hk_block_request *r);
/* Applies $removeparam filters. Returns true and writes the new URL to out when it changes. */
bool hk_blocker_strip_params(const hk_blocker *b, const hk_block_request *r, hk_charbuf *out);

/* For logs: where a filter came from. */
hk_str hk_blocker_filter_list(const hk_blocker *b, int32_t filter);
uint32_t hk_blocker_filter_line(const hk_blocker *b, int32_t filter);
uint32_t hk_blocker_count(const hk_blocker *b);

/* Convenience for tests and small lists: compile text and open the result. The blocker owns it. */
hk_blocker *hk_blocker_from_text(hk_str text, hk_filter_stats *stats);

#endif
