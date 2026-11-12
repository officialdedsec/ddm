#include "integration.hpp"
#include "crypto.hpp"
#include "http.hpp"
#include "../common/addon_files.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <map>
#include <mutex>
#include <regex>
#include <algorithm>
#include <set>
#include <thread>

namespace ddm {
namespace integration {

namespace {

const char* kBuiltInChrome = "banekiajgcomjfnjkbpknfpgkdgfidjd";
const char* kBuiltInEdge = "ikafbphbbbilcadhbohghjjcdaghjhdi";
const char* kBuiltInFirefoxSlug = "ddm-dedsec-download-manager";

std::mutex g_mu;
std::string g_chrome = kBuiltInChrome, g_edge = kBuiltInEdge, g_ffSlug = kBuiltInFirefoxSlug;
bool g_loaded = false;

const wchar_t* kChromiumKeys[] = {
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\", L"Software\\Chromium\\NativeMessagingHosts\\", L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\",
    L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\", L"Software\\Vivaldi\\NativeMessagingHosts\\", L"Software\\Yandex\\YandexBrowser\\NativeMessagingHosts\\",
    L"Software\\Opera Software\\NativeMessagingHosts\\", L"Software\\Google\\Chrome Beta\\NativeMessagingHosts\\", L"Software\\Google\\Chrome Dev\\NativeMessagingHosts\\",
};

void setDefault(const std::wstring& key, const std::string& value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    std::wstring w = W(value);
    RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()), static_cast<DWORD>((w.size() + 1) * 2));
    RegCloseKey(k);
}

void setValue(const std::wstring& key, const wchar_t* name, const std::string& value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    std::wstring w = W(value);
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(w.c_str()), static_cast<DWORD>((w.size() + 1) * 2));
    RegCloseKey(k);
}

void deleteTree(const std::wstring& key) { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); }

std::string cacheFile() { return joinPath(joinPath(dataDir(), "browser"), "stores.json"); }

bool applyStores(const std::string& data, const std::string& sig) {
    if (!crypto::verifyConfig(data.data(), data.size(), crypto::unb64(trim(sig)))) return false;
    std::string text = data;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text = text.substr(3);
    Json o = Json::parse(text);
    static const std::regex id("^[a-p]{32}$"), slug("^[A-Za-z0-9_\\-@.]{1,80}$");
    std::string c = lower(trim(o["chrome"].str())), e = lower(trim(o["edge"].str())), f = trim(o["firefox"].str());
    if (!std::regex_match(c, id)) c = kBuiltInChrome;
    if (!std::regex_match(e, id)) e = kBuiltInEdge;
    if (!std::regex_match(f, slug)) f = kBuiltInFirefoxSlug;
    std::lock_guard<std::mutex> g(g_mu);
    bool changed = c != g_chrome || e != g_edge || f != g_ffSlug;
    g_chrome = c; g_edge = e; g_ffSlug = f;
    return changed;
}

void loadCached() {
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_loaded) return;
        g_loaded = true;
    }
    std::string d, s;
    if (readFile(cacheFile(), d) && readFile(cacheFile() + ".sig", s)) applyStores(d, s);
}

void offerExternal(const std::wstring& root, const std::string& id, const char* updateUrl) {
    if (id.empty()) return;
    setValue(root + W(id), L"update_url", updateUrl);
}

} // namespace

std::string chromeStoreId() { loadCached(); std::lock_guard<std::mutex> g(g_mu); return g_chrome; }
std::string edgeStoreId() { loadCached(); std::lock_guard<std::mutex> g(g_mu); return g_edge; }
std::string chromeListing() { return "https://chromewebstore.google.com/detail/" + chromeStoreId(); }
std::string edgeListing() { return "https://microsoftedge.microsoft.com/addons/detail/" + edgeStoreId(); }
std::string firefoxListing() { loadCached(); std::lock_guard<std::mutex> g(g_mu); return "https://addons.mozilla.org/firefox/addon/" + g_ffSlug + "/"; }

namespace {
std::mutex g_handMu;
std::vector<std::string> g_hand;   // hand-loaded copies of DDM's add-on found in the browsers

std::string fileSha256(const std::string& path) {
    crypto::Bytes h = crypto::hashFile(path, L"SHA256");
    return h.size() == 32 ? crypto::hex(h) : "";
}

// every file is one of DDM's add-on files, byte for byte (the browser's own "_metadata" folder aside)
bool isDdmAddonFolder(const std::string& dir) {
    if (!fileExists(joinPath(dir, "manifest.json"))) return false;
    int matched = 0;
    for (auto& n : listDir(dir)) {
        std::string p = joinPath(dir, n);
        DWORD a = GetFileAttributesW(W(p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (a & FILE_ATTRIBUTE_DIRECTORY) { if (n.empty() || n[0] != '_') return false; continue; }
        if (fileSize(p) > 4 * 1024 * 1024) return false;
        std::string h = fileSha256(p);
        bool ok = false;
        if (n == "manifest.json") { for (auto m : addonfiles::kManifests) if (h == m) ok = true; }
        else for (auto& f : addonfiles::kFiles) if (n == f[0] && h == f[1]) ok = true;
        if (!ok) return false;
        matched++;
    }
    return matched >= 3;
}

void scanPrefs(const std::string& prefsFile, const std::string& profileDir, std::set<std::string>& ids) {
    std::string text;
    if (!readFile(prefsFile, text) || text.size() > 64 * 1024 * 1024) return;
    Json j = Json::parse(text);
    static const std::regex id("^[a-p]{32}$");
    for (auto& kv : j["extensions"]["settings"].items()) {
        if (!std::regex_match(kv.first, id)) continue;
        std::string path = kv.second["path"].str();
        if (path.empty()) continue;
        if (!(path.size() > 2 && path[1] == ':')) path = joinPath(joinPath(profileDir, "Extensions"), path);   // installed: relative to the profile
        if (isDdmAddonFolder(path)) ids.insert(kv.first);
    }
}

std::set<std::string> findHandLoaded() {
    std::set<std::string> ids;
    wchar_t local[MAX_PATH] = {}, roaming[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, 0x1c, nullptr, 0, local);
    SHGetFolderPathW(nullptr, 0x1a, nullptr, 0, roaming);
    std::vector<std::string> userData = {
        joinPath(U(local), "Google\\Chrome\\User Data"), joinPath(U(local), "Microsoft\\Edge\\User Data"),
        joinPath(U(local), "BraveSoftware\\Brave-Browser\\User Data"), joinPath(U(local), "Vivaldi\\User Data"),
        joinPath(U(local), "Chromium\\User Data"), joinPath(U(local), "Yandex\\YandexBrowser\\User Data"),
    };
    for (auto& ud : userData) {
        if (!dirExists(ud)) continue;
        for (auto& prof : listDir(ud)) {
            if (prof != "Default" && prof.compare(0, 8, "Profile ") != 0) continue;
            std::string pd = joinPath(ud, prof);
            scanPrefs(joinPath(pd, "Secure Preferences"), pd, ids);
            scanPrefs(joinPath(pd, "Preferences"), pd, ids);
        }
    }
    for (const char* opera : {"Opera Software\\Opera Stable", "Opera Software\\Opera GX Stable"}) {   // Opera keeps its profile right there
        std::string pd = joinPath(U(roaming), opera);
        if (!dirExists(pd)) continue;
        scanPrefs(joinPath(pd, "Secure Preferences"), pd, ids);
        scanPrefs(joinPath(pd, "Preferences"), pd, ids);
    }
    return ids;
}
} // namespace

std::vector<std::string> allowedExtensionIds() {
    std::vector<std::string> v = {kChromiumId, chromeStoreId(), edgeStoreId()};
    std::lock_guard<std::mutex> g(g_handMu);
    for (auto& id : g_hand) if (std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
    return v;
}

std::vector<std::string> handLoadedAddonIds() { std::lock_guard<std::mutex> g(g_handMu); return g_hand; }

void watchHandLoadedAddons() {
    std::thread([] {
        int64_t lastStamp = -1;
        for (;;) {
            // only when a browser's settings changed (a newer write time on any profile's preferences)
            int64_t stamp = 0;
            wchar_t local[MAX_PATH] = {}, roaming[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, 0x1c, nullptr, 0, local);
            SHGetFolderPathW(nullptr, 0x1a, nullptr, 0, roaming);
            auto add = [&](const std::string& f) {
                WIN32_FILE_ATTRIBUTE_DATA d{};
                if (GetFileAttributesExW(W(f).c_str(), GetFileExInfoStandard, &d)) stamp += static_cast<int64_t>(d.ftLastWriteTime.dwLowDateTime) + (static_cast<int64_t>(d.ftLastWriteTime.dwHighDateTime) << 20);
            };
            for (const char* b : {"Google\\Chrome\\User Data", "Microsoft\\Edge\\User Data", "BraveSoftware\\Brave-Browser\\User Data", "Vivaldi\\User Data", "Chromium\\User Data", "Yandex\\YandexBrowser\\User Data"}) {
                std::string ud = joinPath(U(local), b);
                if (!dirExists(ud)) continue;
                for (auto& prof : listDir(ud)) if (prof == "Default" || prof.compare(0, 8, "Profile ") == 0) { add(joinPath(joinPath(ud, prof), "Secure Preferences")); add(joinPath(joinPath(ud, prof), "Preferences")); }
            }
            for (const char* o : {"Opera Software\\Opera Stable", "Opera Software\\Opera GX Stable"}) { add(joinPath(joinPath(U(roaming), o), "Secure Preferences")); add(joinPath(joinPath(U(roaming), o), "Preferences")); }
            if (stamp != lastStamp) {
                lastStamp = stamp;
                auto found = findHandLoaded();
                std::vector<std::string> v(found.begin(), found.end());
                bool changed;
                { std::lock_guard<std::mutex> g(g_handMu); changed = v != g_hand; g_hand = v; }
                if (changed) { registerHost(); logLine("browser add-on loaded by hand: " + std::to_string(v.size()) + " allowed"); }
            }
            Sleep(5000);
        }
    }).detach();
}

void registerHost() {
    std::string host = joinPath(exeDir(), "ddmhost.exe");
    if (!fileExists(host)) return;
    std::string dir = joinPath(dataDir(), "browser");
    makeDirs(dir);
    Json origins = Json::array();
    for (auto& id : allowedExtensionIds()) if (!id.empty()) origins.push("chrome-extension://" + id + "/");
    Json c = Json::object();
    c["name"] = kHostName; c["description"] = "DDM — DedSec Download Manager"; c["path"] = host; c["type"] = "stdio"; c["allowed_origins"] = origins;
    std::string chromeJson = joinPath(dir, std::string(kHostName) + ".json");
    writeFileAtomic(chromeJson, c.dump(true));
    Json f = Json::object();
    Json exts = Json::array();
    exts.push(kFirefoxId);
    f["name"] = kHostName; f["description"] = "DDM — DedSec Download Manager"; f["path"] = host; f["type"] = "stdio"; f["allowed_extensions"] = exts;
    std::string ffJson = joinPath(dir, std::string(kHostName) + ".firefox.json");
    writeFileAtomic(ffJson, f.dump(true));
    for (auto k : kChromiumKeys) setDefault(std::wstring(k) + W(kHostName), chromeJson);
    setDefault(L"Software\\Mozilla\\NativeMessagingHosts\\" + W(kHostName), ffJson);
    // Chrome / Edge show "New extension added — DDM" on their next start (like IDM's extension arrives).
    const char* chromeUpd = "https://clients2.google.com/service/update2/crx";
    const char* edgeUpd = "https://edge.microsoft.com/extensionwebstorebase/v1/crx";
    offerExternal(L"Software\\Google\\Chrome\\Extensions\\", chromeStoreId(), chromeUpd);
    offerExternal(L"Software\\Microsoft\\Edge\\Extensions\\", edgeStoreId(), edgeUpd);
    offerExternal(L"Software\\BraveSoftware\\Brave-Browser\\Extensions\\", chromeStoreId(), chromeUpd);
    offerExternal(L"Software\\Vivaldi\\Extensions\\", chromeStoreId(), chromeUpd);
    std::string xpi = joinPath(joinPath(exeDir(), "extension"), "ddm-firefox.xpi");
    if (fileExists(xpi)) setValue(L"Software\\Mozilla\\Firefox\\Extensions", W(kFirefoxId).c_str(), xpi);
}

void unregisterAll() {
    for (auto k : kChromiumKeys) deleteTree(std::wstring(k) + W(kHostName));
    deleteTree(L"Software\\Mozilla\\NativeMessagingHosts\\" + W(kHostName));
    deleteTree(L"Software\\Google\\Chrome\\Extensions\\" + W(chromeStoreId()));
    deleteTree(L"Software\\Microsoft\\Edge\\Extensions\\" + W(edgeStoreId()));
    deleteTree(L"Software\\BraveSoftware\\Brave-Browser\\Extensions\\" + W(chromeStoreId()));
    deleteTree(L"Software\\Vivaldi\\Extensions\\" + W(chromeStoreId()));
    deleteTree(L"Software\\Classes\\DDM.Magnet");
    deleteTree(L"Software\\DedSec\\DDM\\Capabilities");
    { HKEY r; if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0, KEY_SET_VALUE, &r) == ERROR_SUCCESS) { RegDeleteValueW(r, L"DDM"); RegCloseKey(r); } }
    {   // magnet: back to nobody, but only if it's still DDM's
        HKEY c; wchar_t v[1024] = {}; DWORD n = sizeof v;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\magnet\\shell\\open\\command", 0, KEY_READ, &c) == ERROR_SUCCESS) {
            bool ours = RegQueryValueExW(c, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(v), &n) == ERROR_SUCCESS && lower(U(v)).find("ddm.exe") != std::string::npos;
            RegCloseKey(c);
            if (ours) deleteTree(L"Software\\Classes\\magnet");
        }
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Mozilla\\Firefox\\Extensions", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) { RegDeleteValueW(k, W(kFirefoxId).c_str()); RegCloseKey(k); }
    setStartWithWindows(false);
}

void setStartWithWindows(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + W(joinPath(exeDir(), "DDM.exe")) + L"\" /tray";
        RegSetValueExW(k, L"DDM", 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * 2));
    } else RegDeleteValueW(k, L"DDM");
    RegCloseKey(k);
}

void refreshStoreConfig() {
    loadCached();
    std::string stamp = std::to_string(nowMs() / 3600000);
    RequestInfo r;
    r.url = "https://dedseec.com/ddm/stores.json?t=" + stamp;
    std::string d, s;
    if (http::get(r, d, nullptr, 64 * 1024, 20) != 200) return;
    r.url = "https://dedseec.com/ddm/stores.json.sig?t=" + stamp;
    if (http::get(r, s, nullptr, 8 * 1024, 20) != 200) return;
    if (!applyStores(d, s)) return;
    makeDirs(dirName(cacheFile()));
    writeFileAtomic(cacheFile() + ".sig", trim(s));
    writeFileAtomic(cacheFile(), d);
    registerHost();   // new store IDs: allow them too
}

std::vector<std::string> otherDownloaders() {
    struct O { const char* name; const wchar_t* path; const wchar_t* reg; };
    static const O list[] = {
        {"Internet Download Manager", L"Internet Download Manager\\IDMan.exe", L"Software\\DownloadManager"},
        {"Free Download Manager", L"Softdeluxe\\Free Download Manager\\fdm.exe", L"Software\\Softdeluxe\\Free Download Manager"},
        {"JDownloader", L"JDownloader 2.0\\JDownloader2.exe", nullptr},
        {"EagleGet", L"EagleGet\\EagleGet.exe", nullptr},
        {"Xtreme Download Manager", L"XDM\\xdman.exe", nullptr},
        {"Neat Download Manager", L"Neat Download Manager\\NeatDM.exe", nullptr},
        {"Motrix", L"Programs\\Motrix\\Motrix.exe", nullptr},
    };
    std::vector<std::string> out;
    for (auto& o : list) {
        bool found = false;
        if (o.reg) { HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, o.reg, 0, KEY_READ, &k) == ERROR_SUCCESS) { RegCloseKey(k); found = true; } }
        for (int csidl : {0x26 /*ProgramFiles*/, 0x2a /*ProgramFilesX86*/, 0x1c /*LocalAppData*/}) {
            if (found) break;
            wchar_t base[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, base)) && fileExists(joinPath(U(base), U(o.path)))) found = true;
        }
        if (found) out.push_back(o.name);
    }
    return out;
}

void claimMagnetLinks() {
    std::string exe = joinPath(exeDir(), "DDM.exe");
    std::string cmd = "\"" + exe + "\" \"%1\"";
    // DDM's own link type, listed in Windows' Default apps
    setDefault(L"Software\\Classes\\DDM.Magnet", "URL:Magnet link");
    setValue(L"Software\\Classes\\DDM.Magnet", L"URL Protocol", "");
    setDefault(L"Software\\Classes\\DDM.Magnet\\DefaultIcon", "\"" + exe + "\",0");
    setDefault(L"Software\\Classes\\DDM.Magnet\\shell\\open\\command", cmd);
    setValue(L"Software\\DedSec\\DDM\\Capabilities", L"ApplicationName", "DDM — DedSec Download Manager");
    setValue(L"Software\\DedSec\\DDM\\Capabilities", L"ApplicationDescription", "Downloads files, videos and torrents");
    setValue(L"Software\\DedSec\\DDM\\Capabilities\\URLAssociations", L"magnet", "DDM.Magnet");
    setValue(L"Software\\RegisteredApplications", L"DDM", "Software\\DedSec\\DDM\\Capabilities");
    // magnet: itself (used when Windows has no choice of its own saved for it)
    setDefault(L"Software\\Classes\\magnet", "URL:Magnet link");
    setValue(L"Software\\Classes\\magnet", L"URL Protocol", "");
    setDefault(L"Software\\Classes\\magnet\\DefaultIcon", "\"" + exe + "\",0");
    setDefault(L"Software\\Classes\\magnet\\shell\\open\\command", cmd);
}

bool openExtensionsPage(const std::string& key) {
    struct E { const char* key; const wchar_t* app; const wchar_t* page; };
    static const E list[] = {
        {"chrome", L"chrome.exe", L"chrome://extensions"}, {"edge", L"msedge.exe", L"edge://extensions"}, {"brave", L"brave.exe", L"brave://extensions"},
        {"opera", L"opera.exe", L"opera://extensions"}, {"vivaldi", L"vivaldi.exe", L"vivaldi://extensions"}, {"firefox", L"firefox.exe", L"about:addons"},
    };
    for (auto& e : list) {
        if (key != e.key) continue;
        // the browsers' own pages can't be opened like a web link: start the browser with it
        std::string exe = browserExe(key);
        HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.empty() ? e.app : W(exe).c_str(), e.page, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32;
    }
    std::string exe = browserExe(key);   // the other browsers of the same families
    if (exe.empty()) return false;
    const wchar_t* page = isFirefoxFamily(key) ? L"about:addons" : key == "yandex" ? L"browser://tune" : key == "operagx" ? L"opera://extensions" : L"chrome://extensions";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", W(exe).c_str(), page, nullptr, SW_SHOWNORMAL)) > 32;
}

bool addonInstalled(const std::string& key) {
    wchar_t local[MAX_PATH] = {}, roaming[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, 0x1c /*LocalAppData*/, nullptr, 0, local);
    SHGetFolderPathW(nullptr, 0x1a /*AppData*/, nullptr, 0, roaming);
    std::vector<std::string> ids = {chromeStoreId(), kChromiumId};
    if (key == "edge") ids.push_back(edgeStoreId());
    for (auto& i : allowedExtensionIds()) ids.push_back(i);
    auto chromium = [&](const std::string& userData) {   // every profile: Default, Profile 1, …
        if (!dirExists(userData)) return false;
        for (auto& prof : listDir(userData)) {
            if (prof != "Default" && prof.compare(0, 8, "Profile ") != 0) continue;
            std::string ext = joinPath(joinPath(userData, prof), "Extensions");
            for (auto& id : ids) if (!id.empty() && dirExists(joinPath(ext, id))) return true;
            // unpacked or from elsewhere: the profile's own list of extensions mentions DDM's native helper
            std::string prefs;
            if (readFile(joinPath(joinPath(userData, prof), "Secure Preferences"), prefs) && prefs.find("\"name\":\"DDM") != std::string::npos) return true;
        }
        return false;
    };
    std::string L = U(local), R = U(roaming);
    if (key == "chrome") return chromium(joinPath(L, "Google\\Chrome\\User Data"));
    if (key == "edge") return chromium(joinPath(L, "Microsoft\\Edge\\User Data"));
    if (key == "brave") return chromium(joinPath(L, "BraveSoftware\\Brave-Browser\\User Data"));
    if (key == "vivaldi") return chromium(joinPath(L, "Vivaldi\\User Data"));
    if (key == "yandex") return chromium(joinPath(L, "Yandex\\YandexBrowser\\User Data"));
    if (key == "chromium") return chromium(joinPath(L, "Chromium\\User Data"));
    if (key == "opera" || key == "operagx") {
        std::string ext = joinPath(R, key == "opera" ? "Opera Software\\Opera Stable\\Extensions" : "Opera Software\\Opera GX Stable\\Extensions");
        for (auto& id : ids) if (!id.empty() && dirExists(joinPath(ext, id))) return true;
        return false;
    }
    if (isFirefoxFamily(key)) {
        std::string profiles = joinPath(R, key == "firefox" ? "Mozilla\\Firefox\\Profiles" : key == "waterfox" ? "Waterfox\\Profiles" : key == "librewolf" ? "librewolf\\Profiles" : "Floorp\\Profiles");
        for (auto& p : listDir(profiles)) {
            std::string ext = joinPath(joinPath(profiles, p), "extensions");
            if (fileExists(joinPath(ext, std::string(kFirefoxId) + ".xpi")) || dirExists(joinPath(ext, kFirefoxId))) return true;
        }
        return false;
    }
    return false;
}

std::string listingFor(const std::string& key) { return isFirefoxFamily(key) ? firefoxListing() : key == "edge" ? edgeListing() : chromeListing(); }

namespace {
// The browsers DDM knows: how to recognise their program, where their profiles are, which store has the add-on.
struct Known { const char* key; const char* name; const char* exeName; const char* folderHint; const wchar_t* appPath; const wchar_t* guess; };
const Known kKnown[] = {
    {"chrome", "Google Chrome", "chrome.exe", "google\\chrome", L"chrome.exe", L"Google\\Chrome\\Application\\chrome.exe"},
    {"edge", "Microsoft Edge", "msedge.exe", "", L"msedge.exe", L"Microsoft\\Edge\\Application\\msedge.exe"},
    {"firefox", "Mozilla Firefox", "firefox.exe", "", L"firefox.exe", L"Mozilla Firefox\\firefox.exe"},
    {"brave", "Brave", "brave.exe", "", L"brave.exe", L"BraveSoftware\\Brave-Browser\\Application\\brave.exe"},
    {"opera", "Opera", "-", "", L"opera.exe", L"Programs\\Opera\\launcher.exe"},
    {"operagx", "Opera GX", "-", "", nullptr, L"Programs\\Opera GX\\launcher.exe"},
    {"vivaldi", "Vivaldi", "vivaldi.exe", "", L"vivaldi.exe", L"Vivaldi\\Application\\vivaldi.exe"},
    {"yandex", "Yandex Browser", "browser.exe", "yandex", nullptr, L"Yandex\\YandexBrowser\\Application\\browser.exe"},
    {"chromium", "Chromium", "chrome.exe", "chromium", nullptr, L"Chromium\\Application\\chrome.exe"},
    {"waterfox", "Waterfox", "waterfox.exe", "", L"waterfox.exe", L"Waterfox\\waterfox.exe"},
    {"librewolf", "LibreWolf", "librewolf.exe", "", L"librewolf.exe", L"LibreWolf\\librewolf.exe"},
    {"floorp", "Floorp", "floorp.exe", "", L"floorp.exe", L"Ablaze Floorp\\floorp.exe"},
};

std::string regDefault(HKEY root, const std::wstring& path, REGSAM extra = 0) {
    HKEY k;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | extra, &k) != ERROR_SUCCESS) return "";
    wchar_t buf[1024] = {};
    DWORD n = sizeof buf - sizeof(wchar_t), type = 0;
    std::string r;
    if (RegQueryValueExW(k, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buf), &n) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        wchar_t ex[1024] = {};
        if (type == REG_EXPAND_SZ && ExpandEnvironmentStringsW(buf, ex, 1024)) r = U(ex); else r = U(buf);
    }
    RegCloseKey(k);
    return r;
}
std::string exeFromCommand(std::string c) {   // "\"C:\\…\\chrome.exe\" --flag" -> C:\…\chrome.exe
    c = trim(c);
    if (!c.empty() && c[0] == '"') { size_t e = c.find('"', 1); return e == std::string::npos ? c.substr(1) : c.substr(1, e - 1); }
    size_t e = lower(c).find(".exe");
    return e == std::string::npos ? c : c.substr(0, e + 4);
}
const Known* knownFor(const std::string& exe) {
    std::string l = lower(exe), b = lower(baseName(exe));
    for (auto& k : kKnown) if (b == k.exeName && (!*k.folderHint || l.find(k.folderHint) != std::string::npos)) return &k;
    if (b == "chrome.exe" && l.find("chromium") == std::string::npos) return &kKnown[0];
    if ((b == "opera.exe" || b == "launcher.exe") && l.find("opera") != std::string::npos) return l.find("opera gx") != std::string::npos ? &kKnown[5] : &kKnown[4];
    return nullptr;
}

std::map<std::string, std::string> g_exes;   // browser key -> its program (found by browsers())
std::mutex g_exeMu;
} // namespace

std::vector<BrowserInfo> browsers() {
    std::map<std::string, std::string> found;   // key -> program
    // 1) every browser registered with Windows (Settings → Default apps lists the same ones)
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {static_cast<REGSAM>(KEY_WOW64_64KEY), static_cast<REGSAM>(KEY_WOW64_32KEY)}) {
            HKEY k;
            if (RegOpenKeyExW(root, L"Software\\Clients\\StartMenuInternet", 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
            wchar_t name[256];
            for (DWORD i = 0;; i++) {
                DWORD n = 256;
                if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                std::string exe = exeFromCommand(regDefault(root, L"Software\\Clients\\StartMenuInternet\\" + std::wstring(name) + L"\\shell\\open\\command", view));
                if (exe.empty() || !fileExists(exe)) continue;
                if (const Known* kn = knownFor(exe)) if (!found.count(kn->key)) found[kn->key] = exe;
            }
            RegCloseKey(k);
        }
    }
    // 2) App Paths and the usual install folders (portable or per-user installs not registered as browsers)
    for (auto& kn : kKnown) {
        if (found.count(kn.key)) continue;
        if (kn.appPath) for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            std::string exe = exeFromCommand(regDefault(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + std::wstring(kn.appPath)));
            if (!exe.empty() && fileExists(exe) && knownFor(exe) == &kn) { found[kn.key] = exe; break; }
        }
        if (found.count(kn.key)) continue;
        for (int csidl : {0x26 /*ProgramFiles*/, 0x2a /*ProgramFilesX86*/, 0x1c /*LocalAppData*/}) {
            wchar_t base[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, base))) {
                std::string exe = joinPath(U(base), U(kn.guess));
                if (fileExists(exe)) { found[kn.key] = exe; break; }
            }
        }
        if (!found.count(kn.key) && std::string(kn.key) == "firefox") {   // 64-bit Firefox seen from 32-bit DDM
            wchar_t pf[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH)) { std::string exe = joinPath(U(pf), "Mozilla Firefox\\firefox.exe"); if (fileExists(exe)) found["firefox"] = exe; }
        }
    }
    std::vector<BrowserInfo> out;
    for (auto& kn : kKnown) {
        BrowserInfo i;
        i.name = kn.name; i.key = kn.key;
        auto it = found.find(kn.key);
        i.installed = it != found.end();
        if (i.installed) i.exe = it->second;
        out.push_back(i);
    }
    { std::lock_guard<std::mutex> g(g_exeMu); g_exes = found; }
    return out;
}

std::string browserExe(const std::string& key) {
    { std::lock_guard<std::mutex> g(g_exeMu); auto it = g_exes.find(key); if (it != g_exes.end()) return it->second; }
    for (auto& b : browsers()) if (b.key == key && b.installed) return b.exe;
    return "";
}

bool isFirefoxFamily(const std::string& key) { return key == "firefox" || key == "waterfox" || key == "librewolf" || key == "floorp"; }

bool openInBrowser(const std::string& key, const std::string& url) {
    std::string exe = browserExe(key);
    if (!exe.empty()) {
        std::wstring args = L"\"" + W(url) + L"\"";
        if (isFirefoxFamily(key)) args = L"-new-tab " + args;
        HINSTANCE r = ShellExecuteW(nullptr, L"open", W(exe).c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) > 32) return true;
    }
    return false;
}

} // namespace integration
} // namespace ddm
