#include "siterules.hpp"
#include "crypto.hpp"
#include "http.hpp"
#include "../common/util.hpp"
#include "../common/version.hpp"
#include <mutex>

namespace ddm {
namespace siterules {

namespace {
std::mutex g_mu;
Json g_rules = Json::object();
int g_version = 0;
bool g_loaded = false;
int64_t g_checked = 0;

std::string cachePath() { return joinPath(dataDir(), "sites.json"); }

// signed by DedSec, and a rules document
bool accept(const std::string& json, const std::string& sig, Json& out, int& ver) {
    crypto::Bytes s = crypto::unb64(trim(sig));
    if (s.empty() || !(crypto::verifyConfig(json.data(), json.size(), s) || crypto::verifyDdm(json.data(), json.size(), s))) return false;
    std::string j = json;
    if (j.size() >= 3 && static_cast<unsigned char>(j[0]) == 0xEF) j = j.substr(3);
    out = Json::parse(j);
    if (!out.isObject() || !out["rules"].isArray()) return false;
    ver = out["version"].i32();
    return ver > 0;
}

void loadCached() {   // g_mu held
    if (g_loaded) return;
    g_loaded = true;
    // the copy installed with DDM, and the newer one fetched since (whichever is newer)
    for (const std::string& path : {joinPath(exeDir(), "sites.json"), cachePath()}) {
        std::string json, sig;
        if (!readFile(path, json) || !readFile(path + ".sig", sig)) continue;
        Json o;
        int v = 0;
        if (accept(json, sig, o, v)) { if (v > g_version) { g_rules = o; g_version = v; } }
        else logLine("site rules: " + path + " isn't signed by DedSec; ignored");
    }
}
} // namespace

Json rules() { std::lock_guard<std::mutex> g(g_mu); loadCached(); return g_rules; }
int version() { std::lock_guard<std::mutex> g(g_mu); loadCached(); return g_version; }

bool loadFile(const std::string& path) {
    std::string json, sig;
    if (!readFile(path, json) || !readFile(path + ".sig", sig)) return false;
    Json o;
    int v = 0;
    if (!accept(json, sig, o, v)) return false;
    std::lock_guard<std::mutex> g(g_mu);
    g_loaded = true;
    g_rules = o;
    g_version = v;
    return true;
}

bool refresh(double maxAgeHours) {
    {
        std::lock_guard<std::mutex> g(g_mu);
        loadCached();
        if (g_checked && nowMs() - g_checked < static_cast<int64_t>(maxAgeHours * 3600 * 1000)) return false;
        g_checked = nowMs();
    }
    std::string stamp = std::to_string(nowMs() / 3600000);
    const std::pair<std::string, std::string> sources[] = {
        {"https://github.com/" DDM_UPDATE_REPO "/releases/latest/download/sites.json", "https://github.com/" DDM_UPDATE_REPO "/releases/latest/download/sites.json.sig"},
        {"https://dedseec.com/ddm/sites.json?t=" + stamp, "https://dedseec.com/ddm/sites.json.sig?t=" + stamp},
        {"https://github.com/" DDM_UPDATE_REPO_OLD "/releases/latest/download/sites.json", "https://github.com/" DDM_UPDATE_REPO_OLD "/releases/latest/download/sites.json.sig"},
    };
    for (auto& src : sources) {
        RequestInfo r;
        r.url = src.first;
        std::string json, sig;
        if (http::get(r, json, nullptr, 2 * 1024 * 1024, 20) != 200) continue;
        r.url = src.second;
        if (http::get(r, sig, nullptr, 8 * 1024, 20) != 200) continue;
        Json o;
        int v = 0;
        if (!accept(json, sig, o, v)) { logLine("site rules: not signed by DedSec (" + src.first + ")"); continue; }
        std::lock_guard<std::mutex> g(g_mu);
        if (v <= g_version) return false;   // (never goes back to older rules)
        g_rules = o;
        g_version = v;
        writeFileAtomic(cachePath(), json);
        writeFileAtomic(cachePath() + ".sig", sig);
        logLine("site rules: version " + std::to_string(v));
        return true;
    }
    return false;
}

} // namespace siterules
} // namespace ddm
