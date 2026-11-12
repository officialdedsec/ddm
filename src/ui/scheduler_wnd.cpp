// Scheduler, laid out like IDM's: queues on the left (Main download queue, Synchronization queue, your own queues,
// Download limits), and for the selected queue a Schedule tab and a Files in the queue tab.
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "downloadlist.hpp"
#include "rowlist.hpp"
#include "../core/engine.hpp"
#include "../core/queues.hpp"
#include "../core/settings.hpp"
#include "../common/util.hpp"
#include <windowsx.h>
#include <algorithm>
#include <memory>

namespace ddm {
namespace ui {

namespace {

void enable(Wnd& w, bool on) { EnableWindow(w.hwnd(), on); }
void enable(Field& f, bool on) { EnableWindow(f.hwnd(), on); EnableWindow(f.edit(), on); InvalidateRect(f.hwnd(), nullptr, TRUE); }

std::vector<std::string> numbers(int from, int to, bool pad) {
    std::vector<std::string> v;
    for (int i = from; i <= to; i++) { char b[8]; snprintf(b, sizeof b, pad ? "%02d" : "%d", i); v.push_back(b); }
    return v;
}

// hh:mm AM/PM picker made of three dropdowns
struct TimeBox {
    DropDown hour, minute, ampm;
    void create(HWND p, int sec) {
        hour.create(p, numbers(1, 12, false), 0);
        minute.create(p, numbers(0, 59, true), 0);
        ampm.create(p, {"AM", "PM"}, 0);
        set(sec);
    }
    void set(int sec) {
        int hh = (sec / 3600) % 24, mm = (sec / 60) % 60;
        hour.select((hh % 12 == 0 ? 12 : hh % 12) - 1);
        minute.select(mm);
        ampm.select(hh >= 12 ? 1 : 0);
    }
    int get() const { int hh = (hour.selected + 1) % 12 + (ampm.selected == 1 ? 12 : 0); return hh * 3600 + minute.selected * 60; }
    void move(int x, int y, int ht) { hour.move(x, y, S(62), ht); minute.move(x + S(66), y, S(62), ht); ampm.move(x + S(132), y, S(66), ht); }
    void enable(bool on) { EnableWindow(hour.hwnd(), on); EnableWindow(minute.hwnd(), on); EnableWindow(ampm.hwnd(), on); }
    void show(bool on) { hour.show(on); minute.show(on); ampm.show(on); }
};

struct DateBox {
    DropDown day, month, year;
    int baseYear = 2024;
    void create(HWND p, int ymd) {
        SYSTEMTIME n;
        GetLocalTime(&n);
        baseYear = n.wYear;
        if (ymd <= 0) ymd = n.wYear * 10000 + n.wMonth * 100 + n.wDay;
        day.create(p, numbers(1, 31, false), 0);
        month.create(p, {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"}, 0);
        year.create(p, numbers(baseYear, baseYear + 5, false), 0);
        set(ymd);
    }
    void set(int ymd) {
        int y = ymd / 10000, m = (ymd / 100) % 100, d = ymd % 100;
        day.select(std::max(0, std::min(30, d - 1)));
        month.select(std::max(0, std::min(11, m - 1)));
        year.select(std::max(0, std::min(5, y - baseYear)));
    }
    int get() const { return (baseYear + year.selected) * 10000 + (month.selected + 1) * 100 + day.selected + 1; }
    void move(int x, int y, int ht) { day.move(x, y, S(58), ht); month.move(x + S(62), y, S(120), ht); year.move(x + S(186), y, S(78), ht); }
    void enable(bool on) { EnableWindow(day.hwnd(), on); EnableWindow(month.hwnd(), on); EnableWindow(year.hwnd(), on); }
    void show(bool on) { day.show(on); month.show(on); year.show(on); }
};

class SchedulerWnd : public Dialog {
public:
    RowList qlist, files;
    Button newQ, delQ, startNow, stopNow, help, apply, closeBtn;
    int tab = 0;   // 0 Schedule, 1 Files in the queue
    RECT tabRects[2]{};
    std::string cur;    // queue id, or "limits"
    std::string title;
    bool dirtyQ = false, loading = false;
    // Schedule tab
    CheckBox oneTime, periodic, onStartup, startAt, once, daily, stopAt, retries, openFile, hangUp, exitDone, turnOff, force;
    CheckBox days[7];
    TimeBox startTime, stopTime;
    DateBox onceDate;
    Field retryCount, openPath, syncEvery;
    Label lSyncEvery, lMinutes;
    Button browse;
    DropDown turnOffMode;
    int sepY[4]{};
    // Files tab
    Button up, down, removeFile;
    Label lSim, lSim2;
    DropDown simultaneous;
    // Download limits
    CheckBox limOn, limWarn, limRecheck, condCharging;
    Label lNoMore, lMBytes, lEvery, lHours, lBattery, limNote;
    Field limMB, limHours;
    DropDown battery;

    void build() {
        HWND p = h;
        qlist.create(p);
        newQ.create(p, "New queue", BtnStyle::Normal, "plus");
        delQ.create(p, "Delete", BtnStyle::Normal, "delete");
        files.create(p);
        files.headers = {"File name", "Size", "Status"};
        files.nameCols = {0};
        qlist.nameCols = {0};
        oneTime.create(p, "One-time downloading", true); oneTime.radio = true;
        periodic.create(p, "Periodic synchronization", false); periodic.radio = true;
        onStartup.create(p, "Start download on DDM startup", false);
        startAt.create(p, "Start download at", false);
        startTime.create(p, 23 * 3600);
        once.create(p, "Once at", true); once.radio = true;
        onceDate.create(p, 0);
        daily.create(p, "Daily", false); daily.radio = true;
        const char* dn[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        for (int i = 0; i < 7; i++) days[i].create(p, dn[i], true);
        stopAt.create(p, "Stop download at", false);
        stopTime.create(p, 7 * 3600 + 1800);
        retries.create(p, "Number of retries for each file if downloading failed:", false);
        retryCount.create(p, "10");
        lSyncEvery.create(p, "Check the files for changes every");
        syncEvery.create(p, "120");
        lMinutes.create(p, "minutes");
        openFile.create(p, "Open the following file when done:", false);
        openPath.create(p, "");
        browse.create(p, "…");
        hangUp.create(p, "Hang up modem when done", false);
        exitDone.create(p, "Exit DDM when done", false);
        turnOff.create(p, "Turn off computer when done", false);
        turnOffMode.create(p, {"Shut down", "Sleep", "Hibernate", "Restart"}, 0);
        force.create(p, "Force processes to terminate", false);
        up.create(p, "Move up", BtnStyle::Normal, "chevronup");
        down.create(p, "Move down", BtnStyle::Normal, "M6 9l6 6 6-6");
        removeFile.create(p, "Delete from queue", BtnStyle::Normal, "delete");
        lSim.create(p, "Download");
        lSim2.create(p, "files at the same time");
        simultaneous.create(p, numbers(1, 10, false), 0);
        limOn.create(p, "Download limits", false);
        lNoMore.create(p, "Download no more than"); lMBytes.create(p, "MBytes"); lEvery.create(p, "every"); lHours.create(p, "hours");
        limMB.create(p, "200"); limHours.create(p, "5");
        limWarn.create(p, "Show warning when limit is reached", true);
        limRecheck.create(p, "Check the limit again on DDM startup", false);
        limNote.create(p, "Some internet providers allow only a certain amount of data per period. DDM pauses downloads when the limit is reached and continues them when it allows.");
        limNote.wrap = true; limNote.small = true; limNote.setColor(P.muted);
        condCharging.create(p, "Only download while the laptop is plugged in", settings().condCharging);
        lBattery.create(p, "Pause downloads when the battery is below");
        battery.create(p, {"Off", "10%", "20%", "30%", "40%", "50%"}, std::max(0, std::min(5, settings().condMinBattery / 10)));
        startNow.create(p, "Start now", BtnStyle::Primary, "qstart");
        stopNow.create(p, "Stop", BtnStyle::Normal, "qstop");
        help.create(p, "Help", BtnStyle::Normal, "help");
        apply.create(p, "Apply");
        closeBtn.create(p, "Close");

        qlist.onSelect = [this](int i) { if (i >= 0) { if (dirtyQ) save(); show(qlist.items[static_cast<size_t>(i)].key); } };
        newQ.onClick = [this] {
            std::string name;
            if (!inputBox(h, "New queue", "Name of the new queue", name)) return;
            if (dirtyQ) save();
            std::string id = queues::add(name);
            fillQueues();
            for (size_t i = 0; i < qlist.items.size(); i++) if (qlist.items[i].key == id) qlist.select(static_cast<int>(i));
        };
        delQ.onClick = [this] {
            if (cur == "main" || cur == "sync" || cur == "limits") return;
            if (askBox(h, "Delete queue", "Delete the queue \"" + title + "\"? Its downloads stay in the list.", {"Delete", "Cancel"}, 1) != 0) return;
            Engine::get().stopQueue(cur);
            std::vector<std::string> ids;
            { std::lock_guard<std::mutex> g(queues::lock()); if (auto q = queues::find(cur)) ids = q->order; }
            for (auto& id : ids) Engine::get().removeFromQueue(id);
            queues::remove(cur);
            dirtyQ = false;
            fillQueues();
            qlist.select(0);
        };
        auto mark = [this] { if (!loading) { dirtyQ = true; enableAll(); } };
        oneTime.onChange = [this, mark](bool) { periodic.set(false); mark(); layout(); redraw(); };
        periodic.onChange = [this, mark](bool) { oneTime.set(false); mark(); layout(); redraw(); };
        once.onChange = [this, mark](bool) { daily.set(false); mark(); };
        daily.onChange = [this, mark](bool) { once.set(false); mark(); };
        for (CheckBox* c : {&onStartup, &startAt, &stopAt, &retries, &openFile, &hangUp, &exitDone, &turnOff, &force, &limOn, &limWarn, &limRecheck, &condCharging}) c->onChange = [mark](bool) { mark(); };
        for (auto& d : days) d.onChange = [mark](bool) { mark(); };
        for (DropDown* d : {&startTime.hour, &startTime.minute, &startTime.ampm, &stopTime.hour, &stopTime.minute, &stopTime.ampm, &onceDate.day, &onceDate.month, &onceDate.year, &turnOffMode, &simultaneous, &battery})
            d->onChange = [mark](int) { mark(); };
        for (Field* f : {&retryCount, &openPath, &syncEvery, &limMB, &limHours}) f->onChange = [this] { if (!loading) dirtyQ = true; };
        browse.onClick = [this] { std::string f = pickFile(h, "All files|*.*"); if (!f.empty()) { openPath.set(f); openFile.set(true); dirtyQ = true; } };
        up.onClick = [this] { moveFile(-1); };
        down.onClick = [this] { moveFile(1); };
        removeFile.onClick = [this] {
            if (files.selected < 0) return;
            Engine::get().removeFromQueue(files.items[static_cast<size_t>(files.selected)].key);
            fillFiles();
        };
        startNow.onClick = [this] { if (cur == "limits") return; save(); Engine::get().startQueue(cur); enableAll(); };
        stopNow.onClick = [this] { if (cur == "limits") return; Engine::get().stopQueue(cur); enableAll(); };
        help.onClick = [this] { showHelpTopic(h, "scheduler"); };
        apply.onClick = [this] { save(); };
        closeBtn.onClick = [this] { if (dirtyQ) save(); close(0); };
        onEnter = closeBtn.onClick;
        fillQueues();
        qlist.select(0);
        SetTimer(h, 1, 1500, nullptr);
    }

    void fillQueues() {
        std::vector<RowList::Item> v;
        {
            std::lock_guard<std::mutex> g(queues::lock());
            for (auto& q : queues::all()) {
                RowList::Item it;
                it.key = q.id;
                it.cols = {q.name};
                it.icon = q.id == "main" ? "clock" : q.id == "sync" ? "refresh" : "folder";
                it.iconColor = q.id == "main" ? P.accent : q.id == "sync" ? P.good : P.folder;
                it.bold = q.running;
                v.push_back(it);
            }
        }
        RowList::Item lim;
        lim.key = "limits"; lim.cols = {"Download limits"}; lim.icon = "close"; lim.iconColor = P.bad;
        v.push_back(lim);
        qlist.setItems(v);
    }

    void fillFiles() {
        std::vector<RowList::Item> v;
        std::vector<std::string> order;
        { std::lock_guard<std::mutex> g(queues::lock()); if (auto q = queues::find(cur)) order = q->order; }
        for (auto& id : order) {
            Task t;
            if (!Engine::get().find(id, t)) continue;
            RowList::Item it;
            it.key = id;
            it.icon = iconFor(t);
            it.iconColor = iconColorFor(t);
            it.cols = {t.fileName, t.total > 0 ? formatBytes(t.total) : "", statusText(t)};
            v.push_back(it);
        }
        files.setItems(v);
    }

    void moveFile(int d) {
        if (files.selected < 0) return;
        std::string id = files.items[static_cast<size_t>(files.selected)].key;
        queues::move(cur, id, d);
        fillFiles();
        for (size_t i = 0; i < files.items.size(); i++) if (files.items[i].key == id) files.select(static_cast<int>(i));
    }

    void show(const std::string& id) {
        loading = true;
        cur = id;
        dirtyQ = false;
        if (id == "limits") {
            title = "Download limits";
            const DownloadLimits& L = queues::limits();
            limOn.set(L.on); limMB.set(std::to_string(L.maxMB)); limHours.set(std::to_string(L.everyHours)); limWarn.set(L.warn); limRecheck.set(L.recheck);
            condCharging.set(settings().condCharging);
            battery.select(std::max(0, std::min(5, settings().condMinBattery / 10)));
        } else {
            Queue q;
            { std::lock_guard<std::mutex> g(queues::lock()); if (auto p = queues::find(id)) q = *p; }
            title = q.name;
            oneTime.set(!q.periodic); periodic.set(q.periodic);
            onStartup.set(q.startOnStartup);
            startAt.set(q.startAtOn); startTime.set(q.startAt);
            once.set(!q.daily); daily.set(q.daily); onceDate.set(q.onceDate);
            for (int i = 0; i < 7; i++) days[i].set(q.days[static_cast<size_t>(i)] == '1');
            stopAt.set(q.stopAtOn); stopTime.set(q.stopAt);
            retries.set(q.retriesOn); retryCount.set(std::to_string(q.retries));
            syncEvery.set(std::to_string(q.syncEveryMin));
            openFile.set(q.openFileOn); openPath.set(q.openFile);
            hangUp.set(q.hangUp); exitDone.set(q.exitWhenDone);
            turnOff.set(q.turnOffOn);
            turnOffMode.select(q.turnOffMode == "sleep" ? 1 : q.turnOffMode == "hibernate" ? 2 : q.turnOffMode == "restart" ? 3 : 0);
            force.set(q.forceClose);
            simultaneous.select(std::max(0, std::min(9, q.simultaneous - 1)));
            fillFiles();
        }
        loading = false;
        layout();
        enableAll();
        redraw();
    }

    void save() {
        if (cur.empty()) return;
        if (cur == "limits") {
            {
                std::lock_guard<std::mutex> g(queues::lock());
                DownloadLimits& L = queues::limits();
                L.on = limOn.checked; L.maxMB = std::max(1, atoi(limMB.get().c_str())); L.everyHours = std::max(1, atoi(limHours.get().c_str()));
                L.warn = limWarn.checked; L.recheck = limRecheck.checked;
            }
            settings().condCharging = condCharging.checked;
            settings().condMinBattery = battery.selected * 10;
            saveSettings();
        } else {
            std::lock_guard<std::mutex> g(queues::lock());
            Queue* q = queues::find(cur);
            if (!q) return;
            q->periodic = periodic.checked;
            q->startOnStartup = onStartup.checked;
            bool timeChanged = q->startAt != startTime.get() || q->startAtOn != startAt.checked || q->daily != daily.checked || q->onceDate != onceDate.get();
            q->startAtOn = startAt.checked; q->startAt = startTime.get();
            q->daily = daily.checked; q->onceDate = onceDate.get();
            std::string d;
            for (auto& c : days) d += c.checked ? '1' : '0';
            q->days = d;
            if (q->stopAt != stopTime.get() || q->stopAtOn != stopAt.checked) q->lastStopDay = 0;
            q->stopAtOn = stopAt.checked; q->stopAt = stopTime.get();
            q->retriesOn = retries.checked; q->retries = std::max(1, std::min(999, atoi(retryCount.get().c_str())));
            q->syncEveryMin = std::max(5, atoi(syncEvery.get().c_str()));
            q->openFileOn = openFile.checked; q->openFile = trim(openPath.get());
            q->hangUp = hangUp.checked; q->exitWhenDone = exitDone.checked;
            q->turnOffOn = turnOff.checked;
            static const char* modes[] = {"shutdown", "sleep", "hibernate", "restart"};
            q->turnOffMode = modes[std::max(0, std::min(3, turnOffMode.selected))];
            q->forceClose = force.checked;
            q->simultaneous = simultaneous.selected + 1;
            if (timeChanged) q->lastStartDay = 0;   // a new start time can fire today
        }
        queues::save();
        dirtyQ = false;
        enableAll();
    }

    void enableAll() {
        bool lim = cur == "limits";
        bool st = startAt.checked;
        startTime.enable(st);
        enable(once, st); enable(daily, st);
        onceDate.enable(st && once.checked);
        for (auto& d : days) enable(d, st && daily.checked);
        stopTime.enable(stopAt.checked);
        enable(retryCount, retries.checked);
        enable(openPath, openFile.checked); enable(browse, openFile.checked);
        enable(turnOffMode, turnOff.checked); enable(force, turnOff.checked);
        enable(limMB, limOn.checked); enable(limHours, limOn.checked); enable(limWarn, limOn.checked); enable(limRecheck, limOn.checked);
        bool running = !lim && Engine::get().queueRunning(cur);
        enable(startNow, !lim && !running);
        enable(stopNow, !lim && running);
        enable(delQ, !lim && cur != "main" && cur != "sync");
        enable(apply, dirtyQ);
    }

    void showSet(std::initializer_list<Wnd*> ws, bool on) { for (auto w : ws) w->show(on); }

    void layout() override {
        RECT r = client();
        int pad = S(16), leftW = S(250), rh = S(28);
        int lx = pad, ly = S(70);
        qlist.move(lx, ly, leftW, r.bottom - ly - S(64));
        newQ.move(lx, r.bottom - S(50), S(120), S(34));
        delQ.move(lx + leftW - S(120), r.bottom - S(50), S(120), S(34));
        int x0 = lx + leftW + S(26), right = r.right - pad;
        // bottom buttons
        int bx = right;
        for (Button* b : {&closeBtn, &apply, &help, &stopNow, &startNow}) { int w = std::max(S(96), b->preferredWidth()); bx -= w; b->move(bx, r.bottom - S(50), w, S(34)); bx -= S(8); }
        bool lim = cur == "limits";
        bool sched = !lim && tab == 0, filesTab = !lim && tab == 1;
        int panelTop = S(80) + (lim ? 0 : S(32));
        int x = x0 + S(14), y = panelTop + S(12);
        // ---- schedule tab
        showSet({&oneTime, &periodic, &onStartup, &startAt, &stopAt, &retries, &openFile, &openPath, &browse, &hangUp, &exitDone, &turnOff, &turnOffMode, &force}, sched);
        startTime.show(sched); stopTime.show(sched);
        bool per = periodic.checked;
        showSet({&once, &daily}, sched);
        onceDate.show(sched);
        for (auto& d : days) d.show(sched);
        showSet({&retryCount}, sched && !per); retries.show(sched && !per);
        showSet({&lSyncEvery, &syncEvery, &lMinutes}, sched && per);
        if (sched) {
            // short windows (big text sizes, small screens): tighter rows and the days in two lines
            bool compact = r.bottom < S(730);
            int g = compact ? S(4) : S(10), step = compact ? S(25) : S(28), line = compact ? S(26) : S(32);
            if (compact) rh = S(26);
            oneTime.move(x, y, S(230), S(24)); periodic.move(x + S(260), y, S(240), S(24)); y += line; sepY[0] = y; y += g;
            onStartup.move(x, y, S(300), S(24)); y += line; sepY[1] = y; y += g;
            startAt.move(x, y + S(2), S(200), S(24)); startTime.move(x + S(220), y, rh); y += rh + g;
            once.move(x + S(16), y + S(2), S(100), S(24)); onceDate.move(x + S(220), y, rh); y += rh + g;
            daily.move(x + S(16), y, S(100), S(24));
            int perCol = compact ? 2 : 3, colW = compact ? S(98) : S(120);
            for (int i = 0; i < 7; i++) days[i].move(x + S(130) + (i / perCol) * colW, y + (i % perCol) * step, colW - S(4), S(24));
            y += step * perCol + S(6); sepY[2] = y; y += g;
            stopAt.move(x, y + S(2), S(200), S(24)); stopTime.move(x + S(220), y, rh); y += rh + g;
            if (per) { lSyncEvery.move(x, y, S(250), rh); syncEvery.move(x + S(254), y, S(60), rh); lMinutes.move(x + S(322), y, S(80), rh); }
            else { retries.move(x, y + S(2), S(390), S(24)); retryCount.move(x + S(394), y, S(56), rh); }
            y += rh + S(8); sepY[3] = y; y += g;
            openFile.move(x, y, S(320), S(24)); y += step;
            openPath.move(x, y, right - S(14) - x - S(40), rh); browse.move(right - S(14) - S(32), y, S(32), rh); y += rh + g;
            if (compact) { hangUp.move(x, y, S(250), S(24)); exitDone.move(x + S(260), y, S(220), S(24)); y += step; }
            else { hangUp.move(x, y, S(320), S(24)); y += step; exitDone.move(x, y, S(320), S(24)); y += step; }
            turnOff.move(x, y, S(300), S(24)); turnOffMode.move(right - S(14) - S(160), y - S(2), S(160), rh); y += step;
            force.move(x + S(24), y, S(300), S(24));
        }
        if (false) {
            oneTime.move(x, y, S(230), S(24)); periodic.move(x + S(260), y, S(240), S(24)); y += S(32); sepY[0] = y; y += S(10);
            onStartup.move(x, y, S(300), S(24)); y += S(32); sepY[1] = y; y += S(10);
            startAt.move(x, y + S(2), S(200), S(24)); startTime.move(x + S(220), y, rh); y += rh + S(10);
            once.move(x + S(16), y + S(2), S(100), S(24)); onceDate.move(x + S(220), y, rh); y += rh + S(10);
            daily.move(x + S(16), y, S(100), S(24));
            for (int i = 0; i < 7; i++) days[i].move(x + S(130) + (i / 3) * S(120), y + (i % 3) * S(28), S(116), S(24));
            y += S(28) * 3 + S(6); sepY[2] = y; y += S(10);
            stopAt.move(x, y + S(2), S(200), S(24)); stopTime.move(x + S(220), y, rh); y += rh + S(10);
            if (per) { lSyncEvery.move(x, y, S(250), rh); syncEvery.move(x + S(254), y, S(60), rh); lMinutes.move(x + S(322), y, S(80), rh); }
            else { retries.move(x, y + S(2), S(390), S(24)); retryCount.move(x + S(394), y, S(56), rh); }
            y += rh + S(8); sepY[3] = y; y += S(10);
            openFile.move(x, y, S(320), S(24)); y += S(28);
            openPath.move(x, y, right - S(14) - x - S(40), rh); browse.move(right - S(14) - S(32), y, S(32), rh); y += rh + S(10);
            hangUp.move(x, y, S(320), S(24)); y += S(28);
            exitDone.move(x, y, S(320), S(24)); y += S(28);
            turnOff.move(x, y, S(300), S(24)); turnOffMode.move(right - S(14) - S(160), y - S(2), S(160), rh); y += S(28);
            force.move(x + S(24), y, S(300), S(24));
        }
        // ---- files tab
        showSet({&files, &up, &down, &removeFile, &lSim, &lSim2, &simultaneous}, filesTab);
        if (filesTab) {
            int fy = panelTop + S(12);
            lSim.move(x, fy, S(76), rh); simultaneous.move(x + S(80), fy, S(70), rh); lSim2.move(x + S(158), fy, S(220), rh);
            files.widths = {S(260), S(90)};
            int bw = S(156);
            files.move(x, fy + rh + S(10), right - S(14) - x - bw - S(10), r.bottom - S(70) - (fy + rh + S(10)) - S(12));
            up.move(right - S(14) - bw, fy + rh + S(10), bw, S(32));
            down.move(right - S(14) - bw, fy + rh + S(48), bw, S(32));
            removeFile.move(right - S(14) - bw, fy + rh + S(86), bw, S(32));
        }
        // ---- download limits
        showSet({&limOn, &lNoMore, &limMB, &lMBytes, &lEvery, &limHours, &lHours, &limWarn, &limRecheck, &limNote, &condCharging, &lBattery, &battery}, lim);
        if (lim) {
            int ly2 = panelTop + S(14);
            limOn.move(x, ly2, S(300), S(24)); ly2 += S(36);
            lNoMore.move(x + S(24), ly2, S(170), rh); limMB.move(x + S(196), ly2, S(70), rh); lMBytes.move(x + S(272), ly2, S(60), rh);
            lEvery.move(x + S(336), ly2, S(44), rh); limHours.move(x + S(382), ly2, S(50), rh); lHours.move(x + S(438), ly2, S(50), rh); ly2 += rh + S(12);
            limWarn.move(x + S(24), ly2, S(340), S(24)); ly2 += S(30);
            limRecheck.move(x + S(24), ly2, S(340), S(24)); ly2 += S(34);
            limNote.move(x + S(24), ly2, right - S(14) - x - S(24), S(48)); ly2 += S(70);
            sepY[0] = ly2 - S(10);
            condCharging.move(x, ly2, S(400), S(24)); ly2 += S(34);
            lBattery.move(x, ly2, S(290), rh); battery.move(x + S(294), ly2, S(100), rh);
        }
        redraw();
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        int pad = S(16), leftW = S(250);
        RECT ic{pad + S(6), S(16), pad + S(6) + S(40), S(56)};
        icon(dc, "clock", ic, P.accent, 1.6f);
        RECT qt{pad, S(30), pad + leftW, S(60)};
        text(dc, "Queues", qt, P.text, fontBold(), DT_CENTER);
        int x0 = pad + leftW + S(26), right = r.right - pad;
        RECT vl{x0 - S(14), S(16), x0 - S(13), r.bottom - S(16)};
        fill(dc, vl, P.line);
        RECT tt{x0, S(20), right, S(48)};
        text(dc, title, tt, P.text, fontBig(), DT_CENTER);
        bool lim = cur == "limits";
        int panelTop = S(80) + (lim ? 0 : S(32));
        if (!lim) {
            const char* names[] = {"Schedule", "Files in the queue"};
            int x = x0;
            for (int i = 0; i < 2; i++) {
                SIZE sz = textSize(dc, names[i], i == tab ? fontBold() : fontUI());
                RECT trc{x, S(80), x + sz.cx + S(26), panelTop + 1};
                if (i == tab) { fillRound(dc, RECT{trc.left, trc.top, trc.right, trc.bottom + S(6)}, Sf(6), P.line); fillRound(dc, RECT{trc.left + 1, trc.top + 1, trc.right - 1, trc.bottom + S(6)}, Sf(6), P.tile); }
                text(dc, names[i], trc, i == tab ? P.text : P.muted, i == tab ? fontBold() : fontUI(), DT_CENTER);
                tabRects[i] = trc;
                x = trc.right + S(2);
            }
        }
        RECT panel{x0, panelTop, right, r.bottom - S(66)};
        fillRound(dc, panel, Sf(6), P.tile);
        strokeRound(dc, panel, Sf(6), P.line);
        if (!lim && tab == 0) for (int i = 0; i < 4; i++) { RECT ln{x0 + S(14), sepY[i], right - S(14), sepY[i] + 1}; fill(dc, ln, P.line); }
        if (lim) { RECT ln{x0 + S(14), sepY[0], right - S(14), sepY[0] + 1}; fill(dc, ln, P.line); }
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        if (m == WM_DDM_BG) {
            // controls inside the panel sit on the tile colour
            HWND child = reinterpret_cast<HWND>(w);
            if (child && child != qlist.hwnd()) {
                RECT cr; GetWindowRect(child, &cr);
                POINT pt{cr.left, cr.top}; ScreenToClient(h, &pt);
                int x0 = S(16) + S(250) + S(26);
                if (pt.x >= x0 && pt.y >= S(80)) return bgReply(P.tile);
            }
            return bgReply(P.win);
        }
        if (m == WM_LBUTTONDOWN && cur != "limits") {
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            for (int i = 0; i < 2; i++) if (PtInRect(&tabRects[i], pt)) { tab = i; if (i == 1) fillFiles(); layout(); }
            return 0;
        }
        if (m == WM_TIMER) {
            // keep running / stopped states and the files list fresh
            bool selRunning = false;
            for (auto& it : qlist.items) if (it.key != "limits") { bool r = Engine::get().queueRunning(it.key); if (r != it.bold) { it.bold = r; qlist.redraw(); } }
            (void)selRunning;
            if (tab == 1 && cur != "limits") fillFiles();
            enableAll();
            return 0;
        }
        return Dialog::proc(m, w, l);
    }
};

} // namespace

void showScheduler(HWND owner) {
    static bool open = false;
    if (open) return;
    open = true;
    SchedulerWnd d;
    if (d.createDialog(owner, "Scheduler", S(880), S(740), true)) {
        d.build();
        d.runModal();
    }
    open = false;
}

} // namespace ui
} // namespace ddm
