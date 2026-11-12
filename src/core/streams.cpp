#include "streams.hpp"
#include "settings.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace ddm {
namespace streams {

// ------------------------------------------------------------------ addresses
std::string resolveUrl(const std::string& base, const std::string& ref0) {
    std::string ref = trim(ref0);
    if (ref.empty()) return base;
    size_t colon = ref.find("://");
    if (colon != std::string::npos && colon < 10 && ref.find('/') > colon) return ref;
    size_t se = base.find("://");
    if (se == std::string::npos) return ref;
    std::string scheme = base.substr(0, se);
    if (startsWith(ref, "//")) return scheme + ":" + ref;
    size_t pathStart = base.find('/', se + 3);
    std::string origin = pathStart == std::string::npos ? base : base.substr(0, pathStart);
    std::string path = pathStart == std::string::npos ? "/" : base.substr(pathStart);
    size_t q = path.find_first_of("?#");
    std::string pathOnly = q == std::string::npos ? path : path.substr(0, q);
    if (ref[0] == '?') return origin + pathOnly + ref;
    if (ref[0] == '#') return base;
    std::string joined = ref[0] == '/' ? ref : pathOnly.substr(0, pathOnly.rfind('/') + 1) + ref;
    // tidy "./" and "../" in the path (not in the query)
    size_t rq = joined.find_first_of("?#");
    std::string p = rq == std::string::npos ? joined : joined.substr(0, rq), rest = rq == std::string::npos ? "" : joined.substr(rq);
    std::vector<std::string> out;
    for (auto& part : split(p, '/')) {
        if (part == ".") continue;
        if (part == "..") { if (out.size() > 1) out.pop_back(); continue; }
        out.push_back(part);
    }
    std::string np;
    for (size_t i = 0; i < out.size(); i++) np += (i ? "/" : "") + out[i];
    if (!p.empty() && p.back() == '/' && (np.empty() || np.back() != '/')) np += "/";
    if (np.empty() || np[0] != '/') np = "/" + np;
    return origin + np + rest;
}

static std::string pathLower(const std::string& u) { std::string l = lower(u); size_t q = l.find_first_of("?#"); return q == std::string::npos ? l : l.substr(0, q); }
// the registrable part of an address's host ("cdn.video.example.co.uk" -> "example.co.uk")
std::string siteKey(const std::string& url) {
    std::string h = lower(urlHost(url));
    auto parts = split(h, '.');
    if (parts.size() <= 2) return h;
    std::string last2 = parts[parts.size() - 2] + "." + parts.back();
    static const char* second[] = {"co.uk", "org.uk", "ac.uk", "com.au", "net.au", "co.jp", "ne.jp", "com.br", "co.in", "com.tr", "co.kr", "com.mx", "com.pk", "co.za", "com.sg", "com.ar", "com.cn", "com.tw", "com.hk", "co.nz", "co.id", "com.my", "com.ph", "com.vn", "com.ua", "com.eg", "com.sa", "co.il", "com.co"};
    for (auto s : second) if (last2 == s) return parts[parts.size() - 3] + "." + last2;
    return last2;
}
bool sameSite(const std::string& a, const std::string& b) { return !a.empty() && !b.empty() && siteKey(a) == siteKey(b); }

bool looksHls(const std::string& u) { std::string l = pathLower(u); return endsWith(l, ".m3u8") || endsWith(l, ".m3u") || lower(u).find(".m3u8?") != std::string::npos || (lower(u).find("/hls/") != std::string::npos && endsWith(l, "playlist")); }
bool looksDash(const std::string& u) { std::string l = pathLower(u); return endsWith(l, ".mpd") || lower(u).find(".mpd?") != std::string::npos; }

namespace {

std::map<std::string, std::string> attrs(const std::string& s) {   // KEY=VALUE,KEY="VALUE, WITH COMMAS"
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || s[i] == ' ')) i++;
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) break;
        std::string k = trim(s.substr(i, eq - i));
        for (auto& c : k) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        i = eq + 1;
        std::string v;
        if (i < s.size() && s[i] == '"') { size_t e = s.find('"', i + 1); v = s.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1); i = e == std::string::npos ? s.size() : e + 1; }
        else { size_t e = s.find(',', i); v = s.substr(i, e == std::string::npos ? std::string::npos : e - i); i = e == std::string::npos ? s.size() : e; }
        m[k] = trim(v);
    }
    return m;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> v;
    for (auto& l : split(replaceAll(text, "\r", ""), '\n')) { std::string t = trim(l); if (!t.empty()) v.push_back(t); }
    return v;
}

// ---- what the browser read (remember)
struct Kept { std::string text; int64_t at = 0; };
std::mutex g_keptMu;
std::map<std::string, Kept> g_kept;

bool keptFor(const std::string& url, std::string& body, int64_t maxAgeMs) {
    std::lock_guard<std::mutex> g(g_keptMu);
    auto it = g_kept.find(url);
    if (it == g_kept.end() || nowMs() - it->second.at > maxAgeMs) return false;
    body = it->second.text;
    return true;
}

// a few random, meaningless headers: sites that refuse programs by the exact set of headers they send let these through
std::map<std::string, std::string> oddHeaders() {
    static const char* letters = "bcdfghjklmnpqrstvwxz";   // (no vowels: never a real header's name)
    std::map<std::string, std::string> h;
    unsigned seed = static_cast<unsigned>(nowMs()) ^ static_cast<unsigned>(GetCurrentThreadId() * 2654435761u);
    auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) & 0xFFFF; };
    int count = 2 + static_cast<int>(next() % 6);
    for (int i = 0; i < count; i++) {
        std::string k, v;
        int kl = 8 + static_cast<int>(next() % 12), vl = 16 + static_cast<int>(next() % 16);
        for (int c = 0; c < kl; c++) k += letters[next() % 20];
        for (int c = 0; c < vl; c++) v += letters[next() % 20];
        h[k] = v;
    }
    return h;
}

// Lists DDM itself read a moment ago (a finished video's lists never change): the download that starts right after
// the qualities were shown doesn't ask for them a second time.
struct Recent { std::string text; int64_t at = 0; };
std::mutex g_recentMu;
std::map<std::string, Recent> g_recent;
std::set<std::string> g_oddHosts;   // servers that refuse DDM's plain request: asked the other way at once from then on

bool recentFor(const std::string& url, std::string& body) {
    std::lock_guard<std::mutex> g(g_recentMu);
    auto it = g_recent.find(url);
    if (it == g_recent.end() || nowMs() - it->second.at > 60 * 1000) return false;
    body = it->second.text;
    return true;
}
void keepRecent(const std::string& url, const std::string& text) {
    if (text.size() > 4u * 1024 * 1024 || text.find("#EXTM3U") == std::string::npos) return;
    if (text.find("#EXT-X-STREAM-INF") == std::string::npos && text.find("#EXT-X-ENDLIST") == std::string::npos) return;   // (a live list changes all the time)
    std::lock_guard<std::mutex> g(g_recentMu);
    int64_t now = nowMs();
    if (g_recent.size() > 80) for (auto it = g_recent.begin(); it != g_recent.end();) { if (now - it->second.at > 60 * 1000) it = g_recent.erase(it); else ++it; }
    if (g_recent.size() > 120) g_recent.clear();
    Recent r; r.text = text; r.at = now;
    g_recent[url] = r;
}

long fetchText(const RequestInfo& base, const std::string& url, std::string& body) {
    if (keptFor(url, body, 20 * 60 * 1000)) return 200;   // the browser read it a moment ago
    if (recentFor(url, body)) return 200;                 // … or DDM did
    RequestInfo r = base;
    r.url = url;
    if (!base.referer.empty() && !sameSite(url, base.referer)) r.cookies.clear();   // the page's cookies stay with the page's site
    std::string host = urlSite(url);
    bool odd = false;
    { std::lock_guard<std::mutex> g(g_recentMu); odd = g_oddHosts.count(host) > 0; }
    if (odd) for (auto& kv : oddHeaders()) r.headers[kv.first] = kv.second;
    long st = 0;
    for (int i = 0; i < 3; i++) {
        body.clear();
        st = http::get(r, body, nullptr, 32 * 1024 * 1024, 30);
        if (st == 403 && i == 0 && !odd) {   // refused: once more, looking a little different
            for (auto& kv : oddHeaders()) r.headers[kv.first] = kv.second;
            odd = true;
            continue;
        }
        if ((st == 200 || st == 206) && odd) { std::lock_guard<std::mutex> g(g_recentMu); if (g_oddHosts.size() < 500) g_oddHosts.insert(host); }
        if (st == 200 || st == 206 || (st >= 400 && st < 500 && st != 429)) break;
        Sleep(800 * (i + 1));
    }
    if (st == 200 || st == 206) keepRecent(url, body);
    // the server doesn't answer DDM itself: what the browser read earlier (up to six hours ago) is better than nothing
    if (st != 200 && st != 206) { std::string old; if (keptFor(url, old, 6LL * 3600 * 1000)) { body = old; return 200; } }
    return st;
}

std::string ivFromSequence(int64_t seq) {
    char b[33];
    snprintf(b, sizeof b, "%032llx", static_cast<unsigned long long>(seq));
    return b;
}

// ------------------------------------------------------------------ a small XML reader (MPD manifests)
struct XNode { std::string name, text; std::map<std::string, std::string> attr; std::vector<XNode> kids; };

std::string xmlDecode(std::string s) {
    s = replaceAll(s, "&lt;", "<"); s = replaceAll(s, "&gt;", ">"); s = replaceAll(s, "&quot;", "\""); s = replaceAll(s, "&apos;", "'");
    return replaceAll(s, "&amp;", "&");
}

bool parseXml(const std::string& x, size_t& i, XNode& node, int depth) {
    if (depth > 64) return false;
    // at '<'
    size_t e = i + 1;
    while (e < x.size() && !isspace(static_cast<unsigned char>(x[e])) && x[e] != '>' && x[e] != '/') e++;
    node.name = x.substr(i + 1, e - i - 1);
    size_t colon = node.name.find(':');
    if (colon != std::string::npos) node.name = node.name.substr(colon + 1);
    i = e;
    while (i < x.size()) {   // attributes
        while (i < x.size() && isspace(static_cast<unsigned char>(x[i]))) i++;
        if (i >= x.size()) return false;
        if (x[i] == '/') { i = x.find('>', i); if (i == std::string::npos) return false; i++; return true; }
        if (x[i] == '>') { i++; break; }
        size_t eq = x.find('=', i);
        if (eq == std::string::npos) return false;
        std::string k = trim(x.substr(i, eq - i));
        size_t c2 = k.find(':');
        if (c2 != std::string::npos && k.substr(0, c2) != "xmlns") k = k.substr(c2 + 1);
        size_t q = eq + 1;
        while (q < x.size() && isspace(static_cast<unsigned char>(x[q]))) q++;
        if (q >= x.size()) return false;
        char quote = x[q];
        if (quote != '"' && quote != '\'') return false;
        size_t qe = x.find(quote, q + 1);
        if (qe == std::string::npos) return false;
        node.attr[k] = xmlDecode(x.substr(q + 1, qe - q - 1));
        i = qe + 1;
    }
    for (;;) {   // children and text
        size_t lt = x.find('<', i);
        if (lt == std::string::npos) return false;
        node.text += x.substr(i, lt - i);
        i = lt;
        if (x.compare(i, 4, "<!--") == 0) { size_t ce = x.find("-->", i); if (ce == std::string::npos) return false; i = ce + 3; continue; }
        if (x.compare(i, 9, "<![CDATA[") == 0) { size_t ce = x.find("]]>", i); if (ce == std::string::npos) return false; node.text += x.substr(i + 9, ce - i - 9); i = ce + 3; continue; }
        if (x.compare(i, 2, "</") == 0) { size_t ce = x.find('>', i); if (ce == std::string::npos) return false; i = ce + 1; node.text = xmlDecode(trim(node.text)); return true; }
        if (x.compare(i, 2, "<?") == 0) { size_t ce = x.find("?>", i); if (ce == std::string::npos) return false; i = ce + 2; continue; }
        XNode k;
        if (!parseXml(x, i, k, depth + 1)) return false;
        node.kids.push_back(std::move(k));
    }
}

const XNode* child(const XNode& n, const char* name) { for (auto& k : n.kids) if (k.name == name) return &k; return nullptr; }
std::string at(const XNode* n, const char* a) { if (!n) return ""; auto it = n->attr.find(a); return it == n->attr.end() ? "" : it->second; }

double isoDuration(const std::string& s) {   // PT1H2M3.5S, P1DT2H
    double total = 0, num = 0, frac = 0, scale = 0;
    bool time = false;
    for (char c : s) {
        if (c == 'P') continue;
        if (c == 'T') { time = true; continue; }
        if (isdigit(static_cast<unsigned char>(c))) { if (scale > 0) { frac += (c - '0') * scale; scale /= 10; } else num = num * 10 + (c - '0'); continue; }
        if (c == '.') { scale = 0.1; continue; }
        double v = num + frac;
        if (c == 'D') total += v * 86400; else if (c == 'H') total += v * 3600; else if (c == 'M') total += time ? v * 60 : v * 2592000; else if (c == 'S') total += v; else if (c == 'Y') total += v * 31536000; else if (c == 'W') total += v * 604800;
        num = 0; frac = 0; scale = 0;
    }
    return total;
}

std::string fillTemplate(const std::string& t, const std::string& repId, int64_t number, int64_t time, int64_t bandwidth) {
    std::string out;
    for (size_t i = 0; i < t.size();) {
        if (t[i] != '$') { out += t[i++]; continue; }
        size_t e = t.find('$', i + 1);
        if (e == std::string::npos) { out += t.substr(i); break; }
        std::string tok = t.substr(i + 1, e - i - 1);
        i = e + 1;
        if (tok.empty()) { out += '$'; continue; }
        std::string fmt = "%d";
        size_t pct = tok.find('%');
        if (pct != std::string::npos) { fmt = tok.substr(pct); tok = tok.substr(0, pct); }
        if (tok == "RepresentationID") { out += repId; continue; }
        int64_t v = tok == "Number" ? number : tok == "Time" ? time : tok == "Bandwidth" ? bandwidth : 0;
        if (tok != "Number" && tok != "Time" && tok != "Bandwidth") { out += "$" + tok + "$"; continue; }
        std::string f = fmt;
        if (!f.empty() && f.back() == 'd') f = f.substr(0, f.size() - 1) + "lld";
        char b[64];
        snprintf(b, sizeof b, f.c_str(), static_cast<long long>(v));
        out += b;
    }
    return out;
}

double parseRate(const std::string& s) {   // "30000/1001" or "25"
    size_t sl = s.find('/');
    if (sl == std::string::npos) return atof(s.c_str());
    double d = atof(s.substr(sl + 1).c_str());
    return d > 0 ? atof(s.substr(0, sl).c_str()) / d : 0;
}

// ------------------------------------------------------------------ AES-128 (HLS)
bool aesDecrypt(const std::string& key, const std::string& ivHex, std::string& data) {
    if (key.size() != 16 || data.empty() || data.size() % 16) return false;
    unsigned char iv[16] = {};
    std::string h = ivHex;
    if (startsWith(lower(h), "0x")) h = h.substr(2);
    while (h.size() < 32) h = "0" + h;
    for (int i = 0; i < 16; i++) iv[i] = static_cast<unsigned char>(strtol(h.substr(static_cast<size_t>(i) * 2, 2).c_str(), nullptr, 16));
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), sizeof(BCRYPT_CHAIN_MODE_CBC), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), 16, 0) == 0) {
        ULONG got = 0;
        std::string out(data.size(), '\0');
        NTSTATUS st = BCryptDecrypt(k, reinterpret_cast<PUCHAR>(&data[0]), static_cast<ULONG>(data.size()), nullptr, iv, 16, reinterpret_cast<PUCHAR>(&out[0]), static_cast<ULONG>(out.size()), &got, BCRYPT_BLOCK_PADDING);
        if (st != 0) {   // some servers don't pad the last block: decrypt without removing padding
            if (startsWith(lower(h), "0x")) h = h.substr(2);
            for (int i = 0; i < 16; i++) iv[i] = static_cast<unsigned char>(strtol(h.substr(static_cast<size_t>(i) * 2, 2).c_str(), nullptr, 16));
            st = BCryptDecrypt(k, reinterpret_cast<PUCHAR>(&data[0]), static_cast<ULONG>(data.size()), nullptr, iv, 16, reinterpret_cast<PUCHAR>(&out[0]), static_cast<ULONG>(out.size()), &got, 0);
        }
        if (st == 0) { out.resize(got); data.swap(out); ok = true; }
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

} // namespace

void remember(const std::string& url, const std::string& text) {
    // only lists that don't change: an HLS master or finished playlist, a DASH manifest that isn't live
    bool hls = text.find("#EXTM3U") != std::string::npos;
    bool keep = hls ? (text.find("#EXT-X-STREAM-INF") != std::string::npos || text.find("#EXT-X-ENDLIST") != std::string::npos)
                    : (text.find("<MPD") != std::string::npos && text.find("type=\"dynamic\"") == std::string::npos);
    if (!keep || url.empty() || text.size() > 2 * 1024 * 1024) return;
    std::lock_guard<std::mutex> g(g_keptMu);
    int64_t now = nowMs();
    if (g_kept.size() >= 60) {   // the oldest goes
        auto old = g_kept.begin();
        for (auto it = g_kept.begin(); it != g_kept.end(); ++it) if (it->second.at < old->second.at) old = it;
        g_kept.erase(old);
    }
    g_kept[url] = Kept{text, now};
}

// ------------------------------------------------------------------ HLS
bool readHls(const RequestInfo& r, Master& out, std::string& error) {
    out = Master();
    std::string text;
    long st = fetchText(r, r.url, text);
    if (st != 200 && st != 206) { error = st == 0 ? "Couldn't reach the video's server." : "The video's server answered " + std::to_string(st) + "."; return false; }
    if (text.find("#EXTM3U") == std::string::npos) { error = "This isn't a video stream."; return false; }
    auto ls = lines(text);
    bool master = text.find("#EXT-X-STREAM-INF") != std::string::npos;
    if (!master) {
        Variant v; v.url = r.url;
        double d = 0;
        for (auto& l : ls) if (startsWith(l, "#EXTINF:")) d += atof(l.c_str() + 8);
        out.duration = d;
        out.variants.push_back(v);
        return true;
    }
    for (size_t i = 0; i < ls.size(); i++) {
        const std::string& l = ls[i];
        if (startsWith(l, "#EXT-X-STREAM-INF:")) {
            auto a = attrs(l.substr(18));
            Variant v;
            v.bandwidth = _atoi64(a["AVERAGE-BANDWIDTH"].empty() ? a["BANDWIDTH"].c_str() : a["AVERAGE-BANDWIDTH"].c_str());
            std::string res = a["RESOLUTION"];
            size_t x = res.find('x');
            if (x != std::string::npos) { v.width = atoi(res.substr(0, x).c_str()); v.height = atoi(res.substr(x + 1).c_str()); }
            v.codecs = a["CODECS"]; v.audioGroup = a["AUDIO"]; v.fps = atof(a["FRAME-RATE"].c_str());
            for (size_t j = i + 1; j < ls.size(); j++) if (ls[j][0] != '#') { v.url = resolveUrl(r.url, ls[j]); i = j; break; }
            if (!v.url.empty()) out.variants.push_back(v);
        } else if (startsWith(l, "#EXT-X-MEDIA:")) {
            auto a = attrs(l.substr(13));
            if (a["TYPE"] == "SUBTITLES" && !a["URI"].empty() && a["FORCED"] != "YES") {
                Rendition sd; sd.group = a["GROUP-ID"]; sd.url = resolveUrl(r.url, a["URI"]); sd.lang = a["LANGUAGE"]; sd.name = a["NAME"];
                bool have = false;
                for (auto& x : out.subs) if (x.url == sd.url) have = true;
                if (!have && out.subs.size() < 200) out.subs.push_back(sd);
                continue;
            }
            if (a["TYPE"] != "AUDIO" || a["URI"].empty()) continue;
            Rendition rd; rd.group = a["GROUP-ID"]; rd.url = resolveUrl(r.url, a["URI"]); rd.lang = a["LANGUAGE"]; rd.name = a["NAME"]; rd.isDefault = a["DEFAULT"] == "YES";
            out.audios.push_back(rd);
        }
    }
    // only video variants that aren't "I-frame only" and have an address
    if (out.variants.empty()) { error = "The stream lists no qualities."; return false; }
    std::stable_sort(out.variants.begin(), out.variants.end(), [](const Variant& a, const Variant& b) { return a.height != b.height ? a.height > b.height : a.bandwidth > b.bandwidth; });
    {   // the length, from the first quality's own list (for sizes)
        std::string t2;
        if (fetchText(r, out.variants[0].url, t2) == 200) for (auto& l : lines(t2)) if (startsWith(l, "#EXTINF:")) out.duration += atof(l.c_str() + 8);
    }
    return true;
}

bool hlsPlan(const RequestInfo& r, const std::string& url, Plan& out, std::string& error) {
    out = Plan();
    out.playlistUrl = url;
    std::string text;
    long st = fetchText(r, url, text);
    if (st != 200 && st != 206) { error = st == 0 ? "Couldn't reach the video's server." : st == 403 || st == 404 || st == 410 ? "The video's address expired. Open the video's page again (Refresh download link)." : "The video's server answered " + std::to_string(st) + "."; return false; }
    if (text.find("#EXT-X-STREAM-INF") != std::string::npos) {   // a list of qualities: take the best
        Master m;
        RequestInfo r2 = r; r2.url = url;
        if (!readHls(r2, m, error) || m.variants.empty()) return false;
        return hlsPlan(r, m.variants[0].url, out, error);
    }
    auto ls = lines(text);
    int64_t seq = 0, nextOffset = 0;
    std::string keyUrl, keyIv, method = "NONE";
    bool ended = false;
    double pendingDur = 0;
    int64_t brLen = -1, brFrom = -1;
    for (auto& l : ls) {
        if (startsWith(l, "#EXT-X-MEDIA-SEQUENCE:")) seq = _atoi64(l.c_str() + 22);
        else if (startsWith(l, "#EXT-X-ENDLIST")) ended = true;
        else if (startsWith(l, "#EXT-X-KEY:")) {
            auto a = attrs(l.substr(11));
            method = a["METHOD"];
            if (method == "SAMPLE-AES" || method == "SAMPLE-AES-CTR" || (!a["KEYFORMAT"].empty() && a["KEYFORMAT"] != "identity")) { error = "This video is protected (DRM): it can only be played in the site's own player."; return false; }
            keyUrl = method == "AES-128" ? resolveUrl(url, a["URI"]) : "";
            keyIv = a["IV"];
        } else if (startsWith(l, "#EXT-X-MAP:")) {
            auto a = attrs(l.substr(11));
            out.init.url = resolveUrl(url, a["URI"]);
            std::string br = a["BYTERANGE"];
            if (!br.empty()) { size_t atp = br.find('@'); out.init.length = _atoi64(br.c_str()); out.init.from = atp == std::string::npos ? 0 : _atoi64(br.c_str() + atp + 1); }
        } else if (startsWith(l, "#EXT-X-BYTERANGE:")) {
            std::string br = l.substr(17);
            size_t atp = br.find('@');
            brLen = _atoi64(br.c_str());
            brFrom = atp == std::string::npos ? nextOffset : _atoi64(br.c_str() + atp + 1);
        } else if (startsWith(l, "#EXTINF:")) pendingDur = atof(l.c_str() + 8);
        else if (l[0] != '#') {
            Piece p;
            p.url = resolveUrl(url, l);
            if (brLen >= 0) { p.from = brFrom; p.length = brLen; nextOffset = brFrom + brLen; }
            if (method == "AES-128") { p.encrypted = true; p.keyUrl = keyUrl; p.iv = keyIv.empty() ? ivFromSequence(seq) : keyIv; }
            out.pieces.push_back(p);
            seq++;
            brLen = brFrom = -1;
            (void)pendingDur;
        }
    }
    out.live = !ended;
    out.ext = !out.init.url.empty() ? "mp4" : "ts";
    if (out.pieces.empty()) { error = "The stream has no pieces."; return false; }
    return true;
}

// ------------------------------------------------------------------ DASH
bool parseDash(const std::string& xml, const std::string& mpdUrl, Manifest& out, std::string& error) {
    out = Manifest();
    size_t i = xml.find("<MPD");
    if (i == std::string::npos) { size_t p = xml.find(":MPD"); if (p != std::string::npos) i = xml.rfind('<', p); }
    if (i == std::string::npos) { error = "This isn't a DASH manifest."; return false; }
    XNode mpd;
    if (!parseXml(xml, i, mpd, 0)) { error = "The DASH manifest couldn't be read."; return false; }
    out.live = at(&mpd, "type") == "dynamic";
    out.duration = isoDuration(at(&mpd, "mediaPresentationDuration"));
    std::string base = mpdUrl;
    if (const XNode* b = child(mpd, "BaseURL")) base = resolveUrl(base, b->text);
    const XNode* period = nullptr;
    for (auto& k : mpd.kids) if (k.name == "Period") { period = &k; break; }
    if (!period) { error = "The DASH manifest has no content."; return false; }
    double periodDur = isoDuration(at(period, "duration"));
    if (periodDur <= 0) periodDur = out.duration;
    if (const XNode* b = child(*period, "BaseURL")) base = resolveUrl(base, b->text);
    for (auto& as : period->kids) {
        if (as.name != "AdaptationSet") continue;
        std::string asBase = base;
        if (const XNode* b = child(as, "BaseURL")) asBase = resolveUrl(asBase, b->text);
        const XNode* asTpl = child(as, "SegmentTemplate");
        bool protectedSet = child(as, "ContentProtection") != nullptr;
        for (auto& rp : as.kids) {
            if (rp.name != "Representation") continue;
            if (protectedSet || child(rp, "ContentProtection")) continue;   // DRM
            Representation r;
            r.id = at(&rp, "id");
            r.mime = at(&rp, "mimeType"); if (r.mime.empty()) r.mime = at(&as, "mimeType");
            r.codecs = at(&rp, "codecs"); if (r.codecs.empty()) r.codecs = at(&as, "codecs");
            std::string ct = at(&as, "contentType");
            r.video = startsWith(r.mime, "video") || ct == "video";
            r.audio = startsWith(r.mime, "audio") || ct == "audio";
            if (!r.video && !r.audio) {   // subtitles in one file (WebVTT)
                const XNode* tb = child(rp, "BaseURL");
                if ((r.mime == "text/vtt" || (ct == "text" && r.mime.empty())) && tb && !child(rp, "SegmentTemplate") && !asTpl && out.texts.size() < 200) {
                    r.lang = at(&as, "lang"); r.singleUrl = resolveUrl(asBase, tb->text);
                    out.texts.push_back(r);
                }
                continue;
            }
            r.bandwidth = _atoi64(at(&rp, "bandwidth").c_str());
            r.width = atoi((at(&rp, "width").empty() ? at(&as, "width") : at(&rp, "width")).c_str());
            r.height = atoi((at(&rp, "height").empty() ? at(&as, "height") : at(&rp, "height")).c_str());
            r.fps = parseRate(at(&rp, "frameRate").empty() ? at(&as, "frameRate") : at(&rp, "frameRate"));
            r.lang = at(&as, "lang");
            std::string rb = asBase;
            if (const XNode* b = child(rp, "BaseURL")) rb = resolveUrl(rb, b->text);
            const XNode* tpl = child(rp, "SegmentTemplate");
            if (!tpl) tpl = asTpl;
            const XNode* list = child(rp, "SegmentList");
            if (!list) list = child(as, "SegmentList");
            if (tpl) {
                // the representation's own values first, then the adaptation set's
                auto tv = [&](const char* a) { std::string v = at(child(rp, "SegmentTemplate"), a); return v.empty() ? at(asTpl, a) : v; };
                std::string media = tv("media"), init = tv("initialization");
                int64_t timescale = _atoi64(tv("timescale").c_str()); if (timescale <= 0) timescale = 1;
                int64_t startNumber = tv("startNumber").empty() ? 1 : _atoi64(tv("startNumber").c_str());
                int64_t dur = _atoi64(tv("duration").c_str());
                if (!init.empty()) r.initUrl = resolveUrl(rb, fillTemplate(init, r.id, startNumber, 0, r.bandwidth));
                const XNode* tl = child(*tpl, "SegmentTimeline");
                if (!tl && asTpl) tl = child(*asTpl, "SegmentTimeline");
                if (tl) {
                    int64_t t = 0, number = startNumber;
                    int64_t end = static_cast<int64_t>(periodDur * timescale);
                    for (auto& s : tl->kids) {
                        if (s.name != "S") continue;
                        if (!at(&s, "t").empty()) t = _atoi64(at(&s, "t").c_str());
                        int64_t d = _atoi64(at(&s, "d").c_str());
                        int64_t rr = at(&s, "r").empty() ? 0 : _atoi64(at(&s, "r").c_str());
                        if (d <= 0) break;
                        if (rr < 0) rr = end > t ? (end - t + d - 1) / d - 1 : 0;
                        for (int64_t k = 0; k <= rr && r.segments.size() < 200000; k++) { r.segments.push_back(resolveUrl(rb, fillTemplate(media, r.id, number++, t, r.bandwidth))); t += d; }
                    }
                } else if (dur > 0 && periodDur > 0) {
                    int64_t count = static_cast<int64_t>(std::ceil(periodDur * timescale / static_cast<double>(dur)));
                    for (int64_t k = 0; k < count && k < 200000; k++) r.segments.push_back(resolveUrl(rb, fillTemplate(media, r.id, startNumber + k, k * dur, r.bandwidth)));
                }
                if (r.segments.empty()) continue;
            } else if (list) {
                if (const XNode* in = child(*list, "Initialization")) r.initUrl = resolveUrl(rb, at(in, "sourceURL"));
                for (auto& s : list->kids) if (s.name == "SegmentURL") r.segments.push_back(resolveUrl(rb, at(&s, "media").empty() ? "" : at(&s, "media")));
                if (r.segments.empty()) continue;
            } else r.singleUrl = rb;   // SegmentBase or a bare BaseURL: one file
            out.reps.push_back(r);
        }
    }
    if (out.reps.empty()) { error = out.live ? "Live DASH streams can't be saved." : "The DASH manifest has nothing DDM can save (it may be protected)."; return false; }
    return true;
}

bool readDash(const RequestInfo& r, Manifest& out, std::string& error) {
    std::string xml;
    long st = fetchText(r, r.url, xml);
    if (st != 200) { error = st == 0 ? "Couldn't reach the video's server." : "The video's server answered " + std::to_string(st) + "."; return false; }
    return parseDash(xml, r.url, out, error);
}

Plan dashPlan(const Representation& rep) {
    Plan p;
    p.ext = rep.audio && !rep.video ? "m4a" : "mp4";
    if (!rep.initUrl.empty()) p.init.url = rep.initUrl;
    for (auto& s : rep.segments) { Piece x; x.url = s; p.pieces.push_back(x); }
    return p;
}

// ------------------------------------------------------------------ fetching
DownloadResult fetch(const RequestInfo& base, const Plan& plan0, const std::string& outFile, int connections, int limitKBps,
                     std::atomic<bool>* stop, std::function<void(const Progress&)> onProgress, std::string& error) {
    Plan plan = plan0;
    std::string cookieOwner = !base.referer.empty() ? base.referer : !plan.playlistUrl.empty() ? plan.playlistUrl : base.url;
    std::string dir = outFile + ".pieces";
    makeDirs(dir);
    auto pieceFile = [&](size_t i) { char b[32]; snprintf(b, sizeof b, "%07u.part", static_cast<unsigned>(i)); return joinPath(dir, b); };
    std::mutex mu;
    std::map<std::string, std::string> keys;
    std::atomic<int64_t> bytes{0};
    std::atomic<int> active{0}, done{0};
    std::string fatal;
    // pieces already here (from before a pause)
    {   // pieces kept from before belong to this same stream only (same number of pieces, same kind)
        std::string sig = std::to_string(plan.pieces.size()) + "|" + plan.ext + "|" + (plan.init.url.empty() ? "" : "init") + (plan.live ? "|live" : "");
        std::string was;
        readFile(joinPath(dir, "plan.txt"), was);
        if (was != sig) { removeTree(dir); makeDirs(dir); writeFileAtomic(joinPath(dir, "plan.txt"), sig); }
    }
    std::vector<char> have(plan.pieces.size(), 0);
    int64_t haveBytes = 0;
    for (size_t i = 0; i < plan.pieces.size(); i++) { int64_t s = fileSize(pieceFile(i)); if (s > 0) { have[i] = 1; haveBytes += s; done++; } }
    bytes = haveBytes;
    auto get = [&](const Piece& p, std::string& body, http::Session* ses = nullptr) -> long {
        RequestInfo r = base;
        r.url = p.url;
        if (p.from >= 0 && p.length > 0) r.headers["Range"] = "bytes=" + std::to_string(p.from) + "-" + std::to_string(p.from + p.length - 1);
        body.clear();
        if (!sameSite(r.url, cookieOwner)) r.cookies.clear();   // the page's cookies go to the page's own site only
        const int64_t most = 256LL * 1024 * 1024;   // a piece is never bigger (a playlist can't make DDM run out of memory)
        return ses ? ses->get(r, body, nullptr, most, 180) : http::get(r, body, nullptr, most, 180);
    };
    auto keyFor = [&](const std::string& url) -> std::string {
        { std::lock_guard<std::mutex> g(mu); auto it = keys.find(url); if (it != keys.end()) return it->second; }
        std::string k;
        Piece kp; kp.url = url;
        for (int i = 0; i < 4 && k.size() != 16; i++) { if (get(kp, k) != 200) k.clear(); if (k.size() != 16) Sleep(500); }
        std::lock_guard<std::mutex> g(mu);
        keys[url] = k;
        return k;
    };
    // the initialization piece (fMP4 / DASH)
    std::string initPath = joinPath(dir, "init.part");
    if (!plan.init.url.empty() && fileSize(initPath) <= 0) {
        std::string body;
        long st = 0;
        for (int i = 0; i < 4; i++) { st = get(plan.init, body); if ((st == 200 || st == 206) && !body.empty()) break; Sleep(700 * (i + 1)); }
        if ((st != 200 && st != 206) || body.empty() || !writeFileAtomic(initPath, body)) { error = st == 403 || st == 404 || st == 410 ? "The video's address expired. Open the video's page again (Refresh download link)." : "Couldn't get the start of the video."; return DownloadResult::Failed; }
    }
    int workers = std::max(1, std::min(16, connections));
    int64_t started = nowMs();
    std::atomic<size_t> next{0};
    std::set<std::string> seen;
    for (auto& p : plan.pieces) seen.insert(p.url + "#" + std::to_string(p.from));
    std::atomic<int> alive{0};
    auto work = [&] {
        struct Leave { std::atomic<int>& a; ~Leave() { a--; } } leave{alive};
        http::Session ses;   // one kept-open connection per worker: no new handshake for every piece
        for (;;) {
            if ((stop && stop->load())) return;
            { std::lock_guard<std::mutex> g(mu); if (!fatal.empty()) return; }
            size_t i = next.fetch_add(1);
            Piece p;
            {
                std::lock_guard<std::mutex> g(mu);
                if (i >= plan.pieces.size()) return;
                if (have[i]) continue;
                p = plan.pieces[i];
            }
            while (limitKBps > 0 && !(stop && stop->load())) {   // the speed limit: wait until the average is below it
                double secs = (nowMs() - started) / 1000.0;
                if (secs <= 0 || (bytes.load() - haveBytes) / 1024.0 / secs <= limitKBps) break;
                Sleep(100);
            }
            active++;
            std::string body;
            long st = 0;
            int tries = 0;
            for (;;) {
                st = get(p, body, &ses);
                if ((st == 200 || st == 206) && !body.empty()) break;
                if (stop && stop->load()) break;
                if (++tries >= 6 || ((st == 403 || st == 404 || st == 410) && tries >= 3)) break;
                Sleep(std::min(8000, 600 << tries));
            }
            active--;
            if (stop && stop->load()) return;
            if ((st != 200 && st != 206) || body.empty()) {
                std::lock_guard<std::mutex> g(mu);
                if (fatal.empty()) fatal = st == 403 || st == 404 || st == 410 ? "The video's address expired. Open the video's page again (Refresh download link)." : st == 0 ? "The connection to the video's server was lost." : "The video's server answered " + std::to_string(st) + ".";
                return;
            }
            if (p.encrypted) {
                std::string key = keyFor(p.keyUrl);
                if (key.size() != 16 || !aesDecrypt(key, p.iv, body)) { std::lock_guard<std::mutex> g(mu); if (fatal.empty()) fatal = "A piece of the video couldn't be decrypted."; return; }
            }
            if (!writeFileAtomic(pieceFile(i), body)) { std::lock_guard<std::mutex> g(mu); if (fatal.empty()) fatal = "Couldn't write to the disk (is it full?)."; return; }
            bytes += static_cast<int64_t>(body.size());
            done++;
            std::lock_guard<std::mutex> g(mu);
            have[i] = 1;
        }
    };
    double speed = 0;
    int64_t lastBytes = bytes.load(), lastAt = nowMs();
    bool liveStopped = false;
    for (;;) {
        std::vector<std::thread> ts;
        alive = workers;
        for (int w = 0; w < workers; w++) ts.emplace_back(work);
        while (alive.load() > 0) {
            Sleep(250);
            int64_t now = nowMs(), b = bytes.load();
            if (now - lastAt >= 500) { double inst = (b - lastBytes) * 1000.0 / std::max<int64_t>(1, now - lastAt); speed = speed <= 0 ? inst : speed * 0.6 + inst * 0.4; lastBytes = b; lastAt = now; }
            Progress pr;
            pr.done = b; pr.speed = speed; pr.active = active.load(); pr.piecesDone = done.load(); pr.pieces = static_cast<int>(plan.pieces.size());
            if (pr.piecesDone > 0 && !plan.live) pr.estimate = static_cast<int64_t>(static_cast<double>(b) / pr.piecesDone * pr.pieces);
            if (onProgress) onProgress(pr);
        }
        for (auto& t : ts) t.join();
        if (!fatal.empty()) { error = fatal; return DownloadResult::Failed; }
        if (stop && stop->load()) { if (!plan.live) return DownloadResult::Stopped; liveStopped = true; }
        if (!plan.live || liveStopped) break;
        // live: read the list again for new pieces (until the stream ends or you stop it)
        Plan fresh;
        std::string e2;
        for (int waited = 0; waited < 6000 && !(stop && stop->load()); waited += 250) Sleep(250);
        if (stop && stop->load()) { liveStopped = true; break; }
        if (!hlsPlan(base, plan.playlistUrl, fresh, e2)) continue;
        std::lock_guard<std::mutex> g(mu);
        for (auto& p : fresh.pieces) if (seen.insert(p.url + "#" + std::to_string(p.from)).second) { plan.pieces.push_back(p); have.push_back(0); }
        plan.live = fresh.live;
        next = 0;
    }
    // every piece is here: one file, in order
    for (size_t i = 0; i < plan.pieces.size(); i++) if (!have[i] && !liveStopped) { error = "A piece of the video is missing."; return DownloadResult::Failed; }
    std::string tmp = outFile + ".joining";
    HANDLE f = CreateFileW(W(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = "Couldn't write to the disk."; return DownloadResult::Failed; }
    auto append = [&](const std::string& path) {
        std::string data;
        if (!readFile(path, data)) return false;
        DWORD put = 0;
        return WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size();
    };
    bool ok = plan.init.url.empty() || append(initPath);
    for (size_t i = 0; ok && i < plan.pieces.size(); i++) if (have[i]) ok = append(pieceFile(i));
    CloseHandle(f);
    if (!ok || !MoveFileExW(W(tmp).c_str(), W(outFile).c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(W(tmp).c_str()); error = "Couldn't put the video together (is the disk full?)."; return DownloadResult::Failed; }
    removeTree(dir);
    return DownloadResult::Done;
}

bool hlsText(const RequestInfo& r, const std::string& playlistUrl, std::string& out, std::string& error) {
    Plan plan;
    if (!hlsPlan(r, playlistUrl, plan, error)) {
        // not a list after all, but the subtitle file itself
        std::string body;
        if (fetchText(r, playlistUrl, body) == 200 && body.find("-->") != std::string::npos) { out = body; error.clear(); return true; }
        return false;
    }
    if (plan.pieces.size() > 4000) { error = "The subtitles are in too many pieces."; return false; }
    // four at a time, kept in order
    std::vector<std::string> parts(plan.pieces.size());
    std::atomic<size_t> next{0};
    std::atomic<int> failed{0};
    std::vector<std::thread> th;
    for (int w = 0; w < 4; w++) th.emplace_back([&] {
        for (;;) {
            size_t i = next++;
            if (i >= plan.pieces.size() || failed > 8) return;
            if (plan.pieces[i].encrypted) { failed++; continue; }
            if (fetchText(r, plan.pieces[i].url, parts[i]) != 200) { parts[i].clear(); failed++; }
        }
    });
    for (auto& t : th) t.join();
    size_t total = 0;
    for (auto& p : parts) { total += p.size(); if (total > 32u * 1024 * 1024) { error = "The subtitles are too large."; return false; } out += p; out += "\n\n"; }
    if (out.find("-->") == std::string::npos) { error = "The subtitles couldn't be read."; return false; }
    return true;
}

} // namespace streams
} // namespace ddm
