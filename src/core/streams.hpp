// DDM's own engine for video streams: HLS (.m3u8, also encrypted AES-128, fMP4, byte ranges, separate sound
// tracks, live) and DASH (.mpd: templates, timelines, lists, single files). Pieces download in parallel and are kept
// one by one, so a stopped download continues with the next missing piece.
#pragma once
#include "downloader.hpp"
#include "http.hpp"
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace streams {

std::string resolveUrl(const std::string& base, const std::string& ref);
bool looksHls(const std::string& url);
bool looksDash(const std::string& url);
bool sameSite(const std::string& urlA, const std::string& urlB);   // the same site (example.com, a.example.co.uk…): cookies may go
// A stream's list of qualities (an HLS master playlist or a DASH manifest) as the browser itself read it: used
// instead of asking the server again, for a few hours. Some sites answer such a request only from a real browser.
void remember(const std::string& url, const std::string& text);

// ---- HLS
struct Variant { std::string url, codecs, audioGroup; int64_t bandwidth = 0; int width = 0, height = 0; double fps = 0; };
struct Rendition { std::string group, url, lang, name; bool isDefault = false; };
struct Master {
    std::vector<Variant> variants;     // a plain playlist gives one variant (itself)
    std::vector<Rendition> audios;     // separate sound tracks (EXT-X-MEDIA TYPE=AUDIO with a URI)
    std::vector<Rendition> subs;       // subtitle tracks (EXT-X-MEDIA TYPE=SUBTITLES with a URI)
    double duration = 0;               // seconds, when known (first variant)
};
bool readHls(const RequestInfo& r, Master& out, std::string& error);

// ---- DASH
struct Representation {
    std::string id, mime, codecs;
    bool video = false, audio = false;
    int64_t bandwidth = 0;
    int width = 0, height = 0;
    double fps = 0;
    std::string lang;
    std::string singleUrl;                 // the whole stream is one file (SegmentBase): DDM's normal engine fetches it
    std::string initUrl;                   // else: an initialization piece …
    std::vector<std::string> segments;     // … and its pieces in order
};
struct Manifest { std::vector<Representation> reps; std::vector<Representation> texts; double duration = 0; bool live = false; };   // texts: subtitles in one file each (singleUrl, lang)
bool parseDash(const std::string& xml, const std::string& mpdUrl, Manifest& out, std::string& error);
bool readDash(const RequestInfo& r, Manifest& out, std::string& error);

// ---- downloading a stream into one file
struct Piece { std::string url; int64_t from = -1, length = -1; std::string keyUrl, iv; bool encrypted = false; };
struct Plan {
    std::vector<Piece> pieces;
    Piece init;                            // fMP4 / DASH initialization ("" url = none)
    bool live = false;                     // HLS without an end: keeps recording until stopped
    std::string playlistUrl;               // live: read again for new pieces
    std::string ext;                       // "ts" or "mp4"/"m4a"
};
// The whole text of a stream's subtitle track (an HLS list of small WebVTT files, read one after another).
bool hlsText(const RequestInfo& r, const std::string& playlistUrl, std::string& out, std::string& error);
bool hlsPlan(const RequestInfo& r, const std::string& mediaPlaylistUrl, Plan& out, std::string& error);
Plan dashPlan(const Representation& rep);

struct Progress { int64_t done = 0, estimate = -1; double speed = 0; int active = 0; int piecesDone = 0, pieces = 0; };
// Fetches every piece (in parallel, as many connections as Options allow, at most 16), decrypts, and joins them into
// outFile. Pieces already fetched (in outFile + ".pieces") are kept. stop: returns Stopped (a live stream is saved).
DownloadResult fetch(const RequestInfo& base, const Plan& plan, const std::string& outFile, int connections, int limitKBps,
                     std::atomic<bool>* stop, std::function<void(const Progress&)> onProgress, std::string& error);

} // namespace streams
} // namespace ddm
