#include "antivirus.hpp"
#include "process.hpp"
#include "settings.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <mutex>
#include <set>
#include <thread>

namespace ddm {
namespace antivirus {

namespace {
const CLSID kAttachmentServices = {0x4125dd96, 0xe03a, 0x4103, {0x8f, 0x70, 0xe0, 0x59, 0x7d, 0x80, 0x3b, 0x9c}};
const IID kIAttachmentExecute = {0x73db1241, 0x1e85, 0x4581, {0x8e, 0x4f, 0xa8, 0x1e, 0x1d, 0x0f, 0x8c, 0x57}};
const GUID kDdmClient = {0x8d2a7a51, 0x3f5c, 0x4c33, {0x9a, 0x51, 0x1d, 0xd5, 0x0e, 0x1c, 0x44, 0x01}};
std::mutex g_one;   // one scan at a time: antivirus scanners don't like being run many times together

std::string folderPath(int csidl) { wchar_t b[MAX_PATH] = {}; return SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, b)) ? U(b) : ""; }

// Windows' attachment services: runs every antivirus registered with Windows on the file (the same check browsers use)
// and marks it as coming from the internet. A failure means the antivirus blocked or removed it.
HRESULT attachmentScan(const std::string& path, const std::string& url, const std::string& referrer) {
    HRESULT hr = E_FAIL;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
        IAttachmentExecute* ae = nullptr;
        if (SUCCEEDED(CoCreateInstance(kAttachmentServices, nullptr, CLSCTX_INPROC_SERVER, kIAttachmentExecute, reinterpret_cast<void**>(&ae))) && ae) {
            ae->SetClientGuid(kDdmClient);
            ae->SetClientTitle(L"DDM");
            ae->SetLocalPath(W(path).c_str());
            if (!url.empty()) ae->SetSource(W(url).c_str());
            if (!referrer.empty()) ae->SetReferrer(W(referrer).c_str());
            hr = ae->Save();
            ae->Release();
        }
        CoUninitialize();
    }).join();
    return hr;
}
} // namespace

std::string defenderPath() {
    for (int csidl : {CSIDL_PROGRAM_FILES, 0x0026 /*CSIDL_PROGRAM_FILES (64-bit view via sysnative not needed)*/}) {
        std::string pf = folderPath(csidl);
        for (auto sub : {"Windows Defender\\MpCmdRun.exe", "Microsoft Security Client\\MpCmdRun.exe", "Microsoft Security Client\\Antimalware\\MpCmdRun.exe"}) {
            std::string p = joinPath(pf, sub);
            if (fileExists(p)) return p;
        }
    }
    // 64-bit Windows: the 64-bit Program Files from a 32-bit program
    wchar_t pf64[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramW6432", pf64, MAX_PATH)) {
        std::string p = joinPath(U(pf64), "Windows Defender\\MpCmdRun.exe");
        if (fileExists(p)) return p;
    }
    // Windows 10/11 platform folder (newest version)
    std::string base = joinPath(folderPath(CSIDL_COMMON_APPDATA), "Microsoft\\Windows Defender\\Platform");
    WIN32_FIND_DATAW fd;
    std::string best;
    HANDLE f = FindFirstFileW(W(joinPath(base, "*")).c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::string n = U(fd.cFileName);
            if (n == "." || n == "..") continue;
            std::string p = joinPath(joinPath(base, n), "MpCmdRun.exe");
            if (fileExists(p) && n > best) best = n;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return best.empty() ? "" : joinPath(joinPath(base, best), "MpCmdRun.exe");
}

std::string activeName() {
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
            IWbemLocator* loc = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, reinterpret_cast<void**>(&loc)))) {
                IWbemServices* svc = nullptr;
                BSTR ns = SysAllocString(L"ROOT\\SecurityCenter2");
                if (SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)) && svc) {
                    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
                    IEnumWbemClassObject* en = nullptr;
                    BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(L"SELECT displayName, productState FROM AntiVirusProduct");
                    if (SUCCEEDED(svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en)) && en) {
                        IWbemClassObject* o = nullptr;
                        ULONG n = 0;
                        std::string first;
                        while (en->Next(3000, 1, &o, &n) == S_OK && n) {
                            VARIANT name, state;
                            VariantInit(&name); VariantInit(&state);
                            o->Get(L"displayName", 0, &name, nullptr, nullptr);
                            o->Get(L"productState", 0, &state, nullptr, nullptr);
                            std::string nm = name.vt == VT_BSTR && name.bstrVal ? U(name.bstrVal) : "";
                            long st = state.vt == VT_I4 ? state.lVal : 0;
                            bool on = ((st >> 12) & 0xF) == 1;   // the "enabled" nibble of productState
                            if (!nm.empty() && on && cached.empty()) cached = nm;
                            if (first.empty()) first = nm;
                            VariantClear(&name); VariantClear(&state);
                            o->Release();
                        }
                        if (cached.empty()) cached = first;
                        en->Release();
                    }
                    SysFreeString(lang); SysFreeString(q);
                    svc->Release();
                }
                SysFreeString(ns);
                loc->Release();
            }
            CoUninitialize();
        }).join();
    });
    return cached;
}

std::string scannerName() {
    const Settings& s = settings();
    if (!trim(s.antivirusPath).empty()) return baseName(s.antivirusPath);
    std::string a = activeName();
    if (!a.empty()) return a;
    return defenderPath().empty() ? "your antivirus" : "Microsoft Defender";
}

bool wantsScan(const std::string& fileName) {
    const Settings& s = settings();
    if (!s.scanWithAntivirus) return false;
    if (!s.antivirusOnlyRisky) return true;
    static const std::set<std::string> risky = {"exe", "msi", "msix", "msixbundle", "appx", "appxbundle", "bat", "cmd", "com", "scr", "ps1", "vbs", "js", "jse", "wsf", "hta", "jar", "apk",
        "dll", "sys", "lnk", "reg", "iso", "img", "zip", "rar", "7z", "cab", "tar", "gz", "xz", "bz2", "docm", "xlsm", "pptm", "doc", "xls", "pdf", "msu", "torrent"};
    return risky.count(lower(extOf(fileName))) > 0;
}

Result scan(const std::string& path, const std::string& url, const std::string& referrer) {
    std::lock_guard<std::mutex> one(g_one);
    Result r;
    bool isDir = dirExists(path);
    if (!isDir && !fileExists(path)) { r.kind = Result::Error; r.message = "The file isn't there any more."; return r; }
    const Settings& s = settings();
    // 1) a program the user chose
    if (!trim(s.antivirusPath).empty()) {
        r.scanner = stem(baseName(s.antivirusPath));
        std::string args = trim(s.antivirusArgs);
        std::vector<std::string> a;
        bool placed = false;
        for (auto& part : split(args, ' ')) {
            if (part.empty()) continue;
            if (part.find("%1") != std::string::npos || part.find("{file}") != std::string::npos) { a.push_back(replaceAll(replaceAll(replaceAll(part, "\"", ""), "%1", path), "{file}", path)); placed = true; }
            else a.push_back(part);
        }
        if (!placed) a.push_back(path);
        RunResult rr = runHidden(s.antivirusPath, a, nullptr, nullptr, 30 * 60 * 1000);
        if (!rr.started) { r.kind = Result::Error; r.message = "Couldn't start " + baseName(s.antivirusPath) + "."; return r; }
        if (!isDir && !fileExists(path)) { r.kind = Result::Removed; r.message = r.scanner + " removed this file: it contains a threat."; return r; }
        if (rr.exitCode == 0) { r.kind = Result::Clean; r.message = "Scanned by " + r.scanner + ": no threats found."; }
        else { r.kind = Result::Threat; r.message = r.scanner + " reported a problem with this file (code " + std::to_string(rr.exitCode) + "). Check it in " + r.scanner + "."; }
        return r;
    }
    // 2) Windows' attachment check: every registered antivirus (Windows 7–11)
    std::string active = activeName();
    r.scanner = active.empty() ? "Windows" : active;
    if (!isDir) {
        HRESULT hr = attachmentScan(path, url, referrer);
        Sleep(300);
        if (!fileExists(path)) { r.kind = Result::Removed; r.message = r.scanner + " removed this file: it contains a threat."; return r; }
        if (hr == INET_E_SECURITY_PROBLEM || hr == E_ACCESSDENIED || hr == HRESULT_FROM_WIN32(ERROR_VIRUS_INFECTED) || hr == HRESULT_FROM_WIN32(ERROR_VIRUS_DELETED)) {
            r.kind = Result::Threat; r.message = r.scanner + " blocked this file: it may contain a threat."; return r;
        }
    }
    // 3) Microsoft Defender's own scanner, when Defender protects this PC (or nothing else is registered)
    std::string mp = defenderPath();
    bool defenderActive = active.empty() || lower(active).find("defender") != std::string::npos || lower(active).find("security essentials") != std::string::npos ||
                          lower(active).find("windows security") != std::string::npos;
    if (!mp.empty() && defenderActive) {
        r.scanner = active.empty() ? "Microsoft Defender" : active;
        RunResult rr = runHidden(mp, {"-Scan", "-ScanType", "3", "-File", path, "-DisableRemediation"}, nullptr, nullptr, 30 * 60 * 1000);
        if (rr.started) {
            if (rr.exitCode == 2) {   // threat found: let Defender deal with it (quarantine), then tell the user
                runHidden(mp, {"-Scan", "-ScanType", "3", "-File", path}, nullptr, nullptr, 30 * 60 * 1000);
                Sleep(500);
                if (!isDir && !fileExists(path)) { r.kind = Result::Removed; r.message = r.scanner + " removed this file: it contains a threat."; }
                else { r.kind = Result::Threat; r.message = r.scanner + " found a threat in this file. Don't open it."; }
                return r;
            }
            if (rr.exitCode == 0) { r.kind = Result::Clean; r.message = "Scanned by " + r.scanner + ": no threats found."; return r; }
        }
    }
    r.kind = Result::Clean;
    r.message = "Checked by " + r.scanner + ": no threats found.";
    return r;
}

} // namespace antivirus
} // namespace ddm
