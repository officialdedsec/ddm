#include "vault.hpp"
#include "crypto.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <sddl.h>
#include <wincrypt.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <shellapi.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace ddm {
namespace vault {

namespace {

typedef crypto::Bytes Bytes;
const int kIter = 150000;
const char kMagic[4] = {'D', 'D', 'M', 'V'};

std::mutex g_mu;
// the open vault
bool g_open = false;
bool g_decoy = false;
int g_slot = -1;
Bytes g_master;           // 64: enc + mac
Bytes g_privBlob;         // RSA private key (CNG blob)
std::string g_folder;
std::vector<std::string> g_temps;

std::string root() { return joinPath(dataDir(), "vault"); }
std::string headerFile() { return joinPath(root(), "vault.dat"); }

// ---------------------------------------------------------------- protection of the folder
void protectFolder(const std::string& dir) {
    makeDirs(dir);
    std::wstring w = W(dir);
    SetFileAttributesW(w.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_DIRECTORY);
    // only this Windows user (no other accounts, no Users / Everyone), not inherited from the parent
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> buf(len);
    if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len)) {
        PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = GENERIC_ALL;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &ea, nullptr, &acl) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW(&w[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
            LocalFree(acl);
        }
    }
    CloseHandle(tok);
}

void hideFile(const std::string& p) { SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED); }

// ---------------------------------------------------------------- crypto helpers
Bytes pbkdf2(const std::string& pw, const Bytes& salt, int iter) {
    BCRYPT_ALG_HANDLE a = nullptr;
    Bytes out(64);
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != STATUS_SUCCESS) return Bytes();
    NTSTATUS st = BCryptDeriveKeyPBKDF2(a, reinterpret_cast<PUCHAR>(const_cast<char*>(pw.data())), static_cast<ULONG>(pw.size()), const_cast<PUCHAR>(salt.data()), static_cast<ULONG>(salt.size()), static_cast<ULONGLONG>(iter), out.data(), 64, 0);
    BCryptCloseAlgorithmProvider(a, 0);
    return st == STATUS_SUCCESS ? out : Bytes();
}

Bytes sub(const Bytes& b, size_t from, size_t n) { return Bytes(b.begin() + static_cast<long>(from), b.begin() + static_cast<long>(from + n)); }

// AES-256-CBC + HMAC-SHA256 of a small value with a 64-byte key → iv | ct | mac
Bytes seal(const Bytes& key64, const Bytes& plain) {
    Bytes iv = crypto::random(16), ct;
    crypto::aesCbc(true, sub(key64, 0, 32), iv, plain, ct);
    Bytes out = iv;
    out.insert(out.end(), ct.begin(), ct.end());
    Bytes mac = crypto::hmacSha256(sub(key64, 32, 32), out.data(), out.size());
    out.insert(out.end(), mac.begin(), mac.end());
    return out;
}

bool open(const Bytes& key64, const Bytes& sealed, Bytes& plain) {
    if (sealed.size() < 16 + 16 + 32) return false;
    size_t n = sealed.size() - 32;
    Bytes mac = crypto::hmacSha256(sub(key64, 32, 32), sealed.data(), n);
    int r = 0;
    for (size_t i = 0; i < 32; i++) r |= mac[i] ^ sealed[n + i];
    if (r) return false;
    return crypto::aesCbc(false, sub(key64, 0, 32), sub(sealed, 0, 16), Bytes(sealed.begin() + 16, sealed.begin() + static_cast<long>(n)), plain);
}

// RSA-2048 OAEP (SHA-256), with the same built-in library everywhere (Windows 7 to 11 behave the same).
int rng(void*, unsigned char* out, size_t n) { return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == STATUS_SUCCESS ? 0 : -1; }

bool rsaNew(Bytes& pub, Bytes& priv) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0 && mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk), rng, nullptr, 2048, 65537) == 0;
    if (ok) {
        std::vector<unsigned char> buf(4096);
        int n = mbedtls_pk_write_pubkey_der(&pk, buf.data(), buf.size());
        if (n > 0) pub.assign(buf.end() - n, buf.end()); else ok = false;
        n = mbedtls_pk_write_key_der(&pk, buf.data(), buf.size());
        if (n > 0) priv.assign(buf.end() - n, buf.end()); else ok = false;
        std::fill(buf.begin(), buf.end(), 0);
    }
    mbedtls_pk_free(&pk);
    return ok;
}

bool rsa(bool encrypt, const Bytes& keyDer, const Bytes& in, Bytes& out) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    out.clear();
    int rc = encrypt ? mbedtls_pk_parse_public_key(&pk, keyDer.data(), keyDer.size()) : mbedtls_pk_parse_key(&pk, keyDer.data(), keyDer.size(), nullptr, 0, rng, nullptr);
    bool ok = false;
    if (rc == 0 && mbedtls_pk_get_type(&pk) == MBEDTLS_PK_RSA) {
        mbedtls_rsa_context* r = mbedtls_pk_rsa(pk);
        mbedtls_rsa_set_padding(r, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
        out.resize(512);
        if (encrypt) ok = mbedtls_rsa_rsaes_oaep_encrypt(r, rng, nullptr, nullptr, 0, in.size(), in.data(), out.data()) == 0, out.resize(ok ? mbedtls_rsa_get_len(r) : 0);
        else {
            size_t n = 0;
            ok = in.size() == mbedtls_rsa_get_len(r) && mbedtls_rsa_rsaes_oaep_decrypt(r, rng, nullptr, nullptr, 0, &n, in.data(), out.data(), out.size()) == 0;
            out.resize(ok ? n : 0);
        }
    }
    mbedtls_pk_free(&pk);
    return ok;
}

// Streams AES-256-CBC with CNG (the IV carries over between calls); padding only on the last block.
struct Aes {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    Bytes iv;
    explicit Aes(const Bytes& key32, const Bytes& iv16) : iv(iv16) {
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_AES_ALGORITHM, nullptr, 0) != STATUS_SUCCESS) return;
        BCryptSetProperty(a, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)), sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
        BCryptGenerateSymmetricKey(a, &k, nullptr, 0, const_cast<PUCHAR>(key32.data()), 32, 0);
    }
    ~Aes() { if (k) BCryptDestroyKey(k); if (a) BCryptCloseAlgorithmProvider(a, 0); }
    bool run(bool enc, const unsigned char* in, size_t n, Bytes& out, bool last) {
        ULONG flags = last ? BCRYPT_BLOCK_PADDING : 0, got = 0;
        out.resize(n + 32);
        NTSTATUS st = enc ? BCryptEncrypt(k, const_cast<PUCHAR>(in), static_cast<ULONG>(n), nullptr, iv.data(), 16, out.data(), static_cast<ULONG>(out.size()), &got, flags)
                          : BCryptDecrypt(k, const_cast<PUCHAR>(in), static_cast<ULONG>(n), nullptr, iv.data(), 16, out.data(), static_cast<ULONG>(out.size()), &got, flags);
        out.resize(st == STATUS_SUCCESS ? got : 0);
        return st == STATUS_SUCCESS;
    }
};

struct Hmac {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    explicit Hmac(const Bytes& key) {
        BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        BCryptCreateHash(a, &h, nullptr, 0, const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    }
    ~Hmac() { if (h) BCryptDestroyHash(h); if (a) BCryptCloseAlgorithmProvider(a, 0); }
    void add(const void* p, size_t n) { BCryptHashData(h, static_cast<PUCHAR>(const_cast<void*>(p)), static_cast<ULONG>(n), 0); }
    Bytes done() { Bytes m(32); BCryptFinishHash(h, m.data(), 32, 0); return m; }
};

// ---------------------------------------------------------------- header (protected with the Windows account: DPAPI)
struct Slot { std::string folder; Bytes salt, wrapped, priv, pub; };
struct Header { std::vector<Slot> slots; int drop = 0; int fails = 0; int64_t lockUntil = 0; };

Bytes dpapi(bool protect, const Bytes& in) {
    DATA_BLOB i{static_cast<DWORD>(in.size()), const_cast<BYTE*>(in.data())}, o{}, e{};
    const char* ent = "DDM-vault-v1";
    e.cbData = static_cast<DWORD>(strlen(ent));
    e.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(ent));
    BOOL ok = protect ? CryptProtectData(&i, L"DDM", &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &o) : CryptUnprotectData(&i, nullptr, &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &o);
    Bytes r;
    if (ok) { r.assign(o.pbData, o.pbData + o.cbData); LocalFree(o.pbData); }
    return r;
}

bool readHeader(Header& h) {
    std::string raw;
    if (!readFile(headerFile(), raw)) return false;
    Bytes plain = dpapi(false, Bytes(raw.begin(), raw.end()));
    if (plain.empty()) return false;
    bool ok = false;
    Json j = Json::parse(std::string(plain.begin(), plain.end()), &ok);
    if (!ok) return false;
    for (auto& s : j["slots"].list()) h.slots.push_back({s["folder"].str(), crypto::unb64(s["salt"].str()), crypto::unb64(s["wrapped"].str()), crypto::unb64(s["priv"].str()), crypto::unb64(s["pub"].str())});
    h.drop = j["drop"].i32();
    h.fails = j["fails"].i32();
    h.lockUntil = j["lockUntil"].i64();
    return h.slots.size() == 2;
}

bool writeHeader(const Header& h) {
    Json j = Json::object();
    Json arr = Json::array();
    for (auto& s : h.slots) {
        Json o = Json::object();
        o["folder"] = s.folder; o["salt"] = crypto::b64(s.salt); o["wrapped"] = crypto::b64(s.wrapped); o["priv"] = crypto::b64(s.priv); o["pub"] = crypto::b64(s.pub);
        arr.push(o);
    }
    j["slots"] = arr; j["drop"] = h.drop; j["fails"] = h.fails; j["lockUntil"] = h.lockUntil; j["v"] = 1;
    std::string t = j.dump();
    Bytes enc = dpapi(true, Bytes(t.begin(), t.end()));
    if (enc.empty()) return false;
    protectFolder(root());
    bool ok = writeFileAtomic(headerFile(), std::string(enc.begin(), enc.end()));
    hideFile(headerFile());
    return ok;
}

// A fresh slot for a password. role: "main" or "decoy"; decoySet is remembered inside the real vault.
bool makeSlot(const std::string& pw, const std::string& role, bool decoySet, Slot& s, Bytes* masterOut = nullptr, Bytes* privOut = nullptr) {
    s.folder = crypto::hex(crypto::random(6));
    s.salt = crypto::random(16);
    Bytes kek = pbkdf2(pw, s.salt, kIter);
    if (kek.empty()) return false;
    Bytes master = crypto::random(64), priv;
    if (!rsaNew(s.pub, priv)) return false;
    s.wrapped = seal(kek, master);
    Json inner = Json::object();
    inner["role"] = role; inner["decoySet"] = decoySet; inner["key"] = crypto::b64(priv);
    std::string it = inner.dump();
    s.priv = seal(master, Bytes(it.begin(), it.end()));
    protectFolder(root());
    makeDirs(joinPath(root(), s.folder));
    if (masterOut) *masterOut = master;
    if (privOut) *privOut = priv;
    return true;
}

bool openSlot(const Slot& s, const std::string& pw, Bytes& master, Json& inner) {
    Bytes kek = pbkdf2(pw, s.salt, kIter), m, p;
    if (kek.empty() || !open(kek, s.wrapped, m) || m.size() != 64) return false;
    if (!open(m, s.priv, p)) return false;
    bool ok = false;
    inner = Json::parse(std::string(p.begin(), p.end()), &ok);
    if (!ok) return false;
    master = m;
    return true;
}

void wipeFile(const std::string& p) {
    int64_t n = fileSize(p);
    if (n > 0 && n <= 64LL * 1024 * 1024) {   // overwrite small files before deleting
        HANDLE f = CreateFileW(W(p).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<char> z(1 << 20, 0);
            int64_t left = n;
            DWORD put = 0;
            while (left > 0) { DWORD c = static_cast<DWORD>(std::min<int64_t>(left, static_cast<int64_t>(z.size()))); if (!WriteFile(f, z.data(), c, &put, nullptr)) break; left -= c; }
            FlushFileBuffers(f);
            CloseHandle(f);
        }
    }
    SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(W(p).c_str());
}

void wipeFolder(const std::string& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(W(joinPath(dir, "*")).c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::string n = U(fd.cFileName);
            if (n == "." || n == "..") continue;
            std::string p = joinPath(dir, n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) wipeFolder(p);
            else wipeFile(p);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    SetFileAttributesW(W(dir).c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(W(dir).c_str());
}

// ---------------------------------------------------------------- the encrypted file format
// "DDMV" 1 | u16 wrapLen | RSA-OAEP(fileKey64) | metaIv16 | u32 metaLen | metaCt | dataIv16 | dataCt… | HMAC32
struct FileHead { Bytes fileKey; Json meta; int64_t dataStart = 0; Bytes dataIv; };

bool readHead(HANDLE f, const Bytes& priv, FileHead& fh) {
    unsigned char hdr[7];
    DWORD got = 0;
    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(f, hdr, 7, &got, nullptr) || got != 7 || memcmp(hdr, kMagic, 4) != 0 || hdr[4] != 1) return false;
    unsigned wl = hdr[5] | (hdr[6] << 8);
    if (wl == 0 || wl > 1024) return false;
    Bytes wrap(wl);
    if (!ReadFile(f, wrap.data(), wl, &got, nullptr) || got != wl) return false;
    if (!rsa(false, priv, wrap, fh.fileKey) || fh.fileKey.size() != 64) return false;
    unsigned char mi[20];
    if (!ReadFile(f, mi, 20, &got, nullptr) || got != 20) return false;
    uint32_t ml = mi[16] | (mi[17] << 8) | (mi[18] << 16) | (static_cast<uint32_t>(mi[19]) << 24);
    if (ml == 0 || ml > 1024 * 1024) return false;
    Bytes mc(ml), mp;
    if (!ReadFile(f, mc.data(), ml, &got, nullptr) || got != ml) return false;
    if (!crypto::aesCbc(false, sub(fh.fileKey, 0, 32), Bytes(mi, mi + 16), mc, mp)) return false;
    bool ok = false;
    fh.meta = Json::parse(std::string(mp.begin(), mp.end()), &ok);
    if (!ok) return false;
    fh.dataIv.resize(16);
    if (!ReadFile(f, fh.dataIv.data(), 16, &got, nullptr) || got != 16) return false;
    fh.dataStart = 7 + wl + 20 + ml + 16;
    return true;
}

std::string itemPath(const std::string& id) { return joinPath(g_folder, id); }
bool validId(const std::string& id) { return !id.empty() && id.find_first_of("\\/:") == std::string::npos && endsWith(id, ".dvf"); }

} // namespace

// ================================================================ public
bool isSetUp() { Header h; return readHeader(h); }

bool create(const std::string& pw, std::string& error) {
    std::lock_guard<std::mutex> g(g_mu);
    if (pw.size() < 4) { error = "Use at least 4 characters."; return false; }
    Header h;
    Slot real, dummy;
    if (!makeSlot(pw, "main", false, real)) { error = "Couldn't create the vault."; return false; }
    // the second slot is always there: a password nobody knows, until a decoy password is chosen
    if (!makeSlot(crypto::hex(crypto::random(24)), "decoy", false, dummy)) { error = "Couldn't create the vault."; return false; }
    bool realFirst = (crypto::random(1)[0] & 1) != 0;
    h.slots = realFirst ? std::vector<Slot>{real, dummy} : std::vector<Slot>{dummy, real};
    h.drop = realFirst ? 0 : 1;
    if (!writeHeader(h)) { error = "Couldn't save the vault."; return false; }
    return true;
}

int64_t lockedForSeconds() {
    Header h;
    if (!readHeader(h)) return 0;
    int64_t left = (h.lockUntil - nowMs()) / 1000;
    return left > 0 ? left : 0;
}

bool unlock(const std::string& pw, std::string& error) {
    std::lock_guard<std::mutex> g(g_mu);
    Header h;
    if (!readHeader(h)) { error = "The vault can't be opened on this Windows account."; return false; }
    if (h.lockUntil > nowMs()) { error = "Too many wrong passwords. Try again in " + std::to_string((h.lockUntil - nowMs()) / 1000 + 1) + " seconds."; return false; }
    int found = -1;
    Bytes master;
    Json inner;
    for (int i = 0; i < 2; i++) {   // both slots are always tried, so the time taken doesn't tell which one opened
        Bytes m;
        Json in;
        if (openSlot(h.slots[static_cast<size_t>(i)], pw, m, in) && found < 0) { found = i; master = m; inner = in; }
    }
    if (found < 0) {
        h.fails++;
        if (h.fails >= 5) h.lockUntil = nowMs() + std::min<int64_t>(3600, 30LL << std::min(h.fails - 5, 7)) * 1000;
        writeHeader(h);
        error = h.fails >= 5 ? "Wrong password. The vault is locked for a while." : "Wrong password.";
        return false;
    }
    h.fails = 0;
    h.lockUntil = 0;
    writeHeader(h);
    g_open = true;
    g_slot = found;
    g_decoy = inner["role"].str() == "decoy";
    g_master = master;
    g_privBlob = crypto::unb64(inner["key"].str());
    g_folder = joinPath(root(), h.slots[static_cast<size_t>(found)].folder);
    makeDirs(g_folder);
    return true;
}

bool isUnlocked() { std::lock_guard<std::mutex> g(g_mu); return g_open; }
bool isDecoy() { std::lock_guard<std::mutex> g(g_mu); return g_open && g_decoy; }

void clearTemp() {
    std::string t = joinPath(root(), "t");
    if (dirExists(t)) wipeFolder(t);
}

void lock() {
    {
        std::lock_guard<std::mutex> g(g_mu);
        g_open = false;
        g_decoy = false;
        g_slot = -1;
        std::fill(g_master.begin(), g_master.end(), 0);
        std::fill(g_privBlob.begin(), g_privBlob.end(), 0);
        g_master.clear();
        g_privBlob.clear();
        g_folder.clear();
        g_temps.clear();
    }
    clearTemp();
}

bool changePassword(const std::string& oldPw, const std::string& newPw, std::string& error) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_open) { error = "Open the vault first."; return false; }
    if (newPw.size() < 4) { error = "Use at least 4 characters."; return false; }
    Header h;
    if (!readHeader(h)) { error = "The vault can't be read."; return false; }
    Slot& s = h.slots[static_cast<size_t>(g_slot)];
    Bytes m;
    Json in;
    if (!openSlot(s, oldPw, m, in)) { error = "The current password is wrong."; return false; }
    Bytes other;
    Json oi;
    if (openSlot(h.slots[static_cast<size_t>(1 - g_slot)], newPw, other, oi)) { error = "Choose a different password."; return false; }
    s.salt = crypto::random(16);
    Bytes kek = pbkdf2(newPw, s.salt, kIter);
    s.wrapped = seal(kek, m);   // same keys: every file stays readable
    if (!writeHeader(h)) { error = "Couldn't save the vault."; return false; }
    return true;
}

bool hasDecoyPassword() {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_open || g_decoy) return false;
    Header h;
    if (!readHeader(h)) return false;
    Bytes p;
    if (!open(g_master, h.slots[static_cast<size_t>(g_slot)].priv, p)) return false;
    return Json::parse(std::string(p.begin(), p.end()))["decoySet"].boolean();
}

static bool remakeDecoy(const std::string& pw, bool set, std::string& error) {
    if (!g_open || g_decoy) { error = "Open the real vault first."; return false; }
    Header h;
    if (!readHeader(h)) { error = "The vault can't be read."; return false; }
    if (set) {
        Bytes m;
        Json in;
        if (openSlot(h.slots[static_cast<size_t>(g_slot)], pw, m, in)) { error = "The decoy password must be different from the real one."; return false; }
    }
    int d = 1 - g_slot;
    std::string oldFolder = joinPath(root(), h.slots[static_cast<size_t>(d)].folder);
    Slot ns;
    if (!makeSlot(set ? pw : crypto::hex(crypto::random(24)), "decoy", false, ns)) { error = "Couldn't make the decoy vault."; return false; }
    h.slots[static_cast<size_t>(d)] = ns;
    // remember inside the real vault whether a decoy password exists
    Bytes p;
    if (open(g_master, h.slots[static_cast<size_t>(g_slot)].priv, p)) {
        Json inner = Json::parse(std::string(p.begin(), p.end()));
        inner["decoySet"] = set;
        std::string it = inner.dump();
        h.slots[static_cast<size_t>(g_slot)].priv = seal(g_master, Bytes(it.begin(), it.end()));
    }
    if (!writeHeader(h)) { error = "Couldn't save the vault."; return false; }
    if (dirExists(oldFolder)) wipeFolder(oldFolder);
    return true;
}

bool setDecoy(const std::string& pw, std::string& error) {
    std::lock_guard<std::mutex> g(g_mu);
    if (pw.size() < 4) { error = "Use at least 4 characters."; return false; }
    return remakeDecoy(pw, true, error);
}

bool removeDecoy(std::string& error) { std::lock_guard<std::mutex> g(g_mu); return remakeDecoy("", false, error); }

std::vector<Item> list() {
    std::vector<Item> out;
    std::string folder;
    Bytes priv;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_open) return out;
        folder = g_folder;
        priv = g_privBlob;
    }
    WIN32_FIND_DATAW fd;
    HANDLE ff = FindFirstFileW(W(joinPath(folder, "*.dvf")).c_str(), &fd);
    if (ff == INVALID_HANDLE_VALUE) return out;
    do {
        std::string id = U(fd.cFileName);
        HANDLE f = CreateFileW(W(joinPath(folder, id)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        FileHead fh;
        if (readHead(f, priv, fh)) out.push_back({id, fh.meta["name"].str(), fh.meta["size"].i64(), fh.meta["added"].i64(), fh.meta["from"].str()});
        CloseHandle(f);
    } while (FindNextFileW(ff, &fd));
    FindClose(ff);
    std::sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.added > b.added; });
    return out;
}

bool add(const std::string& path, bool move, std::string& error, const std::string& from, std::function<void(int64_t, int64_t)> progress) {
    Bytes pub;
    std::string folder;
    {
        std::lock_guard<std::mutex> g(g_mu);
        Header h;
        if (!readHeader(h)) { error = "Set up the private vault first (Tools → Private vault)."; return false; }
        int slot = g_open ? g_slot : h.drop;   // locked: into the real vault, without needing its password
        pub = h.slots[static_cast<size_t>(slot)].pub;
        folder = joinPath(root(), h.slots[static_cast<size_t>(slot)].folder);
    }
    HANDLE in = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) { error = "Couldn't open " + baseName(path) + " (is it open in another program?)."; return false; }
    LARGE_INTEGER sz{};
    GetFileSizeEx(in, &sz);
    Bytes fileKey = crypto::random(64), wrap;
    if (!rsa(true, pub, fileKey, wrap)) { CloseHandle(in); error = "Encryption failed."; return false; }
    Json meta = Json::object();
    meta["name"] = baseName(path); meta["size"] = static_cast<int64_t>(sz.QuadPart); meta["added"] = nowMs(); meta["from"] = from;
    std::string mt = meta.dump();
    Bytes metaIv = crypto::random(16), metaCt;
    crypto::aesCbc(true, sub(fileKey, 0, 32), metaIv, Bytes(mt.begin(), mt.end()), metaCt);
    makeDirs(folder);
    std::string id = crypto::hex(crypto::random(8)) + ".dvf";
    std::string tmp = joinPath(folder, id + ".part"), dst = joinPath(folder, id);
    HANDLE out = CreateFileW(W(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); error = "Couldn't write into the vault."; return false; }
    Hmac mac(sub(fileKey, 32, 32));
    bool ok = true;
    auto put = [&](const void* p, size_t n) { DWORD w = 0; if (!WriteFile(out, p, static_cast<DWORD>(n), &w, nullptr) || w != n) ok = false; mac.add(p, n); };
    unsigned char hdr[7] = {'D', 'D', 'M', 'V', 1, static_cast<unsigned char>(wrap.size() & 0xFF), static_cast<unsigned char>(wrap.size() >> 8)};
    put(hdr, 7);
    put(wrap.data(), wrap.size());
    put(metaIv.data(), 16);
    uint32_t ml = static_cast<uint32_t>(metaCt.size());
    unsigned char mlb[4] = {static_cast<unsigned char>(ml), static_cast<unsigned char>(ml >> 8), static_cast<unsigned char>(ml >> 16), static_cast<unsigned char>(ml >> 24)};
    put(mlb, 4);
    put(metaCt.data(), metaCt.size());
    Bytes dataIv = crypto::random(16);
    put(dataIv.data(), 16);
    Aes aes(sub(fileKey, 0, 32), dataIv);
    std::vector<unsigned char> buf(1 << 20);
    Bytes ct;
    int64_t done = 0;
    DWORD got = 0;
    while (ok) {
        if (!ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) { ok = false; break; }
        bool last = done + got >= sz.QuadPart || got == 0;
        if (!aes.run(true, buf.data(), got, ct, last)) { ok = false; break; }
        if (!ct.empty()) put(ct.data(), ct.size());
        done += got;
        if (progress) progress(done, sz.QuadPart);
        if (last) break;
    }
    Bytes m = mac.done();
    DWORD w = 0;
    if (ok && (!WriteFile(out, m.data(), 32, &w, nullptr) || w != 32)) ok = false;
    FlushFileBuffers(out);
    CloseHandle(out);
    CloseHandle(in);
    if (!ok || !MoveFileExW(W(tmp).c_str(), W(dst).c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(W(tmp).c_str()); error = "Couldn't encrypt " + baseName(path) + " (is the disk full?)."; return false; }
    hideFile(dst);
    if (move) wipeFile(path);
    return true;
}

bool extract(const std::string& id, const std::string& destPath, std::string& error, std::function<void(int64_t, int64_t)> progress) {
    std::string path;
    Bytes priv;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_open) { error = "The vault is locked."; return false; }
        if (!validId(id)) { error = "Not in the vault."; return false; }
        path = itemPath(id);
        priv = g_privBlob;
    }
    HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { error = "That file isn't in the vault any more."; return false; }
    FileHead fh;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    if (!readHead(f, priv, fh) || sz.QuadPart < fh.dataStart + 32) { CloseHandle(f); error = "This vault file is damaged."; return false; }
    // 1) check the whole file is intact (HMAC) before writing anything out
    {
        Hmac mac(sub(fh.fileKey, 32, 32));
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        std::vector<unsigned char> buf(1 << 20);
        int64_t left = sz.QuadPart - 32;
        DWORD got = 0;
        while (left > 0 && ReadFile(f, buf.data(), static_cast<DWORD>(std::min<int64_t>(left, static_cast<int64_t>(buf.size()))), &got, nullptr) && got) { mac.add(buf.data(), got); left -= got; }
        Bytes want = mac.done(), have(32);
        ReadFile(f, have.data(), 32, &got, nullptr);
        if (left != 0 || want != have) { CloseHandle(f); error = "This vault file was changed or damaged; it can't be opened."; return false; }
    }
    // 2) decrypt
    LARGE_INTEGER pos;
    pos.QuadPart = fh.dataStart;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    makeDirs(dirName(destPath));
    std::string tmp = destPath + ".ddmvault";
    HANDLE out = CreateFileW(W(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(f); error = "Couldn't write to " + dirName(destPath) + "."; return false; }
    Aes aes(sub(fh.fileKey, 0, 32), fh.dataIv);
    std::vector<unsigned char> buf(1 << 20);
    Bytes pt;
    int64_t left = sz.QuadPart - 32 - fh.dataStart, done = 0, total = fh.meta["size"].i64();
    bool ok = true;
    while (left > 0 && ok) {
        DWORD want = static_cast<DWORD>(std::min<int64_t>(left, static_cast<int64_t>(buf.size()))), got = 0;
        if (!ReadFile(f, buf.data(), want, &got, nullptr) || got != want) { ok = false; break; }
        left -= got;
        if (!aes.run(false, buf.data(), got, pt, left == 0)) { ok = false; break; }
        DWORD w = 0;
        if (!pt.empty() && (!WriteFile(out, pt.data(), static_cast<DWORD>(pt.size()), &w, nullptr) || w != pt.size())) ok = false;
        done += static_cast<int64_t>(pt.size());
        if (progress) progress(done, total);
    }
    CloseHandle(out);
    CloseHandle(f);
    if (!ok || !MoveFileExW(W(tmp).c_str(), W(destPath).c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(W(tmp).c_str()); error = "Couldn't decrypt the file."; return false; }
    return true;
}

bool readAll(const std::string& id, std::string& data, std::string& error, int64_t maxBytes) {
    std::string t = joinPath(joinPath(root(), "t"), crypto::hex(crypto::random(8)));
    protectFolder(root());
    makeDirs(dirName(t));
    if (!extract(id, t, error)) return false;
    bool ok = fileSize(t) <= maxBytes && readFile(t, data);
    wipeFile(t);
    if (!ok) error = "The file is too large to show here.";
    return ok;
}

std::string openCopy(const std::string& id, std::string& error) {
    std::string name;
    for (auto& it : list()) if (it.id == id) name = it.name;
    if (name.empty()) { error = "That file isn't in the vault any more."; return ""; }
    protectFolder(root());
    std::string dir = joinPath(joinPath(root(), "t"), crypto::hex(crypto::random(6)));
    makeDirs(dir);
    std::string dest = joinPath(dir, sanitizeFileName(name));
    if (!extract(id, dest, error)) return "";
    std::lock_guard<std::mutex> g(g_mu);
    g_temps.push_back(dest);
    return dest;
}

bool remove(const std::string& id, std::string& error) {
    std::string p;
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (!g_open || !validId(id)) { error = "The vault is locked."; return false; }
        p = itemPath(id);
    }
    if (!fileExists(p)) { error = "That file isn't in the vault any more."; return false; }
    wipeFile(p);
    return !fileExists(p);
}

bool rename(const std::string& id, const std::string& newName, std::string& error) {
    // re-encrypts the details part: simplest is extract → add with the new name (the same file key isn't reused)
    std::string t = joinPath(joinPath(root(), "t"), crypto::hex(crypto::random(6)));
    makeDirs(t);
    std::string dest = joinPath(t, sanitizeFileName(newName));
    if (!extract(id, dest, error)) return false;
    if (!add(dest, true, error)) return false;
    return remove(id, error);
}

} // namespace vault
} // namespace ddm
