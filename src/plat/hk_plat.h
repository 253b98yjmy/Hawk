/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Platform services. Implemented per OS under plat/<os>/. */
#ifndef HK_PLAT_H
#define HK_PLAT_H

#include "base/hk_arena.h"
#include "base/hk_common.h"
#include "base/hk_str.h"
#include "base/hk_vec.h"

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

/*
 * IDNA ToASCII (UTS #46, non-transitional) of a UTF-8 domain name. The result is
 * lowercase and allocated in the arena. Returns false when the name is invalid.
 */
bool hk_plat_idna_to_ascii(hk_arena *a, hk_str domain, hk_str *out);

/* Legacy encodings: Windows code page decoding to UTF-8 (appended to out). */
void hk_plat_decode_codepage(uint32_t codepage, const uint8_t *p, size_t n, hk_charbuf *out);
/* True when b starts a two-byte sequence in a DBCS code page. */
bool hk_plat_is_lead_byte(uint32_t codepage, uint8_t b);

/* Files (paths are UTF-8). */
hk_status hk_plat_read_file(const char *path, hk_charbuf *out);
/* Hawk's per-user data directory (%LOCALAPPDATA%\Hawk), created if missing. */
bool hk_plat_data_dir(hk_charbuf *out);
/* Calls fn for each regular file in dir whose name ends with suffix (full UTF-8 paths). */
void hk_plat_list_files(const char *dir, const char *suffix, void (*fn)(void *user, const char *path),
                        void *user);

/* Primary language of the user interface is Japanese (decides the legacy encoding fallback). */
bool hk_plat_locale_is_japanese(void);

#endif
