#include "settings.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <algorithm>
#include <mutex>

namespace ddm {

namespace {
Settings g_settings;
std::mutex g_mu;

std::vector<Category> defaultCategories() {
    auto c = [](const char* key, const char* name, const char* exts) {
        Category k;
        k.key = key; k.name = name;
        for (auto& e : split(exts, ' ')) if (!e.empty()) k.exts.push_back(e);
        return k;
    };
    return {
        c("compressed", "Compressed", "zip rar r00 r01 arj gz tgz sit sitx sea ace bz2 7z tar xz lzh lha zst cab z lz lzma"),
        c("documents", "Documents", "doc docx pdf ppt pptx pps ppsx xls xlsx csv txt rtf odt ods odp epub mobi djvu key pages numbers md"),
        c("music", "Music", "mp3 wav wma mpa ram ra aac aif aiff m4a flac ogg oga opus mid midi amr"),
        c("programs", "Programs", "exe msi msu msix msixbundle appx appxbundle apk xapk dmg pkg deb rpm bat cmd iso img bin"),
        c("video", "Video", "avi mpg mpe mpeg asf wmv mov qt rm rmvb mp4 flv m4v webm ogv ogm mkv ts mts m2ts 3gp 3g2 f4v vob divx"),
        c("torrent", "Torrents", ""),
    };
}

Json catToJson(const Category& c) {
    Json j = Json::object();
    j["key"] = c.key; j["name"] = c.name; j["folder"] = c.folder; j["remember"] = c.rememberPath;
    Json e = Json::array();
    for (auto& x : c.exts) e.push(x);
    j["exts"] = e;
    return j;
}
} // namespace

Settings& settings() { return g_settings; }

void loadSettings() {
    std::lock_guard<std::mutex> g(g_mu);
    Settings s;
    s.categories = defaultCategories();
    std::string text;
    if (readFile(joinPath(dataDir(), "options.json"), text)) {
        Json j = Json::parse(text);
        auto I = [&](const char* k, int& v) { if (j[k].isNumber()) v = j[k].i32(); };
        auto B = [&](const char* k, bool& v) { if (j[k].isBool()) v = j[k].boolean(); };
        auto S = [&](const char* k, std::string& v) { if (j[k].isString()) v = j[k].str(); };
        I("connections", s.connections); I("maxActive", s.maxActive); I("speedLimitKBps", s.speedLimitKBps); I("retries", s.retries);
        B("useAllAdapters", s.useAllAdapters); S("proxyMode", s.proxyMode); S("proxyAddress", s.proxyAddress); S("proxyHost", s.proxyHost); I("proxyPort", s.proxyPort);
        S("theme", s.theme); S("language", s.language); B("showPrompt", s.showPrompt); B("showProgress", s.showProgress); B("showComplete", s.showComplete);
        B("startWithWindows", s.startWithWindows); B("closeToTray", s.closeToTray); B("minimizeToTray", s.minimizeToTray);
        B("catchBrowser", s.catchBrowser); S("browserTypes", s.browserTypes); I("minCatchKB", s.minCatchKB);
        I("winX", s.winX); I("winY", s.winY); I("winW", s.winW); I("winH", s.winH); B("winMax", s.winMax); I("treeWidth", s.treeWidth); B("hideTree", s.hideTree); S("manualUA", s.manualUserAgent); I("manualUAPreset", s.manualUserAgentPreset);
        S("sortColumn", s.sortColumn); B("sortDesc", s.sortDesc);
        I("uploadLimitKBps", s.uploadLimitKBps); I("torrentConnections", s.torrentConnections); I("uploadSlots", s.uploadSlots); I("activeTorrents", s.activeTorrents); B("seedAfterFinish", s.seedAfterFinish); B("sequentialDefault", s.sequentialDefault); B("dustEffect", s.dustEffect); I("btPort", s.btPort); B("btUpnp", s.btUpnp); B("btDht", s.btDht); B("btPex", s.btPex); B("btLsd", s.btLsd); B("btUtp", s.btUtp); S("btEncryption", s.btEncryption); B("btAnonymous", s.btAnonymous); I("btMaxConnections", s.btMaxConnections); I("btRatioLimitPct", s.btRatioLimitPct); I("btSeedMinutes", s.btSeedMinutes); B("btPreallocate", s.btPreallocate); B("btFirstLast", s.btFirstLast); S("btMoveTo", s.btMoveTo); S("btExtraTrackers", s.btExtraTrackers); S("btIpFilter", s.btIpFilter); S("btWatchDir", s.btWatchDir); B("btProxyPeers", s.btProxyPeers); B("autoRetry", s.autoRetry); B("autoResumeOnStart", s.autoResumeOnStart); B("listBackupEnabled", s.listBackupEnabled); I("listBackupHours", s.listBackupHours); S("queueDone", s.queueDone); S("downloadFolder", s.downloadFolder); S("partsFolder", s.partsFolder); B("autoExtract", s.autoExtract); B("clipboardMonitor", s.clipboardMonitor); S("videoQuality", s.videoQuality); B("catchVideos", s.catchVideos); S("proxyUser", s.proxyUser); S("proxyPass", s.proxyPass); I("torPort", s.torPort); B("scheduleEnabled", s.scheduleEnabled); I("scheduleStartMin", s.scheduleStartMin); I("scheduleStopMin", s.scheduleStopMin); B("scheduleOnlyCharging", s.scheduleOnlyCharging); S("scheduleDays", s.scheduleDays); B("condCharging", s.condCharging); I("condMinBattery", s.condMinBattery); B("linkCheck", s.linkCheck); B("autoVerifyHash", s.autoVerifyHash); B("autoStripMetadata", s.autoStripMetadata); B("autoScanPrograms", s.autoScanPrograms); B("scanEveryProgramSignature", s.scanEveryProgramSignature); S("vtApiKey", s.vtApiKey); B("scanWithAntivirus", s.scanWithAntivirus); B("markOfTheWeb", s.markOfTheWeb); B("soundsOn", s.soundsOn); S("addonBrowsers", s.addonBrowsers); B("addonReminderOff", s.addonReminderOff); I("addonRemindDay", s.addonRemindDay); B("otherDownloaderAsked", s.otherDownloaderAsked); S("soundComplete", s.soundComplete); S("soundFailed", s.soundFailed); S("soundQueue", s.soundQueue); S("soundThreat", s.soundThreat); S("soundCompleteName", s.soundCompleteName); S("soundFailedName", s.soundFailedName); S("soundQueueName", s.soundQueueName); S("soundThreatName", s.soundThreatName); B("idleBoost", s.idleBoost); B("tabLimiter", s.tabLimiter); B("tabWhenDone", s.tabWhenDone); B("tabSecurity", s.tabSecurity); B("progressMinToTray", s.progressToTray); B("trayOneIcon", s.trayOneIcon);   /* 4.2.1: its own key (the window now has a tray button; – goes to the taskbar unless chosen) */ B("autoUpdate", s.autoUpdate); B("videoThumb", s.videoThumb); B("videoSubs", s.videoSubs); S("subLangs", s.subLangs); B("antivirusOnlyRisky", s.antivirusOnlyRisky); S("antivirusPath", s.antivirusPath); S("antivirusArgs", s.antivirusArgs); B("stealthDefault", s.stealthDefault); I("boostCapMb", s.boostCapMb); S("folderRules", s.folderRules); S("listView", s.listView); B("acceptedPrivacy", s.acceptedPrivacy); B("remoteOn", s.remoteOn); S("remoteKey", s.remoteKey); I("remotePort", s.remotePort);
        for (auto& v : j["excludedAdapters"].list()) s.excludedAdapters.push_back(v.str());
        for (auto& v : j["skipSites"].list()) s.skipSites.push_back(v.str());
        for (auto& v : j["columnWidths"].list()) s.columnWidths.push_back(v.i32());
        // Categories: saved folders / extensions win; new built-in categories are kept.
        for (auto& cj : j["categories"].list()) {
            std::string key = cj["key"].str();
            auto it = std::find_if(s.categories.begin(), s.categories.end(), [&](const Category& c) { return c.key == key; });
            Category c;
            if (it != s.categories.end()) c = *it;
            c.key = key;
            if (cj["name"].isString()) c.name = cj["name"].str();
            c.folder = cj["folder"].str();
            c.rememberPath = cj["remember"].boolean(true);
            if (cj["exts"].isArray()) { c.exts.clear(); for (auto& e : cj["exts"].list()) c.exts.push_back(lower(e.str())); }
            if (it != s.categories.end()) *it = c; else if (!key.empty()) s.categories.push_back(c);
        }
    }
    s.connections = std::max(1, std::min(32, s.connections));
    s.maxActive = std::max(1, std::min(20, s.maxActive));
    g_settings = s;
}

void saveSettings() {
    std::lock_guard<std::mutex> g(g_mu);
    const Settings& s = g_settings;
    Json j = Json::object();
    j["connections"] = s.connections; j["maxActive"] = s.maxActive; j["speedLimitKBps"] = s.speedLimitKBps; j["retries"] = s.retries;
    j["useAllAdapters"] = s.useAllAdapters; j["proxyMode"] = s.proxyMode; j["proxyAddress"] = s.proxyAddress; j["proxyHost"] = s.proxyHost; j["proxyPort"] = s.proxyPort;
    j["theme"] = s.theme; j["language"] = s.language; j["showPrompt"] = s.showPrompt; j["showProgress"] = s.showProgress; j["showComplete"] = s.showComplete;
    j["startWithWindows"] = s.startWithWindows; j["closeToTray"] = s.closeToTray; j["minimizeToTray"] = s.minimizeToTray;
    j["catchBrowser"] = s.catchBrowser; j["browserTypes"] = s.browserTypes; j["minCatchKB"] = s.minCatchKB;
    j["winX"] = s.winX; j["winY"] = s.winY; j["winW"] = s.winW; j["winH"] = s.winH; j["winMax"] = s.winMax; j["treeWidth"] = s.treeWidth; j["hideTree"] = s.hideTree; j["manualUA"] = s.manualUserAgent; j["manualUAPreset"] = s.manualUserAgentPreset;
    j["sortColumn"] = s.sortColumn; j["sortDesc"] = s.sortDesc;
    j["uploadLimitKBps"] = s.uploadLimitKBps; j["torrentConnections"] = s.torrentConnections; j["uploadSlots"] = s.uploadSlots; j["activeTorrents"] = s.activeTorrents; j["seedAfterFinish"] = s.seedAfterFinish; j["sequentialDefault"] = s.sequentialDefault; j["dustEffect"] = s.dustEffect; j["btPort"] = s.btPort; j["btUpnp"] = s.btUpnp; j["btDht"] = s.btDht; j["btPex"] = s.btPex; j["btLsd"] = s.btLsd; j["btUtp"] = s.btUtp; j["btEncryption"] = s.btEncryption; j["btAnonymous"] = s.btAnonymous; j["btMaxConnections"] = s.btMaxConnections; j["btRatioLimitPct"] = s.btRatioLimitPct; j["btSeedMinutes"] = s.btSeedMinutes; j["btPreallocate"] = s.btPreallocate; j["btFirstLast"] = s.btFirstLast; j["btMoveTo"] = s.btMoveTo; j["btExtraTrackers"] = s.btExtraTrackers; j["btIpFilter"] = s.btIpFilter; j["btWatchDir"] = s.btWatchDir; j["btProxyPeers"] = s.btProxyPeers; j["autoRetry"] = s.autoRetry; j["autoResumeOnStart"] = s.autoResumeOnStart; j["listBackupEnabled"] = s.listBackupEnabled; j["listBackupHours"] = s.listBackupHours; j["queueDone"] = s.queueDone; j["downloadFolder"] = s.downloadFolder; j["partsFolder"] = s.partsFolder; j["autoExtract"] = s.autoExtract; j["clipboardMonitor"] = s.clipboardMonitor; j["videoQuality"] = s.videoQuality; j["catchVideos"] = s.catchVideos; j["proxyUser"] = s.proxyUser; j["proxyPass"] = s.proxyPass; j["torPort"] = s.torPort; j["scheduleEnabled"] = s.scheduleEnabled; j["scheduleStartMin"] = s.scheduleStartMin; j["scheduleStopMin"] = s.scheduleStopMin; j["scheduleOnlyCharging"] = s.scheduleOnlyCharging; j["scheduleDays"] = s.scheduleDays; j["condCharging"] = s.condCharging; j["condMinBattery"] = s.condMinBattery; j["linkCheck"] = s.linkCheck; j["autoVerifyHash"] = s.autoVerifyHash; j["autoStripMetadata"] = s.autoStripMetadata; j["autoScanPrograms"] = s.autoScanPrograms; j["scanEveryProgramSignature"] = s.scanEveryProgramSignature; j["vtApiKey"] = s.vtApiKey; j["scanWithAntivirus"] = s.scanWithAntivirus; j["markOfTheWeb"] = s.markOfTheWeb; j["soundsOn"] = s.soundsOn; j["addonBrowsers"] = s.addonBrowsers; j["addonReminderOff"] = s.addonReminderOff; j["addonRemindDay"] = s.addonRemindDay; j["otherDownloaderAsked"] = s.otherDownloaderAsked; j["soundComplete"] = s.soundComplete; j["soundFailed"] = s.soundFailed; j["soundQueue"] = s.soundQueue; j["soundThreat"] = s.soundThreat; j["soundCompleteName"] = s.soundCompleteName; j["soundFailedName"] = s.soundFailedName; j["soundQueueName"] = s.soundQueueName; j["soundThreatName"] = s.soundThreatName; j["idleBoost"] = s.idleBoost; j["tabLimiter"] = s.tabLimiter; j["tabWhenDone"] = s.tabWhenDone; j["tabSecurity"] = s.tabSecurity; j["progressMinToTray"] = s.progressToTray; j["trayOneIcon"] = s.trayOneIcon; j["autoUpdate"] = s.autoUpdate; j["videoThumb"] = s.videoThumb; j["videoSubs"] = s.videoSubs; j["subLangs"] = s.subLangs; j["antivirusOnlyRisky"] = s.antivirusOnlyRisky; j["antivirusPath"] = s.antivirusPath; j["antivirusArgs"] = s.antivirusArgs; j["stealthDefault"] = s.stealthDefault; j["boostCapMb"] = s.boostCapMb; j["folderRules"] = s.folderRules; j["listView"] = s.listView; j["acceptedPrivacy"] = s.acceptedPrivacy; j["remoteOn"] = s.remoteOn; j["remoteKey"] = s.remoteKey; j["remotePort"] = s.remotePort;
    Json ea = Json::array(); for (auto& x : s.excludedAdapters) ea.push(x); j["excludedAdapters"] = ea;
    Json ss = Json::array(); for (auto& x : s.skipSites) ss.push(x); j["skipSites"] = ss;
    Json cw = Json::array(); for (auto x : s.columnWidths) cw.push(x); j["columnWidths"] = cw;
    Json cats = Json::array(); for (auto& c : s.categories) cats.push(catToJson(c)); j["categories"] = cats;
    writeFileAtomic(joinPath(dataDir(), "options.json"), j.dump(true));
}

std::string categoryFor(const std::string& fileName, bool torrent) {
    if (torrent) return "torrent";
    std::string e = extOf(fileName);
    if (e.empty()) return "general";
    for (int pass = 0; pass < 2; pass++)   // your own categories first, then the built-in ones
        for (auto& c : g_settings.categories) {
            if (isBuiltinCategory(c.key) != (pass == 1)) continue;
            for (auto& x : c.exts)
                if (x == e || (x.size() >= 2 && x.back() == '*' && startsWith(e, x.substr(0, x.size() - 1)))) return c.key;
        }
    // split archives: .r05, .part3.rar, .001
    if (e.size() == 3 && e[0] == 'r' && isdigit(static_cast<unsigned char>(e[1]))) return "compressed";
    if (e.size() == 3 && isdigit(static_cast<unsigned char>(e[0])) && isdigit(static_cast<unsigned char>(e[1]))) return "compressed";
    return "general";
}

bool isBuiltinCategory(const std::string& key) {
    static const char* b[] = {"compressed", "documents", "music", "programs", "video", "torrent", "general"};
    for (auto k : b) if (key == k) return true;
    return false;
}

namespace {
std::vector<std::string> typesFrom(const std::string& text) {   // "psd, .ai; SVG" -> psd ai svg
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] { if (!cur.empty() && cur.size() <= 12 && std::find(out.begin(), out.end(), cur) == out.end()) out.push_back(cur); cur.clear(); };
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (isalnum(c) || ch == '*' || ch == '_' || ch == '-') cur += static_cast<char>(tolower(c));
        else flush();
    }
    flush();
    return out;
}
std::string cleanName(const std::string& n0) {
    std::string n = trim(sanitizeFileName(trim(n0)));
    if (n.size() > 40) n = n.substr(0, 40);
    return trim(n);
}
} // namespace

std::string addCategory(const std::string& name0, const std::string& exts) {
    std::string name = cleanName(name0);
    if (name.empty() || iequals(name, "General") || iequals(name, "All downloads")) return "";
    for (auto& c : g_settings.categories) if (iequals(c.name, name)) return "";
    Category c;
    c.key = "cat-" + std::to_string(nowMs());
    c.name = name;
    c.exts = typesFrom(exts);
    // placed after the built-in file categories (before Torrents)
    auto at = std::find_if(g_settings.categories.begin(), g_settings.categories.end(), [](const Category& x) { return x.key == "torrent"; });
    g_settings.categories.insert(at, c);
    return c.key;
}
bool renameCategory(const std::string& key, const std::string& name0) {
    std::string name = cleanName(name0);
    if (name.empty() || isBuiltinCategory(key)) return false;
    for (auto& c : g_settings.categories) if (c.key != key && iequals(c.name, name)) return false;
    for (auto& c : g_settings.categories) if (c.key == key) { c.name = name; return true; }
    return false;
}
bool setCategoryTypes(const std::string& key, const std::string& exts) {
    for (auto& c : g_settings.categories) if (c.key == key) { c.exts = typesFrom(exts); return true; }
    return false;
}
bool removeCategory(const std::string& key) {
    if (isBuiltinCategory(key)) return false;
    auto& v = g_settings.categories;
    auto it = std::find_if(v.begin(), v.end(), [&](const Category& c) { return c.key == key; });
    if (it == v.end()) return false;
    v.erase(it);
    return true;
}

const Category* findCategory(const std::string& key) {
    for (auto& c : g_settings.categories) if (c.key == key) return &c;
    return nullptr;
}

std::string categoryName(const std::string& key) {
    auto c = findCategory(key);
    return c ? c->name : "General";
}

std::string folderByRule(const std::string& site0) {
    std::string site = lower(site0);
    if (startsWith(site, "www.")) site = site.substr(4);
    if (site.empty()) return "";
    std::string base = trim(g_settings.downloadFolder).empty() ? downloadsDir() : g_settings.downloadFolder;
    for (auto line : split(replaceAll(g_settings.folderRules, "\r", ""), '\n')) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string rs = lower(trim(line.substr(0, eq))), folder = trim(line.substr(eq + 1));
        if (startsWith(rs, "www.")) rs = rs.substr(4);
        if (rs.empty() || folder.empty()) continue;
        if (site == rs || endsWith(site, "." + rs)) {
            std::string dir = (folder.size() > 1 && (folder[1] == ':' || startsWith(folder, "\\\\"))) ? folder : joinPath(base, folder);
            makeDirs(dir);
            return dir;
        }
    }
    return "";
}

std::string folderFor(const std::string& key) {
    std::string base = trim(g_settings.downloadFolder).empty() ? downloadsDir() : g_settings.downloadFolder;
    auto c = findCategory(key);
    std::string dir = (c && !c->folder.empty()) ? c->folder : (c ? joinPath(base, c->name) : base);
    makeDirs(dir);
    return dir;
}

} // namespace ddm
