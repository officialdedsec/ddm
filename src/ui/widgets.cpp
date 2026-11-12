#include "widgets.hpp"
#include "../core/i18n.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "../common/util.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <map>
#include <memory>

namespace ddm {
namespace ui {

// ---------------------------------------------------------------- Wnd
Wnd::~Wnd() {
    if (h && IsWindow(h)) { SetWindowLongPtrW(h, GWLP_USERDATA, 0); DestroyWindow(h); }
}

void Wnd::registerClass(const wchar_t* cls, UINT style) {
    WNDCLASSEXW wc{};
    if (GetClassInfoExW(GetModuleHandleW(nullptr), cls, &wc)) return;
    wc.cbSize = sizeof wc;
    wc.style = style;
    wc.lpfnWndProc = router;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
}

bool Wnd::create(const wchar_t* cls, const std::string& title, DWORD style, DWORD ex, int x, int y, int w, int ht, HWND parent, HMENU id) {
    registerClass(cls);
    h = CreateWindowExW(ex, cls, W(i18n::tr(title)).c_str(), style, x, y, w, ht, parent, id, GetModuleHandleW(nullptr), this);
    return h != nullptr;
}

LRESULT CALLBACK Wnd::router(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    Wnd* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Wnd*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->h = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else self = reinterpret_cast<Wnd*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, m, w, l);
    if (m == WM_MEASUREITEM && handleMenuMeasure(l)) return TRUE;
    if (m == WM_DRAWITEM && handleMenuDraw(l)) return TRUE;
    LRESULT r = self->proc(m, w, l);
    if (m == WM_NCDESTROY) { SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); self->h = nullptr; }
    return r;
}

LRESULT Wnd::proc(UINT m, WPARAM w, LPARAM l) { return def(m, w, l); }

COLORREF parentBg(HWND child) {
    LRESULT r = SendMessageW(GetParent(child), WM_DDM_BG, reinterpret_cast<WPARAM>(child), 0);
    return (r & 0x01000000) ? static_cast<COLORREF>(r & 0xFFFFFF) : P.win;
}

LRESULT eraseWith(HWND h, HDC dc, COLORREF c) {
    RECT r; GetClientRect(h, &r);
    fill(dc, r, c);
    return 1;
}

static bool trackLeave(HWND h) {
    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
    return TrackMouseEvent(&t) != 0;
}

// ---------------------------------------------------------------- menus
namespace {
struct MenuData { std::string text, shortcut; bool separator, bold, checked, disabled, hasSub; };
std::vector<std::unique_ptr<MenuData>> g_menuData;   // lives until the next menu is built

void addItems(HMENU m, const std::vector<MenuItem>& items) {
    for (auto& it : items) {
        auto d = std::make_unique<MenuData>(MenuData{it.text, it.shortcut, it.separator, it.bold, it.checked, it.disabled, !it.sub.empty()});
        UINT flags = MF_OWNERDRAW | (it.disabled ? MF_GRAYED : 0) | (it.checked ? MF_CHECKED : 0);
        if (it.separator) AppendMenuW(m, MF_OWNERDRAW | MF_SEPARATOR, 0, reinterpret_cast<LPCWSTR>(d.get()));
        else if (!it.sub.empty()) { HMENU s = CreatePopupMenu(); addItems(s, it.sub); AppendMenuW(m, flags | MF_POPUP, reinterpret_cast<UINT_PTR>(s), reinterpret_cast<LPCWSTR>(d.get())); }
        else AppendMenuW(m, flags, it.id, reinterpret_cast<LPCWSTR>(d.get()));
        g_menuData.push_back(std::move(d));
    }
    MENUINFO mi{};
    mi.cbSize = sizeof mi;
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    static HBRUSH br = nullptr;
    static COLORREF brc = 0;
    if (!br || brc != P.menuBg) { if (br) DeleteObject(br); br = CreateSolidBrush(P.menuBg); brc = P.menuBg; }
    mi.hbrBack = br;
    SetMenuInfo(m, &mi);
}
} // namespace

HMENU buildMenu(const std::vector<MenuItem>& items) {
    g_menuData.clear();
    HMENU m = CreatePopupMenu();
    addItems(m, items);
    return m;
}

UINT popupMenu(HWND owner, const std::vector<MenuItem>& items, POINT pt, bool alignRight) {
    HMENU m = buildMenu(items);
    SetForegroundWindow(owner);
    UINT r = static_cast<UINT>(TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | (alignRight ? TPM_RIGHTALIGN : TPM_LEFTALIGN), pt.x, pt.y, owner, nullptr));
    DestroyMenu(m);
    PostMessageW(owner, WM_NULL, 0, 0);
    return r;
}

bool handleMenuMeasure(LPARAM l) {
    auto mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
    if (mi->CtlType != ODT_MENU || !mi->itemData) return false;
    auto d = reinterpret_cast<MenuData*>(mi->itemData);
    if (d->separator) { mi->itemWidth = S(40); mi->itemHeight = S(9); return true; }
    HDC dc = GetDC(nullptr);
    SIZE a = textSize(dc, d->text, d->bold ? fontBold() : fontUI());
    SIZE b = textSize(dc, d->shortcut, fontUI());
    ReleaseDC(nullptr, dc);
    mi->itemWidth = static_cast<UINT>(S(36) + a.cx + (d->shortcut.empty() ? 0 : S(28) + b.cx) + S(28));
    mi->itemHeight = static_cast<UINT>(S(28));
    return true;
}

bool handleMenuDraw(LPARAM l) {
    auto di = reinterpret_cast<DRAWITEMSTRUCT*>(l);
    if (di->CtlType != ODT_MENU || !di->itemData) return false;
    auto d = reinterpret_cast<MenuData*>(di->itemData);
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    fill(dc, r, P.menuBg);
    if (d->separator) { int y = (r.top + r.bottom) / 2; RECT ln{r.left + S(8), y, r.right - S(8), y + 1}; fill(dc, ln, P.line); return true; }
    bool sel = (di->itemState & ODS_SELECTED) && !(di->itemState & ODS_GRAYED);
    if (sel) { RECT hr = r; InflateRect(&hr, -S(4), -S(1)); fillRound(dc, hr, Sf(5), P.menuHover); }
    COLORREF fg = (di->itemState & ODS_GRAYED) ? P.muted : P.text;
    if (di->itemState & ODS_CHECKED) { RECT ic{r.left + S(10), r.top + S(7), r.left + S(24), r.top + S(21)}; icon(dc, "check", ic, P.accent, 2.4f); }
    RECT tr = r;
    tr.left += S(32);
    tr.right -= S(14);
    ui::text(dc, d->text, tr, fg, d->bold ? fontBold() : fontUI());
    if (!d->shortcut.empty()) text(dc, d->shortcut, tr, P.muted, fontUI(), DT_RIGHT);
    // Windows draws the submenu arrow itself after this; exclude it so our colours stay.
    if (d->hasSub) { RECT ar{r.right - S(20), r.top + S(7), r.right - S(6), r.top + S(21)}; icon(dc, "chevronr", ar, P.muted, 2.0f); ExcludeClipRect(dc, r.left, r.top, r.right, r.bottom); }
    return true;
}

// ---------------------------------------------------------------- Button
bool Button::create(HWND parent, const std::string& t, BtnStyle s, const std::string& ic) {
    text = t; style = s; iconName = ic;
    return Wnd::create(L"DDMButton", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 10, 10, parent);
}

int Button::preferredWidth() {
    HDC dc = GetDC(h);
    SIZE sz = textSize(dc, text, style == BtnStyle::Primary ? fontBold() : fontUI());
    ReleaseDC(h, dc);
    return sz.cx + S(28) + (iconName.empty() ? 0 : S(22));
}

LRESULT Button::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            Paint p(h);
            HDC dc = p.dc();
            RECT r = p.rc();
            COLORREF bg = parentBg(h);
            fill(dc, r, bg);
            bool en = IsWindowEnabled(h) != 0;
            COLORREF face = P.field, fg = P.text, border = P.fieldLine;
            if (style == BtnStyle::Primary) { face = P.accent; fg = P.accentText; border = P.accent; if (hover) face = blend(P.accent, isDark() ? RGB(255, 255, 255) : RGB(0, 0, 0), 0.10f); }
            else if (style == BtnStyle::Subtle) { face = hover ? P.hover : bg; border = face; }
            else if (style == BtnStyle::Pill && tinted) { face = blend(bg, tint, isDark() ? 0.22f : 0.13f); fg = tint; border = face; if (hover) face = blend(face, tint, 0.12f); }
            else if (style == BtnStyle::Pill) { face = P.xrayBg; fg = P.xrayText; border = face; if (hover) face = blend(face, P.xrayText, 0.12f); }
            else if (hover) face = P.hover;
            if (down) face = blend(face, P.text, 0.08f);
            if (!en) { fg = P.muted; if (style == BtnStyle::Primary) { face = blend(P.field, P.win, 0.5f); border = P.fieldLine; } }
            if (custom) {   // the window's own look
                face = cFace; fg = cFg; border = cBorder;
                if (hover) face = blend(cFace, cFg, 0.10f);
                if (down) face = blend(face, cFg, 0.08f);
                if (!en) fg = blend(cFg, bg, 0.5f);
            }
            float rad = style == BtnStyle::Pill ? (r.bottom - r.top) / 2.0f : Sf(6);
            if (custom && cRadius >= 0) rad = std::min(cRadius, (r.bottom - r.top) / 2.0f);
            fillRound(dc, r, rad, face);
            if (custom) { if (cBorder != cFace) strokeRound(dc, r, rad, border); }
            else if (style == BtnStyle::Normal || (!en && style == BtnStyle::Primary)) strokeRound(dc, r, rad, border);
            if (GetFocus() == h) { RECT f = r; InflateRect(&f, -S(2), -S(2)); strokeRound(dc, f, rad, custom ? cFocus : P.accent, 1.2f); }
            HFONT font = style == BtnStyle::Primary || style == BtnStyle::Pill ? fontBold() : fontUI();
            if (style == BtnStyle::Pill) font = fontSmallBold();
            if (custom) font = cBold ? fontBold() : fontUI();
            SIZE ts = textSize(dc, text, font);
            int iw = iconName.empty() ? 0 : S(iconSize > 0 ? iconSize : style == BtnStyle::Pill ? 13 : 15);
            int gap = (iw && !text.empty()) ? S(6) : 0;
            int x = (r.right - (iw + gap + ts.cx)) / 2;
            if (iw) { RECT ir{x, (r.bottom - iw) / 2, x + iw, (r.bottom - iw) / 2 + iw}; icon(dc, iconName, ir, fg, 2.1f); }
            RECT tr{x + iw + gap, 0, r.right, r.bottom};
            ui::text(dc, text, tr, fg, font, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            return 0;
        }
        case WM_MOUSEMOVE: if (!hover) { hover = true; trackLeave(h); redraw(); } return 0;
        case WM_MOUSELEAVE: hover = false; down = false; redraw(); return 0;
        case WM_LBUTTONDOWN: down = true; SetCapture(h); redraw(); return 0;
        case WM_LBUTTONUP: {
            bool was = down;
            down = false;
            ReleaseCapture();
            redraw();
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT r = client();
            if (was && PtInRect(&r, pt) && onClick) onClick();
            return 0;
        }
        case WM_KEYDOWN: if ((w == VK_SPACE || w == VK_RETURN) && onClick) onClick(); return 0;
        case WM_GETDLGCODE: return DLGC_BUTTON;
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: redraw(); return 0;
    }
    return def(m, w, l);
}

// ---------------------------------------------------------------- CheckBox
bool CheckBox::create(HWND parent, const std::string& t, bool on) {
    text = t; checked = on;
    return Wnd::create(L"DDMCheck", t, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 10, 10, parent);
}

LRESULT CheckBox::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            Paint p(h);
            HDC dc = p.dc();
            RECT r = p.rc();
            COLORREF bg = parentBg(h);
            fill(dc, r, bg);
            int b = S(15), y = (r.bottom - b) / 2;
            RECT box{0, y, b, y + b};
            bool on = IsWindowEnabled(h);
            if (radio) {
                float cx = (box.left + box.right) / 2.0f, cy = (box.top + box.bottom) / 2.0f;
                fillCircle(dc, cx, cy, b / 2.0f, checked || hover ? (on ? P.accent : P.muted) : P.fieldLine);
                fillCircle(dc, cx, cy, b / 2.0f - Sf(1.3f), P.field);
                if (checked) fillCircle(dc, cx, cy, b / 2.0f - Sf(4), on ? P.accent : P.muted);
            } else if (customColors) {   // the window's own look
                if (checked) { fillRound(dc, box, Sf(3), cAccent); RECT ic = box; InflateRect(&ic, -S(1), -S(1)); icon(dc, "check", ic, cCheck, 3.0f); }
                else { fillRound(dc, box, Sf(3), cBox); strokeRound(dc, box, Sf(3), hover && on ? cAccent : cLine); }
            } else if (checked) { fillRound(dc, box, Sf(3), on ? P.accent : P.muted); RECT ic = box; InflateRect(&ic, -S(1), -S(1)); icon(dc, "check", ic, P.accentText, 3.0f); }
            else { fillRound(dc, box, Sf(3), P.field); strokeRound(dc, box, Sf(3), hover && on ? P.accent : P.fieldLine); }
            RECT tr{b + S(8), 0, r.right, r.bottom};
            ui::text(dc, text, tr, customColors ? cText : IsWindowEnabled(h) ? P.text : P.muted, customColors ? fontBold() : fontUI());
            if (GetFocus() == h) { RECT f = box; InflateRect(&f, S(2), S(2)); strokeRound(dc, f, Sf(4), customColors ? cAccent : P.accent); }
            return 0;
        }
        case WM_MOUSEMOVE: if (!hover) { hover = true; trackLeave(h); redraw(); } return 0;
        case WM_MOUSELEAVE: hover = false; redraw(); return 0;
        case WM_LBUTTONUP: case WM_KEYDOWN:
            if (m == WM_KEYDOWN && w != VK_SPACE) break;
            if (radio && checked) return 0;
            checked = !checked; redraw();
            if (onChange) onChange(checked);
            return 0;
        case WM_LBUTTONDOWN: SetFocus(h); return 0;
        case WM_GETDLGCODE: return DLGC_BUTTON;
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: redraw(); return 0;
    }
    return def(m, w, l);
}

// ---------------------------------------------------------------- Field
bool Field::create(HWND parent, const std::string& value, bool multiline, bool readOnly) {
    multi = multiline;
    if (!Wnd::create(L"DDMField", "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, WS_EX_CONTROLPARENT, 0, 0, 10, 10, parent)) return false;
    DWORD st = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (multi ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN) : ES_AUTOHSCROLL) | (readOnly ? ES_READONLY : 0);
    ed = CreateWindowExW(0, L"EDIT", W(value).c_str(), st, 0, 0, 10, 10, h, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ed, WM_SETFONT, reinterpret_cast<WPARAM>(fontUI()), TRUE);
    SendMessageW(ed, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    SetWindowSubclass(ed, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    styleScroll(ed);
    return true;
}

LRESULT CALLBACK Field::editProc(HWND e, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto f = reinterpret_cast<Field*>(ref);
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) { f->focused = m == WM_SETFOCUS; f->redraw(); }
    if (m == WM_KEYDOWN && w == VK_RETURN && !f->multi) { if (f->onEnter) f->onEnter(); return 0; }
    if (m == WM_CHAR && (w == VK_RETURN) && !f->multi) return 0;
    if (m == WM_KEYDOWN && w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(e, EM_SETSEL, 0, -1); return 0; }
    return DefSubclassProc(e, m, w, l);
}

std::string Field::get() const {
    int n = GetWindowTextLengthW(ed);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(ed, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return U(s);
}

void Field::set(const std::string& v) { SetWindowTextW(ed, W(v).c_str()); }

LRESULT Field::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: {
            int cw = LOWORD(l), ch = HIWORD(l);
            if (multi) MoveWindow(ed, S(9), S(6), cw - S(12), ch - S(10), TRUE);
            else {
                HDC dc = GetDC(h);
                SIZE ts = textSize(dc, "Ag", fontUI());
                ReleaseDC(h, dc);
                MoveWindow(ed, S(9), (ch - ts.cy) / 2, cw - S(18), ts.cy, TRUE);
            }
            return 0;
        }
        case WM_PAINT: {
            Paint p(h);
            HDC dc = p.dc();
            RECT r = p.rc();
            COLORREF bg = parentBg(h);
            fill(dc, r, bg);
            fillRound(dc, r, Sf(5), P.field);
            strokeRound(dc, r, Sf(5), focused ? P.accent : P.fieldLine, focused ? 1.4f : 1.0f);
            return 0;
        }
        case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, P.text);
            SetBkColor(dc, P.field);
            if (brush) DeleteObject(brush);
            brush = CreateSolidBrush(P.field);
            return reinterpret_cast<LRESULT>(brush);
        }
        case WM_COMMAND:
            if (HIWORD(w) == EN_CHANGE && onChange) onChange();
            return 0;
        case WM_LBUTTONDOWN: SetFocus(ed); return 0;
        case WM_DESTROY: if (brush) { DeleteObject(brush); brush = nullptr; } break;
    }
    return def(m, w, l);
}

// ---------------------------------------------------------------- DropDown
bool DropDown::create(HWND parent, const std::vector<std::string>& opts, int sel) {
    options = opts; selected = sel;
    return Wnd::create(L"DDMDrop", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 10, 10, parent);
}

LRESULT DropDown::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            Paint p(h);
            HDC dc = p.dc();
            RECT r = p.rc();
            COLORREF bg = parentBg(h);
            fill(dc, r, bg);
            bool en = IsWindowEnabled(h) != 0;
            fillRound(dc, r, Sf(5), !en ? blend(P.field, P.win, 0.6f) : hover ? blend(P.field, P.hover, 0.6f) : P.field);
            strokeRound(dc, r, Sf(5), GetFocus() == h ? P.accent : P.fieldLine);
            int x = S(9);
            if (!iconName.empty()) { RECT ir{x, (r.bottom - S(15)) / 2, x + S(15), (r.bottom - S(15)) / 2 + S(15)}; icon(dc, iconName, ir, useIconColor ? iconColor : P.folder, 1.8f); x += S(22); }
            RECT tr{x, 0, r.right - S(26), r.bottom};
            ui::text(dc, value(), tr, IsWindowEnabled(h) ? P.text : P.muted, fontUI());
            RECT cr{r.right - S(22), (r.bottom - S(12)) / 2, r.right - S(10), (r.bottom - S(12)) / 2 + S(12)};
            icon(dc, "M6 9l6 6 6-6", cr, P.muted, 2.4f);
            return 0;
        }
        case WM_MOUSEMOVE: if (!hover) { hover = true; trackLeave(h); redraw(); } return 0;
        case WM_MOUSELEAVE: hover = false; redraw(); return 0;
        case WM_LBUTTONDOWN: case WM_KEYDOWN: {
            if (m == WM_KEYDOWN && w != VK_SPACE && w != VK_DOWN && w != VK_F4) break;
            SetFocus(h);
            std::vector<MenuItem> items;
            for (size_t i = 0; i < options.size(); i++) { MenuItem it; it.text = options[i]; it.id = static_cast<UINT>(i + 1); it.checked = static_cast<int>(i) == selected; items.push_back(it); }
            RECT r; GetWindowRect(h, &r);
            UINT c = popupMenu(h, items, POINT{r.left, r.bottom + S(2)});
            if (c) { selected = static_cast<int>(c) - 1; redraw(); if (onChange) onChange(selected); }
            return 0;
        }
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
        case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: redraw(); return 0;
    }
    return def(m, w, l);
}

// ---------------------------------------------------------------- Label, Bar
bool Label::create(HWND parent, const std::string& t) {
    text = t;
    return Wnd::create(L"DDMLabel", t, WS_CHILD | WS_VISIBLE, 0, 0, 0, 10, 10, parent);
}

LRESULT Label::proc(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_PAINT) {
        Paint p(h);
        RECT r = p.rc();
        COLORREF bg = background ? *background : parentBg(h);
        fill(p.dc(), r, bg);
        HFONT f = small ? (bold ? fontSmallBold() : fontSmall()) : (bold ? fontBold() : fontUI());
        UINT fl = (wrap ? DT_WORDBREAK : 0) | (right ? DT_RIGHT : DT_LEFT);
        ui::text(p.dc(), text, r, useColor ? color : P.text, f, fl);
        return 0;
    }
    return def(m, w, l);
}

bool Bar::create(HWND parent) { return Wnd::create(L"DDMBar", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 10, 10, parent); }

LRESULT Bar::proc(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        Paint p(h);
        RECT r = p.rc();
        COLORREF bg = parentBg(h);
        fill(p.dc(), r, bg);
        fillRound(p.dc(), r, Sf(4), P.track);
        strokeRound(p.dc(), r, Sf(4), P.line);
        RECT f = r;
        f.right = r.left + static_cast<LONG>((r.right - r.left) * std::max(0.0, std::min(1.0, value)));
        if (f.right > f.left + 1) fillRound(p.dc(), f, Sf(4), useColor ? color : P.accent);
        return 0;
    }
    return def(m, w, l);
}

void initWidgets() {
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&ic);
}

void tooltip(HWND owner, HWND tool, const std::string& t) {
    static std::map<HWND, HWND> tips;
    HWND& tip = tips[owner];
    if (!tip) tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    std::wstring w = W(i18n::tr(t));
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = reinterpret_cast<UINT_PTR>(tool);
    ti.lpszText = &w[0];
    SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

} // namespace ui
} // namespace ddm
