// Find file (Ctrl+F) like IDM's, with more ways to narrow it down. F3 finds the next match, Shift+F3 the previous one.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "mainwnd.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../common/util.hpp"
#include <commctrl.h>
#include <algorithm>

namespace ddm {
namespace ui {

namespace {

struct Criteria {
    std::string text;
    bool name = true, desc = false, site = true, path = false, matchCase = false, whole = false;
    int where = 0;     // 0 everywhere, 1 finished, 2 unfinished, 3 the selected category
    int type = 0;      // 0 any, else a category key index
    bool valid() const { return !trim(text).empty(); }
};
Criteria g_last;
HWND g_open = nullptr;

const char* kTypes[] = {"Any kind of file", "Compressed", "Documents", "Music", "Programs", "Video", "Torrents", "Other"};
const char* kTypeKeys[] = {"", "compressed", "documents", "music", "programs", "video", "torrent", "general"};

bool matchOne(const std::string& field, const Criteria& c) {
    if (field.empty()) return false;
    std::string a = c.matchCase ? field : lower(field), b = c.matchCase ? trim(c.text) : lower(trim(c.text));
    if (c.whole) {
        if (a == b) return true;
        // whole word inside a longer text
        size_t p = 0;
        while ((p = a.find(b, p)) != std::string::npos) {
            bool l = p == 0 || !isalnum(static_cast<unsigned char>(a[p - 1]));
            bool r = p + b.size() >= a.size() || !isalnum(static_cast<unsigned char>(a[p + b.size()]));
            if (l && r) return true;
            p++;
        }
        return false;
    }
    return a.find(b) != std::string::npos;
}

bool matches(const Task& t, const Criteria& c) {
    if (c.where == 1 && t.state != State::Complete) return false;
    if (c.where == 2 && t.state == State::Complete) return false;
    if (c.type > 0) {
        std::string key = kTypeKeys[c.type];
        if (key == "torrent" ? t.kind != "torrent" : (t.kind == "torrent" || t.category != key)) return false;
    }
    if (c.name && matchOne(t.fileName, c)) return true;
    if (c.desc && matchOne(t.description, c)) return true;
    if (c.site && (matchOne(t.site, c) || matchOne(t.url, c) || matchOne(t.pageUrl, c) || matchOne(t.mediaUrl, c))) return true;
    if (c.path && matchOne(t.dir, c)) return true;
    return false;
}

// Selects the next match after the current selection; returns "found 2 of 5" style text, or "" when nothing matches.
std::string findIn(MainWindow* mw, const Criteria& c, bool forward) {
    if (!mw || !c.valid()) return "";
    if (c.where != 3 && mw->tree.selected != "all") { mw->tree.selected = "all"; mw->refresh(); }
    const auto& rows = mw->list.items();
    std::vector<int> hits;
    for (size_t i = 0; i < rows.size(); i++) if (matches(rows[i], c)) hits.push_back(static_cast<int>(i));
    if (hits.empty()) return "";
    std::string cur = mw->list.focusedId();
    int at = -1;
    for (size_t i = 0; i < rows.size(); i++) if (rows[i].id == cur) at = static_cast<int>(i);
    int pick = -1;
    if (forward) { for (int h : hits) if (h > at) { pick = h; break; } if (pick < 0) pick = hits.front(); }
    else { for (auto it = hits.rbegin(); it != hits.rend(); ++it) if (*it < at) { pick = *it; break; } if (pick < 0) pick = hits.back(); }
    mw->list.selectId(rows[static_cast<size_t>(pick)].id);
    int n = static_cast<int>(std::find(hits.begin(), hits.end(), pick) - hits.begin()) + 1;
    return "Found " + std::to_string(n) + " of " + std::to_string(hits.size());
}

class FindWnd : public Dialog {
public:
    Label lFind, lWhere, lType, status;
    Field what;
    CheckBox name, desc, site, path, matchCase, whole;
    DropDown where, type;
    Button find, prev, cancel;
    int sep = 0;

    void build() {
        lFind.create(h, "Find:");
        what.create(h, g_last.text);
        name.create(h, "File name or part of the name", g_last.name);
        desc.create(h, "Description or part of the description", g_last.desc);
        site.create(h, "Site name / Download link / Parent web page / Referer", g_last.site);
        path.create(h, "Folder where the file is saved", g_last.path);
        lWhere.create(h, "Look in");
        where.create(h, {"All downloads", "Finished downloads", "Unfinished downloads", "The selected category only"}, g_last.where);
        lType.create(h, "Kind of file");
        std::vector<std::string> types(std::begin(kTypes), std::end(kTypes));
        type.create(h, types, g_last.type);
        for (Label* l : {&lWhere, &lType}) l->setColor(P.muted);
        matchCase.create(h, "Match case", g_last.matchCase);
        whole.create(h, "Match whole string only", g_last.whole);
        status.create(h, "");
        status.small = true;
        status.setColor(P.muted);
        find.create(h, "Find", BtnStyle::Primary, "search");
        prev.create(h, "Previous");
        cancel.create(h, "Cancel");
        find.onClick = [this] { go(true); };
        prev.onClick = [this] { go(false); };
        cancel.onClick = [this] { close(0); };
        what.onEnter = [this] { go(true); };
        onEnter = [this] { go(true); };
        layout();
        what.focus();
        SendMessageW(what.edit(), EM_SETSEL, 0, -1);
    }

    void go(bool forward) {
        Criteria c;
        c.text = what.get();
        c.name = name.checked; c.desc = desc.checked; c.site = site.checked; c.path = path.checked;
        c.matchCase = matchCase.checked; c.whole = whole.checked;
        c.where = where.selected; c.type = type.selected;
        if (!c.valid()) { what.focus(); return; }
        if (!c.name && !c.desc && !c.site && !c.path) { status.setText("Tick at least one place to look in."); status.setColor(P.warn); return; }
        g_last = c;
        std::string r = findIn(App::get().main(), c, forward);
        status.setText(r.empty() ? "Nothing matches \"" + trim(c.text) + "\"." : r + " · F3 finds the next one");
        status.setColor(r.empty() ? P.warn : P.good);
    }

    void layout() override {
        RECT r = client();
        int pad = S(20), w = r.right - 2 * pad, y = S(14);
        lFind.move(pad, y, w, S(20)); y += S(24);
        what.move(pad, y, w, S(32)); y += S(44);
        for (CheckBox* c : {&name, &desc, &site, &path}) { c->move(pad, y, w, S(24)); y += S(30); }
        y += S(4);
        lWhere.move(pad, y, S(110), S(30)); where.move(pad + S(110), y, w - S(110), S(30)); y += S(38);
        lType.move(pad, y, S(110), S(30)); type.move(pad + S(110), y, w - S(110), S(30)); y += S(44);
        sep = y; y += S(14);
        matchCase.move(pad + S(18), y, w, S(24)); y += S(30);
        whole.move(pad + S(18), y, w, S(24)); y += S(32);
        status.move(pad, y, w, S(20));
        int bw = S(112), by = r.bottom - S(54);
        cancel.move(r.right - pad - bw, by, bw, S(36));
        prev.move(r.right - pad - 2 * bw - S(8), by, bw, S(36));
        find.move(r.right - pad - 3 * bw - S(16), by, bw, S(36));
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT ln{S(20), sep, r.right - S(20), sep + 1};
        fill(dc, ln, P.line);
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_DESTROY) g_open = nullptr;
        return Dialog::proc(m, w, l);
    }
};

} // namespace

void showFind(HWND owner) {
    if (g_open && IsWindow(g_open)) { ShowWindow(g_open, SW_SHOW); SetForegroundWindow(g_open); return; }
    auto* d = new FindWnd();
    if (!d->createDialog(owner, "Find file", S(520), S(480), true)) { delete d; return; }
    g_open = d->hwnd();
    d->build();
    ShowWindow(d->hwnd(), SW_SHOW);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<FindWnd*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
}

void findAgain(HWND owner, bool forward) {
    if (!g_last.valid()) { showFind(owner); return; }
    std::string r = findIn(App::get().main(), g_last, forward);
    if (r.empty()) { if (auto m = App::get().main()) m->setStatusNote("Nothing matches \"" + trim(g_last.text) + "\""); }
    else if (auto m = App::get().main()) m->setStatusNote(r);
}

} // namespace ui
} // namespace ddm
