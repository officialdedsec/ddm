#include "sites.hpp"
#include "http.hpp"
#include "streams.hpp"
#include "siterules.hpp"
#include "crypto.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <ctime>
#include <mutex>

namespace ddm {
namespace sites {

namespace {

const char* kChrome = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36";

std::string siteOf(const std::string& url) { return urlSite(url); }
bool on(const std::string& site, const char* d) { return site == d || endsWith(site, std::string(".") + d); }

// "https:\/\/x.com\/a\u0026b" -> "https://x.com/a&b"
std::string unescapeJs(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char c = s[++i];
        if (c == 'u' && i + 4 < s.size()) {
            unsigned v = static_cast<unsigned>(strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
            if (v < 0x80) o += static_cast<char>(v);
            else if (v < 0x800) { o += static_cast<char>(0xC0 | (v >> 6)); o += static_cast<char>(0x80 | (v & 0x3F)); }
            else { o += static_cast<char>(0xE0 | (v >> 12)); o += static_cast<char>(0x80 | ((v >> 6) & 0x3F)); o += static_cast<char>(0x80 | (v & 0x3F)); }
        } else if (c == 'n') o += '\n';
        else if (c == 't') o += '\t';
        else if (c == 'r') {}
        else o += c;   // \/ \" \\ …
    }
    return o;
}

std::string htmlDecode(std::string s) {
    s = replaceAll(s, "&quot;", "\""); s = replaceAll(s, "&#39;", "'"); s = replaceAll(s, "&#039;", "'"); s = replaceAll(s, "&lt;", "<"); s = replaceAll(s, "&gt;", ">");
    s = replaceAll(s, "&#x2F;", "/"); s = replaceAll(s, "&#47;", "/");
    return replaceAll(s, "&amp;", "&");
}

// a JSON value starting at text[p] ('{' or '['), braces balanced, strings respected
std::string balanced(const std::string& text, size_t p) {
    if (p >= text.size() || (text[p] != '{' && text[p] != '[')) return "";
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = p; i < text.size(); i++) {
        char c = text[i];
        if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; continue; }
        if (c == '"') inStr = true;
        else if (c == '{' || c == '[') depth++;
        else if ((c == '}' || c == ']') && --depth == 0) return text.substr(p, i - p + 1);
    }
    return "";
}
Json jsonAt(const std::string& text, const std::string& marker, size_t from = 0) {
    size_t p = text.find(marker, from);
    if (p == std::string::npos) return Json();
    p = text.find_first_of("{[", p + marker.size());
    if (p == std::string::npos) return Json();
    return Json::parse(balanced(text, p));
}
// the string value after "key":"…"
std::string jsonString(const std::string& text, const std::string& key, size_t from = 0) {
    std::string m = "\"" + key + "\":\"";
    size_t p = text.find(m, from);
    if (p == std::string::npos) { m = "\"" + key + "\": \""; p = text.find(m, from); }
    if (p == std::string::npos) return "";
    p += m.size();
    std::string v;
    for (size_t i = p; i < text.size(); i++) { if (text[i] == '\\' && i + 1 < text.size()) { v += text[i]; v += text[++i]; continue; } if (text[i] == '"') break; v += text[i]; }
    return unescapeJs(v);
}

// every <name …> tag of a page (no regular expressions over the whole page: fast, and safe on huge pages)
std::vector<std::string> tagsOf(const std::string& html, const std::string& low, const char* name, size_t most = 500) {
    std::vector<std::string> out;
    std::string open = std::string("<") + name;
    for (size_t at = 0; out.size() < most && (at = low.find(open, at)) != std::string::npos; at += open.size()) {
        size_t after = at + open.size();
        if (after < low.size() && !(low[after] == ' ' || low[after] == '\t' || low[after] == '\n' || low[after] == '\r' || low[after] == '/' || low[after] == '>')) continue;   // <videos…>
        size_t end = low.find('>', after);
        if (end == std::string::npos || end - at > 8192) continue;
        out.push_back(html.substr(at, end - at + 1));
    }
    return out;
}
// an attribute's value in one tag (src="…", src='…', src=…)
std::string attrOf(const std::string& tag, const char* name) {
    std::string l = lower(tag), n = name;
    for (size_t at = 0; (at = l.find(n, at)) != std::string::npos; at += n.size()) {
        if (at == 0 || !(l[at - 1] == ' ' || l[at - 1] == '\t' || l[at - 1] == '\n' || l[at - 1] == '\r')) continue;
        size_t i = at + n.size();
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t' || l[i] == '\n' || l[i] == '\r')) i++;
        if (i >= l.size() || l[i] != '=') continue;
        i++;
        while (i < l.size() && (l[i] == ' ' || l[i] == '\t' || l[i] == '\n' || l[i] == '\r')) i++;
        if (i >= l.size()) return "";
        if (tag[i] == '"' || tag[i] == '\'') { size_t e = tag.find(tag[i], i + 1); return e == std::string::npos ? "" : htmlDecode(tag.substr(i + 1, e - i - 1)); }
        size_t e = i;
        while (e < tag.size() && tag[e] != ' ' && tag[e] != '>' && tag[e] != '\t' && tag[e] != '\n') e++;
        return htmlDecode(tag.substr(i, e - i));
    }
    return "";
}
std::string meta(const std::string& html, const std::string& name) {   // <meta property="og:title" content="…">
    std::string low = lower(html);
    for (auto& t : tagsOf(html, low, "meta", 2000)) {
        std::string v = attrOf(t, "property");
        if (v.empty()) v = attrOf(t, "name");
        if (v.empty()) v = attrOf(t, "itemprop");
        if (lower(v) == lower(name)) return attrOf(t, "content");
    }
    return "";
}

std::string pageTitle(const std::string& html) {
    std::string t = meta(html, "og:title");
    if (t.empty()) t = meta(html, "twitter:title");
    if (t.empty()) {
        size_t a = lower(html).find("<title");
        if (a != std::string::npos) { a = html.find('>', a); size_t b = html.find("</", a); if (a != std::string::npos && b != std::string::npos) t = htmlDecode(trim(html.substr(a + 1, b - a - 1))); }
    }
    return trim(t);
}

int heightHint(const std::string& s) {   // "…_720p.mp4", "/1080/", "hd1080"
    static const std::regex re("(?:^|[^0-9])(2160|1440|1080|720|540|480|360|240|144)(?:p|_|/|\\.|-|$)");
    std::smatch m;
    if (std::regex_search(s, m, re)) return atoi(m[1].str().c_str());
    return 0;
}

std::string extOfUrl(const std::string& u) {
    std::string l = lower(u);
    size_t q = l.find_first_of("?#");
    if (q != std::string::npos) l = l.substr(0, q);
    size_t dot = l.rfind('.'), sl = l.rfind('/');
    if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return "";
    return l.substr(dot + 1);
}

long getPage(const std::string& url, const std::string& cookies, const std::string& ua, const std::string& referer, std::string& body, ResponseInfo* info = nullptr,
             const std::map<std::string, std::string>& extra = {}) {
    RequestInfo r;
    r.url = url; r.cookies = cookies; r.userAgent = ua.empty() ? kChrome : ua; r.referer = referer;
    r.headers["Accept-Language"] = "en-US,en;q=0.9";
    r.headers["Accept"] = "text/html,application/xhtml+xml,application/xml;q=0.9,application/json;q=0.8,*/*;q=0.7";
    for (auto& kv : extra) r.headers[kv.first] = kv.second;
    return http::get(r, body, info, 16 * 1024 * 1024, 30);
}

void addSource(Page& p, const std::string& url0, const std::string& kind, int height = 0, int64_t size = -1, const std::string& label = "") {
    std::string url = trim(url0);
    if (url.empty() || !(startsWith(lower(url), "http://") || startsWith(lower(url), "https://"))) return;   // web addresses only (never file:, data:, blob:…)
    for (auto& s : p.sources) if (s.url == url) { if (height && !s.height) s.height = height; return; }
    Source s;
    s.url = url; s.kind = kind; s.height = height ? height : heightHint(url); s.size = size; s.label = label;
    s.ext = kind == "file" || kind == "video" || kind == "audio" ? extOfUrl(url) : "";
    if (s.ext.empty() && kind == "file") s.ext = "mp4";
    p.sources.push_back(s);
}

// ------------------------------------------------------------------ TikTok
void scriptData(const std::string& html, const std::string& low, const std::string& pageUrl, Page& p);
bool tiktokItem(const Json& item, const std::string& cookies, Page& p) {
    if (!item.isObject()) return false;
    const Json& v = item["video"];
    if (!item["id"].str().empty()) p.id = item["id"].str();
    p.title = item["desc"].str();
    p.author = item["author"].isObject() ? item["author"]["uniqueId"].str() : item["author"].str();
    p.duration = v["duration"].i32();
    p.thumbnail = v["originCover"].str(v["cover"].str());
    p.headers["Referer"] = "https://www.tiktok.com/";
    if (!cookies.empty()) p.headers["Cookie"] = cookies;   // TikTok's files want the page's cookies back
    for (auto& b : v["bitrateInfo"].list()) {
        const Json& pa = b["PlayAddr"];
        std::string u;
        for (auto& x : pa["UrlList"].list()) { u = x.str(); if (!u.empty()) break; }
        int w = pa["Width"].i32(), h = pa["Height"].i32();
        int q = w > 0 && h > 0 ? std::min(w, h) : h;   // (the short side names the quality: 1080 for 1080×1920)
        int64_t size = pa["DataSize"].isString() ? _atoi64(pa["DataSize"].str().c_str()) : pa["DataSize"].i64(-1);
        addSource(p, u, "file", q, size > 0 ? size : -1, b["GearName"].str());
    }
    int sh = std::min(v["width"].i32(), v["height"].i32());
    addSource(p, v["playAddr"].str(), "file", sh);
    addSource(p, v["downloadAddr"].str(), "file", sh);
    if (item["music"].isObject() && !p.sources.empty()) addSource(p, item["music"]["playUrl"].str(), "audio");
    if (p.sources.empty() && item["imagePost"].isObject()) {   // a photo slideshow: pictures and a song, no video
        if (item["music"].isObject()) addSource(p, item["music"]["playUrl"].str(), "audio");
        if (p.thumbnail.empty())
            for (auto& im : item["imagePost"]["images"].list()) {
                for (auto& u : im["imageURL"]["urlList"].list()) if (p.thumbnail.empty()) p.thumbnail = u.str();
                if (!p.thumbnail.empty()) break;
            }
        if (p.sources.empty()) p.error = "This TikTok post is a photo slideshow without a video.";
    }
    return !p.sources.empty();
}
// the first object under `key` anywhere in a JSON tree
const Json* findKey(const Json& j, const std::string& key, int depth = 0) {
    if (depth > 24) return nullptr;
    if (j.isObject()) {
        for (auto& kv : j.items()) if (kv.first == key && (kv.second.isObject() || kv.second.isArray())) return &kv.second;
        for (auto& kv : j.items()) if (const Json* r = findKey(kv.second, key, depth + 1)) return r;
    } else if (j.isArray()) {
        for (auto& x : j.list()) if (const Json* r = findKey(x, key, depth + 1)) return r;
    }
    return nullptr;
}
// TikTok's embed page (/embed/v2/ID): videoData { itemInfos { id, text, covers, video.urls }, authorInfos { uniqueId } }
bool tiktokEmbedData(const std::string& html, const std::string& cookies, Page& p) {
    Json st = jsonAt(html, "id=\"__FRONTITY_CONNECT_STATE__\"");
    const Json* vd = findKey(st, "videoData");
    if (!vd) return false;
    const Json& ii = (*vd)["itemInfos"];
    const Json& ai = (*vd)["authorInfos"];
    if (!ii.isObject()) return false;
    if (!ii["id"].str().empty()) p.id = ii["id"].str();
    if (p.title.empty()) p.title = ii["text"].str();
    if (p.author.empty()) p.author = ai["uniqueId"].str();
    for (auto& c : ii["covers"].list()) if (p.thumbnail.empty()) p.thumbnail = c.str();
    if (p.thumbnail.empty()) p.thumbnail = ii["coversOrigin"].list().empty() ? "" : ii["coversOrigin"].list()[0].str();
    const Json& v = ii["video"];
    int sh = std::min(v["videoMeta"]["width"].i32(), v["videoMeta"]["height"].i32());
    if (p.duration <= 0) p.duration = v["videoMeta"]["duration"].i32();
    for (auto& u : v["urls"].list()) addSource(p, u.str(), "file", sh);
    if (p.sources.empty()) return false;
    p.headers["Referer"] = "https://www.tiktok.com/";
    if (!cookies.empty()) p.headers["Cookie"] = cookies;
    return true;
}
// tests only (DDM_TEST_UI with DDM_TEST_TIKTOK=http://127.0.0.1:port): a stand-in TikTok that pages are read from
std::string ttBase() {
    static std::string b = [] {
        char v[256] = "";
        if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_TIKTOK", v, sizeof v)) return std::string(v);
        return std::string("https://www.tiktok.com");
    }();
    return b;
}
std::string pct(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
// TikTok's public oEmbed: the caption, the creator and the cover picture of any public video
void tiktokOembed(const std::string& id, const std::string& user, const std::string& ua, Page& p) {
    if (id.empty()) return;
    for (const std::string& who : {user, std::string("_")}) {
        if (who.empty()) continue;
        std::string body, jar;
        long st = getPage(ttBase() + "/oembed?url=" + pct("https://www.tiktok.com/@" + who + "/video/" + id), jar, ua, "", body, nullptr);
        if (st != 200) continue;
        bool ok = false;
        Json o = Json::parse(body, &ok);
        if (!ok || !o.isObject()) continue;
        std::string t = o["title"].str();
        if (!t.empty() && (p.title.empty() || lower(p.title).find("tiktok") != std::string::npos)) p.title = t;
        if (p.author.empty()) {   // "@handle" from the creator's address; the display name when it has none
            std::string au = o["author_url"].str();
            size_t at = au.find("/@");
            p.author = o["author_unique_id"].str(at != std::string::npos ? au.substr(at + 2) : o["author_name"].str());
        }
        if (p.thumbnail.empty()) p.thumbnail = o["thumbnail_url"].str();
        return;
    }
}
// Every way a TikTok address is written (/@user/video/ID, /@/video/ID from a feed, /embed/v2/ID, /v/ID.html, short
// vm.tiktok.com links): the video page is read with its user's name or without it, then the embed page — each one
// with the cookies the one before handed out.
bool tiktok(const std::string& url, const std::string& cookies, const std::string& ua, Page& p) {
    static const std::regex idRe("/(?:video|photo|v|embed/v2|embed|player/v1)/([0-9]{15,})");
    static const std::regex userRe("/@([^/?#]*)");
    std::smatch m;
    std::string id = std::regex_search(url, m, idRe) ? m[1].str() : "";
    std::string user = std::regex_search(url, m, userRe) ? m[1].str() : "";
    std::vector<std::string> tries;
    auto add = [&](const std::string& u) { if (std::find(tries.begin(), tries.end(), u) == tries.end()) tries.push_back(u); };
    if (id.empty()) add(url);   // a short link: it leads to the video page
    else {
        if (!user.empty()) add(ttBase() + "/@" + user + "/video/" + id);
        add(ttBase() + "/@_/video/" + id);   // TikTok finds the video by its number alone
        add(ttBase() + "/embed/v2/" + id);
    }
    std::string jar = cookies;
    for (auto& t : tries) {
        std::string html;
        ResponseInfo info;
        long st = getPage(t, jar, ua, "https://www.tiktok.com/", html, &info);
        if (!info.setCookies.empty()) jar += (jar.empty() ? "" : "; ") + info.setCookies;
        if (st != 200) continue;
        if (id.empty()) {   // where the short link led
            std::string canon = meta(html, "og:url");
            if (std::regex_search(canon, m, idRe)) id = m[1].str();
        }
        Json item;
        Json data = jsonAt(html, "id=\"__UNIVERSAL_DATA_FOR_REHYDRATION__\"");
        if (data.isObject()) {
            const Json& scope = data["__DEFAULT_SCOPE__"];
            for (const char* k : {"webapp.video-detail", "webapp.reflow.video.detail"}) {
                const Json& d = scope[k];
                if (d["itemInfo"]["itemStruct"].isObject()) { item = d["itemInfo"]["itemStruct"]; break; }
                int code = d["statusCode"].i32();
                if (code == 10204 || code == 10216 || code == 10222) p.error = "This TikTok video is private, was removed, or needs you to be signed in.";
            }
        }
        if (!item.isObject()) {   // older pages
            Json sigi = jsonAt(html, "id=\"SIGI_STATE\"");
            for (auto& kv : sigi["ItemModule"].items()) { item = kv.second; break; }
        }
        if (tiktokItem(item, jar, p)) { if (p.title.empty() || p.thumbnail.empty()) tiktokOembed(id, user.empty() ? p.author : user, ua, p); return true; }
        if (tiktokEmbedData(html, jar, p)) { if (p.title.empty() || p.thumbnail.empty()) tiktokOembed(id, user.empty() ? p.author : user, ua, p); return true; }
        scriptData(html, lower(html), t, p);   // the embed player and new page layouts: any video address in the page's data
        if (!p.sources.empty()) {
            if (p.id.empty()) p.id = id;
            if (p.author.empty()) p.author = user;
            p.title.clear();   // the page's own title ("TikTok Embed", "TikTok - Make Your Day") names nothing
            tiktokOembed(id, user, ua, p);
            if (p.thumbnail.empty()) p.thumbnail = meta(html, "og:image");
            p.headers["Referer"] = "https://www.tiktok.com/";
            if (!jar.empty()) p.headers["Cookie"] = jar;
            return true;
        }
    }
    if (p.id.empty()) p.id = id;
    if (p.author.empty()) p.author = user;
    tiktokOembed(id, user, ua, p);   // even when the video itself can't be read here: its real name and picture for the window
    return false;
}

// ------------------------------------------------------------------ Bilibili (bilibili.com, b23.tv, bilibili.tv)
// tests only (DDM_TEST_UI with DDM_TEST_BILIBILI=http://127.0.0.1:port): a stand-in Bilibili
long biliGet(const std::string& url, const std::string& cookies, const std::string& ua, const std::string& referer, std::string& body, ResponseInfo* info = nullptr) {
    static std::string base = [] {
        char v[256] = "";
        if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_BILIBILI", v, sizeof v)) return std::string(v);
        return std::string();
    }();
    std::string u = url;
    if (!base.empty())
        for (const char* h : {"https://api.bilibili.com", "https://www.bilibili.com", "https://api.bilibili.tv", "https://www.bilibili.tv", "https://b23.tv"})
            if (startsWith(u, h)) { u = base + u.substr(strlen(h)); break; }
    return getPage(u, cookies, ua, referer, body, info);
}
// The player's own API: every quality as separate picture and sound files (DASH), which DDM downloads with many
// connections each and joins. Signed the way the site's player signs it (WBI); the older unsigned address and the
// page's own player data are the fallbacks.
std::string biliMixinKey(const std::string& cookies, const std::string& ua) {
    static std::mutex mu;
    static std::string key;
    static int64_t at = 0;
    std::lock_guard<std::mutex> g(mu);
    if (!key.empty() && nowMs() - at < 3600 * 1000) return key;
    std::string body;
    if (biliGet("https://api.bilibili.com/x/web-interface/nav", cookies, ua, "https://www.bilibili.com/", body) != 200) return key;
    Json j = Json::parse(body);
    auto stem = [](const std::string& u) { std::string f = u.substr(u.find_last_of('/') + 1); return f.substr(0, f.find('.')); };
    std::string raw = stem(j["data"]["wbi_img"]["img_url"].str()) + stem(j["data"]["wbi_img"]["sub_url"].str());
    static const int tab[] = {46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35, 27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
                              37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4, 22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52};
    if (raw.size() < 64) return key;
    std::string k;
    for (int i : tab) { if (k.size() >= 32) break; k += raw[static_cast<size_t>(i)]; }
    key = k;
    at = nowMs();
    return key;
}
std::string biliQuery(const std::map<std::string, std::string>& q) {
    std::string out;
    for (auto& kv : q) out += (out.empty() ? "" : "&") + pct(kv.first) + "=" + pct(kv.second);
    return out;
}
std::string biliSigned(std::map<std::string, std::string> q, const std::string& mixin) {
    q["wts"] = std::to_string(static_cast<long long>(time(nullptr)));
    for (auto& kv : q) { std::string v; for (char c : kv.second) if (!strchr("!'()*", c)) v += c; kv.second = v; }
    std::string qs = biliQuery(q);
    return qs + "&w_rid=" + crypto::hex(crypto::md5(qs + mixin));
}
// picture and sound from the player's answer ("dash": one file per quality; "durl": older single files)
bool biliStreams(const Json& d, Page& p) {
    size_t before = p.sources.size();
    const Json& dash = d["dash"];
    if (dash.isObject()) {
        std::map<int, Json> best;   // per quality: AVC first (plays everywhere), then HEVC, then AV1; the richest of each
        std::map<int, int> rank;
        for (auto& v : dash["video"].list()) {
            std::string c = lower(v["codecs"].str());
            int r = startsWith(c, "avc") ? 3 : (startsWith(c, "hev") || startsWith(c, "hvc")) ? 2 : 1;
            int w = v["width"].i32(), h = v["height"].i32();
            int q = w > 0 && h > 0 ? std::min(w, h) : h;   // the short side names the quality (portrait videos too)
            if (q <= 0) continue;
            if (!best.count(q) || r > rank[q] || (r == rank[q] && v["bandwidth"].i64() > best[q]["bandwidth"].i64())) { best[q] = v; rank[q] = r; }
        }
        for (auto& kv : best) {
            const Json& v = kv.second;
            std::string u = v["baseUrl"].str(v["base_url"].str());
            if (u.empty()) for (auto& b : v["backupUrl"].list()) { u = b.str(); if (!u.empty()) break; }
            size_t n = p.sources.size();
            addSource(p, u, "video", kv.first);
            if (p.sources.size() > n) { p.sources.back().ext = "mp4"; p.sources.back().kbps = v["bandwidth"].i64() / 1000.0; }
        }
        for (auto& a : dash["audio"].list()) {
            std::string u = a["baseUrl"].str(a["base_url"].str());
            if (u.empty()) for (auto& b : a["backupUrl"].list()) { u = b.str(); if (!u.empty()) break; }
            size_t n = p.sources.size();
            addSource(p, u, "audio");
            if (p.sources.size() > n) { p.sources.back().ext = "m4a"; p.sources.back().kbps = a["bandwidth"].i64() / 1000.0; }
        }
    }
    for (auto& x : d["durl"].list()) addSource(p, x["url"].str(), "file", 0, x["size"].i64(-1));
    return p.sources.size() > before;
}
bool bilibiliTv(const std::string& url, const std::string& cookies, const std::string& ua, Page& p) {
    static const std::regex playRe("/play/[0-9]+/([0-9]+)"), videoRe("/video/([0-9]+)");
    std::smatch m;
    std::string ep = std::regex_search(url, m, playRe) ? m[1].str() : "";
    std::string aid = ep.empty() && std::regex_search(url, m, videoRe) ? m[1].str() : "";
    if (ep.empty() && aid.empty()) return false;
    const std::string ref = "https://www.bilibili.tv/";
    std::string html;
    if (biliGet(url, cookies, ua, ref, html) == 200) {
        p.title = meta(html, "og:title");
        p.thumbnail = meta(html, "og:image");
        p.description = meta(html, "og:description");
    }
    std::map<std::string, std::string> q = {{"s_locale", "en_US"}, {"platform", "web"}, {"qn", "112"}, {"type", "0"}, {"device", "wap"}, {"tf", "0"}};
    if (!ep.empty()) q["ep_id"] = ep; else q["aid"] = aid;
    std::string body;
    if (biliGet("https://api.bilibili.tv/intl/gateway/web/playurl?" + biliQuery(q), cookies, ua, ref, body) != 200) return false;
    Json r = Json::parse(body);
    const Json& pu = r["data"]["playurl"];
    for (auto& v : pu["video"].list()) {
        const Json& vr = v["video_resource"];
        std::string u = vr["url"].str();
        if (u.empty()) for (auto& b : vr["backup_url"].list()) { u = b.str(); if (!u.empty()) break; }
        int w = vr["width"].i32(), h = vr["height"].i32();
        size_t n = p.sources.size();
        addSource(p, u, "video", w > 0 && h > 0 ? std::min(w, h) : h);
        if (p.sources.size() > n) { p.sources.back().ext = "mp4"; p.sources.back().kbps = vr["bandwidth"].i64() / 1000.0; }
    }
    for (auto& a : pu["audio_resource"].list()) {
        std::string u = a["url"].str();
        if (u.empty()) for (auto& b : a["backup_url"].list()) { u = b.str(); if (!u.empty()) break; }
        size_t n = p.sources.size();
        addSource(p, u, "audio");
        if (p.sources.size() > n) { p.sources.back().ext = "m4a"; p.sources.back().kbps = a["bandwidth"].i64() / 1000.0; }
    }
    if (p.duration <= 0) p.duration = static_cast<int>(pu["duration"].i64() / 1000);
    p.id = ep.empty() ? aid : ep;
    p.headers["Referer"] = ref;
    if (p.sources.empty() && p.error.empty()) p.error = "Bilibili didn't give DDM this video (it may need you to be signed in, or be for members only).";
    return !p.sources.empty();
}
bool bilibili(const std::string& url0, const std::string& cookies, const std::string& ua, Page& p) {
    std::string url = url0;
    const std::string ref = "https://www.bilibili.com/";
    std::string jar = cookies, body, html;
    ResponseInfo info;
    if (lower(urlHost(url)).find("b23.tv") != std::string::npos) {   // a short link: where it leads
        biliGet(url, jar, ua, ref, html, &info);
        if (!info.finalUrl.empty()) url = info.finalUrl;
        else { std::string og = meta(html, "og:url"); if (!og.empty()) url = og; }
    }
    if (on(urlSite(url), "bilibili.tv")) return bilibiliTv(url, cookies, ua, p);
    static const std::regex bvRe("(BV[0-9A-Za-z]{10})"), avRe("/av([0-9]+)"), epRe("/ep([0-9]+)"), pRe("[?&]p=([0-9]+)");
    std::smatch m;
    std::string bvid = std::regex_search(url, m, bvRe) ? m[1].str() : "";
    std::string aid = bvid.empty() && std::regex_search(url, m, avRe) ? m[1].str() : "";
    std::string ep = std::regex_search(url, m, epRe) ? m[1].str() : "";
    size_t part = std::regex_search(url, m, pRe) ? static_cast<size_t>(std::max(1, atoi(m[1].str().c_str()))) : 1;
    bool ok = false;
    if (!ep.empty()) {   // a series episode (bangumi)
        if (biliGet("https://api.bilibili.com/pgc/view/web/season?ep_id=" + ep, jar, ua, ref, body) == 200) {
            Json sj = Json::parse(body);
            const Json& rs = sj["result"];
            std::string season = rs["season_title"].str(rs["title"].str());
            for (auto& e : rs["episodes"].list()) {
                if (std::to_string(e["ep_id"].i64(e["id"].i64())) != ep) continue;
                std::string t = trim(e["show_title"].str(trim(e["title"].str() + " " + e["long_title"].str())));
                p.title = season.empty() ? t : season + (t.empty() ? "" : " - " + t);
                p.thumbnail = e["cover"].str();
                p.duration = static_cast<int>(e["duration"].i64() / 1000);
                if (bvid.empty()) bvid = e["bvid"].str();
            }
            if (p.title.empty()) p.title = season;
            if (p.thumbnail.empty()) p.thumbnail = rs["cover"].str();
        }
        body.clear();
        if (biliGet("https://api.bilibili.com/pgc/player/web/playurl?" + biliQuery({{"ep_id", ep}, {"qn", "127"}, {"fnval", "4048"}, {"fnver", "0"}, {"fourk", "1"}}), jar, ua, ref, body) == 200) {
            Json r = Json::parse(body);
            const Json& rs = r["result"];
            ok = biliStreams(rs["video_info"].isObject() ? rs["video_info"] : rs, p);
            if (!ok && (r["code"].i32() == -10403 || rs["is_preview"].i32() == 1)) p.error = "This episode is for Bilibili's paying members only.";
        }
        p.id = "ep" + ep;
    } else {
        if (bvid.empty() && aid.empty()) return false;
        std::string cid;
        if (biliGet("https://api.bilibili.com/x/web-interface/view?" + (bvid.empty() ? "aid=" + aid : "bvid=" + bvid), jar, ua, ref, body, &info) == 200) {
            if (!info.setCookies.empty()) jar += (jar.empty() ? "" : "; ") + info.setCookies;
            Json v = Json::parse(body);
            int code = v["code"].i32(-1);
            if (code == 0) {
                const Json& d = v["data"];
                if (bvid.empty()) bvid = d["bvid"].str();
                p.title = d["title"].str();
                p.author = d["owner"]["name"].str();
                p.thumbnail = d["pic"].str();
                p.description = d["desc"].str();
                p.duration = d["duration"].i32();
                cid = std::to_string(d["cid"].i64());
                const auto& pages = d["pages"].list();
                if (part > 1 && part <= pages.size()) {
                    cid = std::to_string(pages[part - 1]["cid"].i64());
                    p.duration = pages[part - 1]["duration"].i32(p.duration);
                    std::string pt = trim(pages[part - 1]["part"].str());
                    if (!pt.empty()) p.title += " - " + pt;
                }
            } else if (code == -404 || code == 62002 || code == 62004) p.error = "This Bilibili video was removed, or it's private.";
            else if (code == -403) p.error = "This Bilibili video needs you to be signed in.";
        }
        p.id = bvid.empty() ? "av" + aid : bvid;
        if (!cid.empty() && cid != "0") {
            std::map<std::string, std::string> q = {{"cid", cid}, {"qn", "127"}, {"fnval", "4048"}, {"fnver", "0"}, {"fourk", "1"}};
            if (!bvid.empty()) q["bvid"] = bvid; else q["avid"] = aid;
            std::vector<std::string> tries;
            std::string mixin = biliMixinKey(jar, ua);
            if (!mixin.empty()) tries.push_back("https://api.bilibili.com/x/player/wbi/playurl?" + biliSigned(q, mixin));
            tries.push_back("https://api.bilibili.com/x/player/playurl?" + biliQuery(q));
            for (auto& t : tries) {
                body.clear();
                if (biliGet(t, jar, ua, ref, body) != 200) continue;
                Json r = Json::parse(body);
                if (r["code"].i32(-1) != 0) continue;
                if (biliStreams(r["data"], p)) { ok = true; break; }
            }
        }
        if (!ok) {   // the page's own player data
            std::string page = "https://www.bilibili.com/video/" + (bvid.empty() ? "av" + aid : bvid) + "/" + (part > 1 ? "?p=" + std::to_string(part) : "");
            html.clear();
            if (biliGet(page, jar, ua, ref, html) == 200) {
                Json pi = jsonAt(html, "window.__playinfo__");
                if (pi.isObject()) ok = biliStreams(pi["data"], p);
                if (p.title.empty()) p.title = meta(html, "og:title");
                if (p.thumbnail.empty()) p.thumbnail = meta(html, "og:image");
            }
        }
    }
    if (startsWith(p.thumbnail, "//")) p.thumbnail = "https:" + p.thumbnail;
    p.headers["Referer"] = ref;   // Bilibili's file servers answer only with it
    if (!ok && p.error.empty()) p.error = "Bilibili didn't give DDM this video (it may need you to be signed in, or be for members only).";
    return ok;
}

// ------------------------------------------------------------------ Facebook / Instagram / Threads
void dashFromString(const std::string& xml, const std::string& base, Page& p) {
    streams::Manifest m;
    std::string err;
    if (xml.empty() || !streams::parseDash(xml, base, m, err)) return;
    for (auto& r : m.reps) {
        if (r.singleUrl.empty()) continue;
        Source s;
        s.url = r.singleUrl; s.kind = r.video ? "video" : "audio";
        s.height = r.width > 0 && r.height > 0 ? std::min(r.width, r.height) : r.height;
        s.kbps = r.bandwidth / 1000.0; s.ext = r.video ? "mp4" : "m4a";
        bool dup = false;
        for (auto& x : p.sources) if (x.url == s.url) dup = true;
        if (!dup) p.sources.push_back(s);
    }
    if (m.duration > 0 && !p.duration) p.duration = static_cast<int>(m.duration);
}

bool meta_(const std::string& html, Page& p) {   // title, picture, description from the page's tags
    if (p.title.empty()) p.title = pageTitle(html);
    if (p.thumbnail.empty()) p.thumbnail = meta(html, "og:image");
    if (p.description.empty()) p.description = meta(html, "og:description");
    return true;
}

bool facebookLike(const std::string& url, const std::string& cookies, const std::string& ua, Page& p) {
    std::string html;
    std::map<std::string, std::string> extra;
    extra["Sec-Fetch-Mode"] = "navigate";
    if (getPage(url, cookies, ua, "", html, nullptr, extra) != 200) return false;
    std::string text = html;
    // the plain files (picture and sound together)
    for (const char* k : {"browser_native_hd_url", "playable_url_quality_hd", "hd_src_no_ratelimit", "hd_src"}) { std::string u = jsonString(text, k); if (!u.empty()) addSource(p, u, "file", 720, -1, "HD"); }
    for (const char* k : {"browser_native_sd_url", "playable_url", "sd_src_no_ratelimit", "sd_src"}) { std::string u = jsonString(text, k); if (!u.empty()) addSource(p, u, "file", 360, -1, "SD"); }
    // Instagram / Threads: every size of the reel
    for (size_t at = 0; (at = text.find("\"video_versions\"", at)) != std::string::npos; at += 16) {
        Json arr = jsonAt(text, "\"video_versions\"", at);
        for (auto& v : arr.list()) addSource(p, v["url"].str(), "file", std::min(v["width"].i32(), v["height"].i32()));
        if (p.sources.size() > 12) break;
    }
    // the separate picture / sound files of every quality (up to 1080p and more)
    for (const char* k : {"dash_manifest_xml_string", "video_dash_manifest", "dash_manifest", "manifest_xml"}) {
        std::string xml = jsonString(text, k);
        if (!xml.empty()) { dashFromString(xml, url, p); break; }
    }
    {   // the video's number: every download gets its own name, even when the page has no title of its own
        static const std::regex fbId("(?:/reels?/|/videos/(?:[^/?#]+/)?|/watch/?\\?v=|[?&]v=|/(?:p|tv)/)([A-Za-z0-9_-]{5,})");
        std::smatch m;
        if (std::regex_search(url, m, fbId)) p.id = m[1].str();
        if (p.id.empty()) { std::string v = jsonString(text, "video_id"); if (v.empty()) v = jsonString(text, "videoId"); if (v.size() < 40) p.id = v; }
    }
    // the caption the creator wrote: Facebook "message":{"text"}, Instagram / Threads "caption":{"text"}
    for (const char* k : {"\"message\"", "\"caption\"", "\"edge_media_to_caption\""}) {
        if (!p.title.empty()) break;
        size_t at = text.find(std::string(k) + ":{");
        if (at == std::string::npos) continue;
        Json o = jsonAt(text, k, at);
        std::string t = o["text"].str();
        if (t.empty()) for (auto& e : o["edges"].list()) { t = e["node"]["text"].str(); if (!t.empty()) break; }
        if (!t.empty()) p.title = t.substr(0, t.find('\n'));
    }
    if (p.title.empty()) p.title = jsonString(text, "accessibility_caption");
    if (p.thumbnail.empty()) {   // the video's own cover ("preferred_thumbnail":{"image":{"uri"}})
        size_t at = text.find("\"preferred_thumbnail\":{");
        if (at != std::string::npos) p.thumbnail = jsonAt(text, "\"preferred_thumbnail\"", at)["image"]["uri"].str();
    }
    if (p.author.empty()) { std::string a = jsonString(text, "owner_name"); if (a.empty()) a = jsonString(text, "username"); p.author = a; }
    meta_(html, p);
    if (p.thumbnail.empty()) p.thumbnail = jsonString(text, "preferred_thumbnail") ;
    if (on(siteOf(url), "instagram.com") || on(siteOf(url), "threads.net")) p.headers["Referer"] = "https://www.instagram.com/";
    if (!p.sources.empty()) return true;
    if (cookies.empty()) p.error = "This video needs you to be signed in. Sign in to the site in your browser and use the Download this video button there.";
    return false;
}

// ------------------------------------------------------------------ X (Twitter)
// ((Number(id) / 1e15) * Math.PI).toString(36) without "0" and "." — as the site's own embed computes it
std::string jsToString36(double value) {
    static const char* chars = "0123456789abcdefghijklmnopqrstuvwxyz";
    const int radix = 36;
    std::string frac;
    double integer = std::floor(value), fraction = value - integer;
    double delta = 0.5 * (std::nextafter(value, INFINITY) - value);
    delta = std::max(std::nextafter(0.0, 1.0), delta);
    if (fraction >= delta) {
        frac = ".";
        do {
            fraction *= radix; delta *= radix;
            int digit = static_cast<int>(fraction);
            frac += chars[digit];
            fraction -= digit;
            if (fraction > 0.5 || (fraction == 0.5 && (digit & 1))) {
                if (fraction + delta > 1) {
                    for (;;) {   // carry into the digits already written
                        frac.pop_back();
                        if (frac.empty()) { integer += 1; break; }
                        char c = frac.back();
                        if (c == '.') { frac.pop_back(); integer += 1; break; }
                        int d = c > '9' ? c - 'a' + 10 : c - '0';
                        frac.pop_back();
                        if (d + 1 < radix) { frac += chars[d + 1]; break; }
                    }
                    break;
                }
            }
        } while (fraction >= delta);
    }
    std::string ip;
    do { double rem = std::fmod(integer, radix); ip = chars[static_cast<int>(rem)] + ip; integer = (integer - rem) / radix; } while (integer > 0);
    return ip + frac;
}

bool twitter(const std::string& url, Page& p) {
    static const std::regex re("/status(?:es)?/([0-9]+)");
    std::smatch m;
    if (!std::regex_search(url, m, re)) return false;
    std::string id = m[1];
    std::string tok;
    for (char c : jsToString36(static_cast<double>(_atoi64(id.c_str())) / 1e15 * 3.141592653589793)) if (c != '0' && c != '.') tok += c;
    std::string body;
    RequestInfo r;
    r.url = "https://cdn.syndication.twimg.com/tweet-result?id=" + id + "&lang=en&token=" + tok;
    r.userAgent = "Googlebot";
    if (http::get(r, body, nullptr, 8 * 1024 * 1024, 30) != 200) return false;
    Json j = Json::parse(body);
    if (!j.isObject()) return false;
    p.id = id;
    p.title = j["text"].str();
    p.author = j["user"]["screen_name"].str();
    for (const Json* src : {&j, &j["quoted_tweet"]}) {
        for (auto& md : (*src)["mediaDetails"].list()) {
            if (p.thumbnail.empty()) p.thumbnail = md["media_url_https"].str();
            const Json& vi = md["video_info"];
            if (p.duration == 0) p.duration = vi["duration_millis"].i32() / 1000;
            for (auto& v : vi["variants"].list()) {
                std::string u = v["url"].str();
                std::string ct = v["content_type"].str();
                if (ct == "application/x-mpegURL") addSource(p, u, "hls");
                else if (ct == "video/mp4") {
                    static const std::regex wh("/([0-9]+)x([0-9]+)/");
                    std::smatch s;
                    int h = std::regex_search(u, s, wh) ? std::min(atoi(s[1].str().c_str()), atoi(s[2].str().c_str())) : 0;
                    addSource(p, u, "file", h);
                    p.sources.back().kbps = v["bitrate"].num() / 1000.0;
                }
            }
        }
    }
    return !p.sources.empty();
}

// ------------------------------------------------------------------ Vimeo, Dailymotion, Reddit
// a subtitle file the page offers (WebVTT or SubRip; other kinds aren't offered)
void addSub(Page& p, const std::string& url, const std::string& lang, const std::string& name, const std::string& format = "") {
    std::string l = lower(url), path = l.substr(0, l.find('?'));
    if (!(startsWith(l, "http://") || startsWith(l, "https://")) || p.subs.size() >= 200) return;
    if (endsWith(path, ".ttml") || endsWith(path, ".xml") || endsWith(path, ".json") || endsWith(path, ".ass")) return;
    for (auto& x : p.subs) if (x.url == url) return;
    SubTrack t;
    t.url = url; t.lang = trim(lang); t.name = trim(name);
    if (t.name.find('&') != std::string::npos && t.name.find(';') != std::string::npos) t.name.clear();   // (a name DDM can't spell: the language's own name is shown)
    t.format = !format.empty() ? format : endsWith(path, ".m3u8") ? "hls" : endsWith(path, ".srt") ? "srt" : "vtt";
    if (t.lang.empty()) t.lang = t.name.empty() ? "und" : t.name;
    p.subs.push_back(t);
}

bool vimeo(const std::string& url, const std::string& referer, Page& p) {
    static const std::regex re("vimeo\\.com/(?:video/|channels/[^/]+/|groups/[^/]+/videos/|showcase/[0-9]+/video/)?([0-9]{5,})");
    std::smatch m;
    if (!std::regex_search(url, m, re)) return false;
    std::string body;
    if (getPage("https://player.vimeo.com/video/" + m[1].str() + "/config", "", "", referer.empty() ? "https://vimeo.com/" : referer, body) != 200) return false;
    Json j = Json::parse(body);
    p.id = m[1];
    p.title = j["video"]["title"].str();
    p.author = j["video"]["owner"]["name"].str();
    p.duration = j["video"]["duration"].i32();
    p.thumbnail = j["video"]["thumbs"]["base"].str();
    for (auto& f : j["request"]["files"]["progressive"].list()) addSource(p, f["url"].str(), "file", f["height"].i32());
    const Json& hls = j["request"]["files"]["hls"];
    std::string cdn = hls["default_cdn"].str();
    std::string hu = hls["cdns"][cdn]["url"].str();
    if (hu.empty()) for (auto& kv : hls["cdns"].items()) { hu = kv.second["url"].str(); if (!hu.empty()) break; }
    addSource(p, hu, "hls");
    for (auto& t : j["request"]["text_tracks"].list()) addSub(p, streams::resolveUrl("https://player.vimeo.com/", t["url"].str()), t["lang"].str(), t["label"].str(), "vtt");
    p.headers["Referer"] = "https://player.vimeo.com/";
    return !p.sources.empty();
}

// tests only (DDM_TEST_UI with DDM_TEST_DAILYMOTION=http://127.0.0.1:port): a stand-in Dailymotion
const std::string& dmBase() {
    static std::string base = [] {
        char v[256] = "";
        if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_DAILYMOTION", v, sizeof v)) return std::string(v);
        return std::string();
    }();
    return base;
}

bool dailymotion(const std::string& url, const std::string& cookies, Page& p) {
    // dailymotion.com/video/x8abc, /embed/video/x8abc, dai.ly/x8abc, and the player in other sites' pages
    // (geo.dailymotion.com/player/xyz.html?video=x8abc)
    static const std::regex re("(?:dailymotion\\.[a-z]{2,3}/(?:(?:crawler/|embed/|swf/)?video/|player(?:/[0-9a-z]+)?\\.html\\?(?:[^#]*&)?video=)|dai\\.ly/)([a-zA-Z0-9]+)", std::regex::icase);
    std::smatch m;
    if (url.size() > 2048 || !std::regex_search(url, m, re)) return false;
    std::string body;
    std::string api = (dmBase().empty() ? std::string("https://www.dailymotion.com") : dmBase()) + "/player/metadata/video/" + m[1].str() + "?app=com.dailymotion.neon";
    if (getPage(api, cookies, "", "https://www.dailymotion.com/", body) != 200) return false;
    Json j = Json::parse(body);
    if (j["error"].isObject()) {   // private, removed, or not shown in this country
        std::string why = j["error"]["title"].str(j["error"]["raw_message"].str());
        p.error = j["error"]["code"].str() == "DM007" ? "Dailymotion doesn't show this video in your country." : why.empty() ? "Dailymotion doesn't give this video." : "Dailymotion: " + why.substr(0, 200);
        return false;
    }
    p.id = m[1];
    p.title = j["title"].str();
    p.author = j["owner"]["screenname"].str();
    p.duration = j["duration"].i32();
    p.thumbnail = j["posters"]["720"].str(j["thumbnails"]["720"].str());
    if (p.thumbnail.empty()) for (auto& kv : j["posters"].items()) p.thumbnail = kv.second.str();
    for (auto& kv : j["qualities"].items())
        for (auto& q : kv.second.list()) {
            std::string t = q["type"].str(), u = q["url"].str();
            size_t hash = u.find('#');
            if (hash != std::string::npos) u = u.substr(0, hash);
            if (u.empty()) continue;
            if (t == "application/x-mpegURL") addSource(p, u, "hls");
            else if (t == "video/mp4") {
                int h = atoi(kv.first.c_str());
                static const std::regex hre("/H264-[0-9]+x([0-9]+)");
                std::smatch hm;
                if (std::regex_search(u, hm, hre)) h = atoi(hm[1].str().c_str());
                addSource(p, u, "file", h);
            }
        }
    for (auto& kv : j["subtitles"]["data"].items())
        for (auto& u : kv.second["urls"].list()) { addSub(p, u.str(), kv.first, kv.second["label"].str(), "srt"); break; }
    p.headers["Referer"] = "https://www.dailymotion.com/";
    p.headers["Origin"] = "https://www.dailymotion.com";
    if (p.sources.empty() && p.error.empty()) p.error = "Dailymotion gave no stream for this video.";
    return !p.sources.empty();
}

bool reddit(const std::string& url, const std::string& cookies, Page& p) {
    std::string u = url;
    size_t q = u.find_first_of("?#");
    if (q != std::string::npos) u = u.substr(0, q);
    if (!u.empty() && u.back() == '/') u.pop_back();
    std::string body;
    if (getPage(u + ".json", cookies, "", "", body) != 200) return false;
    Json j = Json::parse(body);
    const Json& d = j.at(0)["data"]["children"].at(0)["data"];
    const Json& rv = d["secure_media"]["reddit_video"].isObject() ? d["secure_media"]["reddit_video"] : d["media"]["reddit_video"];
    if (!rv.isObject()) return false;
    p.title = d["title"].str();
    p.author = d["author"].str();
    p.duration = rv["duration"].i32();
    p.thumbnail = htmlDecode(d["thumbnail"].str());
    addSource(p, htmlDecode(rv["hls_url"].str()), "hls");
    addSource(p, htmlDecode(rv["dash_url"].str()), "dash");
    return !p.sources.empty();
}

// ------------------------------------------------------------------ any other page
void scanPage(const std::string& html0, const std::string& pageUrl, Page& p, int depth);
bool readAt(const std::string& url, const std::string& cookies, const std::string& userAgent, const std::string& referer, Page& out, int depth);

bool hoverPreview(const std::string& u) {
    std::string l = lower(u);
    return l.find("thumb") != std::string::npos || l.find("preview") != std::string::npos || l.find("sprite") != std::string::npos || l.find("/trailer") != std::string::npos;
}
void addAny(Page& p, const std::string& u, int h = 0, const std::string& label = "") {
    if (streams::looksHls(u)) addSource(p, u, "hls", h, -1, label);
    else if (streams::looksDash(u)) addSource(p, u, "dash", h, -1, label);
    else if (isFileUrl(u)) addSource(p, u, "file", h, -1, label);
}
bool urlStop(char c) { return c == '"' || c == '\'' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '<' || c == '>' || c == '\\' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|' || c == '`' || c == ',' || c == ';'; }
// a JSON string value after "key": (escapes kept for unescapeJs), "" = none; at most 8 KB
std::string valueAfter(const std::string& text, size_t i) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) i++;
    if (i >= text.size() || text[i] != ':') return "";
    i++;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) i++;
    if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) return "";
    char q = text[i++];
    std::string v;
    for (; i < text.size() && v.size() < 8192; i++) {
        if (text[i] == '\\' && i + 1 < text.size()) { v += text[i]; v += text[++i]; continue; }
        if (text[i] == q) return v;
        v += text[i];
    }
    return "";
}

// ------------------------------------------------------------------ data in the page (Next.js, JSON-LD, app state)
// Every <script type="application/json"> / "application/ld+json" block (and __NEXT_DATA__) is walked: video and stream
// addresses are taken by what they are (.m3u8, .mpd, .mp4…) or by what the page calls them (videoUrl, mediaUrl of a
// video, contentUrl of a VideoObject, playbackUrl…). This is how Snapchat, many news sites and most new players
// describe their videos.
bool imageUrl(const std::string& u) {
    std::string e = extOfUrl(u);
    return e == "jpg" || e == "jpeg" || e == "png" || e == "webp" || e == "gif" || e == "avif" || e == "svg" || e == "ico";
}
bool videoHint(const Json& o) {   // an object that describes a video
    if (!o.isObject()) return false;
    std::string t = lower(o["@type"].str() + " " + o["type"].str() + " " + o["mediaType"].str() + " " + o["media_type"].str() + " " + o["kind"].str());
    if (t.find("video") != std::string::npos) return true;
    if (o["snapMediaType"].i32(-1) == 1 || o["isVideo"].boolean() || o["is_video"].boolean()) return true;
    return o["duration"].isNumber() || o["videoMetadata"].isObject() || o["video_duration"].isNumber();
}
void walkJson(const Json& j, const std::string& key, const Json* parent, const std::string& pageUrl, Page& p, int depth, const Json* grand = nullptr) {
    if (depth > 40 || p.sources.size() > 80) return;
    if (j.isObject()) {
        for (auto& kv : j.items()) walkJson(kv.second, kv.first, &j, pageUrl, p, depth + 1, parent);
        if (p.title.empty() && lower(j["@type"].str()) == "videoobject") p.title = j["name"].str();
        if (p.thumbnail.empty() && lower(j["@type"].str()) == "videoobject") p.thumbnail = j["thumbnailUrl"].isString() ? j["thumbnailUrl"].str() : j["thumbnailUrl"].at(0).str();
        return;
    }
    if (j.isArray()) { for (auto& x : j.list()) walkJson(x, key, parent, pageUrl, p, depth + 1, grand); return; }
    if (!j.isString()) return;
    std::string v = j.str();
    if (v.size() < 8 || v.size() > 4096) return;
    std::string lv = lower(v);
    if (!(startsWith(lv, "http://") || startsWith(lv, "https://") || startsWith(lv, "//") || (startsWith(lv, "/") && (streams::looksHls(v) || streams::looksDash(v) || isFileUrl(v))))) return;
    std::string u = streams::resolveUrl(pageUrl, v);
    if (imageUrl(u) || hoverPreview(u)) return;
    int h = 0;
    if (parent && parent->isObject()) {
        const Json& o = *parent;
        h = o["height"].i32();
        if (!h) for (const char* k : {"quality", "label", "resolution", "res"}) { h = heightHint(o[k].isString() ? o[k].str() : std::to_string(o[k].i32())); if (h) break; }
        if (o["width"].i32() > 0 && h > 0) h = std::min(h, o["width"].i32());
    }
    if (streams::looksHls(u) || streams::looksDash(u) || isFileUrl(u)) { addAny(p, u, h); return; }
    // an address without a telling ending: only under a name that says "video"
    std::string k = lower(key);
    static const char* names[] = {"mediaurl", "videourl", "video_url", "contenturl", "playbackurl", "playback_url", "streamurl", "stream_url", "hlsurl", "hls_url", "dashurl",
                                  "dash_url", "mp4url", "mp4_url", "downloadurl", "download_url", "playurl", "play_url", "videosrc", "video_src", "playaddr", "src", "url", "file"};
    bool named = false;
    for (auto n : names) if (k == n) named = true;
    if (!named) return;
    for (const Json* o : {parent, grand})   // a picture, said so by the page (Snapchat: snapMediaType 0)
        if (o && o->isObject() && ((*o)["snapMediaType"].i32(-1) == 0 || lower((*o)["mediaType"].str()) == "image" || lower((*o)["type"].str()) == "image")) return;
    bool videoish = k.find("video") != std::string::npos || k.find("playback") != std::string::npos || k.find("stream") != std::string::npos || k.find("hls") != std::string::npos ||
                    k.find("mp4") != std::string::npos || k.find("play") != std::string::npos || (parent && videoHint(*parent)) || (grand && videoHint(*grand));
    if (!videoish) return;
    if (k.find("hls") != std::string::npos) addSource(p, u, "hls", h);
    else if (k.find("dash") != std::string::npos) addSource(p, u, "dash", h);
    else addSource(p, u, "file", h);
}
void scriptData(const std::string& html, const std::string& low, const std::string& pageUrl, Page& p) {
    int n = 0;
    for (size_t at = 0; n < 40 && (at = low.find("<script", at)) != std::string::npos; at += 7) {
        size_t gt = low.find('>', at);
        if (gt == std::string::npos || gt - at > 2048) continue;
        std::string tag = low.substr(at, gt - at);
        bool data = tag.find("application/json") != std::string::npos || tag.find("application/ld+json") != std::string::npos || tag.find("__next_data__") != std::string::npos;
        if (!data) continue;
        size_t end = low.find("</script", gt);
        if (end == std::string::npos) break;
        n++;
        Json j = Json::parse(html.substr(gt + 1, end - gt - 1));
        if (!j.isNull()) walkJson(j, "", nullptr, pageUrl, p, 0);
        at = end;
    }
}

// ------------------------------------------------------------------ rules that update by themselves (siterules)
// {"sites": ["example.com"], "fetch": "https://example.com/api/video/{id}", "id": "/video/([0-9]+)", "headers": {…},
//  "userAgent": "…", "referer": "…", "keys": ["hd_src", "videoUrl"], "json": true,
//  "patterns": [{"after": "\"hd\":\"", "regex": "^([^\"]+)", "kind": "file", "height": 720}], "title": "og:title"}
bool ruleSites(const Json& rule, const std::string& site) {
    for (auto& s : rule["sites"].list()) { std::string d = lower(s.str()); if (!d.empty() && (site == d || endsWith(site, "." + d))) return true; }
    return false;
}
void applyRuleText(const Json& rule, const std::string& text0, const std::string& pageUrl, Page& p) {
    std::string text = replaceAll(replaceAll(text0, "\\/", "/"), "\\u002F", "/");
    text = replaceAll(text, "\\u0026", "&");
    for (auto& k : rule["keys"].list()) {   // "key": "address" anywhere in the page or the answer
        std::string key = "\"" + k.str() + "\"";
        if (key.size() < 3) continue;
        int found = 0;
        for (size_t at = 0; found < 40 && (at = text.find(key, at)) != std::string::npos; at += key.size()) {
            std::string v = valueAfter(text, at + key.size());
            if (v.size() < 5) continue;
            found++;
            std::string u = streams::resolveUrl(pageUrl, unescapeJs(v));
            if (streams::looksHls(u)) addSource(p, u, "hls"); else if (streams::looksDash(u)) addSource(p, u, "dash"); else if (!imageUrl(u)) addSource(p, u, "file");
        }
    }
    for (auto& pt : rule["patterns"].list()) {   // a regular expression, run only right after a fixed piece of text (fast, safe)
        std::string after = pt["after"].str();
        std::regex re;
        try { re = std::regex(pt["regex"].str().empty() ? "^([^\"'<>\\s]+)" : pt["regex"].str()); } catch (...) { continue; }
        std::string kind = pt["kind"].str();
        int h0 = pt["height"].i32();
        int found = 0;
        for (size_t at = 0; found < 40 && (at = after.empty() ? std::string::npos : text.find(after, at)) != std::string::npos; at += after.size()) {
            std::string win = text.substr(at + after.size(), 4096);
            std::smatch m;
            bool ok = false;
            try { ok = std::regex_search(win, m, re); } catch (...) { ok = false; }
            if (!ok || m.size() < 2) continue;
            found++;
            std::string u = streams::resolveUrl(pageUrl, unescapeJs(htmlDecode(m[1].str())));
            int h = h0 ? h0 : (m.size() > 2 ? atoi(m[2].str().c_str()) : 0);
            std::string k = !kind.empty() ? kind : streams::looksHls(u) ? "hls" : streams::looksDash(u) ? "dash" : "file";
            if (k == "hls" || k == "dash" || k == "file" || k == "video" || k == "audio") addSource(p, u, k, h, -1, pt["label"].str());
        }
    }
    if (rule["json"].boolean()) { Json j = Json::parse(text0); if (!j.isNull()) walkJson(j, "", nullptr, pageUrl, p, 0); }
}
bool applyRules(const std::string& url, const std::string& cookies, const std::string& userAgent, const std::string& referer, Page& p) {
    Json all = siterules::rules();
    std::string site = lower(siteOf(url));
    for (auto& rule : all["rules"].list()) {
        if (!ruleSites(rule, site)) continue;
        std::string fetchUrl = rule["fetch"].str().empty() ? url : rule["fetch"].str();
        if (fetchUrl.find("{id}") != std::string::npos) {
            std::smatch m;
            bool ok = false;
            try { std::regex re(rule["id"].str()); ok = std::regex_search(url, m, re) && m.size() > 1; } catch (...) { ok = false; }
            if (!ok) continue;
            fetchUrl = replaceAll(fetchUrl, "{id}", m[1].str());
        }
        fetchUrl = replaceAll(fetchUrl, "{url}", url);
        if (!(startsWith(lower(fetchUrl), "https://") || startsWith(lower(fetchUrl), "http://"))) continue;
        std::map<std::string, std::string> extra;
        for (auto& kv : rule["headers"].items()) if (kv.second.isString()) extra[kv.first] = kv.second.str();
        std::string ua = rule["userAgent"].str().empty() ? userAgent : rule["userAgent"].str();
        std::string body;
        if (getPage(fetchUrl, cookies, ua, rule["referer"].str().empty() ? referer : rule["referer"].str(), body, nullptr, extra) != 200) continue;
        applyRuleText(rule, body, fetchUrl, p);
        if (p.title.empty()) p.title = pageTitle(body);
        if (p.thumbnail.empty()) p.thumbnail = meta(body, "og:image");
        for (auto& kv : rule["fileHeaders"].items()) if (kv.second.isString()) p.headers[kv.first] = kv.second.str();
        if (!p.sources.empty()) { logLine("video: site rules " + std::to_string(siterules::version()) + " for " + site); return true; }
    }
    return false;
}

void genericSources(const std::string& html, const std::string& pageUrl, Page& p) {
    std::string text = replaceAll(replaceAll(html, "\\/", "/"), "\\u002F", "/");
    text = replaceAll(text, "\\u0026", "&");
    std::string low = lower(text);
    scriptData(html, lower(html), pageUrl, p);   // the page's own data first: it names qualities best
    // og:video, twitter:player:stream, schema.org contentUrl
    for (const char* k : {"og:video:secure_url", "og:video:url", "og:video", "twitter:player:stream"}) {
        std::string u = meta(text, k);
        if (u.empty()) continue;
        u = streams::resolveUrl(pageUrl, u);
        std::string e = extOfUrl(u);
        if (streams::looksHls(u)) addSource(p, u, "hls"); else if (streams::looksDash(u)) addSource(p, u, "dash"); else if (e == "mp4" || e == "webm" || e == "mov" || e == "m4v") addSource(p, u, "file");
    }
    for (const char* k : {"contentUrl", "embedUrl"}) {
        std::string u = jsonString(text, k);
        if (u.empty()) continue;
        addAny(p, streams::resolveUrl(pageUrl, u));
    }
    // players' setups: html5player.setVideoUrlHigh('…'), setVideoUrlLow('…'), setVideoHLS('…')
    for (const char* call : {"setvideourlhigh(", "setvideourllow(", "setvideohls("}) {
        for (size_t at = 0; (at = low.find(call, at)) != std::string::npos; at++) {
            size_t i = at + strlen(call);
            while (i < text.size() && text[i] == ' ') i++;
            if (i >= text.size() || (text[i] != '\'' && text[i] != '"')) continue;
            size_t e = text.find(text[i], i + 1);
            if (e == std::string::npos || e - i > 4096) continue;
            std::string u = streams::resolveUrl(pageUrl, unescapeJs(text.substr(i + 1, e - i - 1)));
            std::string which = std::string(call) == "setvideourlhigh(" ? "High" : std::string(call) == "setvideourllow(" ? "Low" : "";
            if (streams::looksHls(u) || which.empty()) addSource(p, u, "hls"); else addSource(p, u, "file", 0, -1, which);
        }
    }
    // <track kind="subtitles" src srclang label>: the page's own subtitles
    for (auto& tag : tagsOf(text, low, "track", 200)) {
        std::string kind = lower(attrOf(tag, "kind")), u = attrOf(tag, "src");
        if (u.empty() || !(kind.empty() || kind == "subtitles" || kind == "captions")) continue;
        addSub(p, streams::resolveUrl(pageUrl, u), attrOf(tag, "srclang"), attrOf(tag, "label"));
    }
    // <video src>, <source src>
    for (const char* t : {"video", "source"})
        for (auto& tag : tagsOf(text, low, t)) {
            std::string u = attrOf(tag, "src");
            if (u.empty()) u = attrOf(tag, "data-src");
            if (!u.empty()) addAny(p, streams::resolveUrl(pageUrl, u), heightHint(attrOf(tag, "label") + " " + attrOf(tag, "res") + " " + attrOf(tag, "size")));
        }
    // quality lists in the page's data: {"quality":"720","videoUrl":"…"} / {"label":"1080p","file":"…"} / {"res":720,"src":"…"}
    static const std::regex qual("[\"']?(?:quality|label|res|height|resolution|format_id|size)[\"']?\\s*:\\s*[\"']?([0-9]{3,4})");
    for (const char* key : {"\"videourl\"", "\"file\"", "\"src\"", "\"url\"", "\"video_url\"", "\"stream_url\"", "\"videosrc\"", "\"hls\"", "\"mp4\"", "\"playurl\""}) {
        size_t klen = strlen(key);
        for (size_t at = 0; p.sources.size() < 80 && (at = low.find(key, at)) != std::string::npos; at += klen) {
            std::string v = valueAfter(text, at + klen);
            if (v.size() < 5) continue;
            std::string u = streams::resolveUrl(pageUrl, unescapeJs(v));
            if (!(startsWith(lower(u), "http://") || startsWith(lower(u), "https://"))) continue;
            size_t ob = text.rfind('{', at), cb = text.find('}', at);
            int h = 0;
            if (ob != std::string::npos && at - ob < 800 && cb != std::string::npos && cb - at < 800) {
                std::string obj = text.substr(ob, cb - ob + 1);
                std::smatch qm;
                if (std::regex_search(obj, qm, qual)) h = atoi(qm[1].str().c_str());
            }
            if (u.find("get_media") != std::string::npos || u.find("/media/mp4") != std::string::npos) { addSource(p, u, "list", h); continue; }   // a list of files (some players)
            addAny(p, u, h);
        }
    }
    // any address of a video or stream written in the page (quoted, in a player's setup, relative or full)
    int found = 0;
    for (const char* ext : {".m3u8", ".mpd", ".mp4", ".webm", ".m4v", ".mov"}) {
        size_t elen = strlen(ext);
        for (size_t at = 0; found < 200 && p.sources.size() < 80 && (at = low.find(ext, at)) != std::string::npos; at += elen) {
            size_t end = at + elen;
            if (end < text.size() && !urlStop(text[end]) && text[end] != '?' && text[end] != '#' && text[end] != '&') continue;   // ".movie", ".mp4x"
            if (end < text.size() && (text[end] == '?' || text[end] == '#')) while (end < text.size() && end - at < 4096 && !urlStop(text[end])) end++;
            size_t start = at;
            while (start > 0 && at - start < 2048 && !urlStop(text[start - 1]) && text[start - 1] != '=') start--;
            std::string raw = text.substr(start, end - start);
            if (raw.size() < 5 + elen - 1 || raw[0] == '.' ) continue;
            found++;
            std::string u = streams::resolveUrl(pageUrl, htmlDecode(raw));
            if (hoverPreview(u)) continue;   // hover previews
            if (streams::looksHls(u)) addSource(p, u, "hls"); else if (streams::looksDash(u)) addSource(p, u, "dash"); else addSource(p, u, "file");
        }
    }
}

void scanPage(const std::string& html, const std::string& pageUrl, Page& p, int depth) {
    genericSources(html, pageUrl, p);
    meta_(html, p);
    if (!p.sources.empty() || depth >= 2) return;
    // a player inside a frame (or a frame in a frame): read those pages too, the likeliest players first
    std::vector<std::string> frames;
    std::string lowHtml = lower(html);
    for (auto& tag : tagsOf(html, lowHtml, "iframe", 60)) {
        std::string src = attrOf(tag, "src");
        if (src.empty()) src = attrOf(tag, "data-src");
        if (src.empty()) continue;
        std::string u = streams::resolveUrl(pageUrl, src);
        std::string l = lower(u);
        if (!(startsWith(l, "http://") || startsWith(l, "https://"))) continue;
        bool junk = false;
        for (auto k : {"googletagmanager", "doubleclick", "googlesyndication", "facebook.com/plugins", "platform.twitter", "recaptcha", "disqus", "/ads", "adserver", "analytics", "chat", "comments", "about:blank"})
            if (l.find(k) != std::string::npos) junk = true;
        if (junk || std::find(frames.begin(), frames.end(), u) != frames.end()) continue;
        static const std::regex yt("^https?://(?:www\\.)?youtube(?:-nocookie)?\\.com/embed/([A-Za-z0-9_-]{11})", std::regex::icase);
        std::smatch ym;
        if (std::regex_search(u, ym, yt)) { addSource(p, "https://www.youtube.com/watch?v=" + ym[1].str(), "youtube"); return; }   // a YouTube video on the page: YouTube's own reader takes it
        bool likely = l.find("embed") != std::string::npos || l.find("player") != std::string::npos || l.find("video") != std::string::npos || l.find("/e/") != std::string::npos || l.find("/v/") != std::string::npos;
        if (likely) frames.insert(frames.begin(), u); else frames.push_back(u);
    }
    if (frames.size() > 4) frames.resize(4);
    for (auto& u : frames) {
        Page inner;
        if (readAt(u, "", "", pageUrl, inner, depth + 1) && !inner.sources.empty()) {
            for (auto& s : inner.sources) p.sources.push_back(s);
            for (auto& kv : inner.headers) p.headers[kv.first] = kv.second;
            if (!p.headers.count("Referer")) p.headers["Referer"] = u;
            if (p.title.empty()) p.title = inner.title;
            if (p.thumbnail.empty()) p.thumbnail = inner.thumbnail;
            if (!p.duration) p.duration = inner.duration;
            return;
        }
    }
}

// "get_media"-style lists: [{"quality":"1080","videoUrl":"…","format":"mp4"}, …]
void expandLists(Page& p, const std::string& cookies, const std::string& pageUrl) {
    std::vector<Source> out;
    for (auto& s : p.sources) {
        if (s.kind != "list") { out.push_back(s); continue; }
        std::string body;
        if (getPage(s.url, cookies, "", pageUrl, body) != 200) continue;
        Json j = Json::parse(body);
        for (auto& e : j.list()) {
            std::string u = e["videoUrl"].str(e["url"].str());
            if (u.empty()) continue;
            Source x;
            x.url = u;
            x.kind = streams::looksHls(u) ? "hls" : "file";
            x.height = e["quality"].isString() ? atoi(e["quality"].str().c_str()) : e["quality"].i32();
            x.ext = x.kind == "file" ? "mp4" : "";
            out.push_back(x);
        }
    }
    p.sources = out;
}

} // namespace

bool isFileUrl(const std::string& url) {
    static const std::set<std::string> exts = {"mp4", "webm", "mkv", "mov", "m4v", "avi", "flv", "wmv", "3gp", "ts", "mp3", "m4a", "aac", "ogg", "opus", "wav", "flac"};
    return exts.count(extOfUrl(url)) > 0;
}

bool read(const std::string& url, const std::string& cookies, const std::string& userAgent, const std::string& referer, Page& out) { return readAt(url, cookies, userAgent, referer, out, 0); }

bool fromBrowser(const std::string& url, const std::string& data, const std::string& cookies, Page& out) {
    out = Page();
    out.site = siteOf(url);
    if (!on(out.site, "tiktok.com") || data.empty() || data.size() > 4 * 1024 * 1024) return false;
    bool ok = false;
    Json item = Json::parse(data, &ok);
    if (!ok || !item.isObject()) return false;
    // only the post this address names (the data comes from the page)
    static const std::regex idRe("/(?:video|photo|v|embed/v2|embed|player/v1)/([0-9]{15,})");
    std::smatch m;
    std::string id = std::regex_search(url, m, idRe) ? m[1].str() : "";
    if (!id.empty() && item["id"].str() != id) return false;
    if (!tiktokItem(item, cookies, out)) { out.sources.clear(); return false; }
    if (out.thumbnail.empty() || out.title.empty()) {
        Page o;
        tiktokOembed(out.id, out.author, "", o);
        if (out.title.empty()) out.title = o.title;
        if (out.thumbnail.empty()) out.thumbnail = o.thumbnail;
    }
    out.title = trim(out.title);
    return true;
}

namespace {
bool readAt(const std::string& url, const std::string& cookies, const std::string& userAgent, const std::string& referer, Page& out, int depth) {
    out = Page();
    out.site = siteOf(url);
    std::string s = out.site;
    bool ok = false;
    if (depth == 0 && applyRules(url, cookies, userAgent, referer, out)) ok = true;   // DedSec's newest rules for this site (they update by themselves)
    else if (on(s, "tiktok.com")) ok = tiktok(url, cookies, userAgent, out);
    else if (on(s, "facebook.com") || on(s, "fb.watch") || on(s, "instagram.com") || on(s, "threads.net")) ok = facebookLike(url, cookies, userAgent, out);
    else if (on(s, "twitter.com") || on(s, "x.com")) ok = twitter(url, out);
    else if (on(s, "vimeo.com")) ok = vimeo(url, referer, out);
    else if (s.find("dailymotion.") != std::string::npos || on(s, "dai.ly")) ok = dailymotion(url, cookies, out);
    else if (on(s, "reddit.com") || on(s, "redd.it")) ok = reddit(url, cookies, out);
    else if (on(s, "bilibili.com") || on(s, "b23.tv") || on(s, "bilibili.tv")) ok = bilibili(url, cookies, userAgent, out);
    if (!ok) {   // every other site (and the ones above when their own way found nothing)
        std::string html;
        ResponseInfo info;
        long st = getPage(url, cookies, userAgent, referer, html, &info);
        if (st == 200) {
            if (startsWith(info.mime, "video/") || startsWith(info.mime, "audio/")) { addSource(out, url, "file"); return true; }
            if (html.find("#EXTM3U") == 0) { addSource(out, url, "hls"); return true; }
            scanPage(html, url, out, depth);
            if (!info.setCookies.empty() && !out.sources.empty()) out.headers["Cookie"] = cookies.empty() ? info.setCookies : cookies + "; " + info.setCookies;
        } else if (out.error.empty()) out.error = st == 0 ? "Couldn't reach the site. Check your internet connection." : st == 401 || st == 403 ? "The site refused (it may need you to be signed in). Play the video in your browser and use the Download this video button." : "The site answered " + std::to_string(st) + ".";
        expandLists(out, cookies, url);
    }
    if (!out.headers.count("Referer")) out.headers["Referer"] = url;
    out.title = trim(out.title);
    if (!out.thumbnail.empty()) {   // relative pictures ("/thumb.jpg"), web addresses only
        out.thumbnail = streams::resolveUrl(url, out.thumbnail);
        if (!(startsWith(lower(out.thumbnail), "http://") || startsWith(lower(out.thumbnail), "https://"))) out.thumbnail.clear();
    }
    if (out.sources.empty() && out.error.empty()) out.error = "No video was found on this page. Play it in your browser and use the Download this video button.";
    return !out.sources.empty();
}

} // namespace

} // namespace sites
} // namespace ddm
