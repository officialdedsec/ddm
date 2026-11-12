#include "pagepdf.hpp"
#include "process.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <mutex>
#include <cstring>
#include <vector>

namespace ddm {
namespace pagepdf {

namespace {
std::mutex g_one;   // one PDF at a time: several hidden browsers at once only make each other slow

std::vector<std::string> browsers() {
    std::vector<std::string> out;
    auto addOnce = [&](const std::string& p) { for (auto& o : out) if (lower(o) == lower(p)) return; if (fileExists(p)) out.push_back(p); };
    // the App Paths entries first (where Windows itself looks), then the usual folders; Edge before Chrome
    for (const wchar_t* app : {L"msedge.exe", L"chrome.exe"}) {
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            HKEY k;
            std::wstring key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + app;
            if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
            wchar_t v[MAX_PATH * 2] = {};
            DWORD n = sizeof v, type = 0;
            LONG r = RegQueryValueExW(k, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(v), &n);
            RegCloseKey(k);
            if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
            std::string p = trim(U(v));
            if (p.size() > 2 && p.front() == '"') p = p.substr(1, p.find('"', 1) - 1);
            addOnce(p);
        }
        for (int csidl : {0x2a /*ProgramFilesX86*/, 0x26 /*ProgramFiles*/, 0x1c /*LocalAppData*/}) {
            wchar_t base[MAX_PATH] = {};
            if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, base))) continue;
            addOnce(joinPath(U(base), std::wstring(app) == L"msedge.exe" ? "Microsoft\\Edge\\Application\\msedge.exe" : "Google\\Chrome\\Application\\chrome.exe"));
        }
    }
    return out;
}

std::string fileUrl(const std::string& path) {
    wchar_t buf[4096] = {};
    DWORD n = 4096;
    if (SUCCEEDED(UrlCreateFromPathW(W(path).c_str(), buf, &n, 0))) return U(buf);
    return "file:///" + replaceAll(path, "\\", "/");
}

bool isPdf(const std::string& path) {
    if (fileSize(path) <= 0) return false;
    HANDLE f = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char b[5] = {}; DWORD got = 0;
    ReadFile(f, b, 4, &got, nullptr);
    CloseHandle(f);
    return got == 4 && std::string(b, 4) == "%PDF";
}
} // namespace

std::string browserExe() { auto b = browsers(); return b.empty() ? "" : b.front(); }

bool save(const std::string& url, const std::string& html, const std::string& outPath, std::string& error) {
    std::string l = lower(url);
    if (!(startsWith(l, "http://") || startsWith(l, "https://")) || url.size() > 8192) { error = "Only web pages (http / https) can be saved as PDF."; return false; }
    auto exes = browsers();
    if (exes.empty()) { error = "Saving a page as PDF needs Microsoft Edge or Google Chrome on this PC."; return false; }
    std::lock_guard<std::mutex> one(g_one);
    std::string work = joinPath(joinPath(dataDir(), "pdf-work"), newId());
    makeDirs(work);
    // what is printed: the page as you saw it (the add-on's snapshot), else its address
    std::vector<std::string> targets;
    if (!html.empty()) {
        // The copy is opened from a file on this PC, so it is locked down first: no scripts, and it may load only
        // web pictures, styles and fonts, never other files on this PC (a page can't print your files into its PDF).
        static const char* csp = "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src http: https: data: blob:; "
                                 "style-src 'unsafe-inline' http: https: data:; font-src http: https: data:; media-src http: https: data:; "
                                 "frame-src http: https:; script-src 'none'; object-src 'none'; form-action 'none'; base-uri http: https:\">";
        std::string doc = html;
        std::string low = lower(doc.substr(0, std::min<size_t>(doc.size(), 65536)));
        // the lock-down goes before everything else in the page (right after its doctype): nothing in the copy, not even
        // something placed before <head>, can run or load before the policy applies (the browser moves it into <head>)
        size_t first = low.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
        if (first != std::string::npos && low.compare(first, 9, "<!doctype") == 0 && low.find('>', first) != std::string::npos) doc.insert(low.find('>', first) + 1, csp);
        else if (low.find("<head") != std::string::npos || low.find("<html") != std::string::npos) doc = csp + doc;   // (no doctype: kept that way)
        else doc = std::string("<!DOCTYPE html><meta charset=\"utf-8\">") + csp + doc;
        std::string page = joinPath(work, "page.html");
        if (writeFileAtomic(page, doc)) targets.push_back(fileUrl(page));
    }
    targets.push_back(url);
    // the browser writes into DDM's own folder (Windows' "Controlled folder access" and some antivirus tools stop a
    // browser writing into Documents); DDM moves the finished PDF there itself
    std::string tmp = joinPath(work, "page.pdf");
    bool ok = false, started = false, slow = false;
    std::string why;
    for (size_t t = 0; t < targets.size() && !ok; t++) {
        for (auto& exe : exes) {
            for (int mode = 0; mode < 2 && !ok; mode++) {
                // every attempt: a profile of its own (a browser still closing never hands the job to itself)
                std::string profile = joinPath(work, "p" + std::to_string(t) + std::to_string(mode) + std::to_string(&exe - &exes[0]));
                makeDirs(profile);
                DeleteFileW(W(tmp).c_str());
                std::vector<std::string> a = {mode == 0 ? "--headless=new" : "--headless", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
                                              "--disable-extensions", "--disable-sync", "--disable-features=msEdgeStartupBoost,Translate",
                                              "--user-data-dir=" + profile, "--no-pdf-header-footer", "--print-to-pdf-no-header",
                                              "--run-all-compositor-stages-before-draw", "--virtual-time-budget=" + std::string(mode == 0 ? "10000" : "20000"),
                                              "--print-to-pdf=" + tmp};
                a.push_back(targets[t]);   // (the copy's lock-down above already stops every script)
                RunResult r = runHidden(exe, a, nullptr, nullptr, 75000);
                started = started || r.started;
                slow = slow || r.stopped;
                ok = isPdf(tmp);
                if (!ok) {
                    std::string e = trim(r.err);
                    if (e.size() > 300) e = e.substr(e.size() - 300);
                    why = baseName(exe) + " exit " + std::to_string(r.exitCode) + (r.stopped ? " (timed out)" : "") + (e.empty() ? "" : ": " + e);
                    logLine("pdf: " + why);
                }
                removeTree(profile);
            }
            if (ok) break;
        }
    }
    if (!ok) {
        removeTree(work);
        error = !started ? "Edge or Chrome couldn't be started on this PC." : slow ? "The page took too long to load. Try again when it has finished loading." : "The browser couldn't print this page.";
        return false;
    }
    makeDirs(dirName(outPath));
    bool moved = MoveFileExW(W(tmp).c_str(), W(outPath).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE;
    if (!moved) logLine("pdf: couldn't move to " + outPath + " (error " + std::to_string(GetLastError()) + ")");
    removeTree(work);
    if (!moved) { error = "DDM couldn't save the PDF in " + dirName(outPath) + "."; return false; }
    return true;
}

} // namespace pagepdf
} // namespace ddm
