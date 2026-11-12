// HTTP(S)/FTP plumbing over libcurl: one-time setup (TLS backend per Windows version), common request options,
// response-header parsing, and the network adapters a download can be spread over.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

typedef void CURL;

namespace ddm {

struct RequestInfo {
    std::string url;
    std::string referer;
    std::string cookies;                       // "a=b; c=d"
    std::string userAgent;
    std::map<std::string, std::string> headers; // extra headers from the browser
};

// What a server said about a file.
struct ResponseInfo {
    long status = 0;
    int64_t total = -1;            // full size, -1 unknown
    bool ranges = false;           // byte ranges (resume + many connections)
    std::string finalUrl;
    std::string fileName;          // from Content-Disposition
    std::string mime;
    std::string etag, lastModified;
    std::string digest;            // the whole file's checksum the server publishes: "sha256:<hex>", "sha512:…", "md5:…"
    bool digestOfPart = false;     // Content-MD5 of a 206 reply is only that part's
    std::string setCookies;        // "a=b; c=d": cookies the server set (pages whose files want them back)
};

namespace http {

void globalInit();                 // call once at start (picks Schannel on 8.1+, mbedTLS on Windows 7)
void globalCleanup();
bool usingSchannel();

// Applies DDM's common options: redirects, timeouts, TLS, proxy, browser identity.
void setup(CURL* c, const RequestInfo& r, const std::string& localIp = "");

// Parses one header line into info (called from header callbacks).
void parseHeader(const std::string& line, ResponseInfo& info);
std::string contentDispositionName(const std::string& value);

// One-shot GET into memory (small pages, APIs, favicons). Returns status, 0 on network error.
long get(const RequestInfo& r, std::string& body, ResponseInfo* info = nullptr, int64_t maxBytes = 16 * 1024 * 1024, int timeoutSec = 30);

// POST (APIs such as MEGA's). Same as get() otherwise.
// Many requests one after another on the same kept-open connection (no new handshake each time). One thread only.
class Session {
public:
    Session() = default;
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    long get(const RequestInfo& r, std::string& body, ResponseInfo* info = nullptr, int64_t maxBytes = 16 * 1024 * 1024, int timeoutSec = 30);
private:
    void* c = nullptr;
};
long post(const RequestInfo& r, const std::string& data, const std::string& contentType, std::string& body, int timeoutSec = 30);

// Saves a URL to a file (helper programs, updates). progress(got, total). Returns HTTP status, 0 = network error.
long download(const std::string& url, const std::string& path, std::function<void(int64_t, int64_t)> progress = nullptr, int timeoutSec = 600);

std::string defaultUserAgent();
std::vector<std::pair<std::string, std::string>> userAgentPresets();   // Options → Connection: name, user agent ("" = the default)
bool validUserAgent(const std::string& ua);
std::string proxyUrl();            // "" = direct; from Options or Windows' proxy settings

} // namespace http

// Network adapters that can reach the internet (Wi-Fi, Ethernet, mobile hotspot / phone tethering).
struct NetAdapter {
    std::string name;      // "Wi-Fi", "Ethernet"
    std::string ip;        // IPv4 address to bind to
    std::string kind;      // "wifi" | "ethernet" | "mobile" | "other"
    uint64_t linkSpeed = 0;
    unsigned long metric = 0;   // route metric: the lowest is Windows' main connection
};
std::vector<NetAdapter> internetAdapters();

} // namespace ddm
