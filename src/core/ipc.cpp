#include "ipc.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <thread>

namespace ddm {

namespace {
std::atomic<bool> g_stop{false};
std::thread g_thread;
const DWORD kMax = 4 * 1024 * 1024;           // the pipe's buffers
const size_t kMaxMessage = 48 * 1024 * 1024;  // one message (a page snapshot for "Save as PDF" can be large)

std::string moduleDir() { wchar_t b[MAX_PATH] = {}; GetModuleFileNameW(nullptr, b, MAX_PATH); return lower(dirName(U(b))); }
std::string moduleName() { wchar_t b[MAX_PATH] = {}; GetModuleFileNameW(nullptr, b, MAX_PATH); return lower(baseName(U(b))); }

// Only this Windows user (and the system) may use DDM's pipe.
bool userOnly(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    char buf[256]; DWORD len = 0;
    bool ok = false;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &len)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &sid)) {
            std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
            ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) != 0;
            LocalFree(sid);
        }
    }
    CloseHandle(tok);
    if (!ok) return false;
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = sd; sa.bInheritHandle = FALSE;
    return true;
}

// The other end must be DDM itself (not a program that took the pipe's name first to read what the browser sends).
bool trustedServer(HANDLE p) {
    typedef BOOL(WINAPI * Fn)(HANDLE, PULONG);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetNamedPipeServerProcessId"));
    if (!fn) return true;   // very old systems: nothing to check with
    ULONG pid = 0;
    if (!fn(p, &pid) || !pid) return false;
    HANDLE pr = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!pr) return false;
    wchar_t img[MAX_PATH * 2] = {};
    DWORD n = MAX_PATH * 2;
    typedef BOOL(WINAPI * Q)(HANDLE, DWORD, LPWSTR, PDWORD);
    auto q = reinterpret_cast<Q>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryFullProcessImageNameW"));
    bool ok = q && q(pr, 0, img, &n);
    CloseHandle(pr);
    if (!ok) return false;
    std::string server = lower(U(img));
    if (baseName(server) != "ddm.exe") return false;
    // the browser helper only talks to the DDM installed beside it
    if (moduleName() == "ddmhost.exe" && dirName(server) != moduleDir()) return false;
    return true;
}

std::string readMessage(HANDLE p) {
    std::string out;
    char buf[64 * 1024];
    while (true) {
        DWORD got = 0;
        BOOL ok = ReadFile(p, buf, sizeof buf, &got, nullptr);
        out.append(buf, got);
        if (ok) break;
        if (GetLastError() != ERROR_MORE_DATA || out.size() > kMaxMessage) return "";
    }
    return out;
}
} // namespace

std::wstring ipcPipeName() {
    wchar_t user[256] = {};
    DWORD n = 256;
    GetUserNameW(user, &n);
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\DDM.Native." + std::to_wstring(session) + L"." + std::wstring(user);
}

void ipcServe(std::function<Json(const Json&)> handler) {
    g_stop = false;
    g_thread = std::thread([handler] {
        std::wstring name = ipcPipeName();
        while (!g_stop) {
            static SECURITY_ATTRIBUTES sa{};
            static PSECURITY_DESCRIPTOR sd = nullptr;
            static bool haveSa = userOnly(sa, sd);
            HANDLE p = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                        PIPE_UNLIMITED_INSTANCES, kMax, kMax, 0, haveSa ? &sa : nullptr);
            if (p == INVALID_HANDLE_VALUE) { Sleep(500); continue; }
            BOOL ok = ConnectNamedPipe(p, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!ok || g_stop) { CloseHandle(p); continue; }
            std::thread([p, handler] {
                std::string in = readMessage(p);
                bool good = false;
                Json req = Json::parse(in, &good);
                Json rep = good ? handler(req) : Json::object();
                if (!good) { rep["ok"] = false; rep["error"] = "bad request"; }
                std::string out = rep.dump();
                DWORD put = 0;
                WriteFile(p, out.data(), static_cast<DWORD>(out.size()), &put, nullptr);
                FlushFileBuffers(p);
                DisconnectNamedPipe(p);
                CloseHandle(p);
            }).detach();
        }
    });
}

void ipcStop() {
    g_stop = true;
    Json r;
    Json m = Json::object();
    m["type"] = "noop";
    ipcSend(m, r, 300);   // wakes the waiting ConnectNamedPipe
    if (g_thread.joinable()) g_thread.join();
}

bool ipcSend(const Json& request, Json& reply, int timeoutMs) {
    std::wstring name = ipcPipeName();
    if (!WaitNamedPipeW(name.c_str(), static_cast<DWORD>(timeoutMs))) return false;
    HANDLE p = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (p == INVALID_HANDLE_VALUE) return false;
    if (!trustedServer(p)) { CloseHandle(p); logLine("ipc: the pipe isn't served by DDM - nothing sent"); return false; }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(p, &mode, nullptr, nullptr);
    std::string out = request.dump();
    DWORD put = 0;
    bool ok = WriteFile(p, out.data(), static_cast<DWORD>(out.size()), &put, nullptr) != 0;
    if (ok) {
        std::string in = readMessage(p);
        bool good = false;
        reply = Json::parse(in, &good);
        ok = good;
    }
    CloseHandle(p);
    return ok;
}

} // namespace ddm
