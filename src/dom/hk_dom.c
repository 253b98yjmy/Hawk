/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dom/hk_dom.h"
#include "base/hk_mem.h"

#include <string.h>

hk_document *hk_document_create(void)
{
    hk_document *d = hk_alloc_zero(sizeof *d);
    hk_arena_init(&d->arena, 0);
    hk_atom_table_init(&d->atoms, &d->arena, hk_names_table());
    hk_node unused = {0};
    hk_vec_push(&d->nodes, unused);
    hk_node root = {0};
    root.type = HK_NODE_DOCUMENT;
    d->root = d->nodes.len;
    hk_vec_push(&d->nodes, root);
    return d;
}

void hk_document_destroy(hk_document *d)
{
    if (!d) return;
    hk_vec_free(&d->nodes);
    hk_vec_free(&d->elements);
    hk_vec_free(&d->texts);
    hk_vec_free(&d->doctypes);
    hk_atom_table_free(&d->atoms);
    hk_arena_free(&d->arena);
    hk_free(d, sizeof *d);
}

size_t hk_document_memory(const hk_document *d)
{
    return d->arena.bytes_reserved + (size_t)d->nodes.cap * sizeof(hk_node) +
           (size_t)d->elements.cap * sizeof(hk_element) + (size_t)d->texts.cap * sizeof(hk_text) +
           (size_t)d->doctypes.cap * sizeof(hk_doctype) +
           (size_t)d->atoms.slot_cap * sizeof(uint32_t) +
           (size_t)d->atoms.entries.cap * sizeof(hk_atom_entry);
}

static hk_node_id new_node(hk_document *d, hk_node_type type, uint32_t data)
{
    HK_ASSERT(d->nodes.len < UINT32_MAX - 1);
    hk_node n = {0};
    n.type = (uint8_t)type;
    n.data = data;
    hk_node_id id = d->nodes.len;
    hk_vec_push(&d->nodes, n);
    return id;
}

hk_node_id hk_dom_create_element(hk_document *d, hk_ns ns, hk_atom name)
{
    hk_element e = {0};
    e.name = name;
    uint32_t idx = d->elements.len;
    hk_vec_push(&d->elements, e);
    hk_node_id id = new_node(d, HK_NODE_ELEMENT, idx);
    d->nodes.data[id].ns = (uint8_t)ns;
    return id;
}

static uint32_t new_text_data(hk_document *d, const char *p, size_t n)
{
    HK_ASSERT(n < UINT32_MAX / 2);
    hk_text t = {0};
    t.cap = (uint32_t)HK_MAX(n, 16);
    t.p = hk_arena_alloc(&d->arena, t.cap, 1);
    if (n) memcpy(t.p, p, n);
    t.len = (uint32_t)n;
    uint32_t idx = d->texts.len;
    hk_vec_push(&d->texts, t);
    return idx;
}

hk_node_id hk_dom_create_text(hk_document *d, const char *p, size_t n)
{
    return new_node(d, HK_NODE_TEXT, new_text_data(d, p, n));
}

hk_node_id hk_dom_create_comment(hk_document *d, const char *p, size_t n)
{
    return new_node(d, HK_NODE_COMMENT, new_text_data(d, p, n));
}

hk_node_id hk_dom_create_doctype(hk_document *d, const hk_doctype *dt)
{
    hk_doctype c = *dt;
    c.name = hk_arena_str_dup(&d->arena, dt->name);
    c.public_id = hk_arena_str_dup(&d->arena, dt->public_id);
    c.system_id = hk_arena_str_dup(&d->arena, dt->system_id);
    uint32_t idx = d->doctypes.len;
    hk_vec_push(&d->doctypes, c);
    return new_node(d, HK_NODE_DOCTYPE, idx);
}

hk_node_id hk_dom_create_fragment(hk_document *d)
{
    return new_node(d, HK_NODE_FRAGMENT, 0);
}

hk_str hk_dom_text(const hk_document *d, hk_node_id id)
{
    const hk_node *n = hk_dom_node(d, id);
    HK_ASSERT(n->type == HK_NODE_TEXT || n->type == HK_NODE_COMMENT);
    const hk_text *t = &d->texts.data[n->data];
    return (hk_str){t->p, t->len};
}

void hk_dom_text_append(hk_document *d, hk_node_id id, const char *p, size_t n)
{
    hk_node *node = hk_dom_node(d, id);
    HK_ASSERT(node->type == HK_NODE_TEXT || node->type == HK_NODE_COMMENT);
    hk_text *t = &d->texts.data[node->data];
    HK_ASSERT(n < UINT32_MAX / 2 - t->len);
    size_t need = (size_t)t->len + n;
    if (need > t->cap) {
        /* Grow in place when this buffer is the arena's most recent allocation. */
        hk_arena *a = &d->arena;
        if ((uint8_t *)t->p + t->cap == a->cur && need - t->cap <= (size_t)(a->end - a->cur)) {
            a->cur += need - t->cap;
            a->bytes_used += need - t->cap;
            t->cap = (uint32_t)need;
        } else {
            uint32_t cap = (uint32_t)HK_MAX(need, (size_t)t->cap * 2);
            char *np = hk_arena_alloc(a, cap, 1);
            memcpy(np, t->p, t->len);
            t->p = np;
            t->cap = cap;
        }
    }
    memcpy(t->p + t->len, p, n);
    t->len = (uint32_t)need;
}

void hk_dom_insert_before(hk_document *d, hk_node_id parent, hk_node_id child, hk_node_id ref)
{
    hk_node *c = hk_dom_node(d, child);
    hk_node *p = hk_dom_node(d, parent);
    HK_ASSERT(c->parent == HK_NODE_NONE && child != parent);
    c->parent = parent;
    if (ref == HK_NODE_NONE) {
        c->prev = p->last_child;
        c->next = HK_NODE_NONE;
        if (p->last_child) hk_dom_node(d, p->last_child)->next = child;
        else p->first_child = child;
        p->last_child = child;
        return;
    }
    hk_node *r = hk_dom_node(d, ref);
    HK_ASSERT(r->parent == parent);
    c->next = ref;
    c->prev = r->prev;
    if (r->prev) hk_dom_node(d, r->prev)->next = child;
    else p->first_child = child;
    r->prev = child;
}

void hk_dom_append(hk_document *d, hk_node_id parent, hk_node_id child)
{
    hk_dom_insert_before(d, parent, child, HK_NODE_NONE);
}

void hk_dom_remove(hk_document *d, hk_node_id child)
{
    hk_node *c = hk_dom_node(d, child);
    if (c->parent == HK_NODE_NONE) return;
    hk_node *p = hk_dom_node(d, c->parent);
    if (c->prev) hk_dom_node(d, c->prev)->next = c->next;
    else p->first_child = c->next;
    if (c->next) hk_dom_node(d, c->next)->prev = c->prev;
    else p->last_child = c->prev;
    c->parent = c->prev = c->next = HK_NODE_NONE;
}

const hk_attr *hk_dom_attr(const hk_document *d, hk_node_id el, hk_attr_ns ns, hk_atom name)
{
    const hk_element *e = hk_dom_element(d, el);
    for (uint32_t i = 0; i < e->attr_count; i++) {
        if (e->attrs[i].name == name && e->attrs[i].ns == ns) return &e->attrs[i];
    }
    return NULL;
}

bool hk_dom_add_attr(hk_document *d, hk_node_id el, hk_attr_ns ns, hk_atom name, hk_str value)
{
    if (hk_dom_attr(d, el, ns, name)) return false;
    hk_element *e = hk_dom_element(d, el);
    if (e->attr_count == e->attr_cap) {
        uint32_t cap = e->attr_cap ? e->attr_cap * 2 : 4;
        hk_attr *na = HK_ARENA_NEW_ARRAY(&d->arena, hk_attr, cap);
        if (e->attr_count) memcpy(na, e->attrs, e->attr_count * sizeof(hk_attr));
        e->attrs = na;
        e->attr_cap = cap;
    }
    hk_attr *a = &e->attrs[e->attr_count++];
    a->name = name;
    a->ns = (uint8_t)ns;
    a->value = hk_arena_str_dup(&d->arena, value);
    return true;
}
