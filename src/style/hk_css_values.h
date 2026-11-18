/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Component value helpers shared by the cascade. */
#ifndef HK_CSS_VALUES_H
#define HK_CSS_VALUES_H

#include "css/hk_css.h"
#include "style/hk_style.h"

typedef struct hk_cv_span {
    const hk_css_token *t;
    uint32_t n;
} hk_cv_span;

/* Context for resolving relative lengths. */
typedef struct hk_cv_ctx {
    float em;  /* the element's font size (the parent's when computing font-size) */
    float rem; /* the root element's font size */
    float vw, vh;
} hk_cv_ctx;

#define HK_CV_PERCENT 1u
#define HK_CV_AUTO 2u
#define HK_CV_NONE 4u
#define HK_CV_NEGATIVE 8u
#define HK_CV_UNITLESS 16u

uint32_t hk_cv_skip(const hk_css_token *t, uint32_t n, uint32_t i);
/* Splits into whitespace-separated component values. Returns the count (at most max). */
uint32_t hk_cv_split(hk_cv_span s, hk_cv_span *out, uint32_t max);
uint32_t hk_cv_split_commas(hk_cv_span s, hk_cv_span *out, uint32_t max);
hk_cv_span hk_cv_trim(hk_cv_span s);
bool hk_cv_single_ident(hk_cv_span s, hk_str *ident);

bool hk_cv_length(hk_cv_span s, const hk_cv_ctx *c, uint32_t flags, hk_length *out);
bool hk_cv_number(hk_cv_span s, const hk_cv_ctx *c, float *out);
bool hk_cv_color(hk_cv_span s, uint32_t current, uint32_t *out);

#endif
