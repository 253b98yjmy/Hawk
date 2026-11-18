/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net/hk_cache.h"
#include "base/hk_mem.h"
#include "net/hk_cookies.h"
#include "plat/hk_plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRY_BYTES (8u * 1024 * 1024)
#define HEURISTIC_MAX (24 * 3600)

struct hk_cache {
    char dir[512];
    uint64_t max_bytes;
};

hk_cache *hk_cache_open(const char *dir, uint64_t max_bytes)
{
    if (!dir) return NULL;
    hk_cache *c = hk_alloc_zero(sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    c->max_bytes = max_bytes;
    hk_plat_make_dir(dir);
    hk_plat_trim_dir(dir, max_bytes, max_bytes / 10 * 8);
    return c;
}

void hk_cache_close(hk_cache *c)
{
    if (c) hk_free(c, sizeof *c);
}

static void entry_path(const hk_cache *c, hk_str site, hk_str url, char *out, size_t n)
{
    uint64_t h = hk_hash64(site.p, site.n);
    h ^= hk_hash64(url.p, url.n) * 0x9E3779B97F4A7C15ull;
    snprintf(out, n, "%s\\%016llx.hkc", c->dir, (unsigned long long)h);
}

static hk_str field(hk_str head, const char *name)
{
    hk_str key = hk_str_from_cstr(name);
    size_t pos = 0;
    while (pos < head.n) {
        size_t eol = pos;
        while (eol < head.n && head.p[eol] != '\n') eol++;
        hk_str line = hk_str_slice(head, pos, eol);
        pos = eol + 1;
        if (line.n > key.n && line.p[key.n] == ' ' && hk_str_starts_with(line, key)) return hk_str_slice(line, key.n + 1, line.n);
    }
    return HK_STR("");
}

static int64_t to_i64(hk_str s)
{
    char buf[24];
    size_t n = s.n < 23 ? s.n : 23;
    memcpy(buf, s.p, n);
    buf[n] = 0;
    return _strtoi64(buf, NULL, 10);
}

static void copy_field(char *dst, size_t n, hk_str v)
{
    size_t k = v.n < n - 1 ? v.n : n - 1;
    memcpy(dst, v.p, k);
    dst[k] = 0;
}

bool hk_cache_lookup(hk_cache *c, hk_str site, hk_str url, hk_cache_entry *out)
{
    memset(out, 0, sizeof *out);
    if (!c) return false;
    char path[600];
    entry_path(c, site, url, path, sizeof path);
    hk_charbuf data = {0};
    if (hk_plat_read_file(path, &data) != HK_OK) return false;
    hk_str all = {data.data, data.len};
    size_t sep = hk_str_find(all, HK_STR("\n\n"));
    bool ok = false;
    if (sep != HK_STR_NPOS && hk_str_starts_with(all, HK_STR("HKC1\n"))) {
        hk_str head = hk_str_slice(all, 5, sep + 1);
        if (hk_str_eq(field(head, "site"), site) && hk_str_eq(field(head, "url"), url)) {
            out->status = (int)to_i64(field(head, "status"));
            out->stored = to_i64(field(head, "stored"));
            out->fresh_until = to_i64(field(head, "fresh"));
            copy_field(out->content_type, sizeof out->content_type, field(head, "type"));
            copy_field(out->etag, sizeof out->etag, field(head, "etag"));
            copy_field(out->last_modified, sizeof out->last_modified, field(head, "modified"));
            hk_charbuf_append(&out->body, all.p + sep + 2, all.n - sep - 2);
            ok = true;
        }
    }
    hk_vec_free(&data);
    return ok;
}

void hk_cache_entry_free(hk_cache_entry *e)
{
    hk_vec_free(&e->body);
}

static int64_t directive(hk_str cc, const char *name, bool *present)
{
    hk_str key = hk_str_from_cstr(name);
    *present = false;
    size_t i = 0;
    while (i < cc.n) {
        while (i < cc.n && (cc.p[i] == ',' || hk_ascii_is_ws(cc.p[i]))) i++;
        size_t st = i;
        while (i < cc.n && cc.p[i] != ',') i++;
        hk_str d = hk_str_trim_ascii_ws(hk_str_slice(cc, st, i));
        size_t eq = hk_str_find_char(d, '=');
        hk_str dn = hk_str_trim_ascii_ws(eq == HK_STR_NPOS ? d : hk_str_slice(d, 0, eq));
        if (hk_str_eq_ascii_icase(dn, key)) {
            *present = true;
            if (eq == HK_STR_NPOS) return 0;
            hk_str v = hk_str_trim_ascii_ws(hk_str_slice(d, eq + 1, d.n));
            if (v.n && v.p[0] == '"') v = hk_str_slice(v, 1, v.n - 1);
            return to_i64(v);
        }
    }
    return 0;
}

int64_t hk_cache_lifetime(const hk_http_response *r, int64_t now)
{
    hk_str cc = hk_http_header_get(r, "Cache-Control", NULL);
    bool p;
    directive(cc, "no-store", &p);
    if (p) return -1;
    bool has_vary;
    hk_str vary = hk_http_header_get(r, "Vary", &has_vary);
    if (has_vary && !hk_str_eq_ascii_icase(hk_str_trim_ascii_ws(vary), HK_STR("accept-encoding"))) return -1;
    directive(cc, "no-cache", &p);
    if (p) return 0;
    int64_t age = directive(cc, "max-age", &p);
    if (p) return age < 0 ? 0 : age;
    bool has_expires;
    hk_str expires = hk_http_header_get(r, "Expires", &has_expires);
    if (has_expires) {
        int64_t t = hk_http_parse_date(expires);
        bool has_date;
        hk_str date_h = hk_http_header_get(r, "Date", &has_date);
        int64_t date = has_date ? hk_http_parse_date(date_h) : now;
        if (date < 0) date = now;
        return t < 0 ? 0 : (t > date ? t - date : 0);
    }
    bool has_lm;
    hk_str lm = hk_http_header_get(r, "Last-Modified", &has_lm);
    if (has_lm) {
        /* Heuristic freshness: 10% of the time since the last modification. */
        int64_t t = hk_http_parse_date(lm);
        if (t > 0 && t < now) {
            int64_t h = (now - t) / 10;
            return h > HEURISTIC_MAX ? HEURISTIC_MAX : h;
        }
    }
    return 0;
}

static void write_entry(hk_cache *c, hk_str site, hk_str url, int status, hk_str type, hk_str etag, hk_str lm,
                        int64_t stored, int64_t fresh, const uint8_t *body, size_t n)
{
    char path[600];
    entry_path(c, site, url, path, sizeof path);
    hk_charbuf out = {0};
    char num[160];
    hk_charbuf_append(&out, "HKC1\nsite ", 10);
    hk_charbuf_append(&out, site.p, site.n);
    hk_charbuf_append(&out, "\nurl ", 5);
    hk_charbuf_append(&out, url.p, url.n);
    int k = snprintf(num, sizeof num, "\nstatus %d\nstored %lld\nfresh %lld\ntype ", status, (long long)stored,
                     (long long)fresh);
    hk_charbuf_append(&out, num, (size_t)k);
    hk_charbuf_append(&out, type.p, type.n < 120 ? type.n : 120);
    hk_charbuf_append(&out, "\netag ", 6);
    hk_charbuf_append(&out, etag.p, etag.n < 120 ? etag.n : 120);
    hk_charbuf_append(&out, "\nmodified ", 10);
    hk_charbuf_append(&out, lm.p, lm.n < 60 ? lm.n : 60);
    hk_charbuf_append(&out, "\n\n", 2);
    hk_charbuf_append(&out, (const char *)body, n);
    hk_plat_write_file_atomic(path, out.data, out.len);
    hk_vec_free(&out);
}

static bool clean_header(hk_str v)
{
    for (size_t i = 0; i < v.n; i++) {
        if (v.p[i] == '\n' || v.p[i] == '\r') return false;
    }
    return true;
}

void hk_cache_store(hk_cache *c, hk_str site, hk_str url, const hk_http_response *r, const uint8_t *body, size_t n,
                    int64_t now)
{
    if (!c || r->status != 200 || n > MAX_ENTRY_BYTES) return;
    int64_t life = hk_cache_lifetime(r, now);
    if (life < 0) return;
    hk_str etag = hk_http_header_get(r, "ETag", NULL), lm = hk_http_header_get(r, "Last-Modified", NULL);
    /* Without freshness or validators the entry would never be usable. */
    if (life == 0 && !etag.n && !lm.n) return;
    hk_str type = hk_http_header_get(r, "Content-Type", NULL);
    if (!clean_header(type) || !clean_header(etag) || !clean_header(lm) || !clean_header(url) || !clean_header(site))
        return;
    write_entry(c, site, url, 200, type, etag, lm, now, now + life, body, n);
}

void hk_cache_refresh(hk_cache *c, hk_str site, hk_str url, const hk_http_response *r, const hk_cache_entry *e,
                      int64_t now)
{
    if (!c) return;
    int64_t life = hk_cache_lifetime(r, now);
    if (life < 0) return;
    write_entry(c, site, url, e->status, hk_str_from_cstr(e->content_type), hk_str_from_cstr(e->etag),
                hk_str_from_cstr(e->last_modified), now, now + life, (const uint8_t *)e->body.data, e->body.len);
}
