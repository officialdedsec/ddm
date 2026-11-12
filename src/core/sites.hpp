// DDM's own readers for video pages: TikTok, Facebook, Instagram, Threads, X, Vimeo, Dailymotion, Reddit, and any
// other page (the video players' usual ways of naming their files and streams: <video>, og:video, JSON-LD, player
// setups, .mp4 / .m3u8 / .mpd addresses, players inside frames). YouTube has its own reader (youtube.hpp).
#pragma once
#include <cstdint>
#include <map>
#include "model.hpp"
#include <string>
#include <vector>

namespace ddm {
namespace sites {

struct Source {
    std::string url;
    std::string kind;          // "file" (one file with sound), "video" (picture only), "audio", "hls", "dash", "mpd" (a manifest written into the page), "youtube" (a YouTube video shown in the page)
    int height = 0;
    int64_t size = -1;
    double kbps = 0;
    std::string ext;           // "mp4", "webm"… for files
    std::string label;         // "HD", "720p"… as the site names it
};

struct Page {
    std::string title, author, description, thumbnail, id, site;
    int duration = 0;
    std::vector<Source> sources;
    std::vector<SubTrack> subs;                   // subtitle tracks the page offers
    std::map<std::string, std::string> headers;   // what the files expect (Referer…)
    std::string error;
};

// Reads the page at url (with the browser's cookies and identity when given). false = nothing found (error says why).
bool read(const std::string& url, const std::string& cookies, const std::string& userAgent, const std::string& referer, Page& out);
bool isFileUrl(const std::string& url);
// What the browser read about a video itself (TikTok's post data), for sites that hide it from programs. false = unusable.
bool fromBrowser(const std::string& url, const std::string& data, const std::string& cookies, Page& out);   // an address that is a video / sound file itself (.mp4, .webm, .mp3…)

} // namespace sites
} // namespace ddm
