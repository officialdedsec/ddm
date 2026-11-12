// The dust effect: see dust.hpp. Everything heavy happens on the effect's own thread, on its own layered window.
#include "dust.hpp"
#include "theme.hpp"
#include "../core/settings.hpp"
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
#ifndef SPI_GETCLIENTAREAANIMATION
#define SPI_GETCLIENTAREAANIMATION 0x1042
#endif

namespace ddm {
namespace ui {
namespace dust {

namespace {

std::atomic<int> g_running{0};      // effects playing now (a few at most: more just close normally)
const UINT_PTR kTimerTransitions = 0xD057;

struct Grain {
    int16_t sx, sy;                  // where it comes from in the picture
    float delay, life, vx, vy, lift, wob, ph, freq;
};

struct Job {
    std::vector<uint32_t> img;       // BGRA, straight alpha
    int iw = 0, ih = 0;
    POINT at{0, 0};
    int dur = 1100;
    float scale = 1.0f;
    HANDLE ready = nullptr;          // set once the dust is on screen
};

LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

const wchar_t* overlayClass() {
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = overlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DDMDust";
        RegisterClassExW(&wc);
    });
    return L"DDMDust";
}

bool composition() { BOOL on = FALSE; return SUCCEEDED(DwmIsCompositionEnabled(&on)) && on; }

void play(Job* j) {
    const int iw = j->iw, ih = j->ih;
    const float sc = j->scale, dur = static_cast<float>(j->dur);
    // room around the picture for the dust to drift into (up and to the right)
    const int L = static_cast<int>(14 * sc), T = static_cast<int>(210 * sc), R = static_cast<int>(240 * sc), B = static_cast<int>(14 * sc);
    const int ow = iw + L + R, oh = ih + T + B;
    // the grains: fine on small pictures, a little coarser on big windows so every PC keeps up
    int g = std::max(2, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(iw) * ih / 60000.0))));
    std::mt19937 rng(GetTickCount() ^ static_cast<unsigned>(reinterpret_cast<uintptr_t>(j)));
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    std::vector<Grain> grains;
    grains.reserve(static_cast<size_t>((iw / g + 1) * (ih / g + 1)));
    float end = 0;
    for (int y = 0; y < ih; y += g) {
        for (int x = 0; x < iw; x += g) {
            int cx = std::min(iw - 1, x + g / 2), cy = std::min(ih - 1, y + g / 2);
            if ((j->img[static_cast<size_t>(cy) * iw + cx] >> 24) == 0 && (j->img[static_cast<size_t>(y) * iw + x] >> 24) == 0) continue;
            Grain p;
            p.sx = static_cast<int16_t>(x); p.sy = static_cast<int16_t>(y);
            p.delay = static_cast<float>(x) / std::max(1, iw) * dur * 0.42f + U(rng) * dur * 0.12f;
            p.life = dur * (0.38f + U(rng) * 0.32f);
            p.vx = (50.0f + U(rng) * 170.0f) * sc;
            p.vy = -(25.0f + U(rng) * 130.0f) * sc;
            p.lift = -(30.0f + U(rng) * 60.0f) * sc;
            p.wob = (2.0f + U(rng) * 6.0f) * sc;
            p.ph = U(rng) * 6.283f;
            p.freq = 6.0f + U(rng) * 8.0f;
            end = std::max(end, p.delay + p.life);
            grains.push_back(p);
        }
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = ow;
    bi.bmiHeader.biHeight = -oh;     // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HWND w = nullptr;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, overlayClass(), L"", WS_POPUP,
                            j->at.x - L, j->at.y - T, ow, oh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        uint32_t* out = static_cast<uint32_t*>(bits);
        const size_t total = static_cast<size_t>(ow) * oh;
        LARGE_INTEGER f, t0, now;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        bool shown = false;
        const bool dwm = composition();
        while (w) {
            QueryPerformanceCounter(&now);
            float el = static_cast<float>((now.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
            if (!shown) el = 0;   // the first frame is the picture itself, exactly where the window was
            std::memset(out, 0, total * 4);
            for (const Grain& p : grains) {
                float t = (el - p.delay) / p.life;
                if (t >= 1.0f) continue;
                int px = p.sx + L, py = p.sy + T, size = g;
                unsigned fa = 255;
                if (t > 0) {
                    float s = t * p.life / 1000.0f;
                    px = static_cast<int>(p.sx + L + p.vx * s + std::sin(p.ph + t * p.freq) * p.wob * t);
                    py = static_cast<int>(p.sy + T + p.vy * s + 0.5f * p.lift * s * s);
                    fa = static_cast<unsigned>(255.0f * std::pow(1.0f - t, 1.3f));
                    if (t > 0.55f && g > 2) size = g - 1;
                    if (fa == 0) continue;
                }
                for (int dy = 0; dy < size; dy++) {
                    int sy = p.sy + dy, oy = py + dy;
                    if (sy >= ih || oy < 0 || oy >= oh) continue;
                    const uint32_t* src = &j->img[static_cast<size_t>(sy) * iw];
                    uint32_t* dst = &out[static_cast<size_t>(oy) * ow];
                    for (int dx = 0; dx < size; dx++) {
                        int sx = p.sx + dx, ox = px + dx;
                        if (sx >= iw || ox < 0 || ox >= ow) continue;
                        uint32_t c = src[sx];
                        unsigned a = ((c >> 24) * fa + 127) / 255;
                        if (a == 0 || (dst[ox] >> 24) >= a) continue;
                        unsigned r = ((c >> 16) & 255) * a / 255, gg = ((c >> 8) & 255) * a / 255, b = (c & 255) * a / 255;   // premultiplied
                        dst[ox] = (a << 24) | (r << 16) | (gg << 8) | b;
                    }
                }
            }
            POINT pos{j->at.x - L, j->at.y - T}, zero{0, 0};
            SIZE sz{ow, oh};
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(w, screen, &pos, &sz, mem, &zero, 0, &bf, ULW_ALPHA);
            if (!shown) {
                ShowWindow(w, SW_SHOWNOACTIVATE);
                SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (dwm) DwmFlush();   // on screen before the real window goes
                shown = true;
                QueryPerformanceCounter(&t0);
                if (j->ready) SetEvent(j->ready);
            }
            MSG msg;
            while (PeekMessageW(&msg, w, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            if (el > end + 30) break;
            if (dwm) { if (FAILED(DwmFlush())) Sleep(15); } else Sleep(15);   // one frame per screen refresh
        }
        if (w) DestroyWindow(w);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (j->ready) SetEvent(j->ready);   // (also when the window couldn't be made)
}

// what is on screen in r (the window is in front: exactly what the user sees)
bool grabScreen(const RECT& r, std::vector<uint32_t>& px) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY) != FALSE;   // no CAPTUREBLT: other dust in the air isn't picked up
        GdiFlush();
        if (ok) {
            px.assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + static_cast<size_t>(w) * h);
            for (auto& c : px) c |= 0xFF000000u;
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

// the window's own picture even when something covers it (Windows 8.1 and later)
bool grabWindow(HWND hw, const RECT& frame, std::vector<uint32_t>& px) {
    RECT wr;
    if (!GetWindowRect(hw, &wr)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = PrintWindow(hw, mem, PW_RENDERFULLCONTENT) != FALSE;
        GdiFlush();
        if (ok) {
            int fx = frame.left - wr.left, fy = frame.top - wr.top, fw = frame.right - frame.left, fh = frame.bottom - frame.top;
            fx = std::max(0, fx); fy = std::max(0, fy);
            fw = std::min(fw, w - fx); fh = std::min(fh, h - fy);
            const uint32_t* src = static_cast<uint32_t*>(bits);
            px.resize(static_cast<size_t>(std::max(0, fw)) * std::max(0, fh));
            for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) px[static_cast<size_t>(y) * fw + x] = src[static_cast<size_t>(y + fy) * w + x + fx] | 0xFF000000u;
            // an all-black picture means the window couldn't draw itself this way
            size_t dark = 0;
            for (size_t i = 0; i < px.size(); i += 97) if ((px[i] & 0xFFFFFF) == 0) dark++;
            ok = fw > 0 && fh > 0 && dark * 97 < px.size() * 9 / 10;
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

bool win81OrLater() {
    typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    static int v = -1;
    if (v < 0) {
        v = 0;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        RTL_OSVERSIONINFOW o{sizeof o};
        if (fn && fn(&o) == 0) v = (o.dwMajorVersion > 6 || (o.dwMajorVersion == 6 && o.dwMinorVersion >= 3)) ? 1 : 0;
    }
    return v == 1;
}

void CALLBACK transitionsBack(HWND h, UINT, UINT_PTR id, DWORD) {
    KillTimer(h, id);
    BOOL off = FALSE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof off);
}

} // namespace

int durationMs() { return 1150; }

bool enabled() {
    if (!settings().dustEffect) return false;
    // Wine (Linux / macOS): its see-through windows are cut into one X11 shape piece per grain, which its X server
    // can't take for a big window (the program is stopped). The effect needs real Windows.
    static const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    if (wine && !GetEnvironmentVariableW(L"DDM_TEST_DUST", nullptr, 0)) return false;
    if (GetSystemMetrics(SM_REMOTESESSION)) return false;   // Remote Desktop: no heavy animation over the network
    BOOL anim = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0) && !anim) return false;   // Windows: "Show animations" off
    return true;
}

bool image(std::vector<uint32_t> bgra, int w, int h, POINT screen, int dur) {
    if (!enabled() || w <= 0 || h <= 0 || w > 16000 || h > 16000 || bgra.size() < static_cast<size_t>(w) * h) return false;
    if (static_cast<long long>(w) * h > 12000000LL) return false;   // a window across several 4K screens: it just closes (memory stays small)
    if (g_running.fetch_add(1) >= 4) { g_running--; return false; }
    auto* j = new Job();
    j->img = std::move(bgra);
    j->iw = w; j->ih = h; j->at = screen; j->dur = dur > 0 ? dur : durationMs();
    j->scale = std::max(1.0f, dpi() / 96.0f);
    j->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ready = nullptr;
    if (j->ready) DuplicateHandle(GetCurrentProcess(), j->ready, GetCurrentProcess(), &ready, 0, FALSE, DUPLICATE_SAME_ACCESS);
    try {
        std::thread([j] {
            play(j);
            if (j->ready) CloseHandle(j->ready);
            delete j;
            g_running--;
        }).detach();
    } catch (...) {
        if (j->ready) CloseHandle(j->ready);
        delete j;
        g_running--;
        if (ready) CloseHandle(ready);
        return false;
    }
    if (ready) { WaitForSingleObject(ready, 400); CloseHandle(ready); }   // the dust stands where the picture was
    return true;
}

bool window(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) || !enabled()) return false;
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof frame)) || frame.right <= frame.left) GetWindowRect(h, &frame);
    // only the part on the screens
    RECT vs{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT r;
    if (!IntersectRect(&r, &frame, &vs)) return false;
    if (static_cast<long long>(r.right - r.left) * (r.bottom - r.top) > 12000000LL) return false;
    std::vector<uint32_t> px;
    bool front = GetForegroundWindow() == h;
    bool ok = false;
    if (!front && win81OrLater() && EqualRect(&r, &frame)) ok = grabWindow(h, frame, px);
    if (!ok) ok = grabScreen(r, px);
    if (!ok) return false;
    if (!image(std::move(px), r.right - r.left, r.bottom - r.top, POINT{r.left, r.top}, durationMs())) return false;
    // the dust replaces the window: Windows' own fade / minimize animation would show behind it, so skip it this once
    BOOL off = TRUE;
    DwmSetWindowAttribute(h, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof off);
    SetTimer(h, kTimerTransitions, 900, transitionsBack);
    return true;
}

} // namespace dust
} // namespace ui
} // namespace ddm
