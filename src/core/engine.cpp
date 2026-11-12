#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include "engine.hpp"
#include "media.hpp"
#include "queues.hpp"
#include "listvault.hpp"
#include "mega.hpp"
#include "torrent.hpp"
#include "downloader.hpp"
#include "settings.hpp"
#include "stats.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <thread>
#include <algorithm>

namespace ddm {

namespace {
// background downloads (started while Download File Info is open) that aren't plain HTTP work in their own folder
std::string stageDir(const std::string& id) { return joinPath(joinPath(dataDir(), "pre"), id); }
bool isStaged(const Task& t) { return t.kind != "http" && t.kind != "torrent" && !t.id.empty() && iequals(t.dir, stageDir(t.id)); }
bool isVideoPiece(const std::string& lowerName, const std::string& lowerStem, const std::string& lowerFile) {
    if (lowerName == lowerFile || !startsWith(lowerName, lowerStem + ".")) return false;
    std::string rest = lowerName.substr(lowerStem.size());
    if (startsWith(rest, ".stream") || startsWith(rest, ".joining.") || startsWith(rest, ".temp.")) return true;
    if (endsWith(lowerName, ".part") || endsWith(lowerName, ".ddmpart") || endsWith(lowerName, ".ytdl") || rest.find(".part-frag") != std::string::npos) return true;
    if (rest.size() > 3 && rest[1] == 'f' && isdigit(static_cast<unsigned char>(rest[2]))) {   // one format of the video before joining: name.f137.mp4
        size_t i = 2; while (i < rest.size() && isdigit(static_cast<unsigned char>(rest[i]))) i++;
        return i < rest.size() && rest[i] == '.';
    }
    return false;
}
// what an unfinished download leaves behind: its part file, and for videos the separate streams and pieces.
// Never the finished file.
void deleteLeftovers(const Task& t) {
    DeleteFileW(W(t.partPath()).c_str());
    if (t.kind == "torrent") return;
    if (isStaged(t)) { removeTree(t.dir); return; }
    if (t.kind != "video" || t.state == State::Complete || t.dir.empty()) return;
    if (!t.id.empty()) {   // its picture and sound in the parts folder
        std::string pf = partsFolder(), pre = lower(t.id) + ".", subPre = lower(t.id) + "-";   // (<id>-0.ddmpart: a stream still arriving)
        for (auto& n : listDir(pf)) {
            if (!startsWith(lower(n), pre) && !startsWith(lower(n), subPre)) continue;
            std::string p = joinPath(pf, n);
            if (dirExists(p)) removeTree(p); else DeleteFileW(W(p).c_str());
        }
    }
    std::string st = lower(stem(t.fileName)), fn = lower(t.fileName);
    for (auto& n : listDir(t.dir)) if (isVideoPiece(lower(n), st, fn)) DeleteFileW(W(joinPath(t.dir, n)).c_str());
}
} // namespace


Engine& Engine::get() { static Engine e; return e; }

std::shared_ptr<Task> Engine::byId(const std::string& id) {
    for (auto& t : tasks) if (t->id == id) return t;
    return nullptr;
}

namespace {
// DDM 3.x (%APPDATA%\DDM\downloads.json): its list comes over once, the first time 4.0 starts. Finished downloads
// stay listed (Open, Open folder, Properties…); unfinished ones come back paused, to start again from the beginning
// (3.x kept their parts in its own format).
std::vector<std::shared_ptr<Task>> importFrom3x() {
    std::vector<std::shared_ptr<Task>> out;
    if (fileExists(joinPath(exeDir(), "portable.txt"))) return out;
    wchar_t roam[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, roam))) return out;
    std::string text;
    if (!readFile(joinPath(joinPath(U(roam), "DDM"), "downloads.json"), text) || text.size() > 64 * 1024 * 1024) return out;
    Json arr = Json::parse(text);
    if (!arr.isArray()) return out;
    for (auto& o : arr.list()) {
        std::string url = trim(o["url"].str()), kind = o["kind"].str(), state = o["state"].str(), path = o["path"].str();
        if (url.empty() || url.size() > 8192) continue;
        std::string l = lower(url);
        if (!(startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "ftp://") || startsWith(l, "magnet:?"))) continue;
        auto t = std::make_shared<Task>();
        t->id = newId();
        t->kind = kind == "Media" ? "video" : kind == "Torrent" ? "torrent" : (l.find("mega.nz/") != std::string::npos || l.find("mega.co.nz/") != std::string::npos) ? "mega" : "http";
        t->url = url;
        std::string name = sanitizeFileName(!path.empty() ? baseName(path) : !o["file"].str().empty() ? o["file"].str() : fileNameFromUrl(url));
        t->fileName = name.empty() ? "download" : name;
        t->dir = !path.empty() && path.size() > 3 && path[1] == ':' ? dirName(path) : "";
        if (t->dir.empty()) t->dir = folderFor(categoryFor(t->fileName, t->kind == "torrent"));
        t->category = categoryFor(t->fileName, t->kind == "torrent");
        t->description = o["desc"].str();
        t->site = urlSite(url);
        t->videoFormat = o["fmt"].str(); t->formatLabel = o["fmtLabel"].str(); t->convertTo = o["convert"].str() == "mp3" ? "mp3" : "";
        t->priority = std::max(0, std::min(2, o.has("priority") ? o["priority"].i32() : 1));
        t->created = o["created"].i64(); if (t->created <= 0) t->created = nowMs();
        t->checksum = o["hash"].str().find(':') != std::string::npos ? lower(o["hash"].str()) : "";
        bool done = state == "Done" || state == "Seeding";
        if (done && (fileExists(t->path()) || dirExists(t->path()))) {
            t->state = State::Complete;
            t->total = o["total"].i64() > 0 ? o["total"].i64() : fileSize(t->path());
            t->done = t->total;
            t->finished = o["finished"].i64() > 0 ? o["finished"].i64() : t->created;
            t->lastTry = t->finished;
        } else if (!done) {
            t->state = State::Paused;
            t->pausedByUser = true;
            t->total = -1; t->done = 0;
        } else continue;   // finished, but the file isn't there any more
        out.push_back(t);
        if (out.size() >= 5000) break;
    }
    return out;
}
} // namespace

void Engine::load() {
    std::string text;
    std::lock_guard<std::mutex> g(mu);
    tasks.clear();
    std::string listFile = joinPath(dataDir(), "downloads.json"), imported = joinPath(dataDir(), "imported-3x.txt");
    if (!fileExists(listFile) && !fileExists(imported)) {
        auto old = importFrom3x();
        writeFileAtomic(imported, std::to_string(old.size()));
        for (auto& t : old) tasks.push_back(t);
        if (!old.empty()) { dirty = true; logLine("imported " + std::to_string(old.size()) + " downloads from DDM 3.x"); }
    }
    if (readFile(listFile, text)) {
        Json j = Json::parse(text);
        for (auto& tj : j["downloads"].list()) {
            auto t = std::make_shared<Task>(Task::fromJson(tj));
            if (t->id.empty()) continue;
            if (t->hidden) { deleteLeftovers(*t); continue; }   // a window that was never answered
            tasks.push_back(t);
        }
    }
    // anything else left in the background folder (DDM closed while a window was open)
    {
        std::string pre = joinPath(dataDir(), "pre");
        for (auto& n : listDir(pre)) {
            std::string id = endsWith(n, ".ddmpart") ? n.substr(0, n.size() - 8) : n;
            bool known = false;
            for (auto& t : tasks) if (t->id == id) known = true;
            if (known) continue;
            std::string p = joinPath(pre, n);
            if (dirExists(p)) removeTree(p); else DeleteFileW(W(p).c_str());
        }
    }
    applySpeedLimit();
    // queues: membership lives in queues.json; downloads from DDM 4.0 betas with "scheduled" join the Main queue
    bool fresh = !fileExists(joinPath(dataDir(), "queues.json"));
    queues::load();
    {
        std::lock_guard<std::mutex> qg(queues::lock());
        if (fresh) {
            const Settings& st = settings();
            Queue* m = queues::find("main");
            if (m && st.scheduleEnabled) {
                m->startAtOn = true; m->startAt = st.scheduleStartMin * 60; m->daily = true;
                std::string d(7, '1');
                if (st.scheduleDays.size() == 7) for (int i = 0; i < 7; i++) d[static_cast<size_t>((i + 1) % 7)] = st.scheduleDays[static_cast<size_t>(i)];   // Mon..Sun → Sun..Sat
                m->days = d;
                m->stopAtOn = st.scheduleStopMin != st.scheduleStartMin; m->stopAt = st.scheduleStopMin * 60;
            }
            for (auto& t : tasks) if (t->scheduled && m) m->order.push_back(t->id);
        }
        for (auto& t : tasks) {
            t->queue.clear();
            for (auto& q : queues::all()) if (std::find(q.order.begin(), q.order.end(), t->id) != q.order.end()) t->queue = q.id;
        }
        // drop ids that are no longer in the list
        for (auto& q : queues::all())
            q.order.erase(std::remove_if(q.order.begin(), q.order.end(), [this](const std::string& id) { return !byId(id); }), q.order.end());
    }
    if (fresh) queues::save();
    // Saving happens off the window's thread, at most every 2 seconds.
    saver = std::thread([this] {
        int ticks = 0;
        while (!quitting) {
            for (int i = 0; i < 20 && !quitting; i++) Sleep(100);
            if (dirty.exchange(false)) save();
            if (++ticks % 150 == 0) backupIfDue();   // every 5 minutes: automatic list backup when due
            if (ticks % 10 == 0) tick();             // every 20 seconds: scheduler and conditions
        }
    });
}

void Engine::resumeQueued() {
    watchNetwork();
    watchIdle();
    { std::lock_guard<std::mutex> g(mu); schedule(); }
    fire(EngineEvent::Changed, "");
}

void Engine::save() {
    Json arr = Json::array();
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tasks) arr.push(t->toJson());
    }
    Json j = Json::object();
    j["version"] = 1;
    j["downloads"] = arr;
    writeFileAtomic(joinPath(dataDir(), "downloads.json"), j.dump());
}

std::string Engine::exportJson() {
    Json arr = Json::array();
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tasks) arr.push(t->toJson());
    }
    Json j = Json::object();
    j["version"] = 1;
    j["app"] = "DDM";
    j["downloads"] = arr;
    return j.dump(true);
}

int Engine::importList(const std::string& data, std::string& error) {
    std::string text = data;
    if (listvault::looksLikeDdm(data)) {
        if (!listvault::decrypt(std::vector<unsigned char>(data.begin(), data.end()), text, error)) return -1;
    }
    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok) { error = "This file isn't a DDM download list."; return -1; }
    const Json& list = j.isArray() ? j : (j["downloads"].isArray() ? j["downloads"] : j["tasks"]);
    int added = 0;
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& tj : list.list()) {
            Task t = Task::fromJson(tj);
            if (t.url.empty()) t.url = tj["Url"].str();
            if (t.url.empty()) continue;
            {   // a list file can come from anyone: only web links, plain file names, ordinary folders
                std::string l = lower(trim(t.url));
                if (!(startsWith(l, "http://") || startsWith(l, "https://") || startsWith(l, "ftp://") || startsWith(l, "magnet:?"))) continue;
                if (!t.fileName.empty()) t.fileName = sanitizeFileName(t.fileName);
                std::string d = lower(t.dir);
                bool absolute = t.dir.size() > 3 && t.dir[1] == ':' && (t.dir[2] == '\\' || t.dir[2] == '/');
                if (!absolute || d.find("..") != std::string::npos || d.find("\\start menu\\programs\\startup") != std::string::npos ||
                    d.find("/start menu/programs/startup") != std::string::npos || startsWith(d.substr(std::min<size_t>(d.size(), 2)), "\\windows")) t.dir.clear();
                if (!iequals(t.tempDir, joinPath(dataDir(), "pre"))) t.tempDir.clear();   // only DDM's own folder for unfinished downloads
            }
            if (t.id.empty()) t.id = newId();
            bool dup = std::any_of(tasks.begin(), tasks.end(), [&](const std::shared_ptr<Task>& x) { return x->id == t.id || (x->url == t.url && x->fileName == t.fileName); });
            if (dup) continue;
            if (t.fileName.empty()) t.fileName = fileNameFromUrl(t.url);
            if (t.dir.empty()) t.dir = folderFor(categoryFor(t.fileName, t.kind == "torrent"));
            if (t.state == State::Running || t.state == State::Queued) t.state = State::Paused;
            if (t.created == 0) t.created = nowMs();
            tasks.push_back(std::make_shared<Task>(t));
            added++;
        }
    }
    dirty = true;
    fire(EngineEvent::Added, "");
    return added;
}

std::string Engine::backupFolder() { std::string d = joinPath(dataDir(), "backups"); makeDirs(d); return d; }

void Engine::backupNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char name[64];
    snprintf(name, sizeof name, "ddm-list-%04d%02d%02d-%02d%02d.ddm", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    auto blob = listvault::encrypt(exportJson());
    std::string dir = backupFolder();
    writeFileAtomic(joinPath(dir, name), std::string(blob.begin(), blob.end()));
    writeFileAtomic(joinPath(dir, "last.txt"), std::to_string(nowMs()));
    // keep the newest 12
    std::vector<std::string> files;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(W(joinPath(dir, "ddm-list-*.ddm")).c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) { do files.push_back(U(fd.cFileName)); while (FindNextFileW(f, &fd)); FindClose(f); }
    std::sort(files.rbegin(), files.rend());
    for (size_t i = 12; i < files.size(); i++) DeleteFileW(W(joinPath(dir, files[i])).c_str());
}

void Engine::backupIfDue() {
    const Settings& s = settings();
    if (!s.listBackupEnabled) return;
    std::string last;
    readFile(joinPath(backupFolder(), "last.txt"), last);
    int64_t at = _atoi64(trim(last).c_str());
    if (nowMs() - at >= static_cast<int64_t>(std::max(1, s.listBackupHours)) * 3600 * 1000) backupNow();
}

void Engine::shutdown() {
    stats::save();
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& kv : stops) kv.second->store(true);
    }
    for (int i = 0; i < 100 && workers.load() > 0; i++) Sleep(50);
    quitting = true;
    if (saver.joinable()) saver.join();
    save();
}

void Engine::applySpeedLimit() { setGlobalSpeedLimit(idleBoosted ? 0 : static_cast<int64_t>(settings().speedLimitKBps) * 1024); }

namespace {
// all bytes that arrived on the PC's real network adapters (Wi-Fi, Ethernet, mobile), 0 when unknown
int64_t adapterBytesIn() {
    typedef DWORD(WINAPI * Get)(PMIB_IF_TABLE2*);
    typedef VOID(WINAPI * Free)(PVOID);
    static HMODULE ip = systemDll(L"iphlpapi.dll");
    static auto get = reinterpret_cast<Get>(ip ? GetProcAddress(ip, "GetIfTable2") : nullptr);
    static auto fr = reinterpret_cast<Free>(ip ? GetProcAddress(ip, "FreeMibTable") : nullptr);
    if (!get || !fr) return 0;
    PMIB_IF_TABLE2 t = nullptr;
    if (get(&t) != NO_ERROR || !t) return 0;
    int64_t sum = 0;
    for (ULONG i = 0; i < t->NumEntries; i++) {
        const MIB_IF_ROW2& r = t->Table[i];
        if (r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK || !r.InterfaceAndOperStatusFlags.HardwareInterface || r.InterfaceAndOperStatusFlags.FilterInterface) continue;
        sum += static_cast<int64_t>(r.InOctets);
    }
    fr(t);
    return sum;
}
} // namespace

// Smart speed: with a speed limit set, DDM watches how much the rest of the PC downloads. While nothing else uses
// the internet (for 10 seconds), the limit is lifted; as soon as something else needs it, the limit is back.
void Engine::watchIdle() {
    std::thread([this] {
        int64_t lastAll = adapterBytesIn(), lastMine = receivedBytes();
        int quiet = 0;
        while (!quitting) {
            Sleep(2000);
            int64_t all = adapterBytesIn(), mine = receivedBytes();
            int64_t others = std::max<int64_t>(0, (all - lastAll) - (mine - lastMine)) / 2;   // bytes per second
            lastAll = all; lastMine = mine;
            const Settings& s = settings();
            bool can = s.idleBoost && s.speedLimitKBps > 0 && all > 0;
            bool want = idleBoosted;
            if (!can) { want = false; quiet = 0; }
            else if (others < 24 * 1024) { if (++quiet >= 5) want = true; }   // 10 s of a quiet network
            else { quiet = 0; want = false; }
            if (want != idleBoosted) { idleBoosted = want; applySpeedLimit(); fire(EngineEvent::Changed, ""); }
        }
    }).detach();
}

std::string Engine::add(Task t, bool startNow) {
    if (t.id.empty()) t.id = newId();
    if (t.created == 0) t.created = nowMs();
    if (t.site.empty()) t.site = urlSite(t.pageUrl.empty() || t.kind == "http" ? t.url : t.pageUrl);
    if (t.category.empty()) t.category = categoryFor(t.fileName, t.kind == "torrent");
    if (t.dir.empty()) { t.dir = folderByRule(t.site); if (t.dir.empty()) t.dir = folderFor(t.category); }
    t.state = startNow ? State::Queued : State::Paused;
    auto p = std::make_shared<Task>(std::move(t));
    std::string id = p->id;
    {
        std::lock_guard<std::mutex> g(mu);
        // another unfinished download already writes to this name: "name (2).ext", like Windows does
        auto clash = [&](const std::string& path) {
            for (auto& o : tasks) if (o->state != State::Complete && lower(o->path()) == lower(path)) return true;
            return false;
        };
        if (p->kind != "torrent" && clash(p->path())) {
            std::string st = stem(p->fileName), ex = extOf(p->fileName);
            for (int n = 2; n < 1000; n++) {
                p->fileName = st + " (" + std::to_string(n) + ")" + (ex.empty() ? "" : "." + ex);
                if (!clash(p->path()) && !fileExists(p->path())) break;
            }
        }
        tasks.push_back(p);
        schedule();
    }
    dirty = true;
    fire(EngineEvent::Added, id);
    return id;
}

void Engine::start(const std::string& id) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t || t->state == State::Complete) return;
        if (unstaging.count(id)) return;   // being moved to its folder: starts by itself right after
        if (t->state == State::Running) {   // still stopping (Pause then Resume quickly): run again once it has stopped
            auto it = stops.find(id);
            if (it != stops.end() && it->second->load()) restartAfterStop.insert(id);
            return;
        }
        t->state = State::Queued;
        t->error.clear();
        t->pausedByUser = false;
        t->stopped = false;
        t->autoTries = 0;
        schedule();
    }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::stop(const std::string& id) {
    stopSeeding(id);   // (a finished torrent: stops sharing)
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t) return;
        if (t->state != State::Complete) t->stopped = true;
    }
    pause(id);
}

void Engine::pause(const std::string& id) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t) return;
        auto it = stops.find(id);
        restartAfterStop.erase(id);
        t->pausedByUser = true;
        if (it != stops.end()) { it->second->store(true); t->status = "Pausing…"; }
        else if (t->state == State::Queued) t->state = State::Paused;
    }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::remove(const std::string& id, bool deleteFile) {
    queues::removeTask(id);
    stopSeeding(id);
    std::shared_ptr<Task> t;
    bool running = false;
    {
        std::lock_guard<std::mutex> g(mu);
        t = byId(id);
        if (!t) return;
        auto it = stops.find(id);
        if (it != stops.end()) { it->second->store(true); running = true; }
        tasks.erase(std::remove(tasks.begin(), tasks.end(), t), tasks.end());
    }
    dirty = true;
    fire(EngineEvent::Removed, id);   // gone from the list at once; the files are cleaned up behind the scenes
    auto clean = [this, id, t, deleteFile] {
        // its worker lets go of the part file first (at most 10 s)
        for (int i = 0; i < 200; i++) { { std::lock_guard<std::mutex> g(mu); if (!stops.count(id)) break; } Sleep(50); }
        deleteLeftovers(*t);
        if (deleteFile && t->state == State::Complete && !t->fileName.empty()) {
            // to the Recycle Bin, never a hard delete
            std::string p = t->path();
            if (fileExists(p) || dirExists(p)) {
                HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                std::wstring from = W(p);
                from.push_back(L'\0');
                SHFILEOPSTRUCTW op{};
                op.wFunc = FO_DELETE;
                op.pFrom = from.c_str();
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
                int r = SHFileOperationW(&op);
                if (r != 0 || op.fAnyOperationsAborted) logLine("recycle " + p + ": failed (" + std::to_string(r) + ")");
                if (SUCCEEDED(co)) CoUninitialize();
            }
        }
    };
    if (running) std::thread(clean).detach(); else clean();
}

void Engine::redownload(const std::string& id) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t || stops.count(id)) return;
        DeleteFileW(W(t->partPath()).c_str());
        t->segments.clear();
        t->done = 0;
        t->total = -1;
        t->finished = 0;
        t->error.clear();
        t->state = State::Queued;
        schedule();
    }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::refreshLink(const std::string& id, const std::string& newUrl) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t) return;
        t->url = newUrl;
        if (t->state == State::Error) t->state = State::Paused;
        t->error.clear();
    }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::refreshFrom(const std::string& id, const Task& f) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t) return;
        if (!f.url.empty()) t->url = f.url;
        if (!f.cookies.empty()) t->cookies = f.cookies;
        if (!f.userAgent.empty()) t->userAgent = f.userAgent;
        if (!f.headers.empty()) t->headers = f.headers;
        if (!f.pageUrl.empty()) t->pageUrl = f.pageUrl;
        if (t->kind == "video") { t->mediaUrl = f.mediaUrl; t->audioUrl = f.audioUrl; if (!f.mediaExt.empty()) t->mediaExt = f.mediaExt; if (!f.audioExt.empty()) t->audioExt = f.audioExt; }
        if (t->total <= 0 && f.total > 0) t->total = f.total;
        if (t->state == State::Error) t->state = State::Paused;
        t->error.clear();
        t->queueTries = 0;
    }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::setTorrentFiles(const std::string& id, const std::string& files) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t || t->kind != "torrent") return;
        t->videoFormat = files;
        auto it = stops.find(id);
        if (it != stops.end()) { it->second->store(true); restartAfterStop.insert(id); }   // picks up the new choice at once
    }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::startAll() {
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tasks) {
            if (t->hidden || unstaging.count(t->id) || t->stopped) continue;   // (stopped ones stay stopped until started by hand)
            if (t->state == State::Paused || t->state == State::Error) {
                t->state = State::Queued; t->error.clear(); t->pausedByUser = false; t->autoTries = 0;
            } else if (t->state == State::Running) {   // still stopping after Pause all: runs again once it has stopped
                auto it = stops.find(t->id);
                if (it != stops.end() && it->second->load()) restartAfterStop.insert(t->id);
            }
        }
        schedule();
    }
    dirty = true;
    fire(EngineEvent::Changed, "");
}

void Engine::pauseAll() {
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tasks) {
            if (t->hidden) continue;   // (one still waiting in Download File Info isn't the list's)
            if (t->state == State::Queued) t->state = State::Paused;
            if (t->state != State::Complete) t->pausedByUser = true;   // queues and automatic retries leave it paused
            restartAfterStop.erase(t->id);
            auto it = stops.find(t->id);
            if (it != stops.end()) { it->second->store(true); t->status = "Pausing…"; }
        }
    }
    dirty = true;
    fire(EngineEvent::Changed, "");
}

void Engine::removeFinished() {
    {
        std::lock_guard<std::mutex> g(mu);
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const std::shared_ptr<Task>& t) { return t->state == State::Complete; }), tasks.end());
    }
    dirty = true;
    fire(EngineEvent::Removed, "");
}

void Engine::setDescription(const std::string& id, const std::string& text) {
    std::lock_guard<std::mutex> g(mu);
    auto t = byId(id);
    if (t) { t->description = text; dirty = true; }
}

void Engine::edit(const std::string& id, const std::function<void(Task&)>& change) {
    { std::lock_guard<std::mutex> g(mu); auto t = byId(id); if (!t) return; change(*t); }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::setOptions(const std::string& id, int limitKBps, bool openWhenDone, bool shutdownWhenDone) {
    std::lock_guard<std::mutex> g(mu);
    auto t = byId(id);
    if (!t) return;
    t->limitKBps = limitKBps;
    t->openWhenDone = openWhenDone;
    t->shutdownWhenDone = shutdownWhenDone;
    dirty = true;
}

std::vector<Task> Engine::snapshot() {
    std::lock_guard<std::mutex> g(mu);
    std::vector<Task> r;
    r.reserve(tasks.size());
    for (auto& t : tasks) if (!t->hidden) r.push_back(*t);
    return r;
}

void Engine::setScan(const std::string& id, const std::string& state, const std::string& note) {
    { std::lock_guard<std::mutex> g(mu); auto t = byId(id); if (!t) return; t->scanState = state; t->scanNote = note; }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

void Engine::setVerify(const std::string& id, const std::string& state, const std::string& checksum) {
    { std::lock_guard<std::mutex> g(mu); auto t = byId(id); if (!t) return; t->verify = state; if (!checksum.empty()) t->checksum = checksum; }
    dirty = true;
    fire(EngineEvent::Changed, id);
}

std::string Engine::preStart(Task t) {
    {   // many windows at once (a page opening downloads by itself): only the first few start early
        std::lock_guard<std::mutex> g(mu);
        int early = 0;
        for (auto& x : tasks) if (x->hidden) early++;
        if (early >= 6) return "";
    }
    if (t.id.empty()) t.id = newId();
    t.hidden = true;
    if (t.kind == "http" || t.kind == "mega") { t.tempDir = joinPath(dataDir(), "pre"); makeDirs(t.tempDir); }
    else t.tempDir.clear();   // videos work in their own folder under their own name from the first second
    if (t.created == 0) t.created = nowMs();
    t.state = State::Queued;
    t.forceStart = true;   // right away, whatever else is running
    auto p = std::make_shared<Task>(std::move(t));
    std::string id = p->id;
    { std::lock_guard<std::mutex> g(mu); tasks.push_back(p); schedule(); }
    return id;
}

void Engine::preCancel(const std::string& id) {
    std::thread([this, id] { remove(id, false); }).detach();
}

void Engine::preConfirm(const std::string& id, const Task& f, bool startNow) {
    bool moveNow = false, videoDone = false;
    std::string from, to;
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t) return;
        t->extras = f.extras;   // thumbnail / subtitles chosen in the window
        t->subs = f.subs;
        if (t->kind == "video" && !isStaged(*t)) {
            t->description = f.description; t->site = f.site; t->favicon = f.favicon; t->category = f.category;
            t->forceStart = false;
            bool same = iequals(t->dir, f.dir) && iequals(stem(t->fileName), stem(f.fileName));
            bool complete = t->state == State::Complete;
            if (same) {   // the usual case: it's already where it belongs
                t->hidden = false;
                if (complete) t->finished = nowMs();
                else if (!startNow) { t->pausedByUser = true; auto it = stops.find(id); if (it != stops.end()) it->second->store(true); else t->state = State::Paused; }
                else if (t->state == State::Error || t->state == State::Paused) { t->state = State::Queued; t->error.clear(); schedule(); }
                videoDone = complete;
            } else {
            // another folder or name: move what arrived, then carry on
            unstaging.insert(id);
            auto it = stops.find(id);
            if (it != stops.end()) it->second->store(true);
            std::thread([this, id, f, startNow] { unstage(id, f, startNow); }).detach();
            return;
            }
        }
        else if (isStaged(*t)) {   // (older builds) a video or MEGA file in its own folder: stop it for a moment, move what arrived, carry on
            t->description = f.description; t->site = f.site; t->favicon = f.favicon; t->category = f.category;
            t->forceStart = false;
            unstaging.insert(id);
            auto it = stops.find(id);
            if (it != stops.end()) it->second->store(true);
            std::thread([this, id, f, startNow] { unstage(id, f, startNow); }).detach();
            return;
        }
        else {
        t->fileName = f.fileName; t->dir = f.dir; t->category = f.category; t->description = f.description; t->site = f.site;
        t->favicon = f.favicon; t->pageUrl = f.pageUrl; t->cookies = f.cookies; t->userAgent = f.userAgent; t->headers = f.headers;
        t->hidden = false;
        t->forceStart = false;
        if (t->state == State::Complete) {   // it already finished while the window was open
            moveNow = true;
            from = t->partPath();
            t->tempDir.clear();
            to = t->path();
            if (fileExists(to)) { to = uniquePath(to); t->fileName = baseName(to); }
            t->finished = nowMs();
        } else if (!startNow) {
            t->pausedByUser = true;
            auto it = stops.find(id);
            if (it != stops.end()) it->second->store(true);
            else if (t->state == State::Queued || t->state == State::Error) t->state = State::Paused;
        } else if (t->state == State::Error || t->state == State::Paused) {   // the early start failed or stopped: Start Download starts it again
            t->state = State::Queued; t->error.clear(); t->pausedByUser = false;
            schedule();
        }
        }
    }
    if (moveNow) {
        makeDirs(dirName(to));
        MoveFileExW(W(from).c_str(), W(to).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
    }
    dirty = true;
    fire(EngineEvent::Added, id);
    if (moveNow || videoDone) fire(EngineEvent::Completed, id);
}

bool Engine::find(const std::string& id, Task& out) {
    std::lock_guard<std::mutex> g(mu);
    auto t = byId(id);
    if (!t) return false;
    out = *t;
    return true;
}

bool Engine::findUrl(const std::string& url, Task& out) {
    std::lock_guard<std::mutex> g(mu);
    for (auto& t : tasks) if (t->url == url && !t->hidden) { out = *t; return true; }
    return false;
}

int Engine::running() {
    std::lock_guard<std::mutex> g(mu);
    return static_cast<int>(stops.size());
}

double Engine::totalSpeed() {
    std::lock_guard<std::mutex> g(mu);
    double s = 0;
    for (auto& t : tasks) if (t->state == State::Running) s += t->speed;
    return s;
}

Engine::Power Engine::power() {
    Power p;
    SYSTEM_POWER_STATUS s;
    if (GetSystemPowerStatus(&s)) {
        p.hasBattery = s.BatteryFlag != 128 && s.BatteryFlag != 255;
        p.charging = s.ACLineStatus == 1 || !p.hasBattery;
        p.percent = s.BatteryLifePercent > 100 ? 100 : s.BatteryLifePercent;
    }
    return p;
}

std::string Engine::blockReason(const Task& t) {
    const Settings& s = settings();
    Power p = power();
    if ((t.chargingOnly || s.condCharging) && !p.charging) return "Waiting for the charger";
    int minB = t.minBattery > 0 ? t.minBattery : s.condMinBattery;
    if (minB > 0 && p.hasBattery && !p.charging && p.percent < minB) return "Battery below " + std::to_string(minB) + "%";
    if (limitReached) return "Download limit reached — waiting";
    return "";
}

namespace {
int todayYmd() { SYSTEMTIME n; GetLocalTime(&n); return n.wYear * 10000 + n.wMonth * 100 + n.wDay; }
int nowSec() { SYSTEMTIME n; GetLocalTime(&n); return n.wHour * 3600 + n.wMinute * 60 + n.wSecond; }
int weekday() { SYSTEMTIME n; GetLocalTime(&n); return n.wDayOfWeek; }   // Sunday = 0
}

// Downloads started by hand (Queued) run up to "downloads at the same time"; each running queue adds its own files,
// in queue order, up to its "files at the same time".
void Engine::schedule() {
    if (quitting) return;
    const Settings& s = settings();
    int active = 0, activeTorrents = 0;
    for (auto& kv : stops) { auto t = byId(kv.first); if (!t || t->viaQueue) continue; if (t->kind == "torrent") activeTorrents++; else active++; }
    std::lock_guard<std::mutex> qg(queues::lock());
    // queues first
    for (auto& q : queues::all()) {
        if (!q.running) continue;
        int busy = 0;
        for (auto& id : q.order) { auto t = byId(id); if (t && stops.count(id)) busy++; }
        for (auto& id : q.order) {
            if (busy >= q.simultaneous) break;
            auto t = byId(id);
            if (!t || stops.count(id) || t->state == State::Complete) continue;
            if (t->state == State::Error) {
                int allowed = q.retriesOn ? q.retries : 0;
                if (t->queueTries >= allowed || nowMs() < t->retryAt) continue;
            }
            if (t->state == State::Paused && t->pausedByUser) continue;   // paused by hand inside a running queue: leave it
            std::string why = blockReason(*t);
            if (!why.empty()) { t->state = State::Queued; t->waitReason = why; t->status = why; continue; }
            if (t->state == State::Error) t->queueTries++;
            t->error.clear(); t->waitReason.clear(); t->viaQueue = true;
            launch(t);
            busy++;
            hadWork = true;
        }
    }
    std::vector<std::shared_ptr<Task>> order(tasks.begin(), tasks.end());
    std::stable_sort(order.begin(), order.end(), [](const std::shared_ptr<Task>& a, const std::shared_ptr<Task>& b) {
        if (a->forceStart != b->forceStart) return a->forceStart;
        return a->priority > b->priority;
    });
    for (auto& t : order) {
        if (t->state != State::Queued || stops.count(t->id) || unstaging.count(t->id)) continue;
        std::string why = blockReason(*t);
        if (!why.empty()) { t->waitReason = why; t->status = why; continue; }
        bool torrent = t->kind == "torrent";
        if (!t->forceStart && (torrent ? activeTorrents >= std::max(1, s.activeTorrents) : active >= s.maxActive)) { t->waitReason.clear(); continue; }
        t->waitReason.clear();
        t->forceStart = false;
        t->viaQueue = false;
        launch(t);
        if (torrent) activeTorrents++; else active++;
        hadWork = true;
    }
}

// Every 20 s: queue start / stop times, periodic synchronization, download limits, battery / charger conditions,
// and what to do when a queue (or everything) is finished.
void Engine::retryFailed(bool now) {
    if (!settings().autoRetry) return;
    bool any = false;
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tasks) {
            if (t->state != State::Error || !t->retryable || t->pausedByUser || t->viaQueue || stops.count(t->id)) continue;
            if (t->autoTries >= std::max(1, settings().retries)) continue;
            if (!now && nowMs() < t->retryAt) continue;
            t->autoTries++;
            t->state = State::Queued; t->status = "Resuming…"; t->error.clear();
            any = true;
        }
        if (any) schedule();
    }
    if (any) { dirty = true; fire(EngineEvent::Changed, ""); }
}

void Engine::watchNetwork() {
    // the network came back (Wi-Fi reconnected, cable plugged in, PC woke up): resume failed downloads at once
    std::thread([this] {
        while (!quitting) {
            HANDLE h = nullptr;
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            DWORD r = NotifyAddrChange(&h, &ov);
            if (r == ERROR_IO_PENDING) {
                while (!quitting && WaitForSingleObject(ov.hEvent, 1000) == WAIT_TIMEOUT) {}
                if (!quitting) { Sleep(4000); retryFailed(true); }
            } else Sleep(20000);
            CancelIPChangeNotify(&ov);
            CloseHandle(ov.hEvent);
        }
    }).detach();
}

void Engine::tick() {
    retryFailed(false);
    int today = todayYmd(), sec = nowSec(), wd = weekday();
    std::vector<std::string> toStart, toStop, toSync;
    std::vector<Queue> finished;
    // download limits: bytes in the last N hours
    int64_t doneNow = 0;
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tasks) doneNow += std::max<int64_t>(0, t->done);
    }
    if (lastDoneSum >= 0 && doneNow > lastDoneSum) usage.push_back({nowMs(), doneNow - lastDoneSum});
    lastDoneSum = doneNow;
    {
        std::lock_guard<std::mutex> qg(queues::lock());
        const DownloadLimits& L = queues::limits();
        int64_t window = static_cast<int64_t>(std::max(1, L.everyHours)) * 3600 * 1000, sum = 0;
        while (!usage.empty() && nowMs() - usage.front().first > window) usage.pop_front();
        for (auto& u : usage) sum += u.second;
        bool was = limitReached;
        limitReached = L.on && sum >= static_cast<int64_t>(L.maxMB) * 1024 * 1024;
        if (limitReached && !was && L.warn && onNotice) onNotice("Download limit reached", "DDM downloaded " + std::to_string(L.maxMB) + " MB in the last " + std::to_string(L.everyHours) + " hours. Downloads continue when the limit allows.");
        for (auto& q : queues::all()) {
            if (q.startAtOn && sec >= q.startAt && q.lastStartDay != today) {
                bool day = q.daily ? q.days.size() == 7 && q.days[static_cast<size_t>(wd)] == '1' : q.onceDate == today;
                if (day && sec - q.startAt < 6 * 3600) { q.lastStartDay = today; if (!q.running) toStart.push_back(q.id); }
            }
            if (q.stopAtOn && sec >= q.stopAt && q.lastStopDay != today && q.running && sec - q.stopAt < 6 * 3600) { q.lastStopDay = today; toStop.push_back(q.id); }
            if (q.running && q.periodic && q.nextSync && nowMs() >= q.nextSync) { q.nextSync = 0; toSync.push_back(q.id); }
        }
    }
    for (auto& id : toStart) startQueue(id);
    for (auto& id : toStop) stopQueue(id);
    for (auto& id : toSync) syncQueue(id);
    {
        std::lock_guard<std::mutex> g(mu);
        // running downloads that may not run any more (charger unplugged, battery low, limit reached)
        for (auto& t : tasks) {
            if (!stops.count(t->id) || t->kind == "torrent") continue;
            std::string why = blockReason(*t);
            if (!why.empty()) { t->waitReason = why; restartAfterStop.insert(t->id); stops[t->id]->store(true); }
        }
        schedule();
        // a one-time queue with nothing left to do is finished
        std::lock_guard<std::mutex> qg(queues::lock());
        for (auto& q : queues::all()) {
            if (!q.running) continue;
            bool left = false;
            for (auto& id : q.order) {
                auto t = byId(id);
                if (!t) continue;
                if (stops.count(id) || t->state == State::Queued) { left = true; break; }
                if (t->state == State::Paused && !t->pausedByUser) { left = true; break; }
                if (t->state == State::Error && q.retriesOn && t->queueTries < q.retries) { left = true; break; }
            }
            if (left) continue;
            if (q.periodic) { if (!q.nextSync) q.nextSync = nowMs() + static_cast<int64_t>(q.syncEveryMin) * 60000; continue; }
            q.running = false;
            finished.push_back(q);
        }
        // nothing at all running any more: the general "when all downloads finish" action
        bool busy = !stops.empty() || std::any_of(tasks.begin(), tasks.end(), [](const std::shared_ptr<Task>& t) { return t->state == State::Queued && t->waitReason.empty(); });
        if (!busy && hadWork) {
            hadWork = false;
            std::string a = settings().queueDone;
            if (a != "nothing" && onQueueDone) { auto cb = onQueueDone; std::thread([cb, a] { cb(a); }).detach(); }
        }
    }
    if (!finished.empty()) queues::save();
    for (auto& q : finished) if (onQueueFinished) { auto cb = onQueueFinished; std::thread([cb, q] { cb(q); }).detach(); }
    dirty = true;
    fire(EngineEvent::Changed, "");
}

void Engine::startQueue(const std::string& qid) {
    bool periodic = false;
    {
        std::lock_guard<std::mutex> g(mu);
        std::lock_guard<std::mutex> qg(queues::lock());
        auto q = queues::find(qid);
        if (!q) return;
        q->running = true;
        periodic = q->periodic;
        for (auto& id : q->order) {
            auto t = byId(id);
            if (!t) continue;
            t->pausedByUser = false;
            t->queueTries = 0;
            if (t->state == State::Error) { t->state = State::Paused; t->error.clear(); }
        }
    }
    if (periodic) syncQueue(qid);
    {
        std::lock_guard<std::mutex> g(mu);
        schedule();
    }
    dirty = true; fire(EngineEvent::Changed, "");
}

void Engine::stopQueue(const std::string& qid) {
    {
        std::lock_guard<std::mutex> g(mu);
        std::lock_guard<std::mutex> qg(queues::lock());
        auto q = queues::find(qid);
        if (!q) return;
        q->running = false;
        q->nextSync = 0;
        for (auto& id : q->order) {
            auto t = byId(id);
            if (!t) continue;
            auto it = stops.find(id);
            if (it != stops.end() && t->viaQueue) it->second->store(true);
            else if (t->state == State::Queued && t->viaQueue) t->state = State::Paused;
        }
    }
    dirty = true; fire(EngineEvent::Changed, "");
}

bool Engine::queueRunning(const std::string& qid) {
    std::lock_guard<std::mutex> qg(queues::lock());
    auto q = queues::find(qid);
    return q && q->running;
}

// Periodic synchronization: every finished file of the queue is checked on the server; a changed file downloads again.
void Engine::syncQueue(const std::string& qid) {
    std::vector<Task> check;
    {
        std::lock_guard<std::mutex> g(mu);
        std::lock_guard<std::mutex> qg(queues::lock());
        auto q = queues::find(qid);
        if (!q) return;
        for (auto& id : q->order) { auto t = byId(id); if (t && t->state == State::Complete && t->kind == "http") check.push_back(*t); }
    }
    std::thread([this, check] {
        for (auto& t : check) {
            RequestInfo r;
            r.url = t.url; r.referer = t.pageUrl; r.cookies = t.cookies; r.userAgent = t.userAgent; r.headers = t.headers;
            ResponseInfo info;
            if (!probe(r, info)) continue;
            bool changed = (info.total >= 0 && info.total != t.total) || (!info.lastModified.empty() && !t.remoteStamp.empty() && info.lastModified != t.remoteStamp) ||
                           (!fileExists(t.path()));
            {
                std::lock_guard<std::mutex> g(mu);
                auto x = byId(t.id);
                if (!x) continue;
                if (x->remoteStamp.empty()) x->remoteStamp = info.lastModified;
                if (!changed) continue;
                x->remoteStamp = info.lastModified;
                x->segments.clear(); x->done = 0; x->total = -1; x->resumable = false; x->error.clear();
                x->state = State::Paused;   // the running queue picks it up
                logLine("sync: " + x->fileName + " changed on the server, downloading it again");
                schedule();
            }
            dirty = true;
            fire(EngineEvent::Changed, t.id);
        }
    }).detach();
}

void Engine::addToQueue(const std::string& id, const std::string& qid) {
    queues::addTask(qid, id);
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t) return;
        t->queue = qid;
        if (t->state == State::Queued && !stops.count(id)) t->state = State::Paused;
        schedule();
    }
    dirty = true; fire(EngineEvent::Changed, id);
}

void Engine::removeFromQueue(const std::string& id) {
    queues::removeTask(id);
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t) return;
        t->queue.clear();
        t->viaQueue = false;
    }
    dirty = true; fire(EngineEvent::Changed, id);
}

void Engine::setPriority(const std::string& id, int p) {
    { std::lock_guard<std::mutex> g(mu); auto t = byId(id); if (!t) return; t->priority = std::max(0, std::min(2, p)); schedule(); }
    dirty = true; fire(EngineEvent::Changed, id);
}

void Engine::forceStart(const std::string& id) {
    {
        std::lock_guard<std::mutex> g(mu);
        auto t = byId(id);
        if (!t || t->state == State::Running || t->state == State::Complete) return;
        t->priority = 2; t->forceStart = true; t->error.clear(); t->state = State::Queued; t->stopped = false; t->pausedByUser = false;
        schedule();
    }
    dirty = true; fire(EngineEvent::Changed, id);
}

void Engine::restart(const std::string& id) {
    std::shared_ptr<Task> t;
    {
        std::lock_guard<std::mutex> g(mu);
        t = byId(id);
        if (!t) return;
        auto it = stops.find(id);
        if (it != stops.end()) it->second->store(true);
    }
    for (int i = 0; i < 100; i++) { { std::lock_guard<std::mutex> g(mu); if (!stops.count(id)) break; } Sleep(50); }
    std::string part, done;
    { std::lock_guard<std::mutex> g(mu); part = t->partPath(); done = t->path(); }
    auto recycle = [](const std::string& p) {
        if (!fileExists(p) && !dirExists(p)) return;
        std::wstring from = W(p);
        from.push_back(0);
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        SHFileOperationW(&op);
    };
    recycle(part);
    if (t->state == State::Complete || t->kind == "torrent") recycle(done);
    {
        std::lock_guard<std::mutex> g(mu);
        t->segments.clear(); t->done = 0; t->total = -1; t->resumable = false; t->error.clear(); t->finished = 0; t->activeMs = 0;
        t->state = State::Queued;
        schedule();
    }
    dirty = true; fire(EngineEvent::Changed, id);
}

void Engine::launch(const std::shared_ptr<Task>& t) {
    auto stop = std::make_shared<std::atomic<bool>>(false);
    stops[t->id] = stop;
    t->state = State::Running;
    t->lastTry = nowMs();
    t->status = "Connecting…";
    workers++;
    std::thread([this, t, stop] { workerMain(t, stop); }).detach();
}

void Engine::workerMain(std::shared_ptr<Task> t, std::shared_ptr<std::atomic<bool>> stop) {
    fire(EngineEvent::Started, t->id);
    DownloadContext ctx;
    ctx.task = t.get();
    ctx.mu = &mu;
    ctx.stop = stop.get();
    int64_t lastFire = 0;
    ctx.changed = [&] {
        int64_t n = nowMs();
        if (n - lastFire > 400) { lastFire = n; dirty = true; fire(EngineEvent::Changed, t->id); }
    };
    DownloadResult r = DownloadResult::Failed;
    if (t->kind == "http") r = runHttpDownload(ctx);
    else if (t->kind == "video") r = runVideoDownload(ctx);
    else if (t->kind == "mega") r = runMegaDownload(ctx);
    else if (t->kind == "torrent") r = runTorrentDownload(ctx);
    else { std::lock_guard<std::mutex> g(mu); t->error = "This kind of download isn't supported."; }
    EngineEvent ev = EngineEvent::Changed;
    {
        std::lock_guard<std::mutex> g(mu);
        stops.erase(t->id);
        if (t->lastTry > 0) { int64_t ran = nowMs() - t->lastTry; if (ran > 0 && ran < 400LL * 24 * 3600 * 1000) t->activeMs += ran; }   // this run's time
        t->speed = 0;
        t->eta = -1;
        t->activeConnections = 0;
        if (r == DownloadResult::Done) {
            t->state = State::Complete; t->finished = nowMs(); t->status = "Complete"; t->segments.clear(); t->autoTries = 0;
            ev = t->hidden ? EngineEvent::Changed : EngineEvent::Completed;
        }
        else if (r == DownloadResult::Stopped) {
            bool again = restartAfterStop.erase(t->id) > 0;
            t->state = again ? State::Queued : State::Paused;
            t->status = again ? "Queued" : "Paused";
        }
        else {
            t->state = State::Error; t->status = "Error"; ev = EngineEvent::Failed; logLine("download " + t->id + " failed: " + t->error);
            // a dropped connection, a sleeping PC, a busy server: DDM resumes it by itself from the same byte
            std::string e = lower(t->error);
            t->retryable = !t->hidden && t->kind != "torrent";
            for (const char* never : {"expired", "moved", "access denied", "refused", "certificate", "changed on the server", "disk", "create the file", "damaged", "isn't supported", "keeps refusing", "password", "sign in", "rename", "not a file", "web page"})
                if (e.find(never) != std::string::npos) t->retryable = false;
            int k = std::min(t->autoTries, 6);
            t->retryAt = nowMs() + std::min<int64_t>(10 * 60 * 1000, 15000LL << k);
        }
        schedule();
    }
    // confirmed in the very moment it finished: bring the file from the side folder to its place
    std::string from, to;
    {
        std::lock_guard<std::mutex> g(mu);
        if (t->state == State::Complete && !t->hidden && !t->tempDir.empty() && fileExists(t->partPath())) {
            from = t->partPath();
            t->tempDir.clear();
            to = t->path();
            if (fileExists(to)) { to = uniquePath(to); t->fileName = baseName(to); }
        }
    }
    if (!from.empty()) { makeDirs(dirName(to)); MoveFileExW(W(from).c_str(), W(to).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING); ev = EngineEvent::Completed; }
    dirty = true;
    workers--;
    fire(ev, t->id);
}

} // namespace ddm

namespace ddm {
void Engine::unstage(const std::string& id, Task f, bool startNow) {
    for (int i = 0; i < 300; i++) { { std::lock_guard<std::mutex> g(mu); if (!stops.count(id)) break; } Sleep(50); }   // its worker stops (≤ 15 s)
    Task t;
    {
        std::lock_guard<std::mutex> g(mu);
        auto p = byId(id);
        if (!p) { unstaging.erase(id); return; }
        if (stops.count(id)) {   // busy with something it can't leave (getting a helper program): it stays where it is
            unstaging.erase(id);
            p->hidden = false;
            if (startNow) restartAfterStop.insert(id); else p->pausedByUser = true;
            dirty = true;
            std::string tid = id;
            fire(EngineEvent::Added, tid);
            logLine("unstage " + tid + ": still busy, kept in " + p->dir);
            return;
        }
        t = *p;
        if (p->state != State::Complete) { p->state = startNow ? State::Queued : State::Paused; p->status = startNow ? "Connecting…" : "Paused"; }
    }
    std::string stage = t.dir, newName = f.fileName.empty() ? t.fileName : f.fileName;
    bool own = isStaged(t);   // DDM's own side folder (everything in it is this download's) or the user's folder
    bool complete = t.state == State::Complete;
    makeDirs(f.dir);
    if (complete) {
        std::string actual = t.fileName, ext = extOf(actual);
        if (!ext.empty() && lower(ext) != lower(extOf(newName))) newName = stem(newName) + "." + ext;   // the site gave another format
        std::string target = joinPath(f.dir, newName);
        if (fileExists(target)) target = uniquePath(target);
        MoveFileExW(W(joinPath(stage, actual)).c_str(), W(target).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
        newName = baseName(target);
    } else {
        std::string oldName = t.fileName, oldStem = stem(oldName), newStem = stem(newName);
        for (auto& n : listDir(stage)) {
            if (!own && !isVideoPiece(lower(n), lower(oldStem), lower(oldName))) continue;   // only this video's pieces
            std::string to = n;
            if (startsWith(n, oldName)) to = newName + n.substr(oldName.size());
            else if (startsWith(n, oldStem + ".")) to = newStem + n.substr(oldStem.size());
            MoveFileExW(W(joinPath(stage, n)).c_str(), W(joinPath(f.dir, to)).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
        }
    }
    if (own) removeTree(stage);
    {
        std::lock_guard<std::mutex> g(mu);
        unstaging.erase(id);
        auto p = byId(id);
        if (!p) return;
        p->dir = f.dir; p->fileName = newName; p->hidden = false;
        if (complete) p->finished = nowMs();
        else if (startNow) { p->state = State::Queued; p->pausedByUser = false; p->error.clear(); schedule(); }
        else { p->state = State::Paused; p->pausedByUser = true; p->status = "Paused"; }
    }
    dirty = true;
    fire(EngineEvent::Added, id);
    if (complete) fire(EngineEvent::Completed, id);
}
} // namespace ddm
