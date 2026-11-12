// Window base class and DDM's own controls, drawn to match the mock-ups in light and dark.
#pragma once
#include "../core/i18n.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace ui {

// A window with a virtual message handler.
class Wnd {
public:
    virtual ~Wnd();
    HWND hwnd() const { return h; }
    bool create(const wchar_t* cls, const std::string& title, DWORD style, DWORD exStyle, int x, int y, int w, int ht, HWND parent, HMENU id = nullptr);
    // NOCOPYBITS: the control repaints itself at its new place (copying old pixels left stale text behind on Windows)
    void move(int x, int y, int w, int ht) { SetWindowPos(h, nullptr, x, y, w, ht, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS); }
    void show(bool on = true) { ShowWindow(h, on ? SW_SHOW : SW_HIDE); }
    void redraw() { if (h) InvalidateRect(h, nullptr, FALSE); }
    RECT client() const { RECT r{}; GetClientRect(h, &r); return r; }
    static void registerClass(const wchar_t* cls, UINT style = CS_DBLCLKS);

protected:
    HWND h = nullptr;
    virtual LRESULT proc(UINT m, WPARAM w, LPARAM l);
    LRESULT def(UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
private:
    static LRESULT CALLBACK router(HWND h, UINT m, WPARAM w, LPARAM l);
};

// ---------------------------------------------------------------- menus (owner-drawn so they follow the theme everywhere)
struct MenuItem {
    std::string text, shortcut;
    UINT id = 0;
    bool checked = false, disabled = false, separator = false, bold = false;
    std::vector<MenuItem> sub;
    static MenuItem sep() { MenuItem m; m.separator = true; return m; }
};
// Shows a popup at screen point; returns the chosen id (0 = none).
UINT popupMenu(HWND owner, const std::vector<MenuItem>& items, POINT screen, bool alignRight = false);
HMENU buildMenu(const std::vector<MenuItem>& items);
bool handleMenuMeasure(LPARAM l);
bool handleMenuDraw(LPARAM l);

// ---------------------------------------------------------------- controls
enum class BtnStyle { Normal, Primary, Subtle, Pill };

class Button : public Wnd {
public:
    std::string text, iconName;
    BtnStyle style = BtnStyle::Normal;
    std::function<void()> onClick;
    bool create(HWND parent, const std::string& t, BtnStyle s = BtnStyle::Normal, const std::string& ic = "");
    void setText(const std::string& t) { text = t; redraw(); }
    void setTint(COLORREF c) { tint = c; tinted = true; redraw(); }   // a Pill in a warning / good colour
    int preferredWidth();
    COLORREF tint = 0;
    bool tinted = false;
    int iconSize = 0;            // > 0: a bigger (or smaller) icon than the style's own
    // Own colours (a window with its own look, e.g. the download window); off = the theme's colours as always
    bool custom = false;
    COLORREF cFace = 0, cFg = 0, cBorder = 0, cFocus = 0;
    float cRadius = -1;          // < 0: the style's own corner
    bool cBold = false;
    void colors(COLORREF face, COLORREF fg, COLORREF border, float radius, bool bold, COLORREF focus) { custom = true; cFace = face; cFg = fg; cBorder = border; cRadius = radius; cBold = bold; cFocus = focus; redraw(); }
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
private:
    bool hover = false, down = false;
};

class CheckBox : public Wnd {
public:
    std::string text;
    bool checked = false;
    bool radio = false;               // a round option button (clicking a checked one does nothing)
    bool customColors = false;        // own colours (see Button::colors); off = the theme's
    COLORREF cAccent = 0, cText = 0, cBox = 0, cLine = 0, cCheck = 0;
    void colors(COLORREF accent, COLORREF textColor, COLORREF box, COLORREF boxLine, COLORREF tick) { customColors = true; cAccent = accent; cText = textColor; cBox = box; cLine = boxLine; cCheck = tick; redraw(); }
    std::function<void(bool)> onChange;
    bool create(HWND parent, const std::string& t, bool on);
    void set(bool on) { checked = on; redraw(); }
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
private:
    bool hover = false;
};

// Text field: rounded border around a native edit box.
class Field : public Wnd {
public:
    std::function<void()> onChange, onEnter;
    bool create(HWND parent, const std::string& value, bool multiline = false, bool readOnly = false);
    std::string get() const;
    void set(const std::string& v);
    HWND edit() const { return ed; }
    void focus() { SetFocus(ed); }
    void hint(const std::string& t) { std::wstring w = W(i18n::tr(t)); SendMessageW(ed, 0x1501 /*EM_SETCUEBANNER*/, TRUE, reinterpret_cast<LPARAM>(w.c_str())); }
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
private:
    HWND ed = nullptr;
    bool multi = false, focused = false;
    HBRUSH brush = nullptr;
    static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref);
};

// Drop-down list (a field that opens a themed menu).
class DropDown : public Wnd {
public:
    std::vector<std::string> options;
    int selected = 0;
    std::string iconName;
    COLORREF iconColor = 0;
    bool useIconColor = false;
    std::function<void(int)> onChange;
    bool create(HWND parent, const std::vector<std::string>& opts, int sel);
    void select(int i) { selected = i; redraw(); }
    std::string value() const { return selected >= 0 && selected < static_cast<int>(options.size()) ? options[static_cast<size_t>(selected)] : ""; }
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
private:
    bool hover = false;
};

class Label : public Wnd {
public:
    std::string text;
    COLORREF color = 0;
    bool useColor = false, bold = false, small = false, wrap = false, right = false;
    COLORREF* background = nullptr;   // when set, paint this colour instead of the parent's (e.g. a footer band)
    bool create(HWND parent, const std::string& t);
    void setText(const std::string& t) { text = t; redraw(); }
    void setColor(COLORREF c) { color = c; useColor = true; redraw(); }
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
};

// Simple progress bar.
class Bar : public Wnd {
public:
    double value = 0;    // 0..1
    COLORREF color = 0;
    bool useColor = false;
    bool create(HWND parent);
    void set(double v) { if (v != value) { value = v; redraw(); } }
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
};

// Children ask their parent for its background colour: answer bgReply(colour).
const UINT WM_DDM_BG = WM_USER + 900;
inline LRESULT bgReply(COLORREF c) { return static_cast<LRESULT>(c | 0x01000000); }
COLORREF parentBg(HWND child);

void initWidgets();
void tooltip(HWND owner, HWND tool, const std::string& text);
// Fills the background of a child-hosting window with its theme colour (used by WM_ERASEBKGND).
LRESULT eraseWith(HWND h, HDC dc, COLORREF c);

} // namespace ui
} // namespace ddm
