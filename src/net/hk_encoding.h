/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Character encodings (WHATWG Encoding Standard): label lookup, BOM and <meta>
 * sniffing, a Japanese-aware fallback guess, and streaming decoders to UTF-8.
 */
#ifndef HK_ENCODING_H
#define HK_ENCODING_H

#include "base/hk_common.h"
#include "base/hk_str.h"
#include "base/hk_vec.h"

typedef enum hk_encoding {
    HK_ENC_UNKNOWN = 0,
    HK_ENC_UTF8,
    HK_ENC_UTF16LE,
    HK_ENC_UTF16BE,
    HK_ENC_WINDOWS_1252,
    HK_ENC_SHIFT_JIS,
    HK_ENC_EUC_JP,
    HK_ENC_ISO_2022_JP,
    HK_ENC_GB18030,
    HK_ENC_GBK,
    HK_ENC_BIG5,
    HK_ENC_EUC_KR,
    HK_ENC_ISO_8859_2,
    HK_ENC_ISO_8859_5,
    HK_ENC_ISO_8859_7,
    HK_ENC_ISO_8859_15,
    HK_ENC_WINDOWS_1250,
    HK_ENC_WINDOWS_1251,
    HK_ENC_WINDOWS_1253,
    HK_ENC_WINDOWS_1254,
    HK_ENC_WINDOWS_1255,
    HK_ENC_WINDOWS_1256,
    HK_ENC_WINDOWS_1257,
    HK_ENC_WINDOWS_1258,
    HK_ENC_WINDOWS_874,
    HK_ENC_KOI8_R,
    HK_ENC_KOI8_U,
    HK_ENC_IBM866,
    HK_ENC_COUNT_
} hk_encoding;

/* "Get an encoding" from a label (case-insensitive, surrounding whitespace ignored). */
hk_encoding hk_encoding_from_label(hk_str label);
const char *hk_encoding_name(hk_encoding e);

/* Extracts the charset parameter of a Content-Type header value, or HK_ENC_UNKNOWN. */
hk_encoding hk_encoding_from_content_type(hk_str content_type);

/* BOM sniffing; *bom_len receives the number of bytes to skip. */
hk_encoding hk_encoding_from_bom(const uint8_t *p, size_t n, size_t *bom_len);

/* "Prescan a byte stream to determine its encoding" over (at most) the first 1024 bytes. */
hk_encoding hk_encoding_prescan(const uint8_t *p, size_t n);

/* Fallback when nothing declares the encoding: UTF-8 / Shift_JIS / EUC-JP / ISO-2022-JP
 * detection, otherwise the locale default (Shift_JIS for Japanese, else windows-1252). */
hk_encoding hk_encoding_guess(const uint8_t *p, size_t n, bool japanese_locale);

/* The full sniffing order: BOM, transport (HTTP charset), <meta> prescan, guess. */
hk_encoding hk_encoding_sniff(const uint8_t *p, size_t n, hk_encoding transport, bool japanese_locale,
                              size_t *bom_len);

/* Streaming decoder to UTF-8; invalid input becomes U+FFFD. */
typedef struct hk_decoder {
    hk_encoding enc;
    uint8_t pending[4];
    uint32_t pending_len;
    hk_charbuf held; /* whole-input encodings (stateful ones) are decoded at the end */
} hk_decoder;

void hk_decoder_init(hk_decoder *d, hk_encoding enc);
void hk_decoder_free(hk_decoder *d);
/* Appends the decoded UTF-8 to out. last = true flushes incomplete sequences as U+FFFD. */
void hk_decoder_decode(hk_decoder *d, const uint8_t *p, size_t n, bool last, hk_charbuf *out);

#endif
