// DDM browser helper (native messaging host): the browser starts it, it passes each message to the running DDM
// (starting DDM in the tray when needed) and returns DDM's answer. Also: ddmhost.exe --send "<json>" for scripts and tests.
#include "../core/ipc.hpp"
#include "../common/json.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <string>

using namespace ddm;

static bool readExact(void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n) { size_t r = fread(p, 1, n, stdin); if (r == 0) return false; p += r; n -= r; }
    return true;
}

static void writeFrame(const std::string& s) {
    uint32_t n = static_cast<uint32_t>(s.size());
    fwrite(&n, 4, 1, stdout);
    fwrite(s.data(), 1, s.size(), stdout);
    fflush(stdout);
}

static void startDdm() {
    std::wstring exe = W(joinPath(exeDir(), "DDM.exe"));
    std::wstring cmd = L"\"" + exe + L"\" /tray";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    // Break away from the browser's job so closing the browser doesn't close DDM.
    DWORD flags = CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS;
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, flags, nullptr, W(exeDir()).c_str(), &si, &pi))
        CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, W(exeDir()).c_str(), &si, &pi);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
}

static Json forward(const Json& msg) {
    Json reply;
    if (ipcSend(msg, reply, 1500)) return reply;
    startDdm();
    for (int i = 0; i < 40; i++) { Sleep(250); if (ipcSend(msg, reply, 1500)) return reply; }
    Json err = Json::object();
    err["ok"] = false;
    err["error"] = "DDM isn't running";
    return err;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring(argv[1]) == L"--send") {
        bool ok = false;
        Json m = Json::parse(U(argv[2]), &ok);
        if (!ok) { fprintf(stderr, "bad json\n"); return 2; }
        printf("%s\n", forward(m).dump().c_str());
        return 0;
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    while (true) {
        uint32_t n = 0;
        if (!readExact(&n, 4) || n == 0 || n > 48 * 1024 * 1024) return 0;   // (a page snapshot for "Save as PDF" can be large)
        std::string body(n, '\0');
        if (!readExact(&body[0], n)) return 0;
        bool ok = false;
        Json m = Json::parse(body, &ok);
        Json r;
        if (ok && m.isObject() && m.has("source")) m["source"] = "browser";   // whatever reaches DDM through the browser counts as the browser's
        if (!ok) { r = Json::object(); r["ok"] = false; r["error"] = "bad message"; }
        else r = forward(m);
        std::string out = r.dump();
        if (out.size() > 1000 * 1024) out = "{\"ok\":false,\"error\":\"answer too large\"}";
        writeFrame(out);
    }
}
