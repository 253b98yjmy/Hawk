/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The algorithm of https://publicsuffix.org/list/ ("Formal Algorithm") over a sorted rule array. */
#include "net/hk_psl.h"

#include <string.h>

#include "net/hk_psl_data.inc"

static int cmp_rule(hk_str s, const char *rule)
{
    size_t n = strlen(rule);
    size_t m = s.n < n ? s.n : n;
    int r = memcmp(s.p, rule, m);
    if (r) return r;
    return s.n < n ? -1 : s.n > n;
}

static bool has_rule(hk_str s)
{
    size_t lo = 0, hi = HK_PSL_RULES;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp_rule(s, k_psl_rules[mid]);
        if (r == 0) return true;
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

static bool rule_with_prefix(const char *prefix, hk_str s)
{
    char buf[300];
    size_t pn = strlen(prefix);
    if (s.n + pn >= sizeof buf) return false;
    memcpy(buf, prefix, pn);
    memcpy(buf + pn, s.p, s.n);
    return has_rule((hk_str){buf, pn + s.n});
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
