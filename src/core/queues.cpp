#include "queues.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <algorithm>

namespace ddm {
namespace queues {

namespace {
std::mutex g_mu;
std::vector<Queue> g_q;
DownloadLimits g_lim;

Queue defaults(const std::string& id, const std::string& name) { Queue q; q.id = id; q.name = name; q.periodic = id == "sync"; return q; }

std::string file() { return joinPath(dataDir(), "queues.json"); }
} // namespace

std::mutex& lock() { return g_mu; }
std::vector<Queue>& all() { return g_q; }
DownloadLimits& limits() { return g_lim; }

Queue* find(const std::string& id) {
    for (auto& q : g_q) if (q.id == id) return &q;
    return nullptr;
}

void load() {
    std::lock_guard<std::mutex> g(g_mu);
    g_q.clear();
    std::string text;
    Json j = readFile(file(), text) ? Json::parse(text) : Json();
    for (auto& qj : j["queues"].list()) {
        Queue q;
        q.id = qj["id"].str(); q.name = qj["name"].str();
        if (q.id.empty()) continue;
        q.periodic = qj["periodic"].boolean(); q.startOnStartup = qj["startOnStartup"].boolean();
        q.startAtOn = qj["startAtOn"].boolean(); q.startAt = qj["startAt"].i32(q.startAt); q.daily = qj["daily"].boolean(); q.onceDate = qj["onceDate"].i32();
        q.days = qj["days"].str("1111111"); if (q.days.size() != 7) q.days = "1111111";
        q.stopAtOn = qj["stopAtOn"].boolean(); q.stopAt = qj["stopAt"].i32(q.stopAt);
        q.retriesOn = qj["retriesOn"].boolean(); q.retries = qj["retries"].i32(10);
        q.openFileOn = qj["openFileOn"].boolean(); q.openFile = qj["openFile"].str();
        q.hangUp = qj["hangUp"].boolean(); q.exitWhenDone = qj["exitWhenDone"].boolean();
        q.turnOffOn = qj["turnOffOn"].boolean(); q.turnOffMode = qj["turnOffMode"].str("shutdown"); q.forceClose = qj["forceClose"].boolean();
        q.simultaneous = std::max(1, std::min(32, qj["simultaneous"].i32(1)));
        q.syncEveryMin = std::max(5, qj["syncEveryMin"].i32(120));
        q.lastStartDay = qj["lastStartDay"].i64(); q.lastStopDay = qj["lastStopDay"].i64();
        for (auto& x : qj["order"].list()) q.order.push_back(x.str());
        g_q.push_back(q);
    }
    if (!find("main")) g_q.insert(g_q.begin(), defaults("main", "Main download queue"));
    if (!find("sync")) {
        auto it = std::find_if(g_q.begin(), g_q.end(), [](const Queue& q) { return q.id == "main"; });
        g_q.insert(it + 1, defaults("sync", "Synchronization queue"));
    }
    const Json& l = j["limits"];
    g_lim.on = l["on"].boolean(); g_lim.maxMB = l["maxMB"].i32(200); g_lim.everyHours = l["everyHours"].i32(5); g_lim.warn = l["warn"].boolean(true); g_lim.recheck = l["recheck"].boolean();
}

void save() {
    Json arr = Json::array();
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto& q : g_q) {
            Json o = Json::object();
            o["id"] = q.id; o["name"] = q.name; o["periodic"] = q.periodic; o["startOnStartup"] = q.startOnStartup;
            o["startAtOn"] = q.startAtOn; o["startAt"] = q.startAt; o["daily"] = q.daily; o["onceDate"] = q.onceDate; o["days"] = q.days;
            o["stopAtOn"] = q.stopAtOn; o["stopAt"] = q.stopAt; o["retriesOn"] = q.retriesOn; o["retries"] = q.retries;
            o["openFileOn"] = q.openFileOn; o["openFile"] = q.openFile; o["hangUp"] = q.hangUp; o["exitWhenDone"] = q.exitWhenDone;
            o["turnOffOn"] = q.turnOffOn; o["turnOffMode"] = q.turnOffMode; o["forceClose"] = q.forceClose; o["simultaneous"] = q.simultaneous;
            o["syncEveryMin"] = q.syncEveryMin; o["lastStartDay"] = q.lastStartDay; o["lastStopDay"] = q.lastStopDay;
            Json ord = Json::array();
            for (auto& x : q.order) ord.push(x);
            o["order"] = ord;
            arr.push(o);
        }
    }
    Json j = Json::object();
    j["queues"] = arr;
    Json l = Json::object();
    l["on"] = g_lim.on; l["maxMB"] = g_lim.maxMB; l["everyHours"] = g_lim.everyHours; l["warn"] = g_lim.warn; l["recheck"] = g_lim.recheck;
    j["limits"] = l;
    writeFileAtomic(file(), j.dump(true));
}

std::string add(const std::string& name) {
    std::string id;
    {
        std::lock_guard<std::mutex> g(g_mu);
        Queue q = defaults("q" + newId(), name);
        id = q.id;
        g_q.push_back(q);
    }
    save();
    return id;
}

bool remove(const std::string& id) {
    if (id == "main" || id == "sync") return false;
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_q.erase(std::remove_if(g_q.begin(), g_q.end(), [&](const Queue& q) { return q.id == id; }), g_q.end());
    }
    save();
    return true;
}

std::string nameOf(const std::string& id) {
    std::lock_guard<std::mutex> g(g_mu);
    auto q = find(id);
    return q ? q->name : "";
}

void addTask(const std::string& queueId, const std::string& taskId) {
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto& q : g_q) q.order.erase(std::remove(q.order.begin(), q.order.end(), taskId), q.order.end());
        if (auto q = find(queueId)) q->order.push_back(taskId);
    }
    save();
}

void removeTask(const std::string& taskId) {
    {
        std::lock_guard<std::mutex> g(g_mu);
        for (auto& q : g_q) q.order.erase(std::remove(q.order.begin(), q.order.end(), taskId), q.order.end());
    }
    save();
}

void move(const std::string& queueId, const std::string& taskId, int delta) {
    {
        std::lock_guard<std::mutex> g(g_mu);
        auto q = find(queueId);
        if (!q) return;
        auto it = std::find(q->order.begin(), q->order.end(), taskId);
        if (it == q->order.end()) return;
        long i = it - q->order.begin(), j = i + delta;
        if (j < 0 || j >= static_cast<long>(q->order.size())) return;
        std::swap(q->order[static_cast<size_t>(i)], q->order[static_cast<size_t>(j)]);
    }
    save();
}

std::string queueOf(const std::string& taskId) {
    std::lock_guard<std::mutex> g(g_mu);
    for (auto& q : g_q) if (std::find(q.order.begin(), q.order.end(), taskId) != q.order.end()) return q.id;
    return "";
}

} // namespace queues
} // namespace ddm
