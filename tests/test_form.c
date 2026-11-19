/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Forms: control kinds, form owners, labels, state, editing and submission. */
#include "hk_test.h"
#include "form/hk_form.h"
#include "html/hk_html_parser.h"

#include <string.h>

#define A(x) HK_ATOM_##x

#define HK_CHECK_BUF(buf, lit) HK_CHECK(strcmp((buf), (lit)) == 0)

static hk_document *parse(const char *html)
{
    return hk_html_parse(html, strlen(html));
}

static hk_node_id by_id(const hk_document *d, const char *id)
{
    for (hk_node_id n = 1; n < d->nodes.len; n++) {
        if (d->nodes.data[n].type != HK_NODE_ELEMENT) continue;
        const hk_attr *a = hk_dom_attr(d, n, HK_ATTR_NS_NONE, A(id));
        if (a && hk_str_eq(a->value, hk_str_from_cstr(id))) return n;
    }
    return HK_NODE_NONE;
}

static hk_url base_url(hk_arena *a, const char *s)
{
    hk_url u = {0};
    hk_url_parse(a, hk_str_from_cstr(s), NULL, &u);
    return u;
}

/* Submits the form with id "f" (submitter by id, may be NULL); returns the URL or body as a C string in buf. */
static bool submit(hk_forms *f, const hk_document *d, const char *submitter, hk_encoding enc, bool *post, char *buf,
                   size_t n)
{
    hk_arena a;
    hk_arena_init(&a, 0);
    hk_url base = base_url(&a, "https://example.com/dir/page.html?x=1#frag");
    hk_form_submission s;
    bool ok = hk_forms_submit(f, by_id(d, "f"), submitter ? by_id(d, submitter) : HK_NODE_NONE, &base, enc, &s);
    if (ok) {
        *post = s.post;
        const hk_charbuf *out = s.post ? &s.body : &s.url;
        size_t len = out->len < n - 1 ? out->len : n - 1;
        memcpy(buf, out->data ? out->data : "", len);
        buf[len] = 0;
        hk_form_submission_free(&s);
    }
    hk_arena_free(&a);
    return ok;
}

HK_TEST(form_kinds_and_owners)
{
    hk_document *d = parse("<table><form id=f action=/s><tr><td><input id=a name=a></td></tr></form></table>"
                           "<form id=g><input id=b type=CheckBox><button id=c>go</button>"
                           "<button id=r type=reset></button><select id=s></select><textarea id=t></textarea>"
                           "<input id=h type=hidden><input id=p type=password><input id=x type=color></form>"
                           "<input id=o form=f><label id=l1 for=b>B</label><label id=l2>T <input id=in></label>"
                           "<fieldset disabled><legend><input id=e1></legend><input id=e2></fieldset>");
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "a")), HK_CTRL_TEXT);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "b")), HK_CTRL_CHECKBOX);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "c")), HK_CTRL_SUBMIT);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "r")), HK_CTRL_RESET);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "s")), HK_CTRL_SELECT);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "t")), HK_CTRL_TEXTAREA);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "h")), HK_CTRL_HIDDEN);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "p")), HK_CTRL_PASSWORD);
    HK_CHECK_EQ(hk_control_kind_of(d, by_id(d, "x")), HK_CTRL_TEXT); /* unsupported types are text fields */
    /* The parser's form element pointer: the input in the table belongs to the form. */
    HK_CHECK_EQ(hk_form_owner(d, by_id(d, "a")), by_id(d, "f"));
    HK_CHECK_EQ(hk_form_owner(d, by_id(d, "b")), by_id(d, "g"));
    HK_CHECK_EQ(hk_form_owner(d, by_id(d, "o")), by_id(d, "f")); /* form attribute */
    HK_CHECK_EQ(hk_form_owner(d, by_id(d, "in")), HK_NODE_NONE);
    HK_CHECK_EQ(hk_label_control(d, by_id(d, "l1")), by_id(d, "b"));
    HK_CHECK_EQ(hk_label_control(d, by_id(d, "l2")), by_id(d, "in"));
    HK_CHECK(!hk_control_disabled(d, by_id(d, "e1")));
    HK_CHECK(hk_control_disabled(d, by_id(d, "e2")));
    hk_document_destroy(d);
}

HK_TEST(form_state)
{
    hk_document *d = parse("<form id=f><input id=t value='a&#10;b'><textarea id=ta>\nline1\nline2</textarea>"
                           "<input type=radio name=r id=r1 checked><input type=radio name=r id=r2 checked>"
                           "<input type=radio name=r id=r3><input type=checkbox id=c checked>"
                           "<select id=s><option disabled>x<option>y<option>z</select>"
                           "<select id=s2><option>1<option selected>2<option selected>3</select></form>");
    hk_forms *f = hk_forms_create(d);
    HK_CHECK(hk_str_eq(hk_forms_value(f, by_id(d, "t")), HK_STR("ab"))); /* newlines are stripped */
    HK_CHECK(hk_str_eq(hk_forms_value(f, by_id(d, "ta")), HK_STR("line1\nline2")));
    HK_CHECK(!hk_forms_checked(f, by_id(d, "r1"))); /* the last checked radio of a group wins */
    HK_CHECK(hk_forms_checked(f, by_id(d, "r2")));
    hk_forms_set_checked(f, by_id(d, "r3"), true);
    HK_CHECK(!hk_forms_checked(f, by_id(d, "r2")));
    HK_CHECK(hk_forms_checked(f, by_id(d, "c")));
    HK_CHECK_EQ(hk_forms_selected(f, by_id(d, "s")), 1); /* the first enabled option */
    HK_CHECK_EQ(hk_forms_selected(f, by_id(d, "s2")), 2);
    hk_forms_select(f, by_id(d, "s"), 2);
    HK_CHECK_EQ(hk_forms_selected(f, by_id(d, "s")), 2);
    hk_forms_set_value(f, by_id(d, "t"), HK_STR("changed"));
    hk_forms_set_checked(f, by_id(d, "c"), false);
    hk_forms_reset(f, by_id(d, "f"));
    HK_CHECK(hk_str_eq(hk_forms_value(f, by_id(d, "t")), HK_STR("ab")));
    HK_CHECK(hk_forms_checked(f, by_id(d, "c")));
    HK_CHECK(hk_forms_checked(f, by_id(d, "r2")));
    HK_CHECK_EQ(hk_forms_selected(f, by_id(d, "s")), 1);
    hk_forms_destroy(f);
    hk_document_destroy(d);
}

HK_TEST(form_editing)
{
    hk_document *d = parse("<input id=t><input id=m maxlength=4><input id=ro readonly value=fixed>"
                           "<textarea id=ta></textarea><input id=pw type=password value=secret>");
    hk_forms *f = hk_forms_create(d);
    hk_node_id tx = by_id(d, "t");
    HK_CHECK(hk_forms_insert(f, tx, HK_STR("hello world")));
    HK_CHECK(hk_str_eq(hk_forms_value(f, tx), HK_STR("hello world")));
    hk_forms_edit(f, tx, HK_EDIT_WORD_LEFT, false);
    uint32_t caret, anchor;
    hk_forms_selection(f, tx, &caret, &anchor);
    HK_CHECK_EQ(caret, 6u);
    hk_forms_edit(f, tx, HK_EDIT_WORD_LEFT, true);
    HK_CHECK(hk_str_eq(hk_forms_selected_text(f, tx), HK_STR("hello ")));
    HK_CHECK(hk_forms_insert(f, tx, HK_STR("日本語の")));
    HK_CHECK(hk_str_eq(hk_forms_value(f, tx), HK_STR("日本語のworld")));
    hk_forms_edit(f, tx, HK_EDIT_HOME, false);
    hk_forms_edit(f, tx, HK_EDIT_RIGHT, false); /* one code point: 3 bytes */
    hk_forms_selection(f, tx, &caret, &anchor);
    HK_CHECK_EQ(caret, 3u);
    HK_CHECK(hk_forms_edit(f, tx, HK_EDIT_BACKSPACE, false));
    HK_CHECK(hk_str_eq(hk_forms_value(f, tx), HK_STR("本語のworld")));
    hk_forms_edit(f, tx, HK_EDIT_END, false);
    HK_CHECK(hk_forms_edit(f, tx, HK_EDIT_WORD_BACKSPACE, false));
    HK_CHECK(hk_str_eq(hk_forms_value(f, tx), HK_STR("本語の")));
    HK_CHECK(!hk_forms_edit(f, tx, HK_EDIT_DELETE, false)); /* at the end */
    hk_forms_set_caret(f, tx, 4, false);                  /* inside a character: moved back */
    hk_forms_selection(f, tx, &caret, &anchor);
    HK_CHECK_EQ(caret, 3u);
    hk_forms_edit(f, tx, HK_EDIT_SELECT_ALL, false);
    HK_CHECK(hk_forms_insert(f, tx, HK_STR("a\r\nb\nc")));
    HK_CHECK(hk_str_eq(hk_forms_value(f, tx), HK_STR("abc"))); /* single line */

    hk_node_id m = by_id(d, "m");
    HK_CHECK(hk_forms_insert(f, m, HK_STR("abcdef")));
    HK_CHECK(hk_str_eq(hk_forms_value(f, m), HK_STR("abcd")));
    HK_CHECK(!hk_forms_insert(f, m, HK_STR("x")));
    hk_forms_edit(f, m, HK_EDIT_SELECT_ALL, false);
    HK_CHECK(hk_forms_insert(f, m, HK_STR("😀😀😀"))); /* two UTF-16 units each */
    HK_CHECK(hk_str_eq(hk_forms_value(f, m), HK_STR("😀😀")));

    HK_CHECK(!hk_forms_insert(f, by_id(d, "ro"), HK_STR("x")));
    HK_CHECK(!hk_forms_edit(f, by_id(d, "ro"), HK_EDIT_BACKSPACE, false));

    hk_node_id ta = by_id(d, "ta");
    HK_CHECK(hk_forms_insert(f, ta, HK_STR("one\r\ntwo")));
    HK_CHECK(hk_str_eq(hk_forms_value(f, ta), HK_STR("one\ntwo")));
    hk_forms_edit(f, ta, HK_EDIT_HOME, false);
    hk_forms_selection(f, ta, &caret, &anchor);
    HK_CHECK_EQ(caret, 4u); /* the start of the second line */

    hk_node_id pw = by_id(d, "pw");
    hk_forms_edit(f, pw, HK_EDIT_SELECT_ALL, false);
    HK_CHECK_EQ(hk_forms_selected_text(f, pw).n, 0u); /* never copied */
    hk_forms_destroy(f);
    hk_document_destroy(d);
}

HK_TEST(form_submission)
{
    hk_document *d = parse("<form id=f action='search?old=1#x'>"
                           "<input name=q value='a b&c=d'><input name=empty><input value=noname>"
                           "<input type=checkbox name=c1 checked><input type=checkbox name=c2 value=v checked>"
                           "<input type=checkbox name=c3><input type=radio name=r value=1><input type=radio name=r value=2 checked>"
                           "<select name=s><option value=x>X<option selected> Y  Z </select>"
                           "<select name=m multiple><option selected>1<option>2<option selected>3</select>"
                           "<textarea name=t>l1\nl2</textarea><input type=hidden name=_charset_>"
                           "<input name=dis disabled value=1><input type=submit name=go value=Go id=go>"
                           "<input type=submit name=other value=Other id=other><button name=btn value=b id=btn>B</button>"
                           "<input type=image name=img id=img></form>");
    hk_forms *f = hk_forms_create(d);
    char buf[1024];
    bool post = true;
    HK_CHECK(submit(f, d, "go", HK_ENC_UTF8, &post, buf, sizeof buf));
    HK_CHECK(!post);
    HK_CHECK_BUF(buf, "https://example.com/dir/search?q=a+b%26c%3Dd&empty=&c1=on&c2=v&r=2&s=Y+Z&m=1&m=3"
                      "&t=l1%0D%0Al2&_charset_=UTF-8&go=Go#x");
    HK_CHECK(submit(f, d, "btn", HK_ENC_UTF8, &post, buf, sizeof buf));
    HK_CHECK(strstr(buf, "&btn=b") != NULL && strstr(buf, "go=Go") == NULL);
    HK_CHECK(submit(f, d, "img", HK_ENC_UTF8, &post, buf, sizeof buf));
    HK_CHECK(strstr(buf, "&img.x=0&img.y=0") != NULL);
    HK_CHECK(submit(f, d, NULL, HK_ENC_UTF8, &post, buf, sizeof buf));
    HK_CHECK(strstr(buf, "go=") == NULL && strstr(buf, "other=") == NULL);
    hk_forms_destroy(f);
    hk_document_destroy(d);

    /* POST keeps the action's query; the page's encoding is used, with character references for the rest. */
    d = parse("<form id=f method=POST action='/p?k=v'><input name=q value='日本 &#x1F600;'></form>");
    f = hk_forms_create(d);
    HK_CHECK(submit(f, d, NULL, HK_ENC_SHIFT_JIS, &post, buf, sizeof buf));
    HK_CHECK(post);
    HK_CHECK_BUF(buf, "q=%93%FA%96%7B+%26%23128512%3B");
    hk_forms_destroy(f);
    hk_document_destroy(d);

    /* Empty action: the document URL; accept-charset wins over the page's encoding. */
    d = parse("<form id=f accept-charset='bogus utf-8'><input name=q value='é'></form>");
    f = hk_forms_create(d);
    HK_CHECK(submit(f, d, NULL, HK_ENC_WINDOWS_1252, &post, buf, sizeof buf));
    HK_CHECK_BUF(buf, "https://example.com/dir/page.html?q=%C3%A9#frag");
    hk_forms_destroy(f);
    hk_document_destroy(d);

    /* Unsupported schemes and method=dialog are not submitted. */
    d = parse("<form id=f action='mailto:a@example.com'><input name=q></form>");
    f = hk_forms_create(d);
    HK_CHECK(!submit(f, d, NULL, HK_ENC_UTF8, &post, buf, sizeof buf));
    hk_forms_destroy(f);
    hk_document_destroy(d);
}
