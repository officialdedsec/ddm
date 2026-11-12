// QR codes, made by DDM itself (ISO/IEC 18004: byte mode, error correction M, versions 1–40, best mask): the phone
// remote's address as a code to scan with the phone's camera.
#pragma once
#include <string>
#include <vector>

namespace ddm {
namespace qr {

struct Code {
    int size = 0;
    std::vector<bool> dark;           // size × size, row by row
    bool at(int x, int y) const { return dark[static_cast<size_t>(y) * size + x]; }
};

// mask: -1 = the best one (as the standard scores them), 0–7 = that one (tests). false = too long for a QR code.
bool encode(const std::string& text, Code& out, int mask = -1, int minVersion = 1);

} // namespace qr
} // namespace ddm
