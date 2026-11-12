#include "motw.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <urlmon.h>

namespace ddm {
namespace motw {

namespace {
// what may be written: no user name / password, no "#key" (MEGA keys live there), no file:// paths
std::string clean(const std::string& url) {
    std::string u = trim(url);
    std::string low = lower(u);
    if (!(startsWith(low, "http://") || startsWith(low, "https://") || startsWith(low, "ftp://"))) return "";
    size_t hash = u.find('#');
    if (hash != std::string::npos) u = u.substr(0, hash);
    size_t scheme = u.find("://") + 3, slash = u.find('/', scheme), at = u.find('@', scheme);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) u = u.substr(0, scheme) + u.substr(at + 1);
    for (char& c : u) if (c == '\r' || c == '\n') c = ' ';
    return u;
}

// the zone Windows gives the address (Internet 3, Local intranet 1, Trusted 2, Restricted 4) with its own settings
DWORD zoneOf(const std::string& url) {
    DWORD zone = 3;
    if (url.empty()) return zone;
    IInternetSecurityManager* sm = nullptr;
    bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    if (SUCCEEDED(CoInternetCreateSecurityManager(nullptr, &sm, 0)) && sm) {
        DWORD z = 0;
        if (SUCCEEDED(sm->MapUrlToZone(W(url).c_str(), &z, 0))) zone = z;
        sm->Release();
    }
    if (co) CoUninitialize();
    return zone;
}

void markFile(const std::string& path, const std::string& body) {
    HANDLE h = CreateFileW(W(path + ":Zone.Identifier").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // FAT32 / exFAT / network shares without streams: nothing to mark
    DWORD w = 0;
    WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &w, nullptr);
    CloseHandle(h);
}

void walk(const std::string& dir, const std::string& body, int depth) {
    if (depth > 32) return;
    for (auto& n : listDir(dir)) {
        std::string p = joinPath(dir, n);
        DWORD a = GetFileAttributesW(W(p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (a & FILE_ATTRIBUTE_DIRECTORY) walk(p, body, depth + 1);
        else markFile(p, body);
    }
}
} // namespace

void mark(const std::string& path, const std::string& url, const std::string& referrer) {
    std::string host = clean(url), ref = clean(referrer);
    DWORD zone = host.empty() ? 3 : zoneOf(host);
    if (zone == 0) return;   // this computer: browsers don't mark those either
    std::string body = "[ZoneTransfer]\r\nZoneId=" + std::to_string(zone) + "\r\n";
    if (!ref.empty()) body += "ReferrerUrl=" + ref + "\r\n";
    body += "HostUrl=" + (host.empty() ? std::string("about:internet") : host) + "\r\n";
    DWORD a = GetFileAttributesW(W(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (a & FILE_ATTRIBUTE_DIRECTORY) walk(path, body, 0);
    else markFile(path, body);
}

bool isMarked(const std::string& path) {
    DWORD a = GetFileAttributesW(W(path + ":Zone.Identifier").c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

} // namespace motw
} // namespace ddm
