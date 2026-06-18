#include "Overlay.h"
#include <cstdio>
#include <cstring>

static HWND g_overlayWnd = nullptr;
static bool g_overlayVisible = false;
static int g_overlayCorner = 0;
static int g_overlaySize = 14;
static int g_overlayAlpha = 200;
static COLORREF g_overlayColor = RGB(0xe6, 0x4a, 0x4a);
static bool g_overlayShowFps = false;
static int g_overlayFpsValue = 0;

static const wchar_t* OVERLAY_CLASS = L"OpenReplayOverlay";

static int overlayWidth() {
    return g_overlayShowFps ? g_overlaySize + 42 : g_overlaySize;
}

static void updateOverlayPosition() {
    if (!g_overlayWnd || !g_overlayVisible) return;
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int w = overlayWidth();
    int x, y;
    switch (g_overlayCorner) {
        case 0: x = work.right - w - 10; y = work.top + 10; break;
        case 1: x = work.left + 10; y = work.top + 10; break;
        case 2: x = work.right - w - 10; y = work.bottom - g_overlaySize - 10; break;
        case 3: x = work.left + 10; y = work.bottom - g_overlaySize - 10; break;
        default: x = work.right - w - 10; y = work.top + 10;
    }
    SetWindowPos(g_overlayWnd, HWND_TOPMOST,
                 x, y, w, g_overlaySize, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

static void updateOverlayRegion() {
    if (!g_overlayWnd) return;
    int w = overlayWidth();
    HRGN rgn;
    if (g_overlayShowFps) {
        rgn = CreateRectRgn(0, 0, w, g_overlaySize);
    } else {
        rgn = CreateEllipticRgn(0, 0, g_overlaySize, g_overlaySize);
    }
    SetWindowRgn(g_overlayWnd, rgn, TRUE);
    DeleteObject(rgn);
}

static LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);

        HBRUSH br = CreateSolidBrush(g_overlayColor);
        HBRUSH oldBr = (HBRUSH)SelectObject(dc, br);
        SelectObject(dc, GetStockObject(NULL_PEN));

        if (g_overlayShowFps) {
            // Dot on the left
            Ellipse(dc, 0, 0, g_overlaySize, g_overlaySize);
            SelectObject(dc, oldBr);
            DeleteObject(br);

            // FPS text to the right
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HFONT hf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT oldF = (HFONT)SelectObject(dc, hf);
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "%d", g_overlayFpsValue);
            if (len > 0 && len < (int)sizeof(buf))
                TextOutA(dc, g_overlaySize + 4, (rc.bottom - 16) / 2, buf, len);
            SelectObject(dc, oldF);
        } else {
            Ellipse(dc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(dc, oldBr);
            DeleteObject(br);
        }

        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void overlayCreate() {
    if (g_overlayWnd) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = OVERLAY_CLASS;
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    g_overlayWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        OVERLAY_CLASS, L"",
        WS_POPUP,
        0, 0, g_overlaySize, g_overlaySize,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (g_overlayWnd) {
        updateOverlayRegion();
        SetLayeredWindowAttributes(g_overlayWnd, 0, g_overlayAlpha, LWA_ALPHA);
    }
}

void overlayDestroy() {
    if (g_overlayWnd) {
        DestroyWindow(g_overlayWnd);
        g_overlayWnd = nullptr;
    }
    g_overlayVisible = false;
}

void overlayShow(bool show) {
    if (!g_overlayWnd) return;
    if (show == g_overlayVisible) return;
    g_overlayVisible = show;
    if (show) {
        updateOverlayPosition();
        InvalidateRect(g_overlayWnd, nullptr, TRUE);
    } else {
        ShowWindow(g_overlayWnd, SW_HIDE);
    }
}

void overlaySetCorner(int corner) { g_overlayCorner = corner; updateOverlayPosition(); }
void overlaySetSize(int size) {
    g_overlaySize = size;
    if (g_overlayWnd) {
        updateOverlayRegion();
        SetWindowPos(g_overlayWnd, nullptr, 0, 0, overlayWidth(), size, SWP_NOMOVE | SWP_NOZORDER);
        SetLayeredWindowAttributes(g_overlayWnd, 0, g_overlayAlpha, LWA_ALPHA);
    }
    updateOverlayPosition();
}
void overlaySetAlpha(int alpha) { g_overlayAlpha = alpha; if (g_overlayWnd) SetLayeredWindowAttributes(g_overlayWnd, 0, alpha, LWA_ALPHA); }
void overlaySetColor(COLORREF color) { g_overlayColor = color; if (g_overlayVisible) InvalidateRect(g_overlayWnd, nullptr, TRUE); }
void overlaySetShowFps(bool show) {
    g_overlayShowFps = show;
    if (g_overlayWnd) {
        updateOverlayRegion();
        SetWindowPos(g_overlayWnd, nullptr, 0, 0, overlayWidth(), g_overlaySize, SWP_NOMOVE | SWP_NOZORDER);
    }
    updateOverlayPosition();
    if (g_overlayVisible) InvalidateRect(g_overlayWnd, nullptr, TRUE);
}
void overlaySetFpsValue(int fps) {
    g_overlayFpsValue = fps;
    if (g_overlayVisible && g_overlayShowFps)
        InvalidateRect(g_overlayWnd, nullptr, TRUE);
}

int overlayCorner() { return g_overlayCorner; }
int overlaySize() { return g_overlaySize; }
int overlayAlpha() { return g_overlayAlpha; }
COLORREF overlayColor() { return g_overlayColor; }
bool overlayShowFps() { return g_overlayShowFps; }
