// The Torrent window (right-click a torrent → Torrent details…): General, Files (choose files and their priority),
// Trackers (add, remove, update now), Peers, and the torrent's own Options. Plus Tools → Create torrent….
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/settings.hpp"
#include "../core/torrent.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <ctime>
#include <thread>
#include <shlobj.h>

namespace ddm {
namespace ui {

namespace {

std::string speed(int bps) { return bps > 0 ? formatBytes(bps) + "/s" : "—"; }
std::string pct(double v) { char b[32]; snprintf(b, sizeof b, "%.1f%%", v * 100.0); return b; }
std::string when(int64_t t) {
    if (t <= 0) return "—";
    time_t tt = static_cast<time_t>(t);
    tm* x = localtime(&tt);
    if (!x) return "—";
    char b[64]; strftime(b, sizeof b, "%d %b %Y, %H:%M", x);
    return b;
}
std::string duration(int64_t s) {
    if (s <= 0) return "—";
    int64_t d = s / 86400, hh = s / 3600 % 24, m = s / 60 % 60;
    return d ? std::to_string(d) + "d " + std::to_string(hh) + "h" : hh ? std::to_string(hh) + "h " + std::to_string(m) + "m" : std::to_string(m) + "m " + std::to_string(s % 60) + "s";
}
int64_t folderSize(const std::string& dir) {
    int64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(W(joinPath(dir, "*")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += folderSize(joinPath(dir, U(n)));
        else total += (static_cast<int64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return total;
}
std::string prioName(int p) { return p == 0 ? "Don't download" : p <= 2 ? "Low" : p >= 6 ? "High" : "Normal"; }

// A painted table: columns, rows of text, an optional tick box in the first column, one selected row.
class Table : public Wnd {
public:
    struct Col { std::string title; int weight; bool right = false; };
    std::vector<Col> cols;
    std::vector<std::vector<std::string>> rows;
    std::vector<int> ticks;            // per row: -1 none, 0 off, 1 on (when ticked column)
    std::vector<COLORREF> tint;        // per row text colour (0 = normal)
    bool tickable = false;
    int top = 0, sel = -1;
    std::string empty = "Nothing here yet.";
    std::function<void(int row)> onTick;
    std::function<void(int row, POINT screen)> onMenu;
    bool create(HWND parent) { return Wnd::create(L"DDMTorTable", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 10, 10, parent); }
    int rowH() const { return S(28); }
    int headH() const { return S(28); }
    void set(std::vector<std::vector<std::string>> r, std::vector<int> t = {}, std::vector<COLORREF> c = {}) {
        rows = std::move(r); ticks = std::move(t); tint = std::move(c);
        if (sel >= static_cast<int>(rows.size())) sel = -1;
        updateScroll(); redraw();
    }
    void updateScroll() {
        RECT r = client();
        int vis = std::max<int>(1, (r.bottom - headH()) / rowH());
        top = std::max(0, std::min(top, std::max(0, static_cast<int>(rows.size()) - vis)));
        SCROLLINFO si{sizeof si, SIF_ALL, 0, std::max(0, static_cast<int>(rows.size()) - 1), static_cast<UINT>(vis), top, 0};
        SetScrollInfo(h, SB_VERT, &si, TRUE);
    }
    std::vector<RECT> colRects(const RECT& r, int y0, int y1) {
        int total = 0; for (auto& c : cols) total += c.weight;
        int x = tickable ? S(40) : S(12), w = r.right - x - S(8);
        std::vector<RECT> out;
        bool prevRight = false;
        for (auto& c : cols) {
            int cw = w * c.weight / std::max(1, total);
            out.push_back(RECT{x + (prevRight && !c.right ? S(24) : 0), y0, x + cw - S(10), y1});   // room after a right-aligned number
            x += cw; prevRight = c.right;
        }
        return out;
    }
    int rowAt(int y) { y -= headH(); if (y < 0) return -1; int i = top + y / rowH(); return i < static_cast<int>(rows.size()) ? i : -1; }
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
                RECT hr{0, 0, r.right, headH()};
                fill(dc, hr, P.win);
                auto hc = colRects(r, 0, headH());
                for (size_t i = 0; i < cols.size(); i++) text(dc, cols[i].title, hc[i], P.muted, fontSmall(), cols[i].right ? DT_RIGHT : DT_LEFT);
                RECT ln{0, headH() - 1, r.right, headH()};
                fill(dc, ln, P.line);
                if (rows.empty()) { RECT e{0, headH() + S(20), r.right, headH() + S(50)}; text(dc, empty, e, P.muted, fontUI(), DT_CENTER); }
                int y = headH();
                for (int i = top; i < static_cast<int>(rows.size()) && y < r.bottom; i++, y += rowH()) {
                    RECT row{0, y, r.right, y + rowH()};
                    if (i == sel) fill(dc, row, blend(P.sel, P.field, 0.55f));
                    if (tickable && i < static_cast<int>(ticks.size()) && ticks[static_cast<size_t>(i)] >= 0) {
                        int bs = S(16);
                        RECT box{S(14), y + (rowH() - bs) / 2, S(14) + bs, y + (rowH() - bs) / 2 + bs};
                        if (ticks[static_cast<size_t>(i)] == 1) { fillRound(dc, box, Sf(4), P.accent); RECT ck = box; InflateRect(&ck, -S(2), -S(2)); icon(dc, "check", ck, P.accentText, 2.6f); }
                        else strokeRound(dc, box, Sf(4), P.fieldLine);
                    }
                    auto rc = colRects(r, y, y + rowH());
                    COLORREF c = i < static_cast<int>(tint.size()) && tint[static_cast<size_t>(i)] ? tint[static_cast<size_t>(i)] : P.text;
                    for (size_t k = 0; k < cols.size() && k < rows[static_cast<size_t>(i)].size(); k++)
                        (k == 0 ? textName : text)(dc, rows[static_cast<size_t>(i)][k], rc[k], k == 0 ? c : P.muted, fontUI(), (cols[k].right ? DT_RIGHT : DT_LEFT) | DT_END_ELLIPSIS);
                }
                return 0;
            }
            case WM_LBUTTONDOWN: {
                SetFocus(h);
                int i = rowAt(GET_Y_LPARAM(l));
                sel = i;
                redraw();
                if (i >= 0 && tickable && GET_X_LPARAM(l) < S(40) && onTick) onTick(i);
                return 0;
            }
            case WM_LBUTTONDBLCLK: { int i = rowAt(GET_Y_LPARAM(l)); if (i >= 0 && tickable && onTick) onTick(i); return 0; }
            case WM_RBUTTONDOWN: {
                int i = rowAt(GET_Y_LPARAM(l));
                if (i < 0) return 0;
                sel = i; redraw();
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ClientToScreen(h, &pt);
                if (onMenu) onMenu(i, pt);
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
        }
        return def(m, w, l);
    }
};

const char* kTabs[] = {"General", "Files", "Trackers", "Peers", "Options"};

class TorrentWnd : public Dialog {
public:
    std::string id;
    int tab = 0;
    std::vector<RECT> tabRects;
    bt::Details d;
    bt::Options opt;
    // Files
    Table files;
    Button fAll, fNone, fHigh, fNormal, fLow;
    // Trackers
    Table trackers;
    Field newTrackers;
    Button tAdd, tRemove, tUpdate;
    // Peers
    Table peers;
    // Options
    Label lUp, lPeers, lRatio, lMinutes, lKeep, oNote;
    Field up, maxPeers, ratio, minutes;
    DropDown keep;
    CheckBox seq, firstLast, superSeed;
    Button save, recheckBtn, shareBtn;
    // General
    Button openFolder, copyMagnet;
    Button closeBtn;

    void build() {
        opt = bt::options(id);
        files.create(h); files.tickable = true;
        files.cols = {{"Name", 6}, {"Size", 2, true}, {"Done", 2, true}, {"Priority", 2}};
        files.empty = "The file list arrives when the torrent's info does (a magnet link needs peers first).";
        files.onTick = [this](int i) { if (i < static_cast<int>(d.files.size())) setPriority({i}, d.files[static_cast<size_t>(i)].priority == 0 ? 4 : 0); };
        files.onMenu = [this](int i, POINT pt) {
            std::vector<MenuItem> v = {{"High", "", 7}, {"Normal", "", 4}, {"Low", "", 1}, MenuItem::sep(), {"Don't download", "", 100}};
            UINT c = popupMenu(h, v, pt);
            if (c) setPriority({i}, c == 100 ? 0 : static_cast<int>(c));
        };
        fAll.create(h, "Download all"); fNone.create(h, "Download none");
        fHigh.create(h, "High"); fNormal.create(h, "Normal"); fLow.create(h, "Low");
        fAll.onClick = [this] { setPriority(allFiles(), 4); };
        fNone.onClick = [this] { setPriority(allFiles(), 0); };
        fHigh.onClick = [this] { if (files.sel >= 0) setPriority({files.sel}, 7); };
        fNormal.onClick = [this] { if (files.sel >= 0) setPriority({files.sel}, 4); };
        fLow.onClick = [this] { if (files.sel >= 0) setPriority({files.sel}, 1); };
        trackers.create(h);
        trackers.cols = {{"Tracker", 6}, {"Status", 5}, {"Seeds", 1, true}, {"Peers", 1, true}};
        newTrackers.create(h, "", true);
        newTrackers.hint("Tracker addresses to add, one per line");
        tAdd.create(h, "Add trackers"); tRemove.create(h, "Remove selected"); tUpdate.create(h, "Update now");
        tAdd.onClick = [this] {
            int n = 0;
            for (auto l : split(replaceAll(newTrackers.get(), "\r", ""), '\n')) {
                l = trim(l);
                if (l.find("://") == std::string::npos) continue;
                if (std::find(opt.trackers.begin(), opt.trackers.end(), l) == opt.trackers.end()) opt.trackers.push_back(l);
                opt.removedTrackers.erase(std::remove(opt.removedTrackers.begin(), opt.removedTrackers.end(), l), opt.removedTrackers.end());
                n++;
            }
            if (!n) { askBox(h, "Trackers", "Type tracker addresses (like udp://tracker.example.org:1337/announce), one per line.", {"OK"}, 0); return; }
            bt::setOptions(id, opt);
            newTrackers.set("");
            refresh();
        };
        tRemove.onClick = [this] {
            if (trackers.sel < 0 || trackers.sel >= static_cast<int>(d.trackers.size())) return;
            std::string url = d.trackers[static_cast<size_t>(trackers.sel)].url;
            opt.trackers.erase(std::remove(opt.trackers.begin(), opt.trackers.end(), url), opt.trackers.end());
            if (std::find(opt.removedTrackers.begin(), opt.removedTrackers.end(), url) == opt.removedTrackers.end()) opt.removedTrackers.push_back(url);
            bt::setOptions(id, opt);
            refresh();
        };
        tUpdate.onClick = [this] { bt::reannounce(id); refresh(); };
        peers.create(h);
        peers.cols = {{"Address", 4}, {"Program", 4}, {"Has", 2, true}, {"Down", 2, true}, {"Up", 2, true}, {"How", 4}};
        peers.empty = "No peers connected right now.";
        // Options
        lUp.create(h, "Upload limit (KB/s, 0 = none)"); up.create(h, std::to_string(opt.uploadKBps));
        lPeers.create(h, "Maximum peers (0 = as in Options)"); maxPeers.create(h, std::to_string(opt.maxPeers));
        lRatio.create(h, "Stop sharing at ratio (empty = as in Options)"); ratio.create(h, opt.ratioPct >= 0 ? std::to_string(opt.ratioPct / 100) + "." + std::to_string(opt.ratioPct % 100 / 10) : "");
        lMinutes.create(h, "Stop sharing after minutes (empty = as in Options)"); minutes.create(h, opt.seedMinutes >= 0 ? std::to_string(opt.seedMinutes) : "");
        lKeep.create(h, "Keep sharing after finishing");
        keep.create(h, {"As in Options", "Yes", "No"}, opt.keepSeeding == 1 ? 1 : opt.keepSeeding == 0 ? 2 : 0);
        for (Label* l : {&lUp, &lPeers, &lRatio, &lMinutes, &lKeep}) l->setColor(P.muted);
        seq.create(h, "Download in order (watch a video while it downloads)", opt.sequential);
        firstLast.create(h, "First and last pieces of each file first", opt.firstLast);
        superSeed.create(h, "Super-seeding (only when you are the first to share it)", opt.superSeed);
        oNote.create(h, "Speed limit for downloading: the download window's Speed limiter.");
        oNote.small = true; oNote.setColor(P.muted);
        save.create(h, "Save", BtnStyle::Primary);
        recheckBtn.create(h, "Check files again");
        shareBtn.create(h, "Share (seed)");
        save.onClick = [this] { saveOptions(); };
        recheckBtn.onClick = [this] {
            if (!bt::recheck(id)) askBox(h, "Check files again", "Start the torrent (or share it) first; DDM then checks every piece on the disk.", {"OK"}, 0);
            refresh();
        };
        shareBtn.onClick = [this] {
            if (torrentSeeding(id)) { std::string x = id; std::thread([x] { stopSeeding(x); Engine::get().edit(x, [](Task&) {}); }).detach(); }
            else startSeedingNow(id);
            SetTimer(h, 2, 1500, nullptr);
        };
        openFolder.create(h, "Open folder");
        openFolder.onClick = [this] { App::get().openFolder(id); };
        copyMagnet.create(h, "Copy magnet link");
        copyMagnet.onClick = [this] {
            if (d.hash.empty()) return;
            std::string m = "magnet:?xt=urn:btih:" + d.hash + "&dn=" + urlEncodeName(d.name);
            for (auto& t : d.trackers) m += "&tr=" + urlEncodeName(t.url);
            setClipboardText(h, m);
        };
        closeBtn.create(h, "Close");
        closeBtn.onClick = [this] { close(0); };
        onEnter = [this] { if (tab == 4) saveOptions(); };
        showTab(0);
        refresh();
        SetTimer(h, 1, 1000, nullptr);
    }

    static std::string urlEncodeName(const std::string& v) {
        std::string o;
        for (unsigned char c : v) { if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') o += static_cast<char>(c); else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; } }
        return o;
    }
    static void setClipboardText(HWND owner, const std::string& s) {
        std::wstring w = W(s);
        if (!OpenClipboard(owner)) return;
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
        if (g) { memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
        CloseClipboard();
    }

    std::vector<int> allFiles() { std::vector<int> v; for (size_t i = 0; i < d.files.size(); i++) v.push_back(static_cast<int>(i)); return v; }
    void setPriority(const std::vector<int>& which, int p) {
        if (d.files.empty()) return;
        opt.priorities.resize(d.files.size(), 4);
        for (size_t i = 0; i < d.files.size(); i++) if (static_cast<int>(i) < static_cast<int>(opt.priorities.size()) && opt.priorities[i] < 0) opt.priorities[i] = 4;
        for (size_t i = 0; i < d.files.size(); i++) if (opt.priorities[i] != d.files[i].priority && !opt.set) opt.priorities[i] = d.files[i].priority;
        for (int i : which) if (i >= 0 && i < static_cast<int>(opt.priorities.size())) opt.priorities[static_cast<size_t>(i)] = p;
        bool any = std::any_of(opt.priorities.begin(), opt.priorities.end(), [](int x) { return x > 0; });
        if (!any && askBox(h, "Files", "Nothing would be downloaded. Keep at least one file?", {"Keep the last choice", "Download none"}, 0) == 0) { opt = bt::options(id); return; }
        bt::setOptions(id, opt);
        opt = bt::options(id);
        // the list's size follows what you chose
        int64_t wanted = 0;
        for (size_t i = 0; i < d.files.size(); i++) if (opt.priorities[i] > 0) wanted += d.files[i].size;
        Engine::get().edit(id, [wanted](Task& t) { if (t.state != State::Complete && wanted > 0) t.total = wanted; });
        refresh();
    }

    void saveOptions() {
        opt.uploadKBps = std::max(0, atoi(up.get().c_str()));
        opt.maxPeers = std::max(0, std::min(2000, atoi(maxPeers.get().c_str())));
        std::string r = trim(replaceAll(ratio.get(), ",", "."));
        opt.ratioPct = r.empty() ? -1 : std::max(0, static_cast<int>(atof(r.c_str()) * 100 + 0.5));
        std::string mi = trim(minutes.get());
        opt.seedMinutes = mi.empty() ? -1 : std::max(0, atoi(mi.c_str()));
        opt.keepSeeding = keep.selected == 1 ? 1 : keep.selected == 2 ? 0 : -1;
        opt.sequential = seq.checked; opt.firstLast = firstLast.checked; opt.superSeed = superSeed.checked;
        bt::setOptions(id, opt);
        opt = bt::options(id);
        save.setText("Saved");
        SetTimer(h, 3, 1500, nullptr);
    }

    void refresh() {
        bt::details(id, d, tab == 3);
        Task t;
        if (Engine::get().find(id, t)) SetWindowTextW(h, W(i18n::tr("Torrent") + " — " + (d.name.empty() ? t.fileName : d.name)).c_str());
        if (tab == 1) {
            std::vector<std::vector<std::string>> rows;
            std::vector<int> ticks;
            std::vector<COLORREF> tint;
            for (auto& f : d.files) {
                double p = f.size > 0 ? static_cast<double>(f.done) / static_cast<double>(f.size) : 1.0;
                rows.push_back({f.path, formatBytes(f.size), d.live || f.done ? pct(p) : "—", prioName(f.priority)});
                ticks.push_back(f.priority > 0 ? 1 : 0);
                tint.push_back(f.priority == 0 ? P.muted : 0);
            }
            files.set(rows, ticks, tint);
        } else if (tab == 2) {
            std::vector<std::vector<std::string>> rows;
            std::vector<COLORREF> tint;
            for (auto& x : d.trackers) {
                rows.push_back({x.url, x.status, x.seeds >= 0 ? std::to_string(x.seeds) : "—", x.peers >= 0 ? std::to_string(x.peers) : "—"});
                tint.push_back(x.working ? P.good : 0);
            }
            trackers.empty = d.live ? "No trackers: peers come from DHT and peer exchange." : "Start the torrent to see its trackers working.";
            trackers.set(rows, {}, tint);
        } else if (tab == 3) {
            std::vector<std::vector<std::string>> rows;
            for (auto& p : d.peerList) rows.push_back({p.address, p.client.empty() ? "—" : p.client, pct(p.progress), speed(p.down), speed(p.up), p.flags});
            peers.empty = d.live ? "No peers connected right now." : "Start the torrent to see who it's connected to.";
            peers.set(rows);
        } else if (tab == 4) {
            shareBtn.setText(torrentSeeding(id) ? "Stop sharing" : "Share (seed)");
            EnableWindow(shareBtn.hwnd(), t.state == State::Complete);
        }
        InvalidateRect(h, nullptr, FALSE);
    }

    void showTab(int i) {
        tab = i;
        auto vis = [](std::initializer_list<Wnd*> ws, bool on) { for (auto* w : ws) w->show(on); };
        vis({&files, &fAll, &fNone, &fHigh, &fNormal, &fLow}, i == 1);
        vis({&trackers, &newTrackers, &tAdd, &tRemove, &tUpdate}, i == 2);
        vis({&peers}, i == 3);
        vis({&lUp, &up, &lPeers, &maxPeers, &lRatio, &ratio, &lMinutes, &minutes, &lKeep, &keep, &seq, &firstLast, &superSeed, &oNote, &save, &recheckBtn, &shareBtn}, i == 4);
        vis({&openFolder, &copyMagnet}, i == 0);
        layout();
        refresh();
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void layout() override {
        RECT r = client();
        int pad = S(16), top = S(46) + S(14), bottom = r.bottom - S(56);
        int bw = std::max(S(100), closeBtn.preferredWidth());
        closeBtn.move(r.right - pad - bw, r.bottom - S(48), bw, S(34));
        openFolder.move(pad, r.bottom - S(48), std::max(S(120), openFolder.preferredWidth()), S(34));
        copyMagnet.move(pad + std::max(S(120), openFolder.preferredWidth()) + S(8), r.bottom - S(48), std::max(S(150), copyMagnet.preferredWidth()), S(34));
        // Files
        files.move(pad, top, r.right - 2 * pad, bottom - top - S(44));
        int x = pad, by = bottom - S(36);
        for (Button* b : {&fAll, &fNone, &fHigh, &fNormal, &fLow}) { int w = std::max(S(80), b->preferredWidth()); b->move(x, by, w, S(32)); x += w + S(8); }
        // Trackers
        trackers.move(pad, top, r.right - 2 * pad, bottom - top - S(130));
        newTrackers.move(pad, bottom - S(122), r.right - 2 * pad, S(78));
        x = pad;
        for (Button* b : {&tAdd, &tRemove, &tUpdate}) { int w = std::max(S(110), b->preferredWidth()); b->move(x, by, w, S(32)); x += w + S(8); }
        // Peers
        peers.move(pad, top, r.right - 2 * pad, bottom - top);
        // Options
        int lw = S(330), fw = S(120), y = top + S(4);
        for (auto pr : std::vector<std::pair<Label*, Wnd*>>{{&lUp, &up}, {&lPeers, &maxPeers}, {&lRatio, &ratio}, {&lMinutes, &minutes}}) {
            pr.first->move(pad, y + S(5), lw, S(22)); pr.second->move(pad + lw, y, fw, S(30)); y += S(40);
        }
        lKeep.move(pad, y + S(5), lw, S(22)); keep.move(pad + lw, y, S(180), S(32)); y += S(44);
        seq.move(pad, y, r.right - 2 * pad, S(24)); y += S(30);
        firstLast.move(pad, y, r.right - 2 * pad, S(24)); y += S(30);
        superSeed.move(pad, y, r.right - 2 * pad, S(24)); y += S(34);
        oNote.move(pad, y, r.right - 2 * pad, S(20));
        x = pad;
        for (Button* b : {&save, &recheckBtn, &shareBtn}) { int w = std::max(S(110), b->preferredWidth()); b->move(x, by, w, S(32)); x += w + S(8); }
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(14);
        tabRects.clear();
        int x = pad;
        RECT base{0, S(46) - 1, r.right, S(46)};
        fill(dc, base, P.line);
        for (int k = 0; k < 5; k++) {
            bool on = k == tab;
            SIZE s = textSize(dc, kTabs[k], on ? fontBold() : fontUI());
            RECT tr{x, S(14), x + s.cx + S(28), S(46)};
            if (on) {
                fillRound(dc, RECT{tr.left, tr.top, tr.right, tr.bottom + S(7)}, Sf(6), P.line);
                fillRound(dc, RECT{tr.left + 1, tr.top + 1, tr.right - 1, tr.bottom + S(7)}, Sf(6), P.win);
            }
            std::string label = kTabs[k];
            if (k == 1 && !d.files.empty()) label += " (" + std::to_string(d.files.size()) + ")";
            if (k == 3 && d.live) label += " (" + std::to_string(d.seeds + d.peers) + ")";
            if (label != kTabs[k]) { SIZE s2 = textSize(dc, label, on ? fontBold() : fontUI()); tr.right = tr.left + s2.cx + S(28); }
            text(dc, label, tr, on ? P.text : P.muted, on ? fontBold() : fontUI(), DT_CENTER);
            tabRects.push_back(tr);
            x = tr.right + S(2);
        }
        if (tab != 0) return;
        int px = S(18), y = S(46) + S(16), lw = S(170);
        auto row = [&](const std::string& k, const std::string& v, COLORREF c = 0) {
            RECT a{px, y, px + lw, y + S(22)}, b{px + lw + S(10), y, r.right - px, y + S(22)};
            text(dc, k, a, P.muted, fontUI());
            text(dc, v.empty() ? "—" : v, b, c ? c : P.text, fontUI(), DT_END_ELLIPSIS);
            y += S(26);
        };
        double prog = d.wanted > 0 ? static_cast<double>(d.done) / static_cast<double>(d.wanted) : 0;
        row("Name", d.name);
        row("State", d.error.empty() ? d.state : d.error, d.error.empty() ? 0 : P.bad);
        row("Progress", formatBytes(d.done) + " of " + formatBytes(d.wanted) + " (" + pct(prog) + ")" + (d.wanted < d.total ? " · whole torrent " + formatBytes(d.total) : ""));
        if (d.live) row("Speed", "Down " + speed(d.down) + " · Up " + speed(d.up));
        double ratioV = d.wanted > 0 ? static_cast<double>(d.uploaded) / static_cast<double>(d.wanted) : 0;
        char rb[32]; snprintf(rb, sizeof rb, "%.2f", ratioV);
        row("Shared", formatBytes(d.uploaded) + " sent · ratio " + rb);
        if (d.live) {
            row("Seeds / peers", std::to_string(d.seeds) + " / " + std::to_string(d.peers) + " connected" +
                (d.seedsInSwarm >= 0 ? " · " + std::to_string(d.seedsInSwarm) + " / " + std::to_string(std::max(0, d.peersInSwarm)) + " in the swarm" : ""));
            char av[32]; snprintf(av, sizeof av, "%.2f", d.availability);
            row("Availability", av);
            row("Time", "active " + duration(d.activeSeconds) + (d.seedingSeconds > 0 ? " · sharing " + duration(d.seedingSeconds) : ""));
        }
        row("Saved in", d.savePath);
        row("Pieces", d.pieces ? std::to_string(d.pieces) + " × " + formatBytes(d.pieceSize) + " (have " + std::to_string(d.piecesHave) + ")" : "");
        row("Info hash", d.hash + (d.v2 ? " · BitTorrent v2" : ""));
        row("Added / finished", when(d.added) + " / " + when(d.completed));
        row("Created", when(d.created) + (d.creator.empty() ? "" : " by " + d.creator) + (d.isPrivate ? " · private torrent (no DHT, no peer exchange)" : ""));
        if (!d.comment.empty()) row("Comment", d.comment);
        // the pieces as a bar: what is here, what is missing
        if (!d.pieceMap.empty()) {
            y += S(8);
            RECT bar{px, y, r.right - px, y + S(18)};
            fillRound(dc, bar, Sf(4), P.track);
            int w = bar.right - bar.left, n = static_cast<int>(d.pieceMap.size());
            for (int cx = 0; cx < w; cx++) {
                int a = static_cast<int>(static_cast<int64_t>(cx) * n / w), b = std::max(a + 1, static_cast<int>(static_cast<int64_t>(cx + 1) * n / w));
                int have = 0;
                for (int i = a; i < b && i < n; i++) if (d.pieceMap[static_cast<size_t>(i)] == '1') have++;
                if (have) { RECT c{bar.left + cx, bar.top, bar.left + cx + 1, bar.bottom}; fill(dc, c, have * 2 >= b - a ? P.good : blend(P.good, P.track, 0.5f)); }
            }
        }
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_TIMER) {
            if (w == 1) refresh();
            if (w == 2) { KillTimer(h, 2); refresh(); }
            if (w == 3) { KillTimer(h, 3); save.setText("Save"); }
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (size_t i = 0; i < tabRects.size(); i++) if (PtInRect(&tabRects[i], pt)) { showTab(static_cast<int>(i)); return 0; }
        }
        if (m == WM_DESTROY) KillTimer(h, 1);
        return Dialog::proc(m, w, l);
    }
};

// ------------------------------------------------------------------ Create torrent
class CreateWnd : public Dialog {
public:
    Label lSrc, lTrackers, lSeeds, lComment, lVer, lPiece, status;
    Field src, trk, seeds, comment;
    Button pickFileBtn, pickDirBtn, create, cancel;
    DropDown ver, piece;
    CheckBox priv, startSeed;
    Bar bar;
    std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
    bool busy = false;

    void build(const std::string& start) {
        lSrc.create(h, "File or folder to share");
        src.create(h, start);
        pickFileBtn.create(h, "File…"); pickDirBtn.create(h, "Folder…");
        pickFileBtn.onClick = [this] { std::string f = pickFile(h, "All files|*.*"); if (!f.empty()) src.set(f); };
        pickDirBtn.onClick = [this] { std::string f = pickFolder(h, src.get()); if (!f.empty()) src.set(f); };
        lTrackers.create(h, "Trackers (one per line; an empty line starts a backup group)");
        trk.create(h, replaceAll(settings().btExtraTrackers, "\n", "\r\n"), true);
        lSeeds.create(h, "Web seeds (optional: web addresses that have the same files)");
        seeds.create(h, "", true);
        lComment.create(h, "Comment");
        comment.create(h, "");
        lVer.create(h, "Type");
        ver.create(h, {"BitTorrent v1 (works in every program)", "Hybrid v1 + v2", "BitTorrent v2 only"}, 0);
        lPiece.create(h, "Piece size");
        piece.create(h, {"Automatic", "64 KB", "256 KB", "1 MB", "4 MB", "16 MB"}, 0);
        priv.create(h, "Private torrent (only its trackers: no DHT, no peer exchange)", false);
        startSeed.create(h, "Start sharing it in DDM when it's made", true);
        status.create(h, "");
        status.setColor(P.muted);
        bar.create(h);
        create.create(h, "Create", BtnStyle::Primary, "magnet");
        cancel.create(h, "Close");
        for (Label* l : {&lSrc, &lTrackers, &lSeeds, &lComment, &lVer, &lPiece}) l->setColor(P.muted);
        create.onClick = [this] { go(); };
        cancel.onClick = [this] { if (busy) stop->store(true); else close(-1); };
        layout();
    }

    void go() {
        if (busy) return;
        bt::CreateRequest r;
        r.source = trim(src.get());
        while (r.source.size() > 3 && (r.source.back() == '\\' || r.source.back() == '/')) r.source.pop_back();
        if (r.source.empty() || (!fileExists(r.source) && !dirExists(r.source))) { askBox(h, "Create torrent", "Choose the file or folder to share.", {"OK"}, 0); return; }
        for (auto l : split(replaceAll(trk.get(), "\r", ""), '\n')) r.trackers.push_back(trim(l));
        for (auto l : split(replaceAll(seeds.get(), "\r", ""), '\n')) if (!trim(l).empty()) r.webSeeds.push_back(trim(l));
        r.comment = trim(comment.get());
        r.isPrivate = priv.checked;
        r.version = ver.selected == 1 ? "hybrid" : ver.selected == 2 ? "v2" : "v1";
        static const int kb[] = {0, 64, 256, 1024, 4096, 16384};
        r.pieceKB = kb[std::max(0, std::min(5, piece.selected))];
        std::string name = baseName(r.source);
        std::string out = pickFile(h, "Torrent files (*.torrent)|*.torrent", true, name + ".torrent");
        if (out.empty()) return;
        if (lower(extOf(out)) != "torrent") out += ".torrent";
        r.out = out;
        busy = true;
        stop->store(false);
        create.setText("Working…");
        EnableWindow(create.hwnd(), FALSE);
        cancel.setText("Stop");
        status.setText("Reading the files…");
        HWND hw = h;
        auto st = stop;
        bool seed = startSeed.checked;
        std::thread([hw, r, st, seed] {
            std::string magnet, err;
            bool ok = bt::createTorrent(r, [hw, st](int done, int total) {
                if (IsWindow(hw)) PostMessageW(hw, WM_APP + 31, static_cast<WPARAM>(done), static_cast<LPARAM>(total));
                return !st->load();
            }, magnet, err);
            if (ok && seed) {   // shared from where the files are: a finished torrent in the list
                Task t;
                t.kind = "torrent";
                t.url = r.out;
                t.fileName = baseName(r.source);
                t.dir = dirName(r.source);
                t.category = "torrent";
                std::string id = Engine::get().add(t, false);
                if (!id.empty()) {
                    int64_t size = fileExists(r.source) ? fileSize(r.source) : folderSize(r.source);
                    Engine::get().edit(id, [size](Task& x) { x.state = State::Complete; x.total = size; x.done = size; x.finished = nowMs(); });
                    bt::Options o = bt::options(id); o.keepSeeding = 1; bt::setOptions(id, o);
                    startSeedingNow(id);
                }
            }
            auto* res = new std::pair<bool, std::string>(ok, ok ? r.out + "\n" + magnet : err);
            if (!IsWindow(hw) || !PostMessageW(hw, WM_APP + 32, 0, reinterpret_cast<LPARAM>(res))) delete res;
        }).detach();
    }

    void layout() override {
        RECT r = client();
        int pad = S(18), y = S(16), w = r.right - 2 * pad;
        lSrc.move(pad, y, w, S(22)); y += S(24);
        int bw = S(90);
        src.move(pad, y, w - 2 * bw - S(16), S(30));
        pickFileBtn.move(r.right - pad - 2 * bw - S(8), y, bw, S(30));
        pickDirBtn.move(r.right - pad - bw, y, bw, S(30)); y += S(42);
        lTrackers.move(pad, y, w, S(22)); y += S(24);
        trk.move(pad, y, w, S(96)); y += S(106);
        lSeeds.move(pad, y, w, S(22)); y += S(24);
        seeds.move(pad, y, w, S(48)); y += S(58);
        lComment.move(pad, y, w, S(22)); y += S(24);
        comment.move(pad, y, w, S(30)); y += S(42);
        lVer.move(pad, y + S(5), S(110), S(22)); ver.move(pad + S(110), y, S(330), S(32)); y += S(40);
        lPiece.move(pad, y + S(5), S(110), S(22)); piece.move(pad + S(110), y, S(180), S(32)); y += S(42);
        priv.move(pad, y, w, S(24)); y += S(30);
        startSeed.move(pad, y, w, S(24)); y += S(34);
        bar.move(pad, y, w, S(8)); y += S(16);
        status.move(pad, y, w, S(40));
        int x = r.right - pad;
        for (Button* b : {&cancel, &create}) { int bw2 = std::max(S(110), b->preferredWidth()); x -= bw2; b->move(x, r.bottom - S(52), bw2, S(36)); x -= S(8); }
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_APP + 31) {
            int done = static_cast<int>(w), total = static_cast<int>(l);
            bar.set(total > 0 ? static_cast<double>(done) / total : 0);
            status.setText("Reading the files… " + std::to_string(total > 0 ? done * 100 / total : 0) + "%");
            return 0;
        }
        if (m == WM_APP + 32) {
            std::unique_ptr<std::pair<bool, std::string>> res(reinterpret_cast<std::pair<bool, std::string>*>(l));
            busy = false;
            create.setText("Create");
            EnableWindow(create.hwnd(), TRUE);
            cancel.setText("Close");
            if (!res->first) { status.setText(res->second); status.setColor(P.bad); bar.set(0); return 0; }
            bar.set(1);
            size_t nl = res->second.find('\n');
            std::string file = res->second.substr(0, nl), magnet = nl == std::string::npos ? "" : res->second.substr(nl + 1);
            status.setText("Made " + baseName(file) + (startSeed.checked ? " — DDM is sharing it (Torrents → Seeding)." : "."));
            status.setColor(P.good);
            int a = askBox(h, "Create torrent", "The torrent is ready:\n" + file + (magnet.empty() ? "" : "\n\nIts magnet link can be copied to share it without the file."), magnet.empty() ? std::vector<std::string>{"Open folder", "OK"} : std::vector<std::string>{"Open folder", "Copy magnet link", "OK"}, 0);
            if (a == 0) {
                PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(W(file).c_str());
                if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
            } else if (a == 1 && !magnet.empty()) TorrentWnd::setClipboardText(h, magnet);
            return 0;
        }
        if (m == WM_CLOSE && busy) { stop->store(true); return 0; }
        return Dialog::proc(m, w, l);
    }
};

template <class T>
void deleteOnClose(T* d) {
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<T*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
}
} // namespace

void showTorrentWindow(HWND owner, const std::string& taskId) {
    Task t;
    if (!Engine::get().find(taskId, t) || t.kind != "torrent") return;
    auto* d = new TorrentWnd();
    d->id = taskId;
    if (!d->createDialog(owner, "Torrent — " + t.fileName, S(820), S(620), true, true)) { delete d; return; }
    d->build();
    ShowWindow(d->hwnd(), SW_SHOW);
    deleteOnClose(d);
}

void showCreateTorrent(HWND owner, const std::string& source) {
    auto* d = new CreateWnd();
    if (!d->createDialog(owner, "Create torrent", S(640), S(720), true)) { delete d; return; }
    d->build(source);
    ShowWindow(d->hwnd(), SW_SHOW);
    deleteOnClose(d);
}

} // namespace ui
} // namespace ddm
