// Dialog windows: the Download File Info prompt, the progress window, Download complete, and small question boxes.
#pragma once
#include "../core/integration.hpp"
#include "app.hpp"
#include "../core/media.hpp"
#include "widgets.hpp"
#include <memory>

namespace ddm {
namespace ui {

// A themed top-level window with Esc/Enter handling and an optional modal loop.
class Dialog : public Wnd {
public:
    bool createDialog(HWND owner, const std::string& title, int w, int h, bool resizable = false, bool minimizeBox = false);
    int runModal();                    // returns `result`
    int result = -1;
    void close(int r) { result = r; DestroyWindow(h); }
    std::function<void()> onEnter;
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
    virtual void layout() {}
    virtual void paintBackground(HDC dc, RECT r);
    HWND owner = nullptr;
    bool modal = false;
    bool sizable = false;
    SIZE minSize{0, 0};            // the window can't be made smaller than it opens (or than the screen allows)
};

int askBox(HWND owner, const std::string& title, const std::string& text, const std::vector<std::string>& buttons, int defaultButton);
bool inputBox(HWND owner, const std::string& title, const std::string& label, std::string& value);

// Licence

// "notify" | "sleep" | "hibernate" | "shutdown": sleep / hibernate / shut down wait 60 seconds so it can be cancelled.
void powerAction(HWND owner, const std::string& action, bool force = false, const std::string& heading = "");
void hangUpModem();
void showScheduler(HWND owner);
// 4.0.9: play while downloading, playlists and channels, subscriptions, phone remote (media_wnd.cpp)
void showPlayer(HWND owner, const std::string& taskId);
void showPlaylist(HWND owner, const std::string& url, const std::string& cookies);
void askPlaylist(HWND owner);
void showSubscriptions(HWND owner);
void showRemote(HWND owner);
void showFind(HWND owner);
void showVault(HWND owner);                        // Tools → Private vault
void moveToVault(HWND owner, const std::vector<std::string>& ids);   // finished downloads into the vault                         // Ctrl+F
void findAgain(HWND owner, bool forward);          // F3 / Shift+F3
void showHelpTopic(HWND owner, const std::string& topic);   // the built-in help, opened at a topic

// X-Ray: look inside an archive / disc image / torrent / program before downloading it
struct XRayRequest {
    std::string url;                 // a link, or a file on this PC (local)
    bool local = false;
    std::string referer, cookies, userAgent, name;
    std::map<std::string, std::string> headers;
    std::string torrentData;         // a torrent's bytes (already at hand)
    std::string selectFiles;         // out: torrent files chosen ("1,3,4")
};
void showXRay(HWND owner, const XRayRequest& r, std::function<void(int, const XRayRequest&)> done);   // done(1) saved / chosen, done(2) whole file
void showXRayFor(HWND owner, const std::string& taskId);
void warmXRay(const XRayRequest& r);   // starts reading what's inside now, so X-Ray opens at once
void fileTool(HWND owner, const std::string& tool, std::string path);   // one tool on one file: "compress-video", "mp3", "smaller-audio", "trim", "gif", "compress-picture", "remove-bg", "extract", "zip", "strip-meta" ("" path = ask)
void showFileTools(HWND owner);
void showAddonReminder(HWND owner, const std::vector<integration::BrowserInfo>& missing);   // "Add DDM to your browser"
std::vector<integration::BrowserInfo> browsersWithoutAddon();     // Tools → File tools
void showStatistics(HWND owner);    // Tools → Statistics   // the selected download, or ask for a link / file

// The qualities DDM offers for a video (the same list in Download Video and in the browser's button), and their text.
std::vector<VideoFormat> offeredFormats(VideoInfo& vi, const LinkRequest& req);
std::string formatName(const VideoFormat& f);

void showPrompt(const LinkRequest& r);            // Download File Info
void showMegaFolder(const LinkRequest& r);       // a shared MEGA folder: every file in it

// "Download all links / images / selected links with DDM": ONE window lists them all with tick boxes
struct LinkBatchItem { std::string url, text, kind; };   // kind: the browser's hint (a, img, video, audio)
struct LinkBatch {
    std::string mode;                    // "links" | "images" | "selection"
    std::string page, cookies, ua;
    std::vector<LinkBatchItem> items;
};
void showLinksPicker(const LinkBatch& b);

// Torrents: details (General, Files with priorities, Trackers, Peers, its own options) and making a new .torrent
void showTorrentWindow(HWND owner, const std::string& taskId);
void showCreateTorrent(HWND owner, const std::string& source);
void showAddUrl(HWND owner);
void showProgressWindow(const std::string& id);
void closeProgressWindow(const std::string& id);
bool hasProgressWindow(const std::string& id);
void refreshProgressWindows();
void refreshProgressTabs();                                   // Options changed which tabs a download window shows
std::vector<std::pair<std::string, std::string>> progressInTray();   // download windows hidden in the tray: (id, "name — 45%")
bool restoreProgressFromTray();                               // shows them again; false = there were none
void showCompleteWindow(const std::string& id);
void verifyChecksumDialog(HWND owner, const std::string& id);   // compare a finished file with a checksum (MD5…SHA-512, CRC-32)
void verifyDone(HWND owner, LPARAM l);                           // (main window, WM_APP + 45)
void refreshLinkDialog(HWND owner, const std::string& id);   // Refresh Download Link (IDM-style, keeps every byte)
bool refreshTake(const LinkRequest& r);                      // a new link while a refresh waits: is it that file?
void refreshProbed(LPARAM l);                                // (main window, WM_APP + 44)
void propertiesDialog(HWND owner, const std::string& id);
void showAboutWindow(HWND owner);
void showOptionsWindow(HWND owner, int tab = 0);
std::string pickFolder(HWND owner, const std::string& start);
std::string pickFile(HWND owner, const std::string& filter, bool save = false, const std::string& name = "");   // filter "Videos|*.mp4;*.mkv|All files|*.*"
void showHelpWindow(HWND owner);
void retintAll();                                 // repaint every dialog after a theme change

} // namespace ui
} // namespace ddm
