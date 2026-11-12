// "Download all links / images with DDM" (and the links in selected text): ONE window lists everything the page holds,
// with a tick box per item and a filter by kind (files, videos, pictures, music, archives, documents, programs, web
// pages). The ticked items go into the list together — never a window per link.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/settings.hpp"
#include "../core/torrent.hpp"
#include "../core/mega.hpp"
#include "../core/links.hpp"
#include "../core/downloader.hpp"
#include <thread>
#include "../common/util.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <set>
#include <map>

namespace ddm {
namespace ui {

namespace {

struct Pick {
    std::string url, name, kind, host;   // kind: video, picture, music, archive, document, program, file, page, magnet
    bool checked = false;
};

std::string kindOf(const std::string& url, const std::string& name, const std::string& hint) {
    if (isMagnet(url)) return "magnet";
    std::string e = lower(extOf(name));
    static const std::set<std::string> vid = {"mp4", "mkv", "webm", "mov", "avi", "m4v", "flv", "wmv", "3gp", "ts", "m3u8", "mpg", "mpeg"};
    static const std::set<std::string> pic = {"jpg", "jpeg", "png", "gif", "webp", "bmp", "svg", "avif", "tif", "tiff", "ico", "heic"};
    static const std::set<std::string> mus = {"mp3", "m4a", "aac", "flac", "ogg", "opus", "wav", "wma"};
    static const std::set<std::string> arc = {"zip", "rar", "7z", "tar", "gz", "tgz", "bz2", "xz", "iso", "img", "cab", "zst", "torrent"};
    static const std::set<std::string> doc = {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "rtf", "odt", "ods", "odp", "epub", "csv", "md"};
    static const std::set<std::string> prog = {"exe", "msi", "msix", "appx", "apk", "dmg", "deb", "rpm", "bat", "jar"};
    if (vid.count(e) || hint == "video") return "video";
    if (pic.count(e) || hint == "img") return "picture";
    if (mus.count(e) || hint == "audio") return "music";
    if (arc.count(e)) return "archive";
    if (doc.count(e)) return "document";
    if (prog.count(e)) return "program";
    if (isMegaLink(url)) return "file";
    static const std::set<std::string> page = {"", "html", "htm", "php", "asp", "aspx", "jsp", "cgi", "shtml", "xhtml"};
    if (page.count(e) || e.size() > 5) return "page";
    return "file";
}

const char* kindLabel(const std::string& k) {
    return k == "video" ? "Video" : k == "picture" ? "Picture" : k == "music" ? "Music" : k == "archive" ? "Archive" : k == "document" ? "Document" :
           k == "program" ? "Program" : k == "magnet" ? "Magnet link" : k == "page" ? "Web page" : "File";
}
const char* kindIcon(const std::string& k) {
    return k == "video" ? "video" : k == "picture" ? "image" : k == "music" ? "music" : k == "archive" ? "M4 7h16v13H4z M4 7l2-3h12l2 3 M10 11h4" :
           k == "document" ? "M6 3h9l4 4v14H6z M9 12h7 M9 16h7" : k == "program" ? "M4 5h16v14H4z M4 9h16" : k == "magnet" ? "magnet" : k == "page" ? "globe" : "down";
}

// the list: a tick box per row; click a row to tick it, Shift+click ticks everything between
class PickList : public Wnd {
public:
    std::vector<Pick>* all = nullptr;
    std::vector<int> shown;   // indexes into *all
    int top = 0, anchor = -1;
    std::function<void()> onChange;
    bool create(HWND parent) { return Wnd::create(L"DDMPickList", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0, 0, 0, 10, 10, parent); }
    int rowH() const { return S(30); }
    int headH() const { return S(28); }
    void updateScroll() {
        RECT r = client();
        int vis = std::max<int>(1, (r.bottom - headH()) / rowH());
        top = std::max(0, std::min(top, std::max(0, static_cast<int>(shown.size()) - vis)));
        SCROLLINFO si{sizeof si, SIF_ALL, 0, std::max(0, static_cast<int>(shown.size()) - 1), static_cast<UINT>(vis), top, 0};
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
                int nameW = std::max<int>(S(220), (r.right - S(46)) * 45 / 100), kindW = S(110);
                RECT hr{0, 0, r.right, headH()};
                fill(dc, hr, P.win);
                RECT t1{S(44), 0, S(44) + nameW, headH()}, t2{t1.right, 0, t1.right + kindW, headH()}, t3{t2.right, 0, r.right - S(8), headH()};
                text(dc, "Name", t1, P.muted, fontSmall()); text(dc, "Kind", t2, P.muted, fontSmall()); text(dc, "Address", t3, P.muted, fontSmall());
                RECT ln{0, headH() - 1, r.right, headH()};
                fill(dc, ln, P.line);
                int y = headH();
                if (shown.empty()) { RECT e{0, y + S(20), r.right, y + S(50)}; text(dc, "Nothing of this kind on the page.", e, P.muted, fontUI(), DT_CENTER); }
                for (int i = top; i < static_cast<int>(shown.size()) && y < r.bottom; i++, y += rowH()) {
                    const Pick& it = (*all)[static_cast<size_t>(shown[static_cast<size_t>(i)])];
                    RECT row{0, y, r.right, y + rowH()};
                    if (it.checked) fill(dc, row, blend(P.sel, P.field, 0.55f));
                    // tick box
                    int bs = S(16);
                    RECT box{S(14), y + (rowH() - bs) / 2, S(14) + bs, y + (rowH() - bs) / 2 + bs};
                    if (it.checked) { fillRound(dc, box, Sf(4), P.accent); RECT ck = box; InflateRect(&ck, -S(2), -S(2)); icon(dc, "check", ck, P.accentText, 2.6f); }
                    else strokeRound(dc, box, Sf(4), P.fieldLine);
                    RECT ic{S(44), y + (rowH() - S(16)) / 2, S(44) + S(16), y + (rowH() - S(16)) / 2 + S(16)};
                    icon(dc, kindIcon(it.kind), ic, P.folder, 1.7f);
                    RECT a{S(44) + S(24), y, S(44) + nameW - S(8), y + rowH()};
                    textName(dc, it.name, a, P.text, fontUI(), DT_END_ELLIPSIS);
                    RECT b{S(44) + nameW, y, S(44) + nameW + kindW - S(8), y + rowH()};
                    text(dc, kindLabel(it.kind), b, P.muted, fontUI(), DT_END_ELLIPSIS);
                    RECT c{S(44) + nameW + kindW, y, r.right - S(8), y + rowH()};
                    textName(dc, it.url, c, P.muted, fontSmall(), DT_END_ELLIPSIS);
                }
                return 0;
            }
            case WM_LBUTTONDOWN: {
                SetFocus(h);
                int y = GET_Y_LPARAM(l) - headH();
                if (y < 0) return 0;
                int i = top + y / rowH();
                if (i < 0 || i >= static_cast<int>(shown.size())) return 0;
                bool to = !(*all)[static_cast<size_t>(shown[static_cast<size_t>(i)])].checked;
                if ((w & MK_SHIFT) && anchor >= 0 && anchor < static_cast<int>(shown.size())) {
                    to = (*all)[static_cast<size_t>(shown[static_cast<size_t>(anchor)])].checked;
                    for (int k = std::min(anchor, i); k <= std::max(anchor, i); k++) (*all)[static_cast<size_t>(shown[static_cast<size_t>(k)])].checked = to;
                } else { (*all)[static_cast<size_t>(shown[static_cast<size_t>(i)])].checked = to; anchor = i; }
                redraw();
                if (onChange) onChange();
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
            case WM_GETDLGCODE: return DLGC_WANTARROWS;
        }
        return def(m, w, l);
    }
};

class LinksWnd : public Dialog {
public:
    LinkBatch batch;
    std::vector<Pick> items;
    std::vector<std::string> filterKeys;
    Label head, sub, lSave;
    DropDown filter;
    PickList list;
    Button all, none, start, later, cancel, browse;
    CheckBox sortCats;
    Field folder;

    void build() {
        std::set<std::string> seen;
        for (auto& it : batch.items) {
            std::string u = trim(it.url);
            if (u.empty() || u.size() > 8192 || seen.count(u)) continue;
            std::string l = lower(u);
            if (!(startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "ftp://") || isMagnet(u))) continue;
            seen.insert(u);
            Pick p;
            p.url = u;
            p.host = urlSite(u);
            std::string fromUrl = isMagnet(u) ? "" : fileNameFromUrl(u);
            std::string nm = !fromUrl.empty() && fromUrl != "download" && !extOf(fromUrl).empty() ? fromUrl : trim(it.text);
            if (isMagnet(u) && !links::magnetName(u).empty()) nm = links::magnetName(u);
            if (nm.empty()) nm = fromUrl.empty() ? u : fromUrl;
            p.kind = kindOf(u, fromUrl.empty() ? nm : fromUrl, it.kind);
            p.name = nm.size() > 200 ? nm.substr(0, 200) : nm;
            // ticked to begin with: every file (pictures for "all images"); web pages wait for you
            p.checked = batch.mode == "images" ? p.kind == "picture" : batch.mode == "selection" ? true : p.kind != "page";
            items.push_back(p);
        }
        std::string site = urlSite(batch.page);
        head.create(h, std::to_string(items.size()) + (items.size() == 1 ? " link" : " links") + (site.empty() ? "" : " on " + site));
        head.bold = true;
        sub.create(h, "Tick what to download. Click a row to tick it; Shift+click ticks everything in between.");
        sub.setColor(P.muted);
        sub.small = true;
        std::vector<std::string> names;
        auto addF = [&](const std::string& key, const std::string& label) {
            int n = 0;
            for (auto& p : items) if (match(key, p)) n++;
            if (n == 0 && key != "files" && key != "all") return;
            filterKeys.push_back(key);
            names.push_back(label + " (" + std::to_string(n) + ")");
        };
        addF("files", "All files"); addF("video", "Videos"); addF("picture", "Pictures"); addF("music", "Music"); addF("archive", "Archives");
        addF("document", "Documents"); addF("program", "Programs"); addF("magnet", "Torrents"); addF("page", "Web pages"); addF("all", "Every link");
        int sel = 0;
        if (batch.mode == "images") for (size_t i = 0; i < filterKeys.size(); i++) if (filterKeys[i] == "picture") sel = static_cast<int>(i);
        if (batch.mode == "selection") for (size_t i = 0; i < filterKeys.size(); i++) if (filterKeys[i] == "all") sel = static_cast<int>(i);
        filter.create(h, names, sel);
        filter.iconName = "M4 6h16 M7 12h10 M10 18h4";
        filter.onChange = [this](int) { refilter(); };
        list.all = &items;
        list.create(h);
        list.onChange = [this] { counts(); };
        all.create(h, "Tick all shown");
        none.create(h, "Untick all");
        all.onClick = [this] { for (int i : list.shown) items[static_cast<size_t>(i)].checked = true; list.redraw(); counts(); };
        none.onClick = [this] { for (auto& p : items) p.checked = false; list.redraw(); counts(); };
        sortCats.create(h, "Sort into category folders (Video, Music, Documents…)", batch.mode != "images");   // pictures: one folder named after the site
        lSave.create(h, "Save all to");
        lSave.setColor(P.muted);
        folder.create(h, joinPath(downloadsDir(), site.empty() ? "Links" : site));
        browse.create(h, "…");
        browse.onClick = [this] { std::string f = pickFolder(h, folder.get()); if (!f.empty()) folder.set(f); };
        sortCats.onChange = [this](bool) { layout(); };
        start.create(h, "Download", BtnStyle::Primary, "down");
        later.create(h, "Download Later");
        cancel.create(h, "Cancel");
        start.onClick = [this] { go(true); };
        later.onClick = [this] { go(false); };
        cancel.onClick = [this] { close(-1); };
        onEnter = [this] { go(true); };
        refilter();
    }

    static bool match(const std::string& key, const Pick& p) {
        if (key == "all") return true;
        if (key == "files") return p.kind != "page";
        return p.kind == key;
    }
    void refilter() {
        list.shown.clear();
        std::string key = filter.selected >= 0 && filter.selected < static_cast<int>(filterKeys.size()) ? filterKeys[static_cast<size_t>(filter.selected)] : "all";
        for (size_t i = 0; i < items.size(); i++) if (match(key, items[i])) list.shown.push_back(static_cast<int>(i));
        list.top = 0; list.anchor = -1;
        list.updateScroll();
        list.redraw();
        counts();
    }
    void counts() {
        int n = 0;
        for (auto& p : items) if (p.checked) n++;
        start.setText(n ? "Download " + std::to_string(n) : "Download");
        EnableWindow(start.hwnd(), n > 0);
        EnableWindow(later.hwnd(), n > 0);
        start.redraw(); later.redraw();
        layout();
    }

    void go(bool now) {
        std::vector<const Pick*> chosen;
        for (auto& p : items) if (p.checked) chosen.push_back(&p);
        if (chosen.empty()) return;
        std::string base = trim(folder.get());
        if (!sortCats.checked && (base.empty() || !makeDirs(base))) { askBox(h, "Save all to", "DDM can't use the folder " + base + ".", {"OK"}, 0); return; }
        if (chosen.size() > 200 && askBox(h, "Download links", "Download " + std::to_string(chosen.size()) + " items? DDM runs a few at a time (Options → Connection → Downloads at the same time).", {"Download them", "Cancel"}, 0) != 0) return;
        int added = 0;
        std::vector<std::pair<std::string, RequestInfo>> unnamed;
        for (auto* p : chosen) {
            if (isMegaFolderLink(p->url)) {   // a shared MEGA folder: its own list of files
                LinkRequest r; r.url = p->url; r.kind = "mega"; r.source = "browser"; r.pageUrl = batch.page;
                showMegaFolder(r);
                continue;
            }
            Task t;
            t.kind = p->kind == "magnet" || isTorrentFile(p->url) ? "torrent" : isMegaLink(p->url) ? "mega" : "http";
            t.url = p->url;
            t.pageUrl = batch.page;
            t.cookies = batch.cookies;
            t.userAgent = batch.ua;
            // the name: the file's own name in the address; a link without one takes the words on the page
            // ("Mountain lake"), and the server's name or file type is asked for just after (below)
            bool fromText = p->kind != "page" && extOf(p->name).empty() && p->name != p->url && !trim(p->name).empty();
            std::string nm = t.kind == "torrent" && isMagnet(p->url) ? sanitizeFileName(links::magnetName(p->url)) :
                             sanitizeFileName(fromText ? p->name : p->kind == "page" || extOf(p->name).empty() ? fileNameFromUrl(p->url) : p->name);
            if (nm.empty() && t.kind == "torrent") nm = sanitizeFileName(fileNameFromUrl(p->url));   // no name in the magnet: as a pasted magnet
            else if (nm.empty() || nm == "download") nm = sanitizeFileName(p->name);
            t.fileName = nm.empty() ? "download" : nm;
            t.category = categoryFor(t.fileName, t.kind == "torrent");
            t.site = urlSite(batch.page.empty() ? p->url : batch.page);
            t.dir = sortCats.checked ? "" : base;   // "" = the category's folder
            // no extension yet: held for the moment the server is asked (a running download is never renamed)
            bool askName = t.kind == "http" && extOf(t.fileName).empty();
            std::string id = Engine::get().add(t, now && !askName);
            added++;
            if (askName && !id.empty()) {
                RequestInfo q; q.url = t.url; q.referer = t.pageUrl; q.cookies = t.cookies; q.userAgent = t.userAgent;
                unnamed.push_back({id, q});
            }
        }
        if (!unnamed.empty()) {   // one helper for the whole batch: the server's name, or the right extension for its file type
            auto todo = unnamed;
            bool sorted = sortCats.checked, startAfter = now;
            std::thread([todo, sorted, startAfter] {
                static const std::map<std::string, std::string> mimeExt = {{"application/pdf", "pdf"}, {"application/zip", "zip"}, {"video/mp4", "mp4"}, {"audio/mpeg", "mp3"},
                    {"application/x-msdownload", "exe"}, {"application/x-msdos-program", "exe"}, {"application/x-rar-compressed", "rar"}, {"application/x-7z-compressed", "7z"},
                    {"image/jpeg", "jpg"}, {"image/png", "png"}, {"image/gif", "gif"}, {"image/webp", "webp"}, {"image/avif", "avif"}, {"image/svg+xml", "svg"},
                    {"image/bmp", "bmp"}, {"video/webm", "webm"}, {"audio/ogg", "ogg"}, {"audio/mp4", "m4a"}, {"application/x-iso9660-image", "iso"}};
                for (size_t i = 0; i < todo.size() && i < 300; i++) {
                    ResponseInfo info;
                    if (!probe(todo[i].second, info)) { if (startAfter) Engine::get().start(todo[i].first); continue; }
                    std::string mime = lower(trim(info.mime.substr(0, info.mime.find(';'))));
                    std::string server = sanitizeFileName(info.fileName);
                    auto it = mimeExt.find(mime);
                    Engine::get().edit(todo[i].first, [&](Task& t) {
                        if (t.state == State::Complete || t.state == State::Running || t.done > 0 || !extOf(t.fileName).empty()) return;   // started by you, or renamed by you meanwhile
                        if (!server.empty() && !extOf(server).empty()) t.fileName = server;
                        else if (it != mimeExt.end()) t.fileName += "." + it->second;
                        else return;
                        if (sorted) {   // now that the type is known: its category folder (unless a site rule chose another)
                            std::string cat = categoryFor(t.fileName);
                            if (iequals(t.dir, folderFor(t.category))) t.dir = folderFor(cat);
                            t.category = cat;
                        }
                    });
                    if (startAfter) Engine::get().start(todo[i].first);
                }
            }).detach();
        }
        logLine("links window: " + std::to_string(added) + " added from " + urlSite(batch.page) + (now ? "" : " (later)"));
        close(0);
    }

    void layout() override {
        RECT r = client();
        int pad = S(18), y = S(14);
        head.move(pad, y, r.right - 2 * pad - S(240), S(24));
        filter.move(r.right - pad - S(230), y - S(2), S(230), S(32));
        sub.move(pad, y + S(26), r.right - 2 * pad - S(240), S(20));
        int listTop = y + S(56);
        int bottomH = S(52) + (sortCats.checked ? S(34) : S(82));
        list.move(pad, listTop, r.right - 2 * pad, std::max<int>(S(120), r.bottom - listTop - bottomH - S(46)));
        int by = r.bottom - bottomH - S(38);
        all.move(pad, by, std::max(S(120), all.preferredWidth()), S(30));
        none.move(pad + std::max(S(120), all.preferredWidth()) + S(8), by, std::max(S(110), none.preferredWidth()), S(30));
        int sy = by + S(42);
        sortCats.move(pad, sy, r.right - 2 * pad, S(24));
        lSave.show(!sortCats.checked); folder.show(!sortCats.checked); browse.show(!sortCats.checked);
        if (!sortCats.checked) {
            lSave.move(pad, sy + S(34), S(90), S(30));
            folder.move(pad + S(96), sy + S(32), r.right - 2 * pad - S(96) - S(38), S(30));
            browse.move(r.right - pad - S(30), sy + S(32), S(30), S(30));
        }
        int x = r.right - pad, bh = S(36), byy = r.bottom - S(50);
        for (Button* b : {&cancel, &later, &start}) { int w = std::max(S(120), b->preferredWidth()); x -= w; b->move(x, byy, w, bh); x -= S(8); }
    }
};
} // namespace

void showLinksPicker(const LinkBatch& b) {
    if (b.items.empty()) return;
    if (b.items.size() == 1 && b.mode == "selection") {   // one link in the selection: the usual window
        LinkRequest r; r.url = b.items[0].url; r.pageUrl = b.page; r.cookies = b.cookies; r.userAgent = b.ua; r.source = "browser";
        App::get().newDownload(r);
        return;
    }
    auto* d = new LinksWnd();
    d->batch = b;
    if (!d->createDialog(App::get().mainHwnd(), b.mode == "images" ? "Download images" : "Download links", S(880), S(620), true)) { delete d; return; }
    d->build();
    d->layout();
    ShowWindow(d->hwnd(), SW_SHOW);
    SetForegroundWindow(d->hwnd());
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<LinksWnd*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
}

} // namespace ui
} // namespace ddm
