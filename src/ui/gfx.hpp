// Drawing helpers: buffered painting, smooth shapes (GDI+), crisp text (GDI), and the vector icons from the mock-ups.
#pragma once
#include <memory>
#include <windows.h>
#include <string>

namespace ddm {
namespace ui {

void gfxInit();
void gfxShutdown();

// Flicker-free painting: draw into the returned DC; the buffer is copied on destruction.
class Paint {
public:
    explicit Paint(HWND h);
    ~Paint();
    HDC dc() const { return mem; }
    RECT rc() const { return client; }
private:
    HWND hwnd;
    PAINTSTRUCT ps;
    HDC real, mem;
    HBITMAP bmp, old;
    RECT client;
};

void fill(HDC dc, const RECT& r, COLORREF c);
void fillRound(HDC dc, const RECT& r, float radius, COLORREF c);
void strokeRound(HDC dc, const RECT& r, float radius, COLORREF c, float width = 1.0f);
void progressBar(HDC dc, const RECT& r, double frac, COLORREF track, COLORREF from, COLORREF to, float radius, float phase = 0, bool stripes = false);
void fillCircle(HDC dc, float cx, float cy, float rad, COLORREF c);
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c);
// Smooth strokes for gauges and graphs (angles in degrees, 0 = right, clockwise; alpha 0-255)
void arcStroke(HDC dc, float cx, float cy, float r, float startDeg, float sweepDeg, COLORREF c, float width, int alpha = 255);
void lineAA(HDC dc, float x1, float y1, float x2, float y2, COLORREF c, float width, int alpha = 255);
void polyStroke(HDC dc, const float* xy, int points, COLORREF c, float width, int alpha = 255);   // xy: x0,y0,x1,y1…
void polyFill(HDC dc, const float* xy, int points, COLORREF c, int alpha = 255);

// Text (UTF-8). flags = DT_* (DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS by default).
void text(HDC dc, const std::string& s, const RECT& r, COLORREF c, HFONT f, UINT flags = DT_LEFT);
SIZE textSize(HDC dc, const std::string& s, HFONT f);
// the same for names (files, sites, categories, titles): never partly translated
void textName(HDC dc, const std::string& s, const RECT& r, COLORREF c, HFONT f, UINT flags = DT_LEFT);
SIZE textSizeName(HDC dc, const std::string& s, HFONT f);

// Vector icons: a named icon or raw SVG path data ("M12 4v11 M7 10.5l5 5 5-5"), drawn in a 24x24 box scaled to r.
void icon(HDC dc, const std::string& nameOrPath, const RECT& r, COLORREF c, float stroke = 1.8f, bool filled = false);

// The site badge used before favicons arrive: a rounded square with the site's first letter.
void siteBadge(HDC dc, const RECT& r, const std::string& site);
// A favicon image (PNG/ICO bytes), cached by site. Returns false when none is cached.
bool drawFavicon(HDC dc, const RECT& r, const std::string& site);
// Premium coloured icons: file sheets by kind, folders with a kind mark, and gradient tiles.
std::string typeOf(const std::string& category, const std::string& fileName, const std::string& kind);   // video, music, programs, compressed, documents, pdf, image, torrent, file
void fileIcon(HDC dc, const RECT& r, const std::string& type);
bool systemFileIcon(HDC dc, const RECT& r, const std::string& nameOrPath, bool exists);   // Windows' own icon for the file or its type
void folderIcon(HDC dc, const RECT& r, const std::string& type);
void tileIcon(HDC dc, const RECT& r, const std::string& iconNameOrSvg, COLORREF top, COLORREF bottom);
// PNG pictures built into DDM.exe (RCDATA by name)
SIZE resImageSize(const std::string& name);
bool drawResImage(HDC dc, const RECT& r, const std::string& name);
void setFavicon(const std::string& site, const std::string& bytes);
// A picture from its bytes (JPEG, PNG, WebP where Windows can read it). Empty when it can't be read.
std::shared_ptr<void> loadImage(const std::string& bytes);
// File tools: a smaller copy of a picture (longest side at most maxSide, JPEG quality 1-100, PNG stays PNG)
bool compressImage(const std::string& in, const std::string& out, int maxSide, int quality, std::string& error);
// Fills r with the picture (cropped to fit, rounded corners); an optional caption sits on a dark band at the bottom.
void drawImageCover(HDC dc, const RECT& r, const std::shared_ptr<void>& img, float radius, const std::string& caption = "", const std::string& caption2 = "");

// The DDM logo tile (dark rounded square with the lime arrow).
void logo(HDC dc, const RECT& r, bool disabled = false);

} // namespace ui
} // namespace ddm
