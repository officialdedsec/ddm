// DDM Help: a classic Windows help viewer built into DDM.exe (no web pages). Toolbar (Hide, Back, Forward, Home, Print,
// Options), Contents / Index / Search on the left, and illustrated topics on the right. The text and the pictures are
// resources inside the exe (HELP text + PNG screenshots).
//
// Topic source format (res/help.txt):
//   @book Title                 starts a book in Contents (topics below belong to it until the next @book / @endbook)
//   @topic id | Title | key words, comma separated
//   = Heading                   == Smaller heading
//   plain lines form paragraphs; **bold**, [[topic-id|link text]], `Key` (a keyboard key / button name)
//   - bullet       1. numbered
//   ! picture | caption | right|center|left
//   > tip / note box
//   | Key or term | description         (a two-column table row)
//   ---                                 (a separator line)
#include "dialogs.hpp"
#include "app.hpp"
#include "gfx.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "../common/util.hpp"
#include "../core/settings.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>

namespace ddm {
namespace ui {

namespace {

// ================================================================ the document
struct Run { std::string text; int style = 0; std::string target; };   // style: 0 normal, 1 bold, 2 link, 3 key
enum class BT { H1, H2, P, Bullet, Num, Image, Tip, Row, Rule };
struct Block { BT type = BT::P; std::vector<Run> runs, runs2; std::string image, caption, align; int number = 0; };
struct Topic { std::string id, title, book; std::vector<std::string> keys; std::vector<Block> blocks; std::string plain; };
struct Book { std::string title; std::vector<int> topics; bool open = true; };

std::vector<Topic> g_topics;
std::vector<Book> g_books;          // top-level entries: a book, or a single topic when title is empty
bool g_loaded = false;

std::vector<Run> parseRuns(const std::string& s) {
    std::vector<Run> out;
    std::string cur;
    auto flush = [&](int style = 0) { if (!cur.empty()) { out.push_back({cur, style, ""}); cur.clear(); } };
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 2, "**") == 0) {
            size_t e = s.find("**", i + 2);
            if (e != std::string::npos) { flush(); out.push_back({s.substr(i + 2, e - i - 2), 1, ""}); i = e + 2; continue; }
        }
        if (s.compare(i, 2, "[[") == 0) {
            size_t e = s.find("]]", i + 2);
            if (e != std::string::npos) {
                flush();
                std::string in = s.substr(i + 2, e - i - 2);
                size_t bar = in.find('|');
                out.push_back({bar == std::string::npos ? in : in.substr(bar + 1), 2, bar == std::string::npos ? in : in.substr(0, bar)});
                i = e + 2;
                continue;
            }
        }
        if (s[i] == '`') {
            size_t e = s.find('`', i + 1);
            if (e != std::string::npos) { flush(); out.push_back({s.substr(i + 1, e - i - 1), 3, ""}); i = e + 1; continue; }
        }
        cur += s[i++];
    }
    flush();
    return out;
}

std::string plainOf(const std::vector<Run>& r) { std::string s; for (auto& x : r) s += x.text; return s; }

void load() {
    if (g_loaded) return;
    g_loaded = true;
    std::string text;
    HRSRC r = FindResourceW(nullptr, L"DDMHELP", RT_RCDATA);
    if (r) { HGLOBAL g = LoadResource(nullptr, r); auto p = static_cast<const char*>(LockResource(g)); if (p) text.assign(p, SizeofResource(nullptr, r)); }
    std::string book;
    int bookIdx = -1;
    Topic* t = nullptr;
    std::string para;
    int num = 0;
    auto endPara = [&] { if (t && !trim(para).empty()) { Block b; b.type = BT::P; b.runs = parseRuns(trim(para)); t->blocks.push_back(b); } para.clear(); };
    for (auto line : split(replaceAll(text, "\r", ""), '\n')) {
        std::string l = line;
        if (startsWith(l, "@book ")) { endPara(); book = trim(l.substr(6)); g_books.push_back({book, {}, true}); bookIdx = static_cast<int>(g_books.size()) - 1; continue; }
        if (startsWith(l, "@endbook")) { endPara(); book.clear(); bookIdx = -1; continue; }
        if (startsWith(l, "@topic ")) {
            endPara();
            auto parts = split(l.substr(7), '|');
            Topic nt;
            nt.id = trim(parts[0]);
            nt.title = parts.size() > 1 ? trim(parts[1]) : nt.id;
            if (parts.size() > 2) for (auto k : split(parts[2], ',')) if (!trim(k).empty()) nt.keys.push_back(trim(k));
            nt.book = book;
            g_topics.push_back(nt);
            t = &g_topics.back();
            int idx = static_cast<int>(g_topics.size()) - 1;
            if (bookIdx >= 0) g_books[static_cast<size_t>(bookIdx)].topics.push_back(idx);
            else g_books.push_back({"", {idx}, false});
            num = 0;
            continue;
        }
        if (!t) continue;
        std::string tl = trim(l);
        if (tl.empty()) { endPara(); num = 0; continue; }
        Block b;
        if (startsWith(tl, "== ")) { endPara(); b.type = BT::H2; b.runs = parseRuns(tl.substr(3)); }
        else if (startsWith(tl, "= ")) { endPara(); b.type = BT::H1; b.runs = parseRuns(tl.substr(2)); }
        else if (startsWith(tl, "- ")) { endPara(); b.type = BT::Bullet; b.runs = parseRuns(tl.substr(2)); }
        else if (tl.size() > 2 && isdigit(static_cast<unsigned char>(tl[0])) && tl.find(". ") != std::string::npos && tl.find(". ") <= 2) { endPara(); b.type = BT::Num; b.number = ++num; b.runs = parseRuns(tl.substr(tl.find(". ") + 2)); }
        else if (startsWith(tl, "! ")) { endPara(); auto p = split(tl.substr(2), '|'); b.type = BT::Image; b.image = trim(p[0]); b.caption = p.size() > 1 ? trim(p[1]) : ""; b.align = p.size() > 2 ? trim(p[2]) : "center"; }
        else if (startsWith(tl, "> ")) { endPara(); b.type = BT::Tip; b.runs = parseRuns(tl.substr(2)); }
        else if (startsWith(tl, "| ")) { endPara(); auto p = split(tl.substr(2), '|'); b.type = BT::Row; b.runs = parseRuns(trim(p[0])); b.runs2 = parseRuns(p.size() > 1 ? trim(p[1]) : ""); }
        else if (tl == "---") { endPara(); b.type = BT::Rule; }
        else { para += (para.empty() ? "" : " ") + tl; continue; }
        t->blocks.push_back(b);
    }
    endPara();
    for (auto& tp : g_topics) {
        std::string p = tp.title + " ";
        for (auto& b : tp.blocks) p += plainOf(b.runs) + " " + plainOf(b.runs2) + " " + b.caption + " ";
        for (auto& k : tp.keys) p += k + " ";
        tp.plain = lower(p);
    }
}

int topicIndex(const std::string& id) {
    for (size_t i = 0; i < g_topics.size(); i++) if (g_topics[i].id == id) return static_cast<int>(i);
    return -1;
}

// ================================================================ layout and painting of a topic
struct Fonts {
    HFONT body{}, bold{}, h1{}, h2{}, title{}, small{}, key{};
    void make(int px100) {   // px100: pixels for a 100% (13 px) body font
        auto mk = [](int px, int w, const wchar_t* face) { return CreateFontW(-px, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face); };
        body = mk(px100 * 15 / 13, FW_NORMAL, L"Segoe UI");
        bold = mk(px100 * 15 / 13, FW_SEMIBOLD, L"Segoe UI");
        h1 = mk(px100 * 20 / 13, FW_SEMIBOLD, L"Segoe UI");
        h2 = mk(px100 * 17 / 13, FW_SEMIBOLD, L"Segoe UI");
        title = mk(px100 * 26 / 13, FW_NORMAL, L"Segoe UI Light");
        small = mk(px100 * 13 / 13, FW_NORMAL, L"Segoe UI");
        key = mk(px100 * 13 / 13, FW_SEMIBOLD, L"Segoe UI");
    }
    void free() { for (HFONT f : {body, bold, h1, h2, title, small, key}) if (f) DeleteObject(f); *this = Fonts(); }
};

struct Piece {   // something placed on the page
    enum Kind { Text, Image, Box, Line, Bullet, Arrow } kind = Text;
    RECT r{};
    std::string text, target, image;
    int style = 0;
    HFONT font{};
    COLORREF color = 0;
    bool highlight = false;
};

struct Page {
    std::vector<Piece> pieces;
    int height = 0;
};

std::vector<std::string> g_highlight;   // search words to mark in the topic

// Lays out a topic at width w (pixels). k = scale (1.0 = screen 100%).
Page layoutTopic(HDC dc, const Topic& t, int w, const Fonts& F, float k) {
    Page pg;
    auto px = [k](int v) { return static_cast<int>(v * k + 0.5f); };
    int margin = px(22), y = px(18);
    int left = margin, right = w - margin;
    // floating picture on the right: lines next to it are shorter until its bottom
    int floatBottom = -1, floatLeft = right;
    auto lineRight = [&](int yy) { return yy < floatBottom ? floatLeft - px(16) : right; };

    // title with arrows (like IDM's help pages)
    {
        HGDIOBJ o = SelectObject(dc, F.title);
        SIZE ts{};
        std::wstring wt = W(t.title);
        GetTextExtentPoint32W(dc, wt.c_str(), static_cast<int>(wt.size()), &ts);
        SelectObject(dc, o);
        int aw = px(22), total = ts.cx + 2 * (aw + px(12));
        int x = std::max(left, (w - total) / 2);
        Piece a1; a1.kind = Piece::Arrow; a1.r = RECT{x, y + (ts.cy - aw) / 2, x + aw, y + (ts.cy - aw) / 2 + aw}; pg.pieces.push_back(a1);
        Piece tt; tt.kind = Piece::Text; tt.text = t.title; tt.font = F.title; tt.color = P.text; tt.r = RECT{x + aw + px(12), y, x + aw + px(12) + ts.cx, y + ts.cy}; pg.pieces.push_back(tt);
        Piece a2 = a1; a2.r.left = tt.r.right + px(12); a2.r.right = a2.r.left + aw; pg.pieces.push_back(a2);
        y += ts.cy + px(22);
    }

    auto words = [](const std::string& s) {   // split keeping spaces attached to words
        std::vector<std::string> v;
        std::string cur;
        for (char c : s) { cur += c; if (c == ' ') { v.push_back(cur); cur.clear(); } }
        if (!cur.empty()) v.push_back(cur);
        return v;
    };
    auto isHit = [](const std::string& word) {
        if (g_highlight.empty()) return false;
        std::string lw = lower(trim(word));
        for (auto& h : g_highlight) if (!h.empty() && lw.find(h) != std::string::npos) return true;
        return false;
    };

    // flows runs into lines from x0, returns the bottom
    auto flow = [&](const std::vector<Run>& runs, int x0, int y0, HFONT base, HFONT boldF, COLORREF color, int lineGap, int maxRight = 0) {
        int x = x0, yy = y0;
        TEXTMETRICW tm;
        HGDIOBJ o = SelectObject(dc, base);
        GetTextMetricsW(dc, &tm);
        int lh = tm.tmHeight + lineGap;
        for (auto& run : runs) {
            HFONT f = run.style == 1 ? boldF : run.style == 3 ? F.key : base;
            SelectObject(dc, f);
            for (auto& wd : words(run.text)) {
                std::wstring ww = W(wd);
                SIZE sz{};
                GetTextExtentPoint32W(dc, ww.c_str(), static_cast<int>(ww.size()), &sz);
                int extra = run.style == 3 ? px(10) : 0;
                int lim = maxRight ? std::min(maxRight, lineRight(yy)) : lineRight(yy);
                if (x > x0 && x + sz.cx + extra > lim) { x = x0; yy += lh; }
                Piece p;
                p.kind = Piece::Text;
                p.text = wd;
                p.font = f;
                p.style = run.style;
                p.target = run.target;
                p.color = run.style == 2 ? P.accent : color;
                p.highlight = isHit(wd);
                p.r = RECT{x + extra / 2, yy, x + extra / 2 + sz.cx, yy + tm.tmHeight};
                if (run.style == 3) { Piece b; b.kind = Piece::Box; b.r = RECT{x, yy - px(1), x + sz.cx + extra - px(3), yy + tm.tmHeight + px(1)}; pg.pieces.push_back(b); }
                pg.pieces.push_back(p);
                x += sz.cx + extra;
            }
        }
        SelectObject(dc, o);
        return yy + lh;
    };

    for (auto& b : t.blocks) {
        if (floatBottom >= 0 && y >= floatBottom) floatBottom = -1;
        switch (b.type) {
            case BT::H1: if (floatBottom > y && b.type == BT::H1) { y = floatBottom; floatBottom = -1; } y += px(6); y = flow(b.runs, left, y, F.h1, F.h1, P.text, px(2)) + px(6); break;
            case BT::H2: y += px(4); y = flow(b.runs, left, y, F.h2, F.h2, P.text, px(2)) + px(4); break;
            case BT::P: y = flow(b.runs, left, y, F.body, F.bold, P.text, px(5)) + px(12); break;
            case BT::Bullet: case BT::Num: {
                Piece m;
                m.kind = b.type == BT::Bullet ? Piece::Bullet : Piece::Text;
                m.font = F.bold;
                m.color = P.accent;
                m.text = b.type == BT::Num ? std::to_string(b.number) + "." : "";
                m.r = RECT{left + px(4), y, left + px(26), y + px(20)};
                pg.pieces.push_back(m);
                y = flow(b.runs, left + px(28), y, F.body, F.bold, P.text, px(5)) + px(6);
                break;
            }
            case BT::Tip: {
                int x0 = left + px(16), top = y;
                Page sub;
                size_t before = pg.pieces.size();
                y = flow(b.runs, x0 + px(28), y + px(12), F.body, F.bold, P.text, px(5));
                Piece box; box.kind = Piece::Box; box.style = 1; box.r = RECT{left, top, lineRight(top), y + px(10)};
                pg.pieces.insert(pg.pieces.begin() + static_cast<long>(before), box);
                Piece ic; ic.kind = Piece::Image; ic.image = "#info"; ic.r = RECT{x0, top + px(12), x0 + px(18), top + px(30)};
                pg.pieces.push_back(ic);
                y += px(22);
                (void)sub;
                break;
            }
            case BT::Row: {
                if (floatBottom > y) { y = floatBottom; floatBottom = -1; }
                int colW = std::min(px(240), (right - left) / 3);
                int y1 = flow(b.runs, left + px(10), y + px(6), F.bold, F.bold, P.text, px(4), left + colW - px(8));
                int y2 = flow(b.runs2, left + colW + px(10), y + px(6), F.body, F.bold, P.text, px(4));
                int yb = std::max(y1, y2) + px(4);
                Piece ln; ln.kind = Piece::Line; ln.r = RECT{left, yb, right, yb + 1}; pg.pieces.push_back(ln);
                y = yb + px(1);
                break;
            }
            case BT::Rule: { if (floatBottom > y) { y = floatBottom; floatBottom = -1; } Piece ln; ln.kind = Piece::Line; ln.r = RECT{left, y + px(6), right, y + px(7)}; pg.pieces.push_back(ln); y += px(18); break; }
            case BT::Image: {
                SIZE is = resImageSize("HLP_" + b.image);
                if (!is.cx) break;
                // pictures are stored at 2x for sharpness: shown at half size, never wider than the page
                int iw = static_cast<int>(is.cx / 2 * k), ih = static_cast<int>(is.cy / 2 * k);
                int maxW = b.align == "right" ? (right - left) * 45 / 100 : right - left;
                if (iw > maxW) { ih = ih * maxW / iw; iw = maxW; }
                if (floatBottom > y) { y = floatBottom + px(8); floatBottom = -1; }
                int x = b.align == "right" ? right - iw : b.align == "left" ? left : left + (right - left - iw) / 2;
                Piece frame; frame.kind = Piece::Box; frame.style = 2; frame.r = RECT{x - px(1), y - px(1), x + iw + px(1), y + ih + px(1)}; pg.pieces.push_back(frame);
                Piece im; im.kind = Piece::Image; im.image = b.image; im.r = RECT{x, y, x + iw, y + ih}; pg.pieces.push_back(im);
                int bottom = y + ih;
                if (!b.caption.empty()) {
                    Piece c; c.kind = Piece::Text; c.text = b.caption; c.font = F.small; c.color = P.muted; c.style = 9;
                    c.r = RECT{x, bottom + px(6), x + iw, bottom + px(26)};
                    pg.pieces.push_back(c);
                    bottom += px(28);
                }
                if (b.align == "right") { floatBottom = bottom + px(8); floatLeft = x; }
                else y = bottom + px(16);
                break;
            }
        }
    }
    if (floatBottom > y) y = floatBottom;
    pg.height = y + px(30);
    return pg;
}

void paintPage(HDC dc, const Page& pg, int scrollY, int width, bool screen) {
    for (auto& p : pg.pieces) {
        RECT r = p.r;
        OffsetRect(&r, 0, -scrollY);
        if (screen && (r.bottom < -50 || r.top > 4000)) continue;
        switch (p.kind) {
            case Piece::Text: {
                if (p.highlight) { RECT hr = r; InflateRect(&hr, 1, 0); fill(dc, hr, isDark() ? rgb(0x6B5A00) : rgb(0xFFF1A8)); }
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, p.color);
                HGDIOBJ o = SelectObject(dc, p.font);
                std::wstring w = W(p.text);
                if (p.style == 9) DrawTextW(dc, w.c_str(), -1, &r, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                else TextOutW(dc, r.left, r.top, w.c_str(), static_cast<int>(w.size()));
                if (p.style == 2) { RECT u{r.left, r.bottom - 1, r.right - (p.text.back() == ' ' ? 4 : 0), r.bottom}; fill(dc, u, p.color); }
                SelectObject(dc, o);
                break;
            }
            case Piece::Image:
                if (p.image == "#info") icon(dc, "info", r, P.accent, 1.8f);
                else drawResImage(dc, r, "HLP_" + p.image);
                break;
            case Piece::Box:
                if (p.style == 1) { fillRound(dc, r, 6.0f, isDark() ? blend(P.accent, P.win, 0.85f) : blend(P.accent, P.win, 0.92f)); RECT bar{r.left, r.top, r.left + 4, r.bottom}; fill(dc, bar, P.accent); }
                else if (p.style == 2) { fill(dc, r, P.line); }
                else { fillRound(dc, r, 4.0f, P.tile); strokeRound(dc, r, 4.0f, P.line); }
                break;
            case Piece::Line: fill(dc, r, P.line); break;
            case Piece::Bullet: { float cx = r.left + 6.0f, cy = r.top + 11.0f; fillCircle(dc, cx, cy, 3.2f, P.accent); break; }
            case Piece::Arrow: {
                POINT pts[3] = {{r.left, r.top}, {r.right, (r.top + r.bottom) / 2}, {r.left, r.bottom}};
                HBRUSH b = CreateSolidBrush(P.good);
                HPEN pen = CreatePen(PS_SOLID, 1, blend(P.good, RGB(0, 0, 0), 0.25f));
                HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
                Polygon(dc, pts, 3);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(b); DeleteObject(pen);
                break;
            }
        }
    }
    (void)width;
}

// ================================================================ left pane lists
struct NavRow { std::string text; int topic = -1; int book = -1; int level = 0; };

class HelpWnd : public Dialog {
public:
    // toolbar
    struct Tool { std::string label, icon; RECT r{}; bool enabled = true; };
    std::vector<Tool> tools;
    int hotTool = -1;
    bool hidePane = false;
    int paneTab = 0;   // 0 Contents, 1 Index, 2 Search
    RECT paneTabs[3]{};
    // left
    std::vector<NavRow> rows;
    int navTop = 0, navSel = -1, navHot = -1;
    Field indexField, searchField;
    Button listTopics, display;
    std::vector<std::pair<std::string, int>> indexItems;   // keyword -> topic
    std::vector<int> results;
    // right
    int current = -1;
    std::vector<int> back, fwd;
    Page page;
    int scrollY = 0;
    Fonts fonts;
    bool highlightOn = true;
    int paneW = 0;

    void build(const std::string& start) {
        load();
        fonts.make(S(13));
        tools = {{"Hide", "M4 4h16v16H4zM9 4v16"}, {"Back", "M15 6l-6 6 6 6"}, {"Forward", "M9 6l6 6-6 6"}, {"Home", "M3 11l9-7 9 7M5 10v10h14V10"}, {"Print", "M7 8V3h10v5M6 17H4v-7h16v7h-2M7 14h10v7H7z"}, {"Options", "M4 6h16M4 12h16M4 18h16"}};
        indexField.create(h, "");
        searchField.create(h, "");
        listTopics.create(h, "List Topics", BtnStyle::Normal, "search");
        display.create(h, "Display", BtnStyle::Primary);
        indexField.onChange = [this] { fillIndex(); };
        indexField.onEnter = [this] { openNavSel(); };
        searchField.onEnter = [this] { doSearch(); };
        listTopics.onClick = [this] { doSearch(); };
        display.onClick = [this] { openNavSel(); };
        onEnter = [this] {
            HWND f = GetFocus();
            if (f == searchField.edit()) doSearch();
            else if (f == indexField.edit() || paneTab != 0) openNavSel();
        };
        // the index: every key word of every topic, plus every title
        std::set<std::pair<std::string, int>> idx;
        for (size_t i = 0; i < g_topics.size(); i++) {
            idx.insert({g_topics[i].title, static_cast<int>(i)});
            for (auto& k : g_topics[i].keys) idx.insert({k, static_cast<int>(i)});
        }
        std::map<std::string, int> seen;
        for (auto& e : idx) seen[lower(e.first)]++;
        std::vector<std::pair<std::string, int>> v;
        for (auto& e : idx) v.push_back({seen[lower(e.first)] > 1 ? e.first + " — " + g_topics[static_cast<size_t>(e.second)].title : e.first, e.second});
        std::sort(v.begin(), v.end(), [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) { return lower(a.first) < lower(b.first); });
        indexItems = v;
        setPaneTab(0);
        int t = topicIndex(start);
        go(t >= 0 ? t : 0, true);
    }

    void setPaneTab(int i) {
        paneTab = i;
        rows.clear();
        navTop = 0; navSel = -1;
        if (i == 0) fillContents();
        else if (i == 1) fillIndex();
        else { rows.clear(); for (int r : results) rows.push_back({g_topics[static_cast<size_t>(r)].title, r, -1, 0}); }
        layout();
        redraw();
    }

    void fillContents() {
        rows.clear();
        for (size_t b = 0; b < g_books.size(); b++) {
            auto& bk = g_books[b];
            if (bk.title.empty()) { for (int t : bk.topics) rows.push_back({g_topics[static_cast<size_t>(t)].title, t, -1, 0}); continue; }
            rows.push_back({bk.title, -1, static_cast<int>(b), 0});
            if (bk.open) for (int t : bk.topics) rows.push_back({g_topics[static_cast<size_t>(t)].title, t, -1, 1});
        }
        for (size_t i = 0; i < rows.size(); i++) if (rows[i].topic == current && current >= 0) navSel = static_cast<int>(i);
    }

    void fillIndex() {
        std::string f = lower(trim(indexField.get()));
        rows.clear();
        int first = -1;
        for (auto& it : indexItems) {
            rows.push_back({it.first, it.second, -1, 0});
            if (first < 0 && !f.empty() && startsWith(lower(it.first), f)) first = static_cast<int>(rows.size()) - 1;
        }
        if (first >= 0) { navSel = first; navTop = std::max(0, first - 3); }
        redraw();
    }

    void doSearch() {
        std::string q = lower(trim(searchField.get()));
        results.clear();
        g_highlight.clear();
        if (q.empty()) { setPaneTab(2); return; }
        std::vector<std::string> ws;
        for (auto w : split(q, ' ')) if (!trim(w).empty()) ws.push_back(trim(w));
        std::vector<std::pair<int, int>> scored;
        for (size_t i = 0; i < g_topics.size(); i++) {
            int score = 0;
            bool all = true;
            for (auto& w : ws) {
                size_t p = 0;
                int c = 0;
                while ((p = g_topics[i].plain.find(w, p)) != std::string::npos) { c++; p += w.size(); }
                if (!c) { all = false; break; }
                score += c + (lower(g_topics[i].title).find(w) != std::string::npos ? 20 : 0);
            }
            if (all) scored.push_back({-score, static_cast<int>(i)});
        }
        std::sort(scored.begin(), scored.end());
        for (auto& s : scored) results.push_back(s.second);
        g_highlight = highlightOn ? ws : std::vector<std::string>();
        setPaneTab(2);
        if (!results.empty()) { navSel = 0; go(results[0], true); }
        else redraw();
    }

    void openNavSel() {
        if (navSel < 0 || navSel >= static_cast<int>(rows.size())) return;
        auto& r = rows[static_cast<size_t>(navSel)];
        if (r.book >= 0) { g_books[static_cast<size_t>(r.book)].open = !g_books[static_cast<size_t>(r.book)].open; fillContents(); redraw(); return; }
        if (r.topic >= 0) go(r.topic, true);
    }

    void go(int t, bool record) {
        if (t < 0 || t >= static_cast<int>(g_topics.size())) return;
        if (record && current >= 0 && current != t) { back.push_back(current); fwd.clear(); }
        current = t;
        if (paneTab != 2) g_highlight.clear();
        scrollY = 0;
        relayout();
        if (paneTab == 0) {
            // open its book and select it
            for (auto& bk : g_books) if (std::find(bk.topics.begin(), bk.topics.end(), t) != bk.topics.end() && !bk.title.empty()) bk.open = true;
            fillContents();
        }
        SetWindowTextW(h, W(g_topics[static_cast<size_t>(t)].title + " — " + i18n::tr("DDM Help")).c_str());
        redraw();
    }

    RECT pageRect() {
        RECT r = client();
        int top = S(64);
        paneW = hidePane ? 0 : S(280);
        return RECT{paneW ? paneW + S(6) : 0, top, r.right, r.bottom};
    }

    void relayout() {
        if (current < 0) return;
        RECT pr = pageRect();
        HDC dc = GetDC(h);
        page = layoutTopic(dc, g_topics[static_cast<size_t>(current)], pr.right - pr.left - GetSystemMetrics(SM_CXVSCROLL), fonts, S(100) / 100.0f);
        ReleaseDC(h, dc);
        clampScroll();
    }

    void clampScroll() {
        RECT pr = pageRect();
        int maxY = std::max<int>(0, page.height - (pr.bottom - pr.top));
        scrollY = std::max(0, std::min(scrollY, maxY));
    }

    void print() {
        if (current < 0) return;
        PRINTDLGW pd{};
        pd.lStructSize = sizeof pd;
        pd.hwndOwner = h;
        pd.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION;
        if (!PrintDlgW(&pd) || !pd.hDC) return;
        HDC dc = pd.hDC;
        int pw = GetDeviceCaps(dc, HORZRES), ph = GetDeviceCaps(dc, VERTRES), dpi = GetDeviceCaps(dc, LOGPIXELSY);
        float k = dpi / 96.0f;
        Fonts pf;
        pf.make(static_cast<int>(13 * k));
        applyTheme("light");   // paper is white
        Page pg = layoutTopic(dc, g_topics[static_cast<size_t>(current)], pw, pf, k);
        std::wstring title = W(g_topics[static_cast<size_t>(current)].title);
        DOCINFOW di{sizeof di, title.c_str()};
        if (StartDocW(dc, &di) > 0) {
            for (int y = 0; y < pg.height; y += ph) {
                StartPage(dc);
                paintPage(dc, pg, y, pw, false);
                EndPage(dc);
            }
            EndDoc(dc);
        }
        applyTheme(settings().theme);
        pf.free();
        DeleteDC(dc);
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
    }

    void optionsMenu(POINT pt) {
        std::vector<MenuItem> v = {{hidePane ? "Show tabs" : "Hide tabs", "", 1}, {"Back", "Alt+Left", 2}, {"Forward", "Alt+Right", 3}, {"Home", "", 4}, MenuItem::sep(),
                                   {"Print…", "Ctrl+P", 5}, MenuItem::sep(), {"Search highlight on", "", 6}};
        v[1].disabled = back.empty();
        v[2].disabled = fwd.empty();
        v.back().checked = highlightOn;
        UINT c = popupMenu(h, v, pt);
        if (c) toolAction(static_cast<int>(c) - 1);
    }

    void toolAction(int i) {
        switch (i) {
            case 0: hidePane = !hidePane; tools[0].label = hidePane ? "Show" : "Hide"; layout(); relayout(); redraw(); break;
            case 1: if (!back.empty()) { fwd.push_back(current); int t = back.back(); back.pop_back(); go(t, false); } break;
            case 2: if (!fwd.empty()) { back.push_back(current); int t = fwd.back(); fwd.pop_back(); go(t, false); } break;
            case 3: go(0, true); break;
            case 4: print(); break;
            case 5: { RECT r = tools[5].r; POINT p{r.left, r.bottom}; ClientToScreen(h, &p); optionsMenu(p); break; }
            case 6: highlightOn = !highlightOn; if (!highlightOn) g_highlight.clear(); relayout(); redraw(); break;
        }
    }

    void layout() override {
        RECT r = client();
        int x = S(8);
        HDC dc = GetDC(h);
        for (auto& t : tools) {
            SIZE s = textSize(dc, t.label, fontSmall());
            int w = std::max(S(54), static_cast<int>(s.cx) + S(16));
            t.r = RECT{x, S(4), x + w, S(58)};
            x += w + S(2);
        }
        ReleaseDC(h, dc);
        tools[1].enabled = !back.empty();
        tools[2].enabled = !fwd.empty();
        bool show = !hidePane;
        int pw = S(280);
        indexField.show(show && paneTab == 1);
        searchField.show(show && paneTab == 2);
        listTopics.show(show && paneTab == 2);
        display.show(show && paneTab != 0);
        if (show) {
            indexField.move(S(10), S(104), pw - S(20), S(30));
            searchField.move(S(10), S(104), pw - S(20), S(30));
            listTopics.move(S(10), S(140), pw - S(20), S(32));
            display.move(pw - S(10) - S(110), r.bottom - S(46), S(110), S(34));
        }
        relayout();
    }

    RECT navRect() {
        RECT r = client();
        int top = paneTab == 0 ? S(100) : paneTab == 1 ? S(142) : S(182);
        int bottom = paneTab == 0 ? r.bottom - S(8) : r.bottom - S(56);
        return RECT{S(8), top, S(280) - S(8), bottom};
    }

    void paintBackground(HDC dc, RECT r) override {
        fill(dc, r, P.win);
        // toolbar
        RECT tb{0, 0, r.right, S(62)};
        fill(dc, tb, P.title);
        RECT tl{0, S(62), r.right, S(63)};
        fill(dc, tl, P.line);
        for (size_t i = 0; i < tools.size(); i++) {
            auto& t = tools[i];
            bool en = i == 1 ? !back.empty() : i == 2 ? !fwd.empty() : true;
            if (static_cast<int>(i) == hotTool && en) fillRound(dc, t.r, Sf(6), P.hover);
            RECT ir{(t.r.left + t.r.right) / 2 - S(11), t.r.top + S(6), (t.r.left + t.r.right) / 2 + S(11), t.r.top + S(28)};
            icon(dc, t.icon, ir, en ? P.text : P.muted, 1.7f);
            RECT lr{t.r.left, S(34), t.r.right, S(54)};
            text(dc, t.label, lr, en ? P.text : P.muted, fontSmall(), DT_CENTER);
        }
        if (!hidePane) {
            int pw = S(280);
            RECT pane{0, S(63), pw, r.bottom};
            fill(dc, pane, P.title);
            RECT sp{pw, S(63), pw + S(6), r.bottom};
            fill(dc, sp, P.line);
            const char* names[] = {"Contents", "Index", "Search"};
            int x = S(8);
            for (int i = 0; i < 3; i++) {
                SIZE sz = textSize(dc, names[i], i == paneTab ? fontBold() : fontUI());
                RECT tr{x, S(70), x + sz.cx + S(22), S(98)};
                if (i == paneTab) { fillRound(dc, RECT{tr.left, tr.top, tr.right, tr.bottom + S(6)}, Sf(5), P.line); fillRound(dc, RECT{tr.left + 1, tr.top + 1, tr.right - 1, tr.bottom + S(6)}, Sf(5), P.field); }
                text(dc, names[i], tr, i == paneTab ? P.text : P.muted, i == paneTab ? fontBold() : fontUI(), DT_CENTER);
                paneTabs[i] = tr;
                x = tr.right + S(2);
            }
            if (paneTab == 1) { RECT l{S(10), S(102) - S(2), pw, S(102)}; (void)l; }
            if (paneTab == 2 && results.empty() && !trim(searchField.get()).empty()) { RECT nr{S(14), S(186), pw - S(10), S(210)}; text(dc, "No topics found.", nr, P.muted, fontUI()); }
            if (paneTab == 1) { RECT lr{S(10), S(80) + S(2), pw, S(100)}; (void)lr; }
            // list
            RECT nr = navRect();
            fillRound(dc, nr, Sf(5), P.field);
            strokeRound(dc, nr, Sf(5), P.line);
            HRGN clip = CreateRectRgn(nr.left + 1, nr.top + 1, nr.right - 1, nr.bottom - 1);
            SelectClipRgn(dc, clip);
            int rh = S(26), y = nr.top + S(4);
            for (int i = navTop; i < static_cast<int>(rows.size()) && y < nr.bottom; i++, y += rh) {
                auto& row = rows[static_cast<size_t>(i)];
                RECT rr{nr.left + S(3), y, nr.right - S(3), y + rh};
                if (i == navSel) fillRound(dc, rr, Sf(4), P.sel);
                else if (i == navHot) fillRound(dc, rr, Sf(4), P.hover);
                int x0 = rr.left + S(6) + row.level * S(18);
                RECT ic{x0, y + (rh - S(16)) / 2, x0 + S(16), y + (rh - S(16)) / 2 + S(16)};
                if (row.book >= 0) icon(dc, g_books[static_cast<size_t>(row.book)].open ? "M4 7h6l2 2h8v10H4z" : "M4 5h6l2 2h8v12H4z", ic, P.folder, 1.6f);
                else icon(dc, paneTab == 0 ? "document" : paneTab == 1 ? "M12 5v14M5 12h14" : "search", ic, paneTab == 0 ? P.accent : P.muted, 1.6f);
                RECT tr{x0 + S(22), y, rr.right - S(4), y + rh};
                text(dc, row.text, tr, P.text, row.book >= 0 ? fontBold() : fontUI(), DT_END_ELLIPSIS);
            }
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        }
        // the page
        RECT pr = pageRect();
        RECT pbg = pr;
        fill(dc, pbg, isDark() ? P.win : rgb(0xFFFFFF));
        HRGN clip = CreateRectRgn(pr.left, pr.top, pr.right, pr.bottom);
        SelectClipRgn(dc, clip);
        SetViewportOrgEx(dc, pr.left, pr.top, nullptr);
        paintPage(dc, page, scrollY, pr.right - pr.left, true);
        SetViewportOrgEx(dc, 0, 0, nullptr);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        // scrollbar
        int viewH = pr.bottom - pr.top;
        if (page.height > viewH) {
            int sbw = S(8);
            RECT track{pr.right - sbw - S(4), pr.top + S(4), pr.right - S(4), pr.bottom - S(4)};
            int th = std::max<int>(S(30), (track.bottom - track.top) * viewH / page.height);
            int ty = track.top + (track.bottom - track.top - th) * scrollY / std::max(1, page.height - viewH);
            RECT thumb{track.left, ty, track.right, ty + th};
            fillRound(dc, thumb, Sf(4), blend(P.muted, P.win, 0.4f));
        }
    }

    const Piece* linkAt(POINT pt) {
        RECT pr = pageRect();
        if (!PtInRect(&pr, pt)) return nullptr;
        int x = pt.x - pr.left, y = pt.y - pr.top + scrollY;
        for (auto& p : page.pieces) if (p.kind == Piece::Text && p.style == 2 && x >= p.r.left && x < p.r.right && y >= p.r.top && y < p.r.bottom) return &p;
        return nullptr;
    }

    LRESULT proc(UINT m, WPARAM w, LPARAM l) override {
        switch (m) {
            case WM_SIZE: layout(); redraw(); return 0;
            case WM_GETMINMAXINFO: { auto mm = reinterpret_cast<MINMAXINFO*>(l); mm->ptMinTrackSize = POINT{S(640), S(420)}; return 0; }
            case WM_MOUSEWHEEL: {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                ScreenToClient(h, &pt);
                int d = GET_WHEEL_DELTA_WPARAM(w);
                RECT nr = navRect();
                if (!hidePane && PtInRect(&nr, pt)) { navTop = std::max(0, std::min(static_cast<int>(rows.size()) - 1, navTop - d / 40)); }
                else { scrollY -= d * S(60) / 120; clampScroll(); }
                redraw();
                return 0;
            }
            case WM_MOUSEMOVE: {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                int hot = -1;
                for (size_t i = 0; i < tools.size(); i++) if (PtInRect(&tools[i].r, pt)) hot = static_cast<int>(i);
                int nh = -1;
                RECT nr = navRect();
                if (!hidePane && PtInRect(&nr, pt)) nh = navTop + (pt.y - nr.top - S(4)) / S(26);
                if (hot != hotTool || nh != navHot) { hotTool = hot; navHot = nh; redraw(); }
                TRACKMOUSEEVENT te{sizeof te, TME_LEAVE, h, 0};
                TrackMouseEvent(&te);
                SetCursor(LoadCursorW(nullptr, linkAt(pt) || hot >= 0 ? IDC_HAND : IDC_ARROW));
                return 0;
            }
            case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT && reinterpret_cast<HWND>(w) == h) return TRUE; break;   // (not over the search and index boxes: they show the text cursor)
            case WM_MOUSELEAVE: hotTool = -1; navHot = -1; redraw(); return 0;
            case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
                POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                for (size_t i = 0; i < tools.size(); i++) if (PtInRect(&tools[i].r, pt)) { toolAction(static_cast<int>(i)); return 0; }
                if (!hidePane) for (int i = 0; i < 3; i++) if (PtInRect(&paneTabs[i], pt)) { setPaneTab(i); if (i == 1) indexField.focus(); if (i == 2) searchField.focus(); return 0; }
                RECT nr = navRect();
                if (!hidePane && PtInRect(&nr, pt)) {
                    int i = navTop + (pt.y - nr.top - S(4)) / S(26);
                    if (i >= 0 && i < static_cast<int>(rows.size())) {
                        navSel = i;
                        auto& row = rows[static_cast<size_t>(i)];
                        if (paneTab == 0 || m == WM_LBUTTONDBLCLK || row.book >= 0) openNavSel();
                        redraw();
                    }
                    SetFocus(h);
                    return 0;
                }
                if (auto p = linkAt(pt)) {
                    std::string target = p->target;
                    if (startsWith(target, "http")) App::openUrl(target);
                    else go(topicIndex(target), true);
                }
                SetFocus(h);
                return 0;
            }
            case WM_KEYDOWN: {
                bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (w == VK_DOWN) { scrollY += S(40); clampScroll(); redraw(); return 0; }
                if (w == VK_UP) { scrollY -= S(40); clampScroll(); redraw(); return 0; }
                if (w == VK_NEXT) { RECT pr = pageRect(); scrollY += pr.bottom - pr.top - S(40); clampScroll(); redraw(); return 0; }
                if (w == VK_PRIOR) { RECT pr = pageRect(); scrollY -= pr.bottom - pr.top - S(40); clampScroll(); redraw(); return 0; }
                if (w == VK_HOME) { scrollY = 0; redraw(); return 0; }
                if (w == VK_END) { scrollY = page.height; clampScroll(); redraw(); return 0; }
                if (ctrl && w == 'P') { print(); return 0; }
                if (ctrl && w == 'F') { setPaneTab(2); searchField.focus(); return 0; }
                break;
            }
            case WM_SYSKEYDOWN:
                if (w == VK_LEFT) { toolAction(1); return 0; }
                if (w == VK_RIGHT) { toolAction(2); return 0; }
                break;
            case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
            case WM_DESTROY: fonts.free(); break;
        }
        return Dialog::proc(m, w, l);
    }
};

HWND g_help = nullptr;
HelpWnd* g_helpObj = nullptr;

} // namespace

void showHelpTopic(HWND owner, const std::string& topic) {
    if (g_help && IsWindow(g_help)) {
        if (!topic.empty()) { int t = topicIndex(topic); if (t >= 0) g_helpObj->go(t, true); }
        ShowWindow(g_help, IsIconic(g_help) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_help);
        return;
    }
    auto* d = new HelpWnd();
    if (!d->createDialog(nullptr, "DDM Help", S(1080), S(720), true, true)) { delete d; return; }
    g_help = d->hwnd();
    g_helpObj = d;
    d->build(topic);
    ShowWindow(d->hwnd(), SW_SHOW);
    struct Deleter { static LRESULT CALLBACK sub(HWND hw, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
        if (m == WM_NCDESTROY) { RemoveWindowSubclass(hw, sub, 1); LRESULT res = DefSubclassProc(hw, m, w, l); delete reinterpret_cast<HelpWnd*>(ref); g_help = nullptr; g_helpObj = nullptr; return res; }
        return DefSubclassProc(hw, m, w, l); } };
    SetWindowSubclass(d->hwnd(), Deleter::sub, 1, reinterpret_cast<DWORD_PTR>(d));
    (void)owner;
}

void showHelpWindow(HWND owner) { showHelpTopic(owner, ""); }

} // namespace ui
} // namespace ddm
