// A simple themed list: an optional header row, rows of text columns with an icon, one selection.
#pragma once
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace ui {

class RowList : public Wnd {
public:
    struct Item { std::string key, icon; COLORREF iconColor = 0; std::vector<std::string> cols; bool bold = false; };
    std::vector<Item> items;
    std::vector<std::string> headers;
    std::vector<int> widths;   // column widths (last column fills)
    std::vector<int> nameCols; // columns holding names (files, titles, queues): never partly translated
    int selected = -1, top = 0;
    std::function<void(int)> onSelect;
    std::function<void(int)> onOpen;
    std::function<void(POINT)> onContext;
    std::function<void(HDROP)> onDrop;

    bool create(HWND parent) { return Wnd::create(L"DDMRowList", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 10, 10, parent); }
    int rowH() const { return S(26); }
    int headH() const { return headers.empty() ? 0 : S(26); }
    void setItems(std::vector<Item> v) {
        std::string keep = selected >= 0 && selected < static_cast<int>(items.size()) ? items[static_cast<size_t>(selected)].key : "";
        items = std::move(v);
        selected = -1;
        for (size_t i = 0; i < items.size(); i++) if (items[i].key == keep) selected = static_cast<int>(i);
        updateScroll();
        redraw();
    }
    void select(int i) { selected = i; ensureVisible(); redraw(); if (onSelect) onSelect(i); }
    void ensureVisible() {
        RECT r = client();
        int vis = std::max<int>(1, (r.bottom - headH()) / rowH());
        if (selected >= 0 && selected < top) top = selected;
        if (selected >= top + vis) top = selected - vis + 1;
        updateScroll();
    }
    void updateScroll() {
        RECT r = client();
        int vis = std::max<int>(1, (r.bottom - headH()) / rowH());
        top = std::max(0, std::min(top, std::max(0, static_cast<int>(items.size()) - vis)));
        SCROLLINFO si{sizeof si, SIF_ALL, 0, std::max(0, static_cast<int>(items.size()) - 1), static_cast<UINT>(vis), top, 0};
        SetScrollInfo(h, SB_VERT, &si, TRUE);
    }

protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
            case WM_ERASEBKGND: return 1;
            case WM_SIZE: updateScroll(); return 0;
            case WM_PAINT: {
                Paint p(h);
                HDC dc = p.dc();
                RECT r = p.rc();
                fill(dc, r, P.field);
                int y = 0;
                if (!headers.empty()) {
                    RECT hr{0, 0, r.right, headH()};
                    fill(dc, hr, P.win);
                    int x = S(8);
                    for (size_t c = 0; c < headers.size(); c++) {
                        int cw = c + 1 < headers.size() && c < widths.size() ? widths[c] : r.right - x;
                        RECT tr{x, 0, x + cw - S(6), headH()};
                        ui::text(dc, headers[c], tr, P.muted, fontSmall());
                        x += cw;
                    }
                    RECT ln{0, headH() - 1, r.right, headH()};
                    fill(dc, ln, P.line);
                    y = headH();
                }
                for (int i = top; i < static_cast<int>(items.size()) && y < r.bottom; i++, y += rowH()) {
                    const Item& it = items[static_cast<size_t>(i)];
                    RECT row{0, y, r.right, y + rowH()};
                    if (i == selected) fill(dc, row, GetFocus() == h ? P.sel : blend(P.sel, P.field, 0.5f));
                    int x = S(8);
                    if (!it.icon.empty()) {
                        RECT ic{x, y + (rowH() - S(16)) / 2, x + S(16), y + (rowH() - S(16)) / 2 + S(16)};
                        if (it.icon.compare(0, 5, "type:") == 0) { InflateRect(&ic, S(2), S(2)); fileIcon(dc, ic, it.icon.substr(5)); }
                        else icon(dc, it.icon, ic, it.iconColor ? it.iconColor : P.folder, 1.7f);
                        x += S(22);
                    }
                    for (size_t c = 0; c < it.cols.size(); c++) {
                        int cw = c + 1 < it.cols.size() && c < widths.size() ? widths[c] - (c == 0 && !it.icon.empty() ? S(22) : 0) : r.right - x;
                        RECT tr{x, y, x + cw - S(6), y + rowH()};
                        bool asName = std::find(nameCols.begin(), nameCols.end(), static_cast<int>(c)) != nameCols.end();
                        (asName ? ui::textName : ui::text)(dc, it.cols[c], tr, P.text, it.bold && c == 0 ? fontBold() : fontUI(), DT_END_ELLIPSIS);
                        x += cw;
                    }
                }
                return 0;
            }
            case WM_RBUTTONUP: { int y = GET_Y_LPARAM(l) - headH(); int i = top + y / rowH(); if (y >= 0 && i >= 0 && i < static_cast<int>(items.size())) select(i); if (onContext) { POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(h, &pt); onContext(pt); } return 0; }
            case WM_DROPFILES: if (onDrop) onDrop(reinterpret_cast<HDROP>(w)); return 0;
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
                SetFocus(h);
                int y = GET_Y_LPARAM(l) - headH();
                if (y < 0) return 0;
                int i = top + y / rowH();
                if (i >= 0 && i < static_cast<int>(items.size())) { select(i); if (m == WM_LBUTTONDBLCLK && onOpen) onOpen(i); }
                return 0;
            }
            case WM_MOUSEWHEEL: top -= GET_WHEEL_DELTA_WPARAM(w) / 40; updateScroll(); redraw(); return 0;
            case WM_VSCROLL: {
                SCROLLINFO si{sizeof si, SIF_ALL};
                GetScrollInfo(h, SB_VERT, &si);
                switch (LOWORD(w)) {
                    case SB_LINEUP: top--; break; case SB_LINEDOWN: top++; break;
                    case SB_PAGEUP: top -= static_cast<int>(si.nPage); break; case SB_PAGEDOWN: top += static_cast<int>(si.nPage); break;
                    case SB_THUMBTRACK: case SB_THUMBPOSITION: top = si.nTrackPos; break;
                }
                updateScroll(); redraw(); return 0;
            }
            case WM_KEYDOWN:
                if (w == VK_UP && selected > 0) select(selected - 1);
                else if (w == VK_DOWN && selected + 1 < static_cast<int>(items.size())) select(selected + 1);
                else if (w == VK_RETURN && selected >= 0 && onOpen) onOpen(selected);
                return 0;
            case WM_GETDLGCODE: return DLGC_WANTARROWS;
            case WM_SETFOCUS: case WM_KILLFOCUS: redraw(); return 0;
        }
        return def(m, w, l);
    }
};


} // namespace ui
} // namespace ddm
