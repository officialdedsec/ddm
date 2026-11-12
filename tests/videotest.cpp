// Command-line test of DDM's own video engine:
//   videotest probe <url> [streams…]          lists the qualities
//   videotest get <url> <n> <dir> [stopMs]    downloads quality n (stopMs: stop after that long, to test resume)
#include "../src/core/media.hpp"
#include "../src/core/streams.hpp"
#include "../src/core/bgremove.hpp"
#include "../src/core/playlists.hpp"
#include "../src/core/settings.hpp"
#include "../src/core/http.hpp"
#include "../src/common/util.hpp"
#include <windows.h>
#include <cstdio>
#include <thread>
using namespace ddm;
int main(int argc, char** argv) {
    if (argc < 3) { printf("usage\n"); return 2; }
    loadSettings();
    http::globalInit();
    std::string cmd = argv[1];
    if (cmd == "list") {   // videotest list <url>
        playlists::List l;
        int64_t t0 = nowMs();
        bool ok = playlists::read(argv[2], "", l, [](const std::string& s) { printf("  status: %s\n", s.c_str()); });
        printf("list ok=%d in %lld ms kind=%s title='%s' author='%s' feed='%s' items=%zu err='%s' isList=%d\n", ok, (long long)(nowMs() - t0), l.kind.c_str(), l.title.c_str(), l.author.c_str(), l.feed.c_str(), l.items.size(), l.error.c_str(), playlists::isListUrl(argv[2]));
        for (size_t i = 0; i < l.items.size(); i++) if (i < 3 || i + 2 >= l.items.size()) printf("  %s | %s | %d s\n", l.items[i].url.c_str(), l.items[i].title.c_str(), l.items[i].duration);
        if (argc > 3) {   // + subscribe, then check for new ones
            playlists::subscribe(l, "720p", "C:\\subs");
            for (auto& x : playlists::subscriptions()) printf("  after subscribe: feed=%s seen0=%s n=%zu\n", x.feed.c_str(), x.seen.empty() ? "-" : x.seen[0].c_str(), x.seen.size());
            auto v = playlists::checkSubscriptions(true);
            printf("new after subscribing: %zu\n", v.size());
            for (auto& t : v) printf("  task %s | %s | label=%s conv=%s dir=%s\n", t.url.c_str(), t.fileName.c_str(), t.formatLabel.c_str(), t.convertTo.c_str(), t.dir.c_str());
        }
        return ok ? 0 : 1;
    }
    if (cmd == "bg") {   // videotest bg <picture>: the background remover
        std::string err;
        int64_t t0 = nowMs();
        std::string out = bgremove::removeBackground(argv[2], [](double f, const std::string& s) { static int last = -1; int p = static_cast<int>(f * 10); if (p != last) { last = p; printf("  %3.0f%% %s\n", f * 100, s.c_str()); } }, nullptr, err);
        printf("bg out='%s' in %lld ms err='%s'\n", out.c_str(), (long long)(nowMs() - t0), err.c_str());
        return out.empty();
    }
    VideoRequest vr;
    vr.url = argv[2];
    std::string top;
    for (int i = cmd == "probe" ? 3 : 6; i < argc; i++) {   // probe URL [what the browser saw…] | get URL idx dir stopMs [what the browser saw…]
        std::string a = argv[i];
        if (a.rfind("seen=", 0) == 0) {   // seen=kind,url[,size[,mime]]
            auto parts = split(a.substr(5), ',');
            SeenStream s;
            if (parts.size() > 1) { s.kind = parts[0]; s.url = parts[1]; }
            if (parts.size() > 2) s.size = atoll(parts[2].c_str());
            if (parts.size() > 3) s.mime = parts[3];
            vr.seen.push_back(s);
        } else if (a.rfind("text=", 0) == 0) {   // text=url,file : what the browser read
            auto parts = split(a.substr(5), ',');
            std::string t; FILE* f = parts.size() > 1 ? fopen(parts[1].c_str(), "rb") : nullptr;
            if (f) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) t.append(buf, n); fclose(f); }
            streams::remember(parts[0], t);
        } else if (a.rfind("h=", 0) == 0) vr.height = atoi(a.c_str() + 2);
        else if (a.rfind("direct=", 0) == 0) vr.direct = a.substr(7);   // the file the browser's player plays
        else if (a.rfind("hdr=", 0) == 0) {                             // hdr=Name|Value : sent by the browser for the last seen= stream
            auto parts = split(a.substr(4), '|');
            if (parts.size() == 2 && !vr.seen.empty()) vr.seen.back().headers[parts[0]] = parts[1];
        }
        else if (a.rfind("top=", 0) == 0) top = a.substr(4);            // the tab's address (the player sits in a frame)
        else if (a.rfind("title=", 0) == 0) vr.title = a.substr(6);     // the name the browser shows
        else if (a.rfind("dur=", 0) == 0) vr.duration = atoi(a.c_str() + 4);
        else vr.streams.push_back(a);
    }
    {   // what DDM does with a browser's request: the video's own page
        std::string page = videoPageFor(vr.url, top, vr.seen);
        if (page != vr.url) { printf("  page: %s\n", page.c_str()); vr.url = page; }
        vr.framed = !top.empty() && top != vr.url;
        vr.topUrl = top;
    }
    VideoInfo vi;
    int64_t t0 = nowMs();
    bool ok = probeVideo(vr, vi, [](const std::string& s) { printf("  status: %s\n", s.c_str()); });
    printf("probe ok=%d in %lld ms title='%s' thumb='%s' dur=%d site=%s err='%s'\n", ok, (long long)(nowMs() - t0), vi.title.c_str(), vi.thumbnail.c_str(), vi.duration, vi.extractor.c_str(), vi.error.c_str());
    for (size_t i = 0; i < vi.formats.size(); i++) {
        auto& f = vi.formats[i];
        printf("  [%zu] %s %s | %s | sel=%s size=%lld direct=%d v=%s a=%s stream=%s conv=%s\n", i, f.group.c_str(), f.label.c_str(), f.spec.c_str(), f.selector.c_str(), (long long)f.size, f.direct,
               f.videoUrl.substr(0, 60).c_str(), f.audioUrl.substr(0, 60).c_str(), f.streamUrl.substr(0, 60).c_str(), f.convert.c_str());
    }
    for (auto& t : vi.subs) printf("  sub %s | %s | %s%s | %s\n", t.lang.c_str(), t.name.c_str(), t.format.c_str(), t.automatic ? " auto" : "", t.url.substr(0, 70).c_str());
    {   // VT_SUBS=en,es (or all): save those subtitles as <dir>\subs.<lang>.srt, the way a finished video gets them
        char langs[64] = "", dir[300] = "";
        if (GetEnvironmentVariableA("VT_SUBS", langs, sizeof langs) && GetEnvironmentVariableA("VT_SUBDIR", dir, sizeof dir)) {
            std::string e;
            auto picked = unpackSubs(packSubs(subsForLangs(vi.subs, langs)));
            int n = saveSubtitles(joinPath(dir, "subs.mp4"), picked, vr.url, "", "", e);
            printf("subtitles saved=%d of %zu err='%s'\n", n, picked.size(), e.c_str());
        }
    }
    if (cmd != "get" || argc < 5) return ok ? 0 : 1;
    size_t n = static_cast<size_t>(atoi(argv[3]));
    if (n >= vi.formats.size()) { printf("no such quality\n"); return 1; }
    const VideoFormat& f = vi.formats[n];
    Task t;   // as the Download Video window makes it
    t.id = "vt1"; t.kind = "video"; t.url = vr.url; t.pageUrl = vr.url; t.dir = argv[4];
    t.fileName = sanitizeFileName(vi.title.empty() ? "video" : vi.title) + "." + f.ext;
    t.videoFormat = f.selector; t.convertTo = f.convert; t.formatLabel = f.label; t.notBefore = vi.notBefore; t.connections = 8;
    if (!f.streamUrl.empty()) t.url = f.streamUrl;
    for (auto& kv : f.headers) if (lower(kv.first) == "cookie") t.cookies += (t.cookies.empty() ? "" : "; ") + kv.second; else if (f.audioUrl.empty() && !(f.direct && f.convert.empty())) t.headers[kv.first] = kv.second;
    if (!f.audioUrl.empty()) { t.mediaUrl = f.videoUrl; t.audioUrl = f.audioUrl; t.mediaExt = f.videoExt; t.audioExt = f.audioExt; for (auto& kv : f.headers) if (lower(kv.first) != "cookie") t.headers[kv.first] = kv.second; if (f.size > 0) t.total = f.size; }
    else if (f.direct && f.convert.empty()) { t.mediaUrl = f.directUrl; for (auto& kv : f.headers) if (lower(kv.first) != "cookie") t.headers[kv.first] = kv.second; }
    printf("task: media=%s headers=%zu (format headers=%zu) direct=%d conv='%s'\n", t.mediaUrl.substr(0, 60).c_str(), t.headers.size(), f.headers.size(), f.direct ? 1 : 0, f.convert.c_str());
    int stopMs = argc > 5 ? atoi(argv[5]) : 0;
    for (int round = 0; round < 2; round++) {
        std::mutex mu;
        std::atomic<bool> stop{false};
        DownloadContext c;
        c.task = &t; c.mu = &mu; c.stop = &stop;
        int64_t last = 0, st = nowMs();
        c.changed = [&] {
            if (nowMs() - last < 700) return;
            last = nowMs();
            std::lock_guard<std::mutex> g(mu);
            printf("  %s %lld/%lld %s conns=%d\n", t.status.c_str(), (long long)t.done, (long long)t.total, formatKBps(t.speed).c_str(), t.activeConnections);
            fflush(stdout);
        };
        std::thread killer;
        if (stopMs > 0 && round == 0) killer = std::thread([&] { Sleep(stopMs); stop = true; });
        DownloadResult r = runVideoDownload(c);
        if (killer.joinable()) killer.join();
        printf("result=%d in %lld ms file=%s size=%lld err='%s'\n", (int)r, (long long)(nowMs() - st), t.path().c_str(), (long long)fileSize(t.path()), t.error.c_str());
        fflush(stdout);
        if (r != DownloadResult::Stopped) break;
        { char pz[16] = ""; if (GetEnvironmentVariableA("VT_PAUSE", pz, sizeof pz)) Sleep(atoi(pz)); }
        printf("-- resuming --\n");
    }
    return 0;
}
