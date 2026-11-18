/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Hawk application entry (M0): opens an empty browser window.
 *
 * Options:
 *   --smoke   close right after the first frame is presented and log startup
 *             time and memory (used by the smoke test and benchmarks)
 */
#include "base/hk_log.h"
#include "plat/hk_plat.h"
#include "plat/hk_window.h"

#include <string.h>

typedef struct app_state {
    bool smoke;
} app_state;

#define COLOR_BG_LIGHT 0xFFFFFFFFu
#define COLOR_BG_DARK 0xFF1E1E1Eu

static void fill_surface(hk_surface *s, uint32_t color)
{
    for (int y = 0; y < s->height; y++) {
        uint32_t *row = s->pixels + (size_t)y * (size_t)s->stride;
        for (int x = 0; x < s->width; x++) row[x] = color;
    }
}

static void on_paint(void *user, hk_window *w, hk_surface *s)
{
    HK_UNUSED(user);
    fill_surface(s, hk_window_is_dark(w) ? COLOR_BG_DARK : COLOR_BG_LIGHT);
}

static void on_first_present(void *user, hk_window *w)
{
    app_state *app = user;
    hk_plat_mem_info mem;
    hk_plat_mem_info_get(&mem);
    HK_LOGI("first frame: %.1f ms after process start, private %.1f MiB, working set %.1f MiB",
            hk_plat_ms_since_process_start(), (double)mem.private_bytes / (1024.0 * 1024.0),
            (double)mem.working_set / (1024.0 * 1024.0));
    if (app->smoke) hk_window_close(w);
}

int hk_app_main(int argc, char **argv)
{
    app_state app = {0};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--smoke") == 0) {
            app.smoke = true;
        } else {
            HK_LOGW("unknown argument: %s", argv[i]);
        }
    }

    hk_window_desc desc = {0};
    desc.title = HK_STR("Hawk");
    desc.width = 1200;
    desc.height = 800;
    desc.cb.user = &app;
    desc.cb.on_paint = on_paint;
    desc.cb.on_first_present = on_first_present;

    hk_window *window;
    hk_status st = hk_window_create(&desc, &window);
    if (st != HK_OK) {
        HK_LOGE("could not create the main window: %s", hk_status_name(st));
        return 1;
    }
    return hk_plat_run_loop();
}
