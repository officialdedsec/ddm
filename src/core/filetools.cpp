#include "filetools.hpp"
#include "crypto.hpp"
#include "engines.hpp"
#include "http.hpp"
#include "process.hpp"
#include "xray.hpp"
#include "../common/util.hpp"
#include <windows.h>
#include <curl/curl.h>
#include <miniz/miniz.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <cstring>
#include <shellapi.h>
#include <thread>

namespace ddm {
namespace tools {

namespace {
bool stopped(std::atomic<bool>* s) { return s && s->load(); }

// a free name next to the input: "clip (smaller).mp4"
std::string outName(const std::string& in, const std::string& tag, const std::string& ext) {
    return uniquePath(joinPath(dirName(in), stem(baseName(in)) + " (" + tag + ")." + ext));
}

// gzip → plain file (miniz's raw inflate after the gzip header)
bool gunzip(const std::string& in, const std::string& out, Progress p, std::atomic<bool>* stop, std::string& err) {
    HANDLE f = CreateFileW(W(in).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) { err = "Couldn't open " + baseName(in) + "."; return false; }
    HANDLE o = CreateFileW(W(out).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) { CloseHandle(f); err = "Couldn't create " + baseName(out) + "."; return false; }
    int64_t total = fileSize(in), read = 0;
    std::string in1(1 << 20, '\0'), out1(1 << 20, '\0');
    DWORD got = 0;
    ReadFile(f, &in1[0], static_cast<DWORD>(in1.size()), &got, nullptr);
    read += got;
    size_t hp = 10;
    uint8_t flags = got > 3 ? static_cast<uint8_t>(in1[3]) : 0;
    if (got < 18 || static_cast<uint8_t>(in1[0]) != 0x1F || static_cast<uint8_t>(in1[1]) != 0x8B) { CloseHandle(f); CloseHandle(o); DeleteFileW(W(out).c_str()); err = "This isn't a GZ file."; return false; }
    if (flags & 4) hp += 2 + (static_cast<uint8_t>(in1[10]) | (static_cast<uint8_t>(in1[11]) << 8));
    if (flags & 8) { while (hp < got && in1[hp]) hp++; hp++; }
    if (flags & 16) { while (hp < got && in1[hp]) hp++; hp++; }
    if (flags & 2) hp += 2;
    mz_stream z{};
    mz_inflateInit2(&z, -MZ_DEFAULT_WINDOW_BITS);
    z.next_in = reinterpret_cast<const unsigned char*>(in1.data()) + hp;
    z.avail_in = static_cast<unsigned>(got - hp);
    bool ok = true;
    int rc = MZ_OK;
    while (rc != MZ_STREAM_END) {
        if (stopped(stop)) { ok = false; err = "Stopped."; break; }
        if (z.avail_in == 0) {
            if (!ReadFile(f, &in1[0], static_cast<DWORD>(in1.size()), &got, nullptr) || !got) { ok = false; err = "The file ends too early."; break; }
            read += got;
            z.next_in = reinterpret_cast<const unsigned char*>(in1.data()); z.avail_in = got;
            if (p && total > 0) p(static_cast<double>(read) / static_cast<double>(total), "Unpacking " + baseName(in) + "…");
        }
        z.next_out = reinterpret_cast<unsigned char*>(&out1[0]); z.avail_out = static_cast<unsigned>(out1.size());
        rc = mz_inflate(&z, MZ_NO_FLUSH);
        if (rc != MZ_OK && rc != MZ_STREAM_END) { ok = false; err = "The file is damaged."; break; }
        DWORD n = static_cast<DWORD>(out1.size() - z.avail_out), w = 0;
        if (n && (!WriteFile(o, out1.data(), n, &w, nullptr) || w != n)) { ok = false; err = "Couldn't write to the disk (is it full?)."; break; }
    }
    mz_inflateEnd(&z);
    CloseHandle(f); CloseHandle(o);
    if (!ok) DeleteFileW(W(out).c_str());
    return ok;
}

// FFmpeg with a progress line (-progress pipe:1 prints out_time_ms=…)
// Everything runs on DDM's own FFmpeg, installed with DDM: copying, cutting, MP3, AAC, GIF, and H.264 through
// Windows' own encoder. Nothing is downloaded.
std::string runFfmpeg(std::vector<std::string> args, const std::string& out, double duration, const std::string& what, Progress p, std::atomic<bool>* stop, std::string& err) {
    std::string own = engines::joiner();
    if (own.empty()) { err = "A part of DDM is missing (ffmpeg.exe). Reinstall DDM."; return ""; }
    bool x264 = std::find(args.begin(), args.end(), "libx264") != args.end();
    std::vector<std::string> job;   // Windows' H.264 encoder works by bitrate (x264's -preset / -crf don't apply)
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-preset" || args[i] == "-crf") { i++; continue; }
        if (args[i] == "libx264") { job.push_back("h264_mf"); continue; }
        if (args[i] == "-ddm-bitrate") { if (i + 1 < args.size()) { job.push_back("-b:v"); job.push_back(args[++i]); } continue; }
        job.push_back(args[i]);
    }
    std::string tmp = joinPath(dirName(out), stem(baseName(out)) + ".working." + extOf(out));
    auto onLine = [&](const std::string& line) {
        if (startsWith(line, "out_time_ms=") && duration > 0 && p) {
            double secs = static_cast<double>(_atoi64(line.c_str() + 12)) / 1e6;
            p(std::min(0.99, secs / duration), what + "… " + formatDuration(static_cast<int64_t>(secs)) + " of " + formatDuration(static_cast<int64_t>(duration)));
        }
    };
    std::vector<std::string> a = {"-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1", "-y"};
    a.insert(a.end(), job.begin(), job.end());
    a.push_back(tmp);
    if (p) p(0, what + "…");
    RunResult r = runHidden(own, a, onLine, stop, 0, dirName(out));
    if (!r.stopped && x264 && (r.exitCode != 0 || fileSize(tmp) <= 0)) {
        DeleteFileW(W(tmp).c_str());
        logLine("file tools: " + ddm::trim(r.err.substr(0, 300)));
        err = "Windows on this PC has no H.264 video encoder (Windows N editions need Microsoft's Media Feature Pack).";
        return "";
    }
    if (r.stopped) { DeleteFileW(W(tmp).c_str()); err = "Stopped."; return ""; }
    if (r.exitCode != 0 || fileSize(tmp) <= 0) { DeleteFileW(W(tmp).c_str()); err = "This file couldn't be converted: " + ddm::trim(r.err.substr(0, 300)); return ""; }
    if (!MoveFileExW(W(tmp).c_str(), W(out).c_str(), MOVEFILE_REPLACE_EXISTING)) { err = "Couldn't save " + baseName(out) + "."; return ""; }
    if (p) p(1, "Done");
    return out;
}

void listFiles(const std::string& dir, std::vector<std::string>& out, int depth, std::atomic<bool>* stop) {
    if (depth > 32 || stopped(stop)) return;
    for (auto& n : listDir(dir)) {
        std::string p = joinPath(dir, n);
        DWORD a = GetFileAttributesW(W(p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (a & FILE_ATTRIBUTE_DIRECTORY) listFiles(p, out, depth + 1, stop); else out.push_back(p);
    }
}
} // namespace

bool canExtract(const std::string& fileName) {
    std::string k = xray::kindOf(fileName);
    return k == "zip" || k == "tar" || k == "gz" || k == "iso" || k == "rar";
}

int extract(const std::string& archive, const std::string& outDir, Progress p, std::atomic<bool>* stop, std::string& error) {
    std::string name = lower(baseName(archive));
    std::string src = archive, tmpTar;
    if (endsWith(name, ".gz") || endsWith(name, ".tgz")) {   // .gz: one file inside; .tar.gz / .tgz: a TAR inside
        bool tar = endsWith(name, ".tar.gz") || endsWith(name, ".tgz");
        makeDirs(outDir);
        std::string inner = joinPath(outDir, tar ? stem(baseName(archive)) + (endsWith(name, ".tgz") ? ".tar" : "") : stem(baseName(archive)));
        if (tar) inner = joinPath(dataDir(), "unpack-" + newId() + ".tar");
        if (!gunzip(archive, inner, p, stop, error)) return -1;
        if (!tar) return 1;
        src = tmpTar = inner;
    }
    std::string err;
    auto s = xray::openFile(src, err);
    if (!s) { error = err; if (!tmpTar.empty()) DeleteFileW(W(tmpTar).c_str()); return -1; }
    s->stop = stop;
    xray::Index idx;
    if (p) p(-1, "Reading the archive…");
    if (!xray::look(*s, tmpTar.empty() ? baseName(archive) : "x.tar", idx)) { error = idx.error; if (!tmpTar.empty()) DeleteFileW(W(tmpTar).c_str()); return -1; }
    std::vector<size_t> all;
    int blocked = 0;
    for (size_t i = 0; i < idx.entries.size(); i++) { all.push_back(i); if (!idx.entries[i].dir && !idx.entries[i].why.empty()) blocked++; }
    xray::ExtractResult res;
    xray::extract(*s, idx, all, outDir, true, [&](int64_t d, int64_t t, const std::string& cur) { if (p) p(t > 0 ? static_cast<double>(d) / static_cast<double>(t) : -1, "Unpacking " + baseName(replaceAll(cur, "/", "\\")) + "…"); }, res);
    s.reset();
    if (!tmpTar.empty()) DeleteFileW(W(tmpTar).c_str());
    if (blocked && res.saved == 0) { error = "This archive is packed in a way DDM can't unpack (" + idx.entries[0].why + "). Open it with an archive program."; return -1; }
    if (!res.problems.empty() && res.saved == 0) { error = res.problems.front(); return -1; }
    if (blocked) error = std::to_string(blocked) + (blocked == 1 ? " file needs" : " files need") + " an archive program (compressed or password-protected).";
    return res.saved;
}

bool createZip(const std::vector<std::string>& inputs, const std::string& zipPath, Progress p, std::atomic<bool>* stop, std::string& error) {
    struct In { std::string path, name; };
    std::vector<In> files;
    int64_t total = 0;
    for (auto& i : inputs) {
        if (dirExists(i)) {
            std::vector<std::string> v;
            listFiles(i, v, 0, stop);
            std::string base = dirName(i);
            for (auto& f : v) { files.push_back({f, replaceAll(f.substr(base.size() + 1), "\\", "/")}); total += fileSize(f); }
        } else if (fileExists(i)) { files.push_back({i, baseName(i)}); total += fileSize(i); }
    }
    if (files.empty()) { error = "Nothing to pack."; return false; }
    HANDLE out = CreateFileW(W(zipPath + ".working").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { error = "Couldn't create " + baseName(zipPath) + "."; return false; }
    mz_zip_archive z{};
    z.m_pIO_opaque = out;
    z.m_pWrite = [](void* o, mz_uint64 ofs, const void* buf, size_t n) -> size_t {
        LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(ofs);
        DWORD w = 0;
        if (!SetFilePointerEx(o, li, nullptr, FILE_BEGIN) || !WriteFile(o, buf, static_cast<DWORD>(n), &w, nullptr)) return 0;
        return w;
    };
    bool ok = mz_zip_writer_init_v2(&z, 0, total > 0xFFFFFFF0LL || files.size() > 65000 ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0) != 0;
    int64_t done = 0;
    for (auto& f : files) {
        if (!ok) break;
        if (stopped(stop)) { ok = false; error = "Stopped."; break; }
        HANDLE in = CreateFileW(W(f.path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) continue;
        struct Ctx { HANDLE h; int64_t* done; int64_t total; Progress* p; std::string name; } c{in, &done, total, &p, f.name};
        FILETIME ft{}; GetFileTime(in, nullptr, nullptr, &ft);
        ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        MZ_TIME_T mt = static_cast<MZ_TIME_T>(u.QuadPart / 10000000ULL - 11644473600ULL);
        std::string ext = lower(extOf(f.name));
        bool packed = ext == "zip" || ext == "7z" || ext == "rar" || ext == "mp4" || ext == "mkv" || ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "mp3" || ext == "gz" || ext == "webm";
        ok = mz_zip_writer_add_read_buf_callback(&z, f.name.c_str(), [](void* o, mz_uint64 ofs, void* buf, size_t n) -> size_t {
            auto* cx = static_cast<Ctx*>(o);
            LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(ofs);
            DWORD r = 0;
            if (!SetFilePointerEx(cx->h, li, nullptr, FILE_BEGIN) || !ReadFile(cx->h, buf, static_cast<DWORD>(n), &r, nullptr)) return 0;
            *cx->done += r;
            if (*cx->p && cx->total > 0) (*cx->p)(static_cast<double>(*cx->done) / static_cast<double>(cx->total), "Packing " + baseName(replaceAll(cx->name, "/", "\\")) + "…");
            return r;
        }, &c, static_cast<mz_uint64>(fileSize(f.path)), &mt, nullptr, 0, packed ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL, nullptr, 0, nullptr, 0) != 0;
        CloseHandle(in);
        if (!ok) error = "Couldn't pack " + f.name + ".";
    }
    if (ok) ok = mz_zip_writer_finalize_archive(&z) != 0;
    mz_zip_writer_end(&z);
    CloseHandle(out);
    if (!ok) { DeleteFileW(W(zipPath + ".working").c_str()); if (error.empty()) error = "Couldn't create the ZIP."; return false; }
    if (!MoveFileExW(W(zipPath + ".working").c_str(), W(zipPath).c_str(), MOVEFILE_REPLACE_EXISTING)) { error = "Couldn't save " + baseName(zipPath) + "."; return false; }
    if (p) p(1, "Done");
    return true;
}

double durationOf(const std::string& media) {
    std::string err;
    std::string prog = engines::joiner();
    if (prog.empty()) return 0;
    RunResult r = runHidden(prog, {"-hide_banner", "-i", media}, nullptr, nullptr, 30000);
    std::string all = r.out + r.err;
    size_t d = all.find("Duration: ");
    if (d == std::string::npos) return 0;
    int hh = 0, mm = 0; double ss = 0;
    if (sscanf(all.c_str() + d + 10, "%d:%d:%lf", &hh, &mm, &ss) != 3) return 0;
    return hh * 3600.0 + mm * 60.0 + ss;
}

std::string compressVideo(const std::string& in, int quality, Progress p, std::atomic<bool>* stop, std::string& error) {
    const char* crf = quality <= 0 ? "30" : quality == 1 ? "26" : "22";
    // Windows' encoder works by bitrate: bits per pixel by quality, never more than most of what the video has now
    int w = 0, h = 0, kbps = 0;
    double fps = 30;
    {
        std::string prog = engines::joiner();
        if (!prog.empty()) {
            RunResult r = runHidden(prog, {"-hide_banner", "-i", in}, nullptr, nullptr, 30000);
            std::string all = r.out + r.err;
            size_t v = all.find("Video: ");
            if (v != std::string::npos) {
                for (size_t i = v; i < all.size() && all[i] != '\n'; i++)
                    if (isdigit(static_cast<unsigned char>(all[i])) && (i == 0 || all[i - 1] == ' ') && sscanf(all.c_str() + i, "%dx%d", &w, &h) == 2 && w > 15 && h > 15) break; else { w = h = 0; }
                size_t f = all.find(" fps", v);
                if (f != std::string::npos) { size_t b = all.rfind(' ', f - 1); if (b != std::string::npos) fps = std::max(5.0, std::min(120.0, atof(all.c_str() + b + 1))); }
            }
            size_t br = all.find("bitrate: ");
            if (br != std::string::npos) kbps = atoi(all.c_str() + br + 9);
        }
    }
    double bpp = quality <= 0 ? 0.045 : quality == 1 ? 0.075 : 0.11;
    int target = w > 0 && h > 0 ? static_cast<int>(bpp * w * h * fps / 1000.0) : (quality <= 0 ? 900 : quality == 1 ? 1600 : 2800);
    if (kbps > 200) target = std::min(target, static_cast<int>(kbps * (quality <= 0 ? 0.45 : quality == 1 ? 0.6 : 0.75)));
    target = std::max(150, target);
    return runFfmpeg({"-i", in, "-map", "0:v:0", "-map", "0:a?", "-c:v", "libx264", "-preset", "veryfast", "-crf", crf, "-ddm-bitrate", std::to_string(target) + "k", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k", "-movflags", "+faststart"},
                     outName(in, "smaller", "mp4"), durationOf(in), "Compressing", p, stop, error);
}

std::string toMp3(const std::string& in, Progress p, std::atomic<bool>* stop, std::string& error) {
    return runFfmpeg({"-i", in, "-vn", "-c:a", "libmp3lame", "-q:a", "2"}, outName(in, "sound", "mp3"), durationOf(in), "Converting to MP3", p, stop, error);
}

std::string smallerAudio(const std::string& in, int kbps, Progress p, std::atomic<bool>* stop, std::string& error) {
    return runFfmpeg({"-i", in, "-vn", "-c:a", "libmp3lame", "-b:a", std::to_string(std::max(32, std::min(320, kbps))) + "k"}, outName(in, std::to_string(kbps) + " kbps", "mp3"), durationOf(in), "Making it smaller", p, stop, error);
}

std::string trim(const std::string& in, double fromSec, double toSec, Progress p, std::atomic<bool>* stop, std::string& error) {
    if (toSec <= fromSec) { error = "The end must come after the start."; return ""; }
    char a[32], b[32];
    snprintf(a, sizeof a, "%.3f", fromSec); snprintf(b, sizeof b, "%.3f", toSec);
    std::string ext = extOf(in).empty() ? "mp4" : extOf(in);
    return runFfmpeg({"-ss", a, "-to", b, "-i", in, "-c", "copy", "-avoid_negative_ts", "make_zero"}, outName(in, "part", ext), toSec - fromSec, "Cutting", p, stop, error);
}

std::string toGif(const std::string& in, double fromSec, double lengthSec, int width, Progress p, std::atomic<bool>* stop, std::string& error) {
    char a[32], l[32];
    snprintf(a, sizeof a, "%.3f", fromSec); snprintf(l, sizeof l, "%.3f", std::max(0.5, std::min(60.0, lengthSec)));
    std::string vf = "fps=12,scale=" + std::to_string(std::max(120, std::min(1280, width))) + ":-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer";
    return runFfmpeg({"-ss", a, "-t", l, "-i", in, "-vf", vf, "-loop", "0"}, outName(in, "GIF", "gif"), lengthSec, "Making the GIF", p, stop, error);
}

// ----------------------------------------------------------------- metadata remover
namespace {
const std::set<std::string>& metaVideo() { static const std::set<std::string> s = {"mp4", "m4v", "mov", "mkv", "webm", "avi", "wmv", "flv", "3gp", "ts", "mts", "m2ts"}; return s; }
const std::set<std::string>& metaAudio() { static const std::set<std::string> s = {"mp3", "m4a", "aac", "flac", "ogg", "opus", "wav", "wma", "aiff", "aif"}; return s; }
const std::set<std::string>& metaOffice() { static const std::set<std::string> s = {"docx", "xlsx", "pptx", "docm", "xlsm", "pptm", "odt", "ods", "odp"}; return s; }

uint32_t be32(const std::string& b, size_t o) { return (static_cast<uint32_t>(static_cast<uint8_t>(b[o])) << 24) | (static_cast<uint8_t>(b[o + 1]) << 16) | (static_cast<uint8_t>(b[o + 2]) << 8) | static_cast<uint8_t>(b[o + 3]); }
uint32_t le32x(const std::string& b, size_t o) { return static_cast<uint8_t>(b[o]) | (static_cast<uint8_t>(b[o + 1]) << 8) | (static_cast<uint8_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(b[o + 3])) << 24); }

// JPEG: drops EXIF / XMP / IPTC / maker notes / comments (APP1…APP15, COM); keeps the colour profile and Adobe's colour marker
bool cleanJpeg(const std::string& b, std::string& out, int& removed) {
    if (b.size() < 4 || static_cast<uint8_t>(b[0]) != 0xFF || static_cast<uint8_t>(b[1]) != 0xD8) return false;
    out.assign(b, 0, 2);
    size_t p = 2;
    while (p + 2 <= b.size()) {
        if (static_cast<uint8_t>(b[p]) != 0xFF) return false;
        uint8_t m = static_cast<uint8_t>(b[p + 1]);
        if (m == 0xFF) { p++; continue; }
        if (m == 0xDA || m == 0xD9) { out.append(b, p, std::string::npos); return true; }   // the picture itself, untouched
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { out.append(b, p, 2); p += 2; continue; }
        if (p + 4 > b.size()) return false;
        size_t len = (static_cast<uint8_t>(b[p + 2]) << 8) | static_cast<uint8_t>(b[p + 3]);
        if (len < 2 || p + 2 + len > b.size()) return false;
        bool icc = m == 0xE2 && len >= 14 && b.compare(p + 4, 12, std::string("ICC_PROFILE\0", 12)) == 0;
        bool jfxx = m == 0xE0 && len >= 7 && b.compare(p + 4, 5, std::string("JFXX\0", 5)) == 0;   // a thumbnail
        bool drop = ((m >= 0xE1 && m <= 0xEF) && !icc && m != 0xEE) || m == 0xFE || jfxx;
        if (drop) removed++; else out.append(b, p, 2 + len);
        p += 2 + len;
    }
    return false;
}

// PNG: drops text, EXIF and time chunks
bool cleanPng(const std::string& b, std::string& out, int& removed) {
    if (b.size() < 8 || b.compare(0, 8, "\x89PNG\r\n\x1A\n") != 0) return false;
    out.assign(b, 0, 8);
    size_t p = 8;
    while (p + 12 <= b.size()) {
        uint32_t len = be32(b, p);
        if (len > b.size() - p - 12) return false;
        std::string type = b.substr(p + 4, 4);
        bool drop = type == "tEXt" || type == "zTXt" || type == "iTXt" || type == "eXIf" || type == "tIME";
        if (drop) removed++; else out.append(b, p, 12 + len);
        p += 12 + len;
        if (type == "IEND") return true;
    }
    return false;
}

// WebP: drops the EXIF and XMP chunks and their flags
bool cleanWebp(const std::string& b, std::string& out, int& removed) {
    if (b.size() < 12 || b.compare(0, 4, "RIFF") != 0 || b.compare(8, 4, "WEBP") != 0) return false;
    out.assign(b, 0, 12);
    size_t p = 12, vp8x = std::string::npos;
    while (p + 8 <= b.size()) {
        std::string id = b.substr(p, 4);
        uint32_t len = le32x(b, p + 4);
        size_t whole = 8 + static_cast<size_t>(len) + (len & 1);
        if (len > b.size() - p - 8) return false;
        if (whole > b.size() - p) whole = b.size() - p;
        if (id == "EXIF" || id == "XMP ") { removed++; p += whole; continue; }
        if (id == "VP8X") vp8x = out.size();
        out.append(b, p, whole);
        p += whole;
    }
    if (vp8x != std::string::npos && vp8x + 8 < out.size()) out[vp8x + 8] = static_cast<char>(static_cast<uint8_t>(out[vp8x + 8]) & ~0x0C);
    uint32_t riff = static_cast<uint32_t>(out.size() - 8);
    for (int i = 0; i < 4; i++) out[4 + i] = static_cast<char>((riff >> (8 * i)) & 0xFF);
    return true;
}

// Office: the document's properties (author, company, last saved by, dates, title, tags, custom fields) are emptied
bool cleanOffice(const std::string& b, std::string& out, int& removed, std::string& err) {
    mz_zip_archive r{}, w{};
    if (!mz_zip_reader_init_mem(&r, b.data(), b.size(), 0)) { err = "This document is damaged (it isn't a valid Office file)."; return false; }
    if (!mz_zip_writer_init_heap(&w, 0, b.size() + 4096)) { mz_zip_reader_end(&r); err = "Out of memory."; return false; }
    static const char* core = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"></cp:coreProperties>";
    static const char* app = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"></Properties>";
    static const char* custom = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/custom-properties\" xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"></Properties>";
    static const char* odf = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" office:version=\"1.2\"><office:meta/></office:document-meta>";
    bool ok = true;
    mz_uint n = mz_zip_reader_get_num_files(&r);
    for (mz_uint i = 0; i < n && ok; i++) {
        char name[1024] = {0};
        mz_zip_reader_get_filename(&r, i, name, sizeof name);
        std::string nm = lower(name);
        const char* blank = nm == "docprops/core.xml" ? core : nm == "docprops/app.xml" ? app : nm == "docprops/custom.xml" ? custom : nm == "meta.xml" ? odf : nullptr;
        if (blank) { ok = mz_zip_writer_add_mem(&w, name, blank, strlen(blank), MZ_DEFAULT_LEVEL) != 0; removed++; }
        else ok = mz_zip_writer_add_from_zip_reader(&w, &r, i) != 0;
    }
    void* buf = nullptr; size_t size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&w, &buf, &size) != 0;
    if (ok) out.assign(static_cast<const char*>(buf), size);
    mz_zip_writer_end(&w);
    mz_zip_reader_end(&r);
    if (!ok) err = "DDM couldn't rewrite this document.";
    return ok;
}

// the clean copy takes the original's place; the original goes to the Recycle Bin
bool swapIn(const std::string& in, const std::string& clean, std::string& err) {
    std::wstring from = W(in);
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted || fileExists(in)) {
        err = "The clean copy is saved as " + baseName(clean) + " (the original is in use, so DDM left it).";
        return false;
    }
    if (!MoveFileExW(W(clean).c_str(), W(in).c_str(), MOVEFILE_REPLACE_EXISTING)) { err = "The clean copy is saved as " + baseName(clean) + "."; return false; }
    return true;
}
} // namespace

bool canStripMetadata(const std::string& fileName) {
    std::string e = lower(extOf(fileName));
    return e == "jpg" || e == "jpeg" || e == "jfif" || e == "png" || e == "webp" || metaVideo().count(e) || metaAudio().count(e) || metaOffice().count(e);
}

std::string stripMetadata(const std::string& in, Progress p, std::atomic<bool>* stop, std::string& error, std::string* summary) {
    std::string e = lower(extOf(in));
    if (e == "pdf") { error = "PDF files aren't supported: their properties can't be removed without leaving a copy inside the file."; return ""; }
    if (!canStripMetadata(in)) { error = "DDM can remove metadata from pictures (JPEG, PNG, WebP), video, audio and Office documents."; return ""; }
    if (!fileExists(in)) { error = "The file isn't there any more."; return ""; }
    std::string clean = joinPath(dirName(in), stem(baseName(in)) + " (no metadata)." + extOf(in));
    clean = uniquePath(clean);
    int removed = 0;
    std::string what;
    if (metaVideo().count(e) || metaAudio().count(e)) {
        bool audio = metaAudio().count(e) > 0;
        std::vector<std::string> a = {"-i", in};
        if (audio) a.insert(a.end(), {"-map", "0:a"});
        else a.insert(a.end(), {"-map", "0:v?", "-map", "0:a?", "-map", "0:s?"});
        a.insert(a.end(), {"-c", "copy", "-map_metadata", "-1", "-map_chapters", "-1", "-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact"});
        if (e == "mp3") a.insert(a.end(), {"-id3v2_version", "0", "-write_id3v1", "0"});
        if (e == "mp4" || e == "m4v" || e == "mov" || e == "m4a") a.insert(a.end(), {"-movflags", "+faststart"});
        if (runFfmpeg(a, clean, durationOf(in), "Removing the metadata", p, stop, error).empty()) return "";
        what = audio ? "Tags (title, artist, album, comments, cover picture), chapters and the encoder's name were removed."
                     : "Tags (title, comments, creation date, location, device), chapters and the encoder's name were removed.";
    } else {
        if (p) p(-1, "Removing the metadata…");
        if (fileSize(in) > 512LL * 1024 * 1024) { error = "The file is too big for this tool (at most 512 MB)."; return ""; }
        std::string b, out;
        if (!readFile(in, b)) { error = "DDM couldn't read the file."; return ""; }
        bool ok = false;
        if (e == "png") ok = cleanPng(b, out, removed);
        else if (e == "webp") ok = cleanWebp(b, out, removed);
        else if (metaOffice().count(e)) ok = cleanOffice(b, out, removed, error);
        else ok = cleanJpeg(b, out, removed);
        if (!ok) { if (error.empty()) error = "This file is damaged or isn't really a ." + e + " file."; return ""; }
        if (removed == 0) { if (summary) *summary = "There was no metadata in " + baseName(in) + "."; if (p) p(1, "Done"); return in; }
        if (!writeFileAtomic(clean, out)) { error = "DDM couldn't save the clean file."; return ""; }
        what = metaOffice().count(e) ? "The document's properties (author, company, last saved by, dates, title, tags, custom fields) were removed."
                                     : "Camera, GPS position, dates, software, comments and other hidden details were removed (" + std::to_string(removed) + (removed == 1 ? " part)." : " parts).");
    }
    std::string err2;
    std::string result = swapIn(in, clean, err2) ? in : clean;
    if (summary) *summary = what + (err2.empty() ? " The original is in the Recycle Bin." : " " + err2);
    if (p) p(1, "Done");
    return result;
}

std::vector<std::vector<std::string>> duplicates(const std::string& folder, Progress p, std::atomic<bool>* stop) {
    std::vector<std::string> files;
    if (p) p(-1, "Listing the files…");
    listFiles(folder, files, 0, stop);
    std::map<int64_t, std::vector<std::string>> bySize;
    for (auto& f : files) { int64_t s = fileSize(f); if (s > 0) bySize[s].push_back(f); }
    std::vector<std::string> candidates;
    for (auto& kv : bySize) if (kv.second.size() > 1) for (auto& f : kv.second) candidates.push_back(f);
    std::map<std::string, std::vector<std::string>> byHash;
    std::atomic_bool st{false};
    for (size_t i = 0; i < candidates.size(); i++) {
        if (stopped(stop)) break;
        if (p) p(static_cast<double>(i) / static_cast<double>(candidates.size()), "Comparing " + baseName(candidates[i]) + "…");
        std::string h = crypto::fileChecksum(candidates[i], "sha256", &st);
        if (!h.empty()) byHash[std::to_string(fileSize(candidates[i])) + ":" + h].push_back(candidates[i]);
    }
    std::vector<std::vector<std::string>> out;
    for (auto& kv : byHash) if (kv.second.size() > 1) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const std::vector<std::string>& a, const std::vector<std::string>& b) { return fileSize(a[0]) * static_cast<int64_t>(a.size() - 1) > fileSize(b[0]) * static_cast<int64_t>(b.size() - 1); });
    if (p) p(1, "Done");
    return out;
}

int speedTest(Progress p, std::atomic<bool>* stop, std::vector<std::pair<int, double>>& results, std::string& error) {
    // Cloudflare's speed test server: sends as many bytes as asked, from the nearest data centre
    const std::string url = "https://speed.cloudflare.com/__down?bytes=200000000";
    int best = 0;
    double bestSpeed = 0;
    for (int n : {1, 4, 8, 16, 32}) {
        if (stopped(stop)) break;
        if (p) p(static_cast<double>(results.size()) / 5.0, "Testing with " + std::to_string(n) + (n == 1 ? " connection…" : " connections…"));
        std::atomic<int64_t> bytes{0};
        std::atomic<bool> timeUp{false};
        std::vector<std::thread> th;
        for (int i = 0; i < n; i++) th.emplace_back([&] {
            CURL* c = curl_easy_init();
            if (!c) return;
            RequestInfo r; r.url = url;
            http::setup(c, r);
            struct W { std::atomic<int64_t>* b; std::atomic<bool>* up; std::atomic<bool>* stop; } w{&bytes, &timeUp, stop};
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t m, void* u) -> size_t { auto* x = static_cast<W*>(u); if (x->up->load() || (x->stop && x->stop->load())) return 0; *x->b += static_cast<int64_t>(s * m); return s * m; });
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &w);
            curl_easy_setopt(c, CURLOPT_TIMEOUT, 12L);
            curl_easy_perform(c);
            curl_easy_cleanup(c);
        });
        Sleep(1500);                       // connections open and speed up
        int64_t start = bytes.load();
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 40 && !stopped(stop); k++) Sleep(100);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double speed = static_cast<double>(bytes.load() - start) / std::max(0.1, secs);
        timeUp = true;
        for (auto& t : th) t.join();
        results.push_back({n, speed});
        if (speed > bestSpeed * 1.08) { bestSpeed = speed; best = n; }   // more connections only when clearly faster
    }
    if (bestSpeed <= 0) { error = "The test server couldn't be reached. Check your internet connection."; return 0; }
    if (p) p(1, "Done");
    return best;
}

} // namespace tools
} // namespace ddm
