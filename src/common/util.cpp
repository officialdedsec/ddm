#include "util.hpp"
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace ddm {

std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string lower(std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> r;
    size_t st = 0;
    for (size_t i = 0; i <= s.size(); i++) if (i == s.size() || s[i] == sep) { r.push_back(s.substr(st, i - st)); st = i + 1; }
    return r;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    return s;
}

bool readFile(const std::string& path, std::string& out) {
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart > 256LL * 1024 * 1024) { CloseHandle(h); return false; }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    bool ok = sz.QuadPart == 0 || (ReadFile(h, &out[0], static_cast<DWORD>(sz.QuadPart), &got, nullptr) && got == sz.QuadPart);
    CloseHandle(h);
    return ok;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    HANDLE h = CreateFileW(W(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size());
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(W(tmp).c_str()); return false; }
    return MoveFileExW(W(tmp).c_str(), W(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool fileExists(const std::string& path) {
    DWORD a = GetFileAttributesW(W(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::string& path) {
    DWORD a = GetFileAttributesW(W(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool makeDirs(const std::string& path) {
    if (path.empty() || dirExists(path)) return true;
    std::string parent = dirName(path);
    if (!parent.empty() && parent != path) makeDirs(parent);
    return CreateDirectoryW(W(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

int64_t fileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(W(path).c_str(), GetFileExInfoStandard, &d)) return -1;
    return (static_cast<int64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

std::string dirName(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    if (p == std::string::npos) return "";
    if (p == 2 && path.size() > 1 && path[1] == ':') return path.substr(0, 3);
    return path.substr(0, p);
}

std::string baseName(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string extOf(const std::string& name) {
    std::string b = baseName(name);
    size_t p = b.find_last_of('.');
    if (p == std::string::npos || p == 0 || p + 1 >= b.size()) return "";
    return lower(b.substr(p + 1));
}

std::string stem(const std::string& name) {
    std::string b = baseName(name);
    size_t p = b.find_last_of('.');
    return (p == std::string::npos || p == 0) ? b : b.substr(0, p);
}

std::string exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return dirName(U(std::wstring(buf, n)));
}

::HINSTANCE__* systemDll(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    std::wstring full = std::wstring(dir) + L"\\" + name;
    return LoadLibraryW(full.c_str());
}

std::string dataDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    std::string ex = exeDir();
    if (fileExists(joinPath(ex, "portable.txt"))) cached = joinPath(ex, "data");
    else {
        wchar_t buf[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, 0, buf))) cached = joinPath(U(buf), "DDM\\native");
        else cached = joinPath(ex, "data");
    }
    makeDirs(cached);
    return cached;
}

std::string rootDataDir() {
    std::string d = dataDir();
    return endsWith(lower(d), "\\native") ? dirName(d) : d;
}

std::string downloadsDir() {
    PWSTR p = nullptr;
    std::string r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) { r = U(p); }
    if (p) CoTaskMemFree(p);
    if (r.empty()) {
        wchar_t buf[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, buf))) r = joinPath(U(buf), "Downloads");
    }
    return r;
}

std::string sanitizeFileName(const std::string& name) {
    std::string o;
    for (unsigned char c : name) {
        if (c < 32 || c == 127 || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) o += '_';
        else o += static_cast<char>(c);
    }
    // invisible direction marks hide a program's real extension ("photo\u202Egpj.exe" shows as "photoexe.jpg")
    for (const char* bidi : {"\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAC", "\xE2\x80\xAD", "\xE2\x80\xAE", "\xE2\x81\xA6", "\xE2\x81\xA7", "\xE2\x81\xA8", "\xE2\x81\xA9", "\xE2\x80\x8E", "\xE2\x80\x8F"})
        o = replaceAll(o, bidi, "");
    o = trim(o);
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    if (o.empty() || o == "." || o == "..") o = "download";
    // device names stay device names whatever follows the first dot ("con.txt", "nul.tar.gz")
    static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9", "conin$", "conout$"};
    std::string first = lower(trim(o.substr(0, o.find('.'))));
    for (auto r : reserved) if (first == r) { o = "_" + o; break; }
    if (o.size() > 200) {   // keep the extension
        std::string e = extOf(o);
        if (e.size() > 20) e.clear();   // not a real extension (a long name with a dot in it)
        size_t cut = 200 - (e.empty() ? 0 : e.size() + 1);
        while (cut > 0 && (static_cast<unsigned char>(o[cut]) & 0xC0) == 0x80) cut--;   // never half a letter (UTF-8)
        o = o.substr(0, cut) + (e.empty() ? "" : "." + e);
    }
    return o;
}

std::string uniquePath(const std::string& path) {
    if (!fileExists(path) && !dirExists(path)) return path;
    std::string d = dirName(path), b = baseName(path), e = extOf(b), s = stem(b);
    for (int i = 2; i < 10000; i++) {
        std::string c = joinPath(d, s + " (" + std::to_string(i) + ")" + (e.empty() ? "" : "." + e));
        if (!fileExists(c) && !dirExists(c)) return c;
    }
    return path;
}

std::string urlScheme(const std::string& url) {
    size_t p = url.find(':');
    return p == std::string::npos ? "" : lower(url.substr(0, p));
}

std::string urlHost(const std::string& url) {
    size_t p = url.find("://");
    if (p == std::string::npos) return "";
    size_t st = p + 3;
    size_t en = url.find_first_of("/?#", st);
    std::string h = url.substr(st, en == std::string::npos ? std::string::npos : en - st);
    size_t at = h.rfind('@');
    if (at != std::string::npos) h = h.substr(at + 1);
    if (!h.empty() && h[0] == '[') { size_t rb = h.find(']'); return lower(h.substr(0, rb == std::string::npos ? h.size() : rb + 1)); }
    size_t colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    return lower(h);
}

std::string urlSite(const std::string& url) {
    std::string h = urlHost(url);
    if (startsWith(h, "www.")) h = h.substr(4);
    return h;
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string fileNameFromUrl(const std::string& url) {
    std::string u = url;
    size_t q = u.find_first_of("?#");
    if (q != std::string::npos) u = u.substr(0, q);
    size_t p = u.find("://");
    if (p != std::string::npos) {
        size_t sl = u.find('/', p + 3);
        if (sl == std::string::npos) return "";
        u = u.substr(sl);
    }
    std::string b = urlDecode(baseName(u));
    return b;
}

bool looksLikeUrl(const std::string& s0) {
    std::string s = lower(trim(s0));
    return startsWith(s, "http://") || startsWith(s, "https://") || startsWith(s, "ftp://") || startsWith(s, "magnet:?");
}

std::vector<std::string> extractUrls(const std::string& text) {
    std::vector<std::string> r;
    std::string low = lower(text);
    size_t i = 0;
    while (i < text.size()) {
        size_t best = std::string::npos;
        for (const char* p : {"https://", "http://", "ftp://", "magnet:?"}) {
            size_t f = low.find(p, i);
            if (f < best) best = f;
        }
        if (best == std::string::npos) break;
        size_t e = best;
        while (e < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[e]);
            if (c <= ' ' || c == '"' || c == '<' || c == '>' || c == '\'' || c == '`') break;
            e++;
        }
        std::string u = text.substr(best, e - best);
        while (!u.empty() && std::string(").,;]}").find(u.back()) != std::string::npos) u.pop_back();
        if (u.size() > 8 && std::find(r.begin(), r.end(), u) == r.end()) r.push_back(u);
        i = e;
    }
    return r;
}

std::string formatBytes(int64_t n) {
    if (n < 0) return "Unknown";
    char b[32];
    if (n < 1024) snprintf(b, sizeof b, "%lld B", static_cast<long long>(n));
    else if (n < 1024LL * 1024) snprintf(b, sizeof b, "%.1f KB", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024) snprintf(b, sizeof b, n < 100LL * 1024 * 1024 ? "%.1f MB" : "%.0f MB", n / 1048576.0);
    else snprintf(b, sizeof b, "%.2f GB", n / 1073741824.0);
    return b;
}

std::string formatSpeed(double bps) {
    if (bps <= 0) return "";
    return formatBytes(static_cast<int64_t>(bps)) + "/s";
}

std::string groupThousands(int64_t n) {
    bool neg = n < 0;
    std::string d = std::to_string(neg ? -n : n), o;
    for (size_t i = 0; i < d.size(); i++) { if (i && (d.size() - i) % 3 == 0) o += ','; o += d[i]; }
    return neg ? "-" + o : o;
}

std::string formatKBps(double bps) {
    if (bps <= 0) return "";
    double kb = bps / 1024.0;
    if (kb >= 100000) return groupThousands(static_cast<int64_t>(kb + 0.5)) + " KB/s";
    int64_t whole = static_cast<int64_t>(kb);
    int tenth = static_cast<int>((kb - static_cast<double>(whole)) * 10 + 0.5);
    if (tenth >= 10) { whole++; tenth = 0; }
    return groupThousands(whole) + "." + std::to_string(tenth) + " KB/s";
}

std::string formatDuration(int64_t s) {
    if (s < 0) return "";
    char b[48];
    if (s < 60) snprintf(b, sizeof b, "%lld s", static_cast<long long>(s));
    else if (s < 3600) snprintf(b, sizeof b, "%lld min %lld s", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
    else if (s < 86400) snprintf(b, sizeof b, "%lld h %lld min", static_cast<long long>(s / 3600), static_cast<long long>((s % 3600) / 60));
    else snprintf(b, sizeof b, "%lld d %lld h", static_cast<long long>(s / 86400), static_cast<long long>((s % 86400) / 3600));
    return b;
}

std::string formatDate(int64_t ms) {
    if (ms <= 0) return "";
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm lt;
    localtime_s(&lt, &t);
    static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[40];
    snprintf(b, sizeof b, "%s %d, %02d:%02d", mon[lt.tm_mon], lt.tm_mday, lt.tm_hour, lt.tm_min);
    return b;
}

std::string formatDay(int64_t ms) {
    if (ms <= 0) return "";
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm lt;
    localtime_s(&lt, &t);
    static const char* mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[40];
    snprintf(b, sizeof b, "%s %d, %d", mon[lt.tm_mon], lt.tm_mday, lt.tm_year + 1900);
    return b;
}

int64_t nowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
}

std::string newId() {
    static uint32_t counter = 0;
    char b[32];
    snprintf(b, sizeof b, "%llx%04x", static_cast<unsigned long long>(nowMs()), static_cast<unsigned>((++counter + GetCurrentProcessId()) & 0xFFFF));
    return b;
}

void logLine(const std::string& line) {
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    std::string path = joinPath(dataDir(), "ddm.log");
    if (fileSize(path) > 2 * 1024 * 1024) MoveFileExW(W(path).c_str(), W(path + ".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileW(W(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[48];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string l = ts + line + "\r\n";
    DWORD put;
    WriteFile(h, l.data(), static_cast<DWORD>(l.size()), &put, nullptr);
    CloseHandle(h);
}

} // namespace ddm

namespace ddm {
std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> r;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(W(joinPath(dir, "*")).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return r;
    do { std::string n = U(fd.cFileName); if (n != "." && n != "..") r.push_back(n); } while (FindNextFileW(f, &fd));
    FindClose(f);
    return r;
}

void removeTree(const std::string& dir) {
    if (dir.size() < 4) return;   // never a drive root
    for (auto& n : listDir(dir)) {
        std::string p = joinPath(dir, n);
        DWORD a = GetFileAttributesW(W(p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) removeTree(p);   // never follows links out of the folder
        else { if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(W(p).c_str(), a & ~FILE_ATTRIBUTE_READONLY); if (a & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(W(p).c_str()); else DeleteFileW(W(p).c_str()); }
    }
    RemoveDirectoryW(W(dir).c_str());
}
} // namespace ddm
