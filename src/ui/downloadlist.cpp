#include "downloadlist.hpp"
#include "speedglide.hpp"
#include "../core/torrent.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "dust.hpp"
#include "../core/settings.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <climits>

namespace ddm {
namespace ui {

void DownloadList::selectId(const std::string& id) {
    for (size_t i = 0; i < rows.size(); i++) if (rows[i].id == id) {
        selectOnly(id);
        ensureVisible(static_cast<int>(i));
        redraw();
        if (onSelectionChanged) onSelectionChanged();
        return;
    }
}

std::string statusText(const Task& t) {
    char b[64];
    switch (t.state) {
        case State::Complete:
            if (t.scanState == "scanning") return "Complete · scanning…";
            if (t.scanState == "threat") return "Threat found!";
            if (t.scanState == "removed") return "Removed by antivirus";
            if (t.verify == "bad") return "Damaged (checksum)";
            if (t.verify == "checking") return "Complete · checking checksum…";
            if (t.verify == "ok") return "Complete · verified";
            return t.kind == "torrent" && torrentSeeding(t.id) ? "Seeding" : "Complete";
        case State::Error: return "Error — " + (t.error.empty() ? std::string("failed") : t.error);
        case State::Queued: return t.waitReason.empty() ? "Queued" : t.waitReason;
        case State::Paused:
            if (t.scheduled) return t.total > 0 && t.done > 0 ? "Scheduled · " + std::to_string(static_cast<int>(t.progress() * 100)) + "%" : "Scheduled";
            if (t.total > 0 && t.done > 0) { snprintf(b, sizeof b, "%s %.1f%%", t.stopped ? "Stopped" : "Paused", t.progress() * 100); return b; }
            return t.stopped ? "Stopped" : "Paused";
        case State::Running:
            for (auto k : {"Joining", "Checking", "Preparing", "Getting", "Finishing", "Opening", "Trying", "Pausing"})
                if (startsWith(t.status, k)) return t.status;
            if (t.total > 0) { snprintf(b, sizeof b, "%.1f%%", t.progress() * 100); return b; }
            if (t.done > 0) return formatBytes(t.done);
            return t.status.empty() ? "Connecting…" : t.status;
    }
    return "";
}

std::string iconFor(const Task& t) {
    if (t.kind == "torrent") return "magnet";
    std::string c = t.category;
    if (c == "video") return "video";
    if (c == "music") return "music";
    if (c == "programs") return "program";
    if (c == "compressed") return "zipfile";
    if (c == "documents") return "document";
    std::string e = extOf(t.fileName);
    if (e == "jpg" || e == "jpeg" || e == "png" || e == "gif" || e == "webp" || e == "bmp" || e == "svg") return "image";
    return "file";
}

COLORREF iconColorFor(const Task& t) {
    bool d = isDark();
    std::string c = t.kind == "torrent" ? "torrent" : t.category;
    if (c == "video") return d ? rgb(0xB79BFF) : rgb(0x7C4DCC);
    if (c == "music") return d ? rgb(0xF58AC0) : rgb(0xC2457E);
    if (c == "programs") return d ? rgb(0x6FB2F2) : rgb(0x2C6FB0);
    if (c == "compressed") return d ? rgb(0xE7B84E) : rgb(0xB5790E);
    if (c == "documents") return d ? rgb(0xFF8F8F) : rgb(0xC42B2B);
    if (c == "torrent") return d ? rgb(0x6FCF85) : rgb(0x3B7D4A);
    return P.muted;
}

bool DownloadList::create(HWND parent) {
    rowH = S(38);
    headH = S(30);
    cols = {
        {"name", "File name", 300, false}, {"source", "Source", 150, false}, {"size", "Size", 82, true}, {"status", "Status", 140, false},
        {"left", "Time left", 96, false}, {"rate", "Transfer rate", 104, true}, {"last", "Last try", 118, false}, {"desc", "Description", 140, false},
    };
    const auto& cw = settings().columnWidths;
    for (size_t i = 0; i < cols.size(); i++) cols[i].width = S(i < cw.size() && cw[i] > 20 ? cw[i] : cols[i].width);
    sortKey = settings().sortColumn;
    sortDesc = settings().sortDesc;
    return Wnd::create(L"DDMList", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, 0, 0, 0, 10, 10, parent);
}

void DownloadList::saveLayout() {
    auto& s = settings();
    s.columnWidths.clear();
    for (auto& c : cols) s.columnWidths.push_back(MulDiv(c.width, 96, dpi()));
    s.sortColumn = sortKey;
    s.sortDesc = sortDesc;
}

void DownloadList::setItems(std::vector<Task> items) {
    rows = std::move(items);
    if (!vanishing.empty() && slideRows.empty()) {   // the removed rows are gone now
        bool still = false;
        for (auto& r : rows) if (vanishing.count(r.id)) still = true;
        if (!still) vanishing.clear();
    }
    sortRows();
    // forget selections of removed rows
    std::set<std::string> keep;
    for (auto& r : rows) if (selected.count(r.id)) keep.insert(r.id);
    selected.swap(keep);
    updateScroll();
    redraw();
}

void DownloadList::sortRows() {
    auto key = sortKey;
    bool desc = sortDesc;
    std::stable_sort(rows.begin(), rows.end(), [&](const Task& a, const Task& b) {
        int c = 0;
        if (key == "name") c = _stricmp(a.fileName.c_str(), b.fileName.c_str());
        else if (key == "source") c = a.site.compare(b.site);
        else if (key == "size") c = a.total < b.total ? -1 : a.total > b.total ? 1 : 0;
        else if (key == "status") c = static_cast<int>(a.state) - static_cast<int>(b.state);
        else if (key == "rate") c = a.speed < b.speed ? -1 : a.speed > b.speed ? 1 : 0;
        else if (key == "left") c = a.eta < b.eta ? -1 : a.eta > b.eta ? 1 : 0;
        else if (key == "last") c = a.lastTry < b.lastTry ? -1 : a.lastTry > b.lastTry ? 1 : 0;
        else if (key == "desc") c = a.description.compare(b.description);
        else c = a.created < b.created ? -1 : a.created > b.created ? 1 : 0;   // "added"
        return desc ? c > 0 : c < 0;
    });
}

std::vector<std::string> DownloadList::selection() const {
    std::vector<std::string> r;
    for (auto& t : rows) if (selected.count(t.id)) r.push_back(t.id);
    return r;
}

std::string DownloadList::focusedId() const { return focus; }

float DownloadList::slideLeft() const {
    if (slideRows.empty()) return 0;
    int el = static_cast<int>(GetTickCount() - slideStart) - slideHold;
    if (el <= 0) return 1;
    if (el >= slideMs) return 0;
    float t = static_cast<float>(el) / slideMs;
    return 1.0f - (1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t));   // ease out
}

void DownloadList::dustRemove(const std::vector<std::string>& ids) {
    if (!h || ids.empty() || !dust::enabled()) return;
    std::set<std::string> gone(ids.begin(), ids.end());
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right <= 0 || rc.bottom <= headH) return;
    // the rows' picture exactly as drawn (also when another window covers part of the list)
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rc.right; bi.bmiHeader.biHeight = -rc.bottom; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(h);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    ReleaseDC(h, screen);
    if (!bmp || !bits || !mem) { if (bmp) DeleteObject(bmp); if (mem) DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, bmp);
    paint(mem, rc);
    GdiFlush();
    // which rows are on screen, and the box around them
    int top = INT_MAX, bottom = INT_MIN, index = 0;
    std::vector<std::pair<int, int>> bands;
    for (int i = 0; i < static_cast<int>(rows.size()); i++) {
        if (!gone.count(rows[static_cast<size_t>(i)].id)) continue;
        int y = headH + i * rowH - scrollY;
        int y0 = std::max(y, headH), y1 = std::min(y + rowH, static_cast<int>(rc.bottom));
        if (y1 > y0) { bands.push_back({y0, y1}); top = std::min(top, y0); bottom = std::max(bottom, y1); }
        (void)index;
    }
    bool played = false;
    if (!bands.empty()) {
        int w = rc.right, hgt = bottom - top;
        std::vector<uint32_t> px(static_cast<size_t>(w) * hgt, 0);
        const uint32_t* src = static_cast<uint32_t*>(bits);
        for (auto& b : bands)
            for (int y = b.first; y < b.second; y++)
                for (int x = 0; x < w; x++) px[static_cast<size_t>(y - top) * w + x] = src[static_cast<size_t>(y) * w + x] | 0xFF000000u;
        POINT at{0, top};
        ClientToScreen(h, &at);
        played = dust::image(std::move(px), w, hgt, at, dust::durationMs());
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    // the rows go at once; the ones below wait while the dust starts, then slide up
    std::map<std::string, int> below;
    int removedAbove = 0;
    for (auto& t : rows) {
        if (gone.count(t.id)) { removedAbove++; continue; }
        if (removedAbove) below[t.id] = removedAbove;
    }
    vanishing = gone;
    slideRows = below;
    slideStart = GetTickCount();
    slideHold = played ? dust::durationMs() * 40 / 100 : 0;
    SetTimer(h, 0x5D1, 15, nullptr);
    InvalidateRect(h, nullptr, FALSE);
    UpdateWindow(h);
}

int DownloadList::rowAt(int y) const {
    if (y < headH) return -1;
    int i = (y - headH + scrollY) / rowH;
    return i >= 0 && i < static_cast<int>(rows.size()) ? i : -1;
}

int DownloadList::colEdgeAt(int x) const {
    int sx = 0;
    SCROLLINFO si{sizeof si, SIF_POS};
    GetScrollInfo(h, SB_HORZ, &si);
    sx = -si.nPos;
    for (size_t i = 0; i < cols.size(); i++) { sx += cols[i].width; if (abs(x - sx) <= S(4)) return static_cast<int>(i); }
    return -1;
}

int DownloadList::colAt(int x) const {
    SCROLLINFO si{sizeof si, SIF_POS};
    GetScrollInfo(h, SB_HORZ, &si);
    int sx = -si.nPos;
    for (size_t i = 0; i < cols.size(); i++) { if (x >= sx && x < sx + cols[i].width) return static_cast<int>(i); sx += cols[i].width; }
    return -1;
}

void DownloadList::updateScroll() {
    RECT r = client();
    int viewH = std::max(0, static_cast<int>(r.bottom) - headH);
    int maxY = std::max(0, contentH() - viewH);
    scrollY = std::max(0, std::min(scrollY, maxY));
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, contentH(), static_cast<UINT>(viewH), scrollY, 0};
    SetScrollInfo(h, SB_VERT, &si, TRUE);
    int total = 0;
    for (auto& c : cols) total += c.width;
    SCROLLINFO sh{sizeof sh, SIF_RANGE | SIF_PAGE, 0, total, static_cast<UINT>(r.right), 0, 0};
    SetScrollInfo(h, SB_HORZ, &sh, TRUE);
}

void DownloadList::ensureVisible(int row) {
    RECT r = client();
    int viewH = r.bottom - headH;
    int top = row * rowH;
    if (top < scrollY) scrollY = top;
    else if (top + rowH > scrollY + viewH) scrollY = top + rowH - viewH;
    updateScroll();
    redraw();
}

void DownloadList::paintRow(HDC dc, const Task& t, int y, int width, int index) {
    bool sel = selected.count(t.id) > 0;
    COLORREF bg = sel ? P.sel : (index == hoverRow ? P.hover : (index % 2 ? P.zebra : P.list));
    RECT rr{0, y, width, y + rowH};
    fill(dc, rr, bg);
    RECT ln{0, y + rowH - 1, width, y + rowH};
    fill(dc, ln, P.rowLine);
    SCROLLINFO si{sizeof si, SIF_POS};
    GetScrollInfo(h, SB_HORZ, &si);
    int x = -si.nPos;
    for (auto& c : cols) {
        RECT cell{x + S(10), y, x + c.width - S(10), y + rowH};
        if (c.key == "name") {
            cell.left = x + S(12);
            RECT ic{cell.left - S(2), y + (rowH - S(20)) / 2, cell.left + S(18), y + (rowH - S(20)) / 2 + S(20)};
            fileIcon(dc, ic, typeOf(t.category, t.fileName, t.kind));
            cell.left += S(24);
            textName(dc, t.fileName.empty() ? t.url : t.fileName, cell, P.text, fontUI());
        } else if (c.key == "source") {
            RECT ic{cell.left, y + (rowH - S(16)) / 2, cell.left + S(16), y + (rowH - S(16)) / 2 + S(16)};
            std::string site = t.kind == "torrent" && (isMagnet(t.url) || t.site.empty() || fileExists(t.url)) ? (isMagnet(t.url) ? "magnet link" : "torrent file") : t.site;
            if (!drawFavicon(dc, ic, t.site)) { siteBadge(dc, ic, site); App::get().wantFavicon(t.site, t.favicon); }
            cell.left += S(23);
            textName(dc, site, cell, P.muted, fontUI());
        } else if (c.key == "size") {
            text(dc, t.total >= 0 ? formatBytes(t.total) : (t.done > 0 ? formatBytes(t.done) : ""), cell, P.text, fontUI(), DT_RIGHT);
        } else if (c.key == "status") {
            std::string st = statusText(t);
            COLORREF col = t.state == State::Complete ? P.good : t.state == State::Error ? P.bad : t.state == State::Paused ? P.warn : P.text;
            if (t.scanState == "threat" || t.scanState == "removed" || t.verify == "bad") col = P.bad;
            bool bar = t.state != State::Complete;
            RECT tr = cell;
            if (bar) tr.bottom = y + rowH / 2 + S(4);
            text(dc, st, tr, col, fontUI(), DT_LEFT | (bar ? DT_BOTTOM : 0));
            if (bar) {
                RECT b{cell.left, y + rowH / 2 + S(8), cell.right, y + rowH / 2 + S(12)};
                fillRound(dc, b, Sf(2), P.track);
                RECT f = b;
                f.right = b.left + static_cast<LONG>((b.right - b.left) * t.progress());
                COLORREF fc = t.state == State::Paused ? P.warn : t.state == State::Error ? P.bad : P.accent;
                if (f.right > f.left + 1) fillRound(dc, f, Sf(2), fc);
            }
        } else if (c.key == "left") {
            text(dc, t.state == State::Running && t.eta >= 0 ? formatDuration(t.eta) : "", cell, P.muted, fontUI());
        } else if (c.key == "rate") {
            text(dc, t.state == State::Running ? formatKBps(glidedSpeed(t.id, t.speed)) : "", cell, P.text, fontUI(), DT_RIGHT);
        } else if (c.key == "last") {
            text(dc, formatDate(t.lastTry ? t.lastTry : t.created), cell, P.muted, fontUI());
        } else if (c.key == "desc") {
            textName(dc, t.description, cell, P.muted, fontUI());
        }
        x += c.width;
    }
}

void DownloadList::paint(HDC dc, RECT r) {
    fill(dc, r, P.list);
    SCROLLINFO si{sizeof si, SIF_POS};
    GetScrollInfo(h, SB_HORZ, &si);
    // rows
    if (vanishing.empty() && slideRows.empty()) {
        int first = scrollY / rowH;
        for (int i = first; i < static_cast<int>(rows.size()); i++) {
            int y = headH + i * rowH - scrollY;
            if (y > r.bottom) break;
            paintRow(dc, rows[static_cast<size_t>(i)], y, r.right, i);
        }
    } else {   // removing: the removed rows leave a gap, the rows below glide up into it
        float left = slideLeft();
        int shown = 0;
        for (int i = 0; i < static_cast<int>(rows.size()); i++) {
            const Task& t = rows[static_cast<size_t>(i)];
            if (vanishing.count(t.id)) continue;
            auto it = slideRows.find(t.id);
            int extra = it == slideRows.end() ? 0 : static_cast<int>(it->second * rowH * left + 0.5f);
            int y = headH + shown * rowH - scrollY + extra;
            shown++;
            if (y + rowH < headH || y > r.bottom) continue;
            paintRow(dc, t, y, r.right, i);
        }
    }
    if (rows.empty()) {
        RECT er{0, headH + S(40), r.right, headH + S(70)};
        text(dc, "No downloads here yet", er, P.muted, fontUI(), DT_CENTER);
        RECT er2{0, headH + S(66), r.right, headH + S(94)};
        text(dc, "Click Add URL, paste a link (Ctrl+V), or download something in your browser.", er2, P.muted, fontSmall(), DT_CENTER);
    }
    // header
    RECT hr{0, 0, r.right, headH};
    fill(dc, hr, P.head);
    RECT hl{0, headH - 1, r.right, headH};
    fill(dc, hl, P.line);
    int x = -si.nPos;
    for (auto& c : cols) {
        RECT cell{x + (c.key == "name" ? S(36) : S(10)), 0, x + c.width - S(10), headH};
        text(dc, c.title, cell, P.muted, fontSmall(), c.right ? DT_RIGHT : DT_LEFT);
        if (c.key == sortKey) {
            RECT ar{x + c.width - S(16), (headH - S(10)) / 2, x + c.width - S(6), (headH - S(10)) / 2 + S(10)};
            icon(dc, sortDesc ? "M6 9l6 6 6-6" : "M6 15l6-6 6 6", ar, P.muted, 2.2f);
        }
        RECT sep{x + c.width - 1, S(7), x + c.width, headH - S(7)};
        fill(dc, sep, P.line);
        x += c.width;
    }
}

LRESULT DownloadList::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: styleScroll(h); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { Paint p(h); paint(p.dc(), p.rc()); return 0; }
        case WM_TIMER:
            if (w == 0x5D1) {   // rows sliding up after a removal
                int el = static_cast<int>(GetTickCount() - slideStart);
                if (el > slideHold + slideMs) {
                    slideRows.clear();
                    // removed rows the engine hasn't taken out yet stay hidden until it does (setItems); never for long
                    bool still = false;
                    for (auto& t : rows) if (vanishing.count(t.id)) still = true;
                    if (!still || el > slideHold + slideMs + 3000) { vanishing.clear(); KillTimer(h, 0x5D1); }
                }
                redraw();
                return 0;
            }
            break;
        case WM_SIZE: updateScroll(); redraw(); return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(w)) {
                case SB_LINEUP: pos -= rowH; break;
                case SB_LINEDOWN: pos += rowH; break;
                case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = si.nMax; break;
            }
            scrollY = pos;
            updateScroll();
            redraw();
            return 0;
        }
        case WM_HSCROLL: {
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(h, SB_HORZ, &si);
            int pos = si.nPos;
            switch (LOWORD(w)) {
                case SB_LINELEFT: pos -= S(30); break;
                case SB_LINERIGHT: pos += S(30); break;
                case SB_PAGELEFT: pos -= static_cast<int>(si.nPage); break;
                case SB_PAGERIGHT: pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            si.fMask = SIF_POS;
            si.nPos = pos;
            SetScrollInfo(h, SB_HORZ, &si, TRUE);
            redraw();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            scrollY -= GET_WHEEL_DELTA_WPARAM(w) * rowH * 3 / WHEEL_DELTA;
            updateScroll();
            redraw();
            return 0;
        }
        case WM_SETCURSOR: {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if ((pt.y < headH && colEdgeAt(pt.x) >= 0) || dragCol >= 0) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            break;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (dragCol >= 0) {
                cols[static_cast<size_t>(dragCol)].width = std::max(S(40), dragStartW + x - dragStartX);
                updateScroll();
                redraw();
                return 0;
            }
            int r = rowAt(y);
            if (r != hoverRow) {
                hoverRow = r;
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                redraw();
            }
            return 0;
        }
        case WM_MOUSELEAVE: hoverRow = -1; redraw(); return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
            SetFocus(h);
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (y < headH) {
                if (m == WM_LBUTTONDOWN) {
                    int e = colEdgeAt(x);
                    if (e >= 0) { dragCol = e; dragStartX = x; dragStartW = cols[static_cast<size_t>(e)].width; SetCapture(h); }
                }
                return 0;
            }
            int r = rowAt(y);
            if (r < 0) { if (m == WM_LBUTTONDOWN) selected.clear(); redraw(); if (onSelectionChanged) onSelectionChanged(); return 0; }
            const std::string& id = rows[static_cast<size_t>(r)].id;
            bool ctrl = (w & MK_CONTROL) != 0, shift = (w & MK_SHIFT) != 0;
            if (m == WM_RBUTTONDOWN && selected.count(id)) { focus = id; }
            else if (ctrl) { if (selected.count(id)) selected.erase(id); else selected.insert(id); anchor = focus = id; }
            else if (shift && !anchor.empty()) {
                int a = -1;
                for (size_t i = 0; i < rows.size(); i++) if (rows[i].id == anchor) a = static_cast<int>(i);
                selected.clear();
                if (a < 0) a = r;
                for (int i = std::min(a, r); i <= std::max(a, r); i++) selected.insert(rows[static_cast<size_t>(i)].id);
                focus = id;
            } else selectOnly(id);
            redraw();
            if (onSelectionChanged) onSelectionChanged();
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            if (dragCol >= 0) { dragCol = -1; ReleaseCapture(); saveLayout(); return 0; }
            if (y < headH) {
                int c = colAt(x);
                if (c >= 0 && colEdgeAt(x) < 0) {
                    std::string k = cols[static_cast<size_t>(c)].key;
                    if (k == "name" || k == "source" || k == "size" || k == "status" || k == "left" || k == "rate" || k == "last" || k == "desc") {
                        if (sortKey == k) sortDesc = !sortDesc; else { sortKey = k; sortDesc = k == "size" || k == "rate" || k == "last"; }
                        sortRows();
                        saveLayout();
                        redraw();
                    }
                }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int r = rowAt(GET_Y_LPARAM(l));
            if (r >= 0 && onOpen) onOpen(rows[static_cast<size_t>(r)].id);
            return 0;
        }
        case WM_RBUTTONUP: {
            if (GET_Y_LPARAM(l) < headH) return 0;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            if (onContext) onContext(pt);
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (GET_X_LPARAM(l) == -1 && onContext) { RECT r; GetWindowRect(h, &r); onContext(POINT{r.left + S(40), r.top + headH + S(20)}); }
            return 0;
        }
        case WM_KEYDOWN: {
            if (w == VK_DELETE) { if (onDelete) onDelete(); return 0; }
            if (w == VK_RETURN) { if (!focus.empty() && onOpen) onOpen(focus); return 0; }
            if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { for (auto& t : rows) selected.insert(t.id); redraw(); if (onSelectionChanged) onSelectionChanged(); return 0; }
            if (w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT) {
                if (rows.empty()) return 0;
                int cur = 0;
                for (size_t i = 0; i < rows.size(); i++) if (rows[i].id == focus) cur = static_cast<int>(i);
                int page = std::max(1, (static_cast<int>(client().bottom) - headH) / rowH);
                if (w == VK_UP) cur--; else if (w == VK_DOWN) cur++; else if (w == VK_HOME) cur = 0; else if (w == VK_END) cur = static_cast<int>(rows.size()) - 1;
                else if (w == VK_PRIOR) cur -= page; else cur += page;
                cur = std::max(0, std::min(cur, static_cast<int>(rows.size()) - 1));
                selectOnly(rows[static_cast<size_t>(cur)].id);
                ensureVisible(cur);
                if (onSelectionChanged) onSelectionChanged();
                return 0;
            }
            break;
        }
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return def(m, w, l);
}

} // namespace ui
} // namespace ddm
