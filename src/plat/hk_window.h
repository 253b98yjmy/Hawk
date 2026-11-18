/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Top-level windows and their software back buffer. */
#ifndef HK_WINDOW_H
#define HK_WINDOW_H

#include "base/hk_common.h"
#include "base/hk_str.h"

/* 32-bit premultiplied BGRA pixels (0xAARRGGBB as uint32_t), top-down rows. */
typedef struct hk_surface {
    uint32_t *pixels;
    int width;
    int height;
    int stride;   /* in pixels */
    void *native; /* platform drawing context for the surface (HDC on Windows) */
} hk_surface;

typedef enum hk_key {
    HK_KEY_OTHER = 0,
    HK_KEY_UP,
    HK_KEY_DOWN,
    HK_KEY_PAGE_UP,
    HK_KEY_PAGE_DOWN,
    HK_KEY_HOME,
    HK_KEY_END,
    HK_KEY_SPACE,
    HK_KEY_F5,
    HK_KEY_ESCAPE,
} hk_key;

#define HK_MOD_SHIFT 1u
#define HK_MOD_CTRL 2u

typedef struct hk_window hk_window;

typedef struct hk_window_callbacks {
    void *user;
    /* Repaint the whole surface. Called from the window's paint handler when content is stale. */
    void (*on_paint)(void *user, hk_window *w, hk_surface *s);
    /* Client area size in physical pixels. */
    void (*on_resize)(void *user, hk_window *w, int width, int height);
    void (*on_theme_changed)(void *user, hk_window *w, bool dark);
    /* After the first frame reached the screen. */
    void (*on_first_present)(void *user, hk_window *w);
    void (*on_key)(void *user, hk_window *w, hk_key key, unsigned mods);
    /* Wheel movement in notches (positive = away from the user, i.e. scroll up). */
    void (*on_wheel)(void *user, hk_window *w, float notches);
    /* The window is being destroyed; w is invalid after this returns. */
    void (*on_close)(void *user, hk_window *w);
} hk_window_callbacks;

typedef struct hk_window_desc {
    hk_str title;
    int width; /* client size in device-independent pixels (1/96 inch) */
    int height;
    hk_window_callbacks cb;
} hk_window_desc;

hk_status hk_window_create(const hk_window_desc *desc, hk_window **out);
void hk_window_set_title(hk_window *w, hk_str title);
/* Client size in physical pixels. */
void hk_window_client_size(const hk_window *w, int *width, int *height);
/* Repaints now and writes the client area to a 32-bit BMP file (tests and debugging). */
hk_status hk_window_save_bmp(hk_window *w, const char *path);
/* Marks the content stale and schedules a repaint. */
void hk_window_invalidate(hk_window *w);
/* Asks the window to close asynchronously (on_close follows). */
void hk_window_close(hk_window *w);
/* Physical pixels per CSS/device-independent pixel. */
float hk_window_scale(const hk_window *w);
bool hk_window_is_dark(const hk_window *w);

/* Runs the event loop until the last window is closed. Returns the process exit code. */
int hk_plat_run_loop(void);

#endif
