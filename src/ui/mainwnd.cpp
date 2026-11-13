#include "mainwnd.hpp"
#include "speedglide.hpp"
#include "traypanel.hpp"
#include <thread>
#include "../core/remote.hpp"
#include "../core/playlists.hpp"
#include "../core/media.hpp"
#include "../core/preview.hpp"
#include "../core/torrent.hpp"
#include "../core/queues.hpp"
#include "../core/links.hpp"
#include "../core/after.hpp"
#include "app.hpp"
#include <shellapi.h>
#include <commdlg.h>
#include "dialogs.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "../core/engine.hpp"
#include "../core/update.hpp"
#include "../common/version.hpp"
#include "../core/filetools.hpp"
#include "../core/integration.hpp"
#include "../core/xray.hpp"
#include "../core/http.hpp"
#include "../core/settings.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <algorithm>
#include <set>
#include <cstdio>
#include <memory>

namespace ddm {
namespace ui {

namespace { double g_totalSpeed = 0; }   // what every running download adds up to, before easing

// ================================================================ MenuStrip
bool MenuStrip::create(HWND parent) { return Wnd::create(L"DDMMenuStrip", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 10, 10, parent); }

void MenuStrip::layout(HDC dc) {
    rects.clear();
    int x = S(6);
    for (auto& e : entries) {
        SIZE s = textSize(dc, e.title, fontUI());
        RECT r{x, S(2), x + s.cx + S(18), S(24)};
        rects.push_back(r);
        x = r.right + S(2);
    }
}

LRESULT MenuStrip::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            Paint p(h);
            HDC dc = p.dc();
            RECT r = p.rc();
            fill(dc, r, P.title);
            RECT ln{0, r.bottom - 1, r.right, r.bottom};
            fill(dc, ln, P.line);
            layout(dc);
            for (size_t i = 0; i < entries.size(); i++) {
                if (static_cast<int>(i) == hot) fillRound(dc, rects[i], Sf(4), P.hover);
                text(dc, entries[i].title, rects[i], P.text, fontUI(), DT_CENTER);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int nh = -1;
            for (size_t i = 0; i < rects.size(); i++) if (PtInRect(&rects[i], pt)) nh = static_cast<int>(i);
            if (nh != hot) { hot = nh; TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); redraw(); }
            return 0;
        }
        case WM_MOUSELEAVE: hot = -1; redraw(); return 0;
        case WM_LBUTTONDOWN: {
            if (hot < 0) return 0;
            int i = hot;
            RECT r = rects[static_cast<size_t>(i)];
            POINT pt{r.left, r.bottom};
            ClientToScreen(h, &pt);
            UINT c = popupMenu(GetParent(h), entries[static_cast<size_t>(i)].items(), pt);
            hot = -1;
            redraw();
            if (c && onCommand) onCommand(c);
            return 0;
        }
    }
    return def(m, w, l);
}

// ================================================================ Toolbar
bool Toolbar::create(HWND parent) { return Wnd::create(L"DDMToolbar", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 10, 10, parent); }

void Toolbar::setEnabled(UINT id, bool on) {
    for (auto& t : tools) if (t.id == id && t.enabled != on) { t.enabled = on; redraw(); }
}

void Toolbar::setTool(UINT id, const std::string& label, const std::string& ic, COLORREF color) {
    for (auto& t : tools) {
        if (t.id != id || (t.label == label && t.icon == ic && t.color == color)) continue;
        bool wider = t.label != label;
        t.label = label; t.icon = ic; t.color = color;
        if (wider) layout();
        redraw();
    }
}

void Toolbar::layout() {
    rects.clear();
    HDC dc = GetDC(h);
    int x = S(8);
    for (auto& t : tools) {
        if (t.sepBefore) x += S(13);
        SIZE s = textSize(dc, t.label, fontTool());
        int w = std::max(S(66), static_cast<int>(s.cx) + S(16));
        rects.push_back(RECT{x, S(6), x + w, S(64)});
        x += w + S(2);
    }
    ReleaseDC(h, dc);
}

LRESULT Toolbar::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: layout(); return 0;
        case WM_PAINT: {
            Paint p(h);
            HDC dc = p.dc();
            RECT r = p.rc();
            fill(dc, r, P.bar);
            RECT ln{0, r.bottom - 1, r.right, r.bottom};
            fill(dc, ln, P.line);
            if (rects.size() != tools.size()) layout();
            for (size_t i = 0; i < tools.size(); i++) {
                const Tool& t = tools[i];
                RECT b = rects[i];
                if (b.left > r.right) break;
                if (t.sepBefore) { RECT s{b.left - S(7), S(12), b.left - S(6), S(58)}; fill(dc, s, P.line); }
                bool hov = static_cast<int>(i) == hot && t.enabled;
                if (t.id == highlight) fillRound(dc, b, Sf(6), P.toolSel);
                if (hov) fillRound(dc, b, Sf(6), static_cast<int>(i) == pressed ? blend(P.hover, P.text, 0.08f) : P.hover);
                COLORREF c = t.enabled ? t.color : blend(P.bar, P.muted, 0.45f);
                int iw = S(26);
                RECT ir{(b.left + b.right - iw) / 2, b.top + S(6), (b.left + b.right + iw) / 2, b.top + S(6) + iw};
                icon(dc, t.icon, ir, c, 1.7f);
                RECT tr{b.left, ir.bottom + S(3), b.right, b.bottom - S(2)};
                text(dc, t.label, tr, t.enabled ? P.text : P.muted, fontTool(), DT_CENTER);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int nh = -1;
            for (size_t i = 0; i < rects.size(); i++) if (PtInRect(&rects[i], pt)) nh = static_cast<int>(i);
            if (nh != hot) { hot = nh; TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); redraw(); }
            return 0;
        }
        case WM_MOUSELEAVE: hot = -1; pressed = -1; redraw(); return 0;
        case WM_LBUTTONDOWN: pressed = hot; redraw(); return 0;
        case WM_LBUTTONUP: {
            int p = pressed;
            pressed = -1;
            redraw();
            if (p >= 0 && p == hot && tools[static_cast<size_t>(p)].enabled && onCommand) onCommand(tools[static_cast<size_t>(p)].id);
            return 0;
        }
    }
    return def(m, w, l);
}

// ================================================================ CategoryTree
bool CategoryTree::create(HWND parent) {
    rowH = S(32);
    nodes = {
        {"all", "All downloads", "folder", 0, true, true, 0},
        {"compressed", "Compressed", "folder", 1, false, false, 0},
        {"documents", "Documents", "folder", 1, false, false, 0},
        {"music", "Music", "folder", 1, false, false, 0},
        {"programs", "Programs", "folder", 1, false, false, 0},
        {"video", "Video", "folder", 1, false, false, 0},
        {"torrent", "Torrents", "magnet", 0, true, true, 0},
        {"torrent-dl", "Downloading", "down", 1, false, false, 0},
        {"torrent-seed", "Seeding", "check", 1, false, false, 0},
        {"unfinished", "Unfinished", "down", 0, false, false, 0},
        {"finished", "Finished", "check", 0, false, false, 0},
        {"queues", "Queues", "queue", 0, true, true, 0},
    };
    return Wnd::create(L"DDMTree", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 10, 10, parent);
}

// Premium icons for the categories: folders with a mark for the kind of files, gradient tiles for the rest.
void treeIcon(HDC dc, const RECT& r, const std::string& key) {
    if (key == "all") folderIcon(dc, r, "folder");
    else if (key == "compressed" || key == "documents" || key == "music" || key == "programs" || key == "video") folderIcon(dc, r, key);
    else if (key == "torrent") tileIcon(dc, r, "magnet", rgb(0x4CD07A), rgb(0x1F9E4E));
    else if (key == "torrent-dl" || key == "unfinished") tileIcon(dc, r, "down", key == "unfinished" ? rgb(0xFFA94D) : rgb(0x4FA8FF), key == "unfinished" ? rgb(0xE8740C) : rgb(0x1E6FD9));
    else if (key == "torrent-seed") tileIcon(dc, r, "M12 19V5M5 12l7-7 7 7", rgb(0x4CD07A), rgb(0x1F9E4E));
    else if (key == "finished") tileIcon(dc, r, "check", rgb(0x4CD07A), rgb(0x1F9E4E));
    else if (key == "queues") tileIcon(dc, r, "queue", rgb(0x7D8CFF), rgb(0x4B5BE0));
    else if (startsWith(key, "queue-")) tileIcon(dc, r, key == "queue-sync" ? "refresh" : "clock", rgb(0xA98BFF), rgb(0x7650E8));
    else folderIcon(dc, r, "folder");
}

void CategoryTree::syncCategories() {
    // the rows between "All downloads" and "Torrents"
    std::vector<std::pair<std::string, std::string>> want;
    for (auto& c : settings().categories) if (c.key != "torrent" && c.key != "general") want.push_back({c.key, c.name});
    auto tor = std::find_if(nodes.begin(), nodes.end(), [](const Node& n) { return n.key == "torrent"; });
    if (tor == nodes.end() || nodes.empty()) return;
    size_t end = static_cast<size_t>(tor - nodes.begin());
    bool same = end == 1 + want.size();
    for (size_t i = 0; same && i < want.size(); i++) same = nodes[1 + i].key == want[i].first && nodes[1 + i].name == want[i].second;
    if (same) return;
    std::vector<Node> rows;
    for (auto& w : want) rows.push_back({w.first, w.second, "folder", 1, false, false, 0});
    nodes.erase(nodes.begin() + 1, nodes.begin() + static_cast<std::ptrdiff_t>(end));
    nodes.insert(nodes.begin() + 1, rows.begin(), rows.end());
    if (std::none_of(nodes.begin(), nodes.end(), [&](const Node& n) { return n.key == selected; })) { selected = "all"; if (onSelect) onSelect(selected); }
    redraw();
}

void CategoryTree::setCounts(const std::vector<Task>& all) {
    syncCategories();
    // the queues, as the Scheduler has them
    {
        std::vector<std::pair<std::string, std::string>> qs;
        { std::lock_guard<std::mutex> g(queues::lock()); for (auto& q : queues::all()) qs.push_back({"queue-" + q.id, q.name}); }
        auto at = std::find_if(nodes.begin(), nodes.end(), [](const decltype(nodes)::value_type& n) { return n.key == "queues"; });
        size_t qi = static_cast<size_t>(at - nodes.begin());
        bool same = nodes.size() == qi + 1 + qs.size();
        for (size_t i = 0; same && i < qs.size(); i++) same = nodes[qi + 1 + i].key == qs[i].first && nodes[qi + 1 + i].name == qs[i].second;
        if (!same) {
            nodes.resize(qi + 1);
            for (auto& q : qs) { auto n = nodes[qi]; n.key = q.first; n.name = q.second; n.level = 1; n.expandable = false; n.expanded = false; n.count = 0; nodes.push_back(n); }
        }
    }
    for (auto& n : nodes) {
        int c = 0;
        for (auto& t : all) {
            bool tor = t.kind == "torrent";
            if (n.key == "all") c++;
            else if (n.key == "torrent") c += tor;
            else if (n.key == "torrent-dl") c += tor && t.state != State::Complete;
            else if (n.key == "torrent-seed") c += tor && t.state == State::Complete && torrentSeeding(t.id);
            else if (n.key == "unfinished") c += t.state != State::Complete;
            else if (n.key == "finished") c += t.state == State::Complete;
            else if (startsWith(n.key, "queue-")) c += t.queue == n.key.substr(6) && t.state != State::Complete;
            else if (n.key == "queues") c += 0;
            else c += !tor && t.category == n.key;
        }
        n.count = c;
    }
    redraw();
}

std::vector<int> CategoryTree::visible() const {
    std::vector<int> v;
    bool parentOpen = true;
    for (size_t i = 0; i < nodes.size(); i++) {
        if (nodes[i].level == 0) { v.push_back(static_cast<int>(i)); parentOpen = !nodes[i].expandable || nodes[i].expanded; }
        else if (parentOpen) v.push_back(static_cast<int>(i));
    }
    return v;
}

LRESULT CategoryTree::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            Paint p(h);
            HDC dc = p.dc();
            RECT r = p.rc();
            fill(dc, r, P.pane);
            RECT head{0, 0, r.right, S(30)};
            RECT ht{S(12), 0, r.right, S(30)};
            text(dc, "Categories", ht, P.muted, fontSmallBold());
            RECT hl{0, S(30) - 1, r.right, S(30)};
            fill(dc, hl, P.line);
            (void)head;
            int y = S(36);
            auto vis = visible();
            for (size_t k = 0; k < vis.size(); k++) {
                const Node& n = nodes[static_cast<size_t>(vis[k])];
                RECT row{S(6), y, r.right - S(6), y + rowH};
                bool sel = n.key == selected;
                if (sel) fillRound(dc, row, Sf(5), P.sel);
                else if (vis[k] == hot) fillRound(dc, row, Sf(5), P.hover);
                int x = row.left + S(4) + n.level * S(18);
                if (n.expandable) { RECT cr{x, y + (rowH - S(12)) / 2, x + S(12), y + (rowH - S(12)) / 2 + S(12)}; icon(dc, n.expanded ? "chevron" : "chevronr", cr, P.muted, 2.4f); }
                x += S(18);
                int is = n.level ? S(22) : S(24);   // big enough to tell the categories apart at a glance
                RECT ir{x - S(2), y + (rowH - is) / 2, x - S(2) + is, y + (rowH - is) / 2 + is};
                treeIcon(dc, ir, n.key);
                x += is + S(6);
                RECT tr{x, y, row.right - S(30), y + rowH};
                textName(dc, n.name, tr, P.text, sel ? fontBold() : fontUI());
                if (n.key != "queues" && (n.count > 0 || n.level == 1 || n.key == "all")) {
                    RECT cr{row.right - S(36), y, row.right - S(8), y + rowH};
                    text(dc, std::to_string(n.count), cr, P.muted, fontSmall(), DT_RIGHT);
                }
                y += rowH + S(1);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int y = GET_Y_LPARAM(l) - S(36);
            auto vis = visible();
            int k = y >= 0 ? y / (rowH + S(1)) : -1;
            int nh = (k >= 0 && k < static_cast<int>(vis.size())) ? vis[static_cast<size_t>(k)] : -1;
            if (nh != hot) { hot = nh; TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); redraw(); }
            return 0;
        }
        case WM_MOUSELEAVE: hot = -1; redraw(); return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(h);
            if (hot < 0) return 0;
            Node& n = nodes[static_cast<size_t>(hot)];
            int x = GET_X_LPARAM(l);
            if (n.expandable && x < S(28) + n.level * S(18)) { n.expanded = !n.expanded; redraw(); return 0; }
            if (n.key == "queues") { n.expanded = !n.expanded; redraw(); return 0; }
            selected = n.key;
            redraw();
            if (onSelect) onSelect(selected);
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ClientToScreen(h, &pt);
            if (onMenu) onMenu(hot >= 0 ? nodes[static_cast<size_t>(hot)].key : "", pt);
            return 0;
        }
        case WM_LBUTTONDBLCLK: if (hot >= 0 && nodes[static_cast<size_t>(hot)].expandable) { nodes[static_cast<size_t>(hot)].expanded = !nodes[static_cast<size_t>(hot)].expanded; redraw(); } return 0;
        case WM_KEYDOWN: {
            if (w != VK_UP && w != VK_DOWN) break;
            auto vis = visible();
            int cur = 0;
            for (size_t k = 0; k < vis.size(); k++) if (nodes[static_cast<size_t>(vis[k])].key == selected) cur = static_cast<int>(k);
            cur += w == VK_UP ? -1 : 1;
            cur = std::max(0, std::min(cur, static_cast<int>(vis.size()) - 1));
            selected = nodes[static_cast<size_t>(vis[static_cast<size_t>(cur)])].key;
            redraw();
            if (onSelect) onSelect(selected);
            return 0;
        }
        case WM_GETDLGCODE: return DLGC_WANTARROWS;
    }
    return def(m, w, l);
}

// ================================================================ StatusBar
bool StatusBar::create(HWND parent) { return Wnd::create(L"DDMStatus", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 10, 10, parent); }

LRESULT StatusBar::proc(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        Paint p(h);
        HDC dc = p.dc();
        RECT r = p.rc();
        fill(dc, r, P.title);
        RECT ln{0, 0, r.right, 1};
        fill(dc, ln, P.line);
        int x = S(12);
        bool showNote = !note.empty() && GetTickCount64() < noteUntil;
        const std::string& lt = showNote ? note : left;
        SIZE s = textSize(dc, lt, fontSmall());
        RECT a{x, 0, x + s.cx + S(2), r.bottom};
        text(dc, lt, a, showNote ? P.accent : P.muted, fontSmall());
        x = a.right + S(18);
        if (!network.empty()) {
            fillCircle(dc, x + Sf(3.5f), r.bottom / 2.0f, Sf(3.5f), networkOk ? P.good : P.muted);
            x += S(13);
            SIZE ns = textSize(dc, network, fontSmall());
            RECT b{x, 0, x + ns.cx + S(2), r.bottom};
            text(dc, network, b, P.muted, fontSmall());
        }
        RECT sp{0, 0, r.right - S(12), r.bottom};
        text(dc, speed, sp, P.text, fontSmall(), DT_RIGHT);
        SIZE ss = textSize(dc, speed, fontSmall());
        RECT lm{0, 0, r.right - S(30) - ss.cx, r.bottom};
        text(dc, limit, lm, P.muted, fontSmall(), DT_RIGHT);
        return 0;
    }
    return def(m, w, l);
}

// ================================================================ MainWindow
bool MainWindow::create() {
    const Settings& s = settings();
    int w = S(s.winW), ht = S(s.winH);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    w = std::min(w, static_cast<int>(wa.right - wa.left));
    ht = std::min(ht, static_cast<int>(wa.bottom - wa.top));
    int x = s.winX >= 0 ? s.winX : (wa.left + (wa.right - wa.left - w) / 2);
    int y = s.winY >= 0 ? s.winY : (wa.top + (wa.bottom - wa.top - ht) / 2);
    if (!Wnd::create(L"DDMMain", "DedSec Download Manager", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, x, y, w, ht, nullptr)) return false;
    styleWindow(h);

    menu.create(h);
    menu.entries = {
        {"Tasks", [] { return std::vector<MenuItem>{
            {"Add new download…", "Ctrl+N", CmdAddUrl}, {"Add from clipboard", "Ctrl+V", CmdPasteUrl}, {"Download playlist or channel…", "", CmdPlaylist}, MenuItem::sep(),
            {"Find…", "Ctrl+F", CmdFind}, {"Find next", "F3", CmdFindNext}, MenuItem::sep(),
            {"Exit", "", CmdExit}}; }},
        {"File", [this] {
            bool any = !list.selection().empty();
            std::vector<MenuItem> v = {
                {"Open", "Enter", CmdOpen}, {"Open folder", "", CmdOpenFolder}, MenuItem::sep(),
                {"Resume", "", CmdResume}, {"Pause", "", CmdPause}, {"Show progress window", "", CmdShowProgress}, {"Redownload", "", CmdRedownload}, MenuItem::sep(),
                {"Copy address", "", CmdCopyUrl}, {"Remove", "Del", CmdDelete}, {"Remove and delete file", "Shift+Del", CmdDeleteWithFile}};
            for (auto& it : v) if (!it.separator) it.disabled = !any;
            return v; }},
        {"Downloads", [this] {
            std::vector<MenuItem> v = {
                {"Pause all", "", CmdPauseAll}, {"Resume all", "", CmdResumeAll}, {"Delete all finished", "", CmdDeleteFinished}, MenuItem::sep()};
            MenuItem sp; sp.text = "Speed limiter"; sp.sub = speedMenu(); v.push_back(sp);
            v.push_back(MenuItem::sep());
            v.push_back({"Options…", "", CmdOptions});
            return v; }},
        {"View", [] {
            std::string t = settings().theme;
            std::vector<MenuItem> v = {{"Light", "", CmdThemeLight}, {"Dark", "", CmdThemeDark}, {"Same as Windows", "", CmdThemeSystem}};
            v[0].checked = t == "light"; v[1].checked = t == "dark"; v[2].checked = t == "system";
            v.push_back(MenuItem::sep());
            v.push_back({settings().hideTree ? "Show categories" : "Hide categories", "Ctrl+B", CmdToggleTree});
            v.push_back({"Add category…", "", CmdAddCategory});
            return v; }},
        {"Tools", [] { return std::vector<MenuItem>{{"Scheduler…", "", CmdScheduler}, {"X-Ray…", "", CmdXRay}, {"Grabber…", "", CmdGrabber}, MenuItem::sep(),
                                                     {"File tools…", "", CmdFileTools}, {"Create torrent…", "", CmdCreateTorrent}, {"Security…", "", CmdSecurity}, {"Private vault…", "", CmdVault}, {"Statistics…", "", CmdStats}, MenuItem::sep(),
                                                     {"Subscriptions…", "", CmdSubscriptions}, {"Phone remote…", "", CmdRemote}, MenuItem::sep(),
                                                     {"Browser integration…", "", CmdBrowser}}; }},
        {"Help", [] {
            std::vector<MenuItem> v{{"Help", "F1", CmdHelp}, MenuItem::sep(), {"Check for updates", "", CmdUpdates}, MenuItem::sep(), {"About DDM", "", CmdAbout}};
            return v; }},
    };
    menu.onCommand = [this](UINT c) { command(c); };

    toolbar.create(h);
    toolbar.highlight = CmdAddUrl;
    auto tool = [](const char* label, const char* ic, COLORREF c, UINT id, bool sep) { return Toolbar::Tool{label, ic, c, id, sep, true}; };
    toolbar.tools = {
        // one button for Pause / Resume (it follows what's selected), Stop, and one for Pause all / Resume all
        tool("Add URL", "add", P.accent, CmdAddUrl, false), tool("Resume", "resume", P.good, CmdPauseResume, false), tool("Stop", "stop", P.bad, CmdStop, false),
        tool("Pause all", "pauseall", P.warn, CmdPauseResumeAll, true), tool("Delete", "delete", P.bad, CmdDelete, false), tool("Delete finished", "deleteok", P.bad, CmdDeleteFinished, false),
        tool("Options", "options", P.muted, CmdOptions, true), tool("Scheduler", "clock", P.muted, CmdScheduler, false), tool("Start queue", "qstart", P.good, CmdStartQueue, false),
        tool("Stop queue", "qstop", P.bad, CmdStopQueue, false), tool("X-Ray", "xray", P.accent, CmdXRay, true), tool("Grabber", "grab", P.muted, CmdGrabber, false),
        tool(isDark() ? "Light mode" : "Dark mode", isDark() ? "sun" : "moon", P.muted, CmdToggleTheme, true),
    };
    toolbar.onCommand = [this](UINT c) { command(c); };

    tree.create(h);
    treeShown = settings().hideTree ? 0.0 : 1.0;
    tree.onSelect = [this](const std::string&) { refresh(); };
    list.create(h);
    list.onOpen = [this](const std::string& id) {
        Task t;
        if (!Engine::get().find(id, t)) return;
        if (t.state == State::Complete) propertiesDialog(h, id);   // like IDM: the file's properties, with Open
        else App::get().showProgress(id);
    };
    list.onContext = [this](POINT pt) { contextMenu(pt); };
    tree.onMenu = [this](const std::string& key, POINT pt) { categoryMenu(key, pt); };
    list.onDelete = [this] { deleteSelected((GetKeyState(VK_SHIFT) & 0x8000) != 0); };
    list.onSelectionChanged = [this] { updateToolbar(); };
    status.create(h);
    layout();
    addTray();
    SetTimer(h, 1, 500, nullptr);
    // Clipboard watcher (Windows Vista+; on older systems this simply isn't available).
    typedef BOOL(WINAPI * AddListener)(HWND);
    if (auto add = reinterpret_cast<AddListener>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AddClipboardFormatListener"))) add(h);
    refresh();
    return true;
}

void MainWindow::onClipboard() {
    if (!settings().clipboardMonitor || ignoreClipboard) { ignoreClipboard = false; return; }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(h)) return;
    std::string text;
    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) { if (auto p = static_cast<const wchar_t*>(GlobalLock(d))) { text = U(p); GlobalUnlock(d); } }
    CloseClipboard();
    if (text.size() > 20000 || text == lastClip) return;
    lastClip = text;
    auto all = extractUrls(text);
    if (all.size() != 1 || trim(text) != all[0]) return;   // only a copied link, not text that contains one
    auto type = links::classify(all[0]);
    if (type == links::Type::DirectFile || type == links::Type::Magnet || type == links::Type::TorrentFile || type == links::Type::Mega) {
        LinkRequest r;
        r.url = all[0];
        r.source = "clipboard";
        App::get().newDownload(r);
    }
}

std::vector<MenuItem> MainWindow::speedMenu() {
    int cur = settings().speedLimitKBps;
    std::vector<MenuItem> v = {{"No limit", "", CmdLimitOff}, {"256 KB/s", "", CmdLimit256}, {"512 KB/s", "", CmdLimit512}, {"1 MB/s", "", CmdLimit1M},
                               {"2 MB/s", "", CmdLimit2M}, {"5 MB/s", "", CmdLimit5M}, {"10 MB/s", "", CmdLimit10M}};
    int vals[] = {0, 256, 512, 1024, 2048, 5120, 10240};
    for (size_t i = 0; i < v.size(); i++) v[i].checked = cur == vals[i];
    return v;
}

void MainWindow::layout() {
    RECT r = client();
    int menuH = S(26), toolH = S(70), statusH = S(26);
    int treeW = treeWidthNow();
    menu.move(0, 0, r.right, menuH);
    toolbar.move(0, menuH, r.right, toolH);
    int top = menuH + toolH;
    int bodyH = std::max(0, static_cast<int>(r.bottom) - top - statusH);
    if (treeW > 0) {
        // while it slides, the panel keeps its full width and moves out to the left (its rows don't squash)
        int full = S(settings().treeWidth);
        tree.move(treeW - full, top, full, bodyH);
        if (!IsWindowVisible(tree.hwnd())) tree.show(true);
    } else if (IsWindowVisible(tree.hwnd())) tree.show(false);
    int gap = treeW > 0 ? 1 : 0;
    list.move(treeW + gap, top, std::max(0, static_cast<int>(r.right) - treeW - gap), bodyH);
    if (toast) placeToast(1.0);
    status.move(0, r.bottom - statusH, r.right, statusH);
    InvalidateRect(h, nullptr, FALSE);
}

static bool matches(const std::string& key, const Task& t) {
    bool tor = t.kind == "torrent";
    if (key == "all") return true;
    if (key == "torrent") return tor;
    if (key == "torrent-dl") return tor && t.state != State::Complete;
    if (key == "torrent-seed") return tor && t.state == State::Complete && torrentSeeding(t.id);
    if (key == "unfinished") return t.state != State::Complete;
    if (key == "finished") return t.state == State::Complete;
    if (startsWith(key, "queue-")) return t.queue == key.substr(6);
    if (key == "queues") return false;
    return !tor && t.category == key;
}

void MainWindow::refresh() {
    auto all = Engine::get().snapshot();
    if (!pendingIds.empty()) all.erase(std::remove_if(all.begin(), all.end(), [this](const Task& t) { return pendingIds.count(t.id) > 0; }), all.end());   // removed, Undo still possible
    tree.setCounts(all);
    std::vector<Task> shown;
    for (auto& t : all) if (matches(tree.selected, t)) shown.push_back(t);
    list.setItems(shown);
    int run = 0, queued = 0;
    double speed = 0;
    for (auto& t : all) { if (t.state == State::Running) { run++; speed += t.speed; } if (t.state == State::Queued) queued++; }
    char b[96];
    if (run || queued) snprintf(b, sizeof b, "%d downloading%s", run, queued ? (" · " + std::to_string(queued) + " queued").c_str() : "");
    else snprintf(b, sizeof b, "%zu downloads", all.size());
    status.left = b;
    auto ad = internetAdapters();
    std::string net;
    if (ad.size() >= 2 && settings().useAllAdapters) {
        for (size_t i = 0; i < ad.size(); i++) { if (i) net += " + "; net += ad[i].kind == "wifi" ? "Wi-Fi" : ad[i].kind == "ethernet" ? "Ethernet" : ad[i].kind == "mobile" ? "Mobile" : ad[i].name; }
        net = "Connections: " + net + " combined";
    } else if (ad.size() == 1) net = std::string("Connected: ") + (ad[0].kind == "wifi" ? "Wi-Fi" : ad[0].kind == "ethernet" ? "Ethernet" : ad[0].name);
    else net = "No network";
    status.network = net;
    status.networkOk = !ad.empty();
    int lim = settings().speedLimitKBps;
    status.limit = !lim ? "Speed limit: off" : "Speed limit: " + (lim < 1024 ? std::to_string(lim) + " KB/s" : std::to_string(lim / 1024) + " MB/s");
    if (lim && Engine::get().idleBoosted) status.limit += " · full speed while the internet is idle";
    g_totalSpeed = speed;
    status.speed = speed > 0 ? "↓ " + formatKBps(glidedSpeed("*all*", speed)) : "";
    // while something is downloading, the speeds are redrawn 15 times a second so they glide
    if (run > 0) SetTimer(h, 0x7B3, 66, nullptr); else KillTimer(h, 0x7B3);
    status.redraw();
    updateToolbar();
    updateTray();
}

void MainWindow::updateToolbar() {
    auto sel = list.selection();
    bool canResume = false, canPause = false;
    for (auto& id : sel) {
        Task t;
        if (!Engine::get().find(id, t)) continue;
        if (t.state == State::Paused || t.state == State::Error) canResume = true;
        if (t.state == State::Running || t.state == State::Queued) canPause = true;
    }
    toolbar.setEnabled(CmdResume, canResume);
    toolbar.setEnabled(CmdPause, canPause);
    toolbar.setEnabled(CmdDelete, !sel.empty());
    // Pause / Resume in one button: Pause while something selected is downloading, else Resume
    if (canPause) toolbar.setTool(CmdPauseResume, "Pause", "pause", P.warn);
    else toolbar.setTool(CmdPauseResume, "Resume", "resume", P.good);
    toolbar.setEnabled(CmdPauseResume, canPause || canResume);
    bool canStop = false;
    for (auto& id : sel) { Task t; if (Engine::get().find(id, t) && ((t.state != State::Complete && !t.stopped) || (t.kind == "torrent" && torrentSeeding(t.id)))) canStop = true; }
    toolbar.setEnabled(CmdStop, canStop);
    // Pause all / Resume all in one button
    bool anyActive = false, anyWaiting = false;
    for (auto& t : Engine::get().snapshot()) {
        if (t.hidden || pendingIds.count(t.id)) continue;
        if (t.state == State::Running || t.state == State::Queued) anyActive = true;
        if ((t.state == State::Paused || t.state == State::Error) && !t.stopped) anyWaiting = true;
    }
    if (anyActive) toolbar.setTool(CmdPauseResumeAll, "Pause all", "pauseall", P.warn);
    else toolbar.setTool(CmdPauseResumeAll, "Resume all", "resumeall", P.good);
    toolbar.setEnabled(CmdPauseResumeAll, anyActive || anyWaiting);
}

// ---------------------------------------------------------------- tray
static HICON makeTrayIcon(double progress, bool active) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(dc, color);
    memset(bits, 0, static_cast<size_t>(sz) * sz * 4);
    RECT r{0, 0, sz, sz};
    logo(dc, r);
    if (active && progress >= 0) {
        RECT b{1, sz - std::max(3, sz / 6) - 1, sz - 1, sz - 1};
        fillRound(dc, b, 1.5f, rgb(0x3A3D45));
        RECT f = b;
        f.right = b.left + static_cast<LONG>((b.right - b.left) * std::min(1.0, progress));
        if (f.right > f.left) fillRound(dc, f, 1.5f, rgb(0xC6F432));
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

void MainWindow::addTray() {
    tray.cbSize = sizeof tray;
    tray.hWnd = h;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.uCallbackMessage = App::WM_TRAY;
    tray.hIcon = makeTrayIcon(-1, false);
    wcscpy_s(tray.szTip, L"DedSec Download Manager");
    trayAdded = Shell_NotifyIconW(NIM_ADD, &tray) != 0;
}

void MainWindow::setStatusNote(const std::string& text) {
    status.note = text;
    status.noteUntil = GetTickCount64() + 6000;
    status.redraw();
}

void MainWindow::notify(const std::string& title, const std::string& text) {
    if (!trayAdded) return;
    NOTIFYICONDATAW n = tray;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, W(i18n::tr(title)).c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, W(i18n::tr(text)).c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;   // DDM is silent
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void MainWindow::updateTray() {
    if (!trayAdded) return;
    auto all = Engine::get().snapshot();
    int64_t done = 0, total = 0;
    int run = 0;
    double speed = 0;
    const Task* one = nullptr;
    for (auto& t : all) if (t.state == State::Running) { run++; speed += t.speed; if (t.total > 0) { done += t.done; total += t.total; } one = &t; }
    int pct = run && total > 0 ? static_cast<int>(done * 100 / total) : -1;
    std::string tip;
    if (run == 1 && one) {
        tip = one->fileName + "\n" + (one->total > 0 ? std::to_string(static_cast<int>(one->progress() * 100)) + "% · " : "") + formatKBps(one->speed) + (one->eta >= 0 ? " · " + formatDuration(one->eta) + " left" : "");
    } else if (run > 1) tip = "DDM — " + std::to_string(run) + " downloading · " + formatKBps(speed);
    else tip = "DedSec Download Manager";
    std::wstring wt = W(i18n::tr(tip));
    if (run > 1) {   // like IDM: each download with its percentage, as many as the tip holds
        std::vector<const Task*> on;
        for (auto& t : all) if (t.state == State::Running) on.push_back(&t);
        std::sort(on.begin(), on.end(), [](const Task* a, const Task* b) { return a->speed > b->speed; });
        size_t shown = 0;
        for (auto* t : on) {
            std::wstring n = W(t->fileName);
            if (n.size() > 26) n = n.substr(0, 25) + L"…";
            std::wstring line = L"\n" + n + L"  " + (t->total > 0 ? std::to_wstring(static_cast<int>(t->progress() * 100)) + L"%" : W(formatBytes(t->done)));
            std::wstring rest = on.size() - shown > 1 ? L"\n+" + std::to_wstring(on.size() - shown - 1) + L"…" : L"";
            if (wt.size() + line.size() + rest.size() > 127) { wt += L"\n+" + std::to_wstring(on.size() - shown) + L"…"; break; }
            wt += line;
            shown++;
        }
    }
    if (wt.size() > 127) wt = wt.substr(0, 124) + L"…";
    wcscpy_s(tray.szTip, wt.c_str());
    tray.uFlags = NIF_TIP;
    if (pct != lastTrayPct) {
        lastTrayPct = pct;
        HICON old = tray.hIcon;
        tray.hIcon = makeTrayIcon(pct >= 0 ? pct / 100.0 : -1, run > 0);
        tray.uFlags |= NIF_ICON;
        Shell_NotifyIconW(NIM_MODIFY, &tray);
        if (old) DestroyIcon(old);
    } else Shell_NotifyIconW(NIM_MODIFY, &tray);
}

void MainWindow::trayMenu() {
    std::vector<MenuItem> v = {{"Open DDM", "", CmdShow}, {"Add URL…", "Ctrl+N", CmdAddUrl}, MenuItem::sep(), {"Pause all", "", CmdPauseAll}, {"Resume all", "", CmdResumeAll}};
    v[0].bold = true;
    auto hidden = progressInTray();   // download windows minimized into the tray
    if (!hidden.empty()) {
        std::vector<MenuItem> w;
        for (size_t i = 0; i < hidden.size() && i < 50; i++) w.push_back({"Show: " + hidden[i].second, "", static_cast<UINT>(CmdTrayWinBase + i)});
        w.push_back(MenuItem::sep());
        v.insert(v.begin(), w.begin(), w.end());
    }
    MenuItem sp; sp.text = "Speed limiter"; sp.sub = speedMenu(); v.push_back(sp);
    v.push_back({"Scheduler…", "", CmdScheduler});
    v.push_back(MenuItem::sep());
    v.push_back({"Options…", "", CmdOptions});
    v.push_back({"Exit", "", CmdExit});
    POINT pt;
    GetCursorPos(&pt);
    UINT c = popupMenu(h, v, pt);
    if (c >= CmdTrayWinBase && c < CmdTrayWinBase + 50) { size_t i = c - CmdTrayWinBase; if (i < hidden.size()) showProgressWindow(hidden[i].first); return; }
    if (c) command(c);
}

void MainWindow::showFromTray() {
    ShowWindow(h, IsZoomed(h) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
    refresh();
    App::get().checkUpdatesIfStale();   // opened again after a while: a newer DDM shows up now, not hours later
}

// ---------------------------------------------------------------- updating by itself
void MainWindow::installReadyUpdate() {
    if (readyUpdate.empty() || !fileExists(readyUpdate)) { KillTimer(h, 79); readyUpdate.clear(); return; }
    for (auto& t : Engine::get().snapshot()) if (t.state == State::Running || t.state == State::Queued) return;   // later: something is downloading
    if (GetForegroundWindow() && GetWindow(h, GW_ENABLEDPOPUP)) return;   // a DDM window is asking something
    KillTimer(h, 79);
    std::string path = readyUpdate;
    readyUpdate.clear();
    logLine("update: installing " + path + " by itself");
    if (!IsWindowVisible(h)) writeFileAtomic(joinPath(dataDir(), "start-in-tray.txt"), "1");   // it comes back the way it was: in the tray
    // quietly: the installer replaces DDM and starts it again (in the tray when it was there)
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", W(path).c_str(), L"/S", nullptr, SW_SHOWNORMAL)) > 32) App::get().quit();
}

// ---------------------------------------------------------------- your own categories
void MainWindow::recategorize(const std::string& key) {
    // only what the change is about: downloads of the category's file types (left where you put them yourself),
    // and the downloads of a deleted category (back to where their type belongs)
    const Category* cat = findCategory(key);
    for (auto& t : Engine::get().snapshot()) {
        if (t.kind == "torrent") continue;
        std::string now = categoryFor(t.fileName);
        bool mine = t.category == key;
        if (!cat) { if (mine) Engine::get().edit(t.id, [now](Task& k) { k.category = now; }); continue; }
        if (now == key && !mine && (t.category.empty() || t.category == "general" || isBuiltinCategory(t.category))) Engine::get().edit(t.id, [key](Task& k) { k.category = key; });
    }
    refresh();
}

void MainWindow::newCategory() {
    std::string name;
    if (!inputBox(h, "Add category", "Name of the new category", name) || trim(name).empty()) return;
    std::string types;
    if (!inputBox(h, "Add category", "Its file types, for example: psd ai svg", types)) return;
    std::string key = addCategory(name, types);
    if (key.empty()) { askBox(h, "Add category", "There's already a category with that name (or the name can't be used).", {"OK"}, 0); return; }
    saveSettings();
    recategorize(key);
    tree.syncCategories();
    tree.selected = key;
    if (tree.onSelect) tree.onSelect(key);
    setStatusNote("Category added: " + categoryName(key));
}

void MainWindow::categoryMenu(const std::string& key, POINT pt) {
    bool own = !key.empty() && findCategory(key) && !isBuiltinCategory(key);
    bool fileCat = !key.empty() && findCategory(key) && key != "torrent";
    std::vector<MenuItem> v;
    v.push_back({"Add category…", "", 1});
    if (fileCat) {
        v.push_back(MenuItem::sep());
        v.push_back({"Open folder", "", 5});
        v.push_back({"File types…", "", 4});
        v.push_back({"Change folder…", "", 6});
    }
    if (own) {
        v.push_back({"Rename…", "", 2});
        v.push_back({"Delete category", "", 3});
    }
    UINT c = popupMenu(h, v, pt);
    if (c == 1) newCategory();
    else if (c == 2) {
        std::string name = categoryName(key);
        if (!inputBox(h, "Rename category", "New name", name)) return;
        if (!renameCategory(key, name)) { askBox(h, "Rename category", "There's already a category with that name (or the name can't be used).", {"OK"}, 0); return; }
        saveSettings();
        tree.syncCategories();
        refresh();
    } else if (c == 3) {
        std::string name = categoryName(key);
        if (askBox(h, "Delete category", "Delete the category " + name + "?\n\nIts downloads and files stay where they are; they're listed in the built-in categories again.", {"Delete", "Cancel"}, 1) != 0) return;
        removeCategory(key);
        saveSettings();
        recategorize(key);
        tree.syncCategories();
    } else if (c == 4) {
        const Category* cat = findCategory(key);
        if (!cat) return;
        std::string types;
        for (auto& x : cat->exts) types += (types.empty() ? "" : " ") + x;
        if (!inputBox(h, "File types of " + cat->name, "File types (for example: psd ai svg)", types)) return;
        setCategoryTypes(key, types);
        saveSettings();
        recategorize(key);
    } else if (c == 5) {
        std::string d = folderFor(key);
        ShellExecuteW(h, L"open", W(d).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (c == 6) {
        std::string d = pickFolder(h, folderFor(key));
        if (d.empty()) return;
        for (auto& k : settings().categories) if (k.key == key) k.folder = d;
        saveSettings();
        setStatusNote("New " + categoryName(key) + " downloads go to " + d);
    }
}

void MainWindow::hideToTray() { commitPendingDelete(); ShowWindow(h, SW_HIDE); trimMemory(); }   // (the Undo card belongs to the window: it goes with it)

// In the tray, DDM hands back the memory its windows used (Windows pages it back in if it's needed again).
void MainWindow::trimMemory() {
    if (IsWindowVisible(h)) return;
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

// A minute and a half after DDM starts (then every hour): the questions DDM asks once, and the add-on reminder.
void MainWindow::nudges() {
    for (HWND w = GetForegroundWindow(); w; w = nullptr) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); if (pid == GetCurrentProcessId() && w != h) return; }   // not over one of DDM's own windows
    Settings& s = settings();
    // another download manager on this PC: ask once which one catches downloads
    if (!s.otherDownloaderAsked) {
        auto others = integration::otherDownloaders();
        if (!others.empty()) {
            s.otherDownloaderAsked = true;
            saveSettings();
            std::string names;
            for (size_t i = 0; i < others.size(); i++) names += (i ? (i + 1 == others.size() ? " and " : ", ") : "") + others[i];
            int r = askBox(nullptr, "Which download manager?", names + (others.size() == 1 ? " is" : " are") + " also on this PC.\n\nShould DDM be the one that catches your downloads, videos and magnet links first?",
                           {"Use DDM first", "Keep it as it is"}, 0);
            if (r == 0) {
                s.catchBrowser = true; s.catchVideos = true;
                saveSettings();
                integration::claimMagnetLinks();
                std::vector<std::string> btns;
                std::vector<std::string> keys;
                for (auto& b : integration::browsers()) if (b.installed && btns.size() < 3) { btns.push_back((b.key == "chrome" ? std::string("Chrome") : b.key == "edge" ? std::string("Edge") : b.key == "firefox" ? std::string("Firefox") : b.name) + " add-ons"); keys.push_back(b.key); }
                btns.push_back("Done");
                int k = askBox(nullptr, "DDM catches downloads first",
                               "DDM now takes downloads from your browsers and opens magnet links.\n\nOne more step: browsers let only one add-on take a download. In each browser, turn off the add-on of " + names +
                               " so it doesn't take the file before DDM (DDM can open the browser's add-ons page for you).", btns, static_cast<int>(btns.size()) - 1);
                if (k >= 0 && k < static_cast<int>(keys.size())) integration::openExtensionsPage(keys[static_cast<size_t>(k)]);
            }
            SetTimer(h, 78, 2 * 60 * 1000, nullptr);   // one question at a time: the add-on reminder comes a little later
            return;
        }
    }
    // the browser add-on: a gentle reminder, at most once a day, until every browser has it (or "don't remind me")
    if (!s.addonReminderOff) {
        int today = static_cast<int>(nowMs() / 86400000);
        if (s.addonRemindDay == today) return;
        auto missing = browsersWithoutAddon();
        if (missing.empty()) return;
        s.addonRemindDay = today;
        saveSettings();
        showAddonReminder(h, missing);
    }
}

void MainWindow::contextMenu(POINT pt) {
    auto sel = list.selection();
    if (sel.empty()) return;
    Task t;
    Engine::get().find(sel.front(), t);
    bool done = t.state == State::Complete;
    std::vector<MenuItem> v;
    if (done) { v.push_back({"Open", "", CmdOpen}); v.back().bold = true; v.push_back({"Open folder", "", CmdOpenFolder}); }
    else { v.push_back({"Show progress", "", CmdShowProgress}); v.back().bold = true; v.push_back({"Open folder", "", CmdOpenFolder}); }
    if (sel.size() == 1 && preview::canPlay(t)) v.push_back({done ? "Play in DDM" : "Play while downloading", "", CmdPlay});
    v.push_back(MenuItem::sep());
    bool active = t.state == State::Running || t.state == State::Queued;
    if (!done) {   // one Pause / Resume (as the toolbar), and Stop
        v.push_back(active ? MenuItem{"Pause", "", CmdPause} : MenuItem{t.kind == "torrent" ? "Start" : "Resume", "", CmdResume});
        if (!t.stopped || active) v.push_back({"Stop", "", CmdStop});
    }
    if (!done && t.state != State::Running) v.push_back({"Force start (ignore limits)", "", CmdForceStart});
    v.push_back({"Redownload", "", CmdRedownload});
    v.push_back({"Restart from the beginning", "", CmdRestart});
    if (!done && t.kind != "torrent") v.push_back({"Refresh download link…", "", CmdRefreshLink});
    if (xray::canLook(t.fileName) || t.kind == "torrent") v.push_back({"X-Ray (look inside)…", "", CmdXRay});
    if (t.kind == "torrent" && sel.size() == 1) {   // files, trackers, peers and its own options
        v.push_back({"Torrent details…", "", CmdTorrentDetails});
        if (done) v.push_back({torrentSeeding(t.id) ? "Stop sharing (seeding)" : "Share (seed)", "", CmdSeedToggle});
        v.push_back({"Copy magnet link", "", CmdCopyMagnet});
    }
    if (done && sel.size() == 1 && (fileExists(t.path()) || dirExists(t.path()))) v.push_back({t.kind == "torrent" ? "Move to…" : "Move / rename…", "", CmdMoveRename});
    v.push_back(MenuItem::sep());
    // queues (like IDM: Move to queue)
    MenuItem mq; mq.text = "Move to queue";
    {
        std::lock_guard<std::mutex> g(queues::lock());
        int i = 0;
        for (auto& q : queues::all()) { MenuItem it{q.name, "", static_cast<UINT>(CmdQueueBase + i)}; it.checked = t.queue == q.id; mq.sub.push_back(it); i++; }
    }
    v.push_back(mq);
    if (!t.queue.empty()) v.push_back({"Remove from queue", "", CmdQueueRemove});
    MenuItem pr; pr.text = "Priority";
    for (int p = 2; p >= 0; p--) { MenuItem it{p == 2 ? "High" : p == 1 ? "Normal" : "Low", "", static_cast<UINT>(CmdPriorityBase + p)}; it.checked = t.priority == p; pr.sub.push_back(it); }
    v.push_back(pr);
    v.push_back(MenuItem::sep());
    v.push_back({"Copy address", "", CmdCopyUrl});
    v.push_back({"Remove from download list", "Del", CmdDelete});
    v.push_back({done ? "Remove with file (to the Recycle Bin)" : "Remove and delete what arrived", "Shift+Del", CmdDeleteWithFile});
    bool allTorrents = true;
    for (auto& i : sel) { Task x; if (!Engine::get().find(i, x) || x.kind != "torrent") allTorrents = false; }
    if (allTorrents) {   // like a torrent program: what goes with it
        MenuItem dt; dt.text = "Delete torrent";
        dt.sub.push_back({"Remove torrent (keep its files)", "", CmdDelete});
        dt.sub.push_back({"Remove and delete the .torrent file", "", CmdDelTorrentFile});
        dt.sub.push_back({"Remove and delete torrent + data", "", CmdDelTorrentAll});
        dt.sub.push_back({"Delete only the downloaded data", "", CmdDelTorrentData});
        v.push_back(dt);
    }
    std::vector<std::string> toolKeys;
    if (done && sel.size() == 1 && fileExists(t.path())) {   // File tools for this file
        static const std::set<std::string> vid = {"mp4", "mkv", "webm", "mov", "avi", "m4v", "flv", "wmv", "3gp", "ts"}, aud = {"mp3", "m4a", "aac", "ogg", "opus", "wav", "flac", "wma"},
                                          pic = {"jpg", "jpeg", "png", "bmp", "gif", "tif", "tiff", "webp"};
        std::string e = lower(extOf(t.fileName));
        MenuItem ft; ft.text = "File tools";
        auto addTool = [&](const std::string& key, const std::string& label) { ft.sub.push_back({label, "", static_cast<UINT>(CmdToolBase + toolKeys.size())}); toolKeys.push_back(key); };
        if (vid.count(e)) { addTool("compress-video", "Compress video…"); addTool("mp3", "Convert to MP3"); addTool("trim", "Trim…"); addTool("gif", "Video to GIF…"); }
        if (aud.count(e)) { if (e != "mp3") addTool("mp3", "Convert to MP3"); addTool("smaller-audio", "Smaller audio…"); addTool("trim", "Trim…"); }
        if (pic.count(e)) addTool("compress-picture", "Compress picture");
        if (pic.count(e) && e != "webp") addTool("remove-bg", "Remove background");
        if (tools::canExtract(t.fileName)) addTool("extract", "Extract here");
        if (tools::canStripMetadata(t.fileName)) addTool("strip-meta", "Remove metadata…");
        addTool("zip", "Compress into a ZIP");
        ft.sub.push_back(MenuItem::sep());
        ft.sub.push_back({"Verify checksum…", "", CmdVerify});
        ft.sub.push_back({"All file tools…", "", CmdFileTools});
        v.push_back(MenuItem::sep());
        v.push_back(ft);
    }
    if (done) { v.push_back(MenuItem::sep()); v.push_back({"Scan for viruses", "", CmdScan}); v.push_back({"Verify checksum…", "", CmdVerify}); v.push_back({"Move to private vault", "", CmdToVault}); }
    v.push_back(MenuItem::sep());
    v.push_back({"Properties", "", CmdProperties});
    UINT c = popupMenu(h, v, pt);
    if (c >= CmdToolBase && c < CmdToolBase + toolKeys.size()) { fileTool(h, toolKeys[c - CmdToolBase], t.path()); return; }
    if (c) command(c);
}

void MainWindow::deleteSelected(bool withFile) {
    auto sel = list.selection();
    if (sel.empty()) return;
    Task one;
    std::string what = sel.size() == 1 && Engine::get().find(sel[0], one) ? "\"" + one.fileName + "\"" : std::to_string(sel.size()) + " downloads";
    int r = askBox(h, withFile ? "Remove and delete" : "Remove", "Remove " + what + " from the download list?" +
                   std::string(withFile ? "\n\nThe downloaded " + std::string(sel.size() == 1 ? "file goes" : "files go") + " to the Recycle Bin (you can restore " + (sel.size() == 1 ? "it" : "them") + " from there)." :
                                          "\n\nFinished files stay on your disk; unfinished parts are cleaned up."),
                   {withFile ? "Remove and delete" : "Remove", "Cancel"}, 0);
    if (r != 0) return;
    removeWithUndo(sel, withFile, 0, what);
}

// ---------------------------------------------------------------- removal with Undo
void MainWindow::removeWithUndo(const std::vector<std::string>& ids, bool withFile, int torrentMode, const std::string& what) {
    if (ids.empty()) return;
    commitPendingDelete();      // an earlier removal still waiting: it goes now
    UpdateWindow(h);            // the question box is gone from the picture before the rows turn to dust
    list.dustRemove(ids);
    pending.ids = ids; pending.withFile = withFile; pending.torrentMode = torrentMode;
    pendingIds.clear();
    pendingIds.insert(ids.begin(), ids.end());
    std::string n = ids.size() == 1 ? what : std::to_string(ids.size()) + " downloads";
    toastText = "Removed " + n + (torrentMode == 2 || withFile ? " and " + std::string(ids.size() == 1 ? "its" : "their") + " files" : torrentMode == 1 ? " and the .torrent file" : "");
    refresh();
    showToast();
}

void MainWindow::commitPendingDelete() {
    if (pending.ids.empty()) return;
    PendingDelete p = pending;
    pending = PendingDelete();
    pendingIds.clear();
    if (toast) { DestroyWindow(toast); toast = nullptr; }
    KillTimer(h, 0x7B1);
    for (auto& id : p.ids) {
        Task t;
        bool have = Engine::get().find(id, t);
        bool tor = have && t.kind == "torrent" && p.torrentMode > 0;
        Engine::get().remove(id, tor ? false : p.withFile);
        if (tor) {   // after the torrent let go of its files: its data and/or .torrent to the Recycle Bin
            int mode = p.torrentMode;
            std::thread([t, mode] {
                for (int i = 0; i < 12; i++) {
                    Sleep(i == 0 ? 1500 : 1000);
                    torrentDeleteFiles(t, mode == 2, true);
                    if (mode != 2 || (!fileExists(t.path()) && !dirExists(t.path()))) break;
                }
            }).detach();
        }
    }
    refresh();
}

void MainWindow::undoDelete() {
    if (pending.ids.empty()) return;
    pending = PendingDelete();
    pendingIds.clear();
    if (toast) { DestroyWindow(toast); toast = nullptr; }
    KillTimer(h, 0x7B1);
    list.cancelDust();
    refresh();
    setStatusNote("Brought back");
}

LRESULT CALLBACK MainWindow::toastProc(HWND hw, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    switch (m) {
        case WM_NCCREATE: SetWindowLongPtrW(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams)); break;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR: {
            POINT pt; GetCursorPos(&pt); ScreenToClient(hw, &pt);
            SetCursor(LoadCursorW(nullptr, self && PtInRect(&self->toastUndo, pt) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_MOUSEMOVE: if (self && !self->toastHover) { self->toastHover = true; TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hw, 0}; TrackMouseEvent(&t); InvalidateRect(hw, nullptr, FALSE); } return 0;
        case WM_MOUSELEAVE: if (self) { self->toastHover = false; InvalidateRect(hw, nullptr, FALSE); } return 0;
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (self && PtInRect(&self->toastUndo, pt)) self->undoDelete();
            return 0;
        }
        case WM_PAINT: {
            if (!self) break;
            Paint p(hw);
            HDC dc = p.dc();
            RECT r = p.rc();
            // a calm, inverted card: dark on the light look, light on the dark look
            COLORREF bg = isDark() ? rgb(0xE9EDF3) : rgb(0x1F2430), fg = isDark() ? rgb(0x161A22) : rgb(0xF4F6FA);
            COLORREF accent = isDark() ? rgb(0x0B66D0) : rgb(0x7DB7FF);
            fill(dc, r, P.win);
            fillRound(dc, r, Sf(12), bg);
            RECT ic{S(16), (r.bottom - S(20)) / 2 - S(1), S(36), (r.bottom - S(20)) / 2 + S(19)};
            icon(dc, "delete", ic, blend(fg, bg, 0.3f), 1.7f);
            SIZE us = textSize(dc, "Undo", fontBold());
            RECT ub{r.right - us.cx - S(46), S(9), r.right - S(10), r.bottom - S(11)};
            self->toastUndo = ub;
            if (self->toastHover) fillRound(dc, ub, Sf(8), blend(accent, bg, 0.82f));
            RECT ui{ub.left + S(9), (ub.top + ub.bottom) / 2 - S(8), ub.left + S(25), (ub.top + ub.bottom) / 2 + S(8)};
            icon(dc, "undo", ui, accent, 2.0f);
            RECT ut{ui.right + S(4), ub.top, ub.right - S(6), ub.bottom};
            text(dc, "Undo", ut, accent, fontBold());
            RECT tr{S(46), 0, ub.left - S(10), r.bottom - S(2)};
            text(dc, self->toastText, tr, fg, fontUI(), DT_END_ELLIPSIS);
            // the time left to undo, as a line that runs out
            ULONGLONG now = GetTickCount64();
            double left = self->toastUntil > now ? static_cast<double>(self->toastUntil - now) / 3000.0 : 0.0;
            RECT bar{S(14), r.bottom - S(5), S(14) + static_cast<LONG>((r.right - S(28)) * std::min(1.0, left)), r.bottom - S(3)};
            if (bar.right > bar.left) fillRound(dc, bar, Sf(1), accent);
            return 0;
        }
    }
    return DefWindowProcW(hw, m, w, l);
}

void MainWindow::showToast() {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = toastProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"DDMToast";
        RegisterClassExW(&wc);
        reg = true;
    }
    if (toast) { DestroyWindow(toast); toast = nullptr; }
    toastHover = false;
    toastShownAt = GetTickCount64();
    toastUntil = toastShownAt + 3000;
    toast = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DDMToast", L"", WS_POPUP, 0, 0, S(420), S(52), h, nullptr, GetModuleHandleW(nullptr), this);
    if (!toast) { commitPendingDelete(); return; }
    placeToast(0.0);
    ShowWindow(toast, SW_SHOWNOACTIVATE);
    SetTimer(h, 0x7B1, 16, nullptr);
}

void MainWindow::placeToast(double slide) {
    if (!toast) return;
    RECT r = client();
    int treeW = treeWidthNow();
    int listW = std::max(0, static_cast<int>(r.right) - treeW);
    int w = std::min(S(480), std::max(S(240), listW - S(40))), ht = S(52);
    POINT at{treeW + (listW - w) / 2, r.bottom - S(26) - S(20) - ht + static_cast<int>((1.0 - slide) * S(18))};
    ClientToScreen(h, &at);
    HRGN rg = CreateRoundRectRgn(0, 0, w + 1, ht + 1, S(24), S(24));
    SetWindowRgn(toast, rg, FALSE);
    SetWindowPos(toast, nullptr, at.x, at.y, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
    SetLayeredWindowAttributes(toast, 0, static_cast<BYTE>(std::max(0.0, std::min(1.0, slide)) * 250), LWA_ALPHA);
    InvalidateRect(toast, nullptr, FALSE);
}

// ---------------------------------------------------------------- View → Hide / Show categories
int MainWindow::treeWidthNow() const {
    double e = treeShown;
    e = e * e * (3 - 2 * e);   // eased
    return static_cast<int>(S(settings().treeWidth) * e + 0.5);
}

void MainWindow::toggleTree() {
    settings().hideTree = !settings().hideTree;
    saveSettings();
    treeAnimFrom = treeShown;
    treeAnimStart = GetTickCount64();
    SetTimer(h, 0x7B2, 15, nullptr);
}

// ---------------------------------------------------------------- Move / rename a finished file
void MainWindow::moveRename() {
    auto sel = list.selection();
    if (sel.size() != 1) return;
    Task t;
    if (!Engine::get().find(sel[0], t) || t.state != State::Complete) return;
    std::string from = t.path();
    if (!fileExists(from) && !dirExists(from)) { askBox(h, "Move / rename", "The file isn't there any more.", {"OK"}, 0); return; }
    std::string to;
    if (t.kind == "torrent") {   // a torrent keeps its name (its files are named inside it): only the folder changes
        std::string dir = pickFolder(h, t.dir);
        if (dir.empty() || iequals(dir, t.dir)) return;
        to = joinPath(dir, t.fileName);
    } else {
        wchar_t file[MAX_PATH * 2] = {};
        wcsncpy(file, W(t.fileName).c_str(), MAX_PATH * 2 - 1);
        std::wstring dir = W(t.dir), ext = W(extOf(t.fileName));
        std::wstring filter = (ext.empty() ? std::wstring(L"All files") : L"." + ext + L" file") + std::wstring(1, L'\0') + (ext.empty() ? std::wstring(L"*.*") : L"*." + ext) + std::wstring(1, L'\0') + L"All files" + std::wstring(1, L'\0') + L"*.*" + std::wstring(2, L'\0');
        OPENFILENAMEW of{};
        of.lStructSize = sizeof of;
        of.hwndOwner = h;
        of.lpstrFile = file;
        of.nMaxFile = MAX_PATH * 2;
        of.lpstrInitialDir = dir.c_str();
        of.lpstrFilter = filter.c_str();
        of.lpstrTitle = L"Move or rename";
        of.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
        of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetSaveFileNameW(&of)) return;
        to = U(file);
    }
    if (iequals(to, from)) return;
    bool seeding = t.kind == "torrent" && torrentSeeding(t.id);
    std::string id = t.id;
    HWND hw = h;
    setStatusNote("Moving " + t.fileName + "…");
    std::thread([id, from, to, seeding, hw] {
        if (seeding) { stopSeeding(id); Sleep(800); }
        makeDirs(dirName(to));
        if (fileExists(to) && !iequals(to, from)) DeleteFileW(W(to).c_str());   // (you chose to replace it)
        bool ok = MoveFileExW(W(from).c_str(), W(to).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != 0;
        if (ok) Engine::get().edit(id, [&](Task& k) { k.dir = dirName(to); k.fileName = baseName(to); });
        if (seeding) startSeedingNow(id);
        auto* note = new std::string(ok ? "Moved to " + to : "!Couldn't move the file (is it open in another program?).");
        if (!PostMessageW(hw, WM_APP + 48, 0, reinterpret_cast<LPARAM>(note))) delete note;
    }).detach();
}

void MainWindow::command(UINT id) {
    if (id >= CmdQueueBase && id < CmdQueueBase + 100) {
        std::string qid;
        { std::lock_guard<std::mutex> g(queues::lock()); size_t i = id - CmdQueueBase; if (i < queues::all().size()) qid = queues::all()[i].id; }
        if (!qid.empty()) for (auto& i : list.selection()) Engine::get().addToQueue(i, qid);
        return;
    }
    if (id >= CmdPriorityBase && id < CmdPriorityBase + 3) { for (auto& i : list.selection()) Engine::get().setPriority(i, static_cast<int>(id - CmdPriorityBase)); return; }
    Engine& e = Engine::get();
    auto sel = list.selection();
    auto setLimit = [this](int kb) { settings().speedLimitKBps = kb; saveSettings(); Engine::get().applySpeedLimit(); refresh(); };
    switch (id) {
        case CmdAddUrl: App::get().addUrlDialog(); break;
        case CmdPasteUrl: {
            std::string clip;
            if (OpenClipboard(h)) {
                HANDLE d = GetClipboardData(CF_UNICODETEXT);
                if (d) { auto p = static_cast<const wchar_t*>(GlobalLock(d)); if (p) clip = U(p); GlobalUnlock(d); }
                CloseClipboard();
            }
            auto urls = extractUrls(clip);
            if (urls.empty()) { App::get().addUrlDialog(); break; }
            for (auto& u : urls) { LinkRequest r; r.url = u; r.source = "clipboard"; App::get().newDownload(r); }
            break;
        }
        case CmdResume: for (auto& i : sel) e.start(i); break;
        case CmdPause: for (auto& i : sel) e.pause(i); break;
        case CmdPauseAll: e.pauseAll(); break;
        case CmdResumeAll: e.startAll(); break;
        case CmdStopQueue: e.stopQueue("main"); break;
        case CmdStartQueue: e.startQueue("main"); break;
        case CmdDelete: deleteSelected(false); break;
        case CmdDeleteWithFile: deleteSelected(true); break;
        case CmdDeleteFinished:
            if (askBox(h, "Delete finished", "Remove all finished downloads from the list? The files stay on your PC.", {"Remove", "Cancel"}, 0) == 0) {
                std::vector<std::string> done;
                for (auto& t : list.items()) if (t.state == State::Complete) done.push_back(t.id);
                if (done.empty()) e.removeFinished();
                else { Task one; removeWithUndo(done, false, 0, done.size() == 1 && e.find(done[0], one) ? "\"" + one.fileName + "\"" : "1 download"); }
            }
            break;
        case CmdOptions: App::get().showOptions(); break;
        case CmdScheduler: showScheduler(h); break;
        case CmdXRay: showXRayFor(h, sel.empty() ? "" : sel.front()); break;
        case CmdGrabber: askBox(h, "Grabber", "Right-click any page in your browser and choose \"Download all links with DDM\" to pick files from it.", {"OK"}, 0); break;
        case CmdExit: App::get().quit(); break;
        case CmdShow: showFromTray(); break;
        case CmdOpen: if (!sel.empty()) App::get().openFile(sel.front()); break;
        case CmdOpenFolder: if (!sel.empty()) App::get().openFolder(sel.front()); break;
        case CmdShowProgress: if (!sel.empty()) App::get().showProgress(sel.front()); break;
        case CmdRedownload: for (auto& i : sel) e.redownload(i); break;
        case CmdCopyUrl: {
            std::string all;
            for (auto& i : sel) { Task t; if (e.find(i, t)) all += (all.empty() ? "" : "\r\n") + t.url; }
            ignoreClipboard = true;
            if (OpenClipboard(h)) {
                EmptyClipboard();
                std::wstring w = W(all);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
                if (g) { memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                CloseClipboard();
            }
            break;
        }
        case CmdRefreshLink: if (!sel.empty()) refreshLinkDialog(h, sel.front()); break;
        case CmdProperties: if (!sel.empty()) propertiesDialog(h, sel.front()); break;
        case CmdThemeLight: App::get().setTheme("light"); break;
        case CmdThemeDark: App::get().setTheme("dark"); break;
        case CmdThemeSystem: App::get().setTheme("system"); break;
        case CmdToggleTheme: App::get().toggleTheme(); break;
        case CmdHelp: App::get().showHelp(); break;
        case CmdAbout: App::get().showAbout(); break;
        case CmdFind: showFind(h); break;
        case CmdVault: showVault(h); break;
        case CmdSecurity: showOptionsWindow(h, 6); break;
        case CmdFileTools: showFileTools(h); break;
        case CmdStats: showStatistics(h); break;
        case CmdBrowser: showOptionsWindow(h, 5); break;
        case CmdForceStart: for (auto& i : sel) e.forceStart(i); break;
        case CmdRestart:
            if (askBox(h, "Restart", "The downloaded data goes to the Recycle Bin and the download starts again from 0%.", {"Restart", "Cancel"}, 1) == 0)
                for (auto& i : sel) { std::string id = i; std::thread([id] { Engine::get().restart(id); }).detach(); }
            break;
        case CmdQueueRemove: for (auto& i : sel) e.removeFromQueue(i); break;
        case CmdScan: for (auto& i : sel) after::scanNow(i); break;
        case CmdVerify: if (!sel.empty()) verifyChecksumDialog(h, sel.front()); break;
        case CmdTorrentDetails: if (!sel.empty()) showTorrentWindow(h, sel.front()); break;
        case CmdSeedToggle:
            if (!sel.empty()) {
                std::string id = sel.front();
                if (torrentSeeding(id)) std::thread([id] { stopSeeding(id); Engine::get().edit(id, [](Task&) {}); }).detach();
                else startSeedingNow(id);
            }
            break;
        case CmdCreateTorrent: showCreateTorrent(h, ""); break;
        case CmdPauseResume: {   // the toolbar's one button: pause what's downloading, else resume
            bool anyActive = false;
            for (auto& i : sel) { Task t; if (e.find(i, t) && (t.state == State::Running || t.state == State::Queued)) anyActive = true; }
            for (auto& i : sel) { if (anyActive) e.pause(i); else e.start(i); }
            break;
        }
        case CmdPauseResumeAll: {
            bool anyActive = false;
            for (auto& t : e.snapshot()) if (!t.hidden && !pendingIds.count(t.id) && (t.state == State::Running || t.state == State::Queued)) anyActive = true;
            if (anyActive) e.pauseAll(); else e.startAll();
            break;
        }
        case CmdStop: for (auto& i : sel) e.stop(i); break;
        case CmdMoveRename: moveRename(); break;
        case CmdCopyMagnet: {
            std::string all;
            for (auto& i : sel) { std::string m = torrentMagnet(i); if (!m.empty()) all += (all.empty() ? "" : "\r\n") + m; }
            if (all.empty()) { setStatusNote("No magnet link for this torrent yet"); break; }
            ignoreClipboard = true;
            if (OpenClipboard(h)) {
                EmptyClipboard();
                std::wstring w = W(all);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
                if (g) { memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
                CloseClipboard();
            }
            setStatusNote("Magnet link copied");
            break;
        }
        case CmdDelTorrentFile: case CmdDelTorrentAll: {
            if (sel.empty()) break;
            Task one;
            std::string what = sel.size() == 1 && e.find(sel[0], one) ? "\"" + one.fileName + "\"" : std::to_string(sel.size()) + " torrents";
            bool all = id == CmdDelTorrentAll;
            if (askBox(h, all ? "Delete torrent + data" : "Delete the .torrent file", "Remove " + what + " from the list and send " +
                       (all ? std::string("its downloaded files and its .torrent file") : std::string("its .torrent file")) + " to the Recycle Bin?", {"Delete", "Cancel"}, 1) != 0) break;
            removeWithUndo(sel, false, all ? 2 : 1, what);
            break;
        }
        case CmdDelTorrentData: {
            if (sel.empty()) break;
            if (askBox(h, "Delete only the data", "The downloaded files go to the Recycle Bin. The torrent stays in the list, paused at 0%: start it to download again.", {"Delete data", "Cancel"}, 1) != 0) break;
            for (auto& i : sel) torrentDeleteData(i);
            break;
        }
        case CmdToggleTree: toggleTree(); break;
        case CmdAddCategory: newCategory(); break;
        case CmdPlaylist: askPlaylist(h); break;
        case CmdSubscriptions: showSubscriptions(h); break;
        case CmdRemote: showRemote(h); break;
        case CmdPlay: {   // (a paused download continues: playing it needs what comes next)
            auto s = list.selection();
            if (s.empty()) break;
            Task pt;
            if (Engine::get().find(s.front(), pt) && pt.state == State::Paused) Engine::get().start(s.front());
            showPlayer(h, s.front());
            break;
        }
        case CmdUndoDelete: undoDelete(); break;
        case CmdToVault: moveToVault(h, sel); break;
        case CmdFindNext: findAgain(h, true); break;
        case CmdFindPrev: findAgain(h, false); break;
        case CmdUpdates: App::get().checkUpdatesNow(h); break;
        case CmdLimitOff: setLimit(0); break;
        case CmdLimit256: setLimit(256); break;
        case CmdLimit512: setLimit(512); break;
        case CmdLimit1M: setLimit(1024); break;
        case CmdLimit2M: setLimit(2048); break;
        case CmdLimit5M: setLimit(5120); break;
        case CmdLimit10M: setLimit(10240); break;
    }
    refresh();
}

void MainWindow::onThemeChanged() {
    styleWindow(h);
    for (auto& t : toolbar.tools) {
        if (t.id == CmdAddUrl || t.id == CmdXRay) t.color = P.accent;
        else if (t.id == CmdResume || t.id == CmdStartQueue) t.color = P.good;
        else if (t.id == CmdPause || t.id == CmdPauseAll) t.color = P.warn;
        else if (t.id == CmdDelete || t.id == CmdDeleteFinished || t.id == CmdStopQueue) t.color = P.bad;
        else t.color = P.muted;
        if (t.id == CmdToggleTheme) { t.label = isDark() ? "Light mode" : "Dark mode"; t.icon = isDark() ? "sun" : "moon"; }
        if (t.id == CmdStop) t.color = P.bad;
        if (t.id == CmdPauseResume || t.id == CmdPauseResumeAll) t.color = t.icon == "resume" || t.icon == "resumeall" ? P.good : P.warn;
    }
    styleScroll(list.hwnd());
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
    lastTrayPct = -2;
    updateTray();
}

LRESULT MainWindow::proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: return eraseWith(h, reinterpret_cast<HDC>(w), P.line);
        case WM_DDM_BG: return bgReply(P.win);
        case WM_SIZE:
            if (w == SIZE_MINIMIZED) commitPendingDelete();   // the Undo card is shown only while the window is
            if (w == SIZE_MINIMIZED && settings().minimizeToTray) { ShowWindow(h, SW_HIDE); trimMemory(); return 0; }
            layout();
            return 0;
        case WM_MOVE: if (toast) placeToast(1.0); break;
        case WM_SHOWWINDOW: if (!w) commitPendingDelete(); break;   // hidden (tray): the Undo card goes too
        case WM_GETMINMAXINFO: { auto mm = reinterpret_cast<MINMAXINFO*>(l); mm->ptMinTrackSize = POINT{S(760), S(420)}; return 0; }
        case WM_SETCURSOR: {
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            int tw = S(settings().treeWidth);
            if (LOWORD(l) == HTCLIENT && !settings().hideTree && treeShown >= 1.0 && abs(pt.x - tw) <= S(3) && pt.y > S(96)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            break;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l);
            if (!settings().hideTree && treeShown >= 1.0 && abs(x - S(settings().treeWidth)) <= S(3)) { splitDrag = x; SetCapture(h); }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (splitDrag >= 0) { int x = GET_X_LPARAM(l); settings().treeWidth = std::max(150, std::min(420, MulDiv(x, 96, dpi()))); layout(); }
            return 0;
        case WM_LBUTTONUP: if (splitDrag >= 0) { splitDrag = -1; ReleaseCapture(); saveSettings(); } return 0;
        case 0x031D: onClipboard(); return 0;   // WM_CLIPBOARDUPDATE
        case WM_TIMER:
            if (w == 77) { nudges(); trimMemory(); return 0; }
            if (w == 78) { KillTimer(h, 78); nudges(); return 0; }
            if (w == 79) { installReadyUpdate(); return 0; }
            if (w == 0x7B1) {   // the Undo card: in, wait 3 s (longer while the mouse is on it), out, then the removal happens
                ULONGLONG now = GetTickCount64();
                if (!toast) { KillTimer(h, 0x7B1); return 0; }
                if (toastHover && toastUntil < now + 900) toastUntil = now + 900;
                double in = std::min(1.0, static_cast<double>(now - toastShownAt) / 180.0);
                if (now < toastUntil) { placeToast(1 - (1 - in) * (1 - in)); return 0; }
                double out = static_cast<double>(now - toastUntil) / 160.0;
                if (out < 1.0) { placeToast(1.0 - out * out); return 0; }
                commitPendingDelete();
                return 0;
            }
            if (w == 0x7B2) {   // the categories slide away / back
                double target = settings().hideTree ? 0.0 : 1.0;
                double t = std::min(1.0, static_cast<double>(GetTickCount64() - treeAnimStart) / 200.0);
                treeShown = treeAnimFrom + (target - treeAnimFrom) * t;
                if (t >= 1.0) { treeShown = target; KillTimer(h, 0x7B2); }
                layout();
                UpdateWindow(h);
                return 0;
            }
            if (w == 0x7B3) {   // between the engine's figures: draw the speeds where they have glided to
                if (!IsWindowVisible(h) || IsIconic(h)) return 0;
                std::string s = g_totalSpeed > 0 ? "↓ " + formatKBps(glidedSpeed("*all*", g_totalSpeed)) : "";
                if (s != status.speed) { status.speed = s; status.redraw(); }
                list.redraw();
                return 0;
            }
            if (IsWindowVisible(h)) refresh(); else updateTray();
            return 0;
        case WM_CLOSE:
            if (settings().closeToTray) { hideToTray(); return 0; }
            App::get().quit();
            return 0;
        case WM_COMMAND: command(LOWORD(w)); return 0;
        case WM_KEYDOWN: break;
        case WM_SETTINGCHANGE:
            if (l && settings().theme == "system" && wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0) App::get().setTheme("system");
            break;
        case WM_ENDSESSION: if (w) App::get().quit(); return 0;
    }
    if (m == App::WM_ENGINE) {
        std::unique_ptr<std::string> id(reinterpret_cast<std::string*>(l));
        auto ev = static_cast<EngineEvent>(w);
        if (ev == EngineEvent::Completed && id) {
            Task t;
            if (Engine::get().find(*id, t)) {
                if (!hasProgressWindow(*id) && settings().showComplete) App::get().showComplete(*id);
                if (t.openWhenDone) App::get().openFile(*id);
                if (t.shutdownWhenDone) powerAction(h, "shutdown");
            }
        }
        refresh();
        return 0;
    }
    if (m == App::WM_LINK) {
        std::unique_ptr<LinkRequest> r(reinterpret_cast<LinkRequest*>(l));
        if (r) App::get().newDownload(*r);
        return 0;
    }
    if (m == App::WM_LINKS) {
        std::unique_ptr<LinkBatch> b(reinterpret_cast<LinkBatch*>(l));
        if (b) showLinksPicker(*b);
        return 0;
    }
    if (m == App::WM_FAVICON) { list.redraw(); return 0; }
    if (m == WM_APP + 42) {   // a queue finished: its "when done" actions
        std::unique_ptr<Queue> q(reinterpret_cast<Queue*>(l));
        if (q->openFileOn && !q->openFile.empty()) ShellExecuteW(nullptr, L"open", W(q->openFile).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (q->hangUp) hangUpModem();
        if (q->turnOffOn) powerAction(h, q->turnOffMode, q->forceClose, q->name + " is finished");
        else if (q->exitWhenDone) App::get().quit();
        return 0;
    }
    if (m == WM_APP + 44) { refreshProbed(l); return 0; }
    if (m == WM_APP + 45) { verifyDone(h, l); return 0; }
    if (m == WM_APP + 46) {   // an add-on said hello
        std::unique_ptr<std::string> b(reinterpret_cast<std::string*>(l));
        std::string list = "," + settings().addonBrowsers + ",";
        if (!b->empty() && b->size() < 20 && list.find("," + *b + ",") == std::string::npos) { settings().addonBrowsers += (settings().addonBrowsers.empty() ? "" : ",") + *b; saveSettings(); }
        return 0;
    }
    if (m == WM_APP + 43) { std::unique_ptr<std::pair<std::string, std::string>> n(reinterpret_cast<std::pair<std::string, std::string>*>(l)); notify(n->first, n->second); return 0; }
    if (m == WM_APP + 41) { std::unique_ptr<std::string> a(reinterpret_cast<std::string*>(l)); powerAction(h, *a); return 0; }
    if (m == WM_APP + 80) {   // the phone remote asked
        std::unique_ptr<remote::Ask> a(reinterpret_cast<remote::Ask*>(l));
        if (a->action == "pause") Engine::get().pause(a->id);
        else if (a->action == "resume") Engine::get().start(a->id);
        else if (a->action == "stop") Engine::get().stop(a->id);
        else if (a->action == "remove") Engine::get().remove(a->id, false);
        else if (a->action == "pauseall") Engine::get().pauseAll();
        else if (a->action == "resumeall") Engine::get().startAll();
        else if (a->action == "add") {
            std::string url = a->url;
            if (isVideoSite(url) || playlists::isListUrl(url)) {   // a video: its title first (that's its file name)
                HWND hw = h;
                std::thread([url, hw] {
                    VideoRequest vr;
                    vr.url = url;
                    VideoInfo vi;
                    probeVideo(vr, vi);
                    std::string q = settings().videoQuality == "mp3" ? "MP3" : (settings().videoQuality == "best" || settings().videoQuality == "ask") ? "" : settings().videoQuality + "p";
                    auto* t = new Task(playlists::videoTask(url, vi.title.empty() ? stem(fileNameFromUrl(url)) : vi.title, q, ""));
                    if (!PostMessageW(hw, WM_APP + 82, 0, reinterpret_cast<LPARAM>(t))) delete t;
                }).detach();
            } else {
                LinkRequest r;
                r.url = url;
                r.source = "remote";
                App::get().newDownload(r);
            }
            setStatusNote("Added from your phone");
        }
        refresh();
        return 0;
    }
    if (m == WM_APP + 81) {   // new videos of your subscriptions
        std::unique_ptr<std::vector<Task>> v(reinterpret_cast<std::vector<Task>*>(l));
        for (auto& t : *v) Engine::get().add(t, true);
        if (!v->empty()) notify("Subscriptions", std::to_string(v->size()) + (v->size() == 1 ? " new video is" : " new videos are") + " downloading.");
        refresh();
        return 0;
    }
    if (m == WM_APP + 82) {   // a video added from the phone
        std::unique_ptr<Task> t(reinterpret_cast<Task*>(l));
        Engine::get().add(*t, true);
        refresh();
        return 0;
    }
    if (m == App::WM_UPDATE) {
        if (w == 5) {   // this DDM replaced an older one
            std::unique_ptr<std::string> prev(reinterpret_cast<std::string*>(l));
            auto u = update::check(false);
            std::string body = i18n::tr("DDM was updated to " DDM_VERSION " (from " + *prev + "). The old version was replaced and its installer removed; your downloads and settings stayed as they were.") +
                               (u.version == DDM_VERSION && !u.notes.empty() ? "\n\n" + i18n::tr("What's new:") + "\n" + u.notes : std::string());
            askBox(IsWindowVisible(h) ? h : nullptr, "DDM is up to date", body, {"OK"}, 0);
            return 0;
        }
        if (w == 3) {   // updating by itself: installed as soon as nothing is downloading
            std::unique_ptr<std::string> path(reinterpret_cast<std::string*>(l));
            readyUpdate = *path;
            SetTimer(h, 79, 60 * 1000, nullptr);
            installReadyUpdate();
            return 0;
        }
        if (w == 2) {   // the installer arrived
            std::unique_ptr<std::string> path(reinterpret_cast<std::string*>(l));
            if (!path->empty() && (*path)[0] == '!') { askBox(h, "Update", "The update couldn't be downloaded: " + path->substr(1), {"OK"}, 0); return 0; }
            auto u = update::check(false);
            std::string what = u.version.empty() ? "A new DDM" : "DDM " + u.version;
            if (askBox(IsWindowVisible(h) ? h : nullptr, "Update ready", what + " is downloaded and checked (signed by DedSec).\n\nInstall it now? DDM closes for a few seconds and starts again by itself; unfinished downloads continue where they were." + (u.notes.empty() ? std::string() : "\n\nWhat's new:\n" + u.notes), {"Install now", "Later"}, 0) == 0) {
                // quietly: the installer replaces DDM and starts it again
                if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", W(*path).c_str(), L"/S", nullptr, SW_SHOWNORMAL)) > 32) App::get().quit();
                else askBox(h, "Update", "The installer couldn't be started. Run it yourself: " + *path, {"OK"}, 0);
            }
            return 0;
        }
        App::get().offerUpdate(w == 1, w == 4);
        return 0;
    }
    if (m == WM_APP + 9) {   // tests only (DDM_TEST_UI)
        std::unique_ptr<std::string> j(reinterpret_cast<std::string*>(l));
        Json t = Json::parse(*j);
        std::string a = t["action"].str(), id = t["id"].str();
        if (id.empty() && !t["name"].str().empty()) for (auto& x : Engine::get().snapshot()) if (x.fileName == t["name"].str()) id = x.id;
        if (a == "pause") Engine::get().pause(id);
        else if (a == "start") Engine::get().start(id);
        else if (a == "seturl") Engine::get().refreshLink(id, t["url"].str());
        else if (a == "refresh") refreshLinkDialog(h, id);
        else if (a == "props") propertiesDialog(h, id);
        else if (a == "torrent") showTorrentWindow(h, id);
        else if (a == "create-torrent") showCreateTorrent(h, t["file"].str());
        else if (a == "tools") showFileTools(h);
        else if (a == "stats") showStatistics(h);
        else if (a == "cmd") PostMessageW(h, WM_COMMAND, static_cast<WPARAM>(t["cmd"].i32()), 0);
        else if (a == "xray") { XRayRequest x; x.url = t["url"].str(); x.local = t["local"].boolean(); x.name = t["file"].str(); showXRay(h, x, nullptr); }
        else if (a == "progress") App::get().showProgress(id);
        else if (a == "complete") App::get().showComplete(id);
        else if (a == "options") showOptionsWindow(h, t["tab"].i32());
        else if (a == "scheduler") showScheduler(h);
        else if (a == "find") showFind(h);
        else if (a == "vault") showVault(h);
        else if (a == "helptopic") showHelpTopic(h, t["topic"].str());
        else if (a == "addurl") App::get().addUrlDialog();
        else if (a == "ask") { std::vector<std::string> b; for (auto& x : t["buttons"].list()) b.push_back(x.str()); askBox(h, t["title"].str(), t["text"].str(), b, t["def"].i32()); }
        else if (a == "addonwnd") {
            std::vector<integration::BrowserInfo> v;
            for (auto& x : t["browsers"].list()) { integration::BrowserInfo b; b.key = x["key"].str(); b.name = x["name"].str(); b.installed = true; v.push_back(b); }
            showAddonReminder(h, v);
        }
        else if (a == "help") App::get().showHelp();
        else if (a == "about") PostMessageW(h, WM_COMMAND, CmdAbout, 0);
        else if (a == "theme") App::get().setTheme(t["mode"].str());
        else if (a == "select") { tree.selected = t["key"].str(); refresh(); }
        else if (a == "tray") trayMenu();
        else if (a == "context") { list.setItems(Engine::get().snapshot()); POINT pt{S(600), S(300)}; ClientToScreen(h, &pt); contextMenu(pt); }
        else if (a == "close") { for (HWND w2 = GetTopWindow(nullptr); w2; w2 = GetNextWindow(w2, GW_HWNDNEXT)) { DWORD pid = 0; GetWindowThreadProcessId(w2, &pid); wchar_t c[32]; GetClassNameW(w2, c, 32); if (pid == GetCurrentProcessId() && wcscmp(c, L"DDMDialog") == 0 && IsWindowVisible(w2)) { PostMessageW(w2, WM_CLOSE, 0, 0); } } }
        else if (a == "hide") hideToTray();
        else if (a == "showmain") showFromTray();
        else if (a == "progresstab") { for (HWND w2 = GetTopWindow(nullptr); w2; w2 = GetNextWindow(w2, GW_HWNDNEXT)) { wchar_t c[32]; GetClassNameW(w2, c, 32); if (wcscmp(c, L"DDMDialog") == 0 && IsWindowVisible(w2)) { int i = t["tab"].i32(); PostMessageW(w2, WM_LBUTTONDOWN, 0, MAKELPARAM(S(30) + i * S(130), S(30))); break; } } }
        return 0;
    }
    if (m == App::WM_TRAY) {
        if (l == WM_LBUTTONUP && trayPanelWanted()) toggleTrayPanel();   // like IDM's one icon: every download, live
        else if (l == WM_LBUTTONUP || l == WM_LBUTTONDBLCLK) { closeTrayPanel(); if (!restoreProgressFromTray()) showFromTray(); }   // a download window waiting in the tray comes back first
        else if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) trayMenu();
        return 0;
    }
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (m == taskbarCreated) { trayAdded = false; addTray(); return 0; }
    if (m == WM_APP + 48) {   // Move / rename finished
        std::unique_ptr<std::string> n(reinterpret_cast<std::string*>(l));
        if (!n->empty() && (*n)[0] == '!') askBox(h, "Move / rename", n->substr(1), {"OK"}, 0); else setStatusNote(*n);
        refresh();
        return 0;
    }
    if (m == WM_DESTROY) {
        commitPendingDelete();   // a removal waiting for Undo happens now
        if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &tray);
        WINDOWPLACEMENT wp{sizeof wp};
        GetWindowPlacement(h, &wp);
        auto& s = settings();
        s.winX = wp.rcNormalPosition.left; s.winY = wp.rcNormalPosition.top;
        s.winW = MulDiv(wp.rcNormalPosition.right - wp.rcNormalPosition.left, 96, dpi());
        s.winH = MulDiv(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, 96, dpi());
        s.winMax = wp.showCmd == SW_SHOWMAXIMIZED;
        list.saveLayout();
        saveSettings();
        return 0;
    }
    return def(m, w, l);
}

} // namespace ui
} // namespace ddm
