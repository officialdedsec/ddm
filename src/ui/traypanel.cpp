#include "traypanel.hpp"
#include "speedglide.hpp"
#include "app.hpp"
#include "dialogs.hpp"
#include "gfx.hpp"
#include "mainwnd.hpp"
#include "theme.hpp"
#include "../core/engine.hpp"
#include "../core/i18n.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <dwmapi.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace ddm {
namespace ui {

namespace {
const wchar_t* kClass = L"DDMTrayPanel";
const int kMaxRows = 7;

struct Row { std::string id, name; State state = State::Queued; bool stopped = false; double pct = -1, speed = 0; int64_t eta = -1, done = 0, total = 0; bool hidden = false; };

HWND g_panel = nullptr;
ULONGLONG g_closedAt = 0;   // (a click on the icon that closed it doesn't open it again)
std::vector<Row> g_rows;
int g_more = 0, g_running = 0, g_hidden = 0;
double g_speed = 0;
int g_hover = -1;           // 0..rows-1: a download; 100..102: the buttons at the bottom
int g_alpha = 0, g_slide = 0;

std::vector<Row> gather(int& more, int& running, int& hidden, double& speed) {
    std::set<std::string> inTray;
    for (auto& p : progressInTray()) inTray.insert(p.first);
    std::vector<Row> all;
    running = 0; speed = 0;
    for (auto& t : Engine::get().snapshot()) {
        bool active = t.state == State::Running || t.state == State::Queued;
        bool tray = inTray.count(t.id) != 0;
        if (!active && !tray) continue;
        if (t.state == State::Complete) continue;
        Row r;
        r.id = t.id; r.name = t.fileName; r.state = t.state; r.stopped = t.stopped; r.hidden = tray;
        r.pct = t.total > 0 ? t.progress() * 100 : -1;
        r.speed = t.state == State::Running ? t.speed : 0;
        r.eta = t.eta; r.done = t.done; r.total = t.total;
        if (t.state == State::Running) { running++; speed += t.speed; }
        all.push_back(r);
    }
    hidden = static_cast<int>(inTray.size());
    // downloading first, then waiting, then the rest; the fastest on top
    std::stable_sort(all.begin(), all.end(), [](const Row& a, const Row& b) {
        auto rank = [](const Row& r) { return r.state == State::Running ? 0 : r.state == State::Queued ? 1 : 2; };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a.speed > b.speed;
    });
    more = static_cast<int>(all.size()) > kMaxRows ? static_cast<int>(all.size()) - kMaxRows : 0;
    if (more) all.resize(kMaxRows);
    return all;
}

int headH() { return S(58); }
int rowH() { return S(60); }
int footH() { return S(50); }
int panelW() { return S(390); }
int panelH() {
    int rows = std::max<int>(1, static_cast<int>(g_rows.size()));
    return headH() + rows * rowH() + (g_more ? S(26) : 0) + footH();
}
RECT rowRect(int i) { return RECT{S(8), headH() + i * rowH(), panelW() - S(8), headH() + (i + 1) * rowH() - S(2)}; }
RECT footBtn(int k) {   // 0 Open DDM, 1 Show the windows, 2 Pause all / Resume all
    int w = (panelW() - S(16) - S(16)) / 3, y = panelH() - footH() + S(8);
    int x = S(8) + k * (w + S(8));
    return RECT{x, y, x + w, y + S(34)};
}

void place(HWND h, bool first) {
    int w = panelW(), hgt = panelH();
    static POINT at{};
    if (first) GetCursorPos(&at);
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    int gap = S(10), x = at.x - w / 2, y = at.y - hgt - S(16);
    if (at.y >= wa.bottom) y = wa.bottom - hgt - gap;           // taskbar at the bottom
    else if (at.y < wa.top) y = wa.top + gap;                    // at the top
    else if (at.x >= wa.right) { x = wa.right - w - gap; y = at.y - hgt / 2; }   // on the right
    else if (at.x < wa.left) { x = wa.left + gap; y = at.y - hgt / 2; }          // on the left
    x = std::max<int>(wa.left + gap, std::min<int>(x, wa.right - w - gap));
    y = std::max<int>(wa.top + gap, std::min<int>(y, wa.bottom - hgt - gap));
    SetWindowPos(h, HWND_TOPMOST, x, y + (first ? 0 : 0), w, hgt, SWP_NOACTIVATE);
}

void refresh(HWND h) {
    size_t before = g_rows.size();
    int moreBefore = g_more;
    g_rows = gather(g_more, g_running, g_hidden, g_speed);
    if (g_rows.size() != before || g_more != moreBefore) { place(h, false); }
    InvalidateRect(h, nullptr, FALSE);
}

std::string pctText(double p) { char b[16]; snprintf(b, sizeof b, "%.1f%%", p); return b; }

void paint(HWND h, HDC dc, RECT rc) {
    fill(dc, rc, P.win);
    RECT border = rc;
    HBRUSH line = CreateSolidBrush(P.line);
    FrameRect(dc, &border, line);
    DeleteObject(line);
    // the top: DDM, and what it's doing
    RECT lg{S(16), S(16), S(16) + S(26), S(16) + S(26)};
    logo(dc, lg);
    RECT t1{lg.right + S(12), S(12), rc.right - S(16), S(32)};
    text(dc, "DedSec Download Manager", t1, P.text, fontBold(), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT t2{lg.right + S(12), S(32), rc.right - S(16), S(50)};
    std::string sum = g_running ? std::to_string(g_running) + (g_running == 1 ? " downloading · " : " downloading · ") + formatKBps(glidedSpeed("*tray*", g_speed))
                    : g_rows.empty() ? "Nothing is downloading" : "Nothing is downloading right now";
    text(dc, sum, t2, g_running ? P.accent : P.muted, fontSmall(), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT sep{S(12), headH() - S(4), rc.right - S(12), headH() - S(3)};
    fill(dc, sep, P.line);
    if (g_rows.empty()) {
        RECT r{S(16), headH(), rc.right - S(16), headH() + rowH()};
        text(dc, "Your downloads appear here while they run.", r, P.muted, fontUI(), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    for (size_t i = 0; i < g_rows.size(); i++) {
        const Row& w = g_rows[i];
        RECT r = rowRect(static_cast<int>(i));
        if (g_hover == static_cast<int>(i)) fillRound(dc, r, Sf(8), P.hover);
        std::string right = w.pct >= 0 ? pctText(w.pct) : (w.done > 0 ? formatBytes(w.done) : "");
        HFONT nf = fontBold();
        SIZE rs = textSize(dc, right, nf);
        RECT nr{r.left + S(10), r.top + S(6), r.right - S(14) - rs.cx - S(8), r.top + S(26)};
        textName(dc, w.name, nr, P.text, nf, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT pr{r.right - S(10) - rs.cx - S(4), r.top + S(6), r.right - S(10), r.top + S(26)};
        COLORREF stc = w.state == State::Running ? P.accent : w.state == State::Error ? P.bad : P.warn;
        text(dc, right, pr, stc, nf, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        RECT bar{r.left + S(10), r.top + S(30), r.right - S(10), r.top + S(37)};
        COLORREF a1 = P.accent, a2 = blend(P.accent, RGB(255, 255, 255), 0.25f);
        if (w.state == State::Error) { a1 = RGB(220, 38, 38); a2 = RGB(248, 113, 113); }
        else if (w.state != State::Running) { a1 = RGB(234, 140, 8); a2 = RGB(251, 191, 36); }
        progressBar(dc, bar, w.pct >= 0 ? w.pct / 100.0 : 0, P.track, a1, a2, Sf(3.5f));
        std::string sub;
        // (each piece in your language; the whole line is never a sentence of its own)
        if (w.state == State::Running) sub = w.speed > 0 ? formatKBps(glidedSpeed(w.id, w.speed)) + (w.eta >= 0 ? " · " + i18n::tr(formatDuration(w.eta) + " left") : "") : i18n::tr("Connecting…");
        else if (w.state == State::Queued) sub = i18n::tr("Waiting");
        else if (w.state == State::Error) sub = i18n::tr("Error");
        else sub = i18n::tr(w.stopped ? "Stopped" : "Paused");
        if (w.total > 0) sub += " · " + formatBytes(w.done) + " / " + formatBytes(w.total);
        RECT sr{r.left + S(10), r.top + S(40), r.right - S(10), r.top + S(56)};
        if (w.hidden) {   // its window waits in the tray
            SIZE hs = textSize(dc, "In the tray", fontSmall());
            text(dc, "In the tray", sr, P.muted, fontSmall(), DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            sr.right -= hs.cx + S(10);
        }
        text(dc, sub, sr, P.muted, fontSmall(), DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    if (g_more) {
        RECT mr{S(18), headH() + static_cast<int>(g_rows.size()) * rowH(), rc.right - S(18), headH() + static_cast<int>(g_rows.size()) * rowH() + S(24)};
        text(dc, "+ " + std::to_string(g_more) + " more in DDM", mr, P.muted, fontSmall(), DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    RECT fs{S(12), rc.bottom - footH(), rc.right - S(12), rc.bottom - footH() + 1};
    fill(dc, fs, P.line);
    bool anyRun = g_running > 0;
    const char* labels[3] = {"Open DDM", "Show windows", anyRun ? "Pause all" : "Resume all"};
    for (int k = 0; k < 3; k++) {
        RECT b = footBtn(k);
        bool off = k == 1 && g_hidden == 0;
        if (!off && g_hover == 100 + k) fillRound(dc, b, Sf(8), P.hover);
        text(dc, labels[k], b, off ? blend(P.muted, P.win, 0.5f) : (k == 0 ? P.accent : P.text), k == 0 ? fontBold() : fontUI(), DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
}

int hitTest(POINT pt) {
    for (size_t i = 0; i < g_rows.size(); i++) { RECT r = rowRect(static_cast<int>(i)); if (PtInRect(&r, pt)) return static_cast<int>(i); }
    for (int k = 0; k < 3; k++) { RECT b = footBtn(k); if (PtInRect(&b, pt)) return 100 + k; }
    return -1;
}

void act(int what) {
    HWND main = App::get().mainHwnd();
    if (what >= 0 && what < static_cast<int>(g_rows.size())) { std::string id = g_rows[static_cast<size_t>(what)].id; closeTrayPanel(); App::get().showProgress(id); return; }
    if (what == 100) { closeTrayPanel(); App::get().restoreMain(); return; }
    if (what == 101) { if (g_hidden == 0) return; closeTrayPanel(); restoreProgressFromTray(); return; }
    if (what == 102) { PostMessageW(main, WM_COMMAND, g_running > 0 ? MainWindow::CmdPauseAll : MainWindow::CmdResumeAll, 0); return; }
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { Paint p(h); paint(h, p.dc(), p.rc()); return 0; }
        case WM_TIMER:
            if (w == 1) refresh(h);
            else if (w == 3) { if (g_running > 0) InvalidateRect(h, nullptr, FALSE); }
            else if (w == 2) {   // it fades in and rises a little
                g_alpha = std::min(255, g_alpha + 40);
                g_slide = std::max(0, g_slide - S(2));
                SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(g_alpha), LWA_ALPHA);
                place(h, false);
                if (g_slide > 0) { RECT n; GetWindowRect(h, &n); SetWindowPos(h, nullptr, n.left, n.top + g_slide, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
                if (g_alpha >= 255 && g_slide == 0) KillTimer(h, 2);
            }
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int hv = hitTest(pt);
            if (hv != g_hover) { g_hover = hv; InvalidateRect(h, nullptr, FALSE); }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            SetCursor(LoadCursor(nullptr, (hv >= 0 && hv != 101) || (hv == 101 && g_hidden) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
        case WM_MOUSELEAVE: if (g_hover != -1) { g_hover = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
        case WM_LBUTTONUP: { POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; act(hitTest(pt)); return 0; }
        case WM_KEYDOWN: if (w == VK_ESCAPE) { closeTrayPanel(); return 0; } break;
        case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) { g_closedAt = GetTickCount64(); PostMessageW(h, WM_CLOSE, 0, 0); } return 0;
        case WM_CLOSE: closeTrayPanel(); return 0;
        case WM_DESTROY: if (g_panel == h) g_panel = nullptr; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace

bool trayPanelWanted() {
    int more = 0, run = 0, hid = 0;
    double sp = 0;
    return !gather(more, run, hid, sp).empty();
}

void closeTrayPanel() {
    if (!g_panel) return;
    HWND h = g_panel;
    g_panel = nullptr;
    g_closedAt = GetTickCount64();
    DestroyWindow(h);
}

void toggleTrayPanel() {
    if (g_panel) { closeTrayPanel(); return; }
    if (GetTickCount64() - g_closedAt < 350) return;   // the click on the icon is what closed it
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.style = CS_DROPSHADOW;
        registered = RegisterClassW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    g_rows = gather(g_more, g_running, g_hidden, g_speed);
    g_hover = -1;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kClass, L"DDM", WS_POPUP, 0, 0, panelW(), panelH(), nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) return;
    g_panel = h;
    DWORD round = 2;   // Windows 11: rounded corners
    DwmSetWindowAttribute(h, 33, &round, sizeof round);
    g_alpha = 0;
    g_slide = S(12);
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    place(h, true);
    { RECT n; GetWindowRect(h, &n); SetWindowPos(h, nullptr, n.left, n.top + g_slide, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(h);
    SetTimer(h, 1, 500, nullptr);
    SetTimer(h, 3, 66, nullptr);    // the speeds glide between the engine's figures
    SetTimer(h, 2, 15, nullptr);
}

} // namespace ui
} // namespace ddm
