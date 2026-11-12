#include "sound.hpp"
#include "settings.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <mutex>

namespace ddm {
namespace sound {

namespace {
const int64_t kMaxBytes = 10 * 1024 * 1024;
std::mutex g_mu;
DWORD g_last[4] = {0, 0, 0, 0};

std::string dir() { return joinPath(dataDir(), "sounds"); }
std::string fileOf(Event e) { return joinPath(dir(), std::string(key(e)) + ".wav"); }
std::string pendingOf(Event e) { return joinPath(dir(), std::string(key(e)) + ".new.wav"); }

uint32_t le32(const std::string& b, size_t o) { return o + 4 <= b.size() ? static_cast<uint32_t>(static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8) | (static_cast<uint8_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(b[o + 3])) << 24)) : 0; }
uint16_t le16(const std::string& b, size_t o) { return o + 2 <= b.size() ? static_cast<uint16_t>(static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8)) : 0; }

// a well-formed RIFF/WAVE: every chunk inside the file, a sane "fmt " and a "data" chunk
bool validWav(const std::string& b, std::string& error) {
    if (b.size() < 44 || b.compare(0, 4, "RIFF") != 0 || b.compare(8, 4, "WAVE") != 0) { error = "This isn't a WAV sound file."; return false; }
    bool fmt = false, data = false;
    for (size_t p = 12; p + 8 <= b.size();) {
        std::string id = b.substr(p, 4);
        uint32_t len = le32(b, p + 4);
        if (len > b.size() - p - 8) { error = "This WAV file is damaged (a part of it runs past its end)."; return false; }
        if (id == "fmt ") {
            if (len < 16) { error = "This WAV file is damaged."; return false; }
            uint16_t format = le16(b, p + 8), channels = le16(b, p + 10), bits = le16(b, p + 22);
            uint32_t rate = le32(b, p + 12);
            if (format != 1 && format != 3 && format != 0xFFFE) { error = "This WAV file is compressed in a way Windows' sound player may not play. Use an uncompressed (PCM) WAV."; return false; }
            if (channels < 1 || channels > 8 || rate < 4000 || rate > 384000 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) { error = "This WAV file has unusual settings and can't be used."; return false; }
            fmt = true;
        } else if (id == "data") data = len > 0;
        p += 8 + len + (len & 1);
    }
    if (!fmt || !data) { error = "This WAV file has no sound in it."; return false; }
    return true;
}

bool localPath(const std::string& p) {
    // a drive on this PC (C:\…): never a network share (\\server\…), a device path (\\?\, \\.\) or a stream (file:alt)
    if (p.size() < 4 || !isalpha(static_cast<unsigned char>(p[0])) || p[1] != ':' || (p[2] != '\\' && p[2] != '/')) return false;
    if (p.find(':', 2) != std::string::npos) return false;
    std::wstring root = W(p.substr(0, 3));
    UINT t = GetDriveTypeW(root.c_str());
    return t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_RAMDISK || t == DRIVE_CDROM;
}

std::string& modeOf(Settings& s, Event e) {
    switch (e) {
        case Event::Complete: return s.soundComplete;
        case Event::Failed: return s.soundFailed;
        case Event::QueueDone: return s.soundQueue;
        default: return s.soundThreat;
    }
}

const wchar_t* windowsAlias(Event e) {
    switch (e) {
        case Event::Complete: return L"SystemAsterisk";
        case Event::Failed: return L"SystemExclamation";
        case Event::QueueDone: return L"SystemNotification";
        default: return L"SystemHand";
    }
}

void playMode(Event e, const std::string& mode, bool preferNew = false) {
    if (mode == "custom") {
        std::string f = preferNew && fileExists(pendingOf(e)) ? pendingOf(e) : fileOf(e);
        if (fileExists(f) && fileSize(f) <= kMaxBytes) { PlaySoundW(W(f).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT); return; }
    }
    if (mode == "custom" || mode == "windows") PlaySoundW(windowsAlias(e), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}
} // namespace

const char* key(Event e) {
    switch (e) {
        case Event::Complete: return "complete";
        case Event::Failed: return "failed";
        case Event::QueueDone: return "queue";
        default: return "threat";
    }
}

bool import(Event e, const std::string& wavPath, std::string& error) {
    if (!localPath(wavPath)) { error = "Choose a sound file on this PC (not on a network share)."; return false; }
    if (lower(extOf(wavPath)) != "wav") { error = "Choose a .wav sound file."; return false; }
    DWORD a = GetFileAttributesW(W(wavPath).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE))) { error = "That file can't be used."; return false; }
    int64_t size = fileSize(wavPath);
    if (size <= 0 || size > kMaxBytes) { error = "Sounds can be at most 10 MB (a few seconds of sound is plenty)."; return false; }
    std::string data;
    if (!readFile(wavPath, data) || static_cast<int64_t>(data.size()) != size) { error = "DDM couldn't read that file."; return false; }
    if (!validWav(data, error)) return false;
    makeDirs(dir());
    std::lock_guard<std::mutex> g(g_mu);
    if (!writeFileAtomic(pendingOf(e), data)) { error = "DDM couldn't save the sound."; return false; }
    return true;
}

bool hasCustom(Event e) { return fileExists(fileOf(e)) || fileExists(pendingOf(e)); }

void commit() {
    std::lock_guard<std::mutex> g(g_mu);
    for (Event e : {Event::Complete, Event::Failed, Event::QueueDone, Event::Threat})
        if (fileExists(pendingOf(e))) MoveFileExW(W(pendingOf(e)).c_str(), W(fileOf(e)).c_str(), MOVEFILE_REPLACE_EXISTING);
}

void discard() {
    std::lock_guard<std::mutex> g(g_mu);
    for (Event e : {Event::Complete, Event::Failed, Event::QueueDone, Event::Threat}) DeleteFileW(W(pendingOf(e)).c_str());
}

void play(Event e) {
    const Settings& s = settings();
    if (!s.soundsOn) return;
    Settings copy = s;
    std::string mode = modeOf(copy, e);
    if (mode == "off" || mode.empty()) return;
    {
        std::lock_guard<std::mutex> g(g_mu);   // many downloads ending together: one sound, not a cacophony
        DWORD now = GetTickCount(), &last = g_last[static_cast<int>(e)];
        if (last && now - last < 1500) return;
        last = now;
    }
    playMode(e, mode);
}

void preview(Event e, const std::string& mode) { playMode(e, mode, true); }

} // namespace sound
} // namespace ddm
