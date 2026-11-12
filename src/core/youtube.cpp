#include "youtube.hpp"
#include "http.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>
#include <regex>

namespace ddm {
namespace youtube {

namespace {

// YouTube's app for Apple Vision Pro: its streams come as plain addresses (nothing to decode, no extra token), the
// same way the best-known downloaders ask today.
const char* kClientName = "VISIONOS";
const char* kClientVersion = "1.02";
const char* kClientId = "101";
const char* kClientUA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
const char* kWebUA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36";

// tests only (DDM_TEST_UI with DDM_TEST_YOUTUBE=http://127.0.0.1:port): a stand-in YouTube
std::string base() {
    static std::string b = [] {
        char v[256] = {};
        if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0) && GetEnvironmentVariableA("DDM_TEST_YOUTUBE", v, sizeof v)) return std::string(v);
        return std::string("https://www.youtube.com");
    }();
    return b;
}

std::string queryParam(const std::string& url, const std::string& key) {
    size_t q = url.find('?');
    if (q == std::string::npos) return "";
    for (auto& kv : split(url.substr(q + 1), '&')) {
        size_t eq = kv.find('=');
        std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
        if (k == key) return urlDecode(eq == std::string::npos ? "" : kv.substr(eq + 1));
    }
    return "";
}

bool validId(const std::string& id) {
    if (id.size() != 11) return false;
    for (char c : id) if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return true;
}

// a JSON value written into a page as  name = {...};  (balanced braces, strings respected)
std::string jsonAfter(const std::string& page, const std::string& marker) {
    size_t p = page.find(marker);
    if (p == std::string::npos) return "";
    p = page.find('{', p + marker.size());
    if (p == std::string::npos) return "";
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = p; i < page.size(); i++) {
        char c = page[i];
        if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; continue; }
        if (c == '"') inStr = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return page.substr(p, i - p + 1);
    }
    return "";
}

std::string textOf(const Json& j) {   // {"simpleText": "…"} or {"runs": [{"text": "…"}, …]}
    if (j["simpleText"].isString()) return j["simpleText"].str();
    std::string s;
    for (auto& r : j["runs"].list()) s += r["text"].str();
    return s;
}

std::string friendlyReason(const std::string& status, const std::string& reason) {
    std::string r = lower(reason);
    if (r.find("not a bot") != std::string::npos) return "YouTube asked to confirm this isn't a bot. Make sure you're signed in to YouTube in this browser, then play the video again.";
    if (r.find("age") != std::string::npos) return "This video is age-restricted: YouTube wants you to be signed in.";
    if (r.find("private") != std::string::npos) return "This video is private.";
    if (r.find("members") != std::string::npos || r.find("join this channel") != std::string::npos) return "This video is for the channel's members only.";
    if (status == "LIVE_STREAM_OFFLINE") return "This live stream hasn't started yet.";
    if (!reason.empty()) return reason;
    return "YouTube didn't give this video.";
}

// "video/mp4; codecs=\"avc1.640028\"" -> ext mp4, codec avc1.640028
void parseMime(const std::string& mime, Stream& s) {
    std::string type = lower(trim(mime.substr(0, mime.find(';'))));
    std::string codecs;
    size_t c = mime.find("codecs=\"");
    if (c != std::string::npos) { size_t e = mime.find('"', c + 8); codecs = mime.substr(c + 8, e == std::string::npos ? std::string::npos : e - c - 8); }
    auto parts = split(codecs, ',');
    for (auto& p : parts) p = lower(trim(p));
    bool video = startsWith(type, "video/"), audio = startsWith(type, "audio/");
    std::string sub = type.substr(type.find('/') + 1);
    s.ext = audio ? (sub == "mp4" ? "m4a" : sub == "webm" ? "webm" : sub) : (sub == "3gpp" ? "3gp" : sub);
    if (video && parts.size() >= 2) { s.vcodec = parts[0]; s.acodec = parts[1]; }
    else if (video) { s.vcodec = parts.empty() ? "unknown" : parts[0]; s.acodec = "none"; }
    else if (audio) { s.vcodec = "none"; s.acodec = parts.empty() ? "unknown" : parts[0]; }
}

// pre-roll ads in the answer: YouTube serves the streams only after they would have played
int64_t waitFrom(const Json& pr) {
    int64_t wait = 0;
    auto add = [&](const Json& r) {
        double skip = r["skipOffsetMilliseconds"].num(-1);
        if (skip >= 0) { wait += static_cast<int64_t>((skip + 999) / 1000); return; }
        std::string pv = r["playerVars"].str();
        std::string len = queryParam("?" + pv, "length_seconds");
        if (!len.empty()) wait += atoll(len.c_str());
    };
    for (auto& p : pr["adPlacements"].list()) {
        const Json& cfg = p["adPlacementRenderer"]["config"]["adPlacementConfig"];
        if (cfg["kind"].str() != "AD_PLACEMENT_KIND_START") continue;
        const Json& r = p["adPlacementRenderer"]["renderer"]["instreamVideoAdRenderer"];
        if (r.isObject()) add(r);
    }
    return wait;
}

struct Session { std::string visitor; int64_t at = 0; };
std::mutex g_mu;
Session g_session;   // YouTube's visitor id: one per DDM session (like a browser keeps its own)

std::string cookieHeader(const std::string& cookies) {
    // YouTube's cookie consent (Europe): accept the basic one so the page is the video's, not a consent form
    if (cookies.find("SOCS=") != std::string::npos || cookies.find("CONSENT=") != std::string::npos) return cookies;
    return cookies.empty() ? "SOCS=CAI" : cookies + "; SOCS=CAI";
}

bool askPlayer(const std::string& id, const std::string& visitor, Json& pr, std::string& err) {
    Json client = Json::object();
    client["clientName"] = kClientName; client["clientVersion"] = kClientVersion;
    client["deviceMake"] = "Apple"; client["deviceModel"] = "RealityDevice17,1";
    client["userAgent"] = kClientUA; client["osName"] = "visionOS"; client["osVersion"] = "26.5.23O471";
    client["hl"] = "en"; client["timeZone"] = "UTC"; client["utcOffsetMinutes"] = 0;
    if (!visitor.empty()) client["visitorData"] = visitor;
    Json ctx = Json::object();
    ctx["client"] = client;
    Json body = Json::object();
    body["context"] = ctx;
    body["videoId"] = id;
    Json cpc = Json::object();
    cpc["html5Preference"] = "HTML5_PREF_WANTS";
    Json pc = Json::object();
    pc["contentPlaybackContext"] = cpc;
    body["playbackContext"] = pc;
    body["contentCheckOk"] = true;
    body["racyCheckOk"] = true;
    RequestInfo r;
    r.url = base() + "/youtubei/v1/player?prettyPrint=false";
    r.userAgent = kClientUA;
    r.headers["X-YouTube-Client-Name"] = kClientId;
    r.headers["X-YouTube-Client-Version"] = kClientVersion;
    r.headers["Origin"] = "https://www.youtube.com";
    if (!visitor.empty()) r.headers["X-Goog-Visitor-Id"] = visitor;
    std::string out;
    long st = http::post(r, body.dump(), "application/json", out, 30);
    if (st != 200) { err = st == 0 ? "Couldn't reach YouTube. Check your internet connection." : "YouTube answered " + std::to_string(st) + "."; return false; }
    bool ok = false;
    pr = Json::parse(out, &ok);
    if (!ok || !pr.isObject()) { err = "YouTube's answer couldn't be read."; return false; }
    return true;
}

} // namespace

std::string videoId(const std::string& url0) {
    std::string url = trim(url0);
    std::string l = lower(url);
    std::string host = urlSite(url);
    std::string id;
    if (host == "youtu.be") {
        size_t s = url.find("youtu.be/");
        if (s != std::string::npos) id = url.substr(s + 9, 11);
    } else if (host == "youtube.com" || endsWith(host, ".youtube.com") || host == "youtube-nocookie.com" || endsWith(host, ".youtube-nocookie.com")) {
        id = queryParam(url, "v");
        for (const char* p : {"/shorts/", "/embed/", "/live/", "/v/", "/e/"}) {
            if (!id.empty()) break;
            size_t s = l.find(p);
            if (s != std::string::npos) id = url.substr(s + strlen(p), 11);
        }
    }
    return validId(id) ? id : "";
}

int itagOf(const std::string& u) { std::string v = queryParam(u, "itag"); return v.empty() ? 0 : atoi(v.c_str()); }
int64_t clenOf(const std::string& u) { std::string v = queryParam(u, "clen"); return v.empty() ? -1 : _atoi64(v.c_str()); }

bool read(const std::string& url, const std::string& cookies, Video& out, std::function<void(const std::string&)> status) {
    out = Video();
    out.id = videoId(url);
    if (out.id.empty()) { out.error = "This YouTube address isn't one video."; return false; }
    out.userAgent = kClientUA;
    if (status) status("Getting the video's qualities…");
    // 1) the watch page: the title and description, and YouTube's visitor id for this session
    std::string visitor;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_session.visitor.empty() && nowMs() - g_session.at < 6LL * 3600 * 1000) visitor = g_session.visitor;
    }
    Json page;
    {
        RequestInfo r;
        r.url = base() + "/watch?v=" + out.id + "&bpctr=9999999999&has_verified=1";
        r.userAgent = kWebUA;
        r.cookies = cookieHeader(cookies);
        r.headers["Accept-Language"] = "en-US,en;q=0.9";
        std::string html;
        if (http::get(r, html, nullptr, 8 * 1024 * 1024, 30) == 200) {
            if (visitor.empty()) {
                size_t at = html.find("\"VISITOR_DATA\":\"");   // (a plain search: the page is a megabyte)
                size_t end = at == std::string::npos ? at : html.find('"', at + 16);
                if (end != std::string::npos && end - at - 16 < 512) {
                    visitor = html.substr(at + 16, end - at - 16);
                    std::lock_guard<std::mutex> g(g_mu);
                    g_session.visitor = visitor; g_session.at = nowMs();
                }
            }
            std::string pj = jsonAfter(html, "ytInitialPlayerResponse");
            if (!pj.empty()) page = Json::parse(pj);
        }
    }
    // 2) the streams
    Json pr;
    if (!askPlayer(out.id, visitor, pr, out.error)) return false;
    const Json& ps = pr["playabilityStatus"];
    std::string st = ps["status"].str();
    if (st != "OK") {
        out.error = friendlyReason(st, ps["reason"].str());
        out.tryHelper = st != "ERROR";   // "Video unavailable": the helper can't do better
        return false;
    }
    const Json& vd = pr["videoDetails"].isObject() ? pr["videoDetails"] : page["videoDetails"];
    const Json& pvd = page["videoDetails"].isObject() ? page["videoDetails"] : vd;
    if (vd["videoId"].isString() && vd["videoId"].str() != out.id) { out.error = "YouTube answered about another video."; return false; }
    out.title = pvd["title"].str(vd["title"].str());
    out.author = pvd["author"].str(vd["author"].str());
    out.description = pvd["shortDescription"].str(vd["shortDescription"].str()).substr(0, 2000);
    out.duration = atoi(vd["lengthSeconds"].str(pvd["lengthSeconds"].str("0")).c_str());
    bool live = vd["isLive"].boolean() || vd["isUpcoming"].boolean();
    if (vd["isUpcoming"].boolean() && !vd["isLive"].boolean()) { out.error = "This live stream hasn't started yet."; out.tryHelper = false; return false; }
    {   // the largest picture
        int best = 0;
        for (auto& t : pvd["thumbnail"]["thumbnails"].list()) { int w = t["width"].i32(); if (w >= best && t["url"].isString()) { best = w; out.thumbnail = t["url"].str(); } }
        if (out.thumbnail.empty()) out.thumbnail = "https://i.ytimg.com/vi/" + out.id + "/hqdefault.jpg";
        size_t q = out.thumbnail.find('?');
        if (q != std::string::npos) out.thumbnail = out.thumbnail.substr(0, q);
    }
    for (auto& c : pr["captions"]["playerCaptionsTracklistRenderer"]["captionTracks"].list()) {
        Caption k;
        k.url = c["baseUrl"].str(); k.lang = c["languageCode"].str(); k.name = textOf(c["name"]); k.automatic = c["kind"].str() == "asr";
        if (!k.url.empty() && !k.lang.empty()) out.captions.push_back(k);
    }
    int64_t wait = waitFrom(pr);
    if (wait > 0) out.notBefore = static_cast<int64_t>(time(nullptr)) + wait;
    // 3) the streams that can be fetched as they are
    const Json& sd = pr["streamingData"];
    out.hls = sd["hlsManifestUrl"].str();   // live streams (and some videos) as an HLS stream: DDM's stream engine
    if (live) {
        if (out.hls.empty()) { out.error = "YouTube didn't give this live stream to DDM. Try again when it has ended."; return false; }
        out.live = true;
        return true;
    }
    for (const char* list : {"formats", "adaptiveFormats"}) {
        for (auto& f : sd[list].list()) {
            Stream s;
            s.itag = f["itag"].i32();
            s.url = f["url"].str();
            if (s.url.empty()) continue;                                   // a scrambled address
            if (!queryParam(s.url, "n").empty()) continue;                 // an address YouTube slows down until it's decoded
            if (f["drmFamilies"].isArray() && f["drmFamilies"].size() > 0) continue;
            if (f["type"].str() == "FORMAT_STREAM_TYPE_OTF" || f["targetDurationSec"].isNumber()) continue;   // live / pieced streams
            if (f["isDrc"].boolean()) continue;                            // "stable volume" copies of the sound
            s.mime = f["mimeType"].str();
            parseMime(s.mime, s);
            if (s.ext.empty()) continue;
            s.width = f["width"].i32(); s.height = f["height"].i32(); s.fps = f["fps"].i32();
            s.kbps = (f["averageBitrate"].isNumber() ? f["averageBitrate"].num() : f["bitrate"].num()) / 1000.0;
            std::string cl = f["contentLength"].str();
            s.size = cl.empty() ? -1 : _atoi64(cl.c_str());
            if (f["audioTrack"].isObject()) {
                s.audioLang = f["audioTrack"]["id"].str();
                s.defaultAudio = f["audioTrack"]["audioIsDefault"].boolean(true);
            }
            out.streams.push_back(s);
        }
    }
    if (out.streams.empty() && out.hls.empty()) { out.error = "YouTube didn't give plain streams for this video."; return false; }
    return true;
}

std::string freshUrl(const std::string& id, int itag, const std::string& cookies) {
    Video v;
    if (!read("https://www.youtube.com/watch?v=" + id, cookies, v)) return "";
    for (auto& s : v.streams) if (s.itag == itag) return s.url;
    return "";
}

std::string vttToSrt(const std::string& vtt) {
    std::string out, text, times;
    int n = 0;
    auto flush = [&] {
        if (!times.empty() && !trim(text).empty()) {
            out += std::to_string(++n) + "\r\n" + times + "\r\n" + trim(text) + "\r\n\r\n";
        }
        times.clear(); text.clear();
    };
    auto fixTime = [](std::string t) {   // 00:01.500 -> 00:00:01,500
        t = trim(t);
        size_t sp = t.find(' ');
        if (sp != std::string::npos) t = t.substr(0, sp);
        if (std::count(t.begin(), t.end(), ':') == 1) t = "00:" + t;
        std::replace(t.begin(), t.end(), '.', ',');
        return t;
    };
    for (auto line : split(replaceAll(vtt, "\r", ""), '\n')) {
        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            flush();
            times = fixTime(line.substr(0, arrow)) + " --> " + fixTime(line.substr(arrow + 3));
            continue;
        }
        if (times.empty()) continue;   // header, notes, styles
        if (trim(line).empty()) { flush(); continue; }
        std::string clean;   // without <c>, <00:00:01.000> and other markup
        bool tag = false;
        for (char c : line) { if (c == '<') tag = true; else if (c == '>') tag = false; else if (!tag) clean += c; }
        clean = replaceAll(replaceAll(replaceAll(clean, "&amp;", "&"), "&lt;", "<"), "&gt;", ">");
        if (!trim(clean).empty() && text.find(trim(clean)) == std::string::npos) text += (text.empty() ? "" : "\r\n") + trim(clean);
    }
    flush();
    return out;
}

} // namespace youtube
} // namespace ddm
