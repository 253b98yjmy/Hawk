/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_log.h"

#include <stdarg.h>
#include <stdio.h>

static void default_sink(hk_log_level level, const char *msg, size_t len)
{
    HK_UNUSED(level);
    fwrite(msg, 1, len, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

static hk_log_sink_fn g_sink = default_sink;
static hk_log_level g_min_level = HK_LOG_INFO;

void hk_log_set_sink(hk_log_sink_fn fn)
{
    g_sink = fn ? fn : default_sink;
}

void hk_log_set_level(hk_log_level min_level)
{
    g_min_level = min_level;
}

void hk_log(hk_log_level level, const char *fmt, ...)
{
    static const char *const k_names[] = {"debug", "info", "warn", "error", "fatal"};
    if (level < g_min_level) return;

    char buf[1024];
    int prefix = snprintf(buf, sizeof buf, "[%s] ", k_names[level]);
    if (prefix < 0) prefix = 0;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + prefix, sizeof buf - (size_t)prefix, fmt, ap);
    va_end(ap);

    size_t len = (size_t)prefix;
    if (n > 0) len = HK_MIN(len + (size_t)n, sizeof buf - 1);
    g_sink(level, buf, len);
}
