#include "xray.hpp"
#include "downloader.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <curl/curl.h>
#include <miniz/miniz.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <thread>

extern "C" {
#include "../../third_party/lzma/LzmaDec.h"
#include "../../third_party/lzma/Lzma2Dec.h"
}

namespace ddm {
namespace xray {

namespace {

// ------------------------------------------------------------------ little helpers
uint16_t u16(const std::string& b, size_t o) { return o + 2 <= b.size() ? static_cast<uint16_t>(static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8)) : 0; }
uint32_t u32(const std::string& b, size_t o) { return o + 4 <= b.size() ? static_cast<uint32_t>(u16(b, o)) | (static_cast<uint32_t>(u16(b, o + 2)) << 16) : 0; }
uint64_t u64(const std::string& b, size_t o) { return o + 8 <= b.size() ? static_cast<uint64_t>(u32(b, o)) | (static_cast<uint64_t>(u32(b, o + 4)) << 32) : 0; }
uint8_t u8(const std::string& b, size_t o) { return o < b.size() ? static_cast<uint8_t>(b[o]) : 0; }

std::string utf16le(const char* p, size_t bytes) {
    std::wstring w;
    for (size_t i = 0; i + 1 < bytes; i += 2) { wchar_t c = static_cast<wchar_t>(static_cast<uint8_t>(p[i]) | (static_cast<uint8_t>(p[i + 1]) << 8)); if (!c) break; w.push_back(c); }
    return U(w);
}
std::string utf16be(const char* p, size_t bytes) {
    std::wstring w;
    for (size_t i = 0; i + 1 < bytes; i += 2) { wchar_t c = static_cast<wchar_t>((static_cast<uint8_t>(p[i]) << 8) | static_cast<uint8_t>(p[i + 1])); if (!c) break; w.push_back(c); }
    return U(w);
}
std::string oem(const std::string& s) {   // old archives: names in the DOS code page
    if (s.empty()) return s;
    int n = MultiByteToWideChar(CP_OEMCP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(0, n)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_OEMCP, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return U(w);
}
bool validUtf8(const std::string& s) { return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0 || s.empty(); }

int64_t dosTime(uint16_t date, uint16_t time) {
    if (!date) return 0;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(1980 + (date >> 9)); st.wMonth = static_cast<WORD>((date >> 5) & 15); st.wDay = static_cast<WORD>(date & 31);
    st.wHour = static_cast<WORD>(time >> 11); st.wMinute = static_cast<WORD>((time >> 5) & 63); st.wSecond = static_cast<WORD>((time & 31) * 2);
    FILETIME ft;
    if (st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1 || !SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}
int64_t fileTime(uint64_t ft) { return ft ? static_cast<int64_t>(ft / 10000000ULL) - 11644473600LL : 0; }

std::string normPath(std::string p) {
    p = replaceAll(p, "\\", "/");
    while (!p.empty() && p.front() == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

const std::set<std::string>& programExts() {
    static const std::set<std::string> s = {"exe", "scr", "com", "bat", "cmd", "pif", "msi", "msix", "appx", "vbs", "vbe", "js", "jse", "wsf", "wsh", "hta", "ps1", "lnk", "jar", "reg", "cpl", "dll", "sys", "inf", "iso", "img", "vhd", "vhdx", "chm"};
    return s;
}
const std::set<std::string>& mediaExts() {
    static const std::set<std::string> s = {"mp4", "mkv", "avi", "mov", "wmv", "webm", "mp3", "flac", "wav", "m4a", "aac", "ogg", "opus", "jpg", "jpeg", "png", "gif", "bmp", "webp", "heic", "pdf", "doc", "docx", "txt", "srt"};
    return s;
}

// Marks entries that look dangerous: tries to write outside the folder, hides a program behind a second extension,
// a program inside an archive that looks like photos / music / films.
void markRisks(Index& idx, const std::string& archiveName) {
    std::string an = lower(archiveName);
    bool mediaArchive = an.find("photo") != std::string::npos || an.find("movie") != std::string::npos || an.find("music") != std::string::npos ||
                        an.find("video") != std::string::npos || an.find("film") != std::string::npos || an.find("album") != std::string::npos || an.find("pics") != std::string::npos;
    int media = 0, programs = 0;
    for (auto& e : idx.entries) {
        if (e.dir) continue;
        std::string ext = lower(extOf(e.path));
        if (mediaExts().count(ext)) media++;
        if (programExts().count(ext)) programs++;
    }
    for (auto& e : idx.entries) {
        std::string p = e.path;
        if (p.find("../") != std::string::npos || p == ".." || startsWith(p, "/") || (p.size() > 1 && p[1] == ':') || endsWith(p, "/.."))
            { e.risk = "Tries to write outside its folder"; continue; }
        if (e.dir) continue;
        std::string name = lower(baseName(replaceAll(p, "/", "\\")));
        std::string ext = extOf(name);
        if (!programExts().count(ext)) continue;
        std::string inner = extOf(stem(name));
        if (!inner.empty() && mediaExts().count(inner)) { e.risk = "A program disguised as ." + inner; continue; }
        if (name.find("\xE2\x80\xAE") != std::string::npos) { e.risk = "Its name is reversed to hide what it is"; continue; }
        if ((mediaArchive || (media >= 3 && programs * 10 < media)) && ext != "dll" && ext != "inf" && ext != "sys")
            e.risk = "A program among " + std::string(mediaArchive ? "media files" : "pictures and documents");
    }
}

// ------------------------------------------------------------------ HTTP source
struct Easy {
    CURL* h = nullptr;
    curl_slist* hdr = nullptr;
    ~Easy() { if (hdr) curl_slist_free_all(hdr); if (h) curl_easy_cleanup(h); }
};

struct FetchState { std::string* out; size_t limit; std::string contentRange; };
size_t onBody(char* p, size_t s, size_t n, void* u) {
    auto* st = static_cast<FetchState*>(u);
    size_t len = s * n;
    if (st->out->size() + len > st->limit) return 0;   // the server sends more than asked (whole file): stop
    st->out->append(p, len);
    return len;
}
size_t onHeader(char* p, size_t s, size_t n, void* u) {
    auto* st = static_cast<FetchState*>(u);
    std::string line(p, s * n);
    if (lower(line).compare(0, 14, "content-range:") == 0) st->contentRange = trim(line.substr(14));
    return s * n;
}

class HttpSource : public Source {
public:
    RequestInfo req;
    std::mutex mu;                                   // cache + main handle
    std::map<int64_t, std::string> cache;           // 64 KB blocks
    std::unique_ptr<Easy> main;
    static const int64_t B = 64 * 1024;

    std::unique_ptr<Easy> handle() {
        auto e = std::make_unique<Easy>();
        e->h = curl_easy_init();
        if (!e->h) return e;
        http::setup(e->h, req);
        for (auto& kv : req.headers) {
            std::string k = lower(kv.first);
            if (k == "range" || k == "cookie" || k == "accept-encoding" || k == "user-agent" || k == "referer" || k == "host" || k == "content-length") continue;
            e->hdr = curl_slist_append(e->hdr, (kv.first + ": " + kv.second).c_str());
        }
        e->hdr = curl_slist_append(e->hdr, "Accept-Encoding: identity");
        curl_easy_setopt(e->h, CURLOPT_HTTPHEADER, e->hdr);
        curl_easy_setopt(e->h, CURLOPT_LOW_SPEED_TIME, 20L);
        return e;
    }

    // one range request; true when the server sent exactly that part of the same file
    bool fetch(Easy& e, int64_t from, int64_t to, std::string& out) { std::string err; bool ok = fetchE(e, from, to, out, err); if (!ok) setError(err); return ok; }
    void setError(const std::string& m) { std::lock_guard<std::mutex> g(errMu); error = m; }
    std::mutex errMu;
    bool fetchE(Easy& e, int64_t from, int64_t to, std::string& out, std::string& error) {
        for (int attempt = 0; attempt < 3; attempt++) {
            if (stopped()) { error = "Stopped."; return false; }
            out.clear();
            FetchState st{&out, static_cast<size_t>(to - from + 1), ""};
            curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, onBody);
            curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &st);
            curl_easy_setopt(e.h, CURLOPT_HEADERFUNCTION, onHeader);
            curl_easy_setopt(e.h, CURLOPT_HEADERDATA, &st);
            curl_easy_setopt(e.h, CURLOPT_RANGE, (std::to_string(from) + "-" + std::to_string(to)).c_str());
            CURLcode rc = curl_easy_perform(e.h);
            long status = 0;
            curl_easy_getinfo(e.h, CURLINFO_RESPONSE_CODE, &status);
            if (status == 200) { error = "The server stopped sending parts of the file, so X-Ray can't continue. Download the whole file instead."; return false; }
            if (status == 401 || status == 403 || status == 404 || status == 410) { error = "The link has expired (the server answered " + std::to_string(status) + "). Open the download page again."; return false; }
            if (rc == CURLE_OK && status == 206) {
                int64_t a = -1, total = -1;
                const std::string& cr = st.contentRange;
                size_t sp = cr.find(' '), dash = cr.find('-'), slash = cr.find('/');
                if (sp != std::string::npos && dash != std::string::npos) a = _atoi64(cr.substr(sp + 1, dash - sp - 1).c_str());
                if (slash != std::string::npos && cr.substr(slash + 1) != "*") total = _atoi64(cr.substr(slash + 1).c_str());
                if (a != from) { error = "The server sent the wrong part of the file."; return false; }
                if (total >= 0 && total != length) { error = "The file on the server changed while X-Ray was reading it. Try again."; return false; }
                if (static_cast<int64_t>(out.size()) != to - from + 1) { if (attempt == 2) { error = "The connection closed early."; return false; } continue; }
                fetched += static_cast<int64_t>(out.size());
                return true;
            }
            if (status >= 400 && status < 500) { error = "The server answered " + std::to_string(status) + "."; return false; }
            error = friendlyError(static_cast<long>(rc), status);
            Sleep(400 * (attempt + 1));
        }
        return false;
    }

    // blocks [b0, b1] into the cache (one request for the missing run)
    bool load(Easy& e, int64_t b0, int64_t b1) {
        int64_t last = (length - 1) / B;
        b1 = std::min(b1, last);
        while (b0 <= b1) {
            { std::lock_guard<std::mutex> g(mu); while (b0 <= b1 && cache.count(b0)) b0++; }
            if (b0 > b1) break;
            int64_t e1 = b0;
            { std::lock_guard<std::mutex> g(mu); while (e1 + 1 <= b1 && !cache.count(e1 + 1)) e1++; }
            std::string data;
            if (!fetch(e, b0 * B, std::min(length - 1, (e1 + 1) * B - 1), data)) return false;
            std::lock_guard<std::mutex> g(mu);
            if (cache.size() > 1024)   // 64 MB at most: forget what's far from here
                for (auto it = cache.begin(); it != cache.end();) { if (it->first < b0 - 64 || it->first > e1 + 64) it = cache.erase(it); else ++it; }
            for (int64_t b = b0; b <= e1; b++) cache[b] = data.substr(static_cast<size_t>((b - b0) * B), static_cast<size_t>(B));
            b0 = e1 + 1;
        }
        return true;
    }

    bool read(int64_t off, size_t n, std::string& out) override {
        out.clear();
        if (off < 0 || off >= length || n == 0) return true;
        if (off + static_cast<int64_t>(n) > length) n = static_cast<size_t>(length - off);
        int64_t b0 = off / B, b1 = (off + static_cast<int64_t>(n) - 1) / B;
        bool missing = false;
        { std::lock_guard<std::mutex> g(mu); for (int64_t b = b0; b <= b1 && !missing; b++) missing = !cache.count(b); }
        if (missing) {
            std::lock_guard<std::mutex> one(fetchMu);
            if (!main) main = handle();
            if (!main->h) { setError("X-Ray couldn't connect."); return false; }
            if (!load(*main, b0, b1 + 3)) return false;   // a little read-ahead: the next header is usually close
        }
        std::lock_guard<std::mutex> g(mu);
        for (int64_t b = b0; b <= b1; b++) {
            auto it = cache.find(b);
            if (it == cache.end()) { error = "X-Ray couldn't read that part of the file."; return false; }
            size_t from = b == b0 ? static_cast<size_t>(off - b * B) : 0;
            size_t to = b == b1 ? static_cast<size_t>(off + static_cast<int64_t>(n) - b * B) : static_cast<size_t>(B);
            if (from < it->second.size()) out.append(it->second, from, std::min(to, it->second.size()) - from);
        }
        return true;
    }

    void prefetch(const std::vector<std::pair<int64_t, int64_t>>& ranges) override {
        // merge nearby ranges, then fetch up to 8 at a time on separate connections
        std::vector<std::pair<int64_t, int64_t>> blocks;
        for (auto& r : ranges) { if (r.first < 0 || r.second <= 0 || r.first >= length) continue; blocks.push_back({r.first / B, (std::min(length, r.first + r.second) - 1) / B}); }
        std::sort(blocks.begin(), blocks.end());
        std::vector<std::pair<int64_t, int64_t>> runs;
        for (auto& b : blocks) { if (!runs.empty() && b.first <= runs.back().second + 2) runs.back().second = std::max(runs.back().second, b.second); else runs.push_back(b); }
        { std::lock_guard<std::mutex> g(mu); runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const std::pair<int64_t, int64_t>& r) { for (int64_t b = r.first; b <= r.second; b++) if (!cache.count(b)) return false; return true; }), runs.end()); }
        if (runs.empty()) return;
        // big runs in 512 KB pieces, so they arrive in parallel too
        std::vector<std::pair<int64_t, int64_t>> jobs;
        for (auto& r : runs) for (int64_t b = r.first; b <= r.second; b += 8) jobs.push_back({b, std::min(r.second, b + 7)});
        std::atomic<size_t> next{0};
        int n = static_cast<int>(std::min<size_t>(8, jobs.size()));
        std::vector<std::thread> th;
        for (int i = 0; i < n; i++) th.emplace_back([&] {
            auto e = take();
            if (!e || !e->h) return;
            for (size_t k; (k = next++) < jobs.size();) { if (stopped() || !load(*e, jobs[k].first, jobs[k].second)) break; }
            give(std::move(e));
        });
        for (auto& t : th) t.join();
    }

    bool copyTo(int64_t off, int64_t n, void* file, int64_t fileOff, std::function<void(int64_t)> progress) override {
        // big pieces: several connections at once, like a normal download
        const int64_t chunk = 2 * 1024 * 1024;
        int64_t parts = (n + chunk - 1) / chunk;
        std::atomic<int64_t> next{0};
        std::atomic<bool> bad{false};
        std::mutex wmu;
        std::string err;
        int threads = static_cast<int>(std::min<int64_t>(8, std::max<int64_t>(1, parts)));
        std::vector<std::thread> th;
        for (int i = 0; i < threads; i++) th.emplace_back([&] {
            auto e = handle();
            if (!e->h) { bad = true; return; }
            for (int64_t k; !bad && (k = next++) < parts;) {
                int64_t a = off + k * chunk, len = std::min(chunk, n - k * chunk);
                std::string data, ferr;
                if (!fetchE(*e, a, a + len - 1, data, ferr)) { std::lock_guard<std::mutex> g(wmu); if (err.empty()) err = ferr; bad = true; return; }
                std::lock_guard<std::mutex> g(wmu);
                LARGE_INTEGER p; p.QuadPart = fileOff + k * chunk;
                DWORD wrote = 0;
                if (!SetFilePointerEx(file, p, nullptr, FILE_BEGIN) || !WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) || wrote != data.size()) { err = "Couldn't write to the disk (is it full?)."; bad = true; return; }
                if (progress) progress(static_cast<int64_t>(data.size()));
            }
        });
        for (auto& t : th) t.join();
        if (bad) setError(err.empty() ? "X-Ray couldn't download that part." : err);
        return !bad;
    }

private:
    std::mutex fetchMu;
    // connections kept open between batches (no new handshake each time)
    std::mutex poolMu;
    std::vector<std::unique_ptr<Easy>> pool;
    std::unique_ptr<Easy> take() { { std::lock_guard<std::mutex> g(poolMu); if (!pool.empty()) { auto e = std::move(pool.back()); pool.pop_back(); return e; } } return handle(); }
    void give(std::unique_ptr<Easy> e) { std::lock_guard<std::mutex> g(poolMu); if (pool.size() < 8) pool.push_back(std::move(e)); }
};

class FileSource : public Source {
public:
    HANDLE f = INVALID_HANDLE_VALUE;
    std::mutex mu;
    ~FileSource() override { if (f != INVALID_HANDLE_VALUE) CloseHandle(f); }
    bool read(int64_t off, size_t n, std::string& out) override {
        out.clear();
        if (off < 0 || off >= length) return true;
        if (off + static_cast<int64_t>(n) > length) n = static_cast<size_t>(length - off);
        out.resize(n);
        std::lock_guard<std::mutex> g(mu);
        LARGE_INTEGER p; p.QuadPart = off;
        DWORD got = 0;
        if (!SetFilePointerEx(f, p, nullptr, FILE_BEGIN) || !ReadFile(f, &out[0], static_cast<DWORD>(n), &got, nullptr)) { error = "Couldn't read the file."; return false; }
        out.resize(got);
        fetched += got;
        return true;
    }
};

} // namespace

bool Source::copyTo(int64_t off, int64_t n, void* file, int64_t fileOff, std::function<void(int64_t)> progress) {
    const int64_t step = 1024 * 1024;
    for (int64_t done = 0; done < n;) {
        if (stopped()) { error = "Stopped."; return false; }
        std::string data;
        if (!read(off + done, static_cast<size_t>(std::min(step, n - done)), data) || data.empty()) { if (error.empty()) error = "The file ends too early."; return false; }
        LARGE_INTEGER p; p.QuadPart = fileOff + done;
        DWORD wrote = 0;
        if (!SetFilePointerEx(file, p, nullptr, FILE_BEGIN) || !WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) || wrote != data.size()) { error = "Couldn't write to the disk (is it full?)."; return false; }
        done += static_cast<int64_t>(data.size());
        if (progress) progress(static_cast<int64_t>(data.size()));
    }
    return true;
}

std::unique_ptr<Source> openHttp(const RequestInfo& r, std::string& error, std::atomic<bool>* stop) {
    ResponseInfo info;
    std::string perr;
    if (!probe(r, info, &perr)) { error = perr.empty() ? "Couldn't reach the server." : perr; return nullptr; }
    if (info.status >= 400) { error = "The server answered " + std::to_string(info.status) + " (the link may have expired)."; return nullptr; }
    if (lower(info.mime).compare(0, 9, "text/html") == 0) { error = "This link opens a web page, not a file."; return nullptr; }
    if (!info.ranges || info.total <= 0) { error = "This server doesn't allow reading parts of the file, so X-Ray can't look inside it. You can still download it normally."; return nullptr; }
    auto s = std::make_unique<HttpSource>();
    s->req = r;
    if (!info.finalUrl.empty()) s->req.url = info.finalUrl;
    s->length = info.total;
    s->name = info.fileName;
    s->stop = stop;
    return s;
}

std::unique_ptr<Source> openFile(const std::string& path, std::string& error) {
    auto s = std::make_unique<FileSource>();
    s->f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (s->f == INVALID_HANDLE_VALUE) { error = "Couldn't open " + baseName(path) + "."; return nullptr; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(s->f, &sz);
    s->length = sz.QuadPart;
    s->name = baseName(path);
    return s;
}

namespace {

// ------------------------------------------------------------------ ZIP
bool lookZip(Source& s, Index& idx) {
    int64_t tailLen = std::min<int64_t>(s.length, 66 * 1024 + 22);
    std::string tail;
    if (!s.read(s.length - tailLen, static_cast<size_t>(tailLen), tail)) { idx.error = s.error; return false; }
    size_t eocd = std::string::npos;
    for (size_t i = tail.size() >= 22 ? tail.size() - 22 : 0;; i--) { if (u32(tail, i) == 0x06054b50) { eocd = i; break; } if (i == 0) break; }
    if (eocd == std::string::npos) { idx.error = "This isn't a complete ZIP file (its table of contents is missing)."; return false; }
    int64_t cdSize = u32(tail, eocd + 12), cdOff = u32(tail, eocd + 16), count = u16(tail, eocd + 10);
    bool z64 = false;
    if (eocd >= 20 && u32(tail, eocd - 20) == 0x07064b50) {   // ZIP64
        int64_t z64off = static_cast<int64_t>(u64(tail, eocd - 20 + 8));
        std::string r;
        if (!s.read(z64off, 56, r) || u32(r, 0) != 0x06064b50) { idx.error = "The ZIP64 table of contents is damaged."; return false; }
        count = static_cast<int64_t>(u64(r, 32)); cdSize = static_cast<int64_t>(u64(r, 40)); cdOff = static_cast<int64_t>(u64(r, 48));
        z64 = true;
    }
    // self-extracting archives and files with something in front: the offsets are relative to where the ZIP starts
    int64_t eocdPos = s.length - tailLen + static_cast<int64_t>(eocd);
    int64_t shift = eocdPos - (cdOff + cdSize) - (z64 ? 76 : 0);
    if (shift < 0) shift = 0;
    if (cdSize <= 0 || cdSize > 128LL * 1024 * 1024 || cdOff + shift + cdSize > s.length) { idx.error = "The ZIP's table of contents is damaged."; return false; }
    std::string cd;
    if (!s.read(cdOff + shift, static_cast<size_t>(cdSize), cd)) { idx.error = s.error; return false; }
    size_t p = 0;
    std::vector<int64_t> starts;
    for (int64_t i = 0; i < count && p + 46 <= cd.size(); i++) {
        if (u32(cd, p) != 0x02014b50) break;
        uint16_t flags = u16(cd, p + 8), method = u16(cd, p + 10);
        Entry e;
        e.method = method;
        e.mtime = dosTime(u16(cd, p + 14), u16(cd, p + 12));
        e.crc = u32(cd, p + 16); e.hasCrc = true;
        int64_t csize = u32(cd, p + 20), usize = u32(cd, p + 24), off = u32(cd, p + 42);
        size_t nlen = u16(cd, p + 28), xlen = u16(cd, p + 30), clen = u16(cd, p + 32);
        if (p + 46 + nlen + xlen + clen > cd.size()) break;
        std::string name = cd.substr(p + 46, nlen);
        std::string extra = cd.substr(p + 46 + nlen, xlen);
        for (size_t x = 0; x + 4 <= extra.size();) {   // ZIP64 sizes, Unicode path
            uint16_t id = u16(extra, x), sz = u16(extra, x + 2);
            size_t q = x + 4;
            if (id == 0x0001) {
                if (usize == 0xFFFFFFFF && q + 8 <= x + 4 + sz) { usize = static_cast<int64_t>(u64(extra, q)); q += 8; }
                if (csize == 0xFFFFFFFF && q + 8 <= x + 4 + sz) { csize = static_cast<int64_t>(u64(extra, q)); q += 8; }
                if (off == 0xFFFFFFFF && q + 8 <= x + 4 + sz) { off = static_cast<int64_t>(u64(extra, q)); }
            } else if (id == 0x7075 && sz > 5) {
                std::string un = extra.substr(x + 9, sz - 5);
                if (validUtf8(un)) name = un;
            }
            x += 4 + sz;
        }
        if (!(flags & 0x800) && !validUtf8(name)) name = oem(name);
        e.path = normPath(name);
        e.dir = !name.empty() && (name.back() == '/' || name.back() == '\\');
        e.size = usize; e.packed = csize; e.offset = off + shift;
        e.encrypted = (flags & 1) != 0;
        uint32_t ext = u32(cd, p + 38);
        if (u8(cd, p + 5) == 3 && ((ext >> 16) & 0xF000) == 0xA000) e.why = "a link, not a file";
        else if (e.encrypted) e.why = "password-protected";
        else if (method != 0 && method != 8) e.why = method == 9 ? "Deflate64 compression" : method == 12 ? "BZip2 compression" : method == 14 ? "LZMA compression" : method == 93 ? "Zstandard compression" : "unusual compression";
        if (!e.path.empty()) idx.entries.push_back(e);
        p += 46 + nlen + xlen + clen;
    }
    idx.format = z64 ? "ZIP archive (ZIP64)" : "ZIP archive";
    return true;
}

// ------------------------------------------------------------------ ISO 9660 / Joliet / UDF
const int64_t SEC = 2048;

int64_t isoDate7(const std::string& b, size_t o) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(1900 + u8(b, o)); st.wMonth = u8(b, o + 1); st.wDay = u8(b, o + 2); st.wHour = u8(b, o + 3); st.wMinute = u8(b, o + 4); st.wSecond = u8(b, o + 5);
    FILETIME ft;
    if (st.wMonth < 1 || st.wMonth > 12 || !SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    int64_t t = static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
    return t - static_cast<int8_t>(u8(b, o + 6)) * 15 * 60;
}

// Every folder's place on the disc is listed in one small table: read them all at once (in parallel) up front.
void prefetchFolders(Source& s, const std::string& vd) {
    uint32_t size = u32(vd, 132), loc = u32(vd, 140);
    if (size == 0 || size > 4 * 1024 * 1024 || loc == 0) return;
    std::string pt;
    if (!s.read(static_cast<int64_t>(loc) * SEC, size, pt)) return;
    std::vector<std::pair<int64_t, int64_t>> dirs;
    for (size_t p = 0; p + 8 <= pt.size();) {
        uint8_t len = u8(pt, p);
        if (len == 0) break;
        dirs.push_back({static_cast<int64_t>(u32(pt, p + 2)) * SEC, SEC});
        p += 8 + len + (len & 1);
    }
    s.prefetch(dirs);
}

bool lookIso9660(Source& s, Index& idx, const std::string& pvdSector, bool joliet, std::function<void(const std::string&)> status) {
    prefetchFolders(s, pvdSector);
    struct Dir { std::string path; int64_t lba, size; int depth; };
    std::vector<Dir> level = {{"", static_cast<int64_t>(u32(pvdSector, 156 + 2)), static_cast<int64_t>(u32(pvdSector, 156 + 10)), 0}};
    std::set<int64_t> seen;
    std::map<std::string, size_t> multi;   // multi-extent files (over 4 GB)
    int dirs = 0;
    while (!level.empty()) {
        std::vector<std::pair<int64_t, int64_t>> pre;
        for (auto& d : level) pre.push_back({d.lba * SEC, d.size});
        s.prefetch(pre);
        std::vector<Dir> next;
        for (auto& d : level) {
            if (s.stopped()) { idx.error = "Stopped."; return false; }
            if (!seen.insert(d.lba).second || d.size <= 0 || d.size > 64 * 1024 * 1024 || d.depth > 64) continue;
            std::string b;
            if (!s.read(d.lba * SEC, static_cast<size_t>(d.size), b)) { idx.error = s.error; return false; }
            if (status && ++dirs % 20 == 0) status("Reading folders… " + std::to_string(dirs));
            for (size_t p = 0; p < b.size();) {
                uint8_t len = u8(b, p);
                if (len == 0) { p = (p / SEC + 1) * SEC; continue; }   // records never cross a sector
                if (p + len > b.size() || len < 34) break;
                uint8_t nlen = u8(b, p + 32), flags = u8(b, p + 25);
                std::string raw = b.substr(p + 33, nlen);
                p += len;
                if (nlen == 1 && (raw[0] == 0 || raw[0] == 1)) continue;   // . and ..
                std::string name = joliet ? utf16be(raw.data(), raw.size()) : raw;
                size_t semi = name.rfind(';');
                if (semi != std::string::npos) name = name.substr(0, semi);
                if (!joliet && endsWith(name, ".")) name.pop_back();
                std::string full = d.path.empty() ? name : d.path + "/" + name;
                int64_t lba = u32(b, p - len + 2), size = u32(b, p - len + 10);
                if (flags & 2) {
                    Entry e; e.path = full; e.dir = true; e.mtime = isoDate7(b, p - len + 18);
                    idx.entries.push_back(e);
                    next.push_back({full, lba, size, d.depth + 1});
                } else {
                    auto it = multi.find(full);
                    if (it != multi.end()) { Entry& m = idx.entries[it->second]; m.size += size; m.extents.push_back({lba * SEC, size}); }
                    else {
                        Entry e; e.path = full; e.size = size; e.packed = size; e.mtime = isoDate7(b, p - len + 18);
                        e.extents.push_back({lba * SEC, size});
                        idx.entries.push_back(e);
                        if (flags & 0x80) multi[full] = idx.entries.size() - 1;
                    }
                }
            }
        }
        level.swap(next);
    }
    return true;
}

struct LongAd { uint32_t len = 0, lbn = 0; uint16_t part = 0; };
LongAd longAd(const std::string& b, size_t o) { LongAd a; a.len = u32(b, o) & 0x3FFFFFFF; a.lbn = u32(b, o + 4); a.part = u16(b, o + 8); return a; }

int64_t udfTime(const std::string& b, size_t o) {
    SYSTEMTIME st{};
    st.wYear = u16(b, o + 2); st.wMonth = u8(b, o + 4); st.wDay = u8(b, o + 5); st.wHour = u8(b, o + 6); st.wMinute = u8(b, o + 7); st.wSecond = u8(b, o + 8);
    FILETIME ft;
    if (st.wMonth < 1 || st.wMonth > 12 || !SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::string udfName(const std::string& b, size_t o, size_t len) {
    if (o >= b.size()) return "";
    len = std::min(len, b.size() - o);   // (a damaged entry can't make it read past the data)
    if (len == 0) return "";
    uint8_t comp = u8(b, o);
    if (comp == 16) return utf16be(b.data() + o + 1, len - 1);
    std::wstring w;
    for (size_t i = 1; i < len; i++) w.push_back(static_cast<wchar_t>(u8(b, o + i)));   // 8-bit: Latin-1
    return U(w);
}

struct Udf {
    Source& s;
    int64_t partStart = 0;      // bytes
    uint32_t block = 2048;
    // metadata partition (UDF 2.5, Blu-ray): its file entry's extents map the metadata blocks
    bool hasMeta = false; uint16_t metaRef = 0;
    std::vector<std::pair<int64_t, int64_t>> metaExt;   // (byte offset, length)
    explicit Udf(Source& src) : s(src) {}
    int64_t pos(uint16_t part, uint32_t lbn) {
        if (hasMeta && part == metaRef) {
            int64_t want = static_cast<int64_t>(lbn) * block;
            for (auto& e : metaExt) { if (want < e.second) return e.first + want; want -= e.second; }
            return -1;
        }
        return partStart + static_cast<int64_t>(lbn) * block;
    }
    struct Node { bool dir = false; int64_t size = 0, mtime = 0; std::vector<std::pair<int64_t, int64_t>> ext; std::string inl; bool ok = false; };
    Node entry(int64_t at, uint16_t part) {
        Node n;
        std::string b;
        if (at < 0 || !s.read(at, block, b)) return n;
        uint16_t tag = u16(b, 0);
        if (tag != 261 && tag != 266) return n;
        bool ext = tag == 266;
        uint8_t ftype = u8(b, 16 + 11);
        n.dir = ftype == 4;
        n.size = static_cast<int64_t>(u64(b, 56));
        n.mtime = udfTime(b, ext ? 92 : 84);
        uint32_t lea = u32(b, ext ? 208 : 168), lad = u32(b, ext ? 212 : 172);
        size_t ad = (ext ? 216 : 176) + lea;
        int adType = u16(b, 16 + 18) & 7;
        if (ad + lad > b.size()) return n;
        if (adType == 3) { n.inl = b.substr(ad, lad); n.ok = true; return n; }
        for (size_t p = ad; p + (adType == 0 ? 8 : 16) <= ad + lad;) {
            uint32_t raw = u32(b, p), len = raw & 0x3FFFFFFF, kind = raw >> 30;
            if (len == 0) break;
            uint32_t lbn = u32(b, p + 4);
            uint16_t pr = adType == 1 ? u16(b, p + 8) : part;
            if (kind == 3) break;   // continuation of the list: rare on discs
            n.ext.push_back({kind == 0 ? pos(pr, lbn) : -1, len});
            p += adType == 0 ? 8 : 16;
        }
        n.ok = true;
        return n;
    }
    bool readData(const Node& n, std::string& out, int64_t cap) {
        if (!n.inl.empty() || n.ext.empty()) { out = n.inl; return true; }
        out.clear();
        for (auto& e : n.ext) {
            if (static_cast<int64_t>(out.size()) >= cap) break;
            std::string piece;
            size_t want = static_cast<size_t>(std::max<int64_t>(0, std::min<int64_t>(e.second, cap - static_cast<int64_t>(out.size()))));   // never more than the cap
            if (e.first < 0) piece.assign(want, '\0');
            else if (!s.read(e.first, want, piece)) return false;
            out += piece;
        }
        if (static_cast<int64_t>(out.size()) > n.size) out.resize(static_cast<size_t>(n.size));
        return true;
    }
};

bool lookUdf(Source& s, Index& idx, std::function<void(const std::string&)> status) {
    Udf u(s);
    std::string avdp;
    if (!s.read(256 * SEC, SEC, avdp) || u16(avdp, 0) != 2) return false;
    uint32_t vdsLen = u32(avdp, 16), vdsLoc = u32(avdp, 20);
    std::string vds;
    if (!s.read(static_cast<int64_t>(vdsLoc) * SEC, std::min<uint32_t>(vdsLen, 64 * SEC), vds)) return false;
    LongAd fsd;
    std::string label;
    uint16_t partNum = 0;
    bool havePart = false, haveLv = false;
    std::string maps;
    uint32_t nMaps = 0;
    for (size_t p = 0; p + SEC <= vds.size(); p += SEC) {
        uint16_t tag = u16(vds, p);
        if (tag == 5 && !havePart) { partNum = u16(vds, p + 22); u.partStart = static_cast<int64_t>(u32(vds, p + 188)) * SEC; havePart = true; }
        else if (tag == 6 && !haveLv) {
            u.block = u32(vds, p + 212);
            label = udfName(vds, p + 84, std::min<size_t>(u8(vds, p + 84 + 127), 127));
            fsd = longAd(vds, p + 248);
            uint32_t mtl = u32(vds, p + 264); nMaps = u32(vds, p + 268);
            maps = vds.substr(p + 440, std::min<size_t>(mtl, SEC - 440));
            haveLv = true;
        } else if (tag == 8) break;
    }
    if (!havePart || !haveLv || u.block == 0 || u.block > 65536) return false;
    (void)partNum;
    // a metadata partition (UDF 2.50+): find its file
    for (uint32_t i = 0, mp = 0; i < nMaps && mp + 2 <= maps.size(); i++) {
        uint8_t type = u8(maps, mp), len = u8(maps, mp + 1);
        if (type == 2 && maps.compare(mp + 5, 23, "*UDF Metadata Partition") == 0) {
            uint32_t metaFileLoc = u32(maps, mp + 40);
            u.hasMeta = false;
            Udf::Node mf = u.entry(u.partStart + static_cast<int64_t>(metaFileLoc) * u.block, 0);
            if (mf.ok) { u.metaExt = mf.ext; u.hasMeta = true; u.metaRef = static_cast<uint16_t>(i); }
        }
        if (len == 0) break;
        mp += len;
    }
    // the folders' descriptors usually sit together at the start of the partition: fetch that part in parallel
    s.prefetch({{u.partStart, 2 * 1024 * 1024}});
    std::string f;
    if (!s.read(u.pos(fsd.part, fsd.lbn), u.block, f) || u16(f, 0) != 256) return false;
    LongAd root = longAd(f, 400);
    struct Dir { std::string path; int64_t at; uint16_t part; int depth; };
    std::vector<Dir> level = {{"", u.pos(root.part, root.lbn), root.part, 0}};
    std::set<int64_t> seen;
    int dirs = 0;
    while (!level.empty()) {
        std::vector<std::pair<int64_t, int64_t>> pre;
        for (auto& d : level) pre.push_back({d.at, u.block});
        s.prefetch(pre);
        struct Child { std::string path; int64_t at; uint16_t part; bool dir; };
        std::vector<Child> kids;
        std::vector<Dir> next;
        for (auto& d : level) {
            if (s.stopped()) { idx.error = "Stopped."; return false; }
            if (d.at < 0 || !seen.insert(d.at).second || d.depth > 64) continue;
            Udf::Node dn = u.entry(d.at, d.part);
            if (!dn.ok) continue;
            std::string data;
            std::vector<std::pair<int64_t, int64_t>> dpre;
            for (auto& e : dn.ext) if (e.first >= 0) dpre.push_back(e);
            s.prefetch(dpre);
            if (!u.readData(dn, data, 64 * 1024 * 1024)) { idx.error = s.error; return false; }
            if (status && ++dirs % 20 == 0) status("Reading folders… " + std::to_string(dirs));
            for (size_t p = 0; p + 38 <= data.size();) {
                if (u16(data, p) != 257) break;
                uint8_t ch = u8(data, p + 18), lfi = u8(data, p + 19);
                LongAd icb = longAd(data, p + 20);
                uint16_t liu = u16(data, p + 36);
                size_t nameAt = p + 38 + liu;
                std::string name = udfName(data, nameAt, lfi);
                p += (38 + liu + lfi + 3) & ~static_cast<size_t>(3);
                if (ch & 0x08) continue;   // parent
                if (ch & 0x04) continue;   // deleted
                if (name.empty()) continue;
                kids.push_back({d.path.empty() ? name : d.path + "/" + name, u.pos(icb.part, icb.lbn), icb.part, (ch & 0x02) != 0});
            }
        }
        // every child's descriptor of this level at once
        pre.clear();
        for (auto& k : kids) pre.push_back({k.at, u.block});
        s.prefetch(pre);
        for (auto& k : kids) {
            Udf::Node n = u.entry(k.at, k.part);
            if (!n.ok) continue;
            Entry e;
            e.path = k.path; e.dir = k.dir || n.dir; e.mtime = n.mtime;
            if (e.dir) next.push_back({k.path, k.at, k.part, 1});
            else { e.size = n.size; e.packed = n.size; e.extents = n.ext; e.embedded = n.inl; if (n.ext.empty() && n.inl.empty() && n.size > 0) e.why = "stored in an unusual way"; }
            idx.entries.push_back(e);
        }
        level.swap(next);
    }
    idx.label = label;
    idx.format = "ISO disc image (UDF)";
    return true;
}

bool lookIso(Source& s, Index& idx, std::function<void(const std::string&)> status) {
    std::string vd;
    if (!s.read(16 * SEC, 16 * SEC, vd)) { idx.error = s.error; return false; }
    bool udf = false;
    std::string pvd, svd;
    for (size_t p = 0; p + SEC <= vd.size(); p += SEC) {
        std::string id = vd.substr(p + 1, 5);
        if (id == "NSR02" || id == "NSR03") udf = true;
        if (id != "CD001") continue;
        uint8_t type = u8(vd, p);
        if (type == 1 && pvd.empty()) pvd = vd.substr(p, SEC);
        if (type == 2 && svd.empty() && u8(vd, p + 88) == 0x25 && u8(vd, p + 89) == 0x2F && (u8(vd, p + 90) == 0x40 || u8(vd, p + 90) == 0x43 || u8(vd, p + 90) == 0x45)) svd = vd.substr(p, SEC);
        if (type == 255) { /* terminator: UDF descriptors may follow */ }
    }
    if (udf) {
        Index u;
        if (lookUdf(s, u, status) && !u.entries.empty()) { idx.entries = std::move(u.entries); idx.format = u.format; idx.label = u.label; return true; }
        if (!u.error.empty() && u.error == "Stopped.") { idx.error = u.error; return false; }
    }
    if (pvd.empty()) { idx.error = udf ? "X-Ray couldn't read this disc image's folders." : "This isn't a disc image X-Ray knows."; return false; }
    bool joliet = !svd.empty();
    idx.label = trim(joliet ? utf16be(svd.data() + 40, 32) : pvd.substr(40, 32));
    if (!lookIso9660(s, idx, joliet ? svd : pvd, joliet, status)) return false;
    idx.format = joliet ? "ISO disc image (Joliet)" : "ISO disc image";
    return true;
}

// ------------------------------------------------------------------ RAR
bool vint(const std::string& b, size_t& p, uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64 && p < b.size(); shift += 7) {
        uint8_t c = u8(b, p++);
        v |= static_cast<uint64_t>(c & 0x7F) << shift;
        if (!(c & 0x80)) return true;
    }
    return false;
}

bool lookRar5(Source& s, Index& idx, int64_t start, std::function<void(const std::string&)> status) {
    int64_t pos = start + 8;
    for (int n = 0; pos < s.length && n < 200000; n++) {
        if (s.stopped()) { idx.error = "Stopped."; return false; }
        std::string h;
        if (!s.read(pos, 16, h) || h.size() < 6) break;
        size_t p = 4;
        uint64_t hsize = 0;
        if (!vint(h, p, hsize) || hsize > 2 * 1024 * 1024) { idx.error = "The RAR archive is damaged."; return false; }
        size_t hstart = p;
        if (!s.read(pos, hstart + static_cast<size_t>(hsize), h)) { idx.error = s.error; return false; }
        p = hstart;
        uint64_t type = 0, flags = 0, extra = 0, data = 0;
        vint(h, p, type); vint(h, p, flags);
        if (flags & 1) vint(h, p, extra);
        if (flags & 2) vint(h, p, data);
        if (type == 4) { idx.error = "The file list of this RAR archive is password-protected."; return false; }
        if (type == 5) break;
        if (type == 2) {
            uint64_t fflags = 0, usize = 0, attr = 0, comp = 0, os = 0, nlen = 0;
            vint(h, p, fflags); vint(h, p, usize); vint(h, p, attr);
            Entry e;
            if (fflags & 2) { e.mtime = u32(h, p); p += 4; }
            if (fflags & 4) { e.crc = u32(h, p); e.hasCrc = true; p += 4; }
            vint(h, p, comp); vint(h, p, os); vint(h, p, nlen);
            e.path = normPath(h.substr(p, static_cast<size_t>(nlen)));
            p += static_cast<size_t>(nlen);
            e.dir = (fflags & 1) != 0;
            e.size = static_cast<int64_t>(usize);
            e.packed = static_cast<int64_t>(data);
            e.method = static_cast<int>((comp >> 7) & 7);
            bool solid = (comp & 0x40) != 0;
            // extra records: encryption (type 1)
            size_t ex = hstart + static_cast<size_t>(hsize) - static_cast<size_t>(extra);
            for (size_t q = ex; q < hstart + hsize;) {
                uint64_t rsize = 0, rtype = 0; size_t q0 = q;
                if (!vint(h, q, rsize) || rsize == 0) break;
                size_t rend = q + static_cast<size_t>(rsize);
                vint(h, q, rtype);
                if (rtype == 1) e.encrypted = true;
                if (rtype == 5) e.why = "a link, not a file";   // symbolic link / junction: never followed
                q = std::max(rend, q0 + 1);
            }
            e.offset = pos + static_cast<int64_t>(hstart + hsize);
            if (fflags & 8) e.size = -1;   // size unknown
            if (!e.why.empty()) {}
            else if (e.encrypted) e.why = "password-protected";
            else if (flags & 0x08 || flags & 0x10) e.why = "split across several parts";
            else if (e.method != 0 || solid) e.why = "compressed (RAR)";
            if (!e.path.empty()) idx.entries.push_back(e);
            if (status && idx.entries.size() % 200 == 0) status("Reading the list… " + std::to_string(idx.entries.size()));
        }
        pos += static_cast<int64_t>(hstart + hsize + data);
    }
    idx.format = "RAR 5 archive";
    return true;
}

bool lookRar4(Source& s, Index& idx, int64_t start, std::function<void(const std::string&)> status) {
    int64_t pos = start + 7;
    for (int n = 0; pos + 7 <= s.length && n < 200000; n++) {
        if (s.stopped()) { idx.error = "Stopped."; return false; }
        std::string h;
        if (!s.read(pos, 7, h) || h.size() < 7) break;
        uint8_t type = u8(h, 2);
        uint16_t flags = u16(h, 3), hsize = u16(h, 5);
        if (hsize < 7) { idx.error = "The RAR archive is damaged."; return false; }
        if (!s.read(pos, hsize, h)) { idx.error = s.error; return false; }
        uint64_t add = (flags & 0x8000) && hsize >= 11 ? u32(h, 7) : 0;
        if (type == 0x73 && (flags & 0x80)) { idx.error = "The file list of this RAR archive is password-protected."; return false; }
        if (type == 0x7B) break;
        if (type == 0x74) {
            Entry e;
            uint64_t packed = u32(h, 7), unp = u32(h, 11);
            e.crc = u32(h, 16); e.hasCrc = true;
            uint32_t ft = u32(h, 20);
            e.mtime = dosTime(static_cast<uint16_t>(ft >> 16), static_cast<uint16_t>(ft & 0xFFFF));
            uint8_t method = u8(h, 25);
            uint16_t nsize = u16(h, 26);
            size_t np = 32;
            if (flags & 0x100) { packed |= static_cast<uint64_t>(u32(h, 32)) << 32; unp |= static_cast<uint64_t>(u32(h, 36)) << 32; np = 40; }
            std::string raw = h.substr(np, nsize);
            size_t z = raw.find('\0');
            std::string name;
            if (flags & 0x200) name = z == std::string::npos ? raw : raw.substr(0, z);   // the Unicode form follows; the plain one is enough to show
            else name = oem(raw);
            if ((flags & 0x200) && !validUtf8(name)) name = oem(name);
            e.path = normPath(name);
            e.dir = (flags & 0xE0) == 0xE0;
            e.size = static_cast<int64_t>(unp);
            e.packed = static_cast<int64_t>(packed);
            e.method = method == 0x30 ? 0 : method;
            e.encrypted = (flags & 0x04) != 0;
            e.offset = pos + hsize;
            uint32_t attr = u32(h, 28);
            if (u8(h, 15) == 3 && (attr & 0xF000) == 0xA000) e.why = "a link, not a file";
            else if (e.encrypted) e.why = "password-protected";
            else if (flags & 0x03) e.why = "split across several parts";
            else if (method != 0x30 || (flags & 0x10)) e.why = "compressed (RAR)";
            if (!e.path.empty()) idx.entries.push_back(e);
            if (status && idx.entries.size() % 200 == 0) status("Reading the list… " + std::to_string(idx.entries.size()));
            add = packed;
        }
        pos += hsize + static_cast<int64_t>(add);
    }
    idx.format = "RAR archive";
    return true;
}

// ------------------------------------------------------------------ 7z
struct Bytes {
    const std::string& b; size_t p = 0; bool bad = false;
    explicit Bytes(const std::string& s) : b(s) {}
    uint8_t byte() { if (p >= b.size()) { bad = true; return 0; } return u8(b, p++); }
    uint64_t num() {
        uint8_t first = byte(), mask = 0x80;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            if (!(first & mask)) { uint64_t high = first & (mask - 1u); return v | (high << (8 * i)); }
            v |= static_cast<uint64_t>(byte()) << (8 * i);
            mask >>= 1;
        }
        return v;
    }
    void skip(uint64_t n) { if (n > b.size() - std::min(p, b.size())) { bad = true; p = b.size(); } else p += static_cast<size_t>(n); }
    std::vector<bool> bits(size_t n) {   // bit vector, or "all defined"
        std::vector<bool> v(n);
        uint8_t all = byte();
        if (all) { for (size_t i = 0; i < n; i++) v[i] = true; return v; }
        uint8_t cur = 0;
        for (size_t i = 0; i < n; i++) { if (i % 8 == 0) cur = byte(); v[i] = (cur & (0x80 >> (i % 8))) != 0; }
        return v;
    }
    std::vector<bool> rawBits(size_t n) {
        std::vector<bool> v(n); uint8_t cur = 0;
        for (size_t i = 0; i < n; i++) { if (i % 8 == 0) cur = byte(); v[i] = (cur & (0x80 >> (i % 8))) != 0; }
        return v;
    }
};

struct Folder7 {
    std::vector<std::string> methods;   // coder ids (hex)
    std::string lzmaProps;
    std::vector<uint64_t> unpackSizes;
    uint64_t numOut = 0;
    bool encrypted = false;
    uint64_t unpackSize() const { return unpackSizes.empty() ? 0 : unpackSizes.back(); }
};

struct Streams7 {
    uint64_t packPos = 0;
    std::vector<uint64_t> packSizes;
    std::vector<Folder7> folders;
    std::vector<uint64_t> numUnpack;      // per folder
    std::vector<uint64_t> subSizes;       // every file stream
    std::vector<uint32_t> subCrc; std::vector<bool> subHasCrc;
};

void readFolder(Bytes& r, Folder7& f) {
    uint64_t coders = r.num(), totalOut = 0, totalIn = 0;
    for (uint64_t c = 0; c < coders && !r.bad; c++) {
        uint8_t flag = r.byte();
        size_t idLen = flag & 0x0F;
        std::string id;
        for (size_t i = 0; i < idLen; i++) { char hx[3]; snprintf(hx, sizeof hx, "%02X", r.byte()); id += hx; }
        uint64_t in = 1, out = 1;
        if (flag & 0x10) { in = r.num(); out = r.num(); }
        totalIn += in; totalOut += out;
        std::string props;
        if (flag & 0x20) { uint64_t n = r.num(); for (uint64_t i = 0; i < n && !r.bad; i++) props.push_back(static_cast<char>(r.byte())); }
        f.methods.push_back(id);
        if (id == "030101") f.lzmaProps = props;
        if (id == "21") f.lzmaProps = "2" + props;   // LZMA2: one byte of properties
        if (id == "06F10701") f.encrypted = true;
    }
    f.numOut = totalOut;
    uint64_t bindPairs = totalOut ? totalOut - 1 : 0;
    for (uint64_t i = 0; i < bindPairs && !r.bad; i++) { r.num(); r.num(); }
    uint64_t packed = totalIn - bindPairs;
    if (packed > 1) for (uint64_t i = 0; i < packed && !r.bad; i++) r.num();
}

bool readStreams(Bytes& r, Streams7& st) {
    for (uint8_t id = r.byte(); id != 0 && !r.bad; id = r.byte()) {
        if (id == 6) {   // pack info
            st.packPos = r.num();
            uint64_t n = r.num();
            for (uint8_t t = r.byte(); t != 0 && !r.bad; t = r.byte()) {
                if (t == 9) for (uint64_t i = 0; i < n; i++) st.packSizes.push_back(r.num());
                else if (t == 10) { auto d = r.bits(static_cast<size_t>(n)); for (bool x : d) if (x) r.skip(4); }
                else { uint64_t sz = r.num(); r.skip(sz); }
            }
        } else if (id == 7) {   // unpack info
            if (r.byte() != 11) return false;
            uint64_t n = r.num();
            if (n > 1000000) return false;
            if (r.byte() != 0) return false;   // external
            st.folders.resize(static_cast<size_t>(n));
            for (auto& f : st.folders) readFolder(r, f);
            if (r.byte() != 12) return false;
            for (auto& f : st.folders) for (uint64_t i = 0; i < f.numOut; i++) f.unpackSizes.push_back(r.num());
            for (uint8_t t = r.byte(); t != 0 && !r.bad; t = r.byte()) {
                if (t == 10) { auto d = r.bits(st.folders.size()); for (bool x : d) if (x) r.skip(4); }
                else { uint64_t sz = r.num(); r.skip(sz); }
            }
        } else if (id == 8) {   // substreams
            st.numUnpack.assign(st.folders.size(), 1);
            uint8_t t = r.byte();
            if (t == 13) { for (auto& n : st.numUnpack) n = r.num(); t = r.byte(); }
            bool sizes = t == 9;
            for (size_t f = 0; f < st.folders.size(); f++) {
                uint64_t n = st.numUnpack[f];
                if (n == 0) continue;
                uint64_t sum = 0;
                for (uint64_t i = 1; i < n; i++) { uint64_t s = sizes ? r.num() : 0; st.subSizes.push_back(s); sum += s; }
                st.subSizes.push_back(st.folders[f].unpackSize() - sum);
            }
            if (sizes) t = r.byte();
            while (t != 0 && !r.bad) {
                if (t == 10) {
                    size_t total = 0;
                    for (size_t f = 0; f < st.folders.size(); f++) total += static_cast<size_t>(st.numUnpack[f]);
                    auto d = r.bits(total);
                    for (bool x : d) { st.subHasCrc.push_back(x); st.subCrc.push_back(x ? static_cast<uint32_t>(r.byte() | (r.byte() << 8) | (r.byte() << 16) | (static_cast<uint32_t>(r.byte()) << 24)) : 0); }
                } else { uint64_t sz = r.num(); r.skip(sz); }
                t = r.byte();
            }
        } else return false;
    }
    if (st.numUnpack.empty()) { st.numUnpack.assign(st.folders.size(), 1); for (auto& f : st.folders) st.subSizes.push_back(f.unpackSize()); }
    return !r.bad;
}

bool lzmaDecode(const std::string& props, const std::string& in, size_t outSize, std::string& out) {
    if (outSize > 64 * 1024 * 1024) return false;   // a list bigger than that is a trick, not an archive
    out.assign(outSize, '\0');
    SizeT destLen = outSize, srcLen = in.size();
    ELzmaStatus st;
    ISzAlloc alloc = {[](ISzAllocPtr, size_t s) -> void* { return malloc(s); }, [](ISzAllocPtr, void* a) { free(a); }};
    if (props.size() == 2 && props[0] == '2') {
        SRes r = Lzma2Decode(reinterpret_cast<Byte*>(&out[0]), &destLen, reinterpret_cast<const Byte*>(in.data()), &srcLen, static_cast<Byte>(props[1]), LZMA_FINISH_ANY, &st, &alloc);
        return r == SZ_OK && destLen == outSize;
    }
    if (props.size() != 5) return false;
    SRes r = LzmaDecode(reinterpret_cast<Byte*>(&out[0]), &destLen, reinterpret_cast<const Byte*>(in.data()), &srcLen,
                        reinterpret_cast<const Byte*>(props.data()), 5, LZMA_FINISH_ANY, &st, &alloc);
    return r == SZ_OK && destLen == outSize;
}

bool look7z(Source& s, Index& idx) {
    std::string sh;
    if (!s.read(0, 32, sh) || sh.size() < 32) { idx.error = s.error; return false; }
    int64_t nextOff = static_cast<int64_t>(u64(sh, 12)), nextSize = static_cast<int64_t>(u64(sh, 20));
    if (nextOff < 0 || nextOff > s.length || nextSize <= 0 || nextSize > 64 * 1024 * 1024 || 32 + nextOff + nextSize > s.length) { idx.error = "This 7z archive is incomplete or damaged."; return false; }
    std::string hdr;
    if (!s.read(32 + nextOff, static_cast<size_t>(nextSize), hdr)) { idx.error = s.error; return false; }
    for (int round = 0; round < 4 && !hdr.empty() && u8(hdr, 0) == 0x17; round++) {   // the list is itself packed
        Bytes r(hdr); r.byte();
        Streams7 st;
        if (!readStreams(r, st) || st.folders.empty() || st.packSizes.empty()) { idx.error = "This 7z archive's list is damaged."; return false; }
        const Folder7& f = st.folders[0];
        if (f.encrypted) { idx.error = "The file list of this 7z archive is password-protected."; return false; }
        if (f.lzmaProps.empty()) { idx.error = "This 7z archive keeps its list in a way X-Ray can't read. Download the whole file instead."; return false; }
        std::string packed, plain;
        if (st.packSizes[0] > 64ULL * 1024 * 1024 || st.packPos > static_cast<uint64_t>(s.length)) { idx.error = "This 7z archive's list is damaged."; return false; }   // (a packed list that size is a trick)
        if (!s.read(32 + static_cast<int64_t>(st.packPos), static_cast<size_t>(st.packSizes[0]), packed)) { idx.error = s.error; return false; }
        if (!lzmaDecode(f.lzmaProps, packed, static_cast<size_t>(f.unpackSize()), plain)) { idx.error = "This 7z archive's list is damaged."; return false; }
        hdr.swap(plain);
    }
    Bytes r(hdr);
    if (r.byte() != 1) { idx.error = "This 7z archive's list is damaged."; return false; }
    Streams7 st;
    std::vector<Entry> files;
    for (uint8_t id = r.byte(); id != 0 && !r.bad; id = r.byte()) {
        if (id == 2) { for (uint8_t t = r.byte(); t != 0 && !r.bad; t = r.byte()) { uint64_t sz = r.num(); r.skip(sz); } }   // archive properties
        else if (id == 3) { uint8_t t; while ((t = r.byte()) != 0 && !r.bad) { (void)t; } }   // additional streams (rare)
        else if (id == 4) { if (!readStreams(r, st)) { idx.error = "This 7z archive's list is damaged."; return false; } }
        else if (id == 5) {
            uint64_t n = r.num();
            if (n > 500000) { idx.error = "This 7z archive's list is damaged."; return false; }
            files.resize(static_cast<size_t>(n));
            std::vector<bool> empty(files.size()), emptyFile, anti;
            for (uint8_t t = r.byte(); t != 0 && !r.bad; t = r.byte()) {
                uint64_t sz = r.num();
                size_t end = r.p + static_cast<size_t>(sz);
                if (t == 14) empty = r.rawBits(files.size());
                else if (t == 15) { size_t ne = 0; for (bool x : empty) ne += x; emptyFile = r.rawBits(ne); }
                else if (t == 17) {
                    r.byte();   // external
                    for (auto& e : files) { size_t st0 = r.p; while (r.p + 1 < hdr.size() && (hdr[r.p] || hdr[r.p + 1])) r.p += 2; e.path = normPath(utf16le(hdr.data() + st0, r.p - st0)); r.p += 2; }
                } else if (t == 20) {
                    auto d = r.bits(files.size()); r.byte();
                    for (size_t i = 0; i < files.size(); i++) if (d[i]) { uint64_t ft = 0; for (int k = 0; k < 8; k++) ft |= static_cast<uint64_t>(r.byte()) << (8 * k); files[i].mtime = fileTime(ft); }
                } else if (t == 21) {
                    auto d = r.bits(files.size()); r.byte();
                    for (size_t i = 0; i < files.size(); i++) if (d[i]) { uint32_t a = r.byte() | (r.byte() << 8) | (r.byte() << 16) | (static_cast<uint32_t>(r.byte()) << 24); if (a & 0x10) files[i].dir = true; }
                }
                r.p = std::min(end, hdr.size());
            }
            size_t emptyIdx = 0, stream = 0;
            for (size_t i = 0; i < files.size(); i++) {
                Entry& e = files[i];
                if (empty[i]) {
                    bool isFile = emptyIdx < emptyFile.size() && emptyFile[emptyIdx];
                    emptyIdx++;
                    if (!isFile) e.dir = true;
                    e.size = 0;
                } else {
                    e.size = stream < st.subSizes.size() ? static_cast<int64_t>(st.subSizes[stream]) : 0;
                    if (stream < st.subHasCrc.size() && st.subHasCrc[stream]) { e.crc = st.subCrc[stream]; e.hasCrc = true; }
                    stream++;
                }
            }
        } else return false;
    }
    bool enc = false;
    for (auto& f : st.folders) enc = enc || f.encrypted;
    for (auto& e : files) { if (!e.dir) e.why = enc ? "password-protected" : "compressed (7z)"; e.encrypted = enc && !e.dir; if (!e.path.empty()) idx.entries.push_back(e); }
    idx.format = "7z archive";
    return !r.bad || !idx.entries.empty();
}

// ------------------------------------------------------------------ TAR
int64_t octal(const std::string& b, size_t o, size_t n) {
    if (u8(b, o) & 0x80) { int64_t v = 0; for (size_t i = 1; i < n; i++) v = (v << 8) | u8(b, o + i); return v; }   // base-256 (big files)
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) { char c = b[o + i]; if (c == ' ' && v == 0) continue; if (c < '0' || c > '7') break; v = v * 8 + (c - '0'); }
    return v;
}

bool lookTar(Source& s, Index& idx, std::function<void(const std::string&)> status) {
    int64_t pos = 0;
    std::string longName;
    for (int n = 0; pos + 512 <= s.length && n < 500000; n++) {
        if (s.stopped()) { idx.error = "Stopped."; return false; }
        std::string h;
        if (!s.read(pos, 512, h) || h.size() < 512) break;
        if (h.find_first_not_of('\0') == std::string::npos) break;   // the end
        int64_t size = octal(h, 124, 12);
        char type = h[156];
        std::string name = h.substr(0, std::min<size_t>(100, h.find('\0') == std::string::npos ? 100 : h.find('\0')));
        if (h.compare(257, 5, "ustar") == 0 && h[345]) { std::string pre = h.substr(345, 155); pre = pre.substr(0, pre.find('\0')); name = pre + "/" + name; }
        int64_t data = pos + 512;
        int64_t next = data + ((size + 511) / 512) * 512;
        if (type == 'L') { std::string ln; if (s.read(data, static_cast<size_t>(std::min<int64_t>(size, 65536)), ln)) longName = ln.substr(0, ln.find('\0')); pos = next; continue; }
        if (type == 'x' || type == 'g') {
            std::string px;
            if (type == 'x' && s.read(data, static_cast<size_t>(std::min<int64_t>(size, 65536)), px)) {
                size_t k = px.find(" path=");
                if (k != std::string::npos) { size_t e = px.find('\n', k); longName = px.substr(k + 6, e == std::string::npos ? std::string::npos : e - k - 6); }
            }
            pos = next; continue;
        }
        if (!longName.empty()) { name = longName; longName.clear(); }
        Entry e;
        e.path = normPath(name);
        e.dir = type == '5' || endsWith(name, "/");
        e.size = (type == '0' || type == '\0' || type == '7') ? size : 0;
        e.packed = e.size;
        e.mtime = octal(h, 136, 12);
        e.offset = data;
        if (type == '1' || type == '2') e.why = "a link, not a file";
        if (!e.path.empty() && (e.dir || type == '0' || type == '\0' || type == '7' || type == '1' || type == '2')) idx.entries.push_back(e);
        if (status && idx.entries.size() % 500 == 0) status("Reading the list… " + std::to_string(idx.entries.size()));
        pos = next;
    }
    idx.format = "TAR archive";
    return true;
}

// ------------------------------------------------------------------ CAB
bool lookCab(Source& s, Index& idx) {
    std::string h;
    if (!s.read(0, 60, h) || h.compare(0, 4, "MSCF") != 0) { idx.error = "This CAB file is damaged."; return false; }
    uint32_t coff = u32(h, 16);
    uint16_t files = u16(h, 28);
    std::string b;
    if (!s.read(coff, std::min<int64_t>(s.length - coff, static_cast<int64_t>(files) * 300 + 1024), b)) { idx.error = s.error; return false; }
    size_t p = 0;
    for (uint16_t i = 0; i < files && p + 16 < b.size(); i++) {
        Entry e;
        e.size = u32(b, p);
        e.mtime = dosTime(u16(b, p + 10), u16(b, p + 12));
        uint16_t attr = u16(b, p + 14);
        size_t z = b.find('\0', p + 16);
        if (z == std::string::npos) break;
        std::string name = b.substr(p + 16, z - p - 16);
        e.path = normPath((attr & 0x80) ? name : oem(name));
        e.why = "compressed (CAB)";
        idx.entries.push_back(e);
        p = z + 1;
    }
    idx.format = "Cabinet (CAB) archive";
    return true;
}

// ------------------------------------------------------------------ GZ
bool lookGz(Source& s, Index& idx, const std::string& fileName) {
    std::string h, t;
    if (!s.read(0, 1024, h) || !s.read(s.length - 4, 4, t)) { idx.error = s.error; return false; }
    uint8_t flags = u8(h, 3);
    size_t p = 10;
    if (flags & 4) p += 2 + u16(h, 10);
    std::string name;
    if (flags & 8) { size_t z = h.find('\0', p); if (z != std::string::npos) name = h.substr(p, z - p); }
    if (name.empty()) name = stem(fileName);
    Entry e;
    e.path = normPath(oem(name));
    e.size = u32(t, 0);
    e.packed = s.length;
    e.mtime = u32(h, 4);
    e.why = "compressed (GZ)";
    idx.entries.push_back(e);
    idx.format = "GZ compressed file";
    std::string ln = lower(fileName);
    if (endsWith(ln, ".tar.gz") || endsWith(ln, ".tgz")) idx.notes.push_back("A .tar.gz shows its files only once it is unpacked as a whole (it's packed in one piece). Size shown: the .tar inside" + std::string(s.length > 0xFFFFFFFFLL ? " (approximate: over 4 GB)." : "."));
    idx.listing = true;
    return true;
}

// ------------------------------------------------------------------ programs: who signed it
std::string signerOf(const std::string& pkcs7, std::string& issuer) {
    CRYPT_DATA_BLOB blob{static_cast<DWORD>(pkcs7.size()), reinterpret_cast<BYTE*>(const_cast<char*>(pkcs7.data()))};
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    std::string who;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_BLOB, &blob, CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr, &store, &msg, nullptr)) return "";
    DWORD len = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &len) && len) {
        std::string buf(len, '\0');
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, &buf[0], &len)) {
            auto* si = reinterpret_cast<CMSG_SIGNER_INFO*>(&buf[0]);
            CERT_INFO ci{};
            ci.Issuer = si->Issuer; ci.SerialNumber = si->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
            if (cert) {
                wchar_t name[512] = {};
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 512);
                who = U(name);
                name[0] = 0;
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, CERT_NAME_ISSUER_FLAG, nullptr, name, 512);
                issuer = U(name);
                CertFreeCertificateContext(cert);
            }
        }
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return who;
}

bool lookProgram(Source& s, Index& idx) {
    std::string h;
    if (!s.read(0, 4096, h) || h.size() < 64 || h.compare(0, 2, "MZ") != 0) { idx.error = "This isn't a Windows program."; return false; }
    uint32_t pe = u32(h, 0x3C);
    std::string ph;
    if (!s.read(pe, 512, ph) || ph.compare(0, 4, std::string("PE\0\0", 4)) != 0) { idx.error = "This isn't a Windows program."; return false; }
    uint16_t machine = u16(ph, 4), chars = u16(ph, 22);
    uint32_t stamp = u32(ph, 8);
    size_t opt = 24;
    uint16_t magic = u16(ph, opt);
    bool pe64 = magic == 0x20B;
    size_t dd = opt + (pe64 ? 112 : 96);
    uint32_t certOff = u32(ph, dd + 4 * 8), certLen = u32(ph, dd + 4 * 8 + 4);
    uint16_t subsystem = u16(ph, opt + 68);
    idx.listing = false;
    idx.format = (chars & 0x2000) ? "Windows library (DLL)" : "Windows program";
    idx.notes.push_back(std::string(machine == 0x8664 ? "64-bit" : machine == 0x14C ? "32-bit" : machine == 0xAA64 ? "ARM64" : "Other processor") +
                        (subsystem == 3 ? " · runs in a console window" : subsystem == 2 ? " · has its own windows" : ""));
    if (stamp > 315532800 && stamp < 4102444800u) idx.notes.push_back("Built " + formatDay(static_cast<int64_t>(stamp) * 1000));
    if (certOff == 0 || certLen < 8) { idx.warnings.push_back("Not signed: nobody vouches for who made this program."); return true; }
    if (static_cast<int64_t>(certOff) + certLen > s.length || certLen > 2 * 1024 * 1024) { idx.warnings.push_back("Its signature is damaged."); return true; }
    std::string cert;
    if (!s.read(certOff, certLen, cert)) { idx.error = s.error; return false; }
    uint32_t wl = u32(cert, 0);
    uint16_t type = u16(cert, 6);
    if (type != 2 || wl < 8 || wl > cert.size()) { idx.warnings.push_back("Its signature is damaged."); return true; }
    std::string issuer, who = signerOf(cert.substr(8, wl - 8), issuer);
    if (who.empty()) idx.warnings.push_back("Its signature is damaged.");
    else idx.notes.push_back("Signed by " + who + (issuer.empty() ? "" : " (certificate from " + issuer + ")") + ". Windows checks the signature fully once the file is on your PC.");
    idx.label = who;
    return true;
}

// ------------------------------------------------------------------ torrents
struct BNode {
    enum T { Int, Str, List, Dict } t = Str;
    int64_t i = 0; std::string s; std::vector<BNode> l; std::vector<std::pair<std::string, BNode>> d;
    const BNode* get(const std::string& k) const { for (auto& kv : d) if (kv.first == k) return &kv.second; return nullptr; }
};
bool bparse(const std::string& b, size_t& p, BNode& n, int depth = 0) {
    if (p >= b.size() || depth > 64) return false;
    char c = b[p];
    if (c == 'i') { size_t e = b.find('e', p); if (e == std::string::npos) return false; n.t = BNode::Int; n.i = _atoi64(b.substr(p + 1, e - p - 1).c_str()); p = e + 1; return true; }
    if (c == 'l') { n.t = BNode::List; p++; while (p < b.size() && b[p] != 'e') { BNode x; if (!bparse(b, p, x, depth + 1)) return false; n.l.push_back(std::move(x)); } p++; return true; }
    if (c == 'd') { n.t = BNode::Dict; p++; while (p < b.size() && b[p] != 'e') { BNode k, v; if (!bparse(b, p, k, depth + 1) || !bparse(b, p, v, depth + 1)) return false; n.d.push_back({k.s, std::move(v)}); } p++; return true; }
    if (c >= '0' && c <= '9') { size_t col = b.find(':', p); if (col == std::string::npos) return false; int64_t len = _atoi64(b.substr(p, col - p).c_str()); if (len < 0 || col + 1 + static_cast<size_t>(len) > b.size()) return false; n.t = BNode::Str; n.s = b.substr(col + 1, static_cast<size_t>(len)); p = col + 1 + static_cast<size_t>(len); return true; }
    return false;
}

} // namespace

bool lookTorrent(const std::string& data, Index& idx) {
    BNode root;
    size_t p = 0;
    if (!bparse(data, p, root) || root.t != BNode::Dict) { idx.error = "This torrent file is damaged."; return false; }
    const BNode* info = root.get("info");
    if (!info || info->t != BNode::Dict) { idx.error = "This torrent file is damaged."; return false; }
    const BNode* nm = info->get("name.utf-8"); if (!nm) nm = info->get("name");
    std::string name = nm ? nm->s : "torrent";
    idx.label = name;
    const BNode* files = info->get("files");
    int64_t off = 0;
    int number = 0;
    if (files && files->t == BNode::List) {
        for (auto& f : files->l) {
            number++;
            const BNode* pp = f.get("path.utf-8"); if (!pp) pp = f.get("path");
            const BNode* len = f.get("length");
            const BNode* attr = f.get("attr");
            std::string path = name;
            if (pp) for (auto& part : pp->l) path += "/" + part.s;
            Entry e; e.path = normPath(path); e.size = len ? len->i : 0; e.offset = off; e.index = number; off += e.size;
            if (attr && attr->s.find('p') != std::string::npos) continue;   // padding
            idx.entries.push_back(e);
        }
    } else {
        const BNode* len = info->get("length");
        Entry e; e.path = normPath(name); e.size = len ? len->i : 0; e.offset = 0; e.index = 1;
        idx.entries.push_back(e);
    }
    const BNode* pl = info->get("piece length");
    const BNode* pieces = info->get("pieces");
    int trackers = 0;
    if (const BNode* al = root.get("announce-list")) for (auto& tier : al->l) trackers += static_cast<int>(tier.l.size());
    else if (root.get("announce")) trackers = 1;
    std::string facts = std::to_string(pieces ? pieces->s.size() / 20 : 0) + " pieces of " + formatBytes(pl ? pl->i : 0) + " · " + std::to_string(trackers) + (trackers == 1 ? " tracker" : " trackers");
    const BNode* priv = info->get("private");
    if (priv && priv->i == 1) facts += " · private";
    if (const BNode* cd = root.get("creation date")) facts += " · made " + formatDay(cd->i * 1000);
    idx.notes.push_back(facts);
    // the biggest file is a program in a "film" or "album"? a classic fake
    const Entry* big = nullptr;
    for (auto& e : idx.entries) if (!big || e.size > big->size) big = &e;
    std::string ln = lower(name);
    if (big && programExts().count(lower(extOf(big->path))) && (ln.find("1080p") != std::string::npos || ln.find("720p") != std::string::npos || ln.find("x264") != std::string::npos || ln.find("bluray") != std::string::npos || ln.find("mp3") != std::string::npos || ln.find("flac") != std::string::npos))
        idx.warnings.push_back("Looks fake: it's named like a film or music, but its biggest file is a program.");
    for (auto& e : idx.entries) if (lower(extOf(e.path)) == "rar" || lower(extOf(e.path)) == "zip") { if (ln.find("password") != std::string::npos) idx.warnings.push_back("Password-protected archives in a torrent are a common trick to make you visit a website."); break; }
    idx.format = "Torrent";
    idx.length = static_cast<int64_t>(data.size());
    markRisks(idx, name);
    return true;
}

std::string kindOf(const std::string& fileName) {
    std::string e = lower(extOf(fileName)), n = lower(fileName);
    if (endsWith(n, ".tar.gz") || endsWith(n, ".tgz")) return "gz";
    static const std::set<std::string> zip = {"zip", "jar", "apk", "xapk", "apkm", "aab", "ipa", "docx", "xlsx", "pptx", "odt", "ods", "odp", "epub", "cbz", "nupkg", "vsix", "whl", "crx", "xpi", "appx", "msix", "appxbundle", "msixbundle", "kmz", "3mf", "war", "ear"};
    if (zip.count(e)) return "zip";
    if (e == "iso" || e == "udf") return "iso";
    if (e == "rar" || e == "cbr") return "rar";
    if (e == "7z" || e == "cb7") return "7z";
    if (e == "tar") return "tar";
    if (e == "cab") return "cab";
    if (e == "gz" || e == "gzip") return "gz";
    if (e == "torrent") return "torrent";
    if (e == "exe" || e == "dll" || e == "scr" || e == "sys" || e == "msi" || e == "ocx" || e == "cpl") return "program";
    return "";
}
bool canLook(const std::string& fileName) { return !kindOf(fileName).empty(); }

static bool lookInside(Source& s, const std::string& fileName, Index& idx, std::function<void(const std::string&)> status);

// A damaged or hostile file can only fail the look, never take DDM down (out of memory, anything unexpected).
bool look(Source& s, const std::string& fileName, Index& idx, std::function<void(const std::string&)> status) {
    try { return lookInside(s, fileName, idx, status); }
    catch (...) { idx.entries.clear(); idx.error = "X-Ray couldn't look inside."; return false; }
}

static bool lookInside(Source& s, const std::string& fileName, Index& idx, std::function<void(const std::string&)> status) {
    auto t0 = std::chrono::steady_clock::now();
    idx.length = s.length;
    {   // the start (headers, disc descriptors) and the end (ZIP's directory) together, on two connections
        int64_t tail = std::min<int64_t>(s.length, 128 * 1024);
        std::vector<std::pair<int64_t, int64_t>> first = {{0, std::min<int64_t>(s.length, 128 * 1024)}};
        if (s.length > 256 * 1024) first.push_back({s.length - tail, tail});
        s.prefetch(first);
    }
    std::string head;
    if (!s.read(0, 64 * 1024, head)) { idx.error = s.error; return false; }
    std::string kind = kindOf(fileName);
    // the file's own first bytes decide; the name is only a hint
    bool ok = false;
    if (head.compare(0, 6, std::string("7z\xBC\xAF\x27\x1C", 6)) == 0) ok = look7z(s, idx);
    else if (head.compare(0, 8, std::string("Rar!\x1A\x07\x01\x00", 8)) == 0) ok = lookRar5(s, idx, 0, status);
    else if (head.compare(0, 7, std::string("Rar!\x1A\x07\x00", 7)) == 0) ok = lookRar4(s, idx, 0, status);
    else if (head.compare(0, 4, "MSCF") == 0) ok = lookCab(s, idx);
    else if (head.size() > 2 && u8(head, 0) == 0x1F && u8(head, 1) == 0x8B) ok = lookGz(s, idx, fileName);
    else if (head.size() >= 262 && head.compare(257, 5, "ustar") == 0) ok = lookTar(s, idx, status);
    else if (kind == "torrent" || (head.size() > 11 && head.compare(0, 11, "d8:announce") == 0) || head.compare(0, 7, "d4:info") == 0) {
        std::string all;
        if (s.length > 20 * 1024 * 1024) idx.error = "This torrent file is too big.";
        else if (s.read(0, static_cast<size_t>(s.length), all)) ok = lookTorrent(all, idx);
        else idx.error = s.error;
    }
    else if (kind == "iso" || s.length > 34 * SEC) {
        std::string vd;
        bool iso = s.length > 18 * SEC && s.read(16 * SEC, 8, vd) && (vd.compare(1, 5, "CD001") == 0 || vd.compare(1, 5, "BEA01") == 0);
        if (iso) ok = lookIso(s, idx, status);
        else if (head.compare(0, 2, "MZ") == 0 && kind != "zip") {
            // a self-extracting archive is a program with an archive inside: show the archive when there is one
            Index z;
            if (kind == "program" && lookZip(s, z) && !z.entries.empty()) { idx = z; idx.notes.push_back("A self-extracting archive (a program with a ZIP inside)."); ok = true; Index p; if (lookProgram(s, p)) { for (auto& n : p.notes) idx.notes.push_back(n); for (auto& w : p.warnings) idx.warnings.push_back(w); } }
            else ok = lookProgram(s, idx);
        }
        else if (head.compare(0, 8, std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) == 0 && kind == "program") { idx.listing = false; idx.format = "Windows Installer package (MSI)"; idx.notes.push_back("Who signed it is checked once it's on your PC."); ok = true; }
        else ok = lookZip(s, idx);
    }
    else if (head.compare(0, 2, "MZ") == 0) ok = lookProgram(s, idx);
    else ok = lookZip(s, idx);
    if (!ok && idx.error.empty()) idx.error = "X-Ray doesn't know how to look inside this file.";
    idx.bytesRead = s.fetched;
    idx.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (ok) markRisks(idx, fileName);
    return ok;
}

// ------------------------------------------------------------------ saving chosen files
std::string safeOutPath(const std::string& root, const std::string& inner, bool keepFolders) {
    std::string p = replaceAll(inner, "\\", "/");
    std::vector<std::string> parts;
    for (auto& part : split(p, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find(':') != std::string::npos) return "";
        std::string clean = sanitizeFileName(part);
        std::string up = lower(stem(clean));
        static const std::set<std::string> dev = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
        if (dev.count(up)) clean = "_" + clean;
        while (!clean.empty() && (clean.back() == '.' || clean.back() == ' ')) clean.pop_back();
        if (clean.empty()) clean = "_";
        parts.push_back(clean);
    }
    if (parts.empty()) return "";
    std::string out = root;
    if (keepFolders) for (auto& x : parts) out = joinPath(out, x);
    else out = joinPath(out, parts.back());
    return out;
}

bool extract(Source& s, const Index& idx, const std::vector<size_t>& which, const std::string& outDir, bool keepFolders,
             std::function<void(int64_t, int64_t, const std::string&)> progress, ExtractResult& res) {
    int64_t total = 0, done = 0;
    for (size_t i : which) if (i < idx.entries.size() && !idx.entries[i].dir) total += idx.entries[i].packed > 0 ? idx.entries[i].packed : idx.entries[i].size;
    bool zip = idx.format.compare(0, 3, "ZIP") == 0;
    std::set<std::string> used;
    for (size_t i : which) {
        if (s.stopped()) { res.problems.push_back("Stopped."); return false; }
        if (i >= idx.entries.size()) continue;
        const Entry& e = idx.entries[i];
        std::string out = safeOutPath(outDir, e.path, keepFolders);
        if (out.empty()) { res.problems.push_back(e.path + ": refused (it tries to write outside the folder)."); continue; }
        if (e.dir) { if (keepFolders) makeDirs(out); continue; }
        if (!e.why.empty()) { res.problems.push_back(e.path + ": can't be picked out (" + e.why + ")."); continue; }
        if (fileExists(out) || used.count(lower(out))) out = uniquePath(out);
        used.insert(lower(out));
        makeDirs(dirName(out));
        std::string tmp = out + ".ddmpart";
        HANDLE f = CreateFileW(W(tmp).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { res.problems.push_back(e.path + ": couldn't create " + out + "."); continue; }
        if (progress) progress(done, total, e.path);
        auto tick = [&](int64_t n) { done += n; if (progress) progress(done, total, e.path); };
        bool ok = true;
        std::string why;
        if (zip) {
            std::string lh;
            if (!s.read(e.offset, 30, lh) || u32(lh, 0) != 0x04034b50) { ok = false; why = "the archive is damaged here"; }
            else {
                int64_t data = e.offset + 30 + u16(lh, 26) + u16(lh, 28);
                if (e.method == 0) ok = s.copyTo(data, e.packed, f, 0, tick);
                else {
                    // the packed bytes first (several connections), then unpacked here
                    std::string packTmp = out + ".packed";
                    HANDLE pf = CreateFileW(W(packTmp).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                    ok = pf != INVALID_HANDLE_VALUE && s.copyTo(data, e.packed, pf, 0, tick);
                    if (ok) {
                        SetFilePointer(pf, 0, nullptr, FILE_BEGIN);
                        mz_stream z{};
                        ok = mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK;
                        std::string in(1 << 20, '\0'), o(1 << 20, '\0');
                        int rc = MZ_OK;
                        while (ok && rc != MZ_STREAM_END) {
                            DWORD got = 0;
                            if (z.avail_in == 0) { if (!ReadFile(pf, &in[0], static_cast<DWORD>(in.size()), &got, nullptr)) { ok = false; break; } z.next_in = reinterpret_cast<const unsigned char*>(in.data()); z.avail_in = got; if (!got) break; }
                            z.next_out = reinterpret_cast<unsigned char*>(&o[0]); z.avail_out = static_cast<unsigned>(o.size());
                            rc = mz_inflate(&z, MZ_NO_FLUSH);
                            if (rc != MZ_OK && rc != MZ_STREAM_END) { ok = false; why = "its data is damaged"; break; }
                            DWORD n = static_cast<DWORD>(o.size() - z.avail_out), w = 0;
                            if (n && (!WriteFile(f, o.data(), n, &w, nullptr) || w != n)) { ok = false; why = "couldn't write to the disk"; }
                        }
                        mz_inflateEnd(&z);
                        if (ok && rc != MZ_STREAM_END) { ok = false; why = "its data ends too early"; }
                    }
                    if (pf != INVALID_HANDLE_VALUE) CloseHandle(pf);
                }
            }
        } else if (!e.extents.empty() || !e.embedded.empty()) {   // ISO / UDF
            int64_t at = 0;
            if (!e.embedded.empty()) { DWORD w = 0; ok = WriteFile(f, e.embedded.data(), static_cast<DWORD>(e.embedded.size()), &w, nullptr) != 0; at = static_cast<int64_t>(e.embedded.size()); }
            for (auto& x : e.extents) {
                if (!ok) break;
                int64_t len = std::min<int64_t>(x.second, e.size - at);
                if (len <= 0) break;
                if (x.first < 0) { at += len; tick(len); continue; }   // a hole: zeros
                ok = s.copyTo(x.first, len, f, at, tick);
                at += len;
            }
            if (ok) { LARGE_INTEGER sz; sz.QuadPart = e.size; SetFilePointerEx(f, sz, nullptr, FILE_BEGIN); SetEndOfFile(f); }
        } else if (e.offset >= 0) {   // TAR, stored RAR
            ok = s.copyTo(e.offset, e.size, f, 0, tick);
        } else { ok = false; why = "X-Ray can't pick it out"; }
        if (ok && e.hasCrc) {   // checked against the archive's own checksum
            SetFilePointer(f, 0, nullptr, FILE_BEGIN);
            std::string buf(1 << 20, '\0');
            mz_ulong crc = MZ_CRC32_INIT;
            DWORD got = 0;
            while (ReadFile(f, &buf[0], static_cast<DWORD>(buf.size()), &got, nullptr) && got) crc = mz_crc32(crc, reinterpret_cast<const unsigned char*>(buf.data()), got);
            if (static_cast<uint32_t>(crc) != e.crc) { ok = false; why = "its checksum doesn't match (damaged on the server)"; }
        }
        CloseHandle(f);
        if (!ok) {
            DeleteFileW(W(tmp).c_str());
            res.problems.push_back(e.path + ": " + (why.empty() ? (s.error.empty() ? "couldn't be downloaded" : s.error) : why) + ".");
            if (s.stopped()) return false;
            continue;
        }
        if (!MoveFileExW(W(tmp).c_str(), W(out).c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(W(tmp).c_str()); res.problems.push_back(e.path + ": couldn't be saved."); continue; }
        if (e.mtime > 0) {
            HANDLE h = CreateFileW(W(out).c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { ULARGE_INTEGER u; u.QuadPart = (static_cast<uint64_t>(e.mtime) + 11644473600ULL) * 10000000ULL; FILETIME ft{u.LowPart, u.HighPart}; SetFileTime(h, nullptr, nullptr, &ft); CloseHandle(h); }
        }
        res.saved++;
        res.files.push_back(out);
    }
    return true;
}

} // namespace xray
} // namespace ddm
