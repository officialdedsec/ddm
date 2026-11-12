// File tools: everything runs on this PC, nothing is uploaded.
//   Archives: extract (ZIP, TAR, TAR.GZ, GZ, ISO, uncompressed RAR) and create ZIP.
//   Video and audio (with FFmpeg): compress video, to MP3, smaller audio, trim, video to GIF.
//   Disk and network: find duplicate files, connection speed test.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ddm {
namespace tools {

// progress(fraction 0..1 or -1 = unknown, status line)
using Progress = std::function<void(double, const std::string&)>;

// Unpacks an archive into outDir. Returns the number of files saved; error explains a failure.
int extract(const std::string& archive, const std::string& outDir, Progress p, std::atomic<bool>* stop, std::string& error);
bool canExtract(const std::string& fileName);
// Packs files and folders into a ZIP.
bool createZip(const std::vector<std::string>& inputs, const std::string& zipPath, Progress p, std::atomic<bool>* stop, std::string& error);

// FFmpeg tools: the result is written next to the input; returns its path ("" + error on failure).
double durationOf(const std::string& media);
std::string compressVideo(const std::string& in, int quality /*0 small … 2 good*/, Progress p, std::atomic<bool>* stop, std::string& error);
std::string toMp3(const std::string& in, Progress p, std::atomic<bool>* stop, std::string& error);
std::string smallerAudio(const std::string& in, int kbps, Progress p, std::atomic<bool>* stop, std::string& error);
std::string trim(const std::string& in, double fromSec, double toSec, Progress p, std::atomic<bool>* stop, std::string& error);
std::string toGif(const std::string& in, double fromSec, double lengthSec, int width, Progress p, std::atomic<bool>* stop, std::string& error);

// Metadata remover: takes out what a file says about where, when, by whom and with what it was made — camera, GPS
// position, dates, author, software, comments, titles, tags. Pictures (JPEG, PNG, WebP) and Office documents are
// cleaned without touching the picture or the text; video and audio are copied without their tags (FFmpeg, no
// re-encoding). The file is replaced by the clean copy and the original goes to the Recycle Bin.
// Returns the cleaned file's path ("" + error on failure); summary says what was removed.
bool canStripMetadata(const std::string& fileName);
std::string stripMetadata(const std::string& in, Progress p, std::atomic<bool>* stop, std::string& error, std::string* summary = nullptr);

// Groups of identical files (same content) in a folder and its subfolders.
std::vector<std::vector<std::string>> duplicates(const std::string& folder, Progress p, std::atomic<bool>* stop);

// Tries 1…32 connections against a fast test server; returns the best number (0 = test failed) and the speeds seen.
int speedTest(Progress p, std::atomic<bool>* stop, std::vector<std::pair<int, double>>& results, std::string& error);

} // namespace tools
} // namespace ddm
