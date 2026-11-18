/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "base/hk_common.h"
#include "base/hk_log.h"

#include <stdlib.h>

static hk_panic_hook_fn g_panic_hook;

const char *hk_status_name(hk_status s)
{
    switch (s) {
    case HK_OK: return "ok";
    case HK_ERR_NOMEM: return "out of memory";
    case HK_ERR_INVALID: return "invalid";
    case HK_ERR_IO: return "i/o error";
    case HK_ERR_UNSUPPORTED: return "unsupported";
    case HK_ERR_NOT_FOUND: return "not found";
    case HK_ERR_LIMIT: return "limit exceeded";
    }
    return "unknown";
}

void hk_set_panic_hook(hk_panic_hook_fn fn)
{
    g_panic_hook = fn;
}

void hk_panic(const char *file, int line, const char *msg)
{
    hk_log(HK_LOG_FATAL, "panic at %s:%d: %s", file, line, msg);
    if (g_panic_hook) g_panic_hook(file, line, msg);
    abort();
}
