#include "update.hpp"
#include "crypto.hpp"
#include "http.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include "../common/version.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>
#include <cstdio>
#include <mutex>
#include <regex>
#include <vector>

namespace ddm {
namespace update {

namespace {
// The public key of the certificate that signed a program ("" = not signed)
std::string signerThumbprint(const std::string& file) {
    HCERTSTORE store = nullptr; HCRYPTMSG msg = nullptr;
    std::string out;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, W(file).c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, nullptr, nullptr, nullptr, &store, &msg, nullptr)) return "";
    DWORD len = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &len) && len) {
        std::string buf(len, '\0');
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, &buf[0], &len)) {
            auto* si = reinterpret_cast<CMSG_SIGNER_INFO*>(&buf[0]);
            CERT_INFO ci{}; ci.Issuer = si->Issuer; ci.SerialNumber = si->SerialNumber;
            PCCERT_CONTEXT c = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, nullptr);
            if (c) {
                // the publisher's key, not the certificate: a renewed certificate (new e-mail, new dates) with the same key is still DedSec
                const CRYPT_BIT_BLOB& k = c->pCertInfo->SubjectPublicKeyInfo.PublicKey;
                char x[3];
                for (DWORD i = 0; i < k.cbData; i++) { snprintf(x, sizeof x, "%02x", k.pbData[i]); out += x; }
                CertFreeCertificateContext(c);
            }
        }
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return out;
}
// Authenticode check of the whole file (catches any change after signing)
LONG verifyTrust(const std::string& file) {
    std::wstring wf = W(file);
    WINTRUST_FILE_INFO fi{}; fi.cbStruct = sizeof fi; fi.pcwszFilePath = wf.c_str();
    WINTRUST_DATA wd{}; wd.cbStruct = sizeof wd; wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE; wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    return r;
}
std::string exePath() { wchar_t b[MAX_PATH] = {}; GetModuleFileNameW(nullptr, b, MAX_PATH); return U(b); }
} // namespace


namespace {
std::mutex g_mu;
int64_t g_last = 0;
Info g_info;
}

bool isNewer(const std::string& a, const std::string& b) {
    auto x = split(a, '.'), y = split(b, '.');
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        int xi = i < x.size() ? atoi(x[i].c_str()) : 0, yi = i < y.size() ? atoi(y[i].c_str()) : 0;
        if (xi != yi) return xi > yi;
    }
    return false;
}

Info check(bool force) {
    std::lock_guard<std::mutex> g(g_mu);
    if (!force && g_last && nowMs() - g_last < 20LL * 3600 * 1000) return g_info;
    g_last = nowMs();
    Info info;
    std::string stamp = std::to_string(nowMs() / 3600000);
    // 1. the newest GitHub release of DDM's repository; 2. dedseec.com. Either way the information must carry DedSec's
    // signature, so nobody else can point DDM at another installer.
    const std::vector<std::pair<std::string, std::string>> sources = {
        {"https://github.com/" DDM_UPDATE_REPO "/releases/latest/download/update.json", "https://github.com/" DDM_UPDATE_REPO "/releases/latest/download/update.json.sig"},
        {"https://dedseec.com/ddm/update.json?t=" + stamp, "https://dedseec.com/ddm/update.json.sig?t=" + stamp},
        {"https://github.com/" DDM_UPDATE_REPO_OLD "/releases/latest/download/update.json", "https://github.com/" DDM_UPDATE_REPO_OLD "/releases/latest/download/update.json.sig"},
    };
    std::string json, sig;
    long st = 0;
    bool signedOk = false, reached = false;
    for (auto& src : sources) {
        RequestInfo r;
        r.url = src.first;
        json.clear(); sig.clear();
        st = http::get(r, json, nullptr, 64 * 1024, 20);
        if (st == 0) continue;
        reached = true;
        if (st != 200) continue;
        r.url = src.second;
        if (http::get(r, sig, nullptr, 8 * 1024, 20) != 200) continue;
        crypto::Bytes s = crypto::unb64(trim(sig));
        if (crypto::verifyConfig(json.data(), json.size(), s) || crypto::verifyDdm(json.data(), json.size(), s)) { signedOk = true; break; }
        logLine("update.json signature invalid (" + src.first + ")");
    }
    if (!signedOk) {
        info.error = !reached ? "Couldn't reach the update server. Check your internet connection." : "The update information isn't available right now.";
        g_info = info;
        return info;
    }
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEF) json = json.substr(3);
    Json o = Json::parse(json);
    std::string v = trim(o["version"].str());
    if (!std::regex_match(v, std::regex("^[0-9]+(\\.[0-9]+){1,3}$"))) { info.error = "The update information isn't valid."; g_info = info; return info; }
    info.version = v;
    info.url = trim(o["url"].str());
    if (!startsWith(lower(info.url), "https://")) info.url = "https://github.com/" DDM_UPDATE_REPO "/releases/latest";
    std::string setup = trim(o["setup"].str());
    if (startsWith(lower(setup), "https://") && endsWith(lower(setup), ".exe")) info.setup = setup;
    info.setupSha = lower(trim(o["sha256"].str()));
    info.notes = trim(o["notes"].str());
    info.newer = isNewer(v, DDM_VERSION);
    g_info = info;
    return info;
}

std::string downloadSetup(const std::string& url, std::function<void(int64_t, int64_t)> progress, std::string& error) {
    if (!startsWith(lower(url), "https://")) { error = "The update link must be https."; return ""; }
    std::string dir = joinPath(dataDir(), "update");
    makeDirs(dir);
    std::string path = joinPath(dir, "DDM-Setup.exe");
    long st = http::download(url, path, progress, 1800);
    if (st != 200 || fileSize(path) < 100 * 1024) { error = st == 0 ? "The download was interrupted." : "The update server replied " + std::to_string(st) + "."; return ""; }
    // the installer must be exactly what the signed update information describes, or signed like this DDM
    std::string sha;
    { std::lock_guard<std::mutex> g(g_mu); if (g_info.setup == url) sha = g_info.setupSha; }
    if (!sha.empty() && lower(crypto::hex(crypto::hashFile(path, BCRYPT_SHA256_ALGORITHM))) != sha) {
        DeleteFileW(W(path).c_str()); error = "The downloaded installer doesn't match the update information. It wasn't run."; logLine("update: installer hash mismatch"); return "";
    }
    std::string mine = signerThumbprint(exePath()), theirs = signerThumbprint(path);
    LONG trust = verifyTrust(path);
    bool intact = trust == ERROR_SUCCESS || trust == CERT_E_UNTRUSTEDROOT || trust == CERT_E_CHAINING;   // a self-signed publisher is fine when it's the same one
    bool ok = !mine.empty() ? (intact && theirs == mine) : (!sha.empty() || trust == ERROR_SUCCESS);
    if (!ok) { DeleteFileW(W(path).c_str()); error = "The downloaded installer isn't signed by DedSec. It wasn't run: download DDM from its official page instead."; logLine("update: installer signature rejected"); return ""; }
    return path;
}

} // namespace update
} // namespace ddm
