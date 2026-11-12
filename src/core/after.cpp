#include "after.hpp"
#include "antivirus.hpp"
#include "crypto.hpp"
#include "motw.hpp"
#include "inspect.hpp"
#include "media.hpp"
#include "stats.hpp"
#include "filetools.hpp"
#include "engine.hpp"
#include "settings.hpp"
#include "vault.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ddm {
namespace after {

std::function<void(const std::string&, const std::string&, const std::string&)> onAlert;
std::function<void(const std::string&, const std::string&)> onNote;

namespace {
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<std::pair<std::string, bool>> g_jobs;   // id, forced (scan even when automatic scanning is off)
bool g_started = false;

std::string upperAlgo(std::string a);

void work(const std::string& id, bool forced) {
    Task t;
    if (!Engine::get().find(id, t) || t.state != State::Complete) return;
    std::string path = t.path();
    if (!forced) {
        // Statistics count finished downloads only: cancelled, failed or removed ones never add to them
        int64_t size = t.total > 0 ? t.total : fileSize(path);
        if (size <= 0 && t.kind == "torrent") size = t.done;
        stats::addBytes(size);
        stats::addFinished(t.site.empty() ? urlSite(t.pageUrl.empty() ? t.url : t.pageUrl) : t.site, size, t.finished > t.lastTry && t.lastTry ? t.finished - t.lastTry : 0);
        // like a browser: the file says it came from the internet (SmartScreen, Office Protected View…)
        if (settings().markOfTheWeb) motw::mark(path, t.kind == "torrent" ? "" : t.url, t.pageUrl);
        // a program dressed up as a picture / video / document (photo.jpg.exe, a ".pdf" that is really a program)
        if (settings().linkCheck && fileExists(path)) {
            std::string why = inspect::checkLocalFile(path);
            if (!why.empty()) {
                Engine::get().setScan(id, "threat", "Disguised program: " + why);
                logLine("disguised " + t.fileName + ": " + why);
                if (onAlert) onAlert("Threat found", t.fileName + "\n" + why + " Don't open it.", id);
                return;
            }
        }
        // the checksum the server published (or you gave): a damaged file is caught before you open it
        if (settings().autoVerifyHash && !t.checksum.empty() && fileExists(path)) {
            size_t colon = t.checksum.find(':');
            std::string algo = colon == std::string::npos ? crypto::checksumKind(t.checksum) : t.checksum.substr(0, colon);
            std::string want = lower(colon == std::string::npos ? t.checksum : t.checksum.substr(colon + 1));
            Engine::get().setVerify(id, "checking");
            std::string got = crypto::fileChecksum(path, algo);
            bool ok = !got.empty() && got == want;
            Engine::get().setVerify(id, got.empty() ? "" : ok ? "ok" : "bad");
            logLine("checksum " + t.fileName + ": " + (ok ? "matches" : "DOESN'T match") + " (" + algo + ")");
            if (!got.empty() && !ok && onAlert) onAlert("Damaged download", t.fileName + "\nIts " + upperAlgo(algo) + " checksum doesn't match: the file was damaged on the way. Redownload it.", id);
        }
    }
    // videos: the thumbnail picture and the subtitles next to the video (Download File Info / Options → General)
    if (!forced && t.kind == "video" && (!t.extras.empty() || !t.subs.empty()) && fileExists(path)) {
        bool th = t.extras.find("thumb") != std::string::npos, su = t.extras.find("subs") != std::string::npos;
        std::string e;
        int n = 0;
        if (!t.subs.empty()) {   // the subtitle languages chosen for this video
            std::string page = t.pageUrl.empty() ? t.url : t.pageUrl;
            n = saveSubtitles(path, unpackSubs(t.subs), page, t.cookies, t.userAgent, e);
            if (th) { std::string e2; int k = saveVideoExtras(page, path, true, false, "", t.cookies, t.userAgent, e2); if (k > 0) n += k; }
            su = n == 0;   // (nothing saved: say why)
        } else
            n = saveVideoExtras(t.pageUrl.empty() ? t.url : t.pageUrl, path, th, su, settings().subLangs, t.cookies, t.userAgent, e);
        logLine("video extras " + t.fileName + ": " + (n >= 0 ? std::to_string(n) + " files" : e));
        if (n == 0 && su && !e.empty() && onNote) onNote("Subtitles", t.fileName + "\n" + e);
    }
    if (forced || antivirus::wantsScan(t.fileName)) {
        Engine::get().setScan(id, "scanning", "Scanning with " + antivirus::scannerName() + "…");
        auto r = antivirus::scan(path, t.url, t.pageUrl);
        const char* st = r.kind == antivirus::Result::Clean ? "clean" : r.kind == antivirus::Result::Threat ? "threat" : r.kind == antivirus::Result::Removed ? "removed" : "error";
        Engine::get().setScan(id, st, r.message);
        logLine("scan " + t.fileName + ": " + r.message);
        if ((r.kind == antivirus::Result::Threat || r.kind == antivirus::Result::Removed) && onAlert) onAlert(r.kind == antivirus::Result::Removed ? "Threat removed" : "Threat found", t.fileName + "\n" + r.message, id);
        if (r.kind == antivirus::Result::Threat || r.kind == antivirus::Result::Removed) return;   // never into the vault
    }
    // Options → Downloads → extract archives after downloading: into a folder next to the archive
    if (!forced && settings().autoExtract && tools::canExtract(t.fileName) && fileExists(path) && t.verify != "bad") {
        std::string out = uniquePath(joinPath(dirName(path), stem(t.fileName)));
        std::string e;
        int n = tools::extract(path, out, nullptr, nullptr, e);
        logLine("extract " + t.fileName + ": " + (n >= 0 ? std::to_string(n) + " files" : e));
        if (n > 0 && settings().markOfTheWeb) motw::mark(out, t.url, t.pageUrl);   // what came out of a downloaded archive is marked too
        if (n < 0) RemoveDirectoryW(W(out).c_str());
    }
    // Options → Security → put finished downloads straight into the private vault
    if (!forced && settings().stealthDefault && vault::isSetUp() && fileExists(path) && t.kind != "torrent") {
        std::string e;
        if (vault::add(path, true, e, t.url)) Engine::get().remove(id, false);
        else logLine("vault: " + e);
    }
}

std::string upperAlgo(std::string a) { for (auto& c : a) c = static_cast<char>(toupper(static_cast<unsigned char>(c))); return a == "SHA256" ? "SHA-256" : a == "SHA512" ? "SHA-512" : a == "SHA384" ? "SHA-384" : a == "SHA1" ? "SHA-1" : a == "CRC32" ? "CRC-32" : a; }

void ensureWorker() {
    if (g_started) return;
    g_started = true;
    std::thread([] {
        while (true) {
            std::pair<std::string, bool> job;
            {
                std::unique_lock<std::mutex> l(g_mu);
                g_cv.wait(l, [] { return !g_jobs.empty(); });
                job = g_jobs.front();
                g_jobs.pop_front();
            }
            Sleep(400);   // let the file settle (renamed, closed) before scanners open it
            work(job.first, job.second);
        }
    }).detach();
}
} // namespace

void finished(const std::string& id) {
    std::lock_guard<std::mutex> g(g_mu);
    ensureWorker();
    g_jobs.push_back({id, false});
    g_cv.notify_one();
}

void scanNow(const std::string& id) {
    std::lock_guard<std::mutex> g(g_mu);
    ensureWorker();
    g_jobs.push_back({id, true});
    g_cv.notify_one();
}

} // namespace after
} // namespace ddm
