// Download queues, as in IDM's Scheduler: the Main download queue, the Synchronization queue (periodic re-check of
// files that change on the server) and any number of the user's own queues, plus global download limits.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ddm {

struct Queue {
    std::string id;                  // "main", "sync", "q<random>"
    std::string name;
    bool periodic = false;           // Periodic synchronization (else One-time downloading)
    bool startOnStartup = false;     // Start download on DDM startup
    bool startAtOn = false;          // Start download at <time>
    int startAt = 23 * 3600;         // seconds after midnight
    bool daily = false;              // false = Once at <date>
    int onceDate = 0;                // yyyymmdd
    std::string days = "1111111";    // Sunday..Saturday (IDM's order)
    bool stopAtOn = false;
    int stopAt = 7 * 3600 + 1800;
    bool retriesOn = false;
    int retries = 10;                // per file when it fails
    bool openFileOn = false;
    std::string openFile;            // a file to open when the queue is done
    bool hangUp = false;             // hang up the dial-up / modem connection when done
    bool exitWhenDone = false;
    bool turnOffOn = false;
    std::string turnOffMode = "shutdown";   // "shutdown" | "sleep" | "hibernate" | "restart"
    bool forceClose = false;         // force processes to terminate
    int simultaneous = 1;            // files downloaded at the same time from this queue
    int syncEveryMin = 120;          // periodic: check the files again every N minutes
    std::vector<std::string> order;  // download ids, in queue order

    // live
    bool running = false;
    int64_t lastStartDay = 0;        // yyyymmdd the start time last fired
    int64_t lastStopDay = 0;
    int64_t nextSync = 0;
};

struct DownloadLimits {
    bool on = false;
    int maxMB = 200;                 // Download no more than N MBytes
    int everyHours = 5;              // every N hours
    bool warn = true;                // show a warning when the limit is reached
    bool recheck = false;            // check the limit again when DDM starts
};

namespace queues {

std::mutex& lock();
std::vector<Queue>& all();           // guarded by lock()
DownloadLimits& limits();
void load();
void save();
Queue* find(const std::string& id);  // caller holds lock()
std::string add(const std::string& name);
bool remove(const std::string& id);  // main and sync can't be removed
std::string nameOf(const std::string& id);
void addTask(const std::string& queueId, const std::string& taskId);   // to the end (removes it from any other queue)
void removeTask(const std::string& taskId);
void move(const std::string& queueId, const std::string& taskId, int delta);   // up (-1) / down (+1)
std::string queueOf(const std::string& taskId);

} // namespace queues
} // namespace ddm
