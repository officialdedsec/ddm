// X-Ray: looks inside a file before (or without) downloading it. Archives keep their table of contents in a known
// place (a ZIP at its end, an ISO near its start, a 7z behind a pointer in its first bytes), so X-Ray reads only
// those few kilobytes with HTTP range requests (the same requests the downloader uses), shows what's inside and can
// download just the files you tick.
//   Lists:   ZIP (and ZIP64, JAR, APK, DOCX, XLSX, EPUB…), ISO 9660 / Joliet / UDF disc images, RAR 4 and 5, 7z, TAR,
//            CAB, GZ, torrents; programs (EXE, DLL, MSI) show who signed them.
//   Picks out single files from: ZIP (stored, deflate), ISO / UDF, TAR, RAR (stored). Each one is checked (CRC) when
//   the archive keeps a checksum.
#pragma once
#include "http.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ddm {
namespace xray {

struct Entry {
    std::string path;          // inside the archive, "/" separators, never starting with "/"
    bool dir = false;
    int64_t size = 0;          // unpacked
    int64_t packed = -1;       // as stored (-1 unknown)
    int64_t mtime = 0;         // unix seconds, 0 unknown
    uint32_t crc = 0;
    bool hasCrc = false;
    bool encrypted = false;
    int method = 0;            // ZIP: 0 stored, 8 deflate; RAR: 0 stored
    int index = 0;             // torrent: the file's number for the torrent engine (1-based)
    int64_t offset = -1;       // where its data (or ZIP local header) starts in the archive
    std::vector<std::pair<int64_t, int64_t>> extents;   // ISO / UDF: (offset, length) pieces; offset -1 = zeros
    std::string embedded;      // UDF: tiny files kept inside their descriptor
    std::string risk;          // why it looks dangerous ("" = fine)
    std::string why;           // why it can't be picked out on its own ("" = it can)
};

struct Index {
    std::string format;        // "ZIP archive", "ISO disc image (UDF)", "RAR 5 archive", …
    std::string label;         // disc label, torrent name
    std::vector<Entry> entries;
    std::vector<std::string> notes;   // facts for the window (torrent trackers, program signer…)
    std::vector<std::string> warnings;
    int64_t length = 0;        // the archive's own size
    int64_t bytesRead = 0;     // what X-Ray actually transferred
    double seconds = 0;
    bool listing = true;       // false: a single file described (program, GZ)
    std::string error;
};

// Random access to a file on a web server (range requests, cached) or on this PC.
class Source {
public:
    virtual ~Source() = default;
    int64_t length = 0;
    std::string name;          // the file name the server suggests
    std::atomic<int64_t> fetched{0};
    std::atomic<bool>* stop = nullptr;
    std::string error;
    // exactly n bytes at off (fewer only at the end of the file); false + error on failure
    virtual bool read(int64_t off, size_t n, std::string& out) = 0;
    // warms the cache for several places at once (parallel requests); harmless to skip
    virtual void prefetch(const std::vector<std::pair<int64_t, int64_t>>& ranges) {}
    // copies [off, off+n) into an open file at fileOff, as fast as possible; progress(bytes) as it goes
    virtual bool copyTo(int64_t off, int64_t n, void* fileHandle, int64_t fileOff, std::function<void(int64_t)> progress);
    bool stopped() const { return stop && stop->load(); }
};

std::unique_ptr<Source> openHttp(const RequestInfo& r, std::string& error, std::atomic<bool>* stop = nullptr);
std::unique_ptr<Source> openFile(const std::string& path, std::string& error);

// What X-Ray can show for a name ("zip", "iso", … "" = nothing).
std::string kindOf(const std::string& fileName);
bool canLook(const std::string& fileName);

// Reads the table of contents. status() gets short progress lines ("Reading folders… 12").
bool look(Source& src, const std::string& fileName, Index& out, std::function<void(const std::string&)> status = nullptr);

// A torrent file's list (from its bytes).
bool lookTorrent(const std::string& data, Index& out);

// Saves the chosen entries under outDir (folders kept or flattened). Never writes outside outDir.
struct ExtractResult { int saved = 0; std::vector<std::string> problems; std::vector<std::string> files; };
bool extract(Source& src, const Index& idx, const std::vector<size_t>& which, const std::string& outDir, bool keepFolders,
             std::function<void(int64_t done, int64_t total, const std::string& current)> progress, ExtractResult& res);

// A safe place for an archive path under root ("" = refused: absolute, "..", device names…).
std::string safeOutPath(const std::string& root, const std::string& inner, bool keepFolders);

} // namespace xray
} // namespace ddm
