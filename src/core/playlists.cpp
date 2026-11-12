#include "playlists.hpp"
#include "http.hpp"
#include "settings.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <regex>
#include <set>

namespace ddm {
namespace playlists {

namespace {
const char* kUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36";

std::string ytBase() {
    static std::string b = [] {
        char v[256] = {};
        if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_YOUTUBE", v, sizeof v)) return std::string(v);
        return std::string("https://www.youtube.com");
    }();
    return b;
}
bool isYt(const std::string& url) {
    std::string h = lower(urlHost(url));
    if (h == "youtube.com" || endsWith(h, ".youtube.com") || h == "youtu.be") return true;
    std::string b = lower(urlHost(ytBase()));
    return !b.empty() && h == b && ytBase() != "https://www.youtube.com";
}
std::string param(const std::string& url, const std::string& name) {
    size_t q = url.find('?');
    if (q == std::string::npos) return "";
    std::string qs = url.substr(q + 1);
    size_t hash = qs.find('#');
    if (hash != std::string::npos) qs = qs.substr(0, hash);
    for (auto& kv : split(qs, '&')) { size_t e = kv.find('='); if (kv.substr(0, e) == name && e != std::string::npos) return kv.substr(e + 1); }
    return "";
}
std::string pathOf(const std::string& url) {
    size_t s = url.find("://");
    size_t p = url.find('/', s == std::string::npos ? 0 : s + 3);
    if (p == std::string::npos) return "/";
    std::string r = url.substr(p);
    size_t q = r.find_first_of("?#");
    return q == std::string::npos ? r : r.substr(0, q);
}

// a channel's address without its tab: /@name, /channel/UC…, /c/name, /user/name
std::string channelBase(const std::string& url, std::string& tab) {
    std::string p = pathOf(url);
    auto seg = split(p, '/');
    std::vector<std::string> s;
    for (auto& x : seg) if (!x.empty()) s.push_back(x);
    if (s.empty()) return "";
    size_t n = 0;
    if (s[0].size() > 1 && s[0][0] == '@') n = 1;
    else if ((s[0] == "channel" || s[0] == "c" || s[0] == "user") && s.size() >= 2) n = 2;
    else return "";
    tab = s.size() > n ? lower(s[n]) : "";
    std::string b;
    for (size_t i = 0; i < n; i++) b += "/" + s[i];
    return ytBase() + b;
}

long get(const std::string& url, const std::string& cookies, std::string& body) {
    RequestInfo r;
    r.url = url;
    r.userAgent = kUa;
    r.cookies = cookies.find("SOCS=") != std::string::npos ? cookies : (cookies.empty() ? "SOCS=CAI" : cookies + "; SOCS=CAI");
    r.headers["Accept-Language"] = "en-US,en;q=0.9";
    return http::get(r, body, nullptr, 16 * 1024 * 1024, 30);
}

// the JSON object that starts at the first '{' after `marker`
Json jsonAfter(const std::string& text, const std::string& marker) {
    size_t at = text.find(marker);
    if (at == std::string::npos) return Json();
    size_t s = text.find('{', at + marker.size());
    if (s == std::string::npos || s - at > marker.size() + 8) return Json();
    int depth = 0;
    bool str = false;
    for (size_t i = s; i < text.size(); i++) {
        char c = text[i];
        if (str) { if (c == '\\') i++; else if (c == '"') str = false; continue; }
        if (c == '"') str = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return Json::parse(text.substr(s, i - s + 1));
    }
    return Json();
}

std::string textOf(const Json& t) {   // {"runs":[{"text":…}]} / {"simpleText":…} / {"content":…}
    if (t.isString()) return t.str();
    if (t["simpleText"].isString()) return t["simpleText"].str();
    if (t["content"].isString()) return t["content"].str();
    std::string s;
    for (auto& r : t["runs"].list()) s += r["text"].str();
    return s;
}
int secondsOf(const std::string& s) {   // "1:02:03" / "12:34"
    int total = 0, part = 0, colons = 0;
    for (char c : s) {
        if (isdigit(static_cast<unsigned char>(c))) part = part * 10 + (c - '0');
        else if (c == ':') { total = total * 60 + part; part = 0; colons++; }
        else if (c != ' ') return 0;
    }
    return colons ? total * 60 + part : 0;
}
// a time like "12:34" anywhere inside (badges on the thumbnail)
int durationInside(const Json& j, int depth = 0) {
    if (depth > 12) return 0;
    if (j.isString()) { std::string s = j.str(); return s.size() <= 8 && s.find(':') != std::string::npos ? secondsOf(s) : 0; }
    if (j.isObject()) for (auto& kv : j.items()) { if (kv.first == "accessibility" || kv.first == "accessibilityData") continue; int d = durationInside(kv.second, depth + 1); if (d) return d; }
    if (j.isArray()) for (auto& x : j.list()) { int d = durationInside(x, depth + 1); if (d) return d; }
    return 0;
}

struct Collect {
    std::vector<Item> items;
    std::set<std::string> ids;
    std::string token;
    void add(const std::string& id, const std::string& title, int dur) {
        if (id.size() != 11 || ids.count(id)) return;
        ids.insert(id);
        Item it;
        it.id = id;
        it.url = "https://www.youtube.com/watch?v=" + id;
        it.title = trim(title);
        it.duration = dur;
        items.push_back(it);
    }
};

// Every video in YouTube's page data, whichever way the page shows it (the walk doesn't depend on its layout).
void walk(const Json& j, Collect& c, int depth) {
    if (depth > 60) return;
    if (j.isArray()) { for (auto& x : j.list()) walk(x, c, depth + 1); return; }
    if (!j.isObject()) return;
    for (auto& kv : j.items()) {
        const std::string& k = kv.first;
        const Json& v = kv.second;
        if (!v.isObject()) { if (v.isArray()) walk(v, c, depth + 1); continue; }
        if (k == "playlistVideoRenderer" || k == "videoRenderer" || k == "gridVideoRenderer" || k == "compactVideoRenderer" || k == "playlistPanelVideoRenderer" || k == "reelItemRenderer") {
            std::string title = textOf(v["title"]);
            if (title.empty()) title = textOf(v["headline"]);
            int d = v["lengthSeconds"].isString() ? atoi(v["lengthSeconds"].str().c_str()) : secondsOf(textOf(v["lengthText"]));
            if (!d) d = durationInside(v["thumbnailOverlays"]);
            if (v["isPlayable"].isBool() && !v["isPlayable"].boolean()) continue;   // deleted / private
            c.add(v["videoId"].str(), title, d);
            continue;
        }
        if (k == "lockupViewModel") {
            std::string type = v["contentType"].str();
            if (type.empty() || type == "LOCKUP_CONTENT_TYPE_VIDEO") c.add(v["contentId"].str(), textOf(v["metadata"]["lockupMetadataViewModel"]["title"]), durationInside(v["contentImage"]));
            continue;
        }
        if (k == "shortsLockupViewModel") {
            std::string id = v["onTap"]["innertubeCommand"]["reelWatchEndpoint"]["videoId"].str();
            if (id.empty()) { std::string e = v["entityId"].str(); size_t u = e.rfind('-'); if (u != std::string::npos) id = e.substr(u + 1); }
            c.add(id, textOf(v["overlayMetadata"]["primaryText"]), 0);
            continue;
        }
        if (k == "continuationCommand" && v["token"].isString()) { c.token = v["token"].str(); continue; }
        walk(v, c, depth + 1);
    }
}

bool readYouTube(const std::string& url, const std::string& cookies, List& out, std::function<void(const std::string&)> status, int most) {
    std::string listId = param(url, "list"), tab;
    std::string chan = listId.empty() ? channelBase(url, tab) : "";
    std::string page;
    if (!listId.empty()) {
        if (startsWith(listId, "RD")) { out.error = "This is one of YouTube's endless mixes, not a playlist."; return false; }
        page = ytBase() + "/playlist?list=" + listId;
        out.kind = "playlist";
    } else if (!chan.empty()) {
        if (tab != "shorts" && tab != "streams") tab = "videos";
        page = chan + "/" + tab;
        out.kind = "channel";
    } else { out.error = "This isn't a playlist or a channel."; return false; }
    if (status) status("Reading the list…");
    std::string html;
    long st = get(page, cookies, html);
    if (st != 200) { out.error = st == 0 ? "Couldn't reach YouTube. Check your internet connection." : st == 404 ? "YouTube says this list doesn't exist." : "YouTube answered " + std::to_string(st) + "."; return false; }
    Json data = jsonAfter(html, "var ytInitialData = ");
    if (!data.isObject()) data = jsonAfter(html, "window[\"ytInitialData\"] = ");
    if (!data.isObject()) { out.error = "YouTube's page couldn't be read (it may be private)."; return false; }
    // the list's name and owner
    const Json& meta = data["metadata"];
    out.title = meta["playlistMetadataRenderer"]["title"].str();
    if (out.title.empty()) out.title = meta["channelMetadataRenderer"]["title"].str();
    if (out.title.empty()) out.title = textOf(data["header"]["pageHeaderRenderer"]["pageTitle"]);
    if (out.title.empty()) { size_t a = html.find("<title>"), b = html.find("</title>"); if (a != std::string::npos && b > a) out.title = trim(replaceAll(html.substr(a + 7, b - a - 7), " - YouTube", "")); }
    out.author = meta["channelMetadataRenderer"]["title"].str();
    std::string channelId = meta["channelMetadataRenderer"]["externalId"].str();
    if (out.kind == "playlist") out.feed = ytBase() + "/feeds/videos.xml?playlist_id=" + listId;
    else if (!channelId.empty()) out.feed = ytBase() + "/feeds/videos.xml?channel_id=" + channelId;
    // the videos, then the next pages of the list
    std::string version = "2.20251020.00.00";
    { size_t v = html.find("\"INNERTUBE_CLIENT_VERSION\":\""); if (v != std::string::npos) { size_t e = html.find('"', v + 28); if (e != std::string::npos && e - v < 60) version = html.substr(v + 28, e - v - 28); } }
    Collect c;
    walk(data, c, 0);
    std::set<std::string> usedTokens;
    for (int pageNo = 0; pageNo < 200 && !c.token.empty() && static_cast<int>(c.items.size()) < most; pageNo++) {
        if (usedTokens.count(c.token)) break;
        usedTokens.insert(c.token);
        if (status) status("Reading the list… " + std::to_string(c.items.size()) + " videos");
        Json body = Json::object(), ctx = Json::object(), client = Json::object();
        client["clientName"] = "WEB"; client["clientVersion"] = version; client["hl"] = "en"; client["gl"] = "US";
        ctx["client"] = client;
        body["context"] = ctx;
        body["continuation"] = c.token;
        RequestInfo r;
        r.url = ytBase() + "/youtubei/v1/browse?prettyPrint=false";
        r.userAgent = kUa;
        r.cookies = "SOCS=CAI";
        r.headers["Origin"] = "https://www.youtube.com";
        r.headers["X-YouTube-Client-Name"] = "1";
        r.headers["X-YouTube-Client-Version"] = version;
        std::string resp;
        c.token.clear();
        if (http::post(r, body.dump(), "application/json", resp, 30) != 200) break;
        walk(Json::parse(resp), c, 0);
    }
    out.items = c.items;
    if (static_cast<int>(out.items.size()) > most) out.items.resize(static_cast<size_t>(most));
    if (out.items.empty()) { out.error = "This list has no videos DDM can download (they may be private or members-only)."; return false; }
    return true;
}

// any other page: the links to video pages on the same site
bool readPage(const std::string& url, const std::string& cookies, List& out, int most) {
    std::string html;
    RequestInfo r;
    r.url = url; r.cookies = cookies; r.userAgent = kUa;
    long st = http::get(r, html, nullptr, 16 * 1024 * 1024, 30);
    if (st != 200) { out.error = st == 0 ? "Couldn't reach the site." : "The site answered " + std::to_string(st) + "."; return false; }
    out.kind = "page";
    { size_t a = lower(html).find("<title"), b = lower(html).find("</title>"); if (a != std::string::npos && b > a) { size_t gt = html.find('>', a); if (gt < b) out.title = trim(html.substr(gt + 1, b - gt - 1)); } }
    std::string host = lower(urlHost(url)), low = lower(html);
    std::set<std::string> seen;
    static const char* marks[] = {"/watch", "/video/", "/videos/", "/v/", "/view_video", "/embed/", "/reel/", "/reels/", "/shorts/", "/clip/", "/clips/", "/play/", "/movies/", "/episode"};
    for (size_t at = 0; static_cast<int>(out.items.size()) < most && (at = low.find("<a ", at)) != std::string::npos; at += 3) {
        size_t end = low.find("</a>", at);
        size_t gt = low.find('>', at);
        if (gt == std::string::npos || gt - at > 4096) continue;
        std::string tag = html.substr(at, gt - at);
        size_t h = lower(tag).find("href=");
        if (h == std::string::npos || h + 6 >= tag.size()) continue;
        char q = tag[h + 5];
        std::string href;
        if (q == '"' || q == '\'') { size_t e = tag.find(q, h + 6); if (e == std::string::npos) continue; href = tag.substr(h + 6, e - h - 6); }
        else { size_t e = tag.find_first_of(" >", h + 5); href = tag.substr(h + 5, e == std::string::npos ? std::string::npos : e - h - 5); }
        href = replaceAll(href, "&amp;", "&");
        std::string full = href;
        if (startsWith(href, "//")) full = "https:" + href;
        else if (startsWith(href, "/")) { size_t s = url.find("://"); size_t p = url.find('/', s + 3); full = url.substr(0, p) + href; }
        else if (!startsWith(lower(href), "http")) continue;
        if (lower(urlHost(full)) != host) continue;
        std::string path = lower(pathOf(full));
        bool video = false;
        for (auto m : marks) { size_t k = path.find(m); if (k != std::string::npos && path.size() > k + strlen(m) + 2) video = true; }
        if (!video || full == url || seen.count(full)) continue;
        seen.insert(full);
        Item it;
        it.url = full;
        std::string inner = end != std::string::npos && end > gt ? html.substr(gt + 1, std::min<size_t>(end - gt - 1, 2000)) : "";
        std::string text;
        bool inTag = false;
        for (char ch : inner) { if (ch == '<') inTag = true; else if (ch == '>') { inTag = false; text += ' '; } else if (!inTag) text += ch; }
        text = trim(replaceAll(replaceAll(text, "\n", " "), "  ", " "));
        size_t ti = lower(tag).find("title=\"");
        if (text.size() < 3 && ti != std::string::npos) { size_t e = tag.find('"', ti + 7); if (e != std::string::npos) text = tag.substr(ti + 7, e - ti - 7); }
        if (text.size() > 160) text = text.substr(0, 160);
        it.title = text.empty() ? stem(fileNameFromUrl(full)) : text;
        it.id = full;
        out.items.push_back(it);
    }
    if (out.items.empty()) { out.error = "No links to videos were found on this page."; return false; }
    return true;
}

std::mutex g_subMu;
std::string subsPath() { return joinPath(dataDir(), "subscriptions.json"); }
std::vector<Sub> loadSubs() {
    std::vector<Sub> v;
    std::string s;
    if (!readFile(subsPath(), s)) return v;
    Json all = Json::parse(s);   // (kept: the loop reads from it)
    for (auto& j : all.list()) {
        Sub x;
        x.id = j["id"].str(); x.name = j["name"].str(); x.url = j["url"].str(); x.feed = j["feed"].str(); x.quality = j["quality"].str(); x.folder = j["folder"].str();
        for (auto& k : j["seen"].list()) x.seen.push_back(k.str());
        x.added = static_cast<int64_t>(j["added"].num()); x.lastCheck = static_cast<int64_t>(j["lastCheck"].num()); x.lastFound = static_cast<int64_t>(j["lastFound"].num());
        x.downloaded = j["downloaded"].i32(); x.paused = j["paused"].boolean();
        if (!x.id.empty() && !x.url.empty()) v.push_back(x);
    }
    return v;
}
void saveSubs(const std::vector<Sub>& v) {
    Json a = Json::array();
    for (auto& x : v) {
        Json j = Json::object();
        j["id"] = x.id; j["name"] = x.name; j["url"] = x.url; j["feed"] = x.feed; j["quality"] = x.quality; j["folder"] = x.folder;
        Json s = Json::array();
        size_t from = x.seen.size() > 5000 ? x.seen.size() - 5000 : 0;
        for (size_t i = from; i < x.seen.size(); i++) s.push(x.seen[i]);
        j["seen"] = s;
        j["added"] = static_cast<double>(x.added); j["lastCheck"] = static_cast<double>(x.lastCheck); j["lastFound"] = static_cast<double>(x.lastFound);
        j["downloaded"] = x.downloaded; j["paused"] = x.paused;
        a.push(j);
    }
    writeFileAtomic(subsPath(), a.dump());
}
} // namespace

bool isListUrl(const std::string& url) {
    if (!isYt(url)) return false;
    std::string p = pathOf(url), tab;
    if (!param(url, "list").empty() && param(url, "v").empty() && !startsWith(param(url, "list"), "RD")) return true;   // a playlist (not a video in one)
    if (startsWith(p, "/playlist")) return !param(url, "list").empty();
    return !channelBase(url, tab).empty() && (tab.empty() || tab == "videos" || tab == "shorts" || tab == "streams" || tab == "featured");
}

bool read(const std::string& url, const std::string& cookies, List& out, std::function<void(const std::string&)> status, int most) {
    out = List();
    out.url = url;
    if (isYt(url)) return readYouTube(url, cookies, out, status, most);
    return readPage(url, cookies, out, most);
}

bool latest(const std::string& url, const std::string& feed, List& out) {
    out = List();
    out.url = url;
    if (!feed.empty()) {   // YouTube's feed: the 15 newest, in one small request
        std::string xml;
        if (get(feed, "", xml) == 200) {
            for (size_t at = 0; (at = xml.find("<entry>", at)) != std::string::npos; at += 7) {
                size_t end = xml.find("</entry>", at);
                if (end == std::string::npos) break;
                std::string e = xml.substr(at, end - at);
                auto tagText = [&](const std::string& tag) { size_t a = e.find("<" + tag + ">"); size_t b = e.find("</" + tag + ">"); return a == std::string::npos || b == std::string::npos ? std::string() : e.substr(a + tag.size() + 2, b - a - tag.size() - 2); };
                Item it;
                it.id = tagText("yt:videoId");
                if (it.id.size() != 11) continue;
                it.url = "https://www.youtube.com/watch?v=" + it.id;
                it.title = replaceAll(replaceAll(replaceAll(replaceAll(tagText("title"), "&amp;", "&"), "&quot;", "\""), "&#39;", "'"), "&lt;", "<");
                out.items.push_back(it);
            }
            if (!out.items.empty()) return true;
        }
    }
    return read(url, "", out, nullptr, 60);
}

Task videoTask(const std::string& pageUrl, const std::string& title, const std::string& quality, const std::string& folder) {
    Task t;
    t.kind = "video";
    t.url = pageUrl;
    t.pageUrl = pageUrl;
    bool mp3 = lower(quality) == "mp3";
    std::string name = sanitizeFileName(trim(title).empty() ? "video" : trim(title));
    if (name.size() > 150) name = name.substr(0, 150);
    t.fileName = name + (mp3 ? ".mp3" : ".mp4");
    if (mp3) { t.convertTo = "mp3"; t.formatLabel = "MP3"; }
    else if (!quality.empty() && lower(quality) != "best") t.formatLabel = quality;
    t.dir = folder;
    t.category = categoryFor(t.fileName);
    return t;
}

std::vector<Sub> subscriptions() { std::lock_guard<std::mutex> g(g_subMu); return loadSubs(); }

std::string subscribe(const List& l, const std::string& quality, const std::string& folder) {
    std::lock_guard<std::mutex> g(g_subMu);
    auto v = loadSubs();
    for (auto& x : v) if (x.url == l.url) { x.quality = quality; x.folder = folder; x.paused = false; saveSubs(v); return x.id; }
    Sub s;
    s.id = "s" + std::to_string(nowMs());
    s.name = l.title.empty() ? l.url : l.title;
    s.url = l.url; s.feed = l.feed; s.quality = quality; s.folder = folder;
    s.added = s.lastCheck = nowMs() / 1000;
    for (auto& it : l.items) s.seen.push_back(it.id.empty() ? it.url : it.id);
    v.push_back(s);
    saveSubs(v);
    return s.id;
}

void unsubscribe(const std::string& id) {
    std::lock_guard<std::mutex> g(g_subMu);
    auto v = loadSubs();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Sub& x) { return x.id == id; }), v.end());
    saveSubs(v);
}

void setPaused(const std::string& id, bool paused) {
    std::lock_guard<std::mutex> g(g_subMu);
    auto v = loadSubs();
    for (auto& x : v) if (x.id == id) x.paused = paused;
    saveSubs(v);
}

std::vector<Task> checkSubscriptions(bool force) {
    std::vector<Sub> v;
    { std::lock_guard<std::mutex> g(g_subMu); v = loadSubs(); }
    std::vector<Task> out;
    int64_t now = nowMs() / 1000;
    std::vector<std::pair<std::string, Sub>> changed;
    for (auto& s : v) {
        if (s.paused || (!force && now - s.lastCheck < 3 * 3600)) continue;
        List l;
        bool ok = latest(s.url, s.feed, l);
        s.lastCheck = now;
        if (ok) {
            std::set<std::string> seen(s.seen.begin(), s.seen.end());
            std::vector<Item> fresh;
            for (auto& it : l.items) { std::string key = it.id.empty() ? it.url : it.id; if (!seen.count(key)) fresh.push_back(it); }
            std::reverse(fresh.begin(), fresh.end());   // oldest first
            for (auto& it : fresh) {
                std::string folder = s.folder;
                out.push_back(videoTask(it.url, it.title, s.quality, folder));
                s.seen.push_back(it.id.empty() ? it.url : it.id);
                s.downloaded++;
                s.lastFound = now;
            }
            if (!fresh.empty()) logLine("subscriptions: " + std::to_string(fresh.size()) + " new in " + s.name);
        }
        changed.push_back({s.id, s});
    }
    if (!changed.empty()) {   // saved again over the newest copy (the list may have changed meanwhile)
        std::lock_guard<std::mutex> g(g_subMu);
        auto cur = loadSubs();
        for (auto& c : cur) for (auto& ch : changed) if (c.id == ch.first) { auto paused = c.paused; c.seen = ch.second.seen; c.lastCheck = ch.second.lastCheck; c.lastFound = ch.second.lastFound; c.downloaded = ch.second.downloaded; c.paused = paused; }
        saveSubs(cur);
    }
    return out;
}

} // namespace playlists
} // namespace ddm
