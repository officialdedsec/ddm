// Torrents and magnet links (the "Torrents" category): DDM's built-in BitTorrent engine (libtorrent), one shared
// session; each torrent pauses, resumes and fails on its own like any other download. Seeding afterwards, per-torrent
// options (file priorities, order, limits, trackers), peers, and making new .torrent files.
#pragma once
#include "downloader.hpp"
#include <functional>
#include <string>
#include <vector>

namespace ddm {

bool isMagnet(const std::string& url);
bool isTorrentFile(const std::string& urlOrPath);

DownloadResult runTorrentDownload(DownloadContext& ctx);

// Seeding (sharing back) after a torrent finished, when Options → Torrents (or the torrent's own options) say so.
bool torrentSeeding(const std::string& taskId);
void startSeedingNow(const std::string& taskId);     // a finished torrent: share it again (right-click → Seed)
void stopSeeding(const std::string& taskId);
void stopAllSeeding();                               // DDM is closing: remember what was sharing (shared again next start)
void torrentShutdown();                              // after the downloads stopped: save progress and the DHT, close
void restartSeeding();                               // DDM started: share again what was sharing when it closed
void torrentSettingsChanged();                       // Options → Torrents changed
std::string torrentMagnet(const std::string& taskId);   // its magnet link ("" if it can't be made)
void torrentDeleteFiles(const Task& t, bool data, bool torrentFile);   // after it left the list: its data and/or .torrent to the Recycle Bin
void torrentDeleteData(const std::string& taskId);     // only what was downloaded goes; it stays in the list, paused at 0%

namespace bt {

// Per-torrent options (kept next to its progress; -1 = as in Options)
struct Options {
    std::vector<int> priorities;      // per file: 0 don't download, 1 low, 4 normal, 7 high (empty = all normal)
    bool sequential = false;          // pieces in order
    bool firstLast = false;           // first and last piece of each file first
    bool superSeed = false;
    int uploadKBps = 0;               // 0 = no limit of its own
    int maxPeers = 0;                 // 0 = Options value
    int ratioPct = -1;                // stop sharing at this ratio ×100
    int seedMinutes = -1;
    int keepSeeding = -1;             // -1 Options, 0 no, 1 yes
    std::vector<std::string> trackers;   // added by you
    std::vector<std::string> removedTrackers;
    bool set = false;                 // saved once (else: from Options and the chosen files)
};
Options options(const std::string& taskId);
void setOptions(const std::string& taskId, const Options& o);   // saved, and applied at once when it's running

struct FileRow { std::string path; int64_t size = 0, done = 0; int priority = 4; };
struct PeerRow { std::string address, client, flags; float progress = 0; int down = 0, up = 0; };
struct TrackerRow { std::string url, status; int tier = 0, seeds = -1, peers = -1; bool working = false; };
struct Details {
    bool live = false;                // in the engine now (running or sharing); else from the saved progress
    std::string name, hash, comment, creator, savePath, state, error;
    int64_t created = 0, added = 0, completed = 0;
    int64_t total = 0, wanted = 0, done = 0, uploaded = 0, downloaded = 0;
    int pieces = 0, pieceSize = 0, piecesHave = 0;
    std::string pieceMap;             // '1' have, '0' missing (at most a few thousand)
    int seeds = 0, peers = 0, seedsInSwarm = -1, peersInSwarm = -1;
    float availability = 0;
    int down = 0, up = 0;
    int64_t seedingSeconds = 0, activeSeconds = 0;
    bool isPrivate = false, v2 = false;
    std::vector<FileRow> files;
    std::vector<PeerRow> peerList;
    std::vector<TrackerRow> trackers;
};
bool details(const std::string& taskId, Details& out, bool withPeers);

void reannounce(const std::string& taskId);
bool recheck(const std::string& taskId);              // running or sharing: check the files on the disk again
std::string engineStatus();                           // "Port 51413 open on the router · DHT 312 nodes"

struct CreateRequest {
    std::string source;               // a file or a folder
    std::vector<std::string> trackers, webSeeds;   // trackers: an empty line starts a new tier
    std::string comment;
    bool isPrivate = false;
    int pieceKB = 0;                  // 0 = automatic
    std::string version = "v1";       // "v1" (works everywhere) | "hybrid" | "v2"
    std::string out;                  // the .torrent to write
};
bool createTorrent(const CreateRequest& r, std::function<bool(int done, int total)> progress, std::string& magnet, std::string& error);

} // namespace bt
} // namespace ddm
