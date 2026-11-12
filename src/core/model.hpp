// A download and its parts.
#pragma once
#include "../common/json.hpp"
#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ddm {

// A video or sound stream the browser itself loaded on a page (seen by the add-on): its address, what it is
// ("hls", "dash" or "file"), the type its server gave and its size when known.
// One subtitle track a video offers. format: "vtt" / "srt" (one file) or "hls" (a list of small subtitle files).
struct SubTrack { std::string lang, name, url, format; bool automatic = false; };

struct SeenStream {
    std::string url, kind, mime;
    long long size = -1;
    std::map<std::string, std::string> headers;   // what the browser itself sent when it asked for it (no cookies): DDM asks the same way
};

struct Segment {
    int64_t start = 0;
    int64_t end = -1;     // inclusive; -1 = to the end of an unknown size
    int64_t pos = 0;      // next byte to write
    bool done() const { return end >= 0 && pos > end; }
    int64_t left() const { return end < 0 ? -1 : end - pos + 1; }
};

enum class State { Queued, Running, Paused, Complete, Error };

struct Task {
    std::string id;
    std::string kind = "http";           // "http" | "video" | "torrent" | "mega"
    std::string url;
    std::string pageUrl;                 // page the download came from (Referer)
    std::string cookies, userAgent;
    std::map<std::string, std::string> headers;
    std::string fileName, dir, category, description;
    std::string site;                    // shown in the Source column ("tiktok.com")
    std::string favicon;                 // favicon URL given by the browser, if any
    std::string videoFormat;             // for kind == "video": extractor format selector
    std::string convertTo;               // "mp3": sound converted after download
    std::string formatLabel;             // "1080p" shown in the list
    std::string mediaUrl;
    std::string audioUrl;                // the sound stream when picture and sound come separately (joined after)
    std::string mediaExt, audioExt;
    std::vector<std::string> partState; // progress of each stream (saved, so a pause continues)                // the video as one plain file (DDM's own engine); falls back to the extractor
    int64_t total = -1, done = 0;
    bool resumable = false;
    State state = State::Paused;
    std::string error;
    std::vector<Segment> segments;
    int connections = 0;                 // 0 = Options value
    int64_t created = 0, lastTry = 0, finished = 0;
    int64_t activeMs = 0;                // time spent actually downloading, every run added up (pauses not counted)
    std::string queue;                   // queue id ("main", "sync", …) or "" (mirrors queues.json)
    int limitKBps = 0;                   // this download only (progress window → Speed limiter)
    bool openWhenDone = false;           // progress window → When done
    bool shutdownWhenDone = false;
    bool scheduled = false;              // in the scheduled queue (runs inside the scheduler's time window)
    bool chargingOnly = false;           // waits while the laptop runs on battery
    int minBattery = 0;                  // pauses below this battery level (0 = off)
    int priority = 1;                    // 0 low, 1 normal, 2 high
    bool forceStart = false;
    int64_t notBefore = 0;               // video streams: the site serves them only from this time (unix seconds)
    bool stopped = false;                // Stop (not Pause): stays stopped until you start it yourself (Resume all and queues leave it)
    std::string scanState;               // "", "scanning", "clean", "threat", "removed", "error"
    std::string scanNote;                // what the antivirus said
    bool hidden = false;                 // started quietly while Download File Info is open (not in the list yet)
    std::string tempDir;                 // where its data goes until the user confirms a folder             // ignores "downloads at the same time" once

    // Live values (not saved).
    double speed = 0;
    int64_t eta = -1;
    int activeConnections = 0;
    bool viaQueue = false;               // started by a running queue
    bool pausedByUser = false;           // paused by hand: a running queue leaves it alone
    int queueTries = 0;
    int autoTries = 0;                   // automatic resumes after a failure (live)
    bool retryable = false;              // the failure was the connection's, not the link's (live)
    int64_t retryAt = 0;
    std::string status;                  // "Connecting…", "Receiving data…"
    std::string checksum;                // "sha256:<hex>": what the server or the user says the file must be
    std::string verify;                  // "", "ok", "bad": the checksum check after it finished
    std::string subs;                    // videos: the subtitle tracks chosen for it, one per line: lang \t format \t url \t name
    std::string extras;                  // videos: "thumb", "subs" or "thumb,subs" (saved next to the video when it finishes)
    std::string remoteStamp;             // Last-Modified on the server (synchronization queue)
    std::string waitReason;              // why a queued download isn't running ("Waiting for the charger")

    std::string path() const;            // final file
    std::string partPath() const;        // where data goes while downloading (DDM's parts folder, never the download folder)
    double progress() const { return total > 0 ? static_cast<double>(done) / total : (state == State::Complete ? 1 : 0); }
    Json toJson() const;
    static Task fromJson(const Json& j);
};

const char* stateName(State s);

// The folder for unfinished downloads ("" = DDM's data folder\temp)
std::string partsFolder();
void setPartsFolder(const std::string& dir);

} // namespace ddm
