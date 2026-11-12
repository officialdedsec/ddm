// Playlists, channels and pages full of videos: every video of a YouTube playlist or channel (videos, Shorts, live),
// or the video pages linked from any other page. Subscriptions download a list's new videos by themselves.
#pragma once
#include "model.hpp"
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace playlists {

struct Item { std::string url, id, title; int duration = 0; };
struct List {
    std::string title, author, url, feed;   // feed: where new videos show first (YouTube's own feed), "" = read the list again
    std::string kind;                         // "playlist", "channel", "page"
    std::vector<Item> items;
    std::string error;
};

bool isListUrl(const std::string& url);   // a YouTube playlist / channel link (not a single video)
// Reads every video of the list (YouTube: all pages of it, up to `most`). status(text) as it goes.
bool read(const std::string& url, const std::string& cookies, List& out, std::function<void(const std::string&)> status = nullptr, int most = 5000);
// The newest videos only (for subscriptions): the feed when there is one (fast), else the list.
bool latest(const std::string& url, const std::string& feed, List& out);

// A video download as the Download Video window would add it, for a quality chosen beforehand:
// quality "" / "Best", "2160p" … "360p", or "MP3". The quality is matched when the download starts (or the nearest below).
Task videoTask(const std::string& pageUrl, const std::string& title, const std::string& quality, const std::string& folder);

// ---- subscriptions (data\subscriptions.json)
struct Sub {
    std::string id, name, url, feed, quality, folder;
    std::vector<std::string> seen;   // video ids / links already taken
    int64_t added = 0, lastCheck = 0, lastFound = 0;
    int downloaded = 0;
    bool paused = false;
};
std::vector<Sub> subscriptions();
std::string subscribe(const List& l, const std::string& quality, const std::string& folder);   // the videos there now count as seen
void unsubscribe(const std::string& id);
void setPaused(const std::string& id, bool paused);
// Checks the subscriptions that are due (every 3 hours; force = all now). Returns the new videos' downloads
// (not added yet). Network: call from a worker thread.
std::vector<Task> checkSubscriptions(bool force);

} // namespace playlists
} // namespace ddm
