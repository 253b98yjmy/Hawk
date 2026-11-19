/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_plat.h"
#include "base/hk_log.h"
#include "base/hk_utf8.h"
#include "base/hk_mem.h"
#include "base/hk_vec.h"

#include <windows.h>
#include <psapi.h>

#include "hk_win32_internal.h"
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
 *  - extension point disabling: Microsoft IME and Mozc then stop converting in Hawk's own window (the
 *    text fields of pages), although the address bar (an EDIT control) keeps working
 */
static void apply_mitigations(void)
{
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
    hk_win32_main_queue_init();
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

/* --- text services ------------------------------------------------------------- */

/* RFC 3492 Punycode encoding of one label (UTF-16, BMP + surrogate pairs), appended to out. */
static bool punycode_label(const wchar_t *w, int n, wchar_t *out, int cap, int *olen)
{
    enum { BASE = 36, TMIN = 1, TMAX = 26, SKEW = 38, DAMP = 700, INITIAL_BIAS = 72, INITIAL_N = 128 };
    uint32_t cps[256];
    int count = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00u);
        if (count == 256) return false;
        cps[count++] = c;
    }
    int o = *olen;
#define EMIT(ch)                                                                                   \
    do {                                                                                           \
        if (o >= cap) return false;                                                                \
        out[o++] = (wchar_t)(ch);                                                                  \
    } while (0)
    EMIT('x'); EMIT('n'); EMIT('-'); EMIT('-');
    int basic = 0;
    for (int i = 0; i < count; i++) {
        if (cps[i] < 0x80) {
            EMIT(cps[i]);
            basic++;
        }
    }
    if (basic) EMIT('-');
    uint32_t cp = INITIAL_N, delta = 0, bias = INITIAL_BIAS;
    for (int h = basic; h < count;) {
        uint32_t m = UINT32_MAX;
        for (int i = 0; i < count; i++) {
            if (cps[i] >= cp && cps[i] < m) m = cps[i];
        }
        delta += (m - cp) * (uint32_t)(h + 1);
        cp = m;
        for (int i = 0; i < count; i++) {
            if (cps[i] < cp) delta++;
            if (cps[i] != cp) continue;
            uint32_t q = delta;
            for (uint32_t k = BASE;; k += BASE) {
                uint32_t t = k <= bias ? TMIN : (k >= bias + TMAX ? TMAX : k - bias);
                if (q < t) break;
                uint32_t d = t + (q - t) % (BASE - t);
                EMIT(d < 26 ? 'a' + d : '0' + d - 26);
                q = (q - t) / (BASE - t);
            }
            EMIT(q < 26 ? 'a' + q : '0' + q - 26);
            /* adapt */
            uint32_t dl = h == basic ? delta / DAMP : delta / 2;
            dl += dl / (uint32_t)(h + 1);
            uint32_t k = 0;
            while (dl > ((BASE - TMIN) * TMAX) / 2) {
                dl /= BASE - TMIN;
                k += BASE;
            }
            bias = k + (BASE - TMIN + 1) * dl / (dl + SKEW);
            delta = 0;
            h++;
        }
        delta++;
        cp++;
    }
#undef EMIT
    *olen = o;
    return true;
}

bool hk_plat_idna_to_ascii(hk_arena *a, hk_str domain, hk_str *out)
{
    hk_arena_mark m = hk_arena_get_mark(a);
    size_t wlen = 0;
    const wchar_t *w = (const wchar_t *)hk_utf8_to_utf16(a, domain, &wlen);
    if (wlen == 0 || wlen > 1024) {
        hk_arena_rewind(a, m);
        return false;
    }
    wchar_t ascii[1024];
    int n;
    /*
     * IdnToAscii applies transitional processing (ß -> ss, ς -> σ), but the URL Standard is
     * non-transitional. Names with those deviation characters are lowercased, normalized and
     * Punycode-encoded here (without the remaining UTS #46 validation).
     */
    bool deviation = false;
    for (size_t i = 0; i < wlen; i++) deviation = deviation || w[i] == 0x00DF || w[i] == 0x03C2;
    if (deviation) {
        wchar_t lower[1024], nfc[1024];
        int ln = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w, (int)wlen, lower,
                               (int)HK_ARRAY_LEN(lower), NULL, NULL, 0);
        int nn = ln > 0 ? NormalizeString(NormalizationC, lower, ln, nfc, (int)HK_ARRAY_LEN(nfc)) : 0;
        n = 0;
        for (int i = 0, start = 0; nn > 0 && i <= nn; i++) {
            if (i < nn && nfc[i] != L'.') continue;
            bool ascii_label = true;
            for (int k = start; k < i; k++) ascii_label = ascii_label && nfc[k] < 0x80;
            if (ascii_label) {
                for (int k = start; k < i && n < (int)HK_ARRAY_LEN(ascii); k++) ascii[n++] = nfc[k];
            } else if (!punycode_label(nfc + start, i - start, ascii, (int)HK_ARRAY_LEN(ascii), &n)) {
                n = 0;
                break;
            }
            if (i < nn && n < (int)HK_ARRAY_LEN(ascii)) ascii[n++] = L'.';
            start = i + 1;
        }
    } else {
        n = IdnToAscii(0, w, (int)wlen, ascii, (int)HK_ARRAY_LEN(ascii));
    }
    hk_arena_rewind(a, m);
    if (n <= 0) return false;
    char *s = hk_arena_alloc(a, (size_t)n + 1, 1);
    for (int i = 0; i < n; i++) {
        if (ascii[i] >= 0x80) return false;
        s[i] = hk_ascii_lower((char)ascii[i]);
    }
    s[n] = 0;
    *out = (hk_str){s, (size_t)n};
    return true;
}

void hk_plat_decode_codepage(uint32_t codepage, const uint8_t *p, size_t n, hk_charbuf *out)
{
    if (n == 0) return;
    HK_ASSERT(n < INT32_MAX / 2);
    int wn = MultiByteToWideChar(codepage, 0, (const char *)p, (int)n, NULL, 0);
    if (wn <= 0) return;
    wchar_t *w = hk_alloc((size_t)wn * sizeof(wchar_t));
    MultiByteToWideChar(codepage, 0, (const char *)p, (int)n, w, wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
    if (un > 0) {
        hk_vec_reserve(out, (uint64_t)out->len + (uint64_t)un);
        WideCharToMultiByte(CP_UTF8, 0, w, wn, out->data + out->len, un, NULL, NULL);
        out->len += (uint32_t)un;
    }
    hk_free(w, (size_t)wn * sizeof(wchar_t));
}

bool hk_plat_encode_codepage(uint32_t codepage, uint32_t cp, hk_charbuf *out)
{
    wchar_t w[2];
    int wn = 1;
    if (cp >= 0x10000) {
        w[0] = (wchar_t)(0xD800 | ((cp - 0x10000) >> 10));
        w[1] = (wchar_t)(0xDC00 | ((cp - 0x10000) & 0x3FF));
        wn = 2;
    } else {
        w[0] = (wchar_t)cp;
    }
    char buf[16];
    BOOL used_default = FALSE;
    /* ISO-2022-JP (50220) and other stateful code pages do not accept the default-char flags. */
    bool stateful = codepage >= 50000 && codepage < 60000;
    int n = WideCharToMultiByte(codepage, stateful ? 0 : WC_NO_BEST_FIT_CHARS, w, wn, buf, (int)sizeof buf, NULL,
                                stateful ? NULL : &used_default);
    if (n <= 0 || used_default || (stateful && n == 1 && buf[0] == '?' && cp != '?')) return false;
    hk_charbuf_append(out, buf, (size_t)n);
    return true;
}

bool hk_plat_is_lead_byte(uint32_t codepage, uint8_t b)
{
    return IsDBCSLeadByteEx(codepage, b) != 0;
}

bool hk_plat_locale_is_japanese(void)
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE;
}

