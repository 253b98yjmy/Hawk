/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * CSS: tokenizer (CSS Syntax Level 3), stylesheet parser, selectors and media
 * queries. A stylesheet belongs to one document: selector names are interned in
 * that document's atom table so matching compares integers.
 */
#ifndef HK_CSS_H
#define HK_CSS_H

#include "base/hk_arena.h"
#include "base/hk_atom.h"
#include "base/hk_vec.h"

/* --- tokens ----------------------------------------------------------------------------- */

typedef enum hk_css_token_type {
    HK_CT_EOF = 0,
    HK_CT_IDENT,
    HK_CT_FUNCTION, /* value = name; followed by the arguments and a matching ')' */
    HK_CT_AT,
    HK_CT_HASH,
    HK_CT_STRING,
    HK_CT_BAD_STRING,
    HK_CT_URL,
    HK_CT_BAD_URL,
    HK_CT_DELIM,
    HK_CT_NUMBER,
    HK_CT_PERCENT,
    HK_CT_DIMENSION,
    HK_CT_WS,
    HK_CT_CDO,
    HK_CT_CDC,
    HK_CT_COLON,
    HK_CT_SEMI,
    HK_CT_COMMA,
    HK_CT_LBRACKET,
    HK_CT_RBRACKET,
    HK_CT_LPAREN,
    HK_CT_RPAREN,
    HK_CT_LBRACE,
    HK_CT_RBRACE,
} hk_css_token_type;

#define HK_CSS_HASH_ID 1u   /* hash token is a valid identifier */
#define HK_CSS_INTEGER 2u   /* number written without a fraction or exponent */

typedef struct hk_css_token {
    uint8_t type;
    uint8_t flags;
    uint32_t delim; /* code point for HK_CT_DELIM */
    double num;
    hk_str value; /* ident, function/at/hash name, string, url */
    hk_str unit;  /* dimension unit */
} hk_css_token;

typedef struct hk_css_tokens {
    hk_css_token *t;
    uint32_t n;
} hk_css_tokens;

/* Tokenizes UTF-8 CSS. Tokens and unescaped strings live in the arena; plain strings
 * point into src, which must outlive the tokens. A terminating HK_CT_EOF is appended. */
hk_css_tokens hk_css_tokenize(hk_arena *a, hk_str src);

/* --- selectors ----------------------------------------------------------------------------- */

typedef enum hk_css_simple_kind {
    HK_SS_TYPE,
    HK_SS_UNIVERSAL,
    HK_SS_ID,
    HK_SS_CLASS,
    HK_SS_ATTR_EXISTS,
    HK_SS_ATTR_EQ,
    HK_SS_ATTR_INCLUDES, /* ~= */
    HK_SS_ATTR_DASH,     /* |= */
    HK_SS_ATTR_PREFIX,   /* ^= */
    HK_SS_ATTR_SUFFIX,   /* $= */
    HK_SS_ATTR_SUBSTR,   /* *= */
    HK_SS_ROOT,
    HK_SS_EMPTY,
    HK_SS_FIRST_CHILD,
    HK_SS_LAST_CHILD,
    HK_SS_ONLY_CHILD,
    HK_SS_FIRST_OF_TYPE,
    HK_SS_LAST_OF_TYPE,
    HK_SS_ONLY_OF_TYPE,
    HK_SS_NTH_CHILD,
    HK_SS_NTH_LAST_CHILD,
    HK_SS_NTH_OF_TYPE,
    HK_SS_NTH_LAST_OF_TYPE,
    HK_SS_LINK,     /* :link, :any-link */
    HK_SS_NOT,
    HK_SS_IS,       /* :is(), :where(), :matches() */
    HK_SS_NEVER,    /* dynamic or unsupported state that never matches (:hover, :visited, ...) */
} hk_css_simple_kind;

typedef enum hk_css_combinator {
    HK_COMB_NONE = 0,
    HK_COMB_DESCENDANT,
    HK_COMB_CHILD,
    HK_COMB_NEXT,       /* + */
    HK_COMB_SUBSEQUENT, /* ~ */
} hk_css_combinator;

typedef struct hk_css_selector_list hk_css_selector_list;

typedef struct hk_css_simple {
    uint8_t kind;
    bool icase;   /* attribute value comparison: [a=b i] */
    hk_atom name; /* tag, id, class or attribute name (in the document's atom table) */
    hk_str value; /* attribute value */
    int32_t a, b; /* nth: an+b */
    const hk_css_selector_list *inner;
} hk_css_simple;

typedef struct hk_css_compound {
    const hk_css_simple *s;
    uint32_t n;
    uint8_t combinator; /* how this compound relates to the next one on its left */
} hk_css_compound;

typedef struct hk_css_selector {
    const hk_css_compound *c; /* c[0] is the rightmost compound (the subject) */
    uint32_t n;
    uint32_t specificity; /* (a << 20) | (b << 10) | c */
} hk_css_selector;

struct hk_css_selector_list {
    const hk_css_selector *sel;
    uint32_t n;
};

/* Parses a selector list; returns false if any selector is invalid (the whole rule is dropped). */
bool hk_css_parse_selectors(hk_arena *a, hk_atom_table *atoms, const hk_css_token *t, uint32_t n,
                            hk_css_selector_list *out);

/* --- media queries ------------------------------------------------------------------------- */

typedef struct hk_css_media_env {
    float width, height; /* viewport in CSS px */
    bool dark;
} hk_css_media_env;

/* Evaluates a media query list (the prelude of @media or a media attribute). Empty = true. */
bool hk_css_media_matches(const hk_css_token *t, uint32_t n, const hk_css_media_env *env);

/* --- stylesheets ------------------------------------------------------------------------------ */

typedef struct hk_css_decl {
    uint16_t prop;  /* hk_css_property, or HK_PROP_CUSTOM */
    bool important;
    hk_str custom;  /* custom property name ("--x") */
    const hk_css_token *v;
    uint32_t vn;
} hk_css_decl;

typedef struct hk_css_condition hk_css_condition;
struct hk_css_condition {
    const hk_css_condition *parent;
    const hk_css_token *media; /* media query prelude */
    uint32_t media_n;
};

typedef struct hk_css_rule {
    hk_css_selector_list selectors;
    const hk_css_decl *decls;
    uint32_t ndecls;
    const hk_css_condition *cond; /* enclosing @media blocks, or NULL */
} hk_css_rule;

typedef struct hk_css_sheet {
    hk_arena *arena;
    hk_str source;
    HK_VEC(hk_css_rule) rules;
    HK_VEC(hk_str) imports; /* @import URLs, in order */
} hk_css_sheet;

/* Parses a stylesheet. The sheet's data lives in the arena (the document's). */
void hk_css_parse_sheet(hk_arena *a, hk_atom_table *atoms, hk_str src, hk_css_sheet *out);
void hk_css_sheet_free(hk_css_sheet *s);

/* Declarations of a style="" attribute. */
void hk_css_parse_declarations(hk_arena *a, hk_str src, const hk_css_decl **out, uint32_t *n);

bool hk_css_condition_matches(const hk_css_condition *c, const hk_css_media_env *env);

/* Is the token an identifier equal (ASCII case-insensitively) to s? */
bool hk_css_ident_is(const hk_css_token *t, const char *s);

#endif
