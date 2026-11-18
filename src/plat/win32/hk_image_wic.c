/* SPDX-License-Identifier: GPL-3.0-or-later */
#define COBJMACROS
#include <windows.h>
#include <wincodec.h>

#include "plat/hk_image.h"
#include "base/hk_mem.h"

static DWORD g_tls = TLS_OUT_OF_INDEXES;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_tls(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    HK_UNUSED(once);
    HK_UNUSED(param);
    HK_UNUSED(ctx);
    g_tls = TlsAlloc();
    return TRUE;
}

/* One WIC factory per thread (COM is initialized on first use). */
static IWICImagingFactory *factory(void)
{
    InitOnceExecuteOnce(&g_once, init_tls, NULL, NULL);
    IWICImagingFactory *f = TlsGetValue(g_tls);
    if (f) return f;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&f)))
        return NULL;
    TlsSetValue(g_tls, f);
    return f;
}

hk_status hk_image_decode(const uint8_t *data, size_t n, int max_w, int max_h, hk_image *out)
{
    memset(out, 0, sizeof *out);
    IWICImagingFactory *f = factory();
    if (!f || n == 0 || n > 0x7FFFFFFF) return HK_ERR_UNSUPPORTED;
    hk_status st = HK_ERR_INVALID;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *conv = NULL;
    if (FAILED(IWICImagingFactory_CreateStream(f, &stream))) goto done;
    if (FAILED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)n))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
                                                          &dec)))
        goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))) goto done;
    UINT w, h;
    if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) || w == 0 || h == 0 || w > 30000 || h > 30000) goto done;
    out->intrinsic_w = (int)w;
    out->intrinsic_h = (int)h;
    UINT tw = w, th = h;
    if (max_w > 0 && max_h > 0 && (w > (UINT)max_w || h > (UINT)max_h)) {
        double sx = (double)max_w / w, sy = (double)max_h / h, s = sx < sy ? sx : sy;
        tw = (UINT)(w * s + 0.5);
        th = (UINT)(h * s + 0.5);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    IWICBitmapSource *src = (IWICBitmapSource *)frame;
    if (tw != w || th != h) {
        if (FAILED(IWICImagingFactory_CreateBitmapScaler(f, &scaler))) goto done;
        if (FAILED(IWICBitmapScaler_Initialize(scaler, src, tw, th, WICBitmapInterpolationModeFant))) goto done;
        src = (IWICBitmapSource *)scaler;
    }
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &conv))) goto done;
    if (FAILED(IWICFormatConverter_Initialize(conv, src, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL,
                                              0, WICBitmapPaletteTypeCustom)))
        goto done;
    size_t bytes = (size_t)tw * th * 4;
    uint32_t *px = hk_alloc(bytes);
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, tw * 4, (UINT)bytes, (BYTE *)px))) {
        hk_free(px, bytes);
        goto done;
    }
    out->width = (int)tw;
    out->height = (int)th;
    out->pixels = px;
    st = HK_OK;
done:
    if (conv) IWICFormatConverter_Release(conv);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    return st;
}

void hk_image_free(hk_image *img)
{
    if (img->pixels) hk_free(img->pixels, (size_t)img->width * (size_t)img->height * 4);
    img->pixels = NULL;
}
