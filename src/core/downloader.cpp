#include "downloader.hpp"
#include "speedmeter.hpp"
#include "links.hpp"
#include "settings.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <winioctl.h>
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <memory>

namespace ddm {

// ---------------------------------------------------------------- speed limit (token bucket, all downloads together)
namespace {
std::mutex g_limitMu;
int64_t g_limit = 0;           // bytes per second
double g_tokens = 0;
int64_t g_lastRefill = 0;

void refillTokens() {
    std::lock_guard<std::mutex> g(g_limitMu);
    if (g_limit <= 0) return;
    int64_t now = nowMs();
    if (g_lastRefill == 0) g_lastRefill = now;
    double add = g_limit * (now - g_lastRefill) / 1000.0;
    g_lastRefill = now;
    g_tokens = std::min(g_tokens + add, static_cast<double>(g_limit) / 4 + 1);
}

bool takeTokens(size_t n) {
    std::lock_guard<std::mutex> g(g_limitMu);
    if (g_limit <= 0) return true;
    if (g_tokens <= 0) return false;
    g_tokens -= static_cast<double>(n);   // may go into debt; the next refills pay it back
    return true;
}

bool limited() { std::lock_guard<std::mutex> g(g_limitMu); return g_limit > 0; }
} // namespace

std::atomic<int64_t> g_received{0};
int64_t receivedBytes() { return g_received.load(); }

void setGlobalSpeedLimit(int64_t bps) {
    std::lock_guard<std::mutex> g(g_limitMu);
    g_limit = bps;
    g_tokens = 0;
    g_lastRefill = 0;
}

std::string friendlyError(long code, long status) {
    if (status == 401 || status == 403) return "Access denied — the link may have expired. Use Refresh download link.";
    if (status == 404 || status == 410) return "The link has expired or the file was moved. Use Refresh download link.";
    if (status == 416) return "The server refused to continue this file.";
    if (status >= 500) return "The server had a problem (" + std::to_string(status) + "). DDM will retry.";
    if (status >= 400) return "The server refused the download (" + std::to_string(status) + ").";
    switch (code) {
        case CURLE_COULDNT_RESOLVE_HOST: return "Couldn't find the server — check the address or your internet connection.";
        case CURLE_COULDNT_CONNECT: return "Couldn't connect to the server.";
        case CURLE_OPERATION_TIMEDOUT: return "The server stopped answering.";
        case CURLE_PEER_FAILED_VERIFICATION: return "The site's security certificate isn't trusted.";
        case CURLE_SSL_CONNECT_ERROR: return "Couldn't make a secure connection to the server.";
        case CURLE_WRITE_ERROR: return "Couldn't write the file — is the disk full?";
        case CURLE_RECV_ERROR: case CURLE_PARTIAL_FILE: case CURLE_GOT_NOTHING: return "The connection was lost.";
        case CURLE_COULDNT_RESOLVE_PROXY: return "Couldn't reach the proxy.";
        default: return std::string("Network error: ") + curl_easy_strerror(static_cast<CURLcode>(code));
    }
}

// ---------------------------------------------------------------- probe
namespace {
struct ProbeSink { ResponseInfo* info; };
size_t probeHeader(char* p, size_t s, size_t n, void* u) {
    http::parseHeader(std::string(p, s * n), *static_cast<ProbeSink*>(u)->info);
    return s * n;
}
size_t probeWrite(char*, size_t, size_t, void*) { return 0; }   // got the headers: stop
} // namespace

bool probe(const RequestInfo& r, ResponseInfo& out, std::string* error) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    http::setup(c, r);
    struct curl_slist* hl = nullptr;
    for (auto& kv : r.headers) hl = curl_slist_append(hl, (kv.first + ": " + kv.second).c_str());
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_RANGE, "0-");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 40L);
    out = ResponseInfo();
    ProbeSink sink{&out};
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probeHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probeWrite);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    char* eff = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) out.finalUrl = eff;
    if (out.total < 0) {
        curl_off_t cl = -1;
        if (curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK && cl >= 0) out.total = cl;
    }
    if (status == 206) out.ranges = true;
    if (urlScheme(r.url) == "ftp") { out.ranges = true; out.status = status ? status : 200; }
    if (out.status == 0) out.status = status;
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    bool ok = (rc == CURLE_OK || rc == CURLE_WRITE_ERROR) && (status < 400);
    if (!ok && error) *error = friendlyError(rc == CURLE_WRITE_ERROR ? 0 : rc, status);
    return ok;
}

// ---------------------------------------------------------------- extra connections' daily allowance (Options → Connection)
namespace {
std::mutex g_boostMu;
int g_boostDay = 0;
int64_t g_boostBytes = 0;
int today() { SYSTEMTIME n; GetLocalTime(&n); return n.wYear * 10000 + n.wMonth * 100 + n.wDay; }
void boostLoad() {
    if (g_boostDay) return;
    std::string s;
    readFile(joinPath(dataDir(), "boost.txt"), s);
    auto p = split(trim(s), ' ');
    g_boostDay = p.size() == 2 ? atoi(p[0].c_str()) : today();
    g_boostBytes = p.size() == 2 ? _atoi64(p[1].c_str()) : 0;
}
void boostAdd(int64_t n) {
    std::lock_guard<std::mutex> g(g_boostMu);
    boostLoad();
    if (g_boostDay != today()) { g_boostDay = today(); g_boostBytes = 0; }
    int64_t before = g_boostBytes;
    g_boostBytes += n;
    if (before / (8 << 20) != g_boostBytes / (8 << 20)) writeFileAtomic(joinPath(dataDir(), "boost.txt"), std::to_string(g_boostDay) + " " + std::to_string(g_boostBytes));
}
bool boostAllowed() {
    int cap = settings().boostCapMb;
    if (cap <= 0) return true;
    std::lock_guard<std::mutex> g(g_boostMu);
    boostLoad();
    if (g_boostDay != today()) { g_boostDay = today(); g_boostBytes = 0; }
    return g_boostBytes < static_cast<int64_t>(cap) * 1024 * 1024;
}
} // namespace

// ---------------------------------------------------------------- the downloader
namespace {

const int64_t kMinPart = 256 * 1024;   // never split below this

std::mutex g_wantMu;
std::map<std::string, std::pair<int64_t, int64_t>> g_want;   // task id -> (byte, when asked)
int64_t wanted(const std::string& id) {
    std::lock_guard<std::mutex> g(g_wantMu);
    auto it = g_want.find(id);
    if (it == g_want.end()) return -1;
    if (nowMs() - it->second.second > 20000) { g_want.erase(it); return -1; }
    return it->second.first;
}

class Download;

struct Conn {
    Download* d = nullptr;
    CURL* h = nullptr;
    curl_slist* hdrs = nullptr;
    int seg = -1;
    ResponseInfo info;
    int64_t requestedFrom = 0;
    int64_t requestedTo = -1;    // asked for a piece only (maxRequest): the rest of the part is asked next
    bool checked = false;        // first body bytes validated
    bool finishedSeg = false;    // stopped on purpose at the segment's end
    bool rangeIgnored = false;   // server answered 200 to a range request
    bool fatal = false;          // disk error, file changed on the server
    bool paused = false;
    bool first = false;          // learns size and resume support
    std::string ip;
    bool extra = false;          // over one of the extra connections (counts toward the daily allowance)
};

class Download {
public:
    explicit Download(DownloadContext& c) : ctx(c) {}
    ~Download() { cleanup(); }
    DownloadResult run();
    size_t onWrite(Conn* c, const char* p, size_t len);

private:
    DownloadContext& ctx;
    Task snap;                       // copy of the task's request fields
    std::vector<Segment> segs;
    std::vector<int> owner;          // segment -> connection slot, -1 = free
    std::vector<std::unique_ptr<Conn>> conns;
    CURLM* multi = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    int64_t total = -1;
    bool ranges = false, rangesKnown = false;
    int maxConns = 8;
    std::vector<NetAdapter> adapters;
    int nextAdapter = 0;
    std::deque<std::pair<int64_t, int>> retries;  // (when, segment)
    int failuresInRow = 0;
    long lastStatus = 0, lastCode = 0;
    std::string fatalError;
    SpeedMeter meter;                // how fast it is going, smooth enough to draw
    double speed = 0;
    bool restartSingle = false;
    double ownTokens = 0;            // this download's own limit (bytes), refilled every loop
    int64_t ownRefill = 0;
    int64_t ownLimit() { std::lock_guard<std::mutex> g(*ctx.mu); return static_cast<int64_t>(ctx.task->limitKBps) * 1024; }
    int64_t cachedOwnLimit = 0;
    std::vector<char> xbuf;
    int renewals = 0;
    int wantConns = 8;           // what Options asks for; maxConns drops when the server refuses more and climbs back
    int64_t lastCut = 0;
    int validateRounds = 0;      // segment checks before the file is finished

    void cleanup();
    bool openFile(bool fresh);
    void setSparseSize(int64_t size);
    bool startConn(int seg);
    void finishConn(Conn* c, CURLcode rc);
    void removeConn(Conn* c);
    int findWork();
    int validateSegments();
    bool allDone() const;
    int64_t doneBytes() const;
    void sync(const char* status);
};

size_t writeCb(char* p, size_t s, size_t n, void* u) {
    auto c = static_cast<Conn*>(u);
    return c->d->onWrite(c, p, s * n);
}

size_t headerCb(char* p, size_t s, size_t n, void* u) {
    http::parseHeader(std::string(p, s * n), static_cast<Conn*>(u)->info);
    return s * n;
}

void Download::cleanup() {
    for (auto& c : conns) if (c) removeConn(c.get());
    conns.clear();
    if (multi) { curl_multi_cleanup(multi); multi = nullptr; }
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
}

bool Download::openFile(bool fresh) {
    std::string part = snap.partPath();
    makeDirs(dirName(part));
    file = CreateFileW(W(part).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, fresh ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { fatalError = "Couldn't create the file in " + dirName(part) + "."; return false; }
    return true;
}

// Sparse: Windows doesn't fill the gap with zeros when a part far into a big file is written first.
void Download::setSparseSize(int64_t size) {
    if (file == INVALID_HANDLE_VALUE || size <= 0) return;
    DWORD r = 0;
    DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &r, nullptr);
    LARGE_INTEGER li; li.QuadPart = size;
    LARGE_INTEGER cur{};
    LARGE_INTEGER zero{};
    SetFilePointerEx(file, zero, &cur, FILE_END);
    if (cur.QuadPart < size) { SetFilePointerEx(file, li, nullptr, FILE_BEGIN); SetEndOfFile(file); }
}

bool Download::startConn(int seg) {
    if (seg < 0 || seg >= static_cast<int>(segs.size()) || owner[static_cast<size_t>(seg)] >= 0 || segs[static_cast<size_t>(seg)].done()) return false;
    auto c = std::make_unique<Conn>();
    c->d = this;
    c->seg = seg;
    c->h = curl_easy_init();
    if (!c->h) return false;
    RequestInfo r;
    r.url = ctx.rangeUrl ? ctx.rangeUrl(segs[static_cast<size_t>(seg)].pos) : ctx.urlNow ? ctx.urlNow() : snap.url; r.referer = snap.pageUrl; r.cookies = snap.cookies; r.userAgent = snap.userAgent; r.headers = snap.headers;
    if (!adapters.empty()) {
        int n = boostAllowed() ? static_cast<int>(adapters.size()) : 1;   // allowance used up: only the main connection
        c->ip = adapters[static_cast<size_t>(nextAdapter++ % n)].ip;
        c->extra = c->ip != adapters[0].ip;
    }
    http::setup(c->h, r, c->ip);
    for (auto& kv : snap.headers) c->hdrs = curl_slist_append(c->hdrs, (kv.first + ": " + kv.second).c_str());
    if (c->hdrs) curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, c->hdrs);
    const Segment& s = segs[static_cast<size_t>(seg)];
    c->requestedFrom = s.pos;
    c->first = !rangesKnown;
    if (!ctx.rangeUrl && rangesKnown && ranges && ctx.maxRequest > 0) {   // a piece of the part at a time
        int64_t to = s.pos + ctx.maxRequest - 1;
        if (s.end >= 0) to = std::min(to, s.end);
        c->requestedTo = to;
        curl_easy_setopt(c->h, CURLOPT_RANGE, (std::to_string(s.pos) + "-" + std::to_string(to)).c_str());
    } else if (!ctx.rangeUrl && (s.pos > 0 || !rangesKnown)) curl_easy_setopt(c->h, CURLOPT_RANGE, (std::to_string(s.pos) + "-").c_str());
    curl_easy_setopt(c->h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c->h, CURLOPT_WRITEDATA, c.get());
    curl_easy_setopt(c->h, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(c->h, CURLOPT_HEADERDATA, c.get());
    curl_easy_setopt(c->h, CURLOPT_PRIVATE, c.get());
    if (curl_multi_add_handle(multi, c->h) != CURLM_OK) { curl_easy_cleanup(c->h); curl_slist_free_all(c->hdrs); return false; }
    // slot = index in conns
    for (size_t i = 0; i <= conns.size(); i++) {
        if (i == conns.size()) { conns.push_back(std::move(c)); owner[static_cast<size_t>(seg)] = static_cast<int>(i); break; }
        if (!conns[i]) { conns[i] = std::move(c); owner[static_cast<size_t>(seg)] = static_cast<int>(i); break; }
    }
    return true;
}

void Download::removeConn(Conn* c) {
    if (!c || !c->h) return;
    curl_multi_remove_handle(multi, c->h);
    curl_easy_cleanup(c->h);
    curl_slist_free_all(c->hdrs);
    c->h = nullptr;
    c->hdrs = nullptr;
}

size_t Download::onWrite(Conn* c, const char* p, size_t len) {
    if (c->seg < 0) return 0;
    if (!c->checked) {
        c->checked = true;
        if (!c->info.digest.empty() && !c->info.digestOfPart && !ctx.transform) {   // the server says what the whole file must be
            std::lock_guard<std::mutex> g(*ctx.mu);
            if (ctx.task->checksum.empty()) ctx.task->checksum = c->info.digest;
        }
        long status = c->info.status;
        if (status == 0) curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &status);
        bool ftp = urlScheme(snap.url) == "ftp";
        if (!ftp && status >= 400) { lastStatus = status; return 0; }
        if (!ftp && !ctx.rangeUrl && c->requestedFrom > 0 && status != 206) { c->rangeIgnored = true; return 0; }
        int64_t t = c->info.total;
        if (t < 0) { curl_off_t cl = -1; if (curl_easy_getinfo(c->h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK && cl >= 0) t = cl + c->requestedFrom; }
        if (status != 206 && c->info.total >= 0 && c->requestedFrom == 0) t = c->info.total;
        if (c->first && !rangesKnown) {
            rangesKnown = true;
            ranges = ftp || status == 206 || (status == 200 && c->info.ranges);
            if (total >= 0 && t >= 0 && t != total) { fatalError = "The file changed on the server — start it again."; c->fatal = true; return 0; }
            if (t >= 0) total = t;
            if (total > 0 && segs.size() == 1 && segs[0].end < 0) segs[0].end = total - 1;
            setSparseSize(total);
            std::lock_guard<std::mutex> g(*ctx.mu);
            ctx.task->total = total;
            ctx.task->resumable = ranges && total > 0;
        } else if (total >= 0 && status == 206 && c->info.total >= 0 && c->info.total != total) {
            fatalError = "The file changed on the server — start it again."; c->fatal = true; return 0;
        }
    }
    if (cachedOwnLimit > 0) {
        if (ownTokens <= 0) { c->paused = true; return CURL_WRITEFUNC_PAUSE; }
        ownTokens -= static_cast<double>(len);
    }
    if (limited() && !takeTokens(len)) { c->paused = true; return CURL_WRITEFUNC_PAUSE; }
    Segment& s = segs[static_cast<size_t>(c->seg)];
    size_t room = len;
    if (s.end >= 0) {
        int64_t left = s.end - s.pos + 1;
        if (left <= 0) { c->finishedSeg = true; return 0; }
        if (static_cast<int64_t>(room) > left) room = static_cast<size_t>(left);
    }
    if (ctx.transform) {
        xbuf.assign(p, p + room);
        ctx.transform(s.pos, xbuf.data(), room);
        p = xbuf.data();
    }
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(s.pos & 0xFFFFFFFF);
    ov.OffsetHigh = static_cast<DWORD>(s.pos >> 32);
    DWORD put = 0;
    if (!WriteFile(file, p, static_cast<DWORD>(room), &put, &ov) || put != room) {
        DWORD e = GetLastError();
        fatalError = e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL ? "The disk is full." : "Couldn't write the file (error " + std::to_string(e) + ").";
        c->fatal = true;
        return 0;
    }
    s.pos += static_cast<int64_t>(room);
    meter.add(static_cast<int64_t>(room));
    g_received += static_cast<int64_t>(room);
    if (c->extra) boostAdd(static_cast<int64_t>(room));
    failuresInRow = 0;
    renewals = 0;
    if (s.end >= 0 && s.pos > s.end) { c->finishedSeg = true; return 0; }   // stop here; the rest belongs to another part
    return len;
}

int Download::findWork() {
    int64_t want = wanted(snap.id);   // (play while downloading)
    if (want >= 0) {
        // 0) the part where the player is waiting: an idle one that holds it, or one split right there
        for (size_t i = 0; i < segs.size(); i++)
            if (owner[i] < 0 && !segs[i].done() && segs[i].pos <= want && (segs[i].end < 0 || want <= segs[i].end)) return static_cast<int>(i);
        if (ranges && total > 0)
            for (size_t i = 0; i < segs.size(); i++) {
                Segment& b = segs[i];
                if (b.done() || b.end < 0 || want < b.pos || want > b.end) continue;
                if (want - b.pos < 2 * 1024 * 1024 || b.end - want < kMinPart) break;   // its connection gets there soon anyway
                int64_t at = std::max(b.pos + kMinPart, want & ~static_cast<int64_t>(16383));
                Segment n;
                n.start = at; n.end = b.end; n.pos = at;
                b.end = at - 1;
                segs.push_back(n);
                owner.push_back(-1);
                return static_cast<int>(segs.size() - 1);
            }
    }
    // 1) a part nobody is working on
    for (size_t i = 0; i < segs.size(); i++) {
        if (owner[i] >= 0 || segs[i].done()) continue;
        bool waiting = false;
        for (auto& r : retries) if (r.second == static_cast<int>(i)) waiting = true;
        if (!waiting) return static_cast<int>(i);
    }
    if (!ranges || total <= 0) return -1;
    // 2) take half of the biggest part still being downloaded
    int best = -1;
    int64_t bestLeft = 2 * kMinPart;
    for (size_t i = 0; i < segs.size(); i++) {
        if (segs[i].done() || segs[i].end < 0) continue;
        int64_t left = segs[i].left();
        if (left > bestLeft) { bestLeft = left; best = static_cast<int>(i); }
    }
    if (best < 0) return -1;
    Segment& b = segs[static_cast<size_t>(best)];
    int64_t mid = b.pos + bestLeft / 2;
    Segment n;
    n.start = mid; n.end = b.end; n.pos = mid;
    b.end = mid - 1;
    segs.push_back(n);
    owner.push_back(-1);
    return static_cast<int>(segs.size() - 1);
}

namespace {
size_t sampleCb(char* p, size_t sz, size_t n, void* u) { auto* s = static_cast<std::string*>(u); if (s->size() + sz * n > 4096) return 0; s->append(p, sz * n); return sz * n; }
}

int Download::validateSegments() {
    if (total <= 0 || segs.size() < 2) return 0;
    // 1) together the parts cover the file exactly: a gap becomes a new part to download
    std::vector<size_t> order(segs.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return segs[a].start < segs[b].start; });
    int64_t covered = 0;
    int gaps = 0;
    std::vector<int64_t> joins;
    for (size_t i : order) {
        const Segment& x = segs[i];
        if (x.start > covered) { Segment g; g.start = covered; g.end = x.start - 1; g.pos = covered; segs.push_back(g); owner.push_back(-1); gaps++; }
        else if (x.start > 0 && x.start == covered) joins.push_back(x.start);
        covered = std::max(covered, x.end + 1);
    }
    if (covered < total) { Segment g; g.start = covered; g.end = total - 1; g.pos = covered; segs.push_back(g); owner.push_back(-1); gaps++; }
    if (gaps) { logLine("segments: " + std::to_string(gaps) + " missing piece(s) downloaded again"); return gaps; }
    // 2) where two parts meet, the server still sends the same bytes as the file has (64 bytes across each join)
    if (!ranges || ctx.transform || ctx.rangeUrl || file == INVALID_HANDLE_VALUE || joins.empty()) return 0;
    std::vector<int64_t> pick;
    size_t want = std::min<size_t>(32, joins.size());
    for (size_t k = 0; k < want; k++) pick.push_back(joins[k * joins.size() / want]);
    CURLM* m = curl_multi_init();
    std::vector<CURL*> hs(pick.size(), nullptr);
    std::vector<std::string> got(pick.size());
    std::vector<curl_slist*> hl(pick.size(), nullptr);
    for (size_t k = 0; k < pick.size(); k++) {
        CURL* h = curl_easy_init();
        if (!h) continue;
        RequestInfo r; r.url = snap.url; r.referer = snap.pageUrl; r.cookies = snap.cookies; r.userAgent = snap.userAgent;
        http::setup(h, r);
        for (auto& kv : snap.headers) hl[k] = curl_slist_append(hl[k], (kv.first + ": " + kv.second).c_str());
        if (hl[k]) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl[k]);
        int64_t a = std::max<int64_t>(0, pick[k] - 32), b = std::min<int64_t>(total - 1, pick[k] + 31);
        curl_easy_setopt(h, CURLOPT_RANGE, (std::to_string(a) + "-" + std::to_string(b)).c_str());
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, sampleCb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &got[k]);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 20L);
        curl_multi_add_handle(m, h);
        hs[k] = h;
    }
    for (int running = 1; running;) {
        if (ctx.stop && ctx.stop->load()) break;
        curl_multi_perform(m, &running);
        if (running) curl_multi_poll(m, nullptr, 0, 200, nullptr);
    }
    int bad = 0;
    for (size_t k = 0; k < pick.size(); k++) {
        if (!hs[k]) continue;
        long status = 0;
        curl_easy_getinfo(hs[k], CURLINFO_RESPONSE_CODE, &status);
        int64_t a = std::max<int64_t>(0, pick[k] - 32), b = std::min<int64_t>(total - 1, pick[k] + 31);
        if (status == 206 && static_cast<int64_t>(got[k].size()) == b - a + 1) {   // a sample that couldn't be fetched isn't held against the file
            std::string mine(got[k].size(), '\0');
            LARGE_INTEGER li; li.QuadPart = a;
            DWORD rd = 0;
            if (SetFilePointerEx(file, li, nullptr, FILE_BEGIN) && ReadFile(file, &mine[0], static_cast<DWORD>(mine.size()), &rd, nullptr) && rd == mine.size() && mine != got[k]) {
                for (auto& x : segs) if ((x.end >= a && x.start <= b)) { x.pos = x.start; bad++; }   // both parts that meet here
            }
        }
        curl_multi_remove_handle(m, hs[k]);
        curl_easy_cleanup(hs[k]);
        if (hl[k]) curl_slist_free_all(hl[k]);
    }
    curl_multi_cleanup(m);
    if (bad) logLine("segments: " + std::to_string(bad) + " part(s) disagreed with the server and are downloaded again");
    return bad;
}

bool Download::allDone() const {
    for (auto& s : segs) if (!s.done()) return false;
    return !segs.empty();
}

int64_t Download::doneBytes() const {
    int64_t d = 0;
    for (auto& s : segs) d += std::max<int64_t>(0, (s.end >= 0 ? std::min(s.pos, s.end + 1) : s.pos) - s.start);
    return d;
}

void Download::sync(const char* status) {
    int active = 0;
    for (auto& c : conns) if (c && c->h) active++;
    std::lock_guard<std::mutex> g(*ctx.mu);
    Task& t = *ctx.task;
    t.segments = segs;
    t.done = doneBytes();
    if (total >= 0) t.total = total;
    t.speed = speed;
    t.eta = (speed > 1 && total > 0) ? static_cast<int64_t>((total - t.done) / speed) : -1;
    t.activeConnections = active;
    if (status) t.status = status;
}

void Download::finishConn(Conn* c, CURLcode rc) {
    long status = 0;
    curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, &status);
    int seg = c->seg;
    bool ok = false;
    if (c->fatal) { /* fatalError set */ }
    else if (c->rangeIgnored) { restartSingle = true; }
    else if (c->finishedSeg || (seg >= 0 && segs[static_cast<size_t>(seg)].done())) ok = true;
    else if (rc == CURLE_OK && c->requestedTo >= 0 && seg >= 0 && segs[static_cast<size_t>(seg)].pos > c->requestedTo) {
        ok = true;   // its piece arrived: the next piece of the same part right away
        retries.push_front({0, seg});
    }
    else if (rc == CURLE_OK && seg >= 0 && segs[static_cast<size_t>(seg)].end < 0) {   // unknown size: the end of the stream is the end of the file
        Segment& s = segs[static_cast<size_t>(seg)];
        s.end = s.pos - 1;
        total = s.pos;
        ok = true;
    }
    if (!ok && !c->fatal && !c->rangeIgnored) {
        lastCode = rc;
        if (status >= 400) lastStatus = status;
        failuresInRow++;
        bool permanent = status == 401 || status == 403 || status == 404 || status == 410 || status == 416 || rc == CURLE_PEER_FAILED_VERIFICATION;
        if (ctx.renew && (status == 403 || status == 404 || status == 410 || status == 509)) {
            if (status == 509 && ctx.statusError) fatalError = ctx.statusError(509);
            else if (renewals < 6) {
                int rn = ctx.renew();
                if (rn == 1) { renewals++; failuresInRow = 0; }   // a new address: try again at once
                if (rn) permanent = false;                         // (2: another connection renewed it; waits grow, so a dead address isn't hammered)
                else if (ctx.statusError) fatalError = ctx.statusError(status);
            }
            else if (ctx.statusError) fatalError = ctx.statusError(status);
        }
        // the server limits connections per user (429, 503, refused while others work): use fewer, try more later
        if ((status == 429 || status == 503 || rc == CURLE_COULDNT_CONNECT || rc == CURLE_GOT_NOTHING || rc == CURLE_RECV_ERROR) && maxConns > 1) {
            int alive = 0;
            for (auto& o : conns) if (o && o.get() != c && o->h) alive++;
            if (alive > 0 && alive < maxConns) { maxConns = alive; lastCut = nowMs(); }
        }
        if (!c->ip.empty() && (rc == CURLE_INTERFACE_FAILED || rc == CURLE_COULDNT_CONNECT)) {   // this adapter can't reach the server: stop using it
            adapters.erase(std::remove_if(adapters.begin(), adapters.end(), [&](const NetAdapter& a) { return a.ip == c->ip; }), adapters.end());
            permanent = false;
        }
        bool anyProgress = doneBytes() > 0;
        if (permanent && !anyProgress) fatalError = friendlyError(rc, status);
        else if (permanent && status >= 400) {
            // other connections may still be fine; only fail when nobody is downloading
            int others = 0;
            for (auto& o : conns) if (o && o.get() != c && o->h) others++;
            if (others == 0) fatalError = friendlyError(rc, status);
        }
        if (fatalError.empty() && seg >= 0) {
            if (ok == false && renewals > 0 && failuresInRow == 0) failuresInRow = 1;
            int64_t wait = std::min<int64_t>(30000, 1000LL * failuresInRow);
            retries.push_back({nowMs() + wait, seg});
        }
    }
    if (seg >= 0 && seg < static_cast<int>(owner.size())) owner[static_cast<size_t>(seg)] = -1;
    // free the slot
    for (auto& p : conns) if (p.get() == c) { removeConn(c); p.reset(); break; }
}

DownloadResult Download::run() {
    {
        std::lock_guard<std::mutex> g(*ctx.mu);
        snap = *ctx.task;
    }
    if (links::isDrive(snap.url)) snap.url = links::resolveDrive(snap.url, snap.cookies);   // Google Drive: the file itself, past the warning page
    const Settings& st = settings();
    maxConns = std::max(1, std::min(32, snap.connections > 0 ? snap.connections : st.connections));
    wantConns = maxConns;
    total = snap.total;
    segs = snap.segments;
    bool fresh = segs.empty() || !fileExists(snap.partPath());
    if (!fresh && !snap.resumable) fresh = true;   // the server can't continue: start over
    if (fresh) { segs.clear(); segs.push_back(Segment{0, -1, 0}); total = -1; }   // the first answer sets the size
    rangesKnown = !fresh && snap.resumable;
    ranges = rangesKnown;
    if (fresh && ctx.knownTotal >= 0) {   // size known up front (MEGA): all connections start together
        total = ctx.knownTotal;
        segs.assign(1, Segment{0, total - 1, 0});
        rangesKnown = ranges = true;
        std::lock_guard<std::mutex> g(*ctx.mu);
        ctx.task->total = total;
        ctx.task->resumable = true;
    }
    owner.assign(segs.size(), -1);
    if (!openFile(fresh)) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->error = fatalError; return DownloadResult::Failed; }
    if (fresh && ctx.knownTotal > 0) setSparseSize(total);
    if (st.useAllAdapters) {
        auto all = internetAdapters();
        for (auto& a : all) if (std::find(st.excludedAdapters.begin(), st.excludedAdapters.end(), a.name) == st.excludedAdapters.end()) adapters.push_back(a);
        if (adapters.size() < 2) adapters.clear();   // one adapter: let Windows route
        else logLine("download " + snap.id + ": using " + std::to_string(adapters.size()) + " network adapters");
    }
    multi = curl_multi_init();
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 0L);
    sync("Connecting…");
    if (ctx.changed) ctx.changed();

    // Start: one connection learns size and resume support; resuming parts start together.
    if (rangesKnown) { for (int i = 0, n = 0; i < static_cast<int>(segs.size()) && n < maxConns; i++) if (!segs[static_cast<size_t>(i)].done() && startConn(i)) n++; }
    else for (int i = 0; i < static_cast<int>(segs.size()); i++) if (!segs[static_cast<size_t>(i)].done()) { startConn(i); break; }

    meter.start(nowMs());
    int64_t lastSync = 0;
    DownloadResult result = DownloadResult::Failed;
    while (true) {
        if (ctx.stop && ctx.stop->load()) { result = DownloadResult::Stopped; break; }
        int running = 0;
        curl_multi_perform(multi, &running);
        int q = 0;
        while (CURLMsg* m = curl_multi_info_read(multi, &q)) {
            if (m->msg != CURLMSG_DONE) continue;
            Conn* c = nullptr;
            curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, reinterpret_cast<char**>(&c));
            if (c) finishConn(c, m->data.result);
        }
        if (!fatalError.empty()) break;
        if (restartSingle) {   // the server ignores ranges: one connection from the start
            restartSingle = false;
            for (auto& c : conns) if (c) { removeConn(c.get()); c.reset(); }
            CloseHandle(file); file = INVALID_HANDLE_VALUE;
            segs.assign(1, Segment{0, -1, 0});
            owner.assign(1, -1);
            ranges = false; rangesKnown = true; total = -1;
            { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->resumable = false; ctx.task->total = -1; }
            if (!openFile(true)) break;
            startConn(0);
        }
        if (allDone()) {
            // every part is checked before the file is finished: no gaps or overlaps, and the server still sends the
            // same bytes where two parts meet; a part that disagrees is downloaded again
            int bad = validateSegments();
            if (bad < 0) break;                       // fatalError explains
            if (bad > 0 && validateRounds++ < 2) continue;
            if (bad > 0) { fatalError = "The server keeps sending different data for the same file. Restart it from the beginning."; break; }
            result = DownloadResult::Done; break;
        }

        int64_t now = nowMs();
        refillTokens();
        cachedOwnLimit = ownLimit();
        if (cachedOwnLimit > 0) {
            if (ownRefill == 0) ownRefill = now;
            ownTokens = std::min(ownTokens + cachedOwnLimit * (now - ownRefill) / 1000.0, cachedOwnLimit / 4.0 + 1);
            ownRefill = now;
        } else { ownTokens = 0; ownRefill = 0; }
        for (auto& c : conns) if (c && c->h && c->paused) { c->paused = false; curl_easy_pause(c->h, CURLPAUSE_CONT); }
        // retries that are due
        while (!retries.empty() && retries.front().first <= now) {
            int seg = retries.front().second;
            retries.pop_front();
            if (static_cast<int>(segs.size()) > seg && !segs[static_cast<size_t>(seg)].done() && owner[static_cast<size_t>(seg)] < 0) startConn(seg);
        }
        // connections cut because the server refused: one more every 20 s while everything goes well
        if (maxConns < wantConns && lastCut && now - lastCut > 20000 && failuresInRow == 0) { maxConns++; lastCut = now; }
        // more connections, once the server said it can do ranges
        int active = 0;
        for (auto& c : conns) if (c && c->h) active++;
        if (rangesKnown && ranges) {
            while (active < maxConns) {
                int w = findWork();
                if (w < 0 || !startConn(w)) break;
                active++;
            }
        }
        if (active == 0 && retries.empty() && !allDone()) {
            if (!rangesKnown || !ranges) { // single stream ended early
                if (failuresInRow > (st.autoRetry ? st.retries : 1)) { fatalError = friendlyError(lastCode, lastStatus); break; }
            }
            int w = findWork();
            if (w >= 0) startConn(w);
            else if (fatalError.empty()) { fatalError = friendlyError(lastCode, lastStatus); break; }
        }
        if (failuresInRow > (st.autoRetry ? st.retries : 1) * std::max(1, maxConns)) { fatalError = friendlyError(lastCode, lastStatus); break; }
        if (now - lastSync >= 250) {   // the speed is worked out afresh each time, so what is drawn is never stale
            lastSync = now;
            speed = meter.value(now);
            sync(doneBytes() > 0 ? "Receiving data…" : "Connecting…");
            if (ctx.changed) ctx.changed();
        }
        curl_multi_poll(multi, nullptr, 0, 100, nullptr);
    }

    for (auto& c : conns) if (c) { removeConn(c.get()); c.reset(); }
    if (result == DownloadResult::Done && total < 0) total = doneBytes();
    meter.stop();
    speed = 0;
    sync(nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        if (result == DownloadResult::Done && total >= 0) {
            LARGE_INTEGER li; li.QuadPart = total;
            SetFilePointerEx(file, li, nullptr, FILE_BEGIN);
            SetEndOfFile(file);
        }
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
    }
    if (!fatalError.empty()) {
        if (doneBytes() == 0) DeleteFileW(W(snap.partPath()).c_str());   // nothing downloaded: leave no empty file behind
        std::lock_guard<std::mutex> g(*ctx.mu);
        ctx.task->error = fatalError;
        return DownloadResult::Failed;
    }
    if (result == DownloadResult::Done) {
        Task now;
        { std::lock_guard<std::mutex> g(*ctx.mu); now = *ctx.task; }
        if (now.hidden && !now.tempDir.empty()) {   // finished while Download File Info is still open: stays aside until confirmed
            std::lock_guard<std::mutex> g(*ctx.mu);
            ctx.task->total = total;
            ctx.task->done = total;
            return result;
        }
        std::string target = now.path();   // the user may have chosen another name or folder meanwhile
        makeDirs(dirName(target));
        if (fileExists(target)) target = uniquePath(target);
        if (toupper(static_cast<unsigned char>(snap.partPath()[0])) != toupper(static_cast<unsigned char>(target[0]))) {   // another drive: a copy, which takes a moment
            { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->status = "Moving to its folder…"; }
            if (ctx.changed) ctx.changed();
        }
        if (!MoveFileExW(W(snap.partPath()).c_str(), W(target).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::lock_guard<std::mutex> g(*ctx.mu);
            ctx.task->error = "Downloaded, but couldn't rename the file (is it open in another program?).";
            return DownloadResult::Failed;
        }
        std::lock_guard<std::mutex> g(*ctx.mu);
        ctx.task->fileName = baseName(target);
        ctx.task->dir = dirName(target);
        ctx.task->tempDir.clear();
        ctx.task->total = total;
        ctx.task->done = total;
    }
    return result;
}

} // namespace

void wantByte(const std::string& taskId, int64_t offset) {
    std::lock_guard<std::mutex> g(g_wantMu);
    if (offset < 0) g_want.erase(taskId); else g_want[taskId] = {offset, nowMs()};
}

DownloadResult runHttpDownload(DownloadContext& ctx) {
    Download d(ctx);
    return d.run();
}

} // namespace ddm
