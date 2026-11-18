/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net/hk_encoding.h"
#include "base/hk_utf8.h"
#include "plat/hk_plat.h"

#include <string.h>

/* --- labels ------------------------------------------------------------------------------ */

typedef struct label {
    const char *name;
    hk_encoding enc;
} label;

static const label k_labels[] = {
    {"unicode-1-1-utf-8", HK_ENC_UTF8}, {"unicode11utf8", HK_ENC_UTF8}, {"unicode20utf8", HK_ENC_UTF8},
    {"utf-8", HK_ENC_UTF8}, {"utf8", HK_ENC_UTF8}, {"x-unicode20utf8", HK_ENC_UTF8},
    {"ansi_x3.4-1968", HK_ENC_WINDOWS_1252}, {"ascii", HK_ENC_WINDOWS_1252},
    {"cp1252", HK_ENC_WINDOWS_1252}, {"cp819", HK_ENC_WINDOWS_1252},
    {"csisolatin1", HK_ENC_WINDOWS_1252}, {"ibm819", HK_ENC_WINDOWS_1252},
    {"iso-8859-1", HK_ENC_WINDOWS_1252}, {"iso-ir-100", HK_ENC_WINDOWS_1252},
    {"iso8859-1", HK_ENC_WINDOWS_1252}, {"iso88591", HK_ENC_WINDOWS_1252},
    {"iso_8859-1", HK_ENC_WINDOWS_1252}, {"iso_8859-1:1987", HK_ENC_WINDOWS_1252},
    {"l1", HK_ENC_WINDOWS_1252}, {"latin1", HK_ENC_WINDOWS_1252}, {"us-ascii", HK_ENC_WINDOWS_1252},
    {"windows-1252", HK_ENC_WINDOWS_1252}, {"x-cp1252", HK_ENC_WINDOWS_1252},
    {"csshiftjis", HK_ENC_SHIFT_JIS}, {"ms932", HK_ENC_SHIFT_JIS}, {"ms_kanji", HK_ENC_SHIFT_JIS},
    {"shift-jis", HK_ENC_SHIFT_JIS}, {"shift_jis", HK_ENC_SHIFT_JIS}, {"sjis", HK_ENC_SHIFT_JIS},
    {"windows-31j", HK_ENC_SHIFT_JIS}, {"x-sjis", HK_ENC_SHIFT_JIS},
    {"cseucpkdfmtjapanese", HK_ENC_EUC_JP}, {"euc-jp", HK_ENC_EUC_JP}, {"x-euc-jp", HK_ENC_EUC_JP},
    {"csiso2022jp", HK_ENC_ISO_2022_JP}, {"iso-2022-jp", HK_ENC_ISO_2022_JP},
    {"gb18030", HK_ENC_GB18030},
    {"chinese", HK_ENC_GBK}, {"csgb2312", HK_ENC_GBK}, {"csiso58gb231280", HK_ENC_GBK},
    {"gb2312", HK_ENC_GBK}, {"gb_2312", HK_ENC_GBK}, {"gb_2312-80", HK_ENC_GBK}, {"gbk", HK_ENC_GBK},
    {"iso-ir-58", HK_ENC_GBK}, {"x-gbk", HK_ENC_GBK},
    {"big5", HK_ENC_BIG5}, {"big5-hkscs", HK_ENC_BIG5}, {"cn-big5", HK_ENC_BIG5},
    {"csbig5", HK_ENC_BIG5}, {"x-x-big5", HK_ENC_BIG5},
    {"cseuckr", HK_ENC_EUC_KR}, {"csksc56011987", HK_ENC_EUC_KR}, {"euc-kr", HK_ENC_EUC_KR},
    {"iso-ir-149", HK_ENC_EUC_KR}, {"korean", HK_ENC_EUC_KR}, {"ks_c_5601-1987", HK_ENC_EUC_KR},
    {"ks_c_5601-1989", HK_ENC_EUC_KR}, {"ksc5601", HK_ENC_EUC_KR}, {"ksc_5601", HK_ENC_EUC_KR},
    {"windows-949", HK_ENC_EUC_KR},
    {"csunicode", HK_ENC_UTF16LE}, {"iso-10646-ucs-2", HK_ENC_UTF16LE}, {"ucs-2", HK_ENC_UTF16LE},
    {"unicode", HK_ENC_UTF16LE}, {"unicodefeff", HK_ENC_UTF16LE}, {"utf-16", HK_ENC_UTF16LE},
    {"utf-16le", HK_ENC_UTF16LE}, {"unicodefffe", HK_ENC_UTF16BE}, {"utf-16be", HK_ENC_UTF16BE},
    {"csisolatin2", HK_ENC_ISO_8859_2}, {"iso-8859-2", HK_ENC_ISO_8859_2},
    {"iso-ir-101", HK_ENC_ISO_8859_2}, {"iso8859-2", HK_ENC_ISO_8859_2},
    {"iso88592", HK_ENC_ISO_8859_2}, {"iso_8859-2", HK_ENC_ISO_8859_2},
    {"iso_8859-2:1987", HK_ENC_ISO_8859_2}, {"l2", HK_ENC_ISO_8859_2}, {"latin2", HK_ENC_ISO_8859_2},
    {"csisolatincyrillic", HK_ENC_ISO_8859_5}, {"cyrillic", HK_ENC_ISO_8859_5},
    {"iso-8859-5", HK_ENC_ISO_8859_5}, {"iso-ir-144", HK_ENC_ISO_8859_5},
    {"iso8859-5", HK_ENC_ISO_8859_5}, {"iso88595", HK_ENC_ISO_8859_5},
    {"iso_8859-5", HK_ENC_ISO_8859_5}, {"iso_8859-5:1988", HK_ENC_ISO_8859_5},
    {"csisolatingreek", HK_ENC_ISO_8859_7}, {"ecma-118", HK_ENC_ISO_8859_7},
    {"elot_928", HK_ENC_ISO_8859_7}, {"greek", HK_ENC_ISO_8859_7}, {"greek8", HK_ENC_ISO_8859_7},
    {"iso-8859-7", HK_ENC_ISO_8859_7}, {"iso-ir-126", HK_ENC_ISO_8859_7},
    {"iso8859-7", HK_ENC_ISO_8859_7}, {"iso88597", HK_ENC_ISO_8859_7},
    {"iso_8859-7", HK_ENC_ISO_8859_7}, {"iso_8859-7:1987", HK_ENC_ISO_8859_7},
    {"sun_eu_greek", HK_ENC_ISO_8859_7},
    {"csisolatin9", HK_ENC_ISO_8859_15}, {"iso-8859-15", HK_ENC_ISO_8859_15},
    {"iso8859-15", HK_ENC_ISO_8859_15}, {"iso885915", HK_ENC_ISO_8859_15},
    {"iso_8859-15", HK_ENC_ISO_8859_15}, {"l9", HK_ENC_ISO_8859_15},
    {"cp1250", HK_ENC_WINDOWS_1250}, {"windows-1250", HK_ENC_WINDOWS_1250}, {"x-cp1250", HK_ENC_WINDOWS_1250},
    {"cp1251", HK_ENC_WINDOWS_1251}, {"windows-1251", HK_ENC_WINDOWS_1251}, {"x-cp1251", HK_ENC_WINDOWS_1251},
    {"cp1253", HK_ENC_WINDOWS_1253}, {"windows-1253", HK_ENC_WINDOWS_1253}, {"x-cp1253", HK_ENC_WINDOWS_1253},
    {"cp1254", HK_ENC_WINDOWS_1254}, {"csisolatin5", HK_ENC_WINDOWS_1254},
    {"iso-8859-9", HK_ENC_WINDOWS_1254}, {"iso-ir-148", HK_ENC_WINDOWS_1254},
    {"iso8859-9", HK_ENC_WINDOWS_1254}, {"iso88599", HK_ENC_WINDOWS_1254},
    {"iso_8859-9", HK_ENC_WINDOWS_1254}, {"iso_8859-9:1989", HK_ENC_WINDOWS_1254},
    {"l5", HK_ENC_WINDOWS_1254}, {"latin5", HK_ENC_WINDOWS_1254},
    {"windows-1254", HK_ENC_WINDOWS_1254}, {"x-cp1254", HK_ENC_WINDOWS_1254},
    {"cp1255", HK_ENC_WINDOWS_1255}, {"windows-1255", HK_ENC_WINDOWS_1255}, {"x-cp1255", HK_ENC_WINDOWS_1255},
    {"cp1256", HK_ENC_WINDOWS_1256}, {"windows-1256", HK_ENC_WINDOWS_1256}, {"x-cp1256", HK_ENC_WINDOWS_1256},
    {"cp1257", HK_ENC_WINDOWS_1257}, {"windows-1257", HK_ENC_WINDOWS_1257}, {"x-cp1257", HK_ENC_WINDOWS_1257},
    {"cp1258", HK_ENC_WINDOWS_1258}, {"windows-1258", HK_ENC_WINDOWS_1258}, {"x-cp1258", HK_ENC_WINDOWS_1258},
    {"dos-874", HK_ENC_WINDOWS_874}, {"iso-8859-11", HK_ENC_WINDOWS_874},
    {"iso8859-11", HK_ENC_WINDOWS_874}, {"iso885911", HK_ENC_WINDOWS_874},
    {"tis-620", HK_ENC_WINDOWS_874}, {"windows-874", HK_ENC_WINDOWS_874},
    {"cskoi8r", HK_ENC_KOI8_R}, {"koi", HK_ENC_KOI8_R}, {"koi8", HK_ENC_KOI8_R},
    {"koi8-r", HK_ENC_KOI8_R}, {"koi8_r", HK_ENC_KOI8_R},
    {"koi8-ru", HK_ENC_KOI8_U}, {"koi8-u", HK_ENC_KOI8_U},
    {"866", HK_ENC_IBM866}, {"cp866", HK_ENC_IBM866}, {"csibm866", HK_ENC_IBM866}, {"ibm866", HK_ENC_IBM866},
};

static const struct {
    const char *name;
    uint32_t codepage; /* 0 = decoded by Hawk itself */
    bool whole_input;  /* stateful or multi-byte without a reliable lead-byte test */
} k_info[HK_ENC_COUNT_] = {
    [HK_ENC_UNKNOWN] = {"unknown", 0, false},
    [HK_ENC_UTF8] = {"UTF-8", 0, false},
    [HK_ENC_UTF16LE] = {"UTF-16LE", 0, false},
    [HK_ENC_UTF16BE] = {"UTF-16BE", 0, false},
    [HK_ENC_WINDOWS_1252] = {"windows-1252", 0, false},
    [HK_ENC_SHIFT_JIS] = {"Shift_JIS", 932, false},
    [HK_ENC_EUC_JP] = {"EUC-JP", 20932, true},
    [HK_ENC_ISO_2022_JP] = {"ISO-2022-JP", 50220, true},
    [HK_ENC_GB18030] = {"gb18030", 54936, true},
    [HK_ENC_GBK] = {"GBK", 936, false},
    [HK_ENC_BIG5] = {"Big5", 950, false},
    [HK_ENC_EUC_KR] = {"EUC-KR", 949, false},
    [HK_ENC_ISO_8859_2] = {"ISO-8859-2", 28592, false},
    [HK_ENC_ISO_8859_5] = {"ISO-8859-5", 28595, false},
    [HK_ENC_ISO_8859_7] = {"ISO-8859-7", 28597, false},
    [HK_ENC_ISO_8859_15] = {"ISO-8859-15", 28605, false},
    [HK_ENC_WINDOWS_1250] = {"windows-1250", 1250, false},
    [HK_ENC_WINDOWS_1251] = {"windows-1251", 1251, false},
    [HK_ENC_WINDOWS_1253] = {"windows-1253", 1253, false},
    [HK_ENC_WINDOWS_1254] = {"windows-1254", 1254, false},
    [HK_ENC_WINDOWS_1255] = {"windows-1255", 1255, false},
    [HK_ENC_WINDOWS_1256] = {"windows-1256", 1256, false},
    [HK_ENC_WINDOWS_1257] = {"windows-1257", 1257, false},
    [HK_ENC_WINDOWS_1258] = {"windows-1258", 1258, false},
    [HK_ENC_WINDOWS_874] = {"windows-874", 874, false},
    [HK_ENC_KOI8_R] = {"KOI8-R", 20866, false},
    [HK_ENC_KOI8_U] = {"KOI8-U", 21866, false},
    [HK_ENC_IBM866] = {"IBM866", 866, false},
};

static bool is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

hk_encoding hk_encoding_from_label(hk_str l)
{
    l = hk_str_trim_ascii_ws(l);
    for (size_t i = 0; i < HK_ARRAY_LEN(k_labels); i++) {
        if (hk_str_eq_ascii_icase(l, hk_str_from_cstr(k_labels[i].name))) return k_labels[i].enc;
    }
    return HK_ENC_UNKNOWN;
}

const char *hk_encoding_name(hk_encoding e)
{
    return e < HK_ENC_COUNT_ ? k_info[e].name : "unknown";
}

/* Value after "charset=" in s (case-insensitive), possibly quoted; stops at ';' or whitespace. */
static hk_str charset_param(hk_str s, bool allow_ws_around_eq)
{
    for (size_t i = 0; i + 7 <= s.n; i++) {
        if (!hk_str_eq_ascii_icase(hk_str_slice(s, i, i + 7), HK_STR("charset"))) continue;
        size_t k = i + 7;
        if (allow_ws_around_eq)
            while (k < s.n && is_ws((uint8_t)s.p[k])) k++;
        if (k >= s.n || s.p[k] != '=') continue;
        k++;
        if (allow_ws_around_eq)
            while (k < s.n && is_ws((uint8_t)s.p[k])) k++;
        if (k < s.n && (s.p[k] == '"' || s.p[k] == '\'')) {
            char q = s.p[k++];
            size_t e = k;
            while (e < s.n && s.p[e] != q) e++;
            if (e >= s.n) return HK_STR("");
            return hk_str_slice(s, k, e);
        }
        size_t e = k;
        while (e < s.n && s.p[e] != ';' && !is_ws((uint8_t)s.p[e]) && s.p[e] != '"' && s.p[e] != '\'') e++;
        return hk_str_slice(s, k, e);
    }
    return HK_STR("");
}

hk_encoding hk_encoding_from_content_type(hk_str ct)
{
    size_t semi = hk_str_find_char(ct, ';');
    if (semi == HK_STR_NPOS) return HK_ENC_UNKNOWN;
    hk_str v = charset_param(hk_str_slice(ct, semi + 1, ct.n), true);
    return v.n ? hk_encoding_from_label(v) : HK_ENC_UNKNOWN;
}

hk_encoding hk_encoding_from_bom(const uint8_t *p, size_t n, size_t *bom_len)
{
    *bom_len = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        *bom_len = 3;
        return HK_ENC_UTF8;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        *bom_len = 2;
        return HK_ENC_UTF16BE;
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        *bom_len = 2;
        return HK_ENC_UTF16LE;
    }
    return HK_ENC_UNKNOWN;
}

/* --- <meta> prescan (HTML 13.2.3.2) --------------------------------------------------------- */

typedef struct scan {
    const uint8_t *p;
    size_t n, pos;
    char name[64], value[128];
    size_t name_len, value_len;
} scan;

static char lower(uint8_t c)
{
    return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

static void put_name(scan *s, uint8_t c)
{
    if (s->name_len < sizeof s->name - 1) s->name[s->name_len++] = lower(c);
}

static void put_value(scan *s, uint8_t c)
{
    if (s->value_len < sizeof s->value - 1) s->value[s->value_len++] = lower(c);
}

/* "Get an attribute". Returns 0 = none, 1 = attribute, -1 = ran out of input. */
static int get_attribute(scan *s)
{
#define AT(k) ((k) < s->n ? s->p[k] : 0)
    while (s->pos < s->n && (is_ws(s->p[s->pos]) || s->p[s->pos] == '/')) s->pos++;
    if (s->pos >= s->n) return -1;
    if (s->p[s->pos] == '>') return 0;
    s->name_len = s->value_len = 0;
    for (;;) {
        if (s->pos >= s->n) return -1;
        uint8_t c = s->p[s->pos];
        if (c == '=' && s->name_len) {
            s->pos++;
            goto value;
        }
        if (is_ws(c)) break;
        if (c == '/' || c == '>') return 1;
        put_name(s, c);
        s->pos++;
    }
    while (s->pos < s->n && is_ws(s->p[s->pos])) s->pos++;
    if (s->pos >= s->n) return -1;
    if (s->p[s->pos] != '=') return 1;
    s->pos++;
value:
    while (s->pos < s->n && is_ws(s->p[s->pos])) s->pos++;
    if (s->pos >= s->n) return -1;
    {
        uint8_t c = s->p[s->pos];
        if (c == '"' || c == '\'') {
            for (s->pos++;; s->pos++) {
                if (s->pos >= s->n) return -1;
                if (s->p[s->pos] == c) {
                    s->pos++;
                    return 1;
                }
                put_value(s, s->p[s->pos]);
            }
        }
        if (c == '>') return 1;
        put_value(s, c);
        s->pos++;
    }
    for (;; s->pos++) {
        if (s->pos >= s->n) return -1;
        uint8_t c = s->p[s->pos];
        if (is_ws(c) || c == '>') return 1;
        put_value(s, c);
    }
#undef AT
}

static bool at_icase(const scan *s, const char *lit)
{
    size_t n = strlen(lit);
    if (s->pos + n > s->n) return false;
    for (size_t i = 0; i < n; i++) {
        if (lower(s->p[s->pos + i]) != lit[i]) return false;
    }
    return true;
}

static bool is_letter(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

hk_encoding hk_encoding_prescan(const uint8_t *p, size_t n)
{
    scan s = {p, HK_MIN(n, (size_t)1024), 0, {0}, {0}, 0, 0};
    while (s.pos < s.n) {
        if (at_icase(&s, "<!--")) {
            size_t k = s.pos + 2;
            while (k + 2 < s.n && !(s.p[k] == '-' && s.p[k + 1] == '-' && s.p[k + 2] == '>')) k++;
            if (k + 2 >= s.n) return HK_ENC_UNKNOWN;
            s.pos = k + 3;
            continue;
        }
        if (at_icase(&s, "<meta") && s.pos + 5 < s.n && (is_ws(s.p[s.pos + 5]) || s.p[s.pos + 5] == '/')) {
            s.pos += 5;
            bool got_pragma = false;
            int need_pragma = -1; /* -1 null, 0 false, 1 true */
            hk_encoding charset = HK_ENC_UNKNOWN;
            char seen[8][64];
            size_t seen_count = 0;
            for (;;) {
                int r = get_attribute(&s);
                if (r < 0) return HK_ENC_UNKNOWN;
                if (r == 0) break;
                s.name[s.name_len] = 0;
                hk_str name = {s.name, s.name_len};
                bool dup = false;
                for (size_t k = 0; k < seen_count && !dup; k++) dup = strcmp(seen[k], s.name) == 0;
                if (dup) continue;
                if (seen_count < 8) memcpy(seen[seen_count++], s.name, s.name_len + 1);
                hk_str value = {s.value, s.value_len};
                if (hk_str_eq(name, HK_STR("http-equiv"))) {
                    if (hk_str_eq(value, HK_STR("content-type"))) got_pragma = true;
                } else if (hk_str_eq(name, HK_STR("content"))) {
                    hk_str cs = charset_param(value, true);
                    if (cs.n && charset == HK_ENC_UNKNOWN) {
                        charset = hk_encoding_from_label(cs);
                        need_pragma = 1;
                    }
                } else if (hk_str_eq(name, HK_STR("charset"))) {
                    charset = hk_encoding_from_label(value);
                    need_pragma = 0;
                }
            }
            if (need_pragma == -1 || (need_pragma == 1 && !got_pragma) || charset == HK_ENC_UNKNOWN) {
                s.pos++;
                continue;
            }
            if (charset == HK_ENC_UTF16LE || charset == HK_ENC_UTF16BE) charset = HK_ENC_UTF8;
            return charset;
        }
        if (s.pos + 1 < s.n && s.p[s.pos] == '<' &&
            (is_letter(s.p[s.pos + 1]) || (s.p[s.pos + 1] == '/' && s.pos + 2 < s.n && is_letter(s.p[s.pos + 2])))) {
            while (s.pos < s.n && !is_ws(s.p[s.pos]) && s.p[s.pos] != '>') s.pos++;
            for (;;) {
                int r = get_attribute(&s);
                if (r < 0) return HK_ENC_UNKNOWN;
                if (r == 0) break;
            }
            s.pos++;
            continue;
        }
        if (s.pos + 1 < s.n && s.p[s.pos] == '<' &&
            (s.p[s.pos + 1] == '!' || s.p[s.pos + 1] == '/' || s.p[s.pos + 1] == '?')) {
            while (s.pos < s.n && s.p[s.pos] != '>') s.pos++;
        }
        s.pos++;
    }
    return HK_ENC_UNKNOWN;
}

/* --- fallback guess -------------------------------------------------------------------------- */

static bool valid_sjis(const uint8_t *p, size_t n, bool *saw_low_lead)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c < 0x80 || (c >= 0xA1 && c <= 0xDF)) continue;
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
            if (i + 1 >= n) return true; /* truncated sample */
            uint8_t t = p[++i];
            if (t < 0x40 || t == 0x7F || t > 0xFC) return false;
            if (c <= 0x9F) *saw_low_lead = true;
            continue;
        }
        return false;
    }
    return true;
}

static bool valid_eucjp(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c < 0x80) continue;
        size_t need = c == 0x8F ? 2 : 1;
        if (!(c == 0x8E || c == 0x8F || (c >= 0xA1 && c <= 0xFE))) return false;
        for (size_t k = 0; k < need; k++) {
            if (i + 1 >= n) return true;
            uint8_t t = p[++i];
            if (t < 0xA1 || t > 0xFE) return false;
        }
    }
    return true;
}

hk_encoding hk_encoding_guess(const uint8_t *p, size_t n, bool japanese_locale)
{
    hk_encoding fallback = japanese_locale ? HK_ENC_SHIFT_JIS : HK_ENC_WINDOWS_1252;
    bool ascii = true;
    for (size_t i = 0; i < n && ascii; i++) ascii = p[i] < 0x80;
    for (size_t i = 0; i + 2 < n; i++) {
        if (p[i] == 0x1B && ((p[i + 1] == '$' && (p[i + 2] == 'B' || p[i + 2] == '@')) ||
                             (p[i + 1] == '(' && p[i + 2] == 'J')))
            return HK_ENC_ISO_2022_JP;
    }
    if (ascii) return fallback;

    /* Valid UTF-8 (allowing a sequence cut off at the end of the sample). */
    size_t check = n;
    for (size_t k = 0; k < 3 && check > 0; k++) {
        if ((p[check - 1] & 0xC0) == 0x80) check--;
        else break;
    }
    if (check > 0 && p[check - 1] >= 0xC0) check--;
    if (hk_utf8_valid(p, check)) return HK_ENC_UTF8;

    bool low_lead = false;
    bool sjis = valid_sjis(p, n, &low_lead);
    bool euc = valid_eucjp(p, n);
    if (sjis && (!euc || low_lead)) return HK_ENC_SHIFT_JIS;
    if (euc) return HK_ENC_EUC_JP;
    return fallback;
}

hk_encoding hk_encoding_sniff(const uint8_t *p, size_t n, hk_encoding transport, bool japanese_locale,
                              size_t *bom_len)
{
    hk_encoding e = hk_encoding_from_bom(p, n, bom_len);
    if (e) return e;
    if (transport) return transport;
    e = hk_encoding_prescan(p, n);
    if (e) return e;
    return hk_encoding_guess(p, HK_MIN(n, (size_t)64 * 1024), japanese_locale);
}

/* --- decoders ---------------------------------------------------------------------------------- */

static const uint16_t k_1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

static void put_cp(hk_charbuf *out, uint32_t cp)
{
    uint8_t tmp[4];
    size_t n = hk_utf8_encode(cp, tmp);
    hk_charbuf_append(out, (const char *)tmp, n);
}

void hk_decoder_init(hk_decoder *d, hk_encoding enc)
{
    memset(d, 0, sizeof *d);
    d->enc = enc == HK_ENC_UNKNOWN ? HK_ENC_UTF8 : enc;
}

void hk_decoder_free(hk_decoder *d)
{
    hk_vec_free(&d->held);
}

static size_t utf8_seq_len(uint8_t b)
{
    if (b >= 0xC2 && b <= 0xDF) return 2;
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;
    return 1;
}

static void decode_utf8(hk_decoder *d, const uint8_t *p, size_t n, bool last, hk_charbuf *out)
{
    size_t i = 0;
    while (i < n) {
        size_t run = i;
        while (run < n && p[run] < 0x80) run++;
        if (run > i) {
            hk_charbuf_append(out, (const char *)p + i, run - i);
            i = run;
            continue;
        }
        uint32_t cp;
        size_t len = hk_utf8_decode(p + i, n - i, &cp);
        if (!last && i + len == n && len < utf8_seq_len(p[i]) && cp == HK_REPLACEMENT_CHAR) {
            /* Possibly a sequence split across chunks: keep it for the next call. */
            memcpy(d->pending, p + i, len);
            d->pending_len = (uint32_t)len;
            return;
        }
        put_cp(out, cp);
        i += len;
    }
}

static void decode_utf16(hk_decoder *d, const uint8_t *p, size_t n, bool last, hk_charbuf *out)
{
    bool be = d->enc == HK_ENC_UTF16BE;
    size_t i = 0;
    while (i + 1 < n) {
        uint32_t u = be ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 3 >= n) {
                if (!last) break;
                put_cp(out, HK_REPLACEMENT_CHAR);
                i += 2;
                continue;
            }
            uint32_t lo = be ? ((uint32_t)p[i + 2] << 8 | p[i + 3]) : ((uint32_t)p[i + 3] << 8 | p[i + 2]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                put_cp(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i += 4;
                continue;
            }
            put_cp(out, HK_REPLACEMENT_CHAR);
            i += 2;
            continue;
        }
        put_cp(out, (u >= 0xDC00 && u <= 0xDFFF) ? HK_REPLACEMENT_CHAR : u);
        i += 2;
    }
    if (i < n) {
        if (last) put_cp(out, HK_REPLACEMENT_CHAR);
        else {
            memcpy(d->pending, p + i, n - i);
            d->pending_len = (uint32_t)(n - i);
        }
    }
}

static void decode_codepage(hk_decoder *d, const uint8_t *p, size_t n, bool last, hk_charbuf *out)
{
    uint32_t cp = k_info[d->enc].codepage;
    size_t i = 0, end = n;
    if (!last) {
        /* Keep a trailing lead byte of a double-byte code page for the next chunk. */
        while (i < n) i += hk_plat_is_lead_byte(cp, p[i]) ? 2 : 1;
        if (i > n) end = n - 1;
    }
    hk_plat_decode_codepage(cp, p, end, out);
    if (end < n) {
        d->pending[0] = p[end];
        d->pending_len = 1;
    }
}

void hk_decoder_decode(hk_decoder *d, const uint8_t *p, size_t n, bool last, hk_charbuf *out)
{
    if (k_info[d->enc].whole_input) {
        hk_charbuf_append(&d->held, (const char *)p, n);
        if (last) {
            hk_plat_decode_codepage(k_info[d->enc].codepage, (const uint8_t *)d->held.data, d->held.len, out);
            d->held.len = 0;
        }
        return;
    }
    if (d->pending_len) {
        /* Prepend the bytes kept from the previous chunk (at most 3) and decode the joined data. */
        hk_charbuf joined = {0};
        hk_charbuf_append(&joined, (const char *)d->pending, d->pending_len);
        hk_charbuf_append(&joined, (const char *)p, n);
        d->pending_len = 0;
        hk_decoder_decode(d, (const uint8_t *)joined.data, joined.len, last, out);
        hk_vec_free(&joined);
        return;
    }
    switch (d->enc) {
    case HK_ENC_UTF8: decode_utf8(d, p, n, last, out); break;
    case HK_ENC_UTF16LE:
    case HK_ENC_UTF16BE: decode_utf16(d, p, n, last, out); break;
    case HK_ENC_WINDOWS_1252:
        for (size_t i = 0; i < n; i++) {
            uint8_t c = p[i];
            if (c < 0x80) hk_charbuf_push(out, (char)c);
            else put_cp(out, (c <= 0x9F) ? k_1252[c - 0x80] : c);
        }
        break;
    default: decode_codepage(d, p, n, last, out); break;
    }
}
