// DDM's own YouTube reader: asks YouTube's player service for a video's streams the way YouTube's own apps do
// (no helper program, nothing to install). When YouTube refuses this way (age check, "made for kids", live
// streams, sign-in), DDM falls back to its video helper.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace youtube {

struct Stream {
    int itag = 0;
    std::string url, mime, ext, vcodec, acodec;   // vcodec / acodec "none" when the stream has no picture / no sound
    int width = 0, height = 0, fps = 0;
    double kbps = 0;            // average bitrate
    int64_t size = -1;          // contentLength (exact)
    std::string audioLang;      // a video with several sound tracks: the track's language ("" = the only / default one)
    bool defaultAudio = true;
};

struct Caption { std::string lang, name, url; bool automatic = false; };

struct Video {
    std::string id, title, author, description, thumbnail;
    int duration = 0;
    std::vector<Stream> streams;
    std::vector<Caption> captions;
    int64_t notBefore = 0;      // unix seconds: the streams answer only after this (YouTube's pre-roll wait)
    std::string userAgent;      // the streams expect this browser identity
    std::string hls;            // the video as an HLS stream (live streams), "" = none
    bool live = false;
    std::string error;          // why it failed
    bool tryHelper = true;      // worth trying again another way (false: the video simply isn't available)
};

std::string videoId(const std::string& url);   // "" = not a single YouTube video
bool read(const std::string& url, const std::string& cookies, Video& out, std::function<void(const std::string&)> status = nullptr);
// A new address for the same stream (addresses expire after some hours): "" = couldn't.
std::string freshUrl(const std::string& videoId, int itag, const std::string& cookies);
int itagOf(const std::string& streamUrl);      // from a googlevideo address ("…&itag=137&…"), 0 = none
int64_t clenOf(const std::string& streamUrl);  // the stream's size written in its address, -1 = none
// Captions in WebVTT (as YouTube sends them with fmt=vtt) turned into SubRip (.srt).
std::string vttToSrt(const std::string& vtt);

} // namespace youtube
} // namespace ddm
