/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared between the Win32 platform files only. */
#ifndef HK_WIN32_INTERNAL_H
#define HK_WIN32_INTERNAL_H

#include <windows.h>

/* Thread message that tells the main loop to run posted tasks. */
#define HK_WM_TASKS (WM_APP + 1)

void hk_win32_main_queue_init(void);
void hk_win32_run_main_tasks(void);

#endif
