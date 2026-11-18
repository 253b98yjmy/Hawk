/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests for the Public Suffix List, cookies and HTTP cache freshness. */
#include "hk_test.h"
#include "net/hk_cache.h"
#include "net/hk_cookies.h"
#include "net/hk_psl.h"

#include <string.h>

HK_TEST(psl_registrable)
{
    HK_CHECK_STR(hk_psl_registrable_domain(HK_STR("www.example.co.jp")), "example.co.jp");
    HK_CHECK_STR(hk_psl_registrable_domain(HK_STR("example.com")), "example.com");
    HK_CHECK_STR(hk_psl_registrable_domain(HK_STR("a.b.example.com.")), "example.com");
    HK_CHECK_STR(hk_psl_registrable_domain(HK_STR("co.jp")), "");
    HK_CHECK_STR(hk_psl_public_suffix(HK_STR("www.example.co.jp")), "co.jp");
    /* Wildcard and exception rules: *.kawasaki.jp, !city.kawasaki.jp */
    HK_CHECK_STR(hk_psl_registrable_domain(HK_STR("x.foo.kawasaki.jp")), "x.foo.kawasaki.jp");
    HK_CHECK_STR(hk_psl_registrable_domain(HK_STR("a.city.kawasaki.jp")), "city.kawasaki.jp");
    HK_CHECK_STR(hk_psl_registrable_domain(HK_STR("192.168.0.1")), "192.168.0.1");
    HK_CHECK(hk_psl_same_site(HK_STR("a.example.com"), HK_STR("b.example.com")));
    HK_CHECK(!hk_psl_same_site(HK_STR("example.com"), HK_STR("example.org")));
    HK_CHECK(!hk_psl_same_site(HK_STR("a.github.io"), HK_STR("b.github.io"))); /* github.io is a public suffix */
}

HK_TEST(http_dates)
{
    HK_CHECK_EQ(hk_http_parse_date(HK_STR("Sun, 06 Nov 1994 08:49:37 GMT")), 784111777);
    HK_CHECK_EQ(hk_http_parse_date(HK_STR("Sunday, 06-Nov-94 08:49:37 GMT")), 784111777);
    HK_CHECK_EQ(hk_http_parse_date(HK_STR("Sun Nov  6 08:49:37 1994")), 784111777);
    HK_CHECK(hk_http_parse_date(HK_STR("not a date")) < 0);
}

static void parse_url(hk_arena *a, const char *s, hk_url *u)
{
    hk_url_parse(a, hk_str_from_cstr(s), NULL, u);
}

HK_TEST(cookie_jar)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_cookie_jar j;
    hk_cookie_jar_init(&j);
    hk_url page, api, other;
    parse_url(&a, "https://www.example.com/dir/page.html", &page);
    parse_url(&a, "https://api.example.com/v1/x", &api);
    parse_url(&a, "https://tracker.example.net/p", &other);
    int64_t now = 1700000000;
    hk_str site = HK_STR("www.example.com");
    hk_cookie_set(&j, &page, site, HK_STR("a=1"), now);
    hk_cookie_set(&j, &page, site, HK_STR("b=2; Domain=example.com; Path=/; Max-Age=3600; Secure"), now);
    hk_cookie_set(&j, &page, site, HK_STR("c=3; Domain=com"), now);                /* public suffix: rejected */
    hk_cookie_set(&j, &page, site, HK_STR("d=4; Domain=other.org"), now);          /* not covering the host */
    hk_cookie_set(&j, &other, site, HK_STR("t=1; SameSite=None; Secure"), now);    /* third party: ignored */
    hk_cookie_set(&j, &page, site, HK_STR("__Host-x=5; Path=/; Secure"), now);
    hk_cookie_set(&j, &page, site, HK_STR("__Host-y=6; Domain=example.com; Path=/; Secure"), now); /* invalid */
    HK_CHECK_EQ(j.cookies.len, 3);

    hk_charbuf h = {0};
    hk_cookie_header(&j, &page, site, true, now, &h);
    HK_CHECK_STR(((hk_str){h.data, h.len}), "a=1; b=2; __Host-x=5");
    hk_cookie_header(&j, &api, site, false, now, &h);
    HK_CHECK_STR(((hk_str){h.data, h.len}), "b=2"); /* domain cookie only; a=1 is host-only for /dir */
    hk_cookie_header(&j, &other, site, false, now, &h);
    HK_CHECK_EQ(h.len, 0); /* no third-party cookies */
    hk_cookie_header(&j, &page, site, true, now + 7200, &h);
    HK_CHECK_STR(((hk_str){h.data, h.len}), "a=1; __Host-x=5"); /* b expired */

    /* Persistence keeps only persistent cookies. */
    hk_cookie_set(&j, &page, site, HK_STR("p=7; Max-Age=600"), now + 7200);
    hk_charbuf saved = {0};
    hk_cookie_jar_save(&j, &saved, now + 7200);
    hk_cookie_jar k;
    hk_cookie_jar_init(&k);
    hk_cookie_jar_load(&k, (hk_str){saved.data, saved.len}, now + 7200);
    HK_CHECK_EQ(k.cookies.len, 1);
    hk_cookie_header(&k, &page, site, true, now + 7200, &h);
    HK_CHECK_STR(((hk_str){h.data, h.len}), "p=7");

    hk_vec_free(&h);
    hk_vec_free(&saved);
    hk_cookie_jar_free(&k);
    hk_cookie_jar_free(&j);
    hk_arena_free(&a);
}

static int64_t lifetime(const char *name1, const char *v1, const char *name2, const char *v2)
{
    hk_http_response r = {0};
    r.status = 200;
    hk_http_header h1 = {hk_str_from_cstr(name1), hk_str_from_cstr(v1)};
    hk_vec_push(&r.headers, h1);
    if (name2) {
        hk_http_header h2 = {hk_str_from_cstr(name2), hk_str_from_cstr(v2)};
        hk_vec_push(&r.headers, h2);
    }
    int64_t l = hk_cache_lifetime(&r, 784111777);
    hk_vec_free(&r.headers);
    return l;
}

HK_TEST(cache_lifetime)
{
    HK_CHECK_EQ(lifetime("Cache-Control", "public, max-age=600", NULL, NULL), 600);
    HK_CHECK_EQ(lifetime("Cache-Control", "no-store", NULL, NULL), (uint64_t)-1);
    HK_CHECK_EQ(lifetime("Cache-Control", "no-cache", NULL, NULL), 0);
    HK_CHECK_EQ(lifetime("Expires", "Sun, 06 Nov 1994 09:49:37 GMT", "Date", "Sun, 06 Nov 1994 08:49:37 GMT"), 3600);
    HK_CHECK_EQ(lifetime("Vary", "Cookie", NULL, NULL), (uint64_t)-1);
    HK_CHECK_EQ(lifetime("Last-Modified", "Sun, 06 Nov 1994 07:49:37 GMT", NULL, NULL), 360); /* 10% heuristic */
}
