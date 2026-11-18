/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Parsing of CSS component values: colors, lengths (with calc()), keywords. */
#include "style/hk_css_values.h"

#include <math.h>
#include <string.h>

/* --- spans ---------------------------------------------------------------------------------- */

static bool is_open(uint8_t t)
{
    return t == HK_CT_FUNCTION || t == HK_CT_LPAREN || t == HK_CT_LBRACKET || t == HK_CT_LBRACE;
}

uint32_t hk_cv_skip(const hk_css_token *t, uint32_t n, uint32_t i)
{
    if (i >= n) return n;
    if (!is_open(t[i].type)) return i + 1;
    int depth = 0;
    for (; i < n; i++) {
        if (is_open(t[i].type)) depth++;
        else if (t[i].type == HK_CT_RPAREN || t[i].type == HK_CT_RBRACKET || t[i].type == HK_CT_RBRACE) {
            if (--depth == 0) return i + 1;
        }
    }
    return n;
}

uint32_t hk_cv_split(hk_cv_span s, hk_cv_span *out, uint32_t max)
{
    uint32_t count = 0, i = 0;
    while (i < s.n && count < max) {
        if (s.t[i].type == HK_CT_WS) {
            i++;
            continue;
        }
        uint32_t end = hk_cv_skip(s.t, s.n, i);
        out[count].t = s.t + i;
        out[count].n = end - i;
        count++;
        i = end;
    }
    return count;
}

uint32_t hk_cv_split_commas(hk_cv_span s, hk_cv_span *out, uint32_t max)
{
    uint32_t count = 0, start = 0, i = 0;
    while (count < max) {
        if (i >= s.n || s.t[i].type == HK_CT_COMMA) {
            out[count].t = s.t + start;
            out[count].n = i - start;
            count++;
            if (i >= s.n) break;
            start = ++i;
            continue;
        }
        i = hk_cv_skip(s.t, s.n, i);
    }
    return count;
}

hk_cv_span hk_cv_trim(hk_cv_span s)
{
    while (s.n && s.t[0].type == HK_CT_WS) s.t++, s.n--;
    while (s.n && s.t[s.n - 1].type == HK_CT_WS) s.n--;
    return s;
}

bool hk_cv_single_ident(hk_cv_span s, hk_str *ident)
{
    s = hk_cv_trim(s);
    if (s.n != 1 || s.t[0].type != HK_CT_IDENT) return false;
    *ident = s.t[0].value;
    return true;
}

/* --- lengths --------------------------------------------------------------------------------- */

typedef struct calc_val {
    float px, pct, num;
    int kind; /* 0 number, 1 length/percentage */
    bool ok;
} calc_val;

static bool unit_px(const hk_css_token *t, const hk_cv_ctx *c, float *px)
{
    double v = t->num;
    hk_str u = t->unit;
#define U(s) hk_str_eq_ascii_icase(u, HK_STR(s))
    if (U("px")) *px = (float)v;
    else if (U("em")) *px = (float)(v * c->em);
    else if (U("rem")) *px = (float)(v * c->rem);
    else if (U("ex") || U("ch") || U("cap")) *px = (float)(v * c->em * 0.5);
    else if (U("rex") || U("rch")) *px = (float)(v * c->rem * 0.5);
    else if (U("lh")) *px = (float)(v * c->em * 1.2);
    else if (U("pt")) *px = (float)(v * 96.0 / 72.0);
    else if (U("pc")) *px = (float)(v * 16);
    else if (U("in")) *px = (float)(v * 96);
    else if (U("cm")) *px = (float)(v * 96 / 2.54);
    else if (U("mm")) *px = (float)(v * 96 / 25.4);
    else if (U("q")) *px = (float)(v * 96 / 101.6);
    else if (U("vw") || U("svw") || U("lvw") || U("dvw") || U("vi")) *px = (float)(v * c->vw / 100);
    else if (U("vh") || U("svh") || U("lvh") || U("dvh") || U("vb")) *px = (float)(v * c->vh / 100);
    else if (U("vmin")) *px = (float)(v * fminf(c->vw, c->vh) / 100);
    else if (U("vmax")) *px = (float)(v * fmaxf(c->vw, c->vh) / 100);
    else return false;
#undef U
    return true;
}

static calc_val calc_sum(const hk_css_token *t, uint32_t n, uint32_t *i, const hk_cv_ctx *c);

static void skip_ws(const hk_css_token *t, uint32_t n, uint32_t *i)
{
    while (*i < n && t[*i].type == HK_CT_WS) (*i)++;
}

static calc_val calc_fn(const hk_css_token *t, uint32_t n, uint32_t *i, const hk_cv_ctx *c);

static calc_val calc_factor(const hk_css_token *t, uint32_t n, uint32_t *i, const hk_cv_ctx *c)
{
    calc_val r = {0, 0, 0, 0, false};
    skip_ws(t, n, i);
    if (*i >= n) return r;
    const hk_css_token *k = &t[*i];
    if (k->type == HK_CT_NUMBER) {
        r.num = (float)k->num;
        r.ok = true;
        (*i)++;
    } else if (k->type == HK_CT_PERCENT) {
        r.pct = (float)k->num;
        r.kind = 1;
        r.ok = true;
        (*i)++;
    } else if (k->type == HK_CT_DIMENSION) {
        r.kind = 1;
        r.ok = unit_px(k, c, &r.px);
        (*i)++;
    } else if (k->type == HK_CT_LPAREN) {
        (*i)++;
        r = calc_sum(t, n, i, c);
        skip_ws(t, n, i);
        if (*i < n && t[*i].type == HK_CT_RPAREN) (*i)++;
        else r.ok = false;
    } else if (k->type == HK_CT_FUNCTION) {
        r = calc_fn(t, n, i, c);
    }
    return r;
}

static calc_val calc_product(const hk_css_token *t, uint32_t n, uint32_t *i, const hk_cv_ctx *c)
{
    calc_val a = calc_factor(t, n, i, c);
    for (;;) {
        uint32_t save = *i;
        skip_ws(t, n, i);
        if (*i < n && t[*i].type == HK_CT_DELIM && (t[*i].delim == '*' || t[*i].delim == '/')) {
            bool mul = t[*i].delim == '*';
            (*i)++;
            calc_val b = calc_factor(t, n, i, c);
            if (!a.ok || !b.ok) return (calc_val){0, 0, 0, 0, false};
            if (mul) {
                if (a.kind == 0 && b.kind == 0) a.num *= b.num;
                else if (a.kind == 0) b.px *= a.num, b.pct *= a.num, a = b;
                else if (b.kind == 0) a.px *= b.num, a.pct *= b.num;
                else a.ok = false;
            } else {
                if (b.kind != 0 || b.num == 0) a.ok = false;
                else if (a.kind == 0) a.num /= b.num;
                else a.px /= b.num, a.pct /= b.num;
            }
            continue;
        }
        *i = save;
        return a;
    }
}

static calc_val calc_sum(const hk_css_token *t, uint32_t n, uint32_t *i, const hk_cv_ctx *c)
{
    calc_val a = calc_product(t, n, i, c);
    for (;;) {
        uint32_t save = *i;
        skip_ws(t, n, i);
        if (*i < n && t[*i].type == HK_CT_DELIM && (t[*i].delim == '+' || t[*i].delim == '-')) {
            float sign = t[*i].delim == '+' ? 1.0f : -1.0f;
            (*i)++;
            calc_val b = calc_product(t, n, i, c);
            if (!a.ok || !b.ok) return (calc_val){0, 0, 0, 0, false};
            if (a.kind != b.kind && !(a.kind == 0 && a.num == 0) && !(b.kind == 0 && b.num == 0)) {
                a.ok = false;
                return a;
            }
            if (a.kind == 0 && b.kind == 1) a.kind = 1;
            a.num += sign * b.num;
            a.px += sign * b.px;
            a.pct += sign * b.pct;
            continue;
        }
        *i = save;
        return a;
    }
}

/* calc(), min(), max(), clamp(); *i points at the function token and ends past its ')'. */
static calc_val calc_fn(const hk_css_token *t, uint32_t n, uint32_t *i, const hk_cv_ctx *c)
{
    calc_val bad = {0, 0, 0, 0, false};
    hk_str name = t[*i].value;
    uint32_t end = hk_cv_skip(t, n, *i); /* past ')' */
    uint32_t inner_n = end - 1;          /* index of ')' */
    uint32_t j = *i + 1;
    *i = end;
    bool is_calc = hk_str_eq_ascii_icase(name, HK_STR("calc")) || hk_str_eq_ascii_icase(name, HK_STR("-webkit-calc"));
    bool is_min = hk_str_eq_ascii_icase(name, HK_STR("min"));
    bool is_max = hk_str_eq_ascii_icase(name, HK_STR("max"));
    bool is_clamp = hk_str_eq_ascii_icase(name, HK_STR("clamp"));
    if (is_calc) {
        calc_val r = calc_sum(t, inner_n, &j, c);
        skip_ws(t, inner_n, &j);
        return j == inner_n ? r : bad;
    }
    if (!is_min && !is_max && !is_clamp) return bad;
    calc_val args[8];
    int na = 0;
    while (j < inner_n && na < 8) {
        args[na] = calc_sum(t, inner_n, &j, c);
        if (!args[na].ok) return bad;
        na++;
        skip_ws(t, inner_n, &j);
        if (j < inner_n && t[j].type == HK_CT_COMMA) j++;
        else break;
    }
    if (na == 0 || (is_clamp && na != 3)) return bad;
    /* Percentages cannot be compared before layout: mixed arguments keep the absolute one
       (min(100%, 800px) behaves like a capped width on wide screens). */
    calc_val r = args[0];
    for (int k = 1; k < na && !is_clamp; k++) {
        calc_val b = args[k];
        if (r.pct != 0 || b.pct != 0) {
            if (r.pct != 0 && b.pct == 0) r = b;
            continue;
        }
        float av = r.kind ? r.px : r.num, bv = b.kind ? b.px : b.num;
        if ((is_min && bv < av) || (is_max && bv > av)) r = b;
    }
    if (is_clamp) {
        calc_val lo = args[0], val = args[1], hi = args[2];
        if (lo.pct || val.pct || hi.pct) return val;
        float v = val.kind ? val.px : val.num;
        float l = lo.kind ? lo.px : lo.num, h = hi.kind ? hi.px : hi.num;
        r = val;
        float x = fmaxf(l, fminf(v, h));
        if (r.kind) r.px = x;
        else r.num = x;
    }
    return r;
}

bool hk_cv_length(hk_cv_span s, const hk_cv_ctx *c, uint32_t flags, hk_length *out)
{
    s = hk_cv_trim(s);
    if (s.n == 0) return false;
    const hk_css_token *t = &s.t[0];
    if (s.n == 1) {
        if (t->type == HK_CT_IDENT) {
            if ((flags & HK_CV_AUTO) && (hk_css_ident_is(t, "auto"))) {
                *out = (hk_length){0, 0, HK_LEN_AUTO};
                return true;
            }
            if ((flags & HK_CV_NONE) && hk_css_ident_is(t, "none")) {
                *out = (hk_length){0, 0, HK_LEN_NONE};
                return true;
            }
            if ((flags & HK_CV_AUTO) && (hk_css_ident_is(t, "fit-content") || hk_css_ident_is(t, "max-content") ||
                                         hk_css_ident_is(t, "min-content") || hk_css_ident_is(t, "-webkit-fill-available") ||
                                         hk_css_ident_is(t, "stretch"))) {
                *out = (hk_length){0, 0, HK_LEN_AUTO};
                return true;
            }
            return false;
        }
        if (t->type == HK_CT_NUMBER) {
            if (t->num != 0 && !(flags & HK_CV_UNITLESS)) return false;
            *out = (hk_length){(float)t->num, 0, HK_LEN_PX};
            return (flags & HK_CV_NEGATIVE) || t->num >= 0;
        }
        if (t->type == HK_CT_PERCENT) {
            if (!(flags & HK_CV_PERCENT)) return false;
            *out = (hk_length){0, (float)t->num, HK_LEN_PCT};
            return (flags & HK_CV_NEGATIVE) || t->num >= 0;
        }
        if (t->type == HK_CT_DIMENSION) {
            float px;
            if (!unit_px(t, c, &px)) return false;
            *out = (hk_length){px, 0, HK_LEN_PX};
            return (flags & HK_CV_NEGATIVE) || px >= 0;
        }
    }
    if (t->type == HK_CT_FUNCTION) {
        uint32_t i = 0;
        calc_val v = calc_fn(s.t, s.n, &i, c);
        if (!v.ok || i != s.n) return false;
        if (v.kind == 0) {
            if (v.num != 0 && !(flags & HK_CV_UNITLESS)) return false;
            v.px = v.num;
        }
        if (v.pct != 0 && !(flags & HK_CV_PERCENT)) return false;
        if (v.pct == 0) *out = (hk_length){v.px, 0, HK_LEN_PX};
        else if (v.px == 0) *out = (hk_length){0, v.pct, HK_LEN_PCT};
        else *out = (hk_length){v.px, v.pct, HK_LEN_CALC};
        if (!(flags & HK_CV_NEGATIVE) && out->px < 0 && out->pct <= 0) out->px = 0;
        return true;
    }
    return false;
}

bool hk_cv_number(hk_cv_span s, const hk_cv_ctx *c, float *out)
{
    s = hk_cv_trim(s);
    if (s.n == 1 && s.t[0].type == HK_CT_NUMBER) {
        *out = (float)s.t[0].num;
        return true;
    }
    if (s.n && s.t[0].type == HK_CT_FUNCTION) {
        uint32_t i = 0;
        calc_val v = calc_fn(s.t, s.n, &i, c);
        if (v.ok && v.kind == 0 && i == s.n) {
            *out = v.num;
            return true;
        }
    }
    return false;
}

/* --- colors ------------------------------------------------------------------------------------ */

typedef struct named_color {
    const char *name;
    uint32_t rgb;
} named_color;

static const named_color k_colors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF}, {"aquamarine", 0x7FFFD4},
    {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC}, {"bisque", 0xFFE4C4}, {"black", 0x000000},
    {"blanchedalmond", 0xFFEBCD}, {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00}, {"chocolate", 0xD2691E},
    {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED}, {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C},
    {"cyan", 0x00FFFF}, {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9}, {"darkkhaki", 0xBDB76B},
    {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F}, {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC},
    {"darkred", 0x8B0000}, {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1}, {"darkviolet", 0x9400D3},
    {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"dodgerblue", 0x1E90FF}, {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF}, {"gold", 0xFFD700},
    {"goldenrod", 0xDAA520}, {"gray", 0x808080}, {"green", 0x008000}, {"greenyellow", 0xADFF2F},
    {"grey", 0x808080}, {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C}, {"lavender", 0xE6E6FA},
    {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00}, {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6},
    {"lightcoral", 0xF08080}, {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1}, {"lightsalmon", 0xFFA07A},
    {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xB0C4DE}, {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66CDAA},
    {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3}, {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371},
    {"mediumslateblue", 0x7B68EE}, {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA}, {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD}, {"navy", 0x000080}, {"oldlace", 0xFDF5E6},
    {"olive", 0x808000}, {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500},
    {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98}, {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5}, {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F},
    {"pink", 0xFFC0CB}, {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F}, {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072}, {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57},
    {"seashell", 0xFFF5EE}, {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB},
    {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4}, {"tan", 0xD2B48C}, {"teal", 0x008080},
    {"thistle", 0xD8BFD8}, {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE},
    {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5}, {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32},
    /* system colors (light scheme) */
    {"canvas", 0xFFFFFF}, {"canvastext", 0x000000}, {"linktext", 0x0000EE}, {"visitedtext", 0x551A8B},
    {"activetext", 0xFF0000}, {"buttonface", 0xEFEFEF}, {"buttontext", 0x000000}, {"buttonborder", 0x767676},
    {"field", 0xFFFFFF}, {"fieldtext", 0x000000}, {"highlight", 0x3390FF}, {"highlighttext", 0xFFFFFF},
    {"graytext", 0x6D6D6D}, {"mark", 0xFFFF00}, {"marktext", 0x000000}, {"window", 0xFFFFFF},
    {"windowtext", 0x000000},
};

static int hex1(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static uint8_t clamp255(double v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
}

static float hue_to_rgb(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

bool hk_cv_color(hk_cv_span s, uint32_t current, uint32_t *out)
{
    s = hk_cv_trim(s);
    if (s.n == 0) return false;
    const hk_css_token *t = &s.t[0];
    if (t->type == HK_CT_HASH && s.n == 1) {
        hk_str h = t->value;
        int v[8];
        for (size_t i = 0; i < h.n && i < 8; i++) {
            v[i] = hex1((unsigned char)h.p[i]);
            if (v[i] < 0) return false;
        }
        uint32_t r, g, b, a = 255;
        if (h.n == 3 || h.n == 4) {
            r = (uint32_t)v[0] * 17;
            g = (uint32_t)v[1] * 17;
            b = (uint32_t)v[2] * 17;
            if (h.n == 4) a = (uint32_t)v[3] * 17;
        } else if (h.n == 6 || h.n == 8) {
            r = (uint32_t)(v[0] * 16 + v[1]);
            g = (uint32_t)(v[2] * 16 + v[3]);
            b = (uint32_t)(v[4] * 16 + v[5]);
            if (h.n == 8) a = (uint32_t)(v[6] * 16 + v[7]);
        } else {
            return false;
        }
        *out = a << 24 | r << 16 | g << 8 | b;
        return true;
    }
    if (t->type == HK_CT_IDENT && s.n == 1) {
        if (hk_css_ident_is(t, "transparent")) {
            *out = 0;
            return true;
        }
        if (hk_css_ident_is(t, "currentcolor")) {
            *out = current;
            return true;
        }
        for (size_t i = 0; i < sizeof k_colors / sizeof k_colors[0]; i++) {
            if (hk_str_eq_ascii_icase(t->value, hk_str_from_cstr(k_colors[i].name))) {
                *out = 0xFF000000u | k_colors[i].rgb;
                return true;
            }
        }
        return false;
    }
    if (t->type == HK_CT_FUNCTION && hk_str_eq_ascii_icase(t->value, HK_STR("light-dark"))) {
        /* Pages render in the light scheme: the first color applies. */
        uint32_t end = hk_cv_skip(s.t, s.n, 0);
        hk_cv_span args[2];
        hk_cv_span inner = {s.t + 1, end - 2};
        if (hk_cv_split_commas(inner, args, 2) != 2) return false;
        return hk_cv_color(args[0], current, out);
    }
    if (t->type == HK_CT_FUNCTION) {
        hk_str fn = t->value;
        bool rgb = hk_str_eq_ascii_icase(fn, HK_STR("rgb")) || hk_str_eq_ascii_icase(fn, HK_STR("rgba"));
        bool hsl = hk_str_eq_ascii_icase(fn, HK_STR("hsl")) || hk_str_eq_ascii_icase(fn, HK_STR("hsla"));
        if (!rgb && !hsl) return false;
        /* Arguments: numbers/percentages separated by commas, spaces, or "/" before alpha. */
        double vals[4] = {0, 0, 0, 1};
        bool pct[4] = {false, false, false, false};
        int k = 0;
        uint32_t end = hk_cv_skip(s.t, s.n, 0);
        for (uint32_t i = 1; i + 1 < end && k < 4; i++) {
            const hk_css_token *a = &s.t[i];
            if (a->type == HK_CT_WS || a->type == HK_CT_COMMA || (a->type == HK_CT_DELIM && a->delim == '/')) continue;
            if (a->type == HK_CT_NUMBER) vals[k++] = a->num;
            else if (a->type == HK_CT_PERCENT) pct[k] = true, vals[k++] = a->num;
            else if (a->type == HK_CT_DIMENSION && k == 0 && hsl) vals[k++] = a->num; /* deg */
            else if (hk_css_ident_is(a, "none")) vals[k++] = 0;
            else return false;
        }
        if (k < 3) return false;
        double alpha = pct[3] ? vals[3] / 100 : vals[3];
        uint32_t a8 = clamp255(alpha * 255);
        if (rgb) {
            uint32_t r = clamp255(pct[0] ? vals[0] * 2.55 : vals[0]);
            uint32_t g = clamp255(pct[1] ? vals[1] * 2.55 : vals[1]);
            uint32_t b = clamp255(pct[2] ? vals[2] * 2.55 : vals[2]);
            *out = a8 << 24 | r << 16 | g << 8 | b;
        } else {
            float h = (float)fmod(fmod(vals[0], 360) + 360, 360) / 360.0f;
            float sat = (float)(vals[1] / 100), l = (float)(vals[2] / 100);
            float q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat, p = 2 * l - q;
            uint32_t r = clamp255(hue_to_rgb(p, q, h + 1.0f / 3) * 255);
            uint32_t g = clamp255(hue_to_rgb(p, q, h) * 255);
            uint32_t b = clamp255(hue_to_rgb(p, q, h - 1.0f / 3) * 255);
            *out = a8 << 24 | r << 16 | g << 8 | b;
        }
        return end == s.n;
    }
    return false;
}
