/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The basic URL parser of the WHATWG URL Standard (section 4.4), without state
 * overrides (Hawk never runs setters). Input is UTF-8; all delimiters are ASCII,
 * so the parser walks bytes and percent-encodes non-ASCII bytes directly.
 */
#include "net/hk_url.h"
#include "base/hk_vec.h"
#include "plat/hk_plat.h"

#include <stdio.h>
#include <string.h>

#define EOF_CH (-1)

typedef HK_VEC(hk_str) seg_list;

typedef enum {
    S_SCHEME_START,
    S_SCHEME,
    S_NO_SCHEME,
    S_SPECIAL_RELATIVE_OR_AUTHORITY,
    S_PATH_OR_AUTHORITY,
    S_RELATIVE,
    S_RELATIVE_SLASH,
    S_SPECIAL_AUTHORITY_SLASHES,
    S_SPECIAL_AUTHORITY_IGNORE_SLASHES,
    S_AUTHORITY,
    S_HOST,
    S_PORT,
    S_FILE,
    S_FILE_SLASH,
    S_FILE_HOST,
    S_PATH_START,
    S_PATH,
    S_OPAQUE_PATH,
    S_QUERY,
    S_FRAGMENT,
} state;

/* --- character sets ---------------------------------------------------------------------- */

static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}
static bool is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hex_val(int c)
{
    return is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10;
}

typedef enum { SET_C0, SET_FRAGMENT, SET_QUERY, SET_SPECIAL_QUERY, SET_PATH, SET_USERINFO } pct_set;

static bool in_set(int c, pct_set set)
{
    if (c < 0x20 || c > 0x7E) return true;
    if (set == SET_C0) return false;
    if (set == SET_FRAGMENT) return c == ' ' || c == '"' || c == '<' || c == '>' || c == '`';
    if (c == ' ' || c == '"' || c == '#' || c == '<' || c == '>') return true;
    if (set == SET_QUERY) return false;
    if (set == SET_SPECIAL_QUERY) return c == '\'';
    if (c == '?' || c == '^' || c == '`' || c == '{' || c == '}') return true;
    if (set == SET_PATH) return false;
    return c == '/' || c == ':' || c == ';' || c == '=' || c == '@' || (c >= '[' && c <= ']') ||
           c == '|';
}

static void pct_append(hk_charbuf *b, int c, pct_set set)
{
    if (in_set(c, set)) {
        char tmp[4];
        snprintf(tmp, sizeof tmp, "%%%02X", (unsigned)c & 0xFF);
        hk_charbuf_append(b, tmp, 3);
    } else {
        hk_charbuf_push(b, (char)c);
    }
}

/* --- schemes ---------------------------------------------------------------------------------- */

int32_t hk_url_default_port(hk_str s)
{
    if (hk_str_eq(s, HK_STR("http")) || hk_str_eq(s, HK_STR("ws"))) return 80;
    if (hk_str_eq(s, HK_STR("https")) || hk_str_eq(s, HK_STR("wss"))) return 443;
    if (hk_str_eq(s, HK_STR("ftp"))) return 21;
    return -1;
}

static bool scheme_is_special(hk_str s)
{
    return hk_url_default_port(s) >= 0 || hk_str_eq(s, HK_STR("file"));
}

bool hk_url_is_special(const hk_url *u)
{
    return scheme_is_special(u->scheme);
}

int32_t hk_url_effective_port(const hk_url *u)
{
    return u->port >= 0 ? u->port : hk_url_default_port(u->scheme);
}

/* --- hosts ------------------------------------------------------------------------------------ */

static bool forbidden_host(int c)
{
    return c == 0 || c == '\t' || c == '\n' || c == '\r' || c == ' ' || c == '#' || c == '/' ||
           c == ':' || c == '<' || c == '>' || c == '?' || c == '@' || c == '[' || c == '\\' ||
           c == ']' || c == '^' || c == '|';
}

static bool forbidden_domain(int c)
{
    return forbidden_host(c) || (c >= 0 && c <= 0x1F) || c == '%' || c == 0x7F;
}

static bool parse_ipv4_number(hk_str s, uint64_t *out)
{
    if (s.n == 0) return false;
    *out = 0;
    int radix = 10;
    if (s.n >= 2 && s.p[0] == '0' && (s.p[1] == 'x' || s.p[1] == 'X')) {
        radix = 16;
        s = hk_str_slice(s, 2, s.n);
    } else if (s.n >= 2 && s.p[0] == '0') {
        radix = 8;
        s = hk_str_slice(s, 1, s.n);
    }
    uint64_t v = 0; /* an empty string after the prefix is the number 0 */
    for (size_t i = 0; i < s.n; i++) {
        int c = (unsigned char)s.p[i];
        int d;
        if (radix == 16 && is_hex(c)) d = hex_val(c);
        else if (is_digit(c) && c - '0' < radix) d = c - '0';
        else return false;
        v = v * (uint64_t)radix + (uint64_t)d;
        if (v > 0xFFFFFFFFFFull) v = 0xFFFFFFFFFFull; /* saturate: too large either way */
    }
    *out = v;
    return true;
}

static bool ends_in_number(hk_str s)
{
    hk_str last = s;
    if (last.n && last.p[last.n - 1] == '.') last.n--;
    size_t dot = last.n;
    while (dot > 0 && last.p[dot - 1] != '.') dot--;
    hk_str label = hk_str_slice(last, dot, last.n);
    if (label.n == 0) return false;
    bool digits = true;
    for (size_t i = 0; i < label.n; i++) digits = digits && is_digit((unsigned char)label.p[i]);
    if (digits) return true;
    uint64_t v;
    return label.n >= 2 && label.p[0] == '0' && (label.p[1] == 'x' || label.p[1] == 'X') &&
           parse_ipv4_number(label, &v);
}

static bool parse_ipv4(hk_arena *a, hk_str s, hk_str *out)
{
    uint64_t parts[5];
    size_t count = 0, start = 0;
    if (s.n && s.p[s.n - 1] == '.') s.n--;
    for (size_t i = 0; i <= s.n; i++) {
        if (i == s.n || s.p[i] == '.') {
            if (count == 4) return false;
            if (!parse_ipv4_number(hk_str_slice(s, start, i), &parts[count])) return false;
            count++;
            start = i + 1;
        }
    }
    for (size_t i = 0; i + 1 < count; i++) {
        if (parts[i] > 255) return false;
    }
    uint64_t limit = 1ull << (8 * (5 - count));
    if (parts[count - 1] >= limit) return false;
    uint64_t addr = parts[count - 1];
    for (size_t i = 0; i + 1 < count; i++) addr += parts[i] << (8 * (3 - i));
    char buf[16];
    int n = snprintf(buf, sizeof buf, "%u.%u.%u.%u", (unsigned)(addr >> 24) & 255,
                     (unsigned)(addr >> 16) & 255, (unsigned)(addr >> 8) & 255, (unsigned)addr & 255);
    *out = hk_arena_str_dup(a, (hk_str){buf, (size_t)n});
    return true;
}

static bool parse_ipv6(hk_arena *a, hk_str s, hk_str *out)
{
    uint16_t addr[8] = {0};
    int piece = 0, compress = -1;
    size_t i = 0;
    const char *p = s.p;
    size_t n = s.n;
#define C(k) ((k) < n ? (int)(unsigned char)p[k] : EOF_CH)
    if (C(i) == ':') {
        if (C(i + 1) != ':') return false;
        i += 2;
        compress = ++piece;
    }
    while (C(i) != EOF_CH) {
        if (piece == 8) return false;
        if (C(i) == ':') {
            if (compress >= 0) return false;
            i++;
            compress = ++piece;
            continue;
        }
        unsigned value = 0;
        int length = 0;
        while (length < 4 && is_hex(C(i))) {
            value = value * 16 + (unsigned)hex_val(C(i));
            i++;
            length++;
        }
        if (C(i) == '.') {
            if (length == 0) return false;
            i -= (size_t)length;
            if (piece > 6) return false;
            int seen = 0;
            while (C(i) != EOF_CH) {
                int v = -1;
                if (seen > 0) {
                    if (C(i) == '.' && seen < 4) i++;
                    else return false;
                }
                if (!is_digit(C(i))) return false;
                while (is_digit(C(i))) {
                    int d = C(i) - '0';
                    if (v == -1) v = d;
                    else if (v == 0) return false;
                    else v = v * 10 + d;
                    if (v > 255) return false;
                    i++;
                }
                addr[piece] = (uint16_t)(addr[piece] * 0x100 + v);
                seen++;
                if (seen == 2 || seen == 4) piece++;
            }
            if (seen != 4) return false;
            break;
        }
        if (C(i) == ':') {
            i++;
            if (C(i) == EOF_CH) return false;
        } else if (C(i) != EOF_CH) {
            return false;
        }
        addr[piece++] = (uint16_t)value;
    }
#undef C
    if (compress >= 0) {
        int swaps = piece - compress;
        piece = 7;
        while (piece != 0 && swaps > 0) {
            uint16_t t = addr[piece];
            addr[piece] = addr[compress + swaps - 1];
            addr[compress + swaps - 1] = t;
            piece--;
            swaps--;
        }
    } else if (piece != 8) {
        return false;
    }

    /* Serialize, compressing the first longest run of two or more zero pieces. */
    int best = -1, best_len = 1;
    for (int k = 0; k < 8;) {
        int len = 0;
        while (k + len < 8 && addr[k + len] == 0) len++;
        if (len > best_len) {
            best = k;
            best_len = len;
        }
        k += len ? len : 1;
    }
    char buf[48];
    size_t o = 0;
    buf[o++] = '[';
    bool ignore0 = false;
    for (int k = 0; k < 8; k++) {
        if (ignore0 && addr[k] == 0) continue;
        ignore0 = false;
        if (k == best) {
            buf[o++] = ':';
            if (k == 0) buf[o++] = ':';
            ignore0 = true;
            continue;
        }
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%x", addr[k]);
        if (k != 7) buf[o++] = ':';
    }
    buf[o++] = ']';
    *out = hk_arena_str_dup(a, (hk_str){buf, o});
    return true;
}

static hk_str percent_decode(hk_arena *a, hk_str s)
{
    char *out = hk_arena_alloc(a, s.n + 1, 1);
    size_t o = 0;
    for (size_t i = 0; i < s.n; i++) {
        if (s.p[i] == '%' && i + 2 < s.n && is_hex((unsigned char)s.p[i + 1]) &&
            is_hex((unsigned char)s.p[i + 2])) {
            out[o++] = (char)(hex_val((unsigned char)s.p[i + 1]) * 16 + hex_val((unsigned char)s.p[i + 2]));
            i += 2;
        } else {
            out[o++] = s.p[i];
        }
    }
    out[o] = 0;
    return (hk_str){out, o};
}

static bool parse_host(hk_arena *a, hk_str input, bool not_special, hk_str *out)
{
    if (input.n && input.p[0] == '[') {
        if (input.p[input.n - 1] != ']') return false;
        return parse_ipv6(a, hk_str_slice(input, 1, input.n - 1), out);
    }
    if (not_special) {
        hk_charbuf b = {0};
        for (size_t i = 0; i < input.n; i++) {
            int c = (unsigned char)input.p[i];
            if (forbidden_host(c)) {
                hk_vec_free(&b);
                return false;
            }
            pct_append(&b, c, SET_C0);
        }
        *out = hk_arena_str_dup(a, (hk_str){b.data, b.len});
        hk_vec_free(&b);
        return true;
    }
    hk_str domain = percent_decode(a, input);
    bool ascii = true;
    for (size_t i = 0; i < domain.n; i++) ascii = ascii && (unsigned char)domain.p[i] < 0x80;
    hk_str ascii_domain;
    bool has_punycode = false;
    for (size_t i = 0; i + 4 <= domain.n; i++) {
        if ((i == 0 || domain.p[i - 1] == '.') && hk_str_eq_ascii_icase(hk_str_slice(domain, i, i + 4), HK_STR("xn--")))
            has_punycode = true;
    }
    if (ascii && !has_punycode) {
        char *low = hk_arena_alloc(a, domain.n + 1, 1);
        for (size_t i = 0; i < domain.n; i++) low[i] = hk_ascii_lower(domain.p[i]);
        low[domain.n] = 0;
        ascii_domain = (hk_str){low, domain.n};
    } else if (!hk_plat_idna_to_ascii(a, domain, &ascii_domain)) {
        return false;
    }
    if (ascii_domain.n == 0) return false;
    for (size_t i = 0; i < ascii_domain.n; i++) {
        if (forbidden_domain((unsigned char)ascii_domain.p[i])) return false;
    }
    if (ends_in_number(ascii_domain)) return parse_ipv4(a, ascii_domain, out);
    *out = ascii_domain;
    return true;
}

/* --- paths ------------------------------------------------------------------------------------ */

static bool is_windows_drive_letter(hk_str s, bool normalized)
{
    return s.n == 2 && is_alpha((unsigned char)s.p[0]) && (s.p[1] == ':' || (!normalized && s.p[1] == '|'));
}

static bool starts_with_windows_drive_letter(const char *p, size_t n)
{
    if (n < 2 || !is_alpha((unsigned char)p[0]) || (p[1] != ':' && p[1] != '|')) return false;
    if (n == 2) return true;
    char c = p[2];
    return c == '/' || c == '\\' || c == '?' || c == '#';
}

static bool is_single_dot(hk_str s)
{
    return hk_str_eq(s, HK_STR(".")) || hk_str_eq_ascii_icase(s, HK_STR("%2e"));
}

static bool is_double_dot(hk_str s)
{
    return hk_str_eq(s, HK_STR("..")) || hk_str_eq_ascii_icase(s, HK_STR(".%2e")) ||
           hk_str_eq_ascii_icase(s, HK_STR("%2e.")) || hk_str_eq_ascii_icase(s, HK_STR("%2e%2e"));
}

static void shorten_path(seg_list *path, hk_str scheme)
{
    if (hk_str_eq(scheme, HK_STR("file")) && path->len == 1 && is_windows_drive_letter(path->data[0], true))
        return;
    if (path->len) path->len--;
}

/* Splits a serialized (non-opaque) path back into segments. */
static void split_path(hk_str path, seg_list *out)
{
    out->len = 0;
    if (path.n == 0) return;
    size_t start = 1;
    for (size_t i = 1; i <= path.n; i++) {
        if (i == path.n || path.p[i] == '/') {
            hk_vec_push(out, hk_str_slice(path, start, i));
            start = i + 1;
        }
    }
}

/* --- the parser ------------------------------------------------------------------------------- */

static hk_str buf_dup(hk_arena *a, const hk_charbuf *b)
{
    return hk_arena_str_dup(a, (hk_str){b->data, b->len});
}

static hk_status parse(hk_arena *a, hk_str in, const hk_url *base, hk_url *url)
{
    /* Strip leading/trailing C0 controls and spaces, remove tabs and newlines. */
    size_t b0 = 0, e0 = in.n;
    while (b0 < e0 && (unsigned char)in.p[b0] <= 0x20) b0++;
    while (e0 > b0 && (unsigned char)in.p[e0 - 1] <= 0x20) e0--;
    hk_charbuf clean = {0};
    for (size_t i = b0; i < e0; i++) {
        char c = in.p[i];
        if (c != '\t' && c != '\n' && c != '\r') hk_charbuf_push(&clean, c);
    }
    const char *p = clean.data;
    size_t n = clean.len;

    memset(url, 0, sizeof *url);
    url->port = -1;
    url->scheme = HK_STR("");
    url->username = url->password = url->host = url->path = url->query = url->fragment = HK_STR("");
    seg_list path = {0};
    hk_charbuf buf = {0}, opaque = {0}, query = {0}, frag = {0}, user = {0}, pass = {0};
    bool at_seen = false, in_brackets = false, password_seen = false;
    hk_status st = HK_OK;
    state s = S_SCHEME_START;
    bool special = false;

    for (size_t ptr = 0;; ptr++) {
        int c = ptr < n ? (unsigned char)p[ptr] : EOF_CH;
        size_t remaining = ptr < n ? n - ptr - 1 : 0;
        const char *rest = p ? p + ptr + 1 : NULL;
        switch (s) {
        case S_SCHEME_START:
            if (is_alpha(c)) {
                hk_charbuf_push(&buf, hk_ascii_lower((char)c));
                s = S_SCHEME;
            } else {
                s = S_NO_SCHEME;
                ptr--;
            }
            break;

        case S_SCHEME:
            if (is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.') {
                hk_charbuf_push(&buf, hk_ascii_lower((char)c));
            } else if (c == ':') {
                url->scheme = buf_dup(a, &buf);
                buf.len = 0;
                special = scheme_is_special(url->scheme);
                if (hk_str_eq(url->scheme, HK_STR("file"))) {
                    s = S_FILE;
                } else if (special && base && hk_str_eq(base->scheme, url->scheme)) {
                    s = S_SPECIAL_RELATIVE_OR_AUTHORITY;
                } else if (special) {
                    s = S_SPECIAL_AUTHORITY_SLASHES;
                } else if (remaining && rest[0] == '/') {
                    s = S_PATH_OR_AUTHORITY;
                    ptr++;
                } else {
                    url->opaque_path = true;
                    s = S_OPAQUE_PATH;
                }
            } else {
                buf.len = 0;
                s = S_NO_SCHEME;
                ptr = (size_t)-1; /* start over */
            }
            break;

        case S_NO_SCHEME:
            if (!base || (base->opaque_path && c != '#')) {
                st = HK_ERR_INVALID;
                goto done;
            }
            if (base->opaque_path && c == '#') {
                url->scheme = hk_arena_str_dup(a, base->scheme);
                special = scheme_is_special(url->scheme);
                url->opaque_path = true;
                hk_charbuf_append(&opaque, base->path.p, base->path.n);
                url->has_query = base->has_query;
                hk_charbuf_append(&query, base->query.p, base->query.n);
                url->has_fragment = true;
                s = S_FRAGMENT;
            } else if (!hk_str_eq(base->scheme, HK_STR("file"))) {
                s = S_RELATIVE;
                ptr--;
            } else {
                s = S_FILE;
                ptr--;
            }
            break;

        case S_SPECIAL_RELATIVE_OR_AUTHORITY:
            if (c == '/' && remaining && rest[0] == '/') {
                s = S_SPECIAL_AUTHORITY_IGNORE_SLASHES;
                ptr++;
            } else {
                s = S_RELATIVE;
                ptr--;
            }
            break;

        case S_PATH_OR_AUTHORITY:
            if (c == '/') s = S_AUTHORITY;
            else {
                s = S_PATH;
                ptr--;
            }
            break;

        case S_RELATIVE:
            url->scheme = hk_arena_str_dup(a, base->scheme);
            special = scheme_is_special(url->scheme);
            if (c == '/') s = S_RELATIVE_SLASH;
            else if (special && c == '\\') s = S_RELATIVE_SLASH;
            else {
                hk_charbuf_append(&user, base->username.p, base->username.n);
                hk_charbuf_append(&pass, base->password.p, base->password.n);
                url->has_host = base->has_host;
                url->host = hk_arena_str_dup(a, base->host);
                url->port = base->port;
                split_path(base->path, &path);
                url->has_query = base->has_query;
                hk_charbuf_append(&query, base->query.p, base->query.n);
                if (c == '?') {
                    url->has_query = true;
                    query.len = 0;
                    s = S_QUERY;
                } else if (c == '#') {
                    url->has_fragment = true;
                    s = S_FRAGMENT;
                } else if (c != EOF_CH) {
                    url->has_query = false;
                    query.len = 0;
                    shorten_path(&path, url->scheme);
                    s = S_PATH;
                    ptr--;
                }
            }
            break;

        case S_RELATIVE_SLASH:
            if (special && (c == '/' || c == '\\')) s = S_SPECIAL_AUTHORITY_IGNORE_SLASHES;
            else if (c == '/') s = S_AUTHORITY;
            else {
                hk_charbuf_append(&user, base->username.p, base->username.n);
                hk_charbuf_append(&pass, base->password.p, base->password.n);
                url->has_host = base->has_host;
                url->host = hk_arena_str_dup(a, base->host);
                url->port = base->port;
                s = S_PATH;
                ptr--;
            }
            break;

        case S_SPECIAL_AUTHORITY_SLASHES:
            s = S_SPECIAL_AUTHORITY_IGNORE_SLASHES;
            if (c == '/' && remaining && rest[0] == '/') ptr++;
            else ptr--;
            break;

        case S_SPECIAL_AUTHORITY_IGNORE_SLASHES:
            if (c != '/' && c != '\\') {
                s = S_AUTHORITY;
                ptr--;
            }
            break;

        case S_AUTHORITY:
            if (c == '@') {
                if (at_seen) {
                    hk_charbuf tmp = {0};
                    hk_charbuf_append(&tmp, "%40", 3);
                    hk_charbuf_append(&tmp, buf.data, buf.len);
                    buf.len = 0;
                    hk_charbuf_append(&buf, tmp.data, tmp.len);
                    hk_vec_free(&tmp);
                }
                at_seen = true;
                for (size_t k = 0; k < buf.len; k++) {
                    int cc = (unsigned char)buf.data[k];
                    if (cc == ':' && !password_seen) {
                        password_seen = true;
                        continue;
                    }
                    pct_append(password_seen ? &pass : &user, cc, SET_USERINFO);
                }
                buf.len = 0;
            } else if (c == EOF_CH || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
                if (at_seen && buf.len == 0) {
                    st = HK_ERR_INVALID;
                    goto done;
                }
                ptr -= buf.len + 1;
                buf.len = 0;
                s = S_HOST;
            } else {
                hk_charbuf_push(&buf, (char)c);
            }
            break;

        case S_HOST:
            if (c == ':' && !in_brackets) {
                if (buf.len == 0) {
                    st = HK_ERR_INVALID;
                    goto done;
                }
                if (!parse_host(a, (hk_str){buf.data, buf.len}, !special, &url->host)) {
                    st = HK_ERR_INVALID;
                    goto done;
                }
                url->has_host = true;
                buf.len = 0;
                s = S_PORT;
            } else if (c == EOF_CH || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
                ptr--;
                if (special && buf.len == 0) {
                    st = HK_ERR_INVALID;
                    goto done;
                }
                if (!parse_host(a, (hk_str){buf.data, buf.len}, !special, &url->host)) {
                    st = HK_ERR_INVALID;
                    goto done;
                }
                url->has_host = true;
                buf.len = 0;
                s = S_PATH_START;
            } else {
                if (c == '[') in_brackets = true;
                if (c == ']') in_brackets = false;
                hk_charbuf_push(&buf, (char)c);
            }
            break;

        case S_PORT:
            if (is_digit(c)) {
                hk_charbuf_push(&buf, (char)c);
            } else if (c == EOF_CH || c == '/' || c == '?' || c == '#' || (special && c == '\\')) {
                if (buf.len) {
                    uint32_t port = 0;
                    for (size_t k = 0; k < buf.len; k++) {
                        port = port * 10 + (uint32_t)(buf.data[k] - '0');
                        if (port > 65535) {
                            st = HK_ERR_INVALID;
                            goto done;
                        }
                    }
                    url->port = (int32_t)port == hk_url_default_port(url->scheme) ? -1 : (int32_t)port;
                    buf.len = 0;
                }
                s = S_PATH_START;
                ptr--;
            } else {
                st = HK_ERR_INVALID;
                goto done;
            }
            break;

        case S_FILE:
            url->scheme = HK_STR("file");
            special = true;
            url->has_host = true;
            url->host = HK_STR("");
            if (c == '/' || c == '\\') {
                s = S_FILE_SLASH;
            } else if (base && hk_str_eq(base->scheme, HK_STR("file"))) {
                url->has_host = base->has_host;
                url->host = hk_arena_str_dup(a, base->host);
                split_path(base->path, &path);
                url->has_query = base->has_query;
                hk_charbuf_append(&query, base->query.p, base->query.n);
                if (c == '?') {
                    url->has_query = true;
                    query.len = 0;
                    s = S_QUERY;
                } else if (c == '#') {
                    url->has_fragment = true;
                    s = S_FRAGMENT;
                } else if (c != EOF_CH) {
                    url->has_query = false;
                    query.len = 0;
                    if (!starts_with_windows_drive_letter(p + ptr, n - ptr)) shorten_path(&path, url->scheme);
                    else path.len = 0;
                    s = S_PATH;
                    ptr--;
                }
            } else {
                s = S_PATH;
                ptr--;
            }
            break;

        case S_FILE_SLASH:
            if (c == '/' || c == '\\') {
                s = S_FILE_HOST;
            } else {
                if (base && hk_str_eq(base->scheme, HK_STR("file"))) {
                    url->has_host = base->has_host;
                    url->host = hk_arena_str_dup(a, base->host);
                    seg_list bp = {0};
                    split_path(base->path, &bp);
                    if (!starts_with_windows_drive_letter(p + ptr, ptr < n ? n - ptr : 0) && bp.len &&
                        is_windows_drive_letter(bp.data[0], true))
                        hk_vec_push(&path, bp.data[0]);
                    hk_vec_free(&bp);
                }
                s = S_PATH;
                ptr--;
            }
            break;

        case S_FILE_HOST:
            if (c == EOF_CH || c == '/' || c == '\\' || c == '?' || c == '#') {
                ptr--;
                if (is_windows_drive_letter((hk_str){buf.data, buf.len}, false)) {
                    s = S_PATH; /* the buffer is kept and becomes the first path segment */
                } else if (buf.len == 0) {
                    url->has_host = true;
                    url->host = HK_STR("");
                    s = S_PATH_START;
                } else {
                    hk_str host;
                    if (!parse_host(a, (hk_str){buf.data, buf.len}, false, &host)) {
                        st = HK_ERR_INVALID;
                        goto done;
                    }
                    if (hk_str_eq(host, HK_STR("localhost"))) host = HK_STR("");
                    url->has_host = true;
                    url->host = host;
                    buf.len = 0;
                    s = S_PATH_START;
                }
            } else {
                hk_charbuf_push(&buf, (char)c);
            }
            break;

        case S_PATH_START:
            if (special) {
                s = S_PATH;
                if (c != '/' && c != '\\') ptr--;
            } else if (c == '?') {
                url->has_query = true;
                s = S_QUERY;
            } else if (c == '#') {
                url->has_fragment = true;
                s = S_FRAGMENT;
            } else if (c != EOF_CH) {
                s = S_PATH;
                if (c != '/') ptr--;
            }
            break;

        case S_PATH:
            if (c == EOF_CH || c == '/' || (special && c == '\\') || c == '?' || c == '#') {
                hk_str seg = {buf.data, buf.len};
                bool slash = c == '/' || (special && c == '\\');
                if (is_double_dot(seg)) {
                    shorten_path(&path, url->scheme);
                    if (!slash) hk_vec_push(&path, HK_STR(""));
                } else if (is_single_dot(seg) && !slash) {
                    hk_vec_push(&path, HK_STR(""));
                } else if (!is_single_dot(seg)) {
                    if (hk_str_eq(url->scheme, HK_STR("file")) && path.len == 0 &&
                        is_windows_drive_letter(seg, false))
                        buf.data[1] = ':';
                    hk_vec_push(&path, buf_dup(a, &buf));
                }
                buf.len = 0;
                if (c == '?') {
                    url->has_query = true;
                    query.len = 0;
                    s = S_QUERY;
                } else if (c == '#') {
                    url->has_fragment = true;
                    s = S_FRAGMENT;
                }
            } else {
                pct_append(&buf, c, SET_PATH);
            }
            break;

        case S_OPAQUE_PATH:
            if (c == '?') {
                url->has_query = true;
                s = S_QUERY;
            } else if (c == '#') {
                url->has_fragment = true;
                s = S_FRAGMENT;
            } else if (c == ' ') {
                bool next_special = remaining && (rest[0] == '?' || rest[0] == '#');
                if (next_special) hk_charbuf_append(&opaque, "%20", 3);
                else hk_charbuf_push(&opaque, ' ');
            } else if (c != EOF_CH) {
                pct_append(&opaque, c, SET_C0);
            }
            break;

        case S_QUERY:
            if (c == '#' || c == EOF_CH) {
                for (size_t k = 0; k < buf.len; k++)
                    pct_append(&query, (unsigned char)buf.data[k], special ? SET_SPECIAL_QUERY : SET_QUERY);
                buf.len = 0;
                if (c == '#') {
                    url->has_fragment = true;
                    s = S_FRAGMENT;
                }
            } else {
                hk_charbuf_push(&buf, (char)c);
            }
            break;

        case S_FRAGMENT:
            if (c != EOF_CH) pct_append(&frag, c, SET_FRAGMENT);
            break;
        }
        if (c == EOF_CH && ptr >= n && ptr != (size_t)-1) break;
    }

    /* Assemble the components and the serialization. */
    url->username = buf_dup(a, &user);
    url->password = buf_dup(a, &pass);
    url->query = buf_dup(a, &query);
    url->fragment = buf_dup(a, &frag);
    {
        hk_charbuf ser = {0};
        if (url->opaque_path) {
            url->path = buf_dup(a, &opaque);
        } else {
            for (uint32_t k = 0; k < path.len; k++) {
                hk_charbuf_push(&ser, '/');
                hk_charbuf_append(&ser, path.data[k].p, path.data[k].n);
            }
            url->path = buf_dup(a, &ser);
        }
        ser.len = 0;
        hk_charbuf_append(&ser, url->scheme.p, url->scheme.n);
        hk_charbuf_push(&ser, ':');
        if (url->has_host) {
            hk_charbuf_append(&ser, "//", 2);
            if (url->username.n || url->password.n) {
                hk_charbuf_append(&ser, url->username.p, url->username.n);
                if (url->password.n) {
                    hk_charbuf_push(&ser, ':');
                    hk_charbuf_append(&ser, url->password.p, url->password.n);
                }
                hk_charbuf_push(&ser, '@');
            }
            hk_charbuf_append(&ser, url->host.p, url->host.n);
            if (url->port >= 0) {
                char tmp[8];
                int k = snprintf(tmp, sizeof tmp, ":%d", url->port);
                hk_charbuf_append(&ser, tmp, (size_t)k);
            }
        } else if (!url->opaque_path && path.len > 1 && path.data[0].n == 0) {
            hk_charbuf_append(&ser, "/.", 2);
        }
        hk_charbuf_append(&ser, url->path.p, url->path.n);
        if (url->has_query) {
            hk_charbuf_push(&ser, '?');
            hk_charbuf_append(&ser, url->query.p, url->query.n);
        }
        if (url->has_fragment) {
            hk_charbuf_push(&ser, '#');
            hk_charbuf_append(&ser, url->fragment.p, url->fragment.n);
        }
        url->href = buf_dup(a, &ser);
        hk_vec_free(&ser);
    }

done:
    hk_vec_free(&clean);
    hk_vec_free(&path);
    hk_vec_free(&buf);
    hk_vec_free(&opaque);
    hk_vec_free(&query);
    hk_vec_free(&frag);
    hk_vec_free(&user);
    hk_vec_free(&pass);
    return st;
}

hk_status hk_url_parse(hk_arena *a, hk_str input, const hk_url *base, hk_url *out)
{
    return parse(a, input, base, out);
}

hk_str hk_url_request_target(hk_arena *a, const hk_url *u)
{
    hk_charbuf b = {0};
    if (u->path.n == 0) hk_charbuf_push(&b, '/');
    hk_charbuf_append(&b, u->path.p, u->path.n);
    if (u->has_query) {
        hk_charbuf_push(&b, '?');
        hk_charbuf_append(&b, u->query.p, u->query.n);
    }
    hk_str r = hk_arena_str_dup(a, (hk_str){b.data, b.len});
    hk_vec_free(&b);
    return r;
}
