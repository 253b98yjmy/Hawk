/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_textview.h"
#include "base/hk_mem.h"
#include "base/hk_utf8.h"
#include "base/hk_vec.h"

#include <windows.h>

typedef struct para {
    hk_tv_kind kind;
    wchar_t *text;
    size_t len;
    int top, height;
} para;

struct hk_textview {
    hk_arena arena;
    HK_VEC(para) paras;
    int width, height, margin, column;
    float scale;
    HFONT fonts[HK_TV_NOTE + 1];
};

hk_textview *hk_textview_create(void)
{
    hk_textview *tv = hk_alloc_zero(sizeof *tv);
    hk_arena_init(&tv->arena, 0);
    return tv;
}

static void free_fonts(hk_textview *tv)
{
    for (int i = 0; i <= HK_TV_NOTE; i++) {
        if (tv->fonts[i]) DeleteObject(tv->fonts[i]);
        tv->fonts[i] = NULL;
    }
}

void hk_textview_destroy(hk_textview *tv)
{
    if (!tv) return;
    free_fonts(tv);
    hk_vec_free(&tv->paras);
    hk_arena_free(&tv->arena);
    hk_free(tv, sizeof *tv);
}

void hk_textview_clear(hk_textview *tv)
{
    tv->paras.len = 0;
    tv->height = 0;
    hk_arena_reset(&tv->arena);
}

void hk_textview_add(hk_textview *tv, hk_tv_kind kind, hk_str text)
{
    para p = {0};
    p.kind = kind;
    p.text = (wchar_t *)hk_utf8_to_utf16(&tv->arena, text, &p.len);
    hk_vec_push(&tv->paras, p);
}

static void make_fonts(hk_textview *tv)
{
    static const struct {
        int px, weight;
        const wchar_t *face;
    } k[] = {
        [HK_TV_BODY] = {16, FW_NORMAL, L"Yu Gothic UI"}, [HK_TV_H1] = {28, FW_BOLD, L"Yu Gothic UI"},
        [HK_TV_H2] = {22, FW_BOLD, L"Yu Gothic UI"},     [HK_TV_H3] = {18, FW_BOLD, L"Yu Gothic UI"},
        [HK_TV_PRE] = {14, FW_NORMAL, L"Consolas"},      [HK_TV_NOTE] = {15, FW_NORMAL, L"Yu Gothic UI"},
    };
    free_fonts(tv);
    for (int i = 0; i <= HK_TV_NOTE; i++) {
        tv->fonts[i] = CreateFontW(-(int)(k[i].px * tv->scale + 0.5f), 0, 0, 0, k[i].weight, i == HK_TV_NOTE, 0, 0,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, k[i].face);
    }
}

static UINT draw_flags(hk_tv_kind kind)
{
    UINT f = DT_NOPREFIX | DT_EXPANDTABS | DT_EDITCONTROL;
    if (kind != HK_TV_PRE) f |= DT_WORDBREAK;
    return f;
}

void hk_textview_layout(hk_textview *tv, int width, float scale)
{
    if (scale != tv->scale || !tv->fonts[0]) {
        tv->scale = scale;
        make_fonts(tv);
    }
    tv->width = width;
    tv->margin = (int)(24 * scale);
    tv->column = HK_MIN(width - 2 * tv->margin, (int)(860 * scale));
    if (tv->column < 50) tv->column = 50;
    HDC dc = CreateCompatibleDC(NULL);
    int y = tv->margin;
    for (uint32_t i = 0; i < tv->paras.len; i++) {
        para *p = &tv->paras.data[i];
        SelectObject(dc, tv->fonts[p->kind]);
        RECT r = {0, 0, tv->column, 0};
        DrawTextW(dc, p->text, (int)p->len, &r, draw_flags(p->kind) | DT_CALCRECT);
        int gap = (int)((p->kind == HK_TV_H1 || p->kind == HK_TV_H2 ? 18 : 10) * scale);
        if (p->kind == HK_TV_H1 || p->kind == HK_TV_H2) y += (int)(10 * scale);
        p->top = y;
        p->height = r.bottom - r.top;
        y += p->height + gap;
    }
    DeleteDC(dc);
    tv->height = y + tv->margin;
}

int hk_textview_height(const hk_textview *tv)
{
    return tv->height;
}

void hk_textview_paint(hk_textview *tv, hk_surface *s, int scroll_y, bool dark)
{
    HDC dc = s->native;
    COLORREF bg = dark ? RGB(0x1E, 0x1E, 0x1E) : RGB(0xFF, 0xFF, 0xFF);
    COLORREF fg = dark ? RGB(0xE6, 0xE6, 0xE6) : RGB(0x1A, 0x1A, 0x1A);
    COLORREF note = dark ? RGB(0x9A, 0x9A, 0x9A) : RGB(0x66, 0x66, 0x66);
    RECT all = {0, 0, s->width, s->height};
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dc, &all, brush);
    DeleteObject(brush);
    SetBkMode(dc, TRANSPARENT);
    int left = (s->width - tv->column) / 2;
    if (left < tv->margin) left = tv->margin;
    for (uint32_t i = 0; i < tv->paras.len; i++) {
        para *p = &tv->paras.data[i];
        int top = p->top - scroll_y;
        if (top + p->height < 0) continue;
        if (top > s->height) break;
        SelectObject(dc, tv->fonts[p->kind]);
        SetTextColor(dc, p->kind == HK_TV_NOTE ? note : fg);
        RECT r = {left, top, left + tv->column, top + p->height};
        DrawTextW(dc, p->text, (int)p->len, &r, draw_flags(p->kind));
    }
}
