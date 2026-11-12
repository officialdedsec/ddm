// The main window: menu bar, toolbar, categories, download list, status bar, and the tray icon.
#pragma once
#include <set>
#include "downloadlist.hpp"
#include "widgets.hpp"
#include <shellapi.h>
#include <functional>

namespace ddm {
namespace ui {

// A strip of text menus (Tasks, File, Downloads, View, Help) that open themed popups.
class MenuStrip : public Wnd {
public:
    struct Entry { std::string title; std::function<std::vector<MenuItem>()> items; };
    std::vector<Entry> entries;
    std::function<void(UINT)> onCommand;
    bool create(HWND parent);
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
private:
    int hot = -1;
    std::vector<RECT> rects;
    void layout(HDC dc);
};

// Big icon buttons with labels.
class Toolbar : public Wnd {
public:
    struct Tool { std::string label, icon; COLORREF color; UINT id; bool sepBefore; bool enabled = true; };
    std::vector<Tool> tools;
    std::function<void(UINT)> onCommand;
    UINT highlight = 0;     // drawn with the accent wash (Add URL)
    bool create(HWND parent);
    void setEnabled(UINT id, bool on);
    void setTool(UINT id, const std::string& label, const std::string& icon, COLORREF color);   // a button that changes (Pause ↔ Resume)
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
private:
    int hot = -1, pressed = -1;
    std::vector<RECT> rects;
    void layout();
};

// Categories: All downloads (Compressed, Documents, Music, Programs, Video), Torrents, Unfinished, Finished, Queues.
class CategoryTree : public Wnd {
public:
    struct Node { std::string key, name, icon; int level; bool expandable; bool expanded; int count; };
    std::vector<Node> nodes;
    std::string selected = "all";
    std::function<void(const std::string&)> onSelect;
    std::function<void(const std::string&, POINT)> onMenu;   // right-click (key of the row, "" = empty space)
    bool create(HWND parent);
    void syncCategories();   // the file categories as Options has them (your own ones too)
    void setCounts(const std::vector<Task>& all);
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
private:
    int hot = -1;
    int rowH = 32;
    std::vector<int> visible() const;
};

class StatusBar : public Wnd {
public:
    std::string left, network, limit, speed;
    std::string note;                // a short message (Find), shown for a few seconds
    ULONGLONG noteUntil = 0;
    bool networkOk = false;
    bool create(HWND parent);
protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;
};

class MainWindow : public Wnd {
public:
    bool create();
    void refresh();                      // pull from the engine
    void showFromTray();
    void hideToTray();
    void onThemeChanged();
    DownloadList list;
    CategoryTree tree;

    // command ids
    enum : UINT {
        CmdAddUrl = 100, CmdResume, CmdPause, CmdPauseAll, CmdResumeAll, CmdDelete, CmdDeleteFinished, CmdOptions, CmdScheduler,
        CmdStartQueue, CmdStopQueue, CmdXRay, CmdGrabber, CmdExit, CmdOpen, CmdOpenFolder, CmdRedownload, CmdDeleteWithFile, CmdCopyUrl,
        CmdRefreshLink, CmdProperties, CmdThemeLight, CmdThemeDark, CmdThemeSystem, CmdToggleTheme, CmdHelp, CmdAbout, CmdShow,
        CmdLimitOff, CmdLimit256, CmdLimit512, CmdLimit1M, CmdLimit2M, CmdLimit5M, CmdLimit10M, CmdPasteUrl, CmdSelectAll, CmdShowProgress,
        CmdFileTools, CmdSecurity, CmdVault, CmdStats, CmdBrowser, CmdUpdates, CmdFind, CmdFindNext, CmdFindPrev, CmdForceStart, CmdRestart, CmdQueueRemove, CmdScan, CmdToVault, CmdVerify,
        CmdTorrentDetails, CmdSeedToggle, CmdCreateTorrent,
        CmdStop, CmdPauseResume, CmdPauseResumeAll, CmdMoveRename, CmdCopyMagnet, CmdDelTorrentFile, CmdDelTorrentAll, CmdDelTorrentData, CmdToggleTree, CmdUndoDelete, CmdAddCategory, CmdPlaylist, CmdSubscriptions, CmdRemote, CmdPlay,
        CmdPriorityBase = 900, CmdQueueBase = 1000, CmdTrayWinBase = 1200, CmdToolBase = 1300,
    };
    void command(UINT id);
    void onClipboard();
    void notify(const std::string& title, const std::string& text);   // silent tray balloon
    void nudges();       // once-only questions and the add-on reminder
    void trimMemory();   // hidden in the tray: give back memory
    void setStatusNote(const std::string& text);
    void commitPendingDelete();          // the removal waiting for Undo happens now (DDM closing, another removal)
    bool ignoreClipboard = false;   // DDM itself copied something
    std::string lastClip;

protected:
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override;

private:
    MenuStrip menu;
    Toolbar toolbar;
    StatusBar status;
    NOTIFYICONDATAW tray{};
    bool trayAdded = false;
    int splitDrag = -1;
    int lastTrayPct = -2;
    void layout();
    void addTray();
    void updateTray();
    void trayMenu();
    void contextMenu(POINT pt);
    std::vector<MenuItem> speedMenu();
    void deleteSelected(bool withFile);
    void updateToolbar();
    // removal with Undo: the downloads leave the list at once, and really go 3 seconds later unless Undo is clicked
    struct PendingDelete { std::vector<std::string> ids; bool withFile = false; int torrentMode = 0; };   // torrentMode: 1 .torrent file, 2 .torrent + data
    PendingDelete pending;
    std::set<std::string> pendingIds;
    HWND toast = nullptr;
    ULONGLONG toastUntil = 0, toastShownAt = 0;
    bool toastHover = false;
    std::string toastText;
    RECT toastUndo{};
    void removeWithUndo(const std::vector<std::string>& ids, bool withFile, int torrentMode, const std::string& what);
    void undoDelete();
    void showToast();
    void placeToast(double slide);
    static LRESULT CALLBACK toastProc(HWND hw, UINT m, WPARAM w, LPARAM l);
    // View → Hide categories: the panel slides away (and back)
    double treeShown = 1.0;   // 0 hidden … 1 shown
    ULONGLONG treeAnimStart = 0;
    double treeAnimFrom = 1.0;
    int treeWidthNow() const;
    void toggleTree();
    void moveRename();
    void categoryMenu(const std::string& key, POINT pt);
    void newCategory();
    void recategorize(const std::string& key);   // downloads of a category you added, changed or deleted
    std::string readyUpdate;   // a checked installer waiting until nothing downloads (Options: update by itself)
    void installReadyUpdate();   // every download's category again (after your categories changed)
};

} // namespace ui
} // namespace ddm
