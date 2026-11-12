// Colours (light and dark, exactly as the mock-ups), fonts, DPI scaling and Windows dark-mode plumbing.
#pragma once
#include <windows.h>
#include <string>

namespace ddm {
namespace ui {

struct Palette {
    COLORREF win, title, bar, pane, list, head, line, text, muted, sel, hover, zebra, rowLine;
    COLORREF accent, accentText, good, warn, bad, track, folder;
    COLORREF field, fieldLine, tile, foot, menuBg, menuHover, xrayBg, xrayText, toolSel;
};

extern Palette P;               // current colours
bool isDark();
void applyTheme(const std::string& mode);   // "system" | "light" | "dark"
bool systemUsesDark();

// DPI
void initDpi();
int S(int px);                  // scale 96-dpi pixels to the screen
float Sf(float px);
int dpi();

// Fonts (created for the current DPI)
HFONT fontUI();                 // 13 px, normal
HFONT fontBold();               // 13 px, semibold
HFONT fontSmall();              // 12 px
HFONT fontSmallBold();
HFONT fontTool();               // toolbar labels (11.5 px)
HFONT fontBig();                // 18 px semibold (prompt size)
void rebuildFonts();
const wchar_t* uiFace();         // the UI typeface for the current language
void fontsForLanguage();        // after a language change: new fonts when the language needs another face

// Windows 10/11 dark title bar, scrollbars and menus; no-ops on Windows 7.
void styleWindow(HWND h);
void styleScroll(HWND h);       // dark scrollbars for a control
void allowDarkMenus();

COLORREF blend(COLORREF a, COLORREF b, float t);
inline COLORREF rgb(unsigned v) { return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255); }

} // namespace ui
} // namespace ddm
