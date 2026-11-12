// File Properties (double-click a finished download, or right-click → Properties): everything DDM knows about the
// file, laid out like IDM's: type, status, size, where it is (and Move), its address, description, the page it came
// from, referrer, sign-in — plus dates, speed, checksum, antivirus result and the Mark of the Web.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/crypto.hpp"
#include "../core/engine.hpp"
#include "../core/motw.hpp"
#include "../core/queues.hpp"
#include "../core/settings.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <algorithm>
#include <thread>

namespace ddm {
namespace ui {

std::string algoName(const std::string& a);

namespace {

const UINT kMoved = WM_APP + 70, kHashed = WM_APP + 71;

std::string withCommas(int64_t n) {
    std::string s = std::to_string(n < 0 ? 0 : n), o;
    for (size_t i = 0; i < s.size(); i++) { if (i && (s.size() - i) % 3 == 0) o += ','; o += s[i]; }
    return o;
}

std::string typeName(const std::string& fileName) {
    SHFILEINFOW fi{};
    if (SHGetFileInfoW(W(fileName).c_str(), FILE_ATTRIBUTE_NORMAL, &fi, sizeof fi, SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) && fi.szTypeName[0]) return U(fi.szTypeName);
    std::string e = extOf(fileName);
    for (auto& c : e) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return e.empty() ? "File" : e + " file";
}

std::string statusOf(const Task& t, COLORREF& c) {
    c = P.text;
    switch (t.state) {
        case State::Complete:
            if (t.scanState == "threat") { c = P.bad; return "Complete · threat found by the antivirus"; }
            if (t.scanState == "removed") { c = P.bad; return "Removed by the antivirus"; }
            if (t.verify == "bad") { c = P.bad; return "Complete · damaged (checksum doesn't match)"; }
            c = P.good;
            return fileExists(t.path()) ? "Complete" : "Complete · the file was moved or deleted";
        case State::Running: return "Downloading " + std::to_string(static_cast<int>(t.progress() * 100)) + "%";
        case State::Paused: return t.total > 0 ? "Paused at " + std::to_string(static_cast<int>(t.progress() * 100)) + "%" : "Paused";
        case State::Queued: return "Waiting";
        case State::Error: c = P.bad; return "Error: " + t.error;
    }
    return "";
}

// "Basic dXNlcjpwYXNz" <-> user / password
void loginOf(const Task& t, std::string& user, std::string& pass) {
    for (auto& kv : t.headers) if (lower(kv.first) == "authorization" && lower(kv.second).compare(0, 6, "basic ") == 0) {
        auto b = crypto::unb64(trim(kv.second.substr(6)));
        std::string s(b.begin(), b.end());
        size_t c = s.find(':');
        if (c != std::string::npos) { user = s.substr(0, c); pass = s.substr(c + 1); }
    }
}

class PropsWnd : public Dialog {
public:
    std::string id;
    Task t;
    Field savePath, address, desc, referer, login, password;
    Button moveBtn, openBtn, folderBtn, hashBtn, okBtn, cancelBtn;
    RECT pageRect{};
    std::string sha;            // calculated on request
    bool busy = false;

    void build() {
        savePath.create(h, t.path(), false, true);
        moveBtn.create(h, "Move…");
        address.create(h, t.url);
        desc.create(h, t.description);
        std::string ref;
        for (auto& kv : t.headers) if (lower(kv.first) == "referer") ref = kv.second;
        referer.create(h, ref.empty() ? t.pageUrl : ref, false, true);
        std::string u, p;
        loginOf(t, u, p);
        login.create(h, u);
        password.create(h, p);
        SetWindowLongW(password.edit(), GWL_STYLE, GetWindowLongW(password.edit(), GWL_STYLE) | ES_PASSWORD);
        SendMessageW(password.edit(), EM_SETPASSWORDCHAR, static_cast<WPARAM>(0x25CF), 0);
        openBtn.create(h, "Open", BtnStyle::Normal, "open");
        folderBtn.create(h, "Open folder", BtnStyle::Normal, "folder");
        hashBtn.create(h, "Checksums", BtnStyle::Normal, "M4 9h16 M4 15h16 M10 3L8 21 M16 3l-2 18");
        okBtn.create(h, "OK", BtnStyle::Primary);
        cancelBtn.create(h, "Cancel");
        tooltip(h, hashBtn.hwnd(), "Calculate the file's SHA-256 (to compare with the one the website shows)");
        bool done = t.state == State::Complete && fileExists(t.path());
        EnableWindow(openBtn.hwnd(), done && t.scanState != "threat");
        EnableWindow(hashBtn.hwnd(), done);
        openBtn.onClick = [this] { App::get().openFile(id); };
        folderBtn.onClick = [this] { App::get().openFolder(id); };
        moveBtn.onClick = [this] { moveFile(); };
        hashBtn.onClick = [this] { hash(); };
        okBtn.onClick = [this] { apply(); close(0); };
        cancelBtn.onClick = [this] { close(0); };
        onEnter = okBtn.onClick;
        layout();
        SetFocus(okBtn.hwnd());
    }

    void apply() {
        std::string url = trim(address.get()), d = trim(desc.get()), u = trim(login.get()), p = password.get();
        bool urlChanged = !url.empty() && url != t.url;
        Engine::get().edit(id, [&](Task& k) {
            k.description = d;
            // sign-in for servers that ask for a user name and password
            for (auto it = k.headers.begin(); it != k.headers.end();) { if (lower(it->first) == "authorization" && lower(it->second).compare(0, 6, "basic ") == 0) it = k.headers.erase(it); else ++it; }
            if (!u.empty()) { std::string s = u + ":" + p; k.headers["Authorization"] = "Basic " + crypto::b64(crypto::Bytes(s.begin(), s.end())); }
        });
        if (urlChanged) Engine::get().refreshLink(id, url);
    }

    void moveFile() {
        if (busy) return;
        std::string to = pickFolder(h, t.dir);
        if (to.empty() || iequals(to, t.dir)) return;
        Task cur;
        if (!Engine::get().find(id, cur)) return;
        if (cur.state == State::Running) { askBox(h, "Move", "Pause the download first, then move it.", {"OK"}, 0); return; }
        busy = true;
        EnableWindow(moveBtn.hwnd(), FALSE);
        moveBtn.setText("Moving…");
        HWND hw = h;
        std::string tid = id;
        std::thread([hw, tid, cur, to] {
            auto* err = new std::string();
            std::string from = cur.state == State::Complete ? cur.path() : (cur.tempDir.empty() ? cur.partPath() : "");
            std::string name = cur.fileName;
            if (!from.empty() && fileExists(from)) {
                std::string dest = joinPath(to, cur.state == State::Complete ? name : baseName(from));
                if (fileExists(dest)) { dest = uniquePath(dest); if (cur.state == State::Complete) name = baseName(dest); }
                makeDirs(to);
                if (!MoveFileExW(W(from).c_str(), W(dest).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) *err = "Couldn't move the file (is it open in another program?).";
            }
            if (err->empty()) Engine::get().edit(tid, [&](Task& k) { k.dir = to; k.fileName = name; });
            if (!PostMessageW(hw, kMoved, 0, reinterpret_cast<LPARAM>(err))) delete err;
        }).detach();
    }

    void hash() {
        if (busy) return;
        busy = true;
        hashBtn.setText("Calculating…");
        EnableWindow(hashBtn.hwnd(), FALSE);
        HWND hw = h;
        std::string path = t.path();
        std::thread([hw, path] {
            auto* r = new std::string(crypto::fileChecksum(path, "sha256"));
            if (!PostMessageW(hw, kHashed, 0, reinterpret_cast<LPARAM>(r))) delete r;
        }).detach();
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == kMoved) {
            std::unique_ptr<std::string> e(reinterpret_cast<std::string*>(l));
            busy = false; EnableWindow(moveBtn.hwnd(), TRUE); moveBtn.setText("Move…");
            if (!e->empty()) askBox(h, "Move", *e, {"OK"}, 0);
            Engine::get().find(id, t);
            savePath.set(t.path());
            redraw();
            return 0;
        }
        if (m == kHashed) {
            std::unique_ptr<std::string> r(reinterpret_cast<std::string*>(l));
            busy = false; hashBtn.setText("Checksums"); EnableWindow(hashBtn.hwnd(), TRUE);
            sha = *r;
            redraw();
            if (!sha.empty()) {
                int a = askBox(h, "Checksums", t.fileName + "\n\nSHA-256: " + sha, {"Copy", "Compare with the website's…", "Close"}, 2);
                if (a == 0 && OpenClipboard(h)) {
                    EmptyClipboard();
                    std::wstring ws = W(sha);
                    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (ws.size() + 1) * 2);
                    if (g) { memcpy(GlobalLock(g), ws.c_str(), (ws.size() + 1) * 2); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                    CloseClipboard();
                } else if (a == 1) verifyChecksumDialog(h, id);
            }
            return 0;
        }
        if (m == WM_LBUTTONUP) {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&pageRect, pt) && !t.pageUrl.empty()) App::get().openUrl(t.pageUrl);
            return 0;
        }
        if (m == WM_SETCURSOR && reinterpret_cast<HWND>(w) == h) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            if (PtInRect(&pageRect, pt) && !t.pageUrl.empty()) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        }
        if (m == WM_DDM_BG) return bgReply(P.win);
        return Dialog::proc(m, w, l);
    }

    // rows: y positions shared by layout() and paintBackground()
    int pad() const { return S(18); }
    int lw() const { return S(112); }
    int yType() const { return S(86); }
    int ySave() const { return yType() + 3 * S(26) + S(6); }
    int yAddr() const { return ySave() + S(40); }
    int yDesc() const { return yAddr() + S(40); }
    int yPage() const { return yDesc() + S(52); }
    int yRef() const { return yPage() + S(54); }
    int yLogin() const { return yRef() + S(40); }
    int yPass() const { return yLogin(); }
    int yMore() const { return yLogin() + S(52); }

    void layout() override {
        RECT r = client();
        int x = pad() + lw(), right = r.right - pad();
        int mw = std::max(S(96), moveBtn.preferredWidth());
        savePath.move(x, ySave(), right - x - mw - S(8), S(30));
        moveBtn.move(right - mw, ySave(), mw, S(30));
        address.move(x, yAddr(), right - x, S(30));
        desc.move(x, yDesc(), right - x, S(30));
        referer.move(x, yRef(), right - x, S(30));
        int half = (right - pad()) / 2;
        login.move(pad() + lw(), yLogin(), half - lw() - S(10), S(30));
        password.move(pad() + half + S(86), yPass(), right - (pad() + half + S(86)), S(30));
        int by = r.bottom - S(50), bx = r.right - pad();
        for (Button* b : {&cancelBtn, &okBtn}) { int w = std::max(S(90), b->preferredWidth()); bx -= w; b->move(bx, by, w, S(34)); bx -= S(8); }
        int lx = pad();
        for (Button* b : {&openBtn, &folderBtn, &hashBtn}) { int w = std::max(S(90), b->preferredWidth()); if (lx + w > bx - S(8)) { b->show(false); continue; } b->show(true); b->move(lx, by, w, S(34)); lx += w + S(8); }
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int p = pad(), right = r.right - p;
        Engine::get().find(id, t);
        RECT ic{p, S(16), p + S(48), S(16) + S(48)};
        bool there = t.state == State::Complete && t.kind != "torrent" && fileExists(t.path());
        if (t.kind == "torrent" || !systemFileIcon(dc, ic, there ? t.path() : t.fileName, there)) fileIcon(dc, ic, typeOf(t.category, t.fileName, t.kind));
        RECT nm{p + S(64), S(20), right, S(46)};
        textName(dc, t.fileName, nm, P.text, fontBig(), DT_END_ELLIPSIS);
        RECT ln{p, S(74), right, S(75)}; fill(dc, ln, P.line);
        auto row = [&](int y, const std::string& k, const std::string& v, COLORREF c) {
            RECT a{p, y, p + lw(), y + S(24)}; text(dc, k, a, P.muted, fontUI());
            RECT b{p + lw(), y, right, y + S(24)}; text(dc, v, b, c, fontUI(), DT_END_ELLIPSIS);
        };
        COLORREF sc;
        std::string st = statusOf(t, sc);
        row(yType(), "Type:", t.kind == "torrent" ? "Torrent" : typeName(t.fileName), P.text);
        row(yType() + S(26), "Status:", st, sc);
        int64_t size = t.total >= 0 ? t.total : t.done;
        row(yType() + S(52), "Size:", size >= 0 ? formatBytes(size) + "  (" + withCommas(size) + " bytes)" : "Unknown", P.text);
        auto label = [&](int y, const std::string& k) { RECT a{p, y, p + lw(), y + S(30)}; text(dc, k, a, P.muted, fontUI()); };
        label(ySave(), "Save To:");
        label(yAddr(), "Address:");
        label(yDesc(), "Description:");
        RECT l2{p, yPage() - S(12), right, yPage() - S(11)}; fill(dc, l2, P.line);
        RECT pt{p, yPage(), right, yPage() + S(22)};
        text(dc, "The web page from which this file was obtained:", pt, P.muted, fontUI());
        pageRect = RECT{p, yPage() + S(22), right, yPage() + S(44)};
        text(dc, t.pageUrl.empty() ? "(not known)" : t.pageUrl, pageRect, t.pageUrl.empty() ? P.muted : P.accent, fontUI(), DT_END_ELLIPSIS);
        if (!t.pageUrl.empty()) { SIZE ts = textSize(dc, t.pageUrl, fontUI()); RECT ul{p, pageRect.bottom - S(3), std::min<int>(right, p + ts.cx), pageRect.bottom - S(2)}; fill(dc, ul, P.accent); }
        label(yRef(), "Referer:");
        int half = (right - p) / 2;
        label(yLogin(), "Login:");
        RECT pw{p + half, yPass(), p + half + S(80), yPass() + S(30)}; text(dc, "Password:", pw, P.muted, fontUI());
        RECT l3{p, yMore() - S(12), right, yMore() - S(11)}; fill(dc, l3, P.line);
        // more about the file, two columns
        std::vector<std::pair<std::string, std::string>> facts;
        facts.push_back({"Added", t.created ? formatDate(t.created) : ""});
        if (t.finished) facts.push_back({"Finished", formatDate(t.finished)});
        if (t.finished && t.activeMs > 0) {   // the time it was actually downloading, all runs added up (pauses don't count)
            int64_t secs = std::max<int64_t>(1, (t.activeMs + 500) / 1000);
            char took[64];   // to the second, also for long downloads
            long long hh = secs / 3600, mm = (secs % 3600) / 60, ss = secs % 60;
            if (hh) snprintf(took, sizeof took, "%lld h %lld min %lld s", hh, mm, ss);
            else if (mm) snprintf(took, sizeof took, "%lld min %lld s", mm, ss);
            else snprintf(took, sizeof took, "%lld s", ss);
            facts.push_back({"Took", took});
            if (t.total > 0) facts.push_back({"Average speed", formatSpeed(static_cast<double>(t.total) * 1000.0 / static_cast<double>(std::max<int64_t>(1, t.activeMs)))});
        } else if (t.finished && t.lastTry && t.finished > t.lastTry) {
            int64_t secs = std::max<int64_t>(1, (t.finished - t.lastTry) / 1000);
            facts.push_back({"Took", formatDuration(secs)});
            if (t.total > 0) facts.push_back({"Average speed", formatSpeed(static_cast<double>(t.total) / static_cast<double>(secs))});
        }
        facts.push_back({"Category", categoryName(t.category)});
        std::string qn = t.queue.empty() ? "" : queues::nameOf(t.queue);
        if (!qn.empty()) facts.push_back({"Queue", qn});
        facts.push_back({"Website", t.site.empty() ? urlSite(t.url) : t.site});
        facts.push_back({"Can resume", t.resumable ? "Yes" : "No"});
        if (!t.formatLabel.empty()) facts.push_back({"Quality", t.formatLabel});
        if (!t.checksum.empty()) {
            std::string a = t.checksum.substr(0, t.checksum.find(':'));
            facts.push_back({"Checksum", algoName(a) + (t.verify == "ok" ? " · matches" : t.verify == "bad" ? " · no match!" : "")});
        }
        if (!sha.empty()) facts.push_back({"SHA-256", sha});
        if (!t.scanNote.empty()) facts.push_back({"Antivirus", t.scanNote});
        if (t.state == State::Complete && fileExists(t.path())) facts.push_back({"From the internet", motw::isMarked(t.path()) ? "Yes (Windows checks it before it runs)" : "No mark"});
        // short facts in columns, long ones (SHA-256, antivirus) across the whole width
        int cols = right - p >= S(660) ? 3 : 2, colW = (right - p) / cols, fy = yMore(), col = 0;
        std::vector<std::pair<std::string, std::string>> wideOnes;
        for (auto& f : facts) {
            if (f.first == "SHA-256" || f.first == "Antivirus" || f.first == "From the internet") { wideOnes.push_back(f); continue; }
            int cx = p + col * colW;
            RECT k{cx, fy, cx + S(100), fy + S(22)};
            text(dc, f.first, k, P.muted, fontSmall());
            RECT v{cx + S(102), fy, cx + colW - S(8), fy + S(22)};
            text(dc, f.second, v, f.second.find("no match") != std::string::npos ? P.bad : P.text, fontSmall(), DT_END_ELLIPSIS);
            if (++col == cols) { col = 0; fy += S(22); }
        }
        if (col) fy += S(22);
        for (auto& f : wideOnes) {
            if (fy > r.bottom - S(78)) break;
            RECT k{p, fy, p + S(118), fy + S(22)};
            text(dc, f.first, k, P.muted, fontSmall());
            RECT v{p + S(120), fy, right, fy + S(22)};
            text(dc, f.second, v, P.text, fontSmall(), DT_END_ELLIPSIS);
            fy += S(22);
        }
    }
};

} // namespace

void propertiesDialog(HWND owner, const std::string& id) {
    auto* d = new PropsWnd();
    d->id = id;
    if (!Engine::get().find(id, d->t)) { delete d; return; }
    if (!d->createDialog(owner, "File Properties", S(720), S(690), true)) { delete d; return; }
    d->build();
    ShowWindow(d->hwnd(), SW_SHOW);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<PropsWnd*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
}

} // namespace ui
} // namespace ddm
