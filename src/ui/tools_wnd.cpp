// Tools → File tools and Tools → Statistics.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../core/engine.hpp"
#include "../core/engines.hpp"
#include "../core/filetools.hpp"
#include "../core/bgremove.hpp"
#include "../core/integration.hpp"
#include "../core/settings.hpp"
#include "../core/stats.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <algorithm>
#include <map>
#include <memory>
#include <thread>

namespace ddm {
namespace ui {

namespace {

const UINT kToolProgress = WM_APP + 80, kToolDone = WM_APP + 81;

void showInFolder(const std::string& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(W(path).c_str());
    if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
}

// ----------------------------------------------------------------- a tool at work: progress, Stop, then the result
struct ToolResult { std::string path, message, error; };
using Job = std::function<ToolResult(tools::Progress, std::atomic<bool>*)>;

class RunWnd : public Dialog {
public:
    std::string what, status = "Starting…";
    double frac = -1;
    bool done = false;
    ToolResult res;
    std::atomic<bool> stop{false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    Button stopBtn, openBtn;
    std::function<void(const ToolResult&)> after;
    ~RunWnd() override { stop = true; *alive = false; }
    void start(Job job) {
        stopBtn.create(h, "Stop");
        openBtn.create(h, "Show in folder", BtnStyle::Primary, "folder");
        openBtn.show(false);
        stopBtn.onClick = [this] { if (done) close(0); else { stop = true; status = "Stopping…"; redraw(); } };
        openBtn.onClick = [this] { showInFolder(res.path); close(0); };
        onEnter = stopBtn.onClick;
        layout();
        HWND hw = h;
        auto flag = alive;
        std::atomic<bool>* st = &stop;
        std::thread([hw, job, flag, st] {
            ToolResult r = job([hw, flag](double f, const std::string& s) {
                static DWORD last = 0;
                DWORD now = GetTickCount();
                if (now - last < 120 && f < 1) return;
                last = now;
                if (*flag) PostMessageW(hw, kToolProgress, 0, reinterpret_cast<LPARAM>(new std::pair<double, std::string>(f, s)));
            }, st);
            auto* p = new ToolResult(r);
            if (!*flag || !PostMessageW(hw, kToolDone, 0, reinterpret_cast<LPARAM>(p))) delete p;
        }).detach();
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == kToolProgress) { std::unique_ptr<std::pair<double, std::string>> p(reinterpret_cast<std::pair<double, std::string>*>(l)); if (!done) { frac = p->first; status = p->second; redraw(); } return 0; }
        if (m == kToolDone) {
            std::unique_ptr<ToolResult> r(reinterpret_cast<ToolResult*>(l));
            res = *r; done = true; frac = res.error.empty() ? 1 : frac;
            status = !res.error.empty() ? res.error : !res.message.empty() ? res.message : "Done: " + baseName(res.path);
            stopBtn.setText("Close");
            openBtn.show(res.error.empty() && !res.path.empty());
            layout(); redraw();
            if (after) after(res);
            return 0;
        }
        if (m == WM_CLOSE && !done) { stop = true; }
        return Dialog::proc(m, w, l);
    }
    void layout() override {
        RECT r = client();
        int x = r.right - S(18);
        for (Button* b : {&stopBtn, &openBtn}) { if (!IsWindowVisible(b->hwnd()) && b == &openBtn) continue; int bw = std::max(S(96), b->preferredWidth()); x -= bw; b->move(x, r.bottom - S(50), bw, S(34)); x -= S(8); }
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT t{S(18), S(16), r.right - S(18), S(42)};
        text(dc, what, t, P.text, fontBig(), DT_END_ELLIPSIS);
        RECT bar{S(18), S(58), r.right - S(18), S(66)};
        fillRound(dc, bar, Sf(4), P.track);
        if (frac > 0) { RECT f = bar; f.right = bar.left + static_cast<LONG>((bar.right - bar.left) * std::min(1.0, frac)); fillRound(dc, f, Sf(4), !res.error.empty() ? P.bad : done ? P.good : P.accent); }
        else if (!done) { RECT f = bar; int span = (bar.right - bar.left) / 4, off = static_cast<int>((GetTickCount() / 8) % static_cast<DWORD>(bar.right - bar.left + span)) - span; f.left = std::max<LONG>(bar.left, bar.left + off); f.right = std::min<LONG>(bar.right, bar.left + off + span); if (f.right > f.left) fillRound(dc, f, Sf(4), P.accent); SetTimer(h, 1, 40, nullptr); }
        RECT s{S(18), S(74), r.right - S(18), r.bottom - S(60)};
        text(dc, status, s, !res.error.empty() ? P.bad : P.muted, fontUI(), DT_WORDBREAK);
    }
};

void runTool(HWND owner, const std::string& what, Job job, std::function<void(const ToolResult&)> after = nullptr) {
    auto* d = new RunWnd();
    d->what = what;
    d->after = after;
    if (!d->createDialog(owner, what, S(520), S(200), true)) { delete d; return; }
    d->start(job);
    ShowWindow(d->hwnd(), SW_SHOW);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_TIMER) { InvalidateRect(hw, nullptr, FALSE); KillTimer(hw, 1); }
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<RunWnd*>(ref); return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
}

// "1:20" / "80" / "1:02:03.5" -> seconds (-1 = not a time)
double parseTime(const std::string& s0) {
    std::string s = trim(s0);
    if (s.empty()) return -1;
    double v = 0, part = 0;
    bool any = false;
    for (auto& p : split(s, ':')) { char* e = nullptr; part = strtod(p.c_str(), &e); if (e == p.c_str()) return -1; v = v * 60 + part; any = true; }
    return any ? v : -1;
}

const char* kMedia = "Video and audio|*.mp4;*.mkv;*.webm;*.mov;*.avi;*.m4v;*.flv;*.wmv;*.3gp;*.ts;*.mp3;*.m4a;*.aac;*.ogg;*.opus;*.wav;*.flac|All files|*.*";
const char* kVideo = "Videos|*.mp4;*.mkv;*.webm;*.mov;*.avi;*.m4v;*.flv;*.wmv;*.3gp;*.ts|All files|*.*";

// ----------------------------------------------------------------- one tool on one file (File tools, or right-click in the list)
} // namespace

void fileTool(HWND me, const std::string& key, std::string f) {
    if (key == "extract") {
        if (f.empty()) f = pickFile(me, "Archives|*.zip;*.tar;*.tgz;*.gz;*.iso;*.rar;*.jar;*.apk;*.docx;*.xlsx|All files|*.*");
        if (f.empty()) return;
        std::string out = uniquePath(joinPath(dirName(f), stem(baseName(f))));
        runTool(me, "Extracting " + baseName(f), [f, out](tools::Progress p, std::atomic<bool>* st) {
            ToolResult r; int n = tools::extract(f, out, p, st, r.error);
            if (n >= 0) { r.path = out; r.message = std::to_string(n) + (n == 1 ? " file" : " files") + " extracted into " + out + (r.error.empty() ? "." : ". " + r.error); r.error.clear(); }
            return r;
        });
    } else if (key == "zip") {   // a file (right-click) or a folder (File tools)
        std::string d = f.empty() ? pickFolder(me, downloadsDir()) : f;
        if (d.empty()) return;
        std::string zip = uniquePath((dirExists(d) ? d : joinPath(dirName(d), stem(baseName(d)))) + ".zip");
        runTool(me, "Packing " + baseName(d), [d, zip](tools::Progress p, std::atomic<bool>* st) { ToolResult r; if (tools::createZip({d}, zip, p, st, r.error)) { r.path = zip; r.message = "Saved " + baseName(zip) + " (" + formatBytes(fileSize(zip)) + ")."; } return r; });
    } else if (key == "compress-video") {
        if (f.empty()) f = pickFile(me, kVideo);
        if (f.empty()) return;
        int q = askBox(me, "Compress a video", "How small?", {"Smallest", "Balanced", "Best quality", "Cancel"}, 1);
        if (q < 0 || q > 2) return;
        runTool(me, "Compressing " + baseName(f), [f, q](tools::Progress p, std::atomic<bool>* st) { ToolResult r; r.path = tools::compressVideo(f, q, p, st, r.error); if (!r.path.empty()) r.message = "Saved " + baseName(r.path) + ": " + formatBytes(fileSize(f)) + " → " + formatBytes(fileSize(r.path)) + "."; return r; });
    } else if (key == "mp3") {
        if (f.empty()) f = pickFile(me, kMedia);
        if (f.empty()) return;
        runTool(me, "Converting " + baseName(f), [f](tools::Progress p, std::atomic<bool>* st) { ToolResult r; r.path = tools::toMp3(f, p, st, r.error); return r; });
    } else if (key == "smaller-audio") {
        if (f.empty()) f = pickFile(me, kMedia);
        if (f.empty()) return;
        int k = askBox(me, "Smaller audio", "Quality of the new file", {"64 kbps (speech)", "96 kbps", "128 kbps (music)", "Cancel"}, 2);
        if (k < 0 || k > 2) return;
        int kbps = k == 0 ? 64 : k == 1 ? 96 : 128;
        runTool(me, "Making " + baseName(f) + " smaller", [f, kbps](tools::Progress p, std::atomic<bool>* st) { ToolResult r; r.path = tools::smallerAudio(f, kbps, p, st, r.error); return r; });
    } else if (key == "trim") {
        if (f.empty()) f = pickFile(me, kMedia);
        if (f.empty()) return;
        std::string v = "0:00-0:30";
        if (!inputBox(me, "Trim", "The part to keep, from-to (for example 1:20-2:45). The file is " + formatDuration(static_cast<int64_t>(tools::durationOf(f))) + " long", v)) return;
        size_t dash = v.find('-');
        double a = dash == std::string::npos ? -1 : parseTime(v.substr(0, dash)), b = dash == std::string::npos ? -1 : parseTime(v.substr(dash + 1));
        if (a < 0 || b <= a) { askBox(me, "Trim", "Write it like 1:20-2:45 (start-end).", {"OK"}, 0); return; }
        runTool(me, "Cutting " + baseName(f), [f, a, b](tools::Progress p, std::atomic<bool>* st) { ToolResult r; r.path = tools::trim(f, a, b, p, st, r.error); return r; });
    } else if (key == "gif") {
        if (f.empty()) f = pickFile(me, kVideo);
        if (f.empty()) return;
        std::string v = "0:00 5";
        if (!inputBox(me, "Video to GIF", "Start and length in seconds (for example 0:12 4)", v)) return;
        auto parts = split(trim(v), ' ');
        double a = parts.empty() ? -1 : parseTime(parts[0]), len = parts.size() > 1 ? parseTime(parts.back()) : 5;
        if (a < 0 || len <= 0) { askBox(me, "Video to GIF", "Write it like 0:12 4 (start, then length).", {"OK"}, 0); return; }
        runTool(me, "Making a GIF", [f, a, len](tools::Progress p, std::atomic<bool>* st) { ToolResult r; r.path = tools::toGif(f, a, len, 480, p, st, r.error); return r; });
    } else if (key == "remove-bg") {
        if (f.empty()) f = pickFile(me, "Pictures|*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff|All files|*.*");
        if (f.empty()) return;
        if (!IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE)) { askBox(me, "Remove image background", "This needs a processor with SSE2 (any PC from the last 20 years).", {"OK"}, 0); return; }
        runTool(me, "Removing the background of " + baseName(f), [f](tools::Progress p, std::atomic<bool>* st) {
            ToolResult r;
            int64_t t0 = nowMs();
            r.path = bgremove::removeBackground(f, p, st, r.error);
            if (!r.path.empty()) { char secs[32]; snprintf(secs, sizeof secs, "%.1f", (nowMs() - t0) / 1000.0); r.message = "Saved " + baseName(r.path) + " (transparent background) in " + secs + " s."; }
            return r;
        });
    } else if (key == "compress-picture") {
        if (f.empty()) f = pickFile(me, "Pictures|*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff|All files|*.*");
        if (f.empty()) return;
        std::string ext = lower(extOf(f)) == "png" ? "png" : "jpg";
        std::string out = uniquePath(joinPath(dirName(f), stem(baseName(f)) + " (smaller)." + ext));
        runTool(me, "Compressing " + baseName(f), [f, out](tools::Progress p, std::atomic<bool>*) {
            ToolResult r; if (p) p(-1, "Compressing…");
            if (compressImage(f, out, 1920, 82, r.error)) { r.path = out; r.message = "Saved " + baseName(out) + ": " + formatBytes(fileSize(f)) + " → " + formatBytes(fileSize(out)) + "."; }
            return r;
        });
    } else if (key == "strip-meta") {
        if (f.empty()) f = pickFile(me, "Pictures, video, audio and Office documents|*.jpg;*.jpeg;*.png;*.webp;*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.mp3;*.m4a;*.flac;*.ogg;*.opus;*.wav;*.docx;*.xlsx;*.pptx;*.odt;*.ods;*.odp|All files|*.*");
        if (f.empty()) return;
        if (!tools::canStripMetadata(f) || lower(extOf(f)) == "pdf") {
            askBox(me, "Remove metadata", lower(extOf(f)) == "pdf" ? "PDF files aren't supported: their properties can't be removed without leaving a copy inside the file." : "DDM can remove metadata from pictures (JPEG, PNG, WebP), video, audio and Office documents (Word, Excel, PowerPoint, OpenDocument).", {"OK"}, 0);
            return;
        }
        if (askBox(me, "Remove metadata", "Remove everything " + baseName(f) + " says about where, when, by whom and with what it was made (camera, GPS position, dates, author, software, comments, tags)?\n\nThe picture, sound or text stays exactly as it is. The original goes to the Recycle Bin.", {"Remove metadata", "Cancel"}, 0) != 0) return;
        runTool(me, "Removing metadata from " + baseName(f), [f](tools::Progress p, std::atomic<bool>* st) {
            ToolResult r; std::string sum;
            r.path = tools::stripMetadata(f, p, st, r.error, &sum);
            if (!r.path.empty()) r.message = sum;
            return r;
        });
    } else if (key == "checksums") {
        // handled by the caller (Verify checksum…)
    }
}

namespace {
// ----------------------------------------------------------------- File tools
class ToolsWnd : public Dialog {
public:
    struct Item { std::string section, title, desc, button, icon; std::function<void()> act; Button* btn = nullptr; RECT rc{}; int col = 0; };
    std::vector<Item> items;
    std::vector<std::unique_ptr<Button>> buttons;
    void add(int col, const std::string& sec, const std::string& t, const std::string& d, const std::string& b, const std::string& ic, std::function<void()> f) {
        Item it{sec, t, d, b, ic, f}; it.col = col; items.push_back(it);
    }
    void build() {
        HWND me = h;
        add(0, "Look inside", "X-Ray a link or a file", "Archives, disc images, torrents and programs: see what's inside before downloading", "Open…", "xray", [me] { showXRayFor(me, ""); });
        add(0, "Archives", "Extract", "ZIP, TAR, TAR.GZ, GZ and ISO, into a folder next to the archive", "Choose…", "M4 7h16v13H4z M4 7l2-3h12l2 3 M10 11h4", [me] { fileTool(me, "extract", ""); });
        add(0, "", "Create a ZIP", "Pack a folder into a ZIP file next to it", "Choose folder…", "M4 7h16v13H4z M4 7l2-3h12l2 3 M12 10v6 M9 13l3 3 3-3", [me] { fileTool(me, "zip", ""); });
        add(0, "Disk and network", "Find duplicate files", "The same file under different names: see them and free up space", "Choose folder…", "copy", [me] {
            std::string d = pickFolder(me, downloadsDir());
            if (d.empty()) return;
            auto groups = std::make_shared<std::vector<std::vector<std::string>>>();
            runTool(me, "Looking for duplicates", [d, groups](tools::Progress p, std::atomic<bool>* st) {
                *groups = tools::duplicates(d, p, st);
                ToolResult r;
                int64_t waste = 0; for (auto& g : *groups) waste += fileSize(g[0]) * static_cast<int64_t>(g.size() - 1);
                r.message = groups->empty() ? "No duplicates in " + d + "." : std::to_string(groups->size()) + (groups->size() == 1 ? " file has" : " files have") + " copies (" + formatBytes(waste) + " could be freed).";
                return r;
            }, [me, groups](const ToolResult& r) {
                if (groups->empty() || !r.error.empty()) return;
                std::string list;
                for (size_t i = 0; i < groups->size() && i < 8; i++) { list += "\n• " + baseName((*groups)[i][0]) + " — " + std::to_string((*groups)[i].size()) + " copies, " + formatBytes(fileSize((*groups)[i][0])) + " each"; }
                if (groups->size() > 8) list += "\n• …and " + std::to_string(groups->size() - 8) + " more";
                if (askBox(me, "Duplicate files", r.message + list + "\n\nKeep one copy of each and move the others to the Recycle Bin?", {"Move extra copies to the Recycle Bin", "Keep everything"}, 1) != 0) return;
                std::wstring from;
                for (auto& g : *groups) for (size_t i = 1; i < g.size(); i++) { from += W(g[i]); from.push_back(L'\0'); }
                from.push_back(L'\0');
                SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                SHFileOperationW(&op);
            });
        });
        add(0, "", "Connection speed test", "Finds how many connections give the best speed on your network, and uses it", "Run test", "M4 18a8 8 0 1 1 16 0 M12 18l4-6", [me] {
            auto res = std::make_shared<std::vector<std::pair<int, double>>>();
            auto best = std::make_shared<int>(0);
            runTool(me, "Connection speed test", [res, best](tools::Progress p, std::atomic<bool>* st) {
                ToolResult r; *best = tools::speedTest(p, st, *res, r.error);
                if (*best) { r.message = ""; for (auto& x : *res) r.message += std::to_string(x.first) + (x.first == 1 ? " connection: " : " connections: ") + formatSpeed(x.second) + "\n"; r.message += "Best: " + std::to_string(*best) + "."; }
                return r;
            }, [me, best](const ToolResult& r) {
                if (!*best) return;
                if (askBox(me, "Speed test", r.message + "\n\nUse " + std::to_string(*best) + " connections per download from now on?", {"Use it", "Keep " + std::to_string(settings().connections)}, 0) == 0) { settings().connections = *best; saveSettings(); }
            });
        });
        add(1, "Video and audio", "Compress a video", "Make a video much smaller (H.264), with sound", "Choose video…", "video", [me] { fileTool(me, "compress-video", ""); });
        add(1, "", "Convert to MP3", "The sound of a video, or any audio, as MP3", "Choose file…", "music", [me] { fileTool(me, "mp3", ""); });
        add(1, "", "Smaller audio", "Lower the bitrate of a song or podcast", "Choose file…", "music", [me] { fileTool(me, "smaller-audio", ""); });
        add(1, "", "Trim", "Cut a part of a video or song (no quality loss)", "Choose file…", "M6 6l12 12 M6 18L18 6 M4 6a2 2 0 1 0 4 0 2 2 0 1 0-4 0 M4 18a2 2 0 1 0 4 0 2 2 0 1 0-4 0", [me] { fileTool(me, "trim", ""); });
        add(1, "", "Video to GIF", "An animated GIF from a few seconds of a video", "Choose video…", "M4 5h16v14H4z M9 9v6 M13 9v6 M13 9h3 M13 12h2", [me] { fileTool(me, "gif", ""); });
        add(0, "Pictures and privacy", "Compress a picture", "A smaller copy (longest side 1920 px) of a JPEG, PNG, BMP or GIF", "Choose picture…", "image", [me] { fileTool(me, "compress-picture", ""); });
        add(0, "", "Remove metadata", "GPS position, camera, author, dates and tags out of pictures, video, audio and Office files", "Choose file…", "M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z M9 12l2 2 4-4", [me] { fileTool(me, "strip-meta", ""); });
        add(1, "Pictures", "Remove image background", "The subject cut out on a transparent background (PNG), worked out on your PC: nothing is uploaded", "Choose picture…", "M4 5h16v14H4z M8 15l3-3 2 2 3-4 2 3 M9 9.5a1.2 1.2 0 1 0 0.01 0", [me] { fileTool(me, "remove-bg", ""); });
        add(1, "Help", "Logs", "What DDM noted while working (useful if something goes wrong)", "Open", "M6 3h9l4 4v14H6z M9 12h7 M9 16h7", [] { showInFolder(joinPath(dataDir(), "ddm.log")); });
        for (auto& it : items) {
            auto b = std::make_unique<Button>();
            b->create(h, it.button);
            auto f = it.act;
            b->onClick = [f] { f(); };
            it.btn = b.get();
            buttons.push_back(std::move(b));
        }
        layout();
    }
    void layout() override {
        RECT r = client();
        int pad = S(20), gap = S(24), colW = (r.right - 2 * pad - gap) / 2;
        int y[2] = {S(70), S(70)};
        for (auto& it : items) {
            int x = pad + it.col * (colW + gap);
            int& yy = y[it.col];
            if (!it.section.empty()) yy += S(30);
            it.rc = RECT{x, yy, x + colW, yy + S(50)};
            int bw = std::max(S(96), it.btn->preferredWidth());
            it.btn->move(x + colW - bw, yy + S(9), bw, S(32));
            yy += S(54);
        }
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        RECT t{S(20), S(16), r.right - S(20), S(42)};
        text(dc, "File tools", t, P.text, fontBig());
        RECT st{S(20), S(42), r.right - S(20), S(62)};
        text(dc, "Everything runs on your PC: files are never uploaded.", st, P.muted, fontSmall());
        for (auto& it : items) {
            if (!it.section.empty()) {
                RECT sr{it.rc.left, it.rc.top - S(26), it.rc.right, it.rc.top - S(6)};
                text(dc, it.section, sr, P.text, fontBold());
            }
            RECT ic{it.rc.left, it.rc.top + S(12), it.rc.left + S(26), it.rc.top + S(38)};
            tileIcon(dc, ic, it.icon, blend(P.accent, P.win, 0.15f), blend(P.accent, RGB(0, 0, 0), 0.2f));
            int bw = std::max(S(96), it.btn->preferredWidth());
            RECT a{it.rc.left + S(36), it.rc.top + S(5), it.rc.right - bw - S(10), it.rc.top + S(25)};
            text(dc, it.title, a, P.text, fontBold(), DT_END_ELLIPSIS);
            RECT b{it.rc.left + S(36), it.rc.top + S(25), it.rc.right - bw - S(10), it.rc.top + S(45)};
            text(dc, it.desc, b, P.muted, fontSmall(), DT_END_ELLIPSIS);
        }
    }
};

// ----------------------------------------------------------------- Statistics
class StatsWnd : public Dialog {
public:
    Button clearBtn, closeBtn;
    void build() {
        clearBtn.create(h, "Clear statistics");
        closeBtn.create(h, "Close", BtnStyle::Primary);
        clearBtn.onClick = [this] { if (askBox(h, "Clear statistics", "Start counting from zero?", {"Clear", "Cancel"}, 1) == 0) { stats::clear(); redraw(); } };
        closeBtn.onClick = [this] { close(0); };
        onEnter = closeBtn.onClick;
        layout();
        SetTimer(h, 1, 2000, nullptr);
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_TIMER) { redraw(); return 0; }
        return Dialog::proc(m, w, l);
    }
    void layout() override {
        RECT r = client();
        int cw = std::max(S(90), closeBtn.preferredWidth()), kw = std::max(S(120), clearBtn.preferredWidth());
        closeBtn.move(r.right - S(20) - cw, r.bottom - S(50), cw, S(34));
        clearBtn.move(S(20), r.bottom - S(50), kw, S(34));
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(20), right = r.right - pad;
        RECT t{pad, S(14), right, S(40)};
        text(dc, "Statistics", t, P.text, fontBig());
        RECT st{pad, S(40), right, S(58)};
        text(dc, "Counted on your PC only.", st, P.muted, fontSmall());
        auto days = stats::lastDays(30);
        int64_t month = 0, week = 0;
        for (size_t i = 0; i < days.size(); i++) { month += days[i].second; if (i + 7 >= days.size()) week += days[i].second; }
        auto list = Engine::get().snapshot();
        int fin = 0, unf = 0, err = 0;
        for (auto& x : list) { if (x.state == State::Complete) fin++; else if (x.state == State::Error) err++; else unf++; }
        // tiles
        struct Tile { std::string k, v; };
        std::vector<Tile> tiles = {{"Today", formatBytes(days.back().second)}, {"Last 7 days", formatBytes(week)}, {"Last 30 days", formatBytes(month)}, {"All time", formatBytes(stats::total())},
                                   {"Finished", std::to_string(fin)}, {"Unfinished", std::to_string(unf + err)}, {"Fastest download", stats::fastest() > 0 ? formatSpeed(static_cast<double>(stats::fastest())) : "—"}, {"Speed now", Engine::get().totalSpeed() > 1 ? formatSpeed(Engine::get().totalSpeed()) : "—"}};
        int tw = (right - pad - S(8) * 3) / 4;
        for (size_t i = 0; i < tiles.size(); i++) {
            int x = pad + static_cast<int>(i % 4) * (tw + S(8)), y = S(68) + static_cast<int>(i / 4) * S(66);
            RECT b{x, y, x + tw, y + S(58)};
            fillRound(dc, b, Sf(8), P.tile);
            RECT k{x + S(12), y + S(8), x + tw - S(8), y + S(26)}; text(dc, tiles[i].k, k, P.muted, fontSmall());
            RECT v{x + S(12), y + S(26), x + tw - S(8), y + S(52)}; text(dc, tiles[i].v, v, P.text, fontBig(), DT_END_ELLIPSIS);
        }
        // chart: data per day
        int cy = S(68) + 2 * S(66) + S(10), ch = S(170);
        RECT card{pad, cy, right, cy + ch};
        fillRound(dc, card, Sf(8), P.tile);
        RECT ct{pad + S(14), cy + S(8), right - S(14), cy + S(28)};
        text(dc, "Data downloaded per day (last 30 days)", ct, P.text, fontBold());
        int64_t mx = 1;
        for (auto& d : days) mx = std::max(mx, d.second);
        RECT mxr{pad, cy + S(8), right - S(14), cy + S(28)};
        text(dc, formatBytes(mx), mxr, P.muted, fontSmall(), DT_RIGHT);
        int x0 = pad + S(14), x1 = right - S(14), y0 = cy + ch - S(26), y1 = cy + S(36);
        double bw = static_cast<double>(x1 - x0) / static_cast<double>(days.size());
        for (size_t i = 0; i < days.size(); i++) {
            int hgt = static_cast<int>((y0 - y1) * static_cast<double>(days[i].second) / static_cast<double>(mx));
            if (days[i].second > 0) { RECT b{x0 + static_cast<int>(i * bw + bw * 0.18), y0 - std::max(S(2), hgt), x0 + static_cast<int>(i * bw + bw * 0.82), y0}; fillRound(dc, b, Sf(2), i + 1 == days.size() ? P.good : P.accent); }
            if (i % 5 == 0 || i + 1 == days.size()) { RECT lb{x0 + static_cast<int>(i * bw) - S(20), y0 + S(4), x0 + static_cast<int>((i + 1) * bw) + S(20), y0 + S(22)}; text(dc, days[i].first.substr(5), lb, P.muted, fontSmall(), DT_CENTER); }
        }
        // top websites and categories
        int ly = cy + ch + S(14);
        int half = (right - pad - S(16)) / 2;
        RECT h1{pad, ly, pad + half, ly + S(22)}; text(dc, "Top websites", h1, P.text, fontBold());
        RECT h2{pad + half + S(16), ly, right, ly + S(22)}; text(dc, "By category", h2, P.text, fontBold());
        auto top = stats::topSites(6);
        int yy = ly + S(26);
        if (top.empty()) { RECT e{pad, yy, pad + half, yy + S(22)}; text(dc, "Nothing yet.", e, P.muted, fontUI()); }
        for (auto& s : top) {
            if (yy > r.bottom - S(78)) break;
            RECT fv{pad, yy + S(3), pad + S(16), yy + S(19)};
            if (!drawFavicon(dc, fv, s.first)) siteBadge(dc, fv, s.first);
            RECT n{pad + S(24), yy, pad + half - S(90), yy + S(22)}; textName(dc, s.first, n, P.text, fontUI(), DT_END_ELLIPSIS);
            RECT v{pad + half - S(90), yy, pad + half, yy + S(22)}; text(dc, formatBytes(s.second), v, P.muted, fontUI(), DT_RIGHT);
            yy += S(24);
        }
        std::map<std::string, std::pair<int, int64_t>> cats;
        for (auto& x : list) if (x.state == State::Complete) { auto& c = cats[categoryName(x.category)]; c.first++; c.second += std::max<int64_t>(0, x.total); }
        yy = ly + S(26);
        int cx = pad + half + S(16);
        for (auto& c : cats) {
            if (yy > r.bottom - S(78)) break;
            RECT n{cx, yy, right - S(150), yy + S(22)}; textName(dc, c.first, n, P.text, fontUI(), DT_END_ELLIPSIS);
            RECT v{right - S(150), yy, right, yy + S(22)}; text(dc, std::to_string(c.second.first) + (c.second.first == 1 ? " file · " : " files · ") + formatBytes(c.second.second), v, P.muted, fontUI(), DT_RIGHT);
            yy += S(24);
        }
    }
};

template <class T> void showModeless(T* d, HWND owner, const std::string& title, int w, int ht) {
    if (!d->createDialog(owner, title, w, ht, true)) { delete d; return; }
    d->build();
    ShowWindow(d->hwnd(), SW_SHOW);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, wp, lp); delete reinterpret_cast<T*>(ref); return res; }
        return DefSubclassProc(hw, m, wp, lp); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
}

// ----------------------------------------------------------------- Add DDM to your browsers (a gentle reminder)
class AddonWnd : public Dialog {
public:
    struct Row { integration::BrowserInfo b; std::unique_ptr<Button> btn; bool done = false; };
    std::vector<Row> rows;
    CheckBox never;
    Button closeB;
    void build() {
        for (auto& r : rows) {
            r.btn = std::make_unique<Button>();
            std::string key = r.b.key, name = r.b.name;
            r.btn->create(h, "Add to " + shortName(name), BtnStyle::Primary, "plus");
            Button* b = r.btn.get();
            r.btn->onClick = [this, key, b] {   // in that browser (not the default one): the store page with DDM's add-on
                std::string url = integration::listingFor(key);
                if (!integration::openInBrowser(key, url)) App::openUrl(url);
                b->iconName = "";
                b->setText("Opened — click \"Add\" there");
                layout();   // the longer text gets a wider button; the browser's name moves aside
                redraw();
            };
        }
        never.create(h, "Don't remind me again", false);
        closeB.create(h, "Not now");
        closeB.onClick = [this] { finish(); };
        layout();
        SetTimer(h, 1, 4000, nullptr);
    }
    static std::string shortName(const std::string& n) { return n == "Google Chrome" ? "Chrome" : n == "Microsoft Edge" ? "Edge" : n == "Mozilla Firefox" ? "Firefox" : n; }
    void finish() {
        if (never.checked) { settings().addonReminderOff = true; saveSettings(); }
        close(0);
    }
    void layout() override {
        RECT r = client();
        int pad = S(20), y = S(96);
        for (auto& row : rows) {
            int bw = std::max(S(150), row.btn->preferredWidth());
            row.btn->move(r.right - pad - bw, y + S(4), bw, S(32));
            row.btn->show(!row.done);
            y += S(44);
        }
        never.move(pad, r.bottom - S(50), S(260), S(24));
        closeB.move(r.right - pad - S(110), r.bottom - S(54), S(110), S(32));
    }
    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(20);
        RECT t{pad, S(16), r.right - pad, S(44)};
        text(dc, "Add DDM to your browser", t, P.text, fontBig());
        RECT st{pad, S(46), r.right - pad, S(92)};
        text(dc, "With the add-on, DDM catches downloads and shows \"Download this video\" on videos. It takes one click in the browser's store.", st, P.muted, fontSmall(), DT_WORDBREAK);
        int y = S(96);
        for (auto& row : rows) {
            RECT ic{pad, y + S(8), pad + S(24), y + S(32)};
            tileIcon(dc, ic, "globe", blend(P.accent, P.win, 0.15f), blend(P.accent, RGB(0, 0, 0), 0.2f));
            int bw = row.done ? S(150) : std::max(S(150), row.btn->preferredWidth());
            RECT n{pad + S(34), y, r.right - pad - bw - S(10), y + S(40)};
            text(dc, row.b.name, n, P.text, fontBold(), DT_END_ELLIPSIS);
            if (row.done) {
                RECT d{r.right - pad - S(170), y, r.right - pad - S(26), y + S(40)};
                text(dc, "Added", d, P.good, fontBold(), DT_RIGHT);
                RECT ck{r.right - pad - S(18), y + S(11), r.right - pad, y + S(29)};
                icon(dc, "check", ck, P.good, 2.4f);
            }
            y += S(44);
        }
    }
    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_TIMER && w == 1) {   // added meanwhile? tick it off; all done: close by itself
            bool all = true, changed = false;
            std::string seen = "," + settings().addonBrowsers + ",";
            for (auto& row : rows) {
                if (!row.done && (seen.find("," + row.b.key + ",") != std::string::npos || integration::addonInstalled(row.b.key))) { row.done = true; changed = true; }
                all = all && row.done;
            }
            if (changed) { layout(); redraw(); }
            if (all) { KillTimer(h, 1); SetTimer(h, 2, 2500, nullptr); }
            return 0;
        }
        if (m == WM_TIMER && w == 2) { KillTimer(h, 2); close(0); return 0; }
        if (m == WM_CLOSE) { finish(); return 0; }
        return Dialog::proc(m, w, l);
    }
};

} // namespace

// Browsers on this PC that don't have DDM's add-on yet (reminder: once a day until they do, or "don't remind me").
std::vector<integration::BrowserInfo> browsersWithoutAddon() {
    std::vector<integration::BrowserInfo> out;
    std::string seen = "," + settings().addonBrowsers + ",";
    for (auto& b : integration::browsers())
        if (b.installed && seen.find("," + b.key + ",") == std::string::npos && !integration::addonInstalled(b.key)) out.push_back(b);
    return out;
}

void showAddonReminder(HWND owner, const std::vector<integration::BrowserInfo>& missing) {
    static HWND open = nullptr;
    if (open && IsWindow(open)) { SetForegroundWindow(open); return; }
    if (missing.empty()) return;
    auto* d = new AddonWnd();
    for (auto& b : missing) { AddonWnd::Row r; r.b = b; d->rows.push_back(std::move(r)); }
    int ht = S(96) + static_cast<int>(missing.size()) * S(44) + S(80);
    showModeless(d, nullptr, "DDM", S(560), ht);
    open = d->hwnd();
    // like a notification: the bottom-right corner of the screen
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT wr; GetWindowRect(open, &wr);
    SetWindowPos(open, HWND_TOPMOST, wa.right - (wr.right - wr.left) - S(16), wa.bottom - (wr.bottom - wr.top) - S(16), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    (void)owner;
}

void showFileTools(HWND owner) { showModeless(new ToolsWnd(), owner, "File tools", S(980), S(612)); }
void showStatistics(HWND owner) { showModeless(new StatsWnd(), owner, "Statistics", S(760), S(620)); }

} // namespace ui
} // namespace ddm
