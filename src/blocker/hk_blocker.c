/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "blocker/hk_blocker.h"

#include <string.h>

void hk_blocker_init(hk_blocker *b)
{
    memset(b, 0, sizeof *b);
    hk_arena_init(&b->arena, 0);
    hk_map64_init(&b->block);
    hk_map64_init(&b->allow);
}

void hk_blocker_free(hk_blocker *b)
{
    hk_map64_free(&b->block);
    hk_map64_free(&b->allow);
    hk_vec_free(&b->names);
    hk_arena_free(&b->arena);
}

static bool host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

static bool add_host(hk_blocker *b, hk_map64 *set, hk_str host)
{
    if (host.n == 0 || host.n > 253) return false;
    char low[256];
    for (size_t i = 0; i < host.n; i++) {
        low[i] = hk_ascii_lower(host.p[i]);
        if (!host_char(low[i])) return false;
    }
    hk_str h = {low, host.n};
    uint64_t key = hk_key64_nonzero(hk_str_hash64(h));
    if (hk_map64_get(set, key, NULL)) return true;
    hk_vec_push(&b->names, hk_arena_str_dup(&b->arena, h));
    hk_map64_put(set, key, b->names.len);
    return true;
}

/* "||host^" with nothing after it except an optional "$" option list we cannot honour yet. */
static bool parse_host_filter(hk_str line, hk_str *host)
{
    if (!hk_str_starts_with(line, HK_STR("||"))) return false;
    hk_str rest = hk_str_slice(line, 2, line.n);
    size_t caret = hk_str_find_char(rest, '^');
    if (caret == HK_STR_NPOS || caret + 1 != rest.n) return false; /* options or a path: later */
    *host = hk_str_slice(rest, 0, caret);
    return true;
}

uint32_t hk_blocker_add_list(hk_blocker *b, hk_str text)
{
    uint32_t added = 0;
    size_t pos = 0;
    while (pos < text.n) {
        size_t eol = pos;
        while (eol < text.n && text.p[eol] != '\n') eol++;
        hk_str line = hk_str_trim_ascii_ws(hk_str_slice(text, pos, eol));
        pos = eol + 1;
        if (line.n == 0 || line.p[0] == '!' || line.p[0] == '[' || line.p[0] == '#') continue;

        hk_str host;
        if (hk_str_starts_with(line, HK_STR("@@")) && parse_host_filter(hk_str_slice(line, 2, line.n), &host)) {
            added += add_host(b, &b->allow, host);
            continue;
        }
        if (parse_host_filter(line, &host)) {
            if (add_host(b, &b->block, host)) added++;
            else b->ignored++;
            continue;
        }
        /* hosts file: "0.0.0.0 example.com" / "127.0.0.1 example.com" */
        if (hk_str_starts_with(line, HK_STR("0.0.0.0 ")) || hk_str_starts_with(line, HK_STR("127.0.0.1 "))) {
            size_t sp = hk_str_find_char(line, ' ');
            hk_str h = hk_str_trim_ascii_ws(hk_str_slice(line, sp + 1, line.n));
            size_t end = 0;
            while (end < h.n && h.p[end] != ' ' && h.p[end] != '\t' && h.p[end] != '#') end++;
            h = hk_str_slice(h, 0, end);
            if (!hk_str_eq(h, HK_STR("localhost")) && add_host(b, &b->block, h)) added++;
            continue;
        }
        b->ignored++;
    }
    return added;
}

static bool set_has(const hk_blocker *b, const hk_map64 *set, hk_str host)
{
    uint64_t v;
    if (!hk_map64_get(set, hk_key64_nonzero(hk_str_hash64(host)), &v)) return false;
    return hk_str_eq(b->names.data[v - 1], host); /* rule out hash collisions */
}

bool hk_blocker_blocks_host(const hk_blocker *b, hk_str host)
{
    if (host.n && host.p[host.n - 1] == '.') host.n--;
    bool blocked = false;
    for (hk_str h = host; h.n;) {
        if (set_has(b, &b->allow, h)) return false;
        if (!blocked && set_has(b, &b->block, h)) blocked = true;
        size_t dot = hk_str_find_char(h, '.');
        if (dot == HK_STR_NPOS) break;
        h = hk_str_slice(h, dot + 1, h.n);
    }
    return blocked;
}

uint32_t hk_blocker_count(const hk_blocker *b)
{
    return b->block.len;
}
