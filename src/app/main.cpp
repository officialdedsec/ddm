// DDM entry point.
#include "../ui/app.hpp"
#include <windows.h>

typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(HANDLE);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    // Sharp text at any scaling: per-monitor on Windows 10+, system DPI on Windows 7/8.
    auto ctx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")));
    if (!ctx || !ctx(reinterpret_cast<HANDLE>(-3) /* PER_MONITOR_AWARE */)) SetProcessDPIAware();
    SetDllDirectoryW(L"");   // never load DLLs from the current folder
    return ddm::App::get().run(inst, cmd ? cmd : L"");
}
