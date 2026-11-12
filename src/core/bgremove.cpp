// The network runs layer by layer: convolutions as tiled matrix products on every processor core (SSE2), max-pooling,
// bilinear resizing, joins and sums (built with SSE2: build.sh). Its file ("DDMN", made from the published u2netp weights) is built into DDM.
#include "bgremove.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
namespace Gdiplus { using std::min; using std::max; }
#include <objidl.h>
#include <gdiplus.h>

namespace ddm {
namespace bgremove {

namespace {

struct Op { int kind = 0; std::vector<int> in; int out = 0; int ic = 0, oc = 0, k = 1, dil = 1, pad = 0; bool relu = false; size_t w = 0; };
struct Net { std::vector<Op> ops; int tensors = 0, output = 0; std::vector<float> weights; };

struct Tensor { int c = 0, h = 0, w = 0; std::vector<float> d; };

bool loadNet(Net& n, std::string& error) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(mod, L"DDMBGNET", MAKEINTRESOURCEW(10));
    HGLOBAL g = r ? LoadResource(mod, r) : nullptr;
    const unsigned char* p = g ? static_cast<const unsigned char*>(LockResource(g)) : nullptr;
    size_t size = r ? SizeofResource(mod, r) : 0;
    if (!p || size < 24 || memcmp(p, "DDMN", 4) != 0) { error = "The background remover is missing from this copy of DDM. Reinstall DDM."; return false; }
    size_t o = 4;
    auto u32 = [&](uint32_t& v) { if (o + 4 > size) return false; memcpy(&v, p + o, 4); o += 4; return true; };
    uint32_t ver, nops, nt, out;
    if (!u32(ver) || !u32(nops) || !u32(nt) || !u32(out) || ver != 1 || nops > 10000 || nt > 10000 || out >= nt) { error = "The background remover's file is damaged."; return false; }
    n.tensors = static_cast<int>(nt);
    n.output = static_cast<int>(out);
    size_t wpos = 0;
    for (uint32_t i = 0; i < nops; i++) {
        Op op;
        if (o + 5 > size) { error = "The background remover's file is damaged."; return false; }
        op.kind = p[o]; o++;
        uint32_t ni, v, par[6];
        if (!u32(ni) || ni == 0 || ni > 16) { error = "The background remover's file is damaged."; return false; }
        for (uint32_t k = 0; k < ni; k++) { if (!u32(v) || v >= nt) { error = "The background remover's file is damaged."; return false; } op.in.push_back(static_cast<int>(v)); }
        if (!u32(v) || v >= nt) { error = "The background remover's file is damaged."; return false; }
        op.out = static_cast<int>(v);
        for (auto& x : par) if (!u32(x)) { error = "The background remover's file is damaged."; return false; }
        if (op.kind == 1) {
            op.ic = static_cast<int>(par[0]); op.oc = static_cast<int>(par[1]); op.k = static_cast<int>(par[2]); op.dil = static_cast<int>(par[3]); op.pad = static_cast<int>(par[4]); op.relu = par[5] != 0;
            if (op.ic <= 0 || op.oc <= 0 || op.ic > 4096 || op.oc > 4096 || (op.k != 1 && op.k != 3)) { error = "The background remover's file is damaged."; return false; }
            op.w = wpos;
            wpos += static_cast<size_t>(op.oc) * op.ic * op.k * op.k + op.oc;
        } else if (op.kind < 2 || op.kind > 6) { error = "The background remover's file is damaged."; return false; }
        n.ops.push_back(op);
    }
    uint32_t nw;
    if (!u32(nw) || nw != wpos || o + static_cast<size_t>(nw) * 4 > size) { error = "The background remover's file is damaged."; return false; }
    n.weights.resize(nw);
    memcpy(n.weights.data(), p + o, static_cast<size_t>(nw) * 4);
    return true;
}

int threadCount() { return static_cast<int>(std::max(1u, std::min(16u, std::thread::hardware_concurrency()))); }

// work items 0…n-1 shared out to the cores
template <class F> void parallel(int n, F f) {
    int t = std::min(threadCount(), n);
    if (t <= 1) { for (int i = 0; i < n; i++) f(i, 0); return; }
    std::atomic<int> next{0};
    std::vector<std::thread> th;
    for (int k = 0; k < t; k++) th.emplace_back([&, k] { for (int i; (i = next++) < n;) f(i, k); });
    for (auto& x : th) x.join();
}

// out[oc][p] = bias + Σ W[oc][k] · col[k][p]   (four output channels at a time: each column value read once for four)
void gemm(const float* W, const float* bias, int oc, int K, const float* col, size_t colStride, int n, float* out, size_t outStride, bool relu) {
    int o = 0;
    for (; o + 4 <= oc; o += 4) {
        float* r0 = out + static_cast<size_t>(o) * outStride; float* r1 = r0 + outStride; float* r2 = r1 + outStride; float* r3 = r2 + outStride;
        for (int p = 0; p < n; p++) { r0[p] = bias[o]; r1[p] = bias[o + 1]; r2[p] = bias[o + 2]; r3[p] = bias[o + 3]; }
        const float* w0 = W + static_cast<size_t>(o) * K; const float* w1 = w0 + K; const float* w2 = w1 + K; const float* w3 = w2 + K;
        for (int k = 0; k < K; k++) {
            const float* c = col + static_cast<size_t>(k) * colStride;
            float a = w0[k], b = w1[k], cc = w2[k], d = w3[k];
            for (int p = 0; p < n; p++) { float v = c[p]; r0[p] += a * v; r1[p] += b * v; r2[p] += cc * v; r3[p] += d * v; }
        }
        if (relu) for (int p = 0; p < n; p++) { r0[p] = std::max(0.0f, r0[p]); r1[p] = std::max(0.0f, r1[p]); r2[p] = std::max(0.0f, r2[p]); r3[p] = std::max(0.0f, r3[p]); }
    }
    for (; o < oc; o++) {
        float* r = out + static_cast<size_t>(o) * outStride;
        for (int p = 0; p < n; p++) r[p] = bias[o];
        const float* w = W + static_cast<size_t>(o) * K;
        for (int k = 0; k < K; k++) { const float* c = col + static_cast<size_t>(k) * colStride; float a = w[k]; for (int p = 0; p < n; p++) r[p] += a * c[p]; }
        if (relu) for (int p = 0; p < n; p++) r[p] = std::max(0.0f, r[p]);
    }
}

void conv(const Tensor& x, const Op& op, const float* W, Tensor& y) {
    y.c = op.oc; y.h = x.h; y.w = x.w;
    y.d.assign(static_cast<size_t>(y.c) * y.h * y.w, 0.0f);
    const int HW = x.h * x.w, K = op.ic * op.k * op.k;
    const float* bias = W + static_cast<size_t>(op.oc) * K;
    const int tile = 512;
    int tiles = (HW + tile - 1) / tile;
    std::vector<std::vector<float>> cols(static_cast<size_t>(threadCount()));
    // a tile's output rows go to a small buffer first, then into place
    std::vector<std::vector<float>> outs(static_cast<size_t>(threadCount()));
    parallel(tiles, [&](int t, int th) {
        int p0 = t * tile, n = std::min(tile, HW - p0);
        auto& ob = outs[static_cast<size_t>(th)];
        ob.resize(static_cast<size_t>(op.oc) * tile);
        if (op.k == 1) gemm(W, bias, op.oc, K, x.d.data() + p0, static_cast<size_t>(HW), n, ob.data(), tile, op.relu);
        else {
            auto& col = cols[static_cast<size_t>(th)];
            col.resize(static_cast<size_t>(K) * tile);
            for (int c = 0; c < op.ic; c++)
                for (int ky = 0; ky < 3; ky++)
                    for (int kx = 0; kx < 3; kx++) {
                        float* dst = col.data() + static_cast<size_t>((c * 3 + ky) * 3 + kx) * tile;
                        const float* src = x.d.data() + static_cast<size_t>(c) * HW;
                        int dy = ky * op.dil - op.pad, dx = kx * op.dil - op.pad;
                        for (int i = 0; i < n; i++) {
                            int p = p0 + i, yy = p / x.w + dy, xx = p % x.w + dx;
                            dst[i] = (yy >= 0 && yy < x.h && xx >= 0 && xx < x.w) ? src[yy * x.w + xx] : 0.0f;
                        }
                    }
            gemm(W, bias, op.oc, K, col.data(), tile, n, ob.data(), tile, op.relu);
        }
        for (int o = 0; o < op.oc; o++) memcpy(y.d.data() + static_cast<size_t>(o) * HW + p0, ob.data() + static_cast<size_t>(o) * tile, static_cast<size_t>(n) * 4);
    });
}

void maxPool(const Tensor& x, Tensor& y) {   // 2×2, stride 2, ceil
    y.c = x.c; y.h = (x.h + 1) / 2; y.w = (x.w + 1) / 2;
    y.d.resize(static_cast<size_t>(y.c) * y.h * y.w);
    for (int c = 0; c < x.c; c++)
        for (int i = 0; i < y.h; i++)
            for (int j = 0; j < y.w; j++) {
                float m = -1e30f;
                for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) {
                    int yy = i * 2 + a, xx = j * 2 + b;
                    if (yy < x.h && xx < x.w) m = std::max(m, x.d[(static_cast<size_t>(c) * x.h + yy) * x.w + xx]);
                }
                y.d[(static_cast<size_t>(c) * y.h + i) * y.w + j] = m;
            }
}

// bilinear, PyTorch's half-pixel rule (align_corners = false)
void resizeTo(const Tensor& x, int H, int Wd, Tensor& y) {
    y.c = x.c; y.h = H; y.w = Wd;
    y.d.resize(static_cast<size_t>(y.c) * H * Wd);
    auto axis = [](int nOut, int nIn, std::vector<int>& i0, std::vector<int>& i1, std::vector<float>& f) {
        i0.resize(nOut); i1.resize(nOut); f.resize(nOut);
        double s = static_cast<double>(nIn) / nOut;
        for (int i = 0; i < nOut; i++) {
            double v = nOut > 1 ? (i + 0.5) * s - 0.5 : 0.0;
            if (v < 0) v = 0;
            int a = std::min(static_cast<int>(v), nIn - 1);
            i0[i] = a; i1[i] = std::min(a + 1, nIn - 1); f[i] = static_cast<float>(v - a);
        }
    };
    std::vector<int> y0, y1, x0, x1;
    std::vector<float> fy, fx;
    axis(H, x.h, y0, y1, fy);
    axis(Wd, x.w, x0, x1, fx);
    for (int c = 0; c < x.c; c++) {
        const float* s = x.d.data() + static_cast<size_t>(c) * x.h * x.w;
        float* d = y.d.data() + static_cast<size_t>(c) * H * Wd;
        for (int i = 0; i < H; i++) {
            const float* ra = s + y0[i] * x.w; const float* rb = s + y1[i] * x.w;
            for (int j = 0; j < Wd; j++) {
                float top = ra[x0[j]] * (1 - fx[j]) + ra[x1[j]] * fx[j], bot = rb[x0[j]] * (1 - fx[j]) + rb[x1[j]] * fx[j];
                d[i * Wd + j] = top * (1 - fy[i]) + bot * fy[i];
            }
        }
    }
}

bool runNet(const Net& n, Tensor input, Tensor& out, std::function<void(double)> progress, std::atomic<bool>* stop) {
    std::vector<Tensor> t(static_cast<size_t>(n.tensors));
    // when each tensor is last needed: freed right after (keeps memory small)
    std::vector<int> last(static_cast<size_t>(n.tensors), -1);
    for (size_t i = 0; i < n.ops.size(); i++) for (int x : n.ops[i].in) last[static_cast<size_t>(x)] = static_cast<int>(i);
    t[0] = std::move(input);
    for (size_t i = 0; i < n.ops.size(); i++) {
        if (stop && stop->load()) return false;
        const Op& op = n.ops[i];
        const Tensor& a = t[static_cast<size_t>(op.in[0])];
        Tensor y;
        switch (op.kind) {
            case 1: if (a.c != op.ic) return false; conv(a, op, n.weights.data() + op.w, y); break;
            case 2: maxPool(a, y); break;
            case 3: { const Tensor& r = t[static_cast<size_t>(op.in[1])]; resizeTo(a, r.h, r.w, y); break; }
            case 4: {
                y.h = a.h; y.w = a.w; y.c = 0;
                for (int x : op.in) { const Tensor& b = t[static_cast<size_t>(x)]; if (b.h != a.h || b.w != a.w) return false; y.c += b.c; }
                y.d.reserve(static_cast<size_t>(y.c) * y.h * y.w);
                for (int x : op.in) { const Tensor& b = t[static_cast<size_t>(x)]; y.d.insert(y.d.end(), b.d.begin(), b.d.end()); }
                break;
            }
            case 5: { const Tensor& b = t[static_cast<size_t>(op.in[1])]; if (b.d.size() != a.d.size()) return false; y = a; for (size_t k = 0; k < y.d.size(); k++) y.d[k] += b.d[k]; break; }
            case 6: y = a; for (auto& v : y.d) v = 1.0f / (1.0f + std::exp(-v)); break;
        }
        for (int x : op.in) if (last[static_cast<size_t>(x)] == static_cast<int>(i) && x != n.output) { Tensor e; std::swap(t[static_cast<size_t>(x)], e); }
        t[static_cast<size_t>(op.out)] = std::move(y);
        if (progress) progress(static_cast<double>(i + 1) / n.ops.size());
    }
    out = std::move(t[static_cast<size_t>(n.output)]);
    return out.c >= 1 && !out.d.empty();
}

// ---- pictures
ULONG_PTR g_gdip = 0;
std::once_flag g_gdipOnce;
void gdiplus() { std::call_once(g_gdipOnce, [] { Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&g_gdip, &in, nullptr); }); }

struct Image { int w = 0, h = 0; std::vector<uint8_t> bgra; };

bool loadPicture(const std::string& path, Image& img, std::string& error) {
    gdiplus();
    std::unique_ptr<Gdiplus::Bitmap> b(Gdiplus::Bitmap::FromFile(W(path).c_str()));
    if (!b || b->GetLastStatus() != Gdiplus::Ok || b->GetWidth() == 0 || b->GetHeight() == 0) { error = "Windows can't read this picture (JPEG, PNG, BMP, GIF or TIFF)."; return false; }
    {   // photos from phones: turned the way the camera held them
        UINT sz = b->GetPropertyItemSize(0x0112);
        if (sz > 0) {
            std::vector<BYTE> buf(sz);
            auto* it = reinterpret_cast<Gdiplus::PropertyItem*>(buf.data());
            if (b->GetPropertyItem(0x0112, sz, it) == Gdiplus::Ok && it->value) {
                int o = *static_cast<unsigned short*>(it->value);
                Gdiplus::RotateFlipType rf = o == 2 ? Gdiplus::RotateNoneFlipX : o == 3 ? Gdiplus::Rotate180FlipNone : o == 4 ? Gdiplus::Rotate180FlipX : o == 5 ? Gdiplus::Rotate90FlipX :
                                             o == 6 ? Gdiplus::Rotate90FlipNone : o == 7 ? Gdiplus::Rotate270FlipX : o == 8 ? Gdiplus::Rotate270FlipNone : Gdiplus::RotateNoneFlipNone;
                if (rf != Gdiplus::RotateNoneFlipNone) b->RotateFlip(rf);
            }
        }
    }
    UINT w = b->GetWidth(), h = b->GetHeight();
    if (static_cast<uint64_t>(w) * h > 60ULL * 1000 * 1000) { error = "This picture is too big (more than 60 megapixels)."; return false; }
    img.w = static_cast<int>(w); img.h = static_cast<int>(h);
    img.bgra.resize(static_cast<size_t>(w) * h * 4);
    Gdiplus::Rect r(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    Gdiplus::BitmapData bd{};
    if (b->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) { error = "Windows can't read this picture."; return false; }
    for (UINT y = 0; y < h; y++) memcpy(img.bgra.data() + static_cast<size_t>(y) * w * 4, static_cast<uint8_t*>(bd.Scan0) + static_cast<ptrdiff_t>(y) * bd.Stride, static_cast<size_t>(w) * 4);
    b->UnlockBits(&bd);
    return true;
}

bool savePng(const std::string& path, const Image& img, std::string& error) {
    gdiplus();
    Gdiplus::Bitmap b(img.w, img.h, PixelFormat32bppARGB);
    Gdiplus::Rect r(0, 0, img.w, img.h);
    Gdiplus::BitmapData bd{};
    if (b.LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) { error = "Couldn't make the picture (not enough memory?)."; return false; }
    for (int y = 0; y < img.h; y++) memcpy(static_cast<uint8_t*>(bd.Scan0) + static_cast<ptrdiff_t>(y) * bd.Stride, img.bgra.data() + static_cast<size_t>(y) * img.w * 4, static_cast<size_t>(img.w) * 4);
    b.UnlockBits(&bd);
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* enc = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, enc);
    for (UINT i = 0; i < num; i++)
        if (wcscmp(enc[i].MimeType, L"image/png") == 0) {
            std::string tmp = path + ".saving";
            if (b.Save(W(tmp).c_str(), &enc[i].Clsid, nullptr) != Gdiplus::Ok) { DeleteFileW(W(tmp).c_str()); error = "Couldn't save " + baseName(path) + " (is the disk full?)."; return false; }
            if (!MoveFileExW(W(tmp).c_str(), W(path).c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(W(tmp).c_str()); error = "Couldn't save " + baseName(path) + "."; return false; }
            return true;
        }
    error = "Windows can't save PNG pictures.";
    return false;
}

// a picture's plane resized (area-averaged when smaller, bilinear when bigger)
std::vector<float> resample(const std::vector<float>& s, int sw, int sh, int dw, int dh) {
    std::vector<float> d(static_cast<size_t>(dw) * dh);
    if (dw <= sw && dh <= sh) {
        double kx = static_cast<double>(sw) / dw, ky = static_cast<double>(sh) / dh;
        for (int y = 0; y < dh; y++) {
            int y0 = static_cast<int>(y * ky), y1 = std::max(y0 + 1, std::min(sh, static_cast<int>((y + 1) * ky)));
            for (int x = 0; x < dw; x++) {
                int x0 = static_cast<int>(x * kx), x1 = std::max(x0 + 1, std::min(sw, static_cast<int>((x + 1) * kx)));
                double sum = 0;
                for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) sum += s[static_cast<size_t>(yy) * sw + xx];
                d[static_cast<size_t>(y) * dw + x] = static_cast<float>(sum / ((y1 - y0) * (x1 - x0)));
            }
        }
        return d;
    }
    for (int y = 0; y < dh; y++) {
        double fy = std::max(0.0, (y + 0.5) * sh / dh - 0.5);
        int y0 = std::min(static_cast<int>(fy), sh - 1), y1 = std::min(y0 + 1, sh - 1);
        float wy = static_cast<float>(fy - y0);
        for (int x = 0; x < dw; x++) {
            double fx = std::max(0.0, (x + 0.5) * sw / dw - 0.5);
            int x0 = std::min(static_cast<int>(fx), sw - 1), x1 = std::min(x0 + 1, sw - 1);
            float wx = static_cast<float>(fx - x0);
            float a = s[static_cast<size_t>(y0) * sw + x0] * (1 - wx) + s[static_cast<size_t>(y0) * sw + x1] * wx;
            float b = s[static_cast<size_t>(y1) * sw + x0] * (1 - wx) + s[static_cast<size_t>(y1) * sw + x1] * wx;
            d[static_cast<size_t>(y) * dw + x] = a * (1 - wy) + b * wy;
        }
    }
    return d;
}

// mean over a (2r+1)² box, edges handled (running sums: any radius at the same cost)
std::vector<float> boxMean(const std::vector<float>& s, int w, int h, int r) {
    std::vector<double> rows(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++) {
        const float* in = s.data() + static_cast<size_t>(y) * w;
        double* out = rows.data() + static_cast<size_t>(y) * w;
        double acc = 0;
        for (int x = 0; x <= std::min(r, w - 1); x++) acc += in[x];
        for (int x = 0; x < w; x++) {
            int a = x - r - 1, b = x + r;
            if (x > 0 && b < w) acc += in[b];
            if (x > 0 && a >= 0) acc -= in[a];
            out[x] = acc / (std::min(w - 1, x + r) - std::max(0, x - r) + 1);
        }
    }
    std::vector<float> d(static_cast<size_t>(w) * h);
    for (int x = 0; x < w; x++) {
        double acc = 0;
        for (int y = 0; y <= std::min(r, h - 1); y++) acc += rows[static_cast<size_t>(y) * w + x];
        for (int y = 0; y < h; y++) {
            int a = y - r - 1, b = y + r;
            if (y > 0 && b < h) acc += rows[static_cast<size_t>(b) * w + x];
            if (y > 0 && a >= 0) acc -= rows[static_cast<size_t>(a) * w + x];
            d[static_cast<size_t>(y) * w + x] = static_cast<float>(acc / (std::min(h - 1, y + r) - std::max(0, y - r) + 1));
        }
    }
    return d;
}

// the mask's edges follow the picture's own edges (guided filter: hair, fur and outlines stay sharp)
std::vector<float> guided(const std::vector<float>& I, const std::vector<float>& p, int w, int h, int r, float eps) {
    size_t n = I.size();
    std::vector<float> Ip(n), II(n);
    for (size_t i = 0; i < n; i++) { Ip[i] = I[i] * p[i]; II[i] = I[i] * I[i]; }
    auto mI = boxMean(I, w, h, r), mp = boxMean(p, w, h, r), mIp = boxMean(Ip, w, h, r), mII = boxMean(II, w, h, r);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; i++) {
        float cov = mIp[i] - mI[i] * mp[i], var = mII[i] - mI[i] * mI[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mI[i];
    }
    auto ma = boxMean(a, w, h, r), mb = boxMean(b, w, h, r);
    std::vector<float> q(n);
    for (size_t i = 0; i < n; i++) q[i] = ma[i] * I[i] + mb[i];
    return q;
}

std::once_flag g_netOnce;
std::unique_ptr<Net> g_net;
std::string g_netError;

// the subject's mask at the picture's size (0…1)
bool maskFor(const Image& img, std::vector<float>& alpha, std::function<void(double, const std::string&)> progress, std::atomic<bool>* stop, std::string& error) {
    std::call_once(g_netOnce, [] { auto n = std::make_unique<Net>(); if (loadNet(*n, g_netError)) g_net = std::move(n); });
    if (!g_net) { error = g_netError; return false; }
    const int S = 320;
    // the network's input: the picture at 320 × 320, scaled 0…1 by its brightest value, then ImageNet's mean and spread
    std::vector<float> ch[3];
    float maxv = 1;
    {
        std::vector<float> plane(static_cast<size_t>(img.w) * img.h);
        for (int c = 0; c < 3; c++) {
            int src = 2 - c;   // BGRA → R, G, B
            for (size_t i = 0; i < plane.size(); i++) {   // transparent parts as white (pictures that already have some)
                float a = img.bgra[i * 4 + 3] / 255.0f;
                plane[i] = img.bgra[i * 4 + static_cast<size_t>(src)] * a + 255.0f * (1 - a);
            }
            ch[c] = resample(plane, img.w, img.h, S, S);
        }
        maxv = 1e-6f;
        for (auto& c : ch) for (float v : c) maxv = std::max(maxv, v);
    }
    static const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
    Tensor in;
    in.c = 3; in.h = S; in.w = S;
    in.d.resize(3 * S * S);
    for (int c = 0; c < 3; c++) for (int i = 0; i < S * S; i++) in.d[static_cast<size_t>(c) * S * S + i] = (ch[c][static_cast<size_t>(i)] / maxv - mean[c]) / sd[c];
    Tensor out;
    if (progress) progress(0.05, "Finding the subject…");
    if (!runNet(*g_net, std::move(in), out, [&](double f) { if (progress) progress(0.05 + 0.8 * f, "Finding the subject…"); }, stop)) {
        if (stop && stop->load()) { error = "Stopped."; return false; }
        error = "The background remover couldn't finish.";
        return false;
    }
    std::vector<float> m(out.d.begin(), out.d.begin() + S * S);
    float lo = *std::min_element(m.begin(), m.end()), hi = *std::max_element(m.begin(), m.end());
    if (hi - lo < 1e-6f) { error = "No subject was found in this picture."; return false; }
    for (auto& v : m) v = (v - lo) / (hi - lo);
    if (progress) progress(0.88, "Refining the edges…");
    // refine at up to 2048 px on the long side (the guide: the picture's brightness), then to full size
    int lw = img.w, lh = img.h;
    double k = std::min(1.0, 2048.0 / std::max(img.w, img.h));
    lw = std::max(1, static_cast<int>(img.w * k)); lh = std::max(1, static_cast<int>(img.h * k));
    std::vector<float> gray(static_cast<size_t>(img.w) * img.h);
    for (size_t i = 0; i < gray.size(); i++) gray[i] = (0.299f * img.bgra[i * 4 + 2] + 0.587f * img.bgra[i * 4 + 1] + 0.114f * img.bgra[i * 4]) / 255.0f;
    std::vector<float> g2 = (lw == img.w && lh == img.h) ? gray : resample(gray, img.w, img.h, lw, lh);
    std::vector<float> p = resample(m, S, S, lw, lh);
    int r = std::max(2, static_cast<int>(std::max(lw, lh) / 320.0 * 2.5 + 0.5));
    std::vector<float> q = guided(g2, p, lw, lh, r, 1e-3f);
    for (size_t i = 0; i < q.size(); i++) {   // where the network was sure, keep its answer; refine only the edges
        float conf = std::fabs(p[i] - 0.5f) * 2;
        float v = conf > 0.9f ? p[i] : q[i] * (1 - conf) + p[i] * conf;
        v = (v - 0.03f) / 0.94f;   // a clean background and a solid subject
        q[i] = std::max(0.0f, std::min(1.0f, v));
    }
    {   // specks away from the subject (bits of background the network half-believed): only the subject's parts stay
        std::vector<int> label(q.size(), 0);
        std::vector<int> area(1, 0);
        std::vector<int> stack;
        for (int y = 0; y < lh; y++)
            for (int x = 0; x < lw; x++) {
                size_t i = static_cast<size_t>(y) * lw + x;
                if (q[i] < 0.5f || label[i]) continue;
                int id = static_cast<int>(area.size()), n = 0;
                area.push_back(0);
                stack.assign(1, static_cast<int>(i));
                label[i] = id;
                while (!stack.empty()) {
                    int c = stack.back(); stack.pop_back(); n++;
                    int cx = c % lw, cy = c / lw;
                    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (auto& d : nb) {
                        int nx = cx + d[0], ny = cy + d[1];
                        if (nx < 0 || ny < 0 || nx >= lw || ny >= lh) continue;
                        size_t j = static_cast<size_t>(ny) * lw + nx;
                        if (q[j] >= 0.5f && !label[j]) { label[j] = id; stack.push_back(static_cast<int>(j)); }
                    }
                }
                area[static_cast<size_t>(id)] = n;
            }
        int biggest = area.size() > 1 ? *std::max_element(area.begin() + 1, area.end()) : 0;
        if (biggest > 0) {
            std::vector<float> keep(q.size(), 0.0f);
            for (size_t i = 0; i < q.size(); i++) if (label[i] && area[static_cast<size_t>(label[i])] * 100 >= biggest) keep[i] = 1;
            int rr = std::max(3, std::max(lw, lh) / 10);   // what's near the subject (hair, a hand) stays
            std::vector<float> around = boxMean(keep, lw, lh, rr);
            for (size_t i = 0; i < q.size(); i++) q[i] *= std::min(1.0f, around[i] * 12.0f);   // (fading out with the distance: no hard cut)
        }
    }
    alpha = (lw == img.w && lh == img.h) ? q : resample(q, lw, lh, img.w, img.h);
    return true;
}

} // namespace

bool subjectMask(const std::string& picture, std::string& mask, int& width, int& height, std::string& error) {
    Image img;
    if (!loadPicture(picture, img, error)) return false;
    std::vector<float> a;
    if (!maskFor(img, a, nullptr, nullptr, error)) return false;
    width = img.w; height = img.h;
    mask.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) mask[i] = static_cast<char>(static_cast<int>(a[i] * 255 + 0.5f));
    return true;
}

std::string removeBackground(const std::string& picture, std::function<void(double, const std::string&)> progress, std::atomic<bool>* stop, std::string& error) {
    auto t0 = std::chrono::steady_clock::now();
    if (progress) progress(0.01, "Reading the picture…");
    Image img;
    if (!loadPicture(picture, img, error)) return "";
    std::vector<float> a;
    if (!maskFor(img, a, progress, stop, error)) return "";
    if (progress) progress(0.95, "Saving…");
    for (size_t i = 0; i < a.size(); i++) {
        float al = a[i] * (img.bgra[i * 4 + 3] / 255.0f);
        img.bgra[i * 4 + 3] = static_cast<uint8_t>(al * 255 + 0.5f);
    }
    std::string out = uniquePath(joinPath(dirName(picture), stem(baseName(picture)) + " (no background).png"));
    if (!savePng(out, img, error)) return "";
    logLine("background removed: " + baseName(picture) + " " + std::to_string(img.w) + "x" + std::to_string(img.h) + " in " +
            std::to_string(static_cast<int>(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1000)) + " ms");
    if (progress) progress(1, "Done");
    return out;
}

} // namespace bgremove
} // namespace ddm
