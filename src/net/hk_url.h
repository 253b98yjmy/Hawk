/* SPDX-License-Identifier: GPL-3.0-or-later */
/* URL parsing and serialization (WHATWG URL Standard). */
#ifndef HK_URL_H
#define HK_URL_H

#include "base/hk_arena.h"
#include "base/hk_common.h"
#include "base/hk_str.h"

typedef struct hk_url {
    hk_str scheme; /* lowercase, without ":" */
    hk_str username;
    hk_str password;
    bool has_host;
    hk_str host; /* serialized: domain, dotted IPv4, or "[...]" IPv6; may be empty */
    int32_t port; /* -1 = none (also when it equals the scheme's default) */
    bool opaque_path;
    hk_str path; /* serialized path ("/a/b", or the opaque path string) */
    bool has_query;
    hk_str query;
    bool has_fragment;
    hk_str fragment;
    hk_str href; /* the full serialization */
} hk_url;

/*
 * Parses input against an optional base URL. All strings of *out are allocated in the arena.
 * Returns HK_ERR_INVALID when the input is not a valid URL.
 */
hk_status hk_url_parse(hk_arena *a, hk_str input, const hk_url *base, hk_url *out);

bool hk_url_is_special(const hk_url *u);
/* Default port of a special scheme, or -1. */
int32_t hk_url_default_port(hk_str scheme);
/* The port to connect to: explicit port or the scheme default. */
int32_t hk_url_effective_port(const hk_url *u);

/* Path + query, as sent in an HTTP request line ("/a?b"). */
hk_str hk_url_request_target(hk_arena *a, const hk_url *u);

#endif
