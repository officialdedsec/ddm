// Command-line engine test: enginetest <url> <dir> [connections] [pauseAfterMs]
#include "../src/core/engine.hpp"
#include "../src/core/downloader.hpp"
#include "../src/core/settings.hpp"
#include "../src/core/media.hpp"
#include "../src/core/mega.hpp"
#include "../src/core/vault.hpp"
#include "../src/core/antivirus.hpp"
#include "../src/core/xray.hpp"
#include "../src/core/filetools.hpp"
#include "../src/core/sound.hpp"
#include "../src/core/inspect.hpp"
#include "../src/core/crypto.hpp"
#include "../src/core/integration.hpp"
#include "../src/core/torrent.hpp"
#include "../src/common/util.hpp"
#include <windows.h>
#include <cstdio>
using namespace ddm;
int main(int argc, char** argv) {
    if (argc < 3) { printf("usage\n"); return 2; }
    loadSettings();
    if (argc > 3) settings().connections = atoi(argv[3]);
    int pauseAfter = argc > 4 ? atoi(argv[4]) : 0;
    if (argc > 5) settings().speedLimitKBps = atoi(argv[5]);
    http::globalInit();
    char probeEnv[8] = "";
    if (GetEnvironmentVariableA("DDM_PROBE", probeEnv, sizeof probeEnv)) {
        if (std::string(probeEnv) == "names") {   // enginetest <file: title\tdescription\tuploader\tpage\tid per line> x
            std::string t; readFile(argv[1], t);
            for (auto& line : split(t, '\n')) {
                if (line.empty()) continue;
                auto f = split(line, '\t'); while (f.size() < 5) f.push_back("");
                printf("%s\n", niceVideoTitle(f[0], replaceAll(f[1], "\\n", "\n"), f[2], f[3], f[4]).c_str());
            }
            return 0;
        }
        if (std::string(probeEnv) == "mktor") {   // enginetest <file or folder> <out.torrent> [v1|hybrid|v2]
            bt::CreateRequest r;
            r.source = argv[1]; r.out = argv[2]; r.version = argc > 3 ? argv[3] : "v1";
            r.trackers = {"http://127.0.0.1:8850/announce", "", "udp://backup.example:80/announce"};
            r.webSeeds = {"http://127.0.0.1:8850/ws/"};
            r.comment = "made by DDM"; r.isPrivate = false;
            std::string magnet, err;
            int calls = 0;
            bool ok = bt::createTorrent(r, [&](int, int) { calls++; return true; }, magnet, err);
            printf("ok=%d progress-calls=%d magnet=%s err=%s\n", ok, calls, magnet.c_str(), err.c_str());
            return 0;
        }
        if (std::string(probeEnv) == "updsig") {   // enginetest <update.json> <update.json.sig>
            std::string j, sg;
            readFile(argv[1], j); readFile(argv[2], sg);
            crypto::Bytes b = crypto::unb64(trim(sg));
            printf("ddm=%d config=%d\n", crypto::verifyDdm(j.data(), j.size(), b), crypto::verifyConfig(j.data(), j.size(), b));
            return 0;
        }
        if (std::string(probeEnv) == "addons") {   // hand-loaded copies of DDM's add-on
            integration::watchHandLoadedAddons();
            Sleep(4000);
            for (auto& id : integration::handLoadedAddonIds()) printf("allowed %s\n", id.c_str());
            printf("done\n");
            return 0;
        }
        if (std::string(probeEnv) == "import") {   // DDM 3.x list → 4.0 on first start
            Engine::get().load();
            for (auto& t : Engine::get().snapshot()) printf("%s | %s | %s | %s | %s\n", t.kind.c_str(), stateName(t.state), t.fileName.c_str(), t.dir.c_str(), t.url.c_str());
            return 0;
        }
        if (std::string(probeEnv) == "inspect") {   // enginetest <mode: link|file|meta> <arg>…
            std::string mode = argv[1];
            for (int i = 2; i < argc; i++) {
                if (mode == "link") {
                    auto r = inspect::checkLink(argv[i], "", fileNameFromUrl(argv[i]));
                    printf("%s -> %s\n", argv[i], r.summary().c_str());
                    for (auto& f : r.findings) printf("    [%d] %s\n", (int)f.level, f.text.c_str());
                } else if (mode == "file") {
                    printf("%s -> '%s'\n", argv[i], inspect::checkLocalFile(argv[i]).c_str());
                } else if (mode == "meta") {
                    std::string err, sum;
                    std::string out = tools::stripMetadata(argv[i], nullptr, nullptr, err, &sum);
                    printf("%s -> '%s' err='%s' sum='%s'\n", argv[i], out.c_str(), err.c_str(), sum.c_str());
                }
            }
            return 0;
        }
        if (std::string(probeEnv) == "sound") {   // enginetest <wav> x
            std::string err;
            bool ok = sound::import(sound::Event::Complete, argv[1], err);
            printf("import=%d err=%s\n", ok, err.c_str());
            sound::discard();
            return 0;
        }
        if (std::string(probeEnv) == "tools") {   // enginetest <archive> <outdir>
            std::string err;
            int n = tools::extract(argv[1], argv[2], nullptr, nullptr, err);
            printf("extract=%d err=%s\n", n, err.c_str());
            bool z = tools::createZip({argv[2]}, std::string(argv[2]) + ".zip", nullptr, nullptr, err);
            printf("zip=%d size=%lld err=%s\n", z, (long long)fileSize(std::string(argv[2]) + ".zip"), err.c_str());
            auto d = tools::duplicates(argv[2], nullptr, nullptr);
            printf("dupgroups=%d\n", (int)d.size());
            return 0;
        }
        if (std::string(probeEnv) == "xray") {   // enginetest <url or file> <outdir> : lists; DDM_PICK=1,2 saves those
            std::string err;
            std::unique_ptr<xray::Source> src;
            std::string a = argv[1];
            if (a.rfind("http", 0) == 0) { RequestInfo r; r.url = a; src = xray::openHttp(r, err); } else src = xray::openFile(a, err);
            if (!src) { printf("open failed: %s\n", err.c_str()); return 1; }
            xray::Index idx;
            std::string name = fileNameFromUrl(a);
            if (a.rfind("http", 0) != 0) name = baseName(a);
            bool ok = xray::look(*src, name, idx, [](const std::string& st) { printf("  .. %s\n", st.c_str()); });
            printf("ok=%d format=%s label=%s entries=%d read=%lld bytes in %.2f s err=%s\n", ok, idx.format.c_str(), idx.label.c_str(), (int)idx.entries.size(), (long long)idx.bytesRead, idx.seconds, idx.error.c_str());
            for (auto& n : idx.notes) printf("  note: %s\n", n.c_str());
            for (auto& n : idx.warnings) printf("  warn: %s\n", n.c_str());
            for (size_t i = 0; i < idx.entries.size() && i < 40; i++) { auto& e = idx.entries[i]; printf("  [%d] %s%s size=%lld packed=%lld crc=%08x m=%d %s%s%s\n", (int)i, e.path.c_str(), e.dir ? "/" : "", (long long)e.size, (long long)e.packed, e.crc, e.method, e.why.empty() ? "" : ("why=" + e.why).c_str(), e.risk.empty() ? "" : " RISK=", e.risk.c_str()); }
            char pick[256] = "";
            if (GetEnvironmentVariableA("DDM_PICK", pick, sizeof pick)) {
                std::vector<size_t> which;
                for (auto& x : split(pick, ',')) if (!x.empty()) which.push_back(static_cast<size_t>(atoi(x.c_str())));
                xray::ExtractResult res;
                bool eok = xray::extract(*src, idx, which, argv[2], true, nullptr, res);
                printf("extract=%d saved=%d\n", eok, res.saved);
                for (auto& p : res.problems) printf("  problem: %s\n", p.c_str());
                for (auto& f : res.files) printf("  file: %s\n", f.c_str());
            }
            return 0;
        }
        if (std::string(probeEnv) == "vault") {   // enginetest <file> x
            std::string err;
            printf("setup=%d\n", vault::isSetUp());
            if (!vault::isSetUp()) printf("create=%d %s\n", vault::create("real-pass-1", err), err.c_str());
            printf("add locked=%d %s\n", vault::add(argv[1], false, err, "http://x"), err.c_str());
            printf("unlock wrong=%d %s\n", vault::unlock("nope", err), err.c_str());
            printf("unlock real=%d decoy=%d %s\n", vault::unlock("real-pass-1", err), vault::isDecoy(), err.c_str());
            auto items = vault::list();
            printf("items=%zu\n", items.size());
            for (auto& i : items) printf("  %s %s %lld\n", i.id.c_str(), i.name.c_str(), (long long)i.size);
            if (!items.empty()) printf("extract=%d %s\n", vault::extract(items[0].id, "Z:\\tmp\\vault_out.bin", err), err.c_str());
            printf("decoySet=%d setDecoy=%d %s\n", vault::hasDecoyPassword(), vault::setDecoy("decoy-pass", err), err.c_str());
            printf("hasDecoy=%d\n", vault::hasDecoyPassword());
            vault::lock();
            { bool u = vault::unlock("decoy-pass", err); bool d = vault::isDecoy(); size_t n = vault::list().size(); printf("unlock decoy=%d decoy=%d items=%zu\n", u, d, n); }
            printf("add into decoy=%d\n", vault::add(argv[1], false, err));
            printf("decoy items=%zu\n", vault::list().size());
            vault::lock();
            { bool u = vault::unlock("real-pass-1", err); bool d = vault::isDecoy(); size_t n = vault::list().size(); printf("unlock real again=%d decoy=%d items=%zu\n", u, d, n); }
            printf("change=%d %s\n", vault::changePassword("real-pass-1", "real-pass-2", err), err.c_str());
            vault::lock();
            { bool o = vault::unlock("real-pass-1", err); bool nw = vault::unlock("real-pass-2", err); size_t n = vault::list().size(); printf("old pw=%d new pw=%d items=%zu\n", o, nw, n); }
            auto it2 = vault::list();
            if (!it2.empty()) { bool r = vault::remove(it2[0].id, err); printf("remove=%d items=%zu\n", r, vault::list().size()); }
            vault::lock();
            for (int i = 0; i < 5; i++) vault::unlock("bad", err);
            printf("after 5 wrong: %s locked=%llds\n", err.c_str(), (long long)vault::lockedForSeconds());
            printf("av: active='%s' defender='%s' scanner='%s'\n", antivirus::activeName().c_str(), antivirus::defenderPath().c_str(), antivirus::scannerName().c_str());
            auto r = antivirus::scan(argv[1]);
            printf("scan kind=%d msg=%s\n", (int)r.kind, r.message.c_str());
            return 0;
        }
        if (std::string(probeEnv) == "queue") {   // queue + scheduler test: enginetest <url> <dir>
            Engine& e = Engine::get();
            e.load();
            bool finished = false;
            e.onQueueFinished = [&](const Queue& q) { printf("QUEUE FINISHED %s\n", q.name.c_str()); finished = true; };
            std::vector<std::string> ids;
            for (int i = 0; i < 2; i++) {
                Task t; t.url = argv[1]; t.dir = argv[2]; t.fileName = "q" + std::to_string(i) + ".bin";
                ids.push_back(e.add(t, false));
                e.addToQueue(ids.back(), "main");
            }
            SYSTEMTIME n; GetLocalTime(&n);
            int startAt = ((n.wHour * 3600 + n.wMinute * 60 + n.wSecond) / 60 + 1) * 60;
            {
                std::lock_guard<std::mutex> g(queues::lock());
                Queue* q = queues::find("main");
                q->startAtOn = true; q->startAt = startAt; q->daily = true; q->days = "1111111"; q->simultaneous = 1; q->lastStartDay = 0;
                q->order = ids;
            }
            printf("start at %02d:%02d\n", startAt / 3600, (startAt / 60) % 60);
            int64_t t0 = nowMs();
            while (!finished && nowMs() - t0 < 200000) {
                Sleep(1000);
                Task a, b; e.find(ids[0], a); e.find(ids[1], b);
                static std::string last;
                std::string now = std::string(stateName(a.state)) + "/" + stateName(b.state) + (e.queueRunning("main") ? " running" : " idle");
                if (now != last) { printf("%llds %s\n", (long long)(nowMs() - t0) / 1000, now.c_str()); last = now; }
            }
            e.shutdown();
            return 0;
        }
        if (std::string(probeEnv) == "mega") {
            std::string name, err; std::vector<MegaEntry> files;
            bool ok = megaFolder(argv[1], name, files, err);
            printf("folder ok=%d name=%s err=%s\n", ok, name.c_str(), err.c_str());
            for (auto& f : files) printf("  %s | %s | %lld | %s\n", f.path.c_str(), f.name.c_str(), (long long)f.size, f.url.c_str());
            return 0;
        }
        VideoRequest r; r.url = argv[1]; VideoInfo vi;
        bool ok = probeVideo(r, vi, [](const std::string& s) { printf("status: %s\n", s.c_str()); });
        printf("probe ok=%d title=%s dur=%d err=%s\n", ok, vi.title.c_str(), vi.duration, vi.error.c_str());
        for (auto& f : vi.formats) printf("  [%s] %s %s | %s | sel=%s ext=%s size=%lld direct=%d\n", f.group.c_str(), f.label.c_str(), f.tag.c_str(), f.spec.c_str(), f.selector.c_str(), f.ext.c_str(), (long long)f.size, f.direct);
        return 0;
    }
    Engine& e = Engine::get();
    e.load();
    Task t; t.url = argv[1]; t.dir = argv[2];
    char kind[64] = "", fmt[256] = "", name[256] = "", media[2048] = "", conv[16] = "";
    GetEnvironmentVariableA("DDM_KIND", kind, sizeof kind);
    GetEnvironmentVariableA("DDM_FORMAT", fmt, sizeof fmt);
    GetEnvironmentVariableA("DDM_NAME", name, sizeof name);
    GetEnvironmentVariableA("DDM_MEDIA", media, sizeof media);
    GetEnvironmentVariableA("DDM_CONVERT", conv, sizeof conv);
    char audio[2048] = "";
    GetEnvironmentVariableA("DDM_AUDIO", audio, sizeof audio);
    if (*kind) {
        t.kind = kind; t.videoFormat = fmt; t.fileName = *name ? name : (t.kind == "mega" ? "MEGA file" : "torrent"); t.mediaUrl = media; t.convertTo = conv; t.audioUrl = audio; t.mediaExt = "webm"; t.audioExt = "webm";
    } else {
        ResponseInfo ri; std::string err;
        RequestInfo rq; rq.url = t.url;
        bool ok = probe(rq, ri, &err);
        printf("probe ok=%d status=%ld total=%lld ranges=%d name=%s err=%s\n", ok, ri.status, (long long)ri.total, ri.ranges, ri.fileName.c_str(), err.c_str());
        t.fileName = ri.fileName.empty() ? fileNameFromUrl(t.url) : ri.fileName;
    }
    std::string id = e.add(t, true);
    int64_t start = nowMs(); bool paused = false;
    while (true) {
        Sleep(250);
        Task s; if (!e.find(id, s)) break;
        printf("\r%-9s %6.2f%% %10s conns=%2d parts=%zu %s     ", stateName(s.state), s.progress()*100, formatSpeed(s.speed).c_str(), s.activeConnections, s.segments.size(), s.status.c_str());
        fflush(stdout);
        if (pauseAfter && !paused && nowMs() - start > pauseAfter) { paused = true; e.pause(id); Sleep(800); printf("\nPAUSED at %lld\n", (long long)s.done); e.start(id); }
        if (s.state == State::Complete || s.state == State::Error || (s.state == State::Paused && !pauseAfter)) { printf("\nRESULT %s %s total=%lld file=%s ms=%lld\n", stateName(s.state), s.error.c_str(), (long long)s.total, s.path().c_str(), (long long)(nowMs()-start)); break; }
    }
    e.shutdown();
    return 0;
}
