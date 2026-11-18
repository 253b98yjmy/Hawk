/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Image decoding (WIC on Windows). Used from the I/O thread. */
#ifndef HK_IMAGE_H
#define HK_IMAGE_H

#include "base/hk_common.h"

typedef struct hk_image {
    int width, height;           /* decoded size (possibly reduced) */
    int intrinsic_w, intrinsic_h; /* size of the image in the file */
    uint32_t *pixels;            /* straight-alpha 0xAARRGGBB, width * height, from hk_alloc */
} hk_image;

/*
 * Decodes the first frame. If the image is larger than max_w x max_h (0 = no limit) it is
 * scaled down while decoding, keeping its aspect ratio, so memory matches the display size.
 */
hk_status hk_image_decode(const uint8_t *data, size_t n, int max_w, int max_h, hk_image *out);
void hk_image_free(hk_image *img);

#endif
