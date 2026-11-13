#include "dialogs.hpp"
#include "speedglide.hpp"
#include "dust.hpp"
#include "gfx.hpp"
#include "mainwnd.hpp"
#include "theme.hpp"
#include "../core/inspect.hpp"
#include "../core/downloader.hpp"
#include "../core/engine.hpp"
#include "../core/xray.hpp"
#include "../core/crypto.hpp"
#include "../core/media.hpp"
#include "../core/mega.hpp"
#include "../core/torrent.hpp"
#include "../core/settings.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>
#include <set>
#include <tuple>
#include <algorithm>
#include <cstdio>
#include <map>
#include <thread>
#include <cstring>
#include <cmath>

namespace ddm {
namespace ui {

// ================================================================ Dialog base
static std::vector<Dialog*> g_dialogs;

bool Dialog::createDialog(HWND own, const std::string& title, int w, int ht, bool resizable, bool minimizeBox) {
    owner = own;
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | (resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0) | (minimizeBox ? WS_MINIMIZEBOX : 0);
    RECT r{0, 0, w, ht};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    int ww = r.right - r.left, hh = r.bottom - r.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    // big text sizes (150 %, 175 %…) on small screens: never taller or wider than the screen
    int maxW = wa.right - wa.left - S(16), maxH = wa.bottom - wa.top - S(16);
    if (ww > maxW) ww = maxW;
    if (hh > maxH) hh = maxH;
    sizable = resizable;
    minSize = {ww, hh};
    int x = wa.left + (wa.right - wa.left - ww) / 2, y = wa.top + (wa.bottom - wa.top - hh) / 2;
    if (!create(L"DDMDialog", title, style, 0, x, y, ww, hh, own)) return false;
    styleWindow(h);
    g_dialogs.push_back(this);
    return true;
}

int Dialog::runModal() {
    modal = true;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    MSG msg;
    while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    return result;
}

void Dialog::paintBackground(HDC dc, RECT r) { fill(dc, r, P.win); }

LRESULT Dialog::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { Paint p(h); paintBackground(p.dc(), p.rc()); return 0; }
        case WM_DDM_BG: return bgReply(P.win);
        case WM_SIZE: layout(); RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN); return 0;
        case WM_GETMINMAXINFO: { auto* mm = reinterpret_cast<MINMAXINFO*>(l); if (minSize.cx) { mm->ptMinTrackSize.x = minSize.cx; mm->ptMinTrackSize.y = minSize.cy; } return 0; }
        case WM_CLOSE: close(-1); return 0;
        case WM_DESTROY: g_dialogs.erase(std::remove(g_dialogs.begin(), g_dialogs.end(), this), g_dialogs.end()); return 0;
        case WM_COMMAND:
            if (LOWORD(w) == IDCANCEL) { SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
            if (LOWORD(w) == IDOK && onEnter) { onEnter(); return 0; }
            break;
    }
    return def(m, w, l);
}

void retintAll() {
    for (auto d : g_dialogs) { styleWindow(d->hwnd()); RedrawWindow(d->hwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME); }
}

// ================================================================ ask / input
namespace {
class AskBox : public Dialog {
public:
    std::string message;
    std::vector<std::unique_ptr<Button>> buttons;
    Label msg;
    int def = 0;
    void build(const std::vector<std::string>& labels) {
        msg.create(h, message);
        msg.wrap = true;
        for (size_t i = 0; i < labels.size(); i++) {
            auto b = std::make_unique<Button>();
            b->create(h, labels[i], static_cast<int>(i) == def ? BtnStyle::Primary : BtnStyle::Normal);
            int idx = static_cast<int>(i);
            b->onClick = [this, idx] { close(idx); };
            buttons.push_back(std::move(b));
        }
        onEnter = [this] { close(def); };
        layout();
        if (!buttons.empty()) SetFocus(buttons[static_cast<size_t>(def)]->hwnd());
    }
    int rows = 1;
    void layout() override {
        RECT r = client();
        int top = r.bottom - S(52) - (rows - 1) * S(42);
        msg.move(S(20), S(18), r.right - S(40), top - S(28));
        // right-aligned rows; the last button is in the bottom row, the first ones move up when space runs out
        std::vector<int> ws;
        for (auto& b : buttons) ws.push_back(std::max(S(96), b->preferredWidth()));
        int x = r.right - S(20), y = r.bottom - S(52);
        for (size_t k = buttons.size(); k-- > 0;) {
            if (x - ws[k] < S(20) && x < r.right - S(20)) { x = r.right - S(20); y -= S(42); }
            int w = std::min<int>(ws[k], static_cast<int>(r.right - S(40)));
            x -= w;
            buttons[k]->move(x, y, w, S(34));
            x -= S(8);
        }
    }
};

class InputBox : public Dialog {
public:
    Label lbl;
    Field fld;
    Button ok, cancel;
    std::string value;
    void layout() override {
        RECT r = client();
        lbl.move(S(20), S(16), r.right - S(40), S(20));
        fld.move(S(20), S(42), r.right - S(40), S(30));
        cancel.move(r.right - S(20) - S(96), r.bottom - S(52), S(96), S(34));
        ok.move(r.right - S(20) - S(96) * 2 - S(8), r.bottom - S(52), S(96), S(34));
    }
};
} // namespace

int askBox(HWND owner, const std::string& title, const std::string& text, const std::vector<std::string>& buttons, int defaultButton) {
    AskBox d;
    d.message = text;
    d.def = defaultButton;
    HDC dc = GetDC(nullptr);
    // wide enough for every button in one row (the buttons decide, not the text): none is ever cut off
    int row = 0;
    for (size_t i = 0; i < buttons.size(); i++) {
        int w = std::max(S(96), static_cast<int>(textSize(dc, buttons[i], static_cast<int>(i) == defaultButton ? fontBold() : fontUI()).cx) + S(28));
        row += w + (i ? S(8) : 0);
    }
    HMONITOR mon = MonitorFromWindow(owner ? owner : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int maxW = (mi.rcWork.right - mi.rcWork.left) - S(40);
    int width = std::min(maxW, std::max(S(440), row + S(40)));
    d.rows = row + S(40) > width ? 2 : 1;   // a very small screen: two rows of buttons
    RECT calc{0, 0, width - S(40), 0};
    std::wstring wt = W(i18n::tr(text));
    HGDIOBJ o = SelectObject(dc, fontUI());
    DrawTextW(dc, wt.c_str(), -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    SelectObject(dc, o);
    ReleaseDC(nullptr, dc);
    if (!d.createDialog(owner, title, width, calc.bottom + S(100) + (d.rows - 1) * S(42))) return -1;
    d.build(buttons);
    return d.runModal();
}

bool inputBox(HWND owner, const std::string& title, const std::string& label, std::string& value) {
    InputBox d;
    if (!d.createDialog(owner, title, S(420), S(140))) return false;
    d.lbl.create(d.hwnd(), label);
    d.lbl.setColor(P.muted);
    d.fld.create(d.hwnd(), value);
    d.ok.create(d.hwnd(), "OK", BtnStyle::Primary);
    d.cancel.create(d.hwnd(), "Cancel");
    bool done = false;
    d.ok.onClick = [&] { value = trim(d.fld.get()); done = !value.empty(); d.close(0); };
    d.cancel.onClick = [&] { d.close(1); };
    d.fld.onEnter = d.ok.onClick;
    d.layout();
    d.fld.focus();
    SendMessageW(d.fld.edit(), EM_SETSEL, 0, -1);
    d.runModal();
    return done;
}

// ================================================================ Download File Info (the prompt)
namespace {
class Prompt;
std::map<int, Prompt*> g_tabPrompts;   // browser tab -> its video prompt (follows the latest video in that tab)
std::vector<Prompt*> g_prompts;

class Prompt : public Dialog {
public:
    std::string preId, preKey;    // the quiet download that starts with the window
    std::shared_ptr<void> thumb;  // a video's picture, shown in the panel on the right
    bool confirmed = false;
    Button existing;              // "Already downloaded · Open"
    Button safety;                // the link check's verdict ("Safe link" / "Be careful" / "Dangerous")
    inspect::Report linkRep;
    std::string repFor;           // the quiet download the report is about
    ~Prompt() override {
        if (!preId.empty() && !confirmed) Engine::get().preCancel(preId);
        for (auto it = g_tabPrompts.begin(); it != g_tabPrompts.end();) { if (it->second == this) it = g_tabPrompts.erase(it); else ++it; }
        g_prompts.erase(std::remove(g_prompts.begin(), g_prompts.end(), this), g_prompts.end());
    }
    LinkRequest req;
    Label lUrl, lCat, lSave, lDesc, lQual, footHost, footResume;
    Field url, saveAs, desc;
    DropDown category, quality;
    VideoInfo vinfo;
    std::vector<VideoFormat> formats;     // what the Quality list offers
    std::string videoError;
    int gen = 0;                           // ignores answers for a video the tab has moved past
    bool isVideo() const { return req.kind == "video"; }
    Button newCat, browse, start, later, cancel, xray;
    CheckBox remember, wantThumb, wantSubs;   // videos: also the thumbnail picture / the subtitles
    Button subsBtn;                           // … and which languages (every subtitle track the video offers)
    std::set<size_t> subPicked;               // indexes into vinfo.subs

    std::string subName(const SubTrack& t) const {
        std::string n = t.name.empty() ? languageName(t.lang) : t.name;
        if (t.automatic && lower(n).find("auto") == std::string::npos) n += " (automatic)";
        return n;
    }
    void subsLabel() {
        std::string text = "Choose languages";
        if (subPicked.size() == 1 || subPicked.size() == 2) {
            text.clear();
            for (size_t i : subPicked) if (i < vinfo.subs.size()) text += (text.empty() ? "" : ", ") + subName(vinfo.subs[i]);
        }
        if (subPicked.size() > 2 || text.size() > 44) text = std::to_string(subPicked.size()) + " languages";
        subsBtn.setText(text);
    }
    void defaultSubs() {   // the languages Options → General asks for
        subPicked.clear();
        for (auto& w : subsForLangs(vinfo.subs, settings().subLangs))
            for (size_t i = 0; i < vinfo.subs.size(); i++) if (vinfo.subs[i].url == w.url) subPicked.insert(i);
    }
    void subsReady() {   // the video's subtitle tracks are known
        defaultSubs();
        subsLabel();
        bool any = !vinfo.subs.empty();
        if (any != (IsWindowVisible(subsBtn.hwnd()) != FALSE)) subsBtn.show(any);
        layout();
    }
    void chooseSubs() {   // a list to tick: it stays open until you click outside it
        if (vinfo.subs.empty()) return;
        RECT br; GetWindowRect(subsBtn.hwnd(), &br);
        for (;;) {
            std::vector<MenuItem> items;
            MenuItem all; all.text = "All languages"; all.id = 1; items.push_back(all);
            MenuItem none; none.text = "No subtitles"; none.id = 2; items.push_back(none);
            items.push_back(MenuItem::sep());
            for (size_t i = 0; i < vinfo.subs.size() && i < 250; i++) {
                MenuItem m; m.text = subName(vinfo.subs[i]); m.id = static_cast<UINT>(100 + i); m.checked = subPicked.count(i) > 0;
                items.push_back(m);
            }
            UINT id = popupMenu(h, items, POINT{br.left, br.bottom + S(2)});
            if (id == 0) break;
            if (id == 1) { for (size_t i = 0; i < vinfo.subs.size(); i++) subPicked.insert(i); }
            else if (id == 2) subPicked.clear();
            else { size_t i = id - 100; if (subPicked.count(i)) subPicked.erase(i); else subPicked.insert(i); }
            wantSubs.set(!subPicked.empty());
            subsLabel();
            layout();
            if (id == 1 || id == 2) break;
        }
    }
    std::vector<std::string> catKeys;
    std::string fileName, typeText = "Checking…", sizeText = "";
    bool sizeExact = true;          // false: the size of a video quality is the site's estimate
    int64_t size = -1;
    bool ranges = false, probed = false, nameEdited = false;
    std::string probeStatus;
    std::string site;

    void build() {
        HWND p = h;
        lUrl.create(p, "URL"); lCat.create(p, "Category"); lSave.create(p, "Save As"); lDesc.create(p, "Description"); lQual.create(p, "Quality");
        for (Label* l : {&lUrl, &lCat, &lSave, &lDesc, &lQual}) l->setColor(P.muted);
        quality.create(p, {"Getting qualities…"}, 0);
        quality.iconName = "video";
        quality.onChange = [this](int i) { applyFormat(i); };
        if (!isVideo()) { lQual.show(false); quality.show(false); }
        g_prompts.push_back(this);
        if (isVideo() && req.tab >= 0) g_tabPrompts[req.tab] = this;
        url.create(p, req.url);
        std::vector<std::string> names;
        for (auto& c : settings().categories) { if (c.key == "torrent" && req.kind != "torrent") continue; catKeys.push_back(c.key); names.push_back(c.name); }
        catKeys.push_back("general"); names.push_back("General");
        category.create(p, names, 0);
        category.iconName = "folder";
        newCat.create(p, "", BtnStyle::Normal, "plus");
        saveAs.create(p, "");
        browse.create(p, "…");
        remember.create(p, "Remember this path", true);
        wantThumb.create(p, "Save the thumbnail", settings().videoThumb);
        wantSubs.create(p, "Save the subtitles", settings().videoSubs);
        tooltip(h, wantThumb.hwnd(), "Also save the video's picture next to it (.jpg)");
        tooltip(h, wantSubs.hwnd(), "Also save the video's subtitles next to it (.srt)");
        subsBtn.create(p, "Choose languages", BtnStyle::Normal, "chevron");
        subsBtn.show(false);
        tooltip(h, subsBtn.hwnd(), "Every subtitle language this video offers: tick the ones to save");
        subsBtn.onClick = [this] { chooseSubs(); };
        wantSubs.onChange = [this](bool on) {
            if (!on || vinfo.subs.empty() || !subPicked.empty()) return;
            defaultSubs();
            subsLabel();
            layout();
            if (subPicked.empty()) chooseSubs();   // none in the usual languages: which ones, then?
        };
        if (!isVideo()) { wantThumb.show(false); wantSubs.show(false); }
        desc.create(p, req.description, true);
        start.create(p, "Start Download", BtnStyle::Primary, "down");
        later.create(p, "Download Later");
        cancel.create(p, "Cancel");
        footHost.create(p, "");
        footHost.small = true;
        footHost.setColor(P.muted);
        footResume.create(p, "");
        footResume.small = true;
        footHost.background = &P.foot;
        footResume.background = &P.foot;
        xray.create(p, "X-Ray", BtnStyle::Pill, "xray");
        xray.show(false);
        xray.onClick = [this] { openXRay(); };
        existing.create(p, "Already downloaded · Open", BtnStyle::Pill, "check");
        existing.show(false);
        safety.create(p, "", BtnStyle::Pill, "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z");
        safety.show(false);
        safety.onClick = [this] { explainSafety(); };
        tooltip(h, safety.hwnd(), "What DDM found out about this link and file");
        SetTimer(h, 7, 600, nullptr);
        existing.onClick = [this] {
            std::string f = existingPath();
            if (!f.empty()) ShellExecuteW(nullptr, L"open", W(f).c_str(), nullptr, W(dirName(f)).c_str(), SW_SHOWNORMAL);
        };
        tooltip(h, newCat.hwnd(), "New category");
        tooltip(h, browse.hwnd(), "Save as… (any folder, any name)");

        site = urlSite(req.pageUrl.empty() ? req.url : req.url);
        fileName = initialName();
        size = req.size;
        setCategory(categoryFor(fileName, req.kind == "torrent"));
        category.onChange = [this](int) { updatePath(); remember.set(true); };
        newCat.onClick = [this] { addCategory(); };
        browse.onClick = [this] { pickPath(); };
        saveAs.onChange = [this] { if (GetFocus() == saveAs.edit()) nameEdited = true; };
        url.onChange = [this] { if (GetFocus() == url.edit()) { probed = false; SetTimer(h, 2, 700, nullptr); } };
        start.onClick = [this] { go(true); };
        later.onClick = [this] { go(false); };
        cancel.onClick = [this] { dust::window(h); close(-1); };
        onEnter = [this] { go(true); };
        layout();
        refreshFooter();
        SetFocus(start.hwnd());
        probe();
        startQuietly();
        refreshExisting();
    }

    std::string initialName() const {
        if (isVideo()) return sanitizeFileName(trim(req.title).empty() ? "video" : trim(req.title)) + ".mp4";
        if (req.kind == "torrent" && isMagnet(req.url)) {   // the name inside the magnet (dn=)
            size_t p = req.url.find("dn=");
            if (p != std::string::npos) { std::string dn = req.url.substr(p + 3); dn = dn.substr(0, dn.find('&')); dn = urlDecode(replaceAll(dn, "+", " ")); if (!trim(dn).empty()) return sanitizeFileName(dn); }
            return "Torrent";
        }
        if (req.kind == "mega") return req.suggestedName.empty() ? "MEGA file" : sanitizeFileName(req.suggestedName);
        std::string n = sanitizeFileName(!req.suggestedName.empty() ? req.suggestedName : fileNameFromUrl(req.url));
        if (req.kind == "torrent" && !endsWith(lower(n), ".torrent")) return n;
        return req.kind == "torrent" ? n.substr(0, n.size() - 8) : n;
    }

    // The tab moved on to another video (TikTok / Reels scrolling): this prompt now offers that one.
    void replaceRequest(const LinkRequest& r) {
        dropQuiet();   // the earlier video isn't wanted any more
        thumb.reset();
        req = r;
        gen++;
        nameEdited = false;
        probed = false;
        videoError.clear();
        formats.clear();
        quality.options = {"Getting qualities…"};
        quality.select(0);
        url.set(r.url);
        site = urlSite(r.url);
        fileName = initialName();
        size = -1;
        updatePath();
        refreshFooter();
        layout();
        redraw();
        probe();
        SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    static std::string sizeLabel(const VideoFormat& f) { return f.size > 0 ? (f.exact ? "" : "~") + formatBytes(f.size) : ""; }

    void applyFormat(int i) {
        if (i < 0 || i >= static_cast<int>(formats.size())) return;
        const VideoFormat& f = formats[static_cast<size_t>(i)];
        // the video's real name: the site's title without view counts, hashtags and "| Facebook"; else its caption,
        // the page's title, or "<uploader> - <id>"
        std::string title = niceVideoTitle(vinfo.rawTitle.empty() ? vinfo.title : vinfo.rawTitle, vinfo.description, vinfo.uploader, req.title, vinfo.videoId);
        if (!nameEdited) fileName = sanitizeFileName(title) + "." + (f.ext.empty() ? "mp4" : f.ext);
        size = f.size > 0 ? f.size : -1;
        sizeExact = f.exact;
        setCategory(f.group == "Audio" ? "music" : "video");
        refreshFooter();
        redraw();
        startQuietly();
    }

    // the video's thumbnail (JPEG preferred: WebP needs a codec older Windows doesn't have)
    void fetchThumb(const std::string& url0) {
        if (url0.empty()) return;
        HWND hw = h;
        int g0 = gen;
        RequestInfo ri;
        ri.url = url0; ri.userAgent = req.userAgent; ri.referer = req.pageUrl;
        std::thread([hw, g0, ri] {
            std::vector<std::string> tries = {ri.url};
            std::string jpg = replaceAll(replaceAll(ri.url, "/vi_webp/", "/vi/"), ".webp", ".jpg");
            if (jpg != ri.url) tries.insert(tries.begin(), jpg);
            for (auto& u : tries) {
                RequestInfo r = ri; r.url = u;
                std::string body;
                if (http::get(r, body, nullptr, 6 * 1024 * 1024, 15) != 200 || body.size() < 200) continue;
                auto img = loadImage(body);
                if (!img) continue;
                auto* p = new std::shared_ptr<void>(img);
                if (!IsWindow(hw) || !PostMessageW(hw, WM_APP + 27, static_cast<WPARAM>(g0), reinterpret_cast<LPARAM>(p))) delete p;
                return;
            }
        }).detach();
    }

    void videoProbed(VideoInfo* vi) {
        probed = true;
        vinfo = *vi;
        subsReady();
        fetchThumb(vinfo.thumbnail);
        formats = offeredFormats(vinfo, req);
        videoError = formats.empty() ? vinfo.error : "";
        std::vector<std::string> names;
        for (auto& f : formats) names.push_back(formatName(f));
        if (names.empty()) names.push_back("Best available");
        quality.options = names;
        int pick = 0;
        std::string want = req.quality.empty() ? settings().videoQuality : req.quality;   // the button's "Best quality" / "Sound only"
        if (want == "mp3") { for (size_t i = 0; i < formats.size(); i++) if (formats[i].convert == "mp3") { pick = static_cast<int>(i); break; } }
        else if (want != "ask" && want != "best") {
            int cap = atoi(want.c_str());
            for (size_t i = 0; i < formats.size(); i++) if (formats[i].group == "Video" && formats[i].height > 0 && formats[i].height <= cap) { pick = static_cast<int>(i); break; }
        }
        if (req.pick >= 0 && req.pick < static_cast<int>(formats.size())) pick = req.pick;   // chosen with the browser's button
        quality.select(pick);
        if (!formats.empty()) applyFormat(pick);
        for (size_t i = 0; i < formats.size(); i++) {   // plain files without a size: ask the server
            if (!formats[i].direct || formats[i].size > 0) continue;
            RequestInfo ri;
            ri.url = formats[i].directUrl; ri.referer = req.url; ri.cookies = req.directCookies.empty() ? req.cookies : req.directCookies; ri.userAgent = req.userAgent;
            for (auto& kv : formats[i].headers) if (lower(kv.first) != "cookie") ri.headers[kv.first] = kv.second;
            HWND hw = h;
            int g0 = gen;
            size_t idx = i;
            std::thread([hw, g0, idx, ri] {
                ResponseInfo info;
                std::string err;
                if (!ddm::probe(ri, info, &err) || info.total <= 0) return;
                auto* sz = new std::pair<size_t, int64_t>(idx, info.total);
                if (!IsWindow(hw) || !PostMessageW(hw, WM_APP + 26, static_cast<WPARAM>(g0), reinterpret_cast<LPARAM>(sz))) delete sz;
            }).detach();
        }
        refreshFooter();
        layout();
        redraw();
    }

    void setCategory(const std::string& key) {
        for (size_t i = 0; i < catKeys.size(); i++) if (catKeys[i] == key) category.select(static_cast<int>(i));
        updatePath();
    }

    std::string catKey() const { return category.selected >= 0 && category.selected < static_cast<int>(catKeys.size()) ? catKeys[static_cast<size_t>(category.selected)] : "general"; }

    // The file this window would download is already on the disk (in the chosen folder, or finished in the list).
    // the file the window's own early start is writing doesn't count as "already downloaded"
    bool ours(const std::string& p) {
        if (preId.empty()) return false;
        Task pt;
        return Engine::get().find(preId, pt) && iequals(pt.path(), p);
    }
    std::string existingPath() {
        std::string p = trim(saveAs.get());
        if (!p.empty() && fileExists(p) && !ours(p)) return p;
        Task d;
        if (Engine::get().findUrl(trim(url.get()), d) && d.state == State::Complete && fileExists(d.path())) {
            // a page can hold many videos (feeds, playlists): the same page is the same video only with the same title
            if (isVideo() && lower(stem(d.fileName)) != lower(stem(fileName))) return "";
            return d.path();
        }
        return "";
    }
    void refreshExisting() {
        bool has = !existingPath().empty();
        if (has != (IsWindowVisible(existing.hwnd()) != FALSE)) { existing.show(has); layout(); }
    }

    // the download exactly as the window describes it now
    Task makeTask(const std::string& link, const std::string& target) const {
        Task t;
        t.kind = req.kind;
        t.url = link;
        t.pageUrl = req.pageUrl.empty() ? req.referer : req.pageUrl;
        t.cookies = req.cookies;
        t.userAgent = req.userAgent;
        t.headers = req.headers;
        t.fileName = baseName(target);
        t.dir = dirName(target);
        t.category = catKey();
        t.description = trim(desc.get());
        t.site = site;
        t.favicon = req.favicon;
        t.videoFormat = req.videoFormat;
        t.total = size;
        t.resumable = ranges;
        if (isVideo()) {
            t.pageUrl = link;
            t.total = -1;
            t.extras = std::string(wantThumb.checked ? "thumb" : "") + (wantThumb.checked && wantSubs.checked ? "," : "") + (wantSubs.checked ? "subs" : "");
            if (wantSubs.checked && !vinfo.subs.empty()) {
                std::vector<SubTrack> picked;
                for (size_t i : subPicked) if (i < vinfo.subs.size()) picked.push_back(vinfo.subs[i]);
                t.subs = packSubs(picked);
            }
            t.resumable = true;
            t.notBefore = vinfo.notBefore;
            if (quality.selected >= 0 && quality.selected < static_cast<int>(formats.size())) {
                const VideoFormat& f = formats[static_cast<size_t>(quality.selected)];
                t.videoFormat = f.selector;
                if (!f.streamUrl.empty()) t.url = f.streamUrl;   // the stream itself (t.pageUrl keeps the page, sent as its Referer)
                for (auto& kv : f.headers)   // what the site's files expect (its Referer, a cookie the page set)
                    if (lower(kv.first) == "cookie") { if (t.cookies.find(kv.second) == std::string::npos) t.cookies = t.cookies.empty() ? kv.second : t.cookies + "; " + kv.second; }
                    else if (f.audioUrl.empty() && !(f.direct && f.convert.empty())) t.headers[kv.first] = kv.second;
                t.convertTo = f.convert;
                t.formatLabel = f.label;
                if (!f.audioUrl.empty()) {   // picture and sound separately: both at full speed, joined after
                    t.mediaUrl = f.videoUrl; t.audioUrl = f.audioUrl; t.mediaExt = f.videoExt; t.audioExt = f.audioExt;
                    for (auto& kv : f.headers) if (lower(kv.first) != "cookie") t.headers[kv.first] = kv.second;
                    if (f.size > 0) t.total = f.size;
                } else if (f.direct && f.convert.empty()) {
                    t.mediaUrl = f.directUrl;
                    for (auto& kv : f.headers) if (lower(kv.first) != "cookie") t.headers[kv.first] = kv.second;
                    if (f.directUrl == req.direct && !req.directCookies.empty()) t.cookies = req.directCookies;
                }
            } else if (!req.direct.empty() && !startsWith(req.direct, "blob:")) t.mediaUrl = req.direct;
        }
        if (req.kind == "torrent") { t.category = "torrent"; t.total = -1; }
        return t;
    }
    static std::string keyOf(const Task& t) { return t.url + "\x1f" + t.videoFormat + "\x1f" + t.convertTo + "\x1f" + t.mediaUrl + "\x1f" + t.audioUrl; }

    // Starts the download as soon as the window opens (32 connections), so Start Download carries on from what
    // already arrived. Nothing in the window says so. Torrents wait for Start (they would share data before you agree).
    void startQuietly() {
        if (req.kind == "torrent" || !looksLikeUrl(trim(url.get())) || !existingPath().empty()) { dropQuiet(); return; }
        if (isVideo() && (!probed || formats.empty())) return;            // after the quality is known
        if (req.kind == "mega" && !probed) return;                        // after MEGA tells the file's name
        std::string target = trim(saveAs.get());
        if (target.empty() || (fileExists(target) && !ours(target))) { dropQuiet(); return; }   // already on the disk: nothing to fetch
        Task t = makeTask(trim(url.get()), target);
        std::string key = keyOf(t);
        if (key == preKey && !preId.empty()) return;
        dropQuiet();
        preId = Engine::get().preStart(t);
        preKey = key;
    }
    // the link check (Options → Security): a coloured pill in the footer, details on click
    void pollSafety() {
        if (!settings().linkCheck || preId.empty()) { if (IsWindowVisible(safety.hwnd())) { safety.show(false); layout(); } return; }
        if (repFor != preId) {
            Task t;
            if (!Engine::get().find(preId, t)) return;
            inspect::start(t);
            repFor = preId;
            linkRep = inspect::Report();
        }
        inspect::Report r;
        if (!inspect::get(preId, r) || r.level == inspect::Level::Unknown) return;
        bool changed = r.level != linkRep.level || r.findings.size() != linkRep.findings.size() || !IsWindowVisible(safety.hwnd());
        linkRep = r;
        if (!changed) return;
        safety.setText(r.level == inspect::Level::Safe ? "Safe link" : r.level == inspect::Level::Caution ? "Be careful" : "Dangerous");
        safety.setTint(r.level == inspect::Level::Safe ? P.good : r.level == inspect::Level::Caution ? P.warn : P.bad);
        safety.show(true);
        layout();
    }
    void explainSafety() {
        std::string text;
        for (int pass = 0; pass < 3; pass++)   // dangers first, then warnings, then the good signs
            for (auto& f : linkRep.findings) if (f.level == (pass == 0 ? inspect::Level::Danger : pass == 1 ? inspect::Level::Caution : inspect::Level::Safe)) text += "• " + f.text + "\n";
        if (text.empty()) text = "No warning signs: the site isn't new, isn't listed for spreading malware and doesn't imitate a well-known name.\n";
        if (!linkRep.fileDone) text += "\nDDM is still looking at the file itself.";
        askBox(h, linkRep.level == inspect::Level::Danger ? "Dangerous download" : linkRep.level == inspect::Level::Caution ? "Be careful with this download" : "Link check", text, {"OK"}, 0);
    }

    void dropQuiet() { if (!preId.empty() && !confirmed) Engine::get().preCancel(preId); preId.clear(); preKey.clear(); }

    XRayRequest xrayRequest() const {
        XRayRequest x;
        x.url = trim(url.get());
        x.referer = req.pageUrl.empty() ? req.referer : req.pageUrl;
        x.cookies = req.cookies; x.userAgent = req.userAgent; x.headers = req.headers;
        x.name = req.kind == "torrent" ? (endsWith(lower(fileName), ".torrent") ? fileName : fileName + ".torrent") : fileName;
        return x;
    }

    void openXRay() {
        XRayRequest x = xrayRequest();
        HWND me = h;
        showXRay(h, x, [me, this](int res, const XRayRequest& r) {
            if (!IsWindow(me)) return;
            if (res == 1 && req.kind == "torrent") { req.videoFormat = r.selectFiles; go(true); }   // the torrent with only the chosen files
            else if (res == 1) close(0);                                                          // the chosen files are saved: nothing else to download
            else if (res == 2) { SetForegroundWindow(me); SetFocus(start.hwnd()); }
        });
    }

    void updatePath() {
        std::string dir = folderByRule(site);
        if (dir.empty()) dir = folderFor(catKey());
        saveAs.set(joinPath(dir, fileName));
        if (existing.hwnd()) refreshExisting();
        redraw();
    }

    void addCategory() {
        std::string name;
        if (!inputBox(h, "New category", "Name (a folder with this name is made in Downloads)", name)) return;
        Category c;
        c.key = lower(sanitizeFileName(name));
        c.name = name;
        auto& cats = settings().categories;
        if (std::none_of(cats.begin(), cats.end(), [&](const Category& x) { return x.key == c.key; })) cats.push_back(c);
        saveSettings();
        catKeys.insert(catKeys.end() - 1, c.key);
        category.options.insert(category.options.end() - 1, c.name);
        setCategory(c.key);
    }

    void pickPath() {
        std::wstring file = W(baseName(saveAs.get()));
        file.resize(MAX_PATH * 2);
        std::wstring dir = W(dirName(saveAs.get()));
        OPENFILENAMEW of{};
        of.lStructSize = sizeof of;
        of.hwndOwner = h;
        of.lpstrFile = &file[0];
        of.nMaxFile = static_cast<DWORD>(file.size());
        of.lpstrInitialDir = dir.c_str();
        of.lpstrFilter = L"All files\0*.*\0";
        of.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&of)) { saveAs.set(U(file.c_str())); nameEdited = true; }
    }

    void probe() {
        HWND hw0 = h;
        int g0 = gen;
        if (isVideo() && req.probed) {   // the browser's button already read the video: no second wait
            auto* vi = new VideoInfo(*static_cast<VideoInfo*>(req.probed.get()));
            req.probed.reset();
            PostMessageW(hw0, WM_APP + 21, static_cast<WPARAM>(g0), reinterpret_cast<LPARAM>(vi));
            return;
        }
        if (isVideo()) {
            VideoRequest vr;
            vr.url = trim(url.get()); vr.cookies = req.cookies; vr.userAgent = req.userAgent; vr.referer = req.pageUrl; vr.streams = req.streams; vr.seen = req.seen; vr.height = req.playHeight; vr.title = req.title; vr.duration = req.duration; vr.framed = !req.topUrl.empty(); vr.topUrl = req.topUrl; vr.direct = req.direct;
            if (trim(url.get()) == req.url) vr.pageData = req.pageData;
            std::thread([hw0, g0, vr] {
                auto* vi = new VideoInfo();
                probeVideo(vr, *vi, [hw0, g0](const std::string& st) { if (IsWindow(hw0)) PostMessageW(hw0, WM_APP + 23, static_cast<WPARAM>(g0), reinterpret_cast<LPARAM>(new std::string(st))); });
                if (!IsWindow(hw0) || !PostMessageW(hw0, WM_APP + 21, static_cast<WPARAM>(g0), reinterpret_cast<LPARAM>(vi))) delete vi;
            }).detach();
            refreshFooter();
            return;
        }
        if (req.kind == "mega") {
            std::string link = trim(url.get());
            std::thread([hw0, g0, link] {
                auto* e = new std::pair<MegaEntry, std::string>();
                megaInfo(link, e->first, e->second);
                if (!IsWindow(hw0) || !PostMessageW(hw0, WM_APP + 22, static_cast<WPARAM>(g0), reinterpret_cast<LPARAM>(e))) delete e;
            }).detach();
            refreshFooter();
            return;
        }
        if (req.kind != "http" || !looksLikeUrl(url.get()) || startsWith(lower(url.get()), "magnet:")) { probed = true; refreshFooter(); return; }
        RequestInfo r;
        r.url = trim(url.get()); r.referer = req.pageUrl; r.cookies = req.cookies; r.userAgent = req.userAgent; r.headers = req.headers;
        HWND hw = h;
        std::thread([hw, r] {
            auto* res = new std::pair<ResponseInfo, bool>();
            std::string err;
            res->second = ddm::probe(r, res->first, &err);
            if (!IsWindow(hw) || !PostMessageW(hw, WM_APP + 20, 0, reinterpret_cast<LPARAM>(res))) delete res;
        }).detach();
    }

    void probed_(ResponseInfo& info, bool ok) {
        probed = true;
        if (ok) {
            if (info.total >= 0) size = info.total;
            ranges = info.ranges;
            std::string name = !info.fileName.empty() ? info.fileName : (req.suggestedName.empty() ? fileNameFromUrl(info.finalUrl.empty() ? req.url : info.finalUrl) : req.suggestedName);
            name = sanitizeFileName(name);
            if (extOf(name).empty() && !info.mime.empty()) {
                static const std::map<std::string, std::string> mimeExt = {{"application/pdf", "pdf"}, {"application/zip", "zip"}, {"video/mp4", "mp4"}, {"audio/mpeg", "mp3"},
                    {"application/x-msdownload", "exe"}, {"application/x-msdos-program", "exe"}, {"application/x-rar-compressed", "rar"}, {"application/x-7z-compressed", "7z"},
                    {"image/jpeg", "jpg"}, {"image/png", "png"}, {"video/webm", "webm"}, {"application/x-iso9660-image", "iso"}};
                auto it = mimeExt.find(info.mime);
                if (it != mimeExt.end()) name += "." + it->second;
            }
            if (!nameEdited && !name.empty() && name != "download") { fileName = name; setCategory(categoryFor(fileName)); }
        }
        refreshFooter();
        redraw();
    }

    void refreshFooter() {
        {   // X-Ray: archives, disc images, torrents and programs can be looked inside first
            std::string link = trim(url.get());
            bool can = (req.kind == "http" && xray::canLook(fileName)) || (req.kind == "torrent" && !isMagnet(link));
            if (can != (IsWindowVisible(xray.hwnd()) != FALSE)) { xray.show(can); layout(); }
            if (can && (probed || req.kind == "torrent")) warmXRay(xrayRequest());   // read ahead: X-Ray then opens at once
        }
        footHost.setText(site);
        if (!videoError.empty()) { footResume.setText(videoError); footResume.setColor(P.warn); }
        else if (!probed && probeStatus.empty()) { footResume.setText(isVideo() ? "Getting qualities…" : "Checking…"); footResume.setColor(P.muted); }
        else if (!probed) { footResume.setText(probeStatus); footResume.setColor(P.muted); }
        else if (req.kind == "torrent") { footResume.setText(isMagnet(req.url) ? "Magnet link" : "Torrent"); footResume.setColor(P.muted); }
        else if (req.kind == "video" || req.kind == "mega") { footResume.setText("Resume: yes"); footResume.setColor(P.good); }
        else if (req.kind != "http") { footResume.setText(""); }
        else { footResume.setText(ranges ? "Resume: yes" : "Resume: no"); footResume.setColor(ranges ? P.good : P.warn); }
        std::string e = extOf(fileName);
        typeText = e.empty() ? "File" : (e == "zip" || e == "rar" || e == "7z" ? upper(e) + " archive" : upper(e) + " file");
        if (req.kind == "torrent") typeText = "Torrent";
        if (isVideo() && quality.selected >= 0 && quality.selected < static_cast<int>(formats.size())) {
            const VideoFormat& f = formats[static_cast<size_t>(quality.selected)];
            typeText = f.group == "Audio" ? upper(f.ext) + " sound" : upper(f.ext) + " video" + (f.label.find('p') != std::string::npos ? " · " + f.label : "");
        }
        // a size the server confirmed, an estimate ("About …"), or "Unknown size" when nobody can tell
        sizeText = size >= 0 ? (isVideo() && !sizeExact ? "About " + formatBytes(size) : formatBytes(size)) : (probed ? "Unknown size" : "…");
    }

    static std::string upper(std::string s) { for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c))); return s; }

    void go(bool now) {
        std::string path = trim(saveAs.get());
        std::string link = trim(url.get());
        if (link.empty()) { SetFocus(url.edit()); return; }
        if (linkRep.level == inspect::Level::Danger && settings().linkCheck) {
            std::string why;
            for (auto& f : linkRep.findings) if (f.level == inspect::Level::Danger) why += "• " + f.text + "\n";
            if (askBox(h, "Dangerous download", "DDM found signs that this file could harm your PC:\n\n" + why + "\nDownload it only if you trust where it comes from.", {"Cancel", "Download anyway"}, 0) != 1) return;
        }
        if (path.empty() || baseName(path).empty()) { SetFocus(saveAs.edit()); return; }
        std::string dir = dirName(path);
        if (!makeDirs(dir)) { askBox(h, "Save As", "DDM can't create the folder " + dir + ".", {"OK"}, 0); return; }
        std::string target = path;
        bool replace = false;
        if (fileExists(target) && !ours(target)) {
            int r = askBox(h, "File already downloaded", baseName(target) + " is already downloaded in this folder.", {"Open it", "Save with a new name", "Replace it", "Cancel"}, 0);
            if (r == 0) { ShellExecuteW(nullptr, L"open", W(target).c_str(), nullptr, W(dir).c_str(), SW_SHOWNORMAL); close(0); return; }
            if (r == 3 || r < 0) return;
            if (r == 1) target = uniquePath(target);
            else replace = true;
        }
        Task dup;
        if (Engine::get().findUrl(link, dup) && dup.state != State::Complete) {
            int r = askBox(h, "Already in the list", "This link is already in your downloads (" + dup.fileName + ").", {"Download again", "Show it", "Cancel"}, 1);
            if (r == 1) { App::get().showProgress(dup.id); close(0); return; }
            if (r != 0) return;
        }
        if (remember.checked) {
            for (auto& c : settings().categories) if (c.key == catKey() && dir != folderFor(c.key)) { c.folder = dir; saveSettings(); }
        }
        Task t = makeTask(link, target);
        if (replace) {   // the old copy goes to the Recycle Bin
            std::wstring from = W(target);
            from.push_back(0);
            SHFILEOPSTRUCTW op{};
            op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
            SHFileOperationW(&op);
        }
        std::string id;
        if (!preId.empty() && preKey == keyOf(t) && req.kind != "torrent") {   // carries on from what already arrived
            Engine::get().preConfirm(preId, t, now);
            id = preId;
            confirmed = true;
        } else id = Engine::get().add(t, now);
        if (!now) Engine::get().addToQueue(id, "main");   // Download Later: the Main download queue (runs with the Scheduler)
        close(0);
        if (now && settings().showProgress) App::get().showProgress(id);
    }

    void layout() override {
        RECT r = client();
        int pad = S(18), col = S(172), lw = S(78), fh = S(30), gap = S(10);
        int right = r.right - pad - col - S(18);
        int x = pad, y = S(16);
        int fx = x + lw + gap, fw = right - fx;
        lUrl.move(x, y, lw, fh); url.move(fx, y, fw, fh); y += fh + gap;
        if (isVideo()) { lQual.move(x, y, lw, fh); quality.move(fx, y, fw, fh); y += fh + gap; }
        lCat.move(x, y, lw, fh); category.move(fx, y, fw - fh - S(6), fh); newCat.move(fx + fw - fh, y, fh, fh); y += fh + gap;
        lSave.move(x, y, lw, fh); saveAs.move(fx, y, fw - fh - S(6), fh); browse.move(fx + fw - fh, y, fh, fh); y += fh + gap;
        remember.move(fx, y, fw, S(22));
        y += S(22) + gap;
        if (isVideo()) {   // also save: the thumbnail picture, the subtitles
            HDC tdc = GetDC(h);
            int tw = textSize(tdc, "Save the thumbnail", fontUI()).cx + S(34), sw = textSize(tdc, "Save the subtitles", fontUI()).cx + S(34);
            ReleaseDC(h, tdc);
            wantThumb.move(fx, y, tw, S(22));
            wantSubs.move(fx + tw + S(16), y, sw, S(22));
            y += S(22) + gap;
            if (IsWindowVisible(subsBtn.hwnd())) {   // which languages: its own line, under "Save the subtitles"
                int bx = fx + tw + S(16);
                subsBtn.move(bx, y - S(4), std::min(std::max(subsBtn.preferredWidth(), S(150)), fx + fw - bx), S(30));
                y += S(30) + gap - S(4);
            }
        }
        lDesc.move(x, y + S(4), lw, S(20)); desc.move(fx, y, fw, S(52));
        int cx = r.right - pad - col;
        int ty = S(16) + S(104) + S(8);
        start.move(cx, ty, col, S(38)); ty += S(38) + S(8);
        later.move(cx, ty, col, S(34)); ty += S(34) + S(8);
        cancel.move(cx, ty, col, S(34));
        int fy = r.bottom - S(40);
        footHost.move(pad + S(22), fy, S(200), S(40));
        SIZE hs{};
        HDC dc = GetDC(h);
        hs = textSize(dc, footHost.text.empty() ? site : footHost.text, fontSmall());
        ReleaseDC(h, dc);
        footHost.move(pad + S(22), fy + 1, hs.cx + S(4), S(40) - 1);   // below the footer's top line
        int ew = existing.preferredWidth();
        existing.move(r.right - pad - ew - (IsWindowVisible(xray.hwnd()) ? xray.preferredWidth() + S(8) : 0), r.bottom - S(40) + S(8), ew, S(24));
        int xw = xray.preferredWidth();
        int rightUsed = (IsWindowVisible(xray.hwnd()) ? xw + S(12) : 0) + (IsWindowVisible(existing.hwnd()) ? ew + S(12) : 0);
        if (IsWindowVisible(safety.hwnd())) {
            int sw = safety.preferredWidth();
            safety.move(r.right - pad - rightUsed - sw, fy + S(8), sw, S(24));
            rightUsed += sw + S(12);
        }
        footResume.move(pad + S(22) + hs.cx + S(24), fy + 1, std::max<int>(S(90), r.right - pad - rightUsed - (pad + S(22) + hs.cx + S(24))), S(40) - 1);
        xray.move(r.right - pad - xw, fy + S(8), xw, S(24));
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(18), col = S(172);
        RECT tile{r.right - pad - col, S(16), r.right - pad, S(16) + S(104)};
        fillRound(dc, tile, Sf(8), P.tile);
        if (thumb) drawImageCover(dc, tile, thumb, Sf(8), sizeText, typeText);   // the video's own picture
        else {
        Task fake;
        fake.fileName = fileName;
        fake.category = catKey();
        fake.kind = req.kind;
        RECT ic{(tile.left + tile.right) / 2 - S(22), tile.top + S(8), (tile.left + tile.right) / 2 + S(22), tile.top + S(52)};
        // Windows' own icon for this type of file (the program's icon for a .pdf, .docx, .exe…)
        if (fake.kind == "torrent" || fileName.empty() || extOf(fileName).empty() || !systemFileIcon(dc, ic, fileName, false)) fileIcon(dc, ic, typeOf(fake.category, fake.fileName, fake.kind));
        RECT st{tile.left, tile.top + S(54), tile.right, tile.top + S(80)};
        text(dc, sizeText, st, P.text, fontBig(), DT_CENTER);
        RECT tt{tile.left, tile.top + S(80), tile.right, tile.top + S(98)};
        text(dc, typeText, tt, P.muted, fontSmall(), DT_CENTER);
        }
        // footer
        RECT f{0, r.bottom - S(40), r.right, r.bottom};
        fill(dc, f, P.foot);
        RECT ln{0, r.bottom - S(40), r.right, r.bottom - S(40) + 1};
        fill(dc, ln, P.line);
        RECT fav{pad, r.bottom - S(27), pad + S(14), r.bottom - S(13)};
        if (!drawFavicon(dc, fav, site)) siteBadge(dc, fav, site);
        HDC mdc = dc;
        SIZE hs = textSize(mdc, site, fontSmall());
        RECT dot{pad + S(22) + hs.cx + S(6), r.bottom - S(40), pad + S(22) + hs.cx + S(18), r.bottom};
        text(dc, "·", dot, P.muted, fontSmall(), DT_CENTER);
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        // X, Esc and – : the window turns to dust (Options → General), then closes or minimizes as always
        if (m == WM_CLOSE) { dust::window(h); close(-1); return 0; }
        if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_MINIMIZE) dust::window(h);
        if (m == WM_APP + 20) {
            auto res = reinterpret_cast<std::pair<ResponseInfo, bool>*>(l);
            probed_(res->first, res->second);
            delete res;
            layout();
            return 0;
        }
        if (m == WM_APP + 21) {
            auto vi = reinterpret_cast<VideoInfo*>(l);
            if (static_cast<int>(w) == gen) videoProbed(vi);
            delete vi;
            return 0;
        }
        if (m == WM_APP + 22) {
            auto e = reinterpret_cast<std::pair<MegaEntry, std::string>*>(l);
            if (static_cast<int>(w) == gen) {
                probed = true;
                if (e->second.empty()) { if (!nameEdited) fileName = e->first.name; size = e->first.size; setCategory(categoryFor(fileName)); videoError.clear(); }
                else videoError = e->second;
                refreshFooter();
                layout();
                redraw();
                if (e->second.empty()) startQuietly();
            }
            delete e;
            return 0;
        }
        if (m == WM_APP + 27) {
            std::unique_ptr<std::shared_ptr<void>> p(reinterpret_cast<std::shared_ptr<void>*>(l));
            if (static_cast<int>(w) == gen) { thumb = *p; redraw(); }
            return 0;
        }
        if (m == WM_APP + 26) {
            auto sz = reinterpret_cast<std::pair<size_t, int64_t>*>(l);
            if (static_cast<int>(w) == gen && sz->first < formats.size()) {
                formats[sz->first].size = sz->second;
                formats[sz->first].exact = true;
                std::string& o = quality.options[sz->first];
                const std::string unknown = "size unknown";
                if (o.size() >= unknown.size() && o.compare(o.size() - unknown.size(), unknown.size(), unknown) == 0) o = o.substr(0, o.size() - unknown.size()) + formatBytes(sz->second);
                else if (o.find("  ·  ~") == std::string::npos && o.rfind(" B") != o.size() - 2) o += "  ·  " + formatBytes(sz->second);
                if (quality.selected == static_cast<int>(sz->first)) { size = sz->second; sizeExact = true; refreshFooter(); redraw(); }
                quality.redraw();
            }
            delete sz;
            return 0;
        }
        if (m == WM_APP + 23) {
            auto st = reinterpret_cast<std::string*>(l);
            if (static_cast<int>(w) == gen && !probed) { probeStatus = *st; refreshFooter(); layout(); }
            delete st;
            return 0;
        }
        if (m == WM_TIMER && w == 7) { pollSafety(); return 0; }
        if (m == WM_TIMER && w == 2) { KillTimer(h, 2); gen++; if (req.kind != "http") dropQuiet(); probe(); startQuietly(); refreshExisting(); return 0; }
        if (m == WM_DDM_BG) {   // the pills in the footer sit on the footer's colour
            RECT cr{}, me{};
            if (w) { GetWindowRect(reinterpret_cast<HWND>(w), &cr); MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&cr), 2); }
            GetClientRect(h, &me);
            return bgReply(w && cr.top >= me.bottom - S(40) ? P.foot : P.win);
        }
        return Dialog::proc(m, w, l);
    }
};
} // namespace

void showPrompt(const LinkRequest& r) {
    if (r.kind == "video" && r.tab >= 0) {
        auto it = g_tabPrompts.find(r.tab);
        if (it != g_tabPrompts.end() && IsWindow(it->second->hwnd())) {
            Prompt* p = it->second;
            if (p->req.url != r.url) p->replaceRequest(r);
            else if (r.pick >= 0 && r.pick < static_cast<int>(p->formats.size())) { p->quality.select(r.pick); p->applyFormat(r.pick); }   // another quality from the button
            SetWindowPos(p->hwnd(), HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(p->hwnd());
            return;
        }
    }
    for (auto p : g_prompts) if (p->req.url == r.url && p->req.kind == r.kind && IsWindow(p->hwnd())) { SetForegroundWindow(p->hwnd()); return; }
    auto* p = new Prompt();
    p->req = r;
    if (!p->createDialog(App::get().mainHwnd(), r.kind == "video" ? "Download Video" : (r.kind == "torrent" ? "Download Torrent" : "Download File Info"), S(640), S(352) + (r.kind == "video" ? S(40) : 0), true, true)) { delete p; return; }
    // – button: it has its own taskbar button, so it minimizes to the taskbar (an owned window would park in a screen corner)
    SetWindowLongPtrW(p->hwnd(), GWL_EXSTYLE, GetWindowLongPtrW(p->hwnd(), GWL_EXSTYLE) | WS_EX_APPWINDOW);
    p->build();
    ShowWindow(p->hwnd(), SW_SHOW);
    if (r.source == "browser") {   // on top of the browser, without stealing focus forever
        SetWindowPos(p->hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(p->hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    SetForegroundWindow(p->hwnd());
    // self-deleting: the window owns the object
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<Prompt*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(p->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(p));
}

// ================================================================ MEGA folder
namespace {
class MegaFolderDlg : public Dialog {
public:
    LinkRequest req;
    Label lInfo, lSave, lStatus;
    Field saveTo;
    Button browse, start, later, cancel;
    std::string folderName;
    std::vector<MegaEntry> files;
    bool ready = false;

    void build() {
        lInfo.create(h, "Reading the MEGA folder…");
        lInfo.bold = true;
        lSave.create(h, "Save to");
        lSave.setColor(P.muted);
        saveTo.create(h, folderFor("general"));
        browse.create(h, "…");
        lStatus.create(h, "");
        lStatus.setColor(P.muted);
        lStatus.wrap = true;
        start.create(h, "Start Download", BtnStyle::Primary, "down");
        later.create(h, "Download Later");
        cancel.create(h, "Cancel");
        start.onClick = [this] { go(true); };
        later.onClick = [this] { go(false); };
        cancel.onClick = [this] { close(-1); };
        browse.onClick = [this] {
            BROWSEINFOW bi{};
            bi.hwndOwner = h;
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
            if (!id) return;
            wchar_t path[MAX_PATH * 2] = {};
            if (SHGetPathFromIDListW(id, path)) saveTo.set(U(path));
            CoTaskMemFree(id);
        };
        onEnter = [this] { go(true); };
        layout();
        std::string link = req.url;
        HWND hw = h;
        std::thread([hw, link] {
            auto* res = new std::tuple<std::string, std::vector<MegaEntry>, std::string>();
            megaFolder(link, std::get<0>(*res), std::get<1>(*res), std::get<2>(*res));
            if (!IsWindow(hw) || !PostMessageW(hw, WM_APP + 24, 0, reinterpret_cast<LPARAM>(res))) delete res;
        }).detach();
    }

    void go(bool now) {
        if (!ready) return;
        std::string base = joinPath(trim(saveTo.get()), folderName);
        for (auto& f : files) {
            Task t;
            t.kind = "mega";
            t.url = f.url;
            t.pageUrl = req.url;
            t.fileName = f.name;
            t.dir = f.path.empty() ? base : joinPath(base, f.path);
            t.category = categoryFor(f.name);
            t.site = "mega.nz";
            t.favicon = req.favicon;
            t.total = f.size;
            Engine::get().add(t, now);
        }
        close(0);
    }

    void layout() override {
        RECT r = client();
        int pad = S(18);
        lInfo.move(pad, S(16), r.right - 2 * pad, S(22));
        lStatus.move(pad, S(42), r.right - 2 * pad, S(40));
        lSave.move(pad, S(92), S(70), S(30));
        saveTo.move(pad + S(78), S(92), r.right - 2 * pad - S(78) - S(36), S(30));
        browse.move(r.right - pad - S(30), S(92), S(30), S(30));
        int y = r.bottom - S(52), x = r.right - pad;
        for (Button* b : {&cancel, &later, &start}) { int w = std::max(S(110), b->preferredWidth()); x -= w; b->move(x, y, w, S(36)); x -= S(8); }
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_APP + 24) {
            auto res = reinterpret_cast<std::tuple<std::string, std::vector<MegaEntry>, std::string>*>(l);
            folderName = std::get<0>(*res);
            files = std::get<1>(*res);
            std::string err = std::get<2>(*res);
            delete res;
            if (!err.empty() || files.empty()) { lInfo.setText("Couldn't open this MEGA folder"); lStatus.setText(err.empty() ? "The folder is empty." : err); lStatus.setColor(P.warn); return 0; }
            int64_t total = 0;
            for (auto& f : files) total += std::max<int64_t>(0, f.size);
            ready = true;
            lInfo.setText(folderName);
            lStatus.setText(std::to_string(files.size()) + (files.size() == 1 ? " file · " : " files · ") + formatBytes(total) + " · saved in a folder named \"" + folderName + "\"");
            return 0;
        }
        return Dialog::proc(m, w, l);
    }
};
} // namespace

void showMegaFolder(const LinkRequest& r) {
    auto* d = new MegaFolderDlg();
    d->req = r;
    if (!d->createDialog(App::get().mainHwnd(), "Download MEGA Folder", S(560), S(196), true)) { delete d; return; }
    d->build();
    ShowWindow(d->hwnd(), SW_SHOW);
    SetForegroundWindow(d->hwnd());
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<MegaFolderDlg*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
}

// ================================================================ Add URL
void showAddUrl(HWND owner) {
    std::string clip;
    if (OpenClipboard(owner)) {
        HANDLE d = GetClipboardData(CF_UNICODETEXT);
        if (d) { auto p = static_cast<const wchar_t*>(GlobalLock(d)); if (p) clip = U(p); GlobalUnlock(d); }
        CloseClipboard();
    }
    auto urls = extractUrls(clip);
    std::string value = urls.empty() ? "" : urls.front();
    if (!inputBox(owner, "Add URL", "Address of the file to download", value)) return;
    auto found = extractUrls(value);
    if (found.empty() && !value.empty()) found.push_back(value.find("://") == std::string::npos ? "https://" + value : value);
    for (auto& u : found) { LinkRequest r; r.url = u; r.source = "paste"; App::get().newDownload(r); }
}

// ================================================================ progress window
std::string algoName(const std::string& a);
namespace {
void trayHint();
// The download window ("Velocity"): a speedometer with the transfer rate, progress, the numbers that matter, the
// speed over the last seconds and each part of the file, in DDM's light or dark look. Included by dialogs.cpp.
class ProgressWnd : public Dialog {
public:
    std::string id;
    Task t;
    int tab = 0;                 // 0 status, 1 speed limiter, 2 when done, 3 security
    bool details = true;
    bool inTray = false;         // hidden by its – button: the tray icon brings it back
    Button hide, pause, cancel, refreshB, trayB;
    CheckBox limitOn, showDone, openDone, shutDone;
    std::vector<int> limits = {64, 128, 256, 512, 1024, 2048, 5120, 10240};
    int limitSel = 3;            // the chosen speed (one of limits)
    std::vector<std::unique_ptr<Button>> limitBtns;
    std::vector<RECT> tabRects;
    std::vector<int> tabKinds;   // the tabs shown (Options → General can hide all but the first)
    inspect::Report report;
    inspect::LiveHash hash;

    // ---- the look: light or dark, following DDM's theme
    struct Look {
        COLORREF bg, card, cardLine, track, text, muted, accent, onAccent, tabOn, tabOnLine, tabLine, good, warn, bad, footLine, tick, tickLabel, cellOff, needle, soft;
    };
    Look V{};
    int lookFor = -1;
    HFONT fNum = nullptr, fNum2 = nullptr, fCap = nullptr, fMono = nullptr, fTick = nullptr, fName = nullptr, fLabel = nullptr;
    int fontDpi = 0;
    std::wstring fontFace;
    double peak = 0;                     // the fastest speed so far
    float phase = 0;                     // the progress bar's stripes slide while it downloads
    // ---- its own title bar (like IDM's): icon, title and four buttons — Minimize to system tray, –, □, ×.
    // Windows draws a normal title bar itself and has no place for a fourth button, so with the desktop's
    // composition on (Windows 7 Aero, 8, 10, 11) DDM draws the title bar; without it (Windows 7 Basic or
    // Classic) Windows' own stays and the tray button sits at the window's top right instead.
    static bool composited() { BOOL on = FALSE; return SUCCEEDED(DwmIsCompositionEnabled(&on)) && on; }
    static bool wantOwnCaption() {
        if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_FRAME", nullptr, 0)) return true;   // (tests)
        return composited();
    }
    bool ownCaption = wantOwnCaption();
    bool active = true;
    int capHover = -1, capPress = -1;
    HWND capTip = nullptr;
    HDC bodyDC = nullptr; HBITMAP bodyBmp = nullptr; HGDIOBJ bodyOld = nullptr; int bodyW = 0, bodyHt = 0;
    int capH() const { return ownCaption ? S(32) : 0; }
    RECT capBtn(int i) const {   // 0 tray, 1 minimize, 2 maximize / restore, 3 close
        RECT r{}; GetClientRect(h, &r);
        int w = S(46), x = r.right - (4 - i) * w;
        return RECT{x, 0, x + w, capH()};
    }
    unsigned ticks = 0;
    static const UINT kSysTray = 0x0E10; // the window menu's "Minimize to system tray"

    ~ProgressWnd() {
        trayRemove(); freeFonts();
        if (bodyDC) { SelectObject(bodyDC, bodyOld); DeleteObject(bodyBmp); DeleteDC(bodyDC); }
    }

    // ---- its own tray icon while it's in the tray (like IDM): progress in the icon, name / % / speed in its tip;
    // double-click brings the window back, right-click: Show, Pause / Resume, Stop, Cancel
    static const UINT kTrayMsg = WM_APP + 89;
    NOTIFYICONDATAW nid{};
    bool trayOn = false;
    int iconPct = -2, iconState = -1;
    static UINT newTrayId() { static UINT n = 100; return ++n; }

    static HICON progressIcon(double p, int state) {   // state: 0 running, 1 paused / waiting, 2 error
        int sz = GetSystemMetrics(SM_CXSMICON);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = sz;
        bi.bmiHeader.biHeight = -sz;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!color) { ReleaseDC(nullptr, screen); return nullptr; }
        HDC dc = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(dc, color);
        memset(bits, 0, static_cast<size_t>(sz) * sz * 4);
        COLORREF fillC = state == 2 ? RGB(239, 68, 68) : state == 1 ? RGB(245, 158, 11) : RGB(34, 197, 94);
        RECT all{0, 0, sz, sz};
        fillRound(dc, all, sz / 5.0f, RGB(30, 41, 59));
        if (p > 0) {   // fills from the bottom as it arrives
            RECT f{1, sz - 1 - static_cast<LONG>((sz - 2) * std::min(1.0, p)), sz - 1, sz - 1};
            if (f.bottom - f.top < 2) f.top = f.bottom - 2;
            fillRound(dc, f, sz / 7.0f, fillC);
        }
        RECT a{sz / 5, sz / 8, sz - sz / 5, sz - sz / 8};
        icon(dc, state == 1 ? "M8 5v14M16 5v14" : "M12 4v14 M6 12l6 6 6-6", a, RGB(255, 255, 255), std::max(1.5f, sz / 8.0f));
        SelectObject(dc, old);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
        ICONINFO ii{TRUE, 0, 0, mask, color};
        HICON ic = CreateIconIndirect(&ii);
        DeleteObject(mask);
        DeleteObject(color);
        return ic;
    }
    void trayTip() {
        std::string state = t.state == State::Running ? (t.speed > 0 ? formatKBps(t.speed) : "Connecting…") : t.state == State::Paused ? (t.stopped ? "Stopped" : "Paused") : t.state == State::Queued ? "Waiting" : t.state == State::Error ? "Error" : "";
        std::string pct;
        if (t.total > 0) { char b[16]; snprintf(b, sizeof b, "%.1f%%", t.progress() * 100); pct = b; }
        std::wstring name = W(t.fileName);
        std::wstring rest = W(i18n::tr((pct.empty() ? "" : pct + " · ") + state + (t.state == State::Running && t.eta >= 0 ? " · " + formatDuration(t.eta) + " left" : "")));
        size_t room = 127 - std::min<size_t>(100, rest.size() + 1);
        if (name.size() > room) name = name.substr(0, room - 1) + L"…";
        std::wstring tip = name + L"\n" + rest;
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    }
    void trayAdd() {
        if (trayOn) { trayUpdate(); return; }
        if (!nid.uID) nid.uID = newTrayId();
        nid.cbSize = sizeof nid;
        nid.hWnd = h;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = kTrayMsg;
        iconState = t.state == State::Error ? 2 : t.state == State::Running ? 0 : 1;
        iconPct = t.total > 0 ? static_cast<int>(t.progress() * 100) : -1;
        nid.hIcon = progressIcon(iconPct >= 0 ? iconPct / 100.0 : 0, iconState);
        trayTip();
        trayOn = Shell_NotifyIconW(NIM_ADD, &nid) != 0;
        if (!trayOn && nid.hIcon) { DestroyIcon(nid.hIcon); nid.hIcon = nullptr; }
    }
    void trayUpdate() {
        if (!trayOn) return;
        trayTip();
        nid.uFlags = NIF_TIP;
        int st = t.state == State::Error ? 2 : t.state == State::Running ? 0 : 1;
        int pc = t.total > 0 ? static_cast<int>(t.progress() * 100) : -1;
        HICON old = nullptr;
        if (st != iconState || pc != iconPct) {
            iconState = st; iconPct = pc;
            old = nid.hIcon;
            nid.hIcon = progressIcon(pc >= 0 ? pc / 100.0 : 0, st);
            nid.uFlags |= NIF_ICON;
        }
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        if (old) DestroyIcon(old);
    }
    void trayRemove() {
        if (trayOn) { NOTIFYICONDATAW d{}; d.cbSize = sizeof d; d.hWnd = nid.hWnd; d.uID = nid.uID; Shell_NotifyIconW(NIM_DELETE, &d); }
        trayOn = false;
        if (nid.hIcon) { DestroyIcon(nid.hIcon); nid.hIcon = nullptr; }
        iconPct = -2; iconState = -1;
    }
    void toTray() {
        inTray = true;
        ShowWindow(h, SW_HIDE);
        if (!settings().trayOneIcon) trayAdd();   // (like IDM, normally: it joins DDM's own icon, which lists every download)
        trayHint();
    }
    void fromTray() {
        trayRemove();
        inTray = false;
        ShowWindow(h, SW_SHOWNORMAL);
        SetForegroundWindow(h);
    }
    void trayMenu() {
        Engine::get().find(id, t);
        bool running = t.state == State::Running || t.state == State::Queued;
        std::vector<MenuItem> v;
        MenuItem show{"Show", "", 1}; show.bold = true; v.push_back(show);
        v.push_back(MenuItem::sep());
        v.push_back({running ? "Pause" : "Resume", "", 2});
        MenuItem stopI{"Stop", "", 3}; stopI.disabled = !running; v.push_back(stopI);
        v.push_back(MenuItem::sep());
        v.push_back({"Cancel", "", 4});
        POINT pt; GetCursorPos(&pt);
        SetForegroundWindow(h);   // (so the menu closes when you click elsewhere)
        UINT c = popupMenu(h, v, pt);
        PostMessageW(h, WM_NULL, 0, 0);
        if (c == 1) fromTray();
        else if (c == 2) { if (running) Engine::get().pause(id); else Engine::get().start(id); update(); }
        else if (c == 3) { Engine::get().stop(id); update(); }
        else if (c == 4) { trayRemove(); Engine::get().pause(id); close(0); }
    }

    static int CALLBACK fontFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) { *reinterpret_cast<bool*>(p) = true; return 0; }
    static bool hasFont(const wchar_t* face) {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
        bool found = false;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &lf, fontFound, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, dc);
        return found;
    }
    void freeFonts() {
        for (HFONT* f : {&fNum, &fNum2, &fCap, &fMono, &fTick, &fName, &fLabel}) if (*f) { DeleteObject(*f); *f = nullptr; }
    }
    void makeFonts() {
        if (fontDpi == dpi() && fNum && fontFace == uiFace()) return;
        freeFonts();
        fontDpi = dpi();
        fontFace = uiFace();
        static const bool bahn = hasFont(L"Bahnschrift");   // Windows 10 and 11: a narrow, technical face for the numbers
        const wchar_t* face = uiFace();
        const wchar_t* num = bahn ? L"Bahnschrift" : face;
        auto mk = [](int px, int weight, const wchar_t* face) {
            return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
        };
        fNum = mk(42, FW_BOLD, num);
        fNum2 = mk(27, FW_BOLD, num);
        fCap = mk(11, FW_SEMIBOLD, face);
        fMono = mk(13, FW_NORMAL, L"Consolas");
        fTick = mk(10, FW_NORMAL, face);
        fName = mk(15, FW_SEMIBOLD, face);
        fLabel = mk(12, FW_SEMIBOLD, face);
    }

    void applyLook() {
        bool dark = isDark();
        if (lookFor == static_cast<int>(dark)) return;
        lookFor = dark;
        if (dark) V = {RGB(6, 10, 19), RGB(11, 18, 34), RGB(22, 34, 58), RGB(19, 28, 47), RGB(232, 238, 248), RGB(139, 154, 179), RGB(34, 211, 238), RGB(4, 18, 26),
                       RGB(16, 34, 58), RGB(31, 74, 107), RGB(22, 34, 58), RGB(134, 239, 172), RGB(251, 191, 36), RGB(253, 164, 175), RGB(18, 27, 45), RGB(43, 58, 87),
                       RGB(95, 111, 140), RGB(18, 26, 44), RGB(255, 255, 255), RGB(168, 180, 200)};
        else V = {RGB(255, 255, 255), RGB(245, 248, 252), RGB(226, 232, 240), RGB(230, 235, 243), RGB(15, 23, 42), RGB(84, 99, 122), RGB(8, 145, 178), RGB(255, 255, 255),
                  RGB(224, 244, 248), RGB(155, 216, 230), RGB(226, 232, 240), RGB(21, 128, 61), RGB(180, 83, 9), RGB(180, 35, 24), RGB(234, 238, 244), RGB(203, 213, 225),
                  RGB(100, 116, 139), RGB(230, 235, 243), RGB(15, 23, 42), RGB(71, 85, 105)};
        hide.colors(V.bg, V.soft, V.bg, S(8), true, V.accent);
        trayB.colors(V.bg, V.good, V.bg, S(8), true, V.accent);
        cancel.colors(V.bg, V.bad, V.bg, S(18), true, V.accent);
        pause.colors(V.accent, V.onAccent, V.accent, S(18), true, V.onAccent);
        refreshB.colors(V.card, V.text, V.cardLine, S(18), true, V.accent);
        for (CheckBox* c : {&limitOn, &showDone, &openDone, &shutDone}) c->colors(V.accent, V.text, V.bg, blend(V.muted, V.bg, dark ? 0.35f : 0.45f), V.onAccent);
        styleLimitButtons();
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    void styleLimitButtons() {
        for (size_t i = 0; i < limitBtns.size(); i++) {
            bool on = limitOn.checked && static_cast<int>(i) == limitSel;
            limitBtns[i]->colors(on ? V.accent : V.card, on ? V.onAccent : V.text, on ? V.accent : V.cardLine, S(12), true, on ? V.onAccent : V.accent);
        }
    }

    void pickTabs() {
        const Settings& st = settings();
        tabKinds = {0};
        if (st.tabLimiter) tabKinds.push_back(1);
        if (st.tabWhenDone) tabKinds.push_back(2);
        if (st.tabSecurity) tabKinds.push_back(3);
        if (std::find(tabKinds.begin(), tabKinds.end(), tab) == tabKinds.end()) tab = 0;
    }
    static const char* tabName(int k) { return k == 1 ? "Speed limiter" : k == 2 ? "When done" : k == 3 ? "Security" : "Download status"; }

    void build() {
        hide.create(h, "Hide details", BtnStyle::Normal, "chevronup");
        pause.create(h, "Pause", BtnStyle::Normal, "M8 5v14M16 5v14");
        cancel.create(h, "Cancel");
        refreshB.create(h, "Refresh download link", BtnStyle::Normal, "refresh");
        refreshB.show(false);
        refreshB.onClick = [this] { refreshLinkDialog(h, id); };
        // like IDM: a second "minimize" next to the title bar's own – that hides the window in the tray (its own icon)
        trayB.create(h, "", BtnStyle::Normal, "M12 3v11 M7 9.5l5 5 5-5 M4 20h16");
        trayB.onClick = [this] { dust::window(h); toTray(); };
        tooltip(h, trayB.hwnd(), "Minimize to system tray");
        trayB.show(!ownCaption);
        capTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, h, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (capTip) {
            static const char* tips[] = {"Minimize to system tray", "Minimize", "Maximize", "Close"};
            for (int i = 0; i < 4; i++) {
                std::wstring tw = W(i18n::tr(tips[i]));
                TOOLINFOW ti{};
                ti.cbSize = sizeof ti;
                ti.uFlags = TTF_SUBCLASS;
                ti.hwnd = h;
                ti.uId = static_cast<UINT_PTR>(i + 1);
                ti.rect = capBtn(i);
                ti.lpszText = &tw[0];
                SendMessageW(capTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            }
        }
        if (HMENU sm = GetSystemMenu(h, FALSE)) {   // and in the title bar's menu (right-click it, or Alt+Space)
            std::wstring item = W(i18n::tr("Minimize to system tray"));
            InsertMenuW(sm, SC_CLOSE, MF_BYCOMMAND | MF_STRING, kSysTray, item.c_str());
            InsertMenuW(sm, SC_CLOSE, MF_BYCOMMAND | MF_SEPARATOR, 0, nullptr);
        }
        limitOn.create(h, "Limit this download's speed", false);
        for (size_t i = 0; i < limits.size(); i++) {
            int k = limits[i];
            auto b = std::make_unique<Button>();
            b->create(h, k < 1024 ? std::to_string(k) + " KB/s" : std::to_string(k / 1024) + " MB/s");
            b->onClick = [this, i] { limitSel = static_cast<int>(i); if (!limitOn.checked) limitOn.set(true); applyLimit(); };
            limitBtns.push_back(std::move(b));
        }
        showDone.create(h, "Show \"Download complete\" when it finishes", settings().showComplete);
        openDone.create(h, "Open the file when it finishes", false);
        shutDone.create(h, "Shut down the computer when it finishes", false);
        hide.onClick = [this] { details = !details; hide.setText(details ? "Hide details" : "Show details"); hide.iconName = details ? "chevronup" : "chevron"; resize(); };
        pause.onClick = [this] {
            Task cur;
            if (!Engine::get().find(id, cur)) return;
            if (cur.state == State::Running || cur.state == State::Queued) Engine::get().pause(id); else Engine::get().start(id);
            update();
        };
        cancel.onClick = [this] { Engine::get().pause(id); dust::window(h); close(0); };
        shutDone.onChange = [this](bool) { applyLimit(); };
        limitOn.onChange = [this](bool) { applyLimit(); };
        openDone.onChange = [this](bool) { applyLimit(); };
        Task cur;
        if (Engine::get().find(id, cur)) {
            if (cur.limitKBps > 0) { limitOn.set(true); for (size_t i = 0; i < limits.size(); i++) if (limits[i] == cur.limitKBps) limitSel = static_cast<int>(i); }
            openDone.set(cur.openWhenDone);
            shutDone.set(cur.shutdownWhenDone);
        }
        makeFonts();
        applyLook();
        update();
        pickTabs();
        if (settings().linkCheck && Engine::get().find(id, cur)) inspect::start(cur);
        {   // a small screen (or big text): the details start hidden so Pause and Cancel stay on the screen
            SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);   // (its own title bar from the start)
            RECT wa{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            bool keep = details; details = true;
            SIZE full = outerSize();
            details = keep;
            if (wa.bottom > wa.top && full.cy > wa.bottom - wa.top) { details = false; hide.setText("Show details"); hide.iconName = "chevron"; }
            resize();
        }
        showTab(0);
        SetTimer(h, 1, 500, nullptr);
        SetTimer(h, 2, 33, nullptr);    // the needle and the numbers glide
    }
    void applyLimit() {
        Engine::get().setOptions(id, limitOn.checked ? limits[static_cast<size_t>(limitSel)] : 0, openDone.checked, shutDone.checked);
        styleLimitButtons();
    }

    int bodyH() const { return details ? S(482) : S(414); }
    SIZE outerSize() const {   // the window's size for bodyH() (with its own title bar: no Windows title bar on top)
        RECT r{0, 0, S(600), bodyH() + capH()};
        AdjustWindowRectEx(&r, static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE)), FALSE, 0);
        return SIZE{r.right - r.left, ownCaption ? r.bottom : r.bottom - r.top};
    }
    void resize() {
        SIZE o = outerSize();
        minSize = o;   // (it opens at its full size: without this it couldn't get shorter)
        SetWindowPos(h, nullptr, 0, 0, o.cx, o.cy, SWP_NOMOVE | SWP_NOZORDER);
        layout();
        redraw();
    }

    void showTab(int i) {
        tab = i;
        hide.show(tab == 0); pause.show(tab == 0); refreshB.show(tab == 0 && t.state == State::Error && t.kind != "torrent");
        cancel.show(tab != 3);   // the Security tab uses the whole window
        limitOn.show(tab == 1);
        for (auto& b : limitBtns) b->show(tab == 1);
        showDone.show(tab == 2); openDone.show(tab == 2); shutDone.show(tab == 2);
        layout();
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void update() {
        if (!Engine::get().find(id, t)) { close(0); return; }
        char title[64];
        if (t.total > 0) snprintf(title, sizeof title, "%d%% ", static_cast<int>(t.progress() * 100)); else title[0] = 0;
        std::wstring wt = W(i18n::tr(std::string(title)) + t.fileName);
        wchar_t cur[512] = {};
        GetWindowTextW(h, cur, 512);
        if (wt != cur) SetWindowTextW(h, wt.c_str());
        bool running = t.state == State::Running || t.state == State::Queued;
        std::string pt = running ? "Pause" : "Resume";
        if (pause.text != pt) { pause.setText(pt); pause.iconName = running ? "M8 5v14M16 5v14" : "M8 5l11 7-11 7z"; }
        bool canRefresh = tab == 0 && t.state == State::Error && t.kind != "torrent";
        if (canRefresh != (IsWindowVisible(refreshB.hwnd()) != FALSE)) { refreshB.show(canRefresh); layout(); }
        if (settings().linkCheck) inspect::get(id, report);
        if (tab == 3 || settings().tabSecurity) hash = inspect::liveHash(t);
        double now = t.state == State::Running ? t.speed : 0;
        peak = std::max(peak, now);
        if (inTray) { if (settings().trayOneIcon) { if (trayOn) trayRemove(); } else trayAdd(); }   // (follows Options → General)
        if (t.state == State::Complete) {
            trayRemove();
            KillTimer(h, 1);
            KillTimer(h, 2);
            bool showIt = showDone.checked;
            std::string doneId = id;
            close(0);
            if (showIt) App::get().showComplete(doneId);
            return;
        }
        redraw();
    }

    void layout() override {
        RECT r = client();
        int pad = S(18), right = r.right - pad, T = capH();
        int fy = T + bodyH() - S(58) + S(11);
        int hw = std::max(S(126), hide.preferredWidth() + S(8));
        hide.move(pad - S(6), fy, hw, S(36));
        int pw = std::max(S(116), pause.preferredWidth() + S(12));
        pause.move(right - pw, fy, pw, S(36));
        int cw = std::max(S(96), cancel.preferredWidth() + S(12));
        cancel.move(tab == 0 ? right - pw - S(8) - cw : right - cw, fy, cw, S(36));
        int rw = refreshB.preferredWidth() + S(16);
        refreshB.move(right - pw - S(8) - cw - S(8) - rw, fy, rw, S(36));
        trayB.move(right - S(34), T + S(14), S(34), S(34));   // (Windows' own title bar: at the top right, under its –)
        // Speed limiter: the switch, then the speeds as buttons
        limitOn.move(pad, T + S(104), S(360), S(24));
        int bw = (r.right - 2 * pad - 3 * S(8)) / 4;
        for (size_t i = 0; i < limitBtns.size(); i++) limitBtns[i]->move(pad + static_cast<int>(i % 4) * (bw + S(8)), T + S(140) + static_cast<int>(i / 4) * S(52), bw, S(44));
        // When done: one card each
        CheckBox* boxes[] = {&showDone, &openDone, &shutDone};
        for (int i = 0; i < 3; i++) boxes[i]->move(pad + S(14), T + S(100) + i * S(62) + S(8), r.right - 2 * pad - S(28), S(22));
        if (capTip) for (int i = 0; i < 4; i++) {   // the title bar buttons' tips follow the window's width
            TOOLINFOW ti{}; ti.cbSize = sizeof ti; ti.hwnd = h; ti.uId = static_cast<UINT_PTR>(i + 1); ti.rect = capBtn(i);
            SendMessageW(capTip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }

    // ---- painting
    static std::string kbNumber(double bps) {   // "1,536" / "978.3" (KB/s): one decimal below 1,000
        double kb = std::max(0.0, bps) / 1024.0;
        if (kb >= 1000) return groupThousands(static_cast<int64_t>(kb + 0.5));
        char b[32];
        snprintf(b, sizeof b, "%.1f", kb);
        std::string s = b;
        size_t dot = s.find('.');
        return groupThousands(_atoi64(s.substr(0, dot).c_str())) + s.substr(dot);
    }
    static std::string unitSpeed(double bps, std::string& unit) {
        char b[32];
        double mb = bps / 1048576.0;
        if (mb >= 100) { snprintf(b, sizeof b, "%.0f", mb); unit = "MB/s"; }
        else if (mb >= 1) { snprintf(b, sizeof b, "%.1f", mb); unit = "MB/s"; }
        else { snprintf(b, sizeof b, "%.0f", bps / 1024.0); unit = "KB/s"; }
        return b;
    }
    void card(HDC dc, const RECT& r, float rad, COLORREF fillC, COLORREF line) { fillRound(dc, r, rad, fillC); strokeRound(dc, r, rad, line); }

    void paintBackground(HDC dc, RECT r) override {
        applyLook();
        makeFonts();
        int T = capH();
        if (T <= 0) { paintBody(dc, r); return; }
        paintCaption(dc, RECT{0, 0, r.right, T});
        int bw = std::max<int>(1, r.right), bh = std::max<int>(1, r.bottom - T);
        if (!bodyDC || bodyW != bw || bodyHt != bh) {
            if (bodyDC) { SelectObject(bodyDC, bodyOld); DeleteObject(bodyBmp); DeleteDC(bodyDC); }
            bodyDC = CreateCompatibleDC(dc);
            bodyBmp = CreateCompatibleBitmap(dc, bw, bh);
            bodyOld = SelectObject(bodyDC, bodyBmp);
            bodyW = bw; bodyHt = bh;
        }
        paintBody(bodyDC, RECT{0, 0, bw, bh});
        BitBlt(dc, 0, T, bw, bh, bodyDC, 0, 0, SRCCOPY);
    }

    void paintCaption(HDC dc, RECT c) {
        fill(dc, c, V.bg);
        int ls = S(18);
        RECT lr{S(12), (c.bottom - ls) / 2, S(12) + ls, (c.bottom - ls) / 2 + ls};
        logo(dc, lr);
        wchar_t title[512] = {};
        GetWindowTextW(h, title, 512);
        RECT tr{lr.right + S(10), 0, capBtn(0).left - S(8), c.bottom};
        textName(dc, U(title), tr, active ? V.text : V.muted, fontUI(), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        COLORREF g = active ? V.text : V.muted;
        for (int i = 0; i < 4; i++) {
            RECT b = capBtn(i);
            bool hov = capHover == i, down = hov && capPress == i;
            if (i == 3 && hov) fill(dc, b, down ? RGB(241, 112, 122) : RGB(232, 17, 35));
            else if (hov) fill(dc, b, blend(V.text, V.bg, down ? 0.8f : 0.9f));
            COLORREF gc = i == 3 && hov ? RGB(255, 255, 255) : g;
            float cx = (b.left + b.right) / 2.0f, cy = (b.top + b.bottom) / 2.0f, k = Sf(5);
            if (i == 0) {   // into the tray: a green arrow down onto a line (like IDM's)
                RECT ir{static_cast<LONG>(cx - Sf(8)), static_cast<LONG>(cy - Sf(8)), static_cast<LONG>(cx + Sf(8)), static_cast<LONG>(cy + Sf(8))};
                icon(dc, "M12 3v11 M7 9.5l5 5 5-5 M4 20h16", ir, V.good, 2.2f);
            } else if (i == 1) lineAA(dc, cx - k, cy, cx + k, cy, gc, Sf(1));
            else if (i == 2) {
                if (IsZoomed(h)) {   // restore: two windows
                    RECT a{static_cast<LONG>(cx - k + Sf(2)), static_cast<LONG>(cy - k), static_cast<LONG>(cx + k + 1), static_cast<LONG>(cy + k - Sf(2) + 1)};
                    strokeRound(dc, a, Sf(1), gc, Sf(1));
                    RECT f{static_cast<LONG>(cx - k), static_cast<LONG>(cy - k + Sf(2)), static_cast<LONG>(cx + k - Sf(2) + 1), static_cast<LONG>(cy + k + 1)};
                    fill(dc, f, i == 3 && hov ? RGB(232, 17, 35) : hov ? blend(V.text, V.bg, down ? 0.8f : 0.9f) : V.bg);
                    strokeRound(dc, f, Sf(1), gc, Sf(1));
                } else {
                    RECT q{static_cast<LONG>(cx - k), static_cast<LONG>(cy - k), static_cast<LONG>(cx + k + 1), static_cast<LONG>(cy + k + 1)};
                    strokeRound(dc, q, Sf(1), gc, Sf(1));
                }
            } else {
                lineAA(dc, cx - k, cy - k, cx + k, cy + k, gc, Sf(1));
                lineAA(dc, cx - k, cy + k, cx + k, cy - k, gc, Sf(1));
            }
        }
    }
    void capRedraw() { RECT c{0, 0, client().right, capH()}; if (c.bottom > 0) InvalidateRect(h, &c, FALSE); }
    void capClick(int i) {
        if (i == 0) { dust::window(h); toTray(); }
        else if (i == 1) PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);   // (the – button's own rules: Options → General)
        else if (i == 2) PostMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0);
        else if (i == 3) PostMessageW(h, WM_SYSCOMMAND, SC_CLOSE, 0);
    }

    void paintBody(HDC dc, RECT r) {
        fill(dc, r, V.bg);
        int pad = S(18), right = r.right - pad;
        // header: what it is and where it comes from
        RECT tile{pad, S(14), pad + S(34), S(48)};
        card(dc, tile, Sf(9), V.card, V.cardLine);
        RECT ti = tile; InflateRect(&ti, -S(8), -S(8));
        icon(dc, "M13 2L4 14h7l-1 8 9-12h-7l1-8z", ti, V.accent, 2.0f);
        bool running = t.state == State::Running, paused = t.state == State::Paused;
        std::string chip = t.state == State::Error ? "Error" : paused ? "Paused" : t.state == State::Queued ? "Waiting" : t.limitKBps > 0 ? "Limited" : "Downloading";
        COLORREF chipC = t.state == State::Error ? V.bad : paused || t.state == State::Queued ? V.warn : V.accent;
        SIZE cs = textSize(dc, chip, fontSmallBold());
        int chipRight = ownCaption ? right : right - S(42);   // (Windows' own title bar: room for the tray button)
        RECT cr{chipRight - cs.cx - S(30), S(18), chipRight, S(44)};
        fillRound(dc, cr, (cr.bottom - cr.top) / 2.0f, blend(chipC, V.bg, lookFor ? 0.82f : 0.88f));
        fillCircle(dc, cr.left + S(14), (cr.top + cr.bottom) / 2.0f, Sf(3.5f), chipC);
        RECT ct{cr.left + S(22), cr.top, cr.right - S(8), cr.bottom};
        text(dc, chip, ct, chipC, fontSmallBold());
        RECT nr{pad + S(46), S(13), cr.left - S(10), S(33)};
        textName(dc, t.fileName, nr, V.text, fName);
        RECT ur{pad + S(46), S(33), cr.left - S(10), S(50)};
        textName(dc, t.dir.empty() ? t.url : t.dir, ur, V.muted, fontSmall(), DT_LEFT | DT_PATH_ELLIPSIS);   // where it goes (the address is in the rows below)
        // tabs
        tabRects.clear();
        int x = pad;
        for (int k : tabKinds) {
            const char* name = tabName(k);
            bool on = k == tab;
            SIZE s = textSize(dc, name, fLabel);
            RECT tr{x, S(58), x + s.cx + S(28), S(88)};
            float rad = (tr.bottom - tr.top) / 2.0f;
            if (on) fillRound(dc, tr, rad, V.tabOn);
            strokeRound(dc, tr, rad, on ? V.tabOnLine : V.tabLine);
            text(dc, name, tr, on ? V.text : V.muted, fLabel, DT_CENTER);
            if (k == 3 && report.level >= inspect::Level::Caution) {   // a warning dot on the Security tab
                int d = S(7);
                RECT dot{tr.right - S(12), tr.top + S(5), tr.right - S(12) + d, tr.top + S(5) + d};
                fillRound(dc, dot, d / 2.0f, report.level == inspect::Level::Danger ? V.bad : V.warn);
            }
            tabRects.push_back(tr);
            x = tr.right + S(6);
        }
        // footer line
        if (tab != 3) { RECT fl{0, bodyH() - S(58), r.right, bodyH() - S(57)}; fill(dc, fl, V.footLine); }
        if (tab == 3) { paintSecurity(dc, r); return; }
        if (tab == 1) { paintLimiter(dc, r); return; }
        if (tab == 2) { paintWhenDone(dc, r); return; }
        paintStatus(dc, r, running);
    }

    // Download status, laid out like IDM's: the facts in rows, a big progress bar, then (Show details) each part of the file
    void paintStatus(HDC dc, RECT r, bool running) {
        int pad = S(18), right = r.right - pad;
        // the rows
        std::string st = t.state == State::Running ? (t.status.empty() ? "Receiving data…" : t.status) : t.state == State::Paused ? (t.stopped ? "Stopped" : "Paused") : t.state == State::Queued ? (t.waitReason.empty() ? "Waiting in the queue" : t.waitReason) : t.state == State::Error ? "Error" : "";   // (what went wrong: in full under the bar)
        COLORREF stC = t.state == State::Error ? V.bad : t.state == State::Running ? V.accent : V.warn;
        bool unknown = t.total < 0 && t.done == 0;
        std::string size = t.total > 0 ? formatBytes(t.total) : unknown ? "Checking…" : "Unknown";
        char pc[32] = "";
        if (t.total > 0) snprintf(pc, sizeof pc, " (%.2f%%)", t.progress() * 100);
        std::string got = formatBytes(t.done) + pc;
        std::string rate = running ? formatKBps(glidedSpeed(id, t.speed)) : "—";
        std::string left = running && t.eta >= 0 ? formatDuration(t.eta) : "—";
        std::string resume = unknown ? "Checking…" : (t.resumable ? "Yes" : "No");
        struct Row { const char* k; std::string v; COLORREF c; HFONT f; int kind; };   // kind: 0 text, 1 a name / address (never translated)
        Row rows[] = {{"URL", t.url, V.text, fontUI(), 1}, {"Status", st, stC, fontBold(), 0}, {"File size", size, V.text, fontUI(), 0},
                      {"Downloaded", got, V.text, fontBold(), 0}, {"Transfer rate", rate, V.text, fontUI(), 0}, {"Time left", left, V.text, fontUI(), 0},
                      {"Resume capability", resume, unknown ? V.muted : t.resumable ? V.good : V.warn, fontBold(), 0}};
        RECT box{pad, S(98), right, S(289)};
        card(dc, box, Sf(12), V.card, V.cardLine);
        int lw = S(150);
        for (const Row& rw : rows) { SIZE ks = textSize(dc, rw.k, fontUI()); lw = std::max(lw, static_cast<int>(ks.cx) + S(18)); }
        lw = std::min(lw, (right - pad) / 2);
        int y = box.top + S(8);
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            const Row& rw = rows[i];
            if (i > 0) { RECT sep{box.left + S(14), y, box.right - S(14), y + 1}; fill(dc, sep, blend(V.cardLine, V.card, 0.35f)); }
            RECT k{box.left + S(16), y, box.left + S(16) + lw, y + S(25)};
            text(dc, rw.k, k, V.muted, fontUI(), DT_VCENTER | DT_SINGLELINE);
            RECT v{box.left + S(16) + lw, y, box.right - S(16), y + S(25)};
            if (rw.kind == 1) textName(dc, rw.v, v, rw.c, rw.f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            else text(dc, rw.v, v, rw.c, rw.f, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += S(25);
        }
        // the progress bar: green-blue while it downloads (stripes slide along), amber when paused, red on an error
        RECT bar{pad, S(300), right, S(322)};
        COLORREF a1 = V.accent, a2 = lookFor ? RGB(56, 189, 248) : RGB(14, 165, 233);
        if (t.state == State::Error) { a1 = RGB(220, 38, 38); a2 = RGB(248, 113, 113); }
        else if (paused() || t.state == State::Queued) { a1 = RGB(234, 140, 8); a2 = RGB(251, 191, 36); }
        progressBar(dc, bar, t.total > 0 ? t.progress() : 0, V.track, a1, a2, Sf(11), phase, running && t.total > 0);
        if (t.total > 0) {   // the percentage on the bar
            char pb[16];
            snprintf(pb, sizeof pb, "%.1f%%", t.progress() * 100);
            bool onFill = t.progress() > 0.53;
            text(dc, pb, bar, onFill ? RGB(255, 255, 255) : V.text, fontSmallBold(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (running && t.done > 0) {   // no size from the server: a block that runs back and forth
            int w = (bar.right - bar.left) / 5, span = bar.right - bar.left - w;
            int pos = static_cast<int>(std::fabs(std::fmod(phase / 3.0f, 2.0f * span) - span));
            RECT b{bar.left + span - pos, bar.top, bar.left + span - pos + w, bar.bottom};
            progressBar(dc, b, 1.0, V.track, a1, a2, Sf(11));
        }
        // under the bar: the connections and the best speed so far
        RECT cap{pad, S(328), right, S(344)};
        if (t.state == State::Error) { RECT er{pad, S(327), right, S(355)}; text(dc, t.error, er, V.bad, fontSmall(), DT_WORDBREAK | DT_END_ELLIPSIS); }
        int conns = std::max(0, t.activeConnections);
        if (running && conns > 0) text(dc, std::to_string(conns) + (conns == 1 ? " connection" : " connections") + " · " + formatKBps(glidedSpeed(id, t.speed) / conns) + " each", cap, V.muted, fontSmall());
        if (peak > 0) text(dc, "Peak " + formatKBps(peak), cap, V.muted, fontSmall(), DT_RIGHT);
        if (!details) return;
        // each part of the file, filled as it arrives
        RECT pcap{pad, S(358), right, S(374)};
        text(dc, "Parts of the file — filled as they arrive", pcap, V.muted, fontSmall());
        const int cells = 32;
        double cover[cells] = {0};
        if (t.total > 0) {
            auto addRange = [&](int64_t a, int64_t b) {   // [a, b) has arrived
                for (int i = 0; i < cells; i++) {
                    int64_t c0 = t.total * i / cells, c1 = t.total * (i + 1) / cells;
                    int64_t o = std::min(b, c1) - std::max(a, c0);
                    if (o > 0 && c1 > c0) cover[i] += static_cast<double>(o) / static_cast<double>(c1 - c0);
                }
            };
            if (t.segments.empty()) { if (t.done > 0) addRange(0, std::min(t.done, t.total)); }
            else for (auto& sg : t.segments) {
                int64_t gotB = (sg.end >= 0 ? std::min(sg.pos, sg.end + 1) : sg.pos) - sg.start;
                if (gotB > 0) addRange(sg.start, sg.start + gotB);
            }
        }
        double cw = (right - pad - 15 * S(3)) / 16.0;   // 16 a row, edge to edge
        for (int i = 0; i < cells; i++) {
            int col = i % 16, row = i / 16;
            LONG cx0 = pad + static_cast<LONG>(col * (cw + S(3)) + 0.5), cx1 = pad + static_cast<LONG>(col * (cw + S(3)) + cw + 0.5);
            RECT c{cx0, S(380) + row * S(19), cx1, S(380) + row * S(19) + S(15)};
            fillRound(dc, c, Sf(3), V.cellOff);
            double f = std::min(1.0, cover[i]);
            if (f > 0) { RECT d = c; d.right = c.left + std::max<LONG>(S(2), static_cast<LONG>((c.right - c.left) * f)); fillRound(dc, d, Sf(3), paused() ? V.warn : V.accent); }
        }
    }
    bool paused() const { return t.state == State::Paused || t.state == State::Error; }

    void paintLimiter(HDC dc, RECT r) {
        int pad = S(18);
        std::string unit = "KB/s", num = kbNumber(t.state == State::Running ? t.speed : 0);
        RECT nr{pad, S(250), r.right - pad, S(300)};
        text(dc, num, nr, V.accent, fNum);
        SIZE ns = textSize(dc, num, fNum);
        RECT ur{pad + ns.cx + S(10), S(268), r.right - pad, S(292)};
        text(dc, unit + " right now", ur, V.muted, fLabel);
        RECT note{pad, S(306), r.right - pad, S(346)};
        text(dc, "Only this download slows down; your other downloads keep their speed.", note, V.muted, fontSmall(), DT_WORDBREAK);
    }

    void paintWhenDone(HDC dc, RECT r) {
        int pad = S(18);
        static const char* notes[] = {"A small window with Open and Open folder.", "Opens it with its usual program.", "You get 60 seconds to cancel first."};
        for (int i = 0; i < 3; i++) {
            RECT c{pad, S(100) + i * S(62), r.right - pad, S(100) + i * S(62) + S(54)};
            card(dc, c, Sf(12), V.card, V.cardLine);
            RECT d{pad + S(39), c.top + S(30), c.right - S(12), c.top + S(48)};
            text(dc, notes[i], d, V.muted, fontSmall());
        }
    }

    // the Security tab: what DDM knows about this file while it downloads, one card each
    void paintSecurity(HDC dc, RECT r) {
        int pad = S(18), y = S(98), right = r.right - pad;
        const Settings& st = settings();
        auto measure = [&](const std::string& s, HFONT f, int w) {
            RECT c{0, 0, w, S(400)};
            HFONT old = static_cast<HFONT>(SelectObject(dc, f));
            std::wstring ws = W(i18n::tr(s));
            DrawTextW(dc, ws.c_str(), -1, &c, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(dc, old);
            return static_cast<int>(c.bottom - c.top);
        };
        // one card: a small label on the left, its lines on the right
        auto item = [&](const std::string& label, const std::vector<std::pair<std::string, COLORREF>>& lines, HFONT f, COLORREF bg, COLORREF line, HFONT first = nullptr) {
            int lx = pad + S(158), w = right - S(14) - lx, hgt = S(14);
            for (size_t i = 0; i < lines.size(); i++) hgt += measure(lines[i].first, i == 0 && first ? first : f, w) + S(2);
            hgt = std::max(hgt, S(34));
            RECT c{pad, y, right, y + hgt};
            card(dc, c, Sf(10), bg, line);
            RECT k{pad + S(14), y + S(9), lx - S(8), y + S(25)};
            text(dc, label, k, V.muted, fCap);
            int ly = y + S(7);
            for (size_t i = 0; i < lines.size(); i++) {
                const auto& l = lines[i];
                HFONT lf = i == 0 && first ? first : f;
                int lh = measure(l.first, lf, w);
                RECT v{lx, ly, right - S(14), ly + lh};
                text(dc, l.first, v, l.second, lf, DT_WORDBREAK);
                ly += lh + S(2);
            }
            y += hgt + S(6);
        };
        // the link and the file on the server
        if (!st.linkCheck) item("LINK CHECK", {{"Off (Options → Security)", V.muted}}, fontUI(), V.card, V.cardLine);
        else {
            COLORREF c = report.level == inspect::Level::Danger ? V.bad : report.level == inspect::Level::Caution ? V.warn : report.level == inspect::Level::Safe ? V.good : V.muted;
            std::string head = report.level == inspect::Level::Unknown ? "Checking where it comes from…" : report.level == inspect::Level::Safe ? (report.fileDone ? "No warning signs" : "No warning signs so far (still looking at the file)") : inspect::levelName(report.level);
            std::vector<std::pair<std::string, COLORREF>> lines = {{head, c}};
            int shown = 0;
            for (int pass = 0; pass < 3; pass++)   // dangers first, then warnings, then the good signs
                for (auto& f : report.findings) {
                    inspect::Level want = pass == 0 ? inspect::Level::Danger : pass == 1 ? inspect::Level::Caution : inspect::Level::Safe;
                    if (f.level != want || shown >= 3) continue;
                    lines.push_back({"• " + f.text, f.level == inspect::Level::Danger ? V.bad : f.level == inspect::Level::Caution ? V.warn : V.text});
                    shown++;
                }
            bool tint = report.level != inspect::Level::Unknown;
            item("LINK CHECK", lines, fontUI(), tint ? blend(c, V.bg, lookFor ? 0.86f : 0.9f) : V.card, tint ? blend(c, V.bg, 0.7f) : V.cardLine, fontBold());
        }
        // the checksum, worked out while it downloads
        std::string hv;
        COLORREF hc = V.text;
        if (!hash.sha256.empty()) hv = hash.sha256;
        else if (!hash.note.empty()) { hv = hash.note; hc = V.muted; }
        else if (hash.total > 0) { char b[160]; snprintf(b, sizeof b, "Checked %s of %s so far (it follows the parts in order)", formatBytes(hash.hashed).c_str(), formatBytes(hash.total).c_str()); hv = b; hc = V.muted; }
        else { hv = hash.hashed > 0 ? "Checked " + formatBytes(hash.hashed) + " so far" : "Starts when data arrives"; hc = V.muted; }
        item("SHA-256", {{hv, hc}}, hash.sha256.empty() ? fontUI() : fMono, V.card, V.cardLine);
        if (!t.checksum.empty()) {
            std::string c = t.checksum, kind = c.substr(0, c.find(':')), val = c.substr(c.find(':') + 1);
            std::string v = "The server published " + algoName(kind) + " " + val;
            COLORREF col = V.text;
            if (!hash.sha256.empty() && lower(kind) == "sha256") { bool ok = lower(val) == lower(hash.sha256); v = ok ? "Matches the server's SHA-256: the file is exactly what was published" : "Doesn't match the server's SHA-256: the file is damaged or was changed"; col = ok ? V.good : V.bad; }
            else v += " (compared when it finishes)";
            item("PUBLISHED CHECKSUM", {{v, col}}, col == V.text ? fontSmall() : fontUI(), V.card, V.cardLine);
        } else item("PUBLISHED CHECKSUM", {{"The server didn't publish one", V.muted}}, fontUI(), V.card, V.cardLine);
        // what happens when it arrives
        std::string av = st.scanWithAntivirus ? (st.antivirusOnlyRisky ? "Scanned when it finishes if it can contain harmful code" : "Scanned by your antivirus as soon as it finishes") : "Off (Options → Security)";
        if (t.scanState == "threat" || t.scanState == "removed") av = "THREAT: " + t.scanNote;
        else if (t.scanState == "clean") av = "Clean";
        item("ANTIVIRUS", {{av, t.scanState == "threat" || t.scanState == "removed" ? V.bad : st.scanWithAntivirus ? V.text : V.muted}}, fontUI(), V.card, V.cardLine);
        item("INTERNET MARK", {{st.markOfTheWeb ? "Added when it finishes: Windows SmartScreen and Office check the file before it opens" : "Off (Options → Security)", st.markOfTheWeb ? V.text : V.muted}}, fontUI(), V.card, V.cardLine);
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
            case WM_TIMER:
                if (w == 1) update();
                else if (w == 2) {   // the bar's stripes slide while it downloads
                    if (tab != 0 || IsIconic(h) || !IsWindowVisible(h)) return 0;
                    bool moving = t.state == State::Running && (t.total > 0 || t.done > 0);
                    if (!moving || (++ticks & 1) != 0) return 0;   // 15 times a second is smooth enough and spares the CPU
                    phase += Sf(2.4f);
                    // the facts and the bar: the speed glides on towards the engine's next figure instead of
                    // stepping to it twice a second, so it reads as movement rather than as stalling and jumping
                    RECT b{0, capH() + S(92), client().right, capH() + S(330)};
                    InvalidateRect(h, &b, FALSE);
                }
                return 0;
            case WM_DDM_BG: {   // the controls take the colour of what they sit on (a card on the When done tab)
                HWND c = reinterpret_cast<HWND>(w);
                if (c == showDone.hwnd() || c == openDone.hwnd() || c == shutDone.hwnd()) return bgReply(V.card);
                return bgReply(V.bg);
            }
            case WM_LBUTTONDOWN: {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (pt.y < capH()) {
                    for (int i = 0; i < 4; i++) { RECT b = capBtn(i); if (PtInRect(&b, pt)) { capPress = capHover = i; SetCapture(h); capRedraw(); } }
                    return 0;
                }
                pt.y -= capH();
                for (size_t i = 0; i < tabRects.size() && i < tabKinds.size(); i++) if (PtInRect(&tabRects[i], pt)) showTab(tabKinds[i]);
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (capH() <= 0) break;
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                int hov = -1;
                for (int i = 0; i < 4; i++) { RECT b = capBtn(i); if (PtInRect(&b, pt)) hov = i; }
                if (hov != capHover) { capHover = hov; capRedraw(); }
                if (hov >= 0) { TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0}; TrackMouseEvent(&tme); }
                break;
            }
            case WM_MOUSELEAVE:
                if (capHover >= 0 && capPress < 0) { capHover = -1; capRedraw(); }
                break;
            case WM_LBUTTONUP:
                if (capPress >= 0) {
                    int i = capPress;
                    capPress = -1;
                    ReleaseCapture();
                    bool on = capHover == i;
                    capHover = -1;
                    capRedraw();
                    if (on) capClick(i);
                    return 0;
                }
                break;
            case WM_CAPTURECHANGED:
                if (capPress >= 0) { capPress = -1; capHover = -1; capRedraw(); }
                break;
            case WM_NCCALCSIZE:   // its own title bar: Windows keeps the edges, the top belongs to DDM
                if (ownCaption && w) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(l);
                    LONG top = p->rgrc[0].top;
                    LRESULT r = def(m, w, l);
                    p->rgrc[0].top = top + (IsZoomed(h) ? GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER) : 0);
                    return r;
                }
                break;
            case WM_NCHITTEST:
                if (ownCaption) {
                    LRESULT r = def(m, w, l);
                    if (r != HTCLIENT) return r;
                    POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                    ScreenToClient(h, &pt);
                    if (pt.y >= capH()) return HTCLIENT;
                    if (!IsZoomed(h) && pt.y < S(4)) { RECT cr = client(); return pt.x < S(10) ? HTTOPLEFT : pt.x >= cr.right - S(10) ? HTTOPRIGHT : HTTOP; }
                    for (int i = 0; i < 4; i++) { RECT b = capBtn(i); if (PtInRect(&b, pt)) return HTCLIENT; }
                    return HTCAPTION;   // drag it, double-click to maximize, right-click for the window menu
                }
                break;
            case WM_NCACTIVATE:
                active = w != FALSE;
                capRedraw();
                if (ownCaption && !composited()) return def(m, w, -1);   // (Windows' Classic look would paint its title bar over DDM's)
                break;
            case WM_SETTEXT:
                if (ownCaption) {
                    LRESULT r;
                    if (!composited()) {   // (the same: hidden for a moment so Windows doesn't paint its title over DDM's)
                        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
                        SetWindowLongPtrW(h, GWL_STYLE, st & ~static_cast<LONG_PTR>(WS_VISIBLE));
                        r = def(m, w, l);
                        SetWindowLongPtrW(h, GWL_STYLE, st);
                    } else r = def(m, w, l);
                    capRedraw();
                    return r;
                }
                break;
            case 0x00AE: case 0x00AF:   // WM_NCUAHDRAWCAPTION / WM_NCUAHDRAWFRAME: Windows' themed title bar, not wanted here
                if (ownCaption && !composited()) return 0;
                break;
            case WM_DWMCOMPOSITIONCHANGED: {   // Windows 7: Aero switched on or off — the title bar follows
                bool want = wantOwnCaption();
                if (want != ownCaption) {
                    ownCaption = want;
                    trayB.show(!ownCaption);
                    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
                    resize();
                }
                break;
            }
            case WM_RBUTTONUP: {   // right-click a tab: hide it (Options → General shows it again)
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                POINT bp{pt.x, pt.y - capH()};   // (the tabs sit under the title bar)
                for (size_t i = 0; i < tabRects.size() && i < tabKinds.size(); i++) {
                    if (!PtInRect(&tabRects[i], bp) || tabKinds[i] == 0) continue;
                    int k = tabKinds[i];
                    std::vector<MenuItem> v = {{std::string("Hide the \"") + tabName(k) + "\" tab", "", 1}};
                    MenuItem note{"Show it again in Options → General", "", 0}; note.disabled = true; v.push_back(note);
                    POINT sp = pt; ClientToScreen(h, &sp);
                    if (popupMenu(h, v, sp) == 1) {
                        Settings& st = settings();
                        (k == 1 ? st.tabLimiter : k == 2 ? st.tabWhenDone : st.tabSecurity) = false;
                        saveSettings();
                        refreshProgressTabs();
                    }
                    return 0;
                }
                return 0;
            }
            case WM_SYSCOMMAND:
                if ((w & 0xFFF0) == kSysTray) { dust::window(h); toTray(); return 0; }   // the window menu's "Minimize to system tray"
                if ((w & 0xFFF0) == SC_MINIMIZE) dust::window(h);   // – : turns to dust, then goes to the tray or the taskbar
                if ((w & 0xFFF0) == SC_MINIMIZE && settings().progressToTray) {   // – : into the tray (its own icon); the download keeps going
                    toTray();
                    return 0;
                }
                break;
            case WM_SIZE:   // minimized some other way (taskbar, keyboard): the same as the – button
                if (w == SIZE_MINIMIZED && settings().progressToTray && IsWindowVisible(h)) {
                    toTray();   // (coming back from the tray restores it to its size)
                    return 0;
                }
                break;
            case kTrayMsg:
                switch (LOWORD(l)) {
                    case WM_LBUTTONDBLCLK: fromTray(); return 0;
                    case WM_RBUTTONUP: case WM_CONTEXTMENU: trayMenu(); return 0;
                }
                return 0;
            case WM_SHOWWINDOW: if (w) { inTray = false; trayRemove(); } break;
            case WM_CLOSE: inTray = false; trayRemove(); dust::window(h); ShowWindow(h, SW_HIDE); return 0;           // the download keeps going
            case WM_DESTROY: KillTimer(h, 1); KillTimer(h, 2); trayRemove(); break;
        }
        static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (m == taskbarCreated && trayOn) { trayOn = false; if (nid.hIcon) { DestroyIcon(nid.hIcon); nid.hIcon = nullptr; } iconPct = -2; trayAdd(); return 0; }   // Explorer restarted: the icon comes back
        return Dialog::proc(m, w, l);
    }
};

std::map<std::string, ProgressWnd*> g_progress;

void trayHint() {   // once a session: say where the window went
    static bool said = false;
    if (said) return;
    said = true;
    auto* c = settings().trayOneIcon
        ? new std::pair<std::string, std::string>("The download keeps going", "Its window is in DDM's tray icon: click the icon to see all your downloads live, double-click it to bring the windows back.")
        : new std::pair<std::string, std::string>("The download keeps going", "Its window is in the tray with its own icon: double-click it to bring the window back, right-click it to pause, stop or cancel.");
    if (!PostMessageW(App::get().mainHwnd(), WM_APP + 43, 0, reinterpret_cast<LPARAM>(c))) delete c;
}
} // namespace

void refreshProgressTabs() { for (auto& kv : g_progress) if (IsWindow(kv.second->hwnd())) { kv.second->pickTabs(); kv.second->showTab(kv.second->tab); } }

std::vector<std::pair<std::string, std::string>> progressInTray() {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& kv : g_progress) {
        if (!IsWindow(kv.second->hwnd()) || !kv.second->inTray) continue;
        const Task& t = kv.second->t;
        out.push_back({kv.first, t.fileName + (t.total > 0 ? " — " + std::to_string(static_cast<int>(t.progress() * 100)) + "%" : "")});
    }
    return out;
}

bool restoreProgressFromTray() {
    bool any = false;
    for (auto& kv : g_progress) if (IsWindow(kv.second->hwnd()) && kv.second->inTray) { kv.second->fromTray(); any = true; }
    return any;
}

void showProgressWindow(const std::string& id) {
    auto it = g_progress.find(id);
    if (it != g_progress.end() && IsWindow(it->second->hwnd())) {
        ShowWindow(it->second->hwnd(), IsIconic(it->second->hwnd()) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(it->second->hwnd());
        return;
    }
    Task t;
    if (!Engine::get().find(id, t)) return;
    auto* p = new ProgressWnd();
    p->id = id;
    if (!p->createDialog(nullptr, t.fileName, S(600), S(482), true, true)) { delete p; return; }   // (build() sets its exact size)
    g_progress[id] = p;
    p->build();
    ShowWindow(p->hwnd(), SW_SHOW);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) {
            auto pw = reinterpret_cast<ProgressWnd*>(ref);
            RemoveWindowSubclass(hw, sub, 1);
            LRESULT res = DefSubclassProc(hw, m, w, l);
            for (auto i = g_progress.begin(); i != g_progress.end(); ++i) if (i->second == pw) { g_progress.erase(i); break; }
            delete pw;
            return res;
        }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(p->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(p));
}

void closeProgressWindow(const std::string& id) {
    auto it = g_progress.find(id);
    if (it != g_progress.end()) DestroyWindow(it->second->hwnd());
}

bool hasProgressWindow(const std::string& id) { auto it = g_progress.find(id); return it != g_progress.end() && IsWindow(it->second->hwnd()); }

void refreshProgressWindows() { for (auto& kv : g_progress) kv.second->update(); }

// ================================================================ Download complete
std::string algoName(const std::string& a);
namespace {
// Drags a file out of DDM the way Explorer does it (into a folder, onto the desktop, into another program).
// Returns true when the file was moved away.
bool dragFileOut(HWND from, const std::string& path) {
    if (!fileExists(path)) return false;
    static bool ole = false;
    if (!ole) { OleInitialize(nullptr); ole = true; }
    std::wstring wp = W(path);
    for (auto& c : wp) if (c == L'/') c = L'\\';
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(wp.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return false;
    IShellFolder* parent = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IDataObject* data = nullptr;
    bool moved = false;
    if (SUCCEEDED(SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parent), &child)) && parent) {
        if (SUCCEEDED(parent->GetUIObjectOf(from, 1, &child, IID_IDataObject, nullptr, reinterpret_cast<void**>(&data))) && data) {
            DWORD effect = 0;
            HRESULT hr = SHDoDragDrop(from, data, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
            moved = hr == DRAGDROP_S_DROP && !fileExists(path);
            data->Release();
        }
        parent->Release();
    }
    CoTaskMemFree(pidl);
    return moved;
}

class CompleteWnd : public Dialog {
public:
    Task t;
    Button open, folder, closeBtn, drag;
    static LRESULT CALLBACK dragSub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<CompleteWnd*>(ref);
        if (m == WM_LBUTTONDOWN) { self->startDrag(hw); return 0; }
        if (m == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        if (m == WM_NCDESTROY) RemoveWindowSubclass(hw, dragSub, 2);
        return DefSubclassProc(hw, m, w, l);
    }
    void startDrag(HWND fromWnd) {   // press the hand (or the file's name) and pull: the file goes where you drop it
        bool bad = t.scanState == "threat" || t.scanState == "removed";
        if (bad) return;
        HWND me = h;
        if (dragFileOut(fromWnd, t.path()) && IsWindow(me)) close(0);   // moved away: nothing left to open here
    }
    void build() {
        drag.create(h, "", BtnStyle::Normal, "M8 11.5V5.5a1.5 1.5 0 0 1 3 0V10.5 M11 10.5V4.5a1.5 1.5 0 0 1 3 0V10.5 M14 10.5V5.5a1.5 1.5 0 0 1 3 0V11.5 M17 11.5V8.5a1.5 1.5 0 0 1 3 0V15a6 6 0 0 1-6 6h-2.2a6 6 0 0 1-4.9-2.5L4.2 14.6a1.5 1.5 0 0 1 2.4-1.8L8 14.5V11.5");
        drag.iconSize = 26;
        tooltip(h, drag.hwnd(), "Drag the file from here into a folder, onto the desktop or into another program");
        SetWindowSubclass(drag.hwnd(), dragSub, 2, reinterpret_cast<DWORD_PTR>(this));
        open.create(h, "Open", BtnStyle::Primary, "open");
        folder.create(h, "Open folder", BtnStyle::Normal, "folder");
        closeBtn.create(h, "Close");
        std::string id = t.id;
        open.onClick = [this, id] { App::get().openFile(id); close(0); };
        folder.onClick = [this, id] { App::get().openFolder(id); close(0); };
        closeBtn.onClick = [this] { close(0); };
        onEnter = open.onClick;
        layout();
        SetFocus(open.hwnd());
        guard();
        SetTimer(h, 1, 700, nullptr);
    }
    // a file the antivirus flagged or removed can't be opened from here
    void guard() {
        bool bad = t.scanState == "threat" || t.scanState == "removed" || t.verify == "bad";
        EnableWindow(open.hwnd(), !bad);
        EnableWindow(drag.hwnd(), !(t.scanState == "threat" || t.scanState == "removed"));
        if (bad) { onEnter = closeBtn.onClick; SetFocus(closeBtn.hwnd()); }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_TIMER) { Task n; if (Engine::get().find(t.id, n) && (n.scanState != t.scanState || n.scanNote != t.scanNote || n.verify != t.verify)) { t = n; guard(); redraw(); } return 0; }
        if (m == WM_LBUTTONDOWN) {   // the file's name and its path can be dragged out too
            POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
            RECT r = client();
            RECT zone{S(18), S(84), r.right - S(18), S(140)};
            if (PtInRect(&zone, pt)) { POINT sp = pt; ClientToScreen(h, &sp); if (DragDetect(h, sp)) { startDrag(h); return 0; } }
        }
        return Dialog::proc(m, w, l);
    }
    void layout() override {
        RECT r = client();
        int y = r.bottom - S(52), x = r.right - S(18);
        for (Button* b : {&closeBtn, &folder, &open}) { int w = std::max(S(100), b->preferredWidth()); x -= w; b->move(x, y, w, S(34)); x -= S(8); }
        drag.move(r.right - S(18) - S(46), S(21), S(46), S(46));
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT tile{S(18), S(18), S(18) + S(52), S(18) + S(52)};
        fillRound(dc, tile, Sf(8), blend(P.win, P.good, isDark() ? 0.18f : 0.12f));
        RECT ic = tile; InflateRect(&ic, -S(13), -S(13));
        bool damaged = t.verify == "bad";
        icon(dc, damaged ? "M12 3.5l9.5 16.5h-19z M12 10v4.5 M12 17.2v.3" : "check", ic, damaged ? P.bad : P.good, 2.6f);
        RECT t1{S(84), S(18), r.right - S(18) - S(54), S(44)};
        text(dc, damaged ? "Download damaged" : "Download complete", t1, damaged ? P.bad : P.good, fontBig());
        RECT t2{S(84), S(44), r.right - S(18) - S(54), S(66)};
        text(dc, formatBytes(t.total) + (t.finished > t.lastTry && t.lastTry ? " · took " + formatDuration((t.finished - t.lastTry) / 1000) : ""), t2, P.muted, fontUI());
        RECT n{S(18), S(84), r.right - S(18), S(108)};
        textName(dc, t.fileName, n, P.text, fontBold());
        RECT pth{S(18), S(110), r.right - S(18), S(140)};
        fillRound(dc, pth, Sf(6), P.tile);
        RECT pi{pth.left + S(10), pth.top + S(7), pth.left + S(26), pth.top + S(23)};
        icon(dc, "folder", pi, P.folder, 1.8f);
        RECT pt{pth.left + S(34), pth.top, pth.right - S(10), pth.bottom};
        textName(dc, t.path(), pt, P.text, fontSmall(), DT_PATH_ELLIPSIS);
        if (!t.scanState.empty()) {   // the antivirus result
            bool bad = t.scanState == "threat" || t.scanState == "removed";
            RECT si{S(18), S(150), S(18) + S(16), S(166)};
            icon(dc, "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z", si, bad ? P.bad : t.scanState == "clean" ? P.good : P.muted, 1.8f);
            RECT sl{S(42), S(146), r.right - S(18), S(170)};
            text(dc, t.scanNote, sl, bad ? P.bad : P.muted, fontSmall(), DT_END_ELLIPSIS);
        }
        if (t.verify == "ok" || t.verify == "bad") {   // the checksum check
            bool bad = t.verify == "bad";
            int y0 = t.scanState.empty() ? S(146) : S(172);
            RECT si{S(18), y0 + S(4), S(34), y0 + S(20)};
            icon(dc, bad ? "M12 3.5l9.5 16.5h-19z M12 10v4.5 M12 17.2v.3" : "M4 12.5l5 5L20 7", si, bad ? P.bad : P.good, 1.9f);
            RECT sl{S(42), y0, r.right - S(18), y0 + S(24)};
            std::string algo = t.checksum.substr(0, t.checksum.find(':'));
            algo = algoName(algo);
            text(dc, bad ? "Damaged: its " + algo + " checksum doesn't match. Redownload it." : "Checksum (" + algo + ") matches: the file arrived intact.", sl, bad ? P.bad : P.muted, fontSmall(), DT_END_ELLIPSIS);
        }
    }
};
} // namespace

void showCompleteWindow(const std::string& id) {
    static std::map<std::string, HWND> open;   // one "Download complete" per download (it can finish while its window opens)
    auto it = open.find(id);
    if (it != open.end() && IsWindow(it->second) && IsWindowVisible(it->second)) { SetWindowPos(it->second, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); return; }
    for (auto o = open.begin(); o != open.end();) { if (!IsWindow(o->second)) o = open.erase(o); else ++o; }
    auto* c = new CompleteWnd();
    if (!Engine::get().find(id, c->t)) { delete c; return; }
    if (!c->createDialog(nullptr, "Download complete", S(470), S(258), true)) { delete c; return; }
    c->build();
    ShowWindow(c->hwnd(), SW_SHOWNOACTIVATE);
    open[id] = c->hwnd();
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<CompleteWnd*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(c->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(c));
}

// ================================================================ small dialogs
} // namespace ui
} // namespace ddm

namespace ddm {
namespace ui {
namespace {
struct VerifyJob { std::string id, file, algo, want, got; };
std::string clipText(HWND h) {
    std::string out;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(h)) return out;
    HANDLE d = GetClipboardData(CF_UNICODETEXT);
    if (d) { auto* p = static_cast<const wchar_t*>(GlobalLock(d)); if (p) { out = U(p); GlobalUnlock(d); } }
    CloseClipboard();
    return trim(out.substr(0, 4096));
}
} // namespace
std::string algoName(const std::string& a) { return a == "sha256" ? "SHA-256" : a == "sha512" ? "SHA-512" : a == "sha384" ? "SHA-384" : a == "sha1" ? "SHA-1" : a == "md5" ? "MD5" : a == "crc32" ? "CRC-32" : a; }

// Right-click → Verify checksum…: paste what the website shows; DDM works out which kind it is and compares.
void verifyChecksumDialog(HWND owner, const std::string& id) {
    Task t;
    if (!Engine::get().find(id, t) || t.state != State::Complete || !fileExists(t.path())) { askBox(owner, "Verify checksum", "The file isn't on the disk any more.", {"OK"}, 0); return; }
    std::string v = clipText(owner);
    // a checksum copied from the web page (often "sha256: abcd…" or "abcd…  file.zip"): take the hex part
    std::string hexPart;
    for (auto& w : split(replaceAll(replaceAll(v, "\t", " "), "\n", " "), ' ')) { std::string x = trim(w); size_t c = x.find(':'); if (c != std::string::npos) x = x.substr(c + 1); if (!crypto::checksumKind(x).empty()) { hexPart = x; break; } }
    if (hexPart.empty() && !t.checksum.empty()) hexPart = t.checksum.substr(t.checksum.find(':') + 1);
    v = hexPart;
    if (!inputBox(owner, "Verify checksum", "The checksum the website shows for " + t.fileName + " (MD5, SHA-1, SHA-256, SHA-512 or CRC-32). Leave it empty to just see the file's SHA-256", v)) return;
    v = lower(trim(v));
    v.erase(std::remove_if(v.begin(), v.end(), [](char ch) { return isspace(static_cast<unsigned char>(ch)) != 0; }), v.end());
    std::string algo = v.empty() ? "sha256" : crypto::checksumKind(v);
    if (algo.empty()) { askBox(owner, "Verify checksum", "That doesn't look like a checksum. It should be 8, 32, 40, 64, 96 or 128 letters and digits (0-9, a-f).", {"OK"}, 0); return; }
    auto* job = new VerifyJob{id, t.path(), algo, v, ""};
    HWND hw = App::get().mainHwnd();
    if (!hw) { delete job; return; }
    Engine::get().setVerify(id, "checking");
    std::thread([job, hw] { job->got = crypto::fileChecksum(job->file, job->algo); if (!PostMessageW(hw, WM_APP + 45, 0, reinterpret_cast<LPARAM>(job))) delete job; }).detach();
}

void verifyDone(HWND owner, LPARAM l) {
    std::unique_ptr<VerifyJob> j(reinterpret_cast<VerifyJob*>(l));
    if (j->got.empty()) { Engine::get().setVerify(j->id, ""); askBox(owner, "Verify checksum", "DDM couldn't read " + baseName(j->file) + ".", {"OK"}, 0); return; }
    if (j->want.empty()) {
        Engine::get().setVerify(j->id, "");
        int r = askBox(owner, "Verify checksum", baseName(j->file) + "\n\nSHA-256: " + j->got, {"Copy", "Close"}, 1);
        if (r == 0 && OpenClipboard(owner)) {
            EmptyClipboard();
            std::wstring w = W(j->got);
            HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * 2);
            if (g) { memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * 2); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
            CloseClipboard();
        }
        return;
    }
    bool ok = j->got == j->want;
    Engine::get().setVerify(j->id, ok ? "ok" : "bad", j->algo + ":" + j->want);
    askBox(owner, ok ? "Checksum matches" : "Checksum doesn't match",
           ok ? baseName(j->file) + " is exactly the file the website describes (" + algoName(j->algo) + ")."
              : baseName(j->file) + " is NOT the file the website describes: it was damaged on the way or changed. Redownload it, and don't open it if it's a program.\n\nExpected: " + j->want + "\nThis file: " + j->got,
           {"OK"}, 0);
}
} // namespace ui
} // namespace ddm

namespace ddm {
namespace ui {
std::vector<VideoFormat> offeredFormats(VideoInfo& vinfo, const LinkRequest& req) {
    std::vector<VideoFormat> formats = vinfo.formats;
    bool blob = req.direct.empty() || startsWith(req.direct, "blob:");
    bool generic = lower(vinfo.extractor) == "generic" || lower(vinfo.extractor).find("html5") != std::string::npos;
    if (generic) {
        if (!trim(req.title).empty()) { vinfo.rawTitle = req.title; vinfo.title = niceVideoTitle(req.title, "", vinfo.uploader, "", vinfo.videoId); }   // the page's own name for what's playing
        if (!blob) formats.clear();                                     // a page with several videos: the one the browser plays
    }
    if (formats.empty() && blob) {   // nothing read, but the browser plays a stream (HLS .m3u8 / DASH .mpd): download that stream
        std::string stream;
        for (auto& u : req.streams) {
            std::string l = lower(u);
            if (!(startsWith(l, "http://") || startsWith(l, "https://"))) continue;
            if (l.find(".m3u8") != std::string::npos) { stream = u; break; }
            if (stream.empty() && l.find(".mpd") != std::string::npos) stream = u;
        }
        if (!stream.empty()) {
            if (!trim(req.title).empty() && trim(vinfo.title).empty()) { vinfo.rawTitle = req.title; vinfo.title = niceVideoTitle(req.title, "", "", "", ""); }
            VideoFormat v;
            v.group = "Video"; v.label = "Best quality"; v.tag = ""; v.spec = "stream · video + sound"; v.ext = "mp4";
            v.selector = "bestvideo*+bestaudio/best"; v.streamUrl = stream;
            formats.push_back(v);
            VideoFormat a;
            a.group = "Audio"; a.label = "Sound only"; a.tag = "MP3"; a.spec = "stream · sound"; a.ext = "mp3"; a.convert = "mp3";
            a.selector = "bestaudio/best"; a.streamUrl = stream;
            formats.push_back(a);
            vinfo.error.clear();
        }
    }
    if (formats.empty() && !blob) {   // the extractor couldn't read the page, but the browser plays a plain file: take that
        VideoFormat o;
        o.group = "Video"; o.label = "Original"; o.tag = ""; o.spec = "video + sound"; o.ext = extOf(fileNameFromUrl(req.direct)); if (o.ext.empty() || o.ext.size() > 4) o.ext = "mp4";
        o.direct = true; o.directUrl = req.direct; o.selector = "";
        formats.push_back(o);
    }
    return formats;
}

std::string formatName(const VideoFormat& f) {
    std::string n = f.label;
    if (!f.tag.empty() && f.tag != f.label && f.group == "Video" && f.height > 0) n += "  " + f.tag;
    std::string sz = f.size > 0 ? (f.exact ? "" : "~") + formatBytes(f.size) : "size unknown";
    std::string e = f.ext; for (auto& c : e) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    n += "  ·  " + (f.group == "Audio" ? (f.convert.empty() ? e + " sound" : "MP3 sound") : e) + "  ·  " + sz;
    return n;
}
} // namespace ui
} // namespace ddm
