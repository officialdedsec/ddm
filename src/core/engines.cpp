#include "engines.hpp"
#include "../common/util.hpp"
#include <windows.h>

namespace ddm {
namespace engines {

std::string dir() { std::string d = joinPath(dataDir(), "engine"); makeDirs(d); return d; }

bool is64() {
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    return wow != FALSE;   // DDM is 32-bit: running under WOW64 means 64-bit Windows
}

int windowsMajor() {
    typedef LONG(WINAPI * Fn)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof v;
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (fn) fn(&v);
    return static_cast<int>(v.dwMajorVersion);
}

std::string joiner() {
    std::string own = joinPath(exeDir(), "ffmpeg.exe");
    return fileSize(own) > 100 * 1024 ? own : "";
}
std::string joinerDir() { std::string j = joiner(); return j.empty() ? "" : dirName(j); }

void removeOld() {
    // what earlier versions fetched: the video helper and its JavaScript runtime, the full FFmpeg, the spare torrent
    // program (aria2) and their leftovers. DDM does all of it itself now.
    for (auto n : {"ddm-video.exe", "deno.exe", "qjs.exe", "ffmpeg.exe", "aria2c.exe", "dht.dat", "checked.txt", "update.tmp"}) DeleteFileW(W(joinPath(dir(), n)).c_str());
    for (auto& f : listDir(dir())) {
        std::string l = lower(f);
        if (f.size() > 5 && l[0] == 'c' && endsWith(l, ".txt")) DeleteFileW(W(joinPath(dir(), f)).c_str());   // cookie files the helper used
        if (endsWith(l, ".aria2")) DeleteFileW(W(joinPath(dir(), f)).c_str());
    }
}

} // namespace engines
} // namespace ddm
