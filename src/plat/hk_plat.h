/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Platform services. Implemented per OS under plat/<os>/. */
#ifndef HK_PLAT_H
#define HK_PLAT_H

#include "base/hk_arena.h"
#include "base/hk_common.h"

/* Application entry point, called by the platform entry after hk_plat_init. argv is UTF-8. */
int hk_app_main(int argc, char **argv);

/* Logging sink, crash behaviour, process mitigations. Called once before anything else. */
void hk_plat_init(void);

/* Monotonic clock. */
uint64_t hk_plat_now_ns(void);
/* Wall time elapsed since the OS created this process. */
double hk_plat_ms_since_process_start(void);

typedef struct hk_plat_mem_info {
    size_t private_bytes; /* commit charge: what docs/design/06-quality.md budgets */
    size_t working_set;
} hk_plat_mem_info;

void hk_plat_mem_info_get(hk_plat_mem_info *out);

#endif
