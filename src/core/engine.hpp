// All downloads: list, queue (how many run at once), start/pause/remove, saving, and events for the window.
#pragma once
#include "model.hpp"
#include "queues.hpp"
#include <deque>
#include <atomic>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ddm {

enum class EngineEvent { Changed, Started, Completed, Failed, Added, Removed };

class Engine {
public:
    static Engine& get();

    void load();
    void resumeQueued();                           // after the window is up: queued downloads start again
    void save();                                   // now
    void shutdown();                               // pause everything, save

    // Adds a download (file name/folder already decided by the prompt). Returns its id.
    std::string add(Task t, bool start);
    void start(const std::string& id);             // queued; runs when a slot is free
    void pause(const std::string& id);
    void stop(const std::string& id);              // like Pause, but Resume all and queues leave it stopped; a torrent also stops sharing
    void remove(const std::string& id, bool deleteFile);
    void redownload(const std::string& id);        // from zero
    void refreshLink(const std::string& id, const std::string& newUrl);
    // Refresh Download Link: the new address (and the browser's cookies for it) for an interrupted download.
    // Everything already downloaded is kept; the download carries on from the same byte.
    void refreshFrom(const std::string& id, const Task& fresh);
    void setTorrentFiles(const std::string& id, const std::string& files);   // X-Ray: only these files of a torrent ("1,3,4")
    void startAll();
    void pauseAll();
    void removeFinished();
    void setDescription(const std::string& id, const std::string& text);
    void edit(const std::string& id, const std::function<void(Task&)>& change);   // File Properties: several fields at once
    void setOptions(const std::string& id, int limitKBps, bool openWhenDone, bool shutdownWhenDone);

    std::vector<Task> snapshot();                  // copies for the window
    bool find(const std::string& id, Task& out);
    bool findUrl(const std::string& url, Task& out);
    int running();
    double totalSpeed();

    // Called on the engine's threads; the window marshals to its own thread.
    std::function<void(EngineEvent, const std::string& id)> onEvent;

    void applySpeedLimit();
    std::atomic<bool> idleBoosted{false};          // the speed limit is lifted: nothing else uses the internet
    void watchIdle();
    std::mutex& lock() { return mu; }

    // The list as JSON / encrypted .ddm (export, import, automatic backups).
    // Scheduler and conditions (called every 20 s by the engine itself).
    // Download File Info: the file starts downloading quietly as soon as the window opens; Start Download moves it
    // into the list (and later into the chosen folder), Cancel throws it away.
    void setScan(const std::string& id, const std::string& state, const std::string& note);
    void setVerify(const std::string& id, const std::string& state, const std::string& checksum = "");   // "ok" | "bad" | ""
    std::string preStart(Task t);
    void preConfirm(const std::string& id, const Task& final, bool startNow);
    void preCancel(const std::string& id);
    void startQueue(const std::string& queueId);
    void stopQueue(const std::string& queueId);
    bool queueRunning(const std::string& queueId);
    void syncQueue(const std::string& queueId);          // periodic synchronization pass
    void addToQueue(const std::string& id, const std::string& queueId);
    void removeFromQueue(const std::string& id);
    void setPriority(const std::string& id, int p);
    void forceStart(const std::string& id);
    void restart(const std::string& id);                 // from 0%: the old data goes to the Recycle Bin
    std::string blockReason(const Task& t);
    void retryFailed(bool now);                          // automatic resume of downloads that failed on the connection
    void watchNetwork();              // "" = may run
    std::function<void(const Queue& q)> onQueueFinished;     // a one-time queue finished: its "when done" actions
    std::function<void(const std::string&, const std::string&)> onNotice;   // title, text (silent tray note)
    std::function<void(const std::string& action)> onQueueDone;   // every download finished ("notify", "sleep", "hibernate", "shutdown")
    struct Power { bool charging = true; int percent = 100; bool hasBattery = false; };
    static Power power();

    std::string exportJson();
    int importList(const std::string& jsonOrDdm, std::string& error);   // returns how many were added
    void backupNow();
    std::string backupFolder();
    void backupIfDue();                                                 // called every few minutes

private:
    Engine() = default;
    std::mutex mu;                                 // guards tasks and stops
    std::vector<std::shared_ptr<Task>> tasks;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> stops;   // running downloads
    std::set<std::string> restartAfterStop;
    std::set<std::string> unstaging;      // confirmed background downloads whose files are being moved to their folder
    void unstage(const std::string& id, Task final, bool startNow);
    bool limitReached = false;
    std::deque<std::pair<int64_t, int64_t>> usage;   // (time, bytes) for download limits
    int64_t lastDoneSum = -1;
    bool hadWork = false;               // something ran since the queue was last empty
    void tick();                              // resumed while still stopping
    std::atomic<int> workers{0};
    std::atomic<bool> dirty{false};
    std::atomic<bool> quitting{false};
    std::thread saver;

    void schedule();                               // start queued downloads while slots are free (mu held)
    void launch(const std::shared_ptr<Task>& t);   // mu held
    void workerMain(std::shared_ptr<Task> t, std::shared_ptr<std::atomic<bool>> stop);
    void fire(EngineEvent e, const std::string& id) { if (onEvent) onEvent(e, id); }
    std::shared_ptr<Task> byId(const std::string& id);
};

} // namespace ddm
