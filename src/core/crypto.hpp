// Hashes, HMAC, RSA signature checks and base64 on Windows' own crypto (CNG, Windows 7+).
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ddm {
namespace crypto {

typedef std::vector<unsigned char> Bytes;

Bytes sha256(const void* data, size_t len);
inline Bytes sha256(const std::string& s) { return sha256(s.data(), s.size()); }
Bytes md5(const void* data, size_t len);   // (a site's request signature, not for security)
inline Bytes md5(const std::string& s) { return md5(s.data(), s.size()); }
// Whole file; algo "SHA256" | "SHA1" | "MD5". Empty on error. progress(done, total) optional.
Bytes hashFile(const std::string& path, const wchar_t* algo, const std::atomic_bool* stop = nullptr);
// A checksum as people write it: "md5" | "sha1" | "sha256" | "sha384" | "sha512" | "crc32" -> lower-case hex ("" on error)
std::string fileChecksum(const std::string& path, const std::string& algo, const std::atomic_bool* stop = nullptr);
// Which checksum a hex string is, by its length ("" = not a checksum)
std::string checksumKind(const std::string& hexText);
Bytes hmacSha256(const Bytes& key, const void* data, size_t len);
std::string hex(const Bytes& b);
std::string b64(const Bytes& b, bool url = false, bool pad = true);
Bytes unb64(const std::string& s);   // accepts both alphabets, with or without padding

// RSA-SHA256 PKCS#1 v1.5 with DDM's own public key (licences, signed update / store files, exe integrity).
bool verifyDdm(const void* data, size_t len, const Bytes& sig);
// Signed files on dedseec.com (stores.json, update.json): DedSec's configuration key.
bool verifyConfig(const void* data, size_t len, const Bytes& sig);
// The same with another public key (modulus big-endian, exponent big-endian).
bool verifyRsa(const Bytes& modulus, const Bytes& exponent, const void* data, size_t len, const Bytes& sig);

Bytes random(size_t n);
// Compares secrets in the same time whatever they hold (nobody can guess one by timing the answers).
bool sameSecret(const std::string& a, const std::string& b);

} // namespace crypto
} // namespace ddm

namespace ddm {
namespace crypto {
// AES-256-CBC with PKCS#7 padding (Windows CNG).
bool aesCbc(bool encrypt, const Bytes& key32, const Bytes& iv16, const Bytes& in, Bytes& out);
} // namespace crypto
} // namespace ddm
