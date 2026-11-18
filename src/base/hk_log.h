/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal logging. The platform layer installs a sink; the default writes to stderr. */
#ifndef HK_LOG_H
#define HK_LOG_H

#include "base/hk_common.h"

typedef enum hk_log_level {
    HK_LOG_DEBUG,
    HK_LOG_INFO,
    HK_LOG_WARN,
    HK_LOG_ERROR,
    HK_LOG_FATAL,
} hk_log_level;

/* msg is not NUL-terminated by contract; len excludes any terminator. */
typedef void (*hk_log_sink_fn)(hk_log_level level, const char *msg, size_t len);

/* Set during startup only (not thread-safe). NULL restores the default sink. */
void hk_log_set_sink(hk_log_sink_fn fn);
void hk_log_set_level(hk_log_level min_level);

void hk_log(hk_log_level level, const char *fmt, ...);

#define HK_LOGD(...) hk_log(HK_LOG_DEBUG, __VA_ARGS__)
#define HK_LOGI(...) hk_log(HK_LOG_INFO, __VA_ARGS__)
#define HK_LOGW(...) hk_log(HK_LOG_WARN, __VA_ARGS__)
#define HK_LOGE(...) hk_log(HK_LOG_ERROR, __VA_ARGS__)

#endif
