/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Winsock + SChannel connections. */
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <subauth.h> /* UNICODE_STRING, needed by schannel.h */
#include <security.h>
#include <schannel.h>
#include <sspi.h>

#include "plat/hk_conn.h"
#include "base/hk_log.h"
#include "base/hk_mem.h"
#include "base/hk_vec.h"

#include <stdio.h>
#include <string.h>

#define IO_TIMEOUT_MS 30000

struct hk_conn {
    SOCKET sock;
    bool tls;
    CtxtHandle ctx;
    bool has_ctx;
    SecPkgContext_StreamSizes sizes;
    hk_charbuf enc;   /* received, not yet decrypted bytes */
    hk_charbuf plain; /* decrypted, not yet returned bytes */
    uint32_t plain_pos;
    bool closed;
};

static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
static CredHandle g_cred;
static bool g_cred_ok;

static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    HK_UNUSED(once);
    HK_UNUSED(param);
    HK_UNUSED(ctx);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    TLS_PARAMETERS tp = {0};
    tp.grbitDisabledProtocols = (DWORD) ~(SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT);
    SCH_CREDENTIALS cred = {0};
    cred.dwVersion = SCH_CREDENTIALS_VERSION;
    cred.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                   SCH_CRED_REVOCATION_CHECK_CACHE_ONLY | SCH_CRED_IGNORE_NO_REVOCATION_CHECK |
                   SCH_CRED_IGNORE_REVOCATION_OFFLINE;
    cred.cTlsParameters = 1;
    cred.pTlsParameters = &tp;
    SECURITY_STATUS st = AcquireCredentialsHandleW(NULL, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL,
                                                   &cred, NULL, NULL, &g_cred, NULL);
    g_cred_ok = st == SEC_E_OK;
    if (!g_cred_ok) HK_LOGE("AcquireCredentialsHandle failed: 0x%08lx", (unsigned long)st);
    return TRUE;
}

static void set_err(char *err, size_t n, const char *fmt, unsigned long code)
{
    if (err && n) snprintf(err, n, fmt, code);
}

static const char *sec_error_text(SECURITY_STATUS st)
{
    switch ((unsigned long)st) {
    case 0x80090325: return "certificate is not trusted (untrusted root)";
    case 0x80090322: return "certificate does not match the host name";
    case 0x80090328: return "certificate has expired";
    case 0x80090327: return "certificate error";
    case 0x80090331: return "no common TLS version or cipher suite";
    case 0x80090308: return "invalid TLS token";
    case 0x80090326: return "TLS protocol error";
    default: return "TLS handshake failed";
    }
}

/* --- raw socket I/O --------------------------------------------------------------------- */

static bool send_all(SOCKET s, const char *p, size_t n)
{
    while (n) {
        int k = send(s, p, (int)HK_MIN(n, (size_t)1 << 20), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* Appends received bytes to b. Returns bytes read, 0 on close, -1 on error. */
static int recv_into(SOCKET s, hk_charbuf *b)
{
    hk_vec_reserve(b, (uint64_t)b->len + 16384);
    int k = recv(s, b->data + b->len, (int)(b->cap - b->len), 0);
    if (k > 0) b->len += (uint32_t)k;
    return k;
}

static void consume_front(hk_charbuf *b, size_t n)
{
    memmove(b->data, b->data + n, b->len - n);
    b->len -= (uint32_t)n;
}

/* --- TLS handshake ------------------------------------------------------------------------ */

static const DWORD k_isc_flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                 ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;

/*
 * Drives InitializeSecurityContext until the handshake completes. Used for the initial
 * handshake (first == true) and for post-handshake messages (TLS 1.3 tickets).
 */
static SECURITY_STATUS handshake(hk_conn *c, const wchar_t *target, bool first)
{
    SECURITY_STATUS st = SEC_I_CONTINUE_NEEDED;
    if (first) {
        SecBuffer out = {0, SECBUFFER_TOKEN, NULL};
        SecBufferDesc outd = {SECBUFFER_VERSION, 1, &out};
        DWORD attrs;
        st = InitializeSecurityContextW(&g_cred, NULL, (SEC_WCHAR *)target, k_isc_flags, 0, 0, NULL, 0,
                                        &c->ctx, &outd, &attrs, NULL);
        if (st != SEC_I_CONTINUE_NEEDED) return st;
        c->has_ctx = true;
        bool ok = send_all(c->sock, out.pvBuffer, out.cbBuffer);
        FreeContextBuffer(out.pvBuffer);
        if (!ok) return SEC_E_INTERNAL_ERROR;
    }
    for (;;) {
        if (c->enc.len == 0 || st == SEC_E_INCOMPLETE_MESSAGE) {
            int k = recv_into(c->sock, &c->enc);
            if (k <= 0) return SEC_E_INTERNAL_ERROR;
        }
        SecBuffer in[2] = {{c->enc.len, SECBUFFER_TOKEN, c->enc.data}, {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc ind = {SECBUFFER_VERSION, 2, in};
        SecBuffer out[3] = {{0, SECBUFFER_TOKEN, NULL}, {0, SECBUFFER_ALERT, NULL}, {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc outd = {SECBUFFER_VERSION, 3, out};
        DWORD attrs;
        st = InitializeSecurityContextW(&g_cred, &c->ctx, (SEC_WCHAR *)target, k_isc_flags, 0, 0, &ind, 0,
                                        NULL, &outd, &attrs, NULL);
        if (out[0].cbBuffer && out[0].pvBuffer) {
            bool ok = send_all(c->sock, out[0].pvBuffer, out[0].cbBuffer);
            FreeContextBuffer(out[0].pvBuffer);
            if (!ok) return SEC_E_INTERNAL_ERROR;
        }
        if (out[1].pvBuffer) FreeContextBuffer(out[1].pvBuffer);
        if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
        if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer) consume_front(&c->enc, c->enc.len - in[1].cbBuffer);
        else c->enc.len = 0;
        if (st == SEC_E_OK) return SEC_E_OK;
        if (st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS) {
            st = SEC_I_CONTINUE_NEEDED;
            continue;
        }
        return st;
    }
}

/* --- public API ------------------------------------------------------------------------------ */

hk_status hk_conn_open(hk_str host, uint16_t port, bool tls, hk_conn **out, char *err, size_t errlen)
{
    *out = NULL;
    InitOnceExecuteOnce(&g_init_once, init_once, NULL, NULL);
    if (tls && !g_cred_ok) {
        set_err(err, errlen, "TLS is not available (0x%lx)", 0);
        return HK_ERR_IO;
    }
    if (host.n >= 2 && host.p[0] == '[' && host.p[host.n - 1] == ']') host = hk_str_slice(host, 1, host.n - 1);
    char name[256], service[8];
    if (host.n == 0 || host.n >= sizeof name) {
        set_err(err, errlen, "invalid host name (%lu)", (unsigned long)host.n);
        return HK_ERR_INVALID;
    }
    memcpy(name, host.p, host.n);
    name[host.n] = 0;
    snprintf(service, sizeof service, "%u", port);

    ADDRINFOA hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int gai = getaddrinfo(name, service, &hints, &res);
    if (gai != 0) {
        set_err(err, errlen, "could not resolve the host name (error %lu)", (unsigned long)gai);
        return HK_ERR_IO;
    }
    SOCKET s = INVALID_SOCKET;
    int last_error = 0;
    for (ADDRINFOA *ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        DWORD timeout = IO_TIMEOUT_MS;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
        BOOL nodelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        last_error = WSAGetLastError();
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) {
        set_err(err, errlen, "could not connect (error %lu)", (unsigned long)last_error);
        return HK_ERR_IO;
    }

    hk_conn *c = hk_alloc_zero(sizeof *c);
    c->sock = s;
    c->tls = tls;
    if (tls) {
        wchar_t target[256];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, target, 256);
        SECURITY_STATUS st = handshake(c, target, true);
        if (st != SEC_E_OK) {
            if (err && errlen) snprintf(err, errlen, "%s (0x%08lx)", sec_error_text(st), (unsigned long)st);
            hk_conn_close(c);
            return HK_ERR_IO;
        }
        QueryContextAttributesW(&c->ctx, SECPKG_ATTR_STREAM_SIZES, &c->sizes);
    }
    *out = c;
    return HK_OK;
}

hk_status hk_conn_write(hk_conn *c, const void *p, size_t n)
{
    if (!c->tls) return send_all(c->sock, p, n) ? HK_OK : HK_ERR_IO;
    const char *src = p;
    size_t max = c->sizes.cbMaximumMessage;
    size_t total = c->sizes.cbHeader + max + c->sizes.cbTrailer;
    char *buf = hk_alloc(total);
    hk_status result = HK_OK;
    while (n) {
        size_t k = HK_MIN(n, max);
        memcpy(buf + c->sizes.cbHeader, src, k);
        SecBuffer b[4] = {
            {c->sizes.cbHeader, SECBUFFER_STREAM_HEADER, buf},
            {(unsigned long)k, SECBUFFER_DATA, buf + c->sizes.cbHeader},
            {c->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buf + c->sizes.cbHeader + k},
            {0, SECBUFFER_EMPTY, NULL},
        };
        SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&c->ctx, 0, &d, 0) != SEC_E_OK ||
            !send_all(c->sock, buf, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) {
            result = HK_ERR_IO;
            break;
        }
        src += k;
        n -= k;
    }
    hk_free(buf, total);
    return result;
}

hk_status hk_conn_read(hk_conn *c, void *buf, size_t cap, size_t *got)
{
    *got = 0;
    if (!c->tls) {
        int k = recv(c->sock, buf, (int)HK_MIN(cap, (size_t)INT32_MAX), 0);
        if (k < 0) return HK_ERR_IO;
        *got = (size_t)k;
        return HK_OK;
    }
    for (;;) {
        if (c->plain_pos < c->plain.len) {
            size_t k = HK_MIN(cap, (size_t)(c->plain.len - c->plain_pos));
            memcpy(buf, c->plain.data + c->plain_pos, k);
            c->plain_pos += (uint32_t)k;
            *got = k;
            return HK_OK;
        }
        c->plain.len = c->plain_pos = 0;
        if (c->closed) return HK_OK;
        if (c->enc.len) {
            SecBuffer b[4] = {{c->enc.len, SECBUFFER_DATA, c->enc.data},
                              {0, SECBUFFER_EMPTY, NULL},
                              {0, SECBUFFER_EMPTY, NULL},
                              {0, SECBUFFER_EMPTY, NULL}};
            SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
            SECURITY_STATUS st = DecryptMessage(&c->ctx, &d, 0, NULL);
            if (st == SEC_E_OK || st == SEC_I_RENEGOTIATE || st == SEC_I_CONTEXT_EXPIRED) {
                size_t extra = 0;
                for (int i = 0; i < 4; i++) {
                    if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer)
                        hk_charbuf_append(&c->plain, b[i].pvBuffer, b[i].cbBuffer);
                    if (b[i].BufferType == SECBUFFER_EXTRA) extra = b[i].cbBuffer;
                }
                if (extra) consume_front(&c->enc, c->enc.len - extra);
                else c->enc.len = 0;
                if (st == SEC_I_CONTEXT_EXPIRED) c->closed = true;
                if (st == SEC_I_RENEGOTIATE) {
                    /* TLS 1.3 post-handshake message (e.g. a session ticket). */
                    SECURITY_STATUS hs = handshake(c, NULL, false);
                    if (hs != SEC_E_OK) return HK_ERR_IO;
                }
                continue;
            }
            if (st != SEC_E_INCOMPLETE_MESSAGE) return HK_ERR_IO;
        }
        int k = recv_into(c->sock, &c->enc);
        if (k == 0) {
            c->closed = true;
            return HK_OK;
        }
        if (k < 0) return HK_ERR_IO;
    }
}

void hk_conn_close(hk_conn *c)
{
    if (!c) return;
    if (c->has_ctx) DeleteSecurityContext(&c->ctx);
    if (c->sock != INVALID_SOCKET) closesocket(c->sock);
    hk_vec_free(&c->enc);
    hk_vec_free(&c->plain);
    hk_free(c, sizeof *c);
}
