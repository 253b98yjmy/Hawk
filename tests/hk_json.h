/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A minimal JSON reader for test data files (trusted input, no error handling). */
#ifndef HK_TEST_JSON_H
#define HK_TEST_JSON_H

#include "base/hk_utf8.h"
#include "base/hk_vec.h"

#include <stdlib.h>
#include <string.h>


typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct json json;
struct json {
    jtype type;
    bool b;
    char *s; /* UTF-8, NUL-terminated (may contain NUL; see len) */
    size_t len;
    bool lone_surrogate;
    json **items; /* arrays: values; objects: alternating key, value */
    size_t count;
};

typedef struct jparser {
    const char *p, *end;
} jparser;

static inline void ws(jparser *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++;
}

static inline json *jnew(jtype t)
{
    json *v = calloc(1, sizeof *v);
    v->type = t;
    return v;
}

static inline void jpush(json *v, json *item)
{
    v->items = realloc(v->items, (v->count + 1) * sizeof *v->items);
    v->items[v->count++] = item;
}

static inline unsigned hex4(const char *s)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

static inline json *jvalue(jparser *j);

static inline json *jstring(jparser *j)
{
    json *v = jnew(J_STR);
    hk_charbuf b = {0};
    j->p++; /* opening quote */
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') {
            hk_charbuf_push(&b, c);
            continue;
        }
        char e = *j->p++;
        switch (e) {
        case 'n': hk_charbuf_push(&b, '\n'); break;
        case 't': hk_charbuf_push(&b, '\t'); break;
        case 'r': hk_charbuf_push(&b, '\r'); break;
        case 'b': hk_charbuf_push(&b, '\b'); break;
        case 'f': hk_charbuf_push(&b, '\f'); break;
        case 'u': {
            unsigned cp = hex4(j->p);
            j->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && j->p[0] == '\\' && j->p[1] == 'u') {
                unsigned lo = hex4(j->p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    j->p += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) v->lone_surrogate = true;
            uint8_t tmp[4];
            size_t n = hk_utf8_encode(cp, tmp);
            hk_charbuf_append(&b, (const char *)tmp, n);
            break;
        }
        default: hk_charbuf_push(&b, e); break;
        }
    }
    j->p++; /* closing quote */
    v->len = b.len;
    v->s = malloc(b.len + 1);
    if (b.len) memcpy(v->s, b.data, b.len);
    v->s[b.len] = 0;
    hk_vec_free(&b);
    return v;
}

static inline json *jvalue(jparser *j)
{
    ws(j);
    char c = *j->p;
    if (c == '"') return jstring(j);
    if (c == '[' || c == '{') {
        json *v = jnew(c == '[' ? J_ARR : J_OBJ);
        char close = c == '[' ? ']' : '}';
        j->p++;
        ws(j);
        while (*j->p != close) {
            if (v->type == J_OBJ) {
                jpush(v, jstring(j));
                ws(j);
                j->p++; /* : */
            }
            jpush(v, jvalue(j));
            ws(j);
            if (*j->p == ',') j->p++;
            ws(j);
        }
        j->p++;
        return v;
    }
    if (c == 't' || c == 'f') {
        json *v = jnew(J_BOOL);
        v->b = c == 't';
        j->p += c == 't' ? 4 : 5;
        return v;
    }
    if (c == 'n') {
        j->p += 4;
        return jnew(J_NULL);
    }
    json *v = jnew(J_NUM);
    while (j->p < j->end && strchr("-+.eE0123456789", *j->p)) j->p++;
    return v;
}

static inline void jfree(json *v)
{
    if (!v) return;
    for (size_t i = 0; i < v->count; i++) jfree(v->items[i]);
    free(v->items);
    free(v->s);
    free(v);
}

static inline json *jget(const json *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (size_t i = 0; i + 1 < obj->count; i += 2) {
        if (strcmp(obj->items[i]->s, key) == 0) return obj->items[i + 1];
    }
    return NULL;
}

static inline bool has_surrogate(const json *v)
{
    if (!v) return false;
    if (v->lone_surrogate) return true;
    for (size_t i = 0; i < v->count; i++) {
        if (has_surrogate(v->items[i])) return true;
    }
    return false;
}


static inline json *json_parse(const char *p, size_t n)
{
    jparser j = {p, p + n};
    return jvalue(&j);
}

#endif
