// New windows of 4.0.9: Play while downloading (DDM's own player, Windows' Media Foundation), Download playlist or
// channel, Subscriptions, and Phone remote (with a QR code made by DDM).
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "rowlist.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/playlists.hpp"
#include "../core/preview.hpp"
#include "../core/qr.hpp"
#include "../core/remote.hpp"
#include "../core/settings.hpp"
#include "../common/util.hpp"
#include "mainwnd.hpp"
#include <commctrl.h>
#include <mfplay.h>
#include <mferror.h>
#include <propvarutil.h>
#include <algorithm>
#include <memory>
#include <set>
#include <thread>

namespace ddm {
namespace ui {

namespace {

template <class T> T* showModelessWnd(T* d, HWND owner, const std::string& title, int w, int ht, bool sizable = true) {
    if (!d->createDialog(owner, title, w, ht, sizable, true)) { delete d; return nullptr; }
    d->build();
    ShowWindow(d->hwnd(), SW_SHOW);
    struct Deleter {
        static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
            if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, wp, lp); delete reinterpret_cast<T*>(ref); return res; }
            return DefSubclassProc(hw, m, wp, lp);
        }
    };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
    return d;
}

std::string clipText() {
    std::string out;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) return out;
    HANDLE d = GetClipboardData(CF_UNICODETEXT);
    if (d) { auto* p = static_cast<const wchar_t*>(GlobalLock(d)); if (p) { out = U(p); GlobalUnlock(d); } }
    CloseClipboard();
    return out.size() > 4096 ? "" : out;
}
void copyText(HWND owner, const std::string& s) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    std::wstring w = W(s);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * 2);
    if (g) { memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * 2); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
    CloseClipboard();
}
void refreshMainWindow() { if (App::get().main()) App::get().main()->refresh(); }

std::string clock(double s) {
    if (s < 0 || s != s) s = 0;
    int t = static_cast<int>(s + 0.5);
    char b[32];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60); else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

// =============================================================================================== player
typedef HRESULT(WINAPI* CreatePlayerFn)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS, IMFPMediaPlayerCallback*, HWND, IMFPMediaPlayer**);
typedef HRESULT(WINAPI* MFStartupFn)(ULONG, DWORD);

class PlayerWnd;

class PlayerEvents final : public IMFPMediaPlayerCallback {
public:
    explicit PlayerEvents(PlayerWnd* w) : wnd(w) {}
    PlayerWnd* wnd;
    LONG refs = 1;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) { *ppv = static_cast<IMFPMediaPlayerCallback*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs); if (r == 0) delete this; return static_cast<ULONG>(r); }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* e) override;
};

// the picture area (MFPlay draws the video into it; music shows a note)
LRESULT CALLBACK videoProc(HWND hw, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT r;
        GetClientRect(hw, &r);
        fill(dc, r, RGB(12, 14, 18));
        bool music = GetWindowLongPtrW(hw, GWLP_USERDATA) == 1;
        if (music) { int s = std::min(r.right, r.bottom) / 4; RECT ic{(r.right - s) / 2, (r.bottom - s) / 2, (r.right + s) / 2, (r.bottom + s) / 2}; icon(dc, "music", ic, RGB(120, 140, 170), 2.0f); }
        EndPaint(hw, &ps);
        return 0;
    }
    return DefWindowProcW(hw, m, w, l);
}

class PlayerWnd : public Dialog {
public:
    std::string id, url;
    Task t;
    HWND video = nullptr;
    IMFPMediaPlayer* player = nullptr;
    PlayerEvents* events = nullptr;
    Button playBtn, browserBtn;
    std::string problem;
    double pos = 0, dur = 0;
    bool playing = false, hasVideo = true, dragging = false;
    RECT bar{};

    void build() {
        Engine::get().find(id, t);
        url = preview::urlFor(id);
        WNDCLASSW wc{};
        wc.lpfnWndProc = videoProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DDMVideo";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        video = CreateWindowExW(0, L"DDMVideo", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, h, nullptr, GetModuleHandleW(nullptr), nullptr);
        playBtn.create(h, "Pause", BtnStyle::Primary, "pause");
        browserBtn.create(h, "Play in your browser", BtnStyle::Normal, "globe");
        playBtn.onClick = [this] { toggle(); };
        browserBtn.onClick = [this] { if (!url.empty()) App::get().openUrl(url); };
        layout();
        SetTimer(h, 1, 250, nullptr);
        start();
    }
    void start() {
        if (url.empty()) { problem = "DDM couldn't start its player on this PC."; showPlayBtn(false); EnableWindow(playBtn.hwnd(), FALSE); return; }
        static HMODULE mf = systemDll(L"mfplat.dll");
        static HMODULE mp = systemDll(L"mfplay.dll");
        auto startup = mf ? reinterpret_cast<MFStartupFn>(reinterpret_cast<void*>(GetProcAddress(mf, "MFStartup"))) : nullptr;
        auto create = mp ? reinterpret_cast<CreatePlayerFn>(reinterpret_cast<void*>(GetProcAddress(mp, "MFPCreateMediaPlayer"))) : nullptr;
        if (!startup || !create) { problem = "Windows' media player parts aren't on this PC (Windows N needs Microsoft's Media Feature Pack). Use Play in your browser."; showPlayBtn(false); EnableWindow(playBtn.hwnd(), FALSE); return; }
        static bool started = SUCCEEDED(startup(0x20070, 0));
        (void)started;
        events = new PlayerEvents(this);
        if (FAILED(create(W(url).c_str(), TRUE, MFP_OPTION_NONE, events, video, &player)) || !player) {
            player = nullptr;
            problem = "Windows can't play this file here. Use Play in your browser.";
        }
        if (!player) { showPlayBtn(false); EnableWindow(playBtn.hwnd(), FALSE); }
    }
    void showPlayBtn(bool pausing) { playBtn.iconName = pausing ? "pause" : "resume"; playBtn.setText(pausing ? "Pause" : "Play"); }
    void stopPlayer() {
        if (events) events->wnd = nullptr;
        if (player) { player->Shutdown(); player->Release(); player = nullptr; }
        if (events) { events->Release(); events = nullptr; }
    }
    ~PlayerWnd() override { stopPlayer(); }
    void toggle() {
        if (!player) return;
        if (playing) player->Pause(); else player->Play();
    }
    void seekTo(double frac) {
        if (!player || dur <= 0) return;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = static_cast<LONGLONG>(std::max(0.0, std::min(1.0, frac)) * dur * 1e7);
        player->SetPosition(GUID_NULL, &pv);   // (MFP_POSITIONTYPE_100NS is GUID_NULL)
        pos = frac * dur;
        redrawBar();
    }
    void onEvent(MFP_EVENT_HEADER* e) {
        if (FAILED(e->hrEvent)) {
            problem = e->hrEvent == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || e->hrEvent == MF_E_TOPO_CODEC_NOT_FOUND || e->hrEvent == MF_E_INVALIDMEDIATYPE
                          ? "Windows can't play this format here. Use Play in your browser."
                          : "The player stopped (" + std::to_string(static_cast<unsigned long>(e->hrEvent)) + "). Use Play in your browser.";
            playing = false;
            showPlayBtn(false);
            EnableWindow(playBtn.hwnd(), FALSE);
            InvalidateRect(h, nullptr, FALSE);
            return;
        }
        switch (e->eEventType) {
            case MFP_EVENT_TYPE_MEDIAITEM_SET: {
                IMFPMediaItem* item = nullptr;
                if (player && SUCCEEDED(player->GetMediaItem(&item)) && item) {
                    BOOL hv = FALSE, sel = FALSE;
                    item->HasVideo(&hv, &sel);
                    hasVideo = hv && sel;
                    SetWindowLongPtrW(video, GWLP_USERDATA, hasVideo ? 0 : 1);
                    InvalidateRect(video, nullptr, TRUE);
                    item->Release();
                }
                break;
            }
            case MFP_EVENT_TYPE_PLAY: playing = true; showPlayBtn(true); break;
            case MFP_EVENT_TYPE_PAUSE: case MFP_EVENT_TYPE_STOP: case MFP_EVENT_TYPE_PLAYBACK_ENDED: playing = false; showPlayBtn(false); break;
            default: break;
        }
    }
    double readyFraction() {
        Task n;
        if (!Engine::get().find(id, n) || n.total <= 0) return 0;
        if (n.state == State::Complete) return 1;
        int64_t ready = 0;
        for (auto& s : n.segments) if (s.start == 0) ready = std::max(ready, s.pos);
        return static_cast<double>(ready) / static_cast<double>(n.total);
    }
    void redrawBar() { RECT r = bar; InflateRect(&r, 0, S(10)); r.right = client().right; r.left = 0; InvalidateRect(h, &r, FALSE); }
    void layout() override {
        RECT r = client();
        int foot = S(76);
        if (video) MoveWindow(video, 0, 0, r.right, r.bottom - foot, TRUE);
        if (player) player->UpdateVideo();
        int y = r.bottom - foot + S(30);
        playBtn.move(S(16), y, S(110), S(34));
        int bw = std::max(S(170), browserBtn.preferredWidth());
        browserBtn.move(r.right - S(16) - bw, y, bw, S(34));
        bar = RECT{S(16), r.bottom - foot + S(12), r.right - S(16), r.bottom - foot + S(18)};
    }
    void paintBackground(HDC dc, RECT r) override {
        RECT foot{0, r.bottom - S(76), r.right, r.bottom};
        fill(dc, foot, P.win);
        // the bar: downloaded so far (lighter), played (accent)
        fillRound(dc, bar, Sf(3), P.track);
        double ready = readyFraction();
        {   // every part already here (a download fetches several parts at once)
            Task n;
            if (Engine::get().find(id, n) && n.total > 0) {
                double W0 = static_cast<double>(bar.right - bar.left);
                auto part = [&](int64_t from, int64_t to) {
                    RECT rr = bar;
                    rr.left = bar.left + static_cast<LONG>(W0 * static_cast<double>(from) / static_cast<double>(n.total));
                    rr.right = bar.left + static_cast<LONG>(W0 * static_cast<double>(to) / static_cast<double>(n.total) + 0.999);
                    if (rr.right > rr.left) fill(dc, rr, blend(P.track, P.accent, 0.35f));
                };
                if (n.state == State::Complete) part(0, n.total);
                else for (auto& s : n.segments) if (s.pos > s.start) part(s.start, std::min(s.pos, s.end >= 0 ? s.end + 1 : s.pos));
            }
        }
        double f = dur > 0 ? pos / dur : 0;
        RECT pr = bar; pr.right = bar.left + static_cast<LONG>((bar.right - bar.left) * f);
        if (pr.right > pr.left) fillRound(dc, pr, Sf(3), P.accent);
        fillCircle(dc, static_cast<float>(pr.right), (bar.top + bar.bottom) / 2.0f, Sf(6), P.accent);
        RECT tr{S(140), r.bottom - S(46), r.right - S(200), r.bottom - S(12)};
        std::string line = problem.empty() ? clock(pos) + " / " + (dur > 0 ? clock(dur) : "…") + "   ·   " + (ready >= 1 ? std::string("downloaded") : "downloading " + std::to_string(static_cast<int>(ready * 100)) + "%")
                                           : problem;
        text(dc, line, tr, problem.empty() ? P.muted : P.bad, fontUI(), DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
            case WM_TIMER:
                if (player && !dragging) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(player->GetPosition(GUID_NULL, &pv))) pos = static_cast<double>(pv.hVal.QuadPart) / 1e7;
                    PropVariantClear(&pv);
                    PropVariantInit(&pv);
                    if (SUCCEEDED(player->GetDuration(GUID_NULL, &pv))) dur = static_cast<double>(pv.uhVal.QuadPart) / 1e7;
                    PropVariantClear(&pv);
                }
                { RECT fr = client(); fr.top = fr.bottom - S(76); InvalidateRect(h, &fr, FALSE); }
                return 0;
            case WM_LBUTTONDOWN: {
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                RECT hit = bar; InflateRect(&hit, 0, S(10));
                if (PtInRect(&hit, p)) { dragging = true; SetCapture(h); seekTo(static_cast<double>(p.x - bar.left) / std::max<LONG>(1, bar.right - bar.left)); return 0; }
                break;
            }
            case WM_MOUSEMOVE:
                if (dragging) { pos = std::max(0.0, std::min(1.0, static_cast<double>(GET_X_LPARAM(l) - bar.left) / std::max<LONG>(1, bar.right - bar.left))) * dur; redrawBar(); return 0; }
                break;
            case WM_LBUTTONUP:
                if (dragging) { dragging = false; ReleaseCapture(); seekTo(static_cast<double>(GET_X_LPARAM(l) - bar.left) / std::max<LONG>(1, bar.right - bar.left)); return 0; }
                break;
            case WM_KEYDOWN:
                if (w == VK_SPACE) { toggle(); return 0; }
                if ((w == VK_LEFT || w == VK_RIGHT) && dur > 0) { seekTo((pos + (w == VK_LEFT ? -10 : 10)) / dur); return 0; }
                break;
            case WM_CLOSE: stopPlayer(); break;
        }
        return Dialog::proc(m, w, l);
    }
};

void STDMETHODCALLTYPE PlayerEvents::OnMediaPlayerEvent(MFP_EVENT_HEADER* e) { if (wnd) wnd->onEvent(e); }

// =============================================================================================== playlist
const char* kQualities[] = {"Best", "2160p", "1440p", "1080p", "720p", "480p", "360p", "MP3"};

class PlaylistWnd : public Dialog {
public:
    std::string url, cookies;
    playlists::List list;
    std::vector<bool> picked;
    bool loading = true;
    std::string status = "Reading the list…";
    RowList rows;
    DropDown quality;
    Field folder;
    Button browse, all, none, go, cancel;
    CheckBox sub;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    static const UINT kRead = WM_APP + 71, kStatus = WM_APP + 72;

    ~PlaylistWnd() override { *alive = false; }
    void build() {
        rows.create(h);
        rows.headers = {"", "Video", "Length"};
        rows.widths = {S(34), S(560), S(80)};
        rows.nameCols = {1};
        rows.onSelect = [this](int i) { if (i >= 0 && i < static_cast<int>(picked.size())) { picked[static_cast<size_t>(i)] = !picked[static_cast<size_t>(i)]; refill(); } };
        int qi = 0;
        std::string want = settings().videoQuality;   // "best", "1080", "mp3"… (Options → Videos)
        for (int i = 0; i < 8; i++) if (iequals(want, kQualities[i]) || iequals(want + "p", kQualities[i])) qi = i;
        quality.create(h, std::vector<std::string>(std::begin(kQualities), std::end(kQualities)), qi);
        folder.create(h, joinPath(folderFor("video"), ""));
        browse.create(h, "Browse…");
        all.create(h, "Select all", BtnStyle::Subtle);
        none.create(h, "Select none", BtnStyle::Subtle);
        sub.create(h, "Also download its new videos by themselves (subscribe)", false);
        go.create(h, "Download", BtnStyle::Primary, "down");
        cancel.create(h, "Cancel");
        browse.onClick = [this] { std::string d = pickFolder(h, folder.get()); if (!d.empty()) folder.set(d); };
        all.onClick = [this] { std::fill(picked.begin(), picked.end(), true); refill(); };
        none.onClick = [this] { std::fill(picked.begin(), picked.end(), false); refill(); };
        cancel.onClick = [this] { close(0); };
        go.onClick = [this] { start(); };
        layout();
        HWND hw = h;
        std::string u = url, c = cookies;
        auto flag = alive;
        std::thread([hw, u, c, flag] {
            auto* l = new playlists::List();
            playlists::read(u, c, *l, [hw, flag](const std::string& s) { if (*flag) PostMessageW(hw, kStatus, 0, reinterpret_cast<LPARAM>(new std::string(s))); });
            if (!*flag || !PostMessageW(hw, kRead, 0, reinterpret_cast<LPARAM>(l))) delete l;
        }).detach();
    }
    void refill() {
        std::vector<RowList::Item> v;
        for (size_t i = 0; i < list.items.size(); i++) {
            RowList::Item it;
            it.key = std::to_string(i);
            it.icon = picked[i] ? "check" : "";
            it.iconColor = P.accent;
            it.cols = {"", list.items[i].title.empty() ? list.items[i].url : list.items[i].title, list.items[i].duration > 0 ? clock(list.items[i].duration) : ""};
            v.push_back(it);
        }
        int keepTop = rows.top;
        rows.setItems(v);
        rows.top = keepTop;
        rows.updateScroll();
        rows.selected = -1;
        int n = static_cast<int>(std::count(picked.begin(), picked.end(), true));
        go.setText(n == 1 ? "Download 1 video" : "Download " + std::to_string(n) + " videos");
        EnableWindow(go.hwnd(), n > 0 || sub.checked);
        InvalidateRect(h, nullptr, FALSE);
    }
    void start() {
        std::string q = quality.value(), dir = trim(folder.get());
        if (dir.empty()) dir = folderFor("video");
        makeDirs(dir);
        int added = 0;
        for (size_t i = 0; i < list.items.size(); i++) {
            if (!picked[i]) continue;
            Task t = playlists::videoTask(list.items[i].url, list.items[i].title, q, dir);
            t.cookies = cookies;
            Engine::get().add(t, true);
            added++;
        }
        if (sub.checked) {
            playlists::List seen = list;   // what's here now counts as seen (the ones picked are downloading)
            playlists::subscribe(seen, q, dir);
        }
        refreshMainWindow();
        close(0);
        if (sub.checked) askBox(nullptr, "Subscribed", "DDM checks " + (list.title.empty() ? std::string("this list") : list.title) + " every few hours and downloads its new videos by itself (" + q + ").\n\nTools → Subscriptions shows and stops your subscriptions.", {"OK"}, 0);
        (void)added;
    }
    void layout() override {
        RECT r = client();
        int pad = S(18);
        rows.move(pad, S(70), r.right - 2 * pad, r.bottom - S(70) - S(170));
        int y = r.bottom - S(160);
        all.move(pad, y, S(110), S(30));
        none.move(pad + S(116), y, S(120), S(30));
        y += S(44);
        quality.move(pad + S(70), y, S(130), S(32));
        folder.move(pad + S(280), y, r.right - pad - S(280) - S(110) - pad, S(32));
        browse.move(r.right - pad - S(100), y, S(100), S(32));
        y += S(44);
        sub.move(pad, y, r.right - 2 * pad, S(26));
        go.move(r.right - pad - S(200), r.bottom - S(46), S(200), S(34));
        cancel.move(r.right - pad - S(200) - S(108), r.bottom - S(46), S(100), S(34));
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(18);
        RECT t1{pad, S(14), r.right - pad, S(40)};
        (loading || list.title.empty() ? text : textName)(dc, loading ? status : (list.title.empty() ? "Videos on this page" : list.title), t1, list.error.empty() ? P.text : P.bad, fontBig(), DT_END_ELLIPSIS);
        RECT t2{pad, S(42), r.right - pad, S(64)};
        std::string sub2 = loading ? url : !list.error.empty() ? list.error
                                     : std::to_string(list.items.size()) + " videos" + (list.author.empty() || list.author == list.title ? "" : " · " + list.author) + (list.kind == "page" ? " · links on this page" : "");
        text(dc, sub2, t2, P.muted, fontUI(), DT_END_ELLIPSIS);
        int y = r.bottom - S(116);
        RECT q{pad, y, pad + S(66), y + S(32)};
        text(dc, "Quality", q, P.text, fontUI(), DT_VCENTER | DT_SINGLELINE);
        RECT fo{pad + S(214), y, pad + S(276), y + S(32)};
        text(dc, "Save to", fo, P.text, fontUI(), DT_VCENTER | DT_SINGLELINE);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == kStatus) { std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l)); status = *s; InvalidateRect(h, nullptr, FALSE); return 0; }
        if (m == kRead) {
            std::unique_ptr<playlists::List> got(reinterpret_cast<playlists::List*>(l));
            list = *got;
            loading = false;
            picked.assign(list.items.size(), true);
            if (!list.title.empty()) SetWindowTextW(h, W(i18n::tr("Download playlist") + " — " + list.title).c_str());
            refill();
            return 0;
        }
        return Dialog::proc(m, w, l);
    }
};

// =============================================================================================== subscriptions
class SubsWnd : public Dialog {
public:
    RowList rows;
    Button check, pause, remove, close_;
    std::vector<playlists::Sub> subs;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::string note;
    static const UINT kChecked = WM_APP + 73;
    ~SubsWnd() override { *alive = false; }
    void build() {
        rows.create(h);
        rows.headers = {"Subscription", "Quality", "Downloaded", "Last new video"};
        rows.widths = {S(330), S(90), S(100), S(160)};
        rows.nameCols = {0};
        check.create(h, "Check now", BtnStyle::Primary, "refresh");
        pause.create(h, "Pause");
        remove.create(h, "Unsubscribe");
        close_.create(h, "Close");
        rows.onSelect = [this](int) { sync(); };
        check.onClick = [this] {
            note = "Checking…";
            InvalidateRect(h, nullptr, FALSE);
            HWND hw = h;
            auto flag = alive;
            std::thread([hw, flag] {
                auto tasks = playlists::checkSubscriptions(true);
                auto* v = new std::vector<Task>(tasks);
                if (!*flag || !PostMessageW(hw, kChecked, 0, reinterpret_cast<LPARAM>(v))) { for (auto& t : *v) Engine::get().add(t, true); delete v; }
            }).detach();
        };
        pause.onClick = [this] { int i = rows.selected; if (i < 0 || i >= static_cast<int>(subs.size())) return; playlists::setPaused(subs[static_cast<size_t>(i)].id, !subs[static_cast<size_t>(i)].paused); reload(); };
        remove.onClick = [this] {
            int i = rows.selected;
            if (i < 0 || i >= static_cast<int>(subs.size())) return;
            if (askBox(h, "Unsubscribe", "Stop downloading new videos of " + subs[static_cast<size_t>(i)].name + "?\n\nVideos already downloaded stay.", {"Unsubscribe", "Cancel"}, 1) != 0) return;
            playlists::unsubscribe(subs[static_cast<size_t>(i)].id);
            reload();
        };
        close_.onClick = [this] { close(0); };
        reload();
        layout();
    }
    void reload() {
        subs = playlists::subscriptions();
        std::vector<RowList::Item> v;
        for (auto& s : subs) {
            RowList::Item it;
            it.key = s.id;
            it.icon = s.paused ? "pause" : "refresh";
            it.iconColor = s.paused ? P.warn : P.accent;
            std::string last = s.lastFound ? formatDate(s.lastFound * 1000) : "—";
            it.cols = {s.name + (s.paused ? " (" + i18n::tr("Paused") + ")" : ""), s.quality.empty() ? "Best" : s.quality, std::to_string(s.downloaded), last};
            v.push_back(it);
        }
        rows.setItems(v);
        sync();
        InvalidateRect(h, nullptr, FALSE);
    }
    void sync() {
        int i = rows.selected;
        bool ok = i >= 0 && i < static_cast<int>(subs.size());
        EnableWindow(pause.hwnd(), ok);
        EnableWindow(remove.hwnd(), ok);
        pause.setText(ok && subs[static_cast<size_t>(i)].paused ? "Resume" : "Pause");
    }
    void layout() override {
        RECT r = client();
        int pad = S(18);
        rows.move(pad, S(70), r.right - 2 * pad, r.bottom - S(70) - S(64));
        int y = r.bottom - S(48);
        check.move(pad, y, S(140), S(34));
        pause.move(pad + S(148), y, S(100), S(34));
        remove.move(pad + S(256), y, S(130), S(34));
        close_.move(r.right - pad - S(100), y, S(100), S(34));
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT t1{S(18), S(14), r.right - S(18), S(40)};
        text(dc, "Subscriptions", t1, P.text, fontBig());
        RECT t2{S(18), S(42), r.right - S(18), S(64)};
        text(dc, !note.empty() ? note : subs.empty() ? "None yet. Paste a YouTube playlist or channel link (or Tasks → Download playlist or channel…) and tick Subscribe." :
                 "DDM checks every few hours and downloads new videos by itself, in the quality you chose.", t2, P.muted, fontUI(), DT_END_ELLIPSIS);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == kChecked) {
            std::unique_ptr<std::vector<Task>> v(reinterpret_cast<std::vector<Task>*>(l));
            for (auto& t : *v) Engine::get().add(t, true);
            note = v->empty() ? "No new videos." : std::to_string(v->size()) + " new " + (v->size() == 1 ? "video" : "videos") + " added to the downloads.";
            refreshMainWindow();
            reload();
            return 0;
        }
        return Dialog::proc(m, w, l);
    }
};

// =============================================================================================== phone remote
class RemoteWnd : public Dialog {
public:
    CheckBox on;
    Button newKey, copy, close_;
    qr::Code code;
    std::vector<std::string> addrs;
    std::string error;
    void build() {
        on.create(h, "Phone remote is on", settings().remoteOn && remote::running());
        newKey.create(h, "New link", BtnStyle::Normal, "refresh");
        copy.create(h, "Copy link");
        close_.create(h, "Close", BtnStyle::Primary);
        on.onChange = [this](bool v) {
            error.clear();
            if (v) { if (!remote::start(error)) { on.set(false); v = false; } }
            else remote::stop();
            settings().remoteOn = v;
            saveSettings();
            refresh();
        };
        newKey.onClick = [this] {
            if (askBox(h, "New link", "Make a new secret link? Phones that use the old one can't connect any more until they scan the new QR code.", {"New link", "Cancel"}, 1) != 0) return;
            remote::newKey();
            refresh();
        };
        copy.onClick = [this] { if (!addrs.empty()) copyText(h, addrs[0]); };
        close_.onClick = [this] { close(0); };
        refresh();
        layout();
    }
    void refresh() {
        addrs = remote::addresses();
        code = qr::Code();
        if (remote::running() && !addrs.empty()) qr::encode(addrs[0], code);
        EnableWindow(copy.hwnd(), remote::running() && !addrs.empty());
        InvalidateRect(h, nullptr, FALSE);
    }
    void layout() override {
        RECT r = client();
        int pad = S(22);
        on.move(pad, S(70), S(300), S(28));
        int y = r.bottom - S(52);
        newKey.move(pad, y, S(120), S(34));
        copy.move(pad + S(128), y, S(110), S(34));
        close_.move(r.right - pad - S(100), y, S(100), S(34));
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(22);
        RECT t1{pad, S(16), r.right - pad, S(42)};
        text(dc, "Phone remote", t1, P.text, fontBig());
        RECT t2{pad, S(42), r.right - pad, S(64)};
        text(dc, "See and control your downloads from your phone, on the same Wi-Fi as this PC.", t2, P.muted, fontUI(), DT_END_ELLIPSIS);
        int qs = S(240);
        RECT box{pad, S(110), pad + qs, S(110) + qs};
        if (code.size > 0) {   // the QR code: dark on white whatever the theme (phones read it best that way)
            fill(dc, box, RGB(255, 255, 255));
            int quiet = 4, cells = code.size + 2 * quiet;
            double cell = static_cast<double>(qs) / cells;
            HBRUSH b = CreateSolidBrush(RGB(0, 0, 0));
            for (int y = 0; y < code.size; y++)
                for (int x = 0; x < code.size; x++)
                    if (code.at(x, y)) {
                        RECT c{box.left + static_cast<LONG>((x + quiet) * cell), box.top + static_cast<LONG>((y + quiet) * cell), box.left + static_cast<LONG>((x + quiet + 1) * cell),
                               box.top + static_cast<LONG>((y + quiet + 1) * cell)};
                        FillRect(dc, &c, b);
                    }
            DeleteObject(b);
        } else {
            fillRound(dc, box, Sf(10), P.tile);
            RECT ic = box;
            InflateRect(&ic, -qs / 3, -qs / 3);
            icon(dc, "M7 2h10v20H7z M11 18h2", ic, P.muted, 2.0f);
        }
        RECT info{pad + qs + S(24), S(110), r.right - pad, r.bottom - S(64)};
        std::string body;
        if (!error.empty()) body = error;
        else if (!remote::running()) body = "Turn it on, then scan the code with your phone's camera.\n\nOnly someone with the code (or its link) can get in: the link holds a secret key. It works on your home network only; nothing goes through the internet.";
        else if (addrs.empty()) body = "This PC isn't connected to a network right now.";
        else {
            body = "Scan the code with your phone's camera (the phone must be on the same Wi-Fi), or open this link on it:\n\n" + addrs[0] +
                   "\n\nAdd links, pause, resume, stop or remove downloads and watch their progress.\n\nIf Windows asks whether DDM may use your network, allow private networks.";
            if (addrs.size() > 1) body += "\n\nOther networks of this PC: " + addrs[1].substr(0, addrs[1].find("/?"));
        }
        text(dc, body, info, error.empty() ? P.text : P.bad, fontUI(), DT_WORDBREAK);
    }
};

} // namespace

void showPlayer(HWND owner, const std::string& taskId) {
    Task t;
    if (!Engine::get().find(taskId, t)) return;
    auto* d = new PlayerWnd();
    d->id = taskId;
    showModelessWnd(d, owner, "Play — " + t.fileName, S(960), S(620));
}

void showPlaylist(HWND owner, const std::string& url, const std::string& cookies) {
    auto* d = new PlaylistWnd();
    d->url = url;
    d->cookies = cookies;
    showModelessWnd(d, owner, "Download playlist", S(820), S(640));
}

void askPlaylist(HWND owner) {
    std::string u = clipText();
    if (!(startsWith(lower(trim(u)), "http://") || startsWith(lower(trim(u)), "https://"))) u.clear();
    if (!inputBox(owner, "Download playlist or channel", "Link of a YouTube playlist or channel, or of a page with many videos", u) || trim(u).empty()) return;
    showPlaylist(owner, trim(u), "");
}

void showSubscriptions(HWND owner) { showModelessWnd(new SubsWnd(), owner, "Subscriptions", S(760), S(460)); }
void showRemote(HWND owner) { showModelessWnd(new RemoteWnd(), owner, "Phone remote", S(720), S(440), false); }

} // namespace ui
} // namespace ddm
