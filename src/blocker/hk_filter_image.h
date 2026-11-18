/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Layout of the compiled filter image ("filters.bin", docs/design/04-blocker.md, 5).
 * All integers are little-endian; every offset is relative to the start of the image,
 * and every section starts on an 8-byte boundary.
 */
#ifndef HK_FILTER_IMAGE_H
#define HK_FILTER_IMAGE_H

#include "base/hk_common.h"

#define HK_FILTER_MAGIC "HKFLT\0\0\0"
#define HK_FILTER_VERSION 1u

enum {
    SEC_STR,     /* bytes: patterns, option values, list names */
    SEC_FILTERS, /* img_filter[] */
    SEC_DOMAINS, /* uint64_t[]: domain hashes, low bit = negated */
    SEC_HOSTIDX, /* img_bucket[] keyed by the hash of a host name (category A) */
    SEC_TOKIDX,  /* img_bucket[] keyed by the hash of a token (categories B and D) */
    SEC_POST,    /* uint32_t[]: filter indices referenced by the buckets */
    SEC_NOTOK,   /* uint32_t[]: filters checked for every request (category C) */
    SEC_PARAM,   /* uint32_t[]: $removeparam filters */
    SEC_LISTS,   /* img_span[]: list names */
    SEC_COUNT
};

/* img_filter.flags */
enum {
    F_EXCEPTION = 1u << 0,
    F_IMPORTANT = 1u << 1,
    F_MATCH_CASE = 1u << 2,
    F_REMOVEPARAM = 1u << 3,
    F_REGEX = 1u << 4,
    F_1P = 1u << 5,
    F_3P = 1u << 6,
    F_START = 1u << 7,  /* "|" at the start */
    F_END = 1u << 8,    /* "|" at the end */
    F_HOST = 1u << 9,   /* "||" */
    F_PARAM_REGEX = 1u << 10,
    F_PARAM_NEGATE = 1u << 11, /* removeparam=~name: remove all but name */
    F_PAGE = 1u << 12,         /* exception with an explicit $document: allows the whole page */
    F_TYPE_SHIFT = 16,         /* bits 16..20: HK_REQ_* mask */
};

typedef struct img_span {
    uint32_t off, len;
} img_span;

typedef struct img_header {
    char magic[8];
    uint32_t version;
    uint32_t nsec;
    uint64_t stamp;
    img_span sec[SEC_COUNT];
} img_header;

typedef struct img_filter {
    uint32_t flags;
    uint32_t pat_off;
    uint16_t pat_len;
    uint16_t from_n;
    uint32_t from_off; /* index into SEC_DOMAINS */
    uint16_t to_n;
    uint16_t param_len;
    uint32_t to_off;
    uint32_t param_off;
    uint32_t source; /* list << 20 | line */
} img_filter;

typedef struct img_bucket {
    uint64_t key; /* 0 = empty */
    uint32_t start, count;
} img_bucket;

static inline uint32_t img_filter_types(const img_filter *f)
{
    return (f->flags >> F_TYPE_SHIFT) & 0x1Fu;
}

#endif
