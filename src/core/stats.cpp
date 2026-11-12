#include "stats.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <map>
#include <mutex>

namespace ddm {
namespace stats {

namespace {
std::mutex g_mu;
bool g_loaded = false, g_dirty = false;
std::map<std::string, int64_t> g_days, g_sites;
int64_t g_total = 0, g_finished = 0, g_fastest = 0;
int64_t g_lastSave = 0;

std::string path() { return joinPath(dataDir(), "stats.json"); }
std::string dayKey(int64_t unixMs) {
    SYSTEMTIME st; FILETIME ft, lt;
    ULARGE_INTEGER u; u.QuadPart = (static_cast<uint64_t>(unixMs) + 11644473600000ULL) * 10000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    FileTimeToLocalFileTime(&ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    char b[16]; snprintf(b, sizeof b, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return b;
}
void load() {
    if (g_loaded) return;
    g_loaded = true;
    std::string text;
    if (!readFile(path(), text)) return;
    Json j = Json::parse(text);
    for (auto& kv : j["days"].items()) g_days[kv.first] = kv.second.i64();
    for (auto& kv : j["sites"].items()) g_sites[kv.first] = kv.second.i64();
    g_total = j["total"].i64(); g_finished = j["finished"].i64(); g_fastest = j["fastest"].i64();
}
void saveLocked() {
    Json j = Json::object(), d = Json::object(), s = Json::object();
    // keep a year of days and the 200 biggest sites
    while (g_days.size() > 400) g_days.erase(g_days.begin());
    if (g_sites.size() > 200) {
        std::vector<std::pair<std::string, int64_t>> v(g_sites.begin(), g_sites.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        v.resize(200);
        g_sites = std::map<std::string, int64_t>(v.begin(), v.end());
    }
    for (auto& kv : g_days) d[kv.first] = kv.second;
    for (auto& kv : g_sites) s[kv.first] = kv.second;
    j["days"] = d; j["sites"] = s; j["total"] = g_total; j["finished"] = g_finished; j["fastest"] = g_fastest;
    writeFileAtomic(path(), j.dump());
    g_dirty = false;
    g_lastSave = nowMs();
}
} // namespace

void addBytes(int64_t bytes) {
    if (bytes <= 0) return;
    std::lock_guard<std::mutex> g(g_mu);
    load();
    g_days[dayKey(nowMs())] += bytes;
    g_total += bytes;
    g_dirty = true;
    if (nowMs() - g_lastSave > 60000) saveLocked();
}

void addFinished(const std::string& site, int64_t size, int64_t millis) {
    std::lock_guard<std::mutex> g(g_mu);
    load();
    g_finished++;
    if (!site.empty() && size > 0) g_sites[site] += size;
    if (size > 0 && millis > 1000) g_fastest = std::max(g_fastest, size * 1000 / millis);
    saveLocked();
}

std::vector<std::pair<std::string, int64_t>> lastDays(int n) {
    std::lock_guard<std::mutex> g(g_mu);
    load();
    std::vector<std::pair<std::string, int64_t>> out;
    int64_t now = nowMs();
    for (int i = n - 1; i >= 0; i--) {
        std::string k = dayKey(now - static_cast<int64_t>(i) * 86400000LL);
        auto it = g_days.find(k);
        out.push_back({k, it == g_days.end() ? 0 : it->second});
    }
    return out;
}

std::vector<std::pair<std::string, int64_t>> topSites(int n) {
    std::lock_guard<std::mutex> g(g_mu);
    load();
    std::vector<std::pair<std::string, int64_t>> v(g_sites.begin(), g_sites.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (static_cast<int>(v.size()) > n) v.resize(static_cast<size_t>(n));
    return v;
}

int64_t total() { std::lock_guard<std::mutex> g(g_mu); load(); return g_total; }
int64_t finishedCount() { std::lock_guard<std::mutex> g(g_mu); load(); return g_finished; }
int64_t fastest() { std::lock_guard<std::mutex> g(g_mu); load(); return g_fastest; }
void clear() { std::lock_guard<std::mutex> g(g_mu); load(); g_days.clear(); g_sites.clear(); g_total = g_finished = g_fastest = 0; saveLocked(); }
void save() { std::lock_guard<std::mutex> g(g_mu); if (g_loaded && g_dirty) saveLocked(); }

} // namespace stats
} // namespace ddm
