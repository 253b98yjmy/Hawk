/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Document tree. Nodes are addressed by 32-bit ids (0 = none) rather than
 * pointers; see docs/design/01-architecture.md, 4.2. Pointers returned by the
 * accessors are invalidated when nodes are created, so hold ids, not pointers,
 * across calls that create nodes.
 *
 * The tree is built by the HTML parser and does not change afterwards
 * (Hawk runs no scripts).
 */
#ifndef HK_DOM_H
#define HK_DOM_H

#include "base/hk_arena.h"
#include "base/hk_atom.h"
#include "base/hk_common.h"
#include "base/hk_vec.h"
#include "dom/hk_names.h"

typedef uint32_t hk_node_id;
#define HK_NODE_NONE 0u

typedef enum hk_node_type {
    HK_NODE_DOCUMENT = 1,
    HK_NODE_DOCTYPE,
    HK_NODE_ELEMENT,
    HK_NODE_TEXT,
    HK_NODE_COMMENT,
    HK_NODE_FRAGMENT, /* template contents */
} hk_node_type;

typedef enum hk_ns {
    HK_NS_HTML = 0,
    HK_NS_SVG,
    HK_NS_MATHML,
} hk_ns;

typedef enum hk_attr_ns {
    HK_ATTR_NS_NONE = 0,
    HK_ATTR_NS_XLINK,
    HK_ATTR_NS_XML,
    HK_ATTR_NS_XMLNS,
} hk_attr_ns;

typedef enum hk_quirks_mode {
    HK_QUIRKS_NO = 0,
    HK_QUIRKS_LIMITED,
    HK_QUIRKS_FULL,
} hk_quirks_mode;

typedef struct hk_node {
    uint8_t type; /* hk_node_type */
    uint8_t ns;   /* hk_ns, elements only */
    uint16_t flags;
    hk_node_id parent, first_child, last_child, prev, next;
    uint32_t data; /* index into elements / texts / doctypes, by type */
} hk_node;

typedef struct hk_attr {
    hk_atom name; /* local name */
    uint8_t ns;   /* hk_attr_ns */
    hk_str value;
} hk_attr;

typedef struct hk_element {
    hk_atom name;
    uint32_t attr_count;
    uint32_t attr_cap;
    hk_attr *attrs;
    hk_node_id template_content; /* fragment for <template>, else none */
} hk_element;

typedef struct hk_text {
    char *p;
    uint32_t len;
    uint32_t cap;
} hk_text;

typedef struct hk_doctype {
    hk_str name;
    hk_str public_id;
    hk_str system_id;
    bool has_public;
    bool has_system;
} hk_doctype;

/* A form-associated element and the form the parser associated it with (its form owner). */
typedef struct hk_form_link {
    hk_node_id element, form;
} hk_form_link;

typedef struct hk_document {
    hk_arena arena; /* strings, attributes, text */
    hk_atom_table atoms;
    HK_VEC(hk_node) nodes; /* nodes.data[0] is unused so that id 0 means none */
    HK_VEC(hk_element) elements;
    HK_VEC(hk_text) texts;
    HK_VEC(hk_doctype) doctypes;
    HK_VEC(hk_form_link) form_links; /* in document order of creation */
    hk_node_id root; /* the Document node */
    uint8_t quirks;  /* hk_quirks_mode */
} hk_document;

/* hk_names_init must have been called. */
hk_document *hk_document_create(void);
void hk_document_destroy(hk_document *d);
/* Bytes used by the document (arena + node arrays), for hawk:about and benchmarks. */
size_t hk_document_memory(const hk_document *d);

static inline hk_node *hk_dom_node(const hk_document *d, hk_node_id id)
{
    HK_ASSERT(id != HK_NODE_NONE && id < d->nodes.len);
    return &d->nodes.data[id];
}

static inline hk_element *hk_dom_element(const hk_document *d, hk_node_id id)
{
    hk_node *n = hk_dom_node(d, id);
    HK_ASSERT(n->type == HK_NODE_ELEMENT);
    return &d->elements.data[n->data];
}

static inline bool hk_dom_is(const hk_document *d, hk_node_id id, hk_ns ns, hk_atom name)
{
    const hk_node *n = hk_dom_node(d, id);
    return n->type == HK_NODE_ELEMENT && n->ns == ns && d->elements.data[n->data].name == name;
}

static inline bool hk_dom_is_html(const hk_document *d, hk_node_id id, hk_atom name)
{
    return hk_dom_is(d, id, HK_NS_HTML, name);
}

hk_node_id hk_dom_create_element(hk_document *d, hk_ns ns, hk_atom name);
hk_node_id hk_dom_create_text(hk_document *d, const char *p, size_t n);
hk_node_id hk_dom_create_comment(hk_document *d, const char *p, size_t n);
hk_node_id hk_dom_create_doctype(hk_document *d, const hk_doctype *dt); /* strings are copied */
hk_node_id hk_dom_create_fragment(hk_document *d);

/* Text and comment data. */
hk_str hk_dom_text(const hk_document *d, hk_node_id id);
void hk_dom_text_append(hk_document *d, hk_node_id id, const char *p, size_t n);

/* Tree mutation (parser only). child must not already have a parent. */
void hk_dom_append(hk_document *d, hk_node_id parent, hk_node_id child);
/* ref == HK_NODE_NONE appends. */
void hk_dom_insert_before(hk_document *d, hk_node_id parent, hk_node_id child, hk_node_id ref);
void hk_dom_remove(hk_document *d, hk_node_id child);

/* Attributes. Values are copied into the document arena. */
const hk_attr *hk_dom_attr(const hk_document *d, hk_node_id el, hk_attr_ns ns, hk_atom name);
/* Adds the attribute unless one with the same namespace and name exists. Returns true if added. */
bool hk_dom_add_attr(hk_document *d, hk_node_id el, hk_attr_ns ns, hk_atom name, hk_str value);

#endif
