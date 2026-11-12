#include "app.hpp"
#include "../core/engines.hpp"
#include "../core/inspect.hpp"
#include "../core/pagepdf.hpp"
#include "../core/motw.hpp"
#include <map>
#include "../common/version.hpp"
#include "dialogs.hpp"
#include "gfx.hpp"
#include "mainwnd.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/http.hpp"
#include "../core/ipc.hpp"
#include "../core/settings.hpp"
#include "../core/integration.hpp"
#include "../core/integrity.hpp"
#include "../core/update.hpp"
#include "themefx.hpp"
#include <atomic>
namespace ddm { static std::atomic<long long> g_lastUpdateCheck{0}; }   // when DDM last asked for a newer version
#include "../core/siterules.hpp"
#include "../core/remote.hpp"
#include "../core/playlists.hpp"
#include "../core/queues.hpp"
#include "../core/after.hpp"
#include "../core/sound.hpp"
#include "../core/vault.hpp"
#include "../core/media.hpp"
#include "../core/streams.hpp"
#include "../core/mega.hpp"
#include "../core/torrent.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <mutex>
#include <atomic>
#include <set>
#include <thread>

namespace ddm {

App& App::get() { static App a; return a; }

HWND App::mainHwnd() const { return mainWnd ? mainWnd->hwnd() : nullptr; }

static LinkRequest requestFromJson(const Json& j) {
    LinkRequest r;
    r.url = j["url"].str();
    r.pageUrl = j["page"].str();
    r.referer = j["referrer"].str();
    r.cookies = j["cookies"].str();
    r.userAgent = j["ua"].str();
    r.suggestedName = j["filename"].str();
    r.mime = j["mime"].str();
    r.size = j["size"].i64(-1);
    r.favicon = j["favicon"].str();
    r.kind = j["kind"].str("http");
    r.videoFormat = j["format"].str();
    r.source = j["source"].str("browser");
    for (auto& kv : j["headers"].items()) r.headers[kv.first] = kv.second.str();
    r.direct = j["direct"].str();
    r.directCookies = j["directCookies"].str();
    r.pageData = j["pageData"].str();
    if (r.pageData.size() > 4 * 1024 * 1024) r.pageData.clear();
    r.title = j["title"].str();
    r.duration = j["duration"].i32();
    r.tab = j["tab"].isNumber() ? j["tab"].i32() : -1;
    { std::string q = j["quality"].str(); if (q == "best" || q == "mp3") r.quality = q; }
    for (auto& s : j["streams"].list()) {
        std::string su = s["url"].str(), sl = lower(su);
        if (!(startsWith(sl, "http://") || startsWith(sl, "https://")) || su.size() > 8192 || r.seen.size() >= 60) continue;
        r.streams.push_back(su);
        SeenStream x;
        x.url = su; x.kind = s["kind"].str(); x.mime = lower(s["mime"].str()).substr(0, 80); x.size = s["size"].i64(-1);
        if (x.kind != "hls" && x.kind != "dash" && x.kind != "file") x.kind.clear();
        {   // what the browser sent when it asked for it: names and values checked, nothing that DDM sets itself
            int n = 0;
            for (auto& kv : s["headers"].items()) {
                std::string k = kv.first, v = kv.second.str(), lk = lower(k);
                bool fine = !k.empty() && k.size() <= 64 && v.size() <= 3000 && ++n <= 24;
                for (unsigned char c : k) if (!isalnum(c) && c != '-') fine = false;
                for (unsigned char c : v) if (c < 0x20 || c == 0x7f) fine = false;
                if (lk == "cookie" || lk == "range" || lk == "host" || lk == "content-length" || lk == "accept-encoding" || lk == "connection" || lk == "te" || startsWith(lk, "if-") || startsWith(lk, "proxy-")) fine = false;
                if (fine) x.headers[k] = v;
            }
        }
        r.seen.push_back(x);
        // the stream's list of qualities as the browser itself read it (some sites answer only a real browser)
        const std::string text = s["text"].str();
        if (!text.empty() && text.size() <= 2 * 1024 * 1024 && (x.kind == "hls" || x.kind == "dash")) streams::remember(su, text);
    }
    r.playHeight = j["height"].isNumber() ? std::max(0, std::min(j["height"].i32(), 8640)) : 0;
    {   // a player inside another page (or one that keeps a single address for every video): the video's own page
        std::string top = trim(j["topUrl"].str());
        if (top.empty() && (j["type"].str() == "video" || j["type"].str() == "formats")) top = trim(j["page"].str());   // (add-ons before 4.0.7 send the tab's address as "page")
        if (looksLikeUrl(top) && top.size() < 4000 && top != r.url) r.topUrl = top;
        // the file the player is playing: asked for the way the browser asked for it
        if (!r.direct.empty()) for (auto& sx : r.seen) if (sx.url == r.direct) for (auto& kv : sx.headers) if (!r.headers.count(kv.first)) r.headers[kv.first] = kv.second;
        std::string t = j["type"].str();
        if (t == "video" || t == "formats") {
            if (!isVideoSite(r.url)) r.title = cleanVideoName(r.title, r.url, r.topUrl);   // the video's name, not the site's slogan
            std::string page = videoPageFor(r.url, r.topUrl, r.seen);
            if (page != r.url) { r.url = page; r.pageUrl = page; r.direct.clear(); }
        }
    }
    if (j["type"].str() == "video") r.kind = "video";
    if (r.pageUrl.empty()) r.pageUrl = r.referer;
    return r;
}

// ---- the browser's "Download this video" button: DDM reads the video's qualities, the button lists them, the one
// picked opens Download Video with that quality already chosen (no second wait).
namespace {
struct Offer { LinkRequest req; std::shared_ptr<VideoInfo> info; int64_t at = 0; };
std::mutex g_offerMu;
std::map<std::string, Offer> g_offers;                       // token -> offer (10 minutes)
std::map<std::string, std::string> g_offerByUrl;            // video address -> token (reuse while fresh)
std::map<std::string, std::shared_ptr<std::mutex>> g_probing; // one reading per address at a time

void forgetOldOffers() {
    int64_t now = nowMs();
    if (g_probing.size() > 200) for (auto it = g_probing.begin(); it != g_probing.end();) { if (it->second.use_count() == 1) it = g_probing.erase(it); else ++it; }
    for (auto it = g_offers.begin(); it != g_offers.end();) { if (now - it->second.at > 30 * 60 * 1000) it = g_offers.erase(it); else ++it; }
    for (auto it = g_offerByUrl.begin(); it != g_offerByUrl.end();) { if (!g_offers.count(it->second)) it = g_offerByUrl.erase(it); else ++it; }
}
} // namespace

// What makes two requests the same video: its page — and, where one page plays video after video without its
// address changing (a player in a frame, a feed), what is playing now: the stream the browser loaded last, the
// plain file, the length.
static std::string offerKey(const LinkRequest& r) {
    std::string k = r.url + "|" + r.topUrl + "|" + r.direct.substr(0, r.direct.find('?'));
    if (isFeedSite(r.url)) return k + "|" + std::to_string(r.duration);   // reels: the address may lag behind; the length tells them apart
    if (isVideoSite(r.url)) return k;   // a site DDM reads by its own rules: the page's address names the video
    for (auto it = r.seen.rbegin(); it != r.seen.rend(); ++it)
        if (it->kind == "hls" || it->kind == "dash") { k += "|" + it->url.substr(0, it->url.find('?')); break; }
    return k + "|" + std::to_string(r.duration);
}

static Json videoFormatsFor(const Json& msg) {
    Json reply = Json::object();
    if (!settings().catchVideos) { reply["ok"] = false; reply["error"] = "Turned off in DDM (Options → Browser)."; return reply; }
    LinkRequest r = requestFromJson(msg);
    r.kind = "video";
    if (r.url.empty() || !looksLikeUrl(r.url)) { reply["ok"] = false; reply["error"] = "This video has no address DDM can read."; return reply; }
    std::shared_ptr<std::mutex> one;
    {
        std::lock_guard<std::mutex> g(g_offerMu);
        forgetOldOffers();
        auto& m = g_probing[offerKey(r)];
        if (!m) m = std::make_shared<std::mutex>();
        one = m;
    }
    std::lock_guard<std::mutex> busy(*one);   // a prefetch already reading it: wait for that instead of reading twice
    std::string token;
    std::shared_ptr<VideoInfo> info;
    {
        std::lock_guard<std::mutex> g(g_offerMu);
        auto it = g_offerByUrl.find(offerKey(r));
        if (it != g_offerByUrl.end() && g_offers.count(it->second)) {   // read once, reused for half an hour (a failed reading is tried again)
            const Offer& o = g_offers[it->second];
            if (nowMs() - o.at < 30 * 60 * 1000 && o.info && !o.info->formats.empty()) { token = it->second; info = o.info; }
        }
    }
    if (!info) {
        info = std::make_shared<VideoInfo>();
        std::string dl = lower(r.direct);
        bool plainFile = (startsWith(dl, "http://") || startsWith(dl, "https://")) && !isVideoSite(r.url);
        if (plainFile) {   // a plain video file on an ordinary page: nothing to read, it's ready at once
            info->title = r.title; info->extractor = "generic"; info->pageUrl = r.url;
        } else {
            VideoRequest vr;
            vr.url = r.url; vr.cookies = r.cookies; vr.userAgent = r.userAgent; vr.referer = r.pageUrl; vr.streams = r.streams; vr.pageData = r.pageData; vr.seen = r.seen; vr.height = r.playHeight;
            vr.title = r.title; vr.duration = r.duration; vr.framed = !r.topUrl.empty(); vr.topUrl = r.topUrl; vr.direct = r.direct;
            probeVideo(vr, *info);
        }
        token = newId();
        std::lock_guard<std::mutex> g(g_offerMu);
        Offer o; o.req = r; o.info = info; o.at = nowMs();
        g_offers[token] = o;
        g_offerByUrl[offerKey(r)] = token;
    }
    VideoInfo copy = *info;
    auto list = ui::offeredFormats(copy, r);
    // plain files without a size: ask their servers at once (all together, at most a second and a half)
    {
        std::vector<std::shared_ptr<std::atomic<int64_t>>> sizes;
        for (auto& f : list) {
            auto sz = std::make_shared<std::atomic<int64_t>>(-2);
            sizes.push_back(sz);
            if (!f.direct || f.size > 0 || f.directUrl.empty()) { sz->store(-1); continue; }
            RequestInfo ri;
            ri.url = f.directUrl; ri.referer = r.url; ri.cookies = r.directCookies.empty() ? r.cookies : r.directCookies; ri.userAgent = r.userAgent;
            for (auto& kv : f.headers) if (lower(kv.first) != "cookie") ri.headers[kv.first] = kv.second;
            std::thread([ri, sz] { ResponseInfo inf; std::string e; sz->store(ddm::probe(ri, inf, &e) && inf.total > 0 ? inf.total : -1); }).detach();
        }
        for (int waited = 0; waited < 1500; waited += 50) {
            bool all = true;
            for (auto& sz : sizes) if (sz->load() == -2) all = false;
            if (all) break;
            Sleep(50);
        }
        for (size_t i = 0; i < list.size(); i++) { int64_t v = sizes[i]->load(); if (v > 0) { list[i].size = v; list[i].exact = true; } }
    }
    reply["ok"] = true;
    reply["token"] = token;
    reply["title"] = niceVideoTitle(copy.rawTitle.empty() ? copy.title : copy.rawTitle, copy.description, copy.uploader, r.title, copy.videoId);
    Json fl = Json::array();
    for (auto& f : list) {
        Json o = Json::object();
        std::string sz = f.size > 0 ? (f.exact ? "" : "~") + formatBytes(f.size) : "size unknown";
        std::string e = f.ext; for (auto& c : e) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        o["label"] = f.label + (!f.tag.empty() && f.tag != f.label && f.group == "Video" && f.height > 0 ? " " + f.tag : "");
        o["detail"] = (f.group == "Audio" ? (f.convert.empty() ? e : std::string("MP3")) : e) + (sz.empty() ? "" : " · " + sz);
        o["group"] = f.group;
        fl.push(o);
    }
    reply["formats"] = fl;
    if (list.empty()) reply["error"] = copy.error;
    return reply;
}

static Json videoPick(const Json& msg) {
    Json reply = Json::object();
    Offer o;
    {
        std::lock_guard<std::mutex> g(g_offerMu);
        auto it = g_offers.find(msg["token"].str());
        if (it == g_offers.end()) { reply["ok"] = false; reply["error"] = "That list is old: click the button again."; return reply; }
        o = it->second;
    }
    auto* r = new LinkRequest(o.req);
    r->probed = std::make_shared<VideoInfo>(*o.info);
    r->pick = msg["index"].isNumber() ? msg["index"].i32() : -1;
    r->refreshChecked = true;
    if (!PostMessageW(App::get().mainHwnd(), App::WM_LINK, 0, reinterpret_cast<LPARAM>(r))) { delete r; reply["ok"] = false; return reply; }
    reply["ok"] = true;
    return reply;
}

// Messages from the browser helper and from a second DDM.exe start. Runs on the pipe thread.
// DDM 3.x browser add-ons speak an older language ({cmd: …}): translated, so an add-on that wasn't updated yet
// still hands downloads and videos to DDM.
static Json fromOldAddon(const Json& m) {
    std::string cmd = m["cmd"].str();
    Json n = Json::object();
    n["source"] = "browser";
    n["cookies"] = m["cookies"].str();
    n["ua"] = m["userAgent"].str();
    if (cmd == "hello") n["type"] = "hello";
    else if (cmd == "show") n["type"] = "show";
    else if (cmd == "download") {
        n["type"] = "download"; n["url"] = m["url"].str(); n["referrer"] = m["referrer"].str(); n["page"] = m["referrer"].str();
        n["filename"] = m["fileName"].str(); n["mime"] = m["mime"].str(); n["size"] = m["size"].isNumber() ? m["size"].i64() : -1;
    } else if (cmd == "video-formats" || cmd == "page") {
        std::string v = m["videoUrl"].str();
        if (v.empty()) v = m["pageUrl"].str();
        if (v.empty()) v = m["url"].str();
        n["type"] = "video"; n["url"] = v; n["page"] = m["frameUrl"].str().empty() ? v : m["frameUrl"].str();
        n["title"] = m["title"].str(); n["direct"] = m["src"].str(); n["duration"] = m["duration"].isNumber() ? m["duration"].i32() : 0;
    } else n["type"] = "old-" + cmd;
    return n;
}

static Json handleIpc(const Json& msg0) {
    bool old = !msg0.has("type") && msg0.has("cmd");
    Json msg = old ? fromOldAddon(msg0) : msg0;
    std::string type = msg["type"].str();
    if (old && type == "hello") {   // what a 3.x add-on expects: its settings inside "config", no in-page buttons
        const Settings& s = settings();
        Json c = Json::object();
        c["catch"] = s.catchBrowser; c["types"] = s.browserTypes; c["minKB"] = s.minCatchKB; c["version"] = DDM_VERSION;
        c["videoButton"] = true;
        Json reply = Json::object(); reply["ok"] = true; reply["config"] = c;
        return reply;
    }
    if (old && (msg0["cmd"].str() == "video-formats")) {   // the old add-on asks for a list; DDM's own window shows it instead
        if (msg0["prefetch"].boolean() || msg0["cmd"].str().empty()) { Json r = Json::object(); r["ok"] = true; r["formats"] = Json::array(); return r; }
    }
    Json reply = Json::object();
    reply["ok"] = true;
    if (type == "hello") {
        const Settings& s = settings();
        reply["catch"] = s.catchBrowser;
        reply["types"] = s.browserTypes;
        reply["minKB"] = s.minCatchKB;
        Json skip = Json::array();
        for (auto& x : s.skipSites) skip.push(x);
        reply["skip"] = skip;
        reply["version"] = DDM_VERSION;
        reply["videoButton"] = s.catchVideos;
        if (!msg["browser"].str().empty()) App::get().browserSeen(msg["browser"].str());
        return reply;
    }
    if (type == "formats") return videoFormatsFor(msg);
    if (type == "pick") return videoPick(msg);
    if (type == "download" || type == "video" || type == "link") {
        auto* r = new LinkRequest(requestFromJson(msg));
        if (r->url.empty() || !PostMessageW(App::get().mainHwnd(), App::WM_LINK, 0, reinterpret_cast<LPARAM>(r))) { delete r; reply["ok"] = false; }
        return reply;
    }
    if (type == "pdf") {   // right-click → Save this page as PDF with DDM
        std::string url = trim(msg["url"].str()), title = trim(msg["title"].str()), html = msg["html"].str();   // html: the page as you saw it
        std::string l = lower(url);
        if (!(startsWith(l, "http://") || startsWith(l, "https://"))) { reply["ok"] = false; reply["error"] = "Only web pages can be saved as PDF."; return reply; }
        std::thread([url, title, html] {
            std::string name = sanitizeFileName(title.empty() ? urlSite(url) : title);
            if (name.empty() || name == "download") name = "page";
            if (name.size() > 120) name = name.substr(0, 120);
            std::string dir = folderFor("documents");
            makeDirs(dir);
            std::string out = uniquePath(joinPath(dir, name + ".pdf")), err;
            auto say = [](const std::string& t, const std::string& x) {
                auto* c = new std::pair<std::string, std::string>(t, x);
                if (!PostMessageW(App::get().mainHwnd(), WM_APP + 43, 0, reinterpret_cast<LPARAM>(c))) delete c;
            };
            if (!pagepdf::save(url, html, out, err)) { say("Couldn't save the page as PDF", err); return; }
            if (settings().markOfTheWeb) motw::mark(out, url, url);
            Task t;
            t.kind = "http"; t.url = url; t.pageUrl = url; t.fileName = baseName(out); t.dir = dir; t.category = "documents";
            t.site = urlSite(url); t.description = "Page saved as PDF";
            std::string id = Engine::get().add(t, false);
            int64_t size = fileSize(out);
            Engine::get().edit(id, [size](Task& x) { x.state = State::Complete; x.total = size; x.done = size; x.resumable = false; x.finished = nowMs(); x.lastTry = x.finished; x.pausedByUser = true; });
            say("Saved as PDF", baseName(out));
        }).detach();
        reply["ok"] = true;
        return reply;
    }
    if (type == "links") {   // several at once (right-click menu): ONE window lists them all, never a window per link
        auto* b = new ui::LinkBatch();
        b->mode = msg["mode"].str().empty() ? "links" : msg["mode"].str();
        b->page = msg["page"].str();
        b->cookies = msg["cookies"].str();
        b->ua = msg["ua"].str();
        for (auto& it : msg["items"].list()) {
            Json one = it;
            ui::LinkBatchItem x;
            x.url = one["url"].str();
            x.text = one["text"].str().empty() ? one["filename"].str() : one["text"].str();
            x.kind = one["kind"].str();
            if (!x.url.empty() && b->items.size() < 5000) b->items.push_back(x);
        }
        if (b->items.empty() || !PostMessageW(App::get().mainHwnd(), App::WM_LINKS, 0, reinterpret_cast<LPARAM>(b))) delete b;
        return reply;
    }
    if (type == "test" && GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0)) {   // tests only: open windows from a script
        auto* a = new std::string(msg.dump());
        PostMessageW(App::get().mainHwnd(), WM_APP + 9, 0, reinterpret_cast<LPARAM>(a));
        return reply;
    }
    if (type == "show") { PostMessageW(App::get().mainHwnd(), WM_COMMAND, ui::MainWindow::CmdShow, 0); return reply; }
    reply["ok"] = false;
    reply["error"] = "unknown request";
    return reply;
}

int App::run(HINSTANCE, const std::wstring& cmdLine) {
    // One DDM per user: hand the command line to the running one.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\DDM.Native.SingleInstance");
    bool already = GetLastError() == ERROR_ALREADY_EXISTS;
    std::string args = U(cmdLine);
    bool tray = args.find("/tray") != std::string::npos || args.find("--tray") != std::string::npos;   // (3.6 wrote --tray)
    {   // started again after updating itself while in the tray
        std::string mark = joinPath(dataDir(), "start-in-tray.txt");
        if (fileExists(mark)) { DeleteFileW(W(mark).c_str()); tray = true; }
    }
    // a .torrent file opened from Explorer ("DDM.exe" "C:\…\file.torrent")
    std::string torrentPath;
    {
        int n = 0;
        LPWSTR* av = CommandLineToArgvW(cmdLine.c_str(), &n);
        for (int i = 0; av && i < n; i++) {
            std::string a = U(av[i]);
            if (a.size() > 10 && a[1] == ':' && lower(extOf(a)) == "torrent" && fileExists(a) && fileSize(a) < 32 * 1024 * 1024) torrentPath = a;
        }
        if (av) LocalFree(av);
    }
    if (already) {
        Json m = Json::object();
        auto urls = extractUrls(args);
        if (!torrentPath.empty()) { m["type"] = "link"; m["url"] = torrentPath; m["source"] = "cmdline"; }
        else if (!urls.empty()) { m["type"] = "link"; m["url"] = urls.front(); m["source"] = "cmdline"; }
        else m["type"] = "show";
        Json r;
        ipcSend(m, r, 3000);
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (args.find("/purge-downloads") != std::string::npos) {   // uninstaller, when you agreed: what DDM downloaded goes to the Recycle Bin
        std::string text;
        if (readFile(joinPath(dataDir(), "downloads.json"), text)) {
            std::wstring list;
            int n = 0;
            Json all = Json::parse(text);   // (kept alive while its list is walked)
            for (auto& tj : all["downloads"].list()) {
                Task t = Task::fromJson(tj);
                for (const std::string& p : {t.path(), t.partPath()}) {
                    if (t.fileName.empty() || t.dir.empty() || p.size() < 4 || p[1] != ':') continue;
                    if (!fileExists(p) && !(t.kind == "torrent" && dirExists(p))) continue;
                    list += W(p); list.push_back(L'\0'); n++;
                }
            }
            if (n) {
                list.push_back(L'\0');
                CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                SHFILEOPSTRUCTW op{};
                op.wFunc = FO_DELETE; op.pFrom = list.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
                SHFileOperationW(&op);
            }
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (args.find("/unregister") != std::string::npos || args.find("--unregister") != std::string::npos) { integration::unregisterAll(); if (mutex) CloseHandle(mutex); return 0; }   // uninstaller
    if (!integrity::hostOk()) { if (mutex) CloseHandle(mutex); return 0; }   // DDM.exe was changed: don't run
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    loadSettings();
    i18n::setLanguage(settings().language.empty() ? i18n::systemLanguage() : settings().language);   // before any window
    setPartsFolder(settings().partsFolder);   // where unfinished downloads wait (before the engine starts)
    ui::initDpi();
    ui::applyTheme(settings().theme);
    ui::gfxInit();
    ui::initWidgets();
    http::globalInit();
    vault::clearTemp();   // copies left open last time
    sound::discard();     // a sound chosen in Options that wasn't confirmed
    integration::registerHost();
    integration::watchHandLoadedAddons();   // DDM's add-on loaded by hand in a browser: allowed once its files are checked
    integration::setStartWithWindows(settings().startWithWindows);
    Engine& e = Engine::get();
    e.load();

    mainWnd = new ui::MainWindow();
    if (!mainWnd->create()) return 1;
    e.onQueueDone = [](const std::string& action) {
        sound::play(sound::Event::QueueDone);
        auto* a = new std::string(action);
        if (!PostMessageW(App::get().mainHwnd(), WM_APP + 41, 0, reinterpret_cast<LPARAM>(a))) delete a;
    };
    e.onQueueFinished = [](const Queue& q) {
        sound::play(sound::Event::QueueDone);
        auto* c = new Queue(q);
        if (!PostMessageW(App::get().mainHwnd(), WM_APP + 42, 0, reinterpret_cast<LPARAM>(c))) delete c;
    };
    e.onNotice = [](const std::string& title, const std::string& text) {
        auto* c = new std::pair<std::string, std::string>(title, text);
        if (!PostMessageW(App::get().mainHwnd(), WM_APP + 43, 0, reinterpret_cast<LPARAM>(c))) delete c;
    };
    after::onAlert = [](const std::string& title, const std::string& text, const std::string& id) {
        sound::play(title.compare(0, 6, "Threat") == 0 ? sound::Event::Threat : sound::Event::Failed);
        auto* c = new std::pair<std::string, std::string>(title, text);
        if (!PostMessageW(App::get().mainHwnd(), WM_APP + 43, 0, reinterpret_cast<LPARAM>(c))) delete c;
        (void)id;
    };
    after::onNote = [](const std::string& title, const std::string& text) {
        auto* c = new std::pair<std::string, std::string>(title, text);
        if (!PostMessageW(App::get().mainHwnd(), WM_APP + 43, 0, reinterpret_cast<LPARAM>(c))) delete c;
    };
    e.onEvent = [](EngineEvent ev, const std::string& id) {
        if (ev == EngineEvent::Completed && !id.empty()) { after::finished(id); sound::play(sound::Event::Complete); }
        if (ev == EngineEvent::Failed && !id.empty()) {   // the first failure only: automatic retries don't ring again
            Task t;
            if (Engine::get().find(id, t) && !t.hidden && t.autoTries == 0) sound::play(sound::Event::Failed);
        }
        HWND hw = App::get().mainHwnd();
        if (!hw) return;
        auto* s = new std::string(id);
        if (!PostMessageW(hw, WM_ENGINE, static_cast<WPARAM>(ev), reinterpret_cast<LPARAM>(s))) delete s;
    };
    if (settings().autoResumeOnStart) e.resumeQueued();
    restartSeeding();   // torrents that were sharing when DDM closed share again
    ipcServe(handleIpc);
    {   // queues set to "Start download on DDM startup"
        std::vector<std::string> start;
        { std::lock_guard<std::mutex> g(queues::lock()); for (auto& q : queues::all()) if (q.startOnStartup) start.push_back(q.id); }
        for (auto& id : start) e.startQueue(id);
    }
    if (!tray) ShowWindow(mainWnd->hwnd(), settings().winMax ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    SetTimer(mainWnd->hwnd(), 77, 60 * 60 * 1000, nullptr);   // hourly: reminders, memory
    SetTimer(mainWnd->hwnd(), 78, 90 * 1000, nullptr);        // once, a minute and a half after starting
    // Background upkeep every 6 hours: store IDs for the browser extension, and "is there a newer DDM?"
    std::thread([] {
        Sleep(15000);
        // after an update: the installer DDM downloaded has done its job (the new DDM replaced the old one in place)
        { std::string upd = joinPath(dataDir(), "update"); if (fileExists(joinPath(upd, "DDM-Setup.exe"))) { removeTree(upd); logLine("update: the used installer was removed"); } }
        std::string updatedFrom;   // just updated (by itself or by hand): said once, with what's new
        {
            std::string mark = joinPath(dataDir(), "version.txt"), prev;
            readFile(mark, prev);
            prev = trim(prev);
            if (prev != DDM_VERSION) writeFileAtomic(mark, DDM_VERSION);
            if (!prev.empty() && prev != DDM_VERSION) updatedFrom = prev;
        }
        bool first = true;
        while (true) {
            integration::refreshStoreConfig();
            auto u = update::check(false);
            g_lastUpdateCheck = nowMs();
            if (first && !updatedFrom.empty()) { auto* p = new std::string(updatedFrom); if (!PostMessageW(App::get().mainHwnd(), WM_UPDATE, 5, reinterpret_cast<LPARAM>(p))) delete p; }
            if (u.newer) PostMessageW(App::get().mainHwnd(), WM_UPDATE, first ? 4 : 0, 0);   // at start: it pops up every time DDM starts
            first = false;
            inspect::refreshLists();   // the list of sites spreading malware (once a day)
            siterules::refresh(6);     // how sites show their videos: DedSec's newest signed rules
            Sleep(6 * 3600 * 1000);
        }
    }).detach();
    std::thread([] { Sleep(20 * 1000); engines::removeOld(); }).detach();   // what earlier versions fetched for videos
    // Phone remote (when it's on): what the phone asks is done by the main window
    remote::setHandler([](const std::string& a, const std::string& id, const std::string& url) {
        auto* p = new remote::Ask{a, id, url};
        if (!PostMessageW(App::get().mainHwnd(), WM_APP + 80, 0, reinterpret_cast<LPARAM>(p))) delete p;
    });
    if (settings().remoteOn) { std::string e; if (!remote::start(e)) logLine("phone remote: " + e); }
    // Subscriptions: new videos of the playlists and channels you follow (checked every 20 minutes, each every 3 hours)
    std::thread([] {
        Sleep(90 * 1000);
        while (true) {
            auto v = playlists::checkSubscriptions(false);
            if (!v.empty()) { auto* p = new std::vector<Task>(v); if (!PostMessageW(App::get().mainHwnd(), WM_APP + 81, 0, reinterpret_cast<LPARAM>(p))) delete p; }
            Sleep(20 * 60 * 1000);
        }
    }).detach();
    auto urls = extractUrls(args);
    if (!torrentPath.empty()) { LinkRequest r; r.url = torrentPath; r.source = "cmdline"; newDownload(r); }
    for (auto& u : urls) { LinkRequest r; r.url = u; r.source = "cmdline"; newDownload(r); }
    if (settings().startWithWindows) integration::setStartWithWindows(true);   // keeps the entry pointing at this DDM (and starting in the tray)

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        HWND top = GetAncestor(msg.hwnd, GA_ROOT);
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F3 && mainWnd) { mainWnd->command((GetKeyState(VK_SHIFT) & 0x8000) ? ui::MainWindow::CmdFindPrev : ui::MainWindow::CmdFindNext); continue; }
        if (top && top != mainHwnd() && IsDialogMessageW(top, &msg)) continue;
        if (msg.message == WM_KEYDOWN && GetAncestor(msg.hwnd, GA_ROOT) == mainHwnd()) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && msg.wParam == 'N') { addUrlDialog(); continue; }
            if (ctrl && msg.wParam == 'V' && GetFocus() != nullptr) {
                wchar_t cls[32] = {};
                GetClassNameW(GetFocus(), cls, 32);
                if (wcscmp(cls, L"Edit") != 0) { mainWnd->command(ui::MainWindow::CmdPasteUrl); continue; }
            }
            if (msg.wParam == VK_F1) { showHelp(); continue; }
            if (ctrl && msg.wParam == 'F') { mainWnd->command(ui::MainWindow::CmdFind); continue; }
            if (ctrl && msg.wParam == 'Z') { mainWnd->command(ui::MainWindow::CmdUndoDelete); continue; }   // brings back what was just removed
            if (ctrl && msg.wParam == 'B') { mainWnd->command(ui::MainWindow::CmdToggleTree); continue; }   // View → Hide / Show categories
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ipcStop();
    vault::lock();
    stopAllSeeding();
    e.shutdown();
    torrentShutdown();   // after the downloads stopped: their progress is saved, then the engine closes
    http::globalCleanup();
    ui::gfxShutdown();
    if (mutex) CloseHandle(mutex);
    return 0;
}

// A browser's DDM add-on said hello: remember it (the reminder stops asking for that browser).
void App::browserSeen(const std::string& name) {
    auto* s = new std::string(lower(name));
    if (!mainHwnd() || !PostMessageW(mainHwnd(), WM_APP + 46, 0, reinterpret_cast<LPARAM>(s))) delete s;
}

void App::settingsChanged() {
    if (mainWnd) InvalidateRect(mainWnd->hwnd(), nullptr, FALSE);
}

void App::openUrl(const std::string& url) {
    // only web pages: a page address comes from the browser or a pasted link, and Windows would also run programs,
    // open local files or protocol handlers (file:, ms-…:, search-ms:…) for other kinds of address
    std::string u = trim(url), l = lower(u);
    if (!(startsWith(l, "https://") || startsWith(l, "http://")) || u.find_first_of("\r\n\t\"") != std::string::npos) { logLine("not opened (not a web address): " + u.substr(0, 120)); return; }
    ShellExecuteW(nullptr, L"open", W(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::checkUpdatesIfStale() {
    static std::atomic<bool> busy{false};
    if (g_lastUpdateCheck == 0 || nowMs() - g_lastUpdateCheck < 3LL * 3600 * 1000 || busy.exchange(true)) return;
    HWND hw = mainHwnd();
    std::thread([hw] {
        auto u = update::check(true);
        g_lastUpdateCheck = nowMs();
        if (u.newer) PostMessageW(hw, WM_UPDATE, 0, 0);
        busy = false;
    }).detach();
}

void App::checkUpdatesNow(HWND owner) {
    HWND hw = mainHwnd();
    std::thread([hw] { update::check(true); PostMessageW(hw, WM_UPDATE, 1, 0); }).detach();
    (void)owner;
}

void App::offerUpdate(bool manual, bool atStart) {
    auto u = update::check(false);
    HWND owner = mainHwnd() && IsWindowVisible(mainHwnd()) ? mainHwnd() : nullptr;
    if (!u.newer) {
        if (manual) ui::askBox(owner, "Check for updates", u.error.empty() ? "You have the newest DDM (" DDM_VERSION ")." : u.error, {"OK"}, 0);
        return;
    }
    std::string mark = joinPath(dataDir(), "update-offered.txt"), last;
    readFile(mark, last);
    std::string today = u.version + " " + std::to_string(nowMs() / 86400000);
    if (!manual && !atStart && trim(last) == today && !settings().autoUpdate) return;   // found while DDM runs: asked at most once a day (at every start: always)
    writeFileAtomic(mark, today);
    if (!manual && settings().autoUpdate && !u.setup.empty()) {   // fetched quietly; DDM asks only once it's ready to install
        std::string setup = u.setup;
        HWND hw = mainHwnd();
        std::thread([setup, hw] {
            std::string err;
            std::string path = update::downloadSetup(setup, nullptr, err);
            if (path.empty()) { logLine("update: " + err); return; }
            auto* s = new std::string(path);
            if (!PostMessageW(hw, WM_UPDATE, 3, reinterpret_cast<LPARAM>(s))) delete s;   // installed by itself once nothing downloads
        }).detach();
        return;
    }
    std::string body = "DDM " + u.version + " is available (you have " DDM_VERSION ")." + (u.notes.empty() ? "" : "\n\n" + u.notes);
    int r = ui::askBox(owner, "A new DDM is available", body, u.setup.empty() ? std::vector<std::string>{"Open the download page", "Later"} : std::vector<std::string>{"Download and install", "Open the download page", "Later"}, 0);
    if (u.setup.empty()) { if (r == 0) openUrl(u.url); return; }
    if (r == 1) { openUrl(u.url); return; }
    if (r != 0) return;
    std::string setup = u.setup;
    HWND hw = mainHwnd();
    std::thread([setup, hw] {
        std::string err;
        std::string path = update::downloadSetup(setup, nullptr, err);
        auto* s = new std::string(path.empty() ? "!" + err : path);
        if (!PostMessageW(hw, WM_UPDATE, 2, reinterpret_cast<LPARAM>(s))) delete s;
    }).detach();
}

void App::newDownload(const LinkRequest& r0) {
    LinkRequest r = r0;
    if (r.url.empty()) return;
    r.url = trim(r.url);
    // a link you added yourself (pasted, Add URL, clipboard, Explorer): the user agent chosen in Options → Connection
    if (r.source != "browser" && trim(r.userAgent).empty() && !settings().manualUserAgent.empty()) r.userAgent = settings().manualUserAgent;
    {   // only web links, magnets and (from Explorer) a local .torrent file
        std::string l = lower(r.url);
        bool web = startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "ftp://") || isMagnet(r.url);
        bool localTorrent = r.source == "cmdline" && r.url.size() > 3 && r.url[1] == ':' && lower(extOf(r.url)) == "torrent" && fileExists(r.url);
        if (!web && !localTorrent) return;
    }
    if (isMagnet(r.url) || (r.kind == "http" && isTorrentFile(r.url))) r.kind = "torrent";
    else if (isMegaLink(r.url)) {
        r.kind = "mega";
        if (isMegaFolderLink(r.url)) { ui::showMegaFolder(r); return; }
    } else if (r.kind == "http" && r.source != "browser" && isVideoSite(r.url) && !isTorrentFile(r.url)) r.kind = "video";   // a pasted video page
    if (r.kind == "video" && !settings().catchVideos && r.source == "browser") return;
    if (ui::refreshTake(r)) return;   // the new link for a download waiting for Refresh Download Link
    if (!r.favicon.empty()) wantFavicon(urlSite(r.url), r.favicon);
    if (r.source != "remote" && r.kind != "torrent" && playlists::isListUrl(r.url)) { ui::showPlaylist(mainHwnd(), r.url, r.cookies); return; }   // a whole playlist / channel
    if (settings().showPrompt && r.source != "remote") { ui::showPrompt(r); return; }   // (from the phone: straight into the list)
    // silent: straight into the list
    Task t;
    t.kind = r.kind; t.url = r.url; t.pageUrl = r.pageUrl.empty() ? r.referer : r.pageUrl; t.cookies = r.cookies; t.userAgent = r.userAgent; t.headers = r.headers;
    t.fileName = sanitizeFileName(r.suggestedName.empty() ? fileNameFromUrl(r.url) : r.suggestedName);
    t.favicon = r.favicon; t.videoFormat = r.videoFormat; t.total = r.size;
    if (t.kind == "video" && t.fileName == "download") t.fileName = sanitizeFileName(r.title.empty() ? "video" : r.title) + ".mp4";
    std::string id = Engine::get().add(t, true);
    if (settings().showProgress) showProgress(id);
}

void App::addUrlDialog() { ui::showAddUrl(mainHwnd()); }
void App::showProgress(const std::string& id) { ui::showProgressWindow(id); }
void App::closeProgress(const std::string& id) { ui::closeProgressWindow(id); }
void App::showComplete(const std::string& id) { ui::showCompleteWindow(id); }
void App::showOptions() { ui::showOptionsWindow(mainHwnd()); }
void App::showHelp() { ui::showHelpWindow(mainHwnd()); }
void App::showAbout() { ui::showAboutWindow(mainHwnd()); }

void App::openFile(const std::string& id) {
    Task t;
    if (!Engine::get().find(id, t)) return;
    if (!fileExists(t.path())) { ui::askBox(mainHwnd(), "Open", "The file isn't in " + t.dir + " any more (moved or deleted).", {"OK"}, 0); return; }
    if (t.scanState == "threat" &&
        ui::askBox(mainHwnd(), "Threat found", t.scanNote + "\n\nOpening it could harm your computer.", {"Don't open", "Open anyway"}, 0) != 1) return;
    ShellExecuteW(mainHwnd(), L"open", W(t.path()).c_str(), nullptr, W(t.dir).c_str(), SW_SHOWNORMAL);
}

void App::openFolder(const std::string& id) {
    Task t;
    if (!Engine::get().find(id, t)) return;
    std::string target = fileExists(t.path()) ? t.path() : (fileExists(t.partPath()) ? t.partPath() : "");
    if (!target.empty()) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(W(target).c_str());
        if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); return; }
    }
    makeDirs(t.dir);
    ShellExecuteW(mainHwnd(), L"open", W(t.dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::setTheme(const std::string& mode) {
    settings().theme = mode;
    saveSettings();
    ui::themeTransition([this, mode] { ui::applyTheme(mode); retheme(); });   // the new look spreads out from where you clicked
}

void App::toggleTheme() { setTheme(ui::isDark() ? "light" : "dark"); }

void App::retheme() {
    if (mainWnd) mainWnd->onThemeChanged();
    ui::retintAll();
}

void App::restoreMain() { if (mainWnd) mainWnd->showFromTray(); }

void App::quit() {
    if (mainWnd && mainWnd->hwnd()) DestroyWindow(mainWnd->hwnd());
    PostQuitMessage(0);
}

// ---------------------------------------------------------------- favicons
static std::mutex g_favMu;
static std::set<std::string> g_favAsked;

void App::wantFavicon(const std::string& site, const std::string& url) {
    if (site.empty() || site == "magnet link" || site == "torrent file") return;
    {
        std::lock_guard<std::mutex> g(g_favMu);
        if (g_favAsked.count(site)) return;
        g_favAsked.insert(site);
    }
    std::string cache = joinPath(joinPath(dataDir(), "favicons"), sanitizeFileName(site) + ".img");
    std::string bytes;
    if (readFile(cache, bytes) && !bytes.empty()) { ui::setFavicon(site, bytes); return; }
    HWND hw = mainHwnd();
    std::thread([site, url, cache, hw] {
        std::vector<std::string> tries;
        if (!url.empty() && startsWith(url, "http")) tries.push_back(url);
        tries.push_back("https://" + site + "/favicon.ico");
        if (!startsWith(site, "www.")) tries.push_back("https://www." + site + "/favicon.ico");
        for (auto& u : tries) {
            RequestInfo r;
            r.url = u;
            std::string body;
            ResponseInfo info;
            long st = http::get(r, body, &info, 512 * 1024, 15);
            if (st == 200 && body.size() > 32 && info.mime.find("html") == std::string::npos) {
                makeDirs(dirName(cache));
                writeFileAtomic(cache, body);
                ui::setFavicon(site, body);
                if (hw) PostMessageW(hw, WM_FAVICON, 0, 0);
                return;
            }
        }
        writeFileAtomic(cache, "");   // none: don't ask again this session; retried next start
    }).detach();
}

} // namespace ddm
