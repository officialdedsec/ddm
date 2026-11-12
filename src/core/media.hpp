// Videos from YouTube, Facebook, TikTok, Instagram, X, Vimeo, Dailymotion, Reddit and any other site with a video
// player: every quality, with sound, read by DDM's own engine (no outside programs). Single files use DDM's
// multi-connection engine; streams (HLS / DASH) use its stream engine (pieces in parallel); picture and sound
// are joined on your PC without re-encoding.
#pragma once
#include "downloader.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ddm {

struct VideoFormat {
    std::string label;          // "1080p"
    std::string tag;            // "HD", "4K", "SD", "MP3"
    std::string spec;           // "MP4 · video + sound · 60fps"
    std::string group;          // "Video" | "Audio"
    std::string selector;       // which quality ("137+140", "hls:720:2500000", "dash:v5+a1")
    std::string ext;            // file extension the result will have
    std::string convert;        // "mp3" for audio conversion
    int64_t size = -1;
    bool exact = false;
    int height = 0;
    bool direct = false;        // one plain file: DDM's engine downloads it
    std::string directUrl;
    // video and sound as two plain files (YouTube, Facebook DASH): both at full speed with DDM's engine, then joined
    std::string videoUrl, audioUrl, videoExt, audioExt;
    std::map<std::string, std::string> headers;
    std::string streamUrl;      // a stream the browser played (HLS .m3u8): downloaded from that address directly
};

struct VideoInfo {
    std::string title, thumbnail, pageUrl, extractor;
    std::string uploader, videoId, description;   // for a good file name when the site's title says little
    std::string rawTitle;                         // the title exactly as the site gave it
    int duration = 0;
    int64_t notBefore = 0;      // unix seconds: the site serves the streams only from then on (YouTube's pre-roll wait)
    std::vector<VideoFormat> formats;
    std::vector<SubTrack> subs;   // every subtitle track the video offers (the uploader's first, automatic ones last)
    std::string error;
};

struct VideoRequest {
    std::string url;            // the video's page (or a direct file)
    std::string cookies;        // for the page's site
    std::string userAgent;
    std::string referer;
    std::vector<std::string> streams;   // streams the browser plays on that page (HLS / DASH): read when the page gives nothing
    std::string pageData;               // the video's data as the browser read it (TikTok): used when DDM can't read the page itself
    std::vector<SeenStream> seen;       // the same streams with what the browser learned about them (kind, type, size): newest last
    int height = 0;                     // the playing video's height in the browser (a seen file's quality)
    std::string forceKind;              // "hls" / "dash": url is that kind of stream, whatever its address looks like
    std::string title;                  // the video's name as the browser shows it (the tab's title for a player inside another page)
    int duration = 0;                   // the playing video's length in the browser (seconds): tells its stream from an advert's
    std::string direct;                 // the file the browser's player is playing (its src), when it is a plain address
    std::map<std::string, std::string> headers;   // asked for the way the browser asked (for a stream the browser loaded)
    std::string topUrl;                 // the tab's address, when the player sits in a frame inside it
    bool framed = false;                // the player sits inside another site's page: that page's title names the video
};

// Reads every quality with DDM's own readers. status() gets friendly progress lines.
bool probeVideo(const VideoRequest& r, VideoInfo& out, std::function<void(const std::string&)> status = nullptr);

// Engine worker for Task::kind == "video" (with progress, stop and resume; expired addresses are read again).
DownloadResult runVideoDownload(DownloadContext& ctx);

bool isVideoSite(const std::string& url);
// Sites that show video after video on one page (feeds, reels): the page's address may still name the previous video.
bool isFeedSite(const std::string& url);
// The video's own page for a player that sits inside another page (or keeps one address for every video): from the
// tab's address, or from the stream the browser loaded. Returns url itself when nothing better is known.
// A page's title without the site's own name or slogan at its end ("Holiday in Rome - SiteName" -> "Holiday in Rome").
std::string cleanVideoName(const std::string& title, const std::string& url, const std::string& topUrl);
std::string videoPageFor(const std::string& url, const std::string& topUrl, const std::vector<SeenStream>& seen);

// The video's real name for its file: the site's title cleaned of view counts, hashtags, links and "| Facebook";
// when that says nothing ("Video by …", "Reels", a number), the caption, then the page's title, then
// "<uploader> - <id>". pageTitle may be "".
std::string niceVideoTitle(const std::string& title, const std::string& description, const std::string& uploader,
                           const std::string& pageTitle, const std::string& videoId);

// After a video finishes: its thumbnail picture (<name>.jpg) and/or subtitles (<name>.<language>.srt) next to it.
// langs: "en", "en,es" or "all". Returns how many files were saved (-1 = failed, error says why).
int saveVideoExtras(const std::string& pageUrl, const std::string& videoPath, bool thumb, bool subs, const std::string& langs,
                    const std::string& cookies, const std::string& userAgent, std::string& error);

// The subtitle tracks to save for a video: those the languages in Options ask for ("en", "en,es", "all").
std::vector<SubTrack> subsForLangs(const std::vector<SubTrack>& all, const std::string& langs);
std::string packSubs(const std::vector<SubTrack>& picked);          // for Task::subs
std::vector<SubTrack> unpackSubs(const std::string& packed);
// Saves the chosen tracks next to the video as <name>.<language>.srt. Returns how many were saved.
int saveSubtitles(const std::string& videoPath, const std::vector<SubTrack>& picked, const std::string& pageUrl,
                  const std::string& cookies, const std::string& userAgent, std::string& error);
// A language's own name for a code ("en" -> "English"); the code itself when unknown.
std::string languageName(const std::string& code);

} // namespace ddm
