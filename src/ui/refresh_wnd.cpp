// Refresh Download Link, the way IDM does it: DDM opens the page the file came from in the browser, you click its
// download link again, and DDM recognises the new address as the same file (same size, same name or site). The
// interrupted download takes the new address and the browser's cookies and carries on from the byte it stopped
// at; no new window appears and nothing already downloaded is lost.
// Videos don't need the browser: DDM reads the video's page again for fresh addresses of the same quality.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/downloader.hpp"
#include "../core/engine.hpp"
#include "../core/links.hpp"
#include "../core/media.hpp"
#include "../core/settings.hpp"
#include "../common/util.hpp"
#include <commctrl.h>
#include <algorithm>
#include <thread>

namespace ddm {
namespace ui {

namespace {

const int64_t kWaitMs = 30 * 60 * 1000;   // a refresh waits half an hour for the click in the browser

class RefreshWnd;
struct Wait { std::string id; int64_t since = 0; RefreshWnd* wnd = nullptr; };
std::vector<Wait> g_waits;   // window thread only

void dropWait(const std::string& id) {
    g_waits.erase(std::remove_if(g_waits.begin(), g_waits.end(), [&](const Wait& w) { return w.id == id; }), g_waits.end());
}

std::string percentText(const Task& t) {
    if (t.total <= 0) return t.done > 0 ? formatBytes(t.done) + " already downloaded" : "nothing downloaded yet";
    char b[32];
    snprintf(b, sizeof b, "%.1f%%", t.progress() * 100.0);
    return std::string(b) + " (" + formatBytes(t.done) + " of " + formatBytes(t.total) + ")";
}

class RefreshWnd : public Dialog {
public:
    std::string id;
    Task t;
    Button reopen, paste, cancelBtn;
    void build() {
        reopen.create(h, "Open the page again", BtnStyle::Normal, "globe");
        paste.create(h, "Paste an address…", BtnStyle::Normal, "link");
        cancelBtn.create(h, "Cancel");
        reopen.onClick = [this] { if (!t.pageUrl.empty()) App::get().openUrl(t.pageUrl); };
        reopen.show(!t.pageUrl.empty());
        paste.onClick = [this] {
            std::string v = t.url;
            if (!inputBox(h, "Refresh download link", "New address for " + t.fileName, v) || trim(v).empty() || trim(v) == t.url) return;
            Task f;
            f.url = trim(v);
            Engine::get().refreshFrom(id, f);
            Engine::get().start(id);
            close(0);
        };
        cancelBtn.onClick = [this] { close(0); };
        onEnter = cancelBtn.onClick;
        layout();
        SetTimer(h, 1, 1000, nullptr);
    }
    ~RefreshWnd() override {
        for (auto& w : g_waits) if (w.wnd == this) w.wnd = nullptr;
        dropWait(id);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_TIMER) {
            Task n;
            bool gone = !Engine::get().find(id, n) || n.state == State::Complete || n.state == State::Running;
            bool late = true;
            for (auto& x : g_waits) if (x.id == id) late = nowMs() - x.since > kWaitMs;
            if (gone || late) { close(0); return 0; }
            if (n.done != t.done) { t = n; redraw(); }
            return 0;
        }
        return Dialog::proc(m, w, l);
    }
    void layout() override {
        RECT r = client();
        int y = r.bottom - S(52), x = r.right - S(18);
        for (Button* b : {&cancelBtn, &paste, &reopen}) {
            if (!IsWindowVisible(b->hwnd()) && b != &cancelBtn && b != &paste) continue;
            int bw = std::max(S(96), b->preferredWidth());
            x -= bw; b->move(x, y, bw, S(34)); x -= S(8);
        }
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT tile{S(18), S(18), S(18) + S(52), S(18) + S(52)};
        fillRound(dc, tile, Sf(8), blend(P.win, P.accent, isDark() ? 0.20f : 0.12f));
        RECT ic = tile; InflateRect(&ic, -S(13), -S(13));
        icon(dc, "refresh", ic, P.accent, 2.4f);
        RECT t1{S(84), S(18), r.right - S(18), S(44)};
        text(dc, "Waiting for the new download link", t1, P.text, fontBig());
        RECT t2{S(84), S(46), r.right - S(18), S(68)};
        text(dc, t.fileName, t2, P.muted, fontUI(), DT_END_ELLIPSIS);
        std::string body = t.pageUrl.empty()
            ? "DDM doesn't know which page this file came from. Open that page in your browser and click the file's download link: DDM recognises it as this download and carries on from " + percentText(t) + "."
            : "DDM opened the page this file came from in your browser. Click the download link for this file there (sign in or solve the check if the site asks). DDM recognises it as this download, takes the new link and carries on from " + percentText(t) + ": nothing already downloaded is lost.";
        RECT b{S(18), S(86), r.right - S(18), r.bottom - S(64)};
        text(dc, body, b, P.text, fontUI(), DT_WORDBREAK);
    }
};

std::string normName(std::string n) {
    n = lower(sanitizeFileName(urlDecode(n)));
    std::string e = extOf(n), s = stem(n);
    size_t p = s.rfind(" (");   // "file (2).zip" is the same file as "file.zip"
    if (p != std::string::npos && !s.empty() && s.back() == ')') s = s.substr(0, p);
    return e.empty() ? s : s + "." + e;
}

struct Probed {
    LinkRequest req;
    bool ok = false;
    ResponseInfo info;
};

// Is the address the browser just sent the interrupted download `t`?
bool sameFile(const Task& t, const Probed& p) {
    std::string name = !p.info.fileName.empty() ? p.info.fileName : (!p.req.suggestedName.empty() ? p.req.suggestedName : fileNameFromUrl(p.info.finalUrl.empty() ? p.req.url : p.info.finalUrl));
    int64_t size = p.info.total > 0 ? p.info.total : p.req.size;
    bool nameEq = !name.empty() && (normName(name) == normName(t.fileName) || normName(name) == normName(fileNameFromUrl(t.url)));
    std::string page = p.req.pageUrl.empty() ? p.req.referer : p.req.pageUrl;
    bool siteEq = (!page.empty() && !t.pageUrl.empty() && urlSite(page) == urlSite(t.pageUrl)) || urlSite(p.req.url) == urlSite(t.url);
    if (t.total > 0 && size > 0) return size == t.total && (nameEq || siteEq);   // the size must match to the byte
    return nameEq;   // size unknown: the same name (DDM checks the size again as soon as data arrives)
}

const UINT kMsgProbed = WM_APP + 44;

// a quiet note by the tray icon (safe from any thread)
void note(const std::string& title, const std::string& text) { if (Engine::get().onNotice) Engine::get().onNotice(title, text); }

} // namespace

// Context menu / progress window / failed download: Refresh Download Link.
void refreshLinkDialog(HWND owner, const std::string& id) {
    Task t;
    if (!Engine::get().find(id, t) || t.state == State::Complete) return;
    if (t.kind == "torrent") { askBox(owner, "Refresh download link", "Torrents don't use a download link: DDM finds other people sharing the file by itself.", {"OK"}, 0); return; }
    if (t.state == State::Running) Engine::get().pause(id);
    if (t.kind == "mega") { Engine::get().start(id); return; }   // MEGA links don't expire: DDM asks MEGA again by itself
    if (t.kind == "video") {   // fresh addresses for the same quality, straight from the video's page
        note("Refresh download link", "Reading the video's page again for " + t.fileName + "…");
        std::thread([t] {
            VideoRequest vr;
            vr.url = t.url; vr.cookies = t.cookies; vr.userAgent = t.userAgent; vr.referer = t.pageUrl;
            VideoInfo vi;
            bool ok = probeVideo(vr, vi);
            Task f;
            bool found = false;
            if (ok) for (auto& fm : vi.formats) {
                if ((!t.videoFormat.empty() && fm.selector == t.videoFormat) || (t.videoFormat.empty() && !t.formatLabel.empty() && fm.label == t.formatLabel)) {
                    if (!fm.audioUrl.empty()) { f.mediaUrl = fm.videoUrl; f.audioUrl = fm.audioUrl; f.mediaExt = fm.videoExt; f.audioExt = fm.audioExt; }
                    else if (fm.direct && fm.convert.empty()) f.mediaUrl = fm.directUrl;
                    for (auto& kv : fm.headers) if (lower(kv.first) != "cookie") f.headers[kv.first] = kv.second;
                    found = true;
                    break;
                }
            }
            Engine& e = Engine::get();
            if (ok) {   // the same quality: fresh addresses; otherwise the video's page itself is read again when it starts
                if (found) e.refreshFrom(t.id, f);
                e.start(t.id);
                note("Download link refreshed", t.fileName + " carries on from where it stopped.");
            }
            else note("Couldn't refresh the link", t.fileName + ": " + (vi.error.empty() ? "the video's page didn't answer." : vi.error));
        }).detach();
        return;
    }
    // ordinary files: the browser, on the page the file came from
    for (auto& w : g_waits) if (w.id == id && w.wnd) { SetForegroundWindow(w.wnd->hwnd()); if (!t.pageUrl.empty()) App::get().openUrl(t.pageUrl); return; }
    dropWait(id);
    auto* d = new RefreshWnd();
    d->id = id;
    d->t = t;
    if (!d->createDialog(nullptr, "Refresh download link", S(520), S(268), true)) { delete d; return; }
    g_waits.push_back({id, nowMs(), d});
    d->build();
    ShowWindow(d->hwnd(), SW_SHOWNORMAL);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<RefreshWnd*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
    if (!t.pageUrl.empty()) App::get().openUrl(t.pageUrl);
}

// A new link arrived (browser, paste, Add URL). While a refresh waits, DDM first checks whether it's the
// interrupted file; only if it isn't does the usual Download File Info window open.
bool refreshTake(const LinkRequest& r) {
    if (g_waits.empty() || r.refreshChecked || r.kind != "http") return false;
    HWND hw = App::get().mainHwnd();
    if (!hw) return false;
    auto* p = new Probed();
    p->req = r;
    std::thread([p, hw] {
        RequestInfo ri;
        ri.url = p->req.url; ri.referer = p->req.referer.empty() ? p->req.pageUrl : p->req.referer;
        ri.cookies = p->req.cookies; ri.userAgent = p->req.userAgent; ri.headers = p->req.headers;
        p->ok = probe(ri, p->info);
        if (!PostMessageW(hw, kMsgProbed, 0, reinterpret_cast<LPARAM>(p))) delete p;
    }).detach();
    return true;
}

void refreshProbed(LPARAM l) {
    std::unique_ptr<Probed> p(reinterpret_cast<Probed*>(l));
    if (p->ok && p->info.status < 400) {
        for (auto it = g_waits.begin(); it != g_waits.end(); ++it) {
            Task t;
            if (!Engine::get().find(it->id, t) || t.state == State::Complete) continue;
            if (!sameFile(t, *p)) continue;
            Task f;
            f.url = p->req.url;
            f.cookies = p->req.cookies;
            f.userAgent = p->req.userAgent;
            f.headers = p->req.headers;
            f.total = p->info.total;
            std::string id = it->id;
            RefreshWnd* w = it->wnd;
            g_waits.erase(it);
            if (w) w->close(0);
            Engine::get().refreshFrom(id, f);
            Engine::get().start(id);
            note("Download link refreshed", t.fileName + " carries on from " + percentText(t) + ".");
            if (settings().showProgress) App::get().showProgress(id);
            return;
        }
    }
    LinkRequest r = p->req;   // another file: the usual window
    r.refreshChecked = true;
    App::get().newDownload(r);
}

} // namespace ui
} // namespace ddm
