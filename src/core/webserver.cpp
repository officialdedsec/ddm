#include "webserver.hpp"
#include "../common/util.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ddm {
namespace web {

namespace {
std::once_flag g_wsa;
void wsa() { std::call_once(g_wsa, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }); }
std::atomic<int> g_open{0};

bool sendAll(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = ::send(s, p, static_cast<int>(std::min<size_t>(n, 64 * 1024)), 0);
        if (k <= 0) return false;
        p += k; n -= static_cast<size_t>(k);
    }
    return true;
}

void serve(SOCKET s, std::string peer, std::function<void(const Request&, Response&)> handler) {
    struct Close { SOCKET s; ~Close() { shutdown(s, SD_SEND); closesocket(s); g_open--; } } closer{s};
    DWORD tmo = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    std::string buf;
    char tmp[8192];
    size_t headEnd = std::string::npos;
    ULONGLONG began = GetTickCount64();   // a client that trickles its request in byte by byte can't hold a slot for long
    while (headEnd == std::string::npos) {
        if (GetTickCount64() - began > 30000) return;
        int k = recv(s, tmp, sizeof tmp, 0);
        if (k <= 0) return;
        buf.append(tmp, static_cast<size_t>(k));
        if (buf.size() > 64 * 1024) return;
        headEnd = buf.find("\r\n\r\n");
    }
    Request rq;
    rq.peer = peer;
    std::string head = buf.substr(0, headEnd);
    auto lines = split(head, '\n');
    if (lines.empty()) return;
    auto first = split(trim(lines[0]), ' ');
    if (first.size() < 2) return;
    rq.method = first[0];
    std::string target = first[1];
    size_t q = target.find('?');
    rq.path = urlDecode(target.substr(0, q));
    if (q != std::string::npos) rq.query = target.substr(q + 1);
    for (size_t i = 1; i < lines.size(); i++) {
        std::string l = trim(lines[i]);
        size_t c = l.find(':');
        if (c != std::string::npos) rq.headers[lower(trim(l.substr(0, c)))] = trim(l.substr(c + 1));
    }
    size_t want = 0;
    if (rq.headers.count("content-length")) want = static_cast<size_t>(std::max(0LL, _atoi64(rq.headers["content-length"].c_str())));
    if (want > 64 * 1024) return;
    rq.body = buf.substr(headEnd + 4);
    while (rq.body.size() < want) {
        if (GetTickCount64() - began > 30000) return;
        int k = recv(s, tmp, sizeof tmp, 0);
        if (k <= 0) return;
        rq.body.append(tmp, static_cast<size_t>(k));
    }
    if (rq.body.size() > want) rq.body.resize(want);
    Response rs;
    try { handler(rq, rs); } catch (...) { rs = Response(); rs.status = 500; rs.body = "error"; }
    auto sender = [s](const char* p, size_t n) { return sendAll(s, p, n); };
    if (rs.stream) { rs.stream(sender); return; }
    std::string out = "HTTP/1.1 " + std::to_string(rs.status) + " " + statusText(rs.status) + "\r\nContent-Type: " + rs.type + "\r\nContent-Length: " + std::to_string(rs.body.size()) +
                      "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    for (auto& kv : rs.headers) out += kv.first + ": " + kv.second + "\r\n";
    out += "\r\n";
    if (rq.method != "HEAD") out += rs.body;
    sendAll(s, out.data(), out.size());
}
} // namespace

std::string Request::param(const std::string& name) const {
    for (auto& kv : split(query, '&')) {
        size_t e = kv.find('=');
        if (urlDecode(kv.substr(0, e)) == name) return e == std::string::npos ? "" : urlDecode(kv.substr(e + 1));
    }
    return "";
}

bool Server::start(bool bindAll, int port, std::function<void(const Request&, Response&)> handler) {
    stop();
    wsa();
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return false;
    BOOL excl = TRUE;
    setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);   // nobody else can take the port over
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = bindAll ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    if (bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(l, 16) != 0) { closesocket(l); return false; }
    int len = sizeof a;
    getsockname(l, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    listener_ = static_cast<uintptr_t>(l);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    alive_ = alive;
    std::thread([l, alive, handler] {
        while (alive->load()) {
            sockaddr_in p{};
            int pl = sizeof p;
            SOCKET c = accept(l, reinterpret_cast<sockaddr*>(&p), &pl);
            if (c == INVALID_SOCKET) { if (!alive->load()) break; Sleep(50); continue; }
            if (g_open.load() >= 24) { closesocket(c); continue; }   // (too many at once)
            char ip[64] = "";
            inet_ntop(AF_INET, &p.sin_addr, ip, sizeof ip);
            g_open++;
            std::thread(serve, c, std::string(ip), handler).detach();
        }
    }).detach();
    return true;
}

void Server::stop() {
    if (alive_) alive_->store(false);
    if (listener_ != ~static_cast<uintptr_t>(0)) closesocket(static_cast<SOCKET>(listener_));
    listener_ = ~static_cast<uintptr_t>(0);
    port_ = 0;
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) && isxdigit(static_cast<unsigned char>(s[i + 2]))) { o += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
        else o += s[i];
    }
    return o;
}

std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string statusText(int c) {
    switch (c) {
        case 200: return "OK"; case 204: return "No Content"; case 206: return "Partial Content"; case 400: return "Bad Request"; case 403: return "Forbidden";
        case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 416: return "Range Not Satisfiable"; case 429: return "Too Many Requests"; case 503: return "Service Unavailable";
    }
    return "Error";
}

std::vector<std::string> localAddresses() {
    std::vector<std::pair<int, std::string>> found;
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &size) != NO_ERROR) return {};
    }
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        int score = a->FirstGatewayAddress ? 0 : 10;   // a network with a router first (your Wi-Fi / Ethernet)
        if (a->IfType != IF_TYPE_IEEE80211 && a->IfType != IF_TYPE_ETHERNET_CSMACD) score += 5;   // VPNs, virtual adapters last
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sa->sin_family != AF_INET) continue;
            char ip[64] = "";
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
            std::string s = ip;
            if (startsWith(s, "169.254.") || startsWith(s, "127.")) continue;
            found.push_back({score, s});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const std::pair<int, std::string>& x, const std::pair<int, std::string>& y) { return x.first < y.first; });
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

} // namespace web
} // namespace ddm
