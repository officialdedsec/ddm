// When every download is finished: a silent notification, or sleep / hibernate / shut down after a 60-second countdown.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "mainwnd.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../common/util.hpp"
#include <powrprof.h>
#include <ras.h>

namespace ddm {
namespace ui {

namespace {

bool enableShutdownPrivilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp{};
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(tok);
    return ok && GetLastError() == ERROR_SUCCESS;
}

bool g_force = false;

void doIt(const std::string& a) {
    enableShutdownPrivilege();
    if (a == "sleep") SetSuspendState(FALSE, g_force, FALSE);
    else if (a == "hibernate") SetSuspendState(TRUE, g_force, FALSE);
    else if (a == "shutdown" || a == "restart") {
        bool reboot = a == "restart";
        App::get().quit();
        if (!InitiateSystemShutdownExW(nullptr, nullptr, 0, g_force, reboot, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED))
            ExitWindowsEx((reboot ? EWX_REBOOT : EWX_SHUTDOWN | EWX_POWEROFF) | (g_force ? EWX_FORCE : 0), SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
    }
}

class CountdownWnd : public Dialog {
public:
    std::string action, title = "All downloads are finished";
    int left = 60;
    Button now, cancel;
    std::string verb() const { return action == "sleep" ? "go to sleep" : action == "hibernate" ? "hibernate" : action == "restart" ? "restart" : "shut down"; }
    void build() {
        now.create(h, action == "sleep" ? "Sleep now" : action == "hibernate" ? "Hibernate now" : action == "restart" ? "Restart now" : "Shut down now", BtnStyle::Normal);
        cancel.create(h, "Cancel", BtnStyle::Primary);
        now.onClick = [this] { std::string a = action; close(0); doIt(a); };
        cancel.onClick = [this] { close(-1); };
        onEnter = cancel.onClick;
        layout();
        SetTimer(h, 1, 1000, nullptr);
        SetFocus(cancel.hwnd());
    }
    void layout() override {
        RECT r = client();
        cancel.move(r.right - S(20) - S(110), r.bottom - S(52), S(110), S(34));
        int w = std::max(S(130), now.preferredWidth());
        now.move(r.right - S(20) - S(110) - S(8) - w, r.bottom - S(52), w, S(34));
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT ic{S(22), S(22), S(22) + S(36), S(22) + S(36)};
        icon(dc, "clock", ic, P.accent, 1.6f);
        RECT t1{S(72), S(20), r.right - S(20), S(44)};
        text(dc, title, t1, P.text, fontBig());
        RECT t2{S(72), S(48), r.right - S(20), S(70)};
        text(dc, "Your computer will " + verb() + " in " + std::to_string(left) + (left == 1 ? " second." : " seconds."), t2, P.muted, fontUI());
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_TIMER) {
            if (--left <= 0) { KillTimer(h, 1); std::string a = action; close(0); doIt(a); return 0; }
            redraw();
            return 0;
        }
        return Dialog::proc(m, w, l);
    }
};

} // namespace

void powerAction(HWND owner, const std::string& action, bool force, const std::string& heading) {
    g_force = force;
    if (action == "notify" || action == "sound") {
        return;   // no "All downloads are finished" pop-up (the sound, if chosen, still plays)
    }
    if (action != "sleep" && action != "hibernate" && action != "shutdown" && action != "restart") return;
    static bool open = false;
    if (open) return;
    open = true;
    CountdownWnd d;
    d.action = action;
    if (!heading.empty()) d.title = heading;
    if (d.createDialog(nullptr, "DDM", S(460), S(150))) {
        d.build();
        SetWindowPos(d.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        d.runModal();
    }
    open = false;
    (void)owner;
}

// "Hang up modem when done": closes every dial-up / modem (RAS) connection.
void hangUpModem() {
    HMODULE ras = systemDll(L"rasapi32.dll");
    if (!ras) return;
    typedef DWORD(WINAPI * EnumFn)(LPRASCONNW, LPDWORD, LPDWORD);
    typedef DWORD(WINAPI * HangFn)(HRASCONN);
    auto en = reinterpret_cast<EnumFn>(GetProcAddress(ras, "RasEnumConnectionsW"));
    auto hang = reinterpret_cast<HangFn>(GetProcAddress(ras, "RasHangUpW"));
    if (en && hang) {
        RASCONNW conns[16];
        conns[0].dwSize = sizeof(RASCONNW);
        DWORD size = sizeof conns, n = 0;
        if (en(conns, &size, &n) == 0) for (DWORD i = 0; i < n; i++) hang(conns[i].hrasconn);
    }
    FreeLibrary(ras);
}

} // namespace ui
} // namespace ddm
