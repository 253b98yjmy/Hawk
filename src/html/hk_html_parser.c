/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Tree construction. Insertion modes and algorithms are named after WHATWG HTML
 * 13.2.6 so each function can be compared with its section of the specification.
 * Parse errors are not reported.
 */
#include "html/hk_html_parser.h"
#include "html/hk_html_tokenizer.h"
#include "base/hk_mem.h"

#include <string.h>

typedef enum mode {
    M_INITIAL,
    M_BEFORE_HTML,
    M_BEFORE_HEAD,
    M_IN_HEAD,
    M_IN_HEAD_NOSCRIPT,
    M_AFTER_HEAD,
    M_IN_BODY,
    M_TEXT,
    M_IN_TABLE,
    M_IN_TABLE_TEXT,
    M_IN_CAPTION,
    M_IN_COLUMN_GROUP,
    M_IN_TABLE_BODY,
    M_IN_ROW,
    M_IN_CELL,
    M_IN_TEMPLATE,
    M_AFTER_BODY,
    M_IN_FRAMESET,
    M_AFTER_FRAMESET,
    M_AFTER_AFTER_BODY,
    M_AFTER_AFTER_FRAMESET,
} mode;

/* A token as the tree builder sees it: the tag name is resolved to an atom once. */
typedef struct tok {
    hk_html_token_type type;
    hk_atom name;  /* tag name atom; 0 for end tags with names no element can have */
    hk_str name_s; /* tag name string */
    const hk_html_tok_attr *attrs;
    uint32_t attr_count;
    bool self_closing;
    hk_str data; /* comment text or exactly one character (UTF-8) */
    const hk_html_token *raw;
} tok;

struct hk_html_parser {
    hk_document *doc;
    hk_html_tokenizer tokenizer;
    mode mode, original_mode;
    HK_VEC(hk_node_id) open;     /* stack of open elements */
    HK_VEC(hk_node_id) afe;      /* active formatting elements; 0 = marker */
    HK_VEC(uint8_t) template_modes;
    hk_node_id head, form;
    bool frameset_ok;
    bool foster_parenting;
    bool ignore_lf;
    bool stopped;
    hk_charbuf table_text;
    bool table_text_non_ws;
};

typedef hk_html_parser P;

static void process(P *p, const tok *t);
static void process_in(P *p, mode m, const tok *t);

/* --- small helpers ------------------------------------------------------------------ */

#define A(x) HK_ATOM_##x

static hk_document *D(P *p)
{
    return p->doc;
}

static hk_node_id current(P *p)
{
    return p->open.len ? p->open.data[p->open.len - 1] : HK_NODE_NONE;
}

static hk_atom el_name(P *p, hk_node_id id)
{
    return hk_dom_element(p->doc, id)->name;
}

static hk_ns el_ns(P *p, hk_node_id id)
{
    return (hk_ns)hk_dom_node(p->doc, id)->ns;
}

static bool is_html(P *p, hk_node_id id, hk_atom name)
{
    return id && hk_dom_is_html(p->doc, id, name);
}

static bool cur_is(P *p, hk_atom name)
{
    return is_html(p, current(p), name);
}

static bool is_html_one_of(P *p, hk_node_id id, const hk_atom *names, size_t n)
{
    if (!id || el_ns(p, id) != HK_NS_HTML) return false;
    hk_atom a = el_name(p, id);
    for (size_t i = 0; i < n; i++) {
        if (names[i] == a) return true;
    }
    return false;
}

#define ONE_OF(p, id, ...)                                                                         \
    is_html_one_of((p), (id), (const hk_atom[]){__VA_ARGS__},                                      \
                   sizeof((const hk_atom[]){__VA_ARGS__}) / sizeof(hk_atom))

static bool tok_is(const tok *t, hk_atom name)
{
    return t->name == name;
}

static bool tok_one_of(const tok *t, const hk_atom *names, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (names[i] == t->name) return true;
    }
    return false;
}

#define TOK_ONE_OF(t, ...)                                                                         \
    tok_one_of((t), (const hk_atom[]){__VA_ARGS__},                                                \
               sizeof((const hk_atom[]){__VA_ARGS__}) / sizeof(hk_atom))

static bool is_start(const tok *t, hk_atom name)
{
    return t->type == HK_TOK_START_TAG && t->name == name;
}

static bool is_end(const tok *t, hk_atom name)
{
    return t->type == HK_TOK_END_TAG && t->name == name;
}

static bool is_ws_char(const tok *t)
{
    if (t->type != HK_TOK_CHARS || t->data.n != 1) return false;
    char c = t->data.p[0];
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static const hk_html_tok_attr *tok_attr(const tok *t, const char *name)
{
    hk_str n = hk_str_from_cstr(name);
    for (uint32_t i = 0; i < t->attr_count; i++) {
        if (hk_str_eq(t->attrs[i].name, n)) return &t->attrs[i];
    }
    return NULL;
}

/* A synthesized start tag without attributes, e.g. the implied <head>. */
static tok fake_start(P *p, hk_atom name)
{
    tok t = {0};
    t.type = HK_TOK_START_TAG;
    t.name = name;
    t.name_s = hk_atom_str(&p->doc->atoms, name);
    return t;
}

/* --- stack of open elements ---------------------------------------------------------------- */

static void push(P *p, hk_node_id id)
{
    hk_vec_push(&p->open, id);
}

static void maybe_clone_option_into_selectedcontent(P *p, hk_node_id option);

static void pop(P *p)
{
    HK_ASSERT(p->open.len > 0);
    hk_node_id id = p->open.data[--p->open.len];
    if (hk_dom_is_html(p->doc, id, HK_ATOM_option)) maybe_clone_option_into_selectedcontent(p, id);
}

static int stack_index(P *p, hk_node_id id)
{
    for (int i = (int)p->open.len - 1; i >= 0; i--) {
        if (p->open.data[i] == id) return i;
    }
    return -1;
}

static void stack_remove(P *p, hk_node_id id)
{
    int i = stack_index(p, id);
    if (i < 0) return;
    memmove(&p->open.data[i], &p->open.data[i + 1], (p->open.len - (uint32_t)i - 1) * sizeof(hk_node_id));
    p->open.len--;
}

static void stack_insert(P *p, uint32_t index, hk_node_id id)
{
    hk_vec_reserve(&p->open, (uint64_t)p->open.len + 1);
    memmove(&p->open.data[index + 1], &p->open.data[index], (p->open.len - index) * sizeof(hk_node_id));
    p->open.data[index] = id;
    p->open.len++;
}

static void pop_until(P *p, hk_atom name)
{
    while (p->open.len) {
        hk_node_id id = current(p);
        pop(p);
        if (is_html(p, id, name)) return;
    }
}

static void pop_until_one_of(P *p, const hk_atom *names, size_t n)
{
    while (p->open.len) {
        hk_node_id id = current(p);
        pop(p);
        if (is_html_one_of(p, id, names, n)) return;
    }
}

static bool stack_has(P *p, hk_atom name)
{
    for (uint32_t i = 0; i < p->open.len; i++) {
        if (is_html(p, p->open.data[i], name)) return true;
    }
    return false;
}

/* --- element categories ------------------------------------------------------------------ */

static bool is_special(P *p, hk_node_id id)
{
    hk_ns ns = el_ns(p, id);
    hk_atom n = el_name(p, id);
    if (ns == HK_NS_MATHML)
        return n == A(mi) || n == A(mo) || n == A(mn) || n == A(ms) || n == A(mtext) ||
               n == A(annotation_xml);
    if (ns == HK_NS_SVG) return n == A(foreignObject) || n == A(desc) || n == A(title);
    static const hk_atom k[] = {
        A(address), A(applet), A(area), A(article), A(aside), A(base), A(basefont),
        A(bgsound), A(blockquote), A(body), A(br), A(button), A(caption), A(center),
        A(col), A(colgroup), A(dd), A(details), A(dir), A(div), A(dl), A(dt), A(embed),
        A(fieldset), A(figcaption), A(figure), A(footer), A(form), A(frame), A(frameset),
        A(h1), A(h2), A(h3), A(h4), A(h5), A(h6), A(head), A(header), A(hgroup), A(hr),
        A(html), A(iframe), A(img), A(input), A(keygen), A(li), A(link), A(listing),
        A(main), A(marquee), A(menu), A(meta), A(nav), A(noembed), A(noframes),
        A(noscript), A(object), A(ol), A(p), A(param), A(plaintext), A(pre), A(script),
        A(search), A(section), A(select), A(source), A(style), A(summary), A(table),
        A(tbody), A(td), A(template), A(textarea), A(tfoot), A(th), A(thead), A(title),
        A(tr), A(track), A(ul), A(wbr), A(xmp),
    };
    return is_html_one_of(p, id, k, HK_ARRAY_LEN(k));
}

static bool is_formatting_name(hk_atom n)
{
    return n == A(a) || n == A(b) || n == A(big) || n == A(code) || n == A(em) ||
           n == A(font) || n == A(i) || n == A(nobr) || n == A(s) || n == A(small) ||
           n == A(strike) || n == A(strong) || n == A(tt) || n == A(u);
}

static bool attr_value_icase(P *p, hk_node_id id, hk_atom name, const char *v)
{
    const hk_attr *a = hk_dom_attr(p->doc, id, HK_ATTR_NS_NONE, name);
    return a && hk_str_eq_ascii_icase(a->value, hk_str_from_cstr(v));
}

static bool is_mathml_text_integration_point(P *p, hk_node_id id)
{
    if (el_ns(p, id) != HK_NS_MATHML) return false;
    hk_atom n = el_name(p, id);
    return n == A(mi) || n == A(mo) || n == A(mn) || n == A(ms) || n == A(mtext);
}

static bool is_html_integration_point(P *p, hk_node_id id)
{
    hk_ns ns = el_ns(p, id);
    hk_atom n = el_name(p, id);
    if (ns == HK_NS_MATHML && n == A(annotation_xml))
        return attr_value_icase(p, id, A(encoding), "text/html") ||
               attr_value_icase(p, id, A(encoding), "application/xhtml+xml");
    return ns == HK_NS_SVG && (n == A(foreignObject) || n == A(desc) || n == A(title));
}

/* --- scopes ----------------------------------------------------------------------------------- */

typedef enum scope { SCOPE_DEFAULT, SCOPE_LIST_ITEM, SCOPE_BUTTON, SCOPE_TABLE } scope;

static bool is_scope_boundary(P *p, hk_node_id id, scope s)
{
    hk_ns ns = el_ns(p, id);
    hk_atom n = el_name(p, id);
    if (s == SCOPE_TABLE) return ns == HK_NS_HTML && (n == A(html) || n == A(table) || n == A(template));
    if (ns == HK_NS_HTML) {
        if (n == A(applet) || n == A(caption) || n == A(html) || n == A(table) || n == A(td) ||
            n == A(th) || n == A(marquee) || n == A(object) || n == A(template))
            return true;
        if (s == SCOPE_LIST_ITEM && (n == A(ol) || n == A(ul))) return true;
        if (s == SCOPE_BUTTON && n == A(button)) return true;
        return false;
    }
    if (ns == HK_NS_MATHML)
        return n == A(mi) || n == A(mo) || n == A(mn) || n == A(ms) || n == A(mtext) ||
               n == A(annotation_xml);
    return n == A(foreignObject) || n == A(desc) || n == A(title);
}

static bool in_scope_names(P *p, const hk_atom *names, size_t count, scope s)
{
    for (int i = (int)p->open.len - 1; i >= 0; i--) {
        hk_node_id id = p->open.data[i];
        if (is_html_one_of(p, id, names, count)) return true;
        if (is_scope_boundary(p, id, s)) return false;
    }
    return false;
}

static bool in_scope(P *p, hk_atom name, scope s)
{
    return in_scope_names(p, &name, 1, s);
}

/* "Has a particular element in scope". Since the relaxed <select> parser, an open select
 * also hides the elements below it (html5lib-tests webkit02.dat, issue 179). */
static bool node_in_scope(P *p, hk_node_id node)
{
    for (int i = (int)p->open.len - 1; i >= 0; i--) {
        hk_node_id id = p->open.data[i];
        if (id == node) return true;
        if (is_scope_boundary(p, id, SCOPE_DEFAULT) || is_html(p, id, A(select))) return false;
    }
    return false;
}

static const hk_atom k_headings[] = {A(h1), A(h2), A(h3), A(h4), A(h5), A(h6)};

/* --- implied end tags ------------------------------------------------------------------------- */

static bool has_implied_end_tag(P *p, hk_node_id id, bool thoroughly)
{
    if (ONE_OF(p, id, A(dd), A(dt), A(li), A(optgroup), A(option), A(p), A(rb), A(rp), A(rt),
               A(rtc)))
        return true;
    return thoroughly && ONE_OF(p, id, A(caption), A(colgroup), A(tbody), A(td), A(tfoot),
                                A(th), A(thead), A(tr));
}

/* except == 0 generates for all. */
static void generate_implied_end_tags(P *p, hk_atom except)
{
    while (p->open.len) {
        hk_node_id id = current(p);
        if (!has_implied_end_tag(p, id, false)) return;
        if (except && is_html(p, id, except)) return;
        pop(p);
    }
}

static void generate_implied_end_tags_thoroughly(P *p)
{
    while (p->open.len && has_implied_end_tag(p, current(p), true)) pop(p);
}

/* --- inserting nodes --------------------------------------------------------------------------- */

typedef struct location {
    hk_node_id parent;
    hk_node_id before; /* insert before this child; none = append */
} location;

static hk_node_id template_content(P *p, hk_node_id id)
{
    if (is_html(p, id, A(template))) {
        hk_element *e = hk_dom_element(p->doc, id);
        if (!e->template_content) {
            hk_node_id frag = hk_dom_create_fragment(p->doc);
            hk_dom_element(p->doc, id)->template_content = frag;
            return frag;
        }
        return e->template_content;
    }
    return id;
}

/* "Appropriate place for inserting a node" (13.2.6.1). */
static location insertion_location(P *p, hk_node_id override)
{
    hk_node_id target = override ? override : current(p);
    location loc = {target, HK_NODE_NONE};
    if (p->foster_parenting && ONE_OF(p, target, A(table), A(tbody), A(tfoot), A(thead), A(tr))) {
        int last_template = -1, last_table = -1;
        for (int i = (int)p->open.len - 1; i >= 0; i--) {
            hk_node_id id = p->open.data[i];
            if (last_template < 0 && is_html(p, id, A(template))) last_template = i;
            if (last_table < 0 && is_html(p, id, A(table))) last_table = i;
        }
        if (last_template >= 0 && (last_table < 0 || last_template > last_table)) {
            loc.parent = p->open.data[last_template];
        } else if (last_table < 0) {
            loc.parent = p->open.data[0];
        } else {
            hk_node_id table = p->open.data[last_table];
            hk_node_id parent = hk_dom_node(p->doc, table)->parent;
            if (parent) {
                loc.parent = parent;
                loc.before = table;
            } else {
                loc.parent = p->open.data[last_table - 1];
            }
        }
    }
    loc.parent = template_content(p, loc.parent);
    return loc;
}

static void insert_at(P *p, location loc, hk_node_id node)
{
    hk_dom_insert_before(p->doc, loc.parent, node, loc.before);
}

static void insert_chars(P *p, const char *s, size_t n)
{
    location loc = insertion_location(p, HK_NODE_NONE);
    hk_node *parent = hk_dom_node(p->doc, loc.parent);
    if (parent->type == HK_NODE_DOCUMENT) return;
    hk_node_id prev = loc.before ? hk_dom_node(p->doc, loc.before)->prev : parent->last_child;
    if (prev && hk_dom_node(p->doc, prev)->type == HK_NODE_TEXT) {
        hk_dom_text_append(p->doc, prev, s, n);
        return;
    }
    insert_at(p, loc, hk_dom_create_text(p->doc, s, n));
}

static void insert_comment_at(P *p, const tok *t, location loc)
{
    insert_at(p, loc, hk_dom_create_comment(p->doc, t->data.p, t->data.n));
}

static void insert_comment(P *p, const tok *t)
{
    insert_comment_at(p, t, insertion_location(p, HK_NODE_NONE));
}

static void append_comment_to(P *p, const tok *t, hk_node_id parent)
{
    location loc = {parent, HK_NODE_NONE};
    insert_comment_at(p, t, loc);
}

/* --- foreign attribute and tag name adjustments ------------------------------------------------ */

typedef struct name_map {
    const char *from, *to;
} name_map;

static const name_map k_svg_tags[] = {
    {"altglyph", "altGlyph"}, {"altglyphdef", "altGlyphDef"}, {"altglyphitem", "altGlyphItem"},
    {"animatecolor", "animateColor"}, {"animatemotion", "animateMotion"},
    {"animatetransform", "animateTransform"}, {"clippath", "clipPath"}, {"feblend", "feBlend"},
    {"fecolormatrix", "feColorMatrix"}, {"fecomponenttransfer", "feComponentTransfer"},
    {"fecomposite", "feComposite"}, {"feconvolvematrix", "feConvolveMatrix"},
    {"fediffuselighting", "feDiffuseLighting"}, {"fedisplacementmap", "feDisplacementMap"},
    {"fedistantlight", "feDistantLight"}, {"fedropshadow", "feDropShadow"},
    {"feflood", "feFlood"}, {"fefunca", "feFuncA"}, {"fefuncb", "feFuncB"},
    {"fefuncg", "feFuncG"}, {"fefuncr", "feFuncR"}, {"fegaussianblur", "feGaussianBlur"},
    {"feimage", "feImage"}, {"femerge", "feMerge"}, {"femergenode", "feMergeNode"},
    {"femorphology", "feMorphology"}, {"feoffset", "feOffset"}, {"fepointlight", "fePointLight"},
    {"fespecularlighting", "feSpecularLighting"}, {"fespotlight", "feSpotLight"},
    {"fetile", "feTile"}, {"feturbulence", "feTurbulence"}, {"foreignobject", "foreignObject"},
    {"glyphref", "glyphRef"}, {"lineargradient", "linearGradient"},
    {"radialgradient", "radialGradient"}, {"textpath", "textPath"},
};

static const name_map k_svg_attrs[] = {
    {"attributename", "attributeName"}, {"attributetype", "attributeType"},
    {"basefrequency", "baseFrequency"}, {"baseprofile", "baseProfile"}, {"calcmode", "calcMode"},
    {"clippathunits", "clipPathUnits"}, {"diffuseconstant", "diffuseConstant"},
    {"edgemode", "edgeMode"}, {"filterunits", "filterUnits"}, {"glyphref", "glyphRef"},
    {"gradienttransform", "gradientTransform"}, {"gradientunits", "gradientUnits"},
    {"kernelmatrix", "kernelMatrix"}, {"kernelunitlength", "kernelUnitLength"},
    {"keypoints", "keyPoints"}, {"keysplines", "keySplines"}, {"keytimes", "keyTimes"},
    {"lengthadjust", "lengthAdjust"}, {"limitingconeangle", "limitingConeAngle"},
    {"markerheight", "markerHeight"}, {"markerunits", "markerUnits"},
    {"markerwidth", "markerWidth"}, {"maskcontentunits", "maskContentUnits"},
    {"maskunits", "maskUnits"}, {"numoctaves", "numOctaves"}, {"pathlength", "pathLength"},
    {"patterncontentunits", "patternContentUnits"}, {"patterntransform", "patternTransform"},
    {"patternunits", "patternUnits"}, {"pointsatx", "pointsAtX"}, {"pointsaty", "pointsAtY"},
    {"pointsatz", "pointsAtZ"}, {"preservealpha", "preserveAlpha"},
    {"preserveaspectratio", "preserveAspectRatio"}, {"primitiveunits", "primitiveUnits"},
    {"refx", "refX"}, {"refy", "refY"}, {"repeatcount", "repeatCount"},
    {"repeatdur", "repeatDur"}, {"requiredextensions", "requiredExtensions"},
    {"requiredfeatures", "requiredFeatures"}, {"specularconstant", "specularConstant"},
    {"specularexponent", "specularExponent"}, {"spreadmethod", "spreadMethod"},
    {"startoffset", "startOffset"}, {"stddeviation", "stdDeviation"},
    {"stitchtiles", "stitchTiles"}, {"surfacescale", "surfaceScale"},
    {"systemlanguage", "systemLanguage"}, {"tablevalues", "tableValues"},
    {"targetx", "targetX"}, {"targety", "targetY"}, {"textlength", "textLength"},
    {"viewbox", "viewBox"}, {"viewtarget", "viewTarget"},
    {"xchannelselector", "xChannelSelector"}, {"ychannelselector", "yChannelSelector"},
    {"zoomandpan", "zoomAndPan"},
};

static hk_str map_name(const name_map *m, size_t n, hk_str s)
{
    for (size_t i = 0; i < n; i++) {
        if (hk_str_eq(s, hk_str_from_cstr(m[i].from))) return hk_str_from_cstr(m[i].to);
    }
    return s;
}

typedef struct foreign_attr {
    const char *qname;
    hk_attr_ns ns;
    const char *local;
} foreign_attr;

static const foreign_attr k_foreign_attrs[] = {
    {"xlink:actuate", HK_ATTR_NS_XLINK, "actuate"}, {"xlink:arcrole", HK_ATTR_NS_XLINK, "arcrole"},
    {"xlink:href", HK_ATTR_NS_XLINK, "href"},       {"xlink:role", HK_ATTR_NS_XLINK, "role"},
    {"xlink:show", HK_ATTR_NS_XLINK, "show"},       {"xlink:title", HK_ATTR_NS_XLINK, "title"},
    {"xlink:type", HK_ATTR_NS_XLINK, "type"},       {"xml:lang", HK_ATTR_NS_XML, "lang"},
    {"xml:space", HK_ATTR_NS_XML, "space"},         {"xmlns", HK_ATTR_NS_XMLNS, "xmlns"},
    {"xmlns:xlink", HK_ATTR_NS_XMLNS, "xlink"},
};

/* --- creating elements -------------------------------------------------------------------------- */

static hk_node_id create_element(P *p, const tok *t, hk_ns ns)
{
    hk_document *d = p->doc;
    hk_atom name = t->name;
    if (ns == HK_NS_SVG) {
        hk_str adjusted = map_name(k_svg_tags, HK_ARRAY_LEN(k_svg_tags), t->name_s);
        name = hk_atom_intern(&d->atoms, adjusted);
    } else if (!name) {
        name = hk_atom_intern(&d->atoms, t->name_s);
    }
    hk_node_id id = hk_dom_create_element(d, ns, name);
    for (uint32_t i = 0; i < t->attr_count; i++) {
        hk_str an = t->attrs[i].name;
        hk_attr_ns ans = HK_ATTR_NS_NONE;
        if (ns == HK_NS_MATHML && hk_str_eq(an, HK_STR("definitionurl"))) {
            an = HK_STR("definitionURL");
        } else if (ns == HK_NS_SVG) {
            an = map_name(k_svg_attrs, HK_ARRAY_LEN(k_svg_attrs), an);
        }
        if (ns != HK_NS_HTML) {
            for (size_t k = 0; k < HK_ARRAY_LEN(k_foreign_attrs); k++) {
                if (hk_str_eq(an, hk_str_from_cstr(k_foreign_attrs[k].qname))) {
                    ans = k_foreign_attrs[k].ns;
                    an = hk_str_from_cstr(k_foreign_attrs[k].local);
                    break;
                }
            }
        }
        hk_dom_add_attr(d, id, ans, hk_atom_intern(&d->atoms, an), t->attrs[i].value);
    }
    return id;
}

/* Recreates a formatting element from an existing one (same name and attributes). */
static hk_node_id clone_element(P *p, hk_node_id src)
{
    hk_document *d = p->doc;
    hk_node_id id = hk_dom_create_element(d, el_ns(p, src), el_name(p, src));
    const hk_element *e = hk_dom_element(d, src);
    uint32_t count = e->attr_count;
    for (uint32_t i = 0; i < count; i++) {
        hk_attr a = hk_dom_element(d, src)->attrs[i]; /* re-fetch: creation may move arrays */
        hk_dom_add_attr(d, id, (hk_attr_ns)a.ns, a.name, a.value);
    }
    return id;
}

/* HTML 13.2.6.1 "create an element for a token", step 12: the form owner of form-associated elements. */
static void associate_form(P *p, hk_node_id id)
{
    hk_atom n = el_name(p, id);
    if (!p->form || (n != A(button) && n != A(fieldset) && n != A(input) && n != A(object) && n != A(select) &&
                     n != A(textarea) && n != A(img)))
        return;
    if (stack_has(p, A(template)) || hk_dom_attr(p->doc, id, HK_ATTR_NS_NONE, A(form))) return;
    hk_form_link fl = {id, p->form};
    hk_vec_push(&p->doc->form_links, fl);
}

static hk_node_id insert_element_ns(P *p, const tok *t, hk_ns ns)
{
    location loc = insertion_location(p, HK_NODE_NONE);
    hk_node_id id = create_element(p, t, ns);
    if (ns == HK_NS_HTML) associate_form(p, id);
    insert_at(p, loc, id);
    push(p, id);
    return id;
}

static hk_node_id insert_html_element(P *p, const tok *t)
{
    return insert_element_ns(p, t, HK_NS_HTML);
}

/* --- active formatting elements ----------------------------------------------------------------- */

static bool same_attrs(P *p, hk_node_id a, hk_node_id b)
{
    const hk_element *ea = hk_dom_element(p->doc, a), *eb = hk_dom_element(p->doc, b);
    if (ea->attr_count != eb->attr_count) return false;
    for (uint32_t i = 0; i < ea->attr_count; i++) {
        const hk_attr *other = hk_dom_attr(p->doc, b, (hk_attr_ns)ea->attrs[i].ns, ea->attrs[i].name);
        if (!other || !hk_str_eq(other->value, ea->attrs[i].value)) return false;
    }
    return true;
}

static void afe_push(P *p, hk_node_id id)
{
    /* Noah's Ark clause: at most three identical entries after the last marker. */
    int count = 0, earliest = -1;
    for (int i = (int)p->afe.len - 1; i >= 0; i--) {
        hk_node_id e = p->afe.data[i];
        if (!e) break;
        if (el_ns(p, e) == el_ns(p, id) && el_name(p, e) == el_name(p, id) && same_attrs(p, e, id)) {
            count++;
            earliest = i;
        }
    }
    if (count >= 3) {
        memmove(&p->afe.data[earliest], &p->afe.data[earliest + 1],
                (p->afe.len - (uint32_t)earliest - 1) * sizeof(hk_node_id));
        p->afe.len--;
    }
    hk_vec_push(&p->afe, id);
}

static void afe_push_marker(P *p)
{
    hk_vec_push(&p->afe, HK_NODE_NONE);
}

static int afe_index(P *p, hk_node_id id)
{
    for (int i = (int)p->afe.len - 1; i >= 0; i--) {
        if (p->afe.data[i] == id) return i;
    }
    return -1;
}

static void afe_remove_at(P *p, int i)
{
    memmove(&p->afe.data[i], &p->afe.data[i + 1], (p->afe.len - (uint32_t)i - 1) * sizeof(hk_node_id));
    p->afe.len--;
}

static void afe_remove(P *p, hk_node_id id)
{
    int i = afe_index(p, id);
    if (i >= 0) afe_remove_at(p, i);
}

static void afe_clear_to_marker(P *p)
{
    while (p->afe.len) {
        hk_node_id e = p->afe.data[--p->afe.len];
        if (!e) return;
    }
}

static void reconstruct_afe(P *p)
{
    if (p->afe.len == 0) return;
    int i = (int)p->afe.len - 1;
    hk_node_id e = p->afe.data[i];
    if (!e || stack_index(p, e) >= 0) return;
    while (i > 0) {
        e = p->afe.data[i - 1];
        if (!e || stack_index(p, e) >= 0) break;
        i--;
    }
    for (; i < (int)p->afe.len; i++) {
        hk_node_id src = p->afe.data[i];
        location loc = insertion_location(p, HK_NODE_NONE);
        hk_node_id id = clone_element(p, src);
        insert_at(p, loc, id);
        push(p, id);
        p->afe.data[i] = id;
    }
}

/* Last formatting element with this name after the last marker, or 0. */
static hk_node_id afe_find(P *p, hk_atom name)
{
    for (int i = (int)p->afe.len - 1; i >= 0; i--) {
        hk_node_id e = p->afe.data[i];
        if (!e) return HK_NODE_NONE;
        if (is_html(p, e, name)) return e;
    }
    return HK_NODE_NONE;
}

/* --- adoption agency (13.2.6.4.7) -------------------------------------------------------------- */

static void any_other_end_tag_in_body(P *p, const tok *t);

static void move_children(P *p, hk_node_id from, hk_node_id to)
{
    hk_node_id c;
    while ((c = hk_dom_node(p->doc, from)->first_child) != HK_NODE_NONE) {
        hk_dom_remove(p->doc, c);
        hk_dom_append(p->doc, to, c);
    }
}

/* Returns false when the caller must act as for "any other end tag". */
static bool adoption_agency(P *p, const tok *t)
{
    hk_atom subject = t->name;
    hk_node_id cur = current(p);
    if (is_html(p, cur, subject) && afe_index(p, cur) < 0) {
        pop(p);
        return true;
    }
    for (int outer = 0; outer < 8; outer++) {
        hk_node_id fe = afe_find(p, subject);
        if (!fe) return false;
        int fe_stack = stack_index(p, fe);
        if (fe_stack < 0) {
            afe_remove(p, fe);
            return true;
        }
        if (!node_in_scope(p, fe)) return true;

        int fb_index = -1;
        for (uint32_t i = (uint32_t)fe_stack + 1; i < p->open.len; i++) {
            if (is_special(p, p->open.data[i])) {
                fb_index = (int)i;
                break;
            }
        }
        if (fb_index < 0) {
            while (p->open.len > (uint32_t)fe_stack) pop(p);
            afe_remove(p, fe);
            return true;
        }
        hk_node_id furthest = p->open.data[fb_index];
        hk_node_id common_ancestor = p->open.data[fe_stack - 1];
        int bookmark = afe_index(p, fe);

        hk_node_id node, last = furthest;
        int node_index = fb_index;
        for (int inner = 1;; inner++) {
            node_index--;
            node = p->open.data[node_index];
            if (node == fe) break;
            int ai = afe_index(p, node);
            if (inner > 3 && ai >= 0) {
                afe_remove_at(p, ai);
                if (ai < bookmark) bookmark--;
                ai = -1;
            }
            if (ai < 0) {
                stack_remove(p, node);
                continue;
            }
            hk_node_id clone = clone_element(p, node);
            p->afe.data[ai] = clone;
            p->open.data[node_index] = clone;
            node = clone;
            if (last == furthest) bookmark = ai + 1;
            hk_dom_remove(p->doc, last);
            hk_dom_append(p->doc, node, last);
            last = node;
        }

        hk_dom_remove(p->doc, last);
        insert_at(p, insertion_location(p, common_ancestor), last);

        hk_node_id clone = clone_element(p, fe);
        move_children(p, furthest, clone);
        hk_dom_append(p->doc, furthest, clone);

        int fe_afe = afe_index(p, fe);
        afe_remove_at(p, fe_afe);
        if (fe_afe < bookmark) bookmark--;
        hk_vec_reserve(&p->afe, (uint64_t)p->afe.len + 1);
        memmove(&p->afe.data[bookmark + 1], &p->afe.data[bookmark],
                (p->afe.len - (uint32_t)bookmark) * sizeof(hk_node_id));
        p->afe.data[bookmark] = clone;
        p->afe.len++;

        stack_remove(p, fe);
        stack_insert(p, (uint32_t)stack_index(p, furthest) + 1, clone);
    }
    return true;
}

/* --- shared algorithms -------------------------------------------------------------------------- */

static void close_p(P *p)
{
    generate_implied_end_tags(p, A(p));
    pop_until(p, A(p));
}

static void close_p_if_in_button_scope(P *p)
{
    if (in_scope(p, A(p), SCOPE_BUTTON)) close_p(p);
}

static void generic_text(P *p, const tok *t, hk_html_tok_state state)
{
    insert_html_element(p, t);
    hk_html_tokenizer_set_state(&p->tokenizer, state);
    p->original_mode = p->mode;
    p->mode = M_TEXT;
}

static void reset_insertion_mode(P *p)
{
    for (int i = (int)p->open.len - 1; i >= 0; i--) {
        hk_node_id node = p->open.data[i];
        bool last = i == 0;
        if (el_ns(p, node) != HK_NS_HTML) {
            if (last) break;
            continue;
        }
        hk_atom n = el_name(p, node);
        if ((n == A(td) || n == A(th)) && !last) p->mode = M_IN_CELL;
        else if (n == A(tr)) p->mode = M_IN_ROW;
        else if (n == A(tbody) || n == A(thead) || n == A(tfoot)) p->mode = M_IN_TABLE_BODY;
        else if (n == A(caption)) p->mode = M_IN_CAPTION;
        else if (n == A(colgroup)) p->mode = M_IN_COLUMN_GROUP;
        else if (n == A(table)) p->mode = M_IN_TABLE;
        else if (n == A(template)) p->mode = (mode)p->template_modes.data[p->template_modes.len - 1];
        else if (n == A(head) && !last) p->mode = M_IN_HEAD;
        else if (n == A(body)) p->mode = M_IN_BODY;
        else if (n == A(frameset)) p->mode = M_IN_FRAMESET;
        else if (n == A(html)) p->mode = p->head ? M_AFTER_HEAD : M_BEFORE_HEAD;
        else if (last) p->mode = M_IN_BODY;
        else continue;
        return;
    }
    p->mode = M_IN_BODY;
}

static void push_template_mode(P *p, mode m)
{
    hk_vec_push(&p->template_modes, (uint8_t)m);
}

static void pop_template_mode(P *p)
{
    if (p->template_modes.len) p->template_modes.len--;
}

static void stop_parsing(P *p)
{
    while (p->open.len) pop(p);
    p->stopped = true;
}

/* --- customizable <select>: mirror the selected option into <selectedcontent> ---------------- */

static hk_node_id next_in_tree_order(P *p, hk_node_id node, hk_node_id root)
{
    const hk_node *n = hk_dom_node(p->doc, node);
    if (n->first_child) return n->first_child;
    while (node != root) {
        n = hk_dom_node(p->doc, node);
        if (n->next) return n->next;
        node = n->parent;
    }
    return HK_NODE_NONE;
}

static hk_node_id clone_deep(P *p, hk_node_id src)
{
    const hk_node *n = hk_dom_node(p->doc, src);
    if (n->type == HK_NODE_TEXT || n->type == HK_NODE_COMMENT) {
        hk_str text = hk_dom_text(p->doc, src);
        return n->type == HK_NODE_TEXT ? hk_dom_create_text(p->doc, text.p, text.n)
                                       : hk_dom_create_comment(p->doc, text.p, text.n);
    }
    hk_node_id copy = clone_element(p, src);
    for (hk_node_id c = hk_dom_node(p->doc, src)->first_child; c; c = hk_dom_node(p->doc, c)->next)
        hk_dom_append(p->doc, copy, clone_deep(p, c));
    return copy;
}

/* Runs when an option is popped off the stack ("maybe clone an option into selectedcontent"). */
static void maybe_clone_option_into_selectedcontent(P *p, hk_node_id option)
{
    hk_node_id select = hk_dom_node(p->doc, option)->parent;
    while (select && !is_html(p, select, A(select))) {
        const hk_node *n = hk_dom_node(p->doc, select);
        select = n->type == HK_NODE_ELEMENT ? n->parent : HK_NODE_NONE;
    }
    if (!select) return;

    hk_node_id first = HK_NODE_NONE, selected = HK_NODE_NONE, target = HK_NODE_NONE;
    for (hk_node_id n = next_in_tree_order(p, select, select); n; n = next_in_tree_order(p, n, select)) {
        if (is_html(p, n, A(option))) {
            if (!first) first = n;
            if (hk_dom_attr(p->doc, n, HK_ATTR_NS_NONE, hk_atom_find(&p->doc->atoms, HK_STR("selected"))))
                selected = n;
        } else if (!target && is_html(p, n, A(selectedcontent))) {
            target = n;
        }
    }
    if (!selected) selected = first;
    if (selected != option || !target) return;

    hk_node_id c;
    while ((c = hk_dom_node(p->doc, target)->first_child) != HK_NODE_NONE) hk_dom_remove(p->doc, c);
    for (c = hk_dom_node(p->doc, option)->first_child; c; c = hk_dom_node(p->doc, c)->next)
        hk_dom_append(p->doc, target, clone_deep(p, c));
}

/* --- quirks mode (13.2.6.4.1) --------------------------------------------------------------------- */

static bool starts_icase(hk_str s, const char *prefix)
{
    hk_str pre = hk_str_from_cstr(prefix);
    return s.n >= pre.n && hk_str_eq_ascii_icase(hk_str_slice(s, 0, pre.n), pre);
}

static hk_quirks_mode doctype_quirks(const hk_html_token *d)
{
    static const char *const k_quirky_prefixes[] = {
        "+//Silmaril//dtd html Pro v0r11 19970101//",
        "-//AS//DTD HTML 3.0 asWedit + extensions//",
        "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
        "-//IETF//DTD HTML 2.0 Level 1//",
        "-//IETF//DTD HTML 2.0 Level 2//",
        "-//IETF//DTD HTML 2.0 Strict Level 1//",
        "-//IETF//DTD HTML 2.0 Strict Level 2//",
        "-//IETF//DTD HTML 2.0 Strict//",
        "-//IETF//DTD HTML 2.0//",
        "-//IETF//DTD HTML 2.1E//",
        "-//IETF//DTD HTML 3.0//",
        "-//IETF//DTD HTML 3.2 Final//",
        "-//IETF//DTD HTML 3.2//",
        "-//IETF//DTD HTML 3//",
        "-//IETF//DTD HTML Level 0//",
        "-//IETF//DTD HTML Level 1//",
        "-//IETF//DTD HTML Level 2//",
        "-//IETF//DTD HTML Level 3//",
        "-//IETF//DTD HTML Strict Level 0//",
        "-//IETF//DTD HTML Strict Level 1//",
        "-//IETF//DTD HTML Strict Level 2//",
        "-//IETF//DTD HTML Strict Level 3//",
        "-//IETF//DTD HTML Strict//",
        "-//IETF//DTD HTML//",
        "-//Metrius//DTD Metrius Presentational//",
        "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
        "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
        "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
        "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
        "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
        "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
        "-//Netscape Comm. Corp.//DTD HTML//",
        "-//Netscape Comm. Corp.//DTD Strict HTML//",
        "-//O'Reilly and Associates//DTD HTML 2.0//",
        "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
        "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
        "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
        "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
        "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
        "-//Spyglass//DTD HTML 2.0 Extended//",
        "-//Sun Microsystems Corp.//DTD HotJava HTML//",
        "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
        "-//W3C//DTD HTML 3 1995-03-24//",
        "-//W3C//DTD HTML 3.2 Draft//",
        "-//W3C//DTD HTML 3.2 Final//",
        "-//W3C//DTD HTML 3.2//",
        "-//W3C//DTD HTML 3.2S Draft//",
        "-//W3C//DTD HTML 4.0 Frameset//",
        "-//W3C//DTD HTML 4.0 Transitional//",
        "-//W3C//DTD HTML Experimental 19960712//",
        "-//W3C//DTD HTML Experimental 970421//",
        "-//W3C//DTD W3 HTML//",
        "-//W3O//DTD W3 HTML 3.0//",
        "-//WebTechs//DTD Mozilla HTML 2.0//",
        "-//WebTechs//DTD Mozilla HTML//",
    };
    if (d->force_quirks || !d->has_name || !hk_str_eq(d->name, HK_STR("html"))) return HK_QUIRKS_FULL;
    hk_str pub = d->public_id, sys = d->system_id;
    if (d->has_public) {
        if (hk_str_eq_ascii_icase(pub, HK_STR("-//W3O//DTD W3 HTML Strict 3.0//EN//")) ||
            hk_str_eq_ascii_icase(pub, HK_STR("-/W3C/DTD HTML 4.0 Transitional/EN")) ||
            hk_str_eq_ascii_icase(pub, HK_STR("HTML")))
            return HK_QUIRKS_FULL;
        for (size_t i = 0; i < HK_ARRAY_LEN(k_quirky_prefixes); i++) {
            if (starts_icase(pub, k_quirky_prefixes[i])) return HK_QUIRKS_FULL;
        }
        bool frameset_or_transitional = starts_icase(pub, "-//W3C//DTD HTML 4.01 Frameset//") ||
                                        starts_icase(pub, "-//W3C//DTD HTML 4.01 Transitional//");
        if (!d->has_system && frameset_or_transitional) return HK_QUIRKS_FULL;
        if (starts_icase(pub, "-//W3C//DTD XHTML 1.0 Frameset//") ||
            starts_icase(pub, "-//W3C//DTD XHTML 1.0 Transitional//"))
            return HK_QUIRKS_LIMITED;
        if (d->has_system && frameset_or_transitional) return HK_QUIRKS_LIMITED;
    }
    if (d->has_system &&
        hk_str_eq_ascii_icase(sys, HK_STR("http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")))
        return HK_QUIRKS_FULL;
    return HK_QUIRKS_NO;
}

/* --- insertion modes --------------------------------------------------------------------------- */

static void reprocess_as(P *p, mode m, const tok *t)
{
    p->mode = m;
    process(p, t);
}

static void mode_initial(P *p, const tok *t)
{
    if (is_ws_char(t)) return;
    if (t->type == HK_TOK_COMMENT) {
        append_comment_to(p, t, p->doc->root);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE) {
        const hk_html_token *r = t->raw;
        hk_doctype dt = {r->name, r->public_id, r->system_id, r->has_public, r->has_system};
        hk_dom_append(p->doc, p->doc->root, hk_dom_create_doctype(p->doc, &dt));
        p->doc->quirks = (uint8_t)doctype_quirks(r);
        p->mode = M_BEFORE_HTML;
        return;
    }
    p->doc->quirks = HK_QUIRKS_FULL;
    reprocess_as(p, M_BEFORE_HTML, t);
}

static void mode_before_html(P *p, const tok *t)
{
    if (t->type == HK_TOK_DOCTYPE || is_ws_char(t)) return;
    if (t->type == HK_TOK_COMMENT) {
        append_comment_to(p, t, p->doc->root);
        return;
    }
    if (is_start(t, A(html))) {
        hk_node_id html = create_element(p, t, HK_NS_HTML);
        hk_dom_append(p->doc, p->doc->root, html);
        push(p, html);
        p->mode = M_BEFORE_HEAD;
        return;
    }
    if (t->type == HK_TOK_END_TAG && !TOK_ONE_OF(t, A(head), A(body), A(html), A(br))) return;
    tok fake = fake_start(p, A(html));
    hk_node_id html = create_element(p, &fake, HK_NS_HTML);
    hk_dom_append(p->doc, p->doc->root, html);
    push(p, html);
    reprocess_as(p, M_BEFORE_HEAD, t);
}

static void mode_before_head(P *p, const tok *t)
{
    if (t->type == HK_TOK_DOCTYPE || is_ws_char(t)) return;
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (is_start(t, A(head))) {
        p->head = insert_html_element(p, t);
        p->mode = M_IN_HEAD;
        return;
    }
    if (t->type == HK_TOK_END_TAG && !TOK_ONE_OF(t, A(head), A(body), A(html), A(br))) return;
    tok fake = fake_start(p, A(head));
    p->head = insert_html_element(p, &fake);
    reprocess_as(p, M_IN_HEAD, t);
}

static void mode_in_head(P *p, const tok *t)
{
    if (is_ws_char(t)) {
        insert_chars(p, t->data.p, t->data.n);
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE) return;
    if (t->type == HK_TOK_START_TAG) {
        if (tok_is(t, A(html))) {
            process_in(p, M_IN_BODY, t);
            return;
        }
        if (TOK_ONE_OF(t, A(base), A(basefont), A(bgsound), A(link), A(meta))) {
            insert_html_element(p, t);
            pop(p);
            return;
        }
        if (tok_is(t, A(title))) {
            generic_text(p, t, HK_TS_RCDATA);
            return;
        }
        if (TOK_ONE_OF(t, A(noframes), A(style))) {
            generic_text(p, t, HK_TS_RAWTEXT);
            return;
        }
        if (tok_is(t, A(noscript))) { /* scripting disabled */
            insert_html_element(p, t);
            p->mode = M_IN_HEAD_NOSCRIPT;
            return;
        }
        if (tok_is(t, A(script))) {
            generic_text(p, t, HK_TS_SCRIPT_DATA);
            return;
        }
        if (tok_is(t, A(template))) {
            insert_html_element(p, t);
            afe_push_marker(p);
            p->frameset_ok = false;
            p->mode = M_IN_TEMPLATE;
            push_template_mode(p, M_IN_TEMPLATE);
            return;
        }
        if (tok_is(t, A(head))) return;
    }
    if (t->type == HK_TOK_END_TAG) {
        if (tok_is(t, A(head))) {
            pop(p);
            p->mode = M_AFTER_HEAD;
            return;
        }
        if (tok_is(t, A(template))) {
            if (!stack_has(p, A(template))) return;
            generate_implied_end_tags_thoroughly(p);
            pop_until(p, A(template));
            afe_clear_to_marker(p);
            pop_template_mode(p);
            reset_insertion_mode(p);
            return;
        }
        if (!TOK_ONE_OF(t, A(body), A(html), A(br))) return;
    }
    pop(p);
    reprocess_as(p, M_AFTER_HEAD, t);
}

static void mode_in_head_noscript(P *p, const tok *t)
{
    if (t->type == HK_TOK_DOCTYPE) return;
    if (is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (is_end(t, A(noscript))) {
        pop(p);
        p->mode = M_IN_HEAD;
        return;
    }
    if (is_ws_char(t) || t->type == HK_TOK_COMMENT ||
        (t->type == HK_TOK_START_TAG &&
         TOK_ONE_OF(t, A(basefont), A(bgsound), A(link), A(meta), A(noframes), A(style)))) {
        process_in(p, M_IN_HEAD, t);
        return;
    }
    if (t->type == HK_TOK_START_TAG && TOK_ONE_OF(t, A(head), A(noscript))) return;
    if (t->type == HK_TOK_END_TAG && !tok_is(t, A(br))) return;
    pop(p);
    reprocess_as(p, M_IN_HEAD, t);
}

static void mode_after_head(P *p, const tok *t)
{
    if (is_ws_char(t)) {
        insert_chars(p, t->data.p, t->data.n);
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE) return;
    if (t->type == HK_TOK_START_TAG) {
        if (tok_is(t, A(html))) {
            process_in(p, M_IN_BODY, t);
            return;
        }
        if (tok_is(t, A(body))) {
            insert_html_element(p, t);
            p->frameset_ok = false;
            p->mode = M_IN_BODY;
            return;
        }
        if (tok_is(t, A(frameset))) {
            insert_html_element(p, t);
            p->mode = M_IN_FRAMESET;
            return;
        }
        if (TOK_ONE_OF(t, A(base), A(basefont), A(bgsound), A(link), A(meta), A(noframes),
                       A(script), A(style), A(template), A(title))) {
            push(p, p->head);
            process_in(p, M_IN_HEAD, t);
            stack_remove(p, p->head);
            return;
        }
        if (tok_is(t, A(head))) return;
    }
    if (t->type == HK_TOK_END_TAG) {
        if (tok_is(t, A(template))) {
            process_in(p, M_IN_HEAD, t);
            return;
        }
        if (!TOK_ONE_OF(t, A(body), A(html), A(br))) return;
    }
    tok fake = fake_start(p, A(body));
    insert_html_element(p, &fake);
    reprocess_as(p, M_IN_BODY, t);
}

/* "in body" handling of a run of characters (no U+0000 inside). */
static void body_chars(P *p, const char *s, size_t n)
{
    reconstruct_afe(p);
    insert_chars(p, s, n);
    for (size_t i = 0; i < n && p->frameset_ok; i++) {
        char c = s[i];
        if (!(c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ')) p->frameset_ok = false;
    }
}

static void any_other_end_tag_in_body(P *p, const tok *t)
{
    for (int i = (int)p->open.len - 1; i >= 0; i--) {
        hk_node_id node = p->open.data[i];
        if (t->name && is_html(p, node, t->name)) {
            generate_implied_end_tags(p, t->name);
            while (p->open.len > (uint32_t)i) pop(p);
            return;
        }
        if (is_special(p, node)) return;
    }
}

static void close_select_if_in_scope(P *p)
{
    if (in_scope(p, A(select), SCOPE_DEFAULT)) pop_until(p, A(select));
}

static void body_start_tag(P *p, const tok *t)
{
    hk_document *d = p->doc;
    hk_atom n = t->name;

    if (n == A(html)) {
        if (stack_has(p, A(template))) return;
        for (uint32_t i = 0; i < t->attr_count; i++) {
            hk_dom_add_attr(d, p->open.data[0], HK_ATTR_NS_NONE,
                            hk_atom_intern(&d->atoms, t->attrs[i].name), t->attrs[i].value);
        }
        return;
    }
    if (TOK_ONE_OF(t, A(base), A(basefont), A(bgsound), A(link), A(meta), A(noframes), A(script),
                   A(style), A(template), A(title))) {
        process_in(p, M_IN_HEAD, t);
        return;
    }
    if (n == A(body)) {
        if (p->open.len < 2 || !is_html(p, p->open.data[1], A(body)) || stack_has(p, A(template)))
            return;
        p->frameset_ok = false;
        for (uint32_t i = 0; i < t->attr_count; i++) {
            hk_dom_add_attr(d, p->open.data[1], HK_ATTR_NS_NONE,
                            hk_atom_intern(&d->atoms, t->attrs[i].name), t->attrs[i].value);
        }
        return;
    }
    if (n == A(frameset)) {
        if (p->open.len < 2 || !is_html(p, p->open.data[1], A(body)) || !p->frameset_ok) return;
        hk_dom_remove(d, p->open.data[1]);
        while (p->open.len > 1) pop(p);
        insert_html_element(p, t);
        p->mode = M_IN_FRAMESET;
        return;
    }
    if (TOK_ONE_OF(t, A(address), A(article), A(aside), A(blockquote), A(center), A(details),
                   A(dialog), A(dir), A(div), A(dl), A(fieldset), A(figcaption), A(figure),
                   A(footer), A(header), A(hgroup), A(main), A(menu), A(nav), A(ol), A(p),
                   A(search), A(section), A(summary), A(ul))) {
        close_p_if_in_button_scope(p);
        insert_html_element(p, t);
        return;
    }
    if (tok_one_of(t, k_headings, HK_ARRAY_LEN(k_headings))) {
        close_p_if_in_button_scope(p);
        if (is_html_one_of(p, current(p), k_headings, HK_ARRAY_LEN(k_headings))) pop(p);
        insert_html_element(p, t);
        return;
    }
    if (n == A(pre) || n == A(listing)) {
        close_p_if_in_button_scope(p);
        insert_html_element(p, t);
        p->ignore_lf = true;
        p->frameset_ok = false;
        return;
    }
    if (n == A(form)) {
        bool has_template = stack_has(p, A(template));
        if (p->form && !has_template) return;
        close_p_if_in_button_scope(p);
        hk_node_id f = insert_html_element(p, t);
        if (!has_template) p->form = f;
        return;
    }
    if (n == A(li) || n == A(dd) || n == A(dt)) {
        p->frameset_ok = false;
        for (int i = (int)p->open.len - 1; i >= 0; i--) {
            hk_node_id node = p->open.data[i];
            hk_atom target = HK_ATOM_NONE_;
            if (n == A(li) && is_html(p, node, A(li))) target = A(li);
            if (n != A(li) && ONE_OF(p, node, A(dd), A(dt))) target = el_name(p, node);
            if (target) {
                generate_implied_end_tags(p, target);
                pop_until(p, target);
                break;
            }
            if (is_special(p, node) && !ONE_OF(p, node, A(address), A(div), A(p))) break;
        }
        close_p_if_in_button_scope(p);
        insert_html_element(p, t);
        return;
    }
    if (n == A(plaintext)) {
        close_p_if_in_button_scope(p);
        insert_html_element(p, t);
        hk_html_tokenizer_set_state(&p->tokenizer, HK_TS_PLAINTEXT);
        return;
    }
    if (n == A(button)) {
        if (in_scope(p, A(button), SCOPE_DEFAULT)) {
            generate_implied_end_tags(p, 0);
            pop_until(p, A(button));
        }
        reconstruct_afe(p);
        insert_html_element(p, t);
        p->frameset_ok = false;
        return;
    }
    if (n == A(a)) {
        hk_node_id existing = afe_find(p, A(a));
        if (existing) {
            if (!adoption_agency(p, t)) any_other_end_tag_in_body(p, t);
            afe_remove(p, existing);
            stack_remove(p, existing);
        }
        reconstruct_afe(p);
        afe_push(p, insert_html_element(p, t));
        return;
    }
    if (is_formatting_name(n) && n != A(nobr)) {
        reconstruct_afe(p);
        afe_push(p, insert_html_element(p, t));
        return;
    }
    if (n == A(nobr)) {
        reconstruct_afe(p);
        if (in_scope(p, A(nobr), SCOPE_DEFAULT)) {
            if (!adoption_agency(p, t)) any_other_end_tag_in_body(p, t);
            reconstruct_afe(p);
        }
        afe_push(p, insert_html_element(p, t));
        return;
    }
    if (n == A(applet) || n == A(marquee) || n == A(object)) {
        reconstruct_afe(p);
        insert_html_element(p, t);
        afe_push_marker(p);
        p->frameset_ok = false;
        return;
    }
    if (n == A(table)) {
        if (d->quirks != HK_QUIRKS_FULL) close_p_if_in_button_scope(p);
        insert_html_element(p, t);
        p->frameset_ok = false;
        p->mode = M_IN_TABLE;
        return;
    }
    if (TOK_ONE_OF(t, A(area), A(br), A(embed), A(img), A(keygen), A(wbr), A(input))) {
        if (n == A(input)) close_select_if_in_scope(p);
        reconstruct_afe(p);
        insert_html_element(p, t);
        pop(p);
        if (n == A(input)) {
            const hk_html_tok_attr *type = tok_attr(t, "type");
            if (!type || !hk_str_eq_ascii_icase(type->value, HK_STR("hidden"))) p->frameset_ok = false;
        } else {
            p->frameset_ok = false;
        }
        return;
    }
    if (TOK_ONE_OF(t, A(param), A(source), A(track))) {
        insert_html_element(p, t);
        pop(p);
        return;
    }
    if (n == A(hr)) {
        close_p_if_in_button_scope(p);
        if (in_scope(p, A(select), SCOPE_DEFAULT)) generate_implied_end_tags(p, 0);
        insert_html_element(p, t);
        pop(p);
        p->frameset_ok = false;
        return;
    }
    if (n == A(image)) {
        tok img = *t;
        img.name = A(img);
        img.name_s = HK_STR("img");
        process(p, &img);
        return;
    }
    if (n == A(textarea)) {
        close_select_if_in_scope(p);
        insert_html_element(p, t);
        p->ignore_lf = true;
        hk_html_tokenizer_set_state(&p->tokenizer, HK_TS_RCDATA);
        p->original_mode = p->mode;
        p->frameset_ok = false;
        p->mode = M_TEXT;
        return;
    }
    if (n == A(xmp)) {
        close_p_if_in_button_scope(p);
        reconstruct_afe(p);
        p->frameset_ok = false;
        generic_text(p, t, HK_TS_RAWTEXT);
        return;
    }
    if (n == A(iframe)) {
        p->frameset_ok = false;
        generic_text(p, t, HK_TS_RAWTEXT);
        return;
    }
    if (n == A(noembed)) {
        generic_text(p, t, HK_TS_RAWTEXT);
        return;
    }
    if (n == A(select)) {
        if (in_scope(p, A(select), SCOPE_DEFAULT)) {
            pop_until(p, A(select));
            return;
        }
        reconstruct_afe(p);
        insert_html_element(p, t);
        p->frameset_ok = false;
        return;
    }
    if (n == A(option)) {
        if (in_scope(p, A(select), SCOPE_DEFAULT)) generate_implied_end_tags(p, A(optgroup));
        else if (cur_is(p, A(option))) pop(p);
        reconstruct_afe(p);
        insert_html_element(p, t);
        return;
    }
    if (n == A(optgroup)) {
        if (in_scope(p, A(select), SCOPE_DEFAULT)) generate_implied_end_tags(p, 0);
        else if (cur_is(p, A(option))) pop(p);
        reconstruct_afe(p);
        insert_html_element(p, t);
        return;
    }
    if (n == A(rb) || n == A(rtc)) {
        if (in_scope(p, A(ruby), SCOPE_DEFAULT)) generate_implied_end_tags(p, 0);
        insert_html_element(p, t);
        return;
    }
    if (n == A(rp) || n == A(rt)) {
        if (in_scope(p, A(ruby), SCOPE_DEFAULT)) generate_implied_end_tags(p, A(rtc));
        insert_html_element(p, t);
        return;
    }
    if (n == A(math) || n == A(svg)) {
        reconstruct_afe(p);
        insert_element_ns(p, t, n == A(math) ? HK_NS_MATHML : HK_NS_SVG);
        if (t->self_closing) pop(p);
        return;
    }
    if (TOK_ONE_OF(t, A(caption), A(col), A(colgroup), A(frame), A(head), A(tbody), A(td),
                   A(tfoot), A(th), A(thead), A(tr)))
        return;
    reconstruct_afe(p);
    insert_html_element(p, t);
}

static void body_end_tag(P *p, const tok *t)
{
    hk_atom n = t->name;
    if (n == A(template)) {
        process_in(p, M_IN_HEAD, t);
        return;
    }
    if (n == A(body) || n == A(html)) {
        if (!in_scope(p, A(body), SCOPE_DEFAULT)) return;
        p->mode = M_AFTER_BODY;
        if (n == A(html)) process(p, t);
        return;
    }
    if (TOK_ONE_OF(t, A(address), A(article), A(aside), A(blockquote), A(button), A(center),
                   A(details), A(dialog), A(dir), A(div), A(dl), A(fieldset), A(figcaption),
                   A(figure), A(footer), A(header), A(hgroup), A(listing), A(main), A(menu),
                   A(nav), A(ol), A(pre), A(search), A(section), A(select), A(summary), A(ul))) {
        if (!in_scope(p, n, SCOPE_DEFAULT)) return;
        generate_implied_end_tags(p, 0);
        pop_until(p, n);
        return;
    }
    if (n == A(form)) {
        if (!stack_has(p, A(template))) {
            hk_node_id node = p->form;
            p->form = HK_NODE_NONE;
            if (!node || !node_in_scope(p, node)) return;
            generate_implied_end_tags(p, 0);
            stack_remove(p, node);
        } else {
            if (!in_scope(p, A(form), SCOPE_DEFAULT)) return;
            generate_implied_end_tags(p, 0);
            pop_until(p, A(form));
        }
        return;
    }
    if (n == A(p)) {
        if (!in_scope(p, A(p), SCOPE_BUTTON)) {
            tok fake = fake_start(p, A(p));
            insert_html_element(p, &fake);
        }
        close_p(p);
        return;
    }
    if (n == A(li)) {
        if (!in_scope(p, A(li), SCOPE_LIST_ITEM)) return;
        generate_implied_end_tags(p, A(li));
        pop_until(p, A(li));
        return;
    }
    if (n == A(dd) || n == A(dt)) {
        if (!in_scope(p, n, SCOPE_DEFAULT)) return;
        generate_implied_end_tags(p, n);
        pop_until(p, n);
        return;
    }
    if (tok_one_of(t, k_headings, HK_ARRAY_LEN(k_headings))) {
        if (!in_scope_names(p, k_headings, HK_ARRAY_LEN(k_headings), SCOPE_DEFAULT)) return;
        generate_implied_end_tags(p, 0);
        pop_until_one_of(p, k_headings, HK_ARRAY_LEN(k_headings));
        return;
    }
    if (is_formatting_name(n)) {
        if (!adoption_agency(p, t)) any_other_end_tag_in_body(p, t);
        return;
    }
    if (n == A(applet) || n == A(marquee) || n == A(object)) {
        if (!in_scope(p, n, SCOPE_DEFAULT)) return;
        generate_implied_end_tags(p, 0);
        pop_until(p, n);
        afe_clear_to_marker(p);
        return;
    }
    if (n == A(br)) {
        tok fake = fake_start(p, A(br));
        body_start_tag(p, &fake);
        return;
    }
    any_other_end_tag_in_body(p, t);
}

static void mode_in_body(P *p, const tok *t)
{
    switch (t->type) {
    case HK_TOK_CHARS:
        if (t->data.n == 1 && t->data.p[0] == '\0') return;
        body_chars(p, t->data.p, t->data.n);
        return;
    case HK_TOK_COMMENT: insert_comment(p, t); return;
    case HK_TOK_DOCTYPE: return;
    case HK_TOK_START_TAG: body_start_tag(p, t); return;
    case HK_TOK_END_TAG: body_end_tag(p, t); return;
    case HK_TOK_EOF:
        if (p->template_modes.len) process_in(p, M_IN_TEMPLATE, t);
        else stop_parsing(p);
        return;
    }
}

static void mode_text(P *p, const tok *t)
{
    if (t->type == HK_TOK_CHARS) {
        insert_chars(p, t->data.p, t->data.n);
        return;
    }
    if (t->type == HK_TOK_EOF) {
        pop(p);
        reprocess_as(p, p->original_mode, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG) {
        pop(p);
        p->mode = p->original_mode;
    }
}

static void clear_to_table_context(P *p)
{
    while (!ONE_OF(p, current(p), A(table), A(template), A(html))) pop(p);
}

static void clear_to_table_body_context(P *p)
{
    while (!ONE_OF(p, current(p), A(tbody), A(tfoot), A(thead), A(template), A(html))) pop(p);
}

static void clear_to_row_context(P *p)
{
    while (!ONE_OF(p, current(p), A(tr), A(template), A(html))) pop(p);
}

static void table_anything_else(P *p, const tok *t)
{
    p->foster_parenting = true;
    process_in(p, M_IN_BODY, t);
    p->foster_parenting = false;
}

static void mode_in_table(P *p, const tok *t)
{
    if (t->type == HK_TOK_CHARS &&
        ONE_OF(p, current(p), A(table), A(tbody), A(template), A(tfoot), A(thead), A(tr))) {
        p->table_text.len = 0;
        p->table_text_non_ws = false;
        p->original_mode = p->mode;
        reprocess_as(p, M_IN_TABLE_TEXT, t);
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE) return;
    if (t->type == HK_TOK_START_TAG) {
        hk_atom n = t->name;
        if (n == A(caption)) {
            clear_to_table_context(p);
            afe_push_marker(p);
            insert_html_element(p, t);
            p->mode = M_IN_CAPTION;
            return;
        }
        if (n == A(colgroup)) {
            clear_to_table_context(p);
            insert_html_element(p, t);
            p->mode = M_IN_COLUMN_GROUP;
            return;
        }
        if (n == A(col)) {
            clear_to_table_context(p);
            tok fake = fake_start(p, A(colgroup));
            insert_html_element(p, &fake);
            reprocess_as(p, M_IN_COLUMN_GROUP, t);
            return;
        }
        if (TOK_ONE_OF(t, A(tbody), A(tfoot), A(thead))) {
            clear_to_table_context(p);
            insert_html_element(p, t);
            p->mode = M_IN_TABLE_BODY;
            return;
        }
        if (TOK_ONE_OF(t, A(td), A(th), A(tr))) {
            clear_to_table_context(p);
            tok fake = fake_start(p, A(tbody));
            insert_html_element(p, &fake);
            reprocess_as(p, M_IN_TABLE_BODY, t);
            return;
        }
        if (n == A(table)) {
            if (!in_scope(p, A(table), SCOPE_TABLE)) return;
            pop_until(p, A(table));
            reset_insertion_mode(p);
            process(p, t);
            return;
        }
        if (TOK_ONE_OF(t, A(style), A(script), A(template))) {
            process_in(p, M_IN_HEAD, t);
            return;
        }
        if (n == A(input)) {
            const hk_html_tok_attr *type = tok_attr(t, "type");
            if (type && hk_str_eq_ascii_icase(type->value, HK_STR("hidden"))) {
                insert_html_element(p, t);
                pop(p);
                return;
            }
        }
        if (n == A(form)) {
            if (stack_has(p, A(template)) || p->form) return;
            p->form = insert_html_element(p, t);
            pop(p);
            return;
        }
    }
    if (t->type == HK_TOK_END_TAG) {
        if (tok_is(t, A(table))) {
            if (!in_scope(p, A(table), SCOPE_TABLE)) return;
            pop_until(p, A(table));
            reset_insertion_mode(p);
            return;
        }
        if (TOK_ONE_OF(t, A(body), A(caption), A(col), A(colgroup), A(html), A(tbody), A(td),
                       A(tfoot), A(th), A(thead), A(tr)))
            return;
        if (tok_is(t, A(template))) {
            process_in(p, M_IN_HEAD, t);
            return;
        }
    }
    if (t->type == HK_TOK_EOF) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    table_anything_else(p, t);
}

static void mode_in_table_text(P *p, const tok *t)
{
    if (t->type == HK_TOK_CHARS) {
        if (t->data.n == 1 && t->data.p[0] == '\0') return;
        hk_charbuf_append(&p->table_text, t->data.p, t->data.n);
        if (!is_ws_char(t)) p->table_text_non_ws = true;
        return;
    }
    if (p->table_text.len) {
        if (p->table_text_non_ws) {
            /* "Anything else" in the table: in body character handling with foster parenting. */
            p->foster_parenting = true;
            body_chars(p, p->table_text.data, p->table_text.len);
            p->foster_parenting = false;
        } else {
            insert_chars(p, p->table_text.data, p->table_text.len);
        }
        p->table_text.len = 0;
    }
    reprocess_as(p, p->original_mode, t);
}

static void mode_in_caption(P *p, const tok *t)
{
    bool end_caption = is_end(t, A(caption));
    bool closes = (t->type == HK_TOK_START_TAG &&
                   TOK_ONE_OF(t, A(caption), A(col), A(colgroup), A(tbody), A(td), A(tfoot),
                              A(th), A(thead), A(tr))) ||
                  is_end(t, A(table));
    if (end_caption || closes) {
        if (!in_scope(p, A(caption), SCOPE_TABLE)) return;
        generate_implied_end_tags(p, 0);
        pop_until(p, A(caption));
        afe_clear_to_marker(p);
        p->mode = M_IN_TABLE;
        if (closes) process(p, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG &&
        TOK_ONE_OF(t, A(body), A(col), A(colgroup), A(html), A(tbody), A(td), A(tfoot), A(th),
                   A(thead), A(tr)))
        return;
    process_in(p, M_IN_BODY, t);
}

static void mode_in_column_group(P *p, const tok *t)
{
    if (is_ws_char(t)) {
        insert_chars(p, t->data.p, t->data.n);
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE) return;
    if (is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (is_start(t, A(col))) {
        insert_html_element(p, t);
        pop(p);
        return;
    }
    if (is_end(t, A(colgroup))) {
        if (!cur_is(p, A(colgroup))) return;
        pop(p);
        p->mode = M_IN_TABLE;
        return;
    }
    if (is_end(t, A(col))) return;
    if (is_start(t, A(template)) || is_end(t, A(template))) {
        process_in(p, M_IN_HEAD, t);
        return;
    }
    if (t->type == HK_TOK_EOF) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (!cur_is(p, A(colgroup))) return;
    pop(p);
    reprocess_as(p, M_IN_TABLE, t);
}

static void mode_in_table_body(P *p, const tok *t)
{
    if (is_start(t, A(tr))) {
        clear_to_table_body_context(p);
        insert_html_element(p, t);
        p->mode = M_IN_ROW;
        return;
    }
    if (t->type == HK_TOK_START_TAG && TOK_ONE_OF(t, A(th), A(td))) {
        clear_to_table_body_context(p);
        tok fake = fake_start(p, A(tr));
        insert_html_element(p, &fake);
        reprocess_as(p, M_IN_ROW, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG && TOK_ONE_OF(t, A(tbody), A(tfoot), A(thead))) {
        if (!in_scope(p, t->name, SCOPE_TABLE)) return;
        clear_to_table_body_context(p);
        pop(p);
        p->mode = M_IN_TABLE;
        return;
    }
    if ((t->type == HK_TOK_START_TAG &&
         TOK_ONE_OF(t, A(caption), A(col), A(colgroup), A(tbody), A(tfoot), A(thead))) ||
        is_end(t, A(table))) {
        static const hk_atom k_sections[] = {A(tbody), A(thead), A(tfoot)};
        if (!in_scope_names(p, k_sections, 3, SCOPE_TABLE)) return;
        clear_to_table_body_context(p);
        pop(p);
        reprocess_as(p, M_IN_TABLE, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG &&
        TOK_ONE_OF(t, A(body), A(caption), A(col), A(colgroup), A(html), A(td), A(th), A(tr)))
        return;
    process_in(p, M_IN_TABLE, t);
}

static void mode_in_row(P *p, const tok *t)
{
    if (t->type == HK_TOK_START_TAG && TOK_ONE_OF(t, A(th), A(td))) {
        clear_to_row_context(p);
        insert_html_element(p, t);
        p->mode = M_IN_CELL;
        afe_push_marker(p);
        return;
    }
    if (is_end(t, A(tr))) {
        if (!in_scope(p, A(tr), SCOPE_TABLE)) return;
        clear_to_row_context(p);
        pop(p);
        p->mode = M_IN_TABLE_BODY;
        return;
    }
    if ((t->type == HK_TOK_START_TAG &&
         TOK_ONE_OF(t, A(caption), A(col), A(colgroup), A(tbody), A(tfoot), A(thead), A(tr))) ||
        is_end(t, A(table))) {
        if (!in_scope(p, A(tr), SCOPE_TABLE)) return;
        clear_to_row_context(p);
        pop(p);
        reprocess_as(p, M_IN_TABLE_BODY, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG && TOK_ONE_OF(t, A(tbody), A(tfoot), A(thead))) {
        if (!in_scope(p, t->name, SCOPE_TABLE) || !in_scope(p, A(tr), SCOPE_TABLE)) return;
        clear_to_row_context(p);
        pop(p);
        reprocess_as(p, M_IN_TABLE_BODY, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG &&
        TOK_ONE_OF(t, A(body), A(caption), A(col), A(colgroup), A(html), A(td), A(th)))
        return;
    process_in(p, M_IN_TABLE, t);
}

static void close_cell(P *p)
{
    static const hk_atom k_cells[] = {A(td), A(th)};
    generate_implied_end_tags(p, 0);
    pop_until_one_of(p, k_cells, 2);
    afe_clear_to_marker(p);
    p->mode = M_IN_ROW;
}

static void mode_in_cell(P *p, const tok *t)
{
    static const hk_atom k_cells[] = {A(td), A(th)};
    if (t->type == HK_TOK_END_TAG && TOK_ONE_OF(t, A(td), A(th))) {
        if (!in_scope(p, t->name, SCOPE_TABLE)) return;
        generate_implied_end_tags(p, 0);
        pop_until(p, t->name);
        afe_clear_to_marker(p);
        p->mode = M_IN_ROW;
        return;
    }
    if (t->type == HK_TOK_START_TAG &&
        TOK_ONE_OF(t, A(caption), A(col), A(colgroup), A(tbody), A(td), A(tfoot), A(th),
                   A(thead), A(tr))) {
        if (!in_scope_names(p, k_cells, 2, SCOPE_TABLE)) return;
        close_cell(p);
        process(p, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG &&
        TOK_ONE_OF(t, A(body), A(caption), A(col), A(colgroup), A(html)))
        return;
    if (t->type == HK_TOK_END_TAG && TOK_ONE_OF(t, A(table), A(tbody), A(tfoot), A(thead), A(tr))) {
        if (!in_scope(p, t->name, SCOPE_TABLE)) return;
        close_cell(p);
        process(p, t);
        return;
    }
    process_in(p, M_IN_BODY, t);
}

static void mode_in_template(P *p, const tok *t)
{
    if (t->type == HK_TOK_CHARS || t->type == HK_TOK_COMMENT || t->type == HK_TOK_DOCTYPE) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if ((t->type == HK_TOK_START_TAG &&
         TOK_ONE_OF(t, A(base), A(basefont), A(bgsound), A(link), A(meta), A(noframes),
                    A(script), A(style), A(template), A(title))) ||
        is_end(t, A(template))) {
        process_in(p, M_IN_HEAD, t);
        return;
    }
    if (t->type == HK_TOK_START_TAG) {
        mode next = M_IN_BODY;
        if (TOK_ONE_OF(t, A(caption), A(colgroup), A(tbody), A(tfoot), A(thead))) next = M_IN_TABLE;
        else if (tok_is(t, A(col))) next = M_IN_COLUMN_GROUP;
        else if (tok_is(t, A(tr))) next = M_IN_TABLE_BODY;
        else if (TOK_ONE_OF(t, A(td), A(th))) next = M_IN_ROW;
        pop_template_mode(p);
        push_template_mode(p, next);
        reprocess_as(p, next, t);
        return;
    }
    if (t->type == HK_TOK_END_TAG) return;
    /* EOF */
    if (!stack_has(p, A(template))) {
        stop_parsing(p);
        return;
    }
    pop_until(p, A(template));
    afe_clear_to_marker(p);
    pop_template_mode(p);
    reset_insertion_mode(p);
    process(p, t);
}

static void mode_after_body(P *p, const tok *t)
{
    if (is_ws_char(t) || is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        append_comment_to(p, t, p->open.data[0]);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE) return;
    if (is_end(t, A(html))) {
        p->mode = M_AFTER_AFTER_BODY;
        return;
    }
    if (t->type == HK_TOK_EOF) {
        stop_parsing(p);
        return;
    }
    reprocess_as(p, M_IN_BODY, t);
}

static void mode_in_frameset(P *p, const tok *t)
{
    if (is_ws_char(t)) {
        insert_chars(p, t->data.p, t->data.n);
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (is_start(t, A(frameset))) {
        insert_html_element(p, t);
        return;
    }
    if (is_end(t, A(frameset))) {
        if (p->open.len == 1) return; /* current node is the root html element */
        pop(p);
        if (!cur_is(p, A(frameset))) p->mode = M_AFTER_FRAMESET;
        return;
    }
    if (is_start(t, A(frame))) {
        insert_html_element(p, t);
        pop(p);
        return;
    }
    if (is_start(t, A(noframes))) {
        process_in(p, M_IN_HEAD, t);
        return;
    }
    if (t->type == HK_TOK_EOF) stop_parsing(p);
}

static void mode_after_frameset(P *p, const tok *t)
{
    if (is_ws_char(t)) {
        insert_chars(p, t->data.p, t->data.n);
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (is_end(t, A(html))) {
        p->mode = M_AFTER_AFTER_FRAMESET;
        return;
    }
    if (is_start(t, A(noframes))) {
        process_in(p, M_IN_HEAD, t);
        return;
    }
    if (t->type == HK_TOK_EOF) stop_parsing(p);
}

static void mode_after_after_body(P *p, const tok *t)
{
    if (t->type == HK_TOK_COMMENT) {
        append_comment_to(p, t, p->doc->root);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE || is_ws_char(t) || is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (t->type == HK_TOK_EOF) {
        stop_parsing(p);
        return;
    }
    reprocess_as(p, M_IN_BODY, t);
}

static void mode_after_after_frameset(P *p, const tok *t)
{
    if (t->type == HK_TOK_COMMENT) {
        append_comment_to(p, t, p->doc->root);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE || is_ws_char(t) || is_start(t, A(html))) {
        process_in(p, M_IN_BODY, t);
        return;
    }
    if (t->type == HK_TOK_EOF) {
        stop_parsing(p);
        return;
    }
    if (is_start(t, A(noframes))) process_in(p, M_IN_HEAD, t);
}

/* --- foreign content (13.2.6.5) ----------------------------------------------------------------- */

static bool breaks_out_of_foreign(const tok *t)
{
    if (t->type == HK_TOK_END_TAG) return tok_is(t, A(br)) || tok_is(t, A(p));
    if (t->type != HK_TOK_START_TAG) return false;
    if (tok_is(t, A(font)))
        return tok_attr(t, "color") || tok_attr(t, "face") || tok_attr(t, "size");
    return TOK_ONE_OF(t, A(b), A(big), A(blockquote), A(body), A(br), A(center), A(code), A(dd),
                      A(div), A(dl), A(dt), A(em), A(embed), A(h1), A(h2), A(h3), A(h4), A(h5),
                      A(h6), A(head), A(hr), A(i), A(img), A(li), A(listing), A(menu), A(meta),
                      A(nobr), A(ol), A(p), A(pre), A(ruby), A(s), A(small), A(span), A(strong),
                      A(strike), A(sub), A(sup), A(table), A(tt), A(u), A(ul), A(var));
}

static void foreign_content(P *p, const tok *t)
{
    if (t->type == HK_TOK_CHARS) {
        if (t->data.n == 1 && t->data.p[0] == '\0') {
            insert_chars(p, "\xEF\xBF\xBD", 3);
            return;
        }
        insert_chars(p, t->data.p, t->data.n);
        if (!is_ws_char(t)) p->frameset_ok = false;
        return;
    }
    if (t->type == HK_TOK_COMMENT) {
        insert_comment(p, t);
        return;
    }
    if (t->type == HK_TOK_DOCTYPE) return;
    if (breaks_out_of_foreign(t)) {
        while (p->open.len) {
            hk_node_id c = current(p);
            if (el_ns(p, c) == HK_NS_HTML || is_mathml_text_integration_point(p, c) ||
                is_html_integration_point(p, c))
                break;
            pop(p);
        }
        process(p, t);
        return;
    }
    if (t->type == HK_TOK_START_TAG) {
        insert_element_ns(p, t, el_ns(p, current(p)));
        if (t->self_closing) pop(p);
        return;
    }
    if (t->type == HK_TOK_END_TAG) {
        hk_node_id cur = current(p);
        if (tok_is(t, A(script)) && el_ns(p, cur) == HK_NS_SVG && el_name(p, cur) == A(script)) {
            pop(p);
            return;
        }
        for (int i = (int)p->open.len - 1; i > 0; i--) {
            hk_node_id node = p->open.data[i];
            if (hk_str_eq_ascii_icase(hk_atom_str(&p->doc->atoms, el_name(p, node)), t->name_s)) {
                while (p->open.len > (uint32_t)i) pop(p);
                return;
            }
            if (el_ns(p, p->open.data[i - 1]) == HK_NS_HTML) {
                process_in(p, p->mode, t);
                return;
            }
        }
    }
}

/* --- dispatch ------------------------------------------------------------------------------------ */

static void process_in(P *p, mode m, const tok *t)
{
    switch (m) {
    case M_INITIAL: mode_initial(p, t); break;
    case M_BEFORE_HTML: mode_before_html(p, t); break;
    case M_BEFORE_HEAD: mode_before_head(p, t); break;
    case M_IN_HEAD: mode_in_head(p, t); break;
    case M_IN_HEAD_NOSCRIPT: mode_in_head_noscript(p, t); break;
    case M_AFTER_HEAD: mode_after_head(p, t); break;
    case M_IN_BODY: mode_in_body(p, t); break;
    case M_TEXT: mode_text(p, t); break;
    case M_IN_TABLE: mode_in_table(p, t); break;
    case M_IN_TABLE_TEXT: mode_in_table_text(p, t); break;
    case M_IN_CAPTION: mode_in_caption(p, t); break;
    case M_IN_COLUMN_GROUP: mode_in_column_group(p, t); break;
    case M_IN_TABLE_BODY: mode_in_table_body(p, t); break;
    case M_IN_ROW: mode_in_row(p, t); break;
    case M_IN_CELL: mode_in_cell(p, t); break;
    case M_IN_TEMPLATE: mode_in_template(p, t); break;
    case M_AFTER_BODY: mode_after_body(p, t); break;
    case M_IN_FRAMESET: mode_in_frameset(p, t); break;
    case M_AFTER_FRAMESET: mode_after_frameset(p, t); break;
    case M_AFTER_AFTER_BODY: mode_after_after_body(p, t); break;
    case M_AFTER_AFTER_FRAMESET: mode_after_after_frameset(p, t); break;
    }
}

/* Tree construction dispatcher: insertion mode or foreign content. */
static void process(P *p, const tok *t)
{
    if (p->stopped) return;
    hk_node_id acn = current(p);
    bool use_mode = !acn || el_ns(p, acn) == HK_NS_HTML || t->type == HK_TOK_EOF;
    if (!use_mode && is_mathml_text_integration_point(p, acn)) {
        use_mode = (t->type == HK_TOK_START_TAG && !tok_is(t, A(mglyph)) &&
                    !tok_is(t, A(malignmark))) ||
                   t->type == HK_TOK_CHARS;
    }
    if (!use_mode && el_ns(p, acn) == HK_NS_MATHML && el_name(p, acn) == A(annotation_xml) &&
        is_start(t, A(svg)))
        use_mode = true;
    if (!use_mode && is_html_integration_point(p, acn))
        use_mode = t->type == HK_TOK_START_TAG || t->type == HK_TOK_CHARS;
    if (use_mode) process_in(p, p->mode, t);
    else foreign_content(p, t);
}

/* Character runs: whole runs in the common modes, one character at a time otherwise. */
static void process_chars(P *p, hk_str s)
{
    size_t i = 0;
    while (i < s.n && !p->stopped) {
        hk_node_id acn = current(p);
        bool html = !acn || el_ns(p, acn) == HK_NS_HTML;
        if (html && (p->mode == M_TEXT || p->mode == M_IN_BODY)) {
            size_t j = i;
            while (j < s.n && s.p[j] != '\0') j++;
            if (j > i) {
                if (p->mode == M_TEXT) insert_chars(p, s.p + i, j - i);
                else body_chars(p, s.p + i, j - i);
                i = j;
                continue;
            }
        }
        /* One code point. */
        size_t len = 1;
        unsigned char c = (unsigned char)s.p[i];
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (len > s.n - i) len = s.n - i;
        tok t = {0};
        t.type = HK_TOK_CHARS;
        t.data = (hk_str){s.p + i, len};
        process(p, &t);
        i += len;
    }
}

static void on_token(void *user, hk_html_token *raw)
{
    P *p = user;
    if (p->stopped) return;
    bool ignore_lf = p->ignore_lf;
    p->ignore_lf = false;

    if (raw->type == HK_TOK_CHARS) {
        hk_str s = raw->data;
        if (ignore_lf && s.n && s.p[0] == '\n') s = hk_str_slice(s, 1, s.n);
        process_chars(p, s);
    } else {
        tok t = {0};
        t.type = raw->type;
        t.raw = raw;
        t.data = raw->data;
        if (raw->type == HK_TOK_START_TAG || raw->type == HK_TOK_END_TAG) {
            t.name_s = raw->name;
            t.name = raw->type == HK_TOK_START_TAG ? hk_atom_intern(&p->doc->atoms, raw->name)
                                                   : hk_atom_find(&p->doc->atoms, raw->name);
            t.attrs = raw->attrs;
            t.attr_count = raw->attr_count;
            t.self_closing = raw->self_closing;
        }
        process(p, &t);
    }

    /* CDATA sections are recognized only when the adjusted current node is foreign. */
    hk_node_id acn = current(p);
    p->tokenizer.cdata_allowed = acn && el_ns(p, acn) != HK_NS_HTML;
}

/* --- public API ---------------------------------------------------------------------------------- */

hk_html_parser *hk_html_parser_create(hk_document *doc)
{
    P *p = hk_alloc_zero(sizeof *p);
    p->doc = doc;
    p->mode = M_INITIAL;
    p->frameset_ok = true;
    hk_html_tokenizer_init(&p->tokenizer, on_token, p);
    return p;
}

void hk_html_parser_destroy(hk_html_parser *p)
{
    if (!p) return;
    hk_html_tokenizer_free(&p->tokenizer);
    hk_vec_free(&p->open);
    hk_vec_free(&p->afe);
    hk_vec_free(&p->template_modes);
    hk_vec_free(&p->table_text);
    hk_free(p, sizeof *p);
}

void hk_html_parser_feed(hk_html_parser *p, const char *utf8, size_t n)
{
    hk_html_tokenizer_feed(&p->tokenizer, utf8, n);
}

void hk_html_parser_finish(hk_html_parser *p)
{
    hk_html_tokenizer_finish(&p->tokenizer);
}

hk_document *hk_html_parse(const char *utf8, size_t n)
{
    hk_document *doc = hk_document_create();
    hk_html_parser *p = hk_html_parser_create(doc);
    hk_html_parser_feed(p, utf8, n);
    hk_html_parser_finish(p);
    hk_html_parser_destroy(p);
    return doc;
}
