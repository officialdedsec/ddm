#include "crypto.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <cstring>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace ddm {
namespace crypto {

namespace {
const unsigned char MOD[256] = { 203,54,68,114,9,1,74,177,131,207,111,232,118,53,246,113,251,61,190,184,108,132,70,66,190,139,199,106,110,24,218,50,137,10,176,38,89,187,139,134,227,245,210,49,3,212,225,64,29,187,13,81,35,249,154,87,180,68,76,107,218,203,137,181,71,17,10,140,237,96,174,83,255,222,81,28,113,241,79,213,89,99,67,177,24,105,145,123,10,126,67,243,79,95,239,72,226,197,86,87,236,154,185,25,146,183,125,29,190,156,124,225,100,42,105,105,231,37,209,84,118,246,67,50,225,12,60,102,88,244,253,160,152,192,18,113,229,68,97,82,36,115,209,159,23,148,41,125,205,251,182,38,140,68,32,198,162,192,147,175,69,44,148,64,16,16,223,244,5,1,132,164,198,239,187,232,11,113,70,120,123,123,128,52,213,201,189,209,142,248,216,56,235,98,247,70,90,37,9,208,92,183,248,160,247,95,134,198,104,134,199,50,223,66,55,165,107,250,6,171,18,153,136,111,10,17,132,249,61,178,203,21,222,30,80,247,107,92,158,165,146,123,136,90,45,148,212,134,90,18,133,230,239,136,166,97 };
const unsigned char EXPN[3] = {0x01, 0x00, 0x01};

Bytes hashWith(const wchar_t* algo, const Bytes* hmacKey, const void* data, size_t len) {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Bytes out;
    if (BCryptOpenAlgorithmProvider(&a, algo, nullptr, hmacKey ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) != STATUS_SUCCESS) return out;
    DWORD hl = 0, got = 0;
    BCryptGetProperty(a, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hl), sizeof hl, &got, 0);
    if (BCryptCreateHash(a, &h, nullptr, 0, hmacKey ? const_cast<PUCHAR>(hmacKey->data()) : nullptr, hmacKey ? static_cast<ULONG>(hmacKey->size()) : 0, 0) == STATUS_SUCCESS) {
        out.resize(hl);
        if (BCryptHashData(h, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(len), 0) != STATUS_SUCCESS ||
            BCryptFinishHash(h, out.data(), hl, 0) != STATUS_SUCCESS) out.clear();
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return out;
}
} // namespace

Bytes sha256(const void* data, size_t len) { return hashWith(BCRYPT_SHA256_ALGORITHM, nullptr, data, len); }
Bytes hmacSha256(const Bytes& key, const void* data, size_t len) { return hashWith(BCRYPT_SHA256_ALGORITHM, &key, data, len); }
Bytes md5(const void* data, size_t len) { return hashWith(BCRYPT_MD5_ALGORITHM, nullptr, data, len); }

Bytes hashFile(const std::string& path, const wchar_t* algo, const std::atomic_bool* stop) {
    Bytes out;
    HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, algo, nullptr, 0) == STATUS_SUCCESS) {
        DWORD hl = 0, got = 0;
        BCryptGetProperty(a, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hl), sizeof hl, &got, 0);
        if (BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == STATUS_SUCCESS) {
            std::vector<unsigned char> buf(1 << 20);
            DWORD n = 0;
            bool ok = true;
            while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n) {
                if (stop && stop->load()) { ok = false; break; }
                if (BCryptHashData(h, buf.data(), n, 0) != STATUS_SUCCESS) { ok = false; break; }
            }
            if (ok) { out.resize(hl); if (BCryptFinishHash(h, out.data(), hl, 0) != STATUS_SUCCESS) out.clear(); }
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(a, 0);
    }
    CloseHandle(f);
    return out;
}

std::string hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

std::string b64(const Bytes& b, bool url, bool pad) {
    const char* a = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    for (; i + 2 < b.size(); i += 3) { unsigned v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2]; o += a[v >> 18]; o += a[(v >> 12) & 63]; o += a[(v >> 6) & 63]; o += a[v & 63]; }
    if (b.size() - i == 1) { unsigned v = b[i] << 16; o += a[v >> 18]; o += a[(v >> 12) & 63]; if (pad) o += "=="; }
    else if (b.size() - i == 2) { unsigned v = (b[i] << 16) | (b[i + 1] << 8); o += a[v >> 18]; o += a[(v >> 12) & 63]; o += a[(v >> 6) & 63]; if (pad) o += "="; }
    return o;
}

Bytes unb64(const std::string& s) {
    static int T[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) T[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(a[i])] = i;
        T[static_cast<unsigned char>('-')] = 62; T[static_cast<unsigned char>('_')] = 63;
        init = true;
    }
    Bytes out;
    int val = 0, bits = 0;
    for (char ch : s) {
        if (ch == '=' || ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        int v = T[static_cast<unsigned char>(ch)];
        if (v < 0) return Bytes();
        val = (val << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF)); }
    }
    return out;
}

bool verifyRsa(const Bytes& mod, const Bytes& expn, const void* data, size_t len, const Bytes& sig) {
    if (mod.empty() || expn.empty() || sig.empty()) return false;
    Bytes hash = sha256(data, len);
    if (hash.size() != 32) return false;
    Bytes blob(sizeof(BCRYPT_RSAKEY_BLOB) + expn.size() + mod.size());
    auto hdr = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    hdr->Magic = BCRYPT_RSAPUBLIC_MAGIC;
    hdr->BitLength = static_cast<ULONG>(mod.size() * 8);
    hdr->cbPublicExp = static_cast<ULONG>(expn.size());
    hdr->cbModulus = static_cast<ULONG>(mod.size());
    hdr->cbPrime1 = hdr->cbPrime2 = 0;
    memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB), expn.data(), expn.size());
    memcpy(blob.data() + sizeof(BCRYPT_RSAKEY_BLOB) + expn.size(), mod.data(), mod.size());
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_RSA_ALGORITHM, nullptr, 0) != STATUS_SUCCESS) return false;
    if (BCryptImportKeyPair(a, nullptr, BCRYPT_RSAPUBLIC_BLOB, &k, blob.data(), static_cast<ULONG>(blob.size()), 0) == STATUS_SUCCESS) {
        BCRYPT_PKCS1_PADDING_INFO pad;
        pad.pszAlgId = BCRYPT_SHA256_ALGORITHM;
        ok = BCryptVerifySignature(k, &pad, hash.data(), 32, const_cast<PUCHAR>(sig.data()), static_cast<ULONG>(sig.size()), BCRYPT_PAD_PKCS1) == STATUS_SUCCESS;
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

bool verifyDdm(const void* data, size_t len, const Bytes& sig) {
    return verifyRsa(Bytes(MOD, MOD + sizeof MOD), Bytes(EXPN, EXPN + 3), data, len, sig);
}

bool verifyConfig(const void* data, size_t len, const Bytes& sig) {
    static const Bytes mod = unb64("nOt4Oq11CmtdvONZ8kLDiAaH2mGZooxcfTXJAfE3KMQ8sOdkz5c1ed7zFvG7HVQuE6Ks2shnDLFJ3wsVKBUx78YCJNbaCF7zmhe7gHLew7Rhbq4EDfXa2mlttGE7TEP3KY2PMATwpDdmKxZSaTBvlywgPBeDrWepsO4yt7uMi6URE90U5EICHgYgnWgK270TqZyx934Cj6ga+5l5jpEr2Oc4ZTpYYhw1I7KhM5ON2Py+Ejnj6M92AVhmfNSka9WyQYPvH3zjtTjEJdDREt7OXXYw+TDxBwcM/i9DwHQtvRkwX6rG15REQF4Blv4Cnebd2W4uk+lRC+VB8+zdPqdPbQ==");
    return verifyRsa(mod, Bytes(EXPN, EXPN + 3), data, len, sig);
}

Bytes random(size_t n) {
    Bytes b(n);
    BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return b;
}

} // namespace crypto
} // namespace ddm

namespace ddm {
namespace crypto {
bool aesCbc(bool encrypt, const Bytes& key, const Bytes& iv0, const Bytes& in, Bytes& out) {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    out.clear();
    if (key.size() != 32 || iv0.size() != 16) return false;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_AES_ALGORITHM, nullptr, 0) != STATUS_SUCCESS) return false;
    BCryptSetProperty(a, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    bool ok = false;
    if (BCryptGenerateSymmetricKey(a, &k, nullptr, 0, const_cast<PUCHAR>(key.data()), 32, 0) == STATUS_SUCCESS) {
        Bytes iv = iv0;
        ULONG n = 0;
        NTSTATUS st = encrypt ? BCryptEncrypt(k, const_cast<PUCHAR>(in.data()), static_cast<ULONG>(in.size()), nullptr, iv.data(), 16, nullptr, 0, &n, BCRYPT_BLOCK_PADDING)
                              : BCryptDecrypt(k, const_cast<PUCHAR>(in.data()), static_cast<ULONG>(in.size()), nullptr, iv.data(), 16, nullptr, 0, &n, BCRYPT_BLOCK_PADDING);
        if (st == STATUS_SUCCESS) {
            out.resize(n);
            iv = iv0;
            st = encrypt ? BCryptEncrypt(k, const_cast<PUCHAR>(in.data()), static_cast<ULONG>(in.size()), nullptr, iv.data(), 16, out.data(), n, &n, BCRYPT_BLOCK_PADDING)
                         : BCryptDecrypt(k, const_cast<PUCHAR>(in.data()), static_cast<ULONG>(in.size()), nullptr, iv.data(), 16, out.data(), n, &n, BCRYPT_BLOCK_PADDING);
            ok = st == STATUS_SUCCESS;
            out.resize(ok ? n : 0);
        }
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}
} // namespace crypto
} // namespace ddm

#include <miniz/miniz.h>
namespace ddm {
namespace crypto {
std::string checksumKind(const std::string& h0) {
    std::string h;
    for (char c : h0) if (!isspace(static_cast<unsigned char>(c))) h.push_back(static_cast<char>(tolower(static_cast<unsigned char>(c))));
    if (h.empty() || h.find_first_not_of("0123456789abcdef") != std::string::npos) return "";
    switch (h.size()) { case 8: return "crc32"; case 32: return "md5"; case 40: return "sha1"; case 64: return "sha256"; case 96: return "sha384"; case 128: return "sha512"; }
    return "";
}
std::string fileChecksum(const std::string& path, const std::string& algo, const std::atomic_bool* stop) {
    if (algo == "crc32") {
        HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE) return "";
        std::string buf(1 << 20, '\0');
        mz_ulong crc = MZ_CRC32_INIT;
        DWORD got = 0;
        bool ok = true;
        while ((ok = ReadFile(f, &buf[0], static_cast<DWORD>(buf.size()), &got, nullptr) != 0) && got) { if (stop && stop->load()) { ok = false; break; } crc = mz_crc32(crc, reinterpret_cast<const unsigned char*>(buf.data()), got); }
        CloseHandle(f);
        if (!ok) return "";
        char x[16]; snprintf(x, sizeof x, "%08lx", static_cast<unsigned long>(crc));
        return x;
    }
    const wchar_t* a = algo == "md5" ? L"MD5" : algo == "sha1" ? L"SHA1" : algo == "sha256" ? L"SHA256" : algo == "sha384" ? L"SHA384" : algo == "sha512" ? L"SHA512" : nullptr;
    if (!a) return "";
    Bytes b = hashFile(path, a, stop);
    return b.empty() ? "" : hex(b);
}
} // namespace crypto
} // namespace ddm

namespace ddm {
namespace crypto {
bool sameSecret(const std::string& a, const std::string& b) {
    if (a.empty() || a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}
} // namespace crypto
} // namespace ddm
