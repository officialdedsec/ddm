#include "mega.hpp"
#include "http.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <mbedtls/aes.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>

// File key = 8 words k[0..7]; AES key = k[0..3] XOR k[4..7]; nonce = k[4..5]; integrity code = k[6..7].
// Content is AES-128-CTR (counter = nonce + 64-bit block number), so any byte range decrypts on its own.

namespace ddm {

namespace {

typedef std::vector<unsigned char> Bytes;
const char* kApi = "https://g.api.mega.co.nz";
std::atomic<long long> g_seq{static_cast<long long>(GetTickCount64() % 1000000000ULL)};
const char* kQuota = "MEGA's free transfer limit for your connection is used up. Your progress is saved: press Resume later (MEGA frees it within a few hours) or switch network.";

struct Link {
    bool folder = false;
    std::string handle;
    Bytes key;           // 32 (file) or 16 (folder); empty when missing
    std::string child;
    int childIsFile = -1;
};

std::string hostOf(const std::string& url) {
    std::string h = lower(urlHost(url));
    return startsWith(h, "www.") ? h.substr(4) : h;
}

Bytes unb64(std::string s) {
    s = trim(s);
    std::string c;
    for (char ch : s) { if (ch == '-') c += '+'; else if (ch == '_') c += '/'; else if (ch == ',' || ch == '=') continue; else c += ch; }
    static int T[256];
    static bool init = false;
    if (!init) { for (int i = 0; i < 256; i++) T[i] = -1; const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(a[i])] = i; init = true; }
    Bytes out;
    int val = 0, bits = 0;
    for (char ch : c) {
        int v = T[static_cast<unsigned char>(ch)];
        if (v < 0) return Bytes();
        val = (val << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<unsigned char>((val >> bits) & 0xFF)); }
    }
    return out;
}

std::string b64(const Bytes& b) {
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    size_t i = 0;
    for (; i + 2 < b.size(); i += 3) { unsigned v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2]; o += a[v >> 18]; o += a[(v >> 12) & 63]; o += a[(v >> 6) & 63]; o += a[v & 63]; }
    if (b.size() - i == 1) { unsigned v = b[i] << 16; o += a[v >> 18]; o += a[(v >> 12) & 63]; }
    else if (b.size() - i == 2) { unsigned v = (b[i] << 16) | (b[i + 1] << 8); o += a[v >> 18]; o += a[(v >> 12) & 63]; o += a[(v >> 6) & 63]; }
    return o;
}

Bytes decodeKey(const std::string& s, size_t size) { if (s.empty()) return Bytes(); Bytes b = unb64(s); return b.size() == size ? b : Bytes(); }

bool isLinkUrl(const std::string& url0) {
    std::string u = trim(url0);
    std::string l = lower(u);
    if (!startsWith(l, "https://") && !startsWith(l, "http://")) return false;
    std::string h = hostOf(u);
    if (h != "mega.nz" && h != "mega.co.nz" && h != "mega.io" && h != "mega.app") return false;
    std::string after = u.substr(u.find("://") + 3);
    size_t sl = after.find('/');
    std::string rest = sl == std::string::npos ? "" : after.substr(sl + 1);
    return startsWith(rest, "file/") || startsWith(rest, "embed/") || startsWith(rest, "folder/") || startsWith(rest, "#!") || startsWith(rest, "#F!") || startsWith(rest, "#P!");
}

bool parse(const std::string& url0, Link& out) {
    if (!isLinkUrl(url0)) return false;
    std::string u = trim(url0);
    std::string after = u.substr(u.find("://") + 3);
    size_t sl = after.find('/');
    std::string tail = "/" + (sl == std::string::npos ? "" : after.substr(sl + 1));
    const std::string H = "([A-Za-z0-9_-]{6,16})", FK = "([A-Za-z0-9_-]{43})", DK = "([A-Za-z0-9_-]{22})";
    std::smatch m;
    if (std::regex_search(tail, m, std::regex("/folder/" + H + "(?:#" + DK + ")?(?:/(file|folder)/" + H + ")?"))) {
        out.folder = true; out.handle = m[1]; out.key = decodeKey(m[2], 16); out.child = m[4];
        out.childIsFile = out.child.empty() ? -1 : (m[3] == "file" ? 1 : 0);
        return true;
    }
    if (std::regex_search(tail, m, std::regex("#F!" + H + "(?:!" + DK + ")?(?:[!?]" + H + ")?"))) {
        out.folder = true; out.handle = m[1]; out.key = decodeKey(m[2], 16); out.child = m[3];
        return true;
    }
    if (std::regex_search(tail, m, std::regex("/(?:file|embed)/" + H + "(?:[#!]" + FK + ")?")) || std::regex_search(tail, m, std::regex("#!" + H + "(?:!" + FK + ")?"))) {
        out.folder = false; out.handle = m[1]; out.key = decodeKey(m[2], 32);
        return true;
    }
    return false;
}

Bytes aesKey(const Bytes& k) {
    if (k.size() == 16) return k;
    Bytes r(16);
    for (int i = 0; i < 16; i++) r[static_cast<size_t>(i)] = k[static_cast<size_t>(i)] ^ k[static_cast<size_t>(i + 16)];
    return r;
}

std::string decryptAttrs(const std::string& at, const Bytes& key, Json& out) {
    Bytes data = unb64(at);
    if (data.empty() || data.size() % 16) return "bad";
    Bytes k = aesKey(key);
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    mbedtls_aes_setkey_dec(&a, k.data(), 128);
    unsigned char iv[16] = {0};
    Bytes plain(data.size());
    mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_DECRYPT, data.size(), iv, data.data(), plain.data());
    mbedtls_aes_free(&a);
    std::string text(plain.begin(), plain.end());
    text.resize(strnlen(text.c_str(), text.size()));
    if (!startsWith(text, "MEGA{")) return "bad";
    bool ok = false;
    out = Json::parse(text.substr(4), &ok);
    return ok ? "" : "bad";
}

Bytes ecbDecrypt(const Bytes& enc, const Bytes& key) {
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    mbedtls_aes_setkey_dec(&a, key.data(), 128);
    Bytes out(enc.size());
    for (size_t i = 0; i + 16 <= enc.size(); i += 16) mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_DECRYPT, enc.data() + i, out.data() + i);
    mbedtls_aes_free(&a);
    return out;
}

struct MegaError { std::string text; bool retry = true; };

MegaError errorText(int code) {
    switch (code) {
        case -2: return {"MEGA says this link is invalid. Copy the whole link again.", false};
        case -9: return {"This file isn't on MEGA any more (removed, or the link is wrong).", false};
        case -11: return {"MEGA refused access to this link.", false};
        case -16: return {"MEGA has taken this file down.", false};
        case -17: case -4: return {kQuota, true};
        case -18: return {"MEGA is temporarily unavailable. DDM will try again.", true};
        default: return {"MEGA error " + std::to_string(code) + ". DDM will try again.", true};
    }
}

// One API command; retries "busy" answers. Returns false with err filled.
bool api(const Json& cmd, const std::string& folder, Json& out, MegaError& err, std::atomic<bool>* stop = nullptr) {
    for (int attempt = 0; attempt < 6; attempt++) {
        if (stop && stop->load()) { err = {"Stopped", true}; return false; }
        static std::string base = [] { char b[256] = ""; if (GetEnvironmentVariableA("DDM_TEST_UI", nullptr, 0)) GetEnvironmentVariableA("DDM_TEST_MEGA_API", b, sizeof b); return std::string(*b ? b : kApi); }();
        std::string url = base + "/cs?id=" + std::to_string(++g_seq) + (folder.empty() ? "" : "&n=" + folder);
        Json arr = Json::array();
        arr.push(cmd);
        RequestInfo r;
        r.url = url;
        std::string body;
        long st = http::post(r, arr.dump(), "application/json", body, 40);
        err = {st == 0 ? "Couldn't reach MEGA. Check your internet connection." : "Unexpected answer from MEGA.", true};
        if (st == 200) {
            bool ok = false;
            Json j = Json::parse(trim(body), &ok);
            if (ok) {
                Json first = j.isArray() && j.size() ? j.at(0) : j;
                if (first.isNumber()) {
                    int c = first.i32();
                    if (c == -3 || c == -6 || c == -1) err = {"MEGA is busy (error " + std::to_string(c) + ").", true};
                    else if (c < 0) { err = errorText(c); return false; }
                } else if (first.isObject()) { out = first; return true; }
            }
        }
        for (int w = 0; w < std::min(20000, 1000 << (attempt + 1)) / 100; w++) { if (stop && stop->load()) break; Sleep(100); }
    }
    return false;
}

bool gUrl(const Json& o, std::string& url, MegaError& err) {
    const Json& g = o["g"];
    url = g.isString() ? g.str() : (g.isArray() && g.size() ? g.at(0).str() : "");
    if (startsWith(url, "http://") || startsWith(url, "https://")) return true;
    if (o["tl"].i64() > 0 || o.has("efq")) err = {kQuota, true};
    else err = {"MEGA didn't give a download address. Try again later.", true};
    return false;
}

Json cmd(std::initializer_list<std::pair<const char*, Json>> kv) { Json j = Json::object(); for (auto& p : kv) j[p.first] = p.second; return j; }

bool downloadUrl(const Link& l, const std::string& node, std::string& url, MegaError& err, std::atomic<bool>* stop) {
    Json o;
    bool ok = l.folder ? api(cmd({{"a", "g"}, {"g", 1}, {"ssl", 2}, {"n", node.empty() ? l.child : node}}), l.handle, o, err, stop)
                       : api(cmd({{"a", "g"}, {"g", 1}, {"ssl", 2}, {"p", l.handle}}), "", o, err, stop);
    return ok && gUrl(o, url, err);
}

struct Node { std::string h, p, name; int type = 0; int64_t size = 0; Bytes key; };

bool folderNodes(const Link& l, std::vector<Node>& all, MegaError& err) {
    Json o;
    if (!api(cmd({{"a", "f"}, {"c", 1}, {"ca", 1}, {"r", 1}}), l.handle, o, err)) return false;
    if (!o["f"].isArray()) { err = {"Unexpected answer from MEGA.", true}; return false; }
    for (auto& n : o["f"].list()) {
        int type = n["t"].i32(-1);
        if (type != 0 && type != 1) continue;
        Bytes enc;
        for (auto& part : split(n["k"].str(), '/')) {
            size_t i = part.find(':');
            if (i == std::string::npos) continue;
            Bytes b = unb64(part.substr(i + 1));
            if (b.size() == (type == 0 ? 32u : 16u)) { enc = b; break; }
        }
        if (enc.empty()) continue;
        Bytes key = ecbDecrypt(enc, l.key);
        Json at;
        if (!decryptAttrs(n["a"].str(), key, at).empty()) continue;
        std::string name = at["n"].str();
        if (trim(name).empty()) continue;
        Node x; x.h = n["h"].str(); x.p = n["p"].str(); x.type = type; x.name = name; x.size = n["s"].i64(); x.key = key;
        all.push_back(x);
    }
    if (all.empty()) { err = {"The key in this MEGA folder link is wrong, or the folder is empty.", false}; return false; }
    return true;
}

std::string folderFileLink(const Link& l, const std::string& node) { return "https://mega.nz/folder/" + l.handle + "#" + b64(l.key) + "/file/" + node; }

// Opening a file: name, size, key and a download address.
struct Opened { std::string name, url; int64_t size = -1; Bytes key; Link link; std::string node; };

bool openFile(const std::string& url, Opened& f, MegaError& err, std::atomic<bool>* stop) {
    std::string p = megaProblem(url);
    if (!p.empty()) { err = {p, false}; return false; }
    Link l;
    parse(url, l);
    f.link = l;
    if (l.folder) {
        if (l.child.empty()) { err = {"This is a folder link, not a file.", false}; return false; }
        std::vector<Node> all;
        if (!folderNodes(l, all, err)) return false;
        auto it = std::find_if(all.begin(), all.end(), [&](const Node& n) { return n.h == l.child && n.type == 0; });
        if (it == all.end()) { err = {"This file isn't in the shared folder any more.", false}; return false; }
        f.name = sanitizeFileName(it->name); f.size = it->size; f.key = it->key; f.node = it->h;
        return downloadUrl(l, f.node, f.url, err, stop);
    }
    Json o;
    if (!api(cmd({{"a", "g"}, {"g", 1}, {"ssl", 2}, {"p", l.handle}}), "", o, err, stop)) return false;
    f.size = o["s"].isNumber() ? o["s"].i64() : -1;
    if (f.size < 0) { err = {"MEGA didn't give the file size. Try again later.", true}; return false; }
    Json at;
    if (!decryptAttrs(o["at"].str(), l.key, at).empty()) { err = {"The key in this MEGA link is wrong. Copy the whole link again.", false}; return false; }
    f.name = sanitizeFileName(trim(at["n"].str()).empty() ? "MEGA file" : at["n"].str());
    f.key = l.key;
    return gUrl(o, f.url, err);
}

// AES-128-CTR at any byte offset.
void ctrApply(mbedtls_aes_context* a, const unsigned char nonce[8], int64_t offset, char* data, size_t len) {
    uint64_t block = static_cast<uint64_t>(offset) / 16;
    size_t at = static_cast<size_t>(offset % 16);
    unsigned char ctr[16], ks[16];
    size_t i = 0;
    while (i < len) {
        memcpy(ctr, nonce, 8);
        for (int j = 0; j < 8; j++) ctr[8 + j] = static_cast<unsigned char>(block >> (56 - 8 * j));
        mbedtls_aes_crypt_ecb(a, MBEDTLS_AES_ENCRYPT, ctr, ks);
        for (; at < 16 && i < len; at++, i++) data[i] ^= static_cast<char>(ks[at]);
        at = 0;
        block++;
    }
}

// The integrity code in the key (CBC-MAC over MEGA's chunks), checked like MEGA's own apps do.
bool verify(const std::string& path, const Bytes& key32, std::atomic<bool>* stop) {
    HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    Bytes k = aesKey(key32);
    mbedtls_aes_context a;
    mbedtls_aes_init(&a);
    mbedtls_aes_setkey_enc(&a, k.data(), 128);
    unsigned char iv0[16];
    memcpy(iv0, key32.data() + 16, 8);
    memcpy(iv0 + 8, key32.data() + 16, 8);
    unsigned char mac[16] = {0};
    std::vector<unsigned char> buf(0x100000 + 16);
    int64_t start = 0, clen = 0x20000, size = sz.QuadPart;
    bool ok = true;
    while (start < size) {
        if (stop && stop->load()) { ok = false; break; }
        int64_t len = std::min(clen, size - start);
        DWORD got = 0, total = 0;
        while (total < len && ReadFile(f, buf.data() + total, static_cast<DWORD>(len - total), &got, nullptr) && got) total += got;
        if (total != len) { ok = false; break; }
        size_t padded = static_cast<size_t>(std::max<int64_t>(16, (len + 15) / 16 * 16));
        memset(buf.data() + len, 0, padded - static_cast<size_t>(len));
        unsigned char iv[16];
        memcpy(iv, iv0, 16);
        std::vector<unsigned char> out(padded);
        mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_ENCRYPT, padded, iv, buf.data(), out.data());
        for (int i = 0; i < 16; i++) mac[i] ^= out[padded - 16 + static_cast<size_t>(i)];
        unsigned char m2[16];
        mbedtls_aes_crypt_ecb(&a, MBEDTLS_AES_ENCRYPT, mac, m2);
        memcpy(mac, m2, 16);
        start += len;
        if (clen < 0x100000) clen += 0x20000;
    }
    mbedtls_aes_free(&a);
    CloseHandle(f);
    if (!ok) return false;
    for (int i = 0; i < 4; i++) {
        if ((mac[i] ^ mac[4 + i]) != key32[static_cast<size_t>(24 + i)]) return false;
        if ((mac[8 + i] ^ mac[12 + i]) != key32[static_cast<size_t>(28 + i)]) return false;
    }
    return true;
}

} // namespace

bool isMegaLink(const std::string& url) { return isLinkUrl(url); }

bool isMegaFolderLink(const std::string& url) { Link l; return parse(url, l) && l.folder && l.childIsFile != 1 && l.child.empty(); }

std::string megaProblem(const std::string& url) {
    if (!isLinkUrl(url)) return "";
    if (url.find("#P!") != std::string::npos) return "Password-protected MEGA links aren't supported. Ask for the normal link (with its key).";
    Link l;
    if (!parse(url, l)) return "This MEGA link isn't complete. Copy the whole link again.";
    if (l.key.empty()) return "This MEGA link has no key (the part after #). Ask for the full link, including the key.";
    return "";
}

bool megaInfo(const std::string& url, MegaEntry& out, std::string& error) {
    Opened f;
    MegaError e;
    if (!openFile(url, f, e, nullptr)) { error = e.text; return false; }
    out.url = url; out.name = f.name; out.size = f.size; out.path.clear();
    return true;
}

bool megaFolder(const std::string& url, std::string& folderName, std::vector<MegaEntry>& files, std::string& error) {
    std::string p = megaProblem(url);
    if (!p.empty()) { error = p; return false; }
    Link l;
    parse(url, l);
    if (!l.folder) { error = "Not a folder link."; return false; }
    std::vector<Node> all;
    MegaError e;
    if (!folderNodes(l, all, e)) { error = e.text; return false; }
    std::map<std::string, const Node*> byHandle;
    for (auto& n : all) byHandle[n.h] = &n;
    const Node* top = nullptr;
    if (!l.child.empty()) {
        auto it = byHandle.find(l.child);
        if (it == byHandle.end()) { error = "This item isn't in the shared folder any more."; return false; }
        top = it->second;
    } else {
        for (auto& n : all) if (n.type == 1 && !byHandle.count(n.p)) { top = &n; break; }
        if (!top) top = &all[0];
    }
    folderName = sanitizeFileName(top->name);
    files.clear();
    if (top->type == 0) { files.push_back({folderFileLink(l, top->h), sanitizeFileName(top->name), "", top->size}); return true; }
    std::map<std::string, std::vector<const Node*>> kids;
    for (auto& n : all) kids[n.p].push_back(&n);
    std::function<void(const Node*, const std::string&, int)> walk = [&](const Node* n, const std::string& path, int depth) {
        if (depth > 64) return;
        auto it = kids.find(n->h);
        if (it == kids.end()) return;
        auto list = it->second;
        std::sort(list.begin(), list.end(), [](const Node* a, const Node* b) { return lower(a->name) < lower(b->name); });
        for (auto c : list) {
            if (c->type == 0) files.push_back({folderFileLink(l, c->h), sanitizeFileName(c->name), path, c->size});
            else walk(c, path.empty() ? sanitizeFileName(c->name) : path + "\\" + sanitizeFileName(c->name), depth + 1);
        }
    };
    walk(top, "", 0);
    if (files.empty()) { error = "This MEGA folder is empty."; return false; }
    return true;
}

DownloadResult runMegaDownload(DownloadContext& ctx) {
    std::string url;
    { std::lock_guard<std::mutex> g(*ctx.mu); url = ctx.task->url; ctx.task->status = "Opening the MEGA link…"; }
    if (ctx.changed) ctx.changed();
    Opened f;
    MegaError e;
    if (!openFile(url, f, e, ctx.stop)) {
        if (ctx.stop && ctx.stop->load()) return DownloadResult::Stopped;
        std::lock_guard<std::mutex> g(*ctx.mu);
        ctx.task->error = e.text;
        return DownloadResult::Failed;
    }
    {
        std::lock_guard<std::mutex> g(*ctx.mu);
        Task& t = *ctx.task;
        if (t.total >= 0 && t.total != f.size) { t.segments.clear(); t.resumable = false; }   // changed on MEGA: start over
        if (t.fileName.empty() || startsWith(t.fileName, "MEGA ")) t.fileName = f.name;
        t.total = f.size;
    }
    if (f.size == 0) {
        std::string path;
        { std::lock_guard<std::mutex> g(*ctx.mu); path = ctx.task->path(); }
        makeDirs(dirName(path));
        writeFileAtomic(uniquePath(path), "");
        return DownloadResult::Done;
    }
    std::mutex urlMu;
    std::string base = f.url;
    Bytes k = aesKey(f.key);
    unsigned char nonce[8];
    memcpy(nonce, f.key.data() + 16, 8);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, k.data(), 128);
    ctx.knownTotal = f.size;
    ctx.rangeUrl = [&](int64_t from) { std::lock_guard<std::mutex> g(urlMu); return base + "/" + std::to_string(from) + "-" + std::to_string(f.size - 1); };
    ctx.transform = [&](int64_t off, char* d, size_t n) { ctrApply(&aes, nonce, off, d, n); };   // one download thread: no lock needed
    int64_t lastRenew = 0;
    ctx.renew = [&]() {
        if (nowMs() - lastRenew < 5000) return 2;   // another part just renewed it
        lastRenew = nowMs();
        std::string fresh;
        MegaError re;
        if (!downloadUrl(f.link, f.node, fresh, re, ctx.stop)) return 0;
        std::lock_guard<std::mutex> g(urlMu);
        base = fresh;
        return 1;
    };
    ctx.statusError = [](long st) { return st == 509 ? std::string(kQuota) : std::string("MEGA keeps refusing the download. Press Resume to try again later."); };
    DownloadResult r = runHttpDownload(ctx);
    mbedtls_aes_free(&aes);
    ctx.rangeUrl = nullptr; ctx.transform = nullptr; ctx.renew = nullptr; ctx.statusError = nullptr; ctx.knownTotal = -1;
    if (r == DownloadResult::Done) {
        std::string path;
        { std::lock_guard<std::mutex> g(*ctx.mu); path = fileExists(ctx.task->path()) ? ctx.task->path() : ctx.task->partPath(); ctx.task->status = "Checking the file…"; }
        if (ctx.changed) ctx.changed();
        if (f.key.size() == 32 && !verify(path, f.key, ctx.stop)) {
            if (ctx.stop && ctx.stop->load()) return DownloadResult::Done;
            std::lock_guard<std::mutex> g(*ctx.mu);
            ctx.task->error = "The file arrived damaged (MEGA's integrity check failed). Delete it and download it again.";
            return DownloadResult::Failed;
        }
    }
    return r;
}

} // namespace ddm
