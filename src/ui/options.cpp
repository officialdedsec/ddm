// Options, About and Help windows.
#include "dialogs.hpp"
#include "app.hpp"
#include "../core/integration.hpp"
#include "../common/version.hpp"
#include <shellapi.h>
#include "gfx.hpp"
#include "theme.hpp"
#include "../core/engine.hpp"
#include "../core/http.hpp"
#include "../core/settings.hpp"
#include "../common/util.hpp"
#include <shlobj.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <memory>

namespace ddm {
namespace ui {


static int CALLBACK folderInit(HWND hw, UINT m, LPARAM, LPARAM data) { if (m == BFFM_INITIALIZED) SendMessageW(hw, BFFM_SETSELECTIONW, TRUE, data); return 0; }

std::string pickFolder(HWND owner, const std::string& start) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    std::wstring title = W(i18n::tr("Choose a folder"));
    bi.lpszTitle = title.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    std::wstring s = W(start);
    bi.lParam = reinterpret_cast<LPARAM>(s.c_str());
    bi.lpfn = folderInit;
    PIDLIST_ABSOLUTE p = SHBrowseForFolderW(&bi);
    if (!p) return "";
    wchar_t path[MAX_PATH];
    std::string r = SHGetPathFromIDListW(p, path) ? U(path) : "";
    CoTaskMemFree(p);
    return r;
}

namespace {
class AboutWnd : public Dialog {
public:
    Button closeBtn, source, licenceB, updates, site, privacy, contact;
    void build() {
        closeBtn.create(h, "Close", BtnStyle::Primary);
        source.create(h, "Source code on GitHub", BtnStyle::Normal, "open");
        licenceB.create(h, "The licence (GPL-3.0)", BtnStyle::Normal, "document");
        updates.create(h, "Check for updates", BtnStyle::Subtle, "refresh");
        site.create(h, "dedseec.com", BtnStyle::Subtle, "open");
        privacy.create(h, "Privacy policy", BtnStyle::Subtle, "document");
        contact.create(h, "Contact us", BtnStyle::Subtle, "info");
        closeBtn.onClick = [this] { close(0); };
        source.onClick = [] { ShellExecuteW(nullptr, L"open", L"https://github.com/officialdedsec/ddm", nullptr, nullptr, SW_SHOWNORMAL); };
        licenceB.onClick = [] { ShellExecuteW(nullptr, L"open", L"https://github.com/officialdedsec/ddm/blob/main/LICENSE", nullptr, nullptr, SW_SHOWNORMAL); };
        updates.onClick = [this] { App::get().checkUpdatesNow(h); };
        site.onClick = [] { ShellExecuteW(nullptr, L"open", L"https://dedseec.com", nullptr, nullptr, SW_SHOWNORMAL); };
        privacy.onClick = [] { ShellExecuteW(nullptr, L"open", L"https://dedseec.com/privacy", nullptr, nullptr, SW_SHOWNORMAL); };
        contact.onClick = [] { ShellExecuteW(nullptr, L"open", L"https://dedseec.com/contact", nullptr, nullptr, SW_SHOWNORMAL); };
        layout();
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT lg{S(24), S(24), S(24) + S(64), S(24) + S(64)};
        logo(dc, lg);
        RECT t1{S(104), S(24), r.right - S(20), S(52)};
        text(dc, "DedSec Download Manager", t1, P.text, fontBig());
        RECT t2{S(104), S(54), r.right - S(20), S(76)};
        text(dc, "Version " DDM_VERSION " · for Windows 7, 8, 10 and 11", t2, P.muted, fontUI());
        RECT t3{S(104), S(76), r.right - S(20), S(98)};
        text(dc, "by DedSec Cyber Security Research Institute · admin@dedseec.com", t3, P.muted, fontUI());
        RECT box{S(20), S(112), r.right - S(20), S(168)};
        fillRound(dc, box, Sf(8), P.tile);
        RECT l1{box.left + S(14), box.top + S(8), box.right - S(14), box.top + S(28)};
        text(dc, "Free and open source", l1, P.text, fontBold());
        RECT l2{box.left + S(14), box.top + S(30), box.right - S(14), box.top + S(50)};
        text(dc, "Free for everyone, with no time limit. Licensed under the GNU GPL, version 3.", l2, P.muted, fontSmall(), DT_END_ELLIPSIS);
    }
    void layout() override {
        RECT r = client();
        int y = S(178), x = S(20);
        for (Button* b : {&source, &licenceB}) { int w = b->preferredWidth(); b->move(x, y, w, S(32)); x += w + S(8); }
        y += S(40); x = S(14);
        for (Button* b : {&updates, &site, &privacy, &contact}) { int w = b->preferredWidth(); b->move(x, y, w, S(30)); x += w + S(2); }
        closeBtn.move(r.right - S(116), r.bottom - S(52), S(96), S(34));
    }
};
} // namespace

void showAboutWindow(HWND owner) {
    AboutWnd d;
    if (!d.createDialog(owner, "About DDM", S(600), S(320))) return;
    d.build();
    d.runModal();
}

} // namespace ui
} // namespace ddm
