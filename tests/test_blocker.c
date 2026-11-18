/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Content blocker: regular expressions, filter syntax, options, $removeparam and the image. */
#include "hk_test.h"
#include "blocker/hk_blocker.h"
#include "blocker/hk_regex.h"

#include <stdio.h>
#include <string.h>

static bool re_search(const char *pattern, const char *text)
{
    hk_regex *re = hk_regex_compile(hk_str_from_cstr(pattern), false);
    if (!re) return false;
    bool m = hk_regex_search(re, hk_str_from_cstr(text));
    hk_regex_free(re);
    return m;
}

static bool re_valid(const char *pattern)
{
    hk_regex *re = hk_regex_compile(hk_str_from_cstr(pattern), false);
    hk_regex_free(re);
    return re != NULL;
}

HK_TEST(regex_engine)
{
    HK_CHECK(re_search("abc", "xxabcxx"));
    HK_CHECK(!re_search("abd", "xxabcxx"));
    HK_CHECK(re_search("^https?://", "http://a"));
    HK_CHECK(!re_search("^b", "ab"));
    HK_CHECK(re_search("c$", "abc"));
    HK_CHECK(!re_search("b$", "abc"));
    HK_CHECK(re_search("a.c", "abc"));
    HK_CHECK(re_search("[0-9]{3}x", "ab123x"));
    HK_CHECK(!re_search("^[0-9]{3}$", "1234"));
    HK_CHECK(re_search("^[0-9]{2,3}$", "123"));
    HK_CHECK(re_search("^a{2,}$", "aaaa"));
    HK_CHECK(!re_search("^a{2,}$", "a"));
    HK_CHECK(re_search("^(ab|cd)+$", "abcdab"));
    HK_CHECK(!re_search("^(ab|cd)+$", "abce"));
    HK_CHECK(re_search("^(?:x|y)?z$", "z"));
    HK_CHECK(re_search("\\d+\\.\\w+", "v12.js"));
    HK_CHECK(re_search("[^a-z]", "abc1"));
    HK_CHECK(!re_search("[^a-z]", "abc"));
    HK_CHECK(re_search("\\bads\\b", "/ads/x"));
    HK_CHECK(!re_search("\\bads\\b", "/loads/x"));
    HK_CHECK(re_search("a|", "zzz")); /* an empty alternative matches everywhere */
    HK_CHECK(re_search("^\\/ads\\/[a-z]+\\.js", "/ads/foo.js"));
    {
        hk_regex *re = hk_regex_compile(HK_STR("ABC"), true);
        HK_CHECK(re && hk_regex_search(re, HK_STR("xabcx")));
        size_t s = 0, e = 0;
        HK_CHECK(re && hk_regex_find(re, HK_STR("xxABc"), &s, &e) && s == 2 && e == 5);
        hk_regex_free(re);
    }
    /* Unsupported syntax is rejected rather than misread. */
    HK_CHECK(!re_valid("(a)\\1"));
    HK_CHECK(!re_valid("a(?=b)"));
    HK_CHECK(!re_valid("a(?<=b)"));
    HK_CHECK(!re_valid("(ab"));
    HK_CHECK(!re_valid("ab)"));
    HK_CHECK(!re_valid("*a"));
    HK_CHECK(!re_valid("a{99}"));
    HK_CHECK(!re_valid("[b-a]"));
    /* Linear time: this would take forever with backtracking. */
    char text[4001];
    memset(text, 'a', 4000);
    text[4000] = 0;
    HK_CHECK(!re_search("^(a|a)*(a|a)*(a|a)*b$", text));
    HK_CHECK(!re_search("(a*)*b", text));
}

typedef struct fixture {
    hk_blocker *b;
    hk_filter_stats st;
} fixture;

static void load(fixture *f, const char *list)
{
    f->b = hk_blocker_from_text(hk_str_from_cstr(list), &f->st);
}

static hk_str host_of(const char *url)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, ":/?#");
    return (hk_str){p, n};
}

/* site: the top-level page ("" for a navigation). */
static bool blocked(const fixture *f, const char *url, const char *site, uint32_t type)
{
    hk_block_request r = {hk_str_from_cstr(url), host_of(url), hk_str_from_cstr(site), host_of(site), type};
    return hk_blocker_match(f->b, &r).blocked;
}

#define PAGE "https://www.news.example/"

HK_TEST(blocker_hosts)
{
    fixture f;
    load(&f, "! comment\n"
             "[Adblock Plus 2.0]\n"
             "||ads.example.com^\n"
             "@@||ok.ads.example.com^\n"
             "||Tracker.Example.NET^\r\n"
             "0.0.0.0 pixel.test # hosts file\n"
             "127.0.0.1 localhost\n"
             "##.ad-banner\n"
             "example.com##.sponsor\n");
    HK_CHECK(f.b != NULL);
    HK_CHECK_EQ(f.st.network, 4);
    HK_CHECK_EQ(f.st.hosts, 4);
    HK_CHECK_EQ(f.st.cosmetic, 2);
    HK_CHECK(blocked(&f, "https://ads.example.com/x.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://cdn.ads.example.com/", PAGE, HK_REQ_OTHER));
    HK_CHECK(blocked(&f, "https://ads.example.com/", "", HK_REQ_DOCUMENT)); /* strict blocking of pages */
    HK_CHECK(!blocked(&f, "https://example.com/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://badads.example.com/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://ok.ads.example.com/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://tracker.example.net/p", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://pixel.test/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://localhost/", PAGE, HK_REQ_IMAGE));
    hk_blocker_close(f.b);
}

HK_TEST(blocker_patterns)
{
    fixture f;
    load(&f, "/banner/*/img^\n"
             "|https://start.example/\n"
             "swf|\n"
             "||cdn.example^*/ads/\n"
             "/ad[0-9]{2}\\.gif/\n"
             "&ad_type=\n"
             "-ad-300x250.\n"
             "||case.example/Big$match-case\n"
             "/\\/X\\D\\D\\.png/\n" /* the case of escapes matters */
             "*\n");
    HK_CHECK(f.b != NULL);
    HK_CHECK_EQ(f.st.network, 9); /* "*" alone would block everything */
    HK_CHECK_EQ(f.st.regex, 2);
    HK_CHECK(blocked(&f, "https://a.example/xab.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://a.example/x12.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://a.example/banner/foo/img?x", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://a.example/banner/foo/img", PAGE, HK_REQ_IMAGE)); /* ^ matches the end */
    HK_CHECK(!blocked(&f, "https://a.example/banner/foo/imgs", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://start.example/x", PAGE, HK_REQ_OTHER));
    HK_CHECK(!blocked(&f, "https://a.example/?u=https://start.example/", PAGE, HK_REQ_OTHER));
    HK_CHECK(blocked(&f, "https://a.example/movie.swf", PAGE, HK_REQ_OTHER));
    HK_CHECK(!blocked(&f, "https://a.example/movie.swf?x", PAGE, HK_REQ_OTHER));
    HK_CHECK(blocked(&f, "https://img.cdn.example/x/ads/1.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://cdn.example.org/x/ads/1.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://xcdn.example/ads/1.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://a.example/AD12.gif", PAGE, HK_REQ_IMAGE)); /* regexes ignore case */
    HK_CHECK(!blocked(&f, "https://a.example/ad1.gif", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://a.example/q?x=1&ad_type=2", PAGE, HK_REQ_OTHER));
    HK_CHECK(blocked(&f, "https://a.example/img-ad-300x250.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://case.example/Big.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://case.example/big.png", PAGE, HK_REQ_IMAGE));
    /* Patterns without explicit types do not block pages; host-name filters do. */
    HK_CHECK(!blocked(&f, "https://a.example/banner/x/img", "", HK_REQ_DOCUMENT));
    hk_blocker_close(f.b);
}

HK_TEST(blocker_options)
{
    fixture f;
    load(&f, "||third.example^$third-party\n"
             "||first.example^$1p\n"
             "||img.example^$image\n"
             "||notimg.example^$~image\n"
             "||doc.example^$document\n"
             "||script.example^$script\n"            /* never requested: dropped */
             "||mixed.example^$script,image\n"       /* the image part is kept */
             "||csp.example^$csp=default-src 'none'\n" /* unsupported: dropped */
             "||dom.example^$domain=news.example|~sub.news.example\n"
             "||ent.example^$domain=shop.*\n"
             "/track.gif$to=t1.example|t2.example\n"
             "||deny.example^$denyallow=ok.example,domain=news.example\n"
             "||imp.example^$important\n"
             "@@||imp.example^\n"
             "||exc.example^\n"
             "@@||exc.example/allowed/\n"
             "||bad.example^$image\n"
             "||bad.example^$image,badfilter\n"
             "||allowpage.example^\n"
             "@@||trusted.example^$document\n"
             "||redir.example/ads.js$script,redirect=noop.js\n"
             "||redir.example/pixel.png$image,redirect=1x1.gif\n"
             "||elem.example^$elemhide\n"
             "||dup.example^\n"
             "||dup.example^\n");
    HK_CHECK(f.b != NULL);
    HK_CHECK_EQ(f.st.duplicate, 1);
    HK_CHECK_EQ(f.st.badfiltered, 1);
    HK_CHECK_EQ(f.st.cosmetic, 1);
    HK_CHECK_EQ(f.st.unsupported, 3);
    /* party */
    HK_CHECK(blocked(&f, "https://third.example/a", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://third.example/a", "https://www.third.example/", HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://third.example/", "", HK_REQ_DOCUMENT));
    HK_CHECK(blocked(&f, "https://first.example/a", "https://www.first.example/", HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://first.example/a", PAGE, HK_REQ_IMAGE));
    /* types */
    HK_CHECK(blocked(&f, "https://img.example/a", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://img.example/a", PAGE, HK_REQ_STYLESHEET));
    HK_CHECK(!blocked(&f, "https://notimg.example/a", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://notimg.example/a", PAGE, HK_REQ_FONT));
    HK_CHECK(blocked(&f, "https://doc.example/", "", HK_REQ_DOCUMENT));
    HK_CHECK(!blocked(&f, "https://doc.example/a.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://script.example/a", PAGE, HK_REQ_OTHER));
    HK_CHECK(blocked(&f, "https://mixed.example/a", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://mixed.example/a", PAGE, HK_REQ_OTHER));
    HK_CHECK(!blocked(&f, "https://csp.example/a", PAGE, HK_REQ_OTHER));
    /* domain=, entities, to=, denyallow= */
    HK_CHECK(blocked(&f, "https://dom.example/a", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://dom.example/a", "https://sub.news.example/", HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://dom.example/a", "https://other.example/", HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://ent.example/a", "https://www.shop.co.jp/", HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://ent.example/a", "https://shop.com/", HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://ent.example/a", "https://shopping.com/", HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://t1.example/track.gif", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://t3.example/track.gif", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://deny.example/a", PAGE, HK_REQ_IMAGE));
    /* important beats exceptions; exceptions beat ordinary filters */
    HK_CHECK(blocked(&f, "https://imp.example/a", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://exc.example/x", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://exc.example/allowed/x", PAGE, HK_REQ_IMAGE));
    /* badfilter */
    HK_CHECK(!blocked(&f, "https://bad.example/a", PAGE, HK_REQ_IMAGE));
    /* @@...$document allows everything on that page */
    HK_CHECK(blocked(&f, "https://allowpage.example/a", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://allowpage.example/a", "https://trusted.example/", HK_REQ_IMAGE));
    /* redirect= blocks */
    HK_CHECK(blocked(&f, "https://redir.example/pixel.png", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://elem.example/a", PAGE, HK_REQ_IMAGE));
    /* where a decision came from */
    hk_block_request r = {HK_STR("https://exc.example/x"), HK_STR("exc.example"), HK_STR(PAGE), HK_STR("www.news.example"),
                          HK_REQ_IMAGE};
    hk_block_result res = hk_blocker_match(f.b, &r);
    HK_CHECK_STR(hk_blocker_filter_list(f.b, res.filter), "text");
    HK_CHECK_EQ(hk_blocker_filter_line(f.b, res.filter), 15);
    hk_blocker_close(f.b);
}

static const char *strip(const fixture *f, const char *url, char *buf, size_t cap)
{
    hk_block_request r = {hk_str_from_cstr(url), host_of(url), HK_STR(PAGE), HK_STR("www.news.example"), HK_REQ_OTHER};
    hk_charbuf out = {0};
    if (hk_blocker_strip_params(f->b, &r, &out)) snprintf(buf, cap, "%.*s", (int)out.len, out.data);
    else snprintf(buf, cap, "%s", url);
    hk_vec_free(&out);
    return buf;
}

HK_TEST(blocker_removeparam)
{
    fixture f;
    load(&f, "$removeparam=utm_source\n"
             "$removeparam=/^utm_/\n"
             "||strip.example^$removeparam\n"
             "||keep.example^$removeparam=~id\n"
             "@@||exempt.example^$removeparam=utm_source\n"
             "@@||exempt.example^$removeparam=/^utm_/\n");
    HK_CHECK(f.b != NULL);
    char b[256];
    HK_CHECK_STR(hk_str_from_cstr(strip(&f, "https://a.example/p?utm_source=x&id=1", b, sizeof b)),
                 "https://a.example/p?id=1");
    HK_CHECK_STR(hk_str_from_cstr(strip(&f, "https://a.example/p?id=1&utm_medium=y#top", b, sizeof b)),
                 "https://a.example/p?id=1#top");
    HK_CHECK_STR(hk_str_from_cstr(strip(&f, "https://a.example/p?utm_source=x", b, sizeof b)), "https://a.example/p");
    HK_CHECK_STR(hk_str_from_cstr(strip(&f, "https://a.example/p?id=1", b, sizeof b)), "https://a.example/p?id=1");
    HK_CHECK_STR(hk_str_from_cstr(strip(&f, "https://strip.example/p?a=1&b=2", b, sizeof b)), "https://strip.example/p");
    HK_CHECK_STR(hk_str_from_cstr(strip(&f, "https://keep.example/p?a=1&id=2&b", b, sizeof b)),
                 "https://keep.example/p?id=2");
    HK_CHECK_STR(hk_str_from_cstr(strip(&f, "https://exempt.example/p?utm_source=x", b, sizeof b)),
                 "https://exempt.example/p?utm_source=x");
    /* $removeparam filters never block */
    HK_CHECK(!blocked(&f, "https://strip.example/p?a=1", PAGE, HK_REQ_OTHER));
    hk_blocker_close(f.b);
}

HK_TEST(blocker_image)
{
    /* !#if, several lists, and the image round trip including corruption checks. */
    hk_filter_compiler *c = hk_filter_compiler_create();
    hk_filter_compiler_add_list(c, HK_STR("one"), HK_STR("||a.example^\n"
                                                         "!#if env_windows\n"
                                                         "||win.example^\n"
                                                         "!#else\n"
                                                         "||notwin.example^\n"
                                                         "!#endif\n"
                                                         "!#if env_mobile || (cap_html_filtering && !false)\n"
                                                         "||mobile.example^\n"
                                                         "!#endif\n"));
    hk_filter_compiler_add_list(c, HK_STR("two"), HK_STR("||b.example^\n||a.example^$badfilter\n"));
    hk_charbuf img = {0};
    hk_filter_stats st;
    hk_filter_compiler_finish(c, 42, &img, &st);
    HK_CHECK_EQ(hk_blocker_image_stamp(img.data, img.len), 42);
    hk_blocker *b = hk_blocker_open(img.data, img.len);
    HK_CHECK(b != NULL);
    fixture f = {b, st};
    HK_CHECK(!blocked(&f, "https://a.example/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://b.example/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(blocked(&f, "https://win.example/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://notwin.example/", PAGE, HK_REQ_IMAGE));
    HK_CHECK(!blocked(&f, "https://mobile.example/", PAGE, HK_REQ_IMAGE));
    hk_block_request r = {HK_STR("https://b.example/"), HK_STR("b.example"), HK_STR(PAGE), HK_STR("www.news.example"),
                          HK_REQ_IMAGE};
    HK_CHECK_STR(hk_blocker_filter_list(b, hk_blocker_match(b, &r).filter), "two");
    hk_blocker_close(b);

    /* A truncated or damaged image is refused. */
    HK_CHECK(hk_blocker_open(img.data, img.len / 2) == NULL);
    img.data[0] ^= 1;
    HK_CHECK(hk_blocker_open(img.data, img.len) == NULL);
    img.data[0] ^= 1;
    uint32_t *sec = (uint32_t *)(img.data + 24); /* the first section's offset */
    *sec += 4;
    HK_CHECK(hk_blocker_open(img.data, img.len) == NULL);
    hk_vec_free(&img);
}
