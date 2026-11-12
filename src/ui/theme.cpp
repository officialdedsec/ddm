#include "theme.hpp"
#include "../core/i18n.hpp"
#include <dwmapi.h>
#include <uxtheme.h>

namespace ddm {
namespace ui {

Palette P;
static bool g_dark = false;
static int g_dpi = 96;
static HFONT g_fonts[6] = {};

static const Palette kLight = {
    rgb(0xFFFFFF), rgb(0xF3F4F6), rgb(0xF7F8FA), rgb(0xF5F6F8), rgb(0xFFFFFF), rgb(0xF7F8FA), rgb(0xDDE0E5), rgb(0x1C1E23), rgb(0x5D636E),
    rgb(0xDCE9FD), rgb(0xEEF1F5), rgb(0xFAFBFC), rgb(0xEEF0F3),
    rgb(0x1F6FEB), rgb(0xFFFFFF), rgb(0x1F8A3E), rgb(0x9A5B00), rgb(0xC42B2B), rgb(0xE2E5EA), rgb(0xC98A12),
    rgb(0xFFFFFF), rgb(0xC9CED6), rgb(0xF3F5F8), rgb(0xF7F8FA), rgb(0xFFFFFF), rgb(0xE8F0FD), rgb(0xE8F0FD), rgb(0x1858BC), rgb(0xE8F0FD),
};

static const Palette kDark = {
    rgb(0x1B1D22), rgb(0x202227), rgb(0x24272D), rgb(0x1F2126), rgb(0x1B1D22), rgb(0x23262C), rgb(0x33363E), rgb(0xE8EAEE), rgb(0xA3A8B3),
    rgb(0x21395E), rgb(0x262A31), rgb(0x1E2025), rgb(0x24262C),
    rgb(0x5EA2FF), rgb(0x0E1726), rgb(0x5BD07A), rgb(0xF2B866), rgb(0xFF7B7B), rgb(0x33363E), rgb(0xE7B84E),
    rgb(0x2A2D34), rgb(0x3E424B), rgb(0x2A2D34), rgb(0x1B1D22), rgb(0x25272D), rgb(0x33363E), rgb(0x25324A), rgb(0xA9CBFF), rgb(0x25324A),
};

bool isDark() { return g_dark; }

bool systemUsesDark() {
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v == 0;
    return false;   // Windows 7/8: light
}

void applyTheme(const std::string& mode) {
    g_dark = mode == "dark" || (mode != "light" && systemUsesDark());
    P = g_dark ? kDark : kLight;
    allowDarkMenus();
}

typedef UINT(WINAPI* GetDpiForSystemFn)();
void initDpi() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    auto fn = reinterpret_cast<GetDpiForSystemFn>(reinterpret_cast<void*>(GetProcAddress(u, "GetDpiForSystem")));
    if (fn) g_dpi = static_cast<int>(fn());
    else { HDC dc = GetDC(nullptr); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(nullptr, dc); }
    if (g_dpi < 96) g_dpi = 96;
    rebuildFonts();
}

int dpi() { return g_dpi; }
int S(int px) { return MulDiv(px, g_dpi, 96); }
float Sf(float px) { return px * g_dpi / 96.0f; }

static int CALLBACK faceFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) { *reinterpret_cast<bool*>(p) = true; return 0; }
static bool faceInstalled(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, faceFound, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// Segoe UI has no Indian scripts: Bengali and Hindi use Windows' own face for them (Nirmala UI on Windows 8.1 and
// later; Vrinda / Mangal on Windows 7). Every other language reads well in Segoe UI.
const wchar_t* uiFace() {
    const std::string& c = i18n::current();
    const wchar_t* pick = L"Segoe UI";
    if (c == "bn") pick = faceInstalled(L"Nirmala UI") ? L"Nirmala UI" : faceInstalled(L"Vrinda") ? L"Vrinda" : L"Segoe UI";
    else if (c == "hi") pick = faceInstalled(L"Nirmala UI") ? L"Nirmala UI" : faceInstalled(L"Mangal") ? L"Mangal" : L"Segoe UI";
    return pick;
}

static std::wstring g_face;

static HFONT makeFont(int px, int weight) {
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, g_face.c_str());
}

void fontsForLanguage() {
    if (g_face == uiFace()) return;
    // The old fonts stay alive: edit boxes made earlier still hold them (a few handles, only when the language changes).
    for (auto& f : g_fonts) f = nullptr;
    rebuildFonts();
}

void rebuildFonts() {
    for (auto& f : g_fonts) if (f) { DeleteObject(f); f = nullptr; }
    g_face = uiFace();
    g_fonts[0] = makeFont(13, FW_NORMAL);
    g_fonts[1] = makeFont(13, FW_SEMIBOLD);
    g_fonts[2] = makeFont(12, FW_NORMAL);
    g_fonts[3] = makeFont(12, FW_SEMIBOLD);
    g_fonts[4] = makeFont(12, FW_NORMAL);
    g_fonts[5] = makeFont(18, FW_SEMIBOLD);
}

HFONT fontUI() { return g_fonts[0]; }
HFONT fontBold() { return g_fonts[1]; }
HFONT fontSmall() { return g_fonts[2]; }
HFONT fontSmallBold() { return g_fonts[3]; }
HFONT fontTool() { return g_fonts[4]; }
HFONT fontBig() { return g_fonts[5]; }

// Undocumented but stable uxtheme entry points (Windows 10 1809+). Looked up by ordinal; missing on Windows 7/8.
typedef int(WINAPI* SetPreferredAppModeFn)(int);
typedef BOOL(WINAPI* AllowDarkModeForWindowFn)(HWND, BOOL);
typedef void(WINAPI* FlushMenuThemesFn)();

static HMODULE uxtheme() { static HMODULE m = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); return m; }

static bool win10_1809() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof v;
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (fn) fn(&v);
    return v.dwMajorVersion >= 10 && v.dwBuildNumber >= 17763;
}

void allowDarkMenus() {
    if (!win10_1809() || !uxtheme()) return;
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(reinterpret_cast<void*>(GetProcAddress(uxtheme(), MAKEINTRESOURCEA(135))));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(reinterpret_cast<void*>(GetProcAddress(uxtheme(), MAKEINTRESOURCEA(136))));
    if (setMode) setMode(g_dark ? 2 : 3);   // 2 = force dark, 3 = force light
    if (flush) flush();
}

void styleWindow(HWND h) {
    BOOL dark = g_dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &dark, sizeof dark))) DwmSetWindowAttribute(h, 19, &dark, sizeof dark);
    COLORREF cap = P.title;
    DwmSetWindowAttribute(h, 35, &cap, sizeof cap);      // Windows 11 caption colour; ignored elsewhere
    COLORREF txt = P.text;
    DwmSetWindowAttribute(h, 36, &txt, sizeof txt);
    if (win10_1809() && uxtheme()) {
        auto allow = reinterpret_cast<AllowDarkModeForWindowFn>(reinterpret_cast<void*>(GetProcAddress(uxtheme(), MAKEINTRESOURCEA(133))));
        if (allow) allow(h, dark);
    }
}

void styleScroll(HWND h) {
    if (win10_1809()) SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

COLORREF blend(COLORREF a, COLORREF b, float t) {
    auto mix = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5f); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

} // namespace ui
} // namespace ddm
