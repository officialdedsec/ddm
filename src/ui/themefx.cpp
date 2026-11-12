#include "themefx.hpp"
#include "theme.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ddm {
namespace ui {

namespace {
const wchar_t* kVeil = L"DDMThemeVeil";

// One DDM window's old look, laid over it while the new look spreads out underneath
struct Veil {
    HWND target = nullptr, wnd = nullptr;
    RECT r{};
    int w = 0, h = 0;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    std::vector<uint32_t> was;   // the old look, opaque
};

BOOL CALLBACK collect(HWND w, LPARAM p) {
    if (!IsWindowVisible(w) || IsIconic(w)) return TRUE;
    wchar_t c[64] = {};
    GetClassNameW(w, c, 64);
    if (!wcscmp(c, L"tooltips_class32") || !wcscmp(c, L"#32768") || !wcscmp(c, kVeil)) return TRUE;
    if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_LAYERED) return TRUE;   // (fading panels: they change on their own)
    RECT r{};
    GetWindowRect(w, &r);
    if (r.right - r.left < 8 || r.bottom - r.top < 8) return TRUE;
    reinterpret_cast<std::vector<HWND>*>(p)->push_back(w);
    return TRUE;
}

bool animationsOn() {
    BOOL on = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return true;
    return on != FALSE;
}

void freeVeil(Veil& v) {
    if (v.wnd) DestroyWindow(v.wnd);
    if (v.dc) { SelectObject(v.dc, v.old); DeleteDC(v.dc); }
    if (v.bmp) DeleteObject(v.bmp);
    v = Veil();
}

bool makeVeil(HWND target, HDC screen, Veil& v) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kVeil;
        registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    RECT r{};
    GetWindowRect(target, &r);
    RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!IntersectRect(&r, &r, &vs)) return false;   // (off every screen)
    v.target = target;
    v.r = r;
    v.w = r.right - r.left;
    v.h = r.bottom - r.top;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = v.w;
    bi.bmiHeader.biHeight = -v.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    v.bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!v.bmp) return false;
    v.bits = static_cast<uint32_t*>(bits);
    v.dc = CreateCompatibleDC(screen);
    v.old = SelectObject(v.dc, v.bmp);
    BitBlt(v.dc, 0, 0, v.w, v.h, screen, r.left, r.top, SRCCOPY);   // what's on the screen right now
    GdiFlush();
    size_t n = static_cast<size_t>(v.w) * v.h;
    v.was.assign(v.bits, v.bits + n);
    for (auto& px : v.was) px |= 0xFF000000u;
    memcpy(v.bits, v.was.data(), n * 4);
    v.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kVeil, L"", WS_POPUP,
                            r.left, r.top, v.w, v.h, target, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!v.wnd) return false;
    POINT src{0, 0}, at{r.left, r.top};
    SIZE sz{v.w, v.h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(v.wnd, screen, &at, &sz, v.dc, &src, 0, &bf, ULW_ALPHA)) return false;
    SetWindowPos(v.wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

// The old look stays outside a circle of radius R around (ox, oy) and fades away across its soft edge
void paintVeil(Veil& v, HDC screen, double ox, double oy, double R, double band) {
    const double inner = R - band;
    const double R2 = R * R, in2 = inner > 0 ? inner * inner : -1;
    const size_t rowBytes = static_cast<size_t>(v.w) * 4;
    for (int y = 0; y < v.h; y++) {
        uint32_t* row = v.bits + static_cast<size_t>(y) * v.w;
        const uint32_t* src = v.was.data() + static_cast<size_t>(y) * v.w;
        memcpy(row, src, rowBytes);
        double dy = v.r.top + y + 0.5 - oy, dy2 = dy * dy;
        if (R <= 0 || dy2 >= R2) continue;   // all of this row still shows the old look
        double xo = std::sqrt(R2 - dy2);
        int x0 = std::max(0, static_cast<int>(std::floor(ox - xo - v.r.left)));
        int x1 = std::min(v.w, static_cast<int>(std::ceil(ox + xo - v.r.left)) + 1);
        int i0 = x1, i1 = x1;   // fully inside: nothing of the old look
        if (in2 > 0 && dy2 < in2) {
            double xi = std::sqrt(in2 - dy2);
            i0 = std::max(x0, static_cast<int>(std::ceil(ox - xi - v.r.left)));
            i1 = std::min(x1, static_cast<int>(std::floor(ox + xi - v.r.left)));
            if (i1 > i0) memset(row + i0, 0, static_cast<size_t>(i1 - i0) * 4);
            else i0 = i1 = x1;
        }
        for (int x = x0; x < x1; x++) {
            if (x >= i0 && x < i1) { x = i1 - 1; continue; }
            double dx = v.r.left + x + 0.5 - ox;
            double d = std::sqrt(dx * dx + dy2);
            double a = (d - inner) / band;   // 0 inside … 1 outside
            if (a >= 1) continue;
            if (a <= 0) { row[x] = 0; continue; }
            a = a * a * (3 - 2 * a);   // a smooth edge
            uint32_t p = src[x];
            unsigned k = static_cast<unsigned>(a * 255 + 0.5);
            unsigned b = (p & 0xFF) * k / 255, g = ((p >> 8) & 0xFF) * k / 255, rr = ((p >> 16) & 0xFF) * k / 255;
            row[x] = (k << 24) | (rr << 16) | (g << 8) | b;   // (premultiplied, as layered windows want)
        }
    }
    POINT srcPt{0, 0}, at{v.r.left, v.r.top};
    SIZE sz{v.w, v.h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(v.wnd, screen, &at, &sz, v.dc, &srcPt, 0, &bf, ULW_ALPHA);
}

void pump(DWORD ms) {
    MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { PostQuitMessage(static_cast<int>(m.wParam)); return; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
} // namespace

void themeTransition(const std::function<void()>& change) {
    static bool busy = false;
    if (busy || !animationsOn()) { change(); return; }
    busy = true;
    bool darkBefore = isDark();
    std::vector<HWND> wins;
    EnumThreadWindows(GetCurrentThreadId(), collect, reinterpret_cast<LPARAM>(&wins));
    HDC screen = GetDC(nullptr);
    std::vector<Veil> veils;
    for (HWND w : wins) {
        Veil v;
        if (makeVeil(w, screen, v)) veils.push_back(v);
        else freeVeil(v);
    }
    change();   // the new look, painted underneath right away
    for (HWND w : wins) if (IsWindow(w)) RedrawWindow(w, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (!veils.empty() && isDark() != darkBefore) {
        // it starts where you clicked (the theme button), else in the middle of the main window
        POINT o{};
        GetCursorPos(&o);
        bool over = false;
        for (auto& v : veils) if (PtInRect(&v.r, o)) over = true;
        if (!over) { o.x = (veils[0].r.left + veils[0].r.right) / 2; o.y = (veils[0].r.top + veils[0].r.bottom) / 2; }
        double reach = 0;
        for (auto& v : veils) {
            for (int c = 0; c < 4; c++) {
                double cx = (c & 1) ? v.r.right : v.r.left, cy = (c & 2) ? v.r.bottom : v.r.top;
                reach = std::max(reach, std::hypot(cx - o.x, cy - o.y));
            }
        }
        double band = Sf(70);
        DWORD dur = reach > Sf(1400) ? 720 : 600;
        if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_SLOWFX", nullptr, 0)) dur *= 10;   // (tests: slow enough to photograph)
        DWORD t0 = GetTickCount();
        while (true) {
            double t = std::min(1.0, (GetTickCount() - t0) / static_cast<double>(dur));
            double e = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;   // eases in and out
            double R = e * (reach + band + 2);
            for (auto& v : veils) if (IsWindow(v.wnd)) paintVeil(v, screen, o.x, o.y, R, band);
            if (t >= 1) break;
            pump(12);
        }
    }
    for (auto& v : veils) freeVeil(v);
    ReleaseDC(nullptr, screen);
    busy = false;
}

} // namespace ui
} // namespace ddm
