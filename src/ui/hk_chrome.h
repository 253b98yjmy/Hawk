/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The browser chrome above the page: the tab strip and the toolbar (back, forward, reload and the
 * address bar's frame; the address bar itself is a native edit control placed by the window).
 * It is drawn as a display list, so the page painter draws it too.
 */
#ifndef HK_CHROME_H
#define HK_CHROME_H

#include "layout/hk_layout.h"

#define HK_CHROME_TAB_ROW 34.0f
#define HK_CHROME_HEIGHT 72.0f /* CSS px: the tab row and the toolbar */

typedef struct hk_chrome_tab {
    hk_str title; /* UTF-8; empty: "新しいタブ" */
    bool loading;
} hk_chrome_tab;

typedef enum hk_chrome_part {
    HK_CHROME_NONE = 0,
    HK_CHROME_TAB,       /* index = the tab */
    HK_CHROME_TAB_CLOSE, /* index = the tab */
    HK_CHROME_NEW_TAB,
    HK_CHROME_BACK,
    HK_CHROME_FORWARD,
    HK_CHROME_RELOAD, /* stop while loading */
    HK_CHROME_ADDRESS,
    HK_CHROME_STRIP, /* empty space in the tab row */
} hk_chrome_part;

typedef struct hk_chrome_hit {
    hk_chrome_part part;
    uint32_t index;
} hk_chrome_hit;

typedef struct hk_chrome_state {
    const hk_chrome_tab *tabs;
    uint32_t ntabs, active;
    bool can_back, can_forward, loading;
    bool dark;
    hk_chrome_hit hover;
} hk_chrome_state;

typedef struct hk_chrome_rect {
    float x, y, w, h;
} hk_chrome_rect;

/* Builds the display list for a chrome of the given width (CSS px). address receives the area the
 * native address bar is placed in. */
void hk_chrome_build(const hk_chrome_state *s, float width, hk_display_list *out, hk_chrome_rect *address);
hk_chrome_hit hk_chrome_hit_test(const hk_chrome_state *s, float width, float x, float y);
/* Colors of the address bar for the theme (0xAARRGGBB). */
void hk_chrome_address_colors(bool dark, uint32_t *background, uint32_t *text);

#endif
