#include "media.hpp"
#include "engines.hpp"
#include "sites.hpp"
#include "streams.hpp"
#include "siterules.hpp"
#include "youtube.hpp"
#include "http.hpp"
#include "process.hpp"
#include "settings.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <regex>
#include <set>
#include <thread>

namespace ddm {

bool isVideoSite(const std::string& url) {
    static const char* sites[] = {"youtube.com", "youtu.be", "youtube-nocookie.com", "facebook.com", "fb.watch", "tiktok.com", "instagram.com", "twitter.com", "x.com",
                                  "vimeo.com", "dailymotion.com", "dai.ly", "twitch.tv", "reddit.com", "v.redd.it", "bilibili.com", "pinterest.com", "linkedin.com", "snapchat.com",
                                  "threads.net", "bilibili.tv", "b23.tv", "soundcloud.com", "rumble.com", "streamable.com", "9gag.com", "imgur.com", "ok.ru", "vk.com", "likee.video", "kwai.com"};
    std::string s = urlSite(url);
    for (auto x : sites) if (s == x || endsWith(s, std::string(".") + x)) return true;
    return false;
}

namespace {

std::string upperExt(const std::string& e) { std::string r = e; for (auto& c : r) c = static_cast<char>(toupper(static_cast<unsigned char>(c))); return r.empty() ? "Audio" : r; }

bool isYouTube(const std::string& url) { std::string s = urlSite(url); return s == "youtube.com" || endsWith(s, ".youtube.com") || s == "youtu.be" || endsWith(s, "youtube-nocookie.com"); }

std::string tagFor(int h) {
    if (h >= 2160) return "4K";
    if (h >= 1440) return "2K";
    if (h >= 720) return "HD";
    return "SD";
}

struct F {
    std::string id, ext, vcodec, acodec, proto, url;
    int height = 0, fps = 0;
    double tbr = 0, abr = 0;
    int64_t size = -1;
    bool exact = false;
    std::map<std::string, std::string> headers;
    bool hasV() const { return !vcodec.empty() && vcodec != "none"; }
    bool hasA() const { return !acodec.empty() && acodec != "none"; }
    bool plain() const { return proto == "https" || proto == "http"; }
    bool avc() const { return startsWith(vcodec, "avc") || startsWith(vcodec, "h264"); }
};

} // namespace

namespace {
std::string oneLine(std::string s) {
    for (auto& c : s) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return s;
}
std::string collapse(const std::string& s) {
    std::string o;
    bool sp = false;
    for (char c : s) { if (c == ' ') { if (!sp && !o.empty()) o += ' '; sp = true; } else { o += c; sp = false; } }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}
// separators and stray punctuation at either end (whole characters: never half of a letter in another alphabet)
std::string trimSeparators(std::string s) {
    static const char* seps[] = {"\u00b7", "\u2022", "\u2013", "\u2014", " ", "-", "|", ":", ",", "."};
    for (bool again = true; again && !s.empty();) {
        again = false;
        for (auto sp : seps) {
            std::string x(sp);
            if (s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0) { s.erase(s.size() - x.size()); again = true; }
            if (s.size() >= x.size() && s.compare(0, x.size(), x) == 0) { s.erase(0, x.size()); again = true; }
        }
    }
    return s;
}
// what a site adds around the real name
std::string cleanPart(std::string s) {
    s = oneLine(s);
    std::string out;
    for (auto& w : split(s, ' ')) {
        std::string lw = lower(w);
        if (startsWith(lw, "http://") || startsWith(lw, "https://") || startsWith(lw, "www.")) continue;   // links
        if (w.size() > 1 && (w[0] == '#' || w[0] == '@')) continue;                                         // #hashtags, @mentions
        out += (out.empty() ? "" : " ") + w;
    }
    out = collapse(out);
    // " | Facebook", " - YouTube", " on TikTok", " • Instagram photos and videos": only after a separator or "on"
    static const std::regex suffix("(?:\\s*(?:\\||-|\u2013|\u2014|\u2022|\u00b7|:)\\s*|\\s+on\\s+)(?:youtube|tiktok|facebook|instagram|twitter|vimeo|dailymotion|reddit)(?:\\s*\\([^)]*\\))?(?:\\s+(?:photos|videos|reels|watch)(?:\\s+and\\s+videos)?)?\\s*$", std::regex::icase);
    static const std::regex notif("^\\(\\d+\\+?\\)\\s*");   // "(3) ", "(20+) " unread count in page titles
    out = std::regex_replace(out, notif, "");
    for (int i = 0; i < 2; i++) out = std::regex_replace(out, suffix, "");
    static const std::regex prefix("^(?:youtube|tiktok|facebook|instagram|twitter)\\s*(?:\\||-|\u2013|\u2014|\u2022|\u00b7|:)\\s*", std::regex::icase);   // "TikTok - …"
    out = std::regex_replace(out, prefix, "");
    out = trimSeparators(out);
    return collapse(out);
}
bool statsPart(const std::string& s) {   // "1.2K views · 30 reactions", "12 comments"
    static const std::regex st("^[\\d.,]+\\s*[kmb]?\\s+(views?|reactions?|comments?|shares?|likes?|plays?|followers?)\\b.*", std::regex::icase);
    return s.size() <= 400 && std::regex_match(s, st);   // (a long caption isn't a "12 views" line; and the regex stays shallow)
}
bool genericName(const std::string& s0, std::string* by = nullptr) {
    std::string s = lower(trim(s0));
    if (s.size() < 3) return true;
    bool letters = false;
    for (unsigned char c : s) if (isalpha(c) || c >= 0x80) letters = true;
    if (!letters) return true;   // only numbers and signs
    static const std::regex byWho("^(video|reel|reels|post|photo|clip|short|story|watch)\\s+(by|from)\\s+(.+)$", std::regex::icase);
    std::smatch m;
    std::string orig = trim(s0);
    if (orig.size() <= 400 && std::regex_match(orig, m, byWho)) { if (by) *by = m[3].str(); return true; }
    static const char* plain[] = {"video", "videos", "reel", "reels", "facebook", "facebook video", "facebook watch", "watch", "instagram", "instagram reel",
                                  "tiktok", "tiktok video", "youtube", "shorts", "untitled", "story", "stories", "post", "media", "clip", "x", "twitter",
                                  "make your day", "tiktok - make your day", "tiktok embed", "tiktok player", "embed", "player", "video player", "dailymotion video player", "jw player", "jwplayer", "vimeo player", "embedded video", "iframe", "loading", "loading...", "index", "instagram embed", "facebook reels", "log in or sign up to view", "log into facebook", "home", "explore", "for you", "following"};
    for (auto p : plain) if (s == p) return true;
    return false;
}
// the best part of a "text | text | By Page" title
std::string bestPart(const std::string& s, std::string* by) {
    std::string best;
    for (auto& part0 : split(replaceAll(s, " \u00b7 ", " | "), '|')) {
        std::string part = cleanPart(part0);
        if (part.empty() || statsPart(part)) continue;
        if (startsWith(lower(part), "by ")) { if (by && by->empty()) *by = trim(part.substr(3)); continue; }
        if (genericName(part, by)) continue;
        if (part.size() > best.size()) best = part;
    }
    return best;
}
} // namespace

std::string niceVideoTitle(const std::string& title, const std::string& description, const std::string& uploader0,
                           const std::string& pageTitle, const std::string& videoId) {
    std::string by = trim(uploader0);
    std::string firstLine = description.substr(0, description.find('\n'));
    std::string name;
    for (const std::string* c : std::initializer_list<const std::string*>{&title, &firstLine, &pageTitle}) {
        name = bestPart(*c, &by);
        if (!name.empty()) break;
    }
    if (name.empty()) {
        std::string id = cleanPart(videoId);
        if (id.empty()) {   // nothing names it: the moment it was found does, so two videos never share one name
            SYSTEMTIME st;
            GetLocalTime(&st);
            char when[40];
            snprintf(when, sizeof when, "%04u-%02u-%02u %02u.%02u.%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            id = when;
        }
        name = !by.empty() ? cleanPart(by) + " - " + id : "video " + id;
    }
    if (name.size() > 100) {   // long captions: cut at a word, never inside a letter
        size_t cut = name.rfind(' ', 100);
        if (cut == std::string::npos || cut < 40) { cut = 100; while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) cut--; }
        name = trimSeparators(trim(name.substr(0, cut)));
    }
    return name;
}

namespace {

std::string lowerExt(std::string e) { for (auto& c : e) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return e; }
bool audioExt(const std::string& e) { static const std::set<std::string> a = {"mp3", "m4a", "aac", "ogg", "opus", "wav", "flac", "weba"}; return a.count(e) > 0; }
bool httpUrl(const std::string& u) { std::string l = lower(u); return startsWith(l, "http://") || startsWith(l, "https://"); }

// a picture saved next to the video (any site): JPEG / PNG / WebP as the server sends it
bool savePicture(const std::string& url, const std::string& referer, const std::string& userAgent, const std::string& dir, const std::string& base) {
    if (!httpUrl(url)) return false;
    RequestInfo r;
    r.url = url; r.referer = referer; r.userAgent = userAgent;
    std::string body;
    if (http::get(r, body, nullptr, 16 * 1024 * 1024, 30) != 200 || body.size() < 512) return false;
    std::string ext = "jpg";
    if (body.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) ext = "png";
    else if (body.size() > 12 && body.compare(0, 4, "RIFF") == 0 && body.compare(8, 4, "WEBP") == 0) ext = "webp";
    else if (!(static_cast<unsigned char>(body[0]) == 0xFF && static_cast<unsigned char>(body[1]) == 0xD8)) return false;   // not a picture
    return writeFileAtomic(joinPath(dir, base + "." + ext), body);
}

} // namespace

int saveVideoExtras(const std::string& pageUrl, const std::string& videoPath, bool thumb, bool subs, const std::string& langs0,
                    const std::string& cookies, const std::string& userAgent, std::string& error) {
    if ((!thumb && !subs) || pageUrl.empty() || !httpUrl(pageUrl)) return 0;
    std::string dir = dirName(videoPath), base = stem(baseName(videoPath));
    if (isYouTube(pageUrl) && !youtube::videoId(pageUrl).empty()) {   // YouTube: DDM's own reader
        youtube::Video v;
        if (!youtube::read(pageUrl, cookies, v)) { error = v.error.empty() ? "YouTube couldn't be read." : v.error; return -1; }
        int made = 0;
        if (thumb && !v.thumbnail.empty()) {
            std::string jpg = joinPath(dir, base + ".jpg");
            std::string big = "https://i.ytimg.com/vi/" + v.id + "/maxresdefault.jpg";
            long st = http::download(big, jpg);
            if ((st != 200 || fileSize(jpg) < 2048) && http::download(v.thumbnail, jpg) != 200) DeleteFileW(W(jpg).c_str()); else made++;
        }
        if (subs) {
            std::string langs;
            for (char c : lower(langs0)) if (isalnum(static_cast<unsigned char>(c)) || c == ',' || c == '-') langs += c;
            if (langs.empty()) langs = "en";
            bool all = langs == "all";
            std::set<std::string> want;
            for (auto& l : split(langs, ',')) if (!l.empty()) want.insert(l);
            std::set<std::string> done;
            int before = made;
            for (int pass = 0; pass < 2; pass++)   // the uploader's own captions first, then YouTube's automatic ones
                for (auto& c : v.captions) {
                    if (c.automatic != (pass == 1)) continue;
                    std::string l = lower(c.lang), primary = l.substr(0, l.find('-'));
                    if (!all && !want.count(l) && !want.count(primary)) continue;
                    if (all && c.automatic) continue;
                    if (done.count(all ? l : primary)) continue;
                    std::string vtt;
                    RequestInfo r;
                    r.url = c.url + "&fmt=vtt";
                    if (http::get(r, vtt, nullptr, 8 * 1024 * 1024, 30) != 200 || vtt.find("-->") == std::string::npos) continue;
                    std::string srt = youtube::vttToSrt(vtt);
                    if (srt.empty()) continue;
                    std::string lang = sanitizeFileName(c.lang);
                    if (writeFileAtomic(joinPath(dir, base + "." + lang + ".srt"), "\xEF\xBB\xBF" + srt)) { made++; done.insert(all ? l : primary); }
                }
            if (made == before && !thumb) error = "This video has no subtitles in " + (all ? std::string("any language") : langs) + ".";
        }
        return made;
    }
    // every other site: the page's own picture
    int made = 0;
    sites::Page pg;
    sites::read(pageUrl, cookies, userAgent, "", pg);
    if (thumb && savePicture(pg.thumbnail, pageUrl, userAgent, dir, base)) made++;
    if (subs && !thumb) error = "This site doesn't offer subtitles for its videos.";
    if (made == 0 && thumb && pg.thumbnail.empty()) error = pg.error.empty() ? "The page has no picture for this video." : pg.error;
    return made;
}

// ------------------------------------------------------------------ subtitles
namespace {
void addSubTrack(std::vector<SubTrack>& list, const SubTrack& t) {
    if (t.url.empty() || list.size() >= 300) return;
    for (auto& x : list) if (x.url == t.url) return;
    list.push_back(t);
}

struct Cue { std::string times, text; };

// "00:01.500" / "1:02:03.5" -> "00:00:01,500" ("" when it isn't a time)
std::string srtTime(std::string t) {
    t = trim(t);
    size_t sp = t.find_first_of(" \t");
    if (sp != std::string::npos) t = t.substr(0, sp);
    std::replace(t.begin(), t.end(), ',', '.');
    auto parts = split(t, ':');
    if (parts.size() < 2 || parts.size() > 3) return "";
    for (char c : t) if (!isdigit(static_cast<unsigned char>(c)) && c != ':' && c != '.') return "";
    double sec = atof(parts.back().c_str());
    int m = atoi(parts[parts.size() - 2].c_str()), h = parts.size() == 3 ? atoi(parts[0].c_str()) : 0;
    long long ms = static_cast<long long>((h * 3600.0 + m * 60.0 + sec) * 1000.0 + 0.5);
    char buf[40];
    snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld,%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
    return buf;
}

// WebVTT or SubRip (one file, or many small ones put together) -> clean SubRip: no markup, no cue twice.
std::string toSrt(const std::string& in) {
    std::vector<Cue> cues;
    Cue cur;
    auto flush = [&] {
        cur.text = trim(cur.text);
        if (!cur.times.empty() && !cur.text.empty()) {
            bool dup = false;   // a cue that spans two pieces is in both; rolling captions repeat the last line
            for (size_t i = cues.size(); i > 0 && i + 6 > cues.size(); i--) if (cues[i - 1].times == cur.times && cues[i - 1].text == cur.text) { dup = true; break; }
            if (!dup) cues.push_back(cur);
        }
        cur = Cue();
    };
    std::string src = replaceAll(in, "\r", "");
    if (startsWith(src, "\xEF\xBB\xBF")) src = src.substr(3);
    for (auto line : split(src, '\n')) {
        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            std::string a = srtTime(line.substr(0, arrow)), b = srtTime(line.substr(arrow + 3));
            if (!a.empty() && !b.empty()) { flush(); cur.times = a + " --> " + b; continue; }
        }
        if (cur.times.empty()) continue;   // headers, notes, styles, cue numbers
        if (trim(line).empty()) { flush(); continue; }
        std::string clean;   // without <c>, <00:00:01.000>, <i> and other markup
        bool tag = false;
        for (char c : line) { if (c == '<') tag = true; else if (c == '>') tag = false; else if (!tag) clean += c; }
        clean = replaceAll(replaceAll(replaceAll(replaceAll(replaceAll(clean, "&amp;", "&"), "&lt;", "<"), "&gt;", ">"), "&nbsp;", " "), "&#39;", "'");
        clean = trim(clean);
        if (!clean.empty() && cur.text.find(clean) == std::string::npos) cur.text += (cur.text.empty() ? "" : "\r\n") + clean;
    }
    flush();
    std::string out;
    int n = 0;
    for (auto& c : cues) out += std::to_string(++n) + "\r\n" + c.times + "\r\n" + c.text + "\r\n\r\n";
    return out;
}
} // namespace

std::string languageName(const std::string& code0) {
    static const std::map<std::string, std::string> names = {
        {"af", "Afrikaans"}, {"sq", "Albanian"}, {"am", "Amharic"}, {"ar", "Arabic"}, {"hy", "Armenian"}, {"az", "Azerbaijani"}, {"eu", "Basque"}, {"be", "Belarusian"},
        {"bn", "Bengali"}, {"bs", "Bosnian"}, {"bg", "Bulgarian"}, {"my", "Burmese"}, {"ca", "Catalan"}, {"zh", "Chinese"}, {"hr", "Croatian"}, {"cs", "Czech"},
        {"da", "Danish"}, {"nl", "Dutch"}, {"en", "English"}, {"et", "Estonian"}, {"fil", "Filipino"}, {"fi", "Finnish"}, {"fr", "French"}, {"gl", "Galician"},
        {"ka", "Georgian"}, {"de", "German"}, {"el", "Greek"}, {"gu", "Gujarati"}, {"he", "Hebrew"}, {"iw", "Hebrew"}, {"hi", "Hindi"}, {"hu", "Hungarian"},
        {"is", "Icelandic"}, {"id", "Indonesian"}, {"ga", "Irish"}, {"it", "Italian"}, {"ja", "Japanese"}, {"kn", "Kannada"}, {"kk", "Kazakh"}, {"km", "Khmer"},
        {"ko", "Korean"}, {"ku", "Kurdish"}, {"lo", "Lao"}, {"lv", "Latvian"}, {"lt", "Lithuanian"}, {"mk", "Macedonian"}, {"ms", "Malay"}, {"ml", "Malayalam"},
        {"mr", "Marathi"}, {"mn", "Mongolian"}, {"ne", "Nepali"}, {"no", "Norwegian"}, {"nb", "Norwegian"}, {"ps", "Pashto"}, {"fa", "Persian"}, {"pl", "Polish"},
        {"pt", "Portuguese"}, {"pa", "Punjabi"}, {"ro", "Romanian"}, {"ru", "Russian"}, {"sr", "Serbian"}, {"si", "Sinhala"}, {"sk", "Slovak"}, {"sl", "Slovenian"},
        {"so", "Somali"}, {"es", "Spanish"}, {"sw", "Swahili"}, {"sv", "Swedish"}, {"ta", "Tamil"}, {"te", "Telugu"}, {"th", "Thai"}, {"tr", "Turkish"},
        {"uk", "Ukrainian"}, {"ur", "Urdu"}, {"uz", "Uzbek"}, {"vi", "Vietnamese"}, {"cy", "Welsh"}, {"zu", "Zulu"}, {"und", "Subtitles"}};
    std::string code = lower(trim(code0));
    std::replace(code.begin(), code.end(), '_', '-');
    std::string primary = code.substr(0, code.find('-'));
    auto it = names.find(primary);
    if (it == names.end()) return code0;
    return code.size() > primary.size() ? it->second + " (" + code0.substr(primary.size() + 1) + ")" : it->second;
}

std::vector<SubTrack> subsForLangs(const std::vector<SubTrack>& all, const std::string& langs0) {
    std::string langs;
    for (char c : lower(langs0)) if (isalnum(static_cast<unsigned char>(c)) || c == ',' || c == '-') langs += c;
    if (langs.empty()) langs = "en";
    bool every = langs == "all";
    std::set<std::string> want, done;
    for (auto& l : split(langs, ',')) if (!l.empty()) want.insert(l);
    std::vector<SubTrack> out;
    for (int pass = 0; pass < 2; pass++)   // people's own subtitles first; automatic ones only where there is nothing else
        for (auto& t : all) {
            if (t.automatic != (pass == 1)) continue;
            std::string l = lower(t.lang), primary = l.substr(0, l.find('-'));
            if (every ? t.automatic : (!want.count(l) && !want.count(primary))) continue;
            if (!done.insert(every ? l + "|" + lower(t.name) : primary).second) continue;
            out.push_back(t);
        }
    return out;
}

std::string packSubs(const std::vector<SubTrack>& picked) {
    std::string o;
    auto clean = [](std::string v) { for (auto& c : v) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return v; };
    for (auto& t : picked) if (!t.url.empty()) o += clean(t.lang) + "\t" + clean(t.format) + "\t" + clean(t.url) + "\t" + clean(t.name) + "\n";
    return o;
}

std::vector<SubTrack> unpackSubs(const std::string& packed) {
    std::vector<SubTrack> out;
    for (auto& line : split(packed, '\n')) {
        auto f = split(line, '\t');
        if (f.size() < 3 || !httpUrl(f[2]) || out.size() >= 300) continue;
        SubTrack t;
        t.lang = f[0]; t.format = f[1]; t.url = f[2]; if (f.size() > 3) t.name = f[3];
        out.push_back(t);
    }
    return out;
}

int saveSubtitles(const std::string& videoPath, const std::vector<SubTrack>& picked, const std::string& pageUrl,
                  const std::string& cookies, const std::string& userAgent, std::string& error) {
    std::string dir = dirName(videoPath), base = stem(baseName(videoPath));
    int made = 0;
    std::set<std::string> used;
    for (auto& t : picked) {
        if (!httpUrl(t.url)) continue;
        RequestInfo r;
        r.url = t.url; r.userAgent = userAgent; r.referer = pageUrl;
        if (streams::sameSite(t.url, pageUrl)) r.cookies = cookies;   // the browser's cookies only go back to their own site
        std::string text, e;
        if (t.format == "hls") { if (!streams::hlsText(r, t.url, text, e)) { if (error.empty()) error = e; continue; } }
        else if (http::get(r, text, nullptr, 16 * 1024 * 1024, 30) != 200) { if (error.empty()) error = "The subtitles (" + languageName(t.lang) + ") couldn't be fetched."; continue; }
        std::string srt = toSrt(text);
        if (srt.empty()) { if (error.empty()) error = "The subtitles (" + languageName(t.lang) + ") are in a format DDM can't read."; continue; }
        std::string lang = sanitizeFileName(t.lang.empty() ? "und" : t.lang);
        if (lang.size() > 24) lang = lang.substr(0, 24);
        std::string name = lang;
        for (int n = 2; used.count(lower(name)); n++) name = lang + "-" + std::to_string(n);
        used.insert(lower(name));
        if (writeFileAtomic(joinPath(dir, base + "." + name + ".srt"), "\xEF\xBB\xBF" + srt)) made++;
    }
    if (made > 0) error.clear();
    return made;
}

// The qualities offered for a video, from every stream the site gives (the helper's list, or DDM's own YouTube reader).
bool formatsFrom(std::vector<F>& fs, VideoInfo& out) {
    // best sound alone (m4a first: it joins into MP4)
    const F* bestAudio = nullptr;
    for (auto& f : fs) {
        if (f.hasV() || !f.hasA()) continue;
        auto score = [](const F& a) { return (a.ext == "m4a" ? 1e6 : 0) + (a.abr > 0 ? a.abr : a.tbr); };
        if (!bestAudio || score(f) > score(*bestAudio)) bestAudio = &f;
    }
    bool canMerge = true;   // FFmpeg is fetched before a merged download starts
    std::set<int> heights;
    for (auto& f : fs) if (f.hasV() && f.height > 0) heights.insert(f.height);
    std::vector<int> hs(heights.rbegin(), heights.rend());
    for (int h : hs) {
        const F* prog = nullptr;
        const F* vid = nullptr;
        for (auto& f : fs) {
            if (!f.hasV() || f.height != h) continue;
            auto score = [](const F& a) { return (a.ext == "mp4" ? 1e7 : 0) + (a.avc() ? 1e6 : 0) + (a.plain() ? 1e5 : 0) + a.tbr; };
            if (f.hasA()) { if (!prog || score(f) > score(*prog)) prog = &f; }
            else if (!vid || score(f) > score(*vid)) vid = &f;
        }
        VideoFormat o;
        o.group = "Video";
        o.height = h;
        o.label = std::to_string(h) + "p";
        o.tag = tagFor(h);
        int fps = 0;
        if (prog && (!vid || !bestAudio || prog->tbr >= vid->tbr * 0.8)) {
            o.selector = prog->id;
            o.ext = prog->ext.empty() ? "mp4" : prog->ext;
            o.size = prog->size; o.exact = prog->exact; fps = prog->fps;
            o.direct = prog->plain() && !prog->url.empty();
            if (o.direct) { o.directUrl = prog->url; o.headers = prog->headers; }
        } else if (vid && bestAudio && canMerge) {
            o.selector = vid->id + "+" + bestAudio->id;
            bool mp4 = (vid->ext == "mp4" && bestAudio->ext == "m4a");
            o.ext = mp4 ? "mp4" : (vid->ext == "webm" && bestAudio->ext == "webm" ? "webm" : "mkv");
            if (vid->avc() || mp4) o.ext = "mp4";
            o.size = (vid->size > 0 && bestAudio->size > 0) ? vid->size + bestAudio->size : -1;
            o.exact = vid->exact && bestAudio->exact; fps = vid->fps;
            if (vid->plain() && bestAudio->plain() && !vid->url.empty() && !bestAudio->url.empty()) {
                o.videoUrl = vid->url; o.audioUrl = bestAudio->url; o.videoExt = vid->ext; o.audioExt = bestAudio->ext; o.headers = vid->headers;
            }
        } else continue;
        std::string e = o.ext;
        for (auto& c : e) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        o.spec = e + " · video + sound" + (fps >= 48 ? " · " + std::to_string(fps) + "fps" : "");
        if (std::none_of(out.formats.begin(), out.formats.end(), [&](const VideoFormat& x) { return x.label == o.label; })) out.formats.push_back(o);
    }
    // a site that gives one file without sizes or codecs (some players): one "Best" choice
    if (out.formats.empty()) {
        for (auto& f : fs) if (f.hasV() || (!f.hasA() && !f.url.empty())) {
            VideoFormat o; o.group = "Video"; o.label = "Best"; o.tag = "HD"; o.selector = "best"; o.ext = f.ext.empty() ? "mp4" : f.ext; o.size = f.size;
            o.spec = "video + sound"; o.direct = f.plain() && !f.url.empty(); if (o.direct) { o.directUrl = f.url; o.headers = f.headers; }
            out.formats.push_back(o);
            break;
        }
    }
    if (bestAudio) {
        VideoFormat mp3;
        mp3.group = "Audio"; mp3.label = "MP3"; mp3.tag = "Audio"; mp3.selector = bestAudio->id; mp3.convert = "mp3"; mp3.ext = "mp3";
        mp3.size = bestAudio->size; mp3.spec = "sound only · converted on your PC";
        if (bestAudio->plain() && !bestAudio->url.empty()) { mp3.audioUrl = bestAudio->url; mp3.audioExt = bestAudio->ext; mp3.headers = bestAudio->headers; }
        out.formats.push_back(mp3);
        VideoFormat orig;
        orig.group = "Audio"; orig.label = upperExt(bestAudio->ext); orig.tag = "Audio"; orig.selector = bestAudio->id; orig.ext = bestAudio->ext;
        orig.size = bestAudio->size; orig.exact = bestAudio->exact; orig.spec = "sound only · original quality";
        orig.direct = bestAudio->plain() && !bestAudio->url.empty();
        if (orig.direct) { orig.directUrl = bestAudio->url; orig.headers = bestAudio->headers; }
        out.formats.push_back(orig);
    }
    if (out.formats.empty()) { out.error = "No downloadable video was found on this page."; return false; }
    return true;
}


namespace {
void hlsFormats(const std::string& url, const RequestInfo& base, const std::map<std::string, std::string>& headers, VideoInfo& out, std::vector<VideoFormat>& add, std::string& err);
}

// YouTube with DDM's own reader.
bool probeYouTube(const VideoRequest& r, VideoInfo& out, std::function<void(const std::string&)> status) {
    youtube::Video v;
    if (!youtube::read(r.url, r.cookies, v, status)) { out.error = v.error.empty() ? "YouTube couldn't be read. Try again in a moment." : v.error; logLine("youtube: " + v.error); return false; }
    out.videoId = v.id;
    out.uploader = v.author;
    out.description = v.description;
    out.rawTitle = v.title;
    out.title = niceVideoTitle(v.title, v.description, v.author, "", v.id);
    out.thumbnail = v.thumbnail;
    out.duration = v.duration;
    out.extractor = "Youtube";
    out.notBefore = v.notBefore;
    for (int pass = 0; pass < 2; pass++)   // the uploader's own subtitles first, then YouTube's automatic ones
        for (auto& c : v.captions) {
            if (c.automatic != (pass == 1) || c.url.empty()) continue;
            SubTrack t;
            t.lang = c.lang; t.name = c.name; t.url = c.url + "&fmt=vtt"; t.format = "vtt"; t.automatic = c.automatic;
            out.subs.push_back(t);
        }
    bool anyDefault = false;
    for (auto& s : v.streams) if (s.vcodec == "none" && !s.audioLang.empty() && s.defaultAudio) anyDefault = true;
    if (v.streams.empty() && !v.hls.empty()) {   // a live stream (or a video YouTube gives only as a stream)
        std::map<std::string, std::string> h;
        h["User-Agent"] = v.userAgent;
        h["Referer"] = "https://www.youtube.com/";
        RequestInfo base;
        base.userAgent = v.userAgent;
        std::string err;
        std::vector<VideoFormat> list;
        hlsFormats(v.hls, base, h, out, list, err);
        for (auto& f : list) if (v.live) f.spec += " · live: recorded until you stop it";
        out.formats = list;
        if (out.formats.empty()) { out.error = err.empty() ? "YouTube's stream couldn't be read." : err; return false; }
        return true;
    }
    std::vector<F> fs;
    for (auto& s : v.streams) {
        if (anyDefault && s.vcodec == "none" && !s.audioLang.empty() && !s.defaultAudio) continue;   // other languages' sound tracks
        F x;
        x.id = std::to_string(s.itag); x.ext = s.ext; x.vcodec = s.vcodec; x.acodec = s.acodec; x.proto = "https"; x.url = s.url;
        x.height = s.height; x.fps = s.fps; x.tbr = s.kbps; x.abr = s.vcodec == "none" ? s.kbps : 0;
        if (s.size > 0) { x.size = s.size; x.exact = true; } else if (s.kbps > 0 && v.duration > 0) x.size = static_cast<int64_t>(s.kbps * 125.0 * v.duration);
        x.headers["User-Agent"] = v.userAgent;
        fs.push_back(x);
    }
    return formatsFrom(fs, out);
}

namespace {

// ------------------------------------------------------------------ every other site: DDM's own readers
std::string fpsText(double fps) { return fps >= 48 ? " · " + std::to_string(static_cast<int>(fps + 0.5)) + "fps" : ""; }

RequestInfo baseRequest(const std::string& cookies, const std::string& userAgent, const std::string& referer, const std::map<std::string, std::string>& headers) {
    RequestInfo b;
    b.cookies = cookies; b.userAgent = userAgent; b.referer = referer;
    for (auto& kv : headers) {
        std::string k = lower(kv.first);
        if (k == "referer") { if (b.referer.empty()) b.referer = kv.second; continue; }
        if (k == "user-agent") { if (b.userAgent.empty()) b.userAgent = kv.second; continue; }
        if (k == "cookie") { b.cookies = b.cookies.empty() ? kv.second : b.cookies + "; " + kv.second; continue; }
        if (k == "range" || k == "host" || k == "content-length") continue;
        b.headers[kv.first] = kv.second;
    }
    return b;
}

void addPlain(std::vector<F>& fs, const sites::Source& s, const std::map<std::string, std::string>& headers, int duration) {
    F x;
    x.url = s.url;
    x.proto = startsWith(lower(s.url), "https") ? "https" : "http";
    x.ext = lowerExt(s.ext.empty() ? extOf(fileNameFromUrl(s.url)) : s.ext);
    if (x.ext.empty() || x.ext.size() > 4) x.ext = s.kind == "audio" ? "m4a" : "mp4";
    x.height = s.height; x.size = s.size; x.exact = s.size > 0; x.tbr = s.kbps;
    if (s.kind == "audio" || (s.kind == "file" && audioExt(x.ext))) { x.vcodec = "none"; x.acodec = x.ext == "webm" ? "opus" : "mp4a"; x.abr = s.kbps; if (x.ext == "mp4") x.ext = "m4a"; }
    else if (s.kind == "video") { x.vcodec = x.ext == "webm" ? "vp9" : "avc1"; x.acodec = "none"; }
    else { x.vcodec = x.ext == "webm" ? "vp9" : "avc1"; x.acodec = "unknown"; }   // one file with picture and sound
    if (x.size < 0 && x.tbr > 0 && duration > 0) x.size = static_cast<int64_t>(x.tbr * 125.0 * duration);
    x.id = s.kind + std::to_string(fs.size());
    x.headers = headers;
    for (auto& f : fs) if (f.url == x.url) return;
    fs.push_back(x);
}

VideoFormat streamFormat(int height, int64_t bandwidth, double fps, int duration, const std::string& url, const std::string& selector,
                         const std::map<std::string, std::string>& headers, const std::string& kind, int width = 0) {
    VideoFormat o;
    o.group = "Video";
    o.height = height;
    if (width > 0 && width < height) height = width;   // an upright video is named by its short side, like everywhere (1080 x 1920 is 1080p)
    o.label = height > 0 ? std::to_string(height) + "p" : bandwidth > 0 ? std::to_string(static_cast<int>(bandwidth / 1000)) + " kbps" : "Best";
    o.tag = height > 0 ? tagFor(height) : "";
    o.ext = "mp4";
    o.selector = selector;
    o.streamUrl = url;
    o.headers = headers;
    o.size = bandwidth > 0 && duration > 0 ? static_cast<int64_t>(bandwidth / 8.0 * duration) : -1;
    o.spec = "MP4 · video + sound · " + kind + fpsText(fps);
    return o;
}

VideoFormat streamMp3(const std::string& url, const std::string& selector, const std::map<std::string, std::string>& headers, int64_t audioBw, int duration) {
    VideoFormat a;
    a.group = "Audio"; a.label = "MP3"; a.tag = "Audio"; a.ext = "mp3"; a.convert = "mp3";
    a.selector = selector; a.streamUrl = url; a.headers = headers;
    a.size = duration > 0 ? static_cast<int64_t>((audioBw > 0 ? audioBw : 192000) / 8.0 * duration) : -1;
    a.spec = "sound only · converted on your PC";
    return a;
}

bool isAudioOnly(const streams::Variant& v) {
    std::string c = lower(v.codecs);
    return v.height == 0 && v.width == 0 && !c.empty() && c.find("avc") == std::string::npos && c.find("hvc") == std::string::npos && c.find("hev") == std::string::npos &&
           c.find("vp0") == std::string::npos && c.find("vp9") == std::string::npos && c.find("av01") == std::string::npos;
}

// an HLS stream: every quality it lists
void hlsFormats(const std::string& url, const RequestInfo& base, const std::map<std::string, std::string>& headers, VideoInfo& out, std::vector<VideoFormat>& add, std::string& err) {
    streams::Master m;
    RequestInfo r = base;
    r.url = url;
    if (!streams::readHls(r, m, err)) return;
    if (out.duration == 0 && m.duration > 0) out.duration = static_cast<int>(m.duration);
    for (auto& sd : m.subs) {
        SubTrack t;
        t.lang = sd.lang.empty() ? (sd.name.empty() ? "und" : sd.name) : sd.lang; t.name = sd.name; t.url = sd.url; t.format = "hls";
        out.subs.push_back(t);
    }
    int64_t audioBw = 0;
    bool any = false;
    for (auto& v : m.variants) {
        if (isAudioOnly(v)) { audioBw = std::max(audioBw, v.bandwidth); continue; }
        add.push_back(streamFormat(v.height, v.bandwidth, v.fps, out.duration, url, "hls:" + std::to_string(v.height) + ":" + std::to_string(v.bandwidth), headers, "stream", v.width));
        any = true;
    }
    if (any || audioBw > 0) add.push_back(streamMp3(url, "hls:audio", headers, audioBw, out.duration));
}

// a DASH stream: qualities in one file go to DDM's normal engine, pieced ones to the stream engine
void dashFormats(const std::string& url, const RequestInfo& base, const std::map<std::string, std::string>& headers, VideoInfo& out, std::vector<F>& fs,
                 std::vector<VideoFormat>& add, std::string& err) {
    streams::Manifest m;
    RequestInfo r = base;
    r.url = url;
    if (!streams::readDash(r, m, err)) return;
    if (m.live) { err = "This is a live stream that only plays in the site's player."; }
    if (out.duration == 0 && m.duration > 0) out.duration = static_cast<int>(m.duration);
    for (auto& tx : m.texts) {
        SubTrack t;
        t.lang = tx.lang.empty() ? "und" : tx.lang; t.url = tx.singleUrl; t.format = "vtt";
        out.subs.push_back(t);
    }
    const streams::Representation* bestA = nullptr;
    for (auto& rep : m.reps) {
        if (!rep.audio || rep.video) continue;
        auto score = [](const streams::Representation& x) { return (startsWith(lower(x.codecs), "mp4a") ? 1e9 : 0) + static_cast<double>(x.bandwidth); };
        if (!bestA || score(rep) > score(*bestA)) bestA = &rep;
    }
    for (auto& rep : m.reps) {
        if (!rep.singleUrl.empty()) {   // one file: full speed, many connections
            sites::Source s;
            s.url = rep.singleUrl; s.kind = rep.video && rep.audio ? "file" : rep.video ? "video" : "audio";
            s.height = rep.height; s.kbps = rep.bandwidth / 1000.0;
            s.ext = rep.video ? (lower(rep.mime).find("webm") != std::string::npos ? "webm" : "mp4") : (lower(rep.mime).find("webm") != std::string::npos ? "webm" : "m4a");
            addPlain(fs, s, headers, out.duration);
            if (rep.video) { fs.back().fps = static_cast<int>(rep.fps); fs.back().vcodec = lower(rep.codecs).empty() ? fs.back().vcodec : lower(rep.codecs); }
            continue;
        }
        if (!rep.video || rep.segments.empty()) continue;
        std::string sel = "dash:" + rep.id + "+" + (bestA && bestA->singleUrl.empty() ? bestA->id : "");
        add.push_back(streamFormat(rep.height, rep.bandwidth + (bestA ? bestA->bandwidth : 0), rep.fps, out.duration, url, sel, headers, "stream"));
    }
    if (bestA && bestA->singleUrl.empty() && !bestA->segments.empty()) add.push_back(streamMp3(url, "dash:audio", headers, bestA->bandwidth, out.duration));
}

// the qualities from a page's sources
bool formatsFromPage(const sites::Page& pg, const VideoRequest& r, VideoInfo& out, std::function<void(const std::string&)> status) {
    std::map<std::string, std::string> headers = pg.headers;
    if (!headers.count("Referer") && !r.url.empty()) headers["Referer"] = r.url;
    RequestInfo base = baseRequest(r.cookies, r.userAgent, "", headers);
    std::vector<F> fs;
    std::vector<VideoFormat> extra;
    std::string err;
    int streamReads = 0;
    for (auto& x : pg.subs) addSubTrack(out.subs, x);
    // streams read in parallel (each needs a request or two): the list is ready as soon as the slowest answers
    std::vector<std::thread> th;
    std::mutex mu;
    for (auto& s : pg.sources) {
        if (s.kind == "hls" || s.kind == "dash") {
            if (++streamReads > 4) continue;
            th.emplace_back([&, s] {
                VideoInfo local;
                local.duration = out.duration;
                std::vector<F> lf;
                std::vector<VideoFormat> la;
                std::string e;
                if (s.kind == "hls") hlsFormats(s.url, base, headers, local, la, e); else dashFormats(s.url, base, headers, local, lf, la, e);
                std::lock_guard<std::mutex> g(mu);
                if (out.duration == 0) out.duration = local.duration;
                for (auto& x : local.subs) addSubTrack(out.subs, x);
                for (auto& x : lf) { bool dup = false; for (auto& y : fs) if (y.url == x.url) dup = true; if (!dup) { x.id = "d" + std::to_string(fs.size()); fs.push_back(x); } }
                for (auto& x : la) extra.push_back(x);
                if (err.empty()) err = e;
            });
        } else addPlain(fs, s, headers, pg.duration);
    }
    for (auto& t : th) t.join();
    VideoInfo tmp = out;
    tmp.formats.clear();
    formatsFrom(fs, tmp);
    std::vector<VideoFormat> list = tmp.formats;
    // files without a known height: each one offered as it is
    int n = 0;
    for (auto& f : fs) {
        if (f.height > 0 || !f.hasV() || !f.hasA()) continue;
        bool there = false;
        for (auto& o : list) if (o.directUrl == f.url) there = true;
        if (there) continue;
        VideoFormat o;
        o.group = "Video"; o.label = n == 0 ? "Original" : "Video " + std::to_string(n + 1); o.tag = ""; o.ext = f.ext; o.size = f.size; o.exact = f.exact;
        o.direct = true; o.directUrl = f.url; o.headers = f.headers; o.selector = "";
        std::string e = f.ext; for (auto& c : e) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        o.spec = e + " · video + sound";
        list.erase(std::remove_if(list.begin(), list.end(), [](const VideoFormat& x) { return x.label == "Best" && x.selector == "best"; }), list.end());
        list.push_back(o);
        n++;
    }
    // streams: qualities the plain files don't already give
    for (auto& x : extra) {
        bool have = false;
        for (auto& o : list) if (o.group == x.group && o.label == x.label) have = true;
        if (!have) list.push_back(x);
    }
    // sound only, from the smallest file with sound, when the site gives no separate sound
    bool hasAudio = std::any_of(list.begin(), list.end(), [](const VideoFormat& x) { return x.group == "Audio"; });
    if (!hasAudio) {
        const F* pick = nullptr;
        for (auto& f : fs) if (f.hasA() && f.hasV() && (!pick || (f.height > 0 && (pick->height == 0 || f.height < pick->height)))) pick = &f;
        if (pick) {
            VideoFormat mp3;
            mp3.group = "Audio"; mp3.label = "MP3"; mp3.tag = "Audio"; mp3.convert = "mp3"; mp3.ext = "mp3";
            mp3.audioUrl = pick->url; mp3.audioExt = pick->ext; mp3.headers = pick->headers;
            mp3.size = out.duration > 0 ? static_cast<int64_t>(24000.0 * out.duration) : -1;
            mp3.spec = "sound only · converted on your PC";
            list.push_back(mp3);
        }
    }
    std::stable_sort(list.begin(), list.end(), [](const VideoFormat& a, const VideoFormat& b) {
        if ((a.group == "Audio") != (b.group == "Audio")) return a.group != "Audio";
        return a.height > b.height;
    });
    out.formats = list;
    if (out.formats.empty()) { out.error = !err.empty() ? err : !pg.error.empty() ? pg.error : "No downloadable video was found on this page."; return false; }
    out.error.clear();
    return true;
}

bool probeNative(const VideoRequest& r, VideoInfo& out, std::function<void(const std::string&)> status) {
    std::string u = trim(r.url);
    if (!httpUrl(u)) { out.error = "This isn't a web address."; return false; }
    sites::Page pg;
    pg.site = urlSite(u);
    if (r.forceKind == "hls" || r.forceKind == "dash" || streams::looksHls(u) || streams::looksDash(u)) {   // the stream itself
        sites::Source s; s.url = u; s.kind = !r.forceKind.empty() ? r.forceKind : streams::looksHls(u) ? "hls" : "dash";
        pg.sources.push_back(s);
        if (!r.referer.empty()) pg.headers["Referer"] = r.referer;
        for (auto& kv : r.headers) pg.headers[kv.first] = kv.second;   // … asked for the way the browser asked
        out.rawTitle = stem(fileNameFromUrl(u));
    } else if (sites::isFileUrl(u)) {                    // a video file
        sites::Source s; s.url = u; s.kind = "file";
        pg.sources.push_back(s);
        if (!r.referer.empty()) pg.headers["Referer"] = r.referer;
        for (auto& kv : r.headers) pg.headers[kv.first] = kv.second;
        out.rawTitle = stem(fileNameFromUrl(u));
    } else {
        if (status) status("Reading the page…");
        // TikTok: what the browser itself read about this post (its servers often hide it from programs)
        bool fromBrowser = !r.pageData.empty() && sites::fromBrowser(u, r.pageData, r.cookies, pg);
        if (!fromBrowser && !sites::read(u, r.cookies, r.userAgent, r.referer, pg)) { out.error = pg.error; out.rawTitle = pg.title; out.thumbnail = pg.thumbnail; out.videoId = pg.id; out.uploader = pg.author; return false; }
        out.rawTitle = pg.title;
        for (auto& s : pg.sources) if (s.kind == "youtube") {   // a YouTube video shown in the page
            VideoRequest yr = r;
            yr.url = s.url;
            VideoInfo yi;
            yi.pageUrl = s.url;
            if (probeYouTube(yr, yi, status)) { out = yi; return true; }
            out.error = yi.error;
            return false;
        }
    }
    out.uploader = pg.author;
    out.videoId = pg.id;
    out.description = pg.description.substr(0, 2000);
    out.thumbnail = pg.thumbnail;
    out.duration = pg.duration;
    out.extractor = pg.site.empty() ? "Web" : pg.site;
    out.title = niceVideoTitle(out.rawTitle, out.description, out.uploader, "", out.videoId);
    if (status) status("Getting the video's qualities…");
    return formatsFromPage(pg, r, out, status);
}

} // namespace

namespace {
// "Holiday in Rome - TubeSite" on tubesite.com -> "Holiday in Rome" (only when what follows the last separator is the site's own name)
std::string dropSiteName(const std::string& title, const std::string& urlA, const std::string& urlB) {
    auto letters = [](const std::string& s) { std::string o; for (unsigned char c : lower(s)) if (isalnum(c)) o += static_cast<char>(c); return o; };
    std::string t = trim(title);
    for (int round = 0; round < 2; round++) {
        size_t cut = std::string::npos, len = 0;
        for (const char* sep : {" - ", " | ", " \xE2\x80\x93 ", " \xE2\x80\x94 ", " \xC2\xB7 ", " :: "}) {
            size_t at = t.rfind(sep);
            if (at != std::string::npos && at > 0 && (cut == std::string::npos || at > cut)) { cut = at; len = strlen(sep); }
        }
        if (cut == std::string::npos) break;
        std::string tail = letters(t.substr(cut + len));
        if (tail.size() < 3 || tail.size() > 80) break;
        bool site = false;
        for (const std::string* u : {&urlA, &urlB}) {
            std::string host = urlSite(*u);                       // "tubesite.com"
            std::string name = letters(host.substr(0, host.find('.')));   // "tubesite"
            std::string whole = letters(host);                    // "tubesitecom"
            if (name.size() >= 3 && (tail == name || tail == whole || (tail.size() >= 5 && name.find(tail) != std::string::npos) || (name.size() >= 4 && tail.find(name) != std::string::npos))) site = true;
        }
        if (!site || trim(t.substr(0, cut)).size() < 3) break;
        t = trim(t.substr(0, cut));
    }
    return t;
}
} // namespace

bool isFeedSite(const std::string& url) {
    std::string s = urlSite(url);
    for (const char* d : {"tiktok.com", "instagram.com", "facebook.com", "fb.watch", "x.com", "twitter.com", "snapchat.com", "pinterest.com", "likee.video", "kwai.com", "triller.co"})
        if (s == d || endsWith(s, std::string(".") + d)) return true;
    return false;
}

std::string cleanVideoName(const std::string& title, const std::string& url, const std::string& topUrl) { return dropSiteName(title, url, topUrl); }

std::string videoPageFor(const std::string& url, const std::string& topUrl, const std::vector<SeenStream>& seen) {
    auto onDm = [](const std::string& u) { std::string s = urlSite(u); return s == "dai.ly" || s.find("dailymotion.") != std::string::npos; };
    bool dmStream = false;
    for (auto& x : seen) if (x.url.find("/manifest/video/") != std::string::npos && (onDm(x.url) || x.url.find("dmcdn.net") != std::string::npos)) dmStream = true;
    if (onDm(url) || onDm(topUrl) || dmStream) {
        // Dailymotion's player keeps one address while it plays video after video: the tab's address names the
        // video on dailymotion.com itself; inside other sites, the stream the player loaded last does.
        static const std::regex page("(?:dailymotion\\.[a-z.]{2,6}/(?:embed/)?video/|dai\\.ly/)([A-Za-z0-9]+)");
        static const std::regex list("/manifest/video/([A-Za-z0-9]+)\\.m3u8");
        static const std::regex par("[?&]video=([A-Za-z0-9]+)");
        std::smatch m;
        std::string id;
        if (onDm(topUrl) && std::regex_search(topUrl, m, page)) id = m[1];
        if (id.empty()) for (auto it = seen.rbegin(); it != seen.rend(); ++it) if (std::regex_search(it->url, m, list)) { id = m[1]; break; }
        if (id.empty() && std::regex_search(url, m, page)) id = m[1];
        if (id.empty() && std::regex_search(url, m, par)) id = m[1];
        if (!id.empty()) return "https://www.dailymotion.com/video/" + id;
        return url;
    }
    // a known video site's page with its player in a frame from elsewhere: the page is the video's address
    if (!topUrl.empty() && topUrl != url && httpUrl(topUrl) && isVideoSite(topUrl) && !isVideoSite(url)) return topUrl;
    return url;
}

bool probeVideo(const VideoRequest& r, VideoInfo& out, std::function<void(const std::string&)> status) {
    out = VideoInfo();
    out.pageUrl = r.url;
    int64_t t0 = nowMs();
    bool yt = isYouTube(r.url) && !youtube::videoId(r.url).empty();
    bool ok = yt ? probeYouTube(r, out, status) : probeNative(r, out, status);
    if (!ok && siterules::refresh(1)) {   // a site may have changed: DedSec's newest rules, then once more
        std::string why = out.error;
        VideoInfo again;
        again.pageUrl = r.url;
        if (yt ? probeYouTube(r, again, status) : probeNative(r, again, status)) { out = again; ok = true; } else out.error = why;
    }
    // a player inside another site's page: its own page is called "Player" or "Embed"; the page around it names the video
    if (ok && r.framed && !trim(r.title).empty() && !isVideoSite(r.url) && !yt) {
        out.rawTitle = trim(r.title);
        out.title = niceVideoTitle(out.rawTitle, out.description, out.uploader, "", out.videoId);
    }
    // A feed (reels, a "for you" page): the address may still name the video before this one. When what the page
    // gave isn't as long as the video playing in the browser, what the browser plays is taken instead.
    VideoInfo fromPage;
    bool hadPage = false;
    if (ok && !yt && isFeedSite(r.url) && r.duration > 0 && out.duration > 0 && std::abs(out.duration - r.duration) > std::max(3, r.duration / 20)
        && (httpUrl(r.direct) || !r.seen.empty())) {
        fromPage = out; hadPage = true; ok = false;
        out = VideoInfo(); out.pageUrl = r.url;
    }
    if (!ok && httpUrl(r.direct) && !streams::looksHls(r.direct) && !streams::looksDash(r.direct)) {   // the file the player is playing
        sites::Page pg;
        pg.site = urlSite(r.url);
        sites::Source s; s.url = r.direct; s.kind = "file"; s.height = r.height;
        pg.sources.push_back(s);
        pg.headers["Referer"] = r.url;
        for (auto& x : r.seen) if (x.url == r.direct) for (auto& kv : x.headers) pg.headers[kv.first] = kv.second;
        VideoInfo v;
        v.pageUrl = r.url;
        std::string name = !trim(r.title).empty() ? trim(r.title) : out.rawTitle;
        v.rawTitle = name; v.title = niceVideoTitle(name, "", "", "", "");
        v.duration = r.duration; v.thumbnail = hadPage ? "" : out.thumbnail; v.extractor = "browser";
        if (formatsFromPage(pg, r, v, status)) { out = v; ok = true; }
    }
    if (!ok) {   // nothing on the page itself, but the browser plays it: what the browser loaded is read instead
        std::string why = out.error;
        std::vector<SeenStream> seen = r.seen;
        if (seen.empty()) for (auto& s : r.streams) { SeenStream x; x.url = s; seen.push_back(x); }   // (an older add-on: addresses only)
        for (auto& x : seen) if (x.kind.empty()) x.kind = streams::looksHls(x.url) ? "hls" : streams::looksDash(x.url) ? "dash" : "";
        auto adopt = [&](VideoInfo& v) {
            v.pageUrl = r.url;
            // the video's name: what the browser shows for it, then the page's own title — never the stream's file name
            std::string name = !trim(r.title).empty() ? trim(r.title) : !out.rawTitle.empty() ? out.rawTitle : v.rawTitle;
            v.rawTitle = name;
            v.title = niceVideoTitle(name, "", out.uploader, out.rawTitle, "");
            if (!out.thumbnail.empty()) v.thumbnail = out.thumbnail;
            if (v.uploader.empty()) v.uploader = out.uploader;
            out = v;
            ok = true;
        };
        // 1. streams (HLS / DASH): the newest six are read together; the one with the most qualities wins (a site's
        //    main list has them all, the lists of single qualities it also loads have one each)
        std::vector<const SeenStream*> lists;
        for (auto it = seen.rbegin(); it != seen.rend() && lists.size() < 6; ++it) {
            if (!httpUrl(it->url) || (it->kind != "hls" && it->kind != "dash")) continue;
            bool dup = false;
            for (auto* o : lists) if (o->url == it->url) dup = true;
            if (!dup) lists.push_back(&*it);
        }
        if (!lists.empty()) {
            std::vector<VideoInfo> got(lists.size());
            std::vector<char> fine(lists.size(), 0);
            std::vector<std::thread> th;
            for (size_t i = 0; i < lists.size(); i++) th.emplace_back([&, i] {
                VideoRequest sr = r;
                sr.url = lists[i]->url; sr.referer = r.url; sr.forceKind = lists[i]->kind; sr.seen.clear(); sr.streams.clear(); sr.pageData.clear();
                sr.headers = lists[i]->headers;
                got[i].pageUrl = r.url;
                fine[i] = probeNative(sr, got[i], nullptr) ? 1 : 0;
            });
            for (auto& t : th) t.join();
            // which one is the video? The one as long as the video playing in the browser (an advert's stream and
            // the next video's are not); among those, the one with the most qualities, then the longest.
            int best = -1;
            size_t most = 0;
            bool bestFits = false;
            for (size_t i = 0; i < lists.size(); i++) {
                if (!fine[i]) { if (why.empty() || why.find("No video") == 0) { if (got[i].error.find("DRM") != std::string::npos) why = got[i].error; } continue; }
                size_t n = 0;
                for (auto& f : got[i].formats) if (f.group == "Video") n++;
                int d = got[i].duration;
                bool fits = r.duration > 0 && d > 0 && std::abs(d - r.duration) <= std::max(3, r.duration / 20);
                int bd = best < 0 ? 0 : got[static_cast<size_t>(best)].duration;
                bool longer = d > bd + std::max(3, bd / 20), shorter = bd > d + std::max(3, d / 20);   // (an advert is short)
                bool better = best < 0 || (fits != bestFits ? fits : longer ? true : shorter ? false : n > most);
                if (better) { best = static_cast<int>(i); most = n; bestFits = fits; }
            }
            if (best >= 0) adopt(got[static_cast<size_t>(best)]);
        }
        // 2. plain files the player loaded: one file with picture and sound, or (players that load them apart) the
        //    biggest picture file joined with the biggest sound file
        if (!ok) {
            auto isSound = [](const SeenStream& x) { return startsWith(x.mime, "audio/"); };
            const SeenStream *vid = nullptr, *snd = nullptr;
            bool feed = isFeedSite(r.url);   // reels: the file loaded last is the one playing, not the biggest of them all
            for (auto& x : seen) {
                if (x.kind != "file" || !httpUrl(x.url)) continue;
                if (isSound(x)) { if (!snd || feed || x.size > snd->size) snd = &x; }
                else if (!vid || feed || x.size > vid->size) vid = &x;
            }
            if (vid || snd) {
                sites::Page pg;
                pg.site = urlSite(r.url);
                pg.title = !trim(r.title).empty() ? trim(r.title) : out.rawTitle;   // the name the browser shows for it
                pg.thumbnail = out.thumbnail;
                pg.headers["Referer"] = r.url;
                if (vid) for (auto& kv : vid->headers) pg.headers[kv.first] = kv.second;   // asked for the way the browser asked
                bool apart = vid && snd && startsWith(vid->mime, "video/");   // picture and sound came as two files
                if (vid) {
                    sites::Source s;
                    s.url = vid->url; s.kind = apart ? "video" : "file"; s.size = vid->size; s.height = apart ? (r.height > 0 ? r.height : 720) : r.height;
                    std::string me = vid->mime.find("webm") != std::string::npos ? "webm" : "mp4";
                    s.ext = me;
                    pg.sources.push_back(s);
                }
                if (snd && (apart || !vid)) {
                    sites::Source s;
                    s.url = snd->url; s.kind = "audio"; s.size = snd->size;
                    s.ext = snd->mime.find("webm") != std::string::npos ? "webm" : snd->mime.find("mpeg") != std::string::npos ? "mp3" : "m4a";
                    pg.sources.push_back(s);
                }
                VideoInfo v;
                v.pageUrl = r.url;
                v.rawTitle = pg.title;
                v.extractor = pg.site.empty() ? "Web" : pg.site;
                v.title = niceVideoTitle(v.rawTitle, "", "", "", "");
                if (formatsFromPage(pg, r, v, nullptr)) adopt(v);
            }
        }
        if (!ok && hadPage) { out = fromPage; ok = true; }   // nothing better than what the page gave
        if (!ok) out.error = why.empty() ? "No video was found on this page." : why;
    }
    // a site DDM has no rule for: its name at the end of the title ("… - SiteName", "… | sitename.com") isn't the video's name
    if (ok && !yt && !isVideoSite(r.url)) {
        std::string t = dropSiteName(out.title, r.url, r.topUrl);
        if (t != out.title) { out.title = t; out.rawTitle = dropSiteName(out.rawTitle, r.url, r.topUrl); }
    }
    logLine("video: read " + urlSite(r.url) + " in " + std::to_string(nowMs() - t0) + " ms, " + std::to_string(out.formats.size()) + " qualities" + (ok ? "" : " (" + out.error + ")"));
    return ok;
}

namespace {

std::string pageOf(const Task& t) { return !t.pageUrl.empty() && httpUrl(t.pageUrl) ? t.pageUrl : t.url; }

// The same quality read again from the video's page (fresh addresses: they expire on many sites).
bool findFormat(const Task& t, VideoFormat& out, VideoInfo* infoOut = nullptr) {
    std::string page = pageOf(t);
    if (!httpUrl(page)) return false;
    VideoRequest vr;
    vr.url = page; vr.cookies = t.cookies; vr.userAgent = t.userAgent;
    auto ref = t.headers.find("Referer");
    if (ref != t.headers.end() && ref->second != page) vr.referer = ref->second;
    VideoInfo vi;
    if (!probeVideo(vr, vi)) return false;
    if (infoOut) *infoOut = vi;
    const VideoFormat* pick = nullptr;
    for (auto& f : vi.formats) if (f.convert == t.convertTo && !t.formatLabel.empty() && f.label == t.formatLabel) { pick = &f; break; }
    if (!pick) for (auto& f : vi.formats) if (f.convert == t.convertTo && !t.videoFormat.empty() && f.selector == t.videoFormat) { pick = &f; break; }
    if (!pick && t.convertTo.empty()) {   // the nearest quality not above the one chosen
        int want = atoi(t.formatLabel.c_str());
        for (auto& f : vi.formats) if (f.group == "Video" && (want <= 0 || f.height <= want)) { pick = &f; break; }
        if (!pick) for (auto& f : vi.formats) if (f.group == "Video") { pick = &f; break; }
    }
    if (!pick) for (auto& f : vi.formats) if (f.convert == t.convertTo && f.group == (t.convertTo.empty() ? "Video" : "Audio")) { pick = &f; break; }
    if (!pick) return false;
    out = *pick;
    return true;
}

// Points the task at a fresh copy of its quality. Progress is kept when it's the same kind of download.
bool refreshTask(DownloadContext& ctx, std::string& error) {
    Task t;
    { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; ctx.task->status = "Reading the video's page again…"; }
    if (ctx.changed) ctx.changed();
    VideoFormat f;
    VideoInfo vi;
    if (!findFormat(t, f, &vi)) { error = vi.error.empty() ? "The video couldn't be found on its page any more." : vi.error; return false; }
    std::lock_guard<std::mutex> g(*ctx.mu);
    Task& k = *ctx.task;
    std::string was = !k.audioUrl.empty() ? "streams" : !k.mediaUrl.empty() ? "direct" : "stream";
    std::string now = !f.audioUrl.empty() ? "streams" : (f.direct && f.convert.empty()) ? "direct" : "stream";
    if (k.pageUrl.empty()) k.pageUrl = pageOf(t);
    k.videoFormat = f.selector;
    k.formatLabel = f.label;
    k.notBefore = vi.notBefore;
    for (auto& kv : f.headers) {
        if (lower(kv.first) == "cookie") { if (k.cookies.find(kv.second) == std::string::npos) k.cookies = k.cookies.empty() ? kv.second : k.cookies + "; " + kv.second; }
        else k.headers[kv.first] = kv.second;
    }
    if (now == "streams") { k.mediaUrl = f.videoUrl; k.audioUrl = f.audioUrl; k.mediaExt = f.videoExt; k.audioExt = f.audioExt; }
    else if (now == "direct") { k.mediaUrl = f.directUrl; k.audioUrl.clear(); }
    else { k.mediaUrl.clear(); k.audioUrl.clear(); k.url = f.streamUrl; }
    if (now != was) { k.partState.clear(); k.segments.clear(); k.done = 0; k.total = f.size > 0 ? f.size : -1; }
    k.error.clear();
    return true;
}

// Any site: when a file's address stops working (expired), the same quality is read again from its page and the
// download carries on where it was.
void pageRenew(DownloadContext& c, const std::string& url, const Task& main, int which, std::function<void(const std::string&)> onNewUrl) {
    if (c.renew) return;
    std::string page = pageOf(main);
    if (!httpUrl(page) || page == url) return;
    auto cur = std::make_shared<std::string>(url);
    auto renewedAt = std::make_shared<int64_t>(0);
    auto mu = std::make_shared<std::mutex>();
    Task snap = main;
    c.urlNow = [cur, mu] { std::lock_guard<std::mutex> g(*mu); return *cur; };
    c.renew = [cur, renewedAt, mu, snap, which, onNewUrl]() -> int {
        std::lock_guard<std::mutex> g(*mu);   // one connection asks; the others wait and use its answer
        if (*renewedAt && nowMs() - *renewedAt < 15000) return 2;
        VideoFormat f;
        if (!findFormat(snap, f)) return 0;
        *renewedAt = nowMs();
        std::string n = which == 1 ? f.audioUrl : (!f.videoUrl.empty() ? f.videoUrl : f.directUrl);
        if (n.empty()) return 0;
        if (n == *cur) return 2;
        *cur = n;
        if (onNewUrl) onNewUrl(n);
        logLine("video: new address from the page");
        return 1;
    };
}

// A YouTube stream (googlevideo.com) with DDM's engine: asked in 10 MB pieces (YouTube slows down bigger requests),
// every connection from the first second (its size is in its address), and a fresh address when the old one
// expires (403) — from DDM's own reader, for the same stream.
void youtubeStream(DownloadContext& c, const std::string& url, const Task& main, bool fresh, std::function<void(const std::string&)> onNewUrl) {
    std::string host = urlSite(url);
    if (host != "googlevideo.com" && !endsWith(host, ".googlevideo.com")) return;
    c.maxRequest = 10LL * 1024 * 1024;
    int64_t clen = youtube::clenOf(url);
    if (fresh && clen > 0) c.knownTotal = clen;
    std::string id = youtube::videoId(main.url);
    if (id.empty()) id = youtube::videoId(main.pageUrl);
    int itag = youtube::itagOf(url);
    if (id.empty() || itag <= 0) return;
    auto cur = std::make_shared<std::string>(url);
    auto renewedAt = std::make_shared<int64_t>(0);
    auto mu = std::make_shared<std::mutex>();
    std::string cookies = main.cookies;
    c.urlNow = [cur, mu] { std::lock_guard<std::mutex> g(*mu); return *cur; };
    c.renew = [cur, renewedAt, mu, id, itag, cookies, onNewUrl]() -> int {
        std::lock_guard<std::mutex> g(*mu);
        if (*renewedAt && nowMs() - *renewedAt < 60000) return 2;   // another connection just got a new address: use it
        std::string n = youtube::freshUrl(id, itag, cookies);
        if (n.empty()) return 0;
        *renewedAt = nowMs();
        if (n == *cur) return 2;
        *cur = n;
        if (onNewUrl) onNewUrl(n);
        logLine("youtube: new address for stream " + std::to_string(itag));
        return 1;
    };
}
void plainContext(DownloadContext& c) { c.maxRequest = 0; c.knownTotal = -1; c.urlNow = nullptr; c.renew = nullptr; }

void waitNotBefore(DownloadContext& ctx, bool& stopped) {
    stopped = false;
    for (;;) {   // YouTube serves the stream a few seconds after asking (as long as its pre-roll would have played)
        int64_t nb;
        { std::lock_guard<std::mutex> g(*ctx.mu); nb = ctx.task->notBefore; }
        int64_t now = static_cast<int64_t>(time(nullptr));
        if (nb <= 0 || now >= nb) return;
        if (ctx.stop && ctx.stop->load()) { stopped = true; return; }
        { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->status = "Starting in " + std::to_string(nb - now) + " s…"; }
        if (ctx.changed) ctx.changed();
        Sleep(250);
    }
}

// A plain single-file video: DDM's own multi-connection engine (resumes where it stopped).
DownloadResult tryDirect(DownloadContext& ctx) {
    bool st = false;
    waitNotBefore(ctx, st);
    if (st) return DownloadResult::Stopped;
    std::string page;
    {
        std::lock_guard<std::mutex> g(*ctx.mu);
        Task& k = *ctx.task;
        page = k.url;
        k.url = k.mediaUrl;
        if (k.pageUrl.empty()) k.pageUrl = page;
    }
    std::string cookies;
    {   // the page's cookies go to the file only when it's on the page's own site
        std::lock_guard<std::mutex> g(*ctx.mu);
        Task& k = *ctx.task;
        cookies = k.cookies;
        if (!streams::sameSite(k.mediaUrl, k.pageUrl)) k.cookies.clear();
    }
    Task cur;
    { std::lock_guard<std::mutex> g(*ctx.mu); cur = *ctx.task; cur.url = page; cur.cookies = cookies; }
    auto onNew = [&ctx](const std::string& n) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->mediaUrl = n; ctx.task->url = n; };
    youtubeStream(ctx, cur.mediaUrl, cur, cur.segments.empty(), onNew);
    pageRenew(ctx, cur.mediaUrl, cur, 0, onNew);
    DownloadResult r = runHttpDownload(ctx);
    plainContext(ctx);
    std::lock_guard<std::mutex> g(*ctx.mu);
    ctx.task->url = page;
    ctx.task->cookies = cookies;
    return r;
}

// One stream of a video with the HTTP engine, as its own small download (progress folded into the video's).
DownloadResult streamPart(DownloadContext& ctx, int index, const std::string& url, const std::string& ext, int64_t& base) {
    Task main;
    { std::lock_guard<std::mutex> g(*ctx.mu); main = *ctx.task; }
    Task sub;
    if (static_cast<int>(main.partState.size()) > index && !main.partState[static_cast<size_t>(index)].empty()) sub = Task::fromJson(Json::parse(main.partState[static_cast<size_t>(index)]));
    if (sub.url != url && !sub.url.empty() && sub.state != State::Complete) logLine("video stream " + std::to_string(index) + ": continues from a new address");
    sub.id = main.id + "-" + std::to_string(index);
    sub.kind = "http";
    sub.url = url;
    sub.pageUrl = main.url;
    sub.cookies = streams::sameSite(url, pageOf(main)) ? main.cookies : "";   // the page's cookies stay with the page's site
    sub.userAgent = main.userAgent;
    sub.headers = main.headers;
    {   // it downloads into DDM's parts folder, out of sight (one an older DDM started next to the video stays there)
        std::string tail = ".stream" + std::to_string(index) + "." + (ext.empty() ? "bin" : ext);
        std::string legacy = joinPath(main.dir, stem(main.fileName) + tail);
        bool old = !main.dir.empty() && (fileExists(legacy) || fileExists(legacy + ".ddmpart"));
        if (!old) makeDirs(partsFolder());
        sub.dir = old ? main.dir : partsFolder();
        sub.fileName = old ? stem(main.fileName) + tail : (main.id.empty() ? std::string("video") : main.id) + tail;
    }
    sub.connections = main.connections;
    sub.limitKBps = main.limitKBps;
    if (sub.state == State::Complete && fileExists(sub.path())) { base += std::max<int64_t>(0, sub.total); return DownloadResult::Done; }
    std::mutex subMu;
    DownloadContext sc;
    sc.task = &sub;
    sc.mu = &subMu;
    sc.stop = ctx.stop;
    auto onNew = [&](const std::string& n) {
        { std::lock_guard<std::mutex> g(subMu); sub.url = n; }
        std::lock_guard<std::mutex> g(*ctx.mu);
        if (index == 0) ctx.task->mediaUrl = n; else ctx.task->audioUrl = n;   // a later resume starts from the new address
    };
    youtubeStream(sc, url, main, sub.segments.empty(), onNew);
    pageRenew(sc, url, main, index, onNew);
    int64_t b0 = base;
    sc.changed = [&] {
        Task s;
        { std::lock_guard<std::mutex> g(subMu); s = sub; }
        {
            std::lock_guard<std::mutex> g(*ctx.mu);
            Task& k = *ctx.task;
            k.done = b0 + s.done;
            k.speed = s.speed;
            k.activeConnections = s.activeConnections;
            if (k.total > 0 && k.total < k.done) k.total = -1;
            k.eta = s.speed > 1 && k.total > 0 && k.total > k.done ? static_cast<int64_t>((k.total - k.done) / s.speed) : -1;
            k.status = s.done > 0 ? (index == 0 ? "Receiving data…" : "Receiving the sound…") : "Connecting…";
            k.resumable = true;
            if (static_cast<int>(k.partState.size()) <= index) k.partState.resize(static_cast<size_t>(index) + 1);
            k.partState[static_cast<size_t>(index)] = s.toJson().dump();
        }
        if (ctx.changed) ctx.changed();
    };
    DownloadResult r = runHttpDownload(sc);
    {
        std::lock_guard<std::mutex> g(*ctx.mu);
        Task& k = *ctx.task;
        if (static_cast<int>(k.partState.size()) <= index) k.partState.resize(static_cast<size_t>(index) + 1);
        if (r == DownloadResult::Done) sub.state = State::Complete;
        k.partState[static_cast<size_t>(index)] = sub.toJson().dump();
        if (r == DownloadResult::Failed) { k.error = sub.error; logLine("video stream " + std::to_string(index) + ": " + sub.error); }
    }
    if (r == DownloadResult::Done) base += std::max<int64_t>(0, sub.total);
    return r;
}

// picture and sound while they download: in DDM's parts folder (a download from an older DDM: where it already is)
std::string streamPath(const Task& main, int index, const std::string& ext) {
    std::string tail = ".stream" + std::to_string(index) + "." + (ext.empty() ? "bin" : ext);
    std::string legacy = joinPath(main.dir, stem(main.fileName) + tail);
    if (!main.dir.empty() && (fileExists(legacy) || dirExists(legacy + ".pieces"))) return legacy;
    makeDirs(partsFolder());
    return joinPath(partsFolder(), (main.id.empty() ? std::string("video") : main.id) + tail);
}

std::string joinerPath(std::function<void(const std::string&)> say, std::string& err) {
    (void)say;
    std::string j = engines::joiner();   // installed with DDM: nothing to download
    if (j.empty()) err = "A part of DDM is missing (ffmpeg.exe). Reinstall DDM.";
    return j;
}

// Joins picture (and sound) into the final file without re-encoding (or sound → MP3). Returns the file made, "" = failed.
std::string joinInto(const std::string& joiner, const std::string& vpath, const std::string& apath, const std::string& out0, bool mp3,
                     std::atomic<bool>* stop, const std::string& dir, bool& stopped, std::string& err) {
    stopped = false;
    std::vector<std::string> outs = {out0};
    if (!mp3) { std::string mkv = joinPath(dirName(out0), stem(baseName(out0)) + ".mkv"); if (lower(extOf(out0)) != "mkv") outs.push_back(mkv); }   // picture or sound MP4 can't hold
    for (auto& out1 : outs) {
        std::string out = out1;
        if (fileExists(out)) out = uniquePath(out);
        std::string tmp = joinPath(partsFolder(), std::to_string(nowMs()) + "-" + std::to_string(GetCurrentThreadId()) + ".joining." + extOf(out));   // (out of sight until it's whole)
        std::vector<std::string> a = {"-hide_banner", "-loglevel", "error", "-y"};
        // what was downloaded is read as the files it is and nothing else: a "video" that is really a list of
        // addresses can't make the joiner open other files or the network
        const std::vector<std::string> only = {"-protocol_whitelist", "file"};
        a.insert(a.end(), only.begin(), only.end());
        if (mp3) a.insert(a.end(), {"-i", apath.empty() ? vpath : apath, "-vn", "-c:a", "libmp3lame", "-q:a", "0", tmp});
        else if (apath.empty()) a.insert(a.end(), {"-i", vpath, "-map", "0", "-c", "copy", "-dn", "-ignore_unknown"});
        else { a.insert(a.end(), {"-i", vpath}); a.insert(a.end(), only.begin(), only.end()); a.insert(a.end(), {"-i", apath, "-map", "0:v:0", "-map", "1:a:0", "-c", "copy"}); }
        if (!mp3) { if (lower(extOf(out)) == "mp4") a.insert(a.end(), {"-movflags", "+faststart"}); a.push_back(tmp); }
        RunResult rr = runHidden(joiner, a, nullptr, stop, 0, dir);
        if (rr.stopped) { DeleteFileW(W(tmp).c_str()); stopped = true; return ""; }
        if (rr.exitCode == 0 && fileSize(tmp) > 0 && MoveFileExW(W(tmp).c_str(), W(out).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) return out;
        DeleteFileW(W(tmp).c_str());
        err = rr.err.substr(0, 400);
        logLine("join: " + err);
    }
    return "";
}

// Picture + sound (or sound → MP3) at full speed: both files with DDM's engine, then joined without re-encoding.
DownloadResult tryStreams(DownloadContext& ctx) {
    Task t;
    { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; }
    auto say = [&](const std::string& s) { { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->status = s; } if (ctx.changed) ctx.changed(); };
    auto fail = [&](const std::string& e) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->error = e; return DownloadResult::Failed; };
    std::string err;
    std::string joiner = joinerPath(say, err);
    if (joiner.empty()) return fail(err.empty() ? "The video joiner is missing. Reinstall DDM." : err);
    bool st = false;
    waitNotBefore(ctx, st);
    if (st) return DownloadResult::Stopped;
    makeDirs(t.dir);
    int64_t base = 0;
    bool audioOnly = t.mediaUrl.empty();
    if (!audioOnly) {
        DownloadResult r = streamPart(ctx, 0, t.mediaUrl, t.mediaExt, base);
        if (r != DownloadResult::Done) return r;
    }
    DownloadResult r = streamPart(ctx, 1, t.audioUrl, t.audioExt, base);
    if (r != DownloadResult::Done) return r;
    { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; }
    say(!t.convertTo.empty() ? "Converting to MP3…" : "Joining video and sound…");
    std::string vpath = streamPath(t, 0, t.mediaExt), apath = streamPath(t, 1, t.audioExt);
    bool stopped = false;
    std::string out = joinInto(joiner, audioOnly ? "" : vpath, apath, t.path(), !t.convertTo.empty() || audioOnly, ctx.stop, t.dir, stopped, err);
    if (stopped) return DownloadResult::Stopped;
    if (out.empty()) return fail("The video and sound couldn't be joined.");
    DeleteFileW(W(vpath).c_str());
    DeleteFileW(W(apath).c_str());
    std::lock_guard<std::mutex> g(*ctx.mu);
    Task& k = *ctx.task;
    k.fileName = baseName(out);
    k.total = k.done = fileSize(out);
    k.partState.clear();
    return DownloadResult::Done;
}

// ------------------------------------------------------------------ HLS / DASH with DDM's stream engine
bool expiredText(const std::string& e) { return e.find("expired") != std::string::npos || e.find("answered 403") != std::string::npos || e.find("answered 404") != std::string::npos || e.find("answered 410") != std::string::npos; }

// one whole file as the stream engine's pieces: 4 MB byte ranges fetched in parallel (never the whole file in memory)
streams::Plan oneFilePlan(const RequestInfo& base, const std::string& url, const std::string& ext) {
    streams::Plan p;
    p.ext = ext;
    RequestInfo r = base;
    r.url = url;
    ResponseInfo info;
    std::string e;
    if (probe(r, info, &e) && info.total > 0 && info.ranges) {
        const int64_t step = 4LL * 1024 * 1024;
        for (int64_t at = 0; at < info.total; at += step) { streams::Piece x; x.url = url; x.from = at; x.length = std::min(step, info.total - at); p.pieces.push_back(x); }
    } else { streams::Piece x; x.url = url; p.pieces.push_back(x); }   // (a server without ranges: one piece, up to the engine's limit)
    return p;
}

// which pieces to fetch for the task's quality
bool planFor(const Task& t, const RequestInfo& base, streams::Plan& vp, streams::Plan& ap, bool& haveV, bool& haveA, std::string& err) {
    haveV = haveA = false;
    std::string sel = t.videoFormat;
    bool soundOnly = !t.convertTo.empty() || sel == "hls:audio" || sel == "dash:audio";
    RequestInfo r = base;
    r.url = t.url;
    if (startsWith(sel, "dash") || (!startsWith(sel, "hls") && streams::looksDash(t.url))) {
        streams::Manifest m;
        if (!streams::readDash(r, m, err)) return false;
        const streams::Representation* v = nullptr;
        const streams::Representation* a = nullptr;
        std::string vid, aid;
        if (startsWith(sel, "dash:") && sel != "dash:audio") { size_t plus = sel.find('+', 5); vid = sel.substr(5, plus == std::string::npos ? std::string::npos : plus - 5); if (plus != std::string::npos) aid = sel.substr(plus + 1); }
        for (auto& x : m.reps) {
            if (x.video && !soundOnly) { if ((!vid.empty() && x.id == vid) || (vid.empty() && (!v || x.height > v->height || (x.height == v->height && x.bandwidth > v->bandwidth)))) v = &x; }
            if (x.audio && !x.video) {
                auto score = [](const streams::Representation& y) { return (startsWith(lower(y.codecs), "mp4a") ? 1e9 : 0) + static_cast<double>(y.bandwidth); };
                if ((!aid.empty() && x.id == aid) || (aid.empty() && (!a || score(x) > score(*a)))) a = &x;
            }
        }
        if (!vid.empty() && !v) { err = "This quality isn't offered any more."; return false; }
        if (v) { vp = v->singleUrl.empty() ? streams::dashPlan(*v) : oneFilePlan(base, v->singleUrl, "mp4"); haveV = !vp.pieces.empty(); }
        if (a) { ap = a->singleUrl.empty() ? streams::dashPlan(*a) : oneFilePlan(base, a->singleUrl, "m4a"); haveA = !ap.pieces.empty(); }
        if (!haveV && !haveA) { err = "The stream has nothing to download."; return false; }
        return true;
    }
    streams::Master m;
    if (!streams::readHls(r, m, err)) return false;
    const streams::Variant* pick = nullptr;
    int wantH = -1;
    int64_t wantBw = -1;
    if (startsWith(sel, "hls:") && sel != "hls:audio") { auto p = split(sel.substr(4), ':'); if (!p.empty()) wantH = atoi(p[0].c_str()); if (p.size() > 1) wantBw = _atoi64(p[1].c_str()); }
    for (auto& v : m.variants) {
        if (isAudioOnly(v)) continue;
        if (wantH >= 0) {
            if (v.height != wantH) continue;
            if (!pick || (wantBw > 0 && std::llabs(v.bandwidth - wantBw) < std::llabs(pick->bandwidth - wantBw))) pick = &v;
        } else if (!pick) pick = &v;   // sorted best first
    }
    if (!pick && wantH >= 0) for (auto& v : m.variants) if (!isAudioOnly(v) && v.height <= wantH) { pick = &v; break; }   // the nearest below
    if (!pick && !m.variants.empty()) pick = &m.variants[0];
    if (!pick) { err = "The stream lists no qualities."; return false; }
    // the sound of that quality, when it's a separate list
    const streams::Rendition* aud = nullptr;
    for (auto& a : m.audios) if (a.group == pick->audioGroup && !a.url.empty() && (!aud || a.isDefault)) aud = &a;
    if (soundOnly) {
        if (aud) { if (!streams::hlsPlan(r, aud->url, ap, err)) return false; haveA = true; return true; }
        const streams::Variant* small = nullptr;   // sound alone, or the smallest quality (its sound is the same)
        for (auto& v : m.variants) if (isAudioOnly(v)) { small = &v; break; }
        if (!small) for (auto& v : m.variants) if (!small || v.bandwidth < small->bandwidth) small = &v;
        if (!streams::hlsPlan(r, small->url, vp, err)) return false;
        haveV = true;
        return true;
    }
    // the picture's list and the sound's list are read at the same time: the download starts sooner
    bool wantA = aud && aud->url != pick->url, okA = true;
    std::string errA;
    std::thread ta;
    if (wantA) ta = std::thread([&] { okA = streams::hlsPlan(r, aud->url, ap, errA); });
    bool okV = streams::hlsPlan(r, pick->url, vp, err);
    if (ta.joinable()) ta.join();
    if (!okV) return false;
    haveV = true;
    if (wantA) { if (!okA) { err = errA; return false; } haveA = true; }
    return true;
}

DownloadResult runStream(DownloadContext& ctx) {
    Task t;
    { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; }
    auto say = [&](const std::string& s) { { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->status = s; } if (ctx.changed) ctx.changed(); };
    auto fail = [&](const std::string& e) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->error = e; ctx.task->speed = 0; return DownloadResult::Failed; };
    std::string err;
    std::string joiner = joinerPath(say, err);
    if (joiner.empty()) return fail(err.empty() ? "The video joiner is missing. Reinstall DDM." : err);
    makeDirs(t.dir);
    int64_t firstTotal = t.total;
    for (int attempt = 0; attempt < 3; attempt++) {
        { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; }
        say("Reading the stream…");
        RequestInfo base = baseRequest(t.cookies, t.userAgent, t.pageUrl != t.url && httpUrl(t.pageUrl) ? t.pageUrl : "", t.headers);
        streams::Plan vp, ap;
        bool haveV = false, haveA = false;
        err.clear();
        if (!planFor(t, base, vp, ap, haveV, haveA, err)) {
            if (ctx.stop && ctx.stop->load()) return DownloadResult::Stopped;
            if (attempt < 2 && (expiredText(err) || err.find("answered 4") != std::string::npos) && pageOf(t) != t.url) { std::string e2; if (refreshTask(ctx, e2)) continue; }
            return fail(err);
        }
        int conns = std::max(1, std::min(16, t.connections > 0 ? t.connections : settings().connections));
        int lim = settings().speedLimitKBps;
        if (t.limitKBps > 0 && (lim == 0 || t.limitKBps < lim)) lim = t.limitKBps;
        std::string vpath = streamPath(t, 0, vp.ext), apath = streamPath(t, 1, ap.ext);
        int64_t base0 = 0;
        bool live = vp.live;
        auto part = [&](int idx, const streams::Plan& plan, const std::string& path) -> DownloadResult {
            if (fileSize(path) > 0 && !dirExists(path + ".pieces")) { base0 += fileSize(path); return DownloadResult::Done; }   // done before a pause
            std::string e;
            int64_t b0 = base0;
            DownloadResult r = streams::fetch(base, plan, path, conns, lim, ctx.stop, [&](const streams::Progress& p) {
                {
                    std::lock_guard<std::mutex> g(*ctx.mu);
                    Task& k = *ctx.task;
                    k.done = b0 + p.done;
                    int64_t est = p.estimate > 0 ? b0 + p.estimate : -1;
                    k.total = plan.live ? -1 : std::max<int64_t>(firstTotal, est);
                    if (k.total > 0 && k.total < k.done) k.total = -1;
                    k.speed = p.speed;
                    k.activeConnections = p.active;
                    k.eta = p.speed > 1 && k.total > k.done ? static_cast<int64_t>((k.total - k.done) / p.speed) : -1;
                    k.status = plan.live ? "Recording the live stream…" : idx == 0 ? "Receiving data…" : "Receiving the sound…";
                    k.resumable = !plan.live;
                }
                if (ctx.changed) ctx.changed();
            }, e);
            if (r == DownloadResult::Done) base0 += fileSize(path);
            else if (r == DownloadResult::Failed) err = e;
            return r;
        };
        DownloadResult r = DownloadResult::Done;
        if (haveV) r = part(0, vp, vpath);
        if (r == DownloadResult::Done && haveA && !(live && ctx.stop && ctx.stop->load())) r = part(1, ap, apath);
        if (r == DownloadResult::Stopped) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->speed = 0; return r; }
        if (r == DownloadResult::Failed) {
            if (ctx.stop && ctx.stop->load()) return DownloadResult::Stopped;
            if (attempt < 2 && expiredText(err)) {   // the addresses expired: read them again (from the page when it gives them) and carry on
                std::string e2;
                if (pageOf(t) != t.url) refreshTask(ctx, e2);
                continue;
            }
            return fail(err);
        }
        // every piece is here: one playable file
        bool mp3 = !t.convertTo.empty();
        say(mp3 ? "Converting to MP3…" : "Making the video file…");
        std::string target = t.path();
        if (!mp3 && lower(extOf(target)) != "mp4" && lower(extOf(target)) != "mkv") target = joinPath(t.dir, stem(t.fileName) + ".mp4");
        bool stopped = false;
        std::string out = joinInto(joiner, haveV ? vpath : "", haveA ? apath : "", target, mp3, nullptr, t.dir, stopped, err);
        if (out.empty() && !mp3 && haveV && !haveA) {   // keep the stream as it came (still plays in most players)
            out = joinPath(t.dir, stem(t.fileName) + "." + (vp.ext.empty() ? "ts" : vp.ext));
            if (fileExists(out)) out = uniquePath(out);
            if (!MoveFileExW(W(vpath).c_str(), W(out).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) out.clear();
        }
        if (out.empty()) return fail("The video couldn't be put together.");
        DeleteFileW(W(vpath).c_str());
        DeleteFileW(W(apath).c_str());
        std::lock_guard<std::mutex> g(*ctx.mu);
        Task& k = *ctx.task;
        k.fileName = baseName(out);
        k.total = k.done = fileSize(out);
        k.speed = 0;
        return DownloadResult::Done;
    }
    return fail(err.empty() ? "The video's address expired. Open the video's page again (Refresh download link)." : err);
}

bool isStreamTask(const Task& t) {
    return startsWith(t.videoFormat, "hls") || startsWith(t.videoFormat, "dash") || streams::looksHls(t.url) || streams::looksDash(t.url);
}

} // namespace

DownloadResult runVideoDownload(DownloadContext& ctx) {
    Task t;
    { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; ctx.task->error.clear(); }
    auto fail = [&](const std::string& e) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->error = e; ctx.task->speed = 0; return DownloadResult::Failed; };
    // a download from an earlier version (its quality named the old way) or one without addresses: read the page now
    if (t.audioUrl.empty() && t.mediaUrl.empty() && !isStreamTask(t)) {
        if (sites::isFileUrl(t.url)) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->mediaUrl = t.url; }
        else {
            std::string e;
            if (!refreshTask(ctx, e)) return fail(e);
        }
        { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; }
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        DownloadResult r;
        if (!t.audioUrl.empty()) r = tryStreams(ctx);
        else if (!t.mediaUrl.empty() && t.convertTo.empty()) r = tryDirect(ctx);
        else if (isStreamTask(t)) return runStream(ctx);
        else if (!t.mediaUrl.empty()) { std::lock_guard<std::mutex> g(*ctx.mu); ctx.task->audioUrl = ctx.task->mediaUrl; ctx.task->audioExt = extOf(fileNameFromUrl(ctx.task->mediaUrl)); ctx.task->mediaUrl.clear(); t = *ctx.task; continue; }   // a file to MP3
        else return fail("No video address is known for this download. Use Refresh download link.");
        if (r != DownloadResult::Failed || (ctx.stop && ctx.stop->load())) return r == DownloadResult::Failed ? DownloadResult::Stopped : r;
        if (attempt == 1) return r;
        // refused or expired: the same quality read again from its page, then the download continues where it was
        std::string why;
        { std::lock_guard<std::mutex> g(*ctx.mu); why = ctx.task->error; }
        std::string e;
        if (!refreshTask(ctx, e)) return fail(why.empty() ? e : why);
        { std::lock_guard<std::mutex> g(*ctx.mu); t = *ctx.task; }
    }
    return DownloadResult::Failed;
}

} // namespace ddm
