/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plat/hk_window.h"
#include "base/hk_arena.h"
#include "base/hk_log.h"
#include "base/hk_mem.h"
#include "base/hk_utf8.h"

#include <windows.h>
#include <dwmapi.h>

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
};

static const wchar_t k_window_class[] = L"HawkWindow";
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
    case WM_KEYDOWN: {
        hk_key key = HK_KEY_OTHER;
        switch (wp) {
        case VK_UP: key = HK_KEY_UP; break;
        case VK_DOWN: key = HK_KEY_DOWN; break;
        case VK_PRIOR: key = HK_KEY_PAGE_UP; break;
        case VK_NEXT: key = HK_KEY_PAGE_DOWN; break;
        case VK_HOME: key = HK_KEY_HOME; break;
        case VK_END: key = HK_KEY_END; break;
        case VK_SPACE: key = HK_KEY_SPACE; break;
        case VK_F5: key = HK_KEY_F5; break;
        case VK_ESCAPE: key = HK_KEY_ESCAPE; break;
        default: break;
        }
        unsigned mods = (GetKeyState(VK_SHIFT) < 0 ? HK_MOD_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? HK_MOD_CTRL : 0);
        if (key != HK_KEY_OTHER && w->cb.on_key) w->cb.on_key(w->cb.user, w, key, mods);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (w->cb.on_wheel) w->cb.on_wheel(w->cb.user, w, (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (w->cb.on_close) w->cb.on_close(w->cb.user, w);
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
    HWND hwnd = CreateWindowExW(0, k_window_class, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL,
                                GetModuleHandleW(NULL), w);
    hk_arena_free(&tmp);
    if (!hwnd) {
        HK_LOGE("CreateWindowExW failed (error %lu)", GetLastError());
        hk_free(w, sizeof *w);
        return HK_ERR_IO;
    }
    w->dpi = GetDpiForWindow(hwnd);
    apply_title_bar_theme(w);
    *out = w; /* callbacks during the first show may already use it */
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);
    return HK_OK;
}

void hk_window_set_title(hk_window *w, hk_str title)
{
    hk_arena tmp;
    hk_arena_init(&tmp, 1024);
    SetWindowTextW(w->hwnd, (const wchar_t *)hk_utf8_to_utf16(&tmp, title, NULL));
    hk_arena_free(&tmp);
}

void hk_window_client_size(const hk_window *w, int *width, int *height)
{
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
    w->dirty = true;
    InvalidateRect(w->hwnd, NULL, FALSE);
}

void hk_window_close(hk_window *w)
{
    PostMessageW(w->hwnd, WM_CLOSE, 0, 0);
}

float hk_window_scale(const hk_window *w)
{
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
