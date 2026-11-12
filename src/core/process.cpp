#include "process.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <algorithm>
#include <thread>
#include <vector>

namespace ddm {

std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\"";
    int slashes = 0;
    for (char c : a) {
        if (c == '\\') { slashes++; continue; }
        if (c == '"') { r.append(static_cast<size_t>(slashes * 2 + 1), '\\'); r += '"'; slashes = 0; continue; }
        r.append(static_cast<size_t>(slashes), '\\');
        slashes = 0;
        r += c;
    }
    r.append(static_cast<size_t>(slashes * 2), '\\');
    r += '"';
    return r;
}

namespace {
void pump(HANDLE pipe, std::string& all, std::function<void(const std::string&)> onLine, const char* prefix) {
    std::string pending;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got) {
        all.append(buf, got);
        if (all.size() > 8 * 1024 * 1024) all.erase(0, all.size() - 4 * 1024 * 1024);
        pending.append(buf, got);
        size_t p;
        while ((p = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string line = pending.substr(0, p);
            pending.erase(0, p + 1);
            if (!line.empty() && onLine) onLine(prefix + line);
        }
    }
    if (!pending.empty() && onLine) onLine(prefix + pending);
}
// The environment a helper starts with. Nothing from a Python, Node or other runtime installed on the PC may change how
// DDM's own helpers behave (they carry everything they need): variables that would point them at someone else's
// libraries are left out. DDM's own helper programs also get only Windows' folders in PATH (no DLLs picked up from
// other programs' folders) and unpack into DDM's own temp folder.
std::wstring helperEnvironment(bool ownHelper) {
    std::wstring out;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return out;
    static const wchar_t* drop[] = {L"PYTHONHOME", L"PYTHONPATH", L"PYTHONSTARTUP", L"PYTHONUSERBASE", L"PYTHONEXECUTABLE", L"PYTHONINSPECT", L"PYTHONOPTIMIZE",
                                    L"PYTHONSAFEPATH", L"PYTHONPLATLIBDIR", L"PYTHONWARNINGS", L"PYTHONIOENCODING", L"PYTHONUTF8", L"PYTHONNOUSERSITE", L"PYTHONDONTWRITEBYTECODE",
                                    L"PYTHONLEGACYWINDOWSFSENCODING", L"PYTHONLEGACYWINDOWSSTDIO", L"PYTHONCASEOK", L"PYTHONVERBOSE", L"PYTHONDEBUG", L"PYTHONMALLOC",
                                    L"VIRTUAL_ENV", L"CONDA_PREFIX", L"NODE_OPTIONS", L"NODE_PATH", L"DENO_DIR", L"SSL_CERT_FILE", L"SSL_CERT_DIR", L"REQUESTS_CA_BUNDLE",
                                    L"CURL_CA_BUNDLE", L"TCL_LIBRARY", L"TK_LIBRARY", L"_MEIPASS2", L"_PYI_APPLICATION_HOME_DIR", L"_PYI_ARCHIVE_FILE", L"_PYI_PARENT_PROCESS_LEVEL"};
    std::wstring tmp;
    if (ownHelper) { tmp = W(joinPath(dataDir(), "engine\\tmp")); makeDirs(U(tmp)); }
    std::vector<std::wstring> vars;
    for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
        std::wstring v = p;
        size_t eq = v.find(L'=', 1);
        if (eq == std::wstring::npos) { vars.push_back(v); continue; }
        std::wstring name = v.substr(0, eq);
        bool skip = false;
        for (auto d : drop) if (_wcsicmp(name.c_str(), d) == 0) skip = true;
        if (ownHelper && (_wcsicmp(name.c_str(), L"PATH") == 0 || _wcsicmp(name.c_str(), L"TEMP") == 0 || _wcsicmp(name.c_str(), L"TMP") == 0)) skip = true;
        if (!skip) vars.push_back(v);
    }
    FreeEnvironmentStringsW(block);
    vars.push_back(L"PYTHONIOENCODING=utf-8");   // UTF-8 everywhere for the helpers' output
    vars.push_back(L"PYTHONUTF8=1");
    vars.push_back(L"PYTHONNOUSERSITE=1");
    vars.push_back(L"PYTHONDONTWRITEBYTECODE=1");
    if (ownHelper) {
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        wchar_t sys[MAX_PATH] = {};
        GetSystemDirectoryW(sys, MAX_PATH);
        vars.push_back(L"PATH=" + W(joinPath(dataDir(), "engine")) + L";" + sys + L";" + win + L";" + std::wstring(sys) + L"\\Wbem");
        vars.push_back(L"TEMP=" + tmp);
        vars.push_back(L"TMP=" + tmp);
    }
    std::stable_sort(vars.begin(), vars.end(), [](const std::wstring& x, const std::wstring& y) {   // Windows keeps it sorted by name
        std::wstring a = x.substr(0, x.find(L'=', 1)), b = y.substr(0, y.find(L'=', 1));
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    for (auto& v : vars) { out += v; out.push_back(L'\0'); }
    out.push_back(L'\0');
    return out;
}
} // namespace

RunResult runHidden(const std::string& exe, const std::vector<std::string>& args, std::function<void(const std::string&)> onLine,
                    std::atomic<bool>* stop, int timeoutMs, const std::string& workDir) {
    RunResult r;
    std::string cmd = quoteArg(exe);
    for (auto& a : args) cmd += " " + quoteArg(a);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE outR, outW, errR, errW;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return r;
    if (!CreatePipe(&errR, &errW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); return r; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    // A job: stopping DDM's helper also stops what it started (the merger, the JavaScript runtime).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    std::wstring wcmd = W(cmd);
    std::wstring wdir = W(workDir);
    // its own clean environment (see helperEnvironment): DDM's helpers are the ones in DDM's data folder
    bool own = startsWith(lower(exe), lower(joinPath(dataDir(), "engine")) + "\\") || startsWith(lower(exe), lower(exeDir()) + "\\");
    std::wstring env = helperEnvironment(own);
    BOOL ok = CreateProcessW(W(exe).c_str(), &wcmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, env.empty() ? nullptr : &env[0],
                             workDir.empty() ? nullptr : wdir.c_str(), &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(outR); CloseHandle(errR);
        if (job) CloseHandle(job);
        r.err = "couldn't start " + baseName(exe) + " (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    r.started = true;
    std::thread te([&] { pump(errR, r.err, onLine, "ERR: "); });
    std::thread to([&] { pump(outR, r.out, onLine, ""); });
    DWORD start = GetTickCount();
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        if ((stop && stop->load()) || (timeoutMs > 0 && GetTickCount() - start > static_cast<DWORD>(timeoutMs))) {
            r.stopped = true;
            if (job) TerminateJobObject(job, 1); else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = static_cast<int>(code);
    if (job) { TerminateJobObject(job, 0); }
    to.join();
    te.join();
    CloseHandle(outR);
    CloseHandle(errR);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return r;
}

} // namespace ddm
