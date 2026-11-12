// The glue between windows, the engine and the browser: one App for the whole program.
#pragma once
#include <memory>
#include "../core/model.hpp"
#include <windows.h>
#include <map>
#include <string>
#include <vector>

namespace ddm {

// A request to download something (from the browser, the clipboard, Add URL, the command line).
struct LinkRequest {
    std::string url;
    std::string pageUrl, cookies, userAgent, referer;
    std::map<std::string, std::string> headers;
    std::string suggestedName, mime, favicon, description;
    int64_t size = -1;
    std::string source = "paste";   // "browser" | "paste" | "clipboard" | "cmdline"
    std::string kind = "http";      // "http" | "video" | "mega" | "torrent"
    std::string videoFormat;
    // videos playing in the browser
    std::string direct, directCookies, title;   // the <video> file when it's a plain one
    std::vector<std::string> streams;           // HLS/DASH addresses seen in the tab
    std::vector<SeenStream> seen;               // …with their kind, type and size (also plain video / sound files the player loaded)
    int playHeight = 0;                         // the playing video's height
    std::string topUrl;                         // the tab's address, when the player sits in a frame inside it
    int duration = 0;
    int tab = -1;                               // browser tab: one prompt per tab follows the latest video
    bool refreshChecked = false;                // already compared with downloads waiting for a refreshed link
    std::shared_ptr<void> probed;               // the browser's "Download this video" button: the VideoInfo DDM already read
    int pick = -1;                              // …and the quality chosen there (index in the offered list)
    std::string quality;                        // "best" | "mp3": chosen in the button's menu before the list arrived
    std::string pageData;                       // what the browser itself read about this video (TikTok: the post's data), when the site hides it from DDM
};

namespace ui {
class MainWindow;
}

class App {
public:
    static App& get();
    int run(HINSTANCE inst, const std::wstring& cmdLine);

    // Things windows ask for.
    void newDownload(const LinkRequest& r);              // shows the Download File Info prompt (or starts silently)
    void browserSeen(const std::string& name);           // the DDM add-on of this browser is installed and talking
    void addUrlDialog();
    void showProgress(const std::string& id);
    void closeProgress(const std::string& id);
    void showComplete(const std::string& id);
    void showOptions();
    void showHelp();
    void showAbout();
    void openFile(const std::string& id);
    void openFolder(const std::string& id);
    void setTheme(const std::string& mode);              // "light" | "dark" | "system"
    void toggleTheme();
    void restoreMain();
    void quit();
    void retheme();                                      // repaint every open window after a theme change
    void checkUpdatesNow(HWND owner);
    void settingsChanged();                              // Options → OK: clipboard watcher etc.                    // Help → Check for updates
    void offerUpdate(bool manual, bool atStart = false); // a newer DDM was found (at start: asked every time DDM starts)
    void checkUpdatesIfStale();                          // DDM came back from the tray: looks again if it's been a while
    static void openUrl(const std::string& url);

    // Favicons for the Source column (fetched once per site, cached on disk).
    void wantFavicon(const std::string& site, const std::string& url = "");

    HWND mainHwnd() const;
    ui::MainWindow* main() { return mainWnd; }

    // Messages posted to the main window.
    static const UINT WM_ENGINE = WM_APP + 1;     // lParam: new std::string* id, wParam: event
    static const UINT WM_TRAY = WM_APP + 2;
    static const UINT WM_LINK = WM_APP + 3;       // lParam: new LinkRequest*
    static const UINT WM_FAVICON = WM_APP + 4;    // lParam: new std::string* site
    static const UINT WM_UPDATE = WM_APP + 5;     // wParam: 1 = asked by the user
    static const UINT WM_LINKS = WM_APP + 47;     // lParam: new ui::LinkBatch* (the links / images picker)

private:
    App() = default;
    ui::MainWindow* mainWnd = nullptr;
    std::vector<HWND> openWindows;
    friend class ui::MainWindow;
};

} // namespace ddm
