// Options (saved as options.json in DDM's data folder) and download categories (IDM-style folders in Downloads).
#pragma once
#include <string>
#include <vector>

namespace ddm {

struct Category {
    std::string key;          // "compressed"
    std::string name;         // "Compressed"
    std::string folder;       // full path; "" = <Downloads>\<name>
    std::vector<std::string> exts;
    bool rememberPath = true;
};

struct Settings {
    // Connection
    int connections = 32;              // per download (1–32)
    int maxActive = 4;                 // downloads running at once
    int speedLimitKBps = 0;            // 0 = no limit (all downloads together)
    bool useAllAdapters = true;        // spread connections over Wi-Fi + Ethernet + hotspot when more than one is online
    std::vector<std::string> excludedAdapters;
    std::string proxyMode = "system";  // "system" | "none" | "http" | "socks5" | "tor" | "manual" (address below)
    std::string proxyHost;
    int proxyPort = 0;
    std::string proxyAddress;          // manual: "http://host:port" or "socks5://host:port"
    int retries = 20;

    // Behaviour
    std::string theme = "system";      // "system" | "light" | "dark"
    std::string language;              // "" = Windows' language (when DDM has it), "en", "bn", "pt-BR"…
    bool showPrompt = true;            // "Download File Info" for new downloads
    bool showProgress = true;          // per-download progress window
    bool showComplete = true;          // "Download complete" window (silent)
    bool startWithWindows = true;
    bool closeToTray = true;
    bool minimizeToTray = true;
    bool catchBrowser = true;
    std::string browserTypes = "3GP 7Z AAC ACE AIF APK ARJ ASF AVI BIN BZ2 EXE GZ GZIP IMG ISO LZH M4A M4V MKV MOV MP3 MP4 MPA MPE MPEG MPG MSI MSU OGG OGV PDF PLJ PPS PPT QT R0* R1* RA RAR RM RMVB SEA SIT SITX TAR TIF TIFF WAV WEBM WMA WMV Z ZIP ZST XZ DMG DEB RPM MSIX APPX CAB TORRENT FLAC OPUS";
    std::vector<std::string> skipSites;
    int minCatchKB = 0;

    // Window
    int winX = -1, winY = -1, winW = 1180, winH = 720;
    bool winMax = false;
    int treeWidth = 236;
    bool hideTree = false;            // View → Hide categories
    std::string manualUserAgent;      // user agent for downloads you add yourself ("" = DDM's usual one, a current Chrome)
    int manualUserAgentPreset = 0;    // which of Options → Connection's choices (the last one: your own)
    std::vector<int> columnWidths;
    std::string sortColumn = "added";
    bool sortDesc = true;


    // More (all options of DDM 3.x)
    int uploadLimitKBps = 0; // torrents: upload limit (0 = none)
    int torrentConnections = 200; // peers per torrent
    int uploadSlots = 8;
    int activeTorrents = 4; // torrents running at once
    bool seedAfterFinish = false; // keep sharing finished torrents
    bool sequentialDefault = false; // download torrent pieces in order (preview while downloading)
    bool dustEffect = true;             // removing downloads and closing / minimizing download windows turns them to dust
    // Torrents (Options → Torrents): the built-in BitTorrent engine
    int btPort = 0;                     // incoming connections port (0 = chosen once at random, then kept)
    bool btUpnp = true;                 // open the port on the router (UPnP and NAT-PMP)
    bool btDht = true, btPex = true, btLsd = true, btUtp = true;   // DHT network, peer exchange, local peers, µTP
    std::string btEncryption = "prefer"; // "prefer" | "require" | "off"
    bool btAnonymous = false;           // don't tell peers and trackers which program this is
    int btMaxConnections = 500;         // all torrents together
    int btRatioLimitPct = 0;            // stop sharing at this ratio ×100 (150 = 1.5); 0 = never
    int btSeedMinutes = 0;              // stop sharing after this long; 0 = never
    bool btPreallocate = false;         // reserve the whole size on the disk when a torrent starts
    bool btFirstLast = false;           // first and last pieces of each file first (previews)
    std::string btMoveTo;               // move finished torrents to this folder ("" = leave them)
    std::string btExtraTrackers;        // added to every new torrent (one per line)
    std::string btIpFilter;             // blocklist file (eMule .dat or PeerGuardian .p2p)
    std::string btWatchDir;             // .torrent files put here are added by themselves
    bool btProxyPeers = true;           // with a proxy set: peers go through it too
    bool autoRetry = true; // retry failed downloads by themselves
    bool autoResumeOnStart = true; // continue unfinished downloads when DDM starts
    bool listBackupEnabled = false;
    int listBackupHours = 6; // 3, 6 or 24
    std::string queueDone = "notify"; // "notify" | "sleep" | "shutdown" | "nothing" when the queue finishes
    std::string downloadFolder = ""; // "" = Windows Downloads folder
    std::string partsFolder = "";    // unfinished downloads ("" = DDM's data folder\temp)
    bool autoExtract = false; // unpack finished archives
    bool clipboardMonitor = true; // offer links copied to the clipboard
    std::string videoQuality = "ask"; // "ask" | "best" | "1080" | "720" | "480" | "mp3"
    bool catchVideos = true; // offer videos playing in the browser
    std::string proxyUser = "";
    std::string proxyPass = "";
    int torPort = 9150;
    bool scheduleEnabled = false;
    int scheduleStartMin = 60; // minutes after midnight
    int scheduleStopMin = 420;
    bool scheduleOnlyCharging = false;
    std::string scheduleDays = "1111111"; // Mon..Sun
    bool condCharging = false; // only download while charging
    int condMinBattery = 0;
    bool linkCheck = true; // check links before downloading
    bool autoVerifyHash = true;
    bool autoStripMetadata = false;
    bool autoScanPrograms = false;
    bool scanEveryProgramSignature = true;
    std::string vtApiKey = "";
    // sounds: off unless turned on; each event "off" | "windows" | "custom" (a checked copy in DDM's folder)
    std::string addonBrowsers;            // browsers whose DDM add-on has talked to DDM ("chrome,firefox")
    bool addonReminderOff = false;        // "don't remind me" for installing the add-ons
    int addonRemindDay = 0;               // the day (days since 1970) the add-on reminder last showed: at most once a day
    bool otherDownloaderAsked = false;    // asked once whether DDM should handle downloads instead of another download manager
    bool soundsOn = false;
    std::string soundComplete = "windows", soundFailed = "windows", soundQueue = "windows", soundThreat = "windows";
    std::string soundCompleteName, soundFailedName, soundQueueName, soundThreatName;   // the chosen files' names (shown in Options)
    bool markOfTheWeb = true;              // finished files carry "from the internet" like a browser's (SmartScreen, Office protected view)
    bool idleBoost = true;
    bool tabLimiter = true, tabWhenDone = true, tabSecurity = true;   // tabs shown in a download's window
    bool trayOneIcon = true;       // download windows in the tray share DDM's icon (like IDM: one icon, all downloads in it); else an icon each
    bool progressToTray = false;   // a download window's – button: the tray (true) or the taskbar; its own tray button always hides it in the tray
    bool autoUpdate = true;                // new versions are downloaded and installed quietly when nothing is downloading            // a download window's – button hides it into the tray (else the taskbar)
    bool videoThumb = false, videoSubs = false;   // videos: also save the thumbnail picture / the subtitles
    std::string subLangs = "en";           // subtitles to save ("en", "en,es", "all")                 // the speed limit is lifted while nothing else uses the internet
    bool scanWithAntivirus = true; // scan every finished download with the antivirus
    std::string antivirusPath = "";
    std::string antivirusArgs = "";
    bool antivirusOnlyRisky = false; // only programs, archives and documents with macros
    bool stealthDefault = false;
    int boostCapMb = 0; // mobile hotspot data cap per download (MB, 0 = none)
    std::string folderRules = "";
    std::string listView = "details";
    bool acceptedPrivacy = false;
    bool remoteOn = false;            // Tools → Phone remote: the remote page on your home network
    std::string remoteKey;            // its secret (in the address the QR code holds)
    int remotePort = 8473;

    std::vector<Category> categories;
};

Settings& settings();
void loadSettings();
void saveSettings();

// Category of a file name: "compressed", "documents", "music", "programs", "video", "torrent", or "general".
std::string categoryFor(const std::string& fileName, bool torrent = false);
const Category* findCategory(const std::string& key);
std::string categoryName(const std::string& key);
bool isBuiltinCategory(const std::string& key);
// Your own categories (View → Add category…, or right-click the categories): their file types win over the built-in
// ones. addCategory returns the new key ("" = no name, or the name is taken).
std::string addCategory(const std::string& name, const std::string& exts);
bool renameCategory(const std::string& key, const std::string& name);
bool setCategoryTypes(const std::string& key, const std::string& exts);
bool removeCategory(const std::string& key);
// The folder new downloads of this category go to; created on first use.
std::string folderFor(const std::string& key);
// Options → Downloads → Folders by website ("youtube.com=Videos\\YouTube"): the folder for this site, or "".
std::string folderByRule(const std::string& site);

} // namespace ddm
