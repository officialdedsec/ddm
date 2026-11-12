#include "preview.hpp"
#include "crypto.hpp"
#include "downloader.hpp"
#include "engine.hpp"
#include "webserver.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <mutex>
#include <set>
#include <map>
#include <vector>
#include <algorithm>

namespace ddm {
namespace preview {

namespace {
std::mutex g_mu;
web::Server g_server;
std::string g_key;

std::string mimeOf(const std::string& ext) {
    static const std::map<std::string, std::string> m = {
        {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"mov", "video/quicktime"}, {"webm", "video/webm"}, {"mkv", "video/x-matroska"}, {"avi", "video/x-msvideo"},
        {"wmv", "video/x-ms-wmv"}, {"3gp", "video/3gpp"}, {"ts", "video/mp2t"}, {"flv", "video/x-flv"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"},
        {"ogg", "audio/ogg"}, {"opus", "audio/ogg"}, {"wav", "audio/wav"}, {"flac", "audio/flac"}, {"wma", "audio/x-ms-wma"}};
    auto it = m.find(ext);
    return it == m.end() ? "" : it->second;
}

// bytes ready from `at` on, without a gap
int64_t readyFrom(const Task& t, int64_t at) {
    if (t.state == State::Complete) return t.total > 0 ? std::max<int64_t>(0, t.total - at) : INT64_MAX;
    int64_t best = 0;
    for (auto& s : t.segments) if (s.start <= at && at < s.pos) best = std::max(best, s.pos - at);
    return best;
}

void handle(const web::Request& rq, web::Response& rs) {
    // /play/<key>/<task id>/<name>
    auto parts = split(rq.path, '/');
    if (parts.size() < 4 || parts[1] != "play" || !crypto::sameSecret(parts[2], g_key) || (rq.method != "GET" && rq.method != "HEAD")) { rs.status = 404; rs.body = "not found"; return; }
    std::string id = parts[3];
    Task t;
    if (!Engine::get().find(id, t) || !canPlay(t)) { rs.status = 404; rs.body = "not found"; return; }
    int64_t total = t.total;
    if (total <= 0) { rs.status = 503; rs.body = "size not known yet"; return; }
    int64_t a = 0, b = total - 1;
    bool ranged = false;
    auto r = rq.headers.find("range");
    if (r != rq.headers.end() && startsWith(r->second, "bytes=")) {
        std::string v = r->second.substr(6);
        size_t d = v.find('-');
        if (d != std::string::npos) {
            if (d == 0) { int64_t n = _atoi64(v.c_str() + 1); a = std::max<int64_t>(0, total - n); }
            else { a = _atoi64(v.substr(0, d).c_str()); if (d + 1 < v.size()) b = std::min(total - 1, _atoi64(v.c_str() + d + 1)); }
            ranged = true;
        }
    }
    if (a < 0 || a >= total || b < a) { rs.status = 416; rs.headers["Content-Range"] = "bytes */" + std::to_string(total); rs.body = ""; return; }
    std::string type = mimeOf(lower(extOf(t.fileName)));
    bool head = rq.method == "HEAD";
    rs.stream = [id, a, b, total, ranged, type, head](std::function<bool(const char*, size_t)> send) {
        std::string h = std::string("HTTP/1.1 ") + (ranged ? "206 Partial Content" : "200 OK") + "\r\nContent-Type: " + type + "\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_string(b - a + 1) +
                        (ranged ? "\r\nContent-Range: bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" + std::to_string(total) : "") + "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
        if (!send(h.data(), h.size()) || head) return;
        std::vector<char> buf(256 * 1024);
        int64_t at = a;
        HANDLE f = INVALID_HANDLE_VALUE;
        std::string open;
        int64_t waitedSince = 0;
        while (at <= b) {
            Task t;
            if (!Engine::get().find(id, t)) break;
            int64_t ready = readyFrom(t, at);
            if (ready <= 0) {   // not here yet: the download is asked to fetch this part next
                if (t.state != State::Running && t.state != State::Queued && t.state != State::Complete) break;   // paused / stopped: nothing will come
                if (!waitedSince) { waitedSince = nowMs(); wantByte(id, at); }
                if (nowMs() - waitedSince > 120000) break;
                Sleep(120);
                continue;
            }
            waitedSince = 0;
            std::string path = t.state == State::Complete ? t.path() : t.partPath();
            if (path != open) {
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                open = path;
                if (f == INVALID_HANDLE_VALUE) { Sleep(200); open.clear(); if (!Engine::get().find(id, t)) break; continue; }
            }
            DWORD want = static_cast<DWORD>(std::min<int64_t>({static_cast<int64_t>(buf.size()), ready, b - at + 1}));
            LARGE_INTEGER li;
            li.QuadPart = at;
            DWORD got = 0;
            if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN) || !ReadFile(f, buf.data(), want, &got, nullptr) || got == 0) { CloseHandle(f); f = INVALID_HANDLE_VALUE; open.clear(); Sleep(100); continue; }
            if (!send(buf.data(), got)) break;
            at += got;
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    };
}
} // namespace

bool canPlay(const Task& t) {
    if (mimeOf(lower(extOf(t.fileName))).empty()) return false;
    if (t.state == State::Complete) return fileExists(t.path());
    if (t.total <= 0) return false;
    if (t.kind == "http") return true;
    // a video kept as one file while it downloads (not picture + sound joined at the end, not a stream)
    return t.kind == "video" && !t.mediaUrl.empty() && t.audioUrl.empty() && t.convertTo.empty();
}

std::string urlFor(const std::string& taskId) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_server.running()) {
        if (g_key.empty()) { g_key = crypto::hex(crypto::random(18)); }
        if (!g_server.start(false, 0, handle)) return "";
    }
    Task t;
    std::string name = Engine::get().find(taskId, t) ? t.fileName : "file";
    std::string enc;
    for (unsigned char c : name) { if (isalnum(c) || c == '.' || c == '-' || c == '_') enc += static_cast<char>(c); else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); enc += b; } }
    std::string u = "http://127.0.0.1:" + std::to_string(g_server.port()) + "/play/" + g_key + "/" + taskId + "/" + enc;
    if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0)) logLine("preview (tests): " + u);
    return u;
}

void stop() { std::lock_guard<std::mutex> g(g_mu); g_server.stop(); }

} // namespace preview
} // namespace ddm
