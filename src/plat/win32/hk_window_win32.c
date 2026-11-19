/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_window.h"
#include "base/hk_arena.h"
#include "base/hk_log.h"
#include "base/hk_mem.h"
#include "base/hk_utf8.h"

#include <windows.h>
#include <dwmapi.h>
#include <imm.h>
#include <string.h>

#include "hk_win32_internal.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

struct hk_window {
    HWND hwnd;
    hk_window_callbacks cb;
    HDC mem_dc;
    HBITMAP dib;
    HGDIOBJ old_bitmap;
    hk_surface surface;
    UINT dpi;
    bool dirty;
    bool dark;
    bool presented;
    hk_cursor cursor;
    wchar_t high_surrogate; /* the first half of a character arriving in two WM_CHAR messages */
    bool text_input;
    /* the address bar */
    HWND edit;
    WNDPROC edit_proc;
    HFONT edit_font;
    UINT edit_font_dpi;
    HBRUSH edit_brush;
    COLORREF edit_bg, edit_fg;
    bool edit_fresh;   /* focused by a click that has not been released yet */
    bool edit_swallow; /* the character of a handled key follows */
};

static const wchar_t k_window_class[] = L"HawkWindow";

static LPCWSTR cursor_id(hk_cursor c)
{
    switch (c) {
    case HK_CURSOR_HAND: return IDC_HAND;
    case HK_CURSOR_WAIT: return IDC_APPSTARTING;
    case HK_CURSOR_TEXT: return IDC_IBEAM;
    default: return IDC_ARROW;
    }
}

unsigned hk_plat_key_mods(void)
{
    return (GetKeyState(VK_SHIFT) < 0 ? HK_MOD_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? HK_MOD_CTRL : 0) |
           (GetKeyState(VK_MENU) < 0 ? HK_MOD_ALT : 0);
}

static hk_key key_of(WPARAM wp, unsigned mods)
{
    switch (wp) {
    case VK_UP: return HK_KEY_UP;
    case VK_DOWN: return HK_KEY_DOWN;
    case VK_LEFT: return HK_KEY_LEFT;
    case VK_RIGHT: return HK_KEY_RIGHT;
    case VK_PRIOR: return HK_KEY_PAGE_UP;
    case VK_NEXT: return HK_KEY_PAGE_DOWN;
    case VK_HOME: return HK_KEY_HOME;
    case VK_END: return HK_KEY_END;
    case VK_SPACE: return HK_KEY_SPACE;
    case VK_F5: return HK_KEY_F5;
    case VK_F6: return HK_KEY_F6;
    case VK_ESCAPE: return HK_KEY_ESCAPE;
    case VK_BROWSER_BACK: return HK_KEY_BROWSER_BACK;
    case VK_BROWSER_FORWARD: return HK_KEY_BROWSER_FORWARD;
    case VK_TAB: return HK_KEY_TAB;
    case VK_RETURN: return HK_KEY_ENTER;
    case VK_BACK: return HK_KEY_BACKSPACE;
    case VK_DELETE: return HK_KEY_DELETE;
    default: break;
    }
    if (wp >= 'A' && wp <= 'Z' && (mods & (HK_MOD_CTRL | HK_MOD_ALT))) return (hk_key)(HK_KEY_A + (int)(wp - 'A'));
    if (wp >= '0' && wp <= '9' && (mods & (HK_MOD_CTRL | HK_MOD_ALT))) return (hk_key)(HK_KEY_0 + (int)(wp - '0'));
    return HK_KEY_OTHER;
}

/* Reads an IME string (GCS_COMPSTR or GCS_RESULTSTR) into a heap buffer; returns its length in UTF-16 units. */
static uint32_t ime_string(HIMC imc, DWORD which, wchar_t **out)
{
    LONG bytes = ImmGetCompositionStringW(imc, which, NULL, 0);
    *out = NULL;
    if (bytes <= 0) return 0;
    *out = hk_alloc((size_t)bytes + sizeof(wchar_t));
    ImmGetCompositionStringW(imc, which, *out, (DWORD)bytes);
    return (uint32_t)bytes / sizeof(wchar_t);
}
static int g_window_count;

/* --- helpers ----------------------------------------------------------------- */

static bool system_prefers_dark(void)
{
    DWORD light = 1, size = sizeof light;
    LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                              L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    return rc == ERROR_SUCCESS && light == 0;
}

static void apply_title_bar_theme(hk_window *w)
{
    BOOL dark = w->dark ? TRUE : FALSE;
    DwmSetWindowAttribute(w->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
}

static void release_back_buffer(hk_window *w)
{
    if (w->mem_dc) {
        SelectObject(w->mem_dc, w->old_bitmap);
        DeleteDC(w->mem_dc);
    }
    if (w->dib) DeleteObject(w->dib);
    w->mem_dc = NULL;
    w->dib = NULL;
    w->old_bitmap = NULL;
    w->surface = (hk_surface){0};
}

/* A minimized window has a 0x0 client area: the back buffer is released to save memory. */
static void resize_back_buffer(hk_window *w, int width, int height)
{
    if (w->dib && w->surface.width == width && w->surface.height == height) return;
    release_back_buffer(w);
    if (width <= 0 || height <= 0) return;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    w->dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!w->dib) {
        HK_LOGE("CreateDIBSection(%dx%d) failed (error %lu)", width, height, GetLastError());
        return;
    }
    w->mem_dc = CreateCompatibleDC(NULL);
    w->old_bitmap = SelectObject(w->mem_dc, w->dib);
    w->surface.pixels = bits;
    w->surface.width = width;
    w->surface.height = height;
    w->surface.stride = width;
    w->surface.native = w->mem_dc;
    w->dirty = true;
}

/* --- window procedure ---------------------------------------------------------- */

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        hk_window *nw = ((CREATESTRUCTW *)lp)->lpCreateParams;
        nw->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)nw);
        g_window_count++;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    hk_window *w = (hk_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE: {
        int width = LOWORD(lp), height = HIWORD(lp);
        resize_back_buffer(w, width, height);
        if (w->cb.on_resize) w->cb.on_resize(w->cb.user, w, width, height);
        w->dirty = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* the back buffer covers the whole client area */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (w->dib) {
            if (w->dirty && w->cb.on_paint) {
                w->cb.on_paint(w->cb.user, w, &w->surface);
                GdiFlush();
            }
            w->dirty = false;
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, w->mem_dc, ps.rcPaint.left, ps.rcPaint.top,
                   SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        if (!w->presented && w->dib) {
            w->presented = true;
            if (w->cb.on_first_present) w->cb.on_first_present(w->cb.user, w);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        w->dpi = HIWORD(wp);
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        w->dirty = true;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            bool dark = system_prefers_dark();
            if (dark != w->dark) {
                w->dark = dark;
                apply_title_bar_theme(w);
                if (w->cb.on_theme_changed) w->cb.on_theme_changed(w->cb.user, w, dark);
                hk_window_invalidate(w);
            }
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        unsigned mods = hk_plat_key_mods();
        hk_key key = key_of(wp, mods);
        if (wp == VK_PROCESSKEY) break; /* the IME handles this key */
        if (key != HK_KEY_OTHER && w->cb.on_key) {
            w->cb.on_key(w->cb.user, w, key, mods);
            return 0;
        }
        break; /* let Alt+F4 and friends reach DefWindowProc */
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        hk_mouse_event ev = msg == WM_MOUSEMOVE ? HK_MOUSE_MOVE : msg == WM_LBUTTONDOWN ? HK_MOUSE_DOWN : HK_MOUSE_UP;
        hk_mouse_button b = HK_BUTTON_NONE;
        if (msg == WM_LBUTTONDOWN) {
            SetCapture(hwnd);
            if (GetFocus() != hwnd) SetFocus(hwnd); /* from the address bar to the page */
        }
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) b = HK_BUTTON_LEFT;
        else if (msg == WM_MBUTTONUP) b = HK_BUTTON_MIDDLE;
        else if (msg == WM_XBUTTONUP) b = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? HK_BUTTON_BACK : HK_BUTTON_FORWARD;
        if (w->cb.on_mouse) w->cb.on_mouse(w->cb.user, w, ev, b, x, y);
        return msg == WM_XBUTTONUP ? TRUE : 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(NULL, cursor_id(w->cursor)));
            return TRUE;
        }
        break;
    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if (c >= 0xD800 && c < 0xDC00) {
            w->high_surrogate = c;
            return 0;
        }
        uint32_t cp = c;
        if (c >= 0xDC00 && c < 0xE000) {
            if (!w->high_surrogate) return 0;
            cp = 0x10000 + (((uint32_t)w->high_surrogate - 0xD800) << 10) + (c - 0xDC00);
        }
        w->high_surrogate = 0;
        if (cp >= 0x20 && cp != 0x7F && w->cb.on_char) w->cb.on_char(w->cb.user, w, cp);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        if ((HWND)lp == w->edit) {
            SetTextColor((HDC)wp, w->edit_fg);
            SetBkColor((HDC)wp, w->edit_bg);
            return (LRESULT)w->edit_brush;
        }
        break;
    case WM_IME_SETCONTEXT:
        /* The composition is drawn inline by Hawk; the IME draws only its candidate window. */
        lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;
        break;
    case WM_IME_STARTCOMPOSITION:
        return 0;
    case WM_IME_COMPOSITION: {
        HIMC imc = ImmGetContext(hwnd);
        if (!imc) break;
        if (lp & GCS_RESULTSTR) {
            wchar_t *s;
            uint32_t n = ime_string(imc, GCS_RESULTSTR, &s);
            if (w->cb.on_composition) w->cb.on_composition(w->cb.user, w, NULL, 0, 0);
            for (uint32_t i = 0; i < n && w->cb.on_char; i++) {
                uint32_t cp = s[i];
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)s[i + 1] - 0xDC00);
                    i++;
                }
                if (cp >= 0x20 || cp == '\n') w->cb.on_char(w->cb.user, w, cp);
            }
            if (s) hk_free(s, (size_t)n * sizeof(wchar_t) + sizeof(wchar_t));
        }
        if (lp & GCS_COMPSTR) {
            wchar_t *s;
            uint32_t n = ime_string(imc, GCS_COMPSTR, &s);
            LONG cursor = (lp & GCS_CURSORPOS) ? ImmGetCompositionStringW(imc, GCS_CURSORPOS, NULL, 0) : (LONG)n;
            if (cursor < 0 || (uint32_t)cursor > n) cursor = (LONG)n;
            if (w->cb.on_composition) w->cb.on_composition(w->cb.user, w, (const uint16_t *)s, n, (uint32_t)cursor);
            if (s) hk_free(s, (size_t)n * sizeof(wchar_t) + sizeof(wchar_t));
        }
        ImmReleaseContext(hwnd, imc);
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        if (w->cb.on_composition) w->cb.on_composition(w->cb.user, w, NULL, 0, 0);
        return 0;
    case WM_MOUSEWHEEL:
        if (w->cb.on_wheel) w->cb.on_wheel(w->cb.user, w, (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (w->cb.on_close) w->cb.on_close(w->cb.user, w);
        if (w->edit) {
            /* Children are destroyed after this message: unhook the address bar before w goes away. */
            SetWindowLongPtrW(w->edit, GWLP_WNDPROC, (LONG_PTR)w->edit_proc);
            SetWindowLongPtrW(w->edit, GWLP_USERDATA, 0);
            DestroyWindow(w->edit);
            w->edit = NULL;
        }
        if (w->edit_font) DeleteObject(w->edit_font);
        if (w->edit_brush) DeleteObject(w->edit_brush);
        release_back_buffer(w);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hk_free(w, sizeof *w);
        if (--g_window_count == 0) PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --- public API ---------------------------------------------------------------- */

static bool register_class(void)
{
    static bool registered;
    if (registered) return true;
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = k_window_class;
    registered = RegisterClassExW(&wc) != 0;
    if (!registered) HK_LOGE("RegisterClassExW failed (error %lu)", GetLastError());
    return registered;
}

hk_status hk_window_create(const hk_window_desc *desc, hk_window **out)
{
    *out = NULL;
    if (!register_class()) return HK_ERR_IO;

    hk_window *w = hk_alloc_zero(sizeof *w);
    w->cb = desc->cb;
    w->dark = system_prefers_dark();
    w->dpi = GetDpiForSystem();

    RECT r = {0, 0, MulDiv(desc->width, (int)w->dpi, 96), MulDiv(desc->height, (int)w->dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, w->dpi);

    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    const wchar_t *title = (const wchar_t *)hk_utf8_to_utf16(&tmp, desc->title, NULL);

    /* Created hidden so the title bar theme is set before the first frame is shown. */
    HWND hwnd = CreateWindowExW(0, k_window_class, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
                                GetModuleHandleW(NULL), w);
    hk_arena_free(&tmp);
    if (!hwnd) {
        HK_LOGE("CreateWindowExW failed (error %lu)", GetLastError());
        hk_free(w, sizeof *w);
        return HK_ERR_IO;
    }
    w->dpi = GetDpiForWindow(hwnd);
    ImmAssociateContextEx(hwnd, NULL, 0); /* no IME until a text field has the focus */
    apply_title_bar_theme(w);
    *out = w; /* callbacks during the first show may already use it */
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    return HK_OK;
}

void hk_window_set_title(hk_window *w, hk_str title)
{
    if (!w) return; /* the window may already be gone (callbacks during shutdown) */
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    SetWindowTextW(w->hwnd, (const wchar_t *)hk_utf8_to_utf16(&tmp, title, NULL));
    hk_arena_free(&tmp);
}

void hk_window_set_cursor(hk_window *w, hk_cursor cursor)
{
    if (!w) return; /* the window may already be gone (callbacks during shutdown) */
    if (w->cursor == cursor) return;
    w->cursor = cursor;
    SetCursor(LoadCursorW(NULL, cursor_id(cursor)));
}

/* --- the address bar ---------------------------------------------------------------------------- */

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    hk_window *w = (hk_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        unsigned mods = hk_plat_key_mods();
        hk_key key = key_of(wp, mods);
        bool offer = key == HK_KEY_ENTER || key == HK_KEY_ESCAPE || key == HK_KEY_TAB || key == HK_KEY_F5 ||
                     key == HK_KEY_F6 || key == HK_KEY_BROWSER_BACK || key == HK_KEY_BROWSER_FORWARD ||
                     (key >= HK_KEY_A && key <= HK_KEY_9) ||
                     ((mods & HK_MOD_ALT) && (key == HK_KEY_LEFT || key == HK_KEY_RIGHT));
        if (offer && w->cb.on_address_key && w->cb.on_address_key(w->cb.user, w, key, mods)) {
            w->edit_swallow = true;
            return 0;
        }
        if (key == HK_KEY_BACKSPACE && (mods & HK_MOD_CTRL)) {
            /* Ctrl+Backspace: delete the previous word (the edit control would insert a box). */
            DWORD start = 0, end = 0;
            SendMessageW(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            if (start == end && start > 0) {
                int n = GetWindowTextLengthW(hwnd);
                wchar_t *buf = hk_alloc(((size_t)n + 1) * sizeof(wchar_t));
                GetWindowTextW(hwnd, buf, n + 1);
                DWORD p = start;
                while (p > 0 && buf[p - 1] == ' ') p--;
                while (p > 0 && buf[p - 1] != ' ' && buf[p - 1] != '/' && buf[p - 1] != '.') p--;
                if (p == start) p--;
                hk_free(buf, ((size_t)n + 1) * sizeof(wchar_t));
                SendMessageW(hwnd, EM_SETSEL, p, start);
            }
            SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)L"");
            w->edit_swallow = true;
            return 0;
        }
        if (key == HK_KEY_LETTER('A') && mods == HK_MOD_CTRL) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            w->edit_swallow = true;
            return 0;
        }
        break;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        if (w->edit_swallow || wp == '\r' || wp == '\n' || wp == '\t' || wp == 0x1B || wp == 0x7F) {
            w->edit_swallow = false;
            return 0;
        }
        break;
    case WM_KEYUP:
        w->edit_swallow = false;
        break;
    case WM_LBUTTONDOWN:
        if (GetFocus() != hwnd) w->edit_fresh = true;
        break;
    case WM_LBUTTONUP: {
        LRESULT r = CallWindowProcW(w->edit_proc, hwnd, msg, wp, lp);
        DWORD start = 0, end = 0;
        SendMessageW(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        if (w->edit_fresh && start == end) SendMessageW(hwnd, EM_SETSEL, 0, -1); /* a click selects all */
        w->edit_fresh = false;
        return r;
    }
    default: break;
    }
    return CallWindowProcW(w->edit_proc, hwnd, msg, wp, lp);
}

static COLORREF colorref(uint32_t argb)
{
    return RGB((argb >> 16) & 255, (argb >> 8) & 255, argb & 255);
}

void hk_window_address_place(hk_window *w, int x, int y, int width, int height, uint32_t background, uint32_t text)
{
    if (!w) return;
    if (!w->edit) {
        w->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, w->hwnd, NULL,
                                  GetModuleHandleW(NULL), NULL);
        if (!w->edit) return;
        SetWindowLongPtrW(w->edit, GWLP_USERDATA, (LONG_PTR)w);
        w->edit_proc = (WNDPROC)SetWindowLongPtrW(w->edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        SendMessageW(w->edit, EM_SETLIMITTEXT, 8192, 0);
    }
    if (!w->edit_font || w->edit_font_dpi != w->dpi) {
        if (w->edit) {
            /* Children are destroyed after this message: unhook the address bar before w goes away. */
            SetWindowLongPtrW(w->edit, GWLP_WNDPROC, (LONG_PTR)w->edit_proc);
            SetWindowLongPtrW(w->edit, GWLP_USERDATA, 0);
            DestroyWindow(w->edit);
            w->edit = NULL;
        }
        if (w->edit_font) DeleteObject(w->edit_font);
        w->edit_font = CreateFontW(-MulDiv(14, (int)w->dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        w->edit_font_dpi = w->dpi;
        SendMessageW(w->edit, WM_SETFONT, (WPARAM)w->edit_font, TRUE);
    }
    COLORREF bg = colorref(background), fg = colorref(text);
    if (!w->edit_brush || bg != w->edit_bg) {
        if (w->edit_brush) DeleteObject(w->edit_brush);
        w->edit_brush = CreateSolidBrush(bg);
        w->edit_bg = bg;
        InvalidateRect(w->edit, NULL, TRUE);
    }
    if (fg != w->edit_fg) {
        w->edit_fg = fg;
        InvalidateRect(w->edit, NULL, TRUE);
    }
    /* The edit control draws its text at the top: center a box one line high. */
    HDC dc = GetDC(w->edit);
    HGDIOBJ old = SelectObject(dc, w->edit_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(w->edit, dc);
    int line = tm.tmHeight + 2, pad = MulDiv(10, (int)w->dpi, 96);
    if (line > height) line = height;
    SetWindowPos(w->edit, NULL, x + pad, y + (height - line) / 2, width - 2 * pad > 10 ? width - 2 * pad : 10, line,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void hk_window_address_set_text(hk_window *w, hk_str text)
{
    if (!w || !w->edit) return;
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    SetWindowTextW(w->edit, (const wchar_t *)hk_utf8_to_utf16(&tmp, text, NULL));
    hk_arena_free(&tmp);
}

void hk_window_address_get_text(hk_window *w, hk_charbuf *out)
{
    if (!w || !w->edit) return;
    int n = GetWindowTextLengthW(w->edit);
    wchar_t *buf = hk_alloc(((size_t)n + 1) * sizeof(wchar_t));
    int got = GetWindowTextW(w->edit, buf, n + 1);
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    hk_str s = hk_utf16_to_utf8(&tmp, (const uint16_t *)buf, (size_t)got);
    hk_charbuf_append(out, s.p, s.n);
    hk_arena_free(&tmp);
    hk_free(buf, ((size_t)n + 1) * sizeof(wchar_t));
}

void hk_window_address_focus(hk_window *w)
{
    if (!w || !w->edit) return;
    SetFocus(w->edit);
    SendMessageW(w->edit, EM_SETSEL, 0, -1);
}

void hk_window_focus_page(hk_window *w)
{
    if (w) SetFocus(w->hwnd);
}

bool hk_window_address_has_focus(const hk_window *w)
{
    return w && w->edit && GetFocus() == w->edit;
}

void hk_window_set_text_input(hk_window *w, bool enabled, int x, int y, int h)
{
    if (!w) return; /* the window may already be gone (callbacks during shutdown) */
    if (enabled != w->text_input) {
        ImmAssociateContextEx(w->hwnd, NULL, enabled ? IACE_DEFAULT : 0);
        w->text_input = enabled;
    }
    if (!enabled) return;
    HIMC imc = ImmGetContext(w->hwnd);
    if (!imc) return;
    COMPOSITIONFORM cf = {0};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = x;
    cf.ptCurrentPos.y = y;
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand = {0};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_EXCLUDE;
    cand.ptCurrentPos.x = x;
    cand.ptCurrentPos.y = y + h;
    cand.rcArea.left = x;
    cand.rcArea.top = y;
    cand.rcArea.right = x + 1;
    cand.rcArea.bottom = y + h;
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(w->hwnd, imc);
}

int hk_window_popup_menu(hk_window *w, const hk_str *items, const uint8_t *flags, uint32_t n, int x, int y)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return -1;
    hk_arena tmp;
    hk_arena_init(&tmp, 4096);
    for (uint32_t i = 0; i < n && i < 4000; i++) {
        UINT f = MF_STRING;
        if (flags && (flags[i] & 1)) f |= MF_GRAYED;
        if (flags && (flags[i] & 2)) f |= MF_CHECKED;
        /* Long menus wrap into columns. */
        if (i && i % 40 == 0) f |= MF_MENUBARBREAK;
        const wchar_t *label = (const wchar_t *)hk_utf8_to_utf16(&tmp, items[i].n ? items[i] : HK_STR(" "), NULL);
        AppendMenuW(menu, f, (UINT_PTR)i + 1, label);
    }
    POINT pt = {x, y};
    ClientToScreen(w->hwnd, &pt);
    int choice = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, pt.x, pt.y,
                                       w->hwnd, NULL);
    DestroyMenu(menu);
    hk_arena_free(&tmp);
    return choice - 1;
}

void hk_plat_clipboard_set(hk_str text)
{
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    size_t n;
    const uint16_t *w16 = hk_utf8_to_utf16(&tmp, text, &n);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
    if (mem) {
        void *p = GlobalLock(mem);
        memcpy(p, w16, (n + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (OpenClipboard(NULL)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            CloseClipboard();
        } else {
            GlobalFree(mem);
        }
    }
    hk_arena_free(&tmp);
}

bool hk_plat_clipboard_get(hk_charbuf *out)
{
    if (!OpenClipboard(NULL)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *p = h ? GlobalLock(h) : NULL;
    if (p) {
        size_t n = wcslen(p);
        hk_arena tmp;
        hk_arena_init(&tmp, 1024);
        hk_str s = hk_utf16_to_utf8(&tmp, (const uint16_t *)p, n);
        hk_charbuf_append(out, s.p, s.n);
        hk_arena_free(&tmp);
        GlobalUnlock(h);
        ok = true;
    }
    CloseClipboard();
    return ok;
}

void hk_window_client_size(const hk_window *w, int *width, int *height)
{
    if (!w) {
        *width = *height = 0;
        return;
    }
    RECT r;
    GetClientRect(w->hwnd, &r);
    *width = r.right - r.left;
    *height = r.bottom - r.top;
}

hk_status hk_window_save_bmp(hk_window *w, const char *path)
{
    if (!w->dib) return HK_ERR_INVALID;
    if (w->cb.on_paint) w->cb.on_paint(w->cb.user, w, &w->surface);
    GdiFlush();
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    HANDLE f = CreateFileW((const wchar_t *)hk_utf8_to_utf16(&tmp, hk_str_from_cstr(path), NULL), GENERIC_WRITE, 0,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    hk_arena_free(&tmp);
    if (f == INVALID_HANDLE_VALUE) return HK_ERR_IO;
    DWORD bytes = (DWORD)w->surface.width * (DWORD)w->surface.height * 4;
    BITMAPFILEHEADER fh = {0};
    BITMAPINFOHEADER ih = {0};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + bytes;
    ih.biSize = sizeof ih;
    ih.biWidth = w->surface.width;
    ih.biHeight = -w->surface.height;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    DWORD written;
    bool ok = WriteFile(f, &fh, sizeof fh, &written, NULL) && WriteFile(f, &ih, sizeof ih, &written, NULL) &&
              WriteFile(f, w->surface.pixels, bytes, &written, NULL);
    CloseHandle(f);
    return ok ? HK_OK : HK_ERR_IO;
}

void hk_window_invalidate(hk_window *w)
{
    if (!w) return; /* the window may already be gone (callbacks during shutdown) */
    w->dirty = true;
    InvalidateRect(w->hwnd, NULL, FALSE);
}

void hk_window_close(hk_window *w)
{
    if (!w) return; /* the window may already be gone (callbacks during shutdown) */
    PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
}

float hk_window_scale(const hk_window *w)
{
    if (!w) return 1;
    return (float)w->dpi / 96.0f;
}

bool hk_window_is_dark(const hk_window *w)
{
    return w->dark;
}

int hk_plat_run_loop(void)
{
    MSG msg;
    BOOL rc;
    while ((rc = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (rc == -1) {
            HK_LOGE("GetMessageW failed (error %lu)", GetLastError());
            return 1;
        }
        if (msg.hwnd == NULL && msg.message == HK_WM_TASKS) {
            hk_win32_run_main_tasks();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
