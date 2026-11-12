// Private vault window: set up, unlock (the real or the decoy vault — they look the same), add, open, move out,
// delete and rename files. It locks itself when closed and after 5 minutes without use.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "rowlist.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/settings.hpp"
#include "../core/vault.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <atomic>
#include <memory>
#include <thread>

namespace ddm {
namespace ui {

namespace {

const UINT WM_VAULT_DONE = WM_APP + 60;   // lParam: new std::string* message ("" = fine)
const UINT WM_VAULT_PROGRESS = WM_APP + 61;

bool isPicture(const std::string& n) {
    std::string e = lower(extOf(n));
    return e == "jpg" || e == "jpeg" || e == "png" || e == "gif" || e == "bmp" || e == "webp" || e == "tif" || e == "tiff";
}

// ---------------------------------------------------------------- picture viewer (decrypted in memory only)
class PictureWnd : public Dialog {
public:
    std::string data, name;
    std::unique_ptr<Gdiplus::Bitmap> bmp;
    IStream* st = nullptr;
    void load() {
        st = SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), static_cast<UINT>(data.size()));
        if (st) { bmp.reset(new Gdiplus::Bitmap(st)); if (bmp->GetLastStatus() != Gdiplus::Ok) bmp.reset(); }
    }
    ~PictureWnd() override { bmp.reset(); if (st) st->Release(); std::fill(data.begin(), data.end(), 0); }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, isDark() ? rgb(0x101114) : rgb(0x202226));
        if (!bmp) { text(dc, "This picture can't be shown here.", r, rgb(0xDDDDDD), fontUI(), DT_CENTER | DT_VCENTER | DT_SINGLELINE); return; }
        double iw = bmp->GetWidth(), ih = bmp->GetHeight();
        double k = std::min(1.0, std::min((r.right - S(20)) / iw, (r.bottom - S(20)) / ih));
        int w = static_cast<int>(iw * k), hh = static_cast<int>(ih * k);
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(bmp.get(), Gdiplus::Rect((r.right - w) / 2, (r.bottom - hh) / 2, w, hh));
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_SIZE) { redraw(); return 0; }
        return Dialog::proc(m, w, l);
    }
};

class VaultWnd : public Dialog {
public:
    enum Mode { Setup, Locked, Open } mode = Locked;
    // setup / locked
    Label head, sub, err;
    Field pw, pw2;
    Button go;
    // open
    RowList list;
    Button add, openB, saveCopy, moveOut, del, settingsB, lockB;
    Label status;
    std::vector<vault::Item> items;
    std::atomic<bool> busy{false};
    ULONGLONG lastUse = 0;

    void build() {
        head.create(h, "");
        head.bold = true;
        sub.create(h, "");
        sub.wrap = true;
        sub.setColor(P.muted);
        err.create(h, "");
        err.setColor(P.bad);
        pw.create(h, "");
        pw2.create(h, "");
        SendMessageW(pw.edit(), EM_SETPASSWORDCHAR, L'●', 0);
        SendMessageW(pw2.edit(), EM_SETPASSWORDCHAR, L'●', 0);
        go.create(h, "Unlock", BtnStyle::Primary, "check");
        list.create(h);
        list.headers = {"Name", "Size", "Added"};
        list.nameCols = {0};
        DragAcceptFiles(list.hwnd(), TRUE);
        DragAcceptFiles(h, TRUE);
        add.create(h, "Add files…", BtnStyle::Primary, "plus");
        openB.create(h, "Open", BtnStyle::Normal, "open");
        saveCopy.create(h, "Save a copy…", BtnStyle::Normal, "copy");
        moveOut.create(h, "Move out…", BtnStyle::Normal, "folder");
        del.create(h, "Delete", BtnStyle::Normal, "delete");
        settingsB.create(h, "Settings", BtnStyle::Normal, "options");
        lockB.create(h, "Lock", BtnStyle::Normal, "M7 11V8a5 5 0 0 1 10 0v3M5 11h14v10H5z");
        status.create(h, "");
        status.small = true;
        status.setColor(P.muted);

        go.onClick = [this] { submit(); };
        pw.onEnter = pw2.onEnter = [this] { submit(); };
        onEnter = [this] { if (mode == Open) openSel(); else submit(); };
        add.onClick = [this] { addFiles(); };
        openB.onClick = [this] { openSel(); };
        saveCopy.onClick = [this] { extractSel(false); };
        moveOut.onClick = [this] { extractSel(true); };
        del.onClick = [this] { deleteSel(); };
        lockB.onClick = [this] { vault::lock(); setMode(Locked); };
        settingsB.onClick = [this] { settingsMenu(); };
        list.onOpen = [this](int) { openSel(); };
        list.onDrop = [this](HDROP d) { dropFiles(d); };
        list.onContext = [this](POINT pt) {
            if (list.selected < 0) return;
            UINT c = popupMenu(h, {{"Open", "Enter", 1}, {"Save a copy…", "", 2}, {"Move out of the vault…", "", 3}, {"Rename…", "", 4}, MenuItem::sep(), {"Delete", "Del", 5}}, pt);
            if (c == 1) openSel(); else if (c == 2) extractSel(false); else if (c == 3) extractSel(true); else if (c == 4) renameSel(); else if (c == 5) deleteSel();
        };
        setMode(!vault::isSetUp() ? Setup : vault::isUnlocked() ? Open : Locked);
        SetTimer(h, 1, 1000, nullptr);
        lastUse = GetTickCount64();
    }

    void setMode(Mode m) {
        mode = m;
        bool open = m == Open;
        for (Wnd* w : {static_cast<Wnd*>(&head), static_cast<Wnd*>(&sub), static_cast<Wnd*>(&err), static_cast<Wnd*>(&pw), static_cast<Wnd*>(&go)}) w->show(!open);
        pw2.show(m == Setup);
        for (Wnd* w : {static_cast<Wnd*>(&list), static_cast<Wnd*>(&add), static_cast<Wnd*>(&openB), static_cast<Wnd*>(&saveCopy), static_cast<Wnd*>(&moveOut), static_cast<Wnd*>(&del),
                       static_cast<Wnd*>(&settingsB), static_cast<Wnd*>(&lockB), static_cast<Wnd*>(&status)}) w->show(open);
        err.setText("");
        pw.set(""); pw2.set("");
        if (m == Setup) {
            head.setText("Set up your private vault");
            sub.setText("Files in the vault are encrypted (AES-256) and hidden. Only DDM can open them, and only with your password — no other program or Windows user can see them. There's no way to recover the files if you forget the password.");
            go.setText("Create vault");
            pw.hint("Password"); pw2.hint("The same password again");
        } else if (m == Locked) {
            head.setText("The private vault is locked");
            sub.setText("Enter your password.");
            pw.hint("Password");
            go.setText("Unlock");
        } else {
            refresh();
        }
        layout();
        redraw();
        if (!open) pw.focus(); else SetFocus(list.hwnd());
    }

    void submit() {
        std::string e;
        if (mode == Setup) {
            if (pw.get().size() < 4) { err.setText("Use at least 4 characters."); return; }
            if (pw.get() != pw2.get()) { err.setText("The two passwords are different."); return; }
            if (!vault::create(pw.get(), e) || !vault::unlock(pw.get(), e)) { err.setText(e); return; }
            setMode(Open);
            return;
        }
        if (pw.get().empty()) { pw.focus(); return; }
        go.setText("Opening…");
        EnableWindow(go.hwnd(), FALSE);
        UpdateWindow(h);
        bool ok = vault::unlock(pw.get(), e);
        EnableWindow(go.hwnd(), TRUE);
        go.setText("Unlock");
        if (!ok) { err.setText(e); pw.set(""); pw.focus(); return; }
        setMode(Open);
    }

    void refresh() {
        items = vault::list();
        std::vector<RowList::Item> v;
        int64_t total = 0;
        for (auto& it : items) {
            RowList::Item r;
            r.key = it.id;
            r.icon = "type:" + typeOf(categoryFor(it.name), it.name, "");
            r.cols = {it.name, formatBytes(it.size), formatDate(it.added)};
            v.push_back(r);
            total += it.size;
        }
        list.setItems(v);
        if (!busy) status.setText(items.empty() ? "The vault is empty. Click Add files… or drop files here." : std::to_string(items.size()) + (items.size() == 1 ? " file · " : " files · ") + formatBytes(total) + " · encrypted");
    }

    const vault::Item* sel() {
        if (list.selected < 0 || list.selected >= static_cast<int>(list.items.size())) return nullptr;
        for (auto& it : items) if (it.id == list.items[static_cast<size_t>(list.selected)].key) return &it;
        return nullptr;
    }

    // runs a vault job in the background with a status line, then refreshes
    void job(const std::string& what, std::function<std::string(std::function<void(int64_t, int64_t)>)> fn) {
        if (busy) return;
        busy = true;
        status.setText(what + "…");
        HWND hw = h;
        std::thread([hw, fn] {
            ULONGLONG last = 0;
            std::string r = fn([hw, &last](int64_t d, int64_t t) {
                if (GetTickCount64() - last < 300 || t <= 0) return;
                last = GetTickCount64();
                PostMessageW(hw, WM_VAULT_PROGRESS, static_cast<WPARAM>(d * 100 / t), 0);
            });
            auto* s = new std::string(r);
            if (!PostMessageW(hw, WM_VAULT_DONE, 0, reinterpret_cast<LPARAM>(s))) delete s;
        }).detach();
    }

    void addPaths(const std::vector<std::string>& paths) {
        if (paths.empty()) return;
        int r = askBox(h, "Add to the vault", paths.size() == 1 ? "Add " + baseName(paths[0]) + " to the private vault?" : "Add " + std::to_string(paths.size()) + " files to the private vault?",
                       {"Move into the vault", "Copy into the vault", "Cancel"}, 0);
        if (r < 0 || r == 2) return;
        bool move = r == 0;
        job("Encrypting", [paths, move](std::function<void(int64_t, int64_t)> p) {
            std::string errs;
            for (auto& f : paths) { std::string e; if (!vault::add(f, move, e, "", p)) errs += e + "\n"; }
            return errs;
        });
    }

    void addFiles() {
        std::wstring buf(64 * 1024, L'\0');
        OPENFILENAMEW of{};
        of.lStructSize = sizeof of;
        of.hwndOwner = h;
        of.lpstrFilter = L"All files\0*.*\0\0";
        of.lpstrFile = &buf[0];
        of.nMaxFile = static_cast<DWORD>(buf.size());
        of.lpstrTitle = L"Add files to the private vault";
        of.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&of)) return;
        std::vector<std::string> paths;
        std::wstring dir = buf.c_str();
        const wchar_t* p = buf.c_str() + dir.size() + 1;
        if (!*p) paths.push_back(U(dir));
        else for (; *p; p += wcslen(p) + 1) paths.push_back(joinPath(U(dir), U(p)));
        addPaths(paths);
    }

    void dropFiles(HDROP d) {
        std::vector<std::string> paths;
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            wchar_t f[MAX_PATH * 2];
            if (DragQueryFileW(d, i, f, MAX_PATH * 2) && !(GetFileAttributesW(f) & FILE_ATTRIBUTE_DIRECTORY)) paths.push_back(U(f));
        }
        DragFinish(d);
        if (mode == Open) addPaths(paths);
    }

    void openSel() {
        auto it = sel();
        if (!it || busy) return;
        std::string id = it->id, name = it->name;
        if (isPicture(name) && it->size < 200LL * 1024 * 1024) {   // pictures: shown inside DDM, never written out
            std::string data, e;
            if (!vault::readAll(id, data, e)) { askBox(h, "Open", e, {"OK"}, 0); return; }
            auto* pv = new PictureWnd();
            pv->data.swap(data);
            pv->name = name;
            pv->load();
            if (!pv->createDialog(nullptr, name, S(900), S(640), true, true)) { delete pv; return; }
            ShowWindow(pv->hwnd(), SW_SHOW);
            struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
                if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<PictureWnd*>(ref); return res; }
                return DefSubclassProc(hw, m, w, l); } };
            SetWindowSubclass(pv->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(pv));
            return;
        }
        HWND hw = h;
        job("Opening " + name, [id, hw](std::function<void(int64_t, int64_t)>) {
            std::string e;
            std::string p = vault::openCopy(id, e);
            if (p.empty()) return e;
            ShellExecuteW(hw, L"open", W(p).c_str(), nullptr, W(dirName(p)).c_str(), SW_SHOWNORMAL);
            return std::string();
        });
    }

    void extractSel(bool move) {
        auto it = sel();
        if (!it || busy) return;
        std::string dest = pickFile(h, "All files|*.*", true, it->name);
        if (dest.empty()) return;
        std::string id = it->id;
        job(move ? "Moving out" : "Decrypting", [id, dest, move](std::function<void(int64_t, int64_t)> p) {
            std::string e;
            if (!vault::extract(id, dest, e, p)) return e;
            if (move && !vault::remove(id, e)) return e;
            return std::string();
        });
    }

    void deleteSel() {
        auto it = sel();
        if (!it || busy) return;
        if (askBox(h, "Delete", "Delete " + it->name + " from the vault for good? It can't be brought back.", {"Delete", "Cancel"}, 1) != 0) return;
        std::string e, id = it->id;
        if (!vault::remove(id, e)) askBox(h, "Delete", e, {"OK"}, 0);
        refresh();
    }

    void renameSel() {
        auto it = sel();
        if (!it || busy) return;
        std::string name = it->name, id = it->id;
        if (!inputBox(h, "Rename", "New name", name) || name == it->name) return;
        job("Renaming", [id, name](std::function<void(int64_t, int64_t)>) { std::string e; return vault::rename(id, name, e) ? std::string() : e; });
    }

    void settingsMenu() {
        std::vector<MenuItem> v = {{"Change password…", "", 1}};
        if (!vault::isDecoy()) {
            bool has = vault::hasDecoyPassword();
            v.push_back(MenuItem::sep());
            v.push_back({has ? "Change the decoy password…" : "Set a decoy password…", "", 2});
            if (has) v.push_back({"Remove the decoy vault", "", 3});
        }
        RECT r;
        GetWindowRect(settingsB.hwnd(), &r);
        UINT c = popupMenu(h, v, POINT{r.left, r.bottom});
        std::string e;
        if (c == 1) {
            std::string old, nw, nw2;
            if (!inputBox(h, "Change password", "Current password", old)) return;
            if (!inputBox(h, "Change password", "New password (at least 4 characters)", nw)) return;
            if (!inputBox(h, "Change password", "The new password again", nw2)) return;
            if (nw != nw2) { askBox(h, "Change password", "The two new passwords are different.", {"OK"}, 0); return; }
            askBox(h, "Change password", vault::changePassword(old, nw, e) ? "The password is changed." : e, {"OK"}, 0);
        } else if (c == 2) {
            askBox(h, "Decoy vault", "A decoy password opens a second, separate vault that looks exactly like this one. If someone makes you open the vault, give them the decoy password: they see only what you put in the decoy vault, and nothing shows that this vault exists.", {"OK"}, 0);
            std::string d, d2;
            if (!inputBox(h, "Decoy password", "Decoy password (different from your real one)", d)) return;
            if (!inputBox(h, "Decoy password", "The decoy password again", d2)) return;
            if (d != d2) { askBox(h, "Decoy password", "The two passwords are different.", {"OK"}, 0); return; }
            askBox(h, "Decoy vault", vault::setDecoy(d, e) ? "The decoy password is set. Lock the vault and open it with the decoy password to put harmless files in it." : e, {"OK"}, 0);
        } else if (c == 3) {
            if (askBox(h, "Decoy vault", "Remove the decoy vault and everything in it?", {"Remove", "Cancel"}, 1) == 0)
                askBox(h, "Decoy vault", vault::removeDecoy(e) ? "The decoy vault is removed." : e, {"OK"}, 0);
        }
    }

    void layout() override {
        RECT r = client();
        int pad = S(22);
        if (mode != Open) {
            int w = std::min<int>(S(420), r.right - 2 * pad), x = (r.right - w) / 2, y = S(90);
            head.move(x, y, w, S(26)); y += S(32);
            sub.move(x, y, w, mode == Setup ? S(76) : S(22)); y += mode == Setup ? S(84) : S(30);
            pw.move(x, y, w, S(34)); y += S(42);
            if (mode == Setup) { pw2.move(x, y, w, S(34)); y += S(42); }
            go.move(x, y, w, S(38)); y += S(46);
            err.move(x, y, w, S(40));
            return;
        }
        int y = S(16), x = pad;
        for (Button* b : {&add, &openB, &saveCopy, &moveOut, &del}) { int w = b->preferredWidth(); b->move(x, y, w, S(34)); x += w + S(6); }
        int rx = r.right - pad;
        for (Button* b : {&lockB, &settingsB}) { int w = b->preferredWidth(); rx -= w; b->move(rx, y, w, S(34)); rx -= S(6); }
        list.widths = {r.right - 2 * pad - S(260), S(100)};
        list.move(pad, S(62), r.right - 2 * pad, r.bottom - S(62) - S(38));
        status.move(pad, r.bottom - S(30), r.right - 2 * pad, S(20));
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        if (mode != Open) {
            RECT ic{(r.right - S(44)) / 2, S(30), (r.right + S(44)) / 2, S(74)};
            tileIcon(dc, ic, "M7 11V8a5 5 0 0 1 10 0v3M5 11h14v10H5z", rgb(0x7D8CFF), rgb(0x4B5BE0));
        }
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
            case WM_VAULT_PROGRESS: status.setText("Working… " + std::to_string(static_cast<int>(w)) + "%"); return 0;
            case WM_VAULT_DONE: {
                std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l));
                busy = false;
                refresh();
                if (!s->empty()) askBox(h, "Private vault", trim(*s), {"OK"}, 0);
                return 0;
            }
            case WM_DROPFILES: dropFiles(reinterpret_cast<HDROP>(w)); return 0;
            case WM_TIMER:
                if (mode == Open && !busy && GetTickCount64() - lastUse > 5ULL * 60 * 1000) { vault::lock(); setMode(Locked); }
                return 0;
            case WM_SETCURSOR: case WM_KEYDOWN: case WM_MOUSEMOVE: lastUse = GetTickCount64(); break;
            case WM_PARENTNOTIFY: lastUse = GetTickCount64(); break;
            case WM_SIZE: layout(); redraw(); return 0;
            case WM_DESTROY: vault::lock(); break;   // closing the window locks the vault
        }
        return Dialog::proc(m, w, l);
    }
};

} // namespace

void showVault(HWND owner) {
    static bool open = false;
    if (open) return;
    open = true;
    VaultWnd d;
    if (d.createDialog(owner, "Private vault", S(860), S(560), true, false)) {
        d.build();
        d.runModal();
    }
    open = false;
}

void moveToVault(HWND owner, const std::vector<std::string>& ids) {
    if (!vault::isSetUp()) {
        if (askBox(owner, "Private vault", "Set up the private vault first: choose a password for it.", {"Set up the vault", "Cancel"}, 0) == 0) showVault(owner);
        if (!vault::isSetUp()) return;
    }
    std::vector<Task> ts;
    for (auto& id : ids) { Task t; if (Engine::get().find(id, t) && t.state == State::Complete && fileExists(t.path())) ts.push_back(t); }
    if (ts.empty()) return;
    if (askBox(owner, "Move to private vault", ts.size() == 1 ? ts[0].fileName + " is encrypted into the private vault and removed from this folder and from the list." : std::to_string(ts.size()) + " files are encrypted into the private vault and removed from their folders and from the list.",
               {"Move to vault", "Cancel"}, 0) != 0) return;
    std::thread([ts] {
        for (auto& t : ts) {
            std::string e;
            if (vault::add(t.path(), true, e, t.url)) Engine::get().remove(t.id, false);
            else logLine("vault: " + e);
        }
    }).detach();
}

} // namespace ui
} // namespace ddm
