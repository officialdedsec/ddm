// Options: every DDM setting, on IDM-style tabs (General, Connection, Save to, Downloads, Torrents, Browser, Data).
#include "../core/torrent.hpp"
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/http.hpp"
#include "../core/integration.hpp"
#include "../core/listvault.hpp"
#include "../core/antivirus.hpp"
#include "../core/sound.hpp"
#include "../core/settings.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <memory>
#include <thread>

namespace ddm {
namespace ui {

std::string pickFile(HWND owner, const std::string& filter, bool save, const std::string& name) {
    std::wstring f;
    {   // "Videos|*.mp4|All files|*.*": the descriptions in the user's language
        size_t i = 0;
        int part = 0;
        while (i <= filter.size()) {
            size_t e = filter.find('|', i);
            std::string piece = filter.substr(i, e == std::string::npos ? std::string::npos : e - i);
            f += W(part % 2 == 0 ? i18n::tr(piece) : piece);
            if (e == std::string::npos) break;
            f.push_back(L'|');
            i = e + 1;
            part++;
        }
    }
    for (auto& c : f) if (c == L'|') c = 0;
    f.push_back(0);
    f.push_back(0);
    std::wstring file = W(name);
    file.resize(MAX_PATH * 4);
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = owner;
    of.lpstrFilter = f.c_str();
    of.lpstrFile = &file[0];
    of.nMaxFile = static_cast<DWORD>(file.size());
    of.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
    return ok ? U(file.c_str()) : "";
}

namespace {

struct Row {
    Wnd* label = nullptr;     // left column (may be null: control spans the width)
    Wnd* ctl = nullptr;
    int width = 0;            // 0 = to the right edge
    int height = 0;           // 0 = one line
    Wnd* side = nullptr;      // a small button to the right ("…", "Test")
    int sideW = 0;
    std::string section;      // a heading drawn above this row
    int indent = 0;
    RECT headRect{};
    std::function<bool()> when;   // shown only while this is true
};

class OptionsWnd : public Dialog {
public:
    int tab = 0;
    std::vector<std::string> tabNames = {"General", "Connection", "Save to", "Downloads", "Torrents", "Browser", "Security", "Data"};
    std::vector<RECT> tabRects;
    std::vector<std::vector<Row>> pages;
    std::vector<std::unique_ptr<Wnd>> owned;
    Settings s;
    Button ok, cancel;

    // controls that apply() reads
    DropDown *uaPick{};
    Field *uaCustom{};
    DropDown* language{};
    std::vector<std::string> langCodes;   // the dropdown's languages ("" = Windows' language)
    DropDown *theme{}, *quality{}, *conns{}, *maxActive{}, *limit{}, *proxyMode{}, *cat{}, *backupEvery{};
    CheckBox *startWin{}, *prompt{}, *progress{}, *complete{}, *closeTray{}, *minTray{}, *allAdapters{}, *autoRetry{}, *autoResume{}, *autoExtract{};
    CheckBox *seed{}, *sequential{}, *catchBrowser{}, *catchVideos{}, *clipboard{}, *backup{};
    Field *proxyHost{}, *proxyPort{}, *proxyUser{}, *proxyPass{}, *torPort{}, *boostCap{}, *folder{}, *exts{}, *baseFolder{}, *partsDir{}, *rules{};
    Field *peers{}, *activeTorrents{}, *upload{}, *types{}, *minKB{}, *skip{};
    Field *btRatio{}, *btMinutes{}, *btMoveTo{}, *btPort{}, *btMaxConn{}, *btFilter{}, *btWatch{}, *btTrackers{};
    CheckBox *btFirstLast{}, *btUpnp{}, *btDht{}, *btPex{}, *btLsd{}, *btUtp{}, *btAnon{}, *btProxyPeers{}, *btPrealloc{};
    DropDown* btEnc{};
    static std::string fmtRatio(int pct) { char b[32]; snprintf(b, sizeof b, "%d.%02d", pct / 100, pct % 100); std::string r = b; while (r.back() == '0') r.pop_back(); if (r.back() == '.') r.pop_back(); return r; }
    static std::string btStatusLine() { std::string st = bt::engineStatus(); return "Now: " + st; }
    Label* testResult{};
    CheckBox* scan{};
    CheckBox* stealth{};
    // sounds: one row per event (No sound / Windows' sound / your .wav / Choose a .wav file…) with a ▶ button
    struct SoundRow { sound::Event ev; DropDown* dd = nullptr; std::string mode, name; };
    std::vector<SoundRow> soundRows;
    CheckBox* soundsOn{};
    static std::vector<std::string> soundOptions(const SoundRow& r) {
        std::vector<std::string> o = {"No sound", "Windows' sound"};
        if (r.mode == "custom" || (!r.name.empty() && sound::hasCustom(r.ev))) o.push_back(r.name.empty() ? "My sound" : r.name);
        o.push_back("Choose a .wav file…");
        return o;
    }
    static int soundIndex(const SoundRow& r) { return r.mode == "off" ? 0 : r.mode == "custom" && sound::hasCustom(r.ev) ? 2 : 1; }
    void addSound(sound::Event ev, const std::string& label, const std::string& mode, const std::string& name) {
        soundRows.push_back({ev, nullptr, mode, name});
        size_t i = soundRows.size() - 1;
        if (soundRows[i].mode == "custom" && !sound::hasCustom(ev)) soundRows[i].mode = "windows";
        auto* d = drop(0, label, soundOptions(soundRows[i]), soundIndex(soundRows[i]), S(300));
        soundRows[i].dd = d;
        pages[0].back().when = [this] { return soundsOn->checked; };
        auto b = make<Button>(); b->create(h, "", BtnStyle::Normal, "M7 5l12 7-12 7z");
        b->onClick = [this, i] { auto& r = soundRows[i]; if (r.mode != "off") sound::preview(r.ev, r.mode); };
        tooltip(h, b->hwnd(), "Play it");
        pages[0].back().side = b; pages[0].back().sideW = S(34);
        d->onChange = [this, i](int sel) {
            auto& r = soundRows[i];
            int last = static_cast<int>(r.dd->options.size()) - 1;
            if (sel == 0) r.mode = "off";
            else if (sel == 1) r.mode = "windows";
            else if (sel < last) r.mode = "custom";
            else {   // a new .wav: checked, then copied into DDM's own folder
                std::string f = pickFile(h, "WAV sounds (*.wav)|*.wav");
                std::string err;
                if (!f.empty() && sound::import(r.ev, f, err)) { r.mode = "custom"; r.name = baseName(f); sound::preview(r.ev, "custom"); }
                else if (!f.empty()) askBox(h, "Sound", err, {"OK"}, 0);
            }
            r.dd->options = soundOptions(r);
            r.dd->select(soundIndex(r));
            r.dd->redraw();
        };
    }
    CheckBox *motwBox{}, *verifyBox{}, *idleBox{};
    CheckBox *tabLimit{}, *tabDone{}, *tabSec{}, *linkBox{}, *thumbBox{}, *subsBox{}, *autoUpd{};
    DropDown* minTarget{};
    DropDown* trayIcons{};
    CheckBox* dustBox{};
    Field* subLangs{};
    DropDown *scanWhat{}, *avWhich{};
    Field *avPath{}, *avArgs{};
    std::vector<int> connVals = {1, 2, 4, 8, 12, 16, 24, 32};
    std::vector<int> limitVals = {0, 128, 256, 512, 1024, 2048, 5120, 10240, 20480};

    template <class T> T* make() { owned.push_back(std::make_unique<T>()); return static_cast<T*>(owned.back().get()); }
    Label* lbl(const std::string& t) { auto l = make<Label>(); l->create(h, t); l->setColor(P.muted); return l; }
    Label* note(const std::string& t) { auto l = make<Label>(); l->create(h, t); l->small = true; l->wrap = true; l->setColor(P.muted); return l; }
    void add(int page, Row r) { pages[static_cast<size_t>(page)].push_back(r); }
    CheckBox* check(int page, const std::string& t, bool v, const std::string& section = "", int indent = 0) {
        auto c = make<CheckBox>(); c->create(h, t, v);
        Row r; r.ctl = c; r.height = S(24); r.section = section; r.indent = indent; add(page, r); return c;
    }
    DropDown* drop(int page, const std::string& label, const std::vector<std::string>& opts, int sel, int width, const std::string& section = "") {
        auto d = make<DropDown>(); d->create(h, opts, std::max(0, sel));
        Row r; r.label = lbl(label); r.ctl = d; r.width = width; r.section = section; add(page, r); return d;
    }
    Field* field(int page, const std::string& label, const std::string& v, int width, int height = 0, bool multi = false, const std::string& section = "") {
        auto f = make<Field>(); f->create(h, v, multi);
        Row r; r.label = label.empty() ? nullptr : lbl(label); r.ctl = f; r.width = width; r.height = height; r.section = section; add(page, r); return f;
    }
    Button* button(int page, const std::string& label, const std::string& text, const std::string& icon, std::function<void()> click, const std::string& section = "") {
        auto b = make<Button>(); b->create(h, text, BtnStyle::Normal, icon); b->onClick = click;
        Row r; r.label = label.empty() ? nullptr : lbl(label); r.ctl = b; r.width = b->preferredWidth(); r.section = section; add(page, r); return b;
    }
    void addNote(int page, const std::string& t, int height = 0) { Row r; r.ctl = note(t); r.height = height ? height : S(18); r.indent = 0; add(page, r); }

    void build() {
        s = settings();
        pages.resize(tabNames.size());
        // ------------------------------------------------ General
        {   // every language DDM has; their names in their own language, so anyone finds theirs
            std::vector<std::string> names;
            std::string sys = i18n::systemLanguage();
            std::string sysName = "English";
            auto langs = i18n::languages();
            for (auto& l : langs) if (l.code == sys) sysName = l.name;
            names.push_back("Same as Windows (" + sysName + ")");
            langCodes.push_back("");
            int sel = 0;
            for (auto& l : langs) {
                names.push_back(l.code == "en" ? l.name : l.name + "  ·  " + l.english);
                langCodes.push_back(l.code);
                if (!s.language.empty() && l.code == s.language) sel = static_cast<int>(langCodes.size()) - 1;
            }
            language = drop(0, "Language", names, sel, S(300));
        }
        theme = drop(0, "Appearance", {"Same as Windows", "Light", "Dark"}, s.theme == "light" ? 1 : s.theme == "dark" ? 2 : 0, S(220));
        static const char* qv[] = {"ask", "best", "1080", "720", "480", "360", "mp3"};
        int qs = 0;
        for (int i = 0; i < 7; i++) if (s.videoQuality == qv[i]) qs = i;
        quality = drop(0, "Video quality", {"Ask every time", "Best", "1080p", "720p", "480p", "360p", "Sound only (MP3)"}, qs, S(220));
        startWin = check(0, "Start DDM with Windows (in the tray)", s.startWithWindows, "Behaviour");
        prompt = check(0, "Show \"Download File Info\" for new downloads", s.showPrompt);
        progress = check(0, "Show the progress window when a download starts", s.showProgress);
        complete = check(0, "Show \"Download complete\" when a download finishes", s.showComplete);
        closeTray = check(0, "The close button (X) keeps DDM running in the tray", s.closeToTray);
        minTray = check(0, "Minimize to the tray", s.minimizeToTray);
        autoUpd = check(0, "Update DDM by itself (new versions install quietly when nothing is downloading)", s.autoUpdate);
        minTarget = drop(0, "A download window's – button", {"Hides it in the tray", "Minimizes it to the taskbar"}, s.progressToTray ? 0 : 1, S(260), "Download windows");
        trayIcons = drop(0, "Download windows in the tray", {"Share DDM's icon (like IDM)", "Get an icon each"}, s.trayOneIcon ? 0 : 1, S(260));
        tabLimit = check(0, "Show the \"Speed limiter\" tab", s.tabLimiter);
        tabDone = check(0, "Show the \"When done\" tab", s.tabWhenDone);
        tabSec = check(0, "Show the \"Security\" tab (link check, live SHA-256, antivirus)", s.tabSecurity);
        dustBox = check(0, "Dust effect when removing downloads and closing or minimizing download windows", s.dustEffect);
        thumbBox = check(0, "Save the video's thumbnail picture next to it (.jpg)", s.videoThumb, "Videos");
        subsBox = check(0, "Save the video's subtitles next to it (.srt)", s.videoSubs);
        subLangs = field(0, "Subtitle languages (en, es, … or all)", s.subLangs, S(200));
        pages[0].back().when = [this] { return subsBox->checked; };
        subsBox->onChange = [this](bool) { showTab(0); };
        soundsOn = check(0, "Play sounds", s.soundsOn, "Sounds");
        soundsOn->onChange = [this](bool) { showTab(0); };
        addSound(sound::Event::Complete, "A download finishes", s.soundComplete, s.soundCompleteName);
        addSound(sound::Event::Failed, "A download fails", s.soundFailed, s.soundFailedName);
        addSound(sound::Event::QueueDone, "All downloads are done", s.soundQueue, s.soundQueueName);
        addSound(sound::Event::Threat, "The antivirus finds a threat", s.soundThreat, s.soundThreatName);
        // ------------------------------------------------ Connection
        std::vector<std::string> cn;
        int cs = 7;
        for (size_t i = 0; i < connVals.size(); i++) { cn.push_back(std::to_string(connVals[i])); if (connVals[i] == s.connections) cs = static_cast<int>(i); }
        conns = drop(1, "Connections per download", cn, cs, S(120));
        std::vector<std::string> mx;
        for (int i = 1; i <= 10; i++) mx.push_back(std::to_string(i));
        maxActive = drop(1, "Downloads at the same time", mx, std::max(0, std::min(9, s.maxActive - 1)), S(120));
        std::vector<std::string> ln;
        int ls = 0;
        for (size_t i = 0; i < limitVals.size(); i++) { ln.push_back(!limitVals[i] ? "No limit" : limitVals[i] < 1024 ? std::to_string(limitVals[i]) + " KB/s" : std::to_string(limitVals[i] / 1024) + " MB/s"); if (limitVals[i] == s.speedLimitKBps) ls = static_cast<int>(i); }
        limit = drop(1, "Speed limit (all downloads)", ln, ls, S(160));
        idleBox = check(1, "Lift the speed limit while nothing else uses the internet", s.idleBoost);
        allAdapters = check(1, "Use all network connections together (Wi-Fi + Ethernet + hotspot)", s.useAllAdapters, "Network connections");
        std::string ad;
        for (auto& a : internetAdapters()) ad += (ad.empty() ? "" : "   ·   ") + a.name + " (" + a.ip + ")";
        addNote(1, ad.empty() ? "No network connection found" : "Online now: " + ad, S(32));
        boostCap = field(1, "Daily limit on extra connections (MB)", std::to_string(s.boostCapMb), S(100));
        pages[1].back().when = [this] { return allAdapters->checked; };
        int pm = s.proxyMode == "none" ? 1 : s.proxyMode == "http" ? 2 : s.proxyMode == "socks5" ? 3 : s.proxyMode == "tor" ? 4 : 0;
        if (s.proxyMode == "manual") {   // older setting: an address in one piece
            std::string a = s.proxyAddress;
            pm = startsWith(lower(a), "socks") ? 3 : 2;
            size_t p = a.find("://");
            if (p != std::string::npos) a = a.substr(p + 3);
            size_t c = a.rfind(':');
            if (c != std::string::npos) { s.proxyHost = a.substr(0, c); s.proxyPort = atoi(a.substr(c + 1).c_str()); } else s.proxyHost = a;
        }
        proxyMode = drop(1, "Proxy", {"Use Windows' proxy settings", "No proxy", "HTTP proxy", "SOCKS5 proxy", "Tor"}, pm, S(230), "Proxy");
        auto manual = [this] { return proxyMode->selected == 2 || proxyMode->selected == 3; };
        proxyHost = field(1, "Address", s.proxyHost, S(230)); pages[1].back().when = manual;
        proxyPort = field(1, "Port", s.proxyPort > 0 ? std::to_string(s.proxyPort) : "", S(100)); pages[1].back().when = manual;
        proxyUser = field(1, "User name (optional)", s.proxyUser, S(230)); pages[1].back().when = manual;
        proxyPass = field(1, "Password (optional)", s.proxyPass, S(230)); pages[1].back().when = manual;
        SendMessageW(proxyPass->edit(), EM_SETPASSWORDCHAR, L'*', 0);
        torPort = field(1, "Tor port (9150 Tor Browser, 9050 service)", std::to_string(s.torPort), S(100)); pages[1].back().when = [this] { return proxyMode->selected == 4; };
        proxyMode->onChange = [this](int) { showTab(1); };
        allAdapters->onChange = [this](bool) { showTab(1); };
        {   // the user agent for links you add yourself (pasted, Add URL, clipboard); the browser's downloads keep the browser's
            std::vector<std::string> names;
            for (auto& p : http::userAgentPresets()) names.push_back(p.first);
            names.push_back("Your own…");
            int sel = std::max(0, std::min(static_cast<int>(names.size()) - 1, s.manualUserAgentPreset));
            uaPick = drop(1, "Identify DDM as", names, sel, S(330), "User agent (downloads you add yourself)");
            uaCustom = field(1, "Your own user agent", sel == static_cast<int>(names.size()) - 1 ? s.manualUserAgent : "", 0);
            pages[1].back().when = [this] { return uaPick->selected == static_cast<int>(http::userAgentPresets().size()); };
            uaPick->onChange = [this](int) { showTab(1); };
            addNote(1, "Used for links you paste, add with Add URL or copy. Downloads that come from your browser keep the browser's own. Some sites give other files (or none) to phones, other browsers or download tools.", S(48));
        }
        auto test = button(1, "", "Test the connection", "wifi", [this] { testConnection(); });
        (void)test;
        testResult = note("");
        { Row r; r.ctl = testResult; r.height = S(18); add(1, r); }
        // ------------------------------------------------ Save to
        std::vector<std::string> cats;
        for (auto& c : s.categories) cats.push_back(c.name);
        cat = drop(2, "Category", cats, 0, S(220));
        cat->iconName = "folder";
        folder = field(2, "Save files to", "", 0);
        {
            auto b = make<Button>(); b->create(h, "…");
            b->onClick = [this] { std::string f = pickFolder(h, folder->get()); if (!f.empty()) folder->set(f); };
            pages[2].back().side = b; pages[2].back().sideW = S(30);
        }
        exts = field(2, "File types", "", 0, S(96), true);
        addNote(2, "Extensions separated by spaces. Each category's folder is made inside Downloads the first time it's needed.", S(34));
        cat->onChange = [this](int) { loadCat(); };
        folder->onChange = [this] { saveCat(); };
        exts->onChange = [this] { saveCat(); };
        loadCat();
        // ------------------------------------------------ Downloads
        baseFolder = field(3, "Download folder", s.downloadFolder.empty() ? downloadsDir() : s.downloadFolder, 0);
        {
            auto b = make<Button>(); b->create(h, "…");
            b->onClick = [this] { std::string f = pickFolder(h, baseFolder->get()); if (!f.empty()) baseFolder->set(f); };
            pages[3].back().side = b; pages[3].back().sideW = S(30);
        }
        addNote(3, "Files are sorted into category folders inside it (Compressed, Documents, Music, Programs, Video, Torrents).", S(34));
        partsDir = field(3, "Folder for unfinished downloads", s.partsFolder.empty() ? joinPath(dataDir(), "temp") : s.partsFolder, 0);
        {
            auto b = make<Button>(); b->create(h, "…");
            b->onClick = [this] { std::string f = pickFolder(h, partsDir->get()); if (!f.empty()) partsDir->set(f); };
            pages[3].back().side = b; pages[3].back().sideW = S(30);
        }
        addNote(3, "Downloads stay here until they finish, then go to their folder: your folders only ever hold finished files.", S(34));
        autoRetry = check(3, "Retry automatically when the connection drops", s.autoRetry, "While downloading");
        autoResume = check(3, "Continue unfinished downloads when DDM starts", s.autoResumeOnStart);
        autoExtract = check(3, "Extract archives after downloading (ZIP, 7z, RAR — into a folder next to the archive)", s.autoExtract);
        rules = field(3, "", s.folderRules, 0, S(84), true, "Folders by website");
        addNote(3, "One rule per line: site=folder — e.g. youtube.com=Videos\\YouTube (inside the download folder, or a full path).", S(34));
        // ------------------------------------------------ Torrents
        peers = field(4, "Maximum peers per torrent", std::to_string(s.torrentConnections), S(100));
        activeTorrents = field(4, "Torrents at the same time", std::to_string(s.activeTorrents), S(100));
        upload = field(4, "Upload limit (KB/s, 0 = unlimited)", std::to_string(s.uploadLimitKBps), S(100));
        seed = check(4, "Keep sharing (seeding) after finishing", s.seedAfterFinish, "Behaviour");
        sequential = check(4, "Download in order (start watching a video before it finishes)", s.sequentialDefault);
        btFirstLast = check(4, "First and last pieces of each file first (previews)", s.btFirstLast);
        btRatio = field(4, "Stop sharing at ratio (0 = never)", s.btRatioLimitPct > 0 ? fmtRatio(s.btRatioLimitPct) : "0", S(100), 0, false, "Sharing (seeding)");
        btMinutes = field(4, "Stop sharing after (minutes, 0 = never)", std::to_string(s.btSeedMinutes), S(100));
        btMoveTo = field(4, "Move finished torrents to", s.btMoveTo, 0);
        {
            auto b = make<Button>(); b->create(h, "…");
            b->onClick = [this] { std::string f = pickFolder(h, btMoveTo->get().empty() ? folderFor("torrent") : btMoveTo->get()); if (!f.empty()) btMoveTo->set(f); };
            pages[4].back().side = b; pages[4].back().sideW = S(30);
        }
        addNote(4, "Leave it empty to keep them where they were downloaded.", S(22));
        btPort = field(4, "Port for incoming connections", std::to_string(s.btPort > 0 ? s.btPort : 20000 + static_cast<int>(GetTickCount() % 40000)), S(100), 0, false, "Connection");
        {
            auto b = make<Button>(); b->create(h, "Random");
            b->onClick = [this] { LARGE_INTEGER c; QueryPerformanceCounter(&c); btPort->set(std::to_string(20000 + static_cast<int>(c.QuadPart % 40000))); };
            pages[4].back().side = b; pages[4].back().sideW = S(90);
        }
        btUpnp = check(4, "Open the port on my router automatically (UPnP / NAT-PMP)", s.btUpnp);
        btMaxConn = field(4, "Connections, all torrents together", std::to_string(s.btMaxConnections), S(100));
        btEnc = drop(4, "Encryption", {"Allow (recommended)", "Require (only encrypted peers)", "Off"}, s.btEncryption == "require" ? 1 : s.btEncryption == "off" ? 2 : 0, S(280));
        btDht = check(4, "DHT network (find peers without trackers; needed for most magnet links)", s.btDht, "Finding peers");
        btPex = check(4, "Peer exchange (peers tell each other about more peers)", s.btPex);
        btLsd = check(4, "Find peers on my local network", s.btLsd);
        btUtp = check(4, "µTP (gentler on your internet connection while torrents run)", s.btUtp);
        btAnon = check(4, "Anonymous mode (don't tell peers and trackers which program this is)", s.btAnonymous, "Privacy");
        btProxyPeers = check(4, "When a proxy is set (Connection), peers go through it too", s.btProxyPeers);
        btFilter = field(4, "IP blocklist file (.dat / .p2p)", s.btIpFilter, 0);
        {
            auto b = make<Button>(); b->create(h, "…");
            b->onClick = [this] { std::string f = pickFile(h, "Blocklists (*.dat;*.p2p;*.txt)|*.dat;*.p2p;*.txt|All files|*.*"); if (!f.empty()) btFilter->set(f); };
            pages[4].back().side = b; pages[4].back().sideW = S(30);
        }
        btPrealloc = check(4, "Reserve the whole size on the disk when a torrent starts", s.btPreallocate, "Files");
        btWatch = field(4, "Add .torrent files put in this folder", s.btWatchDir, 0);
        {
            auto b = make<Button>(); b->create(h, "…");
            b->onClick = [this] { std::string f = pickFolder(h, btWatch->get()); if (!f.empty()) btWatch->set(f); };
            pages[4].back().side = b; pages[4].back().sideW = S(30);
        }
        btTrackers = field(4, "", s.btExtraTrackers, 0, S(84), true, "Trackers added to every new torrent");
        addNote(4, "One tracker address per line. " + btStatusLine(), S(34));
        // ------------------------------------------------ Browser
        catchBrowser = check(5, "Take over downloads from my browsers", s.catchBrowser);
        catchVideos = check(5, "Show the \"Download this video\" button on videos in the browser", s.catchVideos);
        clipboard = check(5, "Watch the clipboard: copied links to files, magnets and MEGA open the download window", s.clipboardMonitor);
        types = field(5, "File types to catch", s.browserTypes, 0, S(64), true);
        minKB = field(5, "Don't catch files smaller than (KB)", std::to_string(s.minCatchKB), S(100));
        std::string sk;
        for (auto& x : s.skipSites) sk += (sk.empty() ? "" : "\r\n") + x;
        skip = field(5, "Never catch on these sites", sk, 0, S(52), true);
        addNote(5, "Hold Alt while clicking a link to let the browser download it instead.");
        bool firstBrowser = true;
        for (auto& b : integration::browsers()) {
            if (!b.installed) continue;
            std::string key = b.key;
            std::string url = integration::listingFor(key);
            button(5, b.name, "Get the extension", "open", [key, url] { if (!integration::openInBrowser(key, url)) App::openUrl(url); }, firstBrowser ? "Browsers on this PC" : "");   // opens in that browser
            firstBrowser = false;
        }
        button(5, "Browser can't reach DDM?", "Repair", "refresh", [this] { integration::registerHost(); askBox(h, "Repair", "Done. Restart your browser.", {"OK"}, 0); }, "Repair");
        // ------------------------------------------------ Security
        scan = check(6, "Scan downloaded files with the antivirus when they finish", s.scanWithAntivirus, "Antivirus");
        scanWhat = drop(6, "What to scan", {"Every downloaded file", "Programs, archives and documents only"}, s.antivirusOnlyRisky ? 1 : 0, S(300));
        std::string active = antivirus::activeName();
        std::string def = active.empty() ? (antivirus::defenderPath().empty() ? "The antivirus Windows uses" : "Microsoft Defender") : active;
        avWhich = drop(6, "Antivirus", {def + " (Windows' antivirus)", "Another antivirus program…"}, trim(s.antivirusPath).empty() ? 0 : 1, S(380));
        avPath = field(6, "Antivirus program (.exe)", s.antivirusPath, 0);
        {
            auto b = make<Button>(); b->create(h, "…");
            b->onClick = [this] { std::string f = pickFile(h, "Antivirus program (*.exe)|*.exe|All files|*.*"); if (!f.empty()) avPath->set(f); };
            pages[6].back().side = b; pages[6].back().sideW = S(30);
            pages[6].back().when = [this] { return avWhich->selected == 1; };
        }
        avArgs = field(6, "Command-line options (%1 = the file)", s.antivirusArgs, 0);
        pages[6].back().when = [this] { return avWhich->selected == 1; };
        addNote(6, "The scan runs in the background right after a file is saved, before you open it. A file with a threat is marked in red in the list and you get a message; if your antivirus removes it, DDM tells you.", S(48));
        avWhich->onChange = [this](int) { showTab(6); };
        linkBox = check(6, "Check every link and file before and while downloading", s.linkCheck, "Link & site check");
        addNote(6, "Brand-new sites, look-alike names (micros0ft…), sites spreading malware, programs disguised as pictures or documents. The result shows as a coloured button in Download File Info and in the download window's Security tab. Nothing about your downloads is sent anywhere: DDM only asks a public registry when a site was registered.", S(66));
        motwBox = check(6, "Mark downloaded files as coming from the internet, like your browser does (SmartScreen, Protected View)", s.markOfTheWeb, "Downloaded files");
        verifyBox = check(6, "Check each file's checksum (SHA-256, MD5…) when the server publishes one", s.autoVerifyHash);
        stealth = check(6, "Put finished downloads straight into the private vault (encrypted, removed from the folder)", s.stealthDefault, "Private vault");
        button(6, "Private vault", "Open the vault…", "M7 11V8a5 5 0 0 1 10 0v3M5 11h14v10H5z", [this] { showVault(h); });
        // ------------------------------------------------ Data
        backup = check(7, "Back up the download list automatically (encrypted .ddm — only DDM can open it)", s.listBackupEnabled, "Your download list");
        backupEvery = drop(7, "Back up every", {"3 hours", "6 hours", "1 day"}, s.listBackupHours == 3 ? 0 : s.listBackupHours == 24 ? 2 : 1, S(160));
        button(7, "Backups", "Back up now and open the folder", "folder", [] { Engine::get().backupNow(); ShellExecuteW(nullptr, L"open", W(Engine::get().backupFolder()).c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
        button(7, "Export the list", "Export…", "down", [this] { exportList(); });
        button(7, "Import a list", "Import…", "plus", [this] { importList(); });
        button(7, "Export settings", "Export…", "down", [this] { exportSettings(); }, "Settings");
        button(7, "Import settings", "Import…", "plus", [this] { importSettings(); });
        button(7, "DDM's data folder", "Open", "folder", [] { ShellExecuteW(nullptr, L"open", W(dataDir()).c_str(), nullptr, nullptr, SW_SHOWNORMAL); });

        ok.create(h, "OK", BtnStyle::Primary);
        cancel.create(h, "Cancel");
        ok.onClick = [this] { if (applyAll()) close(0); };
        cancel.onClick = [this] { close(1); };
        onEnter = [this] { if (GetFocus() && GetWindowLongW(GetFocus(), GWL_STYLE) & ES_MULTILINE) return; if (applyAll()) close(0); };
        makeViewport();
        showTab(tab);
    }

    // The page's rows live in one clipped child window: scrolling moves them all at once (one blit, no flicker), rows
    // can be half in view, and the wheel glides instead of jumping.
    HWND page = nullptr;
    int scrollTarget = 0;
    static LRESULT CALLBACK pageProc(HWND hw, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<OptionsWnd*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
        switch (m) {
            case WM_NCCREATE: SetWindowLongPtrW(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams)); break;
            case WM_ERASEBKGND: return 1;
            case WM_DDM_BG: return bgReply(P.win);
            case WM_MOUSEWHEEL: case WM_VSCROLL: if (self) return SendMessageW(self->h, m, w, l); break;
            case WM_PAINT: {
                if (!self) break;
                Paint p(hw);
                HDC dc = p.dc();
                RECT r = p.rc();
                fill(dc, r, P.win);
                if (self->tab >= 0 && self->tab < static_cast<int>(self->pages.size()))
                    for (auto& row : self->pages[static_cast<size_t>(self->tab)])
                        if (!row.section.empty() && (!row.when || row.when()) && row.headRect.bottom > row.headRect.top) {
                            RECT hr = row.headRect;
                            OffsetRect(&hr, 0, -self->scrollY);
                            if (hr.bottom < 0 || hr.top > r.bottom) continue;
                            text(dc, row.section, hr, P.text, fontBold());
                            RECT ln{hr.left + textSize(dc, row.section, fontBold()).cx + S(10), (hr.top + hr.bottom) / 2, hr.right, (hr.top + hr.bottom) / 2 + 1};
                            fill(dc, ln, P.line);
                        }
                return 0;
            }
        }
        return DefWindowProcW(hw, m, w, l);
    }
    void makeViewport() {
        static bool reg = false;
        if (!reg) {
            WNDCLASSEXW wc{sizeof wc};
            wc.lpfnWndProc = pageProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"DDMOptionsPage";
            RegisterClassExW(&wc);
            reg = true;
        }
        page = CreateWindowExW(WS_EX_CONTROLPARENT, L"DDMOptionsPage", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, h, nullptr, GetModuleHandleW(nullptr), this);
        if (!page) return;
        for (auto& pg : pages)
            for (auto& r : pg) for (Wnd* w : {r.label, r.ctl, r.side}) if (w && w->hwnd()) SetParent(w->hwnd(), page);
        SetWindowPos(page, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    bool loadingCat = false;
    void loadCat() {
        loadingCat = true;
        struct Done { bool& f; ~Done() { f = false; } } done{loadingCat};
        if (cat->selected < 0 || cat->selected >= static_cast<int>(s.categories.size())) return;
        auto& c = s.categories[static_cast<size_t>(cat->selected)];
        std::string base = s.downloadFolder.empty() ? downloadsDir() : s.downloadFolder;
        folder->set(c.folder.empty() ? joinPath(base, c.name) : c.folder);
        std::string e;
        for (auto& x : c.exts) e += (e.empty() ? "" : " ") + x;
        exts->set(e);
    }
    void saveCat() {
        if (loadingCat) return;
        if (cat->selected < 0 || cat->selected >= static_cast<int>(s.categories.size())) return;
        auto& c = s.categories[static_cast<size_t>(cat->selected)];
        std::string base = s.downloadFolder.empty() ? downloadsDir() : s.downloadFolder;
        std::string f = trim(folder->get());
        c.folder = f == joinPath(base, c.name) ? "" : f;
        c.exts.clear();
        for (auto& x : split(replaceAll(replaceAll(exts->get(), "\r", " "), "\n", " "), ' ')) { std::string t = lower(trim(x)); if (!t.empty() && t[0] == '.') t = t.substr(1); if (!t.empty()) c.exts.push_back(t); }
    }

    void readProxy(Settings& o) {
        static const char* modes[] = {"system", "none", "http", "socks5", "tor"};
        o.proxyMode = modes[std::max(0, std::min(4, proxyMode->selected))];
        o.proxyHost = trim(proxyHost->get());
        o.proxyPort = atoi(proxyPort->get().c_str());
        o.proxyUser = trim(proxyUser->get());
        o.proxyPass = proxyPass->get();
        o.torPort = std::max(1, atoi(torPort->get().c_str()));
        o.proxyAddress.clear();
    }

    void testConnection() {
        Settings keep = settings();
        Settings t = keep;
        readProxy(t);
        settings() = t;   // the test uses these proxy settings
        testResult->setText("Testing…");
        testResult->setColor(P.muted);
        HWND hw = h;
        std::thread([hw, keep] {
            RequestInfo r;
            r.url = "https://www.google.com/generate_204";
            std::string body;
            int64_t t0 = nowMs();
            long st = http::get(r, body, nullptr, 4096, 20);
            int64_t ms = nowMs() - t0;
            auto* res = new std::string(st >= 200 && st < 400 ? "✓ Connected (" + std::to_string(ms) + " ms)" : "✗ No connection" + std::string(st ? " (server replied " + std::to_string(st) + ")" : " — check the proxy settings"));
            settings() = keep;
            if (!PostMessageW(hw, WM_APP + 30, 0, reinterpret_cast<LPARAM>(res))) delete res;
        }).detach();
    }

    void exportList() {
        std::string f = pickFile(h, "DDM list, encrypted (*.ddm)|*.ddm|DDM list, plain (*.json)|*.json", true, "ddm-downloads.ddm");
        if (f.empty()) return;
        std::string json = Engine::get().exportJson();
        bool ok;
        if (endsWith(lower(f), ".json")) ok = writeFileAtomic(f, json);
        else { auto b = listvault::encrypt(json); ok = writeFileAtomic(f, std::string(b.begin(), b.end())); }
        askBox(h, "Export", ok ? "The download list was saved." : "Couldn't save the file.", {"OK"}, 0);
    }
    void importList() {
        std::string f = pickFile(h, "DDM list (*.ddm;*.json)|*.ddm;*.json|All files|*.*");
        if (f.empty()) return;
        std::string data, err;
        if (!readFile(f, data)) { askBox(h, "Import", "Couldn't read the file.", {"OK"}, 0); return; }
        int n = Engine::get().importList(data, err);
        askBox(h, n < 0 ? "Couldn't import" : "Imported", n < 0 ? err : std::to_string(n) + (n == 1 ? " download added." : " downloads added."), {"OK"}, 0);
    }
    void exportSettings() {
        std::string f = pickFile(h, "DDM settings (*.json)|*.json", true, "ddm-settings.json");
        if (f.empty()) return;
        saveSettings();
        std::string data;
        readFile(joinPath(dataDir(), "options.json"), data);
        Json j = Json::parse(data);
        j.erase("proxyPass");   // secrets stay on this PC
        j.erase("vtApiKey");
        askBox(h, "Export settings", writeFileAtomic(f, j.dump(true)) ? "Settings saved." : "Couldn't save the file.", {"OK"}, 0);
    }
    void importSettings() {
        std::string f = pickFile(h, "DDM settings (*.json)|*.json|All files|*.*");
        if (f.empty()) return;
        std::string data;
        bool good = false;
        Json j = readFile(f, data) ? Json::parse(data, &good) : Json();
        if (!good || !j.isObject() || !j.has("connections")) { askBox(h, "Import settings", "This isn't a DDM settings file.", {"OK"}, 0); return; }
        Json mine = Json::parse([] { std::string d; readFile(joinPath(dataDir(), "options.json"), d); return d; }());
        for (auto k : {"winX", "winY", "winW", "winH", "winMax", "proxyPass", "vtApiKey"}) if (mine.has(k)) j[k] = mine[k];
        writeFileAtomic(joinPath(dataDir(), "options.json"), j.dump(true));
        loadSettings();
        Engine::get().applySpeedLimit();
        App::get().setTheme(settings().theme);
        i18n::setLanguage(settings().language.empty() ? i18n::systemLanguage() : settings().language);
        fontsForLanguage();
        App::get().retheme();
        askBox(h, "Import settings", "Settings imported.", {"OK"}, 0);
        close(0);
    }

    bool applyAll() {
        saveCat();
        s.theme = theme->selected == 1 ? "light" : theme->selected == 2 ? "dark" : "system";
        static const char* qv[] = {"ask", "best", "1080", "720", "480", "360", "mp3"};
        s.videoQuality = qv[std::max(0, std::min(6, quality->selected))];
        s.startWithWindows = startWin->checked; s.showPrompt = prompt->checked; s.showProgress = progress->checked; s.showComplete = complete->checked;
        s.closeToTray = closeTray->checked; s.minimizeToTray = minTray->checked;
        s.connections = connVals[static_cast<size_t>(std::max(0, conns->selected))];
        s.maxActive = maxActive->selected + 1;
        s.speedLimitKBps = limitVals[static_cast<size_t>(std::max(0, limit->selected))];
        s.useAllAdapters = allAdapters->checked;
        s.boostCapMb = std::max(0, atoi(boostCap->get().c_str()));
        readProxy(s);
        if ((s.proxyMode == "http" || s.proxyMode == "socks5") && s.proxyHost.empty()) { showTab(1); askBox(h, "Proxy", "Enter the proxy's address, or choose another proxy setting.", {"OK"}, 0); proxyHost->focus(); return false; }
        {
            auto presets = http::userAgentPresets();
            int sel = std::max(0, uaPick->selected);
            if (sel < static_cast<int>(presets.size())) { s.manualUserAgentPreset = sel; s.manualUserAgent = presets[static_cast<size_t>(sel)].second; }
            else {
                std::string ua = trim(uaCustom->get());
                if (!http::validUserAgent(ua)) {
                    showTab(1);
                    askBox(h, "User agent", "This doesn't look like a user agent. It's one line of plain characters (no line breaks), up to 512, with a name and a version, for example:\n\nMozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0.0.0 Safari/537.36", {"OK"}, 0);
                    uaCustom->focus();
                    return false;
                }
                s.manualUserAgentPreset = sel; s.manualUserAgent = ua;
            }
        }
        std::string base = trim(baseFolder->get());
        s.downloadFolder = (base.empty() || lower(base) == lower(downloadsDir())) ? "" : base;
        if (!s.downloadFolder.empty() && !makeDirs(s.downloadFolder)) { showTab(3); askBox(h, "Download folder", "DDM can't use the folder " + s.downloadFolder + ".", {"OK"}, 0); return false; }
        {
            std::string pd = trim(partsDir->get());
            s.partsFolder = (pd.empty() || iequals(pd, joinPath(dataDir(), "temp"))) ? "" : pd;
            bool full = s.partsFolder.size() > 2 && ((s.partsFolder[1] == ':' && (s.partsFolder[2] == '\\' || s.partsFolder[2] == '/')) || startsWith(s.partsFolder, "\\\\"));   // a whole path (C:\… or \\server\…)
            if (!s.partsFolder.empty() && (!full || !makeDirs(s.partsFolder))) { showTab(3); askBox(h, "Folder for unfinished downloads", "DDM can't use the folder " + s.partsFolder + ".", {"OK"}, 0); return false; }
            std::string from = partsFolder(), to = s.partsFolder.empty() ? joinPath(dataDir(), "temp") : s.partsFolder;
            if (!iequals(from, to)) {
                if (Engine::get().running() > 0) { showTab(3); askBox(h, "Folder for unfinished downloads", "Pause your downloads first, then change this folder.", {"OK"}, 0); return false; }
                makeDirs(to);
                if (!listDir(from).empty()) {   // unfinished downloads come along, so they continue where they stopped
                    std::wstring src = W(joinPath(from, "*")); src.push_back(0);
                    std::wstring dst = W(to); dst.push_back(0);
                    SHFILEOPSTRUCTW op{};
                    op.hwnd = h; op.wFunc = FO_MOVE; op.pFrom = src.c_str(); op.pTo = dst.c_str();
                    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
                    SHFileOperationW(&op);
                }
            }
            setPartsFolder(s.partsFolder);
        }
        s.autoRetry = autoRetry->checked; s.autoResumeOnStart = autoResume->checked; s.autoExtract = autoExtract->checked;
        s.folderRules = trim(replaceAll(rules->get(), "\r", ""));
        s.torrentConnections = std::max(10, std::min(2000, atoi(peers->get().c_str())));
        s.activeTorrents = std::max(1, std::min(20, atoi(activeTorrents->get().c_str())));
        s.uploadLimitKBps = std::max(0, atoi(upload->get().c_str()));
        s.seedAfterFinish = seed->checked; s.sequentialDefault = sequential->checked;
        {
            std::string r = trim(replaceAll(btRatio->get(), ",", "."));
            s.btRatioLimitPct = std::max(0, std::min(100000, static_cast<int>(atof(r.c_str()) * 100 + 0.5)));
        }
        s.btSeedMinutes = std::max(0, atoi(btMinutes->get().c_str()));
        s.btMoveTo = trim(btMoveTo->get());
        if (!s.btMoveTo.empty() && !makeDirs(s.btMoveTo)) { showTab(4); askBox(h, "Torrents", "DDM can't use the folder " + s.btMoveTo + ".", {"OK"}, 0); return false; }
        {
            int port = atoi(trim(btPort->get()).c_str());
            if (port < 1024 || port > 65535) { showTab(4); askBox(h, "Torrents", "The port must be a number from 1024 to 65535.", {"OK"}, 0); btPort->focus(); return false; }
            s.btPort = port;
        }
        s.btUpnp = btUpnp->checked;
        s.btMaxConnections = std::max(20, std::min(5000, atoi(btMaxConn->get().c_str())));
        s.btEncryption = btEnc->selected == 1 ? "require" : btEnc->selected == 2 ? "off" : "prefer";
        s.btDht = btDht->checked; s.btPex = btPex->checked; s.btLsd = btLsd->checked; s.btUtp = btUtp->checked;
        s.btAnonymous = btAnon->checked; s.btProxyPeers = btProxyPeers->checked; s.btPreallocate = btPrealloc->checked; s.btFirstLast = btFirstLast->checked;
        s.btIpFilter = trim(btFilter->get());
        if (!s.btIpFilter.empty() && !fileExists(s.btIpFilter)) { showTab(4); askBox(h, "Torrents", "The blocklist file isn't there: " + s.btIpFilter, {"OK"}, 0); return false; }
        s.btWatchDir = trim(btWatch->get());
        if (!s.btWatchDir.empty() && !dirExists(s.btWatchDir)) { showTab(4); askBox(h, "Torrents", "The folder to watch isn't there: " + s.btWatchDir, {"OK"}, 0); return false; }
        s.btExtraTrackers = trim(replaceAll(btTrackers->get(), "\r", ""));
        s.catchBrowser = catchBrowser->checked; s.catchVideos = catchVideos->checked; s.clipboardMonitor = clipboard->checked;
        s.browserTypes = trim(replaceAll(replaceAll(types->get(), "\r", " "), "\n", " "));
        for (auto& c : s.browserTypes) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        s.minCatchKB = std::max(0, atoi(minKB->get().c_str()));
        s.skipSites.clear();
        for (auto& x : split(replaceAll(skip->get(), "\r", ""), '\n')) if (!trim(x).empty()) s.skipSites.push_back(trim(x));
        s.scanWithAntivirus = scan->checked;
        s.stealthDefault = stealth->checked;
        s.soundsOn = soundsOn->checked;
        for (auto& r : soundRows) {
            std::string& m = r.ev == sound::Event::Complete ? s.soundComplete : r.ev == sound::Event::Failed ? s.soundFailed : r.ev == sound::Event::QueueDone ? s.soundQueue : s.soundThreat;
            std::string& n = r.ev == sound::Event::Complete ? s.soundCompleteName : r.ev == sound::Event::Failed ? s.soundFailedName : r.ev == sound::Event::QueueDone ? s.soundQueueName : s.soundThreatName;
            m = r.mode; n = r.name;
        }
        s.progressToTray = minTarget->selected == 0;
        s.trayOneIcon = trayIcons->selected == 0;
        s.dustEffect = dustBox->checked;
        s.autoUpdate = autoUpd->checked;
        s.tabLimiter = tabLimit->checked; s.tabWhenDone = tabDone->checked; s.tabSecurity = tabSec->checked;
        s.linkCheck = linkBox->checked;
        s.videoThumb = thumbBox->checked; s.videoSubs = subsBox->checked;
        {
            std::string l = lower(trim(replaceAll(subLangs->get(), " ", "")));
            std::string clean;
            for (char c : l) if (isalnum(static_cast<unsigned char>(c)) || c == ',' || c == '-') clean += c;
            s.subLangs = clean.empty() ? "en" : clean.substr(0, 60);
        }
        s.markOfTheWeb = motwBox->checked; s.autoVerifyHash = verifyBox->checked; s.idleBoost = idleBox->checked;
        s.antivirusOnlyRisky = scanWhat->selected == 1;
        s.antivirusPath = avWhich->selected == 1 ? trim(avPath->get()) : "";
        s.antivirusArgs = trim(avArgs->get());
        if (avWhich->selected == 1 && (s.antivirusPath.empty() || !fileExists(s.antivirusPath))) { showTab(6); askBox(h, "Antivirus", "Choose your antivirus program's .exe file, or use Windows' antivirus.", {"OK"}, 0); return false; }
        s.listBackupEnabled = backup->checked;
        s.listBackupHours = backupEvery->selected == 0 ? 3 : backupEvery->selected == 2 ? 24 : 6;
        s.language = langCodes.empty() ? s.language : langCodes[static_cast<size_t>(std::max(0, std::min(static_cast<int>(langCodes.size()) - 1, language->selected)))];
        bool themeChanged = s.theme != settings().theme;
        bool languageChanged = s.language != settings().language;
        Settings keep = settings();
        s.winX = keep.winX; s.winY = keep.winY; s.winW = keep.winW; s.winH = keep.winH; s.winMax = keep.winMax;
        s.treeWidth = keep.treeWidth; s.columnWidths = keep.columnWidths; s.sortColumn = keep.sortColumn; s.sortDesc = keep.sortDesc;
        sound::commit();   // newly chosen sounds take over now
        settings() = s;
        saveSettings();
        integration::setStartWithWindows(s.startWithWindows);
        Engine::get().applySpeedLimit();
        torrentSettingsChanged();
        if (themeChanged) App::get().setTheme(s.theme);
        if (languageChanged) {   // every window shows the new language right away
            i18n::setLanguage(s.language.empty() ? i18n::systemLanguage() : s.language);
            fontsForLanguage();
            App::get().retheme();
        }
        App::get().settingsChanged();
        refreshProgressTabs();
        return true;
    }

    int scrollY = 0;      // the page scrolls when the window is shorter than the page (big text sizes, small screens)
    void showTab(int i) {
        if (i != tab) { scrollY = 0; scrollTarget = 0; KillTimer(h, 0x51); }
        tab = i;
        for (size_t k = 0; k < pages.size(); k++)
            if (static_cast<int>(k) != i) for (auto& r : pages[k]) { if (r.label) r.label->show(false); r.ctl->show(false); if (r.side) r.side->show(false); }
        layout();
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    void layout() override {
        RECT rc = client();
        int pad = S(22), top = S(62), lw = std::min<int>(S(270), static_cast<int>(rc.right) * 2 / 5), rh = S(30), gap = S(10);
        int pageTop = S(52), pageBottom = rc.bottom - S(72);
        int right = rc.right - pad - (scrollMax > 0 ? GetSystemMetrics(SM_CXVSCROLL) : 0);
        int y = top - scrollY;
        // the rows sit in the page window (from pageTop down to pageBottom): its own coordinates, clipped at its edges
        int off = page ? pageTop : 0;
        if (page) SetWindowPos(page, nullptr, 0, pageTop, std::max(0, right + pad), std::max(0, pageBottom - pageTop), SWP_NOZORDER | SWP_NOACTIVATE);
        HDWP dw = BeginDeferWindowPos(64);
        auto place = [&](Wnd* w, int x, int yy, int ww, int hh) {
            if (dw) dw = DeferWindowPos(dw, w->hwnd(), nullptr, x, yy - off, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
            else w->move(x, yy - off, ww, hh);
        };
        if (tab >= 0 && tab < static_cast<int>(pages.size())) {
            for (auto& r : pages[static_cast<size_t>(tab)]) {
                bool on = !r.when || r.when();
                if (!on) { if (r.label) r.label->show(false); r.ctl->show(false); if (r.side) r.side->show(false); continue; }
                r.headRect = RECT{0, 0, 0, 0};
                if (!r.section.empty()) {
                    y += S(6);
                    if (page) r.headRect = RECT{pad, y - off + scrollY, right, y - off + scrollY + S(20)};   // (in the page, before scrolling)
                    else if (y >= pageTop && y + S(20) <= pageBottom) r.headRect = RECT{pad, y, right, y + S(20)};
                    y += S(26);
                }
                int hgt = r.height ? r.height : rh;
                // without the page window, rows that don't fit wait for the scroll bar; in it they're simply clipped
                bool inside = page ? true : (y >= pageTop && y + hgt <= pageBottom);
                int x = pad + r.indent;
                if (r.label) {
                    place(r.label, x, y, lw - S(8), std::min(hgt, rh));
                    r.label->show(inside);
                    x = pad + lw;
                }
                int w = r.width ? std::min(r.width, right - x - (r.side ? r.sideW + S(6) : 0)) : right - x - (r.side ? r.sideW + S(6) : 0);
                place(r.ctl, x, y, w, hgt);
                r.ctl->show(inside);
                if (r.side) { place(r.side, right - r.sideW, y, r.sideW, rh); r.side->show(inside); }
                y += hgt + gap;
            }
        }
        if (dw) EndDeferWindowPos(dw);
        if (page) InvalidateRect(page, nullptr, FALSE);
        int content = y + scrollY - top, room = pageBottom - top;
        int newMax = std::max(0, content - room);
        if (newMax != scrollMax || scrollY > newMax) {
            scrollMax = newMax;
            if (scrollY > scrollMax) scrollY = scrollMax;
            ShowScrollBar(h, SB_VERT, scrollMax > 0);
            layout();
            return;
        }
        SCROLLINFO si{sizeof si, SIF_ALL | SIF_DISABLENOSCROLL, 0, std::max(0, content - 1), static_cast<UINT>(std::max(1, room)), scrollY, 0};
        if (scrollMax > 0) SetScrollInfo(h, SB_VERT, &si, TRUE);
        cancel.move(rc.right - pad - S(96), rc.bottom - S(52), S(96), S(34));
        ok.move(rc.right - pad - S(96) * 2 - S(8), rc.bottom - S(52), S(96), S(34));
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        tabRects.clear();
        int x = S(12);
        RECT base{0, S(46) - 1, r.right, S(46)};
        fill(dc, base, P.line);
        for (size_t i = 0; i < tabNames.size(); i++) {
            bool on = static_cast<int>(i) == tab;
            SIZE sz = textSize(dc, tabNames[i], on ? fontBold() : fontUI());
            RECT tr{x, S(14), x + sz.cx + S(24), S(46)};
            if (on) { fillRound(dc, RECT{tr.left, tr.top, tr.right, tr.bottom + S(6)}, Sf(6), P.line); fillRound(dc, RECT{tr.left + 1, tr.top + 1, tr.right - 1, tr.bottom + S(6)}, Sf(6), P.win); }
            text(dc, tabNames[i], tr, on ? P.text : P.muted, on ? fontBold() : fontUI(), DT_CENTER);
            tabRects.push_back(tr);
            x = tr.right + S(2);
        }
        if (!page && tab >= 0 && tab < static_cast<int>(pages.size()))
            for (auto& row : pages[static_cast<size_t>(tab)])
                if (!row.section.empty() && (!row.when || row.when()) && row.headRect.bottom > row.headRect.top) {
                    text(dc, row.section, row.headRect, P.text, fontBold());
                    RECT ln{row.headRect.left + textSize(dc, row.section, fontBold()).cx + S(10), (row.headRect.top + row.headRect.bottom) / 2, row.headRect.right, (row.headRect.top + row.headRect.bottom) / 2 + 1};
                    fill(dc, ln, P.line);
                }
        RECT ln{0, r.bottom - S(68), r.right, r.bottom - S(67)};
        fill(dc, ln, P.line);
    }

    int scrollMax = 0;
    void scrollTo(int v) {
        v = std::max(0, std::min(scrollMax, v));
        scrollTarget = v;
        if (v == scrollY) return;
        if (page) {   // every row moves together: one blit of the page, only the newly shown strip is painted
            int dy = scrollY - v;
            scrollY = v;
            ScrollWindowEx(page, 0, dy, nullptr, nullptr, nullptr, nullptr, SW_SCROLLCHILDREN | SW_INVALIDATE);
            SCROLLINFO si{sizeof si, SIF_POS};
            si.nPos = scrollY;
            SetScrollInfo(h, SB_VERT, &si, TRUE);
            UpdateWindow(page);
            return;
        }
        scrollY = v;
        layout();
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    void glideTo(int v) {   // the wheel: a short ease to the new place (≈ 120 ms), several notches add up
        scrollTarget = std::max(0, std::min(scrollMax, v));
        if (!page) { scrollTo(scrollTarget); return; }
        SetTimer(h, 0x51, 10, nullptr);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_LBUTTONDOWN) {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (size_t i = 0; i < tabRects.size(); i++) if (PtInRect(&tabRects[i], pt)) showTab(static_cast<int>(i));
            return 0;
        }
        if (m == WM_MOUSEWHEEL && scrollMax > 0) {
            int from = KillTimer(h, 0x51) ? scrollTarget : scrollY;   // still gliding: from where it's heading
            glideTo(from - GET_WHEEL_DELTA_WPARAM(w) * S(60) / WHEEL_DELTA);
            return 0;
        }
        if (m == WM_TIMER && w == 0x51) {
            int d = scrollTarget - scrollY;
            if (d == 0) { KillTimer(h, 0x51); return 0; }
            int step = d / 3;
            if (step == 0) step = d > 0 ? 1 : -1;
            int keep = scrollTarget;
            scrollTo(scrollY + step);
            scrollTarget = keep;
            return 0;
        }
        if (m == WM_VSCROLL) {
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int v = scrollY;
            switch (LOWORD(w)) {
                case SB_LINEUP: v -= S(40); break; case SB_LINEDOWN: v += S(40); break;
                case SB_PAGEUP: v -= static_cast<int>(si.nPage); break; case SB_PAGEDOWN: v += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: v = si.nTrackPos; break;
            }
            scrollTo(v);
            return 0;
        }
        if (m == WM_APP + 30) {
            std::unique_ptr<std::string> res(reinterpret_cast<std::string*>(l));
            testResult->setText(*res);
            testResult->setColor((*res)[0] == '\xE2' && res->find("Connected") != std::string::npos ? P.good : P.bad);
            return 0;
        }
        return Dialog::proc(m, w, l);
    }
};

} // namespace

void showOptionsWindow(HWND owner, int tab) {
    OptionsWnd d;
    d.tab = tab;
    if (!d.createDialog(owner, "Options", S(720), S(640), true)) return;
    d.build();
    d.runModal();
    sound::discard();   // sounds chosen and then cancelled
}

} // namespace ui
} // namespace ddm
