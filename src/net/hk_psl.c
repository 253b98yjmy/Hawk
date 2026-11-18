/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The algorithm of https://publicsuffix.org/list/ ("Formal Algorithm") over a hash table of the rules. */
#include "net/hk_psl.h"

#include <string.h>

#include "net/hk_psl_data.inc"

/* FNV-1a, as in tools/gen_psl.py; a prefix ("!" or "*.") is hashed first. */
static uint64_t rule_hash(const char *prefix, hk_str s)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (const char *p = prefix; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001B3ull;
    for (size_t i = 0; i < s.n; i++) h = (h ^ (uint8_t)s.p[i]) * 0x100000001B3ull;
    return h ? h : 1;
}

static bool rule_with_prefix(const char *prefix, hk_str s)
{
    uint64_t h = rule_hash(prefix, s);
    for (uint32_t i = (uint32_t)h & (HK_PSL_CAP - 1);; i = (i + 1) & (HK_PSL_CAP - 1)) {
        if (k_psl_table[i] == h) return true;
        if (!k_psl_table[i]) return false;
    }
}

static bool has_rule(hk_str s)
{
    return rule_with_prefix("", s);
}

static bool is_ip(hk_str h)
{
    if (h.n && h.p[0] == '[') return true;
    for (size_t i = 0; i < h.n; i++) {
        if (!((h.p[i] >= '0' && h.p[i] <= '9') || h.p[i] == '.')) return false;
    }
    return h.n > 0;
}

hk_str hk_psl_public_suffix(hk_str host)
{
    if (host.n && host.p[host.n - 1] == '.') host.n--;
    if (is_ip(host)) return host;
    /* Walk the suffixes from the longest: the first (longest) matching rule decides. */
    hk_str best = {0};
    bool found = false;
    for (size_t start = 0; start <= host.n;) {
        hk_str suffix = hk_str_slice(host, start, host.n);
        size_t dot = hk_str_find_char(suffix, '.');
        hk_str parent = dot == HK_STR_NPOS ? HK_STR("") : hk_str_slice(suffix, dot + 1, suffix.n);
        if (rule_with_prefix("!", suffix)) {
            /* Exception: the public suffix is the rule minus its leftmost label. */
            best = parent;
            found = true;
            break;
        }
        if (has_rule(suffix) || (parent.n && rule_with_prefix("*.", parent))) {
            best = suffix;
            found = true;
            break;
        }
        if (dot == HK_STR_NPOS) break;
        start += dot + 1;
    }
    if (!found) {
        /* The default rule "*": the last label. */
        size_t i = host.n;
        while (i > 0 && host.p[i - 1] != '.') i--;
        best = hk_str_slice(host, i, host.n);
    }
    return best;
}

hk_str hk_psl_registrable_domain(hk_str host)
{
    if (host.n && host.p[host.n - 1] == '.') host.n--;
    if (is_ip(host)) return host;
    hk_str suffix = hk_psl_public_suffix(host);
    if (suffix.n >= host.n) return HK_STR("");
    size_t end = host.n - suffix.n - 1; /* the dot before the suffix */
    size_t i = end;
    while (i > 0 && host.p[i - 1] != '.') i--;
    return hk_str_slice(host, i, host.n);
}

bool hk_psl_same_site(hk_str a, hk_str b)
{
    hk_str ra = hk_psl_registrable_domain(a), rb = hk_psl_registrable_domain(b);
    if (!ra.n || !rb.n) return hk_str_eq_ascii_icase(a, b);
    return hk_str_eq_ascii_icase(ra, rb);
}
