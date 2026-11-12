#include "model.hpp"
#include <mutex>
#include "../common/util.hpp"

namespace ddm {

const char* stateName(State s) {
    switch (s) {
        case State::Queued: return "queued";
        case State::Running: return "running";
        case State::Paused: return "paused";
        case State::Complete: return "complete";
        case State::Error: return "error";
    }
    return "paused";
}

static State stateFrom(const std::string& s) {
    if (s == "complete") return State::Complete;
    if (s == "error") return State::Error;
    if (s == "queued" || s == "running") return State::Queued;   // was downloading when DDM closed: carries on
    return State::Paused;
}

std::string Task::path() const { return joinPath(dir, fileName); }
// Unfinished downloads live out of sight, in DDM's own folder, never next to your files; the finished file is moved
// (or joined) into the download folder at the end. A download from an older DDM keeps its part where it already is.
static std::string g_parts;
static std::mutex g_partsMu;
std::string partsFolder() {
    std::lock_guard<std::mutex> g(g_partsMu);
    return g_parts.empty() ? joinPath(dataDir(), "temp") : g_parts;
}
void setPartsFolder(const std::string& dir) {
    { std::lock_guard<std::mutex> g(g_partsMu); g_parts = dir; }
    makeDirs(partsFolder());
}
std::string Task::partPath() const {
    if (!tempDir.empty()) return joinPath(tempDir, id + ".ddmpart");
    if (!fileName.empty() && !dir.empty()) {
        std::string legacy = path() + ".ddmpart";
        if (fileExists(legacy)) return legacy;
    }
    return joinPath(partsFolder(), (id.empty() ? std::string("download") : id) + ".ddmpart");
}

Json Task::toJson() const {
    Json j = Json::object();
    j["id"] = id; j["kind"] = kind; j["url"] = url; j["page"] = pageUrl; j["cookies"] = cookies; j["ua"] = userAgent;
    Json h = Json::object(); for (auto& kv : headers) h[kv.first] = kv.second; j["headers"] = h;
    j["name"] = fileName; j["dir"] = dir; j["cat"] = category; j["desc"] = description; j["site"] = site; j["favicon"] = favicon;
    j["subs"] = subs; j["format"] = videoFormat; j["convert"] = convertTo; j["formatLabel"] = formatLabel; j["scheduled"] = scheduled; j["chargingOnly"] = chargingOnly; j["minBattery"] = minBattery; j["priority"] = priority; j["mediaUrl"] = mediaUrl; j["audioUrl"] = audioUrl; j["mediaExt"] = mediaExt; j["audioExt"] = audioExt; { Json ps = Json::array(); for (auto& x : partState) ps.push(x); j["partState"] = ps; } j["hidden"] = hidden; j["scan"] = scanState == "scanning" ? "" : scanState; j["scanNote"] = scanNote; j["tempDir"] = tempDir; j["remoteStamp"] = remoteStamp; j["checksum"] = checksum; j["verify"] = verify; if (!extras.empty()) j["extras"] = extras; j["total"] = total; j["done"] = done; j["resumable"] = resumable;
    j["state"] = state == State::Running ? "running" : stateName(state);
    j["error"] = error; j["conns"] = connections; j["created"] = created; j["lastTry"] = lastTry; j["finished"] = finished; j["active"] = activeMs; j["stopped"] = stopped; j["notBefore"] = notBefore; j["queue"] = queue; j["limit"] = limitKBps; j["openDone"] = openWhenDone;
    Json segs = Json::array();
    for (auto& s : segments) { Json a = Json::array(); a.push(s.start); a.push(s.end); a.push(s.pos); segs.push(a); }
    j["segs"] = segs;
    return j;
}

Task Task::fromJson(const Json& j) {
    Task t;
    t.id = j["id"].str(); t.kind = j["kind"].str("http"); t.url = j["url"].str(); t.pageUrl = j["page"].str();
    t.cookies = j["cookies"].str(); t.userAgent = j["ua"].str();
    for (auto& kv : j["headers"].items()) t.headers[kv.first] = kv.second.str();
    t.fileName = j["name"].str(); t.dir = j["dir"].str(); t.category = j["cat"].str("general"); t.description = j["desc"].str();
    t.site = j["site"].str(); t.favicon = j["favicon"].str(); t.subs = j["subs"].str(); t.videoFormat = j["format"].str(); t.convertTo = j["convert"].str(); t.formatLabel = j["formatLabel"].str(); t.scheduled = j["scheduled"].boolean(); t.chargingOnly = j["chargingOnly"].boolean(); t.minBattery = j["minBattery"].i32(); t.priority = j["priority"].i32(1); t.mediaUrl = j["mediaUrl"].str(); t.audioUrl = j["audioUrl"].str(); t.mediaExt = j["mediaExt"].str(); t.audioExt = j["audioExt"].str(); for (auto& x : j["partState"].list()) t.partState.push_back(x.str()); t.hidden = j["hidden"].boolean(); t.scanState = j["scan"].str(); t.scanNote = j["scanNote"].str(); t.tempDir = j["tempDir"].str(); t.remoteStamp = j["remoteStamp"].str(); t.checksum = j["checksum"].str(); t.verify = j["verify"].str(); t.extras = j["extras"].str();
    t.total = j["total"].i64(-1); t.done = j["done"].i64(); t.resumable = j["resumable"].boolean();
    t.state = stateFrom(j["state"].str()); t.error = j["error"].str(); t.connections = j["conns"].i32();
    t.created = j["created"].i64(); t.lastTry = j["lastTry"].i64(); t.finished = j["finished"].i64(); t.activeMs = j["active"].i64(); t.stopped = j["stopped"].boolean(); t.notBefore = j["notBefore"].i64(); t.queue = j["queue"].str(); t.limitKBps = j["limit"].i32(); t.openWhenDone = j["openDone"].boolean();
    for (auto& a : j["segs"].list()) { Segment s; s.start = a.at(0).i64(); s.end = a.at(1).i64(-1); s.pos = a.at(2).i64(); t.segments.push_back(s); }
    if (t.site.empty()) t.site = urlSite(t.url);
    return t;
}

} // namespace ddm
