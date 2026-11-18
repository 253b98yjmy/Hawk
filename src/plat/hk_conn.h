/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Blocking TCP connections with optional TLS (the OS TLS stack: SChannel on Windows).
 * Used from the I/O thread only. Certificate chain and host name are verified by the
 * OS; revocation is checked against the local cache only (no OCSP/CRL fetches).
 */
#ifndef HK_CONN_H
#define HK_CONN_H

#include "base/hk_common.h"
#include "base/hk_str.h"

typedef struct hk_conn hk_conn;

/* host: ASCII host name or IP literal (IPv6 with or without brackets). err receives a message. */
hk_status hk_conn_open(hk_str host, uint16_t port, bool tls, hk_conn **out, char *err, size_t errlen);
hk_status hk_conn_write(hk_conn *c, const void *p, size_t n);
/* Reads up to cap bytes; *got == 0 means the peer closed the connection. */
hk_status hk_conn_read(hk_conn *c, void *buf, size_t cap, size_t *got);
void hk_conn_close(hk_conn *c);

#endif
