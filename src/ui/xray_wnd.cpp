// The X-Ray window: what's inside an archive, disc image, torrent or program before it's downloaded. Tick files and
// "Download selected" fetches only those (each checked against the archive's checksum); "Download whole file" goes
// back to the normal download.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/engines.hpp"
#include "../core/torrent.hpp"
#include "../core/settings.hpp"
#include "../core/xray.hpp"
#include "../core/motw.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <memory>
#include <thread>
#include <condition_variable>
#include <chrono>

namespace ddm {
namespace ui {

namespace {

const char* kAlert = "M12 3.5l9.5 16.5h-19z M12 10v4.5 M12 17.2v.3";
const UINT kLooked = WM_APP + 60, kStatus = WM_APP + 61, kSaved = WM_APP + 62, kProgress = WM_APP + 63;

// one row of the tree: a folder (real or implied by paths) or a file
struct Row {
    std::string name, path;
    int depth = 0;
    bool dir = false;
    int entry = -1;            // index in Index::entries, -1 for an implied folder
    int64_t size = 0;
    bool checked = false, open = true;
    std::vector<int> kids;     // rows
    int parent = -1;
};

class XTree : public Wnd {
public:
    std::vector<Row> rows;
    std::vector<int> visible;
    const xray::Index* idx = nullptr;
    std::string filter;
    int top = 0, sel = -1;
    std::function<void()> onCheck;

    bool create(HWND parent) { return Wnd::create(L"DDMXTree", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 10, 10, parent); }
    int rowH() const { return S(26); }
    int headH() const { return S(28); }

    void build(const xray::Index& ix) {
        idx = &ix;
        rows.clear();
        std::map<std::string, int> folders;
        rows.push_back(Row());   // a hidden root
        rows[0].dir = true; rows[0].depth = -1;
        std::function<int(const std::string&)> folder = [&](const std::string& path) -> int {
            if (path.empty()) return 0;
            auto it = folders.find(path);
            if (it != folders.end()) return it->second;
            size_t slash = path.rfind('/');
            int par = folder(slash == std::string::npos ? "" : path.substr(0, slash));
            Row r; r.name = slash == std::string::npos ? path : path.substr(slash + 1); r.path = path; r.dir = true; r.parent = par; r.depth = rows[static_cast<size_t>(par)].depth + 1;
            rows.push_back(r);
            int id = static_cast<int>(rows.size()) - 1;
            rows[static_cast<size_t>(par)].kids.push_back(id);
            folders[path] = id;
            return id;
        };
        for (size_t i = 0; i < ix.entries.size(); i++) {
            const auto& e = ix.entries[i];
            if (e.dir) { int f = folder(e.path); rows[static_cast<size_t>(f)].entry = static_cast<int>(i); continue; }
            size_t slash = e.path.rfind('/');
            int par = folder(slash == std::string::npos ? "" : e.path.substr(0, slash));
            Row r; r.name = slash == std::string::npos ? e.path : e.path.substr(slash + 1); r.path = e.path; r.entry = static_cast<int>(i); r.size = e.size; r.parent = par; r.depth = rows[static_cast<size_t>(par)].depth + 1;
            rows.push_back(r);
            rows[static_cast<size_t>(par)].kids.push_back(static_cast<int>(rows.size()) - 1);
        }
        // folders first, then names (like Explorer); folder sizes add up
        for (auto& r : rows) std::sort(r.kids.begin(), r.kids.end(), [&](int a, int b) {
            const Row& x = rows[static_cast<size_t>(a)]; const Row& y = rows[static_cast<size_t>(b)];
            if (x.dir != y.dir) return x.dir;
            return lower(x.name) < lower(y.name);
        });
        std::function<int64_t(int)> sum = [&](int id) -> int64_t { Row& r = rows[static_cast<size_t>(id)]; if (!r.dir) return r.size; int64_t s = 0; for (int k : r.kids) s += sum(k); r.size = s; return s; };
        sum(0);
        // big trees start folded below the second level
        if (ix.entries.size() > 300) for (auto& r : rows) if (r.dir && r.depth >= 1) r.open = false;
        relist();
    }
    bool pickable(const Row& r) const { if (!idx || r.entry < 0) return r.dir; const auto& e = idx->entries[static_cast<size_t>(r.entry)]; return e.dir || e.why.empty(); }
    bool matches(int id) const {
        const Row& r = rows[static_cast<size_t>(id)];
        if (filter.empty()) return true;
        if (lower(r.name).find(filter) != std::string::npos) return true;
        for (int k : r.kids) if (matches(k)) return true;
        return false;
    }
    void relist() {
        visible.clear();
        std::function<void(int)> walk = [&](int id) {
            for (int k : rows[static_cast<size_t>(id)].kids) {
                if (!matches(k)) continue;
                visible.push_back(k);
                if (rows[static_cast<size_t>(k)].dir && (rows[static_cast<size_t>(k)].open || !filter.empty())) walk(k);
            }
        };
        walk(0);
        updateScroll();
        redraw();
    }
    void setCheck(int id, bool on) {
        Row& r = rows[static_cast<size_t>(id)];
        if (!r.dir) { if (pickable(r)) r.checked = on; return; }
        for (int k : r.kids) setCheck(k, on);
        r.checked = on;
    }
    // a folder shows ticked when every pickable file in it is
    int folderState(int id) const {   // 0 none, 1 some, 2 all
        const Row& r = rows[static_cast<size_t>(id)];
        int files = 0, on = 0;
        std::function<void(int)> count = [&](int k) { const Row& x = rows[static_cast<size_t>(k)]; if (x.dir) { for (int c : x.kids) count(c); return; } if (!pickable(x)) return; files++; if (x.checked) on++; };
        count(id);
        (void)r;
        return on == 0 ? 0 : on == files ? 2 : 1;
    }
    void checkAll(bool on) { setCheck(0, on); redraw(); if (onCheck) onCheck(); }
    std::vector<size_t> chosen() const {
        std::vector<size_t> out;
        for (auto& r : rows) if (!r.dir && r.checked && r.entry >= 0) out.push_back(static_cast<size_t>(r.entry));
        return out;
    }
    void updateScroll() {
        RECT r = client();
        int vis = std::max<int>(1, (r.bottom - headH()) / rowH());
        top = std::max(0, std::min(top, std::max(0, static_cast<int>(visible.size()) - vis)));
        SCROLLINFO si{sizeof si, SIF_ALL, 0, std::max(0, static_cast<int>(visible.size()) - 1), static_cast<UINT>(vis), top, 0};
        SetScrollInfo(h, SB_VERT, &si, TRUE);
    }

protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
            case WM_ERASEBKGND: return 1;
            case WM_SIZE: updateScroll(); return 0;
            case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
            case WM_PAINT: {
                Paint p(h);
                HDC dc = p.dc();
                RECT r = p.rc();
                fill(dc, r, P.field);
                int sizeW = S(96), dateW = S(110), noteW = std::max<int>(S(120), r.right / 4);
                int nameR = r.right - sizeW - dateW - noteW - S(8);
                RECT hr{0, 0, r.right, headH()};
                fill(dc, hr, P.win);
                RECT h1{S(36), 0, nameR, headH()}; text(dc, "Name", h1, P.muted, fontSmall());
                RECT h2{nameR, 0, nameR + sizeW - S(10), headH()}; text(dc, "Size", h2, P.muted, fontSmall(), DT_RIGHT);
                RECT h3{nameR + sizeW + S(6), 0, nameR + sizeW + dateW, headH()}; text(dc, "Modified", h3, P.muted, fontSmall());
                RECT h4{nameR + sizeW + dateW + S(6), 0, r.right - S(8), headH()}; text(dc, "Note", h4, P.muted, fontSmall());
                RECT ln{0, headH() - 1, r.right, headH()}; fill(dc, ln, P.line);
                int y = headH();
                for (int i = top; i < static_cast<int>(visible.size()) && y < r.bottom; i++, y += rowH()) {
                    int id = visible[static_cast<size_t>(i)];
                    const Row& row = rows[static_cast<size_t>(id)];
                    RECT rr{0, y, r.right, y + rowH()};
                    if (id == sel) fill(dc, rr, GetFocus() == h ? P.sel : blend(P.sel, P.field, 0.5f));
                    int x = S(8) + row.depth * S(18);
                    if (row.dir) {   // fold arrow
                        RECT ar{x, y + (rowH() - S(12)) / 2, x + S(12), y + (rowH() - S(12)) / 2 + S(12)};
                        icon(dc, row.open || !filter.empty() ? "chevron" : "chevronright", ar, P.muted, 1.8f);
                    }
                    x += S(16);
                    // tick box
                    bool can = pickable(row);
                    int st = row.dir ? folderState(id) : (row.checked ? 2 : 0);
                    RECT cb{x, y + (rowH() - S(16)) / 2, x + S(16), y + (rowH() - S(16)) / 2 + S(16)};
                    if (can) {
                        if (st) { fillRound(dc, cb, Sf(4), P.accent); RECT ci = cb; InflateRect(&ci, -S(2), -S(2)); if (st == 2) icon(dc, "check", ci, P.accentText, 2.4f); else { RECT d{ci.left + S(2), (ci.top + ci.bottom) / 2 - S(1), ci.right - S(2), (ci.top + ci.bottom) / 2 + S(1)}; fill(dc, d, P.accentText); } }
                        else { fillRound(dc, cb, Sf(4), P.field); strokeRound(dc, cb, Sf(4), P.fieldLine, 1.2f); }
                    } else { strokeRound(dc, cb, Sf(4), blend(P.fieldLine, P.field, 0.5f), 1.0f); }
                    x += S(24);
                    RECT ic{x, y + (rowH() - S(18)) / 2, x + S(18), y + (rowH() - S(18)) / 2 + S(18)};
                    if (row.dir) folderIcon(dc, ic, "");
                    else fileIcon(dc, ic, typeOf(categoryFor(row.name), row.name, ""));
                    x += S(24);
                    const xray::Entry* e = row.entry >= 0 && idx ? &idx->entries[static_cast<size_t>(row.entry)] : nullptr;
                    RECT nr{x, y, nameR - S(8), y + rowH()};
                    textName(dc, row.name, nr, can ? P.text : P.muted, row.dir ? fontBold() : fontUI(), DT_END_ELLIPSIS);
                    RECT sr{nameR, y, nameR + sizeW - S(10), y + rowH()};
                    if (!row.dir || row.size > 0) text(dc, e && e->size < 0 ? "?" : formatBytes(row.size), sr, P.muted, fontSmall(), DT_RIGHT);
                    if (e && e->mtime > 0) { RECT dr{nameR + sizeW + S(6), y, nameR + sizeW + dateW, y + rowH()}; text(dc, formatDay(e->mtime * 1000), dr, P.muted, fontSmall(), DT_END_ELLIPSIS); }
                    if (e && (!e->risk.empty() || !e->why.empty())) {
                        RECT nt{nameR + sizeW + dateW + S(6), y, r.right - S(8), y + rowH()};
                        if (!e->risk.empty()) { RECT wi{nt.left, y + (rowH() - S(14)) / 2, nt.left + S(14), y + (rowH() - S(14)) / 2 + S(14)}; icon(dc, kAlert, wi, P.bad, 1.8f); nt.left += S(18); }
                        text(dc, !e->risk.empty() ? e->risk : e->why, nt, !e->risk.empty() ? P.bad : P.muted, fontSmall(), DT_END_ELLIPSIS);
                    }
                }
                if (visible.empty()) { RECT er{0, headH() + S(20), r.right, headH() + S(50)}; text(dc, filter.empty() ? "" : "Nothing inside matches.", er, P.muted, fontUI(), DT_CENTER); }
                return 0;
            }
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
                SetFocus(h);
                int y = GET_Y_LPARAM(l) - headH(), x = GET_X_LPARAM(l);
                if (y < 0) return 0;
                int i = top + y / rowH();
                if (i < 0 || i >= static_cast<int>(visible.size())) return 0;
                int id = visible[static_cast<size_t>(i)];
                Row& row = rows[static_cast<size_t>(id)];
                sel = id;
                int base = S(8) + row.depth * S(18);
                if (row.dir && (x < base + S(16) || m == WM_LBUTTONDBLCLK)) { row.open = !row.open; relist(); return 0; }
                toggle(id);
                return 0;
            }
            case WM_KEYDOWN: {
                int pos = static_cast<int>(std::find(visible.begin(), visible.end(), sel) - visible.begin());
                if (w == VK_DOWN || w == VK_UP) { pos = std::max(0, std::min(static_cast<int>(visible.size()) - 1, pos + (w == VK_DOWN ? 1 : -1))); if (!visible.empty()) sel = visible[static_cast<size_t>(pos)]; ensure(pos); redraw(); }
                else if ((w == VK_SPACE) && sel >= 0) toggle(sel);
                else if ((w == VK_RIGHT || w == VK_LEFT) && sel >= 0 && rows[static_cast<size_t>(sel)].dir) { rows[static_cast<size_t>(sel)].open = w == VK_RIGHT; relist(); }
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
            case WM_SETFOCUS: case WM_KILLFOCUS: redraw(); return 0;
        }
        return Wnd::proc(m, w, l);
    }
    void toggle(int id) {
        Row& row = rows[static_cast<size_t>(id)];
        if (!pickable(row)) return;
        setCheck(id, row.dir ? folderState(id) != 2 : !row.checked);
        redraw();
        if (onCheck) onCheck();
    }
    void ensure(int pos) {
        RECT r = client();
        int vis = std::max<int>(1, (r.bottom - headH()) / rowH());
        if (pos < top) top = pos;
        if (pos >= top + vis) top = pos - vis + 1;
        updateScroll();
    }
};

struct Looked { bool ok = false; std::shared_ptr<xray::Source> src; xray::Index idx; std::string err; };

// Reads what's inside (any thread). status may be null.
Looked* lookInto(const XRayRequest& r, std::function<void(const std::string&)> status, std::atomic<bool>* st, std::shared_ptr<std::atomic<bool>> flag) {
    auto* out = new Looked();
    std::string err;
    std::unique_ptr<xray::Source> s;
    auto t0 = std::chrono::steady_clock::now();
    if (r.torrentData.empty() && !r.local && xray::kindOf(r.name) == "torrent") {   // a .torrent on the web: it's small, read it whole
        RequestInfo ri; ri.url = r.url; ri.referer = r.referer; ri.cookies = r.cookies; ri.userAgent = r.userAgent;
        std::string body;
        long code = http::get(ri, body, nullptr, 20 * 1024 * 1024, 30);
        if (code == 200 && !body.empty() && body[0] == 'd') { out->ok = xray::lookTorrent(body, out->idx); out->idx.bytesRead = static_cast<int64_t>(body.size()); }
        else out->err = code == 0 ? "Couldn't reach the server." : "The .torrent file couldn't be downloaded (the server answered " + std::to_string(code) + ").";
    } else if (r.torrentData.size()) {
        out->ok = xray::lookTorrent(r.torrentData, out->idx);
    } else {
        if (r.local) s = xray::openFile(r.url, err);
        else { RequestInfo ri; ri.url = r.url; ri.referer = r.referer; ri.cookies = r.cookies; ri.userAgent = r.userAgent; ri.headers = r.headers; if (!flag || *flag) s = xray::openHttp(ri, err, st); }
        if (s) {
            std::string name = r.name.empty() ? (s->name.empty() ? fileNameFromUrl(r.url) : s->name) : r.name;
            out->ok = xray::look(*s, name, out->idx, status);
            out->src = std::move(s);
        } else out->err = err;
    }
    // the whole time, connecting included (what the person waited for)
    out->idx.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!out->ok && out->err.empty()) out->err = out->idx.error;
    return out;
}

// X-Ray read ahead: the download window starts reading the table of contents the moment it shows an archive, so the
// X-Ray window opens with everything already there.
struct Warm {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::unique_ptr<Looked> res;
    int64_t at = 0;
};
std::mutex g_warmMu;
std::map<std::string, std::shared_ptr<Warm>> g_warm;
std::string warmKey(const XRayRequest& r) { return r.url + "\x1f" + r.name; }

// the read ahead for r (waits for it when it's still reading); null = none
std::unique_ptr<Looked> takeWarm(const XRayRequest& r, std::atomic<bool>* st) {
    std::shared_ptr<Warm> w;
    {
        std::lock_guard<std::mutex> g(g_warmMu);
        auto it = g_warm.find(warmKey(r));
        if (it == g_warm.end()) return nullptr;
        w = it->second;
        g_warm.erase(it);
    }
    std::unique_lock<std::mutex> g(w->mu);
    while (!w->done) { if (st && st->load()) return nullptr; w->cv.wait_for(g, std::chrono::milliseconds(50)); }
    if (!w->res || !w->res->ok) return nullptr;   // failed ahead (maybe a passing problem): read again now
    if (nowMs() - w->at > 10 * 60 * 1000) return nullptr;   // old: links expire
    if (w->res->src) w->res->src->stop = st;
    return std::move(w->res);
}

class XRayWnd : public Dialog {
public:
    XRayRequest req;
    std::shared_ptr<xray::Source> src;
    xray::Index idx;
    bool loaded = false, busy = false, failed = false;
    std::string sub = "Reading the table of contents…", errorText;
    std::atomic<bool> stop{false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    int64_t pDone = 0, pTotal = 0;
    std::string pName;
    XTree tree;
    Field search, dest;
    Button selAll, browse, go, whole, closeBtn;
    CheckBox keep;
    int outcome = 0;   // 1 = files saved, 2 = whole file wanted
    std::function<void(int)> onDone;

    ~XRayWnd() override { stop = true; *alive = false; }

    void build() {
        tree.create(h);
        tree.onCheck = [this] { refreshButtons(); };
        search.create(h, "");
        search.hint("Search inside…");
        search.onChange = [this] { tree.filter = lower(trim(search.get())); tree.relist(); };
        selAll.create(h, "Select all");
        selAll.onClick = [this] { bool any = !tree.chosen().empty(); tree.checkAll(!any); selAll.setText(any ? "Select all" : "Select none"); };
        std::string folder = joinPath(folderFor("compressed"), sanitizeFileName(stem(req.name.empty() ? "X-Ray" : req.name)));
        if (req.local) folder = joinPath(dirName(req.url), sanitizeFileName(stem(req.name)));
        dest.create(h, folder);
        browse.create(h, "…");
        browse.onClick = [this] { std::string f = pickFolder(h, dest.get()); if (!f.empty()) dest.set(f); };
        keep.create(h, "Keep folders", true);
        go.create(h, "Download selected", BtnStyle::Primary, "down");
        go.onClick = [this] { save(); };
        whole.create(h, req.local ? "Open the folder" : "Download whole file");
        whole.onClick = [this] {
            if (req.local) { PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(W(req.url).c_str()); if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); } return; }
            outcome = 2; close(0);
        };
        closeBtn.create(h, "Close");
        closeBtn.onClick = [this] { if (busy) { stop = true; return; } close(0); };
        onEnter = [this] { if (loaded && !busy && !tree.chosen().empty()) save(); };
        for (Wnd* w : {static_cast<Wnd*>(&tree), static_cast<Wnd*>(&search), static_cast<Wnd*>(&selAll), static_cast<Wnd*>(&dest), static_cast<Wnd*>(&browse), static_cast<Wnd*>(&keep), static_cast<Wnd*>(&go)}) w->show(false);
        layout();
        lookNow();
    }

    void lookNow() {
        HWND hw = h;
        XRayRequest r = req;
        auto flag = alive;
        std::atomic<bool>* st = &stop;
        std::thread([hw, r, flag, st] {
            Looked* out = nullptr;
            if (auto w = takeWarm(r, st)) out = w.release();
            if (!out && *flag) out = lookInto(r, [hw, flag](const std::string& line) { if (*flag) PostMessageW(hw, kStatus, 0, reinterpret_cast<LPARAM>(new std::string(line))); }, st, flag);
            if (!out) return;
            if (!*flag || !PostMessageW(hw, kLooked, 0, reinterpret_cast<LPARAM>(out))) delete out;
        }).detach();
    }

    void looked(Looked* L) {
        src = L->src;
        if (!L->ok) { failed = true; errorText = L->err; sub = "X-Ray couldn't look inside."; whole.show(!req.local && req.torrentData.empty()); layout(); redraw(); return; }
        idx = std::move(L->idx);
        loaded = true;
        int files = 0, risky = 0, blocked = 0;
        int64_t total = 0;
        for (auto& e : idx.entries) { if (e.dir) continue; files++; total += std::max<int64_t>(0, e.size); if (!e.risk.empty()) risky++; if (!e.why.empty()) blocked++; }
        char secs[32];
        if (idx.seconds < 10) snprintf(secs, sizeof secs, "%d ms", static_cast<int>(idx.seconds * 1000 + 0.5)); else snprintf(secs, sizeof secs, "%.1f s", idx.seconds);
        if (idx.listing) {
            sub = std::to_string(files) + (files == 1 ? " file · " : " files · ") + formatBytes(total) + " · " + idx.format + (idx.label.empty() || idx.format == "Torrent" ? "" : " · \"" + idx.label + "\"");
            if (!isTorrent() && !req.local) sub += " · read in " + std::string(secs) + " (" + formatBytes(idx.bytesRead) + ")";
        } else sub = idx.format + (req.local || isTorrent() ? "" : " · read in " + std::string(secs) + " (" + formatBytes(idx.bytesRead) + ")");
        notes.clear();
        for (auto& w : idx.warnings) notes.push_back({w, P.bad});
        if (risky) notes.push_back({std::to_string(risky) + (risky == 1 ? " file inside looks" : " files inside look") + " dangerous (marked below): DDM saw " + (risky == 1 ? "it" : "them") + " before " + (risky == 1 ? "it" : "they") + " reached your PC.", P.bad});
        if (blocked && idx.listing && !isTorrent()) notes.push_back({std::to_string(blocked) + (blocked == 1 ? " file can't" : " files can't") + " be picked out on its own (password, compression or a link): use \"" + std::string(req.local ? "Open the folder" : "Download whole file") + "\" for " + (blocked == 1 ? "it." : "those."), P.warn});
        for (auto& n : idx.notes) notes.push_back({n, P.muted});
        if (idx.listing && !risky && idx.warnings.empty()) notes.push_back({isTorrent() ? "Nothing suspicious in the file list. Tick only the files you want." : "Nothing suspicious inside. Only the files you tick are downloaded" + std::string(anyCrc() ? ", each one checked with its checksum." : "."), P.good});
        if (idx.listing) {
            tree.build(idx);
            for (Wnd* w : {static_cast<Wnd*>(&tree), static_cast<Wnd*>(&search), static_cast<Wnd*>(&selAll), static_cast<Wnd*>(&dest), static_cast<Wnd*>(&browse), static_cast<Wnd*>(&keep), static_cast<Wnd*>(&go)}) w->show(true);
            if (isTorrent()) { dest.show(false); browse.show(false); keep.show(false); }
            SetFocus(tree.hwnd());
        }
        whole.show(!isTorrent() || !idx.listing);
        refreshButtons();
        layout();
        redraw();
    }
    bool isTorrent() const { return idx.format == "Torrent"; }
    bool anyCrc() const { for (auto& e : idx.entries) if (e.hasCrc) return true; return false; }

    void refreshButtons() {
        size_t n = tree.chosen().size();
        int64_t sz = 0;
        for (size_t i : tree.chosen()) sz += std::max<int64_t>(0, idx.entries[i].size);
        go.setText(n ? (isTorrent() ? "Download " : "Download ") + std::to_string(n) + (n == 1 ? " file" : " files") + " (" + formatBytes(sz) + ")" : "Download selected");
        EnableWindow(go.hwnd(), n > 0 && !busy);
        layout();
    }

    void save() {
        std::vector<size_t> which = tree.chosen();
        if (which.empty() || busy) return;
        if (isTorrent()) {   // the torrent with only these files
            std::string pick;
            for (size_t i : which) { if (!pick.empty()) pick += ","; pick += std::to_string(idx.entries[i].index); }
            req.selectFiles = pick;
            outcome = 1;
            if (onDone) onDone(outcome);
            onDone = nullptr;
            close(0);
            return;
        }
        std::string out = trim(dest.get());
        if (out.empty() || !makeDirs(out)) { askBox(h, "X-Ray", "DDM can't create the folder " + out + ".", {"OK"}, 0); return; }
        int risky = 0;
        for (size_t i : which) if (!idx.entries[i].risk.empty()) risky++;
        if (risky && askBox(h, "X-Ray", std::to_string(risky) + (risky == 1 ? " of the files you ticked looks" : " of the files you ticked look") + " dangerous. Download anyway?", {"Download anyway", "Cancel"}, 1) != 0) return;
        busy = true; stop = false; pDone = 0; pTotal = 0;
        EnableWindow(go.hwnd(), FALSE);
        closeBtn.setText("Stop");
        HWND hw = h;
        auto s = src;
        xray::Index ix = idx;
        bool keepF = keep.checked;
        auto flag = alive;
        std::thread([hw, s, ix, which, out, keepF, flag] {
            auto* res = new xray::ExtractResult();
            xray::extract(*s, ix, which, out, keepF, [hw, flag](int64_t d, int64_t t, const std::string& cur) {
                static DWORD last = 0;
                DWORD now = GetTickCount();
                if (now - last < 150 && d < t) return;
                last = now;
                if (*flag) PostMessageW(hw, kProgress, 0, reinterpret_cast<LPARAM>(new std::tuple<int64_t, int64_t, std::string>(d, t, cur)));
            }, *res);
            if (!*flag || !PostMessageW(hw, kSaved, 0, reinterpret_cast<LPARAM>(res))) delete res;
        }).detach();
        redraw();
    }

    void saved(xray::ExtractResult* r) {
        busy = false;
        // files taken out of an archive from the internet carry the same mark a browser would give them
        if (settings().markOfTheWeb) {
            bool fromWeb = !req.local || motw::isMarked(req.url);
            if (fromWeb) for (auto& f : r->files) motw::mark(f, req.local ? "" : req.url, req.referer);
        }
        closeBtn.setText("Close");
        refreshButtons();
        std::string msg = std::to_string(r->saved) + (r->saved == 1 ? " file saved in " : " files saved in ") + trim(dest.get()) + ".";
        for (size_t i = 0; i < r->problems.size() && i < 8; i++) msg += "\n• " + r->problems[i];
        if (r->problems.size() > 8) msg += "\n• …and " + std::to_string(r->problems.size() - 8) + " more.";
        int a = askBox(h, r->problems.empty() ? "X-Ray: done" : "X-Ray", msg, r->saved ? std::vector<std::string>{"Open the folder", "Close"} : std::vector<std::string>{"OK"}, 0);
        if (r->saved && a == 0) {
            std::string first = r->files.empty() ? trim(dest.get()) : r->files.front();
            PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(W(first).c_str());
            if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
        }
        if (r->saved) { outcome = 1; }
        pDone = pTotal = 0;
        redraw();
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == kLooked) { std::unique_ptr<Looked> L(reinterpret_cast<Looked*>(l)); looked(L.get()); return 0; }
        if (m == kStatus) { std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l)); if (!loaded) { sub = *s; redraw(); } return 0; }
        if (m == kProgress) { std::unique_ptr<std::tuple<int64_t, int64_t, std::string>> p(reinterpret_cast<std::tuple<int64_t, int64_t, std::string>*>(l)); pDone = std::get<0>(*p); pTotal = std::get<1>(*p); pName = std::get<2>(*p); redraw(); return 0; }
        if (m == kSaved) { std::unique_ptr<xray::ExtractResult> r(reinterpret_cast<xray::ExtractResult*>(l)); saved(r.get()); return 0; }
        if (m == WM_CLOSE && busy) { stop = true; return 0; }
        if (m == WM_DESTROY && onDone) { auto f = onDone; onDone = nullptr; f(outcome); }
        if (m == WM_GETMINMAXINFO) { auto* mm = reinterpret_cast<MINMAXINFO*>(l); mm->ptMinTrackSize = {S(640), S(420)}; return 0; }
        if (m == WM_DDM_BG) return bgReply(P.win);
        return Dialog::proc(m, w, l);
    }

    std::vector<std::pair<std::string, COLORREF>> notes;
    int buttonsLeft = 0;
    int headerH() const { return S(84); }
    int notesH() const { return static_cast<int>(notes.size()) * S(22) + (notes.empty() ? 0 : S(10)); }

    void layout() override {
        RECT r = client();
        int pad = S(18);
        int y = headerH() + notesH();
        int bottom = r.bottom - S(60);
        bool list = loaded && idx.listing;
        if (list) {
            search.move(pad, y, S(260), S(30));
            int sw = std::max(S(96), selAll.preferredWidth());
            selAll.move(pad + S(268), y, sw, S(30));
            y += S(40);
            int saveRow = isTorrent() ? 0 : S(42);
            tree.move(pad, y, r.right - 2 * pad, bottom - saveRow - y - S(8));
            if (!isTorrent()) {
                int ky = bottom - S(38);
                int kw = S(130);
                dest.move(pad + S(64), ky, r.right - 2 * pad - S(64) - S(38) - kw - S(12), S(30));
                browse.move(r.right - pad - kw - S(12) - S(30), ky, S(30), S(30));
                keep.move(r.right - pad - kw, ky + S(4), kw, S(22));
            }
        }
        int bx = r.right - pad, by = r.bottom - S(48);
        for (Button* b : {&closeBtn, &whole, &go}) {
            if (!IsWindowVisible(b->hwnd()) && b != &closeBtn) continue;
            int bw = std::max(S(100), b->preferredWidth());
            bx -= bw; b->move(bx, by, bw, S(34)); bx -= S(8);
        }
        buttonsLeft = bx;
        redraw();
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(18);
        RECT tile{pad, S(16), pad + S(52), S(16) + S(52)};
        RECT ic = tile; InflateRect(&ic, -S(4), -S(4));
        fileIcon(dc, ic, typeOf(isTorrent() ? "torrent" : categoryFor(req.name), req.name, isTorrent() ? "torrent" : ""));
        RECT t1{pad + S(66), S(16), r.right - S(170), S(42)};
        textName(dc, req.name.empty() ? "X-Ray" : req.name, t1, P.text, fontBig(), DT_END_ELLIPSIS);
        RECT t2{pad + S(66), S(44), r.right - pad, S(66)};
        text(dc, sub, t2, failed ? P.bad : P.muted, fontUI(), DT_END_ELLIPSIS);
        // badge
        std::string badge = failed ? "Couldn't look inside" : !loaded ? "Reading…" : req.local ? "On this PC" : isTorrent() ? "File list ready" : "Opened remotely";
        for (auto& w : idx.warnings) if (w.compare(0, 10, "Looks fake") == 0) badge = "Looks fake";
        COLORREF bc = failed || badge == "Looks fake" ? P.bad : loaded ? P.good : P.muted;
        HDC mdc = dc;
        SIZE bs = textSize(mdc, badge, fontSmallBold());
        RECT b{r.right - pad - bs.cx - S(20), S(20), r.right - pad, S(20) + S(24)};
        fillRound(dc, b, Sf(12), blend(P.win, bc, isDark() ? 0.22f : 0.13f));
        text(dc, badge, b, bc, fontSmallBold(), DT_CENTER);
        int y = headerH();
        for (auto& n : notes) {
            RECT ni{pad, y + S(4), pad + S(14), y + S(18)};
            icon(dc, n.second == P.bad ? kAlert : n.second == P.good ? "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z M8.5 12l2.5 2.5 4.5-5" : "M12 3a9 9 0 1 0 0.01 0 M12 11v6 M12 7.5v.3", ni, n.second, 1.7f);
            RECT nr{pad + S(22), y, r.right - pad, y + S(22)}; text(dc, n.first, nr, n.second, fontUI(), DT_END_ELLIPSIS); y += S(22);
        }
        if (failed) {
            RECT er{pad, headerH(), r.right - pad, r.bottom - S(70)};
            text(dc, errorText, er, P.text, fontUI(), DT_WORDBREAK);
        }
        if (!loaded && !failed) {   // a slim moving bar while it reads
            RECT tr{pad, headerH() + S(8), r.right - pad, headerH() + S(12)};
            fillRound(dc, tr, Sf(2), P.track);
        }
        if (loaded && idx.listing && !isTorrent()) { RECT lr{pad, r.bottom - S(60) - S(38), pad + S(60), r.bottom - S(60) - S(8)}; text(dc, "Save to", lr, P.muted, fontUI()); }
        if (!loaded || !idx.listing) {}
        if (busy || pTotal > 0) {   // saving: progress in the footer
            int pr = std::max<int>(pad + S(80), buttonsLeft - S(16));   // never under the buttons
            RECT tr{pad, r.bottom - S(44), pr, r.bottom - S(40)};
            fillRound(dc, tr, Sf(2), P.track);
            if (pTotal > 0) { RECT fr = tr; fr.right = fr.left + static_cast<int>((fr.right - fr.left) * std::min(1.0, static_cast<double>(pDone) / static_cast<double>(pTotal))); fillRound(dc, fr, Sf(2), P.accent); }
            RECT pt{pad, r.bottom - S(36), pr, r.bottom - S(16)};
            text(dc, formatBytes(pDone) + " of " + formatBytes(pTotal) + " · " + baseName(replaceAll(pName, "/", "\\")), pt, P.muted, fontSmall(), DT_END_ELLIPSIS);
        }
    }
};

} // namespace

// Opens X-Ray. done(1) = files saved / torrent files chosen (req.selectFiles), done(2) = the whole file wanted.
void warmXRay(const XRayRequest& r0) {
    XRayRequest r = r0;
    if (r.name.empty()) r.name = r.local ? baseName(r.url) : fileNameFromUrl(r.url);
    if (r.url.empty() || !xray::canLook(r.name)) return;
    auto w = std::make_shared<Warm>();
    {
        std::lock_guard<std::mutex> g(g_warmMu);
        for (auto it = g_warm.begin(); it != g_warm.end();) {   // forget old ones
            bool old;
            { std::lock_guard<std::mutex> g2(it->second->mu); old = it->second->done && nowMs() - it->second->at > 10 * 60 * 1000; }
            if (old) it = g_warm.erase(it); else ++it;
        }
        if (g_warm.count(warmKey(r)) || g_warm.size() >= 8) return;
        g_warm[warmKey(r)] = w;
    }
    std::thread([w, r] {
        std::unique_ptr<Looked> res(lookInto(r, nullptr, nullptr, nullptr));
        std::lock_guard<std::mutex> g(w->mu);
        w->res = std::move(res);
        w->at = nowMs();
        w->done = true;
        w->cv.notify_all();
    }).detach();
}

void showXRay(HWND owner, const XRayRequest& r, std::function<void(int, const XRayRequest&)> done) {
    auto* x = new XRayWnd();
    x->req = r;
    if (x->req.name.empty()) x->req.name = r.local ? baseName(r.url) : fileNameFromUrl(r.url);
    if (!x->createDialog(owner, "X-Ray", S(860), S(600), true)) { delete x; return; }
    x->onDone = [x, done](int res) { if (done) done(res, x->req); };
    x->build();
    ShowWindow(x->hwnd(), SW_SHOWNORMAL);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<XRayWnd*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(x->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(x));
}

// Tools → X-Ray / toolbar: the selected download, or a link / file you give.
void showXRayFor(HWND owner, const std::string& taskId) {
    XRayRequest r;
    Task t;
    if (!taskId.empty() && Engine::get().find(taskId, t) && t.kind != "video" && t.kind != "mega") {
        if (t.kind == "torrent") {
            std::string tf = joinPath(engines::dir(), "t" + t.id + ".torrent");
            std::string data;
            if (fileExists(tf) && readFile(tf, data)) { r.torrentData = data; r.name = t.fileName + ".torrent"; }
            else if (!isMagnet(t.url) && fileExists(t.url) && readFile(t.url, data)) { r.torrentData = data; r.name = baseName(t.url); }
            else { askBox(owner, "X-Ray", "This torrent's file list isn't known yet: it arrives once the torrent starts and finds other people sharing it.", {"OK"}, 0); return; }
        } else if (t.state == State::Complete && fileExists(t.path())) { r.local = true; r.url = t.path(); r.name = t.fileName; }
        else { r.url = t.url; r.referer = t.pageUrl; r.cookies = t.cookies; r.userAgent = t.userAgent; r.headers = t.headers; r.name = t.fileName; }
        if (!xray::canLook(r.name)) { askBox(owner, "X-Ray", "X-Ray looks inside archives (ZIP, RAR, 7z, TAR, CAB), disc images (ISO), torrents and programs (EXE, MSI). " + t.fileName + " isn't one of those.", {"OK"}, 0); return; }
        std::string id = t.id;
        bool torrent = t.kind == "torrent";
        showXRay(owner, r, [id, torrent](int res, const XRayRequest& x) {
            if (res == 1 && torrent && !x.selectFiles.empty()) Engine::get().setTorrentFiles(id, x.selectFiles);
        });
        return;
    }
    std::string v;
    if (!inputBox(owner, "X-Ray", "Link to look inside (an archive, disc image, torrent or program), or a file on this PC", v) || trim(v).empty()) return;
    v = trim(v);
    if (v.size() > 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (fileExists(v)) {
        if (lower(extOf(v)) == "torrent") { std::string data; readFile(v, data); r.torrentData = data; r.name = baseName(v); }
        else { r.local = true; r.url = v; r.name = baseName(v); }
    } else if (looksLikeUrl(v) && !isMagnet(v)) { r.url = v; r.name = fileNameFromUrl(v); }
    else { askBox(owner, "X-Ray", "That isn't a link or a file DDM can open.", {"OK"}, 0); return; }
    showXRay(owner, r, [v](int res, const XRayRequest& x) {
        if (x.selectFiles.empty() || res != 1) return;   // a torrent: download only the chosen files
        LinkRequest lr; lr.url = v; lr.kind = "torrent"; lr.videoFormat = x.selectFiles; lr.source = "paste";
        App::get().newDownload(lr);
    });
}

} // namespace ui
} // namespace ddm
