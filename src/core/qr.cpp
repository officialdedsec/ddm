#include "qr.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace ddm {
namespace qr {

namespace {
// error correction level M
const int8_t kEccPerBlock[41] = {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28};
const int8_t kBlocks[41] = {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49};
const int kFormatM = 0;

int rawModules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) { int n = ver / 7 + 2; r -= (25 * n - 10) * n - 55; if (ver >= 7) r -= 36; }
    return r;
}
int dataCodewords(int ver) { return rawModules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

uint8_t gfMul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
    return static_cast<uint8_t>(z);
}
std::vector<uint8_t> rsDivisor(int degree) {
    std::vector<uint8_t> r(static_cast<size_t>(degree), 0);
    r[static_cast<size_t>(degree) - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (size_t j = 0; j < r.size(); j++) { r[j] = gfMul(r[j], root); if (j + 1 < r.size()) r[j] ^= r[j + 1]; }
        root = gfMul(root, 0x02);
    }
    return r;
}
std::vector<uint8_t> rsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
    std::vector<uint8_t> r(div.size(), 0);
    for (uint8_t b : data) {
        uint8_t f = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); i++) r[i] ^= gfMul(div[i], f);
    }
    return r;
}

struct Grid {
    int n;
    std::vector<bool> m, fn;
    explicit Grid(int size) : n(size), m(static_cast<size_t>(size) * size, false), fn(static_cast<size_t>(size) * size, false) {}
    bool get(int x, int y) const { return m[static_cast<size_t>(y) * n + x]; }
    void set(int x, int y, bool d) { m[static_cast<size_t>(y) * n + x] = d; }
    void func(int x, int y, bool d) { set(x, y, d); fn[static_cast<size_t>(y) * n + x] = true; }
    bool isFn(int x, int y) const { return fn[static_cast<size_t>(y) * n + x]; }
};

void finder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int d = std::max(std::abs(dx), std::abs(dy)), x = cx + dx, y = cy + dy;
            if (x >= 0 && x < g.n && y >= 0 && y < g.n) g.func(x, y, d != 2 && d != 4);
        }
}
void alignment(Grid& g, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) g.func(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}
std::vector<int> alignPositions(int ver, int size) {
    if (ver == 1) return {};
    int num = ver / 7 + 2;
    int step = ver == 32 ? 26 : (ver * 4 + num * 2 + 1) / (num * 2 - 2) * 2;
    std::vector<int> r = {6};
    for (int pos = size - 7; static_cast<int>(r.size()) < num; pos -= step) r.insert(r.begin() + 1, pos);
    return r;
}
void formatBits(Grid& g, int mask) {
    int data = kFormatM << 3 | mask, rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; i++) g.func(8, i, bit(i));
    g.func(8, 7, bit(6)); g.func(8, 8, bit(7)); g.func(7, 8, bit(8));
    for (int i = 9; i < 15; i++) g.func(14 - i, 8, bit(i));
    for (int i = 0; i < 8; i++) g.func(g.n - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; i++) g.func(8, g.n - 15 + i, bit(i));
    g.func(8, g.n - 8, true);
}
void functionPatterns(Grid& g, int ver) {
    for (int i = 0; i < g.n; i++) { g.func(6, i, i % 2 == 0); g.func(i, 6, i % 2 == 0); }
    finder(g, 3, 3); finder(g, g.n - 4, 3); finder(g, 3, g.n - 4);
    auto al = alignPositions(ver, g.n);
    for (size_t i = 0; i < al.size(); i++)
        for (size_t j = 0; j < al.size(); j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == al.size() - 1) || (i == al.size() - 1 && j == 0)) continue;
            alignment(g, al[i], al[j]);
        }
    formatBits(g, 0);   // reserved (drawn again with the real mask)
    if (ver >= 7) {
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = static_cast<long>(ver) << 12 | rem;
        for (int i = 0; i < 18; i++) {
            bool b = ((bits >> i) & 1) != 0;
            int a = g.n - 11 + i % 3, c = i / 3;
            g.func(a, c, b); g.func(c, a, b);
        }
    }
}
bool maskAt(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}
void applyMask(Grid& g, int mask) {
    for (int y = 0; y < g.n; y++) for (int x = 0; x < g.n; x++) if (!g.isFn(x, y) && maskAt(mask, x, y)) g.set(x, y, !g.get(x, y));
}
long penalty(const Grid& g) {
    long p = 0;
    int n = g.n;
    for (int pass = 0; pass < 2; pass++)   // rows, then columns
        for (int a = 0; a < n; a++) {
            int run = 1;
            for (int b = 1; b <= n; b++) {
                bool same = b < n && (pass ? g.get(a, b) == g.get(a, b - 1) : g.get(b, a) == g.get(b - 1, a));
                if (same) run++;
                else { if (run >= 5) p += 3 + (run - 5); run = 1; }
            }
            for (int b = 0; b + 10 < n; b++) {   // 1:1:3:1:1 with four light modules on a side
                static const bool pat1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0}, pat2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
                bool m1 = true, m2 = true;
                for (int k = 0; k < 11; k++) { bool v = pass ? g.get(a, b + k) : g.get(b + k, a); if (v != pat1[k]) m1 = false; if (v != pat2[k]) m2 = false; }
                p += (m1 ? 40 : 0) + (m2 ? 40 : 0);
            }
        }
    for (int y = 0; y + 1 < n; y++)
        for (int x = 0; x + 1 < n; x++) { bool c = g.get(x, y); if (c == g.get(x + 1, y) && c == g.get(x, y + 1) && c == g.get(x + 1, y + 1)) p += 3; }
    long darkCount = 0;
    for (bool d : g.m) darkCount += d;
    long total = static_cast<long>(n) * n;
    long k = (std::labs(darkCount * 20 - total * 10) + total - 1) / total - 1;
    p += k * 10;
    return p;
}
} // namespace

bool encode(const std::string& text, Code& out, int mask, int minVersion) {
    int ver = std::max(1, minVersion);
    for (;; ver++) {
        if (ver > 40) return false;
        int countBits = ver <= 9 ? 8 : 16;
        if (4 + countBits + static_cast<int>(text.size()) * 8 <= dataCodewords(ver) * 8) break;
    }
    // the data: byte mode, its length, the bytes, an end mark, then the standard filler
    std::vector<bool> bits;
    auto put = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back(((v >> i) & 1) != 0); };
    put(4, 4);
    put(static_cast<uint32_t>(text.size()), ver <= 9 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    int capacity = dataCodewords(ver) * 8;
    put(0, std::min(4, capacity - static_cast<int>(bits.size())));
    while (bits.size() % 8) bits.push_back(false);
    for (uint8_t pad = 0xEC; static_cast<int>(bits.size()) < capacity; pad ^= 0xEC ^ 0x11) put(pad, 8);
    std::vector<uint8_t> data(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) if (bits[i]) data[i >> 3] |= static_cast<uint8_t>(1 << (7 - (i & 7)));
    // error correction, block by block, interleaved
    int blocks = kBlocks[ver], ecc = kEccPerBlock[ver], raw = rawModules(ver) / 8;
    int shortBlocks = blocks - raw % blocks, shortLen = raw / blocks;
    auto div = rsDivisor(ecc);
    std::vector<std::vector<uint8_t>> all;
    for (int i = 0, k = 0; i < blocks; i++) {
        int len = shortLen - ecc + (i < shortBlocks ? 0 : 1);
        std::vector<uint8_t> d(data.begin() + k, data.begin() + k + len);
        k += len;
        auto e = rsRemainder(d, div);
        if (i < shortBlocks) d.push_back(0);
        d.insert(d.end(), e.begin(), e.end());
        all.push_back(d);
    }
    std::vector<uint8_t> words;
    for (size_t i = 0; i < all[0].size(); i++)
        for (int j = 0; j < blocks; j++)
            if (i != static_cast<size_t>(shortLen - ecc) || j >= shortBlocks) words.push_back(all[static_cast<size_t>(j)][i]);
    // the picture
    int size = ver * 4 + 17;
    Grid g(size);
    functionPatterns(g, ver);
    size_t bi = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int v = 0; v < size; v++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool up = ((right + 1) & 2) == 0;
                int y = up ? size - 1 - v : v;
                if (!g.isFn(x, y) && bi < words.size() * 8) { g.set(x, y, ((words[bi >> 3] >> (7 - (bi & 7))) & 1) != 0); bi++; }
            }
    }
    if (mask < 0) {
        long best = -1;
        for (int k = 0; k < 8; k++) {
            Grid t = g;
            applyMask(t, k);
            formatBits(t, k);
            long pv = penalty(t);
            if (best < 0 || pv < best) { best = pv; mask = k; }
        }
    }
    applyMask(g, mask);
    formatBits(g, mask);
    out.size = size;
    out.dark = g.m;
    return true;
}

} // namespace qr
} // namespace ddm
