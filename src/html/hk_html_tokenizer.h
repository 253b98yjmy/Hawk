/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HTML tokenizer (WHATWG HTML, "Tokenization"). Input is UTF-8 and may arrive in
 * chunks; the tokenizer suspends at the end of the available input and resumes
 * when more is fed, so feeding byte by byte yields the same tokens.
 *
 * Tokens are delivered synchronously to a callback. The tree builder may change
 * the tokenizer state (RCDATA, RAWTEXT, ...) from inside the callback, as the
 * specification requires.
 */
#ifndef HK_HTML_TOKENIZER_H
#define HK_HTML_TOKENIZER_H

#include "base/hk_common.h"
#include "base/hk_str.h"
#include "base/hk_vec.h"

typedef enum hk_html_token_type {
    HK_TOK_DOCTYPE,
    HK_TOK_START_TAG,
    HK_TOK_END_TAG,
    HK_TOK_COMMENT,
    HK_TOK_CHARS, /* a run of characters; may contain U+0000 in the data state */
    HK_TOK_EOF,
} hk_html_token_type;

typedef struct hk_html_tok_attr {
    hk_str name; /* lowercase */
    hk_str value;
} hk_html_tok_attr;

/* All strings point into tokenizer buffers and are valid only during the callback. */
typedef struct hk_html_token {
    hk_html_token_type type;
    hk_str name; /* tag name (lowercase) or DOCTYPE name */
    hk_html_tok_attr *attrs;
    uint32_t attr_count;
    bool self_closing;
    /* DOCTYPE */
    bool has_name, has_public, has_system, force_quirks;
    hk_str public_id, system_id;
    /* comment or characters */
    hk_str data;
} hk_html_token;

typedef enum hk_html_tok_state {
    HK_TS_DATA,
    HK_TS_RCDATA,
    HK_TS_RAWTEXT,
    HK_TS_SCRIPT_DATA,
    HK_TS_PLAINTEXT,
    HK_TS_TAG_OPEN,
    HK_TS_END_TAG_OPEN,
    HK_TS_TAG_NAME,
    HK_TS_RCDATA_LT,
    HK_TS_RCDATA_END_TAG_OPEN,
    HK_TS_RCDATA_END_TAG_NAME,
    HK_TS_RAWTEXT_LT,
    HK_TS_RAWTEXT_END_TAG_OPEN,
    HK_TS_RAWTEXT_END_TAG_NAME,
    HK_TS_SCRIPT_LT,
    HK_TS_SCRIPT_END_TAG_OPEN,
    HK_TS_SCRIPT_END_TAG_NAME,
    HK_TS_SCRIPT_ESCAPE_START,
    HK_TS_SCRIPT_ESCAPE_START_DASH,
    HK_TS_SCRIPT_ESCAPED,
    HK_TS_SCRIPT_ESCAPED_DASH,
    HK_TS_SCRIPT_ESCAPED_DASH_DASH,
    HK_TS_SCRIPT_ESCAPED_LT,
    HK_TS_SCRIPT_ESCAPED_END_TAG_OPEN,
    HK_TS_SCRIPT_ESCAPED_END_TAG_NAME,
    HK_TS_SCRIPT_DOUBLE_ESCAPE_START,
    HK_TS_SCRIPT_DOUBLE_ESCAPED,
    HK_TS_SCRIPT_DOUBLE_ESCAPED_DASH,
    HK_TS_SCRIPT_DOUBLE_ESCAPED_DASH_DASH,
    HK_TS_SCRIPT_DOUBLE_ESCAPED_LT,
    HK_TS_SCRIPT_DOUBLE_ESCAPE_END,
    HK_TS_BEFORE_ATTR_NAME,
    HK_TS_ATTR_NAME,
    HK_TS_AFTER_ATTR_NAME,
    HK_TS_BEFORE_ATTR_VALUE,
    HK_TS_ATTR_VALUE_DQ,
    HK_TS_ATTR_VALUE_SQ,
    HK_TS_ATTR_VALUE_UNQ,
    HK_TS_AFTER_ATTR_VALUE_QUOTED,
    HK_TS_SELF_CLOSING_START_TAG,
    HK_TS_BOGUS_COMMENT,
    HK_TS_MARKUP_DECL_OPEN,
    HK_TS_COMMENT_START,
    HK_TS_COMMENT_START_DASH,
    HK_TS_COMMENT,
    HK_TS_COMMENT_LT,
    HK_TS_COMMENT_LT_BANG,
    HK_TS_COMMENT_LT_BANG_DASH,
    HK_TS_COMMENT_LT_BANG_DASH_DASH,
    HK_TS_COMMENT_END_DASH,
    HK_TS_COMMENT_END,
    HK_TS_COMMENT_END_BANG,
    HK_TS_DOCTYPE,
    HK_TS_BEFORE_DOCTYPE_NAME,
    HK_TS_DOCTYPE_NAME,
    HK_TS_AFTER_DOCTYPE_NAME,
    HK_TS_AFTER_DOCTYPE_PUBLIC_KW,
    HK_TS_BEFORE_DOCTYPE_PUBLIC_ID,
    HK_TS_DOCTYPE_PUBLIC_ID_DQ,
    HK_TS_DOCTYPE_PUBLIC_ID_SQ,
    HK_TS_AFTER_DOCTYPE_PUBLIC_ID,
    HK_TS_BETWEEN_DOCTYPE_PUBLIC_SYSTEM,
    HK_TS_AFTER_DOCTYPE_SYSTEM_KW,
    HK_TS_BEFORE_DOCTYPE_SYSTEM_ID,
    HK_TS_DOCTYPE_SYSTEM_ID_DQ,
    HK_TS_DOCTYPE_SYSTEM_ID_SQ,
    HK_TS_AFTER_DOCTYPE_SYSTEM_ID,
    HK_TS_BOGUS_DOCTYPE,
    HK_TS_CDATA_SECTION,
    HK_TS_CDATA_SECTION_BRACKET,
    HK_TS_CDATA_SECTION_END,
    HK_TS_CHAR_REF,
    HK_TS_NAMED_CHAR_REF,
    HK_TS_AMBIGUOUS_AMPERSAND,
    HK_TS_NUMERIC_CHAR_REF,
    HK_TS_HEX_CHAR_REF_START,
    HK_TS_DEC_CHAR_REF_START,
    HK_TS_HEX_CHAR_REF,
    HK_TS_DEC_CHAR_REF,
    HK_TS_EOF_EMITTED,
} hk_html_tok_state;

typedef void (*hk_html_token_fn)(void *user, hk_html_token *tok);

typedef struct hk_html_tok_attr_span {
    uint32_t name_off, name_len, value_off, value_len;
} hk_html_tok_attr_span;

typedef struct hk_html_tokenizer {
    hk_html_token_fn emit;
    void *user;

    hk_charbuf input; /* newline-normalized input */
    size_t pos;
    bool eof;
    bool last_was_cr;

    hk_html_tok_state state;
    hk_html_tok_state return_state;
    /* Set by the tree builder: CDATA sections are allowed only in foreign content. */
    bool cdata_allowed;

    /* Token under construction. */
    hk_html_token_type tag_type;
    bool self_closing;
    hk_charbuf tag_name;
    hk_charbuf attr_buf;
    HK_VEC(hk_html_tok_attr_span) attrs;
    HK_VEC(hk_html_tok_attr) attr_out;
    hk_charbuf comment;
    hk_charbuf doctype_name, public_id, system_id;
    bool has_name, has_public, has_system, force_quirks;
    hk_charbuf temp;     /* the specification's temporary buffer */
    hk_charbuf text;     /* pending character run */
    hk_charbuf last_start_tag;
    uint32_t char_ref_code;
} hk_html_tokenizer;

void hk_html_tokenizer_init(hk_html_tokenizer *t, hk_html_token_fn emit, void *user);
void hk_html_tokenizer_free(hk_html_tokenizer *t);

/* Appends input and tokenizes as far as possible. */
void hk_html_tokenizer_feed(hk_html_tokenizer *t, const char *p, size_t n);
/* Marks the end of input and tokenizes the rest, ending with an EOF token. */
void hk_html_tokenizer_finish(hk_html_tokenizer *t);

void hk_html_tokenizer_set_state(hk_html_tokenizer *t, hk_html_tok_state s);
/* For tests: the name used by the "appropriate end tag" check. */
void hk_html_tokenizer_set_last_start_tag(hk_html_tokenizer *t, hk_str name);

#endif
