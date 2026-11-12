#include "http.hpp"
#include <algorithm>
#include "settings.hpp"
#include "../common/util.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <curl/curl.h>
#include <mutex>
#include <set>
#include <cstring>

namespace ddm {

namespace {
bool g_schannel = false;
std::string g_caPem;     // mbedTLS only: Mozilla's list + Windows' trusted roots
std::once_flag g_caOnce;

typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);

DWORD windowsVersion() {   // 0x0601 = 7, 0x0602 = 8, 0x0603 = 8.1, 0x0A00 = 10/11
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (fn) fn(&v);
    return (v.dwMajorVersion << 8) | v.dwMinorVersion;
}

std::string embeddedCa() {
    HRSRC r = FindResourceW(nullptr, L"CACERT", reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!r) return "";
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return "";
    auto p = static_cast<const char*>(LockResource(g));
    return p ? std::string(p, SizeofResource(nullptr, r)) : "";
}

void addStore(const wchar_t* name, std::string& pem) {
    HCERTSTORE s = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, name);
    if (!s) return;
    PCCERT_CONTEXT c = nullptr;
    while ((c = CertEnumCertificatesInStore(s, c)) != nullptr) {
        DWORD n = 0;
        if (!CryptBinaryToStringA(c->pbCertEncoded, c->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &n)) continue;
        std::string b(n, '\0');
        if (CryptBinaryToStringA(c->pbCertEncoded, c->cbCertEncoded, CRYPT_STRING_BASE64HEADER, &b[0], &n)) {
            b.resize(strnlen(b.c_str(), b.size()));   // never let a terminator into the list (it would end mbedTLS's parsing)
            pem += b; pem += "\n";
        }
    }
    CertCloseStore(s, 0);
}

void buildCa() {
    g_caPem = embeddedCa();
    addStore(L"ROOT", g_caPem);
    char extra[MAX_PATH] = {};
    if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_CA", extra, sizeof extra)) { std::string more; if (readFile(extra, more)) g_caPem += "\n" + more; }   // tests only
    size_t n = 0; for (size_t p = 0; (p = g_caPem.find("BEGIN CERT", p)) != std::string::npos; p++) n++;
    logLine("tls: " + std::to_string(n) + " trusted root certificates");
    if (GetEnvironmentVariableA("DDM_TEST_DUMPCA", nullptr, 0)) writeFileAtomic(joinPath(dataDir(), "ca-dump.pem"), g_caPem);   // tests only
}
} // namespace

namespace http {

void globalInit() {
    DWORD v = windowsVersion();
    char env[32] = {};
    GetEnvironmentVariableA("DDM_TLS", env, sizeof env);
    bool wantMbed = std::string(env) == "mbedtls" || (std::string(env) != "schannel" && v < 0x0603);
    curl_global_sslset(wantMbed ? CURLSSLBACKEND_MBEDTLS : CURLSSLBACKEND_SCHANNEL, nullptr, nullptr);
    curl_global_init(CURL_GLOBAL_ALL);
    const curl_version_info_data* vi = curl_version_info(CURLVERSION_NOW);
    {   // the active TLS engine is the one NOT in brackets: "(mbedTLS/3.6.6) Schannel" or "mbedTLS/3.6.6 (Schannel)"
        std::string sv = vi && vi->ssl_version ? vi->ssl_version : "";
        g_schannel = sv.find("Schannel") != std::string::npos && sv.find("(Schannel") == std::string::npos;
    }
    logLine(std::string("http: ") + (vi ? vi->version : "?") + " / " + (vi && vi->ssl_version ? vi->ssl_version : "no TLS"));
}

void globalCleanup() { curl_global_cleanup(); }
bool usingSchannel() { return g_schannel; }

// Options → Connection → User agent: what DDM says it is for links you add yourself ("" = defaultUserAgent()).
std::vector<std::pair<std::string, std::string>> userAgentPresets() {
    return {
        {"A current Chrome on Windows (recommended)", ""},
        {"Microsoft Edge on Windows", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36 Edg/141.0.0.0"},
        {"Firefox on Windows", "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:144.0) Gecko/20100101 Firefox/144.0"},
        {"Safari on a Mac", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Safari/605.1.15"},
        {"Chrome on Android", "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Mobile Safari/537.36"},
        {"Safari on iPhone", "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Mobile/15E148 Safari/604.1"},
        {"Wget", "Wget/1.21.4"},
        {"curl", "curl/8.10.1"},
    };
}

// One line of printable characters with at least one product/version token ("Mozilla/5.0", "MyTool/2").
bool validUserAgent(const std::string& ua) {
    if (ua.size() < 3 || ua.size() > 512) return false;
    for (unsigned char c : ua) if (c < 0x20 || c > 0x7E) return false;
    if (!isalnum(static_cast<unsigned char>(ua[0]))) return false;
    size_t slash = ua.find('/');
    return slash != std::string::npos && slash > 0 && slash + 1 < ua.size() && ua[slash + 1] != ' ';
}

std::string defaultUserAgent() {
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36";
}

std::string proxyUrl() {
    const Settings& s = settings();
    char testProxy[256] = {};
    if (GetEnvironmentVariableA("DDM_TEST_PROXY", testProxy, sizeof testProxy)) return std::string(testProxy) == "none" ? "" : testProxy;   // tests only
    if (s.proxyMode == "none") return "";
    if (s.proxyMode == "manual") return s.proxyAddress;
    auto esc = [](const std::string& v) { std::string o; for (unsigned char c : v) { if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') o += static_cast<char>(c); else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; } } return o; };
    std::string auth = s.proxyUser.empty() ? "" : esc(s.proxyUser) + (s.proxyPass.empty() ? "" : ":" + esc(s.proxyPass)) + "@";
    if (s.proxyMode == "http" && !trim(s.proxyHost).empty()) return "http://" + auth + trim(s.proxyHost) + ":" + std::to_string(s.proxyPort > 0 ? s.proxyPort : 8080);
    if (s.proxyMode == "socks5" && !trim(s.proxyHost).empty()) return "socks5h://" + auth + trim(s.proxyHost) + ":" + std::to_string(s.proxyPort > 0 ? s.proxyPort : 1080);
    if (s.proxyMode == "tor") return "socks5h://127.0.0.1:" + std::to_string(s.torPort > 0 ? s.torPort : 9150);
    // Windows' own settings (Internet Options)
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg{};
    std::string r;
    if (WinHttpGetIEProxyConfigForCurrentUser(&cfg)) {
        if (cfg.lpszProxy && !cfg.fAutoDetect) {
            std::string p = U(cfg.lpszProxy);
            // "http=host:port;https=host:port" or "host:port"
            for (auto part : split(p, ';')) {
                part = trim(part);
                if (startsWith(part, "https=")) { r = part.substr(6); break; }
                if (startsWith(part, "http=") && r.empty()) r = part.substr(5);
                if (part.find('=') == std::string::npos && r.empty()) r = part;
            }
        }
        if (cfg.lpszProxy) GlobalFree(cfg.lpszProxy);
        if (cfg.lpszProxyBypass) GlobalFree(cfg.lpszProxyBypass);
        if (cfg.lpszAutoConfigUrl) GlobalFree(cfg.lpszAutoConfigUrl);
    }
    return r;
}

// The site a host belongs to, for cookies: "dl.example.com" -> "example.com", "a.b.co.uk" -> "b.co.uk"; "" for an IP.
static std::string cookieSite(const std::string& host) {
    if (host.empty() || host[0] == '[' || host.find_first_not_of("0123456789.") == std::string::npos) return "";
    auto parts = split(host, '.');
    if (parts.size() <= 2) return host;
    size_t n = parts.size();
    static const std::set<std::string> second = {"co", "com", "net", "org", "gov", "edu", "ac", "or", "ne", "go", "gob", "nic", "mil", "ltd", "plc", "sch", "nom", "info", "biz"};
    size_t keep = (parts[n - 1].size() == 2 && second.count(parts[n - 2])) ? 3 : 2;
    std::string out;
    for (size_t i = n - keep; i < n; i++) out += (out.empty() ? "" : ".") + parts[i];
    return out;
}

void setup(CURL* c, const RequestInfo& r, const std::string& localIp) {
    curl_easy_setopt(c, CURLOPT_URL, r.url.c_str());
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 15L);
    // web and FTP only: an address from a page, a playlist or a redirect can never read files on this PC (file:) or reach other services
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https,ftp,ftps");
    curl_easy_setopt(c, CURLOPT_AUTOREFERER, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 25L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024);
    curl_easy_setopt(c, CURLOPT_USERAGENT, (r.userAgent.empty() ? defaultUserAgent() : r.userAgent).c_str());
    if (!r.referer.empty()) curl_easy_setopt(c, CURLOPT_REFERER, r.referer.c_str());
    // The browser's cookies go only to the site they belong to (and its own subdomains), never to another site a
    // redirect leads to (CURLOPT_COOKIE would send them everywhere).
    curl_easy_setopt(c, CURLOPT_COOKIELIST, "ALL");   // (a handle used again starts without the last request's cookies)
    if (!r.cookies.empty()) {
        std::string host = urlHost(r.url), domain = cookieSite(host);
        bool ip = domain.empty();
        for (auto& part : split(r.cookies, ';')) {
            std::string kv = trim(part);
            size_t eq = kv.find('=');
            if (kv.empty() || eq == std::string::npos || eq == 0) continue;
            std::string name = trim(kv.substr(0, eq)), value = trim(kv.substr(eq + 1));
            if (name.find_first_of("\t\r\n") != std::string::npos || value.find_first_of("\t\r\n") != std::string::npos) continue;
            std::string line = (ip ? host : "." + domain) + (ip ? "\tFALSE" : "\tTRUE") + "\t/\tFALSE\t0\t" + name + "\t" + value;
            curl_easy_setopt(c, CURLOPT_COOKIELIST, line.c_str());
        }
    }
    if (g_schannel) curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT | CURLSSLOPT_NATIVE_CA));
    else {
        std::call_once(g_caOnce, buildCa);
        curl_blob b{const_cast<char*>(g_caPem.data()), g_caPem.size(), CURL_BLOB_NOCOPY};
        curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &b);
    }
    std::string proxy = proxyUrl();
    curl_easy_setopt(c, CURLOPT_PROXY, proxy.c_str());   // "" = direct (also overrides environment variables)
    if (!localIp.empty()) curl_easy_setopt(c, CURLOPT_INTERFACE, ("host!" + localIp).c_str());
    if (GetEnvironmentVariableA("DDM_TEST_VERBOSE", nullptr, 0)) curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);   // tests only
}

std::string contentDispositionName(const std::string& v) {
    std::string low = lower(v);
    size_t p = low.find("filename*=");
    if (p != std::string::npos) {
        std::string x = v.substr(p + 10);
        size_t semi = x.find(';');
        if (semi != std::string::npos) x = x.substr(0, semi);
        x = trim(x);
        if (!x.empty() && x.front() == '"') x = x.substr(1, x.size() >= 2 ? x.size() - 2 : 0);
        size_t q = x.find("''");
        if (q != std::string::npos) x = x.substr(q + 2);
        x = urlDecode(x);
        if (!x.empty()) return x;
    }
    p = low.find("filename=");
    if (p != std::string::npos) {
        std::string x = v.substr(p + 9);
        x = trim(x);
        if (!x.empty() && x.front() == '"') {
            size_t e = x.find('"', 1);
            x = x.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        } else {
            size_t semi = x.find(';');
            if (semi != std::string::npos) x = x.substr(0, semi);
        }
        return trim(x);
    }
    return "";
}

void parseHeader(const std::string& line0, ResponseInfo& info) {
    std::string line = line0;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (startsWith(line, "HTTP/")) {   // a new response (also after redirects): start over
        size_t sp = line.find(' ');
        long st = sp == std::string::npos ? 0 : atol(line.c_str() + sp + 1);
        std::string keepName = info.fileName, keepCookies = info.setCookies;
        info = ResponseInfo();
        info.status = st;
        info.setCookies = keepCookies;   // (cookies set along redirects count too)
        (void)keepName;
        return;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return;
    std::string k = lower(trim(line.substr(0, colon)));
    std::string v = trim(line.substr(colon + 1));
    if (k == "content-length") { if (info.total < 0) info.total = _atoi64(v.c_str()); }
    else if (k == "content-range") {   // bytes 0-0/12345
        size_t sl = v.find('/');
        if (sl != std::string::npos && v.substr(sl + 1) != "*") info.total = _atoi64(v.c_str() + sl + 1);
        info.ranges = true;
    }
    else if (k == "accept-ranges") { if (lower(v).find("bytes") != std::string::npos) info.ranges = true; }
    else if (k == "content-disposition") { std::string n = contentDispositionName(v); if (!n.empty()) info.fileName = n; }
    else if (k == "content-type") info.mime = lower(trim(split(v, ';')[0]));
    else if (k == "set-cookie") { std::string c = trim(v.substr(0, v.find(';'))); if (c.find('=') != std::string::npos) info.setCookies += (info.setCookies.empty() ? "" : "; ") + c; }
    else if (k == "etag") info.etag = v;
    else if (k == "digest" || k == "repr-digest" || k == "x-goog-hash" || k == "content-md5" || k == "x-amz-checksum-sha256") {
        // the whole file's checksum, in base64 (Digest: sha-256=…, Repr-Digest: sha-256=:…:, x-goog-hash: md5=…)
        auto hexOf = [](const std::string& b64) { std::string raw; static const std::string a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; int val = 0, bits = -8;
            for (char c : b64) { size_t i = a.find(c); if (i == std::string::npos) break; val = (val << 6) + static_cast<int>(i); bits += 6; if (bits >= 0) { raw.push_back(static_cast<char>((val >> bits) & 0xFF)); bits -= 8; } }
            std::string h; char x[3]; for (unsigned char c : raw) { snprintf(x, sizeof x, "%02x", c); h += x; } return h; };
        auto put = [&](const std::string& algo, std::string val) {
            val = trim(val); if (!val.empty() && val.front() == ':') val = val.substr(1); if (!val.empty() && val.back() == ':') val.pop_back();
            std::string h = hexOf(val);
            size_t want = algo == "sha512" ? 128 : algo == "sha256" ? 64 : algo == "sha1" ? 40 : algo == "md5" ? 32 : 0;
            if (want && h.size() == want && (info.digest.empty() || algo == "sha512" || (algo == "sha256" && info.digest.compare(0, 6, "sha512") != 0))) info.digest = algo + ":" + h;
        };
        if (k == "content-md5") { put("md5", v); info.digestOfPart = info.status == 206; }
        else if (k == "x-amz-checksum-sha256") put("sha256", v);
        else for (auto& part : split(v, ',')) {
            size_t eq = part.find('=');
            if (eq == std::string::npos) continue;
            std::string algo = lower(trim(part.substr(0, eq)));
            algo = algo == "sha-256" ? "sha256" : algo == "sha-512" ? "sha512" : algo == "sha" ? "sha1" : algo;
            put(algo, part.substr(eq + 1));
        }
    }
    else if (k == "last-modified") info.lastModified = v;
}

namespace {
struct MemSink { std::string* body; ResponseInfo* info; int64_t max; };
size_t memWrite(char* p, size_t s, size_t n, void* u) {
    auto m = static_cast<MemSink*>(u);
    if (static_cast<int64_t>(m->body->size() + s * n) > m->max) return 0;
    m->body->append(p, s * n);
    return s * n;
}
size_t memHeader(char* p, size_t s, size_t n, void* u) {
    auto m = static_cast<MemSink*>(u);
    if (m->info) parseHeader(std::string(p, s * n), *m->info);
    return s * n;
}
} // namespace

namespace {
long performOn(CURL* c, const RequestInfo& r, const std::string* data, const std::string& contentType, std::string& body, ResponseInfo* info, int64_t maxBytes, int timeoutSec);
long getOrPost(const RequestInfo& r, const std::string* data, const std::string& contentType, std::string& body, ResponseInfo* info, int64_t maxBytes, int timeoutSec) {
    CURL* c = curl_easy_init();
    if (!c) return 0;
    long st = performOn(c, r, data, contentType, body, info, maxBytes, timeoutSec);
    curl_easy_cleanup(c);
    return st;
}
long performOn(CURL* c, const RequestInfo& r, const std::string* data, const std::string& contentType, std::string& body, ResponseInfo* info, int64_t maxBytes, int timeoutSec) {
    setup(c, r);
    struct curl_slist* hl = nullptr;
    for (auto& kv : r.headers) hl = curl_slist_append(hl, (kv.first + ": " + kv.second).c_str());
    if (data) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(data->size()));
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, data->c_str());
        hl = curl_slist_append(hl, ("Content-Type: " + contentType).c_str());
    }
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec));
    ResponseInfo local;
    MemSink sink{&body, info ? info : &local, maxBytes};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, memWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, memHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &sink);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    char* eff = nullptr;
    if (info && curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) info->finalUrl = eff;
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(hl);
    return status;
}
} // namespace

Session::~Session() { if (c) curl_easy_cleanup(static_cast<CURL*>(c)); }
long Session::get(const RequestInfo& r, std::string& body, ResponseInfo* info, int64_t maxBytes, int timeoutSec) {
    if (!c) c = curl_easy_init();
    else curl_easy_reset(static_cast<CURL*>(c));   // the settings go, the open connection stays
    if (!c) return 0;
    return performOn(static_cast<CURL*>(c), r, nullptr, "", body, info, maxBytes, timeoutSec);
}

long get(const RequestInfo& r, std::string& body, ResponseInfo* info, int64_t maxBytes, int timeoutSec) { return getOrPost(r, nullptr, "", body, info, maxBytes, timeoutSec); }
long post(const RequestInfo& r, const std::string& data, const std::string& contentType, std::string& body, int timeoutSec) {
    return getOrPost(r, &data, contentType, body, nullptr, 64 * 1024 * 1024, timeoutSec);
}

namespace {
struct FileSink { HANDLE f; int64_t got, total; std::function<void(int64_t, int64_t)> cb; int64_t lastCb; ResponseInfo info; };
size_t fileWrite(char* p, size_t s, size_t n, void* u) {
    auto k = static_cast<FileSink*>(u);
    if (k->info.status >= 400) return 0;
    DWORD put = 0;
    if (!WriteFile(k->f, p, static_cast<DWORD>(s * n), &put, nullptr) || put != s * n) return 0;
    k->got += static_cast<int64_t>(put);
    if (k->cb && k->got - k->lastCb > 512 * 1024) { k->lastCb = k->got; k->cb(k->got, k->info.total); }
    return s * n;
}
size_t fileHeader(char* p, size_t s, size_t n, void* u) { parseHeader(std::string(p, s * n), static_cast<FileSink*>(u)->info); return s * n; }
} // namespace

long download(const std::string& url, const std::string& path, std::function<void(int64_t, int64_t)> progress, int timeoutSec) {
    HANDLE f = CreateFileW(W(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    CURL* c = curl_easy_init();
    RequestInfo r;
    r.url = url;
    r.userAgent = "DDM-Windows";
    setup(c, r);
    FileSink k{f, 0, -1, progress, 0, ResponseInfo()};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, fileWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &k);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, fileHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &k);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec));
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    CloseHandle(f);
    if (rc != CURLE_OK || status >= 400) { DeleteFileW(W(path).c_str()); return rc != CURLE_OK && status < 400 ? 0 : status; }
    return status;
}

} // namespace http

std::vector<NetAdapter> internetAdapters() {
    std::vector<NetAdapter> r;
    ULONG len = 16 * 1024;
    std::vector<unsigned char> buf(len);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &len);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.resize(len); rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &len); }
    if (rc != NO_ERROR) return r;
    for (auto a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        if (!a->FirstGatewayAddress || !a->FirstUnicastAddress) continue;   // no way to the internet
        auto sa = reinterpret_cast<sockaddr_in*>(a->FirstUnicastAddress->Address.lpSockaddr);
        char ip[32] = {};
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
        if (std::string(ip).rfind("169.254.", 0) == 0) continue;
        NetAdapter n;
        n.name = U(a->FriendlyName ? a->FriendlyName : L"");
        n.ip = ip;
        n.kind = a->IfType == IF_TYPE_IEEE80211 ? "wifi" : a->IfType == IF_TYPE_ETHERNET_CSMACD ? "ethernet" : (a->IfType == 243 || a->IfType == 244 || a->IfType == IF_TYPE_PPP) ? "mobile" : "other";
        n.linkSpeed = a->ReceiveLinkSpeed;
        n.metric = a->Ipv4Metric;
        r.push_back(n);
    }
    // Windows' own first choice (lowest route metric) first: the others are the "extra" connections
    std::stable_sort(r.begin(), r.end(), [](const NetAdapter& x, const NetAdapter& y) { return x.metric < y.metric; });
    return r;
}

} // namespace ddm
