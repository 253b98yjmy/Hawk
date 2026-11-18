/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net/hk_cookies.h"
#include "base/hk_mem.h"
#include "net/hk_psl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PER_DOMAIN 50
#define MAX_TOTAL 3000
#define MAX_COOKIE_BYTES 4096

static char *dup_str(hk_str s)
{
    char *p = hk_alloc(s.n + 1);
    if (s.n) memcpy(p, s.p, s.n);
    p[s.n] = 0;
    return p;
}

static void free_str(char *p)
{
    if (p) hk_free(p, strlen(p) + 1);
}

static void free_cookie(hk_cookie *c)
{
    free_str(c->name);
    free_str(c->value);
    free_str(c->domain);
    free_str(c->path);
}

void hk_cookie_jar_init(hk_cookie_jar *j)
{
    memset(j, 0, sizeof *j);
}

void hk_cookie_jar_free(hk_cookie_jar *j)
{
    for (uint32_t i = 0; i < j->cookies.len; i++) free_cookie(&j->cookies.data[i]);
    hk_vec_free(&j->cookies);
}

static void remove_at(hk_cookie_jar *j, uint32_t i)
{
    free_cookie(&j->cookies.data[i]);
    j->cookies.data[i] = j->cookies.data[--j->cookies.len];
    j->dirty = true;
}

/* --- dates -------------------------------------------------------------------------------------------- */

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t hk_http_parse_date(hk_str s)
{
    /* RFC 6265 5.1.1: tokens separated by delimiters; find time, day, month, year. */
    static const char *const k_months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                           "jul", "aug", "sep", "oct", "nov", "dec"};
    int hour = -1, min = 0, sec = 0, day = -1, month = -1, year = -1;
    size_t i = 0;
    while (i < s.n) {
        while (i < s.n && !((s.p[i] >= '0' && s.p[i] <= '9') || (s.p[i] >= 'a' && s.p[i] <= 'z') ||
                            (s.p[i] >= 'A' && s.p[i] <= 'Z') || s.p[i] == ':'))
            i++;
        size_t st = i;
        while (i < s.n && ((s.p[i] >= '0' && s.p[i] <= '9') || (s.p[i] >= 'a' && s.p[i] <= 'z') ||
                           (s.p[i] >= 'A' && s.p[i] <= 'Z') || s.p[i] == ':'))
            i++;
        hk_str tok = hk_str_slice(s, st, i);
        if (!tok.n) continue;
        int h, mi, se;
        char buf[16];
        size_t n = tok.n < 15 ? tok.n : 15;
        memcpy(buf, tok.p, n);
        buf[n] = 0;
        if (hour < 0 && strchr(buf, ':') && sscanf_s(buf, "%d:%d:%d", &h, &mi, &se) == 3) {
            hour = h;
            min = mi;
            sec = se;
            continue;
        }
        if (month < 0 && tok.n >= 3) {
            for (int k = 0; k < 12; k++) {
                if (hk_str_eq_ascii_icase(hk_str_slice(tok, 0, 3), hk_str_from_cstr(k_months[k]))) month = k + 1;
            }
            if (month > 0) continue;
        }
        if (tok.p[0] >= '0' && tok.p[0] <= '9') {
            int v = atoi(buf);
            if (day < 0 && tok.n <= 2) day = v;
            else if (year < 0 && tok.n >= 2 && tok.n <= 4) year = v;
        }
    }
    if (year >= 70 && year <= 99) year += 1900;
    else if (year >= 0 && year <= 69) year += 2000;
    if (hour < 0 || day < 1 || day > 31 || month < 1 || year < 1601 || hour > 23 || min > 59 || sec > 59) return -1;
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
}

/* --- matching ----------------------------------------------------------------------------------------- */

static bool domain_match(hk_str host, const char *domain)
{
    hk_str d = hk_str_from_cstr(domain);
    if (hk_str_eq_ascii_icase(host, d)) return true;
    return host.n > d.n && host.p[host.n - d.n - 1] == '.' &&
           hk_str_eq_ascii_icase(hk_str_slice(host, host.n - d.n, host.n), d);
}

static bool path_match(hk_str req, const char *cookie_path)
{
    hk_str c = hk_str_from_cstr(cookie_path);
    if (hk_str_eq(req, c)) return true;
    if (!hk_str_starts_with(req, c)) return false;
    return (c.n && c.p[c.n - 1] == '/') || (req.n > c.n && req.p[c.n] == '/');
}

static hk_str default_path(const hk_url *u)
{
    hk_str p = u->path;
    if (!p.n || p.p[0] != '/') return HK_STR("/");
    size_t last = p.n;
    while (last > 0 && p.p[last - 1] != '/') last--;
    if (last <= 1) return HK_STR("/");
    return hk_str_slice(p, 0, last - 1);
}

/* --- Set-Cookie ---------------------------------------------------------------------------------------- */

void hk_cookie_set(hk_cookie_jar *j, const hk_url *url, hk_str site_host, hk_str header, int64_t now)
{
    if (header.n > MAX_COOKIE_BYTES) return;
    /* Third-party responses never set cookies. */
    if (!hk_psl_same_site(url->host, site_host)) return;
    size_t semi = hk_str_find_char(header, ';');
    hk_str pair = semi == HK_STR_NPOS ? header : hk_str_slice(header, 0, semi);
    size_t eq = hk_str_find_char(pair, '=');
    if (eq == HK_STR_NPOS) return;
    hk_str name = hk_str_trim_ascii_ws(hk_str_slice(pair, 0, eq));
    hk_str value = hk_str_trim_ascii_ws(hk_str_slice(pair, eq + 1, pair.n));
    if (!name.n) return;
    hk_cookie c = {0};
    c.created = now;
    c.host_only = true;
    hk_str domain = url->host, path = default_path(url);
    int64_t max_age = INT64_MIN, expires = INT64_MIN;
    hk_str rest = semi == HK_STR_NPOS ? HK_STR("") : hk_str_slice(header, semi + 1, header.n);
    while (rest.n) {
        size_t next = hk_str_find_char(rest, ';');
        hk_str av = next == HK_STR_NPOS ? rest : hk_str_slice(rest, 0, next);
        rest = next == HK_STR_NPOS ? HK_STR("") : hk_str_slice(rest, next + 1, rest.n);
        size_t e = hk_str_find_char(av, '=');
        hk_str key = hk_str_trim_ascii_ws(e == HK_STR_NPOS ? av : hk_str_slice(av, 0, e));
        hk_str val = e == HK_STR_NPOS ? HK_STR("") : hk_str_trim_ascii_ws(hk_str_slice(av, e + 1, av.n));
        if (hk_str_eq_ascii_icase(key, HK_STR("expires"))) {
            expires = hk_http_parse_date(val);
        } else if (hk_str_eq_ascii_icase(key, HK_STR("max-age"))) {
            char buf[24];
            size_t n = val.n < 23 ? val.n : 23;
            memcpy(buf, val.p, n);
            buf[n] = 0;
            if (n && ((buf[0] >= '0' && buf[0] <= '9') || buf[0] == '-')) max_age = _strtoi64(buf, NULL, 10);
        } else if (hk_str_eq_ascii_icase(key, HK_STR("domain"))) {
            if (val.n && val.p[0] == '.') val = hk_str_slice(val, 1, val.n);
            if (val.n) {
                domain = val;
                c.host_only = false;
            }
        } else if (hk_str_eq_ascii_icase(key, HK_STR("path"))) {
            if (val.n && val.p[0] == '/') path = val;
        } else if (hk_str_eq_ascii_icase(key, HK_STR("secure"))) {
            c.secure = true;
        } else if (hk_str_eq_ascii_icase(key, HK_STR("httponly"))) {
            c.http_only = true;
        } else if (hk_str_eq_ascii_icase(key, HK_STR("samesite"))) {
            c.same_site = hk_str_eq_ascii_icase(val, HK_STR("strict")) ? 1 : hk_str_eq_ascii_icase(val, HK_STR("none")) ? 2 : 0;
        }
    }
    if (!c.host_only) {
        /* The Domain attribute must cover the request host and must not be a public suffix. */
        char low[256];
        if (domain.n >= sizeof low) return;
        for (size_t i = 0; i < domain.n; i++) low[i] = hk_ascii_lower(domain.p[i]);
        low[domain.n] = 0;
        domain = (hk_str){low, domain.n};
        if (!domain_match(url->host, low) && !hk_str_eq_ascii_icase(url->host, domain)) return;
        if (hk_psl_registrable_domain(domain).n == 0) return;
        c.domain = dup_str(domain);
    } else {
        c.domain = dup_str(domain);
    }
    if (max_age != INT64_MIN) {
        c.persistent = true;
        c.expires = max_age <= 0 ? 0 : now + max_age;
    } else if (expires != INT64_MIN && expires >= 0) {
        c.persistent = true;
        c.expires = expires;
    }
    bool secure_origin = hk_str_eq(url->scheme, HK_STR("https"));
    if (c.secure && !secure_origin) {
        free_str(c.domain);
        return;
    }
    if (hk_str_starts_with(name, HK_STR("__Secure-")) && !c.secure) {
        free_str(c.domain);
        return;
    }
    if (hk_str_starts_with(name, HK_STR("__Host-")) && (!c.secure || !c.host_only || !hk_str_eq(path, HK_STR("/")))) {
        free_str(c.domain);
        return;
    }
    c.name = dup_str(name);
    c.value = dup_str(value);
    c.path = dup_str(path);

    /* Replace an existing cookie with the same name, domain and path. */
    uint32_t same_domain = 0;
    for (uint32_t i = 0; i < j->cookies.len; i++) {
        hk_cookie *o = &j->cookies.data[i];
        if (strcmp(o->domain, c.domain) == 0) {
            same_domain++;
            if (strcmp(o->name, c.name) == 0 && strcmp(o->path, c.path) == 0) {
                c.created = o->created;
                remove_at(j, i);
                same_domain--;
                break;
            }
        }
    }
    if (c.persistent && c.expires <= now) {
        free_cookie(&c); /* deletion */
        j->dirty = true;
        return;
    }
    if (same_domain >= MAX_PER_DOMAIN || j->cookies.len >= MAX_TOTAL) {
        /* Evict the oldest cookie (of this domain if it is over its limit). */
        uint32_t victim = 0;
        int64_t oldest = INT64_MAX;
        for (uint32_t i = 0; i < j->cookies.len; i++) {
            const hk_cookie *o = &j->cookies.data[i];
            if (same_domain >= MAX_PER_DOMAIN && strcmp(o->domain, c.domain) != 0) continue;
            if (o->created < oldest) {
                oldest = o->created;
                victim = i;
            }
        }
        if (j->cookies.len) remove_at(j, victim);
    }
    hk_vec_push(&j->cookies, c);
    if (c.persistent) j->dirty = true;
}

void hk_cookie_header(hk_cookie_jar *j, const hk_url *url, hk_str site_host, bool top_level, int64_t now,
                      hk_charbuf *out)
{
    out->len = 0;
    bool same_site = hk_psl_same_site(url->host, site_host);
    if (!same_site) return; /* no third-party cookies */
    bool secure = hk_str_eq(url->scheme, HK_STR("https"));
    hk_str path = url->path.n ? url->path : HK_STR("/");
    (void)top_level;
    for (uint32_t i = 0; i < j->cookies.len;) {
        hk_cookie *c = &j->cookies.data[i];
        if (c->persistent && c->expires <= now) {
            remove_at(j, i);
            continue;
        }
        i++;
        bool dm = c->host_only ? hk_str_eq_ascii_icase(url->host, hk_str_from_cstr(c->domain)) : domain_match(url->host, c->domain);
        if (!dm || !path_match(path, c->path) || (c->secure && !secure)) continue;
        if (out->len) hk_charbuf_append(out, "; ", 2);
        hk_charbuf_append(out, c->name, strlen(c->name));
        hk_charbuf_push(out, '=');
        hk_charbuf_append(out, c->value, strlen(c->value));
    }
}

/* --- persistence --------------------------------------------------------------------------------------- */

void hk_cookie_jar_save(const hk_cookie_jar *j, hk_charbuf *out, int64_t now)
{
    char head[96];
    for (uint32_t i = 0; i < j->cookies.len; i++) {
        const hk_cookie *c = &j->cookies.data[i];
        if (!c->persistent || c->expires <= now) continue;
        int n = snprintf(head, sizeof head, "%lld\t%lld\t%d%d%d%d\t", (long long)c->expires, (long long)c->created,
                         c->host_only, c->secure, c->http_only, c->same_site);
        hk_charbuf_append(out, head, (size_t)n);
        const char *fields[] = {c->domain, c->path, c->name, c->value};
        for (int k = 0; k < 4; k++) {
            hk_charbuf_append(out, fields[k], strlen(fields[k]));
            hk_charbuf_push(out, k < 3 ? '\t' : '\n');
        }
    }
}

void hk_cookie_jar_load(hk_cookie_jar *j, hk_str text, int64_t now)
{
    size_t pos = 0;
    while (pos < text.n) {
        size_t eol = pos;
        while (eol < text.n && text.p[eol] != '\n') eol++;
        hk_str line = hk_str_slice(text, pos, eol);
        pos = eol + 1;
        hk_str f[7];
        int nf = 0;
        size_t st = 0;
        for (size_t i = 0; i <= line.n && nf < 7; i++) {
            if (i == line.n || line.p[i] == '\t') {
                f[nf++] = hk_str_slice(line, st, i);
                st = i + 1;
            }
        }
        if (nf != 7 || f[2].n != 4) continue;
        char num[24];
        hk_cookie c = {0};
        size_t n = f[0].n < 23 ? f[0].n : 23;
        memcpy(num, f[0].p, n);
        num[n] = 0;
        c.expires = _strtoi64(num, NULL, 10);
        n = f[1].n < 23 ? f[1].n : 23;
        memcpy(num, f[1].p, n);
        num[n] = 0;
        c.created = _strtoi64(num, NULL, 10);
        if (c.expires <= now) continue;
        c.host_only = f[2].p[0] == '1';
        c.secure = f[2].p[1] == '1';
        c.http_only = f[2].p[2] == '1';
        c.same_site = (uint8_t)(f[2].p[3] - '0');
        c.persistent = true;
        c.domain = dup_str(f[3]);
        c.path = dup_str(f[4]);
        c.name = dup_str(f[5]);
        c.value = dup_str(f[6]);
        hk_vec_push(&j->cookies, c);
    }
}
