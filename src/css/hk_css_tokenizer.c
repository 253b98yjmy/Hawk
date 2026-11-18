/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CSS Syntax Level 3, section 4 (tokenization). Input is UTF-8; non-ASCII bytes are name code points. */
#include "css/hk_css.h"
#include "base/hk_utf8.h"

#include <math.h>
#include <string.h>

typedef struct lexer {
    hk_arena *a;
    const char *p;
    size_t n, i;
    hk_charbuf tmp;
} lexer;

#define AT(k) ((l->i + (k)) < l->n ? (int)(unsigned char)l->p[l->i + (k)] : -1)

static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}
static bool is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hexv(int c)
{
    return is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
}
static bool is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n';
}
static bool is_name_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80 || c == '_';
}
static bool is_name(int c)
{
    return is_name_start(c) || is_digit(c) || c == '-';
}
static bool valid_escape(int c0, int c1)
{
    return c0 == '\\' && c1 != '\n' && c1 != -1;
}
static bool starts_ident(int c0, int c1, int c2)
{
    if (c0 == '-') return is_name_start(c1) || c1 == '-' || valid_escape(c1, c2);
    if (is_name_start(c0)) return true;
    return valid_escape(c0, c1);
}
static bool starts_number(int c0, int c1, int c2)
{
    if (c0 == '+' || c0 == '-') return is_digit(c1) || (c1 == '.' && is_digit(c2));
    if (c0 == '.') return is_digit(c1);
    return is_digit(c0);
}

/* Consumes an escape (the backslash already consumed) and appends its UTF-8. */
static void consume_escape(lexer *l, hk_charbuf *out)
{
    int c = AT(0);
    if (c == -1) {
        hk_charbuf_append(out, "\xEF\xBF\xBD", 3);
        return;
    }
    if (is_hex(c)) {
        uint32_t v = 0;
        for (int k = 0; k < 6 && is_hex(AT(0)); k++) {
            v = v * 16 + (uint32_t)hexv(AT(0));
            l->i++;
        }
        if (is_ws(AT(0))) l->i++;
        if (v == 0 || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) v = 0xFFFD;
        uint8_t tmp[4];
        size_t k = hk_utf8_encode(v, tmp);
        hk_charbuf_append(out, (const char *)tmp, k);
        return;
    }
    /* Copy one whole UTF-8 code point. */
    size_t len = 1;
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    if (l->i + len > l->n) len = l->n - l->i;
    hk_charbuf_append(out, l->p + l->i, len);
    l->i += len;
}

/* Consumes a name; returns a slice of the source when it had no escapes. */
static hk_str consume_name(lexer *l)
{
    size_t start = l->i;
    bool escaped = false;
    l->tmp.len = 0;
    for (;;) {
        int c = AT(0);
        if (is_name(c)) {
            hk_charbuf_push(&l->tmp, (char)c);
            l->i++;
        } else if (valid_escape(c, AT(1))) {
            l->i++;
            consume_escape(l, &l->tmp);
            escaped = true;
        } else {
            break;
        }
    }
    if (!escaped) return (hk_str){l->p + start, l->i - start};
    return hk_arena_str_dup(l->a, (hk_str){l->tmp.data, l->tmp.len});
}

static double consume_number(lexer *l, bool *integer)
{
    size_t start = l->i;
    *integer = true;
    if (AT(0) == '+' || AT(0) == '-') l->i++;
    while (is_digit(AT(0))) l->i++;
    if (AT(0) == '.' && is_digit(AT(1))) {
        *integer = false;
        l->i++;
        while (is_digit(AT(0))) l->i++;
    }
    if ((AT(0) == 'e' || AT(0) == 'E') && (is_digit(AT(1)) || ((AT(1) == '+' || AT(1) == '-') && is_digit(AT(2))))) {
        *integer = false;
        l->i += 2;
        while (is_digit(AT(0))) l->i++;
    }
    /* Manual conversion: strtod depends on the C locale. */
    const char *s = l->p + start, *e = l->p + l->i;
    double sign = 1, ip = 0, fp = 0, scale = 1;
    int exp = 0, esign = 1;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    while (s < e && is_digit(*s)) ip = ip * 10 + (*s++ - '0');
    if (s < e && *s == '.') {
        s++;
        while (s < e && is_digit(*s)) {
            fp = fp * 10 + (*s++ - '0');
            scale *= 10;
        }
    }
    if (s < e && (*s == 'e' || *s == 'E')) {
        s++;
        if (*s == '+' || *s == '-') esign = *s++ == '-' ? -1 : 1;
        while (s < e && is_digit(*s) && exp < 10000) exp = exp * 10 + (*s++ - '0');
    }
    return sign * (ip + fp / scale) * pow(10, esign * exp);
}

static void consume_bad_url_remnants(lexer *l)
{
    for (;;) {
        int c = AT(0);
        if (c == -1) return;
        l->i++;
        if (c == ')') return;
        if (valid_escape(c, AT(0))) {
            hk_charbuf scratch = {0};
            consume_escape(l, &scratch);
            hk_vec_free(&scratch);
        }
    }
}

static void consume_url(lexer *l, hk_css_token *t)
{
    l->tmp.len = 0;
    while (is_ws(AT(0))) l->i++;
    for (;;) {
        int c = AT(0);
        if (c == ')' || c == -1) {
            if (c == ')') l->i++;
            break;
        }
        if (is_ws(c)) {
            while (is_ws(AT(0))) l->i++;
            if (AT(0) == ')' || AT(0) == -1) {
                if (AT(0) == ')') l->i++;
                break;
            }
            consume_bad_url_remnants(l);
            t->type = HK_CT_BAD_URL;
            return;
        }
        if (c == '"' || c == '\'' || c == '(' || (c < 0x20 && c != '\t') || c == 0x7F) {
            consume_bad_url_remnants(l);
            t->type = HK_CT_BAD_URL;
            return;
        }
        if (c == '\\') {
            if (valid_escape(c, AT(1))) {
                l->i++;
                consume_escape(l, &l->tmp);
                continue;
            }
            consume_bad_url_remnants(l);
            t->type = HK_CT_BAD_URL;
            return;
        }
        hk_charbuf_push(&l->tmp, (char)c);
        l->i++;
    }
    t->type = HK_CT_URL;
    t->value = hk_arena_str_dup(l->a, (hk_str){l->tmp.data, l->tmp.len});
}

static void consume_string(lexer *l, hk_css_token *t, int quote)
{
    l->tmp.len = 0;
    t->type = HK_CT_STRING;
    for (;;) {
        int c = AT(0);
        if (c == -1) break;
        if (c == quote) {
            l->i++;
            break;
        }
        if (c == '\n') {
            t->type = HK_CT_BAD_STRING;
            break;
        }
        l->i++;
        if (c == '\\') {
            if (AT(0) == -1) continue;
            if (AT(0) == '\n') {
                l->i++;
                continue;
            }
            consume_escape(l, &l->tmp);
            continue;
        }
        hk_charbuf_push(&l->tmp, (char)c);
    }
    t->value = hk_arena_str_dup(l->a, (hk_str){l->tmp.data, l->tmp.len});
}

static void consume_ident_like(lexer *l, hk_css_token *t)
{
    hk_str name = consume_name(l);
    if (hk_str_eq_ascii_icase(name, HK_STR("url")) && AT(0) == '(') {
        l->i++;
        size_t save = l->i;
        while (is_ws(AT(0)) && is_ws(AT(1))) l->i++;
        int c0 = AT(0), c1 = AT(1);
        if (c0 == '"' || c0 == '\'' || (is_ws(c0) && (c1 == '"' || c1 == '\''))) {
            t->type = HK_CT_FUNCTION;
            t->value = name;
            return;
        }
        l->i = save;
        consume_url(l, t);
        return;
    }
    if (AT(0) == '(') {
        l->i++;
        t->type = HK_CT_FUNCTION;
        t->value = name;
        return;
    }
    t->type = HK_CT_IDENT;
    t->value = name;
}

static void consume_numeric(lexer *l, hk_css_token *t)
{
    bool integer;
    t->num = consume_number(l, &integer);
    if (integer) t->flags |= HK_CSS_INTEGER;
    if (starts_ident(AT(0), AT(1), AT(2))) {
        t->type = HK_CT_DIMENSION;
        t->unit = consume_name(l);
    } else if (AT(0) == '%') {
        l->i++;
        t->type = HK_CT_PERCENT;
    } else {
        t->type = HK_CT_NUMBER;
    }
}

static void consume_token(lexer *l, hk_css_token *t)
{
    memset(t, 0, sizeof *t);
    /* Comments. */
    while (AT(0) == '/' && AT(1) == '*') {
        l->i += 2;
        while (AT(0) != -1 && !(AT(0) == '*' && AT(1) == '/')) l->i++;
        if (AT(0) != -1) l->i += 2;
    }
    int c = AT(0);
    if (c == -1) {
        t->type = HK_CT_EOF;
        return;
    }
    if (is_ws(c)) {
        while (is_ws(AT(0))) l->i++;
        t->type = HK_CT_WS;
        return;
    }
    if (c == '"' || c == '\'') {
        l->i++;
        consume_string(l, t, c);
        return;
    }
    if (c == '#') {
        if (is_name(AT(1)) || valid_escape(AT(1), AT(2))) {
            l->i++;
            if (starts_ident(AT(0), AT(1), AT(2))) t->flags |= HK_CSS_HASH_ID;
            t->type = HK_CT_HASH;
            t->value = consume_name(l);
            return;
        }
    }
    if (starts_number(c, AT(1), AT(2))) {
        consume_numeric(l, t);
        return;
    }
    if (c == '-' && AT(1) == '-' && AT(2) == '>') {
        l->i += 3;
        t->type = HK_CT_CDC;
        return;
    }
    if (starts_ident(c, AT(1), AT(2))) {
        consume_ident_like(l, t);
        return;
    }
    if (c == '<' && AT(1) == '!' && AT(2) == '-' && AT(3) == '-') {
        l->i += 4;
        t->type = HK_CT_CDO;
        return;
    }
    if (c == '@' && starts_ident(AT(1), AT(2), AT(3))) {
        l->i++;
        t->type = HK_CT_AT;
        t->value = consume_name(l);
        return;
    }
    l->i++;
    switch (c) {
    case '(': t->type = HK_CT_LPAREN; return;
    case ')': t->type = HK_CT_RPAREN; return;
    case '[': t->type = HK_CT_LBRACKET; return;
    case ']': t->type = HK_CT_RBRACKET; return;
    case '{': t->type = HK_CT_LBRACE; return;
    case '}': t->type = HK_CT_RBRACE; return;
    case ',': t->type = HK_CT_COMMA; return;
    case ':': t->type = HK_CT_COLON; return;
    case ';': t->type = HK_CT_SEMI; return;
    default: break;
    }
    t->type = HK_CT_DELIM;
    if (c < 0x80) {
        t->delim = (uint32_t)c;
    } else {
        uint32_t cp;
        l->i--;
        l->i += hk_utf8_decode((const uint8_t *)l->p + l->i, l->n - l->i, &cp);
        t->delim = cp;
    }
}

hk_css_tokens hk_css_tokenize(hk_arena *a, hk_str src)
{
    /* Input preprocessing: CRLF, CR and FF become LF; U+0000 becomes U+FFFD. */
    hk_charbuf pre = {0};
    hk_vec_reserve(&pre, (uint64_t)src.n + 1);
    for (size_t i = 0; i < src.n; i++) {
        char c = src.p[i];
        if (c == '\r') {
            hk_charbuf_push(&pre, '\n');
            if (i + 1 < src.n && src.p[i + 1] == '\n') i++;
        } else if (c == '\f') {
            hk_charbuf_push(&pre, '\n');
        } else if (c == '\0') {
            hk_charbuf_append(&pre, "\xEF\xBF\xBD", 3);
        } else {
            hk_charbuf_push(&pre, c);
        }
    }
    hk_str text = hk_arena_str_dup(a, (hk_str){pre.data, pre.len});
    hk_vec_free(&pre);

    lexer l = {a, text.p, text.n, 0, {0}};
    HK_VEC(hk_css_token) out = {0};
    for (;;) {
        hk_css_token t;
        consume_token(&l, &t);
        hk_vec_push(&out, t);
        if (t.type == HK_CT_EOF) break;
    }
    hk_vec_free(&l.tmp);
    hk_css_token *arr = HK_ARENA_NEW_ARRAY(a, hk_css_token, out.len);
    memcpy(arr, out.data, out.len * sizeof *arr);
    hk_css_tokens r = {arr, out.len};
    hk_vec_free(&out);
    return r;
}

bool hk_css_ident_is(const hk_css_token *t, const char *s)
{
    return t->type == HK_CT_IDENT && hk_str_eq_ascii_icase(t->value, hk_str_from_cstr(s));
}
