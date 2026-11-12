#include "listvault.hpp"
#include "crypto.hpp"
#include <cstring>

namespace ddm {
namespace listvault {

namespace {
const char kMagic[] = "DDMLIST1";
const unsigned char PEPE[26] = {30, 56, 48, 122, 35, 26, 93, 47, 38, 40, 49, 99, 124, 48, 89, 95, 7, 31, 20, 237, 224, 228, 184, 204, 163, 152};
crypto::Bytes key(const std::string& label) {
    crypto::Bytes k(26);
    for (int i = 0; i < 26; i++) k[static_cast<size_t>(i)] = static_cast<unsigned char>(PEPE[i] ^ 0x5A ^ ((i * 7) & 0xFF));
    return crypto::hmacSha256(k, label.data(), label.size());
}
} // namespace

std::vector<unsigned char> encrypt(const std::string& json) {
    crypto::Bytes iv = crypto::random(16), ct;
    crypto::aesCbc(true, key("ddm-list-aes-v1"), iv, crypto::Bytes(json.begin(), json.end()), ct);
    crypto::Bytes body(kMagic, kMagic + 8);
    body.insert(body.end(), iv.begin(), iv.end());
    body.insert(body.end(), ct.begin(), ct.end());
    crypto::Bytes mac = crypto::hmacSha256(key("ddm-list-mac-v1"), body.data(), body.size());
    body.insert(body.end(), mac.begin(), mac.end());
    return body;
}

bool looksLikeDdm(const std::string& d) { return d.size() >= 8 && memcmp(d.data(), kMagic, 8) == 0; }

bool decrypt(const std::vector<unsigned char>& blob, std::string& json, std::string& error) {
    if (blob.size() < 8 + 16 + 32 || memcmp(blob.data(), kMagic, 8) != 0) { error = "This isn't a DDM list file."; return false; }
    size_t bodyLen = blob.size() - 32;
    crypto::Bytes mac = crypto::hmacSha256(key("ddm-list-mac-v1"), blob.data(), bodyLen);
    int r = 0;
    for (size_t i = 0; i < 32; i++) r |= mac[i] ^ blob[bodyLen + i];
    if (r) { error = "This DDM list file is corrupt, was changed, or isn't from DDM."; return false; }
    crypto::Bytes iv(blob.begin() + 8, blob.begin() + 24), ct(blob.begin() + 24, blob.begin() + static_cast<long>(bodyLen)), plain;
    if (!crypto::aesCbc(false, key("ddm-list-aes-v1"), iv, ct, plain)) { error = "This DDM list file couldn't be opened."; return false; }
    json.assign(plain.begin(), plain.end());
    return true;
}

} // namespace listvault
} // namespace ddm
