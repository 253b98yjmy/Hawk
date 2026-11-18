// SPDX-License-Identifier: GPL-3.0-or-later
// DirectWrite behind the C API of plat/hk_text.h. dwrite.h is C++ only, so this one file is
// C++: no exceptions, no RTTI, no standard library; all memory goes through hk_alloc.
#include <windows.h>
#include <dwrite_2.h>

extern "C" {
#include "plat/hk_text.h"
#include "base/hk_mem.h"
#include "base/hk_utf8.h"
}

#include <new>
#include <string.h>

namespace {

IDWriteFactory2 *g_factory;
IDWriteFontCollection *g_fonts;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK init_once(PINIT_ONCE, PVOID, PVOID *)
{
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                      reinterpret_cast<IUnknown **>(&g_factory))))
        g_factory->GetSystemFontCollection(&g_fonts, FALSE);
    return TRUE;
}

bool ready()
{
    InitOnceExecuteOnce(&g_once, init_once, nullptr, nullptr);
    return g_factory && g_fonts;
}

hk_str default_family()
{
    hk_str s = {"Segoe UI", 8};
    return s;
}

// UTF-8 -> NUL-terminated UTF-16 in a fixed buffer.
void widen(hk_str s, wchar_t *out, int cap)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.p, (int)s.n, out, cap - 1);
    out[n > 0 ? n : 0] = 0;
}

// Drawing effect: carries the color and user value of a text range.
class Effect : public IUnknown {
  public:
    Effect(uint32_t color, uint32_t user) : refs_(1), color(color), user(user) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IUnknown)) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --refs_;
        if (!r) {
            this->~Effect();
            hk_free(this, sizeof *this);
        }
        return r;
    }
    static Effect *make(uint32_t color, uint32_t user)
    {
        void *mem = hk_alloc(sizeof(Effect));
        return new (mem) Effect(color, user);
    }

  private:
    ULONG refs_;

  public:
    uint32_t color, user;
};

struct EmitCtx {
    void (*on_run)(void *, const hk_glyph_run *);
    void (*on_rect)(void *, const hk_text_rect *);
    void *user;
    uint32_t default_color;
};

class Renderer : public IDWriteTextRenderer {
  public:
    explicit Renderer(EmitCtx *c) : c_(c) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *out) override
    {
        *out = TRUE; /* layout is in CSS px; snapping happens when painting at device scale */
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *m) override
    {
        DWRITE_MATRIX id = {1, 0, 0, 1, 0, 0};
        *m = id;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *out) override
    {
        *out = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN *run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION *, IUnknown *effect) override
    {
        if (!c_->on_run || run->glyphCount == 0) return S_OK;
        hk_glyph_run r = {};
        r.face = run->fontFace;
        run->fontFace->AddRef();
        r.size = run->fontEmSize;
        r.x = x;
        r.baseline = y;
        r.count = run->glyphCount;
        r.glyphs = static_cast<uint16_t *>(hk_alloc(run->glyphCount * sizeof(uint16_t)));
        r.advances = static_cast<float *>(hk_alloc(run->glyphCount * sizeof(float)));
        r.offsets = static_cast<float *>(hk_alloc(run->glyphCount * 2 * sizeof(float)));
        memcpy(r.glyphs, run->glyphIndices, run->glyphCount * sizeof(uint16_t));
        memcpy(r.advances, run->glyphAdvances, run->glyphCount * sizeof(float));
        for (UINT32 i = 0; i < run->glyphCount; i++) {
            r.offsets[2 * i] = run->glyphOffsets ? run->glyphOffsets[i].advanceOffset : 0;
            r.offsets[2 * i + 1] = run->glyphOffsets ? run->glyphOffsets[i].ascenderOffset : 0;
        }
        r.rtl = (run->bidiLevel & 1) != 0;
        Effect *e = static_cast<Effect *>(effect);
        r.color = e ? e->color : c_->default_color;
        r.user = e ? e->user : 0;
        c_->on_run(c_->user, &r);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT x, FLOAT y, const DWRITE_UNDERLINE *u, IUnknown *effect) override
    {
        return rect(x, y + u->offset, u->width, u->thickness, effect);
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH *s,
                                                IUnknown *effect) override
    {
        return rect(x, y + s->offset, s->width, s->thickness, effect);
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL, BOOL, IUnknown *) override
    {
        return S_OK;
    }

  private:
    HRESULT rect(float x, float y, float w, float h, IUnknown *effect)
    {
        if (!c_->on_rect) return S_OK;
        Effect *e = static_cast<Effect *>(effect);
        hk_text_rect r = {x, y, w, h < 1 ? 1 : h, e ? e->color : c_->default_color, e ? e->user : 0};
        c_->on_rect(c_->user, &r);
        return S_OK;
    }
    EmitCtx *c_;
};

} // namespace

struct hk_text_layout {
    IDWriteTextLayout *layout;
    uint32_t default_color;
};

extern "C" {

bool hk_font_family_exists(hk_str family)
{
    if (!ready() || family.n == 0 || family.n > 120) return false;
    wchar_t name[128];
    widen(family, name, 128);
    UINT32 index;
    BOOL exists = FALSE;
    return SUCCEEDED(g_fonts->FindFamilyName(name, &index, &exists)) && exists;
}

static void apply_style(IDWriteTextLayout *l, DWRITE_TEXT_RANGE r, const hk_text_style *s)
{
    wchar_t family[128];
    widen(s->family.n ? s->family : default_family(), family, 128);
    l->SetFontFamilyName(family, r);
    l->SetFontSize(s->size > 0 ? s->size : 16, r);
    l->SetFontWeight((DWRITE_FONT_WEIGHT)s->weight, r);
    l->SetFontStyle(s->italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, r);
    l->SetUnderline(s->underline, r);
    l->SetStrikethrough(s->line_through, r);
    Effect *e = Effect::make(s->color, s->user);
    l->SetDrawingEffect(e, r);
    e->Release();
}

hk_text_layout *hk_text_layout_create(const uint16_t *text, uint32_t len, const hk_text_para *para,
                                      const hk_text_style *base)
{
    if (!ready()) return nullptr;
    IDWriteTextFormat *format = nullptr;
    wchar_t family[128];
    widen(base->family.n ? base->family : default_family(), family, 128);
    if (FAILED(g_factory->CreateTextFormat(family, nullptr, (DWRITE_FONT_WEIGHT)base->weight,
                                           base->italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, base->size > 0 ? base->size : 16, L"",
                                           &format)))
        return nullptr;
    IDWriteTextLayout *layout = nullptr;
    float width = para->max_width > 1 ? para->max_width : 1;
    HRESULT hr = g_factory->CreateTextLayout(reinterpret_cast<const WCHAR *>(text), len, format, width, 1e7f, &layout);
    format->Release();
    if (FAILED(hr)) return nullptr;
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    if (para->align == HK_TEXT_RIGHT) align = DWRITE_TEXT_ALIGNMENT_TRAILING;
    else if (para->align == HK_TEXT_CENTER) align = DWRITE_TEXT_ALIGNMENT_CENTER;
    else if (para->align == HK_TEXT_JUSTIFY) align = DWRITE_TEXT_ALIGNMENT_JUSTIFIED;
    layout->SetTextAlignment(align);
    layout->SetWordWrapping(para->wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (para->line_height > 0)
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, para->line_height, para->line_height * 0.8f);
    DWRITE_TEXT_RANGE all = {0, len};
    apply_style(layout, all, base);
    hk_text_layout *l = static_cast<hk_text_layout *>(hk_alloc_zero(sizeof *l));
    l->layout = layout;
    l->default_color = base->color;
    return l;
}

void hk_text_layout_set_style(hk_text_layout *l, uint32_t start, uint32_t len, const hk_text_style *s)
{
    DWRITE_TEXT_RANGE r = {start, len};
    apply_style(l->layout, r, s);
}

void hk_text_layout_size(hk_text_layout *l, float *width, float *height)
{
    DWRITE_TEXT_METRICS m = {};
    l->layout->GetMetrics(&m);
    *width = m.widthIncludingTrailingWhitespace;
    *height = m.height;
}

void hk_text_layout_intrinsic(hk_text_layout *l, float *min_width, float *max_width)
{
    FLOAT minw = 0;
    l->layout->DetermineMinWidth(&minw);
    float saved = l->layout->GetMaxWidth();
    l->layout->SetMaxWidth(1e7f);
    DWRITE_TEXT_METRICS m = {};
    l->layout->GetMetrics(&m);
    l->layout->SetMaxWidth(saved);
    *min_width = minw;
    *max_width = m.widthIncludingTrailingWhitespace;
}

void hk_text_layout_runs(hk_text_layout *l, void (*on_run)(void *, const hk_glyph_run *),
                         void (*on_rect)(void *, const hk_text_rect *), void *user)
{
    EmitCtx c = {on_run, on_rect, user, l->default_color};
    Renderer r(&c);
    l->layout->Draw(nullptr, &r, 0, 0);
}

void hk_text_layout_range_rects(hk_text_layout *l, uint32_t start, uint32_t len,
                                void (*on_rect)(void *, const hk_text_rect *), void *user)
{
    DWRITE_HIT_TEST_METRICS m[32];
    UINT32 count = 0;
    if (FAILED(l->layout->HitTestTextRange(start, len, 0, 0, m, 32, &count))) return;
    for (UINT32 i = 0; i < count && i < 32; i++) {
        hk_text_rect r = {m[i].left, m[i].top, m[i].width, m[i].height, 0, 0};
        on_rect(user, &r);
    }
}

void hk_text_layout_destroy(hk_text_layout *l)
{
    if (!l) return;
    l->layout->Release();
    hk_free(l, sizeof *l);
}

void hk_font_face_retain(void *face)
{
    static_cast<IDWriteFontFace *>(face)->AddRef();
}

void hk_font_face_release(void *face)
{
    static_cast<IDWriteFontFace *>(face)->Release();
}

bool hk_glyph_rasterize(void *face, float size, uint16_t glyph, float x_frac, hk_glyph_mask *out)
{
    if (!ready()) return false;
    DWRITE_GLYPH_RUN run = {};
    FLOAT advance = 0;
    DWRITE_GLYPH_OFFSET offset = {0, 0};
    run.fontFace = static_cast<IDWriteFontFace *>(face);
    run.fontEmSize = size;
    run.glyphCount = 1;
    run.glyphIndices = &glyph;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;
    IDWriteGlyphRunAnalysis *a = nullptr;
    if (FAILED(g_factory->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                 DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DEFAULT,
                                                 DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, x_frac, 0, &a)))
        return false;
    RECT b = {};
    bool ok = SUCCEEDED(a->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &b)) && b.right > b.left && b.bottom > b.top;
    if (ok) {
        int w = b.right - b.left, h = b.bottom - b.top;
        uint8_t *alpha = static_cast<uint8_t *>(hk_alloc((size_t)w * (size_t)h));
        if (SUCCEEDED(a->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &b, alpha, (UINT32)(w * h)))) {
            out->left = b.left;
            out->top = b.top;
            out->w = w;
            out->h = h;
            out->alpha = alpha;
        } else {
            hk_free(alpha, (size_t)w * (size_t)h);
            ok = false;
        }
    }
    a->Release();
    return ok;
}

} // extern "C"
