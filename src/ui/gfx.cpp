#include "gfx.hpp"
#include "theme.hpp"
#include "../common/util.hpp"
#include "../core/i18n.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <map>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

namespace ddm {
namespace ui {

using namespace Gdiplus;

static ULONG_PTR g_gdiplus = 0;

void gfxInit() {
    GdiplusStartupInput in;
    GdiplusStartup(&g_gdiplus, &in, nullptr);
}

void gfxShutdown() { if (g_gdiplus) GdiplusShutdown(g_gdiplus); }

static Color C(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

// ---------------------------------------------------------------- buffered paint
Paint::Paint(HWND h) : hwnd(h) {
    real = BeginPaint(h, &ps);
    GetClientRect(h, &client);
    int w = std::max(1, static_cast<int>(client.right)), ht = std::max(1, static_cast<int>(client.bottom));
    mem = CreateCompatibleDC(real);
    bmp = CreateCompatibleBitmap(real, w, ht);
    old = static_cast<HBITMAP>(SelectObject(mem, bmp));
    SetBkMode(mem, TRANSPARENT);
}

Paint::~Paint() {
    BitBlt(real, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

static void roundPath(GraphicsPath& p, const RECT& r, float rad) {
    float x = static_cast<float>(r.left), y = static_cast<float>(r.top), w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    float d = std::min(rad * 2, std::min(w, h));
    if (d <= 0.5f) { p.AddRectangle(RectF(x, y, w, h)); return; }
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

void fillRound(HDC dc, const RECT& r, float radius, COLORREF c) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    GraphicsPath p;
    roundPath(p, r, radius);
    SolidBrush b(C(c));
    g.FillPath(&b, &p);
}

// A progress bar: a rounded track, the filled part in a left-to-right gradient with a soft shine on its top half and,
// while it moves, faint diagonal stripes that slide along (phase: the stripes' offset in pixels)
void progressBar(HDC dc, const RECT& r, double frac, COLORREF track, COLORREF from, COLORREF to, float radius, float phase, bool stripes) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    GraphicsPath tp;
    roundPath(tp, r, radius);
    SolidBrush tb(C(track));
    g.FillPath(&tb, &tp);
    frac = std::max(0.0, std::min(1.0, frac));
    if (frac <= 0) return;
    float w = static_cast<float>(r.right - r.left), hgt = static_cast<float>(r.bottom - r.top);
    float fw = std::max(radius * 2.0f, static_cast<float>(w * frac));
    if (fw > w) fw = w;
    RECT fr{r.left, r.top, r.left + static_cast<LONG>(fw + 0.5f), r.bottom};
    GraphicsPath fp;
    roundPath(fp, fr, radius);
    LinearGradientBrush gb(PointF(static_cast<float>(r.left) - 1.0f, 0), PointF(static_cast<float>(r.left) + w + 1.0f, 0), C(from), C(to));
    g.FillPath(&gb, &fp);
    g.SetClip(&fp);
    if (stripes) {   // 45° bands, one every 2 bar-heights
        SolidBrush sb(C(RGB(255, 255, 255), 34));
        float step = hgt * 1.6f, band = hgt * 0.7f;
        float start = static_cast<float>(r.left) - hgt - step + std::fmod(phase, step);
        for (float x = start; x < static_cast<float>(fr.right); x += step) {
            PointF q[4] = {PointF(x, static_cast<float>(r.bottom)), PointF(x + band, static_cast<float>(r.bottom)), PointF(x + band + hgt, static_cast<float>(r.top)), PointF(x + hgt, static_cast<float>(r.top))};
            g.FillPolygon(&sb, q, 4);
        }
    }
    LinearGradientBrush shine(PointF(0, static_cast<float>(r.top) - 1.0f), PointF(0, static_cast<float>(r.top) + hgt * 0.55f + 1.0f), C(RGB(255, 255, 255), 70), C(RGB(255, 255, 255), 0));
    g.FillRectangle(&shine, static_cast<float>(fr.left), static_cast<float>(r.top), static_cast<float>(fr.right - fr.left), hgt * 0.55f);
    g.ResetClip();
}

void strokeRound(HDC dc, const RECT& r, float radius, COLORREF c, float width) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    RECT rr = r;
    rr.right -= 1; rr.bottom -= 1;
    GraphicsPath p;
    roundPath(p, rr, radius);
    Pen pen(C(c), width);
    g.DrawPath(&pen, &p);
}

void arcStroke(HDC dc, float cx, float cy, float r, float startDeg, float sweepDeg, COLORREF c, float width, int alpha) {
    if (sweepDeg == 0 || r <= 0) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(C(c, static_cast<BYTE>(std::max(0, std::min(255, alpha)))), width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawArc(&pen, cx - r, cy - r, 2 * r, 2 * r, startDeg, sweepDeg);
}

void lineAA(HDC dc, float x1, float y1, float x2, float y2, COLORREF c, float width, int alpha) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(C(c, static_cast<BYTE>(std::max(0, std::min(255, alpha)))), width);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, x1, y1, x2, y2);
}

void polyStroke(HDC dc, const float* xy, int points, COLORREF c, float width, int alpha) {
    if (points < 2) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    Pen pen(C(c, static_cast<BYTE>(std::max(0, std::min(255, alpha)))), width);
    pen.SetLineJoin(LineJoinRound);
    std::vector<PointF> pts;
    for (int i = 0; i < points; i++) pts.push_back(PointF(xy[2 * i], xy[2 * i + 1]));
    g.DrawLines(&pen, pts.data(), points);
}

void polyFill(HDC dc, const float* xy, int points, COLORREF c, int alpha) {
    if (points < 3) return;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush b(C(c, static_cast<BYTE>(std::max(0, std::min(255, alpha)))));
    std::vector<PointF> pts;
    for (int i = 0; i < points; i++) pts.push_back(PointF(xy[2 * i], xy[2 * i + 1]));
    g.FillPolygon(&b, pts.data(), points);
}

void fillCircle(HDC dc, float cx, float cy, float rad, COLORREF c) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush b(C(c));
    g.FillEllipse(&b, cx - rad, cy - rad, rad * 2, rad * 2);
}

void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ o = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, o);
    DeleteObject(pen);
}

static void drawText(HDC dc, const std::string& shown, const RECT& r, COLORREF c, HFONT f, UINT flags);

// Arabic, Urdu, Hebrew letters in the text: it reads right to left
static bool hasRtl(const std::wstring& w) {
    for (wchar_t c : w) if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF)) return true;
    return false;
}

void text(HDC dc, const std::string& s, const RECT& r, COLORREF c, HFONT f, UINT flags) {
    drawText(dc, i18n::tr(s), r, c, f, flags);   // in the user's language (unchanged for English and for names, links, numbers)
}

void textName(HDC dc, const std::string& s, const RECT& r, COLORREF c, HFONT f, UINT flags) {
    drawText(dc, i18n::trName(s), r, c, f, flags);   // a file, site, category or title: kept as it is (only DDM's own names change)
}

static void drawText(HDC dc, const std::string& shown, const RECT& r, COLORREF c, HFONT f, UINT flags) {
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    std::wstring w = W(shown);
    RECT rr = r;
    if (!(flags & DT_WORDBREAK)) flags |= DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    else flags |= DT_EDITCONTROL;   // long words (paths, links) break instead of running out of the box
    if (i18n::rtl() && hasRtl(w)) flags |= DT_RTLREADING;   // (a name or number alone keeps its own order: "21.9 MB")
    DrawTextW(dc, w.c_str(), static_cast<int>(w.size()), &rr, flags | DT_NOPREFIX);
    SelectObject(dc, o);
}

static SIZE measure(HDC dc, const std::string& shown, HFONT f);

SIZE textSize(HDC dc, const std::string& s, HFONT f) { return measure(dc, i18n::tr(s), f); }
SIZE textSizeName(HDC dc, const std::string& s, HFONT f) { return measure(dc, i18n::trName(s), f); }

static SIZE measure(HDC dc, const std::string& shown, HFONT f) {
    HGDIOBJ o = SelectObject(dc, f);
    std::wstring w = W(shown);
    SIZE sz{};
    GetTextExtentPoint32W(dc, w.c_str(), static_cast<int>(w.size()), &sz);
    bool complex = false;   // Bangla, Hindi, Arabic…: measured the way they're drawn (shaped, with fallback fonts)
    for (wchar_t c : w) if (c >= 0x0590 && !(c >= 0x2000 && c <= 0x2BFF)) { complex = true; break; }
    if (complex) {
        RECT rc{0, 0, 0, 0};
        DrawTextW(dc, w.c_str(), static_cast<int>(w.size()), &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | (i18n::rtl() && hasRtl(w) ? DT_RTLREADING : 0));
        sz.cx = std::max<LONG>(sz.cx, rc.right - rc.left);
        sz.cy = std::max<LONG>(sz.cy, rc.bottom - rc.top);
    }
    SelectObject(dc, o);
    return sz;
}

// ---------------------------------------------------------------- SVG path → GDI+ path
namespace {
struct PathParser {
    const std::string& d;
    size_t i = 0;
    explicit PathParser(const std::string& s) : d(s) {}
    void skip() { while (i < d.size() && (d[i] == ' ' || d[i] == ',' || d[i] == '\n' || d[i] == '\t')) i++; }
    bool atNumber() { skip(); return i < d.size() && (isdigit(static_cast<unsigned char>(d[i])) || d[i] == '-' || d[i] == '+' || d[i] == '.'); }
    float num() {
        skip();
        size_t st = i;
        if (i < d.size() && (d[i] == '-' || d[i] == '+')) i++;
        bool dot = false;
        while (i < d.size() && (isdigit(static_cast<unsigned char>(d[i])) || (d[i] == '.' && !dot))) { if (d[i] == '.') dot = true; i++; }
        if (i < d.size() && (d[i] == 'e' || d[i] == 'E')) { i++; if (i < d.size() && (d[i] == '-' || d[i] == '+')) i++; while (i < d.size() && isdigit(static_cast<unsigned char>(d[i]))) i++; }
        return static_cast<float>(atof(d.substr(st, i - st).c_str()));
    }
    float flag() { skip(); float v = (i < d.size() && d[i] == '1') ? 1.f : 0.f; if (i < d.size()) i++; return v; }
};

void arcToBeziers(GraphicsPath& p, float x1, float y1, float rx, float ry, float phiDeg, bool large, bool sweep, float x2, float y2) {
    if (rx == 0 || ry == 0) { p.AddLine(x1, y1, x2, y2); return; }
    const float PI = 3.14159265f;
    float phi = phiDeg * PI / 180.f, cp = std::cos(phi), sp = std::sin(phi);
    float dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    float x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    rx = std::fabs(rx); ry = std::fabs(ry);
    float lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
    if (lam > 1) { float s = std::sqrt(lam); rx *= s; ry *= s; }
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    float co = (den == 0) ? 0 : std::sqrt(std::max(0.f, num / den));
    if (large == sweep) co = -co;
    float cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx;
    float cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    auto ang = [](float ux, float uy, float vx, float vy) { float a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); return a; };
    float t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry);
    float dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry);
    if (!sweep && dt > 0) dt -= 2 * PI; else if (sweep && dt < 0) dt += 2 * PI;
    int segs = static_cast<int>(std::ceil(std::fabs(dt) / (PI / 2)));
    float delta = dt / segs, t = t1;
    float px = x1, py = y1;
    for (int k = 0; k < segs; k++) {
        float a = 4.f / 3.f * std::tan(delta / 4);
        float c1 = std::cos(t), s1 = std::sin(t), c2 = std::cos(t + delta), s2 = std::sin(t + delta);
        auto map = [&](float ex, float ey, float& ox, float& oy) { ox = cp * rx * ex - sp * ry * ey + cx; oy = sp * rx * ex + cp * ry * ey + cy; };
        float q1x, q1y, q2x, q2y, ex, ey;
        map(c1 - a * s1, s1 + a * c1, q1x, q1y);
        map(c2 + a * s2, s2 - a * c2, q2x, q2y);
        map(c2, s2, ex, ey);
        p.AddBezier(px, py, q1x, q1y, q2x, q2y, ex, ey);
        px = ex; py = ey; t += delta;
    }
}

void buildPath(GraphicsPath& p, const std::string& d) {
    PathParser ps(d);
    float x = 0, y = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    char cmd = 0, prev = 0;
    bool open = false;
    while (true) {
        ps.skip();
        if (ps.i >= d.size()) break;
        char ch = d[ps.i];
        if (isalpha(static_cast<unsigned char>(ch))) { cmd = ch; ps.i++; }
        else if (!cmd) break;
        bool rel = islower(static_cast<unsigned char>(cmd)) != 0;
        char c = static_cast<char>(toupper(static_cast<unsigned char>(cmd)));
        if (c == 'Z') { if (open) p.CloseFigure(); x = sx; y = sy; open = false; prev = 'Z'; cmd = 0; continue; }
        if (!ps.atNumber()) { cmd = 0; continue; }
        switch (c) {
            case 'M': {
                float nx = ps.num(), ny = ps.num();
                if (rel) { nx += x; ny += y; }
                p.StartFigure();
                x = sx = nx; y = sy = ny; open = true;
                cmd = rel ? 'l' : 'L';   // further pairs are lines
                break;
            }
            case 'L': { float nx = ps.num(), ny = ps.num(); if (rel) { nx += x; ny += y; } p.AddLine(x, y, nx, ny); x = nx; y = ny; break; }
            case 'H': { float nx = ps.num(); if (rel) nx += x; p.AddLine(x, y, nx, y); x = nx; break; }
            case 'V': { float ny = ps.num(); if (rel) ny += y; p.AddLine(x, y, x, ny); y = ny; break; }
            case 'C': {
                float a = ps.num(), b = ps.num(), e = ps.num(), f = ps.num(), g = ps.num(), h = ps.num();
                if (rel) { a += x; b += y; e += x; f += y; g += x; h += y; }
                p.AddBezier(x, y, a, b, e, f, g, h); lcx = e; lcy = f; x = g; y = h; break;
            }
            case 'S': {
                float e = ps.num(), f = ps.num(), g = ps.num(), h = ps.num();
                if (rel) { e += x; f += y; g += x; h += y; }
                float a = x, b = y;
                if (prev == 'C' || prev == 'S') { a = 2 * x - lcx; b = 2 * y - lcy; }
                p.AddBezier(x, y, a, b, e, f, g, h); lcx = e; lcy = f; x = g; y = h; break;
            }
            case 'Q': {
                float a = ps.num(), b = ps.num(), g = ps.num(), h = ps.num();
                if (rel) { a += x; b += y; g += x; h += y; }
                p.AddBezier(x, y, x + 2.f / 3 * (a - x), y + 2.f / 3 * (b - y), g + 2.f / 3 * (a - g), h + 2.f / 3 * (b - h), g, h); x = g; y = h; break;
            }
            case 'A': {
                float rx = ps.num(), ry = ps.num(), rot = ps.num(); bool la = ps.flag() > 0, sw = ps.flag() > 0; float nx = ps.num(), ny = ps.num();
                if (rel) { nx += x; ny += y; }
                arcToBeziers(p, x, y, rx, ry, rot, la, sw, nx, ny); x = nx; y = ny; break;
            }
            default: ps.i++; break;
        }
        prev = c;
    }
}

const std::map<std::string, std::string>& iconTable() {
    static const std::map<std::string, std::string> t = {
        {"add", "M5 3h14a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z M12 8v8 M8 12h8"},
        {"resume", "M8 5l11 7-11 7z"},
        {"pause", "M8 5v14 M16 5v14"},
        {"pauseall", "M4 4h16v16H4z M10 8v8 M14 8v8"},
        {"resumeall", "M4 4h16v16H4z M10 8l6 4-6 4z"},
        {"stop", "M7 5h10a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2z"},
        {"undo", "M9 14L4 9l5-5 M4 9h11a5 5 0 0 1 0 10h-3"},
        {"delete", "M4 7h16 M9 7V4h6v3 M6 7l1 13h10l1-13"},
        {"deleteok", "M4 7h16 M9 7V4h6v3 M6 7l1 13h10l1-13 M9.5 13.5l2 2 3.5-4"},
        {"options", "M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6z M12 2v3 M12 19v3 M2 12h3 M19 12h3 M4.9 4.9L7 7 M17 17l2.1 2.1 M4.9 19.1L7 17 M17 7l2.1-2.1"},
        {"clock", "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z M12 7v5l3 2"},
        {"qstart", "M3 6h11 M3 11h11 M3 16h6 M14 14l7 4-7 4z"},
        {"qstop", "M3 6h11 M3 11h11 M3 16h6 M14 15h6v6h-6z"},
        {"xray", "M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12z M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6z"},
        {"grab", "M3 12h4 M17 12h4 M12 3v4 M12 17v4 M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8z"},
        {"folder", "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"},
        {"magnet", "M6 3v8a6 6 0 0 0 12 0V3 M6 7h4 M14 7h4 M10 3v8a2 2 0 0 0 4 0V3"},
        {"down", "M12 4v11 M7 10.5l5 5 5-5 M5 20h14"},
        {"check", "M4 12.5l5 5L20 7"},
        {"queue", "M4 6h16 M4 12h16 M4 18h10"},
        {"chevron", "M8 10l4 4 4-4"},
        {"chevronr", "M10 8l4 4-4 4"},
        {"chevronup", "M6 15l6-6 6 6"},
        {"file", "M6 2h9l5 5v13a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z M14 2v6h6"},
        {"zipfile", "M6 2h9l5 5v13a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z M14 2v6h6 M10 6h2 M10 9h2 M10 12h2 M9.5 15h3v3h-3z"},
        {"plus", "M12 5v14 M5 12h14"},
        {"close", "M6 6l12 12 M18 6L6 18"},
        {"moon", "M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"},
        {"sun", "M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8z M12 2v2 M12 20v2 M2 12h2 M20 12h2 M4.9 4.9l1.4 1.4 M17.7 17.7l1.4 1.4 M4.9 19.1l1.4-1.4 M17.7 6.3l1.4-1.4"},
        {"wifi", "M5 12.5a10 10 0 0 1 14 0 M8 15.5a6 6 0 0 1 8 0 M12 19h.01"},
        {"refresh", "M20 11a8 8 0 1 0-2.3 5.7 M20 4v7h-7"},
        {"globe", "M12 3a9 9 0 1 0 0.01 0 M3 12h18 M12 3c2.5 2.6 3.8 5.6 3.8 9s-1.3 6.4-3.8 9 M12 3c-2.5 2.6-3.8 5.6-3.8 9s1.3 6.4 3.8 9"},
        {"info", "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z M12 11v6 M12 7.5v.1"},
        {"open", "M14 4h6v6 M10 14L20 4 M18 13v6a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h6"},
        {"link", "M10 14a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1 M14 10a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1"},
        {"video", "M3 6h13v12H3z M16 10l5-3v10l-5-3"},
        {"music", "M9 18V5l11-2v13 M9 18a3 3 0 1 1-6 0 3 3 0 0 1 6 0z M20 16a3 3 0 1 1-6 0 3 3 0 0 1 6 0z"},
        {"program", "M3 4h18v12H3z M8 20h8 M12 16v4"},
        {"document", "M6 2h9l5 5v13a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2z M14 2v6h6 M8 13h8 M8 17h6"},
        {"image", "M3 5h18v14H3z M3 16l5-5 4 4 3-3 6 6 M15.5 9.5a1.5 1.5 0 1 0 0-.1"},
        {"help", "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z M9.5 9a2.5 2.5 0 1 1 3.5 2.3c-.6.3-1 .9-1 1.6v.6 M12 17v.1"},
        {"search", "M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14z M16 16l5 5"},
        {"copy", "M9 9h11v11H9z M5 15H4V4h11v1"},
        {"printer", "M7 8V3h10v5 M6 17H4v-7h16v7h-2 M7 14h10v7H7z"},
    };
    return t;
}
} // namespace

void icon(HDC dc, const std::string& nameOrPath, const RECT& r, COLORREF c, float stroke, bool filled) {
    auto& t = iconTable();
    auto it = t.find(nameOrPath);
    const std::string& d = it != t.end() ? it->second : nameOrPath;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    float s = std::min(w, h) / 24.f;
    g.TranslateTransform(r.left + (w - 24 * s) / 2, r.top + (h - 24 * s) / 2);
    g.ScaleTransform(s, s);
    GraphicsPath p;
    buildPath(p, d);
    if (filled) { SolidBrush b(C(c)); g.FillPath(&b, &p); }
    Pen pen(C(c), stroke);
    pen.SetLineJoin(LineJoinRound);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawPath(&pen, &p);
}

// ---------------------------------------------------------------- favicons and badges
namespace {
std::mutex g_favMu;
std::map<std::string, std::shared_ptr<Bitmap>> g_fav;
}

void setFavicon(const std::string& site, const std::string& bytes) {
    if (bytes.empty()) return;
    IStream* st = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
    if (!st) return;
    auto bmp = std::make_shared<Bitmap>(st);
    st->Release();
    if (bmp->GetLastStatus() != Ok || bmp->GetWidth() == 0) return;
    std::lock_guard<std::mutex> g(g_favMu);
    g_fav[site] = bmp;
}

std::shared_ptr<void> loadImage(const std::string& bytes) {
    if (bytes.empty()) return nullptr;
    IStream* st = SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.data()), static_cast<UINT>(bytes.size()));
    if (!st) return nullptr;
    std::shared_ptr<Bitmap> out;
    {
        Bitmap src(st);
        if (src.GetLastStatus() == Ok && src.GetWidth() > 0) {
            Bitmap* c = src.Clone(0, 0, static_cast<INT>(src.GetWidth()), static_cast<INT>(src.GetHeight()), PixelFormat32bppARGB);   // its own pixels: the stream can go
            if (c && c->GetLastStatus() == Ok) out.reset(c); else delete c;
        }
    }
    st->Release();
    return out;
}

bool compressImage(const std::string& in, const std::string& out, int maxSide, int quality, std::string& error) {
    std::unique_ptr<Bitmap> src(Bitmap::FromFile(W(in).c_str()));
    if (!src || src->GetLastStatus() != Ok || src->GetWidth() == 0) { error = "Windows can't read this picture."; return false; }
    UINT w = src->GetWidth(), h = src->GetHeight();
    double k = std::min(1.0, static_cast<double>(maxSide) / static_cast<double>(std::max(w, h)));
    UINT nw = std::max<UINT>(1, static_cast<UINT>(w * k)), nh = std::max<UINT>(1, static_cast<UINT>(h * k));
    Bitmap dst(static_cast<INT>(nw), static_cast<INT>(nh), PixelFormat32bppARGB);
    {
        Graphics g(&dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.DrawImage(src.get(), 0, 0, static_cast<INT>(nw), static_cast<INT>(nh));
    }
    bool png = lower(extOf(out)) == "png";
    const wchar_t* mime = png ? L"image/png" : L"image/jpeg";
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* enc = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num, size, enc);
    CLSID clsid{};
    bool found = false;
    for (UINT i = 0; i < num; i++) if (wcscmp(enc[i].MimeType, mime) == 0) { clsid = enc[i].Clsid; found = true; }
    if (!found) { error = "Windows can't save this kind of picture."; return false; }
    EncoderParameters ep{};
    ULONG q = static_cast<ULONG>(std::max(1, std::min(100, quality)));
    ep.Count = 1; ep.Parameter[0].Guid = EncoderQuality; ep.Parameter[0].Type = EncoderParameterValueTypeLong; ep.Parameter[0].NumberOfValues = 1; ep.Parameter[0].Value = &q;
    if (dst.Save(W(out).c_str(), &clsid, png ? nullptr : &ep) != Ok) { error = "Couldn't save " + baseName(out) + "."; return false; }
    return true;
}

void drawImageCover(HDC dc, const RECT& r, const std::shared_ptr<void>& img, float radius, const std::string& caption, const std::string& caption2) {
    auto* b = static_cast<Bitmap*>(img.get());
    if (!b) return;
    {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float x = static_cast<float>(r.left), y = static_cast<float>(r.top), w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    GraphicsPath path;
    float d = radius * 2;
    path.AddArc(x, y, d, d, 180, 90); path.AddArc(x + w - d, y, d, d, 270, 90); path.AddArc(x + w - d, y + h - d, d, d, 0, 90); path.AddArc(x, y + h - d, d, d, 90, 90); path.CloseFigure();
    g.SetClip(&path);
    float iw = static_cast<float>(b->GetWidth()), ih = static_cast<float>(b->GetHeight());
    float scale = std::max(w / iw, h / ih), sw = w / scale, sh = h / scale;   // crop the middle to fill
    g.DrawImage(b, RectF(x, y, w, h), (iw - sw) / 2, (ih - sh) / 2, sw, sh, UnitPixel);
    if (!caption.empty()) {
        float bh = h * 0.42f;
        LinearGradientBrush shade(PointF(0, y + h - bh - 1), PointF(0, y + h), Color(0, 0, 0, 0), Color(190, 0, 0, 0));
        g.FillRectangle(&shade, x, y + h - bh, w, bh);
    }
    }
    if (!caption.empty()) {
        RECT t1{r.left + S(8), r.bottom - S(46), r.right - S(8), r.bottom - S(22)};
        text(dc, caption, t1, RGB(255, 255, 255), fontBig(), DT_CENTER);
        RECT t2{r.left + S(8), r.bottom - S(22), r.right - S(8), r.bottom - S(4)};
        text(dc, caption2, t2, RGB(232, 232, 236), fontSmall(), DT_CENTER);
    }
}

bool drawFavicon(HDC dc, const RECT& r, const std::string& site) {
    std::shared_ptr<Bitmap> b;
    {
        std::lock_guard<std::mutex> g(g_favMu);
        auto it = g_fav.find(site);
        if (it == g_fav.end()) return false;
        b = it->second;
    }
    Graphics g(dc);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(b.get(), Rect(r.left, r.top, r.right - r.left, r.bottom - r.top));
    return true;
}

// Pictures embedded in DDM.exe (RCDATA, PNG): the help's screenshots.
namespace {
std::map<std::string, std::shared_ptr<Bitmap>> g_res;
std::shared_ptr<Bitmap> resImage(const std::string& name) {
    auto it = g_res.find(name);
    if (it != g_res.end()) return it->second;
    std::shared_ptr<Bitmap> bmp;
    std::wstring wn = W(name);
    HRSRC r = FindResourceW(nullptr, wn.c_str(), RT_RCDATA);
    if (r) {
        HGLOBAL g = LoadResource(nullptr, r);
        const BYTE* p = static_cast<const BYTE*>(LockResource(g));
        DWORD n = SizeofResource(nullptr, r);
        if (p && n) {
            IStream* st = SHCreateMemStream(p, n);
            if (st) { bmp = std::make_shared<Bitmap>(st); st->Release(); if (bmp->GetLastStatus() != Ok) bmp.reset(); }
        }
    }
    g_res[name] = bmp;
    return bmp;
}
} // namespace

SIZE resImageSize(const std::string& name) {
    auto b = resImage(name);
    return b ? SIZE{static_cast<LONG>(b->GetWidth()), static_cast<LONG>(b->GetHeight())} : SIZE{0, 0};
}

bool drawResImage(HDC dc, const RECT& r, const std::string& name) {
    auto b = resImage(name);
    if (!b) return false;
    Graphics g(dc);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(b.get(), Rect(r.left, r.top, r.right - r.left, r.bottom - r.top));
    return true;
}

void siteBadge(HDC dc, const RECT& r, const std::string& site) {
    static const COLORREF colors[] = {rgb(0x1D63D8), rgb(0xC9461C), rgb(0x3B7D4A), rgb(0x6A4BC4), rgb(0xD42020), rgb(0x0E7C86), rgb(0xE0780B), rgb(0x5B6270), rgb(0xB0306A)};
    unsigned h = 0;
    for (char ch : site) h = h * 31 + static_cast<unsigned char>(ch);
    COLORREF c = site.empty() ? rgb(0x5B6270) : colors[h % (sizeof colors / sizeof colors[0])];
    fillRound(dc, r, Sf(4), c);
    std::string letter = site.empty() ? "?" : std::string(1, static_cast<char>(toupper(static_cast<unsigned char>(site[0]))));
    if (startsWith(site, "magnet")) letter = "M";
    text(dc, letter, r, RGB(255, 255, 255), fontSmallBold(), DT_CENTER);
}

void logo(HDC dc, const RECT& r, bool disabled) {
    float rad = (r.right - r.left) * 0.22f;
    fillRound(dc, r, rad, disabled ? RGB(0, 0, 0) : rgb(0x16131C));
    RECT in = r;
    int pad = (r.right - r.left) / 5;
    InflateRect(&in, -pad, -pad);
    if (disabled) icon(dc, "close", in, rgb(0xFF2D2D), 3.4f);
    else icon(dc, "M12 4v11M7 10.5l5 5 5-5M6 20h12", in, rgb(0xC6F432), 3.0f);
}

} // namespace ui
} // namespace ddm

// ================================================================ premium file / folder icons
namespace ddm {
namespace ui {

namespace {
struct TypeLook { COLORREF top, bottom; const char* glyph; bool fillGlyph; };

TypeLook lookFor(const std::string& t) {
    // gradient top → bottom, and a white glyph (24-unit SVG path, drawn inside the sheet)
    if (t == "video") return {rgb(0x9B6BFF), rgb(0x6A3DE8), "M9.5 9.2v6.6l5.6-3.3z", true};
    if (t == "music") return {rgb(0xFF6FA8), rgb(0xE0306F), "M10 16.2a1.7 1.7 0 1 1-1.7-1.7 1.7 1.7 0 0 1 1.7 1.7zM10 16.2V9.4l5-1.2v6.4M15 14.6a1.7 1.7 0 1 1-1.7-1.7 1.7 1.7 0 0 1 1.7 1.7z", false};
    if (t == "programs") return {rgb(0x4FA8FF), rgb(0x1E6FD9), "M8.2 9.2h7.6v6.6H8.2zM8.2 11.2h7.6", false};
    if (t == "compressed") return {rgb(0xFFC23D), rgb(0xE8930C), "M12 5.5v1.4M12 8.1v1.4M12 10.7v1.4M10.8 13.2h2.4v2.6h-2.4z", false};
    if (t == "documents") return {rgb(0x5B8DEF), rgb(0x2F5FD0), "M8.6 10h6.8M8.6 12.6h6.8M8.6 15.2h4.4", false};
    if (t == "pdf") return {rgb(0xFF6B5B), rgb(0xD63A2B), "M8.6 10h6.8M8.6 12.6h6.8M8.6 15.2h4.4", false};
    if (t == "image") return {rgb(0x3CCB9A), rgb(0x12A06E), "M8.2 15.8l2.6-3.2 1.9 2.2 1.4-1.6 1.9 2.6zM14.1 9.8a1 1 0 1 1-.01 0z", true};
    if (t == "torrent") return {rgb(0x4CD07A), rgb(0x1F9E4E), "M8.6 8.4v3.8a3.4 3.4 0 0 0 6.8 0V8.4M8.6 8.4h2v3.8M15.4 8.4h-2v3.8", false};
    return {rgb(0xB7C0CC), rgb(0x8793A3), "M8.6 11h6.8M8.6 13.6h6.8", false};
}

void glyph(Graphics& g, const char* d, bool fill, float stroke) {
    GraphicsPath p;
    buildPath(p, d);
    if (fill) { SolidBrush b(Color(255, 255, 255, 255)); g.FillPath(&b, &p); }
    else {
        Pen pen(Color(255, 255, 255, 255), stroke);
        pen.SetLineJoin(LineJoinRound); pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
        g.DrawPath(&pen, &p);
    }
}

void setup(Graphics& g, const RECT& r) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    float s = std::min(w, h) / 24.f;
    g.TranslateTransform(r.left + (w - 24 * s) / 2, r.top + (h - 24 * s) / 2);
    g.ScaleTransform(s, s);
}

void roundRectPath(GraphicsPath& p, float x, float y, float w, float h, float rad) {
    p.AddArc(x, y, rad * 2, rad * 2, 180, 90);
    p.AddArc(x + w - rad * 2, y, rad * 2, rad * 2, 270, 90);
    p.AddArc(x + w - rad * 2, y + h - rad * 2, rad * 2, rad * 2, 0, 90);
    p.AddArc(x, y + h - rad * 2, rad * 2, rad * 2, 90, 90);
    p.CloseFigure();
}
} // namespace

std::string typeOf(const std::string& category, const std::string& fileName, const std::string& kind) {
    if (kind == "torrent") return "torrent";
    std::string e = lower(extOf(fileName));
    if (e == "pdf") return "pdf";
    if (e == "jpg" || e == "jpeg" || e == "png" || e == "gif" || e == "webp" || e == "bmp" || e == "svg" || e == "heic") return "image";
    if (category == "video" || category == "music" || category == "programs" || category == "compressed" || category == "documents") return category;
    return "file";
}

// A sheet of paper with a folded corner, coloured by kind, with a white mark (play, note, zip, …).
void fileIcon(HDC dc, const RECT& r, const std::string& type) {
    Graphics g(dc);
    setup(g, r);
    TypeLook L = lookFor(type);
    GraphicsPath sheet;
    // 4.5..19.5 x 2.5..21.5, fold 5
    sheet.AddLine(6.0f, 2.5f, 14.5f, 2.5f);
    sheet.AddLine(14.5f, 2.5f, 19.5f, 7.5f);
    sheet.AddLine(19.5f, 7.5f, 19.5f, 20.0f);
    sheet.AddArc(16.5f, 18.5f, 3.0f, 3.0f, 0, 90);
    sheet.AddLine(18.0f, 21.5f, 6.0f, 21.5f);
    sheet.AddArc(4.5f, 18.5f, 3.0f, 3.0f, 90, 90);
    sheet.AddLine(4.5f, 20.0f, 4.5f, 4.0f);
    sheet.AddArc(4.5f, 2.5f, 3.0f, 3.0f, 180, 90);
    sheet.CloseFigure();
    LinearGradientBrush grad(PointF(0, 2.5f), PointF(0, 21.5f), C(L.top), C(L.bottom));
    g.FillPath(&grad, &sheet);
    // the fold
    GraphicsPath fold;
    fold.AddLine(14.5f, 2.5f, 14.5f, 6.3f);
    fold.AddArc(14.5f, 5.1f, 2.4f, 2.4f, 180, -90);
    fold.AddLine(15.7f, 7.5f, 19.5f, 7.5f);
    fold.CloseFigure();
    SolidBrush fb(Color(120, 255, 255, 255));
    g.FillPath(&fb, &fold);
    g.TranslateTransform(0, 1.2f);
    glyph(g, L.glyph, L.fillGlyph, 1.5f);
}

// The icon Windows itself shows for this file (its program's icon for an .exe, Acrobat's for a .pdf…). A file that
// doesn't exist yet gets the icon of its type. Cached; false when Windows has none to give.
bool systemFileIcon(HDC dc, const RECT& r, const std::string& nameOrPath, bool exists) {
    static std::map<std::string, HICON> cache;
    int size = std::min(r.right - r.left, r.bottom - r.top);
    if (size <= 0) return false;
    int list = size <= 16 ? 1 /*SHIL_SMALL*/ : size <= 32 ? 0 /*SHIL_LARGE*/ : 2 /*SHIL_EXTRALARGE: 48 px (the 256 px list draws small icons in a corner)*/;
    std::string e = lower(extOf(nameOrPath));
    // per type (a document's icon is its program's), per file for programs and shortcuts (each has its own picture)
    bool own = exists && (e == "exe" || e == "lnk" || e == "ico" || e == "msi" || e == "scr" || e == "cpl");
    std::string key = std::to_string(list) + "|" + (own ? lower(nameOrPath) : (e.empty() ? std::string("*") : e));
    HICON hi = nullptr;
    auto it = cache.find(key);
    if (it != cache.end()) hi = it->second;
    else {
        SHFILEINFOW fi{};
        std::wstring p = own ? W(nameOrPath) : W(std::string("file.") + (e.empty() ? "unknown-type" : e));
        if (SHGetFileInfoW(p.c_str(), FILE_ATTRIBUTE_NORMAL, &fi, sizeof fi, SHGFI_SYSICONINDEX | (own ? 0 : SHGFI_USEFILEATTRIBUTES))) {
            static const GUID iidImageList = {0x46eb5926, 0x582e, 0x4017, {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};
            IImageList* il = nullptr;
            if (SUCCEEDED(SHGetImageList(list, iidImageList, reinterpret_cast<void**>(&il))) && il) {
                il->GetIcon(fi.iIcon, ILD_TRANSPARENT, &hi);
                il->Release();
            }
        }
        if (cache.size() > 300) { for (auto& c : cache) if (c.second) DestroyIcon(c.second); cache.clear(); }
        cache[key] = hi;
    }
    if (!hi) return false;
    int x = (r.left + r.right - size) / 2, y = (r.top + r.bottom - size) / 2;
    return DrawIconEx(dc, x, y, hi, size, size, 0, nullptr, DI_NORMAL) != FALSE;
}

// A Windows 11-style folder; a small coloured mark tells the kind of files inside.
void folderIcon(HDC dc, const RECT& r, const std::string& type) {
    Graphics g(dc);
    setup(g, r);
    GraphicsPath back;
    back.AddArc(2.5f, 4.5f, 3.0f, 3.0f, 180, 90);
    back.AddLine(4.0f, 4.5f, 9.2f, 4.5f);
    back.AddLine(9.2f, 4.5f, 11.2f, 6.5f);
    back.AddLine(11.2f, 6.5f, 20.0f, 6.5f);
    back.AddArc(18.5f, 6.5f, 3.0f, 3.0f, 270, 90);
    back.AddLine(21.5f, 8.0f, 21.5f, 18.0f);
    back.AddLine(21.5f, 18.0f, 2.5f, 18.0f);
    back.CloseFigure();
    SolidBrush bb(C(rgb(0xE8A317)));
    g.FillPath(&bb, &back);
    GraphicsPath front;
    roundRectPath(front, 2.5f, 8.5f, 19.0f, 11.5f, 1.6f);
    LinearGradientBrush fg(PointF(0, 8.5f), PointF(0, 20.0f), C(rgb(0xFFD25E)), C(rgb(0xF5B52A)));
    g.FillPath(&fg, &front);
    if (type.empty() || type == "folder") return;
    TypeLook L = lookFor(type);
    GraphicsPath dot;
    roundRectPath(dot, 12.2f, 12.0f, 7.6f, 6.6f, 1.6f);
    LinearGradientBrush db(PointF(0, 12.0f), PointF(0, 18.6f), C(L.top), C(L.bottom));
    g.FillPath(&db, &dot);
    // mini glyph scaled into the dot
    g.TranslateTransform(12.2f + 3.8f - 12.0f * 0.42f, 12.0f + 3.3f - 12.0f * 0.42f - 0.3f);
    g.ScaleTransform(0.42f, 0.42f);
    glyph(g, L.glyph, L.fillGlyph, 2.6f);
}

// A small rounded tile with a gradient and a white symbol (navigation entries such as Finished or Queues).
void tileIcon(HDC dc, const RECT& r, const std::string& svg, COLORREF top, COLORREF bottom) {
    Graphics g(dc);
    setup(g, r);
    GraphicsPath t;
    roundRectPath(t, 3.0f, 3.0f, 18.0f, 18.0f, 4.5f);
    LinearGradientBrush b(PointF(0, 3), PointF(0, 21), C(top), C(bottom));
    g.FillPath(&b, &t);
    g.TranslateTransform(12.0f - 12.0f * 0.62f, 12.0f - 12.0f * 0.62f);
    g.ScaleTransform(0.62f, 0.62f);
    auto& tab = iconTable();
    auto it = tab.find(svg);
    glyph(g, it != tab.end() ? it->second.c_str() : svg.c_str(), false, 2.6f);
}

} // namespace ui
} // namespace ddm
