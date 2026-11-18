/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * State names and transitions follow WHATWG HTML 13.2.5 one to one, so the code
 * can be checked against the specification section by section. Parse errors are
 * not reported (Hawk recovers exactly like the specification and has no use for
 * the error list).
 *
 * Input bytes are UTF-8. Every state compares only ASCII, so non-ASCII bytes are
 * copied through unchanged and multi-byte sequences stay intact.
 */
#include "html/hk_html_tokenizer.h"
#include "base/hk_utf8.h"

#include <string.h>

typedef struct hk_entity {
    uint16_t off;
    uint8_t len;
    uint32_t cp1;
    uint16_t cp2;
} hk_entity;

#include "html/hk_html_entities.inc"

#define EOF_CH (-1)
#define FFFD "\xEF\xBF\xBD"

/* --- character classes ---------------------------------------------------------- */

static bool is_ws(int c)
{
    return c == '\t' || c == '\n' || c == '\f' || c == ' ';
}
static bool is_upper(int c)
{
    return c >= 'A' && c <= 'Z';
}
static bool is_lower(int c)
{
    return c >= 'a' && c <= 'z';
}
static bool is_alpha(int c)
{
    return is_upper(c) || is_lower(c);
}
static bool is_digit(int c)
{
    return c >= '0' && c <= '9';
}
static bool is_alnum(int c)
{
    return is_alpha(c) || is_digit(c);
}
static bool is_hex(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static char to_lower(int c)
{
    return (char)(is_upper(c) ? c + 0x20 : c);
}

/* --- buffers ---------------------------------------------------------------------- */

static void buf_utf8(hk_charbuf *b, uint32_t cp)
{
    uint8_t tmp[4];
    size_t n = hk_utf8_encode(cp, tmp);
    hk_charbuf_append(b, (const char *)tmp, n);
}

static hk_str buf_str(const hk_charbuf *b)
{
    return (hk_str){b->data, b->len};
}

static bool buf_eq(const hk_charbuf *b, const char *lit)
{
    size_t n = strlen(lit);
    return b->len == n && (n == 0 || memcmp(b->data, lit, n) == 0);
}

/* --- emitting ----------------------------------------------------------------------- */

static void emit_char(hk_html_tokenizer *t, int c)
{
    hk_charbuf_push(&t->text, (char)c);
}

static void emit_str(hk_html_tokenizer *t, const char *s)
{
    hk_charbuf_append(&t->text, s, strlen(s));
}

static void flush_text(hk_html_tokenizer *t)
{
    if (t->text.len == 0) return;
    hk_html_token tok = {0};
    tok.type = HK_TOK_CHARS;
    tok.data = buf_str(&t->text);
    t->emit(t->user, &tok);
    t->text.len = 0;
}

static void emit_token(hk_html_tokenizer *t, hk_html_token *tok)
{
    flush_text(t);
    t->emit(t->user, tok);
}

static void emit_tag(hk_html_tokenizer *t)
{
    hk_html_token tok = {0};
    tok.type = t->tag_type;
    tok.name = buf_str(&t->tag_name);
    tok.self_closing = t->self_closing;
    t->attr_out.len = 0;
    if (t->tag_type == HK_TOK_START_TAG) {
        for (uint32_t i = 0; i < t->attrs.len; i++) {
            const hk_html_tok_attr_span *s = &t->attrs.data[i];
            hk_html_tok_attr a = {{t->attr_buf.data + s->name_off, s->name_len},
                                  {t->attr_buf.data + s->value_off, s->value_len}};
            bool dup = false; /* duplicate attributes are dropped, the first one wins */
            for (uint32_t k = 0; k < t->attr_out.len && !dup; k++) {
                dup = hk_str_eq(t->attr_out.data[k].name, a.name);
            }
            if (!dup) hk_vec_push(&t->attr_out, a);
        }
        tok.attrs = t->attr_out.data;
        tok.attr_count = t->attr_out.len;
        t->last_start_tag.len = 0;
        hk_charbuf_append(&t->last_start_tag, t->tag_name.data, t->tag_name.len);
    }
    emit_token(t, &tok);
}

static void emit_comment(hk_html_tokenizer *t)
{
    hk_html_token tok = {0};
    tok.type = HK_TOK_COMMENT;
    tok.data = buf_str(&t->comment);
    emit_token(t, &tok);
}

static void emit_doctype(hk_html_tokenizer *t)
{
    hk_html_token tok = {0};
    tok.type = HK_TOK_DOCTYPE;
    tok.has_name = t->has_name;
    tok.name = buf_str(&t->doctype_name);
    tok.has_public = t->has_public;
    tok.public_id = buf_str(&t->public_id);
    tok.has_system = t->has_system;
    tok.system_id = buf_str(&t->system_id);
    tok.force_quirks = t->force_quirks;
    emit_token(t, &tok);
}

static void emit_eof(hk_html_tokenizer *t)
{
    hk_html_token tok = {0};
    tok.type = HK_TOK_EOF;
    emit_token(t, &tok);
    t->state = HK_TS_EOF_EMITTED;
}

/* --- token construction -------------------------------------------------------------- */

static void new_tag(hk_html_tokenizer *t, hk_html_token_type type)
{
    t->tag_type = type;
    t->self_closing = false;
    t->tag_name.len = 0;
    t->attr_buf.len = 0;
    t->attrs.len = 0;
}

static void new_attr(hk_html_tokenizer *t)
{
    hk_html_tok_attr_span s = {t->attr_buf.len, 0, t->attr_buf.len, 0};
    hk_vec_push(&t->attrs, s);
}

static hk_html_tok_attr_span *cur_attr(hk_html_tokenizer *t)
{
    return &t->attrs.data[t->attrs.len - 1];
}

static void attr_name_append(hk_html_tokenizer *t, const char *p, size_t n)
{
    hk_charbuf_append(&t->attr_buf, p, n);
    cur_attr(t)->name_len += (uint32_t)n;
}

static void attr_name_char(hk_html_tokenizer *t, int c)
{
    char ch = (char)c;
    attr_name_append(t, &ch, 1);
}

static void attr_value_begin(hk_html_tokenizer *t)
{
    cur_attr(t)->value_off = t->attr_buf.len;
    cur_attr(t)->value_len = 0;
}

static void attr_value_append(hk_html_tokenizer *t, const char *p, size_t n)
{
    hk_charbuf_append(&t->attr_buf, p, n);
    cur_attr(t)->value_len += (uint32_t)n;
}

static void attr_value_char(hk_html_tokenizer *t, int c)
{
    char ch = (char)c;
    attr_value_append(t, &ch, 1);
}

static void new_comment(hk_html_tokenizer *t)
{
    t->comment.len = 0;
}

static void new_doctype(hk_html_tokenizer *t)
{
    t->doctype_name.len = t->public_id.len = t->system_id.len = 0;
    t->has_name = t->has_public = t->has_system = t->force_quirks = false;
}

static bool appropriate_end_tag(const hk_html_tokenizer *t)
{
    return t->last_start_tag.len == t->tag_name.len &&
           (t->tag_name.len == 0 ||
            memcmp(t->last_start_tag.data, t->tag_name.data, t->tag_name.len) == 0);
}

/* --- character references ------------------------------------------------------------- */

static bool return_is_attr(const hk_html_tokenizer *t)
{
    return t->return_state == HK_TS_ATTR_VALUE_DQ || t->return_state == HK_TS_ATTR_VALUE_SQ ||
           t->return_state == HK_TS_ATTR_VALUE_UNQ;
}

static void flush_char_ref(hk_html_tokenizer *t)
{
    if (return_is_attr(t)) attr_value_append(t, t->temp.data, t->temp.len);
    else hk_charbuf_append(&t->text, t->temp.data, t->temp.len);
    t->temp.len = 0;
}

static int entity_cmp(const char *s, size_t n, const hk_entity *e)
{
    size_t m = HK_MIN(n, (size_t)e->len);
    int r = memcmp(s, k_entity_pool + e->off, m);
    if (r) return r;
    return n < e->len ? -1 : (n > e->len ? 1 : 0);
}

static const hk_entity *find_entity(const char *s, size_t n)
{
    size_t lo = 0, hi = HK_ENTITY_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = entity_cmp(s, n, &k_entities[mid]);
        if (r == 0) return &k_entities[mid];
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* C1 control replacements for numeric references (13.2.5.80). */
static uint32_t numeric_ref_replacement(uint32_t c)
{
    static const uint16_t k_c1[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    };
    if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0xFFFD;
    if (c >= 0x80 && c <= 0x9F) return k_c1[c - 0x80];
    return c;
}

static void numeric_ref_end(hk_html_tokenizer *t)
{
    t->temp.len = 0;
    buf_utf8(&t->temp, numeric_ref_replacement(t->char_ref_code));
    flush_char_ref(t);
    t->state = t->return_state;
}

/* --- lookahead -------------------------------------------------------------------------- */

typedef enum { LA_NO, LA_YES, LA_MORE } lookahead;

/* Does the unconsumed input start with lit? LA_MORE: not enough input yet to tell. */
static lookahead starts_with(const hk_html_tokenizer *t, const char *lit, bool icase)
{
    size_t n = strlen(lit), avail = t->input.len - t->pos;
    const char *p = t->input.data + t->pos;
    for (size_t i = 0; i < n; i++) {
        if (i >= avail) return t->eof ? LA_NO : LA_MORE;
        char c = icase ? to_lower((unsigned char)p[i]) : p[i];
        if (c != lit[i]) return LA_NO;
    }
    return LA_YES;
}

/* Returns false when more input is needed. */
static bool markup_declaration_open(hk_html_tokenizer *t)
{
    lookahead dash = starts_with(t, "--", false);
    lookahead doctype = starts_with(t, "doctype", true);
    lookahead cdata = starts_with(t, "[CDATA[", false);
    if (dash == LA_MORE || doctype == LA_MORE || cdata == LA_MORE) return false;
    if (dash == LA_YES) {
        t->pos += 2;
        new_comment(t);
        t->state = HK_TS_COMMENT_START;
    } else if (doctype == LA_YES) {
        t->pos += 7;
        t->state = HK_TS_DOCTYPE;
    } else if (cdata == LA_YES) {
        t->pos += 7;
        if (t->cdata_allowed) {
            t->state = HK_TS_CDATA_SECTION;
        } else {
            new_comment(t);
            hk_charbuf_append(&t->comment, "[CDATA[", 7);
            t->state = HK_TS_BOGUS_COMMENT;
        }
    } else {
        new_comment(t);
        t->state = HK_TS_BOGUS_COMMENT;
    }
    return true;
}

/* Named character reference state (13.2.5.73). Returns false when more input is needed. */
static bool named_char_ref(hk_html_tokenizer *t)
{
    const char *p = t->input.data + t->pos;
    size_t avail = t->input.len - t->pos, k = 0;
    while (k < avail && k < HK_ENTITY_MAX_NAME && is_alnum((unsigned char)p[k])) k++;
    if (k < avail && k < HK_ENTITY_MAX_NAME && p[k] == ';') k++;
    /* One character past the candidate decides the attribute special case. */
    if (k >= avail && !t->eof) return false;

    const hk_entity *e = NULL;
    size_t len = k;
    for (; len > 0 && !e; len--) e = find_entity(p, len);
    if (!e) {
        flush_char_ref(t); /* the temporary buffer holds "&" */
        t->state = HK_TS_AMBIGUOUS_AMPERSAND;
        return true;
    }
    len++; /* undo the final decrement */
    hk_charbuf_append(&t->temp, p, len);
    t->pos += len;
    bool semicolon = p[len - 1] == ';';
    if (return_is_attr(t) && !semicolon) {
        int next = t->pos < t->input.len ? (unsigned char)t->input.data[t->pos] : EOF_CH;
        if (next == '=' || is_alnum(next)) {
            flush_char_ref(t);
            t->state = t->return_state;
            return true;
        }
    }
    t->temp.len = 0;
    buf_utf8(&t->temp, e->cp1);
    if (e->cp2) buf_utf8(&t->temp, e->cp2);
    flush_char_ref(t);
    t->state = t->return_state;
    return true;
}

/* After DOCTYPE name: "PUBLIC" / "SYSTEM" keywords need six characters of lookahead. */
static bool after_doctype_name(hk_html_tokenizer *t)
{
    if (t->pos >= t->input.len) {
        if (!t->eof) return false;
        t->force_quirks = true;
        emit_doctype(t);
        emit_eof(t);
        return true;
    }
    int c = (unsigned char)t->input.data[t->pos];
    if (is_ws(c)) {
        t->pos++;
        return true;
    }
    if (c == '>') {
        t->pos++;
        t->state = HK_TS_DATA;
        emit_doctype(t);
        return true;
    }
    lookahead pub = starts_with(t, "public", true);
    lookahead sys = starts_with(t, "system", true);
    if (pub == LA_MORE || sys == LA_MORE) return false;
    if (pub == LA_YES) {
        t->pos += 6;
        t->state = HK_TS_AFTER_DOCTYPE_PUBLIC_KW;
    } else if (sys == LA_YES) {
        t->pos += 6;
        t->state = HK_TS_AFTER_DOCTYPE_SYSTEM_KW;
    } else {
        t->force_quirks = true;
        t->state = HK_TS_BOGUS_DOCTYPE; /* reconsume */
    }
    return true;
}

/* --- fast paths -------------------------------------------------------------------------- */

/* Length of the run at the cursor that contains none of the bytes in stop. */
static size_t run_until(const hk_html_tokenizer *t, const char *stop)
{
    const char *p = t->input.data + t->pos;
    size_t n = t->input.len - t->pos, i = 0;
    while (i < n && !strchr(stop, p[i])) i++;
    return i;
}

/* --- the state machine ------------------------------------------------------------------ */

#define RECONSUME(st)                                                                              \
    do {                                                                                           \
        if (c != EOF_CH) t->pos--;                                                                 \
        t->state = (st);                                                                           \
    } while (0)

/* Shared by the RCDATA / RAWTEXT / script data end tag name states. */
static void end_tag_name(hk_html_tokenizer *t, int c, hk_html_tok_state fallback)
{
    if ((is_ws(c) || c == '/' || c == '>') && appropriate_end_tag(t)) {
        if (is_ws(c)) t->state = HK_TS_BEFORE_ATTR_NAME;
        else if (c == '/') t->state = HK_TS_SELF_CLOSING_START_TAG;
        else {
            t->state = HK_TS_DATA;
            emit_tag(t);
        }
    } else if (is_alpha(c)) {
        hk_charbuf_push(&t->tag_name, to_lower(c));
        hk_charbuf_push(&t->temp, (char)c);
    } else {
        emit_str(t, "</");
        hk_charbuf_append(&t->text, t->temp.data, t->temp.len);
        RECONSUME(fallback);
    }
}

static void run(hk_html_tokenizer *t)
{
    for (;;) {
        if (t->state == HK_TS_EOF_EMITTED) return;

        /* States that look ahead without consuming. */
        if (t->state == HK_TS_MARKUP_DECL_OPEN) {
            if (!markup_declaration_open(t)) break;
            continue;
        }
        if (t->state == HK_TS_NAMED_CHAR_REF) {
            if (!named_char_ref(t)) break;
            continue;
        }
        if (t->state == HK_TS_AFTER_DOCTYPE_NAME) {
            if (!after_doctype_name(t)) break;
            continue;
        }

        if (t->pos >= t->input.len && !t->eof) break;

        /* Bulk copies for the common states. */
        size_t n = 0;
        switch (t->state) {
        case HK_TS_DATA: n = run_until(t, "<&"); break;
        case HK_TS_RCDATA: n = run_until(t, "<&"); break;
        case HK_TS_RAWTEXT:
        case HK_TS_SCRIPT_DATA: n = run_until(t, "<"); break;
        case HK_TS_PLAINTEXT: n = run_until(t, ""); break;
        default: break;
        }
        if (n) {
            hk_charbuf_append(&t->text, t->input.data + t->pos, n);
            t->pos += n;
            continue;
        }
        if (t->state == HK_TS_ATTR_VALUE_DQ || t->state == HK_TS_ATTR_VALUE_SQ) {
            n = run_until(t, t->state == HK_TS_ATTR_VALUE_DQ ? "\"&" : "'&");
            if (n) {
                attr_value_append(t, t->input.data + t->pos, n);
                t->pos += n;
                continue;
            }
        }

        int c = t->pos < t->input.len ? (unsigned char)t->input.data[t->pos++] : EOF_CH;

        switch (t->state) {
        case HK_TS_DATA:
            if (c == '&') {
                t->return_state = HK_TS_DATA;
                t->state = HK_TS_CHAR_REF;
            } else if (c == '<') t->state = HK_TS_TAG_OPEN;
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c); /* U+0000 is emitted as is in the data state */
            break;

        case HK_TS_RCDATA:
            if (c == '&') {
                t->return_state = HK_TS_RCDATA;
                t->state = HK_TS_CHAR_REF;
            } else if (c == '<') t->state = HK_TS_RCDATA_LT;
            else if (c == 0) emit_str(t, FFFD);
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c);
            break;

        case HK_TS_RAWTEXT:
            if (c == '<') t->state = HK_TS_RAWTEXT_LT;
            else if (c == 0) emit_str(t, FFFD);
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c);
            break;

        case HK_TS_SCRIPT_DATA:
            if (c == '<') t->state = HK_TS_SCRIPT_LT;
            else if (c == 0) emit_str(t, FFFD);
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c);
            break;

        case HK_TS_PLAINTEXT:
            if (c == 0) emit_str(t, FFFD);
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c);
            break;

        case HK_TS_TAG_OPEN:
            if (c == '!') t->state = HK_TS_MARKUP_DECL_OPEN;
            else if (c == '/') t->state = HK_TS_END_TAG_OPEN;
            else if (is_alpha(c)) {
                new_tag(t, HK_TOK_START_TAG);
                RECONSUME(HK_TS_TAG_NAME);
            } else if (c == '?') {
                new_comment(t);
                RECONSUME(HK_TS_BOGUS_COMMENT);
            } else if (c == EOF_CH) {
                emit_char(t, '<');
                emit_eof(t);
            } else {
                emit_char(t, '<');
                RECONSUME(HK_TS_DATA);
            }
            break;

        case HK_TS_END_TAG_OPEN:
            if (is_alpha(c)) {
                new_tag(t, HK_TOK_END_TAG);
                RECONSUME(HK_TS_TAG_NAME);
            } else if (c == '>') t->state = HK_TS_DATA;
            else if (c == EOF_CH) {
                emit_str(t, "</");
                emit_eof(t);
            } else {
                new_comment(t);
                RECONSUME(HK_TS_BOGUS_COMMENT);
            }
            break;

        case HK_TS_TAG_NAME:
            if (is_ws(c)) t->state = HK_TS_BEFORE_ATTR_NAME;
            else if (c == '/') t->state = HK_TS_SELF_CLOSING_START_TAG;
            else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_tag(t);
            } else if (c == 0) hk_charbuf_append(&t->tag_name, FFFD, 3);
            else if (c == EOF_CH) emit_eof(t);
            else hk_charbuf_push(&t->tag_name, to_lower(c));
            break;

        case HK_TS_RCDATA_LT:
            if (c == '/') {
                t->temp.len = 0;
                t->state = HK_TS_RCDATA_END_TAG_OPEN;
            } else {
                emit_char(t, '<');
                RECONSUME(HK_TS_RCDATA);
            }
            break;

        case HK_TS_RCDATA_END_TAG_OPEN:
            if (is_alpha(c)) {
                new_tag(t, HK_TOK_END_TAG);
                RECONSUME(HK_TS_RCDATA_END_TAG_NAME);
            } else {
                emit_str(t, "</");
                RECONSUME(HK_TS_RCDATA);
            }
            break;

        case HK_TS_RCDATA_END_TAG_NAME: end_tag_name(t, c, HK_TS_RCDATA); break;

        case HK_TS_RAWTEXT_LT:
            if (c == '/') {
                t->temp.len = 0;
                t->state = HK_TS_RAWTEXT_END_TAG_OPEN;
            } else {
                emit_char(t, '<');
                RECONSUME(HK_TS_RAWTEXT);
            }
            break;

        case HK_TS_RAWTEXT_END_TAG_OPEN:
            if (is_alpha(c)) {
                new_tag(t, HK_TOK_END_TAG);
                RECONSUME(HK_TS_RAWTEXT_END_TAG_NAME);
            } else {
                emit_str(t, "</");
                RECONSUME(HK_TS_RAWTEXT);
            }
            break;

        case HK_TS_RAWTEXT_END_TAG_NAME: end_tag_name(t, c, HK_TS_RAWTEXT); break;

        case HK_TS_SCRIPT_LT:
            if (c == '/') {
                t->temp.len = 0;
                t->state = HK_TS_SCRIPT_END_TAG_OPEN;
            } else if (c == '!') {
                t->state = HK_TS_SCRIPT_ESCAPE_START;
                emit_str(t, "<!");
            } else {
                emit_char(t, '<');
                RECONSUME(HK_TS_SCRIPT_DATA);
            }
            break;

        case HK_TS_SCRIPT_END_TAG_OPEN:
            if (is_alpha(c)) {
                new_tag(t, HK_TOK_END_TAG);
                RECONSUME(HK_TS_SCRIPT_END_TAG_NAME);
            } else {
                emit_str(t, "</");
                RECONSUME(HK_TS_SCRIPT_DATA);
            }
            break;

        case HK_TS_SCRIPT_END_TAG_NAME: end_tag_name(t, c, HK_TS_SCRIPT_DATA); break;

        case HK_TS_SCRIPT_ESCAPE_START:
            if (c == '-') {
                t->state = HK_TS_SCRIPT_ESCAPE_START_DASH;
                emit_char(t, '-');
            } else RECONSUME(HK_TS_SCRIPT_DATA);
            break;

        case HK_TS_SCRIPT_ESCAPE_START_DASH:
            if (c == '-') {
                t->state = HK_TS_SCRIPT_ESCAPED_DASH_DASH;
                emit_char(t, '-');
            } else RECONSUME(HK_TS_SCRIPT_DATA);
            break;

        case HK_TS_SCRIPT_ESCAPED:
            if (c == '-') {
                t->state = HK_TS_SCRIPT_ESCAPED_DASH;
                emit_char(t, '-');
            } else if (c == '<') t->state = HK_TS_SCRIPT_ESCAPED_LT;
            else if (c == 0) emit_str(t, FFFD);
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c);
            break;

        case HK_TS_SCRIPT_ESCAPED_DASH:
            if (c == '-') {
                t->state = HK_TS_SCRIPT_ESCAPED_DASH_DASH;
                emit_char(t, '-');
            } else if (c == '<') t->state = HK_TS_SCRIPT_ESCAPED_LT;
            else if (c == 0) {
                t->state = HK_TS_SCRIPT_ESCAPED;
                emit_str(t, FFFD);
            } else if (c == EOF_CH) emit_eof(t);
            else {
                t->state = HK_TS_SCRIPT_ESCAPED;
                emit_char(t, c);
            }
            break;

        case HK_TS_SCRIPT_ESCAPED_DASH_DASH:
            if (c == '-') emit_char(t, '-');
            else if (c == '<') t->state = HK_TS_SCRIPT_ESCAPED_LT;
            else if (c == '>') {
                t->state = HK_TS_SCRIPT_DATA;
                emit_char(t, '>');
            } else if (c == 0) {
                t->state = HK_TS_SCRIPT_ESCAPED;
                emit_str(t, FFFD);
            } else if (c == EOF_CH) emit_eof(t);
            else {
                t->state = HK_TS_SCRIPT_ESCAPED;
                emit_char(t, c);
            }
            break;

        case HK_TS_SCRIPT_ESCAPED_LT:
            if (c == '/') {
                t->temp.len = 0;
                t->state = HK_TS_SCRIPT_ESCAPED_END_TAG_OPEN;
            } else if (is_alpha(c)) {
                t->temp.len = 0;
                emit_char(t, '<');
                RECONSUME(HK_TS_SCRIPT_DOUBLE_ESCAPE_START);
            } else {
                emit_char(t, '<');
                RECONSUME(HK_TS_SCRIPT_ESCAPED);
            }
            break;

        case HK_TS_SCRIPT_ESCAPED_END_TAG_OPEN:
            if (is_alpha(c)) {
                new_tag(t, HK_TOK_END_TAG);
                RECONSUME(HK_TS_SCRIPT_ESCAPED_END_TAG_NAME);
            } else {
                emit_str(t, "</");
                RECONSUME(HK_TS_SCRIPT_ESCAPED);
            }
            break;

        case HK_TS_SCRIPT_ESCAPED_END_TAG_NAME: end_tag_name(t, c, HK_TS_SCRIPT_ESCAPED); break;

        case HK_TS_SCRIPT_DOUBLE_ESCAPE_START:
            if (is_ws(c) || c == '/' || c == '>') {
                t->state = buf_eq(&t->temp, "script") ? HK_TS_SCRIPT_DOUBLE_ESCAPED
                                                      : HK_TS_SCRIPT_ESCAPED;
                emit_char(t, c);
            } else if (is_alpha(c)) {
                hk_charbuf_push(&t->temp, to_lower(c));
                emit_char(t, c);
            } else RECONSUME(HK_TS_SCRIPT_ESCAPED);
            break;

        case HK_TS_SCRIPT_DOUBLE_ESCAPED:
            if (c == '-') {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED_DASH;
                emit_char(t, '-');
            } else if (c == '<') {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED_LT;
                emit_char(t, '<');
            } else if (c == 0) emit_str(t, FFFD);
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c);
            break;

        case HK_TS_SCRIPT_DOUBLE_ESCAPED_DASH:
            if (c == '-') {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED_DASH_DASH;
                emit_char(t, '-');
            } else if (c == '<') {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED_LT;
                emit_char(t, '<');
            } else if (c == 0) {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED;
                emit_str(t, FFFD);
            } else if (c == EOF_CH) emit_eof(t);
            else {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED;
                emit_char(t, c);
            }
            break;

        case HK_TS_SCRIPT_DOUBLE_ESCAPED_DASH_DASH:
            if (c == '-') emit_char(t, '-');
            else if (c == '<') {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED_LT;
                emit_char(t, '<');
            } else if (c == '>') {
                t->state = HK_TS_SCRIPT_DATA;
                emit_char(t, '>');
            } else if (c == 0) {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED;
                emit_str(t, FFFD);
            } else if (c == EOF_CH) emit_eof(t);
            else {
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPED;
                emit_char(t, c);
            }
            break;

        case HK_TS_SCRIPT_DOUBLE_ESCAPED_LT:
            if (c == '/') {
                t->temp.len = 0;
                t->state = HK_TS_SCRIPT_DOUBLE_ESCAPE_END;
                emit_char(t, '/');
            } else RECONSUME(HK_TS_SCRIPT_DOUBLE_ESCAPED);
            break;

        case HK_TS_SCRIPT_DOUBLE_ESCAPE_END:
            if (is_ws(c) || c == '/' || c == '>') {
                t->state = buf_eq(&t->temp, "script") ? HK_TS_SCRIPT_ESCAPED
                                                      : HK_TS_SCRIPT_DOUBLE_ESCAPED;
                emit_char(t, c);
            } else if (is_alpha(c)) {
                hk_charbuf_push(&t->temp, to_lower(c));
                emit_char(t, c);
            } else RECONSUME(HK_TS_SCRIPT_DOUBLE_ESCAPED);
            break;

        case HK_TS_BEFORE_ATTR_NAME:
            if (is_ws(c)) break;
            if (c == '/' || c == '>' || c == EOF_CH) RECONSUME(HK_TS_AFTER_ATTR_NAME);
            else if (c == '=') {
                new_attr(t);
                attr_name_char(t, c);
                t->state = HK_TS_ATTR_NAME;
            } else {
                new_attr(t);
                RECONSUME(HK_TS_ATTR_NAME);
            }
            break;

        case HK_TS_ATTR_NAME:
            if (is_ws(c) || c == '/' || c == '>' || c == EOF_CH) RECONSUME(HK_TS_AFTER_ATTR_NAME);
            else if (c == '=') {
                attr_value_begin(t);
                t->state = HK_TS_BEFORE_ATTR_VALUE;
            } else if (c == 0) attr_name_append(t, FFFD, 3);
            else attr_name_char(t, to_lower(c));
            break;

        case HK_TS_AFTER_ATTR_NAME:
            if (is_ws(c)) break;
            if (c == '/') t->state = HK_TS_SELF_CLOSING_START_TAG;
            else if (c == '=') {
                attr_value_begin(t);
                t->state = HK_TS_BEFORE_ATTR_VALUE;
            } else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_tag(t);
            } else if (c == EOF_CH) emit_eof(t);
            else {
                new_attr(t);
                RECONSUME(HK_TS_ATTR_NAME);
            }
            break;

        case HK_TS_BEFORE_ATTR_VALUE:
            if (is_ws(c)) break;
            if (c == '"') t->state = HK_TS_ATTR_VALUE_DQ;
            else if (c == '\'') t->state = HK_TS_ATTR_VALUE_SQ;
            else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_tag(t);
            } else RECONSUME(HK_TS_ATTR_VALUE_UNQ);
            break;

        case HK_TS_ATTR_VALUE_DQ:
        case HK_TS_ATTR_VALUE_SQ:
            if (c == (t->state == HK_TS_ATTR_VALUE_DQ ? '"' : '\'')) {
                t->state = HK_TS_AFTER_ATTR_VALUE_QUOTED;
            } else if (c == '&') {
                t->return_state = t->state;
                t->state = HK_TS_CHAR_REF;
            } else if (c == 0) attr_value_append(t, FFFD, 3);
            else if (c == EOF_CH) emit_eof(t);
            else attr_value_char(t, c);
            break;

        case HK_TS_ATTR_VALUE_UNQ:
            if (is_ws(c)) t->state = HK_TS_BEFORE_ATTR_NAME;
            else if (c == '&') {
                t->return_state = HK_TS_ATTR_VALUE_UNQ;
                t->state = HK_TS_CHAR_REF;
            } else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_tag(t);
            } else if (c == 0) attr_value_append(t, FFFD, 3);
            else if (c == EOF_CH) emit_eof(t);
            else attr_value_char(t, c);
            break;

        case HK_TS_AFTER_ATTR_VALUE_QUOTED:
            if (is_ws(c)) t->state = HK_TS_BEFORE_ATTR_NAME;
            else if (c == '/') t->state = HK_TS_SELF_CLOSING_START_TAG;
            else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_tag(t);
            } else if (c == EOF_CH) emit_eof(t);
            else RECONSUME(HK_TS_BEFORE_ATTR_NAME);
            break;

        case HK_TS_SELF_CLOSING_START_TAG:
            if (c == '>') {
                t->self_closing = true;
                t->state = HK_TS_DATA;
                emit_tag(t);
            } else if (c == EOF_CH) emit_eof(t);
            else RECONSUME(HK_TS_BEFORE_ATTR_NAME);
            break;

        case HK_TS_BOGUS_COMMENT:
            if (c == '>') {
                t->state = HK_TS_DATA;
                emit_comment(t);
            } else if (c == EOF_CH) {
                emit_comment(t);
                emit_eof(t);
            } else if (c == 0) hk_charbuf_append(&t->comment, FFFD, 3);
            else hk_charbuf_push(&t->comment, (char)c);
            break;

        case HK_TS_COMMENT_START:
            if (c == '-') t->state = HK_TS_COMMENT_START_DASH;
            else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_comment(t);
            } else RECONSUME(HK_TS_COMMENT);
            break;

        case HK_TS_COMMENT_START_DASH:
            if (c == '-') t->state = HK_TS_COMMENT_END;
            else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_comment(t);
            } else if (c == EOF_CH) {
                emit_comment(t);
                emit_eof(t);
            } else {
                hk_charbuf_push(&t->comment, '-');
                RECONSUME(HK_TS_COMMENT);
            }
            break;

        case HK_TS_COMMENT:
            if (c == '<') {
                hk_charbuf_push(&t->comment, '<');
                t->state = HK_TS_COMMENT_LT;
            } else if (c == '-') t->state = HK_TS_COMMENT_END_DASH;
            else if (c == 0) hk_charbuf_append(&t->comment, FFFD, 3);
            else if (c == EOF_CH) {
                emit_comment(t);
                emit_eof(t);
            } else hk_charbuf_push(&t->comment, (char)c);
            break;

        case HK_TS_COMMENT_LT:
            if (c == '!') {
                hk_charbuf_push(&t->comment, '!');
                t->state = HK_TS_COMMENT_LT_BANG;
            } else if (c == '<') hk_charbuf_push(&t->comment, '<');
            else RECONSUME(HK_TS_COMMENT);
            break;

        case HK_TS_COMMENT_LT_BANG:
            if (c == '-') t->state = HK_TS_COMMENT_LT_BANG_DASH;
            else RECONSUME(HK_TS_COMMENT);
            break;

        case HK_TS_COMMENT_LT_BANG_DASH:
            if (c == '-') t->state = HK_TS_COMMENT_LT_BANG_DASH_DASH;
            else RECONSUME(HK_TS_COMMENT_END_DASH);
            break;

        case HK_TS_COMMENT_LT_BANG_DASH_DASH: RECONSUME(HK_TS_COMMENT_END); break;

        case HK_TS_COMMENT_END_DASH:
            if (c == '-') t->state = HK_TS_COMMENT_END;
            else if (c == EOF_CH) {
                emit_comment(t);
                emit_eof(t);
            } else {
                hk_charbuf_push(&t->comment, '-');
                RECONSUME(HK_TS_COMMENT);
            }
            break;

        case HK_TS_COMMENT_END:
            if (c == '>') {
                t->state = HK_TS_DATA;
                emit_comment(t);
            } else if (c == '!') t->state = HK_TS_COMMENT_END_BANG;
            else if (c == '-') hk_charbuf_push(&t->comment, '-');
            else if (c == EOF_CH) {
                emit_comment(t);
                emit_eof(t);
            } else {
                hk_charbuf_append(&t->comment, "--", 2);
                RECONSUME(HK_TS_COMMENT);
            }
            break;

        case HK_TS_COMMENT_END_BANG:
            if (c == '-') {
                hk_charbuf_append(&t->comment, "--!", 3);
                t->state = HK_TS_COMMENT_END_DASH;
            } else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_comment(t);
            } else if (c == EOF_CH) {
                emit_comment(t);
                emit_eof(t);
            } else {
                hk_charbuf_append(&t->comment, "--!", 3);
                RECONSUME(HK_TS_COMMENT);
            }
            break;

        case HK_TS_DOCTYPE:
            if (is_ws(c)) t->state = HK_TS_BEFORE_DOCTYPE_NAME;
            else if (c == EOF_CH) {
                new_doctype(t);
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else RECONSUME(HK_TS_BEFORE_DOCTYPE_NAME);
            break;

        case HK_TS_BEFORE_DOCTYPE_NAME:
            if (is_ws(c)) break;
            new_doctype(t);
            if (c == '>') {
                t->force_quirks = true;
                t->state = HK_TS_DATA;
                emit_doctype(t);
            } else if (c == EOF_CH) {
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else {
                t->has_name = true;
                if (c == 0) hk_charbuf_append(&t->doctype_name, FFFD, 3);
                else hk_charbuf_push(&t->doctype_name, to_lower(c));
                t->state = HK_TS_DOCTYPE_NAME;
            }
            break;

        case HK_TS_DOCTYPE_NAME:
            if (is_ws(c)) t->state = HK_TS_AFTER_DOCTYPE_NAME;
            else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_doctype(t);
            } else if (c == 0) hk_charbuf_append(&t->doctype_name, FFFD, 3);
            else if (c == EOF_CH) {
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else hk_charbuf_push(&t->doctype_name, to_lower(c));
            break;

        case HK_TS_AFTER_DOCTYPE_PUBLIC_KW:
        case HK_TS_AFTER_DOCTYPE_SYSTEM_KW: {
            bool pub = t->state == HK_TS_AFTER_DOCTYPE_PUBLIC_KW;
            if (is_ws(c)) {
                t->state = pub ? HK_TS_BEFORE_DOCTYPE_PUBLIC_ID : HK_TS_BEFORE_DOCTYPE_SYSTEM_ID;
                break;
            }
            /* Missing whitespace is a parse error but otherwise behaves like the "before" state. */
            t->state = pub ? HK_TS_BEFORE_DOCTYPE_PUBLIC_ID : HK_TS_BEFORE_DOCTYPE_SYSTEM_ID;
            RECONSUME(t->state);
            break;
        }

        case HK_TS_BEFORE_DOCTYPE_PUBLIC_ID:
        case HK_TS_BEFORE_DOCTYPE_SYSTEM_ID: {
            bool pub = t->state == HK_TS_BEFORE_DOCTYPE_PUBLIC_ID;
            if (is_ws(c)) break;
            if (c == '"' || c == '\'') {
                if (pub) {
                    t->has_public = true;
                    t->public_id.len = 0;
                    t->state = c == '"' ? HK_TS_DOCTYPE_PUBLIC_ID_DQ : HK_TS_DOCTYPE_PUBLIC_ID_SQ;
                } else {
                    t->has_system = true;
                    t->system_id.len = 0;
                    t->state = c == '"' ? HK_TS_DOCTYPE_SYSTEM_ID_DQ : HK_TS_DOCTYPE_SYSTEM_ID_SQ;
                }
            } else if (c == '>') {
                t->force_quirks = true;
                t->state = HK_TS_DATA;
                emit_doctype(t);
            } else if (c == EOF_CH) {
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else {
                t->force_quirks = true;
                RECONSUME(HK_TS_BOGUS_DOCTYPE);
            }
            break;
        }

        case HK_TS_DOCTYPE_PUBLIC_ID_DQ:
        case HK_TS_DOCTYPE_PUBLIC_ID_SQ:
        case HK_TS_DOCTYPE_SYSTEM_ID_DQ:
        case HK_TS_DOCTYPE_SYSTEM_ID_SQ: {
            bool pub = t->state == HK_TS_DOCTYPE_PUBLIC_ID_DQ || t->state == HK_TS_DOCTYPE_PUBLIC_ID_SQ;
            bool dq = t->state == HK_TS_DOCTYPE_PUBLIC_ID_DQ || t->state == HK_TS_DOCTYPE_SYSTEM_ID_DQ;
            hk_charbuf *b = pub ? &t->public_id : &t->system_id;
            if (c == (dq ? '"' : '\'')) {
                t->state = pub ? HK_TS_AFTER_DOCTYPE_PUBLIC_ID : HK_TS_AFTER_DOCTYPE_SYSTEM_ID;
            } else if (c == 0) hk_charbuf_append(b, FFFD, 3);
            else if (c == '>') {
                t->force_quirks = true;
                t->state = HK_TS_DATA;
                emit_doctype(t);
            } else if (c == EOF_CH) {
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else hk_charbuf_push(b, (char)c);
            break;
        }

        case HK_TS_AFTER_DOCTYPE_PUBLIC_ID:
        case HK_TS_BETWEEN_DOCTYPE_PUBLIC_SYSTEM:
            if (is_ws(c)) {
                t->state = HK_TS_BETWEEN_DOCTYPE_PUBLIC_SYSTEM;
            } else if (c == '>') {
                t->state = HK_TS_DATA;
                emit_doctype(t);
            } else if (c == '"' || c == '\'') {
                t->has_system = true;
                t->system_id.len = 0;
                t->state = c == '"' ? HK_TS_DOCTYPE_SYSTEM_ID_DQ : HK_TS_DOCTYPE_SYSTEM_ID_SQ;
            } else if (c == EOF_CH) {
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else {
                t->force_quirks = true;
                RECONSUME(HK_TS_BOGUS_DOCTYPE);
            }
            break;

        case HK_TS_AFTER_DOCTYPE_SYSTEM_ID:
            if (is_ws(c)) break;
            if (c == '>') {
                t->state = HK_TS_DATA;
                emit_doctype(t);
            } else if (c == EOF_CH) {
                t->force_quirks = true;
                emit_doctype(t);
                emit_eof(t);
            } else RECONSUME(HK_TS_BOGUS_DOCTYPE); /* does not set force-quirks */
            break;

        case HK_TS_BOGUS_DOCTYPE:
            if (c == '>') {
                t->state = HK_TS_DATA;
                emit_doctype(t);
            } else if (c == EOF_CH) {
                emit_doctype(t);
                emit_eof(t);
            }
            break;

        case HK_TS_CDATA_SECTION:
            if (c == ']') t->state = HK_TS_CDATA_SECTION_BRACKET;
            else if (c == EOF_CH) emit_eof(t);
            else emit_char(t, c);
            break;

        case HK_TS_CDATA_SECTION_BRACKET:
            if (c == ']') t->state = HK_TS_CDATA_SECTION_END;
            else {
                emit_char(t, ']');
                RECONSUME(HK_TS_CDATA_SECTION);
            }
            break;

        case HK_TS_CDATA_SECTION_END:
            if (c == ']') emit_char(t, ']');
            else if (c == '>') t->state = HK_TS_DATA;
            else {
                emit_str(t, "]]");
                RECONSUME(HK_TS_CDATA_SECTION);
            }
            break;

        case HK_TS_CHAR_REF:
            t->temp.len = 0;
            hk_charbuf_push(&t->temp, '&');
            if (is_alnum(c)) RECONSUME(HK_TS_NAMED_CHAR_REF);
            else if (c == '#') {
                hk_charbuf_push(&t->temp, '#');
                t->state = HK_TS_NUMERIC_CHAR_REF;
            } else {
                flush_char_ref(t);
                RECONSUME(t->return_state);
            }
            break;

        case HK_TS_AMBIGUOUS_AMPERSAND:
            if (is_alnum(c)) {
                if (return_is_attr(t)) attr_value_char(t, c);
                else emit_char(t, c);
            } else RECONSUME(t->return_state);
            break;

        case HK_TS_NUMERIC_CHAR_REF:
            t->char_ref_code = 0;
            if (c == 'x' || c == 'X') {
                hk_charbuf_push(&t->temp, (char)c);
                t->state = HK_TS_HEX_CHAR_REF_START;
            } else RECONSUME(HK_TS_DEC_CHAR_REF_START);
            break;

        case HK_TS_HEX_CHAR_REF_START:
        case HK_TS_DEC_CHAR_REF_START: {
            bool hex = t->state == HK_TS_HEX_CHAR_REF_START;
            if (hex ? is_hex(c) : is_digit(c)) {
                RECONSUME(hex ? HK_TS_HEX_CHAR_REF : HK_TS_DEC_CHAR_REF);
            } else {
                flush_char_ref(t);
                RECONSUME(t->return_state);
            }
            break;
        }

        case HK_TS_HEX_CHAR_REF:
        case HK_TS_DEC_CHAR_REF: {
            bool hex = t->state == HK_TS_HEX_CHAR_REF;
            if (hex ? is_hex(c) : is_digit(c)) {
                uint32_t v = is_digit(c) ? (uint32_t)(c - '0') : (uint32_t)(to_lower(c) - 'a' + 10);
                /* Saturate: any value above U+10FFFF maps to U+FFFD anyway. */
                if (t->char_ref_code <= 0x10FFFF) t->char_ref_code = t->char_ref_code * (hex ? 16 : 10) + v;
            } else if (c == ';') {
                numeric_ref_end(t);
            } else {
                RECONSUME(t->state); /* keep the position for the return state */
                numeric_ref_end(t);
            }
            break;
        }

        case HK_TS_MARKUP_DECL_OPEN:
        case HK_TS_NAMED_CHAR_REF:
        case HK_TS_AFTER_DOCTYPE_NAME:
        case HK_TS_EOF_EMITTED:
            hk_panic(__FILE__, __LINE__, "tokenizer state handled before the switch");
        }
    }
    /* Deliver pending characters so the tree builder can work incrementally. */
    flush_text(t);
}

/* --- public API ----------------------------------------------------------------------------- */

void hk_html_tokenizer_init(hk_html_tokenizer *t, hk_html_token_fn emit, void *user)
{
    memset(t, 0, sizeof *t);
    t->emit = emit;
    t->user = user;
    t->state = HK_TS_DATA;
}

void hk_html_tokenizer_free(hk_html_tokenizer *t)
{
    hk_vec_free(&t->input);
    hk_vec_free(&t->tag_name);
    hk_vec_free(&t->attr_buf);
    hk_vec_free(&t->attrs);
    hk_vec_free(&t->attr_out);
    hk_vec_free(&t->comment);
    hk_vec_free(&t->doctype_name);
    hk_vec_free(&t->public_id);
    hk_vec_free(&t->system_id);
    hk_vec_free(&t->temp);
    hk_vec_free(&t->text);
    hk_vec_free(&t->last_start_tag);
}

/* Drops consumed input so long documents do not keep their whole source in memory. */
static void compact_input(hk_html_tokenizer *t)
{
    if (t->pos < 64 * 1024 || t->pos < t->input.len / 2) return;
    size_t rest = t->input.len - t->pos;
    memmove(t->input.data, t->input.data + t->pos, rest);
    t->input.len = (uint32_t)rest;
    t->pos = 0;
}

void hk_html_tokenizer_feed(hk_html_tokenizer *t, const char *p, size_t n)
{
    HK_ASSERT(!t->eof);
    compact_input(t);
    hk_vec_reserve(&t->input, (uint64_t)t->input.len + n);
    /* Input stream preprocessing: CRLF and lone CR become LF. */
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\r') {
            t->input.data[t->input.len++] = '\n';
            t->last_was_cr = true;
            continue;
        }
        if (c == '\n' && t->last_was_cr) {
            t->last_was_cr = false;
            continue;
        }
        t->last_was_cr = false;
        t->input.data[t->input.len++] = c;
    }
    run(t);
}

void hk_html_tokenizer_finish(hk_html_tokenizer *t)
{
    t->eof = true;
    run(t);
}

void hk_html_tokenizer_set_state(hk_html_tokenizer *t, hk_html_tok_state s)
{
    t->state = s;
}

void hk_html_tokenizer_set_last_start_tag(hk_html_tokenizer *t, hk_str name)
{
    t->last_start_tag.len = 0;
    hk_charbuf_append(&t->last_start_tag, name.p, name.n);
}
