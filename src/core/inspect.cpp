#include "inspect.hpp"
#include "http.hpp"
#include "model.hpp"
#include "xray.hpp"
#include "crypto.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

namespace ddm {
namespace inspect {

namespace {
std::mutex g_mu;
std::map<std::string, Report> g_reports;        // task id -> report
std::map<std::string, int64_t> g_started;
std::unordered_set<std::string> g_bad;           // hosts spreading malware
bool g_badLoaded = false;
std::map<std::string, std::pair<int64_t, int64_t>> g_age;   // domain -> (registered unix s, checked ms)

const std::set<std::string>& programExts() {
    static const std::set<std::string> s = {"exe", "msi", "msix", "appx", "scr", "com", "bat", "cmd", "pif", "vbs", "vbe", "js", "jse", "wsf", "wsh", "hta", "ps1", "lnk", "jar", "reg", "cpl", "dll", "iso", "img", "vhd", "vhdx", "apk", "chm", "msc"};
    return s;
}
const std::set<std::string>& harmlessExts() {
    static const std::set<std::string> s = {"jpg", "jpeg", "png", "gif", "bmp", "webp", "pdf", "txt", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "mp3", "mp4", "mkv", "avi", "mov", "wav", "flac", "srt", "csv", "rtf", "odt", "epub", "m4a", "webm"};
    return s;
}

std::string hostOf(const std::string& url) { return lower(urlHost(url)); }

// "downloads.example.co.uk" -> "example.co.uk"
std::string registrable(const std::string& host) {
    auto parts = split(host, '.');
    if (parts.size() <= 2) return host;
    std::string sld = parts[parts.size() - 2], tld = parts.back();
    static const std::set<std::string> second = {"co", "com", "org", "net", "gov", "ac", "edu", "ne", "or", "go"};
    size_t take = (tld.size() == 2 && second.count(sld)) ? 3 : 2;
    std::string out;
    for (size_t i = parts.size() - take; i < parts.size(); i++) out += (out.empty() ? "" : ".") + parts[i];
    return out;
}

bool isIp(const std::string& host) {
    if (host.find(':') != std::string::npos) return true;   // IPv6
    int dots = 0;
    for (char c : host) { if (c == '.') dots++; else if (!isdigit(static_cast<unsigned char>(c))) return false; }
    return dots == 3;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= b.size(); j++) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        prev.swap(cur);
    }
    return prev[b.size()];
}

// brands people are tricked with, and the sites that really are theirs
const std::map<std::string, std::vector<std::string>>& brands() {
    static const std::map<std::string, std::vector<std::string>> b = {
        {"microsoft", {"microsoft.com", "live.com", "office.com", "windows.com", "windowsupdate.com", "msft.net", "azureedge.net", "aka.ms", "visualstudio.com", "sharepoint.com", "onedrive.com", "xbox.com", "bing.com", "skype.com", "msecnd.net", "microsoftonline.com"}},
        {"windows", {"microsoft.com", "windows.com", "windowsupdate.com"}},
        {"google", {"google.com", "googleapis.com", "gstatic.com", "googleusercontent.com", "youtube.com", "googlevideo.com", "google.co.uk", "ggpht.com", "gvt1.com", "chrome.com"}},
        {"chrome", {"google.com", "chrome.com", "googleapis.com", "gvt1.com"}},
        {"adobe", {"adobe.com", "adobe.io", "adobecc.com"}},
        {"apple", {"apple.com", "icloud.com", "mzstatic.com"}},
        {"paypal", {"paypal.com", "paypalobjects.com"}},
        {"amazon", {"amazon.com", "amazonaws.com", "media-amazon.com", "amazon.co.uk", "amazon.in", "amazon.de"}},
        {"facebook", {"facebook.com", "fbcdn.net", "fb.com", "facebook.net"}},
        {"instagram", {"instagram.com", "cdninstagram.com"}},
        {"whatsapp", {"whatsapp.com", "whatsapp.net"}},
        {"telegram", {"telegram.org", "t.me", "telegram.me"}},
        {"steam", {"steampowered.com", "steamcommunity.com", "steamstatic.com", "steamcontent.com"}},
        {"epicgames", {"epicgames.com", "unrealengine.com"}},
        {"nvidia", {"nvidia.com", "geforce.com"}},
        {"intel", {"intel.com"}},
        {"mozilla", {"mozilla.org", "mozilla.net", "firefox.com"}},
        {"firefox", {"mozilla.org", "mozilla.net", "firefox.com"}},
        {"github", {"github.com", "githubusercontent.com", "github.io", "githubassets.com"}},
        {"zoom", {"zoom.us", "zoom.com"}},
        {"discord", {"discord.com", "discordapp.com", "discord.gg", "discordapp.net"}},
        {"spotify", {"spotify.com", "scdn.co", "spotifycdn.com"}},
        {"netflix", {"netflix.com", "nflxvideo.net"}},
        {"binance", {"binance.com"}},
        {"coinbase", {"coinbase.com"}},
        {"metamask", {"metamask.io"}},
        {"roblox", {"roblox.com", "rbxcdn.com"}},
        {"minecraft", {"minecraft.net", "mojang.com"}},
        {"winrar", {"win-rar.com", "rarlab.com"}},
        {"teamviewer", {"teamviewer.com"}},
        {"anydesk", {"anydesk.com"}},
        {"obsproject", {"obsproject.com"}},
        {"vlc", {"videolan.org"}},
    };
    return b;
}

std::string brandName(const std::string& b) {
    static const std::map<std::string, std::string> n = {{"paypal", "PayPal"}, {"github", "GitHub"}, {"whatsapp", "WhatsApp"}, {"epicgames", "Epic Games"}, {"obsproject", "OBS Studio"},
        {"vlc", "VLC"}, {"winrar", "WinRAR"}, {"teamviewer", "TeamViewer"}, {"anydesk", "AnyDesk"}, {"metamask", "MetaMask"}, {"nvidia", "NVIDIA"}};
    auto it = n.find(b);
    if (it != n.end()) return it->second;
    std::string o = b;
    if (!o.empty()) o[0] = static_cast<char>(toupper(static_cast<unsigned char>(o[0])));
    return o;
}

void add(Report& r, Level l, const std::string& text) {
    r.findings.push_back({l, text});
    if (static_cast<int>(l) > static_cast<int>(r.level)) r.level = l;
}

std::string listPath() { return joinPath(dataDir(), "malware-hosts.txt"); }

void loadBadHosts() {
    if (g_badLoaded) return;
    g_badLoaded = true;
    std::string text;
    if (!readFile(listPath(), text)) return;
    for (auto& line : split(text, '\n')) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t sp = l.find_first_of(" \t");
        std::string h = lower(trim(sp == std::string::npos ? l : l.substr(sp + 1)));
        if (!h.empty() && h.size() < 256) g_bad.insert(h);
    }
}

// when the domain was registered (RDAP, the public successor of WHOIS); 0 = unknown. Cached for a week.
int64_t registeredAt(const std::string& domain) {
    // only a plain domain name ever goes into the address (letters, digits, dots, dashes)
    if (domain.empty() || domain.size() > 253 || domain.find('.') == std::string::npos) return 0;
    for (char c : domain) if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return 0;
    {
        std::lock_guard<std::mutex> g(g_mu);
        auto it = g_age.find(domain);
        if (it != g_age.end() && nowMs() - it->second.second < 7LL * 86400 * 1000) return it->second.first;
    }
    RequestInfo r;
    r.url = "https://rdap.org/domain/" + domain;
    r.headers["Accept"] = "application/rdap+json";
    std::string body;
    int64_t when = 0;
    if (http::get(r, body, nullptr, 512 * 1024, 8) == 200) {
        Json j = Json::parse(body);
        for (auto& e : j["events"].list()) {
            if (lower(e["eventAction"].str()) != "registration") continue;
            std::string d = e["eventDate"].str();   // 2024-05-01T12:00:00Z
            SYSTEMTIME st{};
            if (d.size() >= 10 && sscanf(d.c_str(), "%4hu-%2hu-%2hu", &st.wYear, &st.wMonth, &st.wDay) == 3) {
                FILETIME ft;
                if (SystemTimeToFileTime(&st, &ft)) { ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime; when = static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL; }
            }
        }
    }
    std::lock_guard<std::mutex> g(g_mu);
    g_age[domain] = {when, nowMs()};
    return when;
}

// a program disguised by its name
std::string nameTrick(const std::string& fileName) {
    std::string n = lower(fileName);
    if (n.find("\xE2\x80\xAE") != std::string::npos || n.find("\xE2\x81\xA6") != std::string::npos) return "Its name hides its real ending with invisible characters.";
    std::string ext = extOf(n), inner = extOf(stem(n));
    if (programExts().count(ext) && harmlessExts().count(inner)) return "It's a program pretending to be ." + inner + " (" + baseName(n) + ").";
    if (programExts().count(ext) && n.find("    ") != std::string::npos) return "Its name hides the real ending behind spaces.";
    return "";
}

// what the first bytes say the file is ("" = nothing special)
std::string realKind(const std::string& head) {
    if (head.size() >= 64 && head[0] == 'M' && head[1] == 'Z') {   // a Windows program: MZ and a PE header where it points
        uint32_t pe = static_cast<uint8_t>(head[60]) | (static_cast<uint8_t>(head[61]) << 8) | (static_cast<uint8_t>(head[62]) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(head[63])) << 24);
        if (pe + 4 <= head.size() && head.compare(pe, 4, std::string("PE\0\0", 4)) == 0) return "program";
        if (pe + 4 > head.size()) return "program";   // header further in than we read: an MZ file all the same
    }
    static const std::string lnk("\x4C\x00\x00\x00\x01\x14\x02\x00\x00\x00\x00\x00\xC0\x00\x00\x00\x00\x00\x00\x46", 20);
    if (head.size() >= 20 && head.compare(0, 20, lnk) == 0) return "shortcut";
    if (head.size() >= 8 && head.compare(0, 8, std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) == 0) return "office";   // old Office / MSI
    std::string l = lower(head.substr(0, 1024));
    if (l.find("<hta:application") != std::string::npos) return "script";
    return "";
}

// a program wearing the name of a harmless file
std::string mismatch(const std::string& fileName, const std::string& head) {
    std::string ext = lower(extOf(fileName)), kind = realKind(head);
    if (kind.empty() || !harmlessExts().count(ext)) return "";
    if (kind == "program" || kind == "shortcut" || kind == "script")
        return "It's named ." + ext + " but it's really a " + (kind == "shortcut" ? "Windows shortcut" : kind == "script" ? "script program" : "Windows program") + ".";
    if (kind == "office" && ext.compare(0, 3, "doc") != 0 && ext.compare(0, 3, "xls") != 0 && ext.compare(0, 3, "ppt") != 0 && ext != "odt")   // (protected Office files are this format too)
        return "It's named ." + ext + " but it's really an Office document or installer.";
    return "";
}

void mark(const std::string& id, const Report& r) { std::lock_guard<std::mutex> g(g_mu); g_reports[id] = r; }
} // namespace

std::string levelName(Level l) { return l == Level::Safe ? "Safe" : l == Level::Caution ? "Be careful" : l == Level::Danger ? "Dangerous" : "Checking…"; }

std::string Report::summary() const {
    if (level == Level::Unknown) return "Checking…";
    std::string worst;
    for (auto& f : findings) if (f.level == level) { worst = f.text; break; }
    if (level == Level::Safe) return "No warning signs";
    return levelName(level) + ": " + worst;
}

Report checkLink(const std::string& url, const std::string& page, const std::string& fileName) {
    Report r;
    std::string host = hostOf(url), low = lower(url);
    if (startsWith(low, "magnet:") || host.empty()) { r.linkDone = true; r.level = Level::Safe; return r; }
    std::string dom = registrable(host);
    bool program = programExts().count(lower(extOf(fileName))) > 0;
    {
        std::lock_guard<std::mutex> g(g_mu);
        loadBadHosts();
        if (g_bad.count(host) || g_bad.count(dom)) add(r, Level::Danger, "This site is listed as spreading malware (URLhaus).");
        std::string ph = hostOf(page);
        if (!ph.empty() && ph != host && (g_bad.count(ph) || g_bad.count(registrable(ph)))) add(r, Level::Danger, "The page it came from is listed as spreading malware (URLhaus).");
    }
    std::string trick = nameTrick(fileName);
    if (!trick.empty()) add(r, Level::Danger, trick);
    {   // names malware likes to hide behind
        std::string n = lower(fileName);
        for (const char* w : {"crack", "keygen", "activator", "kmspico", "loader", "serial-key", "license-key", "free-robux", "hack-tool", "cheat-engine", "patcher"})
            if (n.find(w) != std::string::npos) { add(r, Level::Caution, "Files named like cracks, key generators and activators are a favourite hiding place for malware."); break; }
    }
    // a bare IP address on the internet (a PC on your own network — router, NAS, 192.168.x.x — is fine)
    bool local = host == "localhost" || startsWith(host, "127.") || startsWith(host, "10.") || startsWith(host, "192.168.") || startsWith(host, "169.254.") || host == "::1" || host == "[::1]";
    if (startsWith(host, "172.")) { int b = atoi(host.c_str() + 4); local = local || (b >= 16 && b <= 31); }
    if (isIp(host) && !local) add(r, program ? Level::Danger : Level::Caution, "The link is a bare IP address, not a named site.");
    if (host.find("xn--") != std::string::npos) add(r, Level::Caution, "The site's name uses letters from other alphabets that can look like ours (" + host + ").");
    static const std::set<std::string> shorteners = {"bit.ly", "tinyurl.com", "goo.gl", "is.gd", "cutt.ly", "rebrand.ly", "ow.ly", "buff.ly", "shorturl.at", "tiny.cc", "t.ly", "rb.gy", "s.id", "v.gd", "shorte.st", "adf.ly"};
    if (shorteners.count(dom)) add(r, Level::Caution, "A link shortener hides where the file really comes from.");
    std::string tld = host.substr(host.rfind('.') + 1);
    static const std::set<std::string> riskyTld = {"zip", "mov", "top", "xyz", "click", "country", "gq", "tk", "ml", "cf", "ga", "work", "rest", "kim", "cam", "buzz", "icu", "loan", "racing", "download", "stream", "monster", "sbs", "cfd"};
    if (riskyTld.count(tld)) add(r, Level::Caution, "." + tld + " sites are often used for scams and malware.");
    if (program && startsWith(low, "http://")) add(r, Level::Caution, "A program over an unencrypted link (http): it could be changed on the way.");
    // a site imitating a brand: digits for look-alike letters (micros0ft, paypa1), one letter off, or the brand's name
    // inside another site's name (paypal-secure-login.net)
    auto unmask = [](std::string x) {
        x = replaceAll(replaceAll(x, "rn", "m"), "vv", "w");
        for (auto& c : x) { switch (c) { case '0': c = 'o'; break; case '1': c = 'l'; break; case '3': c = 'e'; break; case '4': c = 'a'; break; case '5': c = 's'; break; case '7': c = 't'; break; case '8': c = 'b'; break; case '9': c = 'g'; break; default: break; } }
        return x;
    };
    static const std::set<std::string> everyday = {"windows", "chrome", "steam", "zoom", "apple", "intel", "vlc", "amazon"};   // also ordinary words
    std::string label = dom.substr(0, dom.find('.'));
    std::vector<std::string> tokens = split(label, '-');
    tokens.push_back(replaceAll(label, "-", ""));
    bool flagged = false;
    for (auto& b : brands()) {
        bool own = false;
        for (auto& o : b.second) if (dom == o || endsWith(host, "." + o) || host == o) own = true;
        if (own) continue;
        const std::string& br = b.first;
        const std::string bn = brandName(br);
        for (auto& t : tokens) {
            if (t.size() < 3) continue;
            if (t != br && unmask(t) == br) { add(r, Level::Danger, "The site imitates " + bn + " (" + dom + ")."); flagged = true; break; }
            if (!everyday.count(br) && br.size() >= 6 && t != br && t.size() + 1 >= br.size() && t.size() <= br.size() + 1 && editDistance(t, br) == 1) { add(r, Level::Danger, "The site's name is one letter away from " + bn + " (" + dom + ")."); flagged = true; break; }
        }
        if (!flagged && !everyday.count(br) && unmask(label) != label && unmask(label).find(br) != std::string::npos && label.find(br) == std::string::npos) { add(r, Level::Danger, "The site imitates " + bn + " (" + dom + ")."); flagged = true; }
        if (!flagged && label == br) { add(r, Level::Caution, "This isn't " + bn + "'s own site, though it has its name (" + dom + ")."); flagged = true; }
        if (!flagged && !everyday.count(br) && label.find(br) != std::string::npos) { add(r, Level::Caution, "The site uses the name " + bn + " but isn't " + bn + "'s own (" + dom + ")."); flagged = true; }
        if (flagged) break;
    }
    // how old the site is
    if (!isIp(host) && !local && host.find('.') != std::string::npos) {
        int64_t reg = registeredAt(dom);
        if (reg > 0) {
            int64_t days = (nowMs() / 1000 - reg) / 86400;
            if (days < 30) add(r, Level::Danger, "The site was registered only " + std::to_string(std::max<int64_t>(0, days)) + (days == 1 ? " day" : " days") + " ago.");
            else if (days < 180) add(r, Level::Caution, "The site is new (registered " + std::to_string(days) + " days ago).");
            else add(r, Level::Safe, "The site exists since " + formatDay(reg * 1000) + ".");
        }
    }
    if (r.level == Level::Unknown) r.level = Level::Safe;
    r.linkDone = true;
    return r;
}

void start(const Task& t) {
    if (t.kind == "torrent" || t.id.empty()) return;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_started.count(t.id)) return;
        g_started[t.id] = nowMs();
    }
    Task c = t;
    std::thread([c] {
        std::string page = c.pageUrl;
        std::string url = c.kind == "video" && !c.mediaUrl.empty() ? c.mediaUrl : c.url;
        Report r = checkLink(c.kind == "video" ? c.url : url, page, c.fileName);
        mark(c.id, r);
        // the file on the server: its first bytes, and inside archives / who signed a program
        if (c.kind == "http" || c.kind == "mega") {
            if (c.kind == "http") {
                RequestInfo ri; ri.url = url; ri.referer = page; ri.cookies = c.cookies; ri.userAgent = c.userAgent; ri.headers = c.headers;
                std::string err;
                auto s = xray::openHttp(ri, err);
                if (s) {
                    std::string head;
                    if (s->read(0, 4096, head)) {
                        std::string mm = mismatch(c.fileName, head);
                        if (!mm.empty()) add(r, Level::Danger, mm);
                    }
                    if (xray::canLook(c.fileName)) {
                        xray::Index idx;
                        if (xray::look(*s, c.fileName, idx)) {
                            int risky = 0; std::string first;
                            for (auto& e : idx.entries) if (!e.risk.empty()) { risky++; if (first.empty()) first = baseName(replaceAll(e.path, "/", "\\")) + ": " + e.risk; }
                            for (auto& w : idx.warnings) add(r, w.find("Not signed") != std::string::npos ? Level::Caution : Level::Danger, w);
                            if (risky) add(r, Level::Danger, std::to_string(risky) + (risky == 1 ? " file inside looks dangerous — " : " files inside look dangerous — ") + first);
                            if (!idx.label.empty() && !idx.listing) { r.signer = idx.label; add(r, Level::Safe, "Signed by " + idx.label + "."); }
                            if (idx.listing && !risky && idx.warnings.empty()) add(r, Level::Safe, idx.format + ": " + std::to_string(idx.entries.size()) + " items inside, nothing suspicious.");
                        }
                    }
                }
            }
        }
        r.fileDone = true;
        if (r.level == Level::Unknown) r.level = Level::Safe;
        mark(c.id, r);
    }).detach();
}

bool get(const std::string& id, Report& out) {
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_reports.find(id);
    if (it == g_reports.end()) return false;
    out = it->second;
    return true;
}

std::string checkLocalFile(const std::string& path) {
    std::string trick = nameTrick(baseName(path));
    if (!trick.empty()) return trick;
    HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string head(4096, '\0');
    DWORD got = 0;
    ReadFile(f, &head[0], static_cast<DWORD>(head.size()), &got, nullptr);
    CloseHandle(f);
    head.resize(got);
    return mismatch(baseName(path), head);
}

namespace {
struct Hasher {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    int64_t off = 0;           // bytes hashed
    int64_t cover = 0;         // bytes known to be in place from the start
    int64_t total = -1;
    std::string path, sha;
    bool running = false, done = false, failed = false;
    int64_t lastAsk = 0;
    void reset() { if (h) BCryptDestroyHash(h); h = nullptr; off = 0; sha.clear(); done = failed = false; }
    ~Hasher() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
};
std::map<std::string, std::shared_ptr<Hasher>> g_hash;
std::mutex g_hashMu;

int64_t contiguous(const Task& t) {
    if (t.state == State::Complete) return t.total >= 0 ? t.total : t.done;
    if (t.segments.empty()) return t.done;
    std::vector<std::pair<int64_t, int64_t>> got;
    for (auto& s : t.segments) got.push_back({s.start, s.end >= 0 ? std::min(s.pos, s.end + 1) : s.pos});
    std::sort(got.begin(), got.end());
    int64_t cover = 0;
    for (auto& g : got) { if (g.first > cover) break; cover = std::max(cover, g.second); }
    return cover;
}

void hashWork(std::shared_ptr<Hasher> hs) {
    std::vector<unsigned char> buf(1 << 20);
    for (;;) {
        std::string path; int64_t from, to;
        {
            std::lock_guard<std::mutex> g(g_hashMu);
            if (nowMs() - hs->lastAsk > 5000 || hs->failed) { hs->running = false; return; }
            path = hs->path; from = hs->off; to = hs->cover;
            if (!hs->h) {
                if (!hs->alg && BCryptOpenAlgorithmProvider(&hs->alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) { hs->failed = true; hs->running = false; return; }
                if (BCryptCreateHash(hs->alg, &hs->h, nullptr, 0, nullptr, 0, 0) != 0) { hs->failed = true; hs->running = false; return; }
            }
        }
        if (from >= to) {
            {
                std::lock_guard<std::mutex> g(g_hashMu);
                if (hs->total >= 0 && from == hs->total && hs->cover == hs->total && !hs->done) {
                    unsigned char out[32];
                    if (BCryptFinishHash(hs->h, out, 32, 0) == 0) { hs->sha = crypto::hex(crypto::Bytes(out, out + 32)); hs->done = true; }
                    BCryptDestroyHash(hs->h); hs->h = nullptr;
                    hs->running = false;
                    return;
                }
                if (hs->done) { hs->running = false; return; }
            }
            Sleep(300);
            continue;
        }
        HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) { Sleep(500); continue; }   // being moved into place; try again
        LARGE_INTEGER li; li.QuadPart = from;
        SetFilePointerEx(f, li, nullptr, FILE_BEGIN);
        int64_t at = from;
        int64_t budget = 64LL << 20;   // a little at a time: never hog the disk while it downloads
        while (at < to && budget > 0) {
            DWORD want = static_cast<DWORD>(std::min<int64_t>(static_cast<int64_t>(buf.size()), to - at)), got = 0;
            if (!ReadFile(f, buf.data(), want, &got, nullptr) || got == 0) break;
            std::lock_guard<std::mutex> g(g_hashMu);
            if (!hs->h || hs->off != at) break;   // started over meanwhile
            BCryptHashData(hs->h, buf.data(), got, 0);
            at += got; hs->off = at; budget -= got;
        }
        CloseHandle(f);
        if (budget <= 0) Sleep(50);
    }
}
} // namespace

LiveHash liveHash(const Task& t) {
    LiveHash out;
    out.total = t.total;
    if (t.kind != "http" && t.kind != "mega") { out.note = "Worked out when it finishes"; return out; }
    std::lock_guard<std::mutex> g(g_hashMu);
    auto& hs = g_hash[t.id];
    if (!hs) hs = std::make_shared<Hasher>();
    std::string path = t.state == State::Complete ? t.path() : t.partPath();
    if (t.state != State::Complete && !fileExists(path) && fileExists(t.path())) path = t.path();
    int64_t cover = contiguous(t);
    if (cover < hs->off || (t.total >= 0 && hs->total >= 0 && t.total != hs->total)) hs->reset();   // the file started over
    if (hs->path != path && hs->off > 0 && t.state != State::Complete) hs->reset();
    hs->path = path; hs->cover = cover; hs->total = t.total; hs->lastAsk = nowMs();
    if (!hs->running && !hs->done && !hs->failed) { hs->running = true; std::thread(hashWork, hs).detach(); }
    out.hashed = hs->off; out.sha256 = hs->sha; out.done = hs->done;
    if (hs->failed) out.note = "Not available";
    return out;
}

void refreshLists() {
    std::thread([] {
        std::string p = listPath();
        WIN32_FILE_ATTRIBUTE_DATA a{};
        bool fresh = false;
        if (GetFileAttributesExW(W(p).c_str(), GetFileExInfoStandard, &a)) {
            FILETIME now; GetSystemTimeAsFileTime(&now);
            ULARGE_INTEGER x, y; x.LowPart = now.dwLowDateTime; x.HighPart = now.dwHighDateTime; y.LowPart = a.ftLastWriteTime.dwLowDateTime; y.HighPart = a.ftLastWriteTime.dwHighDateTime;
            fresh = x.QuadPart - y.QuadPart < 20ULL * 3600 * 10000000ULL;
        }
        if (!fresh) {
            RequestInfo r; r.url = "https://urlhaus.abuse.ch/downloads/hostfile/";
            std::string body;
            if (http::get(r, body, nullptr, 32 * 1024 * 1024, 60) == 200 && body.find("127.0.0.1") != std::string::npos) writeFileAtomic(p, body);
        }
        std::lock_guard<std::mutex> g(g_mu);
        g_badLoaded = false;
        g_bad.clear();
        loadBadHosts();
    }).detach();
}

} // namespace inspect
} // namespace ddm
