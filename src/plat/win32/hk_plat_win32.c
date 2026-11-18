/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_plat.h"
#include "base/hk_log.h"
#include "base/hk_utf8.h"
#include "base/hk_vec.h"

#include <windows.h>
#include <psapi.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_stderr;
static LARGE_INTEGER g_qpc_freq;

/* --- logging ----------------------------------------------------------------- */

static void win32_log_sink(hk_log_level level, const char *msg, size_t len)
{
    HK_UNUSED(level);
    char buf[1100];
    size_t n = HK_MIN(len, sizeof buf - 2);
    memcpy(buf, msg, n);
    buf[n++] = '\n';
    buf[n] = '\0';
    OutputDebugStringA(buf);
    if (g_stderr) {
        DWORD written;
        WriteFile(g_stderr, buf, (DWORD)n, &written, NULL);
    }
}

/* --- mitigations ------------------------------------------------------------- */

static void set_policy(PROCESS_MITIGATION_POLICY kind, void *policy, size_t size, const char *name)
{
    if (!SetProcessMitigationPolicy(kind, policy, size)) {
        HK_LOGW("mitigation %s not applied (error %lu)", name, GetLastError());
    }
}

/*
 * See docs/design/01-architecture.md, section 5. Deliberately not applied:
 *  - child process prohibition: "open folder" / external players need to spawn processes
 *  - non-system font blocking: would hide fonts users installed per-user
 *  - Microsoft-signed-only images: would block third-party IMEs
 */
static void apply_mitigations(void)
{
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext = {0};
    ext.DisableExtensionPoints = 1; /* AppInit DLLs, global hooks, legacy IMM-only IMEs */
    set_policy(ProcessExtensionPointDisablePolicy, &ext, sizeof ext, "extension points");

    PROCESS_MITIGATION_IMAGE_LOAD_POLICY img = {0};
    img.NoRemoteImages = 1;
    img.NoLowMandatoryLabelImages = 1;
    img.PreferSystem32Images = 1;
    set_policy(ProcessImageLoadPolicy, &img, sizeof img, "image load");

    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY handles = {0};
    handles.RaiseExceptionOnInvalidHandleReference = 1;
    handles.HandleExceptionsPermanentlyEnabled = 1;
    set_policy(ProcessStrictHandleCheckPolicy, &handles, sizeof handles, "strict handles");

#if !defined(__SANITIZE_ADDRESS__)
    /* ASan patches code at runtime, so ACG is only enabled in non-ASan builds. */
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY code = {0};
    code.ProhibitDynamicCode = 1;
    set_policy(ProcessDynamicCodePolicy, &code, sizeof code, "dynamic code");
#endif
}

/* --- init / time / memory ---------------------------------------------------- */

void hk_plat_init(void)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) g_stderr = h;
    hk_log_set_sink(win32_log_sink);

    /* Fail fast without the CRT's message box. */
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);

    QueryPerformanceFrequency(&g_qpc_freq);
    apply_mitigations();
}

uint64_t hk_plat_now_ns(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t ticks = (uint64_t)now.QuadPart;
    uint64_t freq = (uint64_t)g_qpc_freq.QuadPart;
    return (ticks / freq) * 1000000000ull + (ticks % freq) * 1000000000ull / freq;
}

static uint64_t filetime_u64(FILETIME ft)
{
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

double hk_plat_ms_since_process_start(void)
{
    FILETIME create, exit_time, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit_time, &kernel, &user)) return -1.0;
    GetSystemTimePreciseAsFileTime(&now);
    return (double)(filetime_u64(now) - filetime_u64(create)) / 10000.0; /* 100 ns units */
}

void hk_plat_mem_info_get(hk_plat_mem_info *out)
{
    PROCESS_MEMORY_COUNTERS_EX pmc = {0};
    pmc.cb = sizeof pmc;
    memset(out, 0, sizeof *out);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc)) {
        out->private_bytes = pmc.PrivateUsage;
        out->working_set = pmc.WorkingSetSize;
    }
}

/* --- command line -------------------------------------------------------------- */

/*
 * Splits the command line with the MSVC CRT rules (quotes, 2n / 2n+1 backslashes).
 * Avoids CommandLineToArgvW, which would pull shell32.dll into the process.
 */
static int split_args(hk_arena *a, const wchar_t *cmd, char ***out_argv)
{
    size_t cmd_len = wcslen(cmd);
    wchar_t *buf = HK_ARENA_NEW_ARRAY(a, wchar_t, cmd_len + 1);
    HK_VEC(char *) args = {0};

    const wchar_t *p = cmd;
    for (;;) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        size_t n = 0;
        bool quoted = false;
        while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
            size_t slashes = 0;
            while (*p == L'\\') {
                slashes++;
                p++;
            }
            if (*p == L'"') {
                for (size_t i = 0; i < slashes / 2; i++) buf[n++] = L'\\';
                if (slashes % 2) {
                    buf[n++] = L'"';
                } else if (quoted && p[1] == L'"') {
                    buf[n++] = L'"'; /* "" inside quotes is a literal quote */
                    p++;
                } else {
                    quoted = !quoted;
                }
                p++;
            } else {
                for (size_t i = 0; i < slashes; i++) buf[n++] = L'\\';
                if (*p && (quoted || (*p != L' ' && *p != L'\t'))) buf[n++] = *p++;
            }
        }
        hk_str arg = hk_utf16_to_utf8(a, (const uint16_t *)buf, n);
        hk_vec_push(&args, (char *)arg.p);
    }

    char **argv = HK_ARENA_NEW_ARRAY(a, char *, (size_t)args.len + 1);
    if (args.len) memcpy(argv, args.data, args.len * sizeof(char *));
    int argc = (int)args.len;
    hk_vec_free(&args);
    *out_argv = argv;
    return argc;
}

/* --- entry point ------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmd_line, int show)
{
    HK_UNUSED(instance);
    HK_UNUSED(prev);
    HK_UNUSED(cmd_line);
    HK_UNUSED(show);

    hk_plat_init();

    hk_arena args_arena;
    hk_arena_init(&args_arena, 4096);
    char **argv;
    int argc = split_args(&args_arena, GetCommandLineW(), &argv);
    int rc = hk_app_main(argc, argv);
    hk_arena_free(&args_arena);
    return rc;
}
